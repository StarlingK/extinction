#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <string>
#ifdef _WIN32
#include <direct.h>
#define CHDIR _chdir
#else
#include <unistd.h>
#define CHDIR chdir
#endif

#include "imgui.h"
#include "glmin.h"
#include "glrender.h"
#include "plat.h"
#include "ui_gl.h"
#include "vecmath.h"
#include "catalog.h"
#include "gamedata.h"
#include "audio.h"

extern "C" {
#include "avp_nmap.h"
}

enum Tool { T_SELECT, T_SCULPT, T_PAINT, T_PLACE, T_CAVE, T_CLIFF };
enum CaveMode { CV_PAINT, CV_ERASE };
enum CliffMode { CL_RAISE, CL_LOWER, CL_RAMP };
enum Brush { B_RAISE, B_LOWER, B_SMOOTH, B_FLATTEN, B_PLATEAU, B_NOISE };
enum Shape { SH_CIRCLE, SH_SQUARE };

struct EObj {
    int kind;
    char cls[48];
    int owner;
    int num;
    float x, y, z, rot;
    int max;
    float delay, radius;
};

struct Terr {
    int w, h;
    std::vector<int16_t> z;
    std::vector<uint8_t> sp;
    std::vector<uint8_t> fl;
};

struct Snapshot {
    std::vector<EObj> objs;
    std::shared_ptr<const Terr> terr;
};

static const float k_layer_rgb[NMAP_LAYERS][3] = {
    { 0.46f, 0.34f, 0.22f }, { 0.30f, 0.46f, 0.20f }, { 0.52f, 0.51f, 0.50f }, { 0.83f, 0.73f, 0.48f }
};
static const char *const k_layer_name[NMAP_LAYERS] = { "Dirt", "Grass", "Rock", "Sand" };
static const float k_player_rgb[9][3] = {
    { 0.63f, 0.66f, 0.69f }, { 0.24f, 0.55f, 1.0f }, { 0.25f, 0.88f, 0.88f }, { 1.0f, 0.55f, 0.12f },
    { 1.0f, 0.28f, 0.28f }, { 0.88f, 0.88f, 0.25f }, { 0.75f, 0.38f, 1.0f }, { 0.38f, 1.0f, 0.38f }, { 1.0f, 1.0f, 1.0f }
};
static const int HLIM_ABS = 2000;
static const int CAVE_LAYER = 2;
static int g_hmin = NMAP_HMIN_DEF, g_hmax = NMAP_HMAX_DEF, g_hstep = NMAP_HSTEP_DEF, g_hlevels = NMAP_HLEVELS_DEF;
static int g_cave_mode = CV_PAINT, g_cliff_mode = CL_RAISE;
static float g_cave_depth = 30.0f, g_cave_wall_h = 60.0f, g_cave_wall_w = 2.0f;
static bool g_cave_doodads = true;

static Terr g_t;
static std::vector<EObj> g_objs;
static std::vector<int> g_sel;
static std::vector<EObj> g_clip;
static std::vector<Snapshot> g_undo, g_redo;
static std::shared_ptr<const Terr> g_terr_snap;
static char g_base[4] = "M01";
static char g_name[NMAP_NAME] = "";
static bool g_dirty;
static char g_msg[160];
static double g_msg_t;

static Tool g_tool = T_SELECT;
static Brush g_brush = B_RAISE;
static float g_brush_r = 10.0f, g_brush_s = 12.0f;
static int g_layer = 1;
static bool g_snap = true;
static float g_grid = 1.0f;
static bool g_labels = true, g_show_grid = true;
static int g_pal_kind = K_OBJ;
static char g_pal_cls[48] = "GMarineInfantry";
static int g_pal_owner;
static bool g_terr_changed;
static std::vector<std::string> g_wall_pool;
static std::string g_wall_pool_level;

struct Meta {
    char title[NMAP_TITLE];
    char author[NMAP_AUTHOR];
    char desc[NMAP_DESC];
    int minp, maxp;
};
struct Slot {
    int race;
    int team;
};
static Meta g_meta;
static Slot g_slot[NMAP_MAX_SPAWN];
static int g_shape = SH_CIRCLE;
static bool g_fixed_h;
static float g_fixed_hv;
static float g_cam_speed = 1.0f;
static bool g_edge_scroll = true;
static int g_overlay;
static float g_slope_deg = 35.0f;
static float g_hmin_w, g_hmax_w = 1.0f;
static float g_ui_scale = 1.0f;
static float g_vx0, g_vy0, g_vx1, g_vy1;
static void recent_add(const char *name);
static void cfg_save();

struct Cam {
    V3 target;
    float yaw, pitch, dist;
};
static Cam g_cam = { { 115, 115, 0 }, 0.8f, 0.9f, 140.0f };
static int g_w = 1280, g_h = 800;
static M4 g_vp, g_view, g_proj;
static V3 g_eye;

static GLuint g_terr_prog, g_solid_prog, g_terr_vao, g_terr_vbo, g_terr_ebo, g_terr_n;
static GLuint g_mesh_vao[3], g_mesh_vbo[3];
static int g_mesh_n[3];
static GLuint g_line_vao, g_line_vbo;
static GLuint g_mm_tex;
static int g_t_rows_dirty0, g_t_rows_dirty1;
static bool g_mm_dirty = true;

static void say(const char *fmt, const char *a = "")
{
    snprintf(g_msg, sizeof g_msg, fmt, a);
    g_msg_t = plat_time();
}

static int16_t &H(int x, int y) { return g_t.z[(size_t)y * (size_t)g_t.w + (size_t)x]; }
static uint8_t &FL(int x, int y) { return g_t.fl[(size_t)y * (size_t)g_t.w + (size_t)x]; }

static float height_at(float x, float y)
{
    if (g_t.w < 2)
        return 0;
    float fx = std::min(std::max(x, 0.0f), (float)(g_t.w - 1) - 0.001f);
    float fy = std::min(std::max(y, 0.0f), (float)(g_t.h - 1) - 0.001f);
    int ix = (int)fx, iy = (int)fy;
    float tx = fx - (float)ix, ty = fy - (float)iy;
    float a = H(ix, iy), b = H(ix + 1, iy), c = H(ix, iy + 1), d = H(ix + 1, iy + 1);
    return ((a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty) * NMAP_HZ;
}

static void terr_new(int w, int h, int layer)
{
    g_t.w = w;
    g_t.h = h;
    g_t.z.assign((size_t)w * (size_t)h, 0);
    g_t.sp.assign((size_t)w * (size_t)h * NMAP_LAYERS, 0);
    g_t.fl.assign((size_t)w * (size_t)h, 0);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        g_t.sp[i * NMAP_LAYERS + (size_t)layer] = 255;
}

static const BaseLevel *find_base(const char *code)
{
    for (int i = 0; i < N_BASES; i++)
        if (!strcmp(k_bases[i].code, code))
            return &k_bases[i];
    return NULL;
}

static int category(const EObj &o)
{
    if (o.kind == K_START) return C_START;
    if (o.kind == K_ATMOS) return C_ATMOS;
    if (o.kind == K_WILD) return C_WILD;
    for (int i = 0; i < COUNT(k_alien); i++) if (!strcmp(o.cls, k_alien[i])) return C_ALIEN;
    for (int i = 0; i < COUNT(k_marine); i++) if (!strcmp(o.cls, k_marine[i])) return C_MARINE;
    for (int i = 0; i < COUNT(k_pred); i++) if (!strcmp(o.cls, k_pred[i])) return C_PRED;
    for (int i = 0; i < COUNT(k_struct); i++) if (!strcmp(o.cls, k_struct[i])) return C_STRUCT;
    for (int i = 0; i < COUNT(k_wild); i++) if (!strcmp(o.cls, k_wild[i])) return C_WILD;
    if (o.cls[0] == '@') {
        if (strstr(o.cls, "Tree")) return C_TREE;
        if (strstr(o.cls, "Rock") || strstr(o.cls, "Bones") || strstr(o.cls, "Ribs") || strstr(o.cls, "Debris")) return C_ROCK;
        if (strstr(o.cls, "Cactus") || strstr(o.cls, "bush") || strstr(o.cls, "Bush")) return C_PLANT;
        if (strstr(o.cls, "rate")) return C_CRATE;
        return C_DOODAD;
    }
    if (!strncmp(o.cls, "GAlien", 6) || !strncmp(o.cls, "GQueen", 6)) return C_ALIEN;
    if (!strncmp(o.cls, "GMarine", 7)) return C_MARINE;
    if (!strncmp(o.cls, "GPred", 5)) return C_PRED;
    return C_STRUCT;
}

static const char *short_name(const EObj &o, char *buf, size_t n)
{
    if (o.kind == K_START) { snprintf(buf, n, "%d", o.num); return buf; }
    if (o.kind == K_ATMOS) return "Atmos";
    const char *s = o.cls;
    if (s[0] == '@' || (s[0] == 'G' && isupper((unsigned char)s[1])))
        s++;
    snprintf(buf, n, "%s", s);
    return buf;
}

static void cat_look(int c, float rgb[3], int *mesh, V3 *sc)
{
    switch (c) {
    case C_ALIEN: rgb[0] = 0.58f; rgb[1] = 0.22f; rgb[2] = 0.75f; *mesh = 1; *sc = v3(1.0f, 1.0f, 1.8f); break;
    case C_MARINE: rgb[0] = 0.25f; rgb[1] = 0.52f; rgb[2] = 1.0f; *mesh = 1; *sc = v3(1.0f, 1.0f, 1.8f); break;
    case C_PRED: rgb[0] = 0.25f; rgb[1] = 0.85f; rgb[2] = 0.35f; *mesh = 1; *sc = v3(1.1f, 1.1f, 2.2f); break;
    case C_STRUCT: rgb[0] = 0.62f; rgb[1] = 0.58f; rgb[2] = 0.50f; *mesh = 0; *sc = v3(3.2f, 3.2f, 2.6f); break;
    case C_TREE: rgb[0] = 0.16f; rgb[1] = 0.52f; rgb[2] = 0.20f; *mesh = 2; *sc = v3(2.6f, 2.6f, 5.5f); break;
    case C_ROCK: rgb[0] = 0.48f; rgb[1] = 0.47f; rgb[2] = 0.46f; *mesh = 0; *sc = v3(2.0f, 1.6f, 1.2f); break;
    case C_PLANT: rgb[0] = 0.55f; rgb[1] = 0.70f; rgb[2] = 0.30f; *mesh = 1; *sc = v3(1.0f, 1.0f, 1.6f); break;
    case C_CRATE: rgb[0] = 0.60f; rgb[1] = 0.42f; rgb[2] = 0.22f; *mesh = 0; *sc = v3(1.4f, 1.4f, 1.4f); break;
    case C_ATMOS: rgb[0] = 1.0f; rgb[1] = 0.62f; rgb[2] = 0.15f; *mesh = 1; *sc = v3(3.4f, 3.4f, 5.5f); break;
    case C_WILD: rgb[0] = 0.95f; rgb[1] = 0.82f; rgb[2] = 0.30f; *mesh = 2; *sc = v3(1.6f, 1.6f, 1.3f); break;
    default: rgb[0] = 0.7f; rgb[1] = 0.7f; rgb[2] = 0.7f; *mesh = 0; *sc = v3(1.5f, 1.5f, 1.5f); break;
    }
}

static float pick_radius(const EObj &o)
{
    float rgb[3];
    int m;
    V3 sc;
    cat_look(category(o), rgb, &m, &sc);
    if (o.kind == K_START)
        return 2.2f;
    float mr;
    if (o.kind != K_ATMOS && gd_bounds(o.cls, &mr, NULL))
        return std::max(mr, 0.5f) + 0.3f;
    return std::max(sc.x, sc.y) * 0.7f + 0.5f;
}

static std::shared_ptr<const Terr> terr_snapshot()
{
    if (!g_terr_snap)
        g_terr_snap = std::make_shared<Terr>(g_t);
    return g_terr_snap;
}

static void push_undo()
{
    Snapshot s;
    s.objs = g_objs;
    s.terr = terr_snapshot();
    g_undo.push_back(s);
    if (g_undo.size() > 60)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_dirty = true;
}

static void mesh_rebuild_all();
static void restore(const Snapshot &s)
{
    g_objs = s.objs;
    if (s.terr != g_terr_snap) {
        g_t = *s.terr;
        g_terr_snap = s.terr;
        mesh_rebuild_all();
        g_mm_dirty = true;
    }
    g_sel.clear();
    g_dirty = true;
}

static void do_undo()
{
    if (g_undo.empty())
        return;
    Snapshot cur;
    cur.objs = g_objs;
    cur.terr = terr_snapshot();
    g_redo.push_back(cur);
    Snapshot s = g_undo.back();
    g_undo.pop_back();
    restore(s);
}

static void do_redo()
{
    if (g_redo.empty())
        return;
    Snapshot cur;
    cur.objs = g_objs;
    cur.terr = terr_snapshot();
    g_undo.push_back(cur);
    Snapshot s = g_redo.back();
    g_redo.pop_back();
    restore(s);
}

static bool is_sel(int i)
{
    return std::find(g_sel.begin(), g_sel.end(), i) != g_sel.end();
}

static int next_start_num()
{
    for (int n = 1; n <= NMAP_MAX_SPAWN; n++) {
        bool used = false;
        for (size_t i = 0; i < g_objs.size(); i++)
            if (g_objs[i].kind == K_START && g_objs[i].num == n)
                used = true;
        if (!used)
            return n;
    }
    return 0;
}

static void snap_pos(float *x, float *y)
{
    if (g_snap && g_grid > 0.01f) {
        *x = floorf(*x / g_grid + 0.5f) * g_grid;
        *y = floorf(*y / g_grid + 0.5f) * g_grid;
    }
    *x = std::min(std::max(*x, 0.0f), (float)(g_t.w - 1));
    *y = std::min(std::max(*y, 0.0f), (float)(g_t.h - 1));
}

struct TerrVert {
    float p[3];
    float n[3];
    uint8_t c[4];
    uint8_t w[4];
    uint8_t f[4];
};
static std::vector<TerrVert> g_tv;

static void terr_vertex(int x, int y)
{
    TerrVert &v = g_tv[(size_t)y * (size_t)g_t.w + (size_t)x];
    int xl = std::max(x - 1, 0), xr = std::min(x + 1, g_t.w - 1), yl = std::max(y - 1, 0), yr = std::min(y + 1, g_t.h - 1);
    float dzx = (float)(H(xr, y) - H(xl, y)) * NMAP_HZ / (float)(xr - xl);
    float dzy = (float)(H(x, yr) - H(x, yl)) * NMAP_HZ / (float)(yr - yl);
    V3 n = norm(v3(-dzx, -dzy, 1.0f));
    v.p[0] = (float)x;
    v.p[1] = (float)y;
    v.p[2] = (float)H(x, y) * NMAP_HZ;
    v.n[0] = n.x; v.n[1] = n.y; v.n[2] = n.z;
    const uint8_t *sp = &g_t.sp[((size_t)y * (size_t)g_t.w + (size_t)x) * NMAP_LAYERS];
    float tot = 0, rgb[3] = { 0, 0, 0 };
    for (int k = 0; k < NMAP_LAYERS; k++) {
        tot += sp[k];
        for (int c = 0; c < 3; c++)
            rgb[c] += k_layer_rgb[k][c] * sp[k];
    }
    if (tot < 1) {
        tot = 1;
        rgb[0] = rgb[1] = rgb[2] = 0.5f;
    }
    for (int c = 0; c < 3; c++)
        v.c[c] = (uint8_t)std::min(255.0f, rgb[c] / tot * 255.0f);
    v.c[3] = 255;
    uint8_t fb = g_t.fl[(size_t)y * (size_t)g_t.w + (size_t)x];
    v.f[0] = (fb & NMAP_CF_CAVE) ? 255 : 0;
    v.f[1] = (fb & NMAP_CF_IMPASSABLE) ? 255 : 0;
    v.f[2] = (fb & NMAP_CF_RAMP) ? 255 : 0;
    v.f[3] = 0;
    for (int k = 0; k < 4; k++)
        v.w[k] = (uint8_t)std::min(255.0f, sp[k] * 255.0f / tot + 0.5f);
}

static void mesh_upload_rows(int y0, int y1)
{
    y0 = std::max(y0, 0);
    y1 = std::min(y1, g_t.h - 1);
    if (y1 < y0)
        return;
    for (int y = std::max(y0 - 1, 0); y <= std::min(y1 + 1, g_t.h - 1); y++)
        for (int x = 0; x < g_t.w; x++)
            terr_vertex(x, y);
    int a = std::max(y0 - 1, 0), b = std::min(y1 + 1, g_t.h - 1);
    glBindBuffer(GL_ARRAY_BUFFER, g_terr_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)a * (size_t)g_t.w * sizeof(TerrVert)),
        (GLsizeiptr)((size_t)(b - a + 1) * (size_t)g_t.w * sizeof(TerrVert)), &g_tv[(size_t)a * (size_t)g_t.w]);
}

