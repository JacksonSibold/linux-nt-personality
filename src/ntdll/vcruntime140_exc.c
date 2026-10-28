/*
 * vcruntime140_exc.c - the C++ standard exception classes that vcruntime140.dll defines for the Microsoft ABI on x86-64:
 * std::exception, bad_cast, bad_typeid, __non_rtti_object, bad_exception, bad_alloc, bad_array_new_length, and type_info's
 * members. Programs construct these through the exported constructors (by their decorated names, see vcruntime140.def) and
 * call what() through the vftable, so the layout is fixed: a vftable pointer, the message pointer and a "free it" flag.
 *
 * Each class has the run-time type information a catch or dynamic_cast reads (built when the DLL is attached, because those
 * records hold offsets from the image base) and a throw descriptor for the exceptions the runtime itself throws.
 */
#ifdef __MINGW32__
#include "nt_types.h"
#include "vcr_rtti_pure.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WINAPI
#define WINAPI __stdcall
#endif

HANDLE WINAPI GetProcessHeap(void);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);

extern char __ImageBase;

void __cdecl _CxxThrowException(void *object, const vcr_throwinfo *ti);
void __cdecl __std_exception_copy(const void *from, void *to);
void __cdecl __std_exception_destroy(void *data);
void __cdecl __std_type_info_destroy_list(void *root);

typedef struct {
    const void *vptr;
    const char *what;
    char do_free;
} vcr_exception_obj;

enum { C_EXCEPTION, C_BAD_CAST, C_BAD_TYPEID, C_NON_RTTI, C_BAD_EXCEPTION, C_BAD_ALLOC, C_BAD_ARRAY, C_TYPE_INFO, C_COUNT };

typedef struct {
    const char *decorated;
    int nbases; /* including itself */
    int bases[3];
    const char *default_what;
} vcr_class;

static const vcr_class g_classes[C_COUNT] = {
    {".?AVexception@std@@", 1, {C_EXCEPTION}, NULL},
    {".?AVbad_cast@std@@", 2, {C_BAD_CAST, C_EXCEPTION}, "bad cast"},
    {".?AVbad_typeid@std@@", 2, {C_BAD_TYPEID, C_EXCEPTION}, "bad typeid"},
    {".?AV__non_rtti_object@std@@", 3, {C_NON_RTTI, C_BAD_TYPEID, C_EXCEPTION}, "bad read pointer - no RTTI data"},
    {".?AVbad_exception@std@@", 2, {C_BAD_EXCEPTION, C_EXCEPTION}, "bad exception"},
    {".?AVbad_alloc@std@@", 2, {C_BAD_ALLOC, C_EXCEPTION}, "bad allocation"},
    {".?AVbad_array_new_length@std@@", 3, {C_BAD_ARRAY, C_BAD_ALLOC, C_EXCEPTION}, "bad array new length"},
    {".?AVtype_info@@", 1, {C_TYPE_INFO}, NULL},
};

/* Run-time type information, filled in by vcr_exc_init. */
static struct {
    vcr_typedesc *td;
    char td_storage[64];
    vcr_col col;
    vcr_chd chd;
    uint32_t base_array[3];
    vcr_bcd bcds[3];
    vcr_throwinfo ti;
    vcr_catchable_array *catchables;
    uint32_t catchable_storage[4];
    vcr_catchable ct[3];
} g_rt[C_COUNT];

/* Each class's vftable: the locator, then the scalar deleting destructor and what(). The exported symbols point past the
 * locator slot. type_info's has the destructor only. */
extern const void *vcr_vt_table[C_COUNT][3];

static uint32_t vcr_rva(const void *p) { return (uint32_t)((const char *)p - &__ImageBase); }

static void *vcr_scalar_deleting_dtor(void *self, unsigned flags)
{
    __std_exception_destroy((char *)self + sizeof(void *));
    if (flags & 1)
        HeapFree(GetProcessHeap(), 0, self);
    return self;
}

static const char *vcr_what(const vcr_exception_obj *self) { return self->what ? self->what : "Unknown exception"; }

