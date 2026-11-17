#ifndef AVP_COOP_H
#define AVP_COOP_H

int avp_mission_count(void);
const char *avp_mission_code(int i);
const char *avp_mission_title(int i);
int avp_mission_race(int i);
int avp_coop_host_launch(int mission);
int avp_coop_host_pick(int mission);
int avp_coop_peer_ready(void);
int avp_coop_self_ready(void);
int avp_coop_set_ready(int on);
const char *avp_coop_guest_mission(void);
void avp_load_mission(int mission);
void avp_load_level_code(const char *code);
void avp_coop_reset(void);

#endif
