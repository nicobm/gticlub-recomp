#include "recomp/ppu.h"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <vector>
#include <mutex>
#include <thread>
#include <map>
#include <cmath>
#include <atomic>
#include <cstring>
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);

namespace gti::recomp {

HostHook g_hostHook = nullptr;
uint64_t g_missLr = 0;

float g_frameStep = 1.0f;
uint32_t g_worldEa = 0, g_physCalls = 0;
// The game's logic scheduler adds the image time (at least 1/60 s) to an accumulator and ticks the logic while it is
// not negative; the physics world (called later in the same image) steps 1/60 s whenever real time allows. At 60
// images per second both happen in every image. Above that the logic ticked every image (too fast), and with its
// own clock it ticked on other images than the physics: objects placed by the logic (tyres, pots, chairs) trembled
// against the camera, which follows the physics. This returns an accumulator that makes the logic tick as often as
// the world is about to step in this image (same formula as the world's scheduler at 0x31b788); where the world is
// not running (menus, pause) the logic keeps its own 60 Hz clock. Original behaviour at 60 Hz (g_frameStep == 1).
// the world's step this file wrote last (1/60 = the game's own): the image rate can change while the game runs
static float g_worldStepSet = 1.0f / 60.0f;
static bool world_step_ours(float v) { return std::fabs(v - 1.0f / 60.0f) < 1e-5f || std::fabs(v - g_worldStepSet) < 1e-5f; }
double rc_logic_acc(PPU& c, double dtClamped, double acc) {
    if (g_frameStep == 1.0f) {
        if (g_worldStepSet != 1.0f / 60.0f && g_worldEa && world_step_ours(c.rdf32(g_worldEa + 156))) c.wrf32(g_worldEa + 156, 1.0f / 60.0f);   // back to 60 (options menu)
        g_worldStepSet = 1.0f / 60.0f;
        return double(float(dtClamped + acc));
    }
    static uint32_t lastCalls = 0;
    static float own = 0;
    const float S = c.rdf32(uint32_t(c.r[25]) + 712);            // the scheduler's tick length
    const uint32_t tm = c.rd32(c.rd32(c.rd32(0xc6f210 + 9716)) + 204);   // time object: image time +188, time scale +224
    const float dt = c.rdf32(tm + 188);
    int n = 0;
    // The world steps once per image (1/rate s) so every image shows new motion, and the logic keeps its own 60 Hz clock.
    // PHYSHZ=0: the world keeps 1/60 s steps and the logic ticks with them (images between steps repeat, loose objects flicker).
    static const bool physHz = !(std::getenv("PHYSHZ") && std::getenv("PHYSHZ")[0] == '0');
    if (physHz && g_physCalls != lastCalls && g_worldEa) {
        // One world step per image, of the real time that image took: 1/rate while the game keeps the rate, 2/rate
        // when an image comes late (never longer than the console's 1/60). Before, the step was always 1/rate, so
        // whenever the images per second dropped (144 setting, 85-130 fps in races) the cars and physics went slower
        // than the logic, the clock and the sound: the game felt sped up against its own cars. WORLDFIX=0: old.
        static const bool fixed = std::getenv("WORLDFIX") && std::getenv("WORLDFIX")[0] == '0';
        static double wacc = 0;
        const double base = double(g_frameStep) / 60.0;
        int k = 1;
        if (!fixed) {
            const int kmax = std::max(1, int(1.0 / (60.0 * base) + 1e-6));   // 144 and 120: 2, 72: 1
            wacc += dt > 0 && dt < 0.1f ? double(dt) : 0.0;
            k = std::clamp(int(wacc / base + 0.3), 1, kmax);   // +0.3: an image a bit early still counts as one
            wacc -= k * base;
            if (wacc > 2 * kmax * base || wacc < -2 * base) wacc = 0;   // a load or a pause: nothing to catch up
        }
        lastCalls = g_physCalls;
        if (world_step_ours(c.rdf32(g_worldEa + 156))) { g_worldStepSet = float(k * base); c.wrf32(g_worldEa + 156, g_worldStepSet); }
    }
    if (!physHz && g_physCalls != lastCalls && g_worldEa) {
        lastCalls = g_physCalls;
        const float step = float(c.rdf32(g_worldEa + 156) * c.rdf32(tm + 224));
        if (step > 1e-6f) n = int(float(dt + c.rdf32(g_worldEa + 160)) / step);
        own = 0;
    } else {
        own += dt > 0 && dt < 0.1f ? dt : (dt >= 0.1f ? 0.1f : 0.0f);
        n = int(own * 60.0f);
        own -= float(n) / 60.0f;
    }
    if (n < 0) n = 0;
    if (n > 6) n = 6;
    // SPEEDSTAT=1: every 5 s of real time, how much game time went by (must stay at 60 logic ticks, 1.00 s of image
    // time and 1.00 s of world time per second whatever the image rate)
    static const bool speedStat = std::getenv("SPEEDSTAT") != nullptr;
    if (speedStat) {
        static auto t0 = std::chrono::steady_clock::now();
        static int ticks = 0, images = 0; static double dts = 0, world = 0; static uint32_t pc0 = 0;
        ticks += n; ++images; dts += dt;
        if (g_worldEa) world += double(g_physCalls - pc0) * c.rdf32(g_worldEa + 156) * c.rdf32(tm + 224);
        pc0 = g_physCalls;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el >= 5) {
            std::fprintf(stderr, "[speed] per second: %.1f images, %.1f logic ticks, %.3f s image time, %.3f s world time (step %.5f)%c", images / el, ticks / el, dts / el, world / el, g_worldEa ? c.rdf32(g_worldEa + 156) : 0.0f, 10);
            t0 = std::chrono::steady_clock::now(); ticks = images = 0; dts = world = 0;
        }
    }
    return n > 0 ? double((float(n) - 0.5f) * S) : double(-0.5f * (S > 0 ? S : 1.0f / 60.0f));
}
uint64_t g_timebaseFixed = 0;   // >0: deterministic time base, advances this many ticks per read
uint64_t rc_timebase() {
    if (g_timebaseFixed) { static uint64_t t = 1; t += g_timebaseFixed; return t; }
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const double s = duration<double>(steady_clock::now() - t0).count();
    {   // TBPROF=a,b: host return addresses of the time base reads between those seconds, printed at b (debug)
        static double a = -1, b = -1; static bool parsed = false, done = false;
        static std::mutex m; static std::map<uintptr_t, uint64_t> hist;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("TBPROF")) std::sscanf(e, "%lf,%lf", &a, &b); }
        if (a >= 0 && !done && s >= a) {
            std::lock_guard<std::mutex> lk(m);
            if (s < b) ++hist[reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))];
            else if (!done) { done = true; for (auto& [k, v] : hist) std::fprintf(stderr, "[tbprof] %llx %llu\n", (unsigned long long)k, (unsigned long long)v); }
        }
    }
    return uint64_t(s * 79800000.0) + 1;
}
uint32_t g_watchLo = 0, g_watchHi = 0;
// current chain of recompiled functions, per host thread (watch reports, crash reports, hang diagnosis)
struct ShadowReg { std::mutex m; std::vector<std::pair<std::thread::id, std::vector<uint32_t>*>> all; };
static ShadowReg& shadow_reg() { static ShadowReg r; return r; }
static std::vector<uint32_t>& shadow_vec() {
    thread_local std::vector<uint32_t>* v = nullptr;
    if (!v) { v = new std::vector<uint32_t>(); v->reserve(256); std::lock_guard<std::mutex> lk(shadow_reg().m); shadow_reg().all.push_back({std::this_thread::get_id(), v}); }
    return *v;
}
#define g_shadow shadow_vec()
void rc_dump_all_shadows() {   // racy snapshot of every thread's call chain (debug only)
    std::lock_guard<std::mutex> lk(shadow_reg().m);
    for (auto& t : shadow_reg().all) {
        std::vector<uint32_t> v = *t.second;
        std::printf("[thread chain] depth %zu:", v.size());
        for (size_t i = v.size() > 24 ? v.size() - 24 : 0; i < v.size(); ++i) std::printf(" %x", v[i]);
        std::printf("\n");
    }
    std::fflush(stdout);
}
bool g_shadowOn = false;
bool g_traceCalls = false;   // print every recompiled function entered (debug)                 // keep the chain also without a watch (crash reports)
void rc_dump_shadow() {
    std::printf("[chain] depth %zu:", g_shadow.size());
    const size_t n = g_shadow.size();
    for (size_t i = n > 40 ? n - 40 : 0; i < n; ++i) std::printf(" %x", g_shadow[i]);
    std::printf("\n");
    std::fflush(stdout);
}

