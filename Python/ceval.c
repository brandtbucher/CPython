/* Execute compiled code */

/* XXX TO DO:
   XXX speed up searching for keywords by using a dictionary
   XXX document it!
   */

#define _PY_INTERPRETER

#include "Python.h"
#include "pycore_abstract.h"      // _PyIndex_Check()
#include "pycore_call.h"          // _PyObject_FastCallDictTstate()
#include "pycore_ceval.h"         // _PyEval_SignalAsyncExc()
#include "pycore_code.h"
#include "pycore_function.h"
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_object.h"        // _PyObject_GC_TRACK()
#include "pycore_moduleobject.h"  // PyModuleObject
#include "pycore_opcode.h"        // EXTRA_CASES
#include "pycore_pyerrors.h"      // _PyErr_Fetch()
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_range.h"         // _PyRangeIterObject
#include "pycore_sliceobject.h"   // _PyBuildSlice_ConsumeRefs
#include "pycore_sysmodule.h"     // _PySys_Audit()
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_emscripten_signal.h"  // _Py_CHECK_EMSCRIPTEN_SIGNALS

#include "pycore_dict.h"
#include "dictobject.h"
#include "pycore_frame.h"
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"         // struct PyMemberDef, T_OFFSET_EX
#include "pycore_uops.h"

#include <ctype.h>
#include <stdbool.h>

#ifdef Py_DEBUG
   /* For debugging the interpreter: */
#  define LLTRACE  1      /* Low-level trace feature */
#endif

#if !defined(Py_BUILD_CORE)
#  error "ceval.c must be build with Py_BUILD_CORE define for best performance"
#endif

#ifndef Py_DEBUG
// GH-89279: The MSVC compiler does not inline these static inline functions
// in PGO build in _PyEval_EvalFrameDefault(), because this function is over
// the limit of PGO, and that limit cannot be configured.
// Define them as macros to make sure that they are always inlined by the
// preprocessor.

#undef Py_DECREF
#define Py_DECREF(arg) \
    do { \
        _Py_DECREF_STAT_INC(); \
        PyObject *op = _PyObject_CAST(arg); \
        if (--op->ob_refcnt == 0) { \
            destructor dealloc = Py_TYPE(op)->tp_dealloc; \
            (*dealloc)(op); \
        } \
    } while (0)

#undef Py_XDECREF
#define Py_XDECREF(arg) \
    do { \
        PyObject *xop = _PyObject_CAST(arg); \
        if (xop != NULL) { \
            Py_DECREF(xop); \
        } \
    } while (0)

#undef Py_IS_TYPE
#define Py_IS_TYPE(ob, type) \
    (_PyObject_CAST(ob)->ob_type == (type))

#undef _Py_DECREF_SPECIALIZED
#define _Py_DECREF_SPECIALIZED(arg, dealloc) \
    do { \
        _Py_DECREF_STAT_INC(); \
        PyObject *op = _PyObject_CAST(arg); \
        if (--op->ob_refcnt == 0) { \
            destructor d = (destructor)(dealloc); \
            d(op); \
        } \
    } while (0)
#endif

// GH-89279: Similar to above, force inlining by using a macro.
#if defined(_MSC_VER) && SIZEOF_INT == 4
#define _Py_atomic_load_relaxed_int32(ATOMIC_VAL) (assert(sizeof((ATOMIC_VAL)->_value) == 4), *((volatile int*)&((ATOMIC_VAL)->_value)))
#else
#define _Py_atomic_load_relaxed_int32(ATOMIC_VAL) _Py_atomic_load_relaxed(ATOMIC_VAL)
#endif


/* Forward declarations */
static PyObject *trace_call_function(
    PyThreadState *tstate, PyObject *callable, PyObject **stack,
    Py_ssize_t oparg, PyObject *kwnames);
static PyObject * do_call_core(
    PyThreadState *tstate, PyObject *func,
    PyObject *callargs, PyObject *kwdict, int use_tracing);

#ifdef LLTRACE
static void
dump_stack(_PyInterpreterFrame *frame, PyObject **stack_pointer)
{
    PyObject **stack_base = _PyFrame_Stackbase(frame);
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    printf("    stack=[");
    for (PyObject **ptr = stack_base; ptr < stack_pointer; ptr++) {
        if (ptr != stack_base) {
            printf(", ");
        }
        if (PyObject_Print(*ptr, stdout, 0) != 0) {
            PyErr_Clear();
            printf("<%s object at %p>",
                   Py_TYPE(*ptr)->tp_name, (void *)(*ptr));
        }
    }
    printf("]\n");
    fflush(stdout);
    PyErr_Restore(type, value, traceback);
}

static void
lltrace_instruction(_PyInterpreterFrame *frame,
                    PyObject **stack_pointer,
                    _Py_CODEUNIT *next_instr)
{
    dump_stack(frame, stack_pointer);
    int oparg = _Py_OPARG(*next_instr);
    int opcode = _Py_OPCODE(*next_instr);
    const char *opname = _PyOpcode_OpName[opcode];
    assert(opname != NULL);
    int offset = (int)(next_instr - _PyCode_CODE(frame->f_code));
    if (HAS_ARG(opcode)) {
        printf("%d: %s %d\n", offset * 2, opname, oparg);
    }
    else {
        printf("%d: %s\n", offset * 2, opname);
    }
    fflush(stdout);
}
static void
lltrace_resume_frame(_PyInterpreterFrame *frame)
{
    PyFunctionObject *f = frame->f_func;
    if (f == NULL) {
        printf("\nResuming frame.");
        return;
    }
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyObject *name = f->func_qualname;
    if (name == NULL) {
        name = f->func_name;
    }
    printf("\nResuming frame");
    if (name) {
        printf(" for ");
        if (PyObject_Print(name, stdout, 0) < 0) {
            PyErr_Clear();
        }
    }
    if (f->func_module) {
        printf(" in module ");
        if (PyObject_Print(f->func_module, stdout, 0) < 0) {
            PyErr_Clear();
        }
    }
    printf("\n");
    fflush(stdout);
    PyErr_Restore(type, value, traceback);
}
#endif
static int call_trace(Py_tracefunc, PyObject *,
                      PyThreadState *, _PyInterpreterFrame *,
                      int, PyObject *);
static int call_trace_protected(Py_tracefunc, PyObject *,
                                PyThreadState *, _PyInterpreterFrame *,
                                int, PyObject *);
static void call_exc_trace(Py_tracefunc, PyObject *,
                           PyThreadState *, _PyInterpreterFrame *);
static int maybe_call_line_trace(Py_tracefunc, PyObject *,
                                 PyThreadState *, _PyInterpreterFrame *, int);
static void maybe_dtrace_line(_PyInterpreterFrame *, PyTraceInfo *, int);
static void dtrace_function_entry(_PyInterpreterFrame *);
static void dtrace_function_return(_PyInterpreterFrame *);

static PyObject * import_name(PyThreadState *, _PyInterpreterFrame *,
                              PyObject *, PyObject *, PyObject *);
static PyObject * import_from(PyThreadState *, PyObject *, PyObject *);
static int import_all_from(PyThreadState *, PyObject *, PyObject *);
static void format_exc_check_arg(PyThreadState *, PyObject *, const char *, PyObject *);
static void format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg);
static int check_args_iterable(PyThreadState *, PyObject *func, PyObject *vararg);
static int check_except_type_valid(PyThreadState *tstate, PyObject* right);
static int check_except_star_type_valid(PyThreadState *tstate, PyObject* right);
static void format_kwargs_error(PyThreadState *, PyObject *func, PyObject *kwargs);
static void format_awaitable_error(PyThreadState *, PyTypeObject *, int);
static int get_exception_handler(PyCodeObject *, int, int*, int*, int*);
static _PyInterpreterFrame *
_PyEvalFramePushAndInit(PyThreadState *tstate, PyFunctionObject *func,
                        PyObject *locals, PyObject* const* args,
                        size_t argcount, PyObject *kwnames);
static void
_PyEvalFrameClearAndPop(PyThreadState *tstate, _PyInterpreterFrame *frame);

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG \
    "cannot access local variable '%s' where it is not associated with a value"
#define UNBOUNDFREE_ERROR_MSG \
    "cannot access free variable '%s' where it is not associated with a" \
    " value in enclosing scope"

#ifndef NDEBUG
/* Ensure that tstate is valid: sanity check for PyEval_AcquireThread() and
   PyEval_RestoreThread(). Detect if tstate memory was freed. It can happen
   when a thread continues to run after Python finalization, especially
   daemon threads. */
static int
is_tstate_valid(PyThreadState *tstate)
{
    assert(!_PyMem_IsPtrFreed(tstate));
    assert(!_PyMem_IsPtrFreed(tstate->interp));
    return 1;
}
#endif


#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

int
Py_GetRecursionLimit(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return interp->ceval.recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    interp->ceval.recursion_limit = new_limit;
    for (PyThreadState *p = interp->threads.head; p != NULL; p = p->next) {
        int depth = p->recursion_limit - p->recursion_remaining;
        p->recursion_limit = new_limit;
        p->recursion_remaining = new_limit - depth;
    }
}

/* The function _Py_EnterRecursiveCallTstate() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches recursion_limit. */
int
_Py_CheckRecursiveCall(PyThreadState *tstate, const char *where)
{
    /* Check against global limit first. */
    int depth = tstate->recursion_limit - tstate->recursion_remaining;
    if (depth < tstate->interp->ceval.recursion_limit) {
        tstate->recursion_limit = tstate->interp->ceval.recursion_limit;
        tstate->recursion_remaining = tstate->recursion_limit - depth;
        assert(tstate->recursion_remaining > 0);
        return 0;
    }
#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        ++tstate->recursion_remaining;
        _PyErr_SetString(tstate, PyExc_MemoryError, "Stack overflow");
        return -1;
    }
#endif
    if (tstate->recursion_headroom) {
        if (tstate->recursion_remaining < -50) {
            /* Overflowing while handling an overflow. Give up. */
            Py_FatalError("Cannot recover from stack overflow.");
        }
    }
    else {
        if (tstate->recursion_remaining <= 0) {
            tstate->recursion_headroom++;
            _PyErr_Format(tstate, PyExc_RecursionError,
                        "maximum recursion depth exceeded%s",
                        where);
            tstate->recursion_headroom--;
            ++tstate->recursion_remaining;
            return -1;
        }
    }
    return 0;
}


static const binaryfunc binary_ops[] = {
    [NB_ADD] = PyNumber_Add,
    [NB_AND] = PyNumber_And,
    [NB_FLOOR_DIVIDE] = PyNumber_FloorDivide,
    [NB_LSHIFT] = PyNumber_Lshift,
    [NB_MATRIX_MULTIPLY] = PyNumber_MatrixMultiply,
    [NB_MULTIPLY] = PyNumber_Multiply,
    [NB_REMAINDER] = PyNumber_Remainder,
    [NB_OR] = PyNumber_Or,
    [NB_POWER] = _PyNumber_PowerNoMod,
    [NB_RSHIFT] = PyNumber_Rshift,
    [NB_SUBTRACT] = PyNumber_Subtract,
    [NB_TRUE_DIVIDE] = PyNumber_TrueDivide,
    [NB_XOR] = PyNumber_Xor,
    [NB_INPLACE_ADD] = PyNumber_InPlaceAdd,
    [NB_INPLACE_AND] = PyNumber_InPlaceAnd,
    [NB_INPLACE_FLOOR_DIVIDE] = PyNumber_InPlaceFloorDivide,
    [NB_INPLACE_LSHIFT] = PyNumber_InPlaceLshift,
    [NB_INPLACE_MATRIX_MULTIPLY] = PyNumber_InPlaceMatrixMultiply,
    [NB_INPLACE_MULTIPLY] = PyNumber_InPlaceMultiply,
    [NB_INPLACE_REMAINDER] = PyNumber_InPlaceRemainder,
    [NB_INPLACE_OR] = PyNumber_InPlaceOr,
    [NB_INPLACE_POWER] = _PyNumber_InPlacePowerNoMod,
    [NB_INPLACE_RSHIFT] = PyNumber_InPlaceRshift,
    [NB_INPLACE_SUBTRACT] = PyNumber_InPlaceSubtract,
    [NB_INPLACE_TRUE_DIVIDE] = PyNumber_InPlaceTrueDivide,
    [NB_INPLACE_XOR] = PyNumber_InPlaceXor,
};


// PEP 634: Structural Pattern Matching


// Return a tuple of values corresponding to keys, with error checks for
// duplicate/missing keys.
static PyObject*
match_keys(PyThreadState *tstate, PyObject *map, PyObject *keys)
{
    assert(PyTuple_CheckExact(keys));
    Py_ssize_t nkeys = PyTuple_GET_SIZE(keys);
    if (!nkeys) {
        // No keys means no items.
        return PyTuple_New(0);
    }
    PyObject *seen = NULL;
    PyObject *dummy = NULL;
    PyObject *values = NULL;
    PyObject *get = NULL;
    // We use the two argument form of map.get(key, default) for two reasons:
    // - Atomically check for a key and get its value without error handling.
    // - Don't cause key creation or resizing in dict subclasses like
    //   collections.defaultdict that define __missing__ (or similar).
    int meth_found = _PyObject_GetMethod(map, &_Py_ID(get), &get);
    if (get == NULL) {
        goto fail;
    }
    seen = PySet_New(NULL);
    if (seen == NULL) {
        goto fail;
    }
    // dummy = object()
    dummy = _PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
    if (dummy == NULL) {
        goto fail;
    }
    values = PyTuple_New(nkeys);
    if (values == NULL) {
        goto fail;
    }
    for (Py_ssize_t i = 0; i < nkeys; i++) {
        PyObject *key = PyTuple_GET_ITEM(keys, i);
        if (PySet_Contains(seen, key) || PySet_Add(seen, key)) {
            if (!_PyErr_Occurred(tstate)) {
                // Seen it before!
                _PyErr_Format(tstate, PyExc_ValueError,
                              "mapping pattern checks duplicate key (%R)", key);
            }
            goto fail;
        }
        PyObject *args[] = { map, key, dummy };
        PyObject *value = NULL;
        if (meth_found) {
            value = PyObject_Vectorcall(get, args, 3, NULL);
        }
        else {
            value = PyObject_Vectorcall(get, &args[1], 2, NULL);
        }
        if (value == NULL) {
            goto fail;
        }
        if (value == dummy) {
            // key not in map!
            Py_DECREF(value);
            Py_DECREF(values);
            // Return None:
            Py_INCREF(Py_None);
            values = Py_None;
            goto done;
        }
        PyTuple_SET_ITEM(values, i, value);
    }
    // Success:
done:
    Py_DECREF(get);
    Py_DECREF(seen);
    Py_DECREF(dummy);
    return values;
fail:
    Py_XDECREF(get);
    Py_XDECREF(seen);
    Py_XDECREF(dummy);
    Py_XDECREF(values);
    return NULL;
}

// Extract a named attribute from the subject, with additional bookkeeping to
// raise TypeErrors for repeated lookups. On failure, return NULL (with no
// error set). Use _PyErr_Occurred(tstate) to disambiguate.
static PyObject*
match_class_attr(PyThreadState *tstate, PyObject *subject, PyObject *type,
                 PyObject *name, PyObject *seen)
{
    assert(PyUnicode_CheckExact(name));
    assert(PySet_CheckExact(seen));
    if (PySet_Contains(seen, name) || PySet_Add(seen, name)) {
        if (!_PyErr_Occurred(tstate)) {
            // Seen it before!
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%s() got multiple sub-patterns for attribute %R",
                          ((PyTypeObject*)type)->tp_name, name);
        }
        return NULL;
    }
    PyObject *attr = PyObject_GetAttr(subject, name);
    if (attr == NULL && _PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Clear(tstate);
    }
    return attr;
}

// On success (match), return a tuple of extracted attributes. On failure (no
// match), return NULL. Use _PyErr_Occurred(tstate) to disambiguate.
static PyObject*
match_class(PyThreadState *tstate, PyObject *subject, PyObject *type,
            Py_ssize_t nargs, PyObject *kwargs)
{
    if (!PyType_Check(type)) {
        const char *e = "called match pattern must be a type";
        _PyErr_Format(tstate, PyExc_TypeError, e);
        return NULL;
    }
    assert(PyTuple_CheckExact(kwargs));
    // First, an isinstance check:
    if (PyObject_IsInstance(subject, type) <= 0) {
        return NULL;
    }
    // So far so good:
    PyObject *seen = PySet_New(NULL);
    if (seen == NULL) {
        return NULL;
    }
    PyObject *attrs = PyList_New(0);
    if (attrs == NULL) {
        Py_DECREF(seen);
        return NULL;
    }
    // NOTE: From this point on, goto fail on failure:
    PyObject *match_args = NULL;
    // First, the positional subpatterns:
    if (nargs) {
        int match_self = 0;
        match_args = PyObject_GetAttrString(type, "__match_args__");
        if (match_args) {
            if (!PyTuple_CheckExact(match_args)) {
                const char *e = "%s.__match_args__ must be a tuple (got %s)";
                _PyErr_Format(tstate, PyExc_TypeError, e,
                              ((PyTypeObject *)type)->tp_name,
                              Py_TYPE(match_args)->tp_name);
                goto fail;
            }
        }
        else if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
            _PyErr_Clear(tstate);
            // _Py_TPFLAGS_MATCH_SELF is only acknowledged if the type does not
            // define __match_args__. This is natural behavior for subclasses:
            // it's as if __match_args__ is some "magic" value that is lost as
            // soon as they redefine it.
            match_args = PyTuple_New(0);
            match_self = PyType_HasFeature((PyTypeObject*)type,
                                            _Py_TPFLAGS_MATCH_SELF);
        }
        else {
            goto fail;
        }
        assert(PyTuple_CheckExact(match_args));
        Py_ssize_t allowed = match_self ? 1 : PyTuple_GET_SIZE(match_args);
        if (allowed < nargs) {
            const char *plural = (allowed == 1) ? "" : "s";
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%s() accepts %d positional sub-pattern%s (%d given)",
                          ((PyTypeObject*)type)->tp_name,
                          allowed, plural, nargs);
            goto fail;
        }
        if (match_self) {
            // Easy. Copy the subject itself, and move on to kwargs.
            PyList_Append(attrs, subject);
        }
        else {
            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject *name = PyTuple_GET_ITEM(match_args, i);
                if (!PyUnicode_CheckExact(name)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "__match_args__ elements must be strings "
                                  "(got %s)", Py_TYPE(name)->tp_name);
                    goto fail;
                }
                PyObject *attr = match_class_attr(tstate, subject, type, name,
                                                  seen);
                if (attr == NULL) {
                    goto fail;
                }
                PyList_Append(attrs, attr);
                Py_DECREF(attr);
            }
        }
        Py_CLEAR(match_args);
    }
    // Finally, the keyword subpatterns:
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwargs); i++) {
        PyObject *name = PyTuple_GET_ITEM(kwargs, i);
        PyObject *attr = match_class_attr(tstate, subject, type, name, seen);
        if (attr == NULL) {
            goto fail;
        }
        PyList_Append(attrs, attr);
        Py_DECREF(attr);
    }
    Py_SETREF(attrs, PyList_AsTuple(attrs));
    Py_DECREF(seen);
    return attrs;
fail:
    // We really don't care whether an error was raised or not... that's our
    // caller's problem. All we know is that the match failed.
    Py_XDECREF(match_args);
    Py_DECREF(seen);
    Py_DECREF(attrs);
    return NULL;
}


static int do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);
static int exception_group_match(
    PyObject* exc_value, PyObject *match_type,
    PyObject **match, PyObject **rest);

static int unpack_iterable(PyThreadState *, PyObject *, int, int, PyObject **);

PyObject *
PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (locals == NULL) {
        locals = globals;
    }
    PyObject *builtins = _PyEval_BuiltinsFromGlobals(tstate, globals); // borrowed ref
    if (builtins == NULL) {
        return NULL;
    }
    PyFrameConstructor desc = {
        .fc_globals = globals,
        .fc_builtins = builtins,
        .fc_name = ((PyCodeObject *)co)->co_name,
        .fc_qualname = ((PyCodeObject *)co)->co_name,
        .fc_code = co,
        .fc_defaults = NULL,
        .fc_kwdefaults = NULL,
        .fc_closure = NULL
    };
    PyFunctionObject *func = _PyFunction_FromConstructor(&desc);
    if (func == NULL) {
        return NULL;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_LEGACY);
    PyObject *res = _PyEval_Vector(tstate, func, locals, NULL, 0, NULL);
    Py_DECREF(func);
    return res;
}


/* Interpreter main loop */

PyObject *
PyEval_EvalFrame(PyFrameObject *f)
{
    /* Function kept for backward compatibility */
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f->f_frame, 0);
}

PyObject *
PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f->f_frame, throwflag);
}


/* Computed GOTOs, or
       the-optimization-commonly-but-improperly-known-as-"threaded code"
   using gcc's labels-as-values extension
   (http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html).

   The traditional bytecode evaluation loop uses a "switch" statement, which
   decent compilers will optimize as a single indirect branch instruction
   combined with a lookup table of jump addresses. However, since the
   indirect jump instruction is shared by all opcodes, the CPU will have a
   hard time making the right prediction for where to jump next (actually,
   it will be always wrong except in the uncommon case of a sequence of
   several identical opcodes).

   "Threaded code" in contrast, uses an explicit jump table and an explicit
   indirect jump instruction at the end of each opcode. Since the jump
   instruction is at a different address for each opcode, the CPU will make a
   separate prediction for each of these instructions, which is equivalent to
   predicting the second opcode of each opcode pair. These predictions have
   a much better chance to turn out valid, especially in small bytecode loops.

   A mispredicted branch on a modern CPU flushes the whole pipeline and
   can cost several CPU cycles (depending on the pipeline depth),
   and potentially many more instructions (depending on the pipeline width).
   A correctly predicted branch, however, is nearly free.

   At the time of this writing, the "threaded code" version is up to 15-20%
   faster than the normal "switch" version, depending on the compiler and the
   CPU architecture.

   NOTE: care must be taken that the compiler doesn't try to "optimize" the
   indirect jumps by sharing them between all opcodes. Such optimizations
   can be disabled on gcc by using the -fno-gcse flag (or possibly
   -fno-crossjumping).
*/

/* Use macros rather than inline functions, to make it as clear as possible
 * to the C compiler that the tracing check is a simple test then branch.
 * We want to be sure that the compiler knows this before it generates
 * the CFG.
 */

#ifdef WITH_DTRACE
#define OR_DTRACE_LINE | (PyDTrace_LINE_ENABLED() ? 255 : 0)
#else
#define OR_DTRACE_LINE
#endif

#ifdef HAVE_COMPUTED_GOTOS
    #ifndef USE_COMPUTED_GOTOS
    #define USE_COMPUTED_GOTOS 1
    #endif
#else
    #if defined(USE_COMPUTED_GOTOS) && USE_COMPUTED_GOTOS
    #error "Computed gotos are not supported on this compiler."
    #endif
    #undef USE_COMPUTED_GOTOS
    #define USE_COMPUTED_GOTOS 0
#endif

#ifdef Py_STATS
#define INSTRUCTION_START(op) \
    do { \
        frame->prev_instr = next_instr++; \
        OPCODE_EXE_INC(op); \
        if (_py_stats) _py_stats->opcode_stats[lastopcode].pair_count[op]++; \
        lastopcode = op; \
    } while (0)
#else
#define INSTRUCTION_START(op) (frame->prev_instr = next_instr++)
#endif

#if USE_COMPUTED_GOTOS
#define TARGET(op) TARGET_##op:
#define DISPATCH_GOTO() goto *opcode_targets[opcode]
#else
#define TARGET(op) case op:
#define DISPATCH_GOTO() goto dispatch_opcode
#endif

/* PRE_DISPATCH_GOTO() does lltrace if enabled. Normally a no-op */
#ifdef LLTRACE
#define PRE_DISPATCH_GOTO() if (lltrace) { \
    lltrace_instruction(frame, stack_pointer, next_instr); }
#else
#define PRE_DISPATCH_GOTO() ((void)0)
#endif

#define NOTRACE_DISPATCH() \
    { \
        NEXTOPARG(); \
        PRE_DISPATCH_GOTO(); \
        DISPATCH_GOTO(); \
    }

/* Do interpreter dispatch accounting for tracing and instrumentation */
#define DISPATCH() \
    { \
        NEXTOPARG(); \
        PRE_DISPATCH_GOTO(); \
        assert(cframe.use_tracing == 0 || cframe.use_tracing == 255); \
        opcode |= cframe.use_tracing OR_DTRACE_LINE; \
        DISPATCH_GOTO(); \
    }

