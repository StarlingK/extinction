#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_gpu.h"
#include "nv2a_vsh_interp.h"
#include "../d3d/d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static const char s_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  float4 vp;\n"
"  uint4 cicw[2]; uint4 aicw[2]; uint4 cocw[2]; uint4 aocw[2];\n"
"  float4 f0[8]; float4 f1[8];\n"
"  uint4 fin;\n"
"  float4 fc0; float4 fc1; float4 fogc;\n"
"  uint4 misc;\n"
"  float4 misc2;\n"
"  float4 texinfo[4];\n"
"  uint4 misc3;\n"
"};\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
"Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"TextureCube tc0c : register(t4); TextureCube tc1c : register(t5);\n"
"TextureCube tc2c : register(t6); TextureCube tc3c : register(t7);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
"struct VSIn { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"struct VSOut { float4 pos : SV_Position; noperspective float4 d0 : COLOR0;\n"
"  noperspective float4 d1 : COLOR1; noperspective float4 fog : TEXCOORD4;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n"
"  float4 t3 : TEXCOORD3; };\n"
"VSOut vsmain(VSIn i) {\n"
"  VSOut o; float w = i.pos.w;\n"
"  o.pos = float4((i.pos.x * vp.x - 1.0) * w, (1.0 - i.pos.y * vp.y) * w,\n"
"                 i.pos.z * vp.z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1.0 - saturate(x);\n"
"  if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return -2.0 * p + 1.0;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float map_out(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;\n"
"  if (m == 0x10) return x * 2.0;\n"
"  if (m == 0x18) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20) return x * 4.0;\n"
"  if (m == 0x30) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 rd_rgb(float4 r, uint b) {\n"
"  float3 v = (b & 16) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float rd_a(float4 r, uint b) { return map_in((b & 16) ? r.a : r.b, b); }\n"
"bool atest(float a) {\n"
"  int v = (int)(a * 255.0 + 0.5); int r = (int)misc2.x; uint f = misc.w;\n"
"  if (f == 0x200) return false;\n"
"  if (f == 0x201) return v < r;\n"
"  if (f == 0x202) return v == r;\n"
"  if (f == 0x203) return v <= r;\n"
"  if (f == 0x204) return v > r;\n"
"  if (f == 0x205) return v != r;\n"
"  if (f == 0x206) return v >= r;\n"
"  return true;\n"
"}\n"
"float2 uvof(float2 uv, uint st) {\n"
"  return texinfo[st].z != 0.0 ? uv : uv / texinfo[st].xy;\n"
"}\n"
"float4 s2d(uint st, float2 uv) {\n"
"  uv = uvof(uv, st);\n"
"  if (st == 0) return tx0.SampleLevel(s0, uv, 0);\n"
"  if (st == 1) return tx1.SampleLevel(s1, uv, 0);\n"
"  if (st == 2) return tx2.SampleLevel(s2, uv, 0);\n"
"  return tx3.SampleLevel(s3, uv, 0);\n"
"}\n"
"float4 scube(uint st, float3 d) {\n"
"  if (st == 0) return tc0c.SampleLevel(s0, d, 0);\n"
"  if (st == 1) return tc1c.SampleLevel(s1, d, 0);\n"
"  if (st == 2) return tc2c.SampleLevel(s2, d, 0);\n"
"  return tc3c.SampleLevel(s3, d, 0);\n"
"}\n"
"float4 fetch(uint st, float4 c) {\n"
"  uint mode = (fin.w >> (st * 5)) & 31;\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) {\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    return s2d(st, c.xy / q);\n"
"  }\n"
"  if (mode == 3) {\n"
"    if (texinfo[st].w == 0.0) return s2d(st, c.xy);\n"
"    return scube(st, c.xyz);\n"
"  }\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      uint ge = (misc3.x >> (st * 4 + j)) & 1;\n"
"      if (ge != 0 ? c[j] >= 0.0 : c[j] < 0.0) discard;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"float4 psmain(VSOut i) : SV_Target {\n"
"  if (misc.x == 0) {\n"
"    float4 c = i.d0;\n"
"    if (misc.y != 0) c = s2d(0, i.t0.xy) * c;\n"
"    c = saturate(c);\n"
"    if (misc.z != 0 && !atest(c.a)) discard;\n"
"    return c;\n"
"  }\n"
"  float4 R[16];\n"
"  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fogc.rgb, i.fog.x);\n"
"  R[4] = i.d0; R[5] = i.d1;\n"
"  R[8] = fetch(0, i.t0); R[9] = fetch(1, i.t1);\n"
"  R[10] = fetch(2, i.t2); R[11] = fetch(3, i.t3);\n"
"  R[12].a = (fin.w & 31) != 0 ? R[8].a : 1.0;\n"
"  uint n = min(fin.z & 0xFF, 8u), flags = fin.z >> 8;\n"
"  for (uint s = 0; s < n; s++) {\n"
"    uint icw = cicw[s >> 2][s & 3], ocw = cocw[s >> 2][s & 3];\n"
"    uint aicw_ = aicw[s >> 2][s & 3], aocw_ = aocw[s >> 2][s & 3];\n"
"    uint fl = ocw >> 12, afl = aocw_ >> 12;\n"
"    R[1] = f0[(flags & 0x010) ? s : 0];\n"
"    R[2] = f1[(flags & 0x100) ? s : 0];\n"
"    bool mux = (flags & 1) ? R[12].a >= 0.5 : (((int)(R[12].a * 255.0)) & 1) != 0;\n"
"    float3 A = rd_rgb(R[(icw >> 24) & 15], icw >> 24);\n"
"    float3 B = rd_rgb(R[(icw >> 16) & 15], (icw >> 16) & 0xFF);\n"
"    float3 C = rd_rgb(R[(icw >> 8) & 15], (icw >> 8) & 0xFF);\n"
"    float3 D = rd_rgb(R[icw & 15], icw & 0xFF);\n"
"    float aA = rd_a(R[(aicw_ >> 24) & 15], aicw_ >> 24);\n"
"    float aB = rd_a(R[(aicw_ >> 16) & 15], (aicw_ >> 16) & 0xFF);\n"
"    float aC = rd_a(R[(aicw_ >> 8) & 15], (aicw_ >> 8) & 0xFF);\n"
"    float aD = rd_a(R[aicw_ & 15], aicw_ & 0xFF);\n"
"    float dab = dot(A, B), dcd = dot(C, D);\n"
"    float3 pab = (fl & 2) ? float3(dab, dab, dab) : A * B;\n"
"    float3 pcd = (fl & 1) ? float3(dcd, dcd, dcd) : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    uint om = fl & 0x38, aom = afl & 0x38;\n"
"    float3 ab = clamp(float3(map_out(pab.x, om), map_out(pab.y, om), map_out(pab.z, om)), -1.0, 1.0);\n"
"    float3 cd = clamp(float3(map_out(pcd.x, om), map_out(pcd.y, om), map_out(pcd.z, om)), -1.0, 1.0);\n"
"    float3 ms = clamp(float3(map_out(sum.x, om), map_out(sum.y, om), map_out(sum.z, om)), -1.0, 1.0);\n"
"    float apab = aA * aB, apcd = aC * aD;\n"
"    float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(map_out(apab, aom), -1.0, 1.0);\n"
"    float acd = clamp(map_out(apcd, aom), -1.0, 1.0);\n"
"    float ams = clamp(map_out(asum, aom), -1.0, 1.0);\n"
"    uint d;\n"
"    d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.z; }\n"
"    d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.z; }\n"
"    d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"    d = (aocw_ >> 4) & 15; if (d) R[d].a = aab;\n"
"    d = aocw_ & 15;        if (d) R[d].a = acd;\n"
"    d = (aocw_ >> 8) & 15; if (d) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 o;\n"
"  if (fin.x != 0 || fin.y != 0) {\n"
"    uint f0w = fin.x, f1w = fin.y, ff = f1w & 0xFF;\n"
"    R[1] = fc0; R[2] = fc1;\n"
"    float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"    float3 E = rd_rgb(R[(f1w >> 24) & 15], f1w >> 24);\n"
"    float3 F = rd_rgb(R[(f1w >> 16) & 15], (f1w >> 16) & 0xFF);\n"
"    R[15] = float4(E * F, 0.0);\n"
"    float3 A = rd_rgb(R[(f0w >> 24) & 15], f0w >> 24);\n"
"    float3 B = rd_rgb(R[(f0w >> 16) & 15], (f0w >> 16) & 0xFF);\n"
"    float3 C = rd_rgb(R[(f0w >> 8) & 15], (f0w >> 8) & 0xFF);\n"
"    float3 D = rd_rgb(R[f0w & 15], f0w & 0xFF);\n"
"    o.rgb = D + C * (1.0 - A) + B * A;\n"
"    o.a = rd_a(R[(f1w >> 8) & 15], (f1w >> 8) & 0xFF);\n"
"  } else {\n"
"    o = R[12];\n"
"  }\n"
"  o = saturate(o);\n"
"  if (misc.z != 0 && !atest(o.a)) discard;\n"
"  return o;\n"
"}\n";

typedef struct {
    float    vp[4];
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
    float    f0[8][4], f1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4], fogc[4];
    uint32_t misc[4];
    float    misc2[4];
    float    texinfo[4][4];
    uint32_t misc3[4];
} Cb;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_il;
static ID3D11Buffer *s_cb, *s_vb;
static ID3D11RasterizerState *s_rs;
static ID3D11ShaderResourceView *s_white2d, *s_whitecube;
static ID3D11Query *s_occ;
static uint32_t s_vb_cap = 1u << 16;
static uint32_t s_vb_pos;

static NvGpuVertex *s_pend;
static uint32_t s_pend_n, s_pend_cap;

static uint32_t s_frame = 1, s_serial;

static struct {
    uint64_t draws, tris, uploads, readbacks, tex_decodes, tex_hashes, clears;
} s_stat;

static void fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "[GPU-D3D11] %s failed (0x%08lX); staying on the CPU rasteriser\n",
            what, (unsigned long)hr);
}

