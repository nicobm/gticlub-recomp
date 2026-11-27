#pragma once
// Cell SPU interpreter: 128 x 128-bit registers, 256 KB local store, MFC DMA against the guest (PS3) memory.
// Channel traffic that is not plain DMA (mailboxes, signals, events, SPURS kernel calls) goes to host callbacks.
#include <cstdint>
#include <cstring>
#include <functional>

namespace gti::recomp {

// SPU single precision is not IEEE: there are no NaNs or infinities (exponent 255 is just a very large number).
// Code relies on it: Edge geometry computes c - a * b on bit masks, where 0 * 0xFFFFFFFF must be 0, not NaN.
// The rule that matters, applied by the interpreter and the recompiled code alike: a product with a zero factor
// is zero whatever the other factor is. Everything else is plain IEEE (clamping every operand and result to the
// SPU's range was tried: correct too, but it made the Edge jobs 40% slower).
inline float spu_mul(float a, float b) { return (a == 0.f) | (b == 0.f) ? 0.f : a * b; }

struct SpuReg {
    uint8_t b[16];   // big-endian: b[0] is the most significant byte of word 0 (the "preferred slot")
    uint32_t w(int i) const { return uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3]; }
    void sw(int i, uint32_t v) { b[4 * i] = uint8_t(v >> 24); b[4 * i + 1] = uint8_t(v >> 16); b[4 * i + 2] = uint8_t(v >> 8); b[4 * i + 3] = uint8_t(v); }
    uint16_t h(int i) const { return uint16_t(b[2 * i] << 8 | b[2 * i + 1]); }
    void sh(int i, uint16_t v) { b[2 * i] = uint8_t(v >> 8); b[2 * i + 1] = uint8_t(v); }
    float f(int i) const { uint32_t u = w(i); float x; std::memcpy(&x, &u, 4); return x; }
    void sf(int i, float x) { uint32_t u; std::memcpy(&u, &x, 4); sw(i, u); }
    uint64_t d(int i) const { return uint64_t(w(2 * i)) << 32 | w(2 * i + 1); }
    void sd(int i, uint64_t v) { sw(2 * i, uint32_t(v >> 32)); sw(2 * i + 1, uint32_t(v)); }
};

class Spu {
public:
    static constexpr uint32_t LS_SIZE = 0x40000;
    uint8_t ls[LS_SIZE];
    SpuReg r[128];
    uint32_t pc = 0;
    uint8_t* mem = nullptr;   // guest main memory (address 0)

    // MFC state
    uint32_t mfcLsa = 0, mfcEah = 0, mfcEal = 0, mfcSize = 0, mfcTag = 0, tagMask = 0;
    uint8_t rdata[128];       // GETLLAR reservation copy
    uint32_t rAddr = 0xFFFFFFFFu;
    uint32_t atomicStat = 0;

    // host hooks: return false to stop the SPU (e.g. the program exited)
    std::function<bool(Spu&, uint32_t code)> onStop;                  // stop / stopd with signal code
    std::function<bool(Spu&, uint32_t ch, uint32_t value)> onWriteCh;  // non-MFC channel write (mailbox, events...)
    std::function<bool(Spu&, uint32_t ch, uint32_t& value)> onReadCh;  // non-MFC channel read
    std::function<bool(Spu&, uint32_t target)> onBranch;               // called for branches below `hookBelow`
    uint32_t hookBelow = 0;

    uint32_t* interpHist = nullptr;   // debug: interpreted instructions per 256 bytes of local store (1024 counters)
    bool dropPuts = false; uint64_t droppedPuts = 0;   // debug: ignore PUT commands (what a module writes to main memory)
    int mfcTrace = 0;         // debug: log this many MFC commands
    bool running = false;
    uint64_t steps = 0, interpSteps = 0, compiledCalls = 0;
    uint32_t badInstr = 0;     // first undecoded instruction word (0 = none)

    // statically recompiled program (tools/spu2cpp.py) covering LS [compiledLo, compiledHi): run() enters it
    void (*compiled)(Spu&) = nullptr;
    uint32_t compiledLo = 0, compiledHi = 0;
    void (*compiled2)(Spu&) = nullptr;   // a second recompiled region (a module the program loads)
    uint32_t compiled2Lo = 0, compiled2Hi = 0;
    void (*compiled3)(Spu&) = nullptr;
    uint32_t compiled3Lo = 0, compiled3Hi = 0;
    // called by run() when pc enters (true) or leaves (false) [region3Lo, region3Hi); returning true on entry means the
    // hook did the work and set pc (to the return address)
    std::function<bool(Spu&, bool enter)> onRegion3;
    uint32_t region3Lo = 0, region3Hi = 0;
    bool inRegion3 = false;

    bool readChPub(uint32_t ch, uint32_t& v) { return readCh(ch, v); }
    bool writeChPub(uint32_t ch, uint32_t v) { return writeCh(ch, v); }
    uint32_t chCount(uint32_t ch);

    void reset() { std::memset(r, 0, sizeof r); pc = 0; running = false; badInstr = 0; rAddr = 0xFFFFFFFFu; }
    void run(uint64_t maxSteps = ~0ull);   // runs until a hook stops it, an unknown instruction, or maxSteps
    bool step();

    uint32_t lsw(uint32_t a) const { a &= LS_SIZE - 4; return uint32_t(ls[a]) << 24 | uint32_t(ls[a + 1]) << 16 | uint32_t(ls[a + 2]) << 8 | ls[a + 3]; }
    void lssw(uint32_t a, uint32_t v) { a &= LS_SIZE - 4; ls[a] = uint8_t(v >> 24); ls[a + 1] = uint8_t(v >> 16); ls[a + 2] = uint8_t(v >> 8); ls[a + 3] = uint8_t(v); }

private:
    void mfcCommand(uint32_t cmd);
    bool readCh(uint32_t ch, uint32_t& v);
    bool writeCh(uint32_t ch, uint32_t v);
};

} // namespace gti::recomp
