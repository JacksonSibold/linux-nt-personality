/* port_test.c - host tests for LPC ports (src/kernel/nt/port.c).
 *
 * Covers connect/accept/complete, request/reply round trips, listen and receive
 * timeouts, an alertable wait, disconnect wake-up, PORT_MESSAGE validation,
 * the 256-byte message cap, double and unknown replies, connection refusal,
 * the connection queue cap, and an allocation-count leak check.
 *
 * Every test uses one connection port, "\LpcMain", created before the
 * allocation baseline. The object manager keeps a named object alive in its
 * directory after its last handle closes, so creating a new named port per test
 * would show up as a leak that is not the port code's. Only per-connection
 * state is checked against the baseline.
 *
 * Client and server flows run on pthreads. Each worker has an eprocess and an
 * ethread set up before the baseline, so thread setup does not count either. */

#include "port.h"
#include "object.h"
#include "handle.h"
#include "sync.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while (0)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; return; } } while (0)
#define ASSERT_STATUS(expr, expect) do { NTSTATUS _s = (expr); if (_s != (expect)) { fprintf(stderr, "FAIL %s:%d: expect 0x%lx got 0x%lx for %s\n", __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_s, #expr); g_fail++; return; } } while (0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

#define NW 26
#define MAIN_PORT_NAME "\\LpcMain"

/* A whole message buffer, as a user program would pass it. */
typedef union {
    PORT_MESSAGE hdr;
    UCHAR        raw[512];
} msgbuf_t;

struct worker {
    pthread_t           pt;
    struct nt_eprocess *proc;
    struct nt_ethread  *thr;
    struct nt_thread   *nthr;
    NTSTATUS          (*fn)(struct worker *);
    NTSTATUS            status;
    int                 count;
    int                 started;
    HANDLE              in_h;
    HANDLE              out_h;
    ULONG               maxlen;
    UNICODE_STRING      name;
    WCHAR               name_buf[64];
    msgbuf_t            req, rep, recv;
};

static struct worker pool[NW];
static struct nt_eprocess *g_proc;
static struct nt_ethread *g_main_thr;
static struct nt_thread *g_main_nthr;
static HANDLE g_conn;
static HANDLE g_close; /* second named port, closed by its test */

static void set_name(struct worker *w, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) w->name_buf[i] = (WCHAR)(unsigned char)s[i];
    w->name_buf[n] = 0;
    w->name.Buffer = w->name_buf;
    w->name.Length = (USHORT)(n * sizeof(WCHAR));
    w->name.MaximumLength = w->name.Length;
}

static void msg_set(PORT_MESSAGE *h, USHORT type, const char *data)
{
    ULONG dlen = data ? (ULONG)strlen(data) : 0;
    memset(h, 0, sizeof(*h));
    h->DataLength = (USHORT)dlen;
    h->TotalLength = (USHORT)(sizeof(PORT_MESSAGE) + dlen);
    h->Type = type;
    if (dlen) memcpy((UCHAR *)h + sizeof(PORT_MESSAGE), data, dlen);
}

static const char *msg_data(const PORT_MESSAGE *h)
{
    return (const char *)h + sizeof(PORT_MESSAGE);
}

/* Worker functions. Each runs on its own thread and returns the final status. */

static NTSTATUS fn_connect(struct worker *w)
{
    return nt_svc_NtConnectPort(&w->out_h, &w->name, NULL, NULL, NULL, &w->maxlen, NULL, NULL);
}

/* Client: connect, then three request/reply round trips. */
static NTSTATUS fn_client_rounds(struct worker *w)
{
    NTSTATUS st = fn_connect(w);
    if (st != STATUS_SUCCESS) return st;
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "ping-%d", i);
        msg_set(&w->req.hdr, LPC_REQUEST, data);
        st = nt_svc_NtRequestWaitReplyPort(w->out_h, &w->req.hdr, &w->rep.hdr);
        if (st != STATUS_SUCCESS) break;
        if (w->rep.hdr.Type != LPC_REPLY || w->rep.hdr.DataLength != strlen(data) ||
            memcmp(msg_data(&w->rep.hdr), data, strlen(data)) != 0) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        w->count++;
    }
    ObCloseHandle(w->proc, w->out_h);
    return st;
}

