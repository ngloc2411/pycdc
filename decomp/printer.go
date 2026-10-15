package decomp

import (
	"bytes"
	"fmt"
	"github.com/ngloc2411/pycdc/decomp/ast"
	"os"
	"strconv"
	"strings"

	"github.com/ngloc2411/pycdc/pyc"
)

// Out is the output sink for printed source.
type Out = *bytes.Buffer

// State shared between the AST builder and the printer.
var (
	cleanBuild               bool
	inLambda                 bool
	lambdaBare               bool
	genexprBare              bool
	printDocstringAndGlobals bool
	printClassDocstring      = true
	curIndent                = -1
)

const fStringQuote = "'''"

func cmpPrec(parent, child ast.Node) int {
	if ast.TypeOf(parent) == ast.NODE_UNARY && parent.(*ast.Unary).Op() == ast.UN_NOT {
		return 1
	}
	switch ast.TypeOf(child) {
	case ast.NODE_BINARY:
		binChild := child.(*ast.Binary)
		switch ast.TypeOf(parent) {
		case ast.NODE_BINARY:
			binParent := parent.(*ast.Binary)
			if binParent.Right() == child {
				if binParent.Op() == ast.BIN_SUBTRACT && binChild.Op() == ast.BIN_ADD {
					return 1
				} else if binParent.Op() == ast.BIN_DIVIDE && binChild.Op() == ast.BIN_MULTIPLY {
					return 1
				}
			}
			return binChild.Op() - binParent.Op()
		case ast.NODE_COMPARE:
			if binChild.Op() == ast.BIN_LOG_AND || binChild.Op() == ast.BIN_LOG_OR {
				return 1
			}
			return -1
		case ast.NODE_UNARY:
			if binChild.Op() == ast.BIN_POWER {
				return -1
			}
			return 1
		}
	case ast.NODE_UNARY:
		unChild := child.(*ast.Unary)
		switch ast.TypeOf(parent) {
		case ast.NODE_BINARY:
			binParent := parent.(*ast.Binary)
			if binParent.Op() == ast.BIN_LOG_AND || binParent.Op() == ast.BIN_LOG_OR {
				return -1
			} else if unChild.Op() == ast.UN_NOT {
				return 1
			} else if binParent.Op() == ast.BIN_POWER {
				return 1
			}
			return -1
		case ast.NODE_COMPARE:
			if unChild.Op() == ast.UN_NOT {
				return 1
			}
			return -1
		case ast.NODE_UNARY:
			return unChild.Op() - parent.(*ast.Unary).Op()
		}
	case ast.NODE_COMPARE:
		cmpChild := child.(*ast.Binary)
		switch ast.TypeOf(parent) {
		case ast.NODE_BINARY:
			op := parent.(*ast.Binary).Op()
			if op == ast.BIN_LOG_AND || op == ast.BIN_LOG_OR {
				return -1
			}
			return 1
		case ast.NODE_COMPARE:
			return cmpChild.Op() - parent.(*ast.Binary).Op()
		case ast.NODE_UNARY:
			if parent.(*ast.Unary).Op() == ast.UN_NOT {
				return -1
			}
			return 1
		}
	}
	return -1
}

func printOrdered(parent, child ast.Node, mod *pyc.Module, w Out) {
	switch ast.TypeOf(child) {
	case ast.NODE_BINARY, ast.NODE_COMPARE:
		if cmpPrec(parent, child) > 0 && !(ast.TypeOf(parent) == ast.NODE_UNARY && isPureChain(child)) {
			w.WriteString("(")
			PrintSrc(child, mod, w)
			w.WriteString(")")
		} else {
			PrintSrc(child, mod, w)
		}
	case ast.NODE_UNARY:
		if cmpPrec(parent, child) > 0 {
			w.WriteString("(")
			PrintSrc(child, mod, w)
			w.WriteString(")")
		} else {
			PrintSrc(child, mod, w)
		}
	case ast.NODE_TERNARY:
		w.WriteString("(")
		PrintSrc(child, mod, w)
		w.WriteString(")")
	default:
		PrintSrc(child, mod, w)
	}
}

func flattenAnd(node ast.Node, out *[]ast.Node) {
	if node != nil && node.Type() == ast.NODE_BINARY && node.(*ast.Binary).Op() == ast.BIN_LOG_AND {
		flattenAnd(node.(*ast.Binary).Left(), out)
		flattenAnd(node.(*ast.Binary).Right(), out)
	} else {
		*out = append(*out, node)
	}
}

func chainLinks(a, b ast.Node) bool {
	return a != nil && b != nil && a.Type() == ast.NODE_COMPARE && b.Type() == ast.NODE_COMPARE &&
		a.(*ast.Binary).Right() != nil && a.(*ast.Binary).Right() == b.(*ast.Binary).Left()
}

func hasChain(parts []ast.Node) bool {
	for i := 0; i+1 < len(parts); i++ {
		if chainLinks(parts[i], parts[i+1]) {
			return true
		}
	}
	return false
}

func isPureChain(node ast.Node) bool {
	if node == nil || node.Type() != ast.NODE_BINARY || node.(*ast.Binary).Op() != ast.BIN_LOG_AND {
		return false
	}
	var parts []ast.Node
	flattenAnd(node, &parts)
	for i := 0; i+1 < len(parts); i++ {
		if !chainLinks(parts[i], parts[i+1]) {
			return false
		}
	}
	return hasChain(parts)
}

