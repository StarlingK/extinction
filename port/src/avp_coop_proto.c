#include "avp_coop_proto.h"
#include <math.h>
#include <string.h>

typedef struct { uint16_t off; uint8_t type; uint32_t mask; } CpField;

enum { FT_POS, FT_ROT, FT_FLT, FT_INT, FT_HND, FT_FLAG, FT_DYING };

#define FULL 0xFFFFFFFFu

static const CpField k_avp_f[] = {
    { 0x040, FT_POS, FULL }, { 0x044, FT_POS, FULL }, { 0x048, FT_POS, FULL },
    { 0x04C, FT_ROT, FULL }, { 0x050, FT_ROT, FULL }, { 0x054, FT_ROT, FULL },
    { 0x064, FT_FLAG, 0x00FFFFFFu },
    { 0x0DC, FT_HND, FULL },
    { 0x0E0, FT_FLT, FULL }, { 0x0E4, FT_FLT, FULL }, { 0x0E8, FT_FLT, FULL },
    { 0x0FC, FT_FLT, FULL }, { 0x100, FT_FLT, FULL }, { 0x104, FT_FLT, FULL },
    { 0x110, FT_HND, FULL },
    { 0x248, FT_HND, FULL }, { 0x24C, FT_HND, FULL }, { 0x250, FT_HND, FULL }, { 0x254, FT_HND, FULL },
    { 0x258, FT_HND, FULL }, { 0x25C, FT_HND, FULL }, { 0x260, FT_HND, FULL }, { 0x264, FT_HND, FULL },
    { 0x268, FT_FLAG, 0xFFFF00FFu },
    { 0x26C, FT_FLAG, 0xFF0000FFu },
    { 0x26C, FT_DYING, 0x0000FF00u },
    { 0x270, FT_FLAG, 0x000000FFu },
    { 0x27C, FT_FLT, FULL }, { 0x280, FT_INT, FULL },
};

static const CpField k_unit_f[] = {
    { 0x474, FT_INT, FULL },
    { 0x4A0, FT_HND, FULL },
    { 0x4A4, FT_FLT, FULL }, { 0x4A8, FT_FLT, FULL }, { 0x4AC, FT_FLT, FULL },
    { 0x4B0, FT_INT, FULL }, { 0x4B4, FT_INT, FULL }, { 0x4B8, FT_FLT, FULL },
    { 0x4E8, FT_INT, FULL }, { 0x4EC, FT_FLAG, 0x000000FFu }, { 0x4F0, FT_INT, FULL }, { 0x4F4, FT_INT, FULL },
    { 0x528, FT_HND, FULL },
    { 0x538, FT_FLT, FULL }, { 0x53C, FT_FLT, FULL },
    { 0x558, FT_FLT, FULL },
    { 0x5C4, FT_FLT, FULL },
    { 0x5DC, FT_FLAG, 0x000000FFu },
    { 0x5E0, FT_HND, FULL }, { 0x5E8, FT_HND, FULL },
    { 0x670, FT_INT, FULL }, { 0x674, FT_FLT, FULL },
};

static const CpField k_trig_f[] = {
    { 0x15C, FT_FLAG, 0x000000FFu },
    { 0x170, FT_FLAG, 0x00FF00FFu },
    { 0x174, FT_INT, FULL },
    { 0x178, FT_FLAG, 0x000000FFu },
    { 0x194, FT_FLAG, 0x000000FFu },
    { 0x1B0, FT_FLAG, 0x0000FF00u },
};

static const CpField k_pm_f[] = {
    { 0x104, FT_FLT, FULL }, { 0x190, FT_FLT, FULL }, { 0x3D4, FT_INT, FULL }, { 0x3D8, FT_FLT, FULL },
    { 0x500, FT_FLT, FULL }, { 0x508, FT_INT, FULL }, { 0x50C, FT_INT, FULL }, { 0x510, FT_INT, FULL },
};

#define N_AVP  (sizeof k_avp_f / sizeof k_avp_f[0])
#define N_UNIT (sizeof k_unit_f / sizeof k_unit_f[0])
#define N_TRIG (sizeof k_trig_f / sizeof k_trig_f[0])
#define N_PM   (sizeof k_pm_f / sizeof k_pm_f[0])

static CpField s_unit_all[N_AVP + N_UNIT];
static int s_tables_ready;

static void tables_init(void)
{
    if (s_tables_ready) return;
    memcpy(s_unit_all, k_avp_f, sizeof k_avp_f);
    memcpy(s_unit_all + N_AVP, k_unit_f, sizeof k_unit_f);
    s_tables_ready = 1;
}

static const CpField *fields_of(uint32_t fl, uint32_t *n)
{
    tables_init();
    if (fl & CF_UNIT) { *n = (uint32_t)(N_AVP + N_UNIT); return s_unit_all; }
    if (fl & CF_AVP) { *n = (uint32_t)N_AVP; return k_avp_f; }
    if (fl & CF_TRIG) { *n = (uint32_t)N_TRIG; return k_trig_f; }
    *n = 0;
    return NULL;
}

