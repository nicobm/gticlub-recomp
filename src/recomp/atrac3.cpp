// ATRAC3 decoder: see atrac3.h (algorithm and tables from FFmpeg's atrac3.c / atrac.c, LGPL 2.1 or later).
#include "recomp/atrac3.h"
#include <cmath>
#include <cstring>
#include <cstdio>

namespace gti::recomp::atrac3 {
bool g_debug = false;
namespace {

constexpr int kFrame = 1024, kMdct = 512;

// ---------------------------------------------------------------- tables
const uint8_t kHuff[][2] = {   // {symbol + 31, code length}, canonical codes assigned in this order
    {31,1},{32,3},{33,3},{34,4},{35,4},{36,5},{37,5},{38,5},{39,5},
    {31,1},{32,3},{30,3},{33,3},{29,3},
    {31,1},{32,3},{30,3},{33,4},{29,4},{34,4},{28,4},
    {31,1},{32,3},{30,3},{33,4},{29,4},{34,5},{28,5},{35,5},{27,5},
    {31,2},{32,3},{30,3},{33,4},{29,4},{34,4},{28,4},{38,4},{24,4},{35,5},{27,5},{36,6},{26,6},{37,6},{25,6},
    {31,3},{32,4},{30,4},{33,4},{29,4},{34,4},{28,4},{46,4},{16,4},{35,5},{27,5},{36,5},{26,5},{37,5},{25,5},{38,6},
    {24,6},{39,6},{23,6},{40,6},{22,6},{41,6},{21,6},{42,7},{20,7},{43,7},{19,7},{44,7},{18,7},{45,7},{17,7},
    {31,3},{62,4},{0,4},{32,5},{30,5},{33,5},{29,5},{34,5},{28,5},{35,5},{27,5},{36,5},{26,5},{37,6},{25,6},{38,6},
    {24,6},{39,6},{23,6},{40,6},{22,6},{41,6},{21,6},{42,6},{20,6},{43,6},{19,6},{44,6},{18,6},{45,7},{17,7},{46,7},
    {16,7},{47,7},{15,7},{48,7},{14,7},{49,7},{13,7},{50,7},{12,7},{51,7},{11,7},{52,8},{10,8},{53,8},{9,8},{54,8},
    {8,8},{55,8},{7,8},{56,8},{6,8},{57,8},{5,8},{58,8},{4,8},{59,8},{3,8},{60,8},{2,8},{61,8},{1,8},
};
const uint8_t kHuffSizes[7] = {9, 5, 7, 9, 15, 31, 63};
const uint8_t kClcLength[8] = {0, 4, 3, 3, 4, 4, 5, 6};
const int8_t kMantissaClc[4] = {0, 1, -2, -1};
const int8_t kMantissaVlc[18] = {0, 0, 0, 1, 0, -1, 1, 0, -1, 0, 1, 1, 1, -1, -1, 1, -1, -1};
const float kInvMaxQuant[8] = {0.0f, 1.0f / 1.5f, 1.0f / 2.5f, 1.0f / 3.5f, 1.0f / 4.5f, 1.0f / 7.5f, 1.0f / 15.5f, 1.0f / 31.5f};
const uint16_t kSubband[33] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 288, 320, 352,
                               384, 416, 448, 480, 512, 576, 640, 704, 768, 896, 1024};
const float kQmf48Half[24] = {
    -0.00001461907f, -0.00009205479f, -0.000056157569f, 0.00030117269f, 0.0002422519f, -0.00085293897f, -0.0005205574f,
    0.0020340169f, 0.00078333891f, -0.0042153862f, -0.00075614988f, 0.0078402944f, -0.000061169922f, -0.01344162f,
    0.0024626821f, 0.021736089f, -0.007801671f, -0.034090221f, 0.01880949f, 0.054326009f, -0.043596379f, -0.099384367f,
    0.13207909f, 0.46424159f};

struct Tables {
    // VLC lookup per selector table: 8-bit index -> (symbol, length); length 0 = invalid code
    struct Entry { int8_t sym; uint8_t len; };
    Entry vlc[7][256];
    float sf[64], qmf[48], window[kMdct], gain1[16], gain2[31];
    // IMDCT basis for the 256 outputs that are not mirrors of others: n = 0..127 and 256..383
    // (y[255 - n] = -y[n] and y[767 - n] = y[n])
    float imdct[256][256];
    Tables() {
        const uint8_t (*h)[2] = kHuff;
        for (int t = 0; t < 7; ++t) {
            std::memset(vlc[t], 0, sizeof vlc[t]);
            uint32_t code = 0;   // left aligned on 8 bits
            for (int i = 0; i < kHuffSizes[t]; ++i, ++h) {
                const int len = (*h)[1], sym = int((*h)[0]) - 31;
                const uint32_t span = 1u << (8 - len);
                for (uint32_t k = 0; k < span; ++k) vlc[t][code + k] = {int8_t(sym), uint8_t(len)};
                code += span;
            }
        }
        for (int i = 0; i < 64; ++i) sf[i] = float(std::pow(2.0, (i - 15) / 3.0));
        for (int i = 0; i < 24; ++i) qmf[i] = qmf[47 - i] = kQmf48Half[i] * 2.0f;
        for (int i = 0, j = 255; i < 128; ++i, --j) {
            const double pi = 3.14159265358979323846;
            const float wi = float(std::sin(((i + 0.5) / 256.0 - 0.5) * pi) + 1.0);
            const float wj = float(std::sin(((j + 0.5) / 256.0 - 0.5) * pi) + 1.0);
            const float w = 0.5f * (wi * wi + wj * wj);
            window[i] = window[511 - i] = wi / w;
            window[j] = window[511 - j] = wj / w;
        }
        for (int i = 0; i < 16; ++i) gain1[i] = std::pow(2.0f, float(4 - i));        // id2exp_offset 4
        for (int i = -15; i < 16; ++i) gain2[i + 15] = std::pow(2.0f, -1.0f / 8 * i);   // loc_size 8
        // IMDCT of FFmpeg's AV_TX_FLOAT_MDCT (inverse, full output, scale 1/32768):
        // y[n] = -scale * sum_k X[k] cos(pi/256 * (n + 0.5 + 128) * (k + 0.5)), sign fitted against FFmpeg's output
        for (int r = 0; r < 256; ++r)
            for (int k = 0, n = r < 128 ? r : r + 128; k < 256; ++k)
                imdct[r][k] = float(-std::cos(3.14159265358979323846 / 256.0 * (n + 0.5 + 128.0) * (k + 0.5)) / 32768.0);
    }
};
const Tables& tables() { static const Tables t; return t; }

struct Bits {
    const uint8_t* p; int size, pos = 0;   // size in bits
    unsigned get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i, ++pos) v = v << 1 | (pos < size ? (p[pos >> 3] >> (7 - (pos & 7))) & 1u : 0u);
        return v;
    }
    int sget(int n) { const int v = int(get(n)); return v & (1 << (n - 1)) ? v - (1 << n) : v; }
    unsigned peek8() { const int s = pos; const unsigned v = get(8); pos = s; return v; }
};

