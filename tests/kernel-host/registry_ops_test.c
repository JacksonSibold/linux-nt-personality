/* registry_ops_test.c - registry value operations not covered by registry_test.c.
 *
 * registry_test.c covers key disposition, the nine value types, embedded NULs,
 * missing keys, NtQueryKey sizing, enumeration past the end, and access on
 * keys. This file covers the value side: partial-information round trips,
 * overwrite replacing type and length, empty data, case-insensitive names,
 * delete, NtEnumerateValueKey over every index, buffer sizing for the three
 * information classes, and access checks on value reads and writes.
 *
 * Built and run by tests/kernel-host/Makefile (userspace kshim, no kernel
 * boot). Test style follows src/kernel/nt/registry_test.c.
 */
#include "registry.h"
#include "object.h"
#include "handle.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while (0)

#define ASSERT_STATUS(expr, expect) do { \
    NTSTATUS _s = (expr); \
    if (_s != (expect)) { \
        fprintf(stderr, "FAIL %s:%d: expect 0x%08lx got 0x%08lx for %s\n", \
                __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_s, #expr); \
        g_fail++; return; \
    } \
} while (0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

static WCHAR wbuf1[512];
static WCHAR wbuf2[512];

static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t blen)
{
    size_t len = strlen(s);
    ASSERT(len * sizeof(WCHAR) < blen);
    for (size_t i = 0; i < len; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    buf[len] = 0;
    us->Buffer = buf;
    us->Length = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = (USHORT)(us->Length + sizeof(WCHAR));
}

static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us)
{
    OBJECT_ATTRIBUTES oa = {0};
    oa.Length = sizeof(oa);
    oa.ObjectName = us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    return oa;
}

static struct nt_eprocess *make_proc(ULONG pid, const char *name)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, name);
    if (st != STATUS_SUCCESS || !p) { fprintf(stderr, "make_proc failed\n"); g_fail++; return NULL; }
    kshim_set_current_process(p);
    return p;
}

static struct nt_ethread *make_thread(struct nt_eprocess *proc, ULONG tid)
{
    struct nt_ethread *t = NULL;
    NTSTATUS st = ObCreateThreadObject(&t, proc, tid);
    if (st != STATUS_SUCCESS || !t) { fprintf(stderr, "make_thread failed\n"); g_fail++; return NULL; }
    kshim_set_current_thread(t);
    return t;
}

static void reset_state(void)
{
    ntreg_cleanup();
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager();
    ASSERT(st == STATUS_SUCCESS);
    ntreg_init();
}

static void end_proc(struct nt_eprocess *proc, struct nt_ethread *thr)
{
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
}

/* Creates a scratch key and returns its handle. The caller deletes it. */
static NTSTATUS scratch_key(HANDLE *out, const char *leaf, ACCESS_MASK access)
{
    char path[160];
    snprintf(path, sizeof(path), "\\Registry\\User\\Software\\%s", leaf);
    UNICODE_STRING us;
    make_us(&us, path, wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us);
    ULONG disp = 0;
    return NtCreateKey(out, access, &oa, 0, NULL, 0, &disp);
}

