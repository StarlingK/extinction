#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp, g_ebp, g_eflags;
extern RECOMP_TLS int g_fp_top;
extern ptrdiff_t g_xbox_mem_offset;

typedef void (*avp_guest_fn)(void);
extern avp_guest_fn recomp_lookup(uint32_t xbox_va);

extern int  xbox_FramebufferMouse(int *fx, int *fy);
extern int  xbox_FramebufferMouseWheel(void);
extern void xbox_FramebufferHideCursor(int hide);
extern void xbox_FramebufferSetBox(int on, int x0, int y0, int x1, int y1);
extern void xbox_FramebufferSetBoxColor(uint32_t rgb);
extern void xbox_FramebufferSetCursor(int on, int x, int y, uint32_t rgb);
extern int  xbox_FramebufferKeyDown(int vk);
extern void xbox_InputKeySet(int vk, int down);
extern uint64_t avp_coop_now_ms(void);
extern void avp_coop_mission_start(void);
extern int avp_coop_give_selection(void);

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

static int s_rclick_b;

static int rclick_key(void)
{
    const char *k = getenv("AVP_RCLICK_KEY");
    return (k && k[0]) ? (k[0] >= 'a' && k[0] <= 'z' ? k[0] - 32 : k[0]) : 'X';
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

static void nav_key_tap(int vk)
{
    if (s_nav_hold > 0)
        xbox_InputKeySet(s_nav_key, 0);
    s_nav_key = vk;
    xbox_InputKeySet(vk, 1);
    s_nav_hold = NAV_HOLD;
}

static int button_key(uint32_t mgr, uint32_t mode, uint32_t obj)
{
    static const struct { const char *name; int vk; } map[] = {
        { "Right_BottomClusterButton_Press", 'Z' }, { "Right_RightClusterButton_Press", 'X' },
        { "Right_LeftClusterButton_Press", 'C' },   { "Right_TopClusterButton_Press", 'V' },
        { "RightTopShoulderButton_Press", 'E' },    { "LeftTopShoulderButton_Press", 'Q' },
        { "LeftBottomShoulderButton_Press", '1' },  { "RightBottomShoulderButton_Press", '3' },
        { "Left_TopClusterButton_Press", VK_UP },   { "Left_BottomClusterButton_Press", VK_DOWN },
        { "Left_LeftClusterButton_Press", VK_LEFT }, { "Left_RightClusterButton_Press", VK_RIGHT },
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
                return map[j].vk;
        if (s_trace) {
            char nb[64];
            fprintf(stderr, "[MOUSE] button %08X: press entry %s has no key\n",
                    obj, name_of(G32(trig + 8), nb, sizeof nb));
        }
    }
    return 0;
}

static int nav_run(uint32_t fm)
{
    uint32_t fh, cur;
    int il, it, ir, ib, cl, ct, cr, cb, dx, dy;

    if (s_nav_hold > 0) {
        if (--s_nav_hold == 0) {
            xbox_InputKeySet(s_nav_key, 0);
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
    s_nav_key = abs(dy) >= abs(dx) ? (dy < 0 ? VK_UP : VK_DOWN) : (dx < 0 ? VK_LEFT : VK_RIGHT);
    xbox_InputKeySet(s_nav_key, 1);
    s_nav_hold = NAV_HOLD;
    s_nav_steps++;
    if (s_trace)
        fprintf(stderr, "[MOUSE] focus walk step %d: key %02X (focus %08X, item +334 %08X, want %08X)\n",
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
#define AVP_MARKER_SPAWN  0x0002EE80u
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

static float band(float d, float lo, float hi)
{
    float a = d - lo + 0.75f, b = hi - d + 0.75f;
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    b = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    return a < b ? a : b;
}

extern int avp_coop_marked(uint32_t handle);

static void coop_marks(uint32_t *rgb, int w, int h)
{
    uint32_t end = G32(AVP_REGIONS_END), e;
    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20) {
        int cx, cy, dx, dy;
        if (!G32(e) || !avp_coop_marked(G32(e)))
            continue;
        cx = ((int)G32(e + 4) + (int)G32(e + 12)) / 2;
        cy = (int)G32(e + 8) - 5;
        for (dy = -3; dy <= 3; dy++)
            for (dx = -3; dx <= 3; dx++) {
                int ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy), x = cx + dx, y = cy + dy;
                if (ad > 3 || x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                rgb[y * w + x] = ad == 3 ? 0x00101010u : 0x00FFD040u;
            }
    }
}

void avp_overlay_draw(uint32_t *rgb, int w, int h)
{
    coop_marks(rgb, w, h);
    static const float kx = 33.6f, ky = 25.0f, squash = 0.72f;
    static int off = -1;
    ULONGLONG now = GetTickCount64();
    float cx, cy;
    int have_cam, i;
    if (off < 0)
        off = getenv("AVP_OVERLAY_MARKER") == NULL;
    if (off)
        return;
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
        ro = 15.0f * s;
        ri = 9.0f * s;
        inner = blend(s_marks[i].rgb, 0x00FFFFFFu, 0.55f);
        x0 = (int)(px - ro - 2); x1 = (int)(px + ro + 2);
        y0 = (int)(py - ro * squash - 2); y1 = (int)(py + ro * squash + 2);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= w) x1 = w - 1;
        if (y1 >= h) y1 = h - 1;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                float dx = (float)x + 0.5f - px, dy = ((float)y + 0.5f - py) / squash;
                float d = sqrtf(dx * dx + dy * dy);
                uint32_t *p = &rgb[(size_t)y * w + x];
                float a;
                a = band(d, ro - 4.5f * s, ro);
                if (a > 0.0f) *p = blend(*p, 0x00707478u, a * 0.7f * fade);
                a = band(d, ro - 0.8f, ro + 0.4f);
                if (a > 0.0f) *p = blend(*p, 0x00282A2Cu, a * 0.5f * fade);
                a = band(d, ri - 2.2f * s, ri);
                if (a > 0.0f) *p = blend(*p, inner, a * 0.95f * fade);
            }
        }
    }
}

