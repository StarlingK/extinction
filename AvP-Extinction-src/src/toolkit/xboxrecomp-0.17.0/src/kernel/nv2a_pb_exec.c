#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a_flip_hold.h"
#include "platform/host_time.h"
#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#endif

#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_backend.h"
#include "nv2a_gpu.h"
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowSetAA(uint32_t sy);
extern uint32_t xbox_Nv2aFenceWordVa(void);
extern int nv2a_pb_dma_walk(void);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

static int guest_span_ok(uint64_t addr, uint32_t bytes);

static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!bytes)
        return 0;

    if (base < XBOX_HEAP_BASE && (uint64_t)base + bytes > XBOX_STACK_BASE)
        return 1;

    if (!guest_span_ok(base, bytes))
        return 1;
    if (!g_xbox_image_hi)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X), a thread stack, or is "
                "outside guest memory.\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_PALETTE         0x1B20
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C

#define NV097_NO_OPERATION                0x0100
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800

#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818

#define NV097_SET_VERTEX3F                0x1500
#define NV097_SET_VERTEX4F                0x1518
#define NV097_SET_VERTEX_DATA2F_M         0x1880
#define NV097_SET_VERTEX_DATA4F_M         0x1A00
#define NV097_SET_VERTEX_DATA4UB          0x1940
#define NV097_SET_VERTEX_DATA2S           0x1900
#define NV097_SET_VERTEX_DATA4S_M         0x1980

#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0

typedef struct {
    uint32_t offset;
    uint32_t type;
    uint32_t size;
    uint32_t stride;
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536

typedef struct {
    uint32_t offset;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t color;
    uint32_t addr_u, addr_v;
    uint32_t palette;
    uint32_t levels;
    uint32_t filter;
    uint32_t border;
    int      border_color;
    int      cube;
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;
    uint32_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;

    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;

    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;

    uint32_t drawn_offset;

    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    int drawn_stale;
    uint64_t pixels;
    uint32_t pixel_max;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clip_raw_h, clip_raw_v;
    float    aa_sx, aa_sy;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;

    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;

    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];
    int blend_npairs;
    Texture  texs[4];

    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];

    uint32_t xf_mode;
    uint32_t batches_program, verts_program, tris_behind;

    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear;

    uint32_t clear_rect_h, clear_rect_v;
    int clear_rect_set;

    float    composite[16];
    int      composite_set;
    uint32_t batches_ffp;
} s_gpu;

static uint32_t s_reg[0x2000 / 4];
static float reg_f(uint32_t method)
{
    float f;
    memcpy(&f, &s_reg[method / 4], 4);
    return f;
}

static uint32_t s_sem_va;
void nv2a_pb_set_semaphore_target(uint32_t guest_va) { s_sem_va = guest_va; }

#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void tex_update_valid(Texture *t)
{
    t->valid = t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;

    tex_update_valid(&s_gpu.texs[0]);
}

#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

static void dump_texture_bmp(uint32_t seq);

#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;

            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static uint16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    uint16_t *slot = &s_unhandled_slot[(method & 0x1FFC) / 4];

    s_gpu.unhandled_total++;
    if (*slot) {
        s_unhandled[*slot - 1].count++;
        s_unhandled[*slot - 1].last_param = param;
        return;
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        *slot = (uint16_t)s_unhandled_count;
    }
}

static int guest_span_ok(uint64_t addr, uint32_t bytes)
{
    uint64_t end = addr + bytes;

    if (end <= (uint64_t)XBOX_CONTIG_SIZE)
        return 1;
    return addr >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

static uint32_t attr_elem_bytes(const VertexAttr *a)
{
    uint32_t n = a->size < 4 ? a->size : 4;

    switch (a->type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2u * n;
    case 4:         return n;
    default:        return 4u * n;
    }
}

static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {

        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        uint64_t at = (uint64_t)a->offset + (uint64_t)index * a->stride;

        if (!a->offset || !guest_span_ok(at, attr_elem_bytes(a)))
            return 0;
        p = mem + at;
    }

    switch (a->type) {
    case 0:

        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;

    case 1:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);

static void note_drawn(void)
{
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{

    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }

    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}

static int s_flip_dumping;

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(offset)
                           + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

static void surface_apply_clip(void)
{
    uint32_t aa = (s_gpu.format >> 12) & 0xF;
    uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;

    s_gpu.aa_sx = (float)sx;
    s_gpu.aa_sy = (float)sy;

    s_gpu.clip_x = (s_gpu.clip_raw_h & 0xFFFF) * sx;
    s_gpu.clip_w = ((s_gpu.clip_raw_h >> 16) & 0xFFFF) * sx;
    s_gpu.clip_y = (s_gpu.clip_raw_v & 0xFFFF) * sy;
    s_gpu.clip_h = ((s_gpu.clip_raw_v >> 16) & 0xFFFF) * sy;
}

static const Nv2aBackend *s_backend;
static void (*s_backend_present_sync)(void);

void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void))
{
    s_backend = backend;
    s_backend_present_sync = backend ? present_sync : NULL;
}

void nv2a_backend_register(const Nv2aBackend *backend)
{
    nv2a_backend_register_with_present_sync(backend, NULL);
}

static int backend_draws(void)
{
    return s_backend && s_backend->draw;
}

static void current_surface(Nv2aSurface *out)
{
    out->color_va        = dma_resolve(s_gpu.color_offset);
    out->width           = s_gpu.clip_w;
    out->height          = s_gpu.clip_h;
    out->pitch           = s_gpu.pitch;
    out->bytes_per_pixel = surface_bpp();
    out->aa_sx           = s_gpu.aa_sx > 1.5f ? 2 : 1;
    out->aa_sy           = s_gpu.aa_sy > 1.5f ? 2 : 1;
}

static void current_render_state(Nv2aRenderState *r)
{
    r->blend_enable      = s_gpu.blend_enable;
    r->blend_src         = s_gpu.blend_sfactor;
    r->blend_dst         = s_gpu.blend_dfactor;
    r->blend_eq          = s_gpu.blend_equation;
    r->blend_color       = s_gpu.blend_color;
    r->alpha_test_enable = s_gpu.alpha_test;
    r->alpha_func        = s_gpu.alpha_func;
    r->alpha_ref         = s_gpu.alpha_ref;
    r->depth_test_enable = s_gpu.depth_test;
    r->depth_func        = s_gpu.depth_func;
    r->depth_write       = s_gpu.depth_mask;
    r->color_mask        = s_gpu.color_mask;
    r->cull_enable       = s_reg[0x0308 / 4];
    r->cull_face         = s_reg[0x039C / 4];
    r->front_face        = s_reg[0x03A0 / 4];
    r->zeta_va           = s_gpu.zeta_offset ? dma_resolve(s_gpu.zeta_offset) : 0;
    r->depth_min         = reg_f(0x0394);
    r->depth_max         = reg_f(0x0398);
    r->stencil_enable    = s_reg[0x032C / 4];
    r->stencil_write_mask = s_reg[0x0360 / 4];
    r->stencil_func      = s_reg[0x0364 / 4];
    r->stencil_ref       = s_reg[0x0368 / 4];
    r->stencil_func_mask = s_reg[0x036C / 4];
    r->stencil_fail      = s_reg[0x0370 / 4];
    r->stencil_zfail     = s_reg[0x0374 / 4];
    r->stencil_zpass     = s_reg[0x0378 / 4];
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb);

int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    Texture saved = s_gpu.texs[0];
    uint32_t x, y;
    int ok = 1;

    s_gpu.texs[0].offset = tex->offset;
    s_gpu.texs[0].width  = tex->width;
    s_gpu.texs[0].height = tex->height;
    s_gpu.texs[0].pitch  = tex->pitch;
    s_gpu.texs[0].color  = tex->color;
    s_gpu.texs[0].addr_u = 3;
    s_gpu.texs[0].addr_v = 3;
    s_gpu.texs[0].palette = 0;
    s_gpu.texs[0].levels = 1;
    s_gpu.texs[0].filter = 0;
    s_gpu.texs[0].cube   = 0;
    s_gpu.texs[0].valid  = 1;
    for (y = 0; y < tex->height && ok; y++)
        for (x = 0; x < tex->width; x++)
            if (!sample_texture(x, y, &argb_out[(size_t)y * tex->width + x])) {
                ok = 0;
                break;
            }
    s_gpu.texs[0] = saved;
    return ok;
}

static int s_swz_on;
static uint32_t s_swz_mx, s_swz_my;

static void surface_swizzle_update(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_SWIZZLED_RT");
        enabled = !(e && e[0] == '0');
    }
    s_swz_on = enabled && ((s_gpu.format >> 8) & 0xF) == 2;
    if (s_swz_on)
        xbox_swizzle_masks(1u << ((s_gpu.format >> 16) & 0xF),
                           1u << ((s_gpu.format >> 24) & 0xF),
                           &s_swz_mx, &s_swz_my);
}

