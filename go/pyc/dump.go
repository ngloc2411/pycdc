package pyc

import (
	"fmt"
	"io"
	"strings"
)

var flagNames = []string{
	"CO_OPTIMIZED", "CO_NEWLOCALS", "CO_VARARGS", "CO_VARKEYWORDS",
	"CO_NESTED", "CO_GENERATOR", "CO_NOFREE", "CO_COROUTINE",
	"CO_ITERABLE_COROUTINE", "CO_ASYNC_GENERATOR", "<0x400>", "<0x800>",
	"CO_GENERATOR_ALLOWED", "<0x2000>", "<0x4000>", "<0x8000>",
	"<0x10000>", "CO_FUTURE_DIVISION", "CO_FUTURE_ABSOLUTE_IMPORT", "CO_FUTURE_WITH_STATEMENT",
	"CO_FUTURE_PRINT_FUNCTION", "CO_FUTURE_UNICODE_LITERALS", "CO_FUTURE_BARRY_AS_BDFL",
	"CO_FUTURE_GENERATOR_STOP",
	"CO_FUTURE_ANNOTATIONS", "CO_NO_MONITORING_EVENTS", "<0x4000000>", "<0x8000000>",
	"<0x10000000>", "<0x20000000>", "<0x40000000>", "<0x80000000>",
}

func printCoFlags(flags uint32, w io.Writer) {
	if flags == 0 {
		io.WriteString(w, "\n")
		return
	}
	io.WriteString(w, " (")
	for k := 0; k < 32; k++ {
		f := uint32(1) << uint(k)
		if flags&f != 0 {
			flags &^= f
			if flags == 0 {
				io.WriteString(w, flagNames[k])
			} else {
				io.WriteString(w, flagNames[k]+" | ")
			}
		}
	}
	io.WriteString(w, ")\n")
}

// DumpObject writes the pycdas style dump of obj.
func DumpObject(obj Object, m *Module, indent int, flags uint, w io.Writer) {
	seen := map[Object]bool{}
	dumpObject(obj, m, indent, flags, w, seen)
}

