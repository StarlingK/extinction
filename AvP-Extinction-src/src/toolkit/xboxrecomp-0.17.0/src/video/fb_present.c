#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "gpu_present.h"
#include "recomp_exit_hook.h"
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;

static uint32_t     *s_present[2];
static LONG          s_present_idx = -1;
static SRWLOCK       s_present_lock = SRWLOCK_INIT;

static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

static volatile LONG s_box_set[5];
static LONG s_box_prev[5], s_box_show[5];
static volatile LONG s_box_color = 0x0060FF60;

void xbox_FramebufferSetBoxColor(uint32_t rgb)
{
    InterlockedExchange(&s_box_color, (LONG)(rgb & 0x00FFFFFFu));
}

void xbox_FramebufferSetBox(int on, int x0, int y0, int x1, int y1)
{
    InterlockedExchange(&s_box_set[1], x0 < x1 ? x0 : x1);
    InterlockedExchange(&s_box_set[2], y0 < y1 ? y0 : y1);
    InterlockedExchange(&s_box_set[3], x0 < x1 ? x1 : x0);
    InterlockedExchange(&s_box_set[4], y0 < y1 ? y1 : y0);
    InterlockedExchange(&s_box_set[0], on);
}

static void fb_latch_box(void)
{
    int i;
    for (i = 0; i < 5; i++) {
        s_box_show[i] = s_box_prev[i];
        s_box_prev[i] = s_box_set[i];
    }
}

static void fb_draw_box(void)
{
    const uint32_t c = (uint32_t)s_box_color;
    int x0, y0, x1, y1, on;
    int x, y, w = (int)s_fb_width, h = (int)s_fb_height;

    AcquireSRWLockShared(&s_present_lock);
    on = (int)s_box_show[0];
    x0 = (int)s_box_show[1]; y0 = (int)s_box_show[2];
    x1 = (int)s_box_show[3]; y1 = (int)s_box_show[4];
    ReleaseSRWLockShared(&s_present_lock);
    if (!on || !s_rgb)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (x = x0; x <= x1; x++) {
        s_rgb[(size_t)y0 * w + x] = c;
        s_rgb[(size_t)y1 * w + x] = c;
    }
    for (y = y0; y <= y1; y++) {
        s_rgb[(size_t)y * w + x0] = c;
        s_rgb[(size_t)y * w + x1] = c;
    }
}

static volatile LONG s_cur_on, s_cur_x, s_cur_y, s_cur_color = 0x00FFFFFF;

void xbox_FramebufferSetCursor(int on, int x, int y, uint32_t rgb)
{
    InterlockedExchange(&s_cur_x, x);
    InterlockedExchange(&s_cur_y, y);
    InterlockedExchange(&s_cur_color, (LONG)(rgb & 0x00FFFFFFu));
    InterlockedExchange(&s_cur_on, on);
}

static void fb_put(int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < (int)s_fb_width && y < (int)s_fb_height)
        s_rgb[(size_t)y * s_fb_width + x] = c;
}

static void fb_draw_cursor(void)
{
    const int arm = 7, gap = 2;
    int cx = (int)s_cur_x, cy = (int)s_cur_y, i, pass;
    uint32_t c = (uint32_t)s_cur_color;

    if (!s_cur_on || !s_rgb)
        return;

    for (pass = 0; pass < 2; pass++) {
        uint32_t col = pass ? c : 0x00101010u;
        int w = pass ? 0 : 1;
        for (i = gap; i <= arm; i++) {
            int d;
            for (d = -w; d <= w; d++) {
                int e;
                for (e = -w; e <= w; e++) {
                    fb_put(cx + i + e, cy + d, col);
                    fb_put(cx - i + e, cy + d, col);
                    fb_put(cx + d, cy + i + e, col);
                    fb_put(cx + d, cy - i + e, col);
                }
            }
        }
        for (i = -w; i <= w; i++) {
            int j;
            for (j = -w; j <= w; j++)
                fb_put(cx + i, cy + j, col);
        }
    }
}

static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{

    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

static HANDLE s_new_frame;

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;
    AcquireSRWLockExclusive(&s_present_lock);
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next]) {
            ReleaseSRWLockExclusive(&s_present_lock);
            fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
            return;
        }
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {

            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_present_idx = next;
    fb_latch_box();
    ReleaseSRWLockExclusive(&s_present_lock);
    if (s_new_frame)
        SetEvent(s_new_frame);
}

static int fb_copy_presented_frame(uint32_t *dst)
{
    int copied = 0;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        memcpy(dst, s_present[s_present_idx],
               (size_t)s_fb_width * s_fb_height * 4);
        copied = 1;
    }
    ReleaseSRWLockShared(&s_present_lock);
    return copied;
}

static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

static int s_full;

static void fb_toggle_fullscreen(HWND h)
{
#define full s_full
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
#undef full
}

typedef void (*xbox_fb_draw_hook)(uint32_t *rgb, int w, int h);
typedef int (*xbox_fb_input_hook)(HWND h, UINT m, WPARAM w, LPARAM l);
static volatile xbox_fb_draw_hook s_draw_hook;
static volatile xbox_fb_input_hook s_input_hook;

void xbox_FramebufferSetDrawHook(xbox_fb_draw_hook fn) { s_draw_hook = fn; }
void xbox_FramebufferSetInputHook(xbox_fb_input_hook fn) { s_input_hook = fn; }

int xbox_FramebufferIsFullscreen(void) { return s_full; }

void xbox_FramebufferSetFullscreen(HWND h, int on)
{
    if (!!on != !!s_full)
        fb_toggle_fullscreen(h);
}

