#include <climits>
#include <cstring>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include "ASTree.h"
#include "ASTMatch.h"
#include "FastStack.h"
#include "pyc_numeric.h"
#include "bytecode.h"

// This must be a triple quote (''' or """), to handle interpolated string literals containing the opposite quote style.
// E.g. f'''{"interpolated "123' literal"}'''    -> valid.
// E.g. f"""{"interpolated "123' literal"}"""    -> valid.
// E.g. f'{"interpolated "123' literal"}'        -> invalid, unescaped quotes in literal.
// E.g. f'{"interpolated \"123\' literal"}'      -> invalid, f-string expression does not allow backslash.
// NOTE: Nested f-strings not supported.
#define F_STRING_QUOTE "'''"

static void append_to_chain_store(const PycRef<ASTNode>& chainStore,
        PycRef<ASTNode> item, FastStack& stack, const PycRef<ASTBlock>& curblock);

/* Use this to determine if an error occurred (and therefore, if we should
 * avoid cleaning the output tree) */
static bool cleanBuild;

/* Use this to prevent printing return keywords and newlines in lambdas. */
static bool inLambda = false;

/* Set just before printing a lambda that sits directly in a call argument
   list, where it needs no enclosing parentheses. */
static bool lambdaBare = false;
static bool genexprBare = false;   /* print a sole generator argument without parentheses */

/* Use this to keep track of whether we need to print out any docstring and
 * the list of global variables that we are using (such as inside a function). */
static bool printDocstringAndGlobals = false;

/* Use this to keep track of whether we need to print a class or module docstring */
static bool printClassDocstring = true;

// shortcut for all top/pop calls
static PycRef<ASTNode> StackPopTop(FastStack& stack)
{
    const auto node(stack.top());
    stack.pop();
    return node;
}

/* compiler generates very, VERY similar byte code for if/else statement block and if-expression
 *  statement
 *      if a: b = 1
 *      else: b = 2
 *  expression:
 *      b = 1 if a else 2
 *  (see for instance https://stackoverflow.com/a/52202007)
 *  here, try to guess if just finished else statement is part of if-expression (ternary operator)
 *  if it is, remove statements from the block and put a ternary node on top of stack
 */
static void CheckIfExpr(FastStack& stack, PycRef<ASTBlock> curblock)
{
    if (stack.empty())
        return;
    if (curblock->nodes().size() < 2)
        return;
    auto rit = curblock->nodes().crbegin();
    // the last is "else" block, the one before should be "if" (could be "for", ...)
    if ((*rit)->type() != ASTNode::NODE_BLOCK ||
        (*rit).cast<ASTBlock>()->blktype() != ASTBlock::BLK_ELSE)
        return;
    ++rit;
    if ((*rit)->type() != ASTNode::NODE_BLOCK ||
        (*rit).cast<ASTBlock>()->blktype() != ASTBlock::BLK_IF)
        return;
    auto else_expr = StackPopTop(stack);
    curblock->removeLast();
    auto if_block = curblock->nodes().back();
    auto if_expr = StackPopTop(stack);
    curblock->removeLast();
    stack.push(new ASTTernary(std::move(if_block), std::move(if_expr), std::move(else_expr)));
}

PycRef<ASTNode> BuildFromCode(PycRef<PycCode> code, PycModule* mod);

/* Sentinel name representing the live exception pushed by PUSH_EXC_INFO
   (Python 3.11+). It is bound by `except ... as <var>` (a STORE) or discarded
   (a POP_TOP); it must never reach the output. */
static const char* const EXC_SENTINEL = "<exception value>";
static bool isExcSentinel(const PycRef<ASTNode>& node)
{
    return node != NULL && node.type() == ASTNode::NODE_NAME
            && node.cast<ASTName>()->name()->isEqual(EXC_SENTINEL);
}

static bool sameName(const PycRef<ASTNode>& a, const PycRef<ASTNode>& b)
{
    return a != NULL && b != NULL
            && a.type() == ASTNode::NODE_NAME && b.type() == ASTNode::NODE_NAME
            && a.cast<ASTName>()->name()->isEqual(b.cast<ASTName>()->name()->value());
}

/* Python 3.11 `except ... as <name>` binding plus its compiler cleanup. Returns
   true if the store was consumed (and should be suppressed). */
static bool handleExceptBinding(PycRef<ASTBlock>& curblock,
                                const PycRef<ASTNode>& value,
                                const PycRef<ASTNode>& name)
{
    if (curblock->blktype() != ASTBlock::BLK_EXCEPT)
        return false;
    PycRef<ASTCondBlock> exc = curblock.try_cast<ASTCondBlock>();
    if (exc == NULL)
        return false;
    if (isExcSentinel(value)) {            /* except <type> as <name>: */
        exc->setExceptVar(name);
        return true;
    }
    /* Compiler cleanup at the end of an `except ... as <name>` handler:
       `<name> = None; del <name>`. The store value may arrive either as a
       NULL placeholder or as an explicit None constant (LOAD_CONST None;
       STORE). Suppress both when the target is the bound exception var. */
    bool valueIsNone = (value == NULL)
            || (value != NULL && value.type() == ASTNode::NODE_OBJECT
                && value.cast<ASTObject>()->object() == Pyc_None);
    if (valueIsNone && exc->exceptVar() != NULL
            && sameName(name, exc->exceptVar()))
        return true;
    return false;
}

/* True if `del <name>` targets the bound exception variable (cleanup). */
static bool isExceptVarDelete(const PycRef<ASTBlock>& curblock,
                              const PycRef<ASTNode>& name)
{
    if (curblock->blktype() != ASTBlock::BLK_EXCEPT)
        return false;
    PycRef<ASTCondBlock> exc = curblock.try_cast<ASTCondBlock>();
    return exc != NULL && exc->exceptVar() != NULL
            && sameName(name, exc->exceptVar());
}

/* Search a decompiled comprehension/generator code body for the
   ASTComprehension node it produces (Python 3.x compiles comprehensions and
   generator expressions into separate code objects). */
