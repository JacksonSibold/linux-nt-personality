/*
 * vcruntime140.c - vcruntime140.dll: what code built with the Microsoft C++ compiler needs from its runtime on x86-64.
 *
 * C++ exceptions: _CxxThrowException raises the exception; __CxxFrameHandler3 is the language handler named in the unwind
 * data of functions with try blocks or objects to destroy. The decisions come from vcr_eh_pure.h (host-tested); this file
 * applies them to a real stack.
 *
 * How a catch runs. The first pass finds the catch block in the frame handler. The thrown object is then moved to the heap
 * (the frames that hold it are about to be unwound), the catch parameter is built from it, and RtlUnwindEx unwinds to the
 * catching frame with vcr_catch_entry as the target. That small routine looks like a call made from the catching frame: it
 * runs the catch funclet, then jumps to the address the funclet returns. While the funclet runs, the exception is "active"
 * and is the one a bare `throw;` raises again; when the funclet returns, or an exception leaves it, the object is released
 * (unless the exception is the same one on its way out again).
 *
 * Not done: the C++/CLI and WinRT exceptions, the SEH translator, FH4 (vcruntime140_1.dll). Objects thrown are moved with a
 * plain copy of their bytes, so a type that points into itself does not survive a throw.
 * Nothing in this DLL has been run against code built by the Microsoft compiler: the table logic is host-tested on built
 * tables, and the DLL is only built and link-checked.
 */
#ifdef __MINGW32__
#include "except.h"
#include "nt_types.h"
#include "ntstatus.h"
#include "unwind.h"
#include "vcr_eh_pure.h"
#include "vcr_typeinfo_pure.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WINAPI
#define WINAPI __stdcall
#endif

#define VCR_STATUS_LONGJUMP 0x80000026u
#define VCR_THREAD_DETACH 3
#define VCR_PROCESS_ATTACH 1
#define VCR_PROCESS_DETACH 0

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
DWORD WINAPI TlsAlloc(void);
BOOL WINAPI TlsFree(DWORD slot);
PVOID WINAPI TlsGetValue(DWORD slot);
BOOL WINAPI TlsSetValue(DWORD slot, PVOID value);
void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);
BOOL WINAPI GetModuleHandleExW(DWORD flags, const void *addr, HANDLE *out);
void __cdecl terminate(void);

extern char __ImageBase;

void vcr_exc_init(void);
void vcr_catch_entry(void);
void *vcr_call_funclet(void *handler, ULONG64 frame, ULONG flags);

/* ---- Per-thread state -------------------------------------------------------------------------------------------------------- */

/* A thrown object that has been moved to the heap. The count is the number of catch blocks (running or about to) holding it. */
typedef struct vcr_obj {
    int refs;
    const vcr_throwinfo *ti;
    const char *base;
    size_t size;
    char data[] __attribute__((aligned(16)));
} vcr_obj;

typedef struct vcr_exc {
    struct vcr_exc *next;
    EXCEPTION_RECORD rec; /* the exception as it will be raised again by a bare throw */
    ULONG64 handler;
    ULONG64 orig_frame;
    int32_t start_level;
    const vcr_funcinfo *descr;
    vcr_obj *obj;
} vcr_exc;

typedef struct {
    vcr_exc *pending; /* catches chosen whose unwinding has not finished */
    vcr_exc *active;  /* catch funclets running, innermost first */
    int processing_throw;
    PVOID current_exception; /* EXCEPTION_RECORD * of the innermost active catch */
    PVOID current_context;
} vcr_tls;

static DWORD g_vcr_tls_slot = 0xFFFFFFFFu;

static void *vcr_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, n ? n : 1); }
static void vcr_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

static vcr_tls *vcr_tls_get(void)
{
    vcr_tls *t;
    if (g_vcr_tls_slot == 0xFFFFFFFFu)
        g_vcr_tls_slot = TlsAlloc();
    t = (vcr_tls *)TlsGetValue(g_vcr_tls_slot);
    if (!t) {
        t = (vcr_tls *)vcr_alloc(sizeof *t);
        if (!t)
            terminate();
        TlsSetValue(g_vcr_tls_slot, t);
    }
    return t;
}

