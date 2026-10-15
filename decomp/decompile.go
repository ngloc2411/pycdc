package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/decomp/ast"
	"github.com/ngloc2411/pycdc/pyc"
)

// codeSeen guards against circular references between code objects.
var codeSeen = map[*pyc.Code]bool{}

func isNoneObject(n ast.Node) bool {
	if ast.TypeOf(n) != ast.NODE_OBJECT {
		return false
	}
	return n.(*ast.Object).Object().Type() == pyc.TypeNone
}

// isReturnNone reports whether n is a (possibly implicit) `return None`.
func isReturnNone(n ast.Node) bool {
	if n == nil || n.Type() != ast.NODE_RETURN || n.(*ast.Return).RetType() != ast.RET_RETURN {
		return false
	}
	v := n.(*ast.Return).Value()
	if v == nil {
		return true
	}
	return isNoneObject(v)
}

func childNodes(node ast.Node) []ast.Node {
	switch ast.TypeOf(node) {
	case ast.NODE_NODELIST:
		return node.(*ast.NodeList).Nodes()
	case ast.NODE_BLOCK:
		return node.(*ast.Block).Nodes()
	}
	return nil
}

func removeLastChild(node ast.Node) {
	if node.Type() == ast.NODE_NODELIST {
		node.(*ast.NodeList).RemoveLast()
	} else {
		node.(*ast.Block).RemoveLast()
	}
}

// stripTailReturnNone drops the `return None` Python 3.x emits at the end of
// every exit path of a function (each except handler, if/else branch, ...).
// When that path is the very last thing in the function the return is
// implicit. Only the trailing statement chain is touched: loops are left alone.
func stripTailReturnNone(node ast.Node) {
	if node == nil {
		return
	}
	nodes := childNodes(node)
	if len(nodes) == 0 {
		return
	}

	last := nodes[len(nodes)-1]
	if isReturnNone(last) {
		removeLastChild(node)
		return
	}
	if last.Type() != ast.NODE_BLOCK {
		return
	}

	strippable := func(b *ast.Block) bool {
		switch b.BlkType() {
		case ast.BLK_IF, ast.BLK_ELIF, ast.BLK_ELSE, ast.BLK_TRY, ast.BLK_EXCEPT,
			ast.BLK_FINALLY, ast.BLK_WITH, ast.BLK_CONTAINER:
			return true
		}
		return false
	}

	// Walk the trailing run of sibling blocks that form one statement
	// (if/elif/else chain, or try/except/else/finally).
	for it := len(nodes) - 1; it >= 0; it-- {
		if nodes[it].Type() != ast.NODE_BLOCK {
			break
		}
		b := nodes[it].(*ast.Block)
		if !strippable(b) {
			break
		}
		if b.BlkType() == ast.BLK_CONTAINER {
			hasElseOrFinally := false
			for _, c := range b.Nodes() {
				if c.Type() == ast.NODE_BLOCK &&
					(c.(*ast.Block).BlkType() == ast.BLK_ELSE || c.(*ast.Block).BlkType() == ast.BLK_FINALLY) {
					hasElseOrFinally = true
				}
			}
			// A container's children are try/except/else/finally blocks.
			if !hasElseOrFinally {
				for _, c := range b.Nodes() {
					if c.Type() == ast.NODE_BLOCK {
						stripTailReturnNone(c)
					}
				}
			} else {
				bn := b.Nodes()
				lastChild := bn[len(bn)-1]
				if lastChild.Type() == ast.NODE_BLOCK && lastChild.(*ast.Block).BlkType() == ast.BLK_ELSE {
					stripTailReturnNone(lastChild)
				}
			}
		} else {
			stripTailReturnNone(nodes[it])
		}
		if b.BlkType() == ast.BLK_IF || b.BlkType() == ast.BLK_TRY ||
			b.BlkType() == ast.BLK_WITH || b.BlkType() == ast.BLK_CONTAINER {
			break
		}
	}
}

// stripModuleTrailingReturn: a `return` is invalid at module scope, yet the
// implicit "return None" (and, with nested `if`s, copies of it) can end up
// inside module-level blocks. Strip the trailing bare return from this block
// and, recursively, from every nested block. Only ever called on the <module>
// code object.
func stripModuleTrailingReturn(node ast.Node) {
	if node == nil {
		return
	}
	nodes := childNodes(node)
	if len(nodes) == 0 {
		return
	}
	for _, child := range nodes {
		if child.Type() == ast.NODE_BLOCK {
			stripModuleTrailingReturn(child)
		}
	}
	nodes = childNodes(node)
	if len(nodes) != 0 && nodes[len(nodes)-1].Type() == ast.NODE_RETURN &&
		nodes[len(nodes)-1].(*ast.Return).RetType() == ast.RET_RETURN {
		removeLastChild(node)
	}
}

