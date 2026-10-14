#include <map>
#include <set>
#include <vector>
#include <utility>
#include "ASTScan.h"
#include "bytecode.h"

/* Bytecode pre-passes ("scanners"). Each walks a code object once and records
   positions/shapes that the AST builder (ASTree.cpp) consults while it
   interprets the instruction stream. They never build AST nodes themselves. */

/* Python 3.11 with-statement pre-pass. For each BEFORE_WITH whose normal exit
   has the canonical shape (body -> implicit __exit__ -> JUMP over the cleanup
   handler -> resume), record the with body end and the resume offset. The
   region [bodyEnd, resume) (implicit __exit__ call + jump + exception cleanup
   handler) is then skipped during decompilation. With-statements without this
   clean shape are left unhandled (no regression). */
void ScanWithBlocks(PycRef<PycCode> code, PycModule* mod,
                           const std::vector<PycExceptionTableEntry>& entries,
                           std::map<int, int>& bodyEndByBefore,
                           std::map<int, int>& resumeByBodyEnd)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    int opcode, operand, pos = 0;
    std::vector<int> befores;
    std::vector<std::pair<int, int>> fwdJumps;   /* (pos, target) */
    std::map<int, int> opcodeAt;                 /* pos -> opcode */
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        opcodeAt[p] = opcode;
        if (opcode == Pyc::BEFORE_WITH || opcode == Pyc::BEFORE_ASYNC_WITH)
            befores.push_back(p);
        else if (opcode == Pyc::JUMP_FORWARD_A)
            fwdJumps.push_back(std::make_pair(p, pos + operand * 2));
    }
    for (int bp : befores) {
        int bodyEnd = -1, handler = -1;
        for (const auto& e : entries) {
            if (e.start_offset > bp) {
                bodyEnd = e.end_offset;
                handler = e.target;
                break;
            }
        }
        if (bodyEnd < 0 || handler < 0)
            continue;
        /* Confirm the handler is a genuine with-cleanup: it must begin with
           PUSH_EXC_INFO followed by WITH_EXCEPT_START. */
        auto h0 = opcodeAt.find(handler);
        if (h0 == opcodeAt.end() || h0->second != Pyc::PUSH_EXC_INFO)
            continue;
        bool hasWithExcept = false;
        for (const auto& kv : opcodeAt) {
            if (kv.first > handler && kv.first <= handler + 4) {
                if (kv.second == Pyc::WITH_EXCEPT_START) { hasWithExcept = true; break; }
            }
        }
        if (!hasWithExcept)
            continue;
        int resume = -1;
        for (const auto& jp : fwdJumps) {
            /* The normal-exit jump sits between the body end and the handler and
               jumps over the handler (target at/after it). */
            if (jp.first >= bodyEnd && jp.first < handler && jp.second >= handler) {
                resume = jp.second;
                break;
            }
        }
        if (resume < 0)
            continue;   /* only the clean jump-over-handler shape */
        bodyEndByBefore[bp] = bodyEnd;
        resumeByBodyEnd[bodyEnd] = resume;
    }
}

/* Python 3.11 try/finally pre-pass. A finally compiles to: try body -> finally
   body (normal copy) -> JUMP over an exception handler that duplicates the
   finally body and re-raises. Recognize it from the exception table and record,
   per try-body entry start: the try body end, the finally block end (the JUMP
   over the duplicate), and the resume offset. The duplicate handler region is
   skipped during decompilation. Distinguishes finally from except handlers:
   after PUSH_EXC_INFO a finally has neither a POP_TOP (bare except) nor a
   CHECK_EXC_MATCH (typed except). */