typedef HRESULT (WINAPI *PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                      ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob **, ID3DBlob **);

static PFN_Compile s_compile;

static ID3DBlob *compile(PFN_Compile fn, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(s_hlsl, sizeof s_hlsl - 1, "nv2a_gpu", NULL, NULL, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3D11ShaderResourceView *make_white(int cube)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[6];
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    static const uint32_t white = 0xFFFFFFFFu;
    int i;

    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = cube ? 6 : 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (i = 0; i < 6; i++) {
        sd[i].pSysMem = &white;
        sd[i].SysMemPitch = 4;
        sd[i].SysMemSlicePitch = 4;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &t)))
        return NULL;
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v);
    ID3D11Texture2D_Release(t);
    return v;
}

static int gpu_init(void)
{
    static const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_Compile fn = dc ? (PFN_Compile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb, *psb;
    HRESULT hr;

    if (!fn) {
        fail("loading d3dcompiler_47.dll", E_FAIL);
        return 0;
    }
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1,
                           D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fail("D3D11CreateDevice", hr);
        return 0;
    }
    ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1);
    {
        const char *e = getenv("RECOMP_GPU_VSH");
        if (!(e && e[0] == '0'))
            s_compile = fn;
    }
    vsb = compile(fn, "vsmain", "vs_5_0");
    psb = compile(fn, "psmain", "ps_5_0");
    if (!vsb || !psb)
        return 0;
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = ID3D11Device_CreateInputLayout(s_dev, el, 8, ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &s_il);
        if (FAILED(hr)) { fail("CreateInputLayout", hr); return 0; }
    }
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    if (FAILED(hr)) { fail("CreateVertexShader", hr); return 0; }
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    if (FAILED(hr)) { fail("CreatePixelShader", hr); return 0; }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(Cb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb))) {
            fail("constant buffer", hr); return 0;
        }
        bd.ByteWidth = s_vb_cap * sizeof(NvGpuVertex);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) {
            fail("vertex buffer", hr); return 0;
        }
    }
    {

        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        if (FAILED(hr = ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs))) {
            fail("rasterizer state", hr); return 0;
        }
    }
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
        ID3D11Device_CreateQuery(s_dev, &qd, &s_occ);
    }
    s_white2d = make_white(0);
    s_whitecube = make_white(1);
    if (!s_white2d || !s_whitecube) { fail("white textures", E_FAIL); return 0; }
    fprintf(stderr, "[GPU-D3D11] pixel work on the host GPU (RECOMP_GPU=1)\n");
    return 1;
}