static PycRef<ASTComprehension> FindComprehension(PycRef<ASTNode> node)
{
    if (node == NULL)
        return NULL;
    switch (node.type()) {
    case ASTNode::NODE_COMPREHENSION:
        return node.cast<ASTComprehension>();
    case ASTNode::NODE_RETURN:
        return FindComprehension(node.cast<ASTReturn>()->value());
    case ASTNode::NODE_STORE:
        return FindComprehension(node.cast<ASTStore>()->src());
    case ASTNode::NODE_NODELIST:
        for (const auto& n : node.cast<ASTNodeList>()->nodes()) {
            PycRef<ASTComprehension> c = FindComprehension(n);
            if (c != NULL)
                return c;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* MAP_ADD/SET_ADD/LIST_APPEND normally fire with curblock exactly the
   comprehension's BLK_FOR. When the comprehension has an `if` filter, the
   add instead executes from inside the filter's BLK_IF/BLK_ELIF, nested
   under that same FOR. Search up through any such filter blocks (stopping
   at the first block that isn't one) to find whether a comprehension FOR
   encloses the current position either way. */
static bool InsideComprehensionAdd(const std::stack<PycRef<ASTBlock> >& blocks)
{
    std::stack<PycRef<ASTBlock> > tmp = blocks;
    while (!tmp.empty()) {
        ASTBlock::BlkType bt = tmp.top()->blktype();
        if (bt == ASTBlock::BLK_FOR)
            return tmp.top().cast<ASTIterBlock>()->isComprehension();
        if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
            return false;
        tmp.pop();
    }
    return false;
}

/* A generator expression compiles to a code object that yields rather than
   building a NODE_COMPREHENSION, so reconstruct it from its for-loop: the FOR
   block becomes the comprehension generator, the yielded value the result, and
   a wrapping `if` its filter. The caller substitutes the implicit ".0" iterator
   with the real iterable. */
/* Walk a comprehension for-loop body for the yielded value, descending through
   (possibly nested) `if` filters and combining their conditions with `and`.
   Returns the yielded value and, via outCond, the combined filter (or NULL). */
static PycRef<ASTNode> findCompYield(const ASTBlock::list_t& nodes,
                                     PycRef<ASTNode> condSoFar,
                                     PycRef<ASTNode>& outCond)
{
    for (const auto& n : nodes) {
        if (n.type() == ASTNode::NODE_RETURN
                && n.cast<ASTReturn>()->rettype() == ASTReturn::YIELD) {
            outCond = condSoFar;
            return n.cast<ASTReturn>()->value();
        }
        if (n.type() == ASTNode::NODE_BLOCK
                && n.cast<ASTBlock>()->blktype() == ASTBlock::BLK_IF) {
            PycRef<ASTCondBlock> ifblk = n.cast<ASTCondBlock>();
            PycRef<ASTNode> c = ifblk->cond();
            if (ifblk->negative())
                c = new ASTUnary(c, ASTUnary::UN_NOT);
            PycRef<ASTNode> combined = (condSoFar == NULL) ? c
                    : new ASTBinary(condSoFar, c, ASTBinary::BIN_LOG_AND);
            PycRef<ASTNode> r = findCompYield(ifblk->nodes(), combined, outCond);
            if (r != NULL)
                return r;
        }
    }
    return NULL;
}

static PycRef<ASTComprehension> SynthGenexpr(PycRef<ASTNode> node)
{
    if (node == NULL)
        return NULL;
    if (node.type() == ASTNode::NODE_NODELIST) {
        for (const auto& n : node.cast<ASTNodeList>()->nodes()) {
            PycRef<ASTComprehension> c = SynthGenexpr(n);
            if (c != NULL)
                return c;
        }
        return NULL;
    }
    if (node.type() == ASTNode::NODE_BLOCK
            && node.cast<ASTBlock>()->blktype() == ASTBlock::BLK_FOR) {
        PycRef<ASTIterBlock> forblk = node.cast<ASTIterBlock>();
        PycRef<ASTNode> cond;
        PycRef<ASTNode> result = findCompYield(forblk->nodes(), NULL, cond);
        if (result == NULL)
            return NULL;
        if (cond != NULL)
            forblk->setCondition(cond);
        PycRef<ASTComprehension> comp = new ASTComprehension(result);
        comp->addGenerator(forblk);
        return comp;
    }
    return NULL;
}

/* Python 3.11 with-statement pre-pass. For each BEFORE_WITH whose normal exit
   has the canonical shape (body -> implicit __exit__ -> JUMP over the cleanup
   handler -> resume), record the with body end and the resume offset. The
   region [bodyEnd, resume) (implicit __exit__ call + jump + exception cleanup
   handler) is then skipped during decompilation. With-statements without this
   clean shape are left unhandled (no regression). */
static void ScanWithBlocks(PycRef<PycCode> code, PycModule* mod,
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
static void ScanTryFinally(PycRef<PycCode> code, PycModule* mod,
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
static void ScanWhileLoops(PycRef<PycCode> code, PycModule* mod,
                           std::map<int,int>& whileGuardEnd,
                           std::set<int>& whileBackedges)
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
    std::map<int,int> instrEndingAt;   /* nextpos -> pos */
    for (int p : order)
        instrEndingAt[nextAt[p]] = p;

    auto isCondBack = [](int op){
        return op == Pyc::POP_JUMP_BACKWARD_IF_TRUE_A
            || op == Pyc::POP_JUMP_BACKWARD_IF_FALSE_A
            || op == Pyc::POP_JUMP_BACKWARD_IF_NONE_A
            || op == Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A;
    };
    auto isFwdGuard = [](int op){
        return op == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
            || op == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
            || op == Pyc::POP_JUMP_FORWARD_IF_NONE_A
            || op == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A;
    };
    for (int b : order) {
        if (!isCondBack(opAt[b]))
            continue;
        int end = nextAt[b];
        int loop_start = end - operAt[b] * 2;   /* backward target */
        if (loop_start < 0 || loop_start >= b)
            continue;
        auto git = instrEndingAt.find(loop_start);
        if (git == instrEndingAt.end())
            continue;
        int guard = git->second;
        if (!isFwdGuard(opAt[guard]))
            continue;
        int guard_target = nextAt[guard] + operAt[guard] * 2;
        if (guard_target != end)            /* guard must skip to loop end */
            continue;
        whileGuardEnd[guard] = end;
        whileBackedges.insert(b);
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
static void ScanChainedCompare(PycRef<PycCode> code, PycModule* mod,
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
static void ScanComprehensionCleanup(PycRef<PycCode> code, PycModule* mod,
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

static void ScanTryElse(PycRef<PycCode> code, PycModule* mod,
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

PycRef<ASTNode> BuildFromCode(PycRef<PycCode> code, PycModule* mod)
{
    PycBuffer source(code->code()->value(), code->code()->length());

    FastStack stack((mod->majorVer() == 1) ? 20 : code->stackSize());
    stackhist_t stack_hist;

    std::stack<PycRef<ASTBlock> > blocks;
    PycRef<ASTBlock> defblock = new ASTBlock(ASTBlock::BLK_MAIN);
    defblock->init();
    PycRef<ASTBlock> curblock = defblock;
    blocks.push(defblock);

    int opcode, operand;
    int curpos = 0;
    int pos = 0;
    int unpack = 0;
    /* Python 3.12+ inlined comprehensions: number of loop variables saved by
       each open LOAD_FAST_AND_CLEAR prologue (innermost last). */
    std::vector<int> inlineComps;
    int prevOp1 = -1, prevOp2 = -1;   /* previous two opcodes (swap-store detection) */
    bool commonGuard = false;   /* 3.14 `f is any` guard of an inlined builtin call */
    const bool isCompCode = [&]() {
        const char* nm = code->name() != NULL ? code->name()->value() : NULL;
        return nm && (!strcmp(nm, "<listcomp>") || !strcmp(nm, "<setcomp>")
                || !strcmp(nm, "<dictcomp>") || !strcmp(nm, "<genexpr>"));
    }();
    bool inlineFinish = false;
    std::set<int> fusedKeepFirst;   /* STORE_FAST_STORE_FAST whose 2nd store is a restore */
    int inlineFinishBase = 0;
    std::vector<int> inlineBase;   /* stack size below the container */
    std::vector<int> inlineKind;   /* ASTComprehension::Kind per open comp */
    int inlineFinishKind = 0;
    /* Python 3.13 fused instructions (STORE_FAST_STORE_FAST, ...) are split
       into the simple instructions they stand for; they are executed from this
       queue before the next real instruction is decoded. */
    std::deque<std::pair<int, int> > pendingOps;
    /* Node pushed by the most recent LOAD_ASSERTION_ERROR (3.9+). */
    PycRef<ASTNode> assertErrNode;
    bool else_pop = false;
    bool need_try = false;
    bool variable_annotations = false;
    /* Tracks whether GET_ITER (optionally followed only by no-op PRECALL/
       CACHE/RESUME opcodes) was the last meaningful thing executed, so
       CALL_A can tell a genuine comprehension-invoking call (`dictcomp(iter(
       x))`, always preceded by GET_ITER) apart from an unrelated call that
       merely happens to have the comprehension's function object sitting a
       couple of stack slots below its own callee (e.g. the `im.items()`
       call that executes while a dict comprehension's MAKE_FUNCTION result
       is still sitting further down the stack, waiting to be invoked). */
    bool sawGetIter = false;
    /* Python 3.11 short-circuit boolean expressions (`a or b`, `a and b`).
       CPython 3.11 only emits JUMP_IF_TRUE_OR_POP / JUMP_IF_FALSE_OR_POP from
       compiler_boolop, i.e. for value-producing boolean expressions (statement
       conditions use POP_JUMP_*), and their oparg is a *relative* forward
       delta in 3.11 (absolute before, removed in 3.12). Treat them purely as
       expressions: remember the left operand and the merge offset, and when
       decoding reaches that offset combine it with the right operand left on
       the stack. */
    struct PendingBoolOp {
        int target;
        PycRef<ASTNode> left;
        ASTBinary::BinOp op;
    };
    std::vector<PendingBoolOp> pendingBool;
    std::vector<PycExceptionTableEntry> exception_entries;
    std::vector<std::pair<int, int> > cleanupSkip;   /* handler ranges to ignore */
    size_t next_exception_entry = 0;

    /* Python 3.11 with-statement reconstruction state. */
    std::map<int, int> withBodyEndByBefore;   /* BEFORE_WITH pos -> body end */
    std::map<int, int> withResumeByBodyEnd;   /* body end -> resume offset */
    int with_skip_until = 0;                   /* skip cleanup region < this */

    /* Python 3.11 try/finally reconstruction state. */
    std::map<int, int> finallyTryEndByStart;   /* entry start -> try body end */
    std::map<int, int> finallyEndByStart;      /* entry start -> finally end */
    std::map<int, int> finallyResumeByEnd;     /* finally end -> resume offset */
    std::set<int> finallyReturnStarts;         /* try/finally whose try returns */
    std::set<int> finallySuppressReturns;      /* RETURN_VALUEs of such a try */

    /* Python 3.11 while-loop reconstruction state. */
    std::map<int, int> whileGuardEnd;          /* guard pos -> loop end */
    std::set<int> whileBackedges;              /* back-edge positions */

    /* Python 3.11 try/except/else state (see ScanTryElse). */
    std::map<int, std::pair<int, int> > elseRegionByStart;
    std::vector<std::pair<int, PycRef<ASTBlock> > > openElse;   /* jump pos, block */

    /* Python 3.11 chained-comparison reconstruction state. */
    std::map<int, int> chainSkipJumps;         /* trailer JUMP_FORWARD -> body */
    std::map<int, int> trampolineExit;         /* trampoline POP_TOP -> exit */

    if (mod->verCompare(3, 11) >= 0) {
        exception_entries = code->exceptionTableEntries();
        if (mod->verCompare(3, 12) >= 0)
            ScanComprehensionCleanup(code, mod, exception_entries, cleanupSkip);
        ScanWithBlocks(code, mod, exception_entries,
                       withBodyEndByBefore, withResumeByBodyEnd);
        ScanTryFinally(code, mod, exception_entries,
                       finallyTryEndByStart, finallyEndByStart, finallyResumeByEnd,
                       finallyReturnStarts, finallySuppressReturns);
        ScanWhileLoops(code, mod, whileGuardEnd, whileBackedges);
        ScanChainedCompare(code, mod, chainSkipJumps, trampolineExit);
        ScanTryElse(code, mod, exception_entries, elseRegionByStart);
    }

    /* Python 3.11 nested-try prescan. The exception table only records the
       innermost handler for each instruction, so in
           try:            # outer -> handler H_out
               try: A      # inner -> handler H_in
               except: B
               C
           except: D
       the outer try's first entry starts *after* A (at the inner try's
       trailing jump), not where the outer try really begins. Recover the
       true start: if another handler's protected range begins earlier and
       that handler itself lies inside this try's span, the inner try is
       nested in this one, so this try starts where the inner one does.
       Map true start -> handlers to open there (outermost first). */
    std::map<int, std::vector<int> > shadowedTryStarts;
    if (!exception_entries.empty()) {
        std::map<int, std::pair<int, int> > span;   /* target -> [start,end) */
        std::set<int> finallyTargets;
        for (const auto& e : exception_entries) {
            if (e.push_lasti)
                continue;
            if (finallyTryEndByStart.count(e.start_offset))
                finallyTargets.insert(e.target);
            auto it = span.find(e.target);
            if (it == span.end()) {
                span[e.target] = std::make_pair(e.start_offset, e.end_offset);
            } else {
                it->second.first = std::min(it->second.first, e.start_offset);
                it->second.second = std::max(it->second.second, e.end_offset);
            }
        }
        std::vector<std::pair<int, int> > found;    /* (eff start, target) */
        for (const auto& kv : span) {
            int T = kv.first;
            if (finallyTargets.count(T))
                continue;
            int s0 = kv.second.first, e0 = kv.second.second;
            int eff = s0;
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& kv2 : span) {
                    int T2 = kv2.first;
                    if (T2 == T)
                        continue;
                    if (kv2.second.first < eff && T2 >= s0 && T2 < e0
                            && kv2.second.second <= s0) {
                        eff = kv2.second.first;
                        changed = true;
                    }
                }
            }
            if (eff < s0)
                found.push_back(std::make_pair(eff, T));
        }
        /* outermost (latest handler / widest span) first */
        std::sort(found.begin(), found.end(),
                  [&span](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                      if (a.first != b.first)
                          return a.first < b.first;
                      return span[a.second].second > span[b.second].second;
                  });
        for (const auto& f : found)
            shadowedTryStarts[f.first].push_back(f.second);
    }

    /* Structural pattern matching (Python 3.10+): the patterns of the cases are
     * decoded statically (see ASTMatch.cpp), the case bodies are decompiled
     * by the regular machinery inside ASTCaseBlock blocks. */
    struct MatchCtx {
        MatchCtx(PycRef<ASTMatchBlock> m, const FastStack& s)
            : mblock(std::move(m)), baseStack(s) { }

        PycRef<ASTMatchBlock> mblock;
        FastStack baseStack;            // the stack without the subject
        PycRef<ASTCaseBlock> cblock;    // the case being decompiled
        size_t cdepth = 0;              // blocks.size() while cblock is the innermost block
        MatchCase cur;
        bool isDefault = false;         // the trailing `case _:`
        bool skipPop = false;           // the next POP_TOP discards the subject
        bool guardOpen = false;         // still before the body: jumps belong to the guard
        int guardTarget = -1;
        int endJumpPos = -1;            // jump closing the case body
        int endPos = -1;                // end of the match statement (if known)
    };
    std::vector<MatchCtx> matches;
    std::unique_ptr<MatchAnalyzer> matcher;

    auto closeNested = [&](MatchCtx& mc, int limit) {
        while (blocks.size() > mc.cdepth) {
            PycRef<ASTBlock> top = blocks.top();
            if ((top->blktype() != ASTBlock::BLK_IF
                    && top->blktype() != ASTBlock::BLK_ELIF
                    && top->blktype() != ASTBlock::BLK_ELSE)
                    || top->end() > limit)
                break;
            if (!stack_hist.empty())
                stack_hist.pop();
            blocks.pop();
            blocks.top()->append(top.cast<ASTNode>());
            curblock = blocks.top();
        }
    };

    auto beginCase = [&](MatchCtx& mc, const MatchCase& nc, bool isDefault) {
        mc.cur = nc;
        mc.isDefault = isDefault;
        PycRef<ASTCaseBlock> cb = new ASTCaseBlock(nc.pattern, nc.bodyEnd > 0 ? nc.bodyEnd : 0);
        blocks.push(cb.cast<ASTBlock>());
        curblock = blocks.top();
        mc.cblock = cb;
        mc.cdepth = blocks.size();
        stack = mc.baseStack;
        mc.skipPop = nc.nonLast;
        mc.guardOpen = true;
        mc.guardTarget = -1;
        mc.endJumpPos = (nc.bodyEnd >= 2) ? nc.bodyEnd - 2 : -1;
        pos = nc.bodyStart;
        source.setPos(pos);
        else_pop = false;
    };

    auto finishMatch = [&](int next) {
        MatchCtx& mc = matches.back();
        while (blocks.size() > 1 && blocks.top() != mc.mblock.cast<ASTBlock>()) {
            PycRef<ASTBlock> top = blocks.top();
            blocks.pop();
            blocks.top()->append(top.cast<ASTNode>());
        }
        PycRef<ASTBlock> mb = blocks.top();
        blocks.pop();
        curblock = blocks.top();
        curblock->append(mb.cast<ASTNode>());
        stack = mc.baseStack;
        matches.pop_back();
        pos = next;
        source.setPos(pos);
        else_pop = (curblock->blktype() == ASTBlock::BLK_ELSE
                    || curblock->blktype() == ASTBlock::BLK_IF
                    || curblock->blktype() == ASTBlock::BLK_ELIF)
                   && curblock->end() == pos;
    };

    auto endCase = [&](MatchCtx& mc) {
        closeNested(mc, pos);
        while (blocks.size() > mc.cdepth) {
            PycRef<ASTBlock> top = blocks.top();
            blocks.pop();
            blocks.top()->append(top.cast<ASTNode>());
        }
        PycRef<ASTCaseBlock> cb = mc.cblock;
        blocks.pop();
        curblock = blocks.top();
        curblock->append(cb.cast<ASTNode>());
        mc.cblock = nullptr;
        stack = mc.baseStack;

        const bool wasNonLast = mc.cur.nonLast;
        const bool wasDefault = mc.isDefault;
        int next = (mc.cur.bodyEnd >= 0 && mc.cur.nextPos >= 0) ? mc.cur.nextPos : pos;
        if (wasNonLast) {
            MatchCase nc;
            if (matcher->parseCase(next, nc)) {
                beginCase(mc, nc, false);
            } else {
                fprintf(stderr, "Warning: unable to decode a case of a match statement at %d\n", next);
                finishMatch(next);
            }
        } else if (!wasDefault) {
            int ni = matcher->indexOf(next);
            if (ni >= 0 && matcher->instrs()[ni].op == Pyc::NOP) {
                MatchCase dc;
                dc.pattern = new ASTMatchPattern(ASTMatchPattern::PAT_WILDCARD);
                dc.bodyStart = matcher->instrs()[ni].end;
                beginCase(mc, dc, true);
            } else {
                finishMatch(next);
            }
        } else {
            finishMatch(next);
        }
    };

    while (!source.atEof()) {
#if defined(BLOCK_DEBUG) || defined(STACK_DEBUG)
        fprintf(stderr, "%-7d", pos);
    #ifdef STACK_DEBUG
        fprintf(stderr, "%-5d", (unsigned int)stack_hist.size() + 1);
    #endif
    #ifdef BLOCK_DEBUG
        for (unsigned int i = 0; i < blocks.size(); i++)
            fprintf(stderr, "    ");
        fprintf(stderr, "%s (%d)", curblock->type_str(), curblock->end());
    #endif
        fprintf(stderr, "\n");
#endif

        /* Python 3.11 with-statement / try-finally: close the body at its end
           and enter the cleanup skip region BEFORE processing exception-table
           entries. An enclosing try re-protects the implicit __exit__/finally
           cleanup region, which would otherwise reopen a spurious try over it. */
        if (curblock->blktype() == ASTBlock::BLK_WITH
                && curblock->end() != 0
                && curblock->end() <= pos
                && blocks.size() > 1) {
            PycRef<ASTBlock> with = curblock;
            blocks.pop();
            curblock = blocks.top();
            curblock->append(with.cast<ASTNode>());
            auto rit = withResumeByBodyEnd.find(with->end());
            if (rit != withResumeByBodyEnd.end())
                with_skip_until = rit->second;
        }
        /* Everything nested inside a finally body ends with it. */
        if (curblock->blktype() != ASTBlock::BLK_FINALLY) {
            bool finEnds = false;
            {
                std::stack<PycRef<ASTBlock> > tmp = blocks;
                while (!tmp.empty()) {
                    PycRef<ASTBlock> b = tmp.top();
                    if (b->blktype() == ASTBlock::BLK_FINALLY) {
                        finEnds = b->end() != 0 && b->end() <= pos;
                        break;
                    }
                    if (b->blktype() == ASTBlock::BLK_MAIN)
                        break;
                    tmp.pop();
                }
            }
            while (finEnds && curblock->blktype() != ASTBlock::BLK_FINALLY
                    && blocks.size() > 1) {
                PycRef<ASTBlock> inner = curblock;
                if (inner->blktype() == ASTBlock::BLK_IF
                        || inner->blktype() == ASTBlock::BLK_ELIF
                        || inner->blktype() == ASTBlock::BLK_ELSE
                        || inner->blktype() == ASTBlock::BLK_EXCEPT) {
                    if (!stack_hist.empty())
                        stack_hist.pop();
                }
                blocks.pop();
                curblock = blocks.top();
                if (inner->blktype() != ASTBlock::BLK_EXCEPT || inner->size() != 0
                        || inner.cast<ASTCondBlock>()->cond() != NULL)
                    curblock->append(inner.cast<ASTNode>());
            }
        }
        if (curblock->blktype() == ASTBlock::BLK_FINALLY
                && curblock->end() != 0
                && curblock->end() <= pos
                && blocks.size() > 1) {
            int finEnd = curblock->end();
            PycRef<ASTBlock> final = curblock;
            blocks.pop();
            curblock = blocks.top();
            curblock->append(final.cast<ASTNode>());
            if (curblock->blktype() == ASTBlock::BLK_CONTAINER && blocks.size() > 1) {
                PycRef<ASTBlock> cont = curblock;
                blocks.pop();
                curblock = blocks.top();
                curblock->append(cont.cast<ASTNode>());
            }
            auto rit = finallyResumeByEnd.find(finEnd);
            if (rit != finallyResumeByEnd.end())
                with_skip_until = rit->second;
        }
        if (with_skip_until > 0) {
            if (pos < with_skip_until) {
                curpos = pos;
                bc_next(source, mod, opcode, operand, pos);
                continue;
            }
            with_skip_until = 0;
        }

        /* Open outer trys whose real start is shadowed by a nested inner try
           (see the nested-try prescan above), before the inner one opens. */
        {
            auto sit = shadowedTryStarts.find(pos);
            if (sit != shadowedTryStarts.end()) {
                for (int T : sit->second) {
                    bool open = false;
                    std::stack<PycRef<ASTBlock> > tmp = blocks;
                    while (!tmp.empty()) {
                        if (tmp.top()->blktype() == ASTBlock::BLK_TRY
                                && tmp.top()->end() == T) {
                            open = true;
                            break;
                        }
                        tmp.pop();
                    }
                    if (open)
                        continue;
                    PycRef<ASTBlock> cont = new ASTContainerBlock(0, T);
                    blocks.push(cont);
                    stack_hist.push(stack);
                    PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, T, true);
                    blocks.push(tryblock);
                    curblock = blocks.top();
                }
            }
        }

        /* A try body that ends inside nested blocks without a jump (e.g. its
           last statement is `if c: return x`) leaves those blocks open when
           the handler begins. Reaching an open try's handler offset ends
           everything nested inside that try, so fold them into their parents
           first and let the try-closing logic below see the BLK_TRY. */
        if (curblock->blktype() != ASTBlock::BLK_TRY) {
            bool tryEndsHere = false;
            {
                std::stack<PycRef<ASTBlock> > tmp = blocks;
                while (!tmp.empty()) {
                    PycRef<ASTBlock> b = tmp.top();
                    if (b->blktype() == ASTBlock::BLK_TRY) {
                        tryEndsHere = (b->end() == pos);
                        break;
                    }
                    /* only look through ordinary nested blocks */
                    if (b->blktype() != ASTBlock::BLK_IF
                            && b->blktype() != ASTBlock::BLK_ELIF
                            && b->blktype() != ASTBlock::BLK_ELSE
                            && b->blktype() != ASTBlock::BLK_FOR
                            && b->blktype() != ASTBlock::BLK_WHILE
                            && b->blktype() != ASTBlock::BLK_WITH)
                        break;
                    tmp.pop();
                }
            }
            if (tryEndsHere) {
                while (curblock->blktype() != ASTBlock::BLK_TRY
                        && blocks.size() > 1) {
                    PycRef<ASTBlock> inner = curblock;
                    if (inner->blktype() == ASTBlock::BLK_IF
                            || inner->blktype() == ASTBlock::BLK_ELIF
                            || inner->blktype() == ASTBlock::BLK_ELSE) {
                        if (!stack_hist.empty())
                            stack_hist.pop();
                    }
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(inner.cast<ASTNode>());
                }
            }
        }

        if (!matches.empty() && matches.back().cblock
                && blocks.size() >= matches.back().cdepth) {
            MatchCtx& mc = matches.back();
            if (mc.endJumpPos >= 0 && pos == mc.endJumpPos) {
                int ji = matcher->indexOf(pos);
                /* The jump ends the case body: conditionals still open in it
                 * (whose own jump was folded into this one) end here */
                bool endsCase = ji >= 0 && matcher->instrs()[ji].op == Pyc::JUMP_FORWARD_A
                        && matcher->jumpTarget(matcher->instrs()[ji]) >= mc.cur.bodyEnd;
                closeNested(mc, endsCase ? INT_MAX : pos);
                if (blocks.size() == mc.cdepth && ji >= 0
                        && matcher->instrs()[ji].op == Pyc::JUMP_FORWARD_A) {
                    int target = matcher->jumpTarget(matcher->instrs()[ji]);
                    if (target >= mc.cur.bodyEnd) {
                        /* The jump over the remaining cases belongs to the match */
                        if (mc.endPos < 0)
                            mc.endPos = target;
                        pos = matcher->instrs()[ji].end;
                        source.setPos(pos);
                        continue;
                    }
                }
            }
            if ((mc.cur.bodyEnd >= 0 && pos >= mc.cur.bodyEnd)
                    || (mc.cur.bodyEnd < 0 && mc.endPos >= 0 && pos >= mc.endPos)) {
                endCase(mc);
                continue;
            }
        }

        while (next_exception_entry < exception_entries.size()
                && exception_entries[next_exception_entry].start_offset < pos) {
            next_exception_entry++;
        }

        if (next_exception_entry < exception_entries.size()) {
            const auto& entry = exception_entries[next_exception_entry];
            if (entry.start_offset == pos
                    && !entry.push_lasti) {
                auto fit = finallyTryEndByStart.find(entry.start_offset);
                if (fit != finallyTryEndByStart.end()) {
                    /* Python 3.11 try/finally: container carries the finally end
                       offset; the try body ends before the normal finally copy. */
                    int finEnd = finallyEndByStart[entry.start_offset];
                    PycRef<ASTBlock> cont = new ASTContainerBlock(finEnd, 0);
                    blocks.push(cont.cast<ASTBlock>());
                    curblock = blocks.top();

                    stack_hist.push(stack);
                    PycRef<ASTBlock> tryblock =
                            new ASTBlock(ASTBlock::BLK_TRY, fit->second, true);
                    blocks.push(tryblock.cast<ASTBlock>());
                    curblock = blocks.top();
                    next_exception_entry++;
                } else {
                    /* Python 3.11 splits a single try into several exception
                       table entries (all pointing at the same handler) when the
                       try body contains a nested try/except: the protected range
                       is broken around the inner handler. If a BLK_TRY with this
                       same handler target is already open, these later entries
                       are continuations of it, not new trys; skip them to avoid
                       a cascade of spurious nested trys. */
                    bool sameTryOpen = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmp = blocks;
                        while (!tmp.empty()) {
                            PycRef<ASTBlock> b = tmp.top();
                            if (b->blktype() == ASTBlock::BLK_TRY
                                    && b->end() == entry.target) {
                                sameTryOpen = true;
                                break;
                            }
                            tmp.pop();
                        }
                    }
                    if (sameTryOpen) {
                        next_exception_entry++;
                    } else {
                        if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                            curblock.cast<ASTContainerBlock>()->setExcept(entry.target);
                        } else {
                            PycRef<ASTBlock> next = new ASTContainerBlock(0, entry.target);
                            blocks.push(next.cast<ASTBlock>());
                            curblock = blocks.top();
                        }

                        stack_hist.push(stack);
                        PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, entry.target, true);
                        blocks.push(tryblock.cast<ASTBlock>());
                        curblock = blocks.top();
                        next_exception_entry++;
                    }
                }
            }
        }

        if (curblock->blktype() == ASTBlock::BLK_TRY
                && curblock->end() == pos
                && blocks.size() > 1) {
            PycRef<ASTBlock> prev = curblock;
            blocks.pop();
            curblock = blocks.top();

            if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                    && curblock.cast<ASTContainerBlock>()->hasExcept()) {
                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }

                curblock->append(prev.cast<ASTNode>());
                stack_hist.push(stack);

                PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, 0, NULL, false);
                except->init();
                blocks.push(except);
                curblock = blocks.top();
            } else if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                    && curblock.cast<ASTContainerBlock>()->hasFinally()) {
                /* Python 3.11 try/finally: the try body is followed by the
                   finally body (normal copy). */
                int finEnd = curblock.cast<ASTContainerBlock>()->finally();
                if (!stack_hist.empty()) {
                    /* `try: return X` + finally: X is still on the stack
                       (returned after the finally runs); emit the return in
                       the try body, where it belongs. */
                    if (finallySuppressReturns.size()
                            && finallyResumeByEnd.count(finEnd)
                            && stack.size() > stack_hist.top().size()
                            && stack.top() != NULL) {
                        bool isRetStyle = false;
                        for (int st : finallyReturnStarts)
                            if (finallyEndByStart[st] == finEnd)
                                isRetStyle = true;
                        if (isRetStyle)
                            prev->append(new ASTReturn(stack.top()));
                    }
                    stack = stack_hist.top();
                    stack_hist.pop();
                }

                curblock->append(prev.cast<ASTNode>());

                PycRef<ASTBlock> final = new ASTBlock(ASTBlock::BLK_FINALLY, finEnd, true);
                final->init();
                blocks.push(final);
                curblock = blocks.top();
            } else {
                blocks.push(prev);
                curblock = prev;
            }
        }

        if (curblock->blktype() == ASTBlock::BLK_EXCEPT
                && curblock->end() == pos
                && blocks.size() > 1) {
            PycRef<ASTBlock> prev = curblock;
            blocks.pop();
            curblock = blocks.top();

            if (!stack_hist.empty()) {
                stack = stack_hist.top();
                stack_hist.pop();
            }

            if (prev->size() != 0
                    || (prev->blktype() == ASTBlock::BLK_EXCEPT
                        && prev.cast<ASTCondBlock>()->cond() != NULL)) {
                curblock->append(prev.cast<ASTNode>());
            }

            if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                    && !curblock.cast<ASTContainerBlock>()->hasFinally()) {
                PycRef<ASTBlock> cont = curblock;
                blocks.pop();
                curblock = blocks.top();
                curblock->append(cont.cast<ASTNode>());
            }
        }

        /* try/except/else (3.11): the code right after the protected range
           and before the handler-skipping jump is the else body. */
        {
            auto er = elseRegionByStart.find(pos);
            if (er != elseRegionByStart.end() && curblock->blktype() == ASTBlock::BLK_TRY
                    && curblock->end() == er->second.second && blocks.size() > 1) {
                PycRef<ASTBlock> tryb = blocks.top();
                blocks.pop();
                bool isContainer = blocks.top()->blktype() == ASTBlock::BLK_CONTAINER
                        && blocks.top().cast<ASTContainerBlock>()->hasExcept();
                blocks.push(tryb);
                if (isContainer) {
                    PycRef<ASTBlock> eb = new ASTBlock(ASTBlock::BLK_ELSE, 0, true);
                    blocks.push(eb);
                    curblock = eb;
                    openElse.push_back(std::make_pair(er->second.first, eb));
                }
            }
            if (!openElse.empty() && pos == openElse.back().first
                    && curblock == openElse.back().second) {
                PycRef<ASTBlock> eb = curblock;
                blocks.pop();
                PycRef<ASTBlock> tryb = blocks.top();
                blocks.pop();
                PycRef<ASTBlock> cont = blocks.top();
                if (eb->size() != 0)
                    cont.cast<ASTContainerBlock>()->setElseBlock(eb);
                blocks.push(tryb);
                curblock = tryb;
                openElse.pop_back();
                curpos = pos;
                bc_next(source, mod, opcode, operand, pos);   /* the JUMP_FORWARD */
                continue;
            }
        }

        curpos = pos;
        if (!pendingOps.empty()) {
            opcode = pendingOps.front().first;
            operand = pendingOps.front().second;
            pendingOps.pop_front();
        } else {
            bc_next(source, mod, opcode, operand, pos);
        }

        if (!cleanupSkip.empty()) {
            bool skipIt = false;
            for (const auto& r : cleanupSkip)
                if (curpos >= r.first && curpos < r.second)
                    skipIt = true;
            if (skipIt)
                continue;
        }

        bool hadGetIter = sawGetIter;
        if (opcode != Pyc::GET_ITER && opcode != Pyc::GET_YIELD_FROM_ITER
                && opcode != Pyc::PRECALL_A && opcode != Pyc::CACHE
                && opcode != Pyc::RESUME_A && opcode != Pyc::INSTRUMENTED_RESUME_A) {
            sawGetIter = false;
        }

        if (need_try && opcode != Pyc::SETUP_EXCEPT_A) {
            need_try = false;

            /* Store the current stack for the except/finally statement(s) */
            stack_hist.push(stack);
            PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, curblock->end(), true);
            blocks.push(tryblock);
            curblock = blocks.top();
        } else if (else_pop
                && opcode != Pyc::JUMP_FORWARD_A
                && opcode != Pyc::JUMP_IF_FALSE_A
                && opcode != Pyc::JUMP_IF_FALSE_OR_POP_A
                && opcode != Pyc::POP_JUMP_IF_FALSE_A
                && opcode != Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                && opcode != Pyc::JUMP_IF_TRUE_A
                && opcode != Pyc::JUMP_IF_TRUE_OR_POP_A
                && opcode != Pyc::POP_JUMP_IF_TRUE_A
                && opcode != Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                && opcode != Pyc::POP_BLOCK) {
            else_pop = false;

            PycRef<ASTBlock> prev = curblock;
            while (prev->end() < pos
                    && prev->blktype() != ASTBlock::BLK_MAIN) {
                if (prev->blktype() != ASTBlock::BLK_CONTAINER) {
                    if (prev->end() == 0) {
                        break;
                    }

                    /* We want to keep the stack the same, but we need to pop
                     * a level off the history. */
                    //stack = stack_hist.top();
                    if (!stack_hist.empty())
                        stack_hist.pop();
                }
                blocks.pop();

                if (blocks.empty())
                    break;

                curblock = blocks.top();
                curblock->append(prev.cast<ASTNode>());

                prev = curblock;

                CheckIfExpr(stack, curblock);
            }
        }

        /* Python 3.11 boolean expressions: merge any pending `left or/and`
           whose merge point is this instruction (after closing blocks that
           end here, so e.g. a ternary right operand is already folded). */
        while (!pendingBool.empty() && pendingBool.back().target <= curpos
                && !stack.empty()) {
            PendingBoolOp pb = pendingBool.back();
            pendingBool.pop_back();
            PycRef<ASTNode> right = stack.top();
            stack.pop();
            stack.push(new ASTBinary(pb.left, right, pb.op));
        }

        if (mod->verCompare(3, 11) == 0
                && (opcode == Pyc::JUMP_IF_TRUE_OR_POP_A
                    || opcode == Pyc::JUMP_IF_FALSE_OR_POP_A)
                && !stack.empty()) {
            PendingBoolOp pb;
            pb.target = pos + operand * (int)sizeof(uint16_t);
            pb.left = stack.top();
            pb.op = (opcode == Pyc::JUMP_IF_TRUE_OR_POP_A)
                    ? ASTBinary::BIN_LOG_OR : ASTBinary::BIN_LOG_AND;
            stack.pop();
            pendingBool.push_back(pb);
            else_pop = ((curblock->blktype() == ASTBlock::BLK_ELSE)
                        || (curblock->blktype() == ASTBlock::BLK_IF)
                        || (curblock->blktype() == ASTBlock::BLK_ELIF))
                    && (curblock->end() == pos);
            continue;
        }

        if (mod->verCompare(3, 8) >= 0 && mod->verCompare(3, 11) < 0
                && (opcode == Pyc::POP_JUMP_IF_FALSE_A || opcode == Pyc::POP_JUMP_IF_TRUE_A)) {
            /* Before 3.11 the compiler threads `if c: body` at the tail of a loop
               so the test jumps straight back to the loop header; that is the
               3.11 POP_JUMP_BACKWARD_IF_* shape. */
            int tgt = operand * (mod->verCompare(3, 10) >= 0 ? (int)sizeof(uint16_t) : 1);
            bool tailOfLoop = false;
            if (tgt < curpos) {
                std::stack<PycRef<ASTBlock> > tmp = blocks;
                while (!tmp.empty()) {
                    ASTBlock::BlkType bt = tmp.top()->blktype();
                    if (bt == ASTBlock::BLK_FOR) {
                        tailOfLoop = !tmp.top().cast<ASTIterBlock>()->isComprehension()
                                && tgt == tmp.top().cast<ASTIterBlock>()->start();
                        break;
                    }
                    if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
                        break;
                    tmp.pop();
                }
            }
            if (tailOfLoop) {
                opcode = (opcode == Pyc::POP_JUMP_IF_TRUE_A)
                        ? Pyc::POP_JUMP_BACKWARD_IF_TRUE_A
                        : Pyc::POP_JUMP_BACKWARD_IF_FALSE_A;
            }
        }

        {
            auto isSimpleLoad = [](int op) {
                return op == Pyc::LOAD_FAST_A || op == Pyc::LOAD_CONST_A
                    || op == Pyc::LOAD_NAME_A || op == Pyc::LOAD_GLOBAL_A
                    || op == Pyc::LOAD_DEREF_A || op == Pyc::LOAD_FAST_BORROW_A
                    || op == Pyc::LOAD_SMALL_INT_A;
            };
            auto isSimpleStore = [](int op) {
                return op == Pyc::STORE_FAST_A || op == Pyc::STORE_NAME_A
                    || op == Pyc::STORE_GLOBAL_A || op == Pyc::STORE_DEREF_A;
            };
            if (!unpack && mod->verCompare(3, 8) >= 0 && isSimpleStore(opcode)
                    && ((isSimpleLoad(prevOp1) && isSimpleLoad(prevOp2))
                        || prevOp1 == Pyc::LOAD_FAST_LOAD_FAST_A
                        || prevOp1 == Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A)
                    && stack.size() >= 2 && (!source.atEof() || !pendingOps.empty())
                    && stack.top() != NULL && stack.top(1) != NULL
                    && stack.top().type() != ASTNode::NODE_CHAINSTORE
                    && stack.top(1).type() != ASTNode::NODE_CHAINSTORE) {
                int sSrc = source.pos(), sPos = pos;
                int o2 = 0, a2 = 0;
                const bool fromPending = !pendingOps.empty();
                if (fromPending) {
                    o2 = pendingOps.front().first;
                    a2 = pendingOps.front().second;
                } else {
                    bc_next(source, mod, o2, a2, pos);
                    source.setPos(sSrc);
                    pos = sPos;
                }
                if (isSimpleStore(o2)) {
                    /* LOAD x; LOAD y; STORE a; STORE b  ==  b, a = x, y
                       (the compiler dropped the SWAP). Consume both stores. */
                    auto storeName = [&](int op, int arg) -> PycRef<ASTNode> {
                        if (op == Pyc::STORE_FAST_A)
                            return new ASTName(code->getLocal(arg));
                        if (op == Pyc::STORE_DEREF_A)
                            return new ASTName(code->getCellVar(mod, arg));
                        return new ASTName(code->getName(arg));
                    };
                    PycRef<ASTNode> t1 = storeName(opcode, operand);
                    if (fromPending)
                        pendingOps.pop_front();
                    else
                        bc_next(source, mod, o2, a2, pos);
                    PycRef<ASTNode> t2 = storeName(o2, a2);
                    PycRef<ASTNode> y = stack.top();
                    stack.pop();
                    PycRef<ASTNode> x = stack.top();
                    stack.pop();
                    ASTTuple::value_t vals, names;
                    vals.push_back(x);
                    vals.push_back(y);
                    names.push_back(t2);
                    names.push_back(t1);
                    auto vt = new ASTTuple(vals);
                    vt->setRequireParens(false);
                    auto nt = new ASTTuple(names);
                    nt->setRequireParens(false);
                    curblock->append(new ASTStore(vt, nt));
                    prevOp1 = prevOp2 = -1;
                    continue;
                }
                source.setPos(sSrc);
                pos = sPos;
            }
            prevOp2 = prevOp1;
            prevOp1 = opcode;
        }

        if (commonGuard && opcode == Pyc::POP_JUMP_IF_FALSE_A) {
            /* 3.14 compiles any(genexpr)/all(...)/tuple(...) as
                   COPY 1; LOAD_COMMON_CONSTANT f; IS_OP; POP_JUMP_IF_FALSE slow
                   <inlined fast path> ... slow: <ordinary call>
               Decompile the ordinary call: pop the test and continue at `slow`. */
            commonGuard = false;
            int jumpBase = pos;
            while (!source.atEof()) {
                int keepSrc = source.pos(), keepPos = pos;
                int o3 = 0, a3 = 0;
                bc_next(source, mod, o3, a3, pos);
                if (o3 == Pyc::CACHE) {
                    jumpBase = pos;
                } else {
                    source.setPos(keepSrc);
                    pos = keepPos;
                    break;
                }
            }
            int target = jumpBase + operand * (int)sizeof(uint16_t);
            if (!stack.empty())
                stack.pop();
            source.setPos(target);
            pos = target;
            continue;
        }

        if ((!inlineComps.empty() || isCompCode) && mod->verCompare(3, 12) >= 0
                && (opcode == Pyc::POP_JUMP_IF_TRUE_A
                    || opcode == Pyc::POP_JUMP_IF_FALSE_A)
                && !source.atEof()) {
            /* Inlined comprehension filter: `if c` compiles to
                   POP_JUMP_IF_TRUE over; JUMP_BACKWARD loop; over: ...
               which is the 3.11 POP_JUMP_BACKWARD_IF_FALSE shape. */
            int sSrc = source.pos(), sPos = pos;
            int o2 = 0, a2 = 0;
            int jumpBase = pos;   /* jump offsets are relative to after caches */
            bc_next(source, mod, o2, a2, pos);
            while (o2 == Pyc::CACHE && !source.atEof()) {
                jumpBase = pos;
                bc_next(source, mod, o2, a2, pos);
            }
            if (o2 == Pyc::NOT_TAKEN && !source.atEof())
                bc_next(source, mod, o2, a2, pos);
            int afterJump = pos;
            if (o2 == Pyc::JUMP_BACKWARD_A || o2 == Pyc::JUMP_BACKWARD_NO_INTERRUPT_A) {
                /* the jump itself may carry inline cache entries */
                for (;;) {
                    if (source.atEof()) break;
                    int keepSrc = source.pos(), keepPos = pos;
                    int o3 = 0, a3 = 0;
                    bc_next(source, mod, o3, a3, pos);
                    if (o3 == Pyc::CACHE) {
                        afterJump = pos;
                    } else {
                        source.setPos(keepSrc);
                        pos = keepPos;
                        break;
                    }
                }
            }
            int target = jumpBase + operand * (int)sizeof(uint16_t);
            if ((o2 == Pyc::JUMP_BACKWARD_A || o2 == Pyc::JUMP_BACKWARD_NO_INTERRUPT_A)
                    && target == afterJump) {
                opcode = (opcode == Pyc::POP_JUMP_IF_TRUE_A)
                        ? Pyc::POP_JUMP_BACKWARD_IF_FALSE_A
                        : Pyc::POP_JUMP_BACKWARD_IF_TRUE_A;
            } else {
                source.setPos(sSrc);
                pos = sPos;
            }
        }

        if (mod->verCompare(3, 10) >= 0 && !stack.empty()
                && ((opcode == Pyc::COPY_A && operand == 1)
                    || opcode == Pyc::DUP_TOP
                    || opcode == Pyc::MATCH_SEQUENCE
                    || opcode == Pyc::MATCH_MAPPING
                    || opcode == Pyc::LOAD_GLOBAL_A
                    || opcode == Pyc::LOAD_NAME_A
                    || opcode == Pyc::LOAD_FAST_A
                    || opcode == Pyc::LOAD_DEREF_A)) {
            if (!matcher)
                matcher.reset(new MatchAnalyzer(code, mod));
            MatchCase first;
            if (matcher->isMatchStart(curpos) && matcher->parseCase(curpos, first)
                    && first.bodyEnd >= 0) {
                MatchCase second;
                if (!first.nonLast || matcher->parseCase(first.nextPos, second)) {
                    PycRef<ASTNode> subject = stack.top();
                    stack.pop();
                    PycRef<ASTMatchBlock> mb = new ASTMatchBlock(subject);
                    blocks.push(mb.cast<ASTBlock>());
                    curblock = blocks.top();
                    matches.emplace_back(mb, stack);
                    beginCase(matches.back(), first, false);
                    continue;
                }
            }
        }

        switch (opcode) {
        case Pyc::BINARY_OP_A:
            {
                if (operand == 26 && mod->verCompare(3, 14) >= 0) {
                    /* NB_SUBSCR: a[b] */
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> src = stack.top();
                    stack.pop();
                    stack.push(new ASTSubscr(src, subscr));
                    break;
                }
                ASTBinary::BinOp op = ASTBinary::from_binary_op(operand);
                if (op == ASTBinary::BIN_INVALID)
                    fprintf(stderr, "Unsupported `BINARY_OP` operand value: %d\n", operand);
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_ADD:
        case Pyc::BINARY_AND:
        case Pyc::BINARY_DIVIDE:
        case Pyc::BINARY_FLOOR_DIVIDE:
        case Pyc::BINARY_LSHIFT:
        case Pyc::BINARY_MODULO:
        case Pyc::BINARY_MULTIPLY:
        case Pyc::BINARY_OR:
        case Pyc::BINARY_POWER:
        case Pyc::BINARY_RSHIFT:
        case Pyc::BINARY_SUBTRACT:
        case Pyc::BINARY_TRUE_DIVIDE:
        case Pyc::BINARY_XOR:
        case Pyc::BINARY_MATRIX_MULTIPLY:
        case Pyc::INPLACE_ADD:
        case Pyc::INPLACE_AND:
        case Pyc::INPLACE_DIVIDE:
        case Pyc::INPLACE_FLOOR_DIVIDE:
        case Pyc::INPLACE_LSHIFT:
        case Pyc::INPLACE_MODULO:
        case Pyc::INPLACE_MULTIPLY:
        case Pyc::INPLACE_OR:
        case Pyc::INPLACE_POWER:
        case Pyc::INPLACE_RSHIFT:
        case Pyc::INPLACE_SUBTRACT:
        case Pyc::INPLACE_TRUE_DIVIDE:
        case Pyc::INPLACE_XOR:
        case Pyc::INPLACE_MATRIX_MULTIPLY:
            {
                ASTBinary::BinOp op = ASTBinary::from_opcode(opcode);
                if (op == ASTBinary::BIN_INVALID)
                    throw std::runtime_error("Unhandled opcode from ASTBinary::from_opcode");
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_SUBSCR:
            {
                PycRef<ASTNode> subscr = stack.top();
                stack.pop();
                PycRef<ASTNode> src = stack.top();
                stack.pop();
                stack.push(new ASTSubscr(src, subscr));
            }
            break;
        case Pyc::BREAK_LOOP:
            curblock->append(new ASTKeyword(ASTKeyword::KW_BREAK));
            break;
        case Pyc::BUILD_CLASS:
            {
                PycRef<ASTNode> class_code = stack.top();
                stack.pop();
                PycRef<ASTNode> bases = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTClass(class_code, bases, name));
            }
            break;
        case Pyc::BUILD_FUNCTION:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                stack.push(new ASTFunction(fun_code, {}, {}));
            }
            break;
        case Pyc::BUILD_LIST_A:
            {
                ASTList::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTList(values));
            }
            break;
        case Pyc::BUILD_SET_A:
            {
                ASTSet::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTSet(values));
            }
            break;
        case Pyc::BUILD_MAP_A:
            if (mod->verCompare(3, 5) >= 0) {
                auto map = new ASTMap;
                for (int i=0; i<operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    map->add(key, value);
                }
                stack.push(map);
            } else {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                stack.push(new ASTMap());
            }
            break;
        case Pyc::BUILD_CONST_KEY_MAP_A:
            // Top of stack will be a tuple of keys.
            // Values will start at TOS - 1.
            {
                PycRef<ASTNode> keys = stack.top();
                stack.pop();

                ASTConstMap::values_t values;
                values.reserve(operand);
                for (int i = 0; i < operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    values.push_back(value);
                }

                stack.push(new ASTConstMap(keys, values));
            }
            break;
        case Pyc::STORE_MAP:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                PycRef<ASTMap> map = stack.top().cast<ASTMap>();
                map->add(key, value);
            }
            break;
        case Pyc::BUILD_SLICE_A:
            {
                if (operand == 2) {
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }
                } else if (operand == 3) {
                    PycRef<ASTNode> step = stack.top();
                    stack.pop();
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (step.type() == ASTNode::NODE_OBJECT
                            && step.cast<ASTObject>()->object() == Pyc_None) {
                        step = NULL;
                    }

                    /* We have to do this as a slice where one side is another slice */
                    /* [[a:b]:c] */

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }

                    PycRef<ASTNode> lhs = stack.top();
                    stack.pop();

                    if (step == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, lhs, step));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, lhs, step));
                    }
                }
            }
            break;
        case Pyc::BUILD_STRING_A:
            {
                // Nearly identical logic to BUILD_LIST
                ASTList::value_t values;
                for (int i = 0; i < operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTJoinedStr(values));
            }
            break;
        case Pyc::BUILD_TUPLE_A:
            {
                // if class is a closure code, ignore this tuple
                PycRef<ASTNode> tos = stack.top();
                if (tos && tos->type() == ASTNode::NODE_LOADBUILDCLASS) {
                    break;
                }

                ASTTuple::value_t values;
                values.resize(operand);
                for (int i=0; i<operand; i++) {
                    values[operand-i-1] = stack.top();
                    stack.pop();
                }
                stack.push(new ASTTuple(values));
            }
            break;
        case Pyc::KW_NAMES_A:
            {

                int kwparams = code->getConst(operand).cast<PycTuple>()->size();
                ASTKwNamesMap kwparamList;
                std::vector<PycRef<PycObject>> keys = code->getConst(operand).cast<PycSimpleSequence>()->values();
                for (int i = 0; i < kwparams; i++) {
                    kwparamList.add(new ASTObject(keys[kwparams - i - 1]), stack.top());
                    stack.pop();
                }
                stack.push(new ASTKwNamesMap(kwparamList));
            }
            break;
        case Pyc::CALL_A:
        case Pyc::CALL_FUNCTION_A:
        case Pyc::INSTRUMENTED_CALL_A:
            {
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;

                /* Test for the load build class function */
                stack_hist.push(stack);
                /* Class keyword arguments (e.g. metaclass=) arrive via a 3.11
                   KW_NAMES map at TOS; capture them before scanning the bases. */
                ASTCall::kwparam_t classKwargs;
                if (mod->verCompare(3, 11) >= 0 && stack.top() != NULL
                        && stack.top().type() == ASTNode::NODE_KW_NAMES_MAP) {
                    PycRef<ASTKwNamesMap> km = stack.top().cast<ASTKwNamesMap>();
                    stack.pop();
                    for (const auto& kv : km->values())
                        classKwargs.push_back(kv);
                }
                int basecnt = 0;
                ASTTuple::value_t bases;
                bases.resize(basecnt);
                PycRef<ASTNode> TOS = stack.top();
                int TOS_type = TOS.type();
                // bases are NODE_NAME and NODE_BINARY at TOS
                while (TOS_type == ASTNode::NODE_NAME || TOS_type == ASTNode::NODE_BINARY) {
                    bases.resize(basecnt + 1);
                    bases[basecnt] = TOS;
                    basecnt++;
                    stack.pop();
                    TOS = stack.top();
                    TOS_type = TOS.type();
                }
                // qualified name is PycString at TOS
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                PycRef<ASTNode> function = stack.top();
                stack.pop();
                PycRef<ASTNode> loadbuild = stack.top();
                stack.pop();
                int loadbuild_type = loadbuild.type();
                if (loadbuild_type == ASTNode::NODE_LOADBUILDCLASS) {
                    /* Python 3.11 pushes a NULL before LOAD_BUILD_CLASS; drop it
                       so a following decorator application sees the real callable
                       beneath the class instead of the NULL. */
                    if (mod->verCompare(3, 11) >= 0 && !stack.empty()
                            && stack.top() == nullptr) {
                        stack.pop();
                    }
                    PycRef<ASTNode> call = new ASTCall(function, pparamList, classKwargs);
                    stack.push(new ASTClass(call, new ASTTuple(bases), name));
                    stack_hist.pop();
                    break;
                }
                else
                {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }

                /*
                KW_NAMES(i)
                    Stores a reference to co_consts[consti] into an internal variable for use by CALL.
                    co_consts[consti] must be a tuple of strings.
                    New in version 3.11.
                */
                if (mod->verCompare(3, 11) >= 0) {
                    PycRef<ASTNode> object_or_map = stack.top();
                    if (object_or_map.type() == ASTNode::NODE_KW_NAMES_MAP) {
                        stack.pop();
                        PycRef<ASTKwNamesMap> kwparams_map = object_or_map.cast<ASTKwNamesMap>();
                        for (ASTKwNamesMap::map_t::const_iterator it = kwparams_map->values().begin(); it != kwparams_map->values().end(); it++) {
                            kwparamList.push_front(std::make_pair(it->first, it->second));
                            pparams -= 1;
                        }
                    }
                }
                else {
                    for (int i = 0; i < kwparams; i++) {
                        PycRef<ASTNode> val = stack.top();
                        stack.pop();
                        PycRef<ASTNode> key = stack.top();
                        stack.pop();
                        kwparamList.push_front(std::make_pair(key, val));
                    }
                }
                for (int i=0; i<pparams; i++) {
                    PycRef<ASTNode> param = stack.top();
                    stack.pop();
                    if (param.type() == ASTNode::NODE_FUNCTION) {
                        PycRef<ASTNode> fun_code = param.cast<ASTFunction>()->code();
                        PycRef<PycCode> code_src = fun_code.cast<ASTObject>()->object().cast<PycCode>();
                        PycRef<PycString> function_name = code_src->name();
                        if (function_name->isEqual("<lambda>")) {
                            pparamList.push_front(param);
                        } else {
                            // Decorator used
                            PycRef<ASTNode> decor_name = new ASTName(function_name);
                            curblock->append(new ASTStore(param, decor_name));

                            pparamList.push_front(decor_name);
                        }
                    } else {
                        pparamList.push_front(param);
                    }
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                bool isDecoratorApply = false;
                bool compInlined = false;
                if (mod->verCompare(3, 11) < 0 && func != NULL
                        && func.type() == ASTNode::NODE_FUNCTION
                        && pparamList.size() == 1 && kwparamList.empty()) {
                    /* Python <= 3.10: a comprehension is a nested function
                       called with the iterator: MAKE_FUNCTION; <iter>; GET_ITER;
                       CALL_FUNCTION 1. */
                    PycRef<PycCode> ccode = func.cast<ASTFunction>()->code()
                            .cast<ASTObject>()->object().cast<PycCode>();
                    const char* cnm = ccode->name()->value();
                    if (cnm && (!strcmp(cnm, "<listcomp>") || !strcmp(cnm, "<setcomp>")
                            || !strcmp(cnm, "<dictcomp>") || !strcmp(cnm, "<genexpr>"))) {
                        bool savedClean = cleanBuild;
                        PycRef<ASTNode> compAst = BuildFromCode(ccode, mod);
                        cleanBuild = savedClean;
                        PycRef<ASTComprehension> comp = FindComprehension(compAst);
                        if (comp == NULL)
                            comp = SynthGenexpr(compAst);
                        if (comp != NULL && !comp->generators().empty()) {
                            if (!strcmp(cnm, "<setcomp>"))
                                comp->setKind(ASTComprehension::KIND_SET);
                            else if (!strcmp(cnm, "<dictcomp>"))
                                comp->setKind(ASTComprehension::KIND_DICT);
                            else if (!strcmp(cnm, "<genexpr>"))
                                comp->setKind(ASTComprehension::KIND_GENERATOR);
                            comp->generators().front()->setIter(pparamList.front());
                            stack.push(comp.cast<ASTNode>());
                            compInlined = true;
                        }
                    }
                }
                if (opcode == Pyc::CALL_A || opcode == Pyc::INSTRUMENTED_CALL_A) {
                    if (!stack.empty() && stack.top() == nullptr) {
                        /* Normal call: discard the NULL self slot. */
                        stack.pop();
                    } else if (!stack.empty() && stack.top() != nullptr) {
                        /* Python 3.11 leaves a non-NULL value in the self slot
                           (no PUSH_NULL) in two cases we reconstruct here:
                            - decorator application: TOS is the decorated
                              function/class, the real callable is below it;
                            - comprehension/genexpr call: the callable below is a
                              comprehension code object and TOS is the iterable. */
                        PycRef<ASTNode> below = stack.top();

                        bool topIsDecoTarget = (func.type() == ASTNode::NODE_FUNCTION
                                || func.type() == ASTNode::NODE_CLASS
                                || (func.type() == ASTNode::NODE_CALL
                                    && func.cast<ASTCall>()->isDecorator()));

                        /* `assert cond, msg` compiles to LOAD_ASSERTION_ERROR; <msg>;
                           CALL 0: the message sits in the callable slot and
                           AssertionError in the self slot (3.11+). */
                        bool belowIsAssert = below != NULL && below == assertErrNode;

                        bool belowIsComp = false;
                        if (hadGetIter && below != NULL && below.type() == ASTNode::NODE_FUNCTION) {
                            /* Only trust this as the comprehension's own
                               invocation when a GET_ITER immediately preceded
                               the call (comprehensions are always invoked as
                               `comp(iter(...))`). Without that guard, an
                               unrelated call that merely executes while the
                               comprehension's MAKE_FUNCTION result is still
                               waiting a couple of stack slots down (e.g. the
                               `im.items()` call feeding a dict comprehension)
                               would be mistaken for the real invocation. */
                            PycRef<PycCode> bcode = below.cast<ASTFunction>()->code()
                                    .cast<ASTObject>()->object().cast<PycCode>();
                            const char* bnm = bcode->name()->value();
                            belowIsComp = bnm && (!strcmp(bnm, "<listcomp>")
                                    || !strcmp(bnm, "<setcomp>")
                                    || !strcmp(bnm, "<dictcomp>")
                                    || !strcmp(bnm, "<genexpr>"));
                        }

                        if (belowIsAssert) {
                            pparamList.push_front(func);
                            func = stack.top();
                            stack.pop();
                        } else if (belowIsComp) {
                            /* Inline the comprehension. The callable is `below`
                               and the iterable is `func` (TOS). Decompile the
                               comprehension body and substitute its implicit
                               ".0" iterator with the real iterable. */
                            PycRef<PycCode> ccode = below.cast<ASTFunction>()->code()
                                    .cast<ASTObject>()->object().cast<PycCode>();
                            bool savedClean = cleanBuild;
                            PycRef<ASTNode> compAst = BuildFromCode(ccode, mod);
                            cleanBuild = savedClean;
                            PycRef<ASTComprehension> comp = FindComprehension(compAst);
                            if (comp == NULL)
                                comp = SynthGenexpr(compAst);   /* generator expr */
                            if (comp != NULL && !comp->generators().empty()) {
                                const char* cnm = ccode->name()->value();
                                if (cnm && !strcmp(cnm, "<setcomp>"))
                                    comp->setKind(ASTComprehension::KIND_SET);
                                else if (cnm && !strcmp(cnm, "<dictcomp>"))
                                    comp->setKind(ASTComprehension::KIND_DICT);
                                else if (cnm && !strcmp(cnm, "<genexpr>"))
                                    comp->setKind(ASTComprehension::KIND_GENERATOR);
                                comp->generators().front()->setIter(func);
                                stack.pop();   /* remove the comprehension callable */
                                stack.push(comp.cast<ASTNode>());
                                compInlined = true;
                            }
                            /* else: leave as-is and fall through to a plain call. */
                        } else if (topIsDecoTarget) {
                            /* Decorator application: `func` (TOS) is the decorated
                               target, the real callable is below. */
                            PycRef<ASTNode> target = func;
                            func = stack.top();
                            stack.pop();
                            if (target.type() == ASTNode::NODE_FUNCTION) {
                                PycRef<PycCode> tcode = target.cast<ASTFunction>()->code()
                                        .cast<ASTObject>()->object().cast<PycCode>();
                                PycRef<PycString> tname = tcode->name();
                                if (!tname->isEqual("<lambda>")) {
                                    /* Emit the def, then reference it by name. */
                                    PycRef<ASTNode> decor_name = new ASTName(tname);
                                    curblock->append(new ASTStore(target, decor_name));
                                    target = decor_name;
                                }
                            } else if (target.type() == ASTNode::NODE_CLASS) {
                                /* Decorated class: emit the class body, then
                                   reference it by name as the decorator argument. */
                                PycRef<ASTNode> cname = target.cast<ASTClass>()->name();
                                PycRef<ASTNode> decor_name;
                                if (cname != NULL && cname.type() == ASTNode::NODE_NAME) {
                                    decor_name = cname;
                                } else if (cname != NULL && cname.type() == ASTNode::NODE_OBJECT) {
                                    PycRef<PycString> s =
                                            cname.cast<ASTObject>()->object().try_cast<PycString>();
                                    if (s != NULL)
                                        decor_name = new ASTName(s);
                                }
                                if (decor_name != NULL) {
                                    curblock->append(new ASTStore(target, decor_name));
                                    target = decor_name;
                                }
                            }
                            pparamList.push_front(target);
                            isDecoratorApply = true;
                        }
                        /* else: a non-null self slot we don't special-case (e.g.
                           method calls collapsed by LOAD_METHOD); behave as before
                           with `func` as the callable. */
                    }
                }

                if (!compInlined) {
                    PycRef<ASTNode> callNode = new ASTCall(func, pparamList, kwparamList);
                    if (isDecoratorApply)
                        callNode.cast<ASTCall>()->setDecorator(true);
                    stack.push(callNode);
                }
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_A:
            {
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_EX_A:
        case Pyc::INSTRUMENTED_CALL_FUNCTION_EX_A:
            {
                /* Call with iterable args and (optionally) a mapping of kwargs.
                   Low bit of the flag operand means a kwargs mapping is present. */
                PycRef<ASTNode> kw;
                /* 3.14 dropped the oparg: the kwargs slot is always present
                   (a NULL when there are none). */
                if ((operand & 0x01) || mod->verCompare(3, 14) >= 0) {
                    kw = stack.top();
                    stack.pop();
                }
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                /* Python 3.11+ pushes a NULL sentinel below the callable. */
                if (mod->verCompare(3, 11) >= 0 && !stack.empty()
                        && stack.top() == nullptr) {
                    stack.pop();
                }

                ASTCall::pparam_t exPos;
                ASTCall::kwparam_t exKw;
                /* `f(a, *args)` compiles to f(*(a, *args)) and
                   `f(c=c, **kw)` to f(**{'c': c, **kw}); undo that so the
                   call reads like the original source. */
                if (var != NULL && var.type() == ASTNode::NODE_TUPLE) {
                    for (const auto& v : var.cast<ASTTuple>()->values())
                        exPos.push_back(v);
                    var = NULL;
                }
                if (kw != NULL && kw.type() == ASTNode::NODE_MAP) {
                    PycRef<ASTNode> unpackedKw;
                    int unpackCount = 0;
                    bool allSimple = true;
                    for (const auto& kv : kw.cast<ASTMap>()->values()) {
                        if (kv.first == NULL) {
                            unpackCount++;
                            unpackedKw = kv.second;
                        } else if (kv.first.type() != ASTNode::NODE_OBJECT
                                   || kv.first.cast<ASTObject>()->object()
                                          .try_cast<PycString>() == NULL) {
                            allSimple = false;
                        }
                    }
                    if (allSimple && unpackCount <= 1) {
                        for (const auto& kv : kw.cast<ASTMap>()->values()) {
                            if (kv.first != NULL)
                                exKw.push_back(kv);
                        }
                        kw = unpackedKw;   /* NULL when there was no ** entry */
                    }
                }

                PycRef<ASTNode> call = new ASTCall(func, exPos, exKw);
                if (var != NULL)
                    call.cast<ASTCall>()->setVar(var);
                if (kw != NULL)
                    call.cast<ASTCall>()->setKW(kw);
                stack.push(call);
            }
            break;
        case Pyc::CALL_METHOD_A:
            {
                ASTCall::pparam_t pparamList;
                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> param = stack.top();
                    stack.pop();
                    if (param.type() == ASTNode::NODE_FUNCTION) {
                        PycRef<ASTNode> fun_code = param.cast<ASTFunction>()->code();
                        PycRef<PycCode> code_src = fun_code.cast<ASTObject>()->object().cast<PycCode>();
                        PycRef<PycString> function_name = code_src->name();
                        if (function_name->isEqual("<lambda>")) {
                            pparamList.push_front(param);
                        } else {
                            // Decorator used
                            PycRef<ASTNode> decor_name = new ASTName(function_name);
                            curblock->append(new ASTStore(param, decor_name));

                            pparamList.push_front(decor_name);
                        }
                    } else {
                        pparamList.push_front(param);
                    }
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                stack.push(new ASTCall(func, pparamList, ASTCall::kwparam_t()));
            }
            break;
        case Pyc::CONTINUE_LOOP_A:
            curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
            break;
        case Pyc::JUMP_IF_NOT_EXC_MATCH_A:
            {
                /* 3.9/3.10: COMPARE_OP(exception match) + POP_JUMP_IF_FALSE
                   fused into one instruction. */
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTCompare(left, right, ASTCompare::CMP_EXCEPTION));
                pendingOps.push_back(std::make_pair((int)Pyc::POP_JUMP_IF_FALSE_A, operand));
            }
            break;
        case Pyc::COMPARE_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                auto arg = operand;
                if (mod->verCompare(3, 12) == 0)
                    arg >>= 4; // changed under GH-100923
                else if (mod->verCompare(3, 13) >= 0)
                    arg >>= 5;
                stack.push(new ASTCompare(left, right, arg));
            }
            break;
        case Pyc::CONTAINS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'in' and 1 for 'not in'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_NOT_IN : ASTCompare::CMP_IN));
            }
            break;
        case Pyc::DELETE_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                curblock->append(new ASTDelete(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR)));
            }
            break;
        case Pyc::DELETE_GLOBAL_A:
            code->markGlobal(code->getName(operand));
            /* Fall through */
        case Pyc::DELETE_NAME_A:
            {
                PycRef<PycString> varname = code->getName(operand);

                if (varname->length() >= 2 && varname->value()[0] == '_'
                        && varname->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                PycRef<ASTNode> name = new ASTName(varname);
                if (isExceptVarDelete(curblock, name))
                    break;
                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_FAST_A:
            {
                PycRef<ASTNode> name;

                if (mod->verCompare(1, 3) < 0)
                    name = new ASTName(code->getName(operand));
                else
                    name = new ASTName(code->getLocal(operand));

                if (name.cast<ASTName>()->name()->value()[0] == '_'
                        && name.cast<ASTName>()->name()->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                if (isExceptVarDelete(curblock, name))
                    break;

                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_SLICE_0:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE0))));
            }
            break;
        case Pyc::DELETE_SLICE_1:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE1, upper))));
            }
            break;
        case Pyc::DELETE_SLICE_2:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE2, NULL, lower))));
            }
            break;
        case Pyc::DELETE_SLICE_3:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE3, upper, lower))));
            }
            break;
        case Pyc::DELETE_SUBSCR:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, key)));
            }
            break;
        case Pyc::DUP_TOP:
            {
                if (stack.top().type() == PycObject::TYPE_NULL) {
                    stack.push(stack.top());
                } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    auto chainstore = stack.top();
                    stack.pop();
                    stack.push(stack.top());
                    stack.push(chainstore);
                } else {
                    stack.push(stack.top());
                    ASTNodeList::list_t targets;
                    stack.push(new ASTChainStore(targets, stack.top()));
                }
            }
            break;
        case Pyc::DUP_TOP_TWO:
            {
                PycRef<ASTNode> first = stack.top();
                stack.pop();
                PycRef<ASTNode> second = stack.top();

                stack.push(first);
                stack.push(second);
                stack.push(first);
            }
            break;
        case Pyc::DUP_TOPX_A:
            {
                std::stack<PycRef<ASTNode> > first;
                std::stack<PycRef<ASTNode> > second;

                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> node = stack.top();
                    stack.pop();
                    first.push(node);
                    second.push(node);
                }

                while (first.size()) {
                    stack.push(first.top());
                    first.pop();
                }

                while (second.size()) {
                    stack.push(second.top());
                    second.pop();
                }
            }
            break;
        case Pyc::END_FINALLY:
            {
                bool isFinally = false;
                if (curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    PycRef<ASTBlock> final = curblock;
                    blocks.pop();

                    stack = stack_hist.top();
                    stack_hist.pop();

                    curblock = blocks.top();
                    curblock->append(final.cast<ASTNode>());
                    isFinally = true;
                } else if (curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                    blocks.pop();
                    PycRef<ASTBlock> prev = curblock;

                    bool isUninitAsyncFor = false;
                    if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                        auto container = blocks.top();
                        blocks.pop();
                        auto asyncForBlock = blocks.top();
                        isUninitAsyncFor = asyncForBlock->blktype() == ASTBlock::BLK_ASYNCFOR && !asyncForBlock->inited();
                        if (isUninitAsyncFor) {
                            auto tryBlock = container->nodes().front().cast<ASTBlock>();
                            if (!tryBlock->nodes().empty() && tryBlock->blktype() == ASTBlock::BLK_TRY) {
                                auto store = tryBlock->nodes().front().try_cast<ASTStore>();
                                if (store) {
                                    asyncForBlock.cast<ASTIterBlock>()->setIndex(store->dest());
                                }
                            }
                            curblock = blocks.top();
                            stack = stack_hist.top();
                            stack_hist.pop();
                            if (!curblock->inited())
                                fprintf(stderr, "Error when decompiling 'async for'.\n");
                        } else {
                            blocks.push(container);
                        }
                    }

                    if (!isUninitAsyncFor) {
                        if (curblock->size() != 0) {
                            blocks.top()->append(curblock.cast<ASTNode>());
                        }

                        curblock = blocks.top();

                        /* Turn it into an else statement. */
                        if (curblock->end() != pos || curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            PycRef<ASTBlock> elseblk = new ASTBlock(ASTBlock::BLK_ELSE, prev->end());
                            elseblk->init();
                            blocks.push(elseblk);
                            curblock = blocks.top();
                        }
                        else {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    /* This marks the end of the except block(s). */
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (!cont->hasFinally() || isFinally) {
                        /* If there's no finally block, pop the container. */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());
                    }
                }
            }
            break;
        case Pyc::EXEC_STMT:
            {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> loc = stack.top();
                stack.pop();
                PycRef<ASTNode> glob = stack.top();
                stack.pop();
                PycRef<ASTNode> stmt = stack.top();
                stack.pop();

                curblock->append(new ASTExec(stmt, glob, loc));
            }
            break;
        case Pyc::FOR_ITER_A:
        case Pyc::INSTRUMENTED_FOR_ITER_A:
            {
                PycRef<ASTNode> iter = stack.top(); // Iterable
                if (mod->verCompare(3, 12) < 0) {
                    // Do not pop the iterator for py 3.12+
                    stack.pop();
                }
                /* Pop it? Don't pop it? */

                int end;
                bool comprehension = false;

                // before 3.8, there is a SETUP_LOOP instruction with block start and end position,
                //    the operand is usually a jump to a POP_BLOCK instruction
                // after 3.8, block extent has to be inferred implicitly; the operand is a jump to a position after the for block
                if (mod->majorVer() == 3 && mod->minorVer() >= 8) {
                    end = operand;
                    if (mod->verCompare(3, 10) >= 0)
                        end *= sizeof(uint16_t); // // BPO-27129
                    end += pos;
                    /* 3.12+: the jump is relative to the instruction after
                       FOR_ITER's single inline CACHE entry. */
                    if (mod->verCompare(3, 12) >= 0)
                        end += sizeof(uint16_t);
                    {
                        const char* cn = code->name()->value();
                        comprehension = cn && (!strcmp(cn, "<listcomp>")
                                || !strcmp(cn, "<setcomp>")
                                || !strcmp(cn, "<dictcomp>"));
                        if (!inlineComps.empty())
                            comprehension = true;
                    }
                } else {
                    PycRef<ASTBlock> top = blocks.top();
                    end = top->end(); // block end position from SETUP_LOOP
                    if (top->blktype() == ASTBlock::BLK_WHILE) {
                        blocks.pop();
                    } else {
                        comprehension = true;
                    }
                }

                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, end, iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                stack.push(NULL);
            }
            break;
        case Pyc::FOR_LOOP_A:
            {
                PycRef<ASTNode> curidx = stack.top(); // Current index
                stack.pop();
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                bool comprehension = false;
                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                } else {
                    comprehension = true;
                }
                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, top->end(), iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                /* Python Docs say:
                      "push the sequence, the incremented counter,
                       and the current item onto the stack." */
                stack.push(iter);
                stack.push(curidx);
                stack.push(NULL); // We can totally hack this >_>
            }
            break;
        case Pyc::GET_AITER:
            {
                // Logic similar to FOR_ITER_A
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                    PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_ASYNCFOR, curpos, top->end(), iter);
                    blocks.push(forblk.cast<ASTBlock>());
                    curblock = blocks.top();
                    stack.push(nullptr);
                } else {
                     fprintf(stderr, "Unsupported use of GET_AITER outside of SETUP_LOOP\n");
                }
            }
            break;
        case Pyc::GET_ANEXT:
            break;
        case Pyc::TO_BOOL:
            /* 3.13: implicit bool() conversion before a conditional jump. */
            break;
        case Pyc::STORE_FAST_STORE_FAST_A:
            pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand >> 4));
            if (!fusedKeepFirst.count(curpos))
                pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand & 0xF));
            break;
        case Pyc::STORE_FAST_LOAD_FAST_A:
            pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand >> 4));
            pendingOps.push_back(std::make_pair((int)Pyc::LOAD_FAST_A, operand & 0xF));
            break;
        case Pyc::CONVERT_VALUE_A:
            {
                /* 3.13 f-string: 1 = !s, 2 = !r, 3 = !a (formatting follows). */
                PycRef<ASTNode> val = stack.top();
                stack.pop();
                stack.push(new ASTFormattedValue(val,
                        static_cast<ASTFormattedValue::ConversionFlag>(operand), nullptr));
            }
            break;
        case Pyc::FORMAT_SIMPLE:
            {
                PycRef<ASTNode> val = stack.top();
                if (val == NULL || val.type() != ASTNode::NODE_FORMATTEDVALUE) {
                    stack.pop();
                    stack.push(new ASTFormattedValue(val, ASTFormattedValue::NONE, nullptr));
                }
            }
            break;
        case Pyc::FORMAT_WITH_SPEC:
            {
                PycRef<ASTNode> spec = stack.top();
                stack.pop();
                PycRef<ASTNode> val = stack.top();
                stack.pop();
                int conv = ASTFormattedValue::NONE;
                if (val != NULL && val.type() == ASTNode::NODE_FORMATTEDVALUE) {
                    PycRef<ASTFormattedValue> fv = val.cast<ASTFormattedValue>();
                    conv = fv->conversion() & ASTFormattedValue::CONVERSION_MASK;
                    val = fv->val();
                }
                stack.push(new ASTFormattedValue(val,
                        static_cast<ASTFormattedValue::ConversionFlag>(
                            conv | ASTFormattedValue::HAVE_FMT_SPEC), spec));
            }
            break;
        case Pyc::CALL_KW_A:
            {
                /* 3.13: like KW_NAMES + CALL; the names tuple is on top. */
                PycRef<ASTNode> names = stack.top();
                stack.pop();
                ASTKwNamesMap kwparamList;
                if (names != NULL && names.type() == ASTNode::NODE_OBJECT) {
                    std::vector<PycRef<PycObject> > keys =
                        names.cast<ASTObject>()->object().cast<PycSimpleSequence>()->values();
                    int n = (int)keys.size();
                    for (int i = 0; i < n; i++) {
                        kwparamList.add(new ASTObject(keys[n - i - 1]), stack.top());
                        stack.pop();
                    }
                }
                stack.push(new ASTKwNamesMap(kwparamList));
                pendingOps.push_back(std::make_pair((int)Pyc::CALL_A, operand));
            }
            break;
        case Pyc::FORMAT_VALUE_A:
            {
                auto conversion_flag = static_cast<ASTFormattedValue::ConversionFlag>(operand);
                PycRef<ASTNode> format_spec = nullptr;
                if (conversion_flag & ASTFormattedValue::HAVE_FMT_SPEC) {
                    format_spec = stack.top();
                    stack.pop();
                }
                auto val = stack.top();
                stack.pop();
                stack.push(new ASTFormattedValue(val, conversion_flag, format_spec));
            }
            break;
        case Pyc::GET_AWAITABLE:
        case Pyc::GET_AWAITABLE_A:
            {
                /* Python 3.11+ adds an operand (0 = plain await, 1/2 = inside
                   async-with __aenter__/__aexit__) that doesn't affect the
                   reconstructed source, so it's safe to ignore here. */
                PycRef<ASTNode> object = stack.top();
                stack.pop();
                stack.push(new ASTAwaitable(object));
            }
            break;
        case Pyc::GET_ITER:
        case Pyc::GET_YIELD_FROM_ITER:
            /* We just entirely ignore this, beyond noting it happened (see
               sawGetIter/hadGetIter) so a following CALL can recognize a
               genuine comprehension invocation. */
            sawGetIter = true;
            break;
        case Pyc::IMPORT_NAME_A:
            if (mod->majorVer() == 1) {
                stack.push(new ASTImport(new ASTName(code->getName(operand)), NULL));
            } else {
                PycRef<ASTNode> fromlist = stack.top();
                stack.pop();
                int level = 0;
                if (mod->verCompare(2, 5) >= 0) {
                    /* Level: 0 = absolute import, N>0 = N leading dots in a
                       relative `from .[.[pkg]] import x`. Needed to render
                       the dots -- co_names[namei] is "" for a bare `from .
                       import x` and would otherwise print as `from  import
                       x` (missing dot(s), invalid syntax). */
                    PycRef<ASTNode> levelNode = stack.top();
                    stack.pop();
                    if (levelNode != NULL && levelNode.type() == ASTNode::NODE_OBJECT) {
                        PycRef<PycObject> levelObj = levelNode.cast<ASTObject>()->object();
                        if (levelObj.type() == PycObject::TYPE_INT) {
                            level = levelObj.cast<PycInt>()->value();
                        }
                    }
                }
                PycRef<ASTImport> imp = new ASTImport(new ASTName(code->getName(operand)), fromlist);
                imp->setLevel(level);
                stack.push(imp.cast<ASTNode>());
            }
            break;
        case Pyc::IMPORT_FROM_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::IMPORT_STAR:
            {
                PycRef<ASTNode> import = stack.top();
                stack.pop();
                curblock->append(new ASTStore(import, NULL));
            }
            break;
        case Pyc::IS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'is' and 1 for 'is not'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_IS_NOT : ASTCompare::CMP_IS));
            }
            break;
        case Pyc::JUMP_IF_FALSE_A:
        case Pyc::JUMP_IF_TRUE_A:
        case Pyc::JUMP_IF_FALSE_OR_POP_A:
        case Pyc::JUMP_IF_TRUE_OR_POP_A:
        case Pyc::POP_JUMP_IF_FALSE_A:
        case Pyc::POP_JUMP_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A:
            {
                /* Python 3.11 while-loop entry guard: open a BLK_WHILE here
                   instead of an if. The matching back-edge (POP_JUMP_BACKWARD)
                   closes it. */
                auto wit = whileGuardEnd.find(curpos);
                if (wit != whileGuardEnd.end()) {
                    PycRef<ASTNode> wcond = stack.top();
                    stack.pop();
                    bool wneg = false;
                    if (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A) {
                        wcond = new ASTCompare(wcond, new ASTObject(Pyc_None),
                                               ASTCompare::CMP_IS_NOT);
                    } else if (opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                        wcond = new ASTCompare(wcond, new ASTObject(Pyc_None),
                                               ASTCompare::CMP_IS);
                    } else if (opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A) {
                        wneg = true;
                    }
                    PycRef<ASTCondBlock> wh = new ASTCondBlock(
                            ASTBlock::BLK_WHILE, wit->second, wcond, wneg);
                    wh->init(ASTCondBlock::PRE_POPPED);
                    blocks.push(wh.cast<ASTBlock>());
                    curblock = blocks.top();
                    break;
                }

                PycRef<ASTNode> cond = stack.top();
                PycRef<ASTCondBlock> ifblk;
                int popped = ASTCondBlock::UNINITED;

                if (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                    /* Python 3.11: compare TOS against None, then pop and jump.
                       IF_NONE jumps past the body when TOS is None, so the body
                       runs for "x is not None" (and vice-versa). */
                    stack.pop();
                    ASTCompare::CompareOp cmpop =
                        (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A)
                        ? ASTCompare::CMP_IS_NOT
                        : ASTCompare::CMP_IS;
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), cmpop);
                    popped = ASTCondBlock::PRE_POPPED;
                }

                if (!matches.empty() && matches.back().cblock == curblock
                        && matches.back().guardOpen && curblock->size() == 0
                        && opcode != Pyc::JUMP_IF_FALSE_A
                        && opcode != Pyc::JUMP_IF_TRUE_A
                        && opcode != Pyc::JUMP_IF_FALSE_OR_POP_A
                        && opcode != Pyc::JUMP_IF_TRUE_OR_POP_A) {
                    /* The guard of a case: `case pattern if cond:` */
                    MatchCtx& mc = matches.back();
                    int gt = operand;
                    if (mod->verCompare(3, 10) >= 0)
                        gt *= sizeof(uint16_t);
                    if (mod->verCompare(3, 12) >= 0
                            || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                            || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A)
                        gt += pos;
                    int ni = matcher->indexOf(gt);
                    bool isGuard;
                    if (mc.guardTarget >= 0)
                        isGuard = (gt == mc.guardTarget);
                    else if (mc.cur.bodyEnd >= 0)
                        isGuard = (gt == mc.cur.nextPos);
                    else
                        isGuard = mc.cur.nonLast || gt == mc.endPos
                                || (ni >= 0 && matcher->instrs()[ni].op == Pyc::NOP)
                                || (!mc.isDefault && mc.cblock->size() == 0 && gt > pos);
                    if (isGuard) {
                        stack.pop();
                        if (opcode == Pyc::POP_JUMP_IF_TRUE_A
                                || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                                || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A)
                            cond = new ASTUnary(cond, ASTUnary::UN_NOT);
                        PycRef<ASTNode> prevGuard = mc.cblock->guard();
                        mc.cblock->setGuard(prevGuard == nullptr ? cond
                                : new ASTBinary(prevGuard, cond, ASTBinary::BIN_LOG_AND));
                        if (mc.guardTarget < 0) {
                            mc.guardTarget = gt;
                            if (mc.cur.bodyEnd < 0) {
                                mc.cur.bodyEnd = gt;
                                mc.cur.nextPos = gt;
                                mc.endJumpPos = gt - 2;
                                mc.cblock->setEnd(gt);
                            }
                        }
                        break;
                    }
                    mc.guardOpen = false;
                }

                if (opcode == Pyc::POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A) {
                    /* Pop condition before the jump */
                    stack.pop();
                    popped = ASTCondBlock::PRE_POPPED;
                }

                /* Store the current stack for the else statement(s) */
                stack_hist.push(stack);

                if (opcode == Pyc::JUMP_IF_FALSE_OR_POP_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A) {
                    /* Pop condition only if condition is met */
                    stack.pop();
                    popped = ASTCondBlock::POPPED;
                }

                /* "Jump if true" means "Jump if not false" */
                bool neg = opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A;

                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129
                if (mod->verCompare(3, 12) >= 0
                        || opcode == Pyc::JUMP_IF_FALSE_A
                        || opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                    /* Offset is relative in these cases */
                    offs += pos;
                }

                /* Chained comparison: if this conditional jump lands on a
                   trampoline (POP_TOP; JUMP_FORWARD exit), resolve it to the
                   real exit so both halves of `a == b == c` share a target and
                   merge with `and` rather than `or`. */
                {
                    auto tit = trampolineExit.find(offs);
                    if (tit != trampolineExit.end())
                        offs = tit->second;
                }

                if (cond.type() == ASTNode::NODE_COMPARE
                        && cond.cast<ASTCompare>()->op() == ASTCompare::CMP_EXCEPTION) {
                    int except_end = offs;
                    if (curblock->blktype() == ASTBlock::BLK_EXCEPT
                            && curblock.cast<ASTCondBlock>()->cond() == NULL) {
                        /* Refining the initial (type-less) except handler. The
                           clause ends at the dispatch jump target (the next
                           clause / reraise), not at the whole handler region;
                           using the region end would swallow following clauses
                           when this clause does not fall through (e.g. raises). */
                        if (offs <= pos)
                            except_end = curblock->end();
                        blocks.pop();
                        curblock = blocks.top();

                        stack_hist.pop();
                    }

                    ifblk = new ASTCondBlock(ASTBlock::BLK_EXCEPT, except_end, cond.cast<ASTCompare>()->right(), false);
                } else if (curblock->blktype() == ASTBlock::BLK_ELSE
                           && curblock->size() == 0) {
                    /* Collapse into elif statement */
                    blocks.pop();
                    stack = stack_hist.top();
                    stack_hist.pop();
                    ifblk = new ASTCondBlock(ASTBlock::BLK_ELIF, offs, cond, neg);
                } else if (curblock->size() == 0 && !curblock->inited()
                           && curblock->blktype() == ASTBlock::BLK_WHILE) {
                    /* The condition for a while loop */
                    PycRef<ASTBlock> top = blocks.top();
                    blocks.pop();
                    ifblk = new ASTCondBlock(top->blktype(), offs, cond, neg);

                    /* We don't store the stack for loops! Pop it! */
                    stack_hist.pop();
                } else if (curblock->size() == 0 && curblock->end() <= offs
                           && (curblock->blktype() == ASTBlock::BLK_IF
                           || curblock->blktype() == ASTBlock::BLK_ELIF
                           || curblock->blktype() == ASTBlock::BLK_WHILE)
                           && blocks.size() > 1) {
                    PycRef<ASTNode> newcond;
                    PycRef<ASTCondBlock> top = curblock.cast<ASTCondBlock>();
                    PycRef<ASTNode> cond1 = top->cond();
                    blocks.pop();

                    if (curblock->blktype() == ASTBlock::BLK_WHILE) {
                        stack_hist.pop();
                    } else {
                        FastStack s_top = stack_hist.top();
                        stack_hist.pop();
                        stack_hist.pop();
                        stack_hist.push(s_top);
                    }

                    if (curblock->end() == offs
                            || (curblock->end() == curpos && !top->negative())) {
                        /* if blah and blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_AND);
                    } else {
                        /* if blah or blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_OR);
                    }
                    ifblk = new ASTCondBlock(top->blktype(), offs, newcond, neg);
                } else if (curblock->blktype() == ASTBlock::BLK_FOR
                            && curblock.cast<ASTIterBlock>()->isComprehension()
                            && mod->verCompare(2, 7) >= 0) {
                    /* Comprehension condition */
                    curblock.cast<ASTIterBlock>()->setCondition(cond);
                    stack_hist.pop();
                    // TODO: Handle older python versions, where condition
                    // is laid out a little differently.
                    break;
                } else {
                    /* Plain old if statement */
                    ifblk = new ASTCondBlock(ASTBlock::BLK_IF, offs, cond, neg);
                }

                if (popped)
                    ifblk->init(popped);

                blocks.push(ifblk.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::POP_JUMP_BACKWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NONE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A:
            {
                /* Python 3.11 while-loop back-edge: the duplicate condition has
                   just been evaluated onto the stack; discard it and close the
                   BLK_WHILE opened at the matching entry guard. Only act when a
                   BLK_WHILE is actually open (guard against a misidentified
                   back-edge collapsing the whole block stack). */
                if (whileBackedges.count(curpos)) {
                    bool hasWhile = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmp = blocks;
                        while (!tmp.empty()) {
                            if (tmp.top()->blktype() == ASTBlock::BLK_WHILE) {
                                hasWhile = true;
                                break;
                            }
                            tmp.pop();
                        }
                    }
                    if (hasWhile) {
                        if (!stack.empty())
                            stack.pop();
                        /* Close any blocks still open inside the loop body, then
                           the BLK_WHILE itself. */
                        while (blocks.size() > 1
                               && curblock->blktype() != ASTBlock::BLK_WHILE) {
                            PycRef<ASTBlock> inner = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            curblock->append(inner.cast<ASTNode>());
                        }
                        if (curblock->blktype() == ASTBlock::BLK_WHILE
                                && blocks.size() > 1) {
                            PycRef<ASTBlock> wh = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            curblock->append(wh.cast<ASTNode>());
                        }
                        break;
                    }
                }

                /* Python 3.11: conditional jump backwards. In compiled loops
                   this implements an in-body guard ("if cond: <rest of body>")
                   where a failed test jumps back to the loop header to start the
                   next iteration. Model it as an if-block that spans to the end
                   of the enclosing loop body. */
                PycRef<ASTNode> cond = stack.top();
                stack.pop();

                bool neg = false;
                if (opcode == Pyc::POP_JUMP_BACKWARD_IF_NONE_A) {
                    /* jumps back when "x is None" -> body runs for "x is not None" */
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), ASTCompare::CMP_IS_NOT);
                } else if (opcode == Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A) {
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), ASTCompare::CMP_IS);
                } else if (opcode == Pyc::POP_JUMP_BACKWARD_IF_TRUE_A) {
                    /* jumps back when cond is true -> body runs for "not cond" */
                    neg = true;
                }

                int end = curblock->end();

                stack_hist.push(stack);
                PycRef<ASTCondBlock> ifblk =
                        new ASTCondBlock(ASTBlock::BLK_IF, end, cond, neg);
                ifblk->init(ASTCondBlock::PRE_POPPED);
                blocks.push(ifblk.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::JUMP_ABSOLUTE_A:
        // bpo-47120: Replaced JUMP_ABSOLUTE by the relative jump JUMP_BACKWARD.
        case Pyc::JUMP_BACKWARD_A:
        case Pyc::JUMP_BACKWARD_NO_INTERRUPT_A:
            {
                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129

                if (mod->verCompare(3, 8) >= 0 && mod->verCompare(3, 11) < 0
                        && offs < pos
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && pos == curblock->end()) {
                    /* `if c: body` at the tail of a loop: this jump is both the
                       last instruction of the if-region and the natural loop
                       back-edge. Close the if-block(s) and let the loop close
                       below. */
                    bool loopOwns = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmp = blocks;
                        while (!tmp.empty()) {
                            ASTBlock::BlkType bt = tmp.top()->blktype();
                            if (bt == ASTBlock::BLK_FOR) {
                                loopOwns = tmp.top()->end() == pos
                                        && !tmp.top().cast<ASTIterBlock>()->isComprehension()
                                        && offs == tmp.top().cast<ASTIterBlock>()->start();
                                break;
                            }
                            if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
                                break;
                            tmp.pop();
                        }
                    }
                    while (loopOwns && (curblock->blktype() == ASTBlock::BLK_IF
                                        || curblock->blktype() == ASTBlock::BLK_ELIF)
                            && blocks.size() > 1) {
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        PycRef<ASTBlock> inner = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(inner.cast<ASTNode>());
                    }
                }

                if (!inlineComps.empty() && offs < pos
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && InsideComprehensionAdd(blocks)) {
                    /* Filter/loop back-edge inside an inlined comprehension:
                       blocks are closed at the matching END_FOR. */
                    break;
                }

                if (offs < pos) {
                    /* Python 3.11 filtered comprehension ("if cond") tail:
                       MAP_ADD/SET_ADD/LIST_APPEND already turned the add into
                       a NODE_COMPREHENSION value sitting on the stack, so this
                       backward jump is the natural end of one filtered
                       iteration, not a real `continue`. Fold the filter's
                       condition into the enclosing comprehension's FOR block
                       (so it still prints as `if cond`), close the filter
                       block(s) plainly, and let the ordinary BLK_FOR handling
                       just below finish closing the loop itself. */
                    if ((curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                            && !stack.empty() && stack.top() != NULL
                            && stack.top().type() == ASTNode::NODE_COMPREHENSION) {
                        bool ancestorIsCompFor = false;
                        {
                            std::stack<PycRef<ASTBlock> > tmp = blocks;
                            while (!tmp.empty()) {
                                ASTBlock::BlkType bt = tmp.top()->blktype();
                                if (bt == ASTBlock::BLK_FOR) {
                                    ancestorIsCompFor =
                                            tmp.top().cast<ASTIterBlock>()->isComprehension();
                                    break;
                                }
                                if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
                                    break;
                                tmp.pop();
                            }
                        }
                        if (ancestorIsCompFor) {
                            while ((curblock->blktype() == ASTBlock::BLK_IF
                                    || curblock->blktype() == ASTBlock::BLK_ELIF)
                                    && blocks.size() > 1) {
                                PycRef<ASTCondBlock> ifb = curblock.cast<ASTCondBlock>();
                                PycRef<ASTNode> c = ifb->cond();
                                if (ifb->negative())
                                    c = new ASTUnary(c, ASTUnary::UN_NOT);
                                blocks.pop();
                                curblock = blocks.top();
                                if (curblock->blktype() == ASTBlock::BLK_FOR) {
                                    PycRef<ASTNode> existing =
                                            curblock.cast<ASTIterBlock>()->condition();
                                    PycRef<ASTNode> combined = (existing == NULL) ? c
                                            : new ASTBinary(existing, c, ASTBinary::BIN_LOG_AND);
                                    curblock.cast<ASTIterBlock>()->setCondition(combined);
                                }
                                if (!stack_hist.empty())
                                    stack_hist.pop();
                            }
                        }
                    }

                    if (curblock->blktype() == ASTBlock::BLK_FOR) {
                        /* The jump operand is relative in 3.10+; the real target
                           is (pos - offs) where pos already points past this
                           instruction. Compare that against the loop start. A
                           for loop has TWO kinds of backward jumps to its start:
                           the implicit loop iteration at the very end of the
                           body (pos == block end), and an explicit `continue`
                           somewhere earlier (pos < block end). Only the former
                           closes the loop; the latter emits a continue. */
                        int target = (mod->verCompare(3, 11) >= 0)
                                     ? (pos - offs)
                                     : offs;   /* absolute up to 3.10 */
                        bool is_jump_to_start =
                                target == curblock.cast<ASTIterBlock>()->start();
                        bool at_loop_end = (curblock->end() != 0)
                                     && (pos == curblock->end());
                        if (!at_loop_end && is_jump_to_start && curblock->end() > pos
                                && !curblock.cast<ASTIterBlock>()->isComprehension()) {
                            /* The compiler may place a duplicated `return None`
                               epilogue (for an enclosing branch that skips the
                               loop) between the loop-back jump and the loop
                               exit. That still makes this jump the natural end
                               of the loop, not a `continue`. */
                            PycBuffer look(code->code()->value(), code->code()->length());
                            look.setPos(pos);
                            int lp = pos, lop = 0, lopnd = 0;
                            bool onlyEpilogue = true, sawReturn = false;
                            while (lp < curblock->end() && !look.atEof()) {
                                bc_next(look, mod, lop, lopnd, lp);
                                if (lop == Pyc::RETURN_VALUE || lop == Pyc::RETURN_CONST_A)
                                    sawReturn = true;
                                else if (lop != Pyc::LOAD_CONST_A && lop != Pyc::NOP
                                         && lop != Pyc::CACHE)
                                    onlyEpilogue = false;
                            }
                            if (onlyEpilogue && sawReturn && lp == curblock->end())
                                at_loop_end = true;
                        }
                        bool should_pop_for_block = curblock.cast<ASTIterBlock>()->isComprehension();
                        // in v3.8, SETUP_LOOP is deprecated and for blocks aren't terminated by POP_BLOCK, so we add them here
                        bool should_add_for_block = mod->majorVer() == 3 && mod->minorVer() >= 8 && is_jump_to_start && at_loop_end && !curblock.cast<ASTIterBlock>()->isComprehension();

                        if (!should_pop_for_block && !should_add_for_block
                                && is_jump_to_start && !at_loop_end
                                && !curblock.cast<ASTIterBlock>()->isComprehension()) {
                            /* explicit continue directly in the for body */
                            curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                            break;
                        }

                        if (should_pop_for_block || should_add_for_block) {
                            PycRef<ASTNode> top = stack.top();

                            if (top.type() == ASTNode::NODE_COMPREHENSION) {
                                PycRef<ASTComprehension> comp = top.cast<ASTComprehension>();

                                comp->addGenerator(curblock.cast<ASTIterBlock>());
                            }

                            PycRef<ASTBlock> tmp = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            if (should_add_for_block) {
                                curblock->append(tmp.cast<ASTNode>());
                            }
                        }
                    } else if (curblock->blktype() == ASTBlock::BLK_ELSE) {
                        /* A backward jump out of an else block is a `continue`
                           when it targets an enclosing loop and we are still
                           before that loop's end (the natural loop-back that
                           closes an else sits exactly at the loop end). Emit a
                           continue inside the else rather than closing it. */
                        if (mod->verCompare(3, 11) >= 0) {
                            int loopEnd = -1;
                            std::stack<PycRef<ASTBlock> > tmp = blocks;
                            while (!tmp.empty()) {
                                int bt = tmp.top()->blktype();
                                if (bt == ASTBlock::BLK_FOR
                                        || bt == ASTBlock::BLK_WHILE) {
                                    loopEnd = tmp.top()->end();
                                    break;
                                }
                                tmp.pop();
                            }
                            if (loopEnd > 0 && pos < loopEnd) {
                                curblock->append(
                                        new ASTKeyword(ASTKeyword::KW_CONTINUE));
                                break;
                            }
                        }
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }

                        blocks.pop();
                        blocks.top()->append(curblock.cast<ASTNode>());
                        curblock = blocks.top();

                        if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                                && !curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            blocks.pop();
                            blocks.top()->append(curblock.cast<ASTNode>());
                            curblock = blocks.top();
                        }
                    } else {
                        curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                    }

                    /* We're in a loop, this jumps back to the start */
                    /* I think we'll just ignore this case... */
                    break; // Bad idea? Probably!
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept() && pos < cont->except()) {
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, 0, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                } else {
                    fprintf(stderr, "Warning: Stack history is empty, something wrong might have happened\n");
                }

                /* Upstream fix (PR #415, "Fix pop from empty block stack"): if
                   we're down to the outermost block and there's still more
                   bytecode to parse, popping it here would underflow the block
                   stack (a hard crash). Bail out of this jump's block-closing
                   logic instead of popping past the bottom. */
                if (blocks.size() == 1 && !source.atEof()) {
                    fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                    cleanBuild = false;
                    break;
                }

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                do {
                    if (blocks.size() == 1) {
                        fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                        cleanBuild = false;
                        break;
                    }
                    blocks.pop();

                    blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF
                            || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, blocks.top()->end());
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, blocks.top()->end(), NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;
                    } else {
                        prev = nil;
                    }

                } while (prev != nil);

                curblock = blocks.top();
            }
            break;
        case Pyc::JUMP_FORWARD_A:
        case Pyc::INSTRUMENTED_JUMP_FORWARD_A:
            {
                /* Chained-comparison trailer: the JUMP_FORWARD that jumps over
                   the trampoline straight to the body. Skip it (and the
                   trampoline) without treating it as an if/else boundary. */
                if (chainSkipJumps.count(curpos)) {
                    int tgt = chainSkipJumps[curpos];
                    while (pos < tgt && !source.atEof()) {
                        curpos = pos;
                        bc_next(source, mod, opcode, operand, pos);
                    }
                    continue;
                }

                /* An if/elif with no else has no trailer jump of its own: its
                   merge point just sits wherever control naturally arrives.
                   When that merge point happens to be this exact instruction's
                   own start offset (curblock->end() == curpos, rather than the
                   usual curblock->end() == pos-after-a-real-trailer-jump), this
                   JUMP_FORWARD isn't the if's trailer -- it belongs to whatever
                   encloses it (e.g. a try body jumping over its own except
                   handler) and merely lands right after the if. Close the
                   already-finished if/elif plainly first, with no synthesized
                   else, so the logic below sees the real enclosing block. */
                while ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && curblock->end() == curpos
                        && blocks.size() > 1) {
                    PycRef<ASTBlock> fin = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(fin.cast<ASTNode>());
                    if (!stack_hist.empty()) {
                        stack_hist.pop();
                    }
                }

                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept()) {
                        stack_hist.push(stack);

                        curblock->setEnd(pos+offs);
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                if (!stack_hist.empty()) {
                    if (stack.empty()) // if it's part of if-expression, TOS at the moment is the result of "if" part
                        stack = stack_hist.top();
                    stack_hist.pop();
                }

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                do {
                    /* PR #415 guard: never pop the outermost block. */
                    if (blocks.size() <= 1) {
                        cleanBuild = false;
                        break;
                    }
                    blocks.pop();

                    if (!blocks.empty())
                        blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF
                            || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, pos+offs);
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;

                        if (prev->blktype() == ASTBlock::BLK_MAIN) {
                            /* Something went out of control! */
                            prev = nil;
                        }
                    } else if (prev->blktype() == ASTBlock::BLK_TRY
                            && prev->end() < pos+offs) {
                        /* Need to add an except/finally block */
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }

                        if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                            PycRef<ASTContainerBlock> cont = blocks.top().cast<ASTContainerBlock>();
                            if (cont->hasExcept()) {
                                if (push) {
                                    stack_hist.push(stack);
                                }

                                PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                                except->init();
                                blocks.push(except);
                            }
                        } else {
                            fprintf(stderr, "Something TERRIBLE happened!!\n");
                        }
                        prev = nil;
                    } else {
                        prev = nil;
                    }

                } while (prev != nil);

                if (!blocks.empty()) {
                    curblock = blocks.top();
                    if (curblock->blktype() == ASTBlock::BLK_EXCEPT)
                        curblock->setEnd(pos+offs);
                }
            }
            break;
        case Pyc::CALL_INTRINSIC_1_A:
            switch (operand) {
            case 2:   /* INTRINSIC_IMPORT_STAR */
                pendingOps.push_back(std::make_pair((int)Pyc::IMPORT_STAR, 0));
                break;
            case 3:   /* INTRINSIC_STOPITERATION_ERROR (generator cleanup) */
            case 4:   /* INTRINSIC_ASYNC_GEN_WRAP */
                break;
            case 5:   /* INTRINSIC_UNARY_POSITIVE */
                {
                    PycRef<ASTNode> arg = stack.top();
                    stack.pop();
                    stack.push(new ASTUnary(arg, ASTUnary::UN_POSITIVE));
                }
                break;
            case 6:   /* INTRINSIC_LIST_TO_TUPLE */
                pendingOps.push_back(std::make_pair((int)Pyc::LIST_TO_TUPLE, 0));
                break;
            default:
                fprintf(stderr, "Unsupported CALL_INTRINSIC_1 operand %d\n", operand);
                break;
            }
            break;
        case Pyc::LIST_TO_TUPLE:
            {
                /* Python 3.9+: convert the list at TOS into a tuple
                   (used when building *args for calls). */
                PycRef<ASTNode> list = stack.top();
                stack.pop();
                if (list.type() == ASTNode::NODE_LIST) {
                    const auto& lv = list.cast<ASTList>()->values();
                    ASTTuple::value_t tv(lv.begin(), lv.end());
                    stack.push(new ASTTuple(tv));
                } else {
                    stack.push(list);
                }
            }
            break;
        case Pyc::RETURN_GENERATOR:
            /* Python 3.11 generator/coroutine prologue; the pushed generator
               object is immediately discarded by a following POP_TOP. */
            stack.push(nullptr);
            break;
        case Pyc::MAP_ADD_A:
            {
                /* Python 3.8+: value at TOS, key at TOS1 (dict comprehensions). */
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                PycRef<ASTNode> key = stack.top();
                stack.pop();

                if (!inlineComps.empty() && curblock->blktype() != ASTBlock::BLK_FOR) {
                    /* Filtered inlined dict comprehension: keep the pair as a
                       yield marker; END_FOR rebuilds the comprehension. */
                    auto m = new ASTMap();
                    m->add(key, value);
                    curblock->append(new ASTReturn(m, ASTReturn::YIELD));
                } else if (InsideComprehensionAdd(blocks)) {
                    /* Dict comprehension (with or without an `if` filter --
                       when filtered, curblock is the filter's BLK_IF/BLK_ELIF
                       rather than the BLK_FOR itself; the backward-jump
                       handling above folds the filter condition into the
                       enclosing FOR block once it sees this comprehension
                       value land back on the stack). */
                    stack.pop();
                    auto m = new ASTMap();
                    m->add(key, value);
                    stack.push(new ASTComprehension(m));
                } else {
                    PycRef<ASTNode> map = stack.top();
                    if (map.type() == ASTNode::NODE_MAP)
                        map.cast<ASTMap>()->add(key, value);
                }
            }
            break;
        case Pyc::SET_ADD_A:
            {
                /* SET_ADD i: set.add(TOS1[-i], TOS). Used in set comprehensions;
                   mirrors LIST_APPEND_A's handling below but builds a set. */
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                PycRef<ASTNode> set = stack.top();

                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    stack.pop();
                    PycRef<ASTComprehension> sc = new ASTComprehension(value);
                    sc->setKind(ASTComprehension::KIND_SET);
                    stack.push(sc.cast<ASTNode>());
                } else {
                    const char* cn = code->name() != NULL ? code->name()->value() : NULL;
                    if ((cn && !strcmp(cn, "<setcomp>")) || !inlineComps.empty()) {
                        curblock->append(new ASTReturn(value, ASTReturn::YIELD));
                    } else {
                        stack.push(new ASTSubscr(set, value)); /* Total hack */
                    }
                }
            }
            break;
        case Pyc::LIST_APPEND:
        case Pyc::LIST_APPEND_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                PycRef<ASTNode> list = stack.top();


                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    stack.pop();
                    stack.push(new ASTComprehension(value));
                } else {
                    const char* cn = code->name() != NULL ? code->name()->value() : NULL;
                    if ((cn && !strcmp(cn, "<listcomp>")) || !inlineComps.empty()) {
                        /* Filtered list comprehension: the append is inside the
                           filter block, so record the value as a yield-style
                           marker and let SynthGenexpr rebuild the comprehension
                           (with its filter condition). */
                        curblock->append(new ASTReturn(value, ASTReturn::YIELD));
                    } else {
                        stack.push(new ASTSubscr(list, value)); /* Total hack */
                    }
                }
            }
            break;
        case Pyc::SET_UPDATE_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTSet> lhs = stack.top().cast<ASTSet>();
                stack.pop();

                ASTSet::value_t result = lhs->values();
                PycRef<PycObject> obj = rhs.type() == ASTNode::NODE_OBJECT
                        ? rhs.cast<ASTObject>()->object() : PycRef<PycObject>();
                if (obj != NULL && obj->type() == PycObject::TYPE_FROZENSET) {
                    for (const auto& it : obj.cast<PycSet>()->values())
                        result.push_back(new ASTObject(it));
                } else if (rhs.type() == ASTNode::NODE_SET) {
                    for (const auto& it : rhs.cast<ASTSet>()->values())
                        result.push_back(it);
                } else {
                    /* {a, *expr}: the elements aren't known at decompile time. */
                    result.push_back(new ASTUnary(rhs, ASTUnary::UN_UNPACK));
                }

                stack.push(new ASTSet(result));
            }
            break;
        case Pyc::DICT_UPDATE_A:
        case Pyc::DICT_MERGE_A:
            {
                /* Python 3.9+: merge mapping at TOS into the dict below it
                   (used for {**a, **b} literals and **kwargs construction). */
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTNode> lhsNode = stack.top();
                if (lhsNode.type() == ASTNode::NODE_MAP) {
                    PycRef<ASTMap> lhs = lhsNode.cast<ASTMap>();
                    if (rhs.type() == ASTNode::NODE_MAP) {
                        for (const auto& it : rhs.cast<ASTMap>()->values())
                            lhs->add(it.first, it.second);
                    } else {
                        /* Spread of a non-literal mapping (**expr); represent
                           with a NULL key so the source writer can emit "**expr". */
                        lhs->add(nullptr, rhs);
                    }
                }
                /* Leave the (updated) dict on the stack. */
            }
            break;
        case Pyc::LIST_EXTEND_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTList> lhs = stack.top().cast<ASTList>();
                stack.pop();

                ASTList::value_t result = lhs->values();

                if (rhs.type() == ASTNode::NODE_OBJECT
                        && (rhs.cast<ASTObject>()->object()->type() == PycObject::TYPE_TUPLE
                            || rhs.cast<ASTObject>()->object()->type() == PycObject::TYPE_SMALL_TUPLE)) {
                    /* [a, b, *CONST_TUPLE]: splice the constant elements in directly. */
                    for (const auto& it : rhs.cast<ASTObject>()->object().cast<PycTuple>()->values())
                        result.push_back(new ASTObject(it));
                } else if (rhs.type() == ASTNode::NODE_LIST || rhs.type() == ASTNode::NODE_TUPLE) {
                    /* [a, b, *[c, d]] / [a, b, *(c, d)]: splice the literal's elements. */
                    for (const auto& it : rhs.cast<ASTList>()->values())
                        result.push_back(it);
                } else {
                    /* [a, b, *some_expr()]: can't know the elements at decompile
                       time, so render it as a starred-unpack expression instead. */
                    result.push_back(new ASTUnary(rhs, ASTUnary::UN_UNPACK));
                }

                stack.push(new ASTList(result));
            }
            break;
        case Pyc::LOAD_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                if (name.type() != ASTNode::NODE_IMPORT) {
                    stack.pop();

                    if (mod->verCompare(3, 12) >= 0) {
                        if (operand & 1) {
                            /* Changed in version 3.12:
                            If the low bit of name is set, then a NULL or self is pushed to the stack
                            before the attribute or unbound method respectively. */
                            stack.push(nullptr);
                        }
                        operand >>= 1;
                    }

                    stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
                }
            }
            break;
        case Pyc::LOAD_COMMON_CONSTANT_A:
            {
                /* 3.14: 0 AssertionError, 1 NotImplementedError, 2 tuple,
                   3 all, 4 any */
                static const char* const names[] = {
                    "AssertionError", "NotImplementedError", "tuple", "all", "any"
                };
                if (operand == 0) {
                    pendingOps.push_back(std::make_pair((int)Pyc::LOAD_ASSERTION_ERROR, 0));
                } else if (operand >= 1 && operand <= 4) {
                    PycRef<PycString> nm = new PycString();
                    nm->setValue(names[operand]);
                    stack.push(new ASTName(nm));
                    if (operand >= 2)
                        commonGuard = true;
                } else {
                    fprintf(stderr, "Unsupported LOAD_COMMON_CONSTANT %d\n", operand);
                }
            }
            break;
        case Pyc::LOAD_ASSERTION_ERROR:
            {
                PycRef<PycString> assertName = new PycString();
                assertName->setValue("AssertionError");
                assertErrNode = new ASTName(assertName);
                stack.push(assertErrNode);
            }
            break;
        case Pyc::LOAD_BUILD_CLASS:
            stack.push(new ASTLoadBuildClass(new PycObject()));
            break;
        case Pyc::LOAD_CLOSURE_A:
            if (mod->verCompare(3, 6) >= 0) {
                /* In 3.6+ the closure cells are collected into a tuple via
                   BUILD_TUPLE and consumed by MAKE_FUNCTION; push a placeholder
                   so that tuple stays balanced (the value is later discarded). */
                stack.push(new ASTName(code->getCellVar(mod, operand)));
            }
            /* else: ignored (older bytecode) */
            break;
        case Pyc::MAKE_CELL_A:
        case Pyc::COPY_FREE_VARS_A:
            /* Python 3.11 function prologue; no stack effect for decompilation */
            break;
        case Pyc::LOAD_CONST_A:
            {
                PycRef<ASTObject> t_ob = new ASTObject(code->getConst(operand));

                if (t_ob->object().type() == PycObject::TYPE_SLICE) {
                    /* Python 3.14 constant-folds x[a:b:c] with constant bounds */
                    PycRef<PycSlice> sl = t_ob->object().cast<PycSlice>();
                    auto bound = [](const PycRef<PycObject>& o) -> PycRef<ASTNode> {
                        if (o == NULL || o.type() == PycObject::TYPE_NONE)
                            return NULL;
                        return new ASTObject(o);
                    };
                    PycRef<ASTNode> st = bound(sl->start()), en = bound(sl->stop()),
                                    sp = bound(sl->step());
                    PycRef<ASTNode> res;
                    if (st == NULL && en == NULL)
                        res = new ASTSlice(ASTSlice::SLICE0);
                    else if (st == NULL)
                        res = new ASTSlice(ASTSlice::SLICE2, st, en);
                    else if (en == NULL)
                        res = new ASTSlice(ASTSlice::SLICE1, st, en);
                    else
                        res = new ASTSlice(ASTSlice::SLICE3, st, en);
                    if (sp != NULL)
                        res = new ASTSlice(ASTSlice::SLICE3, res, sp);
                    stack.push(res);
                } else if ((t_ob->object().type() == PycObject::TYPE_TUPLE ||
                        t_ob->object().type() == PycObject::TYPE_SMALL_TUPLE) &&
                        !t_ob->object().cast<PycTuple>()->values().size()) {
                    ASTTuple::value_t values;
                    stack.push(new ASTTuple(values));
                } else if (t_ob->object().type() == PycObject::TYPE_NONE) {
                    stack.push(NULL);
                } else {
                    stack.push(t_ob.cast<ASTNode>());
                }
            }
            break;
        case Pyc::LOAD_DEREF_A:
        case Pyc::LOAD_CLASSDEREF_A:
            stack.push(new ASTName(code->getCellVar(mod, operand)));
            break;
        case Pyc::LOAD_FAST_AND_CLEAR_A:
            {
                /* Python 3.12+ inlined comprehension prologue:
                     GET_ITER; (LOAD_FAST_AND_CLEAR v)+; SWAP k+1;
                     BUILD_{LIST,SET,MAP} 0; SWAP 2; FOR_ITER ...
                   Model it as the pre-3.12 function form [container, iter]. */
                int k = 1;
                int op2 = 0, arg2 = 0;
                int savedSrc = source.pos();
                int savedPos = pos;
                bool ok = false;
                int container = 0;
                while (!source.atEof()) {
                    bc_next(source, mod, op2, arg2, pos);
                    if (op2 == Pyc::LOAD_FAST_AND_CLEAR_A) { k++; continue; }
                    if (op2 != Pyc::SWAP_A || arg2 != k + 1) break;
                    bc_next(source, mod, op2, arg2, pos);
                    if ((op2 != Pyc::BUILD_LIST_A && op2 != Pyc::BUILD_SET_A
                            && op2 != Pyc::BUILD_MAP_A) || arg2 != 0) break;
                    container = op2;
                    bc_next(source, mod, op2, arg2, pos);
                    ok = (op2 == Pyc::SWAP_A && arg2 == 2);
                    if (ok) {
                        /* 3.13+: GET_ITER of the iterable comes after the setup;
                           it acts on the same stack slot, leave it to the loop. */
                    }
                    break;
                }
                if (!ok) {
                    source.setPos(savedSrc);
                    pos = savedPos;
                    stack.push(nullptr);
                    break;
                }
                PycRef<ASTNode> iter = stack.top();
                stack.pop();
                inlineBase.push_back((int)stack.size());
                if (container == Pyc::BUILD_LIST_A)
                    stack.push(new ASTList(ASTList::value_t()));
                else if (container == Pyc::BUILD_SET_A)
                    stack.push(new ASTSet(ASTSet::value_t()));
                else
                    stack.push(new ASTMap);
                stack.push(iter);
                inlineComps.push_back(k);
                inlineKind.push_back(container == Pyc::BUILD_SET_A ? ASTComprehension::KIND_SET
                        : container == Pyc::BUILD_MAP_A ? ASTComprehension::KIND_DICT
                        : ASTComprehension::KIND_LIST);
            }
            break;
        case Pyc::LOAD_SMALL_INT_A:
            stack.push(new ASTObject(new PycInt(operand)));
            break;
        case Pyc::LOAD_FAST_A:
        case Pyc::LOAD_FAST_BORROW_A:
        case Pyc::LOAD_FAST_CHECK_A:
            if (mod->verCompare(1, 3) < 0)
                stack.push(new ASTName(code->getName(operand)));
            else
                stack.push(new ASTName(code->getLocal(operand)));
            break;
        case Pyc::LOAD_FAST_LOAD_FAST_A:
        case Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A:
            stack.push(new ASTName(code->getLocal(operand >> 4)));
            stack.push(new ASTName(code->getLocal(operand & 0xF)));
            break;
        case Pyc::LOAD_GLOBAL_A:
            if (mod->verCompare(3, 11) >= 0) {
                // Loads the global named co_names[namei>>1] onto the stack.
                if (operand & 1) {
                    /* Changed in version 3.11: 
                    If the low bit of "NAMEI" (operand) is set, 
                    then a NULL is pushed to the stack before the global variable. */
                    stack.push(nullptr);
                }
                operand >>= 1;
            }
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::LOAD_LOCALS:
            stack.push(new ASTNode(ASTNode::NODE_LOCALS));
            break;
        case Pyc::STORE_LOCALS:
            stack.pop();
            break;
        case Pyc::LOAD_METHOD_A:
            {
                // Behave like LOAD_ATTR
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
            }
            break;
        case Pyc::LOAD_NAME_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::MAKE_CLOSURE_A:
        case Pyc::MAKE_FUNCTION_A:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();

                /* Test for the qualified name of the function (at TOS) */
                int tos_type = fun_code.cast<ASTObject>()->object().type();
                if (tos_type != PycObject::TYPE_CODE &&
                    tos_type != PycObject::TYPE_CODE2) {
                    fun_code = stack.top();
                    stack.pop();
                }

                ASTFunction::defarg_t defArgs, kwDefArgs;

                if (mod->verCompare(3, 6) >= 0) {
                    /* Python 3.6+: the operand is a flag bitmask, and each set
                       flag has a single object pushed on the stack (bottom to
                       top): 0x01 defaults tuple, 0x02 kwonly-defaults dict,
                       0x04 annotations, 0x08 closure tuple. The code object was
                       already popped above. They must be consumed top-down. */
                    if (operand & 0x08) {
                        /* closure: tuple of cells - not needed for source */
                        stack.pop();
                    }
                    if (operand & 0x04) {
                        /* annotations - not reconstructed here */
                        stack.pop();
                    }
                    if (operand & 0x02) {
                        /* keyword-only defaults: a mapping on the stack */
                        PycRef<ASTNode> kwd = stack.top();
                        stack.pop();
                        if (kwd != NULL && kwd.type() == ASTNode::NODE_MAP) {
                            for (const auto& it : kwd.cast<ASTMap>()->values())
                                kwDefArgs.push_back(it.second);
                        } else if (kwd != NULL && kwd.type() == ASTNode::NODE_CONST_MAP) {
                            for (const auto& v : kwd.cast<ASTConstMap>()->values())
                                kwDefArgs.push_back(v);
                        }
                    }
                    if (operand & 0x01) {
                        /* positional defaults: a tuple on the stack */
                        PycRef<ASTNode> defs = stack.top();
                        stack.pop();
                        if (defs != NULL && defs.type() == ASTNode::NODE_TUPLE) {
                            for (const auto& v : defs.cast<ASTTuple>()->values())
                                defArgs.push_back(v);
                        } else if (defs != NULL && defs.type() == ASTNode::NODE_OBJECT) {
                            PycRef<PycObject> o = defs.cast<ASTObject>()->object();
                            if (o->type() == PycObject::TYPE_TUPLE
                                    || o->type() == PycObject::TYPE_SMALL_TUPLE) {
                                for (const auto& it : o.cast<PycTuple>()->values())
                                    defArgs.push_back(new ASTObject(it));
                            }
                        }
                    }
                } else {
                    const int defCount = operand & 0xFF;
                    const int kwDefCount = (operand >> 8) & 0xFF;
                    for (int i = 0; i < defCount; ++i) {
                        defArgs.push_front(stack.top());
                        stack.pop();
                    }
                    for (int i = 0; i < kwDefCount; ++i) {
                        kwDefArgs.push_front(stack.top());
                        stack.pop();
                    }
                }
                stack.push(new ASTFunction(fun_code, defArgs, kwDefArgs));
            }
            break;
        case Pyc::MAKE_FUNCTION:
            {
                /* Python 3.13+: MAKE_FUNCTION only takes the code object; the
                   defaults/closure/etc. are attached afterwards by
                   SET_FUNCTION_ATTRIBUTE. */
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                stack.push(new ASTFunction(fun_code, ASTFunction::defarg_t(),
                                           ASTFunction::defarg_t()));
            }
            break;
        case Pyc::SET_FUNCTION_ATTRIBUTE_A:
            {
                /* STACK[-1] is the function, STACK[-2] the attribute value.
                   Flags: 0x01 defaults, 0x02 kw-only defaults, 0x04
                   annotations, 0x08 closure, 0x10 annotate function. */
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                PycRef<ASTNode> attr = stack.top();
                stack.pop();
                if (func == NULL || func.type() != ASTNode::NODE_FUNCTION) {
                    stack.push(func);
                    break;
                }
                PycRef<ASTFunction> fn = func.cast<ASTFunction>();
                ASTFunction::defarg_t defArgs = fn->defargs();
                ASTFunction::defarg_t kwDefArgs = fn->kwdefargs();
                if (operand == 0x01) {
                    defArgs.clear();
                    if (attr != NULL && attr.type() == ASTNode::NODE_TUPLE) {
                        for (const auto& v : attr.cast<ASTTuple>()->values())
                            defArgs.push_back(v);
                    } else if (attr != NULL && attr.type() == ASTNode::NODE_OBJECT) {
                        PycRef<PycObject> o = attr.cast<ASTObject>()->object();
                        if (o->type() == PycObject::TYPE_TUPLE
                                || o->type() == PycObject::TYPE_SMALL_TUPLE) {
                            for (const auto& it : o.cast<PycTuple>()->values())
                                defArgs.push_back(new ASTObject(it));
                        }
                    }
                } else if (operand == 0x02) {
                    kwDefArgs.clear();
                    if (attr != NULL && attr.type() == ASTNode::NODE_MAP) {
                        for (const auto& it : attr.cast<ASTMap>()->values())
                            kwDefArgs.push_back(it.second);
                    } else if (attr != NULL && attr.type() == ASTNode::NODE_CONST_MAP) {
                        for (const auto& v : attr.cast<ASTConstMap>()->values())
                            kwDefArgs.push_back(v);
                    }
                }
                stack.push(new ASTFunction(fn->code(), defArgs, kwDefArgs));
            }
            break;
        case Pyc::POP_ITER:
            /* 3.14: the iterator is popped separately from END_FOR; the
               stack model already dropped it there. */
            break;
        case Pyc::NOT_TAKEN:
        case Pyc::NOP:
            break;
        case Pyc::POP_BLOCK:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER ||
                        curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    /* These should only be popped by an END_FINALLY */
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH) {
                    // This should only be popped by a WITH_CLEANUP
                    break;
                }

                if (curblock->nodes().size() &&
                        curblock->nodes().back().type() == ASTNode::NODE_KEYWORD) {
                    curblock->removeLast();
                }

                if (curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELIF
                        || curblock->blktype() == ASTBlock::BLK_ELSE
                        || curblock->blktype() == ASTBlock::BLK_TRY
                        || curblock->blktype() == ASTBlock::BLK_EXCEPT
                        || curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    if (!stack_hist.empty()) {
                        stack = stack_hist.top();
                        stack_hist.pop();
                    } else {
                        fprintf(stderr, "Warning: Stack history is empty, something wrong might have happened\n");
                    }
                }
                PycRef<ASTBlock> tmp = curblock;
                blocks.pop();

                if (!blocks.empty())
                    curblock = blocks.top();

                if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                        && tmp->nodes().size() == 0)) {
                    curblock->append(tmp.cast<ASTNode>());
                }

                if (tmp->blktype() == ASTBlock::BLK_FOR && tmp->end() >= pos) {
                    stack_hist.push(stack);

                    PycRef<ASTBlock> blkelse = new ASTBlock(ASTBlock::BLK_ELSE, tmp->end());
                    blocks.push(blkelse);
                    curblock = blocks.top();
                }

                if (curblock->blktype() == ASTBlock::BLK_TRY
                        && tmp->blktype() != ASTBlock::BLK_FOR
                        && tmp->blktype() != ASTBlock::BLK_ASYNCFOR
                        && tmp->blktype() != ASTBlock::BLK_WHILE) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    tmp = curblock;
                    blocks.pop();
                    curblock = blocks.top();

                    if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                            && tmp->nodes().size() == 0)) {
                        curblock->append(tmp.cast<ASTNode>());
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();

                    if (tmp->blktype() == ASTBlock::BLK_ELSE && !cont->hasFinally()) {

                        /* Pop the container */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());

                    } else if ((tmp->blktype() == ASTBlock::BLK_ELSE && cont->hasFinally())
                            || (tmp->blktype() == ASTBlock::BLK_TRY && !cont->hasExcept())) {

                        /* Add the finally block */
                        stack_hist.push(stack);

                        PycRef<ASTBlock> final = new ASTBlock(ASTBlock::BLK_FINALLY, 0, true);
                        blocks.push(final);
                        curblock = blocks.top();
                    }
                }

                if ((curblock->blktype() == ASTBlock::BLK_FOR || curblock->blktype() == ASTBlock::BLK_ASYNCFOR)
                        && curblock->end() == pos) {
                    blocks.pop();
                    blocks.top()->append(curblock.cast<ASTNode>());
                    curblock = blocks.top();
                }
            }
            break;
        case Pyc::POP_EXCEPT:
            /* Do nothing. */
            break;
        case Pyc::PUSH_EXC_INFO:
            /* Python 3.11+: pushes the live exception. Use a sentinel so the
               `as <var>` binding and CHECK_EXC_MATCH stay balanced; it is
               consumed by the following STORE (binding) or POP_TOP (discard). */
            {
                PycRef<PycString> s = new PycString();
                s->setValue(EXC_SENTINEL);
                stack.push(new ASTName(s));
            }
            break;
        case Pyc::CHECK_EXC_MATCH:
            {
                /* Python 3.11+: compare the exception (left, kept on the stack
                   for a possible `as` binding) against the handler type. */
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.push(new ASTCompare(left, right, ASTCompare::CMP_EXCEPTION));
            }
            break;
        case Pyc::END_FOR:
            {
                bool inlineCompLoop = !inlineComps.empty();
                if (inlineCompLoop
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && InsideComprehensionAdd(blocks)) {
                    /* close the filter block(s) and the comprehension's FOR */
                    while (blocks.size() > 1
                           && !(curblock->blktype() == ASTBlock::BLK_FOR
                                && curblock.cast<ASTIterBlock>()->isComprehension())) {
                        PycRef<ASTBlock> inner = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(inner.cast<ASTNode>());
                    }
                    if (curblock->blktype() == ASTBlock::BLK_FOR && blocks.size() > 1) {
                        PycRef<ASTBlock> fb = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(fb.cast<ASTNode>());
                    }
                }
                if (inlineCompLoop) {
                    /* Is this the last loop of the comprehension? Then what
                       follows is the restore of the saved loop variables:
                         [POP_ITER] SWAP k+1; STORE_FAST v (x k)
                       or, when the result is stored right away,
                         [POP_ITER] STORE_FAST target...; STORE_FAST v (x k) */
                    int k = inlineComps.back();
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0;
                    struct Op { int start, end, weight; };
                    std::vector<Op> seq;
                    bool finishing = false;
                    int lastP = pos;
                    if (!source.atEof()) {
                        bc_next(source, mod, o2, a2, pos);
                        if (o2 == Pyc::POP_ITER
                                || (o2 == Pyc::POP_TOP && mod->verCompare(3, 13) == 0)) {
                            /* the loop's own iterator pop is never real code */
                            cleanupSkip.push_back(std::make_pair(lastP, pos));
                            lastP = pos;
                            if (!source.atEof())
                                bc_next(source, mod, o2, a2, pos);
                        }
                    }
                    auto storeWeight = [](int op) {
                        return op == Pyc::STORE_FAST_A ? 1
                             : op == Pyc::STORE_FAST_STORE_FAST_A ? 2 : 0;
                    };
                    if (o2 == Pyc::SWAP_A && a2 == k + 1) {
                        seq.push_back({lastP, pos, 0});
                        int cnt = 0;
                        while (!source.atEof() && cnt < k) {
                            int st = pos;
                            bc_next(source, mod, o2, a2, pos);
                            int w = storeWeight(o2);
                            if (!w) break;
                            seq.push_back({st, pos, w});
                            cnt += w;
                        }
                        finishing = (cnt == k);
                    } else if (storeWeight(o2)) {
                        std::vector<Op> stores;
                        stores.push_back({lastP, pos, storeWeight(o2)});
                        while (!source.atEof()) {
                            int st = pos;
                            bc_next(source, mod, o2, a2, pos);
                            int w = storeWeight(o2);
                            if (!w) break;
                            stores.push_back({st, pos, w});
                        }
                        int total = 0;
                        for (const auto& st : stores) total += st.weight;
                        if (total >= k) {
                            finishing = true;
                            int need = k;
                            for (size_t i = stores.size(); i > 0 && need > 0; i--) {
                                const Op& st = stores[i - 1];
                                if (st.weight <= need) {
                                    seq.push_back(st);
                                    need -= st.weight;
                                } else {
                                    /* fused STORE_FAST_STORE_FAST (target, saved) */
                                    fusedKeepFirst.insert(st.start);
                                    need = 0;
                                }
                            }
                        }
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                    if (finishing) {
                        for (const auto& r : seq)
                            cleanupSkip.push_back(std::make_pair(r.start, r.end));
                        inlineComps.pop_back();
                        inlineFinish = true;
                        inlineFinishBase = inlineBase.back();
                        inlineBase.pop_back();
                        inlineFinishKind = inlineKind.back();
                        inlineKind.pop_back();
                    }
                }
                if (!inlineCompLoop)
                    stack.pop();

                if (!inlineCompLoop && (opcode == Pyc::END_FOR) && (mod->majorVer() == 3) && (mod->minorVer() == 12)) {
                    // one additional pop for python 3.12
                    stack.pop();
                }

                // end for loop here
                /* TODO : Ensure that FOR loop ends here. 
                   Due to CACHE instructions at play, the end indicated in
                   the for loop by pycdas is not correct, it is off by
                   some small amount. */
                if (inlineCompLoop) {
                    /* inlined comprehension: loops are closed by their
                       back-edges, nothing to do here */
                }
                else if (curblock->blktype() == ASTBlock::BLK_FOR
                        && (curblock->end() == curpos || curblock->end() == 0
                            || curblock->end() < curpos)) {
                    PycRef<ASTBlock> prev = blocks.top();
                    blocks.pop();

                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
                else if (curblock->blktype() == ASTBlock::BLK_FOR) {
                    /* The loop being closed here was already finished by its
                       closing JUMP_BACKWARD; this END_FOR belongs to it. */
                }
                else if (curblock->blktype() != ASTBlock::BLK_MAIN) {
                    /* (A BLK_MAIN here means the closing JUMP_BACKWARD already
                       ended the for block, which is the normal 3.12 shape.) */
                    fprintf(stderr, "Wrong block type %i for END_FOR\n", curblock->blktype());
                }

                if (inlineFinish) {
                    inlineFinish = false;
                    /* A filtered comprehension leaves its pieces as a yield
                       marker inside the FOR block; rebuild it from there. */
                    PycRef<ASTNode> res;
                    if (!stack.empty() && stack.top() != NULL
                            && stack.top().type() == ASTNode::NODE_COMPREHENSION) {
                        res = stack.top();
                    } else if (curblock->size() > 0) {
                        PycRef<ASTComprehension> synth =
                                SynthGenexpr(curblock->nodes().back());
                        if (synth != NULL) {
                            curblock->removeLast();
                            res = synth.cast<ASTNode>();
                        }
                    }
                    /* replace [container, loop-leftovers] by the result */
                    if (res != NULL && res.type() == ASTNode::NODE_COMPREHENSION)
                        res.cast<ASTComprehension>()->setKind(
                                (ASTComprehension::Kind)inlineFinishKind);
                    if (res != NULL && (int)stack.size() > inlineFinishBase) {
                        while ((int)stack.size() > inlineFinishBase)
                            stack.pop();
                        stack.push(res);
                    }
                }
            }
            break;
        case Pyc::POP_TOP:
            {
                if (!matches.empty() && matches.back().cblock == curblock
                        && matches.back().skipPop) {
                    /* Discards the subject of the match statement */
                    matches.back().skipPop = false;
                    matches.back().guardOpen = false;
                    break;
                }

                PycRef<ASTNode> value = stack.top();
                stack.pop();

                /* The live exception from PUSH_EXC_INFO is discarded here when
                   the handler has no `as` binding; never emit the sentinel. */
                if (isExcSentinel(value)) {
                    if (!curblock->inited())
                        curblock->init();
                    break;
                }

                if (!curblock->inited()) {
                    if (curblock->blktype() == ASTBlock::BLK_WITH) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                    } else {
                        curblock->init();
                    }
                    break;
                } else if (value == nullptr || value->processed()) {
                    break;
                }

                curblock->append(value);

                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    /* This relies on some really uncertain logic...
                     * If it's a comprehension, the only POP_TOP should be
                     * a call to append the iter to the list.
                     */
                    if (value.type() == ASTNode::NODE_CALL) {
                        auto& pparams = value.cast<ASTCall>()->pparams();
                        if (!pparams.empty()) {
                            PycRef<ASTNode> res = pparams.front();
                            stack.push(new ASTComprehension(res));
                        }
                    }
                }
            }
            break;
        case Pyc::PRINT_ITEM:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top()));
                stack.pop();
            }
            break;
        case Pyc::PRINT_ITEM_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top(), stream));
                stack.pop();
                if (stream)
                    stream->setProcessed();
            }
            break;
        case Pyc::PRINT_NEWLINE:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr));
                stack.pop();
            }
            break;
        case Pyc::PRINT_NEWLINE_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr, stream));
                stack.pop();
                if (stream)
                    stream->setProcessed();
            }
            break;
        case Pyc::RAISE_VARARGS_A:
            {
                ASTRaise::param_t paramList;
                for (int i = 0; i < operand; i++) {
                    paramList.push_front(stack.top());
                    stack.pop();
                }
                curblock->append(new ASTRaise(paramList));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && blocks.size() > 1
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
            }
            break;
        case Pyc::RERAISE:
        case Pyc::RERAISE_A:
            /* Python 3.11 cleanup opcode. */
            break;
        case Pyc::RETURN_VALUE:
        case Pyc::INSTRUMENTED_RETURN_VALUE_A:
            if (finallySuppressReturns.count(curpos)) {
                /* the try's own `return X`, already emitted in the try body */
                break;
            }
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && blocks.size() > 1
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());

                    /* A return inside an if/else is, in pre-3.11 bytecode,
                       followed by a redundant JUMP that skips the else branch;
                       that jump must be consumed. In 3.11+ the return usually
                       falls straight into real code (a sibling branch, the next
                       statement such as another `return`, or an exception
                       handler), and consuming it would silently drop that code.
                       Peek the next instruction and only skip it when it is an
                       actual unconditional jump; otherwise keep it. */
                    if (!source.atEof()) {
                        int savedSrc = source.pos();
                        int savedPos = pos;
                        int pkOp = 0, pkOperand = 0;
                        bc_next(source, mod, pkOp, pkOperand, pos);
                        bool keepNext = !(pkOp == Pyc::JUMP_FORWARD_A
                                       || pkOp == Pyc::JUMP_ABSOLUTE_A
                                       || pkOp == Pyc::JUMP_BACKWARD_A
                                       || pkOp == Pyc::JUMP_BACKWARD_NO_INTERRUPT_A);
                        if (keepNext) {
                            source.setPos(savedSrc);
                            pos = savedPos;
                        } else {
                            opcode = pkOp;
                            operand = pkOperand;
                        }
                    }
                }
            }
            break;
        case Pyc::RETURN_CONST_A:
        case Pyc::INSTRUMENTED_RETURN_CONST_A:
            {
                PycRef<ASTObject> value = new ASTObject(code->getConst(operand));
                curblock->append(new ASTReturn(value.cast<ASTNode>()));
            }
            break;
        case Pyc::ROT_TWO:
            {
                {
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0;
                    bool storeNext = false;
                    if (!source.atEof()) {
                        bc_next(source, mod, o2, a2, pos);
                        storeNext = (o2 == Pyc::STORE_FAST_A || o2 == Pyc::STORE_NAME_A
                                  || o2 == Pyc::STORE_GLOBAL_A || o2 == Pyc::STORE_DEREF_A);
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                    if (storeNext && stack.size() >= 2 && stack.top(1) != NULL
                            && stack.top(1).type() != ASTNode::NODE_CHAINSTORE
                            && stack.top() != NULL
                            && stack.top().type() != ASTNode::NODE_CHAINSTORE) {
                        /* `a, b = b, a`: parallel assignment */
                        pendingOps.push_back(std::make_pair((int)Pyc::SWAP_A, 2));
                        break;
                    }
                }
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> two = stack.top();
                stack.pop();

                stack.push(one);
                stack.push(two);
            }
            break;
        case Pyc::ROT_THREE:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::ROT_FOUR:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> four = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(four);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::SET_LINENO_A:
            // Ignore
            break;
        case Pyc::SETUP_WITH_A:
            {
                PycRef<ASTBlock> withblock = new ASTWithBlock(pos+operand);
                blocks.push(withblock);
                curblock = blocks.top();
            }
            break;
        case Pyc::WITH_EXCEPT_START:
            /* Python 3.11 with-statement exception cleanup: calls __exit__ with
               the live exception. Discard the exception sentinel that
               PUSH_EXC_INFO left on the stack and yield a None-like result so
               the implicit cleanup test does not leak the sentinel. */
            if (!stack.empty() && isExcSentinel(stack.top()))
                stack.pop();
            stack.push(nullptr);
            break;
        case Pyc::BEFORE_WITH:
        case Pyc::BEFORE_ASYNC_WITH:
            {
                /* Python 3.11 with-statement (BEFORE_ASYNC_WITH is the same
                   shape for `async with`, just entered via an awaited
                   __aenter__ -- the GET_AWAITABLE/SEND around it is handled
                   elsewhere and doesn't affect the block structure here).
                   If the pre-pass recognized this one, open a with-block; the
                   context manager stays on the stack and the following
                   STORE/POP_TOP turns it into the with-expression (and
                   optional `as <var>`). */
                auto it = withBodyEndByBefore.find(curpos);
                if (it != withBodyEndByBefore.end()) {
                    PycRef<ASTBlock> withblock = new ASTWithBlock(it->second);
                    blocks.push(withblock);
                    curblock = blocks.top();
                }
            }
            break;
        case Pyc::WITH_CLEANUP:
        case Pyc::WITH_CLEANUP_START:
            {
                // Stack top should be a None. Ignore it.
                PycRef<ASTNode> none = stack.top();
                stack.pop();

                if (none != NULL) {
                    fprintf(stderr, "Something TERRIBLE happened!\n");
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH
                        && curblock->end() == curpos) {
                    PycRef<ASTBlock> with = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(with.cast<ASTNode>());
                }
                else {
                    fprintf(stderr, "Something TERRIBLE happened! No matching with block found for WITH_CLEANUP at %d\n", curpos);
                }
            }
            break;
        case Pyc::WITH_CLEANUP_FINISH:
            /* Ignore this */
            break;
        case Pyc::SETUP_EXCEPT_A:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    curblock.cast<ASTContainerBlock>()->setExcept(pos+operand);
                } else {
                    PycRef<ASTBlock> next = new ASTContainerBlock(0, pos+operand);
                    blocks.push(next.cast<ASTBlock>());
                }

                /* Store the current stack for the except/finally statement(s) */
                stack_hist.push(stack);
                PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, pos+operand, true);
                blocks.push(tryblock.cast<ASTBlock>());
                curblock = blocks.top();

                need_try = false;
            }
            break;
        case Pyc::SETUP_FINALLY_A:
            {
                if (mod->verCompare(3, 8) >= 0 && mod->verCompare(3, 11) < 0) {
                    /* 3.8-3.10 use SETUP_FINALLY for both try/finally and
                       try/except. An except handler starts by testing the
                       exception (DUP_TOP) or discarding it (POP_TOP). */
                    int off = (mod->verCompare(3, 10) >= 0) ? operand * 2 : operand;
                    PycBuffer look(code->code()->value(), code->code()->length());
                    int target = pos + off;
                    if (target < (int)code->code()->length()) {
                        look.setPos(target);
                        int lop = 0, lopnd = 0, lp = target;
                        bc_next(look, mod, lop, lopnd, lp);
                        if (lop == Pyc::DUP_TOP || lop == Pyc::POP_TOP) {
                            pendingOps.push_back(std::make_pair((int)Pyc::SETUP_EXCEPT_A, off));
                            break;
                        }
                    }
                }
                PycRef<ASTBlock> next = new ASTContainerBlock(pos+operand);
                blocks.push(next.cast<ASTBlock>());
                curblock = blocks.top();

                need_try = true;
            }
            break;
        case Pyc::SETUP_LOOP_A:
            {
                PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_WHILE, pos+operand, NULL, false);
                blocks.push(next.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::SLICE_0:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE0);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_1:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE1, lower);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_2:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE2, NULL, upper);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::SLICE_3:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                PycRef<ASTNode> slice = new ASTSlice(ASTSlice::SLICE3, lower, upper);
                stack.push(new ASTSubscr(name, slice));
            }
            break;
        case Pyc::STORE_ATTR_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = stack.top();
                    stack.pop();
                    PycRef<ASTNode> attr = new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR);

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(attr);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();
                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> name = stack.top();
                    stack.pop();
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> attr = new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR);
                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, attr, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, attr));
                    }
                }
            }
            break;
        case Pyc::STORE_DEREF_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = new ASTName(code->getCellVar(mod, operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<PycString> derefName = code->getCellVar(mod, operand);
                    PycRef<ASTNode> name = new ASTName(derefName);
                    if (code->isFreeVar(mod, operand))
                        code->markNonlocal(derefName);   /* rebinding a free var */

                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }
            }
            break;
        case Pyc::STORE_FAST_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name;

                    if (mod->verCompare(1, 3) < 0)
                        name = new ASTName(code->getName(operand));
                    else
                        name = new ASTName(code->getLocal(operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> name;

                    if (mod->verCompare(1, 3) < 0)
                        name = new ASTName(code->getName(operand));
                    else
                        name = new ASTName(code->getLocal(operand));

                    if (name.cast<ASTName>()->name()->value()[0] == '_'
                            && name.cast<ASTName>()->name()->value()[1] == '[') {
                        /* Don't show stores of list comp append objects. */
                        break;
                    }

                    if (handleExceptBinding(curblock, value, name))
                        break;

                    if (curblock->blktype() == ASTBlock::BLK_FOR
                            && !curblock->inited()) {
                        curblock.cast<ASTIterBlock>()->setIndex(name);
                    } else if (!stack.empty() && stack.top() != NULL
                            && stack.top().type() == ASTNode::NODE_IMPORT) {
                        /* `from pkg import a as local_a` (or a bare `from pkg
                           import a`, where local_a == a): IMPORT_FROM left the
                           imported name on top of the still-live ASTImport
                           object (so chained `from x import a, b` can repeat
                           this for each name); fold this STORE into it as an
                           `as`-aliased store rather than emitting a standalone
                           assignment and leaving the import object to surface
                           later as a bare, nameless "import" statement (same
                           handling STORE_NAME_A already has below). */
                        PycRef<ASTImport> import = stack.top().cast<ASTImport>();
                        import->add_store(new ASTStore(value, name));
                    } else if (curblock->blktype() == ASTBlock::BLK_WITH
                                   && !curblock->inited()) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                        curblock.cast<ASTWithBlock>()->setVar(name);
                    } else if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }
            }
            break;
        case Pyc::STORE_GLOBAL_A:
            {
                PycRef<ASTNode> name = new ASTName(code->getName(operand));

                if (unpack) {
                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));
                    }
                }

                /* Mark the global as used */
                code->markGlobal(name.cast<ASTName>()->name());
            }
            break;
        case Pyc::STORE_NAME_A:
            {
                if (unpack) {
                    PycRef<ASTNode> name = new ASTName(code->getName(operand));

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(name);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();

                        if (curblock->blktype() == ASTBlock::BLK_FOR
                                && !curblock->inited()) {
                            PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                            if (tuple != NULL)
                                tuple->setRequireParens(false);
                            curblock.cast<ASTIterBlock>()->setIndex(tup);
                        } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();

                    PycRef<PycString> varname = code->getName(operand);
                    if (varname->length() >= 2 && varname->value()[0] == '_'
                            && varname->value()[1] == '[') {
                        /* Don't show stores of list comp append objects. */
                        break;
                    }

                    // Return private names back to their original name
                    const std::string class_prefix = std::string("_") + code->name()->strValue();
                    if (varname->startsWith(class_prefix + std::string("__")))
                        varname->setValue(varname->strValue().substr(class_prefix.size()));

                    PycRef<ASTNode> name = new ASTName(varname);

                    if (handleExceptBinding(curblock, value, name))
                        break;

                    if (curblock->blktype() == ASTBlock::BLK_FOR
                            && !curblock->inited()) {
                        curblock.cast<ASTIterBlock>()->setIndex(name);
                    } else if (stack.top().type() == ASTNode::NODE_IMPORT) {
                        PycRef<ASTImport> import = stack.top().cast<ASTImport>();

                        import->add_store(new ASTStore(value, name));
                    } else if (curblock->blktype() == ASTBlock::BLK_WITH
                               && !curblock->inited()) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                        curblock.cast<ASTWithBlock>()->setVar(name);
                    } else if (value.type() == ASTNode::NODE_CHAINSTORE) {
                        append_to_chain_store(value, name, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(value, name));

                        if (value.type() == ASTNode::NODE_INVALID)
                            break;
                    }
                }
            }
            break;
        case Pyc::STORE_SLICE_0:
            {
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE0))));
            }
            break;
        case Pyc::STORE_SLICE_1:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE1, upper))));
            }
            break;
        case Pyc::STORE_SLICE_2:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE2, NULL, lower))));
            }
            break;
        case Pyc::STORE_SLICE_3:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                curblock->append(new ASTStore(value, new ASTSubscr(dest, new ASTSlice(ASTSlice::SLICE3, upper, lower))));
            }
            break;
        case Pyc::STORE_SUBSCR:
            {
                if (unpack) {
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> dest = stack.top();
                    stack.pop();

                    PycRef<ASTNode> save = new ASTSubscr(dest, subscr);

                    PycRef<ASTNode> tup = stack.top();
                    if (tup.type() == ASTNode::NODE_TUPLE)
                        tup.cast<ASTTuple>()->add(save);
                    else
                        fputs("Something TERRIBLE happened!\n", stderr);

                    if (--unpack <= 0) {
                        stack.pop();
                        PycRef<ASTNode> seq = stack.top();
                        stack.pop();
                        if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(seq, tup, stack, curblock);
                        } else {
                            curblock->append(new ASTStore(seq, tup));
                        }
                    }
                } else {
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> dest = stack.top();
                    stack.pop();
                    PycRef<ASTNode> src = stack.top();
                    stack.pop();

                    // If variable annotations are enabled, we'll need to check for them here.
                    // Python handles a varaible annotation by setting:
                    // __annotations__['var-name'] = type
                    const bool found_annotated_var = (variable_annotations && dest->type() == ASTNode::Type::NODE_NAME
                                                      && dest.cast<ASTName>()->name()->isEqual("__annotations__"));

                    if (found_annotated_var) {
                        // Annotations can be done alone or as part of an assignment.
                        // In the case of an assignment, we'll see a NODE_STORE on the stack.
                        if (!curblock->nodes().empty() && curblock->nodes().back()->type() == ASTNode::Type::NODE_STORE) {
                            // Replace the existing NODE_STORE with a new one that includes the annotation.
                            PycRef<ASTStore> store = curblock->nodes().back().cast<ASTStore>();
                            curblock->removeLast();
                            curblock->append(new ASTStore(store->src(),
                                                          new ASTAnnotatedVar(subscr, src)));
                        } else {
                            curblock->append(new ASTAnnotatedVar(subscr, src));
                        }
                    } else {
                        if (dest.type() == ASTNode::NODE_MAP) {
                            dest.cast<ASTMap>()->add(subscr, src);
                        } else if (src.type() == ASTNode::NODE_CHAINSTORE) {
                            append_to_chain_store(src, new ASTSubscr(dest, subscr), stack, curblock);
                        } else {
                            curblock->append(new ASTStore(src, new ASTSubscr(dest, subscr)));
                        }
                    }
                }
            }
            break;
        case Pyc::UNARY_CALL:
            {
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                stack.push(new ASTCall(func, ASTCall::pparam_t(), ASTCall::kwparam_t()));
            }
            break;
        case Pyc::UNARY_CONVERT:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTConvert(name));
            }
            break;
        case Pyc::UNARY_INVERT:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_INVERT));
            }
            break;
        case Pyc::UNARY_NEGATIVE:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_NEGATIVE));
            }
            break;
        case Pyc::UNARY_NOT:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_NOT));
            }
            break;
        case Pyc::UNARY_POSITIVE:
            {
                PycRef<ASTNode> arg = stack.top();
                stack.pop();
                stack.push(new ASTUnary(arg, ASTUnary::UN_POSITIVE));
            }
            break;
        case Pyc::UNPACK_LIST_A:
        case Pyc::UNPACK_TUPLE_A:
        case Pyc::UNPACK_SEQUENCE_A:
            {
                unpack = operand;
                if (unpack > 0) {
                    ASTTuple::value_t vals;
                    stack.push(new ASTTuple(vals));
                } else {
                    // Unpack zero values and assign it to top of stack or for loop variable.
                    // E.g. [] = TOS / for [] in X
                    ASTTuple::value_t vals;
                    auto tup = new ASTTuple(vals);
                    if (curblock->blktype() == ASTBlock::BLK_FOR
                        && !curblock->inited()) {
                        tup->setRequireParens(true);
                        curblock.cast<ASTIterBlock>()->setIndex(tup);
                    } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                        auto chainStore = stack.top();
                        stack.pop();
                        append_to_chain_store(chainStore, tup, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(stack.top(), tup));
                        stack.pop();
                    }
                }
            }
            break;
        case Pyc::YIELD_FROM:
            {
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                // TODO: Support yielding into a non-null destination
                PycRef<ASTNode> value = stack.top();
                if (value) {
                    value->setProcessed();
                    curblock->append(new ASTReturn(value, ASTReturn::YIELD_FROM));
                }
            }
            break;
        case Pyc::SEND_A:
            {
                /* Python 3.11+ replaces YIELD_FROM with a SEND/YIELD_VALUE loop:
                   SEND(TOS1=iter/awaitable, TOS=value to send in, usually None
                   on the first pass) -- jumping forward over the loop body once
                   the iterator raises StopIteration. Unlike the old YIELD_FROM
                   (always a full statement by itself), SEND appears whether
                   `await`/`yield from` is used as a statement or as a
                   sub-expression, so just drop the sent-in value and leave the
                   awaitable/iterator on the stack as the expression's result --
                   whatever consumes it next (POP_TOP for a bare statement,
                   STORE_FAST for an assignment, ...) prints it once. */
                stack.pop();
            }
            break;
        case Pyc::YIELD_VALUE:
        case Pyc::YIELD_VALUE_A:
        case Pyc::INSTRUMENTED_YIELD_VALUE_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value, ASTReturn::YIELD));
            }
            break;
        case Pyc::SETUP_ANNOTATIONS:
            variable_annotations = true;
            break;
        case Pyc::PRECALL_A:
        case Pyc::RESUME_A:
        case Pyc::INSTRUMENTED_RESUME_A:
            /* We just entirely ignore this / no-op */
            break;
        case Pyc::CACHE:
            /* These "fake" opcodes are used as placeholders for optimizing
               certain opcodes in Python 3.11+.  Since we have no need for
               that during disassembly/decompilation, we can just treat these
               as no-ops. */
            break;
        case Pyc::PUSH_NULL:
            if (mod->verCompare(3, 13) >= 0 && !stack.empty()) {
                /* 3.13 pushes the NULL *above* the callable; keep the
                   internal [NULL, callable] order used for 3.11/3.12. */
                PycRef<ASTNode> callable = stack.top();
                stack.pop();
                stack.push(nullptr);
                stack.push(callable);
            } else {
                stack.push(nullptr);
            }
            break;
        case Pyc::GEN_START_A:
            stack.pop();
            break;
        case Pyc::SWAP_A:
            {
                /* Python 3.11+: swap TOS with the operand-th element from the
                   top. Chained comparisons use it as a real stack operation
                   (SWAP n; COPY n). Every other use is the reordering that
                   precedes a parallel assignment (`a, b = b, a`), which is
                   modelled as the construction of the unpack tuple. */
                int pkOp = 0, pkOperand = 0;
                int savedSrc = source.pos();
                int savedPos = pos;
                if (!source.atEof())
                    bc_next(source, mod, pkOp, pkOperand, pos);
                source.setPos(savedSrc);
                pos = savedPos;
                if (pkOp == Pyc::COPY_A || operand < 2
                        || (int)stack.size() < operand) {
                    stack.swap(operand);
                    break;
                }

                unpack = operand;
                ASTTuple::value_t values;
                ASTTuple::value_t next_tuple;
                values.resize(operand);
                for (int i = 0; i < operand; i++) {
                    values[operand - i - 1] = stack.top();
                    stack.pop();
                }
                auto tup = new ASTTuple(values);
                tup->setRequireParens(false);
                auto next_tup = new ASTTuple(next_tuple);
                next_tup->setRequireParens(false);
                stack.push(tup);
                stack.push(next_tup);
            }
            break;
        case Pyc::BINARY_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }
                stack.push(new ASTSubscr(dest, slice));
            }
            break;
        case Pyc::STORE_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> values = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }

                curblock->append(new ASTStore(values, new ASTSubscr(dest, slice)));
            }
            break;
        case Pyc::COPY_A:
            {
                PycRef<ASTNode> value = stack.top(operand);
                stack.push(value);
            }
            break;
        default:
            fprintf(stderr, "Unsupported opcode: %s (%d)\n", Pyc::OpcodeName(opcode), opcode);
            cleanBuild = false;
            return new ASTNodeList(defblock->nodes());
        }

        else_pop =  ( (curblock->blktype() == ASTBlock::BLK_ELSE)
                      || (curblock->blktype() == ASTBlock::BLK_IF)
                      || (curblock->blktype() == ASTBlock::BLK_ELIF) )
                 && (curblock->end() == pos);
    }

    while (!matches.empty()) {
        /* The code ended inside of a match statement (e.g. in its last case) */
        MatchCtx& mc = matches.back();
        while (blocks.size() > 1 && blocks.top() != mc.mblock.cast<ASTBlock>()) {
            PycRef<ASTBlock> top = blocks.top();
            blocks.pop();
            blocks.top()->append(top.cast<ASTNode>());
        }
        if (blocks.size() > 1) {
            PycRef<ASTBlock> mb = blocks.top();
            blocks.pop();
            blocks.top()->append(mb.cast<ASTNode>());
        }
        matches.pop_back();
    }

    if (stack_hist.size()) {
        fputs("Warning: Stack history is not empty!\n", stderr);

        while (stack_hist.size()) {
            stack_hist.pop();
        }
    }

    if (blocks.size() > 1) {
        fputs("Warning: block stack is not empty!\n", stderr);

        while (blocks.size() > 1) {
            PycRef<ASTBlock> tmp = blocks.top();
            blocks.pop();

            blocks.top()->append(tmp.cast<ASTNode>());
        }
    }

    cleanBuild = true;
    return new ASTNodeList(defblock->nodes());
}

