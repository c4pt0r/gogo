package main

// The USB reset on macOS, through IOKit, without cgo: dlopen and dlsym
// come from libSystem (which every Go program on macOS links) through the
// two trampolines of usbreset_darwin.s, as golang.org/x/sys/unix calls
// libc; IOKit and CoreFoundation are loaded with them when a reset is
// first needed, and every C function is called through syscall.syscall6
// (C calling convention, on the system stack; no call here takes more
// than five arguments).
//
// The serial port's IOSerialBSDClient (its IOCalloutDevice or
// IODialinDevice is the port) is walked up the IOService plane to its
// IOUSBHostDevice; through that device's IOUSBDeviceInterface187 (the
// IOKit plug-in, COM-style vtables) the device is opened and
// re-enumerated (USBDeviceReEnumerate, what libusb_reset_device does on
// macOS): the serial port goes away and comes back within a second, and
// the chip does not restart.

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"unsafe"
)

//go:cgo_import_dynamic libc_dlopen dlopen "/usr/lib/libSystem.B.dylib"
//go:cgo_import_dynamic libc_dlsym dlsym "/usr/lib/libSystem.B.dylib"

// Set by usbreset_darwin.s to the trampolines' addresses.
var libc_dlopen_trampoline_addr, libc_dlsym_trampoline_addr uintptr

// syscall.syscall6 is the one of its family that the Go team keeps
// open to linkname (go.dev/issue/67401), as golang.org/x/sys uses it.
//
//go:linkname syscall_syscall6 syscall.syscall6
func syscall_syscall6(fn, a1, a2, a3, a4, a5, a6 uintptr) (r1, r2 uintptr, err syscall.Errno)

// usbResetRemovesPort: re-enumerating the device removes its serial port
// for a moment.
const usbResetRemovesPort = true

// ccall calls the C function at fn with up to 6 integer or pointer
// arguments. uintptrescapes keeps a Go pointer converted to uintptr in the
// call's arguments on the heap and alive for the call.
//
//go:uintptrescapes
func ccall(fn uintptr, args ...uintptr) uintptr {
	var a [6]uintptr
	if len(args) > len(a) {
		panic("ccall: too many arguments")
	}
	copy(a[:], args)
	r, _, _ := syscall_syscall6(fn, a[0], a[1], a[2], a[3], a[4], a[5])
	return r
}

// cstr is s as a NUL-terminated C string.
func cstr(s string) []byte { return append([]byte(s), 0) }

const (
	rtldNow              = 0x2
	cfStringEncodingUTF8 = 0x08000100
	cfNumberSInt32Type   = 3
	kIOReturnSuccess     = 0
	// What a device that does not answer a control request gives.
	kIOReturnTimeout         = 0xe00002d6
	kIOReturnNotResponding   = 0xe00002ed
	kIOUSBTransactionTimeout = 0xe0004051
)

// iokit holds the C functions used, resolved once.
var iokit struct {
	once sync.Once
	err  error

	memcpy uintptr

	IOServiceMatching, IOServiceGetMatchingServices, IOIteratorNext, IOObjectRelease,
	IORegistryEntryCreateCFProperty, IORegistryEntryGetParentEntry, IOObjectConformsTo,
	IOCreatePlugInInterfaceForService, IODestroyPlugInInterface uintptr

	CFStringCreateWithCString, CFStringGetCString, CFNumberGetValue, CFRelease,
	CFGetTypeID, CFStringGetTypeID, CFNumberGetTypeID, CFUUIDCreateFromString uintptr
}

