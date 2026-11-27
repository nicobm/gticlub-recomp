#pragma once
// Internal interface between the RSX front end (rsx_core.cpp: method stream and register state, microcode ->
// shader translation, vertex and index conversion, window, input, options menu, notifications) and the drawing
// back ends (rsx_gl.cpp: OpenGL 4.5; rsx_d3d11.cpp: Direct3D 11). Everything runs on the RSX command thread.
#include "recomp/rsx_gl.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gti::recomp::rsx {

extern std::mutex g_logMx;
#define RLOG(...) do { std::lock_guard<std::mutex> lk_(g_logMx); std::fprintf(stderr, __VA_ARGS__); } while (0)

// ------------------------------------------------------------------ register state (rsx_core.cpp)
extern Config C;
extern uint32_t R[0x4000];                     // last value written to every NV4097 method (index = method / 4)
extern uint32_t vpCode[512 * 4];               // transform program memory
extern float vpConst[468 * 4];                 // transform constants
extern bool vpConstDirty;
extern uint32_t vpDirtyLo, vpDirtyHi;          // floats of vpConst changed since the last upload
extern uint64_t vpGen;                         // bumped whenever vertex program words are uploaded
struct Range { uint32_t first, count; };
extern std::vector<Range> ranges;              // the draw's DRAW_ARRAYS / DRAW_INDEX_ARRAY ranges
extern bool rangesIndexed;
extern uint32_t prim;
struct Display { uint32_t offset = 0, pitch = 0, w = 0, h = 0; };
extern Display displays[8];
extern SDL_Window* win;
extern uint64_t frameNo, drawsThisFrame;
extern const bool dumpShaders;

inline uint32_t be32(uint32_t ea) { uint32_t v; std::memcpy(&v, C.mem + ea, 4); return __builtin_bswap32(v); }
inline uint16_t be16(uint32_t ea) { return uint16_t(C.mem[ea] << 8 | C.mem[ea + 1]); }
inline float f32(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
inline float rf(uint32_t m) { return f32(R[m >> 2]); }
inline uint32_t ea_of(uint32_t offset, uint32_t location /*0 local, 1 main*/) { return (location ? C.ioAddr : C.localAddr) + offset; }
float half2f(uint16_t h);
// keys used inside this process only (inline: called many times per draw with a few bytes)
inline uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; std::memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// work counters (counters(), [slowdraw]); the back ends add to them
extern uint64_t g_texUploads, g_texUploadBytes, g_streamBuilds, g_streamVerts, g_progBuilds;
extern std::atomic<uint64_t> g_swapUs;
extern uint64_t g_drawTicks, g_drawCount, g_progTicks, g_texTicks, g_convTicks, g_bufTicks;
struct TickScope { uint64_t& acc; uint64_t t0; explicit TickScope(uint64_t& a) : acc(a), t0(SDL_GetPerformanceCounter()) {} ~TickScope() { acc += SDL_GetPerformanceCounter() - t0; } };

// ------------------------------------------------------------------ shaders (microcode -> GLSL, rsx_core.cpp)
// `glsl` is the complete GLSL program; `body` only the translated instructions (what goes inside main), which the
// HLSL back end wraps in its own declarations.
struct VpInfo { std::string glsl, body; uint32_t inputs = 0; uint32_t outputs = 0; int len = 0; bool unsupported = false; };
struct FpInfo { std::string glsl, body; uint32_t texUsed = 0; uint32_t size = 0; uint32_t nConst = 0; bool unsupported = false; bool writesR0 = false, writesH0 = false; };
VpInfo translate_vp(const uint32_t* code, uint32_t start);
FpInfo translate_fp(uint32_t ea, uint32_t texDims, uint32_t control);   // texDims: 2 bits per sampler (texDimsOf)
uint64_t fp_walk(uint32_t ea, uint64_t h, std::vector<float>* consts);
uint32_t fp_size(uint32_t ea);                 // bytes of the fragment program, inline constants included
uint32_t texDimsOf(int i);                     // 0 = 2D, 1 = cube, 2 = 3D, 3 = depth compare (shadow map)
uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh);
uint32_t ilog2(uint32_t v);

// ------------------------------------------------------------------ vertices and indices (rsx_core.cpp)
void bswap32_block(uint32_t* dst, const uint8_t* src, size_t n);
void gather_i16(uint32_t* d, const uint8_t* q, uint32_t n, uint32_t add);
void index_range(const uint32_t* v, size_t n, uint32_t& lo, uint32_t& hi1);
uint32_t attr_comps(int a);
void convert_attr(int a, uint32_t first, uint32_t count, float* out, uint32_t comps = 4);

// ------------------------------------------------------------------ drawing back end
struct Overlay {           // a top-down BGRA bitmap shown over the image (options menu, notification)
    const uint32_t* px = nullptr;
    int w = 0, h = 0;
    bool changed = false;  // new content since the last present (upload it)
    int dx = 0, dy = 0, dw = 0, dh = 0;   // window rectangle, origin top left
};
struct Backend {
    virtual ~Backend() = default;
    virtual uint32_t window_flags() = 0;   // SDL window flags it needs
    virtual bool init() = 0;               // after the window exists
    virtual void new_frame() = 0;          // called at every flip, before present
    virtual void clear(uint32_t mask) = 0; // NV4097_CLEAR_SURFACE
    virtual void draw() = 0;               // SET_BEGIN_END(0) with the current state and ranges
    // the display buffer d into the window, then the overlays (index 0 notification, 1 menu; px null = hidden)
    virtual void present(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) = 0;
    virtual void set_vsync(bool on) = 0;
    virtual void textures_changed() = 0;   // an option that changes sampler state (anisotropy)
    virtual const char* gpu_name() = 0;
    virtual const char* api_name() = 0;     // "OpenGL", "DirectX 11" (window title, problem report)
};
Backend* make_gl_backend();
Backend* make_d3d11_backend();
Backend* make_vulkan_backend();   // nullptr-free: init() fails when Direct3D 11 is not available
void gather_draw_indices(std::vector<uint32_t>& idx, uint32_t& vmin, uint32_t& vmax, bool triFans);
extern Backend* B;

} // namespace gti::recomp::rsx
