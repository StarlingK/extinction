#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "avp_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef AVP_TOOLS_EMBED
#ifdef _WIN32
#define ED_NAME "NostromoEditor.exe"
#else
#define ED_NAME "NostromoEditor"
extern const uint8_t avp_ed_blob_start[];
extern const uint8_t avp_ed_blob_end[];
#endif

static int embedded(const uint8_t **p, size_t *n)
{
#ifdef _WIN32
    HRSRC r = FindResourceA(NULL, "AVPEDEXE", (LPCSTR)RT_RCDATA);
    HGLOBAL g;
    if (!r) return 0;
    g = LoadResource(NULL, r);
    if (!g) return 0;
    *p = (const uint8_t *)LockResource(g);
    *n = (size_t)SizeofResource(NULL, r);
    return *p && *n;
#else
    *p = avp_ed_blob_start;
    *n = (size_t)(avp_ed_blob_end - avp_ed_blob_start);
    return *n > 0;
#endif
}

static uint64_t fnv(uint64_t h, const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= d[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int same_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[16384];
    size_t got, total = 0;
    uint64_t h = 1469598103934665603ull;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size_t)ftell(f) != n) { fclose(f); return 0; }
    rewind(f);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        h = fnv(h, buf, got);
        total += got;
    }
    fclose(f);
    return total == n && h == fnv(1469598103934665603ull, d, n);
}

static int exe_dir(char *out, size_t cap)
{
    char *s;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (k == 0 || k >= cap) return 0;
    s = strrchr(out, '\\');
#else
    ssize_t k = readlink("/proc/self/exe", out, cap - 1);
    if (k <= 0) return 0;
    out[k] = 0;
    s = strrchr(out, '/');
#endif
    if (!s) return 0;
    *s = 0;
    return 1;
}

static int put(const char *dir, const char *name, const uint8_t *d, size_t n)
{
    char out[1400], tmp[1420];
    FILE *f;
    snprintf(out, sizeof out, "%s" AVP_SEP "%s", dir, name);
    if (same_file(out, d, n)) return 1;
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[TOOLS] cannot write %s, skipped\n", name);
        return 0;
    }
    if (fwrite(d, 1, n, f) != n || fclose(f) != 0) {
        remove(tmp);
        fprintf(stderr, "[TOOLS] cannot write %s, skipped\n", name);
        return 0;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING)) {
#else
    chmod(tmp, 0755);
    if (rename(tmp, out) != 0) {
#endif
        remove(tmp);
        fprintf(stderr, "[TOOLS] cannot replace %s, skipped\n", name);
        return 0;
    }
    fprintf(stderr, "[TOOLS] %s unpacked\n", name);
    return 1;
}

void avp_tools_unpack(void)
{
    char dir[1300];
    const uint8_t *d;
    size_t n;
    if (!exe_dir(dir, sizeof dir)) return;
    if (embedded(&d, &n)) put(dir, ED_NAME, d, n);
}
#else
void avp_tools_unpack(void)
{
}
#endif
