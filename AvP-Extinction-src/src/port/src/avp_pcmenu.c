#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void (*xbox_fb_draw_hook)(uint32_t *rgb, int w, int h);
typedef int (*xbox_fb_input_hook)(HWND h, UINT m, WPARAM w, LPARAM l);
extern void xbox_FramebufferSetDrawHook(xbox_fb_draw_hook fn);
extern void xbox_FramebufferSetInputHook(xbox_fb_input_hook fn);
extern int  xbox_FramebufferIsFullscreen(void);
extern void xbox_FramebufferSetFullscreen(HWND h, int on);
extern void xbox_FramebufferClientToFrame(HWND h, int cx, int cy, int *fx, int *fy);
extern void xbox_FramebufferSetWindowScale(HWND h, int scale);
extern void xbox_InputKeysClear(void);

volatile LONG g_avp_mainmenu;
volatile LONG g_avp_menu_req = -1;
volatile LONG g_avp_pcmenu_up;
volatile LONG g_avp_menu_sound;

static void sound(int which) { InterlockedExchange(&g_avp_menu_sound, which); }

typedef struct {
    int fullscreen, scale, edge_scroll, map_glide;
    char name[24];
} Settings;

static Settings s_set = { 0, 2, 1, 0, "Player" };
static const char k_ini[] = "avp_settings.ini";

static void settings_apply_env(void)
{
    _putenv(s_set.edge_scroll ? "AVP_NO_EDGE=" : "AVP_NO_EDGE=1");
    _putenv(s_set.map_glide ? "AVP_MAP_GLIDE=1" : "AVP_MAP_GLIDE=");
}

static void settings_save(void)
{
    FILE *f = fopen(k_ini, "w");
    if (!f) return;
    fprintf(f, "; Aliens versus Predator: Extinction (PC port) settings\n"
               "fullscreen=%d\nwindow_scale=%d\nedge_scroll=%d\nminimap_glide=%d\nplayer_name=%s\n",
            s_set.fullscreen, s_set.scale, s_set.edge_scroll, s_set.map_glide, s_set.name);
    fclose(f);
}

void avp_settings_load(void)
{
    FILE *f = fopen(k_ini, "r");
    char line[128];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *v;
            if (!eq || line[0] == ';') continue;
            *eq = 0;
            v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            if (!strcmp(line, "fullscreen")) s_set.fullscreen = atoi(v) != 0;
            else if (!strcmp(line, "window_scale")) s_set.scale = atoi(v);
            else if (!strcmp(line, "edge_scroll")) s_set.edge_scroll = atoi(v) != 0;
            else if (!strcmp(line, "minimap_glide")) s_set.map_glide = atoi(v) != 0;
            else if (!strcmp(line, "player_name") && *v)
                snprintf(s_set.name, sizeof s_set.name, "%.16s", v);
        }
        fclose(f);
    }
    if (s_set.scale < 1 || s_set.scale > 4) s_set.scale = 2;
    if (!getenv("RECOMP_FULLSCREEN") && s_set.fullscreen)
        _putenv("RECOMP_FULLSCREEN=1");
    if (!getenv("RECOMP_WINDOW_SCALE")) {
        char b[40];
        snprintf(b, sizeof b, "RECOMP_WINDOW_SCALE=%d", s_set.scale);
        _putenv(b);
    }
    if (!getenv("AVP_NO_EDGE") && !getenv("AVP_MAP_GLIDE"))
        settings_apply_env();
    fprintf(stderr, "[PCMENU] settings: fullscreen %d scale %d edge %d glide %d name %s\n",
            s_set.fullscreen, s_set.scale, s_set.edge_scroll, s_set.map_glide, s_set.name);
}

static uint32_t *s_px;
static int s_w, s_h;
static HDC s_dc;
static HBITMAP s_bmp;
static HFONT s_font[3];

enum { F_BTN, F_TITLE, F_BODY };

typedef struct { RECT r; char s[160]; uint32_t c; int font; UINT fmt; } Text;
static Text s_text[96];
static int s_ntext;

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
        static const struct { int h, wt; } spec[3] = { { 19, FW_SEMIBOLD }, { 27, FW_BOLD }, { 15, FW_NORMAL } };
        int i;
        for (i = 0; i < 3; i++) {
            s_font[i] = CreateFontA(-spec[i].h, 0, 0, 0, spec[i].wt, 0, 0, 0, DEFAULT_CHARSET,
                                    OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH, i == F_BODY ? "Bahnschrift" : "Bahnschrift SemiBold Condensed");
        }
    }
    return 1;
}

