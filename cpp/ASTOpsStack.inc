/* BuildFromCode: opcode cases, part 6/8. Included inside the switch in ASTree.cpp. */
        case Pyc::POP_TOP:
            {
                if (!matches.empty() && matches.back().cblock == curblock
                        && matches.back().skipPop) {
                    /* Discards the subject of the match statement */
                    matches.back().skipPop = false;
                    matches.back().guardOpen = false;
                    break;
                }

                PycRef<ASTNode> value = stack.top();
                stack.pop();

                if (mod->verCompare(3, 12) >= 0 && value != NULL) {
                    /* `break`/`return` inside a for loop pops the iterator,
                       which 3.12+ keeps on the stack below the loop body. */
                    std::stack<PycRef<ASTBlock> > tmpb = blocks;
                    while (!tmpb.empty() && tmpb.top()->blktype() != ASTBlock::BLK_FOR)
                        tmpb.pop();
                    if (!tmpb.empty()) {
                        PycRef<ASTIterBlock> fb = tmpb.top().cast<ASTIterBlock>();
                        if (!fb->isComprehension() && fb->iter() == value)
                            break;
                    }
                }

                /* The live exception from PUSH_EXC_INFO is discarded here when
                   the handler has no `as` binding; never emit the sentinel. */
                if (isExcSentinel(value)) {
                    if (!curblock->inited())
                        curblock->init();
                    break;
                }

                if (!curblock->inited()) {
                    if (curblock->blktype() == ASTBlock::BLK_WITH) {
                        curblock.cast<ASTWithBlock>()->setExpr(value);
                    } else {
                        curblock->init();
                    }
                    break;
                } else if (value == nullptr || value->processed()) {
                    break;
                }

                curblock->append(value);

                if (curblock->blktype() == ASTBlock::BLK_FOR
                        && curblock.cast<ASTIterBlock>()->isComprehension()) {
                    /* This relies on some really uncertain logic...
                     * If it's a comprehension, the only POP_TOP should be
                     * a call to append the iter to the list.
                     */
                    if (value.type() == ASTNode::NODE_CALL) {
                        auto& pparams = value.cast<ASTCall>()->pparams();
                        if (!pparams.empty()) {
                            PycRef<ASTNode> res = pparams.front();
                            stack.push(new ASTComprehension(res));
                        }
                    }
                }
            }
            break;
        case Pyc::PRINT_ITEM:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top()));
                stack.pop();
            }
            break;
        case Pyc::PRINT_ITEM_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->add(stack.top());
                else
                    curblock->append(new ASTPrint(stack.top(), stream));
                stack.pop();
                if (stream)
                    stream->setProcessed();
            }
            break;
        case Pyc::PRINT_NEWLINE:
            {
                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == nullptr && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr));
                stack.pop();
            }
            break;
        case Pyc::PRINT_NEWLINE_TO:
            {
                PycRef<ASTNode> stream = stack.top();
                stack.pop();

                PycRef<ASTPrint> printNode;
                if (curblock->size() > 0 && curblock->nodes().back().type() == ASTNode::NODE_PRINT)
                    printNode = curblock->nodes().back().try_cast<ASTPrint>();
                if (printNode && printNode->stream() == stream && !printNode->eol())
                    printNode->setEol(true);
                else
                    curblock->append(new ASTPrint(nullptr, stream));
                stack.pop();
                if (stream)
                    stream->setProcessed();
            }
            break;
        case Pyc::RAISE_VARARGS_A:
            {
                ASTRaise::param_t paramList;
                for (int i = 0; i < operand; i++) {
                    paramList.push_front(stack.top());
                    stack.pop();
                }
                curblock->append(new ASTRaise(paramList));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && blocks.size() > 1
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
            }
            break;
        case Pyc::RERAISE:
        case Pyc::RERAISE_A:
            /* Python 3.11 cleanup opcode. */
            break;
        case Pyc::RETURN_VALUE:
        case Pyc::INSTRUMENTED_RETURN_VALUE_A:
            if (finallySuppressReturns.count(curpos)) {
                /* the try's own `return X`, already emitted in the try body */
                break;
            }
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value));

                if ((curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELSE)
                        && stack_hist.size()
                        && blocks.size() > 1
                        && (mod->verCompare(2, 6) >= 0)) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    PycRef<ASTBlock> prev = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());

                    /* A return inside an if/else is, in pre-3.11 bytecode,
                       followed by a redundant JUMP that skips the else branch;
                       that jump must be consumed. In 3.11+ the return usually
                       falls straight into real code (a sibling branch, the next
                       statement such as another `return`, or an exception
                       handler), and consuming it would silently drop that code.
                       Peek the next instruction and only skip it when it is an
                       actual unconditional jump; otherwise keep it. */
                    if (!source.atEof()) {
                        int savedSrc = source.pos();
                        int savedPos = pos;
                        int pkOp = 0, pkOperand = 0;
                        bc_next(source, mod, pkOp, pkOperand, pos);
                        /* a backward jump here is the loop's own back-edge
                           (`for x in y: if c: return x`), not an else-skip */
                        int jt = pkOperand * (mod->verCompare(3, 10) >= 0 ? 2 : 1);
                        bool keepNext = !(pkOp == Pyc::JUMP_FORWARD_A
                                       || (pkOp == Pyc::JUMP_ABSOLUTE_A && jt > savedPos));
                        if (keepNext) {
                            source.setPos(savedSrc);
                            pos = savedPos;
                        } else {
                            opcode = pkOp;
                            operand = pkOperand;
                        }
                    }
                }
            }
            break;
        case Pyc::RETURN_CONST_A:
        case Pyc::INSTRUMENTED_RETURN_CONST_A:
            {
                PycRef<ASTObject> value = new ASTObject(code->getConst(operand));
                curblock->append(new ASTReturn(value.cast<ASTNode>()));
            }
            break;
        case Pyc::ROT_TWO:
            {
                {
                    /* `return x` inside a for loop (before 3.12): ROT_TWO;
                       POP_TOP discards the iterator that is not on our stack. */
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0, o3 = 0, a3 = 0;
                    if (!source.atEof()) {
                        bc_next(source, mod, o2, a2, pos);
                        if (!source.atEof())
                            bc_next(source, mod, o3, a3, pos);
                    }
                    bool inFor = false;
                    {
                        std::stack<PycRef<ASTBlock> > tmpb = blocks;
                        while (!tmpb.empty() && tmpb.top()->blktype() != ASTBlock::BLK_FOR)
                            tmpb.pop();
                        inFor = !tmpb.empty()
                                && !tmpb.top().cast<ASTIterBlock>()->isComprehension();
                    }
                    if (inFor && o2 == Pyc::POP_TOP && o3 == Pyc::RETURN_VALUE) {
                        /* keep POP_TOP consumed, leave RETURN_VALUE for the next step */
                        source.setPos(sSrc);
                        pos = sPos;
                        bc_next(source, mod, o2, a2, pos);
                        break;
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                }
                {
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0;
                    bool storeNext = false;
                    if (!source.atEof()) {
                        bc_next(source, mod, o2, a2, pos);
                        storeNext = (o2 == Pyc::STORE_FAST_A || o2 == Pyc::STORE_NAME_A
                                  || o2 == Pyc::STORE_GLOBAL_A || o2 == Pyc::STORE_DEREF_A
                                  || o2 == Pyc::UNPACK_SEQUENCE_A || o2 == Pyc::UNPACK_EX_A);
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                    if (storeNext && stack.size() >= 2 && stack.top(1) != NULL
                            && stack.top(1).type() != ASTNode::NODE_CHAINSTORE
                            && stack.top() != NULL
                            && stack.top().type() != ASTNode::NODE_CHAINSTORE) {
                        /* `a, b = b, a`: parallel assignment */
                        pendingOps.push_back(std::make_pair((int)Pyc::SWAP_A, 2));
                        break;
                    }
                }
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> two = stack.top();
                stack.pop();

                stack.push(one);
                stack.push(two);
            }
            break;
        case Pyc::ROT_THREE:
            {
                /* `a, b, c = x, y, z` (<= 3.10): ROT_THREE; ROT_TWO reverses the
                   three values before the stores. Model it as the unpack of a
                   3-tuple, like the 3.11+ SWAP form. */
                if (!unpack && stack.size() >= 3 && !source.atEof()) {
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0, o3 = 0, a3 = 0;
                    bc_next(source, mod, o2, a2, pos);
                    if (o2 == Pyc::ROT_TWO && !source.atEof())
                        bc_next(source, mod, o3, a3, pos);
                    bool parallel = (o2 == Pyc::ROT_TWO)
                            && (o3 == Pyc::STORE_FAST_A || o3 == Pyc::STORE_NAME_A
                                || o3 == Pyc::STORE_GLOBAL_A || o3 == Pyc::STORE_DEREF_A
                                || o3 == Pyc::STORE_ATTR_A || o3 == Pyc::STORE_SUBSCR);
                    for (int k = 1; parallel && k <= 3; k++)
                        parallel = stack.top(k) != NULL
                                   && stack.top(k).type() != ASTNode::NODE_CHAINSTORE;
                    if (parallel) {
                        source.setPos(sSrc);
                        pos = sPos;
                        bc_next(source, mod, o2, a2, pos);   /* consume the ROT_TWO */
                        unpack = 3;
                        ASTTuple::value_t values(3);
                        for (int i = 0; i < 3; i++) {
                            values[2 - i] = stack.top();
                            stack.pop();
                        }
                        auto tup = new ASTTuple(values);
                        tup->setRequireParens(false);
                        ASTTuple::value_t targets;
                        auto next_tup = new ASTTuple(targets);
                        next_tup->setRequireParens(false);
                        stack.push(tup);
                        stack.push(next_tup);
                        break;
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                }
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::ROT_FOUR:
            {
                PycRef<ASTNode> one = stack.top();
                stack.pop();
                PycRef<ASTNode> two = stack.top();
                stack.pop();
                PycRef<ASTNode> three = stack.top();
                stack.pop();
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> four = stack.top();
                stack.pop();
                stack.push(one);
                stack.push(four);
                stack.push(three);
                stack.push(two);
            }
            break;
        case Pyc::SET_LINENO_A:
            // Ignore
            break;
        case Pyc::SETUP_WITH_A:
            {
                PycRef<ASTBlock> withblock = new ASTWithBlock(pos+operand);
                blocks.push(withblock);
                curblock = blocks.top();
            }
            break;
        case Pyc::WITH_EXCEPT_START:
            /* Python 3.11 with-statement exception cleanup: calls __exit__ with
               the live exception. Discard the exception sentinel that
               PUSH_EXC_INFO left on the stack and yield a None-like result so
               the implicit cleanup test does not leak the sentinel. */
            if (!stack.empty() && isExcSentinel(stack.top()))
                stack.pop();
            stack.push(nullptr);
            break;
        case Pyc::BEFORE_WITH:
        case Pyc::BEFORE_ASYNC_WITH:
            {
                /* Python 3.11 with-statement (BEFORE_ASYNC_WITH is the same
                   shape for `async with`, just entered via an awaited
                   __aenter__ -- the GET_AWAITABLE/SEND around it is handled
                   elsewhere and doesn't affect the block structure here).
                   If the pre-pass recognized this one, open a with-block; the
                   context manager stays on the stack and the following
                   STORE/POP_TOP turns it into the with-expression (and
                   optional `as <var>`). */
                auto it = withBodyEndByBefore.find(curpos);
                if (it != withBodyEndByBefore.end()) {
                    PycRef<ASTBlock> withblock = new ASTWithBlock(it->second);
                    blocks.push(withblock);
                    curblock = blocks.top();
                }
            }
            break;
        case Pyc::WITH_CLEANUP:
        case Pyc::WITH_CLEANUP_START:
            {
                // Stack top should be a None. Ignore it.
                PycRef<ASTNode> none = stack.top();
                stack.pop();

                if (none != NULL) {
                    fprintf(stderr, "Something TERRIBLE happened!\n");
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH
                        && curblock->end() == curpos) {
                    PycRef<ASTBlock> with = curblock;
                    blocks.pop();
                    curblock = blocks.top();
                    curblock->append(with.cast<ASTNode>());
                }
                else {
                    fprintf(stderr, "Something TERRIBLE happened! No matching with block found for WITH_CLEANUP at %d\n", curpos);
                }
            }
            break;
        case Pyc::WITH_CLEANUP_FINISH:
            /* Ignore this */
            break;
        case Pyc::SETUP_EXCEPT_A:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    curblock.cast<ASTContainerBlock>()->setExcept(pos+operand);
                } else {
                    PycRef<ASTBlock> next = new ASTContainerBlock(0, pos+operand);
                    blocks.push(next.cast<ASTBlock>());
                }

                /* Store the current stack for the except/finally statement(s) */
                stack_hist.push(stack);
                PycRef<ASTBlock> tryblock = new ASTBlock(ASTBlock::BLK_TRY, pos+operand, true);
                blocks.push(tryblock.cast<ASTBlock>());
                curblock = blocks.top();

                need_try = false;
            }
            break;
