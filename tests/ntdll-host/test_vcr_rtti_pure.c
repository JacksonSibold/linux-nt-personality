/*
 * test_vcr_rtti_pure.c - tests for src/ntdll/vcr_rtti_pure.h on hand-built x86-64 Microsoft RTTI: a diamond-free multiple
 * inheritance case and a virtual-base diamond, covering typeid, cast to void, dynamic_cast in every direction (including the
 * ambiguous one) and the matching of thrown types against handlers.
 */
#include "minitest.h"
#include "vcr_rtti_pure.h"

#include <string.h>

#define IMG_SIZE 0x10000
static char g_img[IMG_SIZE] __attribute__((aligned(16)));
static uint32_t g_top;

static uint32_t take(size_t n)
{
    uint32_t r = g_top;
    g_top += (uint32_t)((n + 7) & ~(size_t)7);
    return r;
}

static void reset(void)
{
    memset(g_img, 0, sizeof g_img);
    g_top = 0x100;
}

static uint32_t td(const char *name)
{
    uint32_t r = take(sizeof(vcr_typedesc) + strlen(name));
    memcpy(((vcr_typedesc *)(g_img + r))->name, name, strlen(name) + 1);
    return r;
}

static vcr_pmd pmd(int m, int p, int v)
{
    vcr_pmd d;
    d.mdisp = m;
    d.pdisp = p;
    d.vdisp = v;
    return d;
}

static uint32_t bcd(uint32_t tdr, uint32_t contained, vcr_pmd where, uint32_t attrs)
{
    uint32_t r = take(sizeof(vcr_bcd));
    vcr_bcd *b = (vcr_bcd *)(g_img + r);
    b->pTypeDescriptor = tdr;
    b->numContainedBases = contained;
    b->where = where;
    b->attributes = attrs;
    return r;
}

static uint32_t chd(const uint32_t *bcds, uint32_t n)
{
    uint32_t arr = take(4 * n), r = take(sizeof(vcr_chd));
    vcr_chd *c = (vcr_chd *)(g_img + r);
    memcpy(g_img + arr, bcds, 4 * n);
    c->numBaseClasses = n;
    c->pBaseClassArray = arr;
    return r;
}

/* A vftable: slot -1 holds the locator. Returns a pointer to slot 0. */
static const void *vtable(uint32_t tdr, uint32_t chdr, uint32_t offset)
{
    uint32_t cr = take(sizeof(vcr_col)), vt = take(16);
    vcr_col *c = (vcr_col *)(g_img + cr);
    c->signature = 1;
    c->offset = offset;
    c->pTypeDescriptor = tdr;
    c->pClassDescriptor = chdr;
    c->pSelf = cr;
    *(const void **)(g_img + vt) = c;
    return g_img + vt + 8;
}

#define TD(r) ((const vcr_typedesc *)(g_img + (r)))

typedef struct {
    const void *vptr;
    int value;
} part;

typedef struct {
    part b;
    part c;
} dobj;

static uint32_t tA, tB, tC, tD, tX;

static void d_objects(dobj *o)
{
    uint32_t arr[5];
    uint32_t a1, a2, b, c, d, hier;
    reset();
    tA = td(".?AVA@@");
    tB = td(".?AVB@@");
    tC = td(".?AVC@@");
    tD = td(".?AVD@@");
    tX = td(".?AVX@@");
    a1 = bcd(tA, 0, pmd(0, -1, 0), 0);
    a2 = bcd(tA, 0, pmd(16, -1, 0), 0);
    b = bcd(tB, 1, pmd(0, -1, 0), 0);
    c = bcd(tC, 1, pmd(16, -1, 0), 0);
    d = bcd(tD, 4, pmd(0, -1, 0), 0);
    arr[0] = d; arr[1] = b; arr[2] = a1; arr[3] = c; arr[4] = a2;
    hier = chd(arr, 5);
    o->b.vptr = vtable(tD, hier, 0);
    o->c.vptr = vtable(tD, hier, 16);
    o->b.value = 1;
    o->c.value = 2;
}

static void typeid_and_cast_to_void(void)
{
    dobj o;
    d_objects(&o);
    MT_EXPECT(vcr_td_eq(vcr_typeid(&o.b), TD(tD)));
    MT_EXPECT(vcr_td_eq(vcr_typeid(&o.c), TD(tD))); /* from the second base's pointer too */
    MT_EXPECT(vcr_typeid(NULL) == NULL);
    MT_EXPECT(vcr_cast_to_void(&o.c) == (void *)&o);
    MT_EXPECT(vcr_cast_to_void(&o.b) == (void *)&o);
    MT_EXPECT(vcr_cast_to_void(NULL) == NULL);
}

