/* BuildFromCode: opcode cases, part 4/8. Included inside the switch in ASTree.cpp. */
        case Pyc::JUMP_ABSOLUTE_A:
        // bpo-47120: Replaced JUMP_ABSOLUTE by the relative jump JUMP_BACKWARD.
        case Pyc::JUMP_BACKWARD_A:
        case Pyc::JUMP_BACKWARD_NO_INTERRUPT_A:
            {
                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129

                if (!spliceStack.empty() && opcode == Pyc::JUMP_BACKWARD_A
                        && curpos >= spliceStack.back().handler && pos <= spliceStack.back().regionEnd) {
                    /* end of a hoisted handler: it falls back to the code behind
                       the try (or to the loop head that code jumps to) */
                    const SpliceCtx& sc = spliceStack.back();
                    int dest = pos - operand * 2;
                    if (dest == sc.resume || (sc.backDest >= 0 && dest == sc.backDest)) {
                        operand = (sc.regionEnd - pos) / 2;
                        opcode = Pyc::JUMP_FORWARD_A;
                        goto jump_forward_impl;
                    }
                }
                {
                    /* end of a try body / except handler that closes the loop
                       body: a jump over the remaining handlers, not a `continue` */
                    auto tej = loops.tryEndJump.find(curpos);
                    if (tej != loops.tryEndJump.end()
                            && (opcode == Pyc::JUMP_ABSOLUTE_A || opcode == Pyc::JUMP_BACKWARD_A)) {
                        int rel = tej->second - pos;
                        operand = mod->verCompare(3, 10) >= 0 ? rel / 2 : rel;
                        opcode = Pyc::JUMP_FORWARD_A;
                        goto jump_forward_impl;
                    }
                }

                if (mod->verCompare(3, 8) >= 0 && mod->verCompare(3, 11) < 0
                        && offs < pos
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && pos == curblock->end()) {
                    /* `if c: body` at the tail of a loop: this jump is both the
                       last instruction of the if-region and the natural loop
                       back-edge. Close the if-block(s) and let the loop close
                       below. */
                    bool loopOwns = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmp = blocks;
                        while (!tmp.empty()) {
                            ASTBlock::BlkType bt = tmp.top()->blktype();
                            if (bt == ASTBlock::BLK_FOR) {
                                loopOwns = tmp.top()->end() == pos
                                        && !tmp.top().cast<ASTIterBlock>()->isComprehension()
                                        && offs == tmp.top().cast<ASTIterBlock>()->start();
                                break;
                            }
                            if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
                                break;
                            tmp.pop();
                        }
                    }
                    while (loopOwns && (curblock->blktype() == ASTBlock::BLK_IF
                                        || curblock->blktype() == ASTBlock::BLK_ELIF)
                            && blocks.size() > 1) {
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        PycRef<ASTBlock> inner = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(inner.cast<ASTNode>());
                    }
                }

                if (!inlineComps.empty() && offs < pos
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && InsideComprehensionAdd(blocks)) {
                    /* Filter/loop back-edge inside an inlined comprehension:
                       blocks are closed at the matching END_FOR. */
                    break;
                }

                if (offs < pos) {
                    /* Python 3.11 filtered comprehension ("if cond") tail:
                       MAP_ADD/SET_ADD/LIST_APPEND already turned the add into
                       a NODE_COMPREHENSION value sitting on the stack, so this
                       backward jump is the natural end of one filtered
                       iteration, not a real `continue`. Fold the filter's
                       condition into the enclosing comprehension's FOR block
                       (so it still prints as `if cond`), close the filter
                       block(s) plainly, and let the ordinary BLK_FOR handling
                       just below finish closing the loop itself. */
                    if ((curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                            && !stack.empty() && stack.top() != NULL
                            && stack.top().type() == ASTNode::NODE_COMPREHENSION) {
                        bool ancestorIsCompFor = false;
                        {
                            std::stack<PycRef<ASTBlock> > tmp = blocks;
                            while (!tmp.empty()) {
                                ASTBlock::BlkType bt = tmp.top()->blktype();
                                if (bt == ASTBlock::BLK_FOR) {
                                    ancestorIsCompFor =
                                            tmp.top().cast<ASTIterBlock>()->isComprehension();
                                    break;
                                }
                                if (bt != ASTBlock::BLK_IF && bt != ASTBlock::BLK_ELIF)
                                    break;
                                tmp.pop();
                            }
                        }
                        if (ancestorIsCompFor) {
                            while ((curblock->blktype() == ASTBlock::BLK_IF
                                    || curblock->blktype() == ASTBlock::BLK_ELIF)
                                    && blocks.size() > 1) {
                                PycRef<ASTCondBlock> ifb = curblock.cast<ASTCondBlock>();
                                PycRef<ASTNode> c = ifb->cond();
                                if (ifb->negative())
                                    c = new ASTUnary(c, ASTUnary::UN_NOT);
                                blocks.pop();
                                curblock = blocks.top();
                                if (curblock->blktype() == ASTBlock::BLK_FOR) {
                                    PycRef<ASTNode> existing =
                                            curblock.cast<ASTIterBlock>()->condition();
                                    PycRef<ASTNode> combined = (existing == NULL) ? c
                                            : new ASTBinary(existing, c, ASTBinary::BIN_LOG_AND);
                                    curblock.cast<ASTIterBlock>()->setCondition(combined);
                                }
                                if (!stack_hist.empty())
                                    stack_hist.pop();
                            }
                        }
                    }

                    if (curblock->blktype() == ASTBlock::BLK_FOR) {
                        /* The jump operand is relative in 3.10+; the real target
                           is (pos - offs) where pos already points past this
                           instruction. Compare that against the loop start. A
                           for loop has TWO kinds of backward jumps to its start:
                           the implicit loop iteration at the very end of the
                           body (pos == block end), and an explicit `continue`
                           somewhere earlier (pos < block end). Only the former
                           closes the loop; the latter emits a continue. */
                        int target = (mod->verCompare(3, 11) >= 0)
                                     ? (pos - offs)
                                     : offs;   /* absolute up to 3.10 */
                        bool is_jump_to_start =
                                target == curblock.cast<ASTIterBlock>()->start();
                        bool at_loop_end = (curblock->end() != 0)
                                     && (pos == curblock->end());
                        if (!at_loop_end && is_jump_to_start && curblock->end() > pos
                                && !curblock.cast<ASTIterBlock>()->isComprehension()) {
                            /* The compiler may place a duplicated `return None`
                               epilogue (for an enclosing branch that skips the
                               loop) between the loop-back jump and the loop
                               exit. That still makes this jump the natural end
                               of the loop, not a `continue`. */
                            PycBuffer look(code->code()->value(), code->code()->length());
                            look.setPos(pos);
                            int lp = pos, lop = 0, lopnd = 0;
                            bool onlyEpilogue = true, sawReturn = false;
                            while (lp < curblock->end() && !look.atEof()) {
                                bc_next(look, mod, lop, lopnd, lp);
                                if (lop == Pyc::RETURN_VALUE || lop == Pyc::RETURN_CONST_A)
                                    sawReturn = true;
                                else if (lop != Pyc::LOAD_CONST_A && lop != Pyc::NOP
                                         && lop != Pyc::CACHE)
                                    onlyEpilogue = false;
                            }
                            if (onlyEpilogue && sawReturn && lp == curblock->end())
                                at_loop_end = true;
                        }
                        bool should_pop_for_block = curblock.cast<ASTIterBlock>()->isComprehension();
                        // in v3.8, SETUP_LOOP is deprecated and for blocks aren't terminated by POP_BLOCK, so we add them here
                        bool should_add_for_block = mod->majorVer() == 3 && mod->minorVer() >= 8 && is_jump_to_start && at_loop_end && !curblock.cast<ASTIterBlock>()->isComprehension();

                        if (!should_pop_for_block && !should_add_for_block
                                && is_jump_to_start && !at_loop_end
                                && !curblock.cast<ASTIterBlock>()->isComprehension()) {
                            /* explicit continue directly in the for body */
                            curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                            break;
                        }

                        if (should_pop_for_block || should_add_for_block) {
                            PycRef<ASTNode> top = stack.top();

                            if (top.type() == ASTNode::NODE_COMPREHENSION) {
                                PycRef<ASTComprehension> comp = top.cast<ASTComprehension>();

                                comp->addGenerator(curblock.cast<ASTIterBlock>());
                            }

                            PycRef<ASTBlock> tmp = curblock;
                            blocks.pop();
                            curblock = blocks.top();
                            if (should_add_for_block) {
                                curblock->append(tmp.cast<ASTNode>());
                            }
                        }
                    } else if (curblock->blktype() == ASTBlock::BLK_ELSE) {
                        /* A backward jump out of an else block is a `continue`
                           when it targets an enclosing loop and we are still
                           before that loop's end (the natural loop-back that
                           closes an else sits exactly at the loop end). Emit a
                           continue inside the else rather than closing it. */
                        if (mod->verCompare(3, 11) >= 0) {
                            int loopEnd = -1;
                            std::stack<PycRef<ASTBlock> > tmp = blocks;
                            while (!tmp.empty()) {
                                int bt = tmp.top()->blktype();
                                if (bt == ASTBlock::BLK_FOR
                                        || bt == ASTBlock::BLK_WHILE) {
                                    loopEnd = tmp.top()->end();
                                    break;
                                }
                                tmp.pop();
                            }
                            if (loopEnd > 0 && pos < loopEnd) {
                                curblock->append(
                                        new ASTKeyword(ASTKeyword::KW_CONTINUE));
                                break;
                            }
                        }
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }

                        blocks.pop();
                        blocks.top()->append(curblock.cast<ASTNode>());
                        curblock = blocks.top();

                        if (curblock->blktype() == ASTBlock::BLK_CONTAINER
                                && !curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            blocks.pop();
                            blocks.top()->append(curblock.cast<ASTNode>());
                            curblock = blocks.top();
                        }
                    } else {
                        curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
                    }

                    /* We're in a loop, this jumps back to the start */
                    /* I think we'll just ignore this case... */
                    break; // Bad idea? Probably!
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept() && pos < cont->except()) {
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, 0, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                if (!stack_hist.empty()) {
                    stack = stack_hist.top();
                    stack_hist.pop();
                } else {
                    fprintf(stderr, "Warning: Stack history is empty, something wrong might have happened\n");
                }

                /* Upstream fix (PR #415, "Fix pop from empty block stack"): if
                   we're down to the outermost block and there's still more
                   bytecode to parse, popping it here would underflow the block
                   stack (a hard crash). Bail out of this jump's block-closing
                   logic instead of popping past the bottom. */
                if (blocks.size() == 1 && !source.atEof()) {
                    fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                    cleanBuild = false;
                    break;
                }

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                do {
                    if (blocks.size() == 1) {
                        fprintf(stderr, "Warning: Refusing to pop last block when there is more code to parse pos: %d OP: %02x\n", pos, opcode & 0xff);
                        cleanBuild = false;
                        break;
                    }
                    blocks.pop();

                    blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF
                            || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, blocks.top()->end());
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, blocks.top()->end(), NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;
                    } else {
                        prev = nil;
                    }

                } while (prev != nil);

                curblock = blocks.top();
            }
            break;
        jump_forward_impl:
        case Pyc::JUMP_FORWARD_A:
        case Pyc::INSTRUMENTED_JUMP_FORWARD_A:
            {
                /* An if/elif with no else has no trailer jump of its own: its
                   merge point just sits wherever control naturally arrives.
                   When that merge point happens to be this exact instruction's
                   own start offset (curblock->end() == curpos, rather than the
                   usual curblock->end() == pos-after-a-real-trailer-jump), this
                   JUMP_FORWARD isn't the if's trailer -- it belongs to whatever
                   encloses it (e.g. a try body jumping over its own except
                   handler) and merely lands right after the if. Close the
                   already-finished if/elif plainly first, with no synthesized
                   else, so the logic below sees the real enclosing block. */
                while ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && curblock->end() == curpos
                        && blocks.size() > 1) {
                    PycRef<ASTBlock> fin = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(fin.cast<ASTNode>());
                    if (!stack_hist.empty()) {
                        stack_hist.pop();
                    }
                }

                int offs = operand;
                if (mod->verCompare(3, 10) >= 0)
                    offs *= sizeof(uint16_t); // // BPO-27129

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (cont->hasExcept()) {
                        stack_hist.push(stack);

                        curblock->setEnd(pos+offs);
                        PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                        except->init();
                        blocks.push(except);
                        curblock = blocks.top();
                    }
                    break;
                }

                if (!stack_hist.empty()) {
                    if (stack.empty()) // if it's part of if-expression, TOS at the moment is the result of "if" part
                        stack = stack_hist.top();
                    stack_hist.pop();
                }

                PycRef<ASTBlock> prev = curblock;
                PycRef<ASTBlock> nil;
                bool push = true;

                do {
                    /* PR #415 guard: never pop the outermost block. */
                    if (blocks.size() <= 1) {
                        cleanBuild = false;
                        break;
                    }
                    blocks.pop();

                    if (!blocks.empty())
                        blocks.top()->append(prev.cast<ASTNode>());

                    if (prev->blktype() == ASTBlock::BLK_IF
                            || prev->blktype() == ASTBlock::BLK_ELIF) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTBlock(ASTBlock::BLK_ELSE, pos+offs);
                        if (prev->inited() == ASTCondBlock::PRE_POPPED) {
                            next->init(ASTCondBlock::PRE_POPPED);
                        }

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_EXCEPT) {
                        if (offs == 0) {
                            prev = nil;
                            continue;
                        }

                        if (push) {
                            stack_hist.push(stack);
                        }
                        PycRef<ASTBlock> next = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                        next->init();

                        blocks.push(next.cast<ASTBlock>());
                        prev = nil;
                    } else if (prev->blktype() == ASTBlock::BLK_ELSE) {
                        /* Special case */
                        prev = blocks.top();
                        if (!push) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                        push = false;

                        if (prev->blktype() == ASTBlock::BLK_MAIN) {
                            /* Something went out of control! */
                            prev = nil;
                        }
                    } else if (prev->blktype() == ASTBlock::BLK_TRY
                            && prev->end() < pos+offs) {
                        /* Need to add an except/finally block */
                        if (!stack_hist.empty()) {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }

                        if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                            PycRef<ASTContainerBlock> cont = blocks.top().cast<ASTContainerBlock>();
                            if (cont->hasExcept()) {
                                if (push) {
                                    stack_hist.push(stack);
                                }

                                PycRef<ASTBlock> except = new ASTCondBlock(ASTBlock::BLK_EXCEPT, pos+offs, NULL, false);
                                except->init();
                                blocks.push(except);
                            }
                        } else {
                            fprintf(stderr, "Something TERRIBLE happened!!\n");
                        }
                        prev = nil;
                    } else {
                        prev = nil;
                    }

                } while (prev != nil);

                if (!blocks.empty()) {
                    curblock = blocks.top();
                    if (curblock->blktype() == ASTBlock::BLK_EXCEPT)
                        curblock->setEnd(pos+offs);
                }
            }
            break;
        case Pyc::CALL_INTRINSIC_1_A:
            switch (operand) {
            case 2:   /* INTRINSIC_IMPORT_STAR */
                pendingOps.push_back(std::make_pair((int)Pyc::IMPORT_STAR, 0));
                break;
            case 3:   /* INTRINSIC_STOPITERATION_ERROR (generator cleanup) */
            case 4:   /* INTRINSIC_ASYNC_GEN_WRAP */
                break;
            case 5:   /* INTRINSIC_UNARY_POSITIVE */
                {
                    PycRef<ASTNode> arg = stack.top();
                    stack.pop();
                    stack.push(new ASTUnary(arg, ASTUnary::UN_POSITIVE));
                }
                break;
            case 6:   /* INTRINSIC_LIST_TO_TUPLE */
                pendingOps.push_back(std::make_pair((int)Pyc::LIST_TO_TUPLE, 0));
                break;
            default:
                fprintf(stderr, "Unsupported CALL_INTRINSIC_1 operand %d\n", operand);
                break;
            }
            break;
        case Pyc::LIST_TO_TUPLE:
            {
                /* Python 3.9+: convert the list at TOS into a tuple
                   (used when building *args for calls). */
                PycRef<ASTNode> list = stack.top();
                stack.pop();
                if (list.type() == ASTNode::NODE_LIST) {
                    const auto& lv = list.cast<ASTList>()->values();
                    ASTTuple::value_t tv(lv.begin(), lv.end());
                    stack.push(new ASTTuple(tv));
                } else {
                    stack.push(list);
                }
            }
            break;
        case Pyc::RETURN_GENERATOR:
            /* Python 3.11 generator/coroutine prologue; the pushed generator
               object is immediately discarded by a following POP_TOP. */
            stack.push(nullptr);
            break;
        case Pyc::MAP_ADD_A:
            {
                /* Python 3.8+: value at TOS, key at TOS1 (dict comprehensions). */
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                PycRef<ASTNode> key = stack.top();
                stack.pop();

                if (!inlineComps.empty() && curblock->blktype() != ASTBlock::BLK_FOR) {
                    /* Filtered inlined dict comprehension: keep the pair as a
                       yield marker; END_FOR rebuilds the comprehension. */
                    auto m = new ASTMap();
                    m->add(key, value);
                    curblock->append(new ASTReturn(m, ASTReturn::YIELD));
                } else if (InsideComprehensionAdd(blocks)) {
                    /* Dict comprehension (with or without an `if` filter --
                       when filtered, curblock is the filter's BLK_IF/BLK_ELIF
                       rather than the BLK_FOR itself; the backward-jump
                       handling above folds the filter condition into the
                       enclosing FOR block once it sees this comprehension
                       value land back on the stack). */
                    stack.pop();
                    auto m = new ASTMap();
                    m->add(key, value);
                    stack.push(new ASTComprehension(m));
                } else {
                    PycRef<ASTNode> map = stack.top();
                    if (map.type() == ASTNode::NODE_MAP)
                        map.cast<ASTMap>()->add(key, value);
                }
            }
            break;
        case Pyc::SET_ADD_A:
            {
                /* SET_ADD i: set.add(TOS1[-i], TOS). Used in set comprehensions;
                   mirrors LIST_APPEND_A's handling below but builds a set. */
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                PycRef<ASTNode> set = stack.top();

                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    stack.pop();
                    PycRef<ASTComprehension> sc = new ASTComprehension(value);
                    sc->setKind(ASTComprehension::KIND_SET);
                    stack.push(sc.cast<ASTNode>());
                } else {
                    const char* cn = code->name() != NULL ? code->name()->value() : NULL;
                    if ((cn && !strcmp(cn, "<setcomp>")) || !inlineComps.empty()) {
                        curblock->append(new ASTReturn(value, ASTReturn::YIELD));
                    } else {
                        if (set.type() == ASTNode::NODE_SET) {
                            /* {*a, 1}: add to the set literal under construction */
                            ASTSet::value_t vals = set.cast<ASTSet>()->values();
                            vals.push_back(value);
                            stack.pop();
                            stack.push(new ASTSet(vals));
                        } else {
                            stack.push(new ASTSubscr(set, value)); /* Total hack */
                        }
                    }
                }
            }
            break;
        case Pyc::LIST_APPEND:
        case Pyc::LIST_APPEND_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();

                PycRef<ASTNode> list = stack.top();


                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    stack.pop();
                    stack.push(new ASTComprehension(value));
                } else {
                    const char* cn = code->name() != NULL ? code->name()->value() : NULL;
                    if ((cn && !strcmp(cn, "<listcomp>")) || !inlineComps.empty()) {
                        /* Filtered list comprehension: the append is inside the
                           filter block, so record the value as a yield-style
                           marker and let SynthGenexpr rebuild the comprehension
                           (with its filter condition). */
                        curblock->append(new ASTReturn(value, ASTReturn::YIELD));
                    } else {
                        if (list.type() == ASTNode::NODE_LIST) {
                            /* [*a, 1]: append to the list literal under construction */
                            ASTList::value_t vals = list.cast<ASTList>()->values();
                            vals.push_back(value);
                            stack.pop();
                            stack.push(new ASTList(vals));
                        } else {
                            stack.push(new ASTSubscr(list, value)); /* Total hack */
                        }
                    }
                }
            }
            break;
        case Pyc::SET_UPDATE_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTSet> lhs = stack.top().cast<ASTSet>();
                stack.pop();

                ASTSet::value_t result = lhs->values();
                PycRef<PycObject> obj = rhs.type() == ASTNode::NODE_OBJECT
                        ? rhs.cast<ASTObject>()->object() : PycRef<PycObject>();
                if (obj != NULL && obj->type() == PycObject::TYPE_FROZENSET) {
                    for (const auto& it : obj.cast<PycSet>()->values())
                        result.push_back(new ASTObject(it));
                } else if (rhs.type() == ASTNode::NODE_SET) {
                    for (const auto& it : rhs.cast<ASTSet>()->values())
                        result.push_back(it);
                } else {
                    /* {a, *expr}: the elements aren't known at decompile time. */
                    result.push_back(new ASTUnary(rhs, ASTUnary::UN_UNPACK));
                }

                stack.push(new ASTSet(result));
            }
            break;
