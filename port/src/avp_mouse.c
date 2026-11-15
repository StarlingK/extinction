#include "avp_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp, g_ebp, g_eflags;
extern RECOMP_TLS int g_fp_top;
extern ptrdiff_t g_xbox_mem_offset;

#include "avp_keys.h"
#include "avp_editor.h"
#include "avp_layer.h"

typedef void (*avp_guest_fn)(void);
extern avp_guest_fn recomp_lookup(uint32_t xbox_va);

extern int  xbox_FramebufferMouse(int *fx, int *fy);
extern int  xbox_FramebufferLayerReady(void);
extern int  avp_wide_inset(void);
extern int  avp_wide_width(void);
extern void avp_wide_frame(void);
extern void avp_wide_narrow(void);
extern int  xbox_FramebufferMouseWheel(void);
extern void xbox_FramebufferHideCursor(int hide);
extern void xbox_FramebufferSetBox(int on, int x0, int y0, int x1, int y1);
extern void xbox_FramebufferSetBoxColor(uint32_t rgb);
extern void xbox_FramebufferSetCursor(int on, int x, int y, uint32_t rgb);
extern int  xbox_FramebufferKeyDown(int vk);
extern void xbox_InputKeySet(int vk, int down);
extern uint64_t avp_coop_now_ms(void);
extern void avp_coop_mission_start(void);
extern void avp_coop_mission_end(void);
extern void avp_skirmish_mission_start(void);
extern void avp_skirmish_tick(void);
extern void avp_skirmish_draw(uint32_t *rgb, int w, int h);
extern void avp_skirmish_capture(const uint32_t *rgb, int w, int h);
extern int avp_skirmish_armed(void);
extern void avp_skirmish_disarm(void);
extern volatile LONG g_avp_pcmenu_up;
extern int avp_coop_give_selection(void);
extern int avp_coop_role(void);
extern int avp_input_controller(void);

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define G8(a)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define AVP_INPUT_MGR    0x002F6824u
#define AVP_CRC_TABLE    0x0026AFA0u
#define AVP_OBJ_TABLE    0x00289E40u
#define AVP_VIEWPORTS    0x002F6CC4u
#define AVP_PTR_SCALE    0x00252134u
#define AVP_IMAGE_LO     0x00010000u
#define AVP_IMAGE_HI     0x0031B000u

#define FRAME_W 640
#define FRAME_H 480
#define EDGE    6

#define AVP_FOCUS_DPAD   0x000E8340u
#define AVP_REGIONS      0x002FFDF0u
#define AVP_REGIONS_END  0x002CC290u
#define AVP_DYNCAST      0x001474DAu
#define AVP_TD_GOBJECT   0x0027E09Cu
#define AVP_TD_MENUITEM  0x0027EF80u
#define AVP_TD_LISTBOX   0x0027EEF4u
#define AVP_POINTER      0x002F6AECu
#define AVP_PTR_MODE     0x22Cu
#define AVP_PTR_SETMODE  0x0002BAB0u
#define AVP_CAM_PAN      0x00029EF0u
#define AVP_CAM_MODE_A   0x002F68ECu
#define AVP_CAM_MODE_B   0x002F6AF8u
#define AVP_MINIMAP      0x002F6990u
#define AVP_EDGE_CALC    0x00284839u
#define AVP_EDGE_X_LO    0x0030954Cu
#define AVP_EDGE_X_HI    0x00309554u
#define AVP_EDGE_Y_LO    0x00309550u
#define AVP_EDGE_Y_HI    0x00309558u
#define AVP_MINIMAP_RELEASE 0x000B9DA0u
#define AVP_FOCUSED      0x27Cu

enum {
    C_MPOS, C_MACT, C_PPOS, C_B1ON, C_B1OFF, C_B2ON, C_B2OFF, C_ZOOM, C_GPMOVE,
    C_FSEL, C_FBACK, C_GPZOOM, C_N
};
static const char *const k_names[C_N] = {
    "MenuPointerUpdatePosition", "MenuPointerAction", "PointerUpdatePosition",
    "PointerMouseButton1Activate", "PointerMouseButton1DeActivate",
    "PointerMouseButton2Activate", "PointerMouseButton2DeActivate",
    "CameraZoom", "CameraGPMove", "FrontEndSelect", "FrontEndBack", "CameraGPZoom",
};
static uint32_t k_crc[C_N];

typedef struct { uint32_t owner, handler, mask; } Cmd;

static int s_on = -1, s_trace, s_script_wait;

static uint32_t crc_name(const char *s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++)
        c = (c >> 8) ^ G32(AVP_CRC_TABLE + ((c ^ (uint8_t)*s) & 0xFFu) * 4u);
    return c;
}

#define NAME_SLOTS 32768u
static struct { uint32_t crc, addr; } *s_names;

static void names_build(void)
{
    uint32_t a = AVP_IMAGE_LO, start = 0;
    s_names = calloc(NAME_SLOTS, sizeof *s_names);
    if (!s_names)
        return;
    for (; a < AVP_IMAGE_HI; a++) {
        uint8_t ch = G8(a);
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
              || (ch >= '0' && ch <= '9') || ch == '_';
        if (ok) {
            if (!start)
                start = a;
            continue;
        }
        if (start && ch == 0 && a - start >= 4 && a - start <= 48) {
            char buf[52];
            uint32_t n = a - start, i, crc, slot;
            for (i = 0; i < n; i++)
                buf[i] = (char)G8(start + i);
            buf[n] = 0;
            crc = crc_name(buf);
            for (slot = crc & (NAME_SLOTS - 1), i = 0; i < 64; i++, slot = (slot + 1) & (NAME_SLOTS - 1))
                if (!s_names[slot].crc) {
                    s_names[slot].crc = crc;
                    s_names[slot].addr = start;
                    break;
                } else if (s_names[slot].crc == crc) {
                    break;
                }
        }
        start = 0;
    }
}

static const char *name_of(uint32_t crc, char *buf, size_t len)
{
    uint32_t slot, i;
    if (s_names)
        for (slot = crc & (NAME_SLOTS - 1), i = 0; i < 64 && s_names[slot].crc; i++,
             slot = (slot + 1) & (NAME_SLOTS - 1))
            if (s_names[slot].crc == crc) {
                size_t k;
                for (k = 0; k + 1 < len && G8(s_names[slot].addr + k); k++)
                    buf[k] = (char)G8(s_names[slot].addr + k);
                buf[k] = 0;
                return buf;
            }
    snprintf(buf, len, "crc %08X", crc);
    return buf;
}

static uint32_t s_call_edi;
static uint32_t s_call_eax;
static uint32_t s_call_esi;
static uint32_t s_call_ebx;

static uint32_t gcall(uint32_t va, uint32_t self, const uint32_t *args, int n)
{
    avp_guest_fn fn = va ? recomp_lookup(va) : NULL;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi;
    uint32_t sebp = g_seh_ebp, ebp = g_ebp, fl = g_eflags, ret;
    int top = g_fp_top, i;

    if (!fn)
        return 0;
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        G32(g_esp) = args[i];
    }
    g_esp -= 4;
    G32(g_esp) = 0;
    g_ecx = self;

    g_esi = self;
    if (s_call_esi) {
        g_esi = s_call_esi;
        s_call_esi = 0;
    }
    if (s_call_ebx) {
        g_ebx = s_call_ebx;
        s_call_ebx = 0;
    }
    if (s_call_edi) {
        g_edi = s_call_edi;
        s_call_edi = 0;
    }
    if (s_call_eax) {
        g_eax = s_call_eax;
        s_call_eax = 0;
    }
    fn();
    ret = g_eax;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
    g_ebx = ebx; g_esi = esi; g_edi = edi;
    g_seh_ebp = sebp; g_ebp = ebp; g_eflags = fl; g_fp_top = top;
    return ret;
}

static uint32_t scratch(uint32_t bytes)
{
    return (g_esp - 0x100u - bytes) & ~15u;
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

typedef struct { uint32_t ms; int x, y, b, w; } Step;
static Step *s_script;
static int s_script_n, s_script_i;

static void script_parse(const char *s)
{
    int cap = 0;
    while (s && *s) {
        Step st = {0, 0, 0, 0, 0};
        char *end;
        st.ms = (uint32_t)strtoul(s, &end, 10);
        if (*end != ':')
            break;
        if (sscanf(end + 1, "%d,%d,%d,%d", &st.x, &st.y, &st.b, &st.w) < 3)
            break;
        if (s_script_n == cap) {
            cap = cap ? cap * 2 : 16;
            s_script = realloc(s_script, (size_t)cap * sizeof *s_script);
            if (!s_script)
                return;
        }
        s_script[s_script_n++] = st;
        s = strchr(end, ';');
        if (s)
            s++;
    }
}

static void find_cmds(uint32_t mgr, uint32_t mode, Cmd cmd[C_N])
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    int c;

    memset(cmd, 0, sizeof(Cmd) * C_N);
    if (!arr || n > 4096)
        return;
    for (i = 0; i < n; i++) {
        uint32_t e = arr + i * 16, mask = G32(e), trig = G32(e + 4), crc, obj;
        if (!(mask & mode) || !trig)
            continue;
        crc = G32(trig + 8);
        for (c = 0; c < C_N; c++)
            if (crc == k_crc[c] && !cmd[c].owner) {
                obj = G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
                if (obj) {
                    cmd[c].owner = obj;
                    cmd[c].handler = G32(e + 12);
                    cmd[c].mask = mask;
                }
            }
    }
}

#define AVP_BTN_PRESS 0x00089800u

static uint32_t press_handler(uint32_t mgr, uint32_t mode, uint32_t owner)
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16;
        if ((G32(e) & mode) && G32(e + 12) == AVP_BTN_PRESS
            && G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u) == owner)
            return AVP_BTN_PRESS;
    }
    return 0;
}

static int bound_release(uint32_t mgr, uint32_t mode, uint32_t obj)
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16;
        if ((G32(e) & mode) && G32(e + 12) == 0x00091180u
            && G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u) == obj)
            return 1;
    }
    return 0;
}

static const char *class_name(uint32_t obj, char *buf, size_t len);

static uint32_t select_button(uint32_t mgr, uint32_t mode)
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    char cb[48];
    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16, obj;
        size_t len;
        if (!(G32(e) & mode) || G32(e + 12) != 0x00091180u)
            continue;
        obj = G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
        class_name(obj, cb, sizeof cb);
        len = strlen(cb);
        if (len >= 12 && !strcmp(cb + len - 12, "SelectButton"))
            return obj;
    }
    return 0;
}

static int obj_rect(uint32_t obj, int *l, int *t, int *r, int *b)
{
    uint32_t end = G32(AVP_REGIONS_END), e;
    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20)
        if (G32(e) && G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u) == obj) {
            *l = (int)G32(e + 4); *t = (int)G32(e + 8);
            *r = (int)G32(e + 12); *b = (int)G32(e + 16);
            return 1;
        }
    return 0;
}

#define NAV_HOLD 6
#define NAV_GAP  6
#define NAV_MAX  10

static int s_rclick_b, s_rclick_armed;

static int menu_key_action(int c)
{
    switch (c >= 'a' && c <= 'z' ? c - 32 : c) {
    case 'Z': return AVP_ACT_A;
    case 'X': return AVP_ACT_B;
    case 'C': return AVP_ACT_X;
    case 'V': return AVP_ACT_Y;
    case 'E': return AVP_ACT_BLACK;
    case 'Q': return AVP_ACT_WHITE;
    case '1': return AVP_ACT_LTRIGGER;
    case '3': return AVP_ACT_RTRIGGER;
    }
    return -1;
}
static uint32_t s_nav_target;
static int s_nav_key, s_nav_hold, s_nav_gap, s_nav_steps, s_nav_select;

static void nav_start(uint32_t item)
{

    static int on = -1;
    if (on < 0) {
        const char *e = getenv("AVP_NAV_WALK");
        on = !(e && e[0] == '0');
    }
    if (!on)
        return;
    if (s_nav_target != item) {
        s_nav_target = item;
        s_nav_steps = 0;
    }
}

static void nav_key_tap(int act)
{
    if (s_nav_hold > 0)
        avp_pad_set(s_nav_key, 0);
    s_nav_key = act;
    avp_pad_set(act, 1);
    s_nav_hold = NAV_HOLD;
}

static int button_key(uint32_t mgr, uint32_t mode, uint32_t obj)
{
    static const struct { const char *name; int act; } map[] = {
        { "Right_BottomClusterButton_Press", AVP_ACT_A }, { "Right_RightClusterButton_Press", AVP_ACT_B },
        { "Right_LeftClusterButton_Press", AVP_ACT_X },   { "Right_TopClusterButton_Press", AVP_ACT_Y },
        { "RightTopShoulderButton_Press", AVP_ACT_BLACK }, { "LeftTopShoulderButton_Press", AVP_ACT_WHITE },
        { "LeftBottomShoulderButton_Press", AVP_ACT_LTRIGGER }, { "RightBottomShoulderButton_Press", AVP_ACT_RTRIGGER },
        { "Left_TopClusterButton_Press", AVP_ACT_DPAD_UP },   { "Left_BottomClusterButton_Press", AVP_ACT_DPAD_DOWN },
        { "Left_LeftClusterButton_Press", AVP_ACT_DPAD_LEFT }, { "Left_RightClusterButton_Press", AVP_ACT_DPAD_RIGHT },
    };
    static uint32_t crcs[sizeof map / sizeof map[0]];
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i, j;
    if (!crcs[0])
        for (j = 0; j < sizeof map / sizeof map[0]; j++)
            crcs[j] = crc_name(map[j].name);
    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16, trig = G32(e + 4);
        if (!(G32(e) & mode) || G32(e + 12) != AVP_BTN_PRESS || !trig
            || G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u) != obj)
            continue;
        for (j = 0; j < sizeof map / sizeof map[0]; j++)
            if (G32(trig + 8) == crcs[j])
                return map[j].act;
        if (s_trace) {
            char nb[64];
            fprintf(stderr, "[MOUSE] button %08X: press entry %s has no action\n",
                    obj, name_of(G32(trig + 8), nb, sizeof nb));
        }
    }
    return -1;
}

static int nav_direct(uint32_t fm, uint32_t mgr, uint32_t mode)
{
    static const uint32_t hs[4] = { 0x000E8340u, 0x000E8330u, 0x000E8320u, 0x000E8310u };
    uint32_t own[4] = { 0, 0, 0, 0 }, blk = mgr + 0x9C, n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    int k, steps = 0, any = 0;

    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16;
        if (!(G32(e) & mode))
            continue;
        for (k = 0; k < 4; k++)
            if (!own[k] && G32(e + 12) == hs[k]) {
                own[k] = G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
                any |= own[k] != 0;
            }
    }
    if (!any)
        return 0;
    while (steps < NAV_MAX) {
        uint32_t fh = G32(fm + AVP_FOCUSED), cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0, nfh;
        int il, it, ir, ib, cl, ct, cr, cb, dx, dy, d;
        if (cur == s_nav_target || !cur
            || !obj_rect(s_nav_target, &il, &it, &ir, &ib) || !obj_rect(cur, &cl, &ct, &cr, &cb))
            break;
        dx = (il + ir) / 2 - (cl + cr) / 2;
        dy = (it + ib) / 2 - (ct + cb) / 2;
        d = abs(dy) >= abs(dx) ? (dy < 0 ? 0 : 1) : (dx < 0 ? 2 : 3);
        if (!own[d]) {
            if (!steps)
                return 0;
            break;
        }
        gcall(hs[d], own[d], &blk, 1);
        steps++;
        nfh = G32(fm + AVP_FOCUSED);
        if (nfh == fh)
            break;
    }
    if (s_trace) {
        uint32_t fh = G32(fm + AVP_FOCUSED), cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
        fprintf(stderr, "[MOUSE] focus walk: %d steps direct\n", steps);
        if (cur != s_nav_target)
            fprintf(stderr, "[MOUSE] focus walk gave up at %08X (wanted %08X)\n", cur, s_nav_target);
    }
    s_nav_target = 0;
    return 1;
}

