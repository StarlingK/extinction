#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "avp_pack.h"

static int s_fail;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            s_fail++; \
        } \
    } while (0)

static size_t put_chunk(uint8_t *d, const char *id, const void *body, uint32_t n)
{
    memcpy(d, id, 4);
    d[4] = (uint8_t)n; d[5] = (uint8_t)(n >> 8); d[6] = (uint8_t)(n >> 16); d[7] = (uint8_t)(n >> 24);
    memcpy(d + 8, body, n);
    if (n & 1u)
        d[8 + n] = 0;
    return 8u + n + (n & 1u);
}

static size_t put_list(uint8_t *d, const char *src)
{
    uint8_t body[256], exta[128];
    size_t k = 4, n = 0;
    memcpy(body, "XBOX", 4);
    if (src) {
        n = strlen(src) + 1;
        memcpy(exta, "\x01\x02", 2);
        memcpy(exta + 2, src, n);
        n += 2;
    } else {
        memcpy(exta, "\x01", 1);
        n = 1;
    }
    k += put_chunk(body + k, "TYPE", "\0\0\0\0", 4);
    k += put_chunk(body + k, "EXTA", exta, (uint32_t)n);
    return put_chunk(d, "LIST", body, (uint32_t)k);
}

int main(void)
{
    static const char *const names[] = {
        "P:\\avp\\art\\maps\\M01\\Objective.kfc", "P:\\avp\\code\\TBDScripts\\MarineUIStuff.kfc",
        "P:\\avp\\art\\maps\\M01\\map.kfc", "P:\\avp\\art\\maps\\M01\\MapLoaded.kfc",
        "P:\\avp\\code\\tbdscripts\\EnvLight_DesertDay.tss", "P:\\avp\\art\\maps\\M01\\LayoutParams.kfc", NULL
    };
    uint8_t *src = calloc(1, 4096), *out = NULL;
    size_t n = 12, on = 0, i;
    int kept = 0, dropped = 0;
    char name[64], code[4];
    AvpRace r;
    memcpy(src, "RIFF", 4);
    memcpy(src + 8, "TBD2", 4);
    for (i = 0; i < 7; i++)
        n += put_list(src + n, names[i]);
    src[4] = (uint8_t)(n - 8);
    src[5] = (uint8_t)((n - 8) >> 8);
    CHECK(avp_pack_compose(src, n, &out, &on, &kept, &dropped));
    CHECK(kept == 4 && dropped == 3);
    CHECK(on < n && on > 12 && (on & 1u) == 0);
    CHECK(out && !memcmp(out, "RIFF", 4) && (size_t)(out[4] | out[5] << 8) == on - 8);
    {
        size_t pos = 12, lists = 0;
        while (pos + 8 <= on) {
            uint32_t sz = (uint32_t)out[pos + 4] | (uint32_t)out[pos + 5] << 8;
            CHECK(!memcmp(out + pos, "LIST", 4));
            avp_pack_list_name(out + pos + 8, sz, name, sizeof name);
            CHECK(!avp_pack_is_mission_list(name));
            lists++;
            pos += 8u + sz + (sz & 1u);
        }
        CHECK(lists == 4 && pos == on);
    }
    free(out);
    CHECK(!avp_pack_compose((const uint8_t *)"not a riff file", 15, &out, &on, NULL, NULL));
    CHECK(avp_pack_code(AVP_RACE_ALIEN, code) && !strcmp(code, "A90"));
    CHECK(avp_pack_code(AVP_RACE_PREDATOR, code) && !strcmp(code, "P90"));
    CHECK(avp_pack_code(AVP_RACE_MARINE, code) && !strcmp(code, "M90"));
    CHECK(avp_pack_race("P90", &r) && r == AVP_RACE_PREDATOR);
    CHECK(!avp_pack_race("M01", &r) && !avp_pack_race("X90", &r));
    free(src);
    printf(s_fail ? "%d failures\n" : "pack tests passed\n", s_fail);
    return s_fail != 0;
}