BOOL WINAPI DllMainCRTStartup(HANDLE h, DWORD reason, PVOID reserved)
{
    (void)h;
    (void)reserved;
    if (reason == VCR_PROCESS_ATTACH) {
        g_vcr_tls_slot = TlsAlloc();
        vcr_exc_init();
    } else if (reason == VCR_THREAD_DETACH && g_vcr_tls_slot != 0xFFFFFFFFu) {
        vcr_free(TlsGetValue(g_vcr_tls_slot));
        TlsSetValue(g_vcr_tls_slot, NULL);
    }
    return TRUE;
}

BOOL WINAPI _DllMainCRTStartup(HANDLE h, DWORD reason, PVOID reserved) { return DllMainCRTStartup(h, reason, reserved); }

void **__cdecl __current_exception(void) { return &vcr_tls_get()->current_exception; }
void **__cdecl __current_exception_context(void) { return &vcr_tls_get()->current_context; }
int *__cdecl __processing_throw(void) { return &vcr_tls_get()->processing_throw; }

/* ---- Calling cleanup and catch funclets ------------------------------------------------------------------------------------- */

/* A funclet is called with the handler address in rcx, the frame pointer of the function it belongs to in rdx and 0x100 in
 * r8 (the second and third argument slots in the caller's home area carry the same, as the compiler's funclets expect). */
__asm__(".text\n\t"
        ".globl vcr_call_funclet\n\t"
        ".def vcr_call_funclet; .scl 2; .type 32; .endef\n\t"
        ".seh_proc vcr_call_funclet\n"
        "vcr_call_funclet:\n\t"
        "subq $0x28, %rsp\n\t"
        ".seh_stackalloc 0x28\n\t"
        ".seh_endprologue\n\t"
        "movq %rcx, 0x0(%rsp)\n\t"
        "movl %r8d, 0x8(%rsp)\n\t"
        "movq %rdx, 0x10(%rsp)\n\t"
        "callq *%rcx\n\t"
        "addq $0x28, %rsp\n\t"
        "ret\n\t"
        ".seh_endproc\n\t");

/* The target of the unwind to a catching frame. Execution arrives here with the registers of that frame, its stack pointer
 * unchanged and the address its call would have returned to in rax; pushing that address makes this look like a call made
 * from the frame. */
__asm__(".text\n\t"
        ".globl vcr_catch_entry\n\t"
        ".def vcr_catch_entry; .scl 2; .type 32; .endef\n"
        "vcr_catch_entry:\n\t"
        "pushq %rax\n\t"
        ".seh_proc vcr_catch_body\n"
        "vcr_catch_body:\n\t"
        "subq $0x28, %rsp\n\t"
        ".seh_stackalloc 0x28\n\t"
        ".seh_handler vcr_catch_frame_handler, @unwind, @except\n\t"
        ".seh_endprologue\n\t"
        "call vcr_catch_run\n\t"
        "addq $0x28, %rsp\n\t"
        "addq $8, %rsp\n\t"
        "jmp *%rax\n\t"
        ".seh_endproc\n\t");

/* ---- The thrown object ------------------------------------------------------------------------------------------------------- */

/* A thrown object that is already on the heap because an earlier catch holds it (a bare throw). */
static vcr_obj *vcr_find_held(vcr_tls *t, const void *object)
{
    vcr_exc *e;
    for (e = t->active; e; e = e->next)
        if (e->obj && (const void *)e->obj->data == object)
            return e->obj;
    for (e = t->pending; e; e = e->next)
        if (e->obj && (const void *)e->obj->data == object)
            return e->obj;
    return NULL;
}

static void vcr_obj_release(vcr_obj *o)
{
    if (!o || --o->refs > 0)
        return;
    if (o->ti && o->ti->pmfnUnwind)
        ((void(__cdecl *)(void *))(uintptr_t)(o->base + o->ti->pmfnUnwind))(o->data);
    vcr_free(o);
}

/* ---- Throwing -------------------------------------------------------------------------------------------------------------------- */

void __cdecl _CxxThrowException(void *object, const vcr_throwinfo *ti)
{
    ULONG_PTR args[4];
    HANDLE module = NULL;
    vcr_tls *t = vcr_tls_get();

    if (!object && !ti) { /* a bare throw raises the exception being handled again */
        vcr_exc *e = t->active;
        if (!e)
            terminate();
        RaiseException(e->rec.ExceptionCode, e->rec.ExceptionFlags, e->rec.NumberParameters, e->rec.ExceptionInformation);
        terminate();
    }
    if (ti)
        GetModuleHandleExW(0x6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, ti, &module);
    args[0] = VCR_EH_MAGIC_VC6;
    args[1] = (ULONG_PTR)object;
    args[2] = (ULONG_PTR)ti;
    args[3] = (ULONG_PTR)module;
    RaiseException(VCR_CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 4, args);
    terminate();
}

