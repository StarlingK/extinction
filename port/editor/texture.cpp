#include "texture.h"
#include "glmin.h"

enum {
    TEX_MAX_LEVEL = 0x813D,
    TEX_REPEAT = 0x2901,
    TEX_LINEAR_MIPMAP_LINEAR = 0x2703
};

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int log2i(int v)
{
    int n = 0;
    while ((1 << n) < v)
        n++;
    return n;
}

static uint32_t swizzle(uint32_t x, uint32_t y, int lw, int lh)
{
    uint32_t r = 0;
    int pos = 0, xi = 0, yi = 0;
    while (xi < lw || yi < lh) {
        if (xi < lw) {
            r |= ((x >> xi) & 1u) << pos;
            pos++;
            xi++;
        }
        if (yi < lh) {
            r |= ((y >> yi) & 1u) << pos;
            pos++;
            yi++;
        }
    }
    return r;
}

bool tex_decode(const uint8_t *data, size_t size, uint32_t off, TexImage &out)
{
    if (!data || (size_t)off + 20 > size)
        return false;
    const uint8_t *hd = data + off;
    int w = (int)rd16(hd), h = (int)rd16(hd + 2), bpp = (int)rd16(hd + 4), levels = (int)rd16(hd + 6);
    uint32_t kind = rd32(hd + 8);
    if (bpp != 32 || levels < 1 || levels > 12 || w < 1 || h < 1 || w > 2048 || h > 2048)
        return false;
    if ((w & (w - 1)) || (h & (h - 1)))
        return false;
    size_t src = (size_t)off + 20, total = 0;
    std::vector<size_t> sz;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        sz.push_back((size_t)lw * (size_t)lh * 4u);
        total += sz.back();
    }
    if (src + total > size)
        return false;
    out.w = w;
    out.h = h;
    out.levels = levels;
    out.alpha = kind != 0;
    out.rgba.assign(total, 0);
    out.offset.clear();
    size_t dst = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        int bw = log2i(lw), bh = log2i(lh);
        out.offset.push_back(dst);
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++) {
                size_t si = src + (size_t)swizzle((uint32_t)x, (uint32_t)y, bw, bh) * 4u;
                size_t di = dst + ((size_t)y * (size_t)lw + (size_t)x) * 4u;
                out.rgba[di] = data[si + 2];
                out.rgba[di + 1] = data[si + 1];
                out.rgba[di + 2] = data[si];
                out.rgba[di + 3] = out.alpha ? data[si + 3] : 255;
            }
        src += sz[(size_t)l];
        dst += sz[(size_t)l];
    }
    return true;
}

unsigned tex_upload(const TexImage &t)
{
    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id)
        return 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int l = 0; l < t.levels; l++) {
        int lw = t.w >> l, lh = t.h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, &t.rgba[t.offset[(size_t)l]]);
    }
    glTexParameteri(GL_TEXTURE_2D, TEX_MAX_LEVEL, t.levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t.levels > 1 ? TEX_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TEX_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TEX_REPEAT);
    return id;
}
