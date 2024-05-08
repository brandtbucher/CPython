//  This implements the reference cycle garbage collector.
//  The Python module inteface to the collector is in gcmodule.c.
//  See https://devguide.python.org/internals/garbage-collector/

#include "Python.h"
#include "pycore_ceval.h"         // _Py_set_eval_breaker_bit()
#include "pycore_context.h"
#include "pycore_dict.h"          // _PyDict_MaybeUntrack()
#include "pycore_initconfig.h"
#include "pycore_interp.h"        // PyInterpreterState.gc
#include "pycore_object.h"
#include "pycore_object_alloc.h"  // _PyObject_MallocWithType()
#include "pycore_pyerrors.h"
#include "pycore_pystate.h"       // _PyThreadState_GET()
#include "pycore_weakref.h"       // _PyWeakref_ClearRef()
#include "pydtrace.h"

#ifndef Py_GIL_DISABLED

typedef struct _gc_runtime_state GCState;

#ifdef Py_DEBUG
#  define GC_DEBUG
#endif

#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

// update_refs() set this bit for all objects in current generation.
// subtract_refs() and move_unreachable() uses this to distinguish
// visited object is in GCing or not.
//
// move_unreachable() removes this flag from reachable objects.
// Only unreachable objects have this flag.
//
// No objects in interpreter have this flag after GC ends.
#define PREV_MASK_COLLECTING   _PyGC_PREV_MASK_COLLECTING

// Lowest bit of _gc_next is used for UNREACHABLE flag.
//
// This flag represents the object is in unreachable list in move_unreachable()
//
// Although this flag is used only in move_unreachable(), move_unreachable()
// doesn't clear this flag to skip unnecessary iteration.
// move_legacy_finalizers() removes this flag instead.
// Between them, unreachable list is not normal list and we can not use
// most gc_list_* functions for it.
#define NEXT_MASK_UNREACHABLE  2

#define AS_GC(op) _Py_AS_GC(op)
#define FROM_GC(gc) _Py_FROM_GC(gc)

// Automatically choose the generation that needs collecting.
#define GENERATION_AUTO (-1)

static inline int
gc_is_collecting(PyGC_Head *g)
{
    return (g->_gc_prev & PREV_MASK_COLLECTING) != 0;
}

static inline void
gc_clear_collecting(PyGC_Head *g)
{
    g->_gc_prev &= ~PREV_MASK_COLLECTING;
}

static inline Py_ssize_t
gc_get_refs(PyGC_Head *g)
{
    return (Py_ssize_t)(g->_gc_prev >> _PyGC_PREV_SHIFT);
}

static inline void
gc_set_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_prev = (g->_gc_prev & ~_PyGC_PREV_MASK)
        | ((uintptr_t)(refs) << _PyGC_PREV_SHIFT);
}

static inline void
gc_reset_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_prev = (g->_gc_prev & _PyGC_PREV_MASK_FINALIZED)
        | PREV_MASK_COLLECTING
        | ((uintptr_t)(refs) << _PyGC_PREV_SHIFT);
}

static inline void
gc_decref(PyGC_Head *g)
{
    _PyObject_ASSERT_WITH_MSG(FROM_GC(g),
                              gc_get_refs(g) > 0,
                              "refcount is too small");
    g->_gc_prev -= 1 << _PyGC_PREV_SHIFT;
}

static inline int
gc_old_space(PyGC_Head *g)
{
    return g->_gc_next & _PyGC_NEXT_MASK_OLD_SPACE_1;
}

static inline int
flip_old_space(int space)
{
    assert(space == 0 || space == 1);
    return space ^ _PyGC_NEXT_MASK_OLD_SPACE_1;
}

static inline void
gc_flip_old_space(PyGC_Head *g)
{
    g->_gc_next ^= _PyGC_NEXT_MASK_OLD_SPACE_1;
}

static inline void
gc_set_old_space(PyGC_Head *g, int space)
{
    assert(space == 0 || space == _PyGC_NEXT_MASK_OLD_SPACE_1);
    g->_gc_next &= ~_PyGC_NEXT_MASK_OLD_SPACE_1;
    g->_gc_next |= space;
}

static PyGC_Head *
GEN_HEAD(GCState *gcstate, int n)
{
    assert((gcstate->visited_space & (~1)) == 0);
    switch(n) {
        case 0:
            return &gcstate->young.head;
        case 1:
            return &gcstate->old[gcstate->visited_space].head;
        case 2:
            return &gcstate->old[gcstate->visited_space^1].head;
        default:
            Py_UNREACHABLE();
    }
}

static GCState *
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}


void
_PyGC_InitState(GCState *gcstate)
{
#define INIT_HEAD(GEN) \
    do { \
        GEN.head._gc_next = (uintptr_t)&GEN.head; \
        GEN.head._gc_prev = (uintptr_t)&GEN.head; \
    } while (0)

    assert(gcstate->young.count == 0);
    assert(gcstate->old[0].count == 0);
    assert(gcstate->old[1].count == 0);
    INIT_HEAD(gcstate->young);
    INIT_HEAD(gcstate->old[0]);
    INIT_HEAD(gcstate->old[1]);
    INIT_HEAD(gcstate->permanent_generation);

#undef INIT_HEAD
}


PyStatus
_PyGC_Init(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;

    gcstate->garbage = PyList_New(0);
    if (gcstate->garbage == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->callbacks = PyList_New(0);
    if (gcstate->callbacks == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    gcstate->heap_size = 0;

    return _PyStatus_OK();
}


/*
_gc_prev values
---------------

Between collections, _gc_prev is used for doubly linked list.

Lowest two bits of _gc_prev are used for flags.
PREV_MASK_COLLECTING is used only while collecting and cleared before GC ends
or _PyObject_GC_UNTRACK() is called.

During a collection, _gc_prev is temporary used for gc_refs, and the gc list
is singly linked until _gc_prev is restored.

gc_refs
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.

PREV_MASK_COLLECTING
    Objects in generation being collected are marked PREV_MASK_COLLECTING in
    update_refs().


_gc_next values
---------------

_gc_next takes these values:

0
    The object is not tracked

!= 0
    Pointer to the next object in the GC list.
    Additionally, lowest bit is used temporary for
    NEXT_MASK_UNREACHABLE flag described below.

NEXT_MASK_UNREACHABLE
    move_unreachable() then moves objects not reachable (whether directly or
    indirectly) from outside the generation into an "unreachable" set and
    set this flag.

    Objects that are found to be reachable have gc_refs set to 1.
    When this flag is set for the reachable object, the object must be in
    "unreachable" set.
    The flag is unset and the object is moved back to "reachable" set.

    move_legacy_finalizers() will remove this flag from "unreachable" set.
*/

/*** list functions ***/

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

/* Append `node` to `list`. */
static inline void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
    assert((list->_gc_prev & ~_PyGC_PREV_MASK) == 0);
    PyGC_Head *last = (PyGC_Head *)list->_gc_prev;

    // last <-> node
    _PyGCHead_SET_PREV(node, last);
    _PyGCHead_SET_NEXT(last, node);

    // node <-> list
    _PyGCHead_SET_NEXT(node, list);
    list->_gc_prev = (uintptr_t)node;
}

/* Remove `node` from the gc list it's currently in. */
static inline void
gc_list_remove(PyGC_Head *node)
{
    PyGC_Head *prev = GC_PREV(node);
    PyGC_Head *next = GC_NEXT(node);

    _PyGCHead_SET_NEXT(prev, next);
    _PyGCHead_SET_PREV(next, prev);

    node->_gc_next = 0; /* object is not currently tracked */
}

/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = (uintptr_t)node;
    _PyGCHead_SET_NEXT(node, list);
}

/* append list `from` onto list `to`; `from` becomes an empty list */
static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
    assert(from != to);
    if (!gc_list_is_empty(from)) {
        PyGC_Head *to_tail = GC_PREV(to);
        PyGC_Head *from_head = GC_NEXT(from);
        PyGC_Head *from_tail = GC_PREV(from);
        assert(from_head != from);
        assert(from_tail != from);
        assert(gc_list_is_empty(to) ||
            gc_old_space(to_tail) == gc_old_space(from_tail));

        _PyGCHead_SET_NEXT(to_tail, from_head);
        _PyGCHead_SET_PREV(from_head, to_tail);

        _PyGCHead_SET_NEXT(from_tail, to);
        _PyGCHead_SET_PREV(to, from_tail);
    }
    gc_list_init(from);
}

