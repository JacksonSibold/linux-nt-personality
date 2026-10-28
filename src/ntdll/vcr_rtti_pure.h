/*
 * vcr_rtti_pure.h - run-time type information as the Microsoft C++ ABI lays it out on x86-64: type descriptors, complete
 * object locators, class hierarchy descriptors, and the dynamic_cast, typeid and cast-to-void operations on them.
 *
 * The locator, hierarchy and base class descriptors refer to each other by 32-bit offsets from the image base; the base is
 * found from the locator itself (its pSelf field holds its own offset). Type descriptors compare by their decorated name,
 * so two modules that each carry a descriptor for the same type agree. No call in this file runs program code.
 *
 * dynamic_cast follows the usual rules by search over the flattened base class array: an unambiguous accessible match
 * wins; ambiguity or no match gives null. Host tests: tests/ntdll-host/test_vcr_rtti_pure.c.
 */
#ifndef VCR_RTTI_PURE_H
#define VCR_RTTI_PURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t mdisp, pdisp, vdisp;
} vcr_pmd;

typedef struct {
    uint32_t pTypeDescriptor;
    uint32_t numContainedBases; /* the entries after this one in the array that belong to its own hierarchy */
    vcr_pmd where;
    uint32_t attributes;
    uint32_t pClassDescriptor;
} vcr_bcd;

typedef struct {
    uint32_t signature;
    uint32_t attributes;
    uint32_t numBaseClasses;
    uint32_t pBaseClassArray; /* offset of an array of numBaseClasses offsets of vcr_bcd */
} vcr_chd;

typedef struct {
    uint32_t signature; /* 1 on x86-64 */
    uint32_t offset;    /* of the vftable's subobject in the complete object */
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;
    uint32_t pClassDescriptor;
    uint32_t pSelf;
} vcr_col;

typedef struct {
    const void *pVFTable;
    void *spare;
    char name[1]; /* decorated, starting with '.' */
} vcr_typedesc;

#define VCR_BCD_NOTVISIBLE 0x01u
#define VCR_BCD_AMBIGUOUS 0x02u
#define VCR_BCD_PRIVORPROTBASE 0x04u
#define VCR_BCD_PRIVORPROTINCOMPOBJ 0x08u
#define VCR_BCD_VBOFCONTOBJ 0x10u

