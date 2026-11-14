#if !defined(_WIN32)

#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/resource.h>
#include <time.h>

#include "video_player.h"
#include "keyboard_sdl.h"
#include "xinput_xbox.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define FB_W 640u
#define FB_H 480u
#define FB_MAXW 1024u

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t bpp;
    int      ox;
};

static volatile int s_pres_ox;

#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;
static Uint32          s_frame_event;
static volatile int    s_wake_pending;
static volatile int    s_in_present;
static volatile uint32_t s_aa_sy = 1;
static volatile uint32_t s_fb_va, s_fb_pitch;

static uint64_t s_flips;
static uint32_t s_frame_draws;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

void xbox_FramebufferWindowSetAA(uint32_t sy) { s_aa_sy = sy; }

void xbox_FramebufferWindowStart(void) {}

static void publish(uint32_t bpp)
{
    s_slot[s_back].bpp = bpp;
    s_slot[s_back].ox = __atomic_load_n(&s_pres_ox, __ATOMIC_RELAXED);
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);

    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;

        if (SDL_PushEvent(&ev) <= 0)
            __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
    }
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    struct fb_slot *s = &s_slot[s_back];
    const uint8_t *src;
    uint32_t bpp = pitch / FB_W, y, x, sy = s_aa_sy ? s_aa_sy : 1;
    size_t need;

    if (bpp != 2 && bpp != 4 && bpp != 8)
        return;
    need = (size_t)FB_W * FB_H * (bpp == 2 ? 2 : 4);
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < FB_H; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? sy : 1);
        if (bpp == 8) {
            uint32_t *d = (uint32_t *)(s->px + (size_t)y * FB_W * 4);
            for (x = 0; x < FB_W; x++) {
                uint32_t a, b;
                memcpy(&a, row + (size_t)x * 8, 4);
                memcpy(&b, row + (size_t)x * 8 + 4, 4);
                d[x] = (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) >> 1 & 0x00FF00FFu)
                     | (((a & 0x0000FF00u) + (b & 0x0000FF00u)) >> 1 & 0x0000FF00u);
            }
        } else {
            memcpy(s->px + (size_t)y * FB_W * bpp, row, (size_t)FB_W * bpp);
        }
    }
    publish(bpp == 8 ? 4 : bpp);
}

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{

    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

typedef void (*xbox_fb_draw_hook)(uint32_t *rgb, int w, int h);
typedef int (*xbox_fb_input_hook)(void *h, unsigned m, uintptr_t w, intptr_t l);

#define AVPMSG_KEYDOWN     0x0100u
#define AVPMSG_KEYUP       0x0101u
#define AVPMSG_CHAR        0x0102u
#define AVPMSG_MOUSEMOVE   0x0200u
#define AVPMSG_LBUTTONDOWN 0x0201u
#define AVPMSG_LBUTTONUP   0x0202u
#define AVPMSG_RBUTTONDOWN 0x0204u
#define AVPMSG_RBUTTONUP   0x0205u
#define AVPMSG_MBUTTONDOWN 0x0207u
#define AVPMSG_MBUTTONUP   0x0208u

static volatile xbox_fb_draw_hook s_draw_hook;
static volatile xbox_fb_input_hook s_input_hook;

void xbox_FramebufferSetDrawHook(xbox_fb_draw_hook fn) { s_draw_hook = fn; }
void xbox_FramebufferSetInputHook(xbox_fb_input_hook fn) { s_input_hook = fn; }

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static volatile int s_mouse_x, s_mouse_y, s_mouse_focus;
static volatile int s_wheel_acc;
static volatile int s_hide_cursor, s_cursor_hidden;

static volatile int s_box_set[5];
static int s_box_prev[5], s_box_show[5];
static volatile uint32_t s_box_color = 0x0060FF60u;
static volatile int s_cur_on, s_cur_x, s_cur_y;
static volatile uint32_t s_cur_color = 0x00FFFFFFu;

static uint32_t s_rgb[FB_MAXW * FB_H];
static uint32_t s_base[FB_W * FB_H];
static uint32_t s_view[FB_W * FB_H];
static volatile int s_cw = (int)FB_W;

void xbox_FramebufferPresentInset(int ox)
{
    __atomic_store_n(&s_pres_ox, ox > 0 && ox <= (int)(FB_MAXW - FB_W) / 2 ? ox : 0, __ATOMIC_RELAXED);
}

static int fb_inset(void)
{
    int cw = s_cw;
    return cw > (int)FB_W ? (cw - (int)FB_W) / 2 : 0;
}

typedef struct { int j0, n; uint32_t w[4]; } FbTap;
static FbTap s_taps[FB_MAXW];
static int s_taps_w;

static void fb_build_taps(int w)
{
    double step = (double)FB_W / (double)w, span = step > 1.0 ? step : 1.0;
    int i;

    if (s_taps_w == w)
        return;
    memset(s_taps, 0, sizeof s_taps);
    for (i = 0; i < w; i++) {
        double c = ((double)i + 0.5) * step, lo = c - span * 0.5, hi = c + span * 0.5;
        double acc[4] = { 0, 0, 0, 0 };
        long j, j0 = (long)floor(lo), j1 = (long)ceil(hi) - 1, base = j0 < 0 ? 0 : j0;
        int k, big = 0;
        uint32_t sum = 0;

        for (j = j0; j <= j1; j++) {
            double a = (double)j > lo ? (double)j : lo, b = (double)(j + 1) < hi ? (double)(j + 1) : hi;
            long jj = j < 0 ? 0 : j >= (long)FB_W ? (long)FB_W - 1 : j;
            if (b > a && jj - base >= 0 && jj - base < 4)
                acc[jj - base] += (b - a) / span;
        }
        s_taps[i].j0 = (int)base;
        s_taps[i].n = j1 < (long)FB_W ? (int)(j1 - base + 1) : (int)((long)FB_W - base);
        if (s_taps[i].n < 1) s_taps[i].n = 1;
        if (s_taps[i].n > 4) s_taps[i].n = 4;
        for (k = 0; k < s_taps[i].n; k++) {
            s_taps[i].w[k] = (uint32_t)(acc[k] * 256.0 + 0.5);
            sum += s_taps[i].w[k];
            if (s_taps[i].w[k] > s_taps[i].w[big]) big = k;
        }
        s_taps[i].w[big] = (uint32_t)((int)s_taps[i].w[big] + 256 - (int)sum);
    }
    s_taps_w = w;
}

static void fb_resample_row(const uint32_t *src, uint32_t *dst, int w)
{
    int x;

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

void xbox_FramebufferSetBoxColor(uint32_t rgb)
{
    __atomic_store_n(&s_box_color, rgb & 0x00FFFFFFu, __ATOMIC_RELAXED);
}

void xbox_FramebufferSetBox(int on, int x0, int y0, int x1, int y1)
{
    __atomic_store_n(&s_box_set[1], x0 < x1 ? x0 : x1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_box_set[2], y0 < y1 ? y0 : y1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_box_set[3], x0 < x1 ? x1 : x0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_box_set[4], y0 < y1 ? y1 : y0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_box_set[0], on, __ATOMIC_RELEASE);
}

void xbox_FramebufferSetCursor(int on, int x, int y, uint32_t rgb)
{
    __atomic_store_n(&s_cur_x, x, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cur_y, y, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cur_color, rgb & 0x00FFFFFFu, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cur_on, on, __ATOMIC_RELEASE);
}

void xbox_FramebufferHideCursor(int hide)
{
    __atomic_store_n(&s_hide_cursor, hide ? 1 : 0, __ATOMIC_RELAXED);
}

int xbox_FramebufferMouseWheel(void)
{
    static int rest;
    int v = __atomic_exchange_n(&s_wheel_acc, 0, __ATOMIC_ACQ_REL) + rest;
    int n = v / 120;
    rest = v - n * 120;
    return n;
}

int xbox_FramebufferMouse(int *fx, int *fy)
{
    if (!__atomic_load_n(&s_mouse_focus, __ATOMIC_ACQUIRE))
        return 0;
    *fx = __atomic_load_n(&s_mouse_x, __ATOMIC_RELAXED);
    *fy = __atomic_load_n(&s_mouse_y, __ATOMIC_RELAXED);
    return 1;
}

static volatile int s_cfg_gen;
static volatile int s_cfg_mode;
static volatile int s_cfg_w = 1280, s_cfg_h = 960;
static volatile int s_cfg_vsync = 1;
static volatile int s_cfg_cap = 60;
static volatile int s_cfg_set;
static int s_cfg_seen;
static int s_geom_force;

void xbox_FramebufferDisplayConfig(int mode, int w, int h, int vsync, int cap)
{
    __atomic_store_n(&s_cfg_mode, mode < 0 || mode > 2 ? 0 : mode, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_w, w > 0 && h > 0 ? w : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_h, w > 0 && h > 0 ? h : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_vsync, vsync ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_cap, cap > 0 ? cap : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_set, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_cfg_gen, 1, __ATOMIC_RELEASE);
}

int xbox_FramebufferDisplayMode(void)
{
    Uint32 fl;

    if (!s_win)
        return 0;
    fl = SDL_GetWindowFlags(s_win);
    if ((fl & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
        return 1;
    return (fl & SDL_WINDOW_FULLSCREEN) ? 2 : 0;
}

int xbox_FramebufferIsFullscreen(void)
{
    return xbox_FramebufferDisplayMode() != 0;
}

void xbox_FramebufferSetFullscreen(void *h, int on)
{
    (void)h;
    if (!s_win || !!on == !!xbox_FramebufferIsFullscreen())
        return;
    __atomic_store_n(&s_cfg_mode, on ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_set, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_cfg_gen, 1, __ATOMIC_RELEASE);
}

int xbox_FramebufferDesktopSize(int *w, int *h)
{
    SDL_DisplayMode dm;
    int disp = s_win ? SDL_GetWindowDisplayIndex(s_win) : 0;

    if (disp < 0 || SDL_GetDesktopDisplayMode(disp, &dm) != 0)
        return 0;
    *w = dm.w;
    *h = dm.h;
    return 1;
}

int xbox_FramebufferDisplayModes(int *wh, int max)
{
    int disp = s_win ? SDL_GetWindowDisplayIndex(s_win) : 0, count, i, n = 0;

    if (disp < 0)
        disp = 0;
    count = SDL_GetNumDisplayModes(disp);
    for (i = 0; i < count; i++) {
        SDL_DisplayMode dm;
        int k, dup = 0;

        if (SDL_GetDisplayMode(disp, i, &dm) != 0 || dm.w < 640 || dm.h < 480
            || SDL_BITSPERPIXEL(dm.format) < 24)
            continue;
        for (k = 0; k < n; k++)
            if (wh[k * 2] == dm.w && wh[k * 2 + 1] == dm.h)
                dup = 1;
        if (dup || n >= max)
            continue;
        k = n++;
        while (k > 0 && (long long)wh[(k - 1) * 2] * wh[(k - 1) * 2 + 1] > (long long)dm.w * dm.h) {
            wh[k * 2] = wh[(k - 1) * 2];
            wh[k * 2 + 1] = wh[(k - 1) * 2 + 1];
            k--;
        }
        wh[k * 2] = dm.w;
        wh[k * 2 + 1] = dm.h;
    }
    return n;
}

static void fb_window_size(int disp, int w, int h, int *ow, int *oh, SDL_Rect *area)
{
    int cw = s_cw ? s_cw : (int)FB_W;

    if (disp < 0 || SDL_GetDisplayUsableBounds(disp, area) != 0) {
        area->x = area->y = 0;
        area->w = 1920;
        area->h = 1080;
    }
    if (w > 0 && h > 0) {
        *ow = w;
        *oh = h;
        if (*ow > area->w || *oh > area->h) {
            if ((long long)area->w * h <= (long long)area->h * w) {
                *oh = (int)((long long)h * area->w / w);
                *ow = area->w;
            } else {
                *ow = (int)((long long)w * area->h / h);
                *oh = area->h;
            }
        }
    } else if ((long long)area->w * (int)FB_H <= (long long)area->h * cw) {
        *ow = area->w;
        *oh = (int)((long long)area->w * (int)FB_H / cw);
    } else {
        *oh = area->h;
        *ow = (int)((long long)area->h * cw / (int)FB_H);
    }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

static void fb_apply_display(SDL_Window *win, SDL_Renderer *ren)
{
    int mode = s_cfg_mode, w = s_cfg_w, h = s_cfg_h, disp = SDL_GetWindowDisplayIndex(win);
    int ow, oh;
    SDL_Rect area;
    static int pm = -1, pw, ph;

#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (ren)
        SDL_RenderSetVSync(ren, s_cfg_vsync);
#else
    (void)ren;
#endif
    if (!s_geom_force && mode == pm && w == pw && h == ph)
        return;
    pm = mode;
    pw = w;
    ph = h;
    s_geom_force = 0;
    if (mode == 2) {
        SDL_DisplayMode got;
        int ok = 0;

        if (disp < 0)
            disp = 0;
        if (w > 0 && h > 0) {
            SDL_DisplayMode want;
            SDL_zero(want);
            want.w = w;
            want.h = h;
            ok = SDL_GetClosestDisplayMode(disp, &want, &got) != NULL;
        }
        if (!ok)
            ok = SDL_GetDesktopDisplayMode(disp, &got) == 0;
        if (ok)
            SDL_SetWindowDisplayMode(win, &got);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
    } else if (mode == 1) {
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(win, 0);
        fb_window_size(disp, w, h, &ow, &oh, &area);
        SDL_SetWindowSize(win, ow, oh);
        SDL_SetWindowPosition(win, area.x + (area.w - ow) / 2, area.y + (area.h - oh) / 2);
    }
}

void xbox_FramebufferClientToFrame(void *h, int cx, int cy, int *fx, int *fy)
{
    float lx = 0, ly = 0;
    (void)h;
    if (!s_ren) {
        *fx = *fy = -1;
        return;
    }
    SDL_RenderWindowToLogical(s_ren, cx, cy, &lx, &ly);
    *fx = (int)lx - fb_inset();
    *fy = (int)ly;
}

void xbox_FramebufferSetWindowScale(void *h, int scale)
{
    (void)h;
    if (scale < 1 || scale > 8)
        return;
    xbox_FramebufferDisplayConfig(xbox_FramebufferDisplayMode(), (int)FB_W * scale,
                                  (int)FB_H * scale, s_cfg_vsync, s_cfg_cap);
}

static void fb_latch_box(void)
{
    int i;
    for (i = 0; i < 5; i++) {
        s_box_show[i] = s_box_prev[i];
        s_box_prev[i] = __atomic_load_n(&s_box_set[i], __ATOMIC_ACQUIRE);
    }
}

static void fb_put(int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < s_cw && y < (int)FB_H)
        s_rgb[(size_t)y * s_cw + x] = c;
}

static void fb_draw_box(void)
{
    const uint32_t c = s_box_color;
    int x0 = s_box_show[1], y0 = s_box_show[2], x1 = s_box_show[3], y1 = s_box_show[4];
    int x, y, w = s_cw, h = (int)FB_H;

    if (!s_box_show[0])
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

static void fb_draw_cursor(void)
{
    const int arm = 7, gap = 2;
    int cx = s_cur_x, cy = s_cur_y, i, pass;
    uint32_t c = s_cur_color;

    if (!__atomic_load_n(&s_cur_on, __ATOMIC_ACQUIRE))
        return;
    for (pass = 0; pass < 2; pass++) {
        uint32_t col = pass ? c : 0x00101010u;
        int w = pass ? 0 : 1;
        for (i = gap; i <= arm; i++) {
            int d, e;
            for (d = -w; d <= w; d++) {
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

static void fb_compose(const uint8_t *px, uint32_t bpp, uint32_t pitch, int ox)
{
    uint32_t x, y;
    xbox_fb_draw_hook hook;
    uint32_t *dst0 = ox ? s_base : s_rgb;

    for (y = 0; y < FB_H; y++) {
        const uint8_t *row = px + (size_t)y * pitch;
        uint32_t *d = dst0 + (size_t)y * FB_W;
        if (bpp == 4) {
            for (x = 0; x < FB_W; x++) {
                uint32_t v;
                memcpy(&v, row + x * 4, 4);
                d[x] = v & 0x00FFFFFFu;
            }
        } else {
            for (x = 0; x < FB_W; x++) {
                uint32_t v = (uint32_t)row[x * 2] | ((uint32_t)row[x * 2 + 1] << 8);
                uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                d[x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    if (ox) {
        int w = (int)FB_W + 2 * ox;
        fb_build_taps(w);
        for (y = 0; y < FB_H; y++)
            fb_resample_row(s_base + (size_t)y * FB_W, s_rgb + (size_t)y * w, w);
        s_cw = w;
    } else {
        s_cw = (int)FB_W;
    }
    fb_latch_box();
    fb_draw_box();
    fb_draw_cursor();
    hook = s_draw_hook;
    if (hook) {
        if (ox) {
            int cw = s_cw;
            for (y = 0; y < FB_H; y++)
                memcpy(s_view + (size_t)y * FB_W, s_rgb + (size_t)y * cw + ox, (size_t)FB_W * 4);
            hook(s_view, (int)FB_W, (int)FB_H);
            for (y = 0; y < FB_H; y++)
                memcpy(s_rgb + (size_t)y * cw + ox, s_view + (size_t)y * FB_W, (size_t)FB_W * 4);
        } else {
            hook(s_rgb, (int)FB_W, (int)FB_H);
        }
    }
}

static int hook_send(unsigned m, uintptr_t w, intptr_t l)
{
    xbox_fb_input_hook hook = s_input_hook;
    return hook && hook((void *)s_win, m, w, l);
}

static void track_mouse(int wx, int wy)
{
    float lx = 0, ly = 0;
    if (!s_ren)
        return;
    SDL_RenderWindowToLogical(s_ren, wx, wy, &lx, &ly);
    __atomic_store_n(&s_mouse_x, (int)lx, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_y, (int)ly, __ATOMIC_RELAXED);
}

static intptr_t mouse_lparam(int x, int y)
{
    return (intptr_t)(((uint32_t)(uint16_t)(int16_t)y << 16) | (uint32_t)(uint16_t)(int16_t)x);
}

static int dispatch_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int down = ev->type == SDL_KEYDOWN;
        SDL_Keycode k = ev->key.keysym.sym;
        int vk = xbox_InputSdlKeyToSideVk(k);
        intptr_t l = (intptr_t)((ev->key.keysym.scancode & 0xFF) << 16);
        if (!vk)
            vk = xbox_InputSdlKeyToVk(k);
        if (!vk)
            return 0;
        if (down && (k == SDLK_F11 || (k == SDLK_RETURN && (ev->key.keysym.mod & KMOD_ALT)))) {
            if (!ev->key.repeat)
                xbox_FramebufferSetFullscreen(NULL, !xbox_FramebufferIsFullscreen());
            return 1;
        }
        if (down && ev->key.repeat)
            l |= (intptr_t)1 << 30;
        if (!down)
            l |= ((intptr_t)1 << 30) | ((intptr_t)1 << 31);
        return hook_send(down ? AVPMSG_KEYDOWN : AVPMSG_KEYUP, (uintptr_t)vk, l);
    }
    case SDL_TEXTINPUT: {
        const unsigned char *p = (const unsigned char *)ev->text.text;
        int used = 0;
        for (; *p; p++)
            if (*p < 0x80)
                used |= hook_send(AVPMSG_CHAR, (uintptr_t)*p, 1);
        return used;
    }
    case SDL_MOUSEMOTION:
        track_mouse(ev->motion.x, ev->motion.y);
        return hook_send(AVPMSG_MOUSEMOVE, 0, mouse_lparam(ev->motion.x, ev->motion.y));
    case SDL_MOUSEWHEEL:
        __atomic_add_fetch(&s_wheel_acc, ev->wheel.y * (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -120 : 120),
                           __ATOMIC_RELAXED);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int down = ev->type == SDL_MOUSEBUTTONDOWN;
        unsigned m;
        track_mouse(ev->button.x, ev->button.y);
        switch (ev->button.button) {
        case SDL_BUTTON_LEFT: m = down ? AVPMSG_LBUTTONDOWN : AVPMSG_LBUTTONUP; break;
        case SDL_BUTTON_RIGHT: m = down ? AVPMSG_RBUTTONDOWN : AVPMSG_RBUTTONUP; break;
        case SDL_BUTTON_MIDDLE: m = down ? AVPMSG_MBUTTONDOWN : AVPMSG_MBUTTONUP; break;
        default: return 0;
        }
        return hook_send(m, 0, mouse_lparam(ev->button.x, ev->button.y));
    }
    }
    return 0;
}

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)

    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0, t_shown_us = 0;
    const char *e;
    int vsync, scale, quit = 0, guest_done = 0, dirty = 1, tex_w = (int)FB_W;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();

    if (!s_cfg_set) {
        e = getenv("RECOMP_WINDOW_SCALE");
        scale = e ? atoi(e) : 2;
        if (scale < 1 || scale > 8)
            scale = 2;
        s_cfg_w = (int)FB_W * scale;
        s_cfg_h = (int)FB_H * scale;
        s_cfg_mode = env_on("RECOMP_FULLSCREEN") ? 1 : 0;
        e = getenv("RECOMP_PRESENT_VSYNC");
        s_cfg_vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    }
    vsync = s_cfg_vsync;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;
    int win_w, win_h;
    SDL_Rect win_area;

    fb_window_size(0, s_cfg_w, s_cfg_h, &win_w, &win_h, &win_area);
    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, win_h, window_flags);
    ren = win ? SDL_CreateRenderer(win, -1,
                                   vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
              : NULL;
    if (!ren) {
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL_RegisterEvents failed; running headless\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
    s_guest_main = guest_main;

    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    SDL_StartTextInput();
    s_win = win;
    s_ren = ren;
    s_cfg_seen = s_cfg_gen;
    fb_apply_display(win, ren);
    SDL_ShowWindow(win);
    s_running = 1;
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        s_running = 0;
        xbox_InputSetMainPump(0);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t bpp = 0, pitch = 0;
        int ox = 0, hold = 0, wait_ms = shown_seq ? 100 : 16;

        if (s_cfg_gen != s_cfg_seen) {
            s_cfg_seen = s_cfg_gen;
            fb_apply_display(win, ren);
            dirty = 1;
        }
        if (s_cfg_cap > 0 && s_cfg_cap < 60 && t_shown_us) {
            uint64_t period = 920000ull / (uint64_t)s_cfg_cap, el = now_us() - t_shown_us;

            if (el < period) {
                hold = 1;
                wait_ms = (int)((period - el) / 1000) + 1;
            }
        }
        if (SDL_WaitEventTimeout(&ev, wait_ms)) {
            do {

                if (!dispatch_event(&ev))
                    xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event)
                    guest_done = 1;
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
            } while (SDL_PollEvent(&ev));
        }

        if (hold && s_cfg_cap > 0 && now_us() - t_shown_us >= 920000ull / (uint64_t)s_cfg_cap)
            hold = 0;
        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq && !hold) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (seq != shown_seq && !hold) {
            shown_seq = seq;
            t_shown_us = now_us();
            px = s_slot[s_front].px;
            bpp = s_slot[s_front].bpp;
            pitch = FB_W * bpp;
            ox = s_slot[s_front].ox;
        } else if (!shown_seq && s_fb_va && s_fb_pitch) {

            bpp = s_fb_pitch / FB_W;
            pitch = s_fb_pitch;
            if (bpp == 2 || bpp == 4)
                px = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
        }
        if (px) {
            int cw;

            fb_compose(px, bpp, pitch, ox);
            cw = s_cw;
            if (tex && tex_w != cw) {
                SDL_DestroyTexture(tex);
                tex = NULL;
            }
            if (cw != tex_w) {
                SDL_RenderSetLogicalSize(ren, cw, (int)FB_H);
                if (!xbox_FramebufferIsFullscreen() && !s_cfg_w) {
                    s_geom_force = 1;
                    fb_apply_display(win, ren);
                }
                tex_w = cw;
            }
            if (!tex)
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        cw, (int)FB_H);
            if (tex)
                SDL_UpdateTexture(tex, NULL, s_rgb, cw * 4);
            dirty = 1;
        }
        {
            int hide = __atomic_load_n(&s_hide_cursor, __ATOMIC_RELAXED);
            Uint32 fl = SDL_GetWindowFlags(win);
            __atomic_store_n(&s_mouse_focus,
                             (fl & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS)) != 0,
                             __ATOMIC_RELEASE);
            if (hide != s_cursor_hidden) {
                SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
                s_cursor_hidden = hide;
            }
        }
        if (dirty) {
            dirty = 0;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex)
                SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        now = now_ms();
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char tb[192];
            snprintf(tb, sizeof tb, "%s | FPS: %.1f", s_title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0);
            SDL_SetWindowTitle(win, tb);
            t_title = now;
            flips_title = f;
        }
    }

    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    s_ren = NULL;
    s_win = NULL;
    if (tex)
        SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (!xbox_InputAnyOpen())
        SDL_Quit();
    else
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        exit(0);
    return s_guest_rc;
}

#endif