static void mesh_rebuild_all()
{
    g_tv.assign((size_t)g_t.w * (size_t)g_t.h, TerrVert());
    for (int y = 0; y < g_t.h; y++)
        for (int x = 0; x < g_t.w; x++)
            terr_vertex(x, y);
    std::vector<uint32_t> idx;
    idx.reserve((size_t)(g_t.w - 1) * (size_t)(g_t.h - 1) * 6);
    for (int y = 0; y < g_t.h - 1; y++)
        for (int x = 0; x < g_t.w - 1; x++) {
            uint32_t a = (uint32_t)(y * g_t.w + x), b = a + 1, c = a + (uint32_t)g_t.w, d = c + 1;
            idx.push_back(a); idx.push_back(b); idx.push_back(d);
            idx.push_back(a); idx.push_back(d); idx.push_back(c);
        }
    g_terr_n = (GLuint)idx.size();
    glBindVertexArray(g_terr_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_terr_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_tv.size() * sizeof(TerrVert)), &g_tv[0], GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_terr_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)), &idx[0], GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TerrVert), (void *)offsetof(TerrVert, f));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TerrVert), (void *)offsetof(TerrVert, w));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrVert), (void *)offsetof(TerrVert, p));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrVert), (void *)offsetof(TerrVert, n));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TerrVert), (void *)offsetof(TerrVert, c));
    glBindVertexArray(0);
    g_mm_dirty = true;
}

static const char *k_terr_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 p;\n"
    "layout(location=1) in vec3 n;\n"
    "layout(location=2) in vec4 c;\n"
    "layout(location=3) in vec4 wt;\n"
    "layout(location=4) in vec4 fl;\n"
    "uniform mat4 vp;\n"
    "out vec3 vn; out vec4 vc; out vec3 wp; out vec4 vw; out vec4 vf;\n"
    "void main(){ vn=n; vc=c; vw=wt; vf=fl; wp=p; gl_Position=vp*vec4(p,1.0); }\n";

static const char *k_terr_fs =
    "#version 330 core\n"
    "in vec3 vn; in vec4 vc; in vec3 wp; in vec4 vw; in vec4 vf;\n"
    "uniform float usetex; uniform sampler2D t0; uniform sampler2D t1; uniform sampler2D t2; uniform sampler2D t3;\n"
    "uniform vec4 brush; uniform float grid; uniform vec3 lightdir;\n"
    "uniform float overlay; uniform vec4 hrange; uniform float slopecos;\n"
    "out vec4 o;\n"
    "float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }\n"
    "float vn2(vec2 p){ vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);\n"
    " return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y); }\n"
    "void main(){\n"
    " vec3 n=normalize(vn);\n"
    " float d=0.35+0.65*max(dot(n,lightdir),0.0);\n"
    " float t=0.82+0.30*vn2(wp.xy*0.9)+0.10*vn2(wp.xy*3.7);\n"
    " vec3 col=vc.rgb*d*t;\n"
    " if(usetex>0.5){ vec2 uv=wp.xy/6.0; vec4 w=vw/max(vw.x+vw.y+vw.z+vw.w,0.001); vec3 tc=w.x*texture(t0,uv).rgb+w.y*texture(t1,uv).rgb+w.z*texture(t2,uv).rgb+w.w*texture(t3,uv).rgb; col=tc*vc.rgb*d*(0.9+0.2*vn2(wp.xy*0.9))*2.4; }\n"
    " col*=mix(1.0,0.5,vf.x)*mix(vec3(1.0),vec3(0.85,0.9,1.15),vf.x);\n"
    " if(grid>0.0){ vec2 g=abs(fract(wp.xy/grid-0.5)-0.5)/fwidth(wp.xy/grid); float l=1.0-min(min(g.x,g.y),1.0); col=mix(col,vec3(1.0),l*0.22);\n"
    "  vec2 g2=abs(fract(wp.xy/(grid*8.0)-0.5)-0.5)/fwidth(wp.xy/(grid*8.0)); float l2=1.0-min(min(g2.x,g2.y),1.0); col=mix(col,vec3(1.0,0.88,0.1),l2*0.75); }\n"
    " if(overlay>0.5&&overlay<1.5){ float walk=step(slopecos,n.z); walk=max(walk,step(0.5,vf.z)); walk*=1.0-step(0.5,vf.y); vec3 oc=mix(vec3(0.9,0.15,0.1),vec3(0.15,0.8,0.25),walk); col=mix(col*0.55,oc,0.55); }\n"
    " if(overlay>1.5){ float t=clamp((wp.z-hrange.x)/max(hrange.y-hrange.x,0.001),0.0,1.0); vec3 oc=vec3(clamp(2.0*t-0.5,0.0,1.0),1.0-abs(2.0*t-1.0),clamp(1.0-2.0*t,0.0,1.0)); col=mix(col*0.6,oc,0.6); }\n"
    " if(brush.w>0.0){ vec2 dd=abs(wp.xy-brush.xy); float r=brush.z>0.5?max(dd.x,dd.y):length(dd); float ring=1.0-smoothstep(0.0,0.6,abs(r-brush.w)); col=mix(col,vec3(1.0,0.9,0.2),ring*0.9); }\n"
    " o=vec4(col,1.0);\n"
    "}\n";

static const char *k_solid_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 p;\n"
    "layout(location=1) in vec3 n;\n"
    "uniform mat4 vp; uniform mat4 model; uniform mat4 nrot; uniform vec3 scl;\n"
    "out vec3 vn;\n"
    "void main(){ vn=normalize((nrot*vec4(n/scl,0.0)).xyz); gl_Position=vp*model*vec4(p,1.0); }\n";

static const char *k_solid_fs =
    "#version 330 core\n"
    "in vec3 vn; uniform vec4 color; uniform vec3 lightdir; uniform float lit;\n"
    "out vec4 o;\n"
    "void main(){ float d=mix(1.0,0.45+0.55*max(dot(normalize(vn),lightdir),0.0),lit); o=vec4(color.rgb*d,color.a); }\n";

static void push_tri(std::vector<float> &v, V3 a, V3 b, V3 c)
{
    V3 n = norm(cross(b - a, c - a));
    V3 p[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        v.push_back(p[i].x); v.push_back(p[i].y); v.push_back(p[i].z);
        v.push_back(n.x); v.push_back(n.y); v.push_back(n.z);
    }
}

static void push_quad(std::vector<float> &v, V3 a, V3 b, V3 c, V3 d)
{
    push_tri(v, a, b, c);
    push_tri(v, a, c, d);
}

