#ifndef UI_CLASSIC_H
#define UI_CLASSIC_H

#include "ui_tiles.h"
#include "ui_skin.h"

static ImVec4 rgb8(int r, int g, int b, int a = 255)
{
    return ImVec4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
}

static void ui_theme(bool dark)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    if (dark)
        ImGui::StyleColorsDark();
    else
        ImGui::StyleColorsLight();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.GrabRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.WindowBorderSize = dark ? 1.0f : 0.0f;
    s.ChildBorderSize = dark ? 1.0f : 0.0f;
    s.PopupBorderSize = dark ? 1.0f : 0.0f;
    s.FrameBorderSize = dark ? 1.0f : 0.0f;
    s.TabBorderSize = 0.0f;
    s.WindowPadding = ImVec2(6, 6);
    s.FramePadding = ImVec2(5, 2);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(4, 3);
    s.ScrollbarSize = 16.0f;
    s.GrabMinSize = 10.0f;
    s.WindowTitleAlign = dark ? ImVec2(0.5f, 0.5f) : ImVec2(0.0f, 0.5f);
    s.IndentSpacing = 12.0f;
    ui_skin_enable(!dark);
    ImVec4 *c = s.Colors;
    if (!dark) {
        ui_skin_colors(s);
        return;
    }
    c[ImGuiCol_Text] = rgb8(230, 230, 232);
    c[ImGuiCol_TextDisabled] = rgb8(135, 135, 140);
    c[ImGuiCol_WindowBg] = rgb8(53, 53, 53);
    c[ImGuiCol_ChildBg] = rgb8(42, 42, 42);
    c[ImGuiCol_PopupBg] = rgb8(45, 45, 45);
    c[ImGuiCol_Border] = rgb8(24, 24, 24);
    c[ImGuiCol_FrameBg] = rgb8(42, 42, 42);
    c[ImGuiCol_FrameBgHovered] = rgb8(52, 52, 56);
    c[ImGuiCol_FrameBgActive] = rgb8(60, 60, 66);
    c[ImGuiCol_TitleBg] = rgb8(32, 32, 32);
    c[ImGuiCol_TitleBgActive] = rgb8(32, 32, 32);
    c[ImGuiCol_TitleBgCollapsed] = rgb8(32, 32, 32);
    c[ImGuiCol_MenuBarBg] = rgb8(40, 40, 40);
    c[ImGuiCol_Button] = rgb8(72, 72, 74);
    c[ImGuiCol_ButtonHovered] = rgb8(86, 86, 90);
    c[ImGuiCol_ButtonActive] = rgb8(42, 130, 218);
    c[ImGuiCol_Header] = rgb8(30, 104, 190);
    c[ImGuiCol_HeaderHovered] = rgb8(50, 120, 205);
    c[ImGuiCol_HeaderActive] = rgb8(42, 130, 218);
    c[ImGuiCol_CheckMark] = rgb8(70, 150, 235);
    c[ImGuiCol_SliderGrab] = rgb8(42, 130, 218);
    c[ImGuiCol_SliderGrabActive] = rgb8(80, 160, 240);
    c[ImGuiCol_Separator] = rgb8(80, 80, 84);
    c[ImGuiCol_ScrollbarBg] = rgb8(36, 36, 36);
    c[ImGuiCol_ScrollbarGrab] = rgb8(96, 96, 100);
    c[ImGuiCol_Tab] = rgb8(44, 44, 46);
    c[ImGuiCol_TabHovered] = rgb8(42, 130, 218);
    c[ImGuiCol_TabSelected] = rgb8(30, 104, 190);
    c[ImGuiCol_TabSelectedOverline] = rgb8(70, 150, 235);
    c[ImGuiCol_TabDimmed] = rgb8(44, 44, 46);
    c[ImGuiCol_TabDimmedSelected] = rgb8(30, 104, 190);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb8(70, 150, 235);
    c[ImGuiCol_TableHeaderBg] = rgb8(60, 60, 64);
    c[ImGuiCol_TableBorderStrong] = rgb8(24, 24, 24);
    c[ImGuiCol_TableBorderLight] = rgb8(40, 40, 42);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextSelectedBg] = rgb8(42, 130, 218, 120);
}

enum Icon {
    IC_NEW, IC_OPEN, IC_SAVE, IC_CUT, IC_COPY, IC_PASTE, IC_UNDO, IC_REDO, IC_SELECT, IC_TERRAIN, IC_DOODAD,
    IC_UNIT, IC_WILD, IC_OBJED, IC_TEST, IC_INFO, IC_PLAYERS, IC_PROPS, IC_FOLDER, IC_CIRCLE, IC_SQUARE,
    IC_RAISE, IC_LOWER, IC_SMOOTH, IC_FLATTEN, IC_PLATEAU, IC_NOISE, IC_GRID, IC_DELETE,
    IC_CAVE, IC_CAVE_ERASE, IC_LEVEL_UP, IC_LEVEL_DOWN, IC_RAMP
};

static void dashed(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float t, float seg)
{
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    int n = std::max(1, (int)(l / seg));
    for (int i = 0; i < n; i += 2) {
        float u0 = (float)i / (float)n, u1 = std::min(1.0f, (float)(i + 1) / (float)n);
        dl->AddLine(ImVec2(a.x + dx * u0, a.y + dy * u0), ImVec2(a.x + dx * u1, a.y + dy * u1), col, t);
    }
}