static int nav_run(uint32_t fm, uint32_t mgr, uint32_t mode)
{
    uint32_t fh, cur;
    int il, it, ir, ib, cl, ct, cr, cb, dx, dy;

    if (s_nav_hold > 0) {
        if (--s_nav_hold == 0) {
            avp_pad_set(s_nav_key, 0);
            s_nav_gap = NAV_GAP;
        }
        return 0;
    }
    if (s_nav_gap > 0) {
        s_nav_gap--;
        return 0;
    }
    if (!s_nav_target)
        return 1;
    if (nav_direct(fm, mgr, mode))
        return 1;
    fh = G32(fm + AVP_FOCUSED);
    cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
    if (cur == s_nav_target || !cur || s_nav_steps >= NAV_MAX
        || !obj_rect(s_nav_target, &il, &it, &ir, &ib) || !obj_rect(cur, &cl, &ct, &cr, &cb)) {
        if (s_trace && cur != s_nav_target)
            fprintf(stderr, "[MOUSE] focus walk gave up at %08X (wanted %08X)\n", cur, s_nav_target);
        s_nav_target = 0;
        return 1;
    }
    dx = (il + ir) / 2 - (cl + cr) / 2;
    dy = (it + ib) / 2 - (ct + cb) / 2;
    s_nav_key = abs(dy) >= abs(dx) ? (dy < 0 ? AVP_ACT_DPAD_UP : AVP_ACT_DPAD_DOWN)
                                   : (dx < 0 ? AVP_ACT_DPAD_LEFT : AVP_ACT_DPAD_RIGHT);
    avp_pad_set(s_nav_key, 1);
    s_nav_hold = NAV_HOLD;
    s_nav_steps++;
    if (s_trace)
        fprintf(stderr, "[MOUSE] focus walk step %d: action %d (focus %08X, item +334 %08X, want %08X)\n",
                s_nav_steps, s_nav_key, cur, G32(fm + 0x334), s_nav_target);
    return 0;
}

static uint32_t focus_manager(uint32_t mgr, uint32_t mode)
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    for (i = 0; arr && i < n && i < 4096; i++) {
        uint32_t e = arr + i * 16;
        if ((G32(e) & mode) && G32(e + 12) == AVP_FOCUS_DPAD)
            return G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
    }
    return 0;
}

static uint32_t menu_item_at(float x, float y, int *count)
{
    uint32_t end = G32(AVP_REGIONS_END), e, found = 0;
    if (count)
        *count = 0;
    if (end < AVP_REGIONS || end > AVP_REGIONS + 20u * 8192u)
        return 0;
    for (e = AVP_REGIONS; e + 20 <= end; e += 20) {
        float l = (float)(int32_t)G32(e + 4), t = (float)(int32_t)G32(e + 8);
        float r = (float)(int32_t)G32(e + 12), b = (float)(int32_t)G32(e + 16);
        uint32_t obj, a[5];
        if (!G32(e) || (!count && !(x > l && x < r && y > t && y < b)))
            continue;
        obj = G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u);
        if (!obj)
            continue;
        a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_MENUITEM; a[4] = 0;
        if (gcall(AVP_DYNCAST, 0, a, 5)) {
            if (!count)
                return obj;
            if (obj != found)
                (*count)++;
            found = obj;
        }
    }
    if (count || found)
        return found;

    {
        enum { ROW_REACH = 240 };
        float best = 1e9f;
        for (e = AVP_REGIONS; e + 20 <= end; e += 20) {
            float l = (float)(int32_t)G32(e + 4), t = (float)(int32_t)G32(e + 8);
            float r = (float)(int32_t)G32(e + 12), b = (float)(int32_t)G32(e + 16);
            uint32_t obj, a[5];
            if (!G32(e) || !(y > t && y < b) || x < r || x - r > best || x - r > ROW_REACH)
                continue;
            if (r - l > 200.0f)
                continue;
            obj = G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u);
            if (!obj)
                continue;
            a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_MENUITEM; a[4] = 0;
            if (gcall(AVP_DYNCAST, 0, a, 5)) {
                best = x - r;
                found = obj;
            }
        }
    }
    return found;
}

static const char *class_name(uint32_t obj, char *buf, size_t len)
{
    uint32_t vt, col, td, k;
    buf[0] = 0;
    if (!obj || obj < AVP_IMAGE_LO)
        return buf;
    vt = G32(obj);
    if (vt < AVP_IMAGE_LO + 4 || vt >= AVP_IMAGE_HI)
        return buf;
    col = G32(vt - 4);
    if (col < AVP_IMAGE_LO || col >= AVP_IMAGE_HI)
        return buf;
    td = G32(col + 12);
    if (td < AVP_IMAGE_LO || td >= AVP_IMAGE_HI || G8(td + 8) != '.')
        return buf;
    for (k = 0; k + 1 < len && G8(td + 12 + k) && G8(td + 12 + k) != '@'; k++)
        buf[k] = (char)G8(td + 12 + k);
    buf[k] = 0;
    return buf;
}

static void trace_list(uint32_t mgr, uint32_t mode)
{
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i;
    char nb[64];
    fprintf(stderr, "[MOUSE] input mode %08X, %u commands:\n", mode, n);
    for (i = 0; arr && i < n && i < 400; i++) {
        uint32_t e = arr + i * 16, mask = G32(e), trig = G32(e + 4);
        if (!trig)
            continue;
        {
            char cb[48];
            uint32_t obj = G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
            fprintf(stderr, "[MOUSE]   %c %-36s mask %08X owner %08X %-24s handler %08X\n",
                    (mask & mode) ? '*' : ' ', name_of(G32(trig + 8), nb, sizeof nb),
                    mask, G32(e + 8), class_name(obj, cb, sizeof cb), G32(e + 12));
        }
    }
    {

        uint32_t end = G32(AVP_REGIONS_END), e;
        for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20) {
            uint32_t obj = G32(e) ? G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u) : 0;
            fprintf(stderr, "[MOUSE]   region %08X obj %08X vt %08X %d,%d-%d,%d\n",
                    G32(e), obj, obj ? G32(obj) : 0,
                    (int)G32(e + 4), (int)G32(e + 8), (int)G32(e + 12), (int)G32(e + 16));
        }
    }
}

static int pointer_free(uint32_t ptr)
{
    uint32_t a[2] = { 2, 0 }, mode;
    static int pad = -1;
    if (pad < 0)
        pad = getenv("AVP_PTR_PAD") != NULL;
    if (!ptr || pad)
        return 0;
    mode = G32(ptr + AVP_PTR_MODE);
    if (mode == 2)
        return 0;
    if (s_trace && mode != 1)
        fprintf(stderr, "[MOUSE] pointer mode %u -> 2\n", mode);
    s_call_edi = ptr;
    gcall(AVP_PTR_SETMODE, 0, a, 2);
    return 1;
}

#define AVP_MARKER_OWNER  0x002F6AECu
#define AVP_MARKER_MAKE   0x0002EFF0u
#define AVP_PTR_GROUND    0x1A8u

static uint32_t race_color(uint32_t ptr);
static int s_race;

#define MARKER_LIFE_MS 900
#define MARKER_SLOTS   8

static struct {
    volatile LONG on;
    int fx, fy;
    float camx, camy;
    ULONGLONG t0;
    uint32_t rgb;
} s_marks[MARKER_SLOTS];
static int s_mark_next;

static int camera_focus(float *x, float *y)
{
    uint32_t cam = G32(0x002F6AF4u);
    if (!cam)
        return 0;
    *x = GF(cam + 0x10C);
    *y = GF(cam + 0x110);
    return fabsf(*x) < 1e7f && fabsf(*y) < 1e7f;
}

static void marker_add(int fx, int fy, uint32_t rgb)
{
    int i = s_mark_next;
    float cx = 0.0f, cy = 0.0f;
    camera_focus(&cx, &cy);
    s_mark_next = (s_mark_next + 1) % MARKER_SLOTS;
    InterlockedExchange(&s_marks[i].on, 0);
    s_marks[i].fx = fx;
    s_marks[i].fy = fy;
    s_marks[i].camx = cx;
    s_marks[i].camy = cy;
    s_marks[i].rgb = rgb;
    s_marks[i].t0 = GetTickCount64();
    InterlockedExchange(&s_marks[i].on, 1);
}

static uint32_t blend(uint32_t dst, uint32_t src, float a)
{
    int k;
    uint32_t out = 0;
    if (a <= 0.0f) return dst;
    if (a > 1.0f) a = 1.0f;
    for (k = 0; k < 24; k += 8) {
        float d = (float)((dst >> k) & 0xFF), s = (float)((src >> k) & 0xFF);
        out |= (uint32_t)(d + (s - d) * a + 0.5f) << k;
    }
    return out;
}

static uint32_t *ov_buf;
static int ov_bw, ov_bh, ov_hi, ov_ox, ov_oy;
static float ov_S = 1.0f;
static int ov_bx0, ov_by0, ov_bx1, ov_by1;

static void ov_reset_box(void)
{
    ov_bx0 = ov_by0 = 0x7FFFFFFF;
    ov_bx1 = ov_by1 = -0x7FFFFFFF;
}

static void ov_touch(int x0, int y0, int x1, int y1)
{
    if (x0 < ov_bx0) ov_bx0 = x0;
    if (y0 < ov_by0) ov_by0 = y0;
    if (x1 > ov_bx1) ov_bx1 = x1;
    if (y1 > ov_by1) ov_by1 = y1;
}

static uint32_t ov_mix(uint32_t d, uint32_t c, float a)
{
    int a8, ia;
    if (!ov_hi)
        return blend(d, c, a);
    if (a <= 0.0f)
        return d;
    a8 = a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f);
    ia = 255 - a8;
    return ((uint32_t)((255 * a8 + (int)(d >> 24) * ia + 127) / 255) << 24)
         | ((uint32_t)((((int)((c >> 16) & 255)) * a8 + (int)((d >> 16) & 255) * ia + 127) / 255) << 16)
         | ((uint32_t)((((int)((c >> 8) & 255)) * a8 + (int)((d >> 8) & 255) * ia + 127) / 255) << 8)
         | (uint32_t)((((int)(c & 255)) * a8 + (int)(d & 255) * ia + 127) / 255);
}

static void ov_put(int x, int y, uint32_t col, float a)
{
    int X0, X1, Y0, Y1, xx, yy;
    if (!ov_hi) {
        uint32_t *p;
        if (x < 0 || y < 0 || x >= ov_bw || y >= ov_bh)
            return;
        p = &ov_buf[(size_t)y * ov_bw + x];
        *p = a >= 1.0f ? col : blend(*p, col, a);
        return;
    }
    X0 = ov_ox + (int)floorf((float)x * ov_S + 0.5f);
    X1 = ov_ox + (int)floorf((float)(x + 1) * ov_S + 0.5f);
    Y0 = ov_oy + (int)floorf((float)y * ov_S + 0.5f);
    Y1 = ov_oy + (int)floorf((float)(y + 1) * ov_S + 0.5f);
    if (X1 <= X0) X1 = X0 + 1;
    if (Y1 <= Y0) Y1 = Y0 + 1;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > ov_bw) X1 = ov_bw;
    if (Y1 > ov_bh) Y1 = ov_bh;
    if (X1 <= X0 || Y1 <= Y0)
        return;
    for (yy = Y0; yy < Y1; yy++)
        for (xx = X0; xx < X1; xx++) {
            uint32_t *p = &ov_buf[(size_t)yy * ov_bw + xx];
            *p = ov_mix(*p, col & 0x00FFFFFFu, a);
        }
    ov_touch(X0, Y0, X1, Y1);
}

static float band(float d, float lo, float hi)
{
    float a = d - lo + 0.75f, b = hi - d + 0.75f;
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    b = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    return a < b ? a : b;
}

extern int avp_coop_marked(uint32_t handle);

#define CM_MAX 96
typedef struct { uint32_t h; float x, y; ULONGLONG seen; int kind; } CoopMark;
static CoopMark s_cm[2][CM_MAX];
static int s_cm_n[2];
static volatile LONG s_cm_cur;

static void coop_marks_snap(void)
{
    uint32_t end = G32(AVP_REGIONS_END), e;
    int src = (int)s_cm_cur, dst = src ^ 1, n = 0, i, j;
    ULONGLONG now = GetTickCount64();
    CoopMark *o = s_cm[src], *d = s_cm[dst];
    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end && n < CM_MAX; e += 20) {
        uint32_t h = G32(e);
        float cx, cy;
        int kind = h ? avp_coop_marked(h) : 0;
        if (!kind)
            continue;
        cx = (float)((int)G32(e + 4) + (int)G32(e + 12)) * 0.5f;
        cy = (float)(int)G32(e + 8) - 7.0f;
        for (j = 0; j < n && d[j].h != h; j++)
            ;
        if (j < n)
            continue;
        for (i = 0; i < s_cm_n[src] && o[i].h != h; i++)
            ;
        if (i < s_cm_n[src] && now - o[i].seen < 400) {
            cx = o[i].x + (cx - o[i].x) * 0.5f;
            cy = o[i].y + (cy - o[i].y) * 0.5f;
        }
        d[n].h = h; d[n].x = cx; d[n].y = cy; d[n].seen = now; d[n].kind = kind;
        n++;
    }
    for (i = 0; i < s_cm_n[src] && n < CM_MAX; i++) {
        if (now - o[i].seen >= 250)
            continue;
        for (j = 0; j < n && d[j].h != o[i].h; j++)
            ;
        if (j == n && (o[i].kind = avp_coop_marked(o[i].h)) != 0)
            d[n++] = o[i];
    }
    s_cm_n[dst] = n;
    InterlockedExchange(&s_cm_cur, dst);
}

static void coop_marks(uint32_t *rgb, int w, int h)
{
    int cur = (int)s_cm_cur, n = s_cm_n[cur], i, dx, dy;
    const CoopMark *m = s_cm[cur];
    (void)rgb;
    for (i = 0; i < n; i++) {
        int cx = (int)floorf(m[i].x + 0.5f), cy = (int)floorf(m[i].y + 0.5f);
        if (m[i].kind == 2) {
            for (dy = -1; dy <= 3; dy++)
                for (dx = -3; dx <= 3; dx++) {
                    int ax = dx < 0 ? -dx : dx, x = cx + dx, y = cy + dy, in;
                    if (x < 0 || y < 0 || x >= w || y >= h)
                        continue;
                    in = dy >= 0 && dy <= 2 && ax <= 2 - dy;
                    if (in)
                        ov_put(x, y, dy == 0 ? 0x0090FF90u : 0x0030C040u, 1.0f);
                    else if (dy >= -1 && dy <= 3 && ax <= 3 - (dy < 0 ? 0 : dy))
                        ov_put(x, y, 0x00081408u, 0.85f);
                }
            continue;
        }
        for (dy = -1; dy <= 4; dy++)
            for (dx = -5; dx <= 5; dx++) {
                int ax = dx < 0 ? -dx : dx, x = cx + dx, y = cy + dy, in, edge;
                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                in = dy >= 0 && dy <= 3 && ax <= 4 - dy;
                edge = !in && dy >= -1 && dy <= 4 && ax <= 5 - (dy < 0 ? 0 : dy);
                if (in)
                    ov_put(x, y, dy == 0 ? 0x00B8F4FFu : 0x0046C8F0u, 1.0f);
                else if (edge)
                    ov_put(x, y, 0x00081420u, 0.85f);
            }
    }
}

static int s_in_mission;
static void group_bar_draw(uint32_t *rgb, int w, int h);
static void marks_draw(int w, int h);
static void coop_banner_draw(int w, int h);

static void ov_legacy(uint32_t *rgb, int w, int h)
{
    ov_buf = rgb;
    ov_bw = w;
    ov_bh = h;
    ov_hi = 0;
    ov_S = 1.0f;
    ov_ox = ov_oy = 0;
}

void avp_overlay_draw(uint32_t *rgb, int w, int h)
{
    static ULONGLONG ptr_since;
    int hi = xbox_FramebufferLayerReady();
    if (!s_in_mission || g_avp_pcmenu_up)
        ptr_since = 0;
    else if (!ptr_since)
        ptr_since = GetTickCount64();
    else if (GetTickCount64() - ptr_since >= 5000)
        avp_skirmish_capture(rgb, w, h);
    ov_legacy(rgb, w, h);
    if (!hi) {
        coop_marks(rgb, w, h);
        group_bar_draw(rgb, w, h);
    }
    avp_skirmish_draw(rgb, w, h);
    avp_editor_draw(rgb, w, h);
    if (!hi) {
        ov_legacy(rgb, w, h);
        marks_draw(w, h);
        coop_banner_draw(w, h);
    }
}

