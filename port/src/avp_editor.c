#include "avp_platform.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "avp_editor.h"
#include "avp_layout.h"
#include "avp_skirmish.h"

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern uint32_t avp_guest_alloca(uint32_t bytes);
extern uint32_t avp_guest_esp(void);
extern void avp_guest_set_esp(uint32_t v);

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define AVP_POINTER      0x002F6AECu
#define AVP_PTR_GROUND   0x1A8u
#define AVP_OBJ_TABLE    0x00289E40u
#define AVP_WORLD_OBJ    0x002F6AF0u
#define AVP_START_DEATH  0x0003C400u
#define AVP_MARKER_MAKE  0x0002EFF0u

#define PX0 500
#define PX1 636
#define PY0 40
#define PY1 440
#define ROW_H 11
#define HUD_Y 400
#define PICK_R 2.0f
#define MARK_MS 700
#define PREVIEW_DELAY 90
#define PREVIEW_PER_FRAME 3
#define PENDING_MS 300
#define PI_F 3.14159265f
#define STEP_ROT (PI_F / 12.0f)
#define N_AFF 18

enum { T_NONE, T_SPAWN, T_UNIT };
enum { S_NONE, S_SPAWN, S_UNIT };
enum { R_HEAD, R_SPAWN, R_UNIT };

typedef struct { int kind; AvpRace race; int n; const char *cls; } Tool;
typedef struct { int type; int race; int n; const char *cls; } Row;

static const char *const k_cls_alien[] = {
    "GAlienDrone", "GAlienRunner", "GAlienWarrior", "GAlienPraetorian", "GAlienRavager",
    "GAlienCarrier", "GQueen", "GEgg", "GHugger", "GChestBurster"
};
static const char *const k_cls_marine[] = {
    "GMarineInfantry", "GMarineMedic", "GMarinePyro", "GMarineSadarTrooper", "GMarineSmartGunner",
    "GMarineSniper", "GMarineCommTech", "GSyntheticArmless"
};
static const char *const k_cls_pred[] = {
    "GPredatorWarrior", "GPredatorBlooded", "GPredatorUnBlooded", "GPredatorBlazer", "GPredatorHydra",
    "GPredatorVanguard", "GPredatorSilentHunter", "GPredatorMasterHunter", "GPredatorAncient",
    "GPredSentryGun", "GPredAntiAircraftGun"
};
static const char *const *const k_cls[AVP_RACE_COUNT] = { k_cls_alien, k_cls_marine, k_cls_pred };
static const int k_ncls[AVP_RACE_COUNT] = { 10, 8, 11 };
static const uint32_t k_rgb[AVP_RACE_COUNT] = { 0x60FF60u, 0x3C8CFFu, 0xFF8C1Eu };
static const uint32_t k_crc[AVP_RACE_COUNT] = { 0x3C905527u, 0x7715B5E5u, 0x55EE21EFu };
static const char *const k_head[AVP_RACE_COUNT] = { "ALIEN", "MARINE", "PREDATOR" };

static AvpLayout s_lay;
static int s_init;
static char s_map[16];
static uint32_t s_h[AVP_LAYOUT_MAX_UNITS];
static uint8_t s_tried[AVP_LAYOUT_MAX_UNITS];
static Tool s_tool;
static volatile int s_sel_kind;
static int s_sel_i = -1;
static Row s_rows[48];
static int s_nrows;
static int s_scroll;
static int s_mx = -1, s_my = -1;
static int s_prev, s_own, s_lmode, s_drag, s_moved;
static uint32_t s_world;
static int s_frames;
static int s_pend = -1;
static ULONGLONG s_pend_t;
static ULONGLONG s_mark_t;
static ULONGLONG s_msg_t;
static char s_msg[96];
static volatile ULONGLONG s_last;
static volatile LONG s_q[16];
static volatile LONG s_qh, s_qt;

static int ptr_ok(uint32_t p)
{
    return p >= 0x10000u && p < 0x84000000u - 0x1000u;
}

static int handle_live(uint32_t h)
{
    uint32_t obj;
    if (!(h >> 16))
        return 0;
    obj = G32(AVP_OBJ_TABLE + (h >> 16) * 4u);
    return ptr_ok(obj) && G32(obj + 0x18) == h;
}