static void move_marker(const uint32_t *pos, int fx, int fy)
{
    static const uint32_t hashes[3] = { 0x7715B5E5u, 0x55EE21EFu, 0x3C905527u };
    static uint32_t made, made_vt;
    uint32_t p = G32(AVP_MARKER_OWNER), m, rgb;
    if (getenv("AVP_NO_MOVE_MARKER"))
        return;
    rgb = race_color(G32(AVP_POINTER));
    marker_add(fx, fy, rgb);
    if (s_trace)
        fprintf(stderr, "[MOUSE] move marker at %d,%d\n", fx, fy);
    if (!p || !getenv("AVP_GAME_MARKER"))
        return;
    if (!(fabsf(u2f(pos[0])) < 1e7f && fabsf(u2f(pos[1])) < 1e7f && fabsf(u2f(pos[2])) < 1e7f))
        return;
    if (!G32(p + 0x264) && s_race >= 0 && s_race < 3)
        G32(p + 0x264) = hashes[s_race];
    if (!G32(p + 0x264))
        return;
    m = G32(p + 0x270);
    if (m && (m != made || G32(m) != made_vt)) {
        G32(p + 0x270) = 0;
        G32(p + 0x274) = 0;
    }
    s_call_esi = p;
    gcall(AVP_MARKER_SPAWN, p, pos, 3);
    made = G32(p + 0x270);
    made_vt = made ? G32(made) : 0;
    if (s_trace)
        fprintf(stderr, "[MOUSE] game marker at %g,%g,%g: %08X\n",
                (double)u2f(pos[0]), (double)u2f(pos[1]), (double)u2f(pos[2]), made);
}

#define AVP_SELECT_UNIT   0x000F0070u
#define AVP_GAME_TIME     0x002F689Cu
#define AVP_HUD_GROUP     0x002F68E8u

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