#define NOTRACE_DISPATCH_SAME_OPARG() \
    { \
        opcode = _Py_OPCODE(*next_instr); \
        PRE_DISPATCH_GOTO(); \
        DISPATCH_GOTO(); \
    }

#define CHECK_EVAL_BREAKER() \
    _Py_CHECK_EMSCRIPTEN_SIGNALS_PERIODICALLY(); \
    if (_Py_atomic_load_relaxed_int32(eval_breaker)) { \
        goto handle_eval_breaker; \
    }


/* Tuple access macros */

#ifndef Py_DEBUG
#define GETITEM(v, i) PyTuple_GET_ITEM((PyTupleObject *)(v), (i))
#else
#define GETITEM(v, i) PyTuple_GetItem((v), (i))
#endif

/* Code access macros */

/* The integer overflow is checked by an assertion below. */
#define INSTR_OFFSET() ((int)(next_instr - first_instr))
#define NEXTOPARG()  do { \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word); \
        oparg = _Py_OPARG(word); \
    } while (0)
#define JUMPTO(x)       (next_instr = first_instr + (x))
#define JUMPBY(x)       (next_instr += (x))

/* Get opcode and oparg from original instructions, not quickened form. */
#define TRACING_NEXTOPARG() do { \
        NEXTOPARG(); \
        opcode = _PyOpcode_Deopt[opcode]; \
    } while (0)

/* OpCode prediction macros
    Some opcodes tend to come in pairs thus making it possible to
    predict the second code when the first is run.  For example,
    COMPARE_OP is often followed by POP_JUMP_IF_FALSE or POP_JUMP_IF_TRUE.

    Verifying the prediction costs a single high-speed test of a register
    variable against a constant.  If the pairing was good, then the
    processor's own internal branch predication has a high likelihood of
    success, resulting in a nearly zero-overhead transition to the
    next opcode.  A successful prediction saves a trip through the eval-loop
    including its unpredictable switch-case branch.  Combined with the
    processor's internal branch prediction, a successful PREDICT has the
    effect of making the two opcodes run as if they were a single new opcode
    with the bodies combined.

    If collecting opcode statistics, your choices are to either keep the
    predictions turned-on and interpret the results as if some opcodes
    had been combined or turn-off predictions so that the opcode frequency
    counter updates for both opcodes.

    Opcode prediction is disabled with threaded code, since the latter allows
    the CPU to record separate branch prediction information for each
    opcode.

*/

#define PREDICT_ID(op)          PRED_##op

#if USE_COMPUTED_GOTOS
#define PREDICT(op)             if (0) goto PREDICT_ID(op)
#else
#define PREDICT(op) \
    do { \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word) | cframe.use_tracing OR_DTRACE_LINE; \
        if (opcode == op) { \
            oparg = _Py_OPARG(word); \
            INSTRUCTION_START(op); \
            goto PREDICT_ID(op); \
        } \
    } while(0)
#endif
#define PREDICTED(op)           PREDICT_ID(op):


/* Stack manipulation macros */

/* The stack can grow at most MAXINT deep, as co_nlocals and
   co_stacksize are ints. */
#define STACK_LEVEL()     ((int)(stack_pointer - _PyFrame_Stackbase(frame)))
#define STACK_SIZE()      (frame->f_code->co_stacksize)
#define EMPTY()           (STACK_LEVEL() == 0)
#define TOP()             (stack_pointer[-1])
#define SECOND()          (stack_pointer[-2])
#define THIRD()           (stack_pointer[-3])
#define FOURTH()          (stack_pointer[-4])
#define PEEK(n)           (stack_pointer[-(n)])
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define SET_SECOND(v)     (stack_pointer[-2] = (v))
#define BASIC_STACKADJ(n) (stack_pointer += n)
#define BASIC_PUSH(v)     (*stack_pointer++ = (v))
#define BASIC_POP()       (*--stack_pointer)

#ifdef Py_DEBUG
#define PUSH(v)         do { \
                            BASIC_PUSH(v); \
                            assert(STACK_LEVEL() <= STACK_SIZE()); \
                        } while (0)
#define POP()           (assert(STACK_LEVEL() > 0), BASIC_POP())
#define STACK_GROW(n)   do { \
                            assert(n >= 0); \
                            BASIC_STACKADJ(n); \
                            assert(STACK_LEVEL() <= STACK_SIZE()); \
                        } while (0)
#define STACK_SHRINK(n) do { \
                            assert(n >= 0); \
                            assert(STACK_LEVEL() >= n); \
                            BASIC_STACKADJ(-(n)); \
                        } while (0)
#else
#define PUSH(v)                BASIC_PUSH(v)
#define POP()                  BASIC_POP()
#define STACK_GROW(n)          BASIC_STACKADJ(n)
#define STACK_SHRINK(n)        BASIC_STACKADJ(-(n))
#endif

/* Local variable macros */

#define GETLOCAL(i)     (frame->localsplus[i])

/* The SETLOCAL() macro must not DECREF the local variable in-place and
   then store the new value; it must copy the old value to a temporary
   value, then store the new value, and then DECREF the temporary value.
   This is because it is possible that during the DECREF the frame is
   accessed by other code (e.g. a __del__ method or gc.collect()) and the
   variable would be pointing to already-freed memory. */
#define SETLOCAL(i, value)      do { PyObject *tmp = GETLOCAL(i); \
                                     GETLOCAL(i) = value; \
                                     Py_XDECREF(tmp); } while (0)

#define JUMP_TO_INSTRUCTION(op) goto PREDICT_ID(op)


#define DEOPT_IF(cond, instname) if (cond) { goto miss; }


#define GLOBALS() frame->f_globals
#define BUILTINS() frame->f_builtins
#define LOCALS() frame->f_locals

/* Shared opcode macros */

#define TRACE_FUNCTION_EXIT() \
    if (cframe.use_tracing) { \
        if (trace_function_exit(tstate, frame, retval)) { \
            Py_DECREF(retval); \
            goto exit_unwind; \
        } \
    }

#define DTRACE_FUNCTION_EXIT() \
    if (PyDTrace_FUNCTION_RETURN_ENABLED()) { \
        dtrace_function_return(frame); \
    }

#define TRACE_FUNCTION_UNWIND()  \
    if (cframe.use_tracing) { \
        /* Since we are already unwinding, \
         * we don't care if this raises */ \
        trace_function_exit(tstate, frame, NULL); \
    }

#define TRACE_FUNCTION_ENTRY() \
    if (cframe.use_tracing) { \
        _PyFrame_SetStackPointer(frame, stack_pointer); \
        int err = trace_function_entry(tstate, frame); \
        stack_pointer = _PyFrame_GetStackPointer(frame); \
        if (err) { \
            goto error; \
        } \
    }

#define TRACE_FUNCTION_THROW_ENTRY() \
    if (cframe.use_tracing) { \
        assert(frame->stacktop >= 0); \
        if (trace_function_entry(tstate, frame)) { \
            goto exit_unwind; \
        } \
    }

#define DTRACE_FUNCTION_ENTRY()  \
    if (PyDTrace_FUNCTION_ENTRY_ENABLED()) { \
        dtrace_function_entry(frame); \
    }

#define ADAPTIVE_COUNTER_IS_ZERO(cache) \
    (cache)->counter < (1<<ADAPTIVE_BACKOFF_BITS)

#define DECREMENT_ADAPTIVE_COUNTER(cache) \
    (cache)->counter -= (1<<ADAPTIVE_BACKOFF_BITS)

static int
trace_function_entry(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    if (tstate->c_tracefunc != NULL) {
        /* tstate->c_tracefunc, if defined, is a
            function that will be called on *every* entry
            to a code block.  Its return value, if not
            None, is a function that will be called at
            the start of each executed line of code.
            (Actually, the function must return itself
            in order to continue tracing.)  The trace
            functions are called with three arguments:
            a pointer to the current frame, a string
            indicating why the function is called, and
            an argument which depends on the situation.
            The global trace function is also called
            whenever an exception is detected. */
        if (call_trace_protected(tstate->c_tracefunc,
                                    tstate->c_traceobj,
                                    tstate, frame,
                                    PyTrace_CALL, Py_None)) {
            /* Trace function raised an error */
            return -1;
        }
    }
    if (tstate->c_profilefunc != NULL) {
        /* Similar for c_profilefunc, except it needn't
            return itself and isn't called for "line" events */
        if (call_trace_protected(tstate->c_profilefunc,
                                    tstate->c_profileobj,
                                    tstate, frame,
                                    PyTrace_CALL, Py_None)) {
            /* Profile function raised an error */
            return -1;
        }
    }
    return 0;
}

static int
trace_function_exit(PyThreadState *tstate, _PyInterpreterFrame *frame, PyObject *retval)
{
    if (tstate->c_tracefunc) {
        if (call_trace_protected(tstate->c_tracefunc, tstate->c_traceobj,
                                    tstate, frame, PyTrace_RETURN, retval)) {
            return -1;
        }
    }
    if (tstate->c_profilefunc) {
        if (call_trace_protected(tstate->c_profilefunc, tstate->c_profileobj,
                                    tstate, frame, PyTrace_RETURN, retval)) {
            return -1;
        }
    }
    return 0;
}

static _PyInterpreterFrame *
pop_frame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    _PyInterpreterFrame *prev_frame = frame->previous;
    _PyEvalFrameClearAndPop(tstate, frame);
    return prev_frame;
}

/* It is only between the KW_NAMES instruction and the following CALL,
 * that this has any meaning.
 */
typedef struct {
    PyObject *kwnames;
} CallShape;

// GH-89279: Must be a macro to be sure it's inlined by MSVC.
#define is_method(stack_pointer, args) (PEEK((args)+2) != NULL)

#define KWNAMES_LEN() \
    (call_shape.kwnames == NULL ? 0 : ((int)PyTuple_GET_SIZE(call_shape.kwnames)))

PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrameDefault(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    _Py_EnsureTstateNotNULL(tstate);
    CALL_STAT_INC(pyeval_calls);

#if USE_COMPUTED_GOTOS
/* Import the static jump table */
#include "opcode_targets.h"
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
    // opcode is an 8-bit value to improve the code generated by MSVC
    // for the big switch below (in combination with the EXTRA_CASES macro).
    uint8_t opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    _Py_atomic_int * const eval_breaker = &tstate->interp->ceval.eval_breaker;
#ifdef LLTRACE
    int lltrace = 0;
#endif

    _PyCFrame cframe;
    CallShape call_shape;
    call_shape.kwnames = NULL; // Borrowed reference. Reset by CALL instructions.

    /* WARNING: Because the _PyCFrame lives on the C stack,
     * but can be accessed from a heap allocated object (tstate)
     * strict stack discipline must be maintained.
     */
    _PyCFrame *prev_cframe = tstate->cframe;
    cframe.use_tracing = prev_cframe->use_tracing;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;

    frame->is_entry = true;
    /* Push frame */
    frame->previous = prev_cframe->current_frame;
    cframe.current_frame = frame;

    /* support for generator.throw() */
    if (throwflag) {
        if (_Py_EnterRecursiveCallTstate(tstate, "")) {
            tstate->recursion_remaining--;
            goto exit_unwind;
        }
        TRACE_FUNCTION_THROW_ENTRY();
        DTRACE_FUNCTION_ENTRY();
        goto resume_with_error;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */

    PyObject *names;
    PyObject *consts;
    _Py_CODEUNIT *first_instr;
    _Py_CODEUNIT *next_instr;
    PyObject **stack_pointer;

/* Sets the above local variables from the frame */
#define SET_LOCALS_FROM_FRAME() \
    { \
        PyCodeObject *co = frame->f_code; \
        names = co->co_names; \
        consts = co->co_consts; \
        first_instr = _PyCode_CODE(co); \
    } \
    assert(_PyInterpreterFrame_LASTI(frame) >= -1); \
    /* Jump back to the last instruction executed... */ \
    next_instr = frame->prev_instr + 1; \
    stack_pointer = _PyFrame_GetStackPointer(frame); \
    /* Set stackdepth to -1. \
        Update when returning or calling trace function. \
        Having stackdepth <= 0 ensures that invalid \
        values are not visible to the cycle GC. \
        We choose -1 rather than 0 to assist debugging. \
        */ \
    frame->stacktop = -1;


start_frame:
    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        tstate->recursion_remaining--;
        goto exit_unwind;
    }

resume_frame:
    SET_LOCALS_FROM_FRAME();

#ifdef LLTRACE
    {
        int r = PyDict_Contains(GLOBALS(), &_Py_ID(__lltrace__));
        if (r < 0) {
            goto exit_unwind;
        }
        lltrace = r;
    }
    if (lltrace) {
        lltrace_resume_frame(frame);
    }
#endif

#ifdef Py_DEBUG
    /* _PyEval_EvalFrameDefault() must not be called with an exception set,
       because it can clear it (directly or indirectly) and so the
       caller loses its exception */
    assert(!_PyErr_Occurred(tstate));
#endif

    DISPATCH();

handle_eval_breaker:

    /* Do periodic things, like check for signals and async I/0.
     * We need to do reasonably frequently, but not too frequently.
     * All loops should include a check of the eval breaker.
     * We also check on return from any builtin function.
     */
    if (_Py_HandlePending(tstate) != 0) {
        goto error;
    }
    DISPATCH();

    {
    /* Start instructions */
#if USE_COMPUTED_GOTOS
    {
#else
    dispatch_opcode:
        switch (opcode) {
#endif

        /* BEWARE!
           It is essential that any operation that fails must goto error
           and that all operation that succeed call DISPATCH() ! */

        TARGET(NOP) {
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(RESUME) {
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            UOP_WARMUP();
            JUMP_TO_INSTRUCTION(RESUME_QUICK);  // TODO
        }

        TARGET(RESUME_QUICK) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(RESUME_QUICK);  // TODO
            assert(tstate->cframe == &cframe);
            assert(frame == cframe.current_frame);
            if (_Py_atomic_load_relaxed_int32(eval_breaker) && oparg < 2) {
                goto handle_eval_breaker;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_CLOSURE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* We keep LOAD_CLOSURE so that the bytecode stays more readable. */
            PyObject *value = GETLOCAL(oparg);
            if (value == NULL) {
                goto unbound_local_error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_FAST_CHECK) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = GETLOCAL(oparg);
            if (value == NULL) {
                goto unbound_local_error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_CONST) {
            PyObject *value;
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(LOAD_CONST);  // TODO
            UOP_LOAD_CONST(value, oparg);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            SETLOCAL(oparg, value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_FAST__LOAD_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_JUMP(1);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_FAST__LOAD_CONST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_JUMP(1);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            value = GETITEM(consts, oparg);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(STORE_FAST__LOAD_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            SETLOCAL(oparg, value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_JUMP(1);
            value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(STORE_FAST__STORE_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            SETLOCAL(oparg, value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_JUMP(1);
            value = POP();
            SETLOCAL(oparg, value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_CONST__LOAD_FAST) {  // TODO
            PyObject *value;
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            value = GETITEM(consts, oparg);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_JUMP(1);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            value = GETLOCAL(oparg);
            assert(value != NULL);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(POP_TOP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            Py_DECREF(value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(PUSH_NULL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* Use BASIC_PUSH as NULL is not a valid object pointer */
            BASIC_PUSH(NULL);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(UNARY_POSITIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();
            PyObject *res = PyNumber_Positive(value);
            Py_DECREF(value);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(UNARY_NEGATIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();
            PyObject *res = PyNumber_Negative(value);
            Py_DECREF(value);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(UNARY_NOT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();
            int err = PyObject_IsTrue(value);
            Py_DECREF(value);
            if (err == 0) {
                Py_INCREF(Py_True);
                UOP_STACK_SET(1, Py_True);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else if (err > 0) {
                Py_INCREF(Py_False);
                UOP_STACK_SET(1, Py_False);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            UOP_STACK_GROW(-1);
            goto error;
        }

        TARGET(UNARY_INVERT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();
            PyObject *res = PyNumber_Invert(value);
            Py_DECREF(value);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_MULTIPLY_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyLong_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            PyObject *prod = _PyLong_Multiply((PyLongObject *)left, (PyLongObject *)right);
            UOP_STACK_SET(2, prod);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            UOP_STACK_GROW(-1);
            if (prod == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_MULTIPLY_FLOAT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dprod = ((PyFloatObject *)left)->ob_fval *
                ((PyFloatObject *)right)->ob_fval;
            PyObject *prod = PyFloat_FromDouble(dprod);
            UOP_STACK_SET(2, prod);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            UOP_STACK_GROW(-1);
            if (prod == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_SUBTRACT_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyLong_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            PyObject *sub = _PyLong_Subtract((PyLongObject *)left, (PyLongObject *)right);
            UOP_STACK_SET(2, sub);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            UOP_STACK_GROW(-1);
            if (sub == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_SUBTRACT_FLOAT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dsub = ((PyFloatObject *)left)->ob_fval - ((PyFloatObject *)right)->ob_fval;
            PyObject *sub = PyFloat_FromDouble(dsub);
            UOP_STACK_SET(2, sub);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            UOP_STACK_GROW(-1);
            if (sub == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_ADD_UNICODE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyUnicode_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            PyObject *res = PyUnicode_Concat(left, right);
            UOP_STACK_GROW(-1);
            UOP_STACK_SET(1, res);
            _Py_DECREF_SPECIALIZED(left, _PyUnicode_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            if (TOP() == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_INPLACE_ADD_UNICODE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyUnicode_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            _Py_CODEUNIT true_next = next_instr[INLINE_CACHE_ENTRIES_BINARY_OP];
            assert(_Py_OPCODE(true_next) == STORE_FAST ||
                   _Py_OPCODE(true_next) == STORE_FAST__LOAD_FAST);
            PyObject **target_local = &GETLOCAL(_Py_OPARG(true_next));
            DEOPT_IF(*target_local != left, BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            /* Handle `left = left + right` or `left += right` for str.
             *
             * When possible, extend `left` in place rather than
             * allocating a new PyUnicodeObject. This attempts to avoid
             * quadratic behavior when one neglects to use str.join().
             *
             * If `left` has only two references remaining (one from
             * the stack, one in the locals), DECREFing `left` leaves
             * only the locals reference, so PyUnicode_Append knows
             * that the string is safe to mutate.
             */
            assert(Py_REFCNT(left) >= 2);
            _Py_DECREF_NO_DEALLOC(left);
            UOP_STACK_GROW(-2);
            PyUnicode_Append(target_local, right);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            if (*target_local == NULL) {
                goto error;
            }
            // The STORE_FAST is already done.
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP + 1);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_ADD_FLOAT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dsum = ((PyFloatObject *)left)->ob_fval +
                ((PyFloatObject *)right)->ob_fval;
            PyObject *sum = PyFloat_FromDouble(dsum);
            UOP_STACK_SET(2, sum);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            UOP_STACK_GROW(-1);
            if (sum == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_ADD_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *left = SECOND();
            PyObject *right = TOP();
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            PyObject *sum = _PyLong_Add((PyLongObject *)left, (PyLongObject *)right);
            UOP_STACK_SET(2, sum);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            UOP_STACK_GROW(-1);
            if (sum == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SUBSCR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(BINARY_SUBSCR);  // TODO
            PyObject *sub = POP();
            PyObject *container = TOP();
            PyObject *res = PyObject_GetItem(container, sub);
            Py_DECREF(container);
            Py_DECREF(sub);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SLICE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *stop = POP();
            PyObject *start = POP();
            PyObject *container = TOP();

            PyObject *slice = _PyBuildSlice_ConsumeRefs(start, stop);
            if (slice == NULL) {
                goto error;
            }
            PyObject *res = PyObject_GetItem(container, slice);
            Py_DECREF(slice);
            if (res == NULL) {
                goto error;
            }
            UOP_STACK_SET(1, res);
            Py_DECREF(container);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_SLICE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *stop = POP();
            PyObject *start = POP();
            PyObject *container = TOP();
            PyObject *v = SECOND();

            PyObject *slice = _PyBuildSlice_ConsumeRefs(start, stop);
            if (slice == NULL) {
                goto error;
            }
            int err = PyObject_SetItem(container, slice, v);
            Py_DECREF(slice);
            if (err) {
                goto error;
            }
            UOP_STACK_GROW(-2);
            Py_DECREF(v);
            Py_DECREF(container);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SUBSCR_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            _PyBinarySubscrCache *cache = (_PyBinarySubscrCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *sub = TOP();
                PyObject *container = SECOND();
                next_instr--;
                if (_Py_Specialize_BinarySubscr(container, sub, next_instr) < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(BINARY_SUBSCR, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(BINARY_SUBSCR);  // TODO
            }
        }

        TARGET(BINARY_SUBSCR_LIST_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *sub = TOP();
            PyObject *list = SECOND();
            DEOPT_IF(!PyLong_CheckExact(sub), BINARY_SUBSCR);
            DEOPT_IF(!PyList_CheckExact(list), BINARY_SUBSCR);

            // Deopt unless 0 <= sub < PyList_Size(list)
            Py_ssize_t signed_magnitude = Py_SIZE(sub);
            DEOPT_IF(((size_t)signed_magnitude) > 1, BINARY_SUBSCR);
            assert(((PyLongObject *)_PyLong_GetZero())->ob_digit[0] == 0);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            DEOPT_IF(index >= PyList_GET_SIZE(list), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            PyObject *res = PyList_GET_ITEM(list, index);
            assert(res != NULL);
            Py_INCREF(res);
            UOP_STACK_GROW(-1);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            UOP_STACK_SET(1, res);
            Py_DECREF(list);
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SUBSCR_TUPLE_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *sub = TOP();
            PyObject *tuple = SECOND();
            DEOPT_IF(!PyLong_CheckExact(sub), BINARY_SUBSCR);
            DEOPT_IF(!PyTuple_CheckExact(tuple), BINARY_SUBSCR);

            // Deopt unless 0 <= sub < PyTuple_Size(list)
            Py_ssize_t signed_magnitude = Py_SIZE(sub);
            DEOPT_IF(((size_t)signed_magnitude) > 1, BINARY_SUBSCR);
            assert(((PyLongObject *)_PyLong_GetZero())->ob_digit[0] == 0);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            DEOPT_IF(index >= PyTuple_GET_SIZE(tuple), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            PyObject *res = PyTuple_GET_ITEM(tuple, index);
            assert(res != NULL);
            Py_INCREF(res);
            UOP_STACK_GROW(-1);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            UOP_STACK_SET(1, res);
            Py_DECREF(tuple);
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SUBSCR_DICT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *dict = SECOND();
            DEOPT_IF(!PyDict_CheckExact(SECOND()), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            PyObject *sub = TOP();
            PyObject *res = PyDict_GetItemWithError(dict, sub);
            if (res == NULL) {
                goto binary_subscr_dict_error;
            }
            Py_INCREF(res);
            UOP_STACK_GROW(-1);
            Py_DECREF(sub);
            UOP_STACK_SET(1, res);
            Py_DECREF(dict);
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_SUBSCR_GETITEM) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *sub = TOP();
            PyObject *container = SECOND();
            _PyBinarySubscrCache *cache = (_PyBinarySubscrCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            PyTypeObject *tp = Py_TYPE(container);
            DEOPT_IF(tp->tp_version_tag != type_version, BINARY_SUBSCR);
            assert(tp->tp_flags & Py_TPFLAGS_HEAPTYPE);
            PyObject *cached = ((PyHeapTypeObject *)tp)->_spec_cache.getitem;
            assert(PyFunction_Check(cached));
            PyFunctionObject *getitem = (PyFunctionObject *)cached;
            DEOPT_IF(getitem->func_version != cache->func_version, BINARY_SUBSCR);
            PyCodeObject *code = (PyCodeObject *)getitem->func_code;
            assert(code->co_argcount == 2);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            Py_INCREF(getitem);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, getitem);
            CALL_STAT_INC(inlined_py_calls);
            UOP_STACK_GROW(-2);
            new_frame->localsplus[0] = container;
            new_frame->localsplus[1] = sub;
            for (int i = 2; i < code->co_nlocalsplus; i++) {
                new_frame->localsplus[i] = NULL;
            }
            _PyFrame_SetStackPointer(frame, stack_pointer);
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            frame->prev_instr = next_instr - 1;
            new_frame->previous = frame;
            frame = cframe.current_frame = new_frame;
            CALL_STAT_INC(inlined_py_calls);
            goto start_frame;
        }

        TARGET(LIST_APPEND) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *v = POP();
            PyObject *list = PEEK(oparg);
            if (_PyList_AppendTakeRef((PyListObject *)list, v) < 0)
                goto error;
            PREDICT(JUMP_BACKWARD_QUICK);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(SET_ADD) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *v = POP();
            PyObject *set = PEEK(oparg);
            int err;
            err = PySet_Add(set, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            PREDICT(JUMP_BACKWARD_QUICK);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_SUBSCR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(STORE_SUBSCR);  // TODO
            PyObject *sub = TOP();
            PyObject *container = SECOND();
            PyObject *v = THIRD();
            int err;
            UOP_STACK_GROW(-3);
            /* container[sub] = v */
            err = PyObject_SetItem(container, sub, v);
            Py_DECREF(v);
            Py_DECREF(container);
            Py_DECREF(sub);
            if (err != 0) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_SUBSCR_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            _PyStoreSubscrCache *cache = (_PyStoreSubscrCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *sub = TOP();
                PyObject *container = SECOND();
                next_instr--;
                if (_Py_Specialize_StoreSubscr(container, sub, next_instr) < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(STORE_SUBSCR, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(STORE_SUBSCR);  // TODO
            }
        }

        TARGET(STORE_SUBSCR_LIST_INT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *sub = TOP();
            PyObject *list = SECOND();
            PyObject *value = THIRD();
            DEOPT_IF(!PyLong_CheckExact(sub), STORE_SUBSCR);
            DEOPT_IF(!PyList_CheckExact(list), STORE_SUBSCR);

            // Ensure nonnegative, zero-or-one-digit ints.
            DEOPT_IF(((size_t)Py_SIZE(sub)) > 1, STORE_SUBSCR);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            // Ensure index < len(list)
            DEOPT_IF(index >= PyList_GET_SIZE(list), STORE_SUBSCR);
            STAT_INC(STORE_SUBSCR, hit);

            PyObject *old_value = PyList_GET_ITEM(list, index);
            PyList_SET_ITEM(list, index, value);
            UOP_STACK_GROW(-3);
            assert(old_value != NULL);
            Py_DECREF(old_value);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            Py_DECREF(list);
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(STORE_SUBSCR_DICT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *sub = TOP();
            PyObject *dict = SECOND();
            PyObject *value = THIRD();
            DEOPT_IF(!PyDict_CheckExact(dict), STORE_SUBSCR);
            UOP_STACK_GROW(-3);
            STAT_INC(STORE_SUBSCR, hit);
            int err = _PyDict_SetItem_Take2((PyDictObject *)dict, sub, value);
            Py_DECREF(dict);
            if (err != 0) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_SUBSCR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DELETE_SUBSCR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *sub = TOP();
            PyObject *container = SECOND();
            int err;
            UOP_STACK_GROW(-2);
            /* del container[sub] */
            err = PyObject_DelItem(container, sub);
            Py_DECREF(container);
            Py_DECREF(sub);
            if (err != 0)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(PRINT_EXPR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            PyObject *hook = _PySys_GetAttr(tstate, &_Py_ID(displayhook));
            PyObject *res;
            if (hook == NULL) {
                _PyErr_SetString(tstate, PyExc_RuntimeError,
                                 "lost sys.displayhook");
                Py_DECREF(value);
                goto error;
            }
            res = PyObject_CallOneArg(hook, value);
            Py_DECREF(value);
            if (res == NULL)
                goto error;
            Py_DECREF(res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(RAISE_VARARGS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cause = NULL, *exc = NULL;
            switch (oparg) {
            case 2:
                cause = POP(); /* cause */
                /* fall through */
            case 1:
                exc = POP(); /* exc */
                /* fall through */
            case 0:
                if (do_raise(tstate, exc, cause)) {
                    goto exception_unwind;
                }
                break;
            default:
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad RAISE_VARARGS oparg");
                break;
            }
            goto error;
        }

        TARGET(RETURN_VALUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *retval = POP();
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            TRACE_FUNCTION_EXIT();
            DTRACE_FUNCTION_EXIT();
            _Py_LeaveRecursiveCallTstate(tstate);
            if (!frame->is_entry) {
                frame = cframe.current_frame = pop_frame(tstate, frame);
                _PyFrame_StackPush(frame, retval);
                goto resume_frame;
            }
            /* Restore previous cframe and return. */
            tstate->cframe = cframe.previous;
            tstate->cframe->use_tracing = cframe.use_tracing;
            assert(tstate->cframe->current_frame == frame->previous);
            assert(!_PyErr_Occurred(tstate));
            return retval;
        }

        TARGET(GET_AITER) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            unaryfunc getter = NULL;
            PyObject *iter = NULL;
            PyObject *obj = TOP();
            PyTypeObject *type = Py_TYPE(obj);

            if (type->tp_as_async != NULL) {
                getter = type->tp_as_async->am_aiter;
            }

            if (getter != NULL) {
                iter = (*getter)(obj);
                Py_DECREF(obj);
                if (iter == NULL) {
                    UOP_STACK_SET(1, NULL);
                    goto error;
                }
            }
            else {
                UOP_STACK_SET(1, NULL);
                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' requires an object with "
                              "__aiter__ method, got %.100s",
                              type->tp_name);
                Py_DECREF(obj);
                goto error;
            }

            if (Py_TYPE(iter)->tp_as_async == NULL ||
                    Py_TYPE(iter)->tp_as_async->am_anext == NULL) {

                UOP_STACK_SET(1, NULL);
                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' received an object from __aiter__ "
                              "that does not implement __anext__: %.100s",
                              Py_TYPE(iter)->tp_name);
                Py_DECREF(iter);
                goto error;
            }

            UOP_STACK_SET(1, iter);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(GET_ANEXT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            unaryfunc getter = NULL;
            PyObject *next_iter = NULL;
            PyObject *awaitable = NULL;
            PyObject *aiter = TOP();
            PyTypeObject *type = Py_TYPE(aiter);

            if (PyAsyncGen_CheckExact(aiter)) {
                awaitable = type->tp_as_async->am_anext(aiter);
                if (awaitable == NULL) {
                    goto error;
                }
            } else {
                if (type->tp_as_async != NULL){
                    getter = type->tp_as_async->am_anext;
                }

                if (getter != NULL) {
                    next_iter = (*getter)(aiter);
                    if (next_iter == NULL) {
                        goto error;
                    }
                }
                else {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'async for' requires an iterator with "
                                  "__anext__ method, got %.100s",
                                  type->tp_name);
                    goto error;
                }

                awaitable = _PyCoro_GetAwaitableIter(next_iter);
                if (awaitable == NULL) {
                    _PyErr_FormatFromCause(
                        PyExc_TypeError,
                        "'async for' received an invalid object "
                        "from __anext__: %.100s",
                        Py_TYPE(next_iter)->tp_name);

                    Py_DECREF(next_iter);
                    goto error;
                } else {
                    Py_DECREF(next_iter);
                }
            }

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, awaitable);
            PREDICT(LOAD_CONST);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(GET_AWAITABLE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(GET_AWAITABLE);  // TODO
            PyObject *iterable = TOP();
            PyObject *iter = _PyCoro_GetAwaitableIter(iterable);

            if (iter == NULL) {
                format_awaitable_error(tstate, Py_TYPE(iterable), oparg);
            }

            Py_DECREF(iterable);

            if (iter != NULL && PyCoro_CheckExact(iter)) {
                PyObject *yf = _PyGen_yf((PyGenObject*)iter);
                if (yf != NULL) {
                    /* `iter` is a coroutine object that is being
                       awaited, `yf` is a pointer to the current awaitable
                       being awaited on. */
                    Py_DECREF(yf);
                    Py_CLEAR(iter);
                    _PyErr_SetString(tstate, PyExc_RuntimeError,
                                     "coroutine is being awaited already");
                    /* The code below jumps to `error` if `iter` is NULL. */
                }
            }

            UOP_STACK_SET(1, iter); /* Even if it's NULL */

            if (iter == NULL) {
                goto error;
            }

            PREDICT(LOAD_CONST);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(SEND) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(frame->is_entry);
            assert(STACK_LEVEL() >= 2);
            PyObject *v = POP();
            PyObject *receiver = TOP();
            PySendResult gen_status;
            PyObject *retval;
            if (tstate->c_tracefunc == NULL) {
                gen_status = PyIter_Send(receiver, v, &retval);
            } else {
                if (Py_IsNone(v) && PyIter_Check(receiver)) {
                    retval = Py_TYPE(receiver)->tp_iternext(receiver);
                }
                else {
                    retval = PyObject_CallMethodOneArg(receiver, &_Py_ID(send), v);
                }
                if (retval == NULL) {
                    if (tstate->c_tracefunc != NULL
                            && _PyErr_ExceptionMatches(tstate, PyExc_StopIteration))
                        call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, frame);
                    if (_PyGen_FetchStopIterationValue(&retval) == 0) {
                        gen_status = PYGEN_RETURN;
                    }
                    else {
                        gen_status = PYGEN_ERROR;
                    }
                }
                else {
                    gen_status = PYGEN_NEXT;
                }
            }
            Py_DECREF(v);
            if (gen_status == PYGEN_ERROR) {
                assert(retval == NULL);
                goto error;
            }
            if (gen_status == PYGEN_RETURN) {
                assert(retval != NULL);
                Py_DECREF(receiver);
                UOP_STACK_SET(1, retval);
                UOP_JUMP(oparg);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            assert(gen_status == PYGEN_NEXT);
            assert(retval != NULL);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, retval);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(ASYNC_GEN_WRAP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *v = TOP();
            assert(frame->f_code->co_flags & CO_ASYNC_GENERATOR);
            PyObject *w = _PyAsyncGenValueWrapperNew(v);
            if (w == NULL) {
                goto error;
            }
            UOP_STACK_SET(1, w);
            Py_DECREF(v);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(YIELD_VALUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            // NOTE: It's important that YIELD_VALUE never raises an exception!
            // The compiler treats any exception raised here as a failed close()
            // or throw() call.
            assert(oparg == STACK_LEVEL());
            assert(frame->is_entry);
            PyObject *retval = POP();
            _PyFrame_GetGenerator(frame)->gi_frame_state = FRAME_SUSPENDED;
            _PyFrame_SetStackPointer(frame, stack_pointer);
            TRACE_FUNCTION_EXIT();
            DTRACE_FUNCTION_EXIT();
            _Py_LeaveRecursiveCallTstate(tstate);
            /* Restore previous cframe and return. */
            tstate->cframe = cframe.previous;
            tstate->cframe->use_tracing = cframe.use_tracing;
            assert(tstate->cframe->current_frame == frame->previous);
            assert(!_PyErr_Occurred(tstate));
            return retval;
        }

        TARGET(POP_EXCEPT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            _PyErr_StackItem *exc_info = tstate->exc_info;
            PyObject *value = exc_info->exc_value;
            exc_info->exc_value = POP();
            Py_XDECREF(value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(RERAISE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            if (oparg) {
                PyObject *lasti = PEEK(oparg + 1);
                if (PyLong_Check(lasti)) {
                    frame->prev_instr = first_instr + PyLong_AsLong(lasti);
                    assert(!_PyErr_Occurred(tstate));
                }
                else {
                    assert(PyLong_Check(lasti));
                    _PyErr_SetString(tstate, PyExc_SystemError, "lasti is not an int");
                    goto error;
                }
            }
            PyObject *val = POP();
            assert(val && PyExceptionInstance_Check(val));
            PyObject *exc = Py_NewRef(PyExceptionInstance_Class(val));
            PyObject *tb = PyException_GetTraceback(val);
            _PyErr_Restore(tstate, exc, val, tb);
            goto exception_unwind;
        }

        TARGET(PREP_RERAISE_STAR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *excs = POP();
            assert(PyList_Check(excs));
            PyObject *orig = POP();

            PyObject *val = _PyExc_PrepReraiseStar(orig, excs);
            Py_DECREF(excs);
            Py_DECREF(orig);

            if (val == NULL) {
                goto error;
            }

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, val);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(END_ASYNC_FOR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *val = POP();
            assert(val && PyExceptionInstance_Check(val));
            if (PyErr_GivenExceptionMatches(val, PyExc_StopAsyncIteration)) {
                Py_DECREF(val);
                Py_DECREF(POP());
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                PyObject *exc = Py_NewRef(PyExceptionInstance_Class(val));
                PyObject *tb = PyException_GetTraceback(val);
                _PyErr_Restore(tstate, exc, val, tb);
                goto exception_unwind;
            }
        }

        TARGET(CLEANUP_THROW) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(throwflag);
            PyObject *exc_value = TOP();
            assert(exc_value && PyExceptionInstance_Check(exc_value));
            if (PyErr_GivenExceptionMatches(exc_value, PyExc_StopIteration)) {
                PyObject *value = ((PyStopIterationObject *)exc_value)->value;
                Py_INCREF(value);
                Py_DECREF(POP());  // The StopIteration.
                Py_DECREF(POP());  // The last sent value.
                Py_DECREF(POP());  // The delegated sub-iterator.
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, value);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            Py_INCREF(exc_value);
            PyObject *exc_type = Py_NewRef(Py_TYPE(exc_value));
            PyObject *exc_traceback = PyException_GetTraceback(exc_value);
            _PyErr_Restore(tstate, exc_type, exc_value, exc_traceback);
            goto exception_unwind;
        }

        TARGET(LOAD_ASSERTION_ERROR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = PyExc_AssertionError;
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_BUILD_CLASS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *bc;
            if (PyDict_CheckExact(BUILTINS())) {
                bc = _PyDict_GetItemWithError(BUILTINS(),
                                              &_Py_ID(__build_class__));
                if (bc == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    }
                    goto error;
                }
                Py_INCREF(bc);
            }
            else {
                bc = PyObject_GetItem(BUILTINS(), &_Py_ID(__build_class__));
                if (bc == NULL) {
                    if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    goto error;
                }
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, bc);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_NAME) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *v = POP();
            PyObject *ns = LOCALS();
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when storing %R", name);
                Py_DECREF(v);
                goto error;
            }
            if (PyDict_CheckExact(ns))
                err = PyDict_SetItem(ns, name, v);
            else
                err = PyObject_SetItem(ns, name, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DELETE_NAME) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *ns = LOCALS();
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when deleting %R", name);
                goto error;
            }
            err = PyObject_DelItem(ns, name);
            if (err != 0) {
                format_exc_check_arg(tstate, PyExc_NameError,
                                     NAME_ERROR_MSG,
                                     name);
                goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(UNPACK_SEQUENCE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(UNPACK_SEQUENCE);  // TODO
            PyObject *seq = POP();
            PyObject **top = stack_pointer + oparg;
            if (!unpack_iterable(tstate, seq, oparg, -1, top)) {
                Py_DECREF(seq);
                goto error;
            }
            UOP_STACK_GROW(oparg);
            Py_DECREF(seq);
            UOP_JUMP(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(UNPACK_SEQUENCE_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyUnpackSequenceCache *cache = (_PyUnpackSequenceCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *seq = TOP();
                next_instr--;
                _Py_Specialize_UnpackSequence(seq, next_instr, oparg);
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(UNPACK_SEQUENCE, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(UNPACK_SEQUENCE);  // TODO
            }
        }

        TARGET(UNPACK_SEQUENCE_TWO_TUPLE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *seq = TOP();
            DEOPT_IF(!PyTuple_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyTuple_GET_SIZE(seq) != 2, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            UOP_STACK_SET(1, Py_NewRef(PyTuple_GET_ITEM(seq, 1)));
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, Py_NewRef(PyTuple_GET_ITEM(seq, 0)));
            Py_DECREF(seq);
            UOP_JUMP(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(UNPACK_SEQUENCE_TUPLE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *seq = TOP();
            DEOPT_IF(!PyTuple_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyTuple_GET_SIZE(seq) != oparg, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            UOP_STACK_GROW(-1);
            PyObject **items = _PyTuple_ITEMS(seq);
            while (oparg--) {
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, Py_NewRef(items[oparg]));
            }
            Py_DECREF(seq);
            UOP_JUMP(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(UNPACK_SEQUENCE_LIST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *seq = TOP();
            DEOPT_IF(!PyList_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyList_GET_SIZE(seq) != oparg, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            UOP_STACK_GROW(-1);
            PyObject **items = _PyList_ITEMS(seq);
            while (oparg--) {
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, Py_NewRef(items[oparg]));
            }
            Py_DECREF(seq);
            UOP_JUMP(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(UNPACK_EX) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            int totalargs = 1 + (oparg & 0xFF) + (oparg >> 8);
            PyObject *seq = POP();
            PyObject **top = stack_pointer + totalargs;
            if (!unpack_iterable(tstate, seq, oparg & 0xFF, oparg >> 8, top)) {
                Py_DECREF(seq);
                goto error;
            }
            UOP_STACK_GROW(totalargs);
            Py_DECREF(seq);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_ATTR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(STORE_ATTR);  // TODO
            PyObject *name = GETITEM(names, oparg);
            PyObject *owner = TOP();
            PyObject *v = SECOND();
            int err;
            UOP_STACK_GROW(-2);
            err = PyObject_SetAttr(owner, name, v);
            Py_DECREF(v);
            Py_DECREF(owner);
            if (err != 0) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DELETE_ATTR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *owner = POP();
            int err;
            err = PyObject_SetAttr(owner, name, (PyObject *)NULL);
            Py_DECREF(owner);
            if (err != 0)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_GLOBAL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *v = POP();
            int err;
            err = PyDict_SetItem(GLOBALS(), name, v);
            Py_DECREF(v);
            if (err != 0)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DELETE_GLOBAL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            int err;
            err = PyDict_DelItem(GLOBALS(), name);
            if (err != 0) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                    format_exc_check_arg(tstate, PyExc_NameError,
                                         NAME_ERROR_MSG, name);
                }
                goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_NAME) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *locals = LOCALS();
            PyObject *v;
            if (locals == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when loading %R", name);
                goto error;
            }
            if (PyDict_CheckExact(locals)) {
                v = PyDict_GetItemWithError(locals, name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                v = PyObject_GetItem(locals, name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        goto error;
                    _PyErr_Clear(tstate);
                }
            }
            if (v == NULL) {
                v = PyDict_GetItemWithError(GLOBALS(), name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
                else {
                    if (PyDict_CheckExact(BUILTINS())) {
                        v = PyDict_GetItemWithError(BUILTINS(), name);
                        if (v == NULL) {
                            if (!_PyErr_Occurred(tstate)) {
                                format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                        Py_INCREF(v);
                    }
                    else {
                        v = PyObject_GetItem(BUILTINS(), name);
                        if (v == NULL) {
                            if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                                format_exc_check_arg(
                                            tstate, PyExc_NameError,
                                            NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                    }
                }
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, v);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_GLOBAL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(LOAD_GLOBAL);  // TODO
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            PyObject *name = GETITEM(names, oparg>>1);
            PyObject *v;
            if (PyDict_CheckExact(GLOBALS())
                && PyDict_CheckExact(BUILTINS()))
            {
                v = _PyDict_LoadGlobal((PyDictObject *)GLOBALS(),
                                       (PyDictObject *)BUILTINS(),
                                       name);
                if (v == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        /* _PyDict_LoadGlobal() returns NULL without raising
                         * an exception if the key doesn't exist */
                        format_exc_check_arg(tstate, PyExc_NameError,
                                             NAME_ERROR_MSG, name);
                    }
                    goto error;
                }
                Py_INCREF(v);
            }
            else {
                /* Slow-path if globals or builtins is not a dict */

                /* namespace 1: globals */
                v = PyObject_GetItem(GLOBALS(), name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);

                    /* namespace 2: builtins */
                    v = PyObject_GetItem(BUILTINS(), name);
                    if (v == NULL) {
                        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                            format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                        }
                        goto error;
                    }
                }
            }
            /* Skip over inline cache */
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            UOP_STACK_GROW(push_null);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, v);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_GLOBAL_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *name = GETITEM(names, oparg>>1);
                next_instr--;
                if (_Py_Specialize_LoadGlobal(GLOBALS(), BUILTINS(), next_instr, name) < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(LOAD_GLOBAL, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(LOAD_GLOBAL);  // TODO
            }
        }

        TARGET(LOAD_GLOBAL_MODULE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(GLOBALS()), LOAD_GLOBAL);
            PyDictObject *dict = (PyDictObject *)GLOBALS();
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            uint32_t version = read_u32(cache->module_keys_version);
            DEOPT_IF(dict->ma_keys->dk_version != version, LOAD_GLOBAL);
            assert(DK_IS_UNICODE(dict->ma_keys));
            PyDictUnicodeEntry *entries = DK_UNICODE_ENTRIES(dict->ma_keys);
            PyObject *res = entries[cache->index].me_value;
            DEOPT_IF(res == NULL, LOAD_GLOBAL);
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            STAT_INC(LOAD_GLOBAL, hit);
            UOP_STACK_GROW(push_null+1);
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_GLOBAL_BUILTIN) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(GLOBALS()), LOAD_GLOBAL);
            DEOPT_IF(!PyDict_CheckExact(BUILTINS()), LOAD_GLOBAL);
            PyDictObject *mdict = (PyDictObject *)GLOBALS();
            PyDictObject *bdict = (PyDictObject *)BUILTINS();
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            uint32_t mod_version = read_u32(cache->module_keys_version);
            uint16_t bltn_version = cache->builtin_keys_version;
            DEOPT_IF(mdict->ma_keys->dk_version != mod_version, LOAD_GLOBAL);
            DEOPT_IF(bdict->ma_keys->dk_version != bltn_version, LOAD_GLOBAL);
            assert(DK_IS_UNICODE(bdict->ma_keys));
            PyDictUnicodeEntry *entries = DK_UNICODE_ENTRIES(bdict->ma_keys);
            PyObject *res = entries[cache->index].me_value;
            DEOPT_IF(res == NULL, LOAD_GLOBAL);
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            STAT_INC(LOAD_GLOBAL, hit);
            UOP_STACK_GROW(push_null+1);
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(DELETE_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *v = GETLOCAL(oparg);
            if (v != NULL) {
                SETLOCAL(oparg, NULL);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            goto unbound_local_error;
        }

        TARGET(MAKE_CELL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            // "initial" is probably NULL but not if it's an arg (or set
            // via PyFrame_LocalsToFast() before MAKE_CELL has run).
            PyObject *initial = GETLOCAL(oparg);
            PyObject *cell = PyCell_New(initial);
            if (cell == NULL) {
                goto resume_with_error;
            }
            SETLOCAL(oparg, cell);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DELETE_DEREF) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cell = GETLOCAL(oparg);
            PyObject *oldobj = PyCell_GET(cell);
            if (oldobj != NULL) {
                PyCell_SET(cell, NULL);
                Py_DECREF(oldobj);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            format_exc_unbound(tstate, frame->f_code, oparg);
            goto error;
        }

        TARGET(LOAD_CLASSDEREF) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name, *value, *locals = LOCALS();
            assert(locals);
            assert(oparg >= 0 && oparg < frame->f_code->co_nlocalsplus);
            name = PyTuple_GET_ITEM(frame->f_code->co_localsplusnames, oparg);
            if (PyDict_CheckExact(locals)) {
                value = PyDict_GetItemWithError(locals, name);
                if (value != NULL) {
                    Py_INCREF(value);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                value = PyObject_GetItem(locals, name);
                if (value == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);
                }
            }
            if (!value) {
                PyObject *cell = GETLOCAL(oparg);
                value = PyCell_GET(cell);
                if (value == NULL) {
                    format_exc_unbound(tstate, frame->f_code, oparg);
                    goto error;
                }
                Py_INCREF(value);
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_DEREF) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cell = GETLOCAL(oparg);
            PyObject *value = PyCell_GET(cell);
            if (value == NULL) {
                format_exc_unbound(tstate, frame->f_code, oparg);
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(STORE_DEREF) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *v = POP();
            PyObject *cell = GETLOCAL(oparg);
            PyObject *oldobj = PyCell_GET(cell);
            PyCell_SET(cell, v);
            Py_XDECREF(oldobj);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(COPY_FREE_VARS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* Copy closure variables to free variables */
            PyCodeObject *co = frame->f_code;
            PyObject *closure = frame->f_func->func_closure;
            int offset = co->co_nlocals + co->co_nplaincellvars;
            assert(oparg == co->co_nfreevars);
            for (int i = 0; i < oparg; ++i) {
                PyObject *o = PyTuple_GET_ITEM(closure, i);
                Py_INCREF(o);
                frame->localsplus[offset + i] = o;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_STRING) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *str;
            str = _PyUnicode_JoinArray(&_Py_STR(empty),
                                       stack_pointer - oparg, oparg);
            if (str == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                Py_DECREF(item);
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, str);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_TUPLE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *tup = PyTuple_New(oparg);
            if (tup == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                PyTuple_SET_ITEM(tup, oparg, item);
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, tup);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_LIST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *list =  PyList_New(oparg);
            if (list == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                PyList_SET_ITEM(list, oparg, item);
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, list);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LIST_TO_TUPLE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *list = POP();
            PyObject *tuple = PyList_AsTuple(list);
            Py_DECREF(list);
            if (tuple == NULL) {
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, tuple);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LIST_EXTEND) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *iterable = POP();
            PyObject *list = PEEK(oparg);
            PyObject *none_val = _PyList_Extend((PyListObject *)list, iterable);
            if (none_val == NULL) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
                   (Py_TYPE(iterable)->tp_iter == NULL && !PySequence_Check(iterable)))
                {
                    _PyErr_Clear(tstate);
                    _PyErr_Format(tstate, PyExc_TypeError,
                          "Value after * must be an iterable, not %.200s",
                          Py_TYPE(iterable)->tp_name);
                }
                Py_DECREF(iterable);
                goto error;
            }
            Py_DECREF(none_val);
            Py_DECREF(iterable);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(SET_UPDATE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *iterable = POP();
            PyObject *set = PEEK(oparg);
            int err = _PySet_Update(set, iterable);
            Py_DECREF(iterable);
            if (err < 0) {
                goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_SET) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *set = PySet_New(NULL);
            int err = 0;
            int i;
            if (set == NULL)
                goto error;
            for (i = oparg; i > 0; i--) {
                PyObject *item = PEEK(i);
                if (err == 0)
                    err = PySet_Add(set, item);
                Py_DECREF(item);
            }
            UOP_STACK_GROW(-oparg);
            if (err != 0) {
                Py_DECREF(set);
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, set);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_MAP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *map = _PyDict_FromItems(
                    &PEEK(2*oparg), 2,
                    &PEEK(2*oparg - 1), 2,
                    oparg);
            if (map == NULL)
                goto error;

            while (oparg--) {
                Py_DECREF(POP());
                Py_DECREF(POP());
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, map);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(SETUP_ANNOTATIONS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            int err;
            PyObject *ann_dict;
            if (LOCALS() == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when setting up annotations");
                goto error;
            }
            /* check if __annotations__ in locals()... */
            if (PyDict_CheckExact(LOCALS())) {
                ann_dict = _PyDict_GetItemWithError(LOCALS(),
                                                    &_Py_ID(__annotations__));
                if (ann_dict == NULL) {
                    if (_PyErr_Occurred(tstate)) {
                        goto error;
                    }
                    /* ...if not, create a new one */
                    ann_dict = PyDict_New();
                    if (ann_dict == NULL) {
                        goto error;
                    }
                    err = PyDict_SetItem(LOCALS(), &_Py_ID(__annotations__),
                                         ann_dict);
                    Py_DECREF(ann_dict);
                    if (err != 0) {
                        goto error;
                    }
                }
            }
            else {
                /* do the same if locals() is not a dict */
                ann_dict = PyObject_GetItem(LOCALS(), &_Py_ID(__annotations__));
                if (ann_dict == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);
                    ann_dict = PyDict_New();
                    if (ann_dict == NULL) {
                        goto error;
                    }
                    err = PyObject_SetItem(LOCALS(), &_Py_ID(__annotations__),
                                           ann_dict);
                    Py_DECREF(ann_dict);
                    if (err != 0) {
                        goto error;
                    }
                }
                else {
                    Py_DECREF(ann_dict);
                }
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BUILD_CONST_KEY_MAP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *map;
            PyObject *keys = TOP();
            if (!PyTuple_CheckExact(keys) ||
                PyTuple_GET_SIZE(keys) != (Py_ssize_t)oparg) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad BUILD_CONST_KEY_MAP keys argument");
                goto error;
            }
            map = _PyDict_FromItems(
                    &PyTuple_GET_ITEM(keys, 0), 1,
                    &PEEK(oparg + 1), 1, oparg);
            if (map == NULL) {
                goto error;
            }

            Py_DECREF(POP());
            while (oparg--) {
                Py_DECREF(POP());
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, map);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DICT_UPDATE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *update = POP();
            PyObject *dict = PEEK(oparg);
            if (PyDict_Update(dict, update) < 0) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                    "'%.200s' object is not a mapping",
                                    Py_TYPE(update)->tp_name);
                }
                Py_DECREF(update);
                goto error;
            }
            Py_DECREF(update);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(DICT_MERGE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *update = POP();
            PyObject *dict = PEEK(oparg);

            if (_PyDict_MergeEx(dict, update, 2) < 0) {
                format_kwargs_error(tstate, PEEK(2 + oparg), update);
                Py_DECREF(update);
                goto error;
            }
            Py_DECREF(update);
            PREDICT(CALL_FUNCTION_EX);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MAP_ADD) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();
            PyObject *key = SECOND();
            PyObject *map;
            UOP_STACK_GROW(-2);
            map = PEEK(oparg);                      /* dict */
            assert(PyDict_CheckExact(map));
            /* map[key] = value */
            if (_PyDict_SetItem_Take2((PyDictObject *)map, key, value) != 0) {
                goto error;
            }
            PREDICT(JUMP_BACKWARD_QUICK);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(LOAD_ATTR);  // TODO
            PyObject *name = GETITEM(names, oparg >> 1);
            PyObject *owner = TOP();
            if (oparg & 1) {
                /* Designed to work in tandem with CALL. */
                PyObject* meth = NULL;

                int meth_found = _PyObject_GetMethod(owner, name, &meth);

                if (meth == NULL) {
                    /* Most likely attribute wasn't found. */
                    goto error;
                }

                if (meth_found) {
                    /* We can bypass temporary bound method object.
                       meth is unbound method and obj is self.

                       meth | self | arg1 | ... | argN
                     */
                    UOP_STACK_SET(1, meth);
                    UOP_STACK_GROW(1);
                    UOP_STACK_SET(1, owner);  // self
                }
                else {
                    /* meth is not an unbound method (but a regular attr, or
                       something was returned by a descriptor protocol).  Set
                       the second element of the stack to NULL, to signal
                       CALL that it's not a method call.

                       NULL | meth | arg1 | ... | argN
                    */
                    UOP_STACK_SET(1, NULL);
                    Py_DECREF(owner);
                    UOP_STACK_GROW(1);
                    UOP_STACK_SET(1, meth);
                }
                UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            PyObject *res = PyObject_GetAttr(owner, name);
            if (res == NULL) {
                goto error;
            }
            Py_DECREF(owner);
            UOP_STACK_SET(1, res);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *owner = TOP();
                PyObject *name = GETITEM(names, oparg>>1);
                next_instr--;
                if (_Py_Specialize_LoadAttr(owner, next_instr, name) < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(LOAD_ATTR, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(LOAD_ATTR);  // TODO
            }
        }

        TARGET(LOAD_ATTR_INSTANCE_VALUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            assert(tp->tp_dictoffset < 0);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(owner);
            DEOPT_IF(!_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            res = _PyDictOrValues_GetValues(dorv)->values[cache->index];
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            UOP_STACK_SET(1, NULL);
            UOP_STACK_GROW(oparg & 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_MODULE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            DEOPT_IF(!PyModule_CheckExact(owner), LOAD_ATTR);
            PyDictObject *dict = (PyDictObject *)((PyModuleObject *)owner)->md_dict;
            assert(dict != NULL);
            DEOPT_IF(dict->ma_keys->dk_version != read_u32(cache->version),
                LOAD_ATTR);
            assert(dict->ma_keys->dk_kind == DICT_KEYS_UNICODE);
            assert(cache->index < dict->ma_keys->dk_nentries);
            PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(dict->ma_keys) + cache->index;
            res = ep->me_value;
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            UOP_STACK_SET(1, NULL);
            UOP_STACK_GROW(oparg & 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_WITH_HINT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(owner);
            DEOPT_IF(_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyDictObject *dict = (PyDictObject *)_PyDictOrValues_GetDict(dorv);
            DEOPT_IF(dict == NULL, LOAD_ATTR);
            assert(PyDict_CheckExact((PyObject *)dict));
            PyObject *name = GETITEM(names, oparg>>1);
            uint16_t hint = cache->index;
            DEOPT_IF(hint >= (size_t)dict->ma_keys->dk_nentries, LOAD_ATTR);
            if (DK_IS_UNICODE(dict->ma_keys)) {
                PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, LOAD_ATTR);
                res = ep->me_value;
            }
            else {
                PyDictKeyEntry *ep = DK_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, LOAD_ATTR);
                res = ep->me_value;
            }
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            UOP_STACK_SET(1, NULL);
            UOP_STACK_GROW(oparg & 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_SLOT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            char *addr = (char *)owner + cache->index;
            res = *(PyObject **)addr;
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            UOP_STACK_SET(1, NULL);
            UOP_STACK_GROW(oparg & 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_CLASS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;

            PyObject *cls = TOP();
            DEOPT_IF(!PyType_Check(cls), LOAD_ATTR);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(((PyTypeObject *)cls)->tp_version_tag != type_version,
                LOAD_ATTR);
            assert(type_version != 0);

            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            Py_INCREF(res);
            UOP_STACK_SET(1, NULL);
            UOP_STACK_GROW(oparg & 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(cls);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_PROPERTY) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            DEOPT_IF(tstate->interp->eval_frame, LOAD_ATTR);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;

            PyObject *owner = TOP();
            PyTypeObject *cls = Py_TYPE(owner);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(type_version != 0);
            PyObject *fget = read_obj(cache->descr);
            assert(Py_IS_TYPE(fget, &PyFunction_Type));
            PyFunctionObject *f = (PyFunctionObject *)fget;
            uint32_t func_version = read_u32(cache->keys_version);
            assert(func_version != 0);
            DEOPT_IF(f->func_version != func_version, LOAD_ATTR);
            PyCodeObject *code = (PyCodeObject *)f->func_code;
            assert(code->co_argcount == 1);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(fget);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, f);
            UOP_STACK_SET(1, NULL);
            int shrink_stack = !(oparg & 1);
            UOP_STACK_GROW(-shrink_stack);
            new_frame->localsplus[0] = owner;
            for (int i = 1; i < code->co_nlocalsplus; i++) {
                new_frame->localsplus[i] = NULL;
            }
            _PyFrame_SetStackPointer(frame, stack_pointer);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            frame->prev_instr = next_instr - 1;
            new_frame->previous = frame;
            frame = cframe.current_frame = new_frame;
            CALL_STAT_INC(inlined_py_calls);
            goto start_frame;
        }

        TARGET(LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            DEOPT_IF(tstate->interp->eval_frame, LOAD_ATTR);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            PyObject *owner = TOP();
            PyTypeObject *cls = Py_TYPE(owner);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(type_version != 0);
            PyObject *getattribute = read_obj(cache->descr);
            assert(Py_IS_TYPE(getattribute, &PyFunction_Type));
            PyFunctionObject *f = (PyFunctionObject *)getattribute;
            PyCodeObject *code = (PyCodeObject *)f->func_code;
            DEOPT_IF(((PyCodeObject *)f->func_code)->co_argcount != 2, LOAD_ATTR);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), CALL);
            STAT_INC(LOAD_ATTR, hit);

            PyObject *name = GETITEM(names, oparg >> 1);
            Py_INCREF(f);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, f);
            UOP_STACK_SET(1, NULL);
            int shrink_stack = !(oparg & 1);
            UOP_STACK_GROW(-shrink_stack);
            Py_INCREF(name);
            new_frame->localsplus[0] = owner;
            new_frame->localsplus[1] = name;
            for (int i = 2; i < code->co_nlocalsplus; i++) {
                new_frame->localsplus[i] = NULL;
            }
            _PyFrame_SetStackPointer(frame, stack_pointer);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            frame->prev_instr = next_instr - 1;
            new_frame->previous = frame;
            frame = cframe.current_frame = new_frame;
            CALL_STAT_INC(inlined_py_calls);
            goto start_frame;
        }

        TARGET(STORE_ATTR_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *owner = TOP();
                PyObject *name = GETITEM(names, oparg);
                next_instr--;
                if (_Py_Specialize_StoreAttr(owner, next_instr, name) < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(STORE_ATTR, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(STORE_ATTR);  // TODO
            }
        }

        TARGET(STORE_ATTR_INSTANCE_VALUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(owner);
            DEOPT_IF(!_PyDictOrValues_IsValues(dorv), STORE_ATTR);
            STAT_INC(STORE_ATTR, hit);
            Py_ssize_t index = cache->index;
            UOP_STACK_GROW(-1);
            PyObject *value = POP();
            PyDictValues *values = _PyDictOrValues_GetValues(dorv);
            PyObject *old_value = values->values[index];
            values->values[index] = value;
            if (old_value == NULL) {
                _PyDictValues_AddToInsertionOrder(values, index);
            }
            else {
                Py_DECREF(old_value);
            }
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(STORE_ATTR_WITH_HINT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(owner);
            DEOPT_IF(_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyDictObject *dict = (PyDictObject *)_PyDictOrValues_GetDict(dorv);
            DEOPT_IF(dict == NULL, STORE_ATTR);
            assert(PyDict_CheckExact((PyObject *)dict));
            PyObject *name = GETITEM(names, oparg);
            uint16_t hint = cache->index;
            DEOPT_IF(hint >= (size_t)dict->ma_keys->dk_nentries, STORE_ATTR);
            PyObject *value, *old_value;
            if (DK_IS_UNICODE(dict->ma_keys)) {
                PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, STORE_ATTR);
                old_value = ep->me_value;
                DEOPT_IF(old_value == NULL, STORE_ATTR);
                UOP_STACK_GROW(-1);
                value = POP();
                ep->me_value = value;
            }
            else {
                PyDictKeyEntry *ep = DK_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, STORE_ATTR);
                old_value = ep->me_value;
                DEOPT_IF(old_value == NULL, STORE_ATTR);
                UOP_STACK_GROW(-1);
                value = POP();
                ep->me_value = value;
            }
            Py_DECREF(old_value);
            STAT_INC(STORE_ATTR, hit);
            /* Ensure dict is GC tracked if it needs to be */
            if (!_PyObject_GC_IS_TRACKED(dict) && _PyObject_GC_MAY_BE_TRACKED(value)) {
                _PyObject_GC_TRACK(dict);
            }
            /* PEP 509 */
            dict->ma_version_tag = DICT_NEXT_VERSION();
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(STORE_ATTR_SLOT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            char *addr = (char *)owner + cache->index;
            STAT_INC(STORE_ATTR, hit);
            UOP_STACK_GROW(-1);
            PyObject *value = POP();
            PyObject *old_value = *(PyObject **)addr;
            *(PyObject **)addr = value;
            Py_XDECREF(old_value);
            Py_DECREF(owner);
            UOP_JUMP(INLINE_CACHE_ENTRIES_STORE_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(COMPARE_OP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(COMPARE_OP);  // TODO
            assert(oparg <= Py_GE);
            PyObject *right = POP();
            PyObject *left = TOP();
            PyObject *res = PyObject_RichCompare(left, right, oparg);
            UOP_STACK_SET(1, res);
            Py_DECREF(left);
            Py_DECREF(right);
            if (res == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_COMPARE_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(COMPARE_OP_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyCompareOpCache *cache = (_PyCompareOpCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *right = TOP();
                PyObject *left = SECOND();
                next_instr--;
                _Py_Specialize_CompareOp(left, right, next_instr, oparg);
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(COMPARE_OP, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(COMPARE_OP);  // TODO
            }
        }

        TARGET(COMPARE_OP_FLOAT_JUMP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (float ? float) + POP_JUMP_(direction)_IF_(true/false)
            _PyCompareOpCache *cache = (_PyCompareOpCache *)next_instr;
            int when_to_jump_mask = cache->mask;
            PyObject *right = TOP();
            PyObject *left = SECOND();
            DEOPT_IF(!PyFloat_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), COMPARE_OP);
            double dleft = PyFloat_AS_DOUBLE(left);
            double dright = PyFloat_AS_DOUBLE(right);
            int sign = (dleft > dright) - (dleft < dright);
            DEOPT_IF(isnan(dleft), COMPARE_OP);
            DEOPT_IF(isnan(dright), COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_COMPARE_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_STACK_GROW(-2);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            assert(opcode == POP_JUMP_FORWARD_IF_FALSE ||
                   opcode == POP_JUMP_BACKWARD_IF_FALSE ||
                   opcode == POP_JUMP_FORWARD_IF_TRUE ||
                   opcode == POP_JUMP_BACKWARD_IF_TRUE);
            int jump = (9 << (sign + 1)) & when_to_jump_mask;
            if (!jump) {
                UOP_JUMP(1);
            }
            else if (jump >= 8) {
                assert(opcode == POP_JUMP_BACKWARD_IF_TRUE ||
                       opcode == POP_JUMP_BACKWARD_IF_FALSE);
                UOP_JUMP(1 - oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else {
                assert(opcode == POP_JUMP_FORWARD_IF_TRUE ||
                       opcode == POP_JUMP_FORWARD_IF_FALSE);
                UOP_JUMP(1 + oparg);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(COMPARE_OP_INT_JUMP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (int ? int) + POP_JUMP_(direction)_IF_(true/false)
            _PyCompareOpCache *cache = (_PyCompareOpCache *)next_instr;
            int when_to_jump_mask = cache->mask;
            PyObject *right = TOP();
            PyObject *left = SECOND();
            DEOPT_IF(!PyLong_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyLong_CheckExact(right), COMPARE_OP);
            DEOPT_IF((size_t)(Py_SIZE(left) + 1) > 2, COMPARE_OP);
            DEOPT_IF((size_t)(Py_SIZE(right) + 1) > 2, COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            assert(Py_ABS(Py_SIZE(left)) <= 1 && Py_ABS(Py_SIZE(right)) <= 1);
            Py_ssize_t ileft = Py_SIZE(left) * ((PyLongObject *)left)->ob_digit[0];
            Py_ssize_t iright = Py_SIZE(right) * ((PyLongObject *)right)->ob_digit[0];
            int sign = (ileft > iright) - (ileft < iright);
            UOP_JUMP(INLINE_CACHE_ENTRIES_COMPARE_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_STACK_GROW(-2);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            assert(opcode == POP_JUMP_FORWARD_IF_FALSE ||
                   opcode == POP_JUMP_BACKWARD_IF_FALSE ||
                   opcode == POP_JUMP_FORWARD_IF_TRUE ||
                   opcode == POP_JUMP_BACKWARD_IF_TRUE);
            int jump = (9 << (sign + 1)) & when_to_jump_mask;
            if (!jump) {
                UOP_JUMP(1);
            }
            else if (jump >= 8) {
                assert(opcode == POP_JUMP_BACKWARD_IF_TRUE ||
                       opcode == POP_JUMP_BACKWARD_IF_FALSE);
                UOP_JUMP(1 - oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else {
                assert(opcode == POP_JUMP_FORWARD_IF_TRUE ||
                       opcode == POP_JUMP_FORWARD_IF_FALSE);
                UOP_JUMP(1 + oparg);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(COMPARE_OP_STR_JUMP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (str == str or str != str) + POP_JUMP_(direction)_IF_(true/false)
            _PyCompareOpCache *cache = (_PyCompareOpCache *)next_instr;
            int when_to_jump_mask = cache->mask;
            PyObject *right = TOP();
            PyObject *left = SECOND();
            DEOPT_IF(!PyUnicode_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyUnicode_CheckExact(right), COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            int res = _PyUnicode_Equal(left, right);
            if (res < 0) {
                goto error;
            }
            assert(oparg == Py_EQ || oparg == Py_NE);
            UOP_JUMP(INLINE_CACHE_ENTRIES_COMPARE_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            assert(opcode == POP_JUMP_FORWARD_IF_FALSE ||
                   opcode == POP_JUMP_BACKWARD_IF_FALSE ||
                   opcode == POP_JUMP_FORWARD_IF_TRUE ||
                   opcode == POP_JUMP_BACKWARD_IF_TRUE);
            UOP_STACK_GROW(-2);
            _Py_DECREF_SPECIALIZED(left, _PyUnicode_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            assert(res == 0 || res == 1);
            int sign = 1 - res;
            int jump = (9 << (sign + 1)) & when_to_jump_mask;
            if (!jump) {
                UOP_JUMP(1);
            }
            else if (jump >= 8) {
                assert(opcode == POP_JUMP_BACKWARD_IF_TRUE ||
                       opcode == POP_JUMP_BACKWARD_IF_FALSE);
                UOP_JUMP(1 - oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else {
                assert(opcode == POP_JUMP_FORWARD_IF_TRUE ||
                       opcode == POP_JUMP_FORWARD_IF_FALSE);
                UOP_JUMP(1 + oparg);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(IS_OP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *right = POP();
            PyObject *left = TOP();
            int res = Py_Is(left, right) ^ oparg;
            PyObject *b = res ? Py_True : Py_False;
            Py_INCREF(b);
            UOP_STACK_SET(1, b);
            Py_DECREF(left);
            Py_DECREF(right);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CONTAINS_OP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *right = POP();
            PyObject *left = POP();
            int res = PySequence_Contains(right, left);
            Py_DECREF(left);
            Py_DECREF(right);
            if (res < 0) {
                goto error;
            }
            PyObject *b = (res^oparg) ? Py_True : Py_False;
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, b);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CHECK_EG_MATCH) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *match_type = POP();
            if (check_except_star_type_valid(tstate, match_type) < 0) {
                Py_DECREF(match_type);
                goto error;
            }

            PyObject *exc_value = TOP();
            PyObject *match = NULL, *rest = NULL;
            int res = exception_group_match(exc_value, match_type,
                                            &match, &rest);
            Py_DECREF(match_type);
            if (res < 0) {
                goto error;
            }

            if (match == NULL || rest == NULL) {
                assert(match == NULL);
                assert(rest == NULL);
                goto error;
            }
            if (Py_IsNone(match)) {
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, match);
                Py_XDECREF(rest);
            }
            else {
                /* Total or partial match - update the stack from
                 * [val]
                 * to
                 * [rest, match]
                 * (rest can be Py_None)
                 */

                UOP_STACK_SET(1, rest);
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, match);
                PyErr_SetExcInfo(NULL, Py_NewRef(match), NULL);
                Py_DECREF(exc_value);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CHECK_EXC_MATCH) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *right = POP();
            PyObject *left = TOP();
            assert(PyExceptionInstance_Check(left));
            if (check_except_type_valid(tstate, right) < 0) {
                 Py_DECREF(right);
                 goto error;
            }

            int res = PyErr_GivenExceptionMatches(left, right);
            Py_DECREF(right);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, Py_NewRef(res ? Py_True : Py_False));
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(IMPORT_NAME) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *fromlist = POP();
            PyObject *level = TOP();
            PyObject *res;
            res = import_name(tstate, frame, name, fromlist, level);
            Py_DECREF(level);
            Py_DECREF(fromlist);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(IMPORT_STAR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *from = POP(), *locals;
            int err;
            if (_PyFrame_FastToLocalsWithError(frame) < 0) {
                Py_DECREF(from);
                goto error;
            }

            locals = LOCALS();
            if (locals == NULL) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "no locals found during 'import *'");
                Py_DECREF(from);
                goto error;
            }
            err = import_all_from(tstate, locals, from);
            _PyFrame_LocalsToFast(frame, 0);
            Py_DECREF(from);
            if (err != 0)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(IMPORT_FROM) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *name = GETITEM(names, oparg);
            PyObject *from = TOP();
            PyObject *res;
            res = import_from(tstate, from, name);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            if (res == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_FORWARD) {
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            UOP_JUMP(oparg);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_BACKWARD) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            UOP_WARMUP();
            JUMP_TO_INSTRUCTION(JUMP_BACKWARD_QUICK);  // TODO
        }

        TARGET(POP_JUMP_BACKWARD_IF_FALSE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(POP_JUMP_BACKWARD_IF_FALSE);  // TODO
            PyObject *cond = POP();
            if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            int err = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (err > 0)
                ;
            else if (err == 0) {
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_FORWARD_IF_FALSE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(POP_JUMP_FORWARD_IF_FALSE);  // TODO
            PyObject *cond = POP();
            if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_JUMP(oparg);
            }
            else {
                int err = PyObject_IsTrue(cond);
                Py_DECREF(cond);
                if (err > 0)
                    ;
                else if (err == 0) {
                    UOP_JUMP(oparg);
                }
                else
                    goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_BACKWARD_IF_TRUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cond = POP();
            if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            int err = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (err > 0) {
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else if (err == 0)
                ;
            else
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_FORWARD_IF_TRUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cond = POP();
            if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_JUMP(oparg);
            }
            else {
                int err = PyObject_IsTrue(cond);
                Py_DECREF(cond);
                if (err > 0) {
                    UOP_JUMP(oparg);
                }
                else if (err == 0)
                    ;
                else
                    goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_BACKWARD_IF_NOT_NONE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            if (!Py_IsNone(value)) {
                Py_DECREF(value);
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            _Py_DECREF_NO_DEALLOC(value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_FORWARD_IF_NOT_NONE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            if (!Py_IsNone(value)) {
                UOP_JUMP(oparg);
            }
            Py_DECREF(value);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_BACKWARD_IF_NONE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            if (Py_IsNone(value)) {
                _Py_DECREF_NO_DEALLOC(value);
                UOP_JUMP(-oparg);
                UOP_CHECK_EVAL_BREAKER();
            }
            else {
                Py_DECREF(value);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(POP_JUMP_FORWARD_IF_NONE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = POP();
            if (Py_IsNone(value)) {
                _Py_DECREF_NO_DEALLOC(value);
                UOP_JUMP(oparg);
            }
            else {
                Py_DECREF(value);
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_IF_FALSE_OR_POP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cond = TOP();
            int err;
            if (Py_IsTrue(cond)) {
                UOP_STACK_GROW(-1);
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            if (Py_IsFalse(cond)) {
                UOP_JUMP(oparg);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            if (err > 0) {
                UOP_STACK_GROW(-1);
                Py_DECREF(cond);
            }
            else if (err == 0)
                UOP_JUMP(oparg);
            else
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_IF_TRUE_OR_POP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *cond = TOP();
            int err;
            if (Py_IsFalse(cond)) {
                UOP_STACK_GROW(-1);
                _Py_DECREF_NO_DEALLOC(cond);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            if (Py_IsTrue(cond)) {
                UOP_JUMP(oparg);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            err = PyObject_IsTrue(cond);
            if (err > 0) {
                UOP_JUMP(oparg);
            }
            else if (err == 0) {
                UOP_STACK_GROW(-1);
                Py_DECREF(cond);
            }
            else
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_BACKWARD_NO_INTERRUPT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* This bytecode is used in the `yield from` or `await` loop.
             * If there is an interrupt, we want it handled in the innermost
             * generator or coroutine, so we deliberately do not check it here.
             * (see bpo-30039).
             */
            UOP_JUMP(-oparg);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(JUMP_BACKWARD_QUICK) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(JUMP_BACKWARD_QUICK);  // TODO
            assert(oparg < INSTR_OFFSET());
            UOP_JUMP(-oparg);
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(GET_LEN) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            // PUSH(len(TOS))
            Py_ssize_t len_i = PyObject_Length(TOP());
            if (len_i < 0) {
                goto error;
            }
            PyObject *len_o = PyLong_FromSsize_t(len_i);
            if (len_o == NULL) {
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, len_o);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MATCH_CLASS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            // Pop TOS and TOS1. Set TOS to a tuple of attributes on success, or
            // None on failure.
            PyObject *names = POP();
            PyObject *type = POP();
            PyObject *subject = TOP();
            assert(PyTuple_CheckExact(names));
            PyObject *attrs = match_class(tstate, subject, type, oparg, names);
            Py_DECREF(names);
            Py_DECREF(type);
            if (attrs) {
                // Success!
                assert(PyTuple_CheckExact(attrs));
                UOP_STACK_SET(1, attrs);
            }
            else if (_PyErr_Occurred(tstate)) {
                // Error!
                goto error;
            }
            else {
                // Failure!
                Py_INCREF(Py_None);
                UOP_STACK_SET(1, Py_None);
            }
            Py_DECREF(subject);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MATCH_MAPPING) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *subject = TOP();
            int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_MAPPING;
            PyObject *res = match ? Py_True : Py_False;
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            UOP_INCREF();
            PREDICT(POP_JUMP_FORWARD_IF_FALSE);  // TODO
            PREDICT(POP_JUMP_BACKWARD_IF_FALSE);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MATCH_SEQUENCE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *subject = TOP();
            int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_SEQUENCE;
            PyObject *res = match ? Py_True : Py_False;
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            UOP_INCREF();
            PREDICT(POP_JUMP_FORWARD_IF_FALSE);  // TODO
            PREDICT(POP_JUMP_BACKWARD_IF_FALSE);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MATCH_KEYS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            // On successful match, PUSH(values). Otherwise, PUSH(None).
            PyObject *keys = TOP();
            PyObject *subject = SECOND();
            PyObject *values_or_none = match_keys(tstate, subject, keys);
            if (values_or_none == NULL) {
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, values_or_none);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(GET_ITER) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter = PyObject_GetIter(iterable);
            Py_DECREF(iterable);
            UOP_STACK_SET(1, iter);
            if (iter == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(GET_YIELD_FROM_ITER) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter;
            if (PyCoro_CheckExact(iterable)) {
                /* `iterable` is a coroutine */
                if (!(frame->f_code->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE))) {
                    /* and it is used in a 'yield from' expression of a
                       regular generator. */
                    Py_DECREF(iterable);
                    UOP_STACK_SET(1, NULL);
                    _PyErr_SetString(tstate, PyExc_TypeError,
                                     "cannot 'yield from' a coroutine object "
                                     "in a non-coroutine generator");
                    goto error;
                }
            }
            else if (!PyGen_CheckExact(iterable)) {
                /* `iterable` is not a generator. */
                iter = PyObject_GetIter(iterable);
                Py_DECREF(iterable);
                UOP_STACK_SET(1, iter);
                if (iter == NULL)
                    goto error;
            }
            PREDICT(LOAD_CONST);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(FOR_ITER) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(FOR_ITER);  // TODO
            /* before: [iter]; after: [iter, iter()] *or* [] */
            PyObject *iter = TOP();
            PyObject *next = (*Py_TYPE(iter)->tp_iternext)(iter);
            if (next != NULL) {
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, next);
                UOP_JUMP(INLINE_CACHE_ENTRIES_FOR_ITER);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            if (_PyErr_Occurred(tstate)) {
                if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
                    goto error;
                }
                else if (tstate->c_tracefunc != NULL) {
                    call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, frame);
                }
                _PyErr_Clear(tstate);
            }
        iterator_exhausted_no_error:
            /* iterator ended normally */
            assert(!_PyErr_Occurred(tstate));
            Py_DECREF(POP());
            UOP_JUMP(INLINE_CACHE_ENTRIES_FOR_ITER + oparg);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(FOR_ITER_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyForIterCache *cache = (_PyForIterCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                next_instr--;
                _Py_Specialize_ForIter(TOP(), next_instr);
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(FOR_ITER, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(FOR_ITER);  // TODO
            }
        }

        TARGET(FOR_ITER_LIST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyListIterObject *it = (_PyListIterObject *)TOP();
            DEOPT_IF(Py_TYPE(it) != &PyListIter_Type, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            PyListObject *seq = it->it_seq;
            if (seq == NULL) {
                goto iterator_exhausted_no_error;
            }
            if (it->it_index < PyList_GET_SIZE(seq)) {
                PyObject *next = PyList_GET_ITEM(seq, it->it_index++);
                Py_INCREF(next);
                UOP_STACK_GROW(1);
                UOP_STACK_SET(1, next);
                UOP_JUMP(INLINE_CACHE_ENTRIES_FOR_ITER);
                UOP_NEXT_OPCODE();
                UOP_NEXT_OPARG();
                UOP_LLTRACE();
                UOP_DISPATCH();
            }
            it->it_seq = NULL;
            Py_DECREF(seq);
            goto iterator_exhausted_no_error;
        }

        TARGET(FOR_ITER_RANGE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyRangeIterObject *r = (_PyRangeIterObject *)TOP();
            DEOPT_IF(Py_TYPE(r) != &PyRangeIter_Type, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            _Py_CODEUNIT next = next_instr[INLINE_CACHE_ENTRIES_FOR_ITER];
            assert(_PyOpcode_Deopt[_Py_OPCODE(next)] == STORE_FAST);
            if (r->index >= r->len) {
                goto iterator_exhausted_no_error;
            }
            long value = (long)(r->start +
                                (unsigned long)(r->index++) * r->step);
            if (_PyLong_AssignValue(&GETLOCAL(_Py_OPARG(next)), value) < 0) {
                goto error;
            }
            // The STORE_FAST is already done.
            UOP_JUMP(INLINE_CACHE_ENTRIES_FOR_ITER + 1);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(BEFORE_ASYNC_WITH) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *mgr = TOP();
            PyObject *res;
            PyObject *enter = _PyObject_LookupSpecial(mgr, &_Py_ID(__aenter__));
            if (enter == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "asynchronous context manager protocol",
                                  Py_TYPE(mgr)->tp_name);
                }
                goto error;
            }
            PyObject *exit = _PyObject_LookupSpecial(mgr, &_Py_ID(__aexit__));
            if (exit == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "asynchronous context manager protocol "
                                  "(missed __aexit__ method)",
                                  Py_TYPE(mgr)->tp_name);
                }
                Py_DECREF(enter);
                goto error;
            }
            UOP_STACK_SET(1, exit);
            Py_DECREF(mgr);
            res = _PyObject_CallNoArgs(enter);
            Py_DECREF(enter);
            if (res == NULL)
                goto error;
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            PREDICT(GET_AWAITABLE);  // TODO
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BEFORE_WITH) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *mgr = TOP();
            PyObject *res;
            PyObject *enter = _PyObject_LookupSpecial(mgr, &_Py_ID(__enter__));
            if (enter == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "context manager protocol",
                                  Py_TYPE(mgr)->tp_name);
                }
                goto error;
            }
            PyObject *exit = _PyObject_LookupSpecial(mgr, &_Py_ID(__exit__));
            if (exit == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "context manager protocol "
                                  "(missed __exit__ method)",
                                  Py_TYPE(mgr)->tp_name);
                }
                Py_DECREF(enter);
                goto error;
            }
            UOP_STACK_SET(1, exit);
            Py_DECREF(mgr);
            res = _PyObject_CallNoArgs(enter);
            Py_DECREF(enter);
            if (res == NULL) {
                goto error;
            }
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(WITH_EXCEPT_START) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* At the top of the stack are 4 values:
               - TOP = exc_info()
               - SECOND = previous exception
               - THIRD: lasti of exception in exc_info()
               - FOURTH: the context.__exit__ bound method
               We call FOURTH(type(TOP), TOP, GetTraceback(TOP)).
               Then we push the __exit__ return value.
            */
            PyObject *exit_func;
            PyObject *exc, *val, *tb, *res;

            val = TOP();
            assert(val && PyExceptionInstance_Check(val));
            exc = PyExceptionInstance_Class(val);
            tb = PyException_GetTraceback(val);
            Py_XDECREF(tb);
            assert(PyLong_Check(PEEK(3)));
            exit_func = PEEK(4);
            PyObject *stack[4] = {NULL, exc, val, tb};
            res = PyObject_Vectorcall(exit_func, stack + 1,
                    3 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
            if (res == NULL)
                goto error;

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(PUSH_EXC_INFO) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *value = TOP();

            _PyErr_StackItem *exc_info = tstate->exc_info;
            if (exc_info->exc_value != NULL) {
                UOP_STACK_SET(1, exc_info->exc_value);
            }
            else {
                Py_INCREF(Py_None);
                UOP_STACK_SET(1, Py_None);
            }

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, value);
            UOP_INCREF();
            assert(PyExceptionInstance_Check(value));
            exc_info->exc_value = value;

            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_METHOD_WITH_VALUES) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* Cached method object */
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            assert(type_version != 0);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(self_cls->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(self);
            DEOPT_IF(!_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyHeapTypeObject *self_heap_type = (PyHeapTypeObject *)self_cls;
            DEOPT_IF(self_heap_type->ht_cached_keys->dk_version !=
                     read_u32(cache->keys_version), LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, self);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_METHOD_WITH_DICT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* Can be either a managed dict, or a tp_dictoffset offset.*/
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;

            DEOPT_IF(self_cls->tp_version_tag != read_u32(cache->type_version),
                     LOAD_ATTR);
            /* Treat index as a signed 16 bit value */
            Py_ssize_t dictoffset = self_cls->tp_dictoffset;
            assert(dictoffset > 0);
            PyDictObject **dictptr = (PyDictObject**)(((char *)self)+dictoffset);
            PyDictObject *dict = *dictptr;
            DEOPT_IF(dict == NULL, LOAD_ATTR);
            DEOPT_IF(dict->ma_keys->dk_version != read_u32(cache->keys_version),
                     LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, self);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_METHOD_NO_DICT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(self_cls->tp_dictoffset == 0);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, self);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(LOAD_ATTR_METHOD_LAZY_DICT) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            Py_ssize_t dictoffset = self_cls->tp_dictoffset;
            assert(dictoffset > 0);
            PyObject *dict = *(PyObject **)((char *)self + dictoffset);
            /* This object has a __dict__, just not yet created */
            DEOPT_IF(dict != NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            Py_INCREF(res);
            UOP_STACK_SET(1, res);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, self);
            UOP_JUMP(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(CALL_BOUND_METHOD_EXACT_ARGS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            DEOPT_IF(is_method(stack_pointer, oparg), CALL);
            PyObject *function = PEEK(oparg + 1);
            DEOPT_IF(Py_TYPE(function) != &PyMethod_Type, CALL);
            STAT_INC(CALL, hit);
            PyObject *meth = ((PyMethodObject *)function)->im_func;
            PyObject *self = ((PyMethodObject *)function)->im_self;
            Py_INCREF(meth);
            Py_INCREF(self);
            PEEK(oparg + 1) = self;
            PEEK(oparg + 2) = meth;
            Py_DECREF(function);
            goto call_exact_args;
        }

        TARGET(KW_NAMES) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            assert(oparg < PyTuple_GET_SIZE(consts));
            call_shape.kwnames = GETITEM(consts, oparg);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            int total_args, is_meth;
        call_function:
            is_meth = is_method(stack_pointer, oparg);
            PyObject *function = PEEK(oparg + 1);
            if (!is_meth && Py_TYPE(function) == &PyMethod_Type) {
                PyObject *meth = ((PyMethodObject *)function)->im_func;
                PyObject *self = ((PyMethodObject *)function)->im_self;
                Py_INCREF(meth);
                Py_INCREF(self);
                PEEK(oparg+1) = self;
                PEEK(oparg+2) = meth;
                Py_DECREF(function);
                is_meth = 1;
            }
            total_args = oparg + is_meth;
            function = PEEK(total_args + 1);
            int positional_args = total_args - KWNAMES_LEN();
            // Check if the call can be inlined or not
            if (Py_TYPE(function) == &PyFunction_Type && tstate->interp->eval_frame == NULL) {
                int code_flags = ((PyCodeObject*)PyFunction_GET_CODE(function))->co_flags;
                PyObject *locals = code_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(function));
                UOP_STACK_GROW(-total_args);
                _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit(
                    tstate, (PyFunctionObject *)function, locals,
                    stack_pointer, positional_args, call_shape.kwnames
                );
                call_shape.kwnames = NULL;
                UOP_STACK_GROW(is_meth - 2);
                // The frame has stolen all the arguments from the stack,
                // so there is no need to clean them up.
                if (new_frame == NULL) {
                    goto error;
                }
                _PyFrame_SetStackPointer(frame, stack_pointer);
                UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
                frame->prev_instr = next_instr - 1;
                new_frame->previous = frame;
                cframe.current_frame = frame = new_frame;
                CALL_STAT_INC(inlined_py_calls);
                goto start_frame;
            }
            /* Callable is not a normal Python function */
            PyObject *res;
            if (cframe.use_tracing) {
                res = trace_call_function(
                    tstate, function, stack_pointer-total_args,
                    positional_args, call_shape.kwnames);
            }
            else {
                res = PyObject_Vectorcall(
                    function, stack_pointer-total_args,
                    positional_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
                    call_shape.kwnames);
            }
            call_shape.kwnames = NULL;
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(function);
            /* Clear the stack */
            UOP_STACK_GROW(-total_args);
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                next_instr--;
                int is_meth = is_method(stack_pointer, oparg);
                int nargs = oparg + is_meth;
                PyObject *callable = PEEK(nargs + 1);
                int err = _Py_Specialize_Call(callable, next_instr, nargs,
                                              call_shape.kwnames);
                if (err < 0) {
                    goto error;
                }
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(CALL, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                goto call_function;
            }
        }

        TARGET(CALL_PY_EXACT_ARGS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
        call_exact_args:
            assert(call_shape.kwnames == NULL);
            DEOPT_IF(tstate->interp->eval_frame, CALL);
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            int is_meth = is_method(stack_pointer, oparg);
            int argcount = oparg + is_meth;
            PyObject *callable = PEEK(argcount + 1);
            DEOPT_IF(!PyFunction_Check(callable), CALL);
            PyFunctionObject *func = (PyFunctionObject *)callable;
            DEOPT_IF(func->func_version != read_u32(cache->func_version), CALL);
            PyCodeObject *code = (PyCodeObject *)func->func_code;
            DEOPT_IF(code->co_argcount != argcount, CALL);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), CALL);
            STAT_INC(CALL, hit);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, func);
            CALL_STAT_INC(inlined_py_calls);
            UOP_STACK_GROW(-argcount);
            for (int i = 0; i < argcount; i++) {
                new_frame->localsplus[i] = stack_pointer[i];
            }
            for (int i = argcount; i < code->co_nlocalsplus; i++) {
                new_frame->localsplus[i] = NULL;
            }
            UOP_STACK_GROW(is_meth - 2);
            _PyFrame_SetStackPointer(frame, stack_pointer);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            frame->prev_instr = next_instr - 1;
            new_frame->previous = frame;
            frame = cframe.current_frame = new_frame;
            goto start_frame;
        }

        TARGET(CALL_PY_WITH_DEFAULTS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            DEOPT_IF(tstate->interp->eval_frame, CALL);
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            int is_meth = is_method(stack_pointer, oparg);
            int argcount = oparg + is_meth;
            PyObject *callable = PEEK(argcount + 1);
            DEOPT_IF(!PyFunction_Check(callable), CALL);
            PyFunctionObject *func = (PyFunctionObject *)callable;
            DEOPT_IF(func->func_version != read_u32(cache->func_version), CALL);
            PyCodeObject *code = (PyCodeObject *)func->func_code;
            DEOPT_IF(argcount > code->co_argcount, CALL);
            int minargs = cache->min_args;
            DEOPT_IF(argcount < minargs, CALL);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), CALL);
            STAT_INC(CALL, hit);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, func);
            CALL_STAT_INC(inlined_py_calls);
            UOP_STACK_GROW(-argcount);
            for (int i = 0; i < argcount; i++) {
                new_frame->localsplus[i] = stack_pointer[i];
            }
            for (int i = argcount; i < code->co_argcount; i++) {
                PyObject *def = PyTuple_GET_ITEM(func->func_defaults,
                                                 i - minargs);
                Py_INCREF(def);
                new_frame->localsplus[i] = def;
            }
            for (int i = code->co_argcount; i < code->co_nlocalsplus; i++) {
                new_frame->localsplus[i] = NULL;
            }
            UOP_STACK_GROW(is_meth - 2);
            _PyFrame_SetStackPointer(frame, stack_pointer);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            frame->prev_instr = next_instr - 1;
            new_frame->previous = frame;
            frame = cframe.current_frame = new_frame;
            goto start_frame;
        }

        TARGET(CALL_NO_KW_TYPE_1) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            assert(cframe.use_tracing == 0);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *obj = TOP();
            PyObject *callable = SECOND();
            DEOPT_IF(callable != (PyObject *)&PyType_Type, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyObject *res = Py_NewRef(Py_TYPE(obj));
            Py_DECREF(callable);
            Py_DECREF(obj);
            UOP_STACK_GROW(-2);
            UOP_STACK_SET(1, res);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_STR_1) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            assert(cframe.use_tracing == 0);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *callable = PEEK(2);
            DEOPT_IF(callable != (PyObject *)&PyUnicode_Type, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyObject *arg = TOP();
            PyObject *res = PyObject_Str(arg);
            Py_DECREF(arg);
            Py_DECREF(&PyUnicode_Type);
            UOP_STACK_GROW(-2);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_TUPLE_1) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *callable = PEEK(2);
            DEOPT_IF(callable != (PyObject *)&PyTuple_Type, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyObject *arg = TOP();
            PyObject *res = PySequence_Tuple(arg);
            Py_DECREF(arg);
            Py_DECREF(&PyTuple_Type);
            UOP_STACK_GROW(-2);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_BUILTIN_CLASS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            int kwnames_len = KWNAMES_LEN();
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyType_Check(callable), CALL);
            PyTypeObject *tp = (PyTypeObject *)callable;
            DEOPT_IF(tp->tp_vectorcall == NULL, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            UOP_STACK_GROW(-total_args);
            PyObject *res = tp->tp_vectorcall((PyObject *)tp, stack_pointer,
                                              total_args-kwnames_len, call_shape.kwnames);
            call_shape.kwnames = NULL;
            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(tp);
            UOP_STACK_GROW(is_meth - 1);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_BUILTIN_O) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            /* Builtin METH_O functions */
            assert(call_shape.kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) != METH_O, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable);
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *arg = TOP();
            PyObject *res = cfunc(PyCFunction_GET_SELF(callable), arg);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            Py_DECREF(arg);
            Py_DECREF(callable);
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_BUILTIN_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            /* Builtin METH_FASTCALL functions, without keywords */
            assert(call_shape.kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) != METH_FASTCALL,
                CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable);
            UOP_STACK_GROW(-total_args);
            /* res = func(self, args, nargs) */
            PyObject *res = ((_PyCFunctionFast)(void(*)(void))cfunc)(
                PyCFunction_GET_SELF(callable),
                stack_pointer,
                total_args);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                /* Not deopting because this doesn't mean our optimization was
                   wrong. `res` can be NULL for valid reasons. Eg. getattr(x,
                   'invalid'). In those cases an exception is set, so we must
                   handle it.
                */
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_BUILTIN_FAST_WITH_KEYWORDS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            /* Builtin METH_FASTCALL | METH_KEYWORDS functions */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) !=
                (METH_FASTCALL | METH_KEYWORDS), CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            UOP_STACK_GROW(-total_args);
            /* res = func(self, args, nargs, kwnames) */
            _PyCFunctionFastWithKeywords cfunc =
                (_PyCFunctionFastWithKeywords)(void(*)(void))
                PyCFunction_GET_FUNCTION(callable);
            PyObject *res = cfunc(
                PyCFunction_GET_SELF(callable),
                stack_pointer,
                total_args - KWNAMES_LEN(),
                call_shape.kwnames
            );
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            call_shape.kwnames = NULL;

            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_LEN) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            assert(call_shape.kwnames == NULL);
            /* len(o) */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyObject *callable = PEEK(total_args + 1);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.len, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyObject *arg = TOP();
            Py_ssize_t len_i = PyObject_Length(arg);
            if (len_i < 0) {
                goto error;
            }
            PyObject *res = PyLong_FromSsize_t(len_i);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            Py_DECREF(arg);
            if (res == NULL) {
                goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_ISINSTANCE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            assert(call_shape.kwnames == NULL);
            /* isinstance(o, o2) */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(total_args != 2, CALL);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.isinstance, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyObject *cls = POP();
            PyObject *inst = TOP();
            int retval = PyObject_IsInstance(inst, cls);
            if (retval < 0) {
                Py_DECREF(cls);
                goto error;
            }
            PyObject *res = PyBool_FromLong(retval);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_SET(1, res);
            Py_DECREF(inst);
            Py_DECREF(cls);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_LIST_APPEND) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            assert(call_shape.kwnames == NULL);
            assert(oparg == 1);
            PyObject *callable = PEEK(3);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.list_append, CALL);
            PyObject *list = SECOND();
            DEOPT_IF(!PyList_Check(list), CALL);
            STAT_INC(CALL, hit);
            // CALL + POP_TOP
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL + 1);
            assert(_Py_OPCODE(next_instr[-1]) == POP_TOP);
            PyObject *arg = POP();
            if (_PyList_AppendTakeRef((PyListObject *)list, arg) < 0) {
                goto error;
            }
            UOP_STACK_GROW(-2);
            Py_DECREF(list);
            Py_DECREF(callable);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_METHOD_DESCRIPTOR_O) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            DEOPT_IF(total_args != 2, CALL);
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != METH_O, CALL);
            PyObject *arg = TOP();
            PyObject *self = SECOND();
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyCFunction cfunc = meth->ml_meth;
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *res = cfunc(self, arg);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(self);
            Py_DECREF(arg);
            UOP_STACK_GROW(-oparg - 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != (METH_FASTCALL|METH_KEYWORDS), CALL);
            PyTypeObject *d_type = callable->d_common.d_type;
            PyObject *self = PEEK(total_args);
            DEOPT_IF(!Py_IS_TYPE(self, d_type), CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            int nargs = total_args-1;
            UOP_STACK_GROW(-nargs);
            _PyCFunctionFastWithKeywords cfunc =
                (_PyCFunctionFastWithKeywords)(void(*)(void))meth->ml_meth;
            PyObject *res = cfunc(self, stack_pointer, nargs - KWNAMES_LEN(),
                                  call_shape.kwnames);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            call_shape.kwnames = NULL;

            /* Free the arguments. */
            for (int i = 0; i < nargs; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(self);
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            assert(oparg == 0 || oparg == 1);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyMethodDescrObject *callable = (PyMethodDescrObject *)SECOND();
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            PyObject *self = TOP();
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            DEOPT_IF(meth->ml_flags != METH_NOARGS, CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            PyCFunction cfunc = meth->ml_meth;
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *res = cfunc(self, NULL);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(self);
            UOP_STACK_GROW(-oparg - 1);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_NO_KW_METHOD_DESCRIPTOR_FAST) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(call_shape.kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            /* Builtin METH_FASTCALL methods, without keywords */
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != METH_FASTCALL, CALL);
            PyObject *self = PEEK(total_args);
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            STAT_INC(CALL, hit);
            UOP_JUMP(INLINE_CACHE_ENTRIES_CALL);
            _PyCFunctionFast cfunc =
                (_PyCFunctionFast)(void(*)(void))meth->ml_meth;
            int nargs = total_args-1;
            UOP_STACK_GROW(-nargs);
            PyObject *res = cfunc(self, stack_pointer, nargs);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            /* Clear the stack of the arguments. */
            for (int i = 0; i < nargs; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(self);
            UOP_STACK_GROW(is_meth - 2);
            UOP_STACK_SET(1, res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CALL_FUNCTION_EX) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(CALL_FUNCTION_EX);  // TODO
            PyObject *func, *callargs, *kwargs = NULL, *result;
            if (oparg & 0x01) {
                kwargs = POP();
                if (!PyDict_CheckExact(kwargs)) {
                    PyObject *d = PyDict_New();
                    if (d == NULL)
                        goto error;
                    if (_PyDict_MergeEx(d, kwargs, 2) < 0) {
                        Py_DECREF(d);
                        format_kwargs_error(tstate, SECOND(), kwargs);
                        Py_DECREF(kwargs);
                        goto error;
                    }
                    Py_DECREF(kwargs);
                    kwargs = d;
                }
                assert(PyDict_CheckExact(kwargs));
            }
            callargs = POP();
            func = TOP();
            if (!PyTuple_CheckExact(callargs)) {
                if (check_args_iterable(tstate, func, callargs) < 0) {
                    Py_DECREF(callargs);
                    goto error;
                }
                Py_SETREF(callargs, PySequence_Tuple(callargs));
                if (callargs == NULL) {
                    goto error;
                }
            }
            assert(PyTuple_CheckExact(callargs));

            result = do_call_core(tstate, func, callargs, kwargs, cframe.use_tracing);
            Py_DECREF(func);
            Py_DECREF(callargs);
            Py_XDECREF(kwargs);

            UOP_STACK_GROW(-1);
            assert(TOP() == NULL);
            UOP_STACK_SET(1, result);
            if (result == NULL) {
                goto error;
            }
            UOP_CHECK_EVAL_BREAKER();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(MAKE_FUNCTION) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *codeobj = POP();
            PyFunctionObject *func = (PyFunctionObject *)
                PyFunction_New(codeobj, GLOBALS());

            Py_DECREF(codeobj);
            if (func == NULL) {
                goto error;
            }

            if (oparg & 0x08) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_closure = POP();
            }
            if (oparg & 0x04) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_annotations = POP();
            }
            if (oparg & 0x02) {
                assert(PyDict_CheckExact(TOP()));
                func->func_kwdefaults = POP();
            }
            if (oparg & 0x01) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_defaults = POP();
            }

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, (PyObject *)func);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(RETURN_GENERATOR) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyGenObject *gen = (PyGenObject *)_Py_MakeCoro(frame->f_func);
            if (gen == NULL) {
                goto error;
            }
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _PyInterpreterFrame *gen_frame = (_PyInterpreterFrame *)gen->gi_iframe;
            _PyFrame_Copy(frame, gen_frame);
            assert(frame->frame_obj == NULL);
            gen->gi_frame_state = FRAME_CREATED;
            gen_frame->owner = FRAME_OWNED_BY_GENERATOR;
            _Py_LeaveRecursiveCallTstate(tstate);
            if (!frame->is_entry) {
                _PyInterpreterFrame *prev = frame->previous;
                _PyThreadState_PopFrame(tstate, frame);
                frame = cframe.current_frame = prev;
                _PyFrame_StackPush(frame, (PyObject *)gen);
                goto resume_frame;
            }
            /* Make sure that frame is in a valid state */
            frame->stacktop = 0;
            frame->f_locals = NULL;
            Py_INCREF(frame->f_func);
            Py_INCREF(frame->f_code);
            /* Restore previous cframe and return. */
            tstate->cframe = cframe.previous;
            tstate->cframe->use_tracing = cframe.use_tracing;
            assert(tstate->cframe->current_frame == frame->previous);
            assert(!_PyErr_Occurred(tstate));
            return (PyObject *)gen;
        }

        TARGET(BUILD_SLICE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PyObject *start, *stop, *step, *slice;
            if (oparg == 3)
                step = POP();
            else
                step = NULL;
            stop = POP();
            start = TOP();
            slice = PySlice_New(start, stop, step);
            Py_DECREF(start);
            Py_DECREF(stop);
            Py_XDECREF(step);
            UOP_STACK_SET(1, slice);
            if (slice == NULL)
                goto error;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(FORMAT_VALUE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            /* Handles f-string value formatting. */
            PyObject *result;
            PyObject *fmt_spec;
            PyObject *value;
            PyObject *(*conv_fn)(PyObject *);
            int which_conversion = oparg & FVC_MASK;
            int have_fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;

            fmt_spec = have_fmt_spec ? POP() : NULL;
            value = POP();

            /* See if any conversion is specified. */
            switch (which_conversion) {
            case FVC_NONE:  conv_fn = NULL;           break;
            case FVC_STR:   conv_fn = PyObject_Str;   break;
            case FVC_REPR:  conv_fn = PyObject_Repr;  break;
            case FVC_ASCII: conv_fn = PyObject_ASCII; break;
            default:
                _PyErr_Format(tstate, PyExc_SystemError,
                              "unexpected conversion flag %d",
                              which_conversion);
                goto error;
            }

            /* If there's a conversion function, call it and replace
               value with that result. Otherwise, just use value,
               without conversion. */
            if (conv_fn != NULL) {
                result = conv_fn(value);
                Py_DECREF(value);
                if (result == NULL) {
                    Py_XDECREF(fmt_spec);
                    goto error;
                }
                value = result;
            }

            /* If value is a unicode object, and there's no fmt_spec,
               then we know the result of format(value) is value
               itself. In that case, skip calling format(). I plan to
               move this optimization in to PyObject_Format()
               itself. */
            if (PyUnicode_CheckExact(value) && fmt_spec == NULL) {
                /* Do nothing, just transfer ownership to result. */
                result = value;
            } else {
                /* Actually call format(). */
                result = PyObject_Format(value, fmt_spec);
                Py_DECREF(value);
                Py_XDECREF(fmt_spec);
                if (result == NULL) {
                    goto error;
                }
            }

            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, result);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(COPY) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(oparg != 0);
            PyObject *peek = PEEK(oparg);
            UOP_STACK_GROW(1);
            UOP_STACK_SET(1, peek);
            UOP_INCREF();
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            PREDICTED(BINARY_OP);  // TODO
            PyObject *rhs = POP();
            PyObject *lhs = TOP();
            assert(0 <= oparg);
            assert((unsigned)oparg < Py_ARRAY_LENGTH(binary_ops));
            assert(binary_ops[oparg]);
            PyObject *res = binary_ops[oparg](lhs, rhs);
            Py_DECREF(lhs);
            Py_DECREF(rhs);
            UOP_STACK_SET(1, res);
            if (res == NULL) {
                goto error;
            }
            UOP_JUMP(INLINE_CACHE_ENTRIES_BINARY_OP);
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(BINARY_OP_ADAPTIVE) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(cframe.use_tracing == 0);
            _PyBinaryOpCache *cache = (_PyBinaryOpCache *)next_instr;
            if (ADAPTIVE_COUNTER_IS_ZERO(cache)) {
                PyObject *lhs = SECOND();
                PyObject *rhs = TOP();
                next_instr--;
                _Py_Specialize_BinaryOp(lhs, rhs, next_instr, oparg, &GETLOCAL(0));
                UOP_NEXT_OPCODE();
                UOP_LLTRACE();
                UOP_CHECK_TRACING();
                UOP_DISPATCH();
            }
            else {
                STAT_INC(BINARY_OP, deferred);
                DECREMENT_ADAPTIVE_COUNTER(cache);
                JUMP_TO_INSTRUCTION(BINARY_OP);  // TODO
            }
        }

        TARGET(SWAP) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(oparg != 0);
            PyObject *top = TOP();
            UOP_STACK_SET(1, PEEK(oparg));
            PEEK(oparg) = top;
            UOP_NEXT_OPCODE();
            UOP_NEXT_OPARG();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(EXTENDED_ARG) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(oparg);
            UOP_EXTEND_OPARG();
            // We might be tracing. To avoid breaking tracing guarantees in
            // quickened instructions, always deoptimize the next opcode:
            opcode = _PyOpcode_Deopt[_Py_OPCODE(*next_instr)];
            UOP_LLTRACE();
            // CPython hasn't traced the following instruction historically
            // (DO_TRACING would clobber our extended oparg anyways), so just
            // skip our usual cframe.use_tracing check before dispatch. Also,
            // make sure the next instruction isn't a RESUME, since that needs
            // to trace properly (and shouldn't have an extended arg anyways):
            assert(opcode != RESUME);
            UOP_DISPATCH();
        }

        TARGET(EXTENDED_ARG_QUICK) {  // TODO
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            assert(oparg);
            UOP_EXTEND_OPARG();
            UOP_NEXT_OPCODE();
            UOP_LLTRACE();
            UOP_CHECK_TRACING();
            UOP_DISPATCH();
        }

        TARGET(CACHE) {
            UOP_WRITE_PREV_INSTR();
            UOP_JUMP(1);
            UOP_UPDATE_STATS();
            UOP_UNREACHABLE();
        }

#if USE_COMPUTED_GOTOS
        TARGET_DO_TRACING:
#else
        case DO_TRACING:
#endif
    {
        assert(cframe.use_tracing);
        assert(tstate->tracing == 0);
        if (INSTR_OFFSET() >= frame->f_code->_co_firsttraceable) {
            int instr_prev = _PyInterpreterFrame_LASTI(frame);
            frame->prev_instr = next_instr;
            TRACING_NEXTOPARG();
            if (opcode == RESUME) {
                if (oparg < 2) {
                    CHECK_EVAL_BREAKER();
                }
                /* Call tracing */
                TRACE_FUNCTION_ENTRY();
                DTRACE_FUNCTION_ENTRY();
            }
            else {
                /* line-by-line tracing support */
                if (PyDTrace_LINE_ENABLED()) {
                    maybe_dtrace_line(frame, &tstate->trace_info, instr_prev);
                }

                if (cframe.use_tracing &&
                    tstate->c_tracefunc != NULL && !tstate->tracing) {
                    int err;
                    /* see maybe_call_line_trace()
                    for expository comments */
                    _PyFrame_SetStackPointer(frame, stack_pointer);

                    err = maybe_call_line_trace(tstate->c_tracefunc,
                                                tstate->c_traceobj,
                                                tstate, frame, instr_prev);
                    // Reload possibly changed frame fields:
                    stack_pointer = _PyFrame_GetStackPointer(frame);
                    frame->stacktop = -1;
                    // next_instr is only reloaded if tracing *does not* raise.
                    // This is consistent with the behavior of older Python
                    // versions. If a trace function sets a new f_lineno and
                    // *then* raises, we use the *old* location when searching
                    // for an exception handler, displaying the traceback, and
                    // so on:
                    if (err) {
                        // next_instr wasn't incremented at the start of this
                        // instruction. Increment it before handling the error,
                        // so that it looks the same as a "normal" instruction:
                        next_instr++;
                        goto error;
                    }
                    // Reload next_instr. Don't increment it, though, since
                    // we're going to re-dispatch to the "true" instruction now:
                    next_instr = frame->prev_instr;
                }
            }
        }
        TRACING_NEXTOPARG();
        PRE_DISPATCH_GOTO();
        DISPATCH_GOTO();
    }

#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#else
        EXTRA_CASES  // From opcode.h, a 'case' for each unused opcode
#endif
            /* Tell C compilers not to hold the opcode variable in the loop.
               next_instr points the current instruction without TARGET(). */
            opcode = _Py_OPCODE(*next_instr);
            fprintf(stderr, "XXX lineno: %d, opcode: %d\n",
                    _PyInterpreterFrame_GetLine(frame),  opcode);
            _PyErr_SetString(tstate, PyExc_SystemError, "unknown opcode");
            goto error;

        } /* End instructions */

        /* This should never be reached. Every opcode should end with DISPATCH()
           or goto error. */
        Py_UNREACHABLE();

/* Specialization misses */

miss:
    {
        STAT_INC(opcode, miss);
        opcode = _PyOpcode_Deopt[opcode];
        STAT_INC(opcode, miss);
        /* The counter is always the first cache entry: */
        _Py_CODEUNIT *counter = (_Py_CODEUNIT *)next_instr;
        *counter -= 1;
        if (*counter == 0) {
            int adaptive_opcode = _PyOpcode_Adaptive[opcode];
            assert(adaptive_opcode);
            _Py_SET_OPCODE(next_instr[-1], adaptive_opcode);
            STAT_INC(opcode, deopt);
            *counter = adaptive_counter_start();
        }
        next_instr--;
        DISPATCH_GOTO();
    }

binary_subscr_dict_error:
        {
            PyObject *sub = POP();
            if (!_PyErr_Occurred(tstate)) {
                _PyErr_SetKeyError(sub);
            }
            Py_DECREF(sub);
            goto error;
        }

unbound_local_error:
        {
            format_exc_check_arg(tstate, PyExc_UnboundLocalError,
                UNBOUNDLOCAL_ERROR_MSG,
                PyTuple_GetItem(frame->f_code->co_localsplusnames, oparg)
            );
            goto error;
        }

error:
        call_shape.kwnames = NULL;
        /* Double-check exception status. */
#ifdef NDEBUG
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_SystemError,
                             "error return without exception set");
        }
#else
        assert(_PyErr_Occurred(tstate));
#endif

        /* Log traceback info. */
        PyFrameObject *f = _PyFrame_GetFrameObject(frame);
        if (f != NULL) {
            PyTraceBack_Here(f);
        }

        if (tstate->c_tracefunc != NULL) {
            /* Make sure state is set to FRAME_UNWINDING for tracing */
            call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj,
                           tstate, frame);
        }

exception_unwind:
        {
            /* We can't use frame->f_lasti here, as RERAISE may have set it */
            int offset = INSTR_OFFSET()-1;
            int level, handler, lasti;
            if (get_exception_handler(frame->f_code, offset, &level, &handler, &lasti) == 0) {
                // No handlers, so exit.
                assert(_PyErr_Occurred(tstate));

                /* Pop remaining stack entries. */
                PyObject **stackbase = _PyFrame_Stackbase(frame);
                while (stack_pointer > stackbase) {
                    PyObject *o = POP();
                    Py_XDECREF(o);
                }
                assert(STACK_LEVEL() == 0);
                _PyFrame_SetStackPointer(frame, stack_pointer);
                TRACE_FUNCTION_UNWIND();
                DTRACE_FUNCTION_EXIT();
                goto exit_unwind;
            }

            assert(STACK_LEVEL() >= level);
            PyObject **new_top = _PyFrame_Stackbase(frame) + level;
            while (stack_pointer > new_top) {
                PyObject *v = POP();
                Py_XDECREF(v);
            }
            PyObject *exc, *val, *tb;
            if (lasti) {
                int frame_lasti = _PyInterpreterFrame_LASTI(frame);
                PyObject *lasti = PyLong_FromLong(frame_lasti);
                if (lasti == NULL) {
                    goto exception_unwind;
                }
                PUSH(lasti);
            }
            _PyErr_Fetch(tstate, &exc, &val, &tb);
            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            _PyErr_NormalizeException(tstate, &exc, &val, &tb);
            if (tb != NULL)
                PyException_SetTraceback(val, tb);
            else
                PyException_SetTraceback(val, Py_None);
            Py_XDECREF(tb);
            Py_XDECREF(exc);
            PUSH(val);
            JUMPTO(handler);
            /* Resume normal execution */
            DISPATCH();
        }
    }

exit_unwind:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallTstate(tstate);
    if (frame->is_entry) {
        /* Restore previous cframe and exit */
        tstate->cframe = cframe.previous;
        tstate->cframe->use_tracing = cframe.use_tracing;
        assert(tstate->cframe->current_frame == frame->previous);
        return NULL;
    }
    frame = cframe.current_frame = pop_frame(tstate, frame);

resume_with_error:
    SET_LOCALS_FROM_FRAME();
    goto error;

}

static void
format_missing(PyThreadState *tstate, const char *kind,
               PyCodeObject *co, PyObject *names, PyObject *qualname)
{
    int err;
    Py_ssize_t len = PyList_GET_SIZE(names);
    PyObject *name_str, *comma, *tail, *tmp;

    assert(PyList_CheckExact(names));
    assert(len >= 1);
    /* Deal with the joys of natural language. */
    switch (len) {
    case 1:
        name_str = PyList_GET_ITEM(names, 0);
        Py_INCREF(name_str);
        break;
    case 2:
        name_str = PyUnicode_FromFormat("%U and %U",
                                        PyList_GET_ITEM(names, len - 2),
                                        PyList_GET_ITEM(names, len - 1));
        break;
    default:
        tail = PyUnicode_FromFormat(", %U, and %U",
                                    PyList_GET_ITEM(names, len - 2),
                                    PyList_GET_ITEM(names, len - 1));
        if (tail == NULL)
            return;
        /* Chop off the last two objects in the list. This shouldn't actually
           fail, but we can't be too careful. */
        err = PyList_SetSlice(names, len - 2, len, NULL);
        if (err == -1) {
            Py_DECREF(tail);
            return;
        }
        /* Stitch everything up into a nice comma-separated list. */
        comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            Py_DECREF(tail);
            return;
        }
        tmp = PyUnicode_Join(comma, names);
        Py_DECREF(comma);
        if (tmp == NULL) {
            Py_DECREF(tail);
            return;
        }
        name_str = PyUnicode_Concat(tmp, tail);
        Py_DECREF(tmp);
        Py_DECREF(tail);
        break;
    }
    if (name_str == NULL)
        return;
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() missing %i required %s argument%s: %U",
                  qualname,
                  len,
                  kind,
                  len == 1 ? "" : "s",
                  name_str);
    Py_DECREF(name_str);
}

static void
missing_arguments(PyThreadState *tstate, PyCodeObject *co,
                  Py_ssize_t missing, Py_ssize_t defcount,
                  PyObject **localsplus, PyObject *qualname)
{
    Py_ssize_t i, j = 0;
    Py_ssize_t start, end;
    int positional = (defcount != -1);
    const char *kind = positional ? "positional" : "keyword-only";
    PyObject *missing_names;

    /* Compute the names of the arguments that are missing. */
    missing_names = PyList_New(missing);
    if (missing_names == NULL)
        return;
    if (positional) {
        start = 0;
        end = co->co_argcount - defcount;
    }
    else {
        start = co->co_argcount;
        end = start + co->co_kwonlyargcount;
    }
    for (i = start; i < end; i++) {
        if (localsplus[i] == NULL) {
            PyObject *raw = PyTuple_GET_ITEM(co->co_localsplusnames, i);
            PyObject *name = PyObject_Repr(raw);
            if (name == NULL) {
                Py_DECREF(missing_names);
                return;
            }
            PyList_SET_ITEM(missing_names, j++, name);
        }
    }
    assert(j == missing);
    format_missing(tstate, kind, co, missing_names, qualname);
    Py_DECREF(missing_names);
}

static void
too_many_positional(PyThreadState *tstate, PyCodeObject *co,
                    Py_ssize_t given, PyObject *defaults,
                    PyObject **localsplus, PyObject *qualname)
{
    int plural;
    Py_ssize_t kwonly_given = 0;
    Py_ssize_t i;
    PyObject *sig, *kwonly_sig;
    Py_ssize_t co_argcount = co->co_argcount;

    assert((co->co_flags & CO_VARARGS) == 0);
    /* Count missing keyword-only args. */
    for (i = co_argcount; i < co_argcount + co->co_kwonlyargcount; i++) {
        if (localsplus[i] != NULL) {
            kwonly_given++;
        }
    }
    Py_ssize_t defcount = defaults == NULL ? 0 : PyTuple_GET_SIZE(defaults);
    if (defcount) {
        Py_ssize_t atleast = co_argcount - defcount;
        plural = 1;
        sig = PyUnicode_FromFormat("from %zd to %zd", atleast, co_argcount);
    }
    else {
        plural = (co_argcount != 1);
        sig = PyUnicode_FromFormat("%zd", co_argcount);
    }
    if (sig == NULL)
        return;
    if (kwonly_given) {
        const char *format = " positional argument%s (and %zd keyword-only argument%s)";
        kwonly_sig = PyUnicode_FromFormat(format,
                                          given != 1 ? "s" : "",
                                          kwonly_given,
                                          kwonly_given != 1 ? "s" : "");
        if (kwonly_sig == NULL) {
            Py_DECREF(sig);
            return;
        }
    }
    else {
        /* This will not fail. */
        kwonly_sig = PyUnicode_FromString("");
        assert(kwonly_sig != NULL);
    }
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() takes %U positional argument%s but %zd%U %s given",
                  qualname,
                  sig,
                  plural ? "s" : "",
                  given,
                  kwonly_sig,
                  given == 1 && !kwonly_given ? "was" : "were");
    Py_DECREF(sig);
    Py_DECREF(kwonly_sig);
}

static int
positional_only_passed_as_keyword(PyThreadState *tstate, PyCodeObject *co,
                                  Py_ssize_t kwcount, PyObject* kwnames,
                                  PyObject *qualname)
{
    int posonly_conflicts = 0;
    PyObject* posonly_names = PyList_New(0);

    for(int k=0; k < co->co_posonlyargcount; k++){
        PyObject* posonly_name = PyTuple_GET_ITEM(co->co_localsplusnames, k);

        for (int k2=0; k2<kwcount; k2++){
            /* Compare the pointers first and fallback to PyObject_RichCompareBool*/
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, k2);
            if (kwname == posonly_name){
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
                continue;
            }

            int cmp = PyObject_RichCompareBool(posonly_name, kwname, Py_EQ);

            if ( cmp > 0) {
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
            } else if (cmp < 0) {
                goto fail;
            }

        }
    }
    if (posonly_conflicts) {
        PyObject* comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            goto fail;
        }
        PyObject* error_names = PyUnicode_Join(comma, posonly_names);
        Py_DECREF(comma);
        if (error_names == NULL) {
            goto fail;
        }
        _PyErr_Format(tstate, PyExc_TypeError,
                      "%U() got some positional-only arguments passed"
                      " as keyword arguments: '%U'",
                      qualname, error_names);
        Py_DECREF(error_names);
        goto fail;
    }

    Py_DECREF(posonly_names);
    return 0;

fail:
    Py_XDECREF(posonly_names);
    return 1;

}


static inline unsigned char *
scan_back_to_entry_start(unsigned char *p) {
    for (; (p[0]&128) == 0; p--);
    return p;
}

static inline unsigned char *
skip_to_next_entry(unsigned char *p, unsigned char *end) {
    while (p < end && ((p[0] & 128) == 0)) {
        p++;
    }
    return p;
}


#define MAX_LINEAR_SEARCH 40

static int
get_exception_handler(PyCodeObject *code, int index, int *level, int *handler, int *lasti)
{
    unsigned char *start = (unsigned char *)PyBytes_AS_STRING(code->co_exceptiontable);
    unsigned char *end = start + PyBytes_GET_SIZE(code->co_exceptiontable);
    /* Invariants:
     * start_table == end_table OR
     * start_table points to a legal entry and end_table points
     * beyond the table or to a legal entry that is after index.
     */
    if (end - start > MAX_LINEAR_SEARCH) {
        int offset;
        parse_varint(start, &offset);
        if (offset > index) {
            return 0;
        }
        do {
            unsigned char * mid = start + ((end-start)>>1);
            mid = scan_back_to_entry_start(mid);
            parse_varint(mid, &offset);
            if (offset > index) {
                end = mid;
            }
            else {
                start = mid;
            }

        } while (end - start > MAX_LINEAR_SEARCH);
    }
    unsigned char *scan = start;
    while (scan < end) {
        int start_offset, size;
        scan = parse_varint(scan, &start_offset);
        if (start_offset > index) {
            break;
        }
        scan = parse_varint(scan, &size);
        if (start_offset + size > index) {
            scan = parse_varint(scan, handler);
            int depth_and_lasti;
            parse_varint(scan, &depth_and_lasti);
            *level = depth_and_lasti >> 1;
            *lasti = depth_and_lasti & 1;
            return 1;
        }
        scan = skip_to_next_entry(scan, end);
    }
    return 0;
}

static int
initialize_locals(PyThreadState *tstate, PyFunctionObject *func,
    PyObject **localsplus, PyObject *const *args,
    Py_ssize_t argcount, PyObject *kwnames)
{
    PyCodeObject *co = (PyCodeObject*)func->func_code;
    const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount;

    /* Create a dictionary for keyword parameters (**kwags) */
    PyObject *kwdict;
    Py_ssize_t i;
    if (co->co_flags & CO_VARKEYWORDS) {
        kwdict = PyDict_New();
        if (kwdict == NULL) {
            goto fail_pre_positional;
        }
        i = total_args;
        if (co->co_flags & CO_VARARGS) {
            i++;
        }
        assert(localsplus[i] == NULL);
        localsplus[i] = kwdict;
    }
    else {
        kwdict = NULL;
    }

    /* Copy all positional arguments into local variables */
    Py_ssize_t j, n;
    if (argcount > co->co_argcount) {
        n = co->co_argcount;
    }
    else {
        n = argcount;
    }
    for (j = 0; j < n; j++) {
        PyObject *x = args[j];
        assert(localsplus[j] == NULL);
        localsplus[j] = x;
    }

    /* Pack other positional arguments into the *args argument */
    if (co->co_flags & CO_VARARGS) {
        PyObject *u = NULL;
        u = _PyTuple_FromArraySteal(args + n, argcount - n);
        if (u == NULL) {
            goto fail_post_positional;
        }
        assert(localsplus[total_args] == NULL);
        localsplus[total_args] = u;
    }
    else if (argcount > n) {
        /* Too many postional args. Error is reported later */
        for (j = n; j < argcount; j++) {
            Py_DECREF(args[j]);
        }
    }

    /* Handle keyword arguments */
    if (kwnames != NULL) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < kwcount; i++) {
            PyObject **co_varnames;
            PyObject *keyword = PyTuple_GET_ITEM(kwnames, i);
            PyObject *value = args[i+argcount];
            Py_ssize_t j;

            if (keyword == NULL || !PyUnicode_Check(keyword)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                            "%U() keywords must be strings",
                          func->func_qualname);
                goto kw_fail;
            }

            /* Speed hack: do raw pointer compares. As names are
            normally interned this should almost always hit. */
            co_varnames = ((PyTupleObject *)(co->co_localsplusnames))->ob_item;
            for (j = co->co_posonlyargcount; j < total_args; j++) {
                PyObject *varname = co_varnames[j];
                if (varname == keyword) {
                    goto kw_found;
                }
            }

            /* Slow fallback, just in case */
            for (j = co->co_posonlyargcount; j < total_args; j++) {
                PyObject *varname = co_varnames[j];
                int cmp = PyObject_RichCompareBool( keyword, varname, Py_EQ);
                if (cmp > 0) {
                    goto kw_found;
                }
                else if (cmp < 0) {
                    goto kw_fail;
                }
            }

            assert(j >= total_args);
            if (kwdict == NULL) {

                if (co->co_posonlyargcount
                    && positional_only_passed_as_keyword(tstate, co,
                                                        kwcount, kwnames,
                                                        func->func_qualname))
                {
                    goto kw_fail;
                }

                _PyErr_Format(tstate, PyExc_TypeError,
                            "%U() got an unexpected keyword argument '%S'",
                          func->func_qualname, keyword);
                goto kw_fail;
            }

            if (PyDict_SetItem(kwdict, keyword, value) == -1) {
                goto kw_fail;
            }
            Py_DECREF(value);
            continue;

        kw_fail:
            for (;i < kwcount; i++) {
                PyObject *value = args[i+argcount];
                Py_DECREF(value);
            }
            goto fail_post_args;

        kw_found:
            if (localsplus[j] != NULL) {
                _PyErr_Format(tstate, PyExc_TypeError,
                            "%U() got multiple values for argument '%S'",
                          func->func_qualname, keyword);
                goto kw_fail;
            }
            localsplus[j] = value;
        }
    }

    /* Check the number of positional arguments */
    if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
        too_many_positional(tstate, co, argcount, func->func_defaults, localsplus,
                            func->func_qualname);
        goto fail_post_args;
    }

    /* Add missing positional arguments (copy default values from defs) */
    if (argcount < co->co_argcount) {
        Py_ssize_t defcount = func->func_defaults == NULL ? 0 : PyTuple_GET_SIZE(func->func_defaults);
        Py_ssize_t m = co->co_argcount - defcount;
        Py_ssize_t missing = 0;
        for (i = argcount; i < m; i++) {
            if (localsplus[i] == NULL) {
                missing++;
            }
        }
        if (missing) {
            missing_arguments(tstate, co, missing, defcount, localsplus,
                              func->func_qualname);
            goto fail_post_args;
        }
        if (n > m)
            i = n - m;
        else
            i = 0;
        if (defcount) {
            PyObject **defs = &PyTuple_GET_ITEM(func->func_defaults, 0);
            for (; i < defcount; i++) {
                if (localsplus[m+i] == NULL) {
                    PyObject *def = defs[i];
                    Py_INCREF(def);
                    localsplus[m+i] = def;
                }
            }
        }
    }

    /* Add missing keyword arguments (copy default values from kwdefs) */
    if (co->co_kwonlyargcount > 0) {
        Py_ssize_t missing = 0;
        for (i = co->co_argcount; i < total_args; i++) {
            if (localsplus[i] != NULL)
                continue;
            PyObject *varname = PyTuple_GET_ITEM(co->co_localsplusnames, i);
            if (func->func_kwdefaults != NULL) {
                PyObject *def = PyDict_GetItemWithError(func->func_kwdefaults, varname);
                if (def) {
                    Py_INCREF(def);
                    localsplus[i] = def;
                    continue;
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto fail_post_args;
                }
            }
            missing++;
        }
        if (missing) {
            missing_arguments(tstate, co, missing, -1, localsplus,
                              func->func_qualname);
            goto fail_post_args;
        }
    }
    return 0;

fail_pre_positional:
    for (j = 0; j < argcount; j++) {
        Py_DECREF(args[j]);
    }
    /* fall through */
fail_post_positional:
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (j = argcount; j < argcount+kwcount; j++) {
            Py_DECREF(args[j]);
        }
    }
    /* fall through */
fail_post_args:
    return -1;
}

/* Consumes references to func, locals and all the args */
static _PyInterpreterFrame *
_PyEvalFramePushAndInit(PyThreadState *tstate, PyFunctionObject *func,
                        PyObject *locals, PyObject* const* args,
                        size_t argcount, PyObject *kwnames)
{
    PyCodeObject * code = (PyCodeObject *)func->func_code;
    CALL_STAT_INC(frames_pushed);
    _PyInterpreterFrame *frame = _PyThreadState_PushFrame(tstate, code->co_framesize);
    if (frame == NULL) {
        goto fail;
    }
    _PyFrame_InitializeSpecials(frame, func, locals, code);
    PyObject **localsarray = &frame->localsplus[0];
    for (int i = 0; i < code->co_nlocalsplus; i++) {
        localsarray[i] = NULL;
    }
    if (initialize_locals(tstate, func, localsarray, args, argcount, kwnames)) {
        assert(frame->owner != FRAME_OWNED_BY_GENERATOR);
        _PyEvalFrameClearAndPop(tstate, frame);
        return NULL;
    }
    return frame;
fail:
    /* Consume the references */
    for (size_t i = 0; i < argcount; i++) {
        Py_DECREF(args[i]);
    }
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            Py_DECREF(args[i+argcount]);
        }
    }
    PyErr_NoMemory();
    return NULL;
}

static void
_PyEvalFrameClearAndPop(PyThreadState *tstate, _PyInterpreterFrame * frame)
{
    // Make sure that this is, indeed, the top frame. We can't check this in
    // _PyThreadState_PopFrame, since f_code is already cleared at that point:
    assert((PyObject **)frame + frame->f_code->co_framesize ==
           tstate->datastack_top);
    tstate->recursion_remaining--;
    assert(frame->frame_obj == NULL || frame->frame_obj->f_frame == frame);
    assert(frame->owner == FRAME_OWNED_BY_THREAD);
    _PyFrame_Clear(frame);
    tstate->recursion_remaining++;
    _PyThreadState_PopFrame(tstate, frame);
}

PyObject *
_PyEval_Vector(PyThreadState *tstate, PyFunctionObject *func,
               PyObject *locals,
               PyObject* const* args, size_t argcount,
               PyObject *kwnames)
{
    /* _PyEvalFramePushAndInit consumes the references
     * to func, locals and all its arguments */
    Py_INCREF(func);
    Py_XINCREF(locals);
    for (size_t i = 0; i < argcount; i++) {
        Py_INCREF(args[i]);
    }
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            Py_INCREF(args[i+argcount]);
        }
    }
    _PyInterpreterFrame *frame = _PyEvalFramePushAndInit(
        tstate, func, locals, args, argcount, kwnames);
    if (frame == NULL) {
        return NULL;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_VECTOR);
    PyObject *retval = _PyEval_EvalFrame(tstate, frame, 0);
    assert(
        _PyFrame_GetStackPointer(frame) == _PyFrame_Stackbase(frame) ||
        _PyFrame_GetStackPointer(frame) == frame->localsplus
    );
    _PyEvalFrameClearAndPop(tstate, frame);
    return retval;
}

/* Legacy API */
PyObject *
PyEval_EvalCodeEx(PyObject *_co, PyObject *globals, PyObject *locals,
                  PyObject *const *args, int argcount,
                  PyObject *const *kws, int kwcount,
                  PyObject *const *defs, int defcount,
                  PyObject *kwdefs, PyObject *closure)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *res = NULL;
    PyObject *defaults = _PyTuple_FromArray(defs, defcount);
    if (defaults == NULL) {
        return NULL;
    }
    PyObject *builtins = _PyEval_BuiltinsFromGlobals(tstate, globals); // borrowed ref
    if (builtins == NULL) {
        Py_DECREF(defaults);
        return NULL;
    }
    if (locals == NULL) {
        locals = globals;
    }
    PyObject *kwnames = NULL;
    PyObject *const *allargs;
    PyObject **newargs = NULL;
    PyFunctionObject *func = NULL;
    if (kwcount == 0) {
        allargs = args;
    }
    else {
        kwnames = PyTuple_New(kwcount);
        if (kwnames == NULL) {
            goto fail;
        }
        newargs = PyMem_Malloc(sizeof(PyObject *)*(kwcount+argcount));
        if (newargs == NULL) {
            goto fail;
        }
        for (int i = 0; i < argcount; i++) {
            newargs[i] = args[i];
        }
        for (int i = 0; i < kwcount; i++) {
            Py_INCREF(kws[2*i]);
            PyTuple_SET_ITEM(kwnames, i, kws[2*i]);
            newargs[argcount+i] = kws[2*i+1];
        }
        allargs = newargs;
    }
    for (int i = 0; i < kwcount; i++) {
        Py_INCREF(kws[2*i]);
        PyTuple_SET_ITEM(kwnames, i, kws[2*i]);
    }
    PyFrameConstructor constr = {
        .fc_globals = globals,
        .fc_builtins = builtins,
        .fc_name = ((PyCodeObject *)_co)->co_name,
        .fc_qualname = ((PyCodeObject *)_co)->co_name,
        .fc_code = _co,
        .fc_defaults = defaults,
        .fc_kwdefaults = kwdefs,
        .fc_closure = closure
    };
    func = _PyFunction_FromConstructor(&constr);
    if (func == NULL) {
        goto fail;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_LEGACY);
    res = _PyEval_Vector(tstate, func, locals,
                         allargs, argcount,
                         kwnames);
fail:
    Py_XDECREF(func);
    Py_XDECREF(kwnames);
    PyMem_Free(newargs);
    Py_DECREF(defaults);
    return res;
}


/* Logic for the raise statement (too complicated for inlining).
   This *consumes* a reference count to each of its arguments. */
static int
do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause)
{
    PyObject *type = NULL, *value = NULL;

    if (exc == NULL) {
        /* Reraise */
        _PyErr_StackItem *exc_info = _PyErr_GetTopmostException(tstate);
        value = exc_info->exc_value;
        if (Py_IsNone(value) || value == NULL) {
            _PyErr_SetString(tstate, PyExc_RuntimeError,
                             "No active exception to reraise");
            return 0;
        }
        assert(PyExceptionInstance_Check(value));
        type = PyExceptionInstance_Class(value);
        Py_XINCREF(type);
        Py_XINCREF(value);
        PyObject *tb = PyException_GetTraceback(value); /* new ref */
        _PyErr_Restore(tstate, type, value, tb);
        return 1;
    }

    /* We support the following forms of raise:
       raise
       raise <instance>
       raise <type> */

    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = _PyObject_CallNoArgs(exc);
        if (value == NULL)
            goto raise_error;
        if (!PyExceptionInstance_Check(value)) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "calling %R should have returned an instance of "
                          "BaseException, not %R",
                          type, Py_TYPE(value));
             goto raise_error;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        Py_DECREF(exc);
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "exceptions must derive from BaseException");
        goto raise_error;
    }

    assert(type != NULL);
    assert(value != NULL);

    if (cause) {
        PyObject *fixed_cause;
        if (PyExceptionClass_Check(cause)) {
            fixed_cause = _PyObject_CallNoArgs(cause);
            if (fixed_cause == NULL)
                goto raise_error;
            Py_DECREF(cause);
        }
        else if (PyExceptionInstance_Check(cause)) {
            fixed_cause = cause;
        }
        else if (Py_IsNone(cause)) {
            Py_DECREF(cause);
            fixed_cause = NULL;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "exception causes must derive from "
                             "BaseException");
            goto raise_error;
        }
        PyException_SetCause(value, fixed_cause);
    }

    _PyErr_SetObject(tstate, type, value);
    /* _PyErr_SetObject incref's its arguments */
    Py_DECREF(value);
    Py_DECREF(type);
    return 0;

raise_error:
    Py_XDECREF(value);
    Py_XDECREF(type);
    Py_XDECREF(cause);
    return 0;
}

/* Logic for matching an exception in an except* clause (too
   complicated for inlining).
*/

static int
exception_group_match(PyObject* exc_value, PyObject *match_type,
                      PyObject **match, PyObject **rest)
{
    if (Py_IsNone(exc_value)) {
        *match = Py_NewRef(Py_None);
        *rest = Py_NewRef(Py_None);
        return 0;
    }
    assert(PyExceptionInstance_Check(exc_value));

    if (PyErr_GivenExceptionMatches(exc_value, match_type)) {
        /* Full match of exc itself */
        bool is_eg = _PyBaseExceptionGroup_Check(exc_value);
        if (is_eg) {
            *match = Py_NewRef(exc_value);
        }
        else {
            /* naked exception - wrap it */
            PyObject *excs = PyTuple_Pack(1, exc_value);
            if (excs == NULL) {
                return -1;
            }
            PyObject *wrapped = _PyExc_CreateExceptionGroup("", excs);
            Py_DECREF(excs);
            if (wrapped == NULL) {
                return -1;
            }
            *match = wrapped;
        }
        *rest = Py_NewRef(Py_None);
        return 0;
    }

    /* exc_value does not match match_type.
     * Check for partial match if it's an exception group.
     */
    if (_PyBaseExceptionGroup_Check(exc_value)) {
        PyObject *pair = PyObject_CallMethod(exc_value, "split", "(O)",
                                             match_type);
        if (pair == NULL) {
            return -1;
        }
        assert(PyTuple_CheckExact(pair));
        assert(PyTuple_GET_SIZE(pair) == 2);
        *match = Py_NewRef(PyTuple_GET_ITEM(pair, 0));
        *rest = Py_NewRef(PyTuple_GET_ITEM(pair, 1));
        Py_DECREF(pair);
        return 0;
    }
    /* no match */
    *match = Py_NewRef(Py_None);
    *rest = Py_NewRef(Py_None);
    return 0;
}

/* Iterate v argcnt times and store the results on the stack (via decreasing
   sp).  Return 1 for success, 0 if error.

   If argcntafter == -1, do a simple unpack. If it is >= 0, do an unpack
   with a variable target.
*/

static int
unpack_iterable(PyThreadState *tstate, PyObject *v,
                int argcnt, int argcntafter, PyObject **sp)
{
    int i = 0, j = 0;
    Py_ssize_t ll = 0;
    PyObject *it;  /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */

    assert(v != NULL);

    it = PyObject_GetIter(v);
    if (it == NULL) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
            Py_TYPE(v)->tp_iter == NULL && !PySequence_Check(v))
        {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "cannot unpack non-iterable %.200s object",
                          Py_TYPE(v)->tp_name);
        }
        return 0;
    }

    for (; i < argcnt; i++) {
        w = PyIter_Next(it);
        if (w == NULL) {
            /* Iterator done, via error or exhaustion. */
            if (!_PyErr_Occurred(tstate)) {
                if (argcntafter == -1) {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected %d, got %d)",
                                  argcnt, i);
                }
                else {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected at least %d, got %d)",
                                  argcnt + argcntafter, i);
                }
            }
            goto Error;
        }
        *--sp = w;
    }

    if (argcntafter == -1) {
        /* We better have exhausted the iterator now. */
        w = PyIter_Next(it);
        if (w == NULL) {
            if (_PyErr_Occurred(tstate))
                goto Error;
            Py_DECREF(it);
            return 1;
        }
        Py_DECREF(w);
        _PyErr_Format(tstate, PyExc_ValueError,
                      "too many values to unpack (expected %d)",
                      argcnt);
        goto Error;
    }

    l = PySequence_List(it);
    if (l == NULL)
        goto Error;
    *--sp = l;
    i++;

    ll = PyList_GET_SIZE(l);
    if (ll < argcntafter) {
        _PyErr_Format(tstate, PyExc_ValueError,
            "not enough values to unpack (expected at least %d, got %zd)",
            argcnt + argcntafter, argcnt + ll);
        goto Error;
    }

    /* Pop the "after-variable" args off the list. */
    for (j = argcntafter; j > 0; j--, i++) {
        *--sp = PyList_GET_ITEM(l, ll - j);
    }
    /* Resize the list. */
    Py_SET_SIZE(l, ll - argcntafter);
    Py_DECREF(it);
    return 1;

Error:
    for (; i > 0; i--, sp++)
        Py_DECREF(*sp);
    Py_XDECREF(it);
    return 0;
}

static void
call_exc_trace(Py_tracefunc func, PyObject *self,
               PyThreadState *tstate,
               _PyInterpreterFrame *f)
{
    PyObject *type, *value, *traceback, *orig_traceback, *arg;
    int err;
    _PyErr_Fetch(tstate, &type, &value, &orig_traceback);
    if (value == NULL) {
        value = Py_None;
        Py_INCREF(value);
    }
    _PyErr_NormalizeException(tstate, &type, &value, &orig_traceback);
    traceback = (orig_traceback != NULL) ? orig_traceback : Py_None;
    arg = PyTuple_Pack(3, type, value, traceback);
    if (arg == NULL) {
        _PyErr_Restore(tstate, type, value, orig_traceback);
        return;
    }
    err = call_trace(func, self, tstate, f, PyTrace_EXCEPTION, arg);
    Py_DECREF(arg);
    if (err == 0) {
        _PyErr_Restore(tstate, type, value, orig_traceback);
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(orig_traceback);
    }
}

static int
call_trace_protected(Py_tracefunc func, PyObject *obj,
                     PyThreadState *tstate, _PyInterpreterFrame *frame,
                     int what, PyObject *arg)
{
    PyObject *type, *value, *traceback;
    int err;
    _PyErr_Fetch(tstate, &type, &value, &traceback);
    err = call_trace(func, obj, tstate, frame, what, arg);
    if (err == 0)
    {
        _PyErr_Restore(tstate, type, value, traceback);
        return 0;
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return -1;
    }
}

static void
initialize_trace_info(PyTraceInfo *trace_info, _PyInterpreterFrame *frame)
{
    PyCodeObject *code = frame->f_code;
    if (trace_info->code != code) {
        trace_info->code = code;
        _PyCode_InitAddressRange(code, &trace_info->bounds);
    }
}

void
PyThreadState_EnterTracing(PyThreadState *tstate)
{
    tstate->tracing++;
    tstate->cframe->use_tracing = 0;
}

void
PyThreadState_LeaveTracing(PyThreadState *tstate)
{
    assert(tstate->tracing > 0 && tstate->cframe->use_tracing == 0);
    tstate->tracing--;
    _PyThreadState_UpdateTracingState(tstate);
}

static int
call_trace(Py_tracefunc func, PyObject *obj,
           PyThreadState *tstate, _PyInterpreterFrame *frame,
           int what, PyObject *arg)
{
    int result;
    if (tstate->tracing) {
        return 0;
    }
    PyFrameObject *f = _PyFrame_GetFrameObject(frame);
    if (f == NULL) {
        return -1;
    }
    int old_what = tstate->tracing_what;
    tstate->tracing_what = what;
    PyThreadState_EnterTracing(tstate);
    assert(_PyInterpreterFrame_LASTI(frame) >= 0);
    if (_PyCode_InitLineArray(frame->f_code)) {
        return -1;
    }
    f->f_lineno = _PyCode_LineNumberFromArray(frame->f_code, _PyInterpreterFrame_LASTI(frame));
    result = func(obj, f, what, arg);
    f->f_lineno = 0;
    PyThreadState_LeaveTracing(tstate);
    tstate->tracing_what = old_what;
    return result;
}

PyObject*
_PyEval_CallTracing(PyObject *func, PyObject *args)
{
    // Save and disable tracing
    PyThreadState *tstate = _PyThreadState_GET();
    int save_tracing = tstate->tracing;
    int save_use_tracing = tstate->cframe->use_tracing;
    tstate->tracing = 0;

    // Call the tracing function
    PyObject *result = PyObject_Call(func, args, NULL);

    // Restore tracing
    tstate->tracing = save_tracing;
    tstate->cframe->use_tracing = save_use_tracing;
    return result;
}

/* See Objects/lnotab_notes.txt for a description of how tracing works. */
static int
maybe_call_line_trace(Py_tracefunc func, PyObject *obj,
                      PyThreadState *tstate, _PyInterpreterFrame *frame, int instr_prev)
{
    int result = 0;

    /* If the last instruction falls at the start of a line or if it
       represents a jump backwards, update the frame's line number and
       then call the trace function if we're tracing source lines.
    */
    if (_PyCode_InitLineArray(frame->f_code)) {
        return -1;
    }
    int lastline;
    if (instr_prev <= frame->f_code->_co_firsttraceable) {
        lastline = -1;
    }
    else {
        lastline = _PyCode_LineNumberFromArray(frame->f_code, instr_prev);
    }
    int line = _PyCode_LineNumberFromArray(frame->f_code, _PyInterpreterFrame_LASTI(frame));
    PyFrameObject *f = _PyFrame_GetFrameObject(frame);
    if (f == NULL) {
        return -1;
    }
    if (line != -1 && f->f_trace_lines) {
        /* Trace backward edges (except in 'yield from') or if line number has changed */
        int trace = line != lastline ||
            (_PyInterpreterFrame_LASTI(frame) < instr_prev &&
             // SEND has no quickened forms, so no need to use _PyOpcode_Deopt
             // here:
             _Py_OPCODE(*frame->prev_instr) != SEND);
        if (trace) {
            result = call_trace(func, obj, tstate, frame, PyTrace_LINE, Py_None);
        }
    }
    /* Always emit an opcode event if we're tracing all opcodes. */
    if (f->f_trace_opcodes) {
        result = call_trace(func, obj, tstate, frame, PyTrace_OPCODE, Py_None);
    }
    return result;
}

int
_PyEval_SetProfile(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    assert(is_tstate_valid(tstate));
    /* The caller must hold the GIL */
    assert(PyGILState_Check());

    static int reentrant = 0;
    if (reentrant) {
        _PyErr_SetString(tstate, PyExc_RuntimeError, "Cannot install a profile function "
                         "while another profile function is being installed");
        reentrant = 0;
        return -1;
    }
    reentrant = 1;

    /* Call _PySys_Audit() in the context of the current thread state,
       even if tstate is not the current thread state. */
    PyThreadState *current_tstate = _PyThreadState_GET();
    if (_PySys_Audit(current_tstate, "sys.setprofile", NULL) < 0) {
        reentrant = 0;
        return -1;
    }

    PyObject *profileobj = tstate->c_profileobj;

    tstate->c_profilefunc = NULL;
    tstate->c_profileobj = NULL;
    /* Must make sure that tracing is not ignored if 'profileobj' is freed */
    _PyThreadState_UpdateTracingState(tstate);
    Py_XDECREF(profileobj);

    Py_XINCREF(arg);
    tstate->c_profileobj = arg;
    tstate->c_profilefunc = func;

    /* Flag that tracing or profiling is turned on */
    _PyThreadState_UpdateTracingState(tstate);
    reentrant = 0;
    return 0;
}

void
PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetProfile(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        _PyErr_WriteUnraisableMsg("in PyEval_SetProfile", NULL);
    }
}

int
_PyEval_SetTrace(PyThreadState *tstate, Py_tracefunc func, PyObject *arg)
{
    assert(is_tstate_valid(tstate));
    /* The caller must hold the GIL */
    assert(PyGILState_Check());

    static int reentrant = 0;

    if (reentrant) {
        _PyErr_SetString(tstate, PyExc_RuntimeError, "Cannot install a trace function "
                         "while another trace function is being installed");
        reentrant = 0;
        return -1;
    }
    reentrant = 1;

    /* Call _PySys_Audit() in the context of the current thread state,
       even if tstate is not the current thread state. */
    PyThreadState *current_tstate = _PyThreadState_GET();
    if (_PySys_Audit(current_tstate, "sys.settrace", NULL) < 0) {
        reentrant = 0;
        return -1;
    }

    PyObject *traceobj = tstate->c_traceobj;

    tstate->c_tracefunc = NULL;
    tstate->c_traceobj = NULL;
    /* Must make sure that profiling is not ignored if 'traceobj' is freed */
    _PyThreadState_UpdateTracingState(tstate);
    Py_XINCREF(arg);
    Py_XDECREF(traceobj);
    tstate->c_traceobj = arg;
    tstate->c_tracefunc = func;

    /* Flag that tracing or profiling is turned on */
    _PyThreadState_UpdateTracingState(tstate);

    reentrant = 0;
    return 0;
}

void
PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetTrace(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        _PyErr_WriteUnraisableMsg("in PyEval_SetTrace", NULL);
    }
}


int
_PyEval_SetCoroutineOriginTrackingDepth(int depth)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (depth < 0) {
        _PyErr_SetString(tstate, PyExc_ValueError, "depth must be >= 0");
        return -1;
    }
    tstate->coroutine_origin_tracking_depth = depth;
    return 0;
}


int
_PyEval_GetCoroutineOriginTrackingDepth(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->coroutine_origin_tracking_depth;
}

int
_PyEval_SetAsyncGenFirstiter(PyObject *firstiter)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_firstiter", NULL) < 0) {
        return -1;
    }

    Py_XINCREF(firstiter);
    Py_XSETREF(tstate->async_gen_firstiter, firstiter);
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFirstiter(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_firstiter;
}

int
_PyEval_SetAsyncGenFinalizer(PyObject *finalizer)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_finalizer", NULL) < 0) {
        return -1;
    }

    Py_XINCREF(finalizer);
    Py_XSETREF(tstate->async_gen_finalizer, finalizer);
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFinalizer(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_finalizer;
}

_PyInterpreterFrame *
_PyEval_GetFrame(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->cframe->current_frame;
}

PyFrameObject *
PyEval_GetFrame(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->cframe->current_frame == NULL) {
        return NULL;
    }
    PyFrameObject *f = _PyFrame_GetFrameObject(tstate->cframe->current_frame);
    if (f == NULL) {
        PyErr_Clear();
    }
    return f;
}

PyObject *
_PyEval_GetBuiltins(PyThreadState *tstate)
{
    _PyInterpreterFrame *frame = tstate->cframe->current_frame;
    if (frame != NULL) {
        return frame->f_builtins;
    }
    return tstate->interp->builtins;
}

PyObject *
PyEval_GetBuiltins(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_GetBuiltins(tstate);
}

/* Convenience function to get a builtin from its name */
PyObject *
_PyEval_GetBuiltin(PyObject *name)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *attr = PyDict_GetItemWithError(PyEval_GetBuiltins(), name);
    if (attr) {
        Py_INCREF(attr);
    }
    else if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetObject(tstate, PyExc_AttributeError, name);
    }
    return attr;
}

PyObject *
_PyEval_GetBuiltinId(_Py_Identifier *name)
{
    return _PyEval_GetBuiltin(_PyUnicode_FromId(name));
}

PyObject *
PyEval_GetLocals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
     _PyInterpreterFrame *current_frame = tstate->cframe->current_frame;
    if (current_frame == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    if (_PyFrame_FastToLocalsWithError(current_frame) < 0) {
        return NULL;
    }

    PyObject *locals = current_frame->f_locals;
    assert(locals != NULL);
    return locals;
}

PyObject *
PyEval_GetGlobals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *current_frame = tstate->cframe->current_frame;
    if (current_frame == NULL) {
        return NULL;
    }
    return current_frame->f_globals;
}

int
PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *current_frame = tstate->cframe->current_frame;
    int result = cf->cf_flags != 0;

    if (current_frame != NULL) {
        const int codeflags = current_frame->f_code->co_flags;
        const int compilerflags = codeflags & PyCF_MASK;
        if (compilerflags) {
            result = 1;
            cf->cf_flags |= compilerflags;
        }
    }
    return result;
}


const char *
PyEval_GetFuncName(PyObject *func)
{
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyFunction_Check(func))
        return PyUnicode_AsUTF8(((PyFunctionObject*)func)->func_name);
    else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else
        return Py_TYPE(func)->tp_name;
}

const char *
PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else
        return " object";
}

