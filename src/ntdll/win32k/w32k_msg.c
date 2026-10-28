/* w32k_msg.c - message queues, send/post/get/peek/dispatch, timers, focus,
 * activation, capture, cursors, hooks, painting and DefWindowProc.
 *
 * Retrieval priority in GetMessage/PeekMessage (fixed by this model, see the
 * header comment): posted messages in FIFO order (input included), then one
 * WM_PAINT for a window that needs it, then one WM_TIMER for a due timer.
 */
#include "w32k_int.h"

static int in_range(uint32_t msg, uint32_t min, uint32_t max)
{
    if (min == 0 && max == 0)
        return 1;
    return msg >= min && msg <= max;
}

static w32k_wake_fn wake_fn;

void w32k_set_wake_hook(w32k_wake_fn fn)
{
    wake_fn = fn;
}

void w32k_notify(uint32_t tid)
{
    if (wake_fn)
        wake_fn(tid);
}

/* Milliseconds until the earliest timer of the current thread is due (1 when one is
 * already due). W32K_NO_TIMER when the thread has no timer. */
uint32_t w32k_timer_wait_ms(void)
{
    w32k_queue *q = w32k_cur_queue();
    uint32_t best = W32K_NO_TIMER;
    if (!q)
        return best;
    for (uint32_t i = 0; i < W32K_MAX_TIMERS; ++i) {
        w32k_timer *t = &q->timers[i];
        uint32_t d;
        if (!t->used)
            continue;
        d = w32k_g.now_ms < t->due ? t->due - w32k_g.now_ms : 1;
        if (d < best)
            best = d;
    }
    return best;
}

int w32k_queue_push(w32k_queue *q, const w32k_msg *m)
{
    if (q->count >= W32K_QUEUE_CAP)
        return 0;
    q->ring[(q->head + q->count) % W32K_QUEUE_CAP] = *m;
    q->count++;
    w32k_notify(q->tid);
    return 1;
}

/* Find the first queued message that matches. Removes it when remove is set. */
static int ring_next(w32k_queue *q, w32k_msg *out, w32k_handle hwnd, uint32_t min, uint32_t max,
                     int remove)
{
    for (uint32_t i = 0; i < q->count; ++i) {
        uint32_t idx = (q->head + i) % W32K_QUEUE_CAP;
        const w32k_msg *m = &q->ring[idx];
        if (hwnd && m->hwnd != hwnd)
            continue;
        if (!in_range(m->message, min, max))
            continue;
        *out = *m;
        if (remove) {
            for (uint32_t j = i; j + 1 < q->count; ++j) {
                uint32_t a = (q->head + j) % W32K_QUEUE_CAP;
                uint32_t b = (q->head + j + 1) % W32K_QUEUE_CAP;
                q->ring[a] = q->ring[b];
            }
            q->count--;
        }
        return 1;
    }
    return 0;
}

static int paint_next(w32k_queue *q, w32k_msg *out, w32k_handle hwnd, uint32_t min, uint32_t max,
                      int remove)
{
    if (!in_range(W32K_WM_PAINT, min, max))
        return 0;
    for (uint32_t i = 0; i < W32K_MAX_WINDOWS; ++i) {
        w32k_window *w = (w32k_window *)w32k_pool_index(&w32k_pool_windows, i);
        w32k_handle h;
        if (!w->hdr.used || w->in_destroy || !w->needs_paint || !w->visible || w->tid != q->tid)
            continue;
        h = ((uint32_t)W32K_T_WINDOW << 24) | (((uint32_t)w->hdr.gen & 0xFFu) << 16) | (i + 1u);
        if (hwnd && h != hwnd)
            continue;
        w32k_zero(out, sizeof(*out));
        out->hwnd = h;
        out->message = W32K_WM_PAINT;
        out->time = w32k_g.now_ms;
        if (remove)
            w->needs_paint = 0;
        return 1;
    }
    return 0;
}

