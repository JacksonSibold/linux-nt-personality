/*
 * console_s12.c - S12 of the console milestone: the win32k windowing layer.
 *
 * Freestanding PE (no CRT), linked against the user32, gdi32 and kernel32 import
 * libraries built from the .def files in src/ntdll. The step (tools/linux-env/step-initramfs.sh)
 * runs it with ntdll, kernel32, user32 and gdi32 under /windows/system32. Every
 * expected value below is written in this source; nothing is read from a recorded
 * output. Each failed check adds one to the exit status (the number of failed
 * checks); 0 means the step passes.
 *
 *  - a visible top-level window is created
 *  - two posted messages and one WM_PAINT are pumped with PeekMessage, and the
 *    window-procedure order matches the sequence written below
 *  - GetMessage blocks on an empty queue until a 10 ms timer is due, and returns
 *    WM_TIMER (the kernel wait path, not a -1 / 1460 failure)
 *  - a 4x4 32 bpp DIB section: SetPixel writes one colour, GetPixel reads it back,
 *    and the section's bits contain that colour exactly once
 */

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef unsigned short WCHAR;
typedef int BOOL;
typedef long long LONG_PTR;
typedef unsigned long long UINT_PTR;
typedef unsigned int UINT;

typedef struct {
    HANDLE hwnd;
    UINT message;
    UINT_PTR wParam;
    LONG_PTR lParam;
    DWORD time;
    long x;
    long y;
} MSG_S12;

typedef LONG_PTR (*WNDPROC_S12)(HANDLE, UINT, UINT_PTR, LONG_PTR);

typedef struct {
    UINT cbSize;
    UINT style;
    WNDPROC_S12 lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HANDLE hInstance;
    HANDLE hIcon;
    HANDLE hCursor;
    HANDLE hbrBackground;
    const WCHAR *lpszMenuName;
    const WCHAR *lpszClassName;
    HANDLE hIconSm;
} WNDCLASSEXW_S12;

typedef struct {
    unsigned int biSize;
    int biWidth;
    int biHeight;
    unsigned short biPlanes;
    unsigned short biBitCount;
    unsigned int biCompression;
    unsigned int biSizeImage;
    int biXPelsPerMeter;
    int biYPelsPerMeter;
    unsigned int biClrUsed;
    unsigned int biClrImportant;
} BITMAPINFOHEADER_S12;

typedef struct {
    HANDLE hdc;
    BOOL fErase;
    long left, top, right, bottom;
    BOOL fRestore;
    BOOL fIncUpdate;
    unsigned char rgbReserved[32];
} PAINTSTRUCT_S12;

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void ExitProcess(unsigned int code);

__declspec(dllimport) UINT_PTR RegisterClassExW(const WNDCLASSEXW_S12 *wc);
__declspec(dllimport) HANDLE CreateWindowExW(DWORD ex, const WCHAR *cls, const WCHAR *name, DWORD style,
                                             int x, int y, int w, int h, HANDLE parent, HANDLE menu,
                                             HANDLE inst, void *param);
__declspec(dllimport) BOOL DestroyWindow(HANDLE hwnd);
__declspec(dllimport) BOOL PostMessageW(HANDLE hwnd, UINT msg, UINT_PTR wp, LONG_PTR lp);
__declspec(dllimport) BOOL PeekMessageW(MSG_S12 *msg, HANDLE hwnd, UINT min, UINT max, UINT flags);
__declspec(dllimport) int GetMessageW(MSG_S12 *msg, HANDLE hwnd, UINT min, UINT max);
__declspec(dllimport) BOOL TranslateMessage(const MSG_S12 *msg);
__declspec(dllimport) LONG_PTR DispatchMessageW(const MSG_S12 *msg);
__declspec(dllimport) void PostQuitMessage(int code);
__declspec(dllimport) LONG_PTR DefWindowProcW(HANDLE hwnd, UINT msg, UINT_PTR wp, LONG_PTR lp);
__declspec(dllimport) HANDLE BeginPaint(HANDLE hwnd, PAINTSTRUCT_S12 *ps);
__declspec(dllimport) BOOL EndPaint(HANDLE hwnd, const PAINTSTRUCT_S12 *ps);
__declspec(dllimport) UINT_PTR SetTimer(HANDLE hwnd, UINT_PTR id, UINT elapse, void *proc);
__declspec(dllimport) BOOL KillTimer(HANDLE hwnd, UINT_PTR id);

__declspec(dllimport) HANDLE CreateCompatibleDC(HANDLE dc);
__declspec(dllimport) BOOL DeleteDC(HANDLE dc);
__declspec(dllimport) HANDLE CreateDIBSection(HANDLE dc, const void *bmi, UINT usage, void **bits, void *section,
                                              DWORD offset);
__declspec(dllimport) HANDLE SelectObject(HANDLE dc, HANDLE obj);
__declspec(dllimport) BOOL DeleteObject(HANDLE obj);
__declspec(dllimport) UINT SetPixel(HANDLE dc, int x, int y, UINT color);
__declspec(dllimport) UINT GetPixel(HANDLE dc, int x, int y);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WS_VISIBLE 0x10000000u
#define DIB_RGB_COLORS 0u
#define WAIT_FAILED_S12 ((int)-1)

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_SHOWWINDOW 0x0018u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_PAINT 0x000Fu
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_TIMER 0x0113u
#define WM_USER 0x0400u

