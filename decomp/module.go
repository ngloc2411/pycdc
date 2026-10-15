package decomp

import (
	"bytes"
	"fmt"

	"github.com/ngloc2411/pycdc/decomp/ast"
	"github.com/ngloc2411/pycdc/pyc"
)

// resetState restores the package state to what a fresh process would have.
// The builder and printer share a few flags; decompiling several modules in
// one process (tests, tools) must not see the leftovers of the previous one.
func resetState() {
	cleanBuild = false
	inLambda = false
	lambdaBare = false
	genexprBare = false
	printDocstringAndGlobals = false
	printClassDocstring = true
	curIndent = -1
	pendingDecorators = nil
	nodeSeen = map[ast.Node]bool{}
	codeSeen = map[*pyc.Code]bool{}
}

// DecompileModule decompiles the whole module m. name is the file name shown
// in the header comment. On a fatal error the source produced so far is
// returned together with the error.
func DecompileModule(m *pyc.Module, name string) (src string, err error) {
	resetState()
	var buf bytes.Buffer
	unicode := ""
	if m.MajorVer() < 3 && m.IsUnicode() {
		unicode = " Unicode"
	}
	buf.WriteString("# Source Generated with Decompyle++\n")
	fmt.Fprintf(&buf, "# File: %s (Python %d.%d%s)\n\n", name, m.MajorVer(), m.MinorVer(), unicode)
	defer func() {
		if r := recover(); r != nil {
			src, err = buf.String(), fmt.Errorf("%v", r)
		}
	}()
	Decompyle(m.Code, m, &buf)
	return buf.String(), nil
}
