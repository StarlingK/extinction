#include "avp_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avp_keys.h"
#include "keyboard.h"
#include "input_map.h"

enum { K_PAD, K_PORT };

typedef struct {
    const char *ini;
    const char *name;
    int kind;
    int control;
    int def[2];
    int vk[2];
    int synth;
} Action;

#define PAD(ini, name, ctl, a, b) { ini, name, K_PAD, ctl, { a, b }, { a, b }, 0 }
#define PORT(ini, name, a, b)     { ini, name, K_PORT, -1, { a, b }, { a, b }, 0 }

static Action s_act[AVP_ACTION_COUNT] = {
    [AVP_ACT_A]            = PAD("a", "Select / confirm", INPUT_A, 'Z', 0),
    [AVP_ACT_B]            = PAD("b", "Move / back", INPUT_B, 'X', 0),
    [AVP_ACT_X]            = PAD("x", "X button", INPUT_X, 'C', 0),
    [AVP_ACT_Y]            = PAD("y", "Y button", INPUT_Y, 'V', 0),
    [AVP_ACT_WHITE]        = PAD("white", "White button", INPUT_WHITE, 'T', 0),
    [AVP_ACT_BLACK]        = PAD("black", "Context action", INPUT_BLACK, 'E', 0),
    [AVP_ACT_LTRIGGER]     = PAD("ltrigger", "Alternate (hold)", INPUT_LEFT_TRIGGER, VK_LSHIFT, 0),
    [AVP_ACT_RTRIGGER]     = PAD("rtrigger", "Orders menu", INPUT_RIGHT_TRIGGER, VK_TAB, 0),
    [AVP_ACT_START]        = PAD("start", "Pause", INPUT_START, VK_ESCAPE, VK_RETURN),
    [AVP_ACT_BACK]         = PAD("back", "Back button", INPUT_BACK, VK_BACK, 0),
    [AVP_ACT_DPAD_UP]      = PAD("dpad_up", "D-pad up", INPUT_DPAD_UP, VK_UP, 0),
    [AVP_ACT_DPAD_DOWN]    = PAD("dpad_down", "D-pad down", INPUT_DPAD_DOWN, VK_DOWN, 0),
    [AVP_ACT_DPAD_LEFT]    = PAD("dpad_left", "D-pad left", INPUT_DPAD_LEFT, VK_LEFT, 0),
    [AVP_ACT_DPAD_RIGHT]   = PAD("dpad_right", "D-pad right", INPUT_DPAD_RIGHT, VK_RIGHT, 0),
    [AVP_ACT_LTHUMB]       = PAD("lthumb", "Left stick click", INPUT_LEFT_THUMB, 'B', 0),
    [AVP_ACT_RTHUMB]       = PAD("rthumb", "Right stick click", INPUT_RIGHT_THUMB, 'N', 0),
    [AVP_ACT_LSTICK_UP]    = PAD("lstick_up", "Left stick up", INPUT_LSTICK_UP, VK_NUMPAD8, 0),
    [AVP_ACT_LSTICK_DOWN]  = PAD("lstick_down", "Left stick down", INPUT_LSTICK_DOWN, VK_NUMPAD2, 0),
    [AVP_ACT_LSTICK_LEFT]  = PAD("lstick_left", "Left stick left", INPUT_LSTICK_LEFT, VK_NUMPAD4, 0),
    [AVP_ACT_LSTICK_RIGHT] = PAD("lstick_right", "Left stick right", INPUT_LSTICK_RIGHT, VK_NUMPAD6, 0),
    [AVP_ACT_RSTICK_LEFT]  = PAD("rstick_left", "Rotate camera left", INPUT_RSTICK_LEFT, 0, 0),
    [AVP_ACT_RSTICK_RIGHT] = PAD("rstick_right", "Rotate camera right", INPUT_RSTICK_RIGHT, 0, 0),
    [AVP_ACT_RSTICK_UP]    = PAD("rstick_up", "Tilt camera up", INPUT_RSTICK_UP, 0, 0),
    [AVP_ACT_RSTICK_DOWN]  = PAD("rstick_down", "Tilt camera down", INPUT_RSTICK_DOWN, 0, 0),
    [AVP_ACT_PAN_UP]       = PORT("pan_up", "Camera pan up", 'W', 0),
    [AVP_ACT_PAN_LEFT]     = PORT("pan_left", "Camera pan left", 'A', 0),
    [AVP_ACT_PAN_DOWN]     = PORT("pan_down", "Camera pan down", 'S', 0),
    [AVP_ACT_PAN_RIGHT]    = PORT("pan_right", "Camera pan right", 'D', 0),
    [AVP_ACT_GIVE]         = PORT("give", "Give units to player 2", 'G', 0),
    [AVP_ACT_SETTINGS]     = PORT("settings", "PC settings", 0, 0),
    [AVP_ACT_GROUP1 + 0]   = PORT("group1", "Control group 1", '1', 0),
    [AVP_ACT_GROUP1 + 1]   = PORT("group2", "Control group 2", '2', 0),
    [AVP_ACT_GROUP1 + 2]   = PORT("group3", "Control group 3", '3', 0),
    [AVP_ACT_GROUP1 + 3]   = PORT("group4", "Control group 4", '4', 0),
    [AVP_ACT_GROUP1 + 4]   = PORT("group5", "Control group 5", '5', 0),
    [AVP_ACT_GROUP1 + 5]   = PORT("group6", "Control group 6", '6', 0),
    [AVP_ACT_GROUP1 + 6]   = PORT("group7", "Control group 7", '7', 0),
    [AVP_ACT_GROUP1 + 7]   = PORT("group8", "Control group 8", '8', 0),
    [AVP_ACT_GROUP1 + 8]   = PORT("group9", "Control group 9", '9', 0),
    [AVP_ACT_GROUP_STORE]  = PORT("group_store", "Store control group", VK_CONTROL, 0),
};

