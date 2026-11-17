#include "avp_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "avp_keys.h"
#include "avp_layout.h"
#include "avp_skirmish.h"
#include "avp_editor.h"
#include "avp_nmap.h"
#include "avp_coop.h"
#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#endif

#if defined(AVP_MP_MODE)
#define APP_MODE 1
#else
#define APP_MODE 0
#endif
#include "avp_layer.h"
#include "avp_steam.h"
#ifndef _WIN32
#include <SDL.h>
#include <sys/stat.h>
#endif

typedef void (*xbox_fb_draw_hook)(uint32_t *rgb, int w, int h);
typedef int (*xbox_fb_input_hook)(HWND h, UINT m, WPARAM w, LPARAM l);
extern void xbox_FramebufferSetDrawHook(xbox_fb_draw_hook fn);
extern void xbox_FramebufferSetInputHook(xbox_fb_input_hook fn);
extern void xbox_FramebufferSetLayerHook(int slot, xbox_fb_layer_hook fn);
extern int  xbox_FramebufferLayerReady(void);
extern void avp_overlay_layer(XboxFbLayer *layer);
extern int  xbox_FramebufferIsFullscreen(void);
extern void xbox_FramebufferSetFullscreen(HWND h, int on);
extern void xbox_FramebufferClientToFrame(HWND h, int cx, int cy, int *fx, int *fy);
extern void xbox_FramebufferDisplayConfig(int mode, int w, int h, int vsync, int cap);
extern int  xbox_FramebufferDisplayMode(void);
extern int  xbox_FramebufferDisplayModes(int *wh, int max);
extern int  xbox_FramebufferDesktopSize(int *w, int *h);
extern void avp_wide_set(int mode);
extern int  avp_wide_mode(void);
extern int  avp_wide_width(void);
extern void xbox_InputKeysClear(void);
extern int  avp_coop_host(int port);
extern int  avp_coop_join(const char *ip, int port);
extern void avp_coop_cancel(void);
extern int  avp_coop_host_steam(void);
extern int  avp_coop_join_steam_code(const char *code);
extern int  avp_coop_join_steam_lobby(unsigned long long lobby);
extern int  avp_coop_state(void);
extern const char *avp_coop_state_text(void);
extern const char *avp_coop_peer_name(void);
extern int  avp_coop_send_cfg(void);
extern int  avp_coop_role(void);
extern int  avp_mission_count(void);
extern const char *avp_mission_code(int);
extern const char *avp_mission_title(int);
extern int  avp_mission_race(int);
extern int  avp_coop_host_launch(int mission);
extern const char *avp_coop_guest_mission(void);
extern int  avp_coop_host_pick(int mission);
extern int  avp_coop_peer_ready(void);
extern int  avp_coop_self_ready(void);
extern int  avp_coop_set_ready(int on);

volatile LONG g_avp_mainmenu;
volatile LONG g_avp_menu_req = -1;
volatile LONG g_avp_pcmenu_up;
volatile LONG g_avp_menu_sound;

static void sound(int which) { InterlockedExchange(&g_avp_menu_sound, which); }

typedef struct {
    int dmode, rw, rh, vsync, cap, edge_scroll, map_glide, input_ctl, original_menu;
    char name[24];
    char mport[8];
    char recent[3][96];
    int wide;
    int draft;
} Settings;

static Settings s_set = { 0, 1280, 960, 1, 60, 1, 0, 0, 0, "Player", "27015" };
static int s_env_draft;
#ifdef _WIN32
static const char *ini_file(void)
{
    return "avp_settings.ini";
}
#else
static const char *ini_file(void)
{
    static char path[512];
    const char *dir = getenv("AVP_CONFIG_DIR"), *home;
    char base[480];
    if (path[0])
        return path;
    if (dir && *dir) {
        snprintf(base, sizeof base, "%s", dir);
    } else if ((dir = getenv("XDG_CONFIG_HOME")) && *dir) {
        snprintf(base, sizeof base, "%s/avp-extinction", dir);
    } else if ((home = getenv("HOME")) && *home) {
        char cfg[400];
        snprintf(cfg, sizeof cfg, "%s/.config", home);
        mkdir(cfg, 0755);
        snprintf(base, sizeof base, "%s/.config/avp-extinction", home);
    } else {
        snprintf(path, sizeof path, "avp_settings.ini");
        return path;
    }
    mkdir(base, 0755);
    snprintf(path, sizeof path, "%s/avp_settings.ini", base);
    return path;
}
#endif

static void settings_apply_env(void)
{
    _putenv(s_set.edge_scroll ? "AVP_NO_EDGE=" : "AVP_NO_EDGE=1");
    _putenv(s_set.map_glide ? "AVP_MAP_GLIDE=1" : "AVP_MAP_GLIDE=");
}

static const int k_caps[] = { 0, 30, 60, 120, 144 };
#define N_CAPS ((int)(sizeof k_caps / sizeof k_caps[0]))

static const char *const k_dmodes[] = { "windowed", "borderless", "exclusive" };

static void display_apply(void)
{
    xbox_FramebufferDisplayConfig(s_set.dmode, s_set.rw, s_set.rh, s_set.vsync, s_set.cap);
}

static void settings_save(void)
{
    FILE *f = fopen(ini_file(), "w");
    int i;
    if (!f) return;
    fprintf(f, "; Aliens versus Predator: Extinction (PC port) settings\n"
               "display_mode=%s\n",
            k_dmodes[s_set.dmode]);
    if (s_set.rw > 0 && s_set.rh > 0) fprintf(f, "resolution=%dx%d\n", s_set.rw, s_set.rh);
    else fprintf(f, "resolution=desktop\n");
    fprintf(f, "frame_cap=%d\nvsync=%d\n", s_set.cap, s_set.vsync);
    fprintf(f, "edge_scroll=%d\nminimap_glide=%d\ninput=%s\nplayer_name=%s\nmenu_style=%s\nmenu_draft=%d\n",
            s_set.edge_scroll, s_set.map_glide,
            s_set.input_ctl ? "controller" : "mouse", s_set.name, s_set.original_menu ? "original" : "pc",
            s_set.draft);
    fprintf(f, "mp_port=%s\n", s_set.mport);
    fprintf(f, "widescreen=%s\n", s_set.wide == 1 ? "16:9" : s_set.wide == 2 ? "16:10" : "off");
    for (i = 0; i < 3; i++)
        if (*s_set.recent[i]) fprintf(f, "mp_recent%d=%s\n", i + 1, s_set.recent[i]);
    avp_keys_save(f);
    fclose(f);
}

void avp_settings_load(void)
{
    FILE *f = fopen(ini_file(), "r");
    char line[128];
    const char *ed = getenv("AVP_MENU_DRAFT");
    int have_mode = 0, have_res = 0, old_full = 0, old_scale = 0;
    s_set.draft = 4;
    s_env_draft = ed ? atoi(ed) : 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *v;
            if (!eq || line[0] == ';') continue;
            *eq = 0;
            v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            if (!strcmp(line, "fullscreen")) old_full = atoi(v) != 0;
            else if (!strcmp(line, "window_scale")) old_scale = atoi(v);
            else if (!strcmp(line, "display_mode")) {
                int k;
                for (k = 0; k < 3; k++)
                    if (!strcmp(v, k_dmodes[k])) { s_set.dmode = k; have_mode = 1; }
            } else if (!strcmp(line, "resolution")) {
                int rw = 0, rh = 0;
                if (!strcmp(v, "desktop")) { s_set.rw = s_set.rh = 0; have_res = 1; }
                else if (sscanf(v, "%dx%d", &rw, &rh) == 2 && rw >= 320 && rw <= 16384 && rh >= 240 && rh <= 16384) {
                    s_set.rw = rw; s_set.rh = rh; have_res = 1;
                }
            } else if (!strcmp(line, "frame_cap")) {
                int c = !strcmp(v, "off") ? 0 : atoi(v), k;
                for (k = 0; k < N_CAPS; k++)
                    if (k_caps[k] == c) s_set.cap = c;
            } else if (!strcmp(line, "vsync")) s_set.vsync = atoi(v) != 0;
            else if (!strcmp(line, "edge_scroll")) s_set.edge_scroll = atoi(v) != 0;
            else if (!strcmp(line, "minimap_glide")) s_set.map_glide = atoi(v) != 0;
            else if (!strcmp(line, "input")) s_set.input_ctl = !strcmp(v, "controller");
            else if (!strcmp(line, "menu_style")) s_set.original_menu = !strcmp(v, "original");
            else if (!strcmp(line, "menu_draft")) s_set.draft = atoi(v);
            else if (!strcmp(line, "widescreen")) s_set.wide = !strcmp(v, "16:9") || !strcmp(v, "1") ? 1 : !strcmp(v, "16:10") || !strcmp(v, "2") ? 2 : 0;
            else if (!strcmp(line, "mp_port") && atoi(v) > 0) snprintf(s_set.mport, sizeof s_set.mport, "%.5s", v);
            else if (!strncmp(line, "mp_recent", 9) && line[9] >= '1' && line[9] <= '3' && !line[10])
                snprintf(s_set.recent[line[9] - '1'], sizeof s_set.recent[0], "%.95s", v);
            else if (!strcmp(line, "player_name") && *v)
                snprintf(s_set.name, sizeof s_set.name, "%.16s", v);
            else if (!strncmp(line, "key_", 4)) avp_keys_load_line(line + 4, v);
        }
        fclose(f);
    }
    avp_keys_apply();
    if (!have_mode && old_full) s_set.dmode = 1;
    if (!have_res && old_scale >= 1 && old_scale <= 8) { s_set.rw = 640 * old_scale; s_set.rh = 480 * old_scale; }
    avp_wide_set(s_set.wide);
    {
        const char *fe = getenv("RECOMP_FULLSCREEN"), *se = getenv("RECOMP_WINDOW_SCALE"), *ve = getenv("RECOMP_PRESENT_VSYNC");
        int sc = se ? atoi(se) : 0;
        Settings keep = s_set;
        if (fe) s_set.dmode = *fe && *fe != '0' ? (s_set.dmode ? s_set.dmode : 1) : 0;
        if (sc >= 1 && sc <= 8) { s_set.rw = 640 * sc; s_set.rh = 480 * sc; }
        if (ve) s_set.vsync = *ve != '0';
        display_apply();
        s_set = keep;
    }
    if (!getenv("AVP_NO_EDGE") && !getenv("AVP_MAP_GLIDE"))
        settings_apply_env();
    fprintf(stderr, "[PCMENU] settings: display %s %dx%d vsync %d cap %d edge %d glide %d menu %s name %s\n",
            k_dmodes[s_set.dmode], s_set.rw, s_set.rh, s_set.vsync, s_set.cap, s_set.edge_scroll, s_set.map_glide,
            s_set.original_menu ? "original" : "pc", s_set.name);
}

int avp_input_controller(void)
{
    return s_set.input_ctl;
}

const char *avp_player_name(void)
{
    return s_set.name;
}

static uint32_t *s_px;
static int s_w, s_h;
#ifdef _WIN32
static HDC s_dc;
static HBITMAP s_bmp;
static HFONT s_font[5];
typedef RECT TextRect;
#else
typedef struct { int left, top, right, bottom; } TextRect;
#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_NOPREFIX 0x0800
#endif

enum { F_BTN, F_TITLE, F_BODY, F_BIG, F_HUGE };

typedef struct { TextRect r; char s[160]; uint32_t c, gc; int font, fx; unsigned fmt; } Text;
static int s_fx;
static uint32_t s_gc;
static Text s_text[128];
static int s_ntext;

#ifndef _WIN32
static int dib_ensure(int w, int h)
{
    if (s_px && w == s_w && h == s_h) return 1;
    free(s_px);
    s_px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!s_px) return 0;
    s_w = w; s_h = h;
    return 1;
}
#else
static int dib_ensure(int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    if (s_px && w == s_w && h == s_h) return 1;
    if (s_bmp) DeleteObject(s_bmp);
    if (!s_dc) s_dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s_bmp) { s_px = NULL; return 0; }
    SelectObject(s_dc, s_bmp);
    s_px = bits; s_w = w; s_h = h;
    if (!s_font[0]) {
        static const struct { int h, wt; } spec[5] = {
            { 19, FW_SEMIBOLD }, { 27, FW_BOLD }, { 15, FW_NORMAL }, { 31, FW_SEMIBOLD }, { 46, FW_BOLD } };
        int i;
        for (i = 0; i < 5; i++) {
            s_font[i] = CreateFontA(-spec[i].h, 0, 0, 0, spec[i].wt, 0, 0, 0, DEFAULT_CHARSET,
                                    OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH, i == F_BODY ? "Bahnschrift" : "Bahnschrift SemiBold Condensed");
        }
    }
    return 1;
}
#endif

#define LW 640
#define LH 480
#define MAX_ITEMS 768

static float s_S = 1.0f, s_ox, s_oy;
static int s_alpha;

typedef struct { int x0, y0, x1, y1; } Clip;

static uint32_t mix_rgb(uint32_t d, uint32_t c, float a)
{
    int dr = (int)((d >> 16) & 255), dg = (int)((d >> 8) & 255), db = (int)(d & 255);
    int cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);
    int r, g, b;
    if (a <= 0) return d & 0x00FFFFFFu;
    if (a >= 1) return c & 0x00FFFFFFu;
    r = dr + (int)((cr - dr) * a + 0.5f);
    g = dg + (int)((cg - dg) * a + 0.5f);
    b = db + (int)((cb - db) * a + 0.5f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t over_px(uint32_t d, uint32_t c, float a)
{
    int a8, ia, r, g, b, al;
    if (a <= 0) return d;
    a8 = a >= 1 ? 255 : (int)(a * 255.0f + 0.5f);
    if (a8 <= 0) return d;
    ia = 255 - a8;
    r = ((int)((c >> 16) & 255) * a8 + (int)((d >> 16) & 255) * ia + 127) / 255;
    g = ((int)((c >> 8) & 255) * a8 + (int)((d >> 8) & 255) * ia + 127) / 255;
    b = ((int)(c & 255) * a8 + (int)(d & 255) * ia + 127) / 255;
    al = (255 * a8 + (int)(d >> 24) * ia + 127) / 255;
    return ((uint32_t)al << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t mix(uint32_t d, uint32_t c, float a)
{
    return s_alpha ? over_px(d, c, a) : mix_rgb(d, c, a);
}

static uint32_t lerp_c(uint32_t a, uint32_t b, float t) { return mix_rgb(a, b, t); }

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static int dev_x(float x) { return (int)floorf(s_ox + x * s_S + 0.5f); }
static int dev_y(float y) { return (int)floorf(s_oy + y * s_S + 0.5f); }

enum { IT_RRECT, IT_PLATE, IT_THUMB, IT_TEXT };

typedef struct {
    int x0, y0, x1, y1, cut;
    float rad, fa, bw;
    uint32_t top, bot, edge, lit, shade;
    const uint32_t *pix;
} Shape;

typedef struct {
    int type, bx0, by0, bx1, by1;
    uint32_t hash;
    union { Shape sh; Text tx; } u;
} Item;

static Item s_item[MAX_ITEMS];
static int s_nitem;

static uint32_t hmix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t hflt(uint32_t h, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return hmix(h, v);
}

static Item *item_new(int type)
{
    Item *it;
    if (s_nitem >= MAX_ITEMS) return NULL;
    it = &s_item[s_nitem++];
    memset(it, 0, sizeof *it);
    it->type = type;
    return it;
}

static void shape_add(int type, int x0, int y0, int x1, int y1, int cut, float rad, uint32_t top, uint32_t bot,
                      float fa, uint32_t edge, float bw, uint32_t lit, uint32_t shade, const uint32_t *pix)
{
    Item *it = item_new(type);
    Shape *s;
    uint32_t h = 2166136261u;
    if (!it) return;
    s = &it->u.sh;
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1; s->cut = cut;
    s->rad = rad; s->fa = fa; s->bw = bw;
    s->top = top; s->bot = bot; s->edge = edge; s->lit = lit; s->shade = shade;
    s->pix = pix;
    it->bx0 = dev_x((float)x0) - 3; it->by0 = dev_y((float)y0) - 3;
    it->bx1 = dev_x((float)x1) + 3; it->by1 = dev_y((float)y1) + 3;
    h = hmix(h, (uint32_t)type);
    h = hmix(h, (uint32_t)x0); h = hmix(h, (uint32_t)y0); h = hmix(h, (uint32_t)x1); h = hmix(h, (uint32_t)y1);
    h = hmix(h, (uint32_t)cut); h = hflt(h, rad); h = hflt(h, fa); h = hflt(h, bw);
    h = hmix(h, top); h = hmix(h, bot); h = hmix(h, edge); h = hmix(h, lit); h = hmix(h, shade);
    if (pix) {
        int i;
        for (i = 0; i < AVP_THUMB_W * AVP_THUMB_H; i++)
            h = hmix(h, pix[i]);
    }
    it->hash = h;
}

static void rrect(int x0, int y0, int x1, int y1, float rad,
                  uint32_t top, uint32_t bot, float fill_a, uint32_t edge, float bw)
{
    shape_add(IT_RRECT, x0, y0, x1, y1, 0, rad, top, bot, fill_a, edge, bw, 0, 0, NULL);
}

static float dev_bw(float bw)
{
    if (bw <= 0) return 0;
    bw *= s_S;
    if (s_S >= 1.5f) bw = fmaxf(1.0f, floorf(bw + 0.5f));
    return bw;
}

static void rrect_px(const Shape *s, const Clip *cl)
{
    float X0 = (float)dev_x((float)s->x0), Y0 = (float)dev_y((float)s->y0);
    float X1 = (float)dev_x((float)s->x1), Y1 = (float)dev_y((float)s->y1);
    float rad = s->rad * s_S, bw = dev_bw(s->bw), cx, cy, hw, hh, spanv;
    int x, y, ya, yb, xa, xb;
    if (X1 < X0 + 1 && s->x1 > s->x0) X1 = X0 + 1;
    if (Y1 < Y0 + 1 && s->y1 > s->y0) Y1 = Y0 + 1;
    cx = (X0 + X1) * 0.5f; cy = (Y0 + Y1) * 0.5f;
    hw = (X1 - X0) * 0.5f; hh = (Y1 - Y0) * 0.5f;
    spanv = Y1 - Y0 > 0 ? Y1 - Y0 : 1;
    ya = (int)Y0 - 1; yb = (int)Y1;
    xa = (int)X0 - 1; xb = (int)X1;
    if (ya < cl->y0) ya = cl->y0;
    if (yb > cl->y1 - 1) yb = cl->y1 - 1;
    if (xa < cl->x0) xa = cl->x0;
    if (xb > cl->x1 - 1) xb = cl->x1 - 1;
    for (y = ya; y <= yb; y++) {
        uint32_t fc = lerp_c(s->top, s->bot, clampf(((float)y - Y0) / spanv));
        uint32_t *row = &s_px[(size_t)y * s_w];
        float py = fabsf((float)y + 0.5f - cy) - (hh - rad);
        float qy = py > 0 ? py : 0;
        for (x = xa; x <= xb; x++) {
            float px, qx, len, d, fa, ea;
            uint32_t *p;
            px = fabsf((float)x + 0.5f - cx) - (hw - rad);
            qx = px > 0 ? px : 0;
            len = (qx > 0 || qy > 0) ? sqrtf(qx * qx + qy * qy) : 0.0f;
            d = len + (px > py ? (px < 0 ? px : 0) : (py < 0 ? py : 0)) - rad;
            fa = clampf(0.5f - d);
            if (fa <= 0) continue;
            p = &row[x];
            *p = mix(*p, fc, fa * s->fa);
            if (bw > 0) {
                ea = fa - clampf(0.5f - (d + bw));
                if (ea > 0) *p = mix(*p, s->edge, ea);
            }
        }
    }
}

static void thumb_px(const Shape *s, const Clip *cl)
{
    int X0 = dev_x((float)s->x0), Y0 = dev_y((float)s->y0);
    int X1 = dev_x((float)(s->x0 + AVP_THUMB_W)), Y1 = dev_y((float)(s->y0 + AVP_THUMB_H));
    int x, y, xa = X0, xb = X1, ya = Y0, yb = Y1;
    float kx, ky;
    if (X1 <= X0 || Y1 <= Y0) return;
    kx = (float)AVP_THUMB_W / (float)(X1 - X0);
    ky = (float)AVP_THUMB_H / (float)(Y1 - Y0);
    if (xa < cl->x0) xa = cl->x0;
    if (xb > cl->x1) xb = cl->x1;
    if (ya < cl->y0) ya = cl->y0;
    if (yb > cl->y1) yb = cl->y1;
    for (y = ya; y < yb; y++) {
        float sy = ((float)(y - Y0) + 0.5f) * ky - 0.5f, fy;
        int y0i = (int)floorf(sy), y1i;
        fy = sy - (float)y0i;
        y1i = y0i + 1;
        if (y0i < 0) y0i = 0;
        if (y1i < 0) y1i = 0;
        if (y0i > AVP_THUMB_H - 1) y0i = AVP_THUMB_H - 1;
        if (y1i > AVP_THUMB_H - 1) y1i = AVP_THUMB_H - 1;
        for (x = xa; x < xb; x++) {
            float sx = ((float)(x - X0) + 0.5f) * kx - 0.5f, fx;
            int x0i = (int)floorf(sx), x1i, k;
            uint32_t out = 0;
            fx = sx - (float)x0i;
            x1i = x0i + 1;
            if (x0i < 0) x0i = 0;
            if (x1i < 0) x1i = 0;
            if (x0i > AVP_THUMB_W - 1) x0i = AVP_THUMB_W - 1;
            if (x1i > AVP_THUMB_W - 1) x1i = AVP_THUMB_W - 1;
            for (k = 0; k < 24; k += 8) {
                float v = (float)((s->pix[y0i * AVP_THUMB_W + x0i] >> k) & 255) * (1 - fx) * (1 - fy)
                        + (float)((s->pix[y0i * AVP_THUMB_W + x1i] >> k) & 255) * fx * (1 - fy)
                        + (float)((s->pix[y1i * AVP_THUMB_W + x0i] >> k) & 255) * (1 - fx) * fy
                        + (float)((s->pix[y1i * AVP_THUMB_W + x1i] >> k) & 255) * fx * fy;
                out |= (uint32_t)(v + 0.5f) << k;
            }
            s_px[(size_t)y * s_w + x] = (out & 0x00FFFFFFu) | (s_alpha ? 0xFF000000u : 0u);
        }
    }
}

static void text(int x0, int y0, int x1, int y1, const char *s, uint32_t c, int font, unsigned fmt)
{
    Text *t;
    if (s_ntext >= (int)(sizeof s_text / sizeof s_text[0])) return;
    t = &s_text[s_ntext++];
    t->r.left = x0; t->r.top = y0; t->r.right = x1; t->r.bottom = y1;
    snprintf(t->s, sizeof t->s, "%s", s);
    t->c = c; t->font = font; t->fmt = fmt;
    t->fx = s_fx; t->gc = s_gc;
}

static const int k_font_px[5] = { 19, 27, 15, 31, 46 };

static void font_scale(int font, float *sx, float *sy)
{
    *sy = (float)k_font_px[font] * 0.72f / 7.0f * s_S;
    *sx = *sy * 0.8f;
}

#define GF_CELL_H 28
#define GF_CAP_H 19
#define GF_BASE 23
#define GF_SPACE 7
#define GF_CAP_K 0.64f
#define GF_FILE "game" AVP_SEP "tbd" AVP_SEP "17F20E3C.tbx"

typedef struct { uint8_t has, page; float u, v, w; } GfGlyph;
static GfGlyph s_gg[256];
static uint8_t *s_gtex[2];
static int s_gf_state;

static uint32_t gf_r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned gf_r16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned gf_spread(unsigned v)
{
    unsigned r = 0, b;
    for (b = 0; b < 8; b++)
        r |= ((v >> b) & 1u) << (2 * b);
    return r;
}

static uint8_t *gf_unpack(const uint8_t *s, size_t len, size_t *outn)
{
    uint32_t claim;
    size_t p = 4, o = 0;
    unsigned win, mask = 0, k = 0;
    uint8_t *d;
    if (len < 8) return NULL;
    claim = gf_r32(s);
    if (claim == 0 || claim > (16u << 20)) return NULL;
    d = (uint8_t *)calloc(1, (size_t)claim + 8);
    if (!d) return NULL;
    win = gf_r16(s + p);
    p += 2;
    while (k < 16 && !(win & 0x8000u)) {
        win = (win << 1) & 0xFFFFu;
        mask = (mask << 1) | 1u;
        k++;
    }
    while (p + 2 <= len) {
        unsigned flags = gf_r16(s + p), bit;
        p += 2;
        for (bit = 0x8000u; bit; bit >>= 1) {
            unsigned v;
            if (p + 2 > len) goto done;
            v = gf_r16(s + p);
            p += 2;
            if (!(flags & bit)) {
                if (o + 2 > claim) goto done;
                d[o++] = (uint8_t)v;
                d[o++] = (uint8_t)(v >> 8);
            } else {
                size_t n, back, i;
                if (!v) goto done;
                n = (size_t)(v & mask) * 2;
                back = (size_t)(v >> k) * 2;
                if (back > o || o + n > claim) goto done;
                for (i = 0; i < n; i++, o++)
                    d[o] = d[o - back];
            }
        }
    }
done:
    *outn = o;
    return d;
}

static int gf_build(const uint8_t *d, size_t dl, const uint8_t *t, size_t tl)
{
    static const size_t tex_obj[2] = { 38080, 42896 };
    const size_t hd = 45456;
    size_t recs, i;
    int pg;
    if (dl < hd + 0x24 + 256 || gf_r16(d + hd + 0x18) != 127) return 0;
    recs = gf_r32(d + hd + 0x20);
    if (recs + 127 * 20 > dl) return 0;
    for (pg = 0; pg < 2; pg++) {
        size_t off, x, y;
        uint8_t *a;
        if (tex_obj[pg] + 8 > dl) return 0;
        off = gf_r32(d + tex_obj[pg] + 4) & 0x0FFFFFFFu;
        if (off + 20 + 256 * 256 * 4 > tl) return 0;
        a = (uint8_t *)malloc(256 * 256);
        if (!a) return 0;
        for (y = 0; y < 256; y++)
            for (x = 0; x < 256; x++)
                a[y * 256 + x] = t[off + 20 + (size_t)(gf_spread((unsigned)x) | (gf_spread((unsigned)y) << 1)) * 4 + 3];
        s_gtex[pg] = a;
    }
    for (i = 0; i < 256; i++) {
        unsigned gi = d[hd + 0x24 + i];
        const uint8_t *r;
        if (gi >= 127) continue;
        r = d + recs + gi * 20;
        if (gf_r16(r + 2) > 1) continue;
        s_gg[i].has = 1;
        s_gg[i].page = (uint8_t)gf_r16(r + 2);
        s_gg[i].w = (float)gf_r16(r + 6) / 16.0f;
        s_gg[i].u = (float)gf_r16(r + 12) / 16.0f;
        s_gg[i].v = (float)gf_r16(r + 16) / 16.0f;
    }
    return 1;
}

static int gf_load(void)
{
    FILE *f = fopen(GF_FILE, "rb");
    uint8_t *buf;
    long n;
    size_t pos;
    int ok = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 64 || n > (64L << 20)) { fclose(f); return 0; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "TBD2", 4)) { free(buf); return 0; }
    pos = 12;
    while (!ok && pos + 12 <= (size_t)n) {
        size_t sz = gf_r32(buf + pos + 4);
        if (!memcmp(buf + pos, "LIST", 4) && pos + 8 + sz <= (size_t)n) {
            size_t p = pos + 12, e = pos + 8 + sz, dl = 0, tl = 0;
            const uint8_t *dp = NULL, *tp = NULL;
            while (p + 8 <= e) {
                size_t s = gf_r32(buf + p + 4);
                if (p + 8 + s > e) break;
                if (!memcmp(buf + p, "DATX", 4)) { dp = buf + p + 8; dl = s; }
                if (!memcmp(buf + p, "TEMX", 4)) { tp = buf + p + 8; tl = s; }
                p += 8 + s + (s & 1);
            }
            if (dp && tp && dl >= 8 && tl >= 8 && gf_r32(dp) == 46980u && gf_r32(tp) == 1691444u) {
                size_t dn = 0, tn = 0;
                uint8_t *du = gf_unpack(dp, dl, &dn), *tu = gf_unpack(tp, tl, &tn);
                if (du && tu) ok = gf_build(du, dn, tu, tn);
                free(du);
                free(tu);
            }
        }
        pos += 8 + sz + (sz & 1);
    }
    free(buf);
    if (!ok) {
        int i;
        for (i = 0; i < 2; i++) { free(s_gtex[i]); s_gtex[i] = NULL; }
        memset(s_gg, 0, sizeof s_gg);
    }
    return ok;
}

static int gf_ready(void)
{
    if (!s_gf_state) {
        s_gf_state = gf_load() ? 1 : -1;
        fprintf(stderr, "[PCMENU] game font %s\n", s_gf_state > 0 ? "loaded" : "unavailable, using fallback");
    }
    return s_gf_state > 0;
}

static float gf_cap(int font) { return (float)k_font_px[font] * GF_CAP_K * s_S; }

static float gf_scale(int font)
{
    static const float snap[] = { 0.5f, 1.0f, 1.5f, 2.0f };
    float s = (float)k_font_px[font] * GF_CAP_K / (float)GF_CAP_H;
    int i;
    for (i = 0; i < 4; i++)
        if (fabsf(s - snap[i]) <= snap[i] * 0.12f) return snap[i] * s_S;
    return s * s_S;
}

static float gf_adv(int ch, float s)
{
    const GfGlyph *g = &s_gg[ch & 255];
    float a = roundf((g->has ? g->w : (float)GF_SPACE) * s);
    return a < 1.0f ? 1.0f : a;
}

typedef struct { int ch; float s; int w, h; uint8_t *a; } GfBmp;
#define GF_BMPS 2048
static GfBmp s_gb[GF_BMPS];
static int s_gb_n;

static float gf_texel(const GfGlyph *g, int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx > 255 || ty > 255) return 0;
    if (tx < (int)floorf(g->u) || tx >= (int)ceilf(g->u + g->w)) return 0;
    if (ty < (int)floorf(g->v) || ty >= (int)ceilf(g->v + (float)GF_CELL_H)) return 0;
    return (float)s_gtex[g->page][ty * 256 + tx] / 255.0f;
}