static void *vcr_type_info_sdd(void *self, unsigned flags)
{
    if (flags & 1)
        HeapFree(GetProcessHeap(), 0, self);
    return self;
}

__asm__(".data\n\t"
        ".balign 16\n\t"
        ".globl vcr_vt_table\n"
        "vcr_vt_table:\n\t"
        ".rept 8\n\t"
        ".quad 0, 0, 0\n\t"
        ".endr\n\t"
        ".text\n\t");

/* The addresses the .def file exports as the vftables: &vcr_vt_table[i][1]. */
__asm__(".globl vcr_vftable_exception\n\t.set vcr_vftable_exception, vcr_vt_table + 8\n\t"
        ".globl vcr_vftable_bad_cast\n\t.set vcr_vftable_bad_cast, vcr_vt_table + 32\n\t"
        ".globl vcr_vftable_bad_typeid\n\t.set vcr_vftable_bad_typeid, vcr_vt_table + 56\n\t"
        ".globl vcr_vftable_non_rtti\n\t.set vcr_vftable_non_rtti, vcr_vt_table + 80\n\t"
        ".globl vcr_vftable_bad_exception\n\t.set vcr_vftable_bad_exception, vcr_vt_table + 104\n\t"
        ".globl vcr_vftable_bad_alloc\n\t.set vcr_vftable_bad_alloc, vcr_vt_table + 128\n\t"
        ".globl vcr_vftable_bad_array\n\t.set vcr_vftable_bad_array, vcr_vt_table + 152\n\t"
        ".globl vcr_vftable_type_info\n\t.set vcr_vftable_type_info, vcr_vt_table + 176\n\t");

static void *vcr_copy_ctor_for(int cls);

static void vcr_exc_build(int cls)
{
    const vcr_class *c = &g_classes[cls];
    int i;
    size_t n = strlen(c->decorated) + 1;
    g_rt[cls].td = (vcr_typedesc *)g_rt[cls].td_storage;
    g_rt[cls].td->pVFTable = &vcr_vt_table[C_TYPE_INFO][1];
    g_rt[cls].td->spare = NULL;
    memcpy(g_rt[cls].td->name, c->decorated, n);
    for (i = 0; i < c->nbases; ++i) {
        int b = c->bases[i];
        g_rt[cls].bcds[i].pTypeDescriptor = vcr_rva(g_rt[b].td);
        g_rt[cls].bcds[i].numContainedBases = (uint32_t)(c->nbases - 1 - i);
        g_rt[cls].bcds[i].where.mdisp = 0;
        g_rt[cls].bcds[i].where.pdisp = -1;
        g_rt[cls].bcds[i].where.vdisp = 0;
        g_rt[cls].bcds[i].attributes = 0;
        g_rt[cls].bcds[i].pClassDescriptor = vcr_rva(&g_rt[b].chd);
        g_rt[cls].base_array[i] = vcr_rva(&g_rt[cls].bcds[i]);
    }
    g_rt[cls].chd.signature = 0;
    g_rt[cls].chd.attributes = 0;
    g_rt[cls].chd.numBaseClasses = (uint32_t)c->nbases;
    g_rt[cls].chd.pBaseClassArray = vcr_rva(g_rt[cls].base_array);
    g_rt[cls].col.signature = 1;
    g_rt[cls].col.offset = 0;
    g_rt[cls].col.cdOffset = 0;
    g_rt[cls].col.pTypeDescriptor = vcr_rva(g_rt[cls].td);
    g_rt[cls].col.pClassDescriptor = vcr_rva(&g_rt[cls].chd);
    g_rt[cls].col.pSelf = vcr_rva(&g_rt[cls].col);
    vcr_vt_table[cls][0] = &g_rt[cls].col;
    vcr_vt_table[cls][1] = cls == C_TYPE_INFO ? (const void *)vcr_type_info_sdd : (const void *)vcr_scalar_deleting_dtor;
    vcr_vt_table[cls][2] = cls == C_TYPE_INFO ? NULL : (const void *)vcr_what;
}

