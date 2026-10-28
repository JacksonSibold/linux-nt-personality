/*
 * test_vcr_eh4_pure.c - tests for src/ntdll/vcr_eh4_pure.h: the variable-length numbers, the function descriptor (plain and
 * split into parts), state lookup, the cleanup chain of the unwind map and the try and catch block records.
 */
#include "minitest.h"
#include "vcr_eh4_pure.h"

#include <string.h>

#define IMG_SIZE 0x10000
static uint8_t g_img[IMG_SIZE];
static uint32_t g_top;

static uint32_t at(void) { return g_top; }

static void put8(uint8_t v) { g_img[g_top++] = v; }

static void put_uint(uint32_t v)
{
    if (v < (1u << 7)) {
        put8((uint8_t)(v << 1));
    } else if (v < (1u << 14)) {
        put8((uint8_t)((v << 2) | 1));
        put8((uint8_t)(v >> 6));
    } else if (v < (1u << 21)) {
        put8((uint8_t)((v << 3) | 3));
        put8((uint8_t)(v >> 5));
        put8((uint8_t)(v >> 13));
    } else if (v < (1u << 28)) {
        put8((uint8_t)((v << 4) | 7));
        put8((uint8_t)(v >> 4));
        put8((uint8_t)(v >> 12));
        put8((uint8_t)(v >> 20));
    } else {
        put8(0x0F);
        put8((uint8_t)v);
        put8((uint8_t)(v >> 8));
        put8((uint8_t)(v >> 16));
        put8((uint8_t)(v >> 24));
    }
}

static uint32_t put_len(uint32_t v)
{
    uint32_t save = g_top;
    put_uint(v);
    {
        uint32_t n = g_top - save;
        g_top = save;
        return n;
    }
}

static void put_rva(uint32_t v)
{
    put8((uint8_t)v);
    put8((uint8_t)(v >> 8));
    put8((uint8_t)(v >> 16));
    put8((uint8_t)(v >> 24));
}

static void reset(void)
{
    memset(g_img, 0, sizeof g_img);
    g_top = 0x100;
}

static void numbers(void)
{
    static const uint32_t vals[] = {0, 1, 127, 128, 16383, 16384, (1u << 21) - 1, 1u << 21, (1u << 28) - 1, 1u << 28, 0xFFFFFFFFu};
    size_t i;
    for (i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        const uint8_t *p;
        uint32_t start;
        reset();
        start = at();
        put_uint(vals[i]);
        p = g_img + start;
        MT_EXPECT(vcr4_uint(&p) == vals[i]);
        MT_EXPECT_EQ((long)(p - (g_img + start)), (long)(at() - start)); /* consumed exactly what was written */
    }
    MT_EXPECT_EQ((long)put_len(127), 1L);
    MT_EXPECT_EQ((long)put_len(128), 2L);
    MT_EXPECT_EQ((long)put_len(1u << 21), 4L);
    MT_EXPECT_EQ((long)put_len(1u << 28), 5L);
}

/* The unwind map of the test function: four entries.
 *   0 destroy the object at frame+0x20     1 destroy the object behind the pointer at frame+0x28
 *   2 nothing (a try's own state)           3 run the cleanup funclet 0xA300 */
static uint32_t g_unwind_map_rva, g_entry_start[4];

static void put_unwind_entry(int i, uint32_t type, int prev, uint32_t handler, uint32_t object)
{
    uint32_t back = prev < 0 ? 1 : at() - g_entry_start[prev];
    g_entry_start[i] = at();
    /* the word is (back << 2) | type, and its own length does not change what "back" measures: back runs from this entry's start */
    put_uint((back << 2) | type);
    if (type == VCR4_UNWIND_DTOR_OBJ || type == VCR4_UNWIND_DTOR_PTR) {
        put_rva(handler);
        put_uint(object);
    } else if (type == VCR4_UNWIND_FRAME) {
        put_rva(handler);
    }
}

static void build_unwind_map(void)
{
    g_unwind_map_rva = at();
    put_uint(4);
    put_unwind_entry(0, VCR4_UNWIND_DTOR_OBJ, -1, 0xA000, 0x20);
    put_unwind_entry(1, VCR4_UNWIND_DTOR_PTR, 0, 0xA100, 0x28);
    put_unwind_entry(2, VCR4_UNWIND_NONE, 1, 0, 0);
    put_unwind_entry(3, VCR4_UNWIND_FRAME, 2, 0xA300, 0);
}

