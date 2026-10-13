/* SPDX-License-Identifier: GPL-2.0 */
/*
 * port.c - LPC ports (NtCreatePort family). Scope and limits: see port.h and
 * docs/decisions/0072-kernel-nt-lpc-ports.md.
 *
 * Locking: one spinlock (port_lock) guards every port, connection request and
 * message. It is never held across a wait, a handle-table call, or an object
 * dereference that can free an object. Waiters block on the port's dispatcher
 * events (queue_ev auto-reset, disc_ev manual-reset) through the normal wait
 * engine, and read their result under port_lock after the wait returns.
 *
 * Lifetimes:
 *  - A connection request (struct nt_port_conn) is owned by the connecting
 *    thread, which frees it after the decision is final. It holds references to
 *    the client port and, once accepted, to the server port.
 *  - A message (struct nt_port_msg) is owned by the requesting thread, which
 *    frees it after the reply or cancellation. The server only links it.
 *  - A connected pair holds a reference on its peer. Closing a handle breaks
 *    the pair (port_shutdown_comm), so the references do not outlive the link.
 */

#include "port.h"
#include "handle.h"
#include "sync.h"

#ifndef __KERNEL__
#include <pthread.h>
#include <string.h>
#endif

#define PORT_CURRENT_PROCESS() kshim_current_process()

enum port_kind {
    PORT_KIND_CONNECTION = 1,
    PORT_KIND_CLIENT,
    PORT_KIND_SERVER,
};

enum port_msg_state {
    PORT_MSG_PENDING = 1,
    PORT_MSG_REPLIED,
    PORT_MSG_CANCELLED,
};

enum port_conn_state {
    PORT_CONN_QUEUED = 1,
    PORT_CONN_LISTENED,
    PORT_CONN_ACCEPTED,
    PORT_CONN_COMPLETED,
    PORT_CONN_REFUSED,
};

/* A whole message, aligned for PORT_MESSAGE. */
union port_msg_buf {
    PORT_MESSAGE hdr;
    UCHAR        raw[PORT_MAXIMUM_MESSAGE_LENGTH];
};

struct nt_port_msg {
    struct list_head    link;
    bool                linked;
    enum port_msg_state state;
    ULONG               message_id;
    struct nt_event    *reply_ev;   /* manual reset; set once, when state leaves PENDING */
    ULONG               request_len;
    ULONG               reply_len;
    union port_msg_buf  request;
    union port_msg_buf  reply;
};

struct nt_port_conn {
    struct list_head    link;
    bool                linked;
    enum port_conn_state state;
    ULONG               message_id;
    struct nt_event    *decision;   /* manual reset; set once, on COMPLETED or REFUSED */
    struct nt_object_header *client_hdr; /* owned ref */
    struct nt_object_header *server_hdr; /* owned ref once ACCEPTED */
    ULONG               message_len;
    union port_msg_buf  message;     /* header plus connection info */
};

/* Object body of a Port. */
struct nt_port {
    enum port_kind      kind;
    bool                disconnected;
    ULONG               max_message_length;
    ULONG               max_connection_info;
    struct nt_port     *peer;        /* client <-> server comm port; owned ref when set */
    struct nt_port_conn *conn;       /* server comm: accepted request awaiting Complete */
    struct nt_event    *queue_ev;    /* auto reset: work is queued */
    struct nt_event    *disc_ev;     /* manual reset: port disconnected */
    struct list_head    conn_queue;    /* connection port: QUEUED requests */
    struct list_head    conn_listened; /* connection port: LISTENED and ACCEPTED requests */
    ULONG               queued;      /* length of conn_queue, capped at PORT_MAX_CONNECTION_QUEUE */
    struct list_head    inbox;       /* server comm: requests not yet received */
    struct list_head    pending;     /* server comm: received, awaiting reply */
};

static void port_obj_delete(struct nt_object_header *hdr);
static void port_obj_close(struct nt_object_header *hdr, HANDLE handle);

struct nt_object_type nt_port_type = {
    .name = "Port",
    .ops = { .delete_obj = port_obj_delete, .close = port_obj_close, },
    .pool_tag = 0,
};

#define PORT_BODY(hdr) ((struct nt_port *)nt_object_body(hdr))

