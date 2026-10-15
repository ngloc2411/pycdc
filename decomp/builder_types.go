package decomp

import (
	"github.com/ngloc2411/pycdc/decomp/ast"
)

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