static int timer_next(w32k_queue *q, w32k_msg *out, w32k_handle hwnd, uint32_t min, uint32_t max,
                      int remove)
{
    if (!in_range(W32K_WM_TIMER, min, max))
        return 0;
    for (uint32_t i = 0; i < W32K_MAX_TIMERS; ++i) {
        w32k_timer *t = &q->timers[i];
        if (!t->used || w32k_g.now_ms < t->due)
            continue;
        if (hwnd && t->hwnd != hwnd)
            continue;
        w32k_zero(out, sizeof(*out));
        out->hwnd = t->hwnd;
        out->message = W32K_WM_TIMER;
        out->wparam = t->id;
        out->lparam = (intptr_t)t->proc;
        out->time = w32k_g.now_ms;
        if (remove)
            t->due = w32k_g.now_ms + t->elapse;
        return 1;
    }
    return 0;
}

static int next_message(w32k_queue *q, w32k_msg *out, w32k_handle hwnd, uint32_t min, uint32_t max,
                        int remove)
{
    if (ring_next(q, out, hwnd, min, max, remove))
        return 1;
    if (paint_next(q, out, hwnd, min, max, remove))
        return 1;
    if (timer_next(q, out, hwnd, min, max, remove))
        return 1;
    return 0;
}

static void stamp(w32k_msg *m, w32k_queue *q)
{
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    (void)q;
    m->time = w32k_g.now_ms;
    m->pt.x = s ? s->cursor_x : 0;
    m->pt.y = s ? s->cursor_y : 0;
}

/* ---- Calling window procedures ---------------------------------------------- */

intptr_t w32k_call_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32k_window *w = w32k_window_of(hwnd);
    uint32_t prev;
    intptr_t r;
    if (!w)
        return 0;
    if (w->proc == NULL)
        return w32k_def_window_proc(hwnd, msg, wp, lp);
    if (w32k_g.cur_tid == w->tid) {
        return w->proc(hwnd, msg, wp, lp);
    }
    /* Cross-thread send: run the owner's procedure with the owner as current thread. */
    prev = w32k_set_current_thread(w->tid);
    r = w->proc(hwnd, msg, wp, lp);
    w32k_set_current_thread(prev);
    return r;
}

intptr_t w32k_send_message(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    w32k_run_hooks(W32K_WH_CALLWNDPROC, 0, (uintptr_t)msg, (intptr_t)hwnd, w->desktop);
    return w32k_call_proc(hwnd, msg, wp, lp);
}

/* ---- Posting ---------------------------------------------------------------- */

int w32k_post_message(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_queue *q;
    w32k_msg m;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    q = w32k_queue_for(w->tid, 1);
    w32k_zero(&m, sizeof(m));
    m.hwnd = hwnd;
    m.message = msg;
    m.wparam = wp;
    m.lparam = lp;
    stamp(&m, q);
    if (!w32k_queue_push(q, &m)) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_QUOTA);
        return 0;
    }
    return 1;
}

int w32k_post_thread_message(uint32_t tid, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32k_queue *q = w32k_queue_for(tid, 0);
    w32k_msg m;
    if (!q) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    w32k_zero(&m, sizeof(m));
    m.message = msg;
    m.wparam = wp;
    m.lparam = lp;
    stamp(&m, q);
    if (!w32k_queue_push(q, &m)) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_QUOTA);
        return 0;
    }
    return 1;
}

void w32k_post_quit_message(int exit_code)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_msg m;
    w32k_zero(&m, sizeof(m));
    m.message = W32K_WM_QUIT;
    m.wparam = (uintptr_t)(uint32_t)exit_code;
    stamp(&m, q);
    w32k_queue_push(q, &m);
}

/* ---- Retrieval -------------------------------------------------------------- */

static void run_getmessage_hooks(w32k_queue *q, const w32k_msg *m, int removed)
{
    (void)removed;
    w32k_run_hooks(W32K_WH_GETMESSAGE, removed ? 1 : 0, (uintptr_t)m->message, (intptr_t)m, q->desktop);
}

