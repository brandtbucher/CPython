#include "Python.h"

#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_dict.h"
#include "pycore_emscripten_signal.h"
#include "pycore_intrinsics.h"
#include "pycore_long.h"
#include "pycore_object.h"
#include "pycore_opcode_metadata.h"
#include "pycore_opcode_utils.h"
#include "pycore_pyerrors.h"
#include "pycore_range.h"
#include "pycore_setobject.h"
#include "pycore_sliceobject.h"
#include "pycore_uops.h"

#define TIER_TWO 2
#include "Python/ceval_macros.h"

#undef DEOPT_IF
#define DEOPT_IF(COND, INSTNAME) \
    if ((COND)) {                \
        goto deoptimize;         \
    }
#undef ENABLE_SPECIALIZATION
#define ENABLE_SPECIALIZATION 0
#undef ASSERT_KWNAMES_IS_NULL
#define ASSERT_KWNAMES_IS_NULL() (void)0

// Stuff that will be patched at "JIT time":
extern _PyInterpreterFrame *_jit_branch(_PyInterpreterFrame *frame,
                                        PyObject **stack_pointer,
                                        PyThreadState *tstate);
extern _PyInterpreterFrame *_jit_continue(_PyInterpreterFrame *frame,
                                          PyObject **stack_pointer,
                                          PyThreadState *tstate);
extern _PyInterpreterFrame *_jit_loop(_PyInterpreterFrame *frame,
                                      PyObject **stack_pointer,
                                      PyThreadState *tstate);
// The address of an extern can't be 0:
extern void _jit_operand_plus_one;

_PyInterpreterFrame *
_jit_entry(_PyInterpreterFrame *frame, PyObject **stack_pointer,
           PyThreadState *tstate)
{
    // Locals that the instruction implementations expect to exist:
    uint32_t opcode = _JIT_OPCODE;
    int32_t oparg = _JIT_OPARG;
    uint64_t operand = (uintptr_t)&_jit_operand_plus_one - 1;
    int pc = -1;  // XXX
    switch (opcode) {
        // Now, the actual instruction definitions (only one will be used):
#include "Python/executor_cases.c.h"
        default:
            Py_UNREACHABLE();
    }
    // Finally, the continuations:
    if (opcode == JUMP_TO_TOP) {
        assert(pc == 0);
        __attribute__((musttail))
        return _jit_loop(frame, stack_pointer, tstate);
    }
    if ((opcode == _POP_JUMP_IF_FALSE || opcode == _POP_JUMP_IF_TRUE) && pc != -1) {
        assert(pc == oparg);
        __attribute__((musttail))
        return _jit_branch(frame, stack_pointer, tstate);
    }
    __attribute__((musttail))
    return _jit_continue(frame, stack_pointer, tstate);
    // Labels that the instruction implementations expect to exist:
unbound_local_error:
    _PyEval_FormatExcCheckArg(tstate, PyExc_UnboundLocalError,
        UNBOUNDLOCAL_ERROR_MSG,
        PyTuple_GetItem(_PyFrame_GetCode(frame)->co_localsplusnames, oparg)
    );
    goto error;
pop_4_error:
    STACK_SHRINK(1);
pop_3_error:
    STACK_SHRINK(1);
pop_2_error:
    STACK_SHRINK(1);
pop_1_error:
    STACK_SHRINK(1);
error:
    _PyFrame_SetStackPointer(frame, stack_pointer);
    return NULL;
deoptimize:
    frame->prev_instr--;
    _PyFrame_SetStackPointer(frame, stack_pointer);
    return frame;
}