int nv2a_gpu_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU");
        on = (e && e[0] == '1') ? gpu_init() : 0;
    }
    return on;
}

#define NV_GPU_MAX_RT 24

typedef struct {
    uint32_t base, pitch, bpp, swz, w, h;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    int gpu_dirty;
    int mem_newer;
    uint32_t last_use, last_draw;
} Rt;

static Rt s_rt[NV_GPU_MAX_RT];

static uint32_t rt_bytes(const Rt *r)
{
    return r->swz ? r->w * r->h * r->bpp : r->pitch * r->h;
}

static void swz_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    xbox_swizzle_masks(w, h, mx, my);
}

static void rt_upload(Rt *r)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset() + r->base;
    s_stat.uploads++;
    if (!r->swz) {
        D3D11_BOX box = { 0, 0, 0, r->w, r->h, 1 };
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, &box,
                                              mem, r->pitch, 0);
    } else {
        uint32_t mx, my, x, y;
        uint8_t *tmp = (uint8_t *)malloc((size_t)r->w * r->h * r->bpp);
        if (!tmp)
            return;
        swz_masks(r->w, r->h, &mx, &my);
        for (y = 0; y < r->h; y++)
            for (x = 0; x < r->w; x++) {
                size_t si = swizzle_deposit(x, mx) | swizzle_deposit(y, my);
                memcpy(tmp + ((size_t)y * r->w + x) * r->bpp, mem + si * r->bpp, r->bpp);
            }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                              tmp, r->w * r->bpp, 0);
        free(tmp);
    }
    r->mem_newer = 0;
}

static void rt_readback(Rt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset() + r->base;
    uint32_t y, x;

    if (!r->gpu_dirty)
        return;
    r->gpu_dirty = 0;
    if (!r->staging) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging)))
            return;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    s_stat.readbacks++;
    if (!r->swz) {
        for (y = 0; y < r->h; y++)
            memcpy(mem + (size_t)y * r->pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                   (size_t)r->w * r->bpp);
    } else {
        uint32_t mx, my;
        swz_masks(r->w, r->h, &mx, &my);
        for (y = 0; y < r->h; y++)
            for (x = 0; x < r->w; x++) {
                size_t di = swizzle_deposit(x, mx) | swizzle_deposit(y, my);
                memcpy(mem + di * r->bpp,
                       (const uint8_t *)m.pData + (size_t)y * m.RowPitch + (size_t)x * r->bpp,
                       r->bpp);
            }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
}

static void rt_free(Rt *r)
{
    rt_readback(r);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    memset(r, 0, sizeof *r);
}

static Rt *rt_get(const NvGpuState *s)
{
    uint32_t w, h, i, need_h = s->clip_y + s->clip_h;
    Rt *r = NULL, *lru = &s_rt[0];
    D3D11_TEXTURE2D_DESC td;

    if (s->swizzled) {
        w = s->swz_w; h = s->swz_h;
    } else {
        if (!s->pitch || !s->bpp)
            return NULL;
        w = s->pitch / s->bpp;
        h = need_h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    for (i = 0; i < NV_GPU_MAX_RT; i++) {
        Rt *c = &s_rt[i];
        if (c->tex && c->base == s->color_va && c->pitch == s->pitch && c->bpp == s->bpp
            && c->swz == s->swizzled && c->w == w && c->h >= h) {
            r = c;
            break;
        }
        if (!c->tex || c->last_use < lru->last_use)
            lru = c;
    }
    if (!r) {

        for (i = 0; i < NV_GPU_MAX_RT; i++)
            if (s_rt[i].tex && s_rt[i].base == s->color_va)
                rt_free(&s_rt[i]);
        if (lru->tex)
            rt_free(lru);
        r = lru;
        memset(&td, 0, sizeof td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = s->bpp == 2 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex,
                                                       NULL, &r->rtv))) {
            ID3D11Texture2D_Release(r->tex);
            r->tex = NULL;
            return NULL;
        }
        r->base = s->color_va; r->pitch = s->pitch; r->bpp = s->bpp;
        r->swz = s->swizzled; r->w = w; r->h = h;
        r->mem_newer = 1;
    }
    if (r->mem_newer)
        rt_upload(r);
    r->last_use = ++s_serial;
    return r;
}

void nv2a_gpu_cpu_write(uint32_t base, uint32_t bytes)
{
    int i;
    for (i = 0; i < NV_GPU_MAX_RT; i++) {
        Rt *r = &s_rt[i];
        if (r->tex && base < r->base + rt_bytes(r) && r->base < base + bytes) {
            rt_readback(r);
            r->mem_newer = 1;
        }
    }
}

void nv2a_gpu_sync_all(void)
{
    int i;
    for (i = 0; i < NV_GPU_MAX_RT; i++)
        if (s_rt[i].tex)
            rt_readback(&s_rt[i]);
}

static struct {
    ID3D11Texture2D *stg[2];
    uint32_t w, h, bpp;
    int next;
    int pending;
    int pend_slot;
    uint32_t pend_base, pend_pitch, pend_w, pend_h, pend_bpp;
} s_flip;

static int s_flip_lazy(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GPU_LAZY_RT");
        v = (e && e[0] == '1');
    }
    return v;
}

