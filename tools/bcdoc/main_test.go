package main

import (
	"os"
	"strings"
	"testing"
)

func read(t *testing.T, p string) string {
	d, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return string(d)
}

// TestDocUpToDate regenerates docs/bytecode.md and fails if the committed
// file differs (run `make bytecode-doc`).
func TestDocUpToDate(t *testing.T) {
	out, err := generate(read(t, "../../runtime/vm/opcodes.h"), read(t, "../../runtime/vm/image.h"))
	if err != nil {
		t.Fatal(err)
	}
	if cur := read(t, "../../docs/bytecode.md"); cur != out {
		t.Error("docs/bytecode.md is stale: run `make bytecode-doc`")
	}
}

func TestParseErrors(t *testing.T) {
	base := "enum Op {\n    OP_NOP = 0,   /*! - | - | Nothing. */\n};\nenum Builtin {\n    B_X = 0   /*! -> nil | x. */\n};\n"
	if _, err := generate(base, ""); err != nil {
		t.Fatal(err)
	}
	for name, src := range map[string]string{
		"missing comment": strings.Replace(base, "/*! - | - | Nothing. */", "/* plain */", 1),
		"two fields":      strings.Replace(base, "- | - | Nothing.", "- | Nothing.", 1),
		"no arrow":        strings.Replace(base, "-> nil | x.", "nil | x.", 1),
		"gap":             strings.Replace(base, "OP_NOP = 0", "OP_NOP = 1", 1),
	} {
		if _, err := generate(src, ""); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
}