func loadIOKit() error {
	iokit.once.Do(func() {
		libs := map[string]uintptr{}
		for _, lib := range []string{
			"/usr/lib/libSystem.B.dylib",
			"/System/Library/Frameworks/IOKit.framework/IOKit",
			"/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
		} {
			name := cstr(lib)
			h := ccall(libc_dlopen_trampoline_addr, uintptr(unsafe.Pointer(&name[0])), rtldNow)
			if h == 0 {
				iokit.err = fmt.Errorf("cannot load %s", lib)
				return
			}
			libs[filepath.Base(lib)] = h
		}
		k := &iokit
		for _, f := range []struct {
			lib, name string
			p         *uintptr
		}{
			{"libSystem.B.dylib", "memcpy", &k.memcpy},
			{"IOKit", "IOServiceMatching", &k.IOServiceMatching},
			{"IOKit", "IOServiceGetMatchingServices", &k.IOServiceGetMatchingServices},
			{"IOKit", "IOIteratorNext", &k.IOIteratorNext},
			{"IOKit", "IOObjectRelease", &k.IOObjectRelease},
			{"IOKit", "IORegistryEntryCreateCFProperty", &k.IORegistryEntryCreateCFProperty},
			{"IOKit", "IORegistryEntryGetParentEntry", &k.IORegistryEntryGetParentEntry},
			{"IOKit", "IOObjectConformsTo", &k.IOObjectConformsTo},
			{"IOKit", "IOCreatePlugInInterfaceForService", &k.IOCreatePlugInInterfaceForService},
			{"IOKit", "IODestroyPlugInInterface", &k.IODestroyPlugInInterface},
			{"CoreFoundation", "CFStringCreateWithCString", &k.CFStringCreateWithCString},
			{"CoreFoundation", "CFStringGetCString", &k.CFStringGetCString},
			{"CoreFoundation", "CFNumberGetValue", &k.CFNumberGetValue},
			{"CoreFoundation", "CFRelease", &k.CFRelease},
			{"CoreFoundation", "CFGetTypeID", &k.CFGetTypeID},
			{"CoreFoundation", "CFStringGetTypeID", &k.CFStringGetTypeID},
			{"CoreFoundation", "CFNumberGetTypeID", &k.CFNumberGetTypeID},
			{"CoreFoundation", "CFUUIDCreateFromString", &k.CFUUIDCreateFromString},
		} {
			name := cstr(f.name)
			*f.p = ccall(libc_dlsym_trampoline_addr, libs[f.lib], uintptr(unsafe.Pointer(&name[0])))
			if *f.p == 0 {
				iokit.err = fmt.Errorf("%s has no %s", f.lib, f.name)
				return
			}
		}
	})
	return iokit.err
}

// peek reads the pointer-sized word at the C address addr.
func peek(addr uintptr) uintptr {
	var v uintptr
	ccall(iokit.memcpy, uintptr(unsafe.Pointer(&v)), addr, unsafe.Sizeof(v))
	return v
}

// cfString is a new CFString of s; release it with CFRelease.
func cfString(s string) uintptr {
	b := cstr(s)
	return ccall(iokit.CFStringCreateWithCString, 0, uintptr(unsafe.Pointer(&b[0])), cfStringEncodingUTF8)
}

// regString reads the string property key of a registry entry; "" if it
// has none.
func regString(entry uintptr, key string) string {
	k := cfString(key)
	defer ccall(iokit.CFRelease, k)
	v := ccall(iokit.IORegistryEntryCreateCFProperty, entry, k, 0, 0)
	if v == 0 {
		return ""
	}
	defer ccall(iokit.CFRelease, v)
	if ccall(iokit.CFGetTypeID, v) != ccall(iokit.CFStringGetTypeID) {
		return ""
	}
	buf := make([]byte, 1024)
	if ccall(iokit.CFStringGetCString, v, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), cfStringEncodingUTF8)&0xff == 0 {
		return ""
	}
	for i, c := range buf {
		if c == 0 {
			return string(buf[:i])
		}
	}
	return ""
}

// regInt reads the 32-bit number property key of a registry entry.
func regInt(entry uintptr, key string) (uint32, bool) {
	k := cfString(key)
	defer ccall(iokit.CFRelease, k)
	v := ccall(iokit.IORegistryEntryCreateCFProperty, entry, k, 0, 0)
	if v == 0 {
		return 0, false
	}
	defer ccall(iokit.CFRelease, v)
	if ccall(iokit.CFGetTypeID, v) != ccall(iokit.CFNumberGetTypeID) {
		return 0, false
	}
	var n uint32
	if ccall(iokit.CFNumberGetValue, v, cfNumberSInt32Type, uintptr(unsafe.Pointer(&n)))&0xff == 0 {
		return 0, false
	}
	return n, true
}