uint32_t nv2a_gpu_flip(uint32_t va, uint32_t *pitch_out)
{
    uint32_t shown = 0;
    Rt *r = NULL;
    int i;

    for (i = 0; i < NV_GPU_MAX_RT; i++)
        if (s_rt[i].tex && !s_rt[i].swz && s_rt[i].base == va) {
            r = &s_rt[i];
            break;
        }

    if (s_flip.pending) {
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11Texture2D *t = s_flip.stg[s_flip.pend_slot];
        s_flip.pending = 0;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)t, 0,
                                              D3D11_MAP_READ, 0, &m))) {
            uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset() + s_flip.pend_base;
            uint32_t y;
            for (y = 0; y < s_flip.pend_h; y++)
                memcpy(mem + (size_t)y * s_flip.pend_pitch,
                       (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                       (size_t)s_flip.pend_w * s_flip.pend_bpp);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)t, 0);
            s_stat.readbacks++;
            shown = s_flip.pend_base;
            if (pitch_out)
                *pitch_out = s_flip.pend_pitch;
        }
    }

    if (r && r->gpu_dirty) {
        if (!s_flip.stg[0] || s_flip.w != r->w || s_flip.h != r->h || s_flip.bpp != r->bpp) {
            D3D11_TEXTURE2D_DESC td;
            for (i = 0; i < 2; i++)
                if (s_flip.stg[i]) {
                    ID3D11Texture2D_Release(s_flip.stg[i]);
                    s_flip.stg[i] = NULL;
                }
            ID3D11Texture2D_GetDesc(r->tex, &td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            for (i = 0; i < 2; i++)
                if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_flip.stg[i])))
                    return shown;
            s_flip.w = r->w; s_flip.h = r->h; s_flip.bpp = r->bpp;
        }
        s_flip.pend_slot = s_flip.next;
        s_flip.next ^= 1;
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_flip.stg[s_flip.pend_slot],
                                         (ID3D11Resource *)r->tex);
        ID3D11DeviceContext_Flush(s_ctx);
        s_flip.pending = 1;
        s_flip.pend_base = r->base; s_flip.pend_pitch = r->pitch;
        s_flip.pend_w = r->w; s_flip.pend_h = r->h; s_flip.pend_bpp = r->bpp;

    }

    if (!s_flip_lazy()) {
        for (i = 0; i < NV_GPU_MAX_RT; i++)
            if (s_rt[i].tex && s_rt[i].gpu_dirty && &s_rt[i] != r)
                rt_readback(&s_rt[i]);
    }
    return shown;
}

#define NV_GPU_MAX_DS 8
static struct {
    uint32_t key, w, h, last_use;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} s_ds[NV_GPU_MAX_DS];

static ID3D11DepthStencilView *ds_get(uint32_t key, uint32_t w, uint32_t h)
{
    int i, lru = 0;
    D3D11_TEXTURE2D_DESC td;

    for (i = 0; i < NV_GPU_MAX_DS; i++) {
        if (s_ds[i].tex && s_ds[i].key == key && s_ds[i].w == w && s_ds[i].h == h) {
            s_ds[i].last_use = ++s_serial;
            return s_ds[i].dsv;
        }
        if (!s_ds[i].tex || s_ds[i].last_use < s_ds[lru].last_use)
            lru = i;
    }
    if (s_ds[lru].dsv) ID3D11DepthStencilView_Release(s_ds[lru].dsv);
    if (s_ds[lru].tex) ID3D11Texture2D_Release(s_ds[lru].tex);
    memset(&s_ds[lru], 0, sizeof s_ds[lru]);
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ds[lru].tex)))
        return NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ds[lru].tex,
                                                   NULL, &s_ds[lru].dsv))) {
        ID3D11Texture2D_Release(s_ds[lru].tex);
        s_ds[lru].tex = NULL;
        return NULL;
    }

    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_ds[lru].dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    s_ds[lru].key = key; s_ds[lru].w = w; s_ds[lru].h = h;
    s_ds[lru].last_use = ++s_serial;
    return s_ds[lru].dsv;
}

#define NV_GPU_TEX_SLOTS 2048

typedef struct {
    uint32_t used, offset, width, height, pitch, color, palette, cube;
    uint64_t hash;
    uint32_t checked_frame, checked_serial;
    ID3D11ShaderResourceView *srv;
} TexEnt;

static TexEnt s_tex[NV_GPU_TEX_SLOTS];
static uint32_t s_tex_count;

static uint64_t hash_mem(const uint8_t *p, size_t n)
{
    uint64_t a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull,
             d = 0x27D4EB2F165667C5ull, w;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(&w, p + i, 8);      a = (a ^ w) * 0x100000001B3ull;
        memcpy(&w, p + i + 8, 8);  b = (b ^ w) * 0x100000001B3ull;
        memcpy(&w, p + i + 16, 8); c = (c ^ w) * 0x100000001B3ull;
        memcpy(&w, p + i + 24, 8); d = (d ^ w) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        a = (a ^ p[i]) * 0x100000001B3ull;
    a ^= (b >> 7) ^ (b << 13);
    a = (a ^ c) * 0xFF51AFD7ED558CCDull;
    a ^= d + (a >> 33);
    return a;
}

static void tex_flush_all(void)
{
    uint32_t i;
    for (i = 0; i < NV_GPU_TEX_SLOTS; i++)
        if (s_tex[i].srv)
            ID3D11ShaderResourceView_Release(s_tex[i].srv);
    memset(s_tex, 0, sizeof s_tex);
    s_tex_count = 0;
}

static int tex_rt_touched(uint32_t off, uint32_t bytes, uint32_t serial)
{
    int i, hit = 0;
    for (i = 0; i < NV_GPU_MAX_RT; i++) {
        Rt *r = &s_rt[i];
        if (r->tex && off < r->base + rt_bytes(r) && r->base < off + bytes) {
            rt_readback(r);
            if (r->last_draw > serial)
                hit = 1;
        }
    }
    return hit;
}

