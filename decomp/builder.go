package decomp

import (
	"fmt"
	"os"
	"sort"

	"github.com/ngloc2411/pycdc/decomp/ast"
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

// builder holds the state of one BuildFromCode run. The former local
// variables of that giant function are fields here; the opcode handlers and
// the per-instruction pre-processing steps are methods split over several files.
type builder struct {
	code                   *pyc.Code
	mod                    *pyc.Module
	source                 *pyc.Buffer
	stackSize              int
	stack                  *FastStack
	stackHist              []*FastStack
	blocks                 blockStack
	defblock               *ast.Block
	curblock               *ast.Block
	opcode                 int
	operand                int
	curpos                 int
	pos                    int
	unpack                 int
	unpackSaved            []unpackFrame
	unpackStar             int
	trueOpened             map[int]bool
	inlineComps            []int
	prevOp1                int
	prevOp2                int
	commonGuard            bool
	isCompCode             bool
	inlineFinish           bool
	fusedKeepFirst         map[int]bool
	inlineFinishBase       int
	inlineBase             []int
	inlineKind             []int
	inlineFinishKind       int
	pendingOps             []intPair
	assertErrNode          ast.Node
	elsePop                bool
	needTry                bool
	variableAnnotations    bool
	sawGetIter             bool
	pendingBool            []pendingBoolOp
	chainVals              map[int]ast.Node
	exceptionEntries       []pyc.ExceptionTableEntry
	cleanupSkip            []intPair
	nextExceptionEntry     int
	withBodyEndByBefore    map[int]int
	withResumeByBodyEnd    map[int]int
	withSkipUntil          int
	spliceByEnd            map[int][2]int
	spliceStack            []spliceCtx
	spliceDone             map[int]int
	finallyTryEndByStart   map[int]int
	finallyEndByStart      map[int]int
	finallyResumeByEnd     map[int]int
	finallyReturnStarts    map[int]bool
	finallySuppressReturns map[int]bool
	whileGuardEnd          map[int]int
	whileBackedges         map[int]bool
	guardChainConds        []ast.Node
	elseRegionByStart      map[int][2]int
	openElse               []openElseEnt
	loops                  *WhileLoops
	xflow                  *ExprFlow
	shadowedTryStarts      map[int][]int
	matches                []*matchCtx
	matcher                *MatchAnalyzer
	hadGetIter             bool
	chainPolarity          int
	result                 ast.Node
	handlers               []func() flow
}

type flow int

const (
	flowNext flow = iota
	flowContinue
	flowBreak
	flowReturn
	flowUnhandled
)

// BuildFromCode rebuilds the AST of a code object.
func BuildFromCode(code *pyc.Code, mod *pyc.Module) ast.Node {
	bld := newBuilder(code, mod)
	return bld.run()
}

func newBuilder(code *pyc.Code, mod *pyc.Module) *builder {
	bld := &builder{code: code, mod: mod}
	bld.source = pyc.NewBuffer([]byte(bld.code.Code.Value))
	bld.stackSize = bld.code.StackSize
	if bld.mod.MajorVer() == 1 {
		bld.stackSize = 20
	}
	bld.stack = NewFastStack(bld.stackSize)
	bld.stackHist = *new([]*FastStack)
	bld.blocks = *new(blockStack)
	bld.defblock = ast.NewBlock(ast.BLK_MAIN, 0, 0)
	bld.defblock.Init()
	bld.curblock = bld.defblock
	bld.blocks.push(bld.defblock)
	bld.opcode = *new(int)
	bld.operand = *new(int)
	bld.curpos = 0
	bld.pos = 0
	bld.unpack = 0
	bld.unpackSaved = *new([]unpackFrame)
	bld.unpackStar = -1
	bld.trueOpened = map[int]bool{}
	bld.inlineComps = *new([]int)
	bld.prevOp1, bld.prevOp2 = -1, -1
	bld.commonGuard = false
	bld.isCompCode = func() bool {
		if bld.code.Name == nil {
			return false
		}
		nm := bld.code.Name.Value
		return nm == "<listcomp>" || nm == "<setcomp>" || nm == "<dictcomp>" || nm == "<genexpr>"
	}()
	bld.inlineFinish = false
	bld.fusedKeepFirst = map[int]bool{}
	bld.inlineFinishBase = 0
	bld.inlineBase = *new([]int)
	bld.inlineKind = *new([]int)
	bld.inlineFinishKind = 0
	bld.pendingOps = *new([]intPair)
	bld.assertErrNode = *new(ast.Node)
	bld.elsePop = false
	bld.needTry = false
	bld.variableAnnotations = false
	bld.sawGetIter = false
	bld.pendingBool = *new([]pendingBoolOp)
	bld.chainVals = map[int]ast.Node{}
	bld.exceptionEntries = *new([]pyc.ExceptionTableEntry)
	bld.cleanupSkip = *new([]intPair)
	bld.nextExceptionEntry = 0
	bld.withBodyEndByBefore = map[int]int{}
	bld.withResumeByBodyEnd = map[int]int{}
	bld.withSkipUntil = 0
	bld.spliceByEnd = map[int][2]int{}
	bld.spliceStack = *new([]spliceCtx)
	bld.spliceDone = map[int]int{}
	bld.finallyTryEndByStart = map[int]int{}
	bld.finallyEndByStart = map[int]int{}
	bld.finallyResumeByEnd = map[int]int{}
	bld.finallyReturnStarts = map[int]bool{}
	bld.finallySuppressReturns = map[int]bool{}
	bld.whileGuardEnd = map[int]int{}
	bld.whileBackedges = map[int]bool{}
	bld.guardChainConds = *new([]ast.Node)
	bld.elseRegionByStart = map[int][2]int{}
	bld.openElse = *new([]openElseEnt)
	bld.loops = NewWhileLoops()
	bld.xflow = NewExprFlow()
	if bld.mod.VerCompare(3, 8) >= 0 {
		scanLoops(bld.code, bld.mod, bld.loops)
		bld.whileGuardEnd = bld.loops.guardEnd
		bld.whileBackedges = bld.loops.backedges
		scanExprFlow(bld.code, bld.mod, bld.xflow, bld.loops)
		bld.cleanupSkip = append(bld.cleanupSkip, bld.xflow.chainSkip...)
	}
	if bld.mod.VerCompare(3, 11) >= 0 {
		bld.exceptionEntries = bld.code.ExceptionTableEntries()
		if bld.mod.VerCompare(3, 12) >= 0 {
			scanComprehensionCleanup(bld.code, bld.mod, &bld.exceptionEntries, &bld.cleanupSkip)
		}
		scanWithBlocks(bld.code, bld.mod, bld.exceptionEntries, bld.withBodyEndByBefore, bld.withResumeByBodyEnd)
		scanTryFinally(bld.code, bld.mod, bld.exceptionEntries, bld.finallyTryEndByStart, bld.finallyEndByStart,
			bld.finallyResumeByEnd, bld.finallyReturnStarts, bld.finallySuppressReturns)
		scanTryElse(bld.code, bld.mod, bld.exceptionEntries, bld.elseRegionByStart)
		scanHoistedHandlers(bld.code, bld.mod, bld.exceptionEntries, bld.spliceByEnd)
	}
	_ = bld.whileGuardEnd
	_ = bld.whileBackedges
	_ = bld.inlineFinish
	_ = bld.fusedKeepFirst
	_ = bld.inlineFinishBase
	_ = bld.inlineFinishKind
	_ = bld.assertErrNode
	_ = bld.variableAnnotations
	_ = bld.commonGuard
	_ = bld.guardChainConds
	_ = bld.inlineBase
	_ = bld.inlineKind
	bld.shadowedTryStarts = map[int][]int{}
	if len(bld.exceptionEntries) != 0 {
		span := map[int][2]int{}
		finallyTargets := map[int]bool{}
		for _, e := range bld.exceptionEntries {
			if e.PushLasti {
				continue
			}
			if _, ok := bld.finallyTryEndByStart[e.StartOffset]; ok {
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
			bld.shadowedTryStarts[f.a] = append(bld.shadowedTryStarts[f.a], f.b)
		}
	}
	bld.matches = *new([]*matchCtx)
	bld.matcher = *new(*MatchAnalyzer)
	bld.handlers = []func() flow{bld.opsExpr, bld.opsCall, bld.opsCondJump, bld.opsJump, bld.opsBuild, bld.opsStack, bld.opsStore, bld.opsMisc}
	return bld
}

func (bld *builder) run() ast.Node {
	for !bld.source.AtEof() || len(bld.spliceStack) != 0 {
		if f := bld.step(); f == flowReturn {
			return bld.result
		} else if f == flowBreak {
			break
		}
	}
	for len(bld.matches) != 0 {
		mc := bld.matches[len(bld.matches)-1]
		for bld.blocks.size() > 1 && bld.blocks.top() != mc.mblock {
			top := bld.blocks.top()
			bld.blocks.pop()
			bld.blocks.top().Append(top)
		}
		if bld.blocks.size() > 1 {
			mb := bld.blocks.top()
			bld.blocks.pop()
			bld.blocks.top().Append(mb)
		}
		bld.matches = bld.matches[:len(bld.matches)-1]
	}
	if len(bld.stackHist) != 0 {
		fmt.Fprint(os.Stderr, "Warning: Stack history is not empty!\n")
		bld.stackHist = nil
	}
	if bld.blocks.size() > 1 {
		fmt.Fprint(os.Stderr, "Warning: block stack is not empty!\n")

		for bld.blocks.size() > 1 {
			tmp := bld.blocks.top()
			bld.blocks.pop()

			bld.blocks.top().Append(tmp)
		}
	}
	cleanBuild = true
	return ast.NewNodeList(bld.defblock.Nodes())
}

// step decodes and handles one instruction.
func (bld *builder) step() flow {
	if f := bld.stepBlocks(); f != flowNext {
		return f
	}
	if f := bld.stepFetch(); f != flowNext {
		return f
	}
	if f := bld.stepPeepholeA(); f != flowNext {
		return f
	}
	if f := bld.stepPeepholeB(); f != flowNext {
		return f
	}
	handled := false
	for _, h := range bld.handlers {
		f := h()
		if f == flowUnhandled {
			continue
		}
		if f != flowNext {
			return f
		}
		handled = true
		break
	}
	if !handled {
		fmt.Fprintf(os.Stderr, "Unsupported opcode: %s (%d)\n", Name(bld.opcode), bld.opcode)
		cleanBuild = false
		bld.result = ast.NewNodeList(bld.defblock.Nodes())
		return flowReturn
	}
	bld.elsePop = (bld.curblock.BlkType() == ast.BLK_ELSE || bld.curblock.BlkType() == ast.BLK_IF ||
		bld.curblock.BlkType() == ast.BLK_ELIF) && bld.curblock.End() == bld.pos
	return flowNext
}
