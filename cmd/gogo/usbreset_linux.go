package main

// The USB reset on Linux: the USBDEVFS_RESET ioctl on the device's usbfs
// node, /dev/bus/usb/BBB/DDD, found from the tty through sysfs
// (sysfsUSBDevice). The node is usually writable by root only; a udev rule
// for the Espressif vendor id (docs/known-issues.md) lets the user reset it.
// cdc_acm keeps its tty across the reset.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

// usbResetRemovesPort: cdc_acm keeps the tty through a reset.
const usbResetRemovesPort = false

const (
	usbdevfsReset = 0x5514 // _IO('U', 20)
)

// udevHint is printed when the usbfs node is not writable.
const udevHint = `allow it with a udev rule: echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0660", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/99-gogo-esp32.rules && sudo udevadm control --reload && sudo udevadm trigger; or ` + replugHint

// linuxUSBDevice finds the USB device behind port and its usbfs node.
func linuxUSBDevice(port string) (usbDevice, string, error) {
	real, err := filepath.EvalSymlinks(port)
	if err != nil {
		return usbDevice{}, "", err
	}
	d, err := sysfsUSBDevice("/sys", filepath.Base(real))
	if err != nil {
		return usbDevice{}, "", err
	}
	if err := checkEspressif(port, d); err != nil {
		return usbDevice{}, "", err
	}
	return d, fmt.Sprintf("/dev/bus/usb/%03d/%03d", d.bus, d.addr), nil
}

func openUSBNode(node string, d usbDevice) (int, error) {
	fd, err := syscall.Open(node, syscall.O_RDWR|syscall.O_CLOEXEC, 0)
	if errors.Is(err, syscall.EACCES) || errors.Is(err, syscall.EPERM) {
		return -1, fmt.Errorf("no permission to reset %s (%s): %s", d, node, udevHint)
	}
	if err != nil {
		return -1, fmt.Errorf("%s: %v", node, err)
	}
	return fd, nil
}

// usbResetPort bus-resets the Espressif USB device behind port.
func usbResetPort(port string) (string, error) {
	d, node, err := linuxUSBDevice(port)
	if err != nil {
		return "", err
	}
	fd, err := openUSBNode(node, d)
	if err != nil {
		return "", err
	}
	defer syscall.Close(fd)
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), usbdevfsReset, 0); e != 0 {
		return "", fmt.Errorf("resetting %s failed (%v): %s", d, e, replugHint)
	}
	return d.String(), nil
}

// usbdevfsCtrlTransfer is struct usbdevfs_ctrltransfer.
type usbdevfsCtrlTransfer struct {
	bRequestType, bRequest  uint8
	wValue, wIndex, wLength uint16
	timeout                 uint32
	data                    unsafe.Pointer
}

// usbProbe asks the device behind port for its string descriptor 0:
// silent when the request times out (ETIMEDOUT, or EPROTO for no
// handshake) or returns no descriptor; another failure is an error.
func usbProbe(port string) (bool, error) {
	d, node, err := linuxUSBDevice(port)
	if err != nil {
		return false, err
	}
	fd, err := openUSBNode(node, d)
	if err != nil {
		return false, err
	}
	defer syscall.Close(fd)
	buf := make([]byte, 255)
	ct := usbdevfsCtrlTransfer{bRequestType: 0x80, bRequest: 6, wValue: 0x0300, wLength: uint16(len(buf)), timeout: 1000, data: unsafe.Pointer(&buf[0])}
	// _IOWR('U', 0, struct usbdevfs_ctrltransfer)
	req := uintptr(3<<30 | unsafe.Sizeof(ct)<<16 | 'U'<<8)
	n, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(unsafe.Pointer(&ct)))
	switch {
	case e == 0:
		// An empty or malformed answer is the descriptor unreadable.
		return n < 4 || buf[1] != 3, nil
	case e == syscall.ETIMEDOUT || e == syscall.EPROTO:
		return true, nil
	}
	return false, fmt.Errorf("the string descriptor request to %s failed (%v)", d, e)
}

// portHolders lists the other processes with port open, from /proc (the
// processes of other users cannot be looked into).
func portHolders(port string) ([]int, error) {
	real, err := filepath.EvalSymlinks(port)
	if err != nil {
		return nil, err
	}
	return procHolders("/proc", real, os.Getpid()), nil
}
