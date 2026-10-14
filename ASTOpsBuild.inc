/* BuildFromCode: opcode cases, part 5/8. Included inside the switch in ASTree.cpp. */
        case Pyc::DICT_UPDATE_A:
        case Pyc::DICT_MERGE_A:
            {
                /* Python 3.9+: merge mapping at TOS into the dict below it
                   (used for {**a, **b} literals and **kwargs construction). */
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTNode> lhsNode = stack.top();
                if (lhsNode.type() == ASTNode::NODE_MAP) {
                    PycRef<ASTMap> lhs = lhsNode.cast<ASTMap>();
                    if (rhs.type() == ASTNode::NODE_MAP) {
                        for (const auto& it : rhs.cast<ASTMap>()->values())
                            lhs->add(it.first, it.second);
                    } else {
                        /* Spread of a non-literal mapping (**expr); represent
                           with a NULL key so the source writer can emit "**expr". */
                        lhs->add(nullptr, rhs);
                    }
                }
                /* Leave the (updated) dict on the stack. */
            }
            break;
        case Pyc::LIST_EXTEND_A:
            {
                PycRef<ASTNode> rhs = stack.top();
                stack.pop();
                PycRef<ASTList> lhs = stack.top().cast<ASTList>();
                stack.pop();

                ASTList::value_t result = lhs->values();

                if (rhs.type() == ASTNode::NODE_OBJECT
                        && (rhs.cast<ASTObject>()->object()->type() == PycObject::TYPE_TUPLE
                            || rhs.cast<ASTObject>()->object()->type() == PycObject::TYPE_SMALL_TUPLE)) {
                    /* [a, b, *CONST_TUPLE]: splice the constant elements in directly. */
                    for (const auto& it : rhs.cast<ASTObject>()->object().cast<PycTuple>()->values())
                        result.push_back(new ASTObject(it));
                } else if (rhs.type() == ASTNode::NODE_LIST || rhs.type() == ASTNode::NODE_TUPLE) {
                    /* [a, b, *[c, d]] / [a, b, *(c, d)]: splice the literal's elements. */
                    for (const auto& it : rhs.cast<ASTList>()->values())
                        result.push_back(it);
                } else {
                    /* [a, b, *some_expr()]: can't know the elements at decompile
                       time, so render it as a starred-unpack expression instead. */
                    result.push_back(new ASTUnary(rhs, ASTUnary::UN_UNPACK));
                }

                stack.push(new ASTList(result));
            }
            break;
        case Pyc::LOAD_ATTR_A:
            {
                PycRef<ASTNode> name = stack.top();
                if (name.type() != ASTNode::NODE_IMPORT) {
                    stack.pop();

                    if (mod->verCompare(3, 12) >= 0) {
                        if (operand & 1) {
                            /* Changed in version 3.12:
                            If the low bit of name is set, then a NULL or self is pushed to the stack
                            before the attribute or unbound method respectively. */
                            stack.push(nullptr);
                        }
                        operand >>= 1;
                    }

                    stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
                }
            }
            break;
        case Pyc::LOAD_COMMON_CONSTANT_A:
            {
                /* 3.14: 0 AssertionError, 1 NotImplementedError, 2 tuple,
                   3 all, 4 any */
                static const char* const names[] = {
                    "AssertionError", "NotImplementedError", "tuple", "all", "any"
                };
                if (operand == 0) {
                    pendingOps.push_back(std::make_pair((int)Pyc::LOAD_ASSERTION_ERROR, 0));
                } else if (operand >= 1 && operand <= 4) {
                    PycRef<PycString> nm = new PycString();
                    nm->setValue(names[operand]);
                    stack.push(new ASTName(nm));
                    if (operand >= 2)
                        commonGuard = true;
                } else {
                    fprintf(stderr, "Unsupported LOAD_COMMON_CONSTANT %d\n", operand);
                }
            }
            break;
        case Pyc::LOAD_ASSERTION_ERROR:
            {
                PycRef<PycString> assertName = new PycString();
                assertName->setValue("AssertionError");
                assertErrNode = new ASTName(assertName);
                stack.push(assertErrNode);
            }
            break;
        case Pyc::LOAD_BUILD_CLASS:
            stack.push(new ASTLoadBuildClass(new PycObject()));
            break;
        case Pyc::LOAD_CLOSURE_A:
            if (mod->verCompare(3, 6) >= 0) {
                /* In 3.6+ the closure cells are collected into a tuple via
                   BUILD_TUPLE and consumed by MAKE_FUNCTION; push a placeholder
                   so that tuple stays balanced (the value is later discarded). */
                stack.push(new ASTName(code->getCellVar(mod, operand)));
            }
            /* else: ignored (older bytecode) */
            break;
        case Pyc::MAKE_CELL_A:
        case Pyc::COPY_FREE_VARS_A:
            /* Python 3.11 function prologue; no stack effect for decompilation */
            break;
        case Pyc::LOAD_CONST_A:
            {
                PycRef<ASTObject> t_ob = new ASTObject(code->getConst(operand));

                if (t_ob->object().type() == PycObject::TYPE_SLICE) {
                    /* Python 3.14 constant-folds x[a:b:c] with constant bounds */
                    PycRef<PycSlice> sl = t_ob->object().cast<PycSlice>();
                    auto bound = [](const PycRef<PycObject>& o) -> PycRef<ASTNode> {
                        if (o == NULL || o.type() == PycObject::TYPE_NONE)
                            return NULL;
                        return new ASTObject(o);
                    };
                    PycRef<ASTNode> st = bound(sl->start()), en = bound(sl->stop()),
                                    sp = bound(sl->step());
                    PycRef<ASTNode> res;
                    if (st == NULL && en == NULL)
                        res = new ASTSlice(ASTSlice::SLICE0);
                    else if (st == NULL)
                        res = new ASTSlice(ASTSlice::SLICE2, st, en);
                    else if (en == NULL)
                        res = new ASTSlice(ASTSlice::SLICE1, st, en);
                    else
                        res = new ASTSlice(ASTSlice::SLICE3, st, en);
                    if (sp != NULL)
                        res = new ASTSlice(ASTSlice::SLICE3, res, sp);
                    stack.push(res);
                } else if ((t_ob->object().type() == PycObject::TYPE_TUPLE ||
                        t_ob->object().type() == PycObject::TYPE_SMALL_TUPLE) &&
                        !t_ob->object().cast<PycTuple>()->values().size()) {
                    ASTTuple::value_t values;
                    stack.push(new ASTTuple(values));
                } else if (t_ob->object().type() == PycObject::TYPE_NONE) {
                    stack.push(NULL);
                } else {
                    stack.push(t_ob.cast<ASTNode>());
                }
            }
            break;
        case Pyc::LOAD_DEREF_A:
        case Pyc::LOAD_CLASSDEREF_A:
            stack.push(new ASTName(code->getCellVar(mod, operand)));
            break;
        case Pyc::LOAD_FAST_AND_CLEAR_A:
            {
                /* Python 3.12+ inlined comprehension prologue:
                     GET_ITER; (LOAD_FAST_AND_CLEAR v)+; SWAP k+1;
                     BUILD_{LIST,SET,MAP} 0; SWAP 2; FOR_ITER ...
                   Model it as the pre-3.12 function form [container, iter]. */
                int k = 1;
                int op2 = 0, arg2 = 0;
                int savedSrc = source.pos();
                int savedPos = pos;
                bool ok = false;
                int container = 0;
                while (!source.atEof()) {
                    bc_next(source, mod, op2, arg2, pos);
                    if (op2 == Pyc::LOAD_FAST_AND_CLEAR_A) { k++; continue; }
                    if (op2 != Pyc::SWAP_A || arg2 != k + 1) break;
                    bc_next(source, mod, op2, arg2, pos);
                    if ((op2 != Pyc::BUILD_LIST_A && op2 != Pyc::BUILD_SET_A
                            && op2 != Pyc::BUILD_MAP_A) || arg2 != 0) break;
                    container = op2;
                    bc_next(source, mod, op2, arg2, pos);
                    ok = (op2 == Pyc::SWAP_A && arg2 == 2);
                    if (ok) {
                        /* 3.13+: GET_ITER of the iterable comes after the setup;
                           it acts on the same stack slot, leave it to the loop. */
                    }
                    break;
                }
                if (!ok) {
                    source.setPos(savedSrc);
                    pos = savedPos;
                    stack.push(nullptr);
                    break;
                }
                PycRef<ASTNode> iter = stack.top();
                stack.pop();
                inlineBase.push_back((int)stack.size());
                if (container == Pyc::BUILD_LIST_A)
                    stack.push(new ASTList(ASTList::value_t()));
                else if (container == Pyc::BUILD_SET_A)
                    stack.push(new ASTSet(ASTSet::value_t()));
                else
                    stack.push(new ASTMap);
                stack.push(iter);
                inlineComps.push_back(k);
                inlineKind.push_back(container == Pyc::BUILD_SET_A ? ASTComprehension::KIND_SET
                        : container == Pyc::BUILD_MAP_A ? ASTComprehension::KIND_DICT
                        : ASTComprehension::KIND_LIST);
            }
            break;
        case Pyc::LOAD_SMALL_INT_A:
            stack.push(new ASTObject(new PycInt(operand)));
            break;
        case Pyc::LOAD_FAST_A:
        case Pyc::LOAD_FAST_BORROW_A:
        case Pyc::LOAD_FAST_CHECK_A:
            if (mod->verCompare(1, 3) < 0)
                stack.push(new ASTName(code->getName(operand)));
            else
                stack.push(new ASTName(code->getLocal(operand)));
            break;
        case Pyc::LOAD_FAST_LOAD_FAST_A:
        case Pyc::LOAD_FAST_BORROW_LOAD_FAST_BORROW_A:
            stack.push(new ASTName(code->getLocal(operand >> 4)));
            stack.push(new ASTName(code->getLocal(operand & 0xF)));
            break;
        case Pyc::LOAD_GLOBAL_A:
            if (mod->verCompare(3, 11) >= 0) {
                // Loads the global named co_names[namei>>1] onto the stack.
                if (operand & 1) {
                    /* Changed in version 3.11: 
                    If the low bit of "NAMEI" (operand) is set, 
                    then a NULL is pushed to the stack before the global variable. */
                    stack.push(nullptr);
                }
                operand >>= 1;
            }
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::LOAD_LOCALS:
            stack.push(new ASTNode(ASTNode::NODE_LOCALS));
            break;
        case Pyc::STORE_LOCALS:
            stack.pop();
            break;
        case Pyc::LOAD_METHOD_A:
            {
                // Behave like LOAD_ATTR
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTBinary(name, new ASTName(code->getName(operand)), ASTBinary::BIN_ATTR));
            }
            break;
        case Pyc::LOAD_NAME_A:
            stack.push(new ASTName(code->getName(operand)));
            break;
        case Pyc::MAKE_CLOSURE_A:
        case Pyc::MAKE_FUNCTION_A:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();

                /* Test for the qualified name of the function (at TOS) */
                int tos_type = fun_code.cast<ASTObject>()->object().type();
                if (tos_type != PycObject::TYPE_CODE &&
                    tos_type != PycObject::TYPE_CODE2) {
                    fun_code = stack.top();
                    stack.pop();
                }

                ASTFunction::defarg_t defArgs, kwDefArgs;

                if (mod->verCompare(3, 6) >= 0) {
                    /* Python 3.6+: the operand is a flag bitmask, and each set
                       flag has a single object pushed on the stack (bottom to
                       top): 0x01 defaults tuple, 0x02 kwonly-defaults dict,
                       0x04 annotations, 0x08 closure tuple. The code object was
                       already popped above. They must be consumed top-down. */
                    if (operand & 0x08) {
                        /* closure: tuple of cells - not needed for source */
                        stack.pop();
                    }
                    if (operand & 0x04) {
                        /* annotations - not reconstructed here */
                        stack.pop();
                    }
                    if (operand & 0x02) {
                        /* keyword-only defaults: a mapping on the stack */
                        PycRef<ASTNode> kwd = stack.top();
                        stack.pop();
                        if (kwd != NULL && kwd.type() == ASTNode::NODE_MAP) {
                            for (const auto& it : kwd.cast<ASTMap>()->values())
                                kwDefArgs.push_back(it.second);
                        } else if (kwd != NULL && kwd.type() == ASTNode::NODE_CONST_MAP) {
                            for (const auto& v : kwd.cast<ASTConstMap>()->values())
                                kwDefArgs.push_back(v);
                        }
                    }
                    if (operand & 0x01) {
                        /* positional defaults: a tuple on the stack */
                        PycRef<ASTNode> defs = stack.top();
                        stack.pop();
                        if (defs != NULL && defs.type() == ASTNode::NODE_TUPLE) {
                            for (const auto& v : defs.cast<ASTTuple>()->values())
                                defArgs.push_back(v);
                        } else if (defs != NULL && defs.type() == ASTNode::NODE_OBJECT) {
                            PycRef<PycObject> o = defs.cast<ASTObject>()->object();
                            if (o->type() == PycObject::TYPE_TUPLE
                                    || o->type() == PycObject::TYPE_SMALL_TUPLE) {
                                for (const auto& it : o.cast<PycTuple>()->values())
                                    defArgs.push_back(new ASTObject(it));
                            }
                        }
                    }
                } else {
                    const int defCount = operand & 0xFF;
                    const int kwDefCount = (operand >> 8) & 0xFF;
                    for (int i = 0; i < defCount; ++i) {
                        defArgs.push_front(stack.top());
                        stack.pop();
                    }
                    for (int i = 0; i < kwDefCount; ++i) {
                        kwDefArgs.push_front(stack.top());
                        stack.pop();
                    }
                }
                stack.push(new ASTFunction(fun_code, defArgs, kwDefArgs));
            }
            break;
        case Pyc::MAKE_FUNCTION:
            {
                /* Python 3.13+: MAKE_FUNCTION only takes the code object; the
                   defaults/closure/etc. are attached afterwards by
                   SET_FUNCTION_ATTRIBUTE. */
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                stack.push(new ASTFunction(fun_code, ASTFunction::defarg_t(),
                                           ASTFunction::defarg_t()));
            }
            break;
        case Pyc::SET_FUNCTION_ATTRIBUTE_A:
            {
                /* STACK[-1] is the function, STACK[-2] the attribute value.
                   Flags: 0x01 defaults, 0x02 kw-only defaults, 0x04
                   annotations, 0x08 closure, 0x10 annotate function. */
                PycRef<ASTNode> func = stack.top();
                stack.pop();
                PycRef<ASTNode> attr = stack.top();
                stack.pop();
                if (func == NULL || func.type() != ASTNode::NODE_FUNCTION) {
                    stack.push(func);
                    break;
                }
                PycRef<ASTFunction> fn = func.cast<ASTFunction>();
                ASTFunction::defarg_t defArgs = fn->defargs();
                ASTFunction::defarg_t kwDefArgs = fn->kwdefargs();
                if (operand == 0x01) {
                    defArgs.clear();
                    if (attr != NULL && attr.type() == ASTNode::NODE_TUPLE) {
                        for (const auto& v : attr.cast<ASTTuple>()->values())
                            defArgs.push_back(v);
                    } else if (attr != NULL && attr.type() == ASTNode::NODE_OBJECT) {
                        PycRef<PycObject> o = attr.cast<ASTObject>()->object();
                        if (o->type() == PycObject::TYPE_TUPLE
                                || o->type() == PycObject::TYPE_SMALL_TUPLE) {
                            for (const auto& it : o.cast<PycTuple>()->values())
                                defArgs.push_back(new ASTObject(it));
                        }
                    }
                } else if (operand == 0x02) {
                    kwDefArgs.clear();
                    if (attr != NULL && attr.type() == ASTNode::NODE_MAP) {
                        for (const auto& it : attr.cast<ASTMap>()->values())
                            kwDefArgs.push_back(it.second);
                    } else if (attr != NULL && attr.type() == ASTNode::NODE_CONST_MAP) {
                        for (const auto& v : attr.cast<ASTConstMap>()->values())
                            kwDefArgs.push_back(v);
                    }
                }
                stack.push(new ASTFunction(fn->code(), defArgs, kwDefArgs));
            }
            break;
        case Pyc::POP_ITER:
            /* 3.14: the iterator is popped separately from END_FOR; the
               stack model already dropped it there. */
            break;
        case Pyc::NOT_TAKEN:
        case Pyc::NOP:
            break;
        case Pyc::POP_BLOCK:
            {
                if (curblock->blktype() == ASTBlock::BLK_CONTAINER ||
                        curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    /* These should only be popped by an END_FINALLY */
                    break;
                }

                if (curblock->blktype() == ASTBlock::BLK_WITH) {
                    // This should only be popped by a WITH_CLEANUP
                    break;
                }

                if (curblock->nodes().size() &&
                        curblock->nodes().back().type() == ASTNode::NODE_KEYWORD) {
                    curblock->removeLast();
                }

                if (curblock->blktype() == ASTBlock::BLK_IF
                        || curblock->blktype() == ASTBlock::BLK_ELIF
                        || curblock->blktype() == ASTBlock::BLK_ELSE
                        || curblock->blktype() == ASTBlock::BLK_TRY
                        || curblock->blktype() == ASTBlock::BLK_EXCEPT
                        || curblock->blktype() == ASTBlock::BLK_FINALLY) {
                    if (!stack_hist.empty()) {
                        stack = stack_hist.top();
                        stack_hist.pop();
                    } else {
                        fprintf(stderr, "Warning: Stack history is empty, something wrong might have happened\n");
                    }
                }
                PycRef<ASTBlock> tmp = curblock;
                blocks.pop();

                if (!blocks.empty())
                    curblock = blocks.top();

                if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                        && tmp->nodes().size() == 0)) {
                    curblock->append(tmp.cast<ASTNode>());
                }

                if (tmp->blktype() == ASTBlock::BLK_FOR && tmp->end() >= pos) {
                    stack_hist.push(stack);

                    PycRef<ASTBlock> blkelse = new ASTBlock(ASTBlock::BLK_ELSE, tmp->end());
                    blocks.push(blkelse);
                    curblock = blocks.top();
                }

                if (curblock->blktype() == ASTBlock::BLK_TRY
                        && tmp->blktype() != ASTBlock::BLK_FOR
                        && tmp->blktype() != ASTBlock::BLK_ASYNCFOR
                        && tmp->blktype() != ASTBlock::BLK_WHILE) {
                    stack = stack_hist.top();
                    stack_hist.pop();

                    tmp = curblock;
                    blocks.pop();
                    curblock = blocks.top();

                    if (!(tmp->blktype() == ASTBlock::BLK_ELSE
                            && tmp->nodes().size() == 0)) {
                        curblock->append(tmp.cast<ASTNode>());
                    }
                }

                if (curblock->blktype() == ASTBlock::BLK_CONTAINER) {
                    PycRef<ASTContainerBlock> cont = curblock.cast<ASTContainerBlock>();

                    if (tmp->blktype() == ASTBlock::BLK_ELSE && !cont->hasFinally()) {

                        /* Pop the container */
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(cont.cast<ASTNode>());

                    } else if ((tmp->blktype() == ASTBlock::BLK_ELSE && cont->hasFinally())
                            || (tmp->blktype() == ASTBlock::BLK_TRY && !cont->hasExcept())) {

                        /* Add the finally block */
                        stack_hist.push(stack);

                        PycRef<ASTBlock> final = new ASTBlock(ASTBlock::BLK_FINALLY, 0, true);
                        blocks.push(final);
                        curblock = blocks.top();
                    }
                }

                if ((curblock->blktype() == ASTBlock::BLK_FOR || curblock->blktype() == ASTBlock::BLK_ASYNCFOR)
                        && curblock->end() == pos) {
                    blocks.pop();
                    blocks.top()->append(curblock.cast<ASTNode>());
                    curblock = blocks.top();
                }
            }
            break;
        case Pyc::POP_EXCEPT:
            /* Do nothing. */
            break;
        case Pyc::PUSH_EXC_INFO:
            /* Python 3.11+: pushes the live exception. Use a sentinel so the
               `as <var>` binding and CHECK_EXC_MATCH stay balanced; it is
               consumed by the following STORE (binding) or POP_TOP (discard). */
            {
                PycRef<PycString> s = new PycString();
                s->setValue(EXC_SENTINEL);
                stack.push(new ASTName(s));
            }
            break;
        case Pyc::CHECK_EXC_MATCH:
            {
                /* Python 3.11+: compare the exception (left, kept on the stack
                   for a possible `as` binding) against the handler type. */
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.push(new ASTCompare(left, right, ASTCompare::CMP_EXCEPTION));
            }
            break;
        case Pyc::END_FOR:
            {
                bool inlineCompLoop = !inlineComps.empty();
                if (inlineCompLoop
                        && (curblock->blktype() == ASTBlock::BLK_IF
                            || curblock->blktype() == ASTBlock::BLK_ELIF)
                        && InsideComprehensionAdd(blocks)) {
                    /* close the filter block(s) and the comprehension's FOR */
                    while (blocks.size() > 1
                           && !(curblock->blktype() == ASTBlock::BLK_FOR
                                && curblock.cast<ASTIterBlock>()->isComprehension())) {
                        PycRef<ASTBlock> inner = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(inner.cast<ASTNode>());
                    }
                    if (curblock->blktype() == ASTBlock::BLK_FOR && blocks.size() > 1) {
                        PycRef<ASTBlock> fb = curblock;
                        blocks.pop();
                        curblock = blocks.top();
                        curblock->append(fb.cast<ASTNode>());
                    }
                }
                if (inlineCompLoop) {
                    /* Is this the last loop of the comprehension? Then what
                       follows is the restore of the saved loop variables:
                         [POP_ITER] SWAP k+1; STORE_FAST v (x k)
                       or, when the result is stored right away,
                         [POP_ITER] STORE_FAST target...; STORE_FAST v (x k) */
                    int k = inlineComps.back();
                    int sSrc = source.pos(), sPos = pos;
                    int o2 = 0, a2 = 0;
                    struct Op { int start, end, weight; };
                    std::vector<Op> seq;
                    bool finishing = false;
                    int lastP = pos;
                    if (!source.atEof()) {
                        bc_next(source, mod, o2, a2, pos);
                        if (o2 == Pyc::POP_ITER
                                || (o2 == Pyc::POP_TOP && mod->verCompare(3, 13) == 0)) {
                            /* the loop's own iterator pop is never real code */
                            cleanupSkip.push_back(std::make_pair(lastP, pos));
                            lastP = pos;
                            if (!source.atEof())
                                bc_next(source, mod, o2, a2, pos);
                        }
                    }
                    auto storeWeight = [](int op) {
                        return op == Pyc::STORE_FAST_A ? 1
                             : op == Pyc::STORE_FAST_STORE_FAST_A ? 2 : 0;
                    };
                    if (o2 == Pyc::SWAP_A && a2 == k + 1) {
                        seq.push_back({lastP, pos, 0});
                        int cnt = 0;
                        while (!source.atEof() && cnt < k) {
                            int st = pos;
                            bc_next(source, mod, o2, a2, pos);
                            int w = storeWeight(o2);
                            if (!w) break;
                            seq.push_back({st, pos, w});
                            cnt += w;
                        }
                        finishing = (cnt == k);
                    } else if (storeWeight(o2)) {
                        std::vector<Op> stores;
                        stores.push_back({lastP, pos, storeWeight(o2)});
                        while (!source.atEof()) {
                            int st = pos;
                            bc_next(source, mod, o2, a2, pos);
                            int w = storeWeight(o2);
                            if (!w) break;
                            stores.push_back({st, pos, w});
                        }
                        int total = 0;
                        for (const auto& st : stores) total += st.weight;
                        if (total >= k) {
                            finishing = true;
                            int need = k;
                            for (size_t i = stores.size(); i > 0 && need > 0; i--) {
                                const Op& st = stores[i - 1];
                                if (st.weight <= need) {
                                    seq.push_back(st);
                                    need -= st.weight;
                                } else {
                                    /* fused STORE_FAST_STORE_FAST (target, saved) */
                                    fusedKeepFirst.insert(st.start);
                                    need = 0;
                                }
                            }
                        }
                    }
                    source.setPos(sSrc);
                    pos = sPos;
                    if (finishing) {
                        for (const auto& r : seq)
                            cleanupSkip.push_back(std::make_pair(r.start, r.end));
                        inlineComps.pop_back();
                        inlineFinish = true;
                        inlineFinishBase = inlineBase.back();
                        inlineBase.pop_back();
                        inlineFinishKind = inlineKind.back();
                        inlineKind.pop_back();
                    }
                }
                if (!inlineCompLoop)
                    stack.pop();

                if (!inlineCompLoop && (opcode == Pyc::END_FOR) && (mod->majorVer() == 3) && (mod->minorVer() == 12)) {
                    // one additional pop for python 3.12
                    stack.pop();
                }

                // end for loop here
                /* TODO : Ensure that FOR loop ends here. 
                   Due to CACHE instructions at play, the end indicated in
                   the for loop by pycdas is not correct, it is off by
                   some small amount. */
                if (inlineCompLoop) {
                    /* inlined comprehension: loops are closed by their
                       back-edges, nothing to do here */
                }
                else if (curblock->blktype() == ASTBlock::BLK_FOR
                        && (curblock->end() == curpos || curblock->end() == 0
                            || curblock->end() < curpos)) {
                    PycRef<ASTBlock> prev = blocks.top();
                    blocks.pop();

                    curblock = blocks.top();
                    curblock->append(prev.cast<ASTNode>());
                }
                else if (curblock->blktype() == ASTBlock::BLK_FOR) {
                    /* The loop being closed here was already finished by its
                       closing JUMP_BACKWARD; this END_FOR belongs to it. */
                }
                else if (curblock->blktype() != ASTBlock::BLK_MAIN
                         && curblock->blktype() != ASTBlock::BLK_WHILE
                         && curblock->blktype() != ASTBlock::BLK_IF
                         && curblock->blktype() != ASTBlock::BLK_ELIF
                         && curblock->blktype() != ASTBlock::BLK_ELSE) {
                    /* (A BLK_MAIN here means the closing JUMP_BACKWARD already
                       ended the for block, which is the normal 3.12 shape.) */
                    fprintf(stderr, "Wrong block type %i for END_FOR\n", curblock->blktype());
                }

                if (inlineFinish) {
                    inlineFinish = false;
                    /* A filtered comprehension leaves its pieces as a yield
                       marker inside the FOR block; rebuild it from there. */
                    PycRef<ASTNode> res;
                    if (!stack.empty() && stack.top() != NULL
                            && stack.top().type() == ASTNode::NODE_COMPREHENSION) {
                        res = stack.top();
                    } else if (curblock->size() > 0) {
                        PycRef<ASTComprehension> synth =
                                SynthGenexpr(curblock->nodes().back());
                        if (synth != NULL) {
                            curblock->removeLast();
                            res = synth.cast<ASTNode>();
                        }
                    }
                    /* replace [container, loop-leftovers] by the result */
                    if (res != NULL && res.type() == ASTNode::NODE_COMPREHENSION)
                        res.cast<ASTComprehension>()->setKind(
                                (ASTComprehension::Kind)inlineFinishKind);
                    if (res != NULL && (int)stack.size() > inlineFinishBase) {
                        while ((int)stack.size() > inlineFinishBase)
                            stack.pop();
                        stack.push(res);
                    }
                }
            }
            break;