/* Server: listen, accept, complete, then echo three requests. */
static NTSTATUS fn_server_rounds(struct worker *w)
{
    NTSTATUS st = nt_svc_NtListenPort(w->in_h, &w->recv.hdr);
    if (st != STATUS_SUCCESS) return st;
    st = nt_svc_NtAcceptConnectPort(&w->out_h, w->in_h, &w->recv.hdr, true, NULL, NULL);
    if (st != STATUS_SUCCESS) return st;
    st = nt_svc_NtCompleteConnectPort(w->out_h);
    if (st != STATUS_SUCCESS) return st;
    st = nt_svc_NtReplyWaitReceivePort(w->out_h, NULL, NULL, &w->recv.hdr);
    for (int i = 0; st == STATUS_SUCCESS && i < 3; i++) {
        memcpy(&w->rep, &w->recv, w->recv.hdr.TotalLength);
        w->rep.hdr.Type = LPC_REPLY;
        w->count++;
        if (i < 2) st = nt_svc_NtReplyWaitReceivePort(w->out_h, NULL, &w->rep.hdr, &w->recv.hdr);
        else st = nt_svc_NtReplyPort(w->out_h, &w->rep.hdr);
    }
    ObCloseHandle(w->proc, w->out_h);
    return st;
}

/* Blocks in NtReplyWaitReceivePort on in_h with no reply. */
static NTSTATUS fn_receive_block(struct worker *w)
{
    return nt_svc_NtReplyWaitReceivePort(w->in_h, NULL, NULL, &w->recv.hdr);
}

/* Client: one request on in_h, waits for the reply. */
static NTSTATUS fn_request_once(struct worker *w)
{
    msg_set(&w->req.hdr, LPC_REQUEST, "once");
    NTSTATUS st = nt_svc_NtRequestWaitReplyPort(w->in_h, &w->req.hdr, &w->rep.hdr);
    if (st == STATUS_SUCCESS) w->count++;
    return st;
}

/* Thread entry: binds the worker's process, thread and nt_thread, then runs fn. */
static void *worker_main(void *arg)
{
    struct worker *w = arg;
    kshim_set_current_process(w->proc);
    kshim_set_current_thread(w->thr);
    nt_set_current_thread(w->nthr);
    w->status = w->fn(w);
    return NULL;
}

static void worker_start(struct worker *w, NTSTATUS (*fn)(struct worker *))
{
    w->fn = fn;
    w->status = STATUS_PENDING;
    w->count = 0;
    pthread_create(&w->pt, NULL, worker_main, w);
    w->started = 1;
}

static void worker_join(struct worker *w)
{
    if (w->started) {
        pthread_join(w->pt, NULL);
        w->started = 0;
    }
}

static void pool_init(void)
{
    NTSTATUS st = ObCreateProcessObject(&g_proc, 3000, "lpctest");
    if (st != STATUS_SUCCESS) { fprintf(stderr, "process object failed\n"); exit(1); }
    kshim_set_current_process(g_proc);
    st = ObCreateThreadObject(&g_main_thr, g_proc, 1);
    if (st != STATUS_SUCCESS) { fprintf(stderr, "main thread object failed\n"); exit(1); }
    kshim_set_current_thread(g_main_thr);
    if (nt_thread_create(&g_main_nthr) != STATUS_SUCCESS) { fprintf(stderr, "nt thread failed\n"); exit(1); }
    nt_set_current_thread(g_main_nthr);
    for (int i = 0; i < NW; i++) {
        struct worker *w = &pool[i];
        w->proc = g_proc;
        st = ObCreateThreadObject(&w->thr, g_proc, (ULONG)(100 + i));
        if (st != STATUS_SUCCESS) { fprintf(stderr, "worker thread object failed\n"); exit(1); }
        if (nt_thread_create(&w->nthr) != STATUS_SUCCESS) { fprintf(stderr, "worker nt thread failed\n"); exit(1); }
    }
}

