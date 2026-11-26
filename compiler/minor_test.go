package compiler

import (
	"fmt"
	"maps"
	"testing"
)

// The 15.0 baseline: what a VM of format 15.0 knows. Every opcode,
// builtin, reserved type and reserved field added after it must carry the
// image format minor it appeared in (opMinor, builtinMinor, typeMinor in
// image.go), or an image using it would be stamped too low, load on older
// firmware and die there with badcode. TestMinorBaseline enforces it: a
// change to those tables fails it until the minor tables follow. Change
// the baseline itself only with a major bump (imageMajor), when every
// table restarts at minor 0.
const (
	baselineOps      = 52 // OP_NOP .. OP_SHR64
	baselineBuiltins = 54 // B_PRINTLN .. B_RANDOM
)

// baselineTypes are the reserved types of 15.0, with their field counts.
var baselineTypes = []string{
	"ProcInfo/1", "Reason/2", "ExitMsg/2", "DownMsg/3", "Request/3", "Options/1",
	"gpio.Config/4", "gpio.Write/1", "gpio.Edge/2", "net.IfConfig/0", "net.SetAddr/4", "net.Up/3",
	"net.Down/0", "net.Addr/2", "net.UDP/1", "net.SendTo/2", "net.Datagram/2", "net.Dial/3",
	"net.Listen/1", "net.Shutdown/0", "net.Resume/0", "net.Connected/0", "net.Accepted/1",
	"fs.Info/2", "fs.Space/2", "kbd.Config/2", "kbd.Key/4", "power.Config/1", "power.Battery/4",
	"power.Read/0", "code.Module/2", "port.Chunk/5", "port.Out/1", "port.Done/2", "port.Closed/2",
	"imu.Config/3", "imu.Gesture/2", "ir.TX/2", "ir.RX/1", "audio.Mic/3", "audio.Speaker/2",
	"net.DialTLS/4", "port.Host/2", "port.Post/3",
}

func TestMinorBaseline(t *testing.T) {
	if imageMajor != 15 {
		t.Fatalf("imageMajor %d: a new major restarts the baseline (update this test)", imageMajor)
	}
	for _, e := range minorBaselineErrors() {
		t.Error(e)
	}
}

// TestMinorBaselineCatches: the check fails for an addition without a
// minor and passes once the minor table has it.
func TestMinorBaselineCatches(t *testing.T) {
	saved := reservedTypes
	restore := saveMinors()
	defer func() { reservedTypes = saved; restore() }()
	// Every negative check starts from a clean baseline (the real tables,
	// screen.Config's minor included), so it fails because of its own
	// change only.
	clean := func(what string) {
		t.Helper()
		restore()
		reservedTypes = append([]struct {
			pkg    string
			name   string
			fields []string
		}(nil), saved...)
		if errs := minorBaselineErrors(); len(errs) != 0 {
			t.Fatalf("before %s: the baseline already fails: %v", what, errs)
		}
	}
	clean("a new reserved type")
	reservedTypes = append(reservedTypes, struct {
		pkg    string
		name   string
		fields []string
	}{PortPath, "Extra", []string{"From", "Reason"}})
	if len(minorBaselineErrors()) == 0 {
		t.Error("a new reserved type without a minor passed")
	}
	typeMinor["port.Extra"] = 1
	if errs := minorBaselineErrors(); imageMinor >= 1 && len(errs) != 0 || imageMinor < 1 && len(errs) == 0 {
		t.Errorf("with typeMinor 1 and imageMinor %d: %v", imageMinor, errs)
	}
	clean("an appended field")
	kbd := reservedTypes[TypeKbdConfig]
	reservedTypes[TypeKbdConfig].fields = append(append([]string(nil), kbd.fields...), "Extra")
	if len(minorBaselineErrors()) == 0 {
		t.Error("a field appended without a minor passed")
	}
	clean("a removed field")
	reservedTypes[TypeKbdConfig].fields = kbd.fields[:1]
	if len(minorBaselineErrors()) == 0 {
		t.Error("a removed field passed")
	}
	clean("a misspelt key")
	typeMinor["kbd.Confg"] = 1
	if len(minorBaselineErrors()) == 0 {
		t.Error("a misspelt typeMinor key passed")
	}
	clean("a 15.0 opcode")
	opMinor[OpAdd] = 1
	if len(minorBaselineErrors()) == 0 {
		t.Error("a minor for a 15.0 opcode passed")
	}
	clean("the end")
}

// saveMinors returns a func that puts the minor tables back as they are
// now (tests change them).
func saveMinors() func() {
	o, b, ty := maps.Clone(opMinor), maps.Clone(builtinMinor), maps.Clone(typeMinor)
	return func() {
		clear(opMinor)
		maps.Copy(opMinor, o)
		clear(builtinMinor)
		maps.Copy(builtinMinor, b)
		clear(typeMinor)
		maps.Copy(typeMinor, ty)
	}
}

func minorBaselineErrors() (errs []string) {
	errorf := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }
	top := uint16(0)
	check := func(what string, minor uint16, ok bool) {
		if !ok || minor < 1 || minor > imageMinor {
			errorf("%s: added after 15.0, needs a minor in 1..%d (got %d, present %v)", what, imageMinor, minor, ok)
		}
	}
	if len(opNames) < baselineOps || len(builtinNames) < baselineBuiltins || len(reservedTypes) < len(baselineTypes) {
		errorf("ops %d, builtins %d, reserved types %d: fewer than the 15.0 baseline (%d, %d, %d); removing one is a major",
			len(opNames), len(builtinNames), len(reservedTypes), baselineOps, baselineBuiltins, len(baselineTypes))
		return errs
	}
	for op, m := range opMinor {
		if int(op) < baselineOps {
			errorf("opMinor has opcode %d, which is in 15.0", op)
		}
		top = max(top, m)
	}
	for op := baselineOps; op < len(opNames); op++ {
		m, ok := opMinor[uint8(op)]
		check("opcode "+opNames[op], m, ok)
	}
	for b, m := range builtinMinor {
		if int(b) < baselineBuiltins {
			errorf("builtinMinor has builtin %d, which is in 15.0", b)
		}
		top = max(top, m)
	}
	for b := baselineBuiltins; b < len(builtinNames); b++ {
		m, ok := builtinMinor[uint32(b)]
		check("builtin "+builtinNames[b], m, ok)
	}
	names := map[string]bool{}
	for i, rt := range reservedTypes {
		name := reservedName(i)
		names[name] = true
		now := fmt.Sprintf("%s/%d", name, len(rt.fields))
		m, ok := typeMinor[name]
		switch {
		case i >= len(baselineTypes):
			check("reserved type "+now, m, ok)
		case now == baselineTypes[i]:
			if ok {
				errorf("typeMinor has %s, unchanged since 15.0", name)
			}
		default:
			var n int
			if _, err := fmt.Sscanf(baselineTypes[i][len(name):], "/%d", &n); err != nil || baselineTypes[i][:len(name)] != name || len(rt.fields) < n {
				errorf("reserved type %d is %s, was %s in 15.0: renamed, reordered or a field removed is a major", i, now, baselineTypes[i])
				continue
			}
			check("reserved type "+now+" (fields appended since "+baselineTypes[i]+")", m, ok)
		}
	}
	for name, m := range typeMinor {
		if !names[name] {
			errorf("typeMinor key %q names no reserved type", name)
		}
		top = max(top, m)
	}
	if top > imageMinor {
		errorf("a minor table says %d, above imageMinor %d", top, imageMinor)
	}
	return errs
}
