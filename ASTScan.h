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
    std::map<int, int> cleanupDest;    /* chained-comparison cleanup POP_TOP -> real exit */
    std::vector<std::pair<int, int> > chainSkip;   /* cleanup/trailer ranges to ignore */
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
    /* value-context chained comparison (`return a < b < c`): the keep jump's false
       target is a `SWAP 2; POP_TOP` cleanup. chainJoin maps the keep jump (or the
       COPY 1 of a 3.12 keep sequence) to the real merge point; chainSkip lists the
       trailer/cleanup ranges the decompiler must ignore. */
    std::map<int, int> chainJoin;
    std::map<int, int> cleanupDest;   /* lone POP_TOP cleanup -> the exit it leads to */
    std::vector<std::pair<int, int> > chainSkip;
    std::map<int, std::pair<int, bool> > boolJump;  /* plain POP_JUMP of a mixed and/or: target, isOr */
    /* statement conditions made of several jumps (`if a and b or c:`) */
    struct CondTree { int op; int leaf; int l, r; };   /* op 0: leaf (jump position), 1: and, 2: or */
    struct CondChain {
        std::vector<int> members;            /* jump positions, in order */
        std::vector<CondTree> trees;
        int root = 0;
        bool negate = false;                  /* the combined test is the *negation* of the tree */
    };
    std::map<int, CondChain> condChains;    /* last jump position -> chain */
    std::set<int> condMember;                /* every non-last jump of a chain */
};
void ScanExprFlow(PycRef<PycCode> code, PycModule* mod, ExprFlow& out, const WhileLoops& loops);

#endif