/* Creates the connection port named name. */
static NTSTATUS create_port(const char *name, ULONG maxlen, HANDLE *out)
{
    UNICODE_STRING us;
    WCHAR buf[64];
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) buf[i] = (WCHAR)(unsigned char)name[i];
    us.Buffer = buf;
    us.Length = (USHORT)(n * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    return nt_svc_NtCreatePort(out, &oa, 0, maxlen, 0);
}

/* Connects client worker c to the main port, and accepts it on this thread.
 * On success c->out_h is the client handle and *srv is the server handle. */
static NTSTATUS open_pair(HANDLE *srv, struct worker *c)
{
    set_name(c, MAIN_PORT_NAME);
    worker_start(c, fn_connect);
    msgbuf_t req;
    NTSTATUS st = nt_svc_NtListenPort(g_conn, &req.hdr);
    if (st == STATUS_SUCCESS) st = nt_svc_NtAcceptConnectPort(srv, g_conn, &req.hdr, true, NULL, NULL);
    if (st == STATUS_SUCCESS) st = nt_svc_NtCompleteConnectPort(*srv);
    worker_join(c);
    if (st != STATUS_SUCCESS) return st;
    return c->status;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

TEST(round_trip)
{
    struct worker *s = &pool[0], *c = &pool[1];
    s->in_h = g_conn;
    set_name(c, MAIN_PORT_NAME);
    worker_start(s, fn_server_rounds);
    worker_start(c, fn_client_rounds);
    worker_join(s);
    worker_join(c);
    ASSERT_STATUS(s->status, STATUS_SUCCESS);
    ASSERT(s->count == 3);
    ASSERT_STATUS(c->status, STATUS_SUCCESS);
    ASSERT(c->count == 3);
    PASS();
}

TEST(timeouts_and_disconnect)
{
    HANDLE srv = NULL;
    struct worker *c = &pool[2];
    LARGE_INTEGER t;
    t.QuadPart = -1000000LL; /* 100 ms, relative */
    msgbuf_t buf;

    /* No client queued: the listen times out. */
    ASSERT_STATUS(port_listen_wait(g_conn, false, &t, &buf.hdr), STATUS_TIMEOUT);

    ASSERT_STATUS(open_pair(&srv, c), STATUS_SUCCESS);
    /* Nothing sent: the receive times out and the pair stays connected. */
    ASSERT_STATUS(port_receive_wait(srv, false, &t, &buf.hdr), STATUS_TIMEOUT);

    /* A receive blocked on the server side wakes with DISCONNECTED when the client closes. */
    struct worker *d = &pool[3];
    d->in_h = srv;
    worker_start(d, fn_receive_block);
    ASSERT_STATUS(ObCloseHandle(g_proc, c->out_h), STATUS_SUCCESS);
    worker_join(d);
    ASSERT_STATUS(d->status, STATUS_PORT_DISCONNECTED);

    ASSERT_STATUS(ObCloseHandle(g_proc, srv), STATUS_SUCCESS);
    PASS();
}

TEST(alertable_listen)
{
    msgbuf_t buf;
    LARGE_INTEGER t;
    t.QuadPart = -10000000LL; /* 1 s; the alert must end the wait before this */
    nt_thread_alert(g_main_nthr);
    NTSTATUS st = port_listen_wait(g_conn, true, &t, &buf.hdr);
    ASSERT(st == STATUS_USER_APC || st == STATUS_ALERTED);
    nt_thread_clear_alert(g_main_nthr);
    PASS();
}

TEST(message_validation)
{
    HANDLE srv = NULL;
    struct worker *c = &pool[4];
    ASSERT_STATUS(open_pair(&srv, c), STATUS_SUCCESS);
    HANDLE cli = c->out_h;
    msgbuf_t m, rep;

    /* Oversize: TotalLength above 256. */
    memset(&m, 0, sizeof(m));
    m.hdr.TotalLength = 300;
    m.hdr.DataLength = 300 - sizeof(PORT_MESSAGE);
    m.hdr.Type = LPC_REQUEST;
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(cli, &m.hdr, &rep.hdr), STATUS_PORT_MESSAGE_TOO_LONG);

    /* DataLength that does not match TotalLength. */
    msg_set(&m.hdr, LPC_REQUEST, "abcd");
    m.hdr.DataLength = 3;
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(cli, &m.hdr, &rep.hdr), STATUS_INVALID_PARAMETER);

    /* Nonzero DataInfoOffset: no data info (views, handles) in v1. */
    msg_set(&m.hdr, LPC_REQUEST, "abcd");
    m.hdr.DataInfoOffset = 8;
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(cli, &m.hdr, &rep.hdr), STATUS_INVALID_PARAMETER);

    /* Wrong type for a request. */
    msg_set(&m.hdr, LPC_REPLY, "abcd");
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(cli, &m.hdr, &rep.hdr), STATUS_INVALID_PARAMETER);

    /* TotalLength shorter than the header. */
    memset(&m, 0, sizeof(m));
    m.hdr.TotalLength = 8;
    m.hdr.Type = LPC_REQUEST;
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(cli, &m.hdr, &rep.hdr), STATUS_INVALID_PARAMETER);

    /* Oversize reply from the server is rejected before the request is looked up. */
    memset(&m, 0, sizeof(m));
    m.hdr.TotalLength = 300;
    m.hdr.DataLength = 300 - sizeof(PORT_MESSAGE);
    m.hdr.Type = LPC_REPLY;
    ASSERT_STATUS(nt_svc_NtReplyPort(srv, &m.hdr), STATUS_PORT_MESSAGE_TOO_LONG);

    /* A connection-port handle used as a client handle is the wrong kind. */
    msg_set(&m.hdr, LPC_REQUEST, "x");
    ASSERT_STATUS(nt_svc_NtRequestWaitReplyPort(g_conn, &m.hdr, &rep.hdr), STATUS_INVALID_PORT_HANDLE);

    ASSERT_STATUS(ObCloseHandle(g_proc, cli), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_proc, srv), STATUS_SUCCESS);
    PASS();
}

