/* BuildFromCode: opcode cases, part 2/8. Included inside the switch in ASTree.cpp. */
        case Pyc::CALL_FUNCTION_EX_A:
        case Pyc::INSTRUMENTED_CALL_FUNCTION_EX_A:
            {
                /* Call with iterable args and (optionally) a mapping of kwargs.
                   Low bit of the flag operand means a kwargs mapping is present. */
                PycRef<ASTNode> kw;
                /* 3.14 dropped the oparg: the kwargs slot is always present
                   (a NULL when there are none). */
                if ((operand & 0x01) || mod->verCompare(3, 14) >= 0) {
                    kw = stack.top();
                    stack.pop();
                }
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                /* Python 3.11+ pushes a NULL sentinel below the callable. */
                if (mod->verCompare(3, 11) >= 0 && !stack.empty()
                        && stack.top() == nullptr) {
                    stack.pop();
                }

                ASTCall::pparam_t exPos;
                ASTCall::kwparam_t exKw;
                /* `f(a, *args)` compiles to f(*(a, *args)) and
                   `f(c=c, **kw)` to f(**{'c': c, **kw}); undo that so the
                   call reads like the original source. */
                if (var != NULL && var.type() == ASTNode::NODE_TUPLE) {
                    for (const auto& v : var.cast<ASTTuple>()->values())
                        exPos.push_back(v);
                    var = NULL;
                } else if (var != NULL && var.type() == ASTNode::NODE_OBJECT
                           && (var.cast<ASTObject>()->object()->type() == PycObject::TYPE_TUPLE
                               || var.cast<ASTObject>()->object()->type() == PycObject::TYPE_SMALL_TUPLE)) {
                    /* f(1, **kw): the positionals were folded into a constant tuple */
                    for (const auto& v : var.cast<ASTObject>()->object().cast<PycTuple>()->values())
                        exPos.push_back(new ASTObject(v));
                    var = NULL;
                }
                if (kw != NULL && kw.type() == ASTNode::NODE_MAP) {
                    PycRef<ASTNode> unpackedKw;
                    int unpackCount = 0;
                    bool allSimple = true;
                    for (const auto& kv : kw.cast<ASTMap>()->values()) {
                        if (kv.first == NULL) {
                            unpackCount++;
                            unpackedKw = kv.second;
                        } else if (kv.first.type() != ASTNode::NODE_OBJECT
                                   || kv.first.cast<ASTObject>()->object()
                                          .try_cast<PycString>() == NULL) {
                            allSimple = false;
                        }
                    }
                    if (allSimple && unpackCount <= 1) {
                        for (const auto& kv : kw.cast<ASTMap>()->values()) {
                            if (kv.first != NULL)
                                exKw.push_back(kv);
                        }
                        kw = unpackedKw;   /* NULL when there was no ** entry */
                    }
                }

                PycRef<ASTNode> call = new ASTCall(func, exPos, exKw);
                if (var != NULL)
                    call.cast<ASTCall>()->setVar(var);
                if (kw != NULL)
                    call.cast<ASTCall>()->setKW(kw);
                stack.push(call);
            }
            break;
        case Pyc::CALL_METHOD_A:
            {
                ASTCall::pparam_t pparamList;
                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> param = stack.top();
                    stack.pop();
                    if (param.type() == ASTNode::NODE_FUNCTION) {
                        PycRef<ASTNode> fun_code = param.cast<ASTFunction>()->code();
                        PycRef<PycCode> code_src = fun_code.cast<ASTObject>()->object().cast<PycCode>();
                        PycRef<PycString> function_name = code_src->name();
                        if (function_name->isEqual("<lambda>")) {
                            pparamList.push_front(param);
                        } else {
                            // Decorator used
                            PycRef<ASTNode> decor_name = new ASTName(function_name);
                            curblock->append(new ASTStore(param, decor_name));

                            pparamList.push_front(decor_name);
                        }
                    } else {
                        pparamList.push_front(param);
                    }
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                stack.push(new ASTCall(func, pparamList, ASTCall::kwparam_t()));
            }
            break;
        case Pyc::CONTINUE_LOOP_A:
            curblock->append(new ASTKeyword(ASTKeyword::KW_CONTINUE));
            break;
        case Pyc::JUMP_IF_NOT_EXC_MATCH_A:
            {
                /* 3.9/3.10: COMPARE_OP(exception match) + POP_JUMP_IF_FALSE
                   fused into one instruction. */
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTCompare(left, right, ASTCompare::CMP_EXCEPTION));
                pendingOps.push_back(std::make_pair((int)Pyc::POP_JUMP_IF_FALSE_A, operand));
            }
            break;
        case Pyc::COMPARE_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                auto arg = operand;
                if (mod->verCompare(3, 12) == 0)
                    arg >>= 4; // changed under GH-100923
                else if (mod->verCompare(3, 13) >= 0)
                    arg >>= 5;
                stack.push(new ASTCompare(left, right, arg));
            }
            break;
        case Pyc::CONTAINS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'in' and 1 for 'not in'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_NOT_IN : ASTCompare::CMP_IN));
            }
            break;
        case Pyc::DELETE_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                curblock->append(new ASTDelete(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR)));
            }
            break;
        case Pyc::DELETE_GLOBAL_A:
            code->markGlobal(code->getName(operand));
            /* Fall through */
        case Pyc::DELETE_NAME_A:
            {
                PycRef<PycString> varname = code->getName(operand);

                if (varname->length() >= 2 && varname->value()[0] == '_'
                        && varname->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                PycRef<ASTNode> name = new ASTName(varname);
                if (isExceptVarDelete(curblock, name))
                    break;
                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_FAST_A:
            {
                PycRef<ASTNode> name;

                if (mod->verCompare(1, 3) < 0)
                    name = new ASTName(code->getName(operand));
                else
                    name = new ASTName(code->getLocal(operand));

                if (name.cast<ASTName>()->name()->value()[0] == '_'
                        && name.cast<ASTName>()->name()->value()[1] == '[') {
                    /* Don't show deletes that are a result of list comps. */
                    break;
                }

                if (isExceptVarDelete(curblock, name))
                    break;

                curblock->append(new ASTDelete(name));
            }
            break;
        case Pyc::DELETE_SLICE_0:
            {
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE0))));
            }
            break;
        case Pyc::DELETE_SLICE_1:
            {
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE1, upper))));
            }
            break;
        case Pyc::DELETE_SLICE_2:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE2, NULL, lower))));
            }
            break;
        case Pyc::DELETE_SLICE_3:
            {
                PycRef<ASTNode> lower = stack.top();
                stack.pop();
                PycRef<ASTNode> upper = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, new ASTSlice(ASTSlice::SLICE3, upper, lower))));
            }
            break;
        case Pyc::DELETE_SUBSCR:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();

                curblock->append(new ASTDelete(new ASTSubscr(name, key)));
            }
            break;
        case Pyc::DUP_TOP:
            {
                /* chained comparison head (<= 3.10): DUP_TOP; ROT_THREE; COMPARE
                   turns [A, B] into [B, A, B] sharing the node of B. */
                if (mod->verCompare(3, 11) < 0 && stack.size() >= 2) {
                    int sp = source.pos();
                    int o2, a2, p2 = pos, o3, a3, p3;
                    bc_next(source, mod, o2, a2, p2);
                    bool isChain = false;
                    if (o2 == Pyc::ROT_THREE) {
                        p3 = p2;
                        bc_next(source, mod, o3, a3, p3);
                        isChain = (o3 == Pyc::COMPARE_OP_A || o3 == Pyc::IS_OP_A
                                   || o3 == Pyc::CONTAINS_OP_A);
                    }
                    if (isChain) {
                        PycRef<ASTNode> b = stack.top();
                        stack.pop();
                        PycRef<ASTNode> a = stack.top();
                        stack.pop();
                        stack.push(b);
                        stack.push(a);
                        stack.push(b);
                        source.setPos(p2);
                        pos = p2;
                        break;
                    }
                    source.setPos(sp);
                }
                if (stack.top().type() == PycObject::TYPE_NULL) {
                    stack.push(stack.top());
                } else if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    auto chainstore = stack.top();
                    stack.pop();
                    stack.push(stack.top());
                    stack.push(chainstore);
                } else {
                    stack.push(stack.top());
                    ASTNodeList::list_t targets;
                    stack.push(new ASTChainStore(targets, stack.top()));
                }
            }
            break;
        case Pyc::DUP_TOP_TWO:
            {
                PycRef<ASTNode> first = stack.top();
                stack.pop();
                PycRef<ASTNode> second = stack.top();

                stack.push(first);
                stack.push(second);
                stack.push(first);
            }
            break;
        case Pyc::DUP_TOPX_A:
            {
                std::stack<PycRef<ASTNode> > first;
                std::stack<PycRef<ASTNode> > second;

                for (int i = 0; i < operand; i++) {
                    PycRef<ASTNode> node = stack.top();
                    stack.pop();
                    first.push(node);
                    second.push(node);
                }

                while (first.size()) {
                    stack.push(first.top());
                    first.pop();
                }

                while (second.size()) {
                    stack.push(second.top());
                    second.pop();
                }
            }
            break;
        case Pyc::END_FINALLY:
            {
                bool isFinally = false;
                if (curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    PycRef<ASTBlock> final = curblock;
                    blocks.pop();

                    stack = stack_hist.top();
                    stack_hist.pop();

                    curblock = blocks.top();
                    curblock->append(final.cast<ASTNode>());
                    isFinally = true;
                } else if (curblock->blktype() == ASTBlock::BLK_EXCEPT) {
                    blocks.pop();
                    PycRef<ASTBlock> prev = curblock;

                    bool isUninitAsyncFor = false;
                    if (blocks.top()->blktype() == ASTBlock::BLK_CONTAINER) {
                        auto container = blocks.top();
                        blocks.pop();
                        auto asyncForBlock = blocks.top();
                        isUninitAsyncFor = asyncForBlock->blktype() == ASTBlock::BLK_ASYNCFOR && !asyncForBlock->inited();
                        if (isUninitAsyncFor) {
                            auto tryBlock = container->nodes().front().cast<ASTBlock>();
                            if (!tryBlock->nodes().empty() && tryBlock->blktype() == ASTBlock::BLK_TRY) {
                                auto store = tryBlock->nodes().front().try_cast<ASTStore>();
                                if (store) {
                                    asyncForBlock.cast<ASTIterBlock>()->setIndex(store->dest());
                                }
                            }
                            curblock = blocks.top();
                            stack = stack_hist.top();
                            stack_hist.pop();
                            if (!curblock->inited())
                                fprintf(stderr, "Error when decompiling 'async for'.\n");
                        } else {
                            blocks.push(container);
                        }
                    }

                    if (!isUninitAsyncFor) {
                        if (curblock->size() != 0) {
                            blocks.top()->append(curblock.cast<ASTNode>());
                        }

                        curblock = blocks.top();

                        /* Turn it into an else statement. */
                        if (curblock->end() != pos || curblock.cast<ASTContainerBlock>()->hasFinally()) {
                            PycRef<ASTBlock> elseblk = new ASTBlock(ASTBlock::BLK_ELSE, prev->end());
                            elseblk->init();
                            blocks.push(elseblk);
                            curblock = blocks.top();
                        }
                        else {
                            stack = stack_hist.top();
                            stack_hist.pop();
                        }
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    /* This marks the end of the except block(s). */
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();
                    if (!cont->hasFinally() || isFinally) {
                        /* If there's no finally block, pop the container. */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());
                    }
                }
            }
            break;
        case Pyc::EXEC_STMT:
            {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                PycRef<ASTNode> loc = stack.top();
                stack.pop();
                PycRef<ASTNode> glob = stack.top();
                stack.pop();
                PycRef<ASTNode> stmt = stack.top();
                stack.pop();

                curblock->append(new ASTExec(stmt, glob, loc));
            }
            break;
        case Pyc::FOR_ITER_A:
        case Pyc::INSTRUMENTED_FOR_ITER_A:
            {
                PycRef<ASTNode> iter = stack.top(); // Iterable
                if (mod->verCompare(3, 12) < 0) {
                    // Do not pop the iterator for py 3.12+
                    stack.pop();
                }
                /* Pop it? Don't pop it? */

                int end;
                bool comprehension = false;

                // before 3.8, there is a SETUP_LOOP instruction with block start and end position,
                //    the operand is usually a jump to a POP_BLOCK instruction
                // after 3.8, block extent has to be inferred implicitly; the operand is a jump to a position after the for block
                if (mod->majorVer() == 3 && mod->minorVer() >= 8) {
                    end = operand;
                    if (mod->verCompare(3, 10) >= 0)
                        end *= sizeof(uint16_t); // // BPO-27129
                    end += pos;
                    /* 3.12+: the jump is relative to the instruction after
                       FOR_ITER's single inline CACHE entry. */
                    if (mod->verCompare(3, 12) >= 0)
                        end += sizeof(uint16_t);
                    {
                        const char* cn = code->name()->value();
                        comprehension = cn && (!strcmp(cn, "<listcomp>")
                                || !strcmp(cn, "<setcomp>")
                                || !strcmp(cn, "<dictcomp>"));
                        if (!inlineComps.empty())
                            comprehension = true;
                    }
                } else {
                    PycRef<ASTBlock> top = blocks.top();
                    end = top->end(); // block end position from SETUP_LOOP
                    if (top->blktype() == ASTBlock::BLK_WHILE) {
                        blocks.pop();
                    } else {
                        comprehension = true;
                    }
                }

                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, end, iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                stack.push(NULL);
            }
            break;
        case Pyc::FOR_LOOP_A:
            {
                PycRef<ASTNode> curidx = stack.top(); // Current index
                stack.pop();
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                bool comprehension = false;
                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                } else {
                    comprehension = true;
                }
                PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_FOR, curpos, top->end(), iter);
                forblk->setComprehension(comprehension);
                blocks.push(forblk.cast<ASTBlock>());
                curblock = blocks.top();

                /* Python Docs say:
                      "push the sequence, the incremented counter,
                       and the current item onto the stack." */
                stack.push(iter);
                stack.push(curidx);
                stack.push(NULL); // We can totally hack this >_>
            }
            break;
        case Pyc::GET_AITER:
            {
                // Logic similar to FOR_ITER_A
                PycRef<ASTNode> iter = stack.top(); // Iterable
                stack.pop();

                PycRef<ASTBlock> top = blocks.top();
                if (top->blktype() == ASTBlock::BLK_WHILE) {
                    blocks.pop();
                    PycRef<ASTIterBlock> forblk = new ASTIterBlock(ASTBlock::BLK_ASYNCFOR, curpos, top->end(), iter);
                    blocks.push(forblk.cast<ASTBlock>());
                    curblock = blocks.top();
                    stack.push(nullptr);
                } else {
                     fprintf(stderr, "Unsupported use of GET_AITER outside of SETUP_LOOP\n");
                }
            }
            break;
        case Pyc::GET_ANEXT:
            break;
        case Pyc::TO_BOOL:
            /* 3.13: implicit bool() conversion before a conditional jump. */
            break;
        case Pyc::STORE_FAST_STORE_FAST_A:
            pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand >> 4));
            if (!fusedKeepFirst.count(curpos))
                pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand & 0xF));
            break;
        case Pyc::STORE_FAST_LOAD_FAST_A:
            pendingOps.push_back(std::make_pair((int)Pyc::STORE_FAST_A, operand >> 4));
            pendingOps.push_back(std::make_pair((int)Pyc::LOAD_FAST_A, operand & 0xF));
            break;
        case Pyc::CONVERT_VALUE_A:
            {
                /* 3.13 f-string: 1 = !s, 2 = !r, 3 = !a (formatting follows). */
                PycRef<ASTNode> val = stack.top();
                stack.pop();
                stack.push(new ASTFormattedValue(val,
                        static_cast<ASTFormattedValue::ConversionFlag>(operand), nullptr));
            }
            break;
        case Pyc::FORMAT_SIMPLE:
            {
                PycRef<ASTNode> val = stack.top();
                if (val == NULL || val.type() != ASTNode::NODE_FORMATTEDVALUE) {
                    stack.pop();
                    stack.push(new ASTFormattedValue(val, ASTFormattedValue::NONE, nullptr));
                }
            }
            break;
        case Pyc::FORMAT_WITH_SPEC:
            {
                PycRef<ASTNode> spec = stack.top();
                stack.pop();
                PycRef<ASTNode> val = stack.top();
                stack.pop();
                int conv = ASTFormattedValue::NONE;
                if (val != NULL && val.type() == ASTNode::NODE_FORMATTEDVALUE) {
                    PycRef<ASTFormattedValue> fv = val.cast<ASTFormattedValue>();
                    conv = fv->conversion() & ASTFormattedValue::CONVERSION_MASK;
                    val = fv->val();
                }
                stack.push(new ASTFormattedValue(val,
                        static_cast<ASTFormattedValue::ConversionFlag>(
                            conv | ASTFormattedValue::HAVE_FMT_SPEC), spec));
            }
            break;
        case Pyc::CALL_KW_A:
            {
                /* 3.13: like KW_NAMES + CALL; the names tuple is on top. */
                PycRef<ASTNode> names = stack.top();
                stack.pop();
                ASTKwNamesMap kwparamList;
                if (names != NULL && names.type() == ASTNode::NODE_OBJECT) {
                    std::vector<PycRef<PycObject> > keys =
                        names.cast<ASTObject>()->object().cast<PycSimpleSequence>()->values();
                    int n = (int)keys.size();
                    for (int i = 0; i < n; i++) {
                        kwparamList.add(new ASTObject(keys[n - i - 1]), stack.top());
                        stack.pop();
                    }
                }
                stack.push(new ASTKwNamesMap(kwparamList));
                pendingOps.push_back(std::make_pair((int)Pyc::CALL_A, operand));
            }
            break;
        case Pyc::FORMAT_VALUE_A:
            {
                auto conversion_flag = static_cast<ASTFormattedValue::ConversionFlag>(operand);
                PycRef<ASTNode> format_spec = nullptr;
                if (conversion_flag & ASTFormattedValue::HAVE_FMT_SPEC) {
                    format_spec = stack.top();
                    stack.pop();
                }
                auto val = stack.top();
                stack.pop();
                stack.push(new ASTFormattedValue(val, conversion_flag, format_spec));
            }
            break;
        case Pyc::GET_AWAITABLE:
        case Pyc::GET_AWAITABLE_A:
            {
                /* Python 3.11+ adds an operand (0 = plain await, 1/2 = inside
                   async-with __aenter__/__aexit__) that doesn't affect the
                   reconstructed source, so it's safe to ignore here. */
                PycRef<ASTNode> object = stack.top();
                stack.pop();
                stack.push(new ASTAwaitable(object));
            }
            break;
        case Pyc::GET_ITER:
        case Pyc::GET_YIELD_FROM_ITER:
            /* We just entirely ignore this, beyond noting it happened (see
               sawGetIter/hadGetIter) so a following CALL can recognize a
               genuine comprehension invocation. */
            sawGetIter = true;
            break;
        case Pyc::IMPORT_NAME_A:
            if (mod->majorVer() == 1) {
                stack.push(new ASTImport(new ASTName(code->getName(operand)), NULL));
            } else {
                PycRef<ASTNode> fromlist = stack.top();
                stack.pop();
                int level = 0;
                if (mod->verCompare(2, 5) >= 0) {
                    /* Level: 0 = absolute import, N>0 = N leading dots in a
                       relative `from .[.[pkg]] import x`. Needed to render
                       the dots -- co_names[namei] is "" for a bare `from .
                       import x` and would otherwise print as `from  import
                       x` (missing dot(s), invalid syntax). */
                    PycRef<ASTNode> levelNode = stack.top();
                    stack.pop();
                    if (levelNode != NULL && levelNode.type() == ASTNode::NODE_OBJECT) {
                        PycRef<PycObject> levelObj = levelNode.cast<ASTObject>()->object();
                        if (levelObj.type() == PycObject::TYPE_INT) {
                            level = levelObj.cast<PycInt>()->value();
                        }
                    }
                }
                PycRef<ASTImport> imp = new ASTImport(new ASTName(code->getName(operand)), fromlist);
                imp->setLevel(level);
                stack.push(imp.cast<ASTNode>());
            }
            break;
        case Pyc::IMPORT_FROM_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::IMPORT_STAR:
            {
                PycRef<ASTNode> import = stack.top();
                stack.pop();
                curblock->append(new ASTStore(import, NULL));
            }
            break;
        case Pyc::IS_OP_A:
            {
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                // The operand will be 0 for 'is' and 1 for 'is not'.
                stack.push(new ASTCompare(left, right, operand ? ASTCompare::CMP_IS_NOT : ASTCompare::CMP_IS));
            }
            break;