static void draw_icon(ImDrawList *dl, int ic, ImVec2 c, float s, ImU32 col)
{
    float t = std::max(1.0f, s / 13.0f);
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 0.5f) * s, c.y + (y - 0.5f) * s); };
    const ImU32 blue = IM_COL32(40, 110, 220, 255), green = IM_COL32(60, 160, 70, 255), red = IM_COL32(205, 60, 50, 255);
    const ImU32 orange = IM_COL32(235, 150, 40, 255), brown = IM_COL32(140, 95, 50, 255), yellow = IM_COL32(240, 200, 60, 255);
    switch (ic) {
    case IC_NEW:
        dl->AddRectFilled(P(0.22f, 0.08f), P(0.78f, 0.92f), IM_COL32(250, 250, 250, 255));
        dl->AddRect(P(0.22f, 0.08f), P(0.78f, 0.92f), col, 0, 0, t);
        break;
    case IC_OPEN:
        dl->AddRectFilled(P(0.08f, 0.3f), P(0.92f, 0.85f), yellow);
        dl->AddRect(P(0.08f, 0.3f), P(0.92f, 0.85f), brown, 0, 0, t);
        dl->AddRectFilled(P(0.08f, 0.2f), P(0.4f, 0.32f), yellow);
        break;
    case IC_SAVE:
        dl->AddRectFilled(P(0.12f, 0.12f), P(0.88f, 0.88f), blue);
        dl->AddRectFilled(P(0.3f, 0.12f), P(0.7f, 0.42f), IM_COL32(240, 240, 240, 255));
        dl->AddRectFilled(P(0.3f, 0.6f), P(0.7f, 0.88f), IM_COL32(200, 210, 230, 255));
        break;
    case IC_CUT:
        dl->AddLine(P(0.3f, 0.1f), P(0.7f, 0.65f), col, t);
        dl->AddLine(P(0.7f, 0.1f), P(0.3f, 0.65f), col, t);
        dl->AddCircle(P(0.28f, 0.78f), s * 0.15f, red, 12, t);
        dl->AddCircle(P(0.72f, 0.78f), s * 0.15f, red, 12, t);
        break;
    case IC_COPY:
        dl->AddRect(P(0.12f, 0.12f), P(0.6f, 0.7f), col, 0, 0, t);
        dl->AddRectFilled(P(0.38f, 0.3f), P(0.88f, 0.9f), IM_COL32(250, 250, 250, 255));
        dl->AddRect(P(0.38f, 0.3f), P(0.88f, 0.9f), col, 0, 0, t);
        break;
    case IC_PASTE:
        dl->AddRectFilled(P(0.15f, 0.15f), P(0.85f, 0.92f), brown);
        dl->AddRectFilled(P(0.3f, 0.3f), P(0.7f, 0.85f), IM_COL32(250, 250, 250, 255));
        dl->AddRectFilled(P(0.35f, 0.08f), P(0.65f, 0.25f), IM_COL32(120, 120, 120, 255));
        break;
    case IC_UNDO:
    case IC_REDO: {
        float sg = ic == IC_UNDO ? 1.0f : -1.0f;
        dl->PathArcTo(P(0.5f, 0.58f), s * 0.3f, ic == IC_UNDO ? -3.0f : -0.14f, ic == IC_UNDO ? 0.2f : 3.0f, 14);
        dl->PathStroke(blue, 0, t * 1.4f);
        dl->AddTriangleFilled(P(0.5f - 0.43f * sg, 0.55f), P(0.5f - 0.13f * sg, 0.55f), P(0.5f - 0.28f * sg, 0.27f), blue);
        break;
    }
    case IC_SELECT:
        dashed(dl, P(0.12f, 0.12f), P(0.88f, 0.12f), col, t, s * 0.12f);
        dashed(dl, P(0.88f, 0.12f), P(0.88f, 0.88f), col, t, s * 0.12f);
        dashed(dl, P(0.88f, 0.88f), P(0.12f, 0.88f), col, t, s * 0.12f);
        dashed(dl, P(0.12f, 0.88f), P(0.12f, 0.12f), col, t, s * 0.12f);
        break;
    case IC_TERRAIN: {
        ImVec2 m[5] = { P(0.05f, 0.85f), P(0.32f, 0.3f), P(0.5f, 0.6f), P(0.68f, 0.2f), P(0.95f, 0.85f) };
        dl->AddConvexPolyFilled(m, 5, IM_COL32(150, 120, 80, 255));
        dl->AddPolyline(m, 5, brown, ImDrawFlags_Closed, t);
        break;
    }
    case IC_DOODAD:
        dl->AddRectFilled(P(0.44f, 0.62f), P(0.56f, 0.92f), brown);
        dl->AddTriangleFilled(P(0.5f, 0.06f), P(0.18f, 0.66f), P(0.82f, 0.66f), green);
        break;
    case IC_UNIT:
        dl->AddCircleFilled(P(0.5f, 0.25f), s * 0.17f, blue);
        dl->AddTriangleFilled(P(0.5f, 0.44f), P(0.18f, 0.92f), P(0.82f, 0.92f), blue);
        break;
    case IC_WILD:
        dl->AddTriangleFilled(P(0.5f, 0.1f), P(0.12f, 0.85f), P(0.88f, 0.85f), yellow);
        dl->AddTriangle(P(0.5f, 0.1f), P(0.12f, 0.85f), P(0.88f, 0.85f), brown, t);
        break;
    case IC_OBJED:
        for (int i = 0; i < 3; i++) {
            float y = 0.2f + 0.3f * (float)i;
            dl->AddRectFilled(P(0.12f, y - 0.07f), P(0.28f, y + 0.07f), orange);
            dl->AddLine(P(0.38f, y), P(0.9f, y), col, t);
        }
        break;
    case IC_TEST:
        dl->AddRectFilled(P(0.1f, 0.15f), P(0.9f, 0.85f), red);
        dl->AddTriangleFilled(P(0.38f, 0.28f), P(0.38f, 0.72f), P(0.74f, 0.5f), IM_COL32(255, 255, 255, 255));
        break;
    case IC_INFO:
        dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.42f, blue);
        dl->AddCircleFilled(P(0.5f, 0.28f), s * 0.07f, IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(P(0.45f, 0.42f), P(0.55f, 0.76f), IM_COL32(255, 255, 255, 255));
        break;
    case IC_PLAYERS:
        dl->AddCircleFilled(P(0.32f, 0.32f), s * 0.14f, blue);
        dl->AddTriangleFilled(P(0.32f, 0.48f), P(0.06f, 0.9f), P(0.58f, 0.9f), blue);
        dl->AddCircleFilled(P(0.7f, 0.36f), s * 0.14f, red);
        dl->AddTriangleFilled(P(0.7f, 0.52f), P(0.44f, 0.92f), P(0.96f, 0.92f), red);
        break;
    case IC_PROPS:
        dl->AddRect(P(0.1f, 0.1f), P(0.9f, 0.9f), col, 0, 0, t);
        for (int i = 0; i < 3; i++) {
            float y = 0.3f + 0.2f * (float)i;
            dl->AddLine(P(0.2f, y), P(0.8f, y), col, t);
        }
        break;
    case IC_FOLDER:
        dl->AddRectFilled(P(0.05f, 0.25f), P(0.95f, 0.85f), yellow);
        dl->AddRectFilled(P(0.05f, 0.15f), P(0.45f, 0.3f), yellow);
        dl->AddRect(P(0.05f, 0.25f), P(0.95f, 0.85f), brown, 0, 0, std::max(1.0f, t * 0.7f));
        break;
    case IC_CIRCLE:
        dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.36f, blue);
        break;
    case IC_SQUARE:
        dl->AddRectFilled(P(0.15f, 0.15f), P(0.85f, 0.85f), blue);
        break;
    case IC_RAISE:
    case IC_LOWER: {
        bool up = ic == IC_RAISE;
        dl->AddLine(P(0.1f, 0.85f), P(0.9f, 0.85f), col, t);
        float y0 = up ? 0.8f : 0.2f, y1 = up ? 0.2f : 0.8f;
        dl->AddLine(P(0.5f, y0), P(0.5f, y1), up ? green : red, t * 1.5f);
        dl->AddTriangleFilled(P(0.5f, y1), P(0.3f, up ? 0.42f : 0.58f), P(0.7f, up ? 0.42f : 0.58f), up ? green : red);
        break;
    }
    case IC_SMOOTH: {
        ImVec2 pts[12];
        for (int i = 0; i < 12; i++)
            pts[i] = P(0.08f + 0.84f * (float)i / 11.0f, 0.5f + 0.25f * sinf((float)i / 11.0f * 6.2832f));
        dl->AddPolyline(pts, 12, blue, 0, t * 1.5f);
        break;
    }
    case IC_FLATTEN:
        dl->AddLine(P(0.08f, 0.5f), P(0.92f, 0.5f), blue, t * 1.5f);
        dl->AddTriangleFilled(P(0.5f, 0.12f), P(0.34f, 0.34f), P(0.66f, 0.34f), col);
        dl->AddTriangleFilled(P(0.5f, 0.88f), P(0.34f, 0.66f), P(0.66f, 0.66f), col);
        break;
    case IC_PLATEAU: {
        ImVec2 m[4] = { P(0.05f, 0.85f), P(0.28f, 0.3f), P(0.72f, 0.3f), P(0.95f, 0.85f) };
        dl->AddConvexPolyFilled(m, 4, IM_COL32(150, 120, 80, 255));
        dl->AddPolyline(m, 4, brown, ImDrawFlags_Closed, t);
        break;
    }
    case IC_NOISE: {
        ImVec2 pts[8] = { P(0.05f, 0.6f), P(0.2f, 0.3f), P(0.33f, 0.7f), P(0.46f, 0.25f), P(0.6f, 0.75f), P(0.72f, 0.35f), P(0.85f, 0.65f), P(0.95f, 0.45f) };
        dl->AddPolyline(pts, 8, orange, 0, t * 1.5f);
        break;
    }
    case IC_CAVE:
    case IC_CAVE_ERASE: {
        ImVec2 a[10];
        for (int i = 0; i < 10; i++) {
            float u = 3.14159265f * (float)i / 9.0f;
            a[i] = P(0.5f - 0.42f * cosf(u), 0.82f - 0.62f * sinf(u));
        }
        dl->AddConvexPolyFilled(a, 10, IM_COL32(70, 60, 55, 255));
        dl->AddPolyline(a, 10, col, ImDrawFlags_Closed, t);
        ImVec2 m[6];
        for (int i = 0; i < 6; i++) {
            float u = 3.14159265f * (float)i / 5.0f;
            m[i] = P(0.5f - 0.2f * cosf(u), 0.82f - 0.34f * sinf(u));
        }
        dl->AddConvexPolyFilled(m, 6, IM_COL32(15, 12, 12, 255));
        if (ic == IC_CAVE_ERASE) {
            dl->AddLine(P(0.15f, 0.15f), P(0.85f, 0.85f), red, t * 1.6f);
            dl->AddLine(P(0.85f, 0.15f), P(0.15f, 0.85f), red, t * 1.6f);
        }
        break;
    }
    case IC_LEVEL_UP:
    case IC_LEVEL_DOWN: {
        bool up = ic == IC_LEVEL_UP;
        ImVec2 st[8] = { P(0.08f, 0.85f), P(0.08f, 0.62f), P(0.36f, 0.62f), P(0.36f, 0.4f), P(0.64f, 0.4f), P(0.64f, 0.18f), P(0.92f, 0.18f), P(0.92f, 0.85f) };
        dl->AddConvexPolyFilled(st, 8, IM_COL32(150, 120, 80, 255));
        dl->AddPolyline(st, 8, brown, ImDrawFlags_Closed, t);
        dl->AddTriangleFilled(up ? P(0.26f, 0.08f) : P(0.26f, 0.5f), up ? P(0.1f, 0.3f) : P(0.1f, 0.28f), up ? P(0.42f, 0.3f) : P(0.42f, 0.28f), up ? green : red);
        break;
    }
    case IC_RAMP: {
        ImVec2 w[3] = { P(0.08f, 0.8f), P(0.92f, 0.8f), P(0.92f, 0.22f) };
        dl->AddConvexPolyFilled(w, 3, IM_COL32(150, 120, 80, 255));
        dl->AddPolyline(w, 3, brown, ImDrawFlags_Closed, t);
        dl->AddLine(P(0.2f, 0.7f), P(0.8f, 0.34f), green, t * 1.4f);
        break;
    }
    case IC_GRID:
        for (int i = 0; i < 4; i++) {
            float v = 0.12f + 0.253f * (float)i;
            dl->AddLine(P(v, 0.12f), P(v, 0.88f), yellow, t);
            dl->AddLine(P(0.12f, v), P(0.88f, v), yellow, t);
        }
        break;
    case IC_DELETE:
        dl->AddLine(P(0.2f, 0.2f), P(0.8f, 0.8f), red, t * 1.6f);
        dl->AddLine(P(0.8f, 0.2f), P(0.2f, 0.8f), red, t * 1.6f);
        break;
    }
}

