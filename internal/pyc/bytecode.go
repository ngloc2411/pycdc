package pyc

import (
	"fmt"
	"io"
	"math"
	"strings"

	op "github.com/ngloc2411/pycdc/internal/opcode"
)

// Disassembly flags.
const (
	DisasmPycodeVerbose = 0x1
	DisasmShowCaches    = 0x2
)

// PrintConst writes a constant the way pycdas shows it.
func PrintConst(w io.Writer, obj Object, m *Module, parentFStringQuote string) {
	if obj == nil {
		io.WriteString(w, "<NULL>")
		return
	}
	list := func(vals []Object) {
		for i, v := range vals {
			if i > 0 {
				io.WriteString(w, ", ")
			}
			PrintConst(w, v, m, "")
		}
	}
	switch o := obj.(type) {
	case *String:
		o.Print(w, m, false, parentFStringQuote)
	case *Seq:
		switch o.typ {
		case TypeTuple, TypeSmallTuple:
			io.WriteString(w, "(")
			list(o.values)
			if len(o.values) == 1 {
				io.WriteString(w, ",)")
			} else {
				io.WriteString(w, ")")
			}
		case TypeList:
			io.WriteString(w, "[")
			list(o.values)
			io.WriteString(w, "]")
		case TypeSet:
			io.WriteString(w, "{")
			list(o.values)
			io.WriteString(w, "}")
		case TypeFrozenSet:
			io.WriteString(w, "frozenset({")
			list(o.values)
			io.WriteString(w, "})")
		}
	case *Dict:
		io.WriteString(w, "{")
		for i, it := range o.Items {
			if i > 0 {
				io.WriteString(w, ", ")
			}
			PrintConst(w, it.Key, m, "")
			io.WriteString(w, ": ")
			PrintConst(w, it.Val, m, "")
		}
		io.WriteString(w, "}")
	case *Slice:
		nz := func(x Object) Object {
			if x == nil {
				return None
			}
			return x
		}
		io.WriteString(w, "slice(")
		PrintConst(w, nz(o.Start), m, "")
		io.WriteString(w, ", ")
		PrintConst(w, nz(o.Stop), m, "")
		io.WriteString(w, ", ")
		PrintConst(w, nz(o.Step), m, "")
		io.WriteString(w, ")")
	case *Simple:
		switch o.typ {
		case TypeNone:
			io.WriteString(w, "None")
		case TypeTrue:
			io.WriteString(w, "True")
		case TypeFalse:
			io.WriteString(w, "False")
		case TypeEllipsis:
			io.WriteString(w, "...")
		default:
			fmt.Fprintf(w, "<TYPE: %d>\n", o.typ)
		}
	case *Int:
		fmt.Fprintf(w, "%d", o.Value)
	case *Long:
		io.WriteString(w, o.Repr(m))
	case *Float:
		if o.complex {
			fmt.Fprintf(w, "(%s+%sj)", o.Value, o.Imag)
		} else {
			io.WriteString(w, o.Value)
		}
	case *CFloat:
		if o.complex {
			fmt.Fprintf(w, "(%s+%sj)", FormatG(o.Value), FormatG(o.Imag))
			return
		}
		v := o.Value
		switch {
		case math.IsNaN(v):
			if math.Signbit(v) {
				io.WriteString(w, "float('-nan')")
			} else {
				io.WriteString(w, "float('nan')")
			}
		case math.IsInf(v, 0):
			if math.Signbit(v) {
				io.WriteString(w, "float('-inf')")
			} else {
				io.WriteString(w, "float('inf')")
			}
		default:
			io.WriteString(w, FormatG(v))
		}
	case *Code:
		io.WriteString(w, "<CODE> "+o.Name.Value)
	default:
		fmt.Fprintf(w, "<TYPE: %d>\n", obj.Type())
	}
}

// FormatG formats like C's "%g".
func FormatG(v float64) string {
	switch {
	case math.IsNaN(v):
		if math.Signbit(v) {
			return "-nan"
		}
		return "nan"
	case math.IsInf(v, 1):
		return "inf"
	case math.IsInf(v, -1):
		return "-inf"
	}
	return fmt.Sprintf("%.6g", v)
}