static void mesh_upload(int slot, const std::vector<float> &v)
{
    glGenVertexArrays(1, &g_mesh_vao[slot]);
    glGenBuffers(1, &g_mesh_vbo[slot]);
    glBindVertexArray(g_mesh_vao[slot]);
    glBindBuffer(GL_ARRAY_BUFFER, g_mesh_vbo[slot]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)), &v[0], GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
    glBindVertexArray(0);
    g_mesh_n[slot] = (int)(v.size() / 6);
}

static void build_meshes()
{
    std::vector<float> box, cyl, cone;
    const float h = 0.5f;
    V3 c[8] = { v3(-h, -h, 0), v3(h, -h, 0), v3(h, h, 0), v3(-h, h, 0), v3(-h, -h, 1), v3(h, -h, 1), v3(h, h, 1), v3(-h, h, 1) };
    push_quad(box, c[0], c[3], c[2], c[1]);
    push_quad(box, c[4], c[5], c[6], c[7]);
    push_quad(box, c[0], c[1], c[5], c[4]);
    push_quad(box, c[1], c[2], c[6], c[5]);
    push_quad(box, c[2], c[3], c[7], c[6]);
    push_quad(box, c[3], c[0], c[4], c[7]);
    const int N = 20;
    for (int i = 0; i < N; i++) {
        float a0 = (float)i / N * 6.2831853f, a1 = (float)(i + 1) / N * 6.2831853f;
        V3 p0 = v3(cosf(a0) * 0.5f, sinf(a0) * 0.5f, 0), p1 = v3(cosf(a1) * 0.5f, sinf(a1) * 0.5f, 0);
        V3 q0 = p0 + v3(0, 0, 1), q1 = p1 + v3(0, 0, 1);
        push_quad(cyl, p0, p1, q1, q0);
        push_tri(cyl, v3(0, 0, 1), q0, q1);
        push_tri(cyl, v3(0, 0, 0), p1, p0);
        push_tri(cone, p0, p1, v3(0, 0, 1));
        push_tri(cone, v3(0, 0, 0), p1, p0);
    }
    mesh_upload(0, box);
    mesh_upload(1, cyl);
    mesh_upload(2, cone);
    glGenVertexArrays(1, &g_line_vao);
    glGenBuffers(1, &g_line_vbo);
}

static void draw_mesh(int mesh, V3 pos, float rot, V3 sc, const float rgb[3], float alpha, bool lit)
{
    M4 model = mul(translate(pos), mul(rotz(rot), scale(sc)));
    M4 nrot = rotz(rot);
    GLint u;
    u = glGetUniformLocation(g_solid_prog, "model"); glUniformMatrix4fv(u, 1, GL_FALSE, model.m);
    u = glGetUniformLocation(g_solid_prog, "nrot"); glUniformMatrix4fv(u, 1, GL_FALSE, nrot.m);
    u = glGetUniformLocation(g_solid_prog, "scl"); glUniform3f(u, sc.x, sc.y, sc.z);
    u = glGetUniformLocation(g_solid_prog, "color"); glUniform4f(u, rgb[0], rgb[1], rgb[2], alpha);
    u = glGetUniformLocation(g_solid_prog, "lit"); glUniform1f(u, lit ? 1.0f : 0.0f);
    glBindVertexArray(g_mesh_vao[mesh]);
    glDrawArrays(GL_TRIANGLES, 0, g_mesh_n[mesh]);
}

static std::vector<float> g_lines;

static void line_ring(V3 c, float r, int segs)
{
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i / (float)segs * 6.2831853f, a1 = (float)(i + 1) / (float)segs * 6.2831853f;
        float x0 = c.x + cosf(a0) * r, y0 = c.y + sinf(a0) * r, x1 = c.x + cosf(a1) * r, y1 = c.y + sinf(a1) * r;
        float z0 = height_at(x0, y0) + 0.25f, z1 = height_at(x1, y1) + 0.25f;
        float p[12] = { x0, y0, z0, 0, 0, 1, x1, y1, z1, 0, 0, 1 };
        g_lines.insert(g_lines.end(), p, p + 12);
    }
}

static void flush_lines(const float rgb[3], float alpha)
{
    if (g_lines.empty())
        return;
    M4 id = identity();
    GLint u;
    u = glGetUniformLocation(g_solid_prog, "model"); glUniformMatrix4fv(u, 1, GL_FALSE, id.m);
    u = glGetUniformLocation(g_solid_prog, "nrot"); glUniformMatrix4fv(u, 1, GL_FALSE, id.m);
    u = glGetUniformLocation(g_solid_prog, "scl"); glUniform3f(u, 1, 1, 1);
    u = glGetUniformLocation(g_solid_prog, "color"); glUniform4f(u, rgb[0], rgb[1], rgb[2], alpha);
    u = glGetUniformLocation(g_solid_prog, "lit"); glUniform1f(u, 0.0f);
    glBindVertexArray(g_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_line_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_lines.size() * sizeof(float)), &g_lines[0], GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
    glDrawArrays(GL_LINES, 0, (GLsizei)(g_lines.size() / 6));
    g_lines.clear();
}

static float g_light[3] = { 0.4f, -0.5f, 0.8f };

