#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "avp_layout.h"

static int s_fail;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            printf("FAIL line %d: %s\n", __LINE__, #c); \
            s_fail++; \
        } \
    } while (0)

static void test_aff(void)
{
    AvpAff a;
    char b[32];
    int i;
    for (i = 1; i <= 8; i++) {
        char s[16];
        snprintf(s, sizeof s, "slot%d", i);
        CHECK(avp_aff_parse(s, &a) && a.kind == AVP_AFF_SLOT && a.slot == i);
        avp_aff_format(&a, b, sizeof b);
        CHECK(!strcmp(b, s));
    }
    CHECK(avp_aff_parse("neutral", &a) && a.kind == AVP_AFF_NEUTRAL && a.slot == 9);
    avp_aff_format(&a, b, sizeof b);
    CHECK(!strcmp(b, "neutral"));
    CHECK(avp_aff_parse("spawn:marine:2", &a) && a.kind == AVP_AFF_SPAWN && a.race == AVP_RACE_MARINE && a.n == 2);
    avp_aff_format(&a, b, sizeof b);
    CHECK(!strcmp(b, "spawn:marine:2"));
    CHECK(avp_aff_parse("spawn:alien:1", &a) && a.race == AVP_RACE_ALIEN);
    CHECK(avp_aff_parse("spawn:predator:3", &a) && a.race == AVP_RACE_PREDATOR && a.n == 3);
    CHECK(!avp_aff_parse("slot0", &a));
    CHECK(!avp_aff_parse("slot9", &a));
    CHECK(!avp_aff_parse("slot", &a));
    CHECK(!avp_aff_parse("slot1x", &a));
    CHECK(!avp_aff_parse("neutralx", &a));
    CHECK(!avp_aff_parse("spawn:marine:4", &a));
    CHECK(!avp_aff_parse("spawn:human:1", &a));
    CHECK(!avp_aff_parse("spawn:marine", &a));
    CHECK(!avp_aff_parse("spawn:marine:1:2", &a));
    CHECK(!avp_aff_parse("", &a));
    CHECK(!avp_aff_parse(NULL, &a));
}

static void test_race(void)
{
    AvpRace r;
    CHECK(avp_race_parse("alien", &r) && r == AVP_RACE_ALIEN);
    CHECK(avp_race_parse("Marine", &r) && r == AVP_RACE_MARINE);
    CHECK(avp_race_parse("PREDATOR", &r) && r == AVP_RACE_PREDATOR);
    CHECK(!avp_race_parse("alienx", &r));
    CHECK(!avp_race_parse("", &r));
    CHECK(!strcmp(avp_race_name(AVP_RACE_MARINE), "marine"));
}

static const char *k_full =
    "# comment\r\n"
    "layout 1\r\n"
    "map M03\r\n"
    "\r\n"
    "spawn alien 1 1.5 2 -3.25 0.5\r\n"
    "spawn marine 3 100 0 -100 3.14159\n"
    "spawn predator 2 0 0 0 0\n"
    "unit GMarineInfantry slot1 10 0 20 1\n"
    "unit GAlienDrone slot8 -10 0.5 20 -1\n"
    "unit GEgg neutral 1 2 3 4\n"
    "unit GPredatorWarrior spawn:predator:2 5 6 7 8\n"
    "future thing 1 2 3\n"
    "other\n";

static void test_roundtrip(void)
{
    AvpLayout a, b;
    char buf1[16384], buf2[16384];
    int n1, n2;
    avp_layout_init(&a, NULL);
    CHECK(avp_layout_parse(&a, k_full) == 0);
    CHECK(!strcmp(a.map, "M03"));
    CHECK(a.spawn[AVP_RACE_ALIEN][0].used && a.spawn[AVP_RACE_ALIEN][0].x == 1.5f && a.spawn[AVP_RACE_ALIEN][0].z == -3.25f);
    CHECK(a.spawn[AVP_RACE_MARINE][2].used && !a.spawn[AVP_RACE_MARINE][0].used);
    CHECK(a.spawn[AVP_RACE_PREDATOR][1].used);
    CHECK(a.nunits == 4);
    CHECK(!strcmp(a.unit[0].cls, "GMarineInfantry") && a.unit[0].aff.slot == 1);
    CHECK(a.unit[1].aff.kind == AVP_AFF_SLOT && a.unit[1].aff.slot == 8);
    CHECK(a.unit[2].aff.kind == AVP_AFF_NEUTRAL);
    CHECK(a.unit[3].aff.kind == AVP_AFF_SPAWN && a.unit[3].aff.race == AVP_RACE_PREDATOR && a.unit[3].aff.n == 2);
    CHECK(a.nextra == 2 && !strcmp(a.extra[0], "future thing 1 2 3") && !strcmp(a.extra[1], "other"));
    n1 = avp_layout_write(&a, buf1, sizeof buf1);
    CHECK(n1 > 0 && (size_t)n1 == strlen(buf1));
    avp_layout_init(&b, NULL);
    CHECK(avp_layout_parse(&b, buf1) == 0);
    n2 = avp_layout_write(&b, buf2, sizeof buf2);
    CHECK(n1 == n2 && !strcmp(buf1, buf2));
    CHECK(b.nunits == 4 && b.nextra == 2);
    CHECK(strstr(buf1, "unit GPredatorWarrior spawn:predator:2 5 6 7 8\n") != NULL);
    CHECK(strstr(buf1, "future thing 1 2 3\n") != NULL);
    CHECK(avp_layout_write(&a, buf2, 10) == -1);
    CHECK(avp_layout_write(&a, buf2, 0) == -1);
}

