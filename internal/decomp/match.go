package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

// Static analysis of the structural pattern matching ("match" statement)
// bytecode emitted by Python 3.10+. The patterns of a case are decoded by
// symbolically executing the (very regular) pattern code; the bodies of the
// case blocks are left to the regular statement decompiler.

// PyInstr is one decoded instruction (CACHE entries are not part of the list).
type PyInstr struct {
	pos int // byte offset of the instruction (including EXTENDED_ARG prefix)
	end int // byte offset of the following instruction
	op  int
	arg int
}

// MatchCase describes one decoded `case`.
type MatchCase struct {
	pattern   *ast.MatchPattern
	nonLast   bool // the subject is kept on the stack for later cases
	bodyStart int  // offset right after the pattern (and its stores)
	bodyEnd   int  // offset where the failure cleanup starts (-1: none)
	nextPos   int  // offset where the next case / code after the match starts
}

// MatchAnalyzer decodes the instruction list of a code object for matching.
type MatchAnalyzer struct {
	code  *pyc.Code
	mod   *pyc.Module
	ins   []PyInstr
	index []int // byte offset -> instruction index (or -1)
}

func NewMatchAnalyzer(code *pyc.Code, mod *pyc.Module) *MatchAnalyzer {
	a := &MatchAnalyzer{code: code, mod: mod}
	a.index = make([]int, len(code.Code.Value)+2)
	for i := range a.index {
		a.index[i] = -1
	}
	for _, in := range decodeAll(code, mod) {
		if in.op == CACHE {
			continue
		}
		if in.pos < len(a.index) {
			a.index[in.pos] = len(a.ins)
		}
		a.ins = append(a.ins, PyInstr{in.pos, in.next, in.op, in.arg})
	}
	return a
}

func (a *MatchAnalyzer) Instrs() []PyInstr { return a.ins }

func (a *MatchAnalyzer) IndexOf(pos int) int {
	if pos < 0 || pos >= len(a.index) {
		return -1
	}
	return a.index[pos]
}

func (a *MatchAnalyzer) JumpTarget(in PyInstr) int {
	switch in.op {
	case POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A:
		if a.mod.VerCompare(3, 12) >= 0 {
			return in.end + in.arg*2
		}
		return in.arg * 2
	case POP_JUMP_IF_NONE_A, POP_JUMP_IF_NOT_NONE_A, POP_JUMP_FORWARD_IF_FALSE_A,
		POP_JUMP_FORWARD_IF_TRUE_A, POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_FORWARD_IF_NOT_NONE_A,
		JUMP_FORWARD_A:
		return in.end + in.arg*2
	}
	return -1
}

func isCopy1(in PyInstr) bool {
	return (in.op == COPY_A && in.arg == 1) || in.op == DUP_TOP
}

// IsMatchStart reports whether pos is a plausible first instruction of a
// `match` statement.
func (a *MatchAnalyzer) IsMatchStart(pos int) bool {
	i := a.IndexOf(pos)
	if i < 0 {
		return false
	}
	in := a.ins[i]
	switch in.op {
	case COPY_A:
		return in.arg == 1
	case DUP_TOP, MATCH_SEQUENCE, MATCH_MAPPING:
		return true
	case LOAD_GLOBAL_A, LOAD_NAME_A, LOAD_FAST_A, LOAD_DEREF_A:
		// A class pattern: LOAD <cls> (LOAD_ATTR)* LOAD_CONST <names> MATCH_CLASS
		for k := i + 1; k < len(a.ins) && k < i+8; k++ {
			if a.ins[k].op == MATCH_CLASS_A {
				return true
			}
			if a.ins[k].op != LOAD_ATTR_A && a.ins[k].op != LOAD_CONST_A {
				return false
			}
		}
	}
	return false
}

