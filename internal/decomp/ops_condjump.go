package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsCondJump() flow {
	switch bld.opcode {
	case JUMP_IF_FALSE_A, JUMP_IF_TRUE_A, JUMP_IF_FALSE_OR_POP_A, JUMP_IF_TRUE_OR_POP_A,
		POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A, POP_JUMP_FORWARD_IF_FALSE_A,
		POP_JUMP_FORWARD_IF_TRUE_A, POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_FORWARD_IF_NOT_NONE_A,
		INSTRUMENTED_POP_JUMP_IF_FALSE_A, INSTRUMENTED_POP_JUMP_IF_TRUE_A:
		{
			if wend, ok := bld.whileGuardEnd[bld.curpos]; ok {
				wcond := bld.stack.Top(1)
				bld.stack.Pop()
				wneg := false
				if bld.opcode == POP_JUMP_FORWARD_IF_NONE_A {
					wcond = ast.NewCompare(wcond, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
				} else if bld.opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
					wcond = ast.NewCompare(wcond, ast.NewObject(pyc.None), ast.CMP_IS)
				} else if bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A || bld.opcode == POP_JUMP_IF_TRUE_A {
					wneg = true
				}
				if chain, ok := bld.loops.guardChain[bld.curpos]; ok && len(chain) == len(bld.guardChainConds) {
					res := wcond
					if wneg {
						res = ast.NewUnary(res, ast.UN_NOT)
					}
					wneg = false
					for ci := len(bld.guardChainConds) - 1; ci >= 0; ci-- {
						cj := chain[ci]
						c := bld.guardChainConds[ci]
						if cj.ifTrue != cj.toBody {
							c = ast.NewUnary(c, ast.UN_NOT)
						}
						bop := ast.BIN_LOG_AND
						if cj.toBody {
							bop = ast.BIN_LOG_OR
						}
						res = ast.NewBinary(c, res, bop)
					}
					wcond = res
				}
				bld.guardChainConds = nil
				wh := ast.NewCondBlock(ast.BLK_WHILE, wend, wcond, wneg)
				wh.InitV(ast.PRE_POPPED)
				bld.blocks.push(wh)
				bld.curblock = bld.blocks.top()
				break
			}

			cond := bld.stack.Top(1)
			var ifblk *ast.Block
			popped := ast.UNINITED

			if bld.opcode == POP_JUMP_FORWARD_IF_NONE_A || bld.opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
				bld.stack.Pop()
				cmpop := ast.CMP_IS
				if bld.opcode == POP_JUMP_FORWARD_IF_NONE_A {
					cmpop = ast.CMP_IS_NOT
				}
				cond = ast.NewCompare(cond, ast.NewObject(pyc.None), cmpop)
				popped = ast.PRE_POPPED
			}

			if len(bld.matches) != 0 && bld.matches[len(bld.matches)-1].cblock == bld.curblock &&
				bld.matches[len(bld.matches)-1].guardOpen && bld.curblock.Size() == 0 &&
				bld.opcode != JUMP_IF_FALSE_A && bld.opcode != JUMP_IF_TRUE_A &&
				bld.opcode != JUMP_IF_FALSE_OR_POP_A && bld.opcode != JUMP_IF_TRUE_OR_POP_A {
				mc := bld.matches[len(bld.matches)-1]
				gt := bld.operand
				if bld.mod.VerCompare(3, 10) >= 0 {
					gt *= 2
				}
				if bld.mod.VerCompare(3, 12) >= 0 || bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
					bld.opcode == POP_JUMP_FORWARD_IF_FALSE_A {
					gt += bld.pos
				}
				ni := bld.matcher.IndexOf(gt)
				var isGuard bool
				if mc.guardTarget >= 0 {
					isGuard = gt == mc.guardTarget
				} else if mc.cur.bodyEnd >= 0 {
					isGuard = gt == mc.cur.nextPos
				} else {
					isGuard = mc.cur.nonLast || gt == mc.endPos ||
						(ni >= 0 && bld.matcher.Instrs()[ni].op == NOP) ||
						(!mc.isDefault && mc.cblock.Size() == 0 && gt > bld.pos)
				}
				if isGuard {
					bld.stack.Pop()
					if bld.opcode == POP_JUMP_IF_TRUE_A || bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
						bld.opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A {
						cond = ast.NewUnary(cond, ast.UN_NOT)
					}
					prevGuard := mc.cblock.Guard()
					if prevGuard == nil {
						mc.cblock.SetGuard(cond)
					} else {
						mc.cblock.SetGuard(ast.NewBinary(prevGuard, cond, ast.BIN_LOG_AND))
					}
					if mc.guardTarget < 0 {
						mc.guardTarget = gt
						if mc.cur.bodyEnd < 0 {
							mc.cur.bodyEnd = gt
							mc.cur.nextPos = gt
							mc.endJumpPos = gt - 2
							mc.cblock.SetEnd(gt)
						}
					}
					break
				}
				mc.guardOpen = false
			}

			if bld.opcode == POP_JUMP_IF_FALSE_A || bld.opcode == POP_JUMP_IF_TRUE_A ||
				bld.opcode == POP_JUMP_FORWARD_IF_FALSE_A || bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
				bld.opcode == INSTRUMENTED_POP_JUMP_IF_FALSE_A || bld.opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A {
				bld.stack.Pop()
				popped = ast.PRE_POPPED
			}

			bld.stackHist = append(bld.stackHist, bld.stack.Copy())

			if bld.opcode == JUMP_IF_FALSE_OR_POP_A || bld.opcode == JUMP_IF_TRUE_OR_POP_A {
				bld.stack.Pop()
				popped = ast.POPPED
			}

			neg := bld.opcode == JUMP_IF_TRUE_A || bld.opcode == JUMP_IF_TRUE_OR_POP_A ||
				bld.opcode == POP_JUMP_IF_TRUE_A || bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
				bld.opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A

			offs := bld.operand
			if bld.mod.VerCompare(3, 10) >= 0 {
				offs *= 2
			}
			if bld.mod.VerCompare(3, 12) >= 0 || bld.opcode == JUMP_IF_FALSE_A || bld.opcode == JUMP_IF_TRUE_A ||
				bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A || bld.opcode == POP_JUMP_FORWARD_IF_FALSE_A ||
				bld.opcode == POP_JUMP_FORWARD_IF_NONE_A || bld.opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
				base := bld.pos
				if bld.mod.VerCompare(3, 13) >= 0 {
					sp, pp := bld.source.Pos(), bld.pos
					for !bld.source.AtEof() {
						p3 := pp
						o3, _ := pyc.BcNext(bld.source, bld.mod, &p3)
						if o3 != CACHE {
							break
						}
						pp = p3
					}
					bld.source.SetPos(sp)
					base = pp
				}
				offs += base
			}

			if t, ok := bld.xflow.cleanupDest[offs]; ok {
				offs = t
			}

			if ast.TypeOf(cond) == ast.NODE_COMPARE && cond.(*ast.Binary).Op() == ast.CMP_EXCEPTION {
				exceptEnd := offs
				if bld.curblock.BlkType() == ast.BLK_EXCEPT && bld.curblock.Cond() == nil {
					if offs <= bld.pos {
						exceptEnd = bld.curblock.End()
					}
					bld.blocks.pop()
					bld.curblock = bld.blocks.top()

					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				}
				ifblk = ast.NewCondBlock(ast.BLK_EXCEPT, exceptEnd, cond.(*ast.Binary).Right(), false)
			} else if bld.curblock.BlkType() == ast.BLK_ELSE && bld.curblock.Size() == 0 {
				bld.blocks.pop()
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				ifblk = ast.NewCondBlock(ast.BLK_ELIF, offs, cond, neg)
			} else if bld.curblock.Size() == 0 && bld.curblock.Inited() == 0 && bld.curblock.BlkType() == ast.BLK_WHILE {
				top := bld.blocks.top()
				bld.blocks.pop()
				ifblk = ast.NewCondBlock(top.BlkType(), offs, cond, neg)
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			} else if bld.curblock.Size() == 0 && bld.curblock.End() <= offs &&
				(bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF || bld.curblock.BlkType() == ast.BLK_WHILE) &&
				bld.blocks.size() > 1 {
				var newcond ast.Node
				top := bld.curblock
				cond1 := top.Cond()
				bld.blocks.pop()

				if bld.curblock.BlkType() == ast.BLK_WHILE {
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				} else {
					sTop := bld.stackHist[len(bld.stackHist)-1]
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-2]
					bld.stackHist = append(bld.stackHist, sTop)
				}

				if bld.curblock.End() == offs || (bld.curblock.End() == bld.curpos && !top.Negative()) {
					newcond = ast.NewBinary(cond1, cond, ast.BIN_LOG_AND)
				} else {
					newcond = ast.NewBinary(cond1, cond, ast.BIN_LOG_OR)
				}
				ifblk = ast.NewCondBlock(top.BlkType(), offs, newcond, neg)
			} else if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.IsComprehension() && bld.mod.VerCompare(2, 7) >= 0 {
				bld.curblock.SetCondition(cond)
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				break
			} else {
				ifblk = ast.NewCondBlock(ast.BLK_IF, offs, cond, neg)
			}

			if popped != 0 {
				ifblk.InitV(popped)
			}

			bld.blocks.push(ifblk)
			bld.curblock = bld.blocks.top()
		}
	case POP_JUMP_BACKWARD_IF_TRUE_A, POP_JUMP_BACKWARD_IF_FALSE_A, POP_JUMP_BACKWARD_IF_NONE_A,
		POP_JUMP_BACKWARD_IF_NOT_NONE_A:
		{
			if bld.whileBackedges[bld.curpos] {
				hasWhile := false
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					if bld.blocks[i].BlkType() == ast.BLK_WHILE {
						hasWhile = true
						break
					}
				}
				if hasWhile {
					if !bld.stack.Empty() {
						bld.stack.Pop()
					}
					for bld.blocks.size() > 1 && bld.curblock.BlkType() != ast.BLK_WHILE {
						inner := bld.curblock
						bld.blocks.pop()
						bld.curblock = bld.blocks.top()
						bld.curblock.Append(inner)
					}
					if bld.curblock.BlkType() == ast.BLK_WHILE && bld.blocks.size() > 1 {
						wh := bld.curblock
						bld.blocks.pop()
						bld.curblock = bld.blocks.top()
						bld.curblock.Append(wh)
					}
					break
				}
			}

			cond := bld.stack.Top(1)
			bld.stack.Pop()

			neg := false
			if bld.opcode == POP_JUMP_BACKWARD_IF_NONE_A {
				cond = ast.NewCompare(cond, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
			} else if bld.opcode == POP_JUMP_BACKWARD_IF_NOT_NONE_A {
				cond = ast.NewCompare(cond, ast.NewObject(pyc.None), ast.CMP_IS)
			} else if bld.opcode == POP_JUMP_BACKWARD_IF_TRUE_A {
				neg = true
			}

			end := bld.curblock.End()

			bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			ifblk := ast.NewCondBlock(ast.BLK_IF, end, cond, neg)
			ifblk.InitV(ast.PRE_POPPED)
			bld.blocks.push(ifblk)
			bld.curblock = bld.blocks.top()
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
