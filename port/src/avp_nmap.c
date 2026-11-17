#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "avp_nmap.h"

#define ATMOS_CLASS "GAtmosProcessor"

int nmap_start_list(const Nmap *m, int *nums)
{
    int i, n = 0;
    for (i = 0; i < NMAP_MAX_SPAWN; i++)
        if (m->spawn[i].used)
            nums[n++] = i + 1;
    return n;
}

int nmap_is_atmos(const char *cls)
{
    return !strcmp(cls, ATMOS_CLASS);
}

void nmap_resolve(Nmap *m, float w, float h, float z)
{
    int i;
    if (!m->relative) {
        if (m->tw > 0) {
            m->bw = (float)m->tw;
            m->bh = (float)m->th;
        }
        return;
    }
    if (!(w > 1.0f) || !(h > 1.0f)) {
        w = m->bw > 1.0f ? m->bw : 230.0f;
        h = m->bh > 1.0f ? m->bh : 230.0f;
    }
    for (i = 0; i < NMAP_MAX_SPAWN; i++)
        if (m->spawn[i].used) {
            m->spawn[i].x *= w;
            m->spawn[i].y *= h;
            m->spawn[i].z = z;
        }
    for (i = 0; i < m->nwild; i++) {
        m->wild[i].x *= w;
        m->wild[i].y *= h;
        m->wild[i].z = z;
    }
    for (i = 0; i < m->nobj; i++) {
        m->obj[i].x *= w;
        m->obj[i].y *= h;
        m->obj[i].z = z;
    }
    m->relative = 0;
    m->bw = w;
    m->bh = h;
}

void nmap_init(Nmap *m, const char *base)
{
    memset(m, 0, sizeof *m);
    if (base)
        snprintf(m->base, sizeof m->base, "%s", base);
}

int nmap_name_ok(const char *s)
{
    size_t i, n = s ? strlen(s) : 0;
    if (!n || n >= NMAP_NAME)
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
            return 0;
    return 1;
}

static int owner_parse(const char *s, int *out)
{
    if (!strcmp(s, "neutral")) {
        *out = 0;
        return 1;
    }
    if (!strncmp(s, "slot", 4) && s[4] >= '1' && s[4] <= '8' && !s[5]) {
        *out = s[4] - '0';
        return 1;
    }
    return 0;
}

static float num_f(const char *s)
{
    double v = atof(s);
    if (!(fabs(v) < 1e7))
        return 0.0f;
    return (float)v;
}