void ScanTryFinally(PycRef<PycCode> code, PycModule* mod,
                           const std::vector<PycExceptionTableEntry>& entries,
                           std::map<int, int>& tryEndByStart,
                           std::map<int, int>& finallyEndByStart,
                           std::map<int, int>& resumeByFinallyEnd,
                           std::set<int>& returnStyleStarts,
                           std::set<int>& suppressReturns)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    int opcode, operand, pos = 0;
    std::map<int, int> opcodeAt;
    std::vector<std::pair<int, int>> fwdJumps;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        opcodeAt[p] = opcode;
        if (opcode == Pyc::JUMP_FORWARD_A)
            fwdJumps.push_back(std::make_pair(p, pos + operand * 2));
    }
    for (const auto& e : entries) {
        if (e.push_lasti)
            continue;                       /* try-body entries are lasti=False */
        int T = e.target;
        auto h0 = opcodeAt.find(T);
        if (h0 == opcodeAt.end() || h0->second != Pyc::PUSH_EXC_INFO)
            continue;
        /* handler region end: the exception entry that starts at the handler */
        int handlerEnd = -1;
        for (const auto& e2 : entries) {
            if (e2.start_offset == T) { handlerEnd = e2.end_offset; break; }
        }
        if (handlerEnd < 0)
            continue;
        auto h1 = opcodeAt.find(T + 2);
        if (h1 != opcodeAt.end() && h1->second == Pyc::POP_TOP)
            continue;                       /* bare except, not finally */
        bool hasCheck = false;
        for (const auto& kv : opcodeAt) {
            if (kv.first >= T && kv.first < handlerEnd
                    && kv.second == Pyc::CHECK_EXC_MATCH) {
                hasCheck = true;
                break;
            }
        }
        if (hasCheck)
            continue;                       /* typed except, not finally */
        /* The normal finally copy ends with a JUMP over the handler. */
        int jumpPos = -1, resume = -1;
        for (const auto& jp : fwdJumps) {
            if (jp.first >= e.end_offset && jp.first < T && jp.second >= T) {
                jumpPos = jp.first;
                resume = jp.second;
                break;
            }
        }
        if (jumpPos < 0) {
            /* `try: ... return X` + finally: the normal-path finally copy is
               not followed by a jump over the handler but ends in the try's
               RETURN_VALUE (the return value stays on the stack across the
               finally body). Recognize the instruction right before the
               handler being RETURN_VALUE; the finally copy then spans up to
               the handler, and every RETURN_VALUE in it is the try's own
               `return X` (rendered inside the try instead). */
            auto prevIt = opcodeAt.lower_bound(T);
            if (prevIt == opcodeAt.begin())
                continue;
            --prevIt;
            if (prevIt->second != Pyc::RETURN_VALUE
                    && prevIt->second != Pyc::RERAISE
                    && prevIt->second != Pyc::RERAISE_A)
                continue;
            bool anyRet = false, anyJumpOut = false;
            for (auto it = opcodeAt.lower_bound(e.end_offset);
                    it != opcodeAt.end() && it->first < T; ++it) {
                if (it->second == Pyc::RETURN_VALUE)
                    anyRet = true;
            }
            for (const auto& jp : fwdJumps)
                if (jp.first >= e.end_offset && jp.first < T && jp.second >= T)
                    anyJumpOut = true;
            if (!anyRet || anyJumpOut)
                continue;
            /* handler cluster end: entries starting inside the handler */
            int maxEnd = T + 2;
            bool grew = true;
            while (grew) {
                grew = false;
                for (const auto& e2 : entries) {
                    if (e2.start_offset >= T && e2.start_offset < maxEnd) {
                        int m = std::max(e2.end_offset, e2.target + 2);
                        if (m > maxEnd) { maxEnd = m; grew = true; }
                    }
                }
            }
            int res = -1;
            for (auto it = opcodeAt.lower_bound(maxEnd - 2); it != opcodeAt.end(); ++it) {
                if (it->second == Pyc::RERAISE || it->second == Pyc::RERAISE_A) {
                    auto nx = std::next(it);
                    res = (nx == opcodeAt.end()) ? (int)code->code()->length() : nx->first;
                    break;
                }
            }
            if (res < 0)
                continue;
            for (auto it = opcodeAt.lower_bound(e.end_offset);
                    it != opcodeAt.end() && it->first < T; ++it) {
                if (it->second == Pyc::RETURN_VALUE)
                    suppressReturns.insert(it->first);
            }
            tryEndByStart[e.start_offset] = e.end_offset;
            finallyEndByStart[e.start_offset] = T;
            resumeByFinallyEnd[T] = res;
            returnStyleStarts.insert(e.start_offset);
            continue;
        }
        tryEndByStart[e.start_offset] = e.end_offset;
        finallyEndByStart[e.start_offset] = jumpPos;
        resumeByFinallyEnd[jumpPos] = resume;
    }
}