static void vcr_throw_build(int cls)
{
    const vcr_class *c = &g_classes[cls];
    int i;
    g_rt[cls].catchables = (vcr_catchable_array *)g_rt[cls].catchable_storage;
    g_rt[cls].catchables->nCatchableTypes = c->nbases;
    for (i = 0; i < c->nbases; ++i) {
        g_rt[cls].ct[i].properties = 0;
        g_rt[cls].ct[i].pType = vcr_rva(g_rt[c->bases[i]].td);
        g_rt[cls].ct[i].thisDisplacement.mdisp = 0;
        g_rt[cls].ct[i].thisDisplacement.pdisp = -1;
        g_rt[cls].ct[i].thisDisplacement.vdisp = 0;
        g_rt[cls].ct[i].sizeOrOffset = (int32_t)sizeof(vcr_exception_obj);
        g_rt[cls].ct[i].copyFunction = vcr_rva(vcr_copy_ctor_for(c->bases[i]));
        g_rt[cls].catchables->types[i] = vcr_rva(&g_rt[cls].ct[i]);
    }
    g_rt[cls].ti.attributes = 0;
    g_rt[cls].ti.pmfnUnwind = vcr_rva(vcr_copy_ctor_for(-1)); /* the destructor */
    g_rt[cls].ti.pForwardCompat = 0;
    g_rt[cls].ti.pCatchableTypeArray = vcr_rva(g_rt[cls].catchables);
}

void vcr_exc_init(void)
{
    int i;
    for (i = 0; i < C_COUNT; ++i)
        vcr_exc_build(i);
    for (i = 0; i < C_TYPE_INFO; ++i)
        vcr_throw_build(i);
}

static void vcr_init_object(vcr_exception_obj *o, int cls, const char *what, char do_free)
{
    o->vptr = &vcr_vt_table[cls][1];
    o->what = what;
    o->do_free = do_free;
}

/* ---- Exported members: one set of entry points per class ---------------------------------------------------------------------- */

#define VCR_CLASS_FUNCS(NAME, CLS)                                                                                              \
    void *__cdecl vcr_##NAME##_ctor_default(vcr_exception_obj *self)                                                            \
    {                                                                                                                           \
        vcr_init_object(self, CLS, g_classes[CLS].default_what, 0);                                                             \
        return self;                                                                                                            \
    }                                                                                                                           \
    void *__cdecl vcr_##NAME##_ctor_cstr(vcr_exception_obj *self, const char *what)                                             \
    {                                                                                                                           \
        vcr_init_object(self, CLS, what, 0);                                                                                    \
        return self;                                                                                                            \
    }                                                                                                                           \
    void *__cdecl vcr_##NAME##_ctor_copy(vcr_exception_obj *self, const vcr_exception_obj *other)                               \
    {                                                                                                                           \
        vcr_init_object(self, CLS, NULL, 0);                                                                                    \
        __std_exception_copy(&other->what, &self->what);                                                                        \
        return self;                                                                                                            \
    }                                                                                                                           \
    void __cdecl vcr_##NAME##_dtor(vcr_exception_obj *self)                                                                     \
    {                                                                                                                           \
        __std_exception_destroy(&self->what);                                                                                   \
    }                                                                                                                           \
    void *__cdecl vcr_##NAME##_assign(vcr_exception_obj *self, const vcr_exception_obj *other)                                  \
    {                                                                                                                           \
        if (self != other) {                                                                                                    \
            __std_exception_destroy(&self->what);                                                                               \
            __std_exception_copy(&other->what, &self->what);                                                                    \
        }                                                                                                                       \
        return self;                                                                                                            \
    }

VCR_CLASS_FUNCS(exception, C_EXCEPTION)
VCR_CLASS_FUNCS(bad_cast, C_BAD_CAST)
VCR_CLASS_FUNCS(bad_typeid, C_BAD_TYPEID)
VCR_CLASS_FUNCS(non_rtti, C_NON_RTTI)
VCR_CLASS_FUNCS(bad_exception, C_BAD_EXCEPTION)
VCR_CLASS_FUNCS(bad_alloc, C_BAD_ALLOC)
VCR_CLASS_FUNCS(bad_array, C_BAD_ARRAY)