void nmap_clean_text(char *dst, size_t n, const char *src)
{
    size_t k = 0;
    while (*src && isspace((unsigned char)*src))
        src++;
    for (; *src && k + 1 < n; src++)
        dst[k++] = (unsigned char)*src < 32 || (unsigned char)*src > 126 ? ' ' : *src;
    while (k && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
}

static int race_parse(const char *s)
{
    if (!strcmp(s, "alien"))
        return 0;
    if (!strcmp(s, "marine"))
        return 1;
    if (!strcmp(s, "predator"))
        return 2;
    return -1;
}

static int parse_text_key(Nmap *m, const char *line)
{
    if (!strncmp(line, "title ", 6)) {
        nmap_clean_text(m->title, sizeof m->title, line + 6);
        return 1;
    }
    if (!strncmp(line, "author ", 7)) {
        nmap_clean_text(m->author, sizeof m->author, line + 7);
        return 1;
    }
    if (!strncmp(line, "desc ", 5)) {
        nmap_clean_text(m->desc, sizeof m->desc, line + 5);
        return 1;
    }
    return 0;
}

static int parse_line(Nmap *m, char *line)
{
    if (parse_text_key(m, line))
        return 0;
    char *tok[10], *p = line;
    int n = 0;
    float v[4];
    int i;
    while (*p && n < 10) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = 0;
    }
    if (!n || tok[0][0] == '#')
        return 0;
    if (!strcmp(tok[0], "nmap"))
        return n == 2 ? 0 : 1;
    if (!strcmp(tok[0], "base")) {
        if (n != 2 || strlen(tok[1]) != 3)
            return 1;
        for (i = 0; i < 3; i++)
            m->base[i] = (char)toupper((unsigned char)tok[1][i]);
        m->base[3] = 0;
        return 0;
    }
    if (!strcmp(tok[0], "relative")) {
        m->relative = n == 2 && atoi(tok[1]) != 0;
        return n == 2 ? 0 : 1;
    }
    if (!strcmp(tok[0], "bounds")) {
        if (n != 3)
            return 1;
        m->bw = num_f(tok[1]);
        m->bh = num_f(tok[2]);
        return 0;
    }
    if (!strcmp(tok[0], "players")) {
        int a = n == 3 ? atoi(tok[1]) : 0, b = n == 3 ? atoi(tok[2]) : 0;
        if (a < 1 || b < a || b > NMAP_MAX_SPAWN)
            return 1;
        m->minp = a;
        m->maxp = b;
        return 0;
    }
    if (!strcmp(tok[0], "slot")) {
        int k = n == 4 ? atoi(tok[1]) : 0, t = n == 4 ? atoi(tok[3]) : 0;
        if (k < 1 || k > NMAP_MAX_SPAWN || t < 0 || t > NMAP_MAX_SPAWN)
            return 1;
        m->slot[k - 1].used = 1;
        m->slot[k - 1].race = race_parse(tok[2]);
        m->slot[k - 1].team = t;
        return 0;
    }
    if (!strcmp(tok[0], "spawn")) {
        int k = n == 6 ? atoi(tok[1]) : 0;
        if (k < 1 || k > NMAP_MAX_SPAWN)
            return 1;
        for (i = 0; i < 4; i++)
            v[i] = num_f(tok[2 + i]);
        m->spawn[k - 1].used = 1;
        m->spawn[k - 1].x = v[0];
        m->spawn[k - 1].y = v[1];
        m->spawn[k - 1].z = v[2];
        m->spawn[k - 1].rot = v[3];
        return 0;
    }
    if (!strcmp(tok[0], "terrain")) {
        int tw = n == 3 ? atoi(tok[1]) : 0, th = n == 3 ? atoi(tok[2]) : 0;
        if (tw < 8 || th < 8 || tw > 1024 || th > 1024)
            return 1;
        m->tw = tw;
        m->th = th;
        return 0;
    }
    if (!strcmp(tok[0], "wildlife")) {
        NmapWild *w;
        if (n != 8 || m->nwild >= NMAP_MAX_WILD || strlen(tok[1]) >= sizeof w->cls)
            return 1;
        w = &m->wild[m->nwild++];
        memset(w, 0, sizeof *w);
        strcpy(w->cls, tok[1]);
        w->x = num_f(tok[2]);
        w->y = num_f(tok[3]);
        w->z = num_f(tok[4]);
        w->max = atoi(tok[5]);
        w->delay = num_f(tok[6]);
        w->radius = num_f(tok[7]);
        if (w->max < 1)
            w->max = 1;
        if (w->max > 16)
            w->max = 16;
        return 0;
    }
    if (!strcmp(tok[0], "atmos") || !strcmp(tok[0], "object")) {
        NmapObj o;
        int at = !strcmp(tok[0], "atmos"), base = at ? 1 : 3;
        if (n != base + 4 || m->nobj >= NMAP_MAX_OBJ)
            return 1;
        memset(&o, 0, sizeof o);
        if (at) {
            strcpy(o.cls, ATMOS_CLASS);
            o.owner = 0;
        } else {
            if (strlen(tok[1]) >= sizeof o.cls || !owner_parse(tok[2], &o.owner))
                return 1;
            strcpy(o.cls, tok[1]);
        }
        o.x = num_f(tok[base]);
        o.y = num_f(tok[base + 1]);
        o.z = num_f(tok[base + 2]);
        o.rot = num_f(tok[base + 3]);
        m->obj[m->nobj++] = o;
        return 0;
    }
    return 1;
}

int nmap_parse(Nmap *m, const char *text)
{
    int errors = 0;
    const char *p = text;
    char line[512];
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n < sizeof line) {
            memcpy(line, p, n);
            line[n] = 0;
            errors += parse_line(m, line);
        } else {
            errors++;
        }
        p += n;
        if (*p == '\n')
            p++;
    }
    return errors;
}