#define C_TRACE(x, call) \
if (use_tracing && tstate->c_profilefunc) { \
    if (call_trace(tstate->c_profilefunc, tstate->c_profileobj, \
        tstate, tstate->cframe->current_frame, \
        PyTrace_C_CALL, func)) { \
        x = NULL; \
    } \
    else { \
        x = call; \
        if (tstate->c_profilefunc != NULL) { \
            if (x == NULL) { \
                call_trace_protected(tstate->c_profilefunc, \
                    tstate->c_profileobj, \
                    tstate, tstate->cframe->current_frame, \
                    PyTrace_C_EXCEPTION, func); \
                /* XXX should pass (type, value, tb) */ \
            } else { \
                if (call_trace(tstate->c_profilefunc, \
                    tstate->c_profileobj, \
                    tstate, tstate->cframe->current_frame, \
                    PyTrace_C_RETURN, func)) { \
                    Py_DECREF(x); \
                    x = NULL; \
                } \
            } \
        } \
    } \
} else { \
    x = call; \
    }


static PyObject *
trace_call_function(PyThreadState *tstate,
                    PyObject *func,
                    PyObject **args, Py_ssize_t nargs,
                    PyObject *kwnames)
{
    int use_tracing = 1;
    PyObject *x;
    if (PyCFunction_CheckExact(func) || PyCMethod_CheckExact(func)) {
        C_TRACE(x, PyObject_Vectorcall(func, args, nargs, kwnames));
        return x;
    }
    else if (Py_IS_TYPE(func, &PyMethodDescr_Type) && nargs > 0) {
        /* We need to create a temporary bound method as argument
           for profiling.

           If nargs == 0, then this cannot work because we have no
           "self". In any case, the call itself would raise
           TypeError (foo needs an argument), so we just skip
           profiling. */
        PyObject *self = args[0];
        func = Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
        if (func == NULL) {
            return NULL;
        }
        C_TRACE(x, PyObject_Vectorcall(func,
                                        args+1, nargs-1,
                                        kwnames));
        Py_DECREF(func);
        return x;
    }
    return PyObject_Vectorcall(func, args, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
}

static PyObject *
do_call_core(PyThreadState *tstate,
             PyObject *func,
             PyObject *callargs,
             PyObject *kwdict,
             int use_tracing
            )
{
    PyObject *result;
    if (PyCFunction_CheckExact(func) || PyCMethod_CheckExact(func)) {
        C_TRACE(result, PyObject_Call(func, callargs, kwdict));
        return result;
    }
    else if (Py_IS_TYPE(func, &PyMethodDescr_Type)) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
        if (nargs > 0 && use_tracing) {
            /* We need to create a temporary bound method as argument
               for profiling.

               If nargs == 0, then this cannot work because we have no
               "self". In any case, the call itself would raise
               TypeError (foo needs an argument), so we just skip
               profiling. */
            PyObject *self = PyTuple_GET_ITEM(callargs, 0);
            func = Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
            if (func == NULL) {
                return NULL;
            }

            C_TRACE(result, _PyObject_FastCallDictTstate(
                                    tstate, func,
                                    &_PyTuple_ITEMS(callargs)[1],
                                    nargs - 1,
                                    kwdict));
            Py_DECREF(func);
            return result;
        }
    }
    EVAL_CALL_STAT_INC_IF_FUNCTION(EVAL_CALL_FUNCTION_EX, func);
    return PyObject_Call(func, callargs, kwdict);
}

