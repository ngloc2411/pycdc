// pycdc decompiles a Python .pyc file back to source.
package main

import (
	"bytes"
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/decomp"
	"github.com/ngloc2411/pycdc/internal/cli"
)

func main() {
	a, err := cli.Parse(os.Args[1:], nil)
	if err != nil {
		cli.Fatal(err.Error())
	}
	if a.Help {
		fmt.Fprintf(os.Stderr, "Usage:  %s [options] input.pyc\n\n", os.Args[0])
		fmt.Fprint(os.Stderr, "Options:\n"+
			"  -o <filename>  Write output to <filename> (default: stdout)\n"+
			"  -c             Specify loading a compiled code object. Requires the version to be set\n"+
			"  -v <x.y>       Specify a Python version for loading a compiled code object\n"+
			"  --help         Show this help text and then exit\n")
		return
	}
	if a.InFile == "" {
		cli.Fatal("No input file specified")
	}
	out := os.Stdout
	if a.Out != "" {
		out, err = os.Create(a.Out)
		if err != nil {
			cli.Fatal(fmt.Sprintf("Error opening file '%s' for writing", a.Out))
		}
		defer out.Close()
	}
	m, err := cli.Load(a)
	if err != nil {
		cli.Fatal(err.Error())
	}
	if m.Code == nil {
		cli.Fatal(fmt.Sprintf("Could not load file %s", a.InFile))
	}

	var buf bytes.Buffer
	unicode := ""
	if m.MajorVer() < 3 && m.IsUnicode() {
		unicode = " Unicode"
	}
	buf.WriteString("# Source Generated with Decompyle++\n")
	fmt.Fprintf(&buf, "# File: %s (Python %d.%d%s)\n\n", cli.BaseName(a.InFile), m.MajorVer(), m.MinorVer(), unicode)

	code := 0
	func() {
		defer func() {
			if r := recover(); r != nil {
				fmt.Fprintf(os.Stderr, "Error decompyling %s: %v\n", a.InFile, r)
				code = 1
			}
		}()
		decomp.Decompyle(m.Code, m, &buf)
	}()
	out.Write(buf.Bytes())
	if code != 0 {
		out.Close()
		os.Exit(code)
	}
}