/* state map: addresses from 0x1000; 0x1000 state -1 (entry value 0), 0x1010 state 0, 0x1030 state 3, 0x1050 state 1 */
static uint32_t build_ip_map(void)
{
    uint32_t r = at();
    put_uint(4);
    put_uint(0x00); put_uint(0); /* from 0x1000: state -1 */
    put_uint(0x10); put_uint(1); /* from 0x1010: state 0 */
    put_uint(0x20); put_uint(4); /* from 0x1030: state 3 */
    put_uint(0x20); put_uint(2); /* from 0x1050: state 1 */
    return r;
}

typedef struct {
    uint32_t types[8];
    uint32_t handlers[8];
    uint32_t objects[8];
    int n;
} unwind_log;

static void log_cb(void *ctx, const vcr_ui4 *ui)
{
    unwind_log *l = (unwind_log *)ctx;
    l->types[l->n] = ui->type;
    l->handlers[l->n] = ui->handler;
    l->objects[l->n] = ui->object;
    ++l->n;
}

static void descriptor_and_states(void)
{
    uint32_t ip_rva, desc;
    vcr_fd4 d;
    reset();
    build_unwind_map();
    ip_rva = build_ip_map();
    desc = at();
    put8(VCR4_UNWIND_MAP | VCR4_EHS);
    put_rva(g_unwind_map_rva);
    put_rva(ip_rva);
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));
    MT_EXPECT_EQ(d.unwind_count, 4);
    MT_EXPECT_EQ(d.ip_count, 4);
    MT_EXPECT_EQ(d.tryblock_count, 0);
    MT_EXPECT(d.header & VCR4_EHS);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x0FFF), -1);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x1000), -1);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x100F), -1);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x1010), 0);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x102F), 0);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x1030), 3);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x1050), 1);
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x9000), 1);
    /* a reserved header bit is refused */
    g_img[desc] = VCR4_RESERVED;
    MT_EXPECT(!vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));
}

static void cleanup_chain(void)
{
    uint32_t ip_rva, desc;
    vcr_fd4 d;
    unwind_log l;
    reset();
    build_unwind_map();
    ip_rva = build_ip_map();
    desc = at();
    put8(VCR4_UNWIND_MAP);
    put_rva(g_unwind_map_rva);
    put_rva(ip_rva);
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));

    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 3, -1, log_cb, &l), 0);
    MT_EXPECT_EQ(l.n, 3); /* entry 2 has no handler */
    MT_EXPECT(l.types[0] == VCR4_UNWIND_FRAME && l.handlers[0] == 0xA300);
    MT_EXPECT(l.types[1] == VCR4_UNWIND_DTOR_PTR && l.handlers[1] == 0xA100 && l.objects[1] == 0x28);
    MT_EXPECT(l.types[2] == VCR4_UNWIND_DTOR_OBJ && l.handlers[2] == 0xA000 && l.objects[2] == 0x20);

    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 3, 1, log_cb, &l), 0); /* stop at state 1: entries 3 and 2 only */
    MT_EXPECT(l.n == 1 && l.handlers[0] == 0xA300);

    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 1, 1, log_cb, &l), 0); /* nothing to do */
    MT_EXPECT_EQ(l.n, 0);
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 1, 5, log_cb, &l), 0); /* already below the target */
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 4, -1, log_cb, &l), -1); /* outside the map */
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, -2, -1, log_cb, &l), -1);
    memset(&l, 0, sizeof l);
    MT_EXPECT_EQ(vcr_fd4_local_unwind(&d, 0, -1, log_cb, &l), 0);
    MT_EXPECT(l.n == 1 && l.handlers[0] == 0xA000);
}

static void split_function(void)
{
    uint32_t map_a, map_b, table, desc;
    vcr_fd4 d;
    reset();
    build_unwind_map();
    map_a = at();
    put_uint(1);
    put_uint(0x00); put_uint(3); /* part A: always state 2 */
    map_b = at();
    put_uint(1);
    put_uint(0x00); put_uint(1); /* part B: always state 0 */
    table = at();
    put_uint(2);
    put_rva(0x1000); put_rva(map_a);
    put_rva(0x2000); put_rva(map_b);
    desc = at();
    put8(VCR4_UNWIND_MAP | VCR4_IS_SEPARATED);
    put_rva(g_unwind_map_rva);
    put_rva(table);
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x1000, 0x1234), 2);
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x2000, &d));
    MT_EXPECT_EQ(vcr_fd4_ip_state(&d, 0x2000, 0x2234), 0);
    MT_EXPECT(!vcr_fd4_parse((const char *)g_img, g_img + desc, 0x3000, &d)); /* no part for this function */
}

