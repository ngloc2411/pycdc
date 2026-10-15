package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsBuild() flow {
	switch bld.opcode {
	case DICT_UPDATE_A, DICT_MERGE_A:
		{
			rhs := bld.stack.Top(1)
			bld.stack.Pop()
			lhsNode := bld.stack.Top(1)
			if ast.TypeOf(lhsNode) == ast.NODE_MAP {
				lhs := lhsNode.(*ast.Map)
				if ast.TypeOf(rhs) == ast.NODE_MAP {
					for _, it := range rhs.(*ast.Map).Values() {
						lhs.Add(it.K, it.V)
					}
				} else {
					lhs.Add(nil, rhs)
				}
			}
		}
	case LIST_EXTEND_A:
		{
			rhs := bld.stack.Top(1)
			bld.stack.Pop()
			lhs, _ := bld.stack.Top(1).(*ast.List)
			bld.stack.Pop()

			var result []ast.Node
			if lhs != nil {
				result = append(result, lhs.Values()...)
			}

			if ast.TypeOf(rhs) == ast.NODE_OBJECT &&
				(rhs.(*ast.Object).Object().Type() == pyc.TypeTuple ||
					rhs.(*ast.Object).Object().Type() == pyc.TypeSmallTuple) {
				for _, it := range rhs.(*ast.Object).Object().(*pyc.Seq).Values() {
					result = append(result, ast.NewObject(it))
				}
			} else if ast.TypeOf(rhs) == ast.NODE_LIST {
				result = append(result, rhs.(*ast.List).Values()...)
			} else if ast.TypeOf(rhs) == ast.NODE_TUPLE {
				result = append(result, rhs.(*ast.Tuple).Values()...)
			} else {
				result = append(result, ast.NewUnary(rhs, ast.UN_UNPACK))
			}

			bld.stack.Push(ast.NewList(result))
		}
	case LOAD_ATTR_A:
		{
			name := bld.stack.Top(1)
			if ast.TypeOf(name) != ast.NODE_IMPORT {
				bld.stack.Pop()

				if bld.mod.VerCompare(3, 12) >= 0 {
					if bld.operand&1 != 0 {
						bld.stack.Push(nil)
					}
					bld.operand >>= 1
				}

				bld.stack.Push(ast.NewBinary(name, ast.NewName(bld.code.GetName(bld.operand)), ast.BIN_ATTR))
			}
		}
	case LOAD_COMMON_CONSTANT_A:
		{
			names := []string{"AssertionError", "NotImplementedError", "tuple", "all", "any"}
			if bld.operand == 0 {
				bld.pendingOps = append(bld.pendingOps, intPair{LOAD_ASSERTION_ERROR, 0})
			} else if bld.operand >= 1 && bld.operand <= 4 {
				bld.stack.Push(ast.NewName(pyc.NewString(names[bld.operand])))
				if bld.operand >= 2 {
					bld.commonGuard = true
				}
			} else {
				fmt.Fprintf(os.Stderr, "Unsupported LOAD_COMMON_CONSTANT %d\n", bld.operand)
			}
		}
	case LOAD_ASSERTION_ERROR:
		{
			bld.assertErrNode = ast.NewName(pyc.NewString("AssertionError"))
			bld.stack.Push(bld.assertErrNode)
		}
	case LOAD_BUILD_CLASS:
		bld.stack.Push(ast.NewLoadBuildClass(nil))
	case LOAD_CLOSURE_A:
		if bld.mod.VerCompare(3, 6) >= 0 {
			bld.stack.Push(ast.NewName(bld.code.GetCellVar(bld.mod, bld.operand)))
		}
	case MAKE_CELL_A, COPY_FREE_VARS_A:
	case LOAD_CONST_A:
		{
			tOb := ast.NewObject(bld.code.GetConst(bld.operand))
			ot := tOb.Object().Type()

			if ot == pyc.TypeSlice {
				sl := tOb.Object().(*pyc.Slice)
				bound := func(o pyc.Object) ast.Node {
					if o == nil || o.Type() == pyc.TypeNone {
						return nil
					}
					return ast.NewObject(o)
				}
				st, en, sp := bound(sl.Start), bound(sl.Stop), bound(sl.Step)
				var res ast.Node
				if st == nil && en == nil {
					res = ast.NewSlice(ast.SLICE0, nil, nil)
				} else if st == nil {
					res = ast.NewSlice(ast.SLICE2, st, en)
				} else if en == nil {
					res = ast.NewSlice(ast.SLICE1, st, en)
				} else {
					res = ast.NewSlice(ast.SLICE3, st, en)
				}
				if sp != nil {
					res = ast.NewSlice(ast.SLICE3, res, sp)
				}
				bld.stack.Push(res)
			} else if (ot == pyc.TypeTuple || ot == pyc.TypeSmallTuple) &&
				len(tOb.Object().(*pyc.Seq).Values()) == 0 {
				bld.stack.Push(ast.NewTuple(nil))
			} else if ot == pyc.TypeNone {
				bld.stack.Push(nil)
			} else {
				bld.stack.Push(tOb)
			}
		}
	case LOAD_DEREF_A, LOAD_CLASSDEREF_A:
		bld.stack.Push(ast.NewName(bld.code.GetCellVar(bld.mod, bld.operand)))
	case LOAD_FAST_AND_CLEAR_A:
		{
			k := 1
			var op2, arg2 int
			savedSrc := bld.source.Pos()
			savedPos := bld.pos
			ok := false
			container := 0
			for !bld.source.AtEof() {
				op2, arg2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				if op2 == LOAD_FAST_AND_CLEAR_A {
					k++
					continue
				}
				if op2 != SWAP_A || arg2 != k+1 {
					break
				}
				op2, arg2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				if (op2 != BUILD_LIST_A && op2 != BUILD_SET_A && op2 != BUILD_MAP_A) || arg2 != 0 {
					break
				}
				container = op2
				op2, arg2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				ok = op2 == SWAP_A && arg2 == 2
				break
			}
			if !ok {
				bld.source.SetPos(savedSrc)
				bld.pos = savedPos
				bld.stack.Push(nil)
				break
			}
			iter := bld.stack.Top(1)
			bld.stack.Pop()
			bld.inlineBase = append(bld.inlineBase, bld.stack.Size())
			if container == BUILD_LIST_A {
				bld.stack.Push(ast.NewList(nil))
			} else if container == BUILD_SET_A {
				bld.stack.Push(ast.NewSet(nil))
			} else {
				bld.stack.Push(ast.NewMap())
			}
			bld.stack.Push(iter)
			bld.inlineComps = append(bld.inlineComps, k)
			if container == BUILD_SET_A {
				bld.inlineKind = append(bld.inlineKind, ast.KIND_SET)
			} else if container == BUILD_MAP_A {
				bld.inlineKind = append(bld.inlineKind, ast.KIND_DICT)
			} else {
				bld.inlineKind = append(bld.inlineKind, ast.KIND_LIST)
			}
		}
	case LOAD_SMALL_INT_A:
		bld.stack.Push(ast.NewObject(pyc.NewInt(bld.operand)))
	case LOAD_FAST_A, LOAD_FAST_BORROW_A, LOAD_FAST_CHECK_A:
		if bld.mod.VerCompare(1, 3) < 0 {
			bld.stack.Push(ast.NewName(bld.code.GetName(bld.operand)))
		} else {
			bld.stack.Push(ast.NewName(bld.code.GetLocal(bld.operand)))
		}
	case LOAD_FAST_LOAD_FAST_A, LOAD_FAST_BORROW_LOAD_FAST_BORROW_A:
		bld.stack.Push(ast.NewName(bld.code.GetLocal(bld.operand >> 4)))
		bld.stack.Push(ast.NewName(bld.code.GetLocal(bld.operand & 0xF)))
	case LOAD_GLOBAL_A:
		if bld.mod.VerCompare(3, 11) >= 0 {
			if bld.operand&1 != 0 {
				bld.stack.Push(nil)
			}
			bld.operand >>= 1
		}
		bld.stack.Push(ast.NewName(bld.code.GetName(bld.operand)))
	case LOAD_LOCALS:
		bld.stack.Push(ast.NewLocals())
	case STORE_LOCALS:
		bld.stack.Pop()
	case LOAD_METHOD_A:
		{
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewBinary(name, ast.NewName(bld.code.GetName(bld.operand)), ast.BIN_ATTR))
		}
	case LOAD_NAME_A:
		bld.stack.Push(ast.NewName(bld.code.GetName(bld.operand)))
	case MAKE_CLOSURE_A, MAKE_FUNCTION_A:
		{
			funCode := bld.stack.Top(1)
			bld.stack.Pop()

			tosType := -1
			if o, ok := funCode.(*ast.Object); ok {
				tosType = o.Object().Type()
			}
			if tosType != pyc.TypeCode && tosType != pyc.TypeCode2 {
				funCode = bld.stack.Top(1)
				bld.stack.Pop()
			}

			var defArgs, kwDefArgs []ast.Node

			if bld.mod.VerCompare(3, 6) >= 0 {
				if bld.operand&0x08 != 0 {
					bld.stack.Pop()
				}
				if bld.operand&0x04 != 0 {
					bld.stack.Pop()
				}
				if bld.operand&0x02 != 0 {
					kwd := bld.stack.Top(1)
					bld.stack.Pop()
					if kwd != nil && ast.TypeOf(kwd) == ast.NODE_MAP {
						for _, it := range kwd.(*ast.Map).Values() {
							kwDefArgs = append(kwDefArgs, it.V)
						}
					} else if kwd != nil && ast.TypeOf(kwd) == ast.NODE_CONST_MAP {
						kwDefArgs = append(kwDefArgs, kwd.(*ast.ConstMap).Values()...)
					}
				}
				if bld.operand&0x01 != 0 {
					defs := bld.stack.Top(1)
					bld.stack.Pop()
					if defs != nil && ast.TypeOf(defs) == ast.NODE_TUPLE {
						defArgs = append(defArgs, defs.(*ast.Tuple).Values()...)
					} else if defs != nil && ast.TypeOf(defs) == ast.NODE_OBJECT {
						o := defs.(*ast.Object).Object()
						if o.Type() == pyc.TypeTuple || o.Type() == pyc.TypeSmallTuple {
							for _, it := range o.(*pyc.Seq).Values() {
								defArgs = append(defArgs, ast.NewObject(it))
							}
						}
					}
				}
			} else {
				defCount := bld.operand & 0xFF
				kwDefCount := (bld.operand >> 8) & 0xFF
				for i := 0; i < defCount; i++ {
					defArgs = pushFront(defArgs, bld.stack.Top(1))
					bld.stack.Pop()
				}
				for i := 0; i < kwDefCount; i++ {
					kwDefArgs = pushFront(kwDefArgs, bld.stack.Top(1))
					bld.stack.Pop()
				}
			}
			bld.stack.Push(ast.NewFunction(funCode, defArgs, kwDefArgs))
		}
	case MAKE_FUNCTION:
		{
			funCode := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewFunction(funCode, nil, nil))
		}
	case SET_FUNCTION_ATTRIBUTE_A:
		{
			fn0 := bld.stack.Top(1)
			bld.stack.Pop()
			attr := bld.stack.Top(1)
			bld.stack.Pop()
			if fn0 == nil || ast.TypeOf(fn0) != ast.NODE_FUNCTION {
				bld.stack.Push(fn0)
				break
			}
			fn := fn0.(*ast.Function)
			defArgs := append([]ast.Node(nil), fn.Defargs()...)
			kwDefArgs := append([]ast.Node(nil), fn.Kwdefargs()...)
			if bld.operand == 0x01 {
				defArgs = nil
				if attr != nil && ast.TypeOf(attr) == ast.NODE_TUPLE {
					defArgs = append(defArgs, attr.(*ast.Tuple).Values()...)
				} else if attr != nil && ast.TypeOf(attr) == ast.NODE_OBJECT {
					o := attr.(*ast.Object).Object()
					if o.Type() == pyc.TypeTuple || o.Type() == pyc.TypeSmallTuple {
						for _, it := range o.(*pyc.Seq).Values() {
							defArgs = append(defArgs, ast.NewObject(it))
						}
					}
				}
			} else if bld.operand == 0x02 {
				kwDefArgs = nil
				if attr != nil && ast.TypeOf(attr) == ast.NODE_MAP {
					for _, it := range attr.(*ast.Map).Values() {
						kwDefArgs = append(kwDefArgs, it.V)
					}
				} else if attr != nil && ast.TypeOf(attr) == ast.NODE_CONST_MAP {
					kwDefArgs = append(kwDefArgs, attr.(*ast.ConstMap).Values()...)
				}
			}
			bld.stack.Push(ast.NewFunction(fn.Code(), defArgs, kwDefArgs))
		}
	case POP_ITER:
	case NOT_TAKEN, NOP:
	case POP_BLOCK:
		{
			if bld.curblock.BlkType() == ast.BLK_CONTAINER || bld.curblock.BlkType() == ast.BLK_FINALLY {
				break
			}

			if bld.curblock.BlkType() == ast.BLK_WITH {
				break
			}

			if bld.curblock.Size() != 0 &&
				ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_KEYWORD {
				bld.curblock.RemoveLast()
			}

			if bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF ||
				bld.curblock.BlkType() == ast.BLK_ELSE || bld.curblock.BlkType() == ast.BLK_TRY ||
				bld.curblock.BlkType() == ast.BLK_EXCEPT || bld.curblock.BlkType() == ast.BLK_FINALLY {
				if len(bld.stackHist) != 0 {
					bld.stack = bld.stackHist[len(bld.stackHist)-1]
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				} else {
					fmt.Fprint(os.Stderr, "Warning: Stack history is empty, something wrong might have happened\n")
				}
			}
			tmp := bld.curblock
			bld.blocks.pop()

			if !bld.blocks.empty() {
				bld.curblock = bld.blocks.top()
			}

			if !(tmp.BlkType() == ast.BLK_ELSE && tmp.Size() == 0) {
				bld.curblock.Append(tmp)
			}

			if tmp.BlkType() == ast.BLK_FOR && tmp.End() >= bld.pos {
				bld.stackHist = append(bld.stackHist, bld.stack.Copy())

				blkelse := ast.NewBlock(ast.BLK_ELSE, tmp.End(), 0)
				bld.blocks.push(blkelse)
				bld.curblock = bld.blocks.top()
			}

			if bld.curblock.BlkType() == ast.BLK_TRY &&
				tmp.BlkType() != ast.BLK_FOR && tmp.BlkType() != ast.BLK_ASYNCFOR &&
				tmp.BlkType() != ast.BLK_WHILE {
				if len(bld.stackHist) != 0 {
					bld.stack = bld.stackHist[len(bld.stackHist)-1]
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				}

				tmp = bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()

				if !(tmp.BlkType() == ast.BLK_ELSE && tmp.Size() == 0) {
					bld.curblock.Append(tmp)
				}
			}

			if bld.curblock.BlkType() == ast.BLK_CONTAINER {
				cont := bld.curblock

				if tmp.BlkType() == ast.BLK_ELSE && !cont.HasFinally() {
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()
					bld.curblock.Append(cont)
				} else if (tmp.BlkType() == ast.BLK_ELSE && cont.HasFinally()) ||
					(tmp.BlkType() == ast.BLK_TRY && !cont.HasExcept()) {
					bld.stackHist = append(bld.stackHist, bld.stack.Copy())

					final := ast.NewBlock(ast.BLK_FINALLY, 0, 1)
					bld.blocks.push(final)
					bld.curblock = bld.blocks.top()
				}
			}

			if (bld.curblock.BlkType() == ast.BLK_FOR || bld.curblock.BlkType() == ast.BLK_ASYNCFOR) &&
				bld.curblock.End() == bld.pos {
				bld.blocks.pop()
				bld.blocks.top().Append(bld.curblock)
				bld.curblock = bld.blocks.top()
			}
		}
	case POP_EXCEPT:
	case PUSH_EXC_INFO:
		bld.stack.Push(ast.NewName(pyc.NewString(excSentinel)))
	case CHECK_EXC_MATCH:
		{
			right := bld.stack.Top(1)
			bld.stack.Pop()
			left := bld.stack.Top(1)
			bld.stack.Push(ast.NewCompare(left, right, ast.CMP_EXCEPTION))
		}
	case END_FOR:
		{
			inlineCompLoop := len(bld.inlineComps) != 0
			if inlineCompLoop &&
				(bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
				insideComprehensionAdd(bld.blocks) {
				for bld.blocks.size() > 1 &&
					!(bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.IsComprehension()) {
					inner := bld.curblock
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()
					bld.curblock.Append(inner)
				}
				if bld.curblock.BlkType() == ast.BLK_FOR && bld.blocks.size() > 1 {
					fb := bld.curblock
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()
					bld.curblock.Append(fb)
				}
			}
			if inlineCompLoop {
				type opRange struct{ start, end, weight int }
				k := bld.inlineComps[len(bld.inlineComps)-1]
				sSrc, sPos := bld.source.Pos(), bld.pos
				var o2, a2 int
				var seq []opRange
				finishing := false
				lastP := bld.pos
				if !bld.source.AtEof() {
					o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
					if o2 == POP_ITER || (o2 == POP_TOP && bld.mod.VerCompare(3, 13) == 0) {
						bld.cleanupSkip = append(bld.cleanupSkip, intPair{lastP, bld.pos})
						lastP = bld.pos
						if !bld.source.AtEof() {
							o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
						}
					}
				}
				storeWeight := func(o int) int {
					if o == STORE_FAST_A {
						return 1
					}
					if o == STORE_FAST_STORE_FAST_A {
						return 2
					}
					return 0
				}
				if o2 == SWAP_A && a2 == k+1 {
					seq = append(seq, opRange{lastP, bld.pos, 0})
					cnt := 0
					for !bld.source.AtEof() && cnt < k {
						st := bld.pos
						o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
						w := storeWeight(o2)
						if w == 0 {
							break
						}
						seq = append(seq, opRange{st, bld.pos, w})
						cnt += w
					}
					finishing = cnt == k
				} else if storeWeight(o2) != 0 {
					var stores []opRange
					stores = append(stores, opRange{lastP, bld.pos, storeWeight(o2)})
					for !bld.source.AtEof() {
						st := bld.pos
						o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
						w := storeWeight(o2)
						if w == 0 {
							break
						}
						stores = append(stores, opRange{st, bld.pos, w})
					}
					total := 0
					for _, st := range stores {
						total += st.weight
					}
					if total >= k {
						finishing = true
						need := k
						for i := len(stores); i > 0 && need > 0; i-- {
							st := stores[i-1]
							if st.weight <= need {
								seq = append(seq, st)
								need -= st.weight
							} else {
								bld.fusedKeepFirst[st.start] = true
								need = 0
							}
						}
					}
				}
				_ = a2
				bld.source.SetPos(sSrc)
				bld.pos = sPos
				if finishing {
					for _, r := range seq {
						bld.cleanupSkip = append(bld.cleanupSkip, intPair{r.start, r.end})
					}
					bld.inlineComps = bld.inlineComps[:len(bld.inlineComps)-1]
					bld.inlineFinish = true
					bld.inlineFinishBase = bld.inlineBase[len(bld.inlineBase)-1]
					bld.inlineBase = bld.inlineBase[:len(bld.inlineBase)-1]
					bld.inlineFinishKind = bld.inlineKind[len(bld.inlineKind)-1]
					bld.inlineKind = bld.inlineKind[:len(bld.inlineKind)-1]
				}
			}
			if !inlineCompLoop {
				bld.stack.Pop()
			}

			if !inlineCompLoop && bld.opcode == END_FOR && bld.mod.MajorVer() == 3 && bld.mod.MinorVer() == 12 {
				bld.stack.Pop()
			}

			if inlineCompLoop {
			} else if bld.curblock.BlkType() == ast.BLK_FOR &&
				(bld.curblock.End() == bld.curpos || bld.curblock.End() == 0 || bld.curblock.End() < bld.curpos) {
				prev := bld.blocks.top()
				bld.blocks.pop()

				bld.curblock = bld.blocks.top()
				bld.curblock.Append(prev)
			} else if bld.curblock.BlkType() == ast.BLK_FOR {
			} else if bld.curblock.BlkType() != ast.BLK_MAIN && bld.curblock.BlkType() != ast.BLK_WHILE &&
				bld.curblock.BlkType() != ast.BLK_IF && bld.curblock.BlkType() != ast.BLK_ELIF &&
				bld.curblock.BlkType() != ast.BLK_ELSE {
				fmt.Fprintf(os.Stderr, "Wrong block type %d for END_FOR\n", bld.curblock.BlkType())
			}

			if bld.inlineFinish {
				bld.inlineFinish = false
				var res ast.Node
				if !bld.stack.Empty() && bld.stack.Top(1) != nil && ast.TypeOf(bld.stack.Top(1)) == ast.NODE_COMPREHENSION {
					res = bld.stack.Top(1)
				} else if bld.curblock.Size() > 0 {
					synth := synthGenexpr(bld.curblock.Nodes()[bld.curblock.Size()-1])
					if synth != nil {
						bld.curblock.RemoveLast()
						res = synth
					}
				}
				if res != nil && ast.TypeOf(res) == ast.NODE_COMPREHENSION {
					res.(*ast.Comprehension).SetKind(bld.inlineFinishKind)
				}
				if res != nil && bld.stack.Size() > bld.inlineFinishBase {
					for bld.stack.Size() > bld.inlineFinishBase {
						bld.stack.Pop()
					}
					bld.stack.Push(res)
				}
			}
		}
	case BUILD_LIST_UNPACK_A, BUILD_TUPLE_UNPACK_A, BUILD_TUPLE_UNPACK_WITH_CALL_A, BUILD_SET_UNPACK_A:
		{
			count := bld.operand & 0xff
			items := make([]ast.Node, count)
			for i := count - 1; i >= 0; i-- {
				items[i] = bld.stack.Top(1)
				bld.stack.Pop()
			}
			var out []ast.Node
			for _, item := range items {
				if item != nil && ast.TypeOf(item) == ast.NODE_OBJECT &&
					(item.(*ast.Object).Object().Type() == pyc.TypeTuple ||
						item.(*ast.Object).Object().Type() == pyc.TypeSmallTuple) {
					for _, v := range item.(*ast.Object).Object().(*pyc.Seq).Values() {
						out = append(out, ast.NewObject(v))
					}
				} else if item != nil && ast.TypeOf(item) == ast.NODE_TUPLE {
					out = append(out, item.(*ast.Tuple).Values()...)
				} else if item != nil && ast.TypeOf(item) == ast.NODE_LIST && bld.opcode == BUILD_LIST_UNPACK_A {
					out = append(out, item.(*ast.List).Values()...)
				} else if item != nil && ast.TypeOf(item) == ast.NODE_SET && bld.opcode == BUILD_SET_UNPACK_A {
					out = append(out, item.(*ast.Set).Values()...)
				} else {
					out = append(out, ast.NewUnary(item, ast.UN_UNPACK))
				}
			}
			if bld.opcode == BUILD_LIST_UNPACK_A {
				bld.stack.Push(ast.NewList(out))
			} else if bld.opcode == BUILD_SET_UNPACK_A {
				bld.stack.Push(ast.NewSet(out))
			} else {
				bld.stack.Push(ast.NewTuple(out))
			}
		}
	case BUILD_MAP_UNPACK_A, BUILD_MAP_UNPACK_WITH_CALL_A:
		{
			count := bld.operand & 0xff
			items := make([]ast.Node, count)
			for i := count - 1; i >= 0; i-- {
				items[i] = bld.stack.Top(1)
				bld.stack.Pop()
			}
			result := ast.NewMap()
			for _, item := range items {
				if item != nil && ast.TypeOf(item) == ast.NODE_MAP {
					for _, kv := range item.(*ast.Map).Values() {
						result.Add(kv.K, kv.V)
					}
				} else {
					result.Add(nil, item)
				}
			}
			bld.stack.Push(result)
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
