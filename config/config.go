// Package config reads the platform configuration store (spec M5 §5.4).
// It is a stub package like gogo/actor: the compiler replaces Get with a
// VM builtin, so the body is never executed.
//
// The store holds string keys and values, set with `gogo config set key
// value`: on unix the file $GOGO_CONFIG (default ~/.gogo/config), on the
// board its NVS. Keys starting with "wifi." (Wi-Fi credentials) belong to
// the platform and are never visible to programs.
package config

// Get returns the value of key; ok is false if key is not set, or starts
// with "wifi.".
func Get(key string) (value string, ok bool) { panic("gogo only") }