static void draw_glyph(ImDrawList *dl, int cat, ImVec2 c, float s, ImU32 fg)
{
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 0.5f) * s, c.y + (y - 0.5f) * s); };
    switch (cat) {
    case C_ALIEN: {
        ImVec2 m[4] = { P(0.5f, 0.1f), P(0.85f, 0.5f), P(0.5f, 0.9f), P(0.15f, 0.5f) };
        dl->AddConvexPolyFilled(m, 4, fg);
        break;
    }
    case C_MARINE: dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.34f, fg); break;
    case C_PRED: dl->AddTriangleFilled(P(0.12f, 0.2f), P(0.88f, 0.2f), P(0.5f, 0.88f), fg); break;
    case C_STRUCT:
        dl->AddRectFilled(P(0.2f, 0.45f), P(0.8f, 0.88f), fg);
        dl->AddTriangleFilled(P(0.1f, 0.48f), P(0.9f, 0.48f), P(0.5f, 0.12f), fg);
        break;
    case C_TREE:
        dl->AddTriangleFilled(P(0.5f, 0.08f), P(0.18f, 0.68f), P(0.82f, 0.68f), fg);
        dl->AddRectFilled(P(0.44f, 0.66f), P(0.56f, 0.92f), fg);
        break;
    case C_ROCK: dl->AddCircleFilled(P(0.5f, 0.55f), s * 0.32f, fg, 6); break;
    case C_PLANT:
        dl->AddCircleFilled(P(0.35f, 0.5f), s * 0.2f, fg);
        dl->AddCircleFilled(P(0.65f, 0.5f), s * 0.2f, fg);
        dl->AddCircleFilled(P(0.5f, 0.3f), s * 0.2f, fg);
        break;
    case C_CRATE:
        dl->AddRect(P(0.2f, 0.2f), P(0.8f, 0.8f), fg, 0, 0, std::max(1.0f, s / 8.0f));
        dl->AddLine(P(0.2f, 0.2f), P(0.8f, 0.8f), fg, std::max(1.0f, s / 8.0f));
        break;
    case C_WILD:
        dl->AddTriangleFilled(P(0.5f, 0.1f), P(0.14f, 0.85f), P(0.86f, 0.85f), fg);
        break;
    case C_START:
        dl->AddRectFilled(P(0.22f, 0.1f), P(0.3f, 0.9f), fg);
        dl->AddTriangleFilled(P(0.3f, 0.12f), P(0.85f, 0.32f), P(0.3f, 0.52f), fg);
        break;
    case C_ATMOS:
        dl->AddRectFilled(P(0.25f, 0.3f), P(0.75f, 0.85f), fg);
        dl->AddCircleFilled(P(0.5f, 0.3f), s * 0.25f, fg);
        break;
    default: dl->AddRectFilled(P(0.22f, 0.22f), P(0.78f, 0.78f), fg); break;
    }
}

static int item_race(const PalItem &it)
{
    for (int i = 0; i < COUNT(k_alien); i++) if (!strcmp(it.cls, k_alien[i])) return 1;
    for (int i = 0; i < COUNT(k_marine); i++) if (!strcmp(it.cls, k_marine[i])) return 2;
    for (int i = 0; i < COUNT(k_pred); i++) if (!strcmp(it.cls, k_pred[i])) return 3;
    static const char *const al[] = { "GHiveNode", "GEggSack", "GQueenTrap" };
    static const char *const ma[] = { "GSentryGun", "GCommLink", "GGenerator", "GDropPod" };
    static const char *const pr[] = { "GSkullKeeper", "GPredSentryGun", "GPredAntiAircraftGun", "GCloakerControlPod" };
    for (int i = 0; i < COUNT(al); i++) if (!strcmp(it.cls, al[i])) return 1;
    for (int i = 0; i < COUNT(ma); i++) if (!strcmp(it.cls, ma[i])) return 2;
    for (int i = 0; i < COUNT(pr); i++) if (!strcmp(it.cls, pr[i])) return 3;
    return 0;
}

static const char *const k_group_name[] = { "Units", "Structures", "Special", "Wildlife", "Trees", "Rocks", "Plants", "Crates", "Doodads" };

static int item_group(const PalItem &it)
{
    switch (it.cat) {
    case C_ALIEN: case C_MARINE: case C_PRED: return 0;
    case C_STRUCT: return 1;
    case C_START: case C_ATMOS: return 2;
    case C_WILD: return 3;
    case C_TREE: return 4;
    case C_ROCK: return 5;
    case C_PLANT: return 6;
    case C_CRATE: return 7;
    default: return 8;
    }
}

static const char *const k_race_filter[] = { "All Races", "Alien", "Marine", "Predator", "Neutral" };
static const char *const k_player_name[] = { "Neutral", "Blue", "Teal", "Orange", "Red", "Yellow", "Purple", "Green", "White" };
static const char *const k_pal_type_name[] = { "Unit Palette", "Doodad Palette", "Terrain Palette", "Wildlife Palette" };
static const char *const k_light_name[] = { "Dawn", "Noon", "Dusk", "Night" };
static const float k_light_val[] = { 0.75f, 1.0f, 0.65f, 0.35f };

static bool item_selected(const PalItem &it)
{
    return g_tool == T_PLACE && g_pal_kind == it.kind && (it.kind == K_START || it.kind == K_ATMOS || !strcmp(g_pal_cls, it.cls));
}

static int type_of_item(const PalItem &it)
{
    if (it.kind == K_WILD)
        return 3;
    if (it.cls[0] == '@')
        return 1;
    return 0;
}

static void pick_item(const PalItem &it)
{
    g_tool = T_PLACE;
    g_pal_kind = it.kind;
    if (it.kind == K_OBJ || it.kind == K_WILD)
        snprintf(g_pal_cls, sizeof g_pal_cls, "%s", it.cls);
    g_pal_type = type_of_item(it);
    g_win_palette = true;
}

static void set_palette(int t)
{
    pal_build();
    g_pal_type = t;
    g_win_palette = true;
    if (t == 2) {
        if (!is_brush_tool())
            g_tool = T_SCULPT;
        return;
    }
    bool doodad = g_pal_kind == K_OBJ && g_pal_cls[0] == '@';
    bool wild = g_pal_kind == K_WILD;
    bool ok = (t == 0 && !doodad && !wild && g_pal_kind != K_WILD) || (t == 1 && doodad) || (t == 3 && wild);
    if (!ok) {
        const char *want = t == 1 ? "@JoshTree2" : t == 3 ? "GRynth" : "GMarineInfantry";
        g_pal_kind = t == 3 ? K_WILD : K_OBJ;
        snprintf(g_pal_cls, sizeof g_pal_cls, "%s", want);
    }
    g_tool = T_PLACE;
}

static void set_tool(Tool t)
{
    if (t == T_SCULPT || t == T_PAINT || t == T_CAVE || t == T_CLIFF) {
        g_pal_type = 2;
        g_win_palette = true;
    }
    g_tool = t;
}

static void ui_hotkeys(bool ctrl, bool shift)
{
    (void)shift;
    auto hit = [&](int k) { return g_key_latch[k]; };
    auto cm = [&](int k) { return ctrl || (g_latch_mod[k] & 1) != 0; };
    int k;
    if (hit(PK_A + 26 + 1)) set_tool(T_SELECT);
    if (hit(PK_A + 26 + 2)) set_tool(T_SCULPT);
    if (hit(PK_A + 26 + 3)) set_tool(T_PAINT);
    if (hit(PK_A + 26 + 5)) { g_cave_mode = CV_PAINT; set_tool(T_CAVE); }
    if (hit(PK_A + 26 + 6)) set_tool(T_CLIFF);
    if (hit(PK_A + 26 + 4)) set_palette(0);
    if (hit(PK_F1 + 1)) g_win_info = !g_win_info;
    if (hit(PK_F1 + 2)) g_win_players = !g_win_players;
    if (hit(PK_F1 + 3)) g_win_props = !g_win_props;
    if (hit(PK_F1 + 5)) g_win_objects = !g_win_objects;
    if (hit(PK_F1 + 6)) g_overlay = g_overlay == 1 ? 0 : 1;
    if (hit(PK_F1 + 7)) g_overlay = g_overlay == 2 ? 0 : 2;
    k = PK_A + 'N' - 'A'; if (hit(k) && cm(k)) request_new_dialog();
    k = PK_A + 'O' - 'A'; if (hit(k) && cm(k)) request_open_dialog();
    k = PK_A + 'D' - 'A'; if (hit(k) && cm(k)) g_sel.clear();
}