/* ---- Funclet calls from the tables ------------------------------------------------------------------------------------------- */

typedef struct {
    const char *base;
    ULONG64 frame;
} vcr_action_ctx;

static void vcr_run_action(void *ctx, uint32_t rva)
{
    const vcr_action_ctx *c = (const vcr_action_ctx *)ctx;
    vcr_call_funclet((void *)(uintptr_t)(c->base + rva), c->frame, 0x100);
}

static void vcr_copy_ctor_cb(void *ctx, uint32_t rva, void *dest, void *src, int has_virtual_base)
{
    const char *base = (const char *)ctx;
    void *fn = (void *)(uintptr_t)(base + rva);
    if (has_virtual_base)
        ((void(__cdecl *)(void *, void *, int))fn)(dest, src, 1);
    else
        ((void(__cdecl *)(void *, void *))fn)(dest, src);
}

/* Runs the cleanup actions of the function at `frame` from where it currently stands down to last_level. The frame keeps the
 * level it reached in its unwind help slot so that a later unwind of the same frame does not run an action twice. */
static void vcr_local_unwind(ULONG64 frame, const DISPATCHER_CONTEXT *dc, const vcr_funcinfo *descr, int32_t last_level)
{
    const char *base = (const char *)(uintptr_t)dc->ImageBase;
    int32_t *help = (int32_t *)(uintptr_t)(frame + (int64_t)descr->unwind_help);
    int32_t level = help[0];
    vcr_action_ctx c;
    c.base = base;
    c.frame = frame;
    if (level == -2)
        level = vcr_eh_ip_state(base, descr, (uint32_t)(dc->ControlPc - dc->ImageBase));
    level = vcr_eh_local_unwind(base, descr, level, last_level, vcr_run_action, &c);
    if (level == -2)
        terminate();
    help[0] = level;
}

/* ---- Catching ----------------------------------------------------------------------------------------------------------------- */

/* Runs on the stack of the catching frame, called from vcr_catch_entry. */
void *vcr_catch_run(void)
{
    vcr_tls *t = vcr_tls_get();
    vcr_exc *e = t->pending;
    void *resume;
    int32_t *help;

    if (!e)
        terminate();
    t->pending = e->next;
    e->next = t->active;
    t->active = e;
    t->current_exception = &e->rec;
    if (t->processing_throw > 0)
        --t->processing_throw;

    resume = vcr_call_funclet((void *)(uintptr_t)e->handler, e->orig_frame, 0x100);

    t->active = e->next;
    t->current_exception = t->active ? &t->active->rec : NULL;
    help = (int32_t *)(uintptr_t)(e->orig_frame + (int64_t)e->descr->unwind_help);
    help[0] = -2;
    help[1] = -1;
    vcr_obj_release(e->obj);
    vcr_free(e);
    return resume;
}

/* The unwind handler of vcr_catch_entry's frame: an exception leaving the catch funclet ends the catch. If it is the same
 * object going out again (a bare throw) the object stays alive for the next catch. */
EXCEPTION_DISPOSITION vcr_catch_frame_handler(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PVOID dispatch)
{
    vcr_tls *t;
    vcr_exc *e;
    (void)frame;
    (void)ctx;
    (void)dispatch;
    if (!(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)))
        return ExceptionContinueSearch;
    t = vcr_tls_get();
    e = t->active;
    if (!e)
        return ExceptionContinueSearch;
    t->active = e->next;
    t->current_exception = t->active ? &t->active->rec : NULL;
    if (e->obj && rec->ExceptionCode == VCR_CXX_EXCEPTION && rec->NumberParameters >= 2 &&
        rec->ExceptionInformation[1] == (ULONG_PTR)e->obj->data) {
        --e->obj->refs; /* on its way out again: not destroyed here */
    } else {
        vcr_obj_release(e->obj);
    }
    vcr_free(e);
    return ExceptionContinueSearch;
}