static void kill_preview(uint32_t h)
{
    if (h && handle_live(h)) {
        uint32_t a[2];
        a[0] = h; a[1] = 1;
        avp_gcall(AVP_START_DEATH, 0, a, 2, h);
    }
}

static int preview_slot(const AvpAff *a)
{
    return a->kind == AVP_AFF_SLOT ? a->slot : 9;
}

static void spawn_preview(int i)
{
    const AvpUnitRec *u = &s_lay.unit[i];
    s_h[i] = avp_skirmish_spawn(u->cls, preview_slot(&u->aff), u->x, u->y, u->z, u->rot);
    s_tried[i] = 1;
}

static void respawn(int i)
{
    kill_preview(s_h[i]);
    s_h[i] = 0;
    spawn_preview(i);
}

static void flush_pending(void)
{
    if (s_pend >= 0 && s_pend < s_lay.nunits)
        respawn(s_pend);
    s_pend = -1;
}

static void mark_pending(int i)
{
    if (s_pend >= 0 && s_pend != i)
        flush_pending();
    s_pend = i;
    s_pend_t = GetTickCount64() + PENDING_MS;
}

static void marker(int race, float x, float y, float z, uint32_t ptr)
{
    uint32_t esp = avp_guest_esp(), pos = avp_guest_alloca(32), regs[6] = { 0 }, a[2];
    GF(pos) = x; GF(pos + 4) = y; GF(pos + 8) = z;
    regs[0] = k_crc[race];
    a[0] = ptr; a[1] = pos;
    avp_gcall_regs(AVP_MARKER_MAKE, regs, a, 2);
    avp_guest_set_esp(esp);
}

static void rows_build(void)
{
    int r, i;
    Row *p = s_rows;
    p->type = R_HEAD; p->race = AVP_RACE_COUNT; p->n = 0; p->cls = "SPAWNS"; p++;
    for (r = 0; r < AVP_RACE_COUNT; r++)
        for (i = 1; i <= 3; i++) {
            p->type = R_SPAWN; p->race = r; p->n = i; p->cls = NULL; p++;
        }
    for (r = 0; r < AVP_RACE_COUNT; r++) {
        p->type = R_HEAD; p->race = r; p->n = 0; p->cls = k_head[r]; p++;
        for (i = 0; i < k_ncls[r]; i++) {
            p->type = R_UNIT; p->race = r; p->n = i; p->cls = k_cls[r][i]; p++;
        }
    }
    s_nrows = (int)(p - s_rows);
}

static int rows_visible(void)
{
    return (PY1 - PY0 - 8) / ROW_H;
}

static int scroll_max(void)
{
    int m = s_nrows - rows_visible();
    return m > 0 ? m : 0;
}

static void world_reset(void)
{
    int i;
    for (i = 0; i < AVP_LAYOUT_MAX_UNITS; i++) {
        s_h[i] = 0;
        s_tried[i] = 0;
    }
    s_frames = 0;
    s_pend = -1;
}

static void ensure_init(void)
{
    AvpSkirmishCfg c;
    if (s_init)
        return;
    memset(&c, 0, sizeof c);
    avp_skirmish_config_load(&c);
    snprintf(s_map, sizeof s_map, "%s", c.map);
    avp_layout_init(&s_lay, s_map);
    if (avp_layout_load(&s_lay, s_map) < 0)
        avp_layout_init(&s_lay, s_map);
    rows_build();
    world_reset();
    s_sel_kind = S_NONE;
    s_sel_i = -1;
    s_tool.kind = T_NONE;
    s_scroll = 0;
    s_init = 1;
    fprintf(stderr, "[EDITOR] map %s: %d units in layout\n", s_map, s_lay.nunits);
}

int avp_editor_active(void)
{
    return avp_skirmish_editor();
}

static int live(void)
{
    return avp_skirmish_editor() && s_init && GetTickCount64() - s_last < 500;
}

static void say(const char *fmt, const char *a)
{
    snprintf(s_msg, sizeof s_msg, fmt, a);
    s_msg_t = GetTickCount64();
}

static int spawn_count(void)
{
    int r, i, n = 0;
    for (r = 0; r < AVP_RACE_COUNT; r++)
        for (i = 0; i < 3; i++)
            n += s_lay.spawn[r][i].used != 0;
    return n;
}

