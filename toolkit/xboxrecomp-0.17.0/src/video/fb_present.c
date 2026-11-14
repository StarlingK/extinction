#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "keyboard.h"
#include "gpu_present.h"
#include "recomp_exit_hook.h"
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
#define FB_MAX_W 1024

static uint32_t     *s_rgb;
static uint32_t     *s_view;
static uint32_t     *s_src;
static volatile uint32_t s_cw = 640;
static volatile LONG s_pres_ox;

void xbox_FramebufferPresentInset(int ox)
{
    InterlockedExchange(&s_pres_ox, ox > 0 && ox <= (FB_MAX_W - 640) / 2 ? ox : 0);
}

static uint32_t fb_inset(void)
{
    uint32_t cw = s_cw;
    return cw > s_fb_width ? (cw - s_fb_width) / 2 : 0;
}

typedef struct { int j0, n; uint32_t w[4]; } FbTap;
static FbTap        *s_taps;
static uint32_t      s_taps_sw, s_taps_w;

static int fb_build_taps(uint32_t sw, uint32_t w)
{
    double step, span;
    uint32_t i;

    if (s_taps && s_taps_sw == sw && s_taps_w == w)
        return 1;
    free(s_taps);
    s_taps = (FbTap *)calloc(w, sizeof *s_taps);
    s_taps_sw = s_taps_w = 0;
    if (!s_taps)
        return 0;
    step = (double)sw / (double)w;
    span = step > 1.0 ? step : 1.0;
    for (i = 0; i < w; i++) {
        double c = ((double)i + 0.5) * step, lo = c - span * 0.5, hi = c + span * 0.5;
        double acc[4] = { 0, 0, 0, 0 };
        long j, j0 = (long)floor(lo), j1 = (long)ceil(hi) - 1, base = j0 < 0 ? 0 : j0;
        int k, big = 0;
        uint32_t sum = 0;

        for (j = j0; j <= j1; j++) {
            double a = (double)j > lo ? (double)j : lo, b = (double)(j + 1) < hi ? (double)(j + 1) : hi;
            long jj = j < 0 ? 0 : j >= (long)sw ? (long)sw - 1 : j;
            if (b > a && jj - base >= 0 && jj - base < 4)
                acc[jj - base] += (b - a) / span;
        }
        s_taps[i].j0 = (int)base;
        s_taps[i].n = (j1 < (long)sw ? (int)(j1 - base + 1) : (int)((long)sw - base));
        if (s_taps[i].n < 1) s_taps[i].n = 1;
        if (s_taps[i].n > 4) s_taps[i].n = 4;
        for (k = 0; k < s_taps[i].n; k++) {
            s_taps[i].w[k] = (uint32_t)(acc[k] * 256.0 + 0.5);
            sum += s_taps[i].w[k];
            if (s_taps[i].w[k] > s_taps[i].w[big]) big = k;
        }
        s_taps[i].w[big] = (uint32_t)((int)s_taps[i].w[big] + 256 - (int)sum);
    }
    s_taps_sw = sw;
    s_taps_w = w;
    return 1;
}

static void fb_resample_row(const uint32_t *src, uint32_t *dst, uint32_t w)
{
    uint32_t x;

    for (x = 0; x < w; x++) {
        const FbTap *t = &s_taps[x];
        uint32_t rb = 0, ga = 0;
        int k;
        for (k = 0; k < t->n; k++) {
            uint32_t p = src[t->j0 + k];
            rb += (p & 0x00FF00FFu) * t->w[k];
            ga += ((p >> 8) & 0x00FF00FFu) * t->w[k];
        }
        dst[x] = (((rb + 0x00800080u) >> 8) & 0x00FF00FFu) | ((ga + 0x00800080u) & 0xFF00FF00u);
    }
}

static uint32_t *fb_view_open(uint32_t *vw)
{
    uint32_t inset = fb_inset(), cw = s_cw, y;

    *vw = cw;
    if (!inset || !s_rgb)
        return s_rgb;
    if (!s_view)
        s_view = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    if (!s_view)
        return s_rgb;
    *vw = s_fb_width;
    for (y = 0; y < s_fb_height; y++)
        memcpy(s_view + (size_t)y * s_fb_width, s_rgb + (size_t)y * cw + inset, (size_t)s_fb_width * 4);
    return s_view;
}

static void fb_view_close(uint32_t *view)
{
    uint32_t inset = fb_inset(), cw = s_cw, y;

    if (!view || view == s_rgb)
        return;
    for (y = 0; y < s_fb_height; y++)
        memcpy(s_rgb + (size_t)y * cw + inset, view + (size_t)y * s_fb_width, (size_t)s_fb_width * 4);
}

