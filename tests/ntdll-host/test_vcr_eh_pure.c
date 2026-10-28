/*
 * test_vcr_eh_pure.c - tests for src/ntdll/vcr_eh_pure.h on a hand-built function table: states from addresses, the unwind
 * chain, nested try blocks, catch selection by type, and the copy of the thrown object into the catch parameter.
 *
 * States of the function modelled here:
 *   0 object x      1 outer try entry   2 object y   3 inner try entry   4 object z (in the inner try)
 *   5 inside the inner catch   6 inside the outer catch
 *   inner try {3..4} catch level 5: catch (B&)          outer try {1..5} catch level 6: catch (X), catch (...)
 */
#include "minitest.h"
#include "vcr_eh_pure.h"

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

static uint32_t tB, tC, tX, tD;
static uint32_t g_fi;
static uint32_t g_catch_inner, g_catch_outer;
static uint32_t g_ti;
static uint32_t g_ct_d, g_ct_b, g_ct_c;

#define FI ((const vcr_funcinfo *)(g_img + g_fi))

static void build(void)
{
    uint32_t unw, tries, ipm, c_in, c_out;
    vcr_unwind_entry *u;
    vcr_tryblock *t;
    vcr_catchblock *c;
    vcr_ipmap *m;
    vcr_funcinfo *fi;
    uint32_t arrr, cnt_off;
    int32_t *cnt;

    memset(g_img, 0, sizeof g_img);
    g_top = 0x100;
    tB = td(".?AVB@@");
    tC = td(".?AVC@@");
    tX = td(".?AVX@@");
    tD = td(".?AVD@@");

    unw = take(sizeof *u * 7);
    u = (vcr_unwind_entry *)(g_img + unw);
    u[0].prev = -1; u[0].handler = 0xA0;
    u[1].prev = 0;  u[1].handler = 0;
    u[2].prev = 1;  u[2].handler = 0xA2;
    u[3].prev = 2;  u[3].handler = 0;
    u[4].prev = 3;  u[4].handler = 0xA4;
    u[5].prev = 3;  u[5].handler = 0xA5;
    u[6].prev = 1;  u[6].handler = 0;

    c_in = take(sizeof *c);
    c = (vcr_catchblock *)(g_img + c_in);
    c->flags = VCR_HT_REFERENCE;
    c->type_info = tB;
    c->offset = 0x40;
    c->handler = 0x5100;
    c->frame = 0x48;
    g_catch_inner = c_in;

    c_out = take(sizeof *c * 2);
    c = (vcr_catchblock *)(g_img + c_out);
    c[0].flags = 0;
    c[0].type_info = tX;
    c[0].offset = 0x50;
    c[0].handler = 0x5200;
    c[0].frame = 0x58;
    c[1].flags = 0;
    c[1].type_info = 0; /* catch (...) */
    c[1].offset = 0;
    c[1].handler = 0x5300;
    c[1].frame = 0x60;
    g_catch_outer = c_out;

    tries = take(sizeof *t * 2);
    t = (vcr_tryblock *)(g_img + tries);
    t[0].start_level = 3; t[0].end_level = 4; t[0].catch_level = 5; t[0].catchblock_count = 1; t[0].catchblock = c_in;
    t[1].start_level = 1; t[1].end_level = 5; t[1].catch_level = 6; t[1].catchblock_count = 2; t[1].catchblock = c_out;

    ipm = take(sizeof *m * 5);
    m = (vcr_ipmap *)(g_img + ipm);
    m[0].ip = 0x1010; m[0].state = 0;
    m[1].ip = 0x1020; m[1].state = 1;
    m[2].ip = 0x1030; m[2].state = 3;
    m[3].ip = 0x1040; m[3].state = 4;
    m[4].ip = 0x1050; m[4].state = 1;

    g_fi = take(sizeof *fi);
    fi = (vcr_funcinfo *)(g_img + g_fi);
    fi->magic_bbt = VCR_EH_MAGIC_VC8;
    fi->unwind_count = 7;
    fi->unwind_table = unw;
    fi->tryblock_count = 2;
    fi->tryblock = tries;
    fi->ipmap_count = 5;
    fi->ipmap = ipm;
    fi->unwind_help = -0x20;

    /* a thrown D, which is a B (at offset 0) and a C (at offset 16, by reference only) */
    g_ct_d = take(sizeof(vcr_catchable));
    g_ct_b = take(sizeof(vcr_catchable));
    g_ct_c = take(sizeof(vcr_catchable));
    {
        vcr_catchable *k = (vcr_catchable *)(g_img + g_ct_d);
        k->pType = tD; k->properties = 0; k->thisDisplacement = pmd(0, -1, 0); k->sizeOrOffset = 32;
        k = (vcr_catchable *)(g_img + g_ct_b);
        k->pType = tB; k->properties = 0; k->thisDisplacement = pmd(0, -1, 0); k->sizeOrOffset = 16; k->copyFunction = 0x7001;
        k = (vcr_catchable *)(g_img + g_ct_c);
        k->pType = tC; k->properties = VCR_CT_BYREF_ONLY; k->thisDisplacement = pmd(16, -1, 0); k->sizeOrOffset = 16;
    }
    arrr = take(4 + 4 * 3);
    cnt = (int32_t *)(g_img + arrr);
    *cnt = 3;
    cnt_off = arrr + 4;
    ((uint32_t *)(g_img + cnt_off))[0] = g_ct_d;
    ((uint32_t *)(g_img + cnt_off))[1] = g_ct_b;
    ((uint32_t *)(g_img + cnt_off))[2] = g_ct_c;
    g_ti = take(sizeof(vcr_throwinfo));
    ((vcr_throwinfo *)(g_img + g_ti))->pCatchableTypeArray = arrr;
}

