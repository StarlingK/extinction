#ifndef AVP_PACK_H
#define AVP_PACK_H

#include <stddef.h>
#include <stdint.h>
#include "avp_layout.h"

#define AVP_PACK_CODE_LEN 3

int avp_pack_race(const char *code, AvpRace *race);
int avp_pack_code(AvpRace race, char code[AVP_PACK_CODE_LEN + 1]);
int avp_pack_compose(const uint8_t *src, size_t n, uint8_t **out, size_t *outn, int *kept, int *dropped);
int avp_pack_last_stripped(void);
int avp_pack_build(AvpRace race, char code[AVP_PACK_CODE_LEN + 1]);
int avp_pack_list_name(const uint8_t *list, size_t n, char *out, size_t cap);
int avp_pack_is_mission_list(const char *name);

#endif
