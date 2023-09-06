#include "Python.h"
#include "pycore_abstract.h"
#include "pycore_ceval.h"
#include "pycore_opcode_metadata.h"
#include "pycore_opcode_utils.h"
#include "pycore_uops.h"
#include "pycore_jit.h"

#include "ceval_macros.h"
#include "jit_stencils.h"

#ifdef MS_WINDOWS
    #include <psapi.h>
    #include <windows.h>
    FARPROC
    LOOKUP(LPCSTR symbol)
    {
        DWORD cbNeeded;
        HMODULE hMods[1024];
        HANDLE hProcess = GetCurrentProcess();
        if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
            for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
                FARPROC value = GetProcAddress(hMods[i], symbol);
                if (value) {
                    return value;
                }
            }
        }
        return NULL;
    }
#else
    #include <sys/mman.h>
    #define LOOKUP(SYMBOL) dlsym(RTLD_DEFAULT, (SYMBOL))
#endif

#define MB (1 << 20)
#define JIT_POOL_SIZE  (128 * MB)

static unsigned char pool[JIT_POOL_SIZE];
static size_t pool_head;
static size_t page_size;

static unsigned char *
alloc(size_t size)
{
    size += (16 - (size & 15)) & 15;
    if (JIT_POOL_SIZE - page_size < pool_head + size) {
        return NULL;
    }
    unsigned char *memory = pool + pool_head;
    pool_head += size;
    assert(((uintptr_t)(pool + pool_head) & 15) == 0);
    assert(((uintptr_t)memory & 15) == 0);
    return memory;
}

static int initialized = 0;

