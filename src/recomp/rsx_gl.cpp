// RSX -> OpenGL 4.5 back end (see rsx_core.h).
#include "recomp/rsx_core.h"
#include "recomp/settings.h"
#include "recomp/lv2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <immintrin.h>

namespace gti::recomp::rsx {
namespace {

// ------------------------------------------------------------------ OpenGL loader (only what this file uses)
typedef unsigned int GLenum; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef int GLint;
typedef int GLsizei; typedef unsigned int GLuint; typedef float GLfloat; typedef double GLdouble; typedef char GLchar;
typedef ptrdiff_t GLsizeiptr; typedef ptrdiff_t GLintptr; typedef struct __GLsync* GLsync; typedef unsigned long long GLuint64;
enum : GLenum {
    GL_NONE = 0, GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6,
    GL_DEPTH_BUFFER_BIT = 0x100, GL_STENCIL_BUFFER_BIT = 0x400, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_FRONT_AND_BACK = 0x408, GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11, GL_POLYGON_OFFSET_FILL = 0x8037, GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_3D = 0x806F, GL_TEXTURE_CUBE_MAP = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
    GL_HALF_FLOAT = 0x140B, GL_RED = 0x1903, GL_GREEN = 0x1904, GL_BLUE = 0x1905, GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_RG = 0x8227,
    GL_BGRA = 0x80E1, GL_DEPTH_COMPONENT = 0x1902, GL_DEPTH_STENCIL = 0x84F9, GL_UNSIGNED_INT_24_8 = 0x84FA, GL_UNSIGNED_INT_8_8_8_8 = 0x8035,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_SHORT_4_4_4_4_REV = 0x8365, GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_TEXTURE_WRAP_R = 0x8072, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F,
    GL_CLAMP_TO_BORDER = 0x812D, GL_MIRRORED_REPEAT = 0x8370, GL_MIRROR_CLAMP_TO_EDGE = 0x8743, GL_TEXTURE_MIN_LOD = 0x813A, GL_TEXTURE_MAX_LOD = 0x813B,
    GL_TEXTURE_BASE_LEVEL = 0x813C, GL_TEXTURE_MAX_LEVEL = 0x813D, GL_TEXTURE_SWIZZLE_RGBA = 0x8E46, GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_TEXTURE_MAX_ANISOTROPY = 0x84FE, GL_TEXTURE0 = 0x84C0, GL_ZERO = 0, GL_ONE = 1,
    GL_RGBA8 = 0x8058, GL_SRGB8_ALPHA8 = 0x8C43, GL_R8 = 0x8229, GL_RG8 = 0x822B, GL_R16 = 0x822A, GL_RG16 = 0x822C, GL_RGBA16F = 0x881A,
    GL_RGBA32F = 0x8814, GL_R32F = 0x822E, GL_RG16F = 0x822F, GL_RGB565 = 0x8D62, GL_RGBA4 = 0x8056, GL_RGB5_A1 = 0x8057,
    GL_DEPTH24_STENCIL8 = 0x88F0, GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT = 0x8C4D, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT = 0x8C4E, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8A11, GL_STREAM_DRAW = 0x88E0, GL_DYNAMIC_DRAW = 0x88E8,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84,
    GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00, GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_LOWER_LEFT = 0x8CA1, GL_ZERO_TO_ONE = 0x935F, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_CW = 0x0900, GL_CCW = 0x0901, GL_FRAMEBUFFER_SRGB = 0x8DB9, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_DEBUG_OUTPUT = 0x92E0,
    GL_DEPTH_CLAMP = 0x864F, GL_PROGRAM_POINT_SIZE = 0x8642,
    GL_MAP_WRITE_BIT = 0x2, GL_MAP_PERSISTENT_BIT = 0x40, GL_MAP_COHERENT_BIT = 0x80, GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_SYNC_FLUSH_COMMANDS_BIT = 0x1,
};
#define RSX_GL(X) \
    X(const unsigned char*, glGetString, GLenum) X(void, glEnable, GLenum) X(void, glDisable, GLenum) X(void, glViewport, GLint, GLint, GLsizei, GLsizei) \
    X(void, glScissor, GLint, GLint, GLsizei, GLsizei) X(void, glClear, GLbitfield) X(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glClearDepth, GLdouble) X(void, glClearStencil, GLint) X(void, glColorMaski, GLuint, GLboolean, GLboolean, GLboolean, GLboolean) \
    X(void, glDepthMask, GLboolean) X(void, glDepthFunc, GLenum) X(void, glStencilFuncSeparate, GLenum, GLenum, GLint, GLuint) \
    X(void, glStencilOpSeparate, GLenum, GLenum, GLenum, GLenum) X(void, glStencilMaskSeparate, GLenum, GLuint) \
    X(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum) X(void, glBlendEquationSeparate, GLenum, GLenum) \
    X(void, glBlendColor, GLfloat, GLfloat, GLfloat, GLfloat) X(void, glCullFace, GLenum) X(void, glFrontFace, GLenum) \
    X(void, glPolygonOffset, GLfloat, GLfloat) X(void, glClipControl, GLenum, GLenum) X(void, glGenTextures, GLsizei, GLuint*) \
    X(void, glDeleteTextures, GLsizei, const GLuint*) X(void, glBindTexture, GLenum, GLuint) X(void, glTexParameteri, GLenum, GLenum, GLint) \
    X(void, glTexParameterf, GLenum, GLenum, GLfloat) X(void, glTexParameteriv, GLenum, GLenum, const GLint*) X(void, glTexParameterfv, GLenum, GLenum, const GLfloat*) \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, glCompressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) \
    X(void, glActiveTexture, GLenum) X(void, glPixelStorei, GLenum, GLint) X(void, glGenFramebuffers, GLsizei, GLuint*) \
    X(void, glBindFramebuffer, GLenum, GLuint) X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    X(void, glDrawBuffers, GLsizei, const GLenum*) X(GLenum, glCheckFramebufferStatus, GLenum) \
    X(void, glCopyTexSubImage2D, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) \
    X(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) \
    X(void, glDeleteFramebuffers, GLsizei, const GLuint*) X(void, glGenBuffers, GLsizei, GLuint*) X(void, glBindBuffer, GLenum, GLuint) \
    X(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum) X(void, glBufferSubData, GLenum, GLintptr, GLsizeiptr, const void*) \
    X(void, glBufferStorage, GLenum, GLsizeiptr, const void*, GLbitfield) X(void*, glMapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield) \
    X(GLsync, glFenceSync, GLenum, GLbitfield) X(GLenum, glClientWaitSync, GLsync, GLbitfield, GLuint64) X(void, glDeleteSync, GLsync) X(void, glBindBufferBase, GLenum, GLuint, GLuint) \
    X(void, glBindBufferRange, GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) \
    X(void, glGenVertexArrays, GLsizei, GLuint*) X(void, glBindVertexArray, GLuint) \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) X(void, glEnableVertexAttribArray, GLuint) \
    X(void, glDisableVertexAttribArray, GLuint) X(void, glVertexAttrib4f, GLuint, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glDrawElements, GLenum, GLsizei, GLenum, const void*) X(void, glDrawElementsBaseVertex, GLenum, GLsizei, GLenum, const void*, GLint) X(void, glDrawArrays, GLenum, GLint, GLsizei) \
    X(GLuint, glCreateShader, GLenum) X(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*) X(void, glCompileShader, GLuint) \
    X(void, glGetShaderiv, GLuint, GLenum, GLint*) X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) X(GLuint, glCreateProgram) \
    X(void, glProgramBinary, GLuint, GLenum, const void*, GLsizei) X(void, glGetProgramBinary, GLuint, GLsizei, GLsizei*, GLenum*, void*) \
    X(void, glAttachShader, GLuint, GLuint) X(void, glLinkProgram, GLuint) X(void, glGetProgramiv, GLuint, GLenum, GLint*) \
    X(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) X(void, glUseProgram, GLuint) X(GLint, glGetUniformLocation, GLuint, const GLchar*) \
    X(void, glUniform4fv, GLint, GLsizei, const GLfloat*) X(void, glUniform1i, GLint, GLint) X(void, glDeleteShader, GLuint) X(GLenum, glGetError) X(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) X(void, glGetIntegerv, GLenum, GLint*) X(GLboolean, glIsEnabled, GLenum) \
    X(void, glGenQueries, GLsizei, GLuint*) X(void, glQueryCounter, GLuint, GLenum) X(void, glGetQueryObjectiv, GLuint, GLenum, GLint*) \
    X(void, glGetQueryObjectui64v, GLuint, GLenum, uint64_t*) X(void, glGetInteger64v, GLenum, int64_t*)
#define RSX_DECL(ret, name, ...) typedef ret (*PFN_##name)(__VA_ARGS__); static PFN_##name name = nullptr;
RSX_GL(RSX_DECL)
#undef RSX_DECL


static SDL_GLContext glc = nullptr;
static bool glOk = false;
static GLuint vao = 0, vbo = 0, ibo = 0, ubo = 0;
static std::vector<float> vtx;
static std::vector<uint32_t> idx;

// ------------------------------------------------------------------ programs
// With GL_KHR_parallel_shader_compile the driver links (and finishes cached binaries) on its own threads: a new
// program is started here and its draws are skipped until GL_COMPLETION_STATUS_KHR says it is done, instead of
// stalling the frame 5-30 ms per program (first play, or a driver update that emptied the driver's own cache).
// GLSYNC=1: wait as before.
struct Program { uint64_t key = 0; GLuint prog = 0; GLint uVs, uVo, uSurf, uTsc, uAlpha, uWpos, uFc; uint32_t nConst = 0; uint32_t vpInputs = 0; uint32_t texUsed = 0; bool bad = false;
                 bool pending = false; int kind = 0; /* 1 binary from glcache, 2 compiled */ GLuint vsSh = 0, fsSh = 0; std::string vsSrc, fsSrc, cacheName; uint64_t t0 = 0; };
typedef void (*PFN_glMaxShaderCompilerThreadsKHR)(GLuint);
static PFN_glMaxShaderCompilerThreadsKHR glMaxShaderCompilerThreadsKHR = nullptr;
static bool g_parallel = false;
static uint64_t g_skippedDraws = 0;
static std::unordered_map<uint64_t, Program> programs;

static GLuint compile(GLenum type, const std::string& src, const char* what, bool async = false) {
    const GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    if (async) return s;   // the link reports errors (checked when it completes)
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        RLOG("[rsx] %s shader compile error:\n%s\n--- source ---\n%s\n", what, log, src.c_str());
    }
    return s;
}