static bool tb_btn(const char *label, bool on, const char *tooltip, bool enabled = true)
{
    ImGui::PushID(label);
    if (!enabled)
        ImGui::BeginDisabled();
    if (on)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool r = ImGui::Button(label);
    if (on)
        ImGui::PopStyleColor();
    if (!enabled)
        ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return r && enabled;
}

static void tb_sep()
{
    ImGui::SameLine(0, 3);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2), ImVec2(p.x, p.y + h - 2), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0, 3);
}

static void edit_cut()
{
    copy_selected();
    delete_selected();
}

static void main_menu()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) request_new_dialog();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) request_open_dialog();
        if (ImGui::BeginMenu("Open Recent", g_nrecent > 0)) {
            for (int i = 0; i < g_nrecent; i++)
                if (ImGui::MenuItem(g_recent[i])) {
                    char nm[NMAP_NAME];
                    snprintf(nm, sizeof nm, "%s", g_recent[i]);
                    do_open(nm);
                }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) save_or_ask();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) request_saveas_dialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Test in game", "F5")) do_test();
        if (ImGui::MenuItem("Open maps folder")) nmap_open_folder();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) g_want_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !g_undo.empty())) do_undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !g_redo.empty())) do_redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", NULL, false, !g_sel.empty())) edit_cut();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !g_sel.empty())) copy_selected();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g_clip.empty())) paste_at(g_cam.target.x, g_cam.target.y);
        if (ImGui::MenuItem("Delete", "Del", false, !g_sel.empty())) delete_selected();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            g_sel.clear();
            for (size_t i = 0; i < g_objs.size(); i++) g_sel.push_back((int)i);
        }
        if (ImGui::MenuItem("Deselect", "Ctrl+D", false, !g_sel.empty())) g_sel.clear();
        ImGui::Separator();
        if (ImGui::MenuItem("Properties...", "F4")) g_win_props = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Grid Lines", "H", &g_show_grid);
        if (ImGui::MenuItem("Pathing Overlay", "F7", g_overlay == 1)) g_overlay = g_overlay == 1 ? 0 : 1;
        if (ImGui::MenuItem("Height Overlay", "F8", g_overlay == 2)) g_overlay = g_overlay == 2 ? 0 : 2;
        ImGui::Separator();
        ImGui::MenuItem("Snap to Grid", "G", &g_snap);
        ImGui::MenuItem("Object Labels", "L", &g_labels);
        ImGui::MenuItem("Wildlife Spawners on Minimap", NULL, &g_show_wild);
        ImGui::MenuItem("Player Starts on Minimap", NULL, &g_show_starts);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Map", "F")) cam_frame();
        if (ImGui::MenuItem("Top-down Camera")) g_cam.pitch = 1.5f;
        if (ImGui::MenuItem("Reset Camera Tilt")) g_cam.pitch = 0.95f;
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Camera Speed", &g_cam_speed, 0.2f, 4.0f, "%.2fx");
        ImGui::Separator();
        ImGui::MenuItem("Left Panel", NULL, &g_show_left);
        ImGui::MenuItem("Toolbar", NULL, &g_show_toolbar);
        ImGui::MenuItem("Status Bar", NULL, &g_show_status);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layer")) {
        if (ImGui::MenuItem("Select", "1", g_tool == T_SELECT)) set_tool(T_SELECT);
        if (ImGui::MenuItem("Terrain: Height", "2", g_tool == T_SCULPT)) set_tool(T_SCULPT);
        if (ImGui::MenuItem("Terrain: Texture", "3", g_tool == T_PAINT)) set_tool(T_PAINT);
        if (ImGui::MenuItem("Terrain: Cave", "5", g_tool == T_CAVE)) set_tool(T_CAVE);
        if (ImGui::MenuItem("Terrain: Cliff and Level", "6", g_tool == T_CLIFF)) set_tool(T_CLIFF);
        if (ImGui::MenuItem("Doodads", NULL, g_tool == T_PLACE && g_pal_type == 1)) set_palette(1);
        if (ImGui::MenuItem("Units", "4", g_tool == T_PLACE && g_pal_type == 0)) set_palette(0);
        if (ImGui::MenuItem("Wildlife", NULL, g_tool == T_PLACE && g_pal_type == 3)) set_palette(3);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scenario")) {
        if (ImGui::MenuItem("Map Info...", "F2")) g_win_info = true;
        if (ImGui::MenuItem("Players and Forces...", "F3")) g_win_players = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Object Editor...", "F6")) g_win_objects = true;
        if (ImGui::MenuItem("Test in game", "F5")) do_test();
        if (ImGui::MenuItem("Open maps folder")) nmap_open_folder();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Advanced")) {
        if (ImGui::MenuItem("Preferences...")) g_win_prefs = true;
        if (ImGui::MenuItem("Dark Theme", NULL, g_dark)) {
            g_dark = !g_dark;
            ui_theme(g_dark);
        }
        if (ImGui::MenuItem("Reset Layout")) {
            g_show_toolbar = g_show_left = g_show_status = g_show_minimap = g_win_palette = true;
            g_win_objects = g_win_info = g_win_players = g_win_prefs = g_win_props = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Module")) {
        if (ImGui::MenuItem("Terrain", NULL, g_pal_type == 2 && g_win_palette)) set_palette(2);
        if (ImGui::MenuItem("Doodads", NULL, g_pal_type == 1 && g_win_palette)) set_palette(1);
        if (ImGui::MenuItem("Units", NULL, g_pal_type == 0 && g_win_palette)) set_palette(0);
        if (ImGui::MenuItem("Wildlife", NULL, g_pal_type == 3 && g_win_palette)) set_palette(3);
        ImGui::Separator();
        if (ImGui::MenuItem("Object Editor", "F6", g_win_objects)) g_win_objects = !g_win_objects;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Tool Palette", NULL, &g_win_palette);
        ImGui::MenuItem("Properties", "F4", &g_win_props);
        ImGui::MenuItem("Object Editor", "F6", &g_win_objects);
        ImGui::MenuItem("Map Info", "F2", &g_win_info);
        ImGui::MenuItem("Players and Forces", "F3", &g_win_players);
        ImGui::MenuItem("Minimap", NULL, &g_show_minimap);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) g_dlg_about = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

static void toolbar(float y, float h)
{
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2((float)g_w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 2));
    ImGui::Begin("##toolbar", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (tb_btn("New", false, "New map (Ctrl+N)")) request_new_dialog();
    ImGui::SameLine(); if (tb_btn("Open", false, "Open map (Ctrl+O)")) request_open_dialog();
    ImGui::SameLine(); if (tb_btn("Save", false, "Save map (Ctrl+S)")) save_or_ask();
    tb_sep();
    if (tb_btn("Cut", false, "Cut", !g_sel.empty())) edit_cut();
    ImGui::SameLine(); if (tb_btn("Copy", false, "Copy (Ctrl+C)", !g_sel.empty())) copy_selected();
    ImGui::SameLine(); if (tb_btn("Paste", false, "Paste (Ctrl+V)", !g_clip.empty())) paste_at(g_cam.target.x, g_cam.target.y);
    tb_sep();
    if (tb_btn("Undo", false, "Undo (Ctrl+Z)", !g_undo.empty())) do_undo();
    ImGui::SameLine(); if (tb_btn("Redo", false, "Redo (Ctrl+Y)", !g_redo.empty())) do_redo();
    tb_sep();
    if (tb_btn("Select", g_tool == T_SELECT, "Select (1)")) set_tool(T_SELECT);
    tb_sep();
    if (tb_btn("Terrain", is_brush_tool(), "Terrain palette (2, 3, 5, 6)")) set_palette(2);
    ImGui::SameLine(); if (tb_btn("Doodads", g_tool == T_PLACE && g_pal_type == 1, "Doodad palette")) set_palette(1);
    ImGui::SameLine(); if (tb_btn("Units", g_tool == T_PLACE && g_pal_type == 0, "Unit palette (4)")) set_palette(0);
    ImGui::SameLine(); if (tb_btn("Wildlife", g_tool == T_PLACE && g_pal_type == 3, "Wildlife palette")) set_palette(3);
    tb_sep();
    if (tb_btn("Grid", g_show_grid, "Grid lines (H)")) g_show_grid = !g_show_grid;
    ImGui::SameLine(); if (tb_btn("Properties", g_win_props, "Properties (F4)")) g_win_props = !g_win_props;
    ImGui::SameLine(); if (tb_btn("Objects", g_win_objects, "Object Editor (F6)")) g_win_objects = !g_win_objects;
    ImGui::SameLine(); if (tb_btn("Map Info", g_win_info, "Map Info (F2)")) g_win_info = !g_win_info;
    ImGui::SameLine(); if (tb_btn("Players", g_win_players, "Players and Forces (F3)")) g_win_players = !g_win_players;
    tb_sep();
    if (tb_btn("Test", false, "Test in game (F5)")) do_test();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

static bool tile_begin(const char *id, float sz, bool on, ImVec2 *p, const char *tooltip)
{
    *p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    bool hov = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p->x + sz, p->y + sz);
    if (g_dark) {
        dl->AddRectFilled(*p, q, IM_COL32(70, 70, 74, 255));
        dl->AddRect(*p, q, on ? IM_COL32(40, 110, 230, 255) : hov ? IM_COL32(120, 150, 210, 255) : ImGui::GetColorU32(ImGuiCol_Border), 0.0f, 0, on ? 2.5f : 1.0f);
    } else {
        bool dn = on || (hov && ImGui::IsMouseDown(0));
        dl->AddRectFilled(*p, q, dn ? IM_COL32(226, 222, 214, 255) : IM_COL32(212, 208, 200, 255));
        if (dn)
            skin_pressed(dl, *p, q);
        else
            skin_raised(dl, *p, q);
        if (hov && !dn)
            dl->AddRect(ImVec2(p->x + 3, p->y + 3), ImVec2(q.x - 3, q.y - 3), IM_COL32(0, 0, 128, 90));
    }
    if (hov && tooltip)
        ImGui::SetTooltip("%s", tooltip);
    return r;
}