func printAndChains(parts []ast.Node, mod *pyc.Module, w Out) {
	for i := 0; i < len(parts); i++ {
		if i > 0 {
			w.WriteString(" and ")
		}
		cur := parts[i]
		if ast.TypeOf(cur) != ast.NODE_COMPARE {
			printOrdered(ast.NewBinary(nil, nil, ast.BIN_LOG_AND), cur, mod, w)
			continue
		}
		first := cur.(*ast.Binary)
		printOrdered(cur, first.Left(), mod, w)
		w.WriteString(first.OpStr())
		printOrdered(cur, first.Right(), mod, w)
		for i+1 < len(parts) && chainLinks(parts[i], parts[i+1]) {
			i++
			nx := parts[i].(*ast.Binary)
			w.WriteString(nx.OpStr())
			printOrdered(parts[i], nx.Right(), mod, w)
		}
	}
}

func printTest(node ast.Node, negated, inTernary bool, mod *pyc.Module, w Out) {
	paren := false
	if node != nil {
		switch node.Type() {
		case ast.NODE_BINARY:
			op := node.(*ast.Binary).Op()
			paren = negated && (op == ast.BIN_LOG_AND || op == ast.BIN_LOG_OR)
			if paren && isPureChain(node) {
				paren = false
			}
		case ast.NODE_TERNARY:
			paren = negated || inTernary
		case ast.NODE_FUNCTION:
			paren = inTernary || negated
		}
	}
	if paren {
		w.WriteString("(")
	}
	PrintSrc(node, mod, w)
	if paren {
		w.WriteString(")")
	}
}

func startLine(indent int, w Out) {
	if inLambda {
		return
	}
	for i := 0; i < indent; i++ {
		w.WriteString("    ")
	}
}

func endLine(w Out) {
	if inLambda {
		return
	}
	w.WriteString("\n")
}

var pendingDecorators []ast.Node