/* GetMessageTime and GetMessagePos report the message the thread last retrieved. A message
 * with no point of its own gets the cursor position. */
static void note_last_message(w32k_queue *q, const w32k_msg *m)
{
    q->msg_time = m->time;
    q->msg_pt = m->pt;
    if (m->pt.x == 0 && m->pt.y == 0)
        w32k_get_cursor_pos(&q->msg_pt);
}

uint32_t w32k_get_message_time(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? q->msg_time : 0;
}

w32k_point w32k_get_message_pos(void)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_point z = {0, 0};
    return q ? q->msg_pt : z;
}

int w32k_get_message(w32k_msg *msg, w32k_handle hwnd, uint32_t min, uint32_t max)
{
    w32k_queue *q = w32k_cur_queue();
    if (!msg || !q) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (!next_message(q, msg, hwnd, min, max, 1))
        return W32K_WOULD_BLOCK;
    run_getmessage_hooks(q, msg, 1);
    note_last_message(q, msg);
    if (msg->message == W32K_WM_QUIT)
        return 0;
    return 1;
}

int w32k_peek_message(w32k_msg *msg, w32k_handle hwnd, uint32_t min, uint32_t max, uint32_t flags)
{
    w32k_queue *q = w32k_cur_queue();
    int remove = (flags & W32K_PM_REMOVE) != 0;
    if (!msg || !q) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!next_message(q, msg, hwnd, min, max, remove))
        return 0;
    if (remove) {
        run_getmessage_hooks(q, msg, 1);
        note_last_message(q, msg);
    }
    return 1;
}

intptr_t w32k_dispatch_message(const w32k_msg *msg)
{
    if (!msg)
        return 0;
    if (msg->message == W32K_WM_TIMER && msg->lparam != 0) {
        w32k_timerproc tp = (w32k_timerproc)(uintptr_t)msg->lparam;
        tp(msg->hwnd, W32K_WM_TIMER, msg->wparam, msg->time);
        return 0;
    }
    if (!msg->hwnd)
        return 0;
    if (!w32k_window_checked(msg->hwnd))
        return 0;
    return w32k_send_message(msg->hwnd, msg->message, msg->wparam, msg->lparam);
}

/* Virtual key to character for the keys TranslateMessage handles (US layout). */
static int vk_to_char(uint32_t vk, int shift, int caps, uint16_t *ch)
{
    if (vk >= 'A' && vk <= 'Z') {
        int upper = shift != caps;
        *ch = (uint16_t)(upper ? vk : vk + ('a' - 'A'));
        return 1;
    }
    if (vk >= '0' && vk <= '9') {
        *ch = (uint16_t)vk;
        return 1;
    }
    if (vk == W32K_VK_SPACE) {
        *ch = ' ';
        return 1;
    }
    if (vk == W32K_VK_RETURN) {
        *ch = '\r';
        return 1;
    }
    if (vk == W32K_VK_TAB) {
        *ch = '\t';
        return 1;
    }
    if (vk == W32K_VK_BACK) {
        *ch = '\b';
        return 1;
    }
    if (vk == W32K_VK_ESCAPE) {
        *ch = 0x1B;
        return 1;
    }
    return 0;
}

int w32k_translate_message(const w32k_msg *msg)
{
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    uint16_t ch;
    int shift, caps;
    uint32_t wm;
    if (!msg || !s)
        return 0;
    if (msg->message != W32K_WM_KEYDOWN && msg->message != W32K_WM_SYSKEYDOWN)
        return 0;
    shift = s->key_down[W32K_VK_SHIFT] != 0;
    caps = 0;
    if (!vk_to_char((uint32_t)msg->wparam, shift, caps, &ch))
        return 0;
    wm = msg->message == W32K_WM_SYSKEYDOWN ? W32K_WM_SYSCHAR : W32K_WM_CHAR;
    return w32k_post_message(msg->hwnd, wm, ch, msg->lparam);
}

