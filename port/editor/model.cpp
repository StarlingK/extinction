#include "model.h"
#include "texture.h"
#include "glmin.h"
#include "glrender.h"
#include "plat.h"
#include "vecmath.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const size_t k_max_parts = 512;
static const size_t k_max_verts = 600000;
static const int k_max_nodes = 256;

ModelCpu::ModelCpu()
{
    for (int i = 0; i < 3; i++) {
        bmin[i] = 1e30f;
        bmax[i] = -1e30f;
    }
}

namespace {

struct Rd {
    const std::vector<uint8_t> *d;
    bool u32(size_t o, uint32_t &v) const
    {
        if (!d || o + 4 > d->size())
            return false;
        const uint8_t *p = &(*d)[o];
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        return true;
    }
    bool u16(size_t o, uint32_t &v) const
    {
        if (!d || o + 2 > d->size())
            return false;
        const uint8_t *p = &(*d)[o];
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        return true;
    }
    bool f32(size_t o, float &v) const
    {
        uint32_t u;
        if (!u32(o, u))
            return false;
        memcpy(&v, &u, 4);
        return v == v && fabsf(v) < 1e6f;
    }
};

inline float s11(uint32_t v)
{
    int t = (int)(v & 0x7FFu);
    return (float)(t >= 1024 ? t - 2048 : t);
}

inline float s10(uint32_t v)
{
    int t = (int)(v & 0x3FFu);
    return (float)(t >= 512 ? t - 1024 : t);
}

bool decode_mesh(TbdStore &s, int pack, uint32_t off, const float node[3], ModelCpu &m)
{
    std::shared_ptr<std::vector<uint8_t> > dp = s.data(pack), pp = s.plat(pack), tp = s.temp(pack);
    if (!dp || !pp || !tp)
        return false;
    Rd d = { dp.get() };
    uint32_t nl, lp, ng, nf, gp, fp, mi, mats;
    if ((size_t)off + 0x138 > dp->size() || !d.u16((size_t)off + 0x48, nl) || nl < 1 || nl > 8 || !d.u32((size_t)off + 0x4C, lp))
        return false;
    if ((size_t)lp + 20 > dp->size() || !d.u16((size_t)lp + 8, ng) || !d.u16((size_t)lp + 10, nf) || ng < 1 || nf < 2 || nf > 16)
        return false;
    if (!d.u32((size_t)lp + 0xC, gp) || !d.u32((size_t)lp + 0x10, fp) || !d.u16((size_t)gp + 2, mi) || !d.u32((size_t)gp + 4, mats))
        return false;
    size_t mat = (size_t)mats + (size_t)mi * 0x5C;
    if (mat + 0x5C > dp->size())
        return false;
    bool have_i = false, have_v = false;
    uint32_t icnt = 0, iptr = 0, vcnt = 0, vptr = 0;
    float sc[3] = { 0, 0, 0 };
    for (uint32_t f = 0; f < nf; f++) {
        size_t fo = (size_t)fp + 0x34u * f;
        uint32_t type, cnt, bytes, ptr;
        if (fo + 0x34 > dp->size() || !d.u32(fo, type) || !d.u32(fo + 4, cnt) || !d.u32(fo + 8, bytes) || !d.u32(fo + 0x30, ptr))
            return false;
        if (type == 6 && !have_i && cnt >= 3 && cnt <= 65535 && bytes == cnt * 2u && (ptr >> 28) == 1 && (size_t)(ptr & 0x0FFFFFFFu) + bytes <= tp->size()) {
            have_i = true;
            icnt = cnt;
            iptr = ptr & 0x0FFFFFFFu;
        } else if (type == 5 && !have_v && cnt >= 1 && cnt <= 65535 && bytes == cnt * 12u && (ptr >> 28) == 3 && (size_t)(ptr & 0x0FFFFFFFu) + bytes <= pp->size()) {
            if (!d.f32(fo + 0xC, sc[0]) || !d.f32(fo + 0x10, sc[1]) || !d.f32(fo + 0x14, sc[2]))
                return false;
            have_v = true;
            vcnt = cnt;
            vptr = ptr & 0x0FFFFFFFu;
        }
    }
    if (!have_i || !have_v)
        return false;
    size_t nverts = 0;
    for (size_t i = 0; i < m.parts.size(); i++)
        nverts += m.parts[i].v.size() / 8;
    if (m.parts.size() >= k_max_parts || nverts + vcnt > k_max_verts)
        return false;
    uint32_t tptr = 0;
    ModelPart part;
    d.u32(mat + 0x40, tptr);
    part.tex = s.resolve_slot(pack, (uint32_t)(mat + 0x40), tptr, GD_T_TEXTURE);
    part.v.resize((size_t)vcnt * 8);
    const uint8_t *vb = &(*pp)[vptr];
    for (uint32_t i = 0; i < vcnt; i++) {
        const uint8_t *q = vb + (size_t)i * 12;
        uint32_t a = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        uint32_t b = (uint32_t)q[4] | ((uint32_t)q[5] << 8) | ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
        uint32_t c = (uint32_t)q[8] | ((uint32_t)q[9] << 8) | ((uint32_t)q[10] << 16) | ((uint32_t)q[11] << 24);
        float *o = &part.v[(size_t)i * 8];
        o[0] = s11(a) / 1023.0f * sc[0] + node[0];
        o[1] = s11(a >> 11) / 1023.0f * sc[1] + node[1];
        o[2] = s10(a >> 22) / 511.0f * sc[2] + node[2];
        o[3] = s11(b) / 1023.0f;
        o[4] = s11(b >> 11) / 1023.0f;
        o[5] = s10(b >> 22) / 511.0f;
        o[6] = s11(c) / 1024.0f;
        o[7] = s11(c >> 16) / 1024.0f;
        for (int k = 0; k < 3; k++) {
            if (o[k] < m.bmin[k]) m.bmin[k] = o[k];
            if (o[k] > m.bmax[k]) m.bmax[k] = o[k];
        }
    }
    const uint8_t *ib = &(*tp)[iptr];
    for (uint32_t i = 0; i + 2 < icnt; i++) {
        uint32_t t0 = (uint32_t)ib[i * 2] | ((uint32_t)ib[i * 2 + 1] << 8);
        uint32_t t1 = (uint32_t)ib[i * 2 + 2] | ((uint32_t)ib[i * 2 + 3] << 8);
        uint32_t t2 = (uint32_t)ib[i * 2 + 4] | ((uint32_t)ib[i * 2 + 5] << 8);
        if (t0 == t1 || t1 == t2 || t0 == t2 || t0 >= vcnt || t1 >= vcnt || t2 >= vcnt)
            continue;
        part.idx.push_back(t0);
        part.idx.push_back((i & 1u) ? t2 : t1);
        part.idx.push_back((i & 1u) ? t1 : t2);
    }
    if (part.idx.empty())
        return false;
    m.parts.push_back(part);
    return true;
}

struct Walker {
    TbdStore &s;
    ModelCpu &m;
    int pack;
    std::shared_ptr<std::vector<uint8_t> > dp;
    int count;
    float head_pos[3];
    bool has_head;
    uint32_t head_hash;
    void walk(uint32_t e, const float pos[3], int depth)
    {
        if (!e || depth > 24 || count >= k_max_nodes)
            return;
        Rd d = { dp.get() };
        uint32_t mesh, nc, A, B;
        if ((size_t)e + 16 > dp->size() || !d.u32(e, mesh) || !d.u32((size_t)e + 4, nc) || !d.u32((size_t)e + 8, A) || !d.u32((size_t)e + 12, B))
            return;
        count++;
        if (mesh && s.type_of(pack, mesh) == gd_crc("CRendXBMesh"))
            decode_mesh(s, pack, mesh, pos, m);
        if (nc > 64 || (size_t)A + 16u * nc > dp->size() || (size_t)B + 4u * nc > dp->size())
            return;
        for (uint32_t i = 0; i < nc; i++) {
            float v[3];
            uint32_t child, hash;
            if (!d.f32((size_t)A + 16u * i, v[0]) || !d.f32((size_t)A + 16u * i + 4, v[1]) || !d.f32((size_t)A + 16u * i + 8, v[2]) ||
                !d.u32((size_t)A + 16u * i + 12, hash) || !d.u32((size_t)B + 4u * i, child))
                continue;
            float np[3] = { pos[0] + v[0], pos[1] + v[1], pos[2] + v[2] };
            if (!has_head && hash == head_hash) {
                has_head = true;
                memcpy(head_pos, np, sizeof np);
            }
            walk(child, np, depth + 1);
        }
    }
};

bool build_skeleton(TbdStore &s, const GdRef &r, const float base[3], ModelCpu &m, float head_pos[3], bool *has_head)
{
    std::shared_ptr<std::vector<uint8_t> > dp = s.data(r.pack);
    if (!dp || (size_t)r.off + 0x5C > dp->size())
        return false;
    Walker w = { s, m, r.pack, dp, 0, { 0, 0, 0 }, false, gd_crc("head") };
    float root[3] = { base[0], base[1], base[2] };
    w.walk(r.off + 0x4C, root, 0);
    if (has_head) {
        *has_head = w.has_head;
        if (w.has_head)
            memcpy(head_pos, w.head_pos, sizeof w.head_pos);
    }
    return !m.parts.empty();
}

}

