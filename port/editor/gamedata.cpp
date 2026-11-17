#include "gamedata.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <ctype.h>

static const uint32_t k_max_chunk = 192u * 1024u * 1024u;
static const size_t k_cache_cap = 256u * 1024u * 1024u;

static const char *const k_levels[] = {
    "A01", "A02", "A03", "A04", "A06", "A07", "A08", "M01", "M02", "M03", "M05", "M06", "M09", "M10",
    "P01", "P02", "P03", "P04", "P06", "P08", "P11", "T01", "T02", "T03", "T04", "T05"
};

static uint32_t g_crc_table[256];
static bool g_crc_ready;

static void crc_init()
{
    if (g_crc_ready)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

uint32_t gd_crc(const char *s)
{
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++)
        c = g_crc_table[(c ^ (uint8_t)*s) & 0xFFu] ^ (c >> 8);
    return c;
}

static uint32_t crc_range(const uint8_t *p, size_t n)
{
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = g_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool lzss(const uint8_t *s, size_t n, std::vector<uint8_t> &out)
{
    if (n < 6)
        return false;
    uint32_t claim = rd32(s);
    if (claim > k_max_chunk)
        return false;
    size_t p = 4, o = 0;
    uint32_t w = (uint32_t)s[p] | ((uint32_t)s[p + 1] << 8);
    p += 2;
    int k = 0;
    uint32_t mask = 0;
    while ((w & 0x8000u) == 0 && k < 16) {
        w = (w << 1) & 0xFFFFu;
        mask = (mask << 1) | 1u;
        k++;
    }
    out.assign(claim, 0);
    while (p + 2 <= n) {
        uint32_t flags = (uint32_t)s[p] | ((uint32_t)s[p + 1] << 8);
        p += 2;
        for (uint32_t bit = 0x8000u; bit; bit >>= 1) {
            if (p + 2 > n)
                return o == claim;
            uint32_t v = (uint32_t)s[p] | ((uint32_t)s[p + 1] << 8);
            p += 2;
            if (!(flags & bit)) {
                if (o + 2 > claim)
                    return false;
                out[o++] = (uint8_t)v;
                out[o++] = (uint8_t)(v >> 8);
                continue;
            }
            if (v == 0)
                return o == claim;
            size_t cnt = (size_t)(v & mask) * 2u, back = (size_t)(v >> k) * 2u;
            if (back == 0 || back > o || o + cnt > claim)
                return false;
            size_t src = o - back;
            size_t dw = cnt / 4;
            for (size_t i = 0; i < dw; i++) {
                uint8_t t[4];
                memcpy(t, &out[src], 4);
                memcpy(&out[o], t, 4);
                src += 4;
                o += 4;
            }
            for (size_t i = cnt & 3u; i > 0; i--)
                out[o++] = out[src++];
        }
    }
    return o == claim;
}

TbdStore::TbdStore()
    : others_indexed_(false), cur_class_(false), clock_(0), cached_bytes_(0), gen_(0)
{
    t_mesh_ = gd_crc("CRendXBMesh");
    t_skel_ = gd_crc("CRendStaticSkeleton");
    t_tex_ = gd_crc("CRendTexture");
    t_map_ = gd_crc("CRendMap");
    t_tmpl_[0] = gd_crc("CHausTemplateFillData");
    t_tmpl_[1] = gd_crc("CDefaultFillData");
    t_tmpl_[2] = gd_crc("CEmbeddedFillData");
}

static bool file_size(FILE *f, uint64_t &n)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return false;
    long v = ftell(f);
    if (v < 0)
        return false;
    n = (uint64_t)v;
    return true;
}

static bool read_at(FILE *f, uint64_t pos, void *dst, size_t n)
{
    if (fseek(f, (long)pos, SEEK_SET) != 0)
        return false;
    return fread(dst, 1, n, f) == n;
}

