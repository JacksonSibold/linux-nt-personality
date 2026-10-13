# 0072: LPC ports (NtCreatePort family) are not implemented

Status: Accepted. Implemented in fa370b1 (port.c, port.h, build lists) and c81f7f4 (port_test.c, kernel-host Makefile).
Finding: none in `docs/review/`. Gap found while listing services in `docs/status/SERVICES.md`: nine LPC services are MISSING (NtCreatePort, NtConnectPort, NtListenPort, NtAcceptConnectPort, NtCompleteConnectPort, NtRequestWaitReplyPort, NtReplyPort, NtReplyWaitReceivePort, NtReplyWaitReplyPort), and NtReplyWaitReceivePortEx is also missing.
Date: 2026-10-12

## Context

The service table `src/kernel/nt/nt_service_list.h` assigns system service numbers to the LPC calls, but no `nt_svc_` wrapper exists for them. A process that calls one gets `STATUS_NOT_IMPLEMENTED` from the weak stubs. The personality already has the pieces a port needs: object types with `ObCreateObject`/`ObInsertObject`/`ObReferenceObjectByName`, handles, dispatcher events with `nt_wait_for_multiple_objects` (alertable, with a timeout), and a leak counter for the host tests.

LPC is the message transport that the NT native and subsystem layers use between a server and its clients. Without it, no client/server pair can talk through the object manager.

## Options

1. Classic LPC in-kernel message queues. A server connection port (created by name), a connection request queue, and a pair of communication ports per connection. Messages are copied through the kernel between a `PORT_MESSAGE` header and a fixed-size body. Tradeoff: about 1000 lines of kernel code and a new object type. Needs the full wait and close protocol right.
2. Do not support LPC. Keep the nine services at `STATUS_NOT_IMPLEMENTED`, and document that a native-style client/server pair cannot run. Tradeoff: no change to the kernel, but the personality cannot run any program that depends on ports.

## Recommendation

Option 1, limited to the scope below. The rest of the personality's wait and object machinery already does most of the work, and the limits keep the surface small enough to test on the host.

Scope for v1:

- Connection-oriented stream only. A client connects through a named server connection port, the server accepts, and both sides exchange request/reply messages on their communication ports. No datagrams.
- `PORT_MESSAGE` header validation. `TotalLength` must be at least `sizeof(PORT_MESSAGE)` and at most `PORT_MAXIMUM_MESSAGE_LENGTH` (256 bytes, header included). `DataLength` must equal `TotalLength - sizeof(PORT_MESSAGE)`. `DataInfoOffset` must be 0. `ClientViewSize` must be 0. `Type` must be `LPC_REQUEST` or `LPC_REPLY`. Violations return `STATUS_PORT_MESSAGE_TOO_LONG` (length) or `STATUS_INVALID_PARAMETER` (other fields).
- Maximum message length. `NtCreatePort` caps the port's maximum at 256 bytes. Larger messages are rejected before they are queued.
- `ClientId` is set by the kernel from the sending thread. `MessageId` is assigned by the kernel to each request, and a reply must echo the request's `MessageId`. A reply matches a pending request on the same communication port.
- No section views and no handles in messages. `DataInfoOffset` is rejected, so there is no view or handle transfer in v1.
- No impersonation. No token is captured or used for the client in v1.
- Connection request queue capped at 16 pending requests per connection port. A request beyond the cap fails with `STATUS_INSUFFICIENT_RESOURCES`.
- Double reply. A second reply to the same `MessageId`, or a reply to an id that was never received, fails with `STATUS_INVALID_PARAMETER`.
- Disconnect. Closing either end of a connection marks both ends disconnected. Any later operation on a disconnected port, and any waiter that is blocked on it, returns `STATUS_PORT_DISCONNECTED`. A connection request that is refused or whose connection port closes returns `STATUS_PORT_CONNECTION_REFUSED`.
- Waits. Receive and listen use the existing dispatcher wait engine on an auto-reset "queue" event and a manual-reset "disconnected" event, so a timeout returns `STATUS_TIMEOUT` and an alertable wait can return `STATUS_USER_APC` or `STATUS_ALERTED`. The NT system service signatures have no timeout argument, so the wrappers pass an infinite, non-alertable wait. The timeout and alertable form is exposed on the internal primitives `port_listen` and `port_receive`, which the host tests exercise.

Not in v1: `NtReplyWaitReceivePortEx` (the fifth argument is a port-context and extended-flags form, deferred), datagrams, section views, handle passing, impersonation, the `LPC_CONNECTION_REQUEST` and `LPC_CLIENT_DIED` notifications, and port-level ACLs beyond the object manager's default.

## Patch sketch

- `src/kernel/nt/port.h`: `PORT_MESSAGE` layout, limits, the internal object bodies, the `port_*` primitives.
- `src/kernel/nt/port.c`: `nt_port_type` registration. Wrappers for the nine services, each taking the NT signature. Queue and reply bookkeeping under a spinlock. Events from `nt_create_event`, waited through `nt_wait_for_multiple_objects`.
- Build lists: `arch/x86/nt/Makefile` (`port.o`), `vm/kbuild.sh` (`expected_objects`, count 17 to 18), `tests/kernel-host/Makefile` (`port_test` added to `LOCAL_TESTS` and its object list). `tests/syntax/check.sh` uses a glob and needs no edit.
- `tests/kernel-host/port_test.c`: server, client and third-thread tests.

## Test plan

Host tests in `tests/kernel-host/port_test.c` (LOCAL_TESTS), run by `make check-quick`, with a server thread and a client thread:

- Connect, accept, complete, then a request and reply round trip with matching `MessageId`.
- Listen and receive timeouts return `STATUS_TIMEOUT`.
- Client disconnect wakes a blocked receiver with `STATUS_PORT_DISCONNECTED`.
- Oversize message (`TotalLength` above 256) is rejected with `STATUS_PORT_MESSAGE_TOO_LONG`.
- Malformed header fields (bad `DataLength`, nonzero `DataInfoOffset`, unknown `Type`) are rejected with `STATUS_INVALID_PARAMETER`.
- Double reply is rejected with `STATUS_INVALID_PARAMETER`.
- Connection queue cap returns `STATUS_INSUFFICIENT_RESOURCES`.
- Refused connection returns `STATUS_PORT_CONNECTION_REFUSED`.
- The allocation counter is unchanged after the run (leak tracker clean).

## Not verified

The kernel build (`vm/kbuild.sh`) is not run for this change, so `port.c` is checked only in the userspace shim build and the syntax check.

## Implementation notes

Deviations from the scope above, and what was found while implementing:

- `NtReplyWaitReplyPort` performs the reply only. Version 1 has no server-initiated messages, so there is nothing to wait for after the reply.
- The NT service signatures have no timeout or alertable argument. The timed and alertable forms are the internal `port_listen_wait` and `port_receive_wait`, which the tests use.
- `NtReplyWaitReceivePortEx` is not implemented and stays MISSING in `docs/status/SERVICES.md`.
- The service table gives `NtAcceptConnectPort` nine arguments. The NT signature has six, and the wrapper takes six.
- The object manager keeps a named object in its directory after its last handle closes, so a named port is never freed. The host tests use one long-lived connection port so that the leak check measures only per-connection state.

The module page is `docs/modules/port.md`.