// ParseCase tries to decode a case whose pattern code starts at byte offset pos.
func (a *MatchAnalyzer) ParseCase(pos int, out *MatchCase) bool {
	i0 := a.IndexOf(pos)
	if i0 < 0 {
		return false
	}
	if isCopy1(a.ins[i0]) {
		// `COPY; <alternative>` is ambiguous between a plain case that keeps the
		// subject and the first alternative of an or-pattern in the last case:
		// prefer the latter if it decodes consistently.
		sim := newMatchSim(a)
		var tmp MatchCase
		if sim.run(i0, false, &tmp) && sim.top != nil && sim.top.kind == holeOR {
			*out = tmp
			return true
		}
	}
	for hyp := 0; hyp < 2; hyp++ {
		sim := newMatchSim(a)
		if sim.run(i0, hyp == 0, out) {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// Symbolic execution of the pattern code

type simFail struct{}
type floorHit struct{ idx int }

// abstract value kinds on the simulated stack
const (
	avHole    = iota // a piece of the subject: it still has to become a pattern
	avOuter          // the subject kept for the following cases (never consumed)
	avExpr           // a loaded expression (constant, name, attribute, tuple)
	avBool           // a test result
	avLen            // len() of a sequence hole
	avNegIdx         // len() - n
	avAttrs          // tuple of attributes (MATCH_CLASS)
	avVals           // tuple of values (MATCH_KEYS)
	avDict           // dict of the remaining mapping items (**rest)
	avKeyItem        // a key of a mapping pattern
)

type av struct {
	k int
	h *hole
	e ast.Node
	n int
}

func newAV(kind int, h *hole, n int) av { return av{k: kind, h: h, n: n} }

const (
	holeUnset = iota
	holeValue
	holeSeq
	holeMap
	holeClass
	holeOR
)

type hole struct {
	kind int
	expr ast.Node // VALUE: expression, CLASS: class

	// SEQ
	seqLen   int
	seqStar  bool
	seqItems map[int]*hole // fetched by index (negative: from the end)
	unpacked bool
	upEx     bool
	upBefore int
	upAfter  int
	upItems  []*hole

	// MAP
	keys      []ast.Node
	valItems  []*hole
	restNames []*pyc.String

	// CLASS
	nPos      int
	kwNames   []*pyc.String
	attrItems []*hole

	// OR
	alts []*hole

	// Names bound to this hole (capture or "as")
	names   []*pyc.String
	mirrors []*hole // holes of other alternatives bound to the same name
	isStar  bool
}

type edge struct{ target, depth int }

type simState struct {
	st    []av
	edges []edge
}

func (s simState) clone() simState {
	return simState{append([]av(nil), s.st...), append([]edge(nil), s.edges...)}
}

const (
	modeNormal = iota
	modeAlt
)

type matchSim struct {
	a              *MatchAnalyzer
	top            *hole
	inlinedBodyEnd int
}

func newMatchSim(a *MatchAnalyzer) *matchSim { return &matchSim{a: a, inlinedBodyEnd: -1} }

func (m *matchSim) newHole() *hole { return &hole{seqLen: -1, seqItems: map[int]*hole{}} }

func simPop(s *simState, floor int) av {
	if len(s.st) <= floor || s.st[len(s.st)-1].k == avOuter {
		panic(simFail{})
	}
	v := s.st[len(s.st)-1]
	s.st = s.st[:len(s.st)-1]
	return v
}

func needHole(v av) *hole {
	if v.k != avHole {
		panic(simFail{})
	}
	return v.h
}

func addName(h *hole, name *pyc.String) {
	h.names = append(h.names, name)
	for _, mh := range h.mirrors {
		addName(mh, name)
	}
}

func constInt(v av) (int, bool) {
	if v.k != avExpr || ast.TypeOf(v.e) != ast.NODE_OBJECT {
		return 0, false
	}
	obj := v.e.(*ast.Object).Object()
	if obj.Type() != pyc.TypeInt {
		return 0, false
	}
	return obj.(*pyc.Int).Value, true
}

func tupleItems(v av) []ast.Node {
	if v.k != avExpr {
		panic(simFail{})
	}
	var res []ast.Node
	if ast.TypeOf(v.e) == ast.NODE_TUPLE {
		res = append(res, v.e.(*ast.Tuple).Values()...)
	} else if ast.TypeOf(v.e) == ast.NODE_OBJECT {
		obj := v.e.(*ast.Object).Object()
		if obj.Type() != pyc.TypeTuple && obj.Type() != pyc.TypeSmallTuple {
			panic(simFail{})
		}
		for _, item := range obj.(*pyc.Seq).Values() {
			res = append(res, ast.NewObject(item))
		}
	} else {
		panic(simFail{})
	}
	return res
}

func (m *matchSim) cmpOp(arg int) int {
	mod := m.a.mod
	if mod.VerCompare(3, 12) == 0 {
		return arg >> 4
	}
	if mod.VerCompare(3, 13) >= 0 {
		return arg >> 5
	}
	return arg
}

func (m *matchSim) itemOf(items []*hole, i int) *hole {
	if i >= len(items) {
		panic(simFail{})
	}
	if items[i] == nil {
		items[i] = m.newHole()
	}
	return items[i]
}

// exec executes instructions starting at *idx.
// modeNormal: until the stack shrinks back to base entries.
// modeAlt: until the JUMP_FORWARD ending an alternative of an or-pattern;
// endJump receives the index of that jump.
func (m *matchSim) exec(s *simState, idx *int, mode int, floor, base int, endJump *int) {
	code := m.a.ins
	co := m.a.code
	mod := m.a.mod
	executed := 0
	for {
		if mode == modeNormal && executed > 0 && len(s.st) == base {
			return
		}
		if *idx < 0 || *idx >= len(code) {
			panic(simFail{})
		}
		in := code[*idx]
		if mode == modeAlt &&
			(in.op == STORE_FAST_A || in.op == STORE_NAME_A || in.op == STORE_GLOBAL_A ||
				in.op == STORE_DEREF_A ||
				(len(s.st) <= floor && (in.op == POP_TOP || in.op == SWAP_A))) {
			panic(floorHit{*idx})
		}
		executed++
		switch in.op {
		case NOP:
			*idx++
		case LOAD_CONST_A:
			v := av{k: avExpr, e: ast.NewObject(co.GetConst(in.arg))}
			s.st = append(s.st, v)
			*idx++
		case LOAD_GLOBAL_A, LOAD_NAME_A, LOAD_FAST_A, LOAD_DEREF_A:
			var name *pyc.String
			arg := in.arg
			switch in.op {
			case LOAD_GLOBAL_A:
				if mod.VerCompare(3, 11) >= 0 {
					if arg&1 != 0 {
						panic(simFail{})
					}
					arg >>= 1
				}
				name = co.GetName(arg)
			case LOAD_NAME_A:
				name = co.GetName(arg)
			case LOAD_FAST_A:
				name = co.GetLocal(arg)
			default:
				name = co.GetCellVar(mod, arg)
			}
			s.st = append(s.st, av{k: avExpr, e: ast.NewName(name)})
			*idx++
		case LOAD_ATTR_A:
			arg := in.arg
			if mod.VerCompare(3, 12) >= 0 {
				if arg&1 != 0 {
					panic(simFail{})
				}
				arg >>= 1
			}
			baseV := simPop(s, floor)
			if baseV.k != avExpr {
				panic(simFail{})
			}
			s.st = append(s.st, av{k: avExpr,
				e: ast.NewBinary(baseV.e, ast.NewName(co.GetName(arg)), ast.BIN_ATTR)})
			*idx++
		case BUILD_TUPLE_A:
			values := make([]ast.Node, in.arg)
			for i := 0; i < in.arg; i++ {
				v := simPop(s, floor)
				if v.k != avExpr {
					panic(simFail{})
				}
				values[in.arg-i-1] = v.e
			}
			s.st = append(s.st, av{k: avExpr, e: ast.NewTuple(values)})
			*idx++
		case DUP_TOP, COPY_A:
			depth := in.arg
			if in.op == DUP_TOP {
				depth = 1
			}
			if depth < 1 || depth > len(s.st) {
				panic(simFail{})
			}
			if depth == 1 && isCopy1(in) && s.st[len(s.st)-1].k == avHole {
				j := *idx
				if m.tryOr(s, &j) {
					*idx = j
					break
				}
			}
			v := s.st[len(s.st)-depth]
			if v.k == avOuter {
				// the first copy of the kept subject is the pattern subject
				h := m.newHole()
				if m.top == nil {
					m.top = h
				}
				v = newAV(avHole, h, 0)
			}
			s.st = append(s.st, v)
			*idx++
		case SWAP_A:
			if in.arg < 1 || in.arg > len(s.st) {
				panic(simFail{})
			}
			a, b := len(s.st)-1, len(s.st)-in.arg
			s.st[a], s.st[b] = s.st[b], s.st[a]
			if s.st[a].k == avOuter || s.st[b].k == avOuter {
				panic(simFail{})
			}
			*idx++
		case ROT_TWO, ROT_THREE, ROT_FOUR, ROT_N_A:
			var n int
			switch in.op {
			case ROT_TWO:
				n = 2
			case ROT_THREE:
				n = 3
			case ROT_FOUR:
				n = 4
			default:
				n = in.arg
			}
			if n < 2 || n > len(s.st) || len(s.st)-n < floor {
				panic(simFail{})
			}
			v := s.st[len(s.st)-1]
			s.st = s.st[:len(s.st)-1]
			at := len(s.st) - (n - 1)
			s.st = append(s.st, av{})
			copy(s.st[at+1:], s.st[at:])
			s.st[at] = v
			for i := 1; i <= n; i++ {
				if s.st[len(s.st)-i].k == avOuter {
					panic(simFail{})
				}
			}
			*idx++
		case POP_TOP:
			simPop(s, floor)
			*idx++
		case GET_LEN:
			if len(s.st) == 0 {
				panic(simFail{})
			}
			h := needHole(s.st[len(s.st)-1])
			s.st = append(s.st, newAV(avLen, h, 0))
			*idx++
		case BINARY_SUBTRACT, BINARY_OP_A:
			if in.op == BINARY_OP_A && binFromBinaryOp(in.arg) != ast.BIN_SUBTRACT {
				panic(simFail{})
			}
			r := simPop(s, floor)
			l := simPop(s, floor)
			k, ok := constInt(r)
			if l.k != avLen || !ok {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avNegIdx, l.h, k))
			*idx++
		case COMPARE_OP_A:
			cmp := m.cmpOp(in.arg)
			r := simPop(s, floor)
			l := simPop(s, floor)
			n, isInt := constInt(r)
			if l.k == avLen && isInt {
				if cmp == ast.CMP_EQUAL {
					l.h.seqLen = n
					l.h.seqStar = false
				} else if cmp == ast.CMP_GREATER_EQUAL {
					l.h.seqLen = n
					l.h.seqStar = true
				} else {
					panic(simFail{})
				}
			} else if l.k == avHole && r.k == avExpr && cmp == ast.CMP_EQUAL {
				if l.h.kind != holeUnset {
					panic(simFail{})
				}
				l.h.kind = holeValue
				l.h.expr = r.e
			} else {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avBool, nil, 0))
			*idx++
		case IS_OP_A:
			r := simPop(s, floor)
			l := simPop(s, floor)
			if l.k == avHole && r.k == avExpr && in.arg == 0 {
				if l.h.kind != holeUnset {
					panic(simFail{})
				}
				l.h.kind = holeValue
				l.h.expr = r.e
			} else if l.k != avAttrs && l.k != avVals {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avBool, nil, 0))
			*idx++
		case MATCH_SEQUENCE, MATCH_MAPPING:
			if len(s.st) == 0 {
				panic(simFail{})
			}
			h := needHole(s.st[len(s.st)-1])
			want := holeSeq
			if in.op == MATCH_MAPPING {
				want = holeMap
			}
			if h.kind != holeUnset && h.kind != want {
				panic(simFail{})
			}
			h.kind = want
			s.st = append(s.st, newAV(avBool, nil, 0))
			*idx++
		case MATCH_KEYS:
			if len(s.st) < 2 {
				panic(simFail{})
			}
			keys := s.st[len(s.st)-1]
			h := needHole(s.st[len(s.st)-2])
			if h.kind != holeMap {
				panic(simFail{})
			}
			h.keys = tupleItems(keys)
			h.valItems = make([]*hole, len(h.keys))
			s.st = append(s.st, newAV(avVals, h, 0))
			if mod.VerCompare(3, 11) < 0 {
				s.st = append(s.st, newAV(avBool, nil, 0))
			}
			*idx++
		case MATCH_CLASS_A:
			names := simPop(s, floor)
			cls := simPop(s, floor)
			subj := simPop(s, floor)
			h := needHole(subj)
			if h.kind != holeUnset || cls.k != avExpr {
				panic(simFail{})
			}
			kw := tupleItems(names)
			h.kind = holeClass
			h.expr = cls.e
			h.nPos = in.arg
			for _, n := range kw {
				if ast.TypeOf(n) != ast.NODE_OBJECT {
					panic(simFail{})
				}
				str, ok := n.(*ast.Object).Object().(*pyc.String)
				if !ok || str == nil {
					panic(simFail{})
				}
				h.kwNames = append(h.kwNames, str)
			}
			h.attrItems = make([]*hole, h.nPos+len(h.kwNames))
			s.st = append(s.st, newAV(avAttrs, h, 0))
			if mod.VerCompare(3, 11) < 0 {
				s.st = append(s.st, newAV(avBool, nil, 0))
			}
			*idx++
		case POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A, POP_JUMP_FORWARD_IF_FALSE_A,
			POP_JUMP_FORWARD_IF_TRUE_A, POP_JUMP_IF_NONE_A, POP_JUMP_IF_NOT_NONE_A,
			POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_FORWARD_IF_NOT_NONE_A:
			c := simPop(s, floor)
			if in.op == POP_JUMP_IF_NOT_NONE_A || in.op == POP_JUMP_FORWARD_IF_NOT_NONE_A {
				// jumps (fails) unless the value is None: `case None:`
				h := needHole(c)
				if h.kind != holeUnset {
					panic(simFail{})
				}
				h.kind = holeValue
				h.expr = ast.NewObject(pyc.None)
			} else if in.op == POP_JUMP_IF_NONE_A || in.op == POP_JUMP_FORWARD_IF_NONE_A {
				if c.k != avAttrs && c.k != avVals {
					panic(simFail{})
				}
			} else if c.k != avBool {
				panic(simFail{})
			}
			target := m.a.JumpTarget(in)
			if target <= in.pos {
				panic(simFail{})
			}
			s.edges = append(s.edges, edge{target, len(s.st)})
			*idx++
		case JUMP_FORWARD_A:
			if mode != modeAlt {
				panic(simFail{})
			}
			*endJump = *idx
			return
		case UNPACK_SEQUENCE_A:
			v := simPop(s, floor)
			var items []*hole
			switch v.k {
			case avHole:
				h := v.h
				if h.kind != holeSeq || h.unpacked {
					panic(simFail{})
				}
				h.unpacked = true
				for i := 0; i < in.arg; i++ {
					h.upItems = append(h.upItems, m.newHole())
				}
				items = h.upItems
			case avAttrs:
				if len(v.h.attrItems) != in.arg {
					panic(simFail{})
				}
				for i := 0; i < in.arg; i++ {
					items = append(items, m.itemOf(v.h.attrItems, i))
				}
			case avVals:
				if len(v.h.valItems) != in.arg {
					panic(simFail{})
				}
				for i := 0; i < in.arg; i++ {
					items = append(items, m.itemOf(v.h.valItems, i))
				}
			case avExpr:
				// the (constant) keys of a mapping, pushed to be deleted from **rest
				if len(tupleItems(v)) != in.arg {
					panic(simFail{})
				}
				for i := 0; i < in.arg; i++ {
					s.st = append(s.st, newAV(avKeyItem, nil, 0))
				}
				*idx++
				continue
			default:
				panic(simFail{})
			}
			for i := len(items) - 1; i >= 0; i-- {
				s.st = append(s.st, newAV(avHole, items[i], 0))
			}
			*idx++
		case UNPACK_EX_A:
			v := simPop(s, floor)
			h := needHole(v)
			if h.kind != holeSeq || h.unpacked {
				panic(simFail{})
			}
			h.unpacked = true
			h.upEx = true
			h.upBefore = in.arg & 0xFF
			h.upAfter = (in.arg >> 8) & 0xFF
			total := h.upBefore + 1 + h.upAfter
			for i := 0; i < total; i++ {
				item := m.newHole()
				item.isStar = i == h.upBefore
				h.upItems = append(h.upItems, item)
			}
			for i := total - 1; i >= 0; i-- {
				s.st = append(s.st, newAV(avHole, h.upItems[i], 0))
			}
			*idx++
		case BINARY_SUBSCR:
			index := simPop(s, floor)
			baseV := simPop(s, floor)
			var item *hole
			if i, ok := constInt(index); baseV.k == avAttrs && ok {
				item = m.itemOf(baseV.h.attrItems, i)
			} else if baseV.k == avVals && ok {
				item = m.itemOf(baseV.h.valItems, i)
			} else if baseV.k == avHole && baseV.h.kind == holeSeq {
				var key int
				if ok {
					key = i
				} else if index.k == avNegIdx && index.h == baseV.h {
					key = -index.n
				} else {
					panic(simFail{})
				}
				slot := baseV.h.seqItems[key]
				if slot == nil {
					slot = m.newHole()
					baseV.h.seqItems[key] = slot
				}
				item = slot
			} else {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avHole, item, 0))
			*idx++
		case BUILD_MAP_A:
			if in.arg != 0 {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avDict, nil, 0))
			*idx++
		case DICT_UPDATE_A:
			upd := simPop(s, floor)
			h := needHole(upd)
			if h.kind != holeMap || in.arg < 1 || in.arg > len(s.st) {
				panic(simFail{})
			}
			d := &s.st[len(s.st)-in.arg]
			if d.k != avDict || d.h != nil {
				panic(simFail{})
			}
			d.h = h
			*idx++
		case COPY_DICT_WITHOUT_KEYS:
			keys := simPop(s, floor)
			if keys.k != avExpr || len(s.st) == 0 {
				panic(simFail{})
			}
			h := needHole(s.st[len(s.st)-1])
			if h.kind != holeMap {
				panic(simFail{})
			}
			s.st = append(s.st, newAV(avDict, h, 0))
			*idx++
		case DELETE_SUBSCR:
			key := simPop(s, floor)
			dict := simPop(s, floor)
			if key.k != avKeyItem && key.k != avExpr {
				panic(simFail{})
			}
			if dict.k != avDict {
				panic(simFail{})
			}
			*idx++
		case STORE_FAST_A, STORE_NAME_A, STORE_GLOBAL_A, STORE_DEREF_A:
			if mode == modeAlt {
				panic(simFail{})
			}
			var name *pyc.String
			switch in.op {
			case STORE_FAST_A:
				name = co.GetLocal(in.arg)
			case STORE_DEREF_A:
				name = co.GetCellVar(mod, in.arg)
			default:
				name = co.GetName(in.arg)
			}
			v := simPop(s, floor)
			if v.k == avHole {
				addName(v.h, name)
			} else if v.k == avDict && v.h != nil {
				v.h.restNames = append(v.h.restNames, name)
			} else {
				panic(simFail{})
			}
			*idx++
		default:
			panic(simFail{})
		}
	}
}