#define TI ((const vcr_throwinfo *)(g_img + g_ti))

static void states_from_addresses(void)
{
    build();
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x1000), -1); /* before the first entry */
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x1010), 0);
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x101F), 0);
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x1035), 3);
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x1040), 4);
    MT_EXPECT_EQ(vcr_eh_ip_state(g_img, FI, 0x9999), 1); /* after the last entry */
}

typedef struct {
    uint32_t seen[8];
    int n;
} log_t;

static void record(void *ctx, uint32_t action)
{
    log_t *l = (log_t *)ctx;
    l->seen[l->n++] = action;
}

static void unwinding_runs_actions_down_the_chain(void)
{
    log_t l;
    build();
    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_eh_local_unwind(g_img, FI, 4, -1, record, &l), -1);
    MT_EXPECT_EQ(l.n, 3); /* states 4, 2, 0 have actions; 3 and 1 have none */
    MT_EXPECT(l.seen[0] == 0xA4 && l.seen[1] == 0xA2 && l.seen[2] == 0xA0);
    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_eh_local_unwind(g_img, FI, 4, 3, record, &l), 3); /* stops at the try's own state */
    MT_EXPECT(l.n == 1 && l.seen[0] == 0xA4);
    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_eh_local_unwind(g_img, FI, 2, 2, record, &l), 2); /* nothing to do */
    MT_EXPECT_EQ(l.n, 0);
    MT_EXPECT_EQ(vcr_eh_local_unwind(g_img, FI, 9, -1, record, &l), -2); /* a state outside the table */
}

static void catch_region_and_funclet_detection(void)
{
    vcr_nested n;
    build();
    MT_EXPECT(vcr_eh_in_catch(g_img, FI, 4) == NULL); /* in the try body */
    MT_EXPECT(vcr_eh_in_catch(g_img, FI, 5) == vcr_eh_tryblock(g_img, FI, 0));
    MT_EXPECT(vcr_eh_in_catch(g_img, FI, 6) == vcr_eh_tryblock(g_img, FI, 1));
    n = vcr_eh_nested(g_img, FI, 5, 0x5100); /* code of the inner catch funclet */
    MT_EXPECT(n.found);
    MT_EXPECT_EQ(n.frame_offset, 0x48);
    MT_EXPECT_EQ(n.unwind_level, 4);
    n = vcr_eh_nested(g_img, FI, 6, 0x5300);
    MT_EXPECT(n.found);
    MT_EXPECT_EQ(n.frame_offset, 0x60);
    MT_EXPECT_EQ(n.unwind_level, 5);
    n = vcr_eh_nested(g_img, FI, 4, 0x5100); /* not in a catch region */
    MT_EXPECT(!n.found);
    n = vcr_eh_nested(g_img, FI, 5, 0x9999); /* not a funclet of this function */
    MT_EXPECT(!n.found);
}