static ID3D11ShaderResourceView *tex_get(const NvGpuTex *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t h = (t->offset * 2654435761u) ^ (t->width << 16) ^ t->height ^ (t->color << 8)
                 ^ (t->pitch * 40503u) ^ (t->palette * 97u) ^ t->cube;
    uint32_t slot = h & (NV_GPU_TEX_SLOTS - 1), probes = 0;
    TexEnt *e;
    uint64_t hash;
    int touched;

    for (;;) {
        e = &s_tex[slot];
        if (!e->used)
            break;
        if (e->offset == t->offset && e->width == t->width && e->height == t->height
            && e->pitch == t->pitch && e->color == t->color && e->palette == t->palette
            && e->cube == t->cube)
            break;
        if (++probes > 64) {
            tex_flush_all();
            e = &s_tex[h & (NV_GPU_TEX_SLOTS - 1)];
            break;
        }
        slot = (slot + 1) & (NV_GPU_TEX_SLOTS - 1);
    }
    touched = tex_rt_touched(t->offset, t->bytes, e->used ? e->checked_serial : 0);
    if (e->used && e->checked_frame == s_frame && !touched)
        return e->srv;
    s_stat.tex_hashes++;
    hash = hash_mem(mem + t->offset, t->bytes);
    if (t->palette)
        hash ^= hash_mem(mem + t->palette, 1024) * 31u;
    e->checked_frame = s_frame;
    e->checked_serial = s_serial;
    if (e->used && e->hash == hash)
        return e->srv;

    if (!e->used) {
        if (++s_tex_count > NV_GPU_TEX_SLOTS * 3 / 4) {
            tex_flush_all();
            return tex_get(t);
        }
    }
    if (e->srv) {
        ID3D11ShaderResourceView_Release(e->srv);
        e->srv = NULL;
    }
    e->used = 1;
    e->offset = t->offset; e->width = t->width; e->height = t->height;
    e->pitch = t->pitch; e->color = t->color; e->palette = t->palette; e->cube = t->cube;
    e->hash = hash;
    {
        uint32_t faces = t->cube ? 6 : 1, f, ok = 1;
        size_t texels = (size_t)t->width * t->height;
        uint32_t *px = (uint32_t *)malloc(texels * 4 * faces);
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd[6];
        ID3D11Texture2D *tx = NULL;

        if (!px)
            return NULL;
        s_stat.tex_decodes++;
        for (f = 0; f < faces && ok; f++)
            ok = nv2a_exec_tex_decode(t->src, f * t->face_stride, px + texels * f);
        if (ok) {
            memset(&td, 0, sizeof td);
            td.Width = t->width; td.Height = t->height;
            td.MipLevels = 1; td.ArraySize = faces;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
            for (f = 0; f < faces; f++) {
                sd[f].pSysMem = px + texels * f;
                sd[f].SysMemPitch = t->width * 4;
                sd[f].SysMemSlicePitch = (UINT)(texels * 4);
            }
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &tx))) {
                ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tx, NULL, &e->srv);
                ID3D11Texture2D_Release(tx);
            }
        }
        free(px);
    }
    return e->srv;
}

void nv2a_gpu_frame(void)
{
    s_frame++;
}

#define NV_GPU_STATE_CACHE 128

static struct { D3D11_BLEND_DESC d; ID3D11BlendState *o; } s_bs[NV_GPU_STATE_CACHE];
static struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *o; } s_dss[NV_GPU_STATE_CACHE];
static struct { D3D11_SAMPLER_DESC d; ID3D11SamplerState *o; } s_ss[NV_GPU_STATE_CACHE];
static int s_nbs, s_ndss, s_nss;

