// Scenario programs for the network tests are gogo sources, not Go
// packages of module gogo; this nested module keeps `go test ./...` and
// `go vet ./...` out of them (as tests/golden does).
module netprogs

go 1.27
