#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <dlfcn.h>
#endif

#ifdef AVP_INTRO_EMBED
#ifdef _WIN32
static bool blob(const unsigned char **p, size_t *n)
{
    HRSRC r = FindResourceA(NULL, "AVPINTRO", (LPCSTR)RT_RCDATA);
    if (!r)
        return false;
    HGLOBAL g = LoadResource(NULL, r);
    if (!g)
        return false;
    *p = (const unsigned char *)LockResource(g);
    *n = (size_t)SizeofResource(NULL, r);
    return *p != NULL && *n > 0;
}
#else
extern "C" {
extern const unsigned char avp_intro_blob_start[];
extern const unsigned char avp_intro_blob_end[];
}
static bool blob(const unsigned char **p, size_t *n)
{
    *p = avp_intro_blob_start;
    *n = (size_t)(avp_intro_blob_end - avp_intro_blob_start);
    return *n > 0;
}
#endif
#else
static bool blob(const unsigned char **, size_t *)
{
    return false;
}
#endif

static std::vector<int16_t> s_pcm;
static int s_rate, s_ch;
static bool s_decoded;

static bool decode()
{
    if (s_decoded)
        return !s_pcm.empty();
    s_decoded = true;
    const unsigned char *p;
    size_t n;
    if (!blob(&p, &n))
        return false;
    mp3dec_t *d = (mp3dec_t *)malloc(sizeof *d);
    if (!d)
        return false;
    mp3dec_init(d);
    mp3dec_frame_info_t info;
    short buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t pos = 0;
    while (pos < n) {
        int s = mp3dec_decode_frame(d, p + pos, (int)(n - pos), buf, &info);
        if (info.frame_bytes <= 0)
            break;
        pos += (size_t)info.frame_bytes;
        if (s > 0 && info.channels > 0) {
            if (!s_rate) {
                s_rate = info.hz;
                s_ch = info.channels;
            }
            if (info.hz == s_rate && info.channels == s_ch)
                s_pcm.insert(s_pcm.end(), buf, buf + s * info.channels);
        }
        if (s_pcm.size() > (size_t)64 * 1024 * 1024)
            break;
    }
    free(d);
    return !s_pcm.empty();
}

bool intro_available()
{
    const unsigned char *p;
    size_t n;
    return blob(&p, &n);
}

#ifdef _WIN32

static std::vector<unsigned char> s_wav;

double intro_start()
{
    if (!decode())
        return 0.0;
    uint32_t bytes = (uint32_t)(s_pcm.size() * 2);
    s_wav.resize(44 + bytes);
    unsigned char *w = &s_wav[0];
    auto u32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) w[o + (size_t)i] = (unsigned char)(v >> (8 * i)); };
    auto u16 = [&](size_t o, uint16_t v) { w[o] = (unsigned char)v; w[o + 1] = (unsigned char)(v >> 8); };
    memcpy(w, "RIFF", 4);
    u32(4, 36 + bytes);
    memcpy(w + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, (uint16_t)s_ch);
    u32(24, (uint32_t)s_rate);
    u32(28, (uint32_t)(s_rate * s_ch * 2));
    u16(32, (uint16_t)(s_ch * 2));
    u16(34, 16);
    memcpy(w + 36, "data", 4);
    u32(40, bytes);
    memcpy(w + 44, &s_pcm[0], bytes);
    PlaySoundA((LPCSTR)w, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    return (double)s_pcm.size() / (double)(s_ch * s_rate);
}

void intro_pump()
{
}

void intro_stop()
{
    PlaySoundA(NULL, NULL, 0);
}

#else

typedef int (*pcm_open_t)(void **, const char *, int, int);
typedef int (*pcm_params_t)(void *, int, int, unsigned, unsigned, int, unsigned);
typedef long (*pcm_writei_t)(void *, const void *, unsigned long);
typedef int (*pcm_recover_t)(void *, int, int);
typedef int (*pcm_close_t)(void *);
typedef int (*pcm_drop_t)(void *);

static void *s_lib, *s_pcmh;
static pcm_writei_t f_write;
static pcm_recover_t f_recover;
static pcm_close_t f_close;
static pcm_drop_t f_drop;
static size_t s_pos;

double intro_start()
{
    if (!decode())
        return 0.0;
    double secs = (double)s_pcm.size() / (double)(s_ch * s_rate);
    s_lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!s_lib)
        return secs;
    pcm_open_t fo = (pcm_open_t)dlsym(s_lib, "snd_pcm_open");
    pcm_params_t fp = (pcm_params_t)dlsym(s_lib, "snd_pcm_set_params");
    f_write = (pcm_writei_t)dlsym(s_lib, "snd_pcm_writei");
    f_recover = (pcm_recover_t)dlsym(s_lib, "snd_pcm_recover");
    f_close = (pcm_close_t)dlsym(s_lib, "snd_pcm_close");
    f_drop = (pcm_drop_t)dlsym(s_lib, "snd_pcm_drop");
    if (!fo || !fp || !f_write || !f_recover || !f_close || fo(&s_pcmh, "default", 0, 1) < 0 || fp(s_pcmh, 2, 3, (unsigned)s_ch, (unsigned)s_rate, 1, 200000) < 0) {
        if (s_pcmh && f_close)
            f_close(s_pcmh);
        s_pcmh = NULL;
        dlclose(s_lib);
        s_lib = NULL;
        return secs;
    }
    s_pos = 0;
    intro_pump();
    return secs;
}

void intro_pump()
{
    if (!s_pcmh)
        return;
    for (int guard = 0; guard < 8 && s_pos < s_pcm.size(); guard++) {
        unsigned long frames = (unsigned long)((s_pcm.size() - s_pos) / (size_t)s_ch);
        if (frames > 4096)
            frames = 4096;
        long r = f_write(s_pcmh, &s_pcm[s_pos], frames);
        if (r == -11)
            break;
        if (r < 0) {
            if (f_recover(s_pcmh, (int)r, 1) < 0)
                break;
            continue;
        }
        s_pos += (size_t)r * (size_t)s_ch;
    }
}

void intro_stop()
{
    if (s_pcmh) {
        if (f_drop)
            f_drop(s_pcmh);
        f_close(s_pcmh);
        s_pcmh = NULL;
    }
    if (s_lib) {
        dlclose(s_lib);
        s_lib = NULL;
    }
}

#endif
