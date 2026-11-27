// Native replacement of Edge Geometry's triangle culling routine (SPU program at EA 0xbaea80, LS 0x9e68, called
// from 0x43fc, returns to 0x4400). Rules measured on captured jobs (CULLCAP, tools: cull_replay):
//   per vertex  c = M * L * p (input 15: 4x4 view-projection, input 16: 3x4 local-to-world)
//               outcode bits: 0x04 z > w, 0x80 z < 0, and for w > 0 with screen = c.xy / w * scale + offset:
//               0x08 y > height, 0x10 x > width, 0x20 y < 0, 0x40 x < 0
//               snapped screen coordinates: trunc(8 v) & ~1, 16 bits
//   triangle    all three share an outcode bit -> culled; any vertex with z < 0 -> kept;
//               mode 3 (r4): float area < 0 -> culled (back face); three equal snapped x or three equal snapped y ->
//               culled; otherwise kept.
// Our arithmetic is not the SPU's (reciprocal estimate, float32 order), so every test that is too close to call
// keeps the triangle: the result is a superset of the console's (a few more slivers drawn, never a missing one).
#include "recomp/edge_cull.h"
#include <cmath>
#include <x86intrin.h>
#include <cstring>
#include <cstdlib>

namespace gti::recomp {

uint64_t g_edgeCullCyc[4];   // profiling: load, vertices, triangles
uint8_t* g_edgeCullWhy;      // debug (cull_replay): why each kept triangle was kept
uint64_t g_edgeCullBox;      // jobs culled whole by the bounding box test

namespace {
inline uint32_t be32(const uint8_t* ls, uint32_t a) { const uint8_t* p = ls + (a & 0x3ffff); return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t* ls, uint32_t a) { const uint8_t* p = ls + (a & 0x3ffff); return uint16_t(p[0] << 8 | p[1]); }
inline float bef(const uint8_t* ls, uint32_t a) { const uint32_t u = be32(ls, a); float f; std::memcpy(&f, &u, 4); return f; }
inline void wr32(uint8_t* ls, uint32_t a, uint32_t v) { uint8_t* p = ls + (a & 0x3ffff); p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// Whole-job shortcut: true when every vertex is certainly outside one and the same clip plane, so that the
// per-vertex pass would give all of them that outcode bit and every triangle is culled (as on the console: the
// per-vertex outcodes only claim what is certain). Half of the vertices in a race are in jobs like that.
// The box of the local positions (w must be 1) is tested at its 8 corners: each condition is linear in the
// position, so holding at the corners means holding inside. Margins: twice the per-vertex error bounds, which
// are themselves bounded over the box.
bool box_outside(const uint8_t* ls, uint32_t pos, uint32_t nv, const double T[4][4], const double TA[4][4],
                 double scX, double scY, double ofX, double ofY, double fW, double fH, double mbOut[4]) {
    const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i bs2 = _mm256_broadcastsi128_si256(bs);
    const uint8_t* src = ls + pos;
    // nan: 0 * q summed, NaN or infinity anywhere makes it NaN (min/max alone would drop a NaN lane)
    __m256 mn = _mm256_set1_ps(3.0e38f), mx = _mm256_set1_ps(-3.0e38f), nan = _mm256_setzero_ps();
    const __m256 zero = _mm256_setzero_ps();
    uint32_t v = 0;
    for (; v + 2 <= nv; v += 2) {
        const __m256 q = _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16 * v)), bs2));
        mn = _mm256_min_ps(mn, q); mx = _mm256_max_ps(mx, q); nan = _mm256_add_ps(nan, _mm256_mul_ps(zero, q));
    }
    {
        float a[8]; _mm256_storeu_ps(a, nan);
        for (float x : a) if (x != 0.0f) return false;
    }
    __m128 mn4 = _mm_min_ps(_mm256_castps256_ps128(mn), _mm256_extractf128_ps(mn, 1));
    __m128 mx4 = _mm_max_ps(_mm256_castps256_ps128(mx), _mm256_extractf128_ps(mx, 1));
    if (v < nv) {
        const __m128 q = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16 * v)), bs));
        mn4 = _mm_min_ps(mn4, q); mx4 = _mm_max_ps(mx4, q);
    }
    float lo[4], hi[4];
    _mm_storeu_ps(lo, mn4); _mm_storeu_ps(hi, mx4);
    if (lo[3] != 1.0f || hi[3] != 1.0f) return false;
    for (int k = 0; k < 3; ++k) if (!(lo[k] <= hi[k]) || !(std::fabs(lo[k]) < 1e30f) || !(std::fabs(hi[k]) < 1e30f)) return false;
    double A[4] = {std::fmax(std::fabs(lo[0]), std::fabs(hi[0])), std::fmax(std::fabs(lo[1]), std::fabs(hi[1])), std::fmax(std::fabs(lo[2]), std::fabs(hi[2])), 1.0};
    double mb[4];
    for (int i = 0; i < 4; ++i) mb[i] = ((TA[i][0] * A[0] + TA[i][1] * A[1]) + (TA[i][2] * A[2] + TA[i][3])) * 3e-7 * 1.02 + 1e-30;
    for (int i = 0; i < 4; ++i) mbOut[i] = mb[i];   // also the error bound of every vertex of the job (see the vertex pass)
    double c[8][4];
    for (int k = 0; k < 8; ++k) {
        const double p[4] = {(k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2], 1.0};
        for (int i = 0; i < 4; ++i) c[k][i] = T[i][0] * p[0] + T[i][1] * p[1] + T[i][2] * p[2] + T[i][3];
    }
    bool far = true, near = true;
    double c3min = 1e300, c0max = 0, c1max = 0;
    for (int k = 0; k < 8; ++k) {
        far = far && c[k][2] - c[k][3] > 2 * (mb[2] + mb[3]);
        near = near && c[k][2] < -2 * mb[2];
        c3min = std::fmin(c3min, c[k][3]);
        c0max = std::fmax(c0max, std::fabs(c[k][0])); c1max = std::fmax(c1max, std::fabs(c[k][1]));
    }
    if (far || near) return true;
    if (!(c3min > 8 * mb[3] + 1e-19)) return false;   // some vertex may lack a screen position
    // bound of the per-vertex screen error e (edge_cull_native), then twice that as margin, in pixels
    const double rw = mb[3] / c3min, X = c0max / c3min * std::fabs(scX) + std::fabs(ofX), Y = c1max / c3min * std::fabs(scY) + std::fabs(ofY);
    const double eb = (mb[0] / c3min + c0max / c3min * rw) * std::fabs(scX) + (mb[1] / c3min + c1max / c3min * rw) * std::fabs(scY) + (X + Y + 1.0) * 6e-7;
    const double em = 2 * eb + 1e-3;
    bool r = true, l = true, b = true, t = true;   // x > width, x < 0, y > height, y < 0
    for (int k = 0; k < 8; ++k) {
        const double sx = c[k][0] * scX + ofX * c[k][3], sy = c[k][1] * scY + ofY * c[k][3];   // screen position times w
        r = r && sx - (fW + em) * c[k][3] > 0;
        l = l && -sx - em * c[k][3] > 0;
        b = b && sy - (fH + em) * c[k][3] > 0;
        t = t && -sy - em * c[k][3] > 0;
    }
    return r || l || b || t;
}

}  // namespace

