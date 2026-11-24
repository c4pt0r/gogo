//go:build darwin || linux

package main

import (
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

// openSerial opens a serial port in raw mode (no echo, no line editing)
// without disturbing the modem lines.
//
// On the ESP32-S3 USB-Serial/JTAG port, DTR and RTS are the chip's reset and
// boot strap (DTR=0 RTS=1 resets; DTR=1 at reset release selects download
// mode). So the port is set up on the already-open descriptor rather than
// with stty, which opens and closes the device itself (with HUPCL that
// drops the lines on close): CLOCAL so the open never waits for carrier,
// HUPCL off so our own close leaves the lines alone, and both lines set to
// the idle state (DTR=0, RTS=0) in a single ioctl, so no intermediate
// reset/strap combination is ever presented.
func openSerial(port string) (*os.File, error) {
	fd, err := syscall.Open(port, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, fmt.Errorf("gogo: %s: %v", port, err)
	}
	if err := setRaw(fd); err != nil {
		syscall.Close(fd)
		return nil, fmt.Errorf("gogo: %s: termios: %v", port, err)
	}
	if err := syscall.SetNonblock(fd, false); err != nil {
		syscall.Close(fd)
		return nil, fmt.Errorf("gogo: %s: %v", port, err)
	}
	f := os.NewFile(uintptr(fd), port)
	if err := setModem(f, 0); err != nil {
		f.Close()
		return nil, err
	}
	return f, nil
}

func ioctl(fd int, req uintptr, arg unsafe.Pointer) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(arg)); e != 0 {
		return e
	}
	return nil
}

// setModem sets DTR and RTS together (bits: syscall.TIOCM_DTR|TIOCM_RTS).
func setModem(f *os.File, bits int32) error {
	if err := ioctl(int(f.Fd()), syscall.TIOCMSET, unsafe.Pointer(&bits)); err != nil {
		return fmt.Errorf("gogo: %s: modem control: %v", f.Name(), err)
	}
	return nil
}

// resetChip asserts RTS with DTR released, then releases both: on the
// ESP32-S3 USB-Serial/JTAG port this resets the chip into the normal boot
// (esptool's hard reset). Each step is one TIOCMSET, so DTR is never seen
// high while the reset is released.
func resetChip(f *os.File) error {
	if err := setModem(f, syscall.TIOCM_RTS); err != nil {
		return err
	}
	time.Sleep(200 * time.Millisecond)
	return setModem(f, 0)
}
