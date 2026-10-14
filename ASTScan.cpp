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
/* Chained comparison in a jump context (`if a < b < c:`, `not`, `or`, `while`,
   ternary tests). The first half's false jump lands on a lone `POP_TOP`
   (discarding the shared middle operand), which then either jumps on to the
   real false exit or falls into it. Record dest[POP_TOP] = real exit so every
   jump to the cleanup is read as a jump to the exit, and list the cleanup (plus
   the trailer jump that hops over it) in `skip`. */
void ScanChainCleanups(PycRef<PycCode> code, PycModule* mod,
                       std::map<int, int>& dest, std::vector<std::pair<int, int> >& skip)
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
    const int n = (int)ins.size();
    for (int i = n - 1; i >= 0; i--)
        if (i + 1 < n && ins[i + 1].op == Pyc::CACHE && ins[i].op != Pyc::CACHE)
            ins[i].real = ins[i + 1].real;
    std::map<int, int> idxAt;
    for (int i = 0; i < n; i++)
        idxAt[ins[i].pos] = i;
    const int unit = mod->verCompare(3, 10) >= 0 ? 2 : 1;
    auto prevReal = [&](int i) {
        i--;
        while (i >= 0 && (ins[i].op == Pyc::CACHE || ins[i].op == Pyc::NOT_TAKEN))
            i--;
        return i;
    };
    auto nextReal = [&](int i) {
        i++;
        while (i < n && (ins[i].op == Pyc::CACHE || ins[i].op == Pyc::NOT_TAKEN))
            i++;
        return i;
    };
    /* target of a POP_JUMP_IF_FALSE, or -1 */
    auto falseJump = [&](const Ins& in) -> int {
        switch (in.op) {
        case Pyc::POP_JUMP_IF_FALSE_A:
            return mod->verCompare(3, 12) >= 0 ? in.real + in.arg * 2 : in.arg * unit;
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
            return in.real + in.arg * 2;
        default:
            return -1;
        }
    };
    auto fwdJump = [&](const Ins& in) -> int {
        if (in.op == Pyc::JUMP_FORWARD_A)
            return in.real + in.arg * unit;
        if (in.op == Pyc::JUMP_ABSOLUTE_A && mod->verCompare(3, 11) < 0) {
            int t = in.arg * unit;
            return t > in.pos ? t : -1;
        }
        return -1;
    };
    auto isCmp = [&](int op) {
        return op == Pyc::COMPARE_OP_A || op == Pyc::IS_OP_A || op == Pyc::CONTAINS_OP_A;
    };
    for (int i = 0; i < n; i++) {
        int t = falseJump(ins[i]);
        if (t <= ins[i].pos)
            continue;
        int cmp = prevReal(i);
        if (cmp < 0 || !isCmp(ins[cmp].op))
            continue;
        int h1 = prevReal(cmp), h2 = h1 >= 0 ? prevReal(h1) : -1;
        bool head = false;
        if (h1 >= 0 && h2 >= 0) {
            if (mod->verCompare(3, 11) >= 0)
                head = ins[h1].op == Pyc::COPY_A && ins[h1].arg == 2
                        && ins[h2].op == Pyc::SWAP_A && ins[h2].arg == 2;
            else
                head = ins[h1].op == Pyc::ROT_THREE && ins[h2].op == Pyc::DUP_TOP;
        }
        if (!head)
            continue;
        auto ti = idxAt.find(t);
        if (ti == idxAt.end() || ins[ti->second].op != Pyc::POP_TOP || dest.count(t))
            continue;
        int s = ti->second;
        int after = nextReal(s);
        if (after >= n)
            continue;
        int target = ins[after].pos, endIdx = after;
        int J = fwdJump(ins[after]);
        if (J > 0) {
            target = J;
            endIdx = nextReal(after);
        } else {
            /* the cleanup may end in a copy of the exit's `return` (the compiler
               duplicates small exit blocks): `POP_TOP; [LOAD_CONST]; RETURN`
               equals the block the second half's false jump lands on */
            int u = -1;
            for (int k = i + 1; k < s && u < 0; k++) {
                int ft = falseJump(ins[k]);
                if (ft > ins[s].pos)
                    u = ft;
            }
            auto ui = u >= 0 ? idxAt.find(u) : idxAt.end();
            if (ui != idxAt.end()) {
                int a = after, b = ui->second;
                bool same = false;
                while (a < n && b < n && ins[a].op == ins[b].op && ins[a].arg == ins[b].arg) {
                    if (ins[a].op == Pyc::RETURN_VALUE || ins[a].op == Pyc::RETURN_CONST_A) {
                        same = true;
                        break;
                    }
                    /* straight-line code only: any jump ends the comparison */
                    if (fwdJump(ins[a]) > 0 || falseJump(ins[a]) > 0
                            || ins[a].op == Pyc::POP_JUMP_IF_TRUE_A
                            || ins[a].op == Pyc::JUMP_BACKWARD_A
                            || ins[a].op == Pyc::FOR_ITER_A)
                        break;
                    a = nextReal(a);
                    b = nextReal(b);
                    if (a >= n || b >= n)
                        break;
                }
                if (same) {
                    target = u;
                    endIdx = nextReal(a);
                }
            }
        }
        /* the exit may be a jump-threaded `continue`: the second half's false
           jump goes straight to the loop header while the cleanup goes through
           the loop's back-edge */
        {
            auto di = idxAt.find(target);
            if (di != idxAt.end() && target > ins[s].pos) {
                const Ins& d = ins[di->second];
                if (d.op == Pyc::JUMP_ABSOLUTE_A && mod->verCompare(3, 11) < 0
                        && d.arg * unit < d.pos)
                    target = d.arg * unit;
                else if (d.op == Pyc::JUMP_BACKWARD_A)
                    target = d.real - d.arg * 2;
            }
        }
        int endPos = endIdx < n ? ins[endIdx].pos : ins[n - 1].real;
        int skipFrom = t;
        int before = prevReal(s);
        if (before >= 0 && fwdJump(ins[before]) == endPos) {
            skipFrom = ins[before].pos;
            dest[skipFrom] = endPos;    /* jumps to the trailer reach the same place */
        }
        /* `continue` as the if-body: the compiler follows the cleanup's jump with a
           dead copy of the body's own back-jump */
        if (before >= 0 && endIdx < n && endIdx != after
                && (ins[before].op == Pyc::JUMP_ABSOLUTE_A || ins[before].op == Pyc::JUMP_BACKWARD_A)
                && ins[endIdx].op == ins[before].op && ins[endIdx].arg == ins[before].arg
                && skipFrom == t) {
            endIdx = nextReal(endIdx);
            endPos = endIdx < n ? ins[endIdx].pos : ins[n - 1].real;
        }
        dest[t] = target;
        skip.push_back(std::make_pair(skipFrom, endPos));
    }
}