static void append_to_chain_store(const PycRef<ASTNode> &chainStore,
        PycRef<ASTNode> item, FastStack& stack, const PycRef<ASTBlock>& curblock)
{
    stack.pop();    // ignore identical source object.
    chainStore.cast<ASTChainStore>()->append(item);
    if (stack.top().type() == PycObject::TYPE_NULL) {
        curblock->append(chainStore);
    } else {
        stack.push(chainStore);
    }
}

static int cmp_prec(PycRef<ASTNode> parent, PycRef<ASTNode> child)
{
    /* Determine whether the parent has higher precedence than therefore
       child, so we don't flood the source code with extraneous parens.
       Else we'd have expressions like (((a + b) + c) + d) when therefore
       equivalent, a + b + c + d would suffice. */

    if (parent.type() == ASTNode::NODE_UNARY && parent.cast<ASTUnary>()->op() == ASTUnary::UN_NOT)
        return 1;   // Always parenthesize not(x)
    if (child.type() == ASTNode::NODE_BINARY) {
        PycRef<ASTBinary> binChild = child.cast<ASTBinary>();
        if (parent.type() == ASTNode::NODE_BINARY) {
            PycRef<ASTBinary> binParent = parent.cast<ASTBinary>();
            if (binParent->right() == child) {
                if (binParent->op() == ASTBinary::BIN_SUBTRACT &&
                    binChild->op() == ASTBinary::BIN_ADD)
                    return 1;
                else if (binParent->op() == ASTBinary::BIN_DIVIDE &&
                         binChild->op() == ASTBinary::BIN_MULTIPLY)
                    return 1;
            }
            return binChild->op() - binParent->op();
        }
        else if (parent.type() == ASTNode::NODE_COMPARE)
            return (binChild->op() == ASTBinary::BIN_LOG_AND ||
                    binChild->op() == ASTBinary::BIN_LOG_OR) ? 1 : -1;
        else if (parent.type() == ASTNode::NODE_UNARY)
            return (binChild->op() == ASTBinary::BIN_POWER) ? -1 : 1;
    } else if (child.type() == ASTNode::NODE_UNARY) {
        PycRef<ASTUnary> unChild = child.cast<ASTUnary>();
        if (parent.type() == ASTNode::NODE_BINARY) {
            PycRef<ASTBinary> binParent = parent.cast<ASTBinary>();
            if (binParent->op() == ASTBinary::BIN_LOG_AND ||
                binParent->op() == ASTBinary::BIN_LOG_OR)
                return -1;
            else if (unChild->op() == ASTUnary::UN_NOT)
                return 1;
            else if (binParent->op() == ASTBinary::BIN_POWER)
                return 1;
            else
                return -1;
        } else if (parent.type() == ASTNode::NODE_COMPARE) {
            return (unChild->op() == ASTUnary::UN_NOT) ? 1 : -1;
        } else if (parent.type() == ASTNode::NODE_UNARY) {
            return unChild->op() - parent.cast<ASTUnary>()->op();
        }
    } else if (child.type() == ASTNode::NODE_COMPARE) {
        PycRef<ASTCompare> cmpChild = child.cast<ASTCompare>();
        if (parent.type() == ASTNode::NODE_BINARY)
            return (parent.cast<ASTBinary>()->op() == ASTBinary::BIN_LOG_AND ||
                    parent.cast<ASTBinary>()->op() == ASTBinary::BIN_LOG_OR) ? -1 : 1;
        else if (parent.type() == ASTNode::NODE_COMPARE)
            return cmpChild->op() - parent.cast<ASTCompare>()->op();
        else if (parent.type() == ASTNode::NODE_UNARY)
            return (parent.cast<ASTUnary>()->op() == ASTUnary::UN_NOT) ? -1 : 1;
    }

    /* For normal nodes, don't parenthesize anything */
    return -1;
}