bool TbdStore::read_raw(int file, const TbdPack::Chunk &c, std::vector<uint8_t> &out) const
{
    if (!c.has || c.size > k_max_chunk)
        return false;
    FILE *f = fopen(files_[(size_t)file].path.c_str(), "rb");
    if (!f)
        return false;
    out.resize(c.size);
    bool ok = c.size == 0 || read_at(f, c.pos, &out[0], c.size);
    fclose(f);
    return ok;
}

static void parse_pairs(const std::vector<uint8_t> &b, std::vector<std::pair<uint32_t, uint32_t> > &out)
{
    if (b.size() < 4)
        return;
    uint32_t n = rd32(&b[0]);
    if ((uint64_t)n * 8u + 4u > b.size())
        n = (uint32_t)((b.size() - 4) / 8);
    out.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        out.push_back(std::make_pair(rd32(&b[4 + 8 * (size_t)i]), rd32(&b[8 + 8 * (size_t)i])));
}

bool TbdStore::index_file(const std::string &path, const std::string &tag, bool primary)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint64_t total = 0;
    uint8_t hd[12];
    if (!file_size(f, total) || total < 12 || !read_at(f, 0, hd, 12) || memcmp(hd, "RIFF", 4) || memcmp(hd + 8, "TBD2", 4)) {
        fclose(f);
        return false;
    }
    int fi = (int)files_.size();
    File fl;
    fl.path = path;
    fl.tag = tag;
    fl.first = (int)packs_.size();
    fl.count = 0;
    fl.primary = primary;
    files_.push_back(fl);
    uint64_t pos = 12;
    while (pos + 8 <= total) {
        uint8_t ch[12];
        if (!read_at(f, pos, ch, 8))
            break;
        uint32_t sz = rd32(ch + 4);
        uint64_t end = pos + 8 + sz;
        if (end > total)
            break;
        if (!memcmp(ch, "LIST", 4) && sz >= 4 && read_at(f, pos + 8, ch + 8, 4) && !memcmp(ch + 8, "XBOX", 4)) {
            TbdPack pk;
            pk.file = fi;
            uint64_t p = pos + 12;
            while (p + 8 <= end) {
                uint8_t h2[8];
                if (!read_at(f, p, h2, 8))
                    break;
                uint32_t cs = rd32(h2 + 4);
                if (p + 8 + cs > end)
                    break;
                TbdPack::Chunk c;
                c.pos = p + 8;
                c.size = cs;
                c.has = true;
                if (!memcmp(h2, "TYPE", 4)) pk.c_type = c;
                else if (!memcmp(h2, "PUBL", 4)) pk.c_publ = c;
                else if (!memcmp(h2, "EXTN", 4)) pk.c_extn = c;
                else if (!memcmp(h2, "EXTA", 4)) pk.c_exta = c;
                else if (!memcmp(h2, "DATA", 4)) pk.c_data = c;
                else if (!memcmp(h2, "DATX", 4)) { c.packed = true; pk.c_data = c; }
                else if (!memcmp(h2, "PLAT", 4)) pk.c_plat = c;
                else if (!memcmp(h2, "PLAX", 4)) { c.packed = true; pk.c_plat = c; }
                else if (!memcmp(h2, "TEMP", 4)) pk.c_temp = c;
                else if (!memcmp(h2, "TEMX", 4)) { c.packed = true; pk.c_temp = c; }
                p += 8 + (uint64_t)cs + (cs & 1u);
            }
            if (packs_.size() < 4096) {
                packs_.push_back(pk);
                files_[(size_t)fi].count++;
            }
        }
        pos = end + (sz & 1u);
    }
    fclose(f);
    for (int i = files_[(size_t)fi].first; i < files_[(size_t)fi].first + files_[(size_t)fi].count; i++) {
        TbdPack &pk = packs_[(size_t)i];
        std::vector<uint8_t> b;
        if (read_raw(fi, pk.c_type, b))
            parse_pairs(b, pk.types);
        std::sort(pk.types.begin(), pk.types.end(), [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &c) { return a.second < c.second; });
        for (size_t k = 0; k < pk.types.size(); k++)
            if (pk.types[k].first == t_tmpl_[0] || pk.types[k].first == t_tmpl_[1] || pk.types[k].first == t_tmpl_[2])
                pk.template_pack = true;
        if (read_raw(fi, pk.c_publ, b)) {
            parse_pairs(b, pk.publ);
            for (size_t k = 0; k < pk.publ.size(); k++)
                symbols_.insert(std::make_pair(pk.publ[k].first, GdRef(i, pk.publ[k].second)));
        }
        if (read_raw(fi, pk.c_extn, b)) {
            parse_pairs(b, pk.extn);
            std::sort(pk.extn.begin(), pk.extn.end(), [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &c) { return a.second < c.second; });
            for (size_t k = 0; k < pk.extn.size(); k++)
                pk.extn_by_slot[pk.extn[k].second] = pk.extn[k].first;
        }
        if (read_raw(fi, pk.c_exta, b)) {
            size_t st = (size_t)-1;
            for (size_t k = 0; k <= b.size(); k++) {
                bool tok = k < b.size() && (isalnum(b[k]) || b[k] == '_');
                if (tok) {
                    if (st == (size_t)-1)
                        st = k;
                    continue;
                }
                if (st != (size_t)-1) {
                    size_t n = k - st;
                    if (n >= 3 && n < 96)
                        names_[crc_range(&b[st], n)] = std::string((const char *)&b[st], n);
                    st = (size_t)-1;
                }
            }
        }
    }
    return true;
}

