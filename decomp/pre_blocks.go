package decomp

import (
	"math"

	"github.com/ngloc2411/pycdc/decomp/ast"
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

func (bld *builder) stepBlocks() flow {
	if bld.curblock.BlkType() == ast.BLK_WITH && bld.curblock.End() != 0 && bld.curblock.End() <= bld.pos && bld.blocks.size() > 1 {
		with := bld.curblock
		bld.blocks.pop()
		bld.curblock = bld.blocks.top()
		bld.curblock.Append(with)
		if r, ok := bld.withResumeByBodyEnd[with.End()]; ok {
			bld.withSkipUntil = r
		}
	}
	if bld.curblock.BlkType() != ast.BLK_FINALLY {
		finEnds := false
		for i := len(bld.blocks) - 1; i >= 0; i-- {
			b := bld.blocks[i]
			if b.BlkType() == ast.BLK_FINALLY {
				finEnds = b.End() != 0 && b.End() <= bld.pos
				break
			}
			if b.BlkType() == ast.BLK_MAIN {
				break
			}
		}
		for finEnds && bld.curblock.BlkType() != ast.BLK_FINALLY && bld.blocks.size() > 1 {
			inner := bld.curblock
			if inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF ||
				inner.BlkType() == ast.BLK_ELSE || inner.BlkType() == ast.BLK_EXCEPT {
				if len(bld.stackHist) != 0 {
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				}
			}
			bld.blocks.pop()
			bld.curblock = bld.blocks.top()
			if inner.BlkType() != ast.BLK_EXCEPT || inner.Size() != 0 || inner.Cond() != nil {
				bld.curblock.Append(inner)
			}
		}
	}
	if bld.curblock.BlkType() == ast.BLK_FINALLY && bld.curblock.End() != 0 && bld.curblock.End() <= bld.pos && bld.blocks.size() > 1 {
		finEnd := bld.curblock.End()
		final := bld.curblock
		bld.blocks.pop()
		bld.curblock = bld.blocks.top()
		bld.curblock.Append(final)
		if bld.curblock.BlkType() == ast.BLK_CONTAINER && bld.blocks.size() > 1 {
			cont := bld.curblock
			bld.blocks.pop()
			bld.curblock = bld.blocks.top()
			bld.curblock.Append(cont)
		}
		if r, ok := bld.finallyResumeByEnd[finEnd]; ok {
			bld.withSkipUntil = r
		}
	}
	if bld.withSkipUntil > 0 {
		if bld.pos < bld.withSkipUntil {
			bld.curpos = bld.pos
			bld.opcode, bld.operand = pyc.BcNext(bld.source, bld.mod, &bld.pos)
			return flowContinue
		}
		bld.withSkipUntil = 0
	}
	if len(bld.spliceDone) != 0 {
		if end, ok := bld.spliceDone[bld.pos]; ok {
			bld.pos = end
			bld.source.SetPos(bld.pos)
			return flowContinue
		}
	}
	if len(bld.spliceByEnd) != 0 {
		if sp, ok := bld.spliceByEnd[bld.pos]; ok {
			handlerPos := sp[0]
			tryOpen := false
			for i := len(bld.blocks) - 1; i >= 0; i-- {
				b := bld.blocks[i]
				if b.BlkType() == ast.BLK_TRY {
					tryOpen = b.End() == handlerPos
					break
				}
				if b.BlkType() != ast.BLK_IF && b.BlkType() != ast.BLK_ELIF && b.BlkType() != ast.BLK_ELSE &&
					b.BlkType() != ast.BLK_FOR && b.BlkType() != ast.BLK_WHILE && b.BlkType() != ast.BLK_WITH {
					break
				}
			}
			if tryOpen {
				var ctx spliceCtx
				ctx.resume = bld.pos
				ctx.handler = handlerPos
				ctx.regionEnd = sp[1]
				ctx.backDest = -1
				{
					sSrc := bld.source.Pos()
					p2 := bld.pos
					o, a := pyc.BcNext(bld.source, bld.mod, &p2)
					if o == JUMP_BACKWARD_A {
						ctx.backDest = p2 - a*2
					}
					bld.source.SetPos(sSrc)
				}
				delete(bld.spliceByEnd, bld.pos)
				bld.spliceStack = append(bld.spliceStack, ctx)
				bld.pos = handlerPos
				bld.source.SetPos(bld.pos)
				bld.nextExceptionEntry = 0
				return flowContinue
			}
		}
	}
	if sit, ok := bld.shadowedTryStarts[bld.pos]; ok {
		for _, T := range sit {
			open := false
			for i := len(bld.blocks) - 1; i >= 0; i-- {
				if bld.blocks[i].BlkType() == ast.BLK_TRY && bld.blocks[i].End() == T {
					open = true
					break
				}
			}
			if open {
				continue
			}
			cont := ast.NewContainerBlock(0, T)
			bld.blocks.push(cont)
			bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			tryblock := ast.NewBlock(ast.BLK_TRY, T, 1)
			bld.blocks.push(tryblock)
			bld.curblock = bld.blocks.top()
		}
	}
	if bld.curblock.BlkType() != ast.BLK_TRY {
		tryEndsHere := false
		for i := len(bld.blocks) - 1; i >= 0; i-- {
			b := bld.blocks[i]
			if b.BlkType() == ast.BLK_TRY {
				tryEndsHere = b.End() == bld.pos
				break
			}
			if b.BlkType() != ast.BLK_IF && b.BlkType() != ast.BLK_ELIF && b.BlkType() != ast.BLK_ELSE &&
				b.BlkType() != ast.BLK_FOR && b.BlkType() != ast.BLK_WHILE && b.BlkType() != ast.BLK_WITH {
				break
			}
		}
		if tryEndsHere {
			for bld.curblock.BlkType() != ast.BLK_TRY && bld.blocks.size() > 1 {
				inner := bld.curblock
				if inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF || inner.BlkType() == ast.BLK_ELSE {
					if len(bld.stackHist) != 0 {
						bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
					}
				}
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(inner)
			}
		}
	}
	if len(bld.matches) != 0 && bld.matches[len(bld.matches)-1].cblock != nil &&
		bld.blocks.size() >= bld.matches[len(bld.matches)-1].cdepth {
		mc := bld.matches[len(bld.matches)-1]
		if mc.endJumpPos >= 0 && bld.pos == mc.endJumpPos {
			ji := bld.matcher.IndexOf(bld.pos)
			endsCase := ji >= 0 && bld.matcher.Instrs()[ji].op == JUMP_FORWARD_A &&
				bld.matcher.JumpTarget(bld.matcher.Instrs()[ji]) >= mc.cur.bodyEnd
			lim := bld.pos
			if endsCase {
				lim = math.MaxInt32
			}
			bld.closeNested(mc, lim)
			if bld.blocks.size() == mc.cdepth && ji >= 0 && bld.matcher.Instrs()[ji].op == JUMP_FORWARD_A {
				target := bld.matcher.JumpTarget(bld.matcher.Instrs()[ji])
				if target >= mc.cur.bodyEnd {
					if mc.endPos < 0 {
						mc.endPos = target
					}
					bld.pos = bld.matcher.Instrs()[ji].end
					bld.source.SetPos(bld.pos)
					return flowContinue
				}
			}
		}
		if (mc.cur.bodyEnd >= 0 && bld.pos >= mc.cur.bodyEnd) ||
			(mc.cur.bodyEnd < 0 && mc.endPos >= 0 && bld.pos >= mc.endPos) {
			bld.endCase(mc)
			return flowContinue
		}
	}
	if _, ok := bld.loops.trueOpen[bld.pos]; ok && !bld.trueOpened[bld.pos] {
		bld.trueOpened[bld.pos] = true
		wh := ast.NewCondBlock(ast.BLK_WHILE, bld.loops.trueOpen[bld.pos], ast.NewObject(pyc.True), false)
		wh.InitV(ast.PRE_POPPED)
		bld.blocks.push(wh)
		bld.curblock = bld.blocks.top()
	}
	for bld.nextExceptionEntry < len(bld.exceptionEntries) && bld.exceptionEntries[bld.nextExceptionEntry].StartOffset < bld.pos {
		bld.nextExceptionEntry++
	}
	if bld.nextExceptionEntry < len(bld.exceptionEntries) {
		entry := bld.exceptionEntries[bld.nextExceptionEntry]
		if entry.StartOffset == bld.pos && !entry.PushLasti {
			if fend, ok := bld.finallyTryEndByStart[entry.StartOffset]; ok {
				finEnd := bld.finallyEndByStart[entry.StartOffset]
				cont := ast.NewContainerBlock(finEnd, 0)
				bld.blocks.push(cont)
				bld.curblock = bld.blocks.top()

				bld.stackHist = append(bld.stackHist, bld.stack.Copy())
				tryblock := ast.NewBlock(ast.BLK_TRY, fend, 1)
				bld.blocks.push(tryblock)
				bld.curblock = bld.blocks.top()
				bld.nextExceptionEntry++
			} else {
				sameTryOpen := false
				for i := len(bld.blocks) - 1; i >= 0; i-- {
					if bld.blocks[i].BlkType() == ast.BLK_TRY && bld.blocks[i].End() == entry.Target {
						sameTryOpen = true
						break
					}
				}
				if sameTryOpen {
					bld.nextExceptionEntry++
				} else {
					if bld.curblock.BlkType() == ast.BLK_CONTAINER {
						bld.curblock.SetExcept(entry.Target)
					} else {
						next := ast.NewContainerBlock(0, entry.Target)
						bld.blocks.push(next)
						bld.curblock = bld.blocks.top()
					}

					bld.stackHist = append(bld.stackHist, bld.stack.Copy())
					tryblock := ast.NewBlock(ast.BLK_TRY, entry.Target, 1)
					bld.blocks.push(tryblock)
					bld.curblock = bld.blocks.top()
					bld.nextExceptionEntry++
				}
			}
		}
	}
	if bld.curblock.BlkType() == ast.BLK_TRY && bld.curblock.End() == bld.pos && bld.blocks.size() > 1 {
		prev := bld.curblock
		bld.blocks.pop()
		bld.curblock = bld.blocks.top()

		if bld.curblock.BlkType() == ast.BLK_CONTAINER && bld.curblock.HasExcept() {
			if len(bld.stackHist) != 0 {
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			}

			bld.curblock.Append(prev)
			bld.stackHist = append(bld.stackHist, bld.stack.Copy())

			except := ast.NewCondBlock(ast.BLK_EXCEPT, 0, nil, false)
			except.Init()
			bld.blocks.push(except)
			bld.curblock = bld.blocks.top()
		} else if bld.curblock.BlkType() == ast.BLK_CONTAINER && bld.curblock.HasFinally() {
			finEnd := bld.curblock.Finally()
			if len(bld.stackHist) != 0 {
				if len(bld.finallySuppressReturns) != 0 {
					if _, ok := bld.finallyResumeByEnd[finEnd]; ok &&
						bld.stack.Size() > bld.stackHist[len(bld.stackHist)-1].Size() && bld.stack.Top(1) != nil {
						isRetStyle := false
						for st := range bld.finallyReturnStarts {
							if bld.finallyEndByStart[st] == finEnd {
								isRetStyle = true
							}
						}
						if isRetStyle {
							prev.Append(ast.NewReturn(bld.stack.Top(1), ast.RET_RETURN))
						}
					}
				}
				bld.stack = bld.stackHist[len(bld.stackHist)-1]
				bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
			}

			bld.curblock.Append(prev)

			final := ast.NewBlock(ast.BLK_FINALLY, finEnd, 1)
			final.Init()
			bld.blocks.push(final)
			bld.curblock = bld.blocks.top()
		} else {
			bld.blocks.push(prev)
			bld.curblock = prev
		}
	}
	if bld.curblock.BlkType() == ast.BLK_EXCEPT && bld.curblock.End() == bld.pos && bld.blocks.size() > 1 {
		prev := bld.curblock
		bld.blocks.pop()
		bld.curblock = bld.blocks.top()

		if len(bld.stackHist) != 0 {
			bld.stack = bld.stackHist[len(bld.stackHist)-1]
			bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
		}

		if prev.Size() != 0 || (prev.BlkType() == ast.BLK_EXCEPT && prev.Cond() != nil) {
			bld.curblock.Append(prev)
		}

		if bld.curblock.BlkType() == ast.BLK_CONTAINER && !bld.curblock.HasFinally() {
			cont := bld.curblock
			bld.blocks.pop()
			bld.curblock = bld.blocks.top()
			bld.curblock.Append(cont)
		}
	}
	return flowNext
}
