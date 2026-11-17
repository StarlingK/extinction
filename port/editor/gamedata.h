#ifndef GAMEDATA_H
#define GAMEDATA_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>

struct GdRef {
    int pack;
    uint32_t off;
    GdRef() : pack(-1), off(0) {}
    GdRef(int p, uint32_t o) : pack(p), off(o) {}
    bool ok() const { return pack >= 0; }
};

struct GdClassRes {
    GdRef body, head;
};

enum {
    GD_T_MESH = 1,
    GD_T_SKELETON,
    GD_T_TEXTURE,
    GD_T_MAP,
    GD_T_COUNT
};

uint32_t gd_crc(const char *s);

struct TbdPack {
    int file;
    struct Chunk {
        uint64_t pos;
        uint32_t size;
        bool packed;
        bool has;
        Chunk() : pos(0), size(0), packed(false), has(false) {}
    };
    Chunk c_type, c_publ, c_extn, c_exta, c_data, c_plat, c_temp;
    std::vector<std::pair<uint32_t, uint32_t> > types;
    std::vector<std::pair<uint32_t, uint32_t> > publ;
    std::vector<std::pair<uint32_t, uint32_t> > extn;
    std::unordered_map<uint32_t, uint32_t> extn_by_slot;
    bool template_pack;
    std::shared_ptr<std::vector<uint8_t> > data, plat, temp;
    uint64_t stamp;
    TbdPack() : file(-1), template_pack(false), stamp(0) {}
};

class TbdStore {
public:
    TbdStore();
    bool open(const std::string &game_dir);
    bool available() const { return !files_.empty(); }
    void set_level(const std::string &code);
    const std::string &level() const { return level_; }
    bool find_symbol(uint32_t hash, GdRef &out);
    uint32_t type_of(int pack, uint32_t off) const;
    bool is_type(const GdRef &r, int kind) const;
    std::shared_ptr<std::vector<uint8_t> > data(int pack);
    std::shared_ptr<std::vector<uint8_t> > plat(int pack);
    std::shared_ptr<std::vector<uint8_t> > temp(int pack);
    bool extn_hash(int pack, uint32_t slot, uint32_t &hash) const;
    GdRef resolve_slot(int pack, uint32_t slot, uint32_t value, int kind);
    bool class_resources(const std::string &cls, GdClassRes &out);
    GdRef doodad(const std::string &name);
    void model_names(const char *needle, std::vector<std::string> &out, bool all_levels);
    std::string name_of(int pack, uint32_t off) const;
    bool ground_textures(std::vector<GdRef> &out) const;
    int file_of(int pack) const { return packs_[(size_t)pack].file; }
    int generation() const { return gen_; }

private:
    struct File {
        std::string path;
        std::string tag;
        int first, count;
        bool primary;
    };
    bool index_file(const std::string &path, const std::string &tag, bool primary);
    bool read_raw(int file, const TbdPack::Chunk &c, std::vector<uint8_t> &out) const;
    std::shared_ptr<std::vector<uint8_t> > load_chunk(TbdPack &p, const TbdPack::Chunk &c, std::shared_ptr<std::vector<uint8_t> > &slot);
    void index_others();
    void trim_cache();
    bool scan_class(uint32_t hash, int from, int to, GdClassRes &best, uint32_t &best_delta, uint32_t window);
    std::string dir_;
    std::string level_;
    std::vector<File> files_;
    std::vector<TbdPack> packs_;
    std::unordered_multimap<uint32_t, GdRef> symbols_;
    std::unordered_map<uint32_t, std::string> names_;
    std::map<std::string, std::pair<bool, GdClassRes> > class_cache_;
    bool others_indexed_;
    bool cur_class_;
    std::vector<std::string> cur_guess_;
    uint64_t clock_;
    size_t cached_bytes_;
    int gen_;
    uint32_t t_mesh_, t_skel_, t_tex_, t_map_;
    uint32_t t_tmpl_[3];
};

TbdStore &gd_store();

bool gd_init(const char *program_dir);
bool gd_available();
void gd_model_names(const char *needle, std::vector<std::string> &out);
const char *gd_message();
void gd_set_level(const char *code);
bool gd_has_model(const char *cls);
bool gd_draw(const char *cls, const float *vp, float x, float y, float z, float rot, bool selected, const float *light);
bool gd_bounds(const char *cls, float *radius, float *height);
bool gd_terrain_bind(unsigned program);
unsigned gd_tile_texture(const char *cls);
unsigned gd_preview_texture(const char *cls, float rotation_deg, float distance);

#endif