static uint32_t mix(uint32_t d, uint32_t c, float a)
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

static uint32_t lerp_c(uint32_t a, uint32_t b, float t) { return mix(a, b, t); }

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void rrect(int x0, int y0, int x1, int y1, float rad,
                  uint32_t top, uint32_t bot, float fill_a, uint32_t edge, float bw)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    int x, y;
    for (y = y0 - 1; y <= y1; y++) {
        uint32_t fc;
        if (y < 0 || y >= s_h) continue;
        fc = lerp_c(top, bot, clampf((float)(y - y0) / (float)(y1 - y0 > 0 ? y1 - y0 : 1)));
        for (x = x0 - 1; x <= x1; x++) {
            float px, py, qx, qy, d, fa, ea;
            uint32_t *p;
            if (x < 0 || x >= s_w) continue;
            px = fabsf(x + 0.5f - cx) - (hw - rad);
            py = fabsf(y + 0.5f - cy) - (hh - rad);
            qx = px > 0 ? px : 0; qy = py > 0 ? py : 0;
            d = sqrtf(qx * qx + qy * qy) + (px > py ? (px < 0 ? px : 0) : (py < 0 ? py : 0)) - rad;
            fa = clampf(0.5f - d);
            if (fa <= 0) continue;
            p = &s_px[(size_t)y * s_w + x];
            *p = mix(*p, fc, fa * fill_a);
            if (bw > 0) {
                ea = fa - clampf(0.5f - (d + bw));
                if (ea > 0) *p = mix(*p, edge, ea);
            }
        }
    }
}

static void text(int x0, int y0, int x1, int y1, const char *s, uint32_t c, int font, UINT fmt)
{
    Text *t;
    if (s_ntext >= (int)(sizeof s_text / sizeof s_text[0])) return;
    t = &s_text[s_ntext++];
    SetRect(&t->r, x0, y0, x1, y1);
    snprintf(t->s, sizeof t->s, "%s", s);
    t->c = c; t->font = font; t->fmt = fmt;
}

static void text_flush(void)
{
    int i;
    GdiFlush();
    SetBkMode(s_dc, TRANSPARENT);
    for (i = 0; i < s_ntext; i++) {
        Text *t = &s_text[i];
        RECT sh = t->r;
        SelectObject(s_dc, s_font[t->font]);
        OffsetRect(&sh, 1, 1);
        SetTextColor(s_dc, RGB(0, 0, 0));
        DrawTextA(s_dc, t->s, -1, &sh, t->fmt | DT_NOPREFIX);
        SetTextColor(s_dc, RGB((t->c >> 16) & 255, (t->c >> 8) & 255, t->c & 255));
        DrawTextA(s_dc, t->s, -1, &t->r, t->fmt | DT_NOPREFIX);
    }
    GdiFlush();
    s_ntext = 0;
}

#define C_EDGE      0x3CC8E6u
#define C_EDGE_DIM  0x2E7896u
#define C_EDGE_HOT  0x8CF0FFu
#define C_TXT       0xD8E6F0u
#define C_TXT_HOT   0xFFFFFFu
#define C_TXT_OFF   0x7C8794u
#define C_GOLD      0xF0D040u