bool model_build_ref(TbdStore &s, const GdRef &r, ModelCpu &m)
{
    float zero[3] = { 0, 0, 0 };
    if (!r.ok())
        return false;
    if (s.is_type(r, GD_T_MESH))
        return decode_mesh(s, r.pack, r.off, zero, m);
    if (s.is_type(r, GD_T_SKELETON))
        return build_skeleton(s, r, zero, m, NULL, NULL);
    return false;
}

bool model_build(TbdStore &s, const GdClassRes &r, ModelCpu &m)
{
    float zero[3] = { 0, 0, 0 }, hp[3] = { 0, 0, 0 };
    bool has_head = false;
    if (!r.body.ok())
        return false;
    if (s.is_type(r.body, GD_T_MESH))
        return decode_mesh(s, r.body.pack, r.body.off, zero, m);
    if (!build_skeleton(s, r.body, zero, m, hp, &has_head))
        return false;
    if (r.head.ok() && has_head && s.is_type(r.head, GD_T_SKELETON)) {
        ModelCpu tmp;
        if (build_skeleton(s, r.head, hp, tmp, NULL, NULL)) {
            for (size_t i = 0; i < tmp.parts.size() && m.parts.size() < k_max_parts; i++)
                m.parts.push_back(tmp.parts[i]);
            for (int k = 0; k < 3; k++) {
                if (tmp.bmin[k] < m.bmin[k]) m.bmin[k] = tmp.bmin[k];
                if (tmp.bmax[k] > m.bmax[k]) m.bmax[k] = tmp.bmax[k];
            }
        }
    }
    return true;
}