static float u2f(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static uint32_t f2u(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static int finite_f(float f)
{
    return f == f && f - f == 0.0f;
}

static int range_f(float f, float lim)
{
    return finite_f(f) && f > -lim && f < lim;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static uint32_t popc(uint32_t v)
{
    uint32_t n = 0;
    while (v) { v &= v - 1u; n++; }
    return n;
}

static int sim_ptr(uint32_t w)
{
    return w >= CP_LOCAL_LO && w < CP_LOCAL_HI;
}

static int td_is(uint32_t td, const char *s)
{
    uint32_t i;
    if (td < 0x10000u || td >= CP_LOCAL_HI - 0x100u) return 0;
    for (i = 0; s[i]; i++)
        if (G8(td + 8u + i) != (uint8_t)s[i]) return 0;
    return G8(td + 8u + i) == 0;
}

static uint32_t rtti_flags(uint32_t vt)
{
    uint32_t col, chd, bca, n, i, fl = 0;
    if (vt < 0x10020u || vt >= 0x0031B000u || (vt & 3u)) return 0;
    col = G32(vt - 4u);
    if (col < 0x10000u || col >= CP_LOCAL_HI - 0x100u || (col & 3u)) return 0;
    chd = G32(col + 16u);
    if (chd < 0x10000u || chd >= CP_LOCAL_HI - 0x100u || (chd & 3u)) return 0;
    n = G32(chd + 8u);
    bca = G32(chd + 12u);
    if (n == 0 || n > 64u || bca < 0x10000u || bca >= CP_LOCAL_HI - 0x400u || (bca & 3u)) return 0;
    for (i = 0; i < n; i++) {
        uint32_t bcd = G32(bca + i * 4u), td;
        if (bcd < 0x10000u || bcd >= CP_LOCAL_HI - 0x100u) continue;
        td = G32(bcd);
        if (td_is(td, ".?AVGAvPObject@@")) fl |= CF_AVP;
        else if (td_is(td, ".?AVGUnit@@")) fl |= CF_UNIT;
        else if (td_is(td, ".?AVGPlayerManager@@")) fl |= CF_PM;
        else if (td_is(td, ".?AVGTrigger@@")) fl |= CF_TRIG;
        else if (td_is(td, ".?AVGSquad@@")) fl |= CF_SQUAD;
    }
    return fl;
}

#define CLS_HASH 2048u

static struct { uint32_t vt, fl; } s_cls[CLS_HASH];

static uint32_t cls_peek(uint32_t vt)
{
    uint32_t h, i;
    if (!vt) return 0;
    h = ((vt >> 2) * 2654435761u) >> 21;
    for (i = 0; i < 64u; i++) {
        uint32_t k = (h + i) & (CLS_HASH - 1u);
        if (s_cls[k].vt == vt) return s_cls[k].fl;
        if (!s_cls[k].vt) break;
    }
    return 0;
}

uint32_t cp_class(uint32_t vt)
{
    uint32_t h, i, fl;
    if (vt < 0x10020u || vt >= 0x0031B000u || (vt & 3u)) return 0;
    h = ((vt >> 2) * 2654435761u) >> 21;
    for (i = 0; i < 64u; i++) {
        uint32_t k = (h + i) & (CLS_HASH - 1u);
        if (s_cls[k].vt == vt) return s_cls[k].fl;
        if (!s_cls[k].vt) {
            fl = rtti_flags(vt);
            s_cls[k].vt = vt;
            s_cls[k].fl = fl;
            return fl;
        }
    }
    return rtti_flags(vt);
}

int cp_obj_ok(uint32_t obj)
{
    return obj >= 0x10000u && !(obj & 3u) && obj < CP_LOCAL_HI - 0xD80u;
}

uint32_t cp_obj_of(uint32_t handle)
{
    uint32_t obj;
    if (!(handle >> 16)) return 0;
    obj = G32(CP_OBJ_TABLE + (handle >> 16) * 4u);
    if (!cp_obj_ok(obj) || G32(obj + 0x18u) != handle) return 0;
    return obj;
}

uint32_t cp_pm_object(uint32_t slot)
{
    uint32_t world = G32(CP_WORLD), pm;
    if (world < 0x10000u || world >= CP_LOCAL_HI - 0x100u || slot >= CP_PM_SLOTS) return 0;
    pm = G32(world + CP_PM_ARRAY + slot * 4u);
    if (pm < 0x10000u || (pm & 3u) || pm >= CP_LOCAL_HI - 0x520u || !(cp_class(G32(pm)) & CF_PM)) return 0;
    return pm;
}

int cp_target_ok(uint32_t handle)
{
    uint32_t obj = cp_obj_of(handle);
    return obj && (cp_class(G32(obj)) & CF_AVP);
}

int cp_unit_ok(uint32_t handle)
{
    uint32_t obj = cp_obj_of(handle);
    return obj && (cp_class(G32(obj)) & CF_UNIT);
}

typedef struct { uint32_t handle, flags; uint32_t w[CP_FIELDS_MAX]; } CpCache;

#define CP_SLOTS 6144u

static CpCache s_cache[CP_SLOTS];
static uint16_t s_slot_of[0x10000];
static uint16_t s_free[CP_SLOTS];
static uint32_t s_nfree, s_cache_ready;
static uint64_t s_tech_hash;

static uint32_t s_h2c_h[0x10000], s_h2c_c[0x10000], s_c2h_c[0x10000], s_c2h_h[0x10000];
static uint32_t s_claim_ep[0x10000], s_claim_h[0x10000], s_epoch;
static uint8_t s_orphan_n[0x10000];
static uint32_t s_orphan_h[0x10000];

#define SM_MAX 4096u

typedef struct { uint32_t h, t_ms; float p[3], r[3]; uint8_t has_p, has_r; } CpSm;

static CpSm s_sm[SM_MAX];
static uint16_t s_sm_at[0x10000];
static uint32_t s_sm_n;

static void cache_init(void)
{
    uint32_t i;
    if (s_cache_ready) return;
    for (i = 0; i < CP_SLOTS; i++) s_free[i] = (uint16_t)(CP_SLOTS - 1u - i);
    s_nfree = CP_SLOTS;
    memset(s_slot_of, 0, sizeof s_slot_of);
    s_cache_ready = 1;
}

static void slot_release(uint32_t idx)
{
    uint32_t s = s_slot_of[idx];
    if (!s) return;
    s_slot_of[idx] = 0;
    s_cache[s - 1u].handle = 0;
    s_free[s_nfree++] = (uint16_t)(s - 1u);
}

static CpCache *slot_get(uint32_t idx)
{
    uint32_t s = s_slot_of[idx];
    if (!s) {
        if (!s_nfree) return NULL;
        s = (uint32_t)s_free[--s_nfree] + 1u;
        s_slot_of[idx] = (uint16_t)s;
        memset(&s_cache[s - 1u], 0, sizeof s_cache[0]);
    }
    return &s_cache[s - 1u];
}

static void sm_clear(void)
{
    uint32_t i;
    for (i = 0; i < s_sm_n; i++) s_sm_at[s_sm[i].h >> 16] = 0;
    s_sm_n = 0;
}

void cp_reset(void)
{
    uint32_t i;
    cache_init();
    for (i = 0; i < 0x10000u; i++) slot_release(i);
    memset(s_h2c_h, 0, sizeof s_h2c_h);
    memset(s_h2c_c, 0, sizeof s_h2c_c);
    memset(s_c2h_c, 0, sizeof s_c2h_c);
    memset(s_c2h_h, 0, sizeof s_c2h_h);
    memset(s_claim_ep, 0, sizeof s_claim_ep);
    memset(s_claim_h, 0, sizeof s_claim_h);
    memset(s_orphan_n, 0, sizeof s_orphan_n);
    memset(s_orphan_h, 0, sizeof s_orphan_h);
    s_epoch = 0;
    s_tech_hash = 0;
    sm_clear();
}

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t tech_rows(uint32_t *out_tt)
{
    uint32_t tt = G32(CP_TECH_TREE), row, n = 0;
    *out_tt = 0;
    if (tt < 0x10000u || tt >= CP_LOCAL_HI - 0x1000u) return 0;
    for (row = tt + 0x38; n < CP_TECH_MAX && G32(row); row += 8, n++) ;
    *out_tt = tt;
    return n;
}

static void put(uint8_t *buf, uint32_t *o, uint32_t v)
{
    wr32(buf + *o, v);
    *o += 4u;
}

uint32_t cp_state_build(uint8_t *buf, uint32_t cap, uint32_t seq, int full, uint32_t host_ms, const CpHooks *hk,
                        CpBuildStats *bs)
{
    uint32_t o = CP_HDR, i, k, npm = 0, ntech = 0, nobj = 0, tt, flags = full ? CP_FLAG_FULL : 0, words = 0, mmflag = 0;
    CpBuildStats z;
    if (!bs) bs = &z;
    memset(bs, 0, sizeof *bs);
    cache_init();
    tables_init();
    if (cap < 8192u) return 0;
    for (i = 0; i < CP_PM_SLOTS; i++) {
        uint32_t pm = cp_pm_object(i);
        if (!pm) continue;
        put(buf, &o, i);
        for (k = 0; k < N_PM; k++) put(buf, &o, G32(pm + k_pm_f[k].off));
        npm++;
    }
    {
        uint32_t n = tech_rows(&tt), r;
        uint64_t th = 0xCBF29CE484222325ull;
        for (r = 0; r < n; r++) {
            uint32_t rw[2];
            rw[0] = G32(tt + 0x38 + r * 8u);
            rw[1] = G32(tt + 0x3C + r * 8u);
            th = fnv64(th, rw, sizeof rw);
        }
        if (n && (full || th != s_tech_hash)) {
            for (r = 0; r < n; r++) {
                put(buf, &o, G32(tt + 0x38 + r * 8u));
                put(buf, &o, G32(tt + 0x3C + r * 8u));
            }
            ntech = n;
            s_tech_hash = th;
        }
    }
    {
        uint32_t mm = G32(CP_MINIMAP);
        if (cp_obj_ok(mm)) {
            put(buf, &o, G32(mm + 0x1020));
            put(buf, &o, G32(mm + 0x1024));
            for (k = 0; k < 32u; k++) put(buf, &o, G32(mm + 0x1028 + 4 * k));
            mmflag = 1;
        }
    }
    for (i = 1; i < 0x10000u; i++) {
        uint32_t obj = G32(CP_OBJ_TABLE + i * 4u), vt, fl, nf, h, w[CP_FIELDS_MAX], lo = 0, hi = 0, pc = 0, oflag = 0;
        const CpField *f;
        CpCache *c;
        int same_handle;
        if (!cp_obj_ok(obj)) {
            slot_release(i);
            continue;
        }
        vt = G32(obj);
        fl = cp_class(vt);
        f = fields_of(fl, &nf);
        if (!f) {
            slot_release(i);
            continue;
        }
        h = G32(obj + 0x18u);
        if ((h >> 16) != i) {
            slot_release(i);
            continue;
        }
        nobj++;
        c = slot_get(i);
        same_handle = c && c->handle == h;
        if (hk && hk->owner && (fl & CF_UNIT) && hk->owner(h) == 1) oflag = 1;
        for (k = 0; k < nf; k++) {
            int ch;
            w[k] = G32(obj + f[k].off) & f[k].mask;
            ch = full || !same_handle || w[k] != c->w[k];
            if (ch) {
                if (k < 32u) lo |= 1u << k;
                else hi |= 1u << (k - 32u);
                pc++;
            }
        }
        if (!pc && same_handle && c->flags == oflag) continue;
        if ((uint64_t)o + 20u + 4u * pc > cap || o + 20u + 4u * pc > CP_STATE_MAX - 4096u) {
            flags |= CP_FLAG_TRUNC;
            bs->truncated = 1;
            break;
        }
        put(buf, &o, h);
        put(buf, &o, vt);
        put(buf, &o, oflag);
        put(buf, &o, lo);
        put(buf, &o, hi);
        for (k = 0; k < nf; k++) {
            int bit = k < 32u ? (int)((lo >> k) & 1u) : (int)((hi >> (k - 32u)) & 1u);
            if (bit) put(buf, &o, w[k]);
        }
        words += pc;
        bs->records++;
        if (c) {
            c->handle = h;
            c->flags = oflag;
            memcpy(c->w, w, nf * 4u);
        }
    }
    wr32(buf + 0, CP_STATE_MAGIC);
    wr32(buf + 4, seq);
    wr32(buf + 8, flags);
    wr32(buf + 12, bs->records);
    wr32(buf + 16, npm);
    wr32(buf + 20, ntech);
    wr32(buf + 24, G32(CP_CLOCK));
    wr32(buf + 28, G32(CP_RNG_ACC));
    wr32(buf + 32, G32(CP_RNG_SEED));
    wr32(buf + 36, host_ms);
    wr32(buf + 40, mmflag);
    wr32(buf + 44, 0);
    bs->objects = nobj;
    bs->words = words;
    bs->pm = npm;
    bs->tech = ntech;
    return o;
}

int cp_state_validate(const uint8_t *p, uint32_t len)
{
    uint32_t nobj, npm, ntech, o, i;
    uint64_t need;
    if (!p || len < CP_HDR || len > CP_STATE_MAX) return 0;
    if (rd32(p) != CP_STATE_MAGIC) return 0;
    nobj = rd32(p + 12);
    npm = rd32(p + 16);
    ntech = rd32(p + 20);
    if (nobj > CP_OBJ_MAX || npm > CP_PM_SLOTS || ntech > CP_TECH_MAX) return 0;
    need = (uint64_t)CP_HDR + (uint64_t)npm * (4u + 4u * N_PM) + (uint64_t)ntech * 8u + (rd32(p + 40) == 1u ? 136u : 0u);
    if (need > len) return 0;
    o = (uint32_t)need;
    for (i = 0; i < nobj; i++) {
        uint32_t h, pc;
        uint64_t end;
        if ((uint64_t)o + 20u > len) return 0;
        h = rd32(p + o);
        pc = popc(rd32(p + o + 12)) + popc(rd32(p + o + 16));
        if (!(h >> 16)) return 0;
        end = (uint64_t)o + 20u + 4ull * pc;
        if (end > len) return 0;
        o = (uint32_t)end;
    }
    return o == len;
}

uint32_t cp_state_clock(const uint8_t *p, uint32_t len)
{
    return len >= CP_HDR ? rd32(p + 24) : 0;
}

static void map_clear_h(uint32_t hidx)
{
    uint32_t ch = s_h2c_c[hidx];
    if (ch && s_c2h_c[ch >> 16] == ch && s_c2h_h[ch >> 16] == s_h2c_h[hidx]) {
        s_c2h_c[ch >> 16] = 0;
        s_c2h_h[ch >> 16] = 0;
    }
    s_h2c_h[hidx] = 0;
    s_h2c_c[hidx] = 0;
}

static void map_set(uint32_t hh, uint32_t ch)
{
    uint32_t hidx = hh >> 16, cidx = ch >> 16;
    if (s_h2c_h[hidx] && s_h2c_c[hidx] != ch) map_clear_h(hidx);
    if (s_c2h_c[cidx] && s_c2h_h[cidx] != hh) {
        uint32_t oh = s_c2h_h[cidx];
        if (s_h2c_h[oh >> 16] == oh) {
            s_h2c_h[oh >> 16] = 0;
            s_h2c_c[oh >> 16] = 0;
        }
    }
    s_h2c_h[hidx] = hh;
    s_h2c_c[hidx] = ch;
    s_c2h_c[cidx] = ch;
    s_c2h_h[cidx] = hh;
}

uint32_t cp_map_to_client(uint32_t hh)
{
    uint32_t idx = hh >> 16, ch;
    if (!idx) return 0;
    if (s_h2c_h[idx] != hh) return 0;
    ch = s_h2c_c[idx];
    if (!cp_obj_of(ch)) return 0;
    return ch;
}

uint32_t cp_map_to_host(uint32_t ch)
{
    uint32_t idx = ch >> 16;
    if (!idx) return 0;
    if (s_c2h_c[idx] != ch || !cp_obj_of(ch)) return 0;
    return s_c2h_h[idx];
}

static int claim_ok(uint32_t cidx, uint32_t hh)
{
    return s_claim_ep[cidx] != s_epoch || s_claim_h[cidx] == hh;
}

static void claim(uint32_t cidx, uint32_t hh)
{
    s_claim_ep[cidx] = s_epoch;
    s_claim_h[cidx] = hh;
}

static int cmapped(uint32_t obj)
{
    uint32_t h = G32(obj + 0x18u), idx = h >> 16;
    return s_c2h_c[idx] == h && s_c2h_h[idx] != 0;
}

static uint32_t resolve_exact(uint32_t hh, uint32_t vt)
{
    uint32_t idx = hh >> 16, ch, obj;
    if (s_h2c_h[idx] == hh) {
        ch = s_h2c_c[idx];
        obj = cp_obj_of(ch);
        if (obj && G32(obj) == vt && claim_ok(ch >> 16, hh)) return obj;
    }
    obj = cp_obj_of(hh);
    if (obj && G32(obj) == vt && claim_ok(idx, hh) && (!cmapped(obj) || s_c2h_h[idx] == hh)) {
        map_set(hh, hh);
        return obj;
    }
    return 0;
}

static void sm_remove(uint32_t idx)
{
    uint32_t s = s_sm_at[idx], last;
    if (!s) return;
    s--;
    s_sm_at[idx] = 0;
    last = s_sm_n - 1u;
    if (s != last) {
        s_sm[s] = s_sm[last];
        s_sm_at[s_sm[s].h >> 16] = (uint16_t)(s + 1u);
    }
    s_sm_n--;
}

static CpSm *sm_get(uint32_t idx, uint32_t h)
{
    uint32_t s = s_sm_at[idx];
    if (s && s_sm[s - 1u].h == h) return &s_sm[s - 1u];
    if (s) sm_remove(idx);
    if (s_sm_n >= SM_MAX) return NULL;
    s_sm[s_sm_n].h = h;
    s_sm[s_sm_n].has_p = s_sm[s_sm_n].has_r = 0;
    s_sm_at[idx] = (uint16_t)(s_sm_n + 1u);
    return &s_sm[s_sm_n++];
}

static float wrap_pi(float d)
{
    const float tp = 6.2831853f;
    int guard = 0;
    while (d > 3.1415927f && guard++ < 8) d -= tp;
    while (d < -3.1415927f && guard++ < 16) d += tp;
    return d;
}

static void put_pos(uint32_t obj, const float *v, const CpHooks *hk)
{
    uint32_t a;
    if (hk && hk->setpos) {
        hk->setpos(obj, v);
        return;
    }
    for (a = 0; a < 3; a++) GF(obj + 0x40u + a * 4u) = v[a];
}

static void put_rot(uint32_t obj, const float *v, const CpHooks *hk)
{
    uint32_t a;
    if (hk && hk->setrot) {
        hk->setrot(obj, v);
        return;
    }
    for (a = 0; a < 3; a++) GF(obj + 0x4Cu + a * 4u) = v[a];
}

void cp_smooth_step(uint32_t now_ms, uint32_t dt_ms, const CpHooks *hk)
{
    uint32_t i = 0;
    float k;
    if (!s_sm_n) return;
    if (dt_ms < 1u) dt_ms = 1u;
    if (dt_ms > 100u) dt_ms = 100u;
    k = (float)dt_ms / 300.0f;
    if (k > 1.0f) k = 1.0f;
    while (i < s_sm_n) {
        CpSm *s = &s_sm[i];
        uint32_t obj = cp_obj_of(s->h);
        if (!obj || now_ms - s->t_ms > 700u) {
            sm_remove(s->h >> 16);
            continue;
        }
        if (s->has_p) {
            uint32_t a;
            float np[3];
            for (a = 0; a < 3; a++) {
                float cur = GF(obj + 0x40u + a * 4u);
                if (!finite_f(cur)) cur = s->p[a];
                np[a] = cur + (s->p[a] - cur) * k;
            }
            put_pos(obj, np, hk);
        }
        if (s->has_r) {
            uint32_t a;
            float nr[3];
            for (a = 0; a < 3; a++) {
                float cur = GF(obj + 0x4Cu + a * 4u);
                if (!finite_f(cur)) cur = s->r[a];
                nr[a] = cur + wrap_pi(s->r[a] - cur) * k;
            }
            put_rot(obj, nr, hk);
        }
        i++;
    }
}

typedef struct { uint32_t obj, off, hh; } PendH;

#define PEND_MAX 8192u

static PendH s_pend[PEND_MAX];
static uint32_t s_pend_n;

static void apply_obj(uint32_t obj, uint32_t vt, uint32_t rflags, const uint8_t *wp, uint32_t mlo, uint32_t mhi,
                      uint32_t now_ms, const CpHooks *hk)
{
    uint32_t fl = cp_class(vt), nf, k, wi = 0, got_p = 0, got_r = 0, h = G32(obj + 0x18u);
    const CpField *f = fields_of(fl, &nf);
    float pv[3] = { 0, 0, 0 }, rv[3] = { 0, 0, 0 };
    if (!f) return;
    for (k = 0; k < 64u; k++) {
        uint32_t bit = k < 32u ? (mlo >> k) & 1u : (mhi >> (k - 32u)) & 1u, v, cur;
        if (!bit) continue;
        v = rd32(wp + 4u * wi++);
        if (k >= nf) continue;
        cur = G32(obj + f[k].off);
        switch (f[k].type) {
        case FT_POS:
            if (range_f(u2f(v), 1.0e6f)) {
                pv[(f[k].off - 0x40u) / 4u] = u2f(v);
                got_p |= 1u << ((f[k].off - 0x40u) / 4u);
            }
            break;
        case FT_ROT:
            if (range_f(u2f(v), 1.0e4f)) {
                rv[(f[k].off - 0x4Cu) / 4u] = u2f(v);
                got_r |= 1u << ((f[k].off - 0x4Cu) / 4u);
            }
            break;
        case FT_FLT:
            if (finite_f(u2f(v))) G32(obj + f[k].off) = v;
            break;
        case FT_INT:
            if (!sim_ptr(v)) G32(obj + f[k].off) = v;
            break;
        case FT_HND:
            if (!v) G32(obj + f[k].off) = 0;
            else if (s_pend_n < PEND_MAX) {
                s_pend[s_pend_n].obj = obj;
                s_pend[s_pend_n].off = f[k].off;
                s_pend[s_pend_n].hh = v;
                s_pend_n++;
            }
            break;
        case FT_FLAG:
            G32(obj + f[k].off) = (cur & ~f[k].mask) | (v & f[k].mask);
            break;
        case FT_DYING:
            if (((v & f[k].mask) != 0) && !G8(obj + 0x26Du) && hk && hk->kill) hk->kill(h);
            break;
        }
    }
    if ((fl & CF_UNIT) && hk && hk->owner_set) hk->owner_set(h, (int)(rflags & 1u));
    if ((fl & CF_AVP) && (got_p || got_r)) {
        uint32_t idx = h >> 16, a;
        float cp[3], err2 = 0.0f;
        int snap;
        for (a = 0; a < 3; a++) {
            cp[a] = GF(obj + 0x40u + a * 4u);
            if (!((got_p >> a) & 1u)) pv[a] = cp[a];
            if (finite_f(cp[a])) {
                float d = pv[a] - cp[a];
                err2 += d * d;
            } else {
                err2 += 1.0e9f;
            }
        }
        snap = err2 > CP_SNAP_DIST * CP_SNAP_DIST;
        if (got_p && snap) put_pos(obj, pv, hk);
        if (got_r && snap) {
            for (a = 0; a < 3; a++) if (!((got_r >> a) & 1u)) rv[a] = GF(obj + 0x4Cu + a * 4u);
            put_rot(obj, rv, hk);
        }
        if (!snap && err2 <= 1.5f * 1.5f) {
            sm_remove(idx);
        } else if (!snap) {
            CpSm *s = sm_get(idx, h);
            if (s) {
                if (got_p) {
                    for (a = 0; a < 3; a++) s->p[a] = pv[a];
                    s->has_p = 1;
                }
                if (got_r) {
                    for (a = 0; a < 3; a++)
                        s->r[a] = ((got_r >> a) & 1u) ? rv[a] : GF(obj + 0x4Cu + a * 4u);
                    s->has_r = 1;
                }
                s->t_ms = now_ms;
            }
        } else {
            sm_remove(idx);
        }
    }
}

typedef struct { uint32_t idx, obj; float p[3]; } Cand;

#define UNM_MAX  1024u
#define CAND_MAX 4096u

static uint32_t s_unm[UNM_MAX];
static Cand s_cand[CAND_MAX];

static int rec_pos(uint32_t vt, uint32_t mlo, uint32_t mhi, const uint8_t *wp, float *pos)
{
    uint32_t fl = cls_peek(vt), nf, k, wi = 0, got = 0;
    const CpField *f = fields_of(fl, &nf);
    if (!f || !(fl & CF_AVP)) return 0;
    for (k = 0; k < 64u; k++) {
        uint32_t bit = k < 32u ? (mlo >> k) & 1u : (mhi >> (k - 32u)) & 1u;
        if (!bit) continue;
        if (k < nf && f[k].type == FT_POS) {
            float v = u2f(rd32(wp + 4u * wi));
            if (range_f(v, 1.0e6f)) {
                pos[(f[k].off - 0x40u) / 4u] = v;
                got |= 1u << ((f[k].off - 0x40u) / 4u);
            }
        }
        wi++;
    }
    return got == 7u;
}

static void apply_globals(const uint8_t *p, uint32_t npm, uint32_t ntech, uint32_t mmflag, uint32_t *o, CpApplyStats *st)
{
    uint32_t i, k;
    for (i = 0; i < npm; i++) {
        uint32_t slot = rd32(p + *o), pm;
        const uint8_t *w = p + *o + 4u;
        *o += 4u + 4u * (uint32_t)N_PM;
        pm = cp_pm_object(slot);
        if (!pm) continue;
        for (k = 0; k < N_PM; k++) {
            uint32_t v = rd32(w + 4u * k);
            if (k_pm_f[k].type == FT_FLT) {
                if (range_f(u2f(v), 1.0e9f)) G32(pm + k_pm_f[k].off) = v;
            } else if ((int32_t)v > -1000000000 && (int32_t)v < 1000000000) {
                G32(pm + k_pm_f[k].off) = v;
            }
        }
        st->pm++;
    }
    if (ntech) {
        uint32_t tt, n = tech_rows(&tt), r;
        for (r = 0; r < ntech && r < n; r++) {
            uint32_t id = rd32(p + *o + r * 8u), fl = rd32(p + *o + r * 8u + 4u);
            if (id == G32(tt + 0x38 + r * 8u)) G8(tt + 0x3Fu + r * 8u) = (uint8_t)(fl >> 24);
        }
        st->tech += r;
        *o += ntech * 8u;
    }
    if (mmflag) {
        uint32_t mm = G32(CP_MINIMAP), k;
        const uint8_t *w = p + *o;
        *o += 136u;
        if (cp_obj_ok(mm) && rd32(w) < 8u && rd32(w + 4) <= 8u) {
            int ok = 1;
            for (k = 0; k < 32u; k++) if (!finite_f(u2f(rd32(w + 8 + 4 * k)))) ok = 0;
            if (ok) {
                G32(mm + 0x1020) = rd32(w);
                G32(mm + 0x1024) = rd32(w + 4);
                for (k = 0; k < 32u; k++) G32(mm + 0x1028 + 4 * k) = rd32(w + 8 + 4 * k);
            }
        }
    }
}

void cp_state_apply(const uint8_t *p, uint32_t len, const CpHooks *hk, CpApplyStats *st, uint32_t local_ms)
{
    uint32_t nobj, npm, ntech, flags, o, i, nunm = 0, now_ms, ncand = 0, idx;
    CpApplyStats z;
    if (!st) st = &z;
    if (!cp_state_validate(p, len)) {
        st->bad++;
        return;
    }
    tables_init();
    nobj = rd32(p + 12);
    npm = rd32(p + 16);
    ntech = rd32(p + 20);
    flags = rd32(p + 8);
    now_ms = local_ms;
    s_epoch++;
    s_pend_n = 0;
    {
        float hc = u2f(rd32(p + 24)), lc = GF(CP_CLOCK);
        if (finite_f(hc) && hc >= 0.0f && hc < 1.0e8f && (!finite_f(lc) || hc - lc > 0.3f || lc - hc > 0.3f))
            G32(CP_CLOCK) = rd32(p + 24);
        G32(CP_RNG_ACC) = rd32(p + 28);
        G32(CP_RNG_SEED) = rd32(p + 32);
    }
    o = CP_HDR;
    apply_globals(p, npm, ntech, rd32(p + 40) == 1u, &o, st);
    for (i = 0; i < nobj; i++) {
        uint32_t hh = rd32(p + o), vt = rd32(p + o + 4), rf = rd32(p + o + 8), mlo = rd32(p + o + 12), mhi = rd32(p + o + 16);
        uint32_t pc = popc(mlo) + popc(mhi), obj;
        const uint8_t *wp = p + o + 20;
        uint32_t rec = o;
        o += 20u + 4u * pc;
        st->records++;
        obj = resolve_exact(hh, vt);
        if (obj) {
            idx = G32(obj + 0x18u) >> 16;
            claim(idx, hh);
            st->exact++;
            apply_obj(obj, vt, rf, wp, mlo, mhi, now_ms, hk);
        } else if (nunm < UNM_MAX) {
            s_unm[nunm++] = rec;
        } else {
            st->skipped++;
        }
    }
    if (nunm) {
        uint32_t u;
        for (idx = 1; idx < 0x10000u && ncand < CAND_MAX; idx++) {
            uint32_t obj = G32(CP_OBJ_TABLE + idx * 4u), vt;
            if (!cp_obj_ok(obj)) continue;
            vt = G32(obj);
            if (!(cp_class(vt) & CF_AVP) || G32(obj + 0x18u) >> 16 != idx || s_claim_ep[idx] == s_epoch || cmapped(obj)) continue;
            s_cand[ncand].idx = idx;
            s_cand[ncand].obj = obj;
            s_cand[ncand].p[0] = GF(obj + 0x40);
            s_cand[ncand].p[1] = GF(obj + 0x44);
            s_cand[ncand].p[2] = GF(obj + 0x48);
            ncand++;
        }
        for (u = 0; u < nunm; u++) {
            uint32_t rec = s_unm[u], hh = rd32(p + rec), vt = rd32(p + rec + 4), mlo = rd32(p + rec + 12), mhi = rd32(p + rec + 16);
            uint32_t c, best = 0xFFFFFFFFu;
            float pos[3] = { 0, 0, 0 }, bd = CP_MATCH_DIST * CP_MATCH_DIST;
            if (rec_pos(vt, mlo, mhi, p + rec + 20, pos)) {
                for (c = 0; c < ncand; c++) {
                    float dx, dy, dz, d2;
                    if (!s_cand[c].obj || G32(s_cand[c].obj) != vt) continue;
                    dx = s_cand[c].p[0] - pos[0];
                    dy = s_cand[c].p[1] - pos[1];
                    dz = s_cand[c].p[2] - pos[2];
                    d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bd) {
                        bd = d2;
                        best = c;
                    }
                }
            }
            if (best != 0xFFFFFFFFu) {
                uint32_t obj = s_cand[best].obj;
                uint32_t ch = G32(obj + 0x18u);
                s_cand[best].obj = 0;
                map_set(hh, ch);
                claim(ch >> 16, hh);
                st->nearby++;
                apply_obj(obj, vt, rd32(p + rec + 8), p + rec + 20, mlo, mhi, now_ms, hk);
            } else {
                st->unmatched++;
                if (hk && hk->unmatched) hk->unmatched(vt);
            }
        }
    }
    for (i = 0; i < s_pend_n; i++) {
        uint32_t lh = cp_map_to_client(s_pend[i].hh);
        if (lh && cp_obj_ok(s_pend[i].obj)) G32(s_pend[i].obj + s_pend[i].off) = lh;
    }
    if ((flags & CP_FLAG_FULL) && !(flags & CP_FLAG_TRUNC)) {
        for (idx = 1; idx < 0x10000u; idx++) {
            uint32_t obj = G32(CP_OBJ_TABLE + idx * 4u), h;
            if (!cp_obj_ok(obj)) {
                s_orphan_n[idx] = 0;
                continue;
            }
            if (!(cp_class(G32(obj)) & CF_UNIT)) continue;
            h = G32(obj + 0x18u);
            if (s_claim_ep[idx] == s_epoch) {
                s_orphan_n[idx] = 0;
                continue;
            }
            if (s_orphan_h[idx] != h) {
                s_orphan_h[idx] = h;
                s_orphan_n[idx] = 0;
            }
            if (G8(obj + 0x26Du)) continue;
            if (++s_orphan_n[idx] >= 2u) {
                st->orphans++;
                if (hk && hk->kill) {
                    hk->kill(h);
                    st->kills++;
                }
                s_orphan_n[idx] = 0;
            }
        }
    }
}

typedef struct { uint32_t type; uint8_t ho, vo, b1, b2; } Schema;

static const Schema k_schema[] = {
    { 0x4E, 0x0C, 0, 0, 0 }, { 0x52, 0x0C, 0, 0, 0 }, { 0x77, 0x0C, 0, 0, 0 }, { 0x7B, 0x0C, 0, 0, 0 },
    { 0x20033, 0x0C, 0, 0, 0 }, { 0x20048, 0x0C, 0, 0, 0 }, { 0x4D, 0x0C, 0, 0, 0 }, { 0x60047, 0x0C, 0, 0, 0 },
    { 0x42, 0, 0, 0, 0 }, { 0x6C, 0, 0, 0, 0 }, { 0x5D, 0, 0, 0, 0 }, { 0x78, 0, 0, 0, 0 }, { 0x79, 0, 0, 0, 0 }, { 0x24, 0, 0, 0, 0 },
    { 0x26, 0, 0, 0, 0 }, { 0x68, 0, 0, 0, 0 }, { 0x4A, 0, 0, 0, 0 }, { 0x76, 0, 0, 0, 0 },
    { 0x5F, 0, 0, 0x0C, 0x0D }, { 0x65, 0, 0, 0x0C, 0 },
    { 0x60030, 0x0C, 0x10, 0x1C, 0 },
    { 0x60039, 0, 0x10, 0x24, 0 }, { 0x6007C, 0, 0x10, 0x24, 0 },
};

static const Schema *schema_of(uint32_t type)
{
    uint32_t i;
    for (i = 0; i < sizeof k_schema / sizeof k_schema[0]; i++)
        if (k_schema[i].type == type) return &k_schema[i];
    return NULL;
}

int cp_order_type_ok(uint32_t type)
{
    return schema_of(type) != NULL;
}

static uint32_t schema_extent(const Schema *s)
{
    uint32_t e = 12;
    if (s->ho && (uint32_t)s->ho + 4u > e) e = (uint32_t)s->ho + 4u;
    if (s->vo && (uint32_t)s->vo + 12u > e) e = (uint32_t)s->vo + 12u;
    if (s->b1 && (uint32_t)s->b1 + 1u > e) e = (uint32_t)s->b1 + 1u;
    if (s->b2 && (uint32_t)s->b2 + 1u > e) e = (uint32_t)s->b2 + 1u;
    return e;
}

static uint32_t msg_table_size(uint32_t type)
{
    uint32_t entry = CP_MSG_TYPES + (type & 0xFFu) * 12u;
    if (!G32(entry)) return 0;
    return G32(entry + 4u);
}

uint32_t cp_order_msg_build(const CpOrder *o, uint8_t *out, uint32_t cap)
{
    const Schema *s = schema_of(o->b);
    uint32_t size, k;
    if (!s) return 0;
    size = msg_table_size(o->b);
    if (size < schema_extent(s) || size > cap || size > CP_MSG_MAX || size != o->n) return 0;
    memset(out, 0, size);
    wr32(out, o->b);
    if (o->c > 15u) return 0;
    out[8] = (uint8_t)o->c;
    if (s->ho) {
        uint32_t h = rd32(o->msg + s->ho);
        if (h && !cp_target_ok(h)) return 0;
        wr32(out + s->ho, h);
    }
    if (s->vo) {
        for (k = 0; k < 3; k++) {
            float v = u2f(rd32(o->msg + s->vo + 4u * k));
            if (!range_f(v, 1.0e7f)) return 0;
            wr32(out + s->vo + 4u * k, f2u(v));
        }
    }
    if (s->b1) {
        if (o->msg[s->b1] > 1u) return 0;
        out[s->b1] = o->msg[s->b1];
    }
    if (s->b2) {
        if (o->msg[s->b2] > 1u) return 0;
        out[s->b2] = o->msg[s->b2];
    }
    return size;
}

static const struct { uint32_t fn; int8_t reg; uint8_t nstack, param; } k_pc[] = {
    { 0x2C880u, 0, 0, 0 }, { 0x2C990u, -1, 1, 0 }, { 0x2CAC0u, -1, 2, 1 }, { 0x2CCD0u, 0, 0, 0 },
    { 0x2CD60u, 0, 0, 0 }, { 0x2CE70u, 0, 0, 0 }, { 0x2CF60u, 0, 0, 0 }, { 0x2D050u, 4, 0, 0 },
    { 0x2D0E0u, 0, 0, 0 }, { 0x2D1D0u, 0, 0, 0 }, { 0x2D2C0u, 4, 0, 0 }, { 0x2D350u, 0, 0, 0 },
    { 0x2D440u, 0, 0, 0 }, { 0x2D580u, 0, 0, 0 }, { 0x2D670u, 0, 0, 0 }, { 0x2D760u, 0, 0, 0 },
    { 0x2DD20u, -1, 1, 0 }, { 0x2DE70u, 0, 0, 0 }, { 0x2DF90u, 4, 0, 0 }, { 0x2DFD0u, 3, 0, 0 },
    { 0x2E130u, 2, 0, 2 }, { 0x2ED80u, 3, 0, 0 },
};

uint32_t cp_pcall_count(void)
{
    return (uint32_t)(sizeof k_pc / sizeof k_pc[0]);
}

uint32_t cp_pcall_fn(uint32_t idx)
{
    return idx < cp_pcall_count() ? k_pc[idx].fn : 0;
}

int cp_pcall_find(uint32_t fn)
{
    uint32_t i;
    for (i = 0; i < cp_pcall_count(); i++)
        if (k_pc[i].fn == fn) return (int)i;
    return -1;
}

int cp_pcall_this_reg(uint32_t idx)
{
    return idx < cp_pcall_count() ? k_pc[idx].reg : -2;
}

uint32_t cp_pcall_nstack(uint32_t idx)
{
    return idx < cp_pcall_count() ? k_pc[idx].nstack : 0;
}

int cp_pcall_param_ok(uint32_t idx, uint32_t param)
{
    if (idx >= cp_pcall_count()) return 0;
    switch (k_pc[idx].param) {
    case 0: return param == 0;
    case 1: return param <= 1u;
    default: return param <= 7u;
    }
}

uint32_t cp_particle_off(uint32_t idx)
{
    return idx < 8u ? 0x250u + 4u * idx : 0;
}

int cp_particle_index(uint32_t off)
{
    if (off >= 0x250u && off <= 0x26Cu && !(off & 3u)) return (int)((off - 0x250u) / 4u);
    return -1;
}

static int floats_ok(const float *f, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!range_f(f[i], 1.0e7f)) return 0;
    return 1;
}

int cp_order_parse(const uint8_t *p, uint32_t len, CpOrder *o)
{
    uint32_t cmd, i;
    if (!p || !o || len < 4u || len > CP_ORDER_WIRE_MAX) return 0;
    memset(o, 0, sizeof *o);
    cmd = rd32(p);
    o->cmd = cmd;
    switch (cmd) {
    case CP_CMD_GIVE:
    case CP_CMD_SELECT: {
        uint32_t n;
        if (len < 8u) return 0;
        n = rd32(p + 4);
        if (n > CP_SEL_MAX || len != 8u + 4u * n) return 0;
        for (i = 0; i < n; i++) {
            o->h[i] = rd32(p + 8u + 4u * i);
            if (!(o->h[i] >> 16)) return 0;
        }
        o->n = n;
        return 1;
    }
    case CP_CMD_MOVE:
        if (len != 48u) return 0;
        o->a = rd32(p + 4);
        o->b = rd32(p + 8);
        for (i = 0; i < 3; i++) o->f[i] = u2f(rd32(p + 12 + 4 * i));
        o->c = rd32(p + 24);
        o->d = rd32(p + 28);
        o->h[0] = rd32(p + 32);
        for (i = 0; i < 3; i++) o->f[3 + i] = u2f(rd32(p + 36 + 4 * i));
        if (o->a > 1u || o->b > 1u || o->c > 1u) return 0;
        if (o->d > 7u && o->d != 0xFFu) return 0;
        return floats_ok(o->f, 6);
    case CP_CMD_PCALL:
        if (len != 28u) return 0;
        o->a = rd32(p + 4);
        o->b = rd32(p + 8);
        o->h[0] = rd32(p + 12);
        for (i = 0; i < 3; i++) o->f[3 + i] = u2f(rd32(p + 16 + 4 * i));
        if (o->a >= cp_pcall_count() || !cp_pcall_param_ok(o->a, o->b)) return 0;
        return floats_ok(o->f + 3, 3);
    case CP_CMD_ORDER: {
        uint32_t n;
        if (len < 24u) return 0;
        if (rd32(p + 4) != 1u) return 0;
        o->h[0] = rd32(p + 8);
        o->b = rd32(p + 12);
        o->c = rd32(p + 16);
        n = rd32(p + 20);
        if (!(o->h[0] >> 16) || !cp_order_type_ok(o->b) || o->c > 15u) return 0;
        if (n < 12u || n > CP_MSG_MAX || (n & 3u) || len != 24u + n) return 0;
        memcpy(o->msg, p + 24, n);
        o->n = n;
        return 1;
    }
    case CP_CMD_REINF:
        if (len != 4u + 4u * 10u) return 0;
        o->a = rd32(p + 4);
        o->b = rd32(p + 8);
        o->h[0] = rd32(p + 12);
        for (i = 0; i < 7; i++) {
            uint32_t t = rd32(p + 16 + 4 * i);
            if (t >= 0x10000u && t < CP_LOCAL_HI) return 0;
            o->h[1 + i] = t;
        }
        return o->a <= 1000000u && o->b <= 1000000u;
    case CP_CMD_BUY:
        if (len != 8u) return 0;
        o->a = rd32(p + 4);
        return o->a < 32u;
    default:
        return 0;
    }
}

uint32_t cp_enc_select(uint8_t *out, const uint32_t *h, uint32_t n)
{
    uint32_t i;
    if (n > CP_SEL_MAX) n = CP_SEL_MAX;
    wr32(out, CP_CMD_SELECT);
    wr32(out + 4, n);
    for (i = 0; i < n; i++) wr32(out + 8 + 4 * i, h[i]);
    return 8u + 4u * n;
}

uint32_t cp_enc_move(uint8_t *out, uint32_t mtype, uint32_t nowhere, const float *pos, uint32_t queue,
                     uint32_t particle, uint32_t under, const float *world)
{
    uint32_t i;
    wr32(out, CP_CMD_MOVE);
    wr32(out + 4, mtype);
    wr32(out + 8, nowhere);
    for (i = 0; i < 3; i++) wr32(out + 12 + 4 * i, f2u(nowhere ? 0.0f : pos[i]));
    wr32(out + 24, queue);
    wr32(out + 28, particle);
    wr32(out + 32, under);
    for (i = 0; i < 3; i++) wr32(out + 36 + 4 * i, f2u(world[i]));
    return 48u;
}

uint32_t cp_enc_pcall(uint8_t *out, uint32_t idx, uint32_t param, uint32_t under, const float *world)
{
    uint32_t i;
    wr32(out, CP_CMD_PCALL);
    wr32(out + 4, idx);
    wr32(out + 8, param);
    wr32(out + 12, under);
    for (i = 0; i < 3; i++) wr32(out + 16 + 4 * i, f2u(world[i]));
    return 28u;
}

uint32_t cp_enc_order(uint8_t *out, uint32_t unit, uint32_t type, uint32_t prio, const uint8_t *msg, uint32_t n)
{
    wr32(out, CP_CMD_ORDER);
    wr32(out + 4, 1);
    wr32(out + 8, unit);
    wr32(out + 12, type);
    wr32(out + 16, prio);
    wr32(out + 20, n);
    memcpy(out + 24, msg, n);
    return 24u + n;
}

uint32_t cp_enc_buy(uint8_t *out, uint32_t idx)
{
    wr32(out, CP_CMD_BUY);
    wr32(out + 4, idx);
    return 8u;
}

uint32_t cp_read_selection(uint32_t ptr, uint32_t *out, uint32_t max)
{
    uint32_t sel, data, n, i, cnt = 0;
    if (ptr < 0x10000u || ptr >= CP_LOCAL_HI - 0x1000u) return 0;
    sel = G32(ptr + 0x1B8u);
    if (sel < 0x10000u || sel >= CP_LOCAL_HI - 0x1000u) return 0;
    data = G32(sel);
    n = G32(sel + 4u);
    if (data < 0x10000u || data >= CP_LOCAL_HI - 0x1000u || n > CP_SEL_MAX) return 0;
    for (i = 0; i < n && cnt < max; i++) {
        uint32_t entry = G32(data + i * 4u), unit, h;
        if (entry < 0x10000u || entry >= CP_LOCAL_HI - 0x1000u) continue;
        unit = G32(entry + 0xA8u);
        if (!cp_obj_ok(unit)) continue;
        h = G32(unit + 0x18u);
        if (h && cp_obj_of(h) == unit) out[cnt++] = h;
    }
    return cnt;
}

int cp_order_xlate_out(uint32_t type, uint8_t *msg, uint32_t size)
{
    const Schema *s = schema_of(type);
    if (!s || size < schema_extent(s) || size > CP_MSG_MAX) return 0;
    if (s->ho) {
        uint32_t h = rd32(msg + s->ho);
        if (h) {
            h = cp_map_to_host(h);
            if (!h) return 0;
        }
        wr32(msg + s->ho, h);
    }
    return 1;
}

int cp_order_xlate_in(uint32_t type, uint8_t *msg, uint32_t size)
{
    const Schema *s = schema_of(type);
    if (!s || size < schema_extent(s) || size > CP_MSG_MAX) return 0;
    if (s->ho) {
        uint32_t h = rd32(msg + s->ho);
        if (h) {
            h = cp_map_to_client(h);
            if (!h) return 0;
        }
        wr32(msg + s->ho, h);
    }
    return 1;
}

uint32_t cp_enc_give(uint8_t *out, const uint32_t *h, uint32_t n)
{
    uint32_t len = cp_enc_select(out, h, n);
    wr32(out, CP_CMD_GIVE);
    return len;
}

uint32_t cp_enc_reinf(uint8_t *out, uint32_t cost, uint32_t supply, uint32_t drop, const uint32_t *types)
{
    uint32_t i;
    wr32(out, CP_CMD_REINF);
    wr32(out + 4, cost);
    wr32(out + 8, supply);
    wr32(out + 12, drop);
    for (i = 0; i < 7; i++) wr32(out + 16 + 4 * i, types[i]);
    return 44u;
}