static float gf_sample_cell(const GfGlyph *g, float u, float v)
{
    int x0 = (int)floorf(u - 0.5f), y0 = (int)floorf(v - 0.5f);
    float fx = u - 0.5f - (float)x0, fy = v - 0.5f - (float)y0;
    return gf_texel(g, x0, y0) * (1 - fx) * (1 - fy) + gf_texel(g, x0 + 1, y0) * fx * (1 - fy) +
           gf_texel(g, x0, y0 + 1) * (1 - fx) * fy + gf_texel(g, x0 + 1, y0 + 1) * fx * fy;
}

static void gf_bmp_free_all(void)
{
    int i;
    for (i = 0; i < GF_BMPS; i++) {
        free(s_gb[i].a);
        s_gb[i].a = NULL;
        s_gb[i].w = s_gb[i].h = 0;
        s_gb[i].s = 0;
    }
    s_gb_n = 0;
}

static void gf_bmp_fill(GfBmp *e, int ch, float s)
{
    const GfGlyph *g = &s_gg[ch & 255];
    float gw = g->w;
    int bw = (int)ceilf(gw * s) + 1, bh = (int)ceilf((float)GF_CELL_H * s) + 1, i, j;
    int native = fabsf(s - 1.0f) < 0.02f;
    float k = 1.0f + (s - 1.0f) * 0.8f, t = 0.85f * clampf((s - 1.0f) / 0.5f);
    uint8_t *a;
    if (k > 5.0f) k = 5.0f;
    e->ch = ch;
    e->s = s;
    e->w = e->h = 0;
    e->a = NULL;
    if (!g->has || bw < 1 || bh < 1) return;
    a = (uint8_t *)calloc((size_t)bw * (size_t)bh, 1);
    if (!a) return;
    for (j = 0; j < bh; j++) {
        for (i = 0; i < bw; i++) {
            float acc = 0;
            if (native) {
                if ((float)i < gw && j < GF_CELL_H)
                    acc = gf_texel(g, (int)(g->u + (float)i), (int)(g->v + (float)j));
            } else if (s > 1.0f) {
                float u = ((float)i + 0.5f) / s, v = ((float)j + 0.5f) / s;
                if (u < gw && v < (float)GF_CELL_H) {
                    float lin = gf_sample_cell(g, g->u + u, g->v + v);
                    float steep = clampf((lin - 0.5f) * k + 0.5f);
                    acc = lin + (steep - lin) * t;
                }
            } else {
                int m, n;
                for (n = 0; n < 3; n++) {
                    float v = (((float)j + ((float)n + 0.5f) / 3.0f)) / s;
                    if (v < 0 || v >= (float)GF_CELL_H) continue;
                    for (m = 0; m < 3; m++) {
                        float u = (((float)i + ((float)m + 0.5f) / 3.0f)) / s;
                        if (u < 0 || u >= gw) continue;
                        acc += gf_sample_cell(g, g->u + u, g->v + v);
                    }
                }
                acc /= 9.0f;
            }
            a[j * bw + i] = (uint8_t)(clampf(acc) * 255.0f + 0.5f);
        }
    }
    e->w = bw;
    e->h = bh;
    e->a = a;
}

static GfBmp *gf_bmp(int ch, float s)
{
    unsigned h, n;
    int pass;
    uint32_t sb;
    memcpy(&sb, &s, 4);
    for (pass = 0; pass < 2; pass++) {
        h = ((unsigned)(ch & 255) * 2654435761u) ^ (sb * 40503u);
        for (n = 0; n < GF_BMPS; n++) {
            GfBmp *e = &s_gb[(h + n) & (GF_BMPS - 1)];
            if (e->s == s && e->ch == (ch & 255) && (e->a || e->w == 0) && e->s != 0)
                return e;
            if (e->s == 0) {
                if (s_gb_n >= GF_BMPS * 3 / 4) break;
                gf_bmp_fill(e, ch & 255, s);
                s_gb_n++;
                return e;
            }
        }
        gf_bmp_free_all();
    }
    return NULL;
}

static void gf_draw(int ch, float x, float base, float s, uint32_t color, float amul, const TextRect *clip)
{
    GfBmp *b = gf_bmp(ch, s);
    int ox = (int)floorf(x + 0.5f), oy = (int)floorf(base - (float)GF_BASE * s + 0.5f);
    int px0, px1, py0, py1, px, py;
    if (!b || !b->a) return;
    px0 = ox; px1 = ox + b->w; py0 = oy; py1 = oy + b->h;
    if (px0 < clip->left) px0 = clip->left;
    if (py0 < clip->top) py0 = clip->top;
    if (px1 > clip->right) px1 = clip->right;
    if (py1 > clip->bottom) py1 = clip->bottom;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > s_w) px1 = s_w;
    if (py1 > s_h) py1 = s_h;
    for (py = py0; py < py1; py++) {
        const uint8_t *ar = b->a + (size_t)(py - oy) * (size_t)b->w - ox;
        uint32_t *d = &s_px[(size_t)py * s_w];
        for (px = px0; px < px1; px++) {
            unsigned v = ar[px];
            float acc;
            if (!v) continue;
            acc = (float)v * (1.0f / 255.0f) * amul;
            if (acc > 0.004f) d[px] = mix(d[px], color, acc > 1.0f ? 1.0f : acc);
        }
    }
}

static float char_adv(int font, int ch)
{
    float sx, sy;
    if (s_gf_state > 0) return gf_adv(ch, gf_scale(font));
    font_scale(font, &sx, &sy);
    return (ch >= 'a' && ch <= 'z' ? 0.8f : 1.0f) * 6.0f * sx;
}

static float span_width(int font, const char *s, int n)
{
    float w = 0;
    int i;
    for (i = 0; i < n; i++)
        w += char_adv(font, (unsigned char)s[i]);
    return w;
}

static void draw_glyph(int font, int ch, float x, float base, uint32_t color, float amul, const TextRect *clip)
{
    const uint8_t *g = avp_font5x7(ch);
    float sx, sy, k = ch >= 'a' && ch <= 'z' ? 0.8f : 1.0f, gx, gy, top;
    int px0, px1, py0, py1, px, py;
    font_scale(font, &sx, &sy);
    gx = sx * k;
    gy = sy * k;
    top = base - 7.0f * gy;
    px0 = (int)floorf(x);
    px1 = (int)ceilf(x + 5.0f * gx);
    py0 = (int)floorf(top);
    py1 = (int)ceilf(base);
    if (px0 < clip->left) px0 = clip->left;
    if (py0 < clip->top) py0 = clip->top;
    if (px1 > clip->right) px1 = clip->right;
    if (py1 > clip->bottom) py1 = clip->bottom;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > s_w) px1 = s_w;
    if (py1 > s_h) py1 = s_h;
    for (py = py0; py < py1; py++) {
        for (px = px0; px < px1; px++) {
            int i, j, hit = 0;
            for (j = 0; j < 4; j++) {
                int row = (int)floorf(((float)py + ((float)j + 0.5f) / 4.0f - top) / gy);
                if (row < 0 || row > 6) continue;
                for (i = 0; i < 4; i++) {
                    int col = (int)floorf(((float)px + ((float)i + 0.5f) / 4.0f - x) / gx);
                    if (col >= 0 && col < 5 && (g[col] >> row) & 1)
                        hit++;
                }
            }
            if (hit) {
                uint32_t *d = &s_px[(size_t)py * s_w + px];
                *d = mix(*d, color, (float)hit / 16.0f * amul);
            }
        }
    }
}

static void draw_line(int font, const char *s, int n, float x, float base, uint32_t color, float amul, const TextRect *clip)
{
    int i;
    float gs = s_gf_state > 0 ? gf_scale(font) : 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch != ' ') {
            if (s_gf_state > 0) gf_draw(ch, x, base, gs, color, amul, clip);
            else draw_glyph(font, ch, x, base, color, amul, clip);
        }
        x += char_adv(font, ch);
    }
}

typedef struct { int start, len; } Span;

static int break_lines(const Text *t, Span *out, int max)
{
    const char *s = t->s;
    int n = (int)strlen(s), i = 0, count = 0;
    float limit = (float)(t->r.right - t->r.left);
    while (i <= n && count < max) {
        int line = i, end = i, last_space = -1;
        float w = 0;
        while (end < n && s[end] != '\n') {
            float a = char_adv(t->font, (unsigned char)s[end]);
            if ((t->fmt & DT_WORDBREAK) && w + a > limit && end > line) {
                if (last_space > line) end = last_space;
                break;
            }
            if (s[end] == ' ') last_space = end;
            w += a;
            end++;
        }
        out[count].start = line;
        out[count].len = end - line;
        count++;
        i = end;
        if (i < n && (s[i] == ' ' || s[i] == '\n'))
            i++;
        else if (i >= n)
            break;
    }
    return count;
}

typedef struct { int dx, dy; float a; int glow; } TextPass;

static int pdev(int v)
{
    int d;
    if (!v) return 0;
    d = (int)floorf(fabsf((float)v) * s_S + 0.5f);
    if (d < 1) d = 1;
    return v < 0 ? -d : d;
}

static void soft_run(const Text *t, const Span *lines, int nl, float y0, float cap, float lh,
                     const TextPass *ps, uint32_t col, const Clip *cl)
{
    TextRect clip = t->r;
    uint32_t c = ps->glow ? t->gc : col;
    int l, pad = t->fx == 2 ? (int)ceilf(4.0f * s_S) : 0;
    clip.left += ps->dx - pad; clip.right += ps->dx + pad;
    clip.top += ps->dy - pad; clip.bottom += ps->dy + pad;
    if (clip.left < cl->x0) clip.left = cl->x0;
    if (clip.top < cl->y0) clip.top = cl->y0;
    if (clip.right > cl->x1) clip.right = cl->x1;
    if (clip.bottom > cl->y1) clip.bottom = cl->y1;
    if (clip.right <= clip.left || clip.bottom <= clip.top) return;
    for (l = 0; l < nl; l++) {
        const char *ls = t->s + lines[l].start;
        int len = lines[l].len;
        float w = span_width(t->font, ls, len), x;
        float base = y0 + lh * (float)l + (lh + cap) * 0.5f + (float)ps->dy;
        if (t->fmt & DT_CENTER) x = (float)t->r.left + ((float)(t->r.right - t->r.left) - w) * 0.5f;
        else if (t->fmt & DT_RIGHT) x = (float)t->r.right - w;
        else x = (float)t->r.left;
        draw_line(t->font, ls, len, x + (float)ps->dx, base, c, ps->a, &clip);
    }
}

static TextRect dev_rect(const TextRect *r)
{
    TextRect o;
    o.left = dev_x((float)r->left); o.top = dev_y((float)r->top);
    o.right = dev_x((float)r->right); o.bottom = dev_y((float)r->bottom);
    return o;
}

static void text_render(const Text *src, const Clip *cl)
{
    Text dt = *src;
    const Text *t = &dt;
    Span lines[16];
    int nl, k, np = 0;
    float lh = (float)k_font_px[t->font] * 1.25f * s_S;
    float cap = s_gf_state > 0 ? gf_cap(t->font) : (float)k_font_px[t->font] * 0.72f * s_S;
    float block, y0;
    TextPass ps[64];
    dt.r = dev_rect(&src->r);
    nl = (t->fmt & DT_SINGLELINE) ? 1 : break_lines(t, lines, 16);
    block = lh * (float)nl;
    if (nl == 1 && (t->fmt & DT_SINGLELINE)) {
        lines[0].start = 0;
        lines[0].len = (int)strlen(t->s);
    }
    y0 = (t->fmt & DT_VCENTER) ? (float)t->r.top + ((float)(t->r.bottom - t->r.top) - block) * 0.5f : (float)t->r.top;
    if (t->fx == 2) {
        if (s_S < 1.75f) {
            static const int ring[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
            for (k = 0; k < 8; k++) {
                TextPass a, b;
                a.dx = pdev(2 * ring[k][0]); a.dy = pdev(2 * ring[k][1]); a.a = 0.10f; a.glow = 1;
                b.dx = pdev(ring[k][0]); b.dy = pdev(ring[k][1]); b.a = 0.16f; b.glow = 1;
                ps[np++] = a;
                ps[np++] = b;
            }
        } else {
            int n = s_S < 3.2f ? 16 : 24;
            float am = 8.0f / (float)n;
            for (k = 0; k < n; k++) {
                float ang = 6.2831853f * (float)k / (float)n, cs = cosf(ang), sn = sinf(ang);
                TextPass a, b;
                a.dx = (int)floorf(2.0f * s_S * cs + 0.5f); a.dy = (int)floorf(2.0f * s_S * sn + 0.5f);
                a.a = 0.10f * am; a.glow = 1;
                b.dx = (int)floorf(s_S * cs + 0.5f); b.dy = (int)floorf(s_S * sn + 0.5f);
                b.a = 0.16f * am; b.glow = 1;
                ps[np++] = a;
                ps[np++] = b;
            }
        }
    }
    if (t->fx == 1) {
        TextPass a, b, c, d;
        a.dx = pdev(2); a.dy = pdev(2); a.a = 0.20f; a.glow = 0;
        b.dx = pdev(1); b.dy = pdev(2); b.a = 0.26f; b.glow = 0;
        c.dx = pdev(1); c.dy = pdev(1); c.a = 0.34f; c.glow = 0;
        d.dx = 0; d.dy = pdev(1); d.a = 0.22f; d.glow = 0;
        ps[np++] = a; ps[np++] = b; ps[np++] = c; ps[np++] = d;
    } else {
        TextPass a, b, c, d;
        a.dx = pdev(1); a.dy = pdev(1); a.a = 0.70f; a.glow = 0;
        b.dx = pdev(1); b.dy = 0; b.a = 0.30f; b.glow = 0;
        c.dx = pdev(-1); c.dy = 0; c.a = 0.30f; c.glow = 0;
        d.dx = 0; d.dy = pdev(-1); d.a = 0.30f; d.glow = 0;
        ps[np++] = a; ps[np++] = b; ps[np++] = c; ps[np++] = d;
    }
    for (k = 0; k < np; k++)
        soft_run(t, lines, nl, y0, cap, lh, &ps[k], 0u, cl);
    {
        TextPass m;
        m.dx = 0; m.dy = 0; m.a = 1.0f; m.glow = 0;
        soft_run(t, lines, nl, y0, cap, lh, &m, t->c, cl);
    }
}

#ifdef _WIN32
static void gdi_text(const Text *t)
{
    RECT sh = t->r;
    GdiFlush();
    SetBkMode(s_dc, TRANSPARENT);
    SelectObject(s_dc, s_font[t->font]);
    OffsetRect(&sh, 1, 1);
    SetTextColor(s_dc, RGB(0, 0, 0));
    DrawTextA(s_dc, t->s, -1, &sh, t->fmt | DT_NOPREFIX);
    SetTextColor(s_dc, RGB((t->c >> 16) & 255, (t->c >> 8) & 255, t->c & 255));
    DrawTextA(s_dc, t->s, -1, (RECT *)&t->r, t->fmt | DT_NOPREFIX);
    GdiFlush();
}
#endif

static void text_flush(void)
{
    int i;
    gf_ready();
    for (i = 0; i < s_ntext; i++) {
        Text *t = &s_text[i];
        Item *it = item_new(IT_TEXT);
        uint32_t h = 2166136261u;
        const char *p;
        int ex;
        if (!it) break;
        it->u.tx = *t;
        ex = (int)ceilf((t->fx == 2 ? 6.0f : 3.0f) * s_S) + 2;
        it->bx0 = dev_x((float)t->r.left) - ex; it->by0 = dev_y((float)t->r.top) - ex;
        it->bx1 = dev_x((float)t->r.right) + ex; it->by1 = dev_y((float)t->r.bottom) + ex;
        h = hmix(h, IT_TEXT);
        h = hmix(h, (uint32_t)t->r.left); h = hmix(h, (uint32_t)t->r.top);
        h = hmix(h, (uint32_t)t->r.right); h = hmix(h, (uint32_t)t->r.bottom);
        h = hmix(h, t->c); h = hmix(h, t->gc); h = hmix(h, (uint32_t)t->font);
        h = hmix(h, (uint32_t)t->fx); h = hmix(h, t->fmt);
        for (p = t->s; *p; p++)
            h = hmix(h, (uint32_t)(unsigned char)*p);
        it->hash = h;
    }
    s_ntext = 0;
}

static void plate_px(const Shape *s, const Clip *cl);

static void item_render(const Item *it, const Clip *cl)
{
    switch (it->type) {
    case IT_RRECT: rrect_px(&it->u.sh, cl); break;
    case IT_PLATE: plate_px(&it->u.sh, cl); break;
    case IT_THUMB: thumb_px(&it->u.sh, cl); break;
    case IT_TEXT:
#ifdef _WIN32
        if (s_gf_state <= 0) {
            gdi_text(&it->u.tx);
            break;
        }
#endif
        text_render(&it->u.tx, cl);
        break;
    }
}

static void render_all(void)
{
    Clip c;
    int i;
    c.x0 = 0; c.y0 = 0; c.x1 = s_w; c.y1 = s_h;
    for (i = 0; i < s_nitem; i++)
        item_render(&s_item[i], &c);
    s_nitem = 0;
}

#define C_EDGE      0x3CC8E6u
#define C_EDGE_DIM  0x2E7896u
#define C_EDGE_HOT  0x8CF0FFu
#define C_TXT       0xD4E8FFu
#define C_TXT_HOT   0xFFFFFFu
#define C_TXT_OFF   0x6A88B0u
#define C_GOLD      0xF0D040u

static void button(int x0, int y0, int x1, int y1, const char *label, int hot, int off, const char *tag);
static void panel(int x0, int y0, int x1, int y1);
static void card(int x0, int y0, int x1, int y1);
static void cell(int x0, int y0, int x1, int y1, int hot, int off);

typedef struct { int x0, y0, x1, y1; } Box;

static int menu_draft(void)
{
    int d = s_env_draft ? s_env_draft : s_set.draft;
    const char *fn = getenv("AVP_MENU_DRAFT_FILE");
    if (fn && *fn) {
        FILE *f = fopen(fn, "r");
        int v = 0;
        if (f) {
            if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 4) d = v;
            fclose(f);
        }
    }
    return d < 1 || d > 4 ? 2 : d;
}

