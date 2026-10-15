package decomp

import (
	"sort"

	"github.com/ngloc2411/pycdc/pyc"
)

// sortedKeys returns the keys of m in ascending order (C++ std::map order).
func sortedKeys[V any](m map[int]V) []int {
	keys := make([]int, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Ints(keys)
	return keys
}

// opIndex is an ordered "offset -> opcode" table of a code object.
type opIndex struct {
	pos []int
	op  map[int]int
}

func newOpIndex() *opIndex { return &opIndex{op: map[int]int{}} }

func (o *opIndex) add(p, opcode int) {
	o.pos = append(o.pos, p)
	o.op[p] = opcode
}

// lowerBound is the index of the first offset >= p.
func (o *opIndex) lowerBound(p int) int { return sort.SearchInts(o.pos, p) }

func (o *opIndex) at(p int) (int, bool) {
	v, ok := o.op[p]
	return v, ok
}

// rawIns is one decoded instruction. real is the offset after any CACHE
// entries that trail the instruction.
type rawIns struct{ pos, op, arg, next, real, fall int }

// decodeAll decodes every instruction of code in order.
func decodeAll(code *pyc.Code, mod *pyc.Module) []rawIns {
	src := pyc.NewBuffer([]byte(code.Code.Value))
	var ins []rawIns
	pos := 0
	for !src.AtEof() {
		p := pos
		opcode, operand := pyc.BcNext(src, mod, &pos)
		ins = append(ins, rawIns{p, opcode, operand, pos, pos, 0})
	}
	return ins
}