static void gpu_fill_state(NvGpuState *st, int draw);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (s_backend && s_backend->clear) {
        Nv2aSurface surf;
        Nv2aRenderState rs;

        if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
            return;
        current_surface(&surf);
        current_render_state(&rs);
        s_backend->clear(&surf, &rs, param, s_gpu.clear_color,
                         s_gpu.zstencil_clear);
        s_gpu.clears++;
        return;
    }

    if (nv2a_gpu_enabled()) {
        NvGpuState st;
        uint32_t x0 = s_gpu.clip_x, x1 = s_gpu.clip_x + s_gpu.clip_w;
        uint32_t y0 = s_gpu.clip_y, y1 = s_gpu.clip_y + s_gpu.clip_h;
        uint32_t zs = s_gpu.zstencil_clear;
        int z16 = ((s_gpu.format >> 4) & 0xF) == 1;
        float z01 = z16 ? (float)(zs & 0xFFFF) / 65535.0f
                        : (float)(zs >> 8) / 16777215.0f;

        if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h
            || (bpp != 2 && bpp != 4))
            return;
        if (((s_gpu.format >> 8) & 0xF) == 2 && (s_gpu.clear_rect_set & 3) == 3) {
            x0 = s_gpu.clear_rect_h & 0xFFFF; x1 = (s_gpu.clear_rect_h >> 16) + 1;
            y0 = s_gpu.clear_rect_v & 0xFFFF; y1 = (s_gpu.clear_rect_v >> 16) + 1;
        }
        gpu_fill_state(&st, 0);
        nv2a_gpu_clear(&st, param, s_gpu.clear_color, z01, zs & 0xFF, x0, y0, x1, y1);
        s_gpu.clears++;
        if (s_gpu.flips == 0) {
            xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);
            xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
        }
        xbox_FramebufferWindowStart();
        return;
    }
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    surface_swizzle_update();
    if (s_swz_on) {

        uint8_t *base = mem + s_gpu.color_base;
        uint32_t sw = 1u << ((s_gpu.format >> 16) & 0xF);
        uint32_t sh = 1u << ((s_gpu.format >> 24) & 0xF);
        uint32_t x0 = s_gpu.clip_x, x1 = s_gpu.clip_x + s_gpu.clip_w;
        uint32_t y0 = s_gpu.clip_y, y1 = s_gpu.clip_y + s_gpu.clip_h;
        uint16_t v16 = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                | ((s_gpu.clear_color >> 5) & 0x07E0)
                                | ((s_gpu.clear_color >> 3) & 0x001F));
        if ((s_gpu.clear_rect_set & 3) == 3) {
            x0 = s_gpu.clear_rect_h & 0xFFFF; x1 = (s_gpu.clear_rect_h >> 16) + 1;
            y0 = s_gpu.clear_rect_v & 0xFFFF; y1 = (s_gpu.clear_rect_v >> 16) + 1;
        }
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                size_t i = swizzle_deposit(x, s_swz_mx) | swizzle_deposit(y, s_swz_my);
                if (bpp == 4)
                    ((uint32_t *)base)[i] = s_gpu.clear_color;
                else if (bpp == 2)
                    ((uint16_t *)base)[i] = v16;
            }
    } else
    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {

            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;

    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);

    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;

        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    if (s_gpu.flips == 0) {
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);
        xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
    }

    xbox_FramebufferWindowStart();
}

static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{

    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;

    if (!(size & (size - 1))) {
        if (mode == 1)
            return c & (size - 1);
        if (mode == 2) {
            uint32_t m = c & (2 * size - 1);
            return m < size ? m : 2 * size - 1 - m;
        }
    }
    if (mode == 1)
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {
        int32_t m = ((sc % (2 * n)) + 2 * n) % (2 * n);
        return (uint32_t)(m < n ? m : 2 * n - 1 - m);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;
    case 0x02: return 0x10;
    case 0x03: return 0x1C;
    case 0x04: return 0x1D;
    case 0x05: return 0x11;
    case 0x06: return 0x12;
    case 0x07: return 0x1E;
    case 0x19: return 0x1F;
    default:   return fmt;
    }
}

#define NV_SWZ_LOG_MAX 13
static uint32_t *volatile s_swz_tab[NV_SWZ_LOG_MAX][NV_SWZ_LOG_MAX];

static const uint32_t *swz_tables(uint32_t w, uint32_t h, uint32_t lw, uint32_t lh)
{
    uint32_t *tab = s_swz_tab[lw][lh], i;

    if (tab)
        return tab;
    tab = (uint32_t *)malloc((w + h) * sizeof *tab);
    if (!tab)
        return NULL;
    for (i = 0; i < w; i++)
        tab[i] = swizzle_offset(i, 0, w, h);
    for (i = 0; i < h; i++)
        tab[w + i] = swizzle_offset(0, i, w, h);
    s_swz_tab[lw][lh] = tab;
    return tab;
}

static inline uint32_t swizzle_fast(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    if (!(w & (w - 1)) && !(h & (h - 1))) {
        uint32_t lw = (uint32_t)__builtin_ctz(w), lh = (uint32_t)__builtin_ctz(h);
        if (lw < NV_SWZ_LOG_MAX && lh < NV_SWZ_LOG_MAX && u < w && v < h) {
            const uint32_t *tab = swz_tables(w, h, lw, lh);
            if (tab)
                return tab[u] | tab[w + v];
        }
    }
    return swizzle_offset(u, v, w, h);
}