static inline int vcr_name_eq(const char *a, const char *b)
{
    if (a == b)
        return 1;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static inline int vcr_td_eq(const vcr_typedesc *a, const vcr_typedesc *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return vcr_name_eq(a->name, b->name);
}

/* The offset of the base within an object, resolving a virtual base through the vbtable. */
static inline ptrdiff_t vcr_pmd_offset(const char *obj, vcr_pmd pmd)
{
    ptrdiff_t off = pmd.mdisp;
    if (pmd.pdisp >= 0) {
        const char *vbptr = *(const char *const *)(obj + pmd.pdisp);
        off += pmd.pdisp;
        off += *(const int32_t *)(vbptr + pmd.vdisp);
    }
    return off;
}

static inline const vcr_col *vcr_col_of(const void *obj) { return (const vcr_col *)((const void *const *)(*(const void *const *)obj))[-1]; }

static inline const char *vcr_image_base(const vcr_col *col) { return (const char *)col - col->pSelf; }

/* The start of the complete object that contains the subobject at inptr. */
static inline const char *vcr_complete_object(const char *inptr, const vcr_col *col)
{
    const char *whole = inptr - col->offset;
    if (col->cdOffset)
        whole -= *(const int32_t *)(inptr - col->cdOffset);
    return whole;
}

/* __RTtypeid: the descriptor of the dynamic type, or NULL for a null pointer (the caller throws bad_typeid). */
static inline const vcr_typedesc *vcr_typeid(const void *inptr)
{
    const vcr_col *col;
    if (!inptr)
        return NULL;
    col = vcr_col_of(inptr);
    return (const vcr_typedesc *)(vcr_image_base(col) + col->pTypeDescriptor);
}

/* __RTCastToVoid */
static inline void *vcr_cast_to_void(void *inptr)
{
    if (!inptr)
        return NULL;
    return (void *)vcr_complete_object((const char *)inptr, vcr_col_of(inptr));
}

#define VCR_MAX_CANDIDATES 64

/* __RTDynamicCast. *ambiguous is set when more than one subobject qualifies. Returns NULL when there is no match. */
static inline void *vcr_dynamic_cast(void *inptr, const vcr_typedesc *src, const vcr_typedesc *target, int *ambiguous)
{
    const vcr_col *col;
    const char *base, *whole;
    const vcr_chd *chd;
    const uint32_t *arr;
    ptrdiff_t delta;
    uint32_t i, n;
    ptrdiff_t found[VCR_MAX_CANDIDATES];
    uint32_t nfound = 0, k;
    int pass;

    if (ambiguous)
        *ambiguous = 0;
    if (!inptr)
        return NULL;
    col = vcr_col_of(inptr);
    base = vcr_image_base(col);
    whole = vcr_complete_object((const char *)inptr, col);
    delta = (const char *)inptr - whole;
    chd = (const vcr_chd *)(base + col->pClassDescriptor);
    arr = (const uint32_t *)(base + chd->pBaseClassArray);
    n = chd->numBaseClasses;

    /* pass 0: the target lies inside the source subobject (an upcast); pass 1: the source lies inside the target (a downcast);
     * pass 2: any accessible target subobject (a cross cast, or the complete object itself). */
    for (pass = 0; pass < 3; ++pass) {
        nfound = 0;
        for (i = 0; i < n; ++i) {
            const vcr_bcd *tb = (const vcr_bcd *)(base + arr[i]);
            ptrdiff_t toff;
            int ok = 0;
            uint32_t j;
            if (!vcr_td_eq((const vcr_typedesc *)(base + tb->pTypeDescriptor), target))
                continue;
            if (tb->attributes & (VCR_BCD_NOTVISIBLE | VCR_BCD_PRIVORPROTINCOMPOBJ))
                continue;
            toff = vcr_pmd_offset(whole, tb->where);
            if (pass == 2) {
                ok = 1;
            } else if (pass == 1) {
                /* the source at delta must be this target or one of its bases */
                for (j = i; j <= i + tb->numContainedBases && j < n; ++j) {
                    const vcr_bcd *sb = (const vcr_bcd *)(base + arr[j]);
                    if (vcr_td_eq((const vcr_typedesc *)(base + sb->pTypeDescriptor), src) && vcr_pmd_offset(whole, sb->where) == delta &&
                        (j == i || !(sb->attributes & VCR_BCD_PRIVORPROTBASE))) {
                        ok = 1;
                        break;
                    }
                }
            } else {
                /* an entry for the source at delta whose hierarchy contains this target */
                for (j = 0; j < n && !ok; ++j) {
                    const vcr_bcd *sb = (const vcr_bcd *)(base + arr[j]);
                    uint32_t m;
                    if (!vcr_td_eq((const vcr_typedesc *)(base + sb->pTypeDescriptor), src) || vcr_pmd_offset(whole, sb->where) != delta)
                        continue;
                    for (m = j + 1; m <= j + sb->numContainedBases && m < n; ++m)
                        if (m == i) {
                            ok = 1;
                            break;
                        }
                }
            }
            if (!ok)
                continue;
            for (k = 0; k < nfound; ++k)
                if (found[k] == toff)
                    break;
            if (k == nfound && nfound < VCR_MAX_CANDIDATES)
                found[nfound++] = toff;
        }
        if (nfound == 1)
            return (void *)(whole + found[0]);
        if (nfound > 1) {
            if (ambiguous)
                *ambiguous = 1;
            return NULL;
        }
    }
    return NULL;
}

/* ---- Thrown object matching (used by the C++ frame handler) ----------------------------------------------------------------- */

typedef struct {
    uint32_t attributes; /* 1 const, 2 volatile */
    uint32_t pmfnUnwind;
    uint32_t pForwardCompat;
    uint32_t pCatchableTypeArray;
} vcr_throwinfo;

typedef struct {
    int32_t nCatchableTypes;
    uint32_t types[1];
} vcr_catchable_array;

typedef struct {
    uint32_t properties; /* 1 simple type, 2 by reference only, 4 has virtual base */
    uint32_t pType;
    vcr_pmd thisDisplacement;
    int32_t sizeOrOffset;
    uint32_t copyFunction;
} vcr_catchable;

#define VCR_TI_CONST 1u
#define VCR_TI_VOLATILE 2u
#define VCR_CT_SIMPLE 1u
#define VCR_CT_BYREF_ONLY 2u
#define VCR_CT_VIRTUAL_BASE 4u

#define VCR_HT_CONST 0x01u
#define VCR_HT_VOLATILE 0x02u
#define VCR_HT_REFERENCE 0x08u

/* Whether a handler for `type` (NULL for catch (...)) with `adjectives` can take the thrown object that ct describes. */
static inline int vcr_type_match(const char *base, const vcr_typedesc *handler_type, uint32_t adjectives, const vcr_catchable *ct,
                                 const vcr_throwinfo *ti)
{
    if (!handler_type)
        return 1;
    if (!vcr_td_eq(handler_type, (const vcr_typedesc *)(base + ct->pType)))
        return 0;
    if ((ti->attributes & VCR_TI_CONST) && !(adjectives & VCR_HT_CONST))
        return 0;
    if ((ti->attributes & VCR_TI_VOLATILE) && !(adjectives & VCR_HT_VOLATILE))
        return 0;
    if ((ct->properties & VCR_CT_BYREF_ONLY) && !(adjectives & VCR_HT_REFERENCE))
        return 0;
    return 1;
}

/* The first catchable type of the thrown object that the handler takes, or NULL. */
static inline const vcr_catchable *vcr_find_catchable(const char *base, const vcr_typedesc *handler_type, uint32_t adjectives,
                                                      const vcr_throwinfo *ti)
{
    const vcr_catchable_array *arr = (const vcr_catchable_array *)(base + ti->pCatchableTypeArray);
    int32_t i;
    for (i = 0; i < arr->nCatchableTypes; ++i) {
        const vcr_catchable *ct = (const vcr_catchable *)(base + arr->types[i]);
        if (vcr_type_match(base, handler_type, adjectives, ct, ti))
            return ct;
    }
    return NULL;
}

/* __AdjustPointer: the address of the base class subobject a catchable type names, inside the thrown object. */
static inline void *vcr_adjust_pointer(void *obj, const vcr_pmd *d) { return (char *)obj + vcr_pmd_offset((const char *)obj, *d); }

#endif