static Py_ssize_t
gc_list_size(PyGC_Head *list)
{
    PyGC_Head *gc;
    Py_ssize_t n = 0;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        n++;
    }
    return n;
}

/* Walk the list and mark all objects as non-collecting */
static inline void
gc_list_clear_collecting(PyGC_Head *collectable)
{
    PyGC_Head *gc;
    for (gc = GC_NEXT(collectable); gc != collectable; gc = GC_NEXT(gc)) {
        gc_clear_collecting(gc);
    }
}

/* Append objects in a GC list to a Python list.
 * Return 0 if all OK, < 0 if error (out of memory for list)
 */
static int
append_objects(PyObject *py_list, PyGC_Head *gc_list)
{
    PyGC_Head *gc;
    for (gc = GC_NEXT(gc_list); gc != gc_list; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);
        if (op != py_list) {
            if (PyList_Append(py_list, op)) {
                return -1; /* exception */
            }
        }
    }
    return 0;
}

// Constants for validate_list's flags argument.
enum flagstates {collecting_clear_unreachable_clear,
                 collecting_clear_unreachable_set,
                 collecting_set_unreachable_clear,
                 collecting_set_unreachable_set};

#ifdef GC_DEBUG
// validate_list checks list consistency.  And it works as document
// describing when flags are expected to be set / unset.
// `head` must be a doubly-linked gc list, although it's fine (expected!) if
// the prev and next pointers are "polluted" with flags.
// What's checked:
// - The `head` pointers are not polluted.
// - The objects' PREV_MASK_COLLECTING and NEXT_MASK_UNREACHABLE flags are all
//   `set or clear, as specified by the 'flags' argument.
// - The prev and next pointers are mutually consistent.
static void
validate_list(PyGC_Head *head, enum flagstates flags)
{
    assert((head->_gc_prev & ~_PyGC_PREV_MASK) == 0);
    assert((head->_gc_next & ~_PyGC_PREV_MASK) == 0);
    uintptr_t prev_value = 0, next_value = 0;
    switch (flags) {
        case collecting_clear_unreachable_clear:
            break;
        case collecting_set_unreachable_clear:
            prev_value = PREV_MASK_COLLECTING;
            break;
        case collecting_clear_unreachable_set:
            next_value = NEXT_MASK_UNREACHABLE;
            break;
        case collecting_set_unreachable_set:
            prev_value = PREV_MASK_COLLECTING;
            next_value = NEXT_MASK_UNREACHABLE;
            break;
        default:
            assert(! "bad internal flags argument");
    }
    PyGC_Head *prev = head;
    PyGC_Head *gc = GC_NEXT(head);
    while (gc != head) {
        PyGC_Head *trueprev = GC_PREV(gc);
        PyGC_Head *truenext = GC_NEXT(gc);
        assert(truenext != NULL);
        assert(trueprev == prev);
        assert((gc->_gc_prev & PREV_MASK_COLLECTING) == prev_value);
        assert((gc->_gc_next & NEXT_MASK_UNREACHABLE) == next_value);
        prev = gc;
        gc = truenext;
    }
    assert(prev == GC_PREV(head));
}

static void
validate_old(GCState *gcstate)
{
    for (int space = 0; space < 2; space++) {
        PyGC_Head *head = &gcstate->old[space].head;
        PyGC_Head *gc = GC_NEXT(head);
        while (gc != head) {
            PyGC_Head *next = GC_NEXT(gc);
            assert(gc_old_space(gc) == space);
            gc = next;
        }
    }
}

static void
validate_consistent_old_space(PyGC_Head *head)
{
    PyGC_Head *prev = head;
    PyGC_Head *gc = GC_NEXT(head);
    if (gc == head) {
        return;
    }
    int old_space = gc_old_space(gc);
    while (gc != head) {
        PyGC_Head *truenext = GC_NEXT(gc);
        assert(truenext != NULL);
        assert(gc_old_space(gc) == old_space);
        prev = gc;
        gc = truenext;
    }
    assert(prev == GC_PREV(head));
}

static void
gc_list_validate_space(PyGC_Head *head, int space) {
    PyGC_Head *gc = GC_NEXT(head);
    while (gc != head) {
        assert(gc_old_space(gc) == space);
        gc = GC_NEXT(gc);
    }
}

#else
#define validate_list(x, y) do{}while(0)
#define validate_old(g) do{}while(0)
#define validate_consistent_old_space(l) do{}while(0)
#define gc_list_validate_space(l, s) do{}while(0)
#endif

/*** end of list stuff ***/


/* Set all gc_refs = ob_refcnt.  After this, gc_refs is > 0 and
 * PREV_MASK_COLLECTING bit is set for all objects in containers.
 */
static void
update_refs(PyGC_Head *containers)
{
    PyGC_Head *next;
    PyGC_Head *gc = GC_NEXT(containers);

    while (gc != containers) {
        next = GC_NEXT(gc);
        PyObject *op = FROM_GC(gc);
        if (_Py_IsImmortal(op)) {
           gc_list_move(gc, &get_gc_state()->permanent_generation.head);
           gc = next;
           continue;
        }
        gc_reset_refs(gc, Py_REFCNT(op));
        /* Python's cyclic gc should never see an incoming refcount
         * of 0:  if something decref'ed to 0, it should have been
         * deallocated immediately at that time.
         * Possible cause (if the assert triggers):  a tp_dealloc
         * routine left a gc-aware object tracked during its teardown
         * phase, and did something-- or allowed something to happen --
         * that called back into Python.  gc can trigger then, and may
         * see the still-tracked dying object.  Before this assert
         * was added, such mistakes went on to allow gc to try to
         * delete the object again.  In a debug build, that caused
         * a mysterious segfault, when _Py_ForgetReference tried
         * to remove the object from the doubly-linked list of all
         * objects a second time.  In a release build, an actual
         * double deallocation occurred, which leads to corruption
         * of the allocator's internal bookkeeping pointers.  That's
         * so serious that maybe this should be a release-build
         * check instead of an assert?
         */
        _PyObject_ASSERT(op, gc_get_refs(gc) != 0);
        gc = next;
    }
}

/* A traversal callback for subtract_refs. */
static int
visit_decref(PyObject *op, void *parent)
{
    OBJECT_STAT_INC(object_visits);
    _PyObject_ASSERT(_PyObject_CAST(parent), !_PyObject_IsFreed(op));

    if (_PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        /* We're only interested in gc_refs for objects in the
         * generation being collected, which can be recognized
         * because only they have positive gc_refs.
         */
        if (gc_is_collecting(gc)) {
            gc_decref(gc);
        }
    }
    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
static void
subtract_refs(PyGC_Head *containers)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(containers);
    for (; gc != containers; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);
        traverse = Py_TYPE(op)->tp_traverse;
        (void) traverse(op,
                        visit_decref,
                        op);
    }
}

