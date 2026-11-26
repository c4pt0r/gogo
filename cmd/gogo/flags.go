package main

// Every subcommand parses its flags with one flag.FlagSet (newFlags) and
// parseFlags, so they all behave alike:
//
//   - a flag is spelt -name or --name (the flag package takes both), and a
//     value follows as the next argument or after "=" (-o=out.ggb);
//   - flags may come before, between or after the positional arguments;
//     "--" ends the flags (everything after it is positional), and a lone
//     "-" is a positional argument;
//   - -h or -help prints the command's usage on standard output and exits 0;
//   - any other mistake is an error naming it, followed by the usage.
//
// The names are the same everywhere: -port (the board's serial port),
// -board, -o (output), -timeout (seconds), -esptool, -path, -yes.

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strings"
)

// helpError is a request for a command's usage (-h): printed on standard
// output, exit status 0.
type helpError struct{ usage string }

func (e helpError) Error() string { return e.usage }

// newFlags returns an empty flag set for `gogo <name>` that reports its
// errors to parseFlags instead of printing them.
func newFlags(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.Usage = func() {}
	return fs
}

// parseFlags parses args with fs, flags and positional arguments in any
// order, and returns the positional ones in order. usage is the command's
// usage text, appended to every error.
func parseFlags(fs *flag.FlagSet, args []string, usage string) ([]string, error) {
	return parseFlagsRaw(fs, args, usage, nil)
}

// parseFlagsRaw is parseFlags where raw(pos), given the positional
// arguments so far, says whether the next argument is positional whatever
// it looks like: `gogo config set k -5` sets k to "-5".
func parseFlagsRaw(fs *flag.FlagSet, args []string, usage string, raw func(pos []string) bool) ([]string, error) {
	var pos []string
	for {
		if raw != nil && len(args) > 0 && raw(pos) {
			pos = append(pos, args[0])
			args = args[1:]
			continue
		}
		if err := fs.Parse(args); err != nil {
			if errors.Is(err, flag.ErrHelp) {
				return nil, helpError{usage}
			}
			return nil, fmt.Errorf("gogo %s: %v\n%s", fs.Name(), err, usage)
		}
		rest := fs.Args()
		// fs.Parse stops at the first positional argument, or after "--"
		// (which it drops: then everything left is positional).
		if len(rest) == 0 {
			return pos, nil
		}
		if len(args) > len(rest) && args[len(args)-len(rest)-1] == "--" {
			return append(pos, rest...), nil
		}
		pos = append(pos, rest[0])
		args = rest[1:]
	}
}

// listFlag is a flag that may be given several times (run's --key).
type listFlag []string

func (l *listFlag) String() string     { return strings.Join(*l, ",") }
func (l *listFlag) Set(v string) error { *l = append(*l, v); return nil }

// usageError is a usage mistake found after parsing (a missing or extra
// argument): why, then the usage.
func usageError(cmd, why, usage string) error {
	return fmt.Errorf("gogo %s: %s\n%s", cmd, why, usage)
}

// flagSet reports whether the flag name was given.
func flagSet(fs *flag.FlagSet, name string) bool {
	set := false
	fs.Visit(func(f *flag.Flag) {
		if f.Name == name {
			set = true
		}
	})
	return set
}