func matchDecoratedDef(def, assign ast.Node, decs *[]ast.Node) bool {
	if def == nil || assign == nil || def.Type() != ast.NODE_STORE || assign.Type() != ast.NODE_STORE {
		return false
	}
	d := def.(*ast.Store)
	a := assign.(*ast.Store)
	if ast.TypeOf(d.Dest()) != ast.NODE_NAME || ast.TypeOf(a.Dest()) != ast.NODE_NAME {
		return false
	}
	if ast.TypeOf(d.Src()) == ast.NODE_FUNCTION {
		c := d.Src().(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
		if c.Name.Value == "<lambda>" {
			return false
		}
	} else if ast.TypeOf(d.Src()) != ast.NODE_CLASS {
		return false
	}
	if d.Dest().(*ast.Name).Name().Value != a.Dest().(*ast.Name).Name().Value {
		return false
	}
	node := a.Src()
	var found []ast.Node
	for node != nil && node.Type() == ast.NODE_CALL {
		c := node.(*ast.Call)
		if len(c.Pparams()) != 1 || len(c.Kwparams()) != 0 || c.HasVar() || c.HasKW() {
			return false
		}
		found = append(found, c.Func())
		node = c.Pparams()[0]
	}
	if len(found) == 0 || node == nil || node.Type() != ast.NODE_NAME ||
		node.(*ast.Name).Name().Value != d.Dest().(*ast.Name).Name().Value {
		return false
	}
	*decs = found
	return true
}

func printBlock(blk *ast.Block, mod *pyc.Module, w Out) {
	lines := blk.Nodes()
	if len(lines) == 0 {
		pass := ast.NewKeyword(ast.KW_PASS)
		startLine(curIndent, w)
		PrintSrc(pass, mod, w)
	}
	for i := 0; i < len(lines); {
		skipNext := false
		var decs []ast.Node
		if i+1 < len(lines) && matchDecoratedDef(lines[i], lines[i+1], &decs) {
			pendingDecorators = decs
			skipNext = true
		}
		if ast.TypeOf(lines[i]) != ast.NODE_NODELIST {
			startLine(curIndent, w)
		}
		PrintSrc(lines[i], mod, w)
		i++
		if skipNext {
			i++
		}
		if i < len(lines) {
			endLine(w)
		}
	}
}

func printPendingDecorators(mod *pyc.Module, w Out) {
	decs := pendingDecorators
	pendingDecorators = nil
	for _, d := range decs {
		startLine(curIndent, w)
		w.WriteString("@")
		PrintSrc(d, mod, w)
		w.WriteString("\n")
	}
}

func printFormattedValue(fv *ast.FormattedValue, mod *pyc.Module, w Out) {
	w.WriteString("{")
	PrintSrc(fv.Val(), mod, w)
	switch fv.Conversion() & ast.FV_CONVERSION_MASK {
	case ast.FV_STR:
		w.WriteString("!s")
	case ast.FV_REPR:
		w.WriteString("!r")
	case ast.FV_ASCII:
		w.WriteString("!a")
	}
	if fv.Conversion()&ast.FV_HAVE_FMT_SPEC != 0 {
		w.WriteString(":" + fv.FormatSpec().(*ast.Object).Object().(*pyc.String).Value)
	}
	w.WriteString("}")
}

func printPattern(pat *ast.MatchPattern, mod *pyc.Module, w Out) {
	switch pat.Kind() {
	case ast.PAT_WILDCARD:
		w.WriteString("_")
	case ast.PAT_CAPTURE:
		w.WriteString(pat.Name().Value)
	case ast.PAT_VALUE:
		PrintSrc(pat.Expr(), mod, w)
	case ast.PAT_STAR:
		w.WriteString("*")
		if pat.Name() != nil {
			w.WriteString(pat.Name().Value)
		} else {
			w.WriteString("_")
		}
	case ast.PAT_SEQUENCE:
		w.WriteString("[")
		for i, sub := range pat.Subs() {
			if i > 0 {
				w.WriteString(", ")
			}
			printPattern(sub, mod, w)
		}
		w.WriteString("]")
	case ast.PAT_MAPPING:
		w.WriteString("{")
		first := true
		for i := 0; i < len(pat.Keys()); i++ {
			if !first {
				w.WriteString(", ")
			}
			PrintSrc(pat.Keys()[i], mod, w)
			w.WriteString(": ")
			printPattern(pat.Subs()[i], mod, w)
			first = false
		}
		if pat.Name() != nil {
			if !first {
				w.WriteString(", ")
			}
			w.WriteString("**" + pat.Name().Value)
		}
		w.WriteString("}")
	case ast.PAT_CLASS:
		PrintSrc(pat.Expr(), mod, w)
		w.WriteString("(")
		first := true
		for i := 0; i < len(pat.Subs()); i++ {
			if !first {
				w.WriteString(", ")
			}
			if i >= pat.NumPositional() {
				w.WriteString(pat.KwNames()[i-pat.NumPositional()].Value + "=")
			}
			printPattern(pat.Subs()[i], mod, w)
			first = false
		}
		w.WriteString(")")
	case ast.PAT_OR:
		first := true
		for _, sub := range pat.Subs() {
			if !first {
				w.WriteString(" | ")
			}
			if sub.Kind() == ast.PAT_AS {
				w.WriteString("(")
				printPattern(sub, mod, w)
				w.WriteString(")")
			} else {
				printPattern(sub, mod, w)
			}
			first = false
		}
	case ast.PAT_AS:
		if k := pat.Subs()[0].Kind(); k == ast.PAT_OR || k == ast.PAT_AS {
			w.WriteString("(")
			printPattern(pat.Subs()[0], mod, w)
			w.WriteString(")")
		} else {
			printPattern(pat.Subs()[0], mod, w)
		}
		w.WriteString(" as " + pat.Name().Value)
	}
}

var nodeSeen = map[ast.Node]bool{}

func printUnpackOperand(node ast.Node, mod *pyc.Module, w Out) {
	paren := false
	if node != nil {
		switch node.Type() {
		case ast.NODE_TERNARY, ast.NODE_COMPARE:
			paren = true
		case ast.NODE_BINARY:
			paren = node.(*ast.Binary).Op() != ast.BIN_ATTR
		case ast.NODE_UNARY:
			paren = node.(*ast.Unary).Op() == ast.UN_NOT
		}
	}
	if paren {
		w.WriteString("(")
	}
	PrintSrc(node, mod, w)
	if paren {
		w.WriteString(")")
	}
}

// PrintSrc prints node as Python source.
func PrintSrc(node ast.Node, mod *pyc.Module, w Out) {
	if node == nil {
		w.WriteString("None")
		cleanBuild = true
		return
	}
	if nodeSeen[node] {
		fmt.Fprint(os.Stderr, "WARNING: Circular reference detected\n")
		return
	}
	nodeSeen[node] = true
	if !printNode(node, mod, w) {
		delete(nodeSeen, node)
		return
	}
	cleanBuild = true
	delete(nodeSeen, node)
}

func tupleLike(o pyc.Object) bool {
	t := pyc.TypeOf(o)
	return t == pyc.TypeTuple || t == pyc.TypeSmallTuple
}

func stripParens(t string) string {
	if len(t) > 2 && t[0] == '(' && t[len(t)-1] == ')' {
		return t[1 : len(t)-1]
	}
	return t
}

func printNode(node ast.Node, mod *pyc.Module, w Out) bool {
	switch node.Type() {
	case ast.NODE_BINARY, ast.NODE_COMPARE:
		bin := node.(*ast.Binary)
		if node.Type() == ast.NODE_BINARY && bin.Op() == ast.BIN_LOG_AND {
			var parts []ast.Node
			flattenAnd(node, &parts)
			if hasChain(parts) {
				printAndChains(parts, mod, w)
				return true
			}
		}
		printOrdered(node, bin.Left(), mod, w)
		w.WriteString(bin.OpStr())
		printOrdered(node, bin.Right(), mod, w)
	case ast.NODE_UNARY:
		un := node.(*ast.Unary)
		w.WriteString(un.OpStr())
		printOrdered(node, un.Operand(), mod, w)
	case ast.NODE_CALL:
		call := node.(*ast.Call)
		PrintSrc(call.Func(), mod, w)
		w.WriteString("(")
		first := true
		for _, param := range call.Pparams() {
			if !first {
				w.WriteString(", ")
			}
			lambdaBare = ast.TypeOf(param) == ast.NODE_FUNCTION
			genexprBare = len(call.Pparams()) == 1 && len(call.Kwparams()) == 0 && !call.HasVar() &&
				!call.HasKW() && param != nil && param.Type() == ast.NODE_COMPREHENSION &&
				param.(*ast.Comprehension).Kind() == ast.KIND_GENERATOR
			PrintSrc(param, mod, w)
			genexprBare = false
			lambdaBare = false
			first = false
		}
		starFirst := mod.VerCompare(3, 5) >= 0
		if call.HasVar() && starFirst {
			if !first {
				w.WriteString(", ")
			}
			w.WriteString("*")
			printUnpackOperand(call.Var(), mod, w)
			first = false
		}
		for _, param := range call.Kwparams() {
			if !first {
				w.WriteString(", ")
			}
			if ast.TypeOf(param.K) == ast.NODE_NAME {
				w.WriteString(param.K.(*ast.Name).Name().Value + " = ")
			} else {
				w.WriteString(param.K.(*ast.Object).Object().(*pyc.String).Value + " = ")
			}
			lambdaBare = ast.TypeOf(param.V) == ast.NODE_FUNCTION
			PrintSrc(param.V, mod, w)
			lambdaBare = false
			first = false
		}
		if call.HasVar() && !starFirst {
			if !first {
				w.WriteString(", ")
			}
			w.WriteString("*")
			printUnpackOperand(call.Var(), mod, w)
			first = false
		}
		if call.HasKW() {
			if !first {
				w.WriteString(", ")
			}
			w.WriteString("**")
			printUnpackOperand(call.KW(), mod, w)
			first = false
		}
		w.WriteString(")")
	case ast.NODE_DELETE:
		w.WriteString("del ")
		PrintSrc(node.(*ast.Delete).Value(), mod, w)
	case ast.NODE_EXEC:
		ex := node.(*ast.Exec)
		w.WriteString("exec ")
		PrintSrc(ex.Statement(), mod, w)
		if ex.Globals() != nil {
			w.WriteString(" in ")
			PrintSrc(ex.Globals(), mod, w)
			if ex.Locals() != nil && ex.Globals() != ex.Locals() {
				w.WriteString(", ")
				PrintSrc(ex.Locals(), mod, w)
			}
		}
	case ast.NODE_FORMATTEDVALUE:
		w.WriteString("f" + fStringQuote)
		printFormattedValue(node.(*ast.FormattedValue), mod, w)
		w.WriteString(fStringQuote)
	case ast.NODE_JOINEDSTR:
		w.WriteString("f" + fStringQuote)
		for _, val := range node.(*ast.JoinedStr).Values() {
			switch ast.TypeOf(val) {
			case ast.NODE_FORMATTEDVALUE:
				printFormattedValue(val.(*ast.FormattedValue), mod, w)
			case ast.NODE_OBJECT:
				pyc.PrintConst(w, val.(*ast.Object).Object(), mod, fStringQuote)
			default:
				w.WriteString("{")
				PrintSrc(val, mod, w)
				w.WriteString("}")
			}
		}
		w.WriteString(fStringQuote)
	case ast.NODE_KEYWORD:
		w.WriteString(node.(*ast.Keyword).WordStr())
	case ast.NODE_LIST:
		w.WriteString("[")
		first := true
		curIndent++
		for _, val := range node.(*ast.List).Values() {
			if first {
				w.WriteString("\n")
			} else {
				w.WriteString(",\n")
			}
			startLine(curIndent, w)
			PrintSrc(val, mod, w)
			first = false
		}
		curIndent--
		w.WriteString("]")
	case ast.NODE_SET:
		w.WriteString("{")
		first := true
		curIndent++
		for _, val := range node.(*ast.Set).Values() {
			if first {
				w.WriteString("\n")
			} else {
				w.WriteString(",\n")
			}
			startLine(curIndent, w)
			PrintSrc(val, mod, w)
			first = false
		}
		curIndent--
		w.WriteString("}")
	case ast.NODE_COMPREHENSION:
		comp := node.(*ast.Comprehension)
		isDict := (comp.Result() != nil && comp.Result().Type() == ast.NODE_MAP) || comp.Kind() == ast.KIND_DICT
		isSet := comp.Kind() == ast.KIND_SET
		isGen := comp.Kind() == ast.KIND_GENERATOR
		open, closeS := "[ ", " ]"
		if isDict || isSet {
			open, closeS = "{ ", " }"
		} else if isGen {
			open, closeS = "( ", " )"
		}
		if isGen && genexprBare {
			open, closeS = "", ""
			genexprBare = false
		}
		w.WriteString(open)
		if isDict {
			entries := comp.Result().(*ast.Map).Values()
			if len(entries) != 0 {
				PrintSrc(entries[0].K, mod, w)
				w.WriteString(": ")
				PrintSrc(entries[0].V, mod, w)
			}
		} else {
			PrintSrc(comp.Result(), mod, w)
		}
		for _, gen := range comp.Generators() {
			w.WriteString(" for ")
			PrintSrc(gen.Index(), mod, w)
			w.WriteString(" in ")
			PrintSrc(gen.Iter(), mod, w)
			if gen.Condition() != nil {
				w.WriteString(" if ")
				PrintSrc(gen.Condition(), mod, w)
			}
		}
		w.WriteString(closeS)
	case ast.NODE_MAP:
		w.WriteString("{")
		first := true
		curIndent++
		for _, val := range node.(*ast.Map).Values() {
			if first {
				w.WriteString("\n")
			} else {
				w.WriteString(",\n")
			}
			startLine(curIndent, w)
			if val.K == nil {
				w.WriteString("**")
				printUnpackOperand(val.V, mod, w)
			} else {
				PrintSrc(val.K, mod, w)
				w.WriteString(": ")
				PrintSrc(val.V, mod, w)
			}
			first = false
		}
		curIndent--
		w.WriteString(" }")
	case ast.NODE_CONST_MAP:
		cm := node.(*ast.ConstMap)
		keys := cm.Keys().(*ast.Object).Object().(*pyc.Seq).Values()
		values := append([]ast.Node(nil), cm.Values()...)
		m := ast.NewMap()
		for _, key := range keys {
			value := values[len(values)-1]
			values = values[:len(values)-1]
			m.Add(ast.NewObject(key), value)
		}
		PrintSrc(m, mod, w)
	case ast.NODE_NAME:
		w.WriteString(node.(*ast.Name).Name().Value)
	case ast.NODE_MATCH_PATTERN:
		printPattern(node.(*ast.MatchPattern), mod, w)
	case ast.NODE_NODELIST:
		curIndent++
		nl := node.(*ast.NodeList).Nodes()
		for i := 0; i < len(nl); i++ {
			ln := nl[i]
			var decs []ast.Node
			skipNext := i+1 < len(nl) && matchDecoratedDef(ln, nl[i+1], &decs)
			if skipNext {
				pendingDecorators = decs
			}
			if ast.TypeOf(ln) != ast.NODE_NODELIST {
				startLine(curIndent, w)
			}
			PrintSrc(ln, mod, w)
			endLine(w)
			if skipNext {
				i++
			}
		}
		curIndent--
	case ast.NODE_BLOCK:
		return printBlockNode(node.(*ast.Block), mod, w)
	case ast.NODE_OBJECT:
		obj := node.(*ast.Object).Object()
		if pyc.TypeOf(obj) == pyc.TypeCode {
			Decompyle(obj.(*pyc.Code), mod, w)
		} else {
			pyc.PrintConst(w, obj, mod, "")
		}
	case ast.NODE_PRINT:
		p := node.(*ast.Print)
		w.WriteString("print ")
		first := true
		if p.Stream() != nil {
			w.WriteString(">>")
			PrintSrc(p.Stream(), mod, w)
			first = false
		}
		for _, val := range p.Values() {
			if !first {
				w.WriteString(", ")
			}
			PrintSrc(val, mod, w)
			first = false
		}
		if !p.Eol() {
			w.WriteString(",")
		}
	case ast.NODE_RAISE:
		r := node.(*ast.Raise)
		w.WriteString("raise")
		if mod.VerCompare(3, 0) >= 0 {
			for i, param := range r.Params() {
				if i == 0 {
					w.WriteString(" ")
				} else {
					w.WriteString(" from ")
				}
				PrintSrc(param, mod, w)
			}
		} else {
			for i, param := range r.Params() {
				if i == 0 {
					w.WriteString(" ")
				} else {
					w.WriteString(", ")
				}
				PrintSrc(param, mod, w)
			}
		}
	case ast.NODE_RETURN:
		ret := node.(*ast.Return)
		value := ret.Value()
		if !inLambda {
			switch ret.RetType() {
			case ast.RET_RETURN:
				w.WriteString("return ")
			case ast.RET_YIELD:
				w.WriteString("yield ")
			case ast.RET_YIELD_FROM:
				if ast.TypeOf(value) == ast.NODE_AWAITABLE {
					w.WriteString("await ")
					value = value.(*ast.Awaitable).Expression()
				} else {
					w.WriteString("yield from ")
				}
			}
		}
		lambdaBare = !inLambda && ret.RetType() == ast.RET_RETURN && value != nil && value.Type() == ast.NODE_FUNCTION
		if !inLambda && ret.RetType() == ast.RET_RETURN && value != nil && value.Type() == ast.NODE_TUPLE &&
			len(value.(*ast.Tuple).Values()) != 0 {
			t := value.(*ast.Tuple)
			saved := t.RequireParens()
			t.SetRequireParens(false)
			PrintSrc(value, mod, w)
			t.SetRequireParens(saved)
		} else {
			PrintSrc(value, mod, w)
		}
		lambdaBare = false
	case ast.NODE_SLICE:
		s := node.(*ast.Binary)
		if s.Op()&ast.SLICE1 != 0 {
			PrintSrc(s.Left(), mod, w)
		}
		w.WriteString(":")
		if s.Op()&ast.SLICE2 != 0 {
			PrintSrc(s.Right(), mod, w)
		}
	case ast.NODE_IMPORT:
		printImportNode(node.(*ast.Import), mod, w)
	case ast.NODE_FUNCTION:
		printLambdaFunc(node.(*ast.Function), mod, w)
	case ast.NODE_STORE:
		printStore(node.(*ast.Store), mod, w)
	case ast.NODE_NAMEDEXPR:
		ne := node.(*ast.NamedExpr)
		w.WriteString("(")
		PrintSrc(ne.Name(), mod, w)
		w.WriteString(" := ")
		PrintSrc(ne.Value(), mod, w)
		w.WriteString(")")
	case ast.NODE_CHAINSTORE:
		cs := node.(*ast.ChainStore)
		for _, dest := range cs.Nodes() {
			PrintSrc(dest, mod, w)
			w.WriteString(" = ")
		}
		PrintSrc(cs.Src(), mod, w)
	case ast.NODE_SUBSCR:
		s := node.(*ast.Subscr)
		PrintSrc(s.Name(), mod, w)
		w.WriteString("[")
		key := s.Key()
		if key != nil && key.Type() == ast.NODE_TUPLE && len(key.(*ast.Tuple).Values()) != 0 {
			vals := key.(*ast.Tuple).Values()
			for i, v := range vals {
				if i > 0 {
					w.WriteString(", ")
				}
				PrintSrc(v, mod, w)
			}
			if len(vals) == 1 {
				w.WriteString(",")
			}
		} else if key != nil && key.Type() == ast.NODE_OBJECT && tupleLike(key.(*ast.Object).Object()) &&
			len(key.(*ast.Object).Object().(*pyc.Seq).Values()) != 0 {
			tmp := &bytes.Buffer{}
			PrintSrc(key, mod, tmp)
			w.WriteString(stripParens(tmp.String()))
		} else {
			PrintSrc(key, mod, w)
		}
		w.WriteString("]")
	case ast.NODE_CONVERT:
		w.WriteString("`")
		PrintSrc(node.(*ast.Convert).Name(), mod, w)
		w.WriteString("`")
	case ast.NODE_TUPLE:
		t := node.(*ast.Tuple)
		values := t.Values()
		if t.RequireParens() {
			w.WriteString("(")
		}
		for i, val := range values {
			if i > 0 {
				w.WriteString(", ")
			}
			PrintSrc(val, mod, w)
		}
		if len(values) == 1 {
			w.WriteString(",")
		}
		if t.RequireParens() {
			w.WriteString(")")
		}
	case ast.NODE_ANNOTATED_VAR:
		av := node.(*ast.AnnotatedVar)
		w.WriteString(av.Name().(*ast.Object).Object().(*pyc.String).Value)
		w.WriteString(": ")
		PrintSrc(av.Annotation(), mod, w)
	case ast.NODE_TERNARY:
		t := node.(*ast.Ternary)
		printTest(t.IfExpr(), false, true, mod, w)
		ifBlock := t.IfBlock().(*ast.Block)
		w.WriteString(" if ")
		if ifBlock.Negative() {
			w.WriteString("not ")
		}
		printTest(ifBlock.Cond(), ifBlock.Negative(), true, mod, w)
		w.WriteString(" else ")
		PrintSrc(t.ElseExpr(), mod, w)
	case ast.NODE_AWAITABLE:
		w.WriteString("await ")
		PrintSrc(node.(*ast.Awaitable).Expression(), mod, w)
	default:
		fmt.Fprintf(w, "<NODE:%d>", node.Type())
		fmt.Fprintf(os.Stderr, "Unsupported ast.Node type: %d\n", node.Type())
		cleanBuild = false
		return false
	}
	return true
}

func printBlockNode(blk *ast.Block, mod *pyc.Module, w Out) bool {
	if blk.BlkType() == ast.BLK_ELSE && blk.Size() == 0 {
		return true
	}
	if blk.BlkType() == ast.BLK_CONTAINER {
		endLine(w)
		elseBlk := blk.ElseBlock()
		if elseBlk == nil {
			printBlock(blk, mod, w)
		} else {
			tmp := ast.NewBlock(ast.BLK_CONTAINER, 0, 0)
			var before, after []ast.Node
			seenFinally := false
			for _, c := range blk.Nodes() {
				if ast.TypeOf(c) == ast.NODE_BLOCK && c.(*ast.Block).BlkType() == ast.BLK_FINALLY {
					seenFinally = true
				}
				if seenFinally {
					after = append(after, c)
				} else {
					before = append(before, c)
				}
			}
			for _, c := range before {
				tmp.Append(c)
			}
			tmp.Append(elseBlk)
			for _, c := range after {
				tmp.Append(c)
			}
			printBlock(tmp, mod, w)
		}
		endLine(w)
		return true
	}

	if blk.BlkType() == ast.BLK_IF && blk.Size() == 1 && ast.TypeOf(blk.Nodes()[0]) == ast.NODE_RAISE && blk.Negative() {
		rp := blk.Nodes()[0].(*ast.Raise).Params()
		var exc, msg ast.Node
		if len(rp) == 1 {
			exc = rp[0]
		}
		isAssert := false
		isAssertName := func(n ast.Node) bool {
			return n != nil && n.Type() == ast.NODE_NAME && n.(*ast.Name).Name().Value == "AssertionError"
		}
		if isAssertName(exc) {
			isAssert = true
		} else if exc != nil && exc.Type() == ast.NODE_CALL {
			c := exc.(*ast.Call)
			if isAssertName(c.Func()) && len(c.Pparams()) == 1 && len(c.Kwparams()) == 0 && !c.HasVar() && !c.HasKW() {
				isAssert = true
				msg = c.Pparams()[0]
			}
		}
		if isAssert {
			w.WriteString("assert ")
			PrintSrc(blk.Cond(), mod, w)
			if msg != nil {
				w.WriteString(", ")
				PrintSrc(msg, mod, w)
			}
			return true
		}
	}

	w.WriteString(blk.TypeStr())
	switch bt := blk.BlkType(); {
	case bt == ast.BLK_IF || bt == ast.BLK_ELIF || bt == ast.BLK_WHILE:
		neg := blk.Negative()
		if neg {
			w.WriteString(" not ")
		} else {
			w.WriteString(" ")
		}
		printTest(blk.Cond(), neg, false, mod, w)
	case bt == ast.BLK_FOR || bt == ast.BLK_ASYNCFOR:
		w.WriteString(" ")
		PrintSrc(blk.Index(), mod, w)
		w.WriteString(" in ")
		PrintSrc(blk.Iter(), mod, w)
	case bt == ast.BLK_EXCEPT && blk.Cond() != nil:
		w.WriteString(" ")
		PrintSrc(blk.Cond(), mod, w)
		if ev := blk.ExceptVar(); ev != nil {
			w.WriteString(" as ")
			PrintSrc(ev, mod, w)
		}
	case bt == ast.BLK_MATCH:
		w.WriteString(" ")
		PrintSrc(blk.Subject(), mod, w)
	case bt == ast.BLK_CASE:
		w.WriteString(" ")
		PrintSrc(blk.Pattern(), mod, w)
		if blk.Guard() != nil {
			w.WriteString(" if ")
			PrintSrc(blk.Guard(), mod, w)
		}
	case bt == ast.BLK_WITH:
		w.WriteString(" ")
		PrintSrc(blk.Expr(), mod, w)
		if v := blk.Var(); v != nil {
			w.WriteString(" as ")
			PrintSrc(v, mod, w)
		}
	}
	w.WriteString(":\n")
	curIndent++
	printBlock(blk, mod, w)
	curIndent--
	return true
}

func printImportNode(imp *ast.Import, mod *pyc.Module, w Out) {
	plainImportAs := len(imp.Stores()) == 1 &&
		(imp.Fromlist() == nil || (imp.Fromlist().Type() == ast.NODE_OBJECT && imp.Fromlist().(*ast.Object).Object() == pyc.None)) &&
		ast.TypeOf(imp.Name()) == ast.NODE_NAME
	if plainImportAs {
		w.WriteString("import ")
		PrintSrc(imp.Name(), mod, w)
		w.WriteString(" as ")
		PrintSrc(imp.Stores()[0].Dest(), mod, w)
	} else if len(imp.Stores()) > 0 {
		stores := imp.Stores()
		w.WriteString("from ")
		nested := ast.TypeOf(imp.Name()) == ast.NODE_IMPORT
		level := imp.Level()
		if nested {
			level = imp.Name().(*ast.Import).Level()
		}
		for i := 0; i < level; i++ {
			w.WriteString(".")
		}
		if nested {
			PrintSrc(imp.Name().(*ast.Import).Name(), mod, w)
		} else {
			PrintSrc(imp.Name(), mod, w)
		}
		w.WriteString(" import ")
		if len(stores) == 1 {
			src := stores[0].Src()
			dest := stores[0].Dest()
			PrintSrc(src, mod, w)
			if src.(*ast.Name).Name().Value != dest.(*ast.Name).Name().Value {
				w.WriteString(" as ")
				PrintSrc(dest, mod, w)
			}
		} else {
			for i, st := range stores {
				if i > 0 {
					w.WriteString(", ")
				}
				PrintSrc(st.Src(), mod, w)
				if st.Src().(*ast.Name).Name().Value != st.Dest().(*ast.Name).Name().Value {
					w.WriteString(" as ")
					PrintSrc(st.Dest(), mod, w)
				}
			}
		}
	} else {
		w.WriteString("import ")
		PrintSrc(imp.Name(), mod, w)
	}
}

func printLambdaFunc(fn *ast.Function, mod *pyc.Module, w Out) {
	bare := lambdaBare
	lambdaBare = false
	if bare {
		w.WriteString("lambda ")
	} else {
		w.WriteString("(lambda ")
	}
	code := fn.Code()
	codeSrc := code.(*ast.Object).Object().(*pyc.Code)
	defargs := fn.Defargs()
	kwdefargs := fn.Kwdefargs()
	da := 0
	narg := 0
	for i := 0; i < codeSrc.ArgCount; i++ {
		if narg != 0 {
			w.WriteString(", ")
		}
		w.WriteString(codeSrc.GetLocal(narg).Value)
		narg++
		if (codeSrc.ArgCount - i) <= len(defargs) {
			w.WriteString(" = ")
			PrintSrc(defargs[da], mod, w)
			da++
		}
	}
	da = 0
	if codeSrc.KwOnlyArgCount != 0 {
		if narg == 0 {
			w.WriteString("*")
		} else {
			w.WriteString(", *")
		}
		for i := 0; i < codeSrc.ArgCount; i++ {
			w.WriteString(", ")
			w.WriteString(codeSrc.GetLocal(narg).Value)
			narg++
			if (codeSrc.KwOnlyArgCount - i) <= len(kwdefargs) {
				w.WriteString(" = ")
				PrintSrc(kwdefargs[da], mod, w)
				da++
			}
		}
	}
	w.WriteString(": ")
	inLambda = true
	PrintSrc(code, mod, w)
	inLambda = false
	if !bare {
		w.WriteString(")")
	}
}

func printStore(st *ast.Store, mod *pyc.Module, w Out) {
	src := st.Src()
	dest := st.Dest()
	switch ast.TypeOf(src) {
	case ast.NODE_FUNCTION:
		fn := src.(*ast.Function)
		code := fn.Code()
		codeSrc := code.(*ast.Object).Object().(*pyc.Code)
		isLambda := false
		if codeSrc.Name.Value == "<lambda>" {
			w.WriteString("\n")
			startLine(curIndent, w)
			PrintSrc(dest, mod, w)
			w.WriteString(" = lambda ")
			isLambda = true
		} else {
			w.WriteString("\n")
			printPendingDecorators(mod, w)
			startLine(curIndent, w)
			if codeSrc.Flags&pyc.CoCoroutine != 0 {
				w.WriteString("async ")
			}
			w.WriteString("def ")
			PrintSrc(dest, mod, w)
			w.WriteString("(")
		}
		defargs := fn.Defargs()
		kwdefargs := fn.Kwdefargs()
		argC := codeSrc.ArgCount
		kwC := codeSrc.KwOnlyArgCount
		hasVarArgs := codeSrc.Flags&pyc.CoVarargs != 0
		hasVarKw := codeSrc.Flags&pyc.CoVarkeywords != 0
		firstParam := true
		da := 0
		for i := 0; i < argC; i++ {
			if !firstParam {
				w.WriteString(", ")
			}
			firstParam = false
			w.WriteString(codeSrc.GetLocal(i).Value)
			if (argC - i) <= len(defargs) {
				w.WriteString(" = ")
				PrintSrc(defargs[da], mod, w)
				da++
			}
			if codeSrc.PosOnlyArgCount > 0 && i == codeSrc.PosOnlyArgCount-1 {
				w.WriteString(", /")
			}
		}
		if hasVarArgs {
			if !firstParam {
				w.WriteString(", ")
			}
			firstParam = false
			w.WriteString("*" + codeSrc.GetLocal(argC+kwC).Value)
		} else if kwC != 0 {
			if !firstParam {
				w.WriteString(", ")
			}
			firstParam = false
			w.WriteString("*")
		}
		kda := 0
		for i := 0; i < kwC; i++ {
			if !firstParam {
				w.WriteString(", ")
			}
			firstParam = false
			w.WriteString(codeSrc.GetLocal(argC + i).Value)
			if (kwC - i) <= len(kwdefargs) {
				w.WriteString(" = ")
				PrintSrc(kwdefargs[kda], mod, w)
				kda++
			}
		}
		if hasVarKw {
			if !firstParam {
				w.WriteString(", ")
			}
			firstParam = false
			idx := argC + kwC
			if hasVarArgs {
				idx++
			}
			w.WriteString("**" + codeSrc.GetLocal(idx).Value)
		}
		if isLambda {
			w.WriteString(": ")
		} else {
			w.WriteString("):\n")
			printDocstringAndGlobals = true
		}
		preLambda := inLambda
		inLambda = inLambda || isLambda
		PrintSrc(code, mod, w)
		inLambda = preLambda
	case ast.NODE_CLASS:
		w.WriteString("\n")
		printPendingDecorators(mod, w)
		startLine(curIndent, w)
		w.WriteString("class ")
		PrintSrc(dest, mod, w)
		cls := src.(*ast.Class)
		bases := cls.Bases().(*ast.Tuple)
		var classKw []ast.KV
		if cc := cls.Code(); cc != nil && cc.Type() == ast.NODE_CALL {
			classKw = cc.(*ast.Call).Kwparams()
		}
		if len(bases.Values()) > 0 || len(classKw) != 0 {
			w.WriteString("(")
			first := true
			for _, val := range bases.Values() {
				if !first {
					w.WriteString(", ")
				}
				PrintSrc(val, mod, w)
				first = false
			}
			for _, kv := range classKw {
				if !first {
					w.WriteString(", ")
				}
				if ast.TypeOf(kv.K) == ast.NODE_NAME {
					w.WriteString(kv.K.(*ast.Name).Name().Value + " = ")
				} else {
					w.WriteString(kv.K.(*ast.Object).Object().(*pyc.String).Value + " = ")
				}
				PrintSrc(kv.V, mod, w)
				first = false
			}
			w.WriteString("):\n")
		} else {
			w.WriteString(":\n")
		}
		printClassDocstring = true
		code := cls.Code().(*ast.Call).Func().(*ast.Function).Code()
		PrintSrc(code, mod, w)
	case ast.NODE_IMPORT:
		imp := src.(*ast.Import)
		if imp.Fromlist() != nil {
			fromlist := imp.Fromlist().(*ast.Object).Object()
			if fromlist != pyc.None {
				w.WriteString("from ")
				nested := ast.TypeOf(imp.Name()) == ast.NODE_IMPORT
				level := imp.Level()
				if nested {
					level = imp.Name().(*ast.Import).Level()
				}
				for i := 0; i < level; i++ {
					w.WriteString(".")
				}
				if nested {
					PrintSrc(imp.Name().(*ast.Import).Name(), mod, w)
				} else {
					PrintSrc(imp.Name(), mod, w)
				}
				w.WriteString(" import ")
				if tupleLike(fromlist) {
					for i, val := range fromlist.(*pyc.Seq).Values() {
						if i > 0 {
							w.WriteString(", ")
						}
						w.WriteString(val.(*pyc.String).Value)
					}
				} else {
					w.WriteString(fromlist.(*pyc.String).Value)
				}
			} else {
				w.WriteString("import ")
				PrintSrc(imp.Name(), mod, w)
			}
		} else {
			w.WriteString("import ")
			importName := imp.Name()
			PrintSrc(importName, mod, w)
			full := importName.(*ast.Name).Name().Value
			firstPart := full
			if i := strings.IndexByte(full, '.'); i >= 0 {
				firstPart = full[:i]
			}
			if dest.(*ast.Name).Name().Value != importName.(*ast.Name).Name().Value &&
				dest.(*ast.Name).Name().Value != firstPart {
				w.WriteString(" as ")
				PrintSrc(dest, mod, w)
			}
		}
	default:
		if ast.TypeOf(src) == ast.NODE_BINARY && src.(*ast.Binary).IsInplace() {
			PrintSrc(src, mod, w)
			return
		}
		PrintSrc(dest, mod, w)
		w.WriteString(" = ")
		if dest != nil && dest.Type() == ast.NODE_TUPLE && src != nil && src.Type() == ast.NODE_OBJECT &&
			tupleLike(src.(*ast.Object).Object()) && len(src.(*ast.Object).Object().(*pyc.Seq).Values()) != 0 {
			tmp := &bytes.Buffer{}
			PrintSrc(src, mod, tmp)
			w.WriteString(stripParens(tmp.String()))
		} else {
			PrintSrc(src, mod, w)
		}
	}
}

// PrintDocstring prints a docstring constant; false if obj is not a string.
func PrintDocstring(obj pyc.Object, indent int, mod *pyc.Module, w Out) bool {
	if doc, ok := obj.(*pyc.String); ok && doc != nil {
		startLine(indent, w)
		doc.Print(w, mod, true, "")
		w.WriteString("\n")
		return true
	}
	return false
}

var _ = strconv.Itoa