static int aff_index(const AvpAff *a)
{
    if (a->kind == AVP_AFF_SLOT)
        return a->slot >= 1 && a->slot <= 8 ? a->slot - 1 : 0;
    if (a->kind == AVP_AFF_NEUTRAL)
        return 8;
    return 9 + (int)a->race * 3 + (a->n >= 1 && a->n <= 3 ? a->n - 1 : 0);
}

static AvpAff aff_from_index(int k)
{
    AvpAff a;
    memset(&a, 0, sizeof a);
    if (k < 8) {
        a.kind = AVP_AFF_SLOT; a.slot = k + 1;
    } else if (k == 8) {
        a.kind = AVP_AFF_NEUTRAL;
    } else {
        a.kind = AVP_AFF_SPAWN; a.race = (AvpRace)((k - 9) / 3); a.n = (k - 9) % 3 + 1;
    }
    return a;
}

static void select_item(int kind, int i)
{
    if (kind != s_sel_kind || i != s_sel_i)
        flush_pending();
    s_sel_i = i;
    s_sel_kind = kind;
}

static float *sel_pos(float **y, float **rot)
{
    if (s_sel_kind == S_SPAWN) {
        AvpSpawn *sp = &s_lay.spawn[s_sel_i / 3][s_sel_i % 3];
        *y = &sp->y; *rot = &sp->rot;
        return &sp->x;
    }
    if (s_sel_kind == S_UNIT && s_sel_i >= 0 && s_sel_i < s_lay.nunits) {
        AvpUnitRec *u = &s_lay.unit[s_sel_i];
        *y = &u->y; *rot = &u->rot;
        return &u->x;
    }
    return NULL;
}

static int hit_test(float x, float y, int *kind, int *idx)
{
    float best = PICK_R * PICK_R;
    int r, i, found = 0;
    for (r = 0; r < AVP_RACE_COUNT; r++)
        for (i = 0; i < 3; i++) {
            const AvpSpawn *sp = &s_lay.spawn[r][i];
            float dx = sp->x - x, dy = sp->y - y, d = dx * dx + dy * dy;
            if (sp->used && d < best) { best = d; *kind = S_SPAWN; *idx = r * 3 + i; found = 1; }
        }
    for (i = 0; i < s_lay.nunits; i++) {
        const AvpUnitRec *u = &s_lay.unit[i];
        float dx = u->x - x, dy = u->y - y, d = dx * dx + dy * dy;
        if (d < best) { best = d; *kind = S_UNIT; *idx = i; found = 1; }
    }
    return found;
}

static void delete_item(int kind, int i)
{
    flush_pending();
    if (kind == S_SPAWN) {
        s_lay.spawn[i / 3][i % 3].used = 0;
        if (s_sel_kind == S_SPAWN && s_sel_i == i) { s_sel_kind = S_NONE; s_sel_i = -1; }
        return;
    }
    if (i < 0 || i >= s_lay.nunits)
        return;
    kill_preview(s_h[i]);
    memmove(&s_h[i], &s_h[i + 1], (size_t)(s_lay.nunits - i - 1) * sizeof s_h[0]);
    memmove(&s_tried[i], &s_tried[i + 1], (size_t)(s_lay.nunits - i - 1) * sizeof s_tried[0]);
    avp_layout_remove_unit(&s_lay, i);
    if (s_sel_kind == S_UNIT) {
        if (s_sel_i == i) { s_sel_kind = S_NONE; s_sel_i = -1; }
        else if (s_sel_i > i) s_sel_i--;
    }
    s_drag = 0;
}

static void place(float x, float y, float z)
{
    if (s_tool.kind == T_SPAWN) {
        AvpSpawn *sp = &s_lay.spawn[s_tool.race][s_tool.n - 1];
        sp->race = s_tool.race; sp->n = s_tool.n;
        sp->x = x; sp->y = y; sp->z = z; sp->rot = 0.0f; sp->used = 1;
        select_item(S_SPAWN, (int)s_tool.race * 3 + s_tool.n - 1);
        s_mark_t = 0;
    } else if (s_tool.kind == T_UNIT) {
        AvpUnitRec u;
        memset(&u, 0, sizeof u);
        snprintf(u.cls, sizeof u.cls, "%s", s_tool.cls);
        u.aff.kind = AVP_AFF_SLOT; u.aff.slot = 1;
        u.x = x; u.y = y; u.z = z; u.rot = 0.0f;
        if (!avp_layout_add_unit(&s_lay, &u)) {
            say("%s", "Unit limit reached");
            return;
        }
        spawn_preview(s_lay.nunits - 1);
        select_item(S_UNIT, s_lay.nunits - 1);
    }
}

