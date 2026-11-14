#ifndef AVP_EDITOR_H
#define AVP_EDITOR_H

#include "avp_platform.h"
#include <stdint.h>

int  avp_editor_active(void);
int  avp_editor_frame(int fx, int fy, int buttons, int wheel);
void avp_editor_draw(uint32_t *rgb, int w, int h);
int  avp_editor_key(UINT msg, WPARAM w, LPARAM l);
const uint8_t *avp_font5x7(int ch);

#endif