/* Deletes a scratch key by reopening it, then closing the handle. */
static void drop_scratch_key(struct nt_eprocess *proc, const char *leaf)
{
    char path[160];
    snprintf(path, sizeof(path), "\\Registry\\User\\Software\\%s", leaf);
    UNICODE_STRING us;
    make_us(&us, path, wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa = make_oa(&us);
    HANDLE h = NULL;
    if (NT_SUCCESS(NtOpenKey(&h, KEY_ALL_ACCESS, &oa))) {
        NtDeleteKey(h);
        ObCloseHandle(proc, h);
    }
}

static NTSTATUS set_value(HANDLE key, const char *name, ULONG type, const void *data, ULONG len)
{
    UNICODE_STRING vn;
    make_us(&vn, name, wbuf2, sizeof(wbuf2));
    return NtSetValueKey(key, &vn, 0, type, (PVOID)data, len);
}

static NTSTATUS query_partial(HANDLE key, const char *name, void *buf, ULONG len, ULONG *rlen)
{
    UNICODE_STRING vn;
    make_us(&vn, name, wbuf2, sizeof(wbuf2));
    return NtQueryValueKey(key, &vn, KeyValuePartialInformation, buf, len, rlen);
}

/* -------------------------------------------------------------------------- */
/* 1. DWORD round trip through partial information                             */
/* -------------------------------------------------------------------------- */
static void test_dword_partial_round_trip(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1100, "regops1");
    struct nt_ethread *thr = make_thread(proc, 1);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_dword", KEY_ALL_ACCESS), STATUS_SUCCESS);

    ULONG v = 0xA1B2C3D4UL;
    ASSERT_STATUS(set_value(hk, "Count", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);

    unsigned char buf[64];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Count", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    /* Header is TitleIndex, Type, DataLength (12 bytes) then the data. */
    ASSERT(rlen == offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG));
    KEY_VALUE_PARTIAL_INFORMATION *p = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    ASSERT(p->Type == REG_DWORD);
    ASSERT(p->DataLength == sizeof(ULONG));
    ULONG got = 0;
    memcpy(&got, p->Data, sizeof(got));
    ASSERT(got == 0xA1B2C3D4UL);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_dword");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. overwrite replaces both the type and the length                          */
/* -------------------------------------------------------------------------- */
static void test_overwrite_replaces_type_and_length(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1101, "regops2");
    struct nt_ethread *thr = make_thread(proc, 2);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_overwrite", KEY_ALL_ACCESS), STATUS_SUCCESS);

    const char *first = "abcdef";                 /* 6 bytes, REG_SZ */
    ASSERT_STATUS(set_value(hk, "Mode", REG_SZ, first, 6), STATUS_SUCCESS);
    ULONG v = 7;
    ASSERT_STATUS(set_value(hk, "Mode", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);

    unsigned char buf[64];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Mode", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    KEY_VALUE_PARTIAL_INFORMATION *p = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    ASSERT(p->Type == REG_DWORD);
    ASSERT(p->DataLength == 4);
    ASSERT(rlen == offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + 4);


    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_overwrite");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. empty data                                                               */
/* -------------------------------------------------------------------------- */
static void test_empty_value_data(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1102, "regops3");
    struct nt_ethread *thr = make_thread(proc, 3);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_empty", KEY_ALL_ACCESS), STATUS_SUCCESS);

    ASSERT_STATUS(set_value(hk, "Blank", REG_NONE, NULL, 0), STATUS_SUCCESS);

    unsigned char buf[32];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Blank", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    KEY_VALUE_PARTIAL_INFORMATION *p = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
    ASSERT(p->Type == REG_NONE);
    ASSERT(p->DataLength == 0);
    ASSERT(rlen == offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data));

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_empty");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 4. value names are case-insensitive                                         */
/* -------------------------------------------------------------------------- */
static void test_value_name_case_insensitive(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1103, "regops4");
    struct nt_ethread *thr = make_thread(proc, 4);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_case", KEY_ALL_ACCESS), STATUS_SUCCESS);

    ULONG v = 99;
    ASSERT_STATUS(set_value(hk, "Alpha", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);

    ULONG got = 0;
    unsigned char buf[32];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "ALPHA", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    memcpy(&got, ((KEY_VALUE_PARTIAL_INFORMATION *)buf)->Data, sizeof(got));
    ASSERT(got == 99);

    /* Setting the other spelling updates the same value rather than adding one. */
    v = 100;
    ASSERT_STATUS(set_value(hk, "alpha", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    ULONG rlen2 = 0;
    NTSTATUS e = NtEnumerateValueKey(hk, 1, KeyValueBasicInformation, buf, sizeof(buf), &rlen2);
    ASSERT(e == STATUS_NO_MORE_ENTRIES);
    ASSERT_STATUS(query_partial(hk, "Alpha", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    memcpy(&got, ((KEY_VALUE_PARTIAL_INFORMATION *)buf)->Data, sizeof(got));
    ASSERT(got == 100);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_case");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. delete value                                                             */
/* -------------------------------------------------------------------------- */
static void test_delete_value(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1104, "regops5");
    struct nt_ethread *thr = make_thread(proc, 5);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_delete", KEY_ALL_ACCESS), STATUS_SUCCESS);

    ULONG v = 1;
    ASSERT_STATUS(set_value(hk, "Keep", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    ASSERT_STATUS(set_value(hk, "Drop", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);

    UNICODE_STRING vn;
    make_us(&vn, "Drop", wbuf2, sizeof(wbuf2));
    ASSERT_STATUS(NtDeleteValueKey(hk, &vn), STATUS_SUCCESS);
    ASSERT_STATUS(NtDeleteValueKey(hk, &vn), STATUS_OBJECT_NAME_NOT_FOUND);

    unsigned char buf[32];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Drop", buf, sizeof(buf), &rlen), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT_STATUS(query_partial(hk, "Keep", buf, sizeof(buf), &rlen), STATUS_SUCCESS);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_delete");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 6. enumeration visits each value once and then reports no more entries      */
/* -------------------------------------------------------------------------- */
static void test_enumerate_values_each_once(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1105, "regops6");
    struct nt_ethread *thr = make_thread(proc, 6);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_enum", KEY_ALL_ACCESS), STATUS_SUCCESS);

    static const char *names[3] = { "One", "Two", "Three" };
    ULONG v = 0;
    for (int i = 0; i < 3; i++) {
        v = (ULONG)i;
        ASSERT_STATUS(set_value(hk, names[i], REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    }

    int seen[3] = {0, 0, 0};
    for (ULONG idx = 0; idx < 3; idx++) {
        unsigned char buf[128];
        ULONG rlen = 0;
        ASSERT_STATUS(NtEnumerateValueKey(hk, idx, KeyValueBasicInformation, buf, sizeof(buf), &rlen),
                      STATUS_SUCCESS);
        KEY_VALUE_BASIC_INFORMATION *b = (KEY_VALUE_BASIC_INFORMATION *)buf;
        ASSERT(b->Type == REG_DWORD);
        ASSERT(rlen == offsetof(KEY_VALUE_BASIC_INFORMATION, Name) + b->NameLength);
        ASSERT(b->NameLength % sizeof(WCHAR) == 0);
        int hit = -1;
        for (int i = 0; i < 3; i++) {
            size_t n = strlen(names[i]);
            if (b->NameLength != n * sizeof(WCHAR)) continue;
            bool same = true;
            for (size_t c = 0; c < n; c++)
                if (b->Name[c] != (WCHAR)(unsigned char)names[i][c]) { same = false; break; }
            if (same) hit = i;
        }
        ASSERT(hit >= 0);
        seen[hit]++;
    }
    ASSERT(seen[0] == 1 && seen[1] == 1 && seen[2] == 1);

    unsigned char buf[128];
    ULONG rlen = 0;
    ASSERT_STATUS(NtEnumerateValueKey(hk, 3, KeyValueBasicInformation, buf, sizeof(buf), &rlen),
                  STATUS_NO_MORE_ENTRIES);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_enum");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 7. buffer sizing: too small reports the needed length, zero length probes   */
/* -------------------------------------------------------------------------- */
static void test_value_buffer_sizing(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1106, "regops7");
    struct nt_ethread *thr = make_thread(proc, 7);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_size", KEY_ALL_ACCESS), STATUS_SUCCESS);

    ULONG v = 0x55;
    ASSERT_STATUS(set_value(hk, "Size", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    const ULONG need = (ULONG)offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + 4;

    /* A zero-length probe reports the size without writing. */
    ULONG rlen = 0;
    NTSTATUS st = query_partial(hk, "Size", NULL, 0, &rlen);
    ASSERT(st == STATUS_BUFFER_TOO_SMALL);
    ASSERT(rlen == need);

    /* One byte short of the full structure is also too small. */
    unsigned char buf[64];
    memset(buf, 0xEE, sizeof(buf));
    rlen = 0;
    st = query_partial(hk, "Size", buf, need - 1, &rlen);
    ASSERT(st == STATUS_BUFFER_TOO_SMALL);
    ASSERT(rlen == need);
    ASSERT(buf[0] == 0xEE);          /* nothing was written */

    /* The exact size succeeds. */
    rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Size", buf, need, &rlen), STATUS_SUCCESS);
    ASSERT(rlen == need);

    /* Full information reports the name bytes plus the data bytes. */
    UNICODE_STRING vn;
    make_us(&vn, "Size", wbuf2, sizeof(wbuf2));
    rlen = 0;
    st = NtQueryValueKey(hk, &vn, KeyValueFullInformation, NULL, 0, &rlen);
    ASSERT(st == STATUS_BUFFER_TOO_SMALL);
    ASSERT(rlen == offsetof(KEY_VALUE_FULL_INFORMATION, Name) + 8 + 4);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_size");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 8. access checks on value reads and writes                                  */
/* -------------------------------------------------------------------------- */
static void test_value_access_checks(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1107, "regops8");
    struct nt_ethread *thr = make_thread(proc, 8);
    HANDLE hw = NULL;
    ASSERT_STATUS(scratch_key(&hw, "ntpers_ops_access", KEY_ALL_ACCESS), STATUS_SUCCESS);
    ULONG v = 3;
    ASSERT_STATUS(set_value(hw, "Guard", REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    ObCloseHandle(proc, hw);

    char path[] = "\\Registry\\User\\Software\\ntpers_ops_access";
    UNICODE_STRING us;
    make_us(&us, path, wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us);

    /* Read-only handle: writes are refused, reads succeed. */
    HANDLE hro = NULL;
    ASSERT_STATUS(NtOpenKey(&hro, KEY_QUERY_VALUE, &oa), STATUS_SUCCESS);
    ASSERT_STATUS(set_value(hro, "Guard", REG_DWORD, &v, sizeof(v)), STATUS_ACCESS_DENIED);
    unsigned char buf[32];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hro, "Guard", buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    UNICODE_STRING vn;
    make_us(&vn, "Guard", wbuf2, sizeof(wbuf2));
    ASSERT_STATUS(NtDeleteValueKey(hro, &vn), STATUS_ACCESS_DENIED);
    ObCloseHandle(proc, hro);

    /* Write-only handle: the value cannot be read back. */
    HANDLE hwo = NULL;
    ASSERT_STATUS(NtOpenKey(&hwo, KEY_SET_VALUE, &oa), STATUS_SUCCESS);
    rlen = 0;
    ASSERT_STATUS(query_partial(hwo, "Guard", buf, sizeof(buf), &rlen), STATUS_ACCESS_DENIED);
    ObCloseHandle(proc, hwo);

    drop_scratch_key(proc, "ntpers_ops_access");
    end_proc(proc, thr);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* 7. REG-1: a delete of a value waits for a set still persisting that value   */
/* The setter is held in the window after the key lock is released and before  */
/* the value is written to disk (ntreg_test_persist_hook). A delete of the same */
/* value must not finish while the set is held. Before the fix it did, and the */
/* setter then persisted a value the delete had already freed.                 */
/* -------------------------------------------------------------------------- */
extern void (*ntreg_test_persist_hook)(void);

static pthread_mutex_t g_race_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_race_cv = PTHREAD_COND_INITIALIZER;
static int g_race_in_window, g_race_release, g_race_del_done;
static struct nt_eprocess *g_race_proc;
static HANDLE g_race_key;
static NTSTATUS g_race_set_st, g_race_del_st;

static void race_window_hook(void)
{
    pthread_mutex_lock(&g_race_mu);
    g_race_in_window = 1;
    pthread_cond_broadcast(&g_race_cv);
    while (!g_race_release) pthread_cond_wait(&g_race_cv, &g_race_mu);
    pthread_mutex_unlock(&g_race_mu);
}

static void *race_setter(void *arg)
{
    (void)arg;
    WCHAR nb[16];
    UNICODE_STRING vn;
    make_us(&vn, "Race", nb, sizeof(nb));
    kshim_set_current_process(g_race_proc);
    kshim_set_current_thread(NULL);
    ULONG v = 7;
    g_race_set_st = NtSetValueKey(g_race_key, &vn, 0, REG_DWORD, &v, sizeof(v));
    return NULL;
}

static void *race_deleter(void *arg)
{
    (void)arg;
    WCHAR nb[16];
    UNICODE_STRING vn;
    make_us(&vn, "Race", nb, sizeof(nb));
    kshim_set_current_process(g_race_proc);
    kshim_set_current_thread(NULL);
    g_race_del_st = NtDeleteValueKey(g_race_key, &vn);
    pthread_mutex_lock(&g_race_mu);
    g_race_del_done = 1;
    pthread_cond_broadcast(&g_race_cv);
    pthread_mutex_unlock(&g_race_mu);
    return NULL;
}

static void test_delete_waits_for_persisting_set(void)
{
    reset_state();
    struct nt_eprocess *proc = make_proc(1105, "regops7");
    struct nt_ethread *thr = make_thread(proc, 7);
    HANDLE hk = NULL;
    ASSERT_STATUS(scratch_key(&hk, "ntpers_ops_race", KEY_ALL_ACCESS), STATUS_SUCCESS);

    g_race_proc = proc;
    g_race_key = hk;
    g_race_in_window = g_race_release = g_race_del_done = 0;
    g_race_set_st = g_race_del_st = STATUS_UNSUCCESSFUL;
    ntreg_test_persist_hook = race_window_hook;

    pthread_t ts, td;
    ASSERT(pthread_create(&ts, NULL, race_setter, NULL) == 0);
    pthread_mutex_lock(&g_race_mu);
    while (!g_race_in_window) pthread_cond_wait(&g_race_cv, &g_race_mu);
    pthread_mutex_unlock(&g_race_mu);

    ASSERT(pthread_create(&td, NULL, race_deleter, NULL) == 0);
    struct timespec wait = { 0, 200L * 1000L * 1000L };
    nanosleep(&wait, NULL);

    pthread_mutex_lock(&g_race_mu);
    int delete_finished_early = g_race_del_done;
    g_race_release = 1;
    pthread_cond_broadcast(&g_race_cv);
    pthread_mutex_unlock(&g_race_mu);

    pthread_join(ts, NULL);
    pthread_join(td, NULL);
    ntreg_test_persist_hook = NULL;

    ASSERT(!delete_finished_early);
    ASSERT_STATUS(g_race_set_st, STATUS_SUCCESS);
    ASSERT_STATUS(g_race_del_st, STATUS_SUCCESS);
    unsigned char buf[32];
    ULONG rlen = 0;
    ASSERT_STATUS(query_partial(hk, "Race", buf, sizeof(buf), &rlen), STATUS_OBJECT_NAME_NOT_FOUND);

    ObCloseHandle(proc, hk);
    drop_scratch_key(proc, "ntpers_ops_race");
    end_proc(proc, thr);
    PASS();
}

int main(void)
{
    printf("registry value ops tests starting\n");
    test_dword_partial_round_trip();
    test_overwrite_replaces_type_and_length();
    test_empty_value_data();
    test_value_name_case_insensitive();
    test_delete_value();
    test_enumerate_values_each_once();
    test_value_buffer_sizing();
    test_value_access_checks();
    test_delete_waits_for_persisting_set();
    printf("registry value ops tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    return 0;
}