// findUSBDevice returns the IOUSBHostDevice behind port (release it with
// IOObjectRelease) and what it is.
func findUSBDevice(port string) (uintptr, usbDevice, error) {
	if err := loadIOKit(); err != nil {
		return 0, usbDevice{}, err
	}
	want := map[string]bool{port: true}
	if real, err := filepath.EvalSymlinks(port); err == nil {
		want[real] = true
	}
	cls := cstr("IOSerialBSDClient")
	match := ccall(iokit.IOServiceMatching, uintptr(unsafe.Pointer(&cls[0])))
	if match == 0 {
		return 0, usbDevice{}, fmt.Errorf("IOServiceMatching failed")
	}
	var iter uint32
	if kr := uint32(ccall(iokit.IOServiceGetMatchingServices, 0, match, uintptr(unsafe.Pointer(&iter)))); kr != kIOReturnSuccess {
		return 0, usbDevice{}, fmt.Errorf("IOServiceGetMatchingServices: %#x", kr)
	}
	defer ccall(iokit.IOObjectRelease, uintptr(iter))
	var client uintptr
	for {
		s := ccall(iokit.IOIteratorNext, uintptr(iter)) & 0xffffffff
		if s == 0 {
			break
		}
		if client == 0 && (want[regString(s, "IOCalloutDevice")] || want[regString(s, "IODialinDevice")]) {
			client = s
			continue
		}
		ccall(iokit.IOObjectRelease, s)
	}
	if client == 0 {
		return 0, usbDevice{}, fmt.Errorf("no serial port %s in the I/O Registry (is the board plugged in?)", port)
	}
	plane := cstr("IOService")
	host, dev := cstr("IOUSBHostDevice"), cstr("IOUSBDevice")
	e := client
	for {
		var parent uint32
		kr := uint32(ccall(iokit.IORegistryEntryGetParentEntry, e, uintptr(unsafe.Pointer(&plane[0])), uintptr(unsafe.Pointer(&parent))))
		ccall(iokit.IOObjectRelease, e)
		if kr != kIOReturnSuccess || parent == 0 {
			return 0, usbDevice{}, fmt.Errorf("%s is not on a USB device", port)
		}
		e = uintptr(parent)
		if ccall(iokit.IOObjectConformsTo, e, uintptr(unsafe.Pointer(&host[0])))&0xff != 0 ||
			ccall(iokit.IOObjectConformsTo, e, uintptr(unsafe.Pointer(&dev[0])))&0xff != 0 {
			break
		}
	}
	runtime.KeepAlive(plane)
	var d usbDevice
	vid, ok1 := regInt(e, "idVendor")
	pid, _ := regInt(e, "idProduct")
	loc, ok2 := regInt(e, "locationID")
	if !ok1 || !ok2 {
		ccall(iokit.IOObjectRelease, e)
		return 0, usbDevice{}, fmt.Errorf("the USB device of %s has no vendor id or location", port)
	}
	d.vendor, d.product, d.location = int(vid), int(pid), loc
	return e, d, nil
}

// UUIDs of the IOKit USB plug-in (IOUSBLib.h, IOCFPlugIn.h).
const (
	uuidUSBDeviceUserClientType = "9DC7B780-9EC0-11D4-A54F-000A27052861"
	uuidCFPlugInInterface       = "C244E858-109C-11D4-91D4-0050E4C6426F"
)

// uuidUSBDeviceInterface187 (kIOUSBDeviceInterfaceID187) as the two
// machine words a 16-byte CFUUIDBytes passed by value takes (two integer
// registers on arm64 and amd64).
var uuidUSBDeviceInterface187 = [2]uintptr{
	uintptr(0xB211_0224_EBE1_9E3C),
	uintptr(0x861E_8027_0A00_7E8E),
}

// Vtable slots (pointer-sized words) of IOCFPlugInInterface and
// IOUSBDeviceInterface187: _reserved, QueryInterface, AddRef, Release,
// ... (IOUSBLib.h; checked with offsetof against the SDK).
const (
	slotQueryInterface       = 1
	slotRelease              = 3
	slotUSBDeviceOpen        = 8
	slotUSBDeviceClose       = 9
	slotDeviceRequestTO      = 30
	slotUSBDeviceReEnumerate = 37
)

// usbIface is an IOUSBDeviceInterface187 ** (a COM-style interface).
type usbIface uintptr

// call calls the interface's function in slot, d first.
//
//go:uintptrescapes
func (d usbIface) call(slot uintptr, args ...uintptr) uint32 {
	vt := peek(uintptr(d))
	fn := peek(vt + slot*unsafe.Sizeof(uintptr(0)))
	return uint32(ccall(fn, append([]uintptr{uintptr(d)}, args...)...))
}