static void print_ordered(PycRef<ASTNode> parent, PycRef<ASTNode> child,
                          PycModule* mod, std::ostream& pyc_output)
{
    if (child.type() == ASTNode::NODE_BINARY ||
        child.type() == ASTNode::NODE_COMPARE) {
        if (cmp_prec(parent, child) > 0) {
            pyc_output << "(";
            print_src(child, mod, pyc_output);
            pyc_output << ")";
        } else {
            print_src(child, mod, pyc_output);
        }
    } else if (child.type() == ASTNode::NODE_UNARY) {
        if (cmp_prec(parent, child) > 0) {
            pyc_output << "(";
            print_src(child, mod, pyc_output);
            pyc_output << ")";
        } else {
            print_src(child, mod, pyc_output);
        }
    } else {
        print_src(child, mod, pyc_output);
    }
}

static void start_line(int indent, std::ostream& pyc_output)
{
    if (inLambda)
        return;
    for (int i=0; i<indent; i++)
        pyc_output << "    ";
}

static void end_line(std::ostream& pyc_output)
{
    if (inLambda)
        return;
    pyc_output << "\n";
}

int cur_indent = -1;

/* Decorators (`@d def f(): ...`) compile to `f = d(<function>)`; the function
   is emitted as a plain def followed by `f = d(f)`. Recognize that pair so it
   can be printed back as decorator lines. */
