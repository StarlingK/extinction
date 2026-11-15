#include "avp_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern ptrdiff_t g_xbox_mem_offset;
extern volatile LONG g_avp_mainmenu;
extern volatile LONG g_avp_pcmenu_up;
extern void nv2a_set_screen_inset(int px);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GI(a)  (*(volatile int32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define LAYOUT_W 640
#define LAYOUT_H 480
#define CAM_TABLE 0x002F7E30u
#define CAM_FRAME 0x002F7E10u
#define CAM_STRIDE 0x210u
#define CAM_PROJ 0x40u
#define CAM_LEFT 0x1C0u
#define CAM_RIGHT 0x1C4u
#define CAM_TOP 0x1C8u
#define CAM_BOTTOM 0x1CCu
#define CAM_NEAR 0x1E0u
#define CAM_DEPTH 0x1E4u
#define FN_SET_CAMERA 0x000E95F0u
#define FN_LOAD_CAMERA 0x000E9570u
#define VIEWPORT_LIST 0x002F6CC4u
#define VIEWPORT_SOLO 0x002F6CD0u

static volatile LONG s_req = -1;
static int s_mode;
static int s_ox;
static int s_w = LAYOUT_W;
static int s_trace = -1;
static int s_applied;
static uint32_t s_last_vp;
static int32_t s_last_x, s_last_w;

static uint32_t bits_of(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static float aspect_for(int ox)
{
    return ox ? (float)(LAYOUT_W + 2 * ox) / (float)LAYOUT_H : 4.0f / 3.0f;
}

static int offset_for(int mode)
{
    static const int offsets[3] = { 0, 107, 64 };

    return mode >= 0 && mode <= 2 ? offsets[mode] : 0;
}

void avp_wide_set(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 0;
    InterlockedExchange(&s_req, mode);
}

int avp_wide_mode(void)
{
    return s_mode;
}

int avp_wide_width(void)
{
    return s_w;
}

int avp_wide_inset(void)
{
    return s_ox;
}

static void reapply_cameras(int old_ox, int new_ox)
{
    float a_old = aspect_for(old_ox), a_new = aspect_for(new_ox);
    uint32_t i;

    for (i = 0; i < 8; i++) {
        uint32_t cam = CAM_TABLE + i * CAM_STRIDE, regs[6] = { i, 0, 0, 0, 0, 0 };
        uint32_t args[6];
        float m00 = GF(cam + CAM_PROJ), m05 = GF(cam + CAM_PROJ + 0x14);
        int changed = 0;

        if (G32(cam + CAM_PROJ + 0x2C) == bits_of(1.0f) && m05 > 0.0f
            && fabsf(m00 * a_old - m05) <= 1e-3f * m05) {
            GF(cam + CAM_PROJ) = m05 / a_new;
            changed = 1;
        }
        if (GF(cam + CAM_LEFT) == (float)-old_ox && GF(cam + CAM_RIGHT) == (float)(LAYOUT_W + old_ox)
            && GF(cam + CAM_TOP) == 0.0f && GF(cam + CAM_BOTTOM) == (float)LAYOUT_H) {
            float near_z = GF(cam + CAM_NEAR), far_z = near_z + GF(cam + CAM_DEPTH);
            args[0] = 0;
            args[1] = 0;
            args[2] = bits_of((float)LAYOUT_W);
            args[3] = bits_of((float)LAYOUT_H);
            args[4] = bits_of(near_z);
            args[5] = bits_of(far_z);
            avp_gcall_regs(FN_SET_CAMERA, regs, args, 6);
            changed = 1;
        }
        if (changed) {
            regs[2] = cam;
            avp_gcall_regs(FN_LOAD_CAMERA, regs, NULL, 0);
        }
    }
}

static void apply(int mode)
{
    int ox = offset_for(mode), old = s_ox;

    s_mode = mode;
    if (ox == old)
        return;
    s_ox = ox;
    s_w = LAYOUT_W + 2 * ox;
    nv2a_set_screen_inset(ox);
    reapply_cameras(old, ox);
    if (ox)
        fprintf(stderr, "[WIDE] %s: virtual screen %d x %d, layout inset %d\n",
                mode == 1 ? "16:9" : "16:10", s_w, LAYOUT_H, ox);
    else
        fprintf(stderr, "[WIDE] off\n");
}

void avp_wide_camera(uint32_t cam, uint32_t esp)
{
    if (!s_ox || cam >= 8)
        return;
    if (G32(esp + 4) != 0 || G32(esp + 8) != 0
        || G32(esp + 12) != bits_of((float)LAYOUT_W) || G32(esp + 16) != bits_of((float)LAYOUT_H))
        return;
    G32(esp + 4) = bits_of((float)-s_ox);
    G32(esp + 12) = bits_of((float)s_w);
}

void avp_wide_ortho(uint32_t esp)
{
    if (!s_ox)
        return;
    if (G32(esp + 4) != 0 || G32(esp + 8) != bits_of((float)LAYOUT_W)
        || G32(esp + 12) != bits_of((float)LAYOUT_H) || G32(esp + 16) != 0)
        return;
    G32(esp + 4) = bits_of((float)-s_ox);
    G32(esp + 8) = bits_of((float)(LAYOUT_W + s_ox));
}

void avp_wide_perspective(uint32_t esp)
{
    if (!s_ox || G32(esp + 8) != 0x3FAAAAA8u)
        return;
    G32(esp + 8) = bits_of(aspect_for(s_ox));
}

void avp_wide_viewport(uint32_t vp)
{
    int32_t x, w, nx, nw, shift;
    float k;

    if (!s_ox || !vp)
        return;
    x = GI(vp);
    w = GI(vp + 8);
    if (s_trace > 0)
        fprintf(stderr, "[WIDE] vp in %d %d %d %d\n", x, GI(vp + 4), w, GI(vp + 12));
    if (w <= 0 || (x <= 0 && x + w >= LAYOUT_W))
        return;
    if (vp == s_last_vp && x == s_last_x && w == s_last_w)
        return;
    shift = s_ox;
    k = (float)LAYOUT_W / (float)s_w;
    nx = (int32_t)(((float)x + (float)shift) * k + 0.5f);
    nw = (int32_t)((float)w * k + 0.5f);
    if (nw < 1)
        nw = 1;
    if (nx < 0)
        nx = 0;
    if (nx + nw > LAYOUT_W)
        nx = LAYOUT_W - nw;
    if (s_trace > 0)
        fprintf(stderr, "[WIDE] viewport %d,%d: %d+%d -> %d+%d\n", GI(vp), GI(vp + 4), x, w, nx, nw);
    GI(vp) = nx;
    GI(vp + 8) = nw;
    s_last_vp = vp;
    s_last_x = nx;
    s_last_w = nw;
}

void avp_wide_quads(uint32_t matrix_x)
{
    uint32_t frame, cam;

    if (!s_ox)
        return;
    frame = G32(CAM_FRAME);
    if (frame >= 8)
        return;
    cam = CAM_TABLE + frame * CAM_STRIDE;
    if (GF(cam + CAM_LEFT) != (float)-s_ox || GF(cam + CAM_RIGHT) != (float)(LAYOUT_W + s_ox))
        return;
    GF(matrix_x) = GF(matrix_x) + (float)s_ox;
}

static void viewport_rect(uint32_t vp, int from, int to)
{
    int pass;

    if (!vp || GI(vp + 8) != 0 || GI(vp + 16) != LAYOUT_H)
        return;
    for (pass = 0; pass < 2; pass++) {
        int edge = pass ? 0 : from;
        if (GI(vp + 4) == -edge && GI(vp + 12) == LAYOUT_W + edge) {
            GI(vp + 4) = -to;
            GI(vp + 12) = LAYOUT_W + to;
            return;
        }
    }
}

void avp_wide_quads_clip(uint32_t matrix_x)
{
    uint32_t frame, cam;
    float half;

    if (!s_ox)
        return;
    frame = G32(CAM_FRAME);
    if (frame >= 8)
        return;
    cam = CAM_TABLE + frame * CAM_STRIDE;
    if (GF(cam + CAM_LEFT) != (float)-s_ox || GF(cam + CAM_RIGHT) != (float)(LAYOUT_W + s_ox))
        return;
    half = (float)LAYOUT_W * 0.5f + (float)s_ox;
    GF(matrix_x) = GF(matrix_x) + (float)s_ox / half;
}

void avp_wide_viewports(void)
{
    int want = (g_avp_mainmenu || g_avp_pcmenu_up) ? 0 : s_ox;
    uint32_t list, n, i;

    if (!s_ox && !s_applied)
        return;
    list = G32(VIEWPORT_LIST);
    n = G32(VIEWPORT_LIST + 4);
    for (i = 0; list && i < n && i < 16; i++)
        viewport_rect(G32(list + 4 * i), s_applied, want);
    viewport_rect(G32(VIEWPORT_SOLO), s_applied, want);
    s_applied = want;
}

void avp_wide_narrow(void)
{
    uint32_t list, n, i;

    if (!s_applied)
        return;
    list = G32(VIEWPORT_LIST);
    n = G32(VIEWPORT_LIST + 4);
    for (i = 0; list && i < n && i < 16; i++)
        viewport_rect(G32(list + 4 * i), s_applied, 0);
    viewport_rect(G32(VIEWPORT_SOLO), s_applied, 0);
    s_applied = 0;
}

void avp_wide_render(int begin)
{
    if (begin)
        avp_wide_viewports();
    else
        avp_wide_narrow();
}

void avp_wide_frame(void)
{
    LONG req = InterlockedExchange(&s_req, -1);
    static int polls;
    const char *test = getenv("AVP_WIDE_TEST");

    if (test && ++polls % 20 == 0) {
        FILE *f = fopen(test, "r");
        int m = -1;

        if (f) {
            if (fscanf(f, "%d", &m) == 1 && m >= 0 && m <= 2 && m != s_mode)
                req = m;
            fclose(f);
        }
    }

    if (s_trace < 0)
        s_trace = getenv("AVP_WIDE_TRACE") != NULL;

    if (req >= 0)
        apply((int)req);
    avp_wide_viewports();
}