// isNoneReturn reports whether there is an implicit `return None` at index q.
func (m *matchSim) isNoneReturn(q int) bool {
	code := m.a.ins
	co := m.a.code
	isNone := func(arg int) bool { return co.GetConst(arg).Type() == pyc.TypeNone }
	if code[q].op == RETURN_CONST_A {
		return isNone(code[q].arg)
	}
	return code[q].op == LOAD_CONST_A && isNone(code[q].arg) &&
		q+1 < len(code) && code[q+1].op == RETURN_VALUE
}

func isTerminator(in PyInstr) bool {
	return in.op == RETURN_VALUE || in.op == RETURN_CONST_A || in.op == RAISE_VARARGS_A
}

// execAlt runs one or-pattern alternative. It returns the index of the
// instruction where the alternative hit the floor (or -1), and false when the
// alternative cannot be decoded.
func (m *matchSim) execAlt(alt *simState, j *int, floor int, jumpIdx *int) (sIdx int, ok bool) {
	defer func() {
		if r := recover(); r != nil {
			switch v := r.(type) {
			case simFail:
				sIdx, ok = -1, false
			case floorHit:
				sIdx, ok = v.idx, true
			default:
				panic(r)
			}
		}
	}()
	m.exec(alt, j, modeAlt, floor, 0, jumpIdx)
	return -1, true
}