/* Python 3.11 while-loop pre-pass. A while loop with the standard bottom-test
   layout compiles to:
        <eval cond>; POP_JUMP_FORWARD_IF_FALSE end     (entry guard)
   loop_start:
        <body>
        <eval cond>; POP_JUMP_BACKWARD_IF_TRUE loop_start   (back-edge)
   end:
   Without recognition pycdc renders both halves as independent `if` blocks and
   never forms the loop (the body's trailing back-edge becomes `if not cond:
   pass`). Detect it from a conditional backward jump whose target is the
   instruction immediately following a matching forward guard, and whose guard
   skips to the instruction right after the back-edge. Record the guard position
   (the BLK_WHILE opens there) and the back-edge position (the loop closes
   there). */
void ScanLoops(PycRef<PycCode> code, PycModule* mod, WhileLoops& out)
{
    struct Ins { int pos, op, arg, next, real; };
    PycBuffer src(code->code()->value(), code->code()->length());
    std::vector<Ins> ins;
    int opcode, operand, pos = 0;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        ins.push_back({p, opcode, operand, pos, pos});
    }
    /* `real`: position after trailing inline CACHE entries */
    for (int i = (int)ins.size() - 1; i >= 0; i--) {
        if (i + 1 < (int)ins.size() && ins[i + 1].op == Pyc::CACHE && ins[i].op != Pyc::CACHE)
            ins[i].real = ins[i + 1].real;
    }
    std::map<int, int> idxAt;
    for (int i = 0; i < (int)ins.size(); i++)
        idxAt[ins[i].pos] = i;

    const int unit = mod->verCompare(3, 10) >= 0 ? 2 : 1;   /* jump operand unit */
    enum Kind { NONE, UNCOND, COND };
    /* Returns jump kind; sets target. */
    auto jumpRaw = [&](const Ins& in, int& target) -> Kind {
        switch (in.op) {
        case Pyc::JUMP_ABSOLUTE_A:
            target = in.arg * unit;
            return UNCOND;
        case Pyc::JUMP_FORWARD_A:
            target = in.real + in.arg * unit;
            return UNCOND;
        case Pyc::JUMP_BACKWARD_A:
        case Pyc::JUMP_BACKWARD_NO_INTERRUPT_A:
            target = in.real - in.arg * 2;
            return UNCOND;
        case Pyc::POP_JUMP_IF_FALSE_A:
        case Pyc::POP_JUMP_IF_TRUE_A:
            if (mod->verCompare(3, 12) >= 0)
                target = in.real + in.arg * 2;
            else
                target = in.arg * unit;
            return COND;
        case Pyc::POP_JUMP_IF_NONE_A:
        case Pyc::POP_JUMP_IF_NOT_NONE_A:
            target = in.real + in.arg * 2;
            return COND;
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
            target = in.real + in.arg * 2;
            return COND;
        case Pyc::POP_JUMP_BACKWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NONE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A:
            target = in.real - in.arg * 2;
            return COND;
        default:
            return NONE;
        }
    };
    /* A backward jump may land on a NOP line marker just before the real
       loop header (`while True:` and `continue`); treat both as one target. */
    auto jump = [&](const Ins& in, int& target) -> Kind {
        Kind k = jumpRaw(in, target);
        if (k != NONE && target < in.pos) {
            auto it = idxAt.find(target);
            while (it != idxAt.end() && ins[it->second].op == Pyc::NOP
                    && it->second + 1 < (int)ins.size()) {
                target = ins[it->second + 1].pos;
                it = idxAt.find(target);
            }
        }
        return k;
    };
    auto isExprOp = [](int op) {
        switch (op) {
        case Pyc::LOAD_FAST_A: case Pyc::LOAD_CONST_A: case Pyc::LOAD_NAME_A:
        case Pyc::LOAD_GLOBAL_A: case Pyc::LOAD_DEREF_A: case Pyc::LOAD_ATTR_A:
        case Pyc::LOAD_METHOD_A: case Pyc::COMPARE_OP_A: case Pyc::IS_OP_A:
        case Pyc::CONTAINS_OP_A: case Pyc::BINARY_OP_A: case Pyc::BINARY_SUBSCR:
        case Pyc::UNARY_NOT: case Pyc::UNARY_NEGATIVE: case Pyc::CALL_A:
        case Pyc::CALL_FUNCTION_A: case Pyc::CALL_METHOD_A: case Pyc::PRECALL_A:
        case Pyc::PUSH_NULL: case Pyc::KW_NAMES_A: case Pyc::TO_BOOL: case Pyc::NOT_TAKEN:
        case Pyc::CACHE: case Pyc::LOAD_FAST_LOAD_FAST_A: case Pyc::LOAD_FAST_BORROW_A:
        case Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A: case Pyc::LOAD_SMALL_INT_A:
        case Pyc::BINARY_ADD: case Pyc::BINARY_SUBTRACT: case Pyc::BINARY_MULTIPLY:
        case Pyc::BINARY_MODULO: case Pyc::BINARY_TRUE_DIVIDE: case Pyc::BINARY_FLOOR_DIVIDE:
            return true;
        default:
            return false;
        }
    };

    struct Loop { int T, B, end, csStart, firstBack; bool typeII; };
    std::vector<Loop> loops;

    /* back-edge candidates grouped by header */
    std::map<int, int> lastUncond;   /* T -> last unconditional back-jump index */
    for (int i = 0; i < (int)ins.size(); i++) {
        int t;
        if (jump(ins[i], t) == UNCOND && t < ins[i].pos
                && ins[i].op != Pyc::JUMP_FORWARD_A) {
            auto ti = idxAt.find(t);
            if (ti == idxAt.end() || ins[ti->second].op == Pyc::FOR_ITER_A)
                continue;
            lastUncond[t] = i;
        }
    }
    auto prevReal = [&](int i) {
        i--;
        while (i >= 0 && ins[i].op == Pyc::CACHE)
            i--;
        return i;
    };
    std::set<int> usedBack;

    /* bottom-tested loops with a conditional back-edge (3.10, 3.11) */
    for (int i = 0; i < (int)ins.size(); i++) {
        int t;
        Kind k = jump(ins[i], t);
        if (k != COND || t >= ins[i].pos)
            continue;
        auto ti = idxAt.find(t);
        if (ti == idxAt.end())
            continue;
        int g = prevReal(ti->second);
        int gt;
        if (g < 0 || jump(ins[g], gt) != COND || gt <= ins[g].pos)
            continue;
        if (gt != ins[i].real)
            continue;
        Loop l{t, ins[i].pos, ins[i].real, -1, ins[g].pos, true};
        out.guardEnd[ins[g].pos] = l.end;
        out.backedges.insert(ins[i].pos);
        loops.push_back(l);
        usedBack.insert(i);
    }
    /* start of the condition that precedes index `g` (the entry guard); jumps
       that target it are threaded `continue`s of a bottom-tested loop */
    std::set<int> entryStarts;
    auto entryStartOf = [&](int g) {
        int j = g - 1, first = -1;
        int tg;
        while (j >= 0 && (isExprOp(ins[j].op) || jump(ins[j], tg) == COND)) {
            first = j;
            j--;
        }
        return first >= 0 ? ins[first].pos : -1;
    };
    for (const auto& l : loops) {
        if (idxAt.count(l.firstBack))
            entryStarts.insert(entryStartOf(idxAt[l.firstBack]));
    }
    if (mod->verCompare(3, 12) >= 0) {
        for (const auto& lu : lastUncond) {
            int t = lu.first, bi = lu.second;
            int p = prevReal(bi), pt, gt;
            int g = idxAt.count(t) ? prevReal(idxAt[t]) : -1;
            if (p >= 0 && jump(ins[p], pt) == COND && pt == ins[bi].real
                    && g >= 0 && jump(ins[g], gt) == COND && gt == ins[bi].real)
                entryStarts.insert(entryStartOf(g));
        }
    }
    /* unconditional back-edges */
    for (const auto& lu : lastUncond) {
        int t = lu.first, bi = lu.second;
        if (usedBack.count(bi))
            continue;
        int p = prevReal(bi);
        int pt;
        int g = idxAt.count(t) ? prevReal(idxAt[t]) : -1;
        int gt;
        if (mod->verCompare(3, 12) >= 0 && p >= 0 && jump(ins[p], pt) == COND
                && pt == ins[bi].real && g >= 0 && jump(ins[g], gt) == COND
                && gt == ins[bi].real) {
            /* 3.12+: cond; POP_JUMP_IF_FALSE end; JUMP_BACKWARD T */
            Loop l{t, ins[bi].pos, ins[bi].real, -1, ins[g].pos, true};
            out.guardEnd[ins[g].pos] = l.end;
            out.backedges.insert(ins[p].pos);
            out.skipJumps.insert(ins[bi].pos);
            loops.push_back(l);
            continue;
        }
        if (entryStarts.count(t))
            continue;   /* a `continue` jumping to a bottom-tested loop's entry test */
        /* top-tested: look for the guard at the header */
        int guard = -1;
        bool clean = true;
        std::vector<int> cj;   /* conditional jumps of the header condition */
        int bodyIdx = idxAt[t];
        for (; bodyIdx < bi; bodyIdx++) {
            int tt;
            Kind k = jump(ins[bodyIdx], tt);
            if (k == NONE) {
                if (!isExprOp(ins[bodyIdx].op)) break;
                continue;
            }
            if (k != COND) break;
            cj.push_back(bodyIdx);
        }
        /* The guard is the last conditional jump of the header condition: it
           leaves the loop, and every earlier jump either also leaves it (`and`)
           or enters the body right after the guard (`or`). Later conditional
           jumps belong to statements of the body. */
        for (int gi = (int)cj.size() - 1; gi >= 0 && guard < 0; gi--) {
            int gtgt = 0;
            jump(ins[cj[gi]], gtgt);
            if (gtgt < ins[bi].real)
                continue;
            int bodyStart = ins[cj[gi]].real;
            {
                int nx = cj[gi] + 1;
                while (nx < (int)ins.size() && (ins[nx].op == Pyc::CACHE
                                                || ins[nx].op == Pyc::NOT_TAKEN))
                    nx++;
                if (nx < (int)ins.size())
                    bodyStart = ins[nx].pos;
            }
            bool ok = true;
            for (int c = 0; ok && c < gi; c++) {
                int ct = 0;
                jump(ins[cj[c]], ct);
                if (ct < ins[bi].real && ct != bodyStart)
                    ok = false;
            }
            if (ok) {
                guard = ins[cj[gi]].pos;
                cj.resize(gi + 1);
            }
        }
        if (guard < 0)
            clean = false;
        Loop l{t, ins[bi].pos, ins[bi].real, -1, guard, false};
        if (guard >= 0 && clean) {
            out.guardEnd[guard] = l.end;
            if (cj.size() > 1) {
                std::vector<ChainJump> chain;
                for (size_t c = 0; c + 1 < cj.size(); c++) {
                    int ct = 0;
                    jump(ins[cj[c]], ct);
                    chain.push_back(ChainJump{ins[cj[c]].pos,
                                              ins[cj[c]].op == Pyc::POP_JUMP_IF_TRUE_A
                                              || ins[cj[c]].op == Pyc::POP_JUMP_FORWARD_IF_TRUE_A,
                                              ct < ins[bi].real});
                    out.guardChainMember.insert(ins[cj[c]].pos);
                }
                out.guardChain[guard] = chain;
            }
        } else if (guard < 0) {
            /* `while True` -- the body starts at T and is not a bare expression */
            out.trueOpen[t] = l.end;
        } else {
            continue;   /* unrecognized shape: leave it alone */
        }
        out.uncondBack.insert(ins[bi].pos);
        loops.push_back(l);
    }

    /* `break` inside a `for` loop: the iterator is popped, then the loop is
       left by an unconditional forward jump past the loop's exit. (A plain
       expression statement also ends in POP_TOP, but its join jump never
       leaves the loop.) */
    for (int f = 0; f < (int)ins.size(); f++) {
        if (ins[f].op != Pyc::FOR_ITER_A)
            continue;
        int e = ins[f].real + ins[f].arg * unit;
        int lastBack = -1;
        for (int i = f + 1; i < (int)ins.size() && ins[i].pos < e; i++) {
            int t;
            if (jump(ins[i], t) == UNCOND && t == ins[f].pos && t < ins[i].pos)
                lastBack = i;
        }
        if (lastBack < 0)
            continue;
        int exitX = -1;
        for (int i = f + 1; i + 1 <= lastBack; i++) {
            if (ins[i].op != Pyc::POP_TOP && ins[i].op != Pyc::POP_ITER)
                continue;
            int n = i + 1;
            while (n < (int)ins.size() && ins[n].op == Pyc::CACHE)
                n++;
            int t;
            if (n >= (int)ins.size() || jump(ins[n], t) != UNCOND || t < e
                    || t <= ins[n].pos || out.skipJumps.count(ins[n].pos))
                continue;
            out.forBreakPops.insert(ins[i].pos);
            out.forBreaks.insert(ins[n].pos);
            if (exitX < 0 || t > exitX) exitX = t;
        }
        if (exitX > e) {
            /* the `else` body starts after the loop exit sequence */
            int n = idxAt.count(e) ? idxAt[e] : -1;
            if (n >= 0) {
                if (ins[n].op == Pyc::END_FOR) {
                    n++;
                    while (n < (int)ins.size() && ins[n].op == Pyc::CACHE)
                        n++;
                    if (n < (int)ins.size()
                            && (ins[n].op == Pyc::POP_TOP || ins[n].op == Pyc::POP_ITER))
                        n++;
                }
                if (n < (int)ins.size() && ins[n].pos < exitX)
                    out.forElse[ins[n].pos] = exitX;
            }
        }
    }

    for (auto& l : loops) {
        out.loopEnds.insert(l.end);
        /* start of the duplicated condition (bottom-tested loops) */
        int csPos = l.T;
        int gcStart = -1;   /* start of the entry guard's condition (target of a threaded `continue`) */
        if (l.typeII) {
            auto isChainJump = [&](int j, int& tgt) {
                if (jump(ins[j], tgt) != COND)
                    return false;
                /* exit, body start, or a later part of the same condition */
                return tgt == l.end || tgt == l.T
                       || (tgt > ins[j].pos && tgt < l.end && tgt <= ins[idxAt[l.B]].real);
            };
            /* walk back over a (possibly `and`/`or` compound) condition expression */
            auto walkCond = [&](int from, int stopPos, std::vector<ChainJump>& chain) {
                int first = from + 1;
                for (int j = from; j >= 0 && ins[j].pos >= stopPos; j--) {
                    int tt;
                    if (isChainJump(j, tt)) {
                        chain.insert(chain.begin(), ChainJump{ins[j].pos,
                                     ins[j].op == Pyc::POP_JUMP_IF_TRUE_A
                                     || ins[j].op == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                                     || ins[j].op == Pyc::POP_JUMP_BACKWARD_IF_TRUE_A,
                                     tt == l.T});
                        first = j;
                        continue;
                    }
                    if (jump(ins[j], tt) == UNCOND && tt == l.T && ins[j].pos != l.B) {
                        /* 3.12+ `or` tails loop back after each part */
                        out.skipJumps.insert(ins[j].pos);
                        first = j;
                        continue;
                    }
                    if (jump(ins[j], tt) != NONE || !isExprOp(ins[j].op))
                        break;
                    first = j;
                }
                return first;
            };
            int bi = idxAt[l.B];
            int closing = bi;   /* the conditional back-edge */
            int ctgt;
            if (jump(ins[bi], ctgt) != COND)
                closing = prevReal(bi);
            std::vector<ChainJump> tail;
            int firstCond = walkCond(closing - 1, l.T, tail);
            for (const auto& cj : tail)
                out.tailChain.insert(cj.pos);
            csPos = ins[std::min(firstCond, bi)].pos;
            l.csStart = csPos;
            if (idxAt.count(l.firstBack)) {
                int g = idxAt[l.firstBack];
                std::vector<ChainJump> chain;
                int first = walkCond(g - 1, 0, chain);
                if (first < g)
                    gcStart = ins[first].pos;
                if (!chain.empty()) {
                    for (const auto& cj : chain)
                        out.guardChainMember.insert(cj.pos);
                    out.guardChain[ins[g].pos] = chain;
                }
            }
        }
        int exitX = -1;
        bool consistent = true;
        std::vector<int> brk, cont;
        for (int i = idxAt[l.T]; i < (int)ins.size() && ins[i].pos < l.B; i++) {
            int t;
            Kind k = jump(ins[i], t);
            if (k != UNCOND || out.skipJumps.count(ins[i].pos))
                continue;
            /* innermost loop owns the jump */
            bool innerOwns = false;
            for (const auto& o : loops) {
                if (&o != &l && o.T > l.T && o.T <= ins[i].pos && ins[i].pos <= o.B
                        && (o.B < l.B)) { innerOwns = true; break; }
            }
            if (innerOwns)
                continue;
            if (t >= l.end) {
                brk.push_back(ins[i].pos);
                if (exitX < 0) exitX = t;
                else if (exitX != t) consistent = false;
            } else if (!l.typeII && t == l.T) {
                cont.push_back(ins[i].pos);
            } else if (l.typeII && (t == l.csStart || t == gcStart)
                       && ins[i].pos < l.csStart) {
                cont.push_back(ins[i].pos);
            }
        }
        (void)consistent;
        if (!l.typeII) {
            auto ei = idxAt.find(l.end);
            int bIdx = idxAt[l.B];
            int fwd = -1;
            if (ei != idxAt.end() && ins[ei->second].op == Pyc::JUMP_FORWARD_A) {
                /* `while ...: ... else:` -- the elided break became a jump
                   over the else body */
                jump(ins[ei->second], fwd);
            }
            if (ei != idxAt.end() && (ins[ei->second].op == Pyc::NOP || fwd > l.end)) {
                if (fwd > l.end) {
                    out.elseEnd[l.end] = fwd;
                    out.skipJumps.insert(ins[ei->second].pos);
                }
                for (int i = idxAt[l.T]; i < bIdx; i++) {
                    int t;
                    if (jump(ins[i], t) != COND || t != l.end || ins[i].pos == l.firstBack)
                        continue;
                    int n = i + 1;
                    while (n < (int)ins.size() && (ins[n].op == Pyc::CACHE
                                                   || ins[n].op == Pyc::NOT_TAKEN))
                        n++;
                    if (n == bIdx)
                        out.breakIf[ins[i].pos] =
                                (ins[i].op == Pyc::POP_JUMP_IF_TRUE_A);
                }
            }
        }
        for (int b : brk) out.breaks.insert(b);
        for (int c : cont) out.continues.insert(c);
        if (exitX > l.end)
            out.elseEnd[l.end] = exitX;
    }
}