TEST(max_message_is_capped)
{
    /* g_conn was created with 4096; the port's limit is capped at 256. */
    HANDLE srv = NULL;
    struct worker *c = &pool[5];
    ASSERT_STATUS(open_pair(&srv, c), STATUS_SUCCESS);
    ASSERT(c->maxlen == PORT_MAXIMUM_MESSAGE_LENGTH);
    ASSERT_STATUS(ObCloseHandle(g_proc, c->out_h), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_proc, srv), STATUS_SUCCESS);
    PASS();
}

TEST(double_and_unknown_reply)
{
    HANDLE srv = NULL;
    struct worker *c = &pool[6];
    ASSERT_STATUS(open_pair(&srv, c), STATUS_SUCCESS);
    struct worker *r = &pool[7];
    r->in_h = c->out_h;
    worker_start(r, fn_request_once);

    msgbuf_t recv, rep;
    ASSERT_STATUS(nt_svc_NtReplyWaitReceivePort(srv, NULL, NULL, &recv.hdr), STATUS_SUCCESS);
    memcpy(&rep, &recv, recv.hdr.TotalLength);
    rep.hdr.Type = LPC_REPLY;
    ASSERT_STATUS(nt_svc_NtReplyPort(srv, &rep.hdr), STATUS_SUCCESS);
    /* The same MessageId again: the request is no longer pending. */
    ASSERT_STATUS(nt_svc_NtReplyPort(srv, &rep.hdr), STATUS_INVALID_PARAMETER);
    /* An id that was never received. */
    rep.hdr.MessageId = 0x7fff0000u;
    ASSERT_STATUS(nt_svc_NtReplyPort(srv, &rep.hdr), STATUS_INVALID_PARAMETER);

    worker_join(r);
    ASSERT_STATUS(r->status, STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_proc, c->out_h), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_proc, srv), STATUS_SUCCESS);
    PASS();
}