struct Gain { int num = 0, lev[7] = {}, loc[7] = {}; };
struct Tonal { int pos = 0, num = 0; float coef[8] = {}; };

}   // namespace

struct Channel {
    float prev[kFrame];
    int gcSwitch;
    Gain gain[2][4];
    Tonal comp[64];
    int ncomp;
    float spectrum[kFrame], imdctBuf[kMdct];
    float delay1[46], delay2[46], delay3[46];
    void reset() { std::memset(this, 0, sizeof *this); }
};

namespace {

int vlc_get(Bits& b, int table) {
    const auto& e = tables().vlc[table][b.peek8()];
    b.pos += e.len ? e.len : 8;
    return e.sym;
}

void read_mantissas(Bits& b, int selector, int clc, int* m, int n) {
    if (selector == 1) n /= 2;
    if (clc) {
        const int bits = kClcLength[selector];
        if (selector > 1) for (int i = 0; i < n; ++i) m[i] = bits ? b.sget(bits) : 0;
        else for (int i = 0; i < n; ++i) { const int code = bits ? int(b.get(bits)) : 0; m[2 * i] = kMantissaClc[code >> 2]; m[2 * i + 1] = kMantissaClc[code & 3]; }
    } else {
        if (selector != 1) for (int i = 0; i < n; ++i) m[i] = vlc_get(b, selector - 1);
        else for (int i = 0; i < n; ++i) { const int s = vlc_get(b, 0); m[2 * i] = kMantissaVlc[s * 2]; m[2 * i + 1] = kMantissaVlc[s * 2 + 1]; }
    }
}

int decode_spectrum(Bits& b, float* out) {
    const Tables& T = tables();
    const int nsub = int(b.get(5)), clc = int(b.get(1));
    int sel[32], sfi[32], m[128];
    for (int i = 0; i <= nsub; ++i) sel[i] = int(b.get(3));
    for (int i = 0; i <= nsub; ++i) if (sel[i]) sfi[i] = int(b.get(6));
    int i = 0;
    for (; i <= nsub; ++i) {
        const int first = kSubband[i], last = kSubband[i + 1];
        if (sel[i]) {
            read_mantissas(b, sel[i], clc, m, last - first);
            const float scale = T.sf[sfi[i]] * kInvMaxQuant[sel[i]];
            for (int j = 0, k = first; k < last; ++k, ++j) out[k] = float(m[j]) * scale;
        } else std::memset(out + first, 0, sizeof(float) * (last - first));
    }
    std::memset(out + kSubband[i], 0, sizeof(float) * (kFrame - kSubband[i]));
    return nsub;
}

int decode_tonal(Bits& b, Tonal* comps, int nbands) {
    const Tables& T = tables();
    const int n = int(b.get(5));
    if (!n) return 0;
    const int modeSel = int(b.get(2));
    if (modeSel == 2) return -1;
    int mode = modeSel & 1, count = 0, flags[4], m[8];
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k <= nbands; ++k) flags[k] = int(b.get(1));
        const int perComp = int(b.get(3)), qstep = int(b.get(3));
        if (qstep <= 1) return -1;
        if (modeSel == 3) mode = int(b.get(1));
        for (int k = 0; k < (nbands + 1) * 4; ++k) {
            if (!flags[k >> 2]) continue;
            const int coded = int(b.get(3));
            for (int c = 0; c < coded; ++c) {
                const int sfi = int(b.get(6));
                if (count >= 64) return -1;
                Tonal& t = comps[count];
                t.pos = k * 64 + int(b.get(6));
                int values = perComp + 1;
                if (values > kFrame - t.pos) values = kFrame - t.pos;
                const float scale = T.sf[sfi] * kInvMaxQuant[qstep];
                read_mantissas(b, qstep, mode, m, values);
                t.num = values;
                for (int j = 0; j < values; ++j) t.coef[j] = float(m[j]) * scale;
                ++count;
            }
        }
    }
    return count;
}

