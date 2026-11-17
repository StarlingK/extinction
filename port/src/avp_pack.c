#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "avp_pack.h"

#define TBD_HEADER 12u
#define PACK_VERSION_TAG "NSKP"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static const char *const k_mission_lists[] = {
    "objective.kfc",
    "maploaded.kfc",
    "maploadedalien.kfc",
    "maploadedpredator.kfc",
    "loadscreen.kfc",
    "layoutparams.kfc",
};

int avp_pack_is_mission_list(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_mission_lists / sizeof k_mission_lists[0]; i++)
        if (!strcmp(name, k_mission_lists[i]))
            return 1;
    return 0;
}

int avp_pack_list_name(const uint8_t *list, size_t n, char *out, size_t cap)
{
    size_t pos = 4;
    out[0] = 0;
    if (n < 4 || memcmp(list, "XBOX", 4))
        return 0;
    while (pos + 8 <= n) {
        uint32_t sz = rd32(list + pos + 4);
        size_t body = pos + 8;
        if (sz > n - body)
            return 0;
        if (!memcmp(list + pos, "EXTA", 4)) {
            size_t i;
            for (i = 0; i + 3 < sz; i++) {
                const uint8_t *s = list + body + i;
                if ((s[0] == 'P' || s[0] == 'p') && s[1] == ':' && s[2] == '\\') {
                    size_t k = 0, start = 0, j;
                    for (j = i; j < sz && list[body + j]; j++)
                        if (list[body + j] == '\\')
                            start = j + 1;
                    for (j = start; j < sz && list[body + j] && k + 1 < cap; j++)
                        out[k++] = (char)tolower(list[body + j]);
                    out[k] = 0;
                    return 1;
                }
            }
            return 0;
        }
        pos = body + sz + (sz & 1u);
    }
    return 0;
}

static uint32_t crc_name(const char *s)
{
    static uint32_t tab[256];
    uint32_t c = 0xFFFFFFFFu;
    if (!tab[1]) {
        uint32_t i, k, x;
        for (i = 0; i < 256; i++) {
            x = i;
            for (k = 0; k < 8; k++)
                x = (x & 1u) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            tab[i] = x;
        }
    }
    for (; *s; s++)
        c = tab[(c ^ (uint8_t)*s) & 0xFFu] ^ (c >> 8);
    return c;
}

static int lzss_unpack(const uint8_t *s, size_t n, uint8_t **out, size_t *outn)
{
    uint32_t claim, w;
    size_t p = 6, o = 0;
    int k = 0;
    uint32_t mask = 0;
    uint8_t *d;
    uint16_t win;
    if (n < 8)
        return 0;
    claim = rd32(s);
    if (claim > (64u << 20))
        return 0;
    d = malloc(claim ? claim : 1);
    if (!d)
        return 0;
    win = (uint16_t)(s[4] | s[5] << 8);
    w = win;
    while (!(w & 0x8000u) && k < 16) {
        w <<= 1;
        mask = (mask << 1) | 1u;
        k++;
    }
    while (p + 2 <= n) {
        uint16_t flags = (uint16_t)(s[p] | s[p + 1] << 8), bit;
        p += 2;
        for (bit = 0x8000u; bit; bit >>= 1) {
            uint16_t v;
            size_t cnt, src, i;
            if (p + 2 > n)
                goto done;
            v = (uint16_t)(s[p] | s[p + 1] << 8);
            p += 2;
            if (!(flags & bit)) {
                if (o + 2 > claim)
                    goto done;
                d[o++] = (uint8_t)v;
                d[o++] = (uint8_t)(v >> 8);
                continue;
            }
            if (!v)
                goto done;
            cnt = (size_t)(v & mask) * 2u;
            src = (size_t)(v >> k) * 2u;
            if (src > o || o - src > o || o + cnt > claim)
                goto done;
            src = o - src;
            for (i = 0; i < cnt; i++)
                d[o++] = d[src + i];
        }
    }
done:
    *out = d;
    *outn = o;
    return 1;
}

typedef struct {
    uint8_t *data;
    size_t n;
} Blob;

