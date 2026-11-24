package main

import (
	"syscall"
	"unsafe"
)

const (
	cbaud   = 0x100f     // CBAUD, not exported by package syscall
	crtscts = 0x80000000 // CRTSCTS, likewise
)

// setRaw: cfmakeraw + 8N1, CLOCAL, CREAD, no HUPCL, 115200 baud.
func setRaw(fd int) error {
	var t syscall.Termios
	if err := ioctl(fd, syscall.TCGETS, unsafe.Pointer(&t)); err != nil {
		return err
	}
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP | syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON | syscall.IXOFF
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.HUPCL | crtscts | cbaud
	t.Cflag |= syscall.CS8 | syscall.CLOCAL | syscall.CREAD | syscall.B115200
	t.Cc[syscall.VMIN] = 1
	t.Cc[syscall.VTIME] = 0
	t.Ispeed, t.Ospeed = syscall.B115200, syscall.B115200
	return ioctl(fd, syscall.TCSETS, unsafe.Pointer(&t))
}
