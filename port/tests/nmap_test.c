#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avp_nmap.h"

static int s_fail;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            s_fail++; \
        } \
    } while (0)

static void test_starter(void)
{
    Nmap *m = malloc(sizeof *m), *r = malloc(sizeof *r);
    NmapTerrain t;
    char *buf = malloc(1 << 20);
    int i, n, spawns = 0, atmos = 0;
    CHECK(nmap_load(m, "Nostromo_Flats") == 0);
    CHECK(!strcmp(m->base, "M01"));
    CHECK(!m->relative && m->tw == 230 && m->th == 230);
    for (i = 0; i < NMAP_MAX_SPAWN; i++)
        spawns += m->spawn[i].used;
    CHECK(spawns == 4);
    CHECK(m->nwild == 2 && !strcmp(m->wild[0].cls, "GRynth") && m->wild[0].max == 4 && m->wild[0].delay == 60.0f);
    for (i = 0; i < m->nobj; i++)
        atmos += nmap_is_atmos(m->obj[i].cls);
    CHECK(atmos == 2);
    CHECK(m->spawn[0].x > 0 && m->spawn[0].x < 230 && m->spawn[2].y > 115);
    CHECK(nmap_terrain_load(&t, "Nostromo_Flats") && t.w == 230 && t.h == 230);
    CHECK(t.splat[(115 * 230 + 115) * NMAP_LAYERS + 3] > 0 || t.splat[(115 * 230 + 115) * NMAP_LAYERS] > 0);
    nmap_terrain_free(&t);
    n = nmap_write(m, buf, 1 << 20);
    CHECK(n > 0);
    nmap_init(r, "");
    CHECK(nmap_parse(r, buf) == 0);
    CHECK(r->nobj == m->nobj && r->nwild == 2 && r->tw == 230 && !strcmp(r->base, "M01"));
    free(buf);
    free(m);
    free(r);
}

static void test_old_relative(void)
{
    Nmap *m = malloc(sizeof *m);
    nmap_init(m, "");
    CHECK(nmap_parse(m, "nmap 1\nbase M01\nrelative 1\nbounds 230 230\nspawn 1 0.5 0.25 0 0\nobject @Cactus1 neutral 0.1 0.2 0 1\n") == 0);
    CHECK(m->relative && m->tw == 0);
    nmap_resolve(m, 230.0f, 230.0f, 5.0f);
    CHECK(!m->relative && m->spawn[0].x == 115.0f && m->spawn[0].y == 57.5f && m->obj[0].z == 5.0f);
    CHECK(nmap_parse(m, "wildlife GKurn 10 20 0 3 30 8\n") == 0 && m->nwild == 1 && m->wild[0].max == 3);
    CHECK(nmap_parse(m, "wildlife GKurn 10 20\n") != 0);
    CHECK(nmap_parse(m, "terrain 4 4\n") != 0);
    free(m);
}

static void test_terrain_io(void)
{
    NmapTerrain a, b;
    int i;
    CHECK(nmap_terrain_alloc(&a, 17, 9));
    for (i = 0; i < 17 * 9; i++) {
        a.z[i] = (int16_t)(i * 7 - 300);
        a.splat[i * NMAP_LAYERS + (i & 3)] = 200;
    }
    CHECK(nmap_terrain_save(&a, "_t_io"));
    CHECK(nmap_terrain_load(&b, "_t_io") && b.w == 17 && b.h == 9);
    CHECK(!memcmp(a.z, b.z, 17 * 9 * 2) && !memcmp(a.splat, b.splat, 17 * 9 * NMAP_LAYERS));
    nmap_terrain_free(&a);
    nmap_terrain_free(&b);
    remove("maps/_t_io.nterr");
}

static void test_terrain_v2(void)
{
    NmapTerrain a, b;
    FILE *f;
    uint8_t hdr[12] = { 'N', 'T', 'R', '1', 8, 0, 0, 0, 8, 0, 0, 0 };
    uint8_t body[8 * 8 * 7];
    int i;
    CHECK(nmap_terrain_alloc(&a, 20, 11));
    a.hmin = -30;
    a.hmax = 150;
    a.hstep = 25;
    a.hlevels = 5;
    for (i = 0; i < 20 * 11; i++)
        a.flags[i] = (uint8_t)(i % 31);
    CHECK(nmap_terrain_save(&a, "_t_v2"));
    CHECK(nmap_terrain_load(&b, "_t_v2") && b.w == 20 && b.h == 11);
    CHECK(b.hmin == -30 && b.hmax == 150 && b.hstep == 25 && b.hlevels == 5);
    CHECK(!memcmp(a.flags, b.flags, 20 * 11));
    nmap_terrain_free(&a);
    nmap_terrain_free(&b);
    remove("maps/_t_v2.nterr");
    memset(body, 0, sizeof body);
    f = fopen("maps/_t_v1.nterr", "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(hdr, 1, 12, f);
        fwrite(body, 1, sizeof body, f);
        fclose(f);
        CHECK(nmap_terrain_load(&b, "_t_v1") && b.w == 8 && b.h == 8);
        CHECK(b.hmin == NMAP_HMIN_DEF && b.hmax == NMAP_HMAX_DEF && b.hstep == NMAP_HSTEP_DEF && b.flags[5] == 0);
        nmap_terrain_free(&b);
        remove("maps/_t_v1.nterr");
    }
}

