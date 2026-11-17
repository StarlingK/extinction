#ifndef TEXTURE_H
#define TEXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

struct TexImage {
    int w, h, levels;
    bool alpha;
    std::vector<uint8_t> rgba;
    std::vector<size_t> offset;
};

bool tex_decode(const uint8_t *data, size_t size, uint32_t off, TexImage &out);
unsigned tex_upload(const TexImage &t);

#endif
