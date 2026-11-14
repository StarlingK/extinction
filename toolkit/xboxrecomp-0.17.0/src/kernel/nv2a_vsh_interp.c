#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
static int      s_const_trace = -1;

static uint32_t s_program_revision;

static uint32_t s_const_revision = 1;

static int vsh_dump_enabled(void)
{
    if (s_const_trace < 0)
        s_const_trace = getenv("RECOMP_VSH_DUMP") != NULL;
    return s_const_trace;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS) {
        s_program[s_load_slot][s_load_word] = word;
        s_program_revision++;
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot)
{
    if (s_start_slot != slot) {
        s_start_slot = slot;
        s_program_revision++;
    }
}
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
uint32_t nv2a_vsh_cxt_write(void) { return s_cxt_write; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
    if (vsh_dump_enabled())
        fprintf(stderr, "  [VSH] constant load -> c[%u]\n", index);
}

void nv2a_vsh_constant_word(uint32_t word)
{
    s_const_revision++;
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    else if (vsh_dump_enabled()) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "  [VSH] constant write DROPPED (c[%u] >= %u)\n",
                    s_const_load, NV2A_VSH_CONSTANTS);
    }
    if (vsh_dump_enabled() && s_const_word == 3 && s_const_load >= 64) {
        static int shown;
        if (shown++ < 96)
            fprintf(stderr, "  [VSH] c[%u] = %.6g %.6g %.6g %.6g\n",
                    s_const_load, s_const[s_const_load][0],
                    s_const[s_const_load][1], s_const[s_const_load][2],
                    s_const[s_const_load][3]);
    }
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    s_const_revision++;
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS) {
        memcpy(s_program[slot], words, sizeof s_program[slot]);
        s_program_revision++;
    }
}

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    s_const_revision++;
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

typedef struct { float v[4]; } vec4;