static bool item_tile(const PalItem &it, float sz)
{
    ImVec2 p;
    char id[64];
    snprintf(id, sizeof id, "##t%s", it.cls);
    char tt[96];
    snprintf(tt, sizeof tt, "%s\n%s", item_label(it), it.cls);
    bool r = tile_begin(id, sz, item_selected(it), &p, tt);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    unsigned tex = ui_tile_texture(it.cls);
    int cat = it.kind == K_ATMOS ? C_ATMOS : it.cat;
    if (tex)
        dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + sz - 2, p.y + sz - 2));
    else {
        ImVec4 cc = cat_color(cat);
        dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + sz - 3, p.y + sz - 3), ImGui::ColorConvertFloat4ToU32(ImVec4(cc.x * 0.55f, cc.y * 0.55f, cc.z * 0.55f, 1)), 2.0f);
        draw_glyph(dl, cat, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), sz * 0.6f, ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(cc.x * 1.25f + 0.1f, 1.0f), std::min(cc.y * 1.25f + 0.1f, 1.0f), std::min(cc.z * 1.25f + 0.1f, 1.0f), 1)));
    }
    return r;
}

static void tile_grid(const std::vector<const PalItem *> &v, float sz)
{
    float sp = ImGui::GetStyle().ItemSpacing.x;
    int per = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (sz + sp)));
    for (size_t i = 0; i < v.size(); i++) {
        if (i % (size_t)per)
            ImGui::SameLine();
        if (item_tile(*v[i], sz))
            pick_item(*v[i]);
    }
}

static const char *const k_cliff_name[] = { "Raise Level", "Lower Level", "Ramp" };

static const char *brush_title(char *buf, size_t n)
{
    if (g_tool == T_SELECT) return "Select";
    if (g_tool == T_SCULPT) { snprintf(buf, n, "Terrain - %s", k_brush_name[g_brush]); return buf; }
    if (g_tool == T_PAINT) { snprintf(buf, n, "Terrain - %s", k_layer_name[g_layer]); return buf; }
    if (g_tool == T_CAVE) { snprintf(buf, n, "Terrain - %s", g_cave_mode == CV_ERASE ? "Erase Cave" : "Cave"); return buf; }
    if (g_tool == T_CLIFF) { snprintf(buf, n, "Terrain - %s", k_cliff_name[g_cliff_mode]); return buf; }
    const char *grp = g_pal_type == 1 ? "Doodads" : g_pal_type == 3 ? "Wildlife" : "Units";
    if (g_pal_kind == K_START) snprintf(buf, n, "%s - Start Location", grp);
    else if (g_pal_kind == K_ATMOS) snprintf(buf, n, "%s - Atmos Processor", grp);
    else snprintf(buf, n, "%s - %s", grp, cls_label(g_pal_cls));
    return buf;
}

static void preview_box()
{
    float w = ImGui::GetContentRegionAvail().x, h = w * 0.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pv", ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilledMultiColor(p, q, IM_COL32(90, 120, 170, 255), IM_COL32(90, 120, 170, 255), IM_COL32(30, 40, 60, 255), IM_COL32(30, 40, 60, 255));
    dl->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Border));
    const char *cls = NULL;
    int kind = K_OBJ, owner = 0;
    EObj tmp;
    if (g_tool == T_PLACE) {
        cls = g_pal_cls;
        kind = g_pal_kind;
        owner = g_pal_owner;
        memset(&tmp, 0, sizeof tmp);
        tmp.kind = kind;
        snprintf(tmp.cls, sizeof tmp.cls, "%s", kind == K_START ? "Player Start" : kind == K_ATMOS ? "GAtmosProcessor" : g_pal_cls);
    } else if (g_tool == T_SELECT && g_sel.size() == 1) {
        tmp = g_objs[(size_t)g_sel[0]];
        cls = tmp.cls;
        kind = tmp.kind;
        owner = tmp.kind == K_START ? tmp.num : tmp.owner;
    } else {
        ImVec2 ts = ImGui::CalcTextSize("No brush");
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(200, 210, 230, 255), "No brush");
        return;
    }
    unsigned tex = ui_preview_texture(cls, g_prev_rot, g_prev_dist);
    if (tex) {
        dl->AddImage((ImTextureID)(intptr_t)tex, p, q);
        return;
    }
    float rgbv[3];
    int mesh;
    V3 sc;
    int cat = category(tmp);
    cat_look(cat, rgbv, &mesh, &sc);
    if (kind == K_START) {
        const float *pc = k_player_rgb[owner >= 1 && owner <= 8 ? owner : 0];
        rgbv[0] = pc[0]; rgbv[1] = pc[1]; rgbv[2] = pc[2];
        mesh = 1;
        sc = v3(1.2f, 1.2f, 4.0f);
    } else if (owner > 0 && kind == K_OBJ) {
        const float *pc = k_player_rgb[owner];
        rgbv[0] = rgbv[0] * 0.5f + pc[0] * 0.5f;
        rgbv[1] = rgbv[1] * 0.5f + pc[1] * 0.5f;
        rgbv[2] = rgbv[2] * 0.5f + pc[2] * 0.5f;
    }
    if (kind == K_ATMOS) { mesh = 1; sc = v3(3.4f, 3.4f, 5.5f); }
    float maxd = std::max(std::max(sc.x, sc.y), sc.z);
    float zoom = (h * 0.62f / (maxd * 1.15f)) * (750.0f / g_prev_dist);
    int N = mesh == 0 ? 4 : 14;
    float ang = g_prev_rot * 0.0174533f + (mesh == 0 ? 0.785398f : 0.0f);
    const float pitch = 0.5f, cp = cosf(pitch), sp = sinf(pitch), ca = cosf(ang), sa = sinf(ang);
    struct Pt { V3 w; ImVec2 s; float d; };
    auto proj = [&](V3 v, float *depth) {
        float x1 = v.x * ca - v.y * sa, y1 = v.x * sa + v.y * ca;
        *depth = y1 * cp - (v.z - sc.z * 0.5f) * sp;
        return ImVec2(p.x + w * 0.5f + x1 * zoom, p.y + h * 0.58f - ((v.z - sc.z * 0.5f) * cp + y1 * sp) * zoom);
    };
    float rx = mesh == 0 ? sc.x * 0.7071f : sc.x * 0.5f, ry = mesh == 0 ? sc.y * 0.7071f : sc.y * 0.5f;
    std::vector<V3> bot((size_t)N), top((size_t)N);
    for (int i = 0; i < N; i++) {
        float a = (float)i / (float)N * 6.2831853f + (mesh == 0 ? 0.785398f : 0.0f);
        bot[(size_t)i] = v3(cosf(a) * rx, sinf(a) * ry, 0);
        top[(size_t)i] = v3(cosf(a) * rx * (mesh == 2 ? 0.0f : 1.0f), sinf(a) * ry * (mesh == 2 ? 0.0f : 1.0f), sc.z);
    }
    struct Face { ImVec2 pts[16]; int n; float depth; float shade; };
    std::vector<Face> faces;
    V3 L = norm(v3(0.4f, -0.5f, 0.8f));
    float lit = k_light_val[g_light_idx];
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        Face f;
        f.n = 4;
        float d0, d1, d2, d3;
        f.pts[0] = proj(bot[(size_t)i], &d0);
        f.pts[1] = proj(bot[(size_t)j], &d1);
        f.pts[2] = proj(top[(size_t)j], &d2);
        f.pts[3] = proj(top[(size_t)i], &d3);
        f.depth = (d0 + d1 + d2 + d3) * 0.25f;
        V3 e1 = bot[(size_t)j] - bot[(size_t)i], e2 = top[(size_t)i] - bot[(size_t)i];
        V3 nn = norm(cross(e1, e2));
        float nx = nn.x * ca - nn.y * sa, ny = nn.x * sa + nn.y * ca;
        (void)nx; (void)ny;
        f.shade = 0.35f + 0.65f * std::max(0.0f, dot(nn, L));
        faces.push_back(f);
    }
    if (mesh != 2) {
        Face f;
        f.n = std::min(N, 16);
        float dd = 0;
        for (int i = 0; i < f.n; i++) {
            float d;
            f.pts[i] = proj(top[(size_t)i], &d);
            dd += d;
        }
        f.depth = dd / (float)f.n - 100.0f;
        f.shade = 0.35f + 0.65f * std::max(0.0f, dot(v3(0, 0, 1), L));
        faces.push_back(f);
    }
    std::sort(faces.begin(), faces.end(), [](const Face &a, const Face &b) { return a.depth > b.depth; });
    float d0;
    ImVec2 gc = proj(v3(0, 0, 0), &d0);
    dl->AddEllipseFilled(ImVec2(gc.x, gc.y + 2), ImVec2(maxd * zoom * 0.7f, maxd * zoom * 0.22f), IM_COL32(0, 0, 0, 80), 0.0f, 24);
    for (size_t i = 0; i < faces.size(); i++) {
        float k = faces[i].shade * lit;
        dl->AddConvexPolyFilled(faces[i].pts, faces[i].n, IM_COL32((int)std::min(255.0f, rgbv[0] * k * 255), (int)std::min(255.0f, rgbv[1] * k * 255), (int)std::min(255.0f, rgbv[2] * k * 255), 255));
        dl->AddPolyline(faces[i].pts, faces[i].n, IM_COL32(0, 0, 0, 70), ImDrawFlags_Closed, 1.0f);
    }
}

