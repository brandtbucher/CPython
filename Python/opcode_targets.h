static void *opcode_targets[256] = {
    &&TARGET_CACHE,
    &&TARGET_POP_TOP,
    &&TARGET_PUSH_NULL,
    &&TARGET_INTERPRETER_EXIT,
    &&TARGET_END_FOR,
    &&TARGET_BINARY_OP_ADD_FLOAT,
    &&TARGET_BINARY_OP_ADD_INT,
    &&TARGET_BINARY_OP_ADD_UNICODE,
    &&TARGET_BINARY_OP_INPLACE_ADD_UNICODE,
    &&TARGET_NOP,
    &&TARGET_BINARY_OP_MULTIPLY_FLOAT,
    &&TARGET_UNARY_NEGATIVE,
    &&TARGET_UNARY_NOT,
    &&TARGET_BINARY_OP_MULTIPLY_INT,
    &&TARGET_BINARY_OP_SUBTRACT_FLOAT,
    &&TARGET_UNARY_INVERT,
    &&TARGET_BINARY_OP_SUBTRACT_INT,
    &&TARGET_BINARY_SUBSCR_DICT,
    &&TARGET_BINARY_SUBSCR_GETITEM,
    &&TARGET_BINARY_SUBSCR_LIST_INT,
    &&TARGET_BINARY_SUBSCR_TUPLE_INT,
    &&TARGET_CALL_PY_EXACT_ARGS,
    &&TARGET_CALL_PY_WITH_DEFAULTS,
    &&TARGET_CALL_BOUND_METHOD_EXACT_ARGS,
    &&TARGET_CALL_BUILTIN_CLASS,
    &&TARGET_BINARY_SUBSCR,
    &&TARGET_BINARY_SLICE,
    &&TARGET_STORE_SLICE,
    &&TARGET_CALL_BUILTIN_FAST_WITH_KEYWORDS,
    &&TARGET_CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS,
    &&TARGET_GET_LEN,
    &&TARGET_MATCH_MAPPING,
    &&TARGET_MATCH_SEQUENCE,
    &&TARGET_MATCH_KEYS,
    &&TARGET_CALL_NO_KW_BUILTIN_FAST,
    &&TARGET_PUSH_EXC_INFO,
    &&TARGET_CHECK_EXC_MATCH,
    &&TARGET_CHECK_EG_MATCH,
    &&TARGET_CALL_NO_KW_BUILTIN_O,
    &&TARGET_CALL_NO_KW_ISINSTANCE,
    &&TARGET_CALL_NO_KW_LEN,
    &&TARGET_CALL_NO_KW_LIST_APPEND,
    &&TARGET_CALL_NO_KW_METHOD_DESCRIPTOR_FAST,
    &&TARGET_CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS,
    &&TARGET_CALL_NO_KW_METHOD_DESCRIPTOR_O,
    &&TARGET_CALL_NO_KW_STR_1,
    &&TARGET_CALL_NO_KW_TUPLE_1,
    &&TARGET_CALL_NO_KW_TYPE_1,
    &&TARGET_COMPARE_AND_BRANCH_FLOAT,
    &&TARGET_WITH_EXCEPT_START,
    &&TARGET_GET_AITER,
    &&TARGET_GET_ANEXT,
    &&TARGET_BEFORE_ASYNC_WITH,
    &&TARGET_BEFORE_WITH,
    &&TARGET_END_ASYNC_FOR,
    &&TARGET_CLEANUP_THROW,
    &&TARGET_COMPARE_AND_BRANCH_INT,
    &&TARGET_COMPARE_AND_BRANCH_STR,
    &&TARGET_FOR_ITER_LIST,
    &&TARGET_FOR_ITER_TUPLE,
    &&TARGET_STORE_SUBSCR,
    &&TARGET_DELETE_SUBSCR,
    &&TARGET_FOR_ITER_RANGE,
    &&TARGET_FOR_ITER_GEN,
    &&TARGET_LOAD_ATTR_CLASS,
    &&TARGET_LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN,
    &&TARGET_LOAD_ATTR_INSTANCE_VALUE,
    &&TARGET_LOAD_ATTR_MODULE,
    &&TARGET_GET_ITER,
    &&TARGET_GET_YIELD_FROM_ITER,
    &&TARGET_LOAD_ATTR_PROPERTY,
    &&TARGET_LOAD_BUILD_CLASS,
    &&TARGET_LOAD_ATTR_SLOT,
    &&TARGET_LOAD_ATTR_WITH_HINT,
    &&TARGET_LOAD_ASSERTION_ERROR,
    &&TARGET_RETURN_GENERATOR,
    &&TARGET_LOAD_ATTR_METHOD_LAZY_DICT,
    &&TARGET_LOAD_ATTR_METHOD_NO_DICT,
    &&TARGET_LOAD_ATTR_METHOD_WITH_VALUES,
    &&TARGET_LOAD_CONST__LOAD_FAST,
    &&TARGET_LOAD_FAST__LOAD_CONST,
    &&TARGET_LOAD_FAST__LOAD_FAST,
    &&TARGET_LOAD_FAST_XXX,
    &&TARGET_RETURN_VALUE,
    &&TARGET_LOAD_GLOBAL_BUILTIN,
    &&TARGET_SETUP_ANNOTATIONS,
    &&TARGET_LOAD_GLOBAL_MODULE,
    &&TARGET_STORE_ATTR_INSTANCE_VALUE,
    &&TARGET_STORE_ATTR_SLOT,
    &&TARGET_POP_EXCEPT,
    &&TARGET_STORE_NAME,
    &&TARGET_DELETE_NAME,
    &&TARGET_UNPACK_SEQUENCE,
    &&TARGET_FOR_ITER,
    &&TARGET_UNPACK_EX,
    &&TARGET_STORE_ATTR,
    &&TARGET_DELETE_ATTR,
    &&TARGET_STORE_GLOBAL,
    &&TARGET_DELETE_GLOBAL,
    &&TARGET_SWAP,
    &&TARGET_LOAD_CONST,
    &&TARGET_LOAD_NAME,
    &&TARGET_BUILD_TUPLE,
    &&TARGET_BUILD_LIST,
    &&TARGET_BUILD_SET,
    &&TARGET_BUILD_MAP,
    &&TARGET_LOAD_ATTR,
    &&TARGET_COMPARE_OP,
    &&TARGET_IMPORT_NAME,
    &&TARGET_IMPORT_FROM,
    &&TARGET_JUMP_FORWARD,
    &&TARGET_JUMP_IF_FALSE_OR_POP,
    &&TARGET_JUMP_IF_TRUE_OR_POP,
    &&TARGET_STORE_ATTR_WITH_HINT,
    &&TARGET_POP_JUMP_IF_FALSE,
    &&TARGET_POP_JUMP_IF_TRUE,
    &&TARGET_LOAD_GLOBAL,
    &&TARGET_IS_OP,
    &&TARGET_CONTAINS_OP,
    &&TARGET_RERAISE,
    &&TARGET_COPY,
    &&TARGET_RETURN_CONST,
    &&TARGET_BINARY_OP,
    &&TARGET_SEND,
    &&TARGET_LOAD_FAST,
    &&TARGET_STORE_FAST,
    &&TARGET_DELETE_FAST,
    &&TARGET_LOAD_FAST_CHECK,
    &&TARGET_POP_JUMP_IF_NOT_NONE,
    &&TARGET_POP_JUMP_IF_NONE,
    &&TARGET_RAISE_VARARGS,
    &&TARGET_GET_AWAITABLE,
    &&TARGET_MAKE_FUNCTION,
    &&TARGET_BUILD_SLICE,
    &&TARGET_JUMP_BACKWARD_NO_INTERRUPT,
    &&TARGET_MAKE_CELL,
    &&TARGET_LOAD_CLOSURE,
    &&TARGET_LOAD_DEREF,
    &&TARGET_STORE_DEREF,
    &&TARGET_DELETE_DEREF,
    &&TARGET_JUMP_BACKWARD,
    &&TARGET_COMPARE_AND_BRANCH,
    &&TARGET_CALL_FUNCTION_EX,
    &&TARGET_STORE_FAST__LOAD_FAST,
    &&TARGET_EXTENDED_ARG,
    &&TARGET_LIST_APPEND,
    &&TARGET_SET_ADD,
    &&TARGET_MAP_ADD,
    &&TARGET_LOAD_CLASSDEREF,
    &&TARGET_COPY_FREE_VARS,
    &&TARGET_YIELD_VALUE,
    &&TARGET_RESUME,
    &&TARGET_MATCH_CLASS,
    &&TARGET_STORE_FAST__STORE_FAST,
    &&TARGET_STORE_SUBSCR_DICT,
    &&TARGET_FORMAT_VALUE,
    &&TARGET_BUILD_CONST_KEY_MAP,
    &&TARGET_BUILD_STRING,
    &&TARGET_STORE_SUBSCR_LIST_INT,
    &&TARGET_UNPACK_SEQUENCE_LIST,
    &&TARGET_UNPACK_SEQUENCE_TUPLE,
    &&TARGET_UNPACK_SEQUENCE_TWO_TUPLE,
    &&TARGET_LIST_EXTEND,
    &&TARGET_SET_UPDATE,
    &&TARGET_DICT_MERGE,
    &&TARGET_DICT_UPDATE,
    &&TARGET_SEND_GEN,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&TARGET_CALL,
    &&TARGET_KW_NAMES,
    &&TARGET_CALL_INTRINSIC_1,
    &&TARGET_CALL_INTRINSIC_2,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&_unknown_opcode,
    &&TARGET_DO_TRACING
};
