package decomp

import (
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

// Expression-level control flow (ternary `a if c else b`, value-context
// `and`/`or`). The statement-level `if` machinery cannot tell these from
// statements, so the relevant jumps are classified up front:
//
//	ternary:  C; POP_JUMP_IF_FALSE else; A; JUMP_FORWARD end; else: B; end:
//	          -- told from an if/else *statement* by the last instruction of both
//	          branches leaving a value on the stack (not a store/pop/return).
//	and/or:   A; JUMP_IF_FALSE_OR_POP end; B; end:            (<= 3.11)
//	          A; COPY 1; [TO_BOOL;] POP_JUMP_IF_FALSE end; POP_TOP; B; end: (3.12+)
//	          and the mixed form `a and b or c`, where the inner test is a plain
//	          POP_JUMP whose target directly follows a keep-style jump.

// ternInfo: cond is 1 jump-if-false, 2 jump-if-true, 3 if-none, 4 if-not-none.
type ternInfo struct{ elseStart, end, thenJump, cond int }
type keepInfo struct {
	isOr                   bool
	target, endPos, popPos int
}
type boolJumpInfo struct {
	target int
	isOr   bool
}

// condTree op: 0 leaf (jump position), 1 and, 2 or.
type condTree struct{ op, leaf, l, r int }
type condChain struct {
	members []int // jump positions, in order
	trees   []condTree
	root    int
	negate  bool // the combined test is the *negation* of the tree
}

type ExprFlow struct {
	ternary         map[int]ternInfo // test jump position -> shape
	ternaryThenJump map[int]int      // JUMP_FORWARD ending the "then" value -> test jump
	keepSeq         map[int]keepInfo // 3.12+: COPY 1 starting `and`/`or` -> shape
	// value-context chained comparison (`return a < b < c`): the keep jump's
	// false target is a `SWAP 2; POP_TOP` cleanup. chainJoin maps the keep jump
	// (or the COPY 1 of a 3.12 keep sequence) to the real merge point; chainSkip
	// lists the trailer/cleanup ranges the decompiler must ignore.
	chainJoin   map[int]int
	cleanupDest map[int]int // lone POP_TOP cleanup -> the exit it leads to
	chainSkip   []intPair
	boolJump    map[int]boolJumpInfo // plain POP_JUMP of a mixed and/or
	// statement conditions made of several jumps (`if a and b or c:`)
	condChains map[int]condChain // last jump position -> chain
	condMember map[int]bool      // every non-last jump of a chain
}

func NewExprFlow() *ExprFlow {
	return &ExprFlow{
		ternary: map[int]ternInfo{}, ternaryThenJump: map[int]int{}, keepSeq: map[int]keepInfo{},
		chainJoin: map[int]int{}, cleanupDest: map[int]int{}, boolJump: map[int]boolJumpInfo{},
		condChains: map[int]condChain{}, condMember: map[int]bool{},
	}
}

func xIsStatementEnd(op int) bool {
	switch op {
	case STORE_FAST_A, STORE_NAME_A, STORE_GLOBAL_A, STORE_DEREF_A, STORE_ATTR_A, STORE_SUBSCR,
		STORE_FAST_STORE_FAST_A, STORE_FAST_LOAD_FAST_A,
		DELETE_FAST_A, DELETE_NAME_A, DELETE_GLOBAL_A, DELETE_ATTR_A, DELETE_SUBSCR, DELETE_DEREF_A,
		POP_TOP, RETURN_VALUE, RETURN_CONST_A, RAISE_VARARGS_A, RERAISE, RERAISE_A,
		JUMP_FORWARD_A, JUMP_ABSOLUTE_A, JUMP_BACKWARD_A, JUMP_BACKWARD_NO_INTERRUPT_A,
		POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A, POP_JUMP_FORWARD_IF_FALSE_A, POP_JUMP_FORWARD_IF_TRUE_A,
		POP_JUMP_BACKWARD_IF_FALSE_A, POP_JUMP_BACKWARD_IF_TRUE_A,
		POP_JUMP_IF_NONE_A, POP_JUMP_IF_NOT_NONE_A,
		POP_BLOCK, POP_EXCEPT, END_FINALLY, NOP, END_FOR, POP_ITER, IMPORT_STAR,
		LIST_APPEND_A, SET_ADD_A, MAP_ADD_A, PRINT_ITEM, PRINT_NEWLINE:
		return true
	}
	return false
}

const (
	condNone = iota
	condIfFalse
	condIfTrue
	condIfNone
	condIfNotNone
)

func scanExprFlow(code *pyc.Code, mod *pyc.Module, out *ExprFlow, loops *WhileLoops) {
	ins := decodeAll(code, mod)
	for i := len(ins) - 1; i >= 0; i-- {
		if i+1 < len(ins) && ins[i+1].op == CACHE && ins[i].op != CACHE {
			ins[i].real = ins[i+1].real
		}
	}
	idxAt := map[int]int{}
	for i := range ins {
		idxAt[ins[i].pos] = i
	}
	unit := 1
	if mod.VerCompare(3, 10) >= 0 {
		unit = 2
	}
	n := len(ins)

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
	condJumpRaw := func(in *rawIns, target *int) int {
		switch in.op {
		case POP_JUMP_IF_FALSE_A, POP_JUMP_IF_TRUE_A:
			if mod.VerCompare(3, 12) >= 0 {
				*target = in.real + in.arg*2
			} else {
				*target = in.arg * unit
			}
			if in.op == POP_JUMP_IF_FALSE_A {
				return condIfFalse
			}
			return condIfTrue
		case POP_JUMP_FORWARD_IF_FALSE_A, POP_JUMP_FORWARD_IF_TRUE_A:
			*target = in.real + in.arg*2
			if in.op == POP_JUMP_FORWARD_IF_FALSE_A {
				return condIfFalse
			}
			return condIfTrue
		case POP_JUMP_FORWARD_IF_NONE_A, POP_JUMP_IF_NONE_A:
			*target = in.real + in.arg*2
			return condIfNone
		case POP_JUMP_FORWARD_IF_NOT_NONE_A, POP_JUMP_IF_NOT_NONE_A:
			*target = in.real + in.arg*2
			return condIfNotNone
		}
		return condNone
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

	// chained-comparison cleanups were located by scanLoops
	for k, v := range loops.cleanupDest {
		out.cleanupDest[k] = v
	}
	out.chainSkip = append([]intPair(nil), loops.chainSkip...)
	condJump := func(in *rawIns, target *int) int {
		c := condJumpRaw(in, target)
		if c != condNone {
			if d, ok := out.cleanupDest[*target]; ok {
				*target = d
			}
		}
		return c
	}

	// A chained comparison's false path: `SWAP 2; POP_TOP` (3.11+) or
	// `ROT_TWO; POP_TOP` before.
	chainCleanup := func(t int, join *int) bool {
		s, ok := idxAt[t]
		if !ok {
			return false
		}
		var isSwap bool
		if mod.VerCompare(3, 11) >= 0 {
			isSwap = ins[s].op == SWAP_A && ins[s].arg == 2
		} else {
			isSwap = ins[s].op == ROT_TWO
		}
		if !isSwap {
			return false
		}
		a := nextReal(s)
		if a >= n || ins[a].op != POP_TOP {
			return false
		}
		before := prevReal(s)
		if before < 0 {
			return false
		}
		endIdx := nextReal(a)
		skipFrom := t
		if ins[before].op == RETURN_VALUE {
			if endIdx >= n || ins[endIdx].op != RETURN_VALUE {
				return false
			}
			*join = ins[before].pos
			endIdx = nextReal(endIdx)
		} else {
			J := fwdJump(&ins[before])
			if J <= t {
				return false
			}
			*join = J
			skipFrom = ins[before].pos
			endIdx = nextReal(a)
		}
		skipTo := ins[n-1].real
		if endIdx < n {
			skipTo = ins[endIdx].pos
		}
		out.chainSkip = append(out.chainSkip, intPair{skipFrom, skipTo})
		return true
	}

	// keep-style jumps (value-context `and`/`or`)
	keepEnd := map[int]bool{} // instruction index where a keep-jump sequence ends
	for i := 0; i < n; i++ {
		if mod.VerCompare(3, 12) < 0 {
			if ins[i].op == JUMP_IF_TRUE_OR_POP_A || ins[i].op == JUMP_IF_FALSE_OR_POP_A {
				keepEnd[i] = true
				if ins[i].op == JUMP_IF_FALSE_OR_POP_A {
					var jt int
					if mod.VerCompare(3, 11) >= 0 {
						jt = ins[i].real + ins[i].arg*2
					} else {
						jt = ins[i].arg * unit
					}
					var join int
					if chainCleanup(jt, &join) {
						out.chainJoin[ins[i].pos] = join
					}
				}
			}
		} else if ins[i].op == COPY_A && ins[i].arg == 1 {
			j := nextReal(i)
			if j < n && ins[j].op == TO_BOOL {
				j = nextReal(j)
			}
			t := 0
			if j >= n {
				continue
			}
			c := condJump(&ins[j], &t)
			if c != condIfFalse && c != condIfTrue {
				continue
			}
			k := nextReal(j)
			if k >= n || ins[k].op != POP_TOP {
				continue
			}
			chainTarget := t
			{
				var join int
				if c == condIfFalse && chainCleanup(t, &join) {
					chainTarget = join
				} else {
					if ti, ok := idxAt[t]; ok && ins[ti].op == SWAP_A && ins[ti].arg == 2 {
						a := nextReal(ti)
						if a < n && ins[a].op == POP_TOP {
							continue
						}
					}
				}
			}
			out.keepSeq[ins[i].pos] = keepInfo{c == condIfTrue, chainTarget, ins[k].real, ins[k].pos}
			keepEnd[k] = true
		}
	}

	for i := 0; i < n; i++ {
		t := 0
		c := condJump(&ins[i], &t)
		if c == condNone || t <= ins[i].pos {
			continue
		}
		inKeep := false
		for back := 1; back <= 3 && i-back >= 0; back++ {
			if kp, ok := out.keepSeq[ins[i-back].pos]; ok && ins[i].real < kp.endPos+1 {
				inKeep = true
				break
			}
		}
		if inKeep {
			continue
		}
		ti, ok := idxAt[t]
		if !ok {
			continue
		}
		before := prevReal(ti)
		if before < 0 {
			continue
		}
		{
			pj := prevReal(i)
			if pj >= 0 && ins[pj].op == IS_OP_A {
				lc := prevReal(pj)
				if lc >= 0 && ins[lc].op == LOAD_COMMON_CONSTANT_A {
					continue
				}
			}
		}
		if keepEnd[before] {
			out.boolJump[ins[i].pos] = boolJumpInfo{t, c == condIfTrue || c == condIfNotNone}
			continue
		}
		U := fwdJump(&ins[before])
		if U <= t {
			continue
		}
		for q := ti; q < n && ins[q].pos < U; q++ {
			if fwdJump(&ins[q]) == U && q+1 < n {
				outer := false
				for o := 0; o < i && !outer; o++ {
					ot := 0
					if condJump(&ins[o], &ot) != condNone && ot == ins[q+1].pos {
						outer = true
					}
				}
				if outer {
					U = ins[q].pos
					break
				}
			}
		}
		thenLast := prevReal(before)
		if thenLast <= i || xIsStatementEnd(ins[thenLast].op) {
			continue
		}
		ui, uok := idxAt[U]
		if !uok {
			continue
		}
		elseLast := prevReal(ui)
		if elseLast < 0 || elseLast < ti || xIsStatementEnd(ins[elseLast].op) {
			continue
		}
		out.ternary[ins[i].pos] = ternInfo{t, U, ins[before].pos, c}
		out.ternaryThenJump[ins[before].pos] = ins[i].pos
	}

	// ---- statement-level compound conditions -------------------------------
	// A run of conditional jumps with only expression code in between forms one
	// boolean test (`if a and b or c:`). Each jump is a decision node whose
	// true/false edges lead to a later node or to the BODY / END terminals;
	// adjacent nodes that share an exit are folded (a and b: same "false" exit;
	// a or b: same "true" exit) until one node remains.
	{
		claimed := map[int]bool{}
		for k := range out.boolJump {
			claimed[k] = true
		}
		for k := range loops.guardEnd {
			claimed[k] = true
		}
		for k := range loops.guardChain {
			claimed[k] = true
		}
		for k := range loops.guardChainMember {
			claimed[k] = true
		}
		for k := range loops.tailChain {
			claimed[k] = true
		}
		for k := range loops.backedges {
			claimed[k] = true
		}
		for k := range loops.breakIf {
			claimed[k] = true
		}
		for k := range out.keepSeq {
			ci := idxAt[k]
			for d := 1; d <= 3 && ci+d < n; d++ {
				claimed[ins[ci+d].pos] = true
			}
		}
		isPlainCond := func(i int, t *int, jumpOnTrue *bool) bool {
			if claimed[ins[i].pos] {
				return false
			}
			c := condJump(&ins[i], t)
			if c != condIfFalse && c != condIfTrue {
				return false
			}
			*jumpOnTrue = c == condIfTrue
			return *t > ins[i].pos
		}
		fallIdx := func(i int) int {
			k := i + 1
			for {
				for k < n && (ins[k].op == CACHE || ins[k].op == NOT_TAKEN) {
					k++
				}
				moved := false
				for _, r := range out.chainSkip {
					if k < n && ins[k].pos == r.a {
						for k < n && ins[k].pos < r.b {
							k++
						}
						moved = true
						break
					}
				}
				if !moved {
					break
				}
			}
			return k
		}
		fallPos := func(i int) int {
			k := fallIdx(i)
			if k < n {
				return ins[k].pos
			}
			return ins[n-1].next
		}
		used := map[int]bool{}
		for i := 0; i < n; i++ {
			t := 0
			onTrue := false
			if used[i] || !isPlainCond(i, &t, &onTrue) {
				continue
			}
			run := []int{i}
			cur := i
			for {
				k := fallIdx(cur)
				jj := -1
				for ; k < n; k++ {
					var t2 int
					var o2 bool
					if isPlainCond(k, &t2, &o2) {
						jj = k
						break
					}
					if xIsStatementEnd(ins[k].op) || condJump(&ins[k], &t2) != condNone ||
						ins[k].op == FOR_ITER_A || ins[k].op == SETUP_FINALLY_A ||
						ins[k].op == SETUP_WITH_A || ins[k].op == BEFORE_WITH {
						break
					}
				}
				if jj < 0 {
					break
				}
				run = append(run, jj)
				cur = jj
			}
			for _, r := range run {
				used[r] = true
			}
			if len(run) < 2 {
				continue
			}
			for m := len(run); m >= 2; m-- {
				type nd struct {
					id   int
					e    [2]int // e[1]=true edge, e[0]=false edge
					tree int
				}
				const termT, termF = -1, -2
				body := fallPos(run[m-1])
				endT := 0
				lastOn := false
				isPlainCond(run[m-1], &endT, &lastOn)
				if endT <= body {
					continue
				}
				startPos := make([]int, m+2)
				for k := 1; k < m; k++ {
					startPos[k+1] = fallPos(run[k-1])
				}
				ok := true
				var nodes []nd
				var trees []condTree
				for k := 1; k <= m && ok; k++ {
					tk := 0
					on := false
					isPlainCond(run[k-1], &tk, &on)
					dest := func(target int) int {
						if target == body {
							return termT
						}
						if target == endT {
							return termF
						}
						for q := k + 1; q <= m; q++ {
							if startPos[q] == target {
								return q
							}
						}
						ok = false
						return 0
					}
					jumpDest := dest(tk)
					fallDest := termT
					if k < m {
						fallDest = k + 1
					}
					var d nd
					d.id = k
					if on {
						d.e[1], d.e[0] = jumpDest, fallDest
					} else {
						d.e[0], d.e[1] = jumpDest, fallDest
					}
					d.tree = len(trees)
					trees = append(trees, condTree{0, ins[run[k-1]].pos, -1, -1})
					nodes = append(nodes, d)
				}
				if !ok {
					continue
				}
				progress := true
				for len(nodes) > 1 && progress {
					progress = false
					for a := 0; a+1 < len(nodes); a++ {
						A := nodes[a]
						B := nodes[a+1]
						for lbl := 0; lbl < 2 && !progress; lbl++ {
							other := 1 - lbl
							if A.e[other] != B.id || A.e[lbl] == B.id {
								continue
							}
							X := A.e[lbl]
							if B.e[lbl] != X || B.e[other] == B.id {
								continue
							}
							refs := 0
							for _, N := range nodes {
								for e := 0; e < 2; e++ {
									if N.e[e] == B.id {
										refs++
									}
								}
							}
							if refs != 1 {
								continue
							}
							op := 2
							if lbl == 0 {
								op = 1
							}
							trees = append(trees, condTree{op, 0, A.tree, B.tree})
							newTree := len(trees) - 1
							var merged nd
							merged.id = A.id
							merged.e[lbl] = X
							merged.e[other] = B.e[other]
							merged.tree = newTree
							nodes = append(nodes[:a+1], nodes[a+2:]...)
							nodes[a] = merged
							progress = true
						}
						if progress {
							break
						}
					}
				}
				if len(nodes) != 1 {
					continue
				}
				R := nodes[0]
				var negate bool
				if R.e[1] == termT && R.e[0] == termF {
					negate = false
				} else if R.e[1] == termF && R.e[0] == termT {
					negate = true
				} else {
					continue
				}
				var cc condChain
				for k := 0; k < m; k++ {
					cc.members = append(cc.members, ins[run[k]].pos)
				}
				cc.trees = trees
				cc.root = R.tree
				cc.negate = negate
				out.condChains[ins[run[m-1]].pos] = cc
				for k := 0; k+1 < m; k++ {
					out.condMember[ins[run[k]].pos] = true
				}
				break
			}
		}
	}
}
