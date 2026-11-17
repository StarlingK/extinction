#ifndef UI_PANELS_H
#define UI_PANELS_H

static const char *const k_cat_name[] = {
    "Aliens", "Marines", "Predators", "Structures", "Trees", "Rocks", "Plants", "Crates", "Doodads", "Atmos", "Special", "Wildlife"
};
static const char *const k_kind_name[] = { "Object", "Player Start", "Atmos Processor", "Wildlife Spawner" };
static const char *const k_race_name[] = { "Any", "Alien", "Marine", "Predator" };
static const char *const k_brush_name[] = { "Raise", "Lower", "Smooth", "Flatten", "Plateau", "Noise" };

static bool g_show_toolbar = true, g_show_left = true, g_show_status = true, g_show_minimap = true;
static bool g_win_objects, g_win_info, g_win_players, g_win_prefs, g_win_palette = true, g_win_props;
static bool g_splash_shown;
static bool g_dark, g_show_wild = true, g_show_starts = true;
static float g_left_w = 190.0f;
static int g_pal_type;
static float g_prev_dist = 750.0f, g_prev_rot;
static int g_light_idx = 1;
static int g_new_w = 128, g_new_h = 128;
static char g_recent[8][NMAP_NAME];
static int g_nrecent;
static bool g_key_latch[128];
static unsigned char g_latch_mod[128];

struct PalItem {
    const char *cls;
    int kind;
    int cat;
};
static std::vector<PalItem> g_pal_items;
static void ui_theme(bool dark);

static const char *cls_label(const char *s)
{
    if (s[0] == '@' || (s[0] == 'G' && isupper((unsigned char)s[1])))
        return s + 1;
    return s;
}

static bool icontains(const char *h, const char *n)
{
    if (!n[0])
        return true;
    size_t hl = strlen(h), nl = strlen(n);
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)h[i + k]) == tolower((unsigned char)n[k]))
            k++;
        if (k == nl)
            return true;
    }
    return false;
}

static void pal_build()
{
    if (!g_pal_items.empty())
        return;
    struct Src { const char *const *a; int n; int kind; };
    Src src[] = {
        { k_alien, COUNT(k_alien), K_OBJ }, { k_marine, COUNT(k_marine), K_OBJ }, { k_pred, COUNT(k_pred), K_OBJ },
        { k_struct, COUNT(k_struct), K_OBJ }, { k_doodad, COUNT(k_doodad), K_OBJ }, { k_wild, COUNT(k_wild), K_WILD }
    };
    for (size_t s = 0; s < sizeof src / sizeof src[0]; s++)
        for (int i = 0; i < src[s].n; i++) {
            EObj o;
            memset(&o, 0, sizeof o);
            o.kind = src[s].kind;
            snprintf(o.cls, sizeof o.cls, "%s", src[s].a[i]);
            PalItem it = { src[s].a[i], src[s].kind, category(o) };
            g_pal_items.push_back(it);
        }
    PalItem st = { "Player Start", K_START, C_START }, at = { "GAtmosProcessor", K_ATMOS, C_START };
    g_pal_items.push_back(st);
    g_pal_items.push_back(at);
}

static ImVec4 cat_color(int c)
{
    float rgb[3];
    int m;
    V3 sc;
    cat_look(c, rgb, &m, &sc);
    return ImVec4(rgb[0], rgb[1], rgb[2], 1.0f);
}

static const char *item_label(const PalItem &it)
{
    if (it.kind == K_ATMOS)
        return "Atmos Processor";
    return cls_label(it.cls);
}

static void recent_add(const char *name)
{
    int at = -1;
    for (int i = 0; i < g_nrecent; i++)
        if (!strcmp(g_recent[i], name))
            at = i;
    if (at < 0) {
        if (g_nrecent < 8)
            g_nrecent++;
        at = g_nrecent - 1;
    }
    for (int i = at; i > 0; i--)
        memcpy(g_recent[i], g_recent[i - 1], NMAP_NAME);
    snprintf(g_recent[0], NMAP_NAME, "%s", name);
    cfg_save();
}

static void cfg_save()
{
    FILE *f = fopen("nostromo_editor.cfg", "wb");
    if (!f)
        return;
    fprintf(f, "cam_speed %.3f\nui_scale %.3f\nedge_scroll %d\nsnap %d\ngrid %.3f\nlabels %d\ndark %d\nsplash_shown %d\n",
        g_cam_speed, g_ui_scale, g_edge_scroll ? 1 : 0, g_snap ? 1 : 0, g_grid, g_labels ? 1 : 0, g_dark ? 1 : 0, g_splash_shown ? 1 : 0);
    for (int i = 0; i < g_nrecent; i++)
        fprintf(f, "recent %s\n", g_recent[i]);
    fclose(f);
}