/* A traversal callback for move_unreachable. */
static int
visit_reachable(PyObject *op, void *arg)
{
    PyGC_Head *reachable = arg;
    OBJECT_STAT_INC(object_visits);
    if (!_PyObject_IS_GC(op)) {
        return 0;
    }

    PyGC_Head *gc = AS_GC(op);
    const Py_ssize_t gc_refs = gc_get_refs(gc);

    // Ignore objects in other generation.
    // This also skips objects "to the left" of the current position in
    // move_unreachable's scan of the 'young' list - they've already been
    // traversed, and no longer have the PREV_MASK_COLLECTING flag.
    if (! gc_is_collecting(gc)) {
        return 0;
    }
    // It would be a logic error elsewhere if the collecting flag were set on
    // an untracked object.
    _PyObject_ASSERT(op, gc->_gc_next != 0);

    if (gc->_gc_next & NEXT_MASK_UNREACHABLE) {
        /* This had gc_refs = 0 when move_unreachable got
         * to it, but turns out it's reachable after all.
         * Move it back to move_unreachable's 'young' list,
         * and move_unreachable will eventually get to it
         * again.
         */
        // Manually unlink gc from unreachable list because the list functions
        // don't work right in the presence of NEXT_MASK_UNREACHABLE flags.
        PyGC_Head *prev = GC_PREV(gc);
        PyGC_Head *next = GC_NEXT(gc);
        _PyObject_ASSERT(FROM_GC(prev),
                         prev->_gc_next & NEXT_MASK_UNREACHABLE);
        _PyObject_ASSERT(FROM_GC(next),
                         next->_gc_next & NEXT_MASK_UNREACHABLE);
        prev->_gc_next = gc->_gc_next;  // copy flag bits
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        _PyGCHead_SET_PREV(next, prev);

        gc_list_append(gc, reachable);
        gc_set_refs(gc, 1);
    }
    else if (gc_refs == 0) {
        /* This is in move_unreachable's 'young' list, but
         * the traversal hasn't yet gotten to it.  All
         * we need to do is tell move_unreachable that it's
         * reachable.
         */
        gc_set_refs(gc, 1);
    }
    /* Else there's nothing to do.
     * If gc_refs > 0, it must be in move_unreachable's 'young'
     * list, and move_unreachable will eventually get to it.
     */
    else {
        _PyObject_ASSERT_WITH_MSG(op, gc_refs > 0, "refcount is too small");
    }
    return 0;
}

/* Move the unreachable objects from young to unreachable.  After this,
 * all objects in young don't have PREV_MASK_COLLECTING flag and
 * unreachable have the flag.
 * All objects in young after this are directly or indirectly reachable
 * from outside the original young; and all objects in unreachable are
 * not.
 *
 * This function restores _gc_prev pointer.  young and unreachable are
 * doubly linked list after this function.
 * But _gc_next in unreachable list has NEXT_MASK_UNREACHABLE flag.
 * So we can not gc_list_* functions for unreachable until we remove the flag.
 */
static void
move_unreachable(PyGC_Head *young, PyGC_Head *unreachable)
{
    // previous elem in the young list, used for restore gc_prev.
    PyGC_Head *prev = young;
    PyGC_Head *gc = GC_NEXT(young);

    /* Invariants:  all objects "to the left" of us in young are reachable
     * (directly or indirectly) from outside the young list as it was at entry.
     *
     * All other objects from the original young "to the left" of us are in
     * unreachable now, and have NEXT_MASK_UNREACHABLE.  All objects to the
     * left of us in 'young' now have been scanned, and no objects here
     * or to the right have been scanned yet.
     */

    validate_consistent_old_space(young);
    /* Record which old space we are in, and set NEXT_MASK_UNREACHABLE bit for convenience */
    uintptr_t flags = NEXT_MASK_UNREACHABLE | (gc->_gc_next & _PyGC_NEXT_MASK_OLD_SPACE_1);
    while (gc != young) {
        if (gc_get_refs(gc)) {
            /* gc is definitely reachable from outside the
             * original 'young'.  Mark it as such, and traverse
             * its pointers to find any other objects that may
             * be directly reachable from it.  Note that the
             * call to tp_traverse may append objects to young,
             * so we have to wait until it returns to determine
             * the next object to visit.
             */
            PyObject *op = FROM_GC(gc);
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(gc) > 0,
                                      "refcount is too small");
            // NOTE: visit_reachable may change gc->_gc_next when
            // young->_gc_prev == gc.  Don't do gc = GC_NEXT(gc) before!
            (void) traverse(op,
                    visit_reachable,
                    (void *)young);
            // relink gc_prev to prev element.
            _PyGCHead_SET_PREV(gc, prev);
            // gc is not COLLECTING state after here.
            gc_clear_collecting(gc);
            prev = gc;
        }
        else {
            /* This *may* be unreachable.  To make progress,
             * assume it is.  gc isn't directly reachable from
             * any object we've already traversed, but may be
             * reachable from an object we haven't gotten to yet.
             * visit_reachable will eventually move gc back into
             * young if that's so, and we'll see it again.
             */
            // Move gc to unreachable.
            // No need to gc->next->prev = prev because it is single linked.
            prev->_gc_next = gc->_gc_next;

            // We can't use gc_list_append() here because we use
            // NEXT_MASK_UNREACHABLE here.
            PyGC_Head *last = GC_PREV(unreachable);
            // NOTE: Since all objects in unreachable set has
            // NEXT_MASK_UNREACHABLE flag, we set it unconditionally.
            // But this may pollute the unreachable list head's 'next' pointer
            // too. That's semantically senseless but expedient here - the
            // damage is repaired when this function ends.
            last->_gc_next = flags | (uintptr_t)gc;
            _PyGCHead_SET_PREV(gc, last);
            gc->_gc_next = flags | (uintptr_t)unreachable;
            unreachable->_gc_prev = (uintptr_t)gc;
        }
        gc = _PyGCHead_NEXT(prev);
    }
    // young->_gc_prev must be last element remained in the list.
    young->_gc_prev = (uintptr_t)prev;
    young->_gc_next &= _PyGC_PREV_MASK;
    // don't let the pollution of the list head's next pointer leak
    unreachable->_gc_next &= _PyGC_PREV_MASK;
}

static void
untrack_tuples(PyGC_Head *head)
{
    PyGC_Head *next, *gc = GC_NEXT(head);
    while (gc != head) {
        PyObject *op = FROM_GC(gc);
        next = GC_NEXT(gc);
        if (PyTuple_CheckExact(op)) {
            _PyTuple_MaybeUntrack(op);
        }
        gc = next;
    }
}

/* Try to untrack all currently tracked dictionaries */
static void
untrack_dicts(PyGC_Head *head)
{
    PyGC_Head *next, *gc = GC_NEXT(head);
    while (gc != head) {
        PyObject *op = FROM_GC(gc);
        next = GC_NEXT(gc);
        if (PyDict_CheckExact(op)) {
            _PyDict_MaybeUntrack(op);
        }
        gc = next;
    }
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return Py_TYPE(op)->tp_del != NULL;
}

/* Move the objects in unreachable with tp_del slots into `finalizers`.
 *
 * This function also removes NEXT_MASK_UNREACHABLE flag
 * from _gc_next in unreachable.
 */
static void
move_legacy_finalizers(PyGC_Head *unreachable, PyGC_Head *finalizers)
{
    PyGC_Head *gc, *next;
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        (unreachable->_gc_next & NEXT_MASK_UNREACHABLE) == 0);

    /* March over unreachable.  Move objects with finalizers into
     * `finalizers`.
     */
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        PyObject *op = FROM_GC(gc);

        _PyObject_ASSERT(op, gc->_gc_next & NEXT_MASK_UNREACHABLE);
        next = GC_NEXT(gc);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;

        if (has_legacy_finalizer(op)) {
            gc_clear_collecting(gc);
            gc_list_move(gc, finalizers);
        }
    }
}

static inline void
clear_unreachable_mask(PyGC_Head *unreachable)
{
    /* Check that the list head does not have the unreachable bit set */
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        ((uintptr_t)unreachable & NEXT_MASK_UNREACHABLE) == 0);
    _PyObject_ASSERT(
        FROM_GC(unreachable),
        (unreachable->_gc_next & NEXT_MASK_UNREACHABLE) == 0);

    PyGC_Head *gc, *next;
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        _PyObject_ASSERT((PyObject*)FROM_GC(gc), gc->_gc_next & NEXT_MASK_UNREACHABLE);
        next = GC_NEXT(gc);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
    }
    validate_list(unreachable, collecting_set_unreachable_clear);
}

/* A traversal callback for move_legacy_finalizer_reachable. */
static int
visit_move(PyObject *op, void *arg)
{
    PyGC_Head *tolist = arg;
    OBJECT_STAT_INC(object_visits);
    if (_PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        if (gc_is_collecting(gc)) {
            gc_list_move(gc, tolist);
            gc_clear_collecting(gc);
        }
    }
    return 0;
}

/* Move objects that are reachable from finalizers, from the unreachable set
 * into finalizers set.
 */