func dumpObject(obj Object, m *Module, indent int, flags uint, w io.Writer, seen map[Object]bool) {
	pad := func(n int) string { return strings.Repeat("    ", n) }
	puts := func(n int, s string) { io.WriteString(w, pad(n)+s) }
	if obj == nil {
		puts(indent, "<NULL>")
		return
	}
	if seen[obj] {
		fmt.Fprintln(errOut, "WARNING: Circular reference detected")
		return
	}
	seen[obj] = true
	defer delete(seen, obj)
	sub := func(o Object, n int) { dumpObject(o, m, n, flags, w, seen) }
	switch o := obj.(type) {
	case *Code:
		puts(indent, "[Code]\n")
		puts(indent+1, fmt.Sprintf("File Name: %s\n", o.FileName.Value))
		puts(indent+1, fmt.Sprintf("Object Name: %s\n", o.Name.Value))
		if m.VerCompare(3, 11) >= 0 {
			puts(indent+1, fmt.Sprintf("Qualified Name: %s\n", o.QualName.Value))
		}
		puts(indent+1, fmt.Sprintf("Arg Count: %d\n", o.ArgCount))
		if m.VerCompare(3, 8) >= 0 {
			puts(indent+1, fmt.Sprintf("Pos Only Arg Count: %d\n", o.PosOnlyArgCount))
		}
		if m.MajorVer() >= 3 {
			puts(indent+1, fmt.Sprintf("KW Only Arg Count: %d\n", o.KwOnlyArgCount))
		}
		if m.VerCompare(3, 11) < 0 {
			puts(indent+1, fmt.Sprintf("Locals: %d\n", o.NumLocals))
		}
		if m.VerCompare(1, 5) >= 0 {
			puts(indent+1, fmt.Sprintf("Stack Size: %d\n", o.StackSize))
		}
		if m.VerCompare(1, 3) >= 0 {
			orig := uint32(o.Flags)
			if m.VerCompare(3, 8) < 0 {
				orig = (orig & 0xFFFF) | ((orig & 0xFFF00000) >> 4)
			}
			puts(indent+1, fmt.Sprintf("Flags: 0x%08X", orig))
			printCoFlags(uint32(o.Flags), w)
		}
		puts(indent+1, "[Names]\n")
		for _, v := range o.Names.Values() {
			sub(v, indent+2)
		}
		if m.VerCompare(1, 3) >= 0 {
			if m.VerCompare(3, 11) >= 0 {
				puts(indent+1, "[Locals+Names]\n")
			} else {
				puts(indent+1, "[Var Names]\n")
			}
			for _, v := range o.LocalNames.Values() {
				sub(v, indent+2)
			}
		}
		if m.VerCompare(3, 11) >= 0 && flags&DisasmPycodeVerbose != 0 {
			puts(indent+1, "[Locals+Kinds]\n")
			sub(o.LocalKinds, indent+2)
		}
		if m.VerCompare(2, 1) >= 0 && m.VerCompare(3, 11) < 0 {
			puts(indent+1, "[Free Vars]\n")
			for _, v := range o.FreeVars.Values() {
				sub(v, indent+2)
			}
			puts(indent+1, "[Cell Vars]\n")
			for _, v := range o.CellVars.Values() {
				sub(v, indent+2)
			}
		}
		puts(indent+1, "[Constants]\n")
		for _, v := range o.Consts.Values() {
			sub(v, indent+2)
		}
		puts(indent+1, "[Disassembly]\n")
		Disasm(w, o, m, indent+2, flags)
		if m.VerCompare(3, 11) >= 0 {
			puts(indent+1, "[Exception Table]\n")
			ExceptionTable(w, o, indent+2)
		}
		if m.VerCompare(1, 5) >= 0 && flags&DisasmPycodeVerbose != 0 {
			puts(indent+1, fmt.Sprintf("First Line: %d\n", o.FirstLine))
			puts(indent+1, "[Line Number Table]\n")
			sub(o.LnTable, indent+2)
		}
	case *String:
		puts(indent, "")
		o.Print(w, m, false, "")
		io.WriteString(w, "\n")
	case *Seq:
		open, closeS := "(", ")"
		switch o.typ {
		case TypeList:
			open, closeS = "[", "]"
		case TypeSet:
			open, closeS = "{", "}"
		case TypeFrozenSet:
			open, closeS = "frozenset({", "})"
		}
		puts(indent, open+"\n")
		for _, v := range o.values {
			sub(v, indent+1)
		}
		puts(indent, closeS+"\n")
	case *Dict:
		puts(indent, "{\n")
		for _, it := range o.Items {
			sub(it.Key, indent+1)
			sub(it.Val, indent+2)
		}
		puts(indent, "}\n")
	case *Simple:
		switch o.typ {
		case TypeNone:
			puts(indent, "None\n")
		case TypeFalse:
			puts(indent, "False\n")
		case TypeTrue:
			puts(indent, "True\n")
		case TypeEllipsis:
			puts(indent, "...\n")
		default:
			puts(indent, fmt.Sprintf("<TYPE: %d>\n", o.typ))
		}
	case *Int:
		puts(indent, fmt.Sprintf("%d\n", o.Value))
	case *Long:
		puts(indent, o.Repr(m)+"\n")
	case *Float:
		if o.complex {
			puts(indent, fmt.Sprintf("(%s+%sj)\n", o.Value, o.Imag))
		} else {
			puts(indent, o.Value+"\n")
		}
	case *CFloat:
		if o.complex {
			puts(indent, fmt.Sprintf("(%s+%sj)\n", FormatG(o.Value), FormatG(o.Imag)))
		} else {
			puts(indent, FormatG(o.Value)+"\n")
		}
	default:
		puts(indent, fmt.Sprintf("<TYPE: %d>\n", obj.Type()))
	}
}