static void vcr_start_catch(PEXCEPTION_RECORD rec, ULONG64 frame, const DISPATCHER_CONTEXT *dc, const vcr_funcinfo *descr,
                            const vcr_catch_match *m, const char *exc_base, ULONG64 orig_frame)
{
    const char *base = (const char *)(uintptr_t)dc->ImageBase;
    vcr_tls *t = vcr_tls_get();
    vcr_exc *e = (vcr_exc *)vcr_alloc(sizeof *e);
    void *object = (void *)rec->ExceptionInformation[1];

    if (!e)
        terminate();
    e->rec = *rec;
    if (m->catchable) {
        vcr_obj *held = vcr_find_held(t, object);
        if (!held) { /* the frames holding the object are about to be unwound: take it to the heap */
            const vcr_throwinfo *ti = (const vcr_throwinfo *)rec->ExceptionInformation[2];
            const vcr_catchable_array *arr = (const vcr_catchable_array *)(exc_base + ti->pCatchableTypeArray);
            const vcr_catchable *whole = (const vcr_catchable *)(exc_base + arr->types[0]);
            size_t size = (size_t)whole->sizeOrOffset;
            held = (vcr_obj *)vcr_alloc(sizeof(vcr_obj) + size);
            if (!held)
                terminate();
            held->ti = ti;
            held->base = exc_base;
            held->size = size;
            memmove(held->data, object, size);
        }
        ++held->refs;
        e->obj = held;
        object = held->data;
        e->rec.ExceptionInformation[1] = (ULONG_PTR)object;
        if (m->catchblock->type_info && m->catchblock->offset)
            vcr_eh_copy_catch_object(exc_base, object, (void *)(uintptr_t)(orig_frame + (int64_t)m->catchblock->offset),
                                     m->catchblock->flags, m->catchable, vcr_copy_ctor_cb, (void *)exc_base);
    }
    e->handler = (ULONG64)(uintptr_t)(base + m->catchblock->handler);
    e->orig_frame = orig_frame;
    e->start_level = m->tryblock->start_level;
    e->descr = descr;
    e->next = t->pending;
    t->pending = e;

    /* Unwind to the catching frame; control continues in vcr_catch_entry. The return value register carries the address the
     * frame's call would return to. */
    RtlUnwindEx((PVOID)(uintptr_t)frame, (PVOID)(uintptr_t)vcr_catch_entry, rec, (PVOID)(uintptr_t)dc->ControlPc, dc->ContextRecord,
                dc->HistoryTable);

    /* RtlUnwindEx does not return unless it could not unwind. */
    t->pending = e->next;
    vcr_obj_release(e->obj);
    vcr_free(e);
}

static void vcr_find_catch_block(PEXCEPTION_RECORD rec, ULONG64 frame, const DISPATCHER_CONTEXT *dc, const vcr_funcinfo *descr,
                                 const vcr_throwinfo *ti, ULONG64 orig_frame)
{
    const char *base = (const char *)(uintptr_t)dc->ImageBase;
    const char *exc_base = ti ? (const char *)(uintptr_t)(rec->NumberParameters == 4 ? rec->ExceptionInformation[3] : dc->ImageBase) : base;
    int32_t trylevel = vcr_eh_ip_state(base, descr, (uint32_t)(dc->ControlPc - dc->ImageBase));
    int32_t *help = (int32_t *)(uintptr_t)(orig_frame + (int64_t)descr->unwind_help);
    vcr_tls *t = vcr_tls_get();
    const vcr_tryblock *in_catch;
    vcr_catch_match m;

    ++t->processing_throw;
    in_catch = vcr_eh_in_catch(base, descr, trylevel);
    /* the state never goes back down while an exception is being handled in this frame */
    if (trylevel > help[1]) {
        help[0] = help[1] = trylevel;
    } else {
        trylevel = help[1];
    }
    if (vcr_eh_find_catch(exc_base, base, descr, trylevel, in_catch, ti, &m))
        vcr_start_catch(rec, frame, dc, descr, &m, exc_base, orig_frame);
    if (t->processing_throw > 0)
        --t->processing_throw;
}

