#ifndef NV2A_GPU_H
#define NV2A_GPU_H

#include <stdint.h>
#include "nv2a_combiner.h"

typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tc[4][4];
} NvGpuVertex;

typedef struct {
    const void *src;
    uint32_t valid;
    uint32_t offset, bytes;
    uint32_t palette;
    uint32_t width, height, pitch, color;
    uint32_t addr_u, addr_v, filter, border, border_color, cube;
    uint32_t face_stride;
    uint32_t normalized;
} NvGpuTex;

typedef struct {

    uint32_t color_va, pitch, bpp;
    uint32_t swizzled, swz_w, swz_h;
    uint32_t clip_x, clip_y, clip_w, clip_h;

    uint32_t zeta_key, z16;
    uint32_t depth_test, depth_func, depth_write;
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;

    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t color_mask;
    uint32_t alpha_test, alpha_func, alpha_ref;

    uint32_t use_rc, textured, bilinear;
    Nv2aCombiner rc;
    uint32_t fog_color;
    uint32_t clip_plane_mode;
    uint32_t count_zpass;
    NvGpuTex tex[4];
} NvGpuState;

int  nv2a_gpu_enabled(void);

void nv2a_gpu_push(const NvGpuVertex v[3]);

uint32_t nv2a_gpu_draw(const NvGpuState *s);

typedef struct {
    uint64_t key;
    uint32_t inputs;
    const float *verts;
    uint32_t nverts;
    const uint32_t *indices;
    uint32_t nindices;
    float aa_sx, aa_sy, near_z;
    uint32_t near_clip;
    uint32_t fog_enable, fog_mode;
    float fog_px, fog_py;
} NvGpuVshDraw;

uint32_t nv2a_gpu_vsh_inputs(uint64_t key, int *ok);

int nv2a_gpu_draw_vsh(const NvGpuState *s, const NvGpuVshDraw *d, uint32_t *zpass);

void nv2a_gpu_draw_discard(void);

void nv2a_gpu_clear(const NvGpuState *s, uint32_t flags, uint32_t argb,
                    float z01, uint32_t stencil,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);

void nv2a_gpu_cpu_write(uint32_t base, uint32_t bytes);

void nv2a_gpu_sync_all(void);

uint32_t nv2a_gpu_flip(uint32_t va, uint32_t *pitch_out);

void nv2a_gpu_frame(void);

void nv2a_gpu_report(void);

int nv2a_exec_tex_decode(const void *tex, uint32_t face_offset, uint32_t *argb_out);

#endif

