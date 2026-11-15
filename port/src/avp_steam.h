#ifndef AVP_STEAM_H
#define AVP_STEAM_H

#include <stdint.h>

enum { AVP_STEAM_OFF, AVP_STEAM_BUSY, AVP_STEAM_READY, AVP_STEAM_FAILED };

typedef struct {
    char name[32];
    uint64_t lobby;
} AvpSteamFriend;

int avp_steam_connect(void);
int avp_steam_unpack(const char *gamedir);
int avp_steam_status(void);
const char *avp_steam_text(void);
const char *avp_steam_error(void);

int avp_steam_host(volatile int *cancel, char *code, int cap);
int avp_steam_join_code(const char *code, volatile int *cancel);
int avp_steam_join_lobby(uint64_t lobby, volatile int *cancel);
int avp_steam_wait_peer(volatile int *cancel);
const char *avp_steam_peer_name(void);
const char *avp_steam_code(void);

int avp_steam_link_up(void);
int avp_steam_send(const void *head, uint32_t hn, const void *body, uint32_t bn);
int avp_steam_recv(void *p, int n);
void avp_steam_link_drop(void);
void avp_steam_leave(void);

void avp_steam_want_friends(int on);
int avp_steam_friends(AvpSteamFriend *out, int max);

void avp_steam_scan_cmdline(void);
int avp_steam_cmd_pending(void);
int avp_steam_take_join(uint64_t *lobby);
int avp_steam_valid_code(const char *c);

#endif
