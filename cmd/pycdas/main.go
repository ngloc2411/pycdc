// pycdas disassembles a Python .pyc file.
package main

import (
	"bufio"
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/internal/cli"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func main() {
	var flags uint
	a, err := cli.Parse(os.Args[1:], func(s string) bool {
		switch s {
		case "--pycode-extra":
			flags |= pyc.DisasmPycodeVerbose
			return true
		case "--show-caches":
			flags |= pyc.DisasmShowCaches
			return true
		}
		return false
	})
	if err != nil {
		cli.Fatal(err.Error())
	}
	if a.Help {
		fmt.Fprintf(os.Stderr, "Usage:  %s [options] input.pyc\n\n", os.Args[0])
		fmt.Fprint(os.Stderr, "Options:\n"+
			"  -o <filename>  Write output to <filename> (default: stdout)\n"+
			"  -c             Specify loading a compiled code object. Requires the version to be set\n"+
			"  -v <x.y>       Specify a Python version for loading a compiled code object\n"+
			"  --pycode-extra Show extra fields in PyCode object dumps\n"+
			"  --show-caches  Don't suprress CACHE instructions in Python 3.11+ disassembly\n"+
			"  --help         Show this help text and then exit\n")
		return
	}
	if a.InFile == "" {
		cli.Fatal("No input file specified")
	}
	m, err := cli.Load(a)
	if err != nil {
		cli.Fatal(err.Error())
	}
	out := os.Stdout
	if a.Out != "" {
		out, err = os.Create(a.Out)
		if err != nil {
			cli.Fatal(fmt.Sprintf("Error opening file '%s' for writing", a.Out))
		}
		defer out.Close()
	}
	w := bufio.NewWriter(out)
	defer w.Flush()
	extra := ""
	if m.MajorVer() < 3 && m.IsUnicode() {
		extra = " -U"
	}
	fmt.Fprintf(w, "%s (Python %d.%d%s)\n", cli.BaseName(a.InFile), m.MajorVer(), m.MinorVer(), extra)
	defer func() {
		if r := recover(); r != nil {
			w.Flush()
			fmt.Fprintf(os.Stderr, "Error disassembling %s: %v\n", a.InFile, r)
			os.Exit(1)
		}
	}()
	pyc.DumpObject(m.Code, m, 0, flags, w)
}
