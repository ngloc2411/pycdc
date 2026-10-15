package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) stepPeepholeA() flow {
	if len(bld.pendingOps) != 0 {
		bld.opcode = bld.pendingOps[0].a
		bld.operand = bld.pendingOps[0].b
		bld.pendingOps = bld.pendingOps[1:]
	} else {
		bld.opcode, bld.operand = pyc.BcNext(bld.source, bld.mod, &bld.pos)
	}
	if len(bld.cleanupSkip) != 0 {
		skipIt := false
		for _, r := range bld.cleanupSkip {
			if bld.curpos >= r.a && bld.curpos < r.b {
				skipIt = true
			}
		}
		if skipIt {
			for _, r := range bld.xflow.chainSkip {
				if bld.curpos >= r.a && bld.curpos < r.b &&
					(bld.curblock.BlkType() == ast.BLK_ELSE || bld.curblock.BlkType() == ast.BLK_IF || bld.curblock.BlkType() == ast.BLK_ELIF) {
					bld.elsePop = bld.curblock.End() == bld.pos
				}
			}
			return flowContinue
		}
	}
	bld.hadGetIter = bld.sawGetIter
	_ = bld.hadGetIter
	if bld.opcode != GET_ITER && bld.opcode != GET_YIELD_FROM_ITER && bld.opcode != PRECALL_A && bld.opcode != CACHE &&
		bld.opcode != RESUME_A && bld.opcode != INSTRUMENTED_RESUME_A {
		bld.sawGetIter = false
	}
	if bld.needTry && bld.opcode != SETUP_EXCEPT_A {
		bld.needTry = false
		bld.stackHist = append(bld.stackHist, bld.stack.Copy())
		tryblock := ast.NewBlock(ast.BLK_TRY, bld.curblock.End(), 1)
		bld.blocks.push(tryblock)
		bld.curblock = bld.blocks.top()
	} else if bld.elsePop && bld.opcode != JUMP_FORWARD_A && bld.opcode != JUMP_IF_FALSE_A &&
		bld.opcode != JUMP_IF_FALSE_OR_POP_A && bld.opcode != POP_JUMP_IF_FALSE_A &&
		bld.opcode != POP_JUMP_FORWARD_IF_FALSE_A && bld.opcode != JUMP_IF_TRUE_A &&
		bld.opcode != JUMP_IF_TRUE_OR_POP_A && bld.opcode != POP_JUMP_IF_TRUE_A &&
		bld.opcode != POP_JUMP_FORWARD_IF_TRUE_A && bld.opcode != POP_BLOCK {
		bld.elsePop = false

		prev := bld.curblock
		for prev.End() < bld.pos && prev.BlkType() != ast.BLK_MAIN {
			if prev.BlkType() != ast.BLK_CONTAINER {
				if prev.End() == 0 {
					break
				}
				if len(bld.stackHist) != 0 {
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				}
			}
			bld.blocks.pop()

			if bld.blocks.empty() {
				break
			}

			bld.curblock = bld.blocks.top()
			bld.curblock.Append(prev)

			prev = bld.curblock

			checkIfExpr(bld.stack, bld.curblock)
		}
	}
	for len(bld.pendingBool) != 0 && bld.pendingBool[len(bld.pendingBool)-1].target <= bld.curpos && !bld.stack.Empty() {
		pb := bld.pendingBool[len(bld.pendingBool)-1]
		bld.pendingBool = bld.pendingBool[:len(bld.pendingBool)-1]
		right := bld.stack.Top(1)
		bld.stack.Pop()
		if pb.kind == 1 {
			tb := ast.NewCondBlock(ast.BLK_IF, 0, pb.left, pb.neg)
			bld.stack.Push(ast.NewTernary(tb, pb.thenVal, right))
		} else {
			bld.stack.Push(mkBoolOp(pb.left, right, pb.op))
		}
	}
	bld.chainPolarity = -1
	if bld.mod.VerCompare(3, 8) >= 0 && len(bld.inlineComps) == 0 && !bld.isCompCode {
		if bld.xflow.condMember[bld.curpos] && !bld.stack.Empty() {
			bld.chainVals[bld.curpos] = bld.stack.Top(1)
			bld.stack.Pop()
			bld.elsePop = false
			return flowContinue
		}
		if cc, ok := bld.xflow.condChains[bld.curpos]; ok && !bld.stack.Empty() {
			have := true
			for k := 0; k+1 < len(cc.members); k++ {
				if _, ok := bld.chainVals[cc.members[k]]; !ok {
					have = false
				}
			}
			if have {
				bld.chainVals[bld.curpos] = bld.stack.Top(1)
				bld.stack.Pop()
				var build func(ti int) ast.Node
				build = func(ti int) ast.Node {
					tr := cc.trees[ti]
					if tr.op == 0 {
						return bld.chainVals[tr.leaf]
					}
					bop := ast.BIN_LOG_OR
					if tr.op == 1 {
						bop = ast.BIN_LOG_AND
					}
					return mkBoolOp(build(tr.l), build(tr.r), bop)
				}
				whole := build(cc.root)
				for _, mpos := range cc.members {
					delete(bld.chainVals, mpos)
				}
				bld.stack.Push(whole)
				if cc.negate {
					bld.chainPolarity = 1
				} else {
					bld.chainPolarity = 0
				}
				wantOnTrue := cc.negate
				isTrueOp := bld.opcode == POP_JUMP_IF_TRUE_A || bld.opcode == POP_JUMP_FORWARD_IF_TRUE_A
				if isTrueOp != wantOnTrue {
					switch bld.opcode {
					case POP_JUMP_IF_TRUE_A:
						bld.opcode = POP_JUMP_IF_FALSE_A
					case POP_JUMP_IF_FALSE_A:
						bld.opcode = POP_JUMP_IF_TRUE_A
					case POP_JUMP_FORWARD_IF_TRUE_A:
						bld.opcode = POP_JUMP_FORWARD_IF_FALSE_A
					case POP_JUMP_FORWARD_IF_FALSE_A:
						bld.opcode = POP_JUMP_FORWARD_IF_TRUE_A
					}
				}
			}
		}
	}
	if bld.mod.VerCompare(3, 8) >= 0 {
		handled := func() bool {
			skipOp := func() { bld.elsePop = false }
			mergeAt := func(at int) {
				for len(bld.pendingBool) != 0 && bld.pendingBool[len(bld.pendingBool)-1].kind == 0 &&
					bld.pendingBool[len(bld.pendingBool)-1].target == at && !bld.stack.Empty() {
					pb := bld.pendingBool[len(bld.pendingBool)-1]
					bld.pendingBool = bld.pendingBool[:len(bld.pendingBool)-1]
					right := bld.stack.Top(1)
					bld.stack.Pop()
					bld.stack.Push(mkBoolOp(pb.left, right, pb.op))
				}
			}
			if _, ok := bld.xflow.ternaryThenJump[bld.curpos]; ok && !bld.stack.Empty() {
				for k := len(bld.pendingBool) - 1; k >= 0; k-- {
					if bld.pendingBool[k].kind == 1 && bld.pendingBool[k].thenJump == bld.curpos && bld.pendingBool[k].thenVal == nil {
						bld.pendingBool[k].thenVal = bld.stack.Top(1)
						bld.stack.Pop()
						skipOp()
						return true
					}
				}
			}
			if tn, ok := bld.xflow.ternary[bld.curpos]; ok && !bld.stack.Empty() {
				c := bld.stack.Top(1)
				bld.stack.Pop()
				neg := false
				if bld.chainPolarity >= 0 {
					neg = bld.chainPolarity == 1
				} else if tn.cond == 2 {
					neg = true
				} else if tn.cond == 3 {
					c = ast.NewCompare(c, ast.NewObject(pyc.None), ast.CMP_IS_NOT)
				} else if tn.cond == 4 {
					c = ast.NewCompare(c, ast.NewObject(pyc.None), ast.CMP_IS)
				}
				if len(bld.pendingBool) != 0 && bld.pendingBool[len(bld.pendingBool)-1].kind == 1 &&
					bld.pendingBool[len(bld.pendingBool)-1].elseStart == tn.elseStart &&
					bld.pendingBool[len(bld.pendingBool)-1].thenVal == nil {
					fr := &bld.pendingBool[len(bld.pendingBool)-1]
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
					bld.pendingBool = append(bld.pendingBool, fr)
				}
				skipOp()
				return true
			}
			if ks, ok := bld.xflow.keepSeq[bld.curpos]; ok && !bld.stack.Empty() {
				mergeAt(ks.popPos)
				mergeAt(ks.endPos)
				var pb pendingBoolOp
				pb.target = ks.target
				for k := len(bld.pendingBool) - 1; k >= 0; k-- {
					if bld.pendingBool[k].kind == 1 && bld.pendingBool[k].thenVal == nil &&
						bld.pendingBool[k].target == pb.target && bld.pendingBool[k].thenJump > bld.curpos {
						pb.target = bld.pendingBool[k].thenJump
						break
					}
				}
				pb.left = bld.stack.Top(1)
				if ks.isOr {
					pb.op = ast.BIN_LOG_OR
				} else {
					pb.op = ast.BIN_LOG_AND
				}
				bld.stack.Pop()
				bld.pendingBool = append(bld.pendingBool, pb)
				bld.source.SetPos(ks.endPos)
				bld.pos = ks.endPos
				skipOp()
				return true
			}
			if (bld.opcode == JUMP_IF_TRUE_OR_POP_A || bld.opcode == JUMP_IF_FALSE_OR_POP_A) && !bld.stack.Empty() {
				var pb pendingBoolOp
				mul := 1
				if bld.mod.VerCompare(3, 10) >= 0 {
					mul = 2
				}
				tgt := bld.operand * mul
				if bld.mod.VerCompare(3, 11) >= 0 {
					tgt += bld.pos
				}
				if cj, ok := bld.xflow.chainJoin[bld.curpos]; ok {
					tgt = cj
				}
				mergeAt(bld.pos)
				for k := len(bld.pendingBool) - 1; k >= 0; k-- {
					if bld.pendingBool[k].kind == 1 && bld.pendingBool[k].thenVal == nil &&
						bld.pendingBool[k].target == tgt && bld.pendingBool[k].thenJump > bld.curpos {
						tgt = bld.pendingBool[k].thenJump
						break
					}
				}
				pb.target = tgt
				pb.left = bld.stack.Top(1)
				if bld.opcode == JUMP_IF_TRUE_OR_POP_A {
					pb.op = ast.BIN_LOG_OR
				} else {
					pb.op = ast.BIN_LOG_AND
				}
				bld.stack.Pop()
				bld.pendingBool = append(bld.pendingBool, pb)
				skipOp()
				return true
			}
			if bj, ok := bld.xflow.boolJump[bld.curpos]; ok && !bld.stack.Empty() {
				var pb pendingBoolOp
				pb.target = bj.target
				pb.left = bld.stack.Top(1)
				if bj.isOr {
					pb.op = ast.BIN_LOG_OR
				} else {
					pb.op = ast.BIN_LOG_AND
				}
				bld.stack.Pop()
				bld.pendingBool = append(bld.pendingBool, pb)
				skipOp()
				return true
			}
			return false
		}()
		if handled {
			return flowContinue
		}
	}
	if bld.mod.VerCompare(3, 8) >= 0 && bld.mod.VerCompare(3, 11) < 0 &&
		(bld.opcode == POP_JUMP_IF_FALSE_A || bld.opcode == POP_JUMP_IF_TRUE_A) {
		mul := 1
		if bld.mod.VerCompare(3, 10) >= 0 {
			mul = 2
		}
		tgt := bld.operand * mul
		tailOfLoop := false
		if tgt < bld.curpos {
			for i := len(bld.blocks) - 1; i >= 0; i-- {
				bt := bld.blocks[i].BlkType()
				if bt == ast.BLK_FOR {
					tailOfLoop = !bld.blocks[i].IsComprehension() && tgt == bld.blocks[i].Start()
					break
				}
				if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
					break
				}
			}
		}
		if tailOfLoop {
			if bld.opcode == POP_JUMP_IF_TRUE_A {
				bld.opcode = POP_JUMP_BACKWARD_IF_TRUE_A
			} else {
				bld.opcode = POP_JUMP_BACKWARD_IF_FALSE_A
			}
		}
	}
	return flowNext
}

