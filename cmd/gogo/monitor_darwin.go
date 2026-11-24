package main

import (
	"syscall"
	"unsafe"
)

const crtscts = 0x30000 // CCTS_OFLOW | CRTS_IFLOW, not exported by package syscall

// setRaw: cfmakeraw + 8N1, CLOCAL, CREAD, no HUPCL, 115200 baud.
func setRaw(fd int) error {
	var t syscall.Termios
	if err := ioctl(fd, syscall.TIOCGETA, unsafe.Pointer(&t)); err != nil {
		return err
	}
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP | syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON | syscall.IXOFF
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.HUPCL | crtscts
	t.Cflag |= syscall.CS8 | syscall.CLOCAL | syscall.CREAD
	t.Cc[syscall.VMIN] = 1
	t.Cc[syscall.VTIME] = 0
	t.Ispeed, t.Ospeed = syscall.B115200, syscall.B115200
	return ioctl(fd, syscall.TIOCSETA, unsafe.Pointer(&t))
}