static int sample_tex(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset;

    if (!t->valid)
        return 0;

    if (t->border_color
        && ((t->addr_u == 4 && u >= t->width) || (t->addr_v == 4 && v >= t->height))) {
        *argb = t->border;
        return 1;
    }
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {

        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = d3d8_format_dxt_block_bytes(fmt);
        uint32_t bx = u >> 2, by = v >> 2, bw = (t->width + 3) >> 2;
        const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(mem + base, fmt, bx * 4 + (i & 3),
                                      by * 4 + (i >> 2), t->width,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {

        fmt = linear_twin(fmt);
        p = mem + base;
        u = swizzle_fast(u, v, t->width, t->height);
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }

    switch (fmt) {

    case 0x12:
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    case 0x0B:
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        return 1;

    case 0x3F: {
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)
              | (((t >>  8) & 0xFFu) << 16)
              | (((t >> 16) & 0xFFu) <<  8)
              |  ((t >> 24) & 0xFFu);
        return 1;
    }
    case 0x41: {
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    case 0x10: {
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    case 0x13: {
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    case 0x24:
    case 0x25: {
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;

            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

static uint8_t *s_surface;

static inline size_t surface_index(int x, int y, uint8_t **row)
{
    if (s_swz_on) {
        *row = s_surface;
        return swizzle_deposit((uint32_t)x, s_swz_mx)
             | swizzle_deposit((uint32_t)y, s_swz_my);
    }
    *row = s_surface + (size_t)y * s_gpu.pitch;
    return (size_t)x;
}

int nv2a_exec_tex_decode(const void *tex, uint32_t face_offset, uint32_t *argb_out)
{
    const Texture *t = (const Texture *)tex;
    uint32_t x, y;

    for (y = 0; y < t->height; y++)
        for (x = 0; x < t->width; x++)
            if (!sample_tex(t, face_offset, x, y, &argb_out[(size_t)y * t->width + x]))
                return 0;
    return 1;
}

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    surface_swizzle_update();

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;

    if (nv2a_gpu_enabled())
        nv2a_gpu_cpu_write(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch);
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static uint32_t pack_color(const float c[4]);

static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;
        case 0x0001: out[k] = 1.0f;            break;
        case 0x0300: out[k] = s[k];            break;
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;
    size_t xi;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;

    if (s_swz_on && ((uint32_t)x >> ((s_gpu.format >> 16) & 0xF)
                     || (uint32_t)y >> ((s_gpu.format >> 24) & 0xF)))
        return;

    xi = surface_index(x, y, &row);

    if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[xi];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[xi];
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;
            case 0x800B: out[k] = b - a; break;
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break;
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break;
            default:     out[k] = a + b; break;
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[xi];
        } else {
            uint32_t t = ((const uint16_t *)row)[xi];
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[xi] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[xi] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

static void tex_filter_config(int *bilinear, int *aniso)
{
    static int b = -1, n = -1;

    if (b < 0) {
        const char *e = getenv("RECOMP_TEX_BILINEAR");
        b = e && strcmp(e, "0") != 0;
    }
    if (n < 0) {
        const char *e = getenv("RECOMP_TEX_ANISO");
        int v = e ? atoi(e) : 0;
        n = v >= 16 ? 16 : v >= 8 ? 8 : 0;
    }
    *aniso = n;
    *bilinear = b || n;
}

static int bilinear_accum(float su, float sv, uint32_t acc[4])
{
    const Texture *t = &s_gpu.texs[0];
    float fx = su - 0.5f, fy = sv - 0.5f;
    int32_t x0 = (int32_t)floorf(fx), y0 = (int32_t)floorf(fy);
    uint32_t wx = (uint32_t)((fx - (float)x0) * 256.0f);
    uint32_t wy = (uint32_t)((fy - (float)y0) * 256.0f);
    uint32_t c[4], w[4], i, ch;

    if (wx > 255) wx = 255;
    if (wy > 255) wy = 255;
    if (!sample_tex(t, 0, (uint32_t)x0,       (uint32_t)y0,       &c[0])
     || !sample_tex(t, 0, (uint32_t)(x0 + 1), (uint32_t)y0,       &c[1])
     || !sample_tex(t, 0, (uint32_t)x0,       (uint32_t)(y0 + 1), &c[2])
     || !sample_tex(t, 0, (uint32_t)(x0 + 1), (uint32_t)(y0 + 1), &c[3]))
        return 0;
    w[0] = (256 - wx) * (256 - wy);
    w[1] = wx * (256 - wy);
    w[2] = (256 - wx) * wy;
    w[3] = wx * wy;
    for (ch = 0; ch < 4; ch++)
        for (i = 0; i < 4; i++)
            acc[ch] += ((c[i] >> (8 * ch)) & 0xFFu) * w[i];
    return 1;
}

static uint32_t filter_result(const uint32_t acc[4], uint32_t taps)
{
    uint32_t total = taps << 16, out = 0, ch;

    for (ch = 0; ch < 4; ch++)
        out |= ((acc[ch] + total / 2) / total) << (8 * ch);
    return out;
}

static uint32_t aniso_footprint(const float gx[2], const float gy[2],
                                uint32_t max_taps, float start[2], float step[2])
{
    float lx = sqrtf(gx[0] * gx[0] + gx[1] * gx[1]);
    float ly = sqrtf(gy[0] * gy[0] + gy[1] * gy[1]);
    const float *major = lx >= ly ? gx : gy;
    float longer = lx >= ly ? lx : ly, shorter = lx >= ly ? ly : lx;
    uint32_t n = 1;

    if (longer > 1e-6f) {
        float ratio = shorter > 1e-6f ? longer / shorter : (float)max_taps;
        n = ratio >= (float)max_taps ? max_taps : (uint32_t)ceilf(ratio);
        if (n < 1) n = 1;
    }
    step[0] = major[0] / (float)n;
    step[1] = major[1] / (float)n;
    start[0] = -0.5f * major[0] + 0.5f * step[0];
    start[1] = -0.5f * major[1] + 0.5f * step[1];
    return n;
}

static uint32_t aniso_taps(const float a[2], const float b[2], const float c[2],
                           const float uv[3][2], float area, uint32_t max_taps,
                           float start[2], float step[2])
{
    float e1x = b[0] - a[0], e1y = b[1] - a[1];
    float e2x = c[0] - a[0], e2y = c[1] - a[1];
    float du1 = uv[1][0] - uv[0][0], du2 = uv[2][0] - uv[0][0];
    float dv1 = uv[1][1] - uv[0][1], dv2 = uv[2][1] - uv[0][1];
    float gx[2] = { (du1 * e2y - du2 * e1y) / area, (dv1 * e2y - dv2 * e1y) / area };
    float gy[2] = { (du2 * e1x - du1 * e2x) / area, (dv2 * e1x - dv1 * e2x) / area };

    return aniso_footprint(gx, gy, max_taps, start, step);
}

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;
    int bilinear = 0, aniso = 0;
    uint32_t taps = 1;
    float tap_start[2] = {0.0f, 0.0f}, tap_step[2] = {0.0f, 0.0f};

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;

    if (textured) {
        tex_filter_config(&bilinear, &aniso);
        if (aniso)
            taps = aniso_taps(a, b, c, uv, area, (uint32_t)aniso, tap_start, tap_step);
    }

    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {

                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (bilinear) {

                    uint32_t acc[4] = {0, 0, 0, 0}, i;
                    for (i = 0; i < taps; i++)
                        if (!bilinear_accum(su + tap_start[0] + (float)i * tap_step[0],
                                            sv + tap_start[1] + (float)i * tap_step[1], acc))
                            break;
                    if (i == taps) {
                        put_pixel(mem, bpp, x, y, filter_result(acc, taps));
                        continue;
                    }
                    put_pixel(mem, bpp, x, y, argb);
                    continue;
                }
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

static int batch_is_ffp(void)
{
    return s_gpu.composite_set && (s_gpu.xf_mode & 3) != 2
        && !s_gpu.inline_active;
}

#define NV_LIGHTS 8

static int strip_alt_winding(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_STRIP_ALT_WINDING");
        on = e && strcmp(e, "0") != 0;
    }
    return on;
}

static int ffp_flip_normals(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("RECOMP_FFP_FLIP_NORMALS") != NULL;
    return on;
}

static int lit_color(uint32_t index, float out[4])
{
    const float *mv = (const float *)&s_reg[0x0480 / 4];
    float pos[4], nrm[4], vc[4], e[3], n[3], len;
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4];
    int i, have_vc;

    if (!s_reg[0x0314 / 4] || !s_gpu.attr[2].size)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], index, pos)
     || !fetch_attr(&s_gpu.attr[2], index, nrm))
        return 0;
    have_vc = fetch_attr(color_attr(), index, vc);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1]
             + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1]
             + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }

    if (ffp_flip_normals() && n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    out[0] = reg_f(0x0A10); out[1] = reg_f(0x0A14); out[2] = reg_f(0x0A18);
    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + (uint32_t)i * 0x80;
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (!kind)
            continue;
        if (kind == 1) {
            l[0] = reg_f(b + 0x34); l[1] = reg_f(b + 0x38); l[2] = reg_f(b + 0x3C);
        } else {
            float d;
            l[0] = reg_f(b + 0x5C) - e[0];
            l[1] = reg_f(b + 0x60) - e[1];
            l[2] = reg_f(b + 0x64) - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > reg_f(b + 0x24) && reg_f(b + 0x24) > 0.0f)
                continue;
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = reg_f(b + 0x68) + reg_f(b + 0x6C) * d + reg_f(b + 0x70) * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = reg_f(b + 0x00 + 4 * k);
            dif[k] = reg_f(b + 0x0C + 4 * k);

            if (have_vc && ((colmat >> 2) & 3) == 1) amb[k] *= vc[k];
            if (have_vc && ((colmat >> 4) & 3) == 1) dif[k] *= vc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
    out[3] = (have_vc && ((colmat >> 4) & 3) == 1) ? vc[3] : reg_f(0x03B4);
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}

static int ffp_vertex(uint32_t index, Nv2aVshOutput *out)
{
    const float *m = s_gpu.composite, *off = nv2a_vsh_constant(59);
    float in[4], clip[4], c[4], w;
    uint32_t st;
    int i;

    if (!fetch_attr(&s_gpu.attr[0], index, in))
        return 0;
    for (i = 0; i < 4; i++)
        clip[i] = m[4 * i] * in[0] + m[4 * i + 1] * in[1]
                + m[4 * i + 2] * in[2] + m[4 * i + 3] * in[3];

    w = clip[3];
    if (fabsf(w) <= 1e-6f)
        return 0;

    memset(out, 0, sizeof *out);
    out->pos[0] = (clip[0] / w + off[0]) * s_gpu.aa_sx;
    out->pos[1] = (clip[1] / w + off[1]) * s_gpu.aa_sy;
    out->pos[2] = clip[2] / w + off[2];
    out->pos[3] = w;

    if (!lit_color(index, c) && !fetch_attr(color_attr(), index, c))
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    for (i = 0; i < 4; i++)
        out->d0[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];

    fetch_attr(texcoord_attr(), index, out->tex[0]);
    for (st = 1; st < 4; st++)
        fetch_attr(&s_gpu.attr[9 + st], index, out->tex[st]);
    return 1;
}

#define OBJECT_SPACE_SPAN 8.0f

static float aa_scale_x(void) { return s_gpu.aa_sx > 1.0f ? s_gpu.aa_sx : 1.0f; }
static float aa_scale_y(void) { return s_gpu.aa_sy > 1.0f ? s_gpu.aa_sy : 1.0f; }

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    if (hi_x * aa_scale_x() < (float)s_gpu.clip_x
     || lo_x * aa_scale_x() > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y * aa_scale_y() < (float)s_gpu.clip_y
     || lo_y * aa_scale_y() > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

static int s_xf_no_tex;

#define NV_BACKEND_MAX_VERTS (NV_MAX_INDICES * 3)
static Nv2aVertex *s_bverts;
static uint32_t   s_bcount;

static Nv2aVertex *backend_slot(void)
{
    if (!s_bverts
        && !(s_bverts = (Nv2aVertex *)calloc(NV_BACKEND_MAX_VERTS, sizeof *s_bverts)))
        return NULL;
    if (s_bcount + 3 > NV_BACKEND_MAX_VERTS)
        return NULL;
    return &s_bverts[s_bcount];
}

static void backend_from_xf(const Nv2aVshOutput *v, Nv2aVertex *o)
{
    float su = 1.0f, sv = 1.0f;

    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    o->x = v->pos[0];
    o->y = v->pos[1];
    o->z = v->pos[2];
    o->rhw = 1.0f / v->pos[3];
    o->diffuse = pack_color(v->d0);
    o->u = v->tex[0][0] * su;
    o->v = v->tex[0][1] * sv;
}

static void backend_flush(int textured)
{
    Nv2aSurface surf;
    Nv2aRenderState rs;
    Nv2aTexture tex;
    Nv2aBatch batch;

    if (!s_bcount)
        return;
    current_surface(&surf);
    current_render_state(&rs);
    batch.vertices = s_bverts;
    batch.count = s_bcount;
    batch.texture = NULL;
    batch.state = &rs;
    if (textured && s_gpu.texs[0].valid) {
        tex.offset = s_gpu.texs[0].offset;
        tex.width  = s_gpu.texs[0].width;
        tex.height = s_gpu.texs[0].height;
        tex.pitch  = s_gpu.texs[0].pitch;
        tex.color  = s_gpu.texs[0].color;
        tex.addr_u = s_gpu.texs[0].addr_u;
        tex.addr_v = s_gpu.texs[0].addr_v;
        tex.levels = s_gpu.texs[0].pitch ? 1 : s_gpu.texs[0].levels;
        tex.filter = s_gpu.texs[0].filter;
        batch.texture = &tex;
    }
    s_backend->draw(&surf, &batch);
    s_gpu.tris_drawn += s_bcount / 3;
    s_bcount = 0;
    note_drawn();
}

static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured, j;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;
    for (j = 0; j < 3; j++) {
        p[j][0] *= aa_scale_x();
        p[j][1] *= aa_scale_y();
    }

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    if (backend_draws()) {
        Nv2aVertex *o = backend_slot();
        const uint32_t ix[3] = { i0, i1, i2 };
        int k;

        if (!o)
            return;
        for (k = 0; k < 3; k++) {
            o[k].x = p[k][0];
            o[k].y = p[k][1];
            o[k].z = p[k][2];
            o[k].rhw = p[k][3];
            o[k].diffuse = vertex_color(ix[k]);
            o[k].u = textured ? uv[k][0] : 0.0f;
            o[k].v = textured ? uv[k][1] : 0.0f;
        }
        s_bcount += 3;
        return;
    }
    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

#define NV_ZBUF_W 1280
#define NV_ZBUF_H 1024
static struct { uint32_t offset; float *z; } s_zbufs[4];
static int s_ftrace;
static int s_zbuf_next;

static float zclear_value(void)
{

    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

static float *zbuf_current(int create)
{
    int i;
    size_t k;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == s_gpu.zeta_offset)
            return s_zbufs[i].z;
    if (!create)
        return NULL;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].z)
        return NULL;
    s_zbufs[i].offset = s_gpu.zeta_offset;
    for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    return s_zbufs[i].z;
}

static void zbuf_clear(void)
{
    float *z = zbuf_current(1), v = zclear_value();
    size_t k;
    if (z)
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
}