static void
move_legacy_finalizer_reachable(PyGC_Head *finalizers)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(finalizers);
    for (; gc != finalizers; gc = GC_NEXT(gc)) {
        /* Note that the finalizers list may grow during this. */
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        (void) traverse(FROM_GC(gc),
                        visit_move,
                        (void *)finalizers);
    }
}

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static int
handle_weakrefs(PyGC_Head *unreachable, PyGC_Head *old)
{
    PyGC_Head *gc;
    PyObject *op;               /* generally FROM_GC(gc) */
    PyWeakReference *wr;        /* generally a cast of op */
    PyGC_Head wrcb_to_call;     /* weakrefs with callbacks to call */
    PyGC_Head *next;
    int num_freed = 0;

    gc_list_init(&wrcb_to_call);

    /* Clear all weakrefs to the objects in unreachable.  If such a weakref
     * also has a callback, move it into `wrcb_to_call` if the callback
     * needs to be invoked.  Note that we cannot invoke any callbacks until
     * all weakrefs to unreachable objects are cleared, lest the callback
     * resurrect an unreachable object via a still-active weakref.  We
     * make another pass over wrcb_to_call, invoking callbacks, after this
     * pass completes.
     */
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        PyWeakReference **wrlist;

        op = FROM_GC(gc);
        next = GC_NEXT(gc);

        if (PyWeakref_Check(op)) {
            /* A weakref inside the unreachable set must be cleared.  If we
             * allow its callback to execute inside delete_garbage(), it
             * could expose objects that have tp_clear already called on
             * them.  Or, it could resurrect unreachable objects.  One way
             * this can happen is if some container objects do not implement
             * tp_traverse.  Then, wr_object can be outside the unreachable
             * set but can be deallocated as a result of breaking the
             * reference cycle.  If we don't clear the weakref, the callback
             * will run and potentially cause a crash.  See bpo-38006 for
             * one example.
             */
            _PyWeakref_ClearRef((PyWeakReference *)op);
        }

        if (! _PyType_SUPPORTS_WEAKREFS(Py_TYPE(op))) {
            continue;
        }

        /* It supports weakrefs.  Does it have any?
         *
         * This is never triggered for static types so we can avoid the
         * (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR().
         */
        wrlist = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(op);

        /* `op` may have some weakrefs.  March over the list, clear
         * all the weakrefs, and move the weakrefs with callbacks
         * that must be called into wrcb_to_call.
         */
        for (wr = *wrlist; wr != NULL; wr = *wrlist) {
            PyGC_Head *wrasgc;                  /* AS_GC(wr) */

            /* _PyWeakref_ClearRef clears the weakref but leaves
             * the callback pointer intact.  Obscure:  it also
             * changes *wrlist.
             */
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == op);
            _PyWeakref_ClearRef(wr);
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == Py_None);
            if (wr->wr_callback == NULL) {
                /* no callback */
                continue;
            }

            /* Headache time.  `op` is going away, and is weakly referenced by
             * `wr`, which has a callback.  Should the callback be invoked?  If wr
             * is also trash, no:
             *
             * 1. There's no need to call it.  The object and the weakref are
             *    both going away, so it's legitimate to pretend the weakref is
             *    going away first.  The user has to ensure a weakref outlives its
             *    referent if they want a guarantee that the wr callback will get
             *    invoked.
             *
             * 2. It may be catastrophic to call it.  If the callback is also in
             *    cyclic trash (CT), then although the CT is unreachable from
             *    outside the current generation, CT may be reachable from the
             *    callback.  Then the callback could resurrect insane objects.
             *
             * Since the callback is never needed and may be unsafe in this case,
             * wr is simply left in the unreachable set.  Note that because we
             * already called _PyWeakref_ClearRef(wr), its callback will never
             * trigger.
             *
             * OTOH, if wr isn't part of CT, we should invoke the callback:  the
             * weakref outlived the trash.  Note that since wr isn't CT in this
             * case, its callback can't be CT either -- wr acted as an external
             * root to this generation, and therefore its callback did too.  So
             * nothing in CT is reachable from the callback either, so it's hard
             * to imagine how calling it later could create a problem for us.  wr
             * is moved to wrcb_to_call in this case.
             */
            if (gc_is_collecting(AS_GC((PyObject *)wr))) {
                /* it should already have been cleared above */
                _PyObject_ASSERT((PyObject*)wr, wr->wr_object == Py_None);
                continue;
            }

            /* Create a new reference so that wr can't go away
             * before we can process it again.
             */
            Py_INCREF(wr);

            /* Move wr to wrcb_to_call, for the next pass. */
            wrasgc = AS_GC((PyObject *)wr);
            // wrasgc is reachable, but next isn't, so they can't be the same
            _PyObject_ASSERT((PyObject *)wr, wrasgc != next);
            gc_list_move(wrasgc, &wrcb_to_call);
        }
    }

    /* Invoke the callbacks we decided to honor.  It's safe to invoke them
     * because they can't reference unreachable objects.
     */
    int visited_space = get_gc_state()->visited_space;
    while (! gc_list_is_empty(&wrcb_to_call)) {
        PyObject *temp;
        PyObject *callback;

        gc = (PyGC_Head*)wrcb_to_call._gc_next;
        op = FROM_GC(gc);
        _PyObject_ASSERT(op, PyWeakref_Check(op));
        wr = (PyWeakReference *)op;
        callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        temp = PyObject_CallOneArg(callback, (PyObject *)wr);
        if (temp == NULL) {
            PyErr_WriteUnraisable(callback);
        }
        else {
            Py_DECREF(temp);
        }

        /* Give up the reference we created in the first pass.  When
         * op's refcount hits 0 (which it may or may not do right now),
         * op's tp_dealloc will decref op->wr_callback too.  Note
         * that the refcount probably will hit 0 now, and because this
         * weakref was reachable to begin with, gc didn't already
         * add it to its count of freed objects.  Example:  a reachable
         * weak value dict maps some key to this reachable weakref.
         * The callback removes this key->weakref mapping from the
         * dict, leaving no other references to the weakref (excepting
         * ours).
         */
        Py_DECREF(op);
        if (wrcb_to_call._gc_next == (uintptr_t)gc) {
            /* object is still alive -- move it */
            gc_set_old_space(gc, visited_space);
            gc_list_move(gc, old);
        }
        else {
            ++num_freed;
        }
    }

    return num_freed;
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Handle uncollectable garbage (cycles with tp_del slots, and stuff reachable
 * only from such cycles).
 * If _PyGC_DEBUG_SAVEALL, all objects in finalizers are appended to the module
 * garbage list (a Python list), else only the objects in finalizers with
 * __del__ methods are appended to garbage.  All objects in finalizers are
 * merged into the old list regardless.
 */