void avp_overlay_layer(XboxFbLayer *L)
{
    static int pv[4];
    static uint32_t *ppx;
    int x0, y0, x1, y1, y;
    if (L->full || L->px != ppx)
        pv[0] = pv[1] = pv[2] = pv[3] = 0;
    x0 = pv[0] < 0 ? 0 : pv[0]; y0 = pv[1] < 0 ? 0 : pv[1];
    x1 = pv[2] > L->w ? L->w : pv[2]; y1 = pv[3] > L->h ? L->h : pv[3];
    for (y = y0; y < y1 && x1 > x0; y++)
        memset(L->px + (size_t)y * (size_t)L->w + x0, 0, (size_t)(x1 - x0) * 4);
    ov_buf = L->px;
    ov_bw = L->w;
    ov_bh = L->h;
    ov_hi = 1;
    ov_S = (float)L->vh / 480.0f;
    ov_ox = L->vx;
    ov_oy = L->vy;
    ov_reset_box();
    coop_marks(NULL, 640, 480);
    group_bar_draw(NULL, 640, 480);
    marks_draw(640, 480);
    coop_banner_draw(640, 480);
    ov_hi = 0;
    ov_S = 1.0f;
    ov_ox = ov_oy = 0;
    ov_buf = NULL;
    if (ov_bx0 < 0) ov_bx0 = 0;
    if (ov_by0 < 0) ov_by0 = 0;
    if (ov_bx1 > L->w) ov_bx1 = L->w;
    if (ov_by1 > L->h) ov_by1 = L->h;
    if (ov_bx1 > ov_bx0 && ov_by1 > ov_by0) {
        L->cx0 = ov_bx0; L->cy0 = ov_by0; L->cx1 = ov_bx1; L->cy1 = ov_by1;
        pv[0] = ov_bx0; pv[1] = ov_by0; pv[2] = ov_bx1; pv[3] = ov_by1;
    } else {
        pv[0] = pv[1] = pv[2] = pv[3] = 0;
    }
    L->dx0 = L->cx0; L->dy0 = L->cy0; L->dx1 = L->cx1; L->dy1 = L->cy1;
    if (x1 > x0 && y1 > y0) {
        if (L->dx1 <= L->dx0) {
            L->dx0 = x0; L->dy0 = y0; L->dx1 = x1; L->dy1 = y1;
        } else {
            if (x0 < L->dx0) L->dx0 = x0;
            if (y0 < L->dy0) L->dy0 = y0;
            if (x1 > L->dx1) L->dx1 = x1;
            if (y1 > L->dy1) L->dy1 = y1;
        }
    }
    ppx = L->px;
}

static void marks_draw(int w, int h)
{
    static const float kx = 33.6f, ky = 25.0f, squash = 0.72f;
    ULONGLONG now = GetTickCount64();
    float cx, cy, S = ov_S;
    int have_cam, i, lx0 = ov_hi ? ov_ox : 0, ly0 = ov_hi ? ov_oy : 0;
    int lx1 = lx0 + (int)floorf((float)w * S + 0.5f), ly1 = ly0 + (int)floorf((float)h * S + 0.5f);
    if (lx1 > ov_bw) lx1 = ov_bw;
    if (ly1 > ov_bh) ly1 = ov_bh;
    have_cam = camera_focus(&cx, &cy);
    for (i = 0; i < MARKER_SLOTS; i++) {
        float age, grow, fade, s, px, py, ro, ri;
        uint32_t inner;
        int x0, x1, y0, y1, x, y;
        if (!s_marks[i].on)
            continue;
        age = (float)(now - s_marks[i].t0);
        if (age >= MARKER_LIFE_MS) {
            InterlockedExchange(&s_marks[i].on, 0);
            continue;
        }
        grow = age < 180.0f ? age / 180.0f : 1.0f;
        fade = age < 500.0f ? 1.0f : 1.0f - (age - 500.0f) / (MARKER_LIFE_MS - 500.0f);
        s = 0.55f + 0.45f * grow;
        px = (float)s_marks[i].fx;
        py = (float)s_marks[i].fy;
        if (have_cam) {
            px -= (cx - s_marks[i].camx) * kx;
            py -= (cy - s_marks[i].camy) * ky;
        }
        px = (float)lx0 + px * S;
        py = (float)ly0 + py * S;
        ro = 15.0f * s * S;
        ri = 9.0f * s * S;
        inner = blend(s_marks[i].rgb, 0x00FFFFFFu, 0.55f);
        x0 = (int)(px - ro - 2 * S); x1 = (int)(px + ro + 2 * S);
        y0 = (int)(py - ro * squash - 2 * S); y1 = (int)(py + ro * squash + 2 * S);
        if (x0 < lx0) x0 = lx0;
        if (y0 < ly0) y0 = ly0;
        if (x1 >= lx1) x1 = lx1 - 1;
        if (y1 >= ly1) y1 = ly1 - 1;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                float dx = (float)x + 0.5f - px, dy = ((float)y + 0.5f - py) / squash;
                float d = sqrtf(dx * dx + dy * dy);
                uint32_t *p = &ov_buf[(size_t)y * ov_bw + x];
                float a;
                a = band(d, ro - 4.5f * s * S, ro);
                if (a > 0.0f) *p = ov_mix(*p, 0x00707478u, a * 0.7f * fade);
                a = band(d, ro - 0.8f * S, ro + 0.4f * S);
                if (a > 0.0f) *p = ov_mix(*p, 0x00282A2Cu, a * 0.5f * fade);
                a = band(d, ri - 2.2f * s * S, ri);
                if (a > 0.0f) *p = ov_mix(*p, inner, a * 0.95f * fade);
            }
        }
        if (ov_hi && x1 >= x0 && y1 >= y0)
            ov_touch(x0, y0, x1 + 1, y1 + 1);
    }
}

#define AVP_MENU_VT        0x00223344u
#define AVP_MENU_VT_ALIEN  0x0024CD2Cu
#define AVP_MENU_VT_MARINE 0x0023B404u
#define AVP_MENU_VT_PRED   0x002313C4u
#define AVP_MENU_ITEM_ACT  0x000BA3F0u
#define AVP_HUD_GROUP      0x002F68E8u
#define AVP_HUD_SHOWN      0x290u
#define AVP_HUD_PARKED     0x294u

static int menu_vt(uint32_t vt)
{
    return vt == AVP_MENU_VT || vt == AVP_MENU_VT_ALIEN || vt == AVP_MENU_VT_MARINE
        || vt == AVP_MENU_VT_PRED;
}

static int menu_valid(uint32_t o)
{
    uint32_t pages;
    if (o < 0x10000u || o >= 0x84000000u - 0x1000u || !menu_vt(G32(o)))
        return 0;
    pages = G32(o + 0x294);
    return pages >= 0x10000u && pages < 0x84000000u && (int32_t)G32(o + 0x2A0) > 0
        && (int32_t)G32(o + 0x2A0) < 64;
}

#define AVP_ITEM_STATE   0x10Cu
#define AVP_ITEM_ID      0x110u
#define AVP_ITEM_HOTKEY  0x118u
#define AVP_ITEM_ENABLED 0x10u

static uint32_t menu_page(uint32_t menu, int32_t state)
{
    if (!menu_valid(menu) || state < 0 || state >= (int32_t)G32(menu + 0x2A0))
        return 0;
    return G32(G32(menu + 0x294) + 4u * (uint32_t)state);
}

static int menu_score(uint32_t menu, int32_t state)
{
    uint32_t item = menu_page(menu, state);
    int score = 0;
    if (!item)
        return 0;
    if ((int32_t)G32(menu + 0x290) == state)
        score += 2;
    if (G32(item + AVP_ITEM_STATE) & AVP_ITEM_ENABLED)
        score += 4;
    return score + 1;
}

static uint32_t find_avp_menu(int32_t state, const char **how)
{
    static uint32_t cached;
    uint32_t i, best = 0, hud = G32(AVP_HUD_GROUP), direct[2];
    int best_score = 0, sc;
    direct[0] = hud ? G32(hud + AVP_HUD_SHOWN) : 0;
    direct[1] = hud ? G32(hud + AVP_HUD_PARKED) : 0;
    *how = "none";
    for (i = 0; i < 2; i++)
        if (menu_valid(direct[i]) && menu_page(direct[i], state)) {
            *how = i ? "hud parked menu (+0x294)" : "hud shown menu (+0x290)";
            cached = direct[i];
            return direct[i];
        }
    if (menu_valid(cached) && menu_score(cached, state) >= 7) {
        *how = "cached";
        return cached;
    }
    for (i = 1; i < 0x10000u; i++) {
        uint32_t o = G32(AVP_OBJ_TABLE + i * 4u);
        if (!menu_valid(o))
            continue;
        sc = menu_score(o, state);
        if (sc > best_score) {
            best_score = sc;
            best = o;
        }
    }
    if (!best && menu_valid(cached))
        best = cached;
    if (best)
        *how = "object table scan";
    cached = best;
    return best;
}

static int context_page_handled(uint32_t item)
{
    static const char *const names[] = {
        "Marine_AttackDwimButton", "Alien_AttackDwimButton", "Predator_AttackDwimButton",
        "Marine_HealChangingButton", "Marine_RepairChangingButton",
        "Marine_PickUpDwimButton", "Alien_PickUpDwimButton", "Predator_PickUpDwimButton",
        "Marine_EnterDwimButton", "Alien_EnterDwimButton", "Alien_EatDwimButton",
        "Alien_SpreadEggsDwimButton", "Alien_EmbarkCarrierDwimButton",
        "Predator_EnterChangingButton", "Predator_EnterDwimButton",
    };
    static uint32_t crcs[sizeof names / sizeof names[0]];
    size_t i;
    if (!crcs[0])
        for (i = 0; i < sizeof names / sizeof names[0]; i++)
            crcs[i] = crc_name(names[i]);
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (G32(item + AVP_ITEM_ID) == crcs[i])
            return 1;
    return 0;
}

static void move_marker(const uint32_t *pos, int fx, int fy)
{
    static const uint32_t hashes[3] = { 0x7715B5E5u, 0x55EE21EFu, 0x3C905527u };
    uint32_t p = G32(AVP_MARKER_OWNER), made = 0;
    const char *gm;
    int use_game;
    if (getenv("AVP_NO_MOVE_MARKER"))
        return;
    gm = getenv("AVP_GAME_MARKER");
    use_game = !(gm && gm[0] == '0');
    if (getenv("AVP_OVERLAY_MARKER") || !use_game)
        marker_add(fx, fy, race_color(G32(AVP_POINTER)));
    if (s_trace)
        fprintf(stderr, "[MOUSE] move marker at %d,%d\n", fx, fy);
    if (!p || !use_game)
        return;
    if (!(fabsf(u2f(pos[0])) < 1e7f && fabsf(u2f(pos[1])) < 1e7f && fabsf(u2f(pos[2])) < 1e7f))
        return;
    if (s_race < 0 || s_race >= 3)
        return;
    {
        uint32_t pv = scratch(16), a[2];
        G32(pv) = pos[0];
        G32(pv + 4) = pos[1];
        G32(pv + 8) = pos[2];
        a[0] = p;
        a[1] = pv;
        s_call_eax = hashes[s_race];
        s_call_ebx = 0;
        made = gcall(AVP_MARKER_MAKE, p, a, 2);
    }
    if (s_trace)
        fprintf(stderr, "[MOUSE] game marker at %g,%g,%g: %08X\n",
                (double)u2f(pos[0]), (double)u2f(pos[1]), (double)u2f(pos[2]), made);
}

#define AVP_SELECT_UNIT   0x000F0070u
#define AVP_GAME_TIME     0x002F689Cu

#define AVP_TD_MARINE_PTR   0x002847A8u
#define AVP_TD_PREDATOR_PTR 0x002847C8u
#define AVP_TD_ALIEN_PTR    0x0028481Cu

static int s_race = -1;

static uint32_t race_color(uint32_t ptr)
{
    static uint32_t last_ptr, last_color = 0x0060FF60u;
    static const struct { uint32_t td, rgb; const char *name; } races[] = {
        { AVP_TD_MARINE_PTR,   0x003C8CFFu, "marine" },
        { AVP_TD_PREDATOR_PTR, 0x00FF8C1Eu, "predator" },
        { AVP_TD_ALIEN_PTR,    0x0060FF60u, "alien" },
    };
    uint32_t vt, col, td = 0;
    int i;

    if (!ptr || ptr == last_ptr)
        return last_color;
    s_race = -1;
    last_ptr = ptr;

    vt = G32(ptr);
    col = (vt >= AVP_IMAGE_LO + 4 && vt < AVP_IMAGE_HI) ? G32(vt - 4) : 0;
    if (col >= AVP_IMAGE_LO && col < AVP_IMAGE_HI)
        td = G32(col + 12);
    for (i = 0; i < 3; i++) {
        if (td == races[i].td) {
            last_color = races[i].rgb;
            s_race = i;
            if (s_trace)
                fprintf(stderr, "[MOUSE] pointer %08X is the %s pointer\n", ptr, races[i].name);
            return last_color;
        }
    }
    if (s_trace)
        fprintf(stderr, "[MOUSE] pointer %08X: no race pointer class matched"
                        " (vt %08X, type descriptor %08X)\n", ptr, vt, td);
    return last_color;
}

static void list_free(uint32_t list)
{
    uint32_t data = G32(list);
    if (data) {
        uint32_t blk = G32(data - 4), pool = G32(blk + 8);
        G32(blk + 8) = 0;
        if (pool && blk < G32(pool))
            G32(pool) = blk;
    }
}

#define AVP_SEND_COMMAND 0x00032AA0u

uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax)
{
    if (eax) s_call_eax = eax;
    return gcall(va, self, args, n);
}

uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n)
{
    avp_guest_fn fn = va ? recomp_lookup(va) : NULL;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi;
    uint32_t sebp = g_seh_ebp, ebp = g_ebp, fl = g_eflags, ret;
    int top = g_fp_top, i;

    if (!fn)
        return 0;
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        G32(g_esp) = args[i];
    }
    g_esp -= 4;
    G32(g_esp) = 0;
    g_eax = regs[0];
    g_ecx = regs[1];
    g_edx = regs[2];
    g_ebx = regs[3];
    g_esi = regs[4];
    g_edi = regs[5];
    fn();
    ret = g_eax;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
    g_ebx = ebx; g_esi = esi; g_edi = edi;
    g_seh_ebp = sebp; g_ebp = ebp; g_eflags = fl; g_fp_top = top;
    return ret;
}

uint32_t avp_guest_alloca(uint32_t bytes)
{
    g_esp = (g_esp - (bytes + 64u)) & ~15u;
    return g_esp + 16;
}

uint32_t avp_guest_esp(void)
{
    return g_esp;
}

void avp_guest_set_esp(uint32_t v)
{
    g_esp = v;
}

#define AVP_QUEUE_ORDER  0x00079400u

void avp_run_order(int route, uint32_t handle, const uint8_t *msg, uint32_t len)
{
    uint32_t obj = G32(AVP_OBJ_TABLE + (handle >> 16) * 4u), esp = g_esp, buf, i, a[1], vt, ai;
    if (!obj || obj < 0x10000u || G32(obj + 0x18) != handle || !len)
        return;
    g_esp = (g_esp - (len + 64u)) & ~15u;
    buf = g_esp + 16;
    for (i = 0; i < len; i++)
        G8(buf + i) = msg[i];
    a[0] = buf;
    if (route == 2) {
        ai = G32(obj + 0x29C);
        if (ai) {
            s_call_eax = ai + 0x58;
            gcall(AVP_QUEUE_ORDER, obj, a, 1);
        }
    } else if (route == 3) {
        vt = G32(obj);
        if ((gcall(G32(vt + 0x38), obj, a, 1) & 0xFFu) != 0)
            gcall(G32(vt + 0x3C), obj, a, 1);
    } else {
        s_call_eax = obj;
        gcall(AVP_SEND_COMMAND, obj, a, 1);
    }
    g_esp = esp;
}

