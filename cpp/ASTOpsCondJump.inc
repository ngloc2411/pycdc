/* BuildFromCode: opcode cases, part 3/8. Included inside the switch in ASTree.cpp. */
        case Pyc::JUMP_IF_FALSE_A:
        case Pyc::JUMP_IF_TRUE_A:
        case Pyc::JUMP_IF_FALSE_OR_POP_A:
        case Pyc::JUMP_IF_TRUE_OR_POP_A:
        case Pyc::POP_JUMP_IF_FALSE_A:
        case Pyc::POP_JUMP_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_FORWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NONE_A:
        case Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A:
        case Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A:
            {
                /* Python 3.11 while-loop entry guard: open a BLK_WHILE here
                   instead of an if. The matching back-edge (POP_JUMP_BACKWARD)
                   closes it. */
                auto wit = whileGuardEnd.find(curpos);
                if (wit != whileGuardEnd.end()) {
                    PycRef<ASTNode> wcond = stack.top();
                    stack.pop();
                    bool wneg = false;
                    if (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A) {
                        wcond = new ASTCompare(wcond, new ASTObject(Pyc_None),
                                               ASTCompare::CMP_IS_NOT);
                    } else if (opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                        wcond = new ASTCompare(wcond, new ASTObject(Pyc_None),
                                               ASTCompare::CMP_IS);
                    } else if (opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                               || opcode == Pyc::POP_JUMP_IF_TRUE_A) {
                        wneg = true;
                    }
                    auto chainIt = loops.guardChain.find(curpos);
                    if (chainIt != loops.guardChain.end()
                            && chainIt->second.size() == guardChainConds.size()) {
                        /* `while a and b` / `while a or b`: fold the earlier
                           short-circuit jumps into one condition, from the last
                           one backwards. */
                        PycRef<ASTNode> res = wcond;
                        if (wneg)
                            res = new ASTUnary(res, ASTUnary::UN_NOT);
                        wneg = false;
                        for (int ci = (int)guardChainConds.size() - 1; ci >= 0; ci--) {
                            const ChainJump& cj = chainIt->second[ci];
                            PycRef<ASTNode> c = guardChainConds[ci];
                            /* jump-if-true to the exit / jump-if-false to the body
                               means the stored test is the negation */
                            if (cj.ifTrue != cj.toBody)
                                c = new ASTUnary(c, ASTUnary::UN_NOT);
                            res = new ASTBinary(c, res, cj.toBody ? ASTBinary::BIN_LOG_OR
                                                                 : ASTBinary::BIN_LOG_AND);
                        }
                        wcond = res;
                    }
                    guardChainConds.clear();
                    PycRef<ASTCondBlock> wh = new ASTCondBlock(
                            ASTBlock::BLK_WHILE, wit->second, wcond, wneg);
                    wh->init(ASTCondBlock::PRE_POPPED);
                    blocks.push(wh.cast<ASTBlock>());
                    curblock = blocks.top();
                    break;
                }

                PycRef<ASTNode> cond = stack.top();
                PycRef<ASTCondBlock> ifblk;
                int popped = ASTCondBlock::UNINITED;

                if (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                    /* Python 3.11: compare TOS against None, then pop and jump.
                       IF_NONE jumps past the body when TOS is None, so the body
                       runs for "x is not None" (and vice-versa). */
                    stack.pop();
                    ASTCompare::CompareOp cmpop =
                        (opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A)
                        ? ASTCompare::CMP_IS_NOT
                        : ASTCompare::CMP_IS;
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), cmpop);
                    popped = ASTCondBlock::PRE_POPPED;
                }

                if (!matches.empty() && matches.back().cblock == curblock
                        && matches.back().guardOpen && curblock->size() == 0
                        && opcode != Pyc::JUMP_IF_FALSE_A
                        && opcode != Pyc::JUMP_IF_TRUE_A
                        && opcode != Pyc::JUMP_IF_FALSE_OR_POP_A
                        && opcode != Pyc::JUMP_IF_TRUE_OR_POP_A) {
                    /* The guard of a case: `case pattern if cond:` */
                    MatchCtx& mc = matches.back();
                    int gt = operand;
                    if (mod->verCompare(3, 10) >= 0)
                        gt *= sizeof(uint16_t);
                    if (mod->verCompare(3, 12) >= 0
                            || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                            || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A)
                        gt += pos;
                    int ni = matcher->indexOf(gt);
                    bool isGuard;
                    if (mc.guardTarget >= 0)
                        isGuard = (gt == mc.guardTarget);
                    else if (mc.cur.bodyEnd >= 0)
                        isGuard = (gt == mc.cur.nextPos);
                    else
                        isGuard = mc.cur.nonLast || gt == mc.endPos
                                || (ni >= 0 && matcher->instrs()[ni].op == Pyc::NOP)
                                || (!mc.isDefault && mc.cblock->size() == 0 && gt > pos);
                    if (isGuard) {
                        stack.pop();
                        if (opcode == Pyc::POP_JUMP_IF_TRUE_A
                                || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                                || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A)
                            cond = new ASTUnary(cond, ASTUnary::UN_NOT);
                        PycRef<ASTNode> prevGuard = mc.cblock->guard();
                        mc.cblock->setGuard(prevGuard == nullptr ? cond
                                : new ASTBinary(prevGuard, cond, ASTBinary::BIN_LOG_AND));
                        if (mc.guardTarget < 0) {
                            mc.guardTarget = gt;
                            if (mc.cur.bodyEnd < 0) {
                                mc.cur.bodyEnd = gt;
                                mc.cur.nextPos = gt;
                                mc.endJumpPos = gt - 2;
                                mc.cblock->setEnd(gt);
                            }
                        }
                        break;
                    }
                    mc.guardOpen = false;
                }

                if (opcode == Pyc::POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_FALSE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A) {
                    /* Pop condition before the jump */
                    stack.pop();
                    popped = ASTCondBlock::PRE_POPPED;
                }

                /* Store the current stack for the else statement(s) */
                stack_hist.push(stack);

                if (opcode == Pyc::JUMP_IF_FALSE_OR_POP_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A) {
                    /* Pop condition only if condition is met */
                    stack.pop();
                    popped = ASTCondBlock::POPPED;
                }

                /* "Jump if true" means "Jump if not false" */
                bool neg = opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::JUMP_IF_TRUE_OR_POP_A
                        || opcode == Pyc::POP_JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::INSTRUMENTED_POP_JUMP_IF_TRUE_A;

                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129
                if (mod->verCompare(3, 12) >= 0
                        || opcode == Pyc::JUMP_IF_FALSE_A
                        || opcode == Pyc::JUMP_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_TRUE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_FALSE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NONE_A
                        || opcode == Pyc::POP_JUMP_FORWARD_IF_NOT_NONE_A) {
                    /* Offset is relative to the end of the instruction's
                       inline CACHE entries (3.13+ conditional jumps have one) */
                    int base = pos;
                    if (mod->verCompare(3, 13) >= 0) {
                        int sp = source.pos(), pp = pos;
                        while (!source.atEof()) {
                            int o3 = 0, a3 = 0, p3 = pp;
                            bc_next(source, mod, o3, a3, p3);
                            if (o3 != Pyc::CACHE)
                                break;
                            pp = p3;
                        }
                        source.setPos(sp);
                        base = pp;
                    }
                    offs += base;
                }

                /* Chained comparison: if this conditional jump lands on a
                   trampoline (POP_TOP; JUMP_FORWARD exit), resolve it to the
                   real exit so both halves of `a == b == c` share a target and
                   merge with `and` rather than `or`. */
                {
                    auto tit = xflow.cleanupDest.find(offs);
                    if (tit != xflow.cleanupDest.end())
                        offs = tit->second;
                }

                if (cond.type() == ASTNode::NODE_COMPARE
                        && cond.cast<ASTCompare>()->op() == ASTCompare::CMP_EXCEPTION) {
                    int except_end = offs;
                    if (curblock->blktype() == ASTBlock::BLK_EXCEPT
                            && curblock.cast<ASTCondBlock>()->cond() == NULL) {
                        /* Refining the initial (type-less) except handler. The
                           clause ends at the dispatch jump target (the next
                           clause / reraise), not at the whole handler region;
                           using the region end would swallow following clauses
                           when this clause does not fall through (e.g. raises). */
                        if (offs <= pos)
                            except_end = curblock->end();
                        blocks.pop();
                        curblock = blocks.top();

                        stack_hist.pop();
                    }

                    ifblk = new ASTCondBlock(ASTBlock::BLK_EXCEPT, except_end, cond.cast<ASTCompare>()->right(), false);
                } else if (curblock->blktype() == ASTBlock::BLK_ELSE
                           && curblock->size() == 0) {
                    /* Collapse into elif statement */
                    blocks.pop();
                    stack = stack_hist.top();
                    stack_hist.pop();
                    ifblk = new ASTCondBlock(ASTBlock::BLK_ELIF, offs, cond, neg);
                } else if (curblock->size() == 0 && !curblock->inited()
                           && curblock->blktype() == ASTBlock::BLK_WHILE) {
                    /* The condition for a while loop */
                    PycRef<ASTBlock> top = blocks.top();
                    blocks.pop();
                    ifblk = new ASTCondBlock(top->blktype(), offs, cond, neg);

                    /* We don't store the stack for loops! Pop it! */
                    stack_hist.pop();
                } else if (curblock->size() == 0 && curblock->end() <= offs
                           && (curblock->blktype() == ASTBlock::BLK_IF
                           || curblock->blktype() == ASTBlock::BLK_ELIF
                           || curblock->blktype() == ASTBlock::BLK_WHILE)
                           && blocks.size() > 1) {
                    PycRef<ASTNode> newcond;
                    PycRef<ASTCondBlock> top = curblock.cast<ASTCondBlock>();
                    PycRef<ASTNode> cond1 = top->cond();
                    blocks.pop();

                    if (curblock->blktype() == ASTBlock::BLK_WHILE) {
                        stack_hist.pop();
                    } else {
                        FastStack s_top = stack_hist.top();
                        stack_hist.pop();
                        stack_hist.pop();
                        stack_hist.push(s_top);
                    }

                    if (curblock->end() == offs
                            || (curblock->end() == curpos && !top->negative())) {
                        /* if blah and blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_AND);
                    } else {
                        /* if blah or blah */
                        newcond = new ASTBinary(cond1, cond, ASTBinary::BIN_LOG_OR);
                    }
                    ifblk = new ASTCondBlock(top->blktype(), offs, newcond, neg);
                } else if (curblock->blktype() == ASTBlock::BLK_FOR
                            && curblock.cast<ASTIterBlock>()->isComprehension()
                            && mod->verCompare(2, 7) >= 0) {
                    /* Comprehension condition */
                    curblock.cast<ASTIterBlock>()->setCondition(cond);
                    stack_hist.pop();
                    // TODO: Handle older python versions, where condition
                    // is laid out a little differently.
                    break;
                } else {
                    /* Plain old if statement */
                    ifblk = new ASTCondBlock(ASTBlock::BLK_IF, offs, cond, neg);
                }

                if (popped)
                    ifblk->init(popped);

                blocks.push(ifblk.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
        case Pyc::POP_JUMP_BACKWARD_IF_TRUE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_FALSE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NONE_A:
        case Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A:
            {
                /* Python 3.11 while-loop back-edge: the duplicate condition has
                   just been evaluated onto the stack; discard it and close the
                   BLK_WHILE opened at the matching entry guard. Only act when a
                   BLK_WHILE is actually open (guard against a misidentified
                   back-edge collapsing the whole block stack). */
                if (whileBackedges.count(curpos)) {
                    bool hasWhile = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmp = blocks;
                        while (!tmp.empty()) {
                            if (tmp.top()->blktype() == ASTBlock::BLK_WHILE) {
                                hasWhile = true;
                                break;
                            }
                            tmp.pop();
                        }
                    }
                    if (hasWhile) {
                        if (!stack.empty())
                            stack.pop();
                        /* Close any blocks still open inside the loop body, then
                           the BLK_WHILE itself. */
                        while (blocks.size() > 1
                               && curblock->blktype() != ASTBlock::BLK_WHILE) {
                            PycRef<ASTBlock> inner = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            curblock->append(inner.cast<ASTNode>());
                        }
                        if (curblock->blktype() == ASTBlock::BLK_WHILE
                                && blocks.size() > 1) {
                            PycRef<ASTBlock> wh = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            curblock->append(wh.cast<ASTNode>());
                        }
                        break;
                    }
                }

                /* Python 3.11: conditional jump backwards. In compiled loops
                   this implements an in-body guard ("if cond: <rest of body>")
                   where a failed test jumps back to the loop header to start the
                   next iteration. Model it as an if-block that spans to the end
                   of the enclosing loop body. */
                PycRef<ASTNode> cond = stack.top();
                stack.pop();

                bool neg = false;
                if (opcode == Pyc::POP_JUMP_BACKWARD_IF_NONE_A) {
                    /* jumps back when "x is None" -> body runs for "x is not None" */
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), ASTCompare::CMP_IS_NOT);
                } else if (opcode == Pyc::POP_JUMP_BACKWARD_IF_NOT_NONE_A) {
                    cond = new ASTCompare(cond, new ASTObject(Pyc_None), ASTCompare::CMP_IS);
                } else if (opcode == Pyc::POP_JUMP_BACKWARD_IF_TRUE_A) {
                    /* jumps back when cond is true -> body runs for "not cond" */
                    neg = true;
                }

                int end = curblock->end();

                stack_hist.push(stack);
                PycRef<ASTCondBlock> ifblk =
                        new ASTCondBlock(ASTBlock::BLK_IF, end, cond, neg);
                ifblk->init(ASTCondBlock::PRE_POPPED);
                blocks.push(ifblk.cast<ASTBlock>());
                curblock = blocks.top();
            }
            break;