static D3D11_BLEND blend_of(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP blend_op_of(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *blend_state(const NvGpuState *s)
{
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &d.RenderTarget[0];
    int i;
    uint32_t m = s->color_mask;

    memset(&d, 0, sizeof d);
    t->BlendEnable = s->blend_enable && !(s->blend_src == 1 && s->blend_dst == 0);
    t->SrcBlend = blend_of(s->blend_src, 0);
    t->DestBlend = blend_of(s->blend_dst, 0);
    t->BlendOp = blend_op_of(s->blend_eq);
    t->SrcBlendAlpha = blend_of(s->blend_src, 1);
    t->DestBlendAlpha = blend_of(s->blend_dst, 1);
    t->BlendOpAlpha = t->BlendOp;
    if (!t->BlendEnable) {
        t->SrcBlend = t->SrcBlendAlpha = D3D11_BLEND_ONE;
        t->DestBlend = t->DestBlendAlpha = D3D11_BLEND_ZERO;
        t->BlendOp = t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    t->RenderTargetWriteMask = (UINT8)(((m & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                                     | ((m & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                                     | ((m & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                                     | ((m & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    for (i = 0; i < s_nbs; i++)
        if (!memcmp(&s_bs[i].d, &d, sizeof d))
            return s_bs[i].o;
    if (s_nbs == NV_GPU_STATE_CACHE) {
        for (i = 0; i < s_nbs; i++) ID3D11BlendState_Release(s_bs[i].o);
        s_nbs = 0;
    }
    s_bs[s_nbs].d = d;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &s_bs[s_nbs].o)))
        return NULL;
    return s_bs[s_nbs++].o;
}

static D3D11_COMPARISON_FUNC cmp_of(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP sop_of(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *depth_state(const NvGpuState *s)
{
    static int stencil = -1;
    D3D11_DEPTH_STENCIL_DESC d;
    int i;

    if (stencil < 0) {
        const char *e = getenv("RECOMP_GPU_STENCIL");
        stencil = !(e && e[0] == '0');
    }
    memset(&d, 0, sizeof d);

    d.DepthEnable = s->depth_test || s->depth_write;
    d.DepthFunc = s->depth_test ? cmp_of(s->depth_func) : D3D11_COMPARISON_ALWAYS;
    d.DepthWriteMask = s->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    if (stencil && s->stencil_enable) {
        d.StencilEnable = TRUE;
        d.StencilReadMask = (UINT8)s->stencil_func_mask;
        d.StencilWriteMask = (UINT8)s->stencil_write_mask;
        d.FrontFace.StencilFunc = cmp_of(s->stencil_func);
        d.FrontFace.StencilFailOp = sop_of(s->stencil_fail);
        d.FrontFace.StencilDepthFailOp = sop_of(s->stencil_zfail);
        d.FrontFace.StencilPassOp = sop_of(s->stencil_zpass);
        d.BackFace = d.FrontFace;
    }
    for (i = 0; i < s_ndss; i++)
        if (!memcmp(&s_dss[i].d, &d, sizeof d))
            return s_dss[i].o;
    if (s_ndss == NV_GPU_STATE_CACHE) {
        for (i = 0; i < s_ndss; i++) ID3D11DepthStencilState_Release(s_dss[i].o);
        s_ndss = 0;
    }
    s_dss[s_ndss].d = d;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s_dss[s_ndss].o)))
        return NULL;
    return s_dss[s_ndss++].o;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_of(uint32_t m, uint32_t border_color)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return border_color ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *sampler_state(const NvGpuState *s, const NvGpuTex *t)
{
    D3D11_SAMPLER_DESC d;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int linear, i;

    if (s->use_rc)
        linear = s->bilinear || mag == 2 || min == 2 || min == 4 || min == 6;
    else
        linear = s->bilinear;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = addr_of(t->addr_u, t->border_color);
    d.AddressV = addr_of(t->addr_v, t->border_color);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.BorderColor[0] = (float)((t->border >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = (float)((t->border >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (float)(t->border & 0xFF) / 255.0f;
    d.BorderColor[3] = (float)(t->border >> 24) / 255.0f;
    for (i = 0; i < s_nss; i++)
        if (!memcmp(&s_ss[i].d, &d, sizeof d))
            return s_ss[i].o;
    if (s_nss == NV_GPU_STATE_CACHE) {
        for (i = 0; i < s_nss; i++) ID3D11SamplerState_Release(s_ss[i].o);
        s_nss = 0;
    }
    s_ss[s_nss].d = d;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s_ss[s_nss].o)))
        return NULL;
    return s_ss[s_nss++].o;
}

void nv2a_gpu_push(const NvGpuVertex v[3])
{
    if (s_pend_n + 3 > s_pend_cap) {
        uint32_t cap = s_pend_cap ? s_pend_cap * 2 : 3u * 8192u;
        NvGpuVertex *n = (NvGpuVertex *)realloc(s_pend, cap * sizeof *n);
        if (!n)
            return;
        s_pend = n;
        s_pend_cap = cap;
    }
    memcpy(&s_pend[s_pend_n], v, 3 * sizeof *v);
    s_pend_n += 3;
}

void nv2a_gpu_draw_discard(void)
{
    s_pend_n = 0;
}

static void unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static int bind_target(const NvGpuState *s, Rt **out_rt)
{
    Rt *r = rt_get(s);
    ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    uint32_t x1, y1;

    if (!r)
        return 0;
    dsv = ds_get(s->zeta_key, r->w, r->h);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, dsv);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->w; vp.Height = (float)r->h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    x1 = s->clip_x + s->clip_w; y1 = s->clip_y + s->clip_h;
    if (x1 > r->w) x1 = r->w;
    if (y1 > r->h) y1 = r->h;
    sc.left = (LONG)s->clip_x; sc.top = (LONG)s->clip_y;
    sc.right = (LONG)x1; sc.bottom = (LONG)y1;
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    *out_rt = r;
    return 1;
}

static int draw_begin(const NvGpuState *s, Rt **out_r)
{
    Rt *r;
    Cb cb;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    float bf[4];
    uint32_t i, st;

    if (!bind_target(s, &r))
        return 0;

    memset(&cb, 0, sizeof cb);
    cb.vp[0] = 2.0f / (float)r->w;
    cb.vp[1] = 2.0f / (float)r->h;
    cb.vp[2] = s->z16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    for (i = 0; i < 8; i++) {
        cb.cicw[i] = s->rc.color_icw[i]; cb.aicw[i] = s->rc.alpha_icw[i];
        cb.cocw[i] = s->rc.color_ocw[i]; cb.aocw[i] = s->rc.alpha_ocw[i];
        unpack(s->rc.factor0[i], cb.f0[i]);
        unpack(s->rc.factor1[i], cb.f1[i]);
    }
    cb.fin[0] = s->rc.final0; cb.fin[1] = s->rc.final1;
    cb.fin[2] = s->rc.control; cb.fin[3] = s->rc.stage_program;
    unpack(s->rc.final_c0, cb.fc0);
    unpack(s->rc.final_c1, cb.fc1);

    cb.fogc[0] = (float)(s->fog_color & 0xFF) / 255.0f;
    cb.fogc[1] = (float)((s->fog_color >> 8) & 0xFF) / 255.0f;
    cb.fogc[2] = (float)((s->fog_color >> 16) & 0xFF) / 255.0f;
    cb.fogc[3] = (float)(s->fog_color >> 24) / 255.0f;
    cb.misc[0] = s->use_rc;
    cb.misc[1] = s->textured;
    cb.misc[2] = s->alpha_test;
    cb.misc[3] = s->alpha_func;
    cb.misc2[0] = (float)(s->alpha_ref & 0xFF);
    cb.misc3[0] = s->clip_plane_mode;

    for (st = 0; st < 4; st++) {
        const NvGpuTex *t = &s->tex[st];
        ID3D11ShaderResourceView *v = t->valid ? tex_get(t) : NULL;
        srv[st] = s_white2d;
        srv[st + 4] = s_whitecube;
        cb.texinfo[st][0] = t->width ? (float)t->width : 1.0f;
        cb.texinfo[st][1] = t->height ? (float)t->height : 1.0f;
        cb.texinfo[st][2] = t->normalized ? 1.0f : 0.0f;
        cb.texinfo[st][3] = (t->valid && t->cube) ? 1.0f : 0.0f;
        if (!v) {
            cb.texinfo[st][2] = 1.0f;
            cb.texinfo[st][3] = t->cube ? 1.0f : 0.0f;
        } else if (t->cube) {
            srv[st + 4] = v;
        } else {
            srv[st] = v;
        }
        smp[st] = sampler_state(s, t);
    }

    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, &cb, sizeof cb);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);

    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    {
        uint32_t bc = s->blend_color;
        int alpha_const = s->blend_src == 0x8003 || s->blend_src == 0x8004
                       || s->blend_dst == 0x8003 || s->blend_dst == 0x8004;
        unpack(bc, bf);
        if (alpha_const)
            bf[0] = bf[1] = bf[2] = bf[3];
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(s), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_state(s), s->stencil_ref & 0xFF);

    if (s->count_zpass && s_occ)
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_occ);
    *out_r = r;
    return 1;
}

static uint32_t draw_end(const NvGpuState *s, Rt *r, uint32_t tris)
{
    uint32_t zpass = 0;

    if (s->count_zpass && s_occ) {
        UINT64 px = 0;
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_occ);
        while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_occ, &px,
                                           sizeof px, 0) == S_FALSE)
            YieldProcessor();
        zpass = (uint32_t)px;
    }
    s_stat.draws++;
    s_stat.tris += tris;
    r->gpu_dirty = 1;
    r->last_draw = ++s_serial;
    return zpass;
}

uint32_t nv2a_gpu_draw(const NvGpuState *s)
{
    Rt *r;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t done = 0, tris;
    UINT stride = sizeof(NvGpuVertex), offset = 0;

    if (!s_pend_n)
        return 0;
    if (!draw_begin(s, &r)) {
        s_pend_n = 0;
        return 0;
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_il);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    while (done < s_pend_n) {
        uint32_t n = s_pend_n - done;
        if (n > s_vb_cap) n = s_vb_cap - s_vb_cap % 3;
        if (s_vb_pos + n > s_vb_cap) {
            s_vb_pos = 0;
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m)))
                break;
        } else if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                                  D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
            break;
        }
        memcpy((NvGpuVertex *)m.pData + s_vb_pos, s_pend + done, n * sizeof(NvGpuVertex));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        ID3D11DeviceContext_Draw(s_ctx, n, s_vb_pos);
        s_vb_pos += n;
        done += n;
    }
    tris = s_pend_n / 3;
    s_pend_n = 0;
    return draw_end(s, r, tris);
}