static int box_select(uint32_t ptr, float x0, float y0, float x1, float y1)
{
    uint32_t vt, esp = g_esp, tmp, a[3], r, end, e, n = 0;
    int bl = (int)(x0 < x1 ? x0 : x1), bt = (int)(y0 < y1 ? y0 : y1);
    int br = (int)(x0 < x1 ? x1 : x0), bb = (int)(y0 < y1 ? y1 : y0);
    int added = 0;

    if (!ptr)
        return 0;
    vt = G32(ptr);
    g_esp = (g_esp - 64) & ~15u;
    tmp = g_esp + 16;
    G32(tmp) = G32(tmp + 4) = G32(tmp + 8) = G32(tmp + 12) = 0;

    gcall(G32(vt + 0xB4), ptr, NULL, 0);
    a[0] = tmp; a[1] = 0;
    r = gcall(G32(vt + 0xB8), ptr, a, 2);
    end = G32(AVP_REGIONS_END);
    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20) {
        uint32_t obj, unit, h;
        if (!G32(e)
            || !(bl < (int)G32(e + 12) && bt < (int)G32(e + 16)
                 && (int)G32(e + 4) < br && (int)G32(e + 8) < bb))
            continue;
        obj = G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u);
        if (!obj)
            continue;
        n++;
        h = G32(obj + 0x18);
        a[0] = h; a[1] = r; a[2] = tmp;
        unit = gcall(G32(vt + 0xC0), ptr, a, 3);
        if (unit) {
            a[0] = ptr; a[1] = G32(unit + 0x18);
            gcall(AVP_SELECT_UNIT, ptr, a, 2);
            added++;
        }
    }
    list_free(tmp);
    g_esp = esp;
    {
        uint32_t sel = G32(ptr + 0x1B8), hud = G32(AVP_HUD_GROUP);
        if (sel && hud && (int32_t)G32(sel + 4) > 1)
            G32(hud + 0x2D0) = G32(AVP_GAME_TIME);
    }
    if (s_trace)
        fprintf(stderr, "[MOUSE] box select %.0f,%.0f-%.0f,%.0f: %u in the box, %d added\n",
                x0, y0, x1, y1, n, added);
    return added;
}

#define EDGE_BAND   24
#define PAN_RAMP_MS 250.0f

#define AVP_CAMERA  0x002F6AF4u

#define GROUP_MAX 64
#define TAB_W     32
#define TAB_H     15
#define TAB_GAP   3
#define STRIP_Y   3
#define NAME_Y    (STRIP_Y + TAB_H + 3)
#define NAME_SHOW_MS 3000

extern volatile LONG g_avp_mainmenu;
static uint32_t s_grp[10][GROUP_MAX];
static int s_grp_n[10];

typedef struct {
    int n, race, cnt, other;
    char name[20];
} GroupSum;

static GroupSum s_sum[10];
static volatile LONG s_sum_ver[10];
static unsigned s_sum_at[10], s_group_frame;
static volatile LONG s_hover_g, s_last_g;
static volatile ULONGLONG s_last_ms;

static const uint8_t k_digit[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static uint32_t handle_unit(uint32_t h)
{
    uint32_t obj = h ? G32(AVP_OBJ_TABLE + (h >> 16) * 4u) : 0;
    return obj && G32(obj + 0x18) == h ? obj : 0;
}

static int sel_handles(uint32_t ptr, uint32_t *out, int max)
{
    uint32_t sel = G32(ptr + 0x1B8), arr;
    int cnt, i, n = 0;
    if (!sel)
        return 0;
    cnt = (int32_t)G32(sel + 4);
    arr = G32(sel);
    for (i = 0; arr && i < cnt && n < max; i++) {
        uint32_t entry = G32(arr + 4u * i), unit = entry ? G32(entry + 0xA8) : 0, h = unit ? G32(unit + 0x18) : 0;
        if (h)
            out[n++] = h;
    }
    return n;
}

static int group_alive(int g, uint32_t *out)
{
    int i, n = 0;
    for (i = 0; i < s_grp_n[g]; i++)
        if (handle_unit(s_grp[g][i]))
            out[n++] = s_grp[g][i];
    return n;
}

static void group_clear_all(void)
{
    memset(s_grp_n, 0, sizeof s_grp_n);
    memset(s_sum_at, 0, sizeof s_sum_at);
    s_last_g = 0;
    s_hover_g = 0;
}

static void group_store(uint32_t ptr, int g)
{
    uint32_t sel[GROUP_MAX];
    int n = sel_handles(ptr, sel, GROUP_MAX), o, i, j, k;
    for (o = 1; o <= 9; o++) {
        if (o == g || !s_grp_n[o])
            continue;
        for (i = 0, k = 0; i < s_grp_n[o]; i++) {
            for (j = 0; j < n && sel[j] != s_grp[o][i]; j++)
                ;
            if (j == n)
                s_grp[o][k++] = s_grp[o][i];
        }
        if (k != s_grp_n[o]) {
            s_grp_n[o] = k;
            s_sum_at[o] = 0;
        }
    }
    memcpy(s_grp[g], sel, (size_t)n * sizeof sel[0]);
    s_grp_n[g] = n;
    s_sum_at[g] = 0;
    s_last_g = n ? g : 0;
    s_last_ms = GetTickCount64();
    if (s_trace)
        fprintf(stderr, "[MOUSE] group %d stored: %d units\n", g, s_grp_n[g]);
}

static void group_center(uint32_t unit)
{
    uint32_t cam = G32(AVP_CAMERA), pos, one[1] = { 1 };
    int k;
    if (!cam || !unit)
        return;
    pos = gcall(G32(G32(unit) + 0x4C), unit, NULL, 0);
    if (!pos)
        return;
    for (k = 0; k < 3; k++)
        G32(cam + 0x10C + 4u * k) = G32(pos + 4u * k);
    G32(cam + 0x148) = 0;
    s_call_eax = cam;
    gcall(0x0002A1B0u, cam, one, 1);
}

static void group_use(uint32_t ptr, int g)
{
    static int last_g;
    static ULONGLONG last_ms;
    uint32_t h[GROUP_MAX], a[2], hud = G32(AVP_HUD_GROUP);
    ULONGLONG now = GetTickCount64();
    int i, n = group_alive(g, h), dbl = g == last_g && now - last_ms <= 400;
    memcpy(s_grp[g], h, (size_t)n * sizeof h[0]);
    s_grp_n[g] = n;
    s_sum_at[g] = 0;
    if (n) {
        s_last_g = g;
        s_last_ms = now;
    }
    last_g = dbl ? 0 : g;
    last_ms = now;
    if (!n)
        return;
    gcall(G32(G32(ptr) + 0xB4), ptr, NULL, 0);
    for (i = 0; i < n; i++) {
        a[0] = ptr; a[1] = h[i];
        gcall(AVP_SELECT_UNIT, ptr, a, 2);
    }
    if (hud && n > 1)
        G32(hud + 0x2D0) = G32(AVP_GAME_TIME);
    if (dbl)
        group_center(handle_unit(h[0]));
    if (s_trace)
        fprintf(stderr, "[MOUSE] group %d recalled: %d units%s\n", g, n, dbl ? " (centered)" : "");
}

static int strip_count(void)
{
    int g, n = 0;
    for (g = 1; g <= 9; g++)
        if (s_grp_n[g])
            n++;
    return n;
}

static int strip_x0(int count)
{
    return (FRAME_W - (count * TAB_W + (count - 1) * TAB_GAP)) / 2;
}

static int group_at(int fx, int fy)
{
    int g, idx = 0, x0 = strip_x0(strip_count());
    if (fy < STRIP_Y || fy >= STRIP_Y + TAB_H)
        return 0;
    for (g = 1; g <= 9; g++) {
        int x;
        if (!s_grp_n[g])
            continue;
        x = x0 + idx++ * (TAB_W + TAB_GAP);
        if (fx >= x && fx < x + TAB_W)
            return g;
    }
    return 0;
}

static int plausible_text(uint32_t p)
{
    return p >= AVP_IMAGE_LO && p < 0x84000000u - 0x100u;
}

static void unit_type_name(uint32_t unit, char *out, size_t len)
{
    char cb[48];
    const char *c;
    size_t k = 0;
    uint32_t vt = G32(unit), fn = vt >= AVP_IMAGE_LO && vt < AVP_IMAGE_HI ? G32(vt + 0x108u) : 0, s;

    out[0] = 0;
    if (fn >= AVP_IMAGE_LO && fn < AVP_IMAGE_HI) {
        s = gcall(fn, unit, NULL, 0);
        if (plausible_text(s)) {
            for (k = 0; k + 1 < len && G8(s + k) >= 0x20 && G8(s + k) < 0x7F; k++)
                out[k] = (char)toupper(G8(s + k));
            while (k && out[k - 1] == ' ')
                k--;
            out[k] = 0;
        }
    }
    if (k)
        return;
    c = class_name(unit, cb, sizeof cb);
    if (*c == 'G')
        c++;
    if (!strncmp(c, "Marine", 6)) c += 6;
    else if (!strncmp(c, "Predator", 8)) c += 8;
    else if (!strncmp(c, "Pred", 4)) c += 4;
    else if (!strncmp(c, "Alien", 5)) c += 5;
    for (k = 0; k + 1 < len && k < 10 && c[k]; k++)
        out[k] = (char)toupper((unsigned char)c[k]);
    out[k] = 0;
    if (!k)
        snprintf(out, len, "UNIT");
}

static int unit_race(uint32_t unit)
{
    char cb[48];
    const char *c = class_name(unit, cb, sizeof cb);
    if (*c == 'G')
        c++;
    if (!strncmp(c, "Marine", 6))
        return 0;
    if (!strncmp(c, "Pred", 4))
        return 1;
    if (!strncmp(c, "Alien", 5) || !strncmp(c, "AdultAlien", 10))
        return 2;
    return -1;
}

static void group_summarize(int g, GroupSum *out)
{
    uint32_t ah[GROUP_MAX], tds[8];
    char names[8][20];
    int cnts[8], nn = 0, votes[3] = { 0, 0, 0 }, i, j, best = 0, n = group_alive(g, ah);
    memset(out, 0, sizeof *out);
    out->n = n;
    for (i = 0; i < n; i++) {
        uint32_t unit = handle_unit(ah[i]), vt = unit ? G32(unit) : 0, td = vt >= AVP_IMAGE_LO + 4 && vt < AVP_IMAGE_HI ? G32(vt - 4) : 0;
        int r = unit_race(unit);
        if (r >= 0)
            votes[r]++;
        for (j = 0; j < nn && tds[j] != td; j++)
            ;
        if (j == nn && nn < 8) {
            tds[nn] = td;
            unit_type_name(unit, names[nn], sizeof names[0]);
            cnts[nn++] = 0;
        }
        if (j < nn)
            cnts[j]++;
    }
    for (j = 1; j < nn; j++)
        if (cnts[j] > cnts[best])
            best = j;
    if (nn) {
        snprintf(out->name, sizeof out->name, "%.19s", names[best]);
        out->cnt = cnts[best];
        out->other = n - cnts[best];
    }
    out->race = votes[1] > votes[0] ? 1 : 0;
    if (votes[2] > votes[out->race])
        out->race = 2;
    if (!votes[0] && !votes[1] && !votes[2])
        out->race = s_race < 0 ? 0 : s_race;
}

static void group_summaries_refresh(void)
{
    int g;
    s_group_frame++;
    for (g = 1; g <= 9; g++) {
        GroupSum sm;
        if (!s_grp_n[g])
            continue;
        if (s_sum_at[g] && s_group_frame - s_sum_at[g] < 30)
            continue;
        group_summarize(g, &sm);
        s_sum_at[g] = s_group_frame;
        if (!sm.n) {
            s_grp_n[g] = 0;
            continue;
        }
        InterlockedIncrement(&s_sum_ver[g]);
        s_sum[g] = sm;
        InterlockedIncrement(&s_sum_ver[g]);
        if (s_trace)
            fprintf(stderr, "[MOUSE] group %d: %s x%d (+%d other)\n", g, sm.name, sm.cnt, sm.other);
    }
}

static void group_tick(uint32_t ptr)
{
    static uint32_t last_ptr;
    int g = 0, store = 0;
    if (ptr != last_ptr) {
        group_clear_all();
        last_ptr = ptr;
    }
    race_color(ptr);
    group_summaries_refresh();
    if (!avp_group_key_pressed(&g, &store) || g < 1 || g > 9)
        return;
    if (store)
        group_store(ptr, g);
    else
        group_use(ptr, g);
}

static void bar_digit(uint32_t *rgb, int w, int h, int x, int y, int d)
{
    int r, c, sx, sy;
    for (r = 0; r < 5; r++)
        for (c = 0; c < 3; c++) {
            if (!((k_digit[d][r] >> (2 - c)) & 1))
                continue;
            for (sy = 0; sy < 2; sy++)
                for (sx = 0; sx < 2; sx++) {
                    int px = x + c * 2 + sx, py = y + r * 2 + sy;
                    if (px >= 0 && py >= 0 && px < w && py < h)
                        ov_put(px, py, 0x00FFFFFFu, 1.0f);
                }
        }
}

static const uint8_t k_font[39][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 }, { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
};

static void bar_text(uint32_t *rgb, int w, int h, int x, int y, const char *str, uint32_t col)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        const char *t;
        int cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
        for (t = str; *t; t++, cx += 6) {
            int ch = *t, gi, r, c;
            gi = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10
               : ch == '+' ? 36 : ch == '-' ? 37 : ch == '.' ? 38 : -1;
            if (gi < 0)
                continue;
            for (r = 0; r < 7; r++)
                for (c = 0; c < 5; c++) {
                    int px = cx + c, py = cy + r;
                    if (((k_font[gi][r] >> (4 - c)) & 1) && px >= 0 && py >= 0 && px < w && py < h)
                        ov_put(px, py, pass ? col : 0x00080808u, 1.0f);
                }
        }
    }
}

extern volatile LONG g_avp_coop_banner;

static void banner_text(int x, int y, const char *str, uint32_t col, int sc)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        const char *t;
        int cx = x + (pass ? 0 : sc), cy = y + (pass ? 0 : sc);
        for (t = str; *t; t++, cx += 6 * sc) {
            int ch = *t, gi, r, c, i, j;
            gi = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10
               : ch == '+' ? 36 : ch == '-' ? 37 : ch == '.' ? 38 : -1;
            if (gi < 0)
                continue;
            for (r = 0; r < 7; r++)
                for (c = 0; c < 5; c++) {
                    if (!((k_font[gi][r] >> (4 - c)) & 1))
                        continue;
                    for (i = 0; i < sc; i++)
                        for (j = 0; j < sc; j++)
                            ov_put(cx + c * sc + i, cy + r * sc + j, pass ? col : 0x00080808u, 1.0f);
                }
        }
    }
}

static void coop_banner_draw(int w, int h)
{
    static const char wait[] = "WAITING FOR OTHER PLAYER TO LOAD...";
    static const char stall[] = "HOST NOT RESPONDING";
    static const char lost[] = "CONNECTION LOST";
    LONG b = g_avp_coop_banner;
    if (b == 1)
        banner_text((w - (int)(sizeof wait - 1) * 12) / 2, h / 2 - 7, wait, 0x00FFFFFFu, 2);
    else if (b == 2)
        banner_text((w - (int)(sizeof stall - 1) * 12) / 2, 24, stall, 0x00FFC040u, 2);
    else if (b == 3)
        banner_text((w - (int)(sizeof lost - 1) * 12) / 2, 24, lost, 0x00FF3030u, 2);
}

static void bar_dot(uint32_t *rgb, int w, int h, int x, int y, uint32_t col)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        ov_put(x, y, col, 1.0f);
}

static void bar_mark(uint32_t *rgb, int w, int h, int x0, int y0, int race, uint32_t col)
{
    static const uint8_t shape[3][6] = {
        { 0x0C, 0x1E, 0x3F, 0x3F, 0x1E, 0x0C },
        { 0x0C, 0x0C, 0x1E, 0x1E, 0x3F, 0x3F },
        { 0x1E, 0x33, 0x2D, 0x2D, 0x33, 0x1E },
    };
    int x, y;
    for (y = 0; y < 6; y++)
        for (x = 0; x < 6; x++)
            if ((shape[race][y] >> (5 - x)) & 1)
                bar_dot(rgb, w, h, x0 + x, y0 + y, col);
}