type orAlt struct {
	f       *hole
	residue []av
}

// tryOr tries to read an or-pattern whose first alternative starts at *idx
// (a COPY of the subject). On success the state and *idx are updated to
// continue at the code following the or-pattern.
//
// Two layouts exist: every alternative ends with a JUMP_FORWARD to a shared
// block, or (small exit blocks) the shared block is duplicated after each
// alternative.
func (m *matchSim) tryOr(s *simState, idx *int) bool {
	code := m.a.ins
	if len(s.st) == 0 || s.st[len(s.st)-1].k != avHole {
		return false
	}
	subject := s.st[len(s.st)-1].h
	n0 := len(s.st)
	base := s.clone()
	base.edges = nil

	var alts []orAlt
	endPos := -1
	cur := *idx
	form := 0 // 1: jump to shared block, 2: duplicated block
	firstS, firstQ0 := -1, -1
	var tailEdges []edge

	for {
		if cur >= len(code) || !isCopy1(code[cur]) {
			return false
		}
		alt := base.clone()
		f := m.newHole()
		alt.st = append(alt.st, newAV(avHole, f, 0))
		j := cur + 1
		jumpIdx := -1
		sIdx, ok := m.execAlt(&alt, &j, n0, &jumpIdx)
		if !ok {
			return false
		}
		myForm := 2
		if sIdx < 0 {
			myForm = 1
		}
		if form == 0 {
			form = myForm
		} else if form != myForm {
			return false
		}

		rec := orAlt{f: f, residue: append([]av(nil), alt.st[n0:]...)}
		alts = append(alts, rec)

		if form == 1 {
			jump := code[jumpIdx]
			target := m.a.JumpTarget(jump)
			if endPos < 0 {
				endPos = target
			} else if endPos != target {
				return false
			}

			q := jumpIdx + 1
			for q < len(code) && code[q].op == POP_TOP {
				q++
			}
			if q >= len(code) {
				return false
			}

			for _, e := range alt.edges {
				if e.target < jump.end || e.target > code[q].pos {
					return false
				}
			}

			if isCopy1(code[q]) {
				cur = q
				continue
			}

			var endIdx int
			if code[q].pos == endPos {
				endIdx = q
			} else if code[q].op == JUMP_FORWARD_A && code[q].end == endPos {
				outer := m.a.JumpTarget(code[q])
				if outer <= code[q].pos {
					return false
				}
				tailEdges = append(tailEdges, edge{outer, n0 - 1})
				endIdx = m.a.IndexOf(endPos)
			} else if m.isNoneReturn(q) && code[q+boolInt(code[q].op == RETURN_CONST_A, 1, 2)-1].end == endPos {
				endIdx = m.a.IndexOf(endPos)
			} else {
				return false
			}
			if endIdx < 0 || len(alts) < 2 {
				return false
			}
			if !joinAlts(s, subject, alts, tailEdges) {
				return false
			}
			*idx = endIdx
			return true
		}

		// Duplicated exit block: find its end
		k := sIdx
		for k < len(code) && k-sIdx < 8 && !isTerminator(code[k]) {
			op := code[k].op
			if op == POP_TOP || op == STORE_FAST_A || op == LOAD_FAST_A || op == LOAD_CONST_A ||
				op == LOAD_GLOBAL_A || op == LOAD_NAME_A || op == STORE_NAME_A || op == NOP ||
				op == STORE_GLOBAL_A {
				k++
				continue
			}
			return false
		}
		if k >= len(code) || !isTerminator(code[k]) {
			return false
		}
		q0 := k + 1
		if firstS < 0 {
			firstS = sIdx
			firstQ0 = q0
		} else {
			if q0-sIdx != firstQ0-firstS {
				return false
			}
			for t := 0; t < q0-sIdx; t++ {
				if code[sIdx+t].op != code[firstS+t].op || code[sIdx+t].arg != code[firstS+t].arg {
					return false
				}
			}
		}
		r := 0
		for q0+r < len(code) && code[q0+r].op == POP_TOP {
			r++
		}
		mm := 0
		for _, e := range alt.edges {
			dd := e.depth - n0
			if dd < 0 {
				return false
			}
			if dd > mm {
				mm = dd
			}
		}
		if len(alt.edges) == 0 {
			return false
		}
		for _, e := range alt.edges {
			want := q0 + mm - (e.depth - n0)
			if want >= len(code) || code[want].pos != e.target {
				return false
			}
		}
		deepest := false
		for _, e := range alt.edges {
			if e.target == code[q0].pos {
				deepest = true
			}
		}
		if !deepest {
			return false
		}
		if r == mm && q0+r < len(code) && isCopy1(code[q0+r]) {
			cur = q0 + r
			continue
		}
		if r < mm+1 || len(alts) < 2 || q0+mm+1 >= len(code) {
			return false
		}
		tailEdges = append(tailEdges, edge{code[q0+mm+1].pos, n0 - 1})
		if !joinAlts(s, subject, alts, tailEdges) {
			return false
		}
		m.inlinedBodyEnd = code[firstQ0].pos
		*idx = firstS
		return true
	}
}

