package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsStack() flow {
	switch bld.opcode {
	case POP_TOP:
		{
			if len(bld.matches) != 0 && bld.matches[len(bld.matches)-1].cblock == bld.curblock &&
				bld.matches[len(bld.matches)-1].skipPop {
				bld.matches[len(bld.matches)-1].skipPop = false
				bld.matches[len(bld.matches)-1].guardOpen = false
				break
			}

			value := bld.stack.Top(1)
			bld.stack.Pop()

			if bld.mod.VerCompare(3, 12) >= 0 && value != nil {
				var fb *ast.Block
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					if bld.blocks[i].BlkType() == ast.BLK_FOR {
						fb = bld.blocks[i]
						break
					}
				}
				if fb != nil {
					if !fb.IsComprehension() && fb.Iter() == value {
						break
					}
				}
			}

			if isExcSentinel(value) {
				if bld.curblock.Inited() == 0 {
					bld.curblock.Init()
				}
				break
			}

			if bld.curblock.Inited() == 0 {
				if bld.curblock.BlkType() == ast.BLK_WITH {
					bld.curblock.SetExpr(value)
				} else {
					bld.curblock.Init()
				}
				break
			} else if value == nil || value.Processed() {
				break
			}

			bld.curblock.Append(value)

			if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.IsComprehension() {
				if ast.TypeOf(value) == ast.NODE_CALL {
					pparams := value.(*ast.Call).Pparams()
					if len(pparams) != 0 {
						bld.stack.Push(ast.NewComprehension(pparams[0]))
					}
				}
			}
		}
	case PRINT_ITEM:
		{
			var printNode *ast.Print
			if bld.curblock.Size() > 0 && ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_PRINT {
				printNode = bld.curblock.Nodes()[bld.curblock.Size()-1].(*ast.Print)
			}
			if printNode != nil && printNode.Stream() == nil && !printNode.Eol() {
				printNode.Add(bld.stack.Top(1))
			} else {
				bld.curblock.Append(ast.NewPrint(bld.stack.Top(1), nil))
			}
			bld.stack.Pop()
		}
	case PRINT_ITEM_TO:
		{
			stream := bld.stack.Top(1)
			bld.stack.Pop()

			var printNode *ast.Print
			if bld.curblock.Size() > 0 && ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_PRINT {
				printNode = bld.curblock.Nodes()[bld.curblock.Size()-1].(*ast.Print)
			}
			if printNode != nil && printNode.Stream() == stream && !printNode.Eol() {
				printNode.Add(bld.stack.Top(1))
			} else {
				bld.curblock.Append(ast.NewPrint(bld.stack.Top(1), stream))
			}
			bld.stack.Pop()
			if stream != nil {
				stream.SetProcessed()
			}
		}
	case PRINT_NEWLINE:
		{
			var printNode *ast.Print
			if bld.curblock.Size() > 0 && ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_PRINT {
				printNode = bld.curblock.Nodes()[bld.curblock.Size()-1].(*ast.Print)
			}
			if printNode != nil && printNode.Stream() == nil && !printNode.Eol() {
				printNode.SetEol(true)
			} else {
				bld.curblock.Append(ast.NewPrint(nil, nil))
			}
			bld.stack.Pop()
		}
	case PRINT_NEWLINE_TO:
		{
			stream := bld.stack.Top(1)
			bld.stack.Pop()

			var printNode *ast.Print
			if bld.curblock.Size() > 0 && ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_PRINT {
				printNode = bld.curblock.Nodes()[bld.curblock.Size()-1].(*ast.Print)
			}
			if printNode != nil && printNode.Stream() == stream && !printNode.Eol() {
				printNode.SetEol(true)
			} else {
				bld.curblock.Append(ast.NewPrint(nil, stream))
			}
			bld.stack.Pop()
			if stream != nil {
				stream.SetProcessed()
			}
		}
	case RAISE_VARARGS_A:
		{
			var paramList []ast.Node
			for i := 0; i < bld.operand; i++ {
				paramList = pushFront(paramList, bld.stack.Top(1))
				bld.stack.Pop()
			}
			bld.curblock.Append(ast.NewRaise(paramList))

			if (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELSE) &&
				len(bld.stackHist) != 0 && bld.blocks.size() > 1 && bld.mod.VerCompare(2, 6) >= 0 {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]

				prev := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(prev)
			}
		}
	case RERAISE, RERAISE_A:
	case RETURN_VALUE, INSTRUMENTED_RETURN_VALUE_A:
		if bld.finallySuppressReturns[bld.curpos] {
			break
		}
		{
			value := bld.stack.Top(1)
			bld.stack.Pop()
			bld.curblock.Append(ast.NewReturn(value, ast.RET_RETURN))

			if (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELSE) &&
				len(bld.stackHist) != 0 && bld.blocks.size() > 1 && bld.mod.VerCompare(2, 6) >= 0 {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]

				prev := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(prev)

				if !bld.source.AtEof() {
					savedSrc := bld.source.Pos()
					savedPos := bld.pos
					pkOp, pkOperand := pyc.BcNext(bld.source, bld.mod, &bld.pos)
					mul := 1
					if bld.mod.VerCompare(3, 10) >= 0 {
						mul = 2
					}
					jt := pkOperand * mul
					keepNext := !(pkOp == JUMP_FORWARD_A || (pkOp == JUMP_ABSOLUTE_A && jt > savedPos))
					if keepNext {
						bld.source.SetPos(savedSrc)
						bld.pos = savedPos
					} else {
						bld.opcode = pkOp
						bld.operand = pkOperand
					}
				}
			}
		}
	case RETURN_CONST_A, INSTRUMENTED_RETURN_CONST_A:
		{
			value := ast.NewObject(bld.code.GetConst(bld.operand))
			bld.curblock.Append(ast.NewReturn(value, ast.RET_RETURN))
		}
	case ROT_TWO:
		{
			{
				sSrc, sPos := bld.source.Pos(), bld.pos
				var o2, o3 int
				if !bld.source.AtEof() {
					o2, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
					if !bld.source.AtEof() {
						o3, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
					}
				}
				inFor := false
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					if bld.blocks[i].BlkType() == ast.BLK_FOR {
						inFor = !bld.blocks[i].IsComprehension()
						break
					}
				}
				if inFor && o2 == POP_TOP && o3 == RETURN_VALUE {
					bld.source.SetPos(sSrc)
					bld.pos = sPos
					pyc.BcNext(bld.source, bld.mod, &bld.pos)
					break
				}
				bld.source.SetPos(sSrc)
				bld.pos = sPos
			}
			{
				sSrc, sPos := bld.source.Pos(), bld.pos
				var o2 int
				storeNext := false
				if !bld.source.AtEof() {
					o2, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
					storeNext = o2 == STORE_FAST_A || o2 == STORE_NAME_A ||
						o2 == STORE_GLOBAL_A || o2 == STORE_DEREF_A ||
						o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A
				}
				bld.source.SetPos(sSrc)
				bld.pos = sPos
				if storeNext && bld.stack.Size() >= 2 && bld.stack.Top(2) != nil &&
					ast.TypeOf(bld.stack.Top(2)) != ast.NODE_CHAINSTORE &&
					bld.stack.Top(1) != nil &&
					ast.TypeOf(bld.stack.Top(1)) != ast.NODE_CHAINSTORE {
					bld.pendingOps = append(bld.pendingOps, intPair{SWAP_A, 2})
					break
				}
			}
			one := bld.stack.Top(1)
			bld.stack.Pop()
			if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				bld.stack.Pop()
			}
			two := bld.stack.Top(1)
			bld.stack.Pop()

			bld.stack.Push(one)
			bld.stack.Push(two)
		}
	case ROT_THREE:
		{
			if bld.unpack == 0 && bld.stack.Size() >= 3 && !bld.source.AtEof() {
				sSrc, sPos := bld.source.Pos(), bld.pos
				var o2, o3 int
				o2, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				if o2 == ROT_TWO && !bld.source.AtEof() {
					o3, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				}
				parallel := o2 == ROT_TWO &&
					(o3 == STORE_FAST_A || o3 == STORE_NAME_A || o3 == STORE_GLOBAL_A ||
						o3 == STORE_DEREF_A || o3 == STORE_ATTR_A || o3 == STORE_SUBSCR)
				for k := 1; parallel && k <= 3; k++ {
					parallel = bld.stack.Top(k) != nil && ast.TypeOf(bld.stack.Top(k)) != ast.NODE_CHAINSTORE
				}
				if parallel {
					bld.source.SetPos(sSrc)
					bld.pos = sPos
					pyc.BcNext(bld.source, bld.mod, &bld.pos)
					bld.unpack = 3
					values := make([]ast.Node, 3)
					for i := 0; i < 3; i++ {
						values[2-i] = bld.stack.Top(1)
						bld.stack.Pop()
					}
					tup := ast.NewTuple(values)
					tup.SetRequireParens(false)
					nextTup := ast.NewTuple(nil)
					nextTup.SetRequireParens(false)
					bld.stack.Push(tup)
					bld.stack.Push(nextTup)
					break
				}
				bld.source.SetPos(sSrc)
				bld.pos = sPos
			}
			one := bld.stack.Top(1)
			bld.stack.Pop()
			two := bld.stack.Top(1)
			bld.stack.Pop()
			if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				bld.stack.Pop()
			}
			three := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(one)
			bld.stack.Push(three)
			bld.stack.Push(two)
		}
	case ROT_FOUR:
		{
			one := bld.stack.Top(1)
			bld.stack.Pop()
			two := bld.stack.Top(1)
			bld.stack.Pop()
			three := bld.stack.Top(1)
			bld.stack.Pop()
			if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE {
				bld.stack.Pop()
			}
			four := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(one)
			bld.stack.Push(four)
			bld.stack.Push(three)
			bld.stack.Push(two)
		}
	case SET_LINENO_A:
	case SETUP_WITH_A:
		{
			withblock := ast.NewWithBlock(bld.pos + bld.operand)
			bld.blocks.push(withblock)
			bld.curblock = bld.blocks.top()
		}
	case WITH_EXCEPT_START:
		if !bld.stack.Empty() && isExcSentinel(bld.stack.Top(1)) {
			bld.stack.Pop()
		}
		bld.stack.Push(nil)
	case BEFORE_WITH, BEFORE_ASYNC_WITH:
		{
			if end, ok := bld.withBodyEndByBefore[bld.curpos]; ok {
				withblock := ast.NewWithBlock(end)
				bld.blocks.push(withblock)
				bld.curblock = bld.blocks.top()
			}
		}
	case WITH_CLEANUP, WITH_CLEANUP_START:
		{
			none := bld.stack.Top(1)
			bld.stack.Pop()

			if none != nil {
				fmt.Fprint(os.Stderr, "Something TERRIBLE happened!\n")
				break
			}

			if bld.curblock.BlkType() == ast.BLK_WITH && bld.curblock.End() == bld.curpos {
				with := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(with)
			} else {
				fmt.Fprintf(os.Stderr, "Something TERRIBLE happened! No matching with block found for WITH_CLEANUP at %d\n", bld.curpos)
			}
		}
	case WITH_CLEANUP_FINISH:
	case SETUP_EXCEPT_A:
		{
			if bld.curblock.BlkType() == ast.BLK_CONTAINER {
				bld.curblock.SetExcept(bld.pos + bld.operand)
			} else {
				next := ast.NewContainerBlock(0, bld.pos+bld.operand)
				bld.blocks.push(next)
			}

			bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			tryblock := ast.NewBlock(ast.BLK_TRY, bld.pos+bld.operand, 1)
			bld.blocks.push(tryblock)
			bld.curblock = bld.blocks.top()

			bld.needTry = false
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
