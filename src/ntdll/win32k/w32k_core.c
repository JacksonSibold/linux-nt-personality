/* w32k_core.c - object pools, window stations, desktops, threads, window classes
 * and the window lifecycle (create, show, move, destroy, properties).
 *
 * Message dispatch, focus, activation and hooks are in w32k_msg.c. Input and
 * the clipboard are in w32k_input.c; metrics, monitors and display settings in
 * w32k_sys.c; GDI in w32k_gdi.c.
 */
#include "w32k_int.h"

w32k_global w32k_g;
w32k_queue w32k_queues[W32K_MAX_THREADS];

static w32k_station st_arr[W32K_MAX_STATIONS];
static w32k_desktop desk_arr[W32K_MAX_DESKTOPS];
static w32k_window wnd_arr[W32K_MAX_WINDOWS];
static w32k_class cls_arr[W32K_MAX_CLASSES];
static w32k_cursor_obj cur_arr[W32K_MAX_CURSORS];
static w32k_icon_obj ico_arr[16];
static w32k_hook hook_arr[W32K_MAX_HOOKS];

w32k_pool w32k_pool_stations;
w32k_pool w32k_pool_desktops;
w32k_pool w32k_pool_windows;
w32k_pool w32k_pool_classes;
w32k_pool w32k_pool_cursors;
w32k_pool w32k_pool_icons;
w32k_pool w32k_pool_hooks;

/* ---- Small helpers ---------------------------------------------------------- */

void w32k_zero(void *p, size_t n)
{
    uint8_t *d = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i)
        d[i] = 0;
}

void w32k_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
}

size_t w32k_wlen(const uint16_t *s, size_t max)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (n < max && s[n])
        ++n;
    return n;
}

static uint16_t w32k_fold(uint16_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(c - 'a' + 'A');
    return c;
}

int w32k_wequal_ci(const uint16_t *a, const uint16_t *b)
{
    if (!a || !b)
        return 0;
    for (size_t i = 0;; ++i) {
        if (w32k_fold(a[i]) != w32k_fold(b[i]))
            return 0;
        if (a[i] == 0)
            return 1;
    }
}

void w32k_wcopy(uint16_t *dst, const uint16_t *src, size_t max)
{
    size_t n = w32k_wlen(src, max - 1);
    for (size_t i = 0; i < n; ++i)
        dst[i] = src[i];
    dst[n] = 0;
}