void xbox_FramebufferClientToFrame(HWND h, int cx, int cy, int *fx, int *fy)
{
    RECT c;
    LONG cw, ch, w, ht;
    GetClientRect(h, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0) { *fx = *fy = -1; return; }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; ht = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        ht = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    *fx = (int)((long long)(cx - (cw - w) / 2) * (LONG)s_fb_width / (w ? w : 1));
    *fy = (int)((long long)(cy - (ch - ht) / 2) * (LONG)s_fb_height / (ht ? ht : 1));
}

void xbox_FramebufferSetWindowScale(HWND h, int scale)
{
    RECT r, work;
    if (s_full || scale < 1 || scale > 8)
        return;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
    }
    for (;; scale--) {
        r.left = 0; r.top = 0;
        r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
        AdjustWindowRect(&r, (DWORD)GetWindowLongPtrA(h, GWL_STYLE), FALSE);
        if (scale <= 1 || (r.right - r.left <= work.right - work.left
                           && r.bottom - r.top <= work.bottom - work.top))
            break;
    }
    SetWindowPos(h, NULL,
                 work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                 work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                 r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_SHOWWINDOW);
}

static HWND          s_fb_hwnd;
static volatile LONG s_wheel;
static volatile LONG s_hide_cursor;

int xbox_FramebufferMouse(int *fx, int *fy)
{
    POINT p;
    RECT c;
    LONG cw, ch, w, h;

    if (!s_fb_hwnd || GetForegroundWindow() != s_fb_hwnd)
        return 0;
    if (!GetCursorPos(&p) || !ScreenToClient(s_fb_hwnd, &p) || !GetClientRect(s_fb_hwnd, &c))
        return 0;
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return 0;
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    if (w <= 0 || h <= 0)
        return 0;
    *fx = (int)((long long)(p.x - (cw - w) / 2) * (LONG)s_fb_width / w);
    *fy = (int)((long long)(p.y - (ch - h) / 2) * (LONG)s_fb_height / h);
    return 1;
}

int xbox_FramebufferMouseWheel(void)
{
    static LONG rest;
    LONG v = InterlockedExchange(&s_wheel, 0) + rest;
    int n = (int)(v / WHEEL_DELTA);
    rest = v - n * WHEEL_DELTA;
    return n;
}

void xbox_FramebufferHideCursor(int hide)
{
    InterlockedExchange(&s_hide_cursor, hide ? 1 : 0);
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEWHEEL) {
        InterlockedExchangeAdd(&s_wheel, GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    if (m == WM_SETCURSOR && s_hide_cursor && LOWORD(l) == HTCLIENT) {
        SetCursor(NULL);
        return TRUE;
    }

    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;
    if (s_input_hook
        && ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST))
        && s_input_hook(h, m, w, l)) {
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN)
            SetCapture(h);
        else if (m == WM_LBUTTONUP || m == WM_RBUTTONUP)
            ReleaseCapture();
        return m == WM_SYSKEYDOWN || m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    }
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:

        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);

        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }

        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }

        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);

            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {

            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (gpu_present_active())
        return gpu_present_frame(s_rgb);
    gdi_blit(hwnd, hdc, bi);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {

        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }

        const char *bg = getenv("RECOMP_FB_BACKGROUND");
        int background = bg && *bg && *bg != '0';
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | (background ? 0 : WS_VISIBLE),
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (hwnd && background)
            ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    s_fb_hwnd = hwnd;
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    timeBeginPeriod(1);
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0, fresh = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (s_present_idx >= 0 && !fb_pinned) {
            DWORD w = MsgWaitForMultipleObjects(1, &s_new_frame, FALSE, 50, QS_ALLINPUT);
            fresh = w == WAIT_OBJECT_0;
            if (!fresh && w != WAIT_TIMEOUT)
                continue;
        }
        if (s_rgb && (fresh || s_present_idx >= 0) && fb_copy_presented_frame(s_rgb)) {

            if (!fb_pinned)
                fb_overlay();
            fb_draw_box();
            fb_draw_cursor();
            if (s_draw_hook)
                s_draw_hook(s_rgb, (int)s_fb_width, (int)s_fb_height);
            paced = fb_show(hwnd, hdc, &bi);

            if (fresh) {
                static int stats = -1;
                static LARGE_INTEGER last, t0, qf;
                static unsigned hist[6], n;
                LARGE_INTEGER now;
                if (stats < 0) {
                    stats = getenv("RECOMP_PRESENT_STATS") != NULL;
                    QueryPerformanceFrequency(&qf);
                }
                QueryPerformanceCounter(&now);
                if (stats && last.QuadPart) {
                    double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)qf.QuadPart;
                    hist[ms < 12 ? 0 : ms < 15.5 ? 1 : ms < 18 ? 2 : ms < 25 ? 3 : ms < 40 ? 4 : 5]++;
                    n++;
                    if (!t0.QuadPart)
                        t0 = now;
                    if ((double)(now.QuadPart - t0.QuadPart) / (double)qf.QuadPart >= 10.0) {
                        fprintf(stderr, "  [FBWIN] %u frames on screen; gaps <12ms %u, 12-15 %u,"
                                        " 15-18 %u, 18-25 %u, 25-40 %u, >40 %u\n",
                                n, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
                        memset(hist, 0, sizeof hist);
                        n = 0;
                        t0 = now;
                    }
                }
                last = now;
            }
        } else if (s_fb_va && s_fb_pitch && s_rgb) {

            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            if (s_draw_hook)
                s_draw_hook(s_rgb, (int)s_fb_width, (int)s_fb_height);
            paced = fb_show(hwnd, hdc, &bi);
        }
        {

            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced && s_present_idx < 0)
            Sleep(16);
    }
    timeEndPeriod(1);

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    if (!s_new_frame)
        s_new_frame = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif

