package main

import (
	"os"
	"testing"
)

// TestCorpusUpToDate regenerates runtime/vm/test/float_corpus.h and fails
// if the committed file differs (run `make float-corpus`).
func TestCorpusUpToDate(t *testing.T) {
	cur, err := os.ReadFile("../../" + out)
	if err != nil {
		t.Fatal(err)
	}
	if string(cur) != generate() {
		t.Error(out + " is stale: run `make float-corpus`")
	}
}