int w32k_rect_empty(const w32k_rect *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

int w32k_rect_intersect(w32k_rect *out, const w32k_rect *a, const w32k_rect *b)
{
    w32k_rect r;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (r.right <= r.left || r.bottom <= r.top) {
        out->left = out->top = out->right = out->bottom = 0;
        return 0;
    }
    *out = r;
    return 1;
}

int w32k_rect_contains(const w32k_rect *r, int32_t x, int32_t y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

/* ---- Pools ------------------------------------------------------------------ */

void *w32k_pool_index(w32k_pool *p, uint32_t i)
{
    return p->base + (size_t)i * p->elem_size;
}

void *w32k_pool_alloc(w32k_pool *p, w32k_handle *out)
{
    for (uint32_t i = 0; i < p->count; ++i) {
        w32k_hdr *h = (w32k_hdr *)w32k_pool_index(p, i);
        if (!h->used) {
            uint16_t gen = h->gen;
            w32k_zero(h, p->elem_size);
            h->used = 1;
            h->gen = gen;
            if (out)
                *out = (p->type << 24) | (((uint32_t)gen & 0xFFu) << 16) | (i + 1u);
            return h;
        }
    }
    return NULL;
}

void *w32k_pool_get(w32k_pool *p, w32k_handle h)
{
    uint32_t idx, gen;
    w32k_hdr *e;
    if (!h || W32K_HANDLE_TYPE(h) != p->type)
        return NULL;
    idx = W32K_HANDLE_INDEX(h);
    if (idx == 0 || idx > p->count)
        return NULL;
    gen = W32K_HANDLE_GEN(h);
    e = (w32k_hdr *)w32k_pool_index(p, idx - 1u);
    if (!e->used || ((uint32_t)e->gen & 0xFFu) != gen)
        return NULL;
    return e;
}

void w32k_pool_free(w32k_pool *p, void *elem)
{
    (void)p;
    w32k_hdr *h = (w32k_hdr *)elem;
    h->used = 0;
    h->gen = (uint16_t)(h->gen + 1u);
}

void w32k_pool_reset(w32k_pool *p)
{
    for (uint32_t i = 0; i < p->count; ++i) {
        w32k_hdr *h = (w32k_hdr *)w32k_pool_index(p, i);
        h->used = 0;
        h->gen = (uint16_t)(h->gen + 1u);
    }
}

static w32k_handle handle_of(w32k_pool *p, void *elem)
{
    uint32_t i = (uint32_t)(((uint8_t *)elem - p->base) / p->elem_size);
    return (p->type << 24) | (((uint32_t)((w32k_hdr *)elem)->gen & 0xFFu) << 16) | (i + 1u);
}

/* ---- Accessors -------------------------------------------------------------- */

w32k_station *w32k_station_of(w32k_handle h)
{
    return (w32k_station *)w32k_pool_get(&w32k_pool_stations, h);
}

w32k_desktop *w32k_desktop_of(w32k_handle h)
{
    return (w32k_desktop *)w32k_pool_get(&w32k_pool_desktops, h);
}

w32k_window *w32k_window_of(w32k_handle h)
{
    return (w32k_window *)w32k_pool_get(&w32k_pool_windows, h);
}

w32k_window *w32k_window_checked(w32k_handle h)
{
    w32k_window *w = w32k_window_of(h);
    if (w && w->in_destroy)
        return NULL;
    return w;
}

w32k_class *w32k_class_of(w32k_handle h)
{
    return (w32k_class *)w32k_pool_get(&w32k_pool_classes, h);
}

void w32k_set_error(uint32_t err)
{
    w32k_queue *q = w32k_cur_queue();
    if (q)
        q->last_error = err;
}

uint32_t w32k_get_last_error(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? q->last_error : 0;
}

void w32k_set_last_error(uint32_t err)
{
    w32k_set_error(err);
}

void w32k_set_now(uint32_t ms)
{
    w32k_g.now_ms = ms;
}

uint32_t w32k_now(void)
{
    return w32k_g.now_ms;
}

/* ---- Thread context --------------------------------------------------------- */

w32k_queue *w32k_queue_for(uint32_t tid, int create)
{
    w32k_queue *free_slot = NULL;
    for (size_t i = 0; i < W32K_MAX_THREADS; ++i) {
        w32k_queue *q = &w32k_queues[i];
        if (q->used && q->tid == tid) {
            /* A queue made before the input desktop existed gets it on first use. */
            if (!q->desktop)
                q->desktop = w32k_get_input_desktop();
            return q;
        }
        if (!q->used && !free_slot)
            free_slot = q;
    }
    if (!create || !free_slot)
        return NULL;
    w32k_zero(free_slot, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->tid = tid;
    free_slot->desktop = w32k_get_input_desktop();
    return free_slot;
}

w32k_queue *w32k_cur_queue(void)
{
    return w32k_queue_for(w32k_g.cur_tid, 1);
}

w32k_desktop *w32k_cur_desktop(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q ? w32k_desktop_of(q->desktop) : NULL;
}

uint32_t w32k_current_thread(void)
{
    return w32k_g.cur_tid;
}

uint32_t w32k_set_current_thread(uint32_t tid)
{
    uint32_t prev = w32k_g.cur_tid;
    w32k_g.cur_tid = tid;
    return prev;
}

int w32k_attach_thread(uint32_t tid)
{
    w32k_queue *q = w32k_queue_for(tid, 1);
    if (!q) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return 1;
}

static void destroy_thread_windows(uint32_t tid)
{
    /* Destroy top-level windows first; their children go with them. */
    for (uint32_t i = 0; i < W32K_MAX_WINDOWS; ++i) {
        w32k_window *w = (w32k_window *)w32k_pool_index(&w32k_pool_windows, i);
        if (w->hdr.used && w->tid == tid && w->parent == 0 && !w->in_destroy)
            w32k_destroy_tree(handle_of(&w32k_pool_windows, w));
    }
    for (uint32_t i = 0; i < W32K_MAX_WINDOWS; ++i) {
        w32k_window *w = (w32k_window *)w32k_pool_index(&w32k_pool_windows, i);
        if (w->hdr.used && w->tid == tid && !w->in_destroy)
            w32k_destroy_tree(handle_of(&w32k_pool_windows, w));
    }
}

void w32k_detach_thread(uint32_t tid)
{
    w32k_queue *q = w32k_queue_for(tid, 0);
    if (!q)
        return;
    uint32_t prev = w32k_set_current_thread(tid);
    destroy_thread_windows(tid);
    w32k_set_current_thread(prev);
    w32k_zero(q, sizeof(*q));
}

/* ---- Window stations and desktops ------------------------------------------ */

static w32k_handle find_station(const uint16_t *name)
{
    for (uint32_t i = 0; i < W32K_MAX_STATIONS; ++i) {
        w32k_station *s = (w32k_station *)w32k_pool_index(&w32k_pool_stations, i);
        if (s->hdr.used && w32k_wequal_ci(s->name, name))
            return handle_of(&w32k_pool_stations, s);
    }
    return 0;
}

w32k_handle w32k_create_winstation(const uint16_t *name)
{
    w32k_handle h;
    w32k_station *s;
    if (!name || !name[0]) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (find_station(name)) {
        w32k_set_error(W32K_ERROR_ALREADY_EXISTS);
        return 0;
    }
    s = (w32k_station *)w32k_pool_alloc(&w32k_pool_stations, &h);
    if (!s) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    w32k_wcopy(s->name, name, W32K_NAME_MAX);
    s->clip_pool = NULL;
    return h;
}

w32k_handle w32k_open_winstation(const uint16_t *name)
{
    w32k_handle h = find_station(name);
    if (!h)
        w32k_set_error(W32K_ERROR_FILE_NOT_FOUND);
    return h;
}

int w32k_close_winstation(w32k_handle station)
{
    w32k_station *s = w32k_station_of(station);
    if (!s) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    for (uint32_t i = 0; i < W32K_MAX_DESKTOPS; ++i) {
        w32k_desktop *d = (w32k_desktop *)w32k_pool_index(&w32k_pool_desktops, i);
        if (d->hdr.used && d->station == station) {
            w32k_set_error(W32K_ERROR_BUSY);
            return 0;
        }
    }
    w32k_pool_free(&w32k_pool_stations, s);
    return 1;
}

static w32k_handle find_desktop(w32k_handle station, const uint16_t *name)
{
    for (uint32_t i = 0; i < W32K_MAX_DESKTOPS; ++i) {
        w32k_desktop *d = (w32k_desktop *)w32k_pool_index(&w32k_pool_desktops, i);
        if (d->hdr.used && d->station == station && w32k_wequal_ci(d->name, name))
            return handle_of(&w32k_pool_desktops, d);
    }
    return 0;
}

w32k_handle w32k_create_desktop(w32k_handle station, const uint16_t *name)
{
    w32k_handle h;
    w32k_desktop *d;
    w32k_station *s;
    if (!station)
        station = w32k_g.main_station;
    s = w32k_station_of(station);
    if (!s || !name || !name[0]) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (find_desktop(station, name)) {
        w32k_set_error(W32K_ERROR_ALREADY_EXISTS);
        return 0;
    }
    d = (w32k_desktop *)w32k_pool_alloc(&w32k_pool_desktops, &h);
    if (!d) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    d->station = station;
    w32k_wcopy(d->name, name, W32K_NAME_MAX);
    if (!s->active_desktop)
        s->active_desktop = h;
    return h;
}

w32k_handle w32k_open_desktop(w32k_handle station, const uint16_t *name)
{
    w32k_handle h;
    if (!station)
        station = w32k_g.main_station;
    h = find_desktop(station, name);
    if (!h)
        w32k_set_error(W32K_ERROR_FILE_NOT_FOUND);
    return h;
}

int w32k_close_desktop(w32k_handle desktop)
{
    w32k_desktop *d = w32k_desktop_of(desktop);
    w32k_station *s;
    if (!d) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (d->window_count) {
        w32k_set_error(W32K_ERROR_BUSY);
        return 0;
    }
    s = w32k_station_of(d->station);
    if (s && s->active_desktop == desktop)
        s->active_desktop = 0;
    w32k_pool_free(&w32k_pool_desktops, d);
    return 1;
}

int w32k_set_thread_desktop(w32k_handle desktop)
{
    w32k_queue *q = w32k_cur_queue();
    if (!w32k_desktop_of(desktop)) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (q->window_count) {
        w32k_set_error(W32K_ERROR_BUSY);
        return 0;
    }
    q->desktop = desktop;
    return 1;
}

w32k_handle w32k_get_thread_desktop(void)
{
    w32k_queue *q = w32k_cur_queue();
    return q->desktop;
}

int w32k_switch_desktop(w32k_handle desktop)
{
    w32k_desktop *d = w32k_desktop_of(desktop);
    w32k_station *s;
    if (!d) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    s = w32k_station_of(d->station);
    if (!s)
        return 0;
    s->active_desktop = desktop;
    return 1;
}

w32k_handle w32k_get_input_desktop(void)
{
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    return s ? s->active_desktop : 0;
}

/* ---- Window classes --------------------------------------------------------- */

static w32k_handle find_class_by_name(const uint16_t *name)
{
    for (uint32_t i = 0; i < W32K_MAX_CLASSES; ++i) {
        w32k_class *c = (w32k_class *)w32k_pool_index(&w32k_pool_classes, i);
        if (c->hdr.used && w32k_wequal_ci(c->name, name))
            return handle_of(&w32k_pool_classes, c);
    }
    return 0;
}

w32k_handle w32k_find_class_handle(uintptr_t cls)
{
    if (cls < 0x10000u) {
        for (uint32_t i = 0; i < W32K_MAX_CLASSES; ++i) {
            w32k_class *c = (w32k_class *)w32k_pool_index(&w32k_pool_classes, i);
            if (c->hdr.used && c->atom == (uint16_t)cls)
                return handle_of(&w32k_pool_classes, c);
        }
        return 0;
    }
    return find_class_by_name((const uint16_t *)cls);
}

uint16_t w32k_register_class(const w32k_wndclass *wc)
{
    w32k_handle h;
    w32k_class *c;
    size_t n;
    if (!wc || !wc->name || !wc->proc || wc->wnd_extra < 0 || wc->wnd_extra > W32K_WND_EXTRA_MAX ||
        wc->cls_extra < 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    n = w32k_wlen(wc->name, W32K_CLASS_NAME_MAX);
    if (n >= W32K_CLASS_NAME_MAX) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (find_class_by_name(wc->name)) {
        w32k_set_error(W32K_ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    c = (w32k_class *)w32k_pool_alloc(&w32k_pool_classes, &h);
    if (!c) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    w32k_wcopy(c->name, wc->name, W32K_CLASS_NAME_MAX);
    c->atom = (uint16_t)(W32K_CLASS_ATOM_BASE + W32K_HANDLE_INDEX(h) - 1u);
    c->style = wc->style;
    c->proc = wc->proc;
    c->cls_extra = wc->cls_extra;
    c->wnd_extra = wc->wnd_extra;
    c->bg_brush = wc->bg_brush;
    c->cursor = wc->cursor;
    c->icon = wc->icon;
    c->menu_name = wc->menu_name;
    c->hinst = wc->hinst;
    return c->atom;
}

int w32k_unregister_class(uintptr_t cls, const void *hinst)
{
    w32k_handle h = w32k_find_class_handle(cls);
    w32k_class *c = w32k_class_of(h);
    (void)hinst;
    if (!c) {
        w32k_set_error(W32K_ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    if (c->window_count) {
        w32k_set_error(W32K_ERROR_CLASS_HAS_WINDOWS);
        return 0;
    }
    w32k_pool_free(&w32k_pool_classes, c);
    return 1;
}

int w32k_get_class_info(uintptr_t cls, w32k_wndclass *out)
{
    w32k_class *c = w32k_class_of(w32k_find_class_handle(cls));
    if (!c || !out) {
        w32k_set_error(W32K_ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    out->name = c->name;
    out->style = c->style;
    out->proc = c->proc;
    out->cls_extra = c->cls_extra;
    out->wnd_extra = c->wnd_extra;
    out->bg_brush = c->bg_brush;
    out->cursor = c->cursor;
    out->icon = c->icon;
    out->menu_name = c->menu_name;
    out->hinst = c->hinst;
    return 1;
}

/* ---- Window tree helpers ---------------------------------------------------- */

void w32k_window_screen_origin(const w32k_window *w, int32_t *x, int32_t *y)
{
    *x = w->rect.left;
    *y = w->rect.top;
}

void w32k_unlink_window(w32k_window *w, w32k_handle h)
{
    w32k_handle *link = NULL;
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        if (p)
            link = &p->first_child;
    } else {
        w32k_desktop *d = w32k_desktop_of(w->desktop);
        if (d)
            link = &d->top_head;
    }
    if (!link)
        return;
    while (*link) {
        if (*link == h) {
            *link = w->next_sibling;
            break;
        }
        w32k_window *s = w32k_window_of(*link);
        if (!s)
            break;
        link = &s->next_sibling;
    }
    w->next_sibling = 0;
}

static void insert_window(w32k_window *w, w32k_handle h)
{
    /* Newest window is the topmost: insert at the head of its sibling list. */
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        w->next_sibling = p->first_child;
        p->first_child = h;
    } else {
        w32k_desktop *d = w32k_desktop_of(w->desktop);
        w->next_sibling = d->top_head;
        d->top_head = h;
    }
}

w32k_handle w32k_window_at_point(w32k_desktop *d, int32_t x, int32_t y)
{
    w32k_handle cur = d->top_head;
    while (cur) {
        w32k_window *w = w32k_window_of(cur);
        if (!w)
            break;
        if (w->visible && w32k_rect_contains(&w->rect, x, y)) {
            /* Hit-test children (topmost first) for the deepest window. */
            w32k_handle child = w->first_child;
            w32k_handle hit = cur;
            while (child) {
                w32k_window *cw = w32k_window_of(child);
                if (!cw)
                    break;
                if (cw->visible && w32k_rect_contains(&cw->rect, x, y)) {
                    hit = child;
                    /* Descend one level further if this child has a matching child. */
                    w32k_handle gc = cw->first_child;
                    while (gc) {
                        w32k_window *g = w32k_window_of(gc);
                        if (!g)
                            break;
                        if (g->visible && w32k_rect_contains(&g->rect, x, y)) {
                            hit = gc;
                            break;
                        }
                        gc = g->next_sibling;
                    }
                    break;
                }
                child = cw->next_sibling;
            }
            return hit;
        }
        cur = w->next_sibling;
    }
    return 0;
}

/* ---- Window creation and destruction ---------------------------------------- */

static const uint16_t empty_name[1] = {0};

static void set_title(w32k_window *w, const uint16_t *name)
{
    w32k_wcopy(w->title, name ? name : empty_name, W32K_NAME_MAX);
    w->title_len = (uint32_t)w32k_wlen(w->title, W32K_NAME_MAX);
}

static void send_size_move(w32k_window *w, w32k_handle h, int moved, int sized)
{
    int32_t lx = 0, ly = 0;
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        if (p) {
            lx = w->rect.left - p->rect.left;
            ly = w->rect.top - p->rect.top;
        }
    } else {
        lx = w->rect.left;
        ly = w->rect.top;
    }
    if (moved)
        w32k_call_proc(h, W32K_WM_MOVE, 0, (intptr_t)(((uint32_t)ly << 16) | ((uint32_t)lx & 0xFFFFu)));
    if (sized) {
        int32_t cx = w->rect.right - w->rect.left, cy = w->rect.bottom - w->rect.top;
        w32k_call_proc(h, W32K_WM_SIZE, 0, (intptr_t)(((uint32_t)cy << 16) | ((uint32_t)cx & 0xFFFFu)));
    }
}

/* Children are stored in screen coordinates, so they move with their parent.
 * The model shifts their rects without sending them position messages. */
static void shift_children(w32k_window *w, int32_t dx, int32_t dy)
{
    w32k_handle c = w->first_child;
    while (c) {
        w32k_window *cw = w32k_window_of(c);
        if (!cw)
            break;
        cw->rect.left += dx;
        cw->rect.right += dx;
        cw->rect.top += dy;
        cw->rect.bottom += dy;
        shift_children(cw, dx, dy);
        c = cw->next_sibling;
    }
}

/* Position change: WINDOWPOSCHANGING (proposed rect), the new rect, then
 * WINDOWPOSCHANGED, then WM_MOVE and WM_SIZE for whatever changed. */
static void apply_rect(w32k_window *w, w32k_handle h, const w32k_rect *nr, int repaint)
{
    int moved = nr->left != w->rect.left || nr->top != w->rect.top;
    int sized = (nr->right - nr->left) != (w->rect.right - w->rect.left) ||
                (nr->bottom - nr->top) != (w->rect.bottom - w->rect.top);
    int32_t dx = nr->left - w->rect.left;
    int32_t dy = nr->top - w->rect.top;
    if (!moved && !sized)
        return;
    w32k_call_proc(h, W32K_WM_WINDOWPOSCHANGING, 0, (intptr_t)nr);
    w->rect = *nr;
    if (dx || dy)
        shift_children(w, dx, dy);
    w32k_call_proc(h, W32K_WM_WINDOWPOSCHANGED, 0, (intptr_t)&w->rect);
    send_size_move(w, h, moved, sized);
    if (repaint && w->visible)
        w->needs_paint = 1;
}

w32k_handle w32k_create_window(const w32k_createwnd *cs)
{
    w32k_handle h = 0, parent = 0;
    w32k_handle clh;
    w32k_queue *q = w32k_cur_queue();
    w32k_desktop *d = w32k_desktop_of(q->desktop);
    w32k_class *c;
    w32k_window *w, *p = NULL;
    int32_t x, y, cx, cy;
    w32k_rect rc;

    if (!cs || !d) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    clh = w32k_find_class_handle(cs->cls);
    c = w32k_class_of(clh);
    if (!c) {
        w32k_set_error(W32K_ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    if (cs->parent) {
        p = w32k_window_checked((w32k_handle)cs->parent);
        if (!p) {
            w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        parent = (w32k_handle)cs->parent;
    }
    if ((cs->style & W32K_WS_CHILD) && !parent) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    w = (w32k_window *)w32k_pool_alloc(&w32k_pool_windows, &h);
    if (!w) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    x = cs->x == (int32_t)W32K_CW_USEDEFAULT ? 0 : cs->x;
    y = cs->y == (int32_t)W32K_CW_USEDEFAULT ? 0 : cs->y;
    cx = cs->cx == (int32_t)W32K_CW_USEDEFAULT ? 640 : cs->cx;
    cy = cs->cy == (int32_t)W32K_CW_USEDEFAULT ? 480 : cs->cy;
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    if (p) {
        x += p->rect.left;
        y += p->rect.top;
    }
    rc.left = x;
    rc.top = y;
    rc.right = x + cx;
    rc.bottom = y + cy;

    w->desktop = q->desktop;
    w->tid = q->tid;
    w->cls = clh;
    w->parent = parent;
    w->style = cs->style;
    w->exstyle = cs->exstyle;
    w->rect = rc;
    w->proc = c->proc;
    w->extra_size = c->wnd_extra;
    w->id = (cs->style & W32K_WS_CHILD) ? (int32_t)cs->menu : 0;
    w->menu = (cs->style & W32K_WS_CHILD) ? 0 : cs->menu;
    w->owner_inst = (intptr_t)cs->hinst;
    w->dpi = 96;
    set_title(w, cs->name);
    insert_window(w, h);
    d->window_count++;
    q->window_count++;
    c->window_count++;

    if (!w32k_call_proc(h, W32K_WM_NCCREATE, 0, (intptr_t)cs)) {
        goto fail;
    }
    w32k_call_proc(h, W32K_WM_NCCALCSIZE, 0, (intptr_t)&w->rect);
    if (w32k_call_proc(h, W32K_WM_CREATE, 0, (intptr_t)cs) == -1) {
        goto fail;
    }
    if (cs->style & W32K_WS_VISIBLE)
        w32k_show_window(h, W32K_SW_SHOW);
    return h;

fail:
    w32k_call_proc(h, W32K_WM_NCDESTROY, 0, 0);
    w32k_unlink_window(w, h);
    d->window_count--;
    q->window_count--;
    c->window_count--;
    w32k_pool_free(&w32k_pool_windows, w);
    w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
    return 0;
}

void w32k_destroy_tree(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_of(hwnd);
    w32k_queue *q;
    w32k_desktop *d;
    w32k_class *c;
    if (!w || w->in_destroy)
        return;
    w->in_destroy = 1;
    q = w32k_queue_for(w->tid, 0);
    d = w32k_desktop_of(w->desktop);
    if (d && d->capture == hwnd) {
        d->capture = 0;
        w32k_call_proc(hwnd, W32K_WM_CAPTURECHANGED, 0, 0);
    }
    if (q && q->focus == hwnd) {
        q->focus = 0;
        w32k_call_proc(hwnd, W32K_WM_KILLFOCUS, 0, 0);
    }
    if (q && q->active == hwnd) {
        q->active = 0;
        w32k_call_proc(hwnd, W32K_WM_ACTIVATE, W32K_WA_INACTIVE, 0);
    }
    w32k_call_proc(hwnd, W32K_WM_DESTROY, 0, 0);
    while (w->first_child)
        w32k_destroy_tree(w->first_child);
    w32k_call_proc(hwnd, W32K_WM_NCDESTROY, 0, 0);

    w32k_unlink_window(w, hwnd);
    c = w32k_class_of(w->cls);
    if (c && c->window_count)
        c->window_count--;
    if (d && d->window_count)
        d->window_count--;
    if (q && q->window_count)
        q->window_count--;
    /* Drop queued messages addressed to this window and its timers. */
    if (q) {
        for (uint32_t i = 0; i < W32K_MAX_TIMERS; ++i) {
            if (q->timers[i].used && q->timers[i].hwnd == hwnd)
                q->timers[i].used = 0;
        }
    }
    w32k_pool_free(&w32k_pool_windows, w);
}

int w32k_destroy_window(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_queue *q = w32k_cur_queue();
    w32k_handle next = 0;
    uint32_t owner_tid;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    owner_tid = w->tid;
    if (!w->parent && q->active == hwnd) {
        /* Activate the next visible top-level window of the same thread. */
        w32k_desktop *d = w32k_desktop_of(w->desktop);
        for (w32k_handle cur = d ? d->top_head : 0; cur; ) {
            w32k_window *cw = w32k_window_of(cur);
            if (!cw)
                break;
            if (cur != hwnd && cw->tid == owner_tid && cw->visible && !cw->in_destroy) {
                next = cur;
                break;
            }
            cur = cw->next_sibling;
        }
    }
    w32k_destroy_tree(hwnd);
    if (next) {
        uint32_t prev = w32k_set_current_thread(owner_tid);
        w32k_set_active_window(next);
        w32k_set_current_thread(prev);
    }
    return 1;
}

int w32k_show_window(w32k_handle hwnd, int cmd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    int was;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    was = w->visible;
    if (cmd == W32K_SW_HIDE) {
        if (!was)
            return 0;
        w->visible = 0;
        w32k_call_proc(hwnd, W32K_WM_SHOWWINDOW, 0, 0);
        return was;
    }
    if (cmd == W32K_SW_MINIMIZE || cmd == W32K_SW_SHOWMINIMIZED) {
        w->state = W32K_SW_SHOWMINIMIZED;
    } else if (cmd == W32K_SW_SHOWMAXIMIZED) {
        w->state = W32K_SW_SHOWMAXIMIZED;
    } else {
        w->state = W32K_SW_SHOWNORMAL;
    }
    if (!was) {
        w->visible = 1;
        w->needs_paint = 1;
        w32k_call_proc(hwnd, W32K_WM_SHOWWINDOW, 1, 0);
        /* Showing always reports the position and size, even when unchanged. */
        w32k_call_proc(hwnd, W32K_WM_WINDOWPOSCHANGING, 0, (intptr_t)&w->rect);
        w32k_call_proc(hwnd, W32K_WM_WINDOWPOSCHANGED, 0, (intptr_t)&w->rect);
        send_size_move(w, hwnd, 1, 1);
        if (!w->parent && cmd != W32K_SW_SHOWNOACTIVATE && cmd != W32K_SW_SHOWMINIMIZED &&
            cmd != W32K_SW_MINIMIZE) {
            w32k_set_active_window(hwnd);
        }
    }
    return was;
}

int w32k_move_window(w32k_handle hwnd, int32_t x, int32_t y, int32_t cx, int32_t cy, int repaint)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_rect nr;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        if (p) {
            x += p->rect.left;
            y += p->rect.top;
        }
    }
    nr.left = x;
    nr.top = y;
    nr.right = x + cx;
    nr.bottom = y + cy;
    apply_rect(w, hwnd, &nr, repaint);
    return 1;
}

int w32k_get_window_rect(w32k_handle hwnd, w32k_rect *rc)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w || !rc) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    *rc = w->rect;
    return 1;
}

int w32k_get_client_rect(w32k_handle hwnd, w32k_rect *rc)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w || !rc) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    rc->left = 0;
    rc->top = 0;
    rc->right = w->rect.right - w->rect.left;
    rc->bottom = w->rect.bottom - w->rect.top;
    return 1;
}

int w32k_client_to_screen(w32k_handle hwnd, w32k_point *pt)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w || !pt) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    pt->x += w->rect.left;
    pt->y += w->rect.top;
    return 1;
}

int w32k_screen_to_client(w32k_handle hwnd, w32k_point *pt)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w || !pt) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    pt->x -= w->rect.left;
    pt->y -= w->rect.top;
    return 1;
}

