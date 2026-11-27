#include "recomp/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void*, char*, unsigned long);
#endif

namespace gti::recomp {
namespace {
// next to the executable, whatever the current folder is (a shortcut or another launcher may start the game
// elsewhere, and the settings are read before main runs)
const std::string& ini_path() {
    static const std::string p = [] {
        std::string dir;
#ifdef _WIN32
        char exe[1024];
        const unsigned long n = GetModuleFileNameA(nullptr, exe, sizeof exe);
        if (n > 0 && n < sizeof exe) { dir.assign(exe, n); const size_t k = dir.find_last_of("\\/"); dir = k == std::string::npos ? std::string() : dir.substr(0, k + 1); }
#endif
        return dir + "gtiport.ini";
    }();
    return p;
}
struct Field { const char* name; int Settings::* p; int lo, hi; };
const Field kFields[] = {
    {"pantalla_completa", &Settings::fullscreen, 0, 2}, {"resolucion_interna", &Settings::scale, 1, 3}, {"imagenes_por_segundo", &Settings::gameHz, 60, 144},
    {"pantalla_120hz", &Settings::refresh, 0, 1},        {"idioma", &Settings::language, 1, 5},          {"contador_fps", &Settings::showFps, 0, 1},
    {"vsync", &Settings::vsync, 0, 1},
    {"sonido", &Settings::sound, 0, 1},
    {"antialiasing", &Settings::fxaa, 0, 1},
    {"nitidez", &Settings::sharpen, 0, 3},
    {"modo_directo", &Settings::streaming, 0, 1},
    {"filtrado_anisotropo", &Settings::aniso, 0, 4},
    {"camara", &Settings::camera, 0, 16},
    {"renderizador", &Settings::renderer, 0, 2},
    {"iconos_botones", &Settings::buttons, 0, 3},
    {"online", &Settings::online, 0, 1},
};
}

Settings& settings() {
    static Settings s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (FILE* f = std::fopen(ini_path().c_str(), "r")) {
            char line[128];
            while (std::fgets(line, sizeof line, f)) {
                char* eq = std::strchr(line, '=');
                if (!eq) continue;
                *eq = 0;
                for (const Field& fd : kFields)
                    if (!std::strcmp(line, fd.name)) s.*fd.p = std::clamp(std::atoi(eq + 1), fd.lo, fd.hi);
                auto text = [&](const char* key, char* dst, size_t cap) {   // text values: up to the end of the line, printable ASCII
                    if (std::strcmp(line, key)) return;
                    size_t k = 0;
                    for (const char* q = eq + 1; *q && *q != '\r' && *q != '\n' && k + 1 < cap; ++q) if (*q >= 32 && *q < 127) dst[k++] = *q;
                    dst[k] = 0;
                };
                text("online_nombre", s.onlineName, sizeof s.onlineName);
                text("online_ip", s.onlineIp, sizeof s.onlineIp);
            }
            std::fclose(f);
        }
        if (s.gameHz != 72 && s.gameHz != 120 && s.gameHz != 144) s.gameHz = 60;
        if (const char* e = std::getenv("GAMEHZ")) if (std::atoi(e) > 0) s.gameHz = std::atoi(e);
        if (const char* e = std::getenv("ESCALA")) s.scale = std::clamp(std::atoi(e), 1, 3);
    }
    return s;
}

void settings_save() {
    const Settings& s = settings();
    FILE* f = std::fopen(ini_path().c_str(), "w");
    if (!f) return;
    for (const Field& fd : kFields) std::fprintf(f, "%s=%d\n", fd.name, s.*fd.p);
    std::fprintf(f, "online_nombre=%s\nonline_ip=%s\n", s.onlineName, s.onlineIp);
    std::fclose(f);
}
} // namespace gti::recomp