/* Python 3.11 chained-comparison pre-pass. `a == b == c` compiles to two
   COMPARE_OP/POP_JUMP_FORWARD_IF_FALSE halves sharing the middle operand
   (duplicated via SWAP/COPY), followed by a trailer:
        JUMP_FORWARD body      (both comparisons true -> run the body)
   tramp:
        POP_TOP                (false path: discard the leftover middle operand)
        JUMP_FORWARD exit
   body:
   The first half's POP_JUMP targets `tramp`, the second's targets `exit`; the
   default merge logic sees different targets and joins them with `or` instead of
   `and`, leaving the body outside the if. Record: the trailer's leading
   JUMP_FORWARD (to skip over the trampoline) and the trampoline POP_TOP position
   mapped to its real exit, so a POP_JUMP landing on the trampoline can be
   resolved to the exit and the two halves merge with `and`. */
void ScanChainedCompare(PycRef<PycCode> code, PycModule* mod,
                               std::map<int,int>& chainSkipJumps,
                               std::map<int,int>& trampolineExit)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    int opcode, operand, pos = 0;
    std::vector<int> order;
    std::map<int,int> opAt, operAt, nextAt;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        order.push_back(p);
        opAt[p] = opcode; operAt[p] = operand; nextAt[p] = pos;
    }
    for (size_t i = 0; i + 2 < order.size(); i++) {
        int j = order[i], p = order[i + 1], q = order[i + 2];
        if (opAt[j] == Pyc::JUMP_FORWARD_A
                && opAt[p] == Pyc::POP_TOP
                && opAt[q] == Pyc::JUMP_FORWARD_A) {
            int bodyTarget = nextAt[j] + operAt[j] * 2;
            int exitTarget = nextAt[q] + operAt[q] * 2;
            /* the trailer's leading JUMP_FORWARD must jump just past the
               trampoline (to the body that begins after q) */
            if (bodyTarget == nextAt[q]) {
                chainSkipJumps[j] = bodyTarget;
                trampolineExit[p] = exitTarget;
            }
        }
    }
}

