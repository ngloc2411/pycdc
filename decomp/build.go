package decomp

import (
	"fmt"
	"github.com/ngloc2411/pycdc/decomp/ast"
	"math"
	"os"
	"sort"

	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

var _ = math.MaxInt32
var _ = fmt.Sprint
var _ = sort.Ints

type unpackFrame struct{ remaining, starIdx int }

type pendingBoolOp struct {
	target              int
	left                ast.Node
	op                  int
	kind                int
	neg                 bool
	thenVal             ast.Node
	elseStart, thenJump int
}

type spliceCtx struct{ resume, handler, regionEnd, backDest int }

type openElseEnt struct {
	pos   int
	block *ast.Block
}

type matchCtx struct {
	mblock      *ast.Block
	baseStack   *FastStack
	cblock      *ast.Block
	cdepth      int
	cur         MatchCase
	isDefault   bool
	skipPop     bool
	guardOpen   bool
	guardTarget int
	endJumpPos  int
	endPos      int
}

type storeOp struct{ op, arg int }

// BuildFromCode rebuilds the AST of a code object.
func BuildFromCode(code *pyc.Code, mod *pyc.Module) ast.Node {
	source := pyc.NewBuffer([]byte(code.Code.Value))

	stackSize := code.StackSize
	if mod.MajorVer() == 1 {
		stackSize = 20
	}
	stack := NewFastStack(stackSize)
	var stackHist []*FastStack

	var blocks blockStack
	defblock := ast.NewBlock(ast.BLK_MAIN, 0, 0)
	defblock.Init()
	curblock := defblock
	blocks.push(defblock)

	var opcode, operand int
	curpos := 0
	pos := 0
	unpack := 0
	var unpackSaved []unpackFrame
	unpackStar := -1
	unpackBegin := func(count, starIdx int) {
		if unpack > 0 {
			unpackSaved = append(unpackSaved, unpackFrame{unpack, unpackStar})
		}
		unpack = count
		unpackStar = starIdx
		stack.Push(ast.NewTuple(nil))
	}
	unpackAdd := func(target ast.Node) {
		for {
			tup := stack.Top(1)
			if ast.TypeOf(tup) != ast.NODE_TUPLE {
				fmt.Fprint(os.Stderr, "Something TERRIBLE happened!\n")
				return
			}
			if len(tup.(*ast.Tuple).Values()) == unpackStar {
				target = ast.NewUnary(target, ast.UN_UNPACK)
			}
			tup.(*ast.Tuple).Add(target)
			unpack--
			if unpack > 0 {
				return
			}
			stack.Pop()
			if len(unpackSaved) != 0 {
				inner := tup.(*ast.Tuple)
				inner.SetRequireParens(true)
				target = tup
				last := unpackSaved[len(unpackSaved)-1]
				unpack = last.remaining
				unpackStar = last.starIdx
				unpackSaved = unpackSaved[:len(unpackSaved)-1]
				continue
			}
			seq := stack.Top(1)
			stack.Pop()
			unpackStar = -1
			if curblock.BlkType() == ast.BLK_FOR && curblock.Inited() == 0 {
				if t, ok := tup.(*ast.Tuple); ok {
					t.SetRequireParens(false)
				}
				curblock.SetIndex(tup)
			} else if ast.TypeOf(seq) == ast.NODE_CHAINSTORE {
				appendToChainStore(seq, tup, stack, curblock)
			} else {
				tup.(*ast.Tuple).SetRequireParens(false)
				curblock.Append(ast.NewStore(seq, tup))
			}
			return
		}
	}
	tryWalrus := func() bool {
		if stack.Empty() || stack.Top(1) == nil || ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE || source.AtEof() {
			return false
		}
		sSrc, sPos := source.Pos(), pos
		o, a := pyc.BcNext(source, mod, &pos)
		if o != STORE_FAST_A && o != STORE_NAME_A && o != STORE_GLOBAL_A && o != STORE_DEREF_A {
			source.SetPos(sSrc)
			pos = sPos
			return false
		}
		if o == STORE_NAME_A && code.GetName(a).Value == "__classcell__" {
			source.SetPos(sSrc)
			pos = sPos
			return false
		}
		storeOpc, storeArg := o, a
		aSrc, aPos := source.Pos(), pos
		chain := false
		for n := 0; n < 12 && !source.AtEof(); n++ {
			p2 := pos
			o2, a2 := pyc.BcNext(source, mod, &p2)
			pos = p2
			if o2 == CACHE {
				continue
			}
			if o2 == STORE_FAST_A || o2 == STORE_NAME_A || o2 == STORE_GLOBAL_A ||
				o2 == STORE_DEREF_A || o2 == STORE_ATTR_A || o2 == STORE_SUBSCR ||
				o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A ||
				o2 == DUP_TOP || (o2 == COPY_A && a2 == 1) {
				chain = true
				break
			}
			if o2 != LOAD_FAST_A && o2 != LOAD_NAME_A && o2 != LOAD_GLOBAL_A &&
				o2 != LOAD_DEREF_A && o2 != LOAD_CONST_A && o2 != LOAD_ATTR_A &&
				o2 != BINARY_SUBSCR {
				break
			}
		}
		source.SetPos(aSrc)
		pos = aPos
		if chain {
			source.SetPos(sSrc)
			pos = sPos
			return false
		}
		var name ast.Node
		if storeOpc == STORE_FAST_A {
			name = ast.NewName(code.GetLocal(storeArg))
		} else if storeOpc == STORE_DEREF_A {
			name = ast.NewName(code.GetCellVar(mod, storeArg))
		} else {
			name = ast.NewName(code.GetName(storeArg))
		}
		value := stack.Top(1)
		stack.Pop()
		stack.Push(ast.NewNamedExpr(name, value))
		return true
	}
	trueOpened := map[int]bool{}
	var inlineComps []int
	prevOp1, prevOp2 := -1, -1
	commonGuard := false
	isCompCode := func() bool {
		if code.Name == nil {
			return false
		}
		nm := code.Name.Value
		return nm == "<listcomp>" || nm == "<setcomp>" || nm == "<dictcomp>" || nm == "<genexpr>"
	}()
	inlineFinish := false
	fusedKeepFirst := map[int]bool{}
	inlineFinishBase := 0
	var inlineBase []int
	var inlineKind []int
	inlineFinishKind := 0
	var pendingOps []intPair
	var assertErrNode ast.Node
	elsePop := false
	needTry := false
	variableAnnotations := false
	sawGetIter := false
	var pendingBool []pendingBoolOp
	chainVals := map[int]ast.Node{}
	var exceptionEntries []pyc.ExceptionTableEntry
	var cleanupSkip []intPair
	nextExceptionEntry := 0

	withBodyEndByBefore := map[int]int{}
	withResumeByBodyEnd := map[int]int{}
	withSkipUntil := 0
	spliceByEnd := map[int][2]int{}
	var spliceStack []spliceCtx
	spliceDone := map[int]int{}

	finallyTryEndByStart := map[int]int{}
	finallyEndByStart := map[int]int{}
	finallyResumeByEnd := map[int]int{}
	finallyReturnStarts := map[int]bool{}
	finallySuppressReturns := map[int]bool{}

	whileGuardEnd := map[int]int{}
	whileBackedges := map[int]bool{}
	var guardChainConds []ast.Node

	elseRegionByStart := map[int][2]int{}
	var openElse []openElseEnt

	loops := NewWhileLoops()
	xflow := NewExprFlow()
	if mod.VerCompare(3, 8) >= 0 {
		scanLoops(code, mod, loops)
		whileGuardEnd = loops.guardEnd
		whileBackedges = loops.backedges
		scanExprFlow(code, mod, xflow, loops)
		cleanupSkip = append(cleanupSkip, xflow.chainSkip...)
	}

	if mod.VerCompare(3, 11) >= 0 {
		exceptionEntries = code.ExceptionTableEntries()
		if mod.VerCompare(3, 12) >= 0 {
			scanComprehensionCleanup(code, mod, &exceptionEntries, &cleanupSkip)
		}
		scanWithBlocks(code, mod, exceptionEntries, withBodyEndByBefore, withResumeByBodyEnd)
		scanTryFinally(code, mod, exceptionEntries, finallyTryEndByStart, finallyEndByStart,
			finallyResumeByEnd, finallyReturnStarts, finallySuppressReturns)
		scanTryElse(code, mod, exceptionEntries, elseRegionByStart)
		scanHoistedHandlers(code, mod, exceptionEntries, spliceByEnd)
	}
	_ = whileGuardEnd
	_ = whileBackedges
	_ = inlineFinish
	_ = fusedKeepFirst
	_ = inlineFinishBase
	_ = inlineFinishKind
	_ = assertErrNode
	_ = variableAnnotations
	_ = commonGuard
	_ = guardChainConds
	_ = inlineBase
	_ = inlineKind

	// Nested-try prescan (see the C++ comment): recover the true start of an
	// outer try whose first exception entry starts after an inner try.
	shadowedTryStarts := map[int][]int{}
	if len(exceptionEntries) != 0 {
		span := map[int][2]int{}
		finallyTargets := map[int]bool{}
		for _, e := range exceptionEntries {
			if e.PushLasti {
				continue
			}
			if _, ok := finallyTryEndByStart[e.StartOffset]; ok {
				finallyTargets[e.Target] = true
			}
			if sp, ok := span[e.Target]; !ok {
				span[e.Target] = [2]int{e.StartOffset, e.EndOffset}
			} else {
				if e.StartOffset < sp[0] {
					sp[0] = e.StartOffset
				}
				if e.EndOffset > sp[1] {
					sp[1] = e.EndOffset
				}
				span[e.Target] = sp
			}
		}
		var found []intPair
		spanKeys := make([]int, 0, len(span))
		for k := range span {
			spanKeys = append(spanKeys, k)
		}
		sort.Ints(spanKeys)
		for _, T := range spanKeys {
			if finallyTargets[T] {
				continue
			}
			s0, e0 := span[T][0], span[T][1]
			eff := s0
			changed := true
			for changed {
				changed = false
				for _, T2 := range spanKeys {
					if T2 == T {
						continue
					}
					if span[T2][0] < eff && T2 >= s0 && T2 < e0 && span[T2][1] <= s0 {
						eff = span[T2][0]
						changed = true
					}
				}
			}
			if eff < s0 {
				found = append(found, intPair{eff, T})
			}
		}
		sort.SliceStable(found, func(i, j int) bool {
			a, b := found[i], found[j]
			if a.a != b.a {
				return a.a < b.a
			}
			return span[a.b][1] > span[b.b][1]
		})
		for _, f := range found {
			shadowedTryStarts[f.a] = append(shadowedTryStarts[f.a], f.b)
		}
	}

	var matches []*matchCtx
	var matcher *MatchAnalyzer

	closeNested := func(mc *matchCtx, limit int) {
		for blocks.size() > mc.cdepth {
			top := blocks.top()
			if (top.BlkType() != ast.BLK_IF && top.BlkType() != ast.BLK_ELIF && top.BlkType() != ast.BLK_ELSE) ||
				top.End() > limit {
				break
			}
			if len(stackHist) != 0 {
				stackHist = stackHist[:len(stackHist)-1]
			}
			blocks.pop()
			blocks.top().Append(top)
			curblock = blocks.top()
		}
	}

	beginCase := func(mc *matchCtx, nc *MatchCase, isDefault bool) {
		mc.cur = *nc
		mc.isDefault = isDefault
		end := 0
		if nc.bodyEnd > 0 {
			end = nc.bodyEnd
		}
		cb := ast.NewCaseBlock(nc.pattern, end)
		blocks.push(cb)
		curblock = blocks.top()
		mc.cblock = cb
		mc.cdepth = blocks.size()
		stack = mc.baseStack.Copy()
		mc.skipPop = nc.nonLast
		mc.guardOpen = true
		mc.guardTarget = -1
		if nc.bodyEnd >= 2 {
			mc.endJumpPos = nc.bodyEnd - 2
		} else {
			mc.endJumpPos = -1
		}
		pos = nc.bodyStart
		source.SetPos(pos)
		elsePop = false
	}

	finishMatch := func(next int) {
		mc := matches[len(matches)-1]
		for blocks.size() > 1 && blocks.top() != mc.mblock {
			top := blocks.top()
			blocks.pop()
			blocks.top().Append(top)
		}
		mb := blocks.top()
		blocks.pop()
		curblock = blocks.top()
		curblock.Append(mb)
		stack = mc.baseStack.Copy()
		matches = matches[:len(matches)-1]
		pos = next
		source.SetPos(pos)
		elsePop = (curblock.BlkType() == ast.BLK_ELSE || curblock.BlkType() == ast.BLK_IF ||
			curblock.BlkType() == ast.BLK_ELIF) && curblock.End() == pos
	}

	endCase := func(mc *matchCtx) {
		closeNested(mc, pos)
		for blocks.size() > mc.cdepth {
			top := blocks.top()
			blocks.pop()
			blocks.top().Append(top)
		}
		cb := mc.cblock
		blocks.pop()
		curblock = blocks.top()
		curblock.Append(cb)
		mc.cblock = nil
		stack = mc.baseStack.Copy()

		wasNonLast := mc.cur.nonLast
		wasDefault := mc.isDefault
		next := pos
		if mc.cur.bodyEnd >= 0 && mc.cur.nextPos >= 0 {
			next = mc.cur.nextPos
		}
		if wasNonLast {
			var nc MatchCase
			if matcher.ParseCase(next, &nc) {
				beginCase(mc, &nc, false)
			} else {
				fmt.Fprintf(os.Stderr, "Warning: unable to decode a case of a match statement at %d\n", next)
				finishMatch(next)
			}
		} else if !wasDefault {
			ni := matcher.IndexOf(next)
			if ni >= 0 && matcher.Instrs()[ni].op == NOP {
				dc := MatchCase{bodyEnd: -1, nextPos: -1}
				dc.pattern = ast.NewMatchPattern(ast.PAT_WILDCARD)
				dc.bodyStart = matcher.Instrs()[ni].end
				beginCase(mc, &dc, true)
			} else {
				finishMatch(next)
			}
		} else {
			finishMatch(next)
		}
	}

	for !source.AtEof() || len(spliceStack) != 0 {
		if curblock.BlkType() == ast.BLK_WITH && curblock.End() != 0 && curblock.End() <= pos && blocks.size() > 1 {
			with := curblock
			blocks.pop()
			curblock = blocks.top()
			curblock.Append(with)
			if r, ok := withResumeByBodyEnd[with.End()]; ok {
				withSkipUntil = r
			}
		}
		if curblock.BlkType() != ast.BLK_FINALLY {
			finEnds := false
			for i := len(blocks) - 1; i >= 0; i-- {
				b := blocks[i]
				if b.BlkType() == ast.BLK_FINALLY {
					finEnds = b.End() != 0 && b.End() <= pos
					break
				}
				if b.BlkType() == ast.BLK_MAIN {
					break
				}
			}
			for finEnds && curblock.BlkType() != ast.BLK_FINALLY && blocks.size() > 1 {
				inner := curblock
				if inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF ||
					inner.BlkType() == ast.BLK_ELSE || inner.BlkType() == ast.BLK_EXCEPT {
					if len(stackHist) != 0 {
						stackHist = stackHist[:len(stackHist)-1]
					}
				}
				blocks.pop()
				curblock = blocks.top()
				if inner.BlkType() != ast.BLK_EXCEPT || inner.Size() != 0 || inner.Cond() != nil {
					curblock.Append(inner)
				}
			}
		}
		if curblock.BlkType() == ast.BLK_FINALLY && curblock.End() != 0 && curblock.End() <= pos && blocks.size() > 1 {
			finEnd := curblock.End()
			final := curblock
			blocks.pop()
			curblock = blocks.top()
			curblock.Append(final)
			if curblock.BlkType() == ast.BLK_CONTAINER && blocks.size() > 1 {
				cont := curblock
				blocks.pop()
				curblock = blocks.top()
				curblock.Append(cont)
			}
			if r, ok := finallyResumeByEnd[finEnd]; ok {
				withSkipUntil = r
			}
		}
		if withSkipUntil > 0 {
			if pos < withSkipUntil {
				curpos = pos
				opcode, operand = pyc.BcNext(source, mod, &pos)
				continue
			}
			withSkipUntil = 0
		}

		if len(spliceDone) != 0 {
			if end, ok := spliceDone[pos]; ok {
				pos = end
				source.SetPos(pos)
				continue
			}
		}
		if len(spliceByEnd) != 0 {
			if sp, ok := spliceByEnd[pos]; ok {
				handlerPos := sp[0]
				tryOpen := false
				for i := len(blocks) - 1; i >= 0; i-- {
					b := blocks[i]
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
					ctx.resume = pos
					ctx.handler = handlerPos
					ctx.regionEnd = sp[1]
					ctx.backDest = -1
					{
						sSrc := source.Pos()
						p2 := pos
						o, a := pyc.BcNext(source, mod, &p2)
						if o == JUMP_BACKWARD_A {
							ctx.backDest = p2 - a*2
						}
						source.SetPos(sSrc)
					}
					delete(spliceByEnd, pos)
					spliceStack = append(spliceStack, ctx)
					pos = handlerPos
					source.SetPos(pos)
					nextExceptionEntry = 0
					continue
				}
			}
		}

		if sit, ok := shadowedTryStarts[pos]; ok {
			for _, T := range sit {
				open := false
				for i := len(blocks) - 1; i >= 0; i-- {
					if blocks[i].BlkType() == ast.BLK_TRY && blocks[i].End() == T {
						open = true
						break
					}
				}
				if open {
					continue
				}
				cont := ast.NewContainerBlock(0, T)
				blocks.push(cont)
				stackHist = append(stackHist, stack.Copy())
				tryblock := ast.NewBlock(ast.BLK_TRY, T, 1)
				blocks.push(tryblock)
				curblock = blocks.top()
			}
		}

		if curblock.BlkType() != ast.BLK_TRY {
			tryEndsHere := false
			for i := len(blocks) - 1; i >= 0; i-- {
				b := blocks[i]
				if b.BlkType() == ast.BLK_TRY {
					tryEndsHere = b.End() == pos
					break
				}
				if b.BlkType() != ast.BLK_IF && b.BlkType() != ast.BLK_ELIF && b.BlkType() != ast.BLK_ELSE &&
					b.BlkType() != ast.BLK_FOR && b.BlkType() != ast.BLK_WHILE && b.BlkType() != ast.BLK_WITH {
					break
				}
			}
			if tryEndsHere {
				for curblock.BlkType() != ast.BLK_TRY && blocks.size() > 1 {
					inner := curblock
					if inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF || inner.BlkType() == ast.BLK_ELSE {
						if len(stackHist) != 0 {
							stackHist = stackHist[:len(stackHist)-1]
						}
					}
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(inner)
				}
			}
		}

		if len(matches) != 0 && matches[len(matches)-1].cblock != nil &&
			blocks.size() >= matches[len(matches)-1].cdepth {
			mc := matches[len(matches)-1]
			if mc.endJumpPos >= 0 && pos == mc.endJumpPos {
				ji := matcher.IndexOf(pos)
				endsCase := ji >= 0 && matcher.Instrs()[ji].op == JUMP_FORWARD_A &&
					matcher.JumpTarget(matcher.Instrs()[ji]) >= mc.cur.bodyEnd
				lim := pos
				if endsCase {
					lim = math.MaxInt32
				}
				closeNested(mc, lim)
				if blocks.size() == mc.cdepth && ji >= 0 && matcher.Instrs()[ji].op == JUMP_FORWARD_A {
					target := matcher.JumpTarget(matcher.Instrs()[ji])
					if target >= mc.cur.bodyEnd {
						if mc.endPos < 0 {
							mc.endPos = target
						}
						pos = matcher.Instrs()[ji].end
						source.SetPos(pos)
						continue
					}
				}
			}
			if (mc.cur.bodyEnd >= 0 && pos >= mc.cur.bodyEnd) ||
				(mc.cur.bodyEnd < 0 && mc.endPos >= 0 && pos >= mc.endPos) {
				endCase(mc)
				continue
			}
		}

		if _, ok := loops.trueOpen[pos]; ok && !trueOpened[pos] {
			trueOpened[pos] = true
			wh := ast.NewCondBlock(ast.BLK_WHILE, loops.trueOpen[pos], ast.NewObject(pyc.True), false)
			wh.InitV(ast.PRE_POPPED)
			blocks.push(wh)
			curblock = blocks.top()
		}

		for nextExceptionEntry < len(exceptionEntries) && exceptionEntries[nextExceptionEntry].StartOffset < pos {
			nextExceptionEntry++
		}

		if nextExceptionEntry < len(exceptionEntries) {
			entry := exceptionEntries[nextExceptionEntry]
			if entry.StartOffset == pos && !entry.PushLasti {
				if fend, ok := finallyTryEndByStart[entry.StartOffset]; ok {
					finEnd := finallyEndByStart[entry.StartOffset]
					cont := ast.NewContainerBlock(finEnd, 0)
					blocks.push(cont)
					curblock = blocks.top()

					stackHist = append(stackHist, stack.Copy())
					tryblock := ast.NewBlock(ast.BLK_TRY, fend, 1)
					blocks.push(tryblock)
					curblock = blocks.top()
					nextExceptionEntry++
				} else {
					sameTryOpen := false
					for i := len(blocks) - 1; i >= 0; i-- {
						if blocks[i].BlkType() == ast.BLK_TRY && blocks[i].End() == entry.Target {
							sameTryOpen = true
							break
						}
					}
					if sameTryOpen {
						nextExceptionEntry++
					} else {
						if curblock.BlkType() == ast.BLK_CONTAINER {
							curblock.SetExcept(entry.Target)
						} else {
							next := ast.NewContainerBlock(0, entry.Target)
							blocks.push(next)
							curblock = blocks.top()
						}

						stackHist = append(stackHist, stack.Copy())
						tryblock := ast.NewBlock(ast.BLK_TRY, entry.Target, 1)
						blocks.push(tryblock)
						curblock = blocks.top()
						nextExceptionEntry++
					}
				}
			}
		}

		if curblock.BlkType() == ast.BLK_TRY && curblock.End() == pos && blocks.size() > 1 {
			prev := curblock
			blocks.pop()
			curblock = blocks.top()

			if curblock.BlkType() == ast.BLK_CONTAINER && curblock.HasExcept() {
				if len(stackHist) != 0 {
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]
				}

				curblock.Append(prev)
				stackHist = append(stackHist, stack.Copy())

				except := ast.NewCondBlock(ast.BLK_EXCEPT, 0, nil, false)
				except.Init()
				blocks.push(except)
				curblock = blocks.top()
			} else if curblock.BlkType() == ast.BLK_CONTAINER && curblock.HasFinally() {
				finEnd := curblock.Finally()
				if len(stackHist) != 0 {
					if len(finallySuppressReturns) != 0 {
						if _, ok := finallyResumeByEnd[finEnd]; ok &&
							stack.Size() > stackHist[len(stackHist)-1].Size() && stack.Top(1) != nil {
							isRetStyle := false
							for st := range finallyReturnStarts {
								if finallyEndByStart[st] == finEnd {
									isRetStyle = true
								}
							}
							if isRetStyle {
								prev.Append(ast.NewReturn(stack.Top(1), ast.RET_RETURN))
							}
						}
					}
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]
				}

				curblock.Append(prev)

				final := ast.NewBlock(ast.BLK_FINALLY, finEnd, 1)
				final.Init()
				blocks.push(final)
				curblock = blocks.top()
			} else {
				blocks.push(prev)
				curblock = prev
			}
		}

		if curblock.BlkType() == ast.BLK_EXCEPT && curblock.End() == pos && blocks.size() > 1 {
			prev := curblock
			blocks.pop()
			curblock = blocks.top()

			if len(stackHist) != 0 {
				stack = stackHist[len(stackHist)-1]
				stackHist = stackHist[:len(stackHist)-1]
			}

			if prev.Size() != 0 || (prev.BlkType() == ast.BLK_EXCEPT && prev.Cond() != nil) {
				curblock.Append(prev)
			}

			if curblock.BlkType() == ast.BLK_CONTAINER && !curblock.HasFinally() {
				cont := curblock
				blocks.pop()
				curblock = blocks.top()
				curblock.Append(cont)
			}
		}

		{
			if er, ok := elseRegionByStart[pos]; ok && curblock.BlkType() == ast.BLK_TRY &&
				curblock.End() == er[1] && blocks.size() > 1 {
				tryb := blocks.top()
				blocks.pop()
				isContainer := blocks.top().BlkType() == ast.BLK_CONTAINER && blocks.top().HasExcept()
				blocks.push(tryb)
				if isContainer {
					eb := ast.NewBlock(ast.BLK_ELSE, 0, 1)
					blocks.push(eb)
					curblock = eb
					openElse = append(openElse, openElseEnt{er[0], eb})
				}
			}
			if len(openElse) != 0 && pos == openElse[len(openElse)-1].pos &&
				curblock == openElse[len(openElse)-1].block {
				eb := curblock
				blocks.pop()
				tryb := blocks.top()
				blocks.pop()
				cont := blocks.top()
				if eb.Size() != 0 {
					cont.SetElseBlock(eb)
				}
				blocks.push(tryb)
				curblock = tryb
				openElse = openElse[:len(openElse)-1]
				curpos = pos
				opcode, operand = pyc.BcNext(source, mod, &pos)
				continue
			}
		}

		if len(spliceStack) != 0 && pos == spliceStack[len(spliceStack)-1].regionEnd {
			ctx := spliceStack[len(spliceStack)-1]
			spliceStack = spliceStack[:len(spliceStack)-1]
			spliceDone[ctx.handler] = ctx.regionEnd
			pos = ctx.resume
			source.SetPos(pos)
			nextExceptionEntry = 0
			continue
		}

		curpos = pos
		if len(pendingOps) != 0 {
			opcode = pendingOps[0].a
			operand = pendingOps[0].b
			pendingOps = pendingOps[1:]
		} else {
			opcode, operand = pyc.BcNext(source, mod, &pos)
		}

		if len(cleanupSkip) != 0 {
			skipIt := false
			for _, r := range cleanupSkip {
				if curpos >= r.a && curpos < r.b {
					skipIt = true
				}
			}
			if skipIt {
				for _, r := range xflow.chainSkip {
					if curpos >= r.a && curpos < r.b &&
						(curblock.BlkType() == ast.BLK_ELSE || curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) {
						elsePop = curblock.End() == pos
					}
				}
				continue
			}
		}

		hadGetIter := sawGetIter
		_ = hadGetIter
		if opcode != GET_ITER && opcode != GET_YIELD_FROM_ITER && opcode != PRECALL_A && opcode != CACHE &&
			opcode != RESUME_A && opcode != INSTRUMENTED_RESUME_A {
			sawGetIter = false
		}

		if needTry && opcode != SETUP_EXCEPT_A {
			needTry = false
			stackHist = append(stackHist, stack.Copy())
			tryblock := ast.NewBlock(ast.BLK_TRY, curblock.End(), 1)
			blocks.push(tryblock)
			curblock = blocks.top()
		} else if elsePop && opcode != JUMP_FORWARD_A && opcode != JUMP_IF_FALSE_A &&
			opcode != JUMP_IF_FALSE_OR_POP_A && opcode != POP_JUMP_IF_FALSE_A &&
			opcode != POP_JUMP_FORWARD_IF_FALSE_A && opcode != JUMP_IF_TRUE_A &&
			opcode != JUMP_IF_TRUE_OR_POP_A && opcode != POP_JUMP_IF_TRUE_A &&
			opcode != POP_JUMP_FORWARD_IF_TRUE_A && opcode != POP_BLOCK {
			elsePop = false

			prev := curblock
			for prev.End() < pos && prev.BlkType() != ast.BLK_MAIN {
				if prev.BlkType() != ast.BLK_CONTAINER {
					if prev.End() == 0 {
						break
					}
					if len(stackHist) != 0 {
						stackHist = stackHist[:len(stackHist)-1]
					}
				}
				blocks.pop()

				if blocks.empty() {
					break
				}

				curblock = blocks.top()
				curblock.Append(prev)

				prev = curblock

				checkIfExpr(stack, curblock)
			}
		}

		for len(pendingBool) != 0 && pendingBool[len(pendingBool)-1].target <= curpos && !stack.Empty() {
			pb := pendingBool[len(pendingBool)-1]
			pendingBool = pendingBool[:len(pendingBool)-1]
			right := stack.Top(1)
			stack.Pop()
			if pb.kind == 1 {
				tb := ast.NewCondBlock(ast.BLK_IF, 0, pb.left, pb.neg)
				stack.Push(ast.NewTernary(tb, pb.thenVal, right))
			} else {
				stack.Push(mkBoolOp(pb.left, right, pb.op))
			}
		}

		chainPolarity := -1
		if mod.VerCompare(3, 8) >= 0 && len(inlineComps) == 0 && !isCompCode {
			if xflow.condMember[curpos] && !stack.Empty() {
				chainVals[curpos] = stack.Top(1)
				stack.Pop()
				elsePop = false
				continue
			}
			if cc, ok := xflow.condChains[curpos]; ok && !stack.Empty() {
				have := true
				for k := 0; k+1 < len(cc.members); k++ {
					if _, ok := chainVals[cc.members[k]]; !ok {
						have = false
					}
				}
				if have {
					chainVals[curpos] = stack.Top(1)
					stack.Pop()
					var build func(ti int) ast.Node
					build = func(ti int) ast.Node {
						tr := cc.trees[ti]
						if tr.op == 0 {
							return chainVals[tr.leaf]
						}
						bop := ast.BIN_LOG_OR
						if tr.op == 1 {
							bop = ast.BIN_LOG_AND
						}
						return mkBoolOp(build(tr.l), build(tr.r), bop)
					}
					whole := build(cc.root)
					for _, mpos := range cc.members {
						delete(chainVals, mpos)
					}
					stack.Push(whole)
					if cc.negate {
						chainPolarity = 1
					} else {
						chainPolarity = 0
					}
					wantOnTrue := cc.negate
					isTrueOp := opcode == POP_JUMP_IF_TRUE_A || opcode == POP_JUMP_FORWARD_IF_TRUE_A
					if isTrueOp != wantOnTrue {
						switch opcode {
						case POP_JUMP_IF_TRUE_A:
							opcode = POP_JUMP_IF_FALSE_A
						case POP_JUMP_IF_FALSE_A:
							opcode = POP_JUMP_IF_TRUE_A
						case POP_JUMP_FORWARD_IF_TRUE_A:
							opcode = POP_JUMP_FORWARD_IF_FALSE_A
						case POP_JUMP_FORWARD_IF_FALSE_A:
							opcode = POP_JUMP_FORWARD_IF_TRUE_A
						}
					}
				}
			}
		}

		if mod.VerCompare(3, 8) >= 0 {
			handled := func() bool {
				skipOp := func() { elsePop = false }
				mergeAt := func(at int) {
					for len(pendingBool) != 0 && pendingBool[len(pendingBool)-1].kind == 0 &&
						pendingBool[len(pendingBool)-1].target == at && !stack.Empty() {
						pb := pendingBool[len(pendingBool)-1]
						pendingBool = pendingBool[:len(pendingBool)-1]
						right := stack.Top(1)
						stack.Pop()
						stack.Push(mkBoolOp(pb.left, right, pb.op))
					}
				}
				if _, ok := xflow.ternaryThenJump[curpos]; ok && !stack.Empty() {
					for k := len(pendingBool) - 1; k >= 0; k-- {
						if pendingBool[k].kind == 1 && pendingBool[k].thenJump == curpos && pendingBool[k].thenVal == nil {
							pendingBool[k].thenVal = stack.Top(1)
							stack.Pop()
							skipOp()
							return true
						}
					}
				}
				if tn, ok := xflow.ternary[curpos]; ok && !stack.Empty() {
					c := stack.Top(1)
					stack.Pop()
					neg := false
					if chainPolarity >= 0 {
						neg = chainPolarity == 1
					} else if tn.cond == 2 {
						neg = true
					} else if tn.cond == 3 {
						c = ast.NewCompare(c, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
					} else if tn.cond == 4 {
						c = ast.NewCompare(c, ast.NewObject(pyc.None), ast.CMP_IS)
					}
					if len(pendingBool) != 0 && pendingBool[len(pendingBool)-1].kind == 1 &&
						pendingBool[len(pendingBool)-1].elseStart == tn.elseStart &&
						pendingBool[len(pendingBool)-1].thenVal == nil {
						fr := &pendingBool[len(pendingBool)-1]
						prev := fr.left
						if fr.neg {
							prev = ast.NewUnary(prev, ast.UN_NOT)
						}
						cur := c
						if neg {
							cur = ast.NewUnary(cur, ast.UN_NOT)
						}
						fr.left = mkBoolOp(prev, cur, ast.BIN_LOG_AND)
						fr.neg = false
					} else {
						var fr pendingBoolOp
						fr.target = tn.end
						fr.left = c
						fr.op = ast.BIN_LOG_AND
						fr.kind = 1
						fr.neg = neg
						fr.elseStart = tn.elseStart
						fr.thenJump = tn.thenJump
						pendingBool = append(pendingBool, fr)
					}
					skipOp()
					return true
				}
				if ks, ok := xflow.keepSeq[curpos]; ok && !stack.Empty() {
					mergeAt(ks.popPos)
					mergeAt(ks.endPos)
					var pb pendingBoolOp
					pb.target = ks.target
					for k := len(pendingBool) - 1; k >= 0; k-- {
						if pendingBool[k].kind == 1 && pendingBool[k].thenVal == nil &&
							pendingBool[k].target == pb.target && pendingBool[k].thenJump > curpos {
							pb.target = pendingBool[k].thenJump
							break
						}
					}
					pb.left = stack.Top(1)
					if ks.isOr {
						pb.op = ast.BIN_LOG_OR
					} else {
						pb.op = ast.BIN_LOG_AND
					}
					stack.Pop()
					pendingBool = append(pendingBool, pb)
					source.SetPos(ks.endPos)
					pos = ks.endPos
					skipOp()
					return true
				}
				if (opcode == JUMP_IF_TRUE_OR_POP_A || opcode == JUMP_IF_FALSE_OR_POP_A) && !stack.Empty() {
					var pb pendingBoolOp
					mul := 1
					if mod.VerCompare(3, 10) >= 0 {
						mul = 2
					}
					tgt := operand * mul
					if mod.VerCompare(3, 11) >= 0 {
						tgt += pos
					}
					if cj, ok := xflow.chainJoin[curpos]; ok {
						tgt = cj
					}
					mergeAt(pos)
					for k := len(pendingBool) - 1; k >= 0; k-- {
						if pendingBool[k].kind == 1 && pendingBool[k].thenVal == nil &&
							pendingBool[k].target == tgt && pendingBool[k].thenJump > curpos {
							tgt = pendingBool[k].thenJump
							break
						}
					}
					pb.target = tgt
					pb.left = stack.Top(1)
					if opcode == JUMP_IF_TRUE_OR_POP_A {
						pb.op = ast.BIN_LOG_OR
					} else {
						pb.op = ast.BIN_LOG_AND
					}
					stack.Pop()
					pendingBool = append(pendingBool, pb)
					skipOp()
					return true
				}
				if bj, ok := xflow.boolJump[curpos]; ok && !stack.Empty() {
					var pb pendingBoolOp
					pb.target = bj.target
					pb.left = stack.Top(1)
					if bj.isOr {
						pb.op = ast.BIN_LOG_OR
					} else {
						pb.op = ast.BIN_LOG_AND
					}
					stack.Pop()
					pendingBool = append(pendingBool, pb)
					skipOp()
					return true
				}
				return false
			}()
			if handled {
				continue
			}
		}

		if mod.VerCompare(3, 8) >= 0 && mod.VerCompare(3, 11) < 0 &&
			(opcode == POP_JUMP_IF_FALSE_A || opcode == POP_JUMP_IF_TRUE_A) {
			mul := 1
			if mod.VerCompare(3, 10) >= 0 {
				mul = 2
			}
			tgt := operand * mul
			tailOfLoop := false
			if tgt < curpos {
				for i := len(blocks) - 1; i >= 0; i-- {
					bt := blocks[i].BlkType()
					if bt == ast.BLK_FOR {
						tailOfLoop = !blocks[i].IsComprehension() && tgt == blocks[i].Start()
						break
					}
					if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
						break
					}
				}
			}
			if tailOfLoop {
				if opcode == POP_JUMP_IF_TRUE_A {
					opcode = POP_JUMP_BACKWARD_IF_TRUE_A
				} else {
					opcode = POP_JUMP_BACKWARD_IF_FALSE_A
				}
			}
		}

		{
			isSimpleLoad := func(op int) bool {
				return op == LOAD_FAST_A || op == LOAD_CONST_A || op == LOAD_NAME_A || op == LOAD_GLOBAL_A ||
					op == LOAD_DEREF_A || op == LOAD_FAST_BORROW_A || op == LOAD_SMALL_INT_A
			}
			isSimpleStore := func(op int) bool {
				return op == STORE_FAST_A || op == STORE_NAME_A || op == STORE_GLOBAL_A || op == STORE_DEREF_A
			}
			storeName := func(op, arg int) ast.Node {
				if op == STORE_FAST_A {
					return ast.NewName(code.GetLocal(arg))
				}
				if op == STORE_DEREF_A {
					return ast.NewName(code.GetCellVar(mod, arg))
				}
				return ast.NewName(code.GetName(arg))
			}
			if unpack == 0 && mod.VerCompare(3, 8) >= 0 && isSimpleStore(opcode) &&
				len(pendingOps) == 0 && !source.AtEof() &&
				(isSimpleLoad(prevOp1) || prevOp1 == LOAD_FAST_LOAD_FAST_A ||
					prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) && stack.Size() >= 3 {
				sSrc, sPos := source.Pos(), pos
				stores := []storeOp{{opcode, operand}}
				var o2, a2 int
				for !source.AtEof() {
					o2, a2 = pyc.BcNext(source, mod, &pos)
					if !isSimpleStore(o2) {
						break
					}
					stores = append(stores, storeOp{o2, a2})
				}
				ok := len(stores) >= 3 && len(stores) <= stack.Size()
				for k := 0; ok && k < len(stores); k++ {
					ok = stack.Top(k+1) != nil && ast.TypeOf(stack.Top(k+1)) != ast.NODE_CHAINSTORE
				}
				if ok {
					source.SetPos(sSrc)
					pos = sPos
					for k := 1; k < len(stores); k++ {
						o2, a2 = pyc.BcNext(source, mod, &pos)
					}
					vals := make([]ast.Node, len(stores))
					var names []ast.Node
					for k := len(stores); k > 0; k-- {
						names = append(names, storeName(stores[k-1].op, stores[k-1].arg))
					}
					for k := 0; k < len(stores); k++ {
						vals[len(stores)-1-k] = stack.Top(k + 1)
					}
					for k := 0; k < len(stores); k++ {
						stack.Pop()
					}
					vt := ast.NewTuple(vals)
					nt := ast.NewTuple(names)
					vt.SetRequireParens(false)
					nt.SetRequireParens(false)
					curblock.Append(ast.NewStore(vt, nt))
					prevOp1, prevOp2 = -1, -1
					continue
				}
				source.SetPos(sSrc)
				pos = sPos
			}
			if unpack == 0 && mod.VerCompare(3, 8) >= 0 && isSimpleStore(opcode) &&
				((isSimpleLoad(prevOp1) && isSimpleLoad(prevOp2)) || prevOp1 == LOAD_FAST_LOAD_FAST_A ||
					prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) &&
				stack.Size() >= 2 && (!source.AtEof() || len(pendingOps) != 0) &&
				stack.Top(1) != nil && stack.Top(2) != nil &&
				ast.TypeOf(stack.Top(1)) != ast.NODE_CHAINSTORE && ast.TypeOf(stack.Top(2)) != ast.NODE_CHAINSTORE {
				sSrc, sPos := source.Pos(), pos
				var o2, a2 int
				fromPending := len(pendingOps) != 0
				if fromPending {
					o2 = pendingOps[0].a
					a2 = pendingOps[0].b
				} else {
					o2, a2 = pyc.BcNext(source, mod, &pos)
					source.SetPos(sSrc)
					pos = sPos
				}
				if isSimpleStore(o2) {
					t1 := storeName(opcode, operand)
					if fromPending {
						pendingOps = pendingOps[1:]
					} else {
						o2, a2 = pyc.BcNext(source, mod, &pos)
					}
					t2 := storeName(o2, a2)
					y := stack.Top(1)
					stack.Pop()
					x := stack.Top(1)
					stack.Pop()
					vt := ast.NewTuple([]ast.Node{x, y})
					nt := ast.NewTuple([]ast.Node{t2, t1})
					vt.SetRequireParens(false)
					nt.SetRequireParens(false)
					curblock.Append(ast.NewStore(vt, nt))
					prevOp1, prevOp2 = -1, -1
					continue
				}
				source.SetPos(sSrc)
				pos = sPos
			}
			if unpack == 0 && opcode == STORE_FAST_STORE_FAST_A &&
				((isSimpleLoad(prevOp1) && isSimpleLoad(prevOp2)) || prevOp1 == LOAD_FAST_LOAD_FAST_A ||
					prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) &&
				stack.Size() >= 2 && stack.Top(1) != nil && stack.Top(2) != nil &&
				ast.TypeOf(stack.Top(1)) != ast.NODE_CHAINSTORE && ast.TypeOf(stack.Top(2)) != ast.NODE_CHAINSTORE {
				t1 := ast.NewName(code.GetLocal(operand >> 4))
				t2 := ast.NewName(code.GetLocal(operand & 0xF))
				y := stack.Top(1)
				stack.Pop()
				x := stack.Top(1)
				stack.Pop()
				vt := ast.NewTuple([]ast.Node{x, y})
				nt := ast.NewTuple([]ast.Node{t2, t1})
				vt.SetRequireParens(false)
				nt.SetRequireParens(false)
				curblock.Append(ast.NewStore(vt, nt))
				prevOp1, prevOp2 = -1, -1
				continue
			}
			prevOp2 = prevOp1
			prevOp1 = opcode
		}

		if mod.VerCompare(3, 12) >= 0 {
			if opcode == POP_JUMP_IF_NONE_A {
				opcode = POP_JUMP_FORWARD_IF_NONE_A
			} else if opcode == POP_JUMP_IF_NOT_NONE_A {
				opcode = POP_JUMP_FORWARD_IF_NOT_NONE_A
			}
		}

		if commonGuard && opcode == POP_JUMP_IF_FALSE_A {
			commonGuard = false
			jumpBase := pos
			for !source.AtEof() {
				keepSrc, keepPos := source.Pos(), pos
				o3, _ := pyc.BcNext(source, mod, &pos)
				if o3 == CACHE {
					jumpBase = pos
				} else {
					source.SetPos(keepSrc)
					pos = keepPos
					break
				}
			}
			target := jumpBase + operand*2
			if !stack.Empty() {
				stack.Pop()
			}
			source.SetPos(target)
			pos = target
			continue
		}

		if (len(inlineComps) != 0 || isCompCode) && mod.VerCompare(3, 12) >= 0 &&
			(opcode == POP_JUMP_IF_TRUE_A || opcode == POP_JUMP_IF_FALSE_A) && !source.AtEof() {
			sSrc, sPos := source.Pos(), pos
			jumpBase := pos
			o2, _ := pyc.BcNext(source, mod, &pos)
			for o2 == CACHE && !source.AtEof() {
				jumpBase = pos
				o2, _ = pyc.BcNext(source, mod, &pos)
			}
			if o2 == NOT_TAKEN && !source.AtEof() {
				o2, _ = pyc.BcNext(source, mod, &pos)
			}
			afterJump := pos
			if o2 == JUMP_BACKWARD_A || o2 == JUMP_BACKWARD_NO_INTERRUPT_A {
				for {
					if source.AtEof() {
						break
					}
					keepSrc, keepPos := source.Pos(), pos
					o3, _ := pyc.BcNext(source, mod, &pos)
					if o3 == CACHE {
						afterJump = pos
					} else {
						source.SetPos(keepSrc)
						pos = keepPos
						break
					}
				}
			}
			target := jumpBase + operand*2
			if (o2 == JUMP_BACKWARD_A || o2 == JUMP_BACKWARD_NO_INTERRUPT_A) && target == afterJump {
				if opcode == POP_JUMP_IF_TRUE_A {
					opcode = POP_JUMP_BACKWARD_IF_FALSE_A
				} else {
					opcode = POP_JUMP_BACKWARD_IF_TRUE_A
				}
			} else {
				source.SetPos(sSrc)
				pos = sPos
			}
		}

		if mod.VerCompare(3, 10) >= 0 && !stack.Empty() &&
			((opcode == COPY_A && operand == 1) || opcode == DUP_TOP || opcode == MATCH_SEQUENCE ||
				opcode == MATCH_MAPPING || opcode == LOAD_GLOBAL_A || opcode == LOAD_NAME_A ||
				opcode == LOAD_FAST_A || opcode == LOAD_DEREF_A) {
			if matcher == nil {
				matcher = NewMatchAnalyzer(code, mod)
			}
			var first MatchCase
			if matcher.IsMatchStart(curpos) && matcher.ParseCase(curpos, &first) && first.bodyEnd >= 0 {
				var second MatchCase
				if !first.nonLast || matcher.ParseCase(first.nextPos, &second) {
					subject := stack.Top(1)
					stack.Pop()
					mb := ast.NewMatchBlock(subject)
					blocks.push(mb)
					curblock = blocks.top()
					mc := &matchCtx{mblock: mb, baseStack: stack.Copy(), guardTarget: -1, endJumpPos: -1, endPos: -1}
					matches = append(matches, mc)
					beginCase(mc, &first, false)
					continue
				}
			}
		}

		if mod.VerCompare(3, 8) >= 0 && len(inlineComps) == 0 {
			closeWhile := func(popCond bool) bool {
				hasWhile := false
				for i := len(blocks) - 1; i >= 0; i-- {
					if blocks[i].BlkType() == ast.BLK_WHILE {
						hasWhile = true
						break
					}
				}
				if !hasWhile {
					return false
				}
				if popCond && !stack.Empty() {
					stack.Pop()
				}
				for blocks.size() > 1 && curblock.BlkType() != ast.BLK_WHILE {
					inner := curblock
					if len(stackHist) != 0 && (inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF ||
						inner.BlkType() == ast.BLK_ELSE) {
						stack = stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-1]
					}
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(inner)
				}
				if curblock.BlkType() == ast.BLK_WHILE && blocks.size() > 1 {
					wh := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(wh)
					if ee, ok := loops.elseEnd[wh.End()]; ok {
						eb := ast.NewBlock(ast.BLK_ELSE, ee, 0)
						eb.InitV(ast.PRE_POPPED)
						stackHist = append(stackHist, stack.Copy())
						blocks.push(eb)
						curblock = blocks.top()
					}
				}
				return true
			}
			refreshElsePop := func() {
				elsePop = (curblock.BlkType() == ast.BLK_ELSE || curblock.BlkType() == ast.BLK_IF ||
					curblock.BlkType() == ast.BLK_ELIF) && curblock.End() == pos
			}
			if loops.skipJumps[curpos] {
				refreshElsePop()
				continue
			}
			if (loops.guardChainMember[curpos] || loops.tailChain[curpos]) && !stack.Empty() {
				if loops.guardChainMember[curpos] {
					guardChainConds = append(guardChainConds, stack.Top(1))
				}
				stack.Pop()
				refreshElsePop()
				continue
			}
			if _, ok := loops.trueOpen[curpos]; ok && !trueOpened[curpos] {
				trueOpened[curpos] = true
				wh := ast.NewCondBlock(ast.BLK_WHILE, loops.trueOpen[curpos], ast.NewObject(pyc.True), false)
				wh.InitV(ast.PRE_POPPED)
				blocks.push(wh)
				curblock = blocks.top()
			}
			if loops.backedges[curpos] && closeWhile(true) {
				refreshElsePop()
				continue
			}
			if loops.forTryEnd[curpos] {
				for blocks.size() > 1 && curblock.BlkType() != ast.BLK_FOR && curblock.End() <= curpos &&
					curblock.BlkType() != ast.BLK_MAIN {
					inner := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(inner)
				}
				if curblock.BlkType() == ast.BLK_FOR && curblock.End() == curpos && blocks.size() > 1 &&
					!curblock.IsComprehension() {
					fb := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(fb)
				}
			}
			if loops.uncondBack[curpos] && closeWhile(false) {
				refreshElsePop()
				continue
			}
			if fe, ok := loops.forElse[curpos]; ok {
				eb := ast.NewBlock(ast.BLK_ELSE, fe, 0)
				eb.InitV(ast.PRE_POPPED)
				stackHist = append(stackHist, stack.Copy())
				blocks.push(eb)
				curblock = blocks.top()
			}
			if loops.forBreakPops[curpos] {
				refreshElsePop()
				continue
			}
			if loops.forBreaks[curpos] {
				curblock.Append(ast.NewKeyword(ast.KW_BREAK))
				refreshElsePop()
				continue
			}
			if bv, ok := loops.breakIf[curpos]; ok && !stack.Empty() {
				bcond := stack.Top(1)
				stack.Pop()
				bb := ast.NewCondBlock(ast.BLK_IF, pos, bcond, !bv)
				bb.InitV(ast.PRE_POPPED)
				bb.Append(ast.NewKeyword(ast.KW_BREAK))
				curblock.Append(bb)
				refreshElsePop()
				continue
			}
			if loops.breaks[curpos] {
				curblock.Append(ast.NewKeyword(ast.KW_BREAK))
				refreshElsePop()
				continue
			}
			if loops.continues[curpos] {
				curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
				refreshElsePop()
				continue
			}
		}

		jumpForwardImpl := func() {
			for (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
				curblock.End() == curpos && blocks.size() > 1 {
				fin := curblock
				blocks.pop()
				curblock = blocks.top()
				curblock.Append(fin)
				if len(stackHist) != 0 {
					stackHist = stackHist[:len(stackHist)-1]
				}
			}

			offs := operand
			if mod.VerCompare(3, 10) >= 0 {
				offs *= 2
			}

			if curblock.BlkType() == ast.BLK_CONTAINER {
				if curblock.HasExcept() {
					stackHist = append(stackHist, stack.Copy())

					curblock.SetEnd(pos + offs)
					except := ast.NewCondBlock(ast.BLK_EXCEPT, pos+offs, nil, false)
					except.Init()
					blocks.push(except)
					curblock = blocks.top()
				}
				return
			}

			if len(stackHist) != 0 {
				if stack.Empty() {
					stack = stackHist[len(stackHist)-1]
				}
				stackHist = stackHist[:len(stackHist)-1]
			}

			prev := curblock
			push := true

			for {
				if blocks.size() <= 1 {
					cleanBuild = false
					break
				}
				blocks.pop()

				if !blocks.empty() {
					blocks.top().Append(prev)
				}

				if prev.BlkType() == ast.BLK_IF || prev.BlkType() == ast.BLK_ELIF {
					if offs == 0 {
						prev = nil
						break
					}

					if push {
						stackHist = append(stackHist, stack.Copy())
					}
					next := ast.NewBlock(ast.BLK_ELSE, pos+offs, 0)
					if prev.Inited() == ast.PRE_POPPED {
						next.InitV(ast.PRE_POPPED)
					}

					blocks.push(next)
					prev = nil
				} else if prev.BlkType() == ast.BLK_EXCEPT {
					if offs == 0 {
						prev = nil
						break
					}

					if push {
						stackHist = append(stackHist, stack.Copy())
					}
					next := ast.NewCondBlock(ast.BLK_EXCEPT, pos+offs, nil, false)
					next.Init()

					blocks.push(next)
					prev = nil
				} else if prev.BlkType() == ast.BLK_ELSE {
					prev = blocks.top()
					if !push {
						stack = stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-1]
					}
					push = false

					if prev.BlkType() == ast.BLK_MAIN {
						prev = nil
					}
				} else if prev.BlkType() == ast.BLK_TRY && prev.End() < pos+offs {
					if len(stackHist) != 0 {
						stack = stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-1]
					}

					if blocks.top().BlkType() == ast.BLK_CONTAINER {
						cont := blocks.top()
						if cont.HasExcept() {
							if push {
								stackHist = append(stackHist, stack.Copy())
							}

							except := ast.NewCondBlock(ast.BLK_EXCEPT, pos+offs, nil, false)
							except.Init()
							blocks.push(except)
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

			if !blocks.empty() {
				curblock = blocks.top()
				if curblock.BlkType() == ast.BLK_EXCEPT {
					curblock.SetEnd(pos + offs)
				}
			}
		}

		switch opcode {
		case BINARY_OP_A:
			if operand == 26 && mod.VerCompare(3, 14) >= 0 {
				subscr := stack.Top(1)
				stack.Pop()
				src := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewSubscr(src, subscr))
				break
			}
			{
				op := binFromBinaryOp(operand)
				if op == ast.BIN_INVALID {
					fmt.Fprintf(os.Stderr, "Unsupported `BINARY_OP` operand value: %d\n", operand)
				}
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewBinary(left, right, op))
			}
		case BINARY_ADD, BINARY_AND, BINARY_DIVIDE, BINARY_FLOOR_DIVIDE, BINARY_LSHIFT, BINARY_MODULO,
			BINARY_MULTIPLY, BINARY_OR, BINARY_POWER, BINARY_RSHIFT, BINARY_SUBTRACT, BINARY_TRUE_DIVIDE,
			BINARY_XOR, BINARY_MATRIX_MULTIPLY, INPLACE_ADD, INPLACE_AND, INPLACE_DIVIDE,
			INPLACE_FLOOR_DIVIDE, INPLACE_LSHIFT, INPLACE_MODULO, INPLACE_MULTIPLY, INPLACE_OR,
			INPLACE_POWER, INPLACE_RSHIFT, INPLACE_SUBTRACT, INPLACE_TRUE_DIVIDE, INPLACE_XOR,
			INPLACE_MATRIX_MULTIPLY:
			{
				op := binFromOpcode(opcode)
				if op == ast.BIN_INVALID {
					panic("Unhandled opcode from ast.Binary::from_opcode")
				}
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewBinary(left, right, op))
			}
		case BINARY_SUBSCR:
			{
				subscr := stack.Top(1)
				stack.Pop()
				src := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewSubscr(src, subscr))
			}
		case BREAK_LOOP:
			curblock.Append(ast.NewKeyword(ast.KW_BREAK))
		case BUILD_CLASS:
			{
				classCode := stack.Top(1)
				stack.Pop()
				bases := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewClass(classCode, bases, name))
			}
		case BUILD_FUNCTION:
			{
				funCode := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewFunction(funCode, nil, nil))
			}
		case BUILD_LIST_A:
			{
				var values []ast.Node
				for i := 0; i < operand; i++ {
					values = pushFront(values, stack.Top(1))
					stack.Pop()
				}
				stack.Push(ast.NewList(values))
			}
		case BUILD_SET_A:
			{
				var values []ast.Node
				for i := 0; i < operand; i++ {
					values = pushFront(values, stack.Top(1))
					stack.Pop()
				}
				stack.Push(ast.NewSet(values))
			}
		case BUILD_MAP_A:
			if mod.VerCompare(3, 5) >= 0 {
				m := ast.NewMap()
				for i := 0; i < operand; i++ {
					value := stack.Top(1)
					stack.Pop()
					key := stack.Top(1)
					stack.Pop()
					m.Add(key, value)
				}
				stack.Push(m)
			} else {
				if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					stack.Pop()
				}
				stack.Push(ast.NewMap())
			}
		case BUILD_CONST_KEY_MAP_A:
			{
				keys := stack.Top(1)
				stack.Pop()
				values := make([]ast.Node, 0, operand)
				for i := 0; i < operand; i++ {
					value := stack.Top(1)
					stack.Pop()
					values = append(values, value)
				}
				stack.Push(ast.NewConstMap(keys, values))
			}
		case STORE_MAP:
			{
				key := stack.Top(1)
				stack.Pop()
				value := stack.Top(1)
				stack.Pop()
				m := stack.Top(1).(*ast.Map)
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
				if operand == 2 {
					end := stack.Top(1)
					stack.Pop()
					start := stack.Top(1)
					stack.Pop()
					if isNone(start) {
						start = nil
					}
					if isNone(end) {
						end = nil
					}
					stack.Push(mk(start, end))
				} else if operand == 3 {
					step := stack.Top(1)
					stack.Pop()
					end := stack.Top(1)
					stack.Pop()
					start := stack.Top(1)
					stack.Pop()
					if isNone(start) {
						start = nil
					}
					if isNone(end) {
						end = nil
					}
					if isNone(step) {
						step = nil
					}
					stack.Push(mk(start, end))
					lhs := stack.Top(1)
					stack.Pop()
					if step == nil {
						stack.Push(ast.NewSlice(ast.SLICE1, lhs, step))
					} else {
						stack.Push(ast.NewSlice(ast.SLICE3, lhs, step))
					}
				}
			}
		case BUILD_STRING_A:
			{
				var values []ast.Node
				for i := 0; i < operand; i++ {
					values = pushFront(values, stack.Top(1))
					stack.Pop()
				}
				stack.Push(ast.NewJoinedStr(values))
			}
		case BUILD_TUPLE_A:
			{
				tos := stack.Top(1)
				if tos != nil && tos.Type() == ast.NODE_LOADBUILDCLASS {
					break
				}
				values := make([]ast.Node, operand)
				for i := 0; i < operand; i++ {
					values[operand-i-1] = stack.Top(1)
					stack.Pop()
				}
				stack.Push(ast.NewTuple(values))
			}
		case KW_NAMES_A:
			{
				kwConst := code.GetConst(operand).(*pyc.Seq)
				kwparams := kwConst.Size()
				kwparamList := ast.NewKwNamesMap()
				keys := kwConst.Values()
				for i := 0; i < kwparams; i++ {
					kwparamList.Add(ast.NewObject(keys[kwparams-i-1]), stack.Top(1))
					stack.Pop()
				}
				stack.Push(kwparamList)
			}
		case CALL_A, CALL_FUNCTION_A, INSTRUMENTED_CALL_A:
			{
				kwparams := (operand & 0xFF00) >> 8
				pparams := operand & 0xFF
				var kwparamList []ast.KV
				var pparamList []ast.Node

				stackHist = append(stackHist, stack.Copy())
				var classKwargs []ast.KV
				if mod.VerCompare(3, 11) >= 0 && stack.Top(1) != nil && stack.Top(1).Type() == ast.NODE_KW_NAMES_MAP {
					km := stack.Top(1).(*ast.KwNamesMap)
					stack.Pop()
					classKwargs = append(classKwargs, km.Values()...)
				}
				basecnt := 0
				var bases []ast.Node
				TOS := stack.Top(1)
				TOSType := ast.TypeOf(TOS)
				for TOSType == ast.NODE_NAME || TOSType == ast.NODE_BINARY {
					bases = append(bases, TOS)
					basecnt++
					stack.Pop()
					TOS = stack.Top(1)
					TOSType = ast.TypeOf(TOS)
				}
				name := stack.Top(1)
				stack.Pop()
				function := stack.Top(1)
				stack.Pop()
				loadbuild := stack.Top(1)
				stack.Pop()
				if ast.TypeOf(loadbuild) == ast.NODE_LOADBUILDCLASS {
					if mod.VerCompare(3, 11) >= 0 && !stack.Empty() && stack.Top(1) == nil {
						stack.Pop()
					}
					call := ast.NewCall(function, pparamList, classKwargs)
					stack.Push(ast.NewClass(call, ast.NewTuple(bases), name))
					stackHist = stackHist[:len(stackHist)-1]
					break
				} else {
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]
				}

				if mod.VerCompare(3, 11) >= 0 {
					objectOrMap := stack.Top(1)
					if ast.TypeOf(objectOrMap) == ast.NODE_KW_NAMES_MAP {
						stack.Pop()
						for _, kv := range objectOrMap.(*ast.KwNamesMap).Values() {
							kwparamList = pushFrontKV(kwparamList, ast.KV{K: kv.K, V: kv.V})
							pparams -= 1
						}
					}
				} else {
					for i := 0; i < kwparams; i++ {
						val := stack.Top(1)
						stack.Pop()
						key := stack.Top(1)
						stack.Pop()
						kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
					}
				}
				for i := 0; i < pparams; i++ {
					param := stack.Top(1)
					stack.Pop()
					if ast.TypeOf(param) == ast.NODE_FUNCTION {
						funCode := param.(*ast.Function).Code()
						codeSrc := funCode.(*ast.Object).Object().(*pyc.Code)
						functionName := codeSrc.Name
						if functionName.Value == "<lambda>" {
							pparamList = pushFront(pparamList, param)
						} else {
							decorName := ast.NewName(functionName)
							curblock.Append(ast.NewStore(param, decorName))
							pparamList = pushFront(pparamList, decorName)
						}
					} else {
						pparamList = pushFront(pparamList, param)
					}
				}
				fn := stack.Top(1)
				stack.Pop()

				isDecoratorApply := false
				compInlined := false
				if mod.VerCompare(3, 11) < 0 && fn != nil && fn.Type() == ast.NODE_FUNCTION &&
					len(pparamList) == 1 && len(kwparamList) == 0 {
					ccode := fn.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
					cnm := ccode.Name.Value
					if cnm == "<listcomp>" || cnm == "<setcomp>" || cnm == "<dictcomp>" || cnm == "<genexpr>" {
						savedClean := cleanBuild
						compAst := BuildFromCode(ccode, mod)
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
							stack.Push(comp)
							compInlined = true
						}
					}
				}
				if opcode == CALL_A || opcode == INSTRUMENTED_CALL_A {
					if !stack.Empty() && stack.Top(1) == nil {
						stack.Pop()
					} else if !stack.Empty() && stack.Top(1) != nil {
						below := stack.Top(1)

						topIsDecoTarget := ast.TypeOf(fn) == ast.NODE_FUNCTION || ast.TypeOf(fn) == ast.NODE_CLASS ||
							(ast.TypeOf(fn) == ast.NODE_CALL && fn.(*ast.Call).IsDecorator())

						belowIsAssert := below != nil && below == assertErrNode

						belowIsComp := false
						if hadGetIter && below != nil && below.Type() == ast.NODE_FUNCTION {
							bcode := below.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
							bnm := bcode.Name.Value
							belowIsComp = bnm == "<listcomp>" || bnm == "<setcomp>" || bnm == "<dictcomp>" || bnm == "<genexpr>"
						}

						if belowIsAssert {
							pparamList = pushFront(pparamList, fn)
							fn = stack.Top(1)
							stack.Pop()
						} else if belowIsComp {
							ccode := below.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
							savedClean := cleanBuild
							compAst := BuildFromCode(ccode, mod)
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
								stack.Pop()
								stack.Push(comp)
								compInlined = true
							}
						} else if topIsDecoTarget {
							target := fn
							fn = stack.Top(1)
							stack.Pop()
							if ast.TypeOf(target) == ast.NODE_FUNCTION {
								tcode := target.(*ast.Function).Code().(*ast.Object).Object().(*pyc.Code)
								tname := tcode.Name
								if tname.Value != "<lambda>" {
									decorName := ast.NewName(tname)
									curblock.Append(ast.NewStore(target, decorName))
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
									curblock.Append(ast.NewStore(target, decorName))
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
					stack.Push(callNode)
				}
			}
		case CALL_FUNCTION_VAR_A:
			{
				vr := stack.Top(1)
				stack.Pop()
				kwparams := (operand & 0xFF00) >> 8
				pparams := operand & 0xFF
				var kwparamList []ast.KV
				var pparamList []ast.Node
				for i := 0; i < kwparams; i++ {
					val := stack.Top(1)
					stack.Pop()
					key := stack.Top(1)
					stack.Pop()
					kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
				}
				for i := 0; i < pparams; i++ {
					pparamList = pushFront(pparamList, stack.Top(1))
					stack.Pop()
				}
				fn := stack.Top(1)
				stack.Pop()
				call := ast.NewCall(fn, pparamList, kwparamList)
				call.SetVar(vr)
				stack.Push(call)
			}
		case CALL_FUNCTION_KW_A:
			if mod.VerCompare(3, 6) >= 0 {
				names := stack.Top(1)
				stack.Pop()
				var keys []pyc.Object
				if names != nil && names.Type() == ast.NODE_OBJECT {
					keys = names.(*ast.Object).Object().(*pyc.Seq).Values()
				}
				n := len(keys)
				var kwparamList []ast.KV
				var pparamList []ast.Node
				for i := 0; i < n; i++ {
					kwparamList = pushFrontKV(kwparamList, ast.KV{K: ast.NewObject(keys[n-i-1]), V: stack.Top(1)})
					stack.Pop()
				}
				for i := 0; i < operand-n; i++ {
					pparamList = pushFront(pparamList, stack.Top(1))
					stack.Pop()
				}
				fn := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewCall(fn, pparamList, kwparamList))
				break
			}
			{
				kw := stack.Top(1)
				stack.Pop()
				kwparams := (operand & 0xFF00) >> 8
				pparams := operand & 0xFF
				var kwparamList []ast.KV
				var pparamList []ast.Node
				for i := 0; i < kwparams; i++ {
					val := stack.Top(1)
					stack.Pop()
					key := stack.Top(1)
					stack.Pop()
					kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
				}
				for i := 0; i < pparams; i++ {
					pparamList = pushFront(pparamList, stack.Top(1))
					stack.Pop()
				}
				fn := stack.Top(1)
				stack.Pop()
				call := ast.NewCall(fn, pparamList, kwparamList)
				call.SetKW(kw)
				stack.Push(call)
			}
		case CALL_FUNCTION_VAR_KW_A:
			{
				kw := stack.Top(1)
				stack.Pop()
				vr := stack.Top(1)
				stack.Pop()
				kwparams := (operand & 0xFF00) >> 8
				pparams := operand & 0xFF
				var kwparamList []ast.KV
				var pparamList []ast.Node
				for i := 0; i < kwparams; i++ {
					val := stack.Top(1)
					stack.Pop()
					key := stack.Top(1)
					stack.Pop()
					kwparamList = pushFrontKV(kwparamList, ast.KV{K: key, V: val})
				}
				for i := 0; i < pparams; i++ {
					pparamList = pushFront(pparamList, stack.Top(1))
					stack.Pop()
				}
				fn := stack.Top(1)
				stack.Pop()
				call := ast.NewCall(fn, pparamList, kwparamList)
				call.SetKW(kw)
				call.SetVar(vr)
				stack.Push(call)
			}
		case CALL_FUNCTION_EX_A, INSTRUMENTED_CALL_FUNCTION_EX_A:
			{
				var kw ast.Node
				if operand&0x01 != 0 || mod.VerCompare(3, 14) >= 0 {
					kw = stack.Top(1)
					stack.Pop()
				}
				vr := stack.Top(1)
				stack.Pop()
				fn := stack.Top(1)
				stack.Pop()
				if mod.VerCompare(3, 11) >= 0 && !stack.Empty() && stack.Top(1) == nil {
					stack.Pop()
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
				stack.Push(call)
			}
		case CALL_METHOD_A:
			{
				var pparamList []ast.Node
				for i := 0; i < operand; i++ {
					param := stack.Top(1)
					stack.Pop()
					if ast.TypeOf(param) == ast.NODE_FUNCTION {
						funCode := param.(*ast.Function).Code()
						codeSrc := funCode.(*ast.Object).Object().(*pyc.Code)
						functionName := codeSrc.Name
						if functionName.Value == "<lambda>" {
							pparamList = pushFront(pparamList, param)
						} else {
							decorName := ast.NewName(functionName)
							curblock.Append(ast.NewStore(param, decorName))
							pparamList = pushFront(pparamList, decorName)
						}
					} else {
						pparamList = pushFront(pparamList, param)
					}
				}
				fn := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewCall(fn, pparamList, nil))
			}
		case CONTINUE_LOOP_A:
			curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
		case JUMP_IF_NOT_EXC_MATCH_A:
			{
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewCompare(left, right, ast.CMP_EXCEPTION))
				pendingOps = append(pendingOps, intPair{POP_JUMP_IF_FALSE_A, operand})
			}
		case COMPARE_OP_A:
			{
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				arg := operand
				if mod.VerCompare(3, 12) == 0 {
					arg >>= 4
				} else if mod.VerCompare(3, 13) >= 0 {
					arg >>= 5
				}
				stack.Push(ast.NewCompare(left, right, arg))
			}
		case CONTAINS_OP_A:
			{
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				op := ast.CMP_IN
				if operand != 0 {
					op = ast.CMP_NOT_IN
				}
				stack.Push(ast.NewCompare(left, right, op))
			}
		case DELETE_ATTR_A:
			{
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewBinary(name, ast.NewName(code.GetName(operand)), ast.BIN_ATTR)))
			}
		case DELETE_GLOBAL_A:
			code.MarkGlobal(code.GetName(operand))
			fallthrough
		case DELETE_NAME_A:
			{
				varname := code.GetName(operand)
				if varname.Length() >= 2 && varname.Value[0] == '_' && varname.Value[1] == '[' {
					break
				}
				name := ast.NewName(varname)
				if isExceptVarDelete(curblock, name) {
					break
				}
				curblock.Append(ast.NewDelete(name))
			}
		case DELETE_FAST_A:
			{
				var name *ast.Name
				if mod.VerCompare(1, 3) < 0 {
					name = ast.NewName(code.GetName(operand))
				} else {
					name = ast.NewName(code.GetLocal(operand))
				}
				nv := name.Name().Value
				if len(nv) >= 2 && nv[0] == '_' && nv[1] == '[' {
					break
				}
				if isExceptVarDelete(curblock, name) {
					break
				}
				curblock.Append(ast.NewDelete(name))
			}
		case DELETE_SLICE_0:
			{
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE0, nil, nil))))
			}
		case DELETE_SLICE_1:
			{
				upper := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE1, upper, nil))))
			}
		case DELETE_SLICE_2:
			{
				lower := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE2, nil, lower))))
			}
		case DELETE_SLICE_3:
			{
				lower := stack.Top(1)
				stack.Pop()
				upper := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewSubscr(name, ast.NewSlice(ast.SLICE3, upper, lower))))
			}
		case DELETE_SUBSCR:
			{
				key := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewDelete(ast.NewSubscr(name, key)))
			}
		case DUP_TOP:
			{
				if tryWalrus() {
					break
				}
				if mod.VerCompare(3, 11) < 0 && stack.Size() >= 2 {
					sp := source.Pos()
					p2 := pos
					o2, _ := pyc.BcNext(source, mod, &p2)
					isChain := false
					if o2 == ROT_THREE {
						p3 := p2
						o3, _ := pyc.BcNext(source, mod, &p3)
						isChain = o3 == COMPARE_OP_A || o3 == IS_OP_A || o3 == CONTAINS_OP_A
					}
					if isChain {
						b := stack.Top(1)
						stack.Pop()
						a := stack.Top(1)
						stack.Pop()
						stack.Push(b)
						stack.Push(a)
						stack.Push(b)
						source.SetPos(p2)
						pos = p2
						break
					}
					source.SetPos(sp)
				}
				if stack.Top(1) == nil {
					stack.Push(stack.Top(1))
				} else if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					chainstore := stack.Top(1)
					stack.Pop()
					stack.Push(stack.Top(1))
					stack.Push(chainstore)
				} else {
					stack.Push(stack.Top(1))
					stack.Push(ast.NewChainStore(nil, stack.Top(1)))
				}
			}
		case DUP_TOP_TWO:
			{
				first := stack.Top(1)
				stack.Pop()
				second := stack.Top(1)
				stack.Push(first)
				stack.Push(second)
				stack.Push(first)
			}
		case DUP_TOPX_A:
			{
				var first, second []ast.Node
				for i := 0; i < operand; i++ {
					node := stack.Top(1)
					stack.Pop()
					first = append(first, node)
					second = append(second, node)
				}
				for len(first) != 0 {
					stack.Push(first[len(first)-1])
					first = first[:len(first)-1]
				}
				for len(second) != 0 {
					stack.Push(second[len(second)-1])
					second = second[:len(second)-1]
				}
			}
		case END_FINALLY:
			{
				isFinally := false
				if curblock.BlkType() == ast.BLK_FINALLY {
					final := curblock
					blocks.pop()

					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]

					curblock = blocks.top()
					curblock.Append(final)
					isFinally = true
				} else if curblock.BlkType() == ast.BLK_EXCEPT {
					blocks.pop()
					prev := curblock

					isUninitAsyncFor := false
					if blocks.top().BlkType() == ast.BLK_CONTAINER {
						container := blocks.top()
						blocks.pop()
						asyncForBlock := blocks.top()
						isUninitAsyncFor = asyncForBlock.BlkType() == ast.BLK_ASYNCFOR && asyncForBlock.Inited() == 0
						if isUninitAsyncFor {
							tryBlock := container.Nodes()[0].(*ast.Block)
							if len(tryBlock.Nodes()) != 0 && tryBlock.BlkType() == ast.BLK_TRY {
								if store, ok := tryBlock.Nodes()[0].(*ast.Store); ok {
									asyncForBlock.SetIndex(store.Dest())
								}
							}
							curblock = blocks.top()
							stack = stackHist[len(stackHist)-1]
							stackHist = stackHist[:len(stackHist)-1]
							if curblock.Inited() == 0 {
								fmt.Fprint(os.Stderr, "Error when decompiling 'async for'.\n")
							}
						} else {
							blocks.push(container)
						}
					}

					if !isUninitAsyncFor {
						if curblock.Size() != 0 {
							blocks.top().Append(curblock)
						}

						curblock = blocks.top()

						if curblock.End() != pos || curblock.HasFinally() {
							elseblk := ast.NewBlock(ast.BLK_ELSE, prev.End(), 0)
							elseblk.Init()
							blocks.push(elseblk)
							curblock = blocks.top()
						} else {
							stack = stackHist[len(stackHist)-1]
							stackHist = stackHist[:len(stackHist)-1]
						}
					}
				}

				if curblock.BlkType() == ast.BLK_CONTAINER {
					cont := curblock
					if !cont.HasFinally() || isFinally {
						blocks.pop()
						curblock = blocks.top()
						curblock.Append(cont)
					}
				}
			}
		case EXEC_STMT:
			{
				if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					stack.Pop()
				}
				loc := stack.Top(1)
				stack.Pop()
				glob := stack.Top(1)
				stack.Pop()
				stmt := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewExec(stmt, glob, loc))
			}
		case FOR_ITER_A, INSTRUMENTED_FOR_ITER_A:
			{
				iter := stack.Top(1)
				if mod.VerCompare(3, 12) < 0 {
					stack.Pop()
				}

				var end int
				comprehension := false

				if mod.MajorVer() == 3 && mod.MinorVer() >= 8 {
					end = operand
					if mod.VerCompare(3, 10) >= 0 {
						end *= 2
					}
					end += pos
					if mod.VerCompare(3, 12) >= 0 {
						end += 2
					}
					cn := code.Name.Value
					comprehension = cn == "<listcomp>" || cn == "<setcomp>" || cn == "<dictcomp>"
					if len(inlineComps) != 0 {
						comprehension = true
					}
				} else {
					top := blocks.top()
					end = top.End()
					if top.BlkType() == ast.BLK_WHILE {
						blocks.pop()
					} else {
						comprehension = true
					}
				}

				forblk := ast.NewIterBlock(ast.BLK_FOR, curpos, end, iter)
				forblk.SetComprehension(comprehension)
				blocks.push(forblk)
				curblock = blocks.top()

				stack.Push(nil)
			}
		case FOR_LOOP_A:
			{
				curidx := stack.Top(1)
				stack.Pop()
				iter := stack.Top(1)
				stack.Pop()

				comprehension := false
				top := blocks.top()
				if top.BlkType() == ast.BLK_WHILE {
					blocks.pop()
				} else {
					comprehension = true
				}
				forblk := ast.NewIterBlock(ast.BLK_FOR, curpos, top.End(), iter)
				forblk.SetComprehension(comprehension)
				blocks.push(forblk)
				curblock = blocks.top()

				stack.Push(iter)
				stack.Push(curidx)
				stack.Push(nil)
			}
		case GET_AITER:
			{
				iter := stack.Top(1)
				stack.Pop()

				top := blocks.top()
				if top.BlkType() == ast.BLK_WHILE {
					blocks.pop()
					forblk := ast.NewIterBlock(ast.BLK_ASYNCFOR, curpos, top.End(), iter)
					blocks.push(forblk)
					curblock = blocks.top()
					stack.Push(nil)
				} else {
					fmt.Fprint(os.Stderr, "Unsupported use of GET_AITER outside of SETUP_LOOP\n")
				}
			}
		case GET_ANEXT:
		case TO_BOOL:
		case STORE_FAST_STORE_FAST_A:
			pendingOps = append(pendingOps, intPair{STORE_FAST_A, operand >> 4})
			if !fusedKeepFirst[curpos] {
				pendingOps = append(pendingOps, intPair{STORE_FAST_A, operand & 0xF})
			}
		case STORE_FAST_LOAD_FAST_A:
			pendingOps = append(pendingOps, intPair{STORE_FAST_A, operand >> 4})
			pendingOps = append(pendingOps, intPair{LOAD_FAST_A, operand & 0xF})
		case CONVERT_VALUE_A:
			{
				val := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewFormattedValue(val, operand, nil))
			}
		case FORMAT_SIMPLE:
			{
				val := stack.Top(1)
				if val == nil || val.Type() != ast.NODE_FORMATTEDVALUE {
					stack.Pop()
					stack.Push(ast.NewFormattedValue(val, ast.FV_NONE, nil))
				}
			}
		case FORMAT_WITH_SPEC:
			{
				spec := stack.Top(1)
				stack.Pop()
				val := stack.Top(1)
				stack.Pop()
				conv := ast.FV_NONE
				if val != nil && val.Type() == ast.NODE_FORMATTEDVALUE {
					fv := val.(*ast.FormattedValue)
					conv = fv.Conversion() & ast.FV_CONVERSION_MASK
					val = fv.Val()
				}
				stack.Push(ast.NewFormattedValue(val, conv|ast.FV_HAVE_FMT_SPEC, spec))
			}
		case CALL_KW_A:
			{
				names := stack.Top(1)
				stack.Pop()
				kwparamList := ast.NewKwNamesMap()
				if names != nil && names.Type() == ast.NODE_OBJECT {
					keys := names.(*ast.Object).Object().(*pyc.Seq).Values()
					n := len(keys)
					for i := 0; i < n; i++ {
						kwparamList.Add(ast.NewObject(keys[n-i-1]), stack.Top(1))
						stack.Pop()
					}
				}
				stack.Push(kwparamList)
				pendingOps = append(pendingOps, intPair{CALL_A, operand})
			}
		case FORMAT_VALUE_A:
			{
				conversionFlag := operand
				var formatSpec ast.Node
				if conversionFlag&ast.FV_HAVE_FMT_SPEC != 0 {
					formatSpec = stack.Top(1)
					stack.Pop()
				}
				val := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewFormattedValue(val, conversionFlag, formatSpec))
			}
		case GET_AWAITABLE, GET_AWAITABLE_A:
			{
				object := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewAwaitable(object))
			}
		case GET_ITER, GET_YIELD_FROM_ITER:
			sawGetIter = true
		case IMPORT_NAME_A:
			if mod.MajorVer() == 1 {
				stack.Push(ast.NewImport(ast.NewName(code.GetName(operand)), nil))
			} else {
				fromlist := stack.Top(1)
				stack.Pop()
				level := 0
				if mod.VerCompare(2, 5) >= 0 {
					levelNode := stack.Top(1)
					stack.Pop()
					if levelNode != nil && levelNode.Type() == ast.NODE_OBJECT {
						levelObj := levelNode.(*ast.Object).Object()
						if pyc.TypeOf(levelObj) == pyc.TypeInt {
							level = levelObj.(*pyc.Int).Value
						}
					}
				}
				imp := ast.NewImport(ast.NewName(code.GetName(operand)), fromlist)
				imp.SetLevel(level)
				stack.Push(imp)
			}
		case IMPORT_FROM_A:
			stack.Push(ast.NewName(code.GetName(operand)))
		case IMPORT_STAR:
			{
				imp := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewStore(imp, nil))
			}
		case IS_OP_A:
			{
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Pop()
				op := ast.CMP_IS
				if operand != 0 {
					op = ast.CMP_IS_NOT
				}
				stack.Push(ast.NewCompare(left, right, op))
			}
		case JUMP_IF_FALSE_A, JUMP_IF_TRUE_A, JUMP_IF_FALSE_OR_POP_A, JUMP_IF_TRUE_OR_POP_A,
			POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A, POP_JUMP_FORWARD_IF_FALSE_A,
			POP_JUMP_FORWARD_IF_TRUE_A, POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_FORWARD_IF_NOT_NONE_A,
			INSTRUMENTED_POP_JUMP_IF_FALSE_A, INSTRUMENTED_POP_JUMP_IF_TRUE_A:
			{
				if wend, ok := whileGuardEnd[curpos]; ok {
					wcond := stack.Top(1)
					stack.Pop()
					wneg := false
					if opcode == POP_JUMP_FORWARD_IF_NONE_A {
						wcond = ast.NewCompare(wcond, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
					} else if opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
						wcond = ast.NewCompare(wcond, ast.NewObject(pyc.None), ast.CMP_IS)
					} else if opcode == POP_JUMP_FORWARD_IF_TRUE_A || opcode == POP_JUMP_IF_TRUE_A {
						wneg = true
					}
					if chain, ok := loops.guardChain[curpos]; ok && len(chain) == len(guardChainConds) {
						res := wcond
						if wneg {
							res = ast.NewUnary(res, ast.UN_NOT)
						}
						wneg = false
						for ci := len(guardChainConds) - 1; ci >= 0; ci-- {
							cj := chain[ci]
							c := guardChainConds[ci]
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
					guardChainConds = nil
					wh := ast.NewCondBlock(ast.BLK_WHILE, wend, wcond, wneg)
					wh.InitV(ast.PRE_POPPED)
					blocks.push(wh)
					curblock = blocks.top()
					break
				}

				cond := stack.Top(1)
				var ifblk *ast.Block
				popped := ast.UNINITED

				if opcode == POP_JUMP_FORWARD_IF_NONE_A || opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
					stack.Pop()
					cmpop := ast.CMP_IS
					if opcode == POP_JUMP_FORWARD_IF_NONE_A {
						cmpop = ast.CMP_IS_NOT
					}
					cond = ast.NewCompare(cond, ast.NewObject(pyc.None), cmpop)
					popped = ast.PRE_POPPED
				}

				if len(matches) != 0 && matches[len(matches)-1].cblock == curblock &&
					matches[len(matches)-1].guardOpen && curblock.Size() == 0 &&
					opcode != JUMP_IF_FALSE_A && opcode != JUMP_IF_TRUE_A &&
					opcode != JUMP_IF_FALSE_OR_POP_A && opcode != JUMP_IF_TRUE_OR_POP_A {
					mc := matches[len(matches)-1]
					gt := operand
					if mod.VerCompare(3, 10) >= 0 {
						gt *= 2
					}
					if mod.VerCompare(3, 12) >= 0 || opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
						opcode == POP_JUMP_FORWARD_IF_FALSE_A {
						gt += pos
					}
					ni := matcher.IndexOf(gt)
					var isGuard bool
					if mc.guardTarget >= 0 {
						isGuard = gt == mc.guardTarget
					} else if mc.cur.bodyEnd >= 0 {
						isGuard = gt == mc.cur.nextPos
					} else {
						isGuard = mc.cur.nonLast || gt == mc.endPos ||
							(ni >= 0 && matcher.Instrs()[ni].op == NOP) ||
							(!mc.isDefault && mc.cblock.Size() == 0 && gt > pos)
					}
					if isGuard {
						stack.Pop()
						if opcode == POP_JUMP_IF_TRUE_A || opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
							opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A {
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

				if opcode == POP_JUMP_IF_FALSE_A || opcode == POP_JUMP_IF_TRUE_A ||
					opcode == POP_JUMP_FORWARD_IF_FALSE_A || opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
					opcode == INSTRUMENTED_POP_JUMP_IF_FALSE_A || opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A {
					stack.Pop()
					popped = ast.PRE_POPPED
				}

				stackHist = append(stackHist, stack.Copy())

				if opcode == JUMP_IF_FALSE_OR_POP_A || opcode == JUMP_IF_TRUE_OR_POP_A {
					stack.Pop()
					popped = ast.POPPED
				}

				neg := opcode == JUMP_IF_TRUE_A || opcode == JUMP_IF_TRUE_OR_POP_A ||
					opcode == POP_JUMP_IF_TRUE_A || opcode == POP_JUMP_FORWARD_IF_TRUE_A ||
					opcode == INSTRUMENTED_POP_JUMP_IF_TRUE_A

				offs := operand
				if mod.VerCompare(3, 10) >= 0 {
					offs *= 2
				}
				if mod.VerCompare(3, 12) >= 0 || opcode == JUMP_IF_FALSE_A || opcode == JUMP_IF_TRUE_A ||
					opcode == POP_JUMP_FORWARD_IF_TRUE_A || opcode == POP_JUMP_FORWARD_IF_FALSE_A ||
					opcode == POP_JUMP_FORWARD_IF_NONE_A || opcode == POP_JUMP_FORWARD_IF_NOT_NONE_A {
					base := pos
					if mod.VerCompare(3, 13) >= 0 {
						sp, pp := source.Pos(), pos
						for !source.AtEof() {
							p3 := pp
							o3, _ := pyc.BcNext(source, mod, &p3)
							if o3 != CACHE {
								break
							}
							pp = p3
						}
						source.SetPos(sp)
						base = pp
					}
					offs += base
				}

				if t, ok := xflow.cleanupDest[offs]; ok {
					offs = t
				}

				if ast.TypeOf(cond) == ast.NODE_COMPARE && cond.(*ast.Binary).Op() == ast.CMP_EXCEPTION {
					exceptEnd := offs
					if curblock.BlkType() == ast.BLK_EXCEPT && curblock.Cond() == nil {
						if offs <= pos {
							exceptEnd = curblock.End()
						}
						blocks.pop()
						curblock = blocks.top()

						stackHist = stackHist[:len(stackHist)-1]
					}
					ifblk = ast.NewCondBlock(ast.BLK_EXCEPT, exceptEnd, cond.(*ast.Binary).Right(), false)
				} else if curblock.BlkType() == ast.BLK_ELSE && curblock.Size() == 0 {
					blocks.pop()
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]
					ifblk = ast.NewCondBlock(ast.BLK_ELIF, offs, cond, neg)
				} else if curblock.Size() == 0 && curblock.Inited() == 0 && curblock.BlkType() == ast.BLK_WHILE {
					top := blocks.top()
					blocks.pop()
					ifblk = ast.NewCondBlock(top.BlkType(), offs, cond, neg)
					stackHist = stackHist[:len(stackHist)-1]
				} else if curblock.Size() == 0 && curblock.End() <= offs &&
					(curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF || curblock.BlkType() == ast.BLK_WHILE) &&
					blocks.size() > 1 {
					var newcond ast.Node
					top := curblock
					cond1 := top.Cond()
					blocks.pop()

					if curblock.BlkType() == ast.BLK_WHILE {
						stackHist = stackHist[:len(stackHist)-1]
					} else {
						sTop := stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-2]
						stackHist = append(stackHist, sTop)
					}

					if curblock.End() == offs || (curblock.End() == curpos && !top.Negative()) {
						newcond = ast.NewBinary(cond1, cond, ast.BIN_LOG_AND)
					} else {
						newcond = ast.NewBinary(cond1, cond, ast.BIN_LOG_OR)
					}
					ifblk = ast.NewCondBlock(top.BlkType(), offs, newcond, neg)
				} else if curblock.BlkType() == ast.BLK_FOR && curblock.IsComprehension() && mod.VerCompare(2, 7) >= 0 {
					curblock.SetCondition(cond)
					stackHist = stackHist[:len(stackHist)-1]
					break
				} else {
					ifblk = ast.NewCondBlock(ast.BLK_IF, offs, cond, neg)
				}

				if popped != 0 {
					ifblk.InitV(popped)
				}

				blocks.push(ifblk)
				curblock = blocks.top()
			}
		case POP_JUMP_BACKWARD_IF_TRUE_A, POP_JUMP_BACKWARD_IF_FALSE_A, POP_JUMP_BACKWARD_IF_NONE_A,
			POP_JUMP_BACKWARD_IF_NOT_NONE_A:
			{
				if whileBackedges[curpos] {
					hasWhile := false
					for i := len(blocks) - 1; i >= 0; i-- {
						if blocks[i].BlkType() == ast.BLK_WHILE {
							hasWhile = true
							break
						}
					}
					if hasWhile {
						if !stack.Empty() {
							stack.Pop()
						}
						for blocks.size() > 1 && curblock.BlkType() != ast.BLK_WHILE {
							inner := curblock
							blocks.pop()
							curblock = blocks.top()
							curblock.Append(inner)
						}
						if curblock.BlkType() == ast.BLK_WHILE && blocks.size() > 1 {
							wh := curblock
							blocks.pop()
							curblock = blocks.top()
							curblock.Append(wh)
						}
						break
					}
				}

				cond := stack.Top(1)
				stack.Pop()

				neg := false
				if opcode == POP_JUMP_BACKWARD_IF_NONE_A {
					cond = ast.NewCompare(cond, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
				} else if opcode == POP_JUMP_BACKWARD_IF_NOT_NONE_A {
					cond = ast.NewCompare(cond, ast.NewObject(pyc.None), ast.CMP_IS)
				} else if opcode == POP_JUMP_BACKWARD_IF_TRUE_A {
					neg = true
				}

				end := curblock.End()

				stackHist = append(stackHist, stack.Copy())
				ifblk := ast.NewCondBlock(ast.BLK_IF, end, cond, neg)
				ifblk.InitV(ast.PRE_POPPED)
				blocks.push(ifblk)
				curblock = blocks.top()
			}
		case JUMP_ABSOLUTE_A, JUMP_BACKWARD_A, JUMP_BACKWARD_NO_INTERRUPT_A:
			{
				offs := operand
				if mod.VerCompare(3, 10) >= 0 {
					offs *= 2
				}

				if len(spliceStack) != 0 && opcode == JUMP_BACKWARD_A &&
					curpos >= spliceStack[len(spliceStack)-1].handler && pos <= spliceStack[len(spliceStack)-1].regionEnd {
					sc := spliceStack[len(spliceStack)-1]
					dest := pos - operand*2
					if dest == sc.resume || (sc.backDest >= 0 && dest == sc.backDest) {
						operand = (sc.regionEnd - pos) / 2
						opcode = JUMP_FORWARD_A
						jumpForwardImpl()
						break
					}
				}
				if tej, ok := loops.tryEndJump[curpos]; ok &&
					(opcode == JUMP_ABSOLUTE_A || opcode == JUMP_BACKWARD_A) {
					rel := tej - pos
					if mod.VerCompare(3, 10) >= 0 {
						operand = rel / 2
					} else {
						operand = rel
					}
					opcode = JUMP_FORWARD_A
					jumpForwardImpl()
					break
				}

				if mod.VerCompare(3, 8) >= 0 && mod.VerCompare(3, 11) < 0 &&
					offs < pos &&
					(curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
					pos == curblock.End() {
					loopOwns := false
					for i := len(blocks) - 1; i >= 0; i-- {
						bt := blocks[i].BlkType()
						if bt == ast.BLK_FOR {
							loopOwns = blocks[i].End() == pos && !blocks[i].IsComprehension() &&
								offs == blocks[i].Start()
							break
						}
						if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
							break
						}
					}
					for loopOwns && (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
						blocks.size() > 1 {
						if len(stackHist) != 0 {
							stack = stackHist[len(stackHist)-1]
							stackHist = stackHist[:len(stackHist)-1]
						}
						inner := curblock
						blocks.pop()
						curblock = blocks.top()
						curblock.Append(inner)
					}
				}

				if len(inlineComps) != 0 && offs < pos &&
					(curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
					insideComprehensionAdd(blocks) {
					break
				}

				if offs < pos {
					if (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
						!stack.Empty() && stack.Top(1) != nil &&
						ast.TypeOf(stack.Top(1)) == ast.NODE_COMPREHENSION {
						ancestorIsCompFor := false
						for i := len(blocks) - 1; i >= 0; i-- {
							bt := blocks[i].BlkType()
							if bt == ast.BLK_FOR {
								ancestorIsCompFor = blocks[i].IsComprehension()
								break
							}
							if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
								break
							}
						}
						if ancestorIsCompFor {
							for (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
								blocks.size() > 1 {
								ifb := curblock
								c := ifb.Cond()
								if ifb.Negative() {
									c = ast.NewUnary(c, ast.UN_NOT)
								}
								blocks.pop()
								curblock = blocks.top()
								if curblock.BlkType() == ast.BLK_FOR {
									existing := curblock.Condition()
									var combined ast.Node
									if existing == nil {
										combined = c
									} else {
										combined = ast.NewBinary(existing, c, ast.BIN_LOG_AND)
									}
									curblock.SetCondition(combined)
								}
								if len(stackHist) != 0 {
									stackHist = stackHist[:len(stackHist)-1]
								}
							}
						}
					}

					if curblock.BlkType() == ast.BLK_FOR {
						var target int
						if mod.VerCompare(3, 11) >= 0 {
							target = pos - offs
						} else {
							target = offs
						}
						isJumpToStart := target == curblock.Start()
						atLoopEnd := curblock.End() != 0 && pos == curblock.End()
						if !atLoopEnd && isJumpToStart && curblock.End() > pos &&
							!curblock.IsComprehension() {
							look := pyc.NewBuffer([]byte(code.Code.Value))
							look.SetPos(pos)
							lp := pos
							onlyEpilogue, sawReturn := true, false
							for lp < curblock.End() && !look.AtEof() {
								lop, _ := pyc.BcNext(look, mod, &lp)
								if lop == RETURN_VALUE || lop == RETURN_CONST_A {
									sawReturn = true
								} else if lop != LOAD_CONST_A && lop != NOP && lop != CACHE {
									onlyEpilogue = false
								}
							}
							if onlyEpilogue && sawReturn && lp == curblock.End() {
								atLoopEnd = true
							}
						}
						shouldPopForBlock := curblock.IsComprehension()
						shouldAddForBlock := mod.MajorVer() == 3 && mod.MinorVer() >= 8 &&
							isJumpToStart && atLoopEnd && !curblock.IsComprehension()

						if !shouldPopForBlock && !shouldAddForBlock &&
							isJumpToStart && !atLoopEnd && !curblock.IsComprehension() {
							curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
							break
						}

						if shouldPopForBlock || shouldAddForBlock {
							top := stack.Top(1)

							if ast.TypeOf(top) == ast.NODE_COMPREHENSION {
								top.(*ast.Comprehension).AddGenerator(curblock)
							}

							tmp := curblock
							blocks.pop()
							curblock = blocks.top()
							if shouldAddForBlock {
								curblock.Append(tmp)
							}
						}
					} else if curblock.BlkType() == ast.BLK_ELSE {
						if mod.VerCompare(3, 11) >= 0 {
							loopEnd := -1
							for i := len(blocks) - 1; i >= 0; i-- {
								bt := blocks[i].BlkType()
								if bt == ast.BLK_FOR || bt == ast.BLK_WHILE {
									loopEnd = blocks[i].End()
									break
								}
							}
							if loopEnd > 0 && pos < loopEnd {
								curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
								break
							}
						}
						if len(stackHist) != 0 {
							stack = stackHist[len(stackHist)-1]
							stackHist = stackHist[:len(stackHist)-1]
						}

						blocks.pop()
						blocks.top().Append(curblock)
						curblock = blocks.top()

						if curblock.BlkType() == ast.BLK_CONTAINER && !curblock.HasFinally() {
							blocks.pop()
							blocks.top().Append(curblock)
							curblock = blocks.top()
						}
					} else {
						curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
					}

					break
				}

				if curblock.BlkType() == ast.BLK_CONTAINER {
					if curblock.HasExcept() && pos < curblock.Except() {
						except := ast.NewCondBlock(ast.BLK_EXCEPT, 0, nil, false)
						except.Init()
						blocks.push(except)
						curblock = blocks.top()
					}
					break
				}

				if len(stackHist) != 0 {
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]
				} else {
					fmt.Fprint(os.Stderr, "Warning: Stack history is empty, something wrong might have happened\n")
				}

				if blocks.size() == 1 && !source.AtEof() {
					fmt.Fprintf(os.Stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode&0xff)
					cleanBuild = false
					break
				}

				prev := curblock
				push := true

				for {
					if blocks.size() == 1 {
						fmt.Fprintf(os.Stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode&0xff)
						cleanBuild = false
						break
					}
					blocks.pop()

					blocks.top().Append(prev)

					if prev.BlkType() == ast.BLK_IF || prev.BlkType() == ast.BLK_ELIF {
						if push {
							stackHist = append(stackHist, stack.Copy())
						}
						next := ast.NewBlock(ast.BLK_ELSE, blocks.top().End(), 0)
						if prev.Inited() == ast.PRE_POPPED {
							next.InitV(ast.PRE_POPPED)
						}

						blocks.push(next)
						prev = nil
					} else if prev.BlkType() == ast.BLK_EXCEPT {
						if push {
							stackHist = append(stackHist, stack.Copy())
						}
						next := ast.NewCondBlock(ast.BLK_EXCEPT, blocks.top().End(), nil, false)
						next.Init()

						blocks.push(next)
						prev = nil
					} else if prev.BlkType() == ast.BLK_ELSE {
						prev = blocks.top()
						if !push {
							stack = stackHist[len(stackHist)-1]
							stackHist = stackHist[:len(stackHist)-1]
						}
						push = false
					} else {
						prev = nil
					}

					if prev == nil {
						break
					}
				}

				curblock = blocks.top()
			}
		case JUMP_FORWARD_A, INSTRUMENTED_JUMP_FORWARD_A:
			jumpForwardImpl()
		case CALL_INTRINSIC_1_A:
			switch operand {
			case 2:
				pendingOps = append(pendingOps, intPair{IMPORT_STAR, 0})
			case 3, 4:
			case 5:
				arg := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewUnary(arg, ast.UN_POSITIVE))
			case 6:
				pendingOps = append(pendingOps, intPair{LIST_TO_TUPLE, 0})
			default:
				fmt.Fprintf(os.Stderr, "Unsupported CALL_INTRINSIC_1 operand %d\n", operand)
			}
		case LIST_TO_TUPLE:
			{
				list := stack.Top(1)
				stack.Pop()
				if ast.TypeOf(list) == ast.NODE_LIST {
					lv := list.(*ast.List).Values()
					tv := make([]ast.Node, len(lv))
					copy(tv, lv)
					stack.Push(ast.NewTuple(tv))
				} else {
					stack.Push(list)
				}
			}
		case RETURN_GENERATOR:
			stack.Push(nil)
		case MAP_ADD_A:
			{
				value := stack.Top(1)
				stack.Pop()
				key := stack.Top(1)
				stack.Pop()

				if len(inlineComps) != 0 && curblock.BlkType() != ast.BLK_FOR {
					m := ast.NewMap()
					m.Add(key, value)
					curblock.Append(ast.NewReturn(m, ast.RET_YIELD))
				} else if insideComprehensionAdd(blocks) {
					stack.Pop()
					m := ast.NewMap()
					m.Add(key, value)
					stack.Push(ast.NewComprehension(m))
				} else {
					mp := stack.Top(1)
					if ast.TypeOf(mp) == ast.NODE_MAP {
						mp.(*ast.Map).Add(key, value)
					}
				}
			}
		case SET_ADD_A:
			{
				value := stack.Top(1)
				stack.Pop()

				set := stack.Top(1)

				if curblock.BlkType() == ast.BLK_FOR && curblock.IsComprehension() {
					stack.Pop()
					sc := ast.NewComprehension(value)
					sc.SetKind(ast.KIND_SET)
					stack.Push(sc)
				} else {
					cn := ""
					if code.Name != nil {
						cn = code.Name.Value
					}
					if (code.Name != nil && cn == "<setcomp>") || len(inlineComps) != 0 {
						curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
					} else {
						if ast.TypeOf(set) == ast.NODE_SET {
							old := set.(*ast.Set).Values()
							vals := make([]ast.Node, len(old), len(old)+1)
							copy(vals, old)
							vals = append(vals, value)
							stack.Pop()
							stack.Push(ast.NewSet(vals))
						} else {
							stack.Push(ast.NewSubscr(set, value))
						}
					}
				}
			}
		case LIST_APPEND, LIST_APPEND_A:
			{
				value := stack.Top(1)
				stack.Pop()

				list := stack.Top(1)

				if curblock.BlkType() == ast.BLK_FOR && curblock.IsComprehension() {
					stack.Pop()
					stack.Push(ast.NewComprehension(value))
				} else {
					cn := ""
					if code.Name != nil {
						cn = code.Name.Value
					}
					if (code.Name != nil && cn == "<listcomp>") || len(inlineComps) != 0 {
						curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
					} else {
						if ast.TypeOf(list) == ast.NODE_LIST {
							old := list.(*ast.List).Values()
							vals := make([]ast.Node, len(old), len(old)+1)
							copy(vals, old)
							vals = append(vals, value)
							stack.Pop()
							stack.Push(ast.NewList(vals))
						} else {
							stack.Push(ast.NewSubscr(list, value))
						}
					}
				}
			}
		case SET_UPDATE_A:
			{
				rhs := stack.Top(1)
				stack.Pop()
				lhs, _ := stack.Top(1).(*ast.Set)
				stack.Pop()

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

				stack.Push(ast.NewSet(result))
			}
		case DICT_UPDATE_A, DICT_MERGE_A:
			{
				rhs := stack.Top(1)
				stack.Pop()
				lhsNode := stack.Top(1)
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
				rhs := stack.Top(1)
				stack.Pop()
				lhs, _ := stack.Top(1).(*ast.List)
				stack.Pop()

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

				stack.Push(ast.NewList(result))
			}
		case LOAD_ATTR_A:
			{
				name := stack.Top(1)
				if ast.TypeOf(name) != ast.NODE_IMPORT {
					stack.Pop()

					if mod.VerCompare(3, 12) >= 0 {
						if operand&1 != 0 {
							stack.Push(nil)
						}
						operand >>= 1
					}

					stack.Push(ast.NewBinary(name, ast.NewName(code.GetName(operand)), ast.BIN_ATTR))
				}
			}
		case LOAD_COMMON_CONSTANT_A:
			{
				names := []string{"AssertionError", "NotImplementedError", "tuple", "all", "any"}
				if operand == 0 {
					pendingOps = append(pendingOps, intPair{LOAD_ASSERTION_ERROR, 0})
				} else if operand >= 1 && operand <= 4 {
					stack.Push(ast.NewName(pyc.NewString(names[operand])))
					if operand >= 2 {
						commonGuard = true
					}
				} else {
					fmt.Fprintf(os.Stderr, "Unsupported LOAD_COMMON_CONSTANT %d\n", operand)
				}
			}
		case LOAD_ASSERTION_ERROR:
			{
				assertErrNode = ast.NewName(pyc.NewString("AssertionError"))
				stack.Push(assertErrNode)
			}
		case LOAD_BUILD_CLASS:
			stack.Push(ast.NewLoadBuildClass(nil))
		case LOAD_CLOSURE_A:
			if mod.VerCompare(3, 6) >= 0 {
				stack.Push(ast.NewName(code.GetCellVar(mod, operand)))
			}
		case MAKE_CELL_A, COPY_FREE_VARS_A:
		case LOAD_CONST_A:
			{
				tOb := ast.NewObject(code.GetConst(operand))
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
					stack.Push(res)
				} else if (ot == pyc.TypeTuple || ot == pyc.TypeSmallTuple) &&
					len(tOb.Object().(*pyc.Seq).Values()) == 0 {
					stack.Push(ast.NewTuple(nil))
				} else if ot == pyc.TypeNone {
					stack.Push(nil)
				} else {
					stack.Push(tOb)
				}
			}
		case LOAD_DEREF_A, LOAD_CLASSDEREF_A:
			stack.Push(ast.NewName(code.GetCellVar(mod, operand)))
		case LOAD_FAST_AND_CLEAR_A:
			{
				k := 1
				var op2, arg2 int
				savedSrc := source.Pos()
				savedPos := pos
				ok := false
				container := 0
				for !source.AtEof() {
					op2, arg2 = pyc.BcNext(source, mod, &pos)
					if op2 == LOAD_FAST_AND_CLEAR_A {
						k++
						continue
					}
					if op2 != SWAP_A || arg2 != k+1 {
						break
					}
					op2, arg2 = pyc.BcNext(source, mod, &pos)
					if (op2 != BUILD_LIST_A && op2 != BUILD_SET_A && op2 != BUILD_MAP_A) || arg2 != 0 {
						break
					}
					container = op2
					op2, arg2 = pyc.BcNext(source, mod, &pos)
					ok = op2 == SWAP_A && arg2 == 2
					break
				}
				if !ok {
					source.SetPos(savedSrc)
					pos = savedPos
					stack.Push(nil)
					break
				}
				iter := stack.Top(1)
				stack.Pop()
				inlineBase = append(inlineBase, stack.Size())
				if container == BUILD_LIST_A {
					stack.Push(ast.NewList(nil))
				} else if container == BUILD_SET_A {
					stack.Push(ast.NewSet(nil))
				} else {
					stack.Push(ast.NewMap())
				}
				stack.Push(iter)
				inlineComps = append(inlineComps, k)
				if container == BUILD_SET_A {
					inlineKind = append(inlineKind, ast.KIND_SET)
				} else if container == BUILD_MAP_A {
					inlineKind = append(inlineKind, ast.KIND_DICT)
				} else {
					inlineKind = append(inlineKind, ast.KIND_LIST)
				}
			}
		case LOAD_SMALL_INT_A:
			stack.Push(ast.NewObject(pyc.NewInt(operand)))
		case LOAD_FAST_A, LOAD_FAST_BORROW_A, LOAD_FAST_CHECK_A:
			if mod.VerCompare(1, 3) < 0 {
				stack.Push(ast.NewName(code.GetName(operand)))
			} else {
				stack.Push(ast.NewName(code.GetLocal(operand)))
			}
		case LOAD_FAST_LOAD_FAST_A, LOAD_FAST_BORROW_LOAD_FAST_BORROW_A:
			stack.Push(ast.NewName(code.GetLocal(operand >> 4)))
			stack.Push(ast.NewName(code.GetLocal(operand & 0xF)))
		case LOAD_GLOBAL_A:
			if mod.VerCompare(3, 11) >= 0 {
				if operand&1 != 0 {
					stack.Push(nil)
				}
				operand >>= 1
			}
			stack.Push(ast.NewName(code.GetName(operand)))
		case LOAD_LOCALS:
			stack.Push(ast.NewLocals())
		case STORE_LOCALS:
			stack.Pop()
		case LOAD_METHOD_A:
			{
				name := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewBinary(name, ast.NewName(code.GetName(operand)), ast.BIN_ATTR))
			}
		case LOAD_NAME_A:
			stack.Push(ast.NewName(code.GetName(operand)))
		case MAKE_CLOSURE_A, MAKE_FUNCTION_A:
			{
				funCode := stack.Top(1)
				stack.Pop()

				tosType := -1
				if o, ok := funCode.(*ast.Object); ok {
					tosType = o.Object().Type()
				}
				if tosType != pyc.TypeCode && tosType != pyc.TypeCode2 {
					funCode = stack.Top(1)
					stack.Pop()
				}

				var defArgs, kwDefArgs []ast.Node

				if mod.VerCompare(3, 6) >= 0 {
					if operand&0x08 != 0 {
						stack.Pop()
					}
					if operand&0x04 != 0 {
						stack.Pop()
					}
					if operand&0x02 != 0 {
						kwd := stack.Top(1)
						stack.Pop()
						if kwd != nil && ast.TypeOf(kwd) == ast.NODE_MAP {
							for _, it := range kwd.(*ast.Map).Values() {
								kwDefArgs = append(kwDefArgs, it.V)
							}
						} else if kwd != nil && ast.TypeOf(kwd) == ast.NODE_CONST_MAP {
							kwDefArgs = append(kwDefArgs, kwd.(*ast.ConstMap).Values()...)
						}
					}
					if operand&0x01 != 0 {
						defs := stack.Top(1)
						stack.Pop()
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
					defCount := operand & 0xFF
					kwDefCount := (operand >> 8) & 0xFF
					for i := 0; i < defCount; i++ {
						defArgs = pushFront(defArgs, stack.Top(1))
						stack.Pop()
					}
					for i := 0; i < kwDefCount; i++ {
						kwDefArgs = pushFront(kwDefArgs, stack.Top(1))
						stack.Pop()
					}
				}
				stack.Push(ast.NewFunction(funCode, defArgs, kwDefArgs))
			}
		case MAKE_FUNCTION:
			{
				funCode := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewFunction(funCode, nil, nil))
			}
		case SET_FUNCTION_ATTRIBUTE_A:
			{
				fn0 := stack.Top(1)
				stack.Pop()
				attr := stack.Top(1)
				stack.Pop()
				if fn0 == nil || ast.TypeOf(fn0) != ast.NODE_FUNCTION {
					stack.Push(fn0)
					break
				}
				fn := fn0.(*ast.Function)
				defArgs := append([]ast.Node(nil), fn.Defargs()...)
				kwDefArgs := append([]ast.Node(nil), fn.Kwdefargs()...)
				if operand == 0x01 {
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
				} else if operand == 0x02 {
					kwDefArgs = nil
					if attr != nil && ast.TypeOf(attr) == ast.NODE_MAP {
						for _, it := range attr.(*ast.Map).Values() {
							kwDefArgs = append(kwDefArgs, it.V)
						}
					} else if attr != nil && ast.TypeOf(attr) == ast.NODE_CONST_MAP {
						kwDefArgs = append(kwDefArgs, attr.(*ast.ConstMap).Values()...)
					}
				}
				stack.Push(ast.NewFunction(fn.Code(), defArgs, kwDefArgs))
			}
		case POP_ITER:
		case NOT_TAKEN, NOP:
		case POP_BLOCK:
			{
				if curblock.BlkType() == ast.BLK_CONTAINER || curblock.BlkType() == ast.BLK_FINALLY {
					break
				}

				if curblock.BlkType() == ast.BLK_WITH {
					break
				}

				if curblock.Size() != 0 &&
					ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_KEYWORD {
					curblock.RemoveLast()
				}

				if curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF ||
					curblock.BlkType() == ast.BLK_ELSE || curblock.BlkType() == ast.BLK_TRY ||
					curblock.BlkType() == ast.BLK_EXCEPT || curblock.BlkType() == ast.BLK_FINALLY {
					if len(stackHist) != 0 {
						stack = stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-1]
					} else {
						fmt.Fprint(os.Stderr, "Warning: Stack history is empty, something wrong might have happened\n")
					}
				}
				tmp := curblock
				blocks.pop()

				if !blocks.empty() {
					curblock = blocks.top()
				}

				if !(tmp.BlkType() == ast.BLK_ELSE && tmp.Size() == 0) {
					curblock.Append(tmp)
				}

				if tmp.BlkType() == ast.BLK_FOR && tmp.End() >= pos {
					stackHist = append(stackHist, stack.Copy())

					blkelse := ast.NewBlock(ast.BLK_ELSE, tmp.End(), 0)
					blocks.push(blkelse)
					curblock = blocks.top()
				}

				if curblock.BlkType() == ast.BLK_TRY &&
					tmp.BlkType() != ast.BLK_FOR && tmp.BlkType() != ast.BLK_ASYNCFOR &&
					tmp.BlkType() != ast.BLK_WHILE {
					if len(stackHist) != 0 {
						stack = stackHist[len(stackHist)-1]
						stackHist = stackHist[:len(stackHist)-1]
					}

					tmp = curblock
					blocks.pop()
					curblock = blocks.top()

					if !(tmp.BlkType() == ast.BLK_ELSE && tmp.Size() == 0) {
						curblock.Append(tmp)
					}
				}

				if curblock.BlkType() == ast.BLK_CONTAINER {
					cont := curblock

					if tmp.BlkType() == ast.BLK_ELSE && !cont.HasFinally() {
						blocks.pop()
						curblock = blocks.top()
						curblock.Append(cont)
					} else if (tmp.BlkType() == ast.BLK_ELSE && cont.HasFinally()) ||
						(tmp.BlkType() == ast.BLK_TRY && !cont.HasExcept()) {
						stackHist = append(stackHist, stack.Copy())

						final := ast.NewBlock(ast.BLK_FINALLY, 0, 1)
						blocks.push(final)
						curblock = blocks.top()
					}
				}

				if (curblock.BlkType() == ast.BLK_FOR || curblock.BlkType() == ast.BLK_ASYNCFOR) &&
					curblock.End() == pos {
					blocks.pop()
					blocks.top().Append(curblock)
					curblock = blocks.top()
				}
			}
		case POP_EXCEPT:
		case PUSH_EXC_INFO:
			stack.Push(ast.NewName(pyc.NewString(excSentinel)))
		case CHECK_EXC_MATCH:
			{
				right := stack.Top(1)
				stack.Pop()
				left := stack.Top(1)
				stack.Push(ast.NewCompare(left, right, ast.CMP_EXCEPTION))
			}
		case END_FOR:
			{
				inlineCompLoop := len(inlineComps) != 0
				if inlineCompLoop &&
					(curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELIF) &&
					insideComprehensionAdd(blocks) {
					for blocks.size() > 1 &&
						!(curblock.BlkType() == ast.BLK_FOR && curblock.IsComprehension()) {
						inner := curblock
						blocks.pop()
						curblock = blocks.top()
						curblock.Append(inner)
					}
					if curblock.BlkType() == ast.BLK_FOR && blocks.size() > 1 {
						fb := curblock
						blocks.pop()
						curblock = blocks.top()
						curblock.Append(fb)
					}
				}
				if inlineCompLoop {
					type opRange struct{ start, end, weight int }
					k := inlineComps[len(inlineComps)-1]
					sSrc, sPos := source.Pos(), pos
					var o2, a2 int
					var seq []opRange
					finishing := false
					lastP := pos
					if !source.AtEof() {
						o2, a2 = pyc.BcNext(source, mod, &pos)
						if o2 == POP_ITER || (o2 == POP_TOP && mod.VerCompare(3, 13) == 0) {
							cleanupSkip = append(cleanupSkip, intPair{lastP, pos})
							lastP = pos
							if !source.AtEof() {
								o2, a2 = pyc.BcNext(source, mod, &pos)
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
						seq = append(seq, opRange{lastP, pos, 0})
						cnt := 0
						for !source.AtEof() && cnt < k {
							st := pos
							o2, a2 = pyc.BcNext(source, mod, &pos)
							w := storeWeight(o2)
							if w == 0 {
								break
							}
							seq = append(seq, opRange{st, pos, w})
							cnt += w
						}
						finishing = cnt == k
					} else if storeWeight(o2) != 0 {
						var stores []opRange
						stores = append(stores, opRange{lastP, pos, storeWeight(o2)})
						for !source.AtEof() {
							st := pos
							o2, a2 = pyc.BcNext(source, mod, &pos)
							w := storeWeight(o2)
							if w == 0 {
								break
							}
							stores = append(stores, opRange{st, pos, w})
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
									fusedKeepFirst[st.start] = true
									need = 0
								}
							}
						}
					}
					_ = a2
					source.SetPos(sSrc)
					pos = sPos
					if finishing {
						for _, r := range seq {
							cleanupSkip = append(cleanupSkip, intPair{r.start, r.end})
						}
						inlineComps = inlineComps[:len(inlineComps)-1]
						inlineFinish = true
						inlineFinishBase = inlineBase[len(inlineBase)-1]
						inlineBase = inlineBase[:len(inlineBase)-1]
						inlineFinishKind = inlineKind[len(inlineKind)-1]
						inlineKind = inlineKind[:len(inlineKind)-1]
					}
				}
				if !inlineCompLoop {
					stack.Pop()
				}

				if !inlineCompLoop && opcode == END_FOR && mod.MajorVer() == 3 && mod.MinorVer() == 12 {
					stack.Pop()
				}

				if inlineCompLoop {
				} else if curblock.BlkType() == ast.BLK_FOR &&
					(curblock.End() == curpos || curblock.End() == 0 || curblock.End() < curpos) {
					prev := blocks.top()
					blocks.pop()

					curblock = blocks.top()
					curblock.Append(prev)
				} else if curblock.BlkType() == ast.BLK_FOR {
				} else if curblock.BlkType() != ast.BLK_MAIN && curblock.BlkType() != ast.BLK_WHILE &&
					curblock.BlkType() != ast.BLK_IF && curblock.BlkType() != ast.BLK_ELIF &&
					curblock.BlkType() != ast.BLK_ELSE {
					fmt.Fprintf(os.Stderr, "Wrong block type %d for END_FOR\n", curblock.BlkType())
				}

				if inlineFinish {
					inlineFinish = false
					var res ast.Node
					if !stack.Empty() && stack.Top(1) != nil && ast.TypeOf(stack.Top(1)) == ast.NODE_COMPREHENSION {
						res = stack.Top(1)
					} else if curblock.Size() > 0 {
						synth := synthGenexpr(curblock.Nodes()[curblock.Size()-1])
						if synth != nil {
							curblock.RemoveLast()
							res = synth
						}
					}
					if res != nil && ast.TypeOf(res) == ast.NODE_COMPREHENSION {
						res.(*ast.Comprehension).SetKind(inlineFinishKind)
					}
					if res != nil && stack.Size() > inlineFinishBase {
						for stack.Size() > inlineFinishBase {
							stack.Pop()
						}
						stack.Push(res)
					}
				}
			}
		case BUILD_LIST_UNPACK_A, BUILD_TUPLE_UNPACK_A, BUILD_TUPLE_UNPACK_WITH_CALL_A, BUILD_SET_UNPACK_A:
			{
				count := operand & 0xff
				items := make([]ast.Node, count)
				for i := count - 1; i >= 0; i-- {
					items[i] = stack.Top(1)
					stack.Pop()
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
					} else if item != nil && ast.TypeOf(item) == ast.NODE_LIST && opcode == BUILD_LIST_UNPACK_A {
						out = append(out, item.(*ast.List).Values()...)
					} else if item != nil && ast.TypeOf(item) == ast.NODE_SET && opcode == BUILD_SET_UNPACK_A {
						out = append(out, item.(*ast.Set).Values()...)
					} else {
						out = append(out, ast.NewUnary(item, ast.UN_UNPACK))
					}
				}
				if opcode == BUILD_LIST_UNPACK_A {
					stack.Push(ast.NewList(out))
				} else if opcode == BUILD_SET_UNPACK_A {
					stack.Push(ast.NewSet(out))
				} else {
					stack.Push(ast.NewTuple(out))
				}
			}
		case BUILD_MAP_UNPACK_A, BUILD_MAP_UNPACK_WITH_CALL_A:
			{
				count := operand & 0xff
				items := make([]ast.Node, count)
				for i := count - 1; i >= 0; i-- {
					items[i] = stack.Top(1)
					stack.Pop()
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
				stack.Push(result)
			}
		case POP_TOP:
			{
				if len(matches) != 0 && matches[len(matches)-1].cblock == curblock &&
					matches[len(matches)-1].skipPop {
					matches[len(matches)-1].skipPop = false
					matches[len(matches)-1].guardOpen = false
					break
				}

				value := stack.Top(1)
				stack.Pop()

				if mod.VerCompare(3, 12) >= 0 && value != nil {
					var fb *ast.Block
					for i := len(blocks) - 1; i >= 0; i-- {
						if blocks[i].BlkType() == ast.BLK_FOR {
							fb = blocks[i]
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
					if curblock.Inited() == 0 {
						curblock.Init()
					}
					break
				}

				if curblock.Inited() == 0 {
					if curblock.BlkType() == ast.BLK_WITH {
						curblock.SetExpr(value)
					} else {
						curblock.Init()
					}
					break
				} else if value == nil || value.Processed() {
					break
				}

				curblock.Append(value)

				if curblock.BlkType() == ast.BLK_FOR && curblock.IsComprehension() {
					if ast.TypeOf(value) == ast.NODE_CALL {
						pparams := value.(*ast.Call).Pparams()
						if len(pparams) != 0 {
							stack.Push(ast.NewComprehension(pparams[0]))
						}
					}
				}
			}
		case PRINT_ITEM:
			{
				var printNode *ast.Print
				if curblock.Size() > 0 && ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_PRINT {
					printNode = curblock.Nodes()[curblock.Size()-1].(*ast.Print)
				}
				if printNode != nil && printNode.Stream() == nil && !printNode.Eol() {
					printNode.Add(stack.Top(1))
				} else {
					curblock.Append(ast.NewPrint(stack.Top(1), nil))
				}
				stack.Pop()
			}
		case PRINT_ITEM_TO:
			{
				stream := stack.Top(1)
				stack.Pop()

				var printNode *ast.Print
				if curblock.Size() > 0 && ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_PRINT {
					printNode = curblock.Nodes()[curblock.Size()-1].(*ast.Print)
				}
				if printNode != nil && printNode.Stream() == stream && !printNode.Eol() {
					printNode.Add(stack.Top(1))
				} else {
					curblock.Append(ast.NewPrint(stack.Top(1), stream))
				}
				stack.Pop()
				if stream != nil {
					stream.SetProcessed()
				}
			}
		case PRINT_NEWLINE:
			{
				var printNode *ast.Print
				if curblock.Size() > 0 && ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_PRINT {
					printNode = curblock.Nodes()[curblock.Size()-1].(*ast.Print)
				}
				if printNode != nil && printNode.Stream() == nil && !printNode.Eol() {
					printNode.SetEol(true)
				} else {
					curblock.Append(ast.NewPrint(nil, nil))
				}
				stack.Pop()
			}
		case PRINT_NEWLINE_TO:
			{
				stream := stack.Top(1)
				stack.Pop()

				var printNode *ast.Print
				if curblock.Size() > 0 && ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_PRINT {
					printNode = curblock.Nodes()[curblock.Size()-1].(*ast.Print)
				}
				if printNode != nil && printNode.Stream() == stream && !printNode.Eol() {
					printNode.SetEol(true)
				} else {
					curblock.Append(ast.NewPrint(nil, stream))
				}
				stack.Pop()
				if stream != nil {
					stream.SetProcessed()
				}
			}
		case RAISE_VARARGS_A:
			{
				var paramList []ast.Node
				for i := 0; i < operand; i++ {
					paramList = pushFront(paramList, stack.Top(1))
					stack.Pop()
				}
				curblock.Append(ast.NewRaise(paramList))

				if (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELSE) &&
					len(stackHist) != 0 && blocks.size() > 1 && mod.VerCompare(2, 6) >= 0 {
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]

					prev := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(prev)
				}
			}
		case RERAISE, RERAISE_A:
		case RETURN_VALUE, INSTRUMENTED_RETURN_VALUE_A:
			if finallySuppressReturns[curpos] {
				break
			}
			{
				value := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewReturn(value, ast.RET_RETURN))

				if (curblock.BlkType() == ast.BLK_IF || curblock.BlkType() == ast.BLK_ELSE) &&
					len(stackHist) != 0 && blocks.size() > 1 && mod.VerCompare(2, 6) >= 0 {
					stack = stackHist[len(stackHist)-1]
					stackHist = stackHist[:len(stackHist)-1]

					prev := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(prev)

					if !source.AtEof() {
						savedSrc := source.Pos()
						savedPos := pos
						pkOp, pkOperand := pyc.BcNext(source, mod, &pos)
						mul := 1
						if mod.VerCompare(3, 10) >= 0 {
							mul = 2
						}
						jt := pkOperand * mul
						keepNext := !(pkOp == JUMP_FORWARD_A || (pkOp == JUMP_ABSOLUTE_A && jt > savedPos))
						if keepNext {
							source.SetPos(savedSrc)
							pos = savedPos
						} else {
							opcode = pkOp
							operand = pkOperand
						}
					}
				}
			}
		case RETURN_CONST_A, INSTRUMENTED_RETURN_CONST_A:
			{
				value := ast.NewObject(code.GetConst(operand))
				curblock.Append(ast.NewReturn(value, ast.RET_RETURN))
			}
		case ROT_TWO:
			{
				{
					sSrc, sPos := source.Pos(), pos
					var o2, o3 int
					if !source.AtEof() {
						o2, _ = pyc.BcNext(source, mod, &pos)
						if !source.AtEof() {
							o3, _ = pyc.BcNext(source, mod, &pos)
						}
					}
					inFor := false
					for i := len(blocks) - 1; i >= 0; i-- {
						if blocks[i].BlkType() == ast.BLK_FOR {
							inFor = !blocks[i].IsComprehension()
							break
						}
					}
					if inFor && o2 == POP_TOP && o3 == RETURN_VALUE {
						source.SetPos(sSrc)
						pos = sPos
						pyc.BcNext(source, mod, &pos)
						break
					}
					source.SetPos(sSrc)
					pos = sPos
				}
				{
					sSrc, sPos := source.Pos(), pos
					var o2 int
					storeNext := false
					if !source.AtEof() {
						o2, _ = pyc.BcNext(source, mod, &pos)
						storeNext = o2 == STORE_FAST_A || o2 == STORE_NAME_A ||
							o2 == STORE_GLOBAL_A || o2 == STORE_DEREF_A ||
							o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A
					}
					source.SetPos(sSrc)
					pos = sPos
					if storeNext && stack.Size() >= 2 && stack.Top(2) != nil &&
						ast.TypeOf(stack.Top(2)) != ast.NODE_CHAINSTORE &&
						stack.Top(1) != nil &&
						ast.TypeOf(stack.Top(1)) != ast.NODE_CHAINSTORE {
						pendingOps = append(pendingOps, intPair{SWAP_A, 2})
						break
					}
				}
				one := stack.Top(1)
				stack.Pop()
				if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					stack.Pop()
				}
				two := stack.Top(1)
				stack.Pop()

				stack.Push(one)
				stack.Push(two)
			}
		case ROT_THREE:
			{
				if unpack == 0 && stack.Size() >= 3 && !source.AtEof() {
					sSrc, sPos := source.Pos(), pos
					var o2, o3 int
					o2, _ = pyc.BcNext(source, mod, &pos)
					if o2 == ROT_TWO && !source.AtEof() {
						o3, _ = pyc.BcNext(source, mod, &pos)
					}
					parallel := o2 == ROT_TWO &&
						(o3 == STORE_FAST_A || o3 == STORE_NAME_A || o3 == STORE_GLOBAL_A ||
							o3 == STORE_DEREF_A || o3 == STORE_ATTR_A || o3 == STORE_SUBSCR)
					for k := 1; parallel && k <= 3; k++ {
						parallel = stack.Top(k) != nil && ast.TypeOf(stack.Top(k)) != ast.NODE_CHAINSTORE
					}
					if parallel {
						source.SetPos(sSrc)
						pos = sPos
						pyc.BcNext(source, mod, &pos)
						unpack = 3
						values := make([]ast.Node, 3)
						for i := 0; i < 3; i++ {
							values[2-i] = stack.Top(1)
							stack.Pop()
						}
						tup := ast.NewTuple(values)
						tup.SetRequireParens(false)
						nextTup := ast.NewTuple(nil)
						nextTup.SetRequireParens(false)
						stack.Push(tup)
						stack.Push(nextTup)
						break
					}
					source.SetPos(sSrc)
					pos = sPos
				}
				one := stack.Top(1)
				stack.Pop()
				two := stack.Top(1)
				stack.Pop()
				if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					stack.Pop()
				}
				three := stack.Top(1)
				stack.Pop()
				stack.Push(one)
				stack.Push(three)
				stack.Push(two)
			}
		case ROT_FOUR:
			{
				one := stack.Top(1)
				stack.Pop()
				two := stack.Top(1)
				stack.Pop()
				three := stack.Top(1)
				stack.Pop()
				if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
					stack.Pop()
				}
				four := stack.Top(1)
				stack.Pop()
				stack.Push(one)
				stack.Push(four)
				stack.Push(three)
				stack.Push(two)
			}
		case SET_LINENO_A:
		case SETUP_WITH_A:
			{
				withblock := ast.NewWithBlock(pos + operand)
				blocks.push(withblock)
				curblock = blocks.top()
			}
		case WITH_EXCEPT_START:
			if !stack.Empty() && isExcSentinel(stack.Top(1)) {
				stack.Pop()
			}
			stack.Push(nil)
		case BEFORE_WITH, BEFORE_ASYNC_WITH:
			{
				if end, ok := withBodyEndByBefore[curpos]; ok {
					withblock := ast.NewWithBlock(end)
					blocks.push(withblock)
					curblock = blocks.top()
				}
			}
		case WITH_CLEANUP, WITH_CLEANUP_START:
			{
				none := stack.Top(1)
				stack.Pop()

				if none != nil {
					fmt.Fprint(os.Stderr, "Something TERRIBLE happened!\n")
					break
				}

				if curblock.BlkType() == ast.BLK_WITH && curblock.End() == curpos {
					with := curblock
					blocks.pop()
					curblock = blocks.top()
					curblock.Append(with)
				} else {
					fmt.Fprintf(os.Stderr, "Something TERRIBLE happened! No matching with block found for WITH_CLEANUP at %d\n", curpos)
				}
			}
		case WITH_CLEANUP_FINISH:
		case SETUP_EXCEPT_A:
			{
				if curblock.BlkType() == ast.BLK_CONTAINER {
					curblock.SetExcept(pos + operand)
				} else {
					next := ast.NewContainerBlock(0, pos+operand)
					blocks.push(next)
				}

				stackHist = append(stackHist, stack.Copy())
				tryblock := ast.NewBlock(ast.BLK_TRY, pos+operand, 1)
				blocks.push(tryblock)
				curblock = blocks.top()

				needTry = false
			}
		case SETUP_FINALLY_A:
			{
				if mod.VerCompare(3, 8) >= 0 && mod.VerCompare(3, 11) < 0 {
					off := operand
					if mod.VerCompare(3, 10) >= 0 {
						off = operand * 2
					}
					look := pyc.NewBuffer([]byte(code.Code.Value))
					target := pos + off
					if target < len(code.Code.Value) {
						look.SetPos(target)
						lp := target
						lop, _ := pyc.BcNext(look, mod, &lp)
						if lop == DUP_TOP || lop == POP_TOP {
							pendingOps = append(pendingOps, intPair{SETUP_EXCEPT_A, off})
							break
						}
					}
				}
				next := ast.NewContainerBlock(pos+operand, 0)
				blocks.push(next)
				curblock = blocks.top()

				needTry = true
			}
		case SETUP_LOOP_A:
			{
				next := ast.NewCondBlock(ast.BLK_WHILE, pos+operand, nil, false)
				blocks.push(next)
				curblock = blocks.top()
			}
		case SLICE_0:
			{
				name := stack.Top(1)
				stack.Pop()

				slice := ast.NewSlice(ast.SLICE0, nil, nil)
				stack.Push(ast.NewSubscr(name, slice))
			}
		case SLICE_1:
			{
				lower := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()

				slice := ast.NewSlice(ast.SLICE1, lower, nil)
				stack.Push(ast.NewSubscr(name, slice))
			}
		case SLICE_2:
			{
				upper := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()

				slice := ast.NewSlice(ast.SLICE2, nil, upper)
				stack.Push(ast.NewSubscr(name, slice))
			}
		case SLICE_3:
			{
				upper := stack.Top(1)
				stack.Pop()
				lower := stack.Top(1)
				stack.Pop()
				name := stack.Top(1)
				stack.Pop()

				slice := ast.NewSlice(ast.SLICE3, lower, upper)
				stack.Push(ast.NewSubscr(name, slice))
			}
		case STORE_ATTR_A:
			{
				if unpack != 0 {
					name := stack.Top(1)
					stack.Pop()
					attr := ast.NewBinary(name, ast.NewName(code.GetName(operand)), ast.BIN_ATTR)
					unpackAdd(attr)
				} else {
					name := stack.Top(1)
					stack.Pop()
					value := stack.Top(1)
					stack.Pop()
					attr := ast.NewBinary(name, ast.NewName(code.GetName(operand)), ast.BIN_ATTR)
					if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
						appendToChainStore(value, attr, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(value, attr))
					}
				}
			}
		case STORE_DEREF_A:
			{
				if unpack != 0 {
					name := ast.NewName(code.GetCellVar(mod, operand))
					unpackAdd(name)
				} else {
					value := stack.Top(1)
					stack.Pop()
					derefName := code.GetCellVar(mod, operand)
					name := ast.NewName(derefName)
					if code.IsFreeVar(mod, operand) {
						code.MarkNonlocal(derefName)
					}

					if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
						appendToChainStore(value, name, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(value, name))
					}
				}
			}
		case STORE_FAST_A:
			{
				if unpack != 0 {
					var name ast.Node
					if mod.VerCompare(1, 3) < 0 {
						name = ast.NewName(code.GetName(operand))
					} else {
						name = ast.NewName(code.GetLocal(operand))
					}
					unpackAdd(name)
				} else {
					value := stack.Top(1)
					stack.Pop()
					var name *ast.Name
					if mod.VerCompare(1, 3) < 0 {
						name = ast.NewName(code.GetName(operand))
					} else {
						name = ast.NewName(code.GetLocal(operand))
					}

					nv := name.Name().Value
					if len(nv) >= 2 && nv[0] == '_' && nv[1] == '[' {
						break
					}

					if handleExceptBinding(curblock, value, name) {
						break
					}

					if curblock.BlkType() == ast.BLK_FOR && curblock.Inited() == 0 {
						curblock.SetIndex(name)
					} else if !stack.Empty() && stack.Top(1) != nil &&
						ast.TypeOf(stack.Top(1)) == ast.NODE_IMPORT {
						imp := stack.Top(1).(*ast.Import)
						imp.AddStore(ast.NewStore(value, name))
					} else if curblock.BlkType() == ast.BLK_WITH && curblock.Inited() == 0 {
						curblock.SetExpr(value)
						curblock.SetVar(name)
					} else if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
						appendToChainStore(value, name, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(value, name))
					}
				}
			}
		case STORE_GLOBAL_A:
			{
				name := ast.NewName(code.GetName(operand))

				if unpack != 0 {
					unpackAdd(name)
				} else {
					value := stack.Top(1)
					stack.Pop()
					if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
						appendToChainStore(value, name, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(value, name))
					}
				}

				code.MarkGlobal(name.Name())
			}
		case STORE_NAME_A:
			{
				if unpack != 0 {
					name := ast.NewName(code.GetName(operand))
					unpackAdd(name)
				} else {
					value := stack.Top(1)
					stack.Pop()

					varname := code.GetName(operand)
					if len(varname.Value) >= 2 && varname.Value[0] == '_' && varname.Value[1] == '[' {
						break
					}

					classPrefix := "_" + code.Name.Value
					if varname.StartsWith(classPrefix + "__") {
						varname.Value = varname.Value[len(classPrefix):]
					}

					name := ast.NewName(varname)

					if handleExceptBinding(curblock, value, name) {
						break
					}

					if curblock.BlkType() == ast.BLK_FOR && curblock.Inited() == 0 {
						curblock.SetIndex(name)
					} else if ast.TypeOf(stack.Top(1)) == ast.NODE_IMPORT {
						imp := stack.Top(1).(*ast.Import)
						imp.AddStore(ast.NewStore(value, name))
					} else if curblock.BlkType() == ast.BLK_WITH && curblock.Inited() == 0 {
						curblock.SetExpr(value)
						curblock.SetVar(name)
					} else if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
						appendToChainStore(value, name, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(value, name))
					}
				}
			}
		case STORE_SLICE_0:
			{
				dest := stack.Top(1)
				stack.Pop()
				value := stack.Top(1)
				stack.Pop()

				curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE0, nil, nil))))
			}
		case STORE_SLICE_1:
			{
				upper := stack.Top(1)
				stack.Pop()
				dest := stack.Top(1)
				stack.Pop()
				value := stack.Top(1)
				stack.Pop()

				curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE1, upper, nil))))
			}
		case STORE_SLICE_2:
			{
				lower := stack.Top(1)
				stack.Pop()
				dest := stack.Top(1)
				stack.Pop()
				value := stack.Top(1)
				stack.Pop()

				curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE2, nil, lower))))
			}
		case STORE_SLICE_3:
			{
				lower := stack.Top(1)
				stack.Pop()
				upper := stack.Top(1)
				stack.Pop()
				dest := stack.Top(1)
				stack.Pop()
				value := stack.Top(1)
				stack.Pop()

				curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE3, upper, lower))))
			}
		case STORE_SUBSCR:
			{
				if unpack != 0 {
					subscr := stack.Top(1)
					stack.Pop()
					dest := stack.Top(1)
					stack.Pop()

					save := ast.NewSubscr(dest, subscr)
					unpackAdd(save)
				} else {
					subscr := stack.Top(1)
					stack.Pop()
					dest := stack.Top(1)
					stack.Pop()
					src := stack.Top(1)
					stack.Pop()

					foundAnnotatedVar := false
					if variableAnnotations && ast.TypeOf(dest) == ast.NODE_NAME {
						foundAnnotatedVar = dest.(*ast.Name).Name().Value == "__annotations__"
					}

					if foundAnnotatedVar {
						if curblock.Size() != 0 && ast.TypeOf(curblock.Nodes()[curblock.Size()-1]) == ast.NODE_STORE {
							store := curblock.Nodes()[curblock.Size()-1].(*ast.Store)
							curblock.RemoveLast()
							curblock.Append(ast.NewStore(store.Src(), ast.NewAnnotatedVar(subscr, src)))
						} else {
							curblock.Append(ast.NewAnnotatedVar(subscr, src))
						}
					} else {
						if ast.TypeOf(dest) == ast.NODE_MAP {
							dest.(*ast.Map).Add(subscr, src)
						} else if ast.TypeOf(src) == ast.NODE_CHAINSTORE {
							appendToChainStore(src, ast.NewSubscr(dest, subscr), stack, curblock)
						} else {
							curblock.Append(ast.NewStore(src, ast.NewSubscr(dest, subscr)))
						}
					}
				}
			}
		case UNARY_CALL:
			{
				fn := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewCall(fn, nil, nil))
			}
		case UNARY_CONVERT:
			{
				name := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewConvert(name))
			}
		case UNARY_INVERT:
			{
				arg := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewUnary(arg, ast.UN_INVERT))
			}
		case UNARY_NEGATIVE:
			{
				arg := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewUnary(arg, ast.UN_NEGATIVE))
			}
		case UNARY_NOT:
			{
				arg := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewUnary(arg, ast.UN_NOT))
			}
		case UNARY_POSITIVE:
			{
				arg := stack.Top(1)
				stack.Pop()
				stack.Push(ast.NewUnary(arg, ast.UN_POSITIVE))
			}
		case UNPACK_EX_A:
			unpackBegin((operand&0xff)+(operand>>8)+1, operand&0xff)
		case UNPACK_LIST_A, UNPACK_TUPLE_A, UNPACK_SEQUENCE_A:
			{
				if operand > 0 {
					unpackBegin(operand, -1)
				} else {
					tup := ast.NewTuple(nil)
					if curblock.BlkType() == ast.BLK_FOR && curblock.Inited() == 0 {
						tup.SetRequireParens(true)
						curblock.SetIndex(tup)
					} else if ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
						chainStore := stack.Top(1)
						stack.Pop()
						appendToChainStore(chainStore, tup, stack, curblock)
					} else {
						curblock.Append(ast.NewStore(stack.Top(1), tup))
						stack.Pop()
					}
				}
			}
		case YIELD_FROM:
			{
				stack.Pop()
				value := stack.Top(1)
				if value != nil {
					value.SetProcessed()
					curblock.Append(ast.NewReturn(value, ast.RET_YIELD_FROM))
				}
			}
		case SEND_A:
			stack.Pop()
		case YIELD_VALUE, YIELD_VALUE_A, INSTRUMENTED_YIELD_VALUE_A:
			{
				value := stack.Top(1)
				stack.Pop()
				curblock.Append(ast.NewReturn(value, ast.RET_YIELD))
			}
		case SETUP_ANNOTATIONS:
			variableAnnotations = true
		case PRECALL_A, RESUME_A, INSTRUMENTED_RESUME_A:
		case CACHE:
		case PUSH_NULL:
			if mod.VerCompare(3, 13) >= 0 && !stack.Empty() {
				callable := stack.Top(1)
				stack.Pop()
				stack.Push(nil)
				stack.Push(callable)
			} else {
				stack.Push(nil)
			}
		case GEN_START_A:
			stack.Pop()
		case SWAP_A:
			{
				var pkOp int
				savedSrc := source.Pos()
				savedPos := pos
				if !source.AtEof() {
					pkOp, _ = pyc.BcNext(source, mod, &pos)
				}
				if mod.VerCompare(3, 12) < 0 && operand == 2 && pkOp == POP_TOP && !source.AtEof() {
					rPos := pos
					midSrc := source.Pos()
					rOp, _ := pyc.BcNext(source, mod, &rPos)
					source.SetPos(midSrc)
					inFor := false
					for i := len(blocks) - 1; i >= 0; i-- {
						if blocks[i].BlkType() == ast.BLK_FOR {
							inFor = !blocks[i].IsComprehension()
							break
						}
					}
					if inFor && rOp == RETURN_VALUE {
						break
					}
				}
				source.SetPos(savedSrc)
				pos = savedPos
				if pkOp == COPY_A || pkOp == POP_TOP || operand < 2 ||
					pkOp == SWAP_A || pkOp == STORE_SUBSCR ||
					pkOp == STORE_ATTR_A || pkOp == STORE_SLICE ||
					stack.Size() < operand {
					stack.Swap(operand)
					break
				}

				unpack = operand
				values := make([]ast.Node, operand)
				for i := 0; i < operand; i++ {
					values[operand-i-1] = stack.Top(1)
					stack.Pop()
				}
				tup := ast.NewTuple(values)
				tup.SetRequireParens(false)
				nextTup := ast.NewTuple(nil)
				nextTup.SetRequireParens(false)
				stack.Push(tup)
				stack.Push(nextTup)
			}
		case BINARY_SLICE:
			{
				end := stack.Top(1)
				stack.Pop()
				start := stack.Top(1)
				stack.Pop()
				dest := stack.Top(1)
				stack.Pop()

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
				stack.Push(ast.NewSubscr(dest, slice))
			}
		case STORE_SLICE:
			{
				end := stack.Top(1)
				stack.Pop()
				start := stack.Top(1)
				stack.Pop()
				dest := stack.Top(1)
				stack.Pop()
				values := stack.Top(1)
				stack.Pop()

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

				curblock.Append(ast.NewStore(values, ast.NewSubscr(dest, slice)))
			}
		case COPY_A:
			{
				if operand == 1 && tryWalrus() {
					break
				}
				if operand == 1 && !source.AtEof() && stack.Top(1) != nil && !stack.Empty() {
					sSrc, sPos := source.Pos(), pos
					o2, a2 := pyc.BcNext(source, mod, &pos)
					source.SetPos(sSrc)
					pos = sPos
					if o2 == STORE_FAST_A || o2 == STORE_NAME_A || o2 == STORE_GLOBAL_A ||
						o2 == STORE_DEREF_A || o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A ||
						o2 == LOAD_FAST_A || o2 == LOAD_NAME_A || o2 == LOAD_GLOBAL_A ||
						o2 == LOAD_DEREF_A {
						targetFollows := o2 != LOAD_FAST_A && o2 != LOAD_NAME_A &&
							o2 != LOAD_GLOBAL_A && o2 != LOAD_DEREF_A
						if targetFollows && o2 == STORE_NAME_A &&
							code.GetName(a2).Value == "__classcell__" {
							targetFollows = false
						}
						if targetFollows && ast.TypeOf(stack.Top(1)) == ast.NODE_CHAINSTORE {
							chainstore := stack.Top(1)
							stack.Pop()
							stack.Push(stack.Top(1))
							stack.Push(chainstore)
							break
						}
						if targetFollows {
							stack.Push(stack.Top(1))
							stack.Push(ast.NewChainStore(nil, stack.Top(1)))
							break
						}
					}
				}
				value := stack.Top(operand)
				stack.Push(value)
			}
		default:
			fmt.Fprintf(os.Stderr, "Unsupported opcode: %s (%d)\n", Name(opcode), opcode)
			cleanBuild = false
			return ast.NewNodeList(defblock.Nodes())
		}

		elsePop = (curblock.BlkType() == ast.BLK_ELSE || curblock.BlkType() == ast.BLK_IF ||
			curblock.BlkType() == ast.BLK_ELIF) && curblock.End() == pos
	}

	for len(matches) != 0 {
		mc := matches[len(matches)-1]
		for blocks.size() > 1 && blocks.top() != mc.mblock {
			top := blocks.top()
			blocks.pop()
			blocks.top().Append(top)
		}
		if blocks.size() > 1 {
			mb := blocks.top()
			blocks.pop()
			blocks.top().Append(mb)
		}
		matches = matches[:len(matches)-1]
	}

	if len(stackHist) != 0 {
		fmt.Fprint(os.Stderr, "Warning: Stack history is not empty!\n")
		stackHist = nil
	}

	if blocks.size() > 1 {
		fmt.Fprint(os.Stderr, "Warning: block stack is not empty!\n")

		for blocks.size() > 1 {
			tmp := blocks.top()
			blocks.pop()

			blocks.top().Append(tmp)
		}
	}

	cleanBuild = true
	return ast.NewNodeList(defblock.Nodes())
}