namespace {

struct GpuPart {
    GLuint vao, vbo, ebo, tex;
    GLsizei n;
    bool alpha;
};

struct ModelGpu {
    std::vector<GpuPart> parts;
    float bmin[3], bmax[3];
};

typedef void (GLAPI_CALL *PFN_genfb)(GLsizei, GLuint *);
typedef void (GLAPI_CALL *PFN_bindfb)(GLenum, GLuint);
typedef void (GLAPI_CALL *PFN_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (GLAPI_CALL *PFN_check)(GLenum);
typedef void (GLAPI_CALL *PFN_rbstore)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (GLAPI_CALL *PFN_fbrb)(GLenum, GLenum, GLenum, GLuint);
typedef void (GLAPI_CALL *PFN_geti)(GLenum, GLint *);
typedef GLboolean (GLAPI_CALL *PFN_isen)(GLenum);
typedef void (GLAPI_CALL *PFN_delfb)(GLsizei, const GLuint *);

struct FboApi {
    PFN_genfb gen_fb, gen_rb;
    PFN_bindfb bind_fb, bind_rb;
    PFN_fbtex fb_tex;
    PFN_check check;
    PFN_rbstore rb_store;
    PFN_fbrb fb_rb;
    PFN_geti geti;
    PFN_isen isen;
    PFN_delfb del_fb, del_rb;
    bool ok;
};

struct State {
    int generation;
    std::map<std::string, ModelGpu *> models;
    std::map<uint64_t, std::pair<GLuint, bool> > textures;
    std::map<std::string, GLuint> tiles;
    GLuint prog;
    GLint u_vp, u_model, u_tex, u_hastex, u_acut, u_light, u_sel;
    FboApi fbo;
    bool fbo_tried;
    GLuint pfbo, prb, ptex;
    int psize;
    State() : generation(-1), prog(0), u_vp(-1), u_model(-1), u_tex(-1), u_hastex(-1), u_acut(-1), u_light(-1), u_sel(-1), fbo_tried(false), pfbo(0), prb(0), ptex(0), psize(0)
    {
        memset(&fbo, 0, sizeof fbo);
    }
};

State g_s;

const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 p;\n"
    "layout(location=1) in vec3 n;\n"
    "layout(location=2) in vec2 t;\n"
    "uniform mat4 vp; uniform mat4 model;\n"
    "out vec3 vn; out vec2 uv;\n"
    "void main(){ vn=normalize(mat3(model)*n); uv=t; gl_Position=vp*model*vec4(p,1.0); }\n";

const char *k_fs =
    "#version 330 core\n"
    "in vec3 vn; in vec2 uv;\n"
    "uniform sampler2D tex; uniform float hastex; uniform float acut; uniform vec3 lightdir; uniform float sel;\n"
    "out vec4 o;\n"
    "void main(){\n"
    " vec4 c=hastex>0.5?texture(tex,uv):vec4(0.68,0.68,0.70,1.0);\n"
    " if(acut>0.5&&c.a<0.45) discard;\n"
    " float d=0.42+0.58*abs(dot(normalize(vn),lightdir));\n"
    " vec3 col=c.rgb*d*1.25;\n"
    " col=mix(col,vec3(1.0,0.92,0.25),sel*0.38);\n"
    " o=vec4(min(col,vec3(1.0)),1.0);\n"
    "}\n";

void free_models()
{
    for (std::map<std::string, ModelGpu *>::iterator it = g_s.models.begin(); it != g_s.models.end(); ++it) {
        if (!it->second)
            continue;
        for (size_t i = 0; i < it->second->parts.size(); i++) {
            GpuPart &p = it->second->parts[i];
            glDeleteBuffers(1, &p.vbo);
            glDeleteBuffers(1, &p.ebo);
            glDeleteVertexArrays(1, &p.vao);
        }
        delete it->second;
    }
    g_s.models.clear();
    for (std::map<uint64_t, std::pair<GLuint, bool> >::iterator it = g_s.textures.begin(); it != g_s.textures.end(); ++it)
        if (it->second.first)
            glDeleteTextures(1, &it->second.first);
    g_s.textures.clear();
    for (std::map<std::string, GLuint>::iterator it = g_s.tiles.begin(); it != g_s.tiles.end(); ++it)
        if (it->second)
            glDeleteTextures(1, &it->second);
    g_s.tiles.clear();
}

bool ensure_state()
{
    TbdStore &st = gd_store();
    if (!st.available())
        return false;
    if (g_s.generation != st.generation()) {
        free_models();
        g_s.generation = st.generation();
    }
    if (!g_s.prog) {
        g_s.prog = gl_program(k_vs, k_fs);
        if (!g_s.prog)
            return false;
        g_s.u_vp = glGetUniformLocation(g_s.prog, "vp");
        g_s.u_model = glGetUniformLocation(g_s.prog, "model");
        g_s.u_tex = glGetUniformLocation(g_s.prog, "tex");
        g_s.u_hastex = glGetUniformLocation(g_s.prog, "hastex");
        g_s.u_acut = glGetUniformLocation(g_s.prog, "acut");
        g_s.u_light = glGetUniformLocation(g_s.prog, "lightdir");
        g_s.u_sel = glGetUniformLocation(g_s.prog, "sel");
    }
    return true;
}

std::pair<GLuint, bool> texture_for(const GdRef &r)
{
    TbdStore &st = gd_store();
    uint64_t key = ((uint64_t)(uint32_t)r.pack << 32) | r.off;
    std::map<uint64_t, std::pair<GLuint, bool> >::iterator it = g_s.textures.find(key);
    if (it != g_s.textures.end())
        return it->second;
    std::pair<GLuint, bool> res(0, false);
    std::shared_ptr<std::vector<uint8_t> > dp = st.data(r.pack), tp = st.temp(r.pack);
    uint32_t ptr = 0;
    if (dp && tp && (size_t)r.off + 8 <= dp->size()) {
        const uint8_t *q = &(*dp)[(size_t)r.off + 4];
        ptr = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        TexImage img;
        if ((ptr >> 28) == 1 && tex_decode(&(*tp)[0], tp->size(), ptr & 0x0FFFFFFFu, img)) {
            res.first = tex_upload(img);
            res.second = img.alpha;
        }
    }
    g_s.textures[key] = res;
    return res;
}

ModelGpu *upload(const ModelCpu &m)
{
    ModelGpu *g = new ModelGpu();
    memcpy(g->bmin, m.bmin, sizeof g->bmin);
    memcpy(g->bmax, m.bmax, sizeof g->bmax);
    for (size_t i = 0; i < m.parts.size(); i++) {
        const ModelPart &p = m.parts[i];
        GpuPart gp;
        memset(&gp, 0, sizeof gp);
        glGenVertexArrays(1, &gp.vao);
        glGenBuffers(1, &gp.vbo);
        glGenBuffers(1, &gp.ebo);
        glBindVertexArray(gp.vao);
        glBindBuffer(GL_ARRAY_BUFFER, gp.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(p.v.size() * sizeof(float)), &p.v[0], GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gp.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(p.idx.size() * sizeof(uint32_t)), &p.idx[0], GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
        glBindVertexArray(0);
        gp.n = (GLsizei)p.idx.size();
        if (p.tex.ok()) {
            std::pair<GLuint, bool> t = texture_for(p.tex);
            gp.tex = t.first;
            gp.alpha = t.second;
        }
        g->parts.push_back(gp);
    }
    return g;
}

ModelGpu *model_get(const char *cls)
{
    if (!cls || !cls[0] || !ensure_state())
        return NULL;
    std::string key = cls;
    std::map<std::string, ModelGpu *>::iterator it = g_s.models.find(key);
    if (it != g_s.models.end())
        return it->second;
    TbdStore &st = gd_store();
    ModelCpu cpu;
    bool ok = false;
    if (cls[0] == '@') {
        GdRef r = st.doodad(cls + 1);
        ok = r.ok() && model_build_ref(st, r, cpu);
    } else {
        GdClassRes cr;
        ok = st.class_resources(key, cr) && model_build(st, cr, cpu);
    }
    ModelGpu *g = ok && !cpu.parts.empty() ? upload(cpu) : NULL;
    if (ensure_state())
        g_s.models[key] = g;
    return g;
}

void draw_gpu(const ModelGpu *m, const float *vp, const float *model, bool selected, const float *light)
{
    glUseProgram(g_s.prog);
    glUniformMatrix4fv(g_s.u_vp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(g_s.u_model, 1, GL_FALSE, model);
    glUniform3f(g_s.u_light, light[0], light[1], light[2]);
    glUniform1f(g_s.u_sel, selected ? 1.0f : 0.0f);
    glUniform1i(g_s.u_tex, 0);
    for (size_t i = 0; i < m->parts.size(); i++) {
        const GpuPart &p = m->parts[i];
        glUniform1f(g_s.u_hastex, p.tex ? 1.0f : 0.0f);
        glUniform1f(g_s.u_acut, p.alpha ? 1.0f : 0.0f);
        if (p.tex)
            glBindTexture(GL_TEXTURE_2D, p.tex);
        glBindVertexArray(p.vao);
        glDrawElements(GL_TRIANGLES, p.n, GL_UNSIGNED_INT, 0);
    }
}

bool load_fbo()
{
    if (g_s.fbo_tried)
        return g_s.fbo.ok;
    g_s.fbo_tried = true;
    FboApi &f = g_s.fbo;
    f.gen_fb = (PFN_genfb)plat_getproc("glGenFramebuffers");
    f.gen_rb = (PFN_genfb)plat_getproc("glGenRenderbuffers");
    f.bind_fb = (PFN_bindfb)plat_getproc("glBindFramebuffer");
    f.bind_rb = (PFN_bindfb)plat_getproc("glBindRenderbuffer");
    f.fb_tex = (PFN_fbtex)plat_getproc("glFramebufferTexture2D");
    f.check = (PFN_check)plat_getproc("glCheckFramebufferStatus");
    f.rb_store = (PFN_rbstore)plat_getproc("glRenderbufferStorage");
    f.fb_rb = (PFN_fbrb)plat_getproc("glFramebufferRenderbuffer");
    f.geti = (PFN_geti)plat_getproc("glGetIntegerv");
    f.isen = (PFN_isen)plat_getproc("glIsEnabled");
    f.del_fb = (PFN_delfb)plat_getproc("glDeleteFramebuffers");
    f.del_rb = (PFN_delfb)plat_getproc("glDeleteRenderbuffers");
    f.ok = f.gen_fb && f.gen_rb && f.bind_fb && f.bind_rb && f.fb_tex && f.check && f.rb_store && f.fb_rb && f.geti && f.isen && f.del_fb && f.del_rb;
    return f.ok;
}

const GLenum GLE_FRAMEBUFFER = 0x8D40, GLE_RENDERBUFFER = 0x8D41, GLE_COLOR0 = 0x8CE0, GLE_DEPTH = 0x8D00, GLE_DEPTH16 = 0x81A5;
const GLenum GLE_FB_COMPLETE = 0x8CD5, GLE_FB_BINDING = 0x8CA6, GLE_VIEWPORT = 0x0BA2, GLE_PROGRAM = 0x8B8D;

bool render_thumb(const ModelGpu *m, int size, float yaw_deg, float dist, GLuint tex)
{
    if (!load_fbo())
        return false;
    FboApi &f = g_s.fbo;
    GLint old_fb = 0, vp[4] = { 0, 0, 1, 1 }, old_prog = 0;
    f.geti(GLE_FB_BINDING, &old_fb);
    f.geti(GLE_VIEWPORT, vp);
    f.geti(GLE_PROGRAM, &old_prog);
    bool depth = f.isen(GL_DEPTH_TEST) != 0, scissor = f.isen(GL_SCISSOR_TEST) != 0, cull = f.isen(GL_CULL_FACE) != 0, blend = f.isen(GL_BLEND) != 0;
    if (!g_s.pfbo || g_s.psize != size) {
        if (g_s.pfbo)
            f.del_fb(1, &g_s.pfbo);
        if (g_s.prb)
            f.del_rb(1, &g_s.prb);
        f.gen_fb(1, &g_s.pfbo);
        f.gen_rb(1, &g_s.prb);
        g_s.psize = size;
    }
    f.bind_rb(GLE_RENDERBUFFER, g_s.prb);
    f.rb_store(GLE_RENDERBUFFER, GLE_DEPTH16, size, size);
    f.bind_fb(GLE_FRAMEBUFFER, g_s.pfbo);
    f.fb_tex(GLE_FRAMEBUFFER, GLE_COLOR0, GL_TEXTURE_2D, tex, 0);
    f.fb_rb(GLE_FRAMEBUFFER, GLE_DEPTH, GLE_RENDERBUFFER, g_s.prb);
    bool ok = f.check(GLE_FRAMEBUFFER) == GLE_FB_COMPLETE;
    if (ok) {
        glViewport(0, 0, size, size);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(0.13f, 0.14f, 0.17f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        V3 c = v3(0.5f * (m->bmin[0] + m->bmax[0]), 0.5f * (m->bmin[1] + m->bmax[1]), 0.5f * (m->bmin[2] + m->bmax[2]));
        V3 e = v3(m->bmax[0] - m->bmin[0], m->bmax[1] - m->bmin[1], m->bmax[2] - m->bmin[2]);
        float r = 0.5f * len(e);
        if (r < 0.05f)
            r = 0.05f;
        float yaw = yaw_deg * 0.0174533f, pitch = 0.32f, d = r * 2.9f * (dist > 0.1f ? dist : 1.0f);
        V3 eye = c + v3(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch)) * d;
        M4 proj = perspective(0.6f, 1.0f, d * 0.05f, d * 8.0f);
        M4 view = lookat(eye, c, v3(0, 0, 1));
        M4 mvp = mul(proj, view);
        M4 id = identity();
        V3 ld = norm(v3(0.4f, -0.6f, 0.7f));
        float light[3] = { ld.x, ld.y, ld.z };
        draw_gpu(m, mvp.m, id.m, false, light);
    }
    f.bind_fb(GLE_FRAMEBUFFER, (GLuint)old_fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glUseProgram((GLuint)old_prog);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (blend) glEnable(GL_BLEND);
    return ok;
}

GLuint make_color_texture(int size)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

}

bool gd_has_model(const char *cls)
{
    return model_get(cls) != NULL;
}

bool gd_draw(const char *cls, const float *vp, float x, float y, float z, float rot, bool selected, const float *light)
{
    ModelGpu *m = model_get(cls);
    if (!m)
        return false;
    GLint prev = 0;
    if (g_s.fbo_tried || load_fbo())
        g_s.fbo.geti(GLE_PROGRAM, &prev);
    float lift = m->bmin[2] < 0.0f ? -m->bmin[2] : 0.0f;
    M4 model = mul(translate(v3(x, y, z + lift)), rotz(rot + 1.5707963f));
    draw_gpu(m, vp, model.m, selected, light);
    glUseProgram((GLuint)prev);
    return true;
}

bool gd_bounds(const char *cls, float *radius, float *height)
{
    ModelGpu *m = model_get(cls);
    if (!m)
        return false;
    float rx = fmaxf(fabsf(m->bmin[0]), fabsf(m->bmax[0])), ry = fmaxf(fabsf(m->bmin[1]), fabsf(m->bmax[1]));
    if (radius)
        *radius = sqrtf(rx * rx + ry * ry);
    if (height)
        *height = m->bmax[2] - (m->bmin[2] < 0.0f ? m->bmin[2] : 0.0f);
    return true;
}

bool gd_terrain_bind(unsigned program)
{
    static const int k_layer_slot[4] = { 0, 1, 4, 7 };
    if (!ensure_state())
        return false;
    typedef void (GLAPI_CALL *PFN_active)(GLenum);
    static PFN_active active = (PFN_active)plat_getproc("glActiveTexture");
    std::vector<GdRef> ground;
    if (!active || !gd_store().ground_textures(ground))
        return false;
    GLuint id[4];
    for (int i = 0; i < 4; i++) {
        size_t k = (size_t)k_layer_slot[i];
        if (k >= ground.size())
            return false;
        id[i] = texture_for(ground[k]).first;
        if (!id[i])
            return false;
    }
    for (int i = 0; i < 4; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "t%d", i);
        active(0x84C0u + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, id[i]);
        glUniform1i(glGetUniformLocation(program, nm), i);
    }
    active(0x84C0u);
    return true;
}

unsigned gd_tile_texture(const char *cls)
{
    if (!strncmp(cls, "layer:", 6)) {
        static const char *const k_names[4] = { "Dirt", "Grass", "Rock", "Sand" };
        static const int k_slot[4] = { 0, 1, 4, 7 };
        std::vector<GdRef> ground;
        if (!ensure_state() || !gd_store().ground_textures(ground))
            return 0;
        for (int i = 0; i < 4; i++)
            if (!strcmp(cls + 6, k_names[i]) && (size_t)k_slot[i] < ground.size())
                return texture_for(ground[(size_t)k_slot[i]]).first;
        return 0;
    }
    ModelGpu *m = model_get(cls);
    if (!m)
        return 0;
    std::map<std::string, GLuint>::iterator it = g_s.tiles.find(cls);
    if (it != g_s.tiles.end())
        return it->second;
    GLuint t = make_color_texture(96);
    if (!render_thumb(m, 96, 35.0f, 1.0f, t)) {
        glDeleteTextures(1, &t);
        t = 0;
    }
    g_s.tiles[cls] = t;
    return t;
}

unsigned gd_preview_texture(const char *cls, float rotation_deg, float distance)
{
    ModelGpu *m = model_get(cls);
    if (!m)
        return 0;
    if (!g_s.ptex)
        g_s.ptex = make_color_texture(256);
    return render_thumb(m, 256, rotation_deg, distance, g_s.ptex) ? g_s.ptex : 0;
}