// openDeviceInterface gets the device interface of the USB device service
// dev; release it with call(slotRelease).
func openDeviceInterface(dev uintptr) (usbIface, error) {
	t, p := cfString(uuidUSBDeviceUserClientType), cfString(uuidCFPlugInInterface)
	defer ccall(iokit.CFRelease, t)
	defer ccall(iokit.CFRelease, p)
	tu, pu := ccall(iokit.CFUUIDCreateFromString, 0, t), ccall(iokit.CFUUIDCreateFromString, 0, p)
	if tu == 0 || pu == 0 {
		return 0, fmt.Errorf("CFUUIDCreateFromString failed")
	}
	defer ccall(iokit.CFRelease, tu)
	defer ccall(iokit.CFRelease, pu)
	var plugin uintptr
	var score int32
	if kr := uint32(ccall(iokit.IOCreatePlugInInterfaceForService, dev, tu, pu, uintptr(unsafe.Pointer(&plugin)), uintptr(unsafe.Pointer(&score)))); kr != kIOReturnSuccess || plugin == 0 {
		return 0, fmt.Errorf("cannot reach the USB device (IOCreatePlugInInterfaceForService: %#x)", kr)
	}
	var iface uintptr
	hr := usbIface(plugin).call(slotQueryInterface, uuidUSBDeviceInterface187[0], uuidUSBDeviceInterface187[1], uintptr(unsafe.Pointer(&iface)))
	ccall(iokit.IODestroyPlugInInterface, plugin)
	if hr != 0 || iface == 0 {
		return 0, fmt.Errorf("cannot reach the USB device (QueryInterface: %#x)", hr)
	}
	return usbIface(iface), nil
}

// usbResetPort re-enumerates the Espressif USB device behind port.
func usbResetPort(port string) (string, error) {
	dev, d, err := findUSBDevice(port)
	if err != nil {
		return "", err
	}
	defer ccall(iokit.IOObjectRelease, dev)
	if err := checkEspressif(port, d); err != nil {
		return "", err
	}
	iface, err := openDeviceInterface(dev)
	if err != nil {
		return "", err
	}
	defer iface.call(slotRelease)
	if kr := iface.call(slotUSBDeviceOpen); kr != kIOReturnSuccess {
		return "", fmt.Errorf("cannot open %s (%#x; another program may hold it): %s", d, kr, replugHint)
	}
	kr := iface.call(slotUSBDeviceReEnumerate, 0)
	iface.call(slotUSBDeviceClose)
	if kr != kIOReturnSuccess {
		return "", fmt.Errorf("resetting %s failed (%#x): %s", d, kr, replugHint)
	}
	return d.String(), nil
}

// devRequestTO is IOUSBDevRequestTO (IOUSBLib.h) followed by its data.
type devRequestTO struct {
	bmRequestType, bRequest uint8
	wValue, wIndex, wLength uint16
	pData                   uintptr
	wLenDone                uint32
	noDataTimeout           uint32
	completionTimeout       uint32
	_                       uint32
	data                    [255]byte
}

// usbProbe asks the device behind port for its string descriptor 0 (the
// language ids): silent when the request times out or returns no
// descriptor; another failure is an error.
func usbProbe(port string) (bool, error) {
	dev, d, err := findUSBDevice(port)
	if err != nil {
		return false, err
	}
	defer ccall(iokit.IOObjectRelease, dev)
	if err := checkEspressif(port, d); err != nil {
		return false, err
	}
	iface, err := openDeviceInterface(dev)
	if err != nil {
		return false, err
	}
	defer iface.call(slotRelease)
	r := &devRequestTO{bmRequestType: 0x80, bRequest: 6, wValue: 0x0300, noDataTimeout: 500, completionTimeout: 1000}
	r.wLength = uint16(len(r.data))
	r.pData = uintptr(unsafe.Pointer(&r.data[0]))
	kr := iface.call(slotDeviceRequestTO, uintptr(unsafe.Pointer(r)))
	runtime.KeepAlive(r)
	switch kr {
	case kIOReturnSuccess:
		// An empty or malformed answer is the descriptor unreadable.
		return r.wLenDone < 4 || r.data[1] != 3, nil
	case kIOReturnTimeout, kIOReturnNotResponding, kIOUSBTransactionTimeout:
		return true, nil
	}
	return false, fmt.Errorf("the string descriptor request to %s failed (%#x)", d, kr)
}

// portHolders lists the user's other processes with port (or its tty./cu.
// twin) open, with lsof.
func portHolders(port string) ([]int, error) {
	lsof := "/usr/sbin/lsof"
	if _, err := os.Stat(lsof); err != nil {
		lsof = "lsof"
	}
	paths := []string{port}
	dir, base := filepath.Split(port)
	switch {
	case len(base) > 3 && base[:3] == "cu.":
		paths = append(paths, dir+"tty."+base[3:])
	case len(base) > 4 && base[:4] == "tty.":
		paths = append(paths, dir+"cu."+base[4:])
	}
	return lsofHolders(lsof, paths, os.Getpid(), lsofTimeout)
}
