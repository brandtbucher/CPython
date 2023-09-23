#include "Python.h"

#include "pycore_frame.h"

_PyInterpreterFrame *
_JIT_ENTRY(PyThreadState *tstate, _PyInterpreterFrame *frame, PyObject **stack_pointer)
{
    _PyFrame_SetStackPointer(frame, stack_pointer);
    return NULL;
}
