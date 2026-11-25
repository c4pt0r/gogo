// The ESP examples are gogo sources, not Go packages of the repository's module gogotools (their
// gogo/... imports resolve only in the gogo compiler, from lib/); this nested
// module keeps `go test ./...` and `go vet ./...` out of them.
module espexamples

go 1.27

// gogo/... imports resolve to the standard library sources in lib/ (module
// gogo), for editors and gopls; the gogo compiler embeds its own copy.
require gogo v0.0.0

replace gogo => ../../../lib
