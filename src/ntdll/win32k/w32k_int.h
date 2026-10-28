/* w32k_int.h - internal state shared by the win32k model sources. Not for shims. */
#ifndef W32K_INT_H
#define W32K_INT_H

#include "w32k.h"

#define W32K_MAX_STATIONS 4
#define W32K_MAX_DESKTOPS 16
#define W32K_MAX_WINDOWS 1024
#define W32K_MAX_CLASSES 256
#define W32K_MAX_THREADS 64
#define W32K_MAX_CURSORS 256
#define W32K_MAX_GDI 1024
#define W32K_MAX_HOOKS 32
#define W32K_QUEUE_CAP 256
#define W32K_MAX_TIMERS 32
#define W32K_CLIP_POOL (64u * 1024u)
#define W32K_CLIP_FORMATS 16
#define W32K_CLIP_REGISTERED_MAX 16
#define W32K_NAME_MAX 256
#define W32K_CLASS_NAME_MAX 64
#define W32K_WND_EXTRA_MAX 64
#define W32K_MAX_MONITORS 4
#define W32K_MAX_MODES 4
#define W32K_CLASS_ATOM_BASE 0xC000u
#define W32K_STOCK_OBJECTS 14
#define W32K_STOCK_CURSORS 5

/* Common header at the start of every pooled object. */
typedef struct w32k_hdr {
    uint8_t used;
    uint8_t pad;
    uint16_t gen;
} w32k_hdr;

typedef struct w32k_pool {
    uint8_t *base;
    uint32_t elem_size;
    uint32_t count;
    uint32_t type;
} w32k_pool;

void *w32k_pool_alloc(w32k_pool *p, w32k_handle *out);
void *w32k_pool_get(w32k_pool *p, w32k_handle h);
void w32k_pool_free(w32k_pool *p, void *elem);
void *w32k_pool_index(w32k_pool *p, uint32_t i);
void w32k_pool_reset(w32k_pool *p);

/* Small helpers (no libc). */
void w32k_zero(void *p, size_t n);
void w32k_copy(void *dst, const void *src, size_t n);
size_t w32k_wlen(const uint16_t *s, size_t max);
int w32k_wequal_ci(const uint16_t *a, const uint16_t *b);
void w32k_wcopy(uint16_t *dst, const uint16_t *src, size_t max);
int w32k_rect_empty(const w32k_rect *r);
int w32k_rect_intersect(w32k_rect *out, const w32k_rect *a, const w32k_rect *b);
int w32k_rect_contains(const w32k_rect *r, int32_t x, int32_t y);

typedef struct w32k_station {
    w32k_hdr hdr;
    uint16_t name[W32K_NAME_MAX];
    w32k_handle active_desktop;
    int32_t cursor_x, cursor_y;
    uint8_t key_down[256];
    uint8_t key_pressed[256];
    uint8_t mouse_buttons;
    /* Clipboard (per station). */
    w32k_handle clip_owner;
    w32k_handle clip_open_hwnd;
    int clip_open;
    uint32_t clip_formats_used;
    struct {
        uint32_t format;
        uint32_t offset;
        uint32_t size;
        int used;
    } clip_entries[W32K_CLIP_FORMATS];
    uint8_t *clip_pool;
    uint32_t clip_pool_used;
} w32k_station;

typedef struct w32k_hook {
    w32k_hdr hdr;
    int type;
    w32k_hookproc proc;
    uint32_t tid;
    w32k_handle desktop;
} w32k_hook;

typedef struct w32k_desktop {
    w32k_hdr hdr;
    w32k_handle station;
    uint16_t name[W32K_NAME_MAX];
    w32k_handle top_head;      /* top-level windows, topmost first (next_sibling) */
    w32k_handle foreground;
    w32k_handle capture;
    w32k_handle desktop_window; /* the pseudo window GetDesktopWindow returns; not in top_head */
    uint32_t window_count;
} w32k_desktop;

typedef struct w32k_window {
    w32k_hdr hdr;
    w32k_handle desktop;
    uint32_t tid;
    w32k_handle cls;
    w32k_handle parent;
    w32k_handle first_child;
    w32k_handle next_sibling;
    uint32_t style;
    uint32_t exstyle;
    w32k_rect rect;            /* screen coordinates */
    int32_t id;
    intptr_t user_data;
    intptr_t owner_inst;
    w32k_wndproc proc;
    int32_t extra_size;
    uint8_t extra[W32K_WND_EXTRA_MAX];
    int visible;
    int state;
    int needs_paint;
    int in_destroy;
    uint16_t title[W32K_NAME_MAX];
    uint32_t title_len;
    uintptr_t menu;
    uint32_t dpi;
} w32k_window;

typedef struct w32k_class {
    w32k_hdr hdr;
    uint16_t name[W32K_CLASS_NAME_MAX];
    uint16_t atom;
    uint32_t style;
    w32k_wndproc proc;
    int32_t cls_extra;
    int32_t wnd_extra;
    w32k_handle bg_brush;
    w32k_handle cursor;
    w32k_handle icon;
    const uint16_t *menu_name;
    const void *hinst;
    uint32_t window_count;
} w32k_class;

typedef struct w32k_timer {
    int used;
    w32k_handle hwnd;
    uintptr_t id;
    uint32_t elapse;
    uint32_t due;
    w32k_timerproc proc;
} w32k_timer;

