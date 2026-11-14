#ifndef XBOX_INPUT_KEYBOARD_H
#define XBOX_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_VK_LBUTTON   0x01
#define XBOX_VK_RBUTTON   0x02
#define XBOX_VK_MBUTTON   0x04
#define XBOX_VK_XBUTTON1  0x05
#define XBOX_VK_XBUTTON2  0x06
#define XBOX_VK_BACK      0x08
#define XBOX_VK_TAB       0x09
#define XBOX_VK_RETURN    0x0D
#define XBOX_VK_SHIFT     0x10
#define XBOX_VK_CONTROL   0x11
#define XBOX_VK_MENU      0x12
#define XBOX_VK_PAUSE     0x13
#define XBOX_VK_CAPITAL   0x14
#define XBOX_VK_ESCAPE    0x1B
#define XBOX_VK_SPACE     0x20
#define XBOX_VK_PRIOR     0x21
#define XBOX_VK_NEXT      0x22
#define XBOX_VK_END       0x23
#define XBOX_VK_HOME      0x24
#define XBOX_VK_LEFT      0x25
#define XBOX_VK_UP        0x26
#define XBOX_VK_RIGHT     0x27
#define XBOX_VK_DOWN      0x28
#define XBOX_VK_INSERT    0x2D
#define XBOX_VK_DELETE    0x2E
#define XBOX_VK_NUMPAD0   0x60
#define XBOX_VK_NUMPAD2   0x62
#define XBOX_VK_NUMPAD4   0x64
#define XBOX_VK_NUMPAD6   0x66
#define XBOX_VK_NUMPAD8   0x68
#define XBOX_VK_NUMPAD9   0x69
#define XBOX_VK_MULTIPLY  0x6A
#define XBOX_VK_ADD       0x6B
#define XBOX_VK_SUBTRACT  0x6D
#define XBOX_VK_DECIMAL   0x6E
#define XBOX_VK_DIVIDE    0x6F
#define XBOX_VK_F1        0x70
#define XBOX_VK_F12       0x7B
#define XBOX_VK_LSHIFT    0xA0
#define XBOX_VK_RSHIFT    0xA1
#define XBOX_VK_LCONTROL  0xA2
#define XBOX_VK_RCONTROL  0xA3
#define XBOX_VK_LMENU     0xA4
#define XBOX_VK_RMENU     0xA5
#define XBOX_VK_OEM_1     0xBA
#define XBOX_VK_OEM_PLUS  0xBB
#define XBOX_VK_OEM_COMMA 0xBC
#define XBOX_VK_OEM_MINUS 0xBD
#define XBOX_VK_OEM_PERIOD 0xBE
#define XBOX_VK_OEM_2     0xBF
#define XBOX_VK_OEM_3     0xC0
#define XBOX_VK_OEM_4     0xDB
#define XBOX_VK_OEM_5     0xDC
#define XBOX_VK_OEM_6     0xDD
#define XBOX_VK_OEM_7     0xDE

void xbox_InputKeySet(int vk, int down);

void xbox_InputKeysClear(void);

int  xbox_InputWin32SideVk(int vk, uint32_t lparam);

int  xbox_InputWin32MouseVk(unsigned msg, uint32_t wparam, int *down);

int  xbox_FramebufferKeyDown(int vk);

void xbox_InputKeyboardState(XBOX_INPUT_STATE *out);

struct InputBindings;
void xbox_InputKeyboardBindings(struct InputBindings *b);
void xbox_InputKeyboardOverride(const struct InputBindings *b);

int  xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);

enum { XBOX_KEY_BUTTON, XBOX_KEY_ANALOG, XBOX_KEY_AXIS };
enum { XBOX_KEY_LX, XBOX_KEY_LY, XBOX_KEY_RX, XBOX_KEY_RY };
typedef struct xbox_key_row {
    const char *keys;
    uint8_t     vk[2];
    uint8_t     kind;
    uint16_t    target;
    int8_t      sign;
    const char *guest;
} xbox_key_row;

extern const xbox_key_row xbox_key_map[];
extern const size_t       xbox_key_map_count;

#ifdef __cplusplus
}
#endif

#endif
