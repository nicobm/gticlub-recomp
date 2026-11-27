// Boots the whole recompiled game natively from its ELF entry point (no emulator, no snapshot).
// usage: recomp_boot <EBOOT.elf> <USRDIR> [--trace] [--seconds N]
#include "recomp/guest.h"
#include "recomp/lv2.h"
#include "recomp/hle.h"
#include "recomp/rsx_gl.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

using namespace gti::recomp;
namespace gti::recomp { void host_restore_refresh(); }
namespace gti::recomp { extern bool g_shadowOn; void rc_dump_all_shadows(); extern uint32_t g_watchLo, g_watchHi; }

int main(int argc, char** argv) {
    // Started without arguments (double click on the game's executable): the game files are looked for next to the
    // project like the launcher script does, the game runs until its window is closed, the log goes to
    // recomp_log.txt and no console window stays open.
    std::string elfArg, usrArg;
    int seconds = 20;
    if (argc < 3) {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(argv[0]).parent_path();
#ifdef _WIN32
        { wchar_t exe[1024]; if (GetModuleFileNameW(nullptr, exe, 1024)) dir = std::filesystem::path(exe).parent_path(); }
#endif
        if (!dir.empty()) std::filesystem::current_path(dir, ec);
        // installed layout: juego/EBOOT.elf and juego/USRDIR next to the executable; else the development tree
        elfArg = "juego/EBOOT.elf"; usrArg = "juego/USRDIR";
        if (!std::filesystem::exists(elfArg, ec) || !std::filesystem::exists(usrArg, ec)) { elfArg = "../re/EBOOT.elf"; usrArg = "../../NPEB00060/USRDIR"; }
        if (!std::filesystem::exists(elfArg, ec) || !std::filesystem::exists(usrArg, ec)) {
#ifdef _WIN32
            MessageBoxW(nullptr, L"No encuentro los ficheros del juego.\n\nHacen falta juego\\EBOOT.elf y la carpeta juego\\USRDIR junto a este ejecutable (los crea el instalador a partir de tu copia del juego).", L"Racing Club+", MB_ICONERROR);
#endif
            std::printf("usage: recomp_boot EBOOT.elf USRDIR [--trace] [--seconds N]\n");
            return 1;
        }
        seconds = 0;
#ifdef _WIN32
        FreeConsole();   // first: started from a console (a .bat with "start"), stderr is that console's handle
#endif
        // the log: the file takes over descriptor 2 itself (freopen alone left an empty file when the game was
        // started from a .bat: the writes kept going to the console's handle)
        if (FILE* lf = std::fopen(std::getenv("RECOMPLOG") ? std::getenv("RECOMPLOG") : "recomp_log.txt", "w")) {
            std::fflush(stderr);
            _dup2(_fileno(lf), 2);
#ifdef _WIN32
            SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
#endif
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);   // the log survives however the game ends
    } else { elfArg = argv[1]; usrArg = argv[2]; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    Lv2Config cfg;
    cfg.usrdir = usrArg;
    cfg.hdd0 = (std::filesystem::path(argv[0]).parent_path() / "dev_hdd0").string();
    if (argc < 3) cfg.hdd0 = "dev_hdd0";
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--trace")) cfg.trace = true;
        else if (!std::strcmp(argv[i], "--chain")) g_shadowOn = true;
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc) { g_watchLo = uint32_t(std::strtoul(argv[++i], nullptr, 16)); g_watchHi = g_watchLo + 4; }
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pad") && i + 1 < argc) cfg.padScript = argv[++i];
    }
    std::filesystem::create_directories(cfg.hdd0);
    static Guest g;
    if (!g.init(elfArg.c_str())) { std::printf("cannot load %s\n", elfArg.c_str()); return 1; }
    lv2_install(g, cfg);
    // the main thread runs on its own host thread so this one can stop the test after a while
    std::thread mt([] { const int rc = lv2_run_main(g); std::fprintf(stderr, "[boot] main finished (%d)\n", rc); });
    mt.detach();
    if (const char* fc = std::getenv("FRAMECLOCKS")) {   // FRAMECLOCKS=t1,t2,t3 (with FPSCAP=t2,t3,ms): find floats of the heap
        // that advance by +-1/60 per frame in both intervals (per-frame clocks), then watch who writes each (debug)
        double t[3] = {};
        std::sscanf(fc, "%lf,%lf,%lf", &t[0], &t[1], &t[2]);
        std::thread([t] {
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t base = 0x30000000u, size = 0x0D500000u;
            std::vector<uint8_t> s[3]; unsigned long long fl[3]; double ts[3];
            for (int k = 0; k < 3; ++k) {
                std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(t[k] * 1e6)));
                fl[k] = lv2_flip_count(); ts[k] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                s[k].assign(g.mem + base, g.mem + base + size);
            }
            static const bool perSec = std::getenv("FRAMECLOCKS_SEC") != nullptr;   // look for real-time clocks (+-1/s) instead
            const double n1 = perSec ? ts[1] - ts[0] : double(fl[1] - fl[0]), n2 = perSec ? ts[2] - ts[1] : double(fl[2] - fl[1]);
            std::vector<uint32_t> hits;
            const auto f = [](const std::vector<uint8_t>& v, uint32_t o) { uint32_t u; std::memcpy(&u, v.data() + o, 4); u = __builtin_bswap32(u); float x; std::memcpy(&x, &u, 4); return x; };
            static const bool ints = std::getenv("FRAMECLOCKS_INT") != nullptr;   // small int counters, +-1 per frame
            const auto iv = [](const std::vector<uint8_t>& v, uint32_t o) { uint32_t u; std::memcpy(&u, v.data() + o, 4); return int64_t(int32_t(__builtin_bswap32(u))); };
            for (uint32_t o = 0; ints && o < size; o += 4) {
                const int64_t a = iv(s[0], o), b = iv(s[1], o), c = iv(s[2], o);
                if (a < -5000 || a > 5000 || c < -5000 || c > 5000) continue;
                for (const int k : {1, -1})
                    if (std::llabs(b - a - k * int64_t(n1)) <= 1 && std::llabs(c - b - k * int64_t(n2)) <= 1) { hits.push_back(base + o); std::fprintf(stderr, "[clock] int 0x%x: %lld %lld %lld\n", base + o, (long long)a, (long long)b, (long long)c); }
            }
            for (uint32_t o = 0; !ints && o < size; o += 4) {
                const float a = f(s[0], o), b = f(s[1], o), c = f(s[2], o);
                if (!(std::fabs(a) < 1e5f && std::fabs(b) < 1e5f && std::fabs(c) < 1e5f)) continue;
                const double p1 = (double(b) - a) / n1, p2 = (double(c) - b) / n2;
                static const bool any = std::getenv("FRAMECLOCKS_ANY") != nullptr;   // any steady per-frame rate (no watch)
                if (any) { if (std::fabs(a) < 1000 && p1 != 0 && std::fabs(p1 - p2) < 0.03 * std::fabs(p1)) { const double q1 = (double(b) - a) / (ts[1] - ts[0]), q2 = (double(c) - b) / (ts[2] - ts[1]); if (std::fabs(q1 - q2) > 0.3 * std::fabs(q1)) std::fprintf(stderr, "[anyclock] 0x%x: %g %g %g (%+.6f/frame)\n", base + o, a, b, c, p1); } continue; }
                static const double rate = std::getenv("FRAMECLOCKS_RATE") ? std::atof(std::getenv("FRAMECLOCKS_RATE")) : 0;   // only this rate, value > 0
                if (rate != 0) { if (a > -1e4f && std::fabs(p1 - rate) < std::fabs(rate) * 0.1 && std::fabs(p2 - rate) < std::fabs(rate) * 0.1) { hits.push_back(base + o); std::fprintf(stderr, "[clock] 0x%x: %g %g %g\n", base + o, a, b, c); } continue; }
                for (const double w : {perSec ? 1.0 : 1.0 / 60, perSec ? -1.0 : -1.0 / 60})
                    if (std::fabs(p1 - w) < std::fabs(w) * 0.02 && std::fabs(p2 - w) < std::fabs(w) * 0.02) { hits.push_back(base + o); std::fprintf(stderr, "[clock] 0x%x: %g %g %g (%+.5f/frame)\n", base + o, a, b, c, p1); }
            }
            std::fprintf(stderr, "[clock] %zu per-frame clocks (frames %g, %g)\n", hits.size(), n1, n2);
            if (const char* pw = std::getenv("FRAMECLOCKS_DEREF")) {   // FRAMECLOCKS_DEREF=p,o: watch *(hit + p) + o instead (debug)
                int p = 0, o = 0; std::sscanf(pw, "%d,%d", &p, &o);
                std::vector<uint32_t> keep;
                for (uint32_t h : hits) {
                    uint32_t v; std::memcpy(&v, g.mem + h + p, 4); v = __builtin_bswap32(v);
                    if (v < 0x30000000u || v >= 0x3D500000u) continue;
                    if (std::find(keep.begin(), keep.end(), v + o) == keep.end()) { keep.push_back(v + o); std::fprintf(stderr, "[clock] watching 0x%x (from 0x%x)\n", v + o, h); }
                }
                hits = keep;
            }
            for (size_t i = 0; i < hits.size() && i < 8; ++i) {   // watch each for half a second
                g_watchLo = hits[i]; g_watchHi = hits[i] + 4;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                g_watchHi = 0; g_watchLo = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::fprintf(stderr, "[clock] done\n");
        }).detach();
    }
    if (const char* pk = std::getenv("POKE")) {   // POKE=seconds:hexaddr=float,hexaddr=float...: write floats then (debug)
        std::string spec(pk);
        std::thread([spec] {
            const double t = std::atof(spec.c_str());
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(t * 1e6)));
            size_t p = spec.find(':');
            while (p != std::string::npos && p + 1 < spec.size()) {
                unsigned a = 0; float v = 0;
                if (std::sscanf(spec.c_str() + p + 1, "%x=%f", &a, &v) == 2) {
                    uint32_t u; std::memcpy(&u, &v, 4); u = __builtin_bswap32(u); std::memcpy(g.mem + a, &u, 4);
                    std::fprintf(stderr, "[poke] 0x%x = %g\n", a, v);
                }
                p = spec.find(',', p + 1);
            }
        }).detach();
    }
    if (const char* wa = std::getenv("WATCHAT")) {   // WATCHAT=seconds,hexaddr[,bytes]: start the write watch later (debug)
        double t = 0; unsigned a = 0, n = 4;
        std::sscanf(wa, "%lf,%x,%u", &t, &a, &n);
        std::thread([t, a, n] { std::this_thread::sleep_for(std::chrono::microseconds(int64_t(t * 1e6))); g_watchLo = a; g_watchHi = a + n; std::fprintf(stderr, "[watch] armed at 0x%x\n", a); }).detach();
    }
    if (const char* ms = std::getenv("MEMSNAP")) {   // MEMSNAP=t1,t2,...: guest data + heap to memsnap_N.bin at those seconds (debug)
        std::thread([ms] {
            const auto t0 = std::chrono::steady_clock::now();
            int n = 0;
            for (const char* p = ms; *p; ++n) {
                const double t = std::atof(p);
                std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(t * 1e6)));
                const unsigned long long f = lv2_flip_count();
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                char fn[64]; std::snprintf(fn, sizeof fn, "memsnap_%d.bin", n);
                if (FILE* o = std::fopen(fn, "wb")) {
                    std::fwrite(g.mem + 0x00800000, 1, 0x01800000, o);   // 0x00800000..0x02000000: ELF data, statics
                    std::fwrite(g.mem + 0x30000000, 1, 0x0D500000, o);   // 0x30000000..0x3D500000: game heap
                    std::fclose(o);
                }
                std::fprintf(stderr, "[memsnap] %d: %.3f s, flip %llu\n", n, now, f);
                const char* c = std::strchr(p, ','); if (!c) break; p = c + 1;
            }
        }).detach();
    }
    if (std::getenv("SAMPLE")) {   // sample every thread's call chain twice a second
        for (int k = 0; k < seconds * 2; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); rc_dump_all_shadows(); }
    } else if (seconds <= 0) {   // --seconds 0: play until the window is closed
        // a game that shows no new image for 10 s is stuck: write what every thread waits for to the log (once)
        unsigned long long lastFlips = 0; int still = 0; bool dumped = false;
        while (!rsx::quit_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const unsigned long long f = lv2_flip_count();
            if (f != lastFlips) { lastFlips = f; still = 0; continue; }
            if (f && ++still == 100 && !dumped) { dumped = true; std::fprintf(stderr, "[boot] no image for 10 s: state dump\n"); lv2_dump_waits(); std::fflush(stderr); }
        }
        gti::recomp::host_restore_refresh(); std::_Exit(0);
    } else
        for (int k = 0; k < seconds * 10 && !rsx::quit_requested(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::fprintf(stderr, "[boot] %d s elapsed, stopping\n", seconds);
    if (g_shadowOn) rc_dump_all_shadows();
    hle_dump_counts();
    lv2_dump_waits();
    gti::recomp::host_restore_refresh(); std::_Exit(0);
}