typedef struct w32k_queue {
    int used;
    uint32_t tid;
    w32k_handle desktop;
    w32k_handle focus;
    w32k_handle active;
    w32k_handle cursor;
    w32k_msg ring[W32K_QUEUE_CAP];
    uint32_t head;
    uint32_t count;
    w32k_timer timers[W32K_MAX_TIMERS];
    uint32_t last_error;
    uint32_t window_count;
    uint32_t msg_time;
    w32k_point msg_pt;
    uintptr_t timer_seq;
} w32k_queue;

typedef struct w32k_cursor_obj {
    w32k_hdr hdr;
    int32_t xhot, yhot, w, h;
    int stock;
} w32k_cursor_obj;

typedef struct w32k_icon_obj {
    w32k_hdr hdr;
    int32_t w, h;
    uint32_t planes, bpp;
    int stock;
} w32k_icon_obj;

typedef struct w32k_monitor {
    w32k_rect rc;
    uint32_t dpi;
    int primary;
    int active;
} w32k_monitor;

typedef struct w32k_display_state {
    w32k_devmode current;
    w32k_devmode modes[W32K_MAX_MODES];
    uint32_t mode_count;
} w32k_display_state;

/* Global model state. */
typedef struct w32k_global {
    uint32_t now_ms;
    uint32_t cur_tid;
    w32k_handle main_station;
    w32k_handle default_desktop;
    w32k_handle stock_objects[W32K_STOCK_OBJECTS];
    w32k_handle stock_cursors[W32K_STOCK_CURSORS];
    w32k_handle stock_icons[1];
    w32k_monitor monitors[W32K_MAX_MONITORS];
    uint32_t monitor_count;
    w32k_display_state display;
    uint32_t dpi_awareness;
    int beep_enabled;
    int screen_saver_active;
    uint16_t registered_formats[W32K_CLIP_REGISTERED_MAX][64];
    uint32_t registered_format_count;
    uint32_t cursor_show_count;
    uint32_t double_click_ms;
    uint32_t key_repeat_speed;
    uint32_t mouse_speed;
} w32k_global;

extern w32k_global w32k_g;
extern w32k_pool w32k_pool_stations;
extern w32k_pool w32k_pool_desktops;
extern w32k_pool w32k_pool_windows;
extern w32k_pool w32k_pool_classes;
extern w32k_pool w32k_pool_cursors;
extern w32k_pool w32k_pool_icons;
extern w32k_pool w32k_pool_hooks;
extern w32k_queue w32k_queues[W32K_MAX_THREADS];

/* Setup (w32k_core.c, w32k_input.c, w32k_sys.c). */
void w32k_core_reset(void);
void w32k_input_init(void);
void w32k_rawinput_reset(void);
/* 1 when the raw input registration for the mouse (keyboard != 0: the keyboard) has RIDEV_NOLEGACY. */
int w32k_raw_nolegacy(int keyboard);
uint32_t w32k_dpi_for_rect(const w32k_rect *rc);

/* Accessors (implemented in w32k_core.c). */
w32k_queue *w32k_cur_queue(void);
w32k_queue *w32k_queue_for(uint32_t tid, int create);
/* Calls the wake hook for tid, if one is set (w32k_msg.c). */
void w32k_notify(uint32_t tid);
w32k_station *w32k_station_of(w32k_handle h);
w32k_desktop *w32k_desktop_of(w32k_handle h);
w32k_desktop *w32k_cur_desktop(void);
w32k_window *w32k_window_of(w32k_handle h);
w32k_class *w32k_class_of(w32k_handle h);
w32k_window *w32k_window_checked(w32k_handle h);
w32k_handle w32k_find_class_handle(uintptr_t cls);
void w32k_set_error(uint32_t err);
void w32k_window_screen_origin(const w32k_window *w, int32_t *x, int32_t *y);
w32k_handle w32k_window_at_point(w32k_desktop *d, int32_t x, int32_t y);
void w32k_unlink_window(w32k_window *w, w32k_handle h);
w32k_handle w32k_find_class_handle(uintptr_t cls);
intptr_t w32k_call_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);

/* Message and focus helpers (w32k_msg.c). */
int w32k_queue_push(w32k_queue *q, const w32k_msg *m);
void w32k_send_activate(w32k_window *w, w32k_handle hwnd, int active);
void w32k_destroy_tree(w32k_handle hwnd);
int w32k_run_hooks(int type, int code, uintptr_t wp, intptr_t lp, w32k_handle desk);

/* Display helpers (w32k_sys.c). */
void w32k_broadcast_displaychange(void);

/* GDI setup and framebuffer (w32k_gdi.c). */
void w32k_gdi_init(void);

/* Regions (w32k_region.c). Internal: the model and its tests use these. */
void w32k_region_init(void);
int w32k_region_is(w32k_handle h);
int w32k_region_contains(w32k_handle h, int32_t x, int32_t y);
w32k_handle w32k_region_dup(w32k_handle src);
int w32k_region_copy_into(w32k_handle dst, w32k_handle src);
uint32_t w32k_region_rect_count(w32k_handle h);
int w32k_region_rect_at(w32k_handle h, uint32_t i, w32k_rect *out);
uint32_t w32k_region_count_live(void);
void w32k_gdi_reset(void);
w32k_handle w32k_framebuffer(void);

#endif /* W32K_INT_H */