/* ---- Timers ----------------------------------------------------------------- */

uintptr_t w32k_set_timer(w32k_handle hwnd, uintptr_t id, uint32_t elapse, w32k_timerproc proc)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_timer *slot = NULL;
    if (!q)
        return 0;
    if (hwnd && !w32k_window_checked(hwnd)) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (!hwnd && id == 0) {
        q->timer_seq++;
        if (q->timer_seq == 0)
            q->timer_seq = 1;
        id = q->timer_seq;
    }
    if (elapse < 10)
        elapse = 10;
    for (uint32_t i = 0; i < W32K_MAX_TIMERS; ++i) {
        w32k_timer *t = &q->timers[i];
        if (t->used && t->hwnd == hwnd && t->id == id) {
            slot = t;
            break;
        }
        if (!t->used && !slot)
            slot = t;
    }
    if (!slot) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    slot->used = 1;
    slot->hwnd = hwnd;
    slot->id = id;
    slot->elapse = elapse;
    slot->due = w32k_g.now_ms + elapse;
    slot->proc = proc;
    return id;
}

int w32k_kill_timer(w32k_handle hwnd, uintptr_t id)
{
    w32k_queue *q = w32k_cur_queue();
    int found = 0;
    if (!q)
        return 0;
    for (uint32_t i = 0; i < W32K_MAX_TIMERS; ++i) {
        w32k_timer *t = &q->timers[i];
        if (t->used && t->hwnd == hwnd && t->id == id) {
            t->used = 0;
            found = 1;
        }
    }
    if (!found) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Purge WM_TIMER messages for this timer that are already queued. */
    for (uint32_t i = 0; i < q->count;) {
        uint32_t idx = (q->head + i) % W32K_QUEUE_CAP;
        if (q->ring[idx].message == W32K_WM_TIMER && q->ring[idx].hwnd == hwnd &&
            q->ring[idx].wparam == id) {
            for (uint32_t j = i; j + 1 < q->count; ++j)
                q->ring[(q->head + j) % W32K_QUEUE_CAP] = q->ring[(q->head + j + 1) % W32K_QUEUE_CAP];
            q->count--;
        } else {
            ++i;
        }
    }
    return 1;
}

/* ---- Focus, activation, foreground, capture --------------------------------- */

static w32k_handle top_level_of(w32k_handle h)
{
    w32k_window *w = w32k_window_of(h);
    int guard = 0;
    while (w && w->parent && guard++ < 64) {
        h = w->parent;
        w = w32k_window_of(h);
    }
    return w ? h : 0;
}

static void set_focus_internal(w32k_queue *q, w32k_handle hwnd)
{
    w32k_handle old = q->focus;
    if (old == hwnd)
        return;
    q->focus = 0;
    if (old)
        w32k_call_proc(old, W32K_WM_KILLFOCUS, hwnd, 0);
    q->focus = hwnd;
    if (hwnd)
        w32k_call_proc(hwnd, W32K_WM_SETFOCUS, old, 0);
}

w32k_handle w32k_set_focus(w32k_handle hwnd)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_handle old;
    w32k_window *w;
    if (!q)
        return 0;
    if (hwnd) {
        w = w32k_window_checked(hwnd);
        if (!w) {
            w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        if (w->tid != q->tid) {
            w32k_set_error(W32K_ERROR_ACCESS_DENIED);
            return 0;
        }
    }
    old = q->focus;
    set_focus_internal(q, hwnd);
    return old;
}

w32k_handle w32k_get_focus(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? q->focus : 0;
}

void w32k_send_activate(w32k_window *w, w32k_handle h, int active)
{
    (void)w;
    w32k_call_proc(h, W32K_WM_ACTIVATE, active ? W32K_WA_ACTIVE : W32K_WA_INACTIVE, 0);
}