static void draw_object(const EObj &o, bool selected)
{
    float rgb[3];
    int mesh;
    V3 sc;
    int cat = category(o);
    cat_look(cat, rgb, &mesh, &sc);
    V3 pos = v3(o.x, o.y, o.z);
    if (o.kind == K_START) {
        const float *pc = k_player_rgb[o.num >= 1 && o.num <= 8 ? o.num : 0];
        float pole[3] = { 0.75f, 0.75f, 0.78f };
        draw_mesh(1, pos, 0, v3(0.22f, 0.22f, 4.0f), pole, 1.0f, true);
        V3 fp = pos + v3(cosf(o.rot) * 0.95f, sinf(o.rot) * 0.95f, 2.6f);
        draw_mesh(0, fp, o.rot, v3(1.7f, 0.1f, 1.2f), pc, 1.0f, true);
        draw_mesh(1, pos, 0, v3(1.6f, 1.6f, 0.2f), pc, 1.0f, true);
    } else if (o.kind == K_ATMOS) {
        float dark[3] = { 0.35f, 0.33f, 0.32f };
        draw_mesh(1, pos, o.rot, v3(3.4f, 3.4f, 4.0f), rgb, 1.0f, true);
        draw_mesh(2, pos + v3(0, 0, 4.0f), o.rot, v3(2.2f, 2.2f, 2.0f), dark, 1.0f, true);
    } else if (o.kind == K_WILD) {
        if (!gd_draw(o.cls, g_vp.m, pos.x, pos.y, pos.z, o.rot, selected, g_light)) {
            draw_mesh(2, pos, o.rot, sc, rgb, 1.0f, true);
            draw_mesh(1, pos, 0, v3(0.6f, 0.6f, 2.2f), rgb, 1.0f, true);
        }
        line_ring(pos, std::max(o.radius, 0.5f), 48);
    } else if (gd_draw(o.cls, g_vp.m, pos.x, pos.y, pos.z, o.rot, selected, g_light)) {
        if (o.owner > 0) {
            float oc[3] = { k_player_rgb[o.owner][0], k_player_rgb[o.owner][1], k_player_rgb[o.owner][2] };
            float r = pick_radius(o);
            draw_mesh(1, pos, 0, v3(r * 1.6f, r * 1.6f, 0.12f), oc, 1.0f, false);
        }
    } else {
        draw_mesh(mesh, pos, o.rot, sc, rgb, 1.0f, true);
        if (cat == C_TREE) {
            float trunk[3] = { 0.40f, 0.28f, 0.16f };
            draw_mesh(1, pos, 0, v3(0.5f, 0.5f, 1.4f), trunk, 1.0f, true);
        }
        if (o.owner > 0) {
            float oc[3] = { k_player_rgb[o.owner][0], k_player_rgb[o.owner][1], k_player_rgb[o.owner][2] };
            draw_mesh(1, pos, 0, v3(sc.x * 1.3f + 0.4f, sc.y * 1.3f + 0.4f, 0.12f), oc, 1.0f, false);
        }
    }
    if (selected) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        float yel[3] = { 1.0f, 0.95f, 0.2f };
        float r = pick_radius(o);
        draw_mesh(1, pos - v3(0, 0, 0.1f), 0, v3(r * 2.0f, r * 2.0f, 6.0f), yel, 1.0f, false);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

struct Ray {
    V3 o, d;
};

static Ray mouse_ray(float mx, float my)
{
    float vw = g_vx1 > g_vx0 ? g_vx1 - g_vx0 : (float)g_w, vh = g_vy1 > g_vy0 ? g_vy1 - g_vy0 : (float)g_h;
    float ox = g_vx1 > g_vx0 ? g_vx0 : 0.0f, oy = g_vy1 > g_vy0 ? g_vy0 : 0.0f;
    float x = (mx - ox) / vw * 2.0f - 1.0f, y = 1.0f - (my - oy) / vh * 2.0f;
    M4 inv;
    M4 &v = g_vp;
    (void)inv; (void)v;
    float aspect = vw / vh;
    float t = tanf(0.5f * 0.9f);
    V3 f = norm(g_cam.target - g_eye), s = norm(cross(f, v3(0, 0, 1))), u = cross(s, f);
    V3 d = norm(f + s * (x * t * aspect) + u * (y * t));
    Ray r = { g_eye, d };
    return r;
}

static bool ray_terrain(const Ray &r, V3 *hit)
{
    float tmax = 3000.0f, step = 0.5f, t = 0.0f;
    float prev_t = 0;
    bool prev_above = true;
    while (t < tmax) {
        V3 p = r.o + r.d * t;
        if (p.x >= 0 && p.y >= 0 && p.x <= (float)(g_t.w - 1) && p.y <= (float)(g_t.h - 1)) {
            bool above = p.z > height_at(p.x, p.y);
            if (!above && prev_above && t > 0) {
                float a = prev_t, b = t;
                for (int i = 0; i < 12; i++) {
                    float m = 0.5f * (a + b);
                    V3 q = r.o + r.d * m;
                    if (q.z > height_at(q.x, q.y))
                        a = m;
                    else
                        b = m;
                }
                *hit = r.o + r.d * (0.5f * (a + b));
                return true;
            }
            prev_above = above;
        } else {
            prev_above = true;
        }
        prev_t = t;
        t += step;
        step = std::min(step * 1.01f, 3.0f);
    }
    return false;
}

static bool ray_ground_plane(const Ray &r, V3 *hit)
{
    if (fabsf(r.d.z) < 1e-5f)
        return false;
    float t = -r.o.z / r.d.z;
    if (t < 0)
        return false;
    *hit = r.o + r.d * t;
    return true;
}

static int pick_object(const Ray &r)
{
    int best = -1;
    float bt = 1e9f;
    for (size_t i = 0; i < g_objs.size(); i++) {
        const EObj &o = g_objs[i];
        float rad = pick_radius(o);
        V3 c = v3(o.x, o.y, o.z + rad * 0.8f);
        V3 oc = c - r.o;
        float tca = dot(oc, r.d);
        if (tca < 0)
            continue;
        float d2 = dot(oc, oc) - tca * tca;
        if (d2 > rad * rad)
            continue;
        if (tca < bt) {
            bt = tca;
            best = (int)i;
        }
    }
    return best;
}

static void cam_update()
{
    float cp = cosf(g_cam.pitch), sp = sinf(g_cam.pitch);
    g_eye = g_cam.target + v3(cosf(g_cam.yaw) * cp, sinf(g_cam.yaw) * cp, sp) * g_cam.dist;
    g_view = lookat(g_eye, g_cam.target, v3(0, 0, 1));
    float vw = g_vx1 > g_vx0 ? g_vx1 - g_vx0 : (float)g_w, vh = g_vy1 > g_vy0 ? g_vy1 - g_vy0 : (float)g_h;
    g_proj = perspective(0.9f, vw / std::max(vh, 1.0f), 0.5f, 4000.0f);
    g_vp = mul(g_proj, g_view);
    if (g_vx1 > g_vx0) {
        M4 xf = mul(translate(v3((g_vx0 + g_vx1) / (float)g_w - 1.0f, 1.0f - (g_vy0 + g_vy1) / (float)g_h, 0.0f)), scale(v3(vw / (float)g_w, vh / (float)g_h, 1.0f)));
        g_vp = mul(xf, g_vp);
    }
}

static void cam_pan(float dx, float dy)
{
    V3 f = v3(-cosf(g_cam.yaw), -sinf(g_cam.yaw), 0), r = v3(-f.y, f.x, 0);
    g_cam.target = g_cam.target + f * dy + r * dx * -1.0f;
    g_cam.target.x = std::min(std::max(g_cam.target.x, 0.0f), (float)g_t.w);
    g_cam.target.y = std::min(std::max(g_cam.target.y, 0.0f), (float)g_t.h);
    g_cam.target.z = height_at(g_cam.target.x, g_cam.target.y);
}

static void cam_frame()
{
    g_cam.target = v3((float)g_t.w * 0.5f, (float)g_t.h * 0.5f, 0);
    g_cam.dist = std::max((float)std::max(g_t.w, g_t.h) * 1.05f, 40.0f);
    g_cam.pitch = 0.95f;
    g_cam.yaw = 0.8f;
}

static void strip_ws(const char *src, char *dst, size_t n)
{
    size_t k = 0;
    for (; *src && k + 1 < n; src++)
        if (!isspace((unsigned char)*src))
            dst[k++] = *src;
    dst[k] = 0;
}

static void to_nmap(Nmap *m)
{
    nmap_init(m, g_base);
    m->relative = 0;
    m->tw = g_t.w;
    m->th = g_t.h;
    m->bw = (float)g_t.w;
    m->bh = (float)g_t.h;
    snprintf(m->title, sizeof m->title, "%s", g_meta.title);
    snprintf(m->author, sizeof m->author, "%s", g_meta.author);
    snprintf(m->desc, sizeof m->desc, "%s", g_meta.desc);
    m->minp = g_meta.minp;
    m->maxp = g_meta.maxp;
    for (int k = 0; k < NMAP_MAX_SPAWN; k++)
        if (g_slot[k].race != -1 || g_slot[k].team != 0) {
            m->slot[k].used = 1;
            m->slot[k].race = g_slot[k].race;
            m->slot[k].team = g_slot[k].team;
        }
    for (size_t i = 0; i < g_objs.size(); i++) {
        const EObj &o = g_objs[i];
        if (o.kind == K_START && o.num >= 1 && o.num <= NMAP_MAX_SPAWN) {
            NmapSpawn &s = m->spawn[o.num - 1];
            s.used = 1; s.x = o.x; s.y = o.y; s.z = o.z; s.rot = o.rot;
        } else if (o.kind == K_WILD && m->nwild < NMAP_MAX_WILD) {
            NmapWild &w = m->wild[m->nwild++];
            memset(&w, 0, sizeof w);
            strip_ws(o.cls, w.cls, sizeof w.cls);
            if (!w.cls[0]) {
                m->nwild--;
                continue;
            }
            w.x = o.x; w.y = o.y; w.z = o.z; w.max = o.max; w.delay = o.delay; w.radius = o.radius;
        } else if ((o.kind == K_OBJ || o.kind == K_ATMOS) && m->nobj < NMAP_MAX_OBJ) {
            NmapObj &n = m->obj[m->nobj++];
            memset(&n, 0, sizeof n);
            strip_ws(o.kind == K_ATMOS ? "GAtmosProcessor" : o.cls, n.cls, sizeof n.cls);
            if (!n.cls[0]) {
                m->nobj--;
                continue;
            }
            snprintf(n.cls, sizeof n.cls, "%s", o.kind == K_ATMOS ? "GAtmosProcessor" : o.cls);
            n.owner = o.kind == K_ATMOS ? 0 : o.owner;
            n.x = o.x; n.y = o.y; n.z = o.z; n.rot = o.rot;
        }
    }
}

static bool do_save(const char *name)
{
    if (!nmap_name_ok(name)) {
        say("Invalid map name (letters, digits, - and _)");
        return false;
    }
    Nmap *m = (Nmap *)malloc(sizeof *m);
    if (!m)
        return false;
    to_nmap(m);
    bool ok = nmap_save(m, name) != 0;
    free(m);
    if (ok) {
        NmapTerrain t;
        if (nmap_terrain_alloc(&t, g_t.w, g_t.h)) {
            memcpy(t.z, &g_t.z[0], g_t.z.size() * sizeof(int16_t));
            memcpy(t.splat, &g_t.sp[0], g_t.sp.size());
            memcpy(t.flags, &g_t.fl[0], g_t.fl.size());
            t.hmin = g_hmin;
            t.hmax = g_hmax;
            t.hstep = g_hstep;
            t.hlevels = g_hlevels;
            ok = nmap_terrain_save(&t, name) != 0;
            nmap_terrain_free(&t);
        } else
            ok = false;
    }
    if (ok) {
        if (name != g_name)
            snprintf(g_name, sizeof g_name, "%s", name);
        g_dirty = false;
        recent_add(name);
        say("Saved maps/%s.nmap", name);
    } else
        say("Could not save the map");
    return ok;
}

static void meta_reset()
{
    memset(&g_meta, 0, sizeof g_meta);
    for (int k = 0; k < NMAP_MAX_SPAWN; k++) {
        g_slot[k].race = -1;
        g_slot[k].team = 0;
    }
}

static void reset_doc(const char *base, int w, int h, int layer)
{
    meta_reset();
    g_hmin = NMAP_HMIN_DEF;
    g_hmax = NMAP_HMAX_DEF;
    g_hstep = NMAP_HSTEP_DEF;
    g_hlevels = NMAP_HLEVELS_DEF;
    snprintf(g_base, sizeof g_base, "%s", base);
    terr_new(w, h, layer);
    g_objs.clear();
    g_sel.clear();
    g_undo.clear();
    g_redo.clear();
    g_terr_snap.reset();
    g_dirty = false;
    g_name[0] = 0;
    mesh_rebuild_all();
    cam_frame();
}

static bool do_open(const char *name)
{
    Nmap *m = (Nmap *)malloc(sizeof *m);
    if (!m)
        return false;
    int e = nmap_load(m, name);
    if (e < 0) {
        free(m);
        say("Could not open %s", name);
        return false;
    }
    const BaseLevel *bl = find_base(m->base);
    int w = m->tw, h = m->th;
    NmapTerrain nt;
    bool have = false;
    memset(&nt, 0, sizeof nt);
    if (w > 0 && nmap_terrain_load(&nt, name)) {
        have = true;
        w = nt.w;
        h = nt.h;
    }
    if (w <= 0) {
        w = bl ? bl->w : (m->bw > 8 ? (int)m->bw : 230);
        h = bl ? bl->h : (m->bh > 8 ? (int)m->bh : 230);
    }
    w = std::min(std::max(w, 8), 1024);
    h = std::min(std::max(h, 8), 1024);
    reset_doc(m->base[0] ? m->base : "M01", w, h, 0);
    if (have) {
        memcpy(&g_t.z[0], nt.z, g_t.z.size() * sizeof(int16_t));
        memcpy(&g_t.sp[0], nt.splat, g_t.sp.size());
        memcpy(&g_t.fl[0], nt.flags, g_t.fl.size());
        g_hmin = nt.hmin;
        g_hmax = nt.hmax;
        g_hstep = nt.hstep;
        g_hlevels = nt.hlevels;
        nmap_terrain_free(&nt);
        mesh_rebuild_all();
    } else {
        for (size_t i = 0; i < g_t.z.size(); i++) {
            g_t.sp[i * NMAP_LAYERS + 0] = 0;
            g_t.sp[i * NMAP_LAYERS + 3] = 255;
        }
        mesh_rebuild_all();
    }
    float sx = m->relative ? (float)w : 1.0f, sy = m->relative ? (float)h : 1.0f;
    for (int i = 0; i < NMAP_MAX_SPAWN; i++)
        if (m->spawn[i].used) {
            EObj o;
            memset(&o, 0, sizeof o);
            o.kind = K_START;
            o.num = i + 1;
            snprintf(o.cls, sizeof o.cls, "Player Start");
            o.x = m->spawn[i].x * sx; o.y = m->spawn[i].y * sy; o.rot = m->spawn[i].rot;
            o.z = m->relative ? height_at(o.x, o.y) : m->spawn[i].z;
            g_objs.push_back(o);
        }
    for (int i = 0; i < m->nobj; i++) {
        EObj o;
        memset(&o, 0, sizeof o);
        o.kind = nmap_is_atmos(m->obj[i].cls) ? K_ATMOS : K_OBJ;
        snprintf(o.cls, sizeof o.cls, "%s", m->obj[i].cls);
        o.owner = m->obj[i].owner;
        o.x = m->obj[i].x * sx; o.y = m->obj[i].y * sy; o.rot = m->obj[i].rot;
        o.z = m->relative ? height_at(o.x, o.y) : m->obj[i].z;
        g_objs.push_back(o);
    }
    for (int i = 0; i < m->nwild; i++) {
        EObj o;
        memset(&o, 0, sizeof o);
        o.kind = K_WILD;
        snprintf(o.cls, sizeof o.cls, "%s", m->wild[i].cls);
        o.x = m->wild[i].x * sx; o.y = m->wild[i].y * sy; o.rot = 0;
        o.z = m->relative ? height_at(o.x, o.y) : m->wild[i].z;
        o.max = m->wild[i].max; o.delay = m->wild[i].delay; o.radius = m->wild[i].radius;
        g_objs.push_back(o);
    }
    nmap_clean_text(g_meta.title, sizeof g_meta.title, m->title);
    nmap_clean_text(g_meta.author, sizeof g_meta.author, m->author);
    nmap_clean_text(g_meta.desc, sizeof g_meta.desc, m->desc);
    g_meta.minp = m->minp;
    g_meta.maxp = m->maxp;
    for (int k = 0; k < NMAP_MAX_SPAWN; k++)
        if (m->slot[k].used) {
            g_slot[k].race = m->slot[k].race;
            g_slot[k].team = m->slot[k].team;
        }
    snprintf(g_name, sizeof g_name, "%s", name);
    free(m);
    recent_add(name);
    say("Opened maps/%s.nmap", name);
    return true;
}

static void place_at(float x, float y)
{
    EObj o;
    memset(&o, 0, sizeof o);
    snap_pos(&x, &y);
    o.x = x; o.y = y; o.z = height_at(x, y);
    o.kind = g_pal_kind;
    snprintf(o.cls, sizeof o.cls, "%s", g_pal_cls);
    o.owner = g_pal_owner;
    if (o.kind == K_START) {
        o.num = next_start_num();
        if (!o.num) {
            say("All 8 Player Starts are placed");
            return;
        }
        snprintf(o.cls, sizeof o.cls, "Player Start");
        o.owner = 0;
        o.rot = 1.5708f;
    } else if (o.kind == K_ATMOS) {
        snprintf(o.cls, sizeof o.cls, "GAtmosProcessor");
        o.owner = 0;
    } else if (o.kind == K_WILD) {
        o.max = 4; o.delay = 60.0f; o.radius = 12.0f;
        o.owner = 0;
        int n = 0;
        for (size_t i = 0; i < g_objs.size(); i++)
            if (g_objs[i].kind == K_WILD)
                n++;
        if (n >= NMAP_MAX_WILD) {
            say("At most 16 Wildlife Spawners");
            return;
        }
    } else {
        size_t n = 0;
        for (size_t i = 0; i < g_objs.size(); i++)
            if (g_objs[i].kind == K_OBJ || g_objs[i].kind == K_ATMOS)
                n++;
        if (n >= NMAP_MAX_OBJ - 1) {
            say("Object limit reached");
            return;
        }
    }
    push_undo();
    g_objs.push_back(o);
    g_sel.assign(1, (int)g_objs.size() - 1);
}

static void delete_selected()
{
    if (g_sel.empty())
        return;
    push_undo();
    std::vector<int> s = g_sel;
    std::sort(s.begin(), s.end());
    for (int i = (int)s.size() - 1; i >= 0; i--)
        g_objs.erase(g_objs.begin() + s[(size_t)i]);
    g_sel.clear();
}

static void copy_selected()
{
    g_clip.clear();
    for (size_t i = 0; i < g_sel.size(); i++)
        g_clip.push_back(g_objs[(size_t)g_sel[i]]);
    if (!g_clip.empty())
        say("Copied %s object(s)", std::to_string(g_clip.size()).c_str());
}

static void paste_at(float x, float y)
{
    if (g_clip.empty())
        return;
    float cx = 0, cy = 0;
    for (size_t i = 0; i < g_clip.size(); i++) {
        cx += g_clip[i].x;
        cy += g_clip[i].y;
    }
    cx /= (float)g_clip.size();
    cy /= (float)g_clip.size();
    push_undo();
    g_sel.clear();
    for (size_t i = 0; i < g_clip.size(); i++) {
        EObj o = g_clip[i];
        if (o.kind == K_START) {
            o.num = next_start_num();
            if (!o.num)
                continue;
        } else if (o.kind == K_WILD) {
            int nw = 0;
            for (size_t j = 0; j < g_objs.size(); j++)
                if (g_objs[j].kind == K_WILD)
                    nw++;
            if (nw >= NMAP_MAX_WILD)
                continue;
        } else if (g_objs.size() >= (size_t)NMAP_MAX_OBJ - 1) {
            continue;
        }
        o.x += x - cx;
        o.y += y - cy;
        snap_pos(&o.x, &o.y);
        o.z = height_at(o.x, o.y);
        g_objs.push_back(o);
        g_sel.push_back((int)g_objs.size() - 1);
    }
}

static void minimap_update()
{
    const int S = 160;
    static std::vector<uint32_t> px;
    px.resize((size_t)S * (size_t)S);
    float hmin = 1e9f, hmax = -1e9f;
    for (size_t i = 0; i < g_t.z.size(); i += 7) {
        hmin = std::min(hmin, (float)g_t.z[i]);
        hmax = std::max(hmax, (float)g_t.z[i]);
    }
    float span = std::max(hmax - hmin, 1.0f);
    g_hmin_w = hmin * NMAP_HZ;
    g_hmax_w = std::max(hmax * NMAP_HZ, g_hmin_w + 0.1f);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int tx = x * (g_t.w - 1) / (S - 1), ty = y * (g_t.h - 1) / (S - 1);
            size_t i = (size_t)ty * (size_t)g_t.w + (size_t)tx;
            float sh = 0.65f + 0.35f * ((float)g_t.z[i] - hmin) / span;
            float rgb[3] = { 0, 0, 0 }, tot = 1;
            const uint8_t *sp = &g_t.sp[i * NMAP_LAYERS];
            tot = 0;
            for (int k = 0; k < NMAP_LAYERS; k++) {
                tot += sp[k];
                for (int c = 0; c < 3; c++)
                    rgb[c] += k_layer_rgb[k][c] * sp[k];
            }
            if (tot < 1)
                tot = 1;
            uint32_t r = (uint32_t)std::min(255.0f, rgb[0] / tot * sh * 255), g = (uint32_t)std::min(255.0f, rgb[1] / tot * sh * 255),
                     b = (uint32_t)std::min(255.0f, rgb[2] / tot * sh * 255);
            px[(size_t)(S - 1 - y) * (size_t)S + (size_t)x] = 0xFF000000u | b << 16 | g << 8 | r;
        }
    glBindTexture(GL_TEXTURE_2D, g_mm_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px[0]);
    g_mm_dirty = false;
}