static void panel_click(int fy)
{
    int ri = s_scroll + (fy - (PY0 + 4)) / ROW_H;
    const Row *r;
    if (fy < PY0 + 4 || ri < 0 || ri >= s_nrows)
        return;
    r = &s_rows[ri];
    if (r->type == R_SPAWN) {
        s_tool.kind = T_SPAWN; s_tool.race = (AvpRace)r->race; s_tool.n = r->n; s_tool.cls = NULL;
    } else if (r->type == R_UNIT) {
        s_tool.kind = T_UNIT; s_tool.race = (AvpRace)r->race; s_tool.n = 0; s_tool.cls = r->cls;
    }
}

static void key_run(int code)
{
    int ctrl = (code >> 16) & 1, k = code & 0xFFFF;
    float *y, *rot, *x;
    if (k == VK_TAB) {
        if (s_sel_kind == S_UNIT && s_sel_i >= 0 && s_sel_i < s_lay.nunits) {
            AvpUnitRec *u = &s_lay.unit[s_sel_i];
            u->aff = aff_from_index((aff_index(&u->aff) + 1) % N_AFF);
            mark_pending(s_sel_i);
        }
    } else if (k == '[' || k == ']') {
        x = sel_pos(&y, &rot);
        if (x) {
            float r = *rot + (k == ']' ? STEP_ROT : -STEP_ROT);
            if (r > PI_F) r -= 2.0f * PI_F;
            if (r <= -PI_F) r += 2.0f * PI_F;
            *rot = r;
            if (s_sel_kind == S_UNIT)
                mark_pending(s_sel_i);
        }
    } else if (k == VK_DELETE) {
        if (s_sel_kind != S_NONE)
            delete_item(s_sel_kind, s_sel_i);
    } else if (k == 'S' && ctrl) {
        flush_pending();
        if (avp_layout_save(&s_lay)) {
            say("Saved maps/%s.layout", s_map);
            fprintf(stderr, "[EDITOR] saved maps/%s.layout (%d units)\n", s_map, s_lay.nunits);
        } else {
            say("%s", "Save failed");
        }
    } else if (k == VK_ESCAPE) {
        flush_pending();
        s_sel_kind = S_NONE;
        s_sel_i = -1;
        s_drag = 0;
    }
}

static int ground_point(uint32_t ptr, float *x, float *y, float *z)
{
    if (!ptr)
        return 0;
    *x = GF(ptr + AVP_PTR_GROUND);
    *y = GF(ptr + AVP_PTR_GROUND + 4);
    *z = GF(ptr + AVP_PTR_GROUND + 8);
    if (!(fabsf(*x) < 1e6f && fabsf(*y) < 1e6f && fabsf(*z) < 1e6f))
        return 0;
    return *x != 0.0f || *y != 0.0f || *z != 0.0f;
}