int nmap_write(const Nmap *m, char *buf, size_t n)
{
    size_t pos = 0;
    int i;
    char line[512];
#define EMIT(...) \
    do { \
        int w_ = snprintf(line, sizeof line, __VA_ARGS__); \
        if (w_ < 0 || pos + (size_t)w_ + 1 > n) \
            return -1; \
        memcpy(buf + pos, line, (size_t)w_); \
        pos += (size_t)w_; \
    } while (0)
    EMIT("nmap 1\n");
    EMIT("base %s\n", m->base);
    if (m->relative)
        EMIT("relative 1\n");
    if (m->title[0])
        EMIT("title %s\n", m->title);
    if (m->author[0])
        EMIT("author %s\n", m->author);
    if (m->desc[0])
        EMIT("desc %s\n", m->desc);
    if (m->minp > 0 && m->maxp >= m->minp)
        EMIT("players %d %d\n", m->minp, m->maxp);
    for (i = 0; i < NMAP_MAX_SPAWN; i++) {
        static const char *const rn[] = { "any", "alien", "marine", "predator" };
        if (m->slot[i].used)
            EMIT("slot %d %s %d\n", i + 1, rn[m->slot[i].race >= 0 && m->slot[i].race < 3 ? m->slot[i].race + 1 : 0], m->slot[i].team);
    }
    if (m->bw > 0 && m->bh > 0)
        EMIT("bounds %.6g %.6g\n", m->bw, m->bh);
    if (m->tw > 0 && m->th > 0)
        EMIT("terrain %d %d\n", m->tw, m->th);
    for (i = 0; i < NMAP_MAX_SPAWN; i++)
        if (m->spawn[i].used)
            EMIT("spawn %d %.6g %.6g %.6g %.6g\n", i + 1, m->spawn[i].x, m->spawn[i].y, m->spawn[i].z, m->spawn[i].rot);
    for (i = 0; i < m->nwild; i++) {
        const NmapWild *w = &m->wild[i];
        EMIT("wildlife %s %.6g %.6g %.6g %d %.6g %.6g\n", w->cls, w->x, w->y, w->z, w->max, w->delay, w->radius);
    }
    for (i = 0; i < m->nobj; i++) {
        const NmapObj *o = &m->obj[i];
        if (nmap_is_atmos(o->cls))
            EMIT("atmos %.6g %.6g %.6g %.6g\n", o->x, o->y, o->z, o->rot);
        else if (o->owner)
            EMIT("object %s slot%d %.6g %.6g %.6g %.6g\n", o->cls, o->owner, o->x, o->y, o->z, o->rot);
        else
            EMIT("object %s neutral %.6g %.6g %.6g %.6g\n", o->cls, o->x, o->y, o->z, o->rot);
    }
#undef EMIT
    buf[pos] = 0;
    return (int)pos;
}

int nmap_terrain_resample(const NmapTerrain *src, NmapTerrain *dst, int w, int h)
{
    int x, y, k;
    if (!src || !src->z || src->w < 2 || src->h < 2 || !nmap_terrain_alloc(dst, w, h))
        return 0;
    for (y = 0; y < h; y++) {
        float fy = h > 1 ? (float)y * (float)(src->h - 1) / (float)(h - 1) : 0.0f;
        int y0 = (int)fy, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        float ty = fy - (float)y0;
        for (x = 0; x < w; x++) {
            float fx = w > 1 ? (float)x * (float)(src->w - 1) / (float)(w - 1) : 0.0f;
            int x0 = (int)fx, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            float tx = fx - (float)x0;
            size_t i00 = (size_t)y0 * (size_t)src->w + (size_t)x0, i10 = (size_t)y0 * (size_t)src->w + (size_t)x1;
            size_t i01 = (size_t)y1 * (size_t)src->w + (size_t)x0, i11 = (size_t)y1 * (size_t)src->w + (size_t)x1;
            size_t o = (size_t)y * (size_t)w + (size_t)x;
            float z = ((float)src->z[i00] * (1.0f - tx) + (float)src->z[i10] * tx) * (1.0f - ty)
                    + ((float)src->z[i01] * (1.0f - tx) + (float)src->z[i11] * tx) * ty;
            dst->z[o] = (int16_t)(z < 0.0f ? z - 0.5f : z + 0.5f);
            if (src->flags && dst->flags)
                dst->flags[o] = src->flags[(size_t)(ty < 0.5f ? y0 : y1) * (size_t)src->w + (size_t)(tx < 0.5f ? x0 : x1)];
            for (k = 0; k < NMAP_LAYERS; k++) {
                float v = ((float)src->splat[i00 * NMAP_LAYERS + (size_t)k] * (1.0f - tx) + (float)src->splat[i10 * NMAP_LAYERS + (size_t)k] * tx) * (1.0f - ty)
                        + ((float)src->splat[i01 * NMAP_LAYERS + (size_t)k] * (1.0f - tx) + (float)src->splat[i11 * NMAP_LAYERS + (size_t)k] * tx) * ty;
                dst->splat[o * NMAP_LAYERS + (size_t)k] = (uint8_t)(v + 0.5f);
            }
        }
    }
    return 1;
}