/* Extract a slice index from a PyLong or an object with the
   nb_index slot defined, and store in *pi.
   Silently reduce values larger than PY_SSIZE_T_MAX to PY_SSIZE_T_MAX,
   and silently boost values less than PY_SSIZE_T_MIN to PY_SSIZE_T_MIN.
   Return 0 on error, 1 on success.
*/
int
_PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (!Py_IsNone(v)) {
        Py_ssize_t x;
        if (_PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && _PyErr_Occurred(tstate))
                return 0;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "slice indices must be integers or "
                             "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}

int
_PyEval_SliceIndexNotNone(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Py_ssize_t x;
    if (_PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && _PyErr_Occurred(tstate))
            return 0;
    }
    else {
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "slice indices must be integers or "
                         "have an __index__ method");
        return 0;
    }
    *pi = x;
    return 1;
}

static PyObject *
import_name(PyThreadState *tstate, _PyInterpreterFrame *frame,
            PyObject *name, PyObject *fromlist, PyObject *level)
{
    PyObject *import_func, *res;
    PyObject* stack[5];

    import_func = _PyDict_GetItemWithError(frame->f_builtins, &_Py_ID(__import__));
    if (import_func == NULL) {
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_ImportError, "__import__ not found");
        }
        return NULL;
    }
    PyObject *locals = frame->f_locals;
    /* Fast path for not overloaded __import__. */
    if (import_func == tstate->interp->import_func) {
        int ilevel = _PyLong_AsInt(level);
        if (ilevel == -1 && _PyErr_Occurred(tstate)) {
            return NULL;
        }
        res = PyImport_ImportModuleLevelObject(
                        name,
                        frame->f_globals,
                        locals == NULL ? Py_None :locals,
                        fromlist,
                        ilevel);
        return res;
    }

    Py_INCREF(import_func);

    stack[0] = name;
    stack[1] = frame->f_globals;
    stack[2] = locals == NULL ? Py_None : locals;
    stack[3] = fromlist;
    stack[4] = level;
    res = _PyObject_FastCall(import_func, stack, 5);
    Py_DECREF(import_func);
    return res;
}