int w32k_is_window(w32k_handle hwnd)
{
    return w32k_window_checked(hwnd) != NULL;
}

int w32k_is_window_visible(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    return w ? w->visible : 0;
}

w32k_handle w32k_get_parent(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    return w ? w->parent : 0;
}

uint32_t w32k_get_window_thread(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    return w ? w->tid : 0;
}

/* FindWindow: top-level windows on the calling thread's desktop. */
w32k_handle w32k_find_window(uintptr_t cls, const uint16_t *name)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_desktop *d = q ? w32k_desktop_of(q->desktop) : NULL;
    w32k_handle clh = w32k_find_class_handle(cls);
    w32k_handle cur;
    if (!d || !clh)
        return 0;
    /* Walk the desktop z-order, topmost first. */
    for (cur = d->top_head; cur; ) {
        w32k_window *w = w32k_window_of(cur);
        if (!w)
            break;
        if (!w->in_destroy && w->cls == clh && (!name || w32k_wequal_ci(w->title, name)))
            return cur;
        cur = w->next_sibling;
    }
    return 0;
}

intptr_t w32k_get_window_long(w32k_handle hwnd, int index)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    switch (index) {
    case W32K_GWL_WNDPROC:
        return (intptr_t)w->proc;
    case W32K_GWL_HINSTANCE:
        return w->owner_inst;
    case W32K_GWL_HWNDPARENT:
        return (intptr_t)w->parent;
    case W32K_GWL_STYLE:
        return (intptr_t)w->style;
    case W32K_GWL_EXSTYLE:
        return (intptr_t)w->exstyle;
    case W32K_GWLP_USERDATA:
        return w->user_data;
    case W32K_GWL_ID:
        return w->id;
    default:
        break;
    }
    if (index >= 0 && index + 8 <= w->extra_size) {
        intptr_t v = 0;
        w32k_copy(&v, &w->extra[index], sizeof(v));
        return v;
    }
    w32k_set_error(W32K_ERROR_INVALID_INDEX);
    return 0;
}

