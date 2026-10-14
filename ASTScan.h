#ifndef _PYC_ASTSCAN_H
#define _PYC_ASTSCAN_H

#include <map>
#include <set>
#include <vector>
#include <utility>
#include "pyc_code.h"
#include "pyc_module.h"

/* Loop structure recovered from the control-flow graph, for `while` loops of
   Python 3.8 - 3.14 (a `while` has no dedicated opcode):
     top-tested (<=3.9, and `while True`):  T: cond; guard -> end; body; JUMP T
     bottom-tested (>=3.10):                guard -> end; T: body; cond; JUMP_IF T
                                            (3.12+: cond; JUMP_IF_FALSE end; JUMP_BACKWARD T)
   Besides the loop frame we record which jumps are `break` / `continue`. */
struct ChainJump { int pos; bool ifTrue; bool toBody; };

struct WhileLoops {
    std::map<int, int> guardEnd;       /* guard position -> loop end */
    std::map<int, int> trueOpen;       /* header position -> loop end (`while True`) */
    std::set<int> backedges;           /* conditional back-edges (pop the condition) */
    std::set<int> uncondBack;          /* unconditional closing back-edges */
    std::set<int> skipJumps;           /* instructions that belong to a back-edge */
    std::set<int> breaks, continues;   /* jump positions */
    /* `if c: break` at the tail of a loop whose break jump was elided (3.14):
       conditional jump position -> true when the jump is taken on a true test */
    std::map<int, bool> breakIf;
    /* `for` loops: POP_TOP of the iterator + the exit jump that form a break */
    std::set<int> forBreakPops, forBreaks;
    std::map<int, int> forElse;        /* start of a for/else `else` body -> its end */
    std::map<int, int> elseEnd;        /* loop end -> end of its `else` body */
    std::set<int> loopEnds;
    /* compound loop conditions (`while a and b`) of bottom-tested loops */
    std::map<int, std::vector<ChainJump> > guardChain;   /* last guard jump -> earlier jumps */
    std::set<int> guardChainMember;                        /* those earlier guard jumps */
    std::set<int> tailChain;                               /* jumps of the duplicated condition */
};


void ScanWithBlocks(PycRef<PycCode> code, PycModule* mod,
                    const std::vector<PycExceptionTableEntry>& entries,
                    std::map<int, int>& bodyEndByBefore,
                    std::map<int, int>& resumeByBodyEnd);
void ScanTryFinally(PycRef<PycCode> code, PycModule* mod,
                    const std::vector<PycExceptionTableEntry>& entries,
                    std::map<int, int>& tryEndByStart,
                    std::map<int, int>& finallyEndByStart,
                    std::map<int, int>& resumeByFinallyEnd,
                    std::set<int>& returnStyleStarts,
                    std::set<int>& suppressReturns);
void ScanLoops(PycRef<PycCode> code, PycModule* mod, WhileLoops& out);
void ScanChainedCompare(PycRef<PycCode> code, PycModule* mod,
                        std::map<int,int>& chainSkipJumps,
                        std::map<int,int>& trampolineExit);
void ScanComprehensionCleanup(PycRef<PycCode> code, PycModule* mod,
                              std::vector<PycExceptionTableEntry>& entries,
                              std::vector<std::pair<int, int> >& skip);
void ScanTryElse(PycRef<PycCode> code, PycModule* mod,
                 const std::vector<PycExceptionTableEntry>& entries,
                 std::map<int, std::pair<int, int> >& elseRegionByStart);

/* Expression-level control flow, see ScanExprFlow. */
struct ExprFlow {
    struct Tern { int elseStart, end, thenJump; int cond; };   /* cond: 1 jump-if-false, 2 jump-if-true, 3 if-none, 4 if-not-none */
    struct Keep { bool isOr; int target; int endPos; int popPos; };
    std::map<int, Tern> ternary;            /* test jump position -> shape */
    std::map<int, int> ternaryThenJump;     /* JUMP_FORWARD ending the "then" value -> test jump */
    std::map<int, Keep> keepSeq;            /* 3.12+: COPY 1 starting `and`/`or` -> shape */
    std::map<int, std::pair<int, bool> > boolJump;   /* plain POP_JUMP of a mixed and/or: target, isOr */
};
void ScanExprFlow(PycRef<PycCode> code, PycModule* mod, ExprFlow& out);

#endif
