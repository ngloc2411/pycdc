package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/decomp/ast"
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

func (bld *builder) opsExpr() flow {
	switch bld.opcode {
	case BINARY_OP_A:
		if bld.operand == 26 && bld.mod.VerCompare(3, 14) >= 0 {
			subscr := bld.stack.Top(1)
			bld.stack.Pop()
			src := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewSubscr(src, subscr))
			break
		}
		{
			op := binFromBinaryOp(bld.operand)
			if op == ast.BIN_INVALID {
				fmt.Fprintf(os.Stderr, "Unsupported `BINARY_OP` operand value: %d\n", bld.operand)
			}
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewBinary(left, right, op))
		}
	case BINARY_ADD, BINARY_AND, BINARY_DIVIDE, BINARY_FLOOR_DIVIDE, BINARY_LSHIFT, BINARY_MODULO,
		BINARY_MULTIPLY, BINARY_OR, BINARY_POWER, BINARY_RSHIFT, BINARY_SUBTRACT, BINARY_TRUE_DIVIDE,
		BINARY_XOR, BINARY_MATRIX_MULTIPLY, INPLACE_ADD, INPLACE_AND, INPLACE_DIVIDE,
		INPLACE_FLOOR_DIVIDE, INPLACE_LSHIFT, INPLACE_MODULO, INPLACE_MULTIPLY, INPLACE_OR,
		INPLACE_POWER, INPLACE_RSHIFT, INPLACE_SUBTRACT, INPLACE_TRUE_DIVIDE, INPLACE_XOR,
		INPLACE_MATRIX_MULTIPLY:
		{
			op := binFromOpcode(bld.opcode)
			if op == ast.BIN_INVALID {
				panic("Unhandled opcode from ast.Binary::from_opcode")
			}
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewBinary(left, right, op))
		}
	case BINARY_SUBSCR:
		{
			subscr := bld.stack.Top(1)
			bld.stack.Pop()
			src := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewSubscr(src, subscr))
		}
	case BREAK_LOOP:
		bld.curblock.Append(ast.NewKeyword(ast.KW_BREAK))
	case BUILD_CLASS:
		{
			classCode := bld.stack.Top(1)
			bld.stack.Pop()
			bases := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewClass(classCode, bases, name))
		}
	case BUILD_FUNCTION:
		{
			funCode := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewFunction(funCode, nil, nil))
		}
	case BUILD_LIST_A:
		{
			var values []ast.Node
			for i := 0; i < bld.operand; i++ {
				values = pushFront(values, bld.stack.Top(1))
				bld.stack.Pop()
			}
			bld.stack.Push(ast.NewList(values))
		}
	case BUILD_SET_A:
		{
			var values []ast.Node
			for i := 0; i < bld.operand; i++ {
				values = pushFront(values, bld.stack.Top(1))
				bld.stack.Pop()
			}
			bld.stack.Push(ast.NewSet(values))
		}
	case BUILD_MAP_A:
		if bld.mod.VerCompare(3, 5) >= 0 {
			m := ast.NewMap()
			for i := 0; i < bld.operand; i++ {
				value := bld.stack.Top(1)
				bld.stack.Pop()
				key := bld.stack.Top(1)
				bld.stack.Pop()
				m.Add(key, value)
			}
			bld.stack.Push(m)
		} else {
			if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				bld.stack.Pop()
			}
			bld.stack.Push(ast.NewMap())
		}
	case BUILD_CONST_KEY_MAP_A:
		{
			keys := bld.stack.Top(1)
			bld.stack.Pop()
			values := make([]ast.Node, 0, bld.operand)
			for i := 0; i < bld.operand; i++ {
				value := bld.stack.Top(1)
				bld.stack.Pop()
				values = append(values, value)
			}
			bld.stack.Push(ast.NewConstMap(keys, values))
		}
	case STORE_MAP:
		{
			key := bld.stack.Top(1)
			bld.stack.Pop()
			value := bld.stack.Top(1)
			bld.stack.Pop()
			m := bld.stack.Top(1).(*ast.Map)
			m.Add(key, value)
		}
	case BUILD_SLICE_A:
		{
			isNone := func(n ast.Node) bool {
				return ast.TypeOf(n) == ast.NODE_OBJECT && n.(*ast.Object).Object() == pyc.None
			}
			mk := func(start, end ast.Node) ast.Node {
				if start == nil && end == nil {
					return ast.NewSlice(ast.SLICE0, nil, nil)
				} else if start == nil {
					return ast.NewSlice(ast.SLICE2, start, end)
				} else if end == nil {
					return ast.NewSlice(ast.SLICE1, start, end)
				}
				return ast.NewSlice(ast.SLICE3, start, end)
			}
			if bld.operand == 2 {
				end := bld.stack.Top(1)
				bld.stack.Pop()
				start := bld.stack.Top(1)
				bld.stack.Pop()
				if isNone(start) {
					start = nil
				}
				if isNone(end) {
					end = nil
				}
				bld.stack.Push(mk(start, end))
			} else if bld.operand == 3 {
				step := bld.stack.Top(1)
				bld.stack.Pop()
				end := bld.stack.Top(1)
				bld.stack.Pop()
				start := bld.stack.Top(1)
				bld.stack.Pop()
				if isNone(start) {
					start = nil
				}
				if isNone(end) {
					end = nil
				}
				if isNone(step) {
					step = nil
				}
				bld.stack.Push(mk(start, end))
				lhs := bld.stack.Top(1)
				bld.stack.Pop()
				if step == nil {
					bld.stack.Push(ast.NewSlice(ast.SLICE1, lhs, step))
				} else {
					bld.stack.Push(ast.NewSlice(ast.SLICE3, lhs, step))
				}
			}
		}
	case BUILD_STRING_A:
		{
			var values []ast.Node
			for i := 0; i < bld.operand; i++ {
				values = pushFront(values, bld.stack.Top(1))
				bld.stack.Pop()
			}
			bld.stack.Push(ast.NewJoinedStr(values))
		}
	case BUILD_TUPLE_A:
		{
			tos := bld.stack.Top(1)
			if tos != nil && tos.Type() == ast.NODE_LOADBUILDCLASS {
				break
			}
			values := make([]ast.Node, bld.operand)
			for i := 0; i < bld.operand; i++ {
				values[bld.operand-i-1] = bld.stack.Top(1)
				bld.stack.Pop()
			}
			bld.stack.Push(ast.NewTuple(values))
		}
	case KW_NAMES_A:
		{
			kwConst := bld.code.GetConst(bld.operand).(*pyc.Seq)
			kwparams := kwConst.Size()
			kwparamList := ast.NewKwNamesMap()
			keys := kwConst.Values()
			for i := 0; i < kwparams; i++ {
				kwparamList.Add(ast.NewObject(keys[kwparams-i-1]), bld.stack.Top(1))
				bld.stack.Pop()
			}
			bld.stack.Push(kwparamList)
		}
	case CALL_A, CALL_FUNCTION_A, INSTRUMENTED_CALL_A:
		{
			kwparams := (bld.operand & 0xFF00) >> 8
			pparams := bld.operand & 0xFF
			var kwparamList []ast.KV
			var pparamList []ast.Node

			bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			var classKwargs []ast.KV
			if bld.mod.VerCompare(3, 11) >= 0 && bld.stack.Top(1) != nil && bld.stack.Top(1).Type() == ast.NODE_KW_NAMES_MAP {
				km := bld.stack.Top(1).(*ast.KwNamesMap)
				bld.stack.Pop()
				classKwargs = append(classKwargs, km.Values()...)
			}
			basecnt := 0
			var bases []ast.Node
			TOS := bld.stack.Top(1)
			TOSType := ast.TypeOf(TOS)
			for TOSType == ast.NODE_NAME || TOSType == ast.NODE_BINARY {
				bases = append(bases, TOS)
				basecnt++
				bld.stack.Pop()
				TOS = bld.stack.Top(1)
				TOSType = ast.TypeOf(TOS)
			}
			name := bld.stack.Top(1)
			bld.stack.Pop()
			function := bld.stack.Top(1)
			bld.stack.Pop()
			loadbuild := bld.stack.Top(1)
			bld.stack.Pop()
			if ast.TypeOf(loadbuild) == ast.NODE_LOADBUILDCLASS {
				if bld.mod.VerCompare(3, 11) >= 0 && !bld.stack.Empty() && bld.stack.Top(1) == nil {
					bld.stack.Pop()
				}
				call := ast.NewCall(function, pparamList, classKwargs)
				bld.stack.Push(ast.NewClass(call, ast.NewTuple(bases), name))
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				break
			} else {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			}

			if bld.mod.VerCompare(3, 11) >= 0 {
				objectOrMap := bld.stack.Top(1)
				if ast.TypeOf(objectOrMap) == ast.NODE_KW_NAMES_MAP {
					bld.stack.Pop()
					for _, kv := range objectOrMap.(*ast.KwNamesMap).Values() {
						kwparamList = pushFrontKV(kwparamList, ast.KV{K: kv.K, V: kv.V})
						pparams -= 1
					}
				}
			} else {
				for i := 0; i < kwparams; i++ {
					val := bld.stack.Top(1)
					bld.stack.Pop()
					key := bld.stack.Top(1)
					bld.stack.Pop()
					kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
				}
			}
			for i := 0; i < pparams; i++ {
				param := bld.stack.Top(1)
				bld.stack.Pop()
				if ast.TypeOf(param) == ast.NODE_FUNCTION {
					funCode := param.(*ast.Function).Code()
					codeSrc := funCode.(*ast.Object).Object().(*pyc.Code)
					functionName := codeSrc.Name
					if functionName.Value == "<lambda>" {
						pparamList = pushFront(pparamList, param)
					} else {
						decorName := ast.NewName(functionName)
						bld.curblock.Append(ast.NewStore(param, decorName))
						pparamList = pushFront(pparamList, decorName)
					}
				} else {
					pparamList = pushFront(pparamList, param)
				}
			}
			fn := bld.stack.Top(1)
			bld.stack.Pop()

			isDecoratorApply := false
			compInlined := false
			if bld.mod.VerCompare(3, 11) < 0 && fn != nil && fn.Type() == ast.NODE_FUNCTION &&
				len(pparamList) == 1 && len(kwparamList) == 0 {
				ccode := fn.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
				cnm := ccode.Name.Value
				if cnm == "<listcomp>" || cnm == "<setcomp>" || cnm == "<dictcomp>" || cnm == "<genexpr>" {
					savedClean := cleanBuild
					compAst := BuildFromCode(ccode, bld.mod)
					cleanBuild = savedClean
					comp := findComprehension(compAst)
					if comp == nil {
						comp = synthGenexpr(compAst)
					}
					if comp != nil && len(comp.Generators()) != 0 {
						if cnm == "<setcomp>" {
							comp.SetKind(ast.KIND_SET)
						} else if cnm == "<dictcomp>" {
							comp.SetKind(ast.KIND_DICT)
						} else if cnm == "<genexpr>" {
							comp.SetKind(ast.KIND_GENERATOR)
						}
						comp.Generators()[0].SetIter(pparamList[0])
						bld.stack.Push(comp)
						compInlined = true
					}
				}
			}
			if bld.opcode == CALL_A || bld.opcode == INSTRUMENTED_CALL_A {
				if !bld.stack.Empty() && bld.stack.Top(1) == nil {
					bld.stack.Pop()
				} else if !bld.stack.Empty() && bld.stack.Top(1) != nil {
					below := bld.stack.Top(1)

					topIsDecoTarget := ast.TypeOf(fn) == ast.NODE_FUNCTION || ast.TypeOf(fn) == ast.NODE_CLASS ||
						(ast.TypeOf(fn) == ast.NODE_CALL && fn.(*ast.Call).IsDecorator())

					belowIsAssert := below != nil && below == bld.assertErrNode

					belowIsComp := false
					if bld.hadGetIter && below != nil && below.Type() == ast.NODE_FUNCTION {
						bcode := below.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
						bnm := bcode.Name.Value
						belowIsComp = bnm == "<listcomp>" || bnm == "<setcomp>" || bnm == "<dictcomp>" || bnm == "<genexpr>"
					}

					if belowIsAssert {
						pparamList = pushFront(pparamList, fn)
						fn = bld.stack.Top(1)
						bld.stack.Pop()
					} else if belowIsComp {
						ccode := below.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
						savedClean := cleanBuild
						compAst := BuildFromCode(ccode, bld.mod)
						cleanBuild = savedClean
						comp := findComprehension(compAst)
						if comp == nil {
							comp = synthGenexpr(compAst)
						}
						if comp != nil && len(comp.Generators()) != 0 {
							cnm := ccode.Name.Value
							if cnm == "<setcomp>" {
								comp.SetKind(ast.KIND_SET)
							} else if cnm == "<dictcomp>" {
								comp.SetKind(ast.KIND_DICT)
							} else if cnm == "<genexpr>" {
								comp.SetKind(ast.KIND_GENERATOR)
							}
							comp.Generators()[0].SetIter(fn)
							bld.stack.Pop()
							bld.stack.Push(comp)
							compInlined = true
						}
					} else if topIsDecoTarget {
						target := fn
						fn = bld.stack.Top(1)
						bld.stack.Pop()
						if ast.TypeOf(target) == ast.NODE_FUNCTION {
							tcode := target.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
							tname := tcode.Name
							if tname.Value != "<lambda>" {
								decorName := ast.NewName(tname)
								bld.curblock.Append(ast.NewStore(target, decorName))
								target = decorName
							}
						} else if ast.TypeOf(target) == ast.NODE_CLASS {
							cname := target.(*ast.Class).Name()
							var decorName ast.Node
							if cname != nil && cname.Type() == ast.NODE_NAME {
								decorName = cname
							} else if cname != nil && cname.Type() == ast.NODE_OBJECT {
								if s, ok := cname.(*ast.Object).Object().(*pyc.String); ok && s != nil {
									decorName = ast.NewName(s)
								}
							}
							if decorName != nil {
								bld.curblock.Append(ast.NewStore(target, decorName))
								target = decorName
							}
						}
						pparamList = pushFront(pparamList, target)
						isDecoratorApply = true
					}
				}
			}

			if !compInlined {
				callNode := ast.NewCall(fn, pparamList, kwparamList)
				if isDecoratorApply {
					callNode.SetDecorator(true)
				}
				bld.stack.Push(callNode)
			}
		}
	case CALL_FUNCTION_VAR_A:
		{
			vr := bld.stack.Top(1)
			bld.stack.Pop()
			kwparams := (bld.operand & 0xFF00) >> 8
			pparams := bld.operand & 0xFF
			var kwparamList []ast.KV
			var pparamList []ast.Node
			for i := 0; i < kwparams; i++ {
				val := bld.stack.Top(1)
				bld.stack.Pop()
				key := bld.stack.Top(1)
				bld.stack.Pop()
				kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
			}
			for i := 0; i < pparams; i++ {
				pparamList = pushFront(pparamList, bld.stack.Top(1))
				bld.stack.Pop()
			}
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			call := ast.NewCall(fn, pparamList, kwparamList)
			call.SetVar(vr)
			bld.stack.Push(call)
		}
	case CALL_FUNCTION_KW_A:
		if bld.mod.VerCompare(3, 6) >= 0 {
			names := bld.stack.Top(1)
			bld.stack.Pop()
			var keys []pyc.Object
			if names != nil && names.Type() == ast.NODE_OBJECT {
				keys = names.(*ast.Object).Object().(*pyc.Seq).Values()
			}
			n := len(keys)
			var kwparamList []ast.KV
			var pparamList []ast.Node
			for i := 0; i < n; i++ {
				kwparamList = pushFrontKV(kwparamList, ast.KV{K: ast.NewObject(keys[n-i-1]), V: bld.stack.Top(1)})
				bld.stack.Pop()
			}
			for i := 0; i < bld.operand-n; i++ {
				pparamList = pushFront(pparamList, bld.stack.Top(1))
				bld.stack.Pop()
			}
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewCall(fn, pparamList, kwparamList))
			break
		}
		{
			kw := bld.stack.Top(1)
			bld.stack.Pop()
			kwparams := (bld.operand & 0xFF00) >> 8
			pparams := bld.operand & 0xFF
			var kwparamList []ast.KV
			var pparamList []ast.Node
			for i := 0; i < kwparams; i++ {
				val := bld.stack.Top(1)
				bld.stack.Pop()
				key := bld.stack.Top(1)
				bld.stack.Pop()
				kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
			}
			for i := 0; i < pparams; i++ {
				pparamList = pushFront(pparamList, bld.stack.Top(1))
				bld.stack.Pop()
			}
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			call := ast.NewCall(fn, pparamList, kwparamList)
			call.SetKW(kw)
			bld.stack.Push(call)
		}
	case CALL_FUNCTION_VAR_KW_A:
		{
			kw := bld.stack.Top(1)
			bld.stack.Pop()
			vr := bld.stack.Top(1)
			bld.stack.Pop()
			kwparams := (bld.operand & 0xFF00) >> 8
			pparams := bld.operand & 0xFF
			var kwparamList []ast.KV
			var pparamList []ast.Node
			for i := 0; i < kwparams; i++ {
				val := bld.stack.Top(1)
				bld.stack.Pop()
				key := bld.stack.Top(1)
				bld.stack.Pop()
				kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
			}
			for i := 0; i < pparams; i++ {
				pparamList = pushFront(pparamList, bld.stack.Top(1))
				bld.stack.Pop()
			}
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			call := ast.NewCall(fn, pparamList, kwparamList)
			call.SetKW(kw)
			call.SetVar(vr)
			bld.stack.Push(call)
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
