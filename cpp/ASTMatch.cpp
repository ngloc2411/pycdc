#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "ASTMatch.h"
#include "bytecode.h"
#include "pyc_numeric.h"

/* ---------------------------------------------------------------------- *
 * Instruction decoding
 * ---------------------------------------------------------------------- */

MatchAnalyzer::MatchAnalyzer(PycRef<PycCode> code, PycModule* mod)
    : m_code(std::move(code)), m_mod(mod)
{
    PycBuffer source(m_code->code()->value(), m_code->code()->length());
    int opcode, operand;
    int pos = 0;
    m_index.assign(m_code->code()->length() + 2, -1);
    while (!source.atEof()) {
        int start = pos;
        bc_next(source, m_mod, opcode, operand, pos);
        if (opcode == Pyc::CACHE)
            continue;
        PyInstr in;
        in.pos = start;
        in.end = pos;
        in.op = opcode;
        in.arg = operand;
        if (start < (int)m_index.size())
            m_index[start] = (int)m_ins.size();
        m_ins.push_back(in);
    }
}

int MatchAnalyzer::indexOf(int pos) const
{
    if (pos < 0 || pos >= (int)m_index.size())
        return -1;
    return m_index[pos];
}

int MatchAnalyzer::jumpTarget(const PyInstr& in) const
{
    switch (in.op) {
    case Pyc::POP_JUMP_IF_FALSE_A:
    case Pyc::POP_JUMP_IF_TRUE_A:
        if (m_mod->verCompare(3, 12) >= 0)
            return in.end + in.arg * 2;
        return in.arg * 2;
    case Pyc::POP_JUMP_IF_NONE_A:
    case Pyc::POP_JUMP_IF_NOT_NONE_A:
    case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
    case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
    case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
    case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
    case Pyc::JUMP_FORWARD_A:
        return in.end + in.arg * 2;
    default:
        return -1;
    }
}

static bool isCopy1(const PyInstr& in)
{
    return (in.op == Pyc::COPY_A && in.arg == 1) || in.op == Pyc::DUP_TOP;
}