static void spinner(const char *label, const char *value, int *dir)
{
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(w * 0.46f);
    ImGui::TextUnformatted(value);
    float bw = ImGui::GetFrameHeight();
    ImGui::SameLine(w - bw * 2.0f - 4);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::PushID(label);
    *dir = 0;
    if (ImGui::ArrowButton("##l", ImGuiDir_Left)) *dir = -1;
    ImGui::SameLine(0, 2);
    if (ImGui::ArrowButton("##r", ImGuiDir_Right)) *dir = 1;
    ImGui::PopID();
    ImGui::PopItemFlag();
}

static bool tree_node(const char *label, const char *idkey)
{
    char buf[128];
    snprintf(buf, sizeof buf, "     %s###%s", label, idkey);
    bool open = ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth);
    ImVec2 a = ImGui::GetItemRectMin();
    float fh = ImGui::GetFrameHeight(), sp = ImGui::GetTreeNodeToLabelSpacing();
    draw_icon(ImGui::GetWindowDrawList(), IC_FOLDER, ImVec2(a.x + sp * 0.9f + fh * 0.35f, a.y + fh * 0.5f), fh * 0.75f, ImGui::GetColorU32(ImGuiCol_Text));
    return open;
}

static void tree_leaf(const PalItem &it, int owner)
{
    char buf[128];
    snprintf(buf, sizeof buf, "     %s###%s", item_label(it), it.cls);
    bool on = item_selected(it) && (it.kind != K_OBJ || it.cls[0] == '@' || g_pal_owner == owner);
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | (on ? ImGuiTreeNodeFlags_Selected : 0);
    ImGui::TreeNodeEx(buf, fl);
    bool clicked = ImGui::IsItemClicked();
    ImVec2 a = ImGui::GetItemRectMin();
    float fh = ImGui::GetFrameHeight(), sp = ImGui::GetTreeNodeToLabelSpacing();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec4 cc = cat_color(it.kind == K_ATMOS ? C_ATMOS : it.cat);
    ImVec2 ic(a.x + sp * 0.9f + fh * 0.35f, a.y + fh * 0.5f);
    dl->AddRectFilled(ImVec2(ic.x - fh * 0.32f, ic.y - fh * 0.32f), ImVec2(ic.x + fh * 0.32f, ic.y + fh * 0.32f), ImGui::ColorConvertFloat4ToU32(cc), 2.0f);
    if (clicked) {
        g_pal_owner = it.kind == K_OBJ && it.cls[0] != '@' ? owner : g_pal_owner;
        pick_item(it);
    }
}