static void cfg_load()
{
    FILE *f = fopen("nostromo_editor.cfg", "rb");
    char line[160];
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char key[32], val[96];
        if (sscanf(line, "%31s %95s", key, val) != 2)
            continue;
        float v = (float)atof(val);
        if (!strcmp(key, "cam_speed")) g_cam_speed = std::min(std::max(v, 0.2f), 4.0f);
        else if (!strcmp(key, "ui_scale")) g_ui_scale = std::min(std::max(v, 0.8f), 2.0f);
        else if (!strcmp(key, "edge_scroll")) g_edge_scroll = v != 0;
        else if (!strcmp(key, "snap")) g_snap = v != 0;
        else if (!strcmp(key, "grid")) g_grid = std::min(std::max(v, 0.25f), 16.0f);
        else if (!strcmp(key, "labels")) g_labels = v != 0;
        else if (!strcmp(key, "dark")) g_dark = v != 0;
        else if (!strcmp(key, "splash_shown")) g_splash_shown = v != 0;
        else if (!strcmp(key, "recent") && nmap_name_ok(val) && g_nrecent < 8)
            snprintf(g_recent[g_nrecent++], NMAP_NAME, "%s", val);
    }
    fclose(f);
}

static ImU32 player_col(int n)
{
    const float *c = k_player_rgb[n >= 1 && n <= 8 ? n : 0];
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 1));
}

static ImVec4 player_vec(int n)
{
    const float *c = k_player_rgb[n >= 1 && n <= 8 ? n : 0];
    return ImVec4(c[0], c[1], c[2], 1);
}

static void player_combo(const char *id, int *owner, float w)
{
    char cur[32];
    snprintf(cur, sizeof cur, *owner ? "Player %d" : "Neutral", *owner);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, cur)) {
        for (int i = 0; i <= 8; i++) {
            char l[32];
            snprintf(l, sizeof l, i ? "Player %d" : "Neutral", i);
            ImGui::PushStyleColor(ImGuiCol_Text, player_vec(i));
            bool pick = ImGui::Selectable(l, *owner == i);
            ImGui::PopStyleColor();
            if (pick)
                *owner = i;
        }
        ImGui::EndCombo();
    }
}

static int count_kind(int kind)
{
    int n = 0;
    for (size_t i = 0; i < g_objs.size(); i++)
        if (g_objs[i].kind == kind)
            n++;
    return n;
}

static int start_index(int num)
{
    for (size_t i = 0; i < g_objs.size(); i++)
        if (g_objs[i].kind == K_START && g_objs[i].num == num)
            return (int)i;
    return -1;
}

static void select_only(int i)
{
    g_sel.assign(1, i);
    g_tool = T_SELECT;
}

static void focus_obj(int i)
{
    const EObj &o = g_objs[(size_t)i];
    g_cam.target = v3(o.x, o.y, o.z);
}

static void request_open_dialog()
{
    g_dlg_open = true;
    g_nnames = nmap_list(g_names, 64);
    g_dlg_pick = -1;
}

static void request_new_dialog()
{
    g_dlg_new = true;
    snprintf(g_dlg_name, sizeof g_dlg_name, "%s", "my_map");
}

static void request_saveas_dialog()
{
    g_dlg_saveas = true;
    snprintf(g_dlg_name, sizeof g_dlg_name, "%s", g_name[0] ? g_name : "my_map");
}

static void save_or_ask()
{
    if (g_name[0])
        do_save(g_name);
    else
        request_saveas_dialog();
}

static void new_map_confirmed()
{
    if (!nmap_name_ok(g_dlg_name)) {
        say("Invalid map name (letters, digits, - and _)");
        return;
    }
    g_new_w = std::min(std::max(g_new_w, 64), 256);
    g_new_h = std::min(std::max(g_new_h, 64), 256);
    reset_doc("M01", g_new_w, g_new_h, g_dlg_layer);
    snprintf(g_name, sizeof g_name, "%s", g_dlg_name);
    g_dirty = true;
    g_dlg_new = false;
}

static bool seg_button(const char *label, bool on, ImVec2 size = ImVec2(0, 0))
{
    if (on)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool r = ImGui::Button(label, size);
    if (on)
        ImGui::PopStyleColor();
    return r;
}

