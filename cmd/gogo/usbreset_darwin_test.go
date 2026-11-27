package main

import (
	"os"
	"strings"
	"testing"
	"unsafe"
)

// The macOS FFI, read-only: loading IOKit through the trampolines and
// looking ports up in the I/O Registry. Nothing is opened or reset.
func TestDarwinRegistry(t *testing.T) {
	if err := loadIOKit(); err != nil {
		t.Fatalf("loadIOKit: %v", err)
	}
	if _, _, err := findUSBDevice("/dev/cu.gogo-no-such-port"); err == nil || !strings.Contains(err.Error(), "no serial port") {
		t.Errorf("missing port: %v", err)
	}
	// A serial port that is not on USB (Bluetooth, the debug console),
	// where the machine has one: refused, never reset.
	for _, p := range []string{"/dev/cu.Bluetooth-Incoming-Port", "/dev/cu.debug-console"} {
		if _, err := os.Stat(p); err != nil {
			continue
		}
		if _, err := usbResetPort(p); err == nil || !strings.Contains(err.Error(), "not on a USB device") {
			t.Errorf("%s: %v", p, err)
		}
	}
	var r devRequestTO
	if unsafe.Offsetof(r.pData) != 8 || unsafe.Offsetof(r.wLenDone) != 16 || unsafe.Offsetof(r.completionTimeout) != 24 || unsafe.Offsetof(r.data) != 32 {
		t.Error("devRequestTO's layout differs from IOUSBDevRequestTO")
	}
}