int avp_editor_frame(int fx, int fy, int b, int wheel)
{
    ULONGLONG now = GetTickCount64();
    uint32_t ptr, world;
    ptr = G32(AVP_POINTER);
    world = G32(AVP_WORLD_OBJ);
    float gx = 0, gy = 0, gz = 0;
    int have, down, inpanel, consumed = 0, i;

    ensure_init();
    s_last = now;
    if (world != s_world) {
        s_world = world;
        world_reset();
    }
    s_frames++;
    s_mx = fx; s_my = fy;
    have = ground_point(ptr, &gx, &gy, &gz);
    inpanel = fx >= PX0 && fx < PX1 && fy >= PY0 && fy < PY1;
    down = b & ~s_prev;
    s_prev = b;

    while (s_qh != s_qt) {
        int code = (int)s_q[s_qh];
        s_qh = (s_qh + 1) % 16;
        key_run(code);
    }
    if (s_pend >= 0 && now >= s_pend_t)
        flush_pending();

    if (ptr && s_frames >= PREVIEW_DELAY) {
        int n = 0;
        for (i = 0; i < s_lay.nunits && n < PREVIEW_PER_FRAME; i++)
            if (!s_tried[i]) {
                spawn_preview(i);
                n++;
            }
        if (now - s_mark_t >= MARK_MS) {
            int r, k;
            s_mark_t = now;
            for (r = 0; r < AVP_RACE_COUNT; r++)
                for (k = 0; k < 3; k++) {
                    const AvpSpawn *sp = &s_lay.spawn[r][k];
                    if (sp->used)
                        marker(r, sp->x, sp->y, sp->z, ptr);
                }
        }
    }

    if (inpanel && wheel) {
        s_scroll += wheel > 0 ? -3 : 3;
        if (s_scroll > scroll_max()) s_scroll = scroll_max();
        if (s_scroll < 0) s_scroll = 0;
        consumed = 1;
    }

    if ((down & 1) && !(s_own & 2)) {
        if (inpanel) {
            panel_click(fy);
            s_own |= 1; s_lmode = 1;
        } else if (fy >= 0 && fy <= HUD_Y && fx >= 0) {
            int kind, idx;
            s_own |= 1; s_lmode = 2;
            s_drag = 0; s_moved = 0;
            if (have && hit_test(gx, gy, &kind, &idx)) {
                select_item(kind, idx);
                s_drag = 1;
            } else if (have && s_tool.kind != T_NONE) {
                place(gx, gy, gz);
            } else {
                flush_pending();
                s_sel_kind = S_NONE;
                s_sel_i = -1;
            }
        }
    }
    if (s_own & 1) {
        consumed = 1;
        if (!(b & 1)) {
            if (s_lmode == 2 && s_drag && s_moved && s_sel_kind == S_UNIT
                && s_sel_i >= 0 && s_sel_i < s_lay.nunits)
                respawn(s_sel_i);
            s_drag = 0;
            s_own &= ~1;
            s_lmode = 0;
        } else if (s_lmode == 2 && s_drag && have) {
            float *y, *rot, *x = sel_pos(&y, &rot);
            if (x && (fabsf(*x - gx) > 0.05f || fabsf(*y - gy) > 0.05f)) {
                float *z = NULL;
                *x = gx; *y = gy;
                if (s_sel_kind == S_SPAWN)
                    z = &s_lay.spawn[s_sel_i / 3][s_sel_i % 3].z;
                else if (s_sel_kind == S_UNIT)
                    z = &s_lay.unit[s_sel_i].z;
                if (z) *z = gz;
                s_moved = 1;
                if (s_sel_kind == S_SPAWN)
                    s_mark_t = 0;
            }
        }
    }

    if ((down & 2) && !(s_own & 1) && !inpanel && fy >= 0 && fy <= HUD_Y && have) {
        int kind, idx;
        if (hit_test(gx, gy, &kind, &idx)) {
            delete_item(kind, idx);
            s_own |= 2;
        }
    }
    if (s_own & 2) {
        consumed = 1;
        if (!(b & 2))
            s_own &= ~2;
    }
    return consumed;
}

int avp_editor_key(UINT msg, WPARAM w, LPARAM l)
{
    int ctrl;
    LONG t;
    if (!live())
        return 0;
    ctrl = GetKeyState(VK_CONTROL) < 0;
    if (msg == WM_CHAR) {
        if (w != '[' && w != ']')
            return 0;
    } else if (msg == WM_KEYDOWN) {
        if (w == VK_OEM_4 || w == VK_OEM_6)
            return 1;
        if (w == 'S' && !ctrl)
            return 0;
        if (w == VK_ESCAPE && !s_sel_kind)
            return 0;
        if (w != VK_TAB && w != VK_DELETE && w != 'S' && w != VK_ESCAPE)
            return 0;
        if (w == 'S' && (l & (1 << 30)))
            return 1;
    } else {
        return 0;
    }
    t = (s_qt + 1) % 16;
    if (t != s_qh) {
        s_q[s_qt] = (LONG)((int)w | (ctrl ? 0x10000 : 0));
        s_qt = t;
    }
    return 1;
}

static const uint8_t k_font[65][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x10,0x20,0x10,0x08,0x04}
};

#define GLYPH_CHECK '\x01'

const uint8_t *avp_font5x7(int ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    if (ch >= 0x20 && ch <= 0x5F)
        return k_font[ch - 0x20];
    return k_font[0];
}