func boolInt(c bool, a, b int) int {
	if c {
		return a
	}
	return b
}

func joinAlts(s *simState, subject *hole, alts []orAlt, tailEdges []edge) bool {
	for i := 1; i < len(alts); i++ {
		if len(alts[i].residue) != len(alts[0].residue) {
			return false
		}
	}
	for mi := range alts[0].residue {
		if alts[0].residue[mi].k != avHole {
			return false
		}
		for i := 1; i < len(alts); i++ {
			if alts[i].residue[mi].k != avHole {
				return false
			}
			alts[0].residue[mi].h.mirrors = append(alts[0].residue[mi].h.mirrors, alts[i].residue[mi].h)
		}
	}
	subject.kind = holeOR
	for _, al := range alts {
		subject.alts = append(subject.alts, al.f)
	}
	s.st = append(s.st, alts[0].residue...)
	s.edges = append(s.edges, tailEdges...)
	return true
}

// ---------------------------------------------------------------------------
// Conversion of the decoded holes to pattern nodes

func wrapAs(inner *ast.MatchPattern, name *pyc.String) *ast.MatchPattern {
	as := ast.NewMatchPattern(ast.PAT_AS)
	as.Add(inner)
	as.SetName(name)
	return as
}

func wildcard() *ast.MatchPattern { return ast.NewMatchPattern(ast.PAT_WILDCARD) }