static void tip(const char *t)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", t);
}

static void minimap_panel()
{
    float ms = ImGui::GetContentRegionAvail().x;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float mw = ms, mh = ms * (float)g_t.h / (float)g_t.w;
    if (mh > ms) {
        mh = ms;
        mw = ms * (float)g_t.w / (float)g_t.h;
    }
    ImGui::Image((ImTextureID)(intptr_t)g_mm_tex, ImVec2(mw, mh));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < g_objs.size(); i++) {
        const EObj &o = g_objs[i];
        float px = p0.x + o.x / (float)(g_t.w - 1) * mw, py = p0.y + (1.0f - o.y / (float)(g_t.h - 1)) * mh;
        if (o.kind == K_START) {
            if (!g_show_starts)
                continue;
            dl->AddCircleFilled(ImVec2(px, py), 4.0f, player_col(o.num));
            dl->AddCircle(ImVec2(px, py), 4.5f, IM_COL32(0, 0, 0, 255));
        } else if (o.kind == K_ATMOS)
            dl->AddRectFilled(ImVec2(px - 3, py - 3), ImVec2(px + 3, py + 3), IM_COL32(255, 160, 40, 255));
        else if (o.kind == K_WILD) {
            if (g_show_wild)
                dl->AddTriangleFilled(ImVec2(px, py - 4), ImVec2(px - 4, py + 3), ImVec2(px + 4, py + 3), IM_COL32(240, 210, 70, 255));
        } else
            dl->AddRectFilled(ImVec2(px - 1, py - 1), ImVec2(px + 1, py + 1), is_sel((int)i) ? IM_COL32(255, 255, 0, 255) : IM_COL32(20, 20, 20, 200));
    }
    float cx = p0.x + g_cam.target.x / (float)(g_t.w - 1) * mw, cy = p0.y + (1.0f - g_cam.target.y / (float)(g_t.h - 1)) * mh;
    float vr = std::min(std::max(g_cam.dist * 0.5f / (float)g_t.w * mw, 5.0f), mw * 0.5f);
    dl->AddRect(ImVec2(cx - vr, cy - vr), ImVec2(cx + vr, cy + vr), IM_COL32(255, 255, 255, 255));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(0)) {
        ImVec2 m = ImGui::GetMousePos();
        g_cam.target.x = std::min(std::max((m.x - p0.x) / mw, 0.0f), 1.0f) * (float)(g_t.w - 1);
        g_cam.target.y = (1.0f - std::min(std::max((m.y - p0.y) / mh, 0.0f), 1.0f)) * (float)(g_t.h - 1);
        g_cam.target.z = height_at(g_cam.target.x, g_cam.target.y);
    }
}

static void slot_race_team(int n)
{
    Slot &s = g_slot[n - 1];
    int ri = s.race + 1;
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("Race", &ri, k_race_name, 4)) {
        s.race = ri - 1;
        g_dirty = true;
    }
    char cur[16];
    snprintf(cur, sizeof cur, s.team ? "Team %d" : "Auto", s.team);
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("Team", cur)) {
        for (int t = 0; t <= 8; t++) {
            char l[16];
            snprintf(l, sizeof l, t ? "Team %d" : "Auto", t);
            if (ImGui::Selectable(l, s.team == t)) {
                s.team = t;
                g_dirty = true;
            }
        }
        ImGui::EndCombo();
    }
}