static void put(uint32_t *rgb, int w, int h, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        rgb[(size_t)y * w + x] = c;
}

static void glyph(uint32_t *rgb, int w, int h, int x, int y, int ch, uint32_t c)
{
    const uint8_t *g;
    int cx, ry;
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    if (ch == GLYPH_CHECK)
        g = k_font[64];
    else if (ch >= 0x20 && ch <= 0x5F)
        g = k_font[ch - 0x20];
    else
        g = k_font[0];
    for (cx = 0; cx < 5; cx++)
        for (ry = 0; ry < 7; ry++)
            if (g[cx] & (1 << ry))
                put(rgb, w, h, x + cx, y + ry, c);
}

static void ptext(uint32_t *rgb, int w, int h, int x, int y, const char *s, uint32_t c)
{
    int i;
    for (i = 0; s[i]; i++)
        glyph(rgb, w, h, x + i * 6 + 1, y + 1, (unsigned char)s[i], 0x000000u);
    for (i = 0; s[i]; i++)
        glyph(rgb, w, h, x + i * 6, y, (unsigned char)s[i], c);
}

static void fill(uint32_t *rgb, int w, int h, int x0, int y0, int x1, int y1, uint32_t c, int a)
{
    int x, y, cr = (int)((c >> 16) & 255), cg = (int)((c >> 8) & 255), cb = (int)(c & 255);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            uint32_t *p = &rgb[(size_t)y * w + x];
            int r = (int)((*p >> 16) & 255), g = (int)((*p >> 8) & 255), bl = (int)(*p & 255);
            r += (cr - r) * a / 256;
            g += (cg - g) * a / 256;
            bl += (cb - bl) * a / 256;
            *p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
}

static void frame_box(uint32_t *rgb, int w, int h, int x0, int y0, int x1, int y1, uint32_t c)
{
    fill(rgb, w, h, x0, y0, x1, y0 + 1, c, 200);
    fill(rgb, w, h, x0, y1 - 1, x1, y1, c, 200);
    fill(rgb, w, h, x0, y0, x0 + 1, y1, c, 200);
    fill(rgb, w, h, x1 - 1, y0, x1, y1, c, 200);
}

static int class_count(const char *cls)
{
    int i, n = 0;
    for (i = 0; i < s_lay.nunits; i++)
        n += !strcmp(s_lay.unit[i].cls, cls);
    return n;
}

static void tool_name(char *buf, size_t n)
{
    if (s_tool.kind == T_SPAWN)
        snprintf(buf, n, "%s SPAWN %d", k_head[s_tool.race], s_tool.n);
    else if (s_tool.kind == T_UNIT)
        snprintf(buf, n, "%s", s_tool.cls + 1);
    else
        snprintf(buf, n, "NONE (CLICK AN ENTRY)");
}

static void draw_palette(uint32_t *rgb, int w, int h)
{
    int i, vis = rows_visible(), y = PY0 + 4;
    fill(rgb, w, h, PX0, PY0, PX1, PY1, 0x060C18u, 170);
    frame_box(rgb, w, h, PX0, PY0, PX1, PY1, 0x3CC8E6u);
    for (i = 0; i < vis && s_scroll + i < s_nrows; i++, y += ROW_H) {
        const Row *r = &s_rows[s_scroll + i];
        uint32_t col = r->race < AVP_RACE_COUNT ? k_rgb[r->race] : 0xC8D0D8u;
        char buf[40];
        int hot = s_mx >= PX0 && s_mx < PX1 && s_my >= y && s_my < y + ROW_H;
        if (r->type == R_HEAD) {
            fill(rgb, w, h, PX0 + 2, y, PX1 - 2, y + ROW_H - 1, col, 80);
            ptext(rgb, w, h, PX0 + 6, y + 2, r->cls, col);
            continue;
        }
        if ((r->type == R_SPAWN && s_tool.kind == T_SPAWN && s_tool.race == (AvpRace)r->race && s_tool.n == r->n)
            || (r->type == R_UNIT && s_tool.kind == T_UNIT && s_tool.cls == r->cls))
            fill(rgb, w, h, PX0 + 2, y, PX1 - 2, y + ROW_H - 1, 0x2A64B4u, 170);
        else if (hot)
            fill(rgb, w, h, PX0 + 2, y, PX1 - 2, y + ROW_H - 1, 0xFFFFFFu, 36);
        if (r->type == R_SPAWN) {
            snprintf(buf, sizeof buf, "%s %d", k_head[r->race], r->n);
            ptext(rgb, w, h, PX0 + 10, y + 2, buf, col);
            if (s_lay.spawn[r->race][r->n - 1].used) {
                char ck[2] = { GLYPH_CHECK, 0 };
                ptext(rgb, w, h, PX1 - 14, y + 2, ck, 0x40E060u);
            }
        } else {
            int c = class_count(r->cls);
            ptext(rgb, w, h, PX0 + 10, y + 2, r->cls + 1, 0xD8E6F0u);
            if (c > 0) {
                snprintf(buf, sizeof buf, "%d", c);
                ptext(rgb, w, h, PX1 - 6 - (int)strlen(buf) * 6, y + 2, buf, col);
            }
        }
    }
    if (scroll_max() > 0) {
        int th = (PY1 - PY0 - 8) * vis / s_nrows, ty = PY0 + 4 + (PY1 - PY0 - 8 - th) * s_scroll / scroll_max();
        fill(rgb, w, h, PX1 - 3, PY0 + 4, PX1 - 1, PY1 - 4, 0x3CC8E6u, 40);
        fill(rgb, w, h, PX1 - 3, ty, PX1 - 1, ty + th, 0x3CC8E6u, 220);
    }
}