void nmap_to_world(Nmap *m, float x0, float y0, float w, float h, float margin)
{
    float sx = m->tw > 0 ? w / (float)m->tw : 1.0f, sy = m->th > 0 ? h / (float)m->th : 1.0f;
    float lx = x0 + margin, hx = x0 + w - margin, ly = y0 + margin, hy = y0 + h - margin;
    int i;
    if (m->relative || !(w > 1.0f) || !(h > 1.0f))
        return;
    if (!(sx > 0.0f))
        sx = 1.0f;
    if (!(sy > 0.0f))
        sy = 1.0f;
    for (i = 0; i < NMAP_MAX_SPAWN; i++)
        if (m->spawn[i].used) {
            m->spawn[i].x = fminf(fmaxf(x0 + m->spawn[i].x * sx, lx), hx);
            m->spawn[i].y = fminf(fmaxf(y0 + m->spawn[i].y * sy, ly), hy);
        }
    for (i = 0; i < m->nobj; i++) {
        m->obj[i].x = fminf(fmaxf(x0 + m->obj[i].x * sx, lx), hx);
        m->obj[i].y = fminf(fmaxf(y0 + m->obj[i].y * sy, ly), hy);
    }
    for (i = 0; i < m->nwild; i++) {
        m->wild[i].x = fminf(fmaxf(x0 + m->wild[i].x * sx, lx), hx);
        m->wild[i].y = fminf(fmaxf(y0 + m->wild[i].y * sy, ly), hy);
        m->wild[i].radius *= 0.5f * (sx + sy);
    }
    m->bw = w;
    m->bh = h;
}

int nmap_terrain_alloc(NmapTerrain *t, int w, int h)
{
    memset(t, 0, sizeof *t);
    if (w < 8 || h < 8 || w > 1024 || h > 1024)
        return 0;
    t->z = calloc((size_t)w * (size_t)h, sizeof *t->z);
    t->splat = calloc((size_t)w * (size_t)h, NMAP_LAYERS);
    t->flags = calloc((size_t)w * (size_t)h, 1);
    if (!t->z || !t->splat || !t->flags) {
        nmap_terrain_free(t);
        return 0;
    }
    t->w = w;
    t->h = h;
    t->hmin = NMAP_HMIN_DEF;
    t->hmax = NMAP_HMAX_DEF;
    t->hstep = NMAP_HSTEP_DEF;
    t->hlevels = NMAP_HLEVELS_DEF;
    return 1;
}

void nmap_terrain_free(NmapTerrain *t)
{
    free(t->z);
    free(t->splat);
    free(t->flags);
    memset(t, 0, sizeof *t);
}