static std::vector<PycRef<ASTNode> > pendingDecorators;

static bool matchDecoratedDef(const PycRef<ASTNode>& def, const PycRef<ASTNode>& assign,
                              std::vector<PycRef<ASTNode> >& decs)
{
    if (def == NULL || assign == NULL || def.type() != ASTNode::NODE_STORE
            || assign.type() != ASTNode::NODE_STORE)
        return false;
    PycRef<ASTStore> d = def.cast<ASTStore>();
    PycRef<ASTStore> a = assign.cast<ASTStore>();
    if (d->dest().type() != ASTNode::NODE_NAME || a->dest().type() != ASTNode::NODE_NAME)
        return false;
    if (d->src().type() == ASTNode::NODE_FUNCTION) {
        PycRef<PycCode> c = d->src().cast<ASTFunction>()->code()
                .cast<ASTObject>()->object().cast<PycCode>();
        if (c->name()->isEqual("<lambda>"))
            return false;
    } else if (d->src().type() != ASTNode::NODE_CLASS) {
        return false;
    }
    if (!d->dest().cast<ASTName>()->name()->isEqual(
            a->dest().cast<ASTName>()->name()->value()))
        return false;
    PycRef<ASTNode> node = a->src();
    std::vector<PycRef<ASTNode> > found;
    while (node != NULL && node.type() == ASTNode::NODE_CALL) {
        PycRef<ASTCall> c = node.cast<ASTCall>();
        if (c->pparams().size() != 1 || !c->kwparams().empty() || c->hasVar() || c->hasKW())
            return false;
        found.push_back(c->func());
        node = c->pparams().front();
    }
    if (found.empty() || node == NULL || node.type() != ASTNode::NODE_NAME
            || !node.cast<ASTName>()->name()->isEqual(
                    d->dest().cast<ASTName>()->name()->value()))
        return false;
    decs = found;
    return true;
}

