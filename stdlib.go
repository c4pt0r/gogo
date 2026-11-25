// Package gogo carries the embedded library sources used by the compiler.
package gogo

import "embed"

// StubFS holds the Go source of the gogo libraries, one directory per
// package: actor/, port/, bytes/, netapi/, config/, fs/, kbd/, power/ and
// code/ (gogo/actor, gogo/port, gogo/bytes, gogo/net, gogo/config, gogo/fs,
// gogo/kbd, gogo/power and gogo/code, API stubs the compiler replaces with
// VM instructions) and supervisor/
// (gogo/supervisor, compiled into the image), strings/ and strconv/ (gogo/strings
// and gogo/strconv, compiled into the image like supervisor), dhcp/,
// dns/ and http/ (gogo/dhcp, gogo/dns and gogo/http, likewise; Go build
// tag gogo, since they import gogo/net), kv/, ringlog/ and imulog/ (gogo/kv,
// gogo/ringlog and gogo/imulog, compiled into the image on top of gogo/fs), irproto/ (gogo/irproto, IR
// protocol codecs, compiled into the image). Libraries may import the stubs and other libraries,
// without cycles.
//
//go:embed actor/*.go port/*.go bytes/*.go netapi/*.go config/*.go fs/*.go kbd/*.go power/*.go code/*.go imu/*.go ir/*.go dhcp/*.go dns/*.go http/*.go kv/*.go ringlog/*.go imulog/*.go supervisor/*.go strings/*.go strconv/*.go irproto/*.go
var StubFS embed.FS