// completes a program started by get_program: false while the driver is still working on it (wait = false)
static bool finish_program(Program& p, bool wait) {
    if (!wait) { GLint done = 0; glGetProgramiv(p.prog, 0x91B1 /* GL_COMPLETION_STATUS_KHR */, &done); if (!done) return false; }
    const uint64_t pt1 = SDL_GetPerformanceCounter();
    GLint ok = 0;
    glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    while (glGetError()) {}
    const bool fromCache = p.kind == 1 && ok;
    if (!ok && p.kind == 1) {   // the driver rejected the cached binary (other driver version): compile, waiting this once
        p.vsSh = compile(GL_VERTEX_SHADER, p.vsSrc, "vertex"); p.fsSh = compile(GL_FRAGMENT_SHADER, p.fsSrc, "fragment");
        glAttachShader(p.prog, p.vsSh); glAttachShader(p.prog, p.fsSh);
        glLinkProgram(p.prog);
        glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
        p.kind = 2;
    }
    if (ok && p.kind == 2 && !p.cacheName.empty()) {
        GLint len = 0;
        glGetProgramiv(p.prog, 0x8741 /* GL_PROGRAM_BINARY_LENGTH */, &len);
        if (len > 0) {
            std::vector<uint8_t> bin(size_t(len) + 4);
            GLsizei got = 0; GLenum fmt = 0;
            glGetProgramBinary(p.prog, len, &got, &fmt, bin.data() + 4);
            const uint32_t f32v = fmt; std::memcpy(bin.data(), &f32v, 4);
            if (got > 0) if (FILE* f = std::fopen(p.cacheName.c_str(), "wb")) { std::fwrite(bin.data(), 1, size_t(got) + 4, f); std::fclose(f); }
        }
    }
    if (!ok) {
        char log[4096]; GLsizei ln = 0;
        glGetProgramInfoLog(p.prog, sizeof log, &ln, log);
        RLOG("[rsx] link error: %s\n", log);
        if (p.vsSh) { GLint c = 0; glGetShaderiv(p.vsSh, GL_COMPILE_STATUS, &c); if (!c) { glGetShaderInfoLog(p.vsSh, sizeof log, &ln, log); RLOG("[rsx] vertex shader: %s\n", log); } }
        if (p.fsSh) { GLint c = 0; glGetShaderiv(p.fsSh, GL_COMPILE_STATUS, &c); if (!c) { glGetShaderInfoLog(p.fsSh, sizeof log, &ln, log); RLOG("[rsx] fragment shader: %s\n", log); } }
        p.bad = true;
    }
    glDeleteShader(p.vsSh); glDeleteShader(p.fsSh); p.vsSh = p.fsSh = 0;
    p.vsSrc.clear(); p.vsSrc.shrink_to_fit(); p.fsSrc.clear(); p.fsSrc.shrink_to_fit();
    p.uVs = glGetUniformLocation(p.prog, "u_vscale");
    p.uVo = glGetUniformLocation(p.prog, "u_voffset");
    p.uSurf = glGetUniformLocation(p.prog, "u_surf");
    p.uTsc = glGetUniformLocation(p.prog, "u_tsc");
    p.uAlpha = glGetUniformLocation(p.prog, "u_alpha");
    p.uWpos = glGetUniformLocation(p.prog, "u_wpos");
    p.uFc = glGetUniformLocation(p.prog, "fc");
    p.pending = false;
    {   // [prog]: a program that cost the frame time (waiting) or took long to be ready (parallel)
        const uint64_t now = SDL_GetPerformanceCounter(), fq = SDL_GetPerformanceFrequency();
        const double here = double(now - pt1) * 1000.0 / double(fq), total = double(now - p.t0) * 1000.0 / double(fq);
        if (wait ? total > 2.0 : here > 2.0) RLOG("[prog] %016llx %.1f ms in this frame (%s)\n", (unsigned long long)p.key, wait ? total : here, fromCache ? "cache" : "compiled");
        else if (!wait && total > 50.0) RLOG("[prog] %016llx ready %.0f ms after first use (%s, in parallel)\n", (unsigned long long)p.key, total, fromCache ? "cache" : "compiled");
    }
    return true;
}
static std::vector<float> g_fpConsts;   // constants of the fragment program of the current draw (filled by get_program)
static Program* get_program(uint32_t vpStart, uint32_t fpEa, uint32_t texDims, uint32_t control) {
    // vertex program words up to the end bit
    uint32_t n = 0;
    while (vpStart + n < 512) { ++n; if (vpCode[(vpStart + n - 1) * 4 + 3] & 1) break; }
    static uint64_t vpKeyGen = 0, vpKey = 0; static uint32_t vpKeyStart = ~0u;
    if (vpKeyGen != vpGen || vpKeyStart != vpStart) { vpKey = fnv(vpCode + vpStart * 4, n * 16); vpKeyGen = vpGen; vpKeyStart = vpStart; }
    uint64_t key = vpKey;
    {   // fragment program: hash of its instruction words + its inline constants. Memo per address: when the bytes
        // are unchanged (one memcmp) the walk is skipped; the game patches constants in place, which only costs a re-walk.
        struct FpMemo { std::vector<uint8_t> raw; uint64_t h = 0; std::vector<float> consts; };
        static std::unordered_map<uint32_t, FpMemo> memo;
        static const bool off = std::getenv("RSXNOFPMEMO") != nullptr;
        FpMemo& fm = memo[fpEa];
        if (off || fm.raw.empty() || std::memcmp(C.mem + fpEa, fm.raw.data(), fm.raw.size()) != 0) {
            fm.consts.clear();
            fm.h = fp_walk(fpEa, 0x51ed270b3c6f84d5ull, &fm.consts);
            const uint32_t sz = fp_size(fpEa);
            fm.raw.assign(C.mem + fpEa, C.mem + fpEa + sz);
        }
        g_fpConsts = fm.consts;
        key = (key ^ fm.h) * 0x9E3779B97F4A7C15ull ^ fm.h;
    }
    key ^= (uint64_t(texDims) << 1) ^ (uint64_t(control & 0x4e) << 40) ^ uint64_t(vpStart) * 0x9E3779B97F4A7C15ull;
    auto it = programs.find(key);
    if (it != programs.end()) return &it->second;
    Program& p = programs[key];
    p.key = key;
    ++g_progBuilds;
    TickScope progTime(g_progTicks);
    const uint64_t pt0 = SDL_GetPerformanceCounter();
    const VpInfo vi = translate_vp(vpCode, vpStart);
    const FpInfo fi = translate_fp(fpEa, texDims, control);
    p.vpInputs = vi.inputs;
    p.texUsed = fi.texUsed;
    p.nConst = fi.nConst;
    if (dumpShaders) {
        static int nShader = 0;
        char fn[128];
        std::snprintf(fn, sizeof fn, "rsxdump/prog_%04d_%016llx.glsl", nShader++, (unsigned long long)key);
        if (FILE* f = std::fopen(fn, "wb")) { std::fprintf(f, "// VP start %u len %d\n%s\n// FP %x size %u\n%s", vpStart, vi.len, vi.glsl.c_str(), fpEa, fi.size, fi.glsl.c_str()); std::fclose(f); }
    }
    if (vi.unsupported || fi.unsupported) RLOG("[rsx] program %016llx uses unsupported instructions (vp %d fp %d)\n", (unsigned long long)key, vi.unsupported, fi.unsupported);
    // Linked programs are kept on disk (glcache/): compiling one takes tens of milliseconds, a visible hitch the
    // first time an effect appears. The file name carries the hash of the generated GLSL, so a change in the
    // translator makes new files; a driver that rejects a binary just compiles again. RSXNOGLCACHE=1 turns it off.
    static const bool binCache = std::getenv("RSXNOGLCACHE") == nullptr;
    static bool dirMade = false;
    if (binCache && !dirMade) { dirMade = true; SDL_CreateDirectory("glcache"); }
    char cacheName[96];
    std::snprintf(cacheName, sizeof cacheName, "glcache/%016llx.bin", (unsigned long long)(fnv(vi.glsl.data(), vi.glsl.size(), fnv(fi.glsl.data(), fi.glsl.size()))));
    const uint64_t pt1 = SDL_GetPerformanceCounter();
    p.prog = glCreateProgram();
    p.vsSrc = vi.glsl; p.fsSrc = fi.glsl; p.cacheName = binCache ? cacheName : ""; p.t0 = pt0;
    if (binCache)
        if (FILE* f = std::fopen(cacheName, "rb")) {
            std::vector<uint8_t> bin;
            uint8_t buf[65536]; size_t k;
            while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) bin.insert(bin.end(), buf, buf + k);
            std::fclose(f);
            if (bin.size() > 4) {
                uint32_t fmt; std::memcpy(&fmt, bin.data(), 4);
                glProgramBinary(p.prog, fmt, bin.data() + 4, GLsizei(bin.size() - 4));
                while (glGetError()) {}
                p.kind = 1;
            }
        }
    if (!p.kind) {
        p.vsSh = compile(GL_VERTEX_SHADER, vi.glsl, "vertex", g_parallel); p.fsSh = compile(GL_FRAGMENT_SHADER, fi.glsl, "fragment", g_parallel);
        glAttachShader(p.prog, p.vsSh);
        glAttachShader(p.prog, p.fsSh);
        glLinkProgram(p.prog);
        p.kind = 2;
    }
    p.pending = true;
    (void)pt1;
    if (!g_parallel) finish_program(p, true);
    static int nProg = 0;
    if (++nProg % 50 == 0) RLOG("[rsx] %d programs compiled\n", nProg);
    return &p;
}

// ------------------------------------------------------------------ surfaces (render targets)
struct Surface { GLuint tex = 0; uint32_t w = 0, h = 0, fmt = 0; bool depth = false; uint64_t lastUse = 0; };
static std::map<uint64_t, Surface> surfaces;   // key: location<<32 | offset (+ depth flag)
static std::map<uint64_t, GLuint> fbos;

static void surface_gl_format(uint32_t fmt, GLenum& ifmt, GLenum& f, GLenum& ty) {
    switch (fmt) {
        case 3: ifmt = GL_RGB565; f = GL_RGB; ty = GL_UNSIGNED_SHORT_5_6_5; break;
        case 9: ifmt = GL_R8; f = GL_RED; ty = GL_UNSIGNED_BYTE; break;
        case 0xa: ifmt = GL_RG8; f = GL_RG; ty = GL_UNSIGNED_BYTE; break;
        case 0xb: ifmt = GL_RGBA16F; f = GL_RGBA; ty = GL_HALF_FLOAT; break;
        case 0xc: ifmt = GL_RGBA32F; f = GL_RGBA; ty = GL_FLOAT; break;
        case 0xd: ifmt = GL_R32F; f = GL_RED; ty = GL_FLOAT; break;
        default: ifmt = GL_RGBA8; f = GL_RGBA; ty = GL_UNSIGNED_BYTE; break;
    }
}

// Bumped whenever texture bindings made by bind_texture() may be stale: a surface (render target) appears or
// changes, a texture is created, re-uploaded or deleted, or other code binds textures (menu, cube copies).
static uint64_t g_texGen = 1;
static Surface& get_surface(uint32_t loc, uint32_t off, uint32_t w, uint32_t h, uint32_t fmt, bool depth) {
    const uint64_t key = (uint64_t(loc) << 33) | (uint64_t(depth) << 32) | off;
    const bool fresh = surfaces.find(key) == surfaces.end();
    Surface& s = surfaces[key];
    if (fresh) ++g_texGen;
    if (!s.tex || s.w != w || s.h != h || s.fmt != fmt) {
        // The game reuses an address with different sizes within a frame (the depth buffer of the 256x256 cube
        // faces and of the 2560x720 scene share one): the texture of each size is kept and comes back, instead of
        // being destroyed and created again several times per frame.
        struct Parked { uint64_t key; uint32_t w, h, fmt; GLuint tex; };
        static std::vector<Parked> parked;
        if (s.tex) parked.push_back({key, s.w, s.h, s.fmt, s.tex});
        s.tex = 0;
        for (size_t k = 0; k < parked.size(); ++k)
            if (parked[k].key == key && parked[k].w == w && parked[k].h == h && parked[k].fmt == fmt) { s.tex = parked[k].tex; parked.erase(parked.begin() + k); break; }
        if (s.tex) { s.w = w; s.h = h; s.fmt = fmt; s.depth = depth; s.lastUse = frameNo; return s; }
        glGenTextures(1, &s.tex);
        ++g_texGen;   // bound on the active unit below
        glBindTexture(GL_TEXTURE_2D, s.tex);
        if (depth) {
            if (fmt == 1) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
            else glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        } else {
            GLenum ifmt, f, ty;
            surface_gl_format(fmt, ifmt, f, ty);
            glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, f, ty, nullptr);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        s.w = w; s.h = h; s.fmt = fmt; s.depth = depth;
        RLOG("[rsx] surface %s at %s 0x%x: %ux%u format %u\n", depth ? "depth" : "color", loc ? "main" : "local", off, w, h, fmt);
    }
    s.lastUse = frameNo;
    return s;
}

struct Target { GLuint fbo = 0; uint32_t w = 0, h = 0, aaX = 1, aaY = 1, logW = 0, logH = 0; int nColor = 0; };
static Target cur;

static void aa_factors(uint32_t aa, uint32_t& x, uint32_t& y) {
    switch (aa) { case 3: x = 2; y = 1; break; case 4: case 5: x = 2; y = 2; break; default: x = 1; y = 1; break; }
}

static bool bind_target() {
    const uint32_t fmtReg = R[0x208 >> 2];
    const uint32_t cfmt = fmtReg & 0x1f, zfmt = (fmtReg >> 5) & 7, aa = (fmtReg >> 12) & 0xf;
    const uint32_t clipW = R[0x200 >> 2] >> 16, clipH = R[0x204 >> 2] >> 16;
    const uint32_t clipX = R[0x200 >> 2] & 0xffff, clipY = R[0x204 >> 2] & 0xffff;
    uint32_t ax, ay;
    aa_factors(aa, ax, ay);
    if (clipX + clipW == 1280 && clipY + clipH == 720) { const uint32_t sc = uint32_t(settings().scale); ax *= sc; ay *= sc; }   // internal resolution: the full-screen surfaces only
    const uint32_t w = (clipX + clipW) * ax, h = (clipY + clipH) * ay;
    if (!w || !h) return false;
    const uint32_t ct = R[0x220 >> 2];
    static const uint32_t offReg[4] = {0x210, 0x218, 0x288, 0x28c}, dmaReg[4] = {0x194, 0x18c, 0x1b4, 0x1b8};
    int list[4], nList = 0;
    switch (ct & 0x1f) {
        case 0: break;
        case 1: list[nList++] = 0; break;
        case 2: list[nList++] = 1; break;
        case 0x13: list[nList++] = 0; list[nList++] = 1; break;
        case 0x17: list[nList++] = 0; list[nList++] = 1; list[nList++] = 2; break;
        case 0x1f: for (int k = 0; k < 4; ++k) list[nList++] = k; break;
        default: list[nList++] = 0; break;
    }
    GLuint texs[5] = {};
    uint64_t key = (uint64_t(w) << 48) ^ (uint64_t(h) << 32) ^ fmtReg;
    for (int k = 0; k < nList; ++k) {
        const int i = list[k];
        const uint32_t loc = R[dmaReg[i] >> 2] & 1;
        const uint32_t off = R[offReg[i] >> 2];
        texs[k] = get_surface(loc, off, w, h, cfmt, false).tex;
        key = fnv(&off, 4, key ^ (i + 1));
    }
    const bool hasZ = zfmt != 0;
    if (hasZ) {
        const uint32_t loc = R[0x198 >> 2] & 1, off = R[0x214 >> 2];
        texs[4] = get_surface(loc, off, w, h, zfmt, true).tex;
        key = fnv(&off, 4, key ^ 0x77);
    }
    key = fnv(texs, sizeof texs, key);
    GLuint& fbo = fbos[key];
    if (!fbo) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        GLenum bufs[4];
        for (int k = 0; k < nList; ++k) { glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + k, GL_TEXTURE_2D, texs[k], 0); bufs[k] = GL_COLOR_ATTACHMENT0 + k; }
        if (hasZ) glFramebufferTexture2D(GL_FRAMEBUFFER, zfmt == 1 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texs[4], 0);
        glDrawBuffers(nList, nList ? bufs : nullptr);
        if (!nList) { GLenum none = GL_NONE; glDrawBuffers(1, &none); }
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) RLOG("[rsx] framebuffer incomplete 0x%x (colors %d, z %u, %ux%u)\n", st, nList, zfmt, w, h);
    } else glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    cur.fbo = fbo; cur.w = w; cur.h = h; cur.aaX = ax; cur.aaY = ay; cur.logW = clipX + clipW; cur.logH = clipY + clipH; cur.nColor = nList;
    glViewport(0, 0, w, h);
    return true;
}

