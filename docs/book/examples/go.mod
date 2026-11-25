// The book's examples are gogo programs, not Go packages of module gogo
// (they import gogo/net and use println forms that real Go rejects); this
// nested module keeps `go test ./...` and `go vet ./...` out of them.
// Run them all with ./run.sh from this directory.
module bookexamples

go 1.27