static PyObject *
import_from(PyThreadState *tstate, PyObject *v, PyObject *name)
{
    PyObject *x;
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

    if (_PyObject_LookupAttr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    pkgname = PyObject_GetAttr(v, &_Py_ID(__name__));
    if (pkgname == NULL) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == NULL) {
        Py_DECREF(pkgname);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !_PyErr_Occurred(tstate)) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
 error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == NULL) {
        pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
        if (pkgname_or_unknown == NULL) {
            Py_XDECREF(pkgpath);
            return NULL;
        }
    } else {
        pkgname_or_unknown = pkgname;
    }

    if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
        _PyErr_Clear(tstate);
        errmsg = PyUnicode_FromFormat(
            "cannot import name %R from %R (unknown location)",
            name, pkgname_or_unknown
        );
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
        PyObject *spec = PyObject_GetAttr(v, &_Py_ID(__spec__));
        const char *fmt =
            _PyModuleSpec_IsInitializing(spec) ?
            "cannot import name %R from partially initialized module %R "
            "(most likely due to a circular import) (%S)" :
            "cannot import name %R from %R (%S)";
        Py_XDECREF(spec);

        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }

    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
}

static int
import_all_from(PyThreadState *tstate, PyObject *locals, PyObject *v)
{
    PyObject *all, *dict, *name, *value;
    int skip_leading_underscores = 0;
    int pos, err;

    if (_PyObject_LookupAttr(v, &_Py_ID(__all__), &all) < 0) {
        return -1; /* Unexpected error */
    }
    if (all == NULL) {
        if (_PyObject_LookupAttr(v, &_Py_ID(__dict__), &dict) < 0) {
            return -1;
        }
        if (dict == NULL) {
            _PyErr_SetString(tstate, PyExc_ImportError,
                    "from-import-* object has no __dict__ and no __all__");
            return -1;
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (all == NULL)
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0, err = 0; ; pos++) {
        name = PySequence_GetItem(all, pos);
        if (name == NULL) {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_IndexError)) {
                err = -1;
            }
            else {
                _PyErr_Clear(tstate);
            }
            break;
        }
        if (!PyUnicode_Check(name)) {
            PyObject *modname = PyObject_GetAttr(v, &_Py_ID(__name__));
            if (modname == NULL) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (!PyUnicode_Check(modname)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "module __name__ must be a string, not %.100s",
                              Py_TYPE(modname)->tp_name);
            }
            else {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "%s in %U.%s must be str, not %.100s",
                              skip_leading_underscores ? "Key" : "Item",
                              modname,
                              skip_leading_underscores ? "__dict__" : "__all__",
                              Py_TYPE(name)->tp_name);
            }
            Py_DECREF(modname);
            Py_DECREF(name);
            err = -1;
            break;
        }
        if (skip_leading_underscores) {
            if (PyUnicode_READY(name) == -1) {
                Py_DECREF(name);
                err = -1;
                break;
            }
            if (PyUnicode_READ_CHAR(name, 0) == '_') {
                Py_DECREF(name);
                continue;
            }
        }
        value = PyObject_GetAttr(v, name);
        if (value == NULL)
            err = -1;
        else if (PyDict_CheckExact(locals))
            err = PyDict_SetItem(locals, name, value);
        else
            err = PyObject_SetItem(locals, name, value);
        Py_DECREF(name);
        Py_XDECREF(value);
        if (err != 0)
            break;
    }
    Py_DECREF(all);
    return err;
}

