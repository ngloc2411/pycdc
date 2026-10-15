// pycdc decompiles a Python .pyc file back to source.
package main

import (
	"fmt"
	"io"
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

	src, err := decomp.DecompileModule(m, cli.BaseName(a.InFile))
	io.WriteString(out, src)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error decompyling %s: %v\n", a.InFile, err)
		out.Close()
		os.Exit(1)
	}
}
