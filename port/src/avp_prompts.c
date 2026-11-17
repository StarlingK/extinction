#include "avp_platform.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "avp_keys.h"

extern ptrdiff_t g_xbox_mem_offset;
extern int avp_input_controller(void);

#define G8(a) (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))
#define G32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset))

#define FONT_GLYPH_MAP 0x24u
#define FONT_NONE 0xFFu
#define RENDER_WORKSPACE 0x20u
#define WORKSPACE_TEXT 0xCu
#define TEXT_LIMIT 0x800

#define GLYPH_STICK_LEFT '{'
#define GLYPH_STICK_RIGHT '}'

static uint32_t s_text;
static int s_len;
static char s_saved[TEXT_LIMIT];
static int s_active;

static int glyph_action(unsigned char c)
{
    switch (c) {
    case 0xA9: return AVP_ACT_X;
    case 0xAA: return AVP_ACT_B;
    case 0xAB: return AVP_ACT_Y;
    case 0xAC: return AVP_ACT_A;
    case 0xAD: return AVP_ACT_BLACK;
    case 0xAE: return AVP_ACT_RTRIGGER;
    case 0xAF: return AVP_ACT_LTRIGGER;
    case 0xB0: return AVP_ACT_WHITE;
    case 0xB1: return AVP_ACT_DPAD_UP;
    case 0xB2: return AVP_ACT_DPAD_DOWN;
    case 0xB3: return AVP_ACT_DPAD_LEFT;
    case 0xB4: return AVP_ACT_DPAD_RIGHT;
    default: return -1;
    }
}

static int is_glyph(unsigned char c)
{
    return glyph_action(c) >= 0 || c == GLYPH_STICK_LEFT || c == GLYPH_STICK_RIGHT;
}

static int first_vk(int action)
{
    int vk = avp_keys_vk(action, 0);

    return vk ? vk : avp_keys_vk(action, 1);
}

static int add(char *out, int n, int cap, const char *s)
{
    while (*s && n < cap)
        out[n++] = *s++;
    return n;
}

static int add_group(char *out, int n, int cap, const int *actions, int count)
{
    char names[4][48];
    int i, found = 0, single = 1;

    for (i = 0; i < count; i++) {
        int vk = first_vk(actions[i]);

        names[i][0] = 0;
        if (!vk)
            continue;
        strncpy(names[i], avp_vk_name(vk), sizeof names[i] - 1);
        names[i][sizeof names[i] - 1] = 0;
        found++;
        if (strlen(names[i]) != 1)
            single = 0;
    }
    if (!found)
        return -1;
    for (i = 0; i < count; i++) {
        if (!names[i][0])
            continue;
        if (!single && n > 0 && out[n - 1] != '[')
            n = add(out, n, cap, "/");
        n = add(out, n, cap, names[i]);
    }
    return n;
}

static int key_label(unsigned char c, char *out, int cap)
{
    static const int pan[4] = { AVP_ACT_PAN_UP, AVP_ACT_PAN_LEFT, AVP_ACT_PAN_DOWN, AVP_ACT_PAN_RIGHT };
    static const int rstick[4] = { AVP_ACT_RSTICK_UP, AVP_ACT_RSTICK_LEFT, AVP_ACT_RSTICK_DOWN, AVP_ACT_RSTICK_RIGHT };
    int n = 0, action = glyph_action(c), vk;

    n = add(out, n, cap, "[");
    if (c == GLYPH_STICK_LEFT) {
        n = add_group(out, n, cap, pan, 4);
        if (n < 0)
            return -1;
    } else if (c == GLYPH_STICK_RIGHT) {
        int m = add_group(out, n, cap, rstick, 4);

        n = m < 0 ? add(out, n, cap, "Mouse") : m;
    } else {
        vk = first_vk(action);
        if (!vk)
            return -1;
        n = add(out, n, cap, avp_vk_name(vk));
    }
    n = add(out, n, cap, "]");
    return n;
}

static int font_has(uint32_t font, unsigned char c)
{
    return G8(font + FONT_GLYPH_MAP + c) != FONT_NONE;
}