func (m *matchSim) resolve(h *hole) *ast.MatchPattern {
	var node *ast.MatchPattern
	switch h.kind {
	case holeUnset:
		if h.isStar {
			node = ast.NewMatchPattern(ast.PAT_STAR)
			if len(h.names) != 0 {
				node.SetName(h.names[0])
			}
			return node
		}
		if len(h.names) == 0 {
			return ast.NewMatchPattern(ast.PAT_WILDCARD)
		}
		node = ast.NewMatchPattern(ast.PAT_CAPTURE)
		node.SetName(h.names[0])
		for i := 1; i < len(h.names); i++ {
			node = wrapAs(node, h.names[i])
		}
		return node
	case holeValue:
		node = ast.NewMatchPattern(ast.PAT_VALUE)
		node.SetExpr(h.expr)
	case holeSeq:
		node = m.resolveSeq(h)
	case holeMap:
		node = ast.NewMatchPattern(ast.PAT_MAPPING)
		for i := range h.keys {
			node.AddKey(h.keys[i])
			if i < len(h.valItems) && h.valItems[i] != nil {
				node.Add(m.resolve(h.valItems[i]))
			} else {
				node.Add(ast.NewMatchPattern(ast.PAT_WILDCARD))
			}
		}
		if len(h.restNames) != 0 {
			node.SetName(h.restNames[0])
		}
	case holeClass:
		node = ast.NewMatchPattern(ast.PAT_CLASS)
		node.SetExpr(h.expr)
		node.SetNumPositional(h.nPos)
		for i := range h.attrItems {
			if h.attrItems[i] != nil {
				node.Add(m.resolve(h.attrItems[i]))
			} else {
				node.Add(ast.NewMatchPattern(ast.PAT_WILDCARD))
			}
		}
		for _, n := range h.kwNames {
			node.AddKwName(n)
		}
	case holeOR:
		node = ast.NewMatchPattern(ast.PAT_OR)
		for _, alt := range h.alts {
			node.Add(m.resolve(alt))
		}
	}
	for _, name := range h.names {
		node = wrapAs(node, name)
	}
	return node
}

