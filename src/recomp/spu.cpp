// Cell SPU interpreter (see spu.h). Instruction semantics follow the SPU ISA 1.2; single precision uses host
// IEEE floats (the SPU's extended range / no-denormal behaviour is not modelled yet).
#include "recomp/spu.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace gti::recomp {

namespace {

using u128 = unsigned __int128;
inline int32_t sx(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }
inline u128 q(const SpuReg& r) { u128 x = 0; for (int i = 0; i < 16; ++i) x = (x << 8) | r.b[i]; return x; }
inline void setq(SpuReg& r, u128 x) { for (int i = 15; i >= 0; --i) { r.b[i] = uint8_t(x); x >>= 8; } }
std::mutex g_resvMx;   // serialises GETLLAR/PUTLLC line updates between SPUs
const auto g_dec0 = std::chrono::steady_clock::now();

// DMA put into main memory, last 16-byte block first: the RSX polls command buffer holes (a jump-to-self at the
// start) while SPU jobs fill them, so the first word must change only after everything behind it is in place.
void put_backwards(uint8_t* dst, const uint8_t* src, uint32_t n) {
    // everything after the first 16 bytes in one copy (it was 16 bytes at a time, from the end: a memcpy call per
    // block, a tenth of the sound mixer), then the head with its first word last
    uint32_t end = n;
    if (end > 16) { std::memcpy(dst + 16, src + 16, end - 16); end = 16; }
    std::atomic_thread_fence(std::memory_order_release);
    if (end >= 4) {   // first word last, atomically
        std::memcpy(dst + 4, src + 4, end - 4);
        std::atomic_thread_fence(std::memory_order_release);
        uint32_t w; std::memcpy(&w, src, 4);
        __atomic_store_n(reinterpret_cast<uint32_t*>(dst), w, __ATOMIC_RELEASE);
    } else std::memcpy(dst, src, end);
}

} // namespace

void Spu::mfcCommand(uint32_t cmd) {
    const uint32_t lsa = mfcLsa & (LS_SIZE - 1), size = mfcSize;
    const uint32_t ea = mfcEal;
    if (mfcTrace > 0) { --mfcTrace; std::fprintf(stderr, "[mfc] cmd %02x ls %05x ea %08x size %x tag %u (pc %05x)\n", cmd, lsa, ea, size, mfcTag, pc); }
    switch (cmd & 0xFF) {
        case 0x40: case 0x41: case 0x42:   // GET(B/F)
            if (size) std::memcpy(ls + lsa, mem + ea, std::min(size, LS_SIZE - lsa));
            break;
        case 0x20: case 0x21: case 0x22:   // PUT(B/F)
            if (dropPuts) { ++droppedPuts; break; }   // debug
            if (size) put_backwards(mem + ea, ls + lsa, std::min(size, LS_SIZE - lsa));
            break;
        case 0x44: case 0x45: case 0x46:   // GETL: list of {u32 size(+stall bit), u32 eal} at LS address EAL
        case 0x24: case 0x25: case 0x26: { // PUTL
            // each element lands at the running LS address with the low 4 bits of its EA; the running address
            // advances by the size rounded up to 16 bytes (MFC list semantics)
            uint32_t l = lsa & ~15u;
            const bool get = (cmd & 0xF0) == 0x40;
            struct El { uint32_t ls, ea, sz; };
            El els[2048]; uint32_t ne = 0;
            for (uint32_t off = 0; off + 8 <= size && ne < 2048; off += 8) {
                const uint32_t w0 = lsw((ea & (LS_SIZE - 1)) + off), w1 = lsw((ea & (LS_SIZE - 1)) + off + 4);
                const uint32_t sz = w0 & 0x7FFF;
                const uint32_t el = (l | (w1 & 15)) & (LS_SIZE - 1);
                els[ne++] = {el, w1, std::min(sz, LS_SIZE - el)};
                l = (l + ((sz + 15) & ~15u)) & (LS_SIZE - 1);
            }
            if (get) for (uint32_t k = 0; k < ne; ++k) std::memcpy(ls + els[k].ls, mem + els[k].ea, els[k].sz);
            else for (uint32_t k = ne; k-- > 0;) put_backwards(mem + els[k].ea, ls + els[k].ls, els[k].sz);   // last element first
            break;
        }
        case 0xD0: {   // GETLLAR
            std::lock_guard<std::mutex> lk(g_resvMx);
            const uint32_t line = ea & ~127u;
            std::memcpy(rdata, mem + line, 128);
            std::memcpy(ls + (lsa & ~127u), rdata, 128);
            rAddr = line;
            atomicStat = 4;
            break;
        }
        case 0xB4: {   // PUTLLC
            std::lock_guard<std::mutex> lk(g_resvMx);
            const uint32_t line = ea & ~127u;
            if (rAddr == line && std::memcmp(mem + line, rdata, 128) == 0) { std::memcpy(mem + line, ls + (lsa & ~127u), 128); atomicStat = 0; }
            else atomicStat = 1;
            rAddr = 0xFFFFFFFFu;
            break;
        }
        case 0xB0: case 0xB8: {   // PUTLLUC / PUTQLLUC
            std::lock_guard<std::mutex> lk(g_resvMx);
            std::memcpy(mem + (ea & ~127u), ls + (lsa & ~127u), 128);
            atomicStat = 2;
            break;
        }
        default: break;   // barriers, sync, signals: nothing to do with synchronous transfers
    }
}

