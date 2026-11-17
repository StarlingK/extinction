#include "imgui.h"
#include "imgui_internal.h"
#include "ui_skin.h"
#include <vector>
#include <math.h>

static bool s_on;

struct SkQuad {
    ImVec2 mn, mx;
    ImU32 col;
    bool white;
    int first;
    int vtx[4];
    int cmd;
};

struct SkArrow {
    ImGuiID win;
    ImVec2 mn, mx;
    int dir;
};

static std::vector<SkArrow> s_arrows;
static ImGuiID s_cap_win;
static int s_cap_dir;
static double s_cap_t, s_cap_next;

static ImU32 rgb_of(ImU32 c)
{
    return c & 0x00FFFFFFu;
}

static ImU32 with_alpha(ImU32 rgb, ImU32 a)
{
    return rgb_of(rgb) | (a << 24);
}

static ImU32 solid(ImU32 rgb)
{
    return rgb | 0xFF000000u;
}

static void box(ImDrawList *dl, float x0, float y0, float x1, float y1, ImU32 col)
{
    if (x1 > x0 && y1 > y0)
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
}

static void bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 otl, ImU32 itl, ImU32 obr, ImU32 ibr, bool two)
{
    float x0 = floorf(a.x + 0.5f), y0 = floorf(a.y + 0.5f), x1 = floorf(b.x + 0.5f), y1 = floorf(b.y + 0.5f);
    if (x1 - x0 < 3 || y1 - y0 < 3)
        two = false;
    box(dl, x0, y0, x1 - 1, y0 + 1, otl);
    box(dl, x0, y0, x0 + 1, y1 - 1, otl);
    box(dl, x0, y1 - 1, x1, y1, obr);
    box(dl, x1 - 1, y0, x1, y1, obr);
    if (two) {
        box(dl, x0 + 1, y0 + 1, x1 - 2, y0 + 2, itl);
        box(dl, x0 + 1, y0 + 1, x0 + 2, y1 - 2, itl);
        box(dl, x0 + 1, y1 - 2, x1 - 1, y1 - 1, ibr);
        box(dl, x1 - 2, y0 + 1, x1 - 1, y1 - 1, ibr);
    }
}

void skin_raised(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    bevel(dl, a, b, IM_COL32(255, 255, 255, 255), IM_COL32(236, 233, 227, 255), IM_COL32(64, 64, 64, 255), IM_COL32(128, 128, 128, 255), true);
}

void skin_pressed(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    bevel(dl, a, b, IM_COL32(64, 64, 64, 255), IM_COL32(128, 128, 128, 255), IM_COL32(255, 255, 255, 255), IM_COL32(212, 208, 200, 255), true);
}

void skin_sunken(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    bevel(dl, a, b, IM_COL32(128, 128, 128, 255), IM_COL32(64, 64, 64, 255), IM_COL32(255, 255, 255, 255), IM_COL32(212, 208, 200, 255), true);
}

void skin_arrow(ImDrawList *dl, ImVec2 a, ImVec2 b, int dir, bool down)
{
    dl->AddRectFilled(a, b, IM_COL32(212, 208, 200, 255));
    if (down)
        skin_pressed(dl, a, b);
    else
        skin_raised(dl, a, b);
    float cx = floorf((a.x + b.x) * 0.5f) + (down ? 1.0f : 0.0f), cy = floorf((a.y + b.y) * 0.5f) + (down ? 1.0f : 0.0f);
    float s = floorf(fminf(b.x - a.x, b.y - a.y) * 0.22f);
    if (s < 2.0f)
        s = 2.0f;
    ImU32 k = IM_COL32(0, 0, 0, 255);
    if (dir == 0)
        dl->AddTriangleFilled(ImVec2(cx, cy - s * 0.6f), ImVec2(cx - s, cy + s * 0.6f), ImVec2(cx + s, cy + s * 0.6f), k);
    else if (dir == 1)
        dl->AddTriangleFilled(ImVec2(cx, cy + s * 0.6f), ImVec2(cx + s, cy - s * 0.6f), ImVec2(cx - s, cy - s * 0.6f), k);
    else if (dir == 2)
        dl->AddTriangleFilled(ImVec2(cx - s * 0.6f, cy), ImVec2(cx + s * 0.6f, cy - s), ImVec2(cx + s * 0.6f, cy + s), k);
    else
        dl->AddTriangleFilled(ImVec2(cx + s * 0.6f, cy), ImVec2(cx - s * 0.6f, cy + s), ImVec2(cx - s * 0.6f, cy - s), k);
}

