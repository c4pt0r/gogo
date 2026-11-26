// Command gogoc is libgogoc.a, the gogo compiler as a C library: built
// with go build -buildmode=c-archive (make libgogoc), it exports the
// functions of gogoc.h to C hosts. As an ordinary command it does
// nothing.
package main

/*
#include <stdlib.h>
#include <string.h>
#include "gogoc.h"
*/
import "C"

import (
	"fmt"
	"math"
	"strings"
	"unsafe"

	"gogotools/compiler"
	"gogotools/embed"
)

func main() {}

// gogocCompile is gogoc_compile (gogoc.c), whose input is const.
//
//export gogocCompile
func gogocCompile(input *C.char, inputLen C.size_t, flags C.uint32_t, out **C.uint8_t, outLen *C.size_t, errOut **C.char) (rc C.int) {
	if errOut != nil {
		*errOut = nil
	}
	fail := func(msg string) C.int {
		if errOut != nil {
			*errOut = C.CString(msg)
		}
		return -1
	}
	if out == nil || outLen == nil {
		return fail("gogoc_compile: out and out_len are required")
	}
	*out, *outLen = nil, 0
	// A panic would end the host process (a c-archive's exported function
	// has no Go caller to recover it): it fails the call instead.
	defer func() {
		if r := recover(); r != nil {
			if *out != nil {
				C.free(unsafe.Pointer(*out))
			}
			*out, *outLen = nil, 0
			if errOut != nil && *errOut != nil {
				C.free(unsafe.Pointer(*errOut))
				*errOut = nil
			}
			rc = fail(fmt.Sprint("gogoc_compile: internal error: ", r))
		}
	}()
	if input == nil && inputLen != 0 {
		return fail("gogoc_compile: input is NULL")
	}
	if inputLen > math.MaxInt32 {
		return fail("gogoc_compile: input is 2 GiB or more")
	}
	if flags&^(C.GOGOC_SOURCE|C.GOGOC_GOGOPATH) != 0 {
		return fail("gogoc_compile: unknown flags")
	}
	src := C.GoBytes(unsafe.Pointer(input), C.int(inputLen))
	var warnings []string
	opts := &embed.CompileOptions{Warn: func(w string) { warnings = append(warnings, w) }}
	if flags&C.GOGOC_GOGOPATH != 0 {
		opts.Path = compiler.SearchPath()
	}
	var img []byte
	var err error
	if flags&C.GOGOC_SOURCE != 0 {
		img, err = embed.Compile(map[string][]byte{"main.gg": src}, opts)
	} else {
		if len(src) == 0 {
			return fail("gogoc_compile: no path")
		}
		img, err = embed.CompileDir(string(src), opts)
	}
	if err != nil {
		return fail(err.Error())
	}
	p := C.malloc(C.size_t(len(img)))
	if p == nil {
		return fail("gogoc_compile: out of memory")
	}
	C.memcpy(p, unsafe.Pointer(&img[0]), C.size_t(len(img)))
	*out, *outLen = (*C.uint8_t)(p), C.size_t(len(img))
	if errOut != nil && len(warnings) > 0 {
		*errOut = C.CString(strings.Join(warnings, "\n") + "\n")
	}
	return 0
}

// gogocImageFormat is gogoc_image_format (gogoc.c).
//
//export gogocImageFormat
func gogocImageFormat() C.uint32_t {
	return C.uint32_t(compiler.ImageMajor<<16 | compiler.ImageMinor)
}