static const unsigned char k_synth[] = {
    0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xD8, 0xD9, 0xDA, 0xE8, 0x3A, 0x3B, 0x3C,
};

static int s_synth_ready;

#define KEYS_VERSION 3

static int s_file_version;
static unsigned char s_migrate[AVP_ACTION_COUNT];

static const struct { int action; int old_vk; } k_old_defaults[] = {
    { AVP_ACT_BLACK, VK_SPACE },
    { AVP_ACT_RSTICK_LEFT, 'Q' },
    { AVP_ACT_RSTICK_RIGHT, 'E' },
    { AVP_ACT_RSTICK_UP, 'R' },
    { AVP_ACT_RSTICK_DOWN, 'F' },
    { AVP_ACT_SETTINGS, VK_F10 },
};

static void migrate_old_defaults(void)
{
    size_t i;
    int black_moved = 0;
    for (i = 0; i < sizeof k_old_defaults / sizeof k_old_defaults[0]; i++) {
        int a = k_old_defaults[i].action;
        if (!s_migrate[a])
            continue;
        s_migrate[a] = 0;
        if (s_act[a].vk[0] != k_old_defaults[i].old_vk || s_act[a].vk[1] != 0)
            continue;
        s_act[a].vk[0] = 0;
        s_act[a].vk[1] = 0;
        if (a == AVP_ACT_BLACK)
            black_moved = 1;
    }
    if (black_moved) {
        int a, s, taken = 0;
        for (a = 0; a < AVP_ACTION_COUNT; a++)
            for (s = 0; s < 2; s++)
                if (a != AVP_ACT_BLACK && s_act[a].vk[s] == 'E')
                    taken = 1;
        if (!taken)
            s_act[AVP_ACT_BLACK].vk[0] = 'E';
    }
}

static void synth_init(void)
{
    int a;
    if (s_synth_ready)
        return;
    s_synth_ready = 1;
    for (a = 0; a < AVP_PAD_ACTIONS; a++)
        s_act[a].synth = k_synth[a];
}

static int valid(int action)
{
    return action >= 0 && action < AVP_ACTION_COUNT;
}

int avp_keys_vk(int action, int slot)
{
    if (!valid(action) || slot < 0 || slot > 1)
        return 0;
    return s_act[action].vk[slot];
}

