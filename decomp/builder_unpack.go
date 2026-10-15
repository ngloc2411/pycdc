package decomp

import (
	"fmt"
	"os"

	"github.com/ngloc2411/pycdc/decomp/ast"
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

func (bld *builder) unpackBegin(count, starIdx int) {
	if bld.unpack > 0 {
		bld.unpackSaved = append(bld.unpackSaved, unpackFrame{bld.unpack, bld.unpackStar})
	}
	bld.unpack = count
	bld.unpackStar = starIdx
	bld.stack.Push(ast.NewTuple(nil))
}

func (bld *builder) unpackAdd(target ast.Node) {
	for {
		tup := bld.stack.Top(1)
		if ast.TypeOf(tup) != ast.NODE_TUPLE {
			fmt.Fprint(os.Stderr, "Something TERRIBLE happened!\n")
			return
		}
		if len(tup.(*ast.Tuple).Values()) == bld.unpackStar {
			target = ast.NewUnary(target, ast.UN_UNPACK)
		}
		tup.(*ast.Tuple).Add(target)
		bld.unpack--
		if bld.unpack > 0 {
			return
		}
		bld.stack.Pop()
		if len(bld.unpackSaved) != 0 {
			inner := tup.(*ast.Tuple)
			inner.SetRequireParens(true)
			target = tup
			last := bld.unpackSaved[len(bld.unpackSaved)-1]
			bld.unpack = last.remaining
			bld.unpackStar = last.starIdx
			bld.unpackSaved = bld.unpackSaved[:len(bld.unpackSaved)-1]
			continue
		}
		seq := bld.stack.Top(1)
		bld.stack.Pop()
		bld.unpackStar = -1
		if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.Inited() == 0 {
			if t, ok := tup.(*ast.Tuple); ok {
				t.SetRequireParens(false)
			}
			bld.curblock.SetIndex(tup)
		} else if ast.TypeOf(seq) == ast.NODE_CHAINSTORE {
			appendToChainStore(seq, tup, bld.stack, bld.curblock)
		} else {
			tup.(*ast.Tuple).SetRequireParens(false)
			bld.curblock.Append(ast.NewStore(seq, tup))
		}
		return
	}
}

func (bld *builder) tryWalrus() bool {
	if bld.stack.Empty() || bld.stack.Top(1) == nil || ast.TypeOf(bld.stack.Top(1)) == ast.NODE_CHAINSTORE || bld.source.AtEof() {
		return false
	}
	sSrc, sPos := bld.source.Pos(), bld.pos
	o, a := pyc.BcNext(bld.source, bld.mod, &bld.pos)
	if o != STORE_FAST_A && o != STORE_NAME_A && o != STORE_GLOBAL_A && o != STORE_DEREF_A {
		bld.source.SetPos(sSrc)
		bld.pos = sPos
		return false
	}
	if o == STORE_NAME_A && bld.code.GetName(a).Value == "__classcell__" {
		bld.source.SetPos(sSrc)
		bld.pos = sPos
		return false
	}
	storeOpc, storeArg := o, a
	aSrc, aPos := bld.source.Pos(), bld.pos
	chain := false
	for n := 0; n < 12 && !bld.source.AtEof(); n++ {
		p2 := bld.pos
		o2, a2 := pyc.BcNext(bld.source, bld.mod, &p2)
		bld.pos = p2
		if o2 == CACHE {
			continue
		}
		if o2 == STORE_FAST_A || o2 == STORE_NAME_A || o2 == STORE_GLOBAL_A ||
			o2 == STORE_DEREF_A || o2 == STORE_ATTR_A || o2 == STORE_SUBSCR ||
			o2 == UNPACK_SEQUENCE_A || o2 == UNPACK_EX_A ||
			o2 == DUP_TOP || (o2 == COPY_A && a2 == 1) {
			chain = true
			break
		}
		if o2 != LOAD_FAST_A && o2 != LOAD_NAME_A && o2 != LOAD_GLOBAL_A &&
			o2 != LOAD_DEREF_A && o2 != LOAD_CONST_A && o2 != LOAD_ATTR_A &&
			o2 != BINARY_SUBSCR {
			break
		}
	}
	bld.source.SetPos(aSrc)
	bld.pos = aPos
	if chain {
		bld.source.SetPos(sSrc)
		bld.pos = sPos
		return false
	}
	var name ast.Node
	if storeOpc == STORE_FAST_A {
		name = ast.NewName(bld.code.GetLocal(storeArg))
	} else if storeOpc == STORE_DEREF_A {
		name = ast.NewName(bld.code.GetCellVar(bld.mod, storeArg))
	} else {
		name = ast.NewName(bld.code.GetName(storeArg))
	}
	value := bld.stack.Top(1)
	bld.stack.Pop()
	bld.stack.Push(ast.NewNamedExpr(name, value))
	return true
}
