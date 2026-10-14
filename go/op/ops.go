// Code generated from bytecode_ops.inl; DO NOT EDIT.
package op

// Opcode numbers (internal, version independent).
const (
	STOP_CODE                           = 0
	POP_TOP                             = 1
	ROT_TWO                             = 2
	ROT_THREE                           = 3
	DUP_TOP                             = 4
	DUP_TOP_TWO                         = 5
	UNARY_POSITIVE                      = 6
	UNARY_NEGATIVE                      = 7
	UNARY_NOT                           = 8
	UNARY_CONVERT                       = 9
	UNARY_CALL                          = 10
	UNARY_INVERT                        = 11
	BINARY_POWER                        = 12
	BINARY_MULTIPLY                     = 13
	BINARY_DIVIDE                       = 14
	BINARY_MODULO                       = 15
	BINARY_ADD                          = 16
	BINARY_SUBTRACT                     = 17
	BINARY_SUBSCR                       = 18
	BINARY_CALL                         = 19
	SLICE_0                             = 20
	SLICE_1                             = 21
	SLICE_2                             = 22
	SLICE_3                             = 23
	STORE_SLICE_0                       = 24
	STORE_SLICE_1                       = 25
	STORE_SLICE_2                       = 26
	STORE_SLICE_3                       = 27
	DELETE_SLICE_0                      = 28
	DELETE_SLICE_1                      = 29
	DELETE_SLICE_2                      = 30
	DELETE_SLICE_3                      = 31
	STORE_SUBSCR                        = 32
	DELETE_SUBSCR                       = 33
	BINARY_LSHIFT                       = 34
	BINARY_RSHIFT                       = 35
	BINARY_AND                          = 36
	BINARY_XOR                          = 37
	BINARY_OR                           = 38
	PRINT_EXPR                          = 39
	PRINT_ITEM                          = 40
	PRINT_NEWLINE                       = 41
	BREAK_LOOP                          = 42
	RAISE_EXCEPTION                     = 43
	LOAD_LOCALS                         = 44
	RETURN_VALUE                        = 45
	LOAD_GLOBALS                        = 46
	EXEC_STMT                           = 47
	BUILD_FUNCTION                      = 48
	POP_BLOCK                           = 49
	END_FINALLY                         = 50
	BUILD_CLASS                         = 51
	ROT_FOUR                            = 52
	NOP                                 = 53
	LIST_APPEND                         = 54
	BINARY_FLOOR_DIVIDE                 = 55
	BINARY_TRUE_DIVIDE                  = 56
	INPLACE_FLOOR_DIVIDE                = 57
	INPLACE_TRUE_DIVIDE                 = 58
	GET_LEN                             = 59
	MATCH_MAPPING                       = 60
	MATCH_SEQUENCE                      = 61
	MATCH_KEYS                          = 62
	COPY_DICT_WITHOUT_KEYS              = 63
	STORE_MAP                           = 64
	INPLACE_ADD                         = 65
	INPLACE_SUBTRACT                    = 66
	INPLACE_MULTIPLY                    = 67
	INPLACE_DIVIDE                      = 68
	INPLACE_MODULO                      = 69
	INPLACE_POWER                       = 70
	GET_ITER                            = 71
	PRINT_ITEM_TO                       = 72
	PRINT_NEWLINE_TO                    = 73
	INPLACE_LSHIFT                      = 74
	INPLACE_RSHIFT                      = 75
	INPLACE_AND                         = 76
	INPLACE_XOR                         = 77
	INPLACE_OR                          = 78
	WITH_CLEANUP                        = 79
	WITH_CLEANUP_START                  = 80
	WITH_CLEANUP_FINISH                 = 81
	IMPORT_STAR                         = 82
	SETUP_ANNOTATIONS                   = 83
	YIELD_VALUE                         = 84
	LOAD_BUILD_CLASS                    = 85
	STORE_LOCALS                        = 86
	POP_EXCEPT                          = 87
	SET_ADD                             = 88
	YIELD_FROM                          = 89
	BINARY_MATRIX_MULTIPLY              = 90
	INPLACE_MATRIX_MULTIPLY             = 91
	GET_AITER                           = 92
	GET_ANEXT                           = 93
	BEFORE_ASYNC_WITH                   = 94
	GET_YIELD_FROM_ITER                 = 95
	GET_AWAITABLE                       = 96
	BEGIN_FINALLY                       = 97
	END_ASYNC_FOR                       = 98
	RERAISE                             = 99
	WITH_EXCEPT_START                   = 100
	LOAD_ASSERTION_ERROR                = 101
	LIST_TO_TUPLE                       = 102
	CACHE                               = 103
	PUSH_NULL                           = 104
	PUSH_EXC_INFO                       = 105
	CHECK_EXC_MATCH                     = 106
	CHECK_EG_MATCH                      = 107
	BEFORE_WITH                         = 108
	RETURN_GENERATOR                    = 109
	ASYNC_GEN_WRAP                      = 110
	PREP_RERAISE_STAR                   = 111
	INTERPRETER_EXIT                    = 112
	END_FOR                             = 113
	END_SEND                            = 114
	RESERVED                            = 115
	BINARY_SLICE                        = 116
	STORE_SLICE                         = 117
	CLEANUP_THROW                       = 118
	EXIT_INIT_CHECK                     = 119
	FORMAT_SIMPLE                       = 120
	FORMAT_WITH_SPEC                    = 121
	MAKE_FUNCTION                       = 122
	TO_BOOL                             = 123
	BUILD_TEMPLATE                      = 124
	NOT_TAKEN                           = 125
	POP_ITER                            = 126
	STORE_NAME_A                        = 127
	DELETE_NAME_A                       = 128
	UNPACK_TUPLE_A                      = 129
	UNPACK_LIST_A                       = 130
	UNPACK_ARG_A                        = 131
	STORE_ATTR_A                        = 132
	DELETE_ATTR_A                       = 133
	STORE_GLOBAL_A                      = 134
	DELETE_GLOBAL_A                     = 135
	ROT_N_A                             = 136
	UNPACK_VARARG_A                     = 137
	LOAD_CONST_A                        = 138
	LOAD_NAME_A                         = 139
	BUILD_TUPLE_A                       = 140
	BUILD_LIST_A                        = 141
	BUILD_MAP_A                         = 142
	LOAD_ATTR_A                         = 143
	COMPARE_OP_A                        = 144
	IMPORT_NAME_A                       = 145
	IMPORT_FROM_A                       = 146
	ACCESS_MODE_A                       = 147
	JUMP_FORWARD_A                      = 148
	JUMP_IF_FALSE_A                     = 149
	JUMP_IF_TRUE_A                      = 150
	JUMP_ABSOLUTE_A                     = 151
	FOR_LOOP_A                          = 152
	LOAD_LOCAL_A                        = 153
	LOAD_GLOBAL_A                       = 154
	SET_FUNC_ARGS_A                     = 155
	SETUP_LOOP_A                        = 156
	SETUP_EXCEPT_A                      = 157
	SETUP_FINALLY_A                     = 158
	RESERVE_FAST_A                      = 159
	LOAD_FAST_A                         = 160
	STORE_FAST_A                        = 161
	DELETE_FAST_A                       = 162
	GEN_START_A                         = 163
	SET_LINENO_A                        = 164
	STORE_ANNOTATION_A                  = 165
	RAISE_VARARGS_A                     = 166
	CALL_FUNCTION_A                     = 167
	MAKE_FUNCTION_A                     = 168
	BUILD_SLICE_A                       = 169
	CALL_FUNCTION_VAR_A                 = 170
	CALL_FUNCTION_KW_A                  = 171
	CALL_FUNCTION_VAR_KW_A              = 172
	CALL_FUNCTION_EX_A                  = 173
	UNPACK_SEQUENCE_A                   = 174
	FOR_ITER_A                          = 175
	DUP_TOPX_A                          = 176
	BUILD_SET_A                         = 177
	JUMP_IF_FALSE_OR_POP_A              = 178
	JUMP_IF_TRUE_OR_POP_A               = 179
	POP_JUMP_IF_FALSE_A                 = 180
	POP_JUMP_IF_TRUE_A                  = 181
	CONTINUE_LOOP_A                     = 182
	MAKE_CLOSURE_A                      = 183
	LOAD_CLOSURE_A                      = 184
	LOAD_DEREF_A                        = 185
	STORE_DEREF_A                       = 186
	DELETE_DEREF_A                      = 187
	EXTENDED_ARG_A                      = 188
	SETUP_WITH_A                        = 189
	SET_ADD_A                           = 190
	MAP_ADD_A                           = 191
	UNPACK_EX_A                         = 192
	LIST_APPEND_A                       = 193
	LOAD_CLASSDEREF_A                   = 194
	MATCH_CLASS_A                       = 195
	BUILD_LIST_UNPACK_A                 = 196
	BUILD_MAP_UNPACK_A                  = 197
	BUILD_MAP_UNPACK_WITH_CALL_A        = 198
	BUILD_TUPLE_UNPACK_A                = 199
	BUILD_SET_UNPACK_A                  = 200
	SETUP_ASYNC_WITH_A                  = 201
	FORMAT_VALUE_A                      = 202
	BUILD_CONST_KEY_MAP_A               = 203
	BUILD_STRING_A                      = 204
	BUILD_TUPLE_UNPACK_WITH_CALL_A      = 205
	LOAD_METHOD_A                       = 206
	CALL_METHOD_A                       = 207
	CALL_FINALLY_A                      = 208
	POP_FINALLY_A                       = 209
	IS_OP_A                             = 210
	CONTAINS_OP_A                       = 211
	RERAISE_A                           = 212
	JUMP_IF_NOT_EXC_MATCH_A             = 213
	LIST_EXTEND_A                       = 214
	SET_UPDATE_A                        = 215
	DICT_MERGE_A                        = 216
	DICT_UPDATE_A                       = 217
	SWAP_A                              = 218
	POP_JUMP_FORWARD_IF_FALSE_A         = 219
	POP_JUMP_FORWARD_IF_TRUE_A          = 220
	COPY_A                              = 221
	BINARY_OP_A                         = 222
	SEND_A                              = 223
	POP_JUMP_FORWARD_IF_NOT_NONE_A      = 224
	POP_JUMP_FORWARD_IF_NONE_A          = 225
	GET_AWAITABLE_A                     = 226
	JUMP_BACKWARD_NO_INTERRUPT_A        = 227
	MAKE_CELL_A                         = 228
	JUMP_BACKWARD_A                     = 229
	COPY_FREE_VARS_A                    = 230
	RESUME_A                            = 231
	PRECALL_A                           = 232
	CALL_A                              = 233
	KW_NAMES_A                          = 234
	POP_JUMP_BACKWARD_IF_NOT_NONE_A     = 235
	POP_JUMP_BACKWARD_IF_NONE_A         = 236
	POP_JUMP_BACKWARD_IF_FALSE_A        = 237
	POP_JUMP_BACKWARD_IF_TRUE_A         = 238
	RETURN_CONST_A                      = 239
	LOAD_FAST_CHECK_A                   = 240
	POP_JUMP_IF_NOT_NONE_A              = 241
	POP_JUMP_IF_NONE_A                  = 242
	LOAD_SUPER_ATTR_A                   = 243
	LOAD_FAST_AND_CLEAR_A               = 244
	YIELD_VALUE_A                       = 245
	CALL_INTRINSIC_1_A                  = 246
	CALL_INTRINSIC_2_A                  = 247
	LOAD_FROM_DICT_OR_GLOBALS_A         = 248
	LOAD_FROM_DICT_OR_DEREF_A           = 249
	CALL_KW_A                           = 250
	CONVERT_VALUE_A                     = 251
	ENTER_EXECUTOR_A                    = 252
	LOAD_FAST_LOAD_FAST_A               = 253
	SET_FUNCTION_ATTRIBUTE_A            = 254
	STORE_FAST_LOAD_FAST_A              = 255
	STORE_FAST_STORE_FAST_A             = 256
	INSTRUMENTED_LOAD_SUPER_ATTR_A      = 257
	INSTRUMENTED_POP_JUMP_IF_NONE_A     = 258
	INSTRUMENTED_POP_JUMP_IF_NOT_NONE_A = 259
	INSTRUMENTED_RESUME_A               = 260
	INSTRUMENTED_CALL_A                 = 261
	INSTRUMENTED_RETURN_VALUE_A         = 262
	INSTRUMENTED_YIELD_VALUE_A          = 263
	INSTRUMENTED_CALL_FUNCTION_EX_A     = 264
	INSTRUMENTED_JUMP_FORWARD_A         = 265
	INSTRUMENTED_JUMP_BACKWARD_A        = 266
	INSTRUMENTED_RETURN_CONST_A         = 267
	INSTRUMENTED_FOR_ITER_A             = 268
	INSTRUMENTED_POP_JUMP_IF_FALSE_A    = 269
	INSTRUMENTED_POP_JUMP_IF_TRUE_A     = 270
	INSTRUMENTED_END_FOR_A              = 271
	INSTRUMENTED_END_SEND_A             = 272
	INSTRUMENTED_INSTRUCTION_A          = 273
	INSTRUMENTED_LINE_A                 = 274
	INSTRUMENTED_CALL_KW_A              = 275
	INSTRUMENTED_POP_ITER_A             = 276
	INSTRUMENTED_NOT_TAKEN_A            = 277
	INSTRUMENTED_END_ASYNC_FOR_A        = 278
	BUILD_INTERPOLATION_A               = 279
	LOAD_COMMON_CONSTANT_A              = 280
	LOAD_FAST_BORROW_A                  = 281
	LOAD_FAST_BORROW_LOAD_FAST_BORROW_A = 282
	LOAD_SMALL_INT_A                    = 283
	LOAD_SPECIAL_A                      = 284
	PYC_HAVE_ARG                        = STORE_NAME_A
	PYC_LAST_OPCODE                     = 285
	PYC_INVALID_OPCODE                  = -1
)