bool TbdStore::open(const std::string &game_dir)
{
    files_.clear();
    packs_.clear();
    symbols_.clear();
    names_.clear();
    class_cache_.clear();
    others_indexed_ = false;
    cached_bytes_ = 0;
    dir_ = game_dir;
    level_.clear();
    gen_++;
    if (!index_file(dir_ + "/tbd/370BC447.tbx", "MASTER", true)) {
        files_.clear();
        return false;
    }
    return true;
}

void TbdStore::set_level(const std::string &code)
{
    if (!available() || code == level_)
        return;
    bool valid = !code.empty() && code.size() <= 4;
    for (size_t i = 0; i < code.size(); i++)
        if (!isalnum((unsigned char)code[i]))
            valid = false;
    std::string keep_dir = dir_;
    open(keep_dir);
    level_ = code;
    if (valid)
        index_file(dir_ + "/tbd/" + code + "/FD090663.tbx", code, true);
}

void TbdStore::index_others()
{
    if (others_indexed_)
        return;
    others_indexed_ = true;
    for (size_t i = 0; i < sizeof k_levels / sizeof k_levels[0]; i++) {
        if (level_ == k_levels[i])
            continue;
        index_file(dir_ + "/tbd/" + k_levels[i] + "/FD090663.tbx", k_levels[i], false);
    }
}

bool TbdStore::find_symbol(uint32_t hash, GdRef &out)
{
    for (int pass = 0; pass < 2; pass++) {
        std::pair<std::unordered_multimap<uint32_t, GdRef>::iterator, std::unordered_multimap<uint32_t, GdRef>::iterator> r = symbols_.equal_range(hash);
        const GdRef *best = NULL;
        for (std::unordered_multimap<uint32_t, GdRef>::iterator it = r.first; it != r.second; ++it) {
            const File &fl = files_[(size_t)packs_[(size_t)it->second.pack].file];
            if (fl.tag == level_ && !level_.empty()) {
                out = it->second;
                return true;
            }
            if (!best || it->second.pack < best->pack)
                best = &it->second;
        }
        if (best) {
            out = *best;
            return true;
        }
        if (pass == 0) {
            if (others_indexed_ || !available())
                return false;
            index_others();
        }
    }
    return false;
}

uint32_t TbdStore::type_of(int pack, uint32_t off) const
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return 0;
    const std::vector<std::pair<uint32_t, uint32_t> > &t = packs_[(size_t)pack].types;
    size_t lo = 0, hi = t.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].second < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < t.size() && t[lo].second == off ? t[lo].first : 0;
}

