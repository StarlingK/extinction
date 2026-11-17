#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "avp_mapext.h"
#include "avp_skirmish.h"

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t avp_gcall(uint32_t va, uint32_t self, const uint32_t *args, int n, uint32_t eax);
extern uint32_t avp_gcall_regs(uint32_t va, const uint32_t *regs, const uint32_t *args, int n);
extern int avp_coop_role(void);
extern float avp_camera_z(void);
extern int avp_world_rect(float *x0, float *y0, float *w, float *h);
extern const char *avp_skirmish_nmap(void);

#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define GF(a)  (*(volatile float *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define AVP_OBJ_TABLE   0x00289E40u
#define AVP_DYNCAST     0x001474DAu
#define AVP_TD_GOBJECT  0x0027E09Cu
#define AVP_TD_GUNIT    0x002821F4u
#define VS_UNIT_ISDEAD  55u
#define RENDMAP_VTABLE  0x00207C24u
#define SCAN_FIRST      0x00400000u
#define SCAN_END        0x04000000u
#define NEUTRAL_SLOT    9
#define TICK_FRAMES     30
#define HIGH_GROUND     20
#define SLOPE_STEEP     30.0f
#define SLOPE_EDGE      12.0f
#define PI_F            3.14159265f

typedef struct {
    NmapWild w;
    uint32_t h[NMAP_MAX_WILD];
    int n;
    time_t next;
    uint32_t rng;
} Spawner;

static Spawner s_sp[NMAP_MAX_WILD];
static int s_nsp;
static int s_active;
static int s_frames;
static float s_ox, s_oy;

static int ptr_ok(uint32_t p)
{
    return p >= 0x10000u && p < 0x84000000u - 0x1000u;
}

static int handle_live(uint32_t h, uint32_t *objp)
{
    uint32_t obj;
    if (!(h >> 16))
        return 0;
    obj = G32(AVP_OBJ_TABLE + (h >> 16) * 4u);
    if (!ptr_ok(obj) || G32(obj + 0x18) != h)
        return 0;
    if (objp)
        *objp = obj;
    return 1;
}

static int is_unit(uint32_t obj)
{
    uint32_t a[5], vt = G32(obj);
    if (vt < 0x10000u || vt >= 0x0031B000u)
        return 0;
    a[0] = obj; a[1] = 0; a[2] = AVP_TD_GOBJECT; a[3] = AVP_TD_GUNIT; a[4] = 0;
    return avp_gcall(AVP_DYNCAST, 0, a, 5, 0) != 0;
}

static int unit_dead(uint32_t obj)
{
    uint32_t vt = G32(obj), regs[6];
    regs[0] = G32(obj + 0x18); regs[1] = obj; regs[2] = vt; regs[3] = 0; regs[4] = obj; regs[5] = 0;
    return (avp_gcall_regs(G32(vt + VS_UNIT_ISDEAD * 4u), regs, NULL, 0) & 0xFFu) != 0;
}

static int creature_alive(uint32_t h)
{
    uint32_t obj;
    if (!handle_live(h, &obj))
        return 0;
    if (is_unit(obj) && unit_dead(obj))
        return 0;
    return 1;
}