bool MatchAnalyzer::isMatchStart(int pos) const
{
    int i = indexOf(pos);
    if (i < 0)
        return false;
    const PyInstr& in = m_ins[i];
    switch (in.op) {
    case Pyc::COPY_A:
        return in.arg == 1;
    case Pyc::DUP_TOP:
    case Pyc::MATCH_SEQUENCE:
    case Pyc::MATCH_MAPPING:
        return true;
    case Pyc::LOAD_GLOBAL_A:
    case Pyc::LOAD_NAME_A:
    case Pyc::LOAD_FAST_A:
    case Pyc::LOAD_DEREF_A:
        /* A class pattern: LOAD <cls> (LOAD_ATTR)* LOAD_CONST <names> MATCH_CLASS */
        for (int k = i + 1; k < (int)m_ins.size() && k < i + 8; k++) {
            if (m_ins[k].op == Pyc::MATCH_CLASS_A)
                return true;
            if (m_ins[k].op != Pyc::LOAD_ATTR_A && m_ins[k].op != Pyc::LOAD_CONST_A)
                return false;
        }
        return false;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------- *
 * Symbolic execution of the pattern code
 * ---------------------------------------------------------------------- */

namespace {

struct SimFail { };
struct FloorHit { int idx; };

struct Hole;

/* An abstract value on the simulated stack */
struct AV {
    enum Kind {
        HOLE,       // a piece of the subject: it still has to become a pattern
        OUTER,      // the subject kept for the following cases (never consumed)
        EXPR,       // a loaded expression (constant, name, attribute, tuple)
        BOOL,       // a test result
        LEN,        // len() of a sequence hole
        NEGIDX,     // len() - n
        ATTRS,      // tuple of attributes (MATCH_CLASS)
        VALS,       // tuple of values (MATCH_KEYS)
        DICT,       // dict of the remaining mapping items (**rest)
        KEYITEM     // a key of a mapping pattern
    };

    Kind k;
    Hole* h;
    PycRef<ASTNode> e;
    int n;

    AV() : k(EXPR), h(nullptr), n(0) { }
    explicit AV(Kind kind, Hole* hole = nullptr, int num = 0)
        : k(kind), h(hole), n(num) { }
};

struct Hole {
    enum Kind { UNSET, VALUE, SEQ, MAP, CLASS, OR };

    Kind kind = UNSET;
    PycRef<ASTNode> expr;           // VALUE: expression, CLASS: class

    // SEQ
    int seqLen = -1;
    bool seqStar = false;
    std::map<int, Hole*> seqItems;  // fetched by index (negative: from the end)
    bool unpacked = false;
    bool upEx = false;
    int upBefore = 0, upAfter = 0;
    std::vector<Hole*> upItems;

    // MAP
    std::vector<PycRef<ASTNode>> keys;
    std::vector<Hole*> valItems;
    std::vector<PycRef<PycString>> restNames;

    // CLASS
    int nPos = 0;
    std::vector<PycRef<PycString>> kwNames;
    std::vector<Hole*> attrItems;

    // OR
    std::vector<Hole*> alts;

    // Names bound to this hole (capture or "as")
    std::vector<PycRef<PycString>> names;
    std::vector<Hole*> mirrors;     // holes of other alternatives bound to the same name
    bool isStar = false;
};

struct Edge {
    int target;
    int depth;
};

struct State {
    std::vector<AV> st;
    std::vector<Edge> edges;
};

enum Mode { MODE_NORMAL, MODE_ALT };

struct MatchSim {
    const MatchAnalyzer& a;
    std::deque<Hole> arena;
    Hole* top = nullptr;
    int inlinedBodyEnd = -1;

    explicit MatchSim(const MatchAnalyzer& an) : a(an) { }

    const std::vector<PyInstr>& ins() const { return a.instrs(); }
    PycModule* mod() const { return a.mod(); }

    Hole* newHole()
    {
        arena.emplace_back();
        return &arena.back();
    }

    static AV pop(State& s, size_t floor)
    {
        if (s.st.size() <= floor || s.st.back().k == AV::OUTER)
            throw SimFail();
        AV v = s.st.back();
        s.st.pop_back();
        return v;
    }

    static Hole* needHole(const AV& v)
    {
        if (v.k != AV::HOLE)
            throw SimFail();
        return v.h;
    }

    static void addName(Hole* h, const PycRef<PycString>& name)
    {
        h->names.push_back(name);
        for (Hole* m : h->mirrors)
            addName(m, name);
    }

    static bool constInt(const AV& v, int& out)
    {
        if (v.k != AV::EXPR || v.e.type() != ASTNode::NODE_OBJECT)
            return false;
        PycRef<PycObject> obj = v.e.cast<ASTObject>()->object();
        if (obj.type() != PycObject::TYPE_INT)
            return false;
        out = obj.cast<PycInt>()->value();
        return true;
    }

    static std::vector<PycRef<ASTNode>> tupleItems(const AV& v)
    {
        std::vector<PycRef<ASTNode>> res;
        if (v.k != AV::EXPR)
            throw SimFail();
        if (v.e.type() == ASTNode::NODE_TUPLE) {
            res = v.e.cast<ASTTuple>()->values();
        } else if (v.e.type() == ASTNode::NODE_OBJECT) {
            PycRef<PycObject> obj = v.e.cast<ASTObject>()->object();
            if (obj.type() != PycObject::TYPE_TUPLE && obj.type() != PycObject::TYPE_SMALL_TUPLE)
                throw SimFail();
            for (const auto& item : obj.cast<PycTuple>()->values())
                res.push_back(new ASTObject(item));
        } else {
            throw SimFail();
        }
        return res;
    }

    int cmpOp(int arg) const
    {
        if (mod()->verCompare(3, 12) == 0)
            return arg >> 4;
        if (mod()->verCompare(3, 13) >= 0)
            return arg >> 5;
        return arg;
    }

    static Hole* itemOf(MatchSim& sim, std::vector<Hole*>& items, size_t i)
    {
        if (i >= items.size())
            throw SimFail();
        if (!items[i])
            items[i] = sim.newHole();
        return items[i];
    }

    /* Execute instructions starting at `idx`.
     * MODE_NORMAL: until the stack shrinks back to `base` entries.
     * MODE_ALT: until the JUMP_FORWARD ending an alternative of an or-pattern;
     *           `endJump` receives the index of that jump. */
    void exec(State& s, int& idx, Mode mode, size_t floor, size_t base, int* endJump)
    {
        const std::vector<PyInstr>& code = ins();
        PycRef<PycCode> co = a.code();
        int executed = 0;
        while (true) {
            if (mode == MODE_NORMAL && executed > 0 && s.st.size() == base)
                return;
            if (idx < 0 || idx >= (int)code.size())
                throw SimFail();
            const PyInstr& in = code[idx];
            if (mode == MODE_ALT
                    && (in.op == Pyc::STORE_FAST_A || in.op == Pyc::STORE_NAME_A
                        || in.op == Pyc::STORE_GLOBAL_A || in.op == Pyc::STORE_DEREF_A
                        || (s.st.size() <= floor
                            && (in.op == Pyc::POP_TOP || in.op == Pyc::SWAP_A))))
                throw FloorHit{idx};
            executed++;
            switch (in.op) {
            case Pyc::NOP:
                idx++;
                break;
            case Pyc::LOAD_CONST_A:
                {
                    AV v;
                    v.k = AV::EXPR;
                    v.e = new ASTObject(co->getConst(in.arg));
                    s.st.push_back(v);
                    idx++;
                }
                break;
            case Pyc::LOAD_GLOBAL_A:
            case Pyc::LOAD_NAME_A:
            case Pyc::LOAD_FAST_A:
            case Pyc::LOAD_DEREF_A:
                {
                    PycRef<PycString> name;
                    int arg = in.arg;
                    if (in.op == Pyc::LOAD_GLOBAL_A) {
                        if (mod()->verCompare(3, 11) >= 0) {
                            if (arg & 1)
                                throw SimFail();
                            arg >>= 1;
                        }
                        name = co->getName(arg);
                    } else if (in.op == Pyc::LOAD_NAME_A) {
                        name = co->getName(arg);
                    } else if (in.op == Pyc::LOAD_FAST_A) {
                        name = co->getLocal(arg);
                    } else {
                        name = co->getCellVar(mod(), arg);
                    }
                    AV v;
                    v.k = AV::EXPR;
                    v.e = new ASTName(name);
                    s.st.push_back(v);
                    idx++;
                }
                break;
            case Pyc::LOAD_ATTR_A:
                {
                    int arg = in.arg;
                    if (mod()->verCompare(3, 12) >= 0) {
                        if (arg & 1)
                            throw SimFail();
                        arg >>= 1;
                    }
                    AV base_v = pop(s, floor);
                    if (base_v.k != AV::EXPR)
                        throw SimFail();
                    AV v;
                    v.k = AV::EXPR;
                    v.e = new ASTBinary(base_v.e, new ASTName(co->getName(arg)), ASTBinary::BIN_ATTR);
                    s.st.push_back(v);
                    idx++;
                }
                break;
            case Pyc::BUILD_TUPLE_A:
                {
                    ASTTuple::value_t values;
                    values.resize(in.arg);
                    for (int i = 0; i < in.arg; i++) {
                        AV v = pop(s, floor);
                        if (v.k != AV::EXPR)
                            throw SimFail();
                        values[in.arg - i - 1] = v.e;
                    }
                    AV v;
                    v.k = AV::EXPR;
                    v.e = new ASTTuple(values);
                    s.st.push_back(v);
                    idx++;
                }
                break;
            case Pyc::DUP_TOP:
            case Pyc::COPY_A:
                {
                    int depth = (in.op == Pyc::DUP_TOP) ? 1 : in.arg;
                    if (depth < 1 || (size_t)depth > s.st.size())
                        throw SimFail();
                    if (depth == 1 && isCopy1(in) && s.st.back().k == AV::HOLE) {
                        int j = idx;
                        if (tryOr(s, j)) {
                            idx = j;
                            break;
                        }
                    }
                    AV v = s.st[s.st.size() - depth];
                    if (v.k == AV::OUTER) {
                        /* the first copy of the kept subject is the pattern subject */
                        Hole* h = newHole();
                        if (!top)
                            top = h;
                        v = AV(AV::HOLE, h);
                    }
                    s.st.push_back(v);
                    idx++;
                }
                break;
            case Pyc::SWAP_A:
                {
                    if (in.arg < 1 || (size_t)in.arg > s.st.size())
                        throw SimFail();
                    std::swap(s.st.back(), s.st[s.st.size() - in.arg]);
                    if (s.st.back().k == AV::OUTER || s.st[s.st.size() - in.arg].k == AV::OUTER)
                        throw SimFail();
                    idx++;
                }
                break;
            case Pyc::ROT_TWO:
            case Pyc::ROT_THREE:
            case Pyc::ROT_FOUR:
            case Pyc::ROT_N_A:
                {
                    int n = (in.op == Pyc::ROT_TWO) ? 2 : (in.op == Pyc::ROT_THREE) ? 3
                          : (in.op == Pyc::ROT_FOUR) ? 4 : in.arg;
                    if (n < 2 || (size_t)n > s.st.size() || s.st.size() - n < floor)
                        throw SimFail();
                    AV v = s.st.back();
                    s.st.pop_back();
                    s.st.insert(s.st.end() - (n - 1), v);
                    for (int i = 1; i <= n; i++) {
                        if (s.st[s.st.size() - i].k == AV::OUTER)
                            throw SimFail();
                    }
                    idx++;
                }
                break;
            case Pyc::POP_TOP:
                pop(s, floor);
                idx++;
                break;
            case Pyc::GET_LEN:
                {
                    if (s.st.empty())
                        throw SimFail();
                    Hole* h = needHole(s.st.back());
                    s.st.push_back(AV(AV::LEN, h));
                    idx++;
                }
                break;
            case Pyc::BINARY_SUBTRACT:
            case Pyc::BINARY_OP_A:
                {
                    if (in.op == Pyc::BINARY_OP_A
                            && ASTBinary::from_binary_op(in.arg) != ASTBinary::BIN_SUBTRACT)
                        throw SimFail();
                    AV r = pop(s, floor);
                    AV l = pop(s, floor);
                    int k;
                    if (l.k != AV::LEN || !constInt(r, k))
                        throw SimFail();
                    s.st.push_back(AV(AV::NEGIDX, l.h, k));
                    idx++;
                }
                break;
            case Pyc::COMPARE_OP_A:
                {
                    int cmp = cmpOp(in.arg);
                    AV r = pop(s, floor);
                    AV l = pop(s, floor);
                    int n;
                    if (l.k == AV::LEN && constInt(r, n)) {
                        if (cmp == ASTCompare::CMP_EQUAL) {
                            l.h->seqLen = n;
                            l.h->seqStar = false;
                        } else if (cmp == ASTCompare::CMP_GREATER_EQUAL) {
                            l.h->seqLen = n;
                            l.h->seqStar = true;
                        } else {
                            throw SimFail();
                        }
                    } else if (l.k == AV::HOLE && r.k == AV::EXPR && cmp == ASTCompare::CMP_EQUAL) {
                        if (l.h->kind != Hole::UNSET)
                            throw SimFail();
                        l.h->kind = Hole::VALUE;
                        l.h->expr = r.e;
                    } else {
                        throw SimFail();
                    }
                    s.st.push_back(AV(AV::BOOL));
                    idx++;
                }
                break;
            case Pyc::IS_OP_A:
                {
                    AV r = pop(s, floor);
                    AV l = pop(s, floor);
                    if (l.k == AV::HOLE && r.k == AV::EXPR && in.arg == 0) {
                        if (l.h->kind != Hole::UNSET)
                            throw SimFail();
                        l.h->kind = Hole::VALUE;
                        l.h->expr = r.e;
                    } else if (l.k != AV::ATTRS && l.k != AV::VALS) {
                        throw SimFail();
                    }
                    s.st.push_back(AV(AV::BOOL));
                    idx++;
                }
                break;
            case Pyc::MATCH_SEQUENCE:
            case Pyc::MATCH_MAPPING:
                {
                    if (s.st.empty())
                        throw SimFail();
                    Hole* h = needHole(s.st.back());
                    Hole::Kind want = (in.op == Pyc::MATCH_SEQUENCE) ? Hole::SEQ : Hole::MAP;
                    if (h->kind != Hole::UNSET && h->kind != want)
                        throw SimFail();
                    h->kind = want;
                    s.st.push_back(AV(AV::BOOL));
                    idx++;
                }
                break;
            case Pyc::MATCH_KEYS:
                {
                    if (s.st.size() < 2)
                        throw SimFail();
                    AV keys = s.st.back();
                    Hole* h = needHole(s.st[s.st.size() - 2]);
                    if (h->kind != Hole::MAP)
                        throw SimFail();
                    h->keys = tupleItems(keys);
                    h->valItems.assign(h->keys.size(), nullptr);
                    s.st.push_back(AV(AV::VALS, h));
                    if (mod()->verCompare(3, 11) < 0)
                        s.st.push_back(AV(AV::BOOL));
                    idx++;
                }
                break;
            case Pyc::MATCH_CLASS_A:
                {
                    AV names = pop(s, floor);
                    AV cls = pop(s, floor);
                    AV subj = pop(s, floor);
                    Hole* h = needHole(subj);
                    if (h->kind != Hole::UNSET || cls.k != AV::EXPR)
                        throw SimFail();
                    std::vector<PycRef<ASTNode>> kw = tupleItems(names);
                    h->kind = Hole::CLASS;
                    h->expr = cls.e;
                    h->nPos = in.arg;
                    for (const auto& n : kw) {
                        if (n.type() != ASTNode::NODE_OBJECT)
                            throw SimFail();
                        PycRef<PycString> str = n.cast<ASTObject>()->object().try_cast<PycString>();
                        if (str == nullptr)
                            throw SimFail();
                        h->kwNames.push_back(str);
                    }
                    h->attrItems.assign(h->nPos + h->kwNames.size(), nullptr);
                    s.st.push_back(AV(AV::ATTRS, h));
                    if (mod()->verCompare(3, 11) < 0)
                        s.st.push_back(AV(AV::BOOL));
                    idx++;
                }
                break;
            case Pyc::POP_JUMP_IF_FALSE_A:
            case Pyc::POP_JUMP_IF_TRUE_A:
            case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
            case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
            case Pyc::POP_JUMP_IF_NONE_A:
            case Pyc::POP_JUMP_IF_NOT_NONE_A:
            case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
            case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
                {
                    AV c = pop(s, floor);
                    if (in.op == Pyc::POP_JUMP_IF_NOT_NONE_A
                            || in.op == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                        /* jumps (fails) unless the value is None: `case None:` */
                        Hole* h = needHole(c);
                        if (h->kind != Hole::UNSET)
                            throw SimFail();
                        h->kind = Hole::VALUE;
                        h->expr = new ASTObject(Pyc_None);
                    } else if (in.op == Pyc::POP_JUMP_IF_NONE_A
                            || in.op == Pyc::POP_JUMP_FORWARD_IF_NONE_A) {
                        if (c.k != AV::ATTRS && c.k != AV::VALS)
                            throw SimFail();
                    } else if (c.k != AV::BOOL) {
                        throw SimFail();
                    }
                    int target = a.jumpTarget(in);
                    if (target <= in.pos)
                        throw SimFail();
                    Edge e;
                    e.target = target;
                    e.depth = (int)s.st.size();
                    s.edges.push_back(e);
                    idx++;
                }
                break;
            case Pyc::JUMP_FORWARD_A:
                if (mode != MODE_ALT)
                    throw SimFail();
                *endJump = idx;
                return;
            case Pyc::UNPACK_SEQUENCE_A:
                {
                    AV v = pop(s, floor);
                    std::vector<Hole*> items;
                    if (v.k == AV::HOLE) {
                        Hole* h = v.h;
                        if (h->kind != Hole::SEQ || h->unpacked)
                            throw SimFail();
                        h->unpacked = true;
                        for (int i = 0; i < in.arg; i++)
                            h->upItems.push_back(newHole());
                        items = h->upItems;
                    } else if (v.k == AV::ATTRS) {
                        if ((int)v.h->attrItems.size() != in.arg)
                            throw SimFail();
                        for (int i = 0; i < in.arg; i++)
                            items.push_back(itemOf(*this, v.h->attrItems, i));
                    } else if (v.k == AV::VALS) {
                        if ((int)v.h->valItems.size() != in.arg)
                            throw SimFail();
                        for (int i = 0; i < in.arg; i++)
                            items.push_back(itemOf(*this, v.h->valItems, i));
                    } else if (v.k == AV::EXPR) {
                        /* the (constant) keys of a mapping, pushed to be deleted from **rest */
                        if ((int)tupleItems(v).size() != in.arg)
                            throw SimFail();
                        for (int i = 0; i < in.arg; i++)
                            s.st.push_back(AV(AV::KEYITEM));
                        idx++;
                        break;
                    } else {
                        throw SimFail();
                    }
                    for (int i = (int)items.size() - 1; i >= 0; i--)
                        s.st.push_back(AV(AV::HOLE, items[i]));
                    idx++;
                }
                break;
            case Pyc::UNPACK_EX_A:
                {
                    AV v = pop(s, floor);
                    Hole* h = needHole(v);
                    if (h->kind != Hole::SEQ || h->unpacked)
                        throw SimFail();
                    h->unpacked = true;
                    h->upEx = true;
                    h->upBefore = in.arg & 0xFF;
                    h->upAfter = (in.arg >> 8) & 0xFF;
                    int total = h->upBefore + 1 + h->upAfter;
                    for (int i = 0; i < total; i++) {
                        Hole* item = newHole();
                        item->isStar = (i == h->upBefore);
                        h->upItems.push_back(item);
                    }
                    for (int i = total - 1; i >= 0; i--)
                        s.st.push_back(AV(AV::HOLE, h->upItems[i]));
                    idx++;
                }
                break;
            case Pyc::BINARY_SUBSCR:
                {
                    AV index = pop(s, floor);
                    AV base_v = pop(s, floor);
                    Hole* item = nullptr;
                    int i;
                    if (base_v.k == AV::ATTRS && constInt(index, i)) {
                        item = itemOf(*this, base_v.h->attrItems, i);
                    } else if (base_v.k == AV::VALS && constInt(index, i)) {
                        item = itemOf(*this, base_v.h->valItems, i);
                    } else if (base_v.k == AV::HOLE && base_v.h->kind == Hole::SEQ) {
                        int key;
                        if (constInt(index, i)) {
                            key = i;
                        } else if (index.k == AV::NEGIDX && index.h == base_v.h) {
                            key = -index.n;
                        } else {
                            throw SimFail();
                        }
                        Hole*& slot = base_v.h->seqItems[key];
                        if (!slot)
                            slot = newHole();
                        item = slot;
                    } else {
                        throw SimFail();
                    }
                    s.st.push_back(AV(AV::HOLE, item));
                    idx++;
                }
                break;
            case Pyc::BUILD_MAP_A:
                if (in.arg != 0)
                    throw SimFail();
                s.st.push_back(AV(AV::DICT));
                idx++;
                break;
            case Pyc::DICT_UPDATE_A:
                {
                    AV upd = pop(s, floor);
                    Hole* h = needHole(upd);
                    if (h->kind != Hole::MAP || in.arg < 1 || (size_t)in.arg > s.st.size())
                        throw SimFail();
                    AV& d = s.st[s.st.size() - in.arg];
                    if (d.k != AV::DICT || d.h)
                        throw SimFail();
                    d.h = h;
                    idx++;
                }
                break;
            case Pyc::COPY_DICT_WITHOUT_KEYS:
                {
                    AV keys = pop(s, floor);
                    if (keys.k != AV::EXPR || s.st.empty())
                        throw SimFail();
                    Hole* h = needHole(s.st.back());
                    if (h->kind != Hole::MAP)
                        throw SimFail();
                    s.st.push_back(AV(AV::DICT, h));
                    idx++;
                }
                break;
            case Pyc::DELETE_SUBSCR:
                {
                    AV key = pop(s, floor);
                    AV dict = pop(s, floor);
                    if (key.k != AV::KEYITEM && key.k != AV::EXPR)
                        throw SimFail();
                    if (dict.k != AV::DICT)
                        throw SimFail();
                    idx++;
                }
                break;
            case Pyc::STORE_FAST_A:
            case Pyc::STORE_NAME_A:
            case Pyc::STORE_GLOBAL_A:
            case Pyc::STORE_DEREF_A:
                {
                    if (mode == MODE_ALT)
                        throw SimFail();
                    PycRef<PycString> name;
                    if (in.op == Pyc::STORE_FAST_A)
                        name = co->getLocal(in.arg);
                    else if (in.op == Pyc::STORE_DEREF_A)
                        name = co->getCellVar(mod(), in.arg);
                    else
                        name = co->getName(in.arg);
                    AV v = pop(s, floor);
                    if (v.k == AV::HOLE) {
                        addName(v.h, name);
                    } else if (v.k == AV::DICT && v.h) {
                        v.h->restNames.push_back(name);
                    } else {
                        throw SimFail();
                    }
                    idx++;
                }
                break;
            default:
                throw SimFail();
            }
        }
    }

    static bool orFail(int line)
    {
        if (getenv("PYCDC_MATCH_DEBUG"))
            fprintf(stderr, "tryOr failed at line %d\n", line);
        return false;
    }

    /* Is there an implicit `return None` at instruction index q? */
    bool isNoneReturn(int q) const
    {
        const std::vector<PyInstr>& code = ins();
        PycRef<PycCode> co = a.code();
        auto isNone = [&](int arg) {
            return co->getConst(arg).type() == PycObject::TYPE_NONE;
        };
        if (code[q].op == Pyc::RETURN_CONST_A)
            return isNone(code[q].arg);
        return code[q].op == Pyc::LOAD_CONST_A && isNone(code[q].arg)
            && q + 1 < (int)code.size() && code[q + 1].op == Pyc::RETURN_VALUE;
    }

    static bool isTerminator(const PyInstr& in)
    {
        return in.op == Pyc::RETURN_VALUE || in.op == Pyc::RETURN_CONST_A
            || in.op == Pyc::RAISE_VARARGS_A;
    }

    /* Try to read an or-pattern whose first alternative starts at `idx`
     * (a COPY of the subject).  On success the state and `idx` are updated
     * to continue at the code following the or-pattern.
     *
     * Two layouts exist: every alternative ends with a JUMP_FORWARD to a
     * shared block, or (small exit blocks) the shared block is duplicated
     * after each alternative. */
    bool tryOr(State& s, int& idx)
    {
        const std::vector<PyInstr>& code = ins();
        if (s.st.empty() || s.st.back().k != AV::HOLE)
            return orFail(__LINE__);
        Hole* subject = s.st.back().h;
        const size_t n0 = s.st.size();
        State base = s;
        base.edges.clear();

        struct Alt {
            Hole* f;
            std::vector<AV> residue;
        };
        std::vector<Alt> alts;
        int endPos = -1;
        int cur = idx;
        int form = 0;           // 1: jump to shared block, 2: duplicated block
        int firstS = -1, firstQ0 = -1;
        std::vector<Edge> tailEdges;

        while (true) {
            if (cur >= (int)code.size() || !isCopy1(code[cur]))
                return orFail(__LINE__);
            State alt = base;
            Hole* f = newHole();
            alt.st.push_back(AV(AV::HOLE, f));
            int j = cur + 1;
            int jumpIdx = -1;
            int sIdx = -1;
            try {
                exec(alt, j, MODE_ALT, n0, 0, &jumpIdx);
            } catch (const SimFail&) {
                return orFail(__LINE__);
            } catch (const FloorHit& fh) {
                sIdx = fh.idx;
            }
            int myForm = (sIdx < 0) ? 1 : 2;
            if (form == 0)
                form = myForm;
            else if (form != myForm)
                return orFail(__LINE__);

            Alt rec;
            rec.f = f;
            rec.residue.assign(alt.st.begin() + n0, alt.st.end());
            alts.push_back(rec);

            if (form == 1) {
                const PyInstr& jump = code[jumpIdx];
                int target = a.jumpTarget(jump);
                if (endPos < 0)
                    endPos = target;
                else if (endPos != target)
                    return orFail(__LINE__);

                /* Skip the failure cleanup of this alternative */
                int q = jumpIdx + 1;
                while (q < (int)code.size() && code[q].op == Pyc::POP_TOP)
                    q++;
                if (q >= (int)code.size())
                    return orFail(__LINE__);

                /* The failures of this alternative land between the jump and q */
                for (const Edge& e : alt.edges) {
                    if (e.target < jump.end || e.target > code[q].pos)
                        return orFail(__LINE__);
                }

                if (isCopy1(code[q])) {
                    cur = q;
                    continue;
                }

                /* No more alternatives: the code at q is the "no match" tail */
                int endIdx;
                if (code[q].pos == endPos) {
                    endIdx = q;
                } else if (code[q].op == Pyc::JUMP_FORWARD_A && code[q].end == endPos) {
                    int outer = a.jumpTarget(code[q]);
                    if (outer <= code[q].pos)
                        return orFail(__LINE__);
                    Edge e;
                    e.target = outer;
                    e.depth = (int)n0 - 1;
                    tailEdges.push_back(e);
                    endIdx = a.indexOf(endPos);
                } else if (isNoneReturn(q) && code[q + (code[q].op == Pyc::RETURN_CONST_A ? 1 : 2) - 1].end == endPos) {
                    /* the end of the code was inlined as the failure path;
                     * the case body follows it */
                    endIdx = a.indexOf(endPos);
                } else {
                    return orFail(__LINE__);
                }
                if (endIdx < 0 || alts.size() < 2)
                    return orFail(__LINE__);
                if (!joinAlts(s, subject, alts, tailEdges))
                    return orFail(__LINE__);
                idx = endIdx;
                return true;
            }

            /* Duplicated exit block: find its end */
            int k = sIdx;
            while (k < (int)code.size() && k - sIdx < 8 && !isTerminator(code[k])) {
                int op = code[k].op;
                if (op == Pyc::POP_TOP || op == Pyc::STORE_FAST_A || op == Pyc::LOAD_FAST_A
                        || op == Pyc::LOAD_CONST_A || op == Pyc::LOAD_GLOBAL_A
                        || op == Pyc::LOAD_NAME_A || op == Pyc::STORE_NAME_A
                        || op == Pyc::NOP || op == Pyc::STORE_GLOBAL_A) {
                    k++;
                    continue;
                }
                return orFail(__LINE__);
            }
            if (k >= (int)code.size() || !isTerminator(code[k]))
                return orFail(__LINE__);
            int q0 = k + 1;
            if (firstS < 0) {
                firstS = sIdx;
                firstQ0 = q0;
            } else {
                if (q0 - sIdx != firstQ0 - firstS)
                    return orFail(__LINE__);
                for (int t = 0; t < q0 - sIdx; t++) {
                    if (code[sIdx + t].op != code[firstS + t].op
                            || code[sIdx + t].arg != code[firstS + t].arg)
                        return orFail(__LINE__);
                }
            }
            int r = 0;
            while (q0 + r < (int)code.size() && code[q0 + r].op == Pyc::POP_TOP)
                r++;
            int m = 0;
            for (const Edge& e : alt.edges) {
                int dd = e.depth - (int)n0;
                if (dd < 0)
                    return orFail(__LINE__);
                m = std::max(m, dd);
            }
            if (alt.edges.empty())
                return orFail(__LINE__);
            for (const Edge& e : alt.edges) {
                int want = q0 + m - (e.depth - (int)n0);
                if (want >= (int)code.size() || code[want].pos != e.target)
                    return orFail(__LINE__);
            }
            bool deepest = false;
            for (const Edge& e : alt.edges) {
                if (e.target == code[q0].pos)
                    deepest = true;
            }
            if (!deepest)
                return orFail(__LINE__);
            if (r == m && q0 + r < (int)code.size() && isCopy1(code[q0 + r])) {
                cur = q0 + r;
                continue;
            }
            if (getenv("PYCDC_MATCH_DEBUG"))
                fprintf(stderr, "or last alt: q0=%d r=%d m=%d alts=%d\n", code[q0].pos, r, m, (int)alts.size());
            if (r < m + 1 || alts.size() < 2 || q0 + m + 1 >= (int)code.size())
                return orFail(__LINE__);
            Edge e;
            e.target = code[q0 + m + 1].pos;
            e.depth = (int)n0 - 1;
            tailEdges.push_back(e);
            if (!joinAlts(s, subject, alts, tailEdges))
                return orFail(__LINE__);
            inlinedBodyEnd = code[firstQ0].pos;
            idx = firstS;
            return true;
        }
    }

    template <class AltT>
    bool joinAlts(State& s, Hole* subject, std::vector<AltT>& alts, const std::vector<Edge>& tailEdges)
    {
        for (size_t i = 1; i < alts.size(); i++) {
            if (alts[i].residue.size() != alts[0].residue.size())
                return false;
        }
        for (size_t m = 0; m < alts[0].residue.size(); m++) {
            if (alts[0].residue[m].k != AV::HOLE)
                return false;
            for (size_t i = 1; i < alts.size(); i++) {
                if (alts[i].residue[m].k != AV::HOLE)
                    return false;
                alts[0].residue[m].h->mirrors.push_back(alts[i].residue[m].h);
            }
        }
        subject->kind = Hole::OR;
        for (const AltT& al : alts)
            subject->alts.push_back(al.f);
        for (const AV& v : alts[0].residue)
            s.st.push_back(v);
        for (const Edge& e : tailEdges)
            s.edges.push_back(e);
        return true;
    }


    /* ------------------------------------------------------------------ */
    /* Conversion of the decoded holes to pattern nodes                    */

    typedef PycRef<ASTMatchPattern> pat_t;

    pat_t resolve(Hole* h)
    {
        pat_t node;
        switch (h->kind) {
        case Hole::UNSET:
            if (h->isStar) {
                node = new ASTMatchPattern(ASTMatchPattern::PAT_STAR);
                if (!h->names.empty())
                    node->setName(h->names[0]);
                return node;
            }
            if (h->names.empty())
                return new ASTMatchPattern(ASTMatchPattern::PAT_WILDCARD);
            node = new ASTMatchPattern(ASTMatchPattern::PAT_CAPTURE);
            node->setName(h->names[0]);
            for (size_t i = 1; i < h->names.size(); i++)
                node = wrapAs(node, h->names[i]);
            return node;
        case Hole::VALUE:
            node = new ASTMatchPattern(ASTMatchPattern::PAT_VALUE);
            node->setExpr(h->expr);
            break;
        case Hole::SEQ:
            node = resolveSeq(h);
            break;
        case Hole::MAP:
            node = new ASTMatchPattern(ASTMatchPattern::PAT_MAPPING);
            for (size_t i = 0; i < h->keys.size(); i++) {
                node->addKey(h->keys[i]);
                if (i < h->valItems.size() && h->valItems[i])
                    node->add(resolve(h->valItems[i]));
                else
                    node->add(new ASTMatchPattern(ASTMatchPattern::PAT_WILDCARD));
            }
            if (!h->restNames.empty())
                node->setName(h->restNames[0]);
            break;
        case Hole::CLASS:
            node = new ASTMatchPattern(ASTMatchPattern::PAT_CLASS);
            node->setExpr(h->expr);
            node->setNumPositional(h->nPos);
            for (size_t i = 0; i < h->attrItems.size(); i++) {
                if (h->attrItems[i])
                    node->add(resolve(h->attrItems[i]));
                else
                    node->add(new ASTMatchPattern(ASTMatchPattern::PAT_WILDCARD));
            }
            for (const auto& n : h->kwNames)
                node->addKwName(n);
            break;
        case Hole::OR:
            node = new ASTMatchPattern(ASTMatchPattern::PAT_OR);
            for (Hole* alt : h->alts)
                node->add(resolve(alt));
            break;
        }
        for (const auto& name : h->names)
            node = wrapAs(node, name);
        return node;
    }

    static pat_t wrapAs(const pat_t& inner, const PycRef<PycString>& name)
    {
        pat_t as = new ASTMatchPattern(ASTMatchPattern::PAT_AS);
        as->add(inner);
        as->setName(name);
        return as;
    }

    static pat_t wildcard()
    {
        return new ASTMatchPattern(ASTMatchPattern::PAT_WILDCARD);
    }

    pat_t resolveSeq(Hole* h)
    {
        pat_t node = new ASTMatchPattern(ASTMatchPattern::PAT_SEQUENCE);
        if (h->unpacked) {
            for (Hole* item : h->upItems)
                node->add(resolve(item));
            return node;
        }
        int n = h->seqLen;
        if (!h->seqItems.empty()) {
            int maxPos = -1, maxNeg = 0;
            for (const auto& kv : h->seqItems) {
                if (kv.first >= 0)
                    maxPos = std::max(maxPos, kv.first);
                else
                    maxNeg = std::max(maxNeg, -kv.first);
            }
            int after = maxNeg;
            int before = (n >= 0) ? n - after : maxPos + 1;
            if (before < maxPos + 1)
                before = maxPos + 1;
            for (int i = 0; i < before; i++) {
                auto it = h->seqItems.find(i);
                node->add(it == h->seqItems.end() ? wildcard() : resolve(it->second));
            }
            if (h->seqStar)
                node->add(new ASTMatchPattern(ASTMatchPattern::PAT_STAR));
            for (int k = after; k >= 1; k--) {
                auto it = h->seqItems.find(-k);
                node->add(it == h->seqItems.end() ? wildcard() : resolve(it->second));
            }
            return node;
        }
        /* Only wildcards: only the length is known */
        if (n < 0)
            n = 0;
        for (int i = 0; i < n; i++)
            node->add(wildcard());
        if (h->seqStar || h->seqLen < 0)
            node->add(new ASTMatchPattern(ASTMatchPattern::PAT_STAR));
        return node;
    }

    /* Count the POP_TOP instructions starting at byte offset `pos` */
    int popRun(int pos) const
    {
        int i = a.indexOf(pos);
        if (i < 0)
            return -1;
        int n = 0;
        while (i < (int)ins().size() && ins()[i].op == Pyc::POP_TOP) {
            n++;
            i++;
        }
        return n;
    }

    bool run(int startIdx, bool nonLast, MatchCase& out)
    {
        State s;
        size_t base;
        if (nonLast) {
            s.st.push_back(AV(AV::OUTER));
            base = 1;
        } else {
            top = newHole();
            s.st.push_back(AV(AV::HOLE, top));
            base = 0;
        }
        int idx = startIdx;
        try {
            exec(s, idx, MODE_NORMAL, 0, base, nullptr);
        } catch (const SimFail&) {
            return false;
        }
        if (!top || idx >= (int)ins().size())
            return false;
        if (ins()[idx].op == Pyc::JUMP_FORWARD_A)
            return false;   // an unfinished or-pattern, not a case body

        int bodyEnd = -1, nextPos = -1;
        for (const Edge& e : s.edges) {
            int expected = e.depth - (int)base;
            int pops = popRun(e.target);
            if (expected < 0 || pops != expected)
                return false;
            int chainEnd = e.target + pops * 2;
            if (bodyEnd < 0 || e.target < bodyEnd)
                bodyEnd = e.target;
            if (chainEnd > nextPos)
                nextPos = chainEnd;
        }
        if (inlinedBodyEnd >= 0 && (bodyEnd < 0 || inlinedBodyEnd < bodyEnd))
            bodyEnd = inlinedBodyEnd;
        if (bodyEnd >= 0 && bodyEnd <= ins()[idx].pos)
            return false;

        out.pattern = resolve(top);
        if (getenv("PYCDC_MATCH_DEBUG"))
            fprintf(stderr, "match case at %d: nonLast=%d bodyStart=%d bodyEnd=%d nextPos=%d\n",
                    ins()[startIdx].pos, (int)nonLast, ins()[idx].pos, bodyEnd, nextPos);
        out.nonLast = nonLast;
        out.bodyStart = ins()[idx].pos;
        out.bodyEnd = bodyEnd;
        out.nextPos = nextPos;
        return true;
    }
};

}  // namespace

bool MatchAnalyzer::parseCase(int pos, MatchCase& out)
{
    int i0 = indexOf(pos);
    if (i0 < 0)
        return false;
    if (isCopy1(m_ins[i0])) {
        /* `COPY; <alternative>` is ambiguous between a plain case that keeps
         * the subject and the first alternative of an or-pattern in the last
         * case: prefer the latter if it decodes consistently */
        MatchSim sim(*this);
        MatchCase tmp;
        if (sim.run(i0, false, tmp) && sim.top && sim.top->kind == Hole::OR) {
            out = tmp;
            return true;
        }
    }
    for (int hyp = 0; hyp < 2; hyp++) {
        MatchSim sim(*this);
        if (sim.run(i0, hyp == 0, out))
            return true;
    }
    return false;
}