const char *avp_keys_name(int action)
{
    return valid(action) ? s_act[action].name : "";
}

const char *avp_keys_ini_name(int action)
{
    return valid(action) ? s_act[action].ini : "";
}

int avp_keys_is_pad(int action)
{
    return valid(action) && s_act[action].kind == K_PAD;
}

int avp_keys_rebindable(int action)
{
    return valid(action) && action != AVP_ACT_GROUP_STORE;
}

void avp_pad_set(int pad_action, int down)
{
    synth_init();
    if (pad_action >= 0 && pad_action < AVP_PAD_ACTIONS)
        xbox_InputKeySet(s_act[pad_action].synth, down);
}

int avp_action_down(int action)
{
    int s;
    if (!valid(action))
        return 0;
    for (s = 0; s < 2; s++)
        if (s_act[action].vk[s] && xbox_FramebufferKeyDown(s_act[action].vk[s]))
            return 1;
    return 0;
}

int avp_keys_match(int action, int vk)
{
    return vk && (avp_keys_vk(action, 0) == vk || avp_keys_vk(action, 1) == vk);
}

int avp_group_key_pressed(int *group, int *store)
{
    static int prev[9];
    int g, hit = 0;
    for (g = 0; g < 9; g++) {
        int now = avp_action_down(AVP_ACT_GROUP1 + g);
        if (now && !prev[g] && !hit) {
            hit = 1;
            if (group)
                *group = g + 1;
            if (store)
                *store = xbox_FramebufferKeyDown(VK_CONTROL)
                      || xbox_FramebufferKeyDown(VK_LCONTROL)
                      || xbox_FramebufferKeyDown(VK_RCONTROL);
        }
        prev[g] = now;
    }
    return hit;
}

static int is_ctrl(int vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
}

void avp_keys_apply(void)
{
    InputBindings b;
    int a, s, c;

    synth_init();
    migrate_old_defaults();
    memset(&b, 0, sizeof b);
    b.diagonal = INPUT_DIAGONAL_FULL;
    for (a = 0; a < AVP_PAD_ACTIONS; a++) {
        c = s_act[a].control;
        b.key[c][b.count[c]++] = (uint8_t)s_act[a].synth;
        for (s = 0; s < 2; s++) {
            int vk = s_act[a].vk[s], i, dup = 0;
            if (vk <= 0 || vk > 255)
                continue;
            for (i = 0; i < b.count[c]; i++)
                dup |= b.key[c][i] == vk;
            if (!dup && b.count[c] < INPUT_BINDINGS_PER_CONTROL)
                b.key[c][b.count[c]++] = (uint8_t)vk;
        }
    }
    xbox_InputKeyboardOverride(&b);
}

int avp_keys_conflict(int action, int vk)
{
    int a, s;
    if (!valid(action) || vk <= 0)
        return -1;
    if (is_ctrl(vk) && action != AVP_ACT_GROUP_STORE)
        return AVP_ACT_GROUP_STORE;
    for (a = 0; a < AVP_ACTION_COUNT; a++) {
        if (a == action || a == AVP_ACT_GROUP_STORE)
            continue;
        for (s = 0; s < 2; s++)
            if (s_act[a].vk[s] == vk)
                return a;
    }
    return -1;
}

int avp_keys_bind(int action, int slot, int vk)
{
    int c;
    if (!avp_keys_rebindable(action) || slot < 0 || slot > 1 || vk < 0 || vk > 255)
        return -2;
    c = avp_keys_conflict(action, vk);
    if (c >= 0)
        return c;
    s_act[action].vk[slot] = vk;
    avp_keys_apply();
    return -1;
}

void avp_keys_reset_defaults(void)
{
    int a;
    for (a = 0; a < AVP_ACTION_COUNT; a++) {
        s_act[a].vk[0] = s_act[a].def[0];
        s_act[a].vk[1] = s_act[a].def[1];
    }
    avp_keys_apply();
}

void avp_keys_save(FILE *f)
{
    int a;
    fprintf(f, "key_version=%d\n", KEYS_VERSION);
    for (a = 0; a < AVP_ACTION_COUNT; a++)
        if (avp_keys_rebindable(a))
            fprintf(f, "key_%s=0x%02X,0x%02X\n", s_act[a].ini, s_act[a].vk[0], s_act[a].vk[1]);
}

