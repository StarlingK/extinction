#ifndef AVP_KEYS_H
#define AVP_KEYS_H

#include <stdio.h>

enum {
    AVP_ACT_A, AVP_ACT_B, AVP_ACT_X, AVP_ACT_Y, AVP_ACT_WHITE, AVP_ACT_BLACK,
    AVP_ACT_LTRIGGER, AVP_ACT_RTRIGGER, AVP_ACT_START, AVP_ACT_BACK,
    AVP_ACT_DPAD_UP, AVP_ACT_DPAD_DOWN, AVP_ACT_DPAD_LEFT, AVP_ACT_DPAD_RIGHT,
    AVP_ACT_LTHUMB, AVP_ACT_RTHUMB,
    AVP_ACT_LSTICK_UP, AVP_ACT_LSTICK_DOWN, AVP_ACT_LSTICK_LEFT, AVP_ACT_LSTICK_RIGHT,
    AVP_ACT_RSTICK_LEFT, AVP_ACT_RSTICK_RIGHT, AVP_ACT_RSTICK_UP, AVP_ACT_RSTICK_DOWN,
    AVP_PAD_ACTIONS,
    AVP_ACT_PAN_UP = AVP_PAD_ACTIONS, AVP_ACT_PAN_LEFT, AVP_ACT_PAN_DOWN, AVP_ACT_PAN_RIGHT,
    AVP_ACT_GIVE, AVP_ACT_SETTINGS,
    AVP_ACT_GROUP1,
    AVP_ACT_GROUP9 = AVP_ACT_GROUP1 + 8,
    AVP_ACT_GROUP_STORE,
    AVP_ACTION_COUNT
};

int avp_keys_vk(int action, int slot);
const char *avp_keys_name(int action);
const char *avp_keys_ini_name(int action);
int avp_keys_is_pad(int action);
int avp_keys_rebindable(int action);

void avp_pad_set(int pad_action, int down);
int avp_action_down(int action);
int avp_keys_match(int action, int vk);
int avp_group_key_pressed(int *group, int *store);

void avp_keys_apply(void);
int avp_keys_conflict(int action, int vk);
int avp_keys_bind(int action, int slot, int vk);
void avp_keys_reset_defaults(void);

void avp_keys_save(FILE *f);
int avp_keys_load_line(const char *ini_name, const char *value);

const char *avp_vk_name(int vk);

#endif