intptr_t w32k_set_window_long(w32k_handle hwnd, int index, intptr_t value)
{
    w32k_window *w = w32k_window_checked(hwnd);
    intptr_t old;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    old = w32k_get_window_long(hwnd, index);
    switch (index) {
    case W32K_GWL_WNDPROC:
        w->proc = (w32k_wndproc)(uintptr_t)value;
        return old;
    case W32K_GWL_HINSTANCE:
        w->owner_inst = value;
        return old;
    case W32K_GWL_HWNDPARENT:
        return old;
    case W32K_GWL_STYLE:
        w->style = (uint32_t)value;
        return old;
    case W32K_GWL_EXSTYLE:
        w->exstyle = (uint32_t)value;
        return old;
    case W32K_GWLP_USERDATA:
        w->user_data = value;
        return old;
    case W32K_GWL_ID:
        w->id = (int32_t)value;
        return old;
    default:
        break;
    }
    if (index >= 0 && index + 8 <= w->extra_size) {
        w32k_copy(&w->extra[index], &value, sizeof(value));
        return old;
    }
    w32k_set_error(W32K_ERROR_INVALID_INDEX);
    return 0;
}

int w32k_set_window_text(w32k_handle hwnd, const uint16_t *text)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    set_title(w, text);
    return 1;
}