#ifndef __KERNEL__
static spinlock_t port_lock;
static pthread_once_t port_once = PTHREAD_ONCE_INIT;
static void port_init_once(void) { spin_lock_init(&port_lock); }
static void port_ensure_init(void) { pthread_once(&port_once, port_init_once); }
#else
static DEFINE_SPINLOCK(port_lock);
static void port_ensure_init(void) { }
#endif
static ULONG port_next_message_id; /* guarded by port_lock; 0 is never issued */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static NTSTATUS port_check_header(const PORT_MESSAGE *h, ULONG expect_type, ULONG max_len)
{
    if (h->TotalLength < sizeof(PORT_MESSAGE)) return STATUS_INVALID_PARAMETER;
    if (h->TotalLength > max_len) return STATUS_PORT_MESSAGE_TOO_LONG;
    if (h->DataLength != h->TotalLength - sizeof(PORT_MESSAGE)) return STATUS_INVALID_PARAMETER;
    if (h->DataInfoOffset != 0 || h->ClientViewSize != 0) return STATUS_INVALID_PARAMETER;
    if (h->Type != expect_type) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

/* Copies a user message into kernel memory after validating its header. The
 * header is read first, so the copy length (TotalLength) is bounded by max_len. */
static NTSTATUS port_copy_in(PPORT_MESSAGE user, ULONG expect_type, ULONG max_len,
                             union port_msg_buf *dst, ULONG *out_len)
{
    PORT_MESSAGE hdr;
    if (!user) return STATUS_INVALID_PARAMETER;
    if (copy_from_user(&hdr, user, sizeof(hdr))) return STATUS_ACCESS_VIOLATION;
    NTSTATUS st = port_check_header(&hdr, expect_type, max_len);
    if (!NT_SUCCESS(st)) return st;
    if (copy_from_user(dst->raw, user, hdr.TotalLength)) return STATUS_ACCESS_VIOLATION;
    *out_len = hdr.TotalLength;
    return STATUS_SUCCESS;
}

static void port_fill_client_id(CLIENT_ID *id)
{
    struct nt_eprocess *proc = PORT_CURRENT_PROCESS();
    struct nt_ethread  *thr  = kshim_current_thread();
    id->UniqueProcess = (HANDLE)(ULONG_PTR)(proc ? proc->pid : 0);
    id->UniqueThread  = (HANDLE)(ULONG_PTR)(thr ? thr->tid : 0);
}

/* Copies a user UNICODE_STRING and its characters into kernel buffer buf. */
static NTSTATUS port_copy_name(PUNICODE_STRING src, UNICODE_STRING *dst, WCHAR *buf, size_t buf_bytes)
{
    UNICODE_STRING tmp;
    if (!src) return STATUS_INVALID_PARAMETER;
    if (copy_from_user(&tmp, src, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    if (tmp.Length == 0) return STATUS_INVALID_PARAMETER;
    if ((size_t)tmp.Length > buf_bytes) return STATUS_NAME_TOO_LONG;
    if (copy_from_user(buf, tmp.Buffer, tmp.Length)) return STATUS_ACCESS_VIOLATION;
    dst->Buffer = buf;
    dst->Length = tmp.Length;
    dst->MaximumLength = tmp.Length;
    return STATUS_SUCCESS;
}

static NTSTATUS port_insert_handle(struct nt_eprocess *proc, struct nt_object_header *hdr,
                                   POBJECT_ATTRIBUTES oa, PHANDLE out)
{
    NTSTATUS st;
    if (!proc) return STATUS_INVALID_PARAMETER;
    if (!proc->handle_table) {
        st = ObCreateHandleTable(proc, &proc->handle_table);
        if (!NT_SUCCESS(st)) return st;
    }
    st = ObInsertObject(hdr, oa, PORT_ALL_ACCESS, proc, out);
    if (st == STATUS_OBJECT_NAME_COLLISION)
        return ObOpenNamedHolderAfterCollision(hdr->type, oa, PORT_ALL_ACCESS, proc, out);
    return st;
}

/* Creates a port object and its two events. The caller owns the creation reference. */
static NTSTATUS port_create_object(enum port_kind kind, ULONG max_len, ULONG max_info,
                                   POBJECT_ATTRIBUTES oa, struct nt_object_header **out_hdr)
{
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    OBJECT_ATTRIBUTES tmp;
    port_ensure_init();
    memset(&tmp, 0, sizeof(tmp));
    tmp.Length = sizeof(tmp);
    if (oa) tmp = *oa;
    NTSTATUS st = ObCreateObject(&nt_port_type, &tmp, sizeof(struct nt_port), &hdr, &body);
    if (!NT_SUCCESS(st)) return st;
    struct nt_port *p = (struct nt_port *)body;
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->max_message_length = max_len;
    p->max_connection_info = max_info;
    INIT_LIST_HEAD(&p->conn_queue);
    INIT_LIST_HEAD(&p->conn_listened);
    INIT_LIST_HEAD(&p->inbox);
    INIT_LIST_HEAD(&p->pending);
    st = nt_create_event(&p->queue_ev, SynchronizationEvent, false);
    if (NT_SUCCESS(st)) st = nt_create_event(&p->disc_ev, NotificationEvent, false);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(hdr); /* delete_obj releases whichever event exists */
        return st;
    }
    *out_hdr = hdr;
    return STATUS_SUCCESS;
}

static void port_obj_delete(struct nt_object_header *hdr)
{
    struct nt_port *p = PORT_BODY(hdr);
    if (p->queue_ev) nt_put_event(p->queue_ev);
    if (p->disc_ev) nt_put_event(p->disc_ev);
    p->queue_ev = NULL;
    p->disc_ev = NULL;
    /* A live peer reference exists only if the port was never closed. Drop it;
     * no port lock is held in delete. */
    if (p->peer) {
        struct nt_object_header *peer_hdr = nt_body_to_header(p->peer);
        p->peer = NULL;
        ObDereferenceObject(peer_hdr);
    }
}

static NTSTATUS port_wait_on(struct nt_port *p, BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    struct nt_dispatcher_header *objs[2];
    objs[0] = &p->queue_ev->header;
    objs[1] = &p->disc_ev->header;
    return nt_wait_for_multiple_objects(2, objs, WaitAny, alertable, timeout);
}

/* Cancels every message on list. Caller holds port_lock. */
static void port_cancel_msgs(struct list_head *list)
{
    while (!list_empty(list)) {
        struct nt_port_msg *m = list_first_entry(list, struct nt_port_msg, link);
        list_del(&m->link);
        m->linked = false;
        m->state = PORT_MSG_CANCELLED;
        nt_set_event(m->reply_ev, NULL);
    }
}

/* Ends a connection request that has not completed. Caller holds port_lock.
 * The connecting thread drops its references when it wakes. */
static void port_refuse_conn(struct nt_port_conn *c)
{
    if (c->linked) {
        list_del(&c->link);
        c->linked = false;
    }
    c->state = PORT_CONN_REFUSED;
    if (c->server_hdr) PORT_BODY(c->server_hdr)->conn = NULL;
    nt_set_event(c->decision, NULL);
}

/* Shutdown of a connection port: refuse every request and wake the listeners. */
static void port_shutdown_connection(struct nt_port *p)
{
    spin_lock(&port_lock);
    if (p->disconnected) { spin_unlock(&port_lock); return; }
    p->disconnected = true;
    nt_set_event(p->disc_ev, NULL);
    while (!list_empty(&p->conn_queue)) {
        struct nt_port_conn *c = list_first_entry(&p->conn_queue, struct nt_port_conn, link);
        p->queued--;
        port_refuse_conn(c);
    }
    while (!list_empty(&p->conn_listened)) {
        struct nt_port_conn *c = list_first_entry(&p->conn_listened, struct nt_port_conn, link);
        port_refuse_conn(c);
    }
    spin_unlock(&port_lock);
}

/* Shutdown of a communication port and its peer. The message lists live on
 * the server side, so both ends are cancelled. The peer references are dropped
 * after the lock is released. */
static void port_shutdown_comm(struct nt_port *p)
{
    struct nt_object_header *peer_hdr = NULL;
    struct nt_object_header *back_hdr = NULL;
    spin_lock(&port_lock);
    if (p->disconnected) { spin_unlock(&port_lock); return; }
    p->disconnected = true;
    nt_set_event(p->disc_ev, NULL);
    port_cancel_msgs(&p->inbox);
    port_cancel_msgs(&p->pending);
    if (p->conn) {
        port_refuse_conn(p->conn);
        p->conn = NULL;
    }
    if (p->peer) {
        struct nt_port *other = p->peer;
        p->peer = NULL;
        peer_hdr = nt_body_to_header(other);
        other->disconnected = true;
        nt_set_event(other->disc_ev, NULL);
        port_cancel_msgs(&other->inbox);
        port_cancel_msgs(&other->pending);
        if (other->conn) {
            port_refuse_conn(other->conn);
            other->conn = NULL;
        }
        if (other->peer) {
            back_hdr = nt_body_to_header(other->peer);
            other->peer = NULL;
        }
    }
    spin_unlock(&port_lock);
    if (peer_hdr) ObDereferenceObject(peer_hdr);
    if (back_hdr) ObDereferenceObject(back_hdr);
}

/* Called by the object manager on each handle close, after the handle table
 * lock is released and before the object's reference is dropped. */
static void port_obj_close(struct nt_object_header *hdr, HANDLE handle)
{
    (void)handle;
    struct nt_port *p = PORT_BODY(hdr);
    if (p->kind == PORT_KIND_CONNECTION) port_shutdown_connection(p);
    else port_shutdown_comm(p);
}

/* Looks a handle up with the port type and the required kind. On success the
 * caller holds a reference. */
static NTSTATUS port_ref_handle(HANDLE h, enum port_kind kind, struct nt_object_header **out)
{
    struct nt_eprocess *proc = PORT_CURRENT_PROCESS();
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    port_ensure_init();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(proc, h, &nt_port_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    if (PORT_BODY(hdr)->kind != kind) {
        ObDereferenceObject(hdr);
        return STATUS_INVALID_PORT_HANDLE;
    }
    *out = hdr;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Connection requests                                                 */
/* ------------------------------------------------------------------ */

/* Waits until a connection request is queued, moves it to LISTENED and copies
 * its message (header plus connection info) to out. The caller holds a
 * reference on p. */
static NTSTATUS port_listen_core(struct nt_port *p, BOOLEAN alertable, PLARGE_INTEGER timeout,
                                 UCHAR *out, ULONG *out_len)
{
    for (;;) {
        NTSTATUS st;
        spin_lock(&port_lock);
        if (p->disconnected) { spin_unlock(&port_lock); return STATUS_PORT_DISCONNECTED; }
        if (!list_empty(&p->conn_queue)) {
            struct nt_port_conn *c = list_first_entry(&p->conn_queue, struct nt_port_conn, link);
            list_del(&c->link);
            p->queued--;
            c->state = PORT_CONN_LISTENED;
            list_add_tail(&c->link, &p->conn_listened);
            c->linked = true;
            memcpy(out, c->message.raw, c->message_len);
            *out_len = c->message_len;
            if (p->queued > 0) nt_set_event(p->queue_ev, NULL);
            spin_unlock(&port_lock);
            return STATUS_SUCCESS;
        }
        spin_unlock(&port_lock);
        st = port_wait_on(p, alertable, timeout);
        if (st == STATUS_TIMEOUT || st == STATUS_USER_APC || st == STATUS_ALERTED || !NT_SUCCESS(st))
            return st;
        /* Woken: loop and re-check under the lock. */
    }
}

/* Finds a LISTENED request by MessageId. Caller holds port_lock. */
static struct nt_port_conn *port_find_listened(struct nt_port *p, ULONG id, bool accepted_ok)
{
    struct list_head *pos;
    list_for_each(pos, &p->conn_listened) {
        struct nt_port_conn *c = list_entry(pos, struct nt_port_conn, link);
        if (c->message_id == id && c->state == PORT_CONN_LISTENED) return c;
        if (accepted_ok && c->message_id == id && c->state == PORT_CONN_ACCEPTED) return c;
    }
    return NULL;
}

NTSTATUS nt_svc_NtCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes,
                             ULONG MaxConnectionInfoLength, ULONG MaxMessageLength,
                             ULONG MaxPoolUsage)
{
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[128];
    POBJECT_ATTRIBUTES oa = NULL;
    struct nt_object_header *hdr = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    struct nt_eprocess *proc = PORT_CURRENT_PROCESS();
    ULONG max_len;

    (void)MaxPoolUsage;
    if (!PortHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(PortHandle, sizeof(*PortHandle))) return STATUS_ACCESS_VIOLATION;
    if (MaxMessageLength < sizeof(PORT_MESSAGE)) return STATUS_INVALID_PARAMETER;
    if (MaxConnectionInfoLength > PORT_MESSAGE_DATA_MAX) return STATUS_INVALID_PARAMETER;
    if (!proc) return STATUS_INVALID_PARAMETER;

    if (ObjectAttributes) {
        if (copy_from_user(&k_oa, ObjectAttributes, sizeof(k_oa))) return STATUS_ACCESS_VIOLATION;
        if (k_oa.Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
        if (k_oa.ObjectName) {
            st = port_copy_name(k_oa.ObjectName, &k_name, k_buf, sizeof(k_buf));
            if (!NT_SUCCESS(st)) return st;
            k_oa.ObjectName = &k_name;
        }
        oa = &k_oa;
    }

    max_len = MaxMessageLength > PORT_MAXIMUM_MESSAGE_LENGTH ? PORT_MAXIMUM_MESSAGE_LENGTH : MaxMessageLength;
    st = port_create_object(PORT_KIND_CONNECTION, max_len, MaxConnectionInfoLength, oa, &hdr);
    if (!NT_SUCCESS(st)) return st;
    st = port_insert_handle(proc, hdr, oa, &h);
    ObDereferenceObject(hdr); /* creation reference; the handle keeps the object */
    if (st == STATUS_OBJECT_NAME_COLLISION) { *PortHandle = h; return st; }
    if (!NT_SUCCESS(st)) return st;
    *PortHandle = h;
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtConnectPort(PHANDLE ClientPortHandle, PUNICODE_STRING PortName,
                              PVOID SecurityQos, PVOID ClientView, PVOID ServerView,
                              PULONG MaxMessageLength, PVOID ConnectionInfo,
                              PULONG ConnectionInfoLength)
{
    UNICODE_STRING k_name;
    WCHAR k_buf[128];
    struct nt_object_header *conn_hdr = NULL;
    struct nt_object_header *client_hdr = NULL;
    struct nt_port *conn;
    struct nt_port_conn *c;
    PORT_MESSAGE *hdr;
    struct nt_eprocess *proc = PORT_CURRENT_PROCESS();
    ULONG info_len = 0;
    ULONG maxlen;
    enum port_conn_state final;
    struct nt_object_header *server_hdr;
    HANDLE h = NULL;
    NTSTATUS st;
    bool disconnected;

    (void)SecurityQos;
    if (!ClientPortHandle || !PortName) return STATUS_INVALID_PARAMETER;
    if (!access_ok(ClientPortHandle, sizeof(*ClientPortHandle))) return STATUS_ACCESS_VIOLATION;
    /* Shared views are not supported in v1. */
    if (ClientView || ServerView) return STATUS_INVALID_PARAMETER;
    if (!proc) return STATUS_INVALID_PARAMETER;
    if (ConnectionInfo || ConnectionInfoLength) {
        if (!ConnectionInfo || !ConnectionInfoLength) return STATUS_INVALID_PARAMETER;
        if (copy_from_user(&info_len, ConnectionInfoLength, sizeof(info_len))) return STATUS_ACCESS_VIOLATION;
    }

    st = port_copy_name(PortName, &k_name, k_buf, sizeof(k_buf));
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByName(&k_name, &nt_port_type, UserMode, 0, &conn_hdr);
    if (!NT_SUCCESS(st)) return st;
    conn = PORT_BODY(conn_hdr);
    if (conn->kind != PORT_KIND_CONNECTION) {
        ObDereferenceObject(conn_hdr);
        return STATUS_INVALID_PORT_HANDLE;
    }
    if (info_len > conn->max_connection_info) {
        ObDereferenceObject(conn_hdr);
        return STATUS_PORT_MESSAGE_TOO_LONG;
    }

    st = port_create_object(PORT_KIND_CLIENT, conn->max_message_length, 0, NULL, &client_hdr);
    if (!NT_SUCCESS(st)) { ObDereferenceObject(conn_hdr); return st; }

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        ObDereferenceObject(client_hdr);
        ObDereferenceObject(conn_hdr);
        return STATUS_NO_MEMORY;
    }
    st = nt_create_event(&c->decision, NotificationEvent, false);
    if (!NT_SUCCESS(st)) {
        kfree(c);
        ObDereferenceObject(client_hdr);
        ObDereferenceObject(conn_hdr);
        return st;
    }
    INIT_LIST_HEAD(&c->link);
    c->state = PORT_CONN_QUEUED;
    c->client_hdr = client_hdr;
    hdr = &c->message.hdr;
    hdr->DataLength = (USHORT)info_len;
    hdr->TotalLength = (USHORT)(sizeof(PORT_MESSAGE) + info_len);
    hdr->Type = LPC_CONNECTION_REQUEST;
    hdr->DataInfoOffset = 0;
    hdr->ClientViewSize = 0;
    port_fill_client_id(&hdr->ClientId);
    c->message_len = hdr->TotalLength;
    if (info_len && copy_from_user(c->message.raw + sizeof(PORT_MESSAGE), ConnectionInfo, info_len)) {
        st = STATUS_ACCESS_VIOLATION;
        goto connect_drop;
    }

    spin_lock(&port_lock);
    disconnected = conn->disconnected;
    if (disconnected || conn->queued >= PORT_MAX_CONNECTION_QUEUE) {
        spin_unlock(&port_lock);
        st = disconnected ? STATUS_PORT_CONNECTION_REFUSED : STATUS_INSUFFICIENT_RESOURCES;
        goto connect_drop;
    }
    c->message_id = ++port_next_message_id;
    hdr->MessageId = c->message_id;
    list_add_tail(&c->link, &conn->conn_queue);
    c->linked = true;
    conn->queued++;
    nt_set_event(conn->queue_ev, NULL);
    spin_unlock(&port_lock);

    /* Blocks until the server completes or refuses the connection. */
    nt_wait_for_single_object(&c->decision->header, false, NULL);

    spin_lock(&port_lock);
    final = c->state;
    maxlen = conn->max_message_length;
    server_hdr = c->server_hdr;
    c->server_hdr = NULL;
    spin_unlock(&port_lock);

    st = STATUS_PORT_CONNECTION_REFUSED;
    if (final == PORT_CONN_COMPLETED) {
        st = port_insert_handle(proc, client_hdr, NULL, &h);
        if (NT_SUCCESS(st)) {
            *ClientPortHandle = h;
            if (MaxMessageLength && copy_to_user(MaxMessageLength, &maxlen, sizeof(maxlen)))
                st = STATUS_ACCESS_VIOLATION;
        }
    }
    if (server_hdr) ObDereferenceObject(server_hdr);
    ObDereferenceObject(client_hdr); /* the handle, if any, holds its own reference */
    ObDereferenceObject(conn_hdr);
    nt_put_event(c->decision);
    kfree(c);
    return st;

connect_drop:
    nt_put_event(c->decision);
    kfree(c);
    ObDereferenceObject(client_hdr);
    ObDereferenceObject(conn_hdr);
    return st;
}

NTSTATUS port_listen_wait(HANDLE ConnectionPortHandle, BOOLEAN Alertable,
                          PLARGE_INTEGER Timeout, PPORT_MESSAGE Message)
{
    struct nt_object_header *hdr = NULL;
    union port_msg_buf buf;
    ULONG len = 0;
    NTSTATUS st;
    if (!Message) return STATUS_INVALID_PARAMETER;
    st = port_ref_handle(ConnectionPortHandle, PORT_KIND_CONNECTION, &hdr);
    if (!NT_SUCCESS(st)) return st;
    st = port_listen_core(PORT_BODY(hdr), Alertable, Timeout, buf.raw, &len);
    if (st == STATUS_SUCCESS && copy_to_user(Message, buf.raw, len)) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtListenPort(HANDLE ConnectionPortHandle, PPORT_MESSAGE ConnectionRequest)
{
    return port_listen_wait(ConnectionPortHandle, false, NULL, ConnectionRequest);
}

NTSTATUS nt_svc_NtAcceptConnectPort(PHANDLE ServerPortHandle, HANDLE ConnectionPortHandle,
                                    PPORT_MESSAGE ConnectionRequest, BOOLEAN AcceptConnection,
                                    PVOID ServerView, PVOID ClientView)
{
    PORT_MESSAGE hdr;
    struct nt_object_header *conn_hdr = NULL;
    struct nt_object_header *srv_hdr = NULL;
    struct nt_port *conn;
    struct nt_port_conn *c;
    HANDLE h = NULL;
    ULONG id;
    NTSTATUS st;

    if (!ConnectionRequest) return STATUS_INVALID_PARAMETER;
    if (ServerView || ClientView) return STATUS_INVALID_PARAMETER;
    if (AcceptConnection && !ServerPortHandle) return STATUS_INVALID_PARAMETER;
    if (!PORT_CURRENT_PROCESS()) return STATUS_INVALID_PARAMETER;

    /* Only the header is needed: the request is identified by MessageId. */
    if (copy_from_user(&hdr, ConnectionRequest, sizeof(hdr))) return STATUS_ACCESS_VIOLATION;
    st = port_check_header(&hdr, LPC_CONNECTION_REQUEST, PORT_MAXIMUM_MESSAGE_LENGTH);
    if (!NT_SUCCESS(st)) return st;
    id = hdr.MessageId;

    st = port_ref_handle(ConnectionPortHandle, PORT_KIND_CONNECTION, &conn_hdr);
    if (!NT_SUCCESS(st)) return st;
    conn = PORT_BODY(conn_hdr);

    if (!AcceptConnection) {
        spin_lock(&port_lock);
        c = port_find_listened(conn, id, false);
        if (c) port_refuse_conn(c);
        spin_unlock(&port_lock);
        ObDereferenceObject(conn_hdr);
        return c ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }

    /* Create and insert the server port first. The handle then exists before
     * the request is marked ACCEPTED, so no failure path needs to undo the
     * request state. */
    st = port_create_object(PORT_KIND_SERVER, conn->max_message_length, 0, NULL, &srv_hdr);
    if (!NT_SUCCESS(st)) { ObDereferenceObject(conn_hdr); return st; }
    st = port_insert_handle(PORT_CURRENT_PROCESS(), srv_hdr, NULL, &h);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(srv_hdr);
        ObDereferenceObject(conn_hdr);
        return st;
    }

    spin_lock(&port_lock);
    c = port_find_listened(conn, id, false);
    if (c) {
        c->state = PORT_CONN_ACCEPTED;
        c->server_hdr = srv_hdr;
        ObReferenceObject(srv_hdr);          /* owned by the request */
        PORT_BODY(srv_hdr)->conn = c;
    }
    spin_unlock(&port_lock);

    ObDereferenceObject(srv_hdr); /* creation reference; the handle keeps the object */
    if (!c) {
        /* Not listened on, or already refused: close the new handle. */
        ObCloseHandle(PORT_CURRENT_PROCESS(), h);
        ObDereferenceObject(conn_hdr);
        return STATUS_INVALID_PARAMETER;
    }
    ObDereferenceObject(conn_hdr);
    *ServerPortHandle = h;
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtCompleteConnectPort(HANDLE ServerPortHandle)
{
    struct nt_object_header *srv_hdr = NULL;
    struct nt_port *srv;
    struct nt_port_conn *c;
    NTSTATUS st = port_ref_handle(ServerPortHandle, PORT_KIND_SERVER, &srv_hdr);
    if (!NT_SUCCESS(st)) return st;
    srv = PORT_BODY(srv_hdr);

    spin_lock(&port_lock);
    c = srv->conn;
    if (srv->disconnected) {
        st = STATUS_PORT_DISCONNECTED;
    } else if (!c || c->state != PORT_CONN_ACCEPTED) {
        st = STATUS_INVALID_PARAMETER;
    } else {
        struct nt_port *client = PORT_BODY(c->client_hdr);
        /* Each side holds a reference on the other while they stay linked. */
        ObReferenceObject(c->client_hdr);
        ObReferenceObject(srv_hdr);
        srv->peer = client;
        client->peer = srv;
        srv->conn = NULL;
        list_del(&c->link);
        c->linked = false;
        c->state = PORT_CONN_COMPLETED;
        nt_set_event(c->decision, NULL);
        st = STATUS_SUCCESS;
    }
    spin_unlock(&port_lock);
    ObDereferenceObject(srv_hdr);
    return st;
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

/* Client side: queues a request, then blocks until it is replied to or cancelled. */
static NTSTATUS port_request_core(struct nt_port *p, union port_msg_buf *req, ULONG len,
                                  union port_msg_buf *rep, ULONG *rep_len)
{
    struct nt_port_msg *m;
    struct nt_port *srv;
    NTSTATUS st;

    m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return STATUS_NO_MEMORY;
    st = nt_create_event(&m->reply_ev, NotificationEvent, false);
    if (!NT_SUCCESS(st)) { kfree(m); return st; }
    INIT_LIST_HEAD(&m->link);
    m->request_len = len;
    memcpy(m->request.raw, req->raw, len);

    spin_lock(&port_lock);
    if (p->disconnected || !p->peer) {
        spin_unlock(&port_lock);
        nt_put_event(m->reply_ev);
        kfree(m);
        return STATUS_PORT_DISCONNECTED;
    }
    srv = p->peer;
    m->message_id = ++port_next_message_id;
    m->request.hdr.MessageId = m->message_id;
    port_fill_client_id(&m->request.hdr.ClientId);
    m->state = PORT_MSG_PENDING;
    list_add_tail(&m->link, &srv->inbox);
    m->linked = true;
    nt_set_event(srv->queue_ev, NULL);
    spin_unlock(&port_lock);

    /* The state changes under port_lock and the event is set in the same
     * critical section, so the loop ends after at most one wait. */
    for (;;) {
        nt_wait_for_single_object(&m->reply_ev->header, false, NULL);
        spin_lock(&port_lock);
        if (m->state != PORT_MSG_PENDING) break;
        spin_unlock(&port_lock);
    }
    st = STATUS_PORT_DISCONNECTED;
    if (m->state == PORT_MSG_REPLIED) {
        memcpy(rep->raw, m->reply.raw, m->reply_len);
        *rep_len = m->reply_len;
        st = STATUS_SUCCESS;
    }
    spin_unlock(&port_lock);
    nt_put_event(m->reply_ev);
    kfree(m);
    return st;
}

NTSTATUS nt_svc_NtRequestWaitReplyPort(HANDLE PortHandle, PPORT_MESSAGE RequestMessage,
                                       PPORT_MESSAGE ReplyMessage)
{
    struct nt_object_header *hdr = NULL;
    struct nt_port *p;
    union port_msg_buf req, rep;
    ULONG req_len = 0, rep_len = 0;
    NTSTATUS st = port_ref_handle(PortHandle, PORT_KIND_CLIENT, &hdr);
    if (!NT_SUCCESS(st)) return st;
    p = PORT_BODY(hdr);
    st = port_copy_in(RequestMessage, LPC_REQUEST, p->max_message_length, &req, &req_len);
    if (NT_SUCCESS(st)) {
        st = port_request_core(p, &req, req_len, &rep, &rep_len);
        if (st == STATUS_SUCCESS && ReplyMessage && copy_to_user(ReplyMessage, rep.raw, rep_len))
            st = STATUS_ACCESS_VIOLATION;
    }
    ObDereferenceObject(hdr);
    return st;
}

/* Server side: completes the pending request named by the message's MessageId. */
static NTSTATUS port_reply_core(struct nt_port *p, const union port_msg_buf *msg, ULONG len)
{
    struct list_head *pos;
    NTSTATUS st = STATUS_INVALID_PARAMETER;
    spin_lock(&port_lock);
    if (p->disconnected) {
        spin_unlock(&port_lock);
        return STATUS_PORT_DISCONNECTED;
    }
    list_for_each(pos, &p->pending) {
        struct nt_port_msg *m = list_entry(pos, struct nt_port_msg, link);
        if (m->message_id == msg->hdr.MessageId && m->state == PORT_MSG_PENDING) {
            list_del(&m->link);
            m->linked = false;
            memcpy(m->reply.raw, msg->raw, len);
            m->reply_len = len;
            m->state = PORT_MSG_REPLIED;
            nt_set_event(m->reply_ev, NULL);
            st = STATUS_SUCCESS;
            break;
        }
    }
    spin_unlock(&port_lock);
    return st;
}

static NTSTATUS port_reply_user(HANDLE PortHandle, PPORT_MESSAGE ReplyMessage)
{
    struct nt_object_header *hdr = NULL;
    union port_msg_buf buf;
    ULONG len = 0;
    NTSTATUS st = port_ref_handle(PortHandle, PORT_KIND_SERVER, &hdr);
    if (!NT_SUCCESS(st)) return st;
    st = port_copy_in(ReplyMessage, LPC_REPLY, PORT_BODY(hdr)->max_message_length, &buf, &len);
    if (NT_SUCCESS(st)) st = port_reply_core(PORT_BODY(hdr), &buf, len);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtReplyPort(HANDLE PortHandle, PPORT_MESSAGE ReplyMessage)
{
    return port_reply_user(PortHandle, ReplyMessage);
}

/* v1 has no server-initiated messages, so there is nothing to wait for after
 * the reply. This performs the reply part only. */
NTSTATUS nt_svc_NtReplyWaitReplyPort(HANDLE PortHandle, PPORT_MESSAGE ReplyMessage)
{
    return port_reply_user(PortHandle, ReplyMessage);
}

/* Server side: waits for the next request, moves it to pending and copies it out. */
static NTSTATUS port_receive_core(struct nt_port *p, BOOLEAN alertable, PLARGE_INTEGER timeout,
                                  UCHAR *out, ULONG *out_len)
{
    for (;;) {
        NTSTATUS st;
        spin_lock(&port_lock);
        if (p->disconnected) { spin_unlock(&port_lock); return STATUS_PORT_DISCONNECTED; }
        if (!list_empty(&p->inbox)) {
            struct nt_port_msg *m = list_first_entry(&p->inbox, struct nt_port_msg, link);
            list_del(&m->link);
            list_add_tail(&m->link, &p->pending);
            m->linked = true;
            memcpy(out, m->request.raw, m->request_len);
            *out_len = m->request_len;
            if (!list_empty(&p->inbox)) nt_set_event(p->queue_ev, NULL);
            spin_unlock(&port_lock);
            return STATUS_SUCCESS;
        }
        spin_unlock(&port_lock);
        st = port_wait_on(p, alertable, timeout);
        if (st == STATUS_TIMEOUT || st == STATUS_USER_APC || st == STATUS_ALERTED || !NT_SUCCESS(st))
            return st;
    }
}

NTSTATUS port_receive_wait(HANDLE ServerPortHandle, BOOLEAN Alertable,
                           PLARGE_INTEGER Timeout, PPORT_MESSAGE Message)
{
    struct nt_object_header *hdr = NULL;
    union port_msg_buf buf;
    ULONG len = 0;
    NTSTATUS st;
    if (!Message) return STATUS_INVALID_PARAMETER;
    st = port_ref_handle(ServerPortHandle, PORT_KIND_SERVER, &hdr);
    if (!NT_SUCCESS(st)) return st;
    st = port_receive_core(PORT_BODY(hdr), Alertable, Timeout, buf.raw, &len);
    if (st == STATUS_SUCCESS && copy_to_user(Message, buf.raw, len)) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtReplyWaitReceivePort(HANDLE PortHandle, PVOID *PortContext,
                                       PPORT_MESSAGE ReplyMessage, PPORT_MESSAGE ReceiveMessage)
{
    NTSTATUS st;
    PVOID ctx = NULL;
    if (!ReceiveMessage) return STATUS_INVALID_PARAMETER;
    /* v1 has no port contexts, so the output is always NULL. */
    if (PortContext && copy_to_user(PortContext, &ctx, sizeof(ctx))) return STATUS_ACCESS_VIOLATION;
    if (ReplyMessage) {
        st = port_reply_user(PortHandle, ReplyMessage);
        if (!NT_SUCCESS(st)) return st;
    }
    return port_receive_wait(PortHandle, false, NULL, ReceiveMessage);
}
