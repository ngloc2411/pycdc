/* BuildFromCode: opcode cases, part 8/8. Included inside the switch in ASTree.cpp. */
        case Pyc::UNPACK_EX_A:
            /* `a, *b, c = x`: before + 1 + after targets, the starred one at
               index `before` */
            unpackBegin((operand & 0xff) + (operand >> 8) + 1, operand & 0xff);
            break;
        case Pyc::UNPACK_LIST_A:
        case Pyc::UNPACK_TUPLE_A:
        case Pyc::UNPACK_SEQUENCE_A:
            {
                if (operand > 0) {
                    unpackBegin(operand, -1);
                } else {
                    // Unpack zero values and assign it to top of stack or for loop variable.
                    // E.g. [] = TOS / for [] in X
                    ASTTuple::value_t vals;
                    auto tup = new ASTTuple(vals);
                    if (curblock->blktype() == ASTBlock::BLK_FOR
                        && !curblock->inited()) {
                        tup->setRequireParens(true);
                        curblock.cast<ASTIterBlock>()->setIndex(tup);
                    } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                        auto chainStore = stack.top();
                        stack.pop();
                        append_to_chain_store(chainStore, tup, stack, curblock);
                    } else {
                        curblock->append(new ASTStore(stack.top(), tup));
                        stack.pop();
                    }
                }
            }
            break;
        case Pyc::YIELD_FROM:
            {
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                // TODO: Support yielding into a non-null destination
                PycRef<ASTNode> value = stack.top();
                if (value) {
                    value->setProcessed();
                    curblock->append(new ASTReturn(value, ASTReturn::YIELD_FROM));
                }
            }
            break;
        case Pyc::SEND_A:
            {
                /* Python 3.11+ replaces YIELD_FROM with a SEND/YIELD_VALUE loop:
                   SEND(TOS1=iter/awaitable, TOS=value to send in, usually None
                   on the first pass) -- jumping forward over the loop body once
                   the iterator raises StopIteration. Unlike the old YIELD_FROM
                   (always a full statement by itself), SEND appears whether
                   `await`/`yield from` is used as a statement or as a
                   sub-expression, so just drop the sent-in value and leave the
                   awaitable/iterator on the stack as the expression's result --
                   whatever consumes it next (POP_TOP for a bare statement,
                   STORE_FAST for an assignment, ...) prints it once. */
                stack.pop();
            }
            break;
        case Pyc::YIELD_VALUE:
        case Pyc::YIELD_VALUE_A:
        case Pyc::INSTRUMENTED_YIELD_VALUE_A:
            {
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                curblock->append(new ASTReturn(value, ASTReturn::YIELD));
            }
            break;
        case Pyc::SETUP_ANNOTATIONS:
            variable_annotations = true;
            break;
        case Pyc::PRECALL_A:
        case Pyc::RESUME_A:
        case Pyc::INSTRUMENTED_RESUME_A:
            /* We just entirely ignore this / no-op */
            break;
        case Pyc::CACHE:
            /* These "fake" opcodes are used as placeholders for optimizing
               certain opcodes in Python 3.11+.  Since we have no need for
               that during disassembly/decompilation, we can just treat these
               as no-ops. */
            break;
        case Pyc::PUSH_NULL:
            if (mod->verCompare(3, 13) >= 0 && !stack.empty()) {
                /* 3.13 pushes the NULL *above* the callable; keep the
                   internal [NULL, callable] order used for 3.11/3.12. */
                PycRef<ASTNode> callable = stack.top();
                stack.pop();
                stack.push(nullptr);
                stack.push(callable);
            } else {
                stack.push(nullptr);
            }
            break;
        case Pyc::GEN_START_A:
            stack.pop();
            break;
        case Pyc::SWAP_A:
            {
                /* Python 3.11+: swap TOS with the operand-th element from the
                   top. Chained comparisons use it as a real stack operation
                   (SWAP n; COPY n). Every other use is the reordering that
                   precedes a parallel assignment (`a, b = b, a`), which is
                   modelled as the construction of the unpack tuple. */
                int pkOp = 0, pkOperand = 0;
                int savedSrc = source.pos();
                int savedPos = pos;
                if (!source.atEof())
                    bc_next(source, mod, pkOp, pkOperand, pos);
                if (mod->verCompare(3, 12) < 0 && operand == 2 && pkOp == Pyc::POP_TOP
                        && !source.atEof()) {
                    /* `return x` inside a for loop (3.11): the iterator that
                       SWAP;POP_TOP discards is not on our stack. */
                    int rOp = 0, rArg = 0, rPos = pos;
                    int midSrc = source.pos();
                    bc_next(source, mod, rOp, rArg, rPos);
                    source.setPos(midSrc);
                    bool inFor = false;
                    std::stack<PycRef<ASTBlock> > tmpb = blocks;
                    while (!tmpb.empty() && tmpb.top()->blktype() != ASTBlock::BLK_FOR)
                        tmpb.pop();
                    inFor = !tmpb.empty()
                            && !tmpb.top().cast<ASTIterBlock>()->isComprehension();
                    if (inFor && rOp == Pyc::RETURN_VALUE)
                        break;   /* POP_TOP stays consumed */
                }
                source.setPos(savedSrc);
                pos = savedPos;
                /* augmented assignment to a subscript/attribute/slice
                   (`a[i] += x`): SWAP n ... SWAP 2 reorders [obj, key, value]
                   into the store's operand order */
                if (pkOp == Pyc::COPY_A || pkOp == Pyc::POP_TOP || operand < 2
                        || pkOp == Pyc::SWAP_A || pkOp == Pyc::STORE_SUBSCR
                        || pkOp == Pyc::STORE_ATTR_A || pkOp == Pyc::STORE_SLICE
                        || (int)stack.size() < operand) {
                    stack.swap(operand);
                    break;
                }

                unpack = operand;
                ASTTuple::value_t values;
                ASTTuple::value_t next_tuple;
                values.resize(operand);
                for (int i = 0; i < operand; i++) {
                    values[operand - i - 1] = stack.top();
                    stack.pop();
                }
                auto tup = new ASTTuple(values);
                tup->setRequireParens(false);
                auto next_tup = new ASTTuple(next_tuple);
                next_tup->setRequireParens(false);
                stack.push(tup);
                stack.push(next_tup);
            }
            break;
        case Pyc::BINARY_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }
                stack.push(new ASTSubscr(dest, slice));
            }
            break;
        case Pyc::STORE_SLICE:
            {
                PycRef<ASTNode> end = stack.top();
                stack.pop();
                PycRef<ASTNode> start = stack.top();
                stack.pop();
                PycRef<ASTNode> dest = stack.top();
                stack.pop();
                PycRef<ASTNode> values = stack.top();
                stack.pop();

                if (start.type() == ASTNode::NODE_OBJECT
                        && start.cast<ASTObject>()->object() == Pyc_None) {
                    start = NULL;
                }

                if (end.type() == ASTNode::NODE_OBJECT
                        && end.cast<ASTObject>()->object() == Pyc_None) {
                    end = NULL;
                }

                PycRef<ASTNode> slice;
                if (start == NULL && end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE0);
                } else if (start == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE2, start, end);
                } else if (end == NULL) {
                    slice = new ASTSlice(ASTSlice::SLICE1, start, end);
                } else {
                    slice = new ASTSlice(ASTSlice::SLICE3, start, end);
                }

                curblock->append(new ASTStore(values, new ASTSubscr(dest, slice)));
            }
            break;
        case Pyc::COPY_A:
            {
                PycRef<ASTNode> value = stack.top(operand);
                stack.push(value);
            }
            break;