static void selecting_a_catch_block(void)
{
    vcr_catch_match m;
    vcr_throwinfo plain;
    build();
    /* a thrown D at state 4 (inner try): the inner catch (B&) takes it */
    MT_EXPECT(vcr_eh_find_catch(g_img, g_img, FI, 4, NULL, TI, &m));
    MT_EXPECT(m.tryblock == vcr_eh_tryblock(g_img, FI, 0));
    MT_EXPECT(m.catchable == (const vcr_catchable *)(g_img + g_ct_b));
    /* at state 2 only the outer try is in range: catch (X) does not match a D, catch (...) does */
    MT_EXPECT(vcr_eh_find_catch(g_img, g_img, FI, 2, NULL, TI, &m));
    MT_EXPECT(m.tryblock == vcr_eh_tryblock(g_img, FI, 1));
    MT_EXPECT(m.catchblock == vcr_eh_catchblock(g_img, m.tryblock, 1));
    MT_EXPECT(m.catchable == (const vcr_catchable *)(g_img + g_ct_d)); /* catch (...) copies nothing, but names the first type */
    /* state 0 is outside every try */
    MT_EXPECT(!vcr_eh_find_catch(g_img, g_img, FI, 0, NULL, TI, &m));
    /* thrown while in the inner catch (state 5): only try blocks inside that catch qualify, and there are none */
    MT_EXPECT(!vcr_eh_find_catch(g_img, g_img, FI, 5, vcr_eh_tryblock(g_img, FI, 0), TI, &m));
    /* but with the state alone the outer try applies: this is what the frame's own turn sees */
    MT_EXPECT(vcr_eh_find_catch(g_img, g_img, FI, 5, NULL, TI, &m));
    MT_EXPECT(m.tryblock == vcr_eh_tryblock(g_img, FI, 1));
    /* const objects are not taken by catch (X) without const; a non-C++ exception goes to catch (...) only */
    plain = *TI;
    MT_EXPECT(vcr_eh_find_catch(g_img, g_img, FI, 4, NULL, NULL, &m));
    MT_EXPECT(m.tryblock == vcr_eh_tryblock(g_img, FI, 1) && m.catchable == NULL);
    (void)plain;
}

typedef struct {
    uint32_t ctor;
    void *dest, *src;
    int vbase;
    int calls;
} ctor_log;

static void copy_ctor(void *ctx, uint32_t rva, void *dest, void *src, int vbase)
{
    ctor_log *l = (ctor_log *)ctx;
    l->ctor = rva;
    l->dest = dest;
    l->src = src;
    l->vbase = vbase;
    ++l->calls;
}

static void copying_into_the_catch_parameter(void)
{
    unsigned char obj[32], slot[32];
    ctor_log l;
    vcr_catchable simple, nocopy;
    build();
    memset(&l, 0, sizeof l);
    memset(obj, 0xAB, sizeof obj);
    memset(slot, 0, sizeof slot);
    /* by reference: the slot receives the address of the base subobject */
    vcr_eh_copy_catch_object(g_img, obj, slot, VCR_HT_REFERENCE, (const vcr_catchable *)(g_img + g_ct_c), copy_ctor, &l);
    MT_EXPECT(*(void **)slot == (void *)(obj + 16));
    MT_EXPECT_EQ(l.calls, 0);
    /* by value with a copy constructor */
    vcr_eh_copy_catch_object(g_img, obj, slot, 0, (const vcr_catchable *)(g_img + g_ct_b), copy_ctor, &l);
    MT_EXPECT_EQ(l.calls, 1);
    MT_EXPECT(l.ctor == 0x7001 && l.dest == slot && l.src == obj && l.vbase == 0);
    /* a simple type is copied; a pointer is adjusted to its base */
    memset(&simple, 0, sizeof simple);
    simple.properties = VCR_CT_SIMPLE;
    simple.sizeOrOffset = 4;
    *(int *)obj = 0x12345678;
    memset(slot, 0, sizeof slot);
    vcr_eh_copy_catch_object(g_img, obj, slot, 0, &simple, copy_ctor, &l);
    MT_EXPECT_EQ(*(int *)slot, 0x12345678);
    MT_EXPECT_EQ(l.calls, 1);
    simple.sizeOrOffset = 8;
    simple.thisDisplacement = pmd(16, -1, 0);
    *(void **)obj = obj + 100;
    vcr_eh_copy_catch_object(g_img, obj, slot, 0, &simple, copy_ctor, &l);
    MT_EXPECT(*(void **)slot == (void *)(obj + 116));
    /* no copy constructor: bytes */
    memset(&nocopy, 0, sizeof nocopy);
    nocopy.sizeOrOffset = 8;
    nocopy.thisDisplacement = pmd(8, -1, 0);
    memset(slot, 0, sizeof slot);
    memcpy(obj + 8, "ABCDEFGH", 8);
    vcr_eh_copy_catch_object(g_img, obj, slot, 0, &nocopy, copy_ctor, &l);
    MT_EXPECT(memcmp(slot, "ABCDEFGH", 8) == 0);
    /* a virtual base is flagged for the constructor */
    nocopy.copyFunction = 0x7002;
    nocopy.properties = VCR_CT_VIRTUAL_BASE;
    vcr_eh_copy_catch_object(g_img, obj, slot, 0, &nocopy, copy_ctor, &l);
    MT_EXPECT(l.calls == 2 && l.vbase == 1 && l.ctor == 0x7002);
}

static const mt_case_t cases[] = {
    MT_CASE(states_from_addresses), MT_CASE(unwinding_runs_actions_down_the_chain), MT_CASE(catch_region_and_funclet_detection),
    MT_CASE(selecting_a_catch_block), MT_CASE(copying_into_the_catch_parameter),
};

int main(void)
{
    return MT_RUN("vcr_eh_pure", cases);
}