static void
handle_legacy_finalizers(PyThreadState *tstate,
                         GCState *gcstate,
                         PyGC_Head *finalizers, PyGC_Head *old)
{
    assert(!_PyErr_Occurred(tstate));
    assert(gcstate->garbage != NULL);

    PyGC_Head *gc = GC_NEXT(finalizers);
    for (; gc != finalizers; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);

        if ((gcstate->debug & _PyGC_DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            if (PyList_Append(gcstate->garbage, op) < 0) {
                _PyErr_Clear(tstate);
                break;
            }
        }
    }

    gc_list_merge(finalizers, old);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
static void
finalize_garbage(PyThreadState *tstate, PyGC_Head *collectable)
{
    destructor finalize;
    PyGC_Head seen;

    /* While we're going through the loop, `finalize(op)` may cause op, or
     * other objects, to be reclaimed via refcounts falling to zero.  So
     * there's little we can rely on about the structure of the input
     * `collectable` list across iterations.  For safety, we always take the
     * first object in that list and move it to a temporary `seen` list.
     * If objects vanish from the `collectable` and `seen` lists we don't
     * care.
     */
    gc_list_init(&seen);

    while (!gc_list_is_empty(collectable)) {
        PyGC_Head *gc = GC_NEXT(collectable);
        PyObject *op = FROM_GC(gc);
        gc_list_move(gc, &seen);
        if (!_PyGC_FINALIZED(op) &&
            (finalize = Py_TYPE(op)->tp_finalize) != NULL)
        {
            _PyGC_SET_FINALIZED(op);
            Py_INCREF(op);
            finalize(op);
            assert(!_PyErr_Occurred(tstate));
            Py_DECREF(op);
        }
    }
    gc_list_merge(&seen, collectable);
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
static void
delete_garbage(PyThreadState *tstate, GCState *gcstate,
               PyGC_Head *collectable, PyGC_Head *old)
{
    assert(!_PyErr_Occurred(tstate));

    while (!gc_list_is_empty(collectable)) {
        PyGC_Head *gc = GC_NEXT(collectable);
        PyObject *op = FROM_GC(gc);

        _PyObject_ASSERT_WITH_MSG(op, Py_REFCNT(op) > 0,
                                  "refcount is too small");

        if (gcstate->debug & _PyGC_DEBUG_SAVEALL) {
            assert(gcstate->garbage != NULL);
            if (PyList_Append(gcstate->garbage, op) < 0) {
                _PyErr_Clear(tstate);
            }
        }
        else {
            inquiry clear;
            if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                Py_INCREF(op);
                (void) clear(op);
                if (_PyErr_Occurred(tstate)) {
                    PyErr_FormatUnraisable("Exception ignored in tp_clear of %s",
                                           Py_TYPE(op)->tp_name);
                }
                Py_DECREF(op);
            }
        }
        if (GC_NEXT(collectable) == gc) {
            /* object is still alive, move it, it may die later */
            gc_clear_collecting(gc);
            gc_list_move(gc, old);
        }
    }
}


/* Deduce which objects among "base" are unreachable from outside the list
   and move them to 'unreachable'. The process consist in the following steps:

1. Copy all reference counts to a different field (gc_prev is used to hold
   this copy to save memory).
2. Traverse all objects in "base" and visit all referred objects using
   "tp_traverse" and for every visited object, subtract 1 to the reference
   count (the one that we copied in the previous step). After this step, all
   objects that can be reached directly from outside must have strictly positive
   reference count, while all unreachable objects must have a count of exactly 0.
3. Identify all unreachable objects (the ones with 0 reference count) and move
   them to the "unreachable" list. This step also needs to move back to "base" all
   objects that were initially marked as unreachable but are referred transitively
   by the reachable objects (the ones with strictly positive reference count).

Contracts:

    * The "base" has to be a valid list with no mask set.

    * The "unreachable" list must be uninitialized (this function calls
      gc_list_init over 'unreachable').

IMPORTANT: This function leaves 'unreachable' with the NEXT_MASK_UNREACHABLE
flag set but it does not clear it to skip unnecessary iteration. Before the
flag is cleared (for example, by using 'clear_unreachable_mask' function or
by a call to 'move_legacy_finalizers'), the 'unreachable' list is not a normal
list and we can not use most gc_list_* functions for it. */
static inline void
deduce_unreachable(PyGC_Head *base, PyGC_Head *unreachable) {
    validate_list(base, collecting_clear_unreachable_clear);
    /* Using ob_refcnt and gc_refs, calculate which objects in the
     * container set are reachable from outside the set (i.e., have a
     * refcount greater than 0 when all the references within the
     * set are taken into account).
     */
    update_refs(base);  // gc_prev is used for gc_refs
    subtract_refs(base);

    /* Leave everything reachable from outside base in base, and move
     * everything else (in base) to unreachable.
     *
     * NOTE:  This used to move the reachable objects into a reachable
     * set instead.  But most things usually turn out to be reachable,
     * so it's more efficient to move the unreachable things.  It "sounds slick"
     * to move the unreachable objects, until you think about it - the reason it
     * pays isn't actually obvious.
     *
     * Suppose we create objects A, B, C in that order.  They appear in the young
     * generation in the same order.  If B points to A, and C to B, and C is
     * reachable from outside, then the adjusted refcounts will be 0, 0, and 1
     * respectively.
     *
     * When move_unreachable finds A, A is moved to the unreachable list.  The
     * same for B when it's first encountered.  Then C is traversed, B is moved
     * _back_ to the reachable list.  B is eventually traversed, and then A is
     * moved back to the reachable list.
     *
     * So instead of not moving at all, the reachable objects B and A are moved
     * twice each.  Why is this a win?  A straightforward algorithm to move the
     * reachable objects instead would move A, B, and C once each.
     *
     * The key is that this dance leaves the objects in order C, B, A - it's
     * reversed from the original order.  On all _subsequent_ scans, none of
     * them will move.  Since most objects aren't in cycles, this can save an
     * unbounded number of moves across an unbounded number of later collections.
     * It can cost more only the first time the chain is scanned.
     *
     * Drawback:  move_unreachable is also used to find out what's still trash
     * after finalizers may resurrect objects.  In _that_ case most unreachable
     * objects will remain unreachable, so it would be more efficient to move
     * the reachable objects instead.  But this is a one-time cost, probably not
     * worth complicating the code to speed just a little.
     */
    move_unreachable(base, unreachable);  // gc_prev is pointer again
    validate_list(base, collecting_clear_unreachable_clear);
    validate_list(unreachable, collecting_set_unreachable_set);
}

/* Handle objects that may have resurrected after a call to 'finalize_garbage', moving
   them to 'old_generation' and placing the rest on 'still_unreachable'.

   Contracts:
       * After this function 'unreachable' must not be used anymore and 'still_unreachable'
         will contain the objects that did not resurrect.

       * The "still_unreachable" list must be uninitialized (this function calls
         gc_list_init over 'still_unreachable').

IMPORTANT: After a call to this function, the 'still_unreachable' set will have the
PREV_MARK_COLLECTING set, but the objects in this set are going to be removed so
we can skip the expense of clearing the flag to avoid extra iteration. */
static inline void
handle_resurrected_objects(PyGC_Head *unreachable, PyGC_Head* still_unreachable,
                           PyGC_Head *old_generation)
{
    // Remove the PREV_MASK_COLLECTING from unreachable
    // to prepare it for a new call to 'deduce_unreachable'
    gc_list_clear_collecting(unreachable);

    // After the call to deduce_unreachable, the 'still_unreachable' set will
    // have the PREV_MARK_COLLECTING set, but the objects are going to be
    // removed so we can skip the expense of clearing the flag.
    PyGC_Head* resurrected = unreachable;
    deduce_unreachable(resurrected, still_unreachable);
    clear_unreachable_mask(still_unreachable);

    // Move the resurrected objects to the old generation for future collection.
    gc_list_merge(resurrected, old_generation);
}


#define UNTRACK_TUPLES 1
#define UNTRACK_DICTS 2

static void
gc_collect_region(PyThreadState *tstate,
                  PyGC_Head *from,
                  PyGC_Head *to,
                  int untrack,
                  struct gc_collection_stats *stats);

static inline Py_ssize_t
gc_list_set_space(PyGC_Head *list, int space)
{
    Py_ssize_t size = 0;
    PyGC_Head *gc;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        gc_set_old_space(gc, space);
        size++;
    }
    return size;
}

/* Making progress in the incremental collector
 * In order to eventually collect all cycles
 * the incremental collector must progress through the old
 * space faster than objects are added to the old space.
 *
 * Each young or incremental collection adds a numebr of
 * objects, S (for survivors) to the old space, and
 * incremental collectors scan I objects from the old space.
 * I > S must be true. We also want I > S * N to be where
 * N > 1. Higher values of N mean that the old space is
 * scanned more rapidly.
 * The default incremental threshold of 10 translates to
 * N == 1.4 (1 + 4/threshold)
 */

/* Divide by 10, so that the default incremental threshold of 10
 * scans objects at 1% of the heap size */
#define SCAN_RATE_DIVISOR 10

static void
add_stats(GCState *gcstate, int gen, struct gc_collection_stats *stats)
{
    gcstate->generation_stats[gen].collected += stats->collected;
    gcstate->generation_stats[gen].uncollectable += stats->uncollectable;
    gcstate->generation_stats[gen].collections += 1;
}

static void
gc_collect_young(PyThreadState *tstate,
                 struct gc_collection_stats *stats)
{
    GCState *gcstate = &tstate->interp->gc;
    PyGC_Head *young = &gcstate->young.head;
    PyGC_Head *visited = &gcstate->old[gcstate->visited_space].head;
#ifdef Py_STATS
    {
        Py_ssize_t count = 0;
        PyGC_Head *gc;
        for (gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
            count++;
        }
    }
#endif