static void
patch_one(unsigned char *location, HoleKind kind, uint64_t value, uint64_t addend)
{
    switch (kind) {
        case PATCH_ABS_12: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction & 0x3B000000) == 0x39000000) ||
                   ((instruction & 0x11C00000) == 0x11000000));
            value = (value + addend) & ((1 << 12) - 1);
            int implicit_shift = 0;
            if ((instruction & 0x3B000000) == 0x39000000) {
                implicit_shift = ((instruction >> 30) & 0x3);
                // XXX: We shouldn't have to rewrite these (we *should* be
                // able to just assert the alignment), but something's up with
                // aarch64 + ELF (at least with LLVM 14, I haven't tested 15
                // and 16):
                switch (implicit_shift) {
                    case 3:
                        if ((value & 0x7) == 0) {
                            break;
                        }
                        implicit_shift = 2;
                        instruction = (instruction & ~(0x3UL << 30)) | (implicit_shift << 30);
                        // Fall through...
                    case 2:
                        if ((value & 0x3) == 0) {
                            break;
                        }
                        implicit_shift = 1;
                        instruction = (instruction & ~(0x3UL << 30)) | (implicit_shift << 30);
                        // Fall through...
                    case 1:
                        if ((value & 0x1) == 0) {
                            break;
                        }
                        implicit_shift = 0;
                        instruction = (instruction & ~(0x3UL << 30)) | (implicit_shift << 30);
                        // Fall through...
                    case 0:
                        if ((instruction & 0x04800000) == 0x04800000) {
                            implicit_shift = 4;
                            assert(((value & 0xF) == 0));
                        }
                        break;
                }
            }
            value >>= implicit_shift;
            assert((value & ((1 << 12) - 1)) == value);
            instruction = (instruction & 0xFFC003FF) | ((uint32_t)(value << 10) & 0x003FFC00);
            assert(((instruction & 0x3B000000) == 0x39000000) ||
                   ((instruction & 0x11C00000) == 0x11000000));
            *addr = instruction;
            break;
        }
        case PATCH_ABS_16_A: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction >> 21) & 0x3) == 0);
            instruction = (instruction & 0xFFE0001F) | ((((value + addend) >> 0) & 0xFFFF) << 5);
            *addr = instruction;
            break;
        }
        case PATCH_ABS_16_B: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction >> 21) & 0x3) == 1);
            instruction = (instruction & 0xFFE0001F) | ((((value + addend) >> 16) & 0xFFFF) << 5);
            *addr = instruction;
            break;
        }
        case PATCH_ABS_16_C: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction >> 21) & 0x3) == 2);
            instruction = (instruction & 0xFFE0001F) | ((((value + addend) >> 32) & 0xFFFF) << 5);
            *addr = instruction;
            break;
        }
        case PATCH_ABS_16_D: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction >> 21) & 0x3) == 3);
            instruction = (instruction & 0xFFE0001F) | ((((value + addend) >> 48) & 0xFFFF) << 5);
            *addr = instruction;
            break;
        }
        case PATCH_ABS_32: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            instruction = value + addend;
            *addr = instruction;
            break;
        }
        case PATCH_ABS_64: {
            uint64_t *addr = (uint64_t *)location;
            uint64_t instruction = *addr;
            instruction = value + addend;
            *addr = instruction;
            break;
        }
        case PATCH_REL_21: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert((instruction & 0x9F000000) == 0x90000000);
            value = (((value + addend) >> 12) << 12) - (((uintptr_t)location >> 12) << 12);
            assert((value & 0xFFF) == 0);
            // assert((value & ((1ULL << 33) - 1)) == value);  // XXX: This should be signed.
            uint32_t lo = ((uint64_t)value << 17) & 0x60000000;
            uint32_t hi = ((uint64_t)value >> 9) & 0x00FFFFE0;
            instruction = (instruction & 0x9F00001F) | hi | lo;
            assert((instruction & 0x9F000000) == 0x90000000);
            *addr = instruction;
            break;
        }
        case PATCH_REL_26: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            assert(((instruction & 0xFC000000) == 0x14000000) ||
                   ((instruction & 0xFC000000) == 0x94000000));
            value = value + addend - (uintptr_t)location;
            assert((value & 0x3) == 0);
            // assert((value & ((1ULL << 29) - 1)) == value);  // XXX: This should be signed.
            instruction = (instruction & 0xFC000000) | ((uint32_t)(value >> 2) & 0x03FFFFFF);
            assert(((instruction & 0xFC000000) == 0x14000000) ||
                   ((instruction & 0xFC000000) == 0x94000000));
            *addr = instruction;
            break;
        }
        case PATCH_REL_32: {
            uint32_t *addr = (uint32_t *)location;
            uint32_t instruction = *addr;
            instruction = value + addend - (uintptr_t)location;
            *addr = instruction;
            break;
        }
        case PATCH_REL_64: {
            uint64_t *addr = (uint64_t *)location;
            uint64_t instruction = *addr;
            instruction = value + addend - (uintptr_t)location;
            *addr = instruction;
            break;
        }
        default: {
            printf("XXX: %d!\n", kind);
            Py_UNREACHABLE();
        }
    }
}

static void
copy_and_patch(unsigned char *memory, const Stencil *stencil, uintptr_t patches[])
{
    memcpy(memory, stencil->bytes, stencil->nbytes);
    for (size_t i = 0; i < stencil->nholes; i++) {
        const Hole *hole = &stencil->holes[i];
        patch_one(memory + hole->offset, hole->kind, patches[hole->value], hole->addend);
    }
    for (size_t i = 0; i < stencil->nloads; i++) {
        const SymbolLoad *load = &stencil->loads[i];
        uintptr_t value = symbol_addresses[load->symbol];
        patch_one(memory + load->offset, load->kind, value, load->addend);
    }
}