static ImVec4 c8(int r, int g, int b, int a = 255)
{
    return ImVec4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
}

static ImVec4 sk(ImU32 rgb)
{
    return c8((int)(rgb & 255), (int)((rgb >> 8) & 255), (int)((rgb >> 16) & 255));
}

void ui_skin_colors(ImGuiStyle &s)
{
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = c8(0, 0, 0);
    c[ImGuiCol_TextDisabled] = c8(128, 128, 128);
    c[ImGuiCol_WindowBg] = sk(SK_FACE);
    c[ImGuiCol_ChildBg] = sk(SK_LIST);
    c[ImGuiCol_PopupBg] = sk(SK_FACE);
    c[ImGuiCol_Border] = c8(0, 0, 0, 0);
    c[ImGuiCol_BorderShadow] = c8(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = sk(SK_FRAME);
    c[ImGuiCol_FrameBgHovered] = sk(SK_FRAME_HOT);
    c[ImGuiCol_FrameBgActive] = sk(SK_FRAME_DOWN);
    c[ImGuiCol_TitleBg] = sk(SK_TITLE_OFF);
    c[ImGuiCol_TitleBgActive] = sk(SK_TITLE);
    c[ImGuiCol_TitleBgCollapsed] = sk(SK_TITLE_OFF);
    c[ImGuiCol_MenuBarBg] = sk(SK_FACE);
    c[ImGuiCol_ScrollbarBg] = sk(SK_SB_TRACK);
    c[ImGuiCol_ScrollbarGrab] = sk(SK_SB_GRAB);
    c[ImGuiCol_ScrollbarGrabHovered] = sk(SK_SB_GRAB_HOT);
    c[ImGuiCol_ScrollbarGrabActive] = sk(SK_SB_GRAB_DOWN);
    c[ImGuiCol_CheckMark] = c8(0, 0, 0);
    c[ImGuiCol_SliderGrab] = sk(SK_GRAB);
    c[ImGuiCol_SliderGrabActive] = sk(SK_GRAB_DOWN);
    c[ImGuiCol_Button] = sk(SK_BTN);
    c[ImGuiCol_ButtonHovered] = sk(SK_BTN_HOT);
    c[ImGuiCol_ButtonActive] = sk(SK_BTN_DOWN);
    c[ImGuiCol_Header] = sk(SK_HDR);
    c[ImGuiCol_HeaderHovered] = sk(SK_HDR_HOT);
    c[ImGuiCol_HeaderActive] = sk(SK_HDR_DOWN);
    c[ImGuiCol_Separator] = c8(128, 128, 128);
    c[ImGuiCol_SeparatorHovered] = c8(64, 64, 64);
    c[ImGuiCol_SeparatorActive] = c8(0, 0, 0);
    c[ImGuiCol_ResizeGrip] = c8(128, 128, 128);
    c[ImGuiCol_ResizeGripHovered] = c8(64, 64, 64);
    c[ImGuiCol_ResizeGripActive] = c8(0, 0, 0);
    c[ImGuiCol_Tab] = sk(SK_TAB);
    c[ImGuiCol_TabHovered] = sk(SK_TAB_HOT);
    c[ImGuiCol_TabSelected] = sk(SK_TAB_SEL);
    c[ImGuiCol_TabSelectedOverline] = c8(0, 0, 0, 0);
    c[ImGuiCol_TabDimmed] = sk(SK_TAB);
    c[ImGuiCol_TabDimmedSelected] = sk(SK_TAB_SEL);
    c[ImGuiCol_TabDimmedSelectedOverline] = c8(0, 0, 0, 0);
    c[ImGuiCol_TableHeaderBg] = sk(SK_FACE);
    c[ImGuiCol_TableBorderStrong] = c8(128, 128, 128);
    c[ImGuiCol_TableBorderLight] = c8(192, 192, 188);
    c[ImGuiCol_TableRowBg] = c8(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = c8(244, 242, 236);
    c[ImGuiCol_TextSelectedBg] = c8(10, 36, 106, 150);
    c[ImGuiCol_NavWindowingHighlight] = c8(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = c8(0, 0, 0, 80);
}

void ui_skin_enable(bool on)
{
    s_on = on;
    if (!on) {
        s_arrows.clear();
        s_cap_win = 0;
    }
}

static bool same_rect(const SkQuad &q, ImVec2 mn, ImVec2 mx)
{
    return fabsf(q.mn.x - mn.x) < 1.5f && fabsf(q.mn.y - mn.y) < 1.5f && fabsf(q.mx.x - mx.x) < 1.5f && fabsf(q.mx.y - mx.y) < 1.5f;
}

static void collect(ImDrawList *dl, std::vector<SkQuad> &out)
{
    const ImVec2 wuv = dl->_Data->TexUvWhitePixel;
    for (int c = 0; c < dl->CmdBuffer.Size; c++) {
        const ImDrawCmd &cmd = dl->CmdBuffer[c];
        if (cmd.UserCallback)
            continue;
        const ImDrawIdx *ix = dl->IdxBuffer.Data + cmd.IdxOffset;
        int n = (int)cmd.ElemCount;
        for (int i = 0; i + 5 < n;) {
            int a = ix[i];
            if (ix[i + 1] == a + 1 && ix[i + 2] == a + 2 && ix[i + 3] == a && ix[i + 4] == a + 2 && ix[i + 5] == a + 3) {
                const ImDrawVert *v = dl->VtxBuffer.Data + cmd.VtxOffset + a;
                SkQuad q;
                q.mn = ImVec2(fminf(fminf(v[0].pos.x, v[1].pos.x), fminf(v[2].pos.x, v[3].pos.x)), fminf(fminf(v[0].pos.y, v[1].pos.y), fminf(v[2].pos.y, v[3].pos.y)));
                q.mx = ImVec2(fmaxf(fmaxf(v[0].pos.x, v[1].pos.x), fmaxf(v[2].pos.x, v[3].pos.x)), fmaxf(fmaxf(v[0].pos.y, v[1].pos.y), fmaxf(v[2].pos.y, v[3].pos.y)));
                q.col = v[0].col;
                q.white = true;
                for (int k = 0; k < 4; k++) {
                    q.vtx[k] = (int)cmd.VtxOffset + a + k;
                    if (v[k].uv.x != wuv.x || v[k].uv.y != wuv.y)
                        q.white = false;
                    if (v[k].col != q.col)
                        q.col = 0;
                }
                q.first = a;
                q.cmd = c;
                out.push_back(q);
                i += 6;
            } else
                i += 3;
        }
    }
}

struct SkBevel {
    ImVec2 a, b;
    int kind;
    ImU32 alpha;
};

static void recolor_text(ImDrawList *dl, const std::vector<SkQuad> &qs, ImVec2 mn, ImVec2 mx, ImU32 col, bool bold, std::vector<SkQuad> *dups)
{
    for (size_t i = 0; i < qs.size(); i++) {
        const SkQuad &q = qs[i];
        if (q.white || rgb_of(q.col) != 0 || q.col == 0)
            continue;
        float cx = (q.mn.x + q.mx.x) * 0.5f, cy = (q.mn.y + q.mx.y) * 0.5f;
        if (cx < mn.x || cx > mx.x || cy < mn.y || cy > mx.y)
            continue;
        for (int k = 0; k < 4; k++)
            dl->VtxBuffer.Data[q.vtx[k]].col = col;
        if (bold && dups)
            dups->push_back(q);
    }
}

static void skin_window(ImGuiWindow *w)
{
    ImDrawList *dl = w->DrawList;
    if (!dl || dl->CmdBuffer.Size == 0 || dl->VtxBuffer.Size == 0)
        return;
    std::vector<SkQuad> qs;
    collect(dl, qs);
    std::vector<SkBevel> bev;
    std::vector<SkQuad> dups;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    bool is_child = (w->Flags & ImGuiWindowFlags_ChildWindow) != 0;
    bool titled = !is_child && !(w->Flags & ImGuiWindowFlags_NoTitleBar);
    ImVec2 tmn = w->Pos, tmx = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->TitleBarHeight);
    ImVec2 hmn(0, 0), hmx(0, 0);
    bool have_hdr = false;
    std::vector<std::pair<ImVec2, ImVec2> > hdrs;
    for (size_t i = 0; i < qs.size(); i++) {
        SkQuad &q = qs[i];
        if (!q.white || q.col == 0)
            continue;
        ImU32 rgb = rgb_of(q.col), al = q.col >> 24;
        if (al == 0)
            continue;
        ImVec2 a = q.mn, b = q.mx;
        if (b.x - a.x < 3 || b.y - a.y < 3)
            continue;
        switch (rgb) {
        case SK_BTN:
        case SK_BTN_HOT:
            bev.push_back({ a, b, 0, al });
            break;
        case SK_BTN_DOWN:
            bev.push_back({ a, b, 1, al });
            break;
        case SK_FRAME:
        case SK_FRAME_HOT:
        case SK_FRAME_DOWN:
            bev.push_back({ a, b, 2, al });
            break;
        case SK_LIST:
            if (is_child)
                bev.push_back({ a, b, 2, al });
            break;
        case SK_GRAB:
        case SK_GRAB_DOWN:
            bev.push_back({ a, b, 3, al });
            break;
        case SK_TAB:
        case SK_TAB_HOT:
        case SK_TAB_SEL:
            bev.push_back({ a, b, rgb == SK_TAB_SEL ? 5 : 4, al });
            break;
        case SK_HDR:
        case SK_HDR_HOT:
        case SK_HDR_DOWN:
            hdrs.push_back(std::make_pair(a, b));
            have_hdr = true;
            break;
        case SK_SB_TRACK: {
            bool vert = (b.y - a.y) > (b.x - a.x);
            float thick = vert ? b.x - a.x : b.y - a.y, len = vert ? b.y - a.y : b.x - a.x;
            if (thick < 8 || len < thick * 2 + 8)
                break;
            for (size_t j = i + 1; j < qs.size(); j++) {
                SkQuad &gq = qs[j];
                ImU32 gr = rgb_of(gq.col);
                if (!gq.white || (gr != SK_SB_GRAB && gr != SK_SB_GRAB_HOT && gr != SK_SB_GRAB_DOWN))
                    continue;
                if (gq.mn.x < a.x - 1 || gq.mx.x > b.x + 1 || gq.mn.y < a.y - 1 || gq.mx.y > b.y + 1)
                    continue;
                float room = len - thick * 2;
                ImDrawVert *vv = dl->VtxBuffer.Data;
                float lo = vert ? a.y : a.x, k = room / len;
                ImVec2 ng0 = gq.mn, ng1 = gq.mx;
                for (int m = 0; m < 4; m++) {
                    float &p = vert ? vv[gq.vtx[m]].pos.y : vv[gq.vtx[m]].pos.x;
                    p = lo + thick + (p - lo) * k;
                }
                if (vert) {
                    ng0.y = lo + thick + (gq.mn.y - lo) * k;
                    ng1.y = lo + thick + (gq.mx.y - lo) * k;
                } else {
                    ng0.x = lo + thick + (gq.mn.x - lo) * k;
                    ng1.x = lo + thick + (gq.mx.x - lo) * k;
                }
                bev.push_back({ ng0, ng1, gr == SK_SB_GRAB_DOWN ? 1 : 0, al });
                break;
            }
            SkArrow up, dn;
            up.win = dn.win = w->ID;
            up.dir = vert ? 0 : 2;
            dn.dir = vert ? 1 : 3;
            up.mn = a;
            up.mx = vert ? ImVec2(b.x, a.y + thick) : ImVec2(a.x + thick, b.y);
            dn.mn = vert ? ImVec2(a.x, b.y - thick) : ImVec2(b.x - thick, a.y);
            dn.mx = b;
            s_arrows.push_back(up);
            s_arrows.push_back(dn);
            break;
        }
        case SK_TITLE:
        case SK_TITLE_OFF:
            if (titled && same_rect(q, tmn, tmx)) {
                bool act = rgb == SK_TITLE;
                ImDrawVert *vv = dl->VtxBuffer.Data;
                ImU32 l = act ? IM_COL32(0, 0, 128, 255) : IM_COL32(128, 128, 128, 255), r = act ? IM_COL32(16, 132, 208, 255) : IM_COL32(192, 192, 192, 255);
                for (int m = 0; m < 4; m++) {
                    ImDrawVert &dv = vv[q.vtx[m]];
                    bool left = fabsf(dv.pos.x - q.mn.x) < 0.5f;
                    bool top = fabsf(dv.pos.y - q.mn.y) < 0.5f;
                    dv.col = left ? l : r;
                    dv.pos.x += left ? 2.0f : -2.0f;
                    if (top)
                        dv.pos.y += 2.0f;
                }
                recolor_text(dl, qs, tmn, tmx, act ? IM_COL32(255, 255, 255, 255) : IM_COL32(212, 208, 200, 255), true, &dups);
            }
            break;
        default:
            break;
        }
    }
    if (have_hdr)
        for (size_t i = 0; i < hdrs.size(); i++)
            recolor_text(dl, qs, hdrs[i].first, hdrs[i].second, IM_COL32(255, 255, 255, 255), false, NULL);
    (void)hmn;
    (void)hmx;
    bool frame = false, tip = (w->Flags & ImGuiWindowFlags_Tooltip) != 0, popup = (w->Flags & ImGuiWindowFlags_Popup) != 0 && !tip;
    ImVec2 wmn = w->Pos, wmx = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    if (!is_child && (titled || popup))
        frame = true;
    bool toolbar = w->Name[0] == '#' && !strcmp(w->Name, "##toolbar");
    bool status = w->Name[0] == '#' && !strcmp(w->Name, "##status");
    bool menubar = w->Name[0] == '#' && strstr(w->Name, "MainMenuBar") != NULL;
    if (bev.empty() && dups.empty() && !frame && !tip && !toolbar && !status && !menubar)
        return;
    dl->PushClipRectFullScreen();
    for (size_t i = 0; i < bev.size(); i++) {
        const SkBevel &e = bev[i];
        ImVec2 a = ImVec2(floorf(e.a.x + 0.5f), floorf(e.a.y + 0.5f)), b = ImVec2(floorf(e.b.x + 0.5f), floorf(e.b.y + 0.5f));
        if (e.kind == 0)
            skin_raised(dl, a, b);
        else if (e.kind == 1)
            skin_pressed(dl, a, b);
        else if (e.kind == 2)
            skin_sunken(dl, a, b);
        else if (e.kind == 3)
            bevel(dl, a, b, IM_COL32(255, 255, 255, 255), 0, IM_COL32(64, 64, 64, 255), IM_COL32(128, 128, 128, 255), false);
        else {
            bool sel = e.kind == 5;
            ImU32 wh = IM_COL32(255, 255, 255, 255), dk = IM_COL32(64, 64, 64, 255), gy = IM_COL32(128, 128, 128, 255);
            box(dl, a.x, a.y, b.x - 1, a.y + 1, wh);
            box(dl, a.x, a.y, a.x + 1, b.y, wh);
            box(dl, b.x - 1, a.y, b.x, b.y, dk);
            box(dl, b.x - 2, a.y + 1, b.x - 1, b.y, gy);
            if (sel)
                box(dl, a.x + 1, b.y - 1, b.x - 2, b.y, SK_FACE | 0xFF000000u);
            else
                box(dl, a.x, b.y - 1, b.x, b.y, wh);
        }
    }
    if (!dups.empty()) {
        dl->PushClipRect(ImVec2(tmn.x + 1, tmn.y), ImVec2(tmx.x - 1, tmx.y), false);
        dl->PushTextureID(ImGui::GetIO().Fonts->TexID);
        for (size_t i = 0; i < dups.size(); i++) {
            const SkQuad &q = dups[i];
            ImDrawVert v[4];
            for (int k = 0; k < 4; k++)
                v[k] = dl->VtxBuffer.Data[q.vtx[k]];
            dl->PrimReserve(6, 4);
            dl->PrimQuadUV(ImVec2(v[0].pos.x + 1, v[0].pos.y), ImVec2(v[1].pos.x + 1, v[1].pos.y), ImVec2(v[2].pos.x + 1, v[2].pos.y), ImVec2(v[3].pos.x + 1, v[3].pos.y),
                v[0].uv, v[1].uv, v[2].uv, v[3].uv, v[0].col);
        }
        dl->PopTextureID();
        dl->PopClipRect();
    }
    if (frame) {
        ImVec2 a = ImVec2(floorf(wmn.x + 0.5f), floorf(wmn.y + 0.5f)), b = ImVec2(floorf(wmx.x + 0.5f), floorf(wmx.y + 0.5f));
        bevel(dl, a, b, IM_COL32(212, 208, 200, 255), IM_COL32(255, 255, 255, 255), IM_COL32(64, 64, 64, 255), IM_COL32(128, 128, 128, 255), true);
        if (titled && w->HasCloseButton) {
            float pad = g.Style.FramePadding.x, sz = g.FontSize;
            ImVec2 ca(floorf(wmx.x - pad - sz), floorf(wmn.y + g.Style.FramePadding.y)), cb(ca.x + sz, ca.y + sz);
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool down = ImGui::GetIO().MouseDown[0] && mp.x >= ca.x && mp.x < cb.x && mp.y >= ca.y && mp.y < cb.y && g.HoveredWindow == w;
            dl->AddRectFilled(ca, cb, IM_COL32(212, 208, 200, 255));
            if (down)
                skin_pressed(dl, ca, cb);
            else
                skin_raised(dl, ca, cb);
            float cx = floorf((ca.x + cb.x) * 0.5f) + (down ? 1.0f : 0.0f), cy = floorf((ca.y + cb.y) * 0.5f) + (down ? 1.0f : 0.0f), s = floorf(sz * 0.25f);
            dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s + 1, cy + s + 1), IM_COL32(0, 0, 0, 255), 2.0f);
            dl->AddLine(ImVec2(cx + s, cy - s), ImVec2(cx - s - 1, cy + s + 1), IM_COL32(0, 0, 0, 255), 2.0f);
        }
    }
    if (tip) {
        dl->AddRect(wmn, wmx, IM_COL32(0, 0, 0, 255));
    }
    if (toolbar) {
        box(dl, wmn.x, wmn.y, wmx.x, wmn.y + 1, IM_COL32(255, 255, 255, 255));
        box(dl, wmn.x, wmx.y - 2, wmx.x, wmx.y - 1, IM_COL32(128, 128, 128, 255));
        box(dl, wmn.x, wmx.y - 1, wmx.x, wmx.y, IM_COL32(255, 255, 255, 255));
    }
    if (menubar) {
        box(dl, wmn.x, wmx.y - 2, wmx.x, wmx.y - 1, IM_COL32(128, 128, 128, 255));
        box(dl, wmn.x, wmx.y - 1, wmx.x, wmx.y, IM_COL32(255, 255, 255, 255));
    }
    if (status)
        skin_sunken(dl, ImVec2(wmn.x + 2, wmn.y + 1), ImVec2(wmx.x - 2, wmx.y - 1));
    dl->PopClipRect();
}

