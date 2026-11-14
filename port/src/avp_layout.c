#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#ifdef _WIN32
#include "avp_platform.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "avp_layout.h"

static const char *const s_race_names[AVP_RACE_COUNT] = { "alien", "marine", "predator" };

const char *avp_race_name(AvpRace r)
{
    return (unsigned)r < AVP_RACE_COUNT ? s_race_names[r] : "unknown";
}

int avp_race_parse(const char *s, AvpRace *out)
{
    int i;
    if (!s)
        return 0;
    for (i = 0; i < AVP_RACE_COUNT; i++) {
        const char *a = s, *b = s_race_names[i];
        while (*a && tolower((unsigned char)*a) == *b) {
            a++;
            b++;
        }
        if (!*a && !*b) {
            if (out)
                *out = (AvpRace)i;
            return 1;
        }
    }
    return 0;
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;
    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi)
        return 0;
    *out = (int)v;
    return 1;
}

static int starts_ci(const char *s, const char *p)
{
    while (*p) {
        if (tolower((unsigned char)*s) != *p)
            return 0;
        s++;
        p++;
    }
    return 1;
}

int avp_aff_parse(const char *s, AvpAff *out)
{
    AvpAff a;
    char tmp[32];
    char *c1, *c2;
    memset(&a, 0, sizeof a);
    if (!s)
        return 0;
    if (starts_ci(s, "slot")) {
        int v;
        if (!parse_int(s + 4, 1, 8, &v))
            return 0;
        a.kind = AVP_AFF_SLOT;
        a.slot = v;
    } else if (starts_ci(s, "neutral") && !s[7]) {
        a.kind = AVP_AFF_NEUTRAL;
        a.slot = 9;
    } else if (starts_ci(s, "spawn:")) {
        if (strlen(s + 6) >= sizeof tmp)
            return 0;
        strcpy(tmp, s + 6);
        c1 = strchr(tmp, ':');
        if (!c1)
            return 0;
        *c1++ = 0;
        c2 = strchr(c1, ':');
        if (c2)
            return 0;
        if (!avp_race_parse(tmp, &a.race) || !parse_int(c1, 1, 3, &a.n))
            return 0;
        a.kind = AVP_AFF_SPAWN;
    } else {
        return 0;
    }
    if (out)
        *out = a;
    return 1;
}

void avp_aff_format(const AvpAff *a, char *buf, size_t n)
{
    if (!n)
        return;
    switch (a->kind) {
    case AVP_AFF_SLOT:
        snprintf(buf, n, "slot%d", a->slot);
        break;
    case AVP_AFF_NEUTRAL:
        snprintf(buf, n, "neutral");
        break;
    case AVP_AFF_SPAWN:
        snprintf(buf, n, "spawn:%s:%d", avp_race_name(a->race), a->n);
        break;
    default:
        snprintf(buf, n, "neutral");
        break;
    }
}

void avp_layout_init(AvpLayout *l, const char *map)
{
    int r, i;
    memset(l, 0, sizeof *l);
    if (map)
        snprintf(l->map, sizeof l->map, "%s", map);
    for (r = 0; r < AVP_RACE_COUNT; r++)
        for (i = 0; i < 3; i++) {
            l->spawn[r][i].race = (AvpRace)r;
            l->spawn[r][i].n = i + 1;
        }
}

static int parse_float(const char *s, float *out)
{
    char *end;
    double v;
    if (!s || !*s)
        return 0;
    v = strtod(s, &end);
    if (*end || !isfinite(v))
        return 0;
    *out = (float)v;
    return 1;
}

static int map_name_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (!n || n >= 16)
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
            return 0;
    return 1;
}

static int split(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (n == max)
            return max + 1;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
    }
    return n;
}

static int parse_line(AvpLayout *l, const char *raw)
{
    char line[256], *tok[8];
    char *p;
    int n;
    size_t len = strlen(raw);
    while (len && (raw[len - 1] == '\r' || raw[len - 1] == ' ' || raw[len - 1] == '\t'))
        len--;
    if (len >= sizeof line)
        return 1;
    memcpy(line, raw, len);
    line[len] = 0;
    p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')
        return 0;
    n = split(p, tok, 7);
    if (!strcmp(tok[0], "layout")) {
        int v;
        return n == 2 && parse_int(tok[1], 1, 1, &v) ? 0 : 1;
    }
    if (!strcmp(tok[0], "map")) {
        if (n != 2 || !map_name_ok(tok[1]))
            return 1;
        snprintf(l->map, sizeof l->map, "%s", tok[1]);
        return 0;
    }
    if (!strcmp(tok[0], "spawn")) {
        AvpRace r;
        int k;
        float x, y, z, rot;
        if (n != 7 || !avp_race_parse(tok[1], &r) || !parse_int(tok[2], 1, 3, &k)
            || !parse_float(tok[3], &x) || !parse_float(tok[4], &y)
            || !parse_float(tok[5], &z) || !parse_float(tok[6], &rot))
            return 1;
        l->spawn[r][k - 1].race = r;
        l->spawn[r][k - 1].n = k;
        l->spawn[r][k - 1].x = x;
        l->spawn[r][k - 1].y = y;
        l->spawn[r][k - 1].z = z;
        l->spawn[r][k - 1].rot = rot;
        l->spawn[r][k - 1].used = 1;
        return 0;
    }
    if (!strcmp(tok[0], "unit")) {
        AvpUnitRec u;
        if (n != 7 || strlen(tok[1]) >= sizeof u.cls)
            return 1;
        memset(&u, 0, sizeof u);
        strcpy(u.cls, tok[1]);
        if (!avp_aff_parse(tok[2], &u.aff) || !parse_float(tok[3], &u.x)
            || !parse_float(tok[4], &u.y) || !parse_float(tok[5], &u.z)
            || !parse_float(tok[6], &u.rot))
            return 1;
        return avp_layout_add_unit(l, &u) ? 0 : 1;
    }
    if (len >= sizeof l->extra[0] || l->nextra >= AVP_LAYOUT_MAX_EXTRA)
        return 1;
    memcpy(l->extra[l->nextra], raw, len);
    l->extra[l->nextra][len] = 0;
    l->nextra++;
    return 0;
}

