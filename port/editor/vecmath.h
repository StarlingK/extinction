#ifndef VECMATH_H
#define VECMATH_H

#include <math.h>
#include <string.h>

struct V3 {
    float x, y, z;
};

static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(V3 a) { return sqrtf(dot(a, a)); }
static inline V3 norm(V3 a) { float l = len(a); return l > 1e-9f ? a * (1.0f / l) : a; }

struct M4 {
    float m[16];
};

static inline M4 identity()
{
    M4 r;
    memset(&r, 0, sizeof r);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static inline M4 mul(const M4 &a, const M4 &b)
{
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static inline M4 perspective(float fovy, float aspect, float zn, float zf)
{
    M4 r;
    memset(&r, 0, sizeof r);
    float f = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zf * zn / (zn - zf);
    return r;
}

static inline M4 lookat(V3 eye, V3 at, V3 up)
{
    V3 f = norm(at - eye), s = norm(cross(f, up)), u = cross(s, f);
    M4 r = identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

static inline M4 translate(V3 t)
{
    M4 r = identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

static inline M4 rotz(float a)
{
    M4 r = identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}

static inline M4 scale(V3 s)
{
    M4 r = identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

static inline bool project(const M4 &vp, V3 p, int w, int h, float *sx, float *sy, float *depth)
{
    float x = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    float y = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    float z = vp.m[2] * p.x + vp.m[6] * p.y + vp.m[10] * p.z + vp.m[14];
    float ww = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (ww <= 0.01f)
        return false;
    *sx = (x / ww * 0.5f + 0.5f) * (float)w;
    *sy = (1.0f - (y / ww * 0.5f + 0.5f)) * (float)h;
    if (depth)
        *depth = z / ww;
    return true;
}

#endif