static vec4 read_src(const uint32_t *ins, int which,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    uint32_t neg, swz, mux, reg;
    const float *s;
    vec4 r;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }

    if (mux == 1)
        s = (reg == 12) ? opos->v : (reg < 12 ? temp[reg].v : temp[0].v);
    else if (mux == 2)
        s = in[field(ins, 1, 9, 4)];
    else {
        int ci = (int)field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[(swz >> 6) & 3];
    r.v[1] = s[(swz >> 4) & 3];
    r.v[2] = s[(swz >> 2) & 3];
    r.v[3] = s[swz & 3];
    if (neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;
    case 3: {
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;
    case 5: {
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

static void vsh_finish(Nv2aVshOutput *out, const vec4 *opos,
                       float outregs[13][4])
{
    memcpy(out->pos, opos->v, sizeof out->pos);

    for (int k = 0; k < 4; k++) {
        float c0 = outregs[3][k], c1 = outregs[4][k];
        out->d0[k] = c0 > 1.0f ? 1.0f : (c0 >= 0.0f ? c0 : 0.0f);
        out->d1[k] = c1 > 1.0f ? 1.0f : (c1 >= 0.0f ? c1 : 0.0f);
    }
    memcpy(out->fog, outregs[5], sizeof out->fog);
    memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
    memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
    memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
    memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
}

static void vsh_init_regs(vec4 temp[12], vec4 *opos, float outregs[13][4])
{
    memset(temp, 0, 12 * sizeof *temp);
    memset(opos, 0, sizeof *opos);
    memset(outregs, 0, 13 * sizeof *outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;

    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;
}

enum { DEC_TEMP = 1, DEC_INPUT = 2, DEC_CONST = 3 };

typedef struct {
    const float *cptr;
    uint8_t mux, idx, neg, rel, ident;
    uint8_t sw[4];
    uint8_t c;
} DecSrc;

typedef struct {
    uint8_t mac, ilu, mac_mask, ilu_mask, mac_dst, ilu_dst, omask, oaddr;
    uint8_t to_o, from_ilu, final, reads;
    DecSrc  src[3];
} DecIns;

#define DEC_NO_DST 0xFF

static DecIns   s_dec[NV2A_VSH_SLOTS];
static uint32_t s_dec_count;
static uint32_t s_dec_rev;
static int      s_dec_valid;

static void dec_src(const uint32_t *ins, int which, DecSrc *d)
{
    uint32_t swz, reg;

    if (which == 0) {
        d->neg = (uint8_t)field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        d->mux = (uint8_t)field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        d->neg = (uint8_t)field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        d->mux = (uint8_t)field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        d->neg = (uint8_t)field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        d->mux = (uint8_t)field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    d->sw[0] = (uint8_t)((swz >> 6) & 3);
    d->sw[1] = (uint8_t)((swz >> 4) & 3);
    d->sw[2] = (uint8_t)((swz >> 2) & 3);
    d->sw[3] = (uint8_t)(swz & 3);
    d->ident = swz == 0x1B;
    d->c = (uint8_t)field(ins, 1, 13, 8);
    d->rel = (uint8_t)field(ins, 3, 1, 1);
    d->cptr = NULL;
    if (d->mux == DEC_TEMP) {
        d->idx = (uint8_t)(reg <= 12 ? reg : 0);
    } else if (d->mux == DEC_INPUT) {
        d->idx = (uint8_t)field(ins, 1, 9, 4);
    } else {
        d->mux = DEC_CONST;
        d->idx = 0;
        if (!d->rel)
            d->cptr = s_const[d->c < NV2A_VSH_CONSTANTS ? d->c : 0];
    }
}

static uint8_t dec_reads(uint32_t mac, uint32_t ilu)
{
    uint8_t r = 0;

    if (mac)
        r |= 1;
    if (mac == 2 || (mac >= 4 && mac <= 12))
        r |= 2;
    if (mac == 3 || mac == 4 || ilu)
        r |= 4;
    return r;
}

static void dec_program(void)
{
    uint32_t s, n = 0;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        DecIns *d = &s_dec[n++];
        uint32_t tdst = field(ins, 3, 20, 4), it;
        int k;

        d->mac = (uint8_t)field(ins, 1, 21, 4);
        d->ilu = (uint8_t)field(ins, 1, 25, 3);
        d->mac_mask = (uint8_t)field(ins, 3, 24, 4);
        d->ilu_mask = (uint8_t)field(ins, 3, 16, 4);

        it = d->mac ? 1u : tdst;
        d->mac_dst = (uint8_t)(tdst <= 12 ? tdst : DEC_NO_DST);
        d->ilu_dst = (uint8_t)(it <= 12 ? it : DEC_NO_DST);
        if (!d->mac || d->mac == 13)
            d->mac_mask = 0;
        if (!d->ilu)
            d->ilu_mask = 0;
        d->omask = (uint8_t)field(ins, 3, 12, 4);
        d->oaddr = (uint8_t)field(ins, 3, 3, 8);
        d->to_o = (uint8_t)field(ins, 3, 11, 1);
        d->from_ilu = (uint8_t)field(ins, 3, 2, 1);
        d->final = (uint8_t)field(ins, 3, 0, 1);
        d->reads = dec_reads(d->mac, d->ilu);
        for (k = 0; k < 3; k++)
            dec_src(ins, k, &d->src[k]);
        if (d->final)
            break;
    }
    s_dec_count = n;
    s_dec_rev = s_program_revision;
    s_dec_valid = 1;
}

static vec4 dec_read(const DecSrc *d, const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *regs, int a0)
{
    const float *s;
    vec4 r;

    if (d->mux == DEC_TEMP)
        s = regs[d->idx].v;
    else if (d->mux == DEC_INPUT)
        s = in[d->idx];
    else if (d->cptr)
        s = d->cptr;
    else {
        int ci = (int)d->c + a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    if (d->ident) {
        memcpy(r.v, s, sizeof r.v);
    } else {
        r.v[0] = s[d->sw[0]];
        r.v[1] = s[d->sw[1]];
        r.v[2] = s[d->sw[2]];
        r.v[3] = s[d->sw[3]];
    }
    if (d->neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static int vsh_run_decoded(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    vec4 regs[13];
    float outregs[13][4];
    int a0 = 0;
    uint32_t i;

    if (!s_dec_valid || s_dec_rev != s_program_revision)
        dec_program();
    vsh_init_regs(regs, &regs[12], outregs);

    for (i = 0; i < s_dec_count; i++) {
        const DecIns *d = &s_dec[i];
        vec4 a = {{0, 0, 0, 0}}, b = a, c = a;
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (d->reads & 1) a = dec_read(&d->src[0], in, regs, a0);
        if (d->reads & 2) b = dec_read(&d->src[1], in, regs, a0);
        if (d->reads & 4) c = dec_read(&d->src[2], in, regs, a0);
        if (d->mac)
            mres = run_mac(d->mac, a, b, c, &a0);
        if (d->ilu)
            ires = run_ilu(d->ilu, c);
        if (d->mac_mask && d->mac_dst != DEC_NO_DST)
            write_masked(regs[d->mac_dst].v, &mres, d->mac_mask);
        if (d->ilu_mask && d->ilu_dst != DEC_NO_DST)
            write_masked(regs[d->ilu_dst].v, &ires, d->ilu_mask);
        if (d->omask) {
            const vec4 *src = d->from_ilu ? &ires : &mres;
            if (!d->to_o) {
                if (s_cxt_write && d->oaddr < NV2A_VSH_CONSTANTS) {
                    write_masked(s_const[d->oaddr], src, d->omask);
                    s_const_revision++;
                }
            } else if (d->oaddr == 0) {
                write_masked(regs[12].v, src, d->omask);
            } else if (d->oaddr < 13) {
                write_masked(outregs[d->oaddr], src, d->omask);
            }
        }
        if (d->final) {
            vsh_finish(out, &regs[12], outregs);
            return 1;
        }
    }
    return 0;
}

static int vsh_run_fields(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out);

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int mode = -1;

    if (mode < 0) {
        const char *d = getenv("RECOMP_VSH_DECODED");
        const char *c = getenv("RECOMP_VSH_CHECK");
        mode = (d && d[0] == '0') ? 0 : 1;
        if (c && c[0] == '1')
            mode = 2;
        if (getenv("RECOMP_VSH_DUMP"))
            mode = 0;
    }
    if (mode == 1)
        return vsh_run_decoded(in, out);
    if (mode == 2 && !s_cxt_write) {

        static unsigned long long checked;
        static unsigned told;
        Nv2aVshOutput ref;
        int r1 = vsh_run_fields(in, &ref);
        int r2 = vsh_run_decoded(in, out);

        if (r1 != r2 || (r1 && memcmp(&ref, out, sizeof ref) != 0)) {
            if (told++ < 16)
                fprintf(stderr, "[VSH-CHECK] decoded run differs (fields %d,"
                        " decoded %d): pos %g %g %g %g vs %g %g %g %g\n",
                        r1, r2, ref.pos[0], ref.pos[1], ref.pos[2], ref.pos[3],
                        out->pos[0], out->pos[1], out->pos[2], out->pos[3]);
            *out = ref;
            return r1;
        }
        ++checked;
        if ((checked & (checked - 1)) == 0 && checked >= 1024)
            fprintf(stderr, "[VSH-CHECK] %llu runs identical\n", checked);
        return r2;
    }
    return vsh_run_fields(in, out);
}

static int vsh_run_fields(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {

        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }

    vsh_init_regs(temp, &opos, outregs);

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        vec4 a = read_src(ins, 0, in, temp, &opos, a0);
        vec4 b = read_src(ins, 1, in, temp, &opos, a0);
        vec4 c = read_src(ins, 2, in, temp, &opos, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {

            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = field(ins, 3, 2, 1) ? &ires : &mres;
            if (!field(ins, 3, 11, 1)) {
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS) {
                    write_masked(s_const[oaddr], src, omask);
                    s_const_revision++;
                }
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
            }
        }
        if (field(ins, 3, 0, 1)) {
            vsh_finish(out, &opos, outregs);
            return 1;
        }
    }
    return 0;
}

const float *nv2a_vsh_constants(void) { return &s_const[0][0]; }
uint32_t nv2a_vsh_const_revision(void) { return s_const_revision; }

uint64_t nv2a_vsh_program_key(void)
{
    static uint32_t rev = 0xFFFFFFFFu;
    static uint64_t key;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t i, j;

    if (!s_dec_valid || s_dec_rev != s_program_revision)
        dec_program();
    if (rev == s_program_revision)
        return key;
    rev = s_program_revision;
    if (!s_dec_count || !s_dec[s_dec_count - 1].final) {
        key = 0;
        return 0;
    }
    for (i = 0; i < s_dec_count; i++)
        for (j = 1; j < 4; j++) {
            h ^= s_program[s_start_slot + i][j];
            h *= 0x100000001B3ull;
        }
    key = h | 1;
    return key;
}

typedef struct { char *p; size_t n, cap; } Sb;

static void sb_put(Sb *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>
static void sb_put(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (b->n >= b->cap)
        return;
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= b->cap - b->n)
        b->n = b->cap;
    else
        b->n += (size_t)k;
}

static void hlsl_src(Sb *b, const DecSrc *d, uint32_t *inputs)
{
    static const char sw[] = "xyzw";
    if (d->neg)
        sb_put(b, "-");
    if (d->mux == DEC_TEMP)
        sb_put(b, "r%u", d->idx);
    else if (d->mux == DEC_INPUT) {
        sb_put(b, "i.v%u", d->idx);
        *inputs |= 1u << d->idx;
    } else if (d->rel)
        sb_put(b, "c[cix(%u)]", d->c);
    else
        sb_put(b, "c[%u]", d->c < NV2A_VSH_CONSTANTS ? d->c : 0);
    if (!d->ident)
        sb_put(b, ".%c%c%c%c", sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
}

static void hlsl_mask(Sb *b, const char *dst, uint32_t mask, const char *src)
{
    char m[5];
    int k = 0;
    if (mask & 8) m[k++] = 'x';
    if (mask & 4) m[k++] = 'y';
    if (mask & 2) m[k++] = 'z';
    if (mask & 1) m[k++] = 'w';
    m[k] = 0;
    if (k)
        sb_put(b, "  %s.%s = %s.%s;\n", dst, m, src, m);
}

int nv2a_vsh_hlsl(char *buf, size_t cap, uint32_t *inputs_read)
{
    Sb b = { buf, 0, cap };
    uint32_t i, inputs = 0;
    size_t vin_at;
    char vin[1024];

    if (!nv2a_vsh_program_key())
        return 0;
    sb_put(&b,
        "cbuffer CB : register(b0) { float4 vp; };\n"
        "cbuffer VC : register(b1) { float4 c[192]; };\n"
        "cbuffer VP : register(b2) { float4 xf; float4 fogp; };\n"
        "static const float INF = asfloat(0x7F800000u);\n"
        "uint cix(int k) { return (uint)k < 192u ? (uint)k : 0u; }\n"
        "float fogf(float d) {\n"
        "  if (fogp.z == 0.0) return 1.0;\n"
        "  uint m = (uint)fogp.w; float f;\n"
        "  if (m == 0x800u || m == 0x802u) f = fogp.x + exp2(d * fogp.y * 16.0) - 1.5;\n"
        "  else if (m == 0x801u || m == 0x803u) f = fogp.x + exp2(-d * d * fogp.y * fogp.y * 32.0) - 1.5;\n"
        "  else f = fogp.x + d * fogp.y - 1.0;\n"
        "  if (m == 0x802u || m == 0x803u || m == 0x804u) f = abs(f);\n"
        "  if ((asuint(f) & 0x7FFFFFFFu) > 0x7F800000u) f = 1.0;\n"
        "  return saturate(f);\n"
        "}\n");
    vin_at = b.n;
    sb_put(&b,
        "struct VSOut { float4 pos : SV_Position; noperspective float4 d0 : COLOR0;\n"
        "  noperspective float4 d1 : COLOR1; noperspective float4 fog : TEXCOORD4;\n"
        "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n"
        "  float4 t3 : TEXCOORD3; float2 clip : SV_ClipDistance0; };\n"
        "VSOut vsmain(VSIn i) {\n"
        "  float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0;\n"
        "  float4 r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0;\n"
        "  float4 o1 = 0, o2 = 0, o3 = float4(0, 0, 0, 1), o4 = float4(0, 0, 0, 1);\n"
        "  float4 o5 = 0, o6 = 0, o7 = 0, o8 = 0;\n"
        "  float4 o9 = float4(0, 0, 0, 1), o10 = float4(0, 0, 0, 1);\n"
        "  float4 o11 = float4(0, 0, 0, 1), o12 = float4(0, 0, 0, 1);\n"
        "  int a0 = 0; float4 A, B, C, M, I;\n");
    for (i = 0; i < s_dec_count; i++) {
        const DecIns *d = &s_dec[i];
        char dst[8];
        sb_put(&b, "  // %u\n", i);
        sb_put(&b, "  A = "); if (d->reads & 1) hlsl_src(&b, &d->src[0], &inputs); else sb_put(&b, "0");
        sb_put(&b, "; B = "); if (d->reads & 2) hlsl_src(&b, &d->src[1], &inputs); else sb_put(&b, "0");
        sb_put(&b, "; C = "); if (d->reads & 4) hlsl_src(&b, &d->src[2], &inputs); else sb_put(&b, "0");
        sb_put(&b, ";\n");
        switch (d->mac) {
        case 0:  sb_put(&b, "  M = 0;\n"); break;
        case 2:  sb_put(&b, "  M = A * B;\n"); break;
        case 3:  sb_put(&b, "  M = A + C;\n"); break;
        case 4:  sb_put(&b, "  M = A * B + C;\n"); break;
        case 5:  sb_put(&b, "  M = (A.x * B.x + A.y * B.y + A.z * B.z).xxxx;\n"); break;
        case 6:  sb_put(&b, "  M = (A.x * B.x + A.y * B.y + A.z * B.z + B.w).xxxx;\n"); break;
        case 7:  sb_put(&b, "  M = (A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w).xxxx;\n"); break;
        case 8:  sb_put(&b, "  M = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  sb_put(&b, "  M = A < B ? A : B;\n"); break;
        case 10: sb_put(&b, "  M = A >= B ? A : B;\n"); break;
        case 11: sb_put(&b, "  M = A < B ? 1.0 : 0.0;\n"); break;
        case 12: sb_put(&b, "  M = A >= B ? 1.0 : 0.0;\n"); break;
        case 13: sb_put(&b, "  M = A; a0 = (int)floor(A.x + 0.001);\n"); break;
        default: sb_put(&b, "  M = A;\n"); break;
        }
        switch (d->ilu) {
        case 0: sb_put(&b, "  I = 0;\n"); break;
        case 2: sb_put(&b, "  I = (C.x == 0.0 ? INF : 1.0 / C.x).xxxx;\n"); break;
        case 3: sb_put(&b, "  { float f = 1.0 / C.x, g = clamp(abs(f), 5.42101e-20, 1.884467e19);"
                           " I = (f < 0.0 ? -g : g).xxxx; }\n"); break;
        case 4: sb_put(&b, "  I = (1.0 / sqrt(abs(C.x))).xxxx;\n"); break;
        case 5: sb_put(&b, "  { float f = floor(C.x); I = float4(exp2(f), C.x - f, exp2(C.x), 1.0); }\n"); break;
        case 6: sb_put(&b, "  { float ax = abs(C.x);"
                           " if (ax == 0.0) I = float4(-INF, 1.0, -INF, 1.0);"
                           " else { float e = floor(log2(ax)); I = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"); break;
        case 7: sb_put(&b, "  { float nh = max(C.y, 0.0), p = clamp(C.w, -127.9961, 127.9961);"
                           " float s = C.x > 0.0 ? (nh > 0.0 ? pow(nh, p) : (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : INF))) : 0.0;"
                           " I = float4(1.0, max(C.x, 0.0), s, 1.0); }\n"); break;
        default: sb_put(&b, "  I = C;\n"); break;
        }
        if (d->mac_mask && d->mac_dst != DEC_NO_DST) {
            snprintf(dst, sizeof dst, "r%u", d->mac_dst);
            hlsl_mask(&b, dst, d->mac_mask, "M");
        }
        if (d->ilu_mask && d->ilu_dst != DEC_NO_DST) {
            snprintf(dst, sizeof dst, "r%u", d->ilu_dst);
            hlsl_mask(&b, dst, d->ilu_mask, "I");
        }
        if (d->omask && d->to_o && d->oaddr < 13) {
            if (d->oaddr == 0)
                snprintf(dst, sizeof dst, "r12");
            else
                snprintf(dst, sizeof dst, "o%u", d->oaddr);
            hlsl_mask(&b, dst, d->omask, d->from_ilu ? "I" : "M");
        }
    }

    sb_put(&b,
        "  VSOut o; float4 P = r12;\n"
        "  if (asuint(P.w) == 0u) P.w = 1.0;\n"
        "  P.x *= xf.x; P.y *= xf.y;\n"
        "  float w = P.w;\n"
        "  o.pos = float4((P.x * vp.x - 1.0) * w, (1.0 - P.y * vp.y) * w, P.z * vp.z * w, w);\n"
        "  o.clip = float2(w - 1e-3, xf.w != 0.0 ? (P.z - xf.z) * w : 1.0);\n"
        "  o.d0 = saturate(o3); o.d1 = saturate(o4);\n"
        "  o.fog = float4(fogf(o5.x), 0, 0, 0);\n"
        "  o.t0 = o9; o.t1 = o10; o.t2 = o11; o.t3 = o12;\n"
        "  return o;\n"
        "}\n");
    if (b.n >= b.cap)
        return 0;

    {
        size_t k = 0, tail = b.n - vin_at;
        int any = 0;
        k += (size_t)snprintf(vin + k, sizeof vin - k, "struct VSIn {");
        for (i = 0; i < NV2A_VSH_INPUTS; i++)
            if (inputs & (1u << i)) {
                k += (size_t)snprintf(vin + k, sizeof vin - k, " float4 v%u : ATTR%u;", i, i);
                any = 1;
            }
        if (!any)
            k += (size_t)snprintf(vin + k, sizeof vin - k, " uint vid : SV_VertexID;");
        k += (size_t)snprintf(vin + k, sizeof vin - k, " };\n");
        if (b.n + k >= b.cap)
            return 0;
        memmove(buf + vin_at + k, buf + vin_at, tail);
        memcpy(buf + vin_at, vin, k);
        b.n += k;
        buf[b.n] = 0;
    }
    if (inputs_read)
        *inputs_read = inputs;
    return 1;
}