static const char *const k_mission_classes[] = {
    "GWinAction", "GLoseAction", "GGoalAction", "GLocationTrigger", "GActiveTrigger", "GDeathAllTrigger",
    "GNoMarinePotentialTrigger", "GPopulationTrigger", "GEnviroSwarm", "GAIPlayerAlien", "GAIPlayerMarine",
    "GAIPlayerPredator", "GAICompensate", "GSceneFader", "GXMarkerAction", "GMoveAction", "GObjectField",
    "GAlienDrone", "GAlienRunner", "GMarineInfantry", "GMarineMedic", "GMarineCommTech", "GWaypoint",
    "GWaypointManager", "GFenceNode", "GAIDefensePoint", "GAIMeetingPoint", "GSpawnerSuperAction",
    "GSpawnCreatureAction", "GNoQueenPotentialTrigger", "GNoPredatorPotentialTrigger", "GSkullCountTrigger",
    "GSkullCountGoalAction",
};

static int mission_class(uint32_t key)
{
    static uint32_t h[sizeof k_mission_classes / sizeof k_mission_classes[0]];
    size_t i;
    if (!h[0])
        for (i = 0; i < sizeof h / sizeof h[0]; i++)
            h[i] = crc_name(k_mission_classes[i]);
    for (i = 0; i < sizeof h / sizeof h[0]; i++)
        if (h[i] == key)
            return 1;
    return 0;
}

static int list_strip(const uint8_t *list, size_t n, uint8_t **out, size_t *outn, int *removed)
{
    size_t pos = 4, w = 4, type_at = 0, type_sz = 0, data_at = 0, data_sz = 0;
    int data_packed = 0;
    uint8_t *dst, *raw = NULL;
    size_t rawn = 0;
    uint32_t preset = crc_name("CPresetFillData"), cnt, i, keep = 0;
    uint8_t *ent;
    *removed = 0;
    while (pos + 8 <= n) {
        uint32_t sz = rd32(list + pos + 4);
        if (sz > n - pos - 8)
            return 0;
        if (!memcmp(list + pos, "TYPE", 4)) {
            type_at = pos + 8;
            type_sz = sz;
        } else if (!memcmp(list + pos, "DATX", 4) || !memcmp(list + pos, "DATA", 4)) {
            data_at = pos + 8;
            data_sz = sz;
            data_packed = list[pos + 3] == 'X';
        }
        pos += 8u + sz + (sz & 1u);
    }
    if (!type_at || !data_at || type_sz < 4)
        return 0;
    if (data_packed) {
        if (!lzss_unpack(list + data_at, data_sz, &raw, &rawn))
            return 0;
    } else {
        raw = (uint8_t *)list + data_at;
        rawn = data_sz;
    }
    cnt = rd32(list + type_at);
    if ((size_t)cnt * 8u + 4u > type_sz)
        cnt = (uint32_t)((type_sz - 4u) / 8u);
    ent = malloc((size_t)cnt * 8u + 4u);
    dst = malloc(n);
    if (!ent || !dst) {
        free(ent);
        free(dst);
        if (data_packed)
            free(raw);
        return 0;
    }
    for (i = 0; i < cnt; i++) {
        uint32_t h = rd32(list + type_at + 4u + i * 8u), o = rd32(list + type_at + 8u + i * 8u), at = o & 0x0FFFFFFFu;
        if (h == preset && at + 8u <= rawn && mission_class(rd32(raw + at + 4u))) {
            (*removed)++;
            continue;
        }
        wr32(ent + 4u + keep * 8u, h);
        wr32(ent + 8u + keep * 8u, o);
        keep++;
    }
    wr32(ent, keep);
    pos = 4;
    memcpy(dst, list, 4);
    while (pos + 8 <= n) {
        uint32_t sz = rd32(list + pos + 4);
        size_t total = 8u + (size_t)sz + (sz & 1u);
        if (pos + 8 == type_at) {
            size_t nsz = 4u + (size_t)keep * 8u;
            memcpy(dst + w, "TYPE", 4);
            wr32(dst + w + 4, (uint32_t)nsz);
            memcpy(dst + w + 8, ent, nsz);
            w += 8u + nsz;
        } else {
            memcpy(dst + w, list + pos, total);
            w += total;
        }
        pos += total;
    }
    free(ent);
    if (data_packed)
        free(raw);
    *out = dst;
    *outn = w;
    return 1;
}

static int s_stripped;

int avp_pack_last_stripped(void)
{
    return s_stripped;
}