static int fb_pvideo(const uint32_t *regs, const uint8_t *source, size_t available, unsigned bank)
{
    uint32_t vw;
    uint32_t *view = fb_view_open(&vw);
    int ok = pvideo_scanout(regs, source, available, view, vw, s_fb_height, bank);

    fb_view_close(view);
    return ok;
}

static uint32_t     *s_present[2];
static uint32_t      s_present_sw[2];
static int           s_present_ox[2];
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
    int x, y, w = (int)s_cw, h = (int)s_fb_height;

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
    if (x >= 0 && y >= 0 && x < (int)s_cw && y < (int)s_fb_height)
        s_rgb[(size_t)y * s_cw + x] = c;
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
        if (!fb_pvideo(regs, snapshot, (size_t)bytes, bank)) {
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
        fb_pvideo(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_pvideo_bank);
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
    uint32_t bpp, x, y, sw;
    int ox;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;
    AcquireSRWLockExclusive(&s_present_lock);
    next = (s_present_idx == 0) ? 1 : 0;
    ox = (int)s_pres_ox;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * 2 * s_fb_height,
                                             4);
        if (!s_present[next]) {
            ReleaseSRWLockExclusive(&s_present_lock);
            fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
            return;
        }
    }
    bpp = pitch / s_fb_width;
    sw = ox > 0 && bpp == 8 ? s_fb_width * 2 : s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * sw;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8 && ox > 0) {
            memcpy(dst, row, (size_t)sw * 4);
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
    s_present_sw[next] = sw;
    s_present_ox[next] = ox;
    s_present_idx = next;
    fb_latch_box();
    ReleaseSRWLockExclusive(&s_present_lock);
    if (s_new_frame)
        SetEvent(s_new_frame);
}

static int fb_copy_presented_frame(uint32_t *dst)
{
    int copied = 0, ox = 0;
    uint32_t sw = 0, w, y;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        ox = s_present_ox[s_present_idx];
        if (!ox) {
            memcpy(dst, s_present[s_present_idx],
                   (size_t)s_fb_width * s_fb_height * 4);
        } else if (s_src) {
            sw = s_present_sw[s_present_idx];
            memcpy(s_src, s_present[s_present_idx], (size_t)sw * s_fb_height * 4);
        }
        copied = 1;
    }
    ReleaseSRWLockShared(&s_present_lock);
    if (!copied)
        return 0;
    if (!ox) {
        s_cw = s_fb_width;
        return 1;
    }
    w = s_fb_width + 2u * (uint32_t)ox;
    if (!sw || !fb_build_taps(sw, w))
        return 0;
    for (y = 0; y < s_fb_height; y++)
        fb_resample_row(s_src + (size_t)y * sw, dst + (size_t)y * w, w);
    s_cw = w;
    return 1;
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
static int s_mode;
static HWND s_fb_hwnd;

static volatile LONG s_cfg_set;
static volatile LONG s_cfg_gen;
static volatile LONG s_cfg_mode;
static volatile LONG s_cfg_w = 1280, s_cfg_h = 960;
static volatile LONG s_cfg_vsync = 1;
static volatile LONG s_cfg_cap = 60;
static LONG s_cfg_seen;
static int s_geom_force;
static LONG_PTR s_win_style = WS_OVERLAPPEDWINDOW;

void xbox_FramebufferDisplayConfig(int mode, int w, int h, int vsync, int cap)
{
    InterlockedExchange(&s_cfg_mode, mode < 0 || mode > 2 ? 0 : mode);
    InterlockedExchange(&s_cfg_w, w > 0 && h > 0 ? w : 0);
    InterlockedExchange(&s_cfg_h, w > 0 && h > 0 ? h : 0);
    InterlockedExchange(&s_cfg_vsync, vsync ? 1 : 0);
    InterlockedExchange(&s_cfg_cap, cap > 0 ? cap : 0);
    InterlockedExchange(&s_cfg_set, 1);
    InterlockedIncrement(&s_cfg_gen);
    if (s_new_frame)
        SetEvent(s_new_frame);
}

int xbox_FramebufferDisplayMode(void)
{
    return s_mode;
}

static HMONITOR fb_monitor(HWND h)
{
    POINT origin = { 0, 0 };

    return h ? MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)
             : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

static void fb_monitor_rects(HMONITOR mon, RECT *full, RECT *work)
{
    MONITORINFO mi;

    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoA(mon, &mi)) {
        *full = mi.rcMonitor;
        *work = mi.rcWork;
        return;
    }
    full->left = full->top = work->left = work->top = 0;
    full->right = work->right = 1920;
    full->bottom = work->bottom = 1080;
}