// BcNext decodes the next instruction of source, advancing *pos.
func BcNext(source *Buffer, m *Module, pos *int) (opcode, operand int) {
	opcode = op.ByteToOpcode(m.maj, m.min, source.Byte())
	if m.VerCompare(3, 6) >= 0 {
		operand = source.Byte()
		*pos += 2
		if opcode == op.EXTENDED_ARG_A {
			opcode = op.ByteToOpcode(m.maj, m.min, source.Byte())
			operand = operand<<8 | source.Byte()
			*pos += 2
		}
		return
	}
	operand = 0
	*pos++
	if opcode == op.EXTENDED_ARG_A {
		operand = source.Get16() << 16
		opcode = op.ByteToOpcode(m.maj, m.min, source.Byte())
		*pos += 3
	}
	if opcode >= op.PYC_HAVE_ARG {
		operand |= source.Get16()
		*pos += 2
	}
	return
}

var (
	cmpStrings   = []string{"<", "<=", "==", "!=", ">", ">=", "in", "not in", "is", "is not", "<EXCEPTION MATCH>", "<BAD>"}
	binopStrings = []string{"+", "&", "//", "<<", "@", "*", "%", "|", "**", ">>", "-", "/", "^",
		"+=", "&=", "//=", "<<=", "@=", "*=", "%=", "|=", "**=", ">>=", "-=", "/=", "^="}
	intrinsic1Names = []string{"INTRINSIC_1_INVALID", "INTRINSIC_PRINT", "INTRINSIC_IMPORT_STAR",
		"INTRINSIC_STOPITERATION_ERROR", "INTRINSIC_ASYNC_GEN_WRAP", "INTRINSIC_UNARY_POSITIVE",
		"INTRINSIC_LIST_TO_TUPLE", "INTRINSIC_TYPEVAR", "INTRINSIC_PARAMSPEC", "INTRINSIC_TYPEVARTUPLE",
		"INTRINSIC_SUBSCRIPT_GENERIC", "INTRINSIC_TYPEALIAS"}
	intrinsic2Names = []string{"INTRINSIC_2_INVALID", "INTRINSIC_PREP_RERAISE_STAR",
		"INTRINSIC_TYPEVAR_WITH_BOUND", "INTRINSIC_TYPEVAR_WITH_CONSTRAINTS",
		"INTRINSIC_SET_FUNCTION_TYPE_PARAMS", "INTRINSIC_SET_TYPEPARAM_DEFAULT"}
	formatValueNames = []string{"FVC_NONE", "FVC_STR", "FVC_REPR", "FVC_ASCII"}
)

// Disasm writes the disassembly of code.
func Disasm(w io.Writer, code *Code, m *Module, indent int, flags uint) {
	src := NewBuffer([]byte(code.Code.Value))
	pos := 0
	pad := strings.Repeat("    ", indent)
	invalid := func(operand int) { fmt.Fprintf(w, "%d <INVALID>", operand) }
	name := func(i int) (*String, bool) {
		if i < 0 || i >= code.Names.Size() {
			return nil, false
		}
		s, ok := code.Names.Get(i).(*String)
		return s, ok
	}
	local := func(i int) (*String, bool) {
		if i < 0 || i >= code.LocalNames.Size() {
			return nil, false
		}
		s, ok := code.LocalNames.Get(i).(*String)
		return s, ok
	}
	for !src.AtEof() {
		start := pos
		opcode, operand := BcNext(src, m, &pos)
		if opcode == op.CACHE && flags&DisasmShowCaches == 0 {
			continue
		}
		io.WriteString(w, pad)
		fmt.Fprintf(w, "%-7d %-30s  ", start, op.Name(opcode))
		if opcode >= op.PYC_HAVE_ARG {
			switch opcode {
			case op.LOAD_CONST_A, op.RESERVE_FAST_A, op.KW_NAMES_A, op.RETURN_CONST_A, op.INSTRUMENTED_RETURN_CONST_A:
				if operand < 0 || operand >= code.Consts.Size() {
					invalid(operand)
				} else {
					fmt.Fprintf(w, "%d: ", operand)
					PrintConst(w, code.GetConst(operand), m, "")
				}
			case op.LOAD_GLOBAL_A:
				if m.VerCompare(3, 11) >= 0 {
					n, ok := name(operand >> 1)
					switch {
					case !ok:
						invalid(operand)
					case operand&1 != 0:
						fmt.Fprintf(w, "%d: NULL + %s", operand, n.Value)
					default:
						fmt.Fprintf(w, "%d: %s", operand, n.Value)
					}
				} else if n, ok := name(operand); ok {
					fmt.Fprintf(w, "%d: %s", operand, n.Value)
				} else {
					invalid(operand)
				}
			case op.DELETE_ATTR_A, op.DELETE_GLOBAL_A, op.DELETE_NAME_A, op.IMPORT_FROM_A, op.IMPORT_NAME_A,
				op.LOAD_ATTR_A, op.LOAD_LOCAL_A, op.LOAD_NAME_A, op.STORE_ATTR_A, op.STORE_GLOBAL_A,
				op.STORE_NAME_A, op.STORE_ANNOTATION_A, op.LOAD_METHOD_A, op.LOAD_FROM_DICT_OR_GLOBALS_A:
				arg := operand
				if opcode == op.LOAD_ATTR_A && m.VerCompare(3, 12) >= 0 {
					arg >>= 1
				}
				if n, ok := name(arg); ok {
					fmt.Fprintf(w, "%d: %s", operand, n.Value)
				} else {
					invalid(operand)
				}
			case op.LOAD_SUPER_ATTR_A, op.INSTRUMENTED_LOAD_SUPER_ATTR_A:
				if n, ok := name(operand >> 2); ok {
					fmt.Fprintf(w, "%d: %s", operand, n.Value)
				} else {
					invalid(operand)
				}
			case op.DELETE_FAST_A, op.LOAD_FAST_A, op.STORE_FAST_A, op.LOAD_FAST_CHECK_A, op.LOAD_FAST_AND_CLEAR_A:
				if n, ok := local(operand); ok {
					fmt.Fprintf(w, "%d: %s", operand, n.Value)
				} else {
					invalid(operand)
				}
			case op.LOAD_FAST_LOAD_FAST_A, op.STORE_FAST_LOAD_FAST_A, op.STORE_FAST_STORE_FAST_A:
				a, ok1 := local(operand >> 4)
				b, ok2 := local(operand & 0xF)
				if ok1 && ok2 {
					fmt.Fprintf(w, "%d: %s, %s", operand, a.Value, b.Value)
				} else {
					invalid(operand)
				}
			case op.LOAD_CLOSURE_A, op.LOAD_DEREF_A, op.STORE_DEREF_A, op.DELETE_DEREF_A, op.MAKE_CELL_A,
				op.CALL_FINALLY_A, op.LOAD_FROM_DICT_OR_DEREF_A:
				if s, ok := safeCellVar(code, m, operand); ok {
					fmt.Fprintf(w, "%d: %s", operand, s.Value)
				} else {
					invalid(operand)
				}
			case op.JUMP_FORWARD_A, op.JUMP_IF_FALSE_A, op.JUMP_IF_TRUE_A, op.SETUP_LOOP_A, op.SETUP_FINALLY_A,
				op.SETUP_EXCEPT_A, op.FOR_LOOP_A, op.FOR_ITER_A, op.SETUP_WITH_A, op.SETUP_ASYNC_WITH_A,
				op.POP_JUMP_FORWARD_IF_FALSE_A, op.POP_JUMP_FORWARD_IF_TRUE_A, op.SEND_A,
				op.POP_JUMP_FORWARD_IF_NOT_NONE_A, op.POP_JUMP_FORWARD_IF_NONE_A, op.POP_JUMP_IF_NOT_NONE_A,
				op.POP_JUMP_IF_NONE_A, op.INSTRUMENTED_POP_JUMP_IF_NOT_NONE_A, op.INSTRUMENTED_POP_JUMP_IF_NONE_A,
				op.INSTRUMENTED_JUMP_FORWARD_A, op.INSTRUMENTED_FOR_ITER_A, op.INSTRUMENTED_POP_JUMP_IF_FALSE_A,
				op.INSTRUMENTED_POP_JUMP_IF_TRUE_A:
				offs := operand
				if m.VerCompare(3, 10) >= 0 {
					offs *= 2
				}
				fmt.Fprintf(w, "%d (to %d)", operand, pos+offs)
			case op.JUMP_BACKWARD_NO_INTERRUPT_A, op.JUMP_BACKWARD_A, op.POP_JUMP_BACKWARD_IF_NOT_NONE_A,
				op.POP_JUMP_BACKWARD_IF_NONE_A, op.POP_JUMP_BACKWARD_IF_FALSE_A, op.POP_JUMP_BACKWARD_IF_TRUE_A,
				op.INSTRUMENTED_JUMP_BACKWARD_A:
				fmt.Fprintf(w, "%d (to %d)", operand, pos-operand*2)
			case op.POP_JUMP_IF_FALSE_A, op.POP_JUMP_IF_TRUE_A, op.JUMP_IF_FALSE_OR_POP_A,
				op.JUMP_IF_TRUE_OR_POP_A, op.JUMP_ABSOLUTE_A, op.JUMP_IF_NOT_EXC_MATCH_A:
				if m.VerCompare(3, 12) >= 0 {
					fmt.Fprintf(w, "%d (to %d)", operand, pos+operand*2)
				} else if m.VerCompare(3, 10) >= 0 {
					fmt.Fprintf(w, "%d (to %d)", operand, operand*2)
				} else {
					fmt.Fprintf(w, "%d", operand)
				}
			case op.COMPARE_OP_A:
				arg := operand
				if m.VerCompare(3, 12) == 0 {
					arg >>= 4
				} else if m.VerCompare(3, 13) >= 0 {
					arg >>= 5
				}
				if arg >= 0 && arg < len(cmpStrings) {
					fmt.Fprintf(w, "%d (%s)", operand, cmpStrings[arg])
				} else {
					fmt.Fprintf(w, "%d (UNKNOWN)", operand)
				}
			case op.BINARY_OP_A:
				pick(w, operand, binopStrings)
			case op.IS_OP_A:
				switch operand {
				case 0:
					fmt.Fprintf(w, "%d (is)", operand)
				case 1:
					fmt.Fprintf(w, "%d (is not)", operand)
				default:
					fmt.Fprintf(w, "%d (UNKNOWN)", operand)
				}
			case op.CONTAINS_OP_A:
				switch operand {
				case 0:
					fmt.Fprintf(w, "%d (in)", operand)
				case 1:
					fmt.Fprintf(w, "%d (not in)", operand)
				default:
					fmt.Fprintf(w, "%d (UNKNOWN)", operand)
				}
			case op.CALL_INTRINSIC_1_A:
				pick(w, operand, intrinsic1Names)
			case op.CALL_INTRINSIC_2_A:
				pick(w, operand, intrinsic2Names)
			case op.FORMAT_VALUE_A:
				conv := operand & 3
				flag := ""
				if operand&4 != 0 {
					flag = " | FVS_HAVE_SPEC"
				}
				fmt.Fprintf(w, "%d (%s%s)", operand, formatValueNames[conv], flag)
			case op.CONVERT_VALUE_A:
				pick(w, operand, formatValueNames)
			case op.SET_FUNCTION_ATTRIBUTE_A:
				switch operand {
				case 1:
					fmt.Fprintf(w, "%d (MAKE_FUNCTION_DEFAULTS)", operand)
				case 2:
					fmt.Fprintf(w, "%d (MAKE_FUNCTION_KWDEFAULTS)", operand)
				case 4:
					fmt.Fprintf(w, "%d (MAKE_FUNCTION_ANNOTATIONS)", operand)
				case 8:
					fmt.Fprintf(w, "%d (MAKE_FUNCTION_CLOSURE)", operand)
				default:
					fmt.Fprintf(w, "%d (UNKNOWN)", operand)
				}
			default:
				fmt.Fprintf(w, "%d", operand)
			}
		}
		io.WriteString(w, "\n")
	}
}

func pick(w io.Writer, operand int, table []string) {
	if operand >= 0 && operand < len(table) {
		fmt.Fprintf(w, "%d (%s)", operand, table[operand])
	} else {
		fmt.Fprintf(w, "%d (UNKNOWN)", operand)
	}
}

func safeCellVar(code *Code, m *Module, idx int) (s *String, ok bool) {
	defer func() {
		if recover() != nil {
			s, ok = nil, false
		}
	}()
	if idx < 0 {
		return nil, false
	}
	return code.GetCellVar(m, idx), true
}

// ExceptionTable writes the 3.11+ exception table.
func ExceptionTable(w io.Writer, code *Code, indent int) {
	for _, e := range code.ExceptionTableEntries() {
		io.WriteString(w, strings.Repeat("    ", indent))
		lasti := ""
		if e.PushLasti {
			lasti = "lasti"
		}
		fmt.Fprintf(w, "%d to %d -> %d [%d] %s\n", e.StartOffset, e.EndOffset, e.Target, e.StackDepth, lasti)
	}
}
