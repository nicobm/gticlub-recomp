// Button icons of the controller in use. The menus (Sumo Tool screens, data/sumotool/ps3/<screen>/<screen>[_lang]
// .dat + .gpu) draw the PlayStation buttons from 18x18 sprites of their texture atlases (DXT3). When the game reads
// a .gpu file, the blocks under those sprites are replaced with the icons of an Xbox or Nintendo controller (by
// position: cross = bottom, circle = right, square = left, triangle = top). The reads are remembered, so a
// controller connected later changes the icons in memory too (every texture is then read again).
//
// .dat: chunks (tag, big-endian size). PTEX: sprite count at +0x10, then from +0x18 sprites of 0x28 bytes (texture
// reference u32, hash u32, corners x0 y0 x1 y0 x1 y1 x0 y1 as floats in texels). Pointers count from the chunk's
// +0x10: a texture reference holds (CellGcmTexture pointer, offset of its data in the .gpu); CellGcmTexture: format
// u8, mips u8, dimension u8, cube u8, remap u32, width u16, height u16.
#include "recomp/settings.h"
#include "recomp/rsx_core.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

namespace gti::recomp {
bool host_button_glyph(int style, int symbol, int w, int h, uint8_t* rgba);   // host_win.cpp

namespace {
enum Symbol { kCross, kCircle, kSquare, kTriangle };
struct Patch { uint32_t off; uint8_t data[16]; uint8_t orig[16]; };
struct Sprite { int x, y, w, h; uint32_t fmt, texW, texH, gpuOff; };

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
float bef(const uint8_t* p) { const uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return f; }

std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> d;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0) { d.resize(size_t(n)); d.resize(std::fread(d.data(), 1, d.size(), f)); }
        std::fclose(f);
    }
    return d;
}

// The button-sized sprites of a .dat, on every DXT3/DXT5 texture of the file: the sprite's reference does not tell
// the texture reliably (main_menu: all four buttons are on the first texture, three sprites point at others), so
// patches_of() keeps the places that really hold a button icon.
std::vector<Sprite> button_sprites(const std::vector<uint8_t>& d) {
    std::vector<Sprite> out;
    for (size_t o = 0; o + 8 <= d.size();) {
        const uint32_t size = be32(&d[o + 4]);
        if (size < 8 || o + size > d.size()) break;
        if (!std::memcmp(&d[o], "PTEX", 4) && size >= 0x18) {
            const uint8_t* c = &d[o];
            const uint32_t nspr = be32(c + 0x10);
            struct Tex { uint32_t fmt, w, h, off; };
            std::vector<Tex> texs;
            for (uint32_t k = 0; k < nspr && 0x18 + 0x28 * (k + 1) <= size; ++k) {   // the textures the references name
                const uint32_t ref = 0x10 + be32(c + 0x18 + 0x28 * k);
                if (ref + 8 > size) continue;
                const uint32_t st = 0x10 + be32(c + ref), off = be32(c + ref + 4);
                if (st + 0x18 > size) continue;
                const Tex t{c[st] & ~0x60u, be16(c + st + 8), be16(c + st + 10), off};
                if ((t.fmt == 0x87 || t.fmt == 0x88) && std::none_of(texs.begin(), texs.end(), [&](const Tex& u) { return u.off == t.off; })) texs.push_back(t);
            }
            for (uint32_t k = 0; k < nspr && 0x18 + 0x28 * (k + 1) <= size; ++k) {
                const uint8_t* s = c + 0x18 + 0x28 * k;
                const float x0 = bef(s + 8), y0 = bef(s + 12), x1 = bef(s + 24), y1 = bef(s + 28);
                const int w = int(std::lround(x1 - x0)), h = int(std::lround(y1 - y0));
                if (w != 18 || h != 18 || x0 < 0 || y0 < 0) continue;
                for (const Tex& t : texs) {
                    if (x1 > t.w || y1 > t.h) continue;
                    const Sprite sp{int(std::lround(x0)), int(std::lround(y0)), w, h, t.fmt, t.w, t.h, t.off};
                    if (std::none_of(out.begin(), out.end(), [&](const Sprite& q) { return q.x == sp.x && q.y == sp.y && q.gpuOff == sp.gpuOff; })) out.push_back(sp);
                }
            }
        }
        o += size;
    }
    return out;
}