static EXCEPTION_DISPOSITION vcr_frame_handler(PEXCEPTION_RECORD rec, ULONG64 frame, const DISPATCHER_CONTEXT *dc,
                                               const vcr_funcinfo *descr)
{
    const char *base = (const char *)(uintptr_t)dc->ImageBase;
    const uint32_t magic = vcr_eh_magic(descr);
    const int32_t trylevel = vcr_eh_ip_state(base, descr, (uint32_t)(dc->ControlPc - dc->ImageBase));
    const int catch_unwind = dc->TargetIp == (ULONG64)(uintptr_t)vcr_catch_entry;
    ULONG64 orig_frame = frame;
    int32_t unwindlevel = -1;
    vcr_nested n;

    if (magic < VCR_EH_MAGIC_VC6 || magic > VCR_EH_MAGIC_VC8)
        return ExceptionContinueSearch;
    /* a function built for synchronous exceptions only ignores the others, except while unwinding to one of our catches */
    if (magic >= VCR_EH_MAGIC_VC8 && (descr->flags & VCR_EH_SYNCHRONOUS) && rec->ExceptionCode != VCR_CXX_EXCEPTION && !catch_unwind &&
        rec->ExceptionCode != VCR_STATUS_LONGJUMP)
        return ExceptionContinueSearch;

    /* code in a catch funclet runs in a frame of its own; the function's frame pointer is stored in it */
    n = vcr_eh_nested(base, descr, trylevel, dc->FunctionEntry->BeginAddress);
    if (n.found) {
        unwindlevel = n.unwind_level;
        orig_frame = *(const ULONG64 *)(uintptr_t)(frame + (int64_t)n.frame_offset);
    }

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if (descr->unwind_count) {
            if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) {
                vcr_tls *t = vcr_tls_get();
                vcr_local_unwind(orig_frame, dc, descr, catch_unwind && t->pending ? t->pending->start_level : trylevel);
            } else {
                vcr_local_unwind(orig_frame, dc, descr, unwindlevel);
            }
        }
        return ExceptionContinueSearch;
    }

    if (descr->tryblock_count) {
        const vcr_throwinfo *ti = NULL;
        if (rec->ExceptionCode == VCR_CXX_EXCEPTION) {
            ULONG_PTR magic0 = rec->ExceptionInformation[0];
            if (magic0 >= VCR_EH_MAGIC_VC6 && magic0 <= VCR_EH_MAGIC_VC8)
                ti = (const vcr_throwinfo *)rec->ExceptionInformation[2];
        }
        vcr_find_catch_block(rec, frame, dc, descr, ti, orig_frame);
    }
    /* an exception leaving a noexcept function ends the program */
    if (orig_frame == frame && rec->ExceptionCode == VCR_CXX_EXCEPTION && magic >= VCR_EH_MAGIC_VC8 && (descr->flags & VCR_EH_NOEXCEPT))
        terminate();
    return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler3(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PVOID dispatch)
{
    const DISPATCHER_CONTEXT *dc = (const DISPATCHER_CONTEXT *)dispatch;
    const char *base = (const char *)(uintptr_t)dc->ImageBase;
    (void)ctx;
    return vcr_frame_handler(rec, (ULONG64)(uintptr_t)frame, dc, (const vcr_funcinfo *)(base + *(const uint32_t *)dc->HandlerData));
}

/* Runs the destructor of a thrown object that is not held by a catch. */
void __cdecl __DestructExceptionObject(PEXCEPTION_RECORD rec)
{
    const vcr_throwinfo *ti;
    void *object;
    if (!rec || rec->NumberParameters < 3)
        return;
    object = (void *)rec->ExceptionInformation[1];
    ti = (const vcr_throwinfo *)rec->ExceptionInformation[2];
    if (ti && ti->pmfnUnwind && rec->NumberParameters >= 4)
        ((void(__cdecl *)(void *))(uintptr_t)((const char *)rec->ExceptionInformation[3] + ti->pmfnUnwind))(object);
}

void *__cdecl __AdjustPointer(void *obj, const vcr_pmd *d) { return vcr_adjust_pointer(obj, d); }

/* ---- Run-time type information ----------------------------------------------------------------------------------------------- */

void __cdecl vcr_throw_bad_typeid(void);
void __cdecl vcr_throw_bad_cast(void);
void __cdecl vcr_throw_non_rtti(void);

const vcr_typedesc *__cdecl __RTtypeid(void *inptr)
{
    if (!inptr)
        vcr_throw_bad_typeid();
    return vcr_typeid(inptr);
}

void *__cdecl __RTCastToVoid(void *inptr)
{
    if (!inptr)
        return NULL;
    return vcr_cast_to_void(inptr);
}

void *__cdecl __RTDynamicCast(void *inptr, LONG vfdelta, const vcr_typedesc *src, const vcr_typedesc *target, BOOL is_reference)
{
    void *r;
    (void)vfdelta;
    if (!inptr)
        return NULL;
    r = vcr_dynamic_cast(inptr, src, target, NULL);
    if (!r && is_reference)
        vcr_throw_bad_cast();
    return r;
}

