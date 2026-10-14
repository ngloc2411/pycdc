#include <unordered_set>
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <cstring>
#include "ASTree.h"
#include "ASTMatch.h"
#include "ASTInternal.h"
#include "bytecode.h"

/* decompyle(): post-processing of the built tree and top-level output. */
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

/* A lambda body is a single expression, but `lambda x: a if x else b` compiles
   to the same jump-threaded code as `if x: return a; return b`. Fold such a
   statement list back into one expression (null when it has another shape). */
static PycRef<ASTNode> LambdaExpr(const std::vector<PycRef<ASTNode> >& stmts, size_t from)
{
    if (from >= stmts.size())
        return NULL;
    PycRef<ASTNode> first = stmts[from];
    if (first.type() == ASTNode::NODE_RETURN) {
        if (from + 1 != stmts.size())
            return NULL;
        PycRef<ASTReturn> r = first.cast<ASTReturn>();
        return r->rettype() == ASTReturn::RETURN ? r->value() : PycRef<ASTNode>();
    }
    if (first.type() != ASTNode::NODE_BLOCK)
        return NULL;
    PycRef<ASTBlock> blk = first.cast<ASTBlock>();
    if (blk->blktype() != ASTBlock::BLK_IF && blk->blktype() != ASTBlock::BLK_ELIF)
        return NULL;
    std::vector<PycRef<ASTNode> > body(blk->nodes().begin(), blk->nodes().end());
    PycRef<ASTNode> thenExpr = LambdaExpr(body, 0);
    PycRef<ASTNode> elseExpr;
    if (from + 1 < stmts.size() && stmts[from + 1].type() == ASTNode::NODE_BLOCK
            && stmts[from + 1].cast<ASTBlock>()->blktype() == ASTBlock::BLK_ELSE
            && from + 2 == stmts.size()) {
        PycRef<ASTBlock> eb = stmts[from + 1].cast<ASTBlock>();
        std::vector<PycRef<ASTNode> > eb_nodes(eb->nodes().begin(), eb->nodes().end());
        elseExpr = LambdaExpr(eb_nodes, 0);
    } else {
        elseExpr = LambdaExpr(stmts, from + 1);
    }
    if (thenExpr == NULL || elseExpr == NULL)
        return NULL;
    PycRef<ASTCondBlock> cb = blk.cast<ASTCondBlock>();
    PycRef<ASTCondBlock> tb = new ASTCondBlock(ASTBlock::BLK_IF, 0, cb->cond(), cb->negative());
    return new ASTTernary(tb.cast<ASTNode>(), thenExpr, elseExpr);
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
        if (code->name()->isEqual("<lambda>") && clean->nodes().size() > 1) {
            std::vector<PycRef<ASTNode> > stmts(clean->nodes().begin(), clean->nodes().end());
            PycRef<ASTNode> e = LambdaExpr(stmts, 0);
            if (e != NULL) {
                while (clean->nodes().size() > 0)
                    clean->removeLast();
                clean->append(new ASTReturn(e));
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
        /* 3.14 marks a real docstring with CO_HAS_DOCSTRING; otherwise
           co_consts[0] is just the first constant used by the body. */
        if (code->consts()->size()
                && (mod->verCompare(3, 14) < 0 || (code->flags() & 0x4000000)))
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