static int camera_pan(const Cmd *cmd, int fx, int fy, uint32_t args)
{
    static ULONGLONG start;
    static int panning;
    static float lsx, lsy;
    float sx = 0.0f, sy = 0.0f, ramp;
    int inside = fx >= 0 && fy >= 0 && fx < FRAME_W && fy < FRAME_H;

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
    if (inside && !getenv("AVP_NO_EDGE")) {
        sx = edge_push(fx, FRAME_W);
        sy = edge_push(fy, FRAME_H);
    }
    if (xbox_FramebufferKeyDown('A')) sx -= 1.0f;
    if (xbox_FramebufferKeyDown('D')) sx += 1.0f;
    if (xbox_FramebufferKeyDown('W')) sy -= 1.0f;
    if (xbox_FramebufferKeyDown('S')) sy += 1.0f;
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
                    "+148 %08X\n", G8(AVP_CAM_MODE_A), G8(AVP_CAM_MODE_B),
                    cam ? GF(cam + 0x10C) : 0.0f, cam ? GF(cam + 0x110) : 0.0f,
                    cmd[C_GPMOVE].owner ? G32(cmd[C_GPMOVE].owner + 0x148) : 0);
        }
    }
    ramp = (float)(GetTickCount64() - start) / PAN_RAMP_MS;
    ramp = ramp < 0.25f ? 0.25f : ramp > 1.0f ? 1.0f : ramp;
    G32(args) = f2u(sx * ramp); G32(args + 4) = f2u(sy * ramp); G32(args + 8) = 0;
    if (G8(AVP_CAM_MODE_A) || G8(AVP_CAM_MODE_B)) {

        uint32_t ptr = G32(AVP_POINTER);
        s_call_edi = ptr;
        gcall(cmd[C_GPMOVE].handler, cmd[C_GPMOVE].owner, &args, 1);
        pointer_free(ptr);
    } else {

        uint32_t cam = cmd[C_GPMOVE].owner, a[2], map = G32(AVP_MINIMAP);
        G32(cam + 0x148) = 0;
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
    uint32_t end = G32(AVP_REGIONS_END), e, items[16], one = 1, blk = mgr + 0x9C;
    int tops[16], n = 0, i;

    for (e = AVP_REGIONS; end > e && end - AVP_REGIONS < 20u * 8192u && e + 20 <= end; e += 20) {
        uint32_t obj = G32(e) ? G32(AVP_OBJ_TABLE + (G32(e) >> 16) * 4u) : 0, a[5];
        if (!obj) continue;
        for (i = 0; i < n && items[i] != obj; i++)
            ;
        if (i < n || n >= 16) continue;
        a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_MENUITEM; a[4] = 0;
        if (!gcall(AVP_DYNCAST, 0, a, 5)) continue;
        if (s_trace)
            fprintf(stderr, "[PCMENU]   item %08X %d,%d-%d,%d\n", obj, (int)G32(e + 4),
                    (int)G32(e + 8), (int)G32(e + 12), (int)G32(e + 16));

        if ((int)G32(e + 4) < 300 || (int)G32(e + 8) > 420)
            continue;

        for (i = n; i > 0 && tops[i - 1] > (int)G32(e + 8); i--) {
            items[i] = items[i - 1];
            tops[i] = tops[i - 1];
        }
        items[i] = obj;
        tops[i] = (int)G32(e + 8);
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
            nav_key_tap('Z');
    }
    (void)mode;
}

