package decomp

import (
	. "github.com/ngloc2411/pycdc/opcode"
	"github.com/ngloc2411/pycdc/pyc"
)

// Bytecode pre-passes ("scanners"). Each walks a code object once and records
// positions/shapes that the AST builder consults while it interprets the
// instruction stream. They never build AST nodes themselves.

type jumpRec struct{ pos, target int }

// scanWithBlocks is the Python 3.11 with-statement pre-pass. For each
// BEFORE_WITH whose normal exit has the canonical shape (body -> implicit
// __exit__ -> JUMP over the cleanup handler -> resume), it records the with
// body end and the resume offset.
func scanWithBlocks(code *pyc.Code, mod *pyc.Module, entries []pyc.ExceptionTableEntry,
	bodyEndByBefore, resumeByBodyEnd map[int]int) {
	src := pyc.NewBuffer([]byte(code.Code.Value))
	pos := 0
	var befores []int
	var fwdJumps []jumpRec
	opcodeAt := newOpIndex()
	for !src.AtEof() {
		p := pos
		opcode, operand := pyc.BcNext(src, mod, &pos)
		opcodeAt.add(p, opcode)
		if opcode == BEFORE_WITH || opcode == BEFORE_ASYNC_WITH {
			befores = append(befores, p)
		} else if opcode == JUMP_FORWARD_A {
			fwdJumps = append(fwdJumps, jumpRec{p, pos + operand*2})
		}
	}
	for _, bp := range befores {
		bodyEnd, handler := -1, -1
		for _, e := range entries {
			if e.StartOffset > bp {
				bodyEnd = e.EndOffset
				handler = e.Target
				break
			}
		}
		if bodyEnd < 0 || handler < 0 {
			continue
		}
		if o, ok := opcodeAt.at(handler); !ok || o != PUSH_EXC_INFO {
			continue
		}
		hasWithExcept := false
		for _, p := range opcodeAt.pos {
			if p > handler && p <= handler+4 && opcodeAt.op[p] == WITH_EXCEPT_START {
				hasWithExcept = true
				break
			}
		}
		if !hasWithExcept {
			continue
		}
		resume := -1
		for _, jp := range fwdJumps {
			if jp.pos >= bodyEnd && jp.pos < handler && jp.target >= handler {
				resume = jp.target
				break
			}
		}
		if resume < 0 {
			continue
		}
		bodyEndByBefore[bp] = bodyEnd
		resumeByBodyEnd[bodyEnd] = resume
	}
}

// scanTryFinally is the Python 3.11 try/finally pre-pass. A finally compiles
// to: try body -> finally body (normal copy) -> JUMP over an exception handler
// that duplicates the finally body and re-raises.
func scanTryFinally(code *pyc.Code, mod *pyc.Module, entries []pyc.ExceptionTableEntry,
	tryEndByStart, finallyEndByStart, resumeByFinallyEnd map[int]int,
	returnStyleStarts, suppressReturns map[int]bool) {
	src := pyc.NewBuffer([]byte(code.Code.Value))
	pos := 0
	opcodeAt := newOpIndex()
	var fwdJumps []jumpRec
	for !src.AtEof() {
		p := pos
		opcode, operand := pyc.BcNext(src, mod, &pos)
		opcodeAt.add(p, opcode)
		if opcode == JUMP_FORWARD_A {
			fwdJumps = append(fwdJumps, jumpRec{p, pos + operand*2})
		}
	}
	for _, e := range entries {
		if e.PushLasti {
			continue
		}
		T := e.Target
		if o, ok := opcodeAt.at(T); !ok || o != PUSH_EXC_INFO {
			continue
		}
		handlerEnd := -1
		for _, e2 := range entries {
			if e2.StartOffset == T {
				handlerEnd = e2.EndOffset
				break
			}
		}
		if handlerEnd < 0 {
			continue
		}
		if o, ok := opcodeAt.at(T + 2); ok && o == POP_TOP {
			continue
		}
		hasCheck := false
		for _, p := range opcodeAt.pos {
			if p >= T && p < handlerEnd && opcodeAt.op[p] == CHECK_EXC_MATCH {
				hasCheck = true
				break
			}
		}
		if hasCheck {
			continue
		}
		jumpPos, resume := -1, -1
		for _, jp := range fwdJumps {
			if jp.pos >= e.EndOffset && jp.pos < T && jp.target >= T {
				jumpPos = jp.pos
				resume = jp.target
				break
			}
		}
		if jumpPos < 0 {
			lb := opcodeAt.lowerBound(T)
			if lb == 0 {
				continue
			}
			prevOp := opcodeAt.op[opcodeAt.pos[lb-1]]
			if prevOp != RETURN_VALUE && prevOp != RERAISE && prevOp != RERAISE_A {
				continue
			}
			anyRet, anyJumpOut := false, false
			for i := opcodeAt.lowerBound(e.EndOffset); i < len(opcodeAt.pos) && opcodeAt.pos[i] < T; i++ {
				if opcodeAt.op[opcodeAt.pos[i]] == RETURN_VALUE {
					anyRet = true
				}
			}
			for _, jp := range fwdJumps {
				if jp.pos >= e.EndOffset && jp.pos < T && jp.target >= T {
					anyJumpOut = true
				}
			}
			if !anyRet || anyJumpOut {
				continue
			}
			maxEnd := T + 2
			for grew := true; grew; {
				grew = false
				for _, e2 := range entries {
					if e2.StartOffset >= T && e2.StartOffset < maxEnd {
						m := e2.EndOffset
						if e2.Target+2 > m {
							m = e2.Target + 2
						}
						if m > maxEnd {
							maxEnd = m
							grew = true
						}
					}
				}
			}
			res := -1
			for i := opcodeAt.lowerBound(maxEnd - 2); i < len(opcodeAt.pos); i++ {
				o := opcodeAt.op[opcodeAt.pos[i]]
				if o == RERAISE || o == RERAISE_A {
					if i+1 >= len(opcodeAt.pos) {
						res = len(code.Code.Value)
					} else {
						res = opcodeAt.pos[i+1]
					}
					break
				}
			}
			if res < 0 {
				continue
			}
			for i := opcodeAt.lowerBound(e.EndOffset); i < len(opcodeAt.pos) && opcodeAt.pos[i] < T; i++ {
				if opcodeAt.op[opcodeAt.pos[i]] == RETURN_VALUE {
					suppressReturns[opcodeAt.pos[i]] = true
				}
			}
			tryEndByStart[e.StartOffset] = e.EndOffset
			finallyEndByStart[e.StartOffset] = T
			resumeByFinallyEnd[T] = res
			returnStyleStarts[e.StartOffset] = true
			continue
		}
		tryEndByStart[e.StartOffset] = e.EndOffset
		finallyEndByStart[e.StartOffset] = jumpPos
		resumeByFinallyEnd[jumpPos] = resume
	}
}