static void test_bad(void)
{
    AvpLayout l;
    avp_layout_init(&l, "X");
    CHECK(avp_layout_parse(&l,
        "spawn alien 4 0 0 0 0\n"
        "spawn human 1 0 0 0 0\n"
        "spawn alien 1 0 0 0\n"
        "spawn alien 1 a 0 0 0\n"
        "unit GEgg slot9 0 0 0 0\n"
        "unit GEgg slot1 0 0 0\n"
        "unit GEgg slot1 0 0 0 0 0\n"
        "unit\n"
        "map bad/name\n"
        "layout 2\n"
        "unit GEgg slot2 1 1 1 1\n") == 10);
    CHECK(l.nunits == 1 && !l.spawn[0][0].used && !strcmp(l.map, "X"));
    CHECK(avp_layout_parse(&l, "   \n\t\n# x\n") == 0);
    CHECK(avp_layout_parse(&l, "unit GEgg slot2 1 1 1 1") == 0 && l.nunits == 2);
    CHECK(avp_layout_parse(&l, NULL) == 0);
}

static void test_limits(void)
{
    AvpLayout *l = malloc(sizeof *l);
    AvpUnitRec u;
    char line[64], longcls[100];
    int i;
    avp_layout_init(l, "LIM");
    memset(&u, 0, sizeof u);
    strcpy(u.cls, "GEgg");
    u.aff.kind = AVP_AFF_NEUTRAL;
    for (i = 0; i < AVP_LAYOUT_MAX_UNITS; i++)
        CHECK(avp_layout_add_unit(l, &u));
    CHECK(!avp_layout_add_unit(l, &u));
    CHECK(avp_layout_parse(l, "unit GEgg slot1 0 0 0 0\n") == 1 && l->nunits == AVP_LAYOUT_MAX_UNITS);
    avp_layout_remove_unit(l, -1);
    avp_layout_remove_unit(l, AVP_LAYOUT_MAX_UNITS);
    CHECK(l->nunits == AVP_LAYOUT_MAX_UNITS);
    strcpy(l->unit[1].cls, "Second");
    avp_layout_remove_unit(l, 0);
    CHECK(l->nunits == AVP_LAYOUT_MAX_UNITS - 1 && !strcmp(l->unit[0].cls, "Second"));
    avp_layout_init(l, "LIM");
    for (i = 0; i < AVP_LAYOUT_MAX_EXTRA; i++) {
        snprintf(line, sizeof line, "x%d\n", i);
        CHECK(avp_layout_parse(l, line) == 0);
    }
    CHECK(avp_layout_parse(l, "overflow\n") == 1 && l->nextra == AVP_LAYOUT_MAX_EXTRA);
    memset(longcls, 'A', 60);
    strcpy(longcls + 60, " slot1 0 0 0 0\n");
    {
        char t[128];
        snprintf(t, sizeof t, "unit %s", longcls);
        avp_layout_init(l, "LIM");
        CHECK(avp_layout_parse(l, t) == 1 && l->nunits == 0);
    }
    free(l);
}

static void test_file(void)
{
    AvpLayout a, b;
    AvpUnitRec u;
    int r;
    avp_layout_init(&a, "T99");
    a.spawn[AVP_RACE_MARINE][0].used = 1;
    a.spawn[AVP_RACE_MARINE][0].x = 12.125f;
    memset(&u, 0, sizeof u);
    strcpy(u.cls, "GAlienDrone");
    u.aff.kind = AVP_AFF_SLOT;
    u.aff.slot = 2;
    u.x = 0.1f;
    u.rot = 1.2345678f;
    avp_layout_add_unit(&a, &u);
    strcpy(a.extra[0], "keepme 1");
    a.nextra = 1;
    CHECK(avp_layout_save(&a));
    r = avp_layout_load(&b, "T99");
    CHECK(r == 0 && b.nunits == 1 && b.unit[0].x == 0.1f && b.unit[0].rot == 1.2345678f);
    CHECK(b.spawn[AVP_RACE_MARINE][0].x == 12.125f && b.nextra == 1);
    CHECK(avp_layout_load(&b, "NOPE_NOT_THERE") == -1);
    CHECK(avp_layout_load(&b, "../evil") == -1);
    remove("maps/T99.layout");
}

int main(void)
{
    test_aff();
    test_race();
    test_roundtrip();
    test_bad();
    test_limits();
    test_file();
    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