static float frand(Spawner *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

static void spawn_one(Spawner *s)
{
    float r = s->w.radius > 0.5f ? s->w.radius : 0.5f;
    float ang = frand(s) * 2.0f * PI_F, d = r * 0.8f * sqrtf(frand(s));
    float x = s->w.x + cosf(ang) * d, y = s->w.y + sinf(ang) * d;
    uint32_t h = avp_skirmish_spawn(s->w.cls, NEUTRAL_SLOT, x, y, s->w.z, frand(s) * 2.0f * PI_F);
    int i;
    if (!h) {
        fprintf(stderr, "[WILDLIFE] spawn of %s failed\n", s->w.cls);
        return;
    }
    for (i = 0; i < NMAP_MAX_WILD; i++)
        if (!s->h[i]) {
            s->h[i] = h;
            break;
        }
}

static int count_alive(Spawner *s)
{
    int i, n = 0;
    for (i = 0; i < NMAP_MAX_WILD; i++) {
        if (!s->h[i])
            continue;
        if (creature_alive(s->h[i]))
            n++;
        else
            s->h[i] = 0;
    }
    return n;
}

static void wildlife_start(const Nmap *m)
{
    int i, k;
    float ox = s_ox, oy = s_oy, rw = 0.0f, rh = 0.0f;
    s_nsp = 0;
    memset(s_sp, 0, sizeof s_sp);
    if (!avp_world_rect(&ox, &oy, &rw, &rh)) {
        ox = s_ox;
        oy = s_oy;
        rw = rh = 0.0f;
    }
    for (i = 0; i < m->nwild && s_nsp < NMAP_MAX_WILD; i++) {
        Spawner *s = &s_sp[s_nsp++];
        s->w = m->wild[i];
        if (rw > 8.0f && rh > 8.0f) {
            s->w.x = fminf(fmaxf(s->w.x, ox + 3.0f), ox + rw - 3.0f);
            s->w.y = fminf(fmaxf(s->w.y, oy + 3.0f), oy + rh - 3.0f);
        }
        if (s->w.max < 1)
            s->w.max = 1;
        if (s->w.max > NMAP_MAX_WILD)
            s->w.max = NMAP_MAX_WILD;
        if (!(s->w.delay <= 86400.0f))
            s->w.delay = 86400.0f;
        s->rng = 0x9E3779B9u * (uint32_t)(i + 1);
        for (k = 0; k < s->w.max; k++)
            spawn_one(s);
        s->n = s->w.max;
        s->next = time(NULL) + (time_t)(s->w.delay > 1.0f ? s->w.delay : 1.0f);
        fprintf(stderr, "[WILDLIFE] %s at %.1f %.1f: %d of max %d\n", s->w.cls, s->w.x, s->w.y, count_alive(s), s->w.max);
    }
}

void avp_mapext_tick(void)
{
    int i;
    time_t now;
    if (!s_active || !s_nsp || avp_coop_role() == 2 || !avp_skirmish_active() || !avp_skirmish_nmap()[0])
        return;
    if (++s_frames < TICK_FRAMES)
        return;
    s_frames = 0;
    now = time(NULL);
    for (i = 0; i < s_nsp; i++) {
        Spawner *s = &s_sp[i];
        int alive = count_alive(s);
        if (alive >= s->w.max) {
            s->next = now + (time_t)(s->w.delay > 1.0f ? s->w.delay : 1.0f);
            continue;
        }
        if (now >= s->next) {
            spawn_one(s);
            s->next = now + (time_t)(s->w.delay > 1.0f ? s->w.delay : 1.0f);
        }
    }
}

static uint32_t find_rendmap(int *wo, int *ho)
{
    uint32_t a, best = 0;
    for (a = SCAN_FIRST; a + 0x40u < SCAN_END; a += 4u) {
        uint32_t cells;
        int w, h;
        if (G32(a) != RENDMAP_VTABLE)
            continue;
        w = *(volatile uint16_t *)((uintptr_t)(a + 0x1Cu) + (uintptr_t)g_xbox_mem_offset);
        h = *(volatile uint16_t *)((uintptr_t)(a + 0x1Eu) + (uintptr_t)g_xbox_mem_offset);
        if (w < 32 || h < 32 || w > 1024 || h > 1024)
            continue;
        if (*(volatile uint16_t *)((uintptr_t)(a + 0x20u) + (uintptr_t)g_xbox_mem_offset) != 16u)
            continue;
        if (GF(a + 0x28u) != 1.0f)
            continue;
        cells = G32(a + 0x38u);
        if (ptr_ok(cells) && cells + (uint32_t)w * (uint32_t)h * 16u < SCAN_END) {
            best = a;
            *wo = w;
            *ho = h;
        }
    }
    return best;
}

int avp_mapext_terrain_apply(const NmapTerrain *src)
{
    static const uint8_t k_layer_tile[NMAP_LAYERS] = { 0x10, 0x21, 0x54, 0x87 };
    uint32_t map, cells;
    int x, y, w = 0, h = 0;
    NmapTerrain rs, *t = &rs;
    map = find_rendmap(&w, &h);
    if (!map) {
        fprintf(stderr, "[TERRAIN] no CRendMap in guest memory\n");
        return 0;
    }
    if (src->w == w && src->h == h) {
        if (!nmap_terrain_alloc(&rs, w, h))
            return 0;
        memcpy(rs.z, src->z, (size_t)w * (size_t)h * sizeof *rs.z);
        memcpy(rs.splat, src->splat, (size_t)w * (size_t)h * NMAP_LAYERS);
        if (src->flags && rs.flags)
            memcpy(rs.flags, src->flags, (size_t)w * (size_t)h);
    } else if (!nmap_terrain_resample(src, &rs, w, h)) {
        fprintf(stderr, "[TERRAIN] could not resample %dx%d to %dx%d\n", src->w, src->h, w, h);
        return 0;
    }
    cells = G32(map + 0x38u);
    s_ox = GF(map + 0x10u);
    s_oy = GF(map + 0x14u);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            uint32_t a = cells + (uint32_t)i * 16u, w0 = G32(a), w1 = G32(a + 4u);
            int hz = t->z[i];
            int x1 = x + 1 < w ? x + 1 : x, y1 = y + 1 < h ? y + 1 : y, x0 = x > 0 ? x - 1 : x, y0 = y > 0 ? y - 1 : y;
            float gx = (float)(t->z[(size_t)y * (size_t)w + (size_t)x1] - t->z[(size_t)y * (size_t)w + (size_t)x0]) / (float)(x1 - x0 ? x1 - x0 : 1);
            float gy = (float)(t->z[(size_t)y1 * (size_t)w + (size_t)x] - t->z[(size_t)y0 * (size_t)w + (size_t)x]) / (float)(y1 - y0 ? y1 - y0 : 1);
            float slope = sqrtf(gx * gx + gy * gy);
            uint32_t flags = w0 & 0x08u;
            const uint8_t *sp = t->splat + i * NMAP_LAYERS;
            int best = 0, k;
            for (k = 1; k < NMAP_LAYERS; k++)
                if (sp[k] > sp[best])
                    best = k;
            if (hz >= HIGH_GROUND)
                flags |= 0x80u;
            if (t->flags && (t->flags[i] & (NMAP_CF_IMPASSABLE | NMAP_CF_WALL | NMAP_CF_CLIFF)) && !(t->flags[i] & NMAP_CF_RAMP))
                flags |= 0x60u;
            else if (slope >= SLOPE_STEEP && !(t->flags && (t->flags[i] & NMAP_CF_RAMP)))
                flags |= 0x60u;
            else if (slope >= SLOPE_EDGE)
                flags |= 0x02u;
            G32(a) = ((uint32_t)(uint16_t)(int16_t)hz << 16) | flags;
            G32(a + 4u) = (w1 & 0x00FFFFFFu) | ((uint32_t)k_layer_tile[best] << 24);
        }
    fprintf(stderr, "[TERRAIN] wrote %dx%d cells (from %dx%d) into CRendMap %08X (cells %08X)\n", w, h, src->w, src->h, map, cells);
    nmap_terrain_free(&rs);
    return 1;
}

void avp_mapext_prepare(Nmap *m)
{
    int i;
    s_active = 0;
    s_frames = 0;
    s_ox = s_oy = 0.0f;
    if (m->tw <= 0 || m->th <= 0 || m->relative)
        return;
    {
        NmapTerrain t;
        if (nmap_terrain_load(&t, avp_skirmish_nmap())) {
            avp_mapext_terrain_apply(&t);
            nmap_terrain_free(&t);
        } else
            fprintf(stderr, "[TERRAIN] sidecar for %s missing\n", avp_skirmish_nmap());
    }
    {
        float base = avp_camera_z();
        for (i = 0; i < NMAP_MAX_SPAWN; i++)
            if (m->spawn[i].used)
                m->spawn[i].z += base;
        for (i = 0; i < m->nobj; i++)
            m->obj[i].z += base;
        for (i = 0; i < m->nwild; i++)
            m->wild[i].z += base;
    }
}

void avp_mapext_start(const Nmap *m)
{
    s_frames = 0;
    if (avp_coop_role() == 2) {
        s_nsp = 0;
        s_active = 0;
        return;
    }
    wildlife_start(m);
    s_active = 1;
}

void avp_mapext_stop(void)
{
    s_active = 0;
    s_nsp = 0;
}