#define NV_GPU_VSH_MAX 512
#define NV_GPU_VB2_BYTES (32u << 20)
#define NV_GPU_IB_BYTES  (4u << 20)

typedef struct {
    uint64_t key;
    uint32_t inputs, stride;
    int bad;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
} VshEnt;

static VshEnt s_vsh[NV_GPU_VSH_MAX];
static int s_nvsh;
static VshEnt *s_vsh_last;
static ID3D11Buffer *s_vcb, *s_vpb, *s_vb2, *s_ib;
static uint32_t s_vcb_rev, s_vb2_pos, s_ib_pos;
static int s_vsh_ready = -1;
static struct { uint64_t programs, failed, draws, tris, cache_hits; double compile_ms; } s_vstat;

static int vsh_buffers(void)
{
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    if (s_vsh_ready >= 0)
        return s_vsh_ready;
    s_vsh_ready = 0;
    if (!s_compile)
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 192 * 16;
    if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vcb))) { fail("vsh constants", hr); return 0; }
    bd.ByteWidth = 32;
    if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vpb))) { fail("vsh params", hr); return 0; }
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = NV_GPU_VB2_BYTES;
    if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb2))) { fail("vsh vertices", hr); return 0; }
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = NV_GPU_IB_BYTES;
    if (FAILED(hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib))) { fail("vsh indices", hr); return 0; }
    s_vcb_rev = 0;
    s_vsh_ready = 1;
    fprintf(stderr, "[GPU-D3D11] vertex programs on the host GPU (RECOMP_GPU_VSH=0 keeps them on the CPU)\n");
    return 1;
}

static VshEnt *vsh_get(uint64_t key)
{
    static char *src;
    enum { SRC_CAP = 256 * 1024 };
    D3D11_INPUT_ELEMENT_DESC el[16];
    ID3DBlob *code = NULL, *err = NULL;
    LARGE_INTEGER t0, t1, f;
    char path[MAX_PATH + 64];
    void *cached = NULL;
    size_t cached_size = 0;
    VshEnt *e;
    uint32_t inputs = 0, n = 0, i;
    HRESULT hr;
    int k;

    if (s_vsh_last && s_vsh_last->key == key)
        return s_vsh_last;
    for (k = 0; k < s_nvsh; k++)
        if (s_vsh[k].key == key)
            return s_vsh_last = &s_vsh[k];
    if (s_nvsh == NV_GPU_VSH_MAX || !vsh_buffers())
        return NULL;
    if (!src && !(src = (char *)malloc(SRC_CAP)))
        return NULL;
    e = &s_vsh[s_nvsh++];
    memset(e, 0, sizeof *e);
    e->key = key;
    s_vsh_last = e;
    if (!nv2a_vsh_hlsl(src, SRC_CAP, &inputs)) {
        e->bad = 1;
        s_vstat.failed++;
        return e;
    }

    {
        size_t len = strlen(src);
        uint64_t sh = hash_mem((const uint8_t *)src, len);
        FILE *fp;
        const char *dir = getenv("RECOMP_SHADER_CACHE");
        if (!dir || !dir[0])
            dir = "shader_cache";
        if (!(dir[0] == '0' && !dir[1])) {
            snprintf(path, sizeof path, "%s\\vs_%016llX_%u.dxbc", dir,
                     (unsigned long long)sh, (unsigned)len);
            if ((fp = fopen(path, "rb")) != NULL) {
                long sz;
                fseek(fp, 0, SEEK_END);
                sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz > 0 && sz < (8 << 20) && (cached = malloc((size_t)sz)) != NULL
                    && fread(cached, 1, (size_t)sz, fp) == (size_t)sz)
                    cached_size = (size_t)sz;
                fclose(fp);
            }
        } else {
            path[0] = 0;
        }
        QueryPerformanceCounter(&t0);
        if (cached_size) {
            hr = S_OK;
            s_vstat.cache_hits++;
        } else {
            hr = s_compile(src, len, "nv2a_vsh", NULL, NULL, "vsmain", "vs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS, 0,
                           &code, &err);
            if (SUCCEEDED(hr) && path[0]) {
                CreateDirectoryA(dir, NULL);
                if ((fp = fopen(path, "wb")) != NULL) {
                    fwrite(ID3D10Blob_GetBufferPointer(code), 1,
                           ID3D10Blob_GetBufferSize(code), fp);
                    fclose(fp);
                }
            }
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        s_vstat.compile_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    if (FAILED(hr)) {
        static int told;
        if (told++ < 4) {
            fprintf(stderr, "[GPU-D3D11] vertex program %016llX: %s\n", (unsigned long long)key,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
            if (getenv("RECOMP_GPU_VSH_DUMP"))
                fprintf(stderr, "%s\n", src);
        }
        if (err) ID3D10Blob_Release(err);
        e->bad = 1;
        s_vstat.failed++;
        return e;
    }
    if (err) ID3D10Blob_Release(err);
    for (i = 0; i < 16; i++)
        if (inputs & (1u << i)) {
            el[n].SemanticName = "ATTR";
            el[n].SemanticIndex = i;
            el[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            el[n].InputSlot = 0;
            el[n].AlignedByteOffset = 16 * n;
            el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            el[n].InstanceDataStepRate = 0;
            n++;
        }
    {
        const void *bc = cached_size ? cached : ID3D10Blob_GetBufferPointer(code);
        SIZE_T bn = cached_size ? cached_size : ID3D10Blob_GetBufferSize(code);
        hr = ID3D11Device_CreateVertexShader(s_dev, bc, bn, NULL, &e->vs);
        if (SUCCEEDED(hr) && n)
            hr = ID3D11Device_CreateInputLayout(s_dev, el, n, bc, bn, &e->il);
    }
    if (code) ID3D10Blob_Release(code);
    free(cached);
    if (FAILED(hr)) {
        fail("vertex program shader", hr);
        e->bad = 1;
        s_vstat.failed++;
        return e;
    }
    e->inputs = inputs;
    e->stride = n ? 16 * n : 16;
    s_vstat.programs++;
    return e;
}

uint32_t nv2a_gpu_vsh_inputs(uint64_t key, int *ok)
{
    VshEnt *e = key ? vsh_get(key) : NULL;
    *ok = e && !e->bad;
    return *ok ? e->inputs : 0;
}

static long ring_put(ID3D11Buffer *b, uint32_t *pos, uint32_t cap, const void *p,
                     uint32_t bytes, uint32_t align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    uint32_t at = (*pos + align - 1) / align * align;

    if (bytes > cap)
        return -1;
    if (at + bytes > cap) {
        at = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0, mode, 0, &m)))
        return -1;
    memcpy((uint8_t *)m.pData + at, p, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    *pos = at + bytes;
    return (long)at;
}

int nv2a_gpu_draw_vsh(const NvGpuState *s, const NvGpuVshDraw *d, uint32_t *zpass)
{
    VshEnt *e = vsh_get(d->key);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *cbs[3];
    Rt *r;
    long vo, io;
    UINT stride, offset;
    float vp[8];

    if (!e || e->bad || !d->nindices)
        return 0;
    if (d->nverts * e->stride > NV_GPU_VB2_BYTES || d->nindices * 4 > NV_GPU_IB_BYTES)
        return 0;
    if (!draw_begin(s, &r))
        return 0;
    if (s_vcb_rev != nv2a_vsh_const_revision()) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vcb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, nv2a_vsh_constants(), 192 * 16);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vcb, 0);
            s_vcb_rev = nv2a_vsh_const_revision();
        }
    }
    vp[0] = d->aa_sx; vp[1] = d->aa_sy; vp[2] = d->near_z; vp[3] = d->near_clip ? 1.0f : 0.0f;
    vp[4] = d->fog_px; vp[5] = d->fog_py;
    vp[6] = d->fog_enable ? 1.0f : 0.0f; vp[7] = (float)d->fog_mode;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vpb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, vp, sizeof vp);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vpb, 0);
    }
    vo = ring_put(s_vb2, &s_vb2_pos, NV_GPU_VB2_BYTES, d->verts, d->nverts * e->stride, e->stride);
    io = ring_put(s_ib, &s_ib_pos, NV_GPU_IB_BYTES, d->indices, d->nindices * 4, 4);
    if (vo < 0 || io < 0) {
        *zpass = draw_end(s, r, 0);
        return 1;
    }
    stride = e->stride;
    offset = (UINT)vo;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, e->il);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb2, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, (UINT)io);
    ID3D11DeviceContext_VSSetShader(s_ctx, e->vs, NULL, 0);
    cbs[0] = s_cb; cbs[1] = s_vcb; cbs[2] = s_vpb;
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, cbs);
    ID3D11DeviceContext_DrawIndexed(s_ctx, d->nindices, 0, 0);
    s_vstat.draws++;
    s_vstat.tris += d->nindices / 3;
    *zpass = draw_end(s, r, d->nindices / 3);
    return 1;
}

