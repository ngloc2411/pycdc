package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsCall() flow {
	switch bld.opcode {
	case CALL_FUNCTION_EX_A, INSTRUMENTED_CALL_FUNCTION_EX_A:
		{
			var kw ast.Node
			if bld.operand&0x01 != 0 || bld.mod.VerCompare(3, 14) >= 0 {
				kw = bld.stack.Top(1)
				bld.stack.Pop()
			}
			vr := bld.stack.Top(1)
			bld.stack.Pop()
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			if bld.mod.VerCompare(3, 11) >= 0 && !bld.stack.Empty() && bld.stack.Top(1) == nil {
				bld.stack.Pop()
			}

			var exPos []ast.Node
			var exKw []ast.KV
			if vr != nil && vr.Type() == ast.NODE_TUPLE {
				exPos = append(exPos, vr.(*ast.Tuple).Values()...)
				vr = nil
			} else if vr != nil && vr.Type() == ast.NODE_OBJECT && tupleLike(vr.(*ast.Object).Object()) {
				for _, v := range vr.(*ast.Object).Object().(*pyc.Seq).Values() {
					exPos = append(exPos, ast.NewObject(v))
				}
				vr = nil
			}
			if kw != nil && kw.Type() == ast.NODE_MAP {
				var unpackedKw ast.Node
				unpackCount := 0
				allSimple := true
				for _, kv := range kw.(*ast.Map).Values() {
					if kv.K == nil {
						unpackCount++
						unpackedKw = kv.V
					} else if kv.K.Type() != ast.NODE_OBJECT {
						allSimple = false
					} else if _, ok := kv.K.(*ast.Object).Object().(*pyc.String); !ok {
						allSimple = false
					}
				}
				if allSimple && unpackCount <= 1 {
					for _, kv := range kw.(*ast.Map).Values() {
						if kv.K != nil {
							exKw = append(exKw, kv)
						}
					}
					kw = unpackedKw
				}
			}

			call := ast.NewCall(fn, exPos, exKw)
			if vr != nil {
				call.SetVar(vr)
			}
			if kw != nil {
				call.SetKW(kw)
			}
			bld.stack.Push(call)
		}
	case CALL_METHOD_A:
		{
			var pparamList []ast.Node
			for i := 0; i < bld.operand; i++ {
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
			bld.stack.Push(ast.NewCall(fn, pparamList, nil))
		}
	case CONTINUE_LOOP_A:
		bld.curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
	case JUMP_IF_NOT_EXC_MATCH_A:
		{
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewCompare(left, right, ast.CMP_EXCEPTION))
			bld.pendingOps = append(bld.pendingOps, intPair{POP_JUMP_IF_FALSE_A, bld.operand})
		}
	case COMPARE_OP_A:
		{
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			arg := bld.operand
			if bld.mod.VerCompare(3, 12) == 0 {
				arg >>= 4
			} else if bld.mod.VerCompare(3, 13) >= 0 {
				arg >>= 5
			}
			bld.stack.Push(ast.NewCompare(left, right, arg))
		}
	case CONTAINS_OP_A:
		{
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			op := ast.CMP_IN
			if bld.operand != 0 {
				op = ast.CMP_NOT_IN
			}
			bld.stack.Push(ast.NewCompare(left, right, op))
		}
	case DELETE_ATTR_A:
		{
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewBinary(name, ast.NewName(bld.code.GetName(bld.operand)), ast.BIN_ATTR)))
		}
	case DELETE_GLOBAL_A:
		bld.code.MarkGlobal(bld.code.GetName(bld.operand))
		fallthrough
	case DELETE_NAME_A:
		{
			varname := bld.code.GetName(bld.operand)
			if varname.Length() >= 2 && varname.Value[0] == '_' && varname.Value[1] == '[' {
				break
			}
			name := ast.NewName(varname)
			if isExceptVarDelete(bld.curblock, name) {
				break
			}
			bld.curblock.Append(ast.NewDelete(name))
		}
	case DELETE_FAST_A:
		{
			var name *ast.Name
			if bld.mod.VerCompare(1, 3) < 0 {
				name = ast.NewName(bld.code.GetName(bld.operand))
			} else {
				name = ast.NewName(bld.code.GetLocal(bld.operand))
			}
			nv := name.Name().Value
			if len(nv) >= 2 && nv[0] == '_' && nv[1] == '[' {
				break
			}
			if isExceptVarDelete(bld.curblock, name) {
				break
			}
			bld.curblock.Append(ast.NewDelete(name))
		}
	case DELETE_SLICE_0:
		{
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE0, nil, nil))))
		}
	case DELETE_SLICE_1:
		{
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE1, upper, nil))))
		}
	case DELETE_SLICE_2:
		{
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE2, nil, lower))))
		}
	case DELETE_SLICE_3:
		{
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE3, upper, lower))))
		}
	case DELETE_SUBSCR:
		{
			key := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewDelete(ast.NewSubscr(name, key)))
		}
	case DUP_TOP:
		{
			if bld.tryWalrus() {
				break
			}
			if bld.mod.VerCompare(3, 11) < 0 && bld.stack.Size() >= 2 {
				sp := bld.source.Pos()
				p2 := bld.pos
				o2, _ := pyc.BcNext(bld.source, bld.mod, &p2)
				isChain := false
				if o2 == ROT_THREE {
					p3 := p2
					o3, _ := pyc.BcNext(bld.source, bld.mod, &p3)
					isChain = o3 == COMPARE_OP_A || o3 == IS_OP_A || o3 == CONTAINS_OP_A
				}
				if isChain {
					b := bld.stack.Top(1)
					bld.stack.Pop()
					a := bld.stack.Top(1)
					bld.stack.Pop()
					bld.stack.Push(b)
					bld.stack.Push(a)
					bld.stack.Push(b)
					bld.source.SetPos(p2)
					bld.pos = p2
					break
				}
				bld.source.SetPos(sp)
			}
			if bld.stack.Top(1) == nil {
				bld.stack.Push(bld.stack.Top(1))
			} else if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				chainstore := bld.stack.Top(1)
				bld.stack.Pop()
				bld.stack.Push(bld.stack.Top(1))
				bld.stack.Push(chainstore)
			} else {
				bld.stack.Push(bld.stack.Top(1))
				bld.stack.Push(ast.NewChainStore(nil, bld.stack.Top(1)))
			}
		}
	case DUP_TOP_TWO:
		{
			first := bld.stack.Top(1)
			bld.stack.Pop()
			second := bld.stack.Top(1)
			bld.stack.Push(first)
			bld.stack.Push(second)
			bld.stack.Push(first)
		}
	case DUP_TOPX_A:
		{
			var first, second []ast.Node
			for i := 0; i < bld.operand; i++ {
				node := bld.stack.Top(1)
				bld.stack.Pop()
				first = append(first, node)
				second = append(second, node)
			}
			for len(first) != 0 {
				bld.stack.Push(first[len(first)-1])
				first = first[:len(first)-1]
			}
			for len(second) != 0 {
				bld.stack.Push(second[len(second)-1])
				second = second[:len(second)-1]
			}
		}
	case END_FINALLY:
		{
			isFinally := false
			if bld.curblock.BlkType() == ast.BLK_FINALLY {
				final := bld.curblock
				bld.blocks.pop()

				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]

				bld.curblock = bld.blocks.top()
				bld.curblock.Append(final)
				isFinally = true
			} else if bld.curblock.BlkType() == ast.BLK_EXCEPT {
				bld.blocks.pop()
				prev := bld.curblock

				isUninitAsyncFor := false
				if bld.blocks.top().BlkType() == ast.BLK_CONTAINER {
					container := bld.blocks.top()
					bld.blocks.pop()
					asyncForBlock := bld.blocks.top()
					isUninitAsyncFor = asyncForBlock.BlkType() == ast.BLK_ASYNCFOR && asyncForBlock.Inited() == 0
					if isUninitAsyncFor {
						tryBlock := container.Nodes()[0].(*ast.Block)
						if len(tryBlock.Nodes()) != 0 && tryBlock.BlkType() == ast.BLK_TRY {
							if store, ok := tryBlock.Nodes()[0].(*ast.Store); ok {
								asyncForBlock.SetIndex(store.Dest())
							}
						}
						bld.curblock = bld.blocks.top()
						bld.stack = bld.stackHist[len(bld.stackHist)-1]
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
						if bld.curblock.Inited() == 0 {
							fmt.Fprint(os.Stderr, "Error when decompiling 'async for'.\n")
						}
					} else {
						bld.blocks.push(container)
					}
				}

				if !isUninitAsyncFor {
					if bld.curblock.Size() != 0 {
						bld.blocks.top().Append(bld.curblock)
					}

					bld.curblock = bld.blocks.top()

					if bld.curblock.End() != bld.pos || bld.curblock.HasFinally() {
						elseblk := ast.NewBlock(ast.BLK_ELSE, prev.End(), 0)
						elseblk.Init()
						bld.blocks.push(elseblk)
						bld.curblock = bld.blocks.top()
					} else {
						bld.stack = bld.stackHist[len(bld.stackHist)-1]
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
					}
				}
			}

			if bld.curblock.BlkType() == ast.BLK_CONTAINER {
				cont := bld.curblock
				if !cont.HasFinally() || isFinally {
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()
					bld.curblock.Append(cont)
				}
			}
		}
	case EXEC_STMT:
		{
			if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				bld.stack.Pop()
			}
			loc := bld.stack.Top(1)
			bld.stack.Pop()
			glob := bld.stack.Top(1)
			bld.stack.Pop()
			stmt := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewExec(stmt, glob, loc))
		}
	case FOR_ITER_A, INSTRUMENTED_FOR_ITER_A:
		{
			iter := bld.stack.Top(1)
			if bld.mod.VerCompare(3, 12) < 0 {
				bld.stack.Pop()
			}

			var end int
			comprehension := false

			if bld.mod.MajorVer() == 3 && bld.mod.MinorVer() >= 8 {
				end = bld.operand
				if bld.mod.VerCompare(3, 10) >= 0 {
					end *= 2
				}
				end += bld.pos
				if bld.mod.VerCompare(3, 12) >= 0 {
					end += 2
				}
				cn := bld.code.Name.Value
				comprehension = cn == "<listcomp>" || cn == "<setcomp>" || cn == "<dictcomp>"
				if len(bld.inlineComps) != 0 {
					comprehension = true
				}
			} else {
				top := bld.blocks.top()
				end = top.End()
				if top.BlkType() == ast.BLK_WHILE {
					bld.blocks.pop()
				} else {
					comprehension = true
				}
			}

			forblk := ast.NewIterBlock(ast.BLK_FOR, bld.curpos, end, iter)
			forblk.SetComprehension(comprehension)
			bld.blocks.push(forblk)
			bld.curblock = bld.blocks.top()

			bld.stack.Push(nil)
		}
	case FOR_LOOP_A:
		{
			curidx := bld.stack.Top(1)
			bld.stack.Pop()
			iter := bld.stack.Top(1)
			bld.stack.Pop()

			comprehension := false
			top := bld.blocks.top()
			if top.BlkType() == ast.BLK_WHILE {
				bld.blocks.pop()
			} else {
				comprehension = true
			}
			forblk := ast.NewIterBlock(ast.BLK_FOR, bld.curpos, top.End(), iter)
			forblk.SetComprehension(comprehension)
			bld.blocks.push(forblk)
			bld.curblock = bld.blocks.top()

			bld.stack.Push(iter)
			bld.stack.Push(curidx)
			bld.stack.Push(nil)
		}
	case GET_AITER:
		{
			iter := bld.stack.Top(1)
			bld.stack.Pop()

			top := bld.blocks.top()
			if top.BlkType() == ast.BLK_WHILE {
				bld.blocks.pop()
				forblk := ast.NewIterBlock(ast.BLK_ASYNCFOR, bld.curpos, top.End(), iter)
				bld.blocks.push(forblk)
				bld.curblock = bld.blocks.top()
				bld.stack.Push(nil)
			} else {
				fmt.Fprint(os.Stderr, "Unsupported use of GET_AITER outside of SETUP_LOOP\n")
			}
		}
	case GET_ANEXT:
	case TO_BOOL:
	case STORE_FAST_STORE_FAST_A:
		bld.pendingOps = append(bld.pendingOps, intPair{STORE_FAST_A, bld.operand >> 4})
		if !bld.fusedKeepFirst[bld.curpos] {
			bld.pendingOps = append(bld.pendingOps, intPair{STORE_FAST_A, bld.operand & 0xF})
		}
	case STORE_FAST_LOAD_FAST_A:
		bld.pendingOps = append(bld.pendingOps, intPair{STORE_FAST_A, bld.operand >> 4})
		bld.pendingOps = append(bld.pendingOps, intPair{LOAD_FAST_A, bld.operand & 0xF})
	case CONVERT_VALUE_A:
		{
			val := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewFormattedValue(val, bld.operand, nil))
		}
	case FORMAT_SIMPLE:
		{
			val := bld.stack.Top(1)
			if val == nil || val.Type() != ast.NODE_FORMATTEDVALUE {
				bld.stack.Pop()
				bld.stack.Push(ast.NewFormattedValue(val, ast.FV_NONE, nil))
			}
		}
	case FORMAT_WITH_SPEC:
		{
			spec := bld.stack.Top(1)
			bld.stack.Pop()
			val := bld.stack.Top(1)
			bld.stack.Pop()
			conv := ast.FV_NONE
			if val != nil && val.Type() == ast.NODE_FORMATTEDVALUE {
				fv := val.(*ast.FormattedValue)
				conv = fv.Conversion() & ast.FV_CONVERSION_MASK
				val = fv.Val()
			}
			bld.stack.Push(ast.NewFormattedValue(val, conv|ast.FV_HAVE_FMT_SPEC, spec))
		}
	case CALL_KW_A:
		{
			names := bld.stack.Top(1)
			bld.stack.Pop()
			kwparamList := ast.NewKwNamesMap()
			if names != nil && names.Type() == ast.NODE_OBJECT {
				keys := names.(*ast.Object).Object().(*pyc.Seq).Values()
				n := len(keys)
				for i := 0; i < n; i++ {
					kwparamList.Add(ast.NewObject(keys[n-i-1]), bld.stack.Top(1))
					bld.stack.Pop()
				}
			}
			bld.stack.Push(kwparamList)
			bld.pendingOps = append(bld.pendingOps, intPair{CALL_A, bld.operand})
		}
	case FORMAT_VALUE_A:
		{
			conversionFlag := bld.operand
			var formatSpec ast.Node
			if conversionFlag&ast.FV_HAVE_FMT_SPEC != 0 {
				formatSpec = bld.stack.Top(1)
				bld.stack.Pop()
			}
			val := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewFormattedValue(val, conversionFlag, formatSpec))
		}
	case GET_AWAITABLE, GET_AWAITABLE_A:
		{
			object := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewAwaitable(object))
		}
	case GET_ITER, GET_YIELD_FROM_ITER:
		bld.sawGetIter = true
	case IMPORT_NAME_A:
		if bld.mod.MajorVer() == 1 {
			bld.stack.Push(ast.NewImport(ast.NewName(bld.code.GetName(bld.operand)), nil))
		} else {
			fromlist := bld.stack.Top(1)
			bld.stack.Pop()
			level := 0
			if bld.mod.VerCompare(2, 5) >= 0 {
				levelNode := bld.stack.Top(1)
				bld.stack.Pop()
				if levelNode != nil && levelNode.Type() == ast.NODE_OBJECT {
					levelObj := levelNode.(*ast.Object).Object()
					if pyc.TypeOf(levelObj) == pyc.TypeInt {
						level = levelObj.(*pyc.Int).Value
					}
				}
			}
			imp := ast.NewImport(ast.NewName(bld.code.GetName(bld.operand)), fromlist)
			imp.SetLevel(level)
			bld.stack.Push(imp)
		}
	case IMPORT_FROM_A:
		bld.stack.Push(ast.NewName(bld.code.GetName(bld.operand)))
	case IMPORT_STAR:
		{
			imp := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewStore(imp, nil))
		}
	case IS_OP_A:
		{
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Pop()
			op := ast.CMP_IS
			if bld.operand != 0 {
				op = ast.CMP_IS_NOT
			}
			bld.stack.Push(ast.NewCompare(left, right, op))
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