struct Stroke {
    bool active;
    float ref;
    float lx, ly;
};
static Stroke g_stroke;

static bool is_brush_tool()
{
    return g_tool == T_SCULPT || g_tool == T_PAINT || g_tool == T_CAVE || g_tool == T_CLIFF;
}

static int hclamp(float v)
{
    return std::min(std::max((int)floorf(v + 0.5f), g_hmin), g_hmax);
}

static int cliff_thr()
{
    return std::max(g_hstep / 2, 12);
}

static void terr_flags_update(int x0, int y0, int x1, int y1)
{
    x0 = std::max(x0 - 1, 0);
    y0 = std::max(y0 - 1, 0);
    x1 = std::min(x1 + 1, g_t.w - 1);
    y1 = std::min(y1 + 1, g_t.h - 1);
    static const int ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
    int thr = cliff_thr();
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t &f = FL(x, y);
            f &= (uint8_t) ~(NMAP_CF_CLIFF | NMAP_CF_IMPASSABLE);
            if (!(f & NMAP_CF_RAMP)) {
                int h = H(x, y);
                for (int k = 0; k < 4; k++) {
                    int nx = x + ox[k], ny = y + oy[k];
                    if (nx < 0 || ny < 0 || nx >= g_t.w || ny >= g_t.h || (FL(nx, ny) & NMAP_CF_RAMP))
                        continue;
                    if (h - H(nx, ny) >= thr) {
                        f |= NMAP_CF_CLIFF;
                        break;
                    }
                }
                if (f & (NMAP_CF_WALL | NMAP_CF_CLIFF))
                    f |= NMAP_CF_IMPASSABLE;
            }
        }
}

static void set_layer_full(int x, int y, int layer)
{
    uint8_t *sp = &g_t.sp[((size_t)y * (size_t)g_t.w + (size_t)x) * NMAP_LAYERS];
    for (int k = 0; k < NMAP_LAYERS; k++)
        sp[k] = k == layer ? 255 : 0;
}

static unsigned cell_hash(int x, int y)
{
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static bool in_brush(float cx, float cy, int x, int y)
{
    float ddx = fabsf((float)x - cx), ddy = fabsf((float)y - cy);
    float d = g_shape == SH_SQUARE ? std::max(ddx, ddy) : sqrtf(ddx * ddx + ddy * ddy);
    return d <= g_brush_r;
}

static std::vector<uint8_t> g_wall_snap;
static int g_cave_bx0, g_cave_by0, g_cave_bx1 = -1, g_cave_by1 = -1;

static void cave_stroke_begin()
{
    g_wall_snap.assign(g_t.fl.size(), 0);
    for (size_t i = 0; i < g_t.fl.size(); i++)
        g_wall_snap[i] = (uint8_t)((g_t.fl[i] & NMAP_CF_WALL) ? 1 : 0);
    g_cave_bx0 = g_t.w;
    g_cave_by0 = g_t.h;
    g_cave_bx1 = g_cave_by1 = -1;
}

static void apply_cave(float cx, float cy, bool erase)
{
    int ww = std::max(1, (int)floorf(g_cave_wall_w + 0.5f));
    int R = ww + 2;
    int x0 = std::max((int)floorf(cx - g_brush_r), 0), x1 = std::min((int)ceilf(cx + g_brush_r), g_t.w - 1);
    int y0 = std::max((int)floorf(cy - g_brush_r), 0), y1 = std::min((int)ceilf(cy + g_brush_r), g_t.h - 1);
    int rx0 = std::max(x0 - R, 0), ry0 = std::max(y0 - R, 0), rx1 = std::min(x1 + R, g_t.w - 1), ry1 = std::min(y1 + R, g_t.h - 1);
    int rw = rx1 - rx0 + 1, rh = ry1 - ry0 + 1;
    std::vector<uint8_t> old((size_t)rw * (size_t)rh);
    for (int y = ry0; y <= ry1; y++)
        for (int x = rx0; x <= rx1; x++)
            old[(size_t)(y - ry0) * (size_t)rw + (size_t)(x - rx0)] = FL(x, y);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (!in_brush(cx, cy, x, y))
                continue;
            uint8_t &f = FL(x, y);
            if (!erase) {
                if (!(f & NMAP_CF_CAVE)) {
                    f |= NMAP_CF_CAVE;
                    f &= (uint8_t) ~(NMAP_CF_WALL | NMAP_CF_CLIFF | NMAP_CF_RAMP | NMAP_CF_IMPASSABLE);
                    set_layer_full(x, y, CAVE_LAYER);
                    H(x, y) = (int16_t)hclamp(g_stroke.ref);
                }
            } else if (f & NMAP_CF_CAVE) {
                f &= (uint8_t)~NMAP_CF_CAVE;
                set_layer_full(x, y, g_layer);
            }
        }
    for (int y = ry0; y <= ry1; y++)
        for (int x = rx0; x <= rx1; x++)
            FL(x, y) &= (uint8_t)~NMAP_CF_WALL;
    float wh = g_cave_wall_h;
    for (int y = ry0; y <= ry1; y++)
        for (int x = rx0; x <= rx1; x++) {
            if (FL(x, y) & NMAP_CF_CAVE)
                continue;
            int best = 1 << 20, floor_h = g_hmin - 1;
            for (int dy = -ww; dy <= ww; dy++)
                for (int dx = -ww; dx <= ww; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= g_t.w || ny >= g_t.h || !(FL(nx, ny) & NMAP_CF_CAVE))
                        continue;
                    int d2 = dx * dx + dy * dy;
                    if (d2 > ww * ww + ww)
                        continue;
                    best = std::min(best, d2);
                    floor_h = std::max(floor_h, (int)H(nx, ny));
                }
            if (best == (1 << 20))
                continue;
            FL(x, y) |= NMAP_CF_WALL;
            float top = (float)floor_h + wh;
            if (best > 2)
                top -= wh * (0.1f + 0.15f * (float)(cell_hash(x, y) & 255) / 255.0f);
            H(x, y) = (int16_t)std::max((int)H(x, y), hclamp(top));
            set_layer_full(x, y, CAVE_LAYER);
        }
    std::vector<uint8_t> lost((size_t)rw * (size_t)rh, 0), done((size_t)rw * (size_t)rh, 0);
    const uint8_t blk = NMAP_CF_CAVE | NMAP_CF_WALL;
    for (int y = ry0; y <= ry1; y++)
        for (int x = rx0; x <= rx1; x++) {
            size_t k = (size_t)(y - ry0) * (size_t)rw + (size_t)(x - rx0);
            if ((old[k] & blk) && !(FL(x, y) & blk)) {
                lost[k] = 1;
                set_layer_full(x, y, g_layer);
            }
        }
    for (int pass = 0; pass < 8; pass++) {
        struct Fix { int x, y, h; };
        std::vector<Fix> fixes;
        for (int y = ry0; y <= ry1; y++)
            for (int x = rx0; x <= rx1; x++) {
                size_t k = (size_t)(y - ry0) * (size_t)rw + (size_t)(x - rx0);
                if (!lost[k] || done[k])
                    continue;
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= g_t.w || ny >= g_t.h || (FL(nx, ny) & blk))
                            continue;
                        bool inr = nx >= rx0 && nx <= rx1 && ny >= ry0 && ny <= ry1;
                        size_t nk = (size_t)(ny - ry0) * (size_t)rw + (size_t)(nx - rx0);
                        if (inr && lost[nk] && !done[nk])
                            continue;
                        sum += H(nx, ny);
                        n++;
                    }
                if (n)
                    fixes.push_back({ x, y, sum / n });
            }
        if (fixes.empty())
            break;
        for (size_t i = 0; i < fixes.size(); i++) {
            H(fixes[i].x, fixes[i].y) = (int16_t)hclamp((float)fixes[i].h);
            done[(size_t)(fixes[i].y - ry0) * (size_t)rw + (size_t)(fixes[i].x - rx0)] = 1;
        }
    }
    terr_flags_update(rx0, ry0, rx1, ry1);
    g_cave_bx0 = std::min(g_cave_bx0, rx0);
    g_cave_by0 = std::min(g_cave_by0, ry0);
    g_cave_bx1 = std::max(g_cave_bx1, rx1);
    g_cave_by1 = std::max(g_cave_by1, ry1);
    mesh_upload_rows(std::max(ry0 - 1, 0), std::min(ry1 + 1, g_t.h - 1));
    g_terr_snap.reset();
    g_mm_dirty = true;
    g_dirty = true;
}