func (m *matchSim) resolveSeq(h *hole) *ast.MatchPattern {
	node := ast.NewMatchPattern(ast.PAT_SEQUENCE)
	if h.unpacked {
		for _, item := range h.upItems {
			node.Add(m.resolve(item))
		}
		return node
	}
	n := h.seqLen
	if len(h.seqItems) != 0 {
		maxPos, maxNeg := -1, 0
		for k := range h.seqItems {
			if k >= 0 {
				if k > maxPos {
					maxPos = k
				}
			} else if -k > maxNeg {
				maxNeg = -k
			}
		}
		after := maxNeg
		before := maxPos + 1
		if n >= 0 {
			before = n - after
		}
		if before < maxPos+1 {
			before = maxPos + 1
		}
		for i := 0; i < before; i++ {
			if it, ok := h.seqItems[i]; ok {
				node.Add(m.resolve(it))
			} else {
				node.Add(wildcard())
			}
		}
		if h.seqStar {
			node.Add(ast.NewMatchPattern(ast.PAT_STAR))
		}
		for k := after; k >= 1; k-- {
			if it, ok := h.seqItems[-k]; ok {
				node.Add(m.resolve(it))
			} else {
				node.Add(wildcard())
			}
		}
		return node
	}
	// Only wildcards: only the length is known
	if n < 0 {
		n = 0
	}
	for i := 0; i < n; i++ {
		node.Add(wildcard())
	}
	if h.seqStar || h.seqLen < 0 {
		node.Add(ast.NewMatchPattern(ast.PAT_STAR))
	}
	return node
}

