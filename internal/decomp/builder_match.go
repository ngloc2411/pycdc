package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
)

func (bld *builder) closeNested(mc *matchCtx, limit int) {
	for bld.blocks.size() > mc.cdepth {
		top := bld.blocks.top()
		if (top.BlkType() != ast.BLK_IF && top.BlkType() != ast.BLK_ELIF && top.BlkType() != ast.BLK_ELSE) ||
			top.End() > limit {
			break
		}
		if len(bld.stackHist) != 0 {
			bld.stackHist = bld.stackHist[:len(bld.stackHist)-1]
		}
		bld.blocks.pop()
		bld.blocks.top().Append(top)
		bld.curblock = bld.blocks.top()
	}
}

func (bld *builder) beginCase(mc *matchCtx, nc *MatchCase, isDefault bool) {
	mc.cur = *nc
	mc.isDefault = isDefault
	end := 0
	if nc.bodyEnd > 0 {
		end = nc.bodyEnd
	}
	cb := ast.NewCaseBlock(nc.pattern, end)
	bld.blocks.push(cb)
	bld.curblock = bld.blocks.top()
	mc.cblock = cb
	mc.cdepth = bld.blocks.size()
	bld.stack = mc.baseStack.Copy()
	mc.skipPop = nc.nonLast
	mc.guardOpen = true
	mc.guardTarget = -1
	if nc.bodyEnd >= 2 {
		mc.endJumpPos = nc.bodyEnd - 2
	} else {
		mc.endJumpPos = -1
	}
	bld.pos = nc.bodyStart
	bld.source.SetPos(bld.pos)
	bld.elsePop = false
}

func (bld *builder) finishMatch(next int) {
	mc := bld.matches[len(bld.matches)-1]
	for bld.blocks.size() > 1 && bld.blocks.top() != mc.mblock {
		top := bld.blocks.top()
		bld.blocks.pop()
		bld.blocks.top().Append(top)
	}
	mb := bld.blocks.top()
	bld.blocks.pop()
	bld.curblock = bld.blocks.top()
	bld.curblock.Append(mb)
	bld.stack = mc.baseStack.Copy()
	bld.matches = bld.matches[:len(bld.matches)-1]
	bld.pos = next
	bld.source.SetPos(bld.pos)
	bld.elsePop = (bld.curblock.BlkType() == ast.BLK_ELSE || bld.curblock.BlkType() == ast.BLK_IF ||
		bld.curblock.BlkType() == ast.BLK_ELIF) && bld.curblock.End() == bld.pos
}

func (bld *builder) endCase(mc *matchCtx) {
	bld.closeNested(mc, bld.pos)
	for bld.blocks.size() > mc.cdepth {
		top := bld.blocks.top()
		bld.blocks.pop()
		bld.blocks.top().Append(top)
	}
	cb := mc.cblock
	bld.blocks.pop()
	bld.curblock = bld.blocks.top()
	bld.curblock.Append(cb)
	mc.cblock = nil
	bld.stack = mc.baseStack.Copy()

	wasNonLast := mc.cur.nonLast
	wasDefault := mc.isDefault
	next := bld.pos
	if mc.cur.bodyEnd >= 0 && mc.cur.nextPos >= 0 {
		next = mc.cur.nextPos
	}
	if wasNonLast {
		var nc MatchCase
		if bld.matcher.ParseCase(next, &nc) {
			bld.beginCase(mc, &nc, false)
		} else {
			fmt.Fprintf(os.Stderr, "Warning: unable to decode a case of a match statement at %d\n", next)
			bld.finishMatch(next)
		}
	} else if !wasDefault {
		ni := bld.matcher.IndexOf(next)
		if ni >= 0 && bld.matcher.Instrs()[ni].op == NOP {
			dc := MatchCase{bodyEnd: -1, nextPos: -1}
			dc.pattern = ast.NewMatchPattern(ast.PAT_WILDCARD)
			dc.bodyStart = bld.matcher.Instrs()[ni].end
			bld.beginCase(mc, &dc, true)
		} else {
			bld.finishMatch(next)
		}
	} else {
		bld.finishMatch(next)
	}
}