TEST(refused_connection)
{
    struct worker *c = &pool[8];
    set_name(c, MAIN_PORT_NAME);
    worker_start(c, fn_connect);
    msgbuf_t req;
    ASSERT_STATUS(nt_svc_NtListenPort(g_conn, &req.hdr), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtAcceptConnectPort(NULL, g_conn, &req.hdr, false, NULL, NULL), STATUS_SUCCESS);
    worker_join(c);
    ASSERT_STATUS(c->status, STATUS_PORT_CONNECTION_REFUSED);
    /* A second refusal of the same id finds nothing listened. */
    ASSERT_STATUS(nt_svc_NtAcceptConnectPort(NULL, g_conn, &req.hdr, false, NULL, NULL), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(connection_queue_cap)
{
    for (int i = 0; i < PORT_MAX_CONNECTION_QUEUE; i++) {
        set_name(&pool[9 + i], MAIN_PORT_NAME);
        worker_start(&pool[9 + i], fn_connect);
    }
    /* Give the sixteen connects time to queue. They then block until refused. */
    usleep(300000);
    struct worker *extra = &pool[9 + PORT_MAX_CONNECTION_QUEUE];
    set_name(extra, MAIN_PORT_NAME);
    worker_start(extra, fn_connect);
    worker_join(extra);
    ASSERT_STATUS(extra->status, STATUS_INSUFFICIENT_RESOURCES);

    for (int i = 0; i < PORT_MAX_CONNECTION_QUEUE; i++) {
        msgbuf_t req;
        ASSERT_STATUS(nt_svc_NtListenPort(g_conn, &req.hdr), STATUS_SUCCESS);
        ASSERT_STATUS(nt_svc_NtAcceptConnectPort(NULL, g_conn, &req.hdr, false, NULL, NULL), STATUS_SUCCESS);
    }
    for (int i = 0; i < PORT_MAX_CONNECTION_QUEUE; i++) {
        worker_join(&pool[9 + i]);
        ASSERT_STATUS(pool[9 + i].status, STATUS_PORT_CONNECTION_REFUSED);
    }
    PASS();
}

TEST(connection_port_close_refuses_queued)
{
    /* A connect that is queued when the connection port closes is refused.
     * Uses its own port, so it is not part of the shared baseline. */
    struct worker *c = &pool[9];
    set_name(c, "\\LpcClose");
    worker_start(c, fn_connect);
    usleep(100000);
    ASSERT_STATUS(ObCloseHandle(g_proc, g_close), STATUS_SUCCESS);
    worker_join(c);
    ASSERT_STATUS(c->status, STATUS_PORT_CONNECTION_REFUSED);
    PASS();
}

TEST(connect_unknown_port)
{
    struct worker *c = &pool[0];
    set_name(c, "\\LpcNoSuchPort");
    ASSERT_STATUS(fn_connect(c), STATUS_OBJECT_NAME_NOT_FOUND);
    PASS();
}

/* Leak check: every handle and object created by the tests must be released.
 * The baseline includes the one named connection port and its directory entry. */
static long g_alloc_base;

int main(void)
{
    printf("port tests starting\n");
    ObInitObjectManager();
    pool_init();

    /* The shared connection port and the process handle table exist before the baseline. */
    if (create_port(MAIN_PORT_NAME, 4096, &g_conn) != STATUS_SUCCESS) return 1;
    if (create_port("\\LpcClose", PORT_MAXIMUM_MESSAGE_LENGTH, &g_close) != STATUS_SUCCESS) return 1;
    g_alloc_base = (long)atomic_load(&kshim_alloc_count);

    RUN(round_trip);
    RUN(timeouts_and_disconnect);
    RUN(alertable_listen);
    RUN(message_validation);
    RUN(max_message_is_capped);
    RUN(double_and_unknown_reply);
    RUN(refused_connection);
    RUN(connection_queue_cap);
    RUN(connection_port_close_refuses_queued);
    RUN(connect_unknown_port);

    ObCloseHandle(g_proc, g_conn);
    long end = (long)atomic_load(&kshim_alloc_count);
    if (end != g_alloc_base) {
        fprintf(stderr, "FAIL leak: allocations base=%ld end=%ld\n", g_alloc_base, end);
        kshim_leak_report();
        g_fail++;
    }
    printf("port tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    ObCleanupObjectManager();
    return 0;
}