int avp_layout_parse(AvpLayout *l, const char *text)
{
    int errors = 0;
    const char *p = text;
    char line[512];
    if (!text)
        return 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) {
            errors++;
        } else {
            memcpy(line, p, n);
            line[n] = 0;
            errors += parse_line(l, line);
        }
        p += n;
        if (*p == '\n')
            p++;
    }
    return errors;
}

int avp_layout_add_unit(AvpLayout *l, const AvpUnitRec *u)
{
    if (l->nunits >= AVP_LAYOUT_MAX_UNITS)
        return 0;
    l->unit[l->nunits++] = *u;
    return 1;
}

void avp_layout_remove_unit(AvpLayout *l, int i)
{
    if (i < 0 || i >= l->nunits)
        return;
    memmove(&l->unit[i], &l->unit[i + 1], (size_t)(l->nunits - i - 1) * sizeof l->unit[0]);
    l->nunits--;
}

int avp_layout_write(const AvpLayout *l, char *buf, size_t n)
{
    size_t pos = 0;
    int r, i;
    char line[320];
#define EMIT(...) \
    do { \
        int w_ = snprintf(line, sizeof line, __VA_ARGS__); \
        if (w_ < 0 || pos + (size_t)w_ + 1 > n) \
            return -1; \
        memcpy(buf + pos, line, (size_t)w_); \
        pos += (size_t)w_; \
    } while (0)
    if (!n)
        return -1;
    EMIT("layout 1\n");
    EMIT("map %s\n", l->map);
    for (r = 0; r < AVP_RACE_COUNT; r++)
        for (i = 0; i < 3; i++) {
            const AvpSpawn *s = &l->spawn[r][i];
            if (s->used)
                EMIT("spawn %s %d %.9g %.9g %.9g %.9g\n", avp_race_name((AvpRace)r), i + 1,
                     s->x, s->y, s->z, s->rot);
        }
    for (i = 0; i < l->nunits; i++) {
        const AvpUnitRec *u = &l->unit[i];
        char aff[32];
        avp_aff_format(&u->aff, aff, sizeof aff);
        EMIT("unit %s %s %.9g %.9g %.9g %.9g\n", u->cls, aff, u->x, u->y, u->z, u->rot);
    }
    for (i = 0; i < l->nextra; i++)
        EMIT("%s\n", l->extra[i]);
#undef EMIT
    buf[pos] = 0;
    return (int)pos;
}

static void layout_path(const char *map, char *out, size_t n)
{
    snprintf(out, n, "maps/%s.layout", map);
}

int avp_layout_load(AvpLayout *l, const char *map)
{
    char path[64];
    FILE *f;
    char *text;
    long sz;
    int errors;
    avp_layout_init(l, map);
    if (!map || !map_name_ok(map))
        return -1;
    layout_path(map, path, sizeof path);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    text = malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        return -1;
    }
    sz = (long)fread(text, 1, (size_t)sz, f);
    text[sz] = 0;
    fclose(f);
    errors = avp_layout_parse(l, text);
    free(text);
    if (!l->map[0])
        snprintf(l->map, sizeof l->map, "%s", map);
    return errors;
}

int avp_layout_save(const AvpLayout *l)
{
    char path[64];
    char *buf;
    int n;
    FILE *f;
    size_t cap = 64 * 1024 + (size_t)AVP_LAYOUT_MAX_UNITS * 160 + (size_t)AVP_LAYOUT_MAX_EXTRA * 170;
    if (!map_name_ok(l->map))
        return 0;
    buf = malloc(cap);
    if (!buf)
        return 0;
    n = avp_layout_write(l, buf, cap);
    if (n < 0) {
        free(buf);
        return 0;
    }
#ifdef _WIN32
    CreateDirectoryA("maps", NULL);
#else
    mkdir("maps", 0777);
#endif
    layout_path(l->map, path, sizeof path);
    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return 0;
    }
    n = fwrite(buf, 1, (size_t)n, f) == (size_t)n && fclose(f) == 0;
    free(buf);
    return n;
}
