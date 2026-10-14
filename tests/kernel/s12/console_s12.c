/* console_s12.c - S12 probe for the win32k windowing spike.
 *
 * Creates a visible top-level window, posts two messages, pumps the queue with
 * PeekMessage and DispatchMessage, destroys the window, and checks the recorded
 * message order against the sequence the win32k model documents
 * (src/ntdll/win32k/w32k_msg.c, w32k_core.c). Prints one line per check and
 * exits with the number of failed checks.
 *
 * Built with the MinGW import libraries (user32, kernel32), not the dlltool
 * ones, because dlltool is blocked on the development machine. Not booted:
 * the kernel-side win32k service wiring is not in this spike, so there is no
 * S12 pass condition yet.
 *
 *   x86_64-w64-mingw32-gcc -municode -mconsole -o console_s12.exe console_s12.c -luser32 -lkernel32
 */
#include <windows.h>
#include <stdio.h>

#define LOG_MAX 64
static UINT log_msg[LOG_MAX];
static int log_n;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (log_n < LOG_MAX)
        log_msg[log_n++] = msg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        (void)dc;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static const UINT want[] = {
    WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE,
    WM_SHOWWINDOW, WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE,
    WM_ACTIVATE, WM_SETFOCUS,
    WM_USER + 1, WM_USER + 2, WM_PAINT,
    WM_KILLFOCUS, WM_ACTIVATE, WM_DESTROY, WM_NCDESTROY,
};

static int failures;

static void check(int ok, const wchar_t *what)
{
    wprintf(L"S12 %ls: %ls\n", what, ok ? L"ok" : L"FAIL");
    if (!ok)
        ++failures;
}

int wmain(void)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    size_t i, n = sizeof(want) / sizeof(want[0]);
    int pumped = 0;
    int order_ok;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.lpszClassName = L"S12Probe";
    check(RegisterClassExW(&wc) != 0, L"register class");

    hwnd = CreateWindowExW(0, L"S12Probe", L"probe", WS_VISIBLE, 10, 20, 200, 100, NULL, NULL, NULL, NULL);
    check(hwnd != NULL, L"create visible window");

    PostMessageW(hwnd, WM_USER + 1, 0, 0);
    PostMessageW(hwnd, WM_USER + 2, 0, 0);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        ++pumped;
    }
    check(pumped == 3, L"pump: two posted messages and one paint");

    DestroyWindow(hwnd);

    order_ok = (size_t)log_n == n;
    for (i = 0; order_ok && i < n; ++i)
        order_ok = log_msg[i] == want[i];
    check(order_ok, L"message order");

    PostQuitMessage(0);
    check(GetMessageW(&msg, NULL, 0, 0) == 0, L"quit message ends the loop");
    wprintf(L"S12 failures: %d\n", failures);
    return failures;
}