int w32k_get_window_text(w32k_handle hwnd, uint16_t *buf, int max)
{
    w32k_window *w = w32k_window_checked(hwnd);
    int n;
    if (!w || !buf || max <= 0) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    n = (int)w->title_len;
    if (n > max - 1)
        n = max - 1;
    for (int i = 0; i < n; ++i)
        buf[i] = w->title[i];
    buf[n] = 0;
    return n;
}

uint32_t w32k_get_dpi_for_window(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    return w ? w32k_dpi_for_rect(&w->rect) : 0;
}

int w32k_invalidate_rect(w32k_handle hwnd, const w32k_rect *rc, int erase)
{
    w32k_window *w = w32k_window_checked(hwnd);
    (void)rc;
    (void)erase;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    /* The model keeps one update region per window: the whole client area. */
    w->needs_paint = 1;
    w32k_notify(w->tid);
    return 1;
}

/* Clears every core pool and points each pool at its storage. Generations are
 * bumped, not reset, so handles from before a reset stay invalid. */
void w32k_core_reset(void)
{
    w32k_zero(st_arr, sizeof(st_arr));
    w32k_zero(desk_arr, sizeof(desk_arr));
    w32k_zero(wnd_arr, sizeof(wnd_arr));
    w32k_zero(cls_arr, sizeof(cls_arr));
    w32k_zero(cur_arr, sizeof(cur_arr));
    w32k_zero(ico_arr, sizeof(ico_arr));
    w32k_zero(hook_arr, sizeof(hook_arr));
    w32k_zero(w32k_queues, sizeof(w32k_queues));

    w32k_pool_stations.base = (uint8_t *)st_arr;
    w32k_pool_stations.elem_size = sizeof(st_arr[0]);
    w32k_pool_stations.count = W32K_MAX_STATIONS;
    w32k_pool_stations.type = W32K_T_STATION;

    w32k_pool_desktops.base = (uint8_t *)desk_arr;
    w32k_pool_desktops.elem_size = sizeof(desk_arr[0]);
    w32k_pool_desktops.count = W32K_MAX_DESKTOPS;
    w32k_pool_desktops.type = W32K_T_DESKTOP;

    w32k_pool_windows.base = (uint8_t *)wnd_arr;
    w32k_pool_windows.elem_size = sizeof(wnd_arr[0]);
    w32k_pool_windows.count = W32K_MAX_WINDOWS;
    w32k_pool_windows.type = W32K_T_WINDOW;

    w32k_pool_classes.base = (uint8_t *)cls_arr;
    w32k_pool_classes.elem_size = sizeof(cls_arr[0]);
    w32k_pool_classes.count = W32K_MAX_CLASSES;
    w32k_pool_classes.type = W32K_T_CLASS;

    w32k_pool_cursors.base = (uint8_t *)cur_arr;
    w32k_pool_cursors.elem_size = sizeof(cur_arr[0]);
    w32k_pool_cursors.count = W32K_MAX_CURSORS;
    w32k_pool_cursors.type = W32K_T_CURSOR;

    w32k_pool_icons.base = (uint8_t *)ico_arr;
    w32k_pool_icons.elem_size = sizeof(ico_arr[0]);
    w32k_pool_icons.count = 16;
    w32k_pool_icons.type = W32K_T_ICON;

    w32k_pool_hooks.base = (uint8_t *)hook_arr;
    w32k_pool_hooks.elem_size = sizeof(hook_arr[0]);
    w32k_pool_hooks.count = W32K_MAX_HOOKS;
    w32k_pool_hooks.type = W32K_T_HOOK;
}