static void group_bar_draw(uint32_t *rgb, int w, int h)
{
    static const uint32_t race_rgb[3] = { 0x003C8CFFu, 0x00FF8C1Eu, 0x0060FF60u };
    uint32_t ptr = G32(AVP_POINTER), sh[GROUP_MAX], ah[GROUP_MAX];
    int g, idx = 0, ns, x0, count = strip_count(), want, shown_x = 0;
    GroupSum shown;
    if (!ptr || !s_in_mission || g_avp_mainmenu || !count)
        return;
    ns = sel_handles(ptr, sh, GROUP_MAX);
    x0 = strip_x0(count);
    want = (int)s_hover_g;
    if (!want && GetTickCount64() - s_last_ms < NAME_SHOW_MS)
        want = (int)s_last_g;
    memset(&shown, 0, sizeof shown);
    for (g = 1; g <= 9; g++) {
        int tx, x, y, na, i, j, same, ring, cx, tries;
        uint32_t rc, bc, bright;
        char line[8];
        GroupSum sm;
        if (!s_grp_n[g])
            continue;
        tx = x0 + idx++ * (TAB_W + TAB_GAP);
        memset(&sm, 0, sizeof sm);
        for (tries = 0; tries < 3; tries++) {
            LONG v0 = s_sum_ver[g], v1;
            sm = s_sum[g];
            v1 = s_sum_ver[g];
            if (v0 == v1 && !(v0 & 1))
                break;
        }
        if (!sm.n || sm.race < 0 || sm.race > 2)
            continue;
        na = group_alive(g, ah);
        same = na > 0 && na == ns;
        for (i = 0; same && i < na; i++) {
            for (j = 0; j < ns && sh[j] != ah[i]; j++)
                ;
            same = j < ns;
        }
        rc = race_rgb[sm.race];
        bright = blend(rc, 0x00FFFFFFu, 0.6f);
        bc = same ? bright : rc;
        ring = 1;
        bc = same ? bright : blend(rc, 0x00101010u, 0.35f);
        for (y = STRIP_Y; y < STRIP_Y + TAB_H && y < h; y++)
            for (x = tx; x < tx + TAB_W && x < w; x++) {
                int dx = x - tx, dy = y - STRIP_Y, ex = TAB_W - 1 - dx, ey = TAB_H - 1 - dy;
                int edge = dx < ring || ex < ring || dy < ring || ey < ring;
                if ((dx + dy < 3) || (ex + dy < 3) || (dx + ey < 3) || (ex + ey < 3)) {
                    if (dx + dy == 2 || ex + dy == 2 || dx + ey == 2 || ex + ey == 2)
                        ov_put(x, y, bc, 1.0f);
                    continue;
                }
                if (dx >= 2 && dx < 5 && dy >= 3 && dy < TAB_H - 3)
                    ov_put(x, y, rc, 1.0f);
                else if (edge)
                    ov_put(x, y, bc, 1.0f);
                else
                    ov_put(x, y, 0x00080C10u, 0.72f);
            }
        snprintf(line, sizeof line, "%d", g);
        bar_text(rgb, w, h, tx + 8, STRIP_Y + 4, line, same ? 0x00FFFFFFu : 0x00C8D0D8u);
        snprintf(line, sizeof line, "%d", na < 99 ? na : 99);
        cx = tx + TAB_W - 4 - (int)strlen(line) * 6;
        bar_text(rgb, w, h, cx, STRIP_Y + 4, line, same ? 0x00FFFFFFu : 0x00C8D0D8u);
        if (g == want) {
            shown = sm;
            shown_x = tx;
        }
    }
    if (shown.name[0]) {
        char line[40];
        int len, nx;
        if (shown.other)
            snprintf(line, sizeof line, "%s X%d +%d", shown.name, shown.cnt, shown.other);
        else
            snprintf(line, sizeof line, "%s X%d", shown.name, shown.cnt);
        len = (int)strlen(line) * 6;
        nx = shown_x + TAB_W / 2 - len / 2;
        if (nx < 2)
            nx = 2;
        if (nx + len > w - 2)
            nx = w - 2 - len;
        bar_text(rgb, w, h, nx, NAME_Y, line, 0x00FFFFFFu);
    }
}

static int s_goto;
static float s_goto_x, s_goto_y;
static ULONGLONG s_goto_start, s_goto_progress;

static int goto_step(float *sx, float *sy)
{
    uint32_t cam = G32(AVP_CAMERA);
    float dx, dy, d, v;
    const char *e;
    if (!s_goto || !cam)
        return 0;
    dx = s_goto_x - GF(cam + 0x10C);
    dy = s_goto_y - GF(cam + 0x110);
    d = sqrtf(dx * dx + dy * dy);
    {
        static float best;
        ULONGLONG now = GetTickCount64();
        if (s_goto_progress == s_goto_start || d < best - 0.5f) {
            best = d;
            s_goto_progress = now + (s_goto_progress == s_goto_start);
        }
        if (now > s_goto_progress + 1000)
            d = 0.0f;
    }
    if (d < 1.5f) {
        if (s_trace)
            fprintf(stderr, "[MOUSE] minimap goto done, %.2f away\n", d);
        s_goto = 0;
        return 0;
    }
    e = getenv("AVP_MAP_SPEED");
    v = (e ? (float)atof(e) : 1.0f) * (d > 12.0f ? 1.0f : 0.25f + 0.75f * d / 12.0f);
    *sx = dx / d * v;
    *sy = dy / d * v;
    return 1;
}

static float edge_push(int p, int size)
{
    if (p < EDGE_BAND)
        return -(0.35f + 0.65f * (float)(EDGE_BAND - p) / EDGE_BAND);
    if (p > size - 1 - EDGE_BAND)
        return 0.35f + 0.65f * (float)(p - (size - 1 - EDGE_BAND)) / EDGE_BAND;
    return 0.0f;
}

#define AVP_CAM_TRACK      0x148u
#define AVP_CAM_SUPERTRACK 0x002F6AF9u

static void camera_release(uint32_t cam)
{
    if (!cam)
        return;
    if (s_trace && G32(cam + AVP_CAM_TRACK))
        fprintf(stderr, "[MOUSE] camera tracking %08X released\n", G32(cam + AVP_CAM_TRACK));
    G32(cam + AVP_CAM_TRACK) = 0;
    G8(AVP_CAM_SUPERTRACK) = 0;
}

static int camera_pan(const Cmd *cmd, int fx, int fy, uint32_t args)
{
    static ULONGLONG start;
    static int panning;
    static float lsx, lsy;
    float sx = 0.0f, sy = 0.0f, ramp;
    int inside = fx >= 0 && fy >= 0 && fx < avp_wide_width() && fy < FRAME_H;

    if (!cmd[C_GPMOVE].owner || cmd[C_MACT].owner) {
        if (s_goto && s_trace) {
            static int told;
            if (told++ < 3)
                fprintf(stderr, "[MOUSE] minimap goto waits: move owner %08X, menu owner %08X\n",
                        cmd[C_GPMOVE].owner, cmd[C_MACT].owner);
        }
        panning = 0;
        return 0;
    }
    if (inside && !getenv("AVP_NO_EDGE") && !group_at(fx - avp_wide_inset(), fy)) {
        sx = edge_push(fx, avp_wide_width());
        sy = edge_push(fy, FRAME_H);
    }
    if (avp_action_down(AVP_ACT_PAN_LEFT)) sx -= 1.0f;
    if (avp_action_down(AVP_ACT_PAN_RIGHT)) sx += 1.0f;
    if (avp_action_down(AVP_ACT_PAN_UP)) sy -= 1.0f;
    if (avp_action_down(AVP_ACT_PAN_DOWN)) sy += 1.0f;
    sx = sx < -1.0f ? -1.0f : sx > 1.0f ? 1.0f : sx;
    sy = sy < -1.0f ? -1.0f : sy > 1.0f ? 1.0f : sy;
    if (sx != 0.0f || sy != 0.0f)
        s_goto = 0;
    else if (goto_step(&sx, &sy))
        start = GetTickCount64() - (ULONGLONG)PAN_RAMP_MS;
    if (s_trace && ((sx != 0.0f) != (lsx != 0.0f) || (sy != 0.0f) != (lsy != 0.0f)))
        fprintf(stderr, "[MOUSE] camera pan %.2f,%.2f\n", sx, sy);
    lsx = sx; lsy = sy;
    if (sx == 0.0f && sy == 0.0f) {
        if (panning && s_trace) {
            uint32_t cam = G32(AVP_CAMERA);
            fprintf(stderr, "[MOUSE] pan end: modes A %02X B %02X, camera %.1f,%.1f\n",
                    G8(AVP_CAM_MODE_A), G8(AVP_CAM_MODE_B),
                    cam ? GF(cam + 0x10C) : 0.0f, cam ? GF(cam + 0x110) : 0.0f);
        }
        panning = 0;
        return 0;
    }
    if (!panning) {
        panning = 1;
        start = GetTickCount64();
        if (s_trace) {
            uint32_t cam = G32(AVP_CAMERA);
            fprintf(stderr, "[MOUSE] pan start: modes A %02X B %02X, camera %.1f,%.1f, "
                    "+148 %08X scripted %02X orbit %02X super %02X\n", G8(AVP_CAM_MODE_A),
                    G8(AVP_CAM_MODE_B), cam ? GF(cam + 0x10C) : 0.0f, cam ? GF(cam + 0x110) : 0.0f,
                    cam ? G32(cam + AVP_CAM_TRACK) : 0, cam ? G8(cam + 0x188) : 0,
                    cam ? G8(cam + 0x189) : 0, G8(AVP_CAM_SUPERTRACK));
        }
    }
    ramp = (float)(GetTickCount64() - start) / PAN_RAMP_MS;
    ramp = ramp < 0.25f ? 0.25f : ramp > 1.0f ? 1.0f : ramp;
    G32(args) = f2u(sx * ramp); G32(args + 4) = f2u(sy * ramp); G32(args + 8) = 0;
    camera_release(cmd[C_GPMOVE].owner);
    if (G8(AVP_CAM_MODE_A)) {

        uint32_t ptr = G32(AVP_POINTER);
        s_call_edi = ptr;
        gcall(cmd[C_GPMOVE].handler, cmd[C_GPMOVE].owner, &args, 1);
        pointer_free(ptr);
    } else {

        uint32_t cam = cmd[C_GPMOVE].owner, a[2], map = G32(AVP_MINIMAP);
        if (map && G8(map + 0x25D)) {
            G8(map + 0x25D) = 0;
            gcall(AVP_MINIMAP_RELEASE, 0, NULL, 0);
        }
        a[0] = f2u(sx * ramp); a[1] = f2u(sy * ramp);
        s_call_eax = cam;
        gcall(AVP_CAM_PAN, cam, a, 2);
    }
    return 1;
}

static void minimap_click(int fx, int fy)
{
    static int traced;
    uint32_t map = G32(AVP_MINIMAP);
    const char *ex = getenv("AVP_MAP_OX"), *ey = getenv("AVP_MAP_OY");
    float ox = ex ? (float)atof(ex) : 0.0f, oy = ey ? (float)atof(ey) : 0.0f;
    float scale, cx, cy, mx, my;

    if (!map || !G32(AVP_CAMERA))
        return;
    camera_release(G32(AVP_CAMERA));
    scale = GF(map + 0xF6C);
    mx = GF(map + 0xF70); my = GF(map + 0xF74);
    if (!(scale > 0.0f) || !(mx > 0.0f) || !(my > 0.0f))
        return;
    cx = (float)fx + ox - GF(map + 0xF88);
    cy = (float)fy + oy - GF(map + 0xF8C);
    cx = cx < 0.0f ? 0.0f : cx > mx ? mx : cx;
    cy = cy < 0.0f ? 0.0f : cy > my ? my : cy;
    s_goto_x = cx * scale; s_goto_y = cy * scale;
    if (getenv("AVP_MAP_GLIDE")) {
        if (!s_goto)
            s_goto_start = s_goto_progress = GetTickCount64();
        s_goto = 1;
    } else {

        static const uint32_t vec[3] = { 0x10C, 0x40, 0x170 };
        uint32_t cam = G32(AVP_CAMERA);
        float dx = s_goto_x - GF(cam + 0x10C), dy = s_goto_y - GF(cam + 0x110);
        int i;
        for (i = 0; i < 3; i++) {
            GF(cam + vec[i]) += dx;
            GF(cam + vec[i] + 4) += dy;
        }
        s_goto = 0;
    }
    if (s_trace && traced < 3) {
        uint32_t cam = G32(AVP_CAMERA);
        traced++;
        fprintf(stderr, "[MOUSE] minimap click %d,%d: map pixel %.1f,%.1f -> world %.2f,%.2f "
                "(camera at %.2f,%.2f)\n", fx, fy, cx, cy, s_goto_x, s_goto_y,
                GF(cam + 0x10C), GF(cam + 0x110));
    }
}

#define AVP_BTN_RELEASE 0x00091180u
#define RELEASE_UPDATES 6

static struct { uint32_t ms, crc; int hold; } s_cmds[32];
static int s_ncmds, s_cmd_i;

static void cmd_script_parse(const char *s)
{
    while (s && *s && s_ncmds < 32) {
        char name[64], *end;
        size_t n;
        uint32_t ms = (uint32_t)strtoul(s, &end, 10);
        const char *semi;
        if (*end != ':')
            break;
        end++;
        semi = strchr(end, ';');
        n = semi ? (size_t)(semi - end) : strlen(end);
        if (n >= sizeof name)
            n = sizeof name - 1;
        memcpy(name, end, n);
        name[n] = 0;
        s_cmds[s_ncmds].hold = n && name[n - 1] == '+';
        if (s_cmds[s_ncmds].hold)
            name[n - 1] = 0;
        s_cmds[s_ncmds].ms = ms;
        s_cmds[s_ncmds].crc = name[0] == '#' ? (uint32_t)strtoul(name + 1, NULL, 16)
                                             : crc_name(name);
        s_ncmds++;
        s = semi ? semi + 1 : NULL;
    }
}

static void cmd_script_run(uint32_t mgr, uint32_t mode, uint32_t ms)
{
    static uint32_t rel_owner;
    static int rel_left;
    uint32_t n = G32(mgr + 0x4C), arr = G32(mgr + 0x48), i, blk = mgr + 0x9C;

    if (rel_left > 0 && --rel_left == 0) {
        for (i = 0; arr && i < n && i < 4096; i++) {
            uint32_t e = arr + i * 16;
            if ((G32(e) & mode) && G32(e + 12) == AVP_BTN_RELEASE
                && G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u) == rel_owner) {
                fprintf(stderr, "[MOUSE] cmd release, owner %08X\n", rel_owner);
                gcall(AVP_BTN_RELEASE, rel_owner, &blk, 1);
                break;
            }
        }
    }
    if (s_cmd_i >= s_ncmds || ms < s_cmds[s_cmd_i].ms)
        return;
    {
        int hold = s_cmds[s_cmd_i].hold;
        uint32_t crc = s_cmds[s_cmd_i++].crc;
        for (i = 0; arr && i < n && i < 4096; i++) {
            uint32_t e = arr + i * 16, trig = G32(e + 4), owner;
            if (!(G32(e) & mode) || !trig || G32(trig + 8) != crc)
                continue;
            owner = G32(AVP_OBJ_TABLE + (G32(e + 8) >> 16) * 4u);
            fprintf(stderr, "[MOUSE] cmd %08X: handler %08X owner %08X (vt %08X)\n",
                    crc, G32(e + 12), owner, owner ? G32(owner) : 0);
            if (owner) {
                gcall(G32(e + 12), owner, &blk, 1);
                if (G32(e + 12) != AVP_BTN_RELEASE && !hold) {
                    rel_owner = owner;
                    rel_left = RELEASE_UPDATES;
                }
            }
            return;
        }
        fprintf(stderr, "[MOUSE] cmd %08X: not active in mode %08X\n", crc, mode);
    }
}

extern volatile LONG g_avp_mainmenu, g_avp_menu_req, g_avp_pcmenu_up, g_avp_menu_sound;
volatile LONG g_avp_mainmenu_obj;
extern int avp_pcmenu_hides_game(void);

#define AVP_ITEM_SELECTABLE 0x8u

static int menu_obj_ok(uint32_t o)
{
    return o >= 0x10000u && o < 0x84000000u - 0x1000u;
}

static int under_main(uint32_t item, uint32_t main)
{
    int d, sel = 0;
    for (d = 0; d < 8 && menu_obj_ok(item); d++) {
        if (G32(item + 0x10Cu) & AVP_ITEM_SELECTABLE)
            sel = 1;
        if (G32(item + 0x0Cu) == main)
            return sel;
        item = G32(item + 0x0Cu);
    }
    return 0;
}

int avp_menu_hide_render(uint32_t r)
{
    uint32_t main = (uint32_t)g_avp_mainmenu_obj, item = 0, pos;
    if (!main || !g_avp_mainmenu || !menu_obj_ok(r) || !avp_pcmenu_hides_game())
        return 0;
    pos = G32(r + 8);
    if (pos == r - 0x70u + 0x1CCu)
        item = r - 0x70u;
    else if (pos == r - 0x1DCu + 0x1C0u)
        item = r - 0x1DCu;
    else if (menu_obj_ok(pos - 0x1CCu) && G32(pos - 0x1CCu + 0x248u) == r)
        item = pos - 0x1CCu;
    return item && menu_obj_ok(item) && under_main(item, main);
}