static void print_pending_decorators(PycModule* mod, std::ostream& pyc_output);

static void print_block(PycRef<ASTBlock> blk, PycModule* mod,
                        std::ostream& pyc_output)
{
    ASTBlock::list_t lines = blk->nodes();

    if (lines.size() == 0) {
        PycRef<ASTNode> pass = new ASTKeyword(ASTKeyword::KW_PASS);
        start_line(cur_indent, pyc_output);
        print_src(pass, mod, pyc_output);
    }

    for (auto ln = lines.cbegin(); ln != lines.cend();) {
        bool skipNext = false;
        {
            auto nx = std::next(ln);
            std::vector<PycRef<ASTNode> > decs;
            if (nx != lines.cend() && matchDecoratedDef(*ln, *nx, decs)) {
                pendingDecorators = decs;
                skipNext = true;
            }
        }
        if ((*ln).cast<ASTNode>().type() != ASTNode::NODE_NODELIST) {
            start_line(cur_indent, pyc_output);
        }
        print_src(*ln, mod, pyc_output);
        ++ln;
        if (skipNext)
            ++ln;
        if (ln != lines.end()) {
            end_line(pyc_output);
        }
    }
}

static void print_pending_decorators(PycModule* mod, std::ostream& pyc_output)
{
    std::vector<PycRef<ASTNode> > decs;
    decs.swap(pendingDecorators);
    /* Applied inside-out in the bytecode; the outermost decorator comes
       first in the source. */
    for (const auto& d : decs) {
        start_line(cur_indent, pyc_output);
        pyc_output << "@";
        print_src(d, mod, pyc_output);
        pyc_output << "\n";
    }
}

