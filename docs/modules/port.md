# Module: port (LPC ports)

Sources: `src/kernel/nt/port.c`, `src/kernel/nt/port.h`
Tests: `tests/kernel-host/port_test.c` (LOCAL_TESTS, run by `make check-quick`)
Decision: `docs/decisions/0072-kernel-nt-lpc-ports.md` (scope and option choice)
Related: `object.md` (Port is an object type), `sync.md` (the wait engine and events the queues use), `handle.md` (handle close drives disconnect).

## Purpose

Connection-oriented request/reply messaging between a server and its clients, in the style of classic LPC. A server creates a named connection port. A client connects by name, the server listens, accepts and completes, and the two sides then exchange `PORT_MESSAGE` requests and replies on their communication ports. Before this module the nine LPC services were MISSING in `docs/status/SERVICES.md`.

## Services

Implemented (`nt_svc_` wrappers in `port.c`, NT signatures in `port.h`):

- `NtCreatePort` - creates a connection port, optionally named in the object namespace. The maximum message length is capped at 256 bytes.
- `NtConnectPort` - connects a client to a named connection port. Blocks until the server completes or refuses the connection. The connection info is copied into the listen message.
- `NtListenPort` - waits for a queued connection request and copies its message (header and connection info) to the caller.
- `NtAcceptConnectPort` - accepts (creating the server communication port and its handle) or refuses a listened request, identified by `MessageId`.
- `NtCompleteConnectPort` - completes an accepted request. Only then are the client and server ports linked and the client's connect returns.
- `NtRequestWaitReplyPort` - sends a request and blocks until the reply or until the port disconnects.
- `NtReplyPort` - replies to a received request, matched by `MessageId`.
- `NtReplyWaitReceivePort` - replies (optional) and then waits for the next request.
- `NtReplyWaitReplyPort` - replies only. Version 1 has no server-initiated messages, so there is nothing to wait for after the reply.

Not implemented: `NtReplyWaitReceivePortEx` (still MISSING in `SERVICES.md`).

## Object model

The Port object type (`nt_port_type`, name `Port`) has three kinds, all sharing one body (`struct nt_port`):

- connection port: created by `NtCreatePort`. Holds the queue of connection requests (`conn_queue`, at most `PORT_MAX_CONNECTION_QUEUE` = 16) and the listened or accepted requests (`conn_listened`).
- client communication port: returned by a successful `NtConnectPort`. Holds the link to its peer.
- server communication port: returned by `NtAcceptConnectPort`. Holds the inbox of requests not yet received (`inbox`) and the received requests awaiting a reply (`pending`).

A connected pair holds a reference on its peer. Each side is linked at `NtCompleteConnectPort`. Closing a handle (the object manager's `close` callback, `port_obj_close`) disconnects the pair, so the references are dropped and the objects can be freed.

Named ports stay alive in the object directory after their last handle closes. The object manager takes a reference on insert and does not drop it, so this is existing behaviour, not a port leak. The host tests use one long-lived connection port for this reason.

## Message rules (PORT_MESSAGE, `port_check_header`)

- `TotalLength` at least `sizeof(PORT_MESSAGE)` (32 bytes on x64) and at most the port's maximum, which is at most 256. Above the maximum: `STATUS_PORT_MESSAGE_TOO_LONG`.
- `DataLength` equals `TotalLength - sizeof(PORT_MESSAGE)`.
- `DataInfoOffset` and `ClientViewSize` are 0. Data info, views and handles are not supported in v1.
- `Type` is `LPC_REQUEST` (1) for requests and `LPC_REPLY` (2) for replies. Connection requests are kernel-generated with `LPC_CONNECTION_REQUEST` (10).
- The kernel sets `MessageId` on each request and `ClientId` from the sending process and thread. A reply must echo the request's `MessageId`.
- Replies: a reply to an id that is not pending (never received, already replied, or cancelled) fails with `STATUS_INVALID_PARAMETER`. This covers double replies.
- Messages are copied in full through a kernel buffer, so the caller's buffer is only read for `TotalLength` bytes and written for the reply length.

## Waits and timeouts

Listen and receive wait on the port's auto-reset `queue_ev` and manual-reset `disc_ev` with `nt_wait_for_multiple_objects` (WaitAny). The NT service signatures have no timeout or alertable argument, so the wrappers pass an infinite, non-alertable wait. The timed and alertable forms are the internal functions `port_listen_wait` and `port_receive_wait` (`port.h`); a timeout returns `STATUS_TIMEOUT`, and an alert returns `STATUS_USER_APC` or `STATUS_ALERTED`. The request side (`NtRequestWaitReplyPort`) and connect wait without a timeout and end only by reply, refusal, or disconnect.

## Locking and lifetime

One spinlock (`port_lock`) guards every port, request and message. It is never held across a wait, a handle-table call, or a dereference that could free an object. Waiters read their result under the lock after the wait returns. Each connection request is owned by its connecting thread, and each message by its requesting thread; they are freed once their final state is set and they are unlinked. The server only links them.

## Limits and statuses

| Condition | Status |
|---|---|
| Message above 256 bytes or the port maximum | `STATUS_PORT_MESSAGE_TOO_LONG` |
| Malformed header, unknown or wrong `Type`, `DataInfoOffset` set, bad `MessageId` on reply | `STATUS_INVALID_PARAMETER` |
| Connection queue full (16 waiting) | `STATUS_INSUFFICIENT_RESOURCES` |
| Connection refused, or the connection port closed while the request was queued or listened | `STATUS_PORT_CONNECTION_REFUSED` |
| Port or peer disconnected (including a request still pending when its peer closes) | `STATUS_PORT_DISCONNECTED` |
| Handle of the wrong kind (for example a connection port used as a client) | `STATUS_INVALID_PORT_HANDLE` |
| Name not found on connect | `STATUS_OBJECT_NAME_NOT_FOUND` |

## Known gaps (v1)

- No per-port ACL and no impersonation: the client's token is not captured.
- No section views or handles in messages, no datagrams, no `LPC_CONNECTION_REQUEST` or `LPC_CLIENT_DIED` notifications.
- `NtReplyWaitReplyPort` replies only (see above).
- The `NtAcceptConnectPort` service table entry declares 9 arguments; the NT signature has 6, and the wrapper takes 6.
- One global lock. Per-port locking is deferred.
- The kernel build (`vm/kbuild.sh`) is not run for this change. `port.c` uses only the shim-visible primitives, and its syntax passes `tests/syntax/check.sh`.

## Tests

`tests/kernel-host/port_test.c` runs a server and client on pthreads through a pool of workers whose eprocess and ethread are created before the allocation baseline. Covered: a three-message round trip, listen and receive timeouts, an alertable listen, disconnect waking a blocked receiver, the header rules and the 256-byte cap, the connect-side and reply-side status codes above, double and unknown replies, refusal, the 16-entry queue cap, a connection port closed while a connect is queued, and unknown names. The allocation count after the run must equal the baseline.