static void text_fx(int x0, int y0, int x1, int y1, const char *str, uint32_t c, int font, unsigned fmt, int fx, uint32_t gc)
{
    s_fx = fx;
    s_gc = gc;
    text(x0, y0, x1, y1, str, c, font, fmt);
    s_fx = 0;
    s_gc = 0;
}

static void plate(int x0, int y0, int x1, int y1, int cut, uint32_t top, uint32_t bot, float fill_a,
                  uint32_t edge, float bw, uint32_t lit, uint32_t shade)
{
    shape_add(IT_PLATE, x0, y0, x1, y1, cut, 0, top, bot, fill_a, edge, bw, lit, shade, NULL);
}

static void plate_px(const Shape *s, const Clip *cl)
{
    const float k = 0.70710678f;
    float X0 = (float)dev_x((float)s->x0), Y0 = (float)dev_y((float)s->y0);
    float X1 = (float)dev_x((float)s->x1), Y1 = (float)dev_y((float)s->y1);
    float cut = (float)s->cut * s_S, bw = dev_bw(s->bw), hl = s_S >= 1.5f ? fmaxf(1.0f, floorf(s_S + 0.5f)) : 1.0f;
    float spanv;
    int x, y, ya, yb, xa, xb;
    if (X1 < X0 + 1 && s->x1 > s->x0) X1 = X0 + 1;
    if (Y1 < Y0 + 1 && s->y1 > s->y0) Y1 = Y0 + 1;
    spanv = Y1 - Y0 > 0 ? Y1 - Y0 : 1;
    ya = (int)Y0 - 1; yb = (int)Y1;
    xa = (int)X0 - 1; xb = (int)X1;
    if (ya < cl->y0) ya = cl->y0;
    if (yb > cl->y1 - 1) yb = cl->y1 - 1;
    if (xa < cl->x0) xa = cl->x0;
    if (xb > cl->x1 - 1) xb = cl->x1 - 1;
    for (y = ya; y <= yb; y++) {
        uint32_t fc = lerp_c(s->top, s->bot, clampf(((float)y - Y0) / spanv));
        uint32_t *row = &s_px[(size_t)y * s_w];
        for (x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, d, fa, ea;
            uint32_t *p;
            d = fmaxf(fmaxf(X0 - px, px - X1), fmaxf(Y0 - py, py - Y1));
            d = fmaxf(d, (cut - ((px - X0) + (py - Y0))) * k);
            d = fmaxf(d, (cut - ((X1 - px) + (Y1 - py))) * k);
            fa = clampf(0.5f - d);
            if (fa <= 0) continue;
            p = &row[x];
            *p = mix(*p, fc, fa * s->fa);
            if (bw > 0) {
                ea = fa - clampf(0.5f - (d + bw));
                if (ea > 0) *p = mix(*p, s->edge, ea);
                if (d < -bw && d >= -bw - hl) {
                    float near_tl = (px - X0) + (py - Y0), near_br = (X1 - px) + (Y1 - py);
                    *p = mix(*p, near_tl < near_br ? s->lit : s->shade, 0.5f);
                }
            }
        }
    }
}

static void square_mark(int x, int y, int n, uint32_t c)
{
    rrect(x, y, x + n, y + 2, 0, c, c, 1, 0, 0);
    rrect(x, y + n - 2, x + n, y + n, 0, c, c, 1, 0, 0);
    rrect(x, y + 2, x + 2, y + n - 2, 0, c, c, 1, 0, 0);
    rrect(x + n - 2, y + 2, x + n, y + n - 2, 0, c, c, 1, 0, 0);
}

static int plate_cut(int y0, int y1)
{
    int c = (int)((float)(y1 - y0) * 0.34f);
    return c > 9 ? 9 : c < 2 ? 2 : c;
}

static void cell(int x0, int y0, int x1, int y1, int hot, int off)
{
    int cut = plate_cut(y0, y1);
    if (off)
        plate(x0, y0, x1, y1, cut, 0x16283Fu, 0x0A121Eu, 0.50f, 0x3E5678u, 1.5f, 0x5A7090u, 0x040810u);
    else if (hot)
        plate(x0, y0, x1, y1, cut, 0x2B6FB0u, 0x123F78u, 0.86f, 0x8CF0FFu, 1.6f, 0xD8FAFFu, 0x0A2448u);
    else
        plate(x0, y0, x1, y1, cut, 0x1B4478u, 0x0A2040u, 0.58f, 0x3C94D0u, 1.5f, 0x7CC8F4u, 0x040E20u);
}