void ui_skin_apply()
{
    s_arrows.clear();
    if (!s_on)
        return;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow *w = g.Windows[i];
        if (!w->Active || w->Hidden)
            continue;
        if ((w->Flags & ImGuiWindowFlags_Tooltip) && w->DrawList->VtxBuffer.Size > 0) {
            std::vector<SkQuad> qs;
            collect(w->DrawList, qs);
            for (size_t k = 0; k < qs.size(); k++)
                if (qs[k].white && rgb_of(qs[k].col) == SK_FACE) {
                    for (int m = 0; m < 4; m++)
                        w->DrawList->VtxBuffer.Data[qs[k].vtx[m]].col = IM_COL32(255, 255, 225, 255);
                    break;
                }
        }
        skin_window(w);
    }
}

static void scroll_by(ImGuiID id, int dir, float amount)
{
    ImGuiWindow *w = ImGui::FindWindowByID(id);
    if (!w)
        return;
    if (dir == 0)
        ImGui::SetScrollY(w, w->Scroll.y - amount);
    else if (dir == 1)
        ImGui::SetScrollY(w, w->Scroll.y + amount);
    else if (dir == 2)
        ImGui::SetScrollX(w, w->Scroll.x - amount);
    else
        ImGui::SetScrollX(w, w->Scroll.x + amount);
}

bool ui_skin_mouse(int button, bool down, float x, float y)
{
    if (!s_on || button != 0)
        return false;
    if (!down) {
        bool had = s_cap_win != 0;
        s_cap_win = 0;
        return had;
    }
    ImGuiContext &g = *ImGui::GetCurrentContext();
    for (size_t i = 0; i < s_arrows.size(); i++) {
        const SkArrow &a = s_arrows[i];
        if (x < a.mn.x || x >= a.mx.x || y < a.mn.y || y >= a.mx.y)
            continue;
        ImGuiWindow *w = ImGui::FindWindowByID(a.win);
        if (!w || g.HoveredWindow != w)
            continue;
        s_cap_win = a.win;
        s_cap_dir = a.dir;
        s_cap_t = 0;
        s_cap_next = 0.35;
        scroll_by(a.win, a.dir, g.FontSize * 2.0f);
        return true;
    }
    return false;
}

void ui_skin_tick(float dt)
{
    if (!s_cap_win)
        return;
    s_cap_t += dt;
    while (s_cap_t >= s_cap_next) {
        s_cap_next += 0.05;
        scroll_by(s_cap_win, s_cap_dir, ImGui::GetCurrentContext()->FontSize * 2.0f);
    }
}