static void button(int x0, int y0, int x1, int y1, const char *label, int hot, int off, const char *tag)
{
    if (off)
        rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f - 1, 0x2A2E36u, 0x1A1D22u, 0.85f, 0x50565Eu, 2);
    else if (hot)
        rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f - 1, 0x2A64B4u, 0x143A78u, 0.92f, C_EDGE_HOT, 2.2f);
    else
        rrect(x0, y0, x1, y1, (y1 - y0) * 0.5f - 1, 0x1C3C70u, 0x0E2246u, 0.80f, C_EDGE_DIM, 2);
    text(x0 + 10, y0, x1 - 8, y1, label, off ? C_TXT_OFF : hot ? C_TXT_HOT : C_TXT, F_BTN,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (tag)
        text(x0 + 10, y0, x1 - 12, y1, tag, off ? C_TXT_OFF : C_GOLD, F_BODY,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void panel(int x0, int y0, int x1, int y1)
{
    rrect(x0, y0, x1, y1, 14, 0x0A1628u, 0x060C18u, 0.88f, C_EDGE, 2);
}

enum { SCR_MAIN, SCR_COOP, SCR_MP, SCR_SETTINGS, SCR_QUIT };

typedef struct { const char *label; int game; int screen; const char *tag; } MainItem;
static const MainItem k_main[] = {
    { "Campaign",       0, -1, NULL },
    { "Co-op Campaign", -1, SCR_COOP, NULL },
    { "Load Game",      1, -1, NULL },
    { "Tutorials",      2, -1, NULL },
    { "Game Options",   3, -1, NULL },
    { "PC Settings",    -1, SCR_SETTINGS, NULL },
    { "Bestiary",       4, -1, NULL },
    { "Extras",         5, -1, NULL },
    { "Quit",           -1, SCR_QUIT, NULL },
};
#define N_MAIN ((int)(sizeof k_main / sizeof k_main[0]))

enum { ST_DISPLAY, ST_SCALE, ST_EDGE, ST_GLIDE, ST_NAME, ST_WIDE, ST_KEYS, ST_BACK, ST_N };

static HWND s_hwnd;
static int s_screen, s_sel, s_sel_main, s_forced, s_editing;
static int s_handoff;
static DWORD s_handoff_t;
static int s_mx = -1, s_my = -1;
static int s_was_up;

typedef struct { int x0, y0, x1, y1; } Box;
static Box s_box[16];
static int s_nbox;

static void layout_main(void)
{
    int i;
    s_nbox = 0;
    for (i = 0; i < N_MAIN; i++) {
        Box b = { 408, 140 + i * 29, 612, 140 + i * 29 + 25 };
        s_box[s_nbox++] = b;
    }
}

static void layout_settings(void)
{
    int i;
    s_nbox = 0;
    for (i = 0; i < ST_N; i++) {
        Box b = { 96, 172 + i * 30, 544, 172 + i * 30 + 25 };
        if (i == ST_BACK) { b.x0 = 96; b.x1 = 260; b.y0 += 8; b.y1 += 8; }
        s_box[s_nbox++] = b;
    }
}

static void layout_single(int x0, int y0, int x1, int y1)
{
    Box b = { x0, y0, x1, y1 };
    s_nbox = 0;
    s_box[s_nbox++] = b;
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
    case SCR_COOP: case SCR_MP: layout_single(266, 368, 374, 394); break;
    case SCR_QUIT: layout_quit(); break;
    }
}

static const char *setting_value(int i, char *buf, size_t n)
{
    switch (i) {
    case ST_DISPLAY: return xbox_FramebufferIsFullscreen() ? "Fullscreen" : "Windowed";
    case ST_SCALE:
        snprintf(buf, n, "%dx  (%d x %d)", s_set.scale, 640 * s_set.scale, 480 * s_set.scale);
        return buf;
    case ST_EDGE: return s_set.edge_scroll ? "On" : "Off";
    case ST_GLIDE: return s_set.map_glide ? "Glide" : "Jump";
    case ST_NAME:
        snprintf(buf, n, "%s%s", s_set.name, s_editing && (GetTickCount() / 400) % 2 ? "_" : "");
        return buf;
    case ST_WIDE: case ST_KEYS: return "Coming soon";
    }
    return "";
}

static void draw_screen(void)
{
    char buf[64];
    int i;
    if (s_screen != SCR_MAIN) {

        panel(398, 130, 622, 140 + N_MAIN * 29 + 6);
        for (i = 0; i < N_MAIN; i++)
            button(408, 140 + i * 29, 612, 140 + i * 29 + 25, k_main[i].label,
                   i == s_sel_main, 0, k_main[i].tag);
        text_flush();
        rrect(0, 0, s_w, s_h, 0, 0, 0, 0.35f, 0, 0);
    }
    layout();
    switch (s_screen) {
    case SCR_MAIN:
        panel(398, 130, 622, 140 + N_MAIN * 29 + 6);
        for (i = 0; i < N_MAIN; i++)
            button(s_box[i].x0, s_box[i].y0, s_box[i].x1, s_box[i].y1, k_main[i].label,
                   i == s_sel, 0, k_main[i].tag);
        break;

    case SCR_COOP:
    case SCR_MP: {
        int coop = s_screen == SCR_COOP;
        panel(110, 136, 530, 408);
        text(130, 146, 510, 180, coop ? "Co-op Campaign" : "Multiplayer", C_TXT_HOT, F_TITLE,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(130, 182, 510, 184, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        text(130, 196, 510, 356,
             coop ? "Play the campaign together: one player hosts, a friend joins. "
                    "Start co-op from AvPLauncher: Host co-op, or enter the host's IP and Join co-op.\n\n"
                    "The host starts with the whole army. Select units and press G to give them "
                    "to player 2 for the rest of the mission. Player 2's units carry a small marker."
                  : "Battle other players online as Marines, Predators or Aliens, "
                    "on skirmish maps.\n\n"
                    "Coming soon. Not available in this build.",
             C_TXT, F_BODY, DT_LEFT | DT_WORDBREAK);
        if (*s_set.name) {
            snprintf(buf, sizeof buf, "Playing as %s", s_set.name);
            text(130, 330, 510, 356, buf, C_GOLD, F_BODY, DT_LEFT | DT_SINGLELINE);
        }
        button(s_box[0].x0, s_box[0].y0, s_box[0].x1, s_box[0].y1, "Back", 1, 0, NULL);
        break;
    }

    case SCR_SETTINGS: {
        static const char *const labels[ST_N] = {
            "Display", "Window size", "Edge scrolling", "Minimap click", "Player name",
            "Widescreen", "Key bindings", "Back",
        };
        panel(80, 120, 560, 444);
        text(100, 128, 540, 162, "PC Settings", C_TXT_HOT, F_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rrect(100, 164, 540, 166, 1, C_EDGE_DIM, C_EDGE_DIM, 1, 0, 0);
        for (i = 0; i < ST_N; i++) {
            Box *b = &s_box[i];
            int off = i == ST_WIDE || i == ST_KEYS
                   || (i == ST_SCALE && xbox_FramebufferIsFullscreen());
            if (i == ST_BACK) {
                button(b->x0, b->y0, b->x1, b->y1, "Back", i == s_sel, 0, NULL);
                continue;
            }
            button(b->x0, b->y0, b->x1, b->y1, labels[i], i == s_sel, off, NULL);
            text(300, b->y0, b->x1 - 30, b->y1, setting_value(i, buf, sizeof buf),
                 off ? C_TXT_OFF : i == ST_NAME && s_editing ? C_GOLD : C_TXT_HOT, F_BTN,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (!off && i != ST_NAME) {
                text(282, b->y0, 300, b->y1, "<", i == s_sel ? C_EDGE_HOT : C_EDGE_DIM, F_BTN,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                text(b->x1 - 30, b->y0, b->x1 - 12, b->y1, ">", i == s_sel ? C_EDGE_HOT : C_EDGE_DIM,
                     F_BTN, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        text(280, 418, 544, 440,
             s_sel == ST_NAME ? (s_editing ? "Type a name, Enter to finish" : "Enter to change the name")
                              : "Arrows or click to change  -  Esc to go back",
             C_TXT_OFF, F_BODY, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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

static int visible(void)
{
    if (s_forced) return 1;
    if (s_handoff) {

        if (!g_avp_mainmenu || GetTickCount() - s_handoff_t > 4000)
            s_handoff = 0;
        return 0;
    }
    return g_avp_mainmenu != 0;
}

static void go(int screen)
{
    if (s_screen == SCR_MAIN) s_sel_main = s_sel;
    s_screen = screen;
    s_sel = screen == SCR_MAIN ? s_sel_main : screen == SCR_QUIT ? 1 : 0;
    s_editing = 0;
}

static void back(void)
{
    sound(2);
    if (s_editing) { s_editing = 0; settings_save(); return; }
    if (s_screen == SCR_SETTINGS) settings_save();
    if (s_screen == SCR_MAIN) { go(SCR_QUIT); return; }
    if (s_forced && s_screen == SCR_SETTINGS && !g_avp_mainmenu) {
        s_forced = 0;
        return;
    }
    s_forced = 0;
    go(SCR_MAIN);
}

static void change(int i, int dir)
{
    sound(2);
    switch (i) {
    case ST_DISPLAY:
        xbox_FramebufferSetFullscreen(s_hwnd, !xbox_FramebufferIsFullscreen());
        s_set.fullscreen = xbox_FramebufferIsFullscreen();
        break;
    case ST_SCALE:
        if (xbox_FramebufferIsFullscreen()) return;
        s_set.scale = (s_set.scale - 1 + dir + 4) % 4 + 1;
        xbox_FramebufferSetWindowScale(s_hwnd, s_set.scale);
        break;
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
    switch (s_screen) {
    case SCR_MAIN:
        if (k_main[i].game < 0)
            sound(2);
        if (k_main[i].game >= 0) {
            fprintf(stderr, "[PCMENU] %s: hand to the game's button %d\n", k_main[i].label, k_main[i].game);
            s_sel_main = i;
            InterlockedExchange(&g_avp_menu_req, k_main[i].game);
            s_handoff = 1;
            s_handoff_t = GetTickCount();
        } else {
            fprintf(stderr, "[PCMENU] %s\n", k_main[i].label);
            go(k_main[i].screen);
        }
        break;
    case SCR_COOP: case SCR_MP:
        sound(2);
        back();
        break;
    case SCR_SETTINGS:
        sound(2);
        if (i == ST_BACK) back();
        else if (i == ST_NAME) { if (s_editing) { s_editing = 0; settings_save(); } else s_editing = 1; }
        else change(i, +1);
        break;
    case SCR_QUIT:
        sound(2);
        if (i == 0) {
            fprintf(stderr, "[PCMENU] quit\n");
            settings_save();
            PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
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
    return s_screen == SCR_SETTINGS && (i == ST_WIDE || i == ST_KEYS);
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

    s_hwnd = h;
    if (m == WM_KEYDOWN && w == VK_F10 && !(l & (1 << 30))) {
        if (s_forced && s_screen == SCR_SETTINGS) {
            settings_save();
            s_forced = 0;
            if (g_avp_mainmenu) go(SCR_MAIN);
        } else if (!g_avp_pcmenu_up || s_screen != SCR_SETTINGS) {
            s_forced = 1;
            go(SCR_SETTINGS);
        }
        return 1;
    }
    if (!g_avp_pcmenu_up)
        return 0;

    if (m == WM_KEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP)
        return m == WM_KEYUP ? 0 : 1;
    if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_SYSCHAR)
        return 0;
    layout();

    if (m == WM_CHAR) {
        if (s_editing && w >= 32 && w < 127) {
            size_t n = strlen(s_set.name);
            if (n < 16) { s_set.name[n] = (char)w; s_set.name[n + 1] = 0; }
        }
        return 1;
    }
    if (m == WM_KEYDOWN) {
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
        case VK_UP: case 'W': move_sel(-1); break;
        case VK_DOWN: case 'S': move_sel(+1); break;
        case VK_LEFT: case 'A':
            if (s_screen == SCR_SETTINGS && s_sel != ST_NAME) change(s_sel, -1);
            else if (s_screen == SCR_QUIT) s_sel = 0;
            break;
        case VK_RIGHT: case 'D':
            if (s_screen == SCR_SETTINGS && s_sel != ST_NAME) change(s_sel, +1);
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
            if (i >= 0 && !disabled(i) && !s_editing && i != s_sel) {
                s_sel = i;
                sound(1);
            }
        }
        return 1;
    }
    if (m == WM_LBUTTONDOWN) {
        xbox_FramebufferClientToFrame(h, (short)LOWORD(l), (short)HIWORD(l), &fx, &fy);
        i = box_at(fx, fy);
        if (s_editing && i != ST_NAME) { s_editing = 0; settings_save(); }
        if (i >= 0 && !disabled(i)) {
            s_sel = i;
            if (s_screen == SCR_SETTINGS && i != ST_BACK && i != ST_NAME)
                change(i, fx < 300 ? -1 : +1);
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

extern void avp_overlay_draw(uint32_t *rgb, int w, int h);
static int s_menu_off;

static void draw_hook(uint32_t *rgb, int w, int h)
{
    int up;
    avp_overlay_draw(rgb, w, h);
    if (s_menu_off)
        return;
    up = visible();
    if (up != s_was_up) {
        if (up) {
            xbox_InputKeysClear();
            if (!s_forced) { s_screen = SCR_MAIN; s_sel = s_sel_main; s_editing = 0; }
            fprintf(stderr, "[PCMENU] up (%s)\n", s_forced ? "F10" : "main menu");
        } else {
            fprintf(stderr, "[PCMENU] down\n");
        }
        s_was_up = up;
    }
    InterlockedExchange(&g_avp_pcmenu_up, up);
    if (!up || !dib_ensure(w, h))
        return;
    if (s_hwnd && s_set.fullscreen != xbox_FramebufferIsFullscreen())
        s_set.fullscreen = xbox_FramebufferIsFullscreen();
    memcpy(s_px, rgb, (size_t)w * h * 4);
    draw_screen();
    text_flush();
    memcpy(rgb, s_px, (size_t)w * h * 4);
}

void avp_pcmenu_init(void)
{
    const char *e = getenv("AVP_PCMENU");
    xbox_FramebufferSetDrawHook(draw_hook);
    if (e && e[0] == '0') {
        s_menu_off = 1;
        fprintf(stderr, "[PCMENU] off\n");
        return;
    }
    xbox_FramebufferSetInputHook(input_hook);
}

