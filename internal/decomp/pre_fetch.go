package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) stepFetch() flow {
	{
		if er, ok := bld.elseRegionByStart[bld.pos]; ok && bld.curblock.BlkType() == ast.BLK_TRY &&
			bld.curblock.End() == er[1] && bld.blocks.size() > 1 {
			tryb := bld.blocks.top()
			bld.blocks.pop()
			isContainer := bld.blocks.top().BlkType() == ast.BLK_CONTAINER && bld.blocks.top().HasExcept()
			bld.blocks.push(tryb)
			if isContainer {
				eb := ast.NewBlock(ast.BLK_ELSE, 0, 1)
				bld.blocks.push(eb)
				bld.curblock = eb
				bld.openElse = append(bld.openElse, openElseEnt{er[0], eb})
			}
		}
		if len(bld.openElse) != 0 && bld.pos == bld.openElse[len(bld.openElse)-1].pos &&
			bld.curblock == bld.openElse[len(bld.openElse)-1].block {
			eb := bld.curblock
			bld.blocks.pop()
			tryb := bld.blocks.top()
			bld.blocks.pop()
			cont := bld.blocks.top()
			if eb.Size() != 0 {
				cont.SetElseBlock(eb)
			}
			bld.blocks.push(tryb)
			bld.curblock = tryb
			bld.openElse = bld.openElse[:len(bld.openElse)-1]
			bld.curpos = bld.pos
			bld.opcode, bld.operand = pyc.BcNext(bld.source, bld.mod, &bld.pos)
			return flowContinue
		}
	}
	if len(bld.spliceStack) != 0 && bld.pos == bld.spliceStack[len(bld.spliceStack)-1].regionEnd {
		ctx := bld.spliceStack[len(bld.spliceStack)-1]
		bld.spliceStack = bld.spliceStack[:len(bld.spliceStack)-1]
		bld.spliceDone[ctx.handler] = ctx.regionEnd
		bld.pos = ctx.resume
		bld.source.SetPos(bld.pos)
		bld.nextExceptionEntry = 0
		return flowContinue
	}
	bld.curpos = bld.pos
	return flowNext
}
