// Golden programs are gogo sources, not Go packages of module gogo (some use
// println forms that real Go rejects); this nested module keeps
// `go test ./...` and `go vet ./...` out of them.
module golden

go 1.27