/* exception(const char *const &): copies the text. exception(const char *const &, int): keeps the pointer. */
void *__cdecl vcr_exception_ctor_cstr_ref(vcr_exception_obj *self, const char *const *what)
{
    struct {
        const char *what;
        char do_free;
    } src;
    vcr_init_object(self, C_EXCEPTION, NULL, 0);
    src.what = *what;
    src.do_free = 1;
    __std_exception_copy(&src, &self->what);
    return self;
}

void *__cdecl vcr_exception_ctor_cstr_ref_nocopy(vcr_exception_obj *self, const char *const *what, int unused)
{
    (void)unused;
    vcr_init_object(self, C_EXCEPTION, *what, 0);
    return self;
}

const char *__cdecl vcr_exception_what(const vcr_exception_obj *self) { return vcr_what(self); }

/* The constructor, destructor and copy constructor the thrown-object records name. */
static void *vcr_copy_ctor_for(int cls)
{
    switch (cls) {
    case C_EXCEPTION: return (void *)vcr_exception_ctor_copy;
    case C_BAD_CAST: return (void *)vcr_bad_cast_ctor_copy;
    case C_BAD_TYPEID: return (void *)vcr_bad_typeid_ctor_copy;
    case C_NON_RTTI: return (void *)vcr_non_rtti_ctor_copy;
    case C_BAD_EXCEPTION: return (void *)vcr_bad_exception_ctor_copy;
    case C_BAD_ALLOC: return (void *)vcr_bad_alloc_ctor_copy;
    case C_BAD_ARRAY: return (void *)vcr_bad_array_ctor_copy;
    default: return (void *)vcr_exception_dtor; /* -1: the destructor */
    }
}

/* ---- Exceptions the runtime throws -------------------------------------------------------------------------------------------- */

static void vcr_throw_class(int cls, const char *what)
{
    vcr_exception_obj obj;
    vcr_init_object(&obj, cls, what, 0);
    _CxxThrowException(&obj, &g_rt[cls].ti);
}

void __cdecl vcr_throw_bad_typeid(void) { vcr_throw_class(C_BAD_TYPEID, "Attempted a typeid of nullptr pointer!"); }
void __cdecl vcr_throw_bad_cast(void) { vcr_throw_class(C_BAD_CAST, "Bad dynamic_cast!"); }
void __cdecl vcr_throw_non_rtti(void) { vcr_throw_class(C_NON_RTTI, "Bad read pointer - no RTTI data!"); }

/* ---- type_info ----------------------------------------------------------------------------------------------------------------- */

typedef struct {
    const void *vptr;
    const char *undecorated;
    char name[1];
} vcr_type_info_obj;

void __cdecl vcr_type_info_dtor(vcr_type_info_obj *self)
{
    if (self->undecorated) {
        HeapFree(GetProcessHeap(), 0, (void *)self->undecorated);
        self->undecorated = NULL;
    }
}

BOOL __cdecl vcr_type_info_eq(const vcr_type_info_obj *a, const vcr_type_info_obj *b)
{
    return a == b || vcr_name_eq(a->name + 1, b->name + 1);
}

BOOL __cdecl vcr_type_info_ne(const vcr_type_info_obj *a, const vcr_type_info_obj *b) { return !vcr_type_info_eq(a, b); }

BOOL __cdecl vcr_type_info_before(const vcr_type_info_obj *a, const vcr_type_info_obj *b)
{
    const char *x = a->name + 1, *y = b->name + 1;
    while (*x && *x == *y) {
        ++x;
        ++y;
    }
    return (unsigned char)*x < (unsigned char)*y;
}

const char *__cdecl vcr_type_info_raw_name(const vcr_type_info_obj *self) { return self->name; }

const char *__cdecl __std_type_info_name(vcr_typedesc *d, void *root);

const char *__cdecl vcr_type_info_name(vcr_type_info_obj *self, void *root) { return __std_type_info_name((vcr_typedesc *)self, root); }

#endif /* __MINGW32__ */