static void inspector_panel()
{
    ImGui::Text("Map: %s%s", g_name[0] ? g_name : "(unsaved)", g_dirty ? " *" : "");
    ImGui::TextDisabled("Terrain %dx%d cells", g_t.w, g_t.h);
    ImGui::Separator();
    if (g_tool == T_PLACE)
        ImGui::Text("Placing: %s", g_pal_kind == K_START ? "Player Start" : g_pal_kind == K_ATMOS ? "Atmos Processor" : cls_label(g_pal_cls));
    if (g_sel.size() == 1) {
        EObj &o = g_objs[(size_t)g_sel[0]];
        bool ch = false;
        ImGui::Text("%s", k_kind_name[o.kind]);
        if (o.kind == K_OBJ || o.kind == K_WILD) {
            char buf[48];
            snprintf(buf, sizeof buf, "%s", o.cls);
            ImGui::SetNextItemWidth(-60);
            if (ImGui::InputText("Class", buf, sizeof buf)) {
                snprintf(o.cls, sizeof o.cls, "%s", buf);
                g_dirty = true;
            }
        }
        if (o.kind == K_START) {
            int n = o.num;
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Player", &n) && n >= 1 && n <= 8 && n != o.num) {
                push_undo();
                for (size_t i = 0; i < g_objs.size(); i++)
                    if (g_objs[i].kind == K_START && g_objs[i].num == n)
                        g_objs[i].num = o.num;
                o.num = n;
            }
            slot_race_team(o.num);
        }
        if (o.kind == K_OBJ) {
            int ow = o.owner;
            player_combo("Owner", &ow, 130);
            if (ow != o.owner) {
                push_undo();
                o.owner = ow;
            }
        }
        float p[3] = { o.x, o.y, o.z };
        ImGui::SetNextItemWidth(-60);
        if (ImGui::DragFloat3("Pos", p, 0.1f)) {
            ch = true;
            o.x = std::min(std::max(p[0], 0.0f), (float)(g_t.w - 1));
            o.y = std::min(std::max(p[1], 0.0f), (float)(g_t.h - 1));
            o.z = p[2];
        }
        if (ImGui::Button("Snap to ground")) {
            push_undo();
            o.z = height_at(o.x, o.y);
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus"))
            focus_obj(g_sel[0]);
        float deg = o.rot * 57.2957795f;
        ImGui::SetNextItemWidth(-60);
        if (ImGui::DragFloat("Rot", &deg, 1.0f, -360.0f, 360.0f, "%.0f deg")) {
            o.rot = deg / 57.2957795f;
            ch = true;
        }
        if (o.kind == K_WILD) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Max alive", &o.max)) {
                o.max = std::min(std::max(o.max, 1), 16);
                ch = true;
            }
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("Respawn (s)", &o.delay, 1.0f, 1.0f, 3600.0f, "%.0f")) ch = true;
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("Wander", &o.radius, 0.5f, 1.0f, 100.0f, "%.1f")) ch = true;
        }
        if (ch)
            g_dirty = true;
    } else if (g_sel.size() > 1) {
        ImGui::Text("%d objects selected", (int)g_sel.size());
        int ow = -1;
        player_combo("Owner", &ow, 130);
        if (ow >= 0) {
            push_undo();
            for (size_t i = 0; i < g_sel.size(); i++)
                if (g_objs[(size_t)g_sel[i]].kind == K_OBJ)
                    g_objs[(size_t)g_sel[i]].owner = ow;
        }
        if (ImGui::Button("Snap to ground")) {
            push_undo();
            for (size_t i = 0; i < g_sel.size(); i++) {
                EObj &o = g_objs[(size_t)g_sel[i]];
                o.z = height_at(o.x, o.y);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
            delete_selected();
    } else
        ImGui::TextDisabled("Nothing selected");
}

static void win_map_info()
{
    if (!g_win_info)
        return;
    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((float)g_w * 0.5f - 210, 120), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Info", &g_win_info)) {
        ImGui::End();
        return;
    }
    bool ch = false;
    ImGui::TextDisabled("File");
    ImGui::SameLine();
    ImGui::Text("%s", g_name[0] ? g_name : "(unsaved)");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-90);
    ch |= ImGui::InputText("Name", g_meta.title, sizeof g_meta.title);
    ImGui::SetNextItemWidth(-90);
    ch |= ImGui::InputText("Author", g_meta.author, sizeof g_meta.author);
    ch |= ImGui::InputTextMultiline("Description", g_meta.desc, sizeof g_meta.desc, ImVec2(-90, 80));
    ImGui::SeparatorText("Suggested players");
    int a = g_meta.minp, b = g_meta.maxp;
    ImGui::SetNextItemWidth(110);
    bool c1 = ImGui::InputInt("Min", &a);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    bool c2 = ImGui::InputInt("Max", &b);
    if (c1 || c2) {
        a = std::min(std::max(a, 0), NMAP_MAX_SPAWN);
        b = std::min(std::max(b, 0), NMAP_MAX_SPAWN);
        if (c1 && b < a) b = a;
        if (c2 && a > b) a = b;
        if (a == 0 || b == 0) a = b = 0;
        g_meta.minp = a;
        g_meta.maxp = b;
        ch = true;
    }
    if (g_meta.minp == 0)
        ImGui::TextDisabled("Not set. The game uses the number of Player Starts.");
    if (ImGui::Button("From Player Starts")) {
        int n = count_kind(K_START);
        g_meta.minp = n > 0 ? std::min(2, n) : 0;
        g_meta.maxp = n;
        ch = true;
    }
    ImGui::SeparatorText("Terrain height limits");
    {
        int lo = g_hmin, hi = g_hmax, stp = g_hstep, lvl = g_hlevels;
        bool lc = false;
        ImGui::SetNextItemWidth(110);
        lc |= ImGui::InputInt("Lowest", &lo, 5, 20);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        lc |= ImGui::InputInt("Highest", &hi, 5, 20);
        ImGui::SetNextItemWidth(110);
        bool sc = ImGui::InputInt("Level step", &stp, 5, 10);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        sc |= ImGui::InputInt("Levels", &lvl, 1, 2);
        if (lc) {
            lo = std::min(std::max(lo, -HLIM_ABS), HLIM_ABS - 10);
            hi = std::min(std::max(hi, lo + 10), HLIM_ABS);
            g_hmin = lo;
            g_hmax = hi;
            bool out = false;
            for (size_t i = 0; i < g_t.z.size() && !out; i++)
                out = g_t.z[i] < g_hmin || g_t.z[i] > g_hmax;
            if (out) {
                push_undo();
                for (size_t i = 0; i < g_t.z.size(); i++)
                    g_t.z[i] = (int16_t)std::min(std::max((int)g_t.z[i], g_hmin), g_hmax);
                terr_flags_update(0, 0, g_t.w - 1, g_t.h - 1);
                g_terr_snap.reset();
                mesh_rebuild_all();
            }
            ch = true;
        }
        if (sc) {
            g_hstep = std::min(std::max(stp, 5), 200);
            g_hlevels = std::min(std::max(lvl, 1), 16);
            ch = true;
        }
        ImGui::TextDisabled("Raw units, 10 per world unit. Game terrain runs about -59 to 204.");
        ImGui::TextDisabled("Sculpting, noise, plateau and cliff tools stay inside the range.");
    }
    ImGui::SeparatorText("Statistics");
    ImGui::Text("Terrain %d x %d cells", g_t.w, g_t.h);
    ImGui::Text("Placed objects %d, Player Starts %d, Wildlife %d", count_kind(K_OBJ) + count_kind(K_ATMOS), count_kind(K_START), count_kind(K_WILD));
    if (ch)
        g_dirty = true;
    ImGui::End();
}