    PyGC_Head survivors;
    gc_list_init(&survivors);
    gc_collect_region(tstate, young, &survivors, UNTRACK_TUPLES, stats);
    Py_ssize_t survivor_count = 0;
    if (gcstate->visited_space) {
        /* objects in visited space have bit set, so we set it here */
        survivor_count = gc_list_set_space(&survivors, 1);
    }
    else {
        PyGC_Head *gc;
        for (gc = GC_NEXT(&survivors); gc != &survivors; gc = GC_NEXT(gc)) {
#ifdef GC_DEBUG
            assert(gc_old_space(gc) == 0);
#endif
            survivor_count++;
        }
    }
    (void)survivor_count;  // Silence compiler warning
    gc_list_merge(&survivors, visited);
    validate_old(gcstate);
    gcstate->young.count = 0;
    gcstate->old[gcstate->visited_space].count++;
    Py_ssize_t scale_factor = gcstate->old[0].threshold;
    if (scale_factor < 1) {
        scale_factor = 1;
    }
    gcstate->work_to_do += gcstate->heap_size / SCAN_RATE_DIVISOR / scale_factor;
    add_stats(gcstate, 0, stats);
}

#ifndef NDEBUG
static inline int
IS_IN_VISITED(PyGC_Head *gc, int visited_space)
{
    assert(visited_space == 0 || flip_old_space(visited_space) == 0);
    return gc_old_space(gc) == visited_space;
}
#endif

struct container_and_flag {
    PyGC_Head *container;
    int visited_space;
    uintptr_t size;
};

/* A traversal callback for adding to container) */
static int
visit_add_to_container(PyObject *op, void *arg)
{
    OBJECT_STAT_INC(object_visits);
    struct container_and_flag *cf = (struct container_and_flag *)arg;
    int visited = cf->visited_space;
    assert(visited == get_gc_state()->visited_space);
    if (!_Py_IsImmortal(op) && _PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        if (_PyObject_GC_IS_TRACKED(op) &&
            gc_old_space(gc) != visited) {
            gc_flip_old_space(gc);
            gc_list_move(gc, cf->container);
            cf->size++;
        }
    }
    return 0;
}

static uintptr_t
expand_region_transitively_reachable(PyGC_Head *container, PyGC_Head *gc, GCState *gcstate)
{
    validate_list(container, collecting_clear_unreachable_clear);
    struct container_and_flag arg = {
        .container = container,
        .visited_space = gcstate->visited_space,
        .size = 0
    };
    assert(GC_NEXT(gc) == container);
    while (gc != container) {
        /* Survivors will be moved to visited space, so they should
         * have been marked as visited */
        assert(IS_IN_VISITED(gc, gcstate->visited_space));
        PyObject *op = FROM_GC(gc);
        if (_Py_IsImmortal(op)) {
            PyGC_Head *next = GC_NEXT(gc);
            gc_list_move(gc, &get_gc_state()->permanent_generation.head);
            gc = next;
            continue;
        }
        traverseproc traverse = Py_TYPE(op)->tp_traverse;
        (void) traverse(op,
                        visit_add_to_container,
                        &arg);
        gc = GC_NEXT(gc);
    }
    return arg.size;
}

/* Do bookkeeping for a completed GC cycle */
static void
completed_cycle(GCState *gcstate)
{
#ifdef Py_DEBUG
    PyGC_Head *not_visited = &gcstate->old[gcstate->visited_space^1].head;
    assert(gc_list_is_empty(not_visited));
#endif
    gcstate->visited_space = flip_old_space(gcstate->visited_space);
    /* Make sure all young objects have old space bit set correctly */
    PyGC_Head *young = &gcstate->young.head;
    PyGC_Head *gc = GC_NEXT(young);
    while (gc != young) {
        PyGC_Head *next = GC_NEXT(gc);
        gc_set_old_space(gc, gcstate->visited_space);
        gc = next;
    }
    gcstate->work_to_do = 0;
}

static void
gc_collect_increment(PyThreadState *tstate, struct gc_collection_stats *stats)
{
    GCState *gcstate = &tstate->interp->gc;
    PyGC_Head *not_visited = &gcstate->old[gcstate->visited_space^1].head;
    PyGC_Head *visited = &gcstate->old[gcstate->visited_space].head;
    PyGC_Head increment;
    gc_list_init(&increment);
    Py_ssize_t scale_factor = gcstate->old[0].threshold;
    if (scale_factor < 1) {
        scale_factor = 1;
    }
    gc_list_merge(&gcstate->young.head, &increment);
    gcstate->young.count = 0;
    gc_list_validate_space(&increment, gcstate->visited_space);
    Py_ssize_t increment_size = 0;
    while (increment_size < gcstate->work_to_do) {
        if (gc_list_is_empty(not_visited)) {
            break;
        }
        PyGC_Head *gc = _PyGCHead_NEXT(not_visited);
        gc_list_move(gc, &increment);
        increment_size++;
        gc_set_old_space(gc, gcstate->visited_space);
        increment_size += expand_region_transitively_reachable(&increment, gc, gcstate);
    }
    gc_list_validate_space(&increment, gcstate->visited_space);
    PyGC_Head survivors;
    gc_list_init(&survivors);
    gc_collect_region(tstate, &increment, &survivors, UNTRACK_TUPLES, stats);
    gc_list_validate_space(&survivors, gcstate->visited_space);
    gc_list_merge(&survivors, visited);
    assert(gc_list_is_empty(&increment));
    gcstate->work_to_do += gcstate->heap_size / SCAN_RATE_DIVISOR / scale_factor;
    gcstate->work_to_do -= increment_size;

    validate_old(gcstate);
    add_stats(gcstate, 1, stats);
    if (gc_list_is_empty(not_visited)) {
        completed_cycle(gcstate);
    }
}


static void
gc_collect_full(PyThreadState *tstate,
                struct gc_collection_stats *stats)
{
    GCState *gcstate = &tstate->interp->gc;
    validate_old(gcstate);
    PyGC_Head *young = &gcstate->young.head;
    PyGC_Head *pending = &gcstate->old[gcstate->visited_space^1].head;
    PyGC_Head *visited = &gcstate->old[gcstate->visited_space].head;
    /* merge all generations into visited */
    gc_list_validate_space(young, gcstate->visited_space);
    gc_list_set_space(pending, gcstate->visited_space);
    gc_list_merge(young, pending);
    gcstate->young.count = 0;
    gc_list_merge(pending, visited);

    gc_collect_region(tstate, visited, visited,
                      UNTRACK_TUPLES | UNTRACK_DICTS,
                      stats);
    gcstate->young.count = 0;
    gcstate->old[0].count = 0;
    gcstate->old[1].count = 0;

    gcstate->work_to_do = - gcstate->young.threshold * 2;
    _PyGC_ClearAllFreeLists(tstate->interp);
    validate_old(gcstate);
    add_stats(gcstate, 2, stats);
}

/* This is the main function. Read this to understand how the
 * collection process works. */
static void
gc_collect_region(PyThreadState *tstate,
                  PyGC_Head *from,
                  PyGC_Head *to,
                  int untrack,
                  struct gc_collection_stats *stats)
{
    PyGC_Head unreachable; /* non-problematic unreachable trash */
    PyGC_Head finalizers;  /* objects with, & reachable from, __del__ */
    PyGC_Head *gc; /* initialize to prevent a compiler warning */
    GCState *gcstate = &tstate->interp->gc;

    assert(gcstate->garbage != NULL);
    assert(!_PyErr_Occurred(tstate));

    _Py_Executors_InvalidateAll(tstate->interp, 0);

    gc_list_init(&unreachable);
    deduce_unreachable(from, &unreachable);
    validate_consistent_old_space(from);
    if (untrack & UNTRACK_TUPLES) {
        untrack_tuples(from);
    }
    if (untrack & UNTRACK_DICTS) {
        untrack_dicts(from);
    }
    validate_consistent_old_space(to);
    if (from != to) {
        gc_list_merge(from, to);
    }
    validate_consistent_old_space(to);
    /* Move reachable objects to next generation. */