#define LOG_MAX 64
static UINT log_msg[LOG_MAX];
static int log_n;
static int failures;
static HANDLE out;

static const UINT want[] = {
    WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE,
    WM_SHOWWINDOW, WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE,
    WM_ACTIVATE, WM_SETFOCUS,
    WM_USER + 1, WM_USER + 2, WM_PAINT,
    WM_KILLFOCUS, WM_ACTIVATE, WM_DESTROY, WM_NCDESTROY,
};
#define WANT_N (sizeof(want) / sizeof(want[0]))

static const WCHAR class_name[] = {'S', '1', '2', 'P', 'r', 'o', 'b', 'e', 0};
static const WCHAR window_name[] = {'p', 'r', 'o', 'b', 'e', 0};

static void put(const char *s)
{
    DWORD n = 0, w = 0;
    while (s[n])
        ++n;
    WriteFile(out, s, n, &w, 0);
}

static void put_num(unsigned long v)
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    if (v == 0)
        buf[--i] = '0';
    while (v) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    put(&buf[i]);
}

static void check(int ok, const char *what)
{
    put("S12 ");
    put(what);
    put(ok ? ": ok\n" : ": FAIL\n");
    if (!ok)
        ++failures;
}

static LONG_PTR wnd_proc(HANDLE hwnd, UINT msg, UINT_PTR wp, LONG_PTR lp)
{
    if (log_n < LOG_MAX)
        log_msg[log_n++] = msg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT_S12 ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int main_s12(void)
{
    WNDCLASSEXW_S12 wc;
    MSG_S12 msg;
    HANDLE hwnd;
    DWORD pumped = 0;
    BOOL order_ok;
    UINT_PTR timer_id;
    UINT_PTR i;
    int got;
    BITMAPINFOHEADER_S12 bmi;
    HANDLE dc, bmp;
    unsigned int *bits = 0;
    unsigned int matches = 0;
    unsigned int px;
    /* SetPixel takes a COLORREF (0x00BBGGRR). A 32 bpp DIB word is 0x00RRGGBB, so the
     * same colour is stored in the section bits as 0x00996633 (Win32 DIB layout). */
    const unsigned int colour = 0x00336699u;
    const unsigned int dib_word = 0x00996633u;

    out = GetStdHandle(STD_OUTPUT_HANDLE);

    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = wnd_proc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = 0;
    wc.hIcon = 0;
    wc.hCursor = 0;
    wc.hbrBackground = 0;
    wc.lpszMenuName = 0;
    wc.lpszClassName = class_name;
    wc.hIconSm = 0;
    check(RegisterClassExW(&wc) != 0, "register class");

    hwnd = CreateWindowExW(0, class_name, window_name, WS_VISIBLE, 10, 20, 200, 100, 0, 0, 0, 0);
    check(hwnd != 0, "create visible window");

    PostMessageW(hwnd, WM_USER + 1, 0, 0);
    PostMessageW(hwnd, WM_USER + 2, 0, 0);
    while (PeekMessageW(&msg, 0, 0, 0, 1)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        ++pumped;
    }
    check(pumped == 3, "pump: two posted messages and one paint");

    timer_id = SetTimer(hwnd, 1, 10, 0);
    check(timer_id != 0, "set 10 ms timer");
    got = GetMessageW(&msg, 0, 0, 0);
    check(got == 1 && msg.message == WM_TIMER && msg.wParam == 1, "GetMessage blocks until the timer is due");
    KillTimer(hwnd, 1);

    DestroyWindow(hwnd);
    order_ok = log_n == (int)WANT_N;
    for (i = 0; order_ok && i < WANT_N; ++i)
        order_ok = log_msg[i] == want[i];
    check(order_ok, "message order");

    /* DIB pixel readback. BITMAPINFOHEADER: 4x4, 32 bpp, bottom-up, uncompressed. */
    bmi.biSize = sizeof(bmi);
    bmi.biWidth = 4;
    bmi.biHeight = 4;
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = 0;
    bmi.biSizeImage = 0;
    bmi.biXPelsPerMeter = 0;
    bmi.biYPelsPerMeter = 0;
    bmi.biClrUsed = 0;
    bmi.biClrImportant = 0;
    dc = CreateCompatibleDC(0);
    bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&bits, 0, 0) : 0;
    check(bmp != 0 && bits != 0, "create 4x4 32 bpp DIB section");
    if (bmp)
        SelectObject(dc, bmp);
    SetPixel(dc, 1, 2, colour);
    px = GetPixel(dc, 1, 2);
    check(px == colour, "GetPixel reads back the colour SetPixel wrote");
    if (bits) {
        for (i = 0; i < 16; ++i)
            if (bits[i] == dib_word)
                ++matches;
    }
    check(matches == 1, "DIB bits hold the colour exactly once");
    if (dc)
        DeleteDC(dc);

    PostQuitMessage(0);
    check(GetMessageW(&msg, 0, 0, 0) == 0, "quit message ends the loop");

    put("S12 failures: ");
    put_num((unsigned long)failures);
    put("\n");
    return failures;
}

void _start(void)
{
    ExitProcess((unsigned int)main_s12());
}
