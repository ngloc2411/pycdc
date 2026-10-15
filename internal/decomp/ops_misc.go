package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsMisc() flow {
	switch bld.opcode {
	case UNPACK_EX_A:
		bld.unpackBegin((bld.operand&0xff)+(bld.operand>>8)+1, bld.operand&0xff)
	case UNPACK_LIST_A, UNPACK_TUPLE_A, UNPACK_SEQUENCE_A:
		{
			if bld.operand > 0 {
				bld.unpackBegin(bld.operand, -1)
			} else {
				tup := ast.NewTuple(nil)
				if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.Inited() == 0 {
					tup.SetRequireParens(true)
					bld.curblock.SetIndex(tup)
				} else if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
					chainStore := bld.stack.Top(1)
					bld.stack.Pop()
					appendToChainStore(chainStore, tup, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(bld.stack.Top(1), tup))
					bld.stack.Pop()
				}
			}
		}
	case YIELD_FROM:
		{
			bld.stack.Pop()
			value := bld.stack.Top(1)
			if value != nil {
				value.SetProcessed()
				bld.curblock.Append(ast.NewReturn(value, ast.RET_YIELD_FROM))
			}
		}
	case SEND_A:
		bld.stack.Pop()
	case YIELD_VALUE, YIELD_VALUE_A, INSTRUMENTED_YIELD_VALUE_A:
		{
			value := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
		}
	case SETUP_ANNOTATIONS:
		bld.variableAnnotations = true
	case PRECALL_A, RESUME_A, INSTRUMENTED_RESUME_A:
	case CACHE:
	case PUSH_NULL:
		if bld.mod.VerCompare(3, 13) >= 0 && !bld.stack.Empty() {
			callable := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(nil)
			bld.stack.Push(callable)
		} else {
			bld.stack.Push(nil)
		}
	case GEN_START_A:
		bld.stack.Pop()
	case SWAP_A:
		{
			var pkOp int
			savedSrc := bld.source.Pos()
			savedPos := bld.pos
			if !bld.source.AtEof() {
				pkOp, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
			}
			if bld.mod.VerCompare(3, 12) < 0 && bld.operand == 2 && pkOp == POP_TOP && !bld.source.AtEof() {
				rPos := bld.pos
				midSrc := bld.source.Pos()
				rOp, _ := pyc.BcNext(bld.source, bld.mod, &rPos)
				bld.source.SetPos(midSrc)
				inFor := false
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					if bld.blocks[i].BlkType() == ast.BLK_FOR {
						inFor = !bld.blocks[i].IsComprehension()
						break
					}
				}
				if inFor && rOp == RETURN_VALUE {
					break
				}
			}
			bld.source.SetPos(savedSrc)
			bld.pos = savedPos
			if pkOp == COPY_A || pkOp == POP_TOP || bld.operand < 2 ||
				pkOp == SWAP_A || pkOp == STORE_SUBSCR ||
				pkOp == STORE_ATTR_A || pkOp == STORE_SLICE ||
				bld.stack.Size() < bld.operand {
				bld.stack.Swap(bld.operand)
				break
			}

			bld.unpack = bld.operand
			values := make([]ast.Node, bld.operand)
			for i := 0; i < bld.operand; i++ {
				values[bld.operand-i-1] = bld.stack.Top(1)
				bld.stack.Pop()
			}
			tup := ast.NewTuple(values)
			tup.SetRequireParens(false)
			nextTup := ast.NewTuple(nil)
			nextTup.SetRequireParens(false)
			bld.stack.Push(tup)
			bld.stack.Push(nextTup)
		}
	case BINARY_SLICE:
		{
			end := bld.stack.Top(1)
			bld.stack.Pop()
			start := bld.stack.Top(1)
			bld.stack.Pop()
			dest := bld.stack.Top(1)
			bld.stack.Pop()

			if ast.TypeOf(start) == ast.NODE_OBJECT && start.(*ast.Object).Object() == pyc.Object(pyc.None) {
				start = nil
			}
			if ast.TypeOf(end) == ast.NODE_OBJECT && end.(*ast.Object).Object() == pyc.Object(pyc.None) {
				end = nil
			}

			var slice ast.Node
			if start == nil && end == nil {
				slice = ast.NewSlice(ast.SLICE0, nil, nil)
			} else if start == nil {
				slice = ast.NewSlice(ast.SLICE2, start, end)
			} else if end == nil {
				slice = ast.NewSlice(ast.SLICE1, start, end)
			} else {
				slice = ast.NewSlice(ast.SLICE3, start, end)
			}
			bld.stack.Push(ast.NewSubscr(dest, slice))
		}
	case STORE_SLICE:
		{
			end := bld.stack.Top(1)
			bld.stack.Pop()
			start := bld.stack.Top(1)
			bld.stack.Pop()
			dest := bld.stack.Top(1)
			bld.stack.Pop()
			values := bld.stack.Top(1)
			bld.stack.Pop()

			if ast.TypeOf(start) == ast.NODE_OBJECT && start.(*ast.Object).Object() == pyc.Object(pyc.None) {
				start = nil
			}
			if ast.TypeOf(end) == ast.NODE_OBJECT && end.(*ast.Object).Object() == pyc.Object(pyc.None) {
				end = nil
			}

			var slice ast.Node
			if start == nil && end == nil {
				slice = ast.NewSlice(ast.SLICE0, nil, nil)
			} else if start == nil {
				slice = ast.NewSlice(ast.SLICE2, start, end)
			} else if end == nil {
				slice = ast.NewSlice(ast.SLICE1, start, end)
			} else {
				slice = ast.NewSlice(ast.SLICE3, start, end)
			}

			bld.curblock.Append(ast.NewStore(values, ast.NewSubscr(dest, slice)))
		}
	case COPY_A:
		{
			if bld.operand == 1 && bld.tryWalrus() {
				break
			}
			if bld.operand == 1 && !bld.source.AtEof() && bld.stack.Top(1) != nil && !bld.stack.Empty() {
				sSrc, sPos := bld.source.Pos(), bld.pos
				o2, a2 := pyc.BcNext(bld.source, bld.mod, &bld.pos)
				bld.source.SetPos(sSrc)
				bld.pos = sPos
				if o2 == STORE_FAST_A || o2 == STORE_NAME_A || o2 == STORE_GLOBAL_A ||
					o2 == STORE_DEREF_A || o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A ||
					o2 == LOAD_FAST_A || o2 == LOAD_NAME_A || o2 == LOAD_GLOBAL_A ||
					o2 == LOAD_DEREF_A {
					targetFollows := o2 != LOAD_FAST_A && o2 != LOAD_NAME_A &&
						o2 != LOAD_GLOBAL_A && o2 != LOAD_DEREF_A
					if targetFollows && o2 == STORE_NAME_A &&
						bld.code.GetName(a2).Value == "__classcell__" {
						targetFollows = false
					}
					if targetFollows && ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
						chainstore := bld.stack.Top(1)
						bld.stack.Pop()
						bld.stack.Push(bld.stack.Top(1))
						bld.stack.Push(chainstore)
						break
					}
					if targetFollows {
						bld.stack.Push(bld.stack.Top(1))
						bld.stack.Push(ast.NewChainStore(nil, bld.stack.Top(1)))
						break
					}
				}
			}
			value := bld.stack.Top(bld.operand)
			bld.stack.Push(value)
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
