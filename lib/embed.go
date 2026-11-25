// Package gogo carries the gogo standard library sources, embedded into the
// compiler (see README.md for the package table). It is the root package
// of module gogo (this directory).
package gogo

import "embed"

// StubFS holds the Go source of the gogo libraries, one directory per
// package: gogo/<name> lives in <name>/*.go (gogo/net in net/). actor/,
// port/, bytes/, net/, config/, fs/, kbd/, power/, code/, imu/, ir/, audio/
// and sys/ are API stubs the compiler replaces with VM instructions; the
// others (supervisor/, strings/, strconv/, dhcp/, dns/, http/, sntp/, kv/,
// ringlog/, imulog/, wav/, sound/, irproto/) are libraries written in the
// gogo subset and compiled into every image that imports them. Libraries
// may import the stubs and other libraries, without cycles.
//
// lib/ is the Go module gogo, so gogo/<name> also resolves to <name>/ for
// the host toolchain and editors (gopls); the root module gogotools
// requires it with replace gogo => ./lib.
//
//go:embed actor/*.go port/*.go bytes/*.go net/*.go config/*.go fs/*.go kbd/*.go power/*.go code/*.go imu/*.go ir/*.go audio/*.go sys/*.go sntp/*.go dhcp/*.go dns/*.go http/*.go kv/*.go ringlog/*.go imulog/*.go wav/*.go sound/*.go supervisor/*.go strings/*.go strconv/*.go irproto/*.go
var StubFS embed.FS
