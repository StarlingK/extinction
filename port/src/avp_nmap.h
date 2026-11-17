#ifndef AVP_NMAP_H
#define AVP_NMAP_H

#include <stddef.h>
#include <stdint.h>

#define NMAP_MAX_OBJ 1024
#define NMAP_MAX_SPAWN 8
#define NMAP_NAME 32
#define NMAP_MAX_WILD 16
#define NMAP_LAYERS 4
#define NMAP_HZ 0.1f
#define NMAP_TITLE 64
#define NMAP_AUTHOR 48
#define NMAP_DESC 160

typedef struct { char cls[48]; int owner; float x, y, z, rot; } NmapObj;
typedef struct { int used; float x, y, z, rot; } NmapSpawn;
typedef struct { char cls[32]; float x, y, z; int max; float delay, radius; } NmapWild;
typedef struct { int used; int race; int team; } NmapSlot;
#define NMAP_CF_CAVE 1
#define NMAP_CF_IMPASSABLE 2
#define NMAP_CF_CLIFF 4
#define NMAP_CF_RAMP 8
#define NMAP_CF_WALL 16
#define NMAP_HMIN_DEF (-50)
#define NMAP_HMAX_DEF 180
#define NMAP_HSTEP_DEF 40
#define NMAP_HLEVELS_DEF 4
typedef struct { int w, h; int16_t *z; uint8_t *splat; uint8_t *flags; int hmin, hmax, hstep, hlevels; } NmapTerrain;
typedef struct {
    char base[4];
    int relative;
    float bw, bh;
    int tw, th;
    int nwild;
    NmapWild wild[NMAP_MAX_WILD];
    int nobj;
    NmapObj obj[NMAP_MAX_OBJ];
    NmapSpawn spawn[NMAP_MAX_SPAWN];
    char title[NMAP_TITLE];
    char author[NMAP_AUTHOR];
    char desc[NMAP_DESC];
    int minp, maxp;
    NmapSlot slot[NMAP_MAX_SPAWN];
} Nmap;

void nmap_init(Nmap *m, const char *base);
int  nmap_parse(Nmap *m, const char *text);
int  nmap_write(const Nmap *m, char *buf, size_t n);
int  nmap_load(Nmap *m, const char *name);
int  nmap_save(const Nmap *m, const char *name);
void nmap_clean_text(char *dst, size_t n, const char *src);
int  nmap_name_ok(const char *s);
int  nmap_list(char names[][NMAP_NAME], int max);
void nmap_open_folder(void);
int  nmap_is_atmos(const char *cls);
int  nmap_start_list(const Nmap *m, int *nums);
int  nmap_terrain_alloc(NmapTerrain *t, int w, int h);
void nmap_terrain_free(NmapTerrain *t);
int  nmap_terrain_load(NmapTerrain *t, const char *name);
int  nmap_terrain_save(const NmapTerrain *t, const char *name);
int  nmap_terrain_resample(const NmapTerrain *src, NmapTerrain *dst, int w, int h);
int  nmap_terrain_path(const char *name, char *out, size_t n);
void nmap_resolve(Nmap *m, float w, float h, float z);
void nmap_to_world(Nmap *m, float x0, float y0, float w, float h, float margin);

#endif