    /* All objects in unreachable are trash, but objects reachable from
     * legacy finalizers (e.g. tp_del) can't safely be deleted.
     */
    gc_list_init(&finalizers);
    // NEXT_MASK_UNREACHABLE is cleared here.
    // After move_legacy_finalizers(), unreachable is normal list.
    move_legacy_finalizers(&unreachable, &finalizers);
    /* finalizers contains the unreachable objects with a legacy finalizer;
     * unreachable objects reachable *from* those are also uncollectable,
     * and we move those into the finalizers list too.
     */
    move_legacy_finalizer_reachable(&finalizers);
    validate_list(&finalizers, collecting_clear_unreachable_clear);
    validate_list(&unreachable, collecting_set_unreachable_clear);
    /* Print debugging information. */
    if (gcstate->debug & _PyGC_DEBUG_COLLECTABLE) {
        for (gc = GC_NEXT(&unreachable); gc != &unreachable; gc = GC_NEXT(gc)) {
            debug_cycle("collectable", FROM_GC(gc));
        }
    }

    /* Clear weakrefs and invoke callbacks as necessary. */
    stats->collected += handle_weakrefs(&unreachable, to);
    gc_list_validate_space(to, gcstate->visited_space);
    validate_list(to, collecting_clear_unreachable_clear);
    validate_list(&unreachable, collecting_set_unreachable_clear);

    /* Call tp_finalize on objects which have one. */
    finalize_garbage(tstate, &unreachable);
    /* Handle any objects that may have resurrected after the call
     * to 'finalize_garbage' and continue the collection with the
     * objects that are still unreachable */
    PyGC_Head final_unreachable;
    gc_list_init(&final_unreachable);
    handle_resurrected_objects(&unreachable, &final_unreachable, to);

    /* Call tp_clear on objects in the final_unreachable set.  This will cause
    * the reference cycles to be broken.  It may also cause some objects
    * in finalizers to be freed.
    */
    stats->collected += gc_list_size(&final_unreachable);
    delete_garbage(tstate, gcstate, &final_unreachable, to);

    /* Collect statistics on uncollectable objects found and print
     * debugging information. */
    Py_ssize_t n = 0;
    for (gc = GC_NEXT(&finalizers); gc != &finalizers; gc = GC_NEXT(gc)) {
        n++;
        if (gcstate->debug & _PyGC_DEBUG_COLLECTABLE)
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    stats->uncollectable = n;
    /* Append instances in the uncollectable set to a Python
     * reachable list of garbage.  The programmer has to deal with
     * this if they insist on creating this type of structure.
     */
    handle_legacy_finalizers(tstate, gcstate, &finalizers, to);
    gc_list_validate_space(to, gcstate->visited_space);
    validate_list(to, collecting_clear_unreachable_clear);
}

/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
do_gc_callback(GCState *gcstate, const char *phase,
                   int generation, struct gc_collection_stats *stats)
{
    assert(!PyErr_Occurred());

    /* The local variable cannot be rebound, check it for sanity */
    assert(PyList_CheckExact(gcstate->callbacks));
    PyObject *info = NULL;
    if (PyList_GET_SIZE(gcstate->callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", generation,
            "collected", stats->collected,
            "uncollectable", stats->uncollectable);
        if (info == NULL) {
            PyErr_FormatUnraisable("Exception ignored on invoking gc callbacks");
            return;
        }
    }

    PyObject *phase_obj = PyUnicode_FromString(phase);
    if (phase_obj == NULL) {
        Py_XDECREF(info);
        PyErr_FormatUnraisable("Exception ignored on invoking gc callbacks");
        return;
    }

    PyObject *stack[] = {phase_obj, info};
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(gcstate->callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(gcstate->callbacks, i);
        Py_INCREF(cb); /* make sure cb doesn't go away */
        r = PyObject_Vectorcall(cb, stack, 2, NULL);
        if (r == NULL) {
            PyErr_WriteUnraisable(cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_DECREF(phase_obj);
    Py_XDECREF(info);
    assert(!PyErr_Occurred());
}

static void
invoke_gc_callback(GCState *gcstate, const char *phase,
                   int generation, struct gc_collection_stats *stats)
{
    if (gcstate->callbacks == NULL) {
        return;
    }
    do_gc_callback(gcstate, phase, generation, stats);
}

static int
referrersvisit(PyObject* obj, void *arg)
{
    PyObject *objs = arg;
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++) {
        if (PyTuple_GET_ITEM(objs, i) == obj) {
            return 1;
        }
    }
    return 0;
}

static int
gc_referrers_for(PyObject *objs, PyGC_Head *list, PyObject *resultlist)
{
    PyGC_Head *gc;
    PyObject *obj;
    traverseproc traverse;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        obj = FROM_GC(gc);
        traverse = Py_TYPE(obj)->tp_traverse;
        if (obj == objs || obj == resultlist) {
            continue;
        }
        if (traverse(obj, referrersvisit, objs)) {
            if (PyList_Append(resultlist, obj) < 0) {
                return 0; /* error */
            }
        }
    }
    return 1; /* no error */
}

PyObject *
_PyGC_GetReferrers(PyInterpreterState *interp, PyObject *objs)
{
    PyObject *result = PyList_New(0);
    if (!result) {
        return NULL;
    }

    GCState *gcstate = &interp->gc;
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        if (!(gc_referrers_for(objs, GEN_HEAD(gcstate, i), result))) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

PyObject *
_PyGC_GetObjects(PyInterpreterState *interp, int generation)
{
    assert(generation >= -1 && generation < NUM_GENERATIONS);
    GCState *gcstate = &interp->gc;

    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    if (generation == -1) {
        /* If generation is -1, get all objects from all generations */
        for (int i = 0; i < NUM_GENERATIONS; i++) {
            if (append_objects(result, GEN_HEAD(gcstate, i))) {
                goto error;
            }
        }
    }
    else {
        if (append_objects(result, GEN_HEAD(gcstate, generation))) {
            goto error;
        }
    }

    return result;
error:
    Py_DECREF(result);
    return NULL;
}

void
_PyGC_Freeze(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    /* The permanent_generation has its old space bit set to zero */
    if (gcstate->visited_space) {
        gc_list_set_space(&gcstate->young.head, 0);
    }
    gc_list_merge(&gcstate->young.head, &gcstate->permanent_generation.head);
    gcstate->young.count = 0;
    PyGC_Head*old0 = &gcstate->old[0].head;
    PyGC_Head*old1 = &gcstate->old[1].head;
    gc_list_merge(old0, &gcstate->permanent_generation.head);
    gcstate->old[0].count = 0;
    gc_list_set_space(old1, 0);
    gc_list_merge(old1, &gcstate->permanent_generation.head);
    gcstate->old[1].count = 0;
    validate_old(gcstate);
}

void
_PyGC_Unfreeze(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    gc_list_merge(&gcstate->permanent_generation.head,
                  &gcstate->old[0].head);
    validate_old(gcstate);
}

Py_ssize_t
_PyGC_GetFreezeCount(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    return gc_list_size(&gcstate->permanent_generation.head);
}

/* C API for controlling the state of the garbage collector */
int
PyGC_Enable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 1;
    return old_state;
}

int
PyGC_Disable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 0;
    return old_state;
}

int
PyGC_IsEnabled(void)
{
    GCState *gcstate = get_gc_state();
    return gcstate->enabled;
}

Py_ssize_t
_PyGC_Collect(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    GCState *gcstate = &tstate->interp->gc;

    int expected = 0;
    if (!_Py_atomic_compare_exchange_int(&gcstate->collecting, &expected, 1)) {
        // Don't start a garbage collection if one is already in progress.
        return 0;
    }

    struct gc_collection_stats stats = { 0 };
    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(gcstate, "start", generation, &stats);
    }
    if (PyDTrace_GC_START_ENABLED()) {
        PyDTrace_GC_START(generation);
    }
    PyObject *exc = _PyErr_GetRaisedException(tstate);
    switch(generation) {
        case 0:
            gc_collect_young(tstate, &stats);
            break;
        case 1:
            gc_collect_increment(tstate, &stats);
            break;
        case 2:
            gc_collect_full(tstate, &stats);
            break;
        default:
            Py_UNREACHABLE();
    }
    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(stats.uncollectable + stats.collected);
    }
    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(gcstate, "stop", generation, &stats);
    }
    _PyErr_SetRaisedException(tstate, exc);
    GC_STAT_ADD(generation, objects_collected, stats.collected);