#define AVP_ITEM_ACTIVATED_SND 0x160u
#define AVP_ITEM_FOCUSED_SND   0x164u
#define AVP_SOUND_POST         0x000E1620u
#define AVP_SOUND_SYSTEM       0x002F6834u

static void item_sound(uint32_t item, uint32_t field)
{
    uint32_t h, args[2];
    if (!item || item < 0x80000u)
        return;
    h = G32(item + field);
    if (!h || !G32(AVP_SOUND_SYSTEM))
        return;
    args[0] = G32(AVP_SOUND_SYSTEM);
    args[1] = 0;
    s_call_esi = scratch(64);
    gcall(AVP_SOUND_POST, h, args, 2);
    if (s_trace)
        fprintf(stderr, "[MOUSE] %s sound of %08X (handle %08X)\n",
                field == AVP_ITEM_ACTIVATED_SND ? "activate" : "focus", item, h);
}

static void pcmenu_select(uint32_t mgr, uint32_t mode, uint32_t fm, const Cmd *cmd, int want)
{
    uint32_t items[16], one = 1, blk = mgr + 0x9C, k;
    float tops[16];
    int n = 0, i;

    for (k = 1; k < 0x10000u && fm; k++) {
        uint32_t obj = G32(AVP_OBJ_TABLE + k * 4u), st, a[5];
        float y;
        if (!obj || obj < AVP_IMAGE_HI || obj >= 0x84000000u - 0x1000u || G32(obj + 0x0Cu) != fm)
            continue;
        if ((G32(obj + 0x18u) >> 16) != k || G32(G32(obj) + 0xB0u) < AVP_IMAGE_LO)
            continue;
        st = G32(obj + 0x10Cu);
        if (!(st & 0x8u) || !(st & 0x4u) || n >= 16)
            continue;
        a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_MENUITEM; a[4] = 0;
        if (!gcall(AVP_DYNCAST, 0, a, 5)) continue;
        y = GF(obj + 0x1D0u);
        if (GF(obj + 0x1CCu) < 300.0f || y > 420.0f)
            continue;
        if (s_trace)
            fprintf(stderr, "[PCMENU]   item %08X state %08X at %d,%d\n", obj, st,
                    (int)GF(obj + 0x1CCu), (int)y);
        for (i = n; i > 0 && tops[i - 1] > y; i--) {
            items[i] = items[i - 1];
            tops[i] = tops[i - 1];
        }
        items[i] = obj;
        tops[i] = y;
        n++;
    }
    fprintf(stderr, "[PCMENU] game main menu: %d buttons, selecting %d\n", n, want);
    if (want < 0 || want >= n || n != 6)
        return;
    {
        uint32_t item = items[want];
        uint32_t fh = G32(fm + AVP_FOCUSED);
        uint32_t focused = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
        if (focused != item)
            gcall(G32(G32(item) + 0xB0), item, &one, 1);
        if (cmd[C_FSEL].owner) {
            item_sound(item, AVP_ITEM_ACTIVATED_SND);
            gcall(cmd[C_FSEL].handler, cmd[C_FSEL].owner, &blk, 1);
        }
        else
            nav_key_tap(AVP_ACT_A);
    }
    (void)mode;
}

static void rclick_log(const char *tag, int full, int hud_on, int back_frames, int rc_defer, const Cmd *cmd)
{
    uint32_t ptr = G32(AVP_POINTER), sel = ptr ? G32(ptr + 0x1B8) : 0;
    int32_t state = ptr ? (int32_t)G32(ptr + 0x1C4) : -1;
    fprintf(stderr, "[RCLICK] %s ptr %08X state %d target %08X sel %d", tag, ptr, state,
            ptr ? G32(ptr + 0xDC) : 0, sel ? (int32_t)G32(sel + 4) : 0);
    if (full) {
        const char *how = "none";
        uint32_t menu = state > 0 ? find_avp_menu(state, &how) : 0, item = 0;
        if (menu && state < (int32_t)G32(menu + 0x2A0))
            item = G32(G32(menu + 0x294) + 4u * (uint32_t)state);
        fprintf(stderr, " menu %08X item %08X back_frames %d hud %d defer %d B2ON %08X B2OFF %08X MACT %08X",
                menu, item, back_frames, hud_on, rc_defer, cmd[C_B2ON].owner, cmd[C_B2OFF].owner,
                cmd[C_MACT].owner);
    }
    fprintf(stderr, "\n");
}