static void apply_scissor() {
    // RSX scissor and viewport rectangle both clip (logical pixels; scaled by the AA factors)
    uint32_t sx = R[0x8c0 >> 2] & 0xffff, sw = R[0x8c0 >> 2] >> 16, sy = R[0x8c4 >> 2] & 0xffff, sh = R[0x8c4 >> 2] >> 16;
    const uint32_t vx = R[0xa00 >> 2] & 0xffff, vw = R[0xa00 >> 2] >> 16, vy = R[0xa04 >> 2] & 0xffff, vh = R[0xa04 >> 2] >> 16;
    if (sw == 0 && sh == 0) { sw = 4096; sh = 4096; }
    const int x0 = std::max(sx, vx), y0 = std::max(sy, vy);
    const int x1 = std::min(sx + sw, vx + vw), y1 = std::min(sy + sh, vy + vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * cur.aaX, y0 * cur.aaY, std::max(0, x1 - x0) * cur.aaX, std::max(0, y1 - y0) * cur.aaY);
}

static bool stateValid = false;   // false after anything else touched the GL state apply_state() sets
static void clear_surface(uint32_t mask) {
    if (!bind_target()) return;
    stateValid = false;
    apply_scissor();
    GLbitfield bits = 0;
    {   // RSXSTEP=frame: the main surface starts that frame in magenta, so every draw's contribution shows
        static const long stepFrame = std::getenv("RSXSTEP") ? std::atol(std::getenv("RSXSTEP")) : -1;
        if (long(frameNo) == stepFrame && cur.w == 2560 && !(mask & 0xf0)) {
            for (int k = 0; k < 4; ++k) glColorMaski(k, 1, 1, 1, 1);
            glClearColor(1, 0, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    if (mask & 0xf0) {
        const uint32_t c = R[0x1d90 >> 2];
        glClearColor(((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, (c >> 24) / 255.f);
        for (int k = 0; k < 4; ++k) glColorMaski(k, (mask >> 5) & 1, (mask >> 6) & 1, (mask >> 7) & 1, (mask >> 4) & 1);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    const uint32_t zv = R[0x1d8c >> 2];
    const uint32_t zfmt = (R[0x208 >> 2] >> 5) & 7;
    if (mask & 1) {
        glDepthMask(1);
        glClearDepth(zfmt == 1 ? (zv & 0xffff) / 65535.0 : (zv >> 8) / 16777215.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if ((mask & 2) && zfmt == 2) { glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xff); glClearStencil(zv & 0xff); bits |= GL_STENCIL_BUFFER_BIT; }
    if (bits) glClear(bits);
}

// ------------------------------------------------------------------ textures
struct Tex { GLuint id = 0; uint64_t sig = 0; uint64_t lastUse = 0; GLenum target = GL_TEXTURE_2D; uint64_t regSig = 0, checked = ~0ull, sampler = ~0ull; };
static std::unordered_map<uint64_t, Tex> texCache;


struct TexFmt { GLenum ifmt, fmt, type; uint32_t bpp; bool dxt; uint32_t blockBytes; int swap; };   // swap: 0 none, 2 u16, 4 u32
static bool tex_format(uint32_t f, TexFmt& o) {
    switch (f) {
        case 0x81: o = {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, 0, 0}; return true;
        case 0x82: o = {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, false, 0, 2}; return true;
        case 0x83: o = {GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, false, 0, 2}; return true;
        case 0x84: o = {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, false, 0, 2}; return true;
        case 0x85: case 0x9e: o = {GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, 4, false, 0, 0}; return true;
        case 0x86: o = {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, true, 8, 0}; return true;
        case 0x87: o = {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, true, 16, 0}; return true;
        case 0x88: o = {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, true, 16, 0}; return true;
        case 0x8b: o = {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, 0, 0}; return true;
        case 0x94: o = {GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2, false, 0, 2}; return true;
        case 0x95: o = {GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4, false, 0, 2}; return true;
        case 0x9a: o = {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, 0, 2}; return true;
        case 0x9b: o = {GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, false, 0, 4}; return true;
        case 0x9c: o = {GL_R32F, GL_RED, GL_FLOAT, 4, false, 0, 4}; return true;
        case 0x9f: o = {GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false, 0, 2}; return true;
        default: return false;
    }
}

static GLenum wrap_of(uint32_t w) {
    switch (w) { case 1: return GL_REPEAT; case 2: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE; case 4: return GL_CLAMP_TO_BORDER;
                 case 5: return GL_CLAMP_TO_EDGE; case 6: case 8: return GL_MIRROR_CLAMP_TO_EDGE; default: return GL_CLAMP_TO_EDGE; }
}


// NV4097_SET_TEXTURE_BORDER_COLOR (A8R8G8B8): what CLAMP_TO_BORDER samples outside the texture, e.g. the white
// around a projected shadow that multiplies the road
static void apply_border_color(GLenum target, uint32_t base) {
    const uint32_t bc = R[(base + 0x1c) >> 2];
    const float c[4] = {((bc >> 16) & 0xff) / 255.f, ((bc >> 8) & 0xff) / 255.f, (bc & 0xff) / 255.f, (bc >> 24) / 255.f};
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, c);
}

static uint64_t g_memoHits = 0, g_memoMiss[4] = {};   // RSXPROF: texture-unit memo hits; misses: no memo, generation, age, registers
static bool g_bindMemoOk = false;   // set by bind_texture_impl when the binding is a plain cached texture
static void bind_texture_impl(int i, float* tsc);
// Per texture unit: the registers of its last binding. A draw that samples the same texture as the previous draw
// on that unit (most of them: >60000 draws per second) then costs nothing; the content check still runs every
// few frames because the memo expires.
static GLuint g_lastId = 0; static GLenum g_lastTarget = 0; static uint64_t g_lastSurf = 0;   // surface key | bit 63, 0 = plain texture   // what bind_texture_impl bound when g_bindMemoOk
// Bindings already worked out, keyed by the unit's registers (any unit): a draw that samples a texture seen before
// (most of the >60000 draws per second) only binds its GL texture, or nothing when the unit already holds it.
// Valid while no texture/surface changed (g_texGen) and for 4 frames (the content check), and only if the GL
// texture still carries the sampler state of these registers (another unit may have set other filters on it).
static void bind_texture(int i, float* tsc) {
    struct Fast { uint64_t gen, frame, surf; GLuint id; GLenum target; float tsc[4]; };
    static std::unordered_map<uint64_t, Fast> fast;
    static std::unordered_map<GLuint, uint64_t> paramsOf;   // GL texture -> registers its sampler state was set for
    static GLuint boundOn[16] = {};
    static uint64_t boundGen = 0;
    static const bool off = std::getenv("RSXNOTEXMEMO") != nullptr;
    if (boundGen != g_texGen) { std::memset(boundOn, 0, sizeof boundOn); boundGen = g_texGen; }
    const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
    uint32_t regs[10];   // 40 bytes: whole 8-byte rounds in fnv
    for (int k = 0; k < 8; ++k) regs[k] = R[(base >> 2) + k];
    regs[8] = R[(0x1840 + 4 * i) >> 2]; regs[9] = 0;
    const uint64_t h = fnv(regs, sizeof regs);
    if (!off) {
        auto it = fast.find(h);
        if (it != fast.end() && it->second.gen == g_texGen && frameNo - it->second.frame < 4) {
            const Fast& f = it->second;
            auto pit = paramsOf.find(f.id);
            bool ok = pit != paramsOf.end() && pit->second == h;
            if (ok && f.surf) { auto sit = surfaces.find(f.surf & ~(1ull << 63)); ok = sit != surfaces.end() && sit->second.tex == f.id; }
            if (ok) {
                if (boundOn[i] != f.id) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(f.target, f.id); boundOn[i] = f.id; }
                std::memcpy(tsc, f.tsc, sizeof f.tsc);
                ++g_memoHits;
                return;
            }
            ++g_memoMiss[3];
        } else ++g_memoMiss[it == fast.end() ? 0 : it->second.gen != g_texGen ? 1 : 2];
    }
    g_bindMemoOk = false; g_lastSurf = 0;
    bind_texture_impl(i, tsc);
    if (g_bindMemoOk && g_lastId) {
        Fast& f = fast[h];
        f.gen = g_texGen; f.frame = frameNo; f.surf = g_lastSurf; f.id = g_lastId; f.target = g_lastTarget; std::memcpy(f.tsc, tsc, sizeof f.tsc);
        paramsOf[g_lastId] = h;
        boundOn[i] = g_lastId;
        if (fast.size() > 20000) fast.clear();
    } else boundOn[i] = 0;   // unknown: bound by the slow path (cube, unsupported format...)
}
static void bind_texture_impl(int i, float* tsc) {
    const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
    const uint32_t off = R[base >> 2], fmtReg = R[(base + 4) >> 2], addr = R[(base + 8) >> 2], ctl0 = R[(base + 0xc) >> 2];
    const uint32_t remap = R[(base + 0x10) >> 2], filt = R[(base + 0x14) >> 2], rect = R[(base + 0x18) >> 2];
    const uint32_t ctl3 = R[(0x1840 + 4 * i) >> 2];
    const uint32_t loc = (fmtReg & 3) == 2 ? 1 : 0;
    const bool cube = fmtReg & 4;
    const uint32_t f = (fmtReg >> 8) & 0xff, baseFmt = f & ~0x60u;
    const bool ln = f & 0x20, un = f & 0x40;
    const uint32_t mips = std::max<uint32_t>(1, fmtReg >> 16);
    const uint32_t w = std::max<uint32_t>(1, rect >> 16), h = std::max<uint32_t>(1, rect & 0xffff);
    const uint32_t pitch = ctl3 & 0xfffff;
    tsc[0] = un ? 1.f / w : 1.f; tsc[1] = un ? 1.f / h : 1.f; tsc[2] = float((addr >> 28) & 7); tsc[3] = 1.f;   // z: depth compare function (shcmp)
    glActiveTexture(GL_TEXTURE0 + i);
    static const bool shadowLog = std::getenv("SHADOWLOG") != nullptr;   // (getenv per draw cost ~5% of the RSX thread)
    if (shadowLog && texDimsOf(i) == 3) {   // debug: shadow-map lookups
        static std::set<uint64_t> seen;
        const bool hasDepth = surfaces.count((uint64_t(loc) << 33) | (uint64_t(1) << 32) | off), hasColor = surfaces.count((uint64_t(loc) << 33) | off);
        if (seen.size() < 40) RLOG("[shadow] border color register %08x\n", R[(base + 0x1c) >> 2]);
        if (seen.insert(uint64_t(off) << 8 | uint64_t(i) | uint64_t((addr >> 28) & 7) << 40 | uint64_t(R[0x1d60 >> 2]) << 44).second && seen.size() < 40)
            RLOG("[shadow] unit %d: depth texture at %s 0x%x %ux%u fmt %02x func %u wrap %x: depth surface %s, color surface %s\n", i, loc ? "main" : "local", off, w, h, f, (addr >> 28) & 7, addr & 0xfff, hasDepth ? "found" : "missing", hasColor ? "found" : "missing");
    }
    // render target?
    for (int z = 0; z < 2; ++z) {
        auto it = surfaces.find((uint64_t(loc) << 33) | (uint64_t(z) << 32) | off);
        if (it != surfaces.end() && !cube) {
            glBindTexture(GL_TEXTURE_2D, it->second.tex);
            g_bindMemoOk = true; g_lastId = it->second.tex; g_lastTarget = GL_TEXTURE_2D; g_lastSurf = it->first | (1ull << 63);
            const GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
            const uint32_t mag = (filt >> 24) & 7;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_of(addr & 0xf));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_of((addr >> 8) & 0xf));
            apply_border_color(GL_TEXTURE_2D, base);
            return;
        }
    }
    if (cube) {   // cube map whose faces are render targets (the environment the cars and windows reflect)
        const uint32_t faceStride = (w * h * 4 + 127) & ~127u;
        const uint64_t key0 = (uint64_t(loc) << 33) | off;
        if (surfaces.find(key0) != surfaces.end()) {
            struct RenderedCube { GLuint id = 0; uint64_t built = ~0ull; uint32_t w = 0, h = 0; };
            static std::map<uint64_t, RenderedCube> rcubes;
            static GLuint copyFbo = 0;
            RenderedCube& rc = rcubes[key0];
            if (!rc.id || rc.w != w || rc.h != h) {
                ++g_texGen;
                if (rc.id) glDeleteTextures(1, &rc.id);
                glGenTextures(1, &rc.id);
                glBindTexture(GL_TEXTURE_CUBE_MAP, rc.id);
                for (int k = 0; k < 6; ++k) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + k, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                rc.w = w; rc.h = h; rc.built = ~0ull;
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP, rc.id);
            if (rc.built != frameNo) {   // once per frame: the faces are drawn before the scene that uses them
                rc.built = frameNo;
                if (!copyFbo) glGenFramebuffers(1, &copyFbo);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, copyFbo);
                for (uint32_t k = 0; k < 6; ++k) {
                    auto it = surfaces.find(key0 + uint64_t(k) * faceStride);
                    if (it == surfaces.end()) continue;
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.tex, 0);
                    glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + k, 0, 0, 0, 0, 0, GLsizei(std::min(w, it->second.w)), GLsizei(std::min(h, it->second.h)));
                }
                glBindFramebuffer(GL_READ_FRAMEBUFFER, cur.fbo);
            }
            const GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
            glTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, sw);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            return;
        }
    }
    TexFmt tf;
    if (!tex_format(baseFmt, tf)) {
        static std::map<uint32_t, int> warned;
        if (!warned[baseFmt]++) RLOG("[rsx] texture format 0x%x unsupported (unit %d)\n", baseFmt, i);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    const uint32_t ea = ea_of(off, loc);
    // size of the image data (all faces and levels)
    auto levelBytes = [&](uint32_t lw, uint32_t lh) -> uint32_t {
        if (tf.dxt) return std::max(1u, (lw + 3) / 4) * std::max(1u, (lh + 3) / 4) * tf.blockBytes;
        if (ln && pitch) return pitch * lh;
        return lw * lh * tf.bpp;
    };
    uint32_t faceBytes = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < mips; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) faceBytes += levelBytes(lw, lh);
    const uint32_t faces = cube ? 6 : 1;
    const uint32_t faceStride = cube ? (faceBytes + 127) & ~127u : faceBytes;
    const uint32_t total = faceStride * faces;
    if (uint64_t(ea) + total > 0x100000000ull) return;
    // signature: format registers + sampled content (the content is sampled once per frame and texture)
    const uint64_t key = (uint64_t(loc) << 32 | off) ^ (uint64_t(cube) << 63);
    Tex& tx = texCache[key];
    g_bindMemoOk = true;
    const uint64_t regSig = fnv(&fmtReg, 4) ^ (uint64_t(rect) << 20) ^ (uint64_t(pitch) << 3) ^ remap * 0x100000001b3ull ^ uint64_t(texture_epoch()) * 0x9E3779B97F4A7C15ull;
    uint64_t sig = tx.sig;
    // The content is re-sampled every 4th frame only (and whenever the registers change): textures are rarely
    // rewritten in place (render targets are handled apart), and this sampling was a third of the RSX thread.
    if (!tx.id || tx.regSig != regSig || frameNo - tx.checked >= 4 || tx.checked > frameNo) {
        sig = regSig;
        const uint32_t step = std::max<uint32_t>(64, total / 128);   // 128 samples: a texture streamed over another changes everywhere
        for (uint32_t k = 0; k + 8 <= total; k += step) sig = fnv(C.mem + ea + k, 8, sig);
        sig = fnv(C.mem + ea + total - std::min<uint32_t>(total, 8), std::min<uint32_t>(total, 8), sig);
        tx.checked = frameNo; tx.regSig = regSig;
    }
    const GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    if (!tx.id || tx.sig != sig || tx.target != target) {
        if (tx.id && tx.target != target) { ++g_texGen; glDeleteTextures(1, &tx.id); tx.id = 0; }   // re-uploads keep the GL name: bindings stay valid
        if (!tx.id) glGenTextures(1, &tx.id);
        tx.target = target;
        tx.sig = sig;
        tx.sampler = ~0ull;
        ++g_texUploads; g_texUploadBytes += total;
        TickScope texTime(g_texTicks);
        glBindTexture(target, tx.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        std::vector<uint8_t> buf;
        for (uint32_t face = 0; face < faces; ++face) {
            uint32_t p = ea + face * faceStride;
            const GLenum tgt = cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
            for (uint32_t l = 0, lw = w, lh = h; l < mips; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) {
                const uint32_t lb = levelBytes(lw, lh);
                if (tf.dxt) {
                    glCompressedTexImage2D(tgt, l, tf.ifmt, lw, lh, 0, lb, C.mem + p);
                } else {
                    const bool swz = !ln && (lw & (lw - 1)) == 0 && (lh & (lh - 1)) == 0;
                    const uint32_t lwl = ilog2(lw), lhl = ilog2(lh);
                    if (!swz && tf.swap == 0 && baseFmt != 0x9e) {   // rows as they are in memory: no copy at all
                        if (ln && pitch) glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / tf.bpp));
                        glTexImage2D(tgt, l, tf.ifmt, lw, lh, 0, tf.fmt, tf.type, C.mem + p);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        p += lb;
                        continue;
                    }
                    buf.resize(size_t(lw) * lh * tf.bpp);
                    if (swz && tf.bpp == 4 && tf.swap == 0) {   // swizzled 32-bit texels: x and y bits never overlap, one table each
                        static std::vector<uint32_t> xs, ys;
                        xs.resize(lw); ys.resize(lh);
                        for (uint32_t x = 0; x < lw; ++x) xs[x] = swizzle_index(x, 0, lwl, lhl) * 4;
                        for (uint32_t y = 0; y < lh; ++y) ys[y] = swizzle_index(0, y, lwl, lhl) * 4;
                        const uint8_t* src = C.mem + p;
                        uint32_t* d = reinterpret_cast<uint32_t*>(buf.data());
                        for (uint32_t y = 0; y < lh; ++y) {
                            const uint8_t* row = src + ys[y];
                            for (uint32_t x = 0; x < lw; ++x) std::memcpy(d++, row + xs[x], 4);
                        }
                    } else
                    for (uint32_t y = 0; y < lh; ++y)
                        for (uint32_t x = 0; x < lw; ++x) {
                            const uint32_t src = swz ? swizzle_index(x, y, lwl, lhl) * tf.bpp : (ln && pitch ? y * pitch + x * tf.bpp : (y * lw + x) * tf.bpp);
                            uint8_t* d = buf.data() + (size_t(y) * lw + x) * tf.bpp;
                            const uint8_t* s = C.mem + p + src;
                            if (tf.swap == 2) for (uint32_t k = 0; k < tf.bpp; k += 2) { d[k] = s[k + 1]; d[k + 1] = s[k]; }
                            else if (tf.swap == 4) for (uint32_t k = 0; k < tf.bpp; k += 4) { d[k] = s[k + 3]; d[k + 1] = s[k + 2]; d[k + 2] = s[k + 1]; d[k + 3] = s[k]; }
                            else std::memcpy(d, s, tf.bpp);
                        }
                    if (baseFmt == 0x9e) for (size_t k = 0; k < buf.size(); k += 4) buf[k] = 0xff;   // D8R8G8B8: alpha = 1 (byte 0 = A in memory)
                    glTexImage2D(tgt, l, tf.ifmt, lw, lh, 0, tf.fmt, tf.type, buf.data());
                }
                p += lb;
            }
        }
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, mips - 1);
    } else glBindTexture(target, tx.id);
    tx.lastUse = frameNo;
    g_lastId = tx.id; g_lastTarget = target;
    {   // the sampler state lives in the GL texture object: set it only when the registers changed
        const uint32_t sreg[7] = {addr, ctl0, filt, remap, R[(base + 0x1c) >> 2], mips, uint32_t(settings().aniso)};
        const uint64_t sk = fnv(sreg, sizeof sreg);
        if (tx.sampler == sk) return;
        tx.sampler = sk;
    }
    // remap: per output channel (A, R, G, B) a source channel and a mode (0 zero, 1 one, 2 source)
    {
        const GLint srcCh[4] = {GL_ALPHA, GL_RED, GL_GREEN, GL_BLUE};
        GLint sw[4];
        const int outOrder[4] = {1, 2, 3, 0};   // GL swizzle order R G B A <- RSX A=0 R=1 G=2 B=3
        for (int k = 0; k < 4; ++k) {
            const int rc = outOrder[k];
            const uint32_t sel = (remap >> (2 * rc)) & 3, mode = (remap >> (8 + 2 * rc)) & 3;
            GLint v = srcCh[sel];
            if (tf.fmt == GL_RED) v = GL_RED;
            else if (tf.fmt == GL_RG) v = (sel == 1 || sel == 3) ? GL_GREEN : GL_RED;
            if (mode == 0) v = GL_ZERO; else if (mode == 1) v = GL_ONE;
            sw[k] = v;
        }
        glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, sw);
    }
    const uint32_t minF = (filt >> 16) & 0xff, magF = (filt >> 24) & 7;
    static const GLenum minTab[] = {GL_NEAREST, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR};
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mips > 1 ? minTab[minF & 7] : (minF == 1 || minF == 3 || minF == 5 ? GL_NEAREST : GL_LINEAR));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magF == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_of(addr & 0xf));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_of((addr >> 8) & 0xf));
    glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_of((addr >> 16) & 0xf));
    apply_border_color(target, base);
    glTexParameterf(target, GL_TEXTURE_MIN_LOD, ((ctl0 >> 19) & 0xfff) / 256.f);
    glTexParameterf(target, GL_TEXTURE_MAX_LOD, ((ctl0 >> 7) & 0xfff) / 256.f);
    // the game's own anisotropy, raised to the option's (mipmapped textures only: the road and walls seen at a
    // grazing angle stay sharp in the distance instead of blurring)
    const uint32_t gameAniso = std::min<uint32_t>(4, (ctl0 >> 4) & 7);
    const uint32_t an = mips > 1 ? std::max<uint32_t>(gameAniso, uint32_t(std::clamp(settings().aniso, 0, 4))) : gameAniso;
    glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, float(1u << an));
    if (std::getenv("ANISOLOG")) { static int n = 0; if (n++ < 12) RLOG("[aniso] texture %ux%u, %u levels, filter %08x, game %ux -> %ux%c", w, h, mips, filt, 1u << gameAniso, 1u << an, 10); }
}