void ScanLoops(PycRef<PycCode> code, PycModule* mod, WhileLoops& out)
{
    ScanChainCleanups(code, mod, out.cleanupDest, out.chainSkip);
    struct Ins { int pos, op, arg, next, real; int fall; };
    PycBuffer src(code->code()->value(), code->code()->length());
    std::vector<Ins> ins;
    int opcode, operand, pos = 0;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        ins.push_back({p, opcode, operand, pos, pos, 0});
    }
    /* `real`: position after trailing inline CACHE entries */
    for (int i = (int)ins.size() - 1; i >= 0; i--) {
        if (i + 1 < (int)ins.size() && ins[i + 1].op == Pyc::CACHE && ins[i].op != Pyc::CACHE)
            ins[i].real = ins[i + 1].real;
    }
    /* chained-comparison trailers/cleanups do not take part in loop shapes */
    for (auto& in : ins)
        in.fall = in.real;
    if (!out.chainSkip.empty()) {
        std::vector<Ins> kept;
        for (const auto& in : ins) {
            bool skipIt = false;
            for (const auto& r : out.chainSkip)
                if (in.pos >= r.first && in.pos < r.second)
                    skipIt = true;
            if (!skipIt)
                kept.push_back(in);
        }
        /* `fall`: where control continues, hopping over erased ranges */
        for (auto& in : kept) {
            bool moved = true;
            while (moved) {
                moved = false;
                for (const auto& r : out.chainSkip)
                    if (in.fall == r.first) {
                        in.fall = r.second;
                        moved = true;
                    }
            }
        }
        ins.swap(kept);
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
        if (k == COND && target > in.pos) {
            auto cd = out.cleanupDest.find(target);
            if (cd != out.cleanupDest.end())
                target = cd->second;
        }
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
        case Pyc::DUP_TOP: case Pyc::ROT_THREE: case Pyc::SWAP_A: case Pyc::COPY_A:
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
    /* The end of a try body / except handler that threads straight back to the
       loop head, when the whole `try` closes the loop body (the closing
       back-edge follows its END_FINALLY/RERAISE), is a jump over the remaining
       handlers and not a `continue`. */
    for (int i = 1; i < (int)ins.size(); i++) {
        int t;
        if (ins[i].op != Pyc::JUMP_ABSOLUTE_A || jump(ins[i], t) != UNCOND || t >= ins[i].pos)
            continue;
        if (ins[i - 1].op != Pyc::POP_BLOCK && ins[i - 1].op != Pyc::POP_EXCEPT)
            continue;
        {
            /* 3.10 `for` loop closed by its try: the RERAISE is followed by the
               loop exit itself (the FOR_ITER target) */
            auto hi = idxAt.find(t);
            int ft = 0;
            if (hi != idxAt.end() && ins[hi->second].op == Pyc::FOR_ITER_A) {
                ft = ins[hi->second].real + ins[hi->second].arg * unit;
                auto ei = idxAt.find(ft);
                if (ei != idxAt.end() && ei->second > 0
                        && (ins[ei->second - 1].op == Pyc::RERAISE_A
                            || ins[ei->second - 1].op == Pyc::RERAISE
                            || ins[ei->second - 1].op == Pyc::END_FINALLY)) {
                    out.tryEndJump[ins[i].pos] = ft;
                    out.forTryEnd.insert(ft);
                    continue;
                }
            }
        }
        for (int k = i + 1; k < (int)ins.size(); k++) {
            int t2;
            if (ins[k].op != Pyc::JUMP_ABSOLUTE_A || jump(ins[k], t2) != UNCOND || t2 != t)
                continue;
            if (ins[k - 1].op == Pyc::END_FINALLY || ins[k - 1].op == Pyc::RERAISE
                    || ins[k - 1].op == Pyc::RERAISE_A) {
                out.tryEndJump[ins[i].pos] = ins[k].pos;
                break;
            }
            if (ins[k - 1].op != Pyc::POP_BLOCK && ins[k - 1].op != Pyc::POP_EXCEPT)
                break;   /* a real continue or other jump comes first */
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
        if (gt != ins[i].fall)
            continue;
        Loop l{t, ins[i].pos, ins[i].fall, -1, ins[g].pos, true};
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
            if (p >= 0 && jump(ins[p], pt) == COND && pt == ins[bi].fall
                    && g >= 0 && jump(ins[g], gt) == COND && gt == ins[bi].fall)
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
                && pt == ins[bi].fall && g >= 0 && jump(ins[g], gt) == COND
                && gt == ins[bi].fall) {
            /* 3.12+: cond; POP_JUMP_IF_FALSE end; JUMP_BACKWARD T */
            Loop l{t, ins[bi].pos, ins[bi].fall, -1, ins[g].pos, true};
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
                int hop = ins[bodyIdx].op;
                bool walrusStore = bodyIdx > 0
                        && (ins[bodyIdx - 1].op == Pyc::DUP_TOP || ins[bodyIdx - 1].op == Pyc::COPY_A)
                        && (hop == Pyc::STORE_FAST_A || hop == Pyc::STORE_NAME_A
                            || hop == Pyc::STORE_GLOBAL_A || hop == Pyc::STORE_DEREF_A);
                if (!isExprOp(hop) && !walrusStore) break;   /* `while (x := f()):` stores */
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
            if (gtgt < ins[bi].fall)
                continue;
            int bodyStart = ins[cj[gi]].fall;
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
                if (ct < ins[bi].fall && ct != bodyStart)
                    ok = false;
            }
            if (ok) {
                guard = ins[cj[gi]].pos;
                cj.resize(gi + 1);
            }
        }
        if (guard < 0)
            clean = false;
        Loop l{t, ins[bi].pos, ins[bi].fall, -1, guard, false};
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
                                              ct < ins[bi].fall});
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
        for (int i = f + 1; i < (int)ins.size() && ins[i].pos < e; i++) {
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
            /* 3.12+ tail form: `cond-jump X; JUMP_BACKWARD hdr; X: <break>` is
               `if c: break` (the compiler inverted the test). */
            {
                int jb = i - 1;
                while (jb >= 0 && ins[jb].op == Pyc::CACHE) jb--;
                int tb = 0;
                if (jb >= 0 && jump(ins[jb], tb) == UNCOND && tb == ins[f].pos
                        && ins[jb].pos > ins[f].pos) {
                    int cj = jb - 1;
                    while (cj >= 0 && (ins[cj].op == Pyc::CACHE || ins[cj].op == Pyc::NOT_TAKEN))
                        cj--;
                    int ct = 0;
                    if (cj >= 0 && jump(ins[cj], ct) == COND && ct == ins[i].pos
                            && (ins[cj].op == Pyc::POP_JUMP_IF_TRUE_A
                                || ins[cj].op == Pyc::POP_JUMP_IF_FALSE_A)) {
                        out.breakIf[ins[cj].pos] = (ins[cj].op == Pyc::POP_JUMP_IF_TRUE_A);
                        out.skipJumps.insert(ins[jb].pos);
                        out.forBreaks.erase(ins[n].pos);
                        out.skipJumps.insert(ins[n].pos);
                    }
                }
            }
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
                       || (tgt > ins[j].pos && tgt < l.end && tgt <= ins[idxAt[l.B]].fall);
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
                if (!out.tryEndJump.count(ins[i].pos))
                    cont.push_back(ins[i].pos);
            } else if (l.typeII && (t == l.csStart || t == gcStart)
                       && ins[i].pos < l.csStart) {
                /* end of a try body / handler right before the duplicated
                   loop test (3.10): a jump over the handlers, not a `continue` */
                int ti = idxAt.count(t) ? idxAt[t] : -1;
                bool tryEnd = i > 0 && ti > 0
                        && (ins[ti - 1].op == Pyc::RERAISE || ins[ti - 1].op == Pyc::RERAISE_A || ins[ti - 1].op == Pyc::END_FINALLY)
                        && (ins[i - 1].op == Pyc::POP_BLOCK || ins[i - 1].op == Pyc::POP_EXCEPT);
                if (!tryEnd)
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

/* ---------------------------------------------------------------------------
   Expression-level control flow (ternary `a if c else b`, value-context
   `and`/`or`). The statement-level `if` machinery cannot tell these from
   statements, so we classify the relevant jumps up front:

   ternary:  C; POP_JUMP_IF_FALSE else; A; JUMP_FORWARD end; else: B; end:
             -- told from an if/else *statement* by the last instruction of both
             branches leaving a value on the stack (not a store/pop/return).
   and/or:   A; JUMP_IF_FALSE_OR_POP end; B; end:            (<= 3.11)
             A; COPY 1; [TO_BOOL;] POP_JUMP_IF_FALSE end; POP_TOP; B; end: (3.12+)
             and the mixed form `a and b or c`, where the inner test is a plain
             POP_JUMP whose target directly follows a keep-style jump. */
namespace {
struct XIns { int pos, op, arg, next, real; };
}

static bool xIsStatementEnd(int op)
{
    switch (op) {
    case Pyc::STORE_FAST_A: case Pyc::STORE_NAME_A: case Pyc::STORE_GLOBAL_A:
    case Pyc::STORE_DEREF_A: case Pyc::STORE_ATTR_A: case Pyc::STORE_SUBSCR:
    case Pyc::STORE_FAST_STORE_FAST_A: case Pyc::STORE_FAST_LOAD_FAST_A:
    case Pyc::DELETE_FAST_A: case Pyc::DELETE_NAME_A: case Pyc::DELETE_GLOBAL_A:
    case Pyc::DELETE_ATTR_A: case Pyc::DELETE_SUBSCR: case Pyc::DELETE_DEREF_A:
    case Pyc::POP_TOP: case Pyc::RETURN_VALUE: case Pyc::RETURN_CONST_A:
    case Pyc::RAISE_VARARGS_A: case Pyc::RERAISE: case Pyc::RERAISE_A:
    case Pyc::JUMP_FORWARD_A: case Pyc::JUMP_ABSOLUTE_A: case Pyc::JUMP_BACKWARD_A:
    case Pyc::JUMP_BACKWARD_NO_INTERRUPT_A:
    case Pyc::POP_JUMP_IF_FALSE_A: case Pyc::POP_JUMP_IF_TRUE_A:
    case Pyc::POP_JUMP_FORWARD_IF_FALSE_A: case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
    case Pyc::POP_JUMP_BACKWARD_IF_FALSE_A: case Pyc::POP_JUMP_BACKWARD_IF_TRUE_A:
    case Pyc::POP_JUMP_IF_NONE_A: case Pyc::POP_JUMP_IF_NOT_NONE_A:
    case Pyc::POP_BLOCK: case Pyc::POP_EXCEPT: case Pyc::END_FINALLY:
    case Pyc::NOP: case Pyc::END_FOR: case Pyc::POP_ITER: case Pyc::IMPORT_STAR:
    case Pyc::LIST_APPEND_A: case Pyc::SET_ADD_A: case Pyc::MAP_ADD_A:
    case Pyc::PRINT_ITEM: case Pyc::PRINT_NEWLINE:
        return true;
    default:
        return false;
    }
}

void ScanExprFlow(PycRef<PycCode> code, PycModule* mod, ExprFlow& out, const WhileLoops& loops)
{
    PycBuffer src(code->code()->value(), code->code()->length());
    std::vector<XIns> ins;
    int opcode, operand, pos = 0;
    while (!src.atEof()) {
        int p = pos;
        bc_next(src, mod, opcode, operand, pos);
        ins.push_back({p, opcode, operand, pos, pos});
    }
    for (int i = (int)ins.size() - 1; i >= 0; i--)
        if (i + 1 < (int)ins.size() && ins[i + 1].op == Pyc::CACHE && ins[i].op != Pyc::CACHE)
            ins[i].real = ins[i + 1].real;
    std::map<int, int> idxAt;
    for (int i = 0; i < (int)ins.size(); i++)
        idxAt[ins[i].pos] = i;
    const int unit = mod->verCompare(3, 10) >= 0 ? 2 : 1;
    const int n = (int)ins.size();

    auto prevReal = [&](int i) {
        i--;
        while (i >= 0 && (ins[i].op == Pyc::CACHE || ins[i].op == Pyc::NOT_TAKEN))
            i--;
        return i;
    };
    auto nextReal = [&](int i) {
        i++;
        while (i < n && (ins[i].op == Pyc::CACHE || ins[i].op == Pyc::NOT_TAKEN))
            i++;
        return i;
    };
    enum Cond { NOTCOND, IF_FALSE, IF_TRUE, IF_NONE, IF_NOT_NONE };
    /* conditional pop-jump: returns its kind and sets `target` */
    auto condJumpRaw = [&](const XIns& in, int& target) -> Cond {
        switch (in.op) {
        case Pyc::POP_JUMP_IF_FALSE_A:
        case Pyc::POP_JUMP_IF_TRUE_A:
            target = mod->verCompare(3, 12) >= 0 ? in.real + in.arg * 2 : in.arg * unit;
            return in.op == Pyc::POP_JUMP_IF_FALSE_A ? IF_FALSE : IF_TRUE;
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
            target = in.real + in.arg * 2;
            return in.op == Pyc::POP_JUMP_FORWARD_IF_FALSE_A ? IF_FALSE : IF_TRUE;
        case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
        case Pyc::POP_JUMP_IF_NONE_A:
            target = in.real + in.arg * 2;
            return IF_NONE;
        case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
        case Pyc::POP_JUMP_IF_NOT_NONE_A:
            target = in.real + in.arg * 2;
            return IF_NOT_NONE;
        default:
            return NOTCOND;
        }
    };
    /* unconditional forward jump target, or -1 */
    auto fwdJump = [&](const XIns& in) -> int {
        if (in.op == Pyc::JUMP_FORWARD_A)
            return in.real + in.arg * unit;
        if (in.op == Pyc::JUMP_ABSOLUTE_A && mod->verCompare(3, 11) < 0) {
            int t = in.arg * unit;
            return t > in.pos ? t : -1;
        }
        return -1;
    };

    /* chained-comparison cleanups were located by ScanLoops (ScanChainCleanups) */
    out.cleanupDest = loops.cleanupDest;
    out.chainSkip = loops.chainSkip;
    auto condJump = [&](const XIns& in, int& target) -> Cond {
        Cond c = condJumpRaw(in, target);
        if (c != NOTCOND) {
            auto it = out.cleanupDest.find(target);
            if (it != out.cleanupDest.end())
                target = it->second;
        }
        return c;
    };

    /* A chained comparison's false path: `SWAP 2; POP_TOP` (3.11+) or
       `ROT_TWO; POP_TOP` before. Returns the real merge point of the value
       context and records what to skip. */
    auto chainCleanup = [&](int t, int& join) -> bool {
        auto ti = idxAt.find(t);
        if (ti == idxAt.end())
            return false;
        int s = ti->second;
        bool isSwap = (mod->verCompare(3, 11) >= 0)
                ? (ins[s].op == Pyc::SWAP_A && ins[s].arg == 2)
                : (ins[s].op == Pyc::ROT_TWO);
        if (!isSwap)
            return false;
        int a = nextReal(s);
        if (a >= n || ins[a].op != Pyc::POP_TOP)
            return false;
        int before = prevReal(s);
        if (before < 0)
            return false;
        int endIdx = nextReal(a);
        int skipFrom = t;
        if (ins[before].op == Pyc::RETURN_VALUE) {
            /* the cleanup repeats the return: merge at the main path's return */
            if (endIdx >= n || ins[endIdx].op != Pyc::RETURN_VALUE)
                return false;
            join = ins[before].pos;
            endIdx = nextReal(endIdx);
        } else {
            int J = fwdJump(ins[before]);
            if (J <= t)
                return false;
            join = J;
            skipFrom = ins[before].pos;
            endIdx = nextReal(a);
        }
        int skipTo = endIdx < n ? ins[endIdx].pos : ins[n - 1].real;
        out.chainSkip.push_back(std::make_pair(skipFrom, skipTo));
        return true;
    };

    /* keep-style jumps (value-context `and`/`or`) */
    std::set<int> keepEnd;   /* instruction index where a keep-jump sequence ends */
    for (int i = 0; i < n; i++) {
        if (mod->verCompare(3, 12) < 0) {
            if (ins[i].op == Pyc::JUMP_IF_TRUE_OR_POP_A || ins[i].op == Pyc::JUMP_IF_FALSE_OR_POP_A) {
                keepEnd.insert(i);
                if (ins[i].op == Pyc::JUMP_IF_FALSE_OR_POP_A) {
                    int jt = mod->verCompare(3, 11) >= 0 ? ins[i].real + ins[i].arg * 2
                                                        : ins[i].arg * unit;
                    int join;
                    if (chainCleanup(jt, join))
                        out.chainJoin[ins[i].pos] = join;
                }
            }
        } else if (ins[i].op == Pyc::COPY_A && ins[i].arg == 1) {
            int j = nextReal(i);
            if (j < n && ins[j].op == Pyc::TO_BOOL)
                j = nextReal(j);
            int t = 0;
            if (j >= n)
                continue;
            Cond c = condJump(ins[j], t);
            if (c != IF_FALSE && c != IF_TRUE)
                continue;
            int k = nextReal(j);
            if (k >= n || ins[k].op != Pyc::POP_TOP)
                continue;
            /* `COPY 1; POP_JUMP; POP_TOP` is also the head of a chained
               comparison; there the false path ends in `SWAP 2; POP_TOP`. */
            int chainTarget = t;
            {
                int join;
                if (c == IF_FALSE && chainCleanup(t, join))
                    chainTarget = join;
                else {
                    auto ti = idxAt.find(t);
                    if (ti != idxAt.end() && ins[ti->second].op == Pyc::SWAP_A
                            && ins[ti->second].arg == 2) {
                        int a = nextReal(ti->second);
                        if (a < n && ins[a].op == Pyc::POP_TOP)
                            continue;
                    }
                }
            }
            ExprFlow::Keep kp;
            kp.isOr = (c == IF_TRUE);
            kp.target = chainTarget;
            kp.endPos = ins[k].real;
            kp.popPos = ins[k].pos;
            out.keepSeq[ins[i].pos] = kp;
            keepEnd.insert(k);
        }
    }

    for (int i = 0; i < n; i++) {
        int t = 0;
        Cond c = condJump(ins[i], t);
        if (c == NOTCOND || t <= ins[i].pos)
            continue;
        /* skip the jump that belongs to a keep sequence */
        bool inKeep = false;
        for (int back = 1; back <= 3 && i - back >= 0; back++) {
            auto kit = out.keepSeq.find(ins[i - back].pos);
            if (kit != out.keepSeq.end() && ins[i].real < kit->second.endPos + 1) { inKeep = true; break; }
        }
        if (inKeep)
            continue;
        auto ti = idxAt.find(t);
        if (ti == idxAt.end())
            continue;
        int before = prevReal(ti->second);
        if (before < 0)
            continue;
        {
            /* 3.14 builtin-call inlining guard: COPY 1; LOAD_COMMON_CONSTANT; IS_OP; POP_JUMP */
            int pj = prevReal(i);
            if (pj >= 0 && ins[pj].op == Pyc::IS_OP_A) {
                int lc = prevReal(pj);
                if (lc >= 0 && ins[lc].op == Pyc::LOAD_COMMON_CONSTANT_A)
                    continue;
            }
        }
        /* mixed and/or: the target directly follows a keep-style jump */
        if (keepEnd.count(before)) {
            out.boolJump[ins[i].pos] = std::make_pair(t, c == IF_TRUE || c == IF_NOT_NONE ? true : false);
            continue;
        }
        /* ternary */
        int U = fwdJump(ins[before]);
        if (U <= t)
            continue;
        /* jump threading: when this ternary is nested in the "then" branch of
           another, our exit jump goes straight to the outer exit, while the
           outer then-jump (to the same place) sits right after our else value. */
        for (int q = ti->second; q < n && ins[q].pos < U; q++) {
            if (fwdJump(ins[q]) == U && q + 1 < n) {
                /* it is the outer ternary's then-jump only if an earlier test
                   jumps to the instruction right behind it */
                bool outer = false;
                for (int o = 0; o < i && !outer; o++) {
                    int ot = 0;
                    if (condJump(ins[o], ot) != NOTCOND && ot == ins[q + 1].pos)
                        outer = true;
                }
                if (outer) {
                    U = ins[q].pos;
                    break;
                }
            }
        }
        int thenLast = prevReal(before);
        if (thenLast <= i || xIsStatementEnd(ins[thenLast].op))
            continue;
        auto ui = idxAt.find(U);
        if (ui == idxAt.end())
            continue;
        int elseLast = prevReal(ui->second);
        if (elseLast < 0 || elseLast < ti->second || xIsStatementEnd(ins[elseLast].op))
            continue;
        ExprFlow::Tern tn;
        tn.elseStart = t;
        tn.end = U;
        tn.thenJump = ins[before].pos;
        tn.cond = c;
        out.ternary[ins[i].pos] = tn;
        out.ternaryThenJump[ins[before].pos] = ins[i].pos;
    }

    /* ---- statement-level compound conditions ---------------------------------
       A run of conditional jumps with only expression code in between forms one
       boolean test (`if a and b or c:`). Model each jump as a decision node whose
       true/false edges lead to a later node or to the BODY / END terminals, then
       repeatedly fold adjacent nodes that share an exit (a and b: same "false"
       exit; a or b: same "true" exit) until one node remains. */
    {
        std::set<int> claimed;
        for (const auto& kv : out.boolJump) claimed.insert(kv.first);
        for (const auto& kv : loops.guardEnd) claimed.insert(kv.first);
        for (const auto& kv : loops.guardChain) claimed.insert(kv.first);
        for (int q : loops.guardChainMember) claimed.insert(q);
        for (int q : loops.tailChain) claimed.insert(q);
        for (int q : loops.backedges) claimed.insert(q);
        for (const auto& kv : loops.breakIf) claimed.insert(kv.first);
        for (const auto& kv : out.keepSeq) {
            /* the POP_JUMP inside a keep sequence */
            int ci = idxAt[kv.first];
            for (int d = 1; d <= 3 && ci + d < n; d++)
                claimed.insert(ins[ci + d].pos);
        }
        auto isPlainCond = [&](int i, int& t, bool& jumpOnTrue) {
            if (claimed.count(ins[i].pos))
                return false;
            Cond c = condJump(ins[i], t);
            if (c != IF_FALSE && c != IF_TRUE)
                return false;
            jumpOnTrue = (c == IF_TRUE);
            return t > ins[i].pos;
        };
        /* position right after the jump (and its caches / NOT_TAKEN) */
        /* chained-comparison trailers/cleanups are transparent to fallthrough */
        auto fallIdx = [&](int i) {
            int k = i + 1;
            for (;;) {
                while (k < n && (ins[k].op == Pyc::CACHE || ins[k].op == Pyc::NOT_TAKEN))
                    k++;
                bool moved = false;
                for (const auto& r : out.chainSkip)
                    if (k < n && ins[k].pos == r.first) {
                        while (k < n && ins[k].pos < r.second)
                            k++;
                        moved = true;
                        break;
                    }
                if (!moved)
                    break;
            }
            return k;
        };
        auto fallPos = [&](int i) {
            int k = fallIdx(i);
            return k < n ? ins[k].pos : ins[n - 1].next;
        };
        std::set<int> used;
        for (int i = 0; i < n; i++) {
            int t = 0;
            bool onTrue = false;
            if (used.count(i) || !isPlainCond(i, t, onTrue))
                continue;
            /* collect the run */
            std::vector<int> run;
            run.push_back(i);
            int cur = i;
            for (;;) {
                int k = fallIdx(cur);
                int jj = -1;
                for (; k < n; k++) {
                    int t2; bool o2;
                    if (isPlainCond(k, t2, o2)) { jj = k; break; }
                    if (xIsStatementEnd(ins[k].op) || condJump(ins[k], t2) != NOTCOND
                            || ins[k].op == Pyc::FOR_ITER_A || ins[k].op == Pyc::SETUP_FINALLY_A
                            || ins[k].op == Pyc::SETUP_WITH_A || ins[k].op == Pyc::BEFORE_WITH)
                        break;
                }
                if (jj < 0)
                    break;
                run.push_back(jj);
                cur = jj;
            }
            for (int r : run) used.insert(r);
            if (run.size() < 2)
                continue;
            for (int m = (int)run.size(); m >= 2; m--) {
                struct Nd { int id; int e[2]; int tree; };   /* e[1]=true edge, e[0]=false edge */
                const int TERM_T = -1, TERM_F = -2;
                int body = fallPos(run[m - 1]);
                int endT = 0; bool lastOn = false;
                isPlainCond(run[m - 1], endT, lastOn);
                if (endT <= body)
                    continue;
                std::vector<int> startPos(m + 1);
                for (int k = 1; k < m; k++)
                    startPos[k + 1] = fallPos(run[k - 1]);   /* start of node k+1 (1-based) */
                bool ok = true;
                std::vector<Nd> nodes;
                std::vector<ExprFlow::CondTree> trees;
                for (int k = 1; k <= m && ok; k++) {
                    int tk = 0; bool on = false;
                    isPlainCond(run[k - 1], tk, on);
                    auto dest = [&](int target) -> int {
                        if (target == body) return TERM_T;
                        if (target == endT) return TERM_F;
                        for (int q = k + 1; q <= m; q++)
                            if (startPos[q] == target) return q;
                        ok = false;
                        return 0;
                    };
                    int jumpDest = dest(tk);
                    int fallDest = (k < m) ? k + 1 : TERM_T;
                    Nd nd;
                    nd.id = k;
                    nd.e[on ? 1 : 0] = jumpDest;      /* jump taken on `on` outcome */
                    nd.e[on ? 0 : 1] = fallDest;
                    nd.tree = (int)trees.size();
                    ExprFlow::CondTree leaf; leaf.op = 0; leaf.leaf = ins[run[k - 1]].pos; leaf.l = leaf.r = -1;
                    trees.push_back(leaf);
                    nodes.push_back(nd);
                }
                if (!ok)
                    continue;
                /* fold */
                bool progress = true;
                while (nodes.size() > 1 && progress) {
                    progress = false;
                    for (size_t a = 0; a + 1 < nodes.size(); a++) {
                        Nd& A = nodes[a];
                        Nd& B = nodes[a + 1];
                        for (int lbl = 0; lbl < 2 && !progress; lbl++) {
                            int other = 1 - lbl;
                            /* A: edge `other` falls into B, edge `lbl` jumps to X */
                            if (A.e[other] != B.id || A.e[lbl] == B.id)
                                continue;
                            int X = A.e[lbl];
                            if (B.e[lbl] != X || B.e[other] == B.id)
                                continue;
                            /* B must only be reachable from A */
                            int refs = 0;
                            for (const auto& N : nodes)
                                for (int e = 0; e < 2; e++)
                                    if (N.e[e] == B.id) refs++;
                            if (refs != 1)
                                continue;
                            ExprFlow::CondTree comb;
                            comb.op = (lbl == 0) ? 1 : 2;   /* shared false exit: and; shared true exit: or */
                            comb.leaf = 0;
                            comb.l = A.tree; comb.r = B.tree;
                            trees.push_back(comb);
                            int newTree = (int)trees.size() - 1;
                            Nd merged;
                            merged.id = A.id;
                            merged.e[lbl] = X;
                            merged.e[other] = B.e[other];
                            merged.tree = newTree;
                            int bid = B.id;
                            nodes.erase(nodes.begin() + a + 1);
                            nodes[a] = merged;
                            /* edges that pointed at the absorbed node now never occur */
                            (void)bid;
                            progress = true;
                        }
                        if (progress)
                            break;
                    }
                }
                if (nodes.size() != 1)
                    continue;
                const Nd& R = nodes[0];
                bool negate;
                if (R.e[1] == TERM_T && R.e[0] == TERM_F) negate = false;
                else if (R.e[1] == TERM_F && R.e[0] == TERM_T) negate = true;
                else continue;
                ExprFlow::CondChain cc;
                for (int k = 0; k < m; k++)
                    cc.members.push_back(ins[run[k]].pos);
                cc.trees = trees;
                cc.root = R.tree;
                cc.negate = negate;
                out.condChains[ins[run[m - 1]].pos] = cc;
                for (int k = 0; k + 1 < m; k++)
                    out.condMember.insert(ins[run[k]].pos);
                break;
            }
        }
    }
}