int avp_pack_compose(const uint8_t *src, size_t n, uint8_t **out, size_t *outn, int *kept, int *dropped)
{
    int *stripped = &s_stripped;
    s_stripped = 0;
    size_t pos = TBD_HEADER, w = TBD_HEADER;
    uint8_t *dst;
    int k = 0, d = 0;
    *out = NULL;
    *outn = 0;
    if (n < TBD_HEADER || memcmp(src, "RIFF", 4) || memcmp(src + 8, "TBD2", 4))
        return 0;
    dst = malloc(n);
    if (!dst)
        return 0;
    memcpy(dst, src, TBD_HEADER);
    while (pos + 8 <= n) {
        uint32_t sz = rd32(src + pos + 4);
        size_t total = 8u + (size_t)sz + (sz & 1u);
        int drop = 0;
        if (sz > n - pos - 8) {
            free(dst);
            return 0;
        }
        if (!memcmp(src + pos, "LIST", 4)) {
            char name[64];
            if (avp_pack_list_name(src + pos + 8, sz, name, sizeof name) && avp_pack_is_mission_list(name))
                drop = 1;
            if (drop)
                d++;
            else
                k++;
        }
        if (!drop) {
            size_t take = pos + total > n ? n - pos : total;
            uint8_t *nl = NULL;
            size_t nln = 0;
            int rm = 0;
            char name[64];
            if (!memcmp(src + pos, "LIST", 4) && avp_pack_list_name(src + pos + 8, sz, name, sizeof name)
                && (!strncmp(name, "envlight_", 9) || !strncmp(name, "levelfadein", 11))
                && list_strip(src + pos + 8, sz, &nl, &nln, &rm) && rm > 0) {
                memcpy(dst + w, "LIST", 4);
                wr32(dst + w + 4, (uint32_t)nln);
                memcpy(dst + w + 8, nl, nln);
                if (nln & 1u)
                    dst[w + 8 + nln] = 0;
                w += 8u + nln + (nln & 1u);
                *stripped += rm;
            } else {
                memcpy(dst + w, src + pos, take);
                w += take;
            }
            free(nl);
        }
        pos += total;
    }
    wr32(dst + 4, (uint32_t)(w - 8));
    *out = dst;
    *outn = w;
    if (kept)
        *kept = k;
    if (dropped)
        *dropped = d;
    return 1;
}

static const char *stock_code(AvpRace race)
{
    return race == AVP_RACE_ALIEN ? "A01" : race == AVP_RACE_PREDATOR ? "P01" : "M01";
}

int avp_pack_race(const char *code, AvpRace *race)
{
    if (!code || strlen(code) != AVP_PACK_CODE_LEN || code[1] != '9' || code[2] != '0')
        return 0;
    if (code[0] == 'A')
        *race = AVP_RACE_ALIEN;
    else if (code[0] == 'P')
        *race = AVP_RACE_PREDATOR;
    else if (code[0] == 'M')
        *race = AVP_RACE_MARINE;
    else
        return 0;
    return 1;
}

int avp_pack_code(AvpRace race, char code[AVP_PACK_CODE_LEN + 1])
{
    code[0] = race == AVP_RACE_ALIEN ? 'A' : race == AVP_RACE_PREDATOR ? 'P' : 'M';
    code[1] = '9';
    code[2] = '0';
    code[3] = 0;
    return 1;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int spit(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0777);
#endif
}

int avp_pack_build(AvpRace race, char code[AVP_PACK_CODE_LEN + 1])
{
    char from[128], to[160], dir[128];
    const char *stock = stock_code(race);
    uint8_t *src, *out = NULL;
    size_t n = 0, on = 0;
    int kept = 0, dropped = 0, ok;
    avp_pack_code(race, code);
    snprintf(from, sizeof from, "game/tbd/%s/FD090663.tbx", stock);
    src = slurp(from, &n);
    if (!src) {
        fprintf(stderr, "[PACK] stock level data %s missing\n", from);
        return 0;
    }
    ok = avp_pack_compose(src, n, &out, &on, &kept, &dropped);
    free(src);
    if (!ok) {
        fprintf(stderr, "[PACK] %s is not a TBD2 file\n", from);
        return 0;
    }
    snprintf(dir, sizeof dir, "game/tbd/%s", code);
    make_dir(dir);
    snprintf(to, sizeof to, "%s/FD090663.tbx", dir);
    ok = spit(to, out, on);
    free(out);
    if (!ok) {
        fprintf(stderr, "[PACK] cannot write %s\n", to);
        return 0;
    }
    snprintf(from, sizeof from, "game/tbd/%s/06F4F187.tbx", stock);
    snprintf(to, sizeof to, "%s/06F4F187.tbx", dir);
    src = slurp(from, &n);
    if (src) {
        spit(to, src, n);
        free(src);
    }
    fprintf(stderr, "[PACK] %s built from %s: %d level data blocks kept, %d dropped, %d scripted objects removed (%s)\n", code,
            stock, kept, dropped, avp_pack_last_stripped(), PACK_VERSION_TAG);
    return 1;
}
