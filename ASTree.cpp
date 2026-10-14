#include <climits>
#include <functional>
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
#include "ASTScan.h"
#include "ASTInternal.h"
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
bool cleanBuild;

/* Use this to prevent printing return keywords and newlines in lambdas. */
bool inLambda = false;

/* Set just before printing a lambda that sits directly in a call argument
   list, where it needs no enclosing parentheses. */
bool lambdaBare = false;
bool genexprBare = false;   /* print a sole generator argument without parentheses */

/* Use this to keep track of whether we need to print out any docstring and
 * the list of global variables that we are using (such as inside a function). */
bool printDocstringAndGlobals = false;

/* Use this to keep track of whether we need to print a class or module docstring */
bool printClassDocstring = true;

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

/* Combine two operands with `and`/`or`, keeping chains flat and left-associative
   (`a and b and c`, not `a and (b and c)`). */
static PycRef<ASTNode> mkBoolOp(PycRef<ASTNode> left, PycRef<ASTNode> right, ASTBinary::BinOp op)
{
    if (right != NULL && right.type() == ASTNode::NODE_BINARY
            && right.cast<ASTBinary>()->op() == op)
        return mkBoolOp(mkBoolOp(left, right.cast<ASTBinary>()->left(), op),
                        right.cast<ASTBinary>()->right(), op);
    return new ASTBinary(left, right, op);
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
    /* Nested/starred unpacking (`(a, b), *c = x`). `unpack` counts the targets
       still expected by the innermost UNPACK_*; the enclosing frames wait in
       unpackSaved while a nested sequence is being filled in. `unpackStar` is
       the index of the starred target of the innermost frame (UNPACK_EX). */
    struct UnpackFrame { int remaining; int starIdx; };
    std::vector<UnpackFrame> unpackSaved;
    int unpackStar = -1;
    /* Open a new unpack level: a nested one when targets are still pending. */
    auto unpackBegin = [&](int count, int starIdx) {
        if (unpack > 0)
            unpackSaved.push_back(UnpackFrame{unpack, unpackStar});
        unpack = count;
        unpackStar = starIdx;
        ASTTuple::value_t vals;
        PycRef<ASTTuple> tup = new ASTTuple(vals);
        stack.push(tup.cast<ASTNode>());
    };
    /* Hand one assignment target to the innermost unpack tuple; closes finished
       tuples (adding them to their parent) and emits the statement when the
       outermost one is complete. */
    auto unpackAdd = [&](PycRef<ASTNode> target) {
        for (;;) {
            PycRef<ASTNode> tup = stack.top();
            if (tup.type() != ASTNode::NODE_TUPLE) {
                fputs("Something TERRIBLE happened!\n", stderr);
                return;
            }
            if ((int)tup.cast<ASTTuple>()->values().size() == unpackStar)
                target = new ASTUnary(target, ASTUnary::UN_UNPACK);
            tup.cast<ASTTuple>()->add(target);
            if (--unpack > 0)
                return;
            stack.pop();
            if (!unpackSaved.empty()) {
                /* a nested tuple is complete: it is one target of its parent */
                PycRef<ASTTuple> inner = tup.cast<ASTTuple>();
                inner->setRequireParens(true);
                target = tup;
                unpack = unpackSaved.back().remaining;
                unpackStar = unpackSaved.back().starIdx;
                unpackSaved.pop_back();
                continue;
            }
            PycRef<ASTNode> seq = stack.top();
            stack.pop();
            unpackStar = -1;
            if (curblock->blktype() == ASTBlock::BLK_FOR && !curblock->inited()) {
                PycRef<ASTTuple> tuple = tup.try_cast<ASTTuple>();
                if (tuple != NULL)
                    tuple->setRequireParens(false);
                curblock.cast<ASTIterBlock>()->setIndex(tup);
            } else if (seq.type() == ASTNode::NODE_CHAINSTORE) {
                tup.cast<ASTTuple>()->setRequireParens(false);
                append_to_chain_store(seq, tup, stack, curblock);
            } else {
                tup.cast<ASTTuple>()->setRequireParens(false);
                curblock->append(new ASTStore(seq, tup));
            }
            return;
        }
    };
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
        /* kind 1: an open `then if cond else else` -- `left` is the condition,
           `thenVal` the "then" value once its jump has been seen */
        int kind = 0;
        bool neg = false;
        PycRef<ASTNode> thenVal;
        int elseStart = 0, thenJump = 0;
    };
    std::vector<PendingBoolOp> pendingBool;
    std::map<int, PycRef<ASTNode> > chainVals;   /* tests of an open compound condition */
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
    std::vector<PycRef<ASTNode> > guardChainConds;   /* earlier parts of `while a and b` */

    /* Python 3.11 try/except/else state (see ScanTryElse). */
    std::map<int, std::pair<int, int> > elseRegionByStart;
    std::vector<std::pair<int, PycRef<ASTBlock> > > openElse;   /* jump pos, block */

    /* Python 3.11 chained-comparison reconstruction state. */

    WhileLoops loops;
    ExprFlow xflow;
    if (mod->verCompare(3, 8) >= 0) {
        ScanLoops(code, mod, loops);
        whileGuardEnd = loops.guardEnd;
        whileBackedges = loops.backedges;
        ScanExprFlow(code, mod, xflow, loops);
        for (const auto& r : xflow.chainSkip)
            cleanupSkip.push_back(r);
    }

    if (mod->verCompare(3, 11) >= 0) {
        exception_entries = code->exceptionTableEntries();
        if (mod->verCompare(3, 12) >= 0)
            ScanComprehensionCleanup(code, mod, exception_entries, cleanupSkip);
        ScanWithBlocks(code, mod, exception_entries,
                       withBodyEndByBefore, withResumeByBodyEnd);
        ScanTryFinally(code, mod, exception_entries,
                       finallyTryEndByStart, finallyEndByStart, finallyResumeByEnd,
                       finallyReturnStarts, finallySuppressReturns);
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
            if (skipIt) {
                /* a chained-comparison cleanup sits between an if-body and the
                   code after it: the block still ends where the skipped range
                   ends */
                for (const auto& r : xflow.chainSkip)
                    if (curpos >= r.first && curpos < r.second
                            && (curblock->blktype() == ASTBlock::BLK_ELSE
                                || curblock->blktype() == ASTBlock::BLK_IF
                                || curblock->blktype() == ASTBlock::BLK_ELIF))
                        else_pop = (curblock->end() == pos);
                continue;
            }
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
            if (pb.kind == 1) {
                PycRef<ASTCondBlock> tb = new ASTCondBlock(ASTBlock::BLK_IF, 0, pb.left, pb.neg);
                stack.push(new ASTTernary(tb.cast<ASTNode>(), pb.thenVal, right));
            } else {
                stack.push(mkBoolOp(pb.left, right, pb.op));
            }
        }

        int chainPolarity = -1;   /* set when a compound test was just combined */
        /* Compound statement condition: remember each test, and at the last
           jump replace them by the combined expression (see ScanExprFlow). */
        if (mod->verCompare(3, 8) >= 0 && inlineComps.empty() && !isCompCode) {
            if (xflow.condMember.count(curpos) && !stack.empty()) {
                chainVals[curpos] = stack.top();
                stack.pop();
                else_pop = false;
                continue;
            }
            auto ccit = xflow.condChains.find(curpos);
            if (ccit != xflow.condChains.end() && !stack.empty()) {
                const ExprFlow::CondChain& cc = ccit->second;
                bool have = true;
                for (size_t k = 0; k + 1 < cc.members.size(); k++)
                    if (!chainVals.count(cc.members[k])) have = false;
                if (have) {
                    chainVals[curpos] = stack.top();
                    stack.pop();
                    std::function<PycRef<ASTNode>(int)> build = [&](int ti) -> PycRef<ASTNode> {
                        const ExprFlow::CondTree& tr = cc.trees[ti];
                        if (tr.op == 0)
                            return chainVals[tr.leaf];
                        return mkBoolOp(build(tr.l), build(tr.r),
                                        tr.op == 1 ? ASTBinary::BIN_LOG_AND : ASTBinary::BIN_LOG_OR);
                    };
                    PycRef<ASTNode> whole = build(cc.root);
                    for (int mpos : cc.members)
                        chainVals.erase(mpos);
                    stack.push(whole);
                    chainPolarity = cc.negate ? 1 : 0;
                    /* the combined test jumps to the exit when false (or, for a
                       negated tree, when true) */
                    const bool wantOnTrue = cc.negate;
                    const bool isTrueOp = opcode == Pyc::POP_JUMP_IF_TRUE_A
                            || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A;
                    if (isTrueOp != wantOnTrue) {
                        if (opcode == Pyc::POP_JUMP_IF_TRUE_A) opcode = Pyc::POP_JUMP_IF_FALSE_A;
                        else if (opcode == Pyc::POP_JUMP_IF_FALSE_A) opcode = Pyc::POP_JUMP_IF_TRUE_A;
                        else if (opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A) opcode = Pyc::POP_JUMP_FORWARD_IF_FALSE_A;
                        else if (opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A) opcode = Pyc::POP_JUMP_FORWARD_IF_TRUE_A;
                    }
                }
            }
        }

        /* Expression-level control flow (see ScanExprFlow) */
        if (mod->verCompare(3, 8) >= 0) {
            auto skipOp = [&]() {
                else_pop = false;
            };
            /* merge the pending operands that end right where a keep-style
               jump falls through (`a and b or c`) */
            auto mergeAt = [&](int at) {
                while (!pendingBool.empty() && pendingBool.back().kind == 0
                        && pendingBool.back().target == at && !stack.empty()) {
                    PendingBoolOp pb = pendingBool.back();
                    pendingBool.pop_back();
                    PycRef<ASTNode> right = stack.top();
                    stack.pop();
                    stack.push(mkBoolOp(pb.left, right, pb.op));
                }
            };
            /* "then" value of an open ternary is complete */
            auto tj = xflow.ternaryThenJump.find(curpos);
            if (tj != xflow.ternaryThenJump.end() && !stack.empty()) {
                for (int k = (int)pendingBool.size() - 1; k >= 0; k--) {
                    if (pendingBool[k].kind == 1 && pendingBool[k].thenJump == curpos
                            && pendingBool[k].thenVal == NULL) {
                        pendingBool[k].thenVal = stack.top();
                        stack.pop();
                        skipOp();
                        goto exprflow_done;
                    }
                }
            }
            {
                auto tern = xflow.ternary.find(curpos);
                if (tern != xflow.ternary.end() && !stack.empty()) {
                    const ExprFlow::Tern& tn = tern->second;
                    PycRef<ASTNode> c = stack.top();
                    stack.pop();
                    bool neg = false;
                    if (chainPolarity >= 0) neg = chainPolarity == 1;
                    else if (tn.cond == 2) neg = true;
                    else if (tn.cond == 3)
                        c = new ASTCompare(c, new ASTObject(Pyc_None), ASTCompare::CMP_IS_NOT);
                    else if (tn.cond == 4)
                        c = new ASTCompare(c, new ASTObject(Pyc_None), ASTCompare::CMP_IS);
                    if (!pendingBool.empty() && pendingBool.back().kind == 1
                            && pendingBool.back().elseStart == tn.elseStart
                            && pendingBool.back().thenVal == NULL) {
                        /* `a if x and y else b`: another test jumping to the same else */
                        PendingBoolOp& fr = pendingBool.back();
                        PycRef<ASTNode> prev = fr.left;
                        if (fr.neg)
                            prev = new ASTUnary(prev, ASTUnary::UN_NOT);
                        PycRef<ASTNode> cur = c;
                        if (neg)
                            cur = new ASTUnary(cur, ASTUnary::UN_NOT);
                        fr.left = mkBoolOp(prev, cur, ASTBinary::BIN_LOG_AND);
                        fr.neg = false;
                    } else {
                        PendingBoolOp fr;
                        fr.target = tn.end;
                        fr.left = c;
                        fr.op = ASTBinary::BIN_LOG_AND;
                        fr.kind = 1;
                        fr.neg = neg;
                        fr.elseStart = tn.elseStart;
                        fr.thenJump = tn.thenJump;
                        pendingBool.push_back(fr);
                    }
                    skipOp();
                    goto exprflow_done;
                }
            }
            {
                auto ks = xflow.keepSeq.find(curpos);
                if (ks != xflow.keepSeq.end() && !stack.empty()) {
                    mergeAt(ks->second.popPos);
                    mergeAt(ks->second.endPos);
                    PendingBoolOp pb;
                    pb.target = ks->second.target;
                    pb.left = stack.top();
                    pb.op = ks->second.isOr ? ASTBinary::BIN_LOG_OR : ASTBinary::BIN_LOG_AND;
                    stack.pop();
                    pendingBool.push_back(pb);
                    source.setPos(ks->second.endPos);
                    pos = ks->second.endPos;
                    skipOp();
                    goto exprflow_done;
                }
            }
            if ((opcode == Pyc::JUMP_IF_TRUE_OR_POP_A || opcode == Pyc::JUMP_IF_FALSE_OR_POP_A)
                    && !stack.empty()) {
                PendingBoolOp pb;
                int tgt = operand * (mod->verCompare(3, 10) >= 0 ? 2 : 1);
                if (mod->verCompare(3, 11) >= 0)
                    tgt += pos;
                {
                    auto cj = xflow.chainJoin.find(curpos);
                    if (cj != xflow.chainJoin.end())
                        tgt = cj->second;
                }
                mergeAt(pos);
                pb.target = tgt;
                pb.left = stack.top();
                pb.op = (opcode == Pyc::JUMP_IF_TRUE_OR_POP_A)
                        ? ASTBinary::BIN_LOG_OR : ASTBinary::BIN_LOG_AND;
                stack.pop();
                pendingBool.push_back(pb);
                skipOp();
                goto exprflow_done;
            }
            {
                auto bj = xflow.boolJump.find(curpos);
                if (bj != xflow.boolJump.end() && !stack.empty()) {
                    PendingBoolOp pb;
                    pb.target = bj->second.first;
                    pb.left = stack.top();
                    pb.op = bj->second.second ? ASTBinary::BIN_LOG_OR : ASTBinary::BIN_LOG_AND;
                    stack.pop();
                    pendingBool.push_back(pb);
                    skipOp();
                    goto exprflow_done;
                }
            }
            goto exprflow_skip;
exprflow_done:
            continue;
exprflow_skip:;
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
            /* `a, b, c = x, y, z` (3 or more targets): the compiler emits all the
               loads, then the stores in reverse order, with no SWAP/ROT. */
            if (!unpack && mod->verCompare(3, 8) >= 0 && isSimpleStore(opcode)
                    && pendingOps.empty() && !source.atEof()
                    && (isSimpleLoad(prevOp1)
                        || prevOp1 == Pyc::LOAD_FAST_LOAD_FAST_A
                        || prevOp1 == Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A)
                    && stack.size() >= 3) {
                int sSrc = source.pos(), sPos = pos;
                std::vector<std::pair<int, int> > stores;
                stores.push_back(std::make_pair(opcode, operand));
                int o2 = 0, a2 = 0;
                while (!source.atEof()) {
                    bc_next(source, mod, o2, a2, pos);
                    if (!isSimpleStore(o2))
                        break;
                    stores.push_back(std::make_pair(o2, a2));
                }
                bool ok = stores.size() >= 3 && (int)stores.size() <= stack.size();
                for (size_t k = 0; ok && k < stores.size(); k++)
                    ok = stack.top((int)k + 1) != NULL
                         && stack.top((int)k + 1).type() != ASTNode::NODE_CHAINSTORE;
                if (ok) {
                    /* leave the first non-store op to be read again */
                    source.setPos(sSrc);
                    pos = sPos;
                    for (size_t k = 1; k < stores.size(); k++)
                        bc_next(source, mod, o2, a2, pos);
                    auto storeName = [&](int op, int arg) -> PycRef<ASTNode> {
                        if (op == Pyc::STORE_FAST_A)
                            return new ASTName(code->getLocal(arg));
                        if (op == Pyc::STORE_DEREF_A)
                            return new ASTName(code->getCellVar(mod, arg));
                        return new ASTName(code->getName(arg));
                    };
                    ASTTuple::value_t vals(stores.size()), names;
                    for (size_t k = stores.size(); k > 0; k--)
                        names.push_back(storeName(stores[k - 1].first, stores[k - 1].second));
                    for (size_t k = 0; k < stores.size(); k++)
                        vals[stores.size() - 1 - k] = stack.top((int)k + 1);
                    for (size_t k = 0; k < stores.size(); k++)
                        stack.pop();
                    auto vt = new ASTTuple(vals);
                    auto nt = new ASTTuple(names);
                    vt->setRequireParens(false);
                    nt->setRequireParens(false);
                    curblock->append(new ASTStore(vt, nt));
                    prevOp1 = prevOp2 = -1;
                    continue;
                }
                source.setPos(sSrc);
                pos = sPos;
            }
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
                    auto nt = new ASTTuple(names);
                    vt->setRequireParens(false);
                    nt->setRequireParens(false);
                    curblock->append(new ASTStore(vt, nt));
                    prevOp1 = prevOp2 = -1;
                    continue;
                }
                source.setPos(sSrc);
                pos = sPos;
            }
            if (!unpack && opcode == Pyc::STORE_FAST_STORE_FAST_A
                    && ((isSimpleLoad(prevOp1) && isSimpleLoad(prevOp2))
                        || prevOp1 == Pyc::LOAD_FAST_LOAD_FAST_A
                        || prevOp1 == Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A)
                    && stack.size() >= 2 && stack.top() != NULL && stack.top(1) != NULL
                    && stack.top().type() != ASTNode::NODE_CHAINSTORE
                    && stack.top(1).type() != ASTNode::NODE_CHAINSTORE) {
                PycRef<ASTNode> t1 = new ASTName(code->getLocal(operand >> 4));
                PycRef<ASTNode> t2 = new ASTName(code->getLocal(operand & 0xF));
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
                auto nt = new ASTTuple(names);
                vt->setRequireParens(false);
                nt->setRequireParens(false);
                curblock->append(new ASTStore(vt, nt));
                prevOp1 = prevOp2 = -1;
                continue;
            }
            prevOp2 = prevOp1;
            prevOp1 = opcode;
        }

        if (mod->verCompare(3, 12) >= 0) {
            /* 3.12+ spell the (forward, relative) None tests without FORWARD */
            if (opcode == Pyc::POP_JUMP_IF_NONE_A)
                opcode = Pyc::POP_JUMP_FORWARD_IF_NONE_A;
            else if (opcode == Pyc::POP_JUMP_IF_NOT_NONE_A)
                opcode = Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A;
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

        if (mod->verCompare(3, 8) >= 0 && !inlineComps.size()) {
            auto closeWhile = [&](bool popCond) -> bool {
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
                if (!hasWhile)
                    return false;
                if (popCond && !stack.empty())
                    stack.pop();
                while (blocks.size() > 1 && curblock->blktype() != ASTBlock::BLK_WHILE) {
                    PycRef<ASTBlock> inner = curblock;
                    if (!stack_hist.empty()
                            && (inner->blktype() == ASTBlock::BLK_IF
                                || inner->blktype() == ASTBlock::BLK_ELIF
                                || inner->blktype() == ASTBlock::BLK_ELSE)) {
                        stack = stack_hist.top();
                        stack_hist.pop();
                    }
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(inner.cast<ASTNode>());
                }
                if (curblock->blktype() == ASTBlock::BLK_WHILE && blocks.size() > 1) {
                    PycRef<ASTBlock> wh = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(wh.cast<ASTNode>());
                    auto ee = loops.elseEnd.find(wh->end());
                    if (ee != loops.elseEnd.end()) {
                        PycRef<ASTBlock> eb = new ASTBlock(ASTBlock::BLK_ELSE, ee->second);
                        eb->init(ASTCondBlock::PRE_POPPED);
                        stack_hist.push(stack);
                        blocks.push(eb);
                        curblock = blocks.top();
                    }
                }
                return true;
            };
            auto refreshElsePop = [&]() {
                else_pop = (curblock->blktype() == ASTBlock::BLK_ELSE
                            || curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && curblock->end() == pos;
            };
            if (loops.skipJumps.count(curpos)) {
                refreshElsePop();
                continue;
            }
            if ((loops.guardChainMember.count(curpos) || loops.tailChain.count(curpos))
                    && !stack.empty()) {
                /* part of a compound loop condition: remember (guard side) or
                   drop (duplicated tail side) its test */
                if (loops.guardChainMember.count(curpos))
                    guardChainConds.push_back(stack.top());
                stack.pop();
                refreshElsePop();
                continue;
            }
            if (loops.trueOpen.count(curpos)) {
                PycRef<ASTCondBlock> wh = new ASTCondBlock(
                        ASTBlock::BLK_WHILE, loops.trueOpen[curpos],
                        new ASTObject(Pyc_True), false);
                wh->init(ASTCondBlock::PRE_POPPED);
                blocks.push(wh.cast<ASTBlock>());
                curblock = blocks.top();
            }
            if (loops.backedges.count(curpos) && closeWhile(true)) {
                refreshElsePop();
                continue;
            }
            if (loops.uncondBack.count(curpos) && closeWhile(false)) {
                refreshElsePop();
                continue;
            }
            if (loops.forElse.count(curpos)) {
                /* `for ... else:` -- the else body is reached only when the
                   loop was not left by `break` */
                PycRef<ASTBlock> eb = new ASTBlock(ASTBlock::BLK_ELSE, loops.forElse[curpos]);
                eb->init(ASTCondBlock::PRE_POPPED);
                stack_hist.push(stack);
                blocks.push(eb);
                curblock = blocks.top();
            }
            if (loops.forBreakPops.count(curpos)) {
                /* the iterator popped by a `break` out of a for loop */
                refreshElsePop();
                continue;
            }
            if (loops.forBreaks.count(curpos)) {
                curblock->append(new ASTKeyword(ASTKeyword::KW_BREAK));
                refreshElsePop();
                continue;
            }
            if (loops.breakIf.count(curpos) && !stack.empty()) {
                PycRef<ASTNode> bcond = stack.top();
                stack.pop();
                PycRef<ASTCondBlock> bb = new ASTCondBlock(
                        ASTBlock::BLK_IF, pos, bcond, !loops.breakIf[curpos]);
                bb->init(ASTCondBlock::PRE_POPPED);
                bb->append(new ASTKeyword(ASTKeyword::KW_BREAK));
                curblock->append(bb.cast<ASTNode>());
                refreshElsePop();
                continue;
            }
            if (loops.breaks.count(curpos)) {
                curblock->append(new ASTKeyword(ASTKeyword::KW_BREAK));
                refreshElsePop();
                continue;
            }
            if (loops.continues.count(curpos)) {
                curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                refreshElsePop();
                continue;
            }
        }

        switch (opcode) {
        #include "ASTOpsExpr.inc"
        #include "ASTOpsCall.inc"
        #include "ASTOpsCondJump.inc"
        #include "ASTOpsJump.inc"
        #include "ASTOpsBuild.inc"
        #include "ASTOpsStack.inc"
        #include "ASTOpsExcept.inc"
        #include "ASTOpsMisc.inc"
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