bool TbdStore::is_type(const GdRef &r, int kind) const
{
    uint32_t t = type_of(r.pack, r.off);
    if (!t)
        return false;
    switch (kind) {
    case GD_T_MESH: return t == t_mesh_;
    case GD_T_SKELETON: return t == t_skel_;
    case GD_T_TEXTURE: return t == t_tex_;
    case GD_T_MAP: return t == t_map_;
    }
    return false;
}

void TbdStore::trim_cache()
{
    if (cached_bytes_ <= k_cache_cap)
        return;
    for (size_t i = 0; i < packs_.size(); i++) {
        packs_[i].data.reset();
        packs_[i].plat.reset();
        packs_[i].temp.reset();
    }
    cached_bytes_ = 0;
}

std::shared_ptr<std::vector<uint8_t> > TbdStore::load_chunk(TbdPack &p, const TbdPack::Chunk &c, std::shared_ptr<std::vector<uint8_t> > &slot)
{
    if (slot)
        return slot;
    if (!c.has)
        return slot;
    std::vector<uint8_t> raw;
    if (!read_raw(p.file, c, raw))
        return slot;
    std::shared_ptr<std::vector<uint8_t> > out(new std::vector<uint8_t>());
    if (c.packed) {
        if (!lzss(raw.empty() ? NULL : &raw[0], raw.size(), *out))
            return slot;
    } else {
        out->swap(raw);
    }
    trim_cache();
    cached_bytes_ += out->size();
    slot = out;
    return slot;
}

std::shared_ptr<std::vector<uint8_t> > TbdStore::data(int pack)
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return std::shared_ptr<std::vector<uint8_t> >();
    TbdPack &p = packs_[(size_t)pack];
    return load_chunk(p, p.c_data, p.data);
}

std::shared_ptr<std::vector<uint8_t> > TbdStore::plat(int pack)
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return std::shared_ptr<std::vector<uint8_t> >();
    TbdPack &p = packs_[(size_t)pack];
    return load_chunk(p, p.c_plat, p.plat);
}

std::shared_ptr<std::vector<uint8_t> > TbdStore::temp(int pack)
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return std::shared_ptr<std::vector<uint8_t> >();
    TbdPack &p = packs_[(size_t)pack];
    return load_chunk(p, p.c_temp, p.temp);
}

bool TbdStore::extn_hash(int pack, uint32_t slot, uint32_t &hash) const
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return false;
    const TbdPack &p = packs_[(size_t)pack];
    std::unordered_map<uint32_t, uint32_t>::const_iterator it = p.extn_by_slot.find(slot);
    if (it == p.extn_by_slot.end())
        return false;
    hash = it->second;
    return true;
}

GdRef TbdStore::resolve_slot(int pack, uint32_t slot, uint32_t value, int kind)
{
    uint32_t h;
    if (extn_hash(pack, slot, h)) {
        GdRef r;
        if (find_symbol(h, r) && is_type(r, kind))
            return r;
        return GdRef();
    }
    if (value && value < 0x10000000u) {
        GdRef r(pack, value);
        if (is_type(r, kind))
            return r;
    }
    return GdRef();
}

static std::vector<std::string> class_guesses(const std::string &cls)
{
    std::vector<std::string> g;
    std::string x = cls.size() > 1 && cls[0] == 'G' ? cls.substr(1) : cls;
    const char *pre[] = { "Marine", "Alien", "Predator", "Pred" };
    g.push_back(x + "_Resource");
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        size_t n = strlen(pre[i]);
        if (x.size() > n && !x.compare(0, n, pre[i])) {
            std::string rest = x.substr(n);
            g.push_back(std::string(pre[i]) + "_" + rest + "_Resource");
            g.push_back(std::string(pre[i]) + "_" + rest);
            g.push_back(std::string(pre[i]) + rest + "_Resource");
            g.push_back(rest + "_Resource");
            g.push_back(x);
        }
    }
    g.push_back(x + "_Skeleton");
    g.push_back(x);
    return g;
}