void print_formatted_value(PycRef<ASTFormattedValue> formatted_value, PycModule* mod,
                           std::ostream& pyc_output)
{
    pyc_output << "{";
    print_src(formatted_value->val(), mod, pyc_output);

    switch (formatted_value->conversion() & ASTFormattedValue::CONVERSION_MASK) {
    case ASTFormattedValue::NONE:
        break;
    case ASTFormattedValue::STR:
        pyc_output << "!s";
        break;
    case ASTFormattedValue::REPR:
        pyc_output << "!r";
        break;
    case ASTFormattedValue::ASCII:
        pyc_output << "!a";
        break;
    }
    if (formatted_value->conversion() & ASTFormattedValue::HAVE_FMT_SPEC) {
        pyc_output << ":" << formatted_value->format_spec().cast<ASTObject>()->object().cast<PycString>()->value();
    }
    pyc_output << "}";
}

static void print_pattern(PycRef<ASTMatchPattern> pat, PycModule* mod, std::ostream& pyc_output)
{
    switch (pat->kind()) {
    case ASTMatchPattern::PAT_WILDCARD:
        pyc_output << "_";
        break;
    case ASTMatchPattern::PAT_CAPTURE:
        pyc_output << pat->name()->value();
        break;
    case ASTMatchPattern::PAT_VALUE:
        print_src(pat->expr(), mod, pyc_output);
        break;
    case ASTMatchPattern::PAT_STAR:
        pyc_output << "*" << (pat->name() != nullptr ? pat->name()->value() : "_");
        break;
    case ASTMatchPattern::PAT_SEQUENCE:
        {
            pyc_output << "[";
            bool first = true;
            for (const auto& sub : pat->subs()) {
                if (!first)
                    pyc_output << ", ";
                print_pattern(sub, mod, pyc_output);
                first = false;
            }
            pyc_output << "]";
        }
        break;
    case ASTMatchPattern::PAT_MAPPING:
        {
            pyc_output << "{";
            bool first = true;
            for (size_t i = 0; i < pat->keys().size(); i++) {
                if (!first)
                    pyc_output << ", ";
                print_src(pat->keys()[i], mod, pyc_output);
                pyc_output << ": ";
                print_pattern(pat->subs()[i], mod, pyc_output);
                first = false;
            }
            if (pat->name() != nullptr) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << "**" << pat->name()->value();
            }
            pyc_output << "}";
        }
        break;
    case ASTMatchPattern::PAT_CLASS:
        {
            print_src(pat->expr(), mod, pyc_output);
            pyc_output << "(";
            bool first = true;
            for (size_t i = 0; i < pat->subs().size(); i++) {
                if (!first)
                    pyc_output << ", ";
                if ((int)i >= pat->numPositional())
                    pyc_output << pat->kwNames()[i - pat->numPositional()]->value() << "=";
                print_pattern(pat->subs()[i], mod, pyc_output);
                first = false;
            }
            pyc_output << ")";
        }
        break;
    case ASTMatchPattern::PAT_OR:
        {
            bool first = true;
            for (const auto& sub : pat->subs()) {
                if (!first)
                    pyc_output << " | ";
                if (sub->kind() == ASTMatchPattern::PAT_AS) {
                    pyc_output << "(";
                    print_pattern(sub, mod, pyc_output);
                    pyc_output << ")";
                } else {
                    print_pattern(sub, mod, pyc_output);
                }
                first = false;
            }
        }
        break;
    case ASTMatchPattern::PAT_AS:
        if (pat->subs()[0]->kind() == ASTMatchPattern::PAT_OR
                || pat->subs()[0]->kind() == ASTMatchPattern::PAT_AS) {
            pyc_output << "(";
            print_pattern(pat->subs()[0], mod, pyc_output);
            pyc_output << ")";
        } else {
            print_pattern(pat->subs()[0], mod, pyc_output);
        }
        pyc_output << " as " << pat->name()->value();
        break;
    }
}

static std::unordered_set<ASTNode *> node_seen;

/* `*x` / `**x` operands: low-precedence expressions need parentheses
   (`**(a or {})`, `*(x if c else y)`), otherwise the output is invalid. */
static void print_unpack_operand(PycRef<ASTNode> node, PycModule* mod, std::ostream& pyc_output)
{
    bool paren = false;
    if (node != NULL) {
        if (node.type() == ASTNode::NODE_TERNARY || node.type() == ASTNode::NODE_COMPARE)
            paren = true;
        else if (node.type() == ASTNode::NODE_BINARY
                && node.cast<ASTBinary>()->op() != ASTBinary::BIN_ATTR)
            paren = true;
        else if (node.type() == ASTNode::NODE_UNARY
                && node.cast<ASTUnary>()->op() == ASTUnary::UN_NOT)
            paren = true;
    }
    if (paren) pyc_output << "(";
    print_src(node, mod, pyc_output);
    if (paren) pyc_output << ")";
}

