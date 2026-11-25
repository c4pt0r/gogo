module gogotools

go 1.27

// The gogo standard library (lib/, module gogo): the compiler embeds its
// sources (lib/embed.go, package gogo).
require gogo v0.0.0

replace gogo => ./lib