void nv2a_gpu_clear(const NvGpuState *s, uint32_t flags, uint32_t argb,
                    float z01, uint32_t stencil,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    Rt *r;
    ID3D11DepthStencilView *dsv;

    if (!bind_target(s, &r))
        return;
    s_stat.clears++;
    if ((flags & 0xF0) && x1 > x0 && y1 > y0) {
        float c[4];
        unpack(argb, c);
        if (x1 > r->w) x1 = r->w;
        if (y1 > r->h) y1 = r->h;
        if (s_ctx1) {
            D3D11_RECT rc = { (LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1 };
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
        } else {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        }
        r->gpu_dirty = 1;
        r->last_draw = ++s_serial;
    }
    if (flags & 0x03) {
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        dsv = ds_get(s->zeta_key, r->w, r->h);
        if (dsv)
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, dsv, f, z01, (UINT8)stencil);
    }
}

void nv2a_gpu_report(void)
{
    if (!s_dev)
        return;
    fprintf(stderr, "[GPU-D3D11] draws %llu, triangles %llu, clears %llu, surface uploads %llu,"
                    " readbacks %llu, texture checks %llu, decodes %llu\n",
            (unsigned long long)s_stat.draws, (unsigned long long)s_stat.tris,
            (unsigned long long)s_stat.clears, (unsigned long long)s_stat.uploads,
            (unsigned long long)s_stat.readbacks, (unsigned long long)s_stat.tex_hashes,
            (unsigned long long)s_stat.tex_decodes);
    if (s_vsh_ready > 0)
        fprintf(stderr, "[GPU-D3D11] vertex programs: %llu ready (%llu from disk, %.0f ms), %llu refused,"
                        " %llu draws, %llu triangles\n",
                (unsigned long long)s_vstat.programs, (unsigned long long)s_vstat.cache_hits,
                s_vstat.compile_ms,
                (unsigned long long)s_vstat.failed, (unsigned long long)s_vstat.draws,
                (unsigned long long)s_vstat.tris);
}

#else

#include "nv2a_gpu.h"
int nv2a_gpu_enabled(void) { return 0; }
void nv2a_gpu_push(const NvGpuVertex v[3]) { (void)v; }
uint32_t nv2a_gpu_draw(const NvGpuState *s) { (void)s; return 0; }
void nv2a_gpu_draw_discard(void) {}
uint32_t nv2a_gpu_vsh_inputs(uint64_t key, int *ok) { (void)key; *ok = 0; return 0; }
int nv2a_gpu_draw_vsh(const NvGpuState *s, const NvGpuVshDraw *d, uint32_t *zpass)
{ (void)s; (void)d; (void)zpass; return 0; }
void nv2a_gpu_clear(const NvGpuState *s, uint32_t flags, uint32_t argb, float z01,
                    uint32_t stencil, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)s; (void)flags; (void)argb; (void)z01; (void)stencil; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_gpu_cpu_write(uint32_t base, uint32_t bytes) { (void)base; (void)bytes; }
void nv2a_gpu_sync_all(void) {}
uint32_t nv2a_gpu_flip(uint32_t va, uint32_t *pitch_out) { (void)va; (void)pitch_out; return 0; }
void nv2a_gpu_frame(void) {}
void nv2a_gpu_report(void) {}

#endif

