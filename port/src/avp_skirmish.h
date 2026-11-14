#ifndef AVP_SKIRMISH_H
#define AVP_SKIRMISH_H

#include <stdint.h>
#include "avp_layout.h"

#define AVP_SKIRMISH_MAX_PLAYERS 8
#define AVP_THUMB_W 192
#define AVP_THUMB_H 144

typedef enum { AVP_PK_HUMAN, AVP_PK_AI, AVP_PK_REMOTE } AvpPlayerKind;

typedef struct {
    int slot;
    AvpPlayerKind kind;
    AvpRace race;
    int spawn;
    int team;
} AvpSkirmishPlayer;

typedef struct {
    char map[16];
    int nplayers;
    AvpSkirmishPlayer player[AVP_SKIRMISH_MAX_PLAYERS];
} AvpSkirmishCfg;

int  avp_skirmish_config_load(AvpSkirmishCfg *c);
int  avp_skirmish_config_save(const AvpSkirmishCfg *c);
void avp_skirmish_arm(int editor);
void avp_skirmish_disarm(void);
int  avp_skirmish_armed(void);
int  avp_skirmish_active(void);
int  avp_skirmish_editor(void);
const char *avp_skirmish_cur_map(void);
void avp_skirmish_capture(const uint32_t *rgb, int w, int h);
int  avp_skirmish_thumb_load(const char *code, uint32_t *px);
int  avp_skirmish_block(uint32_t fn);
void avp_skirmish_level(uint32_t name);
void avp_skirmish_fill_trace(uint32_t name);
uint32_t avp_skirmish_spawn(const char *cls, int slot, float x, float y, float z, float rot);
void avp_skirmish_mission_start(void);
void avp_skirmish_tick(void);
void avp_skirmish_draw(uint32_t *rgb, int w, int h);

#endif
