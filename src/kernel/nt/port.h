/* SPDX-License-Identifier: GPL-2.0 */
/*
 * port.h - LPC ports: connection-oriented request/reply messages between a
 *          server and its clients (see docs/decisions/0072-kernel-nt-lpc-ports.md).
 *
 * v1 scope: PORT_MESSAGE headers are validated against PORT_MAXIMUM_MESSAGE_LENGTH,
 * no section views or handles travel in messages, no impersonation.
 */

#ifndef NT_PORT_H
#define NT_PORT_H

#include "kshim.h"
#include "object.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

/* Largest message, header included. NtCreatePort caps the port limit here. */
#define PORT_MAXIMUM_MESSAGE_LENGTH 256
/* Connection requests that may wait on one connection port at a time. */
#define PORT_MAX_CONNECTION_QUEUE   16
/* Generic access for port handles (no per-port ACL in v1). */
#define PORT_ALL_ACCESS             0x001F0001UL

/* PORT_MESSAGE.Type values accepted from user mode in v1. */
#define LPC_REQUEST                 1
#define LPC_REPLY                   2
/* Set by the kernel on the message a listener receives for a connection. */
#define LPC_CONNECTION_REQUEST      10

/* Documented layout: the header is followed by DataLength bytes of data. */
typedef struct _PORT_MESSAGE {
    USHORT    DataLength;
    USHORT    TotalLength;
    USHORT    Type;
    USHORT    DataInfoOffset;
    CLIENT_ID ClientId;
    ULONG     MessageId;
    ULONG     ClientViewSize;
} PORT_MESSAGE, *PPORT_MESSAGE;

/* Bytes available for data in one message. */
#define PORT_MESSAGE_DATA_MAX (PORT_MAXIMUM_MESSAGE_LENGTH - sizeof(PORT_MESSAGE))

/* System service wrappers (NT signatures; see nt_service_list.h). */
NTSTATUS nt_svc_NtCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes,
                             ULONG MaxConnectionInfoLength, ULONG MaxMessageLength,
                             ULONG MaxPoolUsage);
NTSTATUS nt_svc_NtConnectPort(PHANDLE ClientPortHandle, PUNICODE_STRING PortName,
                              PVOID SecurityQos, PVOID ClientView, PVOID ServerView,
                              PULONG MaxMessageLength, PVOID ConnectionInfo,
                              PULONG ConnectionInfoLength);
NTSTATUS nt_svc_NtListenPort(HANDLE ConnectionPortHandle, PPORT_MESSAGE ConnectionRequest);
NTSTATUS nt_svc_NtAcceptConnectPort(PHANDLE ServerPortHandle, HANDLE ConnectionPortHandle,
                                    PPORT_MESSAGE ConnectionRequest, BOOLEAN AcceptConnection,
                                    PVOID ServerView, PVOID ClientView);
NTSTATUS nt_svc_NtCompleteConnectPort(HANDLE ServerPortHandle);
NTSTATUS nt_svc_NtRequestWaitReplyPort(HANDLE PortHandle, PPORT_MESSAGE RequestMessage,
                                       PPORT_MESSAGE ReplyMessage);
NTSTATUS nt_svc_NtReplyPort(HANDLE PortHandle, PPORT_MESSAGE ReplyMessage);
NTSTATUS nt_svc_NtReplyWaitReceivePort(HANDLE PortHandle, PVOID *PortContext,
                                       PPORT_MESSAGE ReplyMessage, PPORT_MESSAGE ReceiveMessage);
NTSTATUS nt_svc_NtReplyWaitReplyPort(HANDLE PortHandle, PPORT_MESSAGE ReplyMessage);

/*
 * Timed and alertable forms of the wait inside NtListenPort and
 * NtReplyWaitReceivePort. The NT signatures have no timeout, so the wrappers
 * pass NULL and FALSE. These are used by port_test.c.
 * Return STATUS_TIMEOUT, STATUS_USER_APC or STATUS_ALERTED when the wait ends
 * that way; STATUS_PORT_DISCONNECTED when the port closes.
 */
NTSTATUS port_listen_wait(HANDLE ConnectionPortHandle, BOOLEAN Alertable,
                          PLARGE_INTEGER Timeout, PPORT_MESSAGE Message);
NTSTATUS port_receive_wait(HANDLE ServerPortHandle, BOOLEAN Alertable,
                           PLARGE_INTEGER Timeout, PPORT_MESSAGE Message);

#endif /* NT_PORT_H */