// lambdaExpr folds a statement list back into one expression. A lambda body
// is a single expression, but `lambda x: a if x else b` compiles to the same
// jump-threaded code as `if x: return a; return b`. nil when it has another
// shape.
func lambdaExpr(stmts []ast.Node, from int) ast.Node {
	if from >= len(stmts) {
		return nil
	}
	first := stmts[from]
	if ast.TypeOf(first) == ast.NODE_RETURN {
		if from+1 != len(stmts) {
			return nil
		}
		r := first.(*ast.Return)
		if r.RetType() == ast.RET_RETURN {
			return r.Value()
		}
		return nil
	}
	if ast.TypeOf(first) != ast.NODE_BLOCK {
		return nil
	}
	blk := first.(*ast.Block)
	if blk.BlkType() != ast.BLK_IF && blk.BlkType() != ast.BLK_ELIF {
		return nil
	}
	thenExpr := lambdaExpr(blk.Nodes(), 0)
	var elseExpr ast.Node
	if from+1 < len(stmts) && ast.TypeOf(stmts[from+1]) == ast.NODE_BLOCK &&
		stmts[from+1].(*ast.Block).BlkType() == ast.BLK_ELSE && from+2 == len(stmts) {
		elseExpr = lambdaExpr(stmts[from+1].(*ast.Block).Nodes(), 0)
	} else {
		elseExpr = lambdaExpr(stmts, from+1)
	}
	if thenExpr == nil || elseExpr == nil {
		return nil
	}
	tb := ast.NewCondBlock(ast.BLK_IF, 0, blk.Cond(), blk.Negative())
	return ast.NewTernary(tb, thenExpr, elseExpr)
}

func firstNode(clean *ast.NodeList) ast.Node {
	if n := clean.Nodes(); len(n) != 0 {
		return n[0]
	}
	return nil
}

func lastNode(clean *ast.NodeList) ast.Node {
	if n := clean.Nodes(); len(n) != 0 {
		return n[len(n)-1]
	}
	return nil
}