static void browser_tree()
{
    pal_build();
    for (int p = 1; p <= 8; p++) {
        char lab[48], key[16];
        snprintf(lab, sizeof lab, "Player %d (%s)", p, k_player_name[p]);
        snprintf(key, sizeof key, "pl%d", p);
        ImGui::PushID(p);
        if (tree_node(lab, key)) {
            for (int r = 1; r <= 3; r++) {
                if (tree_node(k_race_filter[r], k_race_filter[r])) {
                    for (int g = 0; g <= 1; g++) {
                        if (tree_node(k_group_name[g], k_group_name[g])) {
                            for (size_t i = 0; i < g_pal_items.size(); i++)
                                if (g_pal_items[i].kind == K_OBJ && item_race(g_pal_items[i]) == r && item_group(g_pal_items[i]) == g && g_pal_items[i].cls[0] != '@')
                                    tree_leaf(g_pal_items[i], p);
                            ImGui::TreePop();
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (tree_node("Neutral", "neutral")) {
        for (size_t i = 0; i < g_pal_items.size(); i++)
            if (g_pal_items[i].kind == K_OBJ && item_race(g_pal_items[i]) == 0 && item_group(g_pal_items[i]) == 1 && g_pal_items[i].cls[0] != '@')
                tree_leaf(g_pal_items[i], 0);
        ImGui::TreePop();
    }
    if (tree_node("Special", "special")) {
        for (size_t i = 0; i < g_pal_items.size(); i++)
            if (item_group(g_pal_items[i]) == 2)
                tree_leaf(g_pal_items[i], 0);
        ImGui::TreePop();
    }
    if (tree_node("Wildlife", "wildlife")) {
        for (size_t i = 0; i < g_pal_items.size(); i++)
            if (g_pal_items[i].kind == K_WILD)
                tree_leaf(g_pal_items[i], 0);
        ImGui::TreePop();
    }
    if (tree_node("Doodads", "doodads")) {
        for (int g = 4; g <= 8; g++) {
            if (tree_node(k_group_name[g], k_group_name[g])) {
                for (size_t i = 0; i < g_pal_items.size(); i++)
                    if (g_pal_items[i].cls[0] == '@' && item_group(g_pal_items[i]) == g)
                        tree_leaf(g_pal_items[i], 0);
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }
    if (tree_node("Terrain", "terrain")) {
        for (int i = 0; i < NMAP_LAYERS; i++) {
            char buf[64];
            snprintf(buf, sizeof buf, "     %s###tx%d", k_layer_name[i], i);
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | (g_tool == T_PAINT && g_layer == i ? ImGuiTreeNodeFlags_Selected : 0);
            ImGui::TreeNodeEx(buf, fl);
            bool clicked = ImGui::IsItemClicked();
            ImVec2 a = ImGui::GetItemRectMin();
            float fh = ImGui::GetFrameHeight(), sp = ImGui::GetTreeNodeToLabelSpacing();
            ImVec2 ic(a.x + sp * 0.9f + fh * 0.35f, a.y + fh * 0.5f);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ic.x - fh * 0.32f, ic.y - fh * 0.32f), ImVec2(ic.x + fh * 0.32f, ic.y + fh * 0.32f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(k_layer_rgb[i][0], k_layer_rgb[i][1], k_layer_rgb[i][2], 1)), 2.0f);
            if (clicked) {
                g_layer = i;
                set_tool(T_PAINT);
            }
        }
        ImGui::TreePop();
    }
}

static void left_panel(float y, float h)
{
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2(g_left_w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
    ImGui::Begin("##left", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (g_show_minimap) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        minimap_panel();
        ImVec2 b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border));
    }
    ImGui::Checkbox("Wildlife Spawners", &g_show_wild);
    ImGui::Checkbox("Player Starts", &g_show_starts);
    bool pth = g_overlay == 1;
    if (ImGui::Checkbox("Pathing Overlay", &pth))
        g_overlay = pth ? 1 : 0;
    char bb[96];
    ImGui::Separator();
    ImGui::TextWrapped("%s", g_tool == T_PLACE ? (g_pal_kind == K_START ? "Start Location" : g_pal_kind == K_ATMOS ? "Atmos Processor" : cls_label(g_pal_cls)) : brush_title(bb, sizeof bb));
    g_prev_rot += ImGui::GetIO().DeltaTime * 25.0f;
    preview_box();
    int dir;
    char val[32];
    snprintf(val, sizeof val, "%d", (int)g_prev_dist);
    spinner("Distance:", val, &dir);
    g_prev_dist = std::min(std::max(g_prev_dist + (float)dir * 50.0f, 200.0f), 1500.0f);
    snprintf(val, sizeof val, "%d", ((int)g_prev_rot % 360 + 360) % 360);
    spinner("Rotate:", val, &dir);
    g_prev_rot += (float)dir * 15.0f;
    spinner("Lighting:", k_light_name[g_light_idx], &dir);
    g_light_idx = (g_light_idx + dir + 4) % 4;
    ImGui::BeginChild("##tree", ImVec2(0, 0), true);
    browser_tree();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

static void win_tool_palette(float top)
{
    if (!g_win_palette)
        return;
    pal_build();
    ImGui::SetNextWindowPos(ImVec2((float)g_w - 236, top + 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(226, 470), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Tool Palette", &g_win_palette, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    float tile = ImGui::GetFontSize() * 2.1f;
    int pt = g_pal_type;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pt", k_pal_type_name[pt])) {
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(k_pal_type_name[i], pt == i))
                set_palette(i);
        ImGui::EndCombo();
    }
    static char search[48];
    static int race_f, cat_f;
    if (pt == 0) {
        ImGui::SetNextItemWidth(-26);
        player_combo("##own", &g_pal_owner, -26);
        ImGui::SameLine();
        ImVec4 pc = player_vec(g_pal_owner);
        ImGui::ColorButton("##sw", pc, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(hw);
        ImGui::Combo("##race", &race_f, k_race_filter, 5);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        static const char *const cf[] = { "All Groups", "Units", "Structures", "Special" };
        ImGui::Combo("##cat", &cat_f, cf, 4);
    }
    if (pt == 0 || pt == 1 || pt == 3) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##s", "Search...", search, sizeof search);
        ImGui::BeginChild("##sections", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        int g0 = pt == 0 ? 0 : pt == 1 ? 4 : 3, g1 = pt == 0 ? 2 : pt == 1 ? 8 : 3;
        const char *hint = NULL;
        int shown = 0;
        for (int g = g0; g <= g1; g++) {
            if (pt == 0 && cat_f > 0 && g != cat_f - 1)
                continue;
            std::vector<const PalItem *> v;
            for (size_t i = 0; i < g_pal_items.size(); i++) {
                const PalItem &it = g_pal_items[i];
                if (item_group(it) != g)
                    continue;
                if (pt == 0 && race_f > 0) {
                    int r = item_race(it);
                    int want = race_f == 4 ? 0 : race_f;
                    if (r != want)
                        continue;
                }
                if (pt == 0 && g == 2 && race_f > 0 && race_f != 4)
                    continue;
                if (!icontains(item_label(it), search) && !icontains(it.cls, search))
                    continue;
                v.push_back(&it);
            }
            if (v.empty())
                continue;
            shown++;
            ImGui::TextUnformatted(k_group_name[g]);
            tile_grid(v, tile);
            ImGui::Spacing();
        }
        if (!shown)
            ImGui::TextDisabled("No matches");
        (void)hint;
        ImGui::EndChild();
        char bb[96];
        ImGui::TextDisabled("%s", g_tool == T_PLACE ? brush_title(bb, sizeof bb) : "Pick a tile to place");
    } else {
        ImGui::BeginChild("##sections", ImVec2(0, 0), true);
        ImGui::TextUnformatted("Textures");
        float sp = ImGui::GetStyle().ItemSpacing.x;
        int per = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (tile + sp)));
        for (int i = 0; i < NMAP_LAYERS; i++) {
            if (i % per)
                ImGui::SameLine();
            ImVec2 p;
            char id[24];
            snprintf(id, sizeof id, "##lay%d", i);
            if (tile_begin(id, tile, g_tool == T_PAINT && g_layer == i, &p, k_layer_name[i])) {
                g_layer = i;
                set_tool(T_PAINT);
            }
            char key[24];
            snprintf(key, sizeof key, "layer:%s", k_layer_name[i]);
            unsigned tex = ui_tile_texture(key);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (tex)
                dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + tile - 3, p.y + tile - 3));
            else
                dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + tile - 3, p.y + tile - 3), ImGui::ColorConvertFloat4ToU32(ImVec4(k_layer_rgb[i][0], k_layer_rgb[i][1], k_layer_rgb[i][2], 1)));
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Height Tools");
        static const int bic[6] = { IC_RAISE, IC_LOWER, IC_SMOOTH, IC_FLATTEN, IC_PLATEAU, IC_NOISE };
        for (int i = 0; i < 6; i++) {
            if (i % per)
                ImGui::SameLine();
            ImVec2 p;
            char id[24];
            snprintf(id, sizeof id, "##ht%d", i);
            if (tile_begin(id, tile, g_tool == T_SCULPT && g_brush == i, &p, k_brush_name[i])) {
                g_brush = (Brush)i;
                set_tool(T_SCULPT);
            }
            draw_icon(ImGui::GetWindowDrawList(), bic[i], ImVec2(p.x + tile * 0.5f, p.y + tile * 0.5f), tile * 0.7f, ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Cave Tools");
        for (int i = 0; i < 2; i++) {
            if (i)
                ImGui::SameLine();
            ImVec2 p;
            char id[24];
            snprintf(id, sizeof id, "##cv%d", i);
            if (tile_begin(id, tile, g_tool == T_CAVE && g_cave_mode == i, &p, i ? "Erase Cave" : "Cave")) {
                g_cave_mode = i;
                set_tool(T_CAVE);
            }
            draw_icon(ImGui::GetWindowDrawList(), i ? IC_CAVE_ERASE : IC_CAVE, ImVec2(p.x + tile * 0.5f, p.y + tile * 0.5f), tile * 0.7f, ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::TextUnformatted("Level Tools");
        static const int lic[3] = { IC_LEVEL_UP, IC_LEVEL_DOWN, IC_RAMP };
        for (int i = 0; i < 3; i++) {
            if (i)
                ImGui::SameLine();
            ImVec2 p;
            char id[24];
            snprintf(id, sizeof id, "##lv%d", i);
            if (tile_begin(id, tile, g_tool == T_CLIFF && g_cliff_mode == i, &p, k_cliff_name[i])) {
                g_cliff_mode = i;
                set_tool(T_CLIFF);
            }
            draw_icon(ImGui::GetWindowDrawList(), lic[i], ImVec2(p.x + tile * 0.5f, p.y + tile * 0.5f), tile * 0.7f, ImGui::GetColorU32(ImGuiCol_Text));
        }
        if (g_tool == T_CAVE) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##cd", &g_cave_depth, 0.0f, 80.0f, "Floor depth %.0f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##cwh", &g_cave_wall_h, 10.0f, 140.0f, "Wall height %.0f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##cww", &g_cave_wall_w, 1.0f, 4.0f, "Wall width %.0f");
            ImGui::Checkbox("Rock doodads on walls", &g_cave_doodads);
            ImGui::TextDisabled("Ctrl inverts. Walls are impassable.");
        }
        if (g_tool == T_CLIFF) {
            int st = g_hstep, lv = g_hlevels;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##ls", &st, 5, 100, "Level step %d"))
                g_hstep = st;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##ll", &lv, 1, 8, "Levels 0 to %d"))
                g_hlevels = lv;
            ImGui::TextDisabled("Range %d to %d. Ramps stay walkable.", g_hmin, g_hmax);
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Brush Shape");
        for (int i = 0; i < 2; i++) {
            if (i)
                ImGui::SameLine();
            ImVec2 p;
            char id[24];
            snprintf(id, sizeof id, "##sh%d", i);
            if (tile_begin(id, tile, g_shape == i, &p, i ? "Square" : "Circle"))
                g_shape = i;
            draw_icon(ImGui::GetWindowDrawList(), i ? IC_SQUARE : IC_CIRCLE, ImVec2(p.x + tile * 0.5f, p.y + tile * 0.5f), tile * 0.7f, ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::TextUnformatted("Brush Size");
        static const float sizes[6] = { 2, 4, 8, 12, 20, 30 };
        for (int i = 0; i < 6; i++) {
            if (i)
                ImGui::SameLine(0, 2);
            char l[8];
            snprintf(l, sizeof l, "%d", (int)sizes[i]);
            if (seg_button(l, g_brush_r == sizes[i], ImVec2(ImGui::GetFontSize() * 1.7f, 0)))
                g_brush_r = sizes[i];
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##sz", &g_brush_r, 1.0f, 40.0f, "Size %.1f");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##pw", &g_brush_s, 1.0f, 40.0f, g_tool == T_PAINT ? "Opacity %.1f" : "Power %.1f");
        ImGui::Checkbox("Fixed height", &g_fixed_h);
        if (g_fixed_h) {
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##fh", &g_fixed_hv, 0.1f, (float)g_hmin * NMAP_HZ, (float)g_hmax * NMAP_HZ, "Height %.1f");
        }
        ImGui::TextDisabled("Ctrl inverts the brush");
        ImGui::Spacing();
        ImGui::TextUnformatted("Overlay");
        int ov = g_overlay;
        ImGui::SetNextItemWidth(-1);
        const char *ovn[] = { "None", "Pathing (slope)", "Height" };
        if (ImGui::Combo("##ov", &ov, ovn, 3))
            g_overlay = ov;
        if (g_overlay == 1) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##sl", &g_slope_deg, 5.0f, 80.0f, "Max slope %.0f deg");
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

static void scrollbars(float x0, float y0, float x1, float y1, float sb)
{
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    for (int v = 0; v < 2; v++) {
        ImVec2 pos = v ? ImVec2(x1, y0) : ImVec2(x0, y1), size = v ? ImVec2(sb, y1 - y0) : ImVec2(x1 - x0, sb);
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::Begin(v ? "##sbv" : "##sbh", NULL, fl);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float total = v ? size.y : size.x;
        float track = total - sb * 2.0f;
        ImVec2 tp = v ? ImVec2(pos.x, pos.y + sb) : ImVec2(pos.x + sb, pos.y);
        ImVec2 ts = v ? ImVec2(sb, track) : ImVec2(track, sb);
        ImGui::SetCursorScreenPos(tp);
        ImGui::InvisibleButton("##t", ts);
        bool act = ImGui::IsItemActive();
        float len = std::max(sb, track * 0.2f);
        float frac = v ? 1.0f - g_cam.target.y / (float)(g_t.h - 1) : g_cam.target.x / (float)(g_t.w - 1);
        frac = std::min(std::max(frac, 0.0f), 1.0f);
        float off = frac * (track - len);
        ImVec2 t0 = v ? ImVec2(tp.x, tp.y + off) : ImVec2(tp.x + off, tp.y);
        ImVec2 t1 = v ? ImVec2(tp.x + sb, tp.y + off + len) : ImVec2(tp.x + off + len, tp.y + sb);
        ImVec2 a0 = pos, a1 = v ? ImVec2(pos.x + sb, pos.y + sb) : ImVec2(pos.x + sb, pos.y + sb);
        ImVec2 b0 = v ? ImVec2(pos.x, pos.y + size.y - sb) : ImVec2(pos.x + size.x - sb, pos.y);
        ImVec2 b1 = ImVec2(b0.x + sb, b0.y + sb);
        float step = 0;
        ImGui::SetCursorScreenPos(a0);
        ImGui::InvisibleButton("##a", ImVec2(sb, sb));
        bool adn = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0, true))
            step = v ? 6.0f : -6.0f;
        ImGui::SetCursorScreenPos(b0);
        ImGui::InvisibleButton("##b", ImVec2(sb, sb));
        bool bdn = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0, true))
            step = v ? -6.0f : 6.0f;
        if (g_dark) {
            dl->AddRectFilled(tp, ImVec2(tp.x + ts.x, tp.y + ts.y), ImGui::GetColorU32(ImGuiCol_ScrollbarBg));
            dl->AddRectFilled(t0, t1, act ? ImGui::GetColorU32(ImGuiCol_ScrollbarGrabActive) : ImGui::GetColorU32(ImGuiCol_ScrollbarGrab));
            dl->AddRectFilled(a0, a1, ImGui::GetColorU32(ImGuiCol_Button));
            dl->AddRectFilled(b0, b1, ImGui::GetColorU32(ImGuiCol_Button));
        } else {
            dl->AddRectFilled(tp, ImVec2(tp.x + ts.x, tp.y + ts.y), IM_COL32(232, 228, 220, 255));
            dl->AddRectFilled(t0, t1, IM_COL32(212, 208, 200, 255));
            if (act)
                skin_pressed(dl, t0, t1);
            else
                skin_raised(dl, t0, t1);
        }
        skin_arrow(dl, a0, a1, v ? 0 : 2, adn);
        skin_arrow(dl, b0, b1, v ? 1 : 3, bdn);
        if (step != 0) {
            if (v)
                g_cam.target.y = std::min(std::max(g_cam.target.y + step, 0.0f), (float)(g_t.h - 1));
            else
                g_cam.target.x = std::min(std::max(g_cam.target.x + step, 0.0f), (float)(g_t.w - 1));
            g_cam.target.z = height_at(g_cam.target.x, g_cam.target.y);
        }
        if (act) {
            ImVec2 m = ImGui::GetMousePos();
            float f = ((v ? m.y - tp.y : m.x - tp.x) - len * 0.5f) / (track - len);
            f = std::min(std::max(f, 0.0f), 1.0f);
            if (v)
                g_cam.target.y = (1.0f - f) * (float)(g_t.h - 1);
            else
                g_cam.target.x = f * (float)(g_t.w - 1);
            g_cam.target.z = height_at(g_cam.target.x, g_cam.target.y);
        }
        ImGui::End();
    }
    ImGui::PopStyleVar(2);
}

static void status_bar(float sbh, float x0)
{
    ImGui::SetNextWindowPos(ImVec2(0, (float)g_h - sbh));
    ImGui::SetNextWindowSize(ImVec2((float)g_w, sbh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    ImGui::Begin("##status", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    char bb[96], left[128], mid[128];
    snprintf(left, sizeof left, "Brush: %s", brush_title(bb, sizeof bb));
    if (g_sel.empty())
        snprintf(mid, sizeof mid, "Selection: None");
    else if (g_sel.size() == 1)
        snprintf(mid, sizeof mid, "Selection: %s", cls_label(g_objs[(size_t)g_sel[0]].cls));
    else
        snprintf(mid, sizeof mid, "Selection: %d objects", (int)g_sel.size());
    if (plat_time() - g_msg_t < 6.0)
        ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.1f, 1), "%s", g_msg);
    else
        ImGui::TextUnformatted(left);
    float cx = std::max(x0 + (float)g_w * 0.18f, ImGui::CalcTextSize(left).x + 40.0f);
    ImGui::SameLine(cx);
    ImGui::TextUnformatted(mid);
    V3 hit;
    char pos[160] = "";
    time_t tt = time(NULL);
    struct tm *lt = localtime(&tt);
    char clk[16] = "";
    if (lt)
        snprintf(clk, sizeof clk, "%02d:%02d", lt->tm_hour, lt->tm_min);
    if (!g_ui_hover && ray_terrain(mouse_ray(g_mx, g_my), &hit))
        snprintf(pos, sizeof pos, "x %.1f  y %.1f  h %.1f    ", hit.x, hit.y, hit.z);
    char right[240];
    snprintf(right, sizeof right, "%sMap: %dx%d%s    Time: %s", pos, g_t.w, g_t.h, g_dirty ? " *" : "", clk);
    float tw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(std::max(cx + ImGui::CalcTextSize(mid).x + 20.0f, (float)g_w - tw - 12.0f));
    ImGui::TextUnformatted(right);
    ImGui::End();
    ImGui::PopStyleVar();
}

static void selection_rings()
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    for (size_t k = 0; k < g_sel.size(); k++) {
        if (g_sel[k] < 0 || (size_t)g_sel[k] >= g_objs.size())
            continue;
        const EObj &o = g_objs[(size_t)g_sel[k]];
        int pl = o.kind == K_START ? o.num : o.owner;
        ImU32 col = pl > 0 ? player_col(pl) : IM_COL32(70, 150, 255, 255);
        float r = pick_radius(o) * 1.35f;
        const int N = 40;
        ImVec2 pts[N];
        bool ok = true;
        for (int i = 0; i < N; i++) {
            float a = (float)i / (float)N * 6.2831853f;
            float x = o.x + cosf(a) * r, y = o.y + sinf(a) * r, sx, sy, d;
            if (!project(g_vp, v3(x, y, height_at(x, y) + 0.25f), g_w, g_h, &sx, &sy, &d)) {
                ok = false;
                break;
            }
            pts[i] = ImVec2(sx, sy);
        }
        if (!ok)
            continue;
        dl->AddPolyline(pts, N, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 5.0f);
        dl->AddPolyline(pts, N, col, ImDrawFlags_Closed, 3.0f);
    }
}

static void win_props()
{
    if (!g_win_props)
        return;
    ImGui::SetNextWindowSize(ImVec2(260, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(g_left_w + 20, 110), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Properties", &g_win_props))
        inspector_panel();
    ImGui::End();
}

static char g_title[160];

static void menu_and_panels()
{
    main_menu();
    float mb = ImGui::GetFrameHeight();
    float tbh = g_show_toolbar ? ImGui::GetFrameHeight() + 8.0f : 0.0f;
    float sbh = g_show_status ? ImGui::GetFrameHeight() + 4.0f : 0.0f;
    float sw = 16.0f;
    float y0 = mb + tbh, x0 = g_show_left ? g_left_w : 0.0f;
    g_vx0 = x0;
    g_vy0 = y0;
    g_vx1 = (float)g_w - sw;
    g_vy1 = (float)g_h - sbh - sw;
    if (g_show_toolbar)
        toolbar(mb, tbh);
    if (g_show_left)
        left_panel(y0, (float)g_h - y0 - sbh);
    scrollbars(g_vx0, g_vy0, g_vx1, g_vy1, sw);
    if (g_show_status)
        status_bar(sbh, x0);
    selection_rings();
    win_tool_palette(y0);
    win_props();
    win_map_info();
    win_players();
    win_prefs();
    win_objects();
    dialogs();
    char t[160];
    snprintf(t, sizeof t, "Nostromo Editor - %s%s", g_name[0] ? g_name : "Untitled", g_dirty ? " *" : "");
    if (strcmp(t, g_title)) {
        snprintf(g_title, sizeof g_title, "%s", t);
        plat_set_title(g_title);
    }
}

#endif