int nmap_terrain_path(const char *name, char *out, size_t n)
{
    if (!nmap_name_ok(name))
        return 0;
    snprintf(out, n, "maps/%s.nterr", name);
    return 1;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int get_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int nmap_terrain_save(const NmapTerrain *t, const char *name)
{
    char path[96];
    uint8_t hdr[20];
    size_t n = (size_t)t->w * (size_t)t->h, i;
    uint8_t *zb;
    FILE *f;
    int ok;
    if (!t->z || !nmap_terrain_path(name, path, sizeof path))
        return 0;
    zb = malloc(n * 2);
    if (!zb)
        return 0;
    for (i = 0; i < n; i++) {
        zb[i * 2] = (uint8_t)(uint16_t)t->z[i];
        zb[i * 2 + 1] = (uint8_t)((uint16_t)t->z[i] >> 8);
    }
    memcpy(hdr, "NTR2", 4);
    put_u32(hdr + 4, (uint32_t)t->w);
    put_u32(hdr + 8, (uint32_t)t->h);
    put_u16(hdr + 12, (uint16_t)(int16_t)t->hmin);
    put_u16(hdr + 14, (uint16_t)(int16_t)t->hmax);
    put_u16(hdr + 16, (uint16_t)(int16_t)t->hstep);
    put_u16(hdr + 18, (uint16_t)(int16_t)t->hlevels);
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
#else
    mkdir("maps", 0777);
#endif
    f = fopen(path, "wb");
    if (!f) {
        free(zb);
        return 0;
    }
    ok = fwrite(hdr, 1, 20, f) == 20 && fwrite(zb, 1, n * 2, f) == n * 2
        && fwrite(t->splat, NMAP_LAYERS, n, f) == n
        && (!t->flags || fwrite(t->flags, 1, n, f) == n) && fclose(f) == 0;
    free(zb);
    return ok;
}

int nmap_terrain_load(NmapTerrain *t, const char *name)
{
    char path[96];
    uint8_t hdr[20];
    size_t n, i;
    uint8_t *zb;
    FILE *f;
    int w, h, v2;
    memset(t, 0, sizeof *t);
    if (!nmap_terrain_path(name, path, sizeof path))
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, 12, f) != 12 || (memcmp(hdr, "NTR1", 4) && memcmp(hdr, "NTR2", 4))) {
        fclose(f);
        return 0;
    }
    v2 = !memcmp(hdr, "NTR2", 4);
    if (v2 && fread(hdr + 12, 1, 8, f) != 8) {
        fclose(f);
        return 0;
    }
    w = (int)get_u32(hdr + 4);
    h = (int)get_u32(hdr + 8);
    if (!nmap_terrain_alloc(t, w, h)) {
        fclose(f);
        return 0;
    }
    if (v2) {
        t->hmin = get_i16(hdr + 12);
        t->hmax = get_i16(hdr + 14);
        t->hstep = get_i16(hdr + 16);
        t->hlevels = get_i16(hdr + 18);
        if (t->hmin >= t->hmax || t->hstep < 1 || t->hlevels < 1) {
            t->hmin = NMAP_HMIN_DEF;
            t->hmax = NMAP_HMAX_DEF;
            t->hstep = NMAP_HSTEP_DEF;
            t->hlevels = NMAP_HLEVELS_DEF;
        }
    }
    n = (size_t)w * (size_t)h;
    zb = malloc(n * 2);
    if (!zb || fread(zb, 1, n * 2, f) != n * 2 || fread(t->splat, NMAP_LAYERS, n, f) != n
        || (v2 && fread(t->flags, 1, n, f) != n)) {
        free(zb);
        fclose(f);
        nmap_terrain_free(t);
        return 0;
    }
    for (i = 0; i < n; i++)
        t->z[i] = (int16_t)((uint16_t)zb[i * 2] | (uint16_t)zb[i * 2 + 1] << 8);
    free(zb);
    fclose(f);
    return 1;
}

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "maps/%s.nmap", name);
}

int nmap_load(Nmap *m, const char *name)
{
    char path[96], *text;
    FILE *f;
    long sz;
    nmap_init(m, "");
    if (!nmap_name_ok(name))
        return -1;
    path_of(name, path, sizeof path);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024 || !(text = malloc((size_t)sz + 1))) {
        fclose(f);
        return -1;
    }
    sz = (long)fread(text, 1, (size_t)sz, f);
    text[sz] = 0;
    fclose(f);
    {
        int e = nmap_parse(m, text);
        free(text);
        return e;
    }
}

int nmap_save(const Nmap *m, const char *name)
{
    char path[96];
    size_t cap = 4096 + (size_t)NMAP_MAX_OBJ * 140;
    char *buf;
    int n, ok;
    FILE *f;
    if (!nmap_name_ok(name) || !(buf = malloc(cap)))
        return 0;
    n = nmap_write(m, buf, cap);
    if (n < 0) {
        free(buf);
        return 0;
    }
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
#else
    mkdir("maps", 0777);
#endif
    path_of(name, path, sizeof path);
    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return 0;
    }
    ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n && fclose(f) == 0;
    free(buf);
    return ok;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void add_name(char names[][NMAP_NAME], int *n, int max, const char *file)
{
    size_t len = strlen(file);
    char tmp[NMAP_NAME];
    if (len < 6 || strcmp(file + len - 5, ".nmap") || len - 5 >= NMAP_NAME || *n >= max)
        return;
    memcpy(tmp, file, len - 5);
    tmp[len - 5] = 0;
    if (!nmap_name_ok(tmp) || tmp[0] == '_')
        return;
    strcpy(names[(*n)++], tmp);
}

int nmap_list(char names[][NMAP_NAME], int max)
{
    int n = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("maps\\*.nmap", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            add_name(names, &n, max, fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir("maps");
    struct dirent *e;
    if (d) {
        while ((e = readdir(d)) != NULL)
            add_name(names, &n, max, e->d_name);
        closedir(d);
    }
#endif
    qsort(names, (size_t)n, NMAP_NAME, cmp_name);
    return n;
}

void nmap_open_folder(void)
{
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
    ShellExecuteA(NULL, "open", "maps", NULL, NULL, SW_SHOWNORMAL);
#else
    mkdir("maps", 0777);
    if (fork() == 0) {
        execlp("xdg-open", "xdg-open", "maps", (char *)NULL);
        _exit(1);
    }
#endif
}
