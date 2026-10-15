package decomp

import (
	"github.com/ngloc2411/pycdc/decomp/ast"
	"github.com/ngloc2411/pycdc/pyc"
)

type blockStack []*ast.Block

func (b *blockStack) push(x *ast.Block) { *b = append(*b, x) }
func (b *blockStack) pop()              { *b = (*b)[:len(*b)-1] }
func (b blockStack) top() *ast.Block    { return b[len(b)-1] }
func (b blockStack) empty() bool        { return len(b) == 0 }
func (b blockStack) size() int          { return len(b) }

type intPair struct{ a, b int }

func stackPopTop(stack *FastStack) ast.Node {
	n := stack.Top(1)
	stack.Pop()
	return n
}

// checkIfExpr guesses whether the just finished else block is a conditional
// expression and, if so, turns the blocks into a ternary on the stack.
func checkIfExpr(stack *FastStack, curblock *ast.Block) {
	if stack.Empty() {
		return
	}
	nodes := curblock.Nodes()
	if len(nodes) < 2 {
		return
	}
	last := nodes[len(nodes)-1]
	if last.Type() != ast.NODE_BLOCK || last.(*ast.Block).BlkType() != ast.BLK_ELSE {
		return
	}
	prev := nodes[len(nodes)-2]
	if prev.Type() != ast.NODE_BLOCK || prev.(*ast.Block).BlkType() != ast.BLK_IF {
		return
	}
	elseExpr := stackPopTop(stack)
	curblock.RemoveLast()
	nodes = curblock.Nodes()
	ifBlock := nodes[len(nodes)-1]
	ifExpr := stackPopTop(stack)
	curblock.RemoveLast()
	stack.Push(ast.NewTernary(ifBlock, ifExpr, elseExpr))
}

const excSentinel = "<exception value>"

func isExcSentinel(n ast.Node) bool {
	return n != nil && n.Type() == ast.NODE_NAME && n.(*ast.Name).Name().Value == excSentinel
}

func sameName(a, b ast.Node) bool {
	return a != nil && b != nil && a.Type() == ast.NODE_NAME && b.Type() == ast.NODE_NAME &&
		a.(*ast.Name).Name().Value == b.(*ast.Name).Name().Value
}

func handleExceptBinding(curblock *ast.Block, value, name ast.Node) bool {
	if curblock.BlkType() != ast.BLK_EXCEPT {
		return false
	}
	exc := curblock
	if isExcSentinel(value) {
		exc.SetExceptVar(name)
		return true
	}
	valueIsNone := value == nil || (value.Type() == ast.NODE_OBJECT && value.(*ast.Object).Object() == pyc.None)
	if valueIsNone && exc.ExceptVar() != nil && sameName(name, exc.ExceptVar()) {
		return true
	}
	return false
}

func isExceptVarDelete(curblock *ast.Block, name ast.Node) bool {
	if curblock.BlkType() != ast.BLK_EXCEPT {
		return false
	}
	return curblock.ExceptVar() != nil && sameName(name, curblock.ExceptVar())
}

func findComprehension(node ast.Node) *ast.Comprehension {
	if node == nil {
		return nil
	}
	switch node.Type() {
	case ast.NODE_COMPREHENSION:
		return node.(*ast.Comprehension)
	case ast.NODE_RETURN:
		return findComprehension(node.(*ast.Return).Value())
	case ast.NODE_STORE:
		return findComprehension(node.(*ast.Store).Src())
	case ast.NODE_NODELIST:
		for _, n := range node.(*ast.NodeList).Nodes() {
			if c := findComprehension(n); c != nil {
				return c
			}
		}
	}
	return nil
}

func insideComprehensionAdd(blocks blockStack) bool {
	for i := len(blocks) - 1; i >= 0; i-- {
		bt := blocks[i].BlkType()
		if bt == ast.BLK_FOR {
			return blocks[i].IsComprehension()
		}
		if bt != ast.BLK_IF && bt != ast.BLK_ELIF {
			return false
		}
	}
	return false
}

func findCompYield(nodes []ast.Node, condSoFar ast.Node, outCond *ast.Node) ast.Node {
	for _, n := range nodes {
		if n.Type() == ast.NODE_RETURN && n.(*ast.Return).RetType() == ast.RET_YIELD {
			*outCond = condSoFar
			return n.(*ast.Return).Value()
		}
		if n.Type() == ast.NODE_BLOCK && n.(*ast.Block).BlkType() == ast.BLK_IF {
			ifblk := n.(*ast.Block)
			c := ifblk.Cond()
			if ifblk.Negative() {
				c = ast.NewUnary(c, ast.UN_NOT)
			}
			var combined ast.Node
			if condSoFar == nil {
				combined = c
			} else {
				combined = ast.NewBinary(condSoFar, c, ast.BIN_LOG_AND)
			}
			if r := findCompYield(ifblk.Nodes(), combined, outCond); r != nil {
				return r
			}
		}
	}
	return nil
}

func synthGenexpr(node ast.Node) *ast.Comprehension {
	if node == nil {
		return nil
	}
	if node.Type() == ast.NODE_NODELIST {
		for _, n := range node.(*ast.NodeList).Nodes() {
			if c := synthGenexpr(n); c != nil {
				return c
			}
		}
		return nil
	}
	if node.Type() == ast.NODE_BLOCK && node.(*ast.Block).BlkType() == ast.BLK_FOR {
		forblk := node.(*ast.Block)
		var cond ast.Node
		result := findCompYield(forblk.Nodes(), nil, &cond)
		if result == nil {
			return nil
		}
		if cond != nil {
			forblk.SetCondition(cond)
		}
		comp := ast.NewComprehension(result)
		comp.AddGenerator(forblk)
		return comp
	}
	return nil
}

func mkBoolOp(left, right ast.Node, op int) ast.Node {
	if right != nil && right.Type() == ast.NODE_BINARY && right.(*ast.Binary).Op() == op {
		return mkBoolOp(mkBoolOp(left, right.(*ast.Binary).Left(), op), right.(*ast.Binary).Right(), op)
	}
	return ast.NewBinary(left, right, op)
}

func pushFront(l []ast.Node, n ast.Node) []ast.Node { return append([]ast.Node{n}, l...) }
func pushFrontKV(l []ast.KV, kv ast.KV) []ast.KV    { return append([]ast.KV{kv}, l...) }

func appendToChainStore(chainStore ast.Node, item ast.Node, stack *FastStack, curblock *ast.Block) {
	stack.Pop()
	chainStore.(*ast.ChainStore).Append(item)
	if stack.Top(1) == nil {
		curblock.Append(chainStore)
	} else {
		stack.Push(chainStore)
	}
}
