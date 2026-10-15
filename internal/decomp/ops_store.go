package decomp

import (
	"github.com/ngloc2411/pycdc/internal/decomp/ast"
	. "github.com/ngloc2411/pycdc/internal/opcode"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

func (bld *builder) opsStore() flow {
	switch bld.opcode {
	case SETUP_FINALLY_A:
		{
			if bld.mod.VerCompare(3, 8) >= 0 && bld.mod.VerCompare(3, 11) < 0 {
				off := bld.operand
				if bld.mod.VerCompare(3, 10) >= 0 {
					off = bld.operand * 2
				}
				look := pyc.NewBuffer([]byte(bld.code.Code.Value))
				target := bld.pos + off
				if target < len(bld.code.Code.Value) {
					look.SetPos(target)
					lp := target
					lop, _ := pyc.BcNext(look, bld.mod, &lp)
					if lop == DUP_TOP || lop == POP_TOP {
						bld.pendingOps = append(bld.pendingOps, intPair{SETUP_EXCEPT_A, off})
						break
					}
				}
			}
			next := ast.NewContainerBlock(bld.pos+bld.operand, 0)
			bld.blocks.push(next)
			bld.curblock = bld.blocks.top()

			bld.needTry = true
		}
	case SETUP_LOOP_A:
		{
			next := ast.NewCondBlock(ast.BLK_WHILE, bld.pos+bld.operand, nil, false)
			bld.blocks.push(next)
			bld.curblock = bld.blocks.top()
		}
	case SLICE_0:
		{
			name := bld.stack.Top(1)
			bld.stack.Pop()

			slice := ast.NewSlice(ast.SLICE0, nil, nil)
			bld.stack.Push(ast.NewSubscr(name, slice))
		}
	case SLICE_1:
		{
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()

			slice := ast.NewSlice(ast.SLICE1, lower, nil)
			bld.stack.Push(ast.NewSubscr(name, slice))
		}
	case SLICE_2:
		{
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()

			slice := ast.NewSlice(ast.SLICE2, nil, upper)
			bld.stack.Push(ast.NewSubscr(name, slice))
		}
	case SLICE_3:
		{
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			name := bld.stack.Top(1)
			bld.stack.Pop()

			slice := ast.NewSlice(ast.SLICE3, lower, upper)
			bld.stack.Push(ast.NewSubscr(name, slice))
		}
	case STORE_ATTR_A:
		{
			if bld.unpack != 0 {
				name := bld.stack.Top(1)
				bld.stack.Pop()
				attr := ast.NewBinary(name, ast.NewName(bld.code.GetName(bld.operand)), ast.BIN_ATTR)
				bld.unpackAdd(attr)
			} else {
				name := bld.stack.Top(1)
				bld.stack.Pop()
				value := bld.stack.Top(1)
				bld.stack.Pop()
				attr := ast.NewBinary(name, ast.NewName(bld.code.GetName(bld.operand)), ast.BIN_ATTR)
				if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
					appendToChainStore(value, attr, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(value, attr))
				}
			}
		}
	case STORE_DEREF_A:
		{
			if bld.unpack != 0 {
				name := ast.NewName(bld.code.GetCellVar(bld.mod, bld.operand))
				bld.unpackAdd(name)
			} else {
				value := bld.stack.Top(1)
				bld.stack.Pop()
				derefName := bld.code.GetCellVar(bld.mod, bld.operand)
				name := ast.NewName(derefName)
				if bld.code.IsFreeVar(bld.mod, bld.operand) {
					bld.code.MarkNonlocal(derefName)
				}

				if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
					appendToChainStore(value, name, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(value, name))
				}
			}
		}
	case STORE_FAST_A:
		{
			if bld.unpack != 0 {
				var name ast.Node
				if bld.mod.VerCompare(1, 3) < 0 {
					name = ast.NewName(bld.code.GetName(bld.operand))
				} else {
					name = ast.NewName(bld.code.GetLocal(bld.operand))
				}
				bld.unpackAdd(name)
			} else {
				value := bld.stack.Top(1)
				bld.stack.Pop()
				var name *ast.Name
				if bld.mod.VerCompare(1, 3) < 0 {
					name = ast.NewName(bld.code.GetName(bld.operand))
				} else {
					name = ast.NewName(bld.code.GetLocal(bld.operand))
				}

				nv := name.Name().Value
				if len(nv) >= 2 && nv[0] == '_' && nv[1] == '[' {
					break
				}

				if handleExceptBinding(bld.curblock, value, name) {
					break
				}

				if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.Inited() == 0 {
					bld.curblock.SetIndex(name)
				} else if !bld.stack.Empty() && bld.stack.Top(1) != nil &&
					ast.TypeOf(bld.stack.Top(1)) == ast.NODE_IMPORT {
					imp := bld.stack.Top(1).(*ast.Import)
					imp.AddStore(ast.NewStore(value, name))
				} else if bld.curblock.BlkType() == ast.BLK_WITH && bld.curblock.Inited() == 0 {
					bld.curblock.SetExpr(value)
					bld.curblock.SetVar(name)
				} else if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
					appendToChainStore(value, name, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(value, name))
				}
			}
		}
	case STORE_GLOBAL_A:
		{
			name := ast.NewName(bld.code.GetName(bld.operand))

			if bld.unpack != 0 {
				bld.unpackAdd(name)
			} else {
				value := bld.stack.Top(1)
				bld.stack.Pop()
				if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
					appendToChainStore(value, name, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(value, name))
				}
			}

			bld.code.MarkGlobal(name.Name())
		}
	case STORE_NAME_A:
		{
			if bld.unpack != 0 {
				name := ast.NewName(bld.code.GetName(bld.operand))
				bld.unpackAdd(name)
			} else {
				value := bld.stack.Top(1)
				bld.stack.Pop()

				varname := bld.code.GetName(bld.operand)
				if len(varname.Value) >= 2 && varname.Value[0] == '_' && varname.Value[1] == '[' {
					break
				}

				classPrefix := "_" + bld.code.Name.Value
				if varname.StartsWith(classPrefix + "__") {
					varname.Value = varname.Value[len(classPrefix):]
				}

				name := ast.NewName(varname)

				if handleExceptBinding(bld.curblock, value, name) {
					break
				}

				if bld.curblock.BlkType() == ast.BLK_FOR && bld.curblock.Inited() == 0 {
					bld.curblock.SetIndex(name)
				} else if ast.TypeOf(bld.stack.Top(1)) == ast.NODE_IMPORT {
					imp := bld.stack.Top(1).(*ast.Import)
					imp.AddStore(ast.NewStore(value, name))
				} else if bld.curblock.BlkType() == ast.BLK_WITH && bld.curblock.Inited() == 0 {
					bld.curblock.SetExpr(value)
					bld.curblock.SetVar(name)
				} else if ast.TypeOf(value) == ast.NODE_CHAINSTORE {
					appendToChainStore(value, name, bld.stack, bld.curblock)
				} else {
					bld.curblock.Append(ast.NewStore(value, name))
				}
			}
		}
	case STORE_SLICE_0:
		{
			dest := bld.stack.Top(1)
			bld.stack.Pop()
			value := bld.stack.Top(1)
			bld.stack.Pop()

			bld.curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE0, nil, nil))))
		}
	case STORE_SLICE_1:
		{
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			dest := bld.stack.Top(1)
			bld.stack.Pop()
			value := bld.stack.Top(1)
			bld.stack.Pop()

			bld.curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE1, upper, nil))))
		}
	case STORE_SLICE_2:
		{
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			dest := bld.stack.Top(1)
			bld.stack.Pop()
			value := bld.stack.Top(1)
			bld.stack.Pop()

			bld.curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE2, nil, lower))))
		}
	case STORE_SLICE_3:
		{
			lower := bld.stack.Top(1)
			bld.stack.Pop()
			upper := bld.stack.Top(1)
			bld.stack.Pop()
			dest := bld.stack.Top(1)
			bld.stack.Pop()
			value := bld.stack.Top(1)
			bld.stack.Pop()

			bld.curblock.Append(ast.NewStore(value, ast.NewSubscr(dest, ast.NewSlice(ast.SLICE3, upper, lower))))
		}
	case STORE_SUBSCR:
		{
			if bld.unpack != 0 {
				subscr := bld.stack.Top(1)
				bld.stack.Pop()
				dest := bld.stack.Top(1)
				bld.stack.Pop()

				save := ast.NewSubscr(dest, subscr)
				bld.unpackAdd(save)
			} else {
				subscr := bld.stack.Top(1)
				bld.stack.Pop()
				dest := bld.stack.Top(1)
				bld.stack.Pop()
				src := bld.stack.Top(1)
				bld.stack.Pop()

				foundAnnotatedVar := false
				if bld.variableAnnotations && ast.TypeOf(dest) == ast.NODE_NAME {
					foundAnnotatedVar = dest.(*ast.Name).Name().Value == "__annotations__"
				}

				if foundAnnotatedVar {
					if bld.curblock.Size() != 0 && ast.TypeOf(bld.curblock.Nodes()[bld.curblock.Size()-1]) == ast.NODE_STORE {
						store := bld.curblock.Nodes()[bld.curblock.Size()-1].(*ast.Store)
						bld.curblock.RemoveLast()
						bld.curblock.Append(ast.NewStore(store.Src(), ast.NewAnnotatedVar(subscr, src)))
					} else {
						bld.curblock.Append(ast.NewAnnotatedVar(subscr, src))
					}
				} else {
					if ast.TypeOf(dest) == ast.NODE_MAP {
						dest.(*ast.Map).Add(subscr, src)
					} else if ast.TypeOf(src) == ast.NODE_CHAINSTORE {
						appendToChainStore(src, ast.NewSubscr(dest, subscr), bld.stack, bld.curblock)
					} else {
						bld.curblock.Append(ast.NewStore(src, ast.NewSubscr(dest, subscr)))
					}
				}
			}
		}
	case UNARY_CALL:
		{
			fn := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewCall(fn, nil, nil))
		}
	case UNARY_CONVERT:
		{
			name := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewConvert(name))
		}
	case UNARY_INVERT:
		{
			arg := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewUnary(arg, ast.UN_INVERT))
		}
	case UNARY_NEGATIVE:
		{
			arg := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewUnary(arg, ast.UN_NEGATIVE))
		}
	case UNARY_NOT:
		{
			arg := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewUnary(arg, ast.UN_NOT))
		}
	case UNARY_POSITIVE:
		{
			arg := bld.stack.Top(1)
			bld.stack.Pop()
			bld.stack.Push(ast.NewUnary(arg, ast.UN_POSITIVE))
		}
	default:
		return flowUnhandled
	}
	return flowNext
}