bool TbdStore::scan_class(uint32_t hash, int from, int to, GdClassRes &best, uint32_t &best_delta, uint32_t window)
{
    bool found = false;
    std::vector<uint32_t> gh;
    if (cur_class_)
        for (size_t q = 0; q < cur_guess_.size(); q++)
            gh.push_back(gd_crc(cur_guess_[q].c_str()));
    std::vector<uint32_t> hh;
    if (cur_class_)
        for (size_t q = 0; q < cur_guess_.size(); q++) {
            std::string g = cur_guess_[q];
            size_t p = g.rfind("_Resource");
            if (p != std::string::npos)
                g = g.substr(0, p);
            hh.push_back(gd_crc((g + "_Head").c_str()));
        }
    for (int pi = from; pi < to; pi++) {
        TbdPack &p = packs_[(size_t)pi];
        if (!p.template_pack || p.extn.empty())
            continue;
        std::shared_ptr<std::vector<uint8_t> > d = data(pi);
        if (!d || d->size() < 8)
            continue;
        for (size_t k = 0; k + 4 <= d->size(); k += 4) {
            if (rd32(&(*d)[k]) != hash)
                continue;
            std::vector<std::pair<uint32_t, uint32_t> >::iterator it = std::lower_bound(p.extn.begin(), p.extn.end(),
                std::make_pair((uint32_t)0, (uint32_t)(k + 0x10)), [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) { return a.second < b.second; });
            GdRef body, head;
            uint32_t delta = 0;
            for (; it != p.extn.end() && it->second < k + window + 0x40; ++it) {
                GdRef r;
                if (!find_symbol(it->first, r))
                    continue;
                bool skel = is_type(r, GD_T_SKELETON), mesh = is_type(r, GD_T_MESH);
                if (!skel && !mesh)
                    continue;
                if (!body.ok()) {
                    std::unordered_map<uint32_t, std::string>::const_iterator bn = names_.find(it->first);
                    if (bn != names_.end()) {
                        std::string lb = bn->second;
                        for (size_t q = 0; q < lb.size(); q++)
                            lb[q] = (char)tolower((unsigned char)lb[q]);
                        if (lb.find("menu") != std::string::npos || lb.find("order") != std::string::npos)
                            continue;
                    }
                    if (it->second >= k + window)
                        break;
                    body = r;
                    delta = it->second - (uint32_t)k;
                    if (std::find(gh.begin(), gh.end(), it->first) != gh.end())
                        delta = 0;
                    if (!skel)
                        break;
                    continue;
                }
                if (skel && std::find(hh.begin(), hh.end(), it->first) != hh.end()) {
                    head = r;
                    break;
                }
                if (skel) {
                    std::unordered_map<uint32_t, std::string>::const_iterator nm = names_.find(it->first);
                    if (nm != names_.end()) {
                        std::string low = nm->second;
                        for (size_t q = 0; q < low.size(); q++)
                            low[q] = (char)tolower((unsigned char)low[q]);
                        if (low.find("head") != std::string::npos && low.find("upgrade") == std::string::npos) {
                            head = r;
                            break;
                        }
                    }
                }
            }
            if (body.ok() && delta < best_delta) {
                best.body = body;
                best.head = head;
                best_delta = delta;
                found = true;
            }
        }
    }
    return found;
}

bool TbdStore::class_resources(const std::string &cls, GdClassRes &out)
{
    if (!available())
        return false;
    std::map<std::string, std::pair<bool, GdClassRes> >::iterator ci = class_cache_.find(cls);
    if (ci != class_cache_.end()) {
        out = ci->second.second;
        return ci->second.first;
    }
    uint32_t h = gd_crc(cls.c_str());
    cur_guess_ = class_guesses(cls);
    cur_class_ = true;
    GdClassRes best;
    uint32_t bd = 0xFFFFFFFFu;
    bool ok = scan_class(h, 0, (int)packs_.size(), best, bd, 0x100);
    if (!ok && !others_indexed_) {
        int from = (int)packs_.size();
        index_others();
        ok = scan_class(h, from, (int)packs_.size(), best, bd, 0x100);
    }
    if (!ok)
        ok = scan_class(h, 0, (int)packs_.size(), best, bd, 0x200);
    cur_class_ = false;
    if (!ok) {
        for (size_t q = 0; q < cur_guess_.size() && !ok; q++) {
            GdRef r = doodad(cur_guess_[q]);
            if (r.ok()) {
                best.body = r;
                ok = true;
            }
        }
    }
    class_cache_[cls] = std::make_pair(ok, best);
    out = best;
    return ok;
}