static int depth_pass(float z, float stored)
{
    switch (s_gpu.depth_func) {
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

static void rc_tent(const Texture *t, uint32_t face, float u, float v,
                    float out[4])
{
    uint32_t texel;
    float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
    int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;

    wu = fu - (float)iu;
    wv = fv - (float)iv;
    for (k = 0; k < 4; k++) {
        if (sample_tex(t, face, (uint32_t)(iu + (k & 1)),
                       (uint32_t)(iv + (k >> 1)), &texel))
            nv2a_rc_unpack(texel, c[k]);
        else
            c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
    }
    for (j = 0; j < 4; j++)
        out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
               + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
}

static void rc_texel(const Texture *t, uint32_t face, float u, float v,
                     const float *g, float out[4])
{
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int bilinear, aniso;
    float gs[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    tex_filter_config(&bilinear, &aniso);
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
        if (g) {
            gs[0] = g[0] * (float)t->width;  gs[1] = g[1] * (float)t->height;
            gs[2] = g[2] * (float)t->width;  gs[3] = g[3] * (float)t->height;
            g = gs;
        }
    }

    if (aniso && g) {
        float gx[2] = { g[0], g[1] }, gy[2] = { g[2], g[3] }, start[2], step[2];
        float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tap[4];
        uint32_t n = aniso_footprint(gx, gy, (uint32_t)aniso, start, step), i;
        int j;
        for (i = 0; i < n; i++) {
            rc_tent(t, face, u + start[0] + (float)i * step[0],
                    v + start[1] + (float)i * step[1], tap);
            for (j = 0; j < 4; j++)
                sum[j] += tap[j];
        }
        for (j = 0; j < 4; j++)
            out[j] = sum[j] / (float)n;
        return;
    }

    if (bilinear || mag == 2 || min == 2 || min == 4 || min == 6) {
        rc_tent(t, face, u, v, out);
        return;
    }
    if (sample_tex(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v), &texel))
        nv2a_rc_unpack(texel, out);
    else
        out[0] = out[1] = out[2] = out[3] = 1.0f;
}

static int rc_stage_fetch(int st, const float c[4], const float *cx,
                          const float *cy, float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:
        if (cx && cy) {
            float qx = cx[3] != 0.0f ? cx[3] : 1.0f, qy = cy[3] != 0.0f ? cy[3] : 1.0f;
            float u = c[0] / q, v = c[1] / q;
            float g[4] = { cx[0] / qx - u, cx[1] / qx - v,
                           cy[0] / qy - u, cy[1] / qy - v };
            rc_texel(t, 0, u, v, g, out);
            return 1;
        }
        rc_texel(t, 0, c[0] / q, c[1] / q, NULL, out);
        return 1;
    case 3: {
        float x = c[0], y = c[1], z = c[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
        uint32_t face;
        if (!t->cube) {
            rc_texel(t, 0, c[0], c[1], NULL, out);
            return 1;
        }
        if (ax >= ay && ax >= az) {
            face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
        } else {
            face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
        }
        if (ma == 0.0f)
            ma = 1.0f;
        rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
                 (tc / ma + 1.0f) * 0.5f, NULL, out);
        return 1;
    }
    case 4:
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:

        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
}

static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    switch (s_gpu.alpha_func) {
    case 0x200: return 0;
    case 0x201: return v <  r;
    case 0x202: return v == r;
    case 0x203: return v <= r;
    case 0x204: return v >  r;
    case 0x205: return v != r;
    case 0x206: return v >= r;
    default:    return 1;
    }
}

typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float dl[3][2];
    float *zb;
    int minx, maxx, miny, maxy, use_rc, textured;
} XfTri;

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;

static float xf_step_l(const XfTri *T, float l0, float l1, float l2, int axis,
                       float *m0, float *m1, float *m2)
{
    *m0 = l0 + T->dl[0][axis];
    *m1 = l1 + T->dl[1][axis];
    *m2 = l2 + T->dl[2][axis];
    return *m0 * T->iw[0] + *m1 * T->iw[1] + *m2 * T->iw[2];
}

static void xf_tc_step(const XfTri *T, int st, float l0, float l1, float l2,
                       int axis, float out[4])
{
    float m0, m1, m2, pw = xf_step_l(T, l0, l1, l2, axis, &m0, &m1, &m2);
    int j;

    for (j = 0; j < 4; j++)
        out[j] = (m0 * T->stc[0][st][j] + m1 * T->stc[1][st][j]
                + m2 * T->stc[2][st][j]) / pw;
}

static void xf_uv_step(const XfTri *T, float l0, float l1, float l2, int axis,
                       float *u, float *v)
{
    float m0, m1, m2, pw = xf_step_l(T, l0, l1, l2, axis, &m0, &m1, &m2);

    *u = (m0 * T->uv[0][0] + m1 * T->uv[1][0] + m2 * T->uv[2][0]) / pw;
    *v = (m0 * T->uv[0][1] + m1 * T->uv[1][1] + m2 * T->uv[2][1]) / pw;
}