// popRun counts the POP_TOP instructions starting at byte offset pos.
func (m *matchSim) popRun(pos int) int {
	i := m.a.IndexOf(pos)
	if i < 0 {
		return -1
	}
	n := 0
	for i < len(m.a.ins) && m.a.ins[i].op == POP_TOP {
		n++
		i++
	}
	return n
}

func (m *matchSim) run(startIdx int, nonLast bool, out *MatchCase) (ok bool) {
	var s simState
	var base int
	if nonLast {
		s.st = append(s.st, newAV(avOuter, nil, 0))
		base = 1
	} else {
		m.top = m.newHole()
		s.st = append(s.st, newAV(avHole, m.top, 0))
		base = 0
	}
	idx := startIdx
	func() {
		defer func() {
			if r := recover(); r != nil {
				if _, isFail := r.(simFail); isFail {
					ok = false
					idx = -1
					return
				}
				panic(r)
			}
		}()
		m.exec(&s, &idx, modeNormal, 0, base, nil)
		ok = true
	}()
	if !ok {
		return false
	}
	ins := m.a.ins
	if m.top == nil || idx >= len(ins) {
		return false
	}
	if ins[idx].op == JUMP_FORWARD_A {
		return false // an unfinished or-pattern, not a case body
	}

	bodyEnd, nextPos := -1, -1
	for _, e := range s.edges {
		expected := e.depth - base
		pops := m.popRun(e.target)
		if expected < 0 || pops != expected {
			return false
		}
		chainEnd := e.target + pops*2
		if bodyEnd < 0 || e.target < bodyEnd {
			bodyEnd = e.target
		}
		if chainEnd > nextPos {
			nextPos = chainEnd
		}
	}
	if m.inlinedBodyEnd >= 0 && (bodyEnd < 0 || m.inlinedBodyEnd < bodyEnd) {
		bodyEnd = m.inlinedBodyEnd
	}
	if bodyEnd >= 0 && bodyEnd <= ins[idx].pos {
		return false
	}

	out.pattern = m.resolve(m.top)
	out.nonLast = nonLast
	out.bodyStart = ins[idx].pos
	out.bodyEnd = bodyEnd
	out.nextPos = nextPos
	return true
}