bool decode_gain(Bits& b, Gain* g, int nbands) {
    int k = 0;
    for (; k <= nbands; ++k) {
        g[k].num = int(b.get(3));
        for (int j = 0; j < g[k].num; ++j) {
            g[k].lev[j] = int(b.get(4));
            g[k].loc[j] = int(b.get(5));
            if (j && g[k].loc[j] <= g[k].loc[j - 1]) return false;
        }
    }
    for (; k < 4; ++k) g[k].num = 0;
    return true;
}

void gain_compensation(const float* in, float* prev, const Gain& now, const Gain& next, float* out) {
    const Tables& T = tables();
    const int n = 256;
    const float scale = next.num ? T.gain1[next.lev[0]] : 1.0f;
    if (!now.num) {
        for (int i = 0; i < n; ++i) out[i] = in[i] * scale + prev[i];
    } else {
        int pos = 0;
        for (int i = 0; i < now.num; ++i) {
            const int last = now.loc[i] << 3;
            float lev = T.gain1[now.lev[i]];
            const float inc = T.gain2[(i + 1 < now.num ? now.lev[i + 1] : 4) - now.lev[i] + 15];
            for (; pos < last; ++pos) out[pos] = (in[pos] * scale + prev[pos]) * lev;
            for (; pos < last + 8; ++pos) { out[pos] = (in[pos] * scale + prev[pos]) * lev; lev *= inc; }
        }
        for (; pos < n; ++pos) out[pos] = in[pos] * scale + prev[pos];
    }
    std::memcpy(prev, in + n, sizeof(float) * n);
}

void imlt(float* in, float* out, bool odd) {
    const Tables& T = tables();
    if (odd) for (int i = 0; i < 128; ++i) { const float t = in[i]; in[i] = in[255 - i]; in[255 - i] = t; }
    for (int r = 0; r < 256; ++r) {
        const float* row = T.imdct[r];
        float acc[8] = {};   // 8 partial sums: the compiler keeps them in one vector register
        for (int k = 0; k < 256; k += 8)
            for (int j = 0; j < 8; ++j) acc[j] += row[k + j] * in[k + j];
        const float s = ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
        if (r < 128) { out[r] = s * T.window[r]; out[255 - r] = -s * T.window[255 - r]; }
        else { const int n = r + 128; out[n] = s * T.window[n]; out[767 - n] = s * T.window[767 - n]; }
    }
}