static GLenum gl_prim(uint32_t p) {
    switch (p) { case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_LOOP; case 4: return GL_LINE_STRIP; case 5: return GL_TRIANGLES;
                 case 6: case 9: return GL_TRIANGLE_STRIP; case 7: case 10: return GL_TRIANGLE_FAN; case 8: return GL_TRIANGLES; default: return GL_TRIANGLES; }
}

static void apply_state() {
    static const uint16_t regs[] = {0x310, 0x314, 0x318, 0x320, 0x31c, 0x324, 0xa74, 0xa6c, 0xa70, 0x328, 0x330, 0x334, 0x338, 0x33c, 0x340, 0x344, 0x32c,
                                    0x183c, 0x1830, 0x1834, 0xa68, 0xa78, 0xa7c};
    static uint32_t last[sizeof regs / sizeof regs[0]];
    uint32_t now[sizeof regs / sizeof regs[0]];
    for (size_t k = 0; k < sizeof regs / sizeof regs[0]; ++k) now[k] = R[regs[k] >> 2];
    if (stateValid && !std::memcmp(now, last, sizeof now)) return;
    std::memcpy(last, now, sizeof now);
    stateValid = true;
    const auto en = [](GLenum c, bool on) { on ? glEnable(c) : glDisable(c); };
    en(GL_BLEND, R[0x310 >> 2] & 1);
    const uint32_t sf = R[0x314 >> 2], df = R[0x318 >> 2], eq = R[0x320 >> 2], bc = R[0x31c >> 2];
    glBlendFuncSeparate(sf & 0xffff, df & 0xffff, sf >> 16, df >> 16);
    glBlendEquationSeparate((eq & 0xffff) ? (eq & 0xffff) : 0x8006, (eq >> 16) ? (eq >> 16) : 0x8006);
    glBlendColor(((bc >> 16) & 0xff) / 255.f, ((bc >> 8) & 0xff) / 255.f, (bc & 0xff) / 255.f, (bc >> 24) / 255.f);
    const uint32_t cm = R[0x324 >> 2];
    for (int k = 0; k < 4; ++k) glColorMaski(k, (cm >> 16) & 1, (cm >> 8) & 1, cm & 1, (cm >> 24) & 1);
    en(GL_DEPTH_TEST, R[0xa74 >> 2] & 1);
    glDepthFunc(R[0xa6c >> 2] ? R[0xa6c >> 2] : 0x201);
    glDepthMask(R[0xa70 >> 2] & 1);
    en(GL_STENCIL_TEST, R[0x328 >> 2] & 1);
    if (R[0x328 >> 2] & 1) {
        glStencilFuncSeparate(GL_FRONT_AND_BACK, R[0x330 >> 2] ? R[0x330 >> 2] : 0x207, R[0x334 >> 2], R[0x338 >> 2]);
        glStencilOpSeparate(GL_FRONT_AND_BACK, R[0x33c >> 2] ? R[0x33c >> 2] : 0x1e00, R[0x340 >> 2] ? R[0x340 >> 2] : 0x1e00, R[0x344 >> 2] ? R[0x344 >> 2] : 0x1e00);
        glStencilMaskSeparate(GL_FRONT_AND_BACK, R[0x32c >> 2]);
    }
    static const bool noCull = std::getenv("RSXNOCULL") != nullptr;
    en(GL_CULL_FACE, !noCull && (R[0x183c >> 2] & 1));
    glCullFace(R[0x1830 >> 2] ? R[0x1830 >> 2] : 0x405);
    // the surface is drawn upside down in GL (row 0 = top), which flips the winding
    const uint32_t ff = R[0x1834 >> 2] ? R[0x1834 >> 2] : GL_CCW;
    glFrontFace(ff == GL_CW ? GL_CCW : GL_CW);
    en(GL_POLYGON_OFFSET_FILL, R[0xa68 >> 2] & 1);
    glPolygonOffset(rf(0xa78), rf(0xa7c));
}

