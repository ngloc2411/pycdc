package decomp

import (
	"fmt"
	"github.com/ngloc2411/pycdc/decomp/ast"
	"os"

	. "github.com/ngloc2411/pycdc/opcode"
)

// FastStack is the simulated value stack. top(i) is 1-based.
type FastStack struct {
	st  []ast.Node
	ptr int
}

func NewFastStack(size int) *FastStack { return &FastStack{st: make([]ast.Node, size), ptr: -1} }

func (s *FastStack) Copy() *FastStack {
	c := &FastStack{st: make([]ast.Node, len(s.st)), ptr: s.ptr}
	copy(c.st, s.st)
	return c
}

func (s *FastStack) Push(n ast.Node) {
	if len(s.st) == s.ptr+1 {
		s.st = append(s.st, nil)
	}
	s.ptr++
	s.st[s.ptr] = n
}

func (s *FastStack) Pop() {
	if s.ptr > -1 {
		s.st[s.ptr] = nil
		s.ptr--
	}
}

func (s *FastStack) Top(i int) ast.Node {
	if i > 0 {
		idx := s.ptr + 1 - i
		if s.ptr > -1 && idx >= 0 {
			return s.st[idx]
		}
		return nil
	}
	fmt.Fprintf(os.Stderr, "incorrect operand %d\n", i)
	return nil
}

func (s *FastStack) Swap(i int) {
	a := s.ptr
	b := s.ptr + 1 - i
	if a >= 0 && b >= 0 && a != b {
		s.st[a], s.st[b] = s.st[b], s.st[a]
	}
}

func (s *FastStack) Empty() bool { return s.ptr == -1 }
func (s *FastStack) Size() int   { return s.ptr + 1 }

func binFromOpcode(opcode int) int {
	switch opcode {
	case BINARY_ADD:
		return ast.BIN_ADD
	case BINARY_AND:
		return ast.BIN_AND
	case BINARY_DIVIDE, BINARY_TRUE_DIVIDE:
		return ast.BIN_DIVIDE
	case BINARY_FLOOR_DIVIDE:
		return ast.BIN_FLOOR_DIVIDE
	case BINARY_LSHIFT:
		return ast.BIN_LSHIFT
	case BINARY_MODULO:
		return ast.BIN_MODULO
	case BINARY_MULTIPLY:
		return ast.BIN_MULTIPLY
	case BINARY_OR:
		return ast.BIN_OR
	case BINARY_POWER:
		return ast.BIN_POWER
	case BINARY_RSHIFT:
		return ast.BIN_RSHIFT
	case BINARY_SUBTRACT:
		return ast.BIN_SUBTRACT
	case BINARY_XOR:
		return ast.BIN_XOR
	case BINARY_MATRIX_MULTIPLY:
		return ast.BIN_MAT_MULTIPLY
	case INPLACE_ADD:
		return ast.BIN_IP_ADD
	case INPLACE_AND:
		return ast.BIN_IP_AND
	case INPLACE_DIVIDE, INPLACE_TRUE_DIVIDE:
		return ast.BIN_IP_DIVIDE
	case INPLACE_FLOOR_DIVIDE:
		return ast.BIN_IP_FLOOR_DIVIDE
	case INPLACE_LSHIFT:
		return ast.BIN_IP_LSHIFT
	case INPLACE_MODULO:
		return ast.BIN_IP_MODULO
	case INPLACE_MULTIPLY:
		return ast.BIN_IP_MULTIPLY
	case INPLACE_OR:
		return ast.BIN_IP_OR
	case INPLACE_POWER:
		return ast.BIN_IP_POWER
	case INPLACE_RSHIFT:
		return ast.BIN_IP_RSHIFT
	case INPLACE_SUBTRACT:
		return ast.BIN_IP_SUBTRACT
	case INPLACE_XOR:
		return ast.BIN_IP_XOR
	case INPLACE_MATRIX_MULTIPLY:
		return ast.BIN_IP_MAT_MULTIPLY
	}
	return ast.BIN_INVALID
}

var binaryOpTable = []int{
	ast.BIN_ADD, ast.BIN_AND, ast.BIN_FLOOR_DIVIDE, ast.BIN_LSHIFT, ast.BIN_MAT_MULTIPLY, ast.BIN_MULTIPLY, ast.BIN_MODULO,
	ast.BIN_OR, ast.BIN_POWER, ast.BIN_RSHIFT, ast.BIN_SUBTRACT, ast.BIN_DIVIDE, ast.BIN_XOR,
	ast.BIN_IP_ADD, ast.BIN_IP_AND, ast.BIN_IP_FLOOR_DIVIDE, ast.BIN_IP_LSHIFT, ast.BIN_MAT_MULTIPLY, ast.BIN_IP_MULTIPLY,
	ast.BIN_IP_MODULO, ast.BIN_IP_OR, ast.BIN_IP_POWER, ast.BIN_IP_RSHIFT, ast.BIN_IP_SUBTRACT, ast.BIN_IP_DIVIDE, ast.BIN_IP_XOR,
}

func binFromBinaryOp(operand int) int {
	if operand < 0 || operand >= len(binaryOpTable) {
		return ast.BIN_INVALID
	}
	return binaryOpTable[operand]
}
