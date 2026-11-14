#ifndef AVP_LAYOUT_H
#define AVP_LAYOUT_H

#include <stddef.h>

typedef enum { AVP_RACE_ALIEN, AVP_RACE_MARINE, AVP_RACE_PREDATOR, AVP_RACE_COUNT } AvpRace;
typedef enum { AVP_AFF_SLOT, AVP_AFF_NEUTRAL, AVP_AFF_SPAWN } AvpAffKind;
typedef struct { AvpAffKind kind; int slot; AvpRace race; int n; } AvpAff;
typedef struct { AvpRace race; int n; float x, y, z, rot; int used; } AvpSpawn;
typedef struct { char cls[48]; AvpAff aff; float x, y, z, rot; } AvpUnitRec;

#define AVP_LAYOUT_MAX_UNITS 512
#define AVP_LAYOUT_MAX_EXTRA 64

typedef struct {
    char map[16];
    AvpSpawn spawn[AVP_RACE_COUNT][3];
    int nunits;
    AvpUnitRec unit[AVP_LAYOUT_MAX_UNITS];
    int nextra;
    char extra[AVP_LAYOUT_MAX_EXTRA][160];
} AvpLayout;

void avp_layout_init(AvpLayout *l, const char *map);
int  avp_layout_parse(AvpLayout *l, const char *text);
int  avp_layout_load(AvpLayout *l, const char *map);
int  avp_layout_save(const AvpLayout *l);
int  avp_layout_write(const AvpLayout *l, char *buf, size_t n);
const char *avp_race_name(AvpRace r);
int  avp_race_parse(const char *s, AvpRace *out);
int  avp_aff_parse(const char *s, AvpAff *out);
void avp_aff_format(const AvpAff *a, char *buf, size_t n);
int  avp_layout_add_unit(AvpLayout *l, const AvpUnitRec *u);
void avp_layout_remove_unit(AvpLayout *l, int i);

#endif