void avp_mouse_frame(uint32_t mgr)
{
    static uint32_t last_mode = 0xFFFFFFFFu, last_n = 0xFFFFFFFFu;
    static int last_x = -100000, last_y = -100000, last_b, zoom_left, zoom_dir;
    static int back_frames, shop_key;
    static uint32_t back_owner;
    static uint32_t sel_owner;
    static int drag_on, drag_box, drag_fx, drag_fy;
    static float drag_tx, drag_ty;
    static ULONGLONG t0;
    static uint32_t last_owner;
    Cmd cmd[C_N];
    uint32_t mode, vpp, vp, args;
    int fx = 0, fy = 0, b = 0, wheel = 0, focus, c, pass;
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
    if (!s_on || mgr != G32(AVP_INPUT_MGR) || !mgr)
        return;

    if (back_frames > 0 && --back_frames == 0) {
        xbox_InputKeySet('X', 0);
        xbox_InputKeySet(VK_RETURN, 0);
        xbox_InputKeySet('E', 0);
        if (shop_key)
            xbox_InputKeySet(shop_key, 0);
    }

    mode = G32(mgr + 0x94);
    if (s_trace && (mode != last_mode || G32(mgr + 0x4C) != last_n)) {
        trace_list(mgr, mode);
        last_mode = mode;
        last_n = G32(mgr + 0x4C);
    }
    find_cmds(mgr, mode, cmd);
    if (s_ncmds)
        cmd_script_run(mgr, mode, (uint32_t)(avp_coop_now_ms() - t0));

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
        static int adv_key = 'Z';
        if (adv_hold > 0 && --adv_hold == 0)
            xbox_InputKeySet(adv_key, 0);
        if (adv && !adv_hold && !cmd[C_PPOS].owner && !mission_started && now - adv_last > 2500) {
            adv_last = now;
            adv_key = adv_key == 'Z' ? VK_RETURN : 'Z';
            xbox_InputKeySet(adv_key, 1);
            adv_hold = 8;
            if (s_trace) fprintf(stderr, "[MOUSE] auto advance\n");
        }
        static int play_frames;
        play_frames = (cmd[C_B1ON].owner && mode == 1) ? play_frames + 1 : 0;
        {
            static int coop_started, g_was;
            int g_now = xbox_FramebufferKeyDown('G');
            if (!coop_started && play_frames > 60) {
                coop_started = 1;
                avp_coop_mission_start();
            }
            if (coop_started && g_now && !g_was)
                avp_coop_give_selection();
            g_was = g_now;
        }
        if (from_mission && !mission_started && play_frames > 60) {
            mission_started = 1;
            t0 = avp_coop_now_ms();
            s_script_i = 0;
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
    xbox_FramebufferSetCursor(focus && hud != HUD_NONE, fx, fy,
                              hud != HUD_NONE ? race_color(G32(AVP_POINTER)) : 0xFFFFFF);
    if (focus && cmd[C_PPOS].owner) {

        G8(AVP_EDGE_CALC) = 0;
        GF(AVP_EDGE_X_LO) = -1.0e9f; GF(AVP_EDGE_X_HI) = 1.0e9f;
        GF(AVP_EDGE_Y_LO) = -1.0e9f; GF(AVP_EDGE_Y_HI) = 1.0e9f;
    }
    if (!focus)
        return;
    if (fx < 0 || fy < 0 || fx >= FRAME_W || fy >= FRAME_H) {
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
        int cx = fx < 0 ? 0 : fx >= FRAME_W ? FRAME_W - 1 : fx;
        int cy = fy < 0 ? 0 : fy >= FRAME_H ? FRAME_H - 1 : fy;
        tx = left + ((float)cx + 0.5f) * (right - left) / FRAME_W;
        ty = top + ((float)cy + 0.5f) * (bottom - top) / FRAME_H;
    }

    args = scratch(32);

    for (pass = 0; pass < 2; pass++) {
        uint32_t owner = cmd[C_PPOS].owner ? cmd[C_PPOS].owner : cmd[C_MPOS].owner;
        if (pass == 1) {
            if (!camera_pan(cmd, fx, fy, args))
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
                xbox_InputKeySet(s_nav_key, 0);
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
                    nav_key_tap((unsigned char)p[strlen(cb) + 1]);
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
            if (nav_run(fm) && pending_select) {
                uint32_t fh = G32(fm + AVP_FOCUSED);
                uint32_t cur = fh ? G32(AVP_OBJ_TABLE + (fh >> 16) * 4u) : 0;
                uint32_t sel = select_button(mgr, mode);
                if (s_trace)
                    fprintf(stderr, "[MOUSE] select item %08X after the focus walk (focus %08X) via %08X\n",
                            pending_select, cur, sel);
                if (cur == pending_select && sel)
                    nav_key_tap('Z');
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
                    xbox_InputKeySet(VK_RETURN, 1);
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

                    int vk = button_key(mgr, mode, item);
                    if (s_trace)
                        fprintf(stderr, "[MOUSE] tap button %08X (key %02X)\n", item, vk);
                    if (vk) {
                        nav_key_tap(vk);
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
                        nav_key_tap('Z');
                    } else if (sel) {
                        nav_start(item);
                        pending_select = item;
                    }
                }
                return;
            }
        }
    }

    {
        uint32_t blk = mgr + 0x9C;
        int down = b & ~last_b, up = last_b & ~b;
        last_b = b;

        if (hud != HUD_NONE && !drag_on) {
            if ((down & 1) && hud == HUD_SHOP && !back_frames) {
                const char *k = getenv("AVP_SHOP_KEY");
                shop_key = (k && k[0]) ? (k[0] >= 'a' && k[0] <= 'z' ? k[0] - 32 : k[0]) : 'E';
                if (s_trace) fprintf(stderr, "[MOUSE] shop click (key %c)\n", shop_key);
                xbox_InputKeySet(shop_key, 1);
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
                xbox_FramebufferSetBox(1, drag_fx, drag_fy, fx, fy);
        }
        if (drag_on && !(b & 1)) {

            int was_box = drag_box;
            drag_on = drag_box = 0;
            xbox_FramebufferSetBox(0, 0, 0, 0, 0);
            if (cmd[C_B1OFF].owner) {
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
                xbox_InputKeySet(VK_RETURN, 1);
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
                xbox_InputKeySet('X', 1);
                back_frames = 4;
            } else if (!getenv("AVP_RCLICK_OLD")) {

                if (!back_frames) {
                    if (s_trace) fprintf(stderr, "[MOUSE] order (B)\n");
                    xbox_InputKeySet(rclick_key(), 1);
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
            xbox_InputKeySet(rclick_key(), 0);
            s_rclick_b = 0;
            return;
        }
        if ((up & 2) && cmd[C_B2OFF].owner && !cmd[C_MACT].owner) {
            uint32_t wpos[3];
            wpos[0] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND);
            wpos[1] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 4);
            wpos[2] = G32(cmd[C_B2OFF].owner + AVP_PTR_GROUND + 8);
            if (s_trace) fprintf(stderr, "[MOUSE] button 2 up\n");
            gcall(cmd[C_B2OFF].handler, cmd[C_B2OFF].owner, &blk, 1);
            move_marker(wpos, fx, fy);
            return;
        }
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