bool Spu::readCh(uint32_t ch, uint32_t& v) {
    switch (ch) {
        case 24: v = tagMask; return true;              // MFC_RdTagStat: every transfer is already complete
        case 25: v = 0; return true;                    // list stall status
        case 27: v = atomicStat; return true;           // MFC_RdAtomicStat
        case 8: v = uint32_t(-int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_dec0).count() * 80)); return true;
        default: return onReadCh ? onReadCh(*this, ch, v) : (v = 0, true);
    }
}

bool Spu::writeCh(uint32_t ch, uint32_t v) {
    switch (ch) {
        case 16: mfcLsa = v; return true;
        case 17: mfcEah = v; return true;
        case 18: mfcEal = v; return true;
        case 19: mfcSize = v; return true;
        case 20: mfcTag = v; return true;
        case 21: mfcCommand(v); return true;
        case 22: tagMask = v; return true;
        case 23: case 26: case 7: return true;
        default: return onWriteCh ? onWriteCh(*this, ch, v) : true;
    }
}

void Spu::run(uint64_t maxSteps) {
    running = true;
    for (uint64_t k = 0; running && k < maxSteps; ++k) {
        if (onRegion3) {   // entering / leaving the third region (the music decoder module): the hook may run its own code instead
            const bool in = pc >= region3Lo && pc < region3Hi;
            if (in != inRegion3) {
                inRegion3 = in;
                if (onRegion3(*this, in)) { inRegion3 = pc >= region3Lo && pc < region3Hi; continue; }
            }
        }
        if (compiled && pc >= compiledLo && pc < compiledHi) {
            const uint32_t before = pc;
            ++compiledCalls; compiled(*this);
            if (pc != before) { if (pc < hookBelow && onBranch && !onBranch(*this, pc)) break; continue; }   // otherwise the interpreter executes the instruction the program left at
        } else if (compiled3 && pc >= compiled3Lo && pc < compiled3Hi) {
            const uint32_t before = pc;
            ++compiledCalls; compiled3(*this);
            if (pc != before) { if (pc < hookBelow && onBranch && !onBranch(*this, pc)) break; continue; }
        } else if (compiled2 && pc >= compiled2Lo && pc < compiled2Hi) {
            const uint32_t before = pc;
            ++compiledCalls; compiled2(*this);
            if (pc != before) { if (pc < hookBelow && onBranch && !onBranch(*this, pc)) break; continue; }
        }
        if (!step()) break;
    }
}

uint32_t Spu::chCount(uint32_t ch) {
    uint32_t cnt = 1;
    if (ch == 21) cnt = 16;
    else if (ch == 3 || ch == 4 || ch == 29 || ch == 0) { uint32_t v; cnt = (onReadCh && onReadCh(*this, 0x100 | ch, v)) ? v : 0; }
    return cnt;
}