// Vertex streams in RSX local memory (texture coordinates, normals, colours of the level and the cars) do not
// change from frame to frame: they are converted to floats once and kept in a GL buffer. Edge geometry output
// (positions, in main memory) is new every frame and goes through the per-draw path. The content of a cached
// stream is sampled once per frame, so a level change that reuses the address is noticed.
struct Stream { GLuint vbo = 0; size_t offset = 0; uint32_t count = 0; uint64_t sig = 0, checked = ~0ull; };
static std::unordered_map<uint64_t, Stream> streams;
// The streams live in a few big GL buffers (64 MB each, filled front to back): a buffer of its own per stream cost
// ~25 us of driver time each, and a scene change builds 1000-2000 of them (40 ms in one frame). A stream that has to
// grow or changed content gets new room; the space it leaves is reclaimed when the arena is full (everything is
// dropped and rebuilt, once per few scene changes). RSXNOARENA=1: one buffer per stream as before.
static constexpr size_t kArenaChunk = size_t(64) << 20, kArenaMaxChunks = 12;
static std::vector<GLuint> arenaBufs;
static size_t arenaChunk = 0, arenaHead = 0;
static bool arena_alloc(size_t bytes, GLuint& buf, size_t& off) {
    if (bytes > kArenaChunk) return false;
    if (arenaBufs.empty() || arenaHead + bytes > kArenaChunk) {
        if (!arenaBufs.empty()) ++arenaChunk;
        arenaHead = 0;
        if (arenaChunk >= kArenaMaxChunks) {   // full: start over (the streams are rebuilt as draws ask for them)
            for (auto& [k, st] : streams) st.vbo = 0, st.count = 0;
            arenaChunk = 0;
            RLOG("[rsx] vertex stream arena full: rebuilt\n");
        }
        while (arenaBufs.size() <= arenaChunk) {
            GLuint b = 0; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
            if (glBufferStorage) glBufferStorage(GL_ARRAY_BUFFER, GLsizeiptr(kArenaChunk), nullptr, 0x0100 /* GL_DYNAMIC_STORAGE_BIT */);
            else glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kArenaChunk), nullptr, 0x88E4 /* GL_STATIC_DRAW */);
            arenaBufs.push_back(b);
        }
    }
    buf = arenaBufs[arenaChunk]; off = arenaHead;
    arenaHead += (bytes + 255) & ~size_t(255);
    return true;
}
static uint64_t stream_sig(const uint8_t* p, uint32_t stride, uint32_t count) {
    uint64_t h = fnv(&count, 4);
    const uint32_t step = std::max<uint32_t>(1, count / 24);
    for (uint32_t v = 0; v < count; v += step) h = fnv(p + size_t(v) * stride, std::min<uint32_t>(stride, 8), h);
    return fnv(p + size_t(count - 1) * stride, std::min<uint32_t>(stride, 8), h);
}
// GL buffer with attribute a converted for vertices 0..need-1 (4 floats each), or 0 when the stream is not cacheable
static GLuint cached_stream(int a, uint32_t need, size_t& offset) {
    static const bool off = std::getenv("RSXNOSTREAMCACHE") != nullptr;
    const uint32_t fmt = R[(0x1740 + 4 * a) >> 2], offReg = R[(0x1680 + 4 * a) >> 2];
    const uint32_t stride = (fmt >> 8) & 0xff;
    if (off || (offReg >> 31) || stride == 0 || need == 0 || need > 2000000) return 0;
    const uint32_t base = ea_of(offReg & 0x7fffffff, 0) + R[0x1738 >> 2];
    if (uint64_t(base) + uint64_t(need + need / 4 + 64) * stride >= 0xfff00000ull) return 0;
    Stream& st = streams[uint64_t(base) << 16 | (fmt & 0xffff)];
    const uint8_t* p = C.mem + base;
    if (st.count && st.checked != frameNo) {
        st.checked = frameNo;
        if (stream_sig(p, stride, st.count) != st.sig) st.count = 0;   // the memory holds something else now
    }
    if (st.count < need) {
        static std::vector<float> tmp;
        const uint32_t n = need + need / 4 + 64;   // room to grow: other draws of the object use later vertices
        tmp.resize(size_t(n) * 4);
        { TickScope t(g_convTicks); convert_attr(a, 0, n, tmp.data()); }
        TickScope bt(g_bufTicks);
        static const bool noArena = std::getenv("RSXNOARENA") != nullptr;
        GLuint ab = 0; size_t ao = 0;
        if (!noArena && arena_alloc(tmp.size() * 4, ab, ao)) {
            st.vbo = ab; st.offset = ao;   // (an arena reset clears entries but keeps them: st stays valid)
            glBindBuffer(GL_ARRAY_BUFFER, ab);
            glBufferSubData(GL_ARRAY_BUFFER, GLintptr(ao), GLsizeiptr(tmp.size() * 4), tmp.data());
        } else {
            if (!st.vbo || !noArena) { glGenBuffers(1, &st.vbo); }   // (a stream too big for the arena: its own buffer)
            st.offset = 0;
            glBindBuffer(GL_ARRAY_BUFFER, st.vbo);
            glBufferData(GL_ARRAY_BUFFER, tmp.size() * 4, tmp.data(), 0x88E4 /* GL_STATIC_DRAW */);
        }
        st.count = n; st.sig = stream_sig(p, stride, n); st.checked = frameNo;
        ++g_streamBuilds; g_streamVerts += n;
    }
    offset = st.offset;
    return st.vbo;
}

// Per-draw vertex and index data go into one persistently mapped buffer used as a ring of 4 segments: a fence is
// set when the writer leaves a segment and waited for before it comes back (several frames later), so no draw
// pays for a buffer allocation in the driver (two glBufferData per draw before). RSXNORING=1: old path.
static uint64_t pfRingWaits, pfRingWaitT;   // RSXPROF: ring fence waits
static GLuint ringBuf = 0;
static uint8_t* ringPtr = nullptr;
static constexpr size_t kRingSeg = size_t(16) << 20, kRingSegs = 4;
static size_t ringHead = 0, ringSegNo = 0;
static uint64_t ringSegSerial = 0;   // +1 every segment change: data older than 3 changes may be overwritten
static GLsync ringFence[kRingSegs] = {};
static void ring_init() {
    if (std::getenv("RSXNORING") || !glBufferStorage || !glMapBufferRange || !glFenceSync) return;
    glGenBuffers(1, &ringBuf);
    glBindBuffer(GL_ARRAY_BUFFER, ringBuf);
    const GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(GL_ARRAY_BUFFER, GLsizeiptr(kRingSeg * kRingSegs), nullptr, fl);
    ringPtr = static_cast<uint8_t*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(kRingSeg * kRingSegs), fl));
    if (!ringPtr) RLOG("[rsx] persistent vertex ring not available, using glBufferData per draw\n");
}
// room for `bytes` (aligned) in the ring; nullptr when it does not fit in a segment (caller falls back)
static uint8_t* ring_alloc(size_t bytes, size_t align, size_t& off) {
    if (!ringPtr || bytes > kRingSeg) return nullptr;
    size_t h = (ringHead + align - 1) & ~(align - 1);
    if (h + bytes > (ringSegNo + 1) * kRingSeg) {   // next segment: fence this one, wait until the GPU is done with the next
        ringFence[ringSegNo] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        ringSegNo = (ringSegNo + 1) % kRingSegs; ++ringSegSerial;
        if (ringFence[ringSegNo]) { const uint64_t tw = SDL_GetPerformanceCounter(); glClientWaitSync(ringFence[ringSegNo], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); glDeleteSync(ringFence[ringSegNo]); ringFence[ringSegNo] = nullptr; ++pfRingWaits; pfRingWaitT += SDL_GetPerformanceCounter() - tw; }
        h = ringSegNo * kRingSeg;
    }
    ringHead = h + bytes;
    off = h;
    return ringPtr + h;
}

// RSXPROF=1: time inside do_draw by stage (ms per second) and totals per second
static uint64_t pfT[6], pfDraws, pfVerts, pfIdx, pfLast;
static uint64_t pfSub[4];   // RSXPROF: attribute setup, index copy, draw call, (spare)
static const char* const pfName[6] = {"target+program", "state+uniforms", "textures", "indices", "vertex conversion", "upload+draw"};
static void do_draw() {
    if (ranges.empty() || !glOk) return;
    uint64_t stT[6] = {};   // this draw's time per stage (pfName), for [slowdraw]
    struct SlowDraw {   // a draw over 6 ms: what it had to build and which stage took the time
        const uint64_t* st;
        uint64_t t0 = SDL_GetPerformanceCounter(), tu = g_texUploads, tb = g_texUploadBytes, sb = g_streamBuilds, sv = g_streamVerts, pb = g_progBuilds, rw = pfRingWaitT;
        explicit SlowDraw(const uint64_t* s) : st(s) {}
        ~SlowDraw() {
            const uint64_t dt = SDL_GetPerformanceCounter() - t0;
            g_drawTicks += dt; ++g_drawCount;
            const double k = 1000.0 / SDL_GetPerformanceFrequency(), ms = dt * k;
            if (ms > 6) RLOG("[slowdraw] %.0f ms: %llu textures uploaded (%llu KB), %llu vertex streams built (%llu vertices), %llu programs built; "
                             "program %.1f, state+constants %.1f, textures %.1f, indices %.1f, vertices %.1f, draw %.1f ms (ring wait %.1f)\n", ms, (unsigned long long)(g_texUploads - tu),
                             (unsigned long long)((g_texUploadBytes - tb) >> 10), (unsigned long long)(g_streamBuilds - sb), (unsigned long long)(g_streamVerts - sv), (unsigned long long)(g_progBuilds - pb),
                             st[0] * k, st[1] * k, st[2] * k, st[3] * k, st[4] * k, st[5] * k, (pfRingWaitT - rw) * k);
        }
    } slowDraw(stT);
    static const bool prof = std::getenv("RSXPROF") != nullptr;
    uint64_t pt = SDL_GetPerformanceCounter();
    int stage = 0;
    auto mark = [&] { const uint64_t n = SDL_GetPerformanceCounter(); if (stage < 6) { stT[stage] = n - pt; if (prof) pfT[stage] += n - pt; ++stage; } pt = n; };
    ++drawsThisFrame;
    {   // RSXSKIP=a-b: skip draws a..b of every frame (debug bisection)
        static long s0 = -1, s1 = -1;
        static bool parsed = false;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("RSXSKIP")) std::sscanf(e, "%ld-%ld", &s0, &s1); }
        if (long(drawsThisFrame) - 1 >= s0 && long(drawsThisFrame) - 1 <= s1) return;
    }
    if (!bind_target()) return;
    apply_scissor();
    // programs
    const uint32_t sp = R[0x8e4 >> 2];
    const uint32_t fpEa = ea_of(sp & ~3u, (sp & 3) == 2 ? 1 : 0);
    uint32_t texDims = 0;
    for (int i = 0; i < 16; ++i) texDims |= texDimsOf(i) << (2 * i);
    Program* p = get_program(R[0x1ea0 >> 2] & 511, fpEa, texDims, R[0x1d60 >> 2]);
    if (!p) return;