int avp_keys_load_line(const char *ini_name, const char *value)
{
    char *end;
    long v0, v1;
    int a;

    if (!strcmp(ini_name, "version")) {
        s_file_version = atoi(value);
        return 1;
    }
    for (a = 0; a < AVP_ACTION_COUNT; a++)
        if (avp_keys_rebindable(a) && !strcmp(ini_name, s_act[a].ini))
            break;
    if (a == AVP_ACTION_COUNT)
        return 0;
    v0 = strtol(value, &end, 0);
    if (end == value || v0 < 0 || v0 > 255)
        return 1;
    v1 = 0;
    if (*end == ',') {
        value = end + 1;
        v1 = strtol(value, &end, 0);
        if (end == value || v1 < 0 || v1 > 255)
            v1 = 0;
    }
    s_act[a].vk[0] = (int)v0;
    s_act[a].vk[1] = (int)v1;
    if (s_file_version < KEYS_VERSION)
        s_migrate[a] = 1;
    return 1;
}

const char *avp_vk_name(int vk)
{
    static char buf[4][48];
    static int n;
    static const struct { int vk; const char *name; } named[] = {
        { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_UP, "Up" }, { VK_DOWN, "Down" },
        { VK_ESCAPE, "Esc" }, { VK_SPACE, "Space" }, { VK_TAB, "Tab" }, { VK_RETURN, "Enter" },
        { VK_BACK, "Backspace" }, { VK_SHIFT, "Shift" }, { VK_LSHIFT, "L-Shift" },
        { VK_RSHIFT, "R-Shift" }, { VK_CONTROL, "Ctrl" }, { VK_LCONTROL, "L-Ctrl" },
        { VK_RCONTROL, "R-Ctrl" }, { VK_MENU, "Alt" }, { VK_LMENU, "L-Alt" },
        { VK_RMENU, "R-Alt" }, { VK_PRIOR, "Page Up" }, { VK_NEXT, "Page Down" },
        { VK_HOME, "Home" }, { VK_END, "End" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" },
        { VK_CAPITAL, "Caps Lock" }, { VK_PAUSE, "Pause" }, { VK_MULTIPLY, "Num *" },
        { VK_ADD, "Num +" }, { VK_SUBTRACT, "Num -" }, { VK_DECIMAL, "Num ." },
        { VK_DIVIDE, "Num /" },
    };
    char *out = buf[n++ & 3];
    size_t i;
#ifdef _WIN32
    LONG lparam;
    int scan;
#endif

    if (vk <= 0 || vk > 255)
        return "-";
    for (i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].vk == vk)
            return named[i].name;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(out, sizeof buf[0], "Num %d", vk - VK_NUMPAD0);
        return out;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, sizeof buf[0], "F%d", vk - VK_F1 + 1);
        return out;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = (char)vk;
        out[1] = 0;
        return out;
    }
#ifdef _WIN32
    scan = (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    lparam = (LONG)scan << 16;
    if (scan && GetKeyNameTextA(lparam, out, (int)sizeof buf[0]) > 0)
        return out;
#else
    {
        static const struct { int vk; const char *name; } oem[] = {
            { 0xBA, ";" }, { 0xBB, "=" }, { 0xBC, "," }, { 0xBD, "-" }, { 0xBE, "." },
            { 0xBF, "/" }, { 0xC0, "`" }, { 0xDB, "[" }, { 0xDC, "\\" }, { 0xDD, "]" },
            { 0xDE, "'" }, { 0x01, "Mouse 1" }, { 0x02, "Mouse 2" }, { 0x04, "Mouse 3" },
            { 0x05, "Mouse 4" }, { 0x06, "Mouse 5" },
        };
        for (i = 0; i < sizeof oem / sizeof oem[0]; i++)
            if (oem[i].vk == vk)
                return oem[i].name;
    }
#endif
    snprintf(out, sizeof buf[0], "Key %02X", vk);
    return out;
}