// Decompyle rebuilds the source of one code object (post-processing of the
// built tree and top-level output).
func Decompyle(code *pyc.Code, mod *pyc.Module, w Out) {
	if codeSeen[code] {
		fmt.Fprint(os.Stderr, "WARNING: Circular reference detected\n")
		return
	}
	codeSeen[code] = true
	defer delete(codeSeen, code)

	source := BuildFromCode(code, mod)

	clean := source.(*ast.NodeList)
	name := ""
	if code.Name != nil {
		name = code.Name.Value
	}
	if cleanBuild {
		// The Python compiler adds some stuff that we don't really care about,
		// and would add extra code for re-compilation anyway. Strip these lines
		// out here, and add a "pass" statement if the cleaned up code is empty.
		if f := firstNode(clean); ast.TypeOf(f) == ast.NODE_STORE {
			store := f.(*ast.Store)
			if ast.TypeOf(store.Src()) == ast.NODE_NAME && ast.TypeOf(store.Dest()) == ast.NODE_NAME {
				src := store.Src().(*ast.Name)
				dest := store.Dest().(*ast.Name)
				if src.Name().Value == "__name__" && dest.Name().Value == "__module__" {
					// __module__ = __name__ (Python 2.2.1 and later)
					clean.RemoveFirst()
				}
			}
		}
		if f := firstNode(clean); ast.TypeOf(f) == ast.NODE_STORE {
			store := f.(*ast.Store)
			if ast.TypeOf(store.Src()) == ast.NODE_OBJECT && ast.TypeOf(store.Dest()) == ast.NODE_NAME {
				dest := store.Dest().(*ast.Name)
				if dest.Name().Value == "__qualname__" {
					// __qualname__ = '<Class Name>' (Python 3.3 and later)
					clean.RemoveFirst()
				}
			}
		}

		// Class and module docstrings may only appear at the beginning of their source
		if f := firstNode(clean); printClassDocstring && ast.TypeOf(f) == ast.NODE_STORE {
			store := f.(*ast.Store)
			if ast.TypeOf(store.Dest()) == ast.NODE_NAME &&
				store.Dest().(*ast.Name).Name().Value == "__doc__" &&
				ast.TypeOf(store.Src()) == ast.NODE_OBJECT {
				ind := curIndent + 1
				if name == "<module>" {
					ind = curIndent
				}
				if PrintDocstring(store.Src().(*ast.Object).Object(), ind, mod, w) {
					clean.RemoveFirst()
				}
			}
		}
		if l := lastNode(clean); ast.TypeOf(l) == ast.NODE_BLOCK && l.(*ast.Block).BlkType() == ast.BLK_MATCH {
			// A match that ends the code: its cases return None instead of
			// jumping to the end
			for _, cn := range l.(*ast.Block).Nodes() {
				if ast.TypeOf(cn) != ast.NODE_BLOCK {
					continue
				}
				cb := cn.(*ast.Block)
				if cb.BlkType() != ast.BLK_CASE || cb.Size() == 0 ||
					ast.TypeOf(cb.Nodes()[cb.Size()-1]) != ast.NODE_RETURN {
					continue
				}
				r := cb.Nodes()[cb.Size()-1].(*ast.Return)
				if r.Value() == nil || isNoneObject(r.Value()) {
					cb.RemoveLast()
				}
			}
		}
		if name == "<lambda>" && len(clean.Nodes()) > 1 {
			stmts := append([]ast.Node(nil), clean.Nodes()...)
			if e := lambdaExpr(stmts, 0); e != nil {
				for len(clean.Nodes()) > 0 {
					clean.RemoveLast()
				}
				clean.Append(ast.NewReturn(e, ast.RET_RETURN))
			}
		}
		if l := lastNode(clean); ast.TypeOf(l) == ast.NODE_RETURN {
			ret := l.(*ast.Return)
			if ret.Value() == nil || ast.TypeOf(ret.Value()) == ast.NODE_LOCALS || isNoneObject(ret.Value()) {
				// ...except in a lambda, where it is the whole body (`lambda: None`).
				if !(name == "<lambda>" && len(clean.Nodes()) == 1) {
					clean.RemoveLast() // Always an extraneous return statement
				}
			}
		}

		// Python 3.11 class bodies end with compiler-generated
		// "__classcell__ = __class__" and "return __class__"; strip them.
		if l := lastNode(clean); ast.TypeOf(l) == ast.NODE_RETURN {
			ret := l.(*ast.Return)
			if ret.Value() != nil && ast.TypeOf(ret.Value()) == ast.NODE_NAME &&
				ret.Value().(*ast.Name).Name().Value == "__class__" {
				clean.RemoveLast()
			}
		}
		if l := lastNode(clean); ast.TypeOf(l) == ast.NODE_STORE {
			st := l.(*ast.Store)
			if ast.TypeOf(st.Dest()) == ast.NODE_NAME &&
				st.Dest().(*ast.Name).Name().Value == "__classcell__" {
				clean.RemoveLast()
			}
		}

		// Strip the module's implicit trailing "return None" (invalid at module
		// scope), even when nested in a trailing block.
		if name == "<module>" {
			stripModuleTrailingReturn(clean)
		} else if name != "<lambda>" {
			stripTailReturnNone(clean)
		}
	}
	if printClassDocstring {
		printClassDocstring = false
	}
	// This is outside the clean check so a source block will always be
	// compilable, even if decompylation failed.
	if len(clean.Nodes()) == 0 && code != mod.Code {
		clean.Append(ast.NewKeyword(ast.KW_PASS))
	}

	part1clean := cleanBuild

	if printDocstringAndGlobals {
		// 3.14 marks a real docstring with CO_HAS_DOCSTRING; otherwise
		// co_consts[0] is just the first constant used by the body.
		if code.Consts.Size() != 0 && (mod.VerCompare(3, 14) < 0 || code.Flags&0x4000000 != 0) {
			PrintDocstring(code.GetConst(0), curIndent+1, mod, w)
		}

		if globs := code.Globals(); len(globs) != 0 {
			startLine(curIndent+1, w)
			w.WriteString("global ")
			for i, g := range globs {
				if i != 0 {
					w.WriteString(", ")
				}
				w.WriteString(g.Value)
			}
			w.WriteString("\n")
		}
		if nonlocs := code.Nonlocals(); len(nonlocs) != 0 {
			startLine(curIndent+1, w)
			w.WriteString("nonlocal ")
			for i, g := range nonlocs {
				if i != 0 {
					w.WriteString(", ")
				}
				w.WriteString(g.Value)
			}
			w.WriteString("\n")
		}
		printDocstringAndGlobals = false
	}

	PrintSrc(source, mod, w)

	if !cleanBuild || !part1clean {
		startLine(curIndent, w)
		w.WriteString("# WARNING: Decompyle incomplete\n")
	}
}