// Only draws into the full-screen image may be skipped while their shaders are being built (it is drawn again
// next frame); a draw into anything else (paint layers composed once when a race loads, shadow maps, cube faces)
// waits: skipped, it would leave that texture wrong for good (the player's car came out black).
    if (p->pending && !finish_program(*p, !(cur.logW == 1280 && cur.logH == 720))) { ++g_skippedDraws; return; }
    if (p->bad) return;
    mark();
    glUseProgram(p->prog);
    apply_state();
    // vertex program constants
    // Default: a full copy of the constants goes into the persistent ring and is bound by range, so the driver
    // never has to update a buffer the GPU may still be reading (glBufferSubData on a busy UBO stalls the AMD
    // OpenGL driver until the GPU catches up). Only on AMD: on NVIDIA rebinding the range per draw costs ~2 us of
    // driver time per draw (measured 3.2 -> 6.5 us/draw), while its glBufferSubData does not stall.
    // RSXUBORING=1 / RSXUBOSUB=1 force the ring / the old path (dirty range into one UBO).
    static const bool uboSub = [] {
        if (!glBindBufferRange || std::getenv("RSXUBOSUB")) return true;
        if (std::getenv("RSXUBORING")) return false;
        const char* v = reinterpret_cast<const char*>(glGetString(0x1F00 /* GL_VENDOR */));
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const bool amd = (v && (std::strstr(v, "ATI") || std::strstr(v, "AMD"))) || (r && (std::strstr(r, "AMD") || std::strstr(r, "Radeon")));
        RLOG("[rsx] vertex constants: %s\n", amd ? "copied into the persistent ring (AMD)" : "updated in one buffer");
        return !amd;
    }();
    static GLint uboAlign = 0;
    static uint64_t uboSerial = ~0ull;   // ring segment serial when the bound copy was made
    static bool uboInRing = false;
    bool uboDone = false;
    if (!uboSub && ringPtr) {
        if (!uboAlign) { glGetIntegerv(0x8A34 /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */, &uboAlign); if (uboAlign <= 0) uboAlign = 256; }
        if (uboInRing && !vpConstDirty && ringSegSerial - uboSerial < kRingSegs - 1) uboDone = true;   // bound copy still valid
        else {
            size_t uOff = 0;
            if (uint8_t* up = ring_alloc(sizeof vpConst, size_t(uboAlign), uOff)) {
                std::memcpy(up, vpConst, sizeof vpConst);
                glBindBufferRange(GL_UNIFORM_BUFFER, 0, ringBuf, GLintptr(uOff), GLsizeiptr(sizeof vpConst));
                uboSerial = ringSegSerial; uboInRing = true; uboDone = true;
                vpConstDirty = false; vpDirtyLo = 468 * 4; vpDirtyHi = 0;
            }
        }
    }
    if (!uboDone && uboInRing) { uboInRing = false; vpConstDirty = true; vpDirtyLo = 0; vpDirtyHi = 468 * 4; }   // back to the UBO: refresh it whole
    if (!uboDone && vpConstDirty) {   // only the constants written since the last draw (usually a matrix or two of the 468 registers)
        static bool allocated = false;
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        if (!allocated) { glBufferData(GL_UNIFORM_BUFFER, sizeof vpConst, vpConst, GL_DYNAMIC_DRAW); allocated = true; }
        else glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(vpDirtyLo) * 4, GLsizeiptr(vpDirtyHi - vpDirtyLo) * 4, vpConst + vpDirtyLo);
        vpConstDirty = false; vpDirtyLo = 468 * 4; vpDirtyHi = 0;
    }
    if (!uboDone) glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    float vs[4] = {rf(0xa30), rf(0xa34), rf(0xa38), rf(0xa3c)}, vo[4] = {rf(0xa20), rf(0xa24), rf(0xa28), rf(0xa2c)};
    glUniform4fv(p->uVs, 1, vs);
    glUniform4fv(p->uVo, 1, vo);
    const float surf[4] = {float(cur.logW), float(cur.logH), 0, 0};
    glUniform4fv(p->uSurf, 1, surf);
    const uint32_t af = R[0x308 >> 2];
    const float alpha[4] = {float(R[0x304 >> 2] & 1), float(af >= 0x200 ? af - 0x200 : 7), (R[0x30c >> 2] & 0xff) / 255.f, 0};
    glUniform4fv(p->uAlpha, 1, alpha);
    if (p->nConst && p->uFc >= 0) {   // fragment program constants (patched into the microcode by the game)
        glUniform4fv(p->uFc, GLsizei(g_fpConsts.size() / 4), g_fpConsts.data());
    }
    const float wp[4] = {1.f / cur.aaX, 1.f / cur.aaY, 0, 0};
    glUniform4fv(p->uWpos, 1, wp);
    mark();
    float tsc[16 * 4];
    for (int i = 0; i < 16; ++i) { tsc[4 * i] = tsc[4 * i + 1] = tsc[4 * i + 2] = tsc[4 * i + 3] = 1.f; if (p->texUsed & (1u << i)) bind_texture(i, tsc + 4 * i); }
    if (p->uTsc >= 0) glUniform4fv(p->uTsc, 16, tsc);
    mark();
    // vertices
    uint32_t vmin = 0xffffffffu, vmax = 0;
    idx.clear();
    const uint32_t ia = R[0x1820 >> 2];
    const bool i16 = (ia >> 4) & 1;
    const uint32_t ibase = ea_of(R[0x181c >> 2], ia & 1);
    const bool restart = R[0x1dac >> 2] & 1;
    const uint32_t restartIdx = R[0x1db0 >> 2];
    {   // gather the indices of all ranges into idx (0xffffffff = primitive restart): tight loops, this runs for every index of every draw
        const GLenum glp = gl_prim(prim);
        const bool joinStrips = glp != GL_TRIANGLES && glp != GL_LINES && glp != GL_POINTS;
        const uint32_t add = rangesIndexed ? R[0x173c >> 2] : 0;
        size_t total = 0;
        for (const Range& r : ranges) total += size_t(r.count) + 1;
        idx.resize(prim == 8 ? total * 3 / 2 + 8 : total);
        static std::vector<uint32_t> seq;
        uint32_t* o = idx.data();
        for (const Range& r : ranges) {
            if (prim == 8) seq.resize(r.count);
            uint32_t* d = prim == 8 ? seq.data() : o;
            if (!rangesIndexed) for (uint32_t k = 0; k < r.count; ++k) d[k] = r.first + k;
            else if (i16) {
                const uint8_t* q = C.mem + ibase + 2 * size_t(r.first);
                gather_i16(d, q, r.count, add);
            } else {
                const uint8_t* q = C.mem + ibase + 4 * size_t(r.first);
                for (uint32_t k = 0; k < r.count; ++k) { uint32_t w; std::memcpy(&w, q + 4 * size_t(k), 4); d[k] = __builtin_bswap32(w) + add; }
            }
            if (restart && rangesIndexed) for (uint32_t k = 0; k < r.count; ++k) d[k] = d[k] == restartIdx ? 0xffffffffu : d[k];
            if (prim == 8) {   // quads -> triangles
                for (size_t q = 0; q + 3 < seq.size(); q += 4) { o[0] = seq[q]; o[1] = seq[q + 1]; o[2] = seq[q + 2]; o[3] = seq[q]; o[4] = seq[q + 2]; o[5] = seq[q + 3]; o += 6; }
            } else {
                o += r.count;
                if (&r != &ranges.back() && joinStrips) *o++ = 0xffffffffu;
            }
        }
        idx.resize(size_t(o - idx.data()));
        uint32_t lo = 0xffffffffu, hi1 = 0;   // hi1 = highest index + 1 (the restart value wraps to 0 and drops out)
        index_range(idx.data(), idx.size(), lo, hi1);
        if (hi1 == 0) return;
        vmin = lo; vmax = hi1 - 1;
    }
    if (vmin > vmax || idx.empty()) return;
    {   // RSXSKIPFMT=xxxx: skip draws whose position array has this format (debug); RSXONLYFMT: draw only those
        static const long sf = std::getenv("RSXSKIPFMT") ? std::strtol(std::getenv("RSXSKIPFMT"), nullptr, 16) : -1;
        static const long of = std::getenv("RSXONLYFMT") ? std::strtol(std::getenv("RSXONLYFMT"), nullptr, 16) : -1;
        const long f0 = long(R[0x1740 >> 2] & 0xffff);
        if (cur.w == 2560 && ((sf >= 0 && f0 == sf) || (of >= 0 && f0 != of))) return;
    }
    mark();
    // A draw often uses a few vertices spread over a large buffer: convert only the referenced ones then.
    static std::vector<uint32_t> uniq, vmap;
    const uint32_t span = vmax - vmin + 1;
    // Edge-culled draws index a few scattered vertices of static buffers in local memory: when every attribute is
    // such a buffer, the cached streams serve them with the original indices (no sort, conversion or remap per draw)
    static const bool noSparseCache = std::getenv("RSXNOSPARSECACHE") != nullptr;
    // The same holds when the only other attributes are float arrays in main memory (Edge-transformed positions): a
    // byte swap of the whole span is cheaper than sorting the indices and converting the static attributes per draw.
    bool allCacheable = !noSparseCache, mainFloats = false;
    for (int a = 0; a < 16 && allCacheable; ++a) {
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        if (!((fmt >> 4) & 0xf)) continue;
        if (!((fmt >> 8) & 0xff)) allCacheable = false;
        else if (R[(0x1680 + 4 * a) >> 2] >> 31) { if ((fmt & 0xf) == 2) mainFloats = true; else allCacheable = false; }
    }
    static const long spanMul = std::getenv("RSXSPANMUL") ? std::atol(std::getenv("RSXSPANMUL")) : 12;
    const bool dense = allCacheable && vmax < 1000000 && (!mainFloats || span <= idx.size() * size_t(spanMul));
    const bool sparse = rangesIndexed && span > 64 && span > idx.size() * 2 && !dense;
    if (sparse) {
        uniq.clear();
        for (uint32_t v : idx) if (v != 0xffffffffu) uniq.push_back(v);
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    }
    const uint32_t nv = sparse ? uint32_t(uniq.size()) : span;
    if (nv > 4000000) { RLOG("[rsx] draw with %u vertices skipped\n", nv); return; }
    int attrs[16], na = 0;
    for (int a = 0; a < 16; ++a) {
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        if ((fmt >> 4) & 0xf) attrs[na++] = a;
        else {
            glDisableVertexAttribArray(a);
            glVertexAttrib4f(a, rf(0x1c00 + 16 * a), rf(0x1c04 + 16 * a), rf(0x1c08 + 16 * a), rf(0x1c0c + 16 * a));
        }
    }
    GLuint streamVbo[16] = {}; size_t streamOff[16] = {};
    // where in vtx each attribute converted for this draw starts (in floats; -1 = cached stream), and its floats per vertex
    long slot[16]; uint32_t comps[16]; size_t vtxFloats = 0;
    for (int k = 0; k < na; ++k) {
        comps[k] = 4;
        if (!sparse && (streamVbo[k] = cached_stream(attrs[k], vmax + 1, streamOff[k])) != 0) { slot[k] = -1; continue; }
        const uint32_t fmt = R[(0x1740 + 4 * attrs[k]) >> 2];
        comps[k] = ((fmt >> 8) & 0xff) ? attr_comps(attrs[k]) : 4;
        slot[k] = long(vtxFloats);
        vtxFloats += size_t(nv) * comps[k];
    }
    const bool nSlots = vtxFloats != 0;
    size_t vOff = 0;
    float* vb = nSlots ? reinterpret_cast<float*>(ring_alloc(vtxFloats * 4, 16, vOff)) : nullptr;   // converted straight into the ring
    const bool inRing = vb != nullptr;
    if (!inRing) { vtx.resize(vtxFloats); vb = vtx.data(); }
    static const bool fmtStat = std::getenv("FMTSTAT") != nullptr;   // FMTSTAT=1: converted vertices per attribute format (debug)
    static std::map<uint64_t, uint64_t> fmtHist;
    for (int k = 0; k < na; ++k) {
        const uint32_t fmt = R[(0x1740 + 4 * attrs[k]) >> 2];
        const uint32_t stride = (fmt >> 8) & 0xff;
        if (slot[k] < 0) continue;
        const size_t so = size_t(slot[k]);   // first float of this attribute's slot
        const uint32_t cc = comps[k];
        if (fmtStat) {
            fmtHist[uint64_t(fmt & 0xffff) | uint64_t(R[(0x1680 + 4 * attrs[k]) >> 2] >> 31) << 16 | uint64_t(sparse) << 17 | uint64_t(attrs[k]) << 20] += nv;
            if (frameNo % 600 == 0 && drawsThisFrame == 1) {
                std::vector<std::pair<uint64_t, uint64_t>> v(fmtHist.begin(), fmtHist.end());
                std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
                for (size_t i = 0; i < v.size() && i < 12; ++i) RLOG("[fmt] attr %llu fmt %04llx %s%s: %llu k vertices\n", (unsigned long long)(v[i].first >> 20), (unsigned long long)(v[i].first & 0xffff), (v[i].first >> 16 & 1) ? "main" : "local", (v[i].first >> 17 & 1) ? " sparse" : "", (unsigned long long)(v[i].second / 1000));
                fmtHist.clear();
            }
        }
        if (stride == 0) { float c4[4]; convert_attr(attrs[k], 0, 1, c4); for (uint32_t v = 0; v < nv; ++v) std::memcpy(vb + so + v * 4, c4, 16); }
        else if (!sparse) convert_attr(attrs[k], vmin, nv, vb + so, cc);
        else
            for (uint32_t i = 0; i < nv;) {   // runs of consecutive vertices
                uint32_t j = i + 1;
                while (j < nv && uniq[j] == uniq[j - 1] + 1) ++j;
                convert_attr(attrs[k], uniq[i], j - i, vb + so + size_t(i) * cc, cc);
                i = j;
            }
    }
    static const bool vstat = std::getenv("RSXVSTAT") != nullptr;
    if (vstat && na) {   // sanity of positions (attribute 0): NaN / huge values, by memory location
        const uint32_t fo = R[0x1680 >> 2];
        uint32_t bad = 0;
        float mx = 0;
        for (uint32_t v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c) { const float f = vb[v * 4 + c]; if (!(std::fabs(f) < 1e5f)) ++bad; else mx = std::max(mx, std::fabs(f)); }
        static uint64_t tot[2] = {}, badv[2] = {}, draws[2] = {}, badDraws[2] = {};
        const int m = fo >> 31;
        tot[m] += nv; badv[m] += bad; ++draws[m]; if (bad) ++badDraws[m];
        if (bad && badDraws[m] < 20) RLOG("[vstat] %s draw: %u of %u position values bad (attr0 fmt %08x off %08x, max ok %g, idx range %u-%u)\n", m ? "main" : "local", bad, nv * 3, R[0x1740 >> 2], fo, mx, vmin, vmax);
        if (frameNo % 100 == 0 && drawsThisFrame == 1) RLOG("[vstat] local: %llu/%llu draws bad; main: %llu/%llu draws bad\n", (unsigned long long)badDraws[0], (unsigned long long)draws[0], (unsigned long long)badDraws[1], (unsigned long long)draws[1]);
    }
    mark();
    uint64_t ps = prof ? SDL_GetPerformanceCounter() : 0;
    const auto sub = [&](int k) { if (prof) { const uint64_t n = SDL_GetPerformanceCounter(); pfSub[k] += n - ps; ps = n; } };
    glBindVertexArray(vao);
    const GLuint vBuf = inRing ? ringBuf : vbo;
    glBindBuffer(GL_ARRAY_BUFFER, vBuf);
    if (nSlots && !inRing) glBufferData(GL_ARRAY_BUFFER, vtx.size() * 4, vtx.data(), GL_STREAM_DRAW);
    for (int k = 0; k < na; ++k) {
        glEnableVertexAttribArray(attrs[k]);
        if (streamVbo[k]) {   // cached stream: holds every vertex from 0, the indices are relative to vmin
            glBindBuffer(GL_ARRAY_BUFFER, streamVbo[k]);
            glVertexAttribPointer(attrs[k], 4, GL_FLOAT, 0, 16, reinterpret_cast<const void*>(streamOff[k] + size_t(vmin) * 16));
            glBindBuffer(GL_ARRAY_BUFFER, vBuf);
        } else
            glVertexAttribPointer(attrs[k], GLint(comps[k]), GL_FLOAT, 0, GLsizei(4 * comps[k]), reinterpret_cast<const void*>(vOff + size_t(slot[k]) * 4));
    }
    if (sparse) {
        if (vmap.size() < span) vmap.resize(span);
        for (uint32_t i = 0; i < nv; ++i) vmap[uniq[i] - vmin] = i;
        for (uint32_t& v : idx) if (v != 0xffffffffu) v = vmap[v - vmin];
    }
    sub(0);
    size_t iOff = 0;
    if (uint8_t* ip = ring_alloc(idx.size() * 4, 4, iOff)) { std::memcpy(ip, idx.data(), idx.size() * 4); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ringBuf); }
    else { iOff = 0; glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STREAM_DRAW); }
    sub(1);
    // not sparse: the buffers start at vertex vmin, the base vertex takes it off the indices (restart is tested before it)
    glDrawElementsBaseVertex(gl_prim(prim), GLsizei(idx.size()), GL_UNSIGNED_INT, reinterpret_cast<const void*>(iOff), sparse ? 0 : -GLint(vmin));
    sub(2);
    mark();
    if (prof) {
        ++pfDraws; pfVerts += nv; pfIdx += idx.size();
        const uint64_t now = SDL_GetTicks();
        if (now - pfLast >= 4000) {
            const double k2 = 1000.0 / double(SDL_GetPerformanceFrequency()) / ((now - pfLast) / 1000.0);
            RLOG("[rsxprof] upload+draw ms/s: attributes %.0f, index copy %.0f, draw call %.0f; ring fence waits %llu (%.0f ms/s)\n", pfSub[0] * k2, pfSub[1] * k2, pfSub[2] * k2, (unsigned long long)pfRingWaits, pfRingWaitT * k2);
            pfSub[0] = pfSub[1] = pfSub[2] = 0; pfRingWaits = pfRingWaitT = 0;
        }
        if (now - pfLast >= 4000) {
            const double k = 1000.0 / double(SDL_GetPerformanceFrequency()) / ((now - pfLast) / 1000.0);
            std::string o; char b[64];
            for (int q = 0; q < 6; ++q) { std::snprintf(b, sizeof b, " %s %.0f,", pfName[q], pfT[q] * k); o += b; pfT[q] = 0; }
            if (pfLast) RLOG("[rsxprof] ms/s:%s | per second: %.0f draws, %.0f k vertices, %.0f k indices\n", o.c_str(), pfDraws * 1000.0 / (now - pfLast), pfVerts / double(now - pfLast), pfIdx / double(now - pfLast));
            if (pfLast) { RLOG("[rsxprof] texture units: %llu memo hits, misses: %llu new, %llu generation, %llu age, %llu sampler state\n", (unsigned long long)g_memoHits, (unsigned long long)g_memoMiss[0], (unsigned long long)g_memoMiss[1], (unsigned long long)g_memoMiss[2], (unsigned long long)g_memoMiss[3]); g_memoHits = 0; for (auto& x : g_memoMiss) x = 0; }
            pfDraws = pfVerts = pfIdx = 0; pfLast = now;
        }
    }
    {   // RSXSTEP=frame: the main surface after every draw of that frame, 640x360 RGB, appended to rsxstep.bin
        static const long stepFrame = std::getenv("RSXSTEP") ? std::atol(std::getenv("RSXSTEP")) : -1;
        if (long(frameNo) == stepFrame && cur.w == 2560 && cur.h == 720) {
            static FILE* sf = std::fopen("rsxstep.bin", "wb");
            static std::vector<uint8_t> full(2560 * 720 * 4), small(640 * 360 * 3);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, cur.fbo);
            glReadPixels(0, 0, 2560, 720, GL_RGBA, GL_UNSIGNED_BYTE, full.data());
            for (int y = 0; y < 360; ++y)
                for (int x = 0; x < 640; ++x)
                    std::memcpy(&small[(y * 640 + x) * 3], &full[((y * 2) * 2560 + x * 4) * 4], 3);
            if (sf) { const uint32_t n = uint32_t(drawsThisFrame - 1); std::fwrite(&n, 4, 1, sf); std::fwrite(small.data(), 1, small.size(), sf); std::fflush(sf); }
        }
    }
    static const long logFrame = std::getenv("RSXLOG") ? std::atol(std::getenv("RSXLOG")) : -1;
    if (long(frameNo) == logFrame) {
        RLOG("[draw] #%llu prog %016llx fb %u (%ux%u, color %x z %x fmt %08x ct %x) prim %u idx %zu verts %u attrs %d vp %u fp %x tex %x | blend %u depth %u/%x mask %x cull %u alpha %u | pos0 %g %g %g %g\n",
             (unsigned long long)drawsThisFrame - 1, (unsigned long long)p->key, cur.fbo, cur.w, cur.h, R[0x210 >> 2], R[0x214 >> 2], R[0x208 >> 2], R[0x220 >> 2], prim, idx.size(), nv, na, R[0x1ea0 >> 2], fpEa, p->texUsed,
             R[0x310 >> 2], R[0xa74 >> 2], R[0xa6c >> 2], R[0x324 >> 2], R[0x183c >> 2], R[0x304 >> 2], na ? vtx[0] : 0.f, na ? vtx[1] : 0.f, na ? vtx[2] : 0.f, na ? vtx[3] : 0.f);
        for (int i = 0; i < 16; ++i) if (p->texUsed & (1u << i)) {
            const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
            RLOG("[draw]   tex %d off %x fmt %08x addr %08x ctl0 %08x remap %08x filt %08x rect %08x ctl3 %08x\n", i, R[base >> 2], R[(base + 4) >> 2], R[(base + 8) >> 2],
                 R[(base + 0xc) >> 2], R[(base + 0x10) >> 2], R[(base + 0x14) >> 2], R[(base + 0x18) >> 2], R[(0x1840 + 4 * i) >> 2]);
        }
        {
            uint8_t px[4] = {};
            glBindFramebuffer(GL_READ_FRAMEBUFFER, cur.fbo);
            glReadPixels(cur.w / 2, cur.h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            uint8_t c0[4] = {}, c1[4] = {}, c2[4] = {};
            glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c0);
            glReadPixels(cur.w - 2, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c1);
            glReadPixels(1, cur.h - 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c2);
            RLOG("[draw]   corners: %u %u %u %u | %u %u %u %u | %u %u %u %u\n", c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3], c2[0], c2[1], c2[2], c2[3]);
            std::string is; char ib[16];
            for (size_t q = 0; q < idx.size() && q < 12; ++q) { std::snprintf(ib, sizeof ib, " %u", idx[q]); is += ib; }
            RLOG("[draw]   indices:%s (ranges %zu, first %u count %u, indexed %d)\n", is.c_str(), ranges.size(), ranges[0].first, ranges[0].count, int(rangesIndexed));
            GLint sb[4] = {}, vp[4] = {}, cmk[4] = {}, fb = 0, dfb = 0;
            glGetIntegerv(0x0C10, sb); glGetIntegerv(0x0BA2, vp); glGetIntegerv(0x0C23, cmk); glGetIntegerv(0x8CA6, &dfb); glGetIntegerv(0x8CAA, &fb);
            RLOG("[draw]   GL scissor %d %d %d %d (on %d) viewport %d %d %d %d colormask %d %d %d %d drawfb %d readfb %d depth %d stencil %d cull %d blend %d rd %d\n", sb[0], sb[1], sb[2], sb[3], glIsEnabled(GL_SCISSOR_TEST),
                 vp[0], vp[1], vp[2], vp[3], cmk[0], cmk[1], cmk[2], cmk[3], dfb, fb, glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_BLEND), glIsEnabled(0x8C89));
            RLOG("[draw]   center pixel after draw: %u %u %u %u, GL error %x, stencil %u depthbounds %x\n", px[0], px[1], px[2], px[3], glGetError(), R[0x328 >> 2], R[0x380 >> 2]);
        }
        for (int a = 0; a < 16; ++a) if ((p->vpInputs & (1u << a)) && !((R[(0x1740 + 4 * a) >> 2] >> 4) & 0xf))
            RLOG("[draw]   const attr %d: %g %g %g %g\n", a, rf(0x1c00 + 16 * a), rf(0x1c04 + 16 * a), rf(0x1c08 + 16 * a), rf(0x1c0c + 16 * a));
        if (!g_fpConsts.empty()) RLOG("[draw]   fc0 %g %g %g %g (%zu consts) vpInputs %x\n", g_fpConsts[0], g_fpConsts[1], g_fpConsts[2], g_fpConsts[3], g_fpConsts.size() / 4, p->vpInputs);
        RLOG("[draw]   scissor %08x %08x viewport %08x %08x clip %08x %08x window %08x ctl %08x\n", R[0x8c0 >> 2], R[0x8c4 >> 2], R[0xa00 >> 2], R[0xa04 >> 2], R[0x200 >> 2], R[0x204 >> 2], R[0x1d88 >> 2], R[0x1d60 >> 2]);
        for (uint32_t v = 0; v < nv && v < 6; ++v) RLOG("[draw]   v%u: %g %g %g %g | %g %g\n", v, vtx[v * 4], vtx[v * 4 + 1], vtx[v * 4 + 2], vtx[v * 4 + 3], na > 2 ? vtx[(2 * nv + v) * 4] : 0.f, na > 2 ? vtx[(2 * nv + v) * 4 + 1] : 0.f);
        RLOG("[draw]   vc467 %g %g %g %g vc4 %g %g %g %g vscale %g %g %g voff %g %g %g\n", vpConst[467 * 4], vpConst[467 * 4 + 1], vpConst[467 * 4 + 2], vpConst[467 * 4 + 3],
             vpConst[16], vpConst[17], vpConst[18], vpConst[19], vs[0], vs[1], vs[2], vo[0], vo[1], vo[2]);
        for (int k = 0; k < na; ++k) {
            const uint32_t fo = R[(0x1680 + 4 * attrs[k]) >> 2];
            const uint32_t e = ea_of(fo & 0x7fffffff, fo >> 31) + vmin * ((R[(0x1740 + 4 * attrs[k]) >> 2] >> 8) & 0xff);
            RLOG("[draw]   attr %d fmt %08x off %08x raw %08x %08x %08x %08x -> %g %g %g %g\n", attrs[k], R[(0x1740 + 4 * attrs[k]) >> 2], fo, be32(e), be32(e + 4), be32(e + 8), be32(e + 12),
                 vtx[size_t(k) * nv * 4], vtx[size_t(k) * nv * 4 + 1], vtx[size_t(k) * nv * 4 + 2], vtx[size_t(k) * nv * 4 + 3]);
        }
    }
    for (int k = 0; k < na; ++k) glDisableVertexAttribArray(attrs[k]);
}

