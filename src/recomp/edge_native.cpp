// Native replacements of Edge Geometry routines: see edge_native.h. Verified against captured jobs with
// FNCAP + build-test/fn_replay.exe (NATIVE=1), which compares the whole local store outside the routine's stack frame.
#include "recomp/edge_native.h"
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <x86intrin.h>

namespace gti::recomp {

uint64_t g_edgeNativeCalls[4], g_edgeNativeFallbacks[4];   // per routine (0: 0x7140, 1: 0x12780)
thread_local uint32_t g_edgeNativeDead[2];   // local store range the last native call left unlike the SPU code (dead bytes)

namespace {
inline uint32_t be32(const uint8_t* ls, uint32_t a) { const uint8_t* p = ls + (a & 0x3ffff); return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t* ls, uint32_t a) { const uint8_t* p = ls + (a & 0x3ffff); return uint16_t(p[0] << 8 | p[1]); }
inline void wr32(uint8_t* ls, uint32_t a, uint32_t v) { uint8_t* p = ls + (a & 0x3ffff); p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// LS 0x7140 (called from 0x4524, after the culling): writes the vertex positions for the RSX after the index list.
// Output format 0 (byte 4 of the context) is the only one this game uses: x, y, z float of each vertex packed to
// 12 bytes, from the stream whose id byte in the table at ctx+0x160 is 1 (stream pointers at ctx+0x10..).
// The SPU code works on vertex pairs (count rounded up to even); behind the data its last store leaves 8 zero bytes
// and the y, z of the last vertex. Context: +0x54 = start of the data, +0x58 = end (the routine's r3 = start).
bool out_positions(uint8_t* ls, uint32_t ctxp, uint32_t* ret) {
    const uint32_t ctx = ctxp & 0x3fff0;
    if (ctx + 0x170 > 0x40000) return false;
    if (ls[ctx] & 0x10) return false;          // flag the SPU code checks first: nothing to write
    if (ls[ctx + 4] != 0) return false;        // other output formats
    const uint32_t nv = be16(ls, ctx + 8);
    if (nv == 0 || (nv & 1)) return false;
    int k = -1;
    for (int i = 0; i < 16; ++i) if (ls[ctx + 0x160 + i] == 1) { k = i; break; }
    if (k < 0) return false;
    const uint32_t src = be32(ls, ctx + 0x10 + 4 * uint32_t(k)), dst = be32(ls, ctx + 0x58);
    if ((src | dst) & 15 || src >= 0x40000 || dst >= 0x40000) return false;
    const uint32_t end = dst + 12 * nv;
    if (uint64_t(src) + 16ull * nv > 0x40000 || end + 16 > 0x40000) return false;
    if (dst > src && dst < src + 16 * nv) return false;   // the packed data would overrun unread input
    if (src > dst && src < end + 16) {
        // input behind the output: a forward copy is safe as long as every vertex is read before it is overwritten
        // (12-byte writes, 16-byte reads: the write position never passes the read position when dst <= src)
    }
    uint8_t lastYZ[8];
    std::memcpy(lastYZ, ls + src + 16 * (nv - 1) + 4, 8);
    uint8_t* o = ls + dst;
    const uint8_t* in = ls + src;
    // 16 bytes per store, 12 kept: the extra 4 are overwritten by the next vertex (and by the tail after the last);
    // with dst below src (or apart) no store reaches a vertex not read yet
    for (uint32_t v = 0; v < nv; ++v) _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 12 * v), _mm_load_si128(reinterpret_cast<const __m128i*>(in + 16 * v)));
    std::memset(ls + end, 0, 8);
    std::memcpy(ls + end + 8, lastYZ, 8);
    wr32(ls, ctx + 0x54, dst);
    wr32(ls, ctx + 0x58, end);
    *ret = dst;
    return true;
}

// LS 0x12780 (called from 0x434c, before the culling): r3 = context, r4 = compressed index block (16-byte aligned).
// Decompresses Edge's triangle list in place (the list starts where the block was) and sets ctx+0x50 = list,
// ctx+0x58 = end of what the SPU code wrote. Format (reverse engineered for tools/ep3.py):
//   header  u16 numDeltas, u16 bias, u16 flagBytes, u8 deltaBits, u8 pad
//   flagBytes of flags, one bit per index a triangle needs: 1 = next explicit value, 0 = next sequential vertex
//   2 bits per triangle: 3 = three new indexes, 0/1/2 = shares an edge of the previous triangle + one new index
//   byte aligned: numDeltas values of deltaBits bits: explicit[i] = value - bias + explicit[i - 8]
// The SPU code decodes 8 triangles (48 bytes) at a time: what it leaves between the real end of the list and the
// end of the last group, and in the free space above (its work area), is not reproduced: nothing reads it (the
// culling compacts the list and resets the end, the output routine writes over it). g_edgeNativeDead tells the
// test tool where that is.
struct IdxWork { uint8_t in[0x12000]; uint16_t ex[0x10010]; uint16_t val[0x10010]; uint8_t out[0x20010]; };
bool dec_indexes(uint8_t* ls, uint32_t ctxp, uint32_t src, uint32_t* ret) {
    const uint32_t ctx = ctxp & 0x3fff0;
    if (ctx + 0x60 > 0x40000 || (src & 15) || src >= 0x40000) return false;
    const uint32_t ni = be16(ls, ctx + 10);
    if (ni == 0 || ni % 3) return false;
    if (((ls[ctx + 6] >> 4) & 15) != 3) return false;   // index flavour: the only one seen (2 also reaches this code)
    const uint32_t A = be16(ls, src), B = be16(ls, src + 2), C = be16(ls, src + 4), D = ls[src + 6];
    if (D > 16) return false;
    const uint32_t T = ni / 3;
    const uint32_t codesOff = 8 + C, deltaOff = codesOff + (2 * T + 7) / 8, total = deltaOff + (A * D + 7) / 8;
    if (total + 8 > sizeof(IdxWork::in) || src + total > 0x40000 || src + 2 * ni + 48 > 0x40000) return false;
    static thread_local IdxWork* work = nullptr;
    if (!work) work = new IdxWork;
    IdxWork& W = *work;
    std::memcpy(W.in, ls + src, total);
    std::memset(W.in + total, 0, 8);
    // explicit values
    {
        const uint8_t* dp = W.in + deltaOff;
        const uint32_t mask = (1u << D) - 1;
        uint32_t bit = 0;
        for (uint32_t i = 0; i < A; ++i, bit += D) {
            const uint8_t* p = dp + (bit >> 3);
            const uint32_t w = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
            const uint32_t v = (w >> (32 - D - (bit & 7))) & mask;
            W.ex[i] = uint16_t(v - B + (i >= 8 ? W.ex[i - 8] : 0));
        }
    }
    const uint8_t* flags = W.in + 8;
    const uint8_t* codes = W.in + codesOff;
    // Two passes, neither with data-dependent branches or counters chained through memory (flags and codes are close
    // to random; one pass with branches was as slow as the SPU code, which does 8 triangles at a time).
    // Pass 1: the value each flag bit stands for (already byte-swapped): explicit values and sequential vertices
    // are consumed in flag order whatever triangle asks for them.
    const uint32_t nf = std::min(8 * C, 3 * T);
    {
        uint32_t e = 0, n = 0;
        for (uint32_t k = 0; k < nf; ++k) {
            const uint32_t f = (flags[k >> 3] >> (7 - (k & 7))) & 1;
            const uint32_t v = f ? uint32_t(W.ex[e]) : n;
            W.val[k] = __builtin_bswap16(uint16_t(v));
            e += f; n += 1 - f;
        }
    }
    // Pass 2: triangles. Previous (a, b, c): code 0 -> (a, c, new), 1 -> (c, b, new), 2 -> (b, a, new), 3 -> three new.
    uint8_t* o = W.out;
    uint32_t fi = 0, a = 0, b = 0, c = 0;
    for (uint32_t t = 0; t < T; ++t) {
        const uint32_t code = (codes[t >> 2] >> (6 - 2 * (t & 3))) & 3;
        const bool fresh = code == 3 || t == 0;
        const uint32_t v1 = W.val[fi], v2 = W.val[fi + 1], v3 = W.val[fi + 2];
        const uint32_t q = code == 0 ? a : (code == 1 ? c : b), p = code == 0 ? c : (code == 1 ? b : a);
        a = fresh ? v1 : q; b = fresh ? v2 : p; c = fresh ? v3 : v1;
        fi += fresh ? 3 : 1;
        const uint64_t be = uint64_t(a) | uint64_t(b) << 16 | uint64_t(c) << 32;
        std::memcpy(o, &be, 8);   // 6 bytes + 2 the next triangle overwrites (the buffer has room)
        o += 6;
    }
    // a stream that does not follow the format (more indexes than flags, or than explicit values): the SPU code deals with it
    if (fi > nf) return false;
    {
        uint32_t e = 0;
        for (uint32_t k = 0; k < fi; ++k) e += (flags[k >> 3] >> (7 - (k & 7))) & 1;
        if (e > A) return false;
    }
    std::memcpy(ls + src, W.out, 2 * ni);
    const uint32_t end = src + 48 * ((T + 7) / 8);
    wr32(ls, ctx + 0x50, src);
    wr32(ls, ctx + 0x58, end);
    g_edgeNativeDead[0] = src + 2 * ni; g_edgeNativeDead[1] = be32(ls, ctx + 0x5c);
    *ret = ctxp;
    return true;
}

// LS 0x12628 (called from 0x42c4, first stage of the job): r3 = context, r4 = primary vertex stream, r6 = secondary
// stream (0 = none), r5 / r7 = their fixed point data. Calls the generic stream decompressor (0xf5e0) for each.
// The only layout this game's jobs use (checked field by field, anything else goes to the SPU code): one stream
// holding the position as three floats, 12 bytes per vertex, unpacked to float4 with w = 1 at the buffer the
// context names at +0x10. It leaves: +0x54 and +0x58 = the stream (its space is free again), +0x5c = the unpacked
// positions, +0x15c = 1 attribute, table at +0x160: slot 0 = attribute id 1 (position).
bool dec_vertexes(uint8_t* ls, const uint32_t* a, uint32_t* ret) {
    const uint32_t ctx = a[0] & 0x3fff0, src = a[1];
    if (ctx + 0x170 > 0x40000 || a[3] != 0 || a[4] != 0) return false;
    if (ls[ctx + 2] != 0 || (ls[ctx] & 16)) return false;                 // stream format: default; no extra output stage
    if (be32(ls, ctx + 0x6c) != 0 || be32(ls, ctx + 0x7c) != 0) return false;   // custom stream descriptions
    if (be32(ls, ctx + 0x150) != 5 || be32(ls, ctx + 0x154) != 5 || be32(ls, ctx + 0x15c) != 0 || be32(ls, ctx + 0x5c) != 0) return false;
    for (int i = 0; i < 16; ++i) if (ls[ctx + 0x160 + i]) return false;
    const uint32_t nv = be16(ls, ctx + 8), dst = be32(ls, ctx + 0x10);
    if (nv == 0 || (nv & 7) || (src & 15) || (dst & 15) || src >= 0x40000 || dst >= 0x40000) return false;
    if (src + 12 * nv + 4 > 0x40000 || dst + 16 * nv > 0x40000) return false;
    if (!(src + 12 * nv + 4 <= dst || dst + 16 * nv <= src)) return false;   // overlapping buffers: order matters
    const uint8_t* in = ls + src;
    uint8_t* o = ls + dst;
    const __m128i w1 = _mm_set_epi32(0x0000803f, 0, 0, 0);   // bytes 12..15 = 3f 80 00 00 (1.0f big-endian)
    for (uint32_t v = 0; v < nv; ++v) {
        _mm_store_si128(reinterpret_cast<__m128i*>(o + 16 * v), _mm_blend_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 12 * v)), w1, 0xC0));
    }
    wr32(ls, ctx + 0x54, src);
    wr32(ls, ctx + 0x58, src);
    wr32(ls, ctx + 0x5c, dst);
    wr32(ls, ctx + 0x15c, 1);
    ls[ctx + 0x160] = 1;
    *ret = a[0];
    return true;
}
}  // namespace

bool edge_native_run(uint32_t entry, uint8_t* ls, const uint32_t* a, uint32_t r1, uint32_t* ret) {
    (void)r1;
    static const bool off = std::getenv("EDGENATIVE2") && std::atoi(std::getenv("EDGENATIVE2")) == 0;
    if (off) return false;
    g_edgeNativeDead[0] = g_edgeNativeDead[1] = 0;
    switch (entry) {
        case 0x7140: {
            const bool ok = out_positions(ls, a[0], ret);
            ++(ok ? g_edgeNativeCalls[0] : g_edgeNativeFallbacks[0]);
            return ok;
        }
        case 0x12780: {
            const bool ok = dec_indexes(ls, a[0], a[1], ret);
            ++(ok ? g_edgeNativeCalls[1] : g_edgeNativeFallbacks[1]);
            return ok;
        }
        case 0x12628: {
            const bool ok = dec_vertexes(ls, a, ret);
            ++(ok ? g_edgeNativeCalls[2] : g_edgeNativeFallbacks[2]);
            return ok;
        }
        default: return false;
    }
}

} // namespace gti::recomp