bool Spu::step() {
    const uint32_t w = lsw(pc);
    uint32_t npc = (pc + 4) & (LS_SIZE - 1);
    ++steps; ++interpSteps;
    if (interpHist) ++interpHist[pc >> 8];
    const int rt = w & 0x7f, ra = (w >> 7) & 0x7f, rb = (w >> 14) & 0x7f;
    SpuReg& T = r[rt];
    const SpuReg& A = r[ra];
    const SpuReg& B = r[rb];
    auto branch = [&](uint32_t target) {
        target &= LS_SIZE - 4;
        if (target < hookBelow && onBranch) { pc = target; const bool ok = onBranch(*this, target); npc = pc; return ok; }   // the hook decides where execution resumes
        npc = target;
        return true;
    };
    auto bad = [&] { badInstr = w ? w : 0xFFFFFFFFu; running = false; return false; };

    // ---- RRR (bit 31 set)
    if (w >> 31) {
        const int op4 = w >> 28, rT = (w >> 21) & 0x7f, rc = w & 0x7f;
        SpuReg& D = r[rT];
        const SpuReg C = r[rc], AA = r[ra], BB = r[rb];
        switch (op4) {
            case 0x8: for (int i = 0; i < 16; ++i) D.b[i] = uint8_t((AA.b[i] & ~C.b[i]) | (BB.b[i] & C.b[i])); break;   // selb
            case 0xB: for (int i = 0; i < 16; ++i) {   // shufb
                const uint8_t s = C.b[i];
                D.b[i] = (s & 0xC0) == 0x80 ? 0x00 : (s & 0xE0) == 0xC0 ? 0xFF : (s & 0xE0) == 0xE0 ? 0x80 : ((s & 0x10) ? BB.b[s & 0xF] : AA.b[s & 0xF]);
            } break;
            case 0xC: for (int i = 0; i < 4; ++i) D.sw(i, uint32_t(int32_t(int16_t(AA.w(i))) * int32_t(int16_t(BB.w(i))) + int32_t(C.w(i)))); break;   // mpya
            case 0xD: for (int i = 0; i < 4; ++i) D.sf(i, C.f(i) - spu_mul(AA.f(i), BB.f(i))); break;   // fnms
            case 0xE: for (int i = 0; i < 4; ++i) D.sf(i, spu_mul(AA.f(i), BB.f(i)) + C.f(i)); break;   // fma
            case 0xF: for (int i = 0; i < 4; ++i) D.sf(i, spu_mul(AA.f(i), BB.f(i)) - C.f(i)); break;   // fms
            default: return bad();
        }
        pc = npc; return true;
    }
    // ---- RI18
    const uint32_t op7 = w >> 25;
    if (op7 == 0x21) { const uint32_t i18 = (w >> 7) & 0x3ffff; for (int i = 0; i < 4; ++i) T.sw(i, i18); pc = npc; return true; }   // ila
    if (op7 == 0x08 || op7 == 0x09) { pc = npc; return true; }   // hbra / hbrr
    // ---- RI16
    const uint32_t op9 = w >> 23;
    {
        const uint32_t i16 = (w >> 7) & 0xffff;
        const int32_t s16 = sx(i16, 16);
        switch (op9) {
            case 0x064: if (!branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true;   // br
            case 0x060: if (!branch(uint32_t(s16 << 2))) return false; pc = npc; return true;        // bra
            case 0x066: { SpuReg l{}; l.sw(0, npc); T = l; if (!branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true; }   // brsl
            case 0x062: { SpuReg l{}; l.sw(0, npc); T = l; if (!branch(uint32_t(s16 << 2))) return false; pc = npc; return true; }        // brasl
            case 0x042: if (T.w(0) != 0 && !branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true;   // brnz
            case 0x040: if (T.w(0) == 0 && !branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true;   // brz
            case 0x046: if (T.h(1) != 0 && !branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true;   // brhnz
            case 0x044: if (T.h(1) == 0 && !branch(pc + uint32_t(s16 << 2))) return false; pc = npc; return true;   // brhz
            case 0x061: std::memcpy(T.b, ls + (uint32_t(s16 << 2) & 0x3fff0), 16); pc = npc; return true;          // lqa
            case 0x067: std::memcpy(T.b, ls + ((pc + uint32_t(s16 << 2)) & 0x3fff0), 16); pc = npc; return true;   // lqr
            case 0x041: std::memcpy(ls + (uint32_t(s16 << 2) & 0x3fff0), T.b, 16); pc = npc; return true;          // stqa
            case 0x047: std::memcpy(ls + ((pc + uint32_t(s16 << 2)) & 0x3fff0), T.b, 16); pc = npc; return true;   // stqr
            case 0x081: for (int i = 0; i < 4; ++i) T.sw(i, uint32_t(s16)); pc = npc; return true;                  // il
            case 0x083: for (int i = 0; i < 8; ++i) T.sh(i, uint16_t(i16)); pc = npc; return true;                  // ilh
            case 0x082: for (int i = 0; i < 4; ++i) T.sw(i, i16 << 16); pc = npc; return true;                      // ilhu
            case 0x0C1: for (int i = 0; i < 4; ++i) T.sw(i, T.w(i) | i16); pc = npc; return true;                   // iohl
            case 0x065: for (int i = 0; i < 16; ++i) T.b[i] = (i16 >> (15 - i)) & 1 ? 0xFF : 0; pc = npc; return true;   // fsmbi
            default: break;
        }
    }
    // ---- RI10
    const uint32_t op8 = w >> 24;
    {
        const int32_t i10 = sx((w >> 14) & 0x3ff, 10);
        const SpuReg a = A;
        SpuReg o{};
        bool done = true;
        switch (op8) {
            case 0x34: std::memcpy(T.b, ls + ((a.w(0) + uint32_t(i10 << 4)) & 0x3fff0), 16); pc = npc; return true;   // lqd
            case 0x24: std::memcpy(ls + ((a.w(0) + uint32_t(i10 << 4)) & 0x3fff0), T.b, 16); pc = npc; return true;   // stqd
            case 0x1C: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) + uint32_t(i10)); break;                     // ai
            case 0x1D: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(a.h(i) + i10)); break;                     // ahi
            case 0x0C: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(i10) - a.w(i)); break;                     // sfi
            case 0x0D: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(i10 - a.h(i))); break;                     // sfhi
            case 0x14: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) & uint32_t(i10)); break;                     // andi
            case 0x15: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(a.h(i) & i10)); break;                     // andhi
            case 0x16: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] & i10); break;                     // andbi
            case 0x04: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) | uint32_t(i10)); break;                     // ori
            case 0x05: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(a.h(i) | uint16_t(i10))); break;           // orhi
            case 0x06: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] | uint8_t(i10)); break;            // orbi
            case 0x44: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) ^ uint32_t(i10)); break;                     // xori
            case 0x45: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(a.h(i) ^ uint16_t(i10))); break;           // xorhi
            case 0x46: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] ^ uint8_t(i10)); break;            // xorbi
            case 0x74: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(int32_t(int16_t(a.w(i))) * i10)); break;   // mpyi
            case 0x75: for (int i = 0; i < 4; ++i) o.sw(i, (a.w(i) & 0xffff) * (uint32_t(i10) & 0xffff)); break;   // mpyui
            case 0x7C: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) == uint32_t(i10) ? ~0u : 0); break;          // ceqi
            case 0x7D: for (int i = 0; i < 8; ++i) o.sh(i, a.h(i) == uint16_t(i10) ? 0xffff : 0); break;       // ceqhi
            case 0x7E: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] == uint8_t(i10) ? 0xff : 0; break;         // ceqbi
            case 0x4C: for (int i = 0; i < 4; ++i) o.sw(i, int32_t(a.w(i)) > i10 ? ~0u : 0); break;            // cgti
            case 0x4D: for (int i = 0; i < 8; ++i) o.sh(i, int16_t(a.h(i)) > int16_t(i10) ? 0xffff : 0); break;   // cgthi
            case 0x4E: for (int i = 0; i < 16; ++i) o.b[i] = int8_t(a.b[i]) > int8_t(i10) ? 0xff : 0; break;    // cgtbi
            case 0x5C: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) > uint32_t(i10) ? ~0u : 0); break;           // clgti
            case 0x5D: for (int i = 0; i < 8; ++i) o.sh(i, a.h(i) > uint16_t(i10) ? 0xffff : 0); break;        // clgthi
            case 0x5E: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] > uint8_t(i10) ? 0xff : 0; break;          // clgtbi
            case 0x7F: if (a.w(0) == uint32_t(i10)) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;   // heqi
            case 0x4F: if (int32_t(a.w(0)) > i10) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;    // hgti
            case 0x5F: if (a.w(0) > uint32_t(i10)) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;    // hlgti
            default: done = false; break;
        }
        if (done) { T = o; pc = npc; return true; }
    }
    // ---- RI8 (float conversions)
    const uint32_t op10 = w >> 22;
    if (op10 >= 0x1D8 && op10 <= 0x1DB) {
        const int i8 = int((w >> 14) & 0xff);
        SpuReg o{};
        for (int i = 0; i < 4; ++i) {
            switch (op10) {
                case 0x1D8: { const double v = std::ldexp(double(A.f(i)), 173 - i8); o.sw(i, uint32_t(v >= 2147483647.0 ? 0x7fffffff : v <= -2147483648.0 ? int32_t(0x80000000u) : int32_t(v))); break; }   // cflts
                case 0x1D9: { const double v = std::ldexp(double(A.f(i)), 173 - i8); o.sw(i, v >= 4294967295.0 ? 0xffffffffu : v <= 0 ? 0u : uint32_t(v)); break; }   // cfltu
                case 0x1DA: o.sf(i, float(std::ldexp(double(int32_t(A.w(i))), -(155 - i8)))); break;    // csflt
                case 0x1DB: o.sf(i, float(std::ldexp(double(A.w(i)), -(155 - i8)))); break;            // cuflt
            }
        }
        T = o; pc = npc; return true;
    }
    // ---- RR / RI7 (11-bit opcode)
    const uint32_t op11 = w >> 21;
    const SpuReg a = A, bb = B;
    SpuReg o{};
    const int i7 = sx((w >> 14) & 0x7f, 7);
    auto qshl_bytes = [&](int n) { for (int i = 0; i < 16; ++i) o.b[i] = i + n < 16 ? a.b[i + n] : 0; };
    auto qshr_bytes = [&](int n) { for (int i = 0; i < 16; ++i) o.b[i] = i - n >= 0 ? a.b[i - n] : 0; };
    auto qrot_bytes = [&](int n) { for (int i = 0; i < 16; ++i) o.b[i] = a.b[(i + n) & 15]; };
    auto insmask = [&](uint32_t addr, int size) {   // cbd/chd/cwd/cdd family
        for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(0x10 + i);
        const int t = int(addr & 0xF) & ~(size - 1);
        static const uint8_t pat[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const uint8_t* p = size == 1 ? (const uint8_t*)"\x03" : size == 2 ? (const uint8_t*)"\x02\x03" : pat;
        for (int k = 0; k < size; ++k) o.b[t + k] = p[k];
    };
    switch (op11) {
        // control
        case 0x000: { running = false; if (onStop && onStop(*this, w & 0x3fff)) { running = true; pc = npc; return true; } pc = npc; return false; }   // stop
        case 0x140: { running = false; if (onStop && onStop(*this, w & 0x3fff)) { running = true; pc = npc; return true; } pc = npc; return false; }   // stopd
        case 0x001: case 0x201: case 0x002: case 0x003: pc = npc; return true;   // lnop, nop, sync, dsync
        case 0x00C: T = SpuReg{}; pc = npc; return true;   // mfspr
        case 0x10C: case 0x3BA: pc = npc; return true;     // mtspr, fscrwr
        case 0x398: T = SpuReg{}; pc = npc; return true;   // fscrrd
        case 0x00D: { uint32_t v = 0; if (!readCh(uint32_t(ra), v)) { running = false; return false; } SpuReg x{}; x.sw(0, v); T = x; pc = npc; return true; }   // rdch
        case 0x00F: { SpuReg x{}; x.sw(0, chCount(uint32_t(ra))); T = x; pc = npc; return true; }   // rchcnt
        case 0x10D: if (!writeCh(uint32_t(ra), T.w(0))) { running = false; return false; } pc = npc; return true;   // wrch
        // loads/stores indexed
        case 0x1C4: std::memcpy(T.b, ls + ((a.w(0) + bb.w(0)) & 0x3fff0), 16); pc = npc; return true;   // lqx
        case 0x144: std::memcpy(ls + ((a.w(0) + bb.w(0)) & 0x3fff0), T.b, 16); pc = npc; return true;   // stqx
        case 0x1F4: insmask(a.w(0) + uint32_t(i7), 1); break;   // cbd
        case 0x1F5: insmask(a.w(0) + uint32_t(i7), 2); break;   // chd
        case 0x1F6: insmask(a.w(0) + uint32_t(i7), 4); break;   // cwd
        case 0x1F7: insmask(a.w(0) + uint32_t(i7), 8); break;   // cdd
        case 0x1D4: insmask(a.w(0) + bb.w(0), 1); break;        // cbx
        case 0x1D5: insmask(a.w(0) + bb.w(0), 2); break;        // chx
        case 0x1D6: insmask(a.w(0) + bb.w(0), 4); break;        // cwx
        case 0x1D7: insmask(a.w(0) + bb.w(0), 8); break;        // cdx
        // branches
        case 0x1A8: if (!branch(a.w(0))) return false; pc = npc; return true;   // bi
        case 0x1A9: { const uint32_t tg = a.w(0); SpuReg l{}; l.sw(0, npc); T = l; if (!branch(tg)) return false; pc = npc; return true; }   // bisl
        case 0x1AA: pc = npc; return true;   // iret (no interrupts)
        case 0x1AB: { SpuReg l{}; l.sw(0, npc); T = l; pc = npc; return true; }   // bisled: no event pending
        case 0x128: if (T.w(0) == 0 && !branch(a.w(0))) return false; pc = npc; return true;   // biz
        case 0x129: if (T.w(0) != 0 && !branch(a.w(0))) return false; pc = npc; return true;   // binz
        case 0x12A: if (T.h(1) == 0 && !branch(a.w(0))) return false; pc = npc; return true;   // bihz
        case 0x12B: if (T.h(1) != 0 && !branch(a.w(0))) return false; pc = npc; return true;   // bihnz
        case 0x1AC: pc = npc; return true;   // hbr
        // integer arithmetic
        case 0x0C0: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) + bb.w(i)); break;   // a
        case 0x0C8: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(a.h(i) + bb.h(i))); break;   // ah
        case 0x040: for (int i = 0; i < 4; ++i) o.sw(i, bb.w(i) - a.w(i)); break;   // sf
        case 0x048: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(bb.h(i) - a.h(i))); break;   // sfh
        case 0x0C2: for (int i = 0; i < 4; ++i) o.sw(i, (uint64_t(a.w(i)) + bb.w(i)) >> 32); break;   // cg
        case 0x042: for (int i = 0; i < 4; ++i) o.sw(i, bb.w(i) >= a.w(i) ? 1 : 0); break;   // bg
        case 0x340: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) + bb.w(i) + (T.w(i) & 1)); break;   // addx
        case 0x341: for (int i = 0; i < 4; ++i) o.sw(i, bb.w(i) + ~a.w(i) + (T.w(i) & 1)); break;   // sfx
        case 0x342: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t((uint64_t(a.w(i)) + bb.w(i) + (T.w(i) & 1)) >> 32)); break;   // cgx
        case 0x343: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t((uint64_t(bb.w(i)) + uint32_t(~a.w(i)) + (T.w(i) & 1)) >> 32)); break;   // bgx
        case 0x3C4: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(int32_t(int16_t(a.w(i))) * int32_t(int16_t(bb.w(i))))); break;   // mpy
        case 0x3CC: for (int i = 0; i < 4; ++i) o.sw(i, (a.w(i) & 0xffff) * (bb.w(i) & 0xffff)); break;   // mpyu
        case 0x3C5: for (int i = 0; i < 4; ++i) o.sw(i, ((a.w(i) >> 16) * (bb.w(i) & 0xffff)) << 16); break;   // mpyh
        case 0x3C7: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t((int32_t(int16_t(a.w(i))) * int32_t(int16_t(bb.w(i)))) >> 16)); break;   // mpys
        case 0x3C6: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(int32_t(int16_t(a.w(i) >> 16)) * int32_t(int16_t(bb.w(i) >> 16)))); break;   // mpyhh
        case 0x346: for (int i = 0; i < 4; ++i) o.sw(i, T.w(i) + uint32_t(int32_t(int16_t(a.w(i) >> 16)) * int32_t(int16_t(bb.w(i) >> 16)))); break;   // mpyhha
        case 0x3CE: for (int i = 0; i < 4; ++i) o.sw(i, (a.w(i) >> 16) * (bb.w(i) >> 16)); break;   // mpyhhu
        case 0x34E: for (int i = 0; i < 4; ++i) o.sw(i, T.w(i) + (a.w(i) >> 16) * (bb.w(i) >> 16)); break;   // mpyhhau
        case 0x2A5: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) ? uint32_t(__builtin_clz(a.w(i))) : 32); break;   // clz
        case 0x2B4: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(__builtin_popcount(a.b[i])); break;   // cntb
        case 0x1B6: { const uint32_t m = a.w(0) & 0xffff; for (int i = 0; i < 16; ++i) o.b[i] = (m >> (15 - i)) & 1 ? 0xff : 0; break; }   // fsmb
        case 0x1B5: { const uint32_t m = a.w(0) & 0xff; for (int i = 0; i < 8; ++i) o.sh(i, (m >> (7 - i)) & 1 ? 0xffff : 0); break; }   // fsmh
        case 0x1B4: { const uint32_t m = a.w(0) & 0xf; for (int i = 0; i < 4; ++i) o.sw(i, (m >> (3 - i)) & 1 ? ~0u : 0); break; }   // fsm
        case 0x1B2: { uint32_t m = 0; for (int i = 0; i < 16; ++i) m = (m << 1) | (a.b[i] & 1); o.sw(0, m); break; }   // gbb
        case 0x1B1: { uint32_t m = 0; for (int i = 0; i < 8; ++i) m = (m << 1) | (a.h(i) & 1); o.sw(0, m); break; }   // gbh
        case 0x1B0: { uint32_t m = 0; for (int i = 0; i < 4; ++i) m = (m << 1) | (a.w(i) & 1); o.sw(0, m); break; }   // gb
        case 0x0D3: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t((a.b[i] + bb.b[i] + 1) >> 1); break;   // avgb
        case 0x053: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] > bb.b[i] ? a.b[i] - bb.b[i] : bb.b[i] - a.b[i]); break;   // absdb
        case 0x253: for (int i = 0; i < 4; ++i) {   // sumb
            uint32_t sa = 0, sb = 0;
            for (int k = 0; k < 4; ++k) { sa += a.b[4 * i + k]; sb += bb.b[4 * i + k]; }
            o.sh(2 * i, uint16_t(sb)); o.sh(2 * i + 1, uint16_t(sa));
        } break;
        case 0x2B6: for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(int16_t(int8_t(a.b[2 * i + 1])))); break;   // xsbh
        case 0x2AE: for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(int32_t(int16_t(a.w(i))))); break;          // xshw
        case 0x2A6: for (int i = 0; i < 2; ++i) o.sd(i, uint64_t(int64_t(int32_t(a.w(2 * i + 1))))); break;   // xswd
        // logical
        case 0x0C1: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] & bb.b[i]; break;                 // and
        case 0x2C1: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] & ~bb.b[i]); break;       // andc
        case 0x041: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] | bb.b[i]; break;                 // or
        case 0x2C9: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(a.b[i] | ~bb.b[i]); break;       // orc
        case 0x1F0: o.sw(0, a.w(0) | a.w(1) | a.w(2) | a.w(3)); break;                              // orx
        case 0x241: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] ^ bb.b[i]; break;                 // xor
        case 0x0C9: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(~(a.b[i] & bb.b[i])); break;     // nand
        case 0x049: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(~(a.b[i] | bb.b[i])); break;     // nor
        case 0x249: for (int i = 0; i < 16; ++i) o.b[i] = uint8_t(~(a.b[i] ^ bb.b[i])); break;     // eqv
        // shifts / rotates, word and halfword
        case 0x05B: for (int i = 0; i < 4; ++i) { const uint32_t s = bb.w(i) & 0x3f; o.sw(i, s > 31 ? 0 : a.w(i) << s); } break;   // shl
        case 0x05F: for (int i = 0; i < 8; ++i) { const uint32_t s = bb.h(i) & 0x1f; o.sh(i, uint16_t(s > 15 ? 0 : a.h(i) << s)); } break;   // shlh
        case 0x07B: { const uint32_t s = uint32_t(i7) & 0x3f; for (int i = 0; i < 4; ++i) o.sw(i, s > 31 ? 0 : a.w(i) << s); } break;   // shli
        case 0x07F: { const uint32_t s = uint32_t(i7) & 0x1f; for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(s > 15 ? 0 : a.h(i) << s)); } break;   // shlhi
        case 0x058: for (int i = 0; i < 4; ++i) { const uint32_t s = bb.w(i) & 31; o.sw(i, (a.w(i) << s) | (s ? a.w(i) >> (32 - s) : 0)); } break;   // rot
        case 0x05C: for (int i = 0; i < 8; ++i) { const uint32_t s = bb.h(i) & 15; o.sh(i, uint16_t((a.h(i) << s) | (s ? a.h(i) >> (16 - s) : 0))); } break;   // roth
        case 0x078: { const uint32_t s = uint32_t(i7) & 31; for (int i = 0; i < 4; ++i) o.sw(i, (a.w(i) << s) | (s ? a.w(i) >> (32 - s) : 0)); } break;   // roti
        case 0x07C: { const uint32_t s = uint32_t(i7) & 15; for (int i = 0; i < 8; ++i) o.sh(i, uint16_t((a.h(i) << s) | (s ? a.h(i) >> (16 - s) : 0))); } break;   // rothi
        case 0x059: for (int i = 0; i < 4; ++i) { const uint32_t s = uint32_t(-int32_t(bb.w(i))) & 0x3f; o.sw(i, s > 31 ? 0 : a.w(i) >> s); } break;   // rotm
        case 0x05D: for (int i = 0; i < 8; ++i) { const uint32_t s = uint32_t(-int32_t(bb.h(i))) & 0x1f; o.sh(i, uint16_t(s > 15 ? 0 : a.h(i) >> s)); } break;   // rothm
        case 0x079: { const uint32_t s = uint32_t(-i7) & 0x3f; for (int i = 0; i < 4; ++i) o.sw(i, s > 31 ? 0 : a.w(i) >> s); } break;   // rotmi
        case 0x07D: { const uint32_t s = uint32_t(-i7) & 0x1f; for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(s > 15 ? 0 : a.h(i) >> s)); } break;   // rothmi
        case 0x05A: for (int i = 0; i < 4; ++i) { const uint32_t s = uint32_t(-int32_t(bb.w(i))) & 0x3f; o.sw(i, uint32_t(int32_t(a.w(i)) >> std::min<uint32_t>(s, 31))); } break;   // rotma
        case 0x05E: for (int i = 0; i < 8; ++i) { const uint32_t s = uint32_t(-int32_t(bb.h(i))) & 0x1f; o.sh(i, uint16_t(int16_t(a.h(i)) >> std::min<uint32_t>(s, 15))); } break;   // rotmah
        case 0x07A: { const uint32_t s = uint32_t(-i7) & 0x3f; for (int i = 0; i < 4; ++i) o.sw(i, uint32_t(int32_t(a.w(i)) >> std::min<uint32_t>(s, 31))); } break;   // rotmai
        case 0x07E: { const uint32_t s = uint32_t(-i7) & 0x1f; for (int i = 0; i < 8; ++i) o.sh(i, uint16_t(int16_t(a.h(i)) >> std::min<uint32_t>(s, 15))); } break;   // rotmahi
        // quadword shifts / rotates
        case 0x1DF: qshl_bytes(int(bb.w(0) & 0x1f)); break;                 // shlqby
        case 0x1FF: qshl_bytes(int(uint32_t(i7) & 0x1f)); break;            // shlqbyi
        case 0x1CF: qshl_bytes(int((bb.w(0) >> 3) & 0x1f)); break;          // shlqbybi
        case 0x1DC: qrot_bytes(int(bb.w(0) & 0xf)); break;                  // rotqby
        case 0x1FC: qrot_bytes(int(uint32_t(i7) & 0xf)); break;             // rotqbyi
        case 0x1CC: qrot_bytes(int((bb.w(0) >> 3) & 0xf)); break;           // rotqbybi
        case 0x1DD: qshr_bytes(int(uint32_t(-int32_t(bb.w(0))) & 0x1f)); break;          // rotqmby
        case 0x1FD: qshr_bytes(int(uint32_t(-i7) & 0x1f)); break;                        // rotqmbyi
        case 0x1CD: qshr_bytes(int(uint32_t(-int32_t(bb.w(0) >> 3)) & 0x1f)); break;     // rotqmbybi
        case 0x1DB: setq(o, q(a) << (bb.w(0) & 7)); break;                                // shlqbi
        case 0x1FB: setq(o, q(a) << (uint32_t(i7) & 7)); break;                           // shlqbii
        case 0x1D8: { const int s = int(bb.w(0) & 7); const u128 x = q(a); setq(o, s ? (x << s) | (x >> (128 - s)) : x); } break;   // rotqbi
        case 0x1F8: { const int s = int(uint32_t(i7) & 7); const u128 x = q(a); setq(o, s ? (x << s) | (x >> (128 - s)) : x); } break;   // rotqbii
        case 0x1D9: setq(o, q(a) >> (uint32_t(-int32_t(bb.w(0))) & 7)); break;          // rotqmbi
        case 0x1F9: setq(o, q(a) >> (uint32_t(-i7) & 7)); break;                         // rotqmbii
        // compares
        case 0x3C0: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) == bb.w(i) ? ~0u : 0); break;   // ceq
        case 0x3C8: for (int i = 0; i < 8; ++i) o.sh(i, a.h(i) == bb.h(i) ? 0xffff : 0); break;   // ceqh
        case 0x3D0: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] == bb.b[i] ? 0xff : 0; break;   // ceqb
        case 0x240: for (int i = 0; i < 4; ++i) o.sw(i, int32_t(a.w(i)) > int32_t(bb.w(i)) ? ~0u : 0); break;   // cgt
        case 0x248: for (int i = 0; i < 8; ++i) o.sh(i, int16_t(a.h(i)) > int16_t(bb.h(i)) ? 0xffff : 0); break;   // cgth
        case 0x250: for (int i = 0; i < 16; ++i) o.b[i] = int8_t(a.b[i]) > int8_t(bb.b[i]) ? 0xff : 0; break;   // cgtb
        case 0x2C0: for (int i = 0; i < 4; ++i) o.sw(i, a.w(i) > bb.w(i) ? ~0u : 0); break;   // clgt
        case 0x2C8: for (int i = 0; i < 8; ++i) o.sh(i, a.h(i) > bb.h(i) ? 0xffff : 0); break;   // clgth
        case 0x2D0: for (int i = 0; i < 16; ++i) o.b[i] = a.b[i] > bb.b[i] ? 0xff : 0; break;   // clgtb
        case 0x3D8: if (a.w(0) == bb.w(0)) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;   // heq
        case 0x258: if (int32_t(a.w(0)) > int32_t(bb.w(0))) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;   // hgt
        case 0x2D8: if (a.w(0) > bb.w(0)) { running = false; if (onStop) onStop(*this, 0x3FFF); return false; } pc = npc; return true;   // hlgt
        // floating point
        case 0x2C4: for (int i = 0; i < 4; ++i) o.sf(i, a.f(i) + bb.f(i)); break;   // fa
        case 0x2C5: for (int i = 0; i < 4; ++i) o.sf(i, a.f(i) - bb.f(i)); break;   // fs
        case 0x2C6: for (int i = 0; i < 4; ++i) o.sf(i, spu_mul(a.f(i), bb.f(i))); break;   // fm
        case 0x3C2: for (int i = 0; i < 4; ++i) o.sw(i, a.f(i) == bb.f(i) ? ~0u : 0); break;   // fceq
        case 0x2C2: for (int i = 0; i < 4; ++i) o.sw(i, a.f(i) > bb.f(i) ? ~0u : 0); break;    // fcgt
        case 0x3CA: for (int i = 0; i < 4; ++i) o.sw(i, std::fabs(a.f(i)) == std::fabs(bb.f(i)) ? ~0u : 0); break;   // fcmeq
        case 0x2CA: for (int i = 0; i < 4; ++i) o.sw(i, std::fabs(a.f(i)) > std::fabs(bb.f(i)) ? ~0u : 0); break;    // fcmgt
        case 0x1B8: for (int i = 0; i < 4; ++i) o.sf(i, 1.0f / a.f(i)); break;                 // frest (exact)
        case 0x1B9: for (int i = 0; i < 4; ++i) o.sf(i, 1.0f / std::sqrt(std::fabs(a.f(i)))); break;   // frsqest (exact)
        case 0x3D4: o = bb; break;                                                              // fi: estimate already exact
        case 0x2CC: for (int i = 0; i < 2; ++i) { double x, y; uint64_t u = a.d(i), v = bb.d(i); std::memcpy(&x, &u, 8); std::memcpy(&y, &v, 8); x += y; std::memcpy(&u, &x, 8); o.sd(i, u); } break;   // dfa
        case 0x2CD: for (int i = 0; i < 2; ++i) { double x, y; uint64_t u = a.d(i), v = bb.d(i); std::memcpy(&x, &u, 8); std::memcpy(&y, &v, 8); x -= y; std::memcpy(&u, &x, 8); o.sd(i, u); } break;   // dfs
        case 0x2CE: for (int i = 0; i < 2; ++i) { double x, y; uint64_t u = a.d(i), v = bb.d(i); std::memcpy(&x, &u, 8); std::memcpy(&y, &v, 8); x *= y; std::memcpy(&u, &x, 8); o.sd(i, u); } break;   // dfm
        case 0x3B8: for (int i = 0; i < 2; ++i) { const double x = a.f(2 * i); uint64_t u; std::memcpy(&u, &x, 8); o.sd(i, u); } break;   // fesd
        case 0x3B9: for (int i = 0; i < 2; ++i) { double x; uint64_t u = a.d(i); std::memcpy(&x, &u, 8); o.sf(2 * i, float(x)); } break;   // frds
        default: return bad();
    }
    T = o;
    pc = npc;
    return true;
}

} // namespace gti::recomp

namespace gti::recomp::spurt {
static std::mutex g_slowMx;
static std::map<uint64_t, uint64_t> g_slowPcs;
void spu_slow_note(uint32_t ea, uint32_t pc) { std::lock_guard<std::mutex> lk(g_slowMx); ++g_slowPcs[uint64_t(ea) << 32 | pc]; }
void spu_slow_report() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    { std::lock_guard<std::mutex> lk(g_slowMx); for (auto& kv : g_slowPcs) v.push_back({kv.second, kv.first}); g_slowPcs.clear(); }
    std::sort(v.rbegin(), v.rend());
    std::string o; char b[48];
    for (size_t k = 0; k < v.size() && k < 40; ++k) { std::snprintf(b, sizeof b, " %x:%x:%llu", uint32_t(v[k].second >> 32), uint32_t(v[k].second), (unsigned long long)v[k].first); o += b; }
    std::fprintf(stderr, "[spuslow] slow-path entries (program:LS:count):%s\n", o.c_str());
}
} // namespace gti::recomp::spurt