int xbox_FramebufferDesktopSize(int *w, int *h)
{
    RECT full, work;

    fb_monitor_rects(fb_monitor(s_fb_hwnd), &full, &work);
    *w = (int)(full.right - full.left);
    *h = (int)(full.bottom - full.top);
    return 1;
}

int xbox_FramebufferDisplayModes(int *wh, int max)
{
    MONITORINFOEXA mi;
    DEVMODEA dm;
    HMONITOR mon = fb_monitor(s_fb_hwnd);
    const char *dev = NULL;
    int n = 0;
    DWORD i;

    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi))
        dev = mi.szDevice;
    for (i = 0;; i++) {
        int k, w, h, dup = 0;

        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(dev, i, &dm))
            break;
        if (dm.dmBitsPerPel < 24 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480)
            continue;
        w = (int)dm.dmPelsWidth;
        h = (int)dm.dmPelsHeight;
        for (k = 0; k < n; k++)
            if (wh[k * 2] == w && wh[k * 2 + 1] == h)
                dup = 1;
        if (dup || n >= max)
            continue;
        k = n++;
        while (k > 0 && (long long)wh[(k - 1) * 2] * wh[(k - 1) * 2 + 1] > (long long)w * h) {
            wh[k * 2] = wh[(k - 1) * 2];
            wh[k * 2 + 1] = wh[(k - 1) * 2 + 1];
            k--;
        }
        wh[k * 2] = w;
        wh[k * 2 + 1] = h;
    }
    return n;
}

