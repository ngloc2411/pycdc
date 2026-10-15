package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/decomp/ast"
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

func (bld *builder) jumpForwardImpl() {
	for (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
		bld.curblock.End() == bld.curpos && bld.blocks.size() > 1 {
		fin := bld.curblock
		bld.blocks.pop()
		bld.curblock = bld.blocks.top()
		bld.curblock.Append(fin)
		if len(bld.stackHist) != 0 {
			bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
		}
	}

	offs := bld.operand
	if bld.mod.VerCompare(3, 10) >= 0 {
		offs *= 2
	}

	if bld.curblock.BlkType() == ast.BLK_CONTAINER {
		if bld.curblock.HasExcept() {
			bld.stackHist = append(bld.stackHist, bld.stack.Copy())

			bld.curblock.SetEnd(bld.pos + offs)
			except := ast.NewCondBlock(ast.BLK_EXCEPT, bld.pos+offs, nil, false)
			except.Init()
			bld.blocks.push(except)
			bld.curblock = bld.blocks.top()
		}
		return
	}

	if len(bld.stackHist) != 0 {
		if bld.stack.Empty() {
			bld.stack = bld.stackHist[len(bld.stackHist)-1]
		}
		bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
	}

	prev := bld.curblock
	push := true

	for {
		if bld.blocks.size() <= 1 {
			cleanBuild = false
			break
		}
		bld.blocks.pop()

		if !bld.blocks.empty() {
			bld.blocks.top().Append(prev)
		}

		if prev.BlkType() == ast.BLK_IF || prev.BlkType() == ast.BLK_ELIF {
			if offs == 0 {
				prev = nil
				break
			}

			if push {
				bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			}
			next := ast.NewBlock(ast.BLK_ELSE, bld.pos+offs, 0)
			if prev.Inited() == ast.PRE_POPPED {
				next.InitV(ast.PRE_POPPED)
			}

			bld.blocks.push(next)
			prev = nil
		} else if prev.BlkType() == ast.BLK_EXCEPT {
			if offs == 0 {
				prev = nil
				break
			}

			if push {
				bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			}
			next := ast.NewCondBlock(ast.BLK_EXCEPT, bld.pos+offs, nil, false)
			next.Init()

			bld.blocks.push(next)
			prev = nil
		} else if prev.BlkType() == ast.BLK_ELSE {
			prev = bld.blocks.top()
			if !push {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			}
			push = false

			if prev.BlkType() == ast.BLK_MAIN {
				prev = nil
			}
		} else if prev.BlkType() == ast.BLK_TRY && prev.End() < bld.pos+offs {
			if len(bld.stackHist) != 0 {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			}

			if bld.blocks.top().BlkType() == ast.BLK_CONTAINER {
				cont := bld.blocks.top()
				if cont.HasExcept() {
					if push {
						bld.stackHist = append(bld.stackHist, bld.stack.Copy())
					}

					except := ast.NewCondBlock(ast.BLK_EXCEPT, bld.pos+offs, nil, false)
					except.Init()
					bld.blocks.push(except)
				}
			} else {
				fmt.Fprint(os.Stderr, "Something TERRIBLE happened!!\n")
			}
			prev = nil
		} else {
			prev = nil
		}

		if prev == nil {
			break
		}
	}

	if !bld.blocks.empty() {
		bld.curblock = bld.blocks.top()
		if bld.curblock.BlkType() == ast.BLK_EXCEPT {
			bld.curblock.SetEnd(bld.pos + offs)
		}
	}
}

func (bld *builder) opsJump() flow {
	switch bld.opcode {
	case JUMP_ABSOLUTE_A, JUMP_BACKWARD_A, JUMP_BACKWARD_NO_INTERRUPT_A:
		{
			offs := bld.operand
			if bld.mod.VerCompare(3, 10) >= 0 {
				offs *= 2
			}

			if len(bld.spliceStack) != 0 && bld.opcode == JUMP_BACKWARD_A &&
				bld.curpos >= bld.spliceStack[len(bld.spliceStack)-1].handler && bld.pos <= bld.spliceStack[len(bld.spliceStack)-1].regionEnd {
				sc := bld.spliceStack[len(bld.spliceStack)-1]
				dest := bld.pos - bld.operand*2
				if dest == sc.resume || (sc.backDest >= 0 && dest == sc.backDest) {
					bld.operand = (sc.regionEnd - bld.pos) / 2
					bld.opcode = JUMP_FORWARD_A
					bld.jumpForwardImpl()
					break
				}
			}
			if tej, ok := bld.loops.tryEndJump[bld.curpos]; ok &&
				(bld.opcode == JUMP_ABSOLUTE_A || bld.opcode == JUMP_BACKWARD_A) {
				rel := tej - bld.pos
				if bld.mod.VerCompare(3, 10) >= 0 {
					bld.operand = rel / 2
				} else {
					bld.operand = rel
				}
				bld.opcode = JUMP_FORWARD_A
				bld.jumpForwardImpl()
				break
			}

			if bld.mod.VerCompare(3, 8) >= 0 && bld.mod.VerCompare(3, 11) < 0 &&
				offs < bld.pos &&
				(bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
				bld.pos == bld.curblock.End() {
				loopOwns := false
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					bt := bld.blocks[i].BlkType()
					if bt == ast.BLK_FOR {
						loopOwns = bld.blocks[i].End() == bld.pos && !bld.blocks[i].IsComprehension() &&
							offs == bld.blocks[i].Start()
						break
					}
					if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
						break
					}
				}
				for loopOwns && (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
					bld.blocks.size() > 1 {
					if len(bld.stackHist) != 0 {
						bld.stack = bld.stackHist[len(bld.stackHist)-1]
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
					}
					inner := bld.curblock
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()
					bld.curblock.Append(inner)
				}
			}

			if len(bld.inlineComps) != 0 && offs < bld.pos &&
				(bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
				insideComprehensionAdd(bld.blocks) {
				break
			}

			if offs < bld.pos {
				if (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
					!bld.stack.Empty() && bld.stack.Top(1) != nil &&
					ast.TypeOf(bld.stack.Top(1)) == ast.NODE_COMPREHENSION {
					ancestorIsCompFor := false
					for i := len(bld.blocks) - 1; i >= 0; i-- {
						bt := bld.blocks[i].BlkType()
						if bt == ast.BLK_FOR {
							ancestorIsCompFor = bld.blocks[i].IsComprehension()
							break
						}
						if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
							break
						}
					}
					if ancestorIsCompFor {
						for (bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) &&
							bld.blocks.size() > 1 {
							ifb := bld.curblock
							c := ifb.Cond()
							if ifb.Negative() {
								c = ast.NewUnary(c, ast.UN_NOT)
							}
							bld.blocks.pop()
							bld.curblock = bld.blocks.top()
							if bld.curblock.BlkType() == ast.BLK_FOR {
								existing := bld.curblock.Condition()
								var combined ast.Node
								if existing == nil {
									combined = c
								} else {
									combined = ast.NewBinary(existing, c, ast.BIN_LOG_AND)
								}
								bld.curblock.SetCondition(combined)
							}
							if len(bld.stackHist) != 0 {
								bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
							}
						}
					}
				}

				if bld.curblock.BlkType() == ast.BLK_FOR {
					var target int
					if bld.mod.VerCompare(3, 11) >= 0 {
						target = bld.pos - offs
					} else {
						target = offs
					}
					isJumpToStart := target == bld.curblock.Start()
					atLoopEnd := bld.curblock.End() != 0 && bld.pos == bld.curblock.End()
					if !atLoopEnd && isJumpToStart && bld.curblock.End() > bld.pos &&
						!bld.curblock.IsComprehension() {
						look := pyc.NewBuffer([]byte(bld.code.Code.Value))
						look.SetPos(bld.pos)
						lp := bld.pos
						onlyEpilogue, sawReturn := true, false
						for lp < bld.curblock.End() && !look.AtEof() {
							lop, _ := pyc.BcNext(look, bld.mod, &lp)
							if lop == RETURN_VALUE || lop == RETURN_CONST_A {
								sawReturn = true
							} else if lop != LOAD_CONST_A && lop != NOP && lop != CACHE {
								onlyEpilogue = false
							}
						}
						if onlyEpilogue && sawReturn && lp == bld.curblock.End() {
							atLoopEnd = true
						}
					}
					shouldPopForBlock := bld.curblock.IsComprehension()
					shouldAddForBlock := bld.mod.MajorVer() == 3 && bld.mod.MinorVer() >= 8 &&
						isJumpToStart && atLoopEnd && !bld.curblock.IsComprehension()

					if !shouldPopForBlock && !shouldAddForBlock &&
						isJumpToStart && !atLoopEnd && !bld.curblock.IsComprehension() {
						bld.curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
						break
					}

					if shouldPopForBlock || shouldAddForBlock {
						top := bld.stack.Top(1)

						if ast.TypeOf(top) == ast.NODE_COMPREHENSION {
							top.(*ast.Comprehension).AddGenerator(bld.curblock)
						}

						tmp := bld.curblock
						bld.blocks.pop()
						bld.curblock = bld.blocks.top()
						if shouldAddForBlock {
							bld.curblock.Append(tmp)
						}
					}
				} else if bld.curblock.BlkType() == ast.BLK_ELSE {
					if bld.mod.VerCompare(3, 11) >= 0 {
						loopEnd := -1
						for i := len(bld.blocks) - 1; i >= 0; i-- {
							bt := bld.blocks[i].BlkType()
							if bt == ast.BLK_FOR || bt == ast.BLK_WHILE {
								loopEnd = bld.blocks[i].End()
								break
							}
						}
						if loopEnd > 0 && bld.pos < loopEnd {
							bld.curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
							break
						}
					}
					if len(bld.stackHist) != 0 {
						bld.stack = bld.stackHist[len(bld.stackHist)-1]
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
					}

					bld.blocks.pop()
					bld.blocks.top().Append(bld.curblock)
					bld.curblock = bld.blocks.top()

					if bld.curblock.BlkType() == ast.BLK_CONTAINER && !bld.curblock.HasFinally() {
						bld.blocks.pop()
						bld.blocks.top().Append(bld.curblock)
						bld.curblock = bld.blocks.top()
					}
				} else {
					bld.curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
				}

				break
			}

			if bld.curblock.BlkType() == ast.BLK_CONTAINER {
				if bld.curblock.HasExcept() && bld.pos < bld.curblock.Except() {
					except := ast.NewCondBlock(ast.BLK_EXCEPT, 0, nil, false)
					except.Init()
					bld.blocks.push(except)
					bld.curblock = bld.blocks.top()
				}
				break
			}

			if len(bld.stackHist) != 0 {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			} else {
				fmt.Fprint(os.Stderr, "Warning: Stack history is empty, something wrong might have happened\n")
			}

			if bld.blocks.size() == 1 && !bld.source.AtEof() {
				fmt.Fprintf(os.Stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", bld.pos, bld.opcode&0xff)
				cleanBuild = false
				break
			}

			prev := bld.curblock
			push := true

			for {
				if bld.blocks.size() == 1 {
					fmt.Fprintf(os.Stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", bld.pos, bld.opcode&0xff)
					cleanBuild = false
					break
				}
				bld.blocks.pop()

				bld.blocks.top().Append(prev)

				if prev.BlkType() == ast.BLK_IF || prev.BlkType() == ast.BLK_ELIF {
					if push {
						bld.stackHist = append(bld.stackHist, bld.stack.Copy())
					}
					next := ast.NewBlock(ast.BLK_ELSE, bld.blocks.top().End(), 0)
					if prev.Inited() == ast.PRE_POPPED {
						next.InitV(ast.PRE_POPPED)
					}

					bld.blocks.push(next)
					prev = nil
				} else if prev.BlkType() == ast.BLK_EXCEPT {
					if push {
						bld.stackHist = append(bld.stackHist, bld.stack.Copy())
					}
					next := ast.NewCondBlock(ast.BLK_EXCEPT, bld.blocks.top().End(), nil, false)
					next.Init()

					bld.blocks.push(next)
					prev = nil
				} else if prev.BlkType() == ast.BLK_ELSE {
					prev = bld.blocks.top()
					if !push {
						bld.stack = bld.stackHist[len(bld.stackHist)-1]
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
					}
					push = false
				} else {
					prev = nil
				}

				if prev == nil {
					break
				}
			}

			bld.curblock = bld.blocks.top()
		}
	case JUMP_FORWARD_A, INSTRUMENTED_JUMP_FORWARD_A:
		bld.jumpForwardImpl()
	case CALL_INTRINSIC_1_A:
		switch bld.operand {
		case 2:
			bld.pendingOps = append(bld.pendingOps, intPair{IMPORT_STAR, 0})
		case 3, 4:
		case 5:
			arg := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewUnary(arg, ast.UN_POSITIVE))
		case 6:
			bld.pendingOps = append(bld.pendingOps, intPair{LIST_TO_TUPLE, 0})
		default:
			fmt.Fprintf(os.Stderr, "Unsupported CALL_INTRINSIC_1 operand %d\n", bld.operand)
		}
	case LIST_TO_TUPLE:
		{
			list := bld.stack.Top(1)
			bld.stack.Pop()
			if ast.TypeOf(list) == ast.NODE_LIST {
				lv := list.(*ast.List).Values()
				tv := make([]ast.Node, len(lv))
				copy(tv, lv)
				bld.stack.Push(ast.NewTuple(tv))
			} else {
				bld.stack.Push(list)
			}
		}
	case RETURN_GENERATOR:
		bld.stack.Push(nil)
	case MAP_ADD_A:
		{
			value := bld.stack.Top(1)
			bld.stack.Pop()
			key := bld.stack.Top(1)
			bld.stack.Pop()

			if len(bld.inlineComps) != 0 && bld.curblock.BlkType() != ast.BLK_FOR {
				m := ast.NewMap()
				m.Add(key, value)
				bld.curblock.Append(ast.NewReturn(m, ast.RET_YIELD))
			} else if insideComprehensionAdd(bld.blocks) {
				bld.stack.Pop()
				m := ast.NewMap()
				m.Add(key, value)
				bld.stack.Push(ast.NewComprehension(m))
			} else {
				mp := bld.stack.Top(1)
				if ast.TypeOf(mp) == ast.NODE_MAP {
					mp.(*ast.Map).Add(key, value)
				}
			}
		}
	case SET_ADD_A:
		{
			value := bld.stack.Top(1)
			bld.stack.Pop()

			set := bld.stack.Top(1)

			if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.IsComprehension() {
				bld.stack.Pop()
				sc := ast.NewComprehension(value)
				sc.SetKind(ast.KIND_SET)
				bld.stack.Push(sc)
			} else {
				cn := ""
				if bld.code.Name != nil {
					cn = bld.code.Name.Value
				}
				if (bld.code.Name != nil && cn == "<setcomp>") || len(bld.inlineComps) != 0 {
					bld.curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
				} else {
					if ast.TypeOf(set) == ast.NODE_SET {
						old := set.(*ast.Set).Values()
						vals := make([]ast.Node, len(old), len(old)+1)
						copy(vals, old)
						vals = append(vals, value)
						bld.stack.Pop()
						bld.stack.Push(ast.NewSet(vals))
					} else {
						bld.stack.Push(ast.NewSubscr(set, value))
					}
				}
			}
		}
	case LIST_APPEND, LIST_APPEND_A:
		{
			value := bld.stack.Top(1)
			bld.stack.Pop()

			list := bld.stack.Top(1)

			if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.IsComprehension() {
				bld.stack.Pop()
				bld.stack.Push(ast.NewComprehension(value))
			} else {
				cn := ""
				if bld.code.Name != nil {
					cn = bld.code.Name.Value
				}
				if (bld.code.Name != nil && cn == "<listcomp>") || len(bld.inlineComps) != 0 {
					bld.curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
				} else {
					if ast.TypeOf(list) == ast.NODE_LIST {
						old := list.(*ast.List).Values()
						vals := make([]ast.Node, len(old), len(old)+1)
						copy(vals, old)
						vals = append(vals, value)
						bld.stack.Pop()
						bld.stack.Push(ast.NewList(vals))
					} else {
						bld.stack.Push(ast.NewSubscr(list, value))
					}
				}
			}
		}
	case SET_UPDATE_A:
		{
			rhs := bld.stack.Top(1)
			bld.stack.Pop()
			lhs, _ := bld.stack.Top(1).(*ast.Set)
			bld.stack.Pop()

			var result []ast.Node
			if lhs != nil {
				result = append(result, lhs.Values()...)
			}
			var obj pyc.Object
			if ast.TypeOf(rhs) == ast.NODE_OBJECT {
				obj = rhs.(*ast.Object).Object()
			}
			if obj != nil && obj.Type() == pyc.TypeFrozenSet {
				for _, it := range obj.(*pyc.Seq).Values() {
					result = append(result, ast.NewObject(it))
				}
			} else if ast.TypeOf(rhs) == ast.NODE_SET {
				result = append(result, rhs.(*ast.Set).Values()...)
			} else {
				result = append(result, ast.NewUnary(rhs, ast.UN_UNPACK))
			}

			bld.stack.Push(ast.NewSet(result))
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