static void win_players()
{
    if (!g_win_players)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((float)g_w * 0.5f - 260, 130), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Players and Forces", &g_win_players)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("##pl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player", 0, 1.6f);
        ImGui::TableSetupColumn("Start", 0, 2.1f);
        ImGui::TableSetupColumn("Race", 0, 1.3f);
        ImGui::TableSetupColumn("Team", 0, 1.1f);
        ImGui::TableSetupColumn("Position", 0, 1.0f);
        ImGui::TableHeadersRow();
        for (int n = 1; n <= NMAP_MAX_SPAWN; n++) {
            Slot &s = g_slot[n - 1];
            int si = start_index(n);
            ImGui::PushID(n);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::ColorButton("##c", player_vec(n), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(14, 14));
            ImGui::SameLine();
            ImGui::Text("Player %d", n);
            ImGui::TableNextColumn();
            if (si >= 0) {
                if (ImGui::SmallButton("Select")) {
                    select_only(si);
                    focus_obj(si);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    g_sel.assign(1, si);
                    delete_selected();
                }
            } else {
                ImGui::TextDisabled("none");
                ImGui::SameLine();
                if (ImGui::SmallButton("Add")) {
                    g_tool = T_PLACE;
                    g_pal_kind = K_START;
                    float x = g_cam.target.x, y = g_cam.target.y;
                    int keep = next_start_num();
                    (void)keep;
                    place_at(x, y);
                    int ni = (int)g_objs.size() - 1;
                    if (ni >= 0 && g_objs[(size_t)ni].kind == K_START) {
                        int prev = g_objs[(size_t)ni].num;
                        if (prev != n) {
                            int other = start_index(n);
                            if (other >= 0) g_objs[(size_t)other].num = prev;
                            g_objs[(size_t)ni].num = n;
                        }
                    }
                    g_tool = T_SELECT;
                }
            }
            ImGui::TableNextColumn();
            int ri = s.race + 1;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##r", &ri, k_race_name, 4)) {
                s.race = ri - 1;
                g_dirty = true;
            }
            ImGui::TableNextColumn();
            char cur[16];
            snprintf(cur, sizeof cur, s.team ? "Team %d" : "Auto", s.team);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##t", cur)) {
                for (int t = 0; t <= 8; t++) {
                    char l[16];
                    snprintf(l, sizeof l, t ? "Team %d" : "Auto", t);
                    if (ImGui::Selectable(l, s.team == t)) {
                        s.team = t;
                        g_dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (si >= 0)
                ImGui::Text("%.0f, %.0f", g_objs[(size_t)si].x, g_objs[(size_t)si].y);
            else
                ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Forces");
    bool any = false;
    for (int t = 1; t <= 8; t++) {
        char list[96] = "";
        int cnt = 0;
        for (int n = 1; n <= 8; n++)
            if (g_slot[n - 1].team == t) {
                char one[8];
                snprintf(one, sizeof one, cnt ? ", %d" : "%d", n);
                strncat(list, one, sizeof list - strlen(list) - 1);
                cnt++;
            }
        if (cnt) {
            ImGui::Text("Team %d: players %s", t, list);
            any = true;
        }
    }
    if (!any)
        ImGui::TextDisabled("All players are on automatic teams.");
    ImGui::TextDisabled("Race and team are stored in the map. Auto lets the game decide.");
    ImGui::End();
}

static void win_prefs()
{
    if (!g_win_prefs)
        return;
    ImGui::SetNextWindowSize(ImVec2(360, 250), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((float)g_w * 0.5f - 180, 140), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &g_win_prefs)) {
        ImGui::End();
        return;
    }
    ImGui::SliderFloat("Camera speed", &g_cam_speed, 0.2f, 4.0f, "%.2fx");
    ImGui::Checkbox("Scroll when the mouse is at the screen edge", &g_edge_scroll);
    if (ImGui::SliderFloat("UI scale", &g_ui_scale, 0.8f, 2.0f, "%.2f"))
        ImGui::GetIO().FontGlobalScale = g_ui_scale;
    ImGui::Checkbox("Snap to grid", &g_snap);
    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("Grid size", &g_grid, 0.1f, 0.25f, 16.0f, "%.2f");
    ImGui::Checkbox("Object labels", &g_labels);
    if (ImGui::Checkbox("Dark theme", &g_dark))
        ui_theme(g_dark);
    if (ImGui::Button("Save preferences"))
        cfg_save();
    ImGui::End();
}

static int sort_col, sort_dir = 1;

static void win_objects()
{
    if (!g_win_objects)
        return;
    pal_build();
    ImGui::SetNextWindowSize(ImVec2(800, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(g_left_w + 30, 110), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Object Editor", &g_win_objects)) {
        ImGui::End();
        return;
    }
    static char filter[48];
    static int catf = -1;
    if (ImGui::BeginTabBar("##oe")) {
        if (ImGui::BeginTabItem("Placed")) {
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##f", "Search placed objects...", filter, sizeof filter);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##cf", catf < 0 ? "All categories" : k_cat_name[catf])) {
                if (ImGui::Selectable("All categories", catf < 0)) catf = -1;
                for (int c = 0; c < 12; c++)
                    if (ImGui::Selectable(k_cat_name[c], catf == c)) catf = c;
                ImGui::EndCombo();
            }
            std::vector<int> idx;
            for (size_t i = 0; i < g_objs.size(); i++) {
                const EObj &o = g_objs[i];
                int c = category(o);
                if (catf >= 0 && c != catf)
                    continue;
                char nm[64];
                if (filter[0] && !icontains(short_name(o, nm, sizeof nm), filter) && !icontains(o.cls, filter))
                    continue;
                idx.push_back((int)i);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d of %d", (int)idx.size(), (int)g_objs.size());
            ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##placed", 8, tf, ImVec2(0, -1))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 0.5f);
                ImGui::TableSetupColumn("Class", 0, 2.4f);
                ImGui::TableSetupColumn("Category", 0, 1.1f);
                ImGui::TableSetupColumn("Player", 0, 1.1f);
                ImGui::TableSetupColumn("X", 0, 1.0f);
                ImGui::TableSetupColumn("Y", 0, 1.0f);
                ImGui::TableSetupColumn("Z", 0, 0.9f);
                ImGui::TableSetupColumn("Rot", 0, 0.9f);
                ImGui::TableHeadersRow();
                if (ImGuiTableSortSpecs *sp = ImGui::TableGetSortSpecs()) {
                    if (sp->SpecsCount > 0) {
                        sort_col = sp->Specs[0].ColumnIndex;
                        sort_dir = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending ? 1 : -1;
                    }
                }
                std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                    const EObj &x = g_objs[(size_t)a], &y = g_objs[(size_t)b];
                    int r = 0;
                    switch (sort_col) {
                    case 1: r = strcmp(x.cls, y.cls); break;
                    case 2: r = category(x) - category(y); break;
                    case 3: r = x.owner - y.owner; break;
                    case 4: r = x.x < y.x ? -1 : x.x > y.x; break;
                    case 5: r = x.y < y.y ? -1 : x.y > y.y; break;
                    case 6: r = x.z < y.z ? -1 : x.z > y.z; break;
                    case 7: r = x.rot < y.rot ? -1 : x.rot > y.rot; break;
                    default: r = a - b; break;
                    }
                    return sort_dir > 0 ? r < 0 : r > 0;
                });
                ImGuiListClipper clip;
                clip.Begin((int)idx.size());
                while (clip.Step())
                    for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                        int i = idx[(size_t)row];
                        EObj &o = g_objs[(size_t)i];
                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        char num[16];
                        snprintf(num, sizeof num, "%d", i + 1);
                        if (ImGui::Selectable(num, is_sel(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
                            if (ImGui::IsMouseDoubleClicked(0))
                                focus_obj(i);
                            g_sel.assign(1, i);
                        }
                        ImGui::TableNextColumn();
                        if (o.kind == K_OBJ || o.kind == K_WILD) {
                            char buf[48];
                            snprintf(buf, sizeof buf, "%s", o.cls);
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::InputText("##cls", buf, sizeof buf)) {
                                snprintf(o.cls, sizeof o.cls, "%s", buf);
                                g_dirty = true;
                            }
                            if (ImGui::IsItemActivated()) push_undo();
                        } else
                            ImGui::TextUnformatted(o.kind == K_START ? "Player Start" : "Atmos Processor");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(k_cat_name[category(o)]);
                        ImGui::TableNextColumn();
                        if (o.kind == K_OBJ) {
                            int ow = o.owner;
                            player_combo("##ow", &ow, -1);
                            if (ow != o.owner) {
                                push_undo();
                                o.owner = ow;
                            }
                        } else if (o.kind == K_START)
                            ImGui::TextColored(player_vec(o.num), "Player %d", o.num);
                        else
                            ImGui::TextDisabled("-");
                        float *f[3] = { &o.x, &o.y, &o.z };
                        for (int k = 0; k < 3; k++) {
                            ImGui::TableNextColumn();
                            ImGui::PushID(k);
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::DragFloat("##v", f[k], 0.1f, 0, 0, "%.2f")) {
                                o.x = std::min(std::max(o.x, 0.0f), (float)(g_t.w - 1));
                                o.y = std::min(std::max(o.y, 0.0f), (float)(g_t.h - 1));
                                g_dirty = true;
                            }
                            if (ImGui::IsItemActivated()) push_undo();
                            ImGui::PopID();
                        }
                        ImGui::TableNextColumn();
                        float deg = o.rot * 57.2957795f;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat("##rot", &deg, 1.0f, -360.0f, 360.0f, "%.0f")) {
                            o.rot = deg / 57.2957795f;
                            g_dirty = true;
                        }
                        if (ImGui::IsItemActivated()) push_undo();
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Types")) {
            static char tf2[48];
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##tf", "Search types...", tf2, sizeof tf2);
            if (ImGui::BeginTable("##types", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -1))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", 0, 2.0f);
                ImGui::TableSetupColumn("Class", 0, 2.2f);
                ImGui::TableSetupColumn("Category", 0, 1.2f);
                ImGui::TableSetupColumn("Placed", 0, 0.7f);
                ImGui::TableSetupColumn("", 0, 0.8f);
                ImGui::TableHeadersRow();
                for (size_t k = 0; k < g_pal_items.size(); k++) {
                    const PalItem &it = g_pal_items[k];
                    if (!icontains(item_label(it), tf2) && !icontains(it.cls, tf2) && !icontains(k_cat_name[it.cat], tf2))
                        continue;
                    int placed = 0;
                    for (size_t i = 0; i < g_objs.size(); i++)
                        if (g_objs[i].kind == it.kind && (it.kind == K_START || it.kind == K_ATMOS || !strcmp(g_objs[i].cls, it.cls)))
                            placed++;
                    ImGui::PushID((int)k);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(item_label(it));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it.cls);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(k_cat_name[it.cat]);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", placed);
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Place")) {
                        g_tool = T_PLACE;
                        g_pal_kind = it.kind;
                        if (it.kind == K_OBJ || it.kind == K_WILD)
                            snprintf(g_pal_cls, sizeof g_pal_cls, "%s", it.cls);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void dialogs()
{
    ImVec2 cen((float)g_w * 0.5f, (float)g_h * 0.5f);
    if (g_dlg_new && !ImGui::IsPopupOpen("New map")) ImGui::OpenPopup("New map");
    if (g_dlg_open && !ImGui::IsPopupOpen("Open map")) ImGui::OpenPopup("Open map");
    if (g_dlg_saveas && !ImGui::IsPopupOpen("Save map as")) ImGui::OpenPopup("Save map as");
    if (g_dlg_about && !ImGui::IsPopupOpen("Keyboard shortcuts")) ImGui::OpenPopup("Keyboard shortcuts");

    ImGui::SetNextWindowPos(cen, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New map", &g_dlg_new, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", g_dlg_name, sizeof g_dlg_name);
        ImGui::SeparatorText("Map size (cells)");
        static const int presets[] = { 64, 96, 128, 160, 192, 256 };
        static const char *const pname[] = { "Tiny", "Small", "Medium", "Large", "Huge", "Max" };
        for (int i = 0; i < 6; i++) {
            char l[32];
            snprintf(l, sizeof l, "%s %d", pname[i], presets[i]);
            if (i) ImGui::SameLine();
            if (seg_button(l, g_new_w == presets[i] && g_new_h == presets[i])) {
                g_new_w = g_new_h = presets[i];
            }
            if (i == 2) ImGui::NewLine();
        }
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Width", &g_new_w, 8, 32);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Height", &g_new_h, 8, 32);
        g_new_w = std::min(std::max(g_new_w, 64), 256);
        g_new_h = std::min(std::max(g_new_h, 64), 256);
        ImGui::TextDisabled("From 64 to 256 cells per side.");
        ImGui::Combo("Fill texture", &g_dlg_layer, k_layer_name, NMAP_LAYERS);
        if (ImGui::Button("Create")) {
            new_map_confirmed();
            if (!g_dlg_new) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_dlg_new = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(cen, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open map", &g_dlg_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::BeginChild("##list", ImVec2(300, 220), true);
        for (int i = 0; i < g_nnames; i++)
            if (ImGui::Selectable(g_names[i], g_dlg_pick == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                g_dlg_pick = i;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    do_open(g_names[i]);
                    g_dlg_open = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        if (!g_nnames) ImGui::TextDisabled("No maps in the maps folder");
        ImGui::EndChild();
        if (ImGui::Button("Open") && g_dlg_pick >= 0) {
            do_open(g_names[g_dlg_pick]);
            g_dlg_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_dlg_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(cen, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save map as", &g_dlg_saveas, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", g_dlg_name, sizeof g_dlg_name);
        if (ImGui::Button("Save") && do_save(g_dlg_name)) {
            g_dlg_saveas = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_dlg_saveas = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(cen, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Keyboard shortcuts", &g_dlg_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char *const rows[][2] = {
            { "WASD / arrows / screen edge", "Pan the camera" },
            { "Mouse wheel", "Zoom" },
            { "Middle drag", "Orbit" },
            { "Right drag, Shift+middle drag", "Pan" },
            { "F", "Frame the map" },
            { "1 / 2 / 3 / 4", "Select, Height, Texture, Place tools" },
            { "[ and ]  or  Ctrl+wheel", "Brush size" },
            { "Ctrl (while brushing)", "Invert the brush" },
            { "Click, Shift+click, drag box", "Select objects" },
            { "R / Shift+R", "Rotate selection" },
            { "Del", "Delete selection" },
            { "Ctrl+C / Ctrl+V", "Copy and paste" },
            { "Ctrl+A / Ctrl+D", "Select all, deselect" },
            { "Ctrl+Z / Ctrl+Y", "Undo, redo" },
            { "Ctrl+N / O / S", "New, open, save (Shift for Save As)" },
            { "G / H / L", "Snap, grid, labels" },
            { "F2 / F3 / F6", "Map Info, Players and Forces, Object Editor" },
            { "F7 / F8", "Pathing overlay, height overlay" },
            { "F5", "Test in game" },
            { "Esc", "Deselect and return to Select" }
        };
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rows[i][0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rows[i][1]);
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Close")) {
            g_dlg_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

#include "ui_classic.h"

#endif