static void fb_window_rect(HMONITOR mon, DWORD style, int w, int h, RECT *out)
{
    RECT full, work, b;
    LONG aw, ah, cw, ch, fw = (LONG)(s_cw ? s_cw : s_fb_width);

    fb_monitor_rects(mon, &full, &work);
    b.left = b.top = b.right = b.bottom = 0;
    AdjustWindowRect(&b, style, FALSE);
    aw = (work.right - work.left) - (b.right - b.left);
    ah = (work.bottom - work.top) - (b.bottom - b.top);
    if (aw < 64) aw = 64;
    if (ah < 64) ah = 64;
    if (w > 0 && h > 0) {
        cw = w;
        ch = h;
        if (cw > aw || ch > ah) {
            if ((long long)aw * ch <= (long long)ah * cw) {
                ch = (LONG)((long long)ch * aw / cw);
                cw = aw;
            } else {
                cw = (LONG)((long long)cw * ah / ch);
                ch = ah;
            }
        }
    } else if ((long long)aw * s_fb_height <= (long long)ah * fw) {
        cw = aw;
        ch = (LONG)((long long)aw * s_fb_height / fw);
    } else {
        ch = ah;
        cw = (LONG)((long long)ah * fw / s_fb_height);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    out->left = 0; out->top = 0; out->right = cw; out->bottom = ch;
    AdjustWindowRect(out, style, FALSE);
    cw = out->right - out->left;
    ch = out->bottom - out->top;
    out->left = work.left + ((work.right - work.left) - cw) / 2;
    out->top = work.top + ((work.bottom - work.top) - ch) / 2;
    out->right = out->left + cw;
    out->bottom = out->top + ch;
}

static void fb_apply_display(HWND h, int initial)
{
    int mode = (int)s_cfg_mode, w = (int)s_cfg_w, ht = (int)s_cfg_h;
    HMONITOR mon;
    RECT r, full, work;
    UINT show = initial ? 0 : SWP_SHOWWINDOW;

    static int pm = -1, pw, ph;

    gpu_present_set_vsync((int)s_cfg_vsync);
    if (!initial && !s_geom_force && mode == pm && w == pw && ht == ph)
        return;
    pm = mode;
    pw = w;
    ph = ht;
    s_geom_force = 0;
    if (mode == 2 && !gpu_present_active())
        mode = 1;
    if (s_mode == 2 && mode != 2)
        gpu_present_set_exclusive(0, 0, 0);
    if (IsIconic(h))
        ShowWindow(h, SW_RESTORE);
    mon = fb_monitor(h);
    fb_monitor_rects(mon, &full, &work);
    if (mode == 0) {
        if (s_full)
            SetWindowLongPtrA(h, GWL_STYLE, s_win_style);
        if (IsZoomed(h))
            ShowWindow(h, SW_RESTORE);
        s_full = 0;
        fb_window_rect(mon, (DWORD)GetWindowLongPtrA(h, GWL_STYLE), w, ht, &r);
        SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | show);
    } else {
        LONG_PTR st = GetWindowLongPtrA(h, GWL_STYLE);

        if (!s_full)
            s_win_style = st;
        SetWindowLongPtrA(h, GWL_STYLE, (st & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, full.left, full.top, full.right - full.left,
                     full.bottom - full.top, SWP_FRAMECHANGED | SWP_NOACTIVATE | show);
        s_full = 1;
        if (mode == 2) {
            int dw = w > 0 ? w : (int)(full.right - full.left);
            int dh = ht > 0 ? ht : (int)(full.bottom - full.top);

            if (!gpu_present_set_exclusive(1, (uint32_t)dw, (uint32_t)dh)) {
                fprintf(stderr, "  [FBWIN] exclusive fullscreen refused; borderless instead\n");
                mode = 1;
            }
        }
    }
    s_mode = mode;
}

static void fb_toggle_fullscreen(HWND h)
{
    InterlockedExchange(&s_cfg_set, 1);
    InterlockedExchange(&s_cfg_mode, s_mode ? 0 : 1);
    s_cfg_seen = InterlockedIncrement(&s_cfg_gen);
    fb_apply_display(h, 0);
}

typedef void (*xbox_fb_draw_hook)(uint32_t *rgb, int w, int h);
typedef int (*xbox_fb_input_hook)(HWND h, UINT m, WPARAM w, LPARAM l);
static volatile xbox_fb_draw_hook s_draw_hook;
static volatile xbox_fb_input_hook s_input_hook;

void xbox_FramebufferSetDrawHook(xbox_fb_draw_hook fn) { s_draw_hook = fn; }
void xbox_FramebufferSetInputHook(xbox_fb_input_hook fn) { s_input_hook = fn; }

static void fb_run_hook(void)
{
    xbox_fb_draw_hook hook = s_draw_hook;
    uint32_t vw;
    uint32_t *view;

    if (!hook)
        return;
    view = fb_view_open(&vw);
    hook(view, (int)vw, (int)s_fb_height);
    fb_view_close(view);
}

int xbox_FramebufferIsFullscreen(void) { return s_full; }

void xbox_FramebufferSetFullscreen(HWND h, int on)
{
    if (!!on != !!s_full)
        fb_toggle_fullscreen(h);
}

void xbox_FramebufferClientToFrame(HWND h, int cx, int cy, int *fx, int *fy)
{
    RECT c;
    LONG cw, ch, w, ht, fw = (LONG)s_cw;
    GetClientRect(h, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0) { *fx = *fy = -1; return; }
    if ((long long)cw * s_fb_height <= (long long)ch * fw) {
        w = cw; ht = (LONG)((long long)cw * s_fb_height / fw);
    } else {
        ht = ch; w = (LONG)((long long)ch * fw / s_fb_height);
    }
    *fx = (int)((long long)(cx - (cw - w) / 2) * (LONG)fw / (w ? w : 1));
    *fy = (int)((long long)(cy - (ch - ht) / 2) * (LONG)s_fb_height / (ht ? ht : 1));
    *fx -= (int)fb_inset();
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
        r.right = (LONG)s_cw * scale; r.bottom = (LONG)s_fb_height * scale;
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

static volatile LONG s_wheel;
static volatile LONG s_hide_cursor;

int xbox_FramebufferMouse(int *fx, int *fy)
{
    POINT p;
    RECT c;
    LONG cw, ch, w, h, fw = (LONG)s_cw;

    if (!s_fb_hwnd || GetForegroundWindow() != s_fb_hwnd)
        return 0;
    if (!GetCursorPos(&p) || !ScreenToClient(s_fb_hwnd, &p) || !GetClientRect(s_fb_hwnd, &c))
        return 0;
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return 0;
    if ((long long)cw * s_fb_height <= (long long)ch * fw) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / fw);
    } else {
        h = ch; w = (LONG)((long long)ch * fw / s_fb_height);
    }
    if (w <= 0 || h <= 0)
        return 0;
    *fx = (int)((long long)(p.x - (cw - w) / 2) * fw / w);
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
    uint32_t fw = s_cw;
    uint32_t row = ((fw * 3u) + 3u) & ~3u;
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
    memcpy(hdr + 18, &fw, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * fw;
        for (x = 0; x < fw; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, fw, s_fb_height, s_fb_va);
    return 0;
}

static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h, fw = (LONG)s_cw;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * fw) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / fw);
    } else {
        h = ch; w = (LONG)((long long)ch * fw / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, fw, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

static int fb_show(HWND hwnd, HDC hdc, BITMAPINFO *bi)
{
    if (gpu_present_active())
        return gpu_present_frame_size(s_rgb, s_cw, s_fb_height);
    bi->bmiHeader.biWidth = (LONG)s_cw;
    gdi_blit(hwnd, hdc, bi);
    return 0;
}

static void fb_cap_wait(void)
{
    static LARGE_INTEGER qf, last;
    long long cap = (long long)s_cfg_cap, period;
    LARGE_INTEGER now;

    if (!qf.QuadPart)
        QueryPerformanceFrequency(&qf);
    if (cap < 1 || cap >= 60) {
        last.QuadPart = 0;
        return;
    }
    period = qf.QuadPart * 92 / (cap * 100);
    for (;;) {
        long long left;

        QueryPerformanceCounter(&now);
        if (!last.QuadPart || now.QuadPart - last.QuadPart >= period)
            break;
        left = (last.QuadPart + period - now.QuadPart) * 1000 / qf.QuadPart;
        if (left > 2) {
            MSG msg;

            MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(left - 1), QS_ALLINPUT);
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        } else {
            SwitchToThread();
        }
    }
    last = now;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned, background = 0;

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
        const char *bg = getenv("RECOMP_FB_BACKGROUND");
        RECT work, full;
        HMONITOR mon = fb_monitor(NULL);

        background = bg && *bg && *bg != '0';

        if (!s_cfg_set) {
            const char *e = getenv("RECOMP_WINDOW_SCALE"), *fs = getenv("RECOMP_FULLSCREEN");
            const char *vs = getenv("RECOMP_PRESENT_VSYNC");
            int scale = e ? atoi(e) : 2;

            if (scale < 1 || scale > 8)
                scale = 2;
            s_cfg_w = (LONG)s_fb_width * scale;
            s_cfg_h = (LONG)s_fb_height * scale;
            s_cfg_mode = fs && *fs && *fs != '0' ? 1 : 0;
            s_cfg_vsync = !(vs && vs[0] == '0');
        }
        fb_monitor_rects(mon, &full, &work);
        fb_window_rect(mon, WS_OVERLAPPEDWINDOW, (int)s_cfg_w, (int)s_cfg_h, &r);
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW,
                               r.left, r.top, r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (hwnd) {
            s_fb_hwnd = hwnd;
            s_cfg_seen = s_cfg_gen;
            if (background) {
                ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
            } else {
                fb_apply_display(hwnd, 1);
                ShowWindow(hwnd, SW_SHOW);
            }
        }
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)FB_MAX_W * s_fb_height, 4);
    s_src = (uint32_t *)calloc((size_t)s_fb_width * 2 * s_fb_height, 4);

    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled() && gpu_present_init(hwnd, s_fb_width, s_fb_height)
        && s_cfg_mode == 2 && !background)
        s_cfg_seen = s_cfg_gen - 1, s_geom_force = 1;

    timeBeginPeriod(1);
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0, fresh = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (s_cfg_gen != s_cfg_seen) {
            s_cfg_seen = s_cfg_gen;
            fb_apply_display(hwnd, 0);
        } else if (s_cfg_mode == 2 && s_mode == 2 && !gpu_present_is_exclusive()
                   && GetForegroundWindow() == hwnd && !IsIconic(hwnd)) {
            static DWORD retry;
            DWORD tick = GetTickCount();

            if (tick - retry > 1000) {
                retry = tick;
                s_geom_force = 1;
                fb_apply_display(hwnd, 0);
            }
        }
        if (s_present_idx >= 0 && !fb_pinned) {
            DWORD w = MsgWaitForMultipleObjects(1, &s_new_frame, FALSE, 50, QS_ALLINPUT);
            fresh = w == WAIT_OBJECT_0;
            if (!fresh && w != WAIT_TIMEOUT)
                continue;
            if (fresh)
                fb_cap_wait();
        }
        if (s_rgb && (fresh || s_present_idx >= 0) && fb_copy_presented_frame(s_rgb)) {
            static uint32_t fitted = 640;

            if (s_cw != fitted) {
                fitted = s_cw;
                if (!s_full && !s_cfg_w) {
                    s_geom_force = 1;
                    fb_apply_display(hwnd, 0);
                }
            }
            if (!fb_pinned)
                fb_overlay();
            fb_draw_box();
            fb_draw_cursor();
            fb_run_hook();
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
            s_cw = s_fb_width;
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            fb_run_hook();
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
                _snwprintf(tb, 127, L"%ls | FPS: %.1f", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0);
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
    free(s_view);
    s_view = NULL;
    free(s_src);
    s_src = NULL;
    free(s_taps);
    s_taps = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    SetProcessDPIAware();

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