static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k, bilinear, aniso;

    tex_filter_config(&bilinear, &aniso);
    for (y = y0; y < T->maxy; y += step) {
        for (x = T->minx; x < T->maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;

            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (s_gpu.color_mask & 0x01010101u) || s_gpu.alpha_test;

                if (zp && s_gpu.depth_test && !depth_pass(z, *zp)) {
                    cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            for (j = 0; j < 4; j++)
                                tc[j] = (l0 * T->stc[0][st][j] + l1 * T->stc[1][st][j]
                                       + l2 * T->stc[2][st][j]) / pw;
                            if (aniso) {

                                float tcx[4], tcy[4];
                                xf_tc_step(T, st, l0, l1, l2, 0, tcx);
                                xf_tc_step(T, st, l0, l1, l2, 1, tcy);
                                keep = rc_stage_fetch(st, tc, tcx, tcy, t[st]);
                            } else {
                                keep = rc_stage_fetch(st, tc, NULL, NULL, t[st]);
                            }
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (bilinear) {
                                uint32_t acc[4] = {0, 0, 0, 0}, n = 1, i;
                                float tap0[2] = {0.0f, 0.0f}, dtap[2] = {0.0f, 0.0f};
                                if (aniso) {
                                    float ux, vx, uy, vy, gx[2], gy[2];
                                    xf_uv_step(T, l0, l1, l2, 0, &ux, &vx);
                                    xf_uv_step(T, l0, l1, l2, 1, &uy, &vy);
                                    gx[0] = ux - tu; gx[1] = vx - tv;
                                    gy[0] = uy - tu; gy[1] = vy - tv;
                                    n = aniso_footprint(gx, gy, (uint32_t)aniso, tap0, dtap);
                                }
                                for (i = 0; i < n; i++)
                                    if (!bilinear_accum(tu + tap0[0] + (float)i * dtap[0],
                                                        tv + tap0[1] + (float)i * dtap[1], acc))
                                        break;
                                if (i == n)
                                    argb = modulate(filter_result(acc, n), col);
                            } else if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                                      (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (zp && s_gpu.depth_mask)
                    *zp = z;
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (!(s_gpu.color_mask & 0x01010101u))
                    continue;
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

volatile long long g_nv2a_raster_ticks, g_nv2a_raster_flushes;

#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 2048
static long s_mt_min_pixels = NV_RASTER_MT_MIN_PIXELS;

static void xf_rows_owned(const XfTri *tris, int ntris, int k, int n,
                          XfCount *cnt)
{
    int i;
    for (i = 0; i < ntris; i++) {
        int y0 = tris[i].miny + ((k - tris[i].miny) % n + n) % n;
        xf_rows(&tris[i], y0, n, cnt);
    }
}

static int s_raster_last_workers = 1;
int nv2a_raster_last_workers(void) { return s_raster_last_workers; }

#if defined(_WIN32)

#include <immintrin.h>
static struct {
    int n;
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    volatile LONG generation;
    volatile LONG caller_sleeping;
    volatile LONG sleeping[NV_RASTER_MAX_THREADS];
    int spin;
    const XfTri *tris;
    int ntris;
    XfCount cnt[NV_RASTER_MAX_THREADS];

    void (*job)(int k, int n);
} s_pool;

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    LONG seen = 0;
    for (;;) {
        XfCount mine = {0, 0, 0};

        LONG g;
        int spins = 0;
        while ((g = s_pool.generation) == seen) {
            if (spins++ < s_pool.spin) {
                _mm_pause();
                continue;
            }
            InterlockedExchange(&s_pool.sleeping[k], 1);
            if (s_pool.generation == seen)
                WaitForSingleObject(s_pool.start[k], INFINITE);
            InterlockedExchange(&s_pool.sleeping[k], 0);
            spins = 0;
        }
        seen = g;
        if (s_pool.job)
            s_pool.job(k, s_pool.n);
        else
            xf_rows_owned(s_pool.tris, s_pool.ntris, k, s_pool.n, &mine);
        s_pool.cnt[k] = mine;
        if (InterlockedDecrement(&s_pool.pending) == 0 && s_pool.caller_sleeping)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        {
            const char *sp = getenv("RECOMP_RASTER_SPIN");
            const char *mt = getenv("RECOMP_RASTER_MT_MIN");

            s_pool.spin = sp ? atoi(sp) : 20000;
            if (mt) s_mt_min_pixels = atol(mt);
        }
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void pool_run_job(void (*job)(int k, int n))
{
    int n = raster_pool_size(), k, spins = 0;

    if (n <= 1) {
        job(0, 1);
        return;
    }
    s_pool.job = job;
    s_pool.pending = n - 1;
    InterlockedIncrement(&s_pool.generation);
    for (k = 0; k < n - 1; k++)
        if (s_pool.sleeping[k])
            SetEvent(s_pool.start[k]);
    job(n - 1, n);
    while (s_pool.pending != 0) {
        if (spins++ < 200000) {
            _mm_pause();
            continue;
        }
        InterlockedExchange(&s_pool.caller_sleeping, 1);
        if (s_pool.pending != 0)
            WaitForSingleObject(s_pool.done, INFINITE);
        InterlockedExchange(&s_pool.caller_sleeping, 0);
    }
    MemoryBarrier();
    s_pool.job = NULL;
}

static void xf_rows_parallel(const XfTri *T, int ntris, long px,
                             XfCount *total)
{
    int n = raster_pool_size(), k;

    if (n <= 1 || px < s_mt_min_pixels) {
        for (k = 0; k < ntris; k++)
            xf_rows(&T[k], T[k].miny, 1, total);
        return;
    }
    s_pool.job = NULL;
    s_pool.tris = T;
    s_pool.ntris = ntris;
    s_pool.pending = n - 1;
    InterlockedIncrement(&s_pool.generation);
    for (k = 0; k < n - 1; k++)
        if (s_pool.sleeping[k])
            SetEvent(s_pool.start[k]);
    {
        XfCount mine = {0, 0, 0};
        int spins = 0;
        xf_rows_owned(T, ntris, n - 1, n, &mine);
        while (s_pool.pending != 0) {
            if (spins++ < 200000) {
                _mm_pause();
                continue;
            }
            InterlockedExchange(&s_pool.caller_sleeping, 1);
            if (s_pool.pending != 0)
                WaitForSingleObject(s_pool.done, INFINITE);
            InterlockedExchange(&s_pool.caller_sleeping, 0);
        }
        MemoryBarrier();
        total->depth_fail += mine.depth_fail;
        total->pixels += mine.pixels;
        total->zpass += mine.zpass;
    }
    {
        int ran = 1;
        for (k = 0; k < n - 1; k++) {
            if (s_pool.cnt[k].pixels) ran++;
            total->depth_fail += s_pool.cnt[k].depth_fail;
            total->pixels += s_pool.cnt[k].pixels;
            total->zpass += s_pool.cnt[k].zpass;
        }
        s_raster_last_workers = ran;
    }
}
#else

static struct {
    int n;
    pthread_mutex_t lock;
    pthread_cond_t start, done;
    unsigned long generation;
    int pending;
    const XfTri *tris;
    int ntris;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool;

static void *raster_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    unsigned long seen = 0;
    for (;;) {
        pthread_mutex_lock(&s_pool.lock);
        while (s_pool.generation == seen)
            pthread_cond_wait(&s_pool.start, &s_pool.lock);
        seen = s_pool.generation;
        pthread_mutex_unlock(&s_pool.lock);

        memset(&s_pool.cnt[k], 0, sizeof s_pool.cnt[k]);
        xf_rows_owned(s_pool.tris, s_pool.ntris, k, s_pool.n, &s_pool.cnt[k]);

        pthread_mutex_lock(&s_pool.lock);
        if (--s_pool.pending == 0)
            pthread_cond_signal(&s_pool.done);
        pthread_mutex_unlock(&s_pool.lock);
    }
    return NULL;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        int n, k;
        init = 1;

        n = e ? atoi(e) : (int)(cpus > 0 ? cpus : 1) - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            pthread_mutex_init(&s_pool.lock, NULL);
            pthread_cond_init(&s_pool.start, NULL);
            pthread_cond_init(&s_pool.done, NULL);
            for (k = 0; k < n - 1; k++) {
                pthread_t t;
                if (pthread_create(&t, NULL, raster_worker,
                                   (void *)(intptr_t)k) != 0) {

                    s_pool.n = k + 1;
                    break;
                }
                pthread_detach(t);
            }
        }
    }
    return s_pool.n;
}

static void xf_rows_parallel(const XfTri *T, int ntris, long px,
                             XfCount *total)
{
    int n = raster_pool_size(), k;

    if (n <= 1 || px < s_mt_min_pixels) {
        s_raster_last_workers = 1;
        for (k = 0; k < ntris; k++)
            xf_rows(&T[k], T[k].miny, 1, total);
        return;
    }

    pthread_mutex_lock(&s_pool.lock);
    s_pool.tris = T;
    s_pool.ntris = ntris;
    s_pool.pending = n - 1;
    s_pool.generation++;
    pthread_cond_broadcast(&s_pool.start);
    pthread_mutex_unlock(&s_pool.lock);

    {
        XfCount mine = {0, 0, 0};
        xf_rows_owned(T, ntris, n - 1, n, &mine);
        pthread_mutex_lock(&s_pool.lock);
        while (s_pool.pending != 0)
            pthread_cond_wait(&s_pool.done, &s_pool.lock);
        pthread_mutex_unlock(&s_pool.lock);
        total->depth_fail += mine.depth_fail;
        total->pixels += mine.pixels;
        total->zpass += mine.zpass;
    }

    {
        int ran = 1;
        for (k = 0; k < n - 1; k++) {
            if (s_pool.cnt[k].pixels) ran++;
            total->depth_fail += s_pool.cnt[k].depth_fail;
            total->pixels += s_pool.cnt[k].pixels;
            total->zpass += s_pool.cnt[k].zpass;
        }
        s_raster_last_workers = ran;
    }
}

static void pool_run_job(void (*job)(int k, int n))
{
    job(0, 1);
}
#endif

int nv2a_raster_pool_size(void) { return raster_pool_size(); }

#define XF_QUEUE_MAX 2048
typedef struct { XfTri t; Nv2aVshOutput v[3]; } XfQueued;
static XfQueued *s_xfq;
static XfTri s_xfq_tris[XF_QUEUE_MAX];
static int s_xfq_n;
static long s_xfq_px;

static void xf_queue_flush(void)
{
    XfCount cnt = {0, 0, 0};
    int i;

    if (!s_xfq_n)
        return;
    for (i = 0; i < s_xfq_n; i++)
        s_xfq_tris[i] = s_xfq[i].t;
#if defined(_WIN32)
    {
        extern volatile long long g_nv2a_raster_ticks, g_nv2a_raster_flushes;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        xf_rows_parallel(s_xfq_tris, s_xfq_n, s_xfq_px, &cnt);
        QueryPerformanceCounter(&t1);
        g_nv2a_raster_ticks += t1.QuadPart - t0.QuadPart;
        g_nv2a_raster_flushes++;
    }
#else
    xf_rows_parallel(s_xfq_tris, s_xfq_n, s_xfq_px, &cnt);
#endif
    s_gpu.xf_depth_fail += cnt.depth_fail;
    s_gpu.xf_pixels += cnt.pixels;
    s_gpu.pixels += cnt.pixels;
    s_gpu.zpass_count += cnt.zpass;
    s_xfq_n = 0;
    s_xfq_px = 0;
}

static void xf_queue_push(const XfTri *T)
{
    XfQueued *q;

    if (!s_xfq && !(s_xfq = (XfQueued *)malloc(XF_QUEUE_MAX * sizeof *s_xfq))) {
        XfCount cnt = {0, 0, 0};
        xf_rows_parallel(T, 1, 0, &cnt);
        return;
    }
    if (s_xfq_n == XF_QUEUE_MAX)
        xf_queue_flush();
    q = &s_xfq[s_xfq_n++];
    q->t = *T;
    q->v[0] = *T->va; q->v[1] = *T->vb; q->v[2] = *T->vc;
    q->t.va = &q->v[0]; q->t.vb = &q->v[1]; q->t.vc = &q->v[2];
    q->t.a = q->v[0].pos; q->t.b = q->v[1].pos; q->t.c = q->v[2].pos;
    s_xfq_px += (long)(T->maxx - T->minx) * (T->maxy - T->miny);
}

static int s_gpu_pending;

static void gpu_fill_state(NvGpuState *st, int draw)
{
    static int no_rc = -1;
    int bilinear, aniso, s;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(st, 0, sizeof *st);
    st->color_va = dma_resolve(s_gpu.color_offset);
    st->pitch = s_gpu.pitch;
    st->bpp = surface_bpp();
    st->swizzled = ((s_gpu.format >> 8) & 0xF) == 2;
    st->swz_w = 1u << ((s_gpu.format >> 16) & 0xF);
    st->swz_h = 1u << ((s_gpu.format >> 24) & 0xF);
    st->clip_x = s_gpu.clip_x; st->clip_y = s_gpu.clip_y;
    st->clip_w = s_gpu.clip_w; st->clip_h = s_gpu.clip_h;
    st->zeta_key = s_gpu.zeta_offset;
    st->z16 = ((s_gpu.format >> 4) & 0xF) == 1;
    st->depth_test = s_gpu.depth_test;
    st->depth_func = s_gpu.depth_func;
    st->depth_write = s_gpu.depth_mask;
    st->stencil_enable = s_reg[0x032C / 4];
    st->stencil_write_mask = s_reg[0x0360 / 4];
    st->stencil_func = s_reg[0x0364 / 4];
    st->stencil_ref = s_reg[0x0368 / 4];
    st->stencil_func_mask = s_reg[0x036C / 4];
    st->stencil_fail = s_reg[0x0370 / 4];
    st->stencil_zfail = s_reg[0x0374 / 4];
    st->stencil_zpass = s_reg[0x0378 / 4];
    st->blend_enable = s_gpu.blend_enable;
    st->blend_src = s_gpu.blend_sfactor;
    st->blend_dst = s_gpu.blend_dfactor;
    st->blend_eq = s_gpu.blend_equation;
    st->blend_color = s_gpu.blend_color;
    st->color_mask = s_gpu.color_mask;
    st->alpha_test = s_gpu.alpha_test;
    st->alpha_func = s_gpu.alpha_func;
    st->alpha_ref = s_gpu.alpha_ref;
    if (!draw)
        return;
    tex_filter_config(&bilinear, &aniso);
    st->use_rc = s_gpu.rc_seen && !no_rc;
    st->textured = s_gpu.texs[0].valid && !s_xf_no_tex;
    st->bilinear = (uint32_t)bilinear;
    st->rc = s_gpu.rc;
    st->fog_color = s_gpu.fog_color;
    st->clip_plane_mode = s_gpu.clip_plane_mode;
    st->count_zpass = s_gpu.zpass_enable;
    for (s = 0; s < 4; s++) {
        const Texture *t = &s_gpu.texs[s];
        NvGpuTex *g = &st->tex[s];
        uint32_t mode = (s_gpu.rc.stage_program >> (s * 5)) & 0x1F;
        int need = st->use_rc ? (mode >= 1 && mode <= 3) : (s == 0 && st->textured);

        if (!need || !t->valid)
            continue;
        g->src = t;
        g->offset = t->offset;
        g->face_stride = tex_face_stride(t);
        g->bytes = g->face_stride * (t->cube ? 6u : 1u);
        if (!guest_span_ok(t->offset, g->bytes))
            continue;
        g->valid = 1;
        g->palette = t->palette;
        g->width = t->width; g->height = t->height;
        g->pitch = t->pitch; g->color = t->color;
        g->addr_u = t->addr_u; g->addr_v = t->addr_v;
        g->filter = t->filter;
        g->border = t->border;
        g->border_color = (uint32_t)t->border_color;
        g->cube = (uint32_t)t->cube;
        g->normalized = (uint32_t)tex_size_from_format(t->color);
    }
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    XfTri T;
    XfCount cnt = {0, 0, 0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid && !s_xf_no_tex;
    T.use_rc = s_gpu.rc_seen && !no_rc;
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
    if (nv2a_gpu_enabled()) {
        NvGpuVertex g[3];
        for (k = 0; k < 3; k++) {
            memcpy(g[k].pos, v[k]->pos, sizeof g[k].pos);
            memcpy(g[k].d0, v[k]->d0, sizeof g[k].d0);
            memcpy(g[k].d1, v[k]->d1, sizeof g[k].d1);
            g[k].fog[0] = fog_factor(v[k]->fog[0]);
            g[k].fog[1] = g[k].fog[2] = g[k].fog[3] = 0.0f;
            memcpy(g[k].tc, v[k]->tex, sizeof g[k].tc);
        }
        nv2a_gpu_push(g);
        s_gpu.xf_drawn++;
        s_gpu.tris_drawn++;
        s_gpu_pending = 1;
        note_drawn();
        return;
    }
    if (backend_draws()) {
        Nv2aVertex *o = backend_slot();
        if (o) {
            backend_from_xf(va, &o[0]);
            backend_from_xf(vb, &o[1]);
            backend_from_xf(vc, &o[2]);
            s_bcount += 3;
        }
        return;
    }
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (s_gpu.depth_test || s_gpu.depth_mask)
        T.zb = zbuf_current(1);
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }

        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;

    T.dl[0][0] = -(c[1] - b[1]) * T.inv_area;  T.dl[0][1] = (c[0] - b[0]) * T.inv_area;
    T.dl[1][0] = -(a[1] - c[1]) * T.inv_area;  T.dl[1][1] = (a[0] - c[0]) * T.inv_area;
    T.dl[2][0] = -(b[1] - a[1]) * T.inv_area;  T.dl[2][1] = (b[0] - a[0]) * T.inv_area;

    xf_queue_push(&T);
    (void)cnt;
    s_gpu.tris_drawn++;
    note_drawn();
}

#define NV_CLIP_W 1e-3f

static float s_xf_o[3];

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static float xf_clip_dist(const Nv2aVshOutput *v, int pl)
{
    return pl == 0 ? v->pos[3] - NV_CLIP_W : v->pos[2] - s_xf_o[2] * v->pos[3];
}

static int xf_clip_poly(const Nv2aVshOutput *in, int n, int pl,
                        Nv2aVshOutput *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const Nv2aVshOutput *p = &in[i], *q = &in[(i + 1) % n];
        float dp = xf_clip_dist(p, pl), dq = xf_clip_dist(q, pl);
        if (dp >= 0.0f)
            out[m++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            Nv2aVshOutput *v = &out[m++];
            xf_lerp(p, q, dp / (dp - dq), v);

            if (pl == 0)
                v->pos[3] = NV_CLIP_W;
            else
                v->pos[2] = s_xf_o[2] * v->pos[3];
        }
    }
    return m;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    static int near_z = -1;
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput p0[8], p1[8];
    int n = 3, i, j, pl, planes, inside = 1;

    if (near_z < 0) {
        const char *e = getenv("RECOMP_CLIP_NEAR_Z");
        near_z = !(e && e[0] == '0');
    }
    planes = near_z ? 2 : 1;
    in[0] = a; in[1] = b; in[2] = c;
    for (i = 0; i < 3; i++) {
        p0[i] = *in[i];
        for (j = 0; j < 3; j++)
            p0[i].pos[j] = in[i]->pos[j] * in[i]->pos[3];
        for (pl = 0; pl < planes; pl++) {
            float d = xf_clip_dist(&p0[i], pl);
            if (d != d || d - d != 0.0f)
                return;
            if (d < 0.0f)
                inside = 0;
        }
    }
    if (inside) {
        raster_xf_triangle(a, b, c);
        return;
    }
    n = xf_clip_poly(p0, n, 0, p1);
    if (planes > 1 && n)
        n = xf_clip_poly(p1, n, 1, p0);
    else
        memcpy(p0, p1, sizeof p1);
    if (n < 3) {
        s_gpu.tris_behind++;
        return;
    }

    for (i = 0; i < n; i++)
        for (j = 0; j < 3; j++)
            p0[i].pos[j] /= p0[i].pos[3];
    for (i = 1; i + 1 < n; i++)
        raster_xf_triangle(&p0[0], &p0[i], &p0[i + 1]);
}

static Nv2aVshOutput *s_xf;

static int batch_uses_program(void)
{
    static int no_vsh = -1;

    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    return (s_gpu.xf_mode & 3) == 2 && !no_vsh;
}

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    if (!batch_uses_program())
        return ffp_vertex(index, out);
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {

        if (!s_gpu.attr[a].size && a < NV_VERTEX_ATTRS)
            memcpy(in[a], s_gpu.imm_attr[a], sizeof in[a]);
        else
            fetch_attr(&s_gpu.attr[a], index, in[a]);
    }
    if (!nv2a_vsh_run((const float (*)[4])in, out))
        return 0;

    if (out->pos[3] == 0.0f && !signbit(out->pos[3]))
        out->pos[3] = 1.0f;

    out->pos[0] *= s_gpu.aa_sx;
    out->pos[1] *= s_gpu.aa_sy;
    return 1;
}

static void xf_set_viewport(void)
{
    const float *of = nv2a_vsh_constant(59);

    s_xf_o[0] = of[0] * s_gpu.aa_sx;
    s_xf_o[1] = of[1] * s_gpu.aa_sy;
    s_xf_o[2] = of[2];
}

static uint32_t s_vj_list[NV_MAX_INDICES], s_vj_src[NV_MAX_INDICES];
static uint32_t s_vj_n;
static volatile int s_vj_fail;
static volatile long s_vj_next;

static void vsh_job(int k, int n)
{
    (void)k; (void)n;
    for (;;) {
#if defined(_WIN32)
        uint32_t b = 1u + 32u * (uint32_t)InterlockedExchangeAdd((volatile LONG *)&s_vj_next, 1);
#else
        uint32_t b = 1u + 32u * (uint32_t)__atomic_fetch_add(&s_vj_next, 1, __ATOMIC_RELAXED);
#endif
        uint32_t j;
        if (b >= s_vj_n)
            return;
        for (j = b; j < b + 32u && j < s_vj_n; j++) {
            uint32_t i = s_vj_list[j];
            if (!transform_vertex(s_gpu.idx[i], &s_xf[i]))
                s_vj_fail = 1;
        }
    }
}

static float    *s_gv;
static uint32_t *s_gi;
static uint32_t  s_gpos[NV_MAX_INDICES];

static int raster_batch_gpu_vsh(void)
{
    enum { GC_BITS = 12, GC_SIZE = 1 << GC_BITS };
    static uint32_t gc_tag[GC_SIZE], gc_slot[GC_SIZE], gc_stamp[GC_SIZE], stamp;
    static int near_z = -1;
    uint32_t i, n = s_gpu.idx_count, nv = 0, ni = 0, inputs, nin = 0, a;
    uint32_t list[NV2A_VSH_INPUTS];
    uint64_t key;
    NvGpuVshDraw d;
    NvGpuState st;
    uint32_t zpass = 0;
    int ok;

    if (!nv2a_gpu_enabled() || !batch_uses_program() || nv2a_vsh_cxt_write()
        || backend_draws() || n < 3)
        return 0;
    if ((key = nv2a_vsh_program_key()) == 0)
        return 0;
    inputs = nv2a_gpu_vsh_inputs(key, &ok);
    if (!ok)
        return 0;
    if (surface_bpp() != 4 && surface_bpp() != 2)
        return 0;
    if (!s_gv && !(s_gv = (float *)malloc((size_t)NV_MAX_INDICES * NV2A_VSH_INPUTS * 16)))
        return 0;
    if (!s_gi && !(s_gi = (uint32_t *)malloc((size_t)NV_MAX_INDICES * 3 * sizeof *s_gi)))
        return 0;
    if (near_z < 0) {
        const char *e = getenv("RECOMP_CLIP_NEAR_Z");
        near_z = !(e && e[0] == '0');
    }
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (inputs & (1u << a))
            list[nin++] = a;
    if (++stamp == 0) {
        memset(gc_stamp, 0, sizeof gc_stamp);
        stamp = 1;
    }

    for (i = 0; i < n; i++) {
        uint32_t index = s_gpu.idx[i];
        uint32_t h = (index * 2654435761u) >> (32 - GC_BITS), k;
        float *v;
        if (gc_stamp[h] == stamp && gc_tag[h] == index) {
            s_gpos[i] = gc_slot[h];
            continue;
        }
        v = s_gv + (size_t)nv * nin * 4;
        for (k = 0; k < nin; k++) {
            a = list[k];
            if (!s_gpu.attr[a].size && a < NV_VERTEX_ATTRS)
                memcpy(v + k * 4, s_gpu.imm_attr[a], 16);
            else
                fetch_attr(&s_gpu.attr[a], index, v + k * 4);
        }
        gc_stamp[h] = stamp; gc_tag[h] = index; gc_slot[h] = nv;
        s_gpos[i] = nv++;
    }
#define GTRI(p, q, r) do { s_gi[ni] = s_gpos[p]; s_gi[ni + 1] = s_gpos[q]; \
                           s_gi[ni + 2] = s_gpos[r]; ni += 3; } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) GTRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) GTRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) GTRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) { GTRI(i, i + 1, i + 2); GTRI(i, i + 2, i + 3); }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) { GTRI(i, i + 1, i + 3); GTRI(i, i + 3, i + 2); }
        break;
    default:
        return 0;
    }