/* Python 3.11 try/except/else: the else body is the code between the end of
   the protected range and the JUMP_FORWARD (just before the handler) that skips
   the handlers. Map: protected-range end -> (jump position, handler offset). */
/* Python 3.12+ inlined comprehensions are wrapped in an exception-table
   entry whose handler only restores the saved loop variable and re-raises:
       SWAP n; POP_TOP; SWAP n; STORE_FAST v ...; RERAISE 0
   Locate those handlers so they can be dropped from the table (they are not
   a user-level try) and skipped when decompiling. */
void ScanComprehensionCleanup(PycRef<PycCode> code, PycModule* mod,
                                     std::vector<PycExceptionTableEntry>& entries,
                                     std::vector<std::pair<int, int> >& skip)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    int opcode, operand, pos = 0;
    std::map<int, int> opAt, operAt, nextAt;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        opAt[p] = opcode; operAt[p] = operand; nextAt[p] = pos;
    }
    auto skipCache = [&](int p) {
        while (opAt.count(p) && opAt[p] == Pyc::CACHE)
            p = nextAt[p];
        return p;
    };
    std::set<int> targets;
    for (const auto& e : entries) {
        int p = e.target;
        if (!opAt.count(p) || opAt[p] != Pyc::SWAP_A)
            continue;
        p = nextAt[p];
        if (!opAt.count(p) || opAt[p] != Pyc::POP_TOP)
            continue;
        p = nextAt[p];
        if (!opAt.count(p) || opAt[p] != Pyc::SWAP_A)
            continue;
        p = nextAt[p];
        int stores = 0;
        while (opAt.count(p) && opAt[p] == Pyc::STORE_FAST_A) {
            stores++;
            p = nextAt[p];
        }
        if (!stores || !opAt.count(p) || opAt[p] != Pyc::RERAISE_A)
            continue;
        p = nextAt[p];
        if (targets.insert(e.target).second)
            skip.push_back(std::make_pair(e.target, p));
    }
    if (targets.empty())
        return;
    std::vector<PycExceptionTableEntry> kept;
    for (const auto& e : entries) {
        if (targets.count(e.target))
            continue;
        bool inSkip = false;
        for (const auto& r : skip)
            if (e.start_offset >= r.first && e.start_offset < r.second)
                inSkip = true;
        if (!inSkip)
            kept.push_back(e);
    }
    entries.swap(kept);
    (void)skipCache;
}

void ScanTryElse(PycRef<PycCode> code, PycModule* mod,
                        const std::vector<PycExceptionTableEntry>& entries,
                        std::map<int, std::pair<int, int> >& elseRegionByStart)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    int opcode, operand, pos = 0;
    std::map<int, int> opAt, operAt, nextAt;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        opAt[p] = opcode; operAt[p] = operand; nextAt[p] = pos;
    }
    for (const auto& e : entries) {
        int handler = e.target;
        int jp = handler - 2;
        auto it = opAt.find(jp);
        if (it == opAt.end() || it->second != Pyc::JUMP_FORWARD_A)
            continue;
        int dest = nextAt[jp] + operAt[jp] * 2;
        if (dest <= handler || e.end_offset >= jp || e.start_offset >= e.end_offset)
            continue;
        /* the handler must be a plain except handler */
        auto h = opAt.find(handler);
        if (h == opAt.end() || h->second != Pyc::PUSH_EXC_INFO)
            continue;
        /* the first entry ending here that targets this handler */
        if (!elseRegionByStart.count(e.end_offset))
            elseRegionByStart[e.end_offset] = std::make_pair(jp, handler);
    }
}