static void button(int x0, int y0, int x1, int y1, const char *label, int hot, int off, const char *tag)
{
    int mk = y1 - y0 >= 22 ? 6 : 4, tx = x0 + (mk == 6 ? 24 : 18);
    cell(x0, y0, x1, y1, hot && !off, off);
    if (hot && !off) square_mark(x0 + 10, (y0 + y1) / 2 - mk / 2, mk, 0xE0D040u);
    text(tx, y0, x1 - 8, y1, label, off ? C_TXT_OFF : hot ? C_TXT_HOT : 0xCFE4FAu, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (tag)
        text(tx, y0, x1 - 12, y1, tag, off ? C_TXT_OFF : C_GOLD, F_BODY,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void panel(int x0, int y0, int x1, int y1)
{
    plate(x0, y0, x1, y1, 16, 0x0C2244u, 0x050F26u, 0.93f, 0x3C94D0u, 1.8f, 0x7CC8F4u, 0x040E20u);
}

static void card(int x0, int y0, int x1, int y1)
{
    plate(x0, y0, x1, y1, 12, 0x0E2850u, 0x061430u, 0.80f, 0x2E7896u, 1.5f, 0x4C88B4u, 0x040E20u);
}

static void main_native(const Box *b, const char *label, int hot)
{
    float rad = (float)(b->y1 - b->y0) * 0.34f;
    if (hot) {
        rrect(b->x0, b->y0, b->x1, b->y1, rad, 0x3F7BBFu, 0x1E4C92u, 0.97f, 0x49E1EAu, 2.2f);
        rrect(b->x0 + 8, b->y0 + 3, b->x1 - 8, b->y0 + 5, 1, 0xFFFFFFu, 0xFFFFFFu, 0.20f, 0, 0);
    } else {
        rrect(b->x0, b->y0, b->x1, b->y1, rad, 0x2F5C98u, 0x23487Cu, 0.52f, 0x49D8E2u, 2.0f);
    }
    text(b->x0 + 14, b->y0, b->x1 - 10, b->y1, label, hot ? 0xFFFFFFu : 0xCFE4FAu, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void main_panel_item(const Box *b, const char *label, int hot)
{
    if (hot)
        rrect(b->x0, b->y0, b->x1, b->y1, (float)(b->y1 - b->y0) * 0.5f, 0x3F7BBFu, 0x1B4A90u, 0.95f, 0x8CF0FFu, 2.2f);
    else
        rrect(b->x0, b->y0, b->x1, b->y1, (float)(b->y1 - b->y0) * 0.5f, 0x1F427Au, 0x10295Au, 0.86f, 0x3AB4D0u, 2.0f);
    text(b->x0 + 14, b->y0, b->x1 - 10, b->y1, label, hot ? 0xFFFFFFu : 0xCFE4FAu, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void main_minimal(const Box *b, const char *label, int hot)
{
    if (hot) {
        rrect(b->x0 - 9, b->y0 + 2, b->x0 - 6, b->y1 - 2, 0, 0xA6E81Cu, 0x81C301u, 1, 0, 0);
        text_fx(b->x0 + 4, b->y0, b->x1, b->y1, label, 0xC8F545u, F_TITLE,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 2, 0x81C301u);
    } else {
        text_fx(b->x0 + 4, b->y0, b->x1, b->y1, label, 0x7FA6D0u, F_TITLE,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 1, 0);
    }
}

static void main_plate(const Box *b, const char *label, int hot)
{
    if (hot) {
        plate(b->x0, b->y0, b->x1, b->y1, 9, 0x2B6FB0u, 0x123F78u, 0.86f, 0x8CF0FFu, 1.6f, 0xD8FAFFu, 0x0A2448u);
        square_mark(b->x0 + 11, (b->y0 + b->y1) / 2 - 3, 6, 0xE0D040u);
    } else {
        plate(b->x0, b->y0, b->x1, b->y1, 9, 0x1B4478u, 0x0A2040u, 0.58f, 0x3C94D0u, 1.5f, 0x7CC8F4u, 0x040E20u);
    }
    text(b->x0 + 24, b->y0, b->x1 - 10, b->y1, label, hot ? 0xFFFFFFu : 0xCFE4FAu, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

enum { SCR_MAIN, SCR_MP, SCR_SKIRM, SCR_SETTINGS, SCR_QUIT, SCR_KEYS,
       SCR_EDITOR, SCR_HOST, SCR_JOIN, SCR_COOP, SCR_COOPMODE, SCR_SKCUST, SCR_NEDOPEN, SCR_EDLAUNCH };

typedef struct { const char *label; int game; int screen; const char *tag; int vis; } MainItem;
static const MainItem k_main[] = {
    { "Campaign",       0, -1, NULL, 1 },
    { "Co-op",           -1, SCR_COOPMODE, NULL, 1 },
    { "Versus",         -1, SCR_MP, NULL, 0 },
    { "Skirmish vs AI", -1, SCR_SKIRM, NULL, 0 },
    { "Map Editor",     -1, SCR_EDITOR, NULL, 0 },
    { "Load Game",      1, -1, NULL, 1 },
    { "Tutorials",      2, -1, NULL, 1 },
    { "Game Options",   3, -1, NULL, 1 },
    { "PC Settings",    -1, SCR_SETTINGS, NULL, 1 },
    { "Bestiary",       4, -1, NULL, 1 },
    { "Extras",         5, -1, NULL, 1 },
    { "Quit",           -1, SCR_QUIT, NULL, 1 },
};
static const MainItem k_main_mp[] = {
    { "Skirmish",        -1, SCR_SKCUST, NULL, 1 },
    { "PC Settings",     -1, SCR_SETTINGS, NULL, 1 },
    { "Nostromo Editor", -1, SCR_EDLAUNCH, NULL, 1 },
    { "Quit",            -1, SCR_QUIT, NULL, 1 },
};
static const MainItem *main_tab(int *n)
{
#if APP_MODE == 1
    *n = (int)(sizeof k_main_mp / sizeof k_main_mp[0]);
    return k_main_mp;
#else
    *n = (int)(sizeof k_main / sizeof k_main[0]);
    return k_main;
#endif
}
#define MAIN_X0 402
#define MAIN_X1 598
#define MAIN_Y0 140
#define MAIN_PITCH 30
#define MAIN_H 26
#define MAIN_PAD 14

static const MainItem *main_item(int i)
{
    int k, na;
    const MainItem *t = main_tab(&na);
    for (k = 0; k < na; k++)
        if (t[k].vis && i-- == 0) return &t[k];
    return &t[0];
}

static int main_count(void)
{
    int k, na, n = 0;
    const MainItem *t = main_tab(&na);
    for (k = 0; k < na; k++)
        n += t[k].vis;
    return n;
}

enum { ST_DISPLAY, ST_RES, ST_CAP, ST_VSYNC, ST_WIDE, ST_STYLE, ST_INPUT, ST_EDGE, ST_GLIDE, ST_KEYS, ST_NAME, ST_COOP, ST_BACK };

#define SET_VT 154
#define SET_VB 392
#define SET_VH (SET_VB - SET_VT)
#define SET_HEAD_H 24
#define SET_PITCH 29
#define SET_ROW_H 25

static HWND s_hwnd;
static int s_screen, s_sel, s_sel_main, s_forced, s_editing, s_mode, s_tf, s_join_logged;
static int s_coop_from, s_co_side, s_co_col, s_set_scroll;
static char s_ip[64], s_code[8];
static int s_co_steam;
static const char k_code_alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

enum { TF_NONE, TF_PORT, TF_IP, TF_CODE };
static int s_handoff;
static DWORD s_handoff_t;
static int s_mx = -1, s_my = -1;
static int s_was_up;

static Box s_box[48];
static int s_nbox;

#define KB_VIS 7
#define KB_ROW_Y 190
#define KB_ROW_H 24
#define KB_COL_SPLIT 433
#define KB_NAME_END 326

static int s_kb_acts[AVP_ACTION_COUNT], s_kb_n;
static int s_kb_pos, s_kb_col, s_kb_scroll, s_kb_cap;
static char s_kb_err[96];
static DWORD s_kb_err_t;

static const int k_rows_all[] = { ST_DISPLAY, ST_RES, ST_CAP, ST_VSYNC, ST_WIDE, ST_STYLE, ST_INPUT, ST_EDGE, ST_GLIDE,
                                  ST_KEYS, ST_NAME, ST_COOP, ST_BACK };

static const int *rows_of(int screen, int *n)
{
    if (screen == SCR_SETTINGS) {
        *n = (int)(sizeof k_rows_all / sizeof k_rows_all[0]);
        return k_rows_all;
    }
    *n = 0;
    return NULL;
}

static const char *head_before(int id)
{
    switch (id) {
    case ST_DISPLAY: return "Display";
    case ST_STYLE: return "Menu";
    case ST_INPUT: return "Controls";
    case ST_NAME: return "Player";
    case ST_COOP: return "Online";
    }
    return NULL;
}

static int set_row_y(int i)
{
    int k, y = 0;
    for (k = 0; k <= i && k < ST_BACK; k++) {
        if (head_before(k_rows_all[k])) y += SET_HEAD_H;
        if (k == i) return y;
        y += SET_PITCH;
    }
    return y;
}

static int set_total(void)
{
    return set_row_y(ST_BACK - 1) + SET_ROW_H;
}

static int set_vis(int i)
{
    int y = set_row_y(i) - s_set_scroll;
    return y >= 0 && y + SET_ROW_H <= SET_VH;
}

static void set_view(void)
{
    int max = set_total() - SET_VH, i = s_sel;
    if (max < 0) max = 0;
    if (i >= 0 && i < ST_BACK) {
        int y = set_row_y(i), lo = y - (head_before(k_rows_all[i]) ? SET_HEAD_H : 0);
        if (lo < s_set_scroll) s_set_scroll = lo;
        if (y + SET_ROW_H > s_set_scroll + SET_VH) s_set_scroll = y + SET_ROW_H - SET_VH;
    }
    if (s_set_scroll > max) s_set_scroll = max;
    if (s_set_scroll < 0) s_set_scroll = 0;
}

static int is_settings(int screen)
{
    return screen == SCR_SETTINGS || screen == SCR_KEYS;
}

static int row_id(int i)
{
    int n;
    const int *r = rows_of(s_screen, &n);
    return r && i >= 0 && i < n ? r[i] : -1;
}

static int is_value_row(int id)
{
    return id == ST_DISPLAY || id == ST_RES || id == ST_CAP || id == ST_VSYNC || id == ST_WIDE || id == ST_STYLE || id == ST_INPUT
        || id == ST_EDGE || id == ST_GLIDE;
}

static void kb_build(void)
{
    int a;
    s_kb_n = 0;
    for (a = 0; a < AVP_ACTION_COUNT; a++)
        if (avp_keys_rebindable(a)) s_kb_acts[s_kb_n++] = a;
}

static void kb_view(void)
{
    int max = s_kb_n > KB_VIS ? s_kb_n - KB_VIS : 0;
    if (s_kb_pos < s_kb_n) {
        if (s_kb_pos < s_kb_scroll) s_kb_scroll = s_kb_pos;
        if (s_kb_pos >= s_kb_scroll + KB_VIS) s_kb_scroll = s_kb_pos - KB_VIS + 1;
    }
    if (s_kb_scroll > max) s_kb_scroll = max;
    if (s_kb_scroll < 0) s_kb_scroll = 0;
}

extern ptrdiff_t g_xbox_mem_offset;

#define GM32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GM_REGIONS     0x002FFDF0u
#define GM_REGIONS_END 0x002CC290u
#define GM_VIEWPORTS   0x002F6CC4u

typedef struct { int x0, x1, y0, pitch, h, bottom; } Column;

static int column_probe(Column *c)
{
    uint32_t end, e, vp, vpp;
    int l0, t0, r0, b0, n = 0, i, j;
    int rl[64], rr[64], rt[64], rb[64];
    int bl = 0, br = 0, bn = 0, ys[64], ny = 0, h = 0, top, bot, pitch = 0;
    if (!g_xbox_mem_offset) return 0;
    vpp = ((int32_t)GM32(GM_VIEWPORTS + 4) - 1 < 0) ? GM_VIEWPORTS + 0xC : GM32(GM_VIEWPORTS);
    vp = vpp ? GM32(vpp) : 0;
    if (!vp) return 0;
    l0 = (int32_t)GM32(vp + 4); t0 = (int32_t)GM32(vp + 8);
    r0 = (int32_t)GM32(vp + 0xC); b0 = (int32_t)GM32(vp + 0x10);
    if (r0 <= l0 || b0 <= t0) return 0;
    end = GM32(GM_REGIONS_END);
    if (end < GM_REGIONS || end > GM_REGIONS + 20u * 8192u) return 0;
    for (e = GM_REGIONS; e + 20 <= end && n < 64; e += 20) {
        int l, t, r, b;
        if (!GM32(e)) continue;
        l = ((int32_t)GM32(e + 4) - l0) * 640 / (r0 - l0);
        t = ((int32_t)GM32(e + 8) - t0) * 480 / (b0 - t0);
        r = ((int32_t)GM32(e + 12) - l0) * 640 / (r0 - l0);
        b = ((int32_t)GM32(e + 16) - t0) * 480 / (b0 - t0);
        if (l < 300 || r > 640 || r - l < 120 || r - l > 330 || b - t < 14 || b - t > 60 || t < 60 || b > 470)
            continue;
        rl[n] = l; rr[n] = r; rt[n] = t; rb[n] = b; n++;
    }
    for (i = 0; i < n; i++) {
        int cnt = 0;
        for (j = 0; j < n; j++)
            if (abs(rl[j] - rl[i]) <= 2 && abs(rr[j] - rr[i]) <= 2) cnt++;
        if (cnt > bn) { bn = cnt; bl = rl[i]; br = rr[i]; }
    }
    if (bn < 4) return 0;
    top = 100000; bot = 0;
    for (i = 0; i < n; i++) {
        if (abs(rl[i] - bl) > 2 || abs(rr[i] - br) > 2) continue;
        if (rt[i] < top) top = rt[i];
        if (rb[i] > bot) bot = rb[i];
        if (rb[i] - rt[i] > h) h = rb[i] - rt[i];
        ys[ny++] = rt[i];
    }
    for (i = 0; i < ny; i++)
        for (j = i + 1; j < ny; j++)
            if (ys[j] < ys[i]) { int t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    for (i = 1; i < ny; i++)
        if (ys[i] - ys[i - 1] > 2) { pitch = ys[i] - ys[i - 1]; break; }
    if (pitch < h || pitch > 80) return 0;
    c->x0 = bl; c->x1 = br; c->y0 = top; c->pitch = pitch; c->h = h; c->bottom = bot;
    return 1;
}

static Column s_col = { MAIN_X0, MAIN_X1, MAIN_Y0, MAIN_PITCH, MAIN_H, 0 };
static Column s_col_cand;
static int s_col_lock, s_col_same, s_col_miss;

static void column_reset(void)
{
    s_col_lock = 0;
    s_col_same = 0;
    s_col_miss = 0;
}

static void column_update(void)
{
    Column c;
    if (s_col_lock) return;
    if (!getenv("AVP_MENU_PROBE")) {
        Column d = { MAIN_X0, MAIN_X1, MAIN_Y0, MAIN_PITCH, MAIN_H, 0 };
        s_col = d;
        s_col_lock = 1;
        return;
    }
    if (column_probe(&c)) {
        s_col_miss = 0;
        if (!memcmp(&c, &s_col_cand, sizeof c)) s_col_same++;
        else { s_col_cand = c; s_col_same = 0; }
        if (s_col_same >= 8) { s_col = c; s_col_lock = 1; }
    } else {
        s_col_same = 0;
        if (++s_col_miss >= 90) {
            Column d = { MAIN_X0, MAIN_X1, MAIN_Y0, MAIN_PITCH, MAIN_H, 0 };
            s_col = d;
            s_col_lock = 1;
        }
    }
}

static void layout_main(void)
{
    int i, n = main_count();
    s_nbox = 0;
    for (i = 0; i < n; i++) {
        Box b = { s_col.x0, s_col.y0 + i * s_col.pitch, s_col.x1, s_col.y0 + i * s_col.pitch + s_col.h };
        s_box[s_nbox++] = b;
    }
}

static void layout_settings(void)
{
    int i;
    Box bk = { 96, 402, 230, 427 };
    set_view();
    s_nbox = 0;
    for (i = 0; i < ST_BACK; i++) {
        int y = set_row_y(i) - s_set_scroll;
        Box b = { 96, SET_VT + y, 544, SET_VT + y + SET_ROW_H };
        if (!set_vis(i)) b.x0 = b.y0 = b.x1 = b.y1 = 0;
        s_box[s_nbox++] = b;
    }
    s_box[s_nbox++] = bk;
}

#define CO_Y0 174
#define CO_PITCH 30
#define CO_H 24
#define CO_ROWS 7
#define CO_TAB0 2
#define CO_ROW0 5
#define CO_START 12
#define CO_ADDR 13
#define CO_JPORT 14
#define CO_JOIN 15
#define CO_REC0 16
#define CO_BACK 19

static void layout_coop(void)
{
    int i;
    s_nbox = 0;
    {
        Box b0 = { 62, CO_Y0, 298, CO_Y0 + CO_H }, b1 = { 62, CO_Y0 + CO_PITCH, 298, CO_Y0 + CO_PITCH + CO_H };
        s_box[s_nbox++] = b0;
        s_box[s_nbox++] = b1;
    }
    for (i = 0; i < 3; i++) {
        Box b = { 62 + i * 79, 234, 62 + i * 79 + 76, 254 };
        s_box[s_nbox++] = b;
    }
    for (i = 0; i < CO_ROWS; i++) {
        Box b = { 62, 258 + i * 21, 298, 258 + i * 21 + 19 };
        s_box[s_nbox++] = b;
    }
    {
        Box b = { 62, 406, 298, 426 };
        s_box[s_nbox++] = b;
    }
    for (i = 0; i < 2; i++) {
        Box b = { 342, CO_Y0 + i * CO_PITCH, 578, CO_Y0 + i * CO_PITCH + CO_H };
        s_box[s_nbox++] = b;
    }
    {
        Box b = { 342, CO_Y0 + 2 * CO_PITCH, 578, CO_Y0 + 2 * CO_PITCH + CO_H };
        s_box[s_nbox++] = b;
    }
    for (i = 0; i < 3; i++) {
        Box b = { 342, 336 + i * 24, 578, 336 + i * 24 + 22 };
        s_box[s_nbox++] = b;
    }
    {
        Box b = { 50, 440, 190, 464 };
        s_box[s_nbox++] = b;
    }
}

static void layout_coopmode(void)
{
    Box ip = { 200, 190, 440, 218 }, st = { 200, 228, 440, 256 }, bk = { 200, 284, 320, 310 };
    s_nbox = 0;
    s_box[s_nbox++] = ip;
    s_box[s_nbox++] = st;
    s_box[s_nbox++] = bk;
}

static void layout_keys(void)
{
    int i;
    Box reset = { 96, 384, 270, 409 }, bk = { 282, 384, 400, 409 };
    s_nbox = 0;
    for (i = 0; i < KB_VIS; i++) {
        Box b = { 96, KB_ROW_Y + i * KB_ROW_H, 544, KB_ROW_Y + i * KB_ROW_H + KB_ROW_H - 2 };
        if (s_kb_scroll + i >= s_kb_n) b.x0 = b.y0 = b.x1 = b.y1 = 0;
        s_box[s_nbox++] = b;
    }
    s_box[s_nbox++] = reset;
    s_box[s_nbox++] = bk;
}

static void layout_single(int x0, int y0, int x1, int y1)
{
    Box b = { x0, y0, x1, y1 };
    s_nbox = 0;
    s_box[s_nbox++] = b;
}

#define SK_ROWS 4
#define SK_CELLS 4
#define SK_START (1 + SK_ROWS * SK_CELLS)
#define SK_BACK (SK_START + 1)

static const int k_sk_cx[SK_CELLS][2] = { { 108, 176 }, { 180, 264 }, { 268, 322 }, { 326, 380 } };

static void layout_skirm(void)
{
    int r, c;
    Box m = { 40, 84, 380, 109 }, st = { 40, 400, 190, 426 }, bk = { 202, 400, 332, 426 };
    s_nbox = 0;
    s_box[s_nbox++] = m;
    for (r = 0; r < SK_ROWS; r++)
        for (c = 0; c < SK_CELLS; c++) {
            Box b = { k_sk_cx[c][0], 140 + r * 33, k_sk_cx[c][1], 140 + r * 33 + 28 };
            s_box[s_nbox++] = b;
        }
    s_box[s_nbox++] = st;
    s_box[s_nbox++] = bk;
}

static void layout_mp(void)
{
    int i;
    Box bk = { 120, 384, 260, 408 };
    s_nbox = 0;
    for (i = 0; i < 4; i++) {
        int x0 = 120 + (i / 2) * 205, y0 = 280 + (i & 1) * 30;
        Box b = { x0 + 10, y0, x0 + 185, y0 + 24 };
        s_box[s_nbox++] = b;
    }
    s_box[s_nbox++] = bk;
}

static void layout_host(void)
{
    s_nbox = 0;
    if (s_mode) {
        int r, c;
        Box p = { 40, 84, 190, 109 }, h = { 202, 84, 380, 109 }, m = { 40, 138, 380, 163 };
        Box st = { 40, 400, 190, 426 }, bk = { 202, 400, 332, 426 };
        s_box[s_nbox++] = p;
        s_box[s_nbox++] = h;
        s_box[s_nbox++] = m;
        for (r = 0; r < SK_ROWS; r++)
            for (c = 0; c < SK_CELLS; c++) {
                Box b = { k_sk_cx[c][0], 190 + r * 33, k_sk_cx[c][1], 190 + r * 33 + 28 };
                s_box[s_nbox++] = b;
            }
        s_box[s_nbox++] = st;
        s_box[s_nbox++] = bk;
    } else {
        Box p = { 100, 164, 290, 189 }, h = { 302, 164, 540, 189 }, st = { 100, 350, 290, 376 }, bk = { 302, 350, 420, 376 };
        s_box[s_nbox++] = p;
        s_box[s_nbox++] = h;
        s_box[s_nbox++] = st;
        s_box[s_nbox++] = bk;
    }
}

static void layout_join(void)
{
    int i;
    Box ip = { 100, 164, 380, 189 }, pt = { 392, 164, 540, 189 }, jn = { 100, 198, 290, 224 };
    Box go = { 302, 198, 540, 224 }, bk = { 100, 400, 260, 426 };
    s_nbox = 0;
    s_box[s_nbox++] = ip;
    s_box[s_nbox++] = pt;
    s_box[s_nbox++] = jn;
    s_box[s_nbox++] = go;
    for (i = 0; i < 3; i++) {
        Box b = { 100, 306 + i * 28, 540, 306 + i * 28 + 24 };
        s_box[s_nbox++] = b;
    }
    s_box[s_nbox++] = bk;
}

static void layout_editor(void)
{
    Box m = { 90, 164, 370, 190 }, ed = { 90, 376, 230, 402 }, bk = { 242, 376, 360, 402 };
    s_nbox = 0;
    s_box[s_nbox++] = m;
    s_box[s_nbox++] = ed;
    s_box[s_nbox++] = bk;
}

#define KS_ROWS 6
#define KS_FOLDER 6
#define KS_SLOT0 7
#define KS_MAXSLOT 8
#define KS_HOST (KS_SLOT0 + KS_MAXSLOT * 3)
#define KS_JOIN (KS_HOST + 1)
#define KS_START (KS_HOST + 2)
#define KS_BACK (KS_HOST + 3)

static int ks_nslots(void);

static void layout_skcust(void)
{
    int i, n = ks_nslots();
    Box b, z = { 0, 0, 0, 0 };
    s_nbox = 0;
    for (i = 0; i < KS_ROWS; i++) {
        b.x0 = 40; b.y0 = 108 + i * 22; b.x1 = 250; b.y1 = 108 + i * 22 + 20;
        s_box[s_nbox++] = b;
    }
    b.x0 = 40; b.y0 = 244; b.x1 = 250; b.y1 = 266;
    s_box[s_nbox++] = b;
    for (i = 0; i < KS_MAXSLOT; i++) {
        int y = 104 + i * 29;
        b.y0 = y; b.y1 = y + 25;
        b.x0 = 296; b.x1 = 376; s_box[s_nbox++] = i < n ? b : z;
        b.x0 = 380; b.x1 = 480; s_box[s_nbox++] = i < n ? b : z;
        b.x0 = 484; b.x1 = 560; s_box[s_nbox++] = i < n ? b : z;
    }
    b.y0 = 412; b.y1 = 436;
    b.x0 = 40; b.x1 = 150; s_box[s_nbox++] = b;
    b.x0 = 156; b.x1 = 266; s_box[s_nbox++] = b;
    b.x0 = 272; b.x1 = 382; s_box[s_nbox++] = b;
    b.x0 = 388; b.x1 = 480; s_box[s_nbox++] = b;
}

static void layout_nedopen(void)
{
    int i;
    Box b;
    s_nbox = 0;
    for (i = 0; i < KS_ROWS; i++) {
        b.x0 = 90; b.y0 = 160 + i * 24; b.x1 = 400; b.y1 = b.y0 + 22;
        s_box[s_nbox++] = b;
    }
    b.x0 = 90; b.y0 = 376; b.x1 = 230; b.y1 = 402; s_box[s_nbox++] = b;
    b.x0 = 242; b.x1 = 360; s_box[s_nbox++] = b;
}

static void layout_quit(void)
{
    Box yes = { 214, 262, 314, 288 }, no = { 326, 262, 426, 288 };
    s_nbox = 0;
    s_box[s_nbox++] = yes;
    s_box[s_nbox++] = no;
}

static void layout(void)
{
    switch (s_screen) {
    case SCR_MAIN: layout_main(); break;
    case SCR_SETTINGS: layout_settings(); break;
    case SCR_COOP: layout_coop(); break;
    case SCR_COOPMODE: layout_coopmode(); break;
    case SCR_KEYS: layout_keys(); break;
    case SCR_MP: layout_mp(); break;
    case SCR_HOST: layout_host(); break;
    case SCR_JOIN: layout_join(); break;
    case SCR_SKIRM: layout_skirm(); break;
    case SCR_EDITOR: layout_editor(); break;
    case SCR_SKCUST: layout_skcust(); break;
    case SCR_NEDOPEN: layout_nedopen(); break;
    case SCR_QUIT: layout_quit(); break;
    }
}

static void go(int screen);
static void back(void);

static const char k_codes[][4] = {
    "A01", "A02", "A03", "A04", "A06", "A07", "A08",
    "M01", "M02", "M03", "M05", "M06", "M09", "M10",
    "P01", "P02", "P03", "P04", "P06", "P08", "P11",
    "T01", "T02", "T03", "T04", "T05"
};
#define N_CODES ((int)(sizeof k_codes / sizeof k_codes[0]))

typedef struct { int state; uint32_t px[AVP_THUMB_W * AVP_THUMB_H]; char info[96]; } Preview;
static Preview s_prev[N_CODES];

static void preview_reset(void)
{
    int i;
    for (i = 0; i < N_CODES; i++)
        s_prev[i].state = 0;
}

static void preview_fill(int k)
{
    Preview *p = &s_prev[k];
    AvpLayout *lay = malloc(sizeof *lay);
    int r, n;
    p->state = avp_skirmish_thumb_load(k_codes[k], p->px) ? 1 : -1;
    snprintf(p->info, sizeof p->info, "No layout yet");
    if (!lay)
        return;
    if (avp_layout_load(lay, k_codes[k]) >= 0) {
        int len = snprintf(p->info, sizeof p->info, "Spawns:");
        for (r = 0; r < AVP_RACE_COUNT; r++) {
            int any = 0;
            len += snprintf(p->info + len, sizeof p->info - len, "%s %s", r ? "," : "", avp_race_name((AvpRace)r));
            for (n = 0; n < 3; n++)
                if (lay->spawn[r][n].used) {
                    len += snprintf(p->info + len, sizeof p->info - len, " %d", n + 1);
                    any = 1;
                }
            if (!any)
                len += snprintf(p->info + len, sizeof p->info - len, " -");
        }
    }
    free(lay);
}

static void map_preview(int x, int y, int k)
{
    Preview *p = &s_prev[k];
    const char *code = k_codes[k];
    char buf[48];
    static const char *const kind[] = { "Alien mission", "Marine mission", "Predator mission", "Tutorial" };
    int cls = code[0] == 'A' ? 0 : code[0] == 'M' ? 1 : code[0] == 'P' ? 2 : 3;
    if (!p->state)
        preview_fill(k);
    if (p->state > 0) {
        shape_add(IT_THUMB, x, y, x + AVP_THUMB_W, y + AVP_THUMB_H, 0, 0, 0, 0, 0, 0, 0, 0, 0, p->px);
        rrect(x - 2, y - 2, x + AVP_THUMB_W + 2, y + AVP_THUMB_H + 2, 2, 0, 0, 0, C_EDGE_DIM, 2);
    } else {
        rrect(x, y, x + AVP_THUMB_W, y + AVP_THUMB_H, 4, 0x0A1A38u, 0x06112Au, 1.0f, C_EDGE_DIM, 2);
        text(x + 12, y + 8, x + AVP_THUMB_W - 12, y + AVP_THUMB_H - 8,
             "No preview yet - play or edit this map once", C_TXT_OFF, F_BODY,
             DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }
    snprintf(buf, sizeof buf, "%s %d", kind[cls], atoi(code + 1));
    text(x, y + AVP_THUMB_H + 6, x + AVP_THUMB_W, y + AVP_THUMB_H + 26, buf, C_TXT_HOT, F_BODY,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(x, y + AVP_THUMB_H + 28, x + AVP_THUMB_W, y + AVP_THUMB_H + 68, p->info, C_TXT, F_BODY,
         DT_LEFT | DT_WORDBREAK);
}

enum { K_YOU, K_AI, K_REMOTE, K_CLOSED };
typedef struct { int kind, race, spawn, team; } SkRow;
static SkRow s_sk[SK_ROWS];
static int s_sk_map;
static char s_sk_err[96];
static DWORD s_sk_err_t;

static const char *const k_kind_name[4] = { "You", "AI", "Remote", "Closed" };
static const char *const k_race_name[AVP_RACE_COUNT] = { "Alien", "Marine", "Predator" };

static int sk_star(const char *code)
{
    return strstr(" A06 A08 M03 M06 M09 M10 P06 P11 ", code) != NULL;
}

static void sk_load(void)
{
    static const SkRow def[SK_ROWS] = {
        { K_YOU, AVP_RACE_MARINE, 1, 1 }, { K_AI, AVP_RACE_ALIEN, 1, 2 },
        { K_AI, AVP_RACE_PREDATOR, 1, 3 }, { K_CLOSED, AVP_RACE_ALIEN, 2, 4 }
    };
    AvpSkirmishCfg c;
    int i, you = 0;
    memset(&c, 0, sizeof c);
    avp_skirmish_config_load(&c);
    for (i = 0; i < N_CODES; i++)
        if (!strcmp(k_codes[i], c.map)) s_sk_map = i;
    memcpy(s_sk, def, sizeof s_sk);
    if (c.nplayers > 0) {
        for (i = 0; i < SK_ROWS; i++) {
            if (i < c.nplayers) {
                const AvpSkirmishPlayer *p = &c.player[i];
                s_sk[i].kind = p->kind == AVP_PK_HUMAN ? K_YOU : p->kind == AVP_PK_REMOTE ? K_REMOTE : K_AI;
                s_sk[i].race = p->race >= 0 && p->race < AVP_RACE_COUNT ? (int)p->race : AVP_RACE_ALIEN;
                s_sk[i].spawn = p->spawn >= 1 && p->spawn <= 3 ? p->spawn : 1;
                s_sk[i].team = p->team >= 1 && p->team <= 4 ? p->team : i + 1;
            } else {
                s_sk[i].kind = K_CLOSED;
            }
        }
    }
    for (i = 0; i < SK_ROWS; i++) {
        if (s_sk[i].kind != K_YOU) continue;
        if (you++) s_sk[i].kind = K_AI;
    }
    if (!you) s_sk[0].kind = K_YOU;
    *s_sk_err = 0;
}

static void sk_cycle(int row, int col)
{
    SkRow *r = &s_sk[row];
    int i;
    if (col == 0) {
        if (r->kind == K_YOU) return;
        if (s_screen == SCR_HOST) {
            if (row > 1) r->kind = r->kind == K_AI ? K_CLOSED : K_AI;
            return;
        }
        if (r->kind == K_CLOSED) {
            for (i = 0; i < SK_ROWS; i++)
                if (s_sk[i].kind == K_YOU) s_sk[i].kind = K_AI;
            r->kind = K_YOU;
        } else {
            r->kind++;
            if (s_screen == SCR_SKIRM && r->kind == K_REMOTE) r->kind = K_CLOSED;
        }
        return;
    }
    if (r->kind == K_CLOSED) return;
    if (col == 1) r->race = (r->race + 1) % AVP_RACE_COUNT;
    else if (col == 2) r->spawn = r->spawn % 3 + 1;
    else r->team = r->team % 4 + 1;
}

static void sk_map(int dir)
{
    s_sk_map = (s_sk_map + dir + N_CODES) % N_CODES;
    sound(1);
}

static int disabled(int i);

static void handoff_campaign(void)
{
    s_sel_main = 0;
    s_forced = 0;
    InterlockedExchange(&g_avp_menu_req, 0);
    s_handoff = 1;
    s_handoff_t = GetTickCount();
    go(SCR_MAIN);
}

static void sk_fail(void)
{
    snprintf(s_sk_err, sizeof s_sk_err, "Could not write skirmish.cfg");
    s_sk_err_t = GetTickCount();
}

static int sk_build(AvpSkirmishCfg *cp)
{
    AvpSkirmishCfg c;
    int i, n = 0;
    memset(&c, 0, sizeof c);
    snprintf(c.map, sizeof c.map, "%s", k_codes[s_sk_map]);
    for (i = 0; i < SK_ROWS; i++) {
        AvpSkirmishPlayer *p;
        if (s_sk[i].kind == K_CLOSED) continue;
        p = &c.player[n];
        p->slot = ++n;
        p->kind = s_sk[i].kind == K_YOU ? AVP_PK_HUMAN : s_sk[i].kind == K_AI ? AVP_PK_AI : AVP_PK_REMOTE;
        p->race = (AvpRace)s_sk[i].race;
        p->spawn = s_sk[i].spawn;
        p->team = s_sk[i].team;
    }
    c.nplayers = n;
    *cp = c;
    return n;
}

static void sk_start(void)
{
    AvpSkirmishCfg c;
    int n = sk_build(&c);
    if (!avp_skirmish_config_save(&c)) { sk_fail(); return; }
    avp_skirmish_arm(0);
    fprintf(stderr, "[PCMENU] skirmish on %s with %d players\n", c.map, n);
    handoff_campaign();
}

static void level_handoff(void)
{
    s_forced = 0;
    s_handoff = 1;
    s_handoff_t = GetTickCount();
    go(SCR_MAIN);
}

static int nmap_exists(const char *name)
{
    char path[96];
    FILE *f;
    snprintf(path, sizeof path, "maps/%s.nmap", name);
    f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void launch_editor_exe(const char *map);

static void ned_launch(const char *base, const char *name)
{
    (void)base;
    launch_editor_exe(name);
}

static void ed_start(void)
{
    char name[NMAP_NAME];
    int i;
    for (i = 1; i < 1000; i++) {
        snprintf(name, sizeof name, i == 1 ? "Untitled" : "Untitled_%d", i);
        if (!nmap_exists(name)) break;
    }
    ned_launch(k_codes[s_sk_map], name);
}

static void sk_move(int dx, int dy)
{
    static int col;
    int r;
    sound(1);
    if (s_sel == 0 && dx) { sk_map(dx); return; }
    if (s_sel >= 1 && s_sel < SK_START) col = (s_sel - 1) % SK_CELLS;
    if (s_sel >= SK_START) {
        int k = s_sel - SK_START + dx;
        if (dx && k >= 0 && k <= 1) s_sel = SK_START + k;
        if (dy < 0) s_sel = 1 + (SK_ROWS - 1) * SK_CELLS + col;
        return;
    }
    if (dx) {
        if (s_sel >= 1) {
            int c = (s_sel - 1) % SK_CELLS + dx;
            if (c >= 0 && c < SK_CELLS) s_sel += dx;
        }
        return;
    }
    if (s_sel == 0) {
        if (dy > 0) s_sel = 1 + col;
        return;
    }
    r = (s_sel - 1) / SK_CELLS + dy;
    if (r < 0) s_sel = 0;
    else if (r >= SK_ROWS) s_sel = SK_START;
    else s_sel = 1 + r * SK_CELLS + col;
}

static void sk_nav(int dx, int dy)
{
    if (s_screen != SCR_HOST) { sk_move(dx, dy); return; }
    if (s_sel < 2) {
        sound(1);
        if (dx) s_sel = 1 - s_sel;
        else if (dy > 0) s_sel = 2;
        return;
    }
    if (s_sel == 2 && dy < 0) { sound(1); s_sel = 1; return; }
    s_sel -= 2;
    sk_move(dx, dy);
    s_sel += 2;
}

static int sk_screen(void)
{
    return s_screen == SCR_SKIRM || (s_screen == SCR_HOST && s_mode);
}

static void host_rows(void)
{
    int i;
    s_sk[0].kind = K_YOU;
    s_sk[1].kind = K_REMOTE;
    for (i = 2; i < SK_ROWS; i++)
        if (s_sk[i].kind != K_CLOSED) s_sk[i].kind = K_AI;
}

static int mp_port(void)
{
    int v = atoi(s_set.mport);
    return v > 0 && v < 65536 ? v : 27015;
}

static char *tf_buf(size_t *cap)
{
    *cap = s_tf == TF_PORT ? 5 : s_tf == TF_CODE ? 6 : 63;
    return s_tf == TF_PORT ? s_set.mport : s_tf == TF_CODE ? s_code : s_ip;
}

static void tf_add(int c)
{
    size_t cap, n;
    char *b = tf_buf(&cap);
    n = strlen(b);
    if (n >= cap || c >= 127) return;
    if (s_tf == TF_CODE) c = toupper(c);
    if (s_tf == TF_CODE ? (c && strchr(k_code_alpha, c) != NULL) : s_tf == TF_PORT ? (c >= '0' && c <= '9') : (isalnum(c) || c == '.' || c == '-')) {
        b[n] = (char)c;
        b[n + 1] = 0;
    }
}

static void tf_back(void)
{
    size_t cap, n;
    char *b = tf_buf(&cap);
    n = strlen(b);
    if (n) b[n - 1] = 0;
}

static void tf_end(void)
{
    if (s_tf == TF_PORT && !*s_set.mport) strcpy(s_set.mport, "27015");
    s_tf = TF_NONE;
    settings_save();
}

static void recent_add(const char *ip, int port, const char *name)
{
    char e[96];
    size_t kl;
    int i, at = 2;
    snprintf(e, sizeof e, "%s:%d %s", ip, port, name);
    kl = strcspn(e, " ");
    for (i = 0; i < 3; i++)
        if (!strncmp(s_set.recent[i], e, kl) && s_set.recent[i][kl] == ' ') at = i;
    for (i = at; i > 0; i--)
        memcpy(s_set.recent[i], s_set.recent[i - 1], sizeof s_set.recent[0]);
    memcpy(s_set.recent[0], e, sizeof e);
}

static void host_start(void)
{
    AvpSkirmishCfg c;
    if (avp_coop_state() != 2) return;
    if (!s_mode) {
        avp_skirmish_disarm();
        fprintf(stderr, "[PCMENU] co-op campaign with %s\n", avp_coop_peer_name());
        handoff_campaign();
        return;
    }
    sk_build(&c);
    if (!avp_skirmish_config_save(&c)) { sk_fail(); return; }
    avp_skirmish_arm(0);
    if (!avp_coop_send_cfg()) {
        snprintf(s_sk_err, sizeof s_sk_err, "Lost the connection to the other player");
        s_sk_err_t = GetTickCount();
        avp_skirmish_disarm();
        return;
    }
    fprintf(stderr, "[PCMENU] versus on %s with %s\n", c.map, avp_coop_peer_name());
    handoff_campaign();
}

static void host_toggle(void)
{
    int st = avp_coop_state();
    if (st == 1 || st == 2) { avp_coop_cancel(); return; }
    tf_end();
    avp_skirmish_disarm();
    avp_coop_host(mp_port());
}

static void host_activate(int i)
{
    int j = i - 2;
    sound(2);
    if (i == 0) s_tf = TF_PORT;
    else if (i == 1) host_toggle();
    else if (!s_mode) { if (j == 0) host_start(); else back(); }
    else if (j == 0) sk_map(+1);
    else if (j == SK_START) host_start();
    else if (j == SK_BACK) back();
    else sk_cycle((j - 1) / SK_CELLS, (j - 1) % SK_CELLS);
}

static int join_go(void)
{
    return avp_coop_state() == 2 && !s_mode;
}

static void join_activate(int i)
{
    int st = avp_coop_state(), port;
    char ip[64];
    sound(2);
    if (i == 0) s_tf = TF_IP;
    else if (i == 1) s_tf = TF_PORT;
    else if (i == 2) {
        if (st == 1 || st == 2) { avp_coop_cancel(); return; }
        tf_end();
        avp_skirmish_disarm();
        s_join_logged = 0;
        avp_coop_join(s_ip, mp_port());
    } else if (i == 3) {
        avp_skirmish_disarm();
        handoff_campaign();
    } else if (i < 7) {
        if (sscanf(s_set.recent[i - 4], "%63[^: ]:%d", ip, &port) == 2) {
            snprintf(s_ip, sizeof s_ip, "%s", ip);
            snprintf(s_set.mport, sizeof s_set.mport, "%d", port);
        }
    } else {
        back();
    }
}

static void mp_lr(void)
{
    int n = s_sel;
    if (s_screen == SCR_MP) { if (n < 4) n ^= 2; }
    else if (n < 4) n ^= 1;
    if (n != s_sel && !disabled(n)) { s_sel = n; sound(1); }
}

static void coop_err(const char *m)
{
    snprintf(s_sk_err, sizeof s_sk_err, "%s", m);
    s_sk_err_t = GetTickCount();
}

static int s_co_race, s_co_scroll, s_co_pick = -1, s_co_list[64], s_co_n;

static void coop_screen_reset(void)
{
    s_co_side = 0;
    s_co_col = 0;
    s_co_race = 0;
    s_co_scroll = 0;
    s_co_pick = -1;
    s_co_n = 0;
    s_join_logged = 0;
    s_tf = TF_NONE;
    *s_sk_err = 0;
}

static void coop_missions(void)
{
    int i, n = avp_mission_count();
    s_co_n = 0;
    for (i = 0; i < n && s_co_n < 64; i++)
        if (avp_mission_race(i) == s_co_race)
            s_co_list[s_co_n++] = i;
    s_co_scroll = 0;
    s_co_pick = s_co_n ? s_co_list[0] : -1;
}

static int coop_hosting(void)
{
    return avp_coop_role() == 1 && avp_coop_state() == 2;
}

static void coop_host_toggle(void)
{
    int st = avp_coop_state();
    s_co_side = 1;
    if (avp_coop_role() == 1 && (st == 1 || st == 2)) { avp_coop_cancel(); return; }
    tf_end();
    avp_skirmish_disarm();
    if (!avp_coop_host(mp_port()) && avp_coop_state() != 3)
        coop_err("Could not start hosting or joining");
}

static void coop_join_toggle(void)
{
    int st = avp_coop_state();
    s_co_side = 2;
    if (avp_coop_role() == 2 && (st == 1 || st == 2)) { avp_coop_cancel(); return; }
    tf_end();
    if (!*s_ip) { coop_err("Enter the host's address first"); return; }
    avp_skirmish_disarm();
    s_join_logged = 0;
    if (!avp_coop_join(s_ip, mp_port()) && avp_coop_state() != 3)
        coop_err("Could not start hosting or joining");
}

static void coop_start(void)
{
    if (!coop_hosting() || s_co_pick < 0) return;
    avp_skirmish_disarm();
    fprintf(stderr, "[PCMENU] co-op mission %s with %s\n", avp_mission_code(s_co_pick), avp_coop_peer_name());
    if (!avp_coop_host_launch(s_co_pick)) { coop_err("Could not start the mission"); return; }
    s_forced = 0;
    s_handoff = 1;
    s_handoff_t = GetTickCount();
    go(SCR_MAIN);
}

static void steam_activate(int i)
{
    int st = avp_coop_state(), role = avp_coop_role(), active = st == 1 || st == 2;
    AvpSteamFriend fl[3];
    if (i == 0) {
        avp_steam_connect();
    } else if (i == 1) {
        s_co_side = 1;
        if (role == 1 && active) { avp_coop_cancel(); return; }
        tf_end();
        avp_skirmish_disarm();
        if (!avp_coop_host_steam() && avp_coop_state() != 3) coop_err("Could not host with Steam");
    } else if (i == CO_ADDR) {
        s_tf = TF_CODE;
    } else if (i == CO_JOIN) {
        s_co_side = 2;
        if (role == 2 && st == 2 && avp_coop_guest_mission()) {
            if (!avp_coop_set_ready(!avp_coop_self_ready())) coop_err("Could not reach the host");
            return;
        }
        if (role == 2 && active) { avp_coop_cancel(); return; }
        tf_end();
        avp_skirmish_disarm();
        s_join_logged = 1;
        avp_coop_join_steam_code(s_code);
    } else if (i >= CO_REC0 && i < CO_BACK) {
        if (avp_steam_friends(fl, 3) > i - CO_REC0) {
            s_co_side = 2;
            avp_skirmish_disarm();
            s_join_logged = 1;
            avp_coop_join_steam_lobby((unsigned long long)fl[i - CO_REC0].lobby);
        }
    }
}

static void coop_activate(int i)
{
    int port;
    char ip[64];
    sound(2);
    if (s_co_steam && (i < 2 || (i >= CO_ADDR && i < CO_BACK && i != CO_JPORT))) { steam_activate(i); return; }
    if (i == 0) s_tf = TF_PORT;
    else if (i == 1) coop_host_toggle();
    else if (i >= CO_TAB0 && i < CO_ROW0) {
        s_co_race = i - CO_TAB0;
        coop_missions();
    } else if (i >= CO_ROW0 && i < CO_START) {
        int k = s_co_scroll + i - CO_ROW0;
        if (k < s_co_n) {
            s_co_pick = s_co_list[k];
            if (!avp_coop_host_pick(s_co_pick)) coop_err("Could not send the mission to the other player");
        }
    } else if (i == CO_START) coop_start();
    else if (i == CO_ADDR) s_tf = TF_IP;
    else if (i == CO_JPORT) s_tf = TF_PORT;
    else if (i == CO_JOIN) {
        if (avp_coop_role() == 2 && avp_coop_state() == 2 && avp_coop_guest_mission()) {
            if (!avp_coop_set_ready(!avp_coop_self_ready())) coop_err("Could not reach the host");
        } else coop_join_toggle();
    }
    else if (i >= CO_REC0 && i < CO_BACK) {
        if (sscanf(s_set.recent[i - CO_REC0], "%63[^: ]:%d", ip, &port) == 2) {
            snprintf(s_ip, sizeof s_ip, "%s", ip);
            snprintf(s_set.mport, sizeof s_set.mport, "%d", port);
        }
    } else {
        back();
    }
}

static void coop_scroll(int d)
{
    int max = s_co_n - CO_ROWS;
    if (max < 0) max = 0;
    s_co_scroll += d;
    if (s_co_scroll > max) s_co_scroll = max;
    if (s_co_scroll < 0) s_co_scroll = 0;
}

static void coop_nav(int dx, int dy)
{
    int to = s_sel, i, n, k;
    int order[16];
    if (s_sel < CO_ADDR || s_sel == CO_BACK) s_co_col = 0;
    else s_co_col = 1;
    if (dx) {
        if (s_sel >= CO_TAB0 && s_sel < CO_ROW0 && !disabled(s_sel)) {
            to = s_sel + dx;
            if (to < CO_TAB0 || to >= CO_ROW0) to = s_sel;
            if (to != s_sel) { s_sel = to; sound(1); }
            return;
        }
        if (s_co_col == 0) to = s_sel == 0 ? CO_ADDR : s_sel == CO_BACK ? CO_BACK : CO_JOIN;
        else to = s_sel == CO_ADDR || s_sel == CO_JPORT ? 0 : 1;
        if (to == 0 && s_sel == CO_JPORT) to = 0;
        if (!disabled(to)) { s_sel = to; sound(1); }
        return;
    }
    n = 0;
    if (s_co_col == 0) {
        order[n++] = 0;
        order[n++] = 1;
        order[n++] = CO_TAB0 + s_co_race;
        for (i = CO_ROW0; i < CO_START; i++) order[n++] = i;
        order[n++] = CO_START;
        order[n++] = CO_BACK;
    } else {
        for (i = CO_ADDR; i < CO_BACK; i++) order[n++] = i;
        order[n++] = CO_BACK;
    }
    for (i = 0; i < n; i++)
        if (order[i] == s_sel || (s_sel >= CO_TAB0 && s_sel < CO_ROW0 && order[i] == CO_TAB0 + s_co_race))
            break;
    if (i == n) return;
    for (k = i + dy; k >= 0 && k < n; k += dy) {
        if (!disabled(order[k])) { s_sel = order[k]; sound(1); return; }
    }
    if (order[i] >= CO_ROW0 && order[i] < CO_START && s_co_col == 0) {
        int before = s_co_scroll;
        coop_scroll(dy);
        if (s_co_scroll != before) sound(1);
    }
}

static void sk_activate(int i)
{
    sound(2);
    if (i == 0) sk_map(+1);
    else if (i == SK_START) sk_start();
    else if (i == SK_BACK) back();
    else sk_cycle((i - 1) / SK_CELLS, (i - 1) % SK_CELLS);
}

static void ed_activate(int i)
{
    sound(2);
    if (i == 0) sk_map(+1);
    else if (i == 1) ed_start();
    else back();
}

static char s_nm_names[64][NMAP_NAME];
static int s_nm_n, s_nm_pick, s_nm_scroll, s_sk_net;
static int s_ks_kind[KS_MAXSLOT] = { 1, 2, 2, 2, 3, 3, 3, 3 };
static int s_ks_race[KS_MAXSLOT] = { AVP_RACE_MARINE, AVP_RACE_ALIEN, AVP_RACE_ALIEN, AVP_RACE_PREDATOR, AVP_RACE_ALIEN, AVP_RACE_ALIEN, AVP_RACE_ALIEN, AVP_RACE_ALIEN };
static int s_ks_team[KS_MAXSLOT] = { 1, 1, 2, 2, 3, 3, 4, 4 };
static const char *const k_ks_kind[4] = { "Open", "Human", "AI", "Closed" };

static int sk_peer(void)
{
    return avp_coop_role() == 1 && avp_coop_state() == 2;
}

static char s_ks_base[4], s_ks_loaded[NMAP_NAME];
static int s_ks_objs, s_ks_spawns, s_ks_n = 2, s_ks_num[KS_MAXSLOT];

static void ks_refresh(void)
{
    s_nm_n = nmap_list(s_nm_names, 64);
    if (s_nm_pick >= s_nm_n) s_nm_pick = s_nm_n ? s_nm_n - 1 : 0;
    s_ks_loaded[0] = 0;
}

static void ks_info(void)
{
    Nmap *m;
    int i;
    const char *cur = s_nm_n > 0 ? s_nm_names[s_nm_pick] : "";
    if (!strcmp(cur, s_ks_loaded)) return;
    snprintf(s_ks_loaded, sizeof s_ks_loaded, "%s", cur);
    s_ks_base[0] = 0;
    s_ks_objs = s_ks_spawns = 0;
    s_ks_n = 2;
    memset(s_ks_num, 0, sizeof s_ks_num);
    if (!*cur || !(m = malloc(sizeof *m))) return;
    if (nmap_load(m, cur) >= 0) {
        snprintf(s_ks_base, sizeof s_ks_base, "%s", m->base);
        s_ks_objs = m->nobj;
        s_ks_spawns = nmap_start_list(m, s_ks_num);
        s_ks_n = s_ks_spawns < 2 ? 2 : s_ks_spawns;
        for (i = 0; i < s_ks_n && i < KS_MAXSLOT; i++) {
            int n = s_ks_num[i];
            if (n < 1 || n > NMAP_MAX_SPAWN || !m->slot[n - 1].used) continue;
            if (m->slot[n - 1].race >= 0 && m->slot[n - 1].race < AVP_RACE_COUNT) s_ks_race[i] = m->slot[n - 1].race;
            if (m->slot[n - 1].team > 0 && m->slot[n - 1].team <= s_ks_n) s_ks_team[i] = m->slot[n - 1].team;
        }
    }
    free(m);
}

static int ks_nslots(void)
{
    ks_info();
    return s_ks_n;
}

static void ks_cycle_slot(int i, int col)
{
    if (avp_coop_role() == 2) return;
    if (col == 1) {
        s_ks_race[i] = (s_ks_race[i] + 1) % AVP_RACE_COUNT;
        return;
    }
    if (col == 2) {
        s_ks_team[i] = s_ks_team[i] % ks_nslots() + 1;
        return;
    }
    if (i == 0) return;
    s_ks_kind[i] = (s_ks_kind[i] + 1) % 4;
}

static int ks_build(AvpSkirmishCfg *c, char *err, size_t en)
{
    Nmap *m = malloc(sizeof *m);
    int i, n = 0, peer = sk_peer(), used_remote = 0, teams[KS_MAXSLOT + 1] = { 0 }, nteams = 0, ns = ks_nslots();
    if (!m) return 0;
    memset(c, 0, sizeof *c);
    if (s_nm_n <= 0) { snprintf(err, en, "No custom map selected"); free(m); return 0; }
    if (nmap_load(m, s_nm_names[s_nm_pick]) < 0) {
        snprintf(err, en, "Could not read the map"); free(m); return 0;
    }
    snprintf(c->nmap, sizeof c->nmap, "%s", s_nm_names[s_nm_pick]);
    for (i = 0; i < ns; i++) {
        AvpSkirmishPlayer *p;
        int k = s_ks_kind[i], race = s_ks_race[i];
        if (k == 3) continue;
        if (k == 0 || (k == 1 && i > 0)) {
            if (!peer || used_remote) {
                if (k == 1) { snprintf(err, en, "Slot %d needs a player: host and wait, or set AI", i + 1); free(m); return 0; }
                continue;
            }
            used_remote = 1;
            k = 4;
        }
        if (i >= s_ks_spawns) { snprintf(err, en, "The map has only %d Player Starts", s_ks_spawns); free(m); return 0; }
        p = &c->player[n++];
        p->slot = i + 1;
        p->kind = k == 4 ? AVP_PK_REMOTE : k == 2 ? AVP_PK_AI : AVP_PK_HUMAN;
        p->race = (AvpRace)race;
        p->spawn = s_ks_num[i];
        p->team = s_ks_team[i];
        if (!teams[s_ks_team[i]]) { teams[s_ks_team[i]] = 1; nteams++; }
    }
    c->nplayers = n;
    avp_skirmish_pick_code(c);
    free(m);
    if (nteams < 2) { snprintf(err, en, "At least two teams are needed"); return 0; }
    return 1;
}

static void launch_editor_exe(const char *map)
{
#ifdef _WIN32
    char path[MAX_PATH + 32], cmdline[128], *sl;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) { coop_err("Could not locate the editor"); return; }
    sl = strrchr(path, '\\');
    if (!sl) { coop_err("Could not locate the editor"); return; }
    strcpy(sl + 1, "NostromoEditor.exe");
    snprintf(cmdline, sizeof cmdline, "NostromoEditor.exe%s%s", map ? " --open " : "", map ? map : "");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(path, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        coop_err("NostromoEditor.exe was not found next to this program");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    settings_save();
    PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
#else
    char path[600], *sl;
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 24);
    pid_t pid;
    if (n <= 0) { coop_err("Could not locate the editor"); return; }
    path[n] = 0;
    sl = strrchr(path, '/');
    if (!sl) { coop_err("Could not locate the editor"); return; }
    strcpy(sl + 1, "NostromoEditor");
    if (access(path, X_OK) != 0) { coop_err("NostromoEditor was not found next to this program"); return; }
    pid = fork();
    if (pid == 0) {
        if (map) execl(path, path, "--open", map, (char *)NULL);
        else execl(path, path, (char *)NULL);
        _exit(1);
    }
    if (pid < 0) { coop_err("Could not start the editor"); return; }
    settings_save();
    {
        SDL_Event q;
        memset(&q, 0, sizeof q);
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    }
#endif
}

static void nedopen_activate(int i)
{
    sound(2);
    if (i < KS_ROWS) {
        if (s_nm_scroll + i < s_nm_n) s_nm_pick = s_nm_scroll + i;
    } else if (i == KS_ROWS) {
        Nmap *m;
        if (s_nm_n <= 0) return;
        m = malloc(sizeof *m);
        if (!m) return;
        if (nmap_load(m, s_nm_names[s_nm_pick]) < 0 || strlen(m->base) != 3)
            coop_err("Could not read the map");
        else
            ned_launch(m->base, s_nm_names[s_nm_pick]);
        free(m);
    } else {
        back();
    }
}

static void boot_requests(void)
{
    static int done;
    const char *tm;
    if (done || !g_avp_mainmenu) return;
    tm = getenv("AVP_TEST_MAP");
    if (!(tm && *tm)) { done = 1; return; }
    done = 1;
    if (tm && *tm) {
        Nmap *m = malloc(sizeof *m);
        AvpSkirmishCfg c;
        int i, a = -1, b = -1, k = 0;
        if (m && nmap_load(m, tm) >= 0) {
            AvpRace own = m->base[0] == 'A' ? AVP_RACE_ALIEN : m->base[0] == 'P' ? AVP_RACE_PREDATOR : AVP_RACE_MARINE;
            memset(&c, 0, sizeof c);
            snprintf(c.nmap, sizeof c.nmap, "%s", tm);
            for (i = 0; i < 2; i++) if (m->spawn[i].used && a < 0) a = i;
            for (i = 2; i < 4; i++) if (m->spawn[i].used && b < 0) b = i;
            if (a < 0) for (i = 0; i < NMAP_MAX_SPAWN && a < 0; i++) if (m->spawn[i].used && i != b) a = i;
            if (b < 0) for (i = NMAP_MAX_SPAWN - 1; i >= 0 && b < 0; i--) if (m->spawn[i].used && i != a) b = i;
            if (a >= 0) {
                c.player[k].slot = 1; c.player[k].kind = AVP_PK_HUMAN; c.player[k].race = own;
                c.player[k].spawn = a + 1; c.player[k].team = 1; k++;
            }
            if (b >= 0) {
                c.player[k].slot = 3; c.player[k].kind = AVP_PK_AI;
                c.player[k].race = own == AVP_RACE_ALIEN ? AVP_RACE_MARINE : AVP_RACE_ALIEN;
                c.player[k].spawn = b + 1; c.player[k].team = 2; k++;
            }
            c.nplayers = k;
            avp_skirmish_pick_code(&c);
            if (k == 2 && avp_skirmish_config_save(&c)) {
                avp_skirmish_arm(0);
                avp_load_level_code(c.map);
                level_handoff();
            }
        }
        free(m);
        return;
    }
}

static void ks_start(void)
{
    AvpSkirmishCfg c;
    char err[96];
    if (avp_coop_role() == 2) return;
    if (!ks_build(&c, err, sizeof err)) { coop_err(err); return; }
    if (!avp_skirmish_config_save(&c)) { sk_fail(); return; }
    avp_skirmish_arm(0);
    if (sk_peer() && !avp_coop_send_cfg()) {
        coop_err("Lost the connection to the other player");
        avp_skirmish_disarm();
        return;
    }
    fprintf(stderr, "[PCMENU] custom skirmish %s on %s with %d players\n", c.nmap, c.map, c.nplayers);
    avp_load_level_code(c.map);
    level_handoff();
}

static void ks_activate(int i)
{
    sound(2);
    if (i < KS_ROWS) {
        if (s_nm_scroll + i < s_nm_n) s_nm_pick = s_nm_scroll + i;
    } else if (i == KS_FOLDER) {
        nmap_open_folder();
    } else if (i < KS_HOST) {
        ks_cycle_slot((i - KS_SLOT0) / 3, (i - KS_SLOT0) % 3 == 0 ? 0 : (i - KS_SLOT0) % 3);
    } else if (i == KS_HOST || i == KS_JOIN) {
        avp_coop_cancel();
        s_sk_net = 1;
        s_coop_from = SCR_SKCUST;
        s_mode = 0;
        go(SCR_COOPMODE);
    } else if (i == KS_START) {
        ks_start();
    } else {
        back();
    }
}

static void mp_status(int x0, int y0, int x1, int y1)
{
    int st = avp_coop_state();
    text(x0, y0, x1, y1, avp_coop_state_text(), st == 3 ? 0xFF5050u : st == 2 ? 0x60E060u : C_GOLD, F_BODY,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void sk_map_row(const Box *b, int hot, const char *code)
{
    text(b->x0 + 110, b->y0, b->x0 + 220, b->y1, code, C_TXT_HOT, F_BTN, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(b->x0 + 94, b->y0, b->x0 + 112, b->y1, "<", hot ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(b->x0 + 218, b->y0, b->x0 + 236, b->y1, ">", hot ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void sk_grid(int base, int hy)
{
    static const int hx[SK_CELLS] = { 104, 177, 262, 320 };
    static const char *const hn[SK_CELLS] = { "Kind", "Race", "Spawn", "Team" };
    char buf[16];
    int r, c;
    for (c = 0; c < SK_CELLS; c++)
        text(hx[c], hy, hx[c] + 110, hy + 20, hn[c], C_TXT_OFF, F_BODY, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (r = 0; r < SK_ROWS; r++) {
        const Box *rb = &s_box[base + 1 + r * SK_CELLS];
        snprintf(buf, sizeof buf, "Player %d", r + 1);
        text(40, rb->y0, 104, rb->y1, buf, s_sk[r].kind == K_CLOSED ? C_TXT_OFF : C_TXT,
             F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (c = 0; c < SK_CELLS; c++) {
            const Box *b = &s_box[base + 1 + r * SK_CELLS + c];
            int hot = s_sel == base + 1 + r * SK_CELLS + c, off = c > 0 && s_sk[r].kind == K_CLOSED;
            const char *v;
            char nb[16];
            if (c == 0) v = k_kind_name[s_sk[r].kind];
            else if (c == 1) v = k_race_name[s_sk[r].race];
            else { snprintf(nb, sizeof nb, "%d", c == 2 ? s_sk[r].spawn : s_sk[r].team); v = nb; }
            cell(b->x0, b->y0, b->x1, b->y1, hot, off);
            text(b->x0, b->y0, b->x1, b->y1, v, off ? C_TXT_OFF : hot ? C_TXT_HOT : C_TXT, F_BTN,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static const char *row_label(int id)
{
    switch (id) {
    case ST_DISPLAY: return "Display mode";
    case ST_RES: return "Resolution";
    case ST_CAP: return "Frame limit";
    case ST_VSYNC: return "VSync";
    case ST_WIDE: return "Widescreen";
    case ST_STYLE: return "Main menu";
    case ST_INPUT: return "Input";
    case ST_EDGE: return "Edge scrolling";
    case ST_GLIDE: return "Minimap click";
    case ST_KEYS: return "Key bindings...";
    case ST_NAME: return "Player name";
    case ST_COOP: return "Co-op Campaign";
    }
    return "Back";
}

static const char *setting_value(int i, char *buf, size_t n)
{
    switch (i) {
    case ST_DISPLAY: return s_set.dmode == 2 ? "Exclusive fullscreen" : s_set.dmode == 1 ? "Borderless fullscreen" : "Windowed";
    case ST_RES:
        if (s_set.dmode == 1 || s_set.rw <= 0) {
            int dw = 0, dh = 0;
            if (s_set.dmode != 1 && xbox_FramebufferDesktopSize(&dw, &dh)) snprintf(buf, n, "Desktop %dx%d", dw, dh);
            else return "Desktop";
        } else {
            snprintf(buf, n, "%d x %d", s_set.rw, s_set.rh);
        }
        return buf;
    case ST_CAP:
        if (s_set.cap > 0) snprintf(buf, n, "%d fps", s_set.cap);
        else return "Off";
        return buf;
    case ST_VSYNC: return s_set.vsync ? "On" : "Off";
    case ST_STYLE: return s_set.original_menu ? "Original" : "PC";
    case ST_INPUT: return s_set.input_ctl ? "Controller" : "Mouse & keyboard";
    case ST_EDGE: return s_set.edge_scroll ? "On" : "Off";
    case ST_GLIDE: return s_set.map_glide ? "Glide" : "Jump";
    case ST_NAME:
        snprintf(buf, n, "%s%s", s_set.name, s_editing && (GetTickCount() / 400) % 2 ? "_" : "");
        return buf;
    case ST_WIDE:
        snprintf(buf, n, "%s", s_set.wide == 1 ? "16:9" : s_set.wide == 2 ? "16:10" : "Off");
        return buf;
    }
    return "";
}

static void hint_bar(const char *esc)
{
    char b[64];
    snprintf(b, sizeof b, "Enter  Select        Esc  %s", esc);
    text(300, 446, 600, 472, b, C_TXT_OFF, F_BODY, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_main_bg(int sel)
{
    int i, n = main_count(), d = menu_draft();
    int bottom = s_col.y0 + (n - 1) * s_col.pitch + s_col.h;
    if (s_col.bottom > bottom) bottom = s_col.bottom;
    if (d == 2) {
        rrect(s_col.x0 - MAIN_PAD, s_col.y0 - MAIN_PAD, s_col.x1 + MAIN_PAD, bottom + MAIN_PAD, 16,
              0x060C18u, 0x060C18u, 0.55f, 0, 0);
        rrect(s_col.x0 - MAIN_PAD + 4, s_col.y0 - MAIN_PAD + 4, s_col.x1 + MAIN_PAD - 4, bottom + MAIN_PAD - 4, 12,
              0x0B1B34u, 0x07101Fu, 0.80f, 0x49E1EAu, 2);
    }
    for (i = 0; i < n; i++) {
        const Box *bx = &s_box[i];
        const char *lb = main_item(i)->label;
        if (d == 1) main_native(bx, lb, i == sel);
        else if (d == 3) main_minimal(bx, lb, i == sel);
        else if (d == 4) main_plate(bx, lb, i == sel);
        else main_panel_item(bx, lb, i == sel);
    }
    hint_bar(s_screen == SCR_MAIN ? "Quit" : "Back");
}

static const char *coop_guest_title(void)
{
    const char *g = avp_coop_guest_mission();
    int i, n = avp_mission_count();
    if (!g || !*g) return NULL;
    for (i = 0; i < n; i++)
        if (!strcmp(avp_mission_code(i), g)) return avp_mission_title(i);
    return g;
}

static void coop_status(int side, int x0, int y0, int x1, int y1)
{
    int st = avp_coop_state();
    char b[200];
    const char *t = avp_coop_state_text();
    if (s_co_side == side && *t) {
        uint32_t c = st == 3 ? 0xFF6060u : st == 2 ? 0x60E060u : C_GOLD;
        if (side == 2 && st == 2 && avp_coop_role() == 2) {
            const char *m = coop_guest_title();
            if (m) snprintf(b, sizeof b, avp_coop_self_ready() ? "%s\nHost chose %s. Ready, waiting for the host to start." : "%s\nHost chose %s. Press Ready.", t, m);
            else snprintf(b, sizeof b, "%s\nWaiting for the host to choose a mission.", t);
        } else if (side == 1 && st == 2 && avp_coop_role() == 1) {
            snprintf(b, sizeof b, "%s\n%s", t, s_co_pick < 0 ? "Pick a mission." : avp_coop_peer_ready() ? "The other player is ready." : "Waiting for the other player to press Ready.");
        } else {
            snprintf(b, sizeof b, "%s", t);
        }
        text(x0, y0, x1, y1, b, c, F_BODY, DT_LEFT | DT_WORDBREAK);
        return;
    }
    text(x0, y0, x1, y1,
         side == 1 ? "Press Start hosting, then give the other player your address and this port."
                   : "Enter the host's address and port, then press Join.",
         C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
}

static void coop_field(const Box *b, const char *label, const char *val, int hot, int editing, int right)
{
    char buf[80];
    snprintf(buf, sizeof buf, "%s%s", val, editing && (GetTickCount() / 400) % 2 ? "_" : "");
    button(b->x0, b->y0, b->x1, b->y1, label, hot, 0, NULL);
    if (right)
        text(b->x0 + 74, b->y0, b->x1 - 8, b->y1, buf, editing ? C_GOLD : C_TXT_HOT, F_BTN,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    else
        text(b->x0 + 100, b->y0, b->x1 - 8, b->y1, buf, editing ? C_GOLD : C_TXT_HOT, F_BTN,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_coop_steam(void)
{
    const Box *cb = &s_box[0], *hb = &s_box[1], *ja = &s_box[CO_ADDR], *jt = &s_box[CO_JOIN];
    int st = avp_coop_state(), role = avp_coop_role(), ss = avp_steam_status(), k, nf;
    static const char *const races[3] = { "Marine", "Predator", "Alien" };
    AvpSteamFriend fl[3];
    const char *stt = avp_steam_text();
    uint32_t sc = ss == AVP_STEAM_FAILED ? 0xFF6060u : ss == AVP_STEAM_READY ? 0x60E060u : C_GOLD;
    avp_steam_want_friends(ss == AVP_STEAM_READY);
    nf = avp_steam_friends(fl, 3);
    panel(30, 52, 610, 458);
    text(50, 58, 590, 94, "Co-op Campaign: Steam", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    rrect(50, 96, 590, 98, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
    card(50, 106, 310, 434);
    card(330, 106, 590, 434);
    text(62, 112, 298, 168, "Host", C_GOLD, F_BTN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(342, 112, 578, 168, "Join", C_GOLD, F_BTN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    button(cb->x0, cb->y0, cb->x1, cb->y1, ss == AVP_STEAM_READY ? "Steam connected" : ss == AVP_STEAM_BUSY ? "Connecting" : "Connect to Steam",
           s_sel == 0, disabled(0), NULL);
    button(hb->x0, hb->y0, hb->x1, hb->y1, role == 1 && (st == 1 || st == 2) ? "Stop hosting" : "Host with Steam",
           s_sel == 1, disabled(1), NULL);
    if (coop_hosting()) {
        for (k = 0; k < 3; k++) {
            const Box *b = &s_box[CO_TAB0 + k];
            button(b->x0, b->y0, b->x1, b->y1, races[k], s_sel == CO_TAB0 + k, 0, k == s_co_race ? "<" : NULL);
        }
        for (k = 0; k < CO_ROWS; k++) {
            const Box *b = &s_box[CO_ROW0 + k];
            int m = s_co_scroll + k;
            if (m >= s_co_n) break;
            button(b->x0, b->y0, b->x1, b->y1, avp_mission_title(s_co_list[m]), s_sel == CO_ROW0 + k, 0,
                   s_co_list[m] == s_co_pick ? "*" : NULL);
        }
        button(s_box[CO_START].x0, s_box[CO_START].y0, s_box[CO_START].x1, s_box[CO_START].y1,
               s_co_pick >= 0 && !avp_coop_peer_ready() ? "Waiting for ready" : "Start",
               s_sel == CO_START, disabled(CO_START), NULL);
        coop_status(1, 62, 172, 298, 232);
    } else if (s_co_side == 1 && *avp_coop_state_text()) {
        coop_status(1, 62, 264, 298, 330);
    } else {
        text(62, 264, 298, 330, *stt ? stt : "Connect to Steam to host or join.", *stt ? sc : C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
    }
    coop_field(ja, "Code", s_code, s_sel == CO_ADDR, s_tf == TF_CODE, 0);
    button(jt->x0, jt->y0, jt->x1, jt->y1,
           role == 2 && st == 2 && avp_coop_guest_mission() ? (avp_coop_self_ready() ? "Not ready" : "Ready")
           : role == 2 && (st == 1 || st == 2) ? "Cancel" : "Join with code",
           s_sel == CO_JOIN, disabled(CO_JOIN), role == 2 && st == 2 && avp_coop_self_ready() ? "READY" : NULL);
    if (s_co_side == 2 && *avp_coop_state_text()) coop_status(2, 342, 264, 578, 312);
    else text(342, 264, 578, 312, "Enter the host's code, or pick a friend below.", C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
    text(342, 316, 578, 334, "Friends' lobbies", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (k = 0; k < nf; k++) {
        const Box *b = &s_box[CO_REC0 + k];
        button(b->x0, b->y0, b->x1, b->y1, fl[k].name, s_sel == CO_REC0 + k, 0, "Join");
    }
    if (!nf)
        text(342, 338, 578, 360, ss == AVP_STEAM_READY ? "No friends in a lobby" : "Not connected", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    button(s_box[CO_BACK].x0, s_box[CO_BACK].y0, s_box[CO_BACK].x1, s_box[CO_BACK].y1, "Back", s_sel == CO_BACK, 0, NULL);
    if (*s_sk_err) {
        if (GetTickCount() - s_sk_err_t < 6000)
            text(200, 440, 590, 464, s_sk_err, 0xFF6060u, F_BODY, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        else
            *s_sk_err = 0;
    }
}

static void steam_tick(void)
{
    static int scanned;
    uint64_t l;
    if (!scanned) {
        scanned = 1;
        avp_steam_scan_cmdline();
    }
    if (avp_steam_cmd_pending() && avp_steam_status() == AVP_STEAM_OFF) avp_steam_connect();
    if (!g_avp_mainmenu || !avp_steam_take_join(&l)) return;
    s_co_steam = 1;
    s_coop_from = SCR_MAIN;
    s_sk_net = APP_MODE == 1;
    avp_skirmish_disarm();
    go(SCR_COOP);
    s_co_side = 2;
    s_co_col = 1;
    s_join_logged = 1;
    avp_coop_join_steam_lobby((unsigned long long)l);
}

static void draw_screen(void)
{
    steam_tick();
    boot_requests();
    char buf[64];
    int i;
    column_update();
    if (!s_col_lock && s_screen == SCR_MAIN && g_avp_mainmenu && !s_forced) return;
    if (s_screen != SCR_MAIN) {
        if ((APP_MODE != 0 || !s_set.original_menu) && g_avp_mainmenu) {
            Box keep[48];
            int kn = s_nbox;
            memcpy(keep, s_box, sizeof keep);
            layout_main();
            draw_main_bg(s_sel_main);
            memcpy(s_box, keep, sizeof keep);
            s_nbox = kn;
            text_flush();
        }
        rrect(0, 0, LW, LH, 0, 0, 0, 0.35f, 0, 0);
    }
    layout();
    switch (s_screen) {
    case SCR_MAIN:
        draw_main_bg(s_sel);
        break;

    case SCR_MP: {
        static const char *const nm[2] = { "Co-op Campaign", "Versus" };
        static const char *const ds[2] = {
            "Play the campaign together. Select units and press G to give them to player 2.",
            "Fight a friend in a skirmish. The host picks the map and the players." };
        int k;
        panel(100, 110, 540, 420);
        text(120, 118, 520, 152, "Multiplayer", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(120, 154, 520, 156, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        for (k = 0; k < 2; k++) {
            int x0 = 120 + k * 205;
            card(x0, 168, x0 + 195, 344);
            text(x0 + 10, 174, x0 + 185, 204, nm[k], C_GOLD, F_BTN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            text(x0 + 10, 208, x0 + 185, 274, ds[k], C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
            button(s_box[k * 2].x0, s_box[k * 2].y0, s_box[k * 2].x1, s_box[k * 2].y1, "Host", s_sel == k * 2, 0, NULL);
            button(s_box[k * 2 + 1].x0, s_box[k * 2 + 1].y0, s_box[k * 2 + 1].x1, s_box[k * 2 + 1].y1, "Join",
                   s_sel == k * 2 + 1, 0, NULL);
        }
        snprintf(buf, sizeof buf, "Playing as %s", s_set.name);
        text(120, 352, 520, 376, buf, C_GOLD, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        button(s_box[4].x0, s_box[4].y0, s_box[4].x1, s_box[4].y1, "Back", s_sel == 4, 0, NULL);
        break;
    }

    case SCR_HOST: {
        int st = avp_coop_state(), b = s_mode ? 2 : 0, sm = s_mode ? 2 + SK_START : 2;
        const Box *pb = &s_box[0], *hb = &s_box[1];
        if (s_mode) {
            panel(20, 30, 620, 450);
            text(40, 38, 600, 70, "Host a Versus match", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            rrect(40, 72, 600, 74, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        } else {
            panel(80, 100, 560, 420);
            text(100, 108, 540, 142, "Host a Co-op Campaign", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            rrect(100, 146, 540, 148, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        }
        snprintf(buf, sizeof buf, "%s%s", s_set.mport, s_tf == TF_PORT && (GetTickCount() / 400) % 2 ? "_" : "");
        button(pb->x0, pb->y0, pb->x1, pb->y1, "Port", s_sel == 0, 0, NULL);
        text(pb->x0 + 74, pb->y0, pb->x1 - 8, pb->y1, buf, s_tf == TF_PORT ? C_GOLD : C_TXT_HOT, F_BTN,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        button(hb->x0, hb->y0, hb->x1, hb->y1, st == 1 || st == 2 ? "Stop hosting" : "Start hosting", s_sel == 1, 0, NULL);
        if (s_mode) {
            const char *code = k_codes[s_sk_map];
            const Box *mb = &s_box[2];
            mp_status(40, 112, 400, 134);
            button(mb->x0, mb->y0, mb->x1, mb->y1, "Map", s_sel == 2, 0, sk_star(code) ? "* 3 races" : NULL);
            sk_map_row(mb, s_sel == 2, code);
            map_preview(420, 84, s_sk_map);
            sk_grid(b, 166);
            text(40, 326, 400, 390,
                 "Player 2 is the remote player. They join from Multiplayer, Versus, Join. Start match opens the campaign menu and loads the chosen map.",
                 C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
            if (*s_sk_err) {
                if (GetTickCount() - s_sk_err_t < 3000)
                    text(40, 372, 400, 394, s_sk_err, 0xFF5050u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                else
                    *s_sk_err = 0;
            }
        } else {
            mp_status(100, 196, 540, 220);
            text(100, 232, 540, 330,
                 "Start hosting, then ask a friend to join from Multiplayer, Co-op Campaign, Join. "
                 "Start match opens the campaign menu. Both players now pick the same campaign mission.\n\n"
                 "The host starts with the whole army. Select units and press G to give them to player 2.",
                 C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
        }
        button(s_box[sm].x0, s_box[sm].y0, s_box[sm].x1, s_box[sm].y1, "Start match", s_sel == sm, st != 2, NULL);
        button(s_box[sm + 1].x0, s_box[sm + 1].y0, s_box[sm + 1].x1, s_box[sm + 1].y1, "Back", s_sel == sm + 1, 0, NULL);
        break;
    }

    case SCR_SKIRM: {
        const char *code = k_codes[s_sk_map];
        panel(20, 30, 620, 450);
        text(40, 38, 600, 70, "Skirmish", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(40, 72, 600, 74, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        button(s_box[0].x0, s_box[0].y0, s_box[0].x1, s_box[0].y1, "Map", s_sel == 0, 0,
               sk_star(code) ? "* 3 races" : NULL);
        sk_map_row(&s_box[0], s_sel == 0, code);
        map_preview(420, 84, s_sk_map);
        sk_grid(0, 116);
        text(40, 280, 400, 330,
             "The map's own race UI is used. Start opens the campaign menu and loads the chosen map.",
             C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
        if (*s_sk_err) {
            if (GetTickCount() - s_sk_err_t < 3000)
                text(40, 366, 400, 388, s_sk_err, 0xFF5050u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_sk_err = 0;
        }
        button(s_box[SK_START].x0, s_box[SK_START].y0, s_box[SK_START].x1, s_box[SK_START].y1, "Start",
               s_sel == SK_START, 0, NULL);
        button(s_box[SK_BACK].x0, s_box[SK_BACK].y0, s_box[SK_BACK].x1, s_box[SK_BACK].y1, "Back",
               s_sel == SK_BACK, 0, NULL);
        text(380, 400, 600, 426, "Arrows or click  -  Esc to go back", C_TXT_OFF, F_BODY,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }

    case SCR_JOIN: {
        int st = avp_coop_state(), k, any = 0;
        const Box *ib = &s_box[0], *pb = &s_box[1], *jb = &s_box[2], *gb = &s_box[3];
        if (st == 2 && !s_join_logged && *avp_coop_peer_name()) {
            s_join_logged = 1;
            recent_add(s_ip, mp_port(), avp_coop_peer_name());
            settings_save();
        }
        if (st == 2 && avp_skirmish_armed()) {
            handoff_campaign();
            break;
        }
        panel(80, 100, 560, 450);
        text(100, 108, 540, 142, s_mode ? "Join a Versus match" : "Join a Co-op Campaign", C_TXT_HOT, F_TITLE,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(100, 146, 540, 148, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        snprintf(buf, sizeof buf, "%s%s", s_ip, s_tf == TF_IP && (GetTickCount() / 400) % 2 ? "_" : "");
        button(ib->x0, ib->y0, ib->x1, ib->y1, "Address", s_sel == 0, 0, NULL);
        text(ib->x0 + 104, ib->y0, ib->x1 - 8, ib->y1, buf, s_tf == TF_IP ? C_GOLD : C_TXT_HOT, F_BTN,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        snprintf(buf, sizeof buf, "%s%s", s_set.mport, s_tf == TF_PORT && (GetTickCount() / 400) % 2 ? "_" : "");
        button(pb->x0, pb->y0, pb->x1, pb->y1, "Port", s_sel == 1, 0, NULL);
        text(pb->x0 + 74, pb->y0, pb->x1 - 8, pb->y1, buf, s_tf == TF_PORT ? C_GOLD : C_TXT_HOT, F_BTN,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        button(jb->x0, jb->y0, jb->x1, jb->y1, st == 1 || st == 2 ? "Cancel" : "Join", s_sel == 2, 0, NULL);
        if (join_go())
            button(gb->x0, gb->y0, gb->x1, gb->y1, "Go to campaign", s_sel == 3, 0, NULL);
        mp_status(100, 232, 540, 254);
        if (st == 2)
            text(100, 258, 540, 280, s_mode ? "Waiting for the host to start." : "Pick the same campaign mission as the host.",
                 C_TXT, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(100, 284, 540, 304, "Recent hosts", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (k = 0; k < 3; k++) {
            const Box *b = &s_box[4 + k];
            char lab[96], *sp;
            if (!*s_set.recent[k]) continue;
            any = 1;
            snprintf(lab, sizeof lab, "%s", s_set.recent[k]);
            sp = strchr(lab, ' ');
            if (sp) *sp++ = 0;
            button(b->x0, b->y0, b->x1, b->y1, lab, s_sel == 4 + k, 0, sp);
        }
        if (!any)
            text(100, 306, 540, 330, "No recent hosts yet", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        button(s_box[7].x0, s_box[7].y0, s_box[7].x1, s_box[7].y1, "Back", s_sel == 7, 0, NULL);
        break;
    }

    case SCR_EDITOR: {
        const char *code = k_codes[s_sk_map];
        panel(70, 100, 590, 420);
        text(90, 110, 570, 144, "Map Editor", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(90, 146, 570, 148, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        button(s_box[0].x0, s_box[0].y0, s_box[0].x1, s_box[0].y1, "Map", s_sel == 0, 0,
               sk_star(code) ? "* 3 races" : NULL);
        text(170, s_box[0].y0, 270, s_box[0].y1, code, C_TXT_HOT, F_BTN, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(152, s_box[0].y0, 170, s_box[0].y1, "<", s_sel == 0 ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(270, s_box[0].y0, 288, s_box[0].y1, ">", s_sel == 0 ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        map_preview(390, 160, s_sk_map);
        text(90, 204, 370, 300, "Pick the level whose terrain the new map uses. Its mission units and scripts are removed.",
             C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
        if (*s_sk_err) {
            if (GetTickCount() - s_sk_err_t < 3000)
                text(90, 314, 370, 336, s_sk_err, 0xFF5050u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_sk_err = 0;
        }
        button(s_box[1].x0, s_box[1].y0, s_box[1].x1, s_box[1].y1, "Create", s_sel == 1, 0, NULL);
        button(s_box[2].x0, s_box[2].y0, s_box[2].x1, s_box[2].y1, "Back", s_sel == 2, 0, NULL);
        break;
    }

    case SCR_SKCUST: {
        int k, role = avp_coop_role(), st = avp_coop_state();
        if (role == 2 && st == 2 && avp_skirmish_armed()) {
            AvpSkirmishCfg gc;
            if (avp_skirmish_config_load(&gc)) { avp_skirmish_pick_code(&gc); if (strlen(gc.map) == 3) avp_load_level_code(gc.map); }
            level_handoff();
            break;
        }
        panel(20, 30, 620, 450);
        text(40, 38, 600, 70, "Skirmish", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(40, 72, 600, 74, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        text(40, 80, 250, 102, "Custom maps", C_GOLD, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (k = 0; k < KS_ROWS; k++) {
            const Box *b = &s_box[k];
            int m = s_nm_scroll + k;
            if (m >= s_nm_n) break;
            button(b->x0, b->y0, b->x1, b->y1, s_nm_names[m], s_sel == k, 0, m == s_nm_pick ? "*" : NULL);
        }
        if (!s_nm_n)
            text(40, 108, 250, 200, "No custom maps yet. Make one in the Nostromo Editor and save it in the maps folder.",
                 C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
        button(s_box[KS_FOLDER].x0, s_box[KS_FOLDER].y0, s_box[KS_FOLDER].x1, s_box[KS_FOLDER].y1,
               "Open maps folder", s_sel == KS_FOLDER, 0, NULL);
        ks_info();
        text(262, 80, 600, 100, "Players and teams", C_GOLD, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (k = 0; k < ks_nslots(); k++) {
            char nb[24];
            const Box *kb = &s_box[KS_SLOT0 + k * 3], *rb = &s_box[KS_SLOT0 + k * 3 + 1], *tb = &s_box[KS_SLOT0 + k * 3 + 2];
            snprintf(nb, sizeof nb, "%d", k + 1);
            text(262, kb->y0, 292, kb->y1, nb, C_TXT, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            button(kb->x0, kb->y0, kb->x1, kb->y1, k_ks_kind[s_ks_kind[k]], s_sel == KS_SLOT0 + k * 3, k == 0 || role == 2, NULL);
            button(rb->x0, rb->y0, rb->x1, rb->y1, k_race_name[s_ks_race[k]], s_sel == KS_SLOT0 + k * 3 + 1,
                   role == 2, NULL);
            snprintf(nb, sizeof nb, "Team %d", s_ks_team[k]);
            button(tb->x0, tb->y0, tb->x1, tb->y1, nb, s_sel == KS_SLOT0 + k * 3 + 2, role == 2, NULL);
        }
        if (s_nm_n > 0) {
            char ib[200];
            snprintf(ib, sizeof ib, "%d objects, %d Player Starts. Any slot can be any race; player 1's race decides the HUD and menus. Players on the same team are allies.",
                     s_ks_objs, s_ks_spawns);
            text(40, 274, 250, 400, ib, C_GOLD, F_BODY, DT_LEFT | DT_WORDBREAK);
        }
        text(262, 352, 600, 396, "Open and Human slots take the player who joins (one remote human: host and one guest). AI slots run the game's own AI.",
             C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
        mp_status(262, 330, 600, 350);
        if (role == 2)
            text(484, 80, 600, 100, "Host decides", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        button(s_box[KS_HOST].x0, s_box[KS_HOST].y0, s_box[KS_HOST].x1, s_box[KS_HOST].y1, "Host", s_sel == KS_HOST, 0, NULL);
        button(s_box[KS_JOIN].x0, s_box[KS_JOIN].y0, s_box[KS_JOIN].x1, s_box[KS_JOIN].y1, "Join", s_sel == KS_JOIN, 0, NULL);
        button(s_box[KS_START].x0, s_box[KS_START].y0, s_box[KS_START].x1, s_box[KS_START].y1, "Start", s_sel == KS_START, role == 2, NULL);
        button(s_box[KS_BACK].x0, s_box[KS_BACK].y0, s_box[KS_BACK].x1, s_box[KS_BACK].y1, "Back", s_sel == KS_BACK, 0, NULL);
        if (*s_sk_err) {
            if (GetTickCount() - s_sk_err_t < 5000)
                text(40, 440, 600, 460, s_sk_err, 0xFF6060u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_sk_err = 0;
        }
        break;
    }

    case SCR_NEDOPEN: {
        int k;
        panel(70, 100, 590, 420);
        text(90, 110, 570, 144, "Open Map", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(90, 146, 570, 148, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        for (k = 0; k < KS_ROWS; k++) {
            const Box *b = &s_box[k];
            int m = s_nm_scroll + k;
            if (m >= s_nm_n) break;
            button(b->x0, b->y0, b->x1, b->y1, s_nm_names[m], s_sel == k, 0, m == s_nm_pick ? "*" : NULL);
        }
        if (!s_nm_n)
            text(90, 160, 400, 240, "No maps in the maps folder yet.", C_TXT_OFF, F_BODY, DT_LEFT | DT_WORDBREAK);
        button(s_box[KS_ROWS].x0, s_box[KS_ROWS].y0, s_box[KS_ROWS].x1, s_box[KS_ROWS].y1, "Open", s_sel == KS_ROWS, s_nm_n <= 0, NULL);
        button(s_box[KS_ROWS + 1].x0, s_box[KS_ROWS + 1].y0, s_box[KS_ROWS + 1].x1, s_box[KS_ROWS + 1].y1, "Back", s_sel == KS_ROWS + 1, 0, NULL);
        if (*s_sk_err) {
            if (GetTickCount() - s_sk_err_t < 4000)
                text(90, 340, 570, 362, s_sk_err, 0xFF6060u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_sk_err = 0;
        }
        break;
    }

    case SCR_COOPMODE: {
        panel(150, 120, 490, 340);
        text(170, 128, 470, 164, "Co-op Campaign", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(170, 166, 470, 168, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        button(s_box[0].x0, s_box[0].y0, s_box[0].x1, s_box[0].y1, "IP Connection", s_sel == 0, 0, NULL);
        button(s_box[1].x0, s_box[1].y0, s_box[1].x1, s_box[1].y1, "Steam Connection", s_sel == 1, 0, NULL);
        button(s_box[2].x0, s_box[2].y0, s_box[2].x1, s_box[2].y1, "Back", s_sel == 2, 0, NULL);
        break;
    }

    case SCR_COOP: {
        const Box *hp = &s_box[0], *ht = &s_box[1];
        if (s_sk_net && avp_coop_state() == 2) { go(SCR_SKCUST); break; }
        if (s_co_steam) { draw_coop_steam(); break; }
        const Box *ja = &s_box[CO_ADDR], *jp = &s_box[CO_JPORT], *jt = &s_box[CO_JOIN];
        int st = avp_coop_state(), role = avp_coop_role(), k, any = 0;
        static const char *const races[3] = { "Marine", "Predator", "Alien" };
        if (role == 2 && st == 2 && !s_join_logged && *avp_coop_peer_name()) {
            s_join_logged = 1;
            recent_add(s_ip, mp_port(), avp_coop_peer_name());
            settings_save();
        }
        panel(30, 52, 610, 458);
        text(50, 58, 590, 94, "Co-op Campaign", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(50, 96, 590, 98, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        card(50, 106, 310, 434);
        card(330, 106, 590, 434);
        text(62, 112, 298, 168, "Host", C_GOLD, F_BTN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(342, 112, 578, 168, "Join", C_GOLD, F_BTN, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        coop_field(hp, "Port", s_set.mport, s_sel == 0, s_tf == TF_PORT && s_sel == 0, 1);
        button(ht->x0, ht->y0, ht->x1, ht->y1, role == 1 && (st == 1 || st == 2) ? "Stop hosting" : "Start hosting",
               s_sel == 1, 0, NULL);
        if (coop_hosting()) {
            for (k = 0; k < 3; k++) {
                const Box *b = &s_box[CO_TAB0 + k];
                button(b->x0, b->y0, b->x1, b->y1, races[k], s_sel == CO_TAB0 + k, 0, k == s_co_race ? "<" : NULL);
            }
            for (k = 0; k < CO_ROWS; k++) {
                const Box *b = &s_box[CO_ROW0 + k];
                int m = s_co_scroll + k;
                if (m >= s_co_n) break;
                button(b->x0, b->y0, b->x1, b->y1, avp_mission_title(s_co_list[m]), s_sel == CO_ROW0 + k, 0,
                       s_co_list[m] == s_co_pick ? "*" : NULL);
            }
            button(s_box[CO_START].x0, s_box[CO_START].y0, s_box[CO_START].x1, s_box[CO_START].y1,
                   s_co_pick >= 0 && !avp_coop_peer_ready() ? "Waiting for ready" : "Start",
                   s_sel == CO_START, disabled(CO_START), NULL);
            coop_status(1, 62, 172, 298, 232);
        } else {
            coop_status(1, 62, 264, 298, 330);
        }
        coop_field(ja, "Address", s_ip, s_sel == CO_ADDR, s_tf == TF_IP, 0);
        coop_field(jp, "Port", s_set.mport, s_sel == CO_JPORT, s_tf == TF_PORT && s_sel == CO_JPORT, 1);
        button(jt->x0, jt->y0, jt->x1, jt->y1,
               role == 2 && st == 2 && avp_coop_guest_mission() ? (avp_coop_self_ready() ? "Not ready" : "Ready")
               : role == 2 && (st == 1 || st == 2) ? "Cancel" : "Join",
               s_sel == CO_JOIN, 0, role == 2 && st == 2 && avp_coop_self_ready() ? "READY" : NULL);
        coop_status(2, 342, 264, 578, 330);
        text(342, 316, 578, 334, "Recent hosts", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (k = 0; k < 3; k++) {
            const Box *b = &s_box[CO_REC0 + k];
            char lab[96], *sp;
            if (!*s_set.recent[k]) continue;
            any = 1;
            snprintf(lab, sizeof lab, "%s", s_set.recent[k]);
            sp = strchr(lab, ' ');
            if (sp) *sp++ = 0;
            button(b->x0, b->y0, b->x1, b->y1, lab, s_sel == CO_REC0 + k, 0, sp);
        }
        if (!any)
            text(342, 338, 578, 360, "No recent hosts yet", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        button(s_box[CO_BACK].x0, s_box[CO_BACK].y0, s_box[CO_BACK].x1, s_box[CO_BACK].y1, "Back",
               s_sel == CO_BACK, 0, NULL);
        if (*s_sk_err) {
            if (GetTickCount() - s_sk_err_t < 6000)
                text(200, 440, 590, 464, s_sk_err, 0xFF6060u, F_BODY, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_sk_err = 0;
        }
        break;
    }

    case SCR_SETTINGS: {
        int id, hot = row_id(s_sel);
        const Box *bk = &s_box[ST_BACK];
        panel(80, 104, 560, 442);
        text(100, 110, 540, 144, "PC Settings", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(100, 146, 540, 148, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        for (i = 0; i < ST_BACK; i++) {
            const Box *b = &s_box[i];
            const char *hd = head_before(k_rows_all[i]);
            int off, hy = SET_VT + set_row_y(i) - SET_HEAD_H - s_set_scroll;
            id = k_rows_all[i];
            if (hd && hy >= SET_VT && hy + SET_HEAD_H <= SET_VB) {
                text(100, hy, 540, hy + SET_HEAD_H - 2, hd, C_GOLD, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                rrect(100, hy + SET_HEAD_H - 2, 540, hy + SET_HEAD_H - 1, 0, C_EDGE_DIM, C_EDGE_DIM, 0.5f, 0, 0);
            }
            if (!b->x1) continue;
            off = id == ST_RES && s_set.dmode == 1;
            if (id == ST_KEYS || id == ST_COOP) {
                button(b->x0, b->y0, b->x1, b->y1, row_label(id), i == s_sel, 0, id == ST_KEYS ? "Open" : ">");
                continue;
            }
            button(b->x0, b->y0, b->x1, b->y1, row_label(id), i == s_sel, off, NULL);
            text(300, b->y0, b->x1 - 30, b->y1, setting_value(id, buf, sizeof buf),
                 off ? C_TXT_OFF : id == ST_NAME && s_editing ? C_GOLD : C_TXT_HOT, F_BTN,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (!off && id != ST_NAME) {
                text(282, b->y0, 300, b->y1, "<", i == s_sel ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                text(b->x1 - 30, b->y0, b->x1 - 12, b->y1, ">", i == s_sel ? C_EDGE_HOT : C_EDGE_DIM,
                     F_BTN, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        if (set_total() > SET_VH) {
            int th = SET_VH * SET_VH / set_total();
            int ty = SET_VT + (SET_VH - th) * s_set_scroll / (set_total() - SET_VH);
            rrect(548, SET_VT, 552, SET_VB, 2, C_EDGE_DIM, C_EDGE_DIM, 0.3f, 0, 0);
            rrect(548, ty, 552, ty + th, 2, C_EDGE, C_EDGE, 0.9f, 0, 0);
        }
        button(bk->x0, bk->y0, bk->x1, bk->y1, "Back", s_sel == ST_BACK, 0, NULL);
        text(240, bk->y0, 544, bk->y1,
             hot == ST_NAME ? (s_editing ? "Type a name, Enter to finish" : "Enter to change the name")
             : hot == ST_STYLE ? "Original shows the game's own menu"
             : hot == ST_INPUT && s_set.input_ctl ? "Controller: the mouse is off in missions"
             : hot == ST_CAP ? ""
             : hot == ST_RES && s_set.dmode == 1 ? "Borderless always uses the desktop size"
             : is_value_row(hot) ? "Arrows or click to change"
             : hot == ST_BACK ? "Esc to go back" : "Click or Enter to open",
             C_TXT_OFF, F_BODY, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        break;
    }

    case SCR_KEYS: {
        int c;
        panel(80, 120, 560, 444);
        text(100, 128, 540, 162, "Key bindings", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(100, 164, 540, 166, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        text(104, 168, 320, 188, "Action", C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(330, 168, 430, 188, "Primary", C_TXT_OFF, F_BODY, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(436, 168, 536, 188, "Secondary", C_TXT_OFF, F_BODY, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < KB_VIS && s_kb_scroll + i < s_kb_n; i++) {
            Box *b = &s_box[i];
            int row = s_kb_scroll + i, a = s_kb_acts[row], sel = row == s_kb_pos;
            if (sel)
                cell(b->x0, b->y0, b->x1, b->y1, 1, 0);
            text(b->x0 + 8, b->y0, KB_NAME_END - 4, b->y1, avp_keys_name(a), sel ? C_TXT_HOT : C_TXT, F_BODY,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            for (c = 0; c < 2; c++) {
                int x0 = c ? 436 : 330, x1 = c ? 536 : 430;
                int hot = sel && c == s_kb_col, cap = hot && s_kb_cap;
                cell(x0, b->y0 + 1, x1, b->y1 - 1, hot, 0);
                text(x0, b->y0, x1, b->y1, cap ? "Press a key..." : avp_vk_name(avp_keys_vk(a, c)),
                     cap ? C_GOLD : sel ? C_TXT_HOT : C_TXT, F_BODY, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        if (s_kb_n > KB_VIS) {
            int th = (KB_VIS * KB_ROW_H) * KB_VIS / s_kb_n;
            int ty = KB_ROW_Y + (KB_VIS * KB_ROW_H - th) * s_kb_scroll / (s_kb_n - KB_VIS);
            rrect(548, KB_ROW_Y, 552, KB_ROW_Y + KB_VIS * KB_ROW_H, 2, C_EDGE_DIM, C_EDGE_DIM, 0.3f, 0, 0);
            rrect(548, ty, 552, ty + th, 2, C_EDGE, C_EDGE, 0.9f, 0, 0);
        }
        if (*s_kb_err) {
            if (GetTickCount() - s_kb_err_t < 3000)
                text(100, 360, 540, 382, s_kb_err, 0xFF5050u, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else
                *s_kb_err = 0;
        }
        button(s_box[KB_VIS].x0, s_box[KB_VIS].y0, s_box[KB_VIS].x1, s_box[KB_VIS].y1,
               "Reset to defaults", s_kb_pos == s_kb_n, 0, NULL);
        button(s_box[KB_VIS + 1].x0, s_box[KB_VIS + 1].y0, s_box[KB_VIS + 1].x1, s_box[KB_VIS + 1].y1,
               "Back", s_kb_pos == s_kb_n + 1, 0, NULL);
        text(100, 414, 540, 438, "Ctrl + 1-9 stores a control group. Mouse buttons are fixed.",
             C_TXT_OFF, F_BODY, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }

    case SCR_QUIT:
        panel(180, 196, 460, 304);
        text(196, 206, 444, 250, "Quit Aliens versus Predator: Extinction?", C_TXT_HOT, F_BTN,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        button(s_box[0].x0, s_box[0].y0, s_box[0].x1, s_box[0].y1, "Quit", s_sel == 0, 0, NULL);
        button(s_box[1].x0, s_box[1].y0, s_box[1].x1, s_box[1].y1, "Cancel", s_sel == 1, 0, NULL);
        break;
    }
}

static DWORD s_gone_t;

static int visible(void)
{
    if (s_forced) return 1;
    if (s_handoff) {

        if (!g_avp_mainmenu || GetTickCount() - s_handoff_t > 4000)
            s_handoff = 0;
        return 0;
    }
    if (APP_MODE == 0 && s_set.original_menu && s_screen == SCR_MAIN)
        return 0;
    if (g_avp_mainmenu) {
        s_gone_t = 0;
        return 1;
    }
    if (s_was_up) {
        DWORD now = GetTickCount();
        if (!s_gone_t) s_gone_t = now ? now : 1;
        if (now - s_gone_t < 400) return 1;
    }
    return 0;
}

static void go(int screen)
{
    if (s_screen == SCR_MAIN) s_sel_main = s_sel;
    if (screen == SCR_SETTINGS && s_screen != SCR_KEYS && s_screen != SCR_COOP && s_screen != SCR_COOPMODE) s_set_scroll = 0;
    avp_steam_want_friends(0);
    s_screen = screen;
    s_sel = screen == SCR_MAIN ? s_sel_main : screen == SCR_QUIT ? 1 : 0;
    s_editing = 0;
    s_kb_cap = 0;
    s_tf = TF_NONE;
    if (screen == SCR_MP) avp_coop_cancel();
    if (screen == SCR_SKIRM || screen == SCR_EDITOR || screen == SCR_HOST) preview_reset();
    if (screen == SCR_SKIRM) {
        int i;
        sk_load();
        for (i = 0; i < SK_ROWS; i++)
            if (s_sk[i].kind == K_REMOTE) s_sk[i].kind = K_AI;
    }
    if (screen == SCR_HOST) {
        *s_sk_err = 0;
        if (s_mode) {
            sk_load();
            host_rows();
        }
        s_sel = 1;
    }
    if (screen == SCR_JOIN) {
        s_join_logged = 0;
        if (!*s_ip) sscanf(s_set.recent[0], "%63[^: ]", s_ip);
    }
    if (screen == SCR_EDITOR) *s_sk_err = 0;
    if (screen == SCR_SKCUST || screen == SCR_NEDOPEN) { ks_refresh(); *s_sk_err = 0; s_sel = 0; }
    if (screen == SCR_COOP) {
        if (avp_coop_state() == 3) avp_coop_cancel();
        s_co_side = avp_coop_role();
        s_co_col = s_co_side == 2;
        s_mode = 0;
        s_join_logged = 0;
        *s_sk_err = 0;
        if (!*s_ip) sscanf(s_set.recent[0], "%63[^: ]", s_ip);
        s_co_race = 0;
        coop_missions();
        s_sel = s_co_side == 2 ? CO_JOIN : s_co_steam && avp_steam_status() != AVP_STEAM_READY ? 0 : 1;
    }
    if (screen == SCR_COOPMODE) s_sel = s_co_steam;
    if (screen == SCR_KEYS) {
        kb_build();
        s_kb_pos = s_kb_col = s_kb_scroll = 0;
        *s_kb_err = 0;
    }
}

static void back(void)
{
    sound(2);
    if (s_editing) { s_editing = 0; settings_save(); return; }
    if (s_tf) { tf_end(); return; }
    if (s_screen == SCR_HOST || s_screen == SCR_JOIN) {
        int from = s_screen;
        avp_coop_cancel();
        go(SCR_MP);
        s_sel = s_mode * 2 + (from == SCR_JOIN);
        return;
    }
    if (is_settings(s_screen)) settings_save();
    if (s_screen == SCR_MAIN) { go(SCR_QUIT); return; }
    if (s_screen == SCR_KEYS) {
        go(SCR_SETTINGS);
        s_sel = ST_KEYS;
        return;
    }
    if (s_screen == SCR_COOP) {
        avp_coop_cancel();
        go(SCR_COOPMODE);
        return;
    }
    if (s_screen == SCR_SKCUST) { avp_coop_cancel(); s_sk_net = 0; go(SCR_MAIN); return; }
    if (s_screen == SCR_COOPMODE) {
        avp_coop_cancel();
        if (s_coop_from == SCR_SKCUST) {
            go(SCR_SKCUST);
        } else if (s_coop_from == SCR_SETTINGS) {
            go(SCR_SETTINGS);
            s_sel = ST_COOP;
        } else {
            go(SCR_MAIN);
        }
        return;
    }
    if (s_forced && s_screen == SCR_SETTINGS && (!g_avp_mainmenu || s_set.original_menu)) {
        s_forced = 0;
        return;
    }
    s_forced = 0;
    go(SCR_MAIN);
}

static int s_res[72][2], s_res_n;

static int res_count(void)
{
    int wh[128], n, k, i;

    s_res[0][0] = s_res[0][1] = 0;
    s_res_n = 1;
    n = xbox_FramebufferDisplayModes(wh, 64);
    for (k = 0; k < n && s_res_n < 70; k++) {
        s_res[s_res_n][0] = wh[k * 2];
        s_res[s_res_n][1] = wh[k * 2 + 1];
        s_res_n++;
    }
    if (s_set.rw > 0 && s_set.rh > 0) {
        int found = 0;
        for (k = 1; k < s_res_n; k++)
            if (s_res[k][0] == s_set.rw && s_res[k][1] == s_set.rh) found = 1;
        if (!found && s_res_n < 71) {
            for (i = s_res_n; i > 1 && (long long)s_res[i - 1][0] * s_res[i - 1][1] > (long long)s_set.rw * s_set.rh; i--) {
                s_res[i][0] = s_res[i - 1][0];
                s_res[i][1] = s_res[i - 1][1];
            }
            s_res[i][0] = s_set.rw;
            s_res[i][1] = s_set.rh;
            s_res_n++;
        }
    }
    return s_res_n;
}

static int res_index(void)
{
    int k;

    for (k = 1; k < s_res_n; k++)
        if (s_res[k][0] == s_set.rw && s_res[k][1] == s_set.rh) return k;
    return 0;
}

static void change(int i, int dir)
{
    sound(2);
    switch (i) {
    case ST_DISPLAY:
        s_set.dmode = (s_set.dmode + dir + 3) % 3;
        display_apply();
        break;
    case ST_RES: {
        int n = res_count(), k = res_index();
        if (s_set.dmode == 1) return;
        k = (k + dir + n) % n;
        s_set.rw = s_res[k][0];
        s_set.rh = s_res[k][1];
        display_apply();
        break;
    }
    case ST_CAP: {
        int k, at = 0;
        for (k = 0; k < N_CAPS; k++)
            if (k_caps[k] == s_set.cap) at = k;
        s_set.cap = k_caps[(at + dir + N_CAPS) % N_CAPS];
        display_apply();
        break;
    }
    case ST_VSYNC:
        s_set.vsync = !s_set.vsync;
        display_apply();
        break;
#ifdef _WIN32
    case ST_WIDE: s_set.wide = (s_set.wide + dir + 3) % 3; avp_wide_set(s_set.wide); break;
#endif
    case ST_STYLE: s_set.original_menu = !s_set.original_menu; break;
    case ST_INPUT: s_set.input_ctl = !s_set.input_ctl; break;
    case ST_EDGE: s_set.edge_scroll = !s_set.edge_scroll; break;
    case ST_GLIDE: s_set.map_glide = !s_set.map_glide; break;
    case ST_NAME: s_editing = 1; return;
    default: return;
    }
    settings_apply_env();
    settings_save();
}

static void activate(int i)
{
    const MainItem *it;
    switch (s_screen) {
    case SCR_MAIN:
        it = main_item(i);
        if (it->game < 0)
            sound(2);
        if (it->game >= 0 && it->game <= 2)
            avp_skirmish_disarm();
        if (it->game >= 0) {
            fprintf(stderr, "[PCMENU] %s: hand to the game's button %d\n", it->label, it->game);
            s_sel_main = i;
            InterlockedExchange(&g_avp_menu_req, it->game);
            s_handoff = 1;
            s_handoff_t = GetTickCount();
        } else {
            fprintf(stderr, "[PCMENU] %s\n", it->label);
            if (it->screen == SCR_MP || it->screen == SCR_COOPMODE || it->screen == SCR_SKCUST)
                avp_skirmish_disarm();
            if (it->screen == SCR_COOPMODE) {
                s_coop_from = SCR_MAIN;
                s_sk_net = 0;
            }
            if (it->screen == SCR_SKCUST) s_sk_net = 0;
            if (it->screen == SCR_EDLAUNCH) launch_editor_exe(NULL);
            else go(it->screen);
        }
        break;
    case SCR_SKCUST: ks_activate(i); break;
    case SCR_NEDOPEN: nedopen_activate(i); break;
    case SCR_MP:
        sound(2);
        if (i == 4) back();
        else { s_mode = i >= 2; go(i & 1 ? SCR_JOIN : SCR_HOST); }
        break;
    case SCR_HOST: host_activate(i); break;
    case SCR_JOIN: join_activate(i); break;
    case SCR_SKIRM: sk_activate(i); break;
    case SCR_EDITOR: ed_activate(i); break;
    case SCR_COOP: coop_activate(i); break;
    case SCR_COOPMODE:
        sound(2);
        if (i == 2) { back(); break; }
        if (s_co_steam != i) avp_coop_cancel();
        s_co_steam = i;
        avp_skirmish_disarm();
        go(SCR_COOP);
        break;
    case SCR_SETTINGS: {
        int id = row_id(i);
        sound(2);
        if (id == ST_BACK) back();
        else if (id == ST_KEYS) go(SCR_KEYS);
        else if (id == ST_COOP) { s_coop_from = SCR_SETTINGS; go(SCR_COOPMODE); }
        else if (id == ST_NAME) { if (s_editing) { s_editing = 0; settings_save(); } else s_editing = 1; }
        else change(id, +1);
        break;
    }
    case SCR_QUIT:
        sound(2);
        if (i == 0) {
            fprintf(stderr, "[PCMENU] quit\n");
            settings_save();
#ifdef _WIN32
            PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
#else
            {
                SDL_Event q;
                memset(&q, 0, sizeof q);
                q.type = SDL_QUIT;
                SDL_PushEvent(&q);
            }
#endif
        } else {
            go(SCR_MAIN);
        }
        break;
    }
}

static int box_at(int x, int y)
{
    int i;
    for (i = 0; i < s_nbox; i++)
        if (x >= s_box[i].x0 && x < s_box[i].x1 && y >= s_box[i].y0 && y < s_box[i].y1)
            return i;
    return -1;
}

static int disabled(int i)
{
    if (s_screen == SCR_NEDOPEN) return i < KS_ROWS ? s_nm_scroll + i >= s_nm_n : i == KS_ROWS && s_nm_n <= 0;
    if (s_screen == SCR_SKCUST) {
        if (i < KS_ROWS) return s_nm_scroll + i >= s_nm_n;
        if (i >= KS_SLOT0 && i < KS_HOST) {
            int sl = (i - KS_SLOT0) / 3, c = (i - KS_SLOT0) % 3;
            return sl >= ks_nslots() || avp_coop_role() == 2 || (sl == 0 && c == 0);
        }
        return i == KS_START && avp_coop_role() == 2;
    }
    if (s_screen == SCR_JOIN) return (i == 3 && !join_go()) || (i > 3 && i < 7 && !*s_set.recent[i - 4]);
    if (s_screen == SCR_HOST) return i == (s_mode ? 2 + SK_START : 2) && avp_coop_state() != 2;
    if (s_screen == SCR_COOP && s_co_steam && (i < 2 || i >= CO_ADDR)) {
        int ss = avp_steam_status(), st = avp_coop_state(), role = avp_coop_role(), active = st == 1 || st == 2;
        AvpSteamFriend fl[3];
        if (i == 0) return ss == AVP_STEAM_READY || ss == AVP_STEAM_BUSY;
        if (i == 1) return ss != AVP_STEAM_READY && !(role == 1 && active);
        if (i == CO_ADDR || i == CO_BACK) return 0;
        if (i == CO_JPORT) return 1;
        if (i == CO_JOIN) return ss != AVP_STEAM_READY && !(role == 2 && active);
        return avp_steam_friends(fl, 3) <= i - CO_REC0;
    }
    if (s_screen == SCR_COOP) {
        if (i >= CO_TAB0 && i <= CO_START) {
            if (!coop_hosting()) return 1;
            if (i >= CO_ROW0 && i < CO_START) return s_co_scroll + i - CO_ROW0 >= s_co_n;
            return i == CO_START && (s_co_pick < 0 || !avp_coop_peer_ready());
        }
        return i >= CO_REC0 && i < CO_BACK && !*s_set.recent[i - CO_REC0];
    }
    return 0;
}

static void kb_capture(WPARAM w, LPARAM l)
{
    int vk = (int)w, sc = (int)((l >> 16) & 255), ext = (int)((l >> 24) & 1), c, a;
    if (l & (1 << 30)) return;
    if (vk == VK_ESCAPE) { s_kb_cap = 0; return; }
#ifdef _WIN32
    if (vk == VK_SHIFT) vk = (int)MapVirtualKeyA((UINT)sc, MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
    if (vk <= 0 || vk > 255 || vk == VK_PROCESSKEY) return;
#else
    (void)sc; (void)ext;
    if (vk <= 0 || vk > 255) return;
#endif
    a = s_kb_acts[s_kb_pos];
    c = avp_keys_bind(a, s_kb_col, vk);
    s_kb_cap = 0;
    if (c >= 0) {
        snprintf(s_kb_err, sizeof s_kb_err, "%s is already used by %s", avp_vk_name(vk), avp_keys_name(c));
        s_kb_err_t = GetTickCount();
    } else {
        *s_kb_err = 0;
    }
}

static void kb_activate(int p)
{
    sound(2);
    if (p < s_kb_n) {
        s_kb_cap = 1;
        *s_kb_err = 0;
    } else if (p == s_kb_n) {
        avp_keys_reset_defaults();
        settings_save();
        *s_kb_err = 0;
    } else {
        back();
    }
}

static int kb_pos_of(int i)
{
    return i < KB_VIS ? s_kb_scroll + i : s_kb_n + i - KB_VIS;
}

static int keys_input(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int fx, fy, i, p;
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_UP: case 'W':
            if (s_kb_pos > 0) { s_kb_pos--; sound(1); }
            break;
        case VK_DOWN: case 'S':
            if (s_kb_pos < s_kb_n + 1) { s_kb_pos++; sound(1); }
            break;
        case VK_PRIOR:
            s_kb_pos = s_kb_pos >= s_kb_n ? s_kb_n - 1 : s_kb_pos > KB_VIS ? s_kb_pos - KB_VIS : 0;
            sound(1);
            break;
        case VK_NEXT:
            if (s_kb_pos < s_kb_n) s_kb_pos = s_kb_pos + KB_VIS < s_kb_n ? s_kb_pos + KB_VIS : s_kb_n - 1;
            sound(1);
            break;
        case VK_LEFT: case 'A':
            if (s_kb_pos < s_kb_n) s_kb_col = 0; else s_kb_pos = s_kb_n;
            break;
        case VK_RIGHT: case 'D':
            if (s_kb_pos < s_kb_n) s_kb_col = 1; else s_kb_pos = s_kb_n + 1;
            break;
        case VK_RETURN: case 'Z': case VK_SPACE: kb_activate(s_kb_pos); break;
        case VK_ESCAPE: case 'X': back(); return 1;
        case VK_DELETE: case VK_BACK:
            if (s_kb_pos < s_kb_n) {
                avp_keys_bind(s_kb_acts[s_kb_pos], s_kb_col, 0);
                *s_kb_err = 0;
                sound(2);
            }
            break;
        }
        kb_view();
        return 1;
    }
    if (m == WM_MOUSEWHEEL) {
        int d = (short)HIWORD(w), max = s_kb_n > KB_VIS ? s_kb_n - KB_VIS : 0;
        if (d && !s_kb_cap) {
            s_kb_scroll += d > 0 ? -3 : 3;
            if (s_kb_scroll > max) s_kb_scroll = max;
            if (s_kb_scroll < 0) s_kb_scroll = 0;
            if (s_kb_pos < s_kb_n) {
                if (s_kb_pos < s_kb_scroll) s_kb_pos = s_kb_scroll;
                if (s_kb_pos >= s_kb_scroll + KB_VIS) s_kb_pos = s_kb_scroll + KB_VIS - 1;
            }
        }
        return 1;
    }
    if (m == WM_MOUSEMOVE) {
        xbox_FramebufferClientToFrame(h, (short)LOWORD(l), (short)HIWORD(l), &fx, &fy);
        if (fx != s_mx || fy != s_my) {
            s_mx = fx; s_my = fy;
            i = box_at(fx, fy);
            if (i >= 0 && !s_kb_cap) {
                p = kb_pos_of(i);
                if (p != s_kb_pos) { s_kb_pos = p; sound(1); }
                if (i < KB_VIS && fx >= KB_NAME_END) s_kb_col = fx >= KB_COL_SPLIT;
            }
        }
        return 1;
    }
    if (m == WM_LBUTTONDOWN) {
        xbox_FramebufferClientToFrame(h, (short)LOWORD(l), (short)HIWORD(l), &fx, &fy);
        s_kb_cap = 0;
        i = box_at(fx, fy);
        if (i < 0) return 1;
        s_kb_pos = kb_pos_of(i);
        if (i < KB_VIS) {
            if (fx < KB_NAME_END) return 1;
            s_kb_col = fx >= KB_COL_SPLIT;
        }
        kb_activate(s_kb_pos);
        return 1;
    }
    if (m == WM_RBUTTONDOWN) {
        if (s_kb_cap) s_kb_cap = 0; else back();
        return 1;
    }
    return 1;
}

static void move_sel(int d)
{
    int n = s_nbox, k, was = s_sel;
    if (n <= 0) return;
    for (k = 0; k < n; k++) {
        s_sel = (s_sel + d + n) % n;
        if (!disabled(s_sel)) break;
    }
    if (s_sel != was) sound(1);
}

static int input_hook(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int fx, fy, i;

#ifdef _WIN32
    if (s_hwnd != h) {
        extern HICON avp_game_icon(void);
        HICON ic = avp_game_icon();
        if (ic) {
            SendMessageA(h, WM_SETICON, ICON_BIG, (LPARAM)ic);
            SendMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)ic);
        }
    }
#endif
    s_hwnd = h;
    if (g_avp_pcmenu_up && s_screen == SCR_KEYS && s_kb_cap) {
        if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) { kb_capture(w, l); return 1; }
        if (m == WM_SYSKEYUP || m == WM_SYSCHAR || m == WM_CHAR) return 1;
    }
    if (m == WM_KEYDOWN && avp_keys_match(AVP_ACT_SETTINGS, (int)w) && !(l & (1 << 30))) {
        if (s_forced && is_settings(s_screen)) {
            settings_save();
            s_forced = 0;
            if (g_avp_mainmenu) go(SCR_MAIN);
        } else if (!g_avp_pcmenu_up || !is_settings(s_screen)) {
            s_forced = 1;
            go(SCR_SETTINGS);
        }
        return 1;
    }
    if (!g_avp_pcmenu_up)
        return m == WM_MOUSEWHEEL ? 0 : avp_editor_key(m, w, l);

    if (m == WM_KEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP)
        return m == WM_KEYUP ? 0 : 1;
    if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_SYSCHAR)
        return 0;
    layout();
    if (s_screen == SCR_KEYS) return keys_input(h, m, w, l);

    if (m == WM_MOUSEWHEEL && s_screen == SCR_SETTINGS) {
        int d = (short)HIWORD(w), max = set_total() - SET_VH, pick = -1;
        if (d && !s_editing) {
            s_set_scroll += d > 0 ? -SET_PITCH * 2 : SET_PITCH * 2;
            if (s_set_scroll > max) s_set_scroll = max;
            if (s_set_scroll < 0) s_set_scroll = 0;
            if (s_sel < ST_BACK && !set_vis(s_sel)) {
                for (i = 0; i < ST_BACK; i++)
                    if (set_vis(i) && (pick < 0 || d > 0)) pick = i;
                if (pick >= 0) s_sel = pick;
            }
        }
        return 1;
    }
    if (m == WM_MOUSEWHEEL && s_screen == SCR_COOP) {
        int d = (short)HIWORD(w);
        if (d) coop_scroll(d > 0 ? -1 : 1);
        return 1;
    }
    if (m == WM_CHAR) {
        if (s_tf) {
            if (w >= 32 && w < 127) tf_add((int)w);
            return 1;
        }
        if (s_editing && w >= 32 && w < 127) {
            size_t n = strlen(s_set.name);
            if (n < 16) { s_set.name[n] = (char)w; s_set.name[n + 1] = 0; }
        }
        return 1;
    }
    if (m == WM_KEYDOWN) {
        if (s_tf) {
            if (w == VK_BACK) tf_back();
            else if (w == VK_RETURN || w == VK_ESCAPE) tf_end();
            return 1;
        }
        if (s_editing) {
            if (w == VK_BACK) {
                size_t n = strlen(s_set.name);
                if (n) s_set.name[n - 1] = 0;
            } else if (w == VK_RETURN || w == VK_ESCAPE) {
                if (!*s_set.name) strcpy(s_set.name, "Player");
                s_editing = 0;
                settings_save();
            }
            return 1;
        }
        switch (w) {
        case VK_UP: case 'W':
            if (sk_screen()) sk_nav(0, -1); else if (s_screen == SCR_COOP) coop_nav(0, -1); else move_sel(-1);
            break;
        case VK_DOWN: case 'S':
            if (sk_screen()) sk_nav(0, +1); else if (s_screen == SCR_COOP) coop_nav(0, +1); else move_sel(+1);
            break;
        case VK_LEFT: case 'A':
            if (sk_screen()) sk_nav(-1, 0);
            else if (s_screen == SCR_COOP) coop_nav(-1, 0);
            else if (s_screen == SCR_MP || s_screen == SCR_JOIN || s_screen == SCR_HOST) mp_lr();
            else if (s_screen == SCR_EDITOR) { if (s_sel == 0) sk_map(-1); }
            else if (is_value_row(row_id(s_sel))) change(row_id(s_sel), -1);
            else if (s_screen == SCR_QUIT) s_sel = 0;
            break;
        case VK_RIGHT: case 'D':
            if (sk_screen()) sk_nav(+1, 0);
            else if (s_screen == SCR_COOP) coop_nav(+1, 0);
            else if (s_screen == SCR_MP || s_screen == SCR_JOIN || s_screen == SCR_HOST) mp_lr();
            else if (s_screen == SCR_EDITOR) { if (s_sel == 0) sk_map(+1); }
            else if (is_value_row(row_id(s_sel))) change(row_id(s_sel), +1);
            else if (s_screen == SCR_QUIT) s_sel = 1;
            break;
        case VK_RETURN: case 'Z': case VK_SPACE: activate(s_sel); break;
        case VK_ESCAPE: case 'X': case VK_BACK: back(); break;
        }
        return 1;
    }
    if (m == WM_MOUSEMOVE) {
        xbox_FramebufferClientToFrame(h, (short)LOWORD(l), (short)HIWORD(l), &fx, &fy);
        if (fx != s_mx || fy != s_my) {
            s_mx = fx; s_my = fy;
            i = box_at(fx, fy);
            if (i >= 0 && !disabled(i) && !s_editing && !s_tf && i != s_sel) {
                s_sel = i;
                sound(1);
            }
        }
        return 1;
    }
    if (m == WM_LBUTTONDOWN) {
        xbox_FramebufferClientToFrame(h, (short)LOWORD(l), (short)HIWORD(l), &fx, &fy);
        i = box_at(fx, fy);
        if (s_editing && row_id(i) != ST_NAME) { s_editing = 0; settings_save(); }
        if (s_tf) tf_end();
        if (i >= 0 && !disabled(i)) {
            s_sel = i;
            if ((s_screen == SCR_SKIRM || s_screen == SCR_EDITOR) && i == 0) {
                sound(2);
                sk_map(fx < 320 ? -1 : +1);
            } else if (s_screen == SCR_HOST && s_mode && i == 2) {
                sound(2);
                sk_map(fx < 210 ? -1 : +1);
            } else if (is_value_row(row_id(i)))
                change(row_id(i), fx < 300 ? -1 : +1);
            else
                activate(i);
        }
        return 1;
    }
    if (m == WM_RBUTTONDOWN) {

        if (s_screen != SCR_MAIN || s_editing)
            back();
        return 1;
    }
    return 1;
}

void avp_ui_panel(int x0, int y0, int x1, int y1) { panel(x0, y0, x1, y1); }
void avp_ui_card(int x0, int y0, int x1, int y1) { card(x0, y0, x1, y1); }
void avp_ui_cell(int x0, int y0, int x1, int y1, int hot, int off) { cell(x0, y0, x1, y1, hot, off); }
void avp_ui_button(int x0, int y0, int x1, int y1, const char *label, int hot, int off, const char *tag)
{
    button(x0, y0, x1, y1, label, hot, off, tag);
}
void avp_ui_rect(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha)
{
    rrect(x0, y0, x1, y1, 0, rgb, rgb, alpha, 0, 0);
}
void avp_ui_frame(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    rrect(x0, y0, x1, y1, 0, rgb, rgb, 0.0f, rgb, 1.2f);
}
void avp_ui_line(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    rrect(x0, y0, x1, y1, 0, rgb, rgb, 1.0f, 0, 0);
}
void avp_ui_text(int x0, int y0, int x1, int y1, const char *str, uint32_t rgb, int small, int align)
{
    text(x0, y0, x1, y1, str, rgb, small ? F_BODY : F_BTN,
         (align == 1 ? DT_CENTER : align == 2 ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE);
}
void avp_ui_title(int x0, int y0, int x1, int y1, const char *str, uint32_t rgb)
{
    text(x0, y0, x1, y1, str, rgb, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

extern void avp_overlay_draw(uint32_t *rgb, int w, int h);
static int s_menu_off;
static int s_up_now;

static int hires_ok(void)
{
    return xbox_FramebufferLayerReady() && gf_ready();
}

static void draw_hook(uint32_t *rgb, int w, int h)
{
    int up;
    avp_overlay_draw(rgb, w, h);
    if (s_menu_off)
        return;
    up = visible();
    if (up != s_was_up) {
        column_reset();
        if (up) {
            coop_screen_reset();
            xbox_InputKeysClear();
            if (!s_forced) { s_screen = SCR_MAIN; s_sel = s_sel_main; s_editing = 0; }
            fprintf(stderr, "[PCMENU] up (%s)\n", s_forced ? "forced" : "main menu");
        } else {
            fprintf(stderr, "[PCMENU] down\n");
        }
        s_was_up = up;
    }
    InterlockedExchange(&g_avp_pcmenu_up, up);
    s_up_now = up;
    if (!up)
        return;
    {
        static int seen_mode = -1;
        int m = xbox_FramebufferDisplayMode();
        if (m != seen_mode) {
            int was = seen_mode;
            seen_mode = m;
            if (m != s_set.dmode) {
                s_set.dmode = m;
                if (was >= 0) settings_save();
            }
        }
    }
    if (hires_ok() || !dib_ensure(w, h))
        return;
    memcpy(s_px, rgb, (size_t)w * h * 4);
    s_nitem = 0;
    s_ntext = 0;
    draw_screen();
    text_flush();
    render_all();
    memcpy(rgb, s_px, (size_t)w * h * 4);
}

typedef struct { uint32_t hash; int x0, y0, x1, y1; } PrevItem;
static PrevItem s_pv[MAX_ITEMS];
static int s_npv;
static int s_pgeom[6];
static uint32_t *s_ppx;

static void clip_union(Clip *d, int x0, int y0, int x1, int y1)
{
    if (x0 < d->x0) d->x0 = x0;
    if (y0 < d->y0) d->y0 = y0;
    if (x1 > d->x1) d->x1 = x1;
    if (y1 > d->y1) d->y1 = y1;
}

static void layer_hook(XboxFbLayer *L)
{
    uint32_t *sp = s_px;
    int sw = s_w, sh = s_h, geom[6], force, i, n, y;
    Clip D, C;
    if (!hires_ok())
        return;
    geom[0] = L->vx; geom[1] = L->vy; geom[2] = L->vw; geom[3] = L->vh; geom[4] = L->w; geom[5] = L->h;
    s_px = L->px; s_w = L->w; s_h = L->h;
    s_alpha = 1;
    s_S = (float)L->vh / (float)LH;
    s_ox = (float)L->vx;
    s_oy = (float)L->vy;
    s_nitem = 0;
    s_ntext = 0;
    if (s_up_now && !s_menu_off) {
        draw_screen();
        text_flush();
    }
    n = s_nitem;
    force = L->full || L->px != s_ppx || memcmp(geom, s_pgeom, sizeof geom) != 0;
    if (L->full || L->px != s_ppx)
        s_npv = 0;
    D.x0 = D.y0 = 0x7FFFFFFF;
    D.x1 = D.y1 = -0x7FFFFFFF;
    C = D;
    for (i = 0; i < n; i++) {
        const Item *it = &s_item[i];
        clip_union(&C, it->bx0, it->by0, it->bx1, it->by1);
        if (force || i >= s_npv || s_pv[i].hash != it->hash) {
            clip_union(&D, it->bx0, it->by0, it->bx1, it->by1);
            if (i < s_npv)
                clip_union(&D, s_pv[i].x0, s_pv[i].y0, s_pv[i].x1, s_pv[i].y1);
        }
    }
    for (i = n; i < s_npv; i++)
        clip_union(&D, s_pv[i].x0, s_pv[i].y0, s_pv[i].x1, s_pv[i].y1);
    if (force)
        for (i = 0; i < s_npv && i < n; i++)
            clip_union(&D, s_pv[i].x0, s_pv[i].y0, s_pv[i].x1, s_pv[i].y1);
    if (D.x0 < 0) D.x0 = 0;
    if (D.y0 < 0) D.y0 = 0;
    if (D.x1 > L->w) D.x1 = L->w;
    if (D.y1 > L->h) D.y1 = L->h;
    if (C.x0 < 0) C.x0 = 0;
    if (C.y0 < 0) C.y0 = 0;
    if (C.x1 > L->w) C.x1 = L->w;
    if (C.y1 > L->h) C.y1 = L->h;
    if (D.x1 > D.x0 && D.y1 > D.y0) {
        for (y = D.y0; y < D.y1; y++)
            memset(L->px + (size_t)y * (size_t)L->w + D.x0, 0, (size_t)(D.x1 - D.x0) * 4);
        for (i = 0; i < n; i++) {
            const Item *it = &s_item[i];
            if (it->bx1 > D.x0 && it->bx0 < D.x1 && it->by1 > D.y0 && it->by0 < D.y1)
                item_render(it, &D);
        }
        L->dx0 = D.x0; L->dy0 = D.y0; L->dx1 = D.x1; L->dy1 = D.y1;
    }
    if (C.x1 > C.x0 && C.y1 > C.y0) {
        L->cx0 = C.x0; L->cy0 = C.y0; L->cx1 = C.x1; L->cy1 = C.y1;
    }
    for (i = 0; i < n; i++) {
        s_pv[i].hash = s_item[i].hash;
        s_pv[i].x0 = s_item[i].bx0; s_pv[i].y0 = s_item[i].by0;
        s_pv[i].x1 = s_item[i].bx1; s_pv[i].y1 = s_item[i].by1;
    }
    s_npv = n;
    memcpy(s_pgeom, geom, sizeof geom);
    s_ppx = L->px;
    s_nitem = 0;
    s_alpha = 0;
    s_S = 1.0f;
    s_ox = s_oy = 0;
    s_px = sp; s_w = sw; s_h = sh;
}

int avp_pcmenu_hides_game(void)
{
    return g_avp_mainmenu && !s_menu_off && (APP_MODE != 0 || !s_set.original_menu);
}

void avp_pcmenu_init(void)
{
    const char *e = getenv("AVP_PCMENU");
    xbox_FramebufferSetDrawHook(draw_hook);
    xbox_FramebufferSetLayerHook(0, avp_overlay_layer);
    xbox_FramebufferSetLayerHook(1, layer_hook);
    if (e && e[0] == '0') {
        s_menu_off = 1;
        fprintf(stderr, "[PCMENU] off\n");
        return;
    }
    xbox_FramebufferSetInputHook(input_hook);
}