static void downcast_cross_cast_and_upcast(void)
{
    dobj o;
    int amb = 0;
    d_objects(&o);
    /* B* to D*: a downcast */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tB), TD(tD), &amb) == (void *)&o);
    /* C* to D* from the second base */
    MT_EXPECT(vcr_dynamic_cast(&o.c, TD(tC), TD(tD), &amb) == (void *)&o);
    /* B* to C*: a cross cast to the other base */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tB), TD(tC), &amb) == (void *)&o.c);
    MT_EXPECT(vcr_dynamic_cast(&o.c, TD(tC), TD(tB), &amb) == (void *)&o.b);
    /* B* to A*: the A inside B, not the one inside C */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tB), TD(tA), &amb) == (void *)&o.b && !amb);
    MT_EXPECT(vcr_dynamic_cast(&o.c, TD(tC), TD(tA), &amb) == (void *)&o.c && !amb);
    /* identity */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tB), TD(tB), &amb) == (void *)&o.b);
    /* null in, null out */
    MT_EXPECT(vcr_dynamic_cast(NULL, TD(tB), TD(tD), &amb) == NULL);
}

static void ambiguous_and_missing(void)
{
    dobj o;
    int amb = 0;
    d_objects(&o);
    /* D* to A*: two A subobjects */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tD), TD(tA), &amb) == NULL);
    MT_EXPECT_EQ(amb, 1);
    amb = 0;
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tB), TD(tX), &amb) == NULL); /* not in the hierarchy */
    MT_EXPECT_EQ(amb, 0);
}

static void private_bases_are_not_reachable(void)
{
    dobj o;
    int amb = 0;
    uint32_t arr[2], hier, b, a, tP, tQ;
    d_objects(&o);
    reset();
    tP = td(".?AVP@@");
    tQ = td(".?AVQ@@");
    a = bcd(tQ, 0, pmd(0, -1, 0), VCR_BCD_PRIVORPROTBASE | VCR_BCD_PRIVORPROTINCOMPOBJ);
    b = bcd(tP, 1, pmd(0, -1, 0), 0);
    arr[0] = b;
    arr[1] = a;
    hier = chd(arr, 2);
    o.b.vptr = vtable(tP, hier, 0);
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tP), TD(tQ), &amb) == NULL); /* P privately derives from Q */
    MT_EXPECT(vcr_dynamic_cast(&o.b, TD(tP), TD(tP), &amb) == (void *)&o.b);
}

/* Z : X, Y with X and Y each deriving virtually from V. */
typedef struct {
    const void *x_vptr;
    const int32_t *x_vbptr;
    const void *y_vptr;
    const int32_t *y_vbptr;
    const void *v_vptr;
    long v_value;
} zobj;

static void virtual_diamond(void)
{
    static const int32_t vbt_x[2] = {0, 24}, vbt_y[2] = {0, 8};
    zobj z;
    int amb = 0;
    uint32_t tZ, tXx, tY, tV, v1, v2, x, y, zz, arr[5], hier;
    reset();
    tZ = td(".?AVZ@@");
    tXx = td(".?AVX@@");
    tY = td(".?AVY@@");
    tV = td(".?AVV@@");
    v1 = bcd(tV, 0, pmd(0, 8, 4), VCR_BCD_VBOFCONTOBJ);   /* V as reached through X's vbptr at offset 8 */
    v2 = bcd(tV, 0, pmd(0, 24, 4), VCR_BCD_VBOFCONTOBJ);  /* ... and through Y's vbptr at offset 24 */
    x = bcd(tXx, 1, pmd(0, -1, 0), 0);
    y = bcd(tY, 1, pmd(16, -1, 0), 0);
    zz = bcd(tZ, 4, pmd(0, -1, 0), 0);
    arr[0] = zz; arr[1] = x; arr[2] = v1; arr[3] = y; arr[4] = v2;
    hier = chd(arr, 5);
    z.x_vptr = vtable(tZ, hier, 0);
    z.y_vptr = vtable(tZ, hier, 16);
    z.v_vptr = vtable(tZ, hier, 32);
    z.x_vbptr = vbt_x;
    z.y_vbptr = vbt_y;
    z.v_value = 7;
    /* V reached from either path is the same subobject: not ambiguous */
    MT_EXPECT(vcr_dynamic_cast(&z, TD(tZ), TD(tV), &amb) == (void *)&z.v_vptr && !amb);
    MT_EXPECT(vcr_dynamic_cast(&z, TD(tXx), TD(tV), &amb) == (void *)&z.v_vptr && !amb);
    MT_EXPECT(vcr_dynamic_cast(&z.y_vptr, TD(tY), TD(tV), &amb) == (void *)&z.v_vptr && !amb);
    /* back down from V to Z, and across to Y */
    MT_EXPECT(vcr_dynamic_cast(&z.v_vptr, TD(tV), TD(tZ), &amb) == (void *)&z);
    MT_EXPECT(vcr_dynamic_cast(&z, TD(tXx), TD(tY), &amb) == (void *)&z.y_vptr);
    MT_EXPECT(vcr_cast_to_void(&z.v_vptr) == (void *)&z);
    MT_EXPECT(vcr_adjust_pointer(&z, &(vcr_pmd){0, 8, 4}) == (void *)&z.v_vptr);
}

