// Replays an Edge routine (SPU program at EA 0xbaea80) on captures made with FNCAP=entry,return,dir,seconds,n
// (fn_NNNN_in.bin / _out.bin: 256 KB local store + 128 registers) and checks the local store against the capture.
//   fn_replay <dir> <entry hex> <return hex> [first] [count]
//   env NATIVE=1  the native replacement (edge_native.cpp) instead of the recompiled SPU code: only the local store
//                 outside the routine's own stack frame and r3 are compared (saved registers, scratch are dead)
//   env BLOCKS=k  interpreter run of job k, blocks (branch targets) with counts -> stdout
//   env REPEAT=n  time n more runs of each job (state restored each time)
#include "recomp/spu.h"
#include "recomp/spu_rt.h"
#include "recomp/edge_native.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace gti::recomp;

static bool load(const char* fn, std::vector<uint8_t>& d) {
    FILE* f = std::fopen(fn, "rb");
    if (!f) return false;
    d.resize(Spu::LS_SIZE + 128 * 16);
    const size_t n = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    return n == d.size();
}
static uint32_t rw(const std::vector<uint8_t>& d, int reg, int k = 0) {
    const uint8_t* p = d.data() + Spu::LS_SIZE + 16 * reg + 4 * k;
    return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
static void set_state(Spu& s, const std::vector<uint8_t>& d) {
    std::memcpy(s.ls, d.data(), Spu::LS_SIZE);
    for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) s.r[i].sw(k, rw(d, i, k));
}
static uint32_t g_ret, g_r1; static bool g_returned;
static uint32_t stop_at_return(spurt::Q* __restrict r, Spu&, uint32_t pc) {
    if (pc == g_ret && r[1].w[0] == g_r1) { g_returned = true; return 0; }   // outside the program: the dispatcher stops
    return pc;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: fn_replay <dir> <entry hex> <return hex> [first] [count]\n"); return 1; }
    const uint32_t entry = uint32_t(std::strtoul(argv[2], nullptr, 16));
    g_ret = uint32_t(std::strtoul(argv[3], nullptr, 16));
    const int first = argc > 4 ? std::atoi(argv[4]) : 0, count = argc > 5 ? std::atoi(argv[5]) : 100000;
    const bool native = std::getenv("NATIVE") != nullptr;
    const int blocksJob = std::getenv("BLOCKS") ? std::atoi(std::getenv("BLOCKS")) : -1;
    const int repeat = std::getenv("REPEAT") ? std::atoi(std::getenv("REPEAT")) : 0;
    const spurt::Compiled* cp = spurt::find_compiled(0xbaea80);
    if (!cp) { std::printf("program not compiled in\n"); return 1; }
    spurt::g_spuHle[(0xbaea80u >> 4) % 64] = stop_at_return;
    static Spu s;
    std::vector<uint8_t> in, out;
    int jobs = 0, bad = 0, notHandled = 0;
    double totUs = 0;
    std::map<uint32_t, int> badByKey;
    for (int j = first; j < first + count; ++j) {
        char fi[512], fo[512];
        std::snprintf(fi, sizeof fi, "%s/fn_%04d_in.bin", argv[1], j);
        std::snprintf(fo, sizeof fo, "%s/fn_%04d_out.bin", argv[1], j);
        if (!load(fi, in) || !load(fo, out)) break;
        const uint32_t r1 = rw(in, 1);
        g_r1 = r1;
        auto run = [&]() -> bool {
            set_state(s, in);
            s.pc = entry;
            if (native) {
                uint32_t a[8], ret = 0;
                for (int i = 0; i < 8; ++i) a[i] = s.r[3 + i].w(0);
                if (!edge_native_run(entry, s.ls, a, r1, &ret)) return false;
                s.r[3].sw(0, ret);
                // like the hook in lv2.cpp: the routine's prologue saves the link register at r1 + 16 (caller's frame)
                std::memcpy(s.ls + ((r1 + 16) & 0x3fff0), s.r[0].b, 16);
                return true;
            }
            if (j == blocksJob) {
                std::map<uint32_t, uint64_t> blocks; uint32_t prev = 0;
                s.running = true;
                for (uint64_t k = 0; k < 50000000ull; ++k) {
                    if (s.pc == g_ret && s.r[1].w(0) == r1) break;
                    if (s.pc != prev + 4) ++blocks[s.pc];
                    prev = s.pc;
                    if (!s.step()) break;
                }
                for (auto& kv : blocks) std::printf("block %05x %llu\n", kv.first, (unsigned long long)kv.second);
                return true;
            }
            g_returned = false;
            cp->fn(s);
            if (g_returned) s.pc = g_ret;
            return true;
        };
        const auto t0 = std::chrono::steady_clock::now();
        const bool handled = run();
        totUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        static double repUs = 0;
        if (repeat > 0 && handled) {   // the repeated runs include restoring the state
            const auto r0 = std::chrono::steady_clock::now();
            for (int q = 0; q < repeat; ++q) run();
            repUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - r0).count() / repeat;
        }
        if (repeat > 0 && (j % 100) == 99) std::printf("repeat: %.2f us per run (incl. state restore)\n", repUs / (j - first + 1));
        ++jobs;
        if (!handled) { ++notHandled; continue; }
        // compare: native skips the routine's stack frame (below the entry r1) and registers other than r3
        int diffs = 0; uint32_t firstDiff = 0;
        const uint32_t frameLo = native ? (r1 - 0x800) & 0x3fff0 : 0x40000, frameHi = native ? r1 : 0x40000;
        for (uint32_t o = 0; o < Spu::LS_SIZE; o += 4) {
            if (o >= frameLo && o < frameHi) continue;
            if (native && o + 4 > g_edgeNativeDead[0] && o < g_edgeNativeDead[1]) {   // dead bytes; a word straddling the start: compare its live bytes
                if (o < g_edgeNativeDead[0] && std::memcmp(s.ls + o, out.data() + o, g_edgeNativeDead[0] - o)) { if (!diffs++) firstDiff = o; }
                continue;
            }
            if (std::memcmp(s.ls + o, out.data() + o, 4)) { if (!diffs++) firstDiff = o; }
        }
        int rdiff = -1;
        for (int i = 0; i < 128 && rdiff < 0; ++i) {
            if (native && i != 3) continue;
            for (int k = 0; k < (native ? 1 : 4); ++k) if (s.r[i].w(k) != rw(out, i, k)) { rdiff = i; break; }
        }
        if ((!native && s.pc != g_ret) || diffs || rdiff >= 0) {
            if (bad++ < 10) std::printf("job %04d: pc %05x, %d LS words differ (first %05x), first register differing r%d\n", j, s.pc, diffs, firstDiff, rdiff);
        }
    }
    std::printf("%d jobs, %d mismatched, %d not handled, %.2f us per call\n", jobs, bad, notHandled, jobs ? totUs / jobs : 0.0);
    return bad ? 2 : 0;
}
