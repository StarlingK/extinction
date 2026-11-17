#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include "plat.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 1

typedef HGLRC (WINAPI *CreateCtxFn)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *SwapFn)(int);

static HWND s_hwnd;
static HDC s_dc;
static HGLRC s_rc;
static HMODULE s_gl;
static int s_w, s_h;
static PlatEvent s_q[256];
static int s_qh, s_qt;
static LARGE_INTEGER s_freq, s_t0;

static void push(int type, int a, int b, float f)
{
    int n = (s_qt + 1) & 255;
    if (n == s_qh)
        return;
    s_q[s_qt].type = type;
    s_q[s_qt].a = a;
    s_q[s_qt].b = b;
    s_q[s_qt].f = f;
    s_qt = n;
}

static int map_key(WPARAM w)
{
    if (w >= 'A' && w <= 'Z')
        return PK_A + (int)(w - 'A');
    if (w >= '0' && w <= '9')
        return PK_A + 26 + (int)(w - '0');
    if (w >= VK_F1 && w <= VK_F12)
        return PK_F1 + (int)(w - VK_F1);
    switch (w) {
    case VK_TAB: return PK_TAB;
    case VK_LEFT: return PK_LEFT;
    case VK_RIGHT: return PK_RIGHT;
    case VK_UP: return PK_UP;
    case VK_DOWN: return PK_DOWN;
    case VK_PRIOR: return PK_PGUP;
    case VK_NEXT: return PK_PGDN;
    case VK_HOME: return PK_HOME;
    case VK_END: return PK_END;
    case VK_INSERT: return PK_INSERT;
    case VK_DELETE: return PK_DELETE;
    case VK_BACK: return PK_BACKSPACE;
    case VK_SPACE: return PK_SPACE;
    case VK_RETURN: return PK_ENTER;
    case VK_ESCAPE: return PK_ESCAPE;
    case VK_SHIFT: return PK_LSHIFT;
    case VK_CONTROL: return PK_LCTRL;
    case VK_MENU: return PK_LALT;
    }
    return PK_NONE;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        push(PE_CLOSE, 0, 0, 0);
        return 0;
    case WM_SIZE:
        s_w = LOWORD(l);
        s_h = HIWORD(l);
        push(PE_RESIZE, s_w, s_h, 0);
        return 0;
    case WM_KILLFOCUS:
        push(PE_FOCUS_LOST, 0, 0, 0);
        return 0;
    case WM_MOUSEMOVE:
        push(PE_MOUSE_MOVE, GET_X_LPARAM(l), GET_Y_LPARAM(l), 0);
        return 0;
    case WM_LBUTTONDOWN: SetCapture(h); push(PE_MOUSE_BTN, 0, 1, 0); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); push(PE_MOUSE_BTN, 0, 0, 0); return 0;
    case WM_RBUTTONDOWN: SetCapture(h); push(PE_MOUSE_BTN, 1, 1, 0); return 0;
    case WM_RBUTTONUP: ReleaseCapture(); push(PE_MOUSE_BTN, 1, 0, 0); return 0;
    case WM_MBUTTONDOWN: SetCapture(h); push(PE_MOUSE_BTN, 2, 1, 0); return 0;
    case WM_MBUTTONUP: ReleaseCapture(); push(PE_MOUSE_BTN, 2, 0, 0); return 0;
    case WM_MOUSEWHEEL:
        push(PE_WHEEL, 0, 0, (float)GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        push(PE_KEY, map_key(w), 1, 0);
        if (m == WM_SYSKEYDOWN)
            return 0;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        push(PE_KEY, map_key(w), 0, 0);
        return 0;
    case WM_CHAR:
        if (w >= 32 && w < 127)
            push(PE_CHAR, (int)w, 0, 0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int plat_open(const char *title, int w, int h)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC tmp;
    CreateCtxFn create;
    SwapFn swap;
    RECT r = { 0, 0, w, h };
    int ctx_attr[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    int pf;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "NostromoEditorWnd";
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
        CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!s_hwnd)
        return 0;
    s_dc = GetDC(s_hwnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pf = ChoosePixelFormat(s_dc, &pfd);
    if (!pf || !SetPixelFormat(s_dc, pf, &pfd))
        return 0;
    s_gl = LoadLibraryA("opengl32.dll");
    tmp = wglCreateContext(s_dc);
    if (!tmp || !wglMakeCurrent(s_dc, tmp))
        return 0;
    create = (CreateCtxFn)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create)
        return 0;
    s_rc = create(s_dc, NULL, ctx_attr);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!s_rc || !wglMakeCurrent(s_dc, s_rc))
        return 0;
    swap = (SwapFn)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap)
        swap(1);
    GetClientRect(s_hwnd, &r);
    s_w = r.right;
    s_h = r.bottom;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_t0);
    return 1;
}

int plat_poll(PlatEvent *e)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    memset(e, 0, sizeof *e);
    if (s_qh == s_qt)
        return 0;
    *e = s_q[s_qh];
    s_qh = (s_qh + 1) & 255;
    return 1;
}

void plat_swap(void)
{
    SwapBuffers(s_dc);
}

void plat_close(void)
{
    if (s_rc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(s_rc);
    }
    if (s_hwnd)
        DestroyWindow(s_hwnd);
    s_rc = NULL;
    s_hwnd = NULL;
}

void *plat_getproc(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1)
        p = (void *)GetProcAddress(s_gl, name);
    return p;
}

void plat_size(int *w, int *h)
{
    *w = s_w;
    *h = s_h;
}

double plat_time(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - s_t0.QuadPart) / (double)s_freq.QuadPart;
}

void plat_sleep_ms(int ms)
{
    Sleep((DWORD)ms);
}

int plat_exe_dir(char *out, int cap)
{
    char *s;
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (k == 0 || k >= (DWORD)cap)
        return 0;
    s = strrchr(out, '\\');
    if (!s)
        return 0;
    *s = 0;
    return 1;
}

int plat_launch(const char *exe, const char *arg1, const char *arg2)
{
    char cmd[1200];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char dir[1100];
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    snprintf(cmd, sizeof cmd, "\"%s\" %s %s", exe, arg1, arg2);
    snprintf(dir, sizeof dir, "%s", exe);
    {
        char *s = strrchr(dir, '\\');
        if (s)
            *s = 0;
    }
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

void plat_set_title(const char *t)
{
    if (s_hwnd)
        SetWindowTextA(s_hwnd, t);
}