#ifdef Py_STATS
    if (_Py_stats) {
        GC_STAT_ADD(generation, object_visits,
            _Py_stats->object_stats.object_visits);
        _Py_stats->object_stats.object_visits = 0;
    }
#endif
    validate_old(gcstate);
    _Py_atomic_store_int(&gcstate->collecting, 0);
    return stats.uncollectable + stats.collected;
}

/* Public API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    return _PyGC_Collect(_PyThreadState_GET(), 2, _Py_GC_REASON_MANUAL);
}

void
_PyGC_CollectNoFail(PyThreadState *tstate)
{
    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    _PyGC_Collect(_PyThreadState_GET(), 2, _Py_GC_REASON_SHUTDOWN);
}

void
_PyGC_DumpShutdownStats(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    if (!(gcstate->debug & _PyGC_DEBUG_SAVEALL)
        && gcstate->garbage != NULL && PyList_GET_SIZE(gcstate->garbage) > 0) {
        const char *message;
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            message = "gc: %zd uncollectable objects at shutdown";
        }
        else {
            message = "gc: %zd uncollectable objects at shutdown; " \
                "use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        }
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(gcstate->garbage)))
        {
            PyErr_WriteUnraisable(NULL);
        }
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(gcstate->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr))) {
                PyErr_WriteUnraisable(gcstate->garbage);
            }
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}


void
_PyGC_Fini(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    Py_CLEAR(gcstate->garbage);
    Py_CLEAR(gcstate->callbacks);

    /* We expect that none of this interpreters objects are shared
       with other interpreters.
       See https://github.com/python/cpython/issues/90228. */
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
    _PyObject_Dump(FROM_GC(g));
}


#ifdef Py_DEBUG
static int
visit_validate(PyObject *op, void *parent_raw)
{
    PyObject *parent = _PyObject_CAST(parent_raw);
    if (_PyObject_IsFreed(op)) {
        _PyObject_ASSERT_FAILED_MSG(parent,
                                    "PyObject_GC_Track() object is not valid");
    }
    return 0;
}
#endif


/* extension modules might be compiled with GC support so these
   functions must always be available */

void
PyObject_GC_Track(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_ASSERT_FAILED_MSG(op,
                                    "object already tracked "
                                    "by the garbage collector");
    }
    _PyObject_GC_TRACK(op);

#ifdef Py_DEBUG
    /* Check that the object is valid: validate objects traversed
       by tp_traverse() */
    traverseproc traverse = Py_TYPE(op)->tp_traverse;
    (void)traverse(op, visit_validate, op);
#endif
}

void
PyObject_GC_UnTrack(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    /* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
     * call PyObject_GC_UnTrack twice on an object.
     */
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
}

int
PyObject_IS_GC(PyObject *obj)
{
    return _PyObject_IS_GC(obj);
}

void
_Py_ScheduleGC(PyThreadState *tstate)
{
    if (!_Py_eval_breaker_bit_is_set(tstate, _PY_GC_SCHEDULED_BIT))
    {
        _Py_set_eval_breaker_bit(tstate, _PY_GC_SCHEDULED_BIT);
    }
}

void
_PyObject_GC_Link(PyObject *op)
{
    PyGC_Head *gc = AS_GC(op);
    // gc must be correctly aligned
    _PyObject_ASSERT(op, ((uintptr_t)gc & (sizeof(uintptr_t)-1)) == 0);

    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;
    gc->_gc_next = 0;
    gc->_gc_prev = 0;
    gcstate->young.count++; /* number of allocated GC objects */
    gcstate->heap_size++;
    if (gcstate->young.count > gcstate->young.threshold &&
        gcstate->enabled &&
        gcstate->young.threshold &&
        !_Py_atomic_load_int_relaxed(&gcstate->collecting) &&
        !_PyErr_Occurred(tstate))
    {
        _Py_ScheduleGC(tstate);
    }
}

void
_Py_RunGC(PyThreadState *tstate)
{
    if (tstate->interp->gc.enabled) {
        _PyGC_Collect(tstate, 1, _Py_GC_REASON_HEAP);
    }
}

static PyObject *
gc_alloc(PyTypeObject *tp, size_t basicsize, size_t presize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (basicsize > PY_SSIZE_T_MAX - presize) {
        return _PyErr_NoMemory(tstate);
    }
    size_t size = presize + basicsize;
    char *mem = _PyObject_MallocWithType(tp, size);
    if (mem == NULL) {
        return _PyErr_NoMemory(tstate);
    }
    ((PyObject **)mem)[0] = NULL;
    ((PyObject **)mem)[1] = NULL;
    PyObject *op = (PyObject *)(mem + presize);
    _PyObject_GC_Link(op);
    return op;
}


PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_SIZE(tp);
    if (_PyType_HasFeature(tp, Py_TPFLAGS_INLINE_VALUES)) {
        size += _PyInlineValuesSize(tp);
    }
    PyObject *op = gc_alloc(tp, size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_Init(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *)gc_alloc(tp, size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_InitVar(op, tp, nitems);
    return op;
}

PyObject *
PyUnstable_Object_GC_NewWithExtraData(PyTypeObject *tp, size_t extra_size)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    PyObject *op = gc_alloc(tp, _PyObject_SIZE(tp) + extra_size, presize);
    if (op == NULL) {
        return NULL;
    }
    memset(op, 0, _PyObject_SIZE(tp) + extra_size);
    _PyObject_Init(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    const size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > (size_t)PY_SSIZE_T_MAX - presize) {
        return (PyVarObject *)PyErr_NoMemory();
    }
    char *mem = (char *)op - presize;
    mem = (char *)_PyObject_ReallocWithType(Py_TYPE(op), mem, presize + basicsize);
    if (mem == NULL) {
        return (PyVarObject *)PyErr_NoMemory();
    }
    op = (PyVarObject *) (mem + presize);
    Py_SET_SIZE(op, nitems);
    return op;
}

void
PyObject_GC_Del(void *op)
{
    size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    PyGC_Head *g = AS_GC(op);
    if (_PyObject_GC_IS_TRACKED(op)) {
        gc_list_remove(g);
#ifdef Py_DEBUG
        PyObject *exc = PyErr_GetRaisedException();
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, "Object of type %s is not untracked before destruction",
                                     ((PyObject*)op)->ob_type->tp_name)) {
            PyErr_WriteUnraisable(NULL);
        }
        PyErr_SetRaisedException(exc);
#endif
    }
    GCState *gcstate = get_gc_state();
    if (gcstate->young.count > 0) {
        gcstate->young.count--;
    }
    gcstate->heap_size--;
    PyObject_Free(((char *)op)-presize);
}

int
PyObject_GC_IsTracked(PyObject* obj)
{
    if (_PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj)) {
        return 1;
    }
    return 0;
}

int
PyObject_GC_IsFinalized(PyObject *obj)
{
    if (_PyObject_IS_GC(obj) && _PyGC_FINALIZED(obj)) {
         return 1;
    }
    return 0;
}

static int
visit_generation(gcvisitobjects_t callback, void *arg, struct gc_generation *gen)
{
    PyGC_Head *gc_list, *gc;
    gc_list = &gen->head;
    for (gc = GC_NEXT(gc_list); gc != gc_list; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);
        Py_INCREF(op);
        int res = callback(op, arg);
        Py_DECREF(op);
        if (!res) {
            return -1;
        }
    }
    return 0;
}

void
PyUnstable_GC_VisitObjects(gcvisitobjects_t callback, void *arg)
{
    GCState *gcstate = get_gc_state();
    int origenstate = gcstate->enabled;
    gcstate->enabled = 0;
    if (visit_generation(callback, arg, &gcstate->young)) {
        goto done;
    }
    if (visit_generation(callback, arg, &gcstate->old[0])) {
        goto done;
    }
    visit_generation(callback, arg, &gcstate->old[1]);
done:
    gcstate->enabled = origenstate;
}

#endif  // Py_GIL_DISABLED
