#include <unordered_set>
#include <vector>
#include <cstring>
#include <algorithm>
#include "ASTree.h"
#include "ASTMatch.h"
#include "ASTInternal.h"
#include "pyc_numeric.h"
#include "bytecode.h"

/* Source printing: turns the AST built by BuildFromCode back into Python
   source text. */

#define F_STRING_QUOTE "'''"

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
    } else if (child.type() == ASTNode::NODE_TERNARY) {
        /* a conditional expression has the lowest precedence of all operators */
        pyc_output << "(";
        print_src(child, mod, pyc_output);
        pyc_output << ")";
    } else {
        print_src(child, mod, pyc_output);
    }
}

/* Print a test that follows `if `, `while `, `not ` or sits inside a conditional
   expression: low-precedence forms need parentheses in some of those places. */
static void print_test(PycRef<ASTNode> node, bool negated, bool inTernary,
                       PycModule* mod, std::ostream& pyc_output)
{
    bool paren = false;
    if (node != NULL) {
        if (node.type() == ASTNode::NODE_BINARY) {
            int op = node.cast<ASTBinary>()->op();
            paren = negated && (op == ASTBinary::BIN_LOG_AND || op == ASTBinary::BIN_LOG_OR);
        } else if (node.type() == ASTNode::NODE_TERNARY) {
            paren = negated || inTernary;
        } else if (node.type() == ASTNode::NODE_FUNCTION) {
            paren = inTernary || negated;
        }
    }
    if (paren) pyc_output << "(";
    print_src(node, mod, pyc_output);
    if (paren) pyc_output << ")";
}

void start_line(int indent, std::ostream& pyc_output)
{
    if (inLambda)
        return;
    for (int i=0; i<indent; i++)
        pyc_output << "    ";
}

void end_line(std::ostream& pyc_output)
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
                const bool negBlk = blk.cast<ASTCondBlock>()->negative();
                if (negBlk)
                    pyc_output << " not ";
                else
                    pyc_output << " ";

                print_test(blk.cast<ASTCondBlock>()->cond(), negBlk, false, mod, pyc_output);
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
            print_test(ternary->if_expr(), false, true, mod, pyc_output);
            const auto if_block = ternary->if_block().cast<ASTCondBlock>();
            pyc_output << " if ";
            if (if_block->negative())
                pyc_output << "not ";
            print_test(if_block->cond(), if_block->negative(), true, mod, pyc_output);
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

