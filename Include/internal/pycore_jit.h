typedef _PyInterpreterFrame *(*_PyJITFunction)(_PyExecutorObject *self, _PyInterpreterFrame *frame, PyObject **stack_pointer);

PyAPI_FUNC(_PyJITFunction)_PyJIT_CompileTrace(_PyUOpInstruction *trace, int size);
PyAPI_FUNC(void)_PyJIT_Free(_PyJITFunction trace);
