package decomp

import (
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

// chainJump describes one earlier jump of a compound `while` condition.
type chainJump struct {
	pos            int
	ifTrue, toBody bool
}

// WhileLoops is the loop structure recovered from the control-flow graph, for
// `while` loops of Python 3.8 - 3.14 (a `while` has no dedicated opcode):
//
//	top-tested (<=3.9, and `while True`):  T: cond; guard -> end; body; JUMP T
//	bottom-tested (>=3.10):                guard -> end; T: body; cond; JUMP_IF T
//	                                       (3.12+: cond; JUMP_IF_FALSE end; JUMP_BACKWARD T)
//
// Besides the loop frame we record which jumps are `break` / `continue`.
type WhileLoops struct {
	cleanupDest map[int]int  // chained-comparison cleanup POP_TOP -> real exit
	chainSkip   []intPair    // cleanup/trailer ranges to ignore
	guardEnd    map[int]int  // guard position -> loop end
	trueOpen    map[int]int  // header position -> loop end (`while True`)
	backedges   map[int]bool // conditional back-edges (pop the condition)
	uncondBack  map[int]bool // unconditional closing back-edges
	skipJumps   map[int]bool // instructions that belong to a back-edge
	breaks      map[int]bool
	continues   map[int]bool
	// `if c: break` at the tail of a loop whose break jump was elided (3.14):
	// conditional jump position -> true when the jump is taken on a true test
	breakIf map[int]bool
	// `for` loops: POP_TOP of the iterator + the exit jump that form a break
	forBreakPops map[int]bool
	forBreaks    map[int]bool
	forElse      map[int]int // start of a for/else `else` body -> its end
	elseEnd      map[int]int // loop end -> end of its `else` body
	loopEnds     map[int]bool
	forTryEnd    map[int]bool // 3.10: `for` loop exits right after its closing try
	tryEndJump   map[int]int  // try/handler-end jump to the loop head -> closing back-edge
	// compound loop conditions (`while a and b`) of bottom-tested loops
	guardChain       map[int][]chainJump // last guard jump -> earlier jumps
	guardChainMember map[int]bool        // those earlier guard jumps
	tailChain        map[int]bool        // jumps of the duplicated condition
}

func NewWhileLoops() *WhileLoops {
	return &WhileLoops{
		cleanupDest: map[int]int{}, guardEnd: map[int]int{}, trueOpen: map[int]int{},
		backedges: map[int]bool{}, uncondBack: map[int]bool{}, skipJumps: map[int]bool{},
		breaks: map[int]bool{}, continues: map[int]bool{}, breakIf: map[int]bool{},
		forBreakPops: map[int]bool{}, forBreaks: map[int]bool{}, forElse: map[int]int{},
		elseEnd: map[int]int{}, loopEnds: map[int]bool{}, forTryEnd: map[int]bool{},
		tryEndJump: map[int]int{}, guardChain: map[int][]chainJump{},
		guardChainMember: map[int]bool{}, tailChain: map[int]bool{},
	}
}

// scanChainCleanups handles a chained comparison in a jump context
// (`if a < b < c:`, `not`, `or`, `while`, ternary tests). The first half's
// false jump lands on a lone `POP_TOP` (discarding the shared middle operand),
// which then either jumps on to the real false exit or falls into it. It
// records dest[POP_TOP] = real exit so every jump to the cleanup is read as a
// jump to the exit, and lists the cleanup (plus the trailer jump that hops
// over it) in skip.
func scanChainCleanups(code *pyc.Code, mod *pyc.Module, dest map[int]int, skip *[]intPair) {
	ins := decodeAll(code, mod)
	n := len(ins)
	for i := n - 1; i >= 0; i-- {
		if i+1 < n && ins[i+1].op == CACHE && ins[i].op != CACHE {
			ins[i].real = ins[i+1].real
		}
	}
	idxAt := map[int]int{}
	for i := 0; i < n; i++ {
		idxAt[ins[i].pos] = i
	}
	unit := 1
	if mod.VerCompare(3, 10) >= 0 {
		unit = 2
	}
	prevReal := func(i int) int {
		i--
		for i >= 0 && (ins[i].op == CACHE || ins[i].op == NOT_TAKEN) {
			i--
		}
		return i
	}
	nextReal := func(i int) int {
		i++
		for i < n && (ins[i].op == CACHE || ins[i].op == NOT_TAKEN) {
			i++
		}
		return i
	}
	falseJump := func(in *rawIns) int {
		switch in.op {
		case POP_JUMP_IF_FALSE_A:
			if mod.VerCompare(3, 12) >= 0 {
				return in.real + in.arg*2
			}
			return in.arg * unit
		case POP_JUMP_FORWARD_IF_FALSE_A:
			return in.real + in.arg*2
		}
		return -1
	}
	fwdJump := func(in *rawIns) int {
		if in.op == JUMP_FORWARD_A {
			return in.real + in.arg*unit
		}
		if in.op == JUMP_ABSOLUTE_A && mod.VerCompare(3, 11) < 0 {
			t := in.arg * unit
			if t > in.pos {
				return t
			}
			return -1
		}
		return -1
	}
	isCmp := func(op int) bool { return op == COMPARE_OP_A || op == IS_OP_A || op == CONTAINS_OP_A }
	for i := 0; i < n; i++ {
		t := falseJump(&ins[i])
		if t <= ins[i].pos {
			continue
		}
		cmp := prevReal(i)
		if cmp < 0 || !isCmp(ins[cmp].op) {
			continue
		}
		h1 := prevReal(cmp)
		h2 := -1
		if h1 >= 0 {
			h2 = prevReal(h1)
		}
		head := false
		if h1 >= 0 && h2 >= 0 {
			if mod.VerCompare(3, 11) >= 0 {
				head = ins[h1].op == COPY_A && ins[h1].arg == 2 && ins[h2].op == SWAP_A && ins[h2].arg == 2
			} else {
				head = ins[h1].op == ROT_THREE && ins[h2].op == DUP_TOP
			}
		}
		if !head {
			continue
		}
		s, ok := idxAt[t]
		if _, has := dest[t]; !ok || ins[s].op != POP_TOP || has {
			continue
		}
		after := nextReal(s)
		if after >= n {
			continue
		}
		target, endIdx := ins[after].pos, after
		J := fwdJump(&ins[after])
		if J > 0 {
			target = J
			endIdx = nextReal(after)
		} else {
			u := -1
			for k := i + 1; k < s && u < 0; k++ {
				ft := falseJump(&ins[k])
				if ft > ins[s].pos {
					u = ft
				}
			}
			ui, uok := -1, false
			if u >= 0 {
				ui, uok = idxAt[u]
			}
			if uok {
				a, b := after, ui
				same := false
				for a < n && b < n && ins[a].op == ins[b].op && ins[a].arg == ins[b].arg {
					if ins[a].op == RETURN_VALUE || ins[a].op == RETURN_CONST_A {
						same = true
						break
					}
					if fwdJump(&ins[a]) > 0 || falseJump(&ins[a]) > 0 ||
						ins[a].op == POP_JUMP_IF_TRUE_A || ins[a].op == JUMP_BACKWARD_A ||
						ins[a].op == FOR_ITER_A {
						break
					}
					a = nextReal(a)
					b = nextReal(b)
					if a >= n || b >= n {
						break
					}
				}
				if same {
					target = u
					endIdx = nextReal(a)
				}
			}
		}
		if di, ok := idxAt[target]; ok && target > ins[s].pos {
			d := &ins[di]
			if d.op == JUMP_ABSOLUTE_A && mod.VerCompare(3, 11) < 0 && d.arg*unit < d.pos {
				target = d.arg * unit
			} else if d.op == JUMP_BACKWARD_A {
				target = d.real - d.arg*2
			}
		}
		endPos := ins[n-1].real
		if endIdx < n {
			endPos = ins[endIdx].pos
		}
		skipFrom := t
		before := prevReal(s)
		if before >= 0 && fwdJump(&ins[before]) == endPos {
			skipFrom = ins[before].pos
			dest[skipFrom] = endPos
		}
		if before >= 0 && endIdx < n && endIdx != after &&
			(ins[before].op == JUMP_ABSOLUTE_A || ins[before].op == JUMP_BACKWARD_A) &&
			ins[endIdx].op == ins[before].op && ins[endIdx].arg == ins[before].arg &&
			skipFrom == t {
			endIdx = nextReal(endIdx)
			if endIdx < n {
				endPos = ins[endIdx].pos
			} else {
				endPos = ins[n-1].real
			}
		}
		dest[t] = target
		*skip = append(*skip, intPair{skipFrom, endPos})
	}
}

type jumpKind int

const (
	jkNone jumpKind = iota
	jkUncond
	jkCond
)

type loopFrame struct {
	T, B, end, csStart, firstBack int
	typeII                        bool
}

func scanLoops(code *pyc.Code, mod *pyc.Module, out *WhileLoops) {
	scanChainCleanups(code, mod, out.cleanupDest, &out.chainSkip)
	var hoistedRegions []intPair
	ignoreBack := map[int]bool{}
	ins := decodeAll(code, mod)
	for i := len(ins) - 1; i >= 0; i-- {
		if i+1 < len(ins) && ins[i+1].op == CACHE && ins[i].op != CACHE {
			ins[i].real = ins[i+1].real
		}
	}
	if mod.VerCompare(3, 12) >= 0 {
		hoisted := map[int][2]int{}
		scanHoistedHandlers(code, mod, code.ExceptionTableEntries(), hoisted)
		for _, hk := range sortedKeys(hoisted) {
			h := hoisted[hk]
			hoistedRegions = append(hoistedRegions, intPair{h[0], h[1]})
			for _, in := range ins {
				if in.op == JUMP_BACKWARD_A && in.pos >= h[0] && in.pos < h[1] && in.real-in.arg*2 > hk {
					ignoreBack[in.pos] = true
				}
			}
		}
		if len(hoisted) != 0 {
			var kept []rawIns
			for _, in := range ins {
				drop := false
				if in.op == JUMP_BACKWARD_A {
					tgt := in.real - in.arg*2
					for hk, h := range hoisted {
						if in.pos >= h[0] && in.pos < h[1] && tgt == hk {
							drop = true
						}
					}
				}
				if !drop {
					kept = append(kept, in)
				}
			}
			ins = kept
		}
	}
	for i := range ins {
		ins[i].fall = ins[i].real
	}
	if len(out.chainSkip) != 0 {
		var kept []rawIns
		for _, in := range ins {
			skipIt := false
			for _, r := range out.chainSkip {
				if in.pos >= r.a && in.pos < r.b {
					skipIt = true
				}
			}
			if !skipIt {
				kept = append(kept, in)
			}
		}
		for i := range kept {
			for moved := true; moved; {
				moved = false
				for _, r := range out.chainSkip {
					if kept[i].fall == r.a {
						kept[i].fall = r.b
						moved = true
					}
				}
			}
		}
		ins = kept
	}
	idxAt := map[int]int{}
	for i := range ins {
		idxAt[ins[i].pos] = i
	}

	unit := 1
	if mod.VerCompare(3, 10) >= 0 {
		unit = 2
	}
	jumpRaw := func(in *rawIns, target *int) jumpKind {
		switch in.op {
		case JUMP_ABSOLUTE_A:
			*target = in.arg * unit
			return jkUncond
		case JUMP_FORWARD_A:
			*target = in.real + in.arg*unit
			return jkUncond
		case JUMP_BACKWARD_A, JUMP_BACKWARD_NO_INTERRUPT_A:
			*target = in.real - in.arg*2
			return jkUncond
		case POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A:
			if mod.VerCompare(3, 12) >= 0 {
				*target = in.real + in.arg*2
			} else {
				*target = in.arg * unit
			}
			return jkCond
		case POP_JUMP_IF_NONE_A, POP_JUMP_IF_NOT_NONE_A:
			*target = in.real + in.arg*2
			return jkCond
		case POP_JUMP_FORWARD_IF_FALSE_A, POP_JUMP_FORWARD_IF_TRUE_A,
			POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_FORWARD_IF_NOT_NONE_A:
			*target = in.real + in.arg*2
			return jkCond
		case POP_JUMP_BACKWARD_IF_FALSE_A, POP_JUMP_BACKWARD_IF_TRUE_A,
			POP_JUMP_BACKWARD_IF_NONE_A, POP_JUMP_BACKWARD_IF_NOT_NONE_A:
			*target = in.real - in.arg*2
			return jkCond
		}
		return jkNone
	}
	jump := func(in *rawIns, target *int) jumpKind {
		k := jumpRaw(in, target)
		if k == jkCond && *target > in.pos {
			if cd, ok := out.cleanupDest[*target]; ok {
				*target = cd
			}
		}
		if k != jkNone && *target < in.pos {
			it, ok := idxAt[*target]
			for ok && ins[it].op == NOP && it+1 < len(ins) {
				*target = ins[it+1].pos
				it, ok = idxAt[*target]
			}
		}
		return k
	}
	isExprOp := func(op int) bool {
		switch op {
		case LOAD_FAST_A, LOAD_CONST_A, LOAD_NAME_A, LOAD_GLOBAL_A, LOAD_DEREF_A, LOAD_ATTR_A,
			LOAD_METHOD_A, COMPARE_OP_A, IS_OP_A, CONTAINS_OP_A, BINARY_OP_A, BINARY_SUBSCR,
			UNARY_NOT, UNARY_NEGATIVE, CALL_A, CALL_FUNCTION_A, CALL_METHOD_A, PRECALL_A,
			PUSH_NULL, KW_NAMES_A, TO_BOOL, NOT_TAKEN, DUP_TOP, ROT_THREE, SWAP_A, COPY_A,
			CACHE, LOAD_FAST_LOAD_FAST_A, LOAD_FAST_BORROW_A, LOAD_FAST_BORROW_LOAD_FAST_BORROW_A,
			LOAD_SMALL_INT_A, BINARY_ADD, BINARY_SUBTRACT, BINARY_MULTIPLY, BINARY_MODULO,
			BINARY_TRUE_DIVIDE, BINARY_FLOOR_DIVIDE:
			return true
		}
		return false
	}

	var loops []loopFrame

	// back-edge candidates grouped by header
	lastUncond := map[int]int{} // T -> last unconditional back-jump index
	for i := range ins {
		var t int
		if jump(&ins[i], &t) == jkUncond && t < ins[i].pos &&
			ins[i].op != JUMP_FORWARD_A && !ignoreBack[ins[i].pos] {
			ti, ok := idxAt[t]
			if !ok || ins[ti].op == FOR_ITER_A {
				continue
			}
			lastUncond[t] = i
		}
	}
	// The end of a try body / except handler that threads straight back to the
	// loop head, when the whole `try` closes the loop body, is a jump over the
	// remaining handlers and not a `continue`.
	for i := 1; i < len(ins); i++ {
		var t int
		if ins[i].op != JUMP_ABSOLUTE_A || jump(&ins[i], &t) != jkUncond || t >= ins[i].pos {
			continue
		}
		if ins[i-1].op != POP_BLOCK && ins[i-1].op != POP_EXCEPT {
			continue
		}
		{
			hi, ok := idxAt[t]
			if ok && ins[hi].op == FOR_ITER_A {
				ft := ins[hi].real + ins[hi].arg*unit
				ei, eok := idxAt[ft]
				if eok && ei > 0 && (ins[ei-1].op == RERAISE_A || ins[ei-1].op == RERAISE ||
					ins[ei-1].op == END_FINALLY) {
					out.tryEndJump[ins[i].pos] = ft
					out.forTryEnd[ft] = true
					continue
				}
			}
		}
		for k := i + 1; k < len(ins); k++ {
			var t2 int
			if ins[k].op != JUMP_ABSOLUTE_A || jump(&ins[k], &t2) != jkUncond || t2 != t {
				continue
			}
			if ins[k-1].op == END_FINALLY || ins[k-1].op == RERAISE || ins[k-1].op == RERAISE_A {
				out.tryEndJump[ins[i].pos] = ins[k].pos
				break
			}
			if ins[k-1].op != POP_BLOCK && ins[k-1].op != POP_EXCEPT {
				break
			}
		}
	}
	// 3.11+: the same for `for` loops.
	for i := 1; i+1 < len(ins); i++ {
		var t int
		if ins[i].op != JUMP_BACKWARD_A || jump(&ins[i], &t) != jkUncond || t >= ins[i].pos {
			continue
		}
		if ins[i-1].op != POP_EXCEPT && ins[i+1].op != PUSH_EXC_INFO {
			continue
		}
		hi, ok := idxAt[t]
		if !ok || ins[hi].op != FOR_ITER_A {
			continue
		}
		ft := ins[hi].real + ins[hi].arg*unit
		for k := i + 1; k+2 < len(ins); k++ {
			if ins[k].op == COPY_A && ins[k].arg == 3 && ins[k+1].op == POP_EXCEPT &&
				ins[k+2].op == RERAISE_A && ins[k+2].arg == 1 {
				if k+3 < len(ins) && ins[k+3].pos == ft {
					out.tryEndJump[ins[i].pos] = ft
					out.forTryEnd[ft] = true
				}
				break
			}
		}
	}
	prevReal := func(i int) int {
		i--
		for i >= 0 && ins[i].op == CACHE {
			i--
		}
		return i
	}
	usedBack := map[int]bool{}

	// bottom-tested loops with a conditional back-edge (3.10, 3.11)
	for i := range ins {
		var t int
		k := jump(&ins[i], &t)
		if k != jkCond || t >= ins[i].pos {
			continue
		}
		ti, ok := idxAt[t]
		if !ok {
			continue
		}
		g := prevReal(ti)
		for g >= 0 && ins[g].op == NOP {
			g = prevReal(g)
		}
		var gt int
		if g < 0 || jump(&ins[g], &gt) != jkCond || gt <= ins[g].pos {
			continue
		}
		if gt != ins[i].fall {
			continue
		}
		l := loopFrame{t, ins[i].pos, ins[i].fall, -1, ins[g].pos, true}
		out.guardEnd[ins[g].pos] = l.end
		out.backedges[ins[i].pos] = true
		loops = append(loops, l)
		usedBack[i] = true
	}
	// start of the condition that precedes index g (the entry guard); jumps
	// that target it are threaded `continue`s of a bottom-tested loop
	entryStarts := map[int]bool{}
	entryStartOf := func(g int) int {
		j, first := g-1, -1
		var tg int
		for j >= 0 && (isExprOp(ins[j].op) || jump(&ins[j], &tg) == jkCond) {
			first = j
			j--
		}
		if first >= 0 {
			return ins[first].pos
		}
		return -1
	}
	for _, l := range loops {
		if gi, ok := idxAt[l.firstBack]; ok {
			entryStarts[entryStartOf(gi)] = true
		}
	}
	if mod.VerCompare(3, 12) >= 0 {
		for _, t := range sortedKeys(lastUncond) {
			bi := lastUncond[t]
			p := prevReal(bi)
			var pt, gt int
			g := -1
			if ti, ok := idxAt[t]; ok {
				g = prevReal(ti)
			}
			for g >= 0 && ins[g].op == NOP {
				g = prevReal(g)
			}
			if p >= 0 && jump(&ins[p], &pt) == jkCond && pt == ins[bi].fall &&
				g >= 0 && jump(&ins[g], &gt) == jkCond && gt == ins[bi].fall {
				entryStarts[entryStartOf(g)] = true
			}
		}
	}
	// unconditional back-edges
	for _, t := range sortedKeys(lastUncond) {
		bi := lastUncond[t]
		if usedBack[bi] {
			continue
		}
		p := prevReal(bi)
		var pt, gt int
		g := -1
		if ti, ok := idxAt[t]; ok {
			g = prevReal(ti)
		}
		for g >= 0 && ins[g].op == NOP {
			g = prevReal(g)
		}
		if mod.VerCompare(3, 12) >= 0 && p >= 0 && jump(&ins[p], &pt) == jkCond &&
			pt == ins[bi].fall && g >= 0 && jump(&ins[g], &gt) == jkCond &&
			gt == ins[bi].fall {
			l := loopFrame{t, ins[bi].pos, ins[bi].fall, -1, ins[g].pos, true}
			out.guardEnd[ins[g].pos] = l.end
			out.backedges[ins[p].pos] = true
			out.skipJumps[ins[bi].pos] = true
			loops = append(loops, l)
			continue
		}
		if entryStarts[t] {
			continue
		}
		guard := -1
		clean := true
		var cj []int
		bodyIdx := idxAt[t]
		for ; bodyIdx < bi; bodyIdx++ {
			var tt int
			k := jump(&ins[bodyIdx], &tt)
			if k == jkNone {
				hop := ins[bodyIdx].op
				walrusStore := bodyIdx > 0 &&
					(ins[bodyIdx-1].op == DUP_TOP || ins[bodyIdx-1].op == COPY_A) &&
					(hop == STORE_FAST_A || hop == STORE_NAME_A || hop == STORE_GLOBAL_A || hop == STORE_DEREF_A)
				if !isExprOp(hop) && !walrusStore {
					break
				}
				continue
			}
			if k != jkCond {
				break
			}
			cj = append(cj, bodyIdx)
		}
		for gi := len(cj) - 1; gi >= 0 && guard < 0; gi-- {
			gtgt := 0
			jump(&ins[cj[gi]], &gtgt)
			if gtgt < ins[bi].fall {
				continue
			}
			bodyStart := ins[cj[gi]].fall
			{
				nx := cj[gi] + 1
				for nx < len(ins) && (ins[nx].op == CACHE || ins[nx].op == NOT_TAKEN) {
					nx++
				}
				if nx < len(ins) {
					bodyStart = ins[nx].pos
				}
			}
			ok := true
			for c := 0; ok && c < gi; c++ {
				ct := 0
				jump(&ins[cj[c]], &ct)
				if ct < ins[bi].fall && ct != bodyStart {
					ok = false
				}
			}
			if ok {
				guard = ins[cj[gi]].pos
				cj = cj[:gi+1]
			}
		}
		if guard < 0 {
			clean = false
		}
		l := loopFrame{t, ins[bi].pos, ins[bi].fall, -1, guard, false}
		if guard >= 0 && clean {
			out.guardEnd[guard] = l.end
			if len(cj) > 1 {
				var chain []chainJump
				for c := 0; c+1 < len(cj); c++ {
					ct := 0
					jump(&ins[cj[c]], &ct)
					chain = append(chain, chainJump{ins[cj[c]].pos,
						ins[cj[c]].op == POP_JUMP_IF_TRUE_A || ins[cj[c]].op == POP_JUMP_FORWARD_IF_TRUE_A,
						ct < ins[bi].fall})
					out.guardChainMember[ins[cj[c]].pos] = true
				}
				out.guardChain[guard] = chain
			}
		} else if guard < 0 {
			out.trueOpen[t] = l.end
		} else {
			continue
		}
		out.uncondBack[ins[bi].pos] = true
		loops = append(loops, l)
	}

	// `break` inside a `for` loop: the iterator is popped, then the loop is
	// left by an unconditional forward jump past the loop's exit.
	for f := range ins {
		if ins[f].op != FOR_ITER_A {
			continue
		}
		e := ins[f].real + ins[f].arg*unit
		lastBack := -1
		for i := f + 1; i < len(ins) && ins[i].pos < e; i++ {
			var t int
			if jump(&ins[i], &t) == jkUncond && t == ins[f].pos && t < ins[i].pos {
				lastBack = i
			}
		}
		if lastBack < 0 {
			continue
		}
		exitX := -1
		for i := f + 1; i < len(ins); i++ {
			inHoisted := false
			for _, r := range hoistedRegions {
				if ins[i].pos >= r.a && ins[i].pos < r.b {
					inHoisted = true
				}
			}
			if ins[i].pos >= e && !inHoisted {
				continue
			}
			if ins[i].op != POP_TOP && ins[i].op != POP_ITER {
				continue
			}
			n := i + 1
			for n < len(ins) && ins[n].op == CACHE {
				n++
			}
			var t int
			if n >= len(ins) || jump(&ins[n], &t) != jkUncond || t < e || out.skipJumps[ins[n].pos] {
				continue
			}
			if t <= ins[n].pos && !(inHoisted && ins[n].op == JUMP_BACKWARD_A) {
				continue
			}
			out.forBreakPops[ins[i].pos] = true
			out.forBreaks[ins[n].pos] = true
			{
				jb := i - 1
				for jb >= 0 && ins[jb].op == CACHE {
					jb--
				}
				tb := 0
				if jb >= 0 && jump(&ins[jb], &tb) == jkUncond && tb == ins[f].pos &&
					ins[jb].pos > ins[f].pos {
					cj := jb - 1
					for cj >= 0 && (ins[cj].op == CACHE || ins[cj].op == NOT_TAKEN) {
						cj--
					}
					ct := 0
					if cj >= 0 && jump(&ins[cj], &ct) == jkCond && ct == ins[i].pos &&
						(ins[cj].op == POP_JUMP_IF_TRUE_A || ins[cj].op == POP_JUMP_IF_FALSE_A) {
						out.breakIf[ins[cj].pos] = ins[cj].op == POP_JUMP_IF_TRUE_A
						out.skipJumps[ins[jb].pos] = true
						delete(out.forBreaks, ins[n].pos)
						out.skipJumps[ins[n].pos] = true
					}
				}
			}
			if exitX < 0 || t > exitX {
				exitX = t
			}
		}
		if exitX > e {
			n := -1
			if ni, ok := idxAt[e]; ok {
				n = ni
			}
			if n >= 0 {
				if ins[n].op == END_FOR {
					n++
					for n < len(ins) && ins[n].op == CACHE {
						n++
					}
					if n < len(ins) && (ins[n].op == POP_TOP || ins[n].op == POP_ITER) {
						n++
					}
				}
				if n < len(ins) && ins[n].pos < exitX {
					out.forElse[ins[n].pos] = exitX
				}
			}
		}
	}

	for li := range loops {
		l := &loops[li]
		out.loopEnds[l.end] = true
		csPos := l.T
		gcStart := -1
		if l.typeII {
			isChainJump := func(j int, tgt *int) bool {
				if jump(&ins[j], tgt) != jkCond {
					return false
				}
				return *tgt == l.end || *tgt == l.T ||
					(*tgt > ins[j].pos && *tgt < l.end && *tgt <= ins[idxAt[l.B]].fall)
			}
			walkCond := func(from, stopPos int, chain *[]chainJump) int {
				first := from + 1
				for j := from; j >= 0 && ins[j].pos >= stopPos; j-- {
					var tt int
					if isChainJump(j, &tt) {
						cj := chainJump{ins[j].pos,
							ins[j].op == POP_JUMP_IF_TRUE_A || ins[j].op == POP_JUMP_FORWARD_IF_TRUE_A ||
								ins[j].op == POP_JUMP_BACKWARD_IF_TRUE_A,
							tt == l.T}
						*chain = append([]chainJump{cj}, *chain...)
						first = j
						continue
					}
					if jump(&ins[j], &tt) == jkUncond && tt == l.T && ins[j].pos != l.B {
						out.skipJumps[ins[j].pos] = true
						first = j
						continue
					}
					if jump(&ins[j], &tt) != jkNone || !isExprOp(ins[j].op) {
						break
					}
					first = j
				}
				return first
			}
			bi := idxAt[l.B]
			closing := bi
			var ctgt int
			if jump(&ins[bi], &ctgt) != jkCond {
				closing = prevReal(bi)
			}
			var tail []chainJump
			firstCond := walkCond(closing-1, l.T, &tail)
			for _, cj := range tail {
				out.tailChain[cj.pos] = true
			}
			m := firstCond
			if bi < m {
				m = bi
			}
			csPos = ins[m].pos
			l.csStart = csPos
			if g, ok := idxAt[l.firstBack]; ok {
				var chain []chainJump
				first := walkCond(g-1, 0, &chain)
				if first < g {
					gcStart = ins[first].pos
				}
				if len(chain) != 0 {
					for _, cj := range chain {
						out.guardChainMember[cj.pos] = true
					}
					out.guardChain[ins[g].pos] = chain
				}
			}
		}
		exitX := -1
		var brk, cont []int
		for i := idxAt[l.T]; i < len(ins) && ins[i].pos < l.B; i++ {
			var t int
			k := jump(&ins[i], &t)
			if k != jkUncond || out.skipJumps[ins[i].pos] {
				continue
			}
			innerOwns := false
			for oi := range loops {
				o := &loops[oi]
				if oi != li && o.T > l.T && o.T <= ins[i].pos && ins[i].pos <= o.B && o.B < l.B {
					innerOwns = true
					break
				}
			}
			if innerOwns {
				continue
			}
			if t >= l.end {
				brk = append(brk, ins[i].pos)
				if exitX < 0 {
					exitX = t
				}
			} else if !l.typeII && t == l.T {
				if _, ok := out.tryEndJump[ins[i].pos]; !ok {
					cont = append(cont, ins[i].pos)
				}
			} else if l.typeII && (t == l.csStart || t == gcStart) && ins[i].pos < l.csStart {
				ti := -1
				if x, ok := idxAt[t]; ok {
					ti = x
				}
				tryEnd := i > 0 && ti > 0 &&
					(ins[ti-1].op == RERAISE || ins[ti-1].op == RERAISE_A || ins[ti-1].op == END_FINALLY) &&
					(ins[i-1].op == POP_BLOCK || ins[i-1].op == POP_EXCEPT ||
						(i+1 < len(ins) && ins[i+1].op == PUSH_EXC_INFO))
				if !tryEnd {
					cont = append(cont, ins[i].pos)
				}
			}
		}
		if !l.typeII {
			ei, eok := idxAt[l.end]
			bIdx := idxAt[l.B]
			fwd := -1
			if eok && ins[ei].op == JUMP_FORWARD_A {
				jump(&ins[ei], &fwd)
			}
			if eok && (ins[ei].op == NOP || fwd > l.end) {
				if fwd > l.end {
					out.elseEnd[l.end] = fwd
					out.skipJumps[ins[ei].pos] = true
				}
				for i := idxAt[l.T]; i < bIdx; i++ {
					var t int
					if jump(&ins[i], &t) != jkCond || t != l.end || ins[i].pos == l.firstBack {
						continue
					}
					n := i + 1
					for n < len(ins) && (ins[n].op == CACHE || ins[n].op == NOT_TAKEN) {
						n++
					}
					if n == bIdx {
						out.breakIf[ins[i].pos] = ins[i].op == POP_JUMP_IF_TRUE_A
					}
				}
			}
		}
		for _, b := range brk {
			out.breaks[b] = true
		}
		for _, c := range cont {
			out.continues[c] = true
		}
		if exitX > l.end {
			out.elseEnd[l.end] = exitX
		}
	}
}

// scanComprehensionCleanup handles Python 3.12+ inlined comprehensions, which
// are wrapped in an exception-table entry whose handler only restores the
// saved loop variable and re-raises:
//
//	SWAP n; POP_TOP; SWAP n; STORE_FAST v ...; RERAISE 0
//
// It locates those handlers so they can be dropped from the table (they are
// not a user-level try) and skipped when decompiling.
func scanComprehensionCleanup(code *pyc.Code, mod *pyc.Module, entries *[]pyc.ExceptionTableEntry, skip *[]intPair) {
	opAt, nextAt := decodeMaps(code, mod)
	targets := map[int]bool{}
	for _, e := range *entries {
		p := e.Target
		if o, ok := opAt[p]; !ok || o != SWAP_A {
			continue
		}
		p = nextAt[p]
		if o, ok := opAt[p]; !ok || o != POP_TOP {
			continue
		}
		p = nextAt[p]
		if o, ok := opAt[p]; !ok || o != SWAP_A {
			continue
		}
		p = nextAt[p]
		stores := 0
		for {
			o, ok := opAt[p]
			if !ok || o != STORE_FAST_A {
				break
			}
			stores++
			p = nextAt[p]
		}
		if o, ok := opAt[p]; stores == 0 || !ok || o != RERAISE_A {
			continue
		}
		p = nextAt[p]
		if !targets[e.Target] {
			targets[e.Target] = true
			*skip = append(*skip, intPair{e.Target, p})
		}
	}
	if len(targets) == 0 {
		return
	}
	var kept []pyc.ExceptionTableEntry
	for _, e := range *entries {
		if targets[e.Target] {
			continue
		}
		inSkip := false
		for _, r := range *skip {
			if e.StartOffset >= r.a && e.StartOffset < r.b {
				inSkip = true
			}
		}
		if !inSkip {
			kept = append(kept, e)
		}
	}
	*entries = kept
}

// decodeMaps returns opcode and next-offset tables keyed by instruction offset.
func decodeMaps(code *pyc.Code, mod *pyc.Module) (opAt, nextAt map[int]int) {
	opAt, nextAt = map[int]int{}, map[int]int{}
	for _, in := range decodeAll(code, mod) {
		opAt[in.pos] = in.op
		nextAt[in.pos] = in.next
	}
	return
}

func decodeOperands(code *pyc.Code, mod *pyc.Module) map[int]int {
	m := map[int]int{}
	for _, in := range decodeAll(code, mod) {
		m[in.pos] = in.arg
	}
	return m
}

// scanTryElse handles Python 3.11 try/except/else: the else body is the code
// between the end of the protected range and the JUMP_FORWARD (just before the
// handler) that skips the handlers. Map: protected-range end -> (jump
// position, handler offset).
func scanTryElse(code *pyc.Code, mod *pyc.Module, entries []pyc.ExceptionTableEntry,
	elseRegionByStart map[int][2]int) {
	opAt, nextAt := decodeMaps(code, mod)
	operAt := decodeOperands(code, mod)
	for _, e := range entries {
		handler := e.Target
		jp := handler - 2
		if o, ok := opAt[jp]; !ok || o != JUMP_FORWARD_A {
			continue
		}
		dest := nextAt[jp] + operAt[jp]*2
		if dest <= handler || e.EndOffset >= jp || e.StartOffset >= e.EndOffset {
			continue
		}
		if o, ok := opAt[handler]; !ok || o != PUSH_EXC_INFO {
			continue
		}
		if _, ok := elseRegionByStart[e.EndOffset]; !ok {
			elseRegionByStart[e.EndOffset] = [2]int{jp, handler}
		}
	}
}

// scanHoistedHandlers handles Python 3.12+, which moves the (cold) exception
// handlers of a try/except behind the code that follows it, so the handlers
// are not adjacent to the try body:
//
//	body; continuation...; handlers; cleanup
//
// It maps the end of the protected range -> (handler start, end of the handler
// region including its `COPY 3; POP_EXCEPT; RERAISE 1` cleanup) so the
// decompiler can visit the handlers right after the body.
func scanHoistedHandlers(code *pyc.Code, mod *pyc.Module, entries []pyc.ExceptionTableEntry,
	spliceByEnd map[int][2]int) {
	if mod.VerCompare(3, 12) < 0 {
		return
	}
	opAt, nextAt := decodeMaps(code, mod)
	operAt := decodeOperands(code, mod)
	endByHandler := map[int]int{}
	for _, e := range entries {
		if e.PushLasti || e.StartOffset >= e.EndOffset || e.Target <= e.EndOffset {
			continue
		}
		if e.EndOffset > endByHandler[e.Target] {
			endByHandler[e.Target] = e.EndOffset
		}
	}
	for _, H := range sortedKeys(endByHandler) {
		En := endByHandler[H]
		if o, ok := opAt[H]; !ok || o != PUSH_EXC_INFO {
			continue
		}
		if nx, ok := opAt[nextAt[H]]; ok && nx == WITH_EXCEPT_START {
			continue
		}
		at, ok := opAt[En]
		if !ok || at == JUMP_FORWARD_A {
			continue
		}
		if (at == RETURN_CONST_A || at == RETURN_VALUE) && nextAt[En] == H {
			continue
		}
		normal := false
		for p := En; p < H; p = nextAt[p] {
			if opAt[p] == JUMP_FORWARD_A && nextAt[p]+operAt[p]*2 > H {
				normal = true
			}
			if _, ok := nextAt[p]; !ok {
				break
			}
		}
		if normal {
			continue
		}
		cleanup := -1
		for _, e := range entries {
			if e.PushLasti && e.StartOffset == H {
				cleanup = e.Target
			}
		}
		if cleanup < 0 {
			continue
		}
		spliceByEnd[En] = [2]int{H, cleanup + 6}
	}
}