// ------------------------------------------------------------------ final image: antialiasing and sharpening
// Optional passes between the game's display buffer and the window (menu F1). FXAA: luminance edges are blurred
// along their direction (the classic "FXAA console" scheme), in the display buffer's own resolution.
// Sharpening: contrast adaptive (strong where the neighbourhood is flat, weak near strong contrast so edges do not
// ring), applied while scaling to the window. Without either the display buffer is just blitted.
static const char* const kPostVs =
    "#version 450 core\n"
    "out vec2 uv;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char* const kFxaaFs =
    "#version 450 core\n"
    "layout(binding = 15) uniform sampler2D src;\n"
    "uniform vec4 u_p;\n"
    "in vec2 uv; out vec4 o;\n"
    "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "void main() {\n"
    "  vec2 rcp = u_p.xy;\n"
    "  vec3 m = texture(src, uv).rgb;\n"
    "  float lM = lum(m);\n"
    "  float lNW = lum(texture(src, uv + vec2(-1.0, 1.0) * rcp).rgb), lNE = lum(texture(src, uv + vec2(1.0, 1.0) * rcp).rgb);\n"
    "  float lSW = lum(texture(src, uv + vec2(-1.0, -1.0) * rcp).rgb), lSE = lum(texture(src, uv + vec2(1.0, -1.0) * rcp).rgb);\n"
    "  float mn = min(lM, min(min(lNW, lNE), min(lSW, lSE))), mx = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  if (mx - mn < max(0.0312, mx * 0.125)) { o = vec4(m, 1.0); return; }\n"
    "  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * rcp;\n"
    "  vec3 a = 0.5 * (texture(src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  vec3 b = a * 0.5 + 0.25 * (texture(src, uv - dir * 0.5).rgb + texture(src, uv + dir * 0.5).rgb);\n"
    "  float lB = lum(b);\n"
    "  o = vec4((lB < mn || lB > mx) ? a : b, 1.0);\n"
    "}\n";
static const char* const kSharpFs =
    "#version 450 core\n"
    "layout(binding = 15) uniform sampler2D src;\n"
    "uniform vec4 u_p;\n"
    "in vec2 uv; out vec4 o;\n"
    "void main() {\n"
    "  vec2 rcp = u_p.xy; vec2 u = vec2(uv.x, 1.0 - uv.y);\n"   // display buffers are stored upside down
    "  vec3 c = texture(src, u).rgb;\n"
    "  if (u_p.z <= 0.0) { o = vec4(c, 1.0); return; }\n"
    "  vec3 n = texture(src, u + vec2(0.0, rcp.y)).rgb, s = texture(src, u - vec2(0.0, rcp.y)).rgb;\n"
    "  vec3 e = texture(src, u + vec2(rcp.x, 0.0)).rgb, w = texture(src, u - vec2(rcp.x, 0.0)).rgb;\n"
    "  vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));\n"
    "  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));\n"
    "  vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, u_p.z));\n"
    "  o = vec4(clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0), 1.0);\n"
    "}\n";
static GLuint post_program(const char* fs) {
    const GLuint p = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, kPostVs, "post vertex"), f = compile(GL_FRAGMENT_SHADER, fs, "post fragment");
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { RLOG("[rsx] post program failed to link\n"); return 0; }
    return p;
}
// Draws the display buffer `src` (sw x sh) into the window rectangle; false when the passes are off or unusable
static bool post_present(GLuint src, int sw, int sh, int dx, int dy, int dw, int dh) {
    const Settings& st = settings();
    if (!st.fxaa && !st.sharpen) return false;
    static GLuint fxaaProg = 0, sharpProg = 0, mid = 0, midFbo = 0, pvao = 0;
    static GLint fxaaP = -1, sharpP = -1;
    static int midW = 0, midH = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fxaaProg = post_program(kFxaaFs); sharpProg = post_program(kSharpFs);
        if (fxaaProg) fxaaP = glGetUniformLocation(fxaaProg, "u_p");
        if (sharpProg) sharpP = glGetUniformLocation(sharpProg, "u_p");
        glGenVertexArrays(1, &pvao); glGenFramebuffers(1, &midFbo);
    }
    if (!fxaaProg || !sharpProg) return false;
    ++g_texGen;   // unit 15 is rebound here
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(pvao);
    glActiveTexture(GL_TEXTURE0 + 15);
    GLuint img = src;
    if (st.fxaa) {
        if (!mid || midW != sw || midH != sh) {
            if (!mid) glGenTextures(1, &mid);
            glBindTexture(GL_TEXTURE_2D, mid);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mid, 0);
            const GLenum b0 = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &b0);
            midW = sw; midH = sh;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
        glViewport(0, 0, sw, sh);
        glBindTexture(GL_TEXTURE_2D, src);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUseProgram(fxaaProg);
        const float p[4] = {1.f / sw, 1.f / sh, 0, 0};
        glUniform4fv(fxaaP, 1, p);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        img = mid;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(dx, dy, dw, dh);
    glBindTexture(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram(sharpProg);
    static const float amount[4] = {0.f, 0.3f, 0.6f, 1.f};
    const float p[4] = {1.f / sw, 1.f / sh, amount[std::clamp(st.sharpen, 0, 3)], 0};
    glUniform4fv(sharpP, 1, p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glViewport(0, 0, dw, dh);
    return true;
}


// ------------------------------------------------------------------ device
static bool init_gl() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    glc = SDL_GL_CreateContext(win);
    if (!glc) { RLOG("[rsx] GL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_SetSwapInterval(settings().vsync);
#define RSX_LOAD(ret, name, ...) name = reinterpret_cast<PFN_##name>(SDL_GL_GetProcAddress(#name)); if (!name) { RLOG("[rsx] missing GL function %s\n", #name); return false; }
    RSX_GL(RSX_LOAD)
#undef RSX_LOAD
    RLOG("[rsx] OpenGL %s on %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glGenBuffers(1, &ibo); glGenBuffers(1, &ubo);
    ring_init();
    glEnable(0x8D69 /* GL_PRIMITIVE_RESTART_FIXED_INDEX */);
    glEnable(GL_PROGRAM_POINT_SIZE);
    if (std::getenv("RSXCLAMP")) glEnable(GL_DEPTH_CLAMP);
    if (dumpShaders) SDL_CreateDirectory("rsxdump");
    glMaxShaderCompilerThreadsKHR = reinterpret_cast<PFN_glMaxShaderCompilerThreadsKHR>(SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR"));
    if (!glMaxShaderCompilerThreadsKHR) glMaxShaderCompilerThreadsKHR = reinterpret_cast<PFN_glMaxShaderCompilerThreadsKHR>(SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsARB"));
    g_parallel = glMaxShaderCompilerThreadsKHR && !std::getenv("GLSYNC");
    if (g_parallel) glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
    RLOG("[rsx] shader programs: %s\n", g_parallel ? "built in parallel by the driver (draws wait for them)" : "built when first used");
    glOk = true;
    return true;
}

// ------------------------------------------------------------------ overlays (menu, notifications)
static const char* const kOverlayFs =
    "#version 450 core\n"
    "layout(binding = 15) uniform sampler2D src;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(src, vec2(uv.x, 1.0 - uv.y)); }\n";   // bitmap row 0 is its top
// a bitmap (premultiplied BGRA) over the window image, blended by its alpha
static void draw_overlay(int k, const Overlay& o, int wh) {
    static GLuint tex[2] = {}, prog = 0, ovao = 0;
    static bool tried = false;
    if (!o.px) return;
    if (!tried) { tried = true; prog = post_program(kOverlayFs); glGenVertexArrays(1, &ovao); }
    if (!prog) return;
    glActiveTexture(GL_TEXTURE0 + 15); ++g_texGen;
    if (!tex[k]) {
        glGenTextures(1, &tex[k]);
        glBindTexture(GL_TEXTURE_2D, tex[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex[k]);
    if (o.changed) {
        glPixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /* GL_RGBA8 */, o.w, o.h, 0, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, o.px);
    }
    stateValid = false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(1, 0x303, 1, 0x303);   // ONE, ONE_MINUS_SRC_ALPHA
    glBlendEquationSeparate(0x8006, 0x8006);
    glViewport(o.dx, wh - o.dy - o.dh, o.dw, o.dh);
    glBindVertexArray(ovao);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glBindVertexArray(vao);
}

// ------------------------------------------------------------------ present
static void present_gl(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    for (int k = 0; k < 4; ++k) glColorMaski(k, 1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    auto it = surfaces.find(uint64_t(d.offset));
    if (it != surfaces.end()) {
        static GLuint readFbo = 0;
        if (!readFbo) glGenFramebuffers(1, &readFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.tex, 0);
        // keep the aspect ratio
        const float sa = float(d.w ? d.w : it->second.w) / float(d.h ? d.h : it->second.h), wa = float(ww) / float(wh);
        int dw = ww, dh = wh;
        if (wa > sa) dw = int(wh * sa); else dh = int(ww / sa);
        const int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
        if (!post_present(it->second.tex, int(it->second.w), int(it->second.h), dx, dy, dw, dh))
            glBlitFramebuffer(0, 0, it->second.w, it->second.h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);   // the post passes bind GL_FRAMEBUFFER (the screenshots below read through readFbo)
        {   // RSXBURST=seconds,n: n consecutive images from that second, kept in memory at 640x360 and written when the
            // burst ends (rsxburst_NNN.bmp + their times in the log): shows motion image by image without slowing them
            static double from = -1; static int want = 0; static bool parsed = false, done = false;
            static std::vector<std::vector<uint8_t>> frames; static std::vector<double> times;
            static GLuint smallFbo = 0, smallTex = 0;
            if (!parsed) { parsed = true; if (const char* e = std::getenv("RSXBURST")) std::sscanf(e, "%lf,%d", &from, &want); }
            if (want > 0 && !done && SDL_GetTicks() >= from * 1000) {
                if (!smallFbo) {
                    glGenFramebuffers(1, &smallFbo); glGenTextures(1, &smallTex);
                    glBindTexture(GL_TEXTURE_2D, smallTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, smallFbo);
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, smallTex, 0);
                    ++g_texGen;
                }
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, smallFbo);
                glBlitFramebuffer(0, 0, it->second.w, it->second.h, 0, 0, 640, 360, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, smallFbo);
                frames.emplace_back(size_t(640) * 360 * 4);
                glReadPixels(0, 0, 640, 360, GL_RGBA, GL_UNSIGNED_BYTE, frames.back().data());
                times.push_back(SDL_GetTicksNS() / 1e6);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                if (int(frames.size()) >= want) {
                    done = true;
                    for (size_t k = 0; k < frames.size(); ++k) {
                        char fn[64]; std::snprintf(fn, sizeof fn, "rsxburst_%03zu.bmp", k);
                        if (SDL_Surface* srf = SDL_CreateSurfaceFrom(640, 360, SDL_PIXELFORMAT_ABGR8888, frames[k].data(), 640 * 4)) { SDL_SaveBMP(srf, fn); SDL_DestroySurface(srf); }
                        RLOG("[burst] %zu at %.2f ms\n", k, times[k] - times[0]);
                    }
                    frames.clear();
                }
            }
        }
        static const int shotEvery = std::getenv("RSXSHOT") ? std::atoi(std::getenv("RSXSHOT")) : 0;
        static const double shotFrom = std::getenv("RSXSHOTFROM") ? std::atof(std::getenv("RSXSHOTFROM")) : 0, shotTo = std::getenv("RSXSHOTTO") ? std::atof(std::getenv("RSXSHOTTO")) : 1e9;   // seconds after launch
        if (shotEvery > 0 && frameNo % shotEvery == 0 && SDL_GetTicks() >= shotFrom * 1000 && SDL_GetTicks() <= shotTo * 1000) {   // RSXSHOT=N: save every color surface every N frames
            RLOG("[shot] frame %llu at %.2f s\n", (unsigned long long)frameNo, SDL_GetTicks() / 1000.0);
            for (auto& sf : surfaces) {
                if (sf.second.depth && std::getenv("RSXSHOTZ") && sf.second.w >= 512 && sf.second.w == sf.second.h) {   // RSXSHOTZ=1: square depth surfaces (shadow maps) as raw floats
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, 0x821A /* DEPTH_STENCIL_ATTACHMENT */, GL_TEXTURE_2D, sf.second.tex, 0);
                    std::vector<float> z(size_t(sf.second.w) * sf.second.h);
                    glReadPixels(0, 0, int(sf.second.w), int(sf.second.h), GL_DEPTH_COMPONENT, 0x1406 /* FLOAT */, z.data());
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, 0x821A, GL_TEXTURE_2D, 0, 0);
                    char fn[96];
                    std::snprintf(fn, sizeof fn, "rsxshot_%05llu_Z%x_%u.f32", (unsigned long long)frameNo, uint32_t(sf.first), sf.second.w);
                    if (FILE* zf = std::fopen(fn, "wb")) { std::fwrite(z.data(), 4, z.size(), zf); std::fclose(zf); }
                    continue;
                }
                if (sf.second.depth) continue;
                if (std::getenv("RSXSHOTD") && sf.first != uint64_t(d.offset)) continue;   // RSXSHOTD=1: the display buffer only
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sf.second.tex, 0);
                const int sw = int(sf.second.w), sh = int(sf.second.h);
                std::vector<uint8_t> px(size_t(sw) * sh * 4);
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                SDL_Surface* srf = SDL_CreateSurfaceFrom(sw, sh, SDL_PIXELFORMAT_ABGR8888, px.data(), sw * 4);
                char fn[96];
                std::snprintf(fn, sizeof fn, "rsxshot_%05llu_%s%x.bmp", (unsigned long long)frameNo, sf.first == uint64_t(d.offset) ? "DISPLAY_" : "", uint32_t(sf.first));
                if (srf) { SDL_SaveBMP(srf, fn); SDL_DestroySurface(srf); }
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else {
        static int warned = 0;
        if (warned++ < 5) RLOG("[rsx] flip: display buffer at 0x%x was never rendered\n", d.offset);
    }
    for (int k = 0; k < nOvl && k < 2; ++k) draw_overlay(k, ovl[k], wh);
    {   // SWAPLOG=1: how long presenting takes (it blocks when the compositor or the driver throttles)
        static const bool swapLog = std::getenv("SWAPLOG") != nullptr;
        const uint64_t ts = SDL_GetTicksNS();
        SDL_GL_SwapWindow(win);
        g_swapUs += (SDL_GetTicksNS() - ts) / 1000;
        // [gpu]: when the GPU really finished each image, measured from the flip (GPU timestamp after the swap).
        // Past one refresh the image cannot be shown at the next refresh: the display repeats the previous one (a
        // visible skip) although the game produced every image on time.
        static const bool gpuTime = !(std::getenv("GPUTIME") && std::getenv("GPUTIME")[0] == '0');   // on by default (problem reports); GPUTIME=0 off
        if (gpuTime && glQueryCounter && glGetQueryObjectui64v && glGetInteger64v) {
            struct Q { GLuint id; uint64_t cpuNs; };
            static std::deque<Q> pending; static std::vector<GLuint> freeQ;
            static int64_t offNs = 0; static uint64_t calibAt = 0;
            static uint64_t n = 0, over = 0, worst = 0, sum = 0, lastLog = 0;
            const uint64_t nowNs = SDL_GetTicksNS();
            if (!calibAt || nowNs - calibAt > 1000000000ull) {   // GPU clock -> CPU clock
                int64_t g = 0; glGetInteger64v(0x8E28 /* GL_TIMESTAMP */, &g);
                offNs = int64_t(SDL_GetTicksNS()) - g; calibAt = nowNs;
            }
            GLuint q;
            if (freeQ.empty()) glGenQueries(1, &q); else { q = freeQ.back(); freeQ.pop_back(); }
            glQueryCounter(q, 0x8E28);
            pending.push_back({q, ts});
            static const uint64_t periodNs = 1000000000ull / uint64_t(settings().gameHz > 0 ? settings().gameHz : 60);
            while (!pending.empty()) {
                GLint avail = 0; glGetQueryObjectiv(pending.front().id, 0x8867 /* RESULT_AVAILABLE */, &avail);
                if (!avail) break;
                uint64_t g = 0; glGetQueryObjectui64v(pending.front().id, 0x8866 /* RESULT */, &g);
                const int64_t lat = int64_t(g) + offNs - int64_t(pending.front().cpuNs);
                const uint64_t l = lat > 0 ? uint64_t(lat) : 0;
                ++n; sum += l; worst = std::max(worst, l); if (l > periodNs) ++over;
                freeQ.push_back(pending.front().id); pending.pop_front();
            }
            if (nowNs - lastLog > 2000000000ull && n) {
                RLOG("[gpu] %.1f s: %llu images, GPU done %.2f ms after the flip on average, %.2f at worst, %llu past one refresh, %zu in flight\n",
                     nowNs / 1e9, (unsigned long long)n, sum / 1e6 / n, worst / 1e6, (unsigned long long)over, pending.size());
                n = over = worst = sum = 0; lastLog = nowNs;
            }
        }
        if (swapLog) { static uint64_t sum = 0, mx = 0, n = 0; const uint64_t d = SDL_GetTicksNS() - ts; sum += d; mx = std::max(mx, d); if (++n == 240) { RLOG("[swap] 240 presents: %.2f ms average, %.2f ms longest\n", sum / 240e6, mx / 1e6); sum = mx = n = 0; } }
    }
    if (const GLenum e = glGetError()) { static int n = 0; if (n++ < 20) RLOG("[rsx] GL error 0x%x (frame %llu)\n", e, (unsigned long long)frameNo); }
}

} // namespace

struct GlBackend final : Backend {
    uint32_t window_flags() override { return SDL_WINDOW_OPENGL; }
    bool init() override { return init_gl(); }
    void new_frame() override { stateValid = false; }
    void clear(uint32_t mask) override {
        clear_surface(mask);
        static const long logFrame = std::getenv("RSXLOG") ? std::atol(std::getenv("RSXLOG")) : -1;
        if (long(frameNo) == logFrame) RLOG("[clear] fb %u\n", cur.fbo);
    }
    void draw() override { do_draw(); }
    void present(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) override { present_gl(d, ww, wh, ovl, nOvl); }
    void set_vsync(bool on) override { SDL_GL_SetSwapInterval(on); }
    void textures_changed() override { ++g_texGen; }
    const char* gpu_name() override { return reinterpret_cast<const char*>(glGetString(0x1F01 /* GL_RENDERER */)); }
    const char* api_name() override { return "OpenGL"; }
};
Backend* make_gl_backend() { static GlBackend b; return &b; }

} // namespace gti::recomp::rsx