// a PlayStation button icon of the game: transparent corners and a dark disc round the symbol
bool looks_like_button(const std::vector<uint8_t>& rgba, int w, int h) {
    auto a = [&](int x, int y) { return rgba[(size_t(y) * w + x) * 4 + 3]; };
    if (a(0, 0) > 100 || a(w - 1, 0) > 100 || a(0, h - 1) > 100 || a(w - 1, h - 1) > 100) return false;
    int dark = 0;
    for (int i = 0; i < w * h; ++i) if (rgba[4 * i + 3] > 200 && std::max({rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2]}) < 90) ++dark;
    return dark >= w * h / 5;
}

// ---- DXT3 / DXT5 blocks (little-endian, like on a PC)
void rgb565(uint16_t c, int& r, int& g, int& b) { r = (c >> 11) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; }
void decode_block(const uint8_t* blk, uint32_t fmt, uint8_t px[16][4]) {
    if (fmt == 0x87) {
        for (int i = 0; i < 16; ++i) { const int nib = (blk[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = uint8_t(nib * 17); }
    } else {
        const int a0 = blk[0], a1 = blk[1];
        int pal[8] = {a0, a1};
        if (a0 > a1) for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else { for (int k = 1; k < 5; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5; pal[6] = 0; pal[7] = 255; }
        uint64_t bits = 0; for (int k = 0; k < 6; ++k) bits |= uint64_t(blk[2 + k]) << (8 * k);
        for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(pal[(bits >> (3 * i)) & 7]);
    }
    const uint16_t c0 = uint16_t(blk[8] | blk[9] << 8), c1 = uint16_t(blk[10] | blk[11] << 8);
    int r0, g0, b0, r1, g1, b1; rgb565(c0, r0, g0, b0); rgb565(c1, r1, g1, b1);
    const int pal[4][3] = {{r0, g0, b0}, {r1, g1, b1}, {(2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3}, {(r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3}};
    const uint32_t idx = uint32_t(blk[12] | blk[13] << 8 | blk[14] << 16 | uint32_t(blk[15]) << 24);
    for (int i = 0; i < 16; ++i) { const int k = (idx >> (2 * i)) & 3; px[i][0] = uint8_t(pal[k][0]); px[i][1] = uint8_t(pal[k][1]); px[i][2] = uint8_t(pal[k][2]); }
}
uint16_t to565(int r, int g, int b) { return uint16_t((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255); }
void encode_block(const uint8_t px[16][4], uint32_t fmt, uint8_t* blk) {
    std::memset(blk, 0, 16);
    if (fmt == 0x87) {
        for (int i = 0; i < 16; ++i) blk[i / 2] |= uint8_t(((px[i][3] + 8) / 17) << (4 * (i & 1)));
    } else {   // DXT5: 8-step ramp between the extremes
        int lo = 255, hi = 0;
        for (int i = 0; i < 16; ++i) { lo = std::min<int>(lo, px[i][3]); hi = std::max<int>(hi, px[i][3]); }
        blk[0] = uint8_t(hi); blk[1] = uint8_t(lo);
        uint64_t bits = 0;
        for (int i = 0; i < 16; ++i) {
            int k = 0;
            if (hi > lo) { const int t = (hi - px[i][3]) * 7 / (hi - lo); k = t == 0 ? 0 : t == 7 ? 1 : t + 1; }
            bits |= uint64_t(k) << (3 * i);
        }
        for (int k = 0; k < 6; ++k) blk[2 + k] = uint8_t(bits >> (8 * k));
    }
    // colour: the two most distant visible texels as the ends (16 texels: every pair)
    int a = -1, b = -1, best = -1;
    for (int i = 0; i < 16; ++i) {
        if (px[i][3] < 8) continue;
        if (a < 0) { a = b = i; best = 0; }
        for (int j = i + 1; j < 16; ++j) {
            if (px[j][3] < 8) continue;
            const int dr = px[i][0] - px[j][0], dg = px[i][1] - px[j][1], db = px[i][2] - px[j][2];
            const int d2 = dr * dr + dg * dg + db * db;
            if (d2 > best) { best = d2; a = i; b = j; }
        }
    }
    if (a < 0) return;   // all transparent: any colour
    uint16_t c0 = to565(px[a][0], px[a][1], px[a][2]), c1 = to565(px[b][0], px[b][1], px[b][2]);
    if (c0 < c1) std::swap(c0, c1);
    int r0, g0, b0, r1, g1, b1; rgb565(c0, r0, g0, b0); rgb565(c1, r1, g1, b1);
    const int pal[4][3] = {{r0, g0, b0}, {r1, g1, b1}, {(2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3}, {(r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3}};
    uint32_t idx = 0;
    if (c0 != c1)
        for (int i = 0; i < 16; ++i) {
            int bk = 0, bd = 1 << 30;
            for (int k = 0; k < 4; ++k) { const int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2], d2 = dr * dr + dg * dg + db * db; if (d2 < bd) { bd = d2; bk = k; } }
            idx |= uint32_t(bk) << (2 * i);
        }
    blk[8] = uint8_t(c0); blk[9] = uint8_t(c0 >> 8); blk[10] = uint8_t(c1); blk[11] = uint8_t(c1 >> 8);
    blk[12] = uint8_t(idx); blk[13] = uint8_t(idx >> 8); blk[14] = uint8_t(idx >> 16); blk[15] = uint8_t(idx >> 24);
}

// which PlayStation button an icon shows, from the colour of its symbol
int classify(const std::vector<uint8_t>& rgba, int n) {
    int cnt[4] = {};
    for (int i = 0; i < n; ++i) {
        const int r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2], a = rgba[4 * i + 3];
        if (a < 128) continue;
        const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        if (mx < 90 || mx - mn < 40) continue;
        if (b == mx && b > r + 60) ++cnt[kCross];
        else if (r == mx && r > g + 80 && r > b + 80) ++cnt[kCircle];
        else if (g == mx && g > r + 30 && g > b + 30) ++cnt[kTriangle];
        else if (r > g + 25 && b > g + 25) ++cnt[kSquare];
    }
    int best = 0;
    for (int k = 1; k < 4; ++k) if (cnt[k] > cnt[best]) best = k;
    return cnt[best] >= 6 ? best : -1;
}

std::mutex g_mx;
struct FileInfo { bool parsed = false; std::vector<Sprite> sprites; std::map<int, std::vector<Patch>> patches; };
std::map<std::string, FileInfo> g_files;   // by host path of the .gpu
struct Read { std::string path; uint64_t off; uint8_t* dst; size_t n; };
std::vector<Read> g_reads;
int g_applied = 1;   // the style the remembered reads hold

const std::vector<Patch>& patches_of(const std::string& gpuPath, int style) {
    FileInfo& fi = g_files[gpuPath];
    if (!fi.parsed) {
        fi.parsed = true;
        fi.sprites = button_sprites(slurp(gpuPath.substr(0, gpuPath.size() - 4) + ".dat"));
    }
    auto it = fi.patches.find(style);
    if (it != fi.patches.end()) return it->second;
    std::vector<Patch>& out = fi.patches[style];
    if (style == 1 || fi.sprites.empty()) return out;
    const std::vector<uint8_t> gpu = slurp(gpuPath);
    int done = 0;
    for (const Sprite& s : fi.sprites) {
        const int bx0 = s.x / 4, by0 = s.y / 4, bx1 = (s.x + s.w + 3) / 4, by1 = (s.y + s.h + 3) / 4, bw = bx1 - bx0, bh = by1 - by0;
        const uint32_t rowBlocks = (s.texW + 3) / 4;
        auto blockOff = [&](int bx, int by) { return s.gpuOff + (uint32_t(by) * rowBlocks + uint32_t(bx)) * 16; };
        if (blockOff(bx1 - 1, by1 - 1) + 16 > gpu.size()) continue;
        // the blocks under the sprite, decoded
        std::vector<uint8_t> img(size_t(bw * 4) * (bh * 4) * 4);
        const int W = bw * 4;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t px[16][4];
                decode_block(&gpu[blockOff(bx0 + bx, by0 + by)], s.fmt, px);
                for (int i = 0; i < 16; ++i) std::memcpy(&img[(size_t((by * 4 + i / 4) * W + bx * 4 + i % 4)) * 4], px[i], 4);
            }
        std::vector<uint8_t> icon(size_t(s.w) * s.h * 4);
        const int ox = s.x - bx0 * 4, oy = s.y - by0 * 4;
        for (int y = 0; y < s.h; ++y) std::memcpy(&icon[size_t(y * s.w) * 4], &img[(size_t((oy + y) * W + ox)) * 4], size_t(s.w) * 4);
        if (!looks_like_button(icon, s.w, s.h)) continue;
        const int sym = classify(icon, s.w * s.h);
        if (sym < 0) continue;
        if (!host_button_glyph(style, sym, s.w, s.h, icon.data())) continue;
        for (int y = 0; y < s.h; ++y) std::memcpy(&img[(size_t((oy + y) * W + ox)) * 4], &icon[size_t(y * s.w) * 4], size_t(s.w) * 4);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                uint8_t px[16][4];
                for (int i = 0; i < 16; ++i) std::memcpy(px[i], &img[(size_t((by * 4 + i / 4) * W + bx * 4 + i % 4)) * 4], 4);
                Patch p; p.off = blockOff(bx0 + bx, by0 + by);
                encode_block(px, s.fmt, p.data);
                std::memcpy(p.orig, &gpu[p.off], 16);
                out.push_back(p);
            }
        ++done;
    }
    std::fprintf(stderr, "[glyphs] %s: %d of %zu button icons drawn for style %d\n", gpuPath.substr(gpuPath.find_last_of("/\\") + 1).c_str(), done, fi.sprites.size(), style);
    return out;
}

// what the reads [off, off+n) of `path` into dst must hold for `style` (from what they hold for `from`)
// (a block is written only where it still holds what was there: memory the game reused is left alone)
void apply(const std::string& path, uint64_t off, uint8_t* dst, size_t n, int style, int from) {
    if (style == from) return;
    auto put = [&](uint32_t o, const uint8_t* expect, const uint8_t* data) {
        if (o < off || o + 16 > off + n) return;
        uint8_t* p = dst + (o - off);
        if (std::memcmp(p, expect, 16) == 0) std::memcpy(p, data, 16);
    };
    if (from != 1) for (const Patch& p : patches_of(path, from)) put(p.off, p.data, p.orig);   // back to the original
    if (style != 1) for (const Patch& p : patches_of(path, style)) put(p.off, p.orig, p.data);
}

bool is_screen_gpu(const std::string& p) {
    if (p.size() < 5) return false;
    std::string l = p; for (char& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch))), ch = ch == '\\' ? '/' : ch;
    return l.compare(l.size() - 4, 4, ".gpu") == 0 && l.find("/sumotool/") != std::string::npos;
}
}

// 1 PlayStation, 2 Xbox, 3 Nintendo: the option, or the controller connected (no controller: PlayStation)
int button_style() {
    const int o = settings().buttons;
    if (o >= 1 && o <= 3) return o;
    const int k = rsx::pad_kind();
    return k == 0 ? 1 : k;
}

void glyphs_patch_read(const std::string& hostPath, uint64_t fileOff, uint8_t* dst, size_t n) {
    if (!n || !is_screen_gpu(hostPath)) return;
    std::lock_guard<std::mutex> lk(g_mx);
    const int style = button_style();
    if (style != g_applied) {   // the controller changed since the last screen: the remembered ones first
        for (const Read& r : g_reads) apply(r.path, r.off, r.dst, r.n, style, g_applied);
        g_applied = style;
        rsx::textures_rewritten();
    }
    // a screen read again over memory a remembered read used: forget that one
    g_reads.erase(std::remove_if(g_reads.begin(), g_reads.end(), [&](const Read& r) { return r.dst < dst + n && dst < r.dst + r.n; }), g_reads.end());
    g_reads.push_back({hostPath, fileOff, dst, n});
    (void)patches_of(hostPath, 1);   // its icons are known from now on (a controller connected later)
    if (style != 1) apply(hostPath, fileOff, dst, n, style, 1);
}

// called once per image (RSX thread): the controller or the option changed
void glyphs_update() {
    {   // GLYPHTEST=seconds:option (debug): sets the menu option at that time, as if chosen in the menu
        static double at = -1; static int opt = 0; static bool parsed = false;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("GLYPHTEST")) std::sscanf(e, "%lf:%d", &at, &opt); }
        static const auto t0 = std::chrono::steady_clock::now();
        if (at >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= at) { settings().buttons = opt; at = -1; }
    }
    static int last = 0;
    const int style = button_style();
    if (style == last) return;
    last = style;
    std::lock_guard<std::mutex> lk(g_mx);
    if (style == g_applied) return;
    for (const Read& r : g_reads) apply(r.path, r.off, r.dst, r.n, style, g_applied);
    // The game copies the atlases to RSX local memory after reading them: they are found there by content. A row of
    // an icon's blocks (contiguous in the texture: 5 blocks, 80 bytes) as it is now is distinctive enough; single
    // blocks are not (37000 matches for 100 blocks). One pass over local memory, 16 bytes at a time.
    const uint64_t t0 = SDL_GetTicks();
    struct Run { std::vector<uint8_t> now, then; };
    std::vector<Run> runs;
    for (auto& [path, fi] : g_files) {
        if (!fi.parsed || fi.sprites.empty()) continue;
        const std::vector<Patch>* from = g_applied != 1 ? &patches_of(path, g_applied) : nullptr;
        const std::vector<Patch>* to = style != 1 ? &patches_of(path, style) : nullptr;
        const std::vector<Patch>& ref = from ? *from : *to;   // same blocks in the same order for every style
        if (from && to && from->size() != to->size()) continue;
        for (size_t i = 0; i < ref.size();) {
            size_t j = i + 1;
            while (j < ref.size() && ref[j].off == ref[j - 1].off + 16) ++j;
            Run r;
            for (size_t k = i; k < j; ++k) {
                const uint8_t* n = from ? (*from)[k].data : ref[k].orig;
                const uint8_t* t = to ? (*to)[k].data : ref[k].orig;
                r.now.insert(r.now.end(), n, n + 16); r.then.insert(r.then.end(), t, t + 16);
            }
            if (j - i >= 4 && r.now != r.then) runs.push_back(std::move(r));
            i = j;
        }
    }
    std::unordered_map<uint64_t, std::vector<size_t>> first;   // first 8 bytes of a run -> runs
    for (size_t i = 0; i < runs.size(); ++i) { uint64_t k; std::memcpy(&k, runs[i].now.data(), 8); first[k].push_back(i); }
    size_t found = 0;
    if (!first.empty() && rsx::C.mem) {
        uint8_t* base = rsx::C.mem + rsx::C.localAddr;
        const size_t size = 0x0F900000;
        for (size_t o = 0; o + 16 <= size; o += 16) {
            uint64_t k; std::memcpy(&k, base + o, 8);
            auto it = first.find(k);
            if (it == first.end()) continue;
            for (size_t i : it->second) {
                const Run& r = runs[i];
                if (o + r.now.size() <= size && !std::memcmp(base + o, r.now.data(), r.now.size())) { std::memcpy(base + o, r.then.data(), r.then.size()); ++found; break; }
            }
        }
    }
    const size_t nBlocks = runs.size();
    g_applied = style;
    rsx::textures_rewritten();
    std::fprintf(stderr, "[glyphs] button icons now %s (%zu places for %zu icon rows found in RSX memory, %llu ms)\n", style == 2 ? "Xbox" : style == 3 ? "Nintendo" : "PlayStation", found, nBlocks, (unsigned long long)(SDL_GetTicks() - t0));
}
} // namespace gti::recomp