void watch_hit(PPU& c, uint32_t addr, int size) {
    std::printf("[watch] write %d bytes at 0x%x  lr=0x%llx  chain:", size, addr, (unsigned long long)c.lr);
    const size_t n = g_shadow.size();
    for (size_t i = n > 12 ? n - 12 : 0; i < n; ++i) std::printf(" %x", g_shadow[i]);
    std::printf("\n");
}
SyscallHook g_syscallHook = nullptr;

void rc_syscall(PPU& c) {
    if (g_syscallHook) { g_syscallHook(c); return; }
    c.unimpl(0xFFFF0000u | uint32_t(c.r[11]));   // marks which syscall number was needed
}

// Fast dispatch: a flat table from code address to recompiled function, built on the first call. Every call of the
// game goes through rc_call (tens of millions per second), and the map lookup of the host hook plus a binary search
// over the ~30000 functions were a tenth of the main thread. Addresses with a host hook keep the slow path.
static std::vector<uint32_t>& hooked_list() { static std::vector<uint32_t> v; return v; }
// CALLERS=a1,a2,...[@seconds]: count the guest return addresses (lr) of calls to those functions, printed every 3 s
// after the given second (debug: finds who calls a virtual or indirectly called function)
static std::vector<uint32_t>& callers_list() {
    static std::vector<uint32_t> v = [] {
        std::vector<uint32_t> r;
        if (const char* e = std::getenv("CALLERS")) for (const char* p = e; *p && *p != '@';) { r.push_back(uint32_t(std::strtoul(p, nullptr, 16))); const char* c = std::strpbrk(p, ",@"); if (!c || *c == '@') break; p = c + 1; }
        return r;
    }();
    return v;
}
static void callers_note(uint32_t addr, uint32_t lr) {
    static std::mutex m; static std::map<uint64_t, uint64_t> h;
    static const double from = [] { const char* e = std::getenv("CALLERS"); const char* at = e ? std::strchr(e, '@') : nullptr; return at ? std::atof(at + 1) : 0.0; }();
    static const auto t0 = std::chrono::steady_clock::now();
    static double last = 0;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (s < from) return;
    std::lock_guard<std::mutex> lk(m);
    ++h[uint64_t(addr) << 32 | lr];
    if (s - last > 3) { last = s; for (auto& [k, v] : h) std::fprintf(stderr, "[callers] %.1f s: %x <- lr %x: %llu\n", s, uint32_t(k >> 32), uint32_t(k), (unsigned long long)v); h.clear(); }
}
void rc_mark_hooked(uint32_t addr) { hooked_list().push_back(addr); }
namespace {
struct FastTable {
    uint32_t lo = 0, hi = 0;
    std::vector<Fn> fn;   // nullptr: not a function start, or hooked
    FastTable() {
        if (!kFuncCount) return;
        lo = kFuncs[0].addr; hi = kFuncs[kFuncCount - 1].addr + 4;
        for (size_t k = 0; k < kFuncCount; ++k) { lo = std::min(lo, kFuncs[k].addr); hi = std::max(hi, kFuncs[k].addr + 4); }
        fn.assign((hi - lo) / 4, nullptr);
        for (size_t k = 0; k < kFuncCount; ++k) if (!(kFuncs[k].addr & 3)) fn[(kFuncs[k].addr - lo) / 4] = kFuncs[k].fn;
        for (uint32_t a : hooked_list()) if (a >= lo && a < hi) fn[(a - lo) / 4] = nullptr;
        for (uint32_t a : callers_list()) if (a >= lo && a < hi) fn[(a - lo) / 4] = nullptr;
    }
};
}
void rc_call(PPU& c, uint32_t addr) {
    if (c.missing || c.unimplAt) return;          // stop executing once something went wrong
    static const FastTable ft;
    static const bool slowOnly = std::getenv("RCSLOWCALL") != nullptr;
    if (!slowOnly && addr - ft.lo < ft.hi - ft.lo && !g_shadowOn && !g_traceCalls && g_watchHi <= g_watchLo) {
        if (const Fn f = ft.fn[(addr - ft.lo) >> 2]) { f(c); return; }
    }
    if (g_hostHook && g_hostHook(c, addr)) return;
    const FuncEntry* b = kFuncs;
    const FuncEntry* e = kFuncs + kFuncCount;
    const FuncEntry* it = std::lower_bound(b, e, addr, [](const FuncEntry& f, uint32_t a) { return f.addr < a; });
    if (it != e && it->addr == addr) {
        {   // BODYLOG=seconds (with CALLERS=31bab8): per World::update call, the moving bodies of the world: vtable,
            // position +0x50 (render root), +0x90 (centre of mass), speed (debug)
            static const double from = std::getenv("BODYLOG") ? std::atof(std::getenv("BODYLOG")) : -1;
            static const auto t0 = std::chrono::steady_clock::now(); static int n = 0;
            if (from >= 0 && addr == 0x31bab8 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= from && n < 400) {
                ++n;
                const uint32_t w = uint32_t(c.r[3]), arr = c.rd32(w + 256), cnt = c.rd32(w + 260);
                std::fprintf(stderr, "[body] call %d: %u bodies, remainder %g\n", n, cnt, double(c.rdf32(w + 160)));
                if (n == 1) for (uint32_t k = 0; k < cnt && k < 8; ++k) {   // the systems of the world: vtable and (pointer, count) pairs that look like arrays
                    const uint32_t sy = c.rd32(arr + 4 * k);
                    std::fprintf(stderr, "[body] system %u at %x vt %x:", k, sy, c.rd32(sy));
                    for (uint32_t o = 4; o < 0x200; o += 4) {
                        const uint32_t pp = c.rd32(sy + o), nn = c.rd32(sy + o + 4);
                        if (pp >= 0x30000000u && pp < 0x3D500000u && nn > 0 && nn < 5000) { const uint32_t e0 = c.rd32(pp); if (e0 >= 0x30000000u && e0 < 0x3D500000u) std::fprintf(stderr, " +%x:(%x,%u,vt %x)", o, pp, nn, c.rd32(e0)); }
                    }
                    std::fprintf(stderr, "\n");
                }
                for (uint32_t k = 0; k < cnt && k < 4000; ++k) {
                    const uint32_t b = c.rd32(arr + 4 * k);
                    const float vx = c.rdf32(b + 0x140), vy = c.rdf32(b + 0x144), vz = c.rdf32(b + 0x148);
                    const float sp = std::sqrt(vx * vx + vy * vy + vz * vz);
                    if (sp > 1.0f && sp < 200.0f) std::fprintf(stderr, "[body]   %u vt %x p50 %.4f %.4f %.4f p90 %.4f %.4f %.4f v %.2f\n", k, c.rd32(b), double(c.rdf32(b + 0x50)), double(c.rdf32(b + 0x54)), double(c.rdf32(b + 0x58)), double(c.rdf32(b + 0x90)), double(c.rdf32(b + 0x94)), double(c.rdf32(b + 0x98)), double(sp));
                }
            }
        }
        if (!callers_list().empty() && std::find(callers_list().begin(), callers_list().end(), addr) != callers_list().end()) callers_note(addr, uint32_t(c.lr)); {
            static const double argsFrom = std::getenv("CALLARGS") ? std::atof(std::getenv("CALLARGS")) : -1;   // CALLARGS=seconds: r3, f1 and floats at r3+156/+160 of 300 calls after that
            static const auto t0 = std::chrono::steady_clock::now(); static std::atomic<int> n{0};
            if (argsFrom >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= argsFrom && n++ < 300)
                std::fprintf(stderr, "[callargs] %x r3=%x f1=%g [+156]=%g [+160]=%g lr=%x\n", addr, uint32_t(c.r[3]), c.f[1], double(c.rdf32(uint32_t(c.r[3]) + 156)), double(c.rdf32(uint32_t(c.r[3]) + 160)), uint32_t(c.lr));
        }
        if (g_traceCalls) std::printf("[fn] %x r3=%llx r4=%llx r5=%llx r6=%llx\n", addr, (unsigned long long)c.r[3], (unsigned long long)c.r[4], (unsigned long long)c.r[5], (unsigned long long)c.r[6]);
        const bool sh = g_shadowOn || g_watchHi > g_watchLo;
        if (sh) g_shadow.push_back(addr);
        it->fn(c);
        if (sh && !g_shadow.empty()) g_shadow.pop_back();
        return;
    }
    c.missing = addr;
    g_missLr = c.lr;   // where the untranslated target was called from
    if (g_shadowOn) { std::printf("[missing 0x%x] ", addr); rc_dump_shadow(); }
}

} // namespace gti::recomp