static const std::vector<std::string> &wall_pool()
{
    std::string lv = gd_available() ? g_base : "-";
    if (!g_wall_pool.empty() && g_wall_pool_level == lv)
        return g_wall_pool;
    g_wall_pool.clear();
    g_wall_pool_level = lv;
    if (gd_available()) {
        gd_set_level(g_base);
        static const char *const needles[] = { "cliff", "cavewall", "rock" };
        std::vector<std::string> v;
        for (size_t i = 0; i < sizeof needles / sizeof needles[0]; i++)
            gd_model_names(needles[i], v);
        std::sort(v.begin(), v.end());
        for (size_t i = 0; i < v.size() && g_wall_pool.size() < 24; i++) {
            std::string l = v[i];
            for (size_t k = 0; k < l.size(); k++)
                l[k] = (char)tolower((unsigned char)l[k]);
            if (l.find("rocket") != std::string::npos || l.find("entrance") != std::string::npos || v[i].size() + 1 >= sizeof(EObj::cls))
                continue;
            if (std::find(g_wall_pool.begin(), g_wall_pool.end(), "@" + v[i]) == g_wall_pool.end())
                g_wall_pool.push_back("@" + v[i]);
        }
    }
    if (g_wall_pool.empty()) {
        g_wall_pool.push_back("@GenericRock01");
        g_wall_pool.push_back("@GenericRock02");
    }
    return g_wall_pool;
}

static void cave_finish()
{
    if (g_cave_bx1 < 0 || g_wall_snap.size() != g_t.fl.size())
        return;
    const std::vector<std::string> &pool = wall_pool();
    int x0 = g_cave_bx0, y0 = g_cave_by0, x1 = g_cave_bx1, y1 = g_cave_by1;
    g_cave_bx1 = -1;
    bool removed = false;
    for (size_t i = g_objs.size(); i-- > 0;) {
        const EObj &o = g_objs[i];
        if (o.kind != K_OBJ || o.owner != 0 || std::find(pool.begin(), pool.end(), std::string(o.cls)) == pool.end())
            continue;
        int cx = (int)floorf(o.x + 0.5f), cy = (int)floorf(o.y + 0.5f);
        if (cx < x0 || cx > x1 || cy < y0 || cy > y1 || !g_wall_snap[(size_t)cy * (size_t)g_t.w + (size_t)cx])
            continue;
        g_objs.erase(g_objs.begin() + (long)i);
        removed = true;
    }
    int placed = 0;
    if (g_cave_doodads)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!(FL(x, y) & NMAP_CF_WALL) || cell_hash(x, y) % 3 != 0 || g_objs.size() >= 1000)
                    continue;
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < g_t.w && ny < g_t.h && (FL(nx, ny) & NMAP_CF_CAVE)) {
                            edge = true;
                            break;
                        }
                    }
                if (!edge)
                    continue;
                unsigned h = cell_hash(x + 11, y + 5);
                EObj o;
                memset(&o, 0, sizeof o);
                o.kind = K_OBJ;
                snprintf(o.cls, sizeof o.cls, "%s", pool[h % pool.size()].c_str());
                o.x = (float)x + ((float)(h & 15) / 15.0f - 0.5f) * 0.6f;
                o.y = (float)y + ((float)((h >> 4) & 15) / 15.0f - 0.5f) * 0.6f;
                o.z = height_at(o.x, o.y);
                o.rot = (float)((h >> 8) & 255) / 255.0f * 6.2831853f;
                g_objs.push_back(o);
                placed++;
            }
    if (removed || placed)
        g_sel.clear();
    if (placed && g_objs.size() >= 1000)
        say("Object limit reached. Some cave wall rocks were not placed.");
    g_dirty = true;
}

static void apply_cliff(float cx, float cy, float dt, bool alt)
{
    int x0 = std::max((int)floorf(cx - g_brush_r), 0), x1 = std::min((int)ceilf(cx + g_brush_r), g_t.w - 1);
    int y0 = std::max((int)floorf(cy - g_brush_r), 0), y1 = std::min((int)ceilf(cy + g_brush_r), g_t.h - 1);
    if (g_cliff_mode == CL_RAMP) {
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        std::vector<float> src((size_t)bw * (size_t)bh);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float s = 0;
                int n = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int xx = std::min(std::max(x + dx, 0), g_t.w - 1), yy = std::min(std::max(y + dy, 0), g_t.h - 1);
                        s += H(xx, yy);
                        n++;
                    }
                src[(size_t)(y - y0) * (size_t)bw + (size_t)(x - x0)] = s / (float)n;
            }
        float k = std::min(std::max(g_brush_s / 20.0f, 0.1f), 1.0f) * std::min(1.0f, dt * 30.0f);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!in_brush(cx, cy, x, y))
                    continue;
                float ddx = fabsf((float)x - cx), ddy = fabsf((float)y - cy);
                float d = g_shape == SH_SQUARE ? std::max(ddx, ddy) : sqrtf(ddx * ddx + ddy * ddy);
                float t = 1.0f - d / g_brush_r;
                float fall = t * t * (3.0f - 2.0f * t);
                uint8_t &f = FL(x, y);
                if (alt) {
                    f &= (uint8_t)~NMAP_CF_RAMP;
                    continue;
                }
                float cur = H(x, y);
                H(x, y) = (int16_t)hclamp(cur + (src[(size_t)(y - y0) * (size_t)bw + (size_t)(x - x0)] - cur) * std::min(1.0f, fall * k * 2.0f));
                if (fall > 0.15f)
                    f |= NMAP_CF_RAMP;
            }
    } else {
        int tgt = hclamp(g_stroke.ref);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (in_brush(cx, cy, x, y)) {
                    H(x, y) = (int16_t)tgt;
                    FL(x, y) &= (uint8_t)~NMAP_CF_RAMP;
                }
    }
    terr_flags_update(x0, y0, x1, y1);
    mesh_upload_rows(std::max(y0 - 1, 0), std::min(y1 + 1, g_t.h - 1));
    g_terr_snap.reset();
    g_mm_dirty = true;
    g_dirty = true;
}

static void stroke_begin(float hx, float hy, bool alt)
{
    int cx = std::min(std::max((int)floorf(hx + 0.5f), 0), g_t.w - 1), cy = std::min(std::max((int)floorf(hy + 0.5f), 0), g_t.h - 1);
    float h = (float)H(cx, cy);
    g_stroke.ref = g_fixed_h ? g_fixed_hv / NMAP_HZ : h;
    if (g_tool == T_CAVE) {
        g_stroke.ref = (FL(cx, cy) & NMAP_CF_CAVE) ? h : h - g_cave_depth;
        cave_stroke_begin();
    } else if (g_tool == T_CLIFF) {
        int lv = (int)floorf(h / (float)g_hstep + 0.5f);
        int dir = g_cliff_mode == CL_LOWER ? -1 : 1;
        if (alt)
            dir = -dir;
        lv = std::min(std::max(lv + dir, 0), g_hlevels);
        g_stroke.ref = (float)(lv * g_hstep);
    }
}

static void apply_brush(float cx, float cy, float dt, bool alt)
{
    if (g_tool == T_CAVE) {
        apply_cave(cx, cy, (g_cave_mode == CV_ERASE) != alt);
        return;
    }
    if (g_tool == T_CLIFF) {
        apply_cliff(cx, cy, dt, alt);
        return;
    }
    int x0 = std::max((int)floorf(cx - g_brush_r), 0), x1 = std::min((int)ceilf(cx + g_brush_r), g_t.w - 1);
    int y0 = std::max((int)floorf(cy - g_brush_r), 0), y1 = std::min((int)ceilf(cy + g_brush_r), g_t.h - 1);
    std::vector<float> src;
    if (g_tool == T_SCULPT && g_brush == B_SMOOTH) {
        src.resize((size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float s = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = std::min(std::max(x + dx, 0), g_t.w - 1), yy = std::min(std::max(y + dy, 0), g_t.h - 1);
                        s += H(xx, yy);
                        n++;
                    }
                src[(size_t)(y - y0) * (size_t)(x1 - x0 + 1) + (size_t)(x - x0)] = s / (float)n;
            }
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float ddx = fabsf((float)x - cx), ddy = fabsf((float)y - cy);
            float d = g_shape == SH_SQUARE ? std::max(ddx, ddy) : sqrtf(ddx * ddx + ddy * ddy);
            if (d > g_brush_r)
                continue;
            float t = 1.0f - d / g_brush_r;
            float fall = t * t * (3.0f - 2.0f * t);
            size_t i = (size_t)y * (size_t)g_t.w + (size_t)x;
            if (g_tool == T_PAINT) {
                uint8_t *sp = &g_t.sp[i * NMAP_LAYERS];
                float add = g_brush_s * 8.0f * dt * fall * (alt ? -1.0f : 1.0f) * 10.0f;
                float v = std::min(std::max((float)sp[g_layer] + add, 0.0f), 255.0f);
                float rest = 0;
                for (int k = 0; k < NMAP_LAYERS; k++)
                    if (k != g_layer)
                        rest += sp[k];
                float want_rest = 255.0f - v;
                for (int k = 0; k < NMAP_LAYERS; k++) {
                    if (k == g_layer)
                        sp[k] = (uint8_t)(v + 0.5f);
                    else if (rest > 0.5f)
                        sp[k] = (uint8_t)std::min(255.0f, (float)sp[k] * want_rest / rest + 0.5f);
                    else if (k == (g_layer + 1) % NMAP_LAYERS)
                        sp[k] = (uint8_t)want_rest;
                }
                continue;
            }
            float cur = H(x, y), nv = cur;
            float rate = g_brush_s * 20.0f * dt * fall;
            switch (g_brush) {
            case B_RAISE: nv = cur + (alt ? -rate : rate); break;
            case B_LOWER: nv = cur - (alt ? -rate : rate); break;
            case B_SMOOTH: {
                float tgt = src[(size_t)(y - y0) * (size_t)(x1 - x0 + 1) + (size_t)(x - x0)];
                nv = cur + (tgt - cur) * std::min(1.0f, g_brush_s * 0.5f * dt * fall * 4.0f);
                break;
            }
            case B_FLATTEN: nv = cur + (g_stroke.ref - cur) * std::min(1.0f, g_brush_s * 0.5f * dt * fall * 4.0f); break;
            case B_NOISE: nv = cur + ((float)rand() / (float)RAND_MAX - 0.5f) * rate * 2.0f; break;
            case B_PLATEAU: {
                float inner = 0.7f;
                float k = t > 1.0f - inner ? 1.0f : fall / 0.5f;
                nv = cur + (g_stroke.ref - cur) * std::min(1.0f, std::min(k, 1.0f) * std::min(1.0f, g_brush_s * 0.5f * dt * 6.0f));
                break;
            }
            }
            H(x, y) = (int16_t)hclamp(nv);
        }
    if (g_tool == T_SCULPT)
        terr_flags_update(x0, y0, x1, y1);
    mesh_upload_rows(y0, y1);
    g_terr_snap.reset();
    g_mm_dirty = true;
    g_dirty = true;
}

