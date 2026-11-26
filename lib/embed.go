// Package lib carries the gogo standard library sources, embedded into the
// compiler (see README.md for the package table). It is package gogotools/lib
// of the repository's module; its only Go file is this one; the library
// itself is gogo source (.gg), which the host Go toolchain does not read.
package lib

import "embed"

// StubFS holds the gogo source of the gogo libraries, one directory per
// package: gogo/<name> lives in <name>/*.gg (gogo/net in net/). actor/,
// port/, bytes/, net/, gpio/, fs/, kbd/, power/, code/, imu/, ir/, audio/,
// screen/, sys/, errors/ and math/ are API stubs the compiler replaces with VM instructions; the
// others (supervisor/, strings/, strconv/, dhcp/, dns/, http/, sntp/, kv/,
// ringlog/, imulog/, wav/, sound/, irproto/, tui/) are libraries written in the
// gogo subset and compiled into every image that imports them. Libraries
// may import the stubs and other libraries, without cycles.
//
//go:embed */*.gg
var StubFS embed.FS