static void draw_info(uint32_t *rgb, int w, int h)
{
    char buf[96], aff[32];
    int ln = 0;
    float *y, *rot, *x;
    fill(rgb, w, h, 8, 8, 300, 70, 0x060C18u, 170);
    frame_box(rgb, w, h, 8, 8, 300, 70, 0x3CC8E6u);
    tool_name(aff, sizeof aff);
    snprintf(buf, sizeof buf, "MAP %s  TOOL %s", s_map, aff);
    ptext(rgb, w, h, 12, 10 + 9 * ln++, buf, 0xFFFFFFu);
    x = sel_pos(&y, &rot);
    if (x && s_sel_kind == S_SPAWN) {
        snprintf(buf, sizeof buf, "SEL SPAWN %s %d", k_head[s_sel_i / 3], s_sel_i % 3 + 1);
        ptext(rgb, w, h, 12, 10 + 9 * ln++, buf, k_rgb[s_sel_i / 3]);
    } else if (x) {
        avp_aff_format(&s_lay.unit[s_sel_i].aff, aff, sizeof aff);
        snprintf(buf, sizeof buf, "SEL %s  AFF %s", s_lay.unit[s_sel_i].cls + 1, aff);
        ptext(rgb, w, h, 12, 10 + 9 * ln++, buf, 0xF0D040u);
    } else {
        ptext(rgb, w, h, 12, 10 + 9 * ln++, "SEL NONE", 0x7C8794u);
    }
    if (x) {
        snprintf(buf, sizeof buf, "X %.1f  Y %.1f  ROT %d", (double)*x, (double)*y,
                 (int)(*rot * 180.0f / PI_F + (*rot < 0 ? -0.5f : 0.5f)));
        ptext(rgb, w, h, 12, 10 + 9 * ln++, buf, 0xD8E6F0u);
    } else {
        ln++;
    }
    snprintf(buf, sizeof buf, "SPAWNS %d/9  UNITS %d", spawn_count(), s_lay.nunits);
    ptext(rgb, w, h, 12, 10 + 9 * ln++, buf, 0xD8E6F0u);
    ptext(rgb, w, h, 12, 10 + 9 * ln++, "LMB PLACE/SELECT  RMB DELETE  TAB AFFILIATION", 0x7C8794u);
    ptext(rgb, w, h, 12, 10 + 9 * ln++, "[ ] ROTATE  DEL REMOVE  CTRL+S SAVE  ESC DESELECT", 0x7C8794u);
}

void avp_editor_draw(uint32_t *rgb, int w, int h)
{
    if (!live() || w < 640 || h < 480)
        return;
    draw_palette(rgb, w, h);
    draw_info(rgb, w, h);
    if (s_msg[0] && GetTickCount64() - s_msg_t < 2000)
        ptext(rgb, w, h, 12, 74, s_msg, 0x40E060u);
}