w32k_handle w32k_set_active_window(w32k_handle hwnd)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_handle old, top, old_focus;
    int focus_in_old = 0;
    if (!q)
        return 0;
    if (hwnd) {
        w32k_window *w = w32k_window_checked(hwnd);
        if (!w || w->parent) {
            w32k_set_error(w ? W32K_ERROR_INVALID_PARAMETER : W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        if (w->tid != q->tid) {
            w32k_set_error(W32K_ERROR_ACCESS_DENIED);
            return 0;
        }
    }
    old = q->active;
    if (old == hwnd)
        return old;
    old_focus = q->focus;
    top = old_focus ? top_level_of(old_focus) : 0;
    focus_in_old = old && top == old;
    if (old) {
        q->active = 0;
        w32k_send_activate(w32k_window_of(old), old, 0);
    }
    if (focus_in_old) {
        q->focus = 0;
        w32k_call_proc(old_focus, W32K_WM_KILLFOCUS, 0, 0);
    }
    q->active = hwnd;
    if (hwnd) {
        w32k_send_activate(w32k_window_of(hwnd), hwnd, 1);
        if (focus_in_old || q->focus == 0)
            set_focus_internal(q, hwnd);
    }
    return old;
}

w32k_handle w32k_get_active_window(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? q->active : 0;
}

int w32k_set_foreground_window(w32k_handle hwnd)
{
    w32k_desktop *d = w32k_cur_desktop();
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_queue *q = w32k_cur_queue();
    if (!d || !w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    d->foreground = hwnd;
    if (w->tid == q->tid)
        w32k_set_active_window(top_level_of(hwnd));
    return 1;
}

w32k_handle w32k_get_foreground_window(void)
{
    w32k_desktop *d = w32k_cur_desktop();
    if (!d)
        return 0;
    return w32k_window_checked(d->foreground) ? d->foreground : 0;
}

w32k_handle w32k_set_capture(w32k_handle hwnd)
{
    w32k_desktop *d = w32k_cur_desktop();
    w32k_queue *q = w32k_cur_queue();
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_handle old;
    if (!d || !q)
        return 0;
    if (!w || w->tid != q->tid) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    old = d->capture;
    if (old == hwnd)
        return old;
    d->capture = hwnd;
    if (old)
        w32k_call_proc(old, W32K_WM_CAPTURECHANGED, 0, (intptr_t)hwnd);
    return old;
}

int w32k_release_capture(void)
{
    w32k_desktop *d = w32k_cur_desktop();
    w32k_handle old;
    if (!d || !d->capture)
        return 0;
    old = d->capture;
    d->capture = 0;
    w32k_call_proc(old, W32K_WM_CAPTURECHANGED, 0, 0);
    return 1;
}

w32k_handle w32k_get_capture(void)
{
    w32k_desktop *d = w32k_cur_desktop();
    return d ? d->capture : 0;
}

/* ---- Cursor ----------------------------------------------------------------- */

w32k_handle w32k_set_cursor(w32k_handle cursor)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_handle old;
    if (!q)
        return 0;
    if (cursor && !w32k_pool_get(&w32k_pool_cursors, cursor)) {
        w32k_set_error(W32K_ERROR_INVALID_CURSOR_HANDLE);
        return 0;
    }
    old = q->cursor;
    q->cursor = cursor;
    return old;
}

w32k_handle w32k_get_cursor(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? q->cursor : 0;
}

/* ---- Hooks ------------------------------------------------------------------ */

static int hook_type_known(int type)
{
    switch (type) {
    case W32K_WH_MSGFILTER:
    case W32K_WH_KEYBOARD:
    case W32K_WH_GETMESSAGE:
    case W32K_WH_CALLWNDPROC:
    case W32K_WH_CBT:
    case W32K_WH_MOUSE:
    case W32K_WH_SHELL:
    case W32K_WH_KEYBOARD_LL:
    case W32K_WH_MOUSE_LL:
        return 1;
    default:
        return 0;
    }
}

w32k_handle w32k_set_windows_hook(int type, w32k_hookproc proc, uint32_t tid)
{
    w32k_handle h;
    w32k_hook *hk;
    (void)tid;
    if (!proc || !hook_type_known(type)) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    hk = (w32k_hook *)w32k_pool_alloc(&w32k_pool_hooks, &h);
    if (!hk) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    hk->type = type;
    hk->proc = proc;
    hk->tid = tid;
    hk->desktop = w32k_queue_for(w32k_g.cur_tid, 1)->desktop;
    return h;
}

int w32k_unhook_windows_hook(w32k_handle hook)
{
    w32k_hook *hk = (w32k_hook *)w32k_pool_get(&w32k_pool_hooks, hook);
    if (!hk) {
        w32k_set_error(W32K_ERROR_INVALID_HOOK_HANDLE);
        return 0;
    }
    w32k_pool_free(&w32k_pool_hooks, hk);
    return 1;
}

intptr_t w32k_call_next_hook(w32k_handle hook, int code, uintptr_t wp, intptr_t lp)
{
    /* Hook chaining is not modelled: there is no next hook to call. */
    (void)hook;
    (void)code;
    (void)wp;
    (void)lp;
    return 0;
}

/* Runs the hooks of one type. Returns 1 when a low-level hook asked to block
 * the event (nonzero return from a *_LL hook). Other hook types ignore the
 * return value. */
int w32k_run_hooks(int type, int code, uintptr_t wp, intptr_t lp, w32k_handle desk)
{
    for (uint32_t i = 0; i < W32K_MAX_HOOKS; ++i) {
        w32k_hook *hk = (w32k_hook *)w32k_pool_index(&w32k_pool_hooks, i);
        intptr_t r;
        if (!hk->hdr.used || hk->type != type)
            continue;
        if (type != W32K_WH_KEYBOARD_LL && type != W32K_WH_MOUSE_LL && hk->desktop != desk)
            continue;
        r = hk->proc(code, wp, lp);
        if ((type == W32K_WH_KEYBOARD_LL || type == W32K_WH_MOUSE_LL) && r != 0)
            return 1;
    }
    return 0;
}

/* ---- Painting and invalidation ---------------------------------------------- */

w32k_handle w32k_begin_paint(w32k_handle hwnd, w32k_paintstruct *ps)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w || !ps) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    w32k_zero(ps, sizeof(*ps));
    w32k_get_client_rect(hwnd, &ps->rc);
    ps->erase = w->needs_paint != 0;
    w->needs_paint = 0;
    ps->hdc = w32k_get_dc(hwnd);
    return ps->hdc;
}

int w32k_end_paint(w32k_handle hwnd, const w32k_paintstruct *ps)
{
    if (!ps || !ps->hdc)
        return 0;
    return w32k_release_dc(hwnd, ps->hdc);
}

/* ---- DefWindowProc ---------------------------------------------------------- */

intptr_t w32k_def_window_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    w32k_window *w = w32k_window_of(hwnd);
    switch (msg) {
    case W32K_WM_NCCREATE:
        return 1;
    case W32K_WM_CLOSE:
        w32k_destroy_window(hwnd);
        return 0;
    case W32K_WM_ERASEBKGND:
        return 1;
    case W32K_WM_NCCALCSIZE:
        return 0;
    case W32K_WM_SETTEXT:
        if (!w)
            return 0;
        w32k_set_window_text(hwnd, (const uint16_t *)(uintptr_t)lp);
        return 1;
    case W32K_WM_GETTEXT:
        if (!w)
            return 0;
        return w32k_get_window_text(hwnd, (uint16_t *)(uintptr_t)lp, (int)wp);
    case W32K_WM_GETTEXTLENGTH:
        return w ? (intptr_t)w->title_len : 0;
    default:
        return 0;
    }
}