static void avp_mouse_frame_body(uint32_t mgr)
{
    static uint32_t last_mode = 0xFFFFFFFFu, last_n = 0xFFFFFFFFu;
    static int last_x = -100000, last_y = -100000, last_b, zoom_left, zoom_dir;
    static int back_frames, shop_act = -1;
    static uint32_t back_owner;
    static uint32_t sel_owner;
    static int drag_on, drag_box, drag_fx, drag_fy, bar_press;
    static float drag_tx, drag_ty;
    static int rc_defer, rc_left, frame_no, mv_fx = -100000, mv_fy, mv_frame;
    static ULONGLONG t0;
    static uint32_t last_owner;
    Cmd cmd[C_N];
    uint32_t mode, vpp, vp, args;
    int fx = 0, fy = 0, fxf = 0, b = 0, wheel = 0, focus, c, pass;
    enum { HUD_NONE, HUD_SHOP, HUD_MAP } hud;
    float left, top, right, bottom, tx, ty;

    if (s_on < 0) {
        const char *e = getenv("AVP_MOUSE");
        s_on = !(e && e[0] == '0');
        s_trace = getenv("AVP_INPUT_TRACE") != NULL;
        script_parse(getenv("AVP_MOUSE_SCRIPT"));
        cmd_script_parse(getenv("AVP_CMD_SCRIPT"));
        for (c = 0; c < C_N; c++)
            k_crc[c] = crc_name(k_names[c]);
        if (s_trace)
            names_build();
        t0 = avp_coop_now_ms();
        fprintf(stderr, "[MOUSE] %s%s\n", s_on ? "on" : "off",
                s_script_n ? " (scripted)" : "");
    }
    if (mgr && mgr == G32(AVP_INPUT_MGR))
        avp_wide_frame();
    if (!s_on || mgr != G32(AVP_INPUT_MGR) || !mgr)
        return;

    if (back_frames > 0 && --back_frames == 0) {
        avp_pad_set(AVP_ACT_B, 0);
        avp_pad_set(AVP_ACT_START, 0);
        avp_pad_set(AVP_ACT_BLACK, 0);
        if (shop_act >= 0)
            avp_pad_set(shop_act, 0);
    }

    mode = G32(mgr + 0x94);
    if (s_trace && (mode != last_mode || G32(mgr + 0x4C) != last_n)) {
        trace_list(mgr, mode);
        last_mode = mode;
        last_n = G32(mgr + 0x4C);
    }
    find_cmds(mgr, mode, cmd);
    s_in_mission = cmd[C_PPOS].owner != 0;
    if (s_ncmds)
        cmd_script_run(mgr, mode, (uint32_t)(avp_coop_now_ms() - t0));

    if (G32(AVP_POINTER) && !g_avp_pcmenu_up)
        group_tick(G32(AVP_POINTER));

    {
        static int adv = -1, adv_hold, from_mission = -1, mission_started;
        static ULONGLONG adv_last;
        static const char *scan;
        ULONGLONG now = GetTickCount64();
        if (adv < 0) {
            adv = getenv("AVP_AUTO_ADVANCE") != NULL;
            from_mission = getenv("AVP_SCRIPT_FROM_MISSION") != NULL;
            scan = getenv("AVP_OBJ_SCAN");
        }
        static int adv_act = AVP_ACT_A;
        if (adv_hold > 0 && --adv_hold == 0)
            avp_pad_set(adv_act, 0);
        static int sk_presses;
        static ULONGLONG sk_since;
        int sk_auto = avp_skirmish_armed() && !g_avp_pcmenu_up;
        if (!sk_auto)
            sk_since = 0;
        else if (!sk_since) {
            sk_since = now;
            sk_presses = 0;
        } else if (now - sk_since > 60000) {
            fprintf(stderr, "[SKIRMISH] auto-start timed out\n");
            avp_skirmish_disarm();
            sk_auto = 0;
        }
        if ((adv || sk_auto) && !adv_hold && !cmd[C_PPOS].owner && !mission_started
            && now - adv_last > (sk_auto ? 700 : 2500)) {
            adv_last = now;
            if (sk_auto) {
                sk_presses++;
                adv_act = sk_presses <= 6 || adv_act == AVP_ACT_START ? AVP_ACT_A : AVP_ACT_START;
                fprintf(stderr, "[SKIRMISH] auto-start press %d\n", sk_presses);
            } else {
                adv_act = adv_act == AVP_ACT_A ? AVP_ACT_START : AVP_ACT_A;
            }
            avp_pad_set(adv_act, 1);
            adv_hold = 8;
            if (s_trace) fprintf(stderr, "[MOUSE] auto advance\n");
        }
        static int play_frames;
        play_frames = (cmd[C_B1ON].owner && mode == 1) ? play_frames + 1 : 0;
        {
            static int coop_started, g_was, menu_frames;
            int g_now = avp_action_down(AVP_ACT_GIVE);
            menu_frames = g_avp_mainmenu ? menu_frames + 1 : 0;
            if (coop_started && menu_frames > 60) {
                coop_started = 0;
                avp_coop_mission_end();
            }
            if (!coop_started && play_frames > 60) {
                coop_started = 1;
                avp_coop_mission_start();
                avp_skirmish_mission_start();
                group_clear_all();
            }
            if (coop_started && g_now && !g_was)
                avp_coop_give_selection();
            g_was = g_now;
            if (coop_started)
                avp_skirmish_tick();
        }
        if (from_mission && !mission_started && play_frames > 60) {
            mission_started = 1;
            t0 = avp_coop_now_ms();
            s_script_i = 0;
#ifdef _WIN32
            {
                FILETIME c, x, k, u;
                ULARGE_INTEGER ci;
                FILETIME nowft;
                ULARGE_INTEGER ni;
                GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
                GetSystemTimeAsFileTime(&nowft);
                ci.LowPart = c.dwLowDateTime; ci.HighPart = c.dwHighDateTime;
                ni.LowPart = nowft.dwLowDateTime; ni.HighPart = nowft.dwHighDateTime;
                fprintf(stderr, "[MOUSE] mission pointer: script clock starts at %.1f s uptime\n",
                        (double)(ni.QuadPart - ci.QuadPart) / 1e7);
            }
#endif
            if (getenv("AVP_HANDLE_DUMP")) {
                const char *h = getenv("AVP_HANDLE_DUMP");
                while (*h) {
                    char *end, cb[64];
                    uint32_t idx, obj, vt, i;
                    if (*h == 'n') {
                        uint32_t want = (uint32_t)strtoul(h + 1, &end, 16);
                        for (i = 0; i < 0x200; i += 4) {
                            uint32_t col = G32(want - i - 4), td, k;
                            cb[0] = 0;
                            if (col >= AVP_IMAGE_LO && col < AVP_IMAGE_HI) {
                                td = G32(col + 12);
                                if (td >= AVP_IMAGE_LO && td < AVP_IMAGE_HI && G8(td + 8) == '.' && G8(td + 11) == 'V') {
                                    for (k = 0; k + 1 < sizeof cb && G8(td + 12 + k) && G8(td + 12 + k) != '@'; k++)
                                        cb[k] = (char)G8(td + 12 + k);
                                    cb[k] = 0;
                                }
                            }
                            if (cb[0]) {
                                fprintf(stderr, "[HDL] slot %08X: vtable %08X class %s\n", want, want - i, cb);
                                break;
                            }
                        }
                        h = *end ? end + 1 : end;
                        continue;
                    }
                    if (*h == 'v') {
                        uint32_t want = (uint32_t)strtoul(h + 1, &end, 16);
                        for (i = 1; i < 0x10000; i++) {
                            obj = G32(AVP_OBJ_TABLE + i * 4u);
                            if (obj >= 0x10000u && obj < 0x84000000u && G32(obj) == want)
                                fprintf(stderr, "[HDL] vt %08X: handle %04X obj %08X %s\n", want, i, obj, class_name(obj, cb, sizeof cb));
                        }
                        h = *end ? end + 1 : end;
                        continue;
                    }
                    idx = (uint32_t)strtoul(h, &end, 16);
                    if (end == h) break;
                    obj = G32(AVP_OBJ_TABLE + (idx & 0xFFFFu) * 4u);
                    vt = obj ? G32(obj) : 0;
                    fprintf(stderr, "[HDL] %04X obj %08X %s vt %08X:", idx, obj, class_name(obj, cb, sizeof cb), vt);
                    for (i = 0; vt >= AVP_IMAGE_LO && vt < AVP_IMAGE_HI && i < 48; i++)
                        fprintf(stderr, " %X", G32(vt + i * 4));
                    fprintf(stderr, "\n");
                    h = *end ? end + 1 : end;
                }
            }
            if (getenv("AVP_PTR_DUMP")) {
                uint32_t p = G32(AVP_POINTER), o;
                for (o = 0; p && o < 0x400; o += 4)
                    fprintf(stderr, "[PTR] +%03X %08X %g\n", o, G32(p + o), (double)GF(p + o));
                p = G32(0x002F6AECu);
                fprintf(stderr, "[MKO] object %08X\n", p);
                for (o = 0; p && o < 0x300; o += 4)
                    fprintf(stderr, "[MKO] +%03X %08X %g\n", o, G32(p + o), (double)GF(p + o));
            }
        }
        s_script_wait = from_mission && !mission_started;
        if (scan && *scan) {
            static ULONGLONG scan_last;
            static int last_count = -1;
            if (now - scan_last > 250) {
                uint32_t i, first = 0;
                int count = 0;
                char cb[64];
                scan_last = now;
                for (i = 1; i < 0x10000; i++) {
                    uint32_t obj = G32(AVP_OBJ_TABLE + i * 4u);
                    if (!obj || obj < 0x10000u || obj >= 0x84000000u)
                        continue;
                    if (strstr(class_name(obj, cb, sizeof cb), scan)) {
                        if (!first) first = obj;
                        count++;
                    }
                }
                if (count != last_count) {
                    fprintf(stderr, "[SCAN] %d objects of %s* (first %08X %s)\n", count, scan,
                            first, first ? class_name(first, cb, sizeof cb) : "");
                    last_count = count;
                }
            }
        }
        if (getenv("AVP_CAM_TRACE")) {
            static ULONGLONG cam_last;
            uint32_t cam = G32(0x002F6AF4u), ptr = G32(AVP_POINTER);
            if (cam && ptr && now - cam_last > 250) {
                int j;
                cam_last = now;
                fprintf(stderr, "[CAM]");
                for (j = 0; j < 0x200; j += 4) {
                    float v = GF(cam + j);
                    if (fabsf(v) > 1e-3f && fabsf(v) < 1e5f)
                        fprintf(stderr, " %X=%g", j, (double)v);
                }
                fprintf(stderr, " | ptr %g %g ground %g %g %g\n",
                        (double)GF(ptr + 0x40), (double)GF(ptr + 0x44),
                        (double)GF(ptr + AVP_PTR_GROUND), (double)GF(ptr + AVP_PTR_GROUND + 4),
                        (double)GF(ptr + AVP_PTR_GROUND + 8));
            }
        }
        {
            static const char *vts = (const char *)1;
            static uint32_t vt[4], snap[4][0x8C];
            static int nvt, have[4];
            static ULONGLONG vt_last;
            if (vts == (const char *)1) {
                vts = getenv("AVP_VT_SCAN");
                while (vts && *vts && nvt < 4) {
                    char *end;
                    vt[nvt] = (uint32_t)strtoul(vts, &end, 16);
                    if (end == vts) break;
                    nvt++;
                    vts = *end ? end + 1 : end;
                }
            }
            if (nvt && now - vt_last > 500) {
                uint32_t a;
                int v, n[4] = {0};
                vt_last = now;
                for (a = 0x00200000u; a < 0x04000000u; a += 4) {
                    uint32_t d;
                    if (a == 0x00780000u) a = 0x00F80000u;
                    d = G32(a);
                    for (v = 0; v < nvt; v++) {
                        if (d != vt[v]) continue;
                        if (!n[v]) {
                            int j, diffs = 0;
                            for (j = 0; j < 0x8C; j++) {
                                uint32_t w = G32(a + j * 4u);
                                if (have[v] && w != snap[v][j] && diffs < 24) {
                                    if (!diffs) fprintf(stderr, "[VT] %08X at %08X changed:", vt[v], a);
                                    fprintf(stderr, " +%X=%08X(%g)", j * 4, w, (double)*(float *)&w);
                                    diffs++;
                                }
                                snap[v][j] = w;
                            }
                            if (diffs) fprintf(stderr, "\n");
                            have[v] = 1;
                        }
                        n[v]++;
                    }
                }
                for (v = 0; v < nvt; v++) {
                    static int last_n[4] = {-1, -1, -1, -1};
                    if (n[v] != last_n[v]) {
                        fprintf(stderr, "[VT] %08X: %d instances\n", vt[v], n[v]);
                        last_n[v] = n[v];
                    }
                }
            }
        }
    }

    if (!s_script_n && getenv("AVP_MOUSE_PASSIVE"))
        return;
    if (s_script_n) {
        ULONGLONG ms = s_script_wait ? 0 : avp_coop_now_ms() - t0;
        static int last_step = -1;

        {
            static struct { uint32_t ms, hold; int vk, state; } keys[16];
            static int nkeys = -1, k;
            if (nkeys < 0) {
                const char *s = getenv("AVP_KEY_SCRIPT");
                nkeys = 0;
                while (s && *s && nkeys < 16) {
                    char *end;
                    keys[nkeys].ms = (uint32_t)strtoul(s, &end, 10);
                    if (*end != ':' || !end[1]) break;
                    keys[nkeys].vk = (unsigned char)end[1];
                    s = end + 2;
                    keys[nkeys].hold = *s == ':' ? (uint32_t)strtoul(s + 1, &end, 10) : 150;
                    if (*s == ':') s = end;
                    keys[nkeys].state = 0;
                    nkeys++;
                    if (*s == ';') s++;
                }
            }
            for (k = 0; k < nkeys; k++) {
                if (keys[k].state == 0 && ms >= keys[k].ms) {
                    xbox_InputKeySet(keys[k].vk, 1); keys[k].state = 1;
                    if (s_trace) fprintf(stderr, "[MOUSE] key %c down\n", keys[k].vk);
                } else if (keys[k].state == 1 && ms >= keys[k].ms + keys[k].hold) {
                    xbox_InputKeySet(keys[k].vk, 0); keys[k].state = 2;
                }
            }
        }
        if (getenv("AVP_MOUSE_PASSIVE"))
            return;
        while (s_script_i + 1 < s_script_n && s_script[s_script_i + 1].ms <= ms)
            s_script_i++;
        focus = s_script[0].ms <= ms;
        if (focus) {
            fx = s_script[s_script_i].x;
            fy = s_script[s_script_i].y;
            b = s_script[s_script_i].b;
            if (last_step != s_script_i) {
                wheel = s_script[s_script_i].w;
                last_step = s_script_i;
                if (s_trace)
                    fprintf(stderr, "[MOUSE] script %u ms: %d,%d buttons %d wheel %d\n",
                            (unsigned)ms, fx, fy, b, wheel);
            }
        }
    } else {
        focus = xbox_FramebufferMouse(&fx, &fy);
        if (focus) {
            b = (xbox_FramebufferKeyDown(VK_LBUTTON) ? 1 : 0)
              | (xbox_FramebufferKeyDown(VK_RBUTTON) ? 2 : 0);
            wheel = xbox_FramebufferMouseWheel();
        }
    }
    fxf = fx;
    fx -= avp_wide_inset();

    {
        static int ctl_prev, ctl_fix;
        if (cmd[C_PPOS].owner && avp_input_controller()) {
            uint32_t cptr = G32(AVP_POINTER);
            if (!ctl_prev)
                ctl_fix = 1;
            ctl_prev = 1;
            if (ctl_fix && cptr) {
                uint32_t a[2] = { 1, 0 };
                if (G32(cptr + AVP_PTR_MODE) != 1) {
                    s_call_edi = cptr;
                    gcall(AVP_PTR_SETMODE, 0, a, 2);
                }
                ctl_fix = 0;
            }
            xbox_FramebufferHideCursor(1);
            xbox_FramebufferSetCursor(0, 0, 0, 0);
            return;
        }
        ctl_prev = 0;
    }

    hud = HUD_NONE;
    if (focus && cmd[C_PPOS].owner) {

        static const struct { int l, t, r, b; } map_rect[3] = {
            { 452, 300, 640, 480 },
            { 452, 300, 600, 450 },
            { 465, 319, 592, 427 },
        };
        int r;
        race_color(G32(AVP_POINTER));
        r = s_race < 0 ? 0 : s_race;

        if (r == 0 && fx >= 372 && fx < 446 && fy >= 356 && fy < 448)
            hud = HUD_SHOP;
        else if (fx >= map_rect[r].l && fx < map_rect[r].r
                 && fy >= map_rect[r].t && fy < map_rect[r].b)
            hud = HUD_MAP;
    }

    {

        static int recent;
        if (cmd[C_PPOS].owner || cmd[C_MPOS].owner)
            recent = 30;
        else if (recent > 0)
            recent--;
        xbox_FramebufferHideCursor(recent > 0);
    }
    xbox_FramebufferSetCursor(focus && hud != HUD_NONE, fxf, fy,
                              hud != HUD_NONE ? race_color(G32(AVP_POINTER)) : 0xFFFFFF);
    if (focus && cmd[C_PPOS].owner) {

        G8(AVP_EDGE_CALC) = 0;
        GF(AVP_EDGE_X_LO) = -1.0e9f; GF(AVP_EDGE_X_HI) = 1.0e9f;
        GF(AVP_EDGE_Y_LO) = -1.0e9f; GF(AVP_EDGE_Y_HI) = 1.0e9f;
    }
    s_hover_g = focus && cmd[C_PPOS].owner && !g_avp_mainmenu ? group_at(fx, fy) : 0;
    if (!focus)
        return;
    if (fxf < 0 || fy < 0 || fxf >= avp_wide_width() || fy >= FRAME_H) {
        b = 0;
    }

    vpp = ((int32_t)G32(AVP_VIEWPORTS + 4) - 1 < 0) ? AVP_VIEWPORTS + 0xC : G32(AVP_VIEWPORTS);
    vp = vpp ? G32(vpp) : 0;
    if (!vp)
        return;
    left = (float)(int32_t)G32(vp + 4);  top = (float)(int32_t)G32(vp + 8);
    right = (float)(int32_t)G32(vp + 0xC); bottom = (float)(int32_t)G32(vp + 0x10);
    if (right <= left || bottom <= top)
        return;
    {
        int fw = avp_wide_width();
        int cx = fxf < 0 ? 0 : fxf >= fw ? fw - 1 : fxf;
        int cy = fy < 0 ? 0 : fy >= FRAME_H ? FRAME_H - 1 : fy;
        if ((int)(right - left) == fw) {
            tx = left + ((float)cx + 0.5f) * (right - left) / (float)fw;
        } else {
            int lx = cx - avp_wide_inset();
            lx = lx < 0 ? 0 : lx >= FRAME_W ? FRAME_W - 1 : lx;
            tx = left + ((float)lx + 0.5f) * (right - left) / FRAME_W;
        }
        ty = top + ((float)cy + 0.5f) * (bottom - top) / FRAME_H;
    }

    args = scratch(32);

    for (pass = 0; pass < 2; pass++) {
        uint32_t owner = cmd[C_PPOS].owner ? cmd[C_PPOS].owner : cmd[C_MPOS].owner;
        if (pass == 1) {
            if (!camera_pan(cmd, fxf, fy, args))
                break;
            last_x = -100000;
        } else if (owner && owner == cmd[C_PPOS].owner && owner == G32(AVP_POINTER)
                   && G32(owner + AVP_PTR_MODE) != 2) {

            last_x = -100000;
        }

        if (owner && !(owner == cmd[C_PPOS].owner && hud != HUD_NONE)
            && (fx != last_x || fy != last_y || owner != last_owner)) {
            uint32_t vt = G32(owner);
            if (owner == cmd[C_PPOS].owner) {
                uint32_t pos, ptr = G32(AVP_POINTER);
                float scale = GF(AVP_PTR_SCALE);
                if (ptr == owner)
                    pointer_free(ptr);
                pos = gcall(G32(vt + 0x4C), owner, NULL, 0);
                if (s_trace && (!pos || scale == 0.0f)) {
                    static int told;
                    if (told++ < 8)
                        fprintf(stderr, "[MOUSE] pointer %08X (vt %08X): position %08X, scale %g\n",
                                owner, vt, pos, scale);
                }
                if (pos && scale != 0.0f) {

                    float g = G32(vt + 0xD0) == 0x0002EBE0u ? 2.0f : 1.0f;
                    int step;
                    for (step = 0; step < 2; step++) {
                        float px = GF(pos), py = GF(pos + 4);
                        float dx = (tx - px) / (scale * g), dy = (ty - py) / (scale * g);
                        if (step && fabsf(tx - px) < 1.0f && fabsf(ty - py) < 1.0f)
                            break;
                        G32(args) = f2u(dx); G32(args + 4) = f2u(dy); G32(args + 8) = 0;
                        gcall(G32(vt + 0xD0), owner, &args, 1);
                        pos = gcall(G32(vt + 0x4C), owner, NULL, 0);
                        if (!pos)
                            break;
                    }
                    if (s_trace && pass == 0)
                        fprintf(stderr, "[MOUSE] pointer %08X to %.0f,%.0f -> now %.1f,%.1f"
                                        " (mode %u drag %u)\n",
                                owner, tx, ty, pos ? GF(pos) : -1.0f, pos ? GF(pos + 4) : -1.0f,
                                G32(owner + AVP_PTR_MODE), G8(owner + 0x1C0));
                }
            } else {
                G32(args) = f2u(tx); G32(args + 4) = f2u(ty); G32(args + 8) = 0;
                gcall(G32(vt + 0xD4), owner, &args, 1);
                if (s_trace && owner != last_owner)
                    fprintf(stderr, "[MOUSE] menu pointer %08X to %.0f,%.0f\n", owner, tx, ty);
            }
            last_x = fx; last_y = fy; last_owner = owner;
        }
    }

    {
        uint32_t fm = cmd[C_MPOS].owner || cmd[C_PPOS].owner ? 0 : focus_manager(mgr, mode);
        static int hx = -100000, hy = -100000;
        static uint32_t pending_select;
        if (!fm && (s_nav_target || s_nav_hold)) {

            if (s_nav_hold)
                avp_pad_set(s_nav_key, 0);
            s_nav_target = 0; s_nav_hold = s_nav_gap = 0; pending_select = 0;
        }

        if (fm && getenv("AVP_FM_TRACE")) {
            static uint32_t snap[0x200], snap_fm;
            static int lines;
            uint32_t i;
            if (snap_fm == fm) {
                for (i = 0; i < 0x200 && lines < 400; i++) {
                    uint32_t v = G32(fm + i * 4);
                    if (v != snap[i]) {
                        fprintf(stderr, "[MOUSE] fm %08X +%03X: %08X -> %08X\n",
                                fm, i * 4, snap[i], v);
                        lines++;
                    }
                }
            } else if (lines < 400) {
                char cb[48];
                fprintf(stderr, "[MOUSE] fm now %08X (%s)\n", fm, class_name(fm, cb, sizeof cb));
                lines++;
            }
            for (i = 0; i < 0x200; i++)
                snap[i] = G32(fm + i * 4);
            snap_fm = fm;
        }

        if (fm && s_script_n) {
            static const char *keys = (const char *)-1;
            static uint32_t at_fm;
            static int held;
            char cb[48];
            if (keys == (const char *)-1)
                keys = getenv("AVP_MENU_KEYS");
            if (keys) {
                const char *p;
                class_name(fm, cb, sizeof cb);
                if (fm != at_fm) { at_fm = fm; held = 0; }
                held++;
                for (p = strstr(keys, cb); p && ((p > keys && p[-1] != ';') || p[strlen(cb)] != '=');
                     p = strstr(p + 1, cb))
                    ;
                if (held >= 90 && (held - 90) % 180 == 0 && !s_nav_hold && p && *cb) {
                    if (s_trace) fprintf(stderr, "[MOUSE] menu key %c on %s\n", p[strlen(cb) + 1], cb);
                    int act = menu_key_action((unsigned char)p[strlen(cb) + 1]);
                    if (act >= 0)
                        nav_key_tap(act);
                }
            }
        }

        {
            static uint32_t cls_fm;
            static int is_main;
            if (fm != cls_fm) {
                char cb[48];
                cls_fm = fm;
                is_main = fm && !strcmp(class_name(fm, cb, sizeof cb), "GMainMenu");
            }
            InterlockedExchange(&g_avp_mainmenu, is_main);
            InterlockedExchange(&g_avp_mainmenu_obj, is_main ? (LONG)fm : 0);
            if (fm && g_avp_menu_sound) {

                LONG which = InterlockedExchange(&g_avp_menu_sound, 0);
                uint32_t fh = G32(fm + AVP_FOCUSED);
                item_sound(fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0,
                           which == 2 ? AVP_ITEM_ACTIVATED_SND : AVP_ITEM_FOCUSED_SND);
            }
            if (is_main && g_avp_menu_req >= 0) {
                LONG want = InterlockedExchange(&g_avp_menu_req, -1);
                pcmenu_select(mgr, mode, fm, cmd, (int)want);
                last_b = b;
                return;
            }
            if (is_main && g_avp_pcmenu_up) {
                last_b = b;
                return;
            }
        }
        if (fm) {
            int press = (b & 1) && !(last_b & 1);
            uint32_t item = 0, blk = mgr + 0x9C, one = 1;
            if (nav_run(fm, mgr, mode) && pending_select) {
                uint32_t fh = G32(fm + AVP_FOCUSED);
                uint32_t cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
                uint32_t sel = select_button(mgr, mode);
                if (s_trace)
                    fprintf(stderr, "[MOUSE] select item %08X after the focus walk (focus %08X) via %08X\n",
                            pending_select, cur, sel);
                if (cur == pending_select && sel)
                    nav_key_tap(AVP_ACT_A);
                pending_select = 0;
            }
            if (fx != hx || fy != hy || press)
                item = menu_item_at(tx, ty, NULL);
            hx = fx; hy = fy;
            if (press && !item) {

                int n = 0;
                uint32_t only = menu_item_at(0, 0, &n);
                if (n == 1)
                    item = only;
                if (s_trace) {
                    uint32_t end = G32(AVP_REGIONS_END), e;
                    fprintf(stderr, "[MOUSE] click on no item; %d items on screen\n", n);
                    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20) {
                        uint32_t obj = G32(e) ? G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u) : 0, a[5];
                        uint32_t li = 0, mi = 0;
                        if (obj) {
                            a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_MENUITEM; a[4] = 0;
                            mi = gcall(AVP_DYNCAST, 0, a, 5);
                            a[3] = AVP_TD_LISTBOX;
                            li = gcall(AVP_DYNCAST, 0, a, 5);
                        }
                        fprintf(stderr, "[MOUSE]   region %08X obj %08X vt %08X %d,%d-%d,%d%s%s\n",
                                G32(e), obj, obj ? G32(obj) : 0,
                                (int)G32(e + 4), (int)G32(e + 8), (int)G32(e + 12), (int)G32(e + 16),
                                mi ? " item" : "", li ? " listbox" : "");
                    }
                }
                if (n == 0 && !back_frames) {

                    if (s_trace) fprintf(stderr, "[MOUSE] click as START\n");
                    avp_pad_set(AVP_ACT_START, 1);
                    back_frames = 4;
                    last_b = b;
                    return;
                }
            }
            if (press && s_trace)
                fprintf(stderr, "[MOUSE] menu click at %.0f,%.0f: item %08X, focus manager %08X"
                                " (focused handle %08X), select %08X\n", tx, ty, item, fm,
                        G32(fm + AVP_FOCUSED), cmd[C_FSEL].owner);
            if (item) {
                uint32_t fh = G32(fm + AVP_FOCUSED);
                uint32_t focused = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
                if (focused != item) {
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] focus item %08X (was %08X)\n", item, focused);

                    if (cmd[C_FSEL].owner) {
                        gcall(G32(G32(item) + 0xB0), item, &one, 1);

                        if (G32(G32(item) + 0xB0) != 0x000E4340u)
                            item_sound(item, AVP_ITEM_FOCUSED_SND);
                        if (s_trace)
                            fprintf(stderr, "[MOUSE] focus method %08X\n", G32(G32(item) + 0xB0));
                    } else
                        nav_start(item);
                }
            }
            if (press) {
                last_b = b;
                if (item && bound_release(mgr, mode, item)) {

                    int act = button_key(mgr, mode, item);
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] tap button %08X (action %d)\n", item, act);
                    if (act >= 0) {
                        nav_key_tap(act);
                    } else {
                        gcall(AVP_BTN_PRESS, item, &blk, 1);
                        gcall(AVP_BTN_RELEASE, item, &blk, 1);
                    }
                } else if (item && cmd[C_FSEL].owner) {
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] select item %08X\n", item);

                    item_sound(item, AVP_ITEM_ACTIVATED_SND);
                    gcall(cmd[C_FSEL].handler, cmd[C_FSEL].owner, &blk, 1);
                } else if (item) {

                    uint32_t sel = select_button(mgr, mode);
                    uint32_t fh = G32(fm + AVP_FOCUSED);
                    uint32_t cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
                    if (cur == item && sel) {
                        if (s_trace)
                            fprintf(stderr, "[MOUSE] select item %08X (A, as the pad)\n", item);
                        nav_key_tap(AVP_ACT_A);
                    } else if (sel) {
                        nav_start(item);
                        pending_select = item;
                    }
                }
                return;
            }
        }
    }

    if (cmd[C_PPOS].owner && avp_editor_active() && avp_editor_frame(fx, fy, b, wheel)) {
        last_b = b;
        return;
    }

    {
        uint32_t blk = mgr + 0x9C;
        int down = b & ~last_b, up = last_b & ~b, deferred;
        last_b = b;
        frame_no++;
        if (fx != mv_fx || fy != mv_fy) {
            mv_fx = fx; mv_fy = fy;
            mv_frame = frame_no;
        }
        deferred = rc_defer;
        rc_defer = 0;
        if (deferred)
            up |= 2;
        if (s_trace) {
            if (down & 2)
                rclick_log("down", 1, hud != HUD_NONE, back_frames, deferred, cmd);
            if (up & 2)
                rclick_log(deferred ? "up (deferred)" : "up", 1, hud != HUD_NONE, back_frames, deferred, cmd);
            if (rc_left > 0) {
                rc_left--;
                rclick_log("frame", 0, 0, 0, 0, cmd);
            }
            if ((down | up) & 2)
                rc_left = 10;
        }

        if (bar_press) {
            if (!(b & 1))
                bar_press = 0;
            return;
        }
        if ((down & 1) && !drag_on && cmd[C_PPOS].owner && !g_avp_mainmenu && G32(AVP_POINTER)
            && (c = group_at(fx, fy)) > 0) {
            group_use(G32(AVP_POINTER), c);
            bar_press = 1;
            return;
        }

        if (hud != HUD_NONE && !drag_on) {
            if ((down & 1) && hud == HUD_SHOP && !back_frames) {
                const char *k = getenv("AVP_SHOP_KEY");
                shop_act = k && k[0] ? menu_key_action(k[0]) : -1;
                if (shop_act < 0)
                    shop_act = AVP_ACT_BLACK;
                if (s_trace) fprintf(stderr, "[MOUSE] shop click (action %d)\n", shop_act);
                avp_pad_set(shop_act, 1);
                back_frames = 4;
            }
            if (hud == HUD_MAP && (b & 1))
                minimap_click(fx, fy);
            if (down || up)
                return;
        }

        if ((down & 1) && cmd[C_B1ON].owner && !cmd[C_MACT].owner) {
            xbox_FramebufferSetBoxColor(race_color(G32(AVP_POINTER)));
            drag_on = 1; drag_box = 0;
            drag_fx = fx; drag_fy = fy; drag_tx = tx; drag_ty = ty;
        }
        if (drag_on && (b & 1)) {
            if (!drag_box && (abs(fx - drag_fx) > 6 || abs(fy - drag_fy) > 6))
                drag_box = 1;
            if (drag_box)
                xbox_FramebufferSetBox(1, drag_fx + avp_wide_inset(), drag_fy, fx + avp_wide_inset(), fy);
        }
        if (drag_on && !(b & 1)) {

            int was_box = drag_box;
            drag_on = drag_box = 0;
            xbox_FramebufferSetBox(0, 0, 0, 0, 0);
            if (cmd[C_B1OFF].owner) {
                if (!getenv("AVP_GAME_DBLCLICK")) {
                    static ULONGLONG last_ms;
                    static uint32_t last_handle;
                    static int last_fx, last_fy;
                    uint32_t cptr = G32(AVP_POINTER);
                    uint32_t h = 0;
                    ULONGLONG cnow = GetTickCount64();
                    int genuine;
                    if (cptr && (int32_t)G32(cptr + 0x220) > 0)
                        h = G32(G32(cptr + 0x21C));
                    genuine = !was_box && h && h == last_handle && cnow - last_ms <= 400 &&
                              abs(fx - last_fx) <= 6 && abs(fy - last_fy) <= 6;
                    if (cptr) {
                        if (genuine)
                            GF(cptr + 0x1D4) = GF(AVP_GAME_TIME);
                        else
                            G32(cptr + 0x1D4) = 0;
                    }
                    if (genuine) {
                        last_handle = 0;
                    } else {
                        last_handle = h;
                        last_ms = cnow;
                        last_fx = fx;
                        last_fy = fy;
                    }
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] click %s (handle %08X)\n", genuine ? "double" : "single", h);
                }
                if (s_trace) fprintf(stderr, "[MOUSE] button 1 up%s\n", was_box ? " (box)" : "");
                gcall(cmd[C_B1OFF].handler, cmd[C_B1OFF].owner, &blk, 1);
            }
            if (was_box)
                box_select(G32(AVP_POINTER), drag_tx, drag_ty, tx, ty);
            return;
        }
        if (down & 1) {
            if (cmd[C_MACT].owner) {
                if (s_trace) fprintf(stderr, "[MOUSE] menu click\n");
                gcall(cmd[C_MACT].handler, cmd[C_MACT].owner, &blk, 1);
                return;
            }
            if (cmd[C_B1ON].owner) {
                uint32_t ptr = G32(AVP_POINTER);
                pointer_free(ptr);
                gcall(cmd[C_B1ON].handler, cmd[C_B1ON].owner, &blk, 1);
                if (getenv("AVP_TAP_PROBE") && ptr) {
                    uint32_t o;
                    float now = GF(AVP_GAME_TIME);
                    fprintf(stderr, "[TAP] clock %g:", (double)now);
                    for (o = 0; o < 0xE00; o += 4)
                        if (GF(ptr + o) == now) fprintf(stderr, " +%X", o);
                    fprintf(stderr, "\n");
                }
                if (s_trace)
                    fprintf(stderr, "[MOUSE] button 1 down: owner %08X pointer %08X mode %u drag %u\n",
                            cmd[C_B1ON].owner, ptr, ptr ? G32(ptr + AVP_PTR_MODE) : 0,
                            ptr ? G8(ptr + 0x1C0) : 0);
                return;
            }
            if (!cmd[C_MPOS].owner && !cmd[C_PPOS].owner && cmd[C_FSEL].owner) {

                uint32_t ph = press_handler(mgr, mode, cmd[C_FSEL].owner);
                if (s_trace) fprintf(stderr, "[MOUSE] click as A (FrontEndSelect)\n");
                if (ph) {
                    gcall(ph, cmd[C_FSEL].owner, &blk, 1);
                    sel_owner = cmd[C_FSEL].owner;
                } else {
                    gcall(cmd[C_FSEL].handler, cmd[C_FSEL].owner, &blk, 1);
                }
                return;
            }
            if (!cmd[C_MPOS].owner && !cmd[C_PPOS].owner && !back_frames) {

                if (s_trace) fprintf(stderr, "[MOUSE] click as START\n");
                avp_pad_set(AVP_ACT_START, 1);
                back_frames = 4;
            }
        }
        if ((up & 1) && cmd[C_B1OFF].owner && !cmd[C_MACT].owner) {
            if (s_trace) fprintf(stderr, "[MOUSE] button 1 up\n");
            gcall(cmd[C_B1OFF].handler, cmd[C_B1OFF].owner, &blk, 1);
            return;
        }
        if ((up & 1) && sel_owner) {
            if (cmd[C_FSEL].owner == sel_owner) {
                if (s_trace) fprintf(stderr, "[MOUSE] A released (FrontEndSelect)\n");
                gcall(cmd[C_FSEL].handler, cmd[C_FSEL].owner, &blk, 1);
            }
            sel_owner = 0;
            return;
        }
        if ((up & 2) && back_owner) {

            if (cmd[C_FBACK].owner == back_owner) {
                if (s_trace) fprintf(stderr, "[MOUSE] back released (FrontEndBack)\n");
                gcall(cmd[C_FBACK].handler, cmd[C_FBACK].owner, &blk, 1);
            }
            back_owner = 0;
            return;
        }
        if (down & 2) {
            if (cmd[C_FBACK].owner) {
                uint32_t ph = press_handler(mgr, mode, cmd[C_FBACK].owner);
                if (s_trace) fprintf(stderr, "[MOUSE] back pressed (FrontEndBack%s)\n",
                                     ph ? "" : ", no press entry");
                if (ph) {
                    gcall(ph, cmd[C_FBACK].owner, &blk, 1);
                    back_owner = cmd[C_FBACK].owner;
                } else {
                    gcall(cmd[C_FBACK].handler, cmd[C_FBACK].owner, &blk, 1);
                }
                return;
            } else if (cmd[C_MACT].owner || (!cmd[C_B2ON].owner && !cmd[C_B2OFF].owner)) {
                if (s_trace) fprintf(stderr, "[MOUSE] back (B)\n");
                avp_pad_set(AVP_ACT_B, 1);
                back_frames = 4;
            } else if (!getenv("AVP_RCLICK_OLD")) {
                if (!getenv("AVP_RCLICK_HOLD")) {
                    s_rclick_armed = 1;
                    if (s_trace) fprintf(stderr, "[RCLICK] down branch: armed\n");
                    return;
                }
                if (!back_frames) {
                    if (s_trace) fprintf(stderr, "[MOUSE] order (B)\n");
                    avp_pad_set(AVP_ACT_B, 1);
                    back_frames = 4;
                }
                if (getenv("AVP_RCLICK_HOLD"))
                    s_rclick_b = 1;
                return;
            } else if (cmd[C_B2ON].owner) {
                if (s_trace) fprintf(stderr, "[MOUSE] button 2 down\n");
                gcall(cmd[C_B2ON].handler, cmd[C_B2ON].owner, &blk, 1);
                return;
            }
        }
        if ((up & 2) && s_rclick_b) {
            if (s_trace) fprintf(stderr, "[MOUSE] order (B) up\n");
            avp_pad_set(AVP_ACT_B, 0);
            s_rclick_b = 0;
            return;
        }
        if ((up & 2) && cmd[C_B2OFF].owner && !cmd[C_MACT].owner) {
            uint32_t wpos[3];
            int n = 1;
            if (!getenv("AVP_RCLICK_OLD") && !getenv("AVP_RCLICK_HOLD")) {
                if (!deferred && frame_no - mv_frame <= 1) {
                    rc_defer = 1;
                    if (s_trace) fprintf(stderr, "[RCLICK] up branch: deferred one frame (cursor moved)\n");
                    return;
                }
                uint32_t ptr = G32(AVP_POINTER), hud = G32(AVP_HUD_GROUP);
                int32_t state = 0;
                uint32_t target = 0;
                int shifted = hud && G8(hud + 0x2A4);
                s_rclick_armed = 0;
                n = 0;
                if (ptr) {
                    uint32_t sel = G32(ptr + 0x1B8);
                    n = sel ? (int32_t)G32(sel + 4) : 0;
                    state = (int32_t)G32(ptr + 0x1C4);
                    target = G32(ptr + 0xDC);
                }
                if (n > 0 && target && state != 0 && state != 2 && state != 6) {
                    const char *how;
                    uint32_t menu = find_avp_menu(state, &how), item = menu_page(menu, state);
                    if (s_trace) {
                        uint32_t hs = G32(AVP_HUD_GROUP) ? G32(G32(AVP_HUD_GROUP) + AVP_HUD_SHOWN) : 0;
                        fprintf(stderr, "[RCLICK] menu lookup: %s, menu %08X (hud shown %08X vtable %08X) pages %d\n",
                                how, menu, hs, hs ? G32(hs) : 0, menu ? (int)G32(menu + 0x2A0) : 0);
                    }
                    if (s_trace && item) {
                        char nb[64], hb[64];
                        fprintf(stderr, "[RCLICK] context page %d item %08X id %s hotkey %s state %08X menu %08X page %d\n",
                                state, item, name_of(G32(item + AVP_ITEM_ID), nb, sizeof nb),
                                name_of(G32(item + AVP_ITEM_HOTKEY), hb, sizeof hb),
                                G32(item + AVP_ITEM_STATE), menu, (int)G32(menu + 0x290));
                    }
                    if (item && context_page_handled(item)) {
                        uint32_t ia = item;
                        uint32_t ret = gcall(AVP_MENU_ITEM_ACT, menu, &ia, 1);
                        fprintf(stderr, "[MOUSE] order (context page %d item %08X)\n", state, item);
                        if (s_trace)
                            fprintf(stderr, "[RCLICK] up branch: context, ItemActivated returned %08X\n", ret);
                        return;
                    }
                    fprintf(stderr, "[MOUSE] order (context page %d: %s, moving instead)\n", state,
                            item ? "no order on that page" : "no menu item");
                }
                if (shifted && n > 0) {
                    uint32_t nv[3] = { G32(0x00304EDCu), G32(0x00304EE0u), G32(0x00304EE4u) };
                    gcall(0x0002D990u, ptr, nv, 3);
                    wpos[0] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND);
                    wpos[1] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 4);
                    wpos[2] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 8);
                    move_marker(wpos, fx, fy);
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] order (attack-move)\n");
                    if (s_trace)
                        fprintf(stderr, "[RCLICK] up branch: attack-move\n");
                    return;
                }
            }
            wpos[0] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND);
            wpos[1] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 4);
            wpos[2] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 8);
            if (s_trace) fprintf(stderr, "[MOUSE] button 2 up\n");
            gcall(cmd[C_B2OFF].handler, cmd[C_B2OFF].owner, &blk, 1);
            if (s_trace)
                fprintf(stderr, "[RCLICK] up branch: plain move (B2OFF handler), %d selected\n", n);
            if (n > 0)
                move_marker(wpos, fx, fy);
            return;
        }
        if (s_trace && (up & 2))
            fprintf(stderr, "[RCLICK] up not handled: B2OFF owner %08X MACT owner %08X\n",
                    cmd[C_B2OFF].owner, cmd[C_MACT].owner);
    }

    if (wheel) {
        if ((wheel > 0) != (zoom_dir != 0))
            zoom_left = 0;
        zoom_dir = wheel > 0 ? 1 : 0;
        zoom_left += 16 * (wheel > 0 ? wheel : -wheel);
        if (zoom_left > 96)
            zoom_left = 96;
    }

    if (zoom_left > 0) {
        zoom_left--;
        if (s_trace && (zoom_left % 6) == 5)
            fprintf(stderr, "[MOUSE] zoom %s%s\n", zoom_dir ? "in" : "out",
                    cmd[C_ZOOM].owner ? "" : " (no CameraZoom active)");
        if (cmd[C_ZOOM].owner) {
            uint32_t cam = cmd[C_ZOOM].owner;
            float ox = GF(cam + 0x13C), oy = GF(cam + 0x140), oz = GF(cam + 0x144);
            float dist = sqrtf(ox * ox + oy * oy + oz * oz);
            float lo = GF(0x00206FE0u), hi = GF(0x00206FE4u);
            int ok = !(hi > lo && lo > 0.0f) || (zoom_dir ? dist > lo : dist < hi);

            for (c = 0; ok && c < 4; c++) {
                G32(args) = 0; G32(args + 4) = (uint32_t)zoom_dir;
                gcall(cmd[C_ZOOM].handler, cam, &args, 1);
                ox = GF(cam + 0x13C); oy = GF(cam + 0x140); oz = GF(cam + 0x144);
                dist = sqrtf(ox * ox + oy * oy + oz * oz);
                ok = !(hi > lo && lo > 0.0f) || (zoom_dir ? dist > lo : dist < hi);
            }
            if (s_trace && (zoom_left % 6) == 5)
                fprintf(stderr, "[MOUSE]   camera distance %.1f (limits %.1f..%.1f)\n", dist, lo, hi);
        }
    }

}

void avp_mouse_frame(uint32_t mgr)
{
    avp_mouse_frame_body(mgr);
    coop_marks_snap();
    avp_wide_narrow();
}

