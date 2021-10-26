/* Auto-generated by Tools/scripts/generate_opcode_h.py from Lib/opcode.py */
#ifndef Py_OPCODE_H
#define Py_OPCODE_H
#ifdef __cplusplus
extern "C" {
#endif


    /* Instruction opcodes for compiled code */
#define POP_TOP                   1
#define ROT_TWO                   2
#define ROT_THREE                 3
#define DUP_TOP                   4
#define DUP_TOP_TWO               5
#define ROT_FOUR                  6
#define NOP                       9
#define UNARY_POSITIVE           10
#define UNARY_NEGATIVE           11
#define UNARY_NOT                12
#define UNARY_INVERT             15
#define BINARY_POWER             19
#define BINARY_MULTIPLY          20
#define BINARY_ADD               23
#define BINARY_SUBSCR            25
#define GET_LEN                  30
#define MATCH_MAPPING            31
#define MATCH_SEQUENCE           32
#define MATCH_KEYS               33
#define COPY_DICT_WITHOUT_KEYS   34
#define PUSH_EXC_INFO            35
#define POP_EXCEPT_AND_RERAISE   37
#define WITH_EXCEPT_START        49
#define GET_AITER                50
#define GET_ANEXT                51
#define BEFORE_ASYNC_WITH        52
#define BEFORE_WITH              53
#define END_ASYNC_FOR            54
#define INPLACE_ADD              55
#define INPLACE_MULTIPLY         57
#define STORE_SUBSCR             60
#define DELETE_SUBSCR            61
#define INPLACE_POWER            67
#define GET_ITER                 68
#define GET_YIELD_FROM_ITER      69
#define PRINT_EXPR               70
#define LOAD_BUILD_CLASS         71
#define YIELD_FROM               72
#define GET_AWAITABLE            73
#define LOAD_ASSERTION_ERROR     74
#define LIST_TO_TUPLE            82
#define RETURN_VALUE             83
#define IMPORT_STAR              84
#define SETUP_ANNOTATIONS        85
#define YIELD_VALUE              86
#define POP_EXCEPT               89
#define HAVE_ARGUMENT            90
#define STORE_NAME               90
#define DELETE_NAME              91
#define UNPACK_SEQUENCE          92
#define FOR_ITER                 93
#define UNPACK_EX                94
#define STORE_ATTR               95
#define DELETE_ATTR              96
#define STORE_GLOBAL             97
#define DELETE_GLOBAL            98
#define ROT_N                    99
#define LOAD_CONST              100
#define LOAD_NAME               101
#define BUILD_TUPLE             102
#define BUILD_LIST              103
#define BUILD_SET               104
#define BUILD_MAP               105
#define LOAD_ATTR               106
#define COMPARE_OP              107
#define IMPORT_NAME             108
#define IMPORT_FROM             109
#define JUMP_FORWARD            110
#define JUMP_IF_FALSE_OR_POP    111
#define JUMP_IF_TRUE_OR_POP     112
#define JUMP_ABSOLUTE           113
#define POP_JUMP_IF_FALSE       114
#define POP_JUMP_IF_TRUE        115
#define LOAD_GLOBAL             116
#define IS_OP                   117
#define CONTAINS_OP             118
#define RERAISE                 119
#define BINARY_OP               120
#define JUMP_IF_NOT_EXC_MATCH   121
#define LOAD_FAST               124
#define STORE_FAST              125
#define DELETE_FAST             126
#define GEN_START               129
#define RAISE_VARARGS           130
#define CALL_FUNCTION           131
#define MAKE_FUNCTION           132
#define BUILD_SLICE             133
#define MAKE_CELL               135
#define LOAD_CLOSURE            136
#define LOAD_DEREF              137
#define STORE_DEREF             138
#define DELETE_DEREF            139
#define CALL_FUNCTION_KW        141
#define CALL_FUNCTION_EX        142
#define EXTENDED_ARG            144
#define LIST_APPEND             145
#define SET_ADD                 146
#define MAP_ADD                 147
#define LOAD_CLASSDEREF         148
#define MATCH_CLASS             152
#define FORMAT_VALUE            155
#define BUILD_CONST_KEY_MAP     156
#define BUILD_STRING            157
#define LOAD_METHOD             160
#define CALL_METHOD             161
#define LIST_EXTEND             162
#define SET_UPDATE              163
#define DICT_MERGE              164
#define DICT_UPDATE             165
#define CALL_METHOD_KW          166
#define BINARY_ADD_ADAPTIVE       7
#define BINARY_ADD_INT            8
#define BINARY_ADD_FLOAT         13
#define BINARY_ADD_UNICODE       14
#define BINARY_ADD_UNICODE_INPLACE_FAST  16
#define BINARY_MULTIPLY_ADAPTIVE  17
#define BINARY_MULTIPLY_INT      18
#define BINARY_MULTIPLY_FLOAT    21
#define BINARY_OP_ADAPTIVE       22
#define BINARY_OP_SAME_TYPE      24
#define BINARY_SUBSCR_ADAPTIVE   26
#define BINARY_SUBSCR_LIST_INT   27
#define BINARY_SUBSCR_TUPLE_INT  28
#define BINARY_SUBSCR_DICT       29
#define CALL_FUNCTION_ADAPTIVE   36
#define CALL_FUNCTION_BUILTIN_O  38
#define CALL_FUNCTION_BUILTIN_FAST  39
#define CALL_FUNCTION_LEN        40
#define CALL_FUNCTION_ISINSTANCE  41
#define CALL_FUNCTION_PY_SIMPLE  42
#define JUMP_ABSOLUTE_QUICK      43
#define LOAD_ATTR_ADAPTIVE       44
#define LOAD_ATTR_INSTANCE_VALUE  45
#define LOAD_ATTR_WITH_HINT      46
#define LOAD_ATTR_SLOT           47
#define LOAD_ATTR_MODULE         48
#define LOAD_GLOBAL_ADAPTIVE     56
#define LOAD_GLOBAL_MODULE       58
#define LOAD_GLOBAL_BUILTIN      59
#define LOAD_METHOD_ADAPTIVE     62
#define LOAD_METHOD_CACHED       63
#define LOAD_METHOD_CLASS        64
#define LOAD_METHOD_MODULE       65
#define LOAD_METHOD_NO_DICT      66
#define STORE_ATTR_ADAPTIVE      75
#define STORE_ATTR_INSTANCE_VALUE  76
#define STORE_ATTR_SLOT          77
#define STORE_ATTR_WITH_HINT     78
#define LOAD_FAST__LOAD_FAST     79
#define STORE_FAST__LOAD_FAST    80
#define LOAD_FAST__LOAD_CONST    81
#define LOAD_CONST__LOAD_FAST    87
#define STORE_FAST__STORE_FAST   88
#define DO_TRACING              255
#ifdef NEED_OPCODE_JUMP_TABLES
static uint32_t _PyOpcode_RelativeJump[8] = {
    0U,
    0U,
    536870912U,
    16384U,
    0U,
    0U,
    0U,
    0U,
};
static uint32_t _PyOpcode_Jump[8] = {
    0U,
    0U,
    536870912U,
    34586624U,
    0U,
    0U,
    0U,
    0U,
};
#endif /* OPCODE_TABLES */

#define HAS_CONST(op) (false\
    || ((op) == 100) \
    )

typedef enum {
    NB_AND,
    NB_FLOOR_DIVIDE,
    NB_LSHIFT,
    NB_MATRIX_MULTIPLY,
    NB_OR,
    NB_REMAINDER,
    NB_RSHIFT,
    NB_SUBTRACT,
    NB_TRUE_DIVIDE,
    NB_XOR,
    NB_INPLACE_AND,
    NB_INPLACE_FLOOR_DIVIDE,
    NB_INPLACE_LSHIFT,
    NB_INPLACE_MATRIX_MULTIPLY,
    NB_INPLACE_OR,
    NB_INPLACE_REMAINDER,
    NB_INPLACE_RSHIFT,
    NB_INPLACE_SUBTRACT,
    NB_INPLACE_TRUE_DIVIDE,
    NB_INPLACE_XOR,
} _PyOpcode_BinOp;

#define HAS_ARG(op) ((op) >= HAVE_ARGUMENT)

/* Reserve some bytecodes for internal use in the compiler.
 * The value of 240 is arbitrary. */
#define IS_ARTIFICIAL(op) ((op) > 240)

#ifdef __cplusplus
}
#endif
#endif /* !Py_OPCODE_H */