#define CANNOT_CATCH_MSG "catching classes that do not inherit from "\
                         "BaseException is not allowed"

#define CANNOT_EXCEPT_STAR_EG "catching ExceptionGroup with except* "\
                              "is not allowed. Use except instead."

static int
check_except_type_valid(PyThreadState *tstate, PyObject* right)
{
    if (PyTuple_Check(right)) {
        Py_ssize_t i, length;
        length = PyTuple_GET_SIZE(right);
        for (i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            if (!PyExceptionClass_Check(exc)) {
                _PyErr_SetString(tstate, PyExc_TypeError,
                    CANNOT_CATCH_MSG);
                return -1;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(right)) {
            _PyErr_SetString(tstate, PyExc_TypeError,
                CANNOT_CATCH_MSG);
            return -1;
        }
    }
    return 0;
}

static int
check_except_star_type_valid(PyThreadState *tstate, PyObject* right)
{
    if (check_except_type_valid(tstate, right) < 0) {
        return -1;
    }

    /* reject except *ExceptionGroup */

    int is_subclass = 0;
    if (PyTuple_Check(right)) {
        Py_ssize_t length = PyTuple_GET_SIZE(right);
        for (Py_ssize_t i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            is_subclass = PyObject_IsSubclass(exc, PyExc_BaseExceptionGroup);
            if (is_subclass < 0) {
                return -1;
            }
            if (is_subclass) {
                break;
            }
        }
    }
    else {
        is_subclass = PyObject_IsSubclass(right, PyExc_BaseExceptionGroup);
        if (is_subclass < 0) {
            return -1;
        }
    }
    if (is_subclass) {
        _PyErr_SetString(tstate, PyExc_TypeError,
            CANNOT_EXCEPT_STAR_EG);
            return -1;
    }
    return 0;
}

static int
check_args_iterable(PyThreadState *tstate, PyObject *func, PyObject *args)
{
    if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
        /* check_args_iterable() may be called with a live exception:
         * clear it to prevent calling _PyObject_FunctionStr() with an
         * exception set. */
        _PyErr_Clear(tstate);
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%U argument after * must be an iterable, not %.200s",
                          funcstr, Py_TYPE(args)->tp_name);
            Py_DECREF(funcstr);
        }
        return -1;
    }
    return 0;
}