#undef GTRI
    if (!ni)
        return 1;
    s_xf_no_tex = 0;
    gpu_fill_state(&st, 1);
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    if (s_gpu.texs[0].valid)
        note_texture_use();
    if (surface_hits_image(st.color_va, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 1;
    memset(&d, 0, sizeof d);
    d.key = key;
    d.inputs = inputs;
    d.verts = s_gv;
    d.nverts = nv;
    d.indices = s_gi;
    d.nindices = ni;
    d.aa_sx = s_gpu.aa_sx;
    d.aa_sy = s_gpu.aa_sy;
    d.near_z = nv2a_vsh_constant(59)[2];
    d.near_clip = (uint32_t)near_z;
    d.fog_enable = s_gpu.fog_enable;
    d.fog_mode = s_gpu.fog_mode;
    d.fog_px = s_gpu.fog_param[0];
    d.fog_py = s_gpu.fog_param[1];
    if (!nv2a_gpu_draw_vsh(&st, &d, &zpass))
        return 0;
    s_gpu.zpass_count += zpass;
    s_gpu.xf_drawn += ni / 3;
    s_gpu.tris_drawn += ni / 3;
    note_drawn();
    return 1;
}

static void raster_batch_program(void)
{
    uint32_t i, n = s_gpu.idx_count;

    if (raster_batch_gpu_vsh())
        return;
    if (!s_xf && !(s_xf = (Nv2aVshOutput *)calloc(NV_MAX_INDICES, sizeof *s_xf)))
        return;

    {
        enum { VC_BITS = 12, VC_SIZE = 1 << VC_BITS };
        static uint32_t vc_tag[VC_SIZE], vc_slot[VC_SIZE], vc_stamp[VC_SIZE];
        static uint32_t stamp;
        static int off = -1;
        int use = 0;

        if (off < 0)
            off = getenv("RECOMP_NO_VCACHE") != NULL;
        if (!off && !(batch_uses_program() && nv2a_vsh_cxt_write())) {
            use = 1;
            if (++stamp == 0) {
                memset(vc_stamp, 0, sizeof vc_stamp);
                stamp = 1;
            }
        }

        {
            static int mt = -1;
            if (mt < 0) {
                const char *e = getenv("RECOMP_VSH_THREADS");
                mt = !(e && e[0] == '0');
            }
            if (mt && use && batch_uses_program() && n >= 96 && raster_pool_size() > 1) {
                uint32_t nlist = 0;
                for (i = 0; i < n; i++) {
                    uint32_t index = s_gpu.idx[i];
                    uint32_t h = (index * 2654435761u) >> (32 - VC_BITS);
                    if (vc_stamp[h] == stamp && vc_tag[h] == index) {
                        s_vj_src[i] = vc_slot[h];
                        continue;
                    }
                    vc_stamp[h] = stamp; vc_tag[h] = index; vc_slot[h] = i;
                    s_vj_src[i] = i;
                    s_vj_list[nlist++] = i;
                }
                if (!transform_vertex(s_gpu.idx[s_vj_list[0]], &s_xf[s_vj_list[0]]))
                    return;
                s_vj_n = nlist;
                s_vj_fail = 0;
                s_vj_next = 0;
                pool_run_job(vsh_job);
                if (s_vj_fail)
                    return;
                for (i = 0; i < n; i++)
                    if (s_vj_src[i] != i)
                        s_xf[i] = s_xf[s_vj_src[i]];
                goto transformed;
            }
        }
        for (i = 0; i < n; i++) {
            uint32_t index = s_gpu.idx[i];
            if (use) {
                uint32_t h = (index * 2654435761u) >> (32 - VC_BITS);
                if (vc_stamp[h] == stamp && vc_tag[h] == index) {
                    s_xf[i] = s_xf[vc_slot[h]];
                    continue;
                }
                if (!transform_vertex(index, &s_xf[i]))
                    return;
                vc_stamp[h] = stamp; vc_tag[h] = index; vc_slot[h] = i;
            } else if (!transform_vertex(index, &s_xf[i])) {
                return;
            }
        }
    }
transformed:
    xf_set_viewport();
    {
        const VertexAttr *tc = texcoord_attr();
        s_xf_no_tex = !batch_uses_program() && !(tc->offset && tc->stride);
    }
    if (batch_uses_program()) {
        s_gpu.batches_program++;
        s_gpu.verts_program += n;
    } else {
        static int ftrace = -1;

        s_gpu.batches_ffp++;

        if (ftrace < 0)
            ftrace = getenv("RECOMP_FFP_TRACE") != NULL;
        if (ftrace && s_gpu.batches_ffp <= 4) {
            const float *m = s_gpu.composite;
            float in[4];

            fprintf(stderr, "  [FFP] batch %u prim %u n %u mode 0x%X"
                            " clip %ux%u+%u+%u aa %gx%g\n"
                            "  [FFP]   M = %g %g %g %g | %g %g %g %g |"
                            " %g %g %g %g | %g %g %g %g\n",
                    s_gpu.batches_ffp, s_gpu.prim, n, s_gpu.xf_mode,
                    s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                    s_gpu.aa_sx, s_gpu.aa_sy,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (i = 0; i < n && i < 4; i++) {
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], in);
                fprintf(stderr, "  [FFP]   v%u (%g %g %g %g) -> (%g %g %g, w %g)\n",
                        s_gpu.idx[i], in[0], in[1], in[2], in[3],
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
            }
        }
    }
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {

        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }

        if (left > 0 && n >= 3 && s_gpu.batches_program % 20000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.idx[0], s_gpu.idx[1], s_gpu.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if ((i & 1) && strip_alt_winding())
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }

    xf_queue_flush();
    if (s_gpu_pending) {
        NvGpuState st;
        s_gpu_pending = 0;
        gpu_fill_state(&st, 1);
        if (surface_hits_image(st.color_va, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
            nv2a_gpu_draw_discard();
        else
            s_gpu.zpass_count += nv2a_gpu_draw(&st);
    }
    if (backend_draws())
        backend_flush(!s_xf_no_tex);
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;
    int textured = 0;

    if (s_gpu.idx_count < 3)
        return;
    if (batch_uses_program() || batch_is_ffp()) {
        raster_batch_program();
        return;
    }
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            textured = 1;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++) {
            if ((i & 1) && strip_alt_winding())
                raster_indexed(s_gpu.idx[i+1], s_gpu.idx[i], s_gpu.idx[i+2],
                               vertex_color(s_gpu.idx[i]));
            else
                raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                               vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:

        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:

        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;
    }
    if (backend_draws())
        backend_flush(textured);

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {

        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
        raster_batch();
        if (s_ftrace == 2) {
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X | z test %u func %X mask %u | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.idx_count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if ((s_gpu.xf_mode & 3) == 2 && s_xf)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    static int verbose_draw = -1;
    if (verbose_draw < 0)
        verbose_draw = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose_draw) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);

            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }

            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {

                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {

                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;
        case 4:  bytes = s_gpu.attr[a].size;       break;
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);

    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = i;

    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.imm_count++;
}

static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {

        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}

static void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:

        t->offset = dma_resolve(param);
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->border_color = (param >> 3) & 1;
        t->levels = (param >> 16) & 0xF;

        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:

        t->palette = dma_resolve(param & ~0x3Fu);
        break;
    case NV097_SET_TEXTURE_ADDRESS:

        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:
        t->filter = param;
        break;
    case 0x1B24:
        t->border = param;
        break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

static XboxFlipHold s_flip_hold;
static uint32_t s_vbl_count;
static uint64_t s_vbl_moved_ns;

static uint32_t s_walk_vbl;

void nv2a_pb_exec_walk_begin(void)
{
    s_walk_vbl = xbox_VblankCount();
}

int nv2a_pb_exec_flip_pacing(void)
{
    static volatile int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_FLIP_PACING");
        int m = (!e || !*e || !strcmp(e, "0")) ? 0
                : !strcmp(e, "edge") ? 2 : 1;
        s_flip_hold.edge = m == 2;
        mode = m;
        if (e)
            fprintf(stderr, "  [GPU] flip pacing %s (RECOMP_FLIP_PACING=%s)\n",
                    m == 0 ? "off" : m == 2 ? "edge" : "on", e);
    }
    return mode;
}

static uint32_t flip_clock(uint64_t now_ns, int *ok)
{
    uint32_t c = xbox_VblankCount();
    if (c != s_vbl_count) {
        s_vbl_count = c;
        s_vbl_moved_ns = now_ns;
    }
    *ok = s_vbl_moved_ns && now_ns - s_vbl_moved_ns < XBOX_FLIP_CLOCK_STALE_NS;
    return c;
}

int nv2a_pb_exec_flip_poll(void)
{
    uint64_t now;
    uint32_t c;
    int ok;

    if (!s_flip_hold.held)
        return 0;
    now = xbox_HostNowNs();
    c = flip_clock(now, &ok);
    return xbox_FlipHoldPoll(&s_flip_hold, c, now, ok);
}

static void flip_hold_report(void)
{
    XboxFlipHold *h = &s_flip_hold;
    if (nv2a_pb_exec_flip_pacing() == 0)
        return;
    fprintf(stderr, "[GPU] flip hold: holds %u held_ms %.1f hold_timeouts %u"
            " clock_stops %u iv0 %u iv1 %u iv2 %u iv3 %u nonop %u"
            " counters_owned %u\n", h->holds, h->held_ns / 1e6, h->timeouts,
            h->clock_stops, h->iv[0], h->iv[1], h->iv[2], h->iv[3], h->nonop,
            xbox_Nv2aFrameCountersOwned());
    h->holds = h->timeouts = h->clock_stops = h->nonop = 0;
    h->iv[0] = h->iv[1] = h->iv[2] = h->iv[3] = 0;
    h->held_ns = 0;
}

static int s_exec_inited;

int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_PB_CONST_FAST");
        on = !(e && e[0] == '0');
    }
    if (!on || !s_exec_inited)
        return 0;
    s_reg[(method & 0x1FFCu) / 4] = param;
    nv2a_vsh_constant_word(param);
    return 1;
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{

    if (subch == 0 && method == 0x1D70 && s_sem_va) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        *(volatile uint32_t *)(mem + s_sem_va) = param;
        return;
    }
    if (!s_exec_inited) {
        s_exec_inited = 1;
        s_gpu.aa_sx = s_gpu.aa_sy = 1.0f;

        s_reg[0x0364 / 4] = 0x207u;
        s_reg[0x0360 / 4] = s_reg[0x036C / 4] = 0xFFu;
        s_reg[0x0370 / 4] = s_reg[0x0374 / 4] = s_reg[0x0378 / 4] = 0x1E00u;
        s_gpu.color_mask = 0x01010101u;
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    if (subch == 0)
        s_reg[(method & 0x1FFCu) / 4] = param;

    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {

        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && !(method >= 0x1800 && method < 0x1A00)
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    if (subch == 0 && method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return;
    }

    if (method == 0x0310 && subch == 5 && nv2a_pb_dma_walk()) {
        volatile uint32_t *r = xbox_Nv2aRegPtr(0x400B10);

        if (!r)
            return;
        if (!s_sem_va) {
            uint32_t va = xbox_Nv2aFenceWordVa();

            if (va) {
                uint32_t sem = *(volatile uint32_t *)
                               ((uint8_t *)xbox_GetMemoryOffset() + va);
                param = (param & ~0x7Cu) | ((sem & 0x1Fu) << 2);
            }
        }
        *r = param;
        return;
    }
    if (subch != 0) {
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_raw_h = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_raw_v = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        surface_apply_clip();
        break;
    case 0x1D98:
        s_gpu.clear_rect_h = param;
        s_gpu.clear_rect_set |= 1;
        break;
    case 0x1D9C:
        s_gpu.clear_rect_v = param;
        s_gpu.clear_rect_set |= 2;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:
        s_gpu.color_mask = param;
        break;

    case 0x0288: s_gpu.rc.final0 = param; break;
    case 0x028C: s_gpu.rc.final1 = param; break;
    case 0x1E20: s_gpu.rc.final_c0 = param; break;
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x17F8: s_gpu.clip_plane_mode = param; break;

    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x17C8:
        s_gpu.zpass_count = 0;
        break;
    case 0x17CC:
        s_gpu.zpass_enable = param;
        break;
    case 0x17D0: {

        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t at = dma_resolve(param & 0x00FFFFFFu);
        uint64_t stamp = (uint64_t)s_gpu.reports++ * 1000u;
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &s_gpu.zpass_count, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, 10);
        clear_surface(param);
        if ((param & 0x01) && !nv2a_gpu_enabled())
            zbuf_clear();
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {

            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:

        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {

        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        break;
    }

    case NV097_ARRAY_ELEMENT16:

        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;

    case 0x1808:
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES)
            s_gpu.idx[s_gpu.idx_count++] = param;
        break;
    case NV097_NO_OPERATION:

        {
            extern void (*volatile nv2a_sw_method_hook)(uint32_t);
            void (*sw_hook)(uint32_t) = nv2a_sw_method_hook;
            if (param && sw_hook)
                sw_hook(param);
        }
        if (!nv2a_pb_exec_flip_pacing()) {
            if (!imm_vertex_method(method, param))
                note_unhandled(method, param);
            break;
        }
        if (param)
            xbox_FlipHoldSwap(&s_flip_hold, param);
        return;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return;

    case NV097_FLIP_STALL: {

        uint64_t stall_ns = xbox_HostNowNs();
        int clock_ok;
        uint32_t stall_vbl;
        (void)flip_clock(stall_ns, &clock_ok);
        stall_vbl = s_walk_vbl;
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);

        s_gpu.flip_read = s_gpu.flip_write;

        xbox_Nv2aFrameCounterFlip();
        if (s_backend_present_sync)
            s_backend_present_sync();

        uint32_t gpu_shown = 0, gpu_shown_pitch = 0;
        if (nv2a_gpu_enabled()) {
            static int dumping = -1;
            if (dumping < 0)
                dumping = getenv("RECOMP_FB_DUMP") || getenv("RECOMP_FB_DUMP_FLIPS")
                       || getenv("RECOMP_GPU_SYNC_FLIP");
            if (dumping) {
                nv2a_gpu_sync_all();
            } else {
                uint32_t cur = s_gpu.drawn_offset ? s_gpu.drawn_offset : s_gpu.color_offset;
                if (cur)
                    gpu_shown = nv2a_gpu_flip(dma_resolve(cur), &gpu_shown_pitch);
            }
            nv2a_gpu_frame();
        }

        {
            extern void xbox_FramebufferWindowFrameStats(uint32_t);
            static uint32_t draws_at_flip;
            xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
            draws_at_flip = s_gpu.draws;
        }
        if (s_gpu.pitch) {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                               : s_gpu.color_offset;
            uint32_t pitch = s_gpu.drawn_offset ? s_gpu.drawn_pitch
                                                : s_gpu.pitch;
            if (gpu_shown) {
                xbox_FramebufferWindowSet(gpu_shown, gpu_shown_pitch);
                xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
                xbox_FramebufferWindowPresent(gpu_shown, gpu_shown_pitch);
            } else if (done) {
                xbox_FramebufferWindowSet(dma_resolve(done), pitch);
                xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
                xbox_FramebufferWindowPresent(dma_resolve(done), pitch);
            }
        }
        {

            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
                if (on)
                    dump_surface_bmp();
            }
        }
        s_gpu.drawn_stale = 1;
        if (s_backend && s_backend->flip)
            s_backend->flip();

        if (verbose) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }

        if (nv2a_pb_exec_flip_pacing()) {
            extern int nv2a_pb_scan_in_call(void);
            xbox_FlipHoldStall(&s_flip_hold, stall_vbl, stall_ns,
                               clock_ok && !nv2a_pb_scan_in_call());
        }
        return;
    }

    case 0x1E94:
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:
        nv2a_vsh_set_cxt_write(param);
        break;
    case 0x1E9C:
        nv2a_vsh_set_load_slot(param);
        break;
    case 0x1EA0:
        nv2a_vsh_set_start_slot(param);
        break;
    case 0x1EA4:
        nv2a_vsh_set_constant_load(param);
        break;

    case 0x0680: case 0x0684: case 0x0688: case 0x068C:
    case 0x0690: case 0x0694: case 0x0698: case 0x069C:
    case 0x06A0: case 0x06A4: case 0x06A8: case 0x06AC:
    case 0x06B0: case 0x06B4: case 0x06B8: case 0x06BC:
        memcpy(&s_gpu.composite[(method - 0x0680) / 4], &param, 4);
        s_gpu.composite_set = 1;
        break;

    case 0x030C: s_gpu.depth_test = param; break;
    case 0x0354: s_gpu.depth_func = param; break;
    case 0x035C: s_gpu.depth_mask = param; break;
    case 0x0214: s_gpu.zeta_offset = param; break;
    case 0x1D8C: s_gpu.zstencil_clear = param; break;

    default:
        if (method >= 0x0260 && method < 0x0280) {
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }

        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }

        if (method >= 0x09D0 && method < 0x09E0) {
            nv2a_vsh_constant_component(57, (method - 0x09D0) / 4, param);
            break;
        }
        if (method >= 0x0A50 && method < 0x0A60) {
            nv2a_vsh_constant_component(56, (method - 0x0A50) / 4, param);
            break;
        }
        if (method >= 0x0B00 && method < 0x0B80) {
            nv2a_vsh_program_word(param);
            break;
        }
        if (method >= 0x0B80 && method < 0x0C00) {
            nv2a_vsh_constant_word(param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {

            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            if (a->size == 7)
                a->size = 3;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;
    const uint32_t H = 0x43F00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;

        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {

        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {

        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        nv2a_gpu_report();
#if defined(_WIN32)

        {
            extern volatile long long g_nv2a_raster_ticks, g_nv2a_raster_flushes;
            static long long last_ticks, last_flushes;
            static unsigned long long last_cpu;
            LARGE_INTEGER f;
            FILETIME c, e, k, u;
            unsigned long long cpu = 0;
            QueryPerformanceFrequency(&f);
            if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
                cpu = (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)
                    + (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime);
            fprintf(stderr, "[GPUTIME] wall %lu ms, raster %lld ms in %lld flushes, this thread cpu %llu ms\n",
                    now - last_ms,
                    (g_nv2a_raster_ticks - last_ticks) * 1000 / f.QuadPart,
                    g_nv2a_raster_flushes - last_flushes,
                    (cpu - last_cpu) / 10000ULL);
            last_ticks = g_nv2a_raster_ticks;
            last_flushes = g_nv2a_raster_flushes;
            last_cpu = cpu;
        }
#endif
        last_flips = s_gpu.flips;
        last_ms = now;
    }
    flip_hold_report();
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);

    if (!s_flip_dumping)
        dump_surface_bmp();

    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    if (s_gpu.batches_ffp)
        fprintf(stderr, "[GPU] fixed-function transform: %u batches\n",
                s_gpu.batches_ffp);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {

            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }
    for (i = 0; i < s_unhandled_count; i++)
        s_unhandled_slot[(s_unhandled[i].method & 0x1FFC) / 4] = (uint16_t)(i + 1);
    fflush(stderr);
}