static int emit(uint32_t font, char *out, int n, int cap, const char *label, int len)
{
    int i;

    for (i = 0; i < len && n < cap; i++) {
        unsigned char c = (unsigned char)label[i];

        if (c != ' ' && !font_has(font, c)) {
            if (isalpha(c) && font_has(font, (unsigned char)toupper(c)))
                c = (unsigned char)toupper(c);
            else if (isalpha(c) && font_has(font, (unsigned char)tolower(c)))
                c = (unsigned char)tolower(c);
            else
                continue;
        }
        out[n++] = (char)c;
    }
    return n;
}

static int dpad_words(char *out, int cap)
{
    static const int dpad[4] = { AVP_ACT_DPAD_UP, AVP_ACT_DPAD_DOWN, AVP_ACT_DPAD_LEFT, AVP_ACT_DPAD_RIGHT };
    static const int arrows[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    int i, plain = 1, n = 0;

    for (i = 0; i < 4; i++)
        if (first_vk(dpad[i]) != arrows[i])
            plain = 0;
    if (plain)
        return add(out, 0, cap, "arrow keys");
    n = add(out, n, cap, "[");
    n = add_group(out, n, cap, dpad, 4);
    if (n < 0)
        return -1;
    return add(out, n, cap, "]");
}

static int word_at(const char *t, int i, int len, const char **word, int *wlen)
{
    static const char *const words[] = { "directional pad", "D-pad", "START" };
    int k;

    for (k = 0; k < 3; k++) {
        int l = (int)strlen(words[k]);

        if (i + l > len || strncmp(t + i, words[k], (size_t)l))
            continue;
        if (k == 2 && !(i >= 5 && !strncmp(t + i - 5, "ress ", 5)))
            continue;
        *word = words[k];
        *wlen = l;
        return k;
    }
    return -1;
}

static int word_label(int k, char *out, int cap)
{
    int vk, n;

    if (k < 2)
        return dpad_words(out, cap);
    vk = first_vk(AVP_ACT_START);
    if (!vk)
        return -1;
    n = add(out, 0, cap, "[");
    n = add(out, n, cap, avp_vk_name(vk));
    return add(out, n, cap, "]");
}

void avp_prompts_begin(uint32_t font, uint32_t render)
{
    static char out[TEXT_LIMIT];
    uint32_t ws, text;
    int len, i, n = 0, changed = 0;

    s_active = 0;
    if (!font || !render || avp_input_controller())
        return;
    ws = G32(render + RENDER_WORKSPACE);
    if (!ws)
        return;
    text = ws + WORKSPACE_TEXT;
    for (len = 0; len < TEXT_LIMIT - 1 && G8(text + len); len++)
        s_saved[len] = (char)G8(text + len);
    if (len >= TEXT_LIMIT - 1)
        return;
    s_saved[len] = 0;
    for (i = 0; i < len; i++) {
        const char *w;
        int wl;

        if ((is_glyph((unsigned char)s_saved[i]) && font_has(font, (unsigned char)s_saved[i]))
            || word_at(s_saved, i, len, &w, &wl) >= 0) {
            changed = 1;
            break;
        }
    }
    if (!changed)
        return;
    for (i = 0; i < len;) {
        unsigned char c = (unsigned char)s_saved[i];
        char label[96];
        const char *w;
        int ll = -1, wl = 0, k, before = n, step = 1;

        if (is_glyph(c) && font_has(font, c)) {
            ll = key_label(c, label, (int)sizeof label - 1);
        } else if ((k = word_at(s_saved, i, len, &w, &wl)) >= 0) {
            ll = word_label(k, label, (int)sizeof label - 1);
            step = wl;
        }
        if (ll > 0) {
            n = emit(font, out, n, TEXT_LIMIT - 1, label, ll);
            if (n > before) {
                i += step;
                continue;
            }
            n = before;
        }
        if (n < TEXT_LIMIT - 1)
            out[n++] = (char)c;
        i++;
    }
    s_text = text;
    s_len = len;
    for (i = 0; i < n; i++)
        G8(text + i) = (uint8_t)out[i];
    G8(text + n) = 0;
    s_active = 1;
}

void avp_prompts_end(void)
{
    int i;

    if (!s_active)
        return;
    s_active = 0;
    for (i = 0; i < s_len; i++)
        G8(s_text + i) = (uint8_t)s_saved[i];
    G8(s_text + s_len) = 0;
}