static void funclet_descriptor(void)
{
    uint32_t ip_rva, desc;
    vcr_fd4 d;
    reset();
    build_unwind_map();
    ip_rva = build_ip_map();
    desc = at();
    put8(VCR4_IS_CATCH | VCR4_UNWIND_MAP);
    put_rva(g_unwind_map_rva);
    put_rva(ip_rva);
    put_uint(0x58); /* where the parent frame pointer is kept */
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));
    MT_EXPECT(d.header & VCR4_IS_CATCH);
    MT_EXPECT_EQ(d.frame, 0x58);
}

static void try_and_catch_records(void)
{
    uint32_t blocks, tries, ip_rva, desc;
    vcr_fd4 d;
    vcr_tb4 t;
    vcr_cb4 c;
    const uint8_t *p, *q;
    reset();
    build_unwind_map();
    ip_rva = build_ip_map();
    /* two catch blocks: catch (T& e) with a frame slot, then catch (...) with a continuation address */
    blocks = at();
    put_uint(2);
    put8(VCR4_CATCH_FLAGS | VCR4_CATCH_TYPE_INFO | VCR4_CATCH_OFFSET);
    put_uint(8);       /* flags: reference */
    put_rva(0x7000);   /* type */
    put_uint(0x40);    /* slot */
    put_rva(0x5100);   /* funclet */
    put8(VCR4_CATCH_RET_ONE);
    put_rva(0x5200);
    put_uint(0x30);    /* continuation, relative to the function start */
    tries = at();
    put_uint(2);
    put_uint(1); put_uint(2); put_uint(3); put_rva(blocks);
    put_uint(0); put_uint(5); put_uint(6); put_rva(0); /* a try with no catch blocks listed */
    desc = at();
    put8(VCR4_UNWIND_MAP | VCR4_TRYBLOCK_MAP);
    put_rva(g_unwind_map_rva);
    put_rva(tries);
    put_rva(ip_rva);
    MT_EXPECT(vcr_fd4_parse((const char *)g_img, g_img + desc, 0x1000, &d));
    MT_EXPECT_EQ(d.tryblock_count, 2);

    p = d.tryblock_map;
    vcr_tb4_read((const char *)g_img, &p, &t);
    MT_EXPECT(t.start_level == 1 && t.end_level == 2 && t.catch_level == 3 && t.catch_count == 2);
    q = t.catchblocks;
    MT_EXPECT(vcr_cb4_read(&q, 0x1000, &c));
    MT_EXPECT(c.flags == 8 && c.type_info == 0x7000 && c.offset == 0x40 && c.handler == 0x5100 && c.ret_addr[0] == 0);
    MT_EXPECT(vcr_cb4_read(&q, 0x1000, &c));
    MT_EXPECT(c.type_info == 0 && c.handler == 0x5200 && c.ret_addr[0] == 0x1030 && c.ret_addr[1] == 0);
    vcr_tb4_read((const char *)g_img, &p, &t);
    MT_EXPECT(t.start_level == 0 && t.end_level == 5 && t.catch_level == 6 && t.catch_count == 0 && t.catchblocks == NULL);
}

static void catch_continuations_and_bad_headers(void)
{
    vcr_cb4 c;
    const uint8_t *p;
    reset();
    /* separated: absolute offsets, two of them */
    put8(VCR4_CATCH_SEPARATED | VCR4_CATCH_RET_TWO);
    put_rva(0x5300);
    put_rva(0x2100);
    put_rva(0x2200);
    /* relative, two of them */
    put8(VCR4_CATCH_RET_TWO);
    put_rva(0x5400);
    put_uint(0x10);
    put_uint(0x20);
    /* the combination the format does not define, and an unknown bit */
    put8(VCR4_CATCH_RET_ONE | VCR4_CATCH_RET_TWO);
    put8(0x40);
    p = g_img + 0x100;
    MT_EXPECT(vcr_cb4_read(&p, 0x1000, &c));
    MT_EXPECT(c.handler == 0x5300 && c.ret_addr[0] == 0x2100 && c.ret_addr[1] == 0x2200);
    MT_EXPECT(vcr_cb4_read(&p, 0x1000, &c));
    MT_EXPECT(c.handler == 0x5400 && c.ret_addr[0] == 0x1010 && c.ret_addr[1] == 0x1020);
    MT_EXPECT(!vcr_cb4_read(&p, 0x1000, &c));
    p = g_img + at() - 1;
    MT_EXPECT(!vcr_cb4_read(&p, 0x1000, &c));
}

static const mt_case_t cases[] = {
    MT_CASE(numbers), MT_CASE(descriptor_and_states), MT_CASE(cleanup_chain), MT_CASE(split_function),
    MT_CASE(funclet_descriptor), MT_CASE(try_and_catch_records), MT_CASE(catch_continuations_and_bad_headers),
};

int main(void)
{
    return MT_RUN("vcr_eh4_pure", cases);
}