EdgeCullResult edge_cull_native(uint8_t* ls, uint32_t ctx, uint32_t mode, uint32_t entryR1) {
    EdgeCullResult res{};
    if (mode != 1 && mode != 3) return res;
    ctx &= 0x3fff0;
    const uint32_t w2 = be32(ls, ctx + 8);
    const uint32_t nv = w2 >> 16, ni = w2 & 0xffff;
    const uint32_t pos = be32(ls, ctx + 0x10), idx = be32(ls, ctx + 0x50);
    if (ni % 3 || nv == 0 || nv > 0x2000 || pos + nv * 16 > 0x40000 || idx + ni * 2 > 0x40000) return res;
    // job inputs: gathered after the binary in DMA list order, 16-byte aligned, keeping the EA's low 4 bits
    uint32_t inLs[18] = {}, inSz[18] = {};
    {
        const uint32_t n = be16(ls, 0x2d0a) / 8;
        if (n < 18) return res;
        uint32_t off = be32(ls, 0x2c00);
        for (uint32_t k = 0; k < 18; ++k) {
            const uint32_t sz = be32(ls, 0x2d30 + 8 * k) & 0x7fff, ea = be32(ls, 0x2d34 + 8 * k);
            inLs[k] = off | (ea & 15); inSz[k] = sz;
            off += (sz + 15) & ~15u;
        }
    }
    if (inSz[15] != 0x80 || inSz[16] != 0x30) return res;
    const uint32_t vp = inLs[15], lw = inLs[16];
    const unsigned sx0 = be16(ls, vp), sy0 = be16(ls, vp + 2), sw = be16(ls, vp + 4), sh = be16(ls, vp + 6);
    const double scX = bef(ls, vp + 0x50), scY = bef(ls, vp + 0x54), ofX = bef(ls, vp + 0x60), ofY = bef(ls, vp + 0x64);
    // only the viewport layout seen in this game (full-target scissor, scale/offset mapping clip space onto it)
    if (sx0 || sy0 || !sw || !sh || scX != sw * 0.5 || scY != -(sh * 0.5) || ofX != sw * 0.5 || ofY != sh * 0.5) return res;
    double M[4][4], L[3][4], T[4][4], TA[4][4];   // TA: |M| |L|, magnitude of the products the SPU rounds
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) M[i][k] = bef(ls, vp + 0x10 + 16 * i + 4 * k);
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 4; ++k) L[i][k] = bef(ls, lw + 16 * i + 4 * k);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) {
            double v = 0, va = 0;
            for (int j = 0; j < 3; ++j) { v += M[i][j] * L[j][k]; va += std::fabs(M[i][j]) * std::fabs(L[j][k]); }
            if (k == 3) { v += M[i][3]; va += std::fabs(M[i][3]); }
            T[i][k] = v; TA[i][k] = va;
        }
    static const bool boxOn = !std::getenv("EDGEBOX") || std::atoi(std::getenv("EDGEBOX")) != 0;
    double mbBox[4] = {-1, -1, -1, -1};   // set when the box of the positions is usable
    if (boxOn && box_outside(ls, pos, nv, T, TA, scX, scY, ofX, ofY, double(sw), double(sh), mbBox)) {
        uint32_t bad = 0;   // like the full path: an index past the vertices means data we do not understand
        for (uint32_t t = 0; t < ni; ++t) bad |= uint32_t(be16(ls, idx + 2 * t) >= nv);
        if (!bad) {
            ++g_edgeCullBox;
            wr32(ls, ctx + 0x58, idx);
            wr32(ls, entryR1 + 0x258, 0);
            res.handled = true;
            res.count = 0;
            return res;
        }
    }
    // per-vertex pass, 8 vertices at a time in float (separate arrays, no branches). The error bounds (3e-7 of the
    // magnitudes involved) cover both the SPU's float32 roundings and ours.
    struct Soa { float x[0x2000], y[0x2000], e[0x2000], p[4][0x2000]; int16_t qx[0x2000], qy[0x2000]; uint8_t f[0x2000]; };
    static thread_local Soa soaStore;
    Soa& V = soaStore;   // one TLS lookup (MinGW emulates thread_local with a call per access)
    uint64_t q0 = __rdtsc();
    {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(ls + pos);   // 16-byte aligned float4 per vertex
#pragma clang loop vectorize(assume_safety)
        for (uint32_t v = 0; v < nv; ++v) {
            const uint32_t u0 = __builtin_bswap32(src[4 * v]), u1 = __builtin_bswap32(src[4 * v + 1]),
                           u2 = __builtin_bswap32(src[4 * v + 2]), u3 = __builtin_bswap32(src[4 * v + 3]);
            std::memcpy(&V.p[0][v], &u0, 4); std::memcpy(&V.p[1][v], &u1, 4); std::memcpy(&V.p[2][v], &u2, 4); std::memcpy(&V.p[3][v], &u3, 4);
        }
    }
    uint64_t q1 = __rdtsc(); g_edgeCullCyc[0] += q1 - q0;
    float Tf[4][4], TAf[4][4];
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) { Tf[i][k] = float(T[i][k]); TAf[i][k] = float(TA[i][k]); }
    const float fScX = float(scX), fScY = float(scY), fOfX = float(ofX), fOfY = float(ofY);
    const float aScX = std::fabs(fScX), aScY = std::fabs(fScY), fW = float(sw), fH = float(sh);
    // Error bound of the transformed vertex: per vertex from its own magnitudes, or (constM) the one bound of the
    // whole job that the box test worked out, which covers every vertex: a third less arithmetic per vertex for
    // slightly wider margins. EDGECONSTM=0: always per vertex.
    static const bool constMOn = !std::getenv("EDGECONSTM") || std::atoi(std::getenv("EDGECONSTM")) != 0;
    const float mC[4] = {float(mbBox[0] * 1.0001), float(mbBox[1] * 1.0001), float(mbBox[2] * 1.0001), float(mbBox[3] * 1.0001)};
    auto vertexPass = [&](const bool constM) __attribute__((always_inline)) {
#pragma clang loop vectorize(assume_safety) vectorize_width(8)
    for (uint32_t v = 0; v < nv; ++v) {
        const float p0 = V.p[0][v], p1 = V.p[1][v], p2 = V.p[2][v], p3 = V.p[3][v];
        const float a0 = std::fabs(p0), a1 = std::fabs(p1), a2 = std::fabs(p2), a3 = std::fabs(p3);
        float c[4], m[4];
        for (int i = 0; i < 4; ++i) {
            const float t0 = Tf[i][0] * p0, t1 = Tf[i][1] * p1, t2 = Tf[i][2] * p2, t3 = Tf[i][3] * p3;
            c[i] = (t0 + t1) + (t2 + t3);
            // error bound: the SPU may round the matrix product M L and the world position as well as the sums
            m[i] = constM ? mC[i] : ((TAf[i][0] * a0 + TAf[i][1] * a1) + (TAf[i][2] * a2 + TAf[i][3] * a3)) * 3e-7f + 1e-30f;
        }
        const bool screen = c[3] > m[3] * 4 && c[3] > 1e-20f;
        const float iw = 1.0f / (screen ? c[3] : 1.0f), rw = m[3] * iw;
        const float x = c[0] * iw * fScX + fOfX, y = c[1] * iw * fScY + fOfY;
        // propagated error + the SPU's reciprocal estimate and float32 roundings (relative ~1e-6)
        const float e = (m[0] * iw + std::fabs(c[0] * iw) * rw) * aScX + (m[1] * iw + std::fabs(c[1] * iw) * rw) * aScY
                      + (std::fabs(x) + std::fabs(y) + 1.0f) * 6e-7f;
        int f = (c[2] - c[3] > m[2] + m[3] ? 0x04 : 0) | (c[2] < -m[2] ? 0x80 : 0);
        f |= screen ? ((y > fH + e ? 0x08 : 0) | (x > fW + e ? 0x10 : 0) | (y < -e ? 0x20 : 0) | (x < -e ? 0x40 : 0)) : 0;
        const bool noArea = c[2] <= m[2] || !screen;   // z < 0 not ruled out, or no screen position: keep
        // snapped coordinates as the SPU computes them: cflts (scale 8, toward zero) then & ~1, low 16 bits
        const float x8 = x * 8.0f, y8 = y * 8.0f, e8 = e * 8.0f + 1e-6f;
        const float tx = std::trunc(x8), ty = std::trunc(y8);
        const bool snapOk = std::fabs(x8) < 2.0e9f && std::fabs(y8) < 2.0e9f && std::trunc(x8 - e8) == tx && std::trunc(x8 + e8) == tx
                         && std::trunc(y8 - e8) == ty && std::trunc(y8 + e8) == ty;
        V.qx[v] = int16_t(int32_t(snapOk ? tx : 0.0f) & ~1);
        V.qy[v] = int16_t(int32_t(snapOk ? ty : 0.0f) & ~1);
        V.x[v] = x; V.y[v] = y; V.e[v] = e;
        V.f[v] = uint8_t(f | (noArea ? 1 : 0) | (noArea || !snapOk ? 2 : 0));
    }
    };
    if (constMOn && mbBox[0] >= 0) vertexPass(true); else vertexPass(false);
    uint64_t q2 = __rdtsc(); g_edgeCullCyc[1] += q2 - q1;
    if (boxOn) {   // every vertex shares an outcode bit (mixed-plane cases the box misses): every triangle is culled
        uint8_t all = 0xff;
        for (uint32_t v = 0; v < nv; ++v) all &= V.f[v];
        if (all & 0xfc) {
            uint32_t bad = 0;
            for (uint32_t t = 0; t < ni; ++t) bad |= uint32_t(be16(ls, idx + 2 * t) >= nv);
            if (!bad) {
                ++g_edgeCullBox;
                wr32(ls, ctx + 0x58, idx);
                wr32(ls, entryR1 + 0x258, 0);
                res.handled = true;
                res.count = 0;
                return res;
            }
        }
    }
    // triangles, compacted in place
    uint32_t kept = 0;
    for (uint32_t t = 0; t < ni; t += 3) {
        const uint16_t i0 = be16(ls, idx + 2 * t), i1 = be16(ls, idx + 2 * t + 2), i2 = be16(ls, idx + 2 * t + 4);
        if (i0 >= nv || i1 >= nv || i2 >= nv) return EdgeCullResult{};
        const uint8_t fa = V.f[i0], fb = V.f[i1], fc = V.f[i2];
        bool cull = (fa & fb & fc & 0xfc) != 0;
        uint8_t why = 0;   // 1 no area test (near / no screen position), 2 back face too close, 4 snap uncertain, 0 passed
        if (!cull && ((fa | fb | fc) & 1)) why = 1;
        if (!cull && !((fa | fb | fc) & 1)) {
            const double ax = V.x[i0], ay = V.y[i0], bx = V.x[i1], by = V.y[i1], cx = V.x[i2], cy = V.y[i2];   // area in double
            if (mode == 3) {
                const double t1 = (bx - ax) * (cy - ay), t2 = (cx - ax) * (by - ay);
                const double e = std::fmax(V.e[i0], std::fmax(V.e[i1], V.e[i2])) + (std::fabs(ax) + std::fabs(ay) + std::fabs(bx) + std::fabs(by) + std::fabs(cx) + std::fabs(cy)) * 1e-7;
                const double ea = (std::fabs(t1) + std::fabs(t2)) * 1e-6
                    + 2 * e * (std::fabs(bx - ax) + std::fabs(cx - ax) + std::fabs(by - ay) + std::fabs(cy - ay)) + 4 * e * e;
                if (t1 - t2 < -ea) cull = true;
                else if (t1 - t2 < ea) why |= 2;
            }
            if (!cull && !((fa | fb | fc) & 2)
                && ((V.qx[i0] == V.qx[i1] && V.qx[i0] == V.qx[i2]) || (V.qy[i0] == V.qy[i1] && V.qy[i0] == V.qy[i2]))) cull = true;
        }
        if (!cull && ((fa | fb | fc) & 2) && !why) why = 4;
        if (!cull) {
            if (g_edgeCullWhy) g_edgeCullWhy[kept / 3] = why;
            uint8_t* d = ls + ((idx + 2 * kept) & 0x3ffff);
            d[0] = uint8_t(i0 >> 8); d[1] = uint8_t(i0); d[2] = uint8_t(i1 >> 8); d[3] = uint8_t(i1); d[4] = uint8_t(i2 >> 8); d[5] = uint8_t(i2);
            kept += 3;
        }
    }
    g_edgeCullCyc[2] += __rdtsc() - q2;
    wr32(ls, ctx + 0x58, idx + ((kept * 2 + 15) & ~15u));
    wr32(ls, entryR1 + 0x258, kept);
    res.handled = true;
    res.count = kept;
    return res;
}

}  // namespace gti::recomp