std::string TbdStore::name_of(int pack, uint32_t off) const
{
    if (pack < 0 || (size_t)pack >= packs_.size())
        return std::string();
    const std::vector<std::pair<uint32_t, uint32_t> > &p = packs_[(size_t)pack].publ;
    for (size_t i = 0; i < p.size(); i++)
        if (p[i].second == off) {
            std::unordered_map<uint32_t, std::string>::const_iterator it = names_.find(p[i].first);
            return it != names_.end() ? it->second : std::string("?");
        }
    return std::string("?");
}

bool TbdStore::ground_textures(std::vector<GdRef> &out) const
{
    out.clear();
    for (size_t pi = 0; pi < packs_.size(); pi++) {
        const TbdPack &p = packs_[pi];
        if (files_[(size_t)p.file].tag != level_)
            continue;
        bool has_map = false;
        std::vector<GdRef> tex;
        for (size_t i = 0; i < p.types.size(); i++) {
            if (p.types[i].first == t_map_)
                has_map = true;
            else if (p.types[i].first == t_tex_ && p.types[i].second < 0x10000000u)
                tex.push_back(GdRef((int)pi, p.types[i].second));
        }
        if (has_map && tex.size() >= 10) {
            out.assign(tex.begin() + 2, tex.end());
            return true;
        }
    }
    return false;
}

GdRef TbdStore::doodad(const std::string &name)
{
    GdRef r;
    if (!available())
        return r;
    if (find_symbol(gd_crc(name.c_str()), r) && (is_type(r, GD_T_MESH) || is_type(r, GD_T_SKELETON)))
        return r;
    return GdRef();
}

void TbdStore::model_names(const char *needle, std::vector<std::string> &out, bool all_levels)
{
    if (!available())
        return;
    if (all_levels)
        index_others();
    std::string nd(needle);
    for (size_t i = 0; i < nd.size(); i++)
        nd[i] = (char)tolower((unsigned char)nd[i]);
    for (std::unordered_map<uint32_t, std::string>::const_iterator it = names_.begin(); it != names_.end(); ++it) {
        std::string l = it->second;
        for (size_t i = 0; i < l.size(); i++)
            l[i] = (char)tolower((unsigned char)l[i]);
        if (l.find(nd) == std::string::npos)
            continue;
        GdRef r = doodad(it->second);
        if (r.ok() && std::find(out.begin(), out.end(), it->second) == out.end())
            out.push_back(it->second);
    }
    std::sort(out.begin(), out.end());
}

TbdStore &gd_store()
{
    static TbdStore s;
    return s;
}

static bool g_ready;
static std::string g_msg;

bool gd_init(const char *program_dir)
{
    std::string dir = program_dir && program_dir[0] ? program_dir : ".";
    g_ready = gd_store().open(dir + "/game");
    g_msg = g_ready ? "" : "Run AvPExtinctionMP first to set up the game from your disc.";
    return g_ready;
}

bool gd_available()
{
    return g_ready;
}

void gd_model_names(const char *needle, std::vector<std::string> &out)
{
    if (g_ready)
        gd_store().model_names(needle, out, false);
}

const char *gd_message()
{
    return g_ready ? NULL : "Run AvPExtinctionMP first to set up the game from your disc.";
}

void gd_set_level(const char *code)
{
    if (g_ready && code)
        gd_store().set_level(code);
}