static int imkey(int k)
{
    if (k >= PK_A && k < PK_A + 26) return ImGuiKey_A + (k - PK_A);
    if (k >= PK_A + 26 && k < PK_A + 36) return ImGuiKey_0 + (k - PK_A - 26);
    if (k >= PK_F1 && k < PK_F1 + 12) return ImGuiKey_F1 + (k - PK_F1);
    switch (k) {
    case PK_TAB: return ImGuiKey_Tab;
    case PK_LEFT: return ImGuiKey_LeftArrow;
    case PK_RIGHT: return ImGuiKey_RightArrow;
    case PK_UP: return ImGuiKey_UpArrow;
    case PK_DOWN: return ImGuiKey_DownArrow;
    case PK_PGUP: return ImGuiKey_PageUp;
    case PK_PGDN: return ImGuiKey_PageDown;
    case PK_HOME: return ImGuiKey_Home;
    case PK_END: return ImGuiKey_End;
    case PK_INSERT: return ImGuiKey_Insert;
    case PK_DELETE: return ImGuiKey_Delete;
    case PK_BACKSPACE: return ImGuiKey_Backspace;
    case PK_SPACE: return ImGuiKey_Space;
    case PK_ENTER: return ImGuiKey_Enter;
    case PK_ESCAPE: return ImGuiKey_Escape;
    case PK_LSHIFT: return ImGuiKey_LeftShift;
    case PK_LCTRL: return ImGuiKey_LeftCtrl;
    case PK_LALT: return ImGuiKey_LeftAlt;
    }
    return ImGuiKey_None;
}

static bool g_keys[128];
static bool g_mb[3];
static float g_mx, g_my;
static int g_mouse_seen;
static bool g_dlg_new, g_dlg_open, g_dlg_saveas, g_dlg_about;
static char g_dlg_name[NMAP_NAME];
static int g_dlg_base;
static int g_dlg_layer = 3;
static char g_names[64][NMAP_NAME];
static int g_nnames;
static int g_dlg_pick;
static bool g_pending_quit;
static bool g_want_quit;
static char g_exe_dir[1100];

static bool drag_active, box_active;
static V3 drag_grab;
static float box_x0, box_y0;
static bool drag_moved;
static bool g_ui_hover;

static void do_test()
{
    if (!do_save(g_name[0] ? g_name : "untitled"))
        return;
    char exe[1300];
#ifdef _WIN32
    snprintf(exe, sizeof exe, "%s\\AvPExtinctionMP.exe", g_exe_dir);
#else
    snprintf(exe, sizeof exe, "%s/AvPExtinctionMP", g_exe_dir);
#endif
    if (plat_launch(exe, "--test-map", g_name))
        say("Launched the game with %s", g_name);
    else
        say("AvPExtinctionMP was not found next to the editor");
}

#include "ui_panels.h"

static void world_labels()
{
    if (!g_labels)
        return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    for (size_t i = 0; i < g_objs.size(); i++) {
        const EObj &o = g_objs[i];
        float sx, sy, d;
        float top = o.kind == K_START ? 4.6f : o.kind == K_ATMOS ? 7.5f : 3.0f;
        if (!project(g_vp, v3(o.x, o.y, o.z + top), g_w, g_h, &sx, &sy, &d))
            continue;
        if (len(g_eye - v3(o.x, o.y, o.z)) > 260.0f && o.kind != K_START && !is_sel((int)i))
            continue;
        char buf[64], full[96];
        const char *t = short_name(o, buf, sizeof buf);
        if (o.kind == K_WILD)
            snprintf(full, sizeof full, "%s x%d", t, o.max);
        else if (o.kind == K_START)
            snprintf(full, sizeof full, "P%s", t);
        else
            snprintf(full, sizeof full, "%s", t);
        ImVec2 ts = ImGui::CalcTextSize(full);
        dl->AddRectFilled(ImVec2(sx - ts.x * 0.5f - 2, sy - ts.y - 1), ImVec2(sx + ts.x * 0.5f + 2, sy + 1), IM_COL32(0, 0, 0, 150), 2.0f);
        dl->AddText(ImVec2(sx - ts.x * 0.5f, sy - ts.y), IM_COL32(255, 255, 255, 255), full);
    }
}

static void handle_world_input(float dt)
{
    ImGuiIO &io = ImGui::GetIO();
    g_ui_hover = io.WantCaptureMouse;
    bool kb_ok = !io.WantTextInput;
    bool ctrl = g_keys[PK_LCTRL], shift = g_keys[PK_LSHIFT];
    static float px, py;
    float dx = g_mx - px, dy = g_my - py;
    px = g_mx;
    py = g_my;
    if (!g_ui_hover) {
        if (io.MouseWheel != 0 && ctrl && is_brush_tool())
            g_brush_r = std::min(std::max(g_brush_r + io.MouseWheel, 1.0f), 40.0f);
        else if (io.MouseWheel != 0)
            g_cam.dist = std::min(std::max(g_cam.dist * powf(0.88f, io.MouseWheel), 6.0f), 1500.0f);
        if (g_mb[2] && !shift) {
            g_cam.yaw -= dx * 0.006f;
            g_cam.pitch = std::min(std::max(g_cam.pitch + dy * 0.006f, 0.12f), 1.5f);
        }
        if (g_mb[1] || (g_mb[2] && shift)) {
            float k = g_cam.dist * 0.0016f;
            cam_pan(-dx * k, -dy * k);
        }
        float sp = g_cam.dist * 0.9f * dt * g_cam_speed;
        float mx = 0, my = 0;
        if (g_edge_scroll && g_mouse_seen > 6 && (g_mx < 3 || g_my < 3 || g_mx > g_w - 4 || g_my > g_h - 4)) {
            if (g_mx < 3) mx -= 1;
            if (g_mx > g_w - 4) mx += 1;
            if (g_my < 3) my += 1;
            if (g_my > g_h - 4) my -= 1;
        }
        if (mx != 0 || my != 0)
            cam_pan(mx * sp, my * sp);
    }
    if (kb_ok) {
        float sp = g_cam.dist * 0.9f * dt * g_cam_speed;
        float mx = 0, my = 0;
        if (g_keys[PK_A + 'A' - 'A'] || g_keys[PK_LEFT]) mx -= 1;
        if (g_keys[PK_A + 'D' - 'A'] || g_keys[PK_RIGHT]) mx += 1;
        if (g_keys[PK_A + 'W' - 'A'] || g_keys[PK_UP]) my += 1;
        if (g_keys[PK_A + 'S' - 'A'] || g_keys[PK_DOWN]) my -= 1;
        if (!ctrl && (mx != 0 || my != 0))
            cam_pan(mx * sp, my * sp);
    }
    cam_update();
    Ray ray = mouse_ray(g_mx, g_my);
    V3 hit = v3(0, 0, 0);
    bool on_t = !g_ui_hover && ray_terrain(ray, &hit);
    bool lmb = g_mb[0], alt = ctrl;
    static bool prev_l;
    bool pressed = lmb && !prev_l, released = !lmb && prev_l;
    prev_l = lmb;

    if (is_brush_tool()) {
        if (pressed && on_t) {
            g_stroke.active = true;
            stroke_begin(hit.x, hit.y, alt);
            push_undo();
        }
        if (g_stroke.active && lmb && on_t)
            apply_brush(hit.x, hit.y, dt, alt);
        if (released) {
            if (g_stroke.active && g_tool == T_CAVE)
                cave_finish();
            g_stroke.active = false;
        }
    } else if (g_tool == T_PLACE) {
        if (pressed && on_t)
            place_at(hit.x, hit.y);
    } else {
        if (pressed && !g_ui_hover) {
            int pk = pick_object(ray);
            if (pk >= 0) {
                if (shift) {
                    auto it = std::find(g_sel.begin(), g_sel.end(), pk);
                    if (it != g_sel.end()) g_sel.erase(it); else g_sel.push_back(pk);
                } else if (!is_sel(pk))
                    g_sel.assign(1, pk);
                drag_active = true;
                drag_moved = false;
                V3 gp;
                drag_grab = on_t ? hit : (ray_ground_plane(ray, &gp) ? gp : v3(0, 0, 0));
            } else {
                box_active = true;
                box_x0 = g_mx;
                box_y0 = g_my;
                if (!shift)
                    g_sel.clear();
            }
        }
        if (drag_active && lmb && on_t) {
            float ddx = hit.x - drag_grab.x, ddy = hit.y - drag_grab.y;
            if (!drag_moved && (fabsf(ddx) > 0.3f || fabsf(ddy) > 0.3f)) {
                push_undo();
                drag_moved = true;
            }
            if (drag_moved) {
                for (size_t i = 0; i < g_sel.size(); i++) {
                    EObj &o = g_objs[(size_t)g_sel[i]];
                    o.x += ddx;
                    o.y += ddy;
                    snap_pos(&o.x, &o.y);
                    o.z = height_at(o.x, o.y);
                }
                drag_grab = hit;
                g_dirty = true;
            }
        }
        if (box_active && lmb) {
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            dl->AddRect(ImVec2(box_x0, box_y0), ImVec2(g_mx, g_my), IM_COL32(255, 240, 80, 255));
            dl->AddRectFilled(ImVec2(box_x0, box_y0), ImVec2(g_mx, g_my), IM_COL32(255, 240, 80, 40));
        }
        if (released) {
            if (box_active) {
                float x0 = std::min(box_x0, g_mx), x1 = std::max(box_x0, g_mx), y0 = std::min(box_y0, g_my), y1 = std::max(box_y0, g_my);
                if (x1 - x0 > 3 || y1 - y0 > 3)
                    for (size_t i = 0; i < g_objs.size(); i++) {
                        float sx, sy, d;
                        if (project(g_vp, v3(g_objs[i].x, g_objs[i].y, g_objs[i].z + 1.0f), g_w, g_h, &sx, &sy, &d) && sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1 && !is_sel((int)i))
                            g_sel.push_back((int)i);
                    }
            }
            drag_active = false;
            box_active = false;
        }
    }

    if (kb_ok) {
        static bool pk[128];
        auto hit_key = [&](int k) { return g_key_latch[k] || (g_keys[k] && !pk[k]); };
        auto cm = [&](int k) { return ctrl || (g_latch_mod[k] & 1) != 0; };
        if (hit_key(PK_DELETE) || hit_key(PK_BACKSPACE)) delete_selected();
        if (hit_key(PK_A + 'R' - 'A') && !cm(PK_A + 'R' - 'A') && !g_sel.empty()) {
            push_undo();
            for (size_t i = 0; i < g_sel.size(); i++)
                g_objs[(size_t)g_sel[i]].rot += shift ? -0.2618f : 0.2618f;
        }
        if (hit_key(PK_A + 'Z' - 'A') && cm(PK_A + 'Z' - 'A')) do_undo();
        if (hit_key(PK_A + 'Y' - 'A') && cm(PK_A + 'Y' - 'A')) do_redo();
        if (hit_key(PK_A + 'C' - 'A') && cm(PK_A + 'C' - 'A')) copy_selected();
        if (hit_key(PK_A + 'V' - 'A') && cm(PK_A + 'V' - 'A')) paste_at(on_t ? hit.x : g_cam.target.x, on_t ? hit.y : g_cam.target.y);
        if (hit_key(PK_A + 'S' - 'A') && cm(PK_A + 'S' - 'A')) { if (g_name[0] && !shift && !(g_latch_mod[PK_A + 'S' - 'A'] & 2)) do_save(g_name); else request_saveas_dialog(); }
        if (hit_key(PK_A + 'A' - 'A') && cm(PK_A + 'A' - 'A')) { g_sel.clear(); for (size_t i = 0; i < g_objs.size(); i++) g_sel.push_back((int)i); }
        if (hit_key(PK_A + 'G' - 'A') && !cm(PK_A + 'G' - 'A')) g_snap = !g_snap;
        if (hit_key(PK_A + 'H' - 'A') && !cm(PK_A + 'H' - 'A')) g_show_grid = !g_show_grid;
        if (hit_key(PK_A + 'L' - 'A') && !cm(PK_A + 'L' - 'A')) g_labels = !g_labels;
        if (hit_key(PK_A + 'F' - 'A') && !cm(PK_A + 'F' - 'A')) cam_frame();
        if (hit_key(PK_F1 + 4)) do_test();
        ui_hotkeys(ctrl, shift);
        if (hit_key(PK_ESCAPE)) { g_sel.clear(); g_tool = T_SELECT; }
        for (int k = 0; k < 128; k++)
            pk[k] = g_keys[k];
    }
    if (on_t && is_brush_tool()) {
        GLint u;
        glUseProgram(g_terr_prog);
        u = glGetUniformLocation(g_terr_prog, "brush");
        glUniform4f(u, hit.x, hit.y, (float)g_shape, g_brush_r);
    } else {
        glUseProgram(g_terr_prog);
        glUniform4f(glGetUniformLocation(g_terr_prog, "brush"), 0, 0, 0, 0);
    }
}