void iqmf(const float* lo, const float* hi, int n, float* out, float* delay) {
    const Tables& T = tables();
    float temp[46 + 2 * 512];
    std::memcpy(temp, delay, sizeof(float) * 46);
    float* p3 = temp + 46;
    for (int i = 0; i < n; i += 2) {
        p3[2 * i + 0] = lo[i] + hi[i];
        p3[2 * i + 1] = lo[i] - hi[i];
        p3[2 * i + 2] = lo[i + 1] + hi[i + 1];
        p3[2 * i + 3] = lo[i + 1] - hi[i + 1];
    }
    const float* p1 = temp;
    for (int j = n; j; --j) {
        float s1 = 0, s2 = 0;
        for (int i = 0; i < 48; i += 2) { s1 += p1[i] * T.qmf[i]; s2 += p1[i + 1] * T.qmf[i + 1]; }
        out[0] = s2; out[1] = s1;
        p1 += 2; out += 2;
    }
    std::memcpy(delay, temp + n * 2, sizeof(float) * 46);
}

bool decode_unit(Bits& b, Channel& c, float* out) {
    if (b.get(6) != 0x28) return false;
    Gain* g1 = c.gain[c.gcSwitch];
    Gain* g2 = c.gain[1 - c.gcSwitch];
    const int bands = int(b.get(2));
    if (!decode_gain(b, g2, bands)) return false;
    c.ncomp = decode_tonal(b, c.comp, bands);
    if (c.ncomp < 0) return false;
    const int nsub = decode_spectrum(b, c.spectrum);
    int lastTonal = -1;
    for (int i = 0; i < c.ncomp; ++i) {
        const Tonal& t = c.comp[i];
        if (t.pos + t.num > lastTonal) lastTonal = t.pos + t.num;
        for (int j = 0; j < t.num; ++j) c.spectrum[t.pos + j] += t.coef[j];
    }
    int nb = (kSubband[nsub + 1] - 1) >> 8;   // the band holding the last coded subband (older FFmpeg used kSubband[nsub])
    if (g_debug) { std::fprintf(stderr, "unit bands %d nsub %d ncomp %d lastTonal %d gain", bands, nsub, c.ncomp, lastTonal); for (int k = 0; k < 4; ++k) std::fprintf(stderr, " %d/%d", g1[k].num, g2[k].num); std::fputc(10, stderr); }
    if (lastTonal >= 0 && ((lastTonal + 256) >> 8) > nb) nb = (lastTonal + 256) >> 8;
    for (int band = 0; band < 4; ++band) {
        if (band <= nb) imlt(&c.spectrum[band * 256], c.imdctBuf, band & 1);
        else std::memset(c.imdctBuf, 0, sizeof c.imdctBuf);
        gain_compensation(c.imdctBuf, &c.prev[band * 256], g1[band], g2[band], &out[band * 256]);
    }
    c.gcSwitch ^= 1;
    return true;
}

}   // namespace

Decoder::Decoder(int channels, int unitBytes) : nch(channels), unit(unitBytes), ch(new Channel[channels]) { tables(); reset(); }
Decoder::~Decoder() { delete[] ch; }
void Decoder::reset() { for (int i = 0; i < nch; ++i) ch[i].reset(); }

bool Decoder::decode(const uint8_t* frame, float* const* out) {
    bool ok = true;
    for (int i = 0; i < nch; ++i) {
        Bits b{frame + i * unit, unit * 8};
        if (!decode_unit(b, ch[i], out[i])) { ok = false; std::memset(out[i], 0, sizeof(float) * kFrame); }
        if (i < 8) used[i] = b.pos < b.size ? b.pos : b.size;
        float* p1 = out[i];
        float* p2 = p1 + 256;
        float* p3 = p2 + 256;
        float* p4 = p3 + 256;
        iqmf(p1, p2, 256, p1, ch[i].delay1);
        iqmf(p4, p3, 256, p3, ch[i].delay2);
        iqmf(p1, p3, 512, p1, ch[i].delay3);
    }
    return ok;
}

}   // namespace gti::recomp::atrac3