/* ---- type_info ----------------------------------------------------------------------------------------------------------------- */

typedef struct vcr_type_node {
    char *name;
    struct vcr_type_node *next;
} vcr_type_node;

int __cdecl __std_type_info_compare(const vcr_typedesc *a, const vcr_typedesc *b) { return vcr_ti_compare(a->name, b->name); }

size_t __cdecl __std_type_info_hash(const vcr_typedesc *d) { return (size_t)vcr_ti_hash(d->name); }

/* The readable name, made on first use and kept in the descriptor's cache slot; the node list lets the owner free it. */
const char *__cdecl __std_type_info_name(vcr_typedesc *d, vcr_type_node **root)
{
    const char *cached = (const char *)__atomic_load_n((void *const *)&d->spare, __ATOMIC_ACQUIRE);
    if (!cached) {
        char buf[VCR_UN_STR];
        size_t n = vcr_ti_undecorate(d->name, buf, sizeof buf);
        const char *src = n ? buf : d->name + 1;
        size_t len = n ? n : strlen(d->name + 1);
        char *copy = (char *)vcr_alloc(len + 1);
        vcr_type_node *node = (vcr_type_node *)vcr_alloc(sizeof *node);
        void *expected = NULL;
        if (!copy || !node) {
            vcr_free(copy);
            vcr_free(node);
            return d->name + 1;
        }
        memcpy(copy, src, len);
        copy[len] = 0;
        if (!__atomic_compare_exchange_n((void **)&d->spare, &expected, copy, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            vcr_free(copy);
            vcr_free(node);
            return (const char *)expected;
        }
        node->name = copy;
        node->next = *root;
        *root = node; /* a racing push could lose a node; the names then leak at exit, which is harmless */
        cached = copy;
    }
    return cached;
}

void __cdecl __std_type_info_destroy_list(vcr_type_node *root)
{
    while (root) {
        vcr_type_node *next = root->next;
        vcr_free(root->name);
        vcr_free(root);
        root = next;
    }
}

/* ---- Exception data --------------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *what;
    char do_free;
} vcr_exception_data;

void __cdecl __std_exception_copy(const vcr_exception_data *from, vcr_exception_data *to)
{
    if (!from->do_free || !from->what) {
        to->what = from->what;
        to->do_free = 0;
    } else {
        size_t len = strlen(from->what) + 1;
        char *copy = (char *)vcr_alloc(len);
        if (copy)
            memcpy(copy, from->what, len);
        to->what = copy;
        to->do_free = copy != NULL;
    }
}

void __cdecl __std_exception_destroy(vcr_exception_data *d)
{
    if (d->do_free)
        vcr_free((void *)d->what);
    d->what = NULL;
    d->do_free = 0;
}

void __cdecl __std_terminate(void) { terminate(); }

/* ---- Small runtime hooks --------------------------------------------------------------------------------------------------- */

typedef LONG(__stdcall *vcr_unw_fn)(void);

void __cdecl _local_unwind(PVOID frame, PVOID target) { RtlUnwind(frame, target, NULL, NULL); }

void __cdecl __telemetry_main_invoke_trigger(PVOID instance) { (void)instance; }
void __cdecl __telemetry_main_return_trigger(PVOID instance) { (void)instance; }

uint64_t __security_cookie_value = 0x2B992DDFA232ull;

void __cdecl __security_init_cookie(void) {}

/* A stack cookie mismatch ends the process at once, as the fast-fail interrupt would. */
void __cdecl __report_gsfailure(ULONG_PTR cookie)
{
    (void)cookie;
    __asm__ volatile("int $0x29" : : "c"(2));
    for (;;) {
    }
}

char *__cdecl __unDName(char *out, const char *mangled, int len, void *(__cdecl *alloc)(size_t), void(__cdecl *dealloc)(void *), unsigned short flags)
{
    (void)out;
    (void)mangled;
    (void)len;
    (void)alloc;
    (void)dealloc;
    (void)flags;
    return NULL;
}

char *__cdecl __unDNameEx(char *out, const char *mangled, int len, void *(__cdecl *alloc)(size_t), void(__cdecl *dealloc)(void *),
                          char *(__cdecl *getparam)(long), unsigned long flags)
{
    (void)getparam;
    return __unDName(out, mangled, len, alloc, dealloc, (unsigned short)flags);
}

#endif /* __MINGW32__ */
