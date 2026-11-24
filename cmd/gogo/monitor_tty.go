//go:build darwin || linux

package main

import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

// openSerial opens a serial port in raw mode (no echo, no line editing).
func openSerial(port string) (*os.File, error) {
	flag := "-F"
	if runtime.GOOS == "darwin" {
		flag = "-f"
	}
	if out, err := exec.Command("stty", flag, port, "raw", "-echo", "115200").CombinedOutput(); err != nil {
		return nil, fmt.Errorf("gogo: stty %s: %v %s", port, err, out)
	}
	return os.OpenFile(port, os.O_RDWR|syscall.O_NOCTTY, 0)
}

func modemBits(f *os.File, req uintptr, bits int32) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), req, uintptr(unsafe.Pointer(&bits))); e != 0 {
		return fmt.Errorf("gogo: %s: modem control: %v", f.Name(), e)
	}
	return nil
}

// resetChip pulses RTS with DTR released: on the ESP32-S3 USB-Serial/JTAG
// port this resets the chip into the normal boot (esptool's hard reset).
func resetChip(f *os.File) error {
	if err := modemBits(f, syscall.TIOCMBIC, syscall.TIOCM_DTR); err != nil {
		return err
	}
	if err := modemBits(f, syscall.TIOCMBIS, syscall.TIOCM_RTS); err != nil {
		return err
	}
	time.Sleep(200 * time.Millisecond)
	return modemBits(f, syscall.TIOCMBIC, syscall.TIOCM_RTS)
}