void print_src(PycRef<ASTNode> node, PycModule* mod, std::ostream& pyc_output)
{
    if (node == NULL) {
        pyc_output << "None";
        cleanBuild = true;
        return;
    }

    if (node_seen.find((ASTNode *)node) != node_seen.end()) {
        fputs("WARNING: Circular reference detected\n", stderr);
        return;
    }
    node_seen.insert((ASTNode *)node);

    switch (node->type()) {
    case ASTNode::NODE_BINARY:
    case ASTNode::NODE_COMPARE:
        {
            PycRef<ASTBinary> bin = node.cast<ASTBinary>();
            print_ordered(node, bin->left(), mod, pyc_output);
            pyc_output << bin->op_str();
            print_ordered(node, bin->right(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_UNARY:
        {
            PycRef<ASTUnary> un = node.cast<ASTUnary>();
            pyc_output << un->op_str();
            print_ordered(node, un->operand(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_CALL:
        {
            PycRef<ASTCall> call = node.cast<ASTCall>();
            print_src(call->func(), mod, pyc_output);
            pyc_output << "(";
            bool first = true;
            for (const auto& param : call->pparams()) {
                if (!first)
                    pyc_output << ", ";
                lambdaBare = (param.type() == ASTNode::NODE_FUNCTION);
                /* f(x for x in y): the sole generator argument needs no
                   extra parentheses. */
                genexprBare = (call->pparams().size() == 1
                        && call->kwparams().empty() && !call->hasVar() && !call->hasKW()
                        && param != NULL && param.type() == ASTNode::NODE_COMPREHENSION
                        && param.cast<ASTComprehension>()->kind()
                                == ASTComprehension::KIND_GENERATOR);
                print_src(param, mod, pyc_output);
                genexprBare = false;
                lambdaBare = false;
                first = false;
            }
            for (const auto& param : call->kwparams()) {
                if (!first)
                    pyc_output << ", ";
                if (param.first.type() == ASTNode::NODE_NAME) {
                    pyc_output << param.first.cast<ASTName>()->name()->value() << " = ";
                } else {
                    PycRef<PycString> str_name = param.first.cast<ASTObject>()->object().cast<PycString>();
                    pyc_output << str_name->value() << " = ";
                }
                lambdaBare = (param.second.type() == ASTNode::NODE_FUNCTION);
                print_src(param.second, mod, pyc_output);
                lambdaBare = false;
                first = false;
            }
            if (call->hasVar()) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << "*";
                print_unpack_operand(call->var(), mod, pyc_output);
                first = false;
            }
            if (call->hasKW()) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << "**";
                print_unpack_operand(call->kw(), mod, pyc_output);
                first = false;
            }
            pyc_output << ")";
        }
        break;
    case ASTNode::NODE_DELETE:
        {
            pyc_output << "del ";
            print_src(node.cast<ASTDelete>()->value(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_EXEC:
        {
            PycRef<ASTExec> exec = node.cast<ASTExec>();
            pyc_output << "exec ";
            print_src(exec->statement(), mod, pyc_output);

            if (exec->globals() != NULL) {
                pyc_output << " in ";
                print_src(exec->globals(), mod, pyc_output);

                if (exec->locals() != NULL
                        && exec->globals() != exec->locals()) {
                    pyc_output << ", ";
                    print_src(exec->locals(), mod, pyc_output);
                }
            }
        }
        break;
    case ASTNode::NODE_FORMATTEDVALUE:
        pyc_output << "f" F_STRING_QUOTE;
        print_formatted_value(node.cast<ASTFormattedValue>(), mod, pyc_output);
        pyc_output << F_STRING_QUOTE;
        break;
    case ASTNode::NODE_JOINEDSTR:
        pyc_output << "f" F_STRING_QUOTE;
        for (const auto& val : node.cast<ASTJoinedStr>()->values()) {
            switch (val.type()) {
            case ASTNode::NODE_FORMATTEDVALUE:
                print_formatted_value(val.cast<ASTFormattedValue>(), mod, pyc_output);
                break;
            case ASTNode::NODE_OBJECT:
                // When printing a piece of the f-string, keep the quote style consistent.
                // This avoids problems when ''' or """ is part of the string.
                print_const(pyc_output, val.cast<ASTObject>()->object(), mod, F_STRING_QUOTE);
                break;
            default:
                /* Any other node type (e.g. a bare NODE_NAME/NODE_CALL that ended
                   up in the values list without being wrapped in a FormattedValue)
                   is a formatted expression -- wrap it in { } like one. */
                pyc_output << "{";
                print_src(val, mod, pyc_output);
                pyc_output << "}";
                break;
            }
        }
        pyc_output << F_STRING_QUOTE;
        break;
    case ASTNode::NODE_KEYWORD:
        pyc_output << node.cast<ASTKeyword>()->word_str();
        break;
    case ASTNode::NODE_LIST:
        {
            pyc_output << "[";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTList>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                print_src(val, mod, pyc_output);
                first = false;
            }
            cur_indent--;
            pyc_output << "]";
        }
        break;
    case ASTNode::NODE_SET:
        {
            pyc_output << "{";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTSet>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                print_src(val, mod, pyc_output);
                first = false;
            }
            cur_indent--;
            pyc_output << "}";
        }
        break;
    case ASTNode::NODE_COMPREHENSION:
        {
            PycRef<ASTComprehension> comp = node.cast<ASTComprehension>();
            bool is_dict = (comp->result() != NULL
                    && comp->result().type() == ASTNode::NODE_MAP)
                    || comp->kind() == ASTComprehension::KIND_DICT;
            bool is_set = comp->kind() == ASTComprehension::KIND_SET;
            bool is_gen = comp->kind() == ASTComprehension::KIND_GENERATOR;
            const char* open = (is_dict || is_set) ? "{ " : is_gen ? "( " : "[ ";
            const char* close = (is_dict || is_set) ? " }" : is_gen ? " )" : " ]";
            if (is_gen && genexprBare) {
                open = "";
                close = "";
                genexprBare = false;
            }

            pyc_output << open;
            if (is_dict) {
                const auto& entries = comp->result().cast<ASTMap>()->values();
                if (!entries.empty()) {
                    print_src(entries.front().first, mod, pyc_output);
                    pyc_output << ": ";
                    print_src(entries.front().second, mod, pyc_output);
                }
            } else {
                print_src(comp->result(), mod, pyc_output);
            }

            for (const auto& gen : comp->generators()) {
                pyc_output << " for ";
                print_src(gen->index(), mod, pyc_output);
                pyc_output << " in ";
                print_src(gen->iter(), mod, pyc_output);
                if (gen->condition()) {
                    pyc_output << " if ";
                    print_src(gen->condition(), mod, pyc_output);
                }
            }
            pyc_output << close;
        }
        break;
    case ASTNode::NODE_MAP:
        {
            pyc_output << "{";
            bool first = true;
            cur_indent++;
            for (const auto& val : node.cast<ASTMap>()->values()) {
                if (first)
                    pyc_output << "\n";
                else
                    pyc_output << ",\n";
                start_line(cur_indent, pyc_output);
                if (val.first == NULL) {
                    /* Dictionary unpacking: {**expr} */
                    pyc_output << "**";
                    print_unpack_operand(val.second, mod, pyc_output);
                } else {
                    print_src(val.first, mod, pyc_output);
                    pyc_output << ": ";
                    print_src(val.second, mod, pyc_output);
                }
                first = false;
            }
            cur_indent--;
            pyc_output << " }";
        }
        break;
    case ASTNode::NODE_CONST_MAP:
        {
            PycRef<ASTConstMap> const_map = node.cast<ASTConstMap>();
            PycTuple::value_t keys = const_map->keys().cast<ASTObject>()->object().cast<PycTuple>()->values();
            ASTConstMap::values_t values = const_map->values();

            auto map = new ASTMap;
            for (const auto& key : keys) {
                // Values are pushed onto the stack in reverse order.
                PycRef<ASTNode> value = values.back();
                values.pop_back();

                map->add(new ASTObject(key), value);
            }

            print_src(map, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_NAME:
        pyc_output << node.cast<ASTName>()->name()->value();
        break;
    case ASTNode::NODE_MATCH_PATTERN:
        print_pattern(node.cast<ASTMatchPattern>(), mod, pyc_output);
        break;
    case ASTNode::NODE_NODELIST:
        {
            cur_indent++;
            const auto& nl = node.cast<ASTNodeList>()->nodes();
            for (auto it = nl.cbegin(); it != nl.cend(); ++it) {
                const auto& ln = *it;
                auto nx = std::next(it);
                std::vector<PycRef<ASTNode> > decs;
                bool skipNext = nx != nl.cend() && matchDecoratedDef(ln, *nx, decs);
                if (skipNext)
                    pendingDecorators = decs;
                if (ln.cast<ASTNode>().type() != ASTNode::NODE_NODELIST) {
                    start_line(cur_indent, pyc_output);
                }
                print_src(ln, mod, pyc_output);
                end_line(pyc_output);
                if (skipNext)
                    ++it;
            }
            cur_indent--;
        }
        break;
    case ASTNode::NODE_BLOCK:
        {
            PycRef<ASTBlock> blk = node.cast<ASTBlock>();
            if (blk->blktype() == ASTBlock::BLK_ELSE && blk->size() == 0)
                break;

            if (blk->blktype() == ASTBlock::BLK_CONTAINER) {
                end_line(pyc_output);
                PycRef<ASTBlock> elseBlk = blk.cast<ASTContainerBlock>()->elseBlock();
                if (elseBlk == NULL) {
                    print_block(blk, mod, pyc_output);
                } else {
                    /* print: try, excepts, else, finally */
                    PycRef<ASTBlock> tmpBlk = new ASTBlock(ASTBlock::BLK_CONTAINER, 0);
                    ASTBlock::list_t before, after;
                    bool seenFinally = false;
                    for (const auto& c : blk->nodes()) {
                        if (c.type() == ASTNode::NODE_BLOCK
                                && c.cast<ASTBlock>()->blktype() == ASTBlock::BLK_FINALLY)
                            seenFinally = true;
                        (seenFinally ? after : before).push_back(c);
                    }
                    for (const auto& c : before) tmpBlk->append(c);
                    tmpBlk->append(elseBlk.cast<ASTNode>());
                    for (const auto& c : after) tmpBlk->append(c);
                    print_block(tmpBlk, mod, pyc_output);
                }
                end_line(pyc_output);
                break;
            }

            /* `if not cond: raise AssertionError[(msg)]` is how `assert` is
               compiled; print it back as an assert statement. */
            if (blk->blktype() == ASTBlock::BLK_IF && blk->size() == 1
                    && blk->nodes().front().type() == ASTNode::NODE_RAISE
                    && blk.cast<ASTCondBlock>()->negative()) {
                const auto& rp = blk->nodes().front().cast<ASTRaise>()->params();
                PycRef<ASTNode> exc = rp.size() == 1 ? rp.front() : PycRef<ASTNode>();
                PycRef<ASTNode> msg;
                bool isAssert = false;
                auto isAssertName = [](const PycRef<ASTNode>& n) {
                    return n != NULL && n.type() == ASTNode::NODE_NAME
                           && n.cast<ASTName>()->name()->isEqual("AssertionError");
                };
                if (isAssertName(exc)) {
                    isAssert = true;
                } else if (exc != NULL && exc.type() == ASTNode::NODE_CALL) {
                    PycRef<ASTCall> c = exc.cast<ASTCall>();
                    if (isAssertName(c->func()) && c->pparams().size() == 1
                            && c->kwparams().empty() && !c->hasVar() && !c->hasKW()) {
                        isAssert = true;
                        msg = c->pparams().front();
                    }
                }
                if (isAssert) {
                    pyc_output << "assert ";
                    print_src(blk.cast<ASTCondBlock>()->cond(), mod, pyc_output);
                    if (msg != NULL) {
                        pyc_output << ", ";
                        print_src(msg, mod, pyc_output);
                    }
                    break;
                }
            }

            pyc_output << blk->type_str();
            if (blk->blktype() == ASTBlock::BLK_IF
                    || blk->blktype() == ASTBlock::BLK_ELIF
                    || blk->blktype() == ASTBlock::BLK_WHILE) {
                if (blk.cast<ASTCondBlock>()->negative())
                    pyc_output << " not ";
                else
                    pyc_output << " ";

                print_src(blk.cast<ASTCondBlock>()->cond(), mod, pyc_output);
            } else if (blk->blktype() == ASTBlock::BLK_FOR || blk->blktype() == ASTBlock::BLK_ASYNCFOR) {
                pyc_output << " ";
                print_src(blk.cast<ASTIterBlock>()->index(), mod, pyc_output);
                pyc_output << " in ";
                print_src(blk.cast<ASTIterBlock>()->iter(), mod, pyc_output);
            } else if (blk->blktype() == ASTBlock::BLK_EXCEPT &&
                    blk.cast<ASTCondBlock>()->cond() != NULL) {
                pyc_output << " ";
                print_src(blk.cast<ASTCondBlock>()->cond(), mod, pyc_output);
                PycRef<ASTNode> excVar = blk.cast<ASTCondBlock>()->exceptVar();
                if (excVar != NULL) {
                    pyc_output << " as ";
                    print_src(excVar, mod, pyc_output);
                }
            } else if (blk->blktype() == ASTBlock::BLK_MATCH) {
                pyc_output << " ";
                print_src(blk.cast<ASTMatchBlock>()->subject(), mod, pyc_output);
            } else if (blk->blktype() == ASTBlock::BLK_CASE) {
                pyc_output << " ";
                print_src(blk.cast<ASTCaseBlock>()->pattern().cast<ASTNode>(), mod, pyc_output);
                if (blk.cast<ASTCaseBlock>()->guard() != nullptr) {
                    pyc_output << " if ";
                    print_src(blk.cast<ASTCaseBlock>()->guard(), mod, pyc_output);
                }
            } else if (blk->blktype() == ASTBlock::BLK_WITH) {
                pyc_output << " ";
                print_src(blk.cast<ASTWithBlock>()->expr(), mod, pyc_output);
                PycRef<ASTNode> var = blk.try_cast<ASTWithBlock>()->var();
                if (var != NULL) {
                    pyc_output << " as ";
                    print_src(var, mod, pyc_output);
                }
            }
            pyc_output << ":\n";

            cur_indent++;
            print_block(blk, mod, pyc_output);
            cur_indent--;
        }
        break;
    case ASTNode::NODE_OBJECT:
        {
            PycRef<PycObject> obj = node.cast<ASTObject>()->object();
            if (obj.type() == PycObject::TYPE_CODE) {
                PycRef<PycCode> code = obj.cast<PycCode>();
                decompyle(code, mod, pyc_output);
            } else {
                print_const(pyc_output, obj, mod);
            }
        }
        break;
    case ASTNode::NODE_PRINT:
        {
            pyc_output << "print ";
            bool first = true;
            if (node.cast<ASTPrint>()->stream() != nullptr) {
                pyc_output << ">>";
                print_src(node.cast<ASTPrint>()->stream(), mod, pyc_output);
                first = false;
            }

            for (const auto& val : node.cast<ASTPrint>()->values()) {
                if (!first)
                    pyc_output << ", ";
                print_src(val, mod, pyc_output);
                first = false;
            }
            if (!node.cast<ASTPrint>()->eol())
                pyc_output << ",";
        }
        break;
    case ASTNode::NODE_RAISE:
        {
            PycRef<ASTRaise> raise = node.cast<ASTRaise>();
            pyc_output << "raise";
            if (mod->verCompare(3, 0) >= 0) {
                /* Python 3: `raise`, `raise X`, or `raise X from Y`. */
                int i = 0;
                for (const auto& param : raise->params()) {
                    pyc_output << (i == 0 ? " " : " from ");
                    print_src(param, mod, pyc_output);
                    ++i;
                }
            } else {
                /* Python 2: `raise [X [, Y [, Z]]]`. */
                bool first = true;
                for (const auto& param : raise->params()) {
                    pyc_output << (first ? " " : ", ");
                    print_src(param, mod, pyc_output);
                    first = false;
                }
            }
        }
        break;
    case ASTNode::NODE_RETURN:
        {
            PycRef<ASTReturn> ret = node.cast<ASTReturn>();
            PycRef<ASTNode> value = ret->value();
            if (!inLambda) {
                switch (ret->rettype()) {
                case ASTReturn::RETURN:
                    pyc_output << "return ";
                    break;
                case ASTReturn::YIELD:
                    pyc_output << "yield ";
                    break;
                case ASTReturn::YIELD_FROM:
                    if (value.type() == ASTNode::NODE_AWAITABLE) {
                        pyc_output << "await ";
                        value = value.cast<ASTAwaitable>()->expression();
                    } else {
                        pyc_output << "yield from ";
                    }
                    break;
                }
            }
            print_src(value, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_SLICE:
        {
            PycRef<ASTSlice> slice = node.cast<ASTSlice>();

            if (slice->op() & ASTSlice::SLICE1) {
                print_src(slice->left(), mod, pyc_output);
            }
            pyc_output << ":";
            if (slice->op() & ASTSlice::SLICE2) {
                print_src(slice->right(), mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_IMPORT:
        {
            PycRef<ASTImport> import = node.cast<ASTImport>();
            /* `import a.b as c` compiles to IMPORT_NAME a.b (fromlist None)
               followed by IMPORT_FROM b; STORE c. */
            bool plainImportAs = import->stores().size() == 1
                    && (import->fromlist() == NULL
                        || (import->fromlist().type() == ASTNode::NODE_OBJECT
                            && import->fromlist().cast<ASTObject>()->object() == Pyc_None))
                    && import->name().type() == ASTNode::NODE_NAME;
            if (plainImportAs) {
                pyc_output << "import ";
                print_src(import->name(), mod, pyc_output);
                pyc_output << " as ";
                print_src(import->stores().front()->dest(), mod, pyc_output);
            } else if (import->stores().size()) {
                ASTImport::list_t stores = import->stores();

                pyc_output << "from ";
                /* Relative import: `from .`, `from ..`, `from .pkg`, etc.
                   co_names[namei] is "" for a bare `from . import x`, so the
                   dots alone (no trailing name) are correct in that case. */
                bool nestedImport = import->name().type() == ASTNode::NODE_IMPORT;
                int level = nestedImport ? import->name().cast<ASTImport>()->level()
                                          : import->level();
                for (int i = 0; i < level; i++)
                    pyc_output << ".";
                if (nestedImport)
                    print_src(import->name().cast<ASTImport>()->name(), mod, pyc_output);
                else
                    print_src(import->name(), mod, pyc_output);
                pyc_output << " import ";

                if (stores.size() == 1) {
                    auto src = stores.front()->src();
                    auto dest = stores.front()->dest();
                    print_src(src, mod, pyc_output);

                    if (src.cast<ASTName>()->name()->value() != dest.cast<ASTName>()->name()->value()) {
                        pyc_output << " as ";
                        print_src(dest, mod, pyc_output);
                    }
                } else {
                    bool first = true;
                    for (const auto& st : stores) {
                        if (!first)
                            pyc_output << ", ";
                        print_src(st->src(), mod, pyc_output);
                        first = false;

                        if (st->src().cast<ASTName>()->name()->value() != st->dest().cast<ASTName>()->name()->value()) {
                            pyc_output << " as ";
                            print_src(st->dest(), mod, pyc_output);
                        }
                    }
                }
            } else {
                pyc_output << "import ";
                print_src(import->name(), mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_FUNCTION:
        {
            /* Actual named functions are NODE_STORE with a name */
            const bool bare = lambdaBare;
            lambdaBare = false;
            pyc_output << (bare ? "lambda " : "(lambda ");
            PycRef<ASTNode> code = node.cast<ASTFunction>()->code();
            PycRef<PycCode> code_src = code.cast<ASTObject>()->object().cast<PycCode>();
            ASTFunction::defarg_t defargs = node.cast<ASTFunction>()->defargs();
            ASTFunction::defarg_t kwdefargs = node.cast<ASTFunction>()->kwdefargs();
            auto da = defargs.cbegin();
            int narg = 0;
            for (int i=0; i<code_src->argCount(); i++) {
                if (narg)
                    pyc_output << ", ";
                pyc_output << code_src->getLocal(narg++)->value();
                if ((code_src->argCount() - i) <= (int)defargs.size()) {
                    pyc_output << " = ";
                    print_src(*da++, mod, pyc_output);
                }
            }
            da = kwdefargs.cbegin();
            if (code_src->kwOnlyArgCount() != 0) {
                pyc_output << (narg == 0 ? "*" : ", *");
                for (int i = 0; i < code_src->argCount(); i++) {
                    pyc_output << ", ";
                    pyc_output << code_src->getLocal(narg++)->value();
                    if ((code_src->kwOnlyArgCount() - i) <= (int)kwdefargs.size()) {
                        pyc_output << " = ";
                        print_src(*da++, mod, pyc_output);
                    }
                }
            }
            pyc_output << ": ";

            inLambda = true;
            print_src(code, mod, pyc_output);
            inLambda = false;

            if (!bare)
                pyc_output << ")";
        }
        break;
    case ASTNode::NODE_STORE:
        {
            PycRef<ASTNode> src = node.cast<ASTStore>()->src();
            PycRef<ASTNode> dest = node.cast<ASTStore>()->dest();
            if (src.type() == ASTNode::NODE_FUNCTION) {
                PycRef<ASTNode> code = src.cast<ASTFunction>()->code();
                PycRef<PycCode> code_src = code.cast<ASTObject>()->object().cast<PycCode>();
                bool isLambda = false;

                if (strcmp(code_src->name()->value(), "<lambda>") == 0) {
                    pyc_output << "\n";
                    start_line(cur_indent, pyc_output);
                    print_src(dest, mod, pyc_output);
                    pyc_output << " = lambda ";
                    isLambda = true;
                } else {
                    pyc_output << "\n";
                    print_pending_decorators(mod, pyc_output);
                    start_line(cur_indent, pyc_output);
                    if (code_src->flags() & PycCode::CO_COROUTINE)
                        pyc_output << "async ";
                    pyc_output << "def ";
                    print_src(dest, mod, pyc_output);
                    pyc_output << "(";
                }

                ASTFunction::defarg_t defargs = src.cast<ASTFunction>()->defargs();
                ASTFunction::defarg_t kwdefargs = src.cast<ASTFunction>()->kwdefargs();
                /* Python signature order is: positional-or-keyword, *args,
                   keyword-only, **kwargs. Local variables, however, are ordered
                   positional, keyword-only, *args, **kwargs, so index them
                   explicitly rather than sequentially. */
                const int argC = code_src->argCount();
                const int kwC = code_src->kwOnlyArgCount();
                const bool hasVarArgs = (code_src->flags() & PycCode::CO_VARARGS) != 0;
                const bool hasVarKw = (code_src->flags() & PycCode::CO_VARKEYWORDS) != 0;
                bool firstParam = true;
                auto da = defargs.cbegin();
                for (int i = 0; i < argC; ++i) {
                    if (!firstParam) pyc_output << ", ";
                    firstParam = false;
                    pyc_output << code_src->getLocal(i)->value();
                    if ((argC - i) <= (int)defargs.size()) {
                        pyc_output << " = ";
                        print_src(*da++, mod, pyc_output);
                    }
                    if (code_src->posOnlyArgCount() > 0
                            && i == code_src->posOnlyArgCount() - 1)
                        pyc_output << ", /";
                }
                if (hasVarArgs) {
                    if (!firstParam) pyc_output << ", ";
                    firstParam = false;
                    pyc_output << "*" << code_src->getLocal(argC + kwC)->value();
                } else if (kwC != 0) {
                    if (!firstParam) pyc_output << ", ";
                    firstParam = false;
                    pyc_output << "*";
                }
                auto kda = kwdefargs.cbegin();
                for (int i = 0; i < kwC; ++i) {
                    if (!firstParam) pyc_output << ", ";
                    firstParam = false;
                    pyc_output << code_src->getLocal(argC + i)->value();
                    if ((kwC - i) <= (int)kwdefargs.size()) {
                        pyc_output << " = ";
                        print_src(*kda++, mod, pyc_output);
                    }
                }
                if (hasVarKw) {
                    if (!firstParam) pyc_output << ", ";
                    firstParam = false;
                    pyc_output << "**"
                               << code_src->getLocal(argC + kwC + (hasVarArgs ? 1 : 0))->value();
                }

                if (isLambda) {
                    pyc_output << ": ";
                } else {
                    pyc_output << "):\n";
                    printDocstringAndGlobals = true;
                }

                bool preLambda = inLambda;
                inLambda |= isLambda;

                print_src(code, mod, pyc_output);

                inLambda = preLambda;
            } else if (src.type() == ASTNode::NODE_CLASS) {
                pyc_output << "\n";
                print_pending_decorators(mod, pyc_output);
                start_line(cur_indent, pyc_output);
                pyc_output << "class ";
                print_src(dest, mod, pyc_output);
                PycRef<ASTTuple> bases = src.cast<ASTClass>()->bases().cast<ASTTuple>();
                ASTCall::kwparam_t classKw;
                {
                    PycRef<ASTNode> classCall = src.cast<ASTClass>()->code();
                    if (classCall != NULL && classCall.type() == ASTNode::NODE_CALL)
                        classKw = classCall.cast<ASTCall>()->kwparams();
                }
                if (bases->values().size() > 0 || !classKw.empty()) {
                    pyc_output << "(";
                    bool first = true;
                    for (const auto& val : bases->values()) {
                        if (!first)
                            pyc_output << ", ";
                        print_src(val, mod, pyc_output);
                        first = false;
                    }
                    for (const auto& kv : classKw) {
                        if (!first)
                            pyc_output << ", ";
                        if (kv.first.type() == ASTNode::NODE_NAME) {
                            pyc_output << kv.first.cast<ASTName>()->name()->value() << " = ";
                        } else {
                            pyc_output << kv.first.cast<ASTObject>()->object()
                                          .cast<PycString>()->value() << " = ";
                        }
                        print_src(kv.second, mod, pyc_output);
                        first = false;
                    }
                    pyc_output << "):\n";
                } else {
                    // Don't put parens if there are no base classes
                    pyc_output << ":\n";
                }
                printClassDocstring = true;
                PycRef<ASTNode> code = src.cast<ASTClass>()->code().cast<ASTCall>()
                                       ->func().cast<ASTFunction>()->code();
                print_src(code, mod, pyc_output);
            } else if (src.type() == ASTNode::NODE_IMPORT) {
                PycRef<ASTImport> import = src.cast<ASTImport>();
                if (import->fromlist() != NULL) {
                    PycRef<PycObject> fromlist = import->fromlist().cast<ASTObject>()->object();
                    if (fromlist != Pyc_None) {
                        pyc_output << "from ";
                        bool nestedImport = import->name().type() == ASTNode::NODE_IMPORT;
                        int level = nestedImport ? import->name().cast<ASTImport>()->level()
                                                  : import->level();
                        for (int i = 0; i < level; i++)
                            pyc_output << ".";
                        if (nestedImport)
                            print_src(import->name().cast<ASTImport>()->name(), mod, pyc_output);
                        else
                            print_src(import->name(), mod, pyc_output);
                        pyc_output << " import ";
                        if (fromlist.type() == PycObject::TYPE_TUPLE ||
                                fromlist.type() == PycObject::TYPE_SMALL_TUPLE) {
                            bool first = true;
                            for (const auto& val : fromlist.cast<PycTuple>()->values()) {
                                if (!first)
                                    pyc_output << ", ";
                                pyc_output << val.cast<PycString>()->value();
                                first = false;
                            }
                        } else {
                            pyc_output << fromlist.cast<PycString>()->value();
                        }
                    } else {
                        pyc_output << "import ";
                        print_src(import->name(), mod, pyc_output);
                    }
                } else {
                    pyc_output << "import ";
                    PycRef<ASTNode> import_name = import->name();
                    print_src(import_name, mod, pyc_output);
                    /* `import a.b.c` binds `a`: that is the plain form. */
                    std::string full = import_name.cast<ASTName>()->name()->strValue();
                    std::string firstPart = full.substr(0, full.find('.'));
                    if (!dest.cast<ASTName>()->name()->isEqual(import_name.cast<ASTName>()->name().cast<PycObject>())
                            && dest.cast<ASTName>()->name()->strValue() != firstPart) {
                        pyc_output << " as ";
                        print_src(dest, mod, pyc_output);
                    }
                }
            } else if (src.type() == ASTNode::NODE_BINARY
                    && src.cast<ASTBinary>()->is_inplace()) {
                print_src(src, mod, pyc_output);
            } else {
                print_src(dest, mod, pyc_output);
                pyc_output << " = ";
                print_src(src, mod, pyc_output);
            }
        }
        break;
    case ASTNode::NODE_CHAINSTORE:
        {
            for (auto& dest : node.cast<ASTChainStore>()->nodes()) {
                print_src(dest, mod, pyc_output);
                pyc_output << " = ";
            }
            print_src(node.cast<ASTChainStore>()->src(), mod, pyc_output);
        }
        break;
    case ASTNode::NODE_SUBSCR:
        {
            print_src(node.cast<ASTSubscr>()->name(), mod, pyc_output);
            pyc_output << "[";
            print_src(node.cast<ASTSubscr>()->key(), mod, pyc_output);
            pyc_output << "]";
        }
        break;
    case ASTNode::NODE_CONVERT:
        {
            pyc_output << "`";
            print_src(node.cast<ASTConvert>()->name(), mod, pyc_output);
            pyc_output << "`";
        }
        break;
    case ASTNode::NODE_TUPLE:
        {
            PycRef<ASTTuple> tuple = node.cast<ASTTuple>();
            ASTTuple::value_t values = tuple->values();
            if (tuple->requireParens())
                pyc_output << "(";
            bool first = true;
            for (const auto& val : values) {
                if (!first)
                    pyc_output << ", ";
                print_src(val, mod, pyc_output);
                first = false;
            }
            if (values.size() == 1)
                pyc_output << ',';
            if (tuple->requireParens())
                pyc_output << ')';
        }
        break;
    case ASTNode::NODE_ANNOTATED_VAR:
        {
            PycRef<ASTAnnotatedVar> annotated_var = node.cast<ASTAnnotatedVar>();
            PycRef<ASTObject> name = annotated_var->name().cast<ASTObject>();
            PycRef<ASTNode> annotation = annotated_var->annotation();

            pyc_output << name->object().cast<PycString>()->value();
            pyc_output << ": ";
            print_src(annotation, mod, pyc_output);
        }
        break;
    case ASTNode::NODE_TERNARY:
        {
            /* parenthesis might be needed
             * 
             * when if-expr is part of numerical expression, ternary has the LOWEST precedence
             *     print(a + b if False else c)
             * output is c, not a+c (a+b is calculated first)
             * 
             * but, let's not add parenthesis - to keep the source as close to original as possible in most cases
             */
            PycRef<ASTTernary> ternary = node.cast<ASTTernary>();
            //pyc_output << "(";
            print_src(ternary->if_expr(), mod, pyc_output);
            const auto if_block = ternary->if_block().cast<ASTCondBlock>();
            pyc_output << " if ";
            if (if_block->negative())
                pyc_output << "not ";
            print_src(if_block->cond(), mod, pyc_output);
            pyc_output << " else ";
            print_src(ternary->else_expr(), mod, pyc_output);
            //pyc_output << ")";
        }
        break;
    case ASTNode::NODE_AWAITABLE:
        /* `await <expr>` used directly as an expression (e.g. assigned to a
           variable or passed as a call argument) rather than consumed by the
           SEND/YIELD_FROM handling above. */
        pyc_output << "await ";
        print_src(node.cast<ASTAwaitable>()->expression(), mod, pyc_output);
        break;
    default:
        pyc_output << "<NODE:" << node->type() << ">";
        fprintf(stderr, "Unsupported Node type: %d\n", node->type());
        cleanBuild = false;
        node_seen.erase((ASTNode *)node);
        return;
    }

    cleanBuild = true;
    node_seen.erase((ASTNode *)node);
}

bool print_docstring(PycRef<PycObject> obj, int indent, PycModule* mod,
                     std::ostream& pyc_output)
{
    // docstrings are translated from the bytecode __doc__ = 'string' to simply '''string'''
    auto doc = obj.try_cast<PycString>();
    if (doc != nullptr) {
        start_line(indent, pyc_output);
        doc->print(pyc_output, mod, true);
        pyc_output << "\n";
        return true;
    }
    return false;
}

static std::unordered_set<PycCode *> code_seen;

/* A `return` is invalid at module scope, yet the implicit "return None" (and,
   with nested `if`s, copies of it) can end up inside module-level blocks. Strip
   the trailing bare return from this block and, recursively, from every nested
   block (descending into all children, not just the last). Only ever called on
   the <module> code object, whose blocks are module-level control flow (nested
   function/class bodies are separate code objects, so this never touches a real
   `return`). */
/* Python 3.x emits a `return None` at the end of every exit path of a
   function (each except handler, if/else branch, ...). When that path is the
   very last thing in the function the return is implicit, so drop it. Only
   the trailing statement chain is touched: loops are left alone. */
static bool isReturnNone(const PycRef<ASTNode>& n)
{
    if (n == NULL || n.type() != ASTNode::NODE_RETURN
            || n.cast<ASTReturn>()->rettype() != ASTReturn::RETURN)
        return false;
    PycRef<ASTNode> v = n.cast<ASTReturn>()->value();
    if (v == NULL)
        return true;
    if (v.type() == ASTNode::NODE_OBJECT)
        return v.cast<ASTObject>()->object().type() == PycObject::TYPE_NONE;
    return false;
}

static void StripTailReturnNone(PycRef<ASTNode> node)
{
    if (node == NULL)
        return;
    const ASTNodeList::list_t* nodes = NULL;
    if (node.type() == ASTNode::NODE_NODELIST)
        nodes = &node.cast<ASTNodeList>()->nodes();
    else if (node.type() == ASTNode::NODE_BLOCK)
        nodes = &node.cast<ASTBlock>()->nodes();
    if (nodes == NULL || nodes->empty())
        return;

    PycRef<ASTNode> last = nodes->back();
    if (isReturnNone(last)) {
        if (node.type() == ASTNode::NODE_NODELIST)
            node.cast<ASTNodeList>()->removeLast();
        else
            node.cast<ASTBlock>()->removeLast();
        return;
    }
    if (last.type() != ASTNode::NODE_BLOCK)
        return;

    auto strippable = [](const PycRef<ASTBlock>& b) {
        switch (b->blktype()) {
        case ASTBlock::BLK_IF: case ASTBlock::BLK_ELIF: case ASTBlock::BLK_ELSE:
        case ASTBlock::BLK_TRY: case ASTBlock::BLK_EXCEPT: case ASTBlock::BLK_FINALLY:
        case ASTBlock::BLK_WITH: case ASTBlock::BLK_CONTAINER:
            return true;
        default:
            return false;
        }
    };

    /* Walk the trailing run of sibling blocks that form one statement
       (if/elif/else chain, or try/except/else/finally). */
    auto it = nodes->end();
    while (it != nodes->begin()) {
        --it;
        if (it->type() != ASTNode::NODE_BLOCK)
            break;
        PycRef<ASTBlock> b = it->cast<ASTBlock>();
        if (!strippable(b))
            break;
        bool hasElseOrFinally = false;
        if (b->blktype() == ASTBlock::BLK_CONTAINER) {
            for (const auto& c : b->nodes()) {
                if (c.type() == ASTNode::NODE_BLOCK
                        && (c.cast<ASTBlock>()->blktype() == ASTBlock::BLK_ELSE
                            || c.cast<ASTBlock>()->blktype() == ASTBlock::BLK_FINALLY))
                    hasElseOrFinally = true;
            }
            /* A container's children are try/except/else/finally blocks. */
            if (!hasElseOrFinally) {
                for (const auto& c : b->nodes())
                    if (c.type() == ASTNode::NODE_BLOCK)
                        StripTailReturnNone(c);
            } else {
                PycRef<ASTNode> lastChild = b->nodes().back();
                if (lastChild.type() == ASTNode::NODE_BLOCK
                        && lastChild.cast<ASTBlock>()->blktype() == ASTBlock::BLK_ELSE)
                    StripTailReturnNone(lastChild);
            }
        } else {
            StripTailReturnNone(*it);
        }
        if (b->blktype() == ASTBlock::BLK_IF || b->blktype() == ASTBlock::BLK_TRY
                || b->blktype() == ASTBlock::BLK_WITH || b->blktype() == ASTBlock::BLK_CONTAINER)
            break;
    }
}

static void StripModuleTrailingReturn(PycRef<ASTNode> node)
{
    if (node == NULL)
        return;
    const ASTNodeList::list_t* nodes = NULL;
    if (node.type() == ASTNode::NODE_NODELIST)
        nodes = &node.cast<ASTNodeList>()->nodes();
    else if (node.type() == ASTNode::NODE_BLOCK)
        nodes = &node.cast<ASTBlock>()->nodes();
    if (nodes == NULL || nodes->empty())
        return;
    /* Recurse into every nested block first. */
    for (const auto& child : *nodes) {
        if (child.type() == ASTNode::NODE_BLOCK)
            StripModuleTrailingReturn(child);
    }
    /* Then strip this block's own trailing return. Any `return` is invalid at
       module scope (the only ones that appear are reconstruction artifacts), so
       drop it regardless of the returned value. */
    if (!nodes->empty() && nodes->back().type() == ASTNode::NODE_RETURN
            && nodes->back().cast<ASTReturn>()->rettype() == ASTReturn::RETURN) {
        if (node.type() == ASTNode::NODE_NODELIST)
            node.cast<ASTNodeList>()->removeLast();
        else
            node.cast<ASTBlock>()->removeLast();
    }
}

void decompyle(PycRef<PycCode> code, PycModule* mod, std::ostream& pyc_output)
{
    if (code_seen.find((PycCode *)code) != code_seen.end()) {
        fputs("WARNING: Circular reference detected\n", stderr);
        return;
    }
    code_seen.insert((PycCode *)code);

    PycRef<ASTNode> source = BuildFromCode(code, mod);

    PycRef<ASTNodeList> clean = source.cast<ASTNodeList>();
    if (cleanBuild) {
        // The Python compiler adds some stuff that we don't really care
        // about, and would add extra code for re-compilation anyway.
        // We strip these lines out here, and then add a "pass" statement
        // if the cleaned up code is empty
        if (clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->src().type() == ASTNode::NODE_NAME
                    && store->dest().type() == ASTNode::NODE_NAME) {
                PycRef<ASTName> src = store->src().cast<ASTName>();
                PycRef<ASTName> dest = store->dest().cast<ASTName>();
                if (src->name()->isEqual("__name__")
                        && dest->name()->isEqual("__module__")) {
                    // __module__ = __name__
                    // Automatically added by Python 2.2.1 and later
                    clean->removeFirst();
                }
            }
        }
        if (clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->src().type() == ASTNode::NODE_OBJECT
                    && store->dest().type() == ASTNode::NODE_NAME) {
                PycRef<ASTObject> src = store->src().cast<ASTObject>();
                PycRef<PycString> srcString = src->object().try_cast<PycString>();
                PycRef<ASTName> dest = store->dest().cast<ASTName>();
                if (dest->name()->isEqual("__qualname__")) {
                    // __qualname__ = '<Class Name>'
                    // Automatically added by Python 3.3 and later
                    clean->removeFirst();
                }
            }
        }

        // Class and module docstrings may only appear at the beginning of their source
        if (printClassDocstring && clean->nodes().front().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> store = clean->nodes().front().cast<ASTStore>();
            if (store->dest().type() == ASTNode::NODE_NAME &&
                    store->dest().cast<ASTName>()->name()->isEqual("__doc__") &&
                    store->src().type() == ASTNode::NODE_OBJECT) {
                if (print_docstring(store->src().cast<ASTObject>()->object(),
                        cur_indent + (code->name()->isEqual("<module>") ? 0 : 1), mod, pyc_output))
                    clean->removeFirst();
            }
        }
        if (clean->nodes().back().type() == ASTNode::NODE_BLOCK
                && clean->nodes().back().cast<ASTBlock>()->blktype() == ASTBlock::BLK_MATCH) {
            /* A match that ends the code: its cases return None instead of
             * jumping to the end */
            for (const auto& cn : clean->nodes().back().cast<ASTBlock>()->nodes()) {
                if (cn.type() != ASTNode::NODE_BLOCK)
                    continue;
                PycRef<ASTBlock> cb = cn.cast<ASTBlock>();
                if (cb->blktype() != ASTBlock::BLK_CASE || cb->nodes().empty()
                        || cb->nodes().back().type() != ASTNode::NODE_RETURN)
                    continue;
                PycRef<ASTReturn> r = cb->nodes().back().cast<ASTReturn>();
                PycRef<ASTObject> ro = r->value().try_cast<ASTObject>();
                if (r->value() == NULL || (ro && ro->object().type() == PycObject::TYPE_NONE))
                    cb->removeLast();
            }
        }
        if (clean->nodes().back().type() == ASTNode::NODE_RETURN) {
            PycRef<ASTReturn> ret = clean->nodes().back().cast<ASTReturn>();

            PycRef<ASTObject> retObj = ret->value().try_cast<ASTObject>();
            if (ret->value() == NULL || ret->value().type() == ASTNode::NODE_LOCALS ||
                    (retObj && retObj->object().type() == PycObject::TYPE_NONE)) {
                /* ...except in a lambda, where it is the whole body
                   (`lambda: None`). */
                if (!(code->name()->isEqual("<lambda>") && clean->nodes().size() == 1))
                    clean->removeLast();  // Always an extraneous return statement
            }
        }

        // Python 3.11 class bodies end with compiler-generated
        // "__classcell__ = __class__" and "return __class__"; strip them.
        if (clean->nodes().size() &&
                clean->nodes().back().type() == ASTNode::NODE_RETURN) {
            PycRef<ASTReturn> ret = clean->nodes().back().cast<ASTReturn>();
            if (ret->value() != NULL && ret->value().type() == ASTNode::NODE_NAME &&
                    ret->value().cast<ASTName>()->name()->isEqual("__class__")) {
                clean->removeLast();
            }
        }
        if (clean->nodes().size() &&
                clean->nodes().back().type() == ASTNode::NODE_STORE) {
            PycRef<ASTStore> st = clean->nodes().back().cast<ASTStore>();
            if (st->dest().type() == ASTNode::NODE_NAME &&
                    st->dest().cast<ASTName>()->name()->isEqual("__classcell__")) {
                clean->removeLast();
            }
        }

        /* Strip the module's implicit trailing "return None" (invalid at
           module scope), even when nested in a trailing block. */
        if (code->name()->isEqual("<module>"))
            StripModuleTrailingReturn(clean.cast<ASTNode>());
        else if (!code->name()->isEqual("<lambda>"))
            StripTailReturnNone(clean.cast<ASTNode>());
    }
    if (printClassDocstring)
        printClassDocstring = false;
    // This is outside the clean check so a source block will always
    // be compilable, even if decompylation failed.
    if (clean->nodes().size() == 0 && !code.isIdent(mod->code()))
        clean->append(new ASTKeyword(ASTKeyword::KW_PASS));

    bool part1clean = cleanBuild;

    if (printDocstringAndGlobals) {
        if (code->consts()->size())
            print_docstring(code->getConst(0), cur_indent + 1, mod, pyc_output);

        PycCode::globals_t globs = code->getGlobals();
        if (globs.size()) {
            start_line(cur_indent + 1, pyc_output);
            pyc_output << "global ";
            bool first = true;
            for (const auto& glob : globs) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << glob->value();
                first = false;
            }
            pyc_output << "\n";
        }
        PycCode::globals_t nonlocs = code->getNonlocals();
        if (nonlocs.size()) {
            start_line(cur_indent + 1, pyc_output);
            pyc_output << "nonlocal ";
            bool first = true;
            for (const auto& nl : nonlocs) {
                if (!first)
                    pyc_output << ", ";
                pyc_output << nl->value();
                first = false;
            }
            pyc_output << "\n";
        }
        printDocstringAndGlobals = false;
    }

    print_src(source, mod, pyc_output);

    if (!cleanBuild || !part1clean) {
        start_line(cur_indent, pyc_output);
        pyc_output << "# WARNING: Decompyle incomplete\n";
    }

    code_seen.erase((PycCode *)code);
}