var names = [...]string{
	"STOP_CODE",
	"POP_TOP",
	"ROT_TWO",
	"ROT_THREE",
	"DUP_TOP",
	"DUP_TOP_TWO",
	"UNARY_POSITIVE",
	"UNARY_NEGATIVE",
	"UNARY_NOT",
	"UNARY_CONVERT",
	"UNARY_CALL",
	"UNARY_INVERT",
	"BINARY_POWER",
	"BINARY_MULTIPLY",
	"BINARY_DIVIDE",
	"BINARY_MODULO",
	"BINARY_ADD",
	"BINARY_SUBTRACT",
	"BINARY_SUBSCR",
	"BINARY_CALL",
	"SLICE_0",
	"SLICE_1",
	"SLICE_2",
	"SLICE_3",
	"STORE_SLICE_0",
	"STORE_SLICE_1",
	"STORE_SLICE_2",
	"STORE_SLICE_3",
	"DELETE_SLICE_0",
	"DELETE_SLICE_1",
	"DELETE_SLICE_2",
	"DELETE_SLICE_3",
	"STORE_SUBSCR",
	"DELETE_SUBSCR",
	"BINARY_LSHIFT",
	"BINARY_RSHIFT",
	"BINARY_AND",
	"BINARY_XOR",
	"BINARY_OR",
	"PRINT_EXPR",
	"PRINT_ITEM",
	"PRINT_NEWLINE",
	"BREAK_LOOP",
	"RAISE_EXCEPTION",
	"LOAD_LOCALS",
	"RETURN_VALUE",
	"LOAD_GLOBALS",
	"EXEC_STMT",
	"BUILD_FUNCTION",
	"POP_BLOCK",
	"END_FINALLY",
	"BUILD_CLASS",
	"ROT_FOUR",
	"NOP",
	"LIST_APPEND",
	"BINARY_FLOOR_DIVIDE",
	"BINARY_TRUE_DIVIDE",
	"INPLACE_FLOOR_DIVIDE",
	"INPLACE_TRUE_DIVIDE",
	"GET_LEN",
	"MATCH_MAPPING",
	"MATCH_SEQUENCE",
	"MATCH_KEYS",
	"COPY_DICT_WITHOUT_KEYS",
	"STORE_MAP",
	"INPLACE_ADD",
	"INPLACE_SUBTRACT",
	"INPLACE_MULTIPLY",
	"INPLACE_DIVIDE",
	"INPLACE_MODULO",
	"INPLACE_POWER",
	"GET_ITER",
	"PRINT_ITEM_TO",
	"PRINT_NEWLINE_TO",
	"INPLACE_LSHIFT",
	"INPLACE_RSHIFT",
	"INPLACE_AND",
	"INPLACE_XOR",
	"INPLACE_OR",
	"WITH_CLEANUP",
	"WITH_CLEANUP_START",
	"WITH_CLEANUP_FINISH",
	"IMPORT_STAR",
	"SETUP_ANNOTATIONS",
	"YIELD_VALUE",
	"LOAD_BUILD_CLASS",
	"STORE_LOCALS",
	"POP_EXCEPT",
	"SET_ADD",
	"YIELD_FROM",
	"BINARY_MATRIX_MULTIPLY",
	"INPLACE_MATRIX_MULTIPLY",
	"GET_AITER",
	"GET_ANEXT",
	"BEFORE_ASYNC_WITH",
	"GET_YIELD_FROM_ITER",
	"GET_AWAITABLE",
	"BEGIN_FINALLY",
	"END_ASYNC_FOR",
	"RERAISE",
	"WITH_EXCEPT_START",
	"LOAD_ASSERTION_ERROR",
	"LIST_TO_TUPLE",
	"CACHE",
	"PUSH_NULL",
	"PUSH_EXC_INFO",
	"CHECK_EXC_MATCH",
	"CHECK_EG_MATCH",
	"BEFORE_WITH",
	"RETURN_GENERATOR",
	"ASYNC_GEN_WRAP",
	"PREP_RERAISE_STAR",
	"INTERPRETER_EXIT",
	"END_FOR",
	"END_SEND",
	"RESERVED",
	"BINARY_SLICE",
	"STORE_SLICE",
	"CLEANUP_THROW",
	"EXIT_INIT_CHECK",
	"FORMAT_SIMPLE",
	"FORMAT_WITH_SPEC",
	"MAKE_FUNCTION",
	"TO_BOOL",
	"BUILD_TEMPLATE",
	"NOT_TAKEN",
	"POP_ITER",
	"STORE_NAME",
	"DELETE_NAME",
	"UNPACK_TUPLE",
	"UNPACK_LIST",
	"UNPACK_ARG",
	"STORE_ATTR",
	"DELETE_ATTR",
	"STORE_GLOBAL",
	"DELETE_GLOBAL",
	"ROT_N",
	"UNPACK_VARARG",
	"LOAD_CONST",
	"LOAD_NAME",
	"BUILD_TUPLE",
	"BUILD_LIST",
	"BUILD_MAP",
	"LOAD_ATTR",
	"COMPARE_OP",
	"IMPORT_NAME",
	"IMPORT_FROM",
	"ACCESS_MODE",
	"JUMP_FORWARD",
	"JUMP_IF_FALSE",
	"JUMP_IF_TRUE",
	"JUMP_ABSOLUTE",
	"FOR_LOOP",
	"LOAD_LOCAL",
	"LOAD_GLOBAL",
	"SET_FUNC_ARGS",
	"SETUP_LOOP",
	"SETUP_EXCEPT",
	"SETUP_FINALLY",
	"RESERVE_FAST",
	"LOAD_FAST",
	"STORE_FAST",
	"DELETE_FAST",
	"GEN_START",
	"SET_LINENO",
	"STORE_ANNOTATION",
	"RAISE_VARARGS",
	"CALL_FUNCTION",
	"MAKE_FUNCTION",
	"BUILD_SLICE",
	"CALL_FUNCTION_VAR",
	"CALL_FUNCTION_KW",
	"CALL_FUNCTION_VAR_KW",
	"CALL_FUNCTION_EX",
	"UNPACK_SEQUENCE",
	"FOR_ITER",
	"DUP_TOPX",
	"BUILD_SET",
	"JUMP_IF_FALSE_OR_POP",
	"JUMP_IF_TRUE_OR_POP",
	"POP_JUMP_IF_FALSE",
	"POP_JUMP_IF_TRUE",
	"CONTINUE_LOOP",
	"MAKE_CLOSURE",
	"LOAD_CLOSURE",
	"LOAD_DEREF",
	"STORE_DEREF",
	"DELETE_DEREF",
	"EXTENDED_ARG",
	"SETUP_WITH",
	"SET_ADD",
	"MAP_ADD",
	"UNPACK_EX",
	"LIST_APPEND",
	"LOAD_CLASSDEREF",
	"MATCH_CLASS",
	"BUILD_LIST_UNPACK",
	"BUILD_MAP_UNPACK",
	"BUILD_MAP_UNPACK_WITH_CALL",
	"BUILD_TUPLE_UNPACK",
	"BUILD_SET_UNPACK",
	"SETUP_ASYNC_WITH",
	"FORMAT_VALUE",
	"BUILD_CONST_KEY_MAP",
	"BUILD_STRING",
	"BUILD_TUPLE_UNPACK_WITH_CALL",
	"LOAD_METHOD",
	"CALL_METHOD",
	"CALL_FINALLY",
	"POP_FINALLY",
	"IS_OP",
	"CONTAINS_OP",
	"RERAISE",
	"JUMP_IF_NOT_EXC_MATCH",
	"LIST_EXTEND",
	"SET_UPDATE",
	"DICT_MERGE",
	"DICT_UPDATE",
	"SWAP",
	"POP_JUMP_FORWARD_IF_FALSE",
	"POP_JUMP_FORWARD_IF_TRUE",
	"COPY",
	"BINARY_OP",
	"SEND",
	"POP_JUMP_FORWARD_IF_NOT_NONE",
	"POP_JUMP_FORWARD_IF_NONE",
	"GET_AWAITABLE",
	"JUMP_BACKWARD_NO_INTERRUPT",
	"MAKE_CELL",
	"JUMP_BACKWARD",
	"COPY_FREE_VARS",
	"RESUME",
	"PRECALL",
	"CALL",
	"KW_NAMES",
	"POP_JUMP_BACKWARD_IF_NOT_NONE",
	"POP_JUMP_BACKWARD_IF_NONE",
	"POP_JUMP_BACKWARD_IF_FALSE",
	"POP_JUMP_BACKWARD_IF_TRUE",
	"RETURN_CONST",
	"LOAD_FAST_CHECK",
	"POP_JUMP_IF_NOT_NONE",
	"POP_JUMP_IF_NONE",
	"LOAD_SUPER_ATTR",
	"LOAD_FAST_AND_CLEAR",
	"YIELD_VALUE",
	"CALL_INTRINSIC_1",
	"CALL_INTRINSIC_2",
	"LOAD_FROM_DICT_OR_GLOBALS",
	"LOAD_FROM_DICT_OR_DEREF",
	"CALL_KW",
	"CONVERT_VALUE",
	"ENTER_EXECUTOR",
	"LOAD_FAST_LOAD_FAST",
	"SET_FUNCTION_ATTRIBUTE",
	"STORE_FAST_LOAD_FAST",
	"STORE_FAST_STORE_FAST",
	"INSTRUMENTED_LOAD_SUPER_ATTR",
	"INSTRUMENTED_POP_JUMP_IF_NONE",
	"INSTRUMENTED_POP_JUMP_IF_NOT_NONE",
	"INSTRUMENTED_RESUME",
	"INSTRUMENTED_CALL",
	"INSTRUMENTED_RETURN_VALUE",
	"INSTRUMENTED_YIELD_VALUE",
	"INSTRUMENTED_CALL_FUNCTION_EX",
	"INSTRUMENTED_JUMP_FORWARD",
	"INSTRUMENTED_JUMP_BACKWARD",
	"INSTRUMENTED_RETURN_CONST",
	"INSTRUMENTED_FOR_ITER",
	"INSTRUMENTED_POP_JUMP_IF_FALSE",
	"INSTRUMENTED_POP_JUMP_IF_TRUE",
	"INSTRUMENTED_END_FOR",
	"INSTRUMENTED_END_SEND",
	"INSTRUMENTED_INSTRUCTION",
	"INSTRUMENTED_LINE",
	"INSTRUMENTED_CALL_KW",
	"INSTRUMENTED_POP_ITER",
	"INSTRUMENTED_NOT_TAKEN",
	"INSTRUMENTED_END_ASYNC_FOR",
	"BUILD_INTERPOLATION",
	"LOAD_COMMON_CONSTANT",
	"LOAD_FAST_BORROW",
	"LOAD_FAST_BORROW_LOAD_FAST_BORROW",
	"LOAD_SMALL_INT",
	"LOAD_SPECIAL",
}