static void
format_kwargs_error(PyThreadState *tstate, PyObject *func, PyObject *kwargs)
{
    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Clear(tstate);
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            _PyErr_Format(
                tstate, PyExc_TypeError,
                "%U argument after ** must be a mapping, not %.200s",
                funcstr, Py_TYPE(kwargs)->tp_name);
            Py_DECREF(funcstr);
        }
    }
    else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        _PyErr_Fetch(tstate, &exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            _PyErr_Clear(tstate);
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(val, 0);
                _PyErr_Format(
                    tstate, PyExc_TypeError,
                    "%U got multiple values for keyword argument '%S'",
                    funcstr, key);
                Py_DECREF(funcstr);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            _PyErr_Restore(tstate, exc, val, tb);
        }
    }
}

static void
format_exc_check_arg(PyThreadState *tstate, PyObject *exc,
                     const char *format_str, PyObject *obj)
{
    const char *obj_str;

    if (!obj)
        return;

    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;

    _PyErr_Format(tstate, exc, format_str, obj_str);

    if (exc == PyExc_NameError) {
        // Include the name in the NameError exceptions to offer suggestions later.
        PyObject *type, *value, *traceback;
        PyErr_Fetch(&type, &value, &traceback);
        PyErr_NormalizeException(&type, &value, &traceback);
        if (PyErr_GivenExceptionMatches(value, PyExc_NameError)) {
            PyNameErrorObject* exc = (PyNameErrorObject*) value;
            if (exc->name == NULL) {
                // We do not care if this fails because we are going to restore the
                // NameError anyway.
                (void)PyObject_SetAttr(value, &_Py_ID(name), obj);
            }
        }
        PyErr_Restore(type, value, traceback);
    }
}

static void
format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg)
{
    PyObject *name;
    /* Don't stomp existing exception */
    if (_PyErr_Occurred(tstate))
        return;
    name = PyTuple_GET_ITEM(co->co_localsplusnames, oparg);
    if (oparg < co->co_nplaincellvars + co->co_nlocals) {
        format_exc_check_arg(tstate, PyExc_UnboundLocalError,
                             UNBOUNDLOCAL_ERROR_MSG, name);
    } else {
        format_exc_check_arg(tstate, PyExc_NameError,
                             UNBOUNDFREE_ERROR_MSG, name);
    }
}

static void
format_awaitable_error(PyThreadState *tstate, PyTypeObject *type, int oparg)
{
    if (type->tp_as_async == NULL || type->tp_as_async->am_await == NULL) {
        if (oparg == 1) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aenter__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
        else if (oparg == 2) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aexit__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
    }
}

#ifdef Py_STATS

static PyObject *
getarray(uint64_t a[256])
{
    int i;
    PyObject *l = PyList_New(256);
    if (l == NULL) return NULL;
    for (i = 0; i < 256; i++) {
        PyObject *x = PyLong_FromUnsignedLongLong(a[i]);
        if (x == NULL) {
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(l, i, x);
    }
    for (i = 0; i < 256; i++)
        a[i] = 0;
    return l;
}

PyObject *
_Py_GetDXProfile(PyObject *self, PyObject *args)
{
    int i;
    PyObject *l = PyList_New(257);
    if (l == NULL) return NULL;
    for (i = 0; i < 256; i++) {
        PyObject *x = getarray(_py_stats_struct.opcode_stats[i].pair_count);
        if (x == NULL) {
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(l, i, x);
    }
    PyObject *counts = PyList_New(256);
    if (counts == NULL) {
        Py_DECREF(l);
        return NULL;
    }
    for (i = 0; i < 256; i++) {
        PyObject *x = PyLong_FromUnsignedLongLong(
            _py_stats_struct.opcode_stats[i].execution_count);
        if (x == NULL) {
            Py_DECREF(counts);
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(counts, i, x);
    }
    PyList_SET_ITEM(l, 256, counts);
    return l;
}

#endif

Py_ssize_t
_PyEval_RequestCodeExtraIndex(freefunc free)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    Py_ssize_t new_index;

    if (interp->co_extra_user_count == MAX_CO_EXTRA_USERS - 1) {
        return -1;
    }
    new_index = interp->co_extra_user_count++;
    interp->co_extra_freefuncs[new_index] = free;
    return new_index;
}

static void
dtrace_function_entry(_PyInterpreterFrame *frame)
{
    const char *filename;
    const char *funcname;
    int lineno;

    PyCodeObject *code = frame->f_code;
    filename = PyUnicode_AsUTF8(code->co_filename);
    funcname = PyUnicode_AsUTF8(code->co_name);
    lineno = _PyInterpreterFrame_GetLine(frame);

    PyDTrace_FUNCTION_ENTRY(filename, funcname, lineno);
}

static void
dtrace_function_return(_PyInterpreterFrame *frame)
{
    const char *filename;
    const char *funcname;
    int lineno;

    PyCodeObject *code = frame->f_code;
    filename = PyUnicode_AsUTF8(code->co_filename);
    funcname = PyUnicode_AsUTF8(code->co_name);
    lineno = _PyInterpreterFrame_GetLine(frame);

    PyDTrace_FUNCTION_RETURN(filename, funcname, lineno);
}

/* DTrace equivalent of maybe_call_line_trace. */
static void
maybe_dtrace_line(_PyInterpreterFrame *frame,
                  PyTraceInfo *trace_info,
                  int instr_prev)
{
    const char *co_filename, *co_name;

    /* If the last instruction executed isn't in the current
       instruction window, reset the window.
    */
    initialize_trace_info(trace_info, frame);
    int lastline = _PyCode_CheckLineNumber(instr_prev*sizeof(_Py_CODEUNIT), &trace_info->bounds);
    int addr = _PyInterpreterFrame_LASTI(frame) * sizeof(_Py_CODEUNIT);
    int line = _PyCode_CheckLineNumber(addr, &trace_info->bounds);
    if (line != -1) {
        /* Trace backward edges or first instruction of a new line */
        if (_PyInterpreterFrame_LASTI(frame) < instr_prev ||
            (line != lastline && addr == trace_info->bounds.ar_start))
        {
            co_filename = PyUnicode_AsUTF8(frame->f_code->co_filename);
            if (!co_filename) {
                co_filename = "?";
            }
            co_name = PyUnicode_AsUTF8(frame->f_code->co_name);
            if (!co_name) {
                co_name = "?";
            }
            PyDTrace_LINE(co_filename, co_name, line);
        }
    }
}

/* Implement Py_EnterRecursiveCall() and Py_LeaveRecursiveCall() as functions
   for the limited API. */

#undef Py_EnterRecursiveCall

int Py_EnterRecursiveCall(const char *where)
{
    return _Py_EnterRecursiveCall(where);
}

#undef Py_LeaveRecursiveCall

void Py_LeaveRecursiveCall(void)
{
    _Py_LeaveRecursiveCall();
}
