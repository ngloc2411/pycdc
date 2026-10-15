// Package cli holds the argument handling shared by pycdas and pycdc.
package cli

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/ngloc2411/pycdc/pyc"
)

// Args are the parsed common options.
type Args struct {
	InFile     string
	Out        string
	Marshalled bool
	Version    string
	Flags      uint
	Help       bool
}

// Parse parses os.Args style arguments; extra handles tool specific options.
func Parse(argv []string, extra func(string) bool) (Args, error) {
	var a Args
	for i := 0; i < len(argv); i++ {
		s := argv[i]
		switch {
		case s == "-o":
			if i+1 >= len(argv) {
				return a, fmt.Errorf("Option '-o' requires a filename")
			}
			i++
			a.Out = argv[i]
		case s == "-c":
			a.Marshalled = true
		case s == "-v":
			if i+1 >= len(argv) {
				return a, fmt.Errorf("Option '-v' requires a version")
			}
			i++
			a.Version = argv[i]
		case s == "--help" || s == "-h":
			a.Help = true
			return a, nil
		case extra != nil && extra(s):
		case strings.HasPrefix(s, "-") && len(s) > 1:
			return a, fmt.Errorf("Error: Unrecognized argument %s", s)
		default:
			a.InFile = s
		}
	}
	return a, nil
}

// Load loads the module described by a.
func Load(a Args) (*pyc.Module, error) {
	if !a.Marshalled {
		m, err := pyc.LoadFile(a.InFile)
		if err != nil {
			return nil, fmt.Errorf("Error loading file %s: %v", a.InFile, err)
		}
		return m, nil
	}
	if a.Version == "" {
		return nil, fmt.Errorf("Opening raw code objects requires a version to be specified")
	}
	dot := strings.Index(a.Version, ".")
	if dot < 0 || dot == len(a.Version)-1 {
		return nil, fmt.Errorf("Unable to parse version string (use the format x.y)")
	}
	maj, err1 := strconv.Atoi(a.Version[:dot])
	min, err2 := strconv.Atoi(a.Version[dot+1:])
	if err1 != nil || err2 != nil {
		return nil, fmt.Errorf("Unable to parse version string (use the format x.y)")
	}
	return pyc.LoadMarshalled(a.InFile, maj, min)
}

// BaseName is the file name without directories.
func BaseName(p string) string {
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		return p[i+1:]
	}
	return p
}

// Fatal prints msg to stderr and exits 1.
func Fatal(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}