/* Thrown D: catchable as D, B (offset 0), C (offset 16, by reference only), A. */
static void matching_thrown_types(void)
{
    dobj o;
    vcr_throwinfo *ti;
    uint32_t cD, cB, cC, cA, arrr, tir;
    const vcr_catchable *ct;
    int32_t *cnt;
    d_objects(&o);
    cD = take(sizeof(vcr_catchable));
    cB = take(sizeof(vcr_catchable));
    cC = take(sizeof(vcr_catchable));
    cA = take(sizeof(vcr_catchable));
#define SETCT(r, t, props, d) do { vcr_catchable *k = (vcr_catchable *)(g_img + (r)); k->pType = (t); k->properties = (props); k->thisDisplacement = (d); } while (0)
    SETCT(cD, tD, 0, pmd(0, -1, 0));
    SETCT(cB, tB, 0, pmd(0, -1, 0));
    SETCT(cC, tC, VCR_CT_BYREF_ONLY, pmd(16, -1, 0));
    SETCT(cA, tA, 0, pmd(0, -1, 0));
    arrr = take(4 + 4 * 4);
    cnt = (int32_t *)(g_img + arrr);
    *cnt = 4;
    ((uint32_t *)(g_img + arrr + 4))[0] = cD;
    ((uint32_t *)(g_img + arrr + 4))[1] = cB;
    ((uint32_t *)(g_img + arrr + 4))[2] = cC;
    ((uint32_t *)(g_img + arrr + 4))[3] = cA;
    tir = take(sizeof(vcr_throwinfo));
    ti = (vcr_throwinfo *)(g_img + tir);
    ti->pCatchableTypeArray = arrr;

    MT_EXPECT(vcr_find_catchable(g_img, TD(tB), 0, ti) == (const vcr_catchable *)(g_img + cB));
    MT_EXPECT(vcr_find_catchable(g_img, NULL, 0, ti) == (const vcr_catchable *)(g_img + cD)); /* catch (...) takes the first */
    MT_EXPECT(vcr_find_catchable(g_img, TD(tC), 0, ti) == NULL);                              /* C is by reference only */
    ct = vcr_find_catchable(g_img, TD(tC), VCR_HT_REFERENCE, ti);
    MT_EXPECT(ct == (const vcr_catchable *)(g_img + cC));
    MT_EXPECT(vcr_adjust_pointer(&o, &ct->thisDisplacement) == (void *)&o.c);
    MT_EXPECT(vcr_find_catchable(g_img, TD(tX), 0, ti) == NULL);
    /* a const object is not caught by a non-const handler */
    ti->attributes = VCR_TI_CONST;
    MT_EXPECT(vcr_find_catchable(g_img, TD(tB), 0, ti) == NULL);
    MT_EXPECT(vcr_find_catchable(g_img, TD(tB), VCR_HT_CONST, ti) == (const vcr_catchable *)(g_img + cB));
    ti->attributes = VCR_TI_VOLATILE;
    MT_EXPECT(vcr_find_catchable(g_img, TD(tB), VCR_HT_CONST, ti) == NULL);
    MT_EXPECT(vcr_find_catchable(g_img, TD(tB), VCR_HT_VOLATILE, ti) == (const vcr_catchable *)(g_img + cB));
}

static const mt_case_t cases[] = {
    MT_CASE(typeid_and_cast_to_void), MT_CASE(downcast_cross_cast_and_upcast), MT_CASE(ambiguous_and_missing),
    MT_CASE(private_bases_are_not_reachable), MT_CASE(virtual_diamond), MT_CASE(matching_thrown_types),
};

int main(void)
{
    return MT_RUN("vcr_rtti_pure", cases);
}