func (bld *builder) stepPeepholeB() flow {
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
				return ast.NewName(bld.code.GetLocal(arg))
			}
			if op == STORE_DEREF_A {
				return ast.NewName(bld.code.GetCellVar(bld.mod, arg))
			}
			return ast.NewName(bld.code.GetName(arg))
		}
		if bld.unpack == 0 && bld.mod.VerCompare(3, 8) >= 0 && isSimpleStore(bld.opcode) &&
			len(bld.pendingOps) == 0 && !bld.source.AtEof() &&
			(isSimpleLoad(bld.prevOp1) || bld.prevOp1 == LOAD_FAST_LOAD_FAST_A ||
				bld.prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) && bld.stack.Size() >= 3 {
			sSrc, sPos := bld.source.Pos(), bld.pos
			stores := []storeOp{{bld.opcode, bld.operand}}
			var o2, a2 int
			for !bld.source.AtEof() {
				o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				if !isSimpleStore(o2) {
					break
				}
				stores = append(stores, storeOp{o2, a2})
			}
			ok := len(stores) >= 3 && len(stores) <= bld.stack.Size()
			for k := 0; ok && k < len(stores); k++ {
				ok = bld.stack.Top(k+1) != nil && ast.TypeOf(bld.stack.Top(k+1)) != ast.NODE_CHAINSTORE
			}
			if ok {
				bld.source.SetPos(sSrc)
				bld.pos = sPos
				for k := 1; k < len(stores); k++ {
					o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				}
				vals := make([]ast.Node, len(stores))
				var names []ast.Node
				for k := len(stores); k > 0; k-- {
					names = append(names, storeName(stores[k-1].op, stores[k-1].arg))
				}
				for k := 0; k < len(stores); k++ {
					vals[len(stores)-1-k] = bld.stack.Top(k + 1)
				}
				for k := 0; k < len(stores); k++ {
					bld.stack.Pop()
				}
				vt := ast.NewTuple(vals)
				nt := ast.NewTuple(names)
				vt.SetRequireParens(false)
				nt.SetRequireParens(false)
				bld.curblock.Append(ast.NewStore(vt, nt))
				bld.prevOp1, bld.prevOp2 = -1, -1
				return flowContinue
			}
			bld.source.SetPos(sSrc)
			bld.pos = sPos
		}
		if bld.unpack == 0 && bld.mod.VerCompare(3, 8) >= 0 && isSimpleStore(bld.opcode) &&
			((isSimpleLoad(bld.prevOp1) && isSimpleLoad(bld.prevOp2)) || bld.prevOp1 == LOAD_FAST_LOAD_FAST_A ||
				bld.prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) &&
			bld.stack.Size() >= 2 && (!bld.source.AtEof() || len(bld.pendingOps) != 0) &&
			bld.stack.Top(1) != nil && bld.stack.Top(2) != nil &&
			ast.TypeOf(bld.stack.Top(1)) != ast.NODE_CHAINSTORE && ast.TypeOf(bld.stack.Top(2)) != ast.NODE_CHAINSTORE {
			sSrc, sPos := bld.source.Pos(), bld.pos
			var o2, a2 int
			fromPending := len(bld.pendingOps) != 0
			if fromPending {
				o2 = bld.pendingOps[0].a
				a2 = bld.pendingOps[0].b
			} else {
				o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				bld.source.SetPos(sSrc)
				bld.pos = sPos
			}
			if isSimpleStore(o2) {
				t1 := storeName(bld.opcode, bld.operand)
				if fromPending {
					bld.pendingOps = bld.pendingOps[1:]
				} else {
					o2, a2 = pyc.BcNext(bld.source, bld.mod, &bld.pos)
				}
				t2 := storeName(o2, a2)
				y := bld.stack.Top(1)
				bld.stack.Pop()
				x := bld.stack.Top(1)
				bld.stack.Pop()
				vt := ast.NewTuple([]ast.Node{x, y})
				nt := ast.NewTuple([]ast.Node{t2, t1})
				vt.SetRequireParens(false)
				nt.SetRequireParens(false)
				bld.curblock.Append(ast.NewStore(vt, nt))
				bld.prevOp1, bld.prevOp2 = -1, -1
				return flowContinue
			}
			bld.source.SetPos(sSrc)
			bld.pos = sPos
		}
		if bld.unpack == 0 && bld.opcode == STORE_FAST_STORE_FAST_A &&
			((isSimpleLoad(bld.prevOp1) && isSimpleLoad(bld.prevOp2)) || bld.prevOp1 == LOAD_FAST_LOAD_FAST_A ||
				bld.prevOp1 == LOAD_FAST_BORROW_LOAD_FAST_BORROW_A) &&
			bld.stack.Size() >= 2 && bld.stack.Top(1) != nil && bld.stack.Top(2) != nil &&
			ast.TypeOf(bld.stack.Top(1)) != ast.NODE_CHAINSTORE && ast.TypeOf(bld.stack.Top(2)) != ast.NODE_CHAINSTORE {
			t1 := ast.NewName(bld.code.GetLocal(bld.operand >> 4))
			t2 := ast.NewName(bld.code.GetLocal(bld.operand & 0xF))
			y := bld.stack.Top(1)
			bld.stack.Pop()
			x := bld.stack.Top(1)
			bld.stack.Pop()
			vt := ast.NewTuple([]ast.Node{x, y})
			nt := ast.NewTuple([]ast.Node{t2, t1})
			vt.SetRequireParens(false)
			nt.SetRequireParens(false)
			bld.curblock.Append(ast.NewStore(vt, nt))
			bld.prevOp1, bld.prevOp2 = -1, -1
			return flowContinue
		}
		bld.prevOp2 = bld.prevOp1
		bld.prevOp1 = bld.opcode
	}
	if bld.mod.VerCompare(3, 12) >= 0 {
		if bld.opcode == POP_JUMP_IF_NONE_A {
			bld.opcode = POP_JUMP_FORWARD_IF_NONE_A
		} else if bld.opcode == POP_JUMP_IF_NOT_NONE_A {
			bld.opcode = POP_JUMP_FORWARD_IF_NOT_NONE_A
		}
	}
	if bld.commonGuard && bld.opcode == POP_JUMP_IF_FALSE_A {
		bld.commonGuard = false
		jumpBase := bld.pos
		for !bld.source.AtEof() {
			keepSrc, keepPos := bld.source.Pos(), bld.pos
			o3, _ := pyc.BcNext(bld.source, bld.mod, &bld.pos)
			if o3 == CACHE {
				jumpBase = bld.pos
			} else {
				bld.source.SetPos(keepSrc)
				bld.pos = keepPos
				break
			}
		}
		target := jumpBase + bld.operand*2
		if !bld.stack.Empty() {
			bld.stack.Pop()
		}
		bld.source.SetPos(target)
		bld.pos = target
		return flowContinue
	}
	if (len(bld.inlineComps) != 0 || bld.isCompCode) && bld.mod.VerCompare(3, 12) >= 0 &&
		(bld.opcode == POP_JUMP_IF_TRUE_A || bld.opcode == POP_JUMP_IF_FALSE_A) && !bld.source.AtEof() {
		sSrc, sPos := bld.source.Pos(), bld.pos
		jumpBase := bld.pos
		o2, _ := pyc.BcNext(bld.source, bld.mod, &bld.pos)
		for o2 == CACHE && !bld.source.AtEof() {
			jumpBase = bld.pos
			o2, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
		}
		if o2 == NOT_TAKEN && !bld.source.AtEof() {
			o2, _ = pyc.BcNext(bld.source, bld.mod, &bld.pos)
		}
		afterJump := bld.pos
		if o2 == JUMP_BACKWARD_A || o2 == JUMP_BACKWARD_NO_INTERRUPT_A {
			for {
				if bld.source.AtEof() {
					break
				}
				keepSrc, keepPos := bld.source.Pos(), bld.pos
				o3, _ := pyc.BcNext(bld.source, bld.mod, &bld.pos)
				if o3 == CACHE {
					afterJump = bld.pos
				} else {
					bld.source.SetPos(keepSrc)
					bld.pos = keepPos
					break
				}
			}
		}
		target := jumpBase + bld.operand*2
		if (o2 == JUMP_BACKWARD_A || o2 == JUMP_BACKWARD_NO_INTERRUPT_A) && target == afterJump {
			if bld.opcode == POP_JUMP_IF_TRUE_A {
				bld.opcode = POP_JUMP_BACKWARD_IF_FALSE_A
			} else {
				bld.opcode = POP_JUMP_BACKWARD_IF_TRUE_A
			}
		} else {
			bld.source.SetPos(sSrc)
			bld.pos = sPos
		}
	}
	if bld.mod.VerCompare(3, 10) >= 0 && !bld.stack.Empty() &&
		((bld.opcode == COPY_A && bld.operand == 1) || bld.opcode == DUP_TOP || bld.opcode == MATCH_SEQUENCE ||
			bld.opcode == MATCH_MAPPING || bld.opcode == LOAD_GLOBAL_A || bld.opcode == LOAD_NAME_A ||
			bld.opcode == LOAD_FAST_A || bld.opcode == LOAD_DEREF_A) {
		if bld.matcher == nil {
			bld.matcher = NewMatchAnalyzer(bld.code, bld.mod)
		}
		var first MatchCase
		if bld.matcher.IsMatchStart(bld.curpos) && bld.matcher.ParseCase(bld.curpos, &first) && first.bodyEnd >= 0 {
			var second MatchCase
			if !first.nonLast || bld.matcher.ParseCase(first.nextPos, &second) {
				subject := bld.stack.Top(1)
				bld.stack.Pop()
				mb := ast.NewMatchBlock(subject)
				bld.blocks.push(mb)
				bld.curblock = bld.blocks.top()
				mc := &matchCtx{mblock: mb, baseStack: bld.stack.Copy(), guardTarget: -1, endJumpPos: -1, endPos: -1}
				bld.matches = append(bld.matches, mc)
				bld.beginCase(mc, &first, false)
				return flowContinue
			}
		}
	}
	if bld.mod.VerCompare(3, 8) >= 0 && len(bld.inlineComps) == 0 {
		closeWhile := func(popCond bool) bool {
			hasWhile := false
			for i := len(bld.blocks) - 1; i >= 0; i-- {
				if bld.blocks[i].BlkType() == ast.BLK_WHILE {
					hasWhile = true
					break
				}
			}
			if !hasWhile {
				return false
			}
			if popCond && !bld.stack.Empty() {
				bld.stack.Pop()
			}
			for bld.blocks.size() > 1 && bld.curblock.BlkType() != ast.BLK_WHILE {
				inner := bld.curblock
				if len(bld.stackHist) != 0 && (inner.BlkType() == ast.BLK_IF || inner.BlkType() == ast.BLK_ELIF ||
					inner.BlkType() == ast.BLK_ELSE) {
					bld.stack = bld.stackHist[len(bld.stackHist)-1]
					bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
				}
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(inner)
			}
			if bld.curblock.BlkType() == ast.BLK_WHILE && bld.blocks.size() > 1 {
				wh := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(wh)
				if ee, ok := bld.loops.elseEnd[wh.End()]; ok {
					eb := ast.NewBlock(ast.BLK_ELSE, ee, 0)
					eb.InitV(ast.PRE_POPPED)
					bld.stackHist = append(bld.stackHist, bld.stack.Copy())
					bld.blocks.push(eb)
					bld.curblock = bld.blocks.top()
				}
			}
			return true
		}
		refreshElsePop := func() {
			bld.elsePop = (bld.curblock.BlkType() == ast.BLK_ELSE || bld.curblock.BlkType() == ast.BLK_IF ||
				bld.curblock.BlkType() == ast.BLK_ELIF) && bld.curblock.End() == bld.pos
		}
		if bld.loops.skipJumps[bld.curpos] {
			refreshElsePop()
			return flowContinue
		}
		if (bld.loops.guardChainMember[bld.curpos] || bld.loops.tailChain[bld.curpos]) && !bld.stack.Empty() {
			if bld.loops.guardChainMember[bld.curpos] {
				bld.guardChainConds = append(bld.guardChainConds, bld.stack.Top(1))
			}
			bld.stack.Pop()
			refreshElsePop()
			return flowContinue
		}
		if _, ok := bld.loops.trueOpen[bld.curpos]; ok && !bld.trueOpened[bld.curpos] {
			bld.trueOpened[bld.curpos] = true
			wh := ast.NewCondBlock(ast.BLK_WHILE, bld.loops.trueOpen[bld.curpos], ast.NewObject(pyc.True), false)
			wh.InitV(ast.PRE_POPPED)
			bld.blocks.push(wh)
			bld.curblock = bld.blocks.top()
		}
		if bld.loops.backedges[bld.curpos] && closeWhile(true) {
			refreshElsePop()
			return flowContinue
		}
		if bld.loops.forTryEnd[bld.curpos] {
			for bld.blocks.size() > 1 && bld.curblock.BlkType() != ast.BLK_FOR && bld.curblock.End() <= bld.curpos &&
				bld.curblock.BlkType() != ast.BLK_MAIN {
				inner := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(inner)
			}
			if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.End() == bld.curpos && bld.blocks.size() > 1 &&
				!bld.curblock.IsComprehension() {
				fb := bld.curblock
				bld.blocks.pop()
				bld.curblock = bld.blocks.top()
				bld.curblock.Append(fb)
			}
		}
		if bld.loops.uncondBack[bld.curpos] && closeWhile(false) {
			refreshElsePop()
			return flowContinue
		}
		if fe, ok := bld.loops.forElse[bld.curpos]; ok {
			eb := ast.NewBlock(ast.BLK_ELSE, fe, 0)
			eb.InitV(ast.PRE_POPPED)
			bld.stackHist = append(bld.stackHist, bld.stack.Copy())
			bld.blocks.push(eb)
			bld.curblock = bld.blocks.top()
		}
		if bld.loops.forBreakPops[bld.curpos] {
			refreshElsePop()
			return flowContinue
		}
		if bld.loops.forBreaks[bld.curpos] {
			bld.curblock.Append(ast.NewKeyword(ast.KW_BREAK))
			refreshElsePop()
			return flowContinue
		}
		if bv, ok := bld.loops.breakIf[bld.curpos]; ok && !bld.stack.Empty() {
			bcond := bld.stack.Top(1)
			bld.stack.Pop()
			bb := ast.NewCondBlock(ast.BLK_IF, bld.pos, bcond, !bv)
			bb.InitV(ast.PRE_POPPED)
			bb.Append(ast.NewKeyword(ast.KW_BREAK))
			bld.curblock.Append(bb)
			refreshElsePop()
			return flowContinue
		}
		if bld.loops.breaks[bld.curpos] {
			bld.curblock.Append(ast.NewKeyword(ast.KW_BREAK))
			refreshElsePop()
			return flowContinue
		}
		if bld.loops.continues[bld.curpos] {
			bld.curblock.Append(ast.NewKeyword(ast.KW_CONTINUE))
			refreshElsePop()
			return flowContinue
		}
	}
	return flowNext
}
