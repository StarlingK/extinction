#ifndef UI_SKIN_H
#define UI_SKIN_H

#include "imgui.h"

#define SK_RGB(r, g, b) ((ImU32)(r) | (ImU32)(g) << 8 | (ImU32)(b) << 16)

enum {
    SK_FACE = SK_RGB(212, 208, 200),
    SK_BTN = SK_RGB(212, 208, 201),
    SK_BTN_HOT = SK_RGB(212, 208, 202),
    SK_BTN_DOWN = SK_RGB(212, 208, 203),
    SK_GRAB = SK_RGB(212, 208, 204),
    SK_GRAB_DOWN = SK_RGB(212, 208, 205),
    SK_SB_GRAB = SK_RGB(212, 208, 206),
    SK_SB_GRAB_HOT = SK_RGB(212, 208, 207),
    SK_SB_GRAB_DOWN = SK_RGB(212, 208, 209),
    SK_SB_TRACK = SK_RGB(232, 228, 220),
    SK_TAB = SK_RGB(200, 196, 188),
    SK_TAB_HOT = SK_RGB(212, 208, 210),
    SK_TAB_SEL = SK_RGB(212, 208, 211),
    SK_HDR = SK_RGB(10, 36, 106),
    SK_HDR_HOT = SK_RGB(10, 36, 107),
    SK_HDR_DOWN = SK_RGB(10, 36, 108),
    SK_TITLE = SK_RGB(0, 0, 128),
    SK_TITLE_OFF = SK_RGB(128, 128, 129),
    SK_FRAME = SK_RGB(255, 255, 254),
    SK_FRAME_HOT = SK_RGB(255, 255, 253),
    SK_FRAME_DOWN = SK_RGB(255, 255, 252),
    SK_LIST = SK_RGB(255, 255, 251)
};

void ui_skin_colors(ImGuiStyle &s);
void ui_skin_apply();
bool ui_skin_mouse(int button, bool down, float x, float y);
void ui_skin_tick(float dt);
void ui_skin_enable(bool on);
void skin_raised(ImDrawList *dl, ImVec2 a, ImVec2 b);
void skin_sunken(ImDrawList *dl, ImVec2 a, ImVec2 b);
void skin_pressed(ImDrawList *dl, ImVec2 a, ImVec2 b);
void skin_arrow(ImDrawList *dl, ImVec2 a, ImVec2 b, int dir, bool down);

#endif
