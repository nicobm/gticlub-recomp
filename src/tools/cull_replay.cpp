// Replays Edge's triangle culling routine (LS 0x9e68 of the SPU program at EA 0xbaea80) on captures made with
// CULLCAP=dir,seconds,n (cull_NNNN_in.bin / _out.bin: 256 KB local store + 128 registers) and checks the result.
//   cull_replay <dir> [first] [count]          recompiled code; prints mismatches and us per call
//   env INTERP=1  run in the interpreter
//   env BLOCKS=k  interpreter: blocks (branch targets) executed in job k with counts -> stdout
//   env NATIVE=1  run the native replacement (edge_cull.cpp) instead
#include "recomp/spu.h"
#include "recomp/spu_rt.h"
#include "recomp/edge_cull.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace gti::recomp;
namespace gti::recomp { extern uint64_t g_edgeCullCyc[4]; extern uint8_t* g_edgeCullWhy; extern uint64_t g_edgeCullBox; }
static uint8_t g_why[0x4000]; static uint64_t g_extraWhy[8];

static bool load(const char* fn, std::vector<uint8_t>& d) {
    FILE* f = std::fopen(fn, "rb");
    if (!f) return false;
    d.resize(Spu::LS_SIZE + 128 * 16);
    const size_t n = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    return n == d.size();
}
static void set_state(Spu& s, const std::vector<uint8_t>& d) {
    std::memcpy(s.ls, d.data(), Spu::LS_SIZE);
    for (int i = 0; i < 128; ++i)
        for (int k = 0; k < 4; ++k) {
            const uint8_t* p = d.data() + Spu::LS_SIZE + 16 * i + 4 * k;
            s.r[i].sw(k, uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
        }
}
static bool g_returned;
static uint32_t stop_at_return(spurt::Q* __restrict, Spu&, uint32_t pc) {
    if (pc == 0x4400) { g_returned = true; return 0; }   // outside the program: the dispatcher stops
    return pc;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: cull_replay <dir> [first] [count]\n"); return 1; }
    const int first = argc > 2 ? std::atoi(argv[2]) : 0, count = argc > 3 ? std::atoi(argv[3]) : 100000;
    const bool interp = std::getenv("INTERP") != nullptr;
    const int blocksJob = std::getenv("BLOCKS") ? std::atoi(std::getenv("BLOCKS")) : -1;
    const spurt::Compiled* cp = spurt::find_compiled(0xbaea80);
    if (!cp) { std::printf("program not compiled in\n"); return 1; }
    spurt::g_spuHle[(0xbaea80u >> 4) % 64] = stop_at_return;
    static Spu s;
    std::vector<uint8_t> in, out;
    int jobs = 0, bad = 0, notHandled = 0;
    uint64_t trisEdge = 0, trisOurs = 0, repVerts = 0;
    double repUs = 0;
    const bool native = std::getenv("NATIVE") != nullptr;
    double totUs = 0;
    for (int j = first; j < first + count; ++j) {
        char fi[512], fo[512];
        std::snprintf(fi, sizeof fi, "%s/cull_%04d_in.bin", argv[1], j);
        std::snprintf(fo, sizeof fo, "%s/cull_%04d_out.bin", argv[1], j);
        if (!load(fi, in) || !load(fo, out)) break;
        set_state(s, in);
        s.pc = 0x9e68;
        const auto t0 = std::chrono::steady_clock::now();
        if (native) {
            const uint32_t ctx = s.r[3].w(0), mode = s.r[4].w(0), r1 = s.r[1].w(0);
            g_edgeCullWhy = g_why;
            const EdgeCullResult cr = edge_cull_native(s.ls, ctx, mode, r1);
            g_edgeCullWhy = nullptr;
            if (const char* rep = std::getenv("REPEAT")) {   // REPEAT=n: time n more calls (on the already compacted list)
                const int n = std::atoi(rep);
                const auto r0 = std::chrono::steady_clock::now();
                for (int q = 0; q < n; ++q) edge_cull_native(s.ls, ctx, mode, r1);
                repUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - r0).count() / n;
                const uint32_t w2 = uint32_t(s.ls[(ctx & 0x3fff0) + 8]) << 24 | s.ls[(ctx & 0x3fff0) + 9] << 16;
                repVerts += w2 >> 16;
            }
            totUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            ++jobs;
            auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
            const uint32_t edgeCount = be32(out.data() + Spu::LS_SIZE + 3 * 16);
            if (!cr.handled) { ++notHandled; continue; }
            { static uint64_t vAll = 0, vCulled = 0, jCulled = 0; const uint32_t nv0 = uint32_t(in[(ctx & 0x3fff0) + 8]) << 8 | in[(ctx & 0x3fff0) + 9]; vAll += nv0; if (!cr.count) { vCulled += nv0; ++jCulled; }
              if (std::getenv("CULLSTAT") && j == first + count - 1) std::printf("fully culled: %llu jobs (box %llu), %llu of %llu vertices\n", (unsigned long long)jCulled, (unsigned long long)g_edgeCullBox, (unsigned long long)vCulled, (unsigned long long)vAll); }
            const uint32_t idx = be32(s.ls + (ctx & 0x3fff0) + 0x50);
            // ours must contain every console triangle, in order
            uint32_t k = 0; bool sup = true;
            for (uint32_t t = 0; t < edgeCount; t += 3) {
                const uint8_t* e = out.data() + idx + 2 * t;
                while (k < cr.count && std::memcmp(s.ls + idx + 2 * k, e, 6)) { ++g_extraWhy[g_why[k / 3] & 7]; k += 3; }
                if (k >= cr.count) { sup = false; break; }
                k += 3;
            }
            trisEdge += edgeCount / 3; trisOurs += cr.count / 3;
            if (!sup) { if (bad++ < 10) std::printf("job %04d: console triangle missing (console %u, ours %u)\n", j, edgeCount, cr.count); }
            else if (cr.count == edgeCount && (std::memcmp(s.ls + (ctx & 0x3fff0) + 0x58, out.data() + (ctx & 0x3fff0) + 0x58, 4) || std::memcmp(s.ls + r1 + 0x258, out.data() + r1 + 0x258, 4))) {
                if (bad++ < 10) std::printf("job %04d: same triangles but end pointer / count word differ\n", j);
            }
            continue;
        }
        if (interp || j == blocksJob) {
            std::map<uint32_t, uint64_t> blocks;
            uint32_t prev = 0;
            s.running = true;
            for (uint64_t k = 0; k < 50000000ull && s.pc != 0x4400; ++k) {
                if (j == blocksJob && s.pc != prev + 4) ++blocks[s.pc];
                prev = s.pc;
                if (!s.step()) break;
            }
            if (j == blocksJob) for (auto& kv : blocks) std::printf("block %05x %llu\n", kv.first, (unsigned long long)kv.second);
        } else {
            g_returned = false;
            cp->fn(s);
            if (g_returned) s.pc = 0x4400;
        }
        totUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ++jobs;
        // compare local store and registers with the capture
        int diffs = 0; uint32_t firstDiff = 0;
        for (uint32_t o = 0; o < Spu::LS_SIZE; o += 4)
            if (std::memcmp(s.ls + o, out.data() + o, 4)) { if (!diffs++) firstDiff = o; }
        int rdiff = -1;
        for (int i = 0; i < 128 && rdiff < 0; ++i)
            for (int k = 0; k < 4; ++k) {
                const uint8_t* p = out.data() + Spu::LS_SIZE + 16 * i + 4 * k;
                if (s.r[i].w(k) != (uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3])) { rdiff = i; break; }
            }
        if (s.pc != 0x4400 || diffs || rdiff >= 0) {
            if (bad++ < 10) std::printf("job %04d: pc %05x, %d LS words differ (first %05x), first register differing r%d\n", j, s.pc, diffs, firstDiff, rdiff);
        }
    }
    std::printf("%d jobs, %d mismatched, %.2f us per call\n", jobs, bad, jobs ? totUs / jobs : 0.0);
    if (repVerts) std::printf("repeat: %.2f us per call, %.1f vertices per job\n", repUs / jobs, double(repVerts) / jobs);
    if (native) std::printf("cycles per job: load %llu vertices %llu triangles %llu\n", (unsigned long long)(g_edgeCullCyc[0] / jobs), (unsigned long long)(g_edgeCullCyc[1] / jobs), (unsigned long long)(g_edgeCullCyc[2] / jobs));
    if (native) { std::printf("extra triangles by reason (0 passed, 1 near/no screen, 2 back face close, 4 snap):"); for (int q = 0; q < 8; ++q) if (g_extraWhy[q]) std::printf(" %d:%llu", q, (unsigned long long)g_extraWhy[q]); std::printf("\n"); }
    if (native) std::printf("native: %d not handled, triangles console %llu ours %llu (+%.2f%%)\n", notHandled, (unsigned long long)trisEdge, (unsigned long long)trisOurs, trisEdge ? 100.0 * (double(trisOurs) - double(trisEdge)) / double(trisEdge) : 0.0);
    return bad ? 2 : 0;
}
