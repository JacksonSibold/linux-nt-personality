/*
 * except_pure.h - pure exception-dispatch bookkeeping used by except.c.
 *
 * Covers the vectored handler lists (insertion order and removal), the
 * decisions that turn a handler's return value into "resume" or "keep
 * searching", the nesting guard and the frame-walk end test. Nothing here
 * touches the stack, the handler lists' allocator or global state; the host
 * tests in tests/ntdll-host include this header directly.
 *
 * Return values follow the Windows contract:
 *   vectored handler: EXCEPTION_CONTINUE_EXECUTION = -1,
 *                     EXCEPTION_CONTINUE_SEARCH    = 0
 *   frame handler:    an EXCEPTION_DISPOSITION from except.h
 */

#ifndef EXCEPT_PURE_H
#define EXCEPT_PURE_H

#include "except.h"

#include <stddef.h>

/* Node in a vectored handler list. handle is unique per registration. */
typedef struct exc_veh_node {
    struct exc_veh_node *next;
    struct exc_veh_node *prev;
    PVOID handle;
    PVECTORED_EXCEPTION_HANDLER handler;
} exc_veh_node_t;

/* Maximum depth of nested RtlDispatchException calls before a dispatch gives
 * up and returns ExceptionContinueSearch. */
#define EXC_MAX_NESTED_DISPATCH 16

/* Link n at the front (first != 0) or the back of the list at *head. n must
 * already have next and prev cleared. */
static inline void exc_list_insert(exc_veh_node_t **head, exc_veh_node_t *n, int first)
{
    if (first) {
        n->next = *head;
        if (*head)
            (*head)->prev = n;
        *head = n;
        return;
    }
    if (!*head) {
        *head = n;
        return;
    }
    exc_veh_node_t *cur = *head;
    while (cur->next)
        cur = cur->next;
    cur->next = n;
    n->prev = cur;
}

/* Unlink the node with this handle and return it, or NULL when there is none.
 * The caller frees the node. */
static inline exc_veh_node_t *exc_list_remove(exc_veh_node_t **head, PVOID handle)
{
    for (exc_veh_node_t *cur = *head; cur; cur = cur->next) {
        if (cur->handle != handle)
            continue;
        if (cur->prev)
            cur->prev->next = cur->next;
        else
            *head = cur->next;
        if (cur->next)
            cur->next->prev = cur->prev;
        return cur;
    }
    return NULL;
}

/* A vectored exception handler resumes execution only on -1. Any other value
 * (0 here) passes the exception to the next handler. */
static inline int exc_veh_resumes(LONG ret)
{
    return ret == -1;
}

/* A frame handler resumes execution only with ExceptionContinueExecution.
 * Search, nested and collided dispositions unwind to the next frame. */
static inline int exc_frame_resumes(EXCEPTION_DISPOSITION disp)
{
    return disp == ExceptionContinueExecution;
}

/* True when a dispatch at this nesting depth must stop. depth is the depth
 * before this call increments it. */
static inline int exc_depth_exceeded(int depth)
{
    return depth > EXC_MAX_NESTED_DISPATCH;
}

/* The frame walk ends when the unwound context has no return address or no
 * stack. */
static inline int exc_walk_ended(ULONG64 rip, ULONG64 rsp)
{
    return rip == 0 || rsp == 0;
}

#endif /* EXCEPT_PURE_H */