static bool g_splash_on, g_splash_force;
static double g_splash_t0, g_splash_len;

static void splash_begin()
{
    g_splash_on = true;
    g_splash_t0 = plat_time();
    double clip = intro_start();
    g_splash_len = clip > 0.0 ? clip + 0.5 : 3.2;
    if (g_splash_len < 3.2)
        g_splash_len = 3.2;
}

static void splash_end()
{
    g_splash_on = false;
    intro_stop();
    if (!g_splash_force) {
        g_splash_shown = true;
        cfg_save();
    }
}

static void splash_frame()
{
    double t = plat_time() - g_splash_t0;
    intro_pump();
    float a = 1.0f;
    const double fin = 0.9, fout = 0.9;
    if (t < fin)
        a = (float)(t / fin);
    else if (t > g_splash_len - fout)
        a = (float)((g_splash_len - t) / fout);
    a = std::min(std::max(a, 0.0f), 1.0f);
    glViewport(0, 0, g_w, g_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const char *txt = "Nostromo Editor";
    float size = std::max(28.0f, std::min((float)g_w / 11.0f, 110.0f));
    ImFont *f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0.0f, txt);
    ImVec2 p((float)g_w * 0.5f - ts.x * 0.5f, (float)g_h * 0.5f - ts.y * 0.5f);
    ImU32 col = IM_COL32(236, 236, 236, (int)(255.0f * a));
    dl->AddText(f, size, ImVec2(p.x + 1.0f, p.y), col, txt);
    dl->AddText(f, size, p, col, txt);
    ImGui::Render();
    ui_gl_render(ImGui::GetDrawData(), g_w, g_h);
    plat_swap();
    if (t >= g_splash_len)
        splash_end();
}

static void render_scene()
{
    gd_set_level(g_base);
    glViewport(0, 0, g_w, g_h);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    V3 ld = norm(v3(0.4f, -0.5f, 0.8f));
    g_light[0] = ld.x; g_light[1] = ld.y; g_light[2] = ld.z;
    glUseProgram(g_terr_prog);
    glUniform1f(glGetUniformLocation(g_terr_prog, "usetex"), gd_terrain_bind(g_terr_prog) ? 1.0f : 0.0f);
    glUniformMatrix4fv(glGetUniformLocation(g_terr_prog, "vp"), 1, GL_FALSE, g_vp.m);
    glUniform1f(glGetUniformLocation(g_terr_prog, "grid"), g_show_grid ? std::max(g_grid, 1.0f) * 4.0f : 0.0f);
    glUniform3f(glGetUniformLocation(g_terr_prog, "lightdir"), ld.x, ld.y, ld.z);
    glUniform1f(glGetUniformLocation(g_terr_prog, "overlay"), (float)g_overlay);
    glUniform4f(glGetUniformLocation(g_terr_prog, "hrange"), g_hmin_w, g_hmax_w, 0, 0);
    glUniform1f(glGetUniformLocation(g_terr_prog, "slopecos"), cosf(g_slope_deg * 0.0174533f));
    glBindVertexArray(g_terr_vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)g_terr_n, GL_UNSIGNED_INT, 0);
    glUseProgram(g_solid_prog);
    glUniformMatrix4fv(glGetUniformLocation(g_solid_prog, "vp"), 1, GL_FALSE, g_vp.m);
    glUniform3f(glGetUniformLocation(g_solid_prog, "lightdir"), ld.x, ld.y, ld.z);
    for (size_t i = 0; i < g_objs.size(); i++)
        draw_object(g_objs[i], is_sel((int)i));
    float wr[3] = { 0.95f, 0.85f, 0.35f };
    flush_lines(wr, 1.0f);
    float edge[3] = { 0.9f, 0.9f, 0.9f };
    {
        float x1 = (float)(g_t.w - 1), y1 = (float)(g_t.h - 1);
        const int S = 64;
        for (int i = 0; i < S; i++) {
            float a = x1 * (float)i / S, b = x1 * (float)(i + 1) / S, c = y1 * (float)i / S, d = y1 * (float)(i + 1) / S;
            float seg[4][12] = {
                { a, 0, height_at(a, 0) + 0.2f, 0, 0, 1, b, 0, height_at(b, 0) + 0.2f, 0, 0, 1 },
                { a, y1, height_at(a, y1) + 0.2f, 0, 0, 1, b, y1, height_at(b, y1) + 0.2f, 0, 0, 1 },
                { 0, c, height_at(0, c) + 0.2f, 0, 0, 1, 0, d, height_at(0, d) + 0.2f, 0, 0, 1 },
                { x1, c, height_at(x1, c) + 0.2f, 0, 0, 1, x1, d, height_at(x1, d) + 0.2f, 0, 0, 1 }
            };
            for (int s = 0; s < 4; s++)
                g_lines.insert(g_lines.end(), seg[s], seg[s] + 12);
        }
        flush_lines(edge, 0.6f);
    }
}

int main(int argc, char **argv)
{
    int shot_frames = -1;
    const char *shot_path = NULL, *open_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shot_path = argv[++i]; shot_frames = 6; }
        else if (!strcmp(argv[i], "--open") && i + 1 < argc) open_name = argv[++i];
        else if (!strcmp(argv[i], "--splash")) g_splash_force = true;
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { g_w = atoi(argv[++i]); g_h = atoi(argv[++i]); }
    }
    if (plat_exe_dir(g_exe_dir, sizeof g_exe_dir))
        if (CHDIR(g_exe_dir) != 0)
            fprintf(stderr, "cannot enter the program folder\n");
    if (!plat_open("Nostromo Editor", g_w, g_h)) {
        fprintf(stderr, "Could not create an OpenGL 3.3 window\n");
        return 1;
    }
    if (!gl_load(plat_getproc)) {
        fprintf(stderr, "OpenGL 3.3 is required\n");
        return 1;
    }
    if (!gd_init(g_exe_dir))
        say("%s", gd_message());
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    cfg_load();
    ui_theme(g_dark);
    io.FontGlobalScale = g_ui_scale;
    if (!ui_gl_init())
        return 1;
    g_terr_prog = gl_program(k_terr_vs, k_terr_fs);
    g_solid_prog = gl_program(k_solid_vs, k_solid_fs);
    if (!g_terr_prog || !g_solid_prog)
        return 1;
    glGenVertexArrays(1, &g_terr_vao);
    glGenBuffers(1, &g_terr_vbo);
    glGenBuffers(1, &g_terr_ebo);
    glGenTextures(1, &g_mm_tex);
    glBindTexture(GL_TEXTURE_2D, g_mm_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    build_meshes();
    reset_doc("M01", 230, 230, 3);
    if (open_name)
        do_open(open_name);
    else {
        Nmap *probe = (Nmap *)malloc(sizeof *probe);
        if (probe && nmap_load(probe, "Nostromo_Flats") >= 0)
            do_open("Nostromo_Flats");
        free(probe);
    }
    if (g_splash_force || (!g_splash_shown && !shot_path))
        splash_begin();
    double last = plat_time();
    int frame = 0;
    bool running = true;
    while (running) {
        PlatEvent e;
        io.AddKeyEvent(ImGuiMod_Ctrl, g_keys[PK_LCTRL]);
        io.AddKeyEvent(ImGuiMod_Shift, g_keys[PK_LSHIFT]);
        while (plat_poll(&e)) {
            if (g_splash_on) {
                if (e.type == PE_CLOSE)
                    g_want_quit = true;
                else if ((e.type == PE_KEY && e.b) || (e.type == PE_MOUSE_BTN && e.b))
                    splash_end();
                continue;
            }
            switch (e.type) {
            case PE_CLOSE: g_want_quit = true; break;
            case PE_RESIZE: g_w = std::max(e.a, 320); g_h = std::max(e.b, 240); break;
            case PE_FOCUS_LOST: memset(g_keys, 0, sizeof g_keys); memset(g_mb, 0, sizeof g_mb); break;
            case PE_MOUSE_MOVE: g_mouse_seen++; g_mx = (float)e.a; g_my = (float)e.b; io.AddMousePosEvent(g_mx, g_my); break;
            case PE_MOUSE_BTN:
                g_mb[e.a] = e.b != 0;
                if (!ui_skin_mouse(e.a, e.b != 0, g_mx, g_my))
                    io.AddMouseButtonEvent(e.a, e.b != 0);
                break;
            case PE_WHEEL: io.AddMouseWheelEvent(0, e.f); break;
            case PE_CHAR:
                if (!io.WantTextInput && (e.a == '[' || e.a == ']'))
                    g_brush_r = std::min(std::max(g_brush_r + (e.a == ']' ? 1.0f : -1.0f), 1.0f), 40.0f);
                else
                    io.AddInputCharacter((unsigned)e.a);
                break;
            case PE_KEY:
                if (e.a > 0 && e.a < 128) {
                    g_keys[e.a] = e.b != 0;
                    if (e.b) {
                        g_key_latch[e.a] = true;
                        g_latch_mod[e.a] = (unsigned char)((g_keys[PK_LCTRL] ? 1 : 0) | (g_keys[PK_LSHIFT] ? 2 : 0));
                    }
                }
                if (imkey(e.a) != ImGuiKey_None) io.AddKeyEvent((ImGuiKey)imkey(e.a), e.b != 0);
                break;
            }
        }
        double now = plat_time();
        float dt = (float)std::min(now - last, 0.1);
        last = now;
        io.DisplaySize = ImVec2((float)g_w, (float)g_h);
        io.DeltaTime = std::max(dt, 0.0001f);
        if (g_splash_on) {
            splash_frame();
            if (g_want_quit) {
                splash_end();
                running = false;
            }
            continue;
        }
        if (g_mm_dirty)
            minimap_update();
        ui_skin_tick(dt);
        ImGui::NewFrame();
        handle_world_input(dt);
        render_scene();
        menu_and_panels();
        memset(g_key_latch, 0, sizeof g_key_latch);
        world_labels();
        ui_skin_apply();
        ImGui::Render();
        ui_gl_render(ImGui::GetDrawData(), g_w, g_h);
        plat_swap();
        if (g_want_quit) {
            if (g_dirty && !g_pending_quit) {
                g_pending_quit = true;
                g_want_quit = false;
                say("Unsaved changes. Exit again to discard them.");
            } else
                running = false;
        }
        if (shot_path && ++frame >= shot_frames)
            running = false;
    }
    cfg_save();
    plat_close();
    return 0;
}