static void test_resample(void)
{
    NmapTerrain a, b;
    Nmap *m = malloc(sizeof *m);
    int x, y, sizes[4] = { 64, 100, 230, 256 }, k;
    CHECK(nmap_terrain_alloc(&a, 128, 128));
    for (y = 0; y < 128; y++)
        for (x = 0; x < 128; x++) {
            a.z[y * 128 + x] = (int16_t)(x * 4);
            a.splat[(y * 128 + x) * NMAP_LAYERS + 1] = 200;
        }
    a.flags[0] = NMAP_CF_IMPASSABLE;
    a.flags[127 * 128 + 127] = NMAP_CF_CAVE;
    for (k = 0; k < 4; k++) {
        CHECK(nmap_terrain_resample(&a, &b, sizes[k], sizes[3 - k]));
        CHECK(b.w == sizes[k] && b.h == sizes[3 - k]);
        CHECK(b.z[0] == 0 && b.z[b.w - 1] == 127 * 4);
        CHECK(b.flags && b.flags[0] == NMAP_CF_IMPASSABLE && b.flags[b.w * b.h - 1] == NMAP_CF_CAVE);
        CHECK(b.z[(b.h - 1) * b.w + b.w / 2] > 0 && b.splat[NMAP_LAYERS + 1] == 200);
        nmap_terrain_free(&b);
    }
    nmap_terrain_free(&a);
    nmap_init(m, "");
    m->tw = m->th = 128;
    m->nobj = 1;
    snprintf(m->obj[0].cls, sizeof m->obj[0].cls, "@Cactus1");
    m->obj[0].x = 64.0f;
    m->obj[0].y = 32.0f;
    m->spawn[0].used = 1;
    m->spawn[0].x = 0.0f;
    m->spawn[0].y = 128.0f;
    nmap_to_world(m, 10.0f, 20.0f, 256.0f, 256.0f, 3.0f);
    CHECK(m->obj[0].x == 138.0f && m->obj[0].y == 84.0f);
    CHECK(m->spawn[0].x == 13.0f && m->spawn[0].y == 273.0f);
    free(m);
}

static void test_meta(void)
{
    Nmap *m = malloc(sizeof *m), *r = malloc(sizeof *r);
    char *buf = malloc(1 << 16);
    nmap_init(m, "M01");
    snprintf(m->title, sizeof m->title, "My Map");
    snprintf(m->author, sizeof m->author, "Someone");
    snprintf(m->desc, sizeof m->desc, "Two words, and a  long line");
    m->minp = 2;
    m->maxp = 4;
    m->slot[0].used = 1; m->slot[0].race = 1; m->slot[0].team = 1;
    m->slot[2].used = 1; m->slot[2].race = -1; m->slot[2].team = 2;
    CHECK(nmap_write(m, buf, 1 << 16) > 0);
    nmap_init(r, "");
    CHECK(nmap_parse(r, buf) == 0);
    CHECK(!strcmp(r->title, "My Map") && !strcmp(r->author, "Someone") && !strcmp(r->desc, m->desc));
    CHECK(r->minp == 2 && r->maxp == 4);
    CHECK(r->slot[0].used && r->slot[0].race == 1 && r->slot[0].team == 1);
    CHECK(r->slot[2].used && r->slot[2].race == -1 && r->slot[2].team == 2 && !r->slot[1].used);
    nmap_init(r, "");
    CHECK(nmap_parse(r, "nmap 1\nbase M01\nfuturekey 1 2\nobject @Cactus1 neutral 1 2 0 0\n") == 1);
    CHECK(r->nobj == 1 && !r->title[0] && r->minp == 0);
    CHECK(nmap_parse(r, "slot 9 alien 1\n") != 0 && nmap_parse(r, "players 5 2\n") != 0);
    free(buf);
    free(m);
    free(r);
}

int main(void)
{
    test_meta();
    test_starter();
    test_old_relative();
    test_terrain_io();
    test_terrain_v2();
    test_resample();
    printf(s_fail ? "%d failures\n" : "nmap tests passed\n", s_fail);
    return s_fail != 0;
}
