/* BuildFromCode: opcode cases, part 1/8. Included inside the switch in ASTree.cpp. */
        case Pyc::BINARY_OP_A:
            {
                if (operand == 26 && mod->verCompare(3, 14) >= 0) {
                    /* NB_SUBSCR: a[b] */
                    PycRef<ASTNode> subscr = stack.top();
                    stack.pop();
                    PycRef<ASTNode> src = stack.top();
                    stack.pop();
                    stack.push(new ASTSubscr(src, subscr));
                    break;
                }
                ASTBinary::BinOp op = ASTBinary::from_binary_op(operand);
                if (op == ASTBinary::BIN_INVALID)
                    fprintf(stderr, "Unsupported `BINARY_OP` operand value: %d\n", operand);
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_ADD:
        case Pyc::BINARY_AND:
        case Pyc::BINARY_DIVIDE:
        case Pyc::BINARY_FLOOR_DIVIDE:
        case Pyc::BINARY_LSHIFT:
        case Pyc::BINARY_MODULO:
        case Pyc::BINARY_MULTIPLY:
        case Pyc::BINARY_OR:
        case Pyc::BINARY_POWER:
        case Pyc::BINARY_RSHIFT:
        case Pyc::BINARY_SUBTRACT:
        case Pyc::BINARY_TRUE_DIVIDE:
        case Pyc::BINARY_XOR:
        case Pyc::BINARY_MATRIX_MULTIPLY:
        case Pyc::INPLACE_ADD:
        case Pyc::INPLACE_AND:
        case Pyc::INPLACE_DIVIDE:
        case Pyc::INPLACE_FLOOR_DIVIDE:
        case Pyc::INPLACE_LSHIFT:
        case Pyc::INPLACE_MODULO:
        case Pyc::INPLACE_MULTIPLY:
        case Pyc::INPLACE_OR:
        case Pyc::INPLACE_POWER:
        case Pyc::INPLACE_RSHIFT:
        case Pyc::INPLACE_SUBTRACT:
        case Pyc::INPLACE_TRUE_DIVIDE:
        case Pyc::INPLACE_XOR:
        case Pyc::INPLACE_MATRIX_MULTIPLY:
            {
                ASTBinary::BinOp op = ASTBinary::from_opcode(opcode);
                if (op == ASTBinary::BIN_INVALID)
                    throw std::runtime_error("Unhandled opcode from ASTBinary::from_opcode");
                PycRef<ASTNode> right = stack.top();
                stack.pop();
                PycRef<ASTNode> left = stack.top();
                stack.pop();
                stack.push(new ASTBinary(left, right, op));
            }
            break;
        case Pyc::BINARY_SUBSCR:
            {
                PycRef<ASTNode> subscr = stack.top();
                stack.pop();
                PycRef<ASTNode> src = stack.top();
                stack.pop();
                stack.push(new ASTSubscr(src, subscr));
            }
            break;
        case Pyc::BREAK_LOOP:
            curblock->append(new ASTKeyword(ASTKeyword::KW_BREAK));
            break;
        case Pyc::BUILD_CLASS:
            {
                PycRef<ASTNode> class_code = stack.top();
                stack.pop();
                PycRef<ASTNode> bases = stack.top();
                stack.pop();
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                stack.push(new ASTClass(class_code, bases, name));
            }
            break;
        case Pyc::BUILD_FUNCTION:
            {
                PycRef<ASTNode> fun_code = stack.top();
                stack.pop();
                stack.push(new ASTFunction(fun_code, {}, {}));
            }
            break;
        case Pyc::BUILD_LIST_A:
            {
                ASTList::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTList(values));
            }
            break;
        case Pyc::BUILD_SET_A:
            {
                ASTSet::value_t values;
                for (int i=0; i<operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTSet(values));
            }
            break;
        case Pyc::BUILD_MAP_A:
            if (mod->verCompare(3, 5) >= 0) {
                auto map = new ASTMap;
                for (int i=0; i<operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    map->add(key, value);
                }
                stack.push(map);
            } else {
                if (stack.top().type() == ASTNode::NODE_CHAINSTORE) {
                    stack.pop();
                }
                stack.push(new ASTMap());
            }
            break;
        case Pyc::BUILD_CONST_KEY_MAP_A:
            // Top of stack will be a tuple of keys.
            // Values will start at TOS - 1.
            {
                PycRef<ASTNode> keys = stack.top();
                stack.pop();

                ASTConstMap::values_t values;
                values.reserve(operand);
                for (int i = 0; i < operand; ++i) {
                    PycRef<ASTNode> value = stack.top();
                    stack.pop();
                    values.push_back(value);
                }

                stack.push(new ASTConstMap(keys, values));
            }
            break;
        case Pyc::STORE_MAP:
            {
                PycRef<ASTNode> key = stack.top();
                stack.pop();
                PycRef<ASTNode> value = stack.top();
                stack.pop();
                PycRef<ASTMap> map = stack.top().cast<ASTMap>();
                map->add(key, value);
            }
            break;
        case Pyc::BUILD_SLICE_A:
            {
                if (operand == 2) {
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }
                } else if (operand == 3) {
                    PycRef<ASTNode> step = stack.top();
                    stack.pop();
                    PycRef<ASTNode> end = stack.top();
                    stack.pop();
                    PycRef<ASTNode> start = stack.top();
                    stack.pop();

                    if (start.type() == ASTNode::NODE_OBJECT
                            && start.cast<ASTObject>()->object() == Pyc_None) {
                        start = NULL;
                    }

                    if (end.type() == ASTNode::NODE_OBJECT
                            && end.cast<ASTObject>()->object() == Pyc_None) {
                        end = NULL;
                    }

                    if (step.type() == ASTNode::NODE_OBJECT
                            && step.cast<ASTObject>()->object() == Pyc_None) {
                        step = NULL;
                    }

                    /* We have to do this as a slice where one side is another slice */
                    /* [[a:b]:c] */

                    if (start == NULL && end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE0));
                    } else if (start == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE2, start, end));
                    } else if (end == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, start, end));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, start, end));
                    }

                    PycRef<ASTNode> lhs = stack.top();
                    stack.pop();

                    if (step == NULL) {
                        stack.push(new ASTSlice(ASTSlice::SLICE1, lhs, step));
                    } else {
                        stack.push(new ASTSlice(ASTSlice::SLICE3, lhs, step));
                    }
                }
            }
            break;
        case Pyc::BUILD_STRING_A:
            {
                // Nearly identical logic to BUILD_LIST
                ASTList::value_t values;
                for (int i = 0; i < operand; i++) {
                    values.push_front(stack.top());
                    stack.pop();
                }
                stack.push(new ASTJoinedStr(values));
            }
            break;
        case Pyc::BUILD_TUPLE_A:
            {
                // if class is a closure code, ignore this tuple
                PycRef<ASTNode> tos = stack.top();
                if (tos && tos->type() == ASTNode::NODE_LOADBUILDCLASS) {
                    break;
                }

                ASTTuple::value_t values;
                values.resize(operand);
                for (int i=0; i<operand; i++) {
                    values[operand-i-1] = stack.top();
                    stack.pop();
                }
                stack.push(new ASTTuple(values));
            }
            break;
        case Pyc::KW_NAMES_A:
            {

                int kwparams = code->getConst(operand).cast<PycTuple>()->size();
                ASTKwNamesMap kwparamList;
                std::vector<PycRef<PycObject>> keys = code->getConst(operand).cast<PycSimpleSequence>()->values();
                for (int i = 0; i < kwparams; i++) {
                    kwparamList.add(new ASTObject(keys[kwparams - i - 1]), stack.top());
                    stack.pop();
                }
                stack.push(new ASTKwNamesMap(kwparamList));
            }
            break;
        case Pyc::CALL_A:
        case Pyc::CALL_FUNCTION_A:
        case Pyc::INSTRUMENTED_CALL_A:
            {
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;

                /* Test for the load build class function */
                stack_hist.push(stack);
                /* Class keyword arguments (e.g. metaclass=) arrive via a 3.11
                   KW_NAMES map at TOS; capture them before scanning the bases. */
                ASTCall::kwparam_t classKwargs;
                if (mod->verCompare(3, 11) >= 0 && stack.top() != NULL
                        && stack.top().type() == ASTNode::NODE_KW_NAMES_MAP) {
                    PycRef<ASTKwNamesMap> km = stack.top().cast<ASTKwNamesMap>();
                    stack.pop();
                    for (const auto& kv : km->values())
                        classKwargs.push_back(kv);
                }
                int basecnt = 0;
                ASTTuple::value_t bases;
                bases.resize(basecnt);
                PycRef<ASTNode> TOS = stack.top();
                int TOS_type = TOS.type();
                // bases are NODE_NAME and NODE_BINARY at TOS
                while (TOS_type == ASTNode::NODE_NAME || TOS_type == ASTNode::NODE_BINARY) {
                    bases.resize(basecnt + 1);
                    bases[basecnt] = TOS;
                    basecnt++;
                    stack.pop();
                    TOS = stack.top();
                    TOS_type = TOS.type();
                }
                // qualified name is PycString at TOS
                PycRef<ASTNode> name = stack.top();
                stack.pop();
                PycRef<ASTNode> function = stack.top();
                stack.pop();
                PycRef<ASTNode> loadbuild = stack.top();
                stack.pop();
                int loadbuild_type = loadbuild.type();
                if (loadbuild_type == ASTNode::NODE_LOADBUILDCLASS) {
                    /* Python 3.11 pushes a NULL before LOAD_BUILD_CLASS; drop it
                       so a following decorator application sees the real callable
                       beneath the class instead of the NULL. */
                    if (mod->verCompare(3, 11) >= 0 && !stack.empty()
                            && stack.top() == nullptr) {
                        stack.pop();
                    }
                    PycRef<ASTNode> call = new ASTCall(function, pparamList, classKwargs);
                    stack.push(new ASTClass(call, new ASTTuple(bases), name));
                    stack_hist.pop();
                    break;
                }
                else
                {
                    stack = stack_hist.top();
                    stack_hist.pop();
                }

                /*
                KW_NAMES(i)
                    Stores a reference to co_consts[consti] into an internal variable for use by CALL.
                    co_consts[consti] must be a tuple of strings.
                    New in version 3.11.
                */
                if (mod->verCompare(3, 11) >= 0) {
                    PycRef<ASTNode> object_or_map = stack.top();
                    if (object_or_map.type() == ASTNode::NODE_KW_NAMES_MAP) {
                        stack.pop();
                        PycRef<ASTKwNamesMap> kwparams_map = object_or_map.cast<ASTKwNamesMap>();
                        for (ASTKwNamesMap::map_t::const_iterator it = kwparams_map->values().begin(); it != kwparams_map->values().end(); it++) {
                            kwparamList.push_front(std::make_pair(it->first, it->second));
                            pparams -= 1;
                        }
                    }
                }
                else {
                    for (int i = 0; i < kwparams; i++) {
                        PycRef<ASTNode> val = stack.top();
                        stack.pop();
                        PycRef<ASTNode> key = stack.top();
                        stack.pop();
                        kwparamList.push_front(std::make_pair(key, val));
                    }
                }
                for (int i=0; i<pparams; i++) {
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

                bool isDecoratorApply = false;
                bool compInlined = false;
                if (mod->verCompare(3, 11) < 0 && func != NULL
                        && func.type() == ASTNode::NODE_FUNCTION
                        && pparamList.size() == 1 && kwparamList.empty()) {
                    /* Python <= 3.10: a comprehension is a nested function
                       called with the iterator: MAKE_FUNCTION; <iter>; GET_ITER;
                       CALL_FUNCTION 1. */
                    PycRef<PycCode> ccode = func.cast<ASTFunction>()->code()
                            .cast<ASTObject>()->object().cast<PycCode>();
                    const char* cnm = ccode->name()->value();
                    if (cnm && (!strcmp(cnm, "<listcomp>") || !strcmp(cnm, "<setcomp>")
                            || !strcmp(cnm, "<dictcomp>") || !strcmp(cnm, "<genexpr>"))) {
                        bool savedClean = cleanBuild;
                        PycRef<ASTNode> compAst = BuildFromCode(ccode, mod);
                        cleanBuild = savedClean;
                        PycRef<ASTComprehension> comp = FindComprehension(compAst);
                        if (comp == NULL)
                            comp = SynthGenexpr(compAst);
                        if (comp != NULL && !comp->generators().empty()) {
                            if (!strcmp(cnm, "<setcomp>"))
                                comp->setKind(ASTComprehension::KIND_SET);
                            else if (!strcmp(cnm, "<dictcomp>"))
                                comp->setKind(ASTComprehension::KIND_DICT);
                            else if (!strcmp(cnm, "<genexpr>"))
                                comp->setKind(ASTComprehension::KIND_GENERATOR);
                            comp->generators().front()->setIter(pparamList.front());
                            stack.push(comp.cast<ASTNode>());
                            compInlined = true;
                        }
                    }
                }
                if (opcode == Pyc::CALL_A || opcode == Pyc::INSTRUMENTED_CALL_A) {
                    if (!stack.empty() && stack.top() == nullptr) {
                        /* Normal call: discard the NULL self slot. */
                        stack.pop();
                    } else if (!stack.empty() && stack.top() != nullptr) {
                        /* Python 3.11 leaves a non-NULL value in the self slot
                           (no PUSH_NULL) in two cases we reconstruct here:
                            - decorator application: TOS is the decorated
                              function/class, the real callable is below it;
                            - comprehension/genexpr call: the callable below is a
                              comprehension code object and TOS is the iterable. */
                        PycRef<ASTNode> below = stack.top();

                        bool topIsDecoTarget = (func.type() == ASTNode::NODE_FUNCTION
                                || func.type() == ASTNode::NODE_CLASS
                                || (func.type() == ASTNode::NODE_CALL
                                    && func.cast<ASTCall>()->isDecorator()));

                        /* `assert cond, msg` compiles to LOAD_ASSERTION_ERROR; <msg>;
                           CALL 0: the message sits in the callable slot and
                           AssertionError in the self slot (3.11+). */
                        bool belowIsAssert = below != NULL && below == assertErrNode;

                        bool belowIsComp = false;
                        if (hadGetIter && below != NULL && below.type() == ASTNode::NODE_FUNCTION) {
                            /* Only trust this as the comprehension's own
                               invocation when a GET_ITER immediately preceded
                               the call (comprehensions are always invoked as
                               `comp(iter(...))`). Without that guard, an
                               unrelated call that merely executes while the
                               comprehension's MAKE_FUNCTION result is still
                               waiting a couple of stack slots down (e.g. the
                               `im.items()` call feeding a dict comprehension)
                               would be mistaken for the real invocation. */
                            PycRef<PycCode> bcode = below.cast<ASTFunction>()->code()
                                    .cast<ASTObject>()->object().cast<PycCode>();
                            const char* bnm = bcode->name()->value();
                            belowIsComp = bnm && (!strcmp(bnm, "<listcomp>")
                                    || !strcmp(bnm, "<setcomp>")
                                    || !strcmp(bnm, "<dictcomp>")
                                    || !strcmp(bnm, "<genexpr>"));
                        }

                        if (belowIsAssert) {
                            pparamList.push_front(func);
                            func = stack.top();
                            stack.pop();
                        } else if (belowIsComp) {
                            /* Inline the comprehension. The callable is `below`
                               and the iterable is `func` (TOS). Decompile the
                               comprehension body and substitute its implicit
                               ".0" iterator with the real iterable. */
                            PycRef<PycCode> ccode = below.cast<ASTFunction>()->code()
                                    .cast<ASTObject>()->object().cast<PycCode>();
                            bool savedClean = cleanBuild;
                            PycRef<ASTNode> compAst = BuildFromCode(ccode, mod);
                            cleanBuild = savedClean;
                            PycRef<ASTComprehension> comp = FindComprehension(compAst);
                            if (comp == NULL)
                                comp = SynthGenexpr(compAst);   /* generator expr */
                            if (comp != NULL && !comp->generators().empty()) {
                                const char* cnm = ccode->name()->value();
                                if (cnm && !strcmp(cnm, "<setcomp>"))
                                    comp->setKind(ASTComprehension::KIND_SET);
                                else if (cnm && !strcmp(cnm, "<dictcomp>"))
                                    comp->setKind(ASTComprehension::KIND_DICT);
                                else if (cnm && !strcmp(cnm, "<genexpr>"))
                                    comp->setKind(ASTComprehension::KIND_GENERATOR);
                                comp->generators().front()->setIter(func);
                                stack.pop();   /* remove the comprehension callable */
                                stack.push(comp.cast<ASTNode>());
                                compInlined = true;
                            }
                            /* else: leave as-is and fall through to a plain call. */
                        } else if (topIsDecoTarget) {
                            /* Decorator application: `func` (TOS) is the decorated
                               target, the real callable is below. */
                            PycRef<ASTNode> target = func;
                            func = stack.top();
                            stack.pop();
                            if (target.type() == ASTNode::NODE_FUNCTION) {
                                PycRef<PycCode> tcode = target.cast<ASTFunction>()->code()
                                        .cast<ASTObject>()->object().cast<PycCode>();
                                PycRef<PycString> tname = tcode->name();
                                if (!tname->isEqual("<lambda>")) {
                                    /* Emit the def, then reference it by name. */
                                    PycRef<ASTNode> decor_name = new ASTName(tname);
                                    curblock->append(new ASTStore(target, decor_name));
                                    target = decor_name;
                                }
                            } else if (target.type() == ASTNode::NODE_CLASS) {
                                /* Decorated class: emit the class body, then
                                   reference it by name as the decorator argument. */
                                PycRef<ASTNode> cname = target.cast<ASTClass>()->name();
                                PycRef<ASTNode> decor_name;
                                if (cname != NULL && cname.type() == ASTNode::NODE_NAME) {
                                    decor_name = cname;
                                } else if (cname != NULL && cname.type() == ASTNode::NODE_OBJECT) {
                                    PycRef<PycString> s =
                                            cname.cast<ASTObject>()->object().try_cast<PycString>();
                                    if (s != NULL)
                                        decor_name = new ASTName(s);
                                }
                                if (decor_name != NULL) {
                                    curblock->append(new ASTStore(target, decor_name));
                                    target = decor_name;
                                }
                            }
                            pparamList.push_front(target);
                            isDecoratorApply = true;
                        }
                        /* else: a non-null self slot we don't special-case (e.g.
                           method calls collapsed by LOAD_METHOD); behave as before
                           with `func` as the callable. */
                    }
                }

                if (!compInlined) {
                    PycRef<ASTNode> callNode = new ASTCall(func, pparamList, kwparamList);
                    if (isDecoratorApply)
                        callNode.cast<ASTCall>()->setDecorator(true);
                    stack.push(callNode);
                }
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_A:
            {
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                stack.push(call);
            }
            break;
        case Pyc::CALL_FUNCTION_VAR_KW_A:
            {
                PycRef<ASTNode> kw = stack.top();
                stack.pop();
                PycRef<ASTNode> var = stack.top();
                stack.pop();
                int kwparams = (operand & 0xFF00) >> 8;
                int pparams = (operand & 0xFF);
                ASTCall::kwparam_t kwparamList;
                ASTCall::pparam_t pparamList;
                for (int i=0; i<kwparams; i++) {
                    PycRef<ASTNode> val = stack.top();
                    stack.pop();
                    PycRef<ASTNode> key = stack.top();
                    stack.pop();
                    kwparamList.push_front(std::make_pair(key, val));
                }
                for (int i=0; i<pparams; i++) {
                    pparamList.push_front(stack.top());
                    stack.pop();
                }
                PycRef<ASTNode> func = stack.top();
                stack.pop();

                PycRef<ASTNode> call = new ASTCall(func, pparamList, kwparamList);
                call.cast<ASTCall>()->setKW(kw);
                call.cast<ASTCall>()->setVar(var);
                stack.push(call);
            }
            break;