// The world's smallest compiler?
_PyJITFunction
_PyJIT_CompileTrace(_PyUOpInstruction *trace, int size)
{
    if (initialized < 0) {
        return NULL;
    }
    if (initialized == 0) {
        initialized = -1;
        for (size_t i = 0; i < Py_ARRAY_LENGTH(symbols); i++) {
            symbol_addresses[i] = (uintptr_t)LOOKUP(symbols[i]);
            if (symbol_addresses[i] == 0) {
                const char *w = "JIT initialization failed (can't find symbol \"%s\")";
                PyErr_WarnFormat(PyExc_RuntimeWarning, 0, w, symbols[i]);
                return NULL;
            }
        }
#ifdef MS_WINDOWS
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_size = si.dwPageSize;
#else
        page_size = sysconf(_SC_PAGESIZE);
#endif
        assert(page_size);
        assert((page_size & (page_size - 1)) == 0);
        pool_head = (page_size - ((uintptr_t)pool & (page_size - 1))) & (page_size - 1);
        assert(((uintptr_t)(pool + pool_head) & (page_size - 1)) == 0);
#ifdef __APPLE__
        void *mapped = mmap(pool + pool_head, JIT_POOL_SIZE - pool_head - page_size,
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE, -1, 0);
        if (mapped == MAP_FAILED) {
            const char *w = "JIT unable to map fixed memory (%d)";
            PyErr_WarnFormat(PyExc_RuntimeWarning, 0, w, errno);
            return NULL;
        }
        assert(mapped == pool + pool_head);
#endif
        initialized = 1;
    }
    assert(initialized > 0);
    int *offsets = PyMem_Malloc(size * sizeof(int));
    if (offsets == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    // First, loop over everything once to find the total compiled size:
    size_t nbytes = trampoline_stencil.nbytes;
    for (int i = 0; i < size; i++) {
        offsets[i] = nbytes;
        _PyUOpInstruction *instruction = &trace[i];
        if (OPARG_MAX < instruction->oparg) {
            PyMem_Free(offsets);
            return NULL;
        }
        const Stencil *stencil = &stencils[instruction->opcode][instruction->oparg];
        if (stencil->nbytes == 0) {
            PyMem_Free(offsets);
            return NULL;
        }
        nbytes += stencil->nbytes;
    };
    unsigned char *memory = alloc(nbytes);
    if (memory == NULL) {
        PyErr_WarnEx(PyExc_RuntimeWarning, "JIT out of memory", 0);
        PyMem_Free(offsets);
        return NULL;
    }
    unsigned char *page = (unsigned char *)((uintptr_t)memory & ~(page_size - 1));
    size_t page_nbytes = memory + nbytes - page;
#ifdef MS_WINDOWS
    DWORD old;
    if (!VirtualProtect(page, page_nbytes, PAGE_READWRITE, &old)) {
        int code = GetLastError();
#else
    if (mprotect(page, page_nbytes, PROT_READ | PROT_WRITE)) {
        int code = errno;
#endif
        const char *w = "JIT unable to map writable memory (%d)";
        PyErr_WarnFormat(PyExc_RuntimeWarning, 0, w, code);
        PyMem_Free(offsets);
        return NULL;
    }
    unsigned char *head = memory;
    uintptr_t patches[] = GET_PATCHES();
    // First, the trampoline:
    const Stencil *stencil = &trampoline_stencil;
    patches[HOLE_base] = (uintptr_t)head;
    patches[HOLE_continue] = (uintptr_t)head + stencil->nbytes;
    copy_and_patch(head, stencil, patches);
    head += stencil->nbytes;
    // Then, all of the stencils:
    for (int i = 0; i < size; i++) {
        _PyUOpInstruction *instruction = &trace[i];
        const Stencil *stencil = &stencils[instruction->opcode][instruction->oparg];
        patches[HOLE_base] = (uintptr_t)head;
        patches[HOLE_branch] = (uintptr_t)memory + offsets[instruction->oparg % size];
        patches[HOLE_continue] = (uintptr_t)head + stencil->nbytes;
        patches[HOLE_loop] = (uintptr_t)memory + trampoline_stencil.nbytes;
        patches[HOLE_operand_plus_one] = instruction->operand + 1;
        copy_and_patch(head, stencil, patches);
        head += stencil->nbytes;
    };
    PyMem_Free(offsets);
#ifdef MS_WINDOWS
    if (!FlushInstructionCache(GetCurrentProcess(), memory, nbytes) ||
        !VirtualProtect(page, page_nbytes, PAGE_EXECUTE_READ, &old))
    {
        int code = GetLastError();
#else
    __builtin___clear_cache((char *)memory, (char *)memory + nbytes);
    if (mprotect(page, page_nbytes, PROT_EXEC | PROT_READ)) {
        int code = errno;
#endif
        const char *w = "JIT unable to map executable memory (%d)";
        PyErr_WarnFormat(PyExc_RuntimeWarning, 0, w, code);
        return NULL;
    }
    // Wow, done already?
    assert(memory + nbytes == head);
    return (_PyJITFunction)memory;
}
