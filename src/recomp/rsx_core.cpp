// RSX front end (see rsx_core.h): NV4097 method stream, register state, microcode translation, vertex and index
// conversion, window, input, options menu and notifications. Drawing is done by the back end in B.
#include "recomp/host_ui.h"
#include "recomp/rsx_core.h"
#include "recomp/settings.h"
#include "recomp/lv2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <immintrin.h>

namespace gti::recomp::netplay { bool in_room(); }
namespace gti::recomp { void glyphs_update(); }   // button_glyphs.cpp
namespace gti::recomp::netplay { std::string local_address(); std::string player_name(); int upnp_state(); }   // netplay.cpp
namespace gti::recomp { bool host_input_text(const wchar_t* title, const wchar_t* prompt, std::string& value, int maxLen); }   // host_win.cpp
namespace gti::recomp { void host_restart(); void host_report(const char* gpu); std::vector<std::string> host_camera_list(); void host_camera_stop(); bool host_toast_bitmap(const wchar_t* title, const wchar_t* name, const wchar_t* detail, uint32_t rgb, const uint8_t* png, size_t pngSize, int w, int h, uint32_t* out); bool host_banner_bitmap(const wchar_t* line1, const wchar_t* line2, const wchar_t* line3, int w, int h, uint32_t* out); }   // host_win.cpp
namespace gti::recomp::rsx {

std::mutex g_logMx;
Config C;
uint32_t R[0x4000];
uint32_t vpCode[512 * 4];
float vpConst[468 * 4];
bool vpConstDirty = true;
uint32_t vpDirtyLo = 0, vpDirtyHi = 468 * 4;
static uint32_t vpLoad = 0, cLoad = 0;         // load cursors (in words / constants)
static uint32_t vpConstWord = 0;
uint64_t vpGen = 1;
std::vector<Range> ranges;
bool rangesIndexed = false;
uint32_t prim = 0;
Display displays[8];
SDL_Window* win = nullptr;
uint64_t frameNo = 0, drawsThisFrame = 0;
const bool dumpShaders = std::getenv("RSXDUMP") != nullptr;
static const bool rsxTrace = std::getenv("RSXTRACE") != nullptr;
Backend* B = nullptr;
static bool ready = false, initTried = false;  // the back end is up / its start was attempted
static std::atomic<bool> quitFlag{false};
static void set_display_mode(int mode);
static int g_lastFull = 1;   // the full-screen mode F11 goes back to

uint64_t g_texUploads = 0, g_texUploadBytes = 0, g_streamBuilds = 0, g_streamVerts = 0, g_progBuilds = 0;
std::atomic<uint64_t> g_swapUs{0};
uint64_t g_drawTicks = 0, g_drawCount = 0, g_progTicks = 0, g_texTicks = 0, g_convTicks = 0, g_bufTicks = 0;

bool enabled() { static const bool on = !(std::getenv("RSX") && std::getenv("RSX")[0] == '0'); return on; }
bool quit_requested() { return quitFlag.load(); }

float half2f(uint16_t h) {
    const uint32_t sgn = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    uint32_t u;
    if (e == 0) {
        if (!m) u = sgn;
        else { const float v = float(m) * (1.f / 16777216.f); std::memcpy(&u, &v, 4); u |= sgn; }   // subnormal: m * 2^-24
    } else if (e == 31) u = sgn | 0x7f800000u | (m << 13);
    else u = sgn | ((e + 112) << 23) | (m << 13);
    float f; std::memcpy(&f, &u, 4); return f;
}

// ------------------------------------------------------------------ vertex program -> GLSL
// Instruction layout (4 words) as documented by the nouveau / RPCS3 projects for NV40-class vertex engines.

static const char* swz4(uint32_t x, uint32_t y, uint32_t z, uint32_t w, char* buf) {
    static const char c[] = "xyzw";
    buf[0] = '.'; buf[1] = c[x]; buf[2] = c[y]; buf[3] = c[z]; buf[4] = c[w]; buf[5] = 0;
    return buf;
}
static const char* vpOut(uint32_t d) {
    static const char* n[] = {"o_pos", "v_col0", "v_col1", "v_bfc0", "v_bfc1", "v_fog", "o_psz",
                              "v_tc[0]", "v_tc[1]", "v_tc[2]", "v_tc[3]", "v_tc[4]", "v_tc[5]", "v_tc[6]", "v_tc[7]", "v_tc[8]", "v_tc[9]"};
    return d < 17 ? n[d] : "o_dummy";
}

VpInfo translate_vp(const uint32_t* code, uint32_t start) {
    VpInfo vi;
    std::string b;
    char t[512], sw[8];
    for (uint32_t pc = start; pc < 512; ++pc) {
        const uint32_t* in = code + pc * 4;
        const uint32_t d0 = in[0], d1 = in[1], d2 = in[2], d3 = in[3];
        ++vi.len;
        const uint32_t vecOp = (d1 >> 22) & 0x1f, scaOp = (d1 >> 27) & 0x1f;
        const uint32_t inputSrc = (d1 >> 8) & 0xf, constSrc = (d1 >> 12) & 0x3ff;
        const uint32_t src[3] = {((d1 & 0xff) << 9) | (d2 >> 23), (d2 >> 6) & 0x1ffff, ((d2 & 0x3f) << 11) | (d3 >> 21)};
        const bool absS[3] = {bool((d0 >> 21) & 1), bool((d0 >> 22) & 1), bool((d0 >> 23) & 1)};
        const bool indexConst = (d3 >> 1) & 1;
        const uint32_t addrSel = (d0 >> 24) & 1, addrSwz = d0 & 3;
        auto operand = [&](int i) -> std::string {
            const uint32_t s = src[i];
            const uint32_t type = s & 3, tmp = (s >> 2) & 0x3f;
            std::string r;
            if (type == 1) { std::snprintf(t, sizeof t, "R[%u]", tmp); r = t; }
            else if (type == 2) { vi.inputs |= 1u << inputSrc; std::snprintf(t, sizeof t, "in%u", inputSrc); r = t; }
            else if (type == 3) {
                if (indexConst) std::snprintf(t, sizeof t, "vc[clamp(%u + A%u.%c, 0, 467)]", constSrc, addrSel, "xyzw"[addrSwz]);
                else std::snprintf(t, sizeof t, "vc[%u]", constSrc);
                r = t;
            } else r = "vec4(0.0)";
            r += swz4((s >> 14) & 3, (s >> 12) & 3, (s >> 10) & 3, (s >> 8) & 3, sw);
            if (absS[i]) r = "abs(" + r + ")";
            if ((s >> 16) & 1) r = "(-" + r + ")";
            return r;
        };
        const std::string a = operand(0), bb = operand(1), c = operand(2);
        std::string vexpr, sexpr;
        switch (vecOp) {
            case 0: break;
            case 1: vexpr = a; break;
            case 2: vexpr = a + "*" + bb; break;
            case 3: vexpr = a + "+" + c; break;
            case 4: vexpr = a + "*" + bb + "+" + c; break;
            case 5: vexpr = "vec4(dot((" + a + ").xyz,(" + bb + ").xyz))"; break;
            case 6: vexpr = "vec4(dot((" + a + ").xyz,(" + bb + ").xyz)+(" + bb + ").w)"; break;
            case 7: vexpr = "vec4(dot(" + a + "," + bb + "))"; break;
            case 8: vexpr = "vec4(1.0,(" + a + ").y*(" + bb + ").y,(" + a + ").z,(" + bb + ").w)"; break;
            case 9: vexpr = "min(" + a + "," + bb + ")"; break;
            case 10: vexpr = "max(" + a + "," + bb + ")"; break;
            case 11: vexpr = "vec4(lessThan(" + a + "," + bb + "))"; break;
            case 12: vexpr = "vec4(greaterThanEqual(" + a + "," + bb + "))"; break;
            case 13: vexpr = "ARL"; break;
            case 14: vexpr = "fract(" + a + ")"; break;
            case 15: vexpr = "floor(" + a + ")"; break;
            case 16: vexpr = "vec4(equal(" + a + "," + bb + "))"; break;
            case 17: vexpr = "vec4(0.0)"; break;
            case 18: vexpr = "vec4(greaterThan(" + a + "," + bb + "))"; break;
            case 19: vexpr = "vec4(lessThanEqual(" + a + "," + bb + "))"; break;
            case 20: vexpr = "vec4(notEqual(" + a + "," + bb + "))"; break;
            case 21: vexpr = "vec4(1.0)"; break;
            case 22: vexpr = "sign(" + a + ")"; break;
            case 25: vexpr = "vec4(0.0)"; break;   // TXL (vertex texture fetch): not used by this game so far
            default: vi.unsupported = true; std::snprintf(t, sizeof t, "  // unsupported vector op %u\n", vecOp); b += t; break;
        }
        const std::string s = "(" + c + ").x";
        switch (scaOp) {
            case 0: break;
            case 1: sexpr = "vec4(" + s + ")"; break;
            case 2: sexpr = "vec4(1.0/" + s + ")"; break;
            case 3: sexpr = "vec4(rcc(" + s + "))"; break;
            case 4: sexpr = "vec4(inversesqrt(abs(" + s + ")))"; break;
            case 5: sexpr = "vexp(" + s + ")"; break;
            case 6: sexpr = "vlog(" + s + ")"; break;
            case 7: sexpr = "vlit(" + c + ")"; break;
            case 13: sexpr = "vec4(log2(abs(" + s + ")))"; break;
            case 14: sexpr = "vec4(exp2(" + s + "))"; break;
            case 15: sexpr = "vec4(sin(" + s + "))"; break;
            case 16: sexpr = "vec4(cos(" + s + "))"; break;
            default: vi.unsupported = true; std::snprintf(t, sizeof t, "  // unsupported scalar op %u (flow control)\n", scaOp); b += t; break;
        }
        // condition (test against a condition register) shared by both units
        const bool condTest = (d0 >> 13) & 1;
        const uint32_t cond = (d0 >> 10) & 7;
        const bool condUpd = ((d0 >> 14) & 1) || ((d0 >> 29) & 1);
        const uint32_t ccSel = (d0 >> 25) & 1;
        const uint32_t cswz[4] = {(d0 >> 8) & 3, (d0 >> 6) & 3, (d0 >> 4) & 3, (d0 >> 2) & 3};
        const bool sat = (d0 >> 26) & 1;
        const uint32_t dst = (d3 >> 2) & 0x1f, dstTmp = (d0 >> 15) & 0x3f, scaTmp = (d3 >> 7) & 0x3f;
        const bool vecResult = (d0 >> 30) & 1;
        const bool vm[4] = {bool((d3 >> 16) & 1), bool((d3 >> 15) & 1), bool((d3 >> 14) & 1), bool((d3 >> 13) & 1)};
        const bool sm[4] = {bool((d3 >> 20) & 1), bool((d3 >> 19) & 1), bool((d3 >> 18) & 1), bool((d3 >> 17) & 1)};
        if (dst != 0x1f && dst < 17) vi.outputs |= 1u << dst;
        auto emit = [&](const std::string& value, const bool* mask, std::vector<std::string> targets) {
            if (condTest && cond == 0) return;
            std::string v = sat ? "clamp(" + value + ",0.0,1.0)" : value;
            b += "  t = " + v + ";\n";
            if (condUpd) { std::snprintf(t, sizeof t, "  CC%u = t;\n", ccSel); b += t; }   // (writes all components)
            for (const auto& tg : targets)
                for (int k = 0; k < 4; ++k) {
                    if (!mask[k]) continue;
                    const char comp = "xyzw"[k];
                    if (condTest && cond != 7) {
                        static const char* ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
                        std::snprintf(t, sizeof t, "  if (CC%u.%c %s 0.0) %s.%c = t.%c;\n", ccSel, "xyzw"[cswz[k]], ops[cond], tg.c_str(), comp, comp);
                    } else std::snprintf(t, sizeof t, "  %s.%c = t.%c;\n", tg.c_str(), comp, comp);
                    b += t;
                }
        };
        if (!vexpr.empty()) {
            if (vexpr == "ARL") {
                std::snprintf(t, sizeof t, "  A%u = ivec4(floor(%s));\n", addrSel, a.c_str());
                b += t;
            } else {
                std::vector<std::string> tg;
                if (dstTmp != 0x3f) { std::snprintf(t, sizeof t, "R[%u]", dstTmp); tg.push_back(t); }
                if (dst != 0x1f && (vecResult || scaOp == 0)) tg.push_back(vpOut(dst));
                emit(vexpr, vm, tg);
            }
        }
        if (!sexpr.empty()) {
            std::vector<std::string> tg;
            if (scaTmp != 0x3f) { std::snprintf(t, sizeof t, "R[%u]", scaTmp); tg.push_back(t); }
            else if (dst != 0x1f && (!vecResult || vecOp == 0)) tg.push_back(vpOut(dst));
            emit(sexpr, sm, tg);
        }
        if (d3 & 1) break;   // end of program
    }
    std::string h = "#version 450\n"
                    "layout(std140, binding = 0) uniform VC { vec4 vc[468]; };\n"
                    "uniform vec4 u_vscale, u_voffset, u_surf;\n";
    for (int k = 0; k < 16; ++k)
        if (vi.inputs & (1u << k)) { std::snprintf(t, sizeof t, "layout(location = %d) in vec4 in%d;\n", k, k); h += t; }
    h += "out vec4 v_col0, v_col1, v_bfc0, v_bfc1, v_fog;\nout vec4 v_tc[10];\n"
         "float rcc(float x) { float r = 1.0 / x; return sign(r) * clamp(abs(r), 5.42101e-20, 1.884467e19); }\n"
         "vec4 vexp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
         "vec4 vlog(float x) { float a = abs(x); float e = floor(log2(a)); return vec4(e, a / exp2(e), log2(a), 1.0); }\n"
         "vec4 vlit(vec4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0; return vec4(1.0, d, sp, 1.0); }\n"
         "void main() {\n"
         "  vec4 R[64]; for (int i = 0; i < 64; ++i) R[i] = vec4(0.0);\n"
         "  vec4 t, CC0 = vec4(0.0), CC1 = vec4(0.0); ivec4 A0 = ivec4(0), A1 = ivec4(0);\n"
         "  vec4 o_pos = vec4(0.0, 0.0, 0.0, 1.0), o_psz = vec4(1.0), o_dummy;\n"
         "  v_col0 = vec4(1.0); v_col1 = vec4(0.0); v_bfc0 = vec4(1.0); v_bfc1 = vec4(0.0); v_fog = vec4(0.0);\n"
         "  for (int i = 0; i < 10; ++i) v_tc[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
    // epilogue: the RSX viewport transform (window = ndc * scale + offset, window y down from the top of the
    // surface) folded into clip space so that GL clipping still works; FBO row 0 = surface memory row 0.
    if (std::getenv("RSXW1")) b += "  o_pos.w = 1.0;\n";
    if (std::getenv("RSXVCDBG")) b += "  v_col0 = vc[467]; v_col1 = vc[4];\n";
    vi.body = b;
    vi.glsl = h + b +
              "  gl_Position = vec4(o_pos.x * (2.0 * u_vscale.x / u_surf.x) + o_pos.w * (2.0 * u_voffset.x / u_surf.x - 1.0),\n"
              "                     o_pos.y * (2.0 * u_vscale.y / u_surf.y) + o_pos.w * (2.0 * u_voffset.y / u_surf.y - 1.0),\n"
              "                     o_pos.z * u_vscale.z + o_pos.w * u_voffset.z, o_pos.w);\n"
              "  gl_PointSize = o_psz.x;\n" +
              std::string(std::getenv("RSXVSDBG") && (vi.inputs & 1) ? "  v_col0 = vec4(gl_Position.x * 0.25 + 0.5, gl_Position.y * 0.25 + 0.5, gl_Position.w * 0.25, gl_Position.z * 0.25 + 0.5); gl_Position = vec4(float(gl_VertexID & 1) * 2.0 - 1.0, float(gl_VertexID & 2) - 1.0, 0.5, 1.0);\n" : "") +
              "}\n";
    return vi;
}

// ------------------------------------------------------------------ fragment program -> GLSL
static inline uint32_t fpw(uint32_t ea) { const uint32_t v = be32(ea); return (v << 16) | (v >> 16); }

uint32_t fp_size(uint32_t ea) {   // bytes up to and including the instruction with the end bit
    uint32_t o = 0;
    for (int n = 0; n < 2048; ++n) {
        const uint32_t dst = fpw(ea + o), s0 = fpw(ea + o + 4), s1 = fpw(ea + o + 8), s2 = fpw(ea + o + 12);
        o += 16;
        if (((s0 & 3) == 2) || ((s1 & 3) == 2) || ((s2 & 3) == 2)) o += 16;   // inline constant
        if (dst & 1) break;
    }
    return o;
}

// walks the program: hashes the instruction words only (the inline constants hold the uniform values the game
// patches into the microcode) and optionally collects the constants in program order
uint64_t fp_walk(uint32_t ea, uint64_t h, std::vector<float>* consts) {
    uint32_t o = 0;
    for (int n = 0; n < 2048; ++n) {
        uint32_t w[4];
        for (int k = 0; k < 4; ++k) w[k] = fpw(ea + o + 4 * k);
        h = fnv(w, 16, h);
        o += 16;
        if (((w[1] & 3) == 2) || ((w[2] & 3) == 2) || ((w[3] & 3) == 2)) {
            if (consts) for (int k = 0; k < 4; ++k) consts->push_back(f32(fpw(ea + o + 4 * k)));
            o += 16;
        }
        if (w[0] & 1) break;
    }
    return h;
}

// texDims: 2 bits per sampler: 0 = 2D, 1 = cube, 2 = 3D, 3 = 1D; texUn: unnormalised coordinates
FpInfo translate_fp(uint32_t ea, uint32_t texDims, uint32_t control) {
    FpInfo fi;
    std::string b;
    char t[640], sw[8];
    uint32_t o = 0;
    std::vector<uint32_t> elseAt, endAt;   // IFE blocks (word offsets)
    for (int n = 0; n < 2048; ++n) {
        while (!elseAt.empty() && elseAt.back() == o / 4) { b += "  } else {\n"; elseAt.pop_back(); }
        while (!endAt.empty() && endAt.back() == o / 4) { b += "  }\n"; endAt.pop_back(); }
        const uint32_t dst = fpw(ea + o), s0 = fpw(ea + o + 4), s1 = fpw(ea + o + 8), s2 = fpw(ea + o + 12);
        const bool hasConst = ((s0 & 3) == 2) || ((s1 & 3) == 2) || ((s2 & 3) == 2);
        const uint32_t constIdx = fi.nConst;
        if (hasConst) ++fi.nConst;
        const uint32_t opcode = ((dst >> 24) & 0x3f) | (((s1 >> 31) & 1) << 6);
        const uint32_t destReg = (dst >> 1) & 0x3f, attr = (dst >> 13) & 0xf, texNum = (dst >> 17) & 0xf;
        const bool fp16 = (dst >> 7) & 1, setCond = (dst >> 8) & 1, noDest = (dst >> 30) & 1, sat = (dst >> 31) & 1, bx2 = (dst >> 21) & 1;
        const uint32_t prec = (dst >> 22) & 3;
        const bool mask[4] = {bool((dst >> 9) & 1), bool((dst >> 10) & 1), bool((dst >> 11) & 1), bool((dst >> 12) & 1)};
        auto operand = [&](uint32_t s, bool absBit) -> std::string {
            const uint32_t type = s & 3, idx = (s >> 2) & 0x3f;
            const bool h = (s >> 8) & 1;
            std::string r;
            if (type == 0) { std::snprintf(t, sizeof t, "%s[%u]", h ? "H" : "R", idx); r = t; }
            else if (type == 1) {
                static const char* in[] = {"wpos", "v_col0", "v_col1", "v_fog", "v_tc[0]", "v_tc[1]", "v_tc[2]", "v_tc[3]", "v_tc[4]", "v_tc[5]",
                                           "v_tc[6]", "v_tc[7]", "v_tc[8]", "v_tc[9]", "ssa", "vec4(0.0)"};
                r = in[attr];
                if (attr == 1 || attr == 2) r = attr == 1 ? "col0()" : "col1()";
            } else if (type == 2) {
                std::snprintf(t, sizeof t, "fc[%u]", constIdx); r = t;
            }
            else r = "vec4(0.0)";
            r += swz4((s >> 9) & 3, (s >> 11) & 3, (s >> 13) & 3, (s >> 15) & 3, sw);
            if (absBit) r = "abs(" + r + ")";
            if ((s >> 17) & 1) r = "(-" + r + ")";
            return r;
        };
        const std::string a = operand(s0, (s0 >> 29) & 1), bb = operand(s1, (s1 >> 18) & 1), c = operand(s2, (s2 >> 18) & 1);
        auto tex = [&](const char* kind) -> std::string {
            fi.texUsed |= 1u << texNum;
            const uint32_t dim = (texDims >> (2 * texNum)) & 3;
            std::string coord = bx2 ? "(" + a + "*2.0-1.0)" : a;
            std::string r;
            const std::string smp = "tex" + std::to_string(texNum), sc = "u_tsc[" + std::to_string(texNum) + "]";
            if (dim == 1) {   // cube
                if (!std::strcmp(kind, "txl")) r = "textureLod(" + smp + ", (" + coord + ").xyz, (" + coord + ").w)";
                else r = "texture(" + smp + ", (" + coord + ").xyz)";
            } else if (dim == 2) r = "texture(" + smp + ", (" + coord + ").xyz)";
            else if (dim == 3) {   // depth texture with a compare function: 1 where the test passes (2x2 taps)
                const bool proj = !std::strcmp(kind, "txp");
                r = "vec4(shcmp(" + smp + ", " + (proj ? "(" + coord + ").xyz / (" + coord + ").w" : "(" + coord + ").xyz") + ", " + sc + ".z))";
            } else {
                std::string uv = "(" + coord + ").xy";
                if (!std::strcmp(kind, "txp")) uv = "((" + coord + ").xy / (" + coord + ").w)";
                uv = "(" + uv + " * " + sc + ".xy)";
                if (!std::strcmp(kind, "txl")) r = "textureLod(" + smp + ", " + uv + ", (" + coord + ").w)";
                else if (!std::strcmp(kind, "txb")) r = "texture(" + smp + ", " + uv + ", (" + coord + ").w)";
                else if (!std::strcmp(kind, "txd")) r = "textureGrad(" + smp + ", " + uv + ", (" + bb + ").xy, (" + c + ").xy)";
                else r = "texture(" + smp + ", " + uv + ")";
            }
            return r;
        };
        std::string e;
        bool kill = false;
        switch (opcode) {
            case 0x00: break;
            case 0x01: e = a; break;
            case 0x02: e = a + "*" + bb; break;
            case 0x03: e = a + "+" + bb; break;
            case 0x04: e = a + "*" + bb + "+" + c; break;
            case 0x05: e = "vec4(dot((" + a + ").xyz,(" + bb + ").xyz))"; break;
            case 0x06: e = "vec4(dot(" + a + "," + bb + "))"; break;
            case 0x07: e = "vec4(1.0,(" + a + ").y*(" + bb + ").y,(" + a + ").z,(" + bb + ").w)"; break;
            case 0x08: e = "min(" + a + "," + bb + ")"; break;
            case 0x09: e = "max(" + a + "," + bb + ")"; break;
            case 0x0A: e = "vec4(lessThan(" + a + "," + bb + "))"; break;
            case 0x0B: e = "vec4(greaterThanEqual(" + a + "," + bb + "))"; break;
            case 0x0C: e = "vec4(lessThanEqual(" + a + "," + bb + "))"; break;
            case 0x0D: e = "vec4(greaterThan(" + a + "," + bb + "))"; break;
            case 0x0E: e = "vec4(notEqual(" + a + "," + bb + "))"; break;
            case 0x0F: e = "vec4(equal(" + a + "," + bb + "))"; break;
            case 0x10: e = "fract(" + a + ")"; break;
            case 0x11: e = "floor(" + a + ")"; break;
            case 0x12: kill = true; break;
            case 0x13: e = "vec4(uintBitsToFloat(packSnorm4x8(" + a + ")))"; break;
            case 0x14: e = "unpackSnorm4x8(floatBitsToUint((" + a + ").x))"; break;
            case 0x15: e = "dFdx(" + a + ")"; break;
            case 0x16: e = "dFdy(" + a + ")"; break;
            case 0x17: e = tex("tex"); break;
            case 0x18: e = tex("txp"); break;
            case 0x19: e = tex("txd"); break;
            case 0x1A: e = "vec4(1.0/(" + a + ").x)"; break;
            case 0x1B: e = "vec4(inversesqrt(abs((" + a + ").x)))"; break;
            case 0x1C: e = "vec4(exp2((" + a + ").x))"; break;
            case 0x1D: e = "vec4(log2((" + a + ").x))"; break;
            case 0x1E: e = "flit(" + a + ")"; break;
            case 0x1F: e = "mix(" + c + "," + bb + "," + a + ")"; break;
            case 0x20: e = "vec4(1.0)"; break;
            case 0x21: e = "vec4(0.0)"; break;
            case 0x22: e = "vec4(cos((" + a + ").x))"; break;
            case 0x23: e = "vec4(sin((" + a + ").x))"; break;
            case 0x24: e = "vec4(uintBitsToFloat(packHalf2x16((" + a + ").xy)))"; break;
            case 0x25: e = "unpackHalf2x16(floatBitsToUint((" + a + ").x)).xyxy"; break;
            case 0x26: e = "vec4(pow((" + a + ").x,(" + bb + ").x))"; break;
            case 0x27: e = "vec4(uintBitsToFloat(packUnorm4x8(" + a + ")))"; break;
            case 0x28: e = "unpackUnorm4x8(floatBitsToUint((" + a + ").x))"; break;
            case 0x29: e = "vec4(uintBitsToFloat(packSnorm2x16((" + a + ").xy)))"; break;
            case 0x2A: e = "unpackSnorm2x16(floatBitsToUint((" + a + ").x)).xyxy"; break;
            case 0x2C: e = "vec4(uintBitsToFloat(packUnorm4x8(sqrt(" + a + "))))"; break;
            case 0x2D: e = "pow(unpackUnorm4x8(floatBitsToUint((" + a + ").x)), vec4(2.0))"; break;
            case 0x2E: e = "vec4(dot((" + a + ").xy,(" + bb + ").xy)+(" + c + ").x)"; break;
            case 0x2F: e = tex("txl"); break;
            case 0x31: e = tex("txb"); break;
            case 0x36: e = "frefl(" + a + "," + bb + ")"; break;
            case 0x38: e = "vec4(dot((" + a + ").xy,(" + bb + ").xy))"; break;
            case 0x39: e = "vec4(normalize((" + a + ").xyz), 1.0)"; break;
            case 0x3A: e = a + "/(" + bb + ").x"; break;
            case 0x3B: e = a + "*inversesqrt(abs((" + bb + ").x))"; break;
            case 0x3D: case 0x3E: break;   // FENCT / FENCB
            case 0x42: {   // IFE: if (condition) ... else ... end
                const uint32_t elseOff = s1 & 0x7fffffff, endOff = s2 & 0x7fffffff;
                const uint32_t cs[4] = {(s0 >> 21) & 3, (s0 >> 23) & 3, (s0 >> 25) & 3, (s0 >> 27) & 3};
                const bool lt = (s0 >> 18) & 1, eq = (s0 >> 19) & 1, gt = (s0 >> 20) & 1;
                const uint32_t ccr = (s0 >> 31) & 1;
                std::snprintf(t, sizeof t, "  if (fcond(CC%u.%c%c%c%c, %d, %d, %d)) {\n", ccr, "xyzw"[cs[0]], "xyzw"[cs[1]], "xyzw"[cs[2]], "xyzw"[cs[3]], lt, eq, gt);
                b += t;
                if (endOff != elseOff) elseAt.push_back(elseOff);
                endAt.push_back(endOff);
                o += 16;
                if (hasConst) o += 16;
                if (dst & 1) n = 99999;
                continue;
            }
            default:
                fi.unsupported = true;
                std::snprintf(t, sizeof t, "  // unsupported opcode 0x%x\n", opcode);
                b += t;
                break;
        }
        // execution condition
        const bool lt = (s0 >> 18) & 1, eq = (s0 >> 19) & 1, gt = (s0 >> 20) & 1;
        const uint32_t cs[4] = {(s0 >> 21) & 3, (s0 >> 23) & 3, (s0 >> 25) & 3, (s0 >> 27) & 3};
        const uint32_t ccr = (s0 >> 31) & 1, ccw = (s0 >> 30) & 1;
        const bool always = lt && eq && gt;
        auto condOf = [&](int k) -> std::string {
            std::snprintf(t, sizeof t, "fcond1(CC%u.%c, %d, %d, %d)", ccr, "xyzw"[cs[k]], lt, eq, gt);
            return t;
        };
        if (kill) {
            if (always) b += "  discard;\n";
            else b += "  if (" + condOf(0) + " || " + condOf(1) + " || " + condOf(2) + " || " + condOf(3) + ") discard;\n";
        }
        if (!e.empty()) {
            const uint32_t scale = (s1 >> 28) & 7;
            static const char* scl[] = {"", "*2.0", "*4.0", "*8.0", "", "*0.5", "*0.25", "*0.125"};
            std::string v = "(" + e + ")" + scl[scale];
            if (sat) v = "clamp(" + v + ",0.0,1.0)";
            else if (prec == 2) v = "clamp(" + v + ",-2.0,2.0)";   // fx12
            else if (prec == 1) v = "clamp(" + v + ",-65504.0,65504.0)";
            b += "  t = " + v + ";\n";
            if (setCond) {
                for (int k = 0; k < 4; ++k)
                    if (mask[k]) { std::snprintf(t, sizeof t, "  CC%u.%c = t.%c;\n", ccw, "xyzw"[k], "xyzw"[k]); b += t; }
            }
            if (!noDest) {
                std::snprintf(t, sizeof t, "%s[%u]", fp16 ? "H" : "R", destReg);
                const std::string tg = t;
                if (destReg == 0) (fp16 ? fi.writesH0 : fi.writesR0) = true;
                for (int k = 0; k < 4; ++k) {
                    if (!mask[k]) continue;
                    const char comp = "xyzw"[k];
                    if (always) std::snprintf(t, sizeof t, "  %s.%c = t.%c;\n", tg.c_str(), comp, comp);
                    else std::snprintf(t, sizeof t, "  if (%s) %s.%c = t.%c;\n", condOf(k).c_str(), tg.c_str(), comp, comp);
                    b += t;
                }
            }
        }
        o += 16;
        if (hasConst) o += 16;
        if (dst & 1) break;
    }
    while (!elseAt.empty()) { b += "  } else {\n"; elseAt.pop_back(); }
    while (!endAt.empty()) { b += "  }\n"; endAt.pop_back(); }
    fi.size = o;
    static const bool shDbg = std::getenv("SHDBG") != nullptr;   // debug: shadow-map results as the output colour
    std::string h = std::string("#version 450\n") + (shDbg ? "#define SHDBG\nfloat g_shS = -1.0; vec2 g_shRD = vec2(0.0);\n" : "") + "#define SHBIAS " + std::string(std::getenv("SHBIAS") ? std::getenv("SHBIAS") : "0.002") + "\n" +
                    "in vec4 v_col0, v_col1, v_bfc0, v_bfc1, v_fog;\nin vec4 v_tc[10];\n"
                    "uniform vec4 u_tsc[16];\nuniform vec4 u_alpha;\nuniform vec4 u_wpos;\n" + std::string("uniform vec4 fc[") + std::to_string(std::max(1u, fi.nConst)) + "];\n" +
                    "layout(location = 0) out vec4 o_c0;\nlayout(location = 1) out vec4 o_c1;\n"
                    "layout(location = 2) out vec4 o_c2;\nlayout(location = 3) out vec4 o_c3;\n";
    for (int k = 0; k < 16; ++k) {
        if (!(fi.texUsed & (1u << k))) continue;
        const uint32_t dim = (texDims >> (2 * k)) & 3;
        std::snprintf(t, sizeof t, "layout(binding = %d) uniform %s tex%d;\n", k, dim == 1 ? "samplerCube" : dim == 2 ? "sampler3D" : "sampler2D", k);
        h += t;
    }
    h += "bool fcond1(float c, int lt, int eq, int gt) { return (lt != 0 && c < 0.0) || (eq != 0 && c == 0.0) || (gt != 0 && c > 0.0); }\n"
         "bool fcond(vec4 c, int lt, int eq, int gt) { return fcond1(c.x, lt, eq, gt) || fcond1(c.y, lt, eq, gt) || fcond1(c.z, lt, eq, gt) || fcond1(c.w, lt, eq, gt); }\n"
         // depth compare: stored depth OP reference (for GREATER the result is 1 where the surface is NOT behind an
         // occluder; the game shades with it as "lit"). Checked against the car selection screen, where the cars'
         // shadows are dark only this way. SHBIAS (default 0.002) moves the reference towards the light: no acne.
         "float shcmp(sampler2D t, vec3 p, float f) {\n"
         "  vec4 d = textureGather(t, p.xy, 0); vec4 r = vec4(p.z - SHBIAS); vec4 c;\n"
         "  if (f < 1.5) c = vec4(lessThan(d, r)); else if (f < 2.5) c = vec4(equal(d, r)); else if (f < 3.5) c = vec4(lessThanEqual(d, r));\n"
         "  else if (f < 4.5) c = vec4(greaterThan(d, r)); else if (f < 5.5) c = vec4(notEqual(d, r)); else c = vec4(greaterThanEqual(d, r));\n"
         // bilinear percentage-closer filtering like the hardware: the four results weighted by the position inside
         // the texel (an even average made the shadow edges blocky). textureGather order: (0,1) (1,1) (1,0) (0,0)
         "  vec2 fr = fract(p.xy * vec2(textureSize(t, 0)) - 0.5);\n"
         "  float s = mix(mix(c.w, c.z, fr.x), mix(c.x, c.y, fr.x), fr.y);\n"
         "#ifdef SHDBG\n  g_shS = s; g_shRD = vec2(p.z, d.w);\n#endif\n"
         "  return s;\n}\n"
         "vec4 flit(vec4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? exp2(s.w * log2(max(s.y, 1e-30))) : 0.0; return vec4(1.0, d, sp, 1.0); }\n"
         "vec4 frefl(vec4 n, vec4 e) { return vec4(2.0 * dot(n.xyz, e.xyz) / dot(n.xyz, n.xyz) * n.xyz - e.xyz, 0.0); }\n"
         "vec4 col0() { return gl_FrontFacing ? v_col0 : v_col0; }\n"
         "vec4 col1() { return v_col1; }\n"
         "void main() {\n"
         "  vec4 R[64]; vec4 H[64]; for (int i = 0; i < 64; ++i) { R[i] = vec4(0.0); H[i] = vec4(0.0); }\n"
         "  vec4 t = vec4(0.0), CC0 = vec4(0.0), CC1 = vec4(0.0);\n"
         "  vec4 wpos = vec4(gl_FragCoord.x * u_wpos.x, gl_FragCoord.y * u_wpos.y, gl_FragCoord.z, 1.0 / gl_FragCoord.w);\n"
         "  vec4 ssa = vec4(gl_FrontFacing ? 1.0 : -1.0);\n";
    const bool out32 = (control & 0x40) != 0;
    const bool useR = fi.writesR0 && (!fi.writesH0 || out32);
    std::string tail;
    if (useR) tail = "  o_c0 = R[0]; o_c1 = R[2]; o_c2 = R[3]; o_c3 = R[4];\n";
    else tail = "  o_c0 = H[0]; o_c1 = H[4]; o_c2 = H[6]; o_c3 = H[8];\n";
    if (control & 0xe) tail += "  gl_FragDepth = R[1].z;\n";
    tail += "  if (u_alpha.x > 0.5) {\n"
            "    int f = int(u_alpha.y); float a = o_c0.a, r = u_alpha.z; bool pass = true;\n"
            "    if (f == 0) pass = false; else if (f == 1) pass = a < r; else if (f == 2) pass = a == r; else if (f == 3) pass = a <= r;\n"
            "    else if (f == 4) pass = a > r; else if (f == 5) pass = a != r; else if (f == 6) pass = a >= r;\n"
            "    if (!pass) discard;\n  }\n";
    if (std::getenv("RSXRED")) tail += "  o_c0 = v_col0;\n";
    if (shDbg) tail += std::getenv("SHDBG")[0] == '2' ? "  if (g_shS >= 0.0) o_c0 = vec4(g_shRD, g_shS, 1.0);\n" : "  if (g_shS >= 0.0) o_c0 = vec4(g_shS, 1.0 - g_shS, 0.0, 1.0);\n";   // red = shadow 1, green = 0
    fi.body = b;
    fi.glsl = h + b + tail + "}\n";
    return fi;
}


uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {   // Morton order, x in the low bit
    uint32_t r = 0, bit = 0;
    for (uint32_t k = 0; k < 16 && (k < lw || k < lh); ++k) {
        if (k < lw) r |= ((x >> k) & 1) << bit++;
        if (k < lh) r |= ((y >> k) & 1) << bit++;
    }
    return r;
}
uint32_t ilog2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) ++r; return r; }

// uploads (or finds) texture unit i; returns dimension code for the shader key and the coordinate scale
uint32_t texDimsOf(int i) {
    const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
    const uint32_t fmtReg = R[(base + 4) >> 2];
    const uint32_t dim = (fmtReg >> 4) & 0xf;
    if (fmtReg & 4) return 1;
    if (dim == 3) return 2;
    const uint32_t f = (fmtReg >> 8) & 0x9f;   // DEPTH24_D8 / DEPTH24_D8_FLOAT / DEPTH16 / DEPTH16_FLOAT
    if (f >= 0x90 && f <= 0x93 && ((R[(base + 8) >> 2] >> 28) & 7) != 7 && ((R[(base + 8) >> 2] >> 28) & 7) != 0) return 3;   // shadow map: depth compare
    return 0;
}

// ------------------------------------------------------------------ vertex data and draws

// byte-swaps n big-endian 32-bit words (the float vertex arrays: Edge writes ~20 million positions a second)
__attribute__((target("avx2"))) void bswap32_block(uint32_t* dst, const uint8_t* src, size_t n) {
    const __m256i sh = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 4 * i)), sh));
    for (; i < n; ++i) { uint32_t w; std::memcpy(&w, src + 4 * i, 4); dst[i] = __builtin_bswap32(w); }
}
// 16-bit big-endian indices + base -> 32-bit (every index of every draw goes through here)
__attribute__((target("avx2"))) void gather_i16(uint32_t* d, const uint8_t* q, uint32_t n, uint32_t add) {
    const __m128i sh = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    const __m256i va = _mm256_set1_epi32(int(add));
    uint32_t k = 0;
    for (; k + 8 <= n; k += 8)
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + k), _mm256_add_epi32(_mm256_cvtepu16_epi32(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 2 * k)), sh)), va));
    for (; k < n; ++k) { uint16_t w; std::memcpy(&w, q + 2 * size_t(k), 2); d[k] = uint32_t(__builtin_bswap16(w)) + add; }
}
// lowest index and highest index + 1 (the restart value 0xffffffff wraps to 0 and drops out of the maximum)
__attribute__((target("avx2"))) void index_range(const uint32_t* v, size_t n, uint32_t& lo, uint32_t& hi1) {
    __m256i mn = _mm256_set1_epi32(-1), mx = _mm256_setzero_si256();
    const __m256i one = _mm256_set1_epi32(1);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
        mn = _mm256_min_epu32(mn, x); mx = _mm256_max_epu32(mx, _mm256_add_epi32(x, one));
    }
    alignas(32) uint32_t a[8], b[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(a), mn); _mm256_store_si256(reinterpret_cast<__m256i*>(b), mx);
    lo = 0xffffffffu; hi1 = 0;
    for (int k = 0; k < 8; ++k) { lo = std::min(lo, a[k]); hi1 = std::max(hi1, b[k]); }
    for (; i < n; ++i) { lo = std::min(lo, v[i]); hi1 = std::max(hi1, v[i] + 1); }
}
// Number of floats per vertex convert_attr writes for attribute a: float arrays keep their own size (byte swap only,
// the missing components come from GL's defaults 0, 0, 1), everything else is expanded to 4.
uint32_t attr_comps(int a) {
    const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
    const uint32_t size = std::min<uint32_t>((fmt >> 4) & 0xf, 4);
    static const bool off = std::getenv("RSXCOMPS4") != nullptr;   // RSXCOMPS4=1: old path (everything as 4 floats), for A/B checks
    return !off && (fmt & 0xf) == 2 && ((fmt >> 8) & 0xff) && size ? size : 4;
}
void convert_attr(int a, uint32_t first, uint32_t count, float* out, uint32_t comps) {
    const uint32_t fmt = R[(0x1740 + 4 * a) >> 2], offReg = R[(0x1680 + 4 * a) >> 2];
    const uint32_t type = fmt & 0xf, size = std::min<uint32_t>((fmt >> 4) & 0xf, 4), stride = (fmt >> 8) & 0xff;
    const uint32_t base = ea_of(offReg & 0x7fffffff, offReg >> 31) + R[0x1738 >> 2];
    const uint8_t* p = C.mem + base + size_t(stride) * first;
    if (type == 2 && comps == size && stride) {   // packed floats: byte swap only
        uint32_t* o = reinterpret_cast<uint32_t*>(out);
        if (stride == 4 * size) bswap32_block(o, p, size_t(count) * size);
        else for (uint32_t v = 0; v < count; ++v, p += stride, o += size) bswap32_block(o, p, size);
        return;
    }
    // one tight loop per type (this runs for every vertex of every draw); missing components are 0, 0, 0, 1
    const auto r16 = [](const uint8_t* q) { uint16_t v; std::memcpy(&v, q, 2); return uint16_t(__builtin_bswap16(v)); };
    const auto r32 = [](const uint8_t* q) { uint32_t v; std::memcpy(&v, q, 4); return __builtin_bswap32(v); };
#define RSX_CONV(EXPR)     for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {         out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;         for (uint32_t k = 0; k < size; ++k) out[k] = (EXPR);     }
    switch (type) {
        case 1: RSX_CONV(std::max(-1.f, int16_t(r16(p + 2 * k)) / 32767.f)) break;
        case 2:
            if (size == 3) for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) { const uint32_t x[4] = {r32(p), r32(p + 4), r32(p + 8), 0x3f800000u}; std::memcpy(out, x, 16); }
            else if (size == 4) for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) { const uint32_t x[4] = {r32(p), r32(p + 4), r32(p + 8), r32(p + 12)}; std::memcpy(out, x, 16); }
            else if (size == 2) for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) { const uint32_t x[4] = {r32(p), r32(p + 4), 0u, 0x3f800000u}; std::memcpy(out, x, 16); }
            else RSX_CONV(f32(r32(p + 4 * k)))
            break;
        case 3: RSX_CONV(half2f(r16(p + 2 * k))) break;
        case 4: RSX_CONV(p[k] * (1.f / 255.f)) break;
        case 5: RSX_CONV(float(int16_t(r16(p + 2 * k)))) break;
        case 6:
            for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {
                const uint32_t w = r32(p);
                out[0] = float(int32_t(w << 21) >> 21) / 1023.f;
                out[1] = float(int32_t(w << 10) >> 21) / 1023.f;
                out[2] = float(int32_t(w) >> 22) / 511.f;
                out[3] = 1;
            }
            break;
        case 7: RSX_CONV(float(p[k])) break;
        default: for (uint32_t v = 0; v < count; ++v, out += 4) { out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1; } break;
    }
#undef RSX_CONV
}


// The indices of the current draw (all its ranges) into idx, 0xffffffff = primitive restart, and the lowest and
// highest vertex they use (vmin > vmax: nothing to draw). Quads become triangles. With triFans (APIs without fans
// and line loops: Direct3D) fans and polygons become triangle lists and line loops closed line strips.
void gather_draw_indices(std::vector<uint32_t>& idx, uint32_t& vmin, uint32_t& vmax, bool triFans) {
    vmin = 0xffffffffu; vmax = 0;
    idx.clear();
    const uint32_t ia = R[0x1820 >> 2];
    const bool i16 = (ia >> 4) & 1;
    const uint32_t ibase = ea_of(R[0x181c >> 2], ia & 1);
    const bool restart = R[0x1dac >> 2] & 1;
    const uint32_t restartIdx = R[0x1db0 >> 2];
    const bool fan = triFans && (prim == 7 || prim == 10), loop = triFans && prim == 3;
    const bool joinStrips = prim == 3 || prim == 4 || prim == 6 || prim == 9 || (!triFans && (prim == 7 || prim == 10));
    const uint32_t add = rangesIndexed ? R[0x173c >> 2] : 0;
    size_t total = 0;
    for (const Range& r : ranges) total += size_t(r.count) + 2;
    idx.resize(prim == 8 ? total * 3 / 2 + 8 : fan ? total * 3 + 8 : total);
    static std::vector<uint32_t> seq;
    uint32_t* o = idx.data();
    for (const Range& r : ranges) {
        const bool viaSeq = prim == 8 || fan || loop;
        if (viaSeq) seq.resize(r.count);
        uint32_t* d = viaSeq ? seq.data() : o;
        if (!rangesIndexed) for (uint32_t k = 0; k < r.count; ++k) d[k] = r.first + k;
        else if (i16) gather_i16(d, C.mem + ibase + 2 * size_t(r.first), r.count, add);
        else {
            const uint8_t* q = C.mem + ibase + 4 * size_t(r.first);
            for (uint32_t k = 0; k < r.count; ++k) { uint32_t w; std::memcpy(&w, q + 4 * size_t(k), 4); d[k] = __builtin_bswap32(w) + add; }
        }
        if (restart && rangesIndexed) for (uint32_t k = 0; k < r.count; ++k) d[k] = d[k] == restartIdx ? 0xffffffffu : d[k];
        if (prim == 8) {   // quads -> triangles
            for (size_t q = 0; q + 3 < seq.size(); q += 4) { o[0] = seq[q]; o[1] = seq[q + 1]; o[2] = seq[q + 2]; o[3] = seq[q]; o[4] = seq[q + 2]; o[5] = seq[q + 3]; o += 6; }
        } else if (fan) {  // each restart-separated fan: (first, k, k + 1)
            size_t s0 = 0;
            while (s0 < seq.size()) {
                size_t e = s0;
                while (e < seq.size() && seq[e] != 0xffffffffu) ++e;
                for (size_t k = s0 + 1; k + 1 < e; ++k) { o[0] = seq[s0]; o[1] = seq[k]; o[2] = seq[k + 1]; o += 3; }
                s0 = e + 1;
            }
        } else if (loop) { // line loop -> strip back to its first vertex
            for (uint32_t v : seq) *o++ = v;
            if (!seq.empty()) *o++ = seq[0];
            if (&r != &ranges.back()) *o++ = 0xffffffffu;
        } else {
            o += r.count;
            if (&r != &ranges.back() && joinStrips) *o++ = 0xffffffffu;
        }
    }
    idx.resize(size_t(o - idx.data()));
    uint32_t lo = 0xffffffffu, hi1 = 0;   // hi1 = highest index + 1 (the restart value wraps to 0 and drops out)
    index_range(idx.data(), idx.size(), lo, hi1);
    if (hi1 == 0) { vmin = 1; vmax = 0; return; }
    vmin = lo; vmax = hi1 - 1;
}

// ------------------------------------------------------------------ window
// The back end: OpenGL (0) or Direct3D 11 (1), menu option "Renderizador" (read at start).
static void init_window() {
    initTried = true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { RLOG("[rsx] SDL_Init failed: %s\n", SDL_GetError()); return; }
    const char* env = std::getenv("RENDERER");
    int api = settings().renderer;
    if (env) api = (std::strcmp(env, "d3d11") == 0 || std::strcmp(env, "dx11") == 0) ? 1 : (std::strcmp(env, "vulkan") == 0 || std::strcmp(env, "vk") == 0) ? 2 : 0;
    B = api == 1 ? make_d3d11_backend() : api == 2 ? make_vulkan_backend() : make_gl_backend();
    for (int attempt = 0; attempt < 2; ++attempt) {
        win = SDL_CreateWindow("Racing Club+ (F1: opciones)", 1280, 720, B->window_flags() | SDL_WINDOW_RESIZABLE);
        if (!win) { RLOG("[rsx] window: %s\n", SDL_GetError()); return; }
        if (B->init()) break;
        if (B == make_gl_backend()) return;
        RLOG("[rsx] %s not available: back to OpenGL\n", B->api_name());   // the window is made again (OpenGL needs its flag)
        SDL_DestroyWindow(win); win = nullptr;
        B = make_gl_backend();
    }
    if (!win) return;
    if (settings().fullscreen) set_display_mode(settings().fullscreen);
    ready = true;
}

// Display mode: 0 window, 1 borderless window covering the monitor, 2 full screen.
// Full screen (SDL's desktop full screen) lets the NVIDIA OpenGL driver show the image directly ("independent flip"):
// lowest latency, but the desktop compositor never sees it, so screen capture (OBS) records black. The borderless
// mode covers the monitor plus one row, which keeps the window composed: it looks the same and can be captured.
static void set_display_mode(int mode) {
    static int sx = 0, sy = 0, sw = 1280, sh = 720;
    static int cur = 0;
    if (cur == 0 && mode != 0) { SDL_GetWindowPosition(win, &sx, &sy); SDL_GetWindowSize(win, &sw, &sh); }
    if (cur == 2) SDL_SetWindowFullscreen(win, false);
    if (cur != 0) { SDL_SetWindowBordered(win, true); SDL_SetWindowSize(win, sw, sh); SDL_SetWindowPosition(win, sx, sy); }
    if (mode == 1) {
        SDL_Rect r{};
        if (SDL_GetDisplayBounds(SDL_GetDisplayForWindow(win), &r)) {
            SDL_SetWindowBordered(win, false);
            SDL_SetWindowPosition(win, r.x, r.y);
            SDL_SetWindowSize(win, r.w, r.h + 1);
        }
    } else if (mode == 2) {
        SDL_SetWindowFullscreen(win, true);
    }
    if (mode != 0) SDL_RaiseWindow(win);
    if (mode != 0 && std::getenv("FSTOP")) SDL_SetWindowAlwaysOnTop(win, true);   // debug: tests started from a background process
    cur = mode;
}
static SDL_Gamepad* pad = nullptr;
static std::atomic<int> g_padKind{0};   // pad_kind() for other threads (file reads)
static void pad_opened() {
    int k = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: k = 1; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: k = 3; break;
        default: k = 2;
    }
    g_padKind = k;
    RLOG("[pad] %s%s\n", pad ? "controller: " : "no controller", pad ? SDL_GetGamepadName(pad) : "");
}
int pad_kind() { return g_padKind.load(); }
static std::atomic<uint32_t> g_texEpoch{0};
void textures_rewritten() { ++g_texEpoch; }
uint32_t texture_epoch() { return g_texEpoch.load(std::memory_order_relaxed); }
// ------------------------------------------------------------------ options menu (F1)
// Four tabs; up/down walks every option in order (and so across the tabs), Q/E or L1/R1 jump to the previous/next
// tab. The ids are those of menu_change.
static bool menuOpen = std::getenv("MENUTEST") != nullptr, menuDirty = true;   // MENUTEST=1 (debug): open from the start
enum MenuId { mScreen = 1, mScale, mHz, mRefresh, mLang, mFps, mVsync, mSound, mAniso, mStream, mFxaa, mSharpen, mCamera, mApi, mApply, mReport, mQuit, mButtons,
              mOnline, mOnName, mOnIp, mOnMine, mPatreon };
static const int kMenuOrder[] = {mScreen, mScale, mHz, mRefresh, mVsync,   mApi, mFxaa, mSharpen, mAniso,   mLang, mSound, mButtons, mCamera, mStream, mFps,
                                 mOnline, mOnName, mOnIp, mOnMine,   mApply, mReport, mPatreon, mQuit};
static const int kMenuTabStart[] = {0, 5, 9, 15, 19, 23};
static const char* const kPatreonUrl = "https://www.patreon.com/ilesraider/membership";   // F1 > Sistema and the main menu banner
static const wchar_t* const kMenuTabs[] = {L"Pantalla", L"Gráficos", L"Juego", L"Online", L"Sistema"};
static const int kMenuCount = int(sizeof kMenuOrder / sizeof kMenuOrder[0]), kMenuTabCount = 5;
static int menuSel = std::getenv("MENUTEST") ? std::clamp(std::atoi(std::getenv("MENUTEST")) - 1, 0, int(sizeof kMenuOrder / sizeof kMenuOrder[0]) - 1) : 0;   // index in kMenuOrder (MENUTEST=n: the n-th)
static int menu_tab_of(int idx) { int t = 0; while (t + 1 < kMenuTabCount && idx >= kMenuTabStart[t + 1]) ++t; return t; }
static void menu_move(int d) { menuSel = (menuSel + d + kMenuCount) % kMenuCount; menuDirty = true; }
static void menu_tab(int d) { menuSel = kMenuTabStart[(menu_tab_of(menuSel) + d + kMenuTabCount) % kMenuTabCount]; menuDirty = true; }
static std::vector<std::string> g_menuCams;   // the host cameras, listed when the menu is drawn first
// the settings as the game started (the options read only at start show "waiting for the restart" when they differ)
static const Settings& boot_settings() { static const Settings b = settings(); return b; }
static void menu_change(int dir) {
    (void)boot_settings();
    Settings& s = settings();
    switch (kMenuOrder[menuSel]) {
        case mScreen: s.fullscreen = (s.fullscreen + (dir ? dir : 1) + 3) % 3; if (s.fullscreen) g_lastFull = s.fullscreen; set_display_mode(s.fullscreen); break;
        case mScale: s.scale = dir ? std::clamp(s.scale + dir, 1, 3) : s.scale % 3 + 1; break;
        case mHz: { static const int r[4] = {60, 72, 120, 144}; int i = 0; while (i < 3 && r[i] != s.gameHz) ++i; s.gameHz = r[(i + (dir ? dir : 1) + 4) % 4]; break; }
        case mRefresh: s.refresh ^= 1; break;
        case mLang: s.language = 1 + (s.language - 1 + (dir ? dir : 1) + 5) % 5; break;
        case mFps: s.showFps ^= 1; if (!s.showFps) SDL_SetWindowTitle(win, "Racing Club+"); break;
        case mVsync: s.vsync ^= 1; B->set_vsync(s.vsync); break;
        case mSound: s.sound ^= 1; break;
        case mAniso: s.aniso = dir ? std::clamp(s.aniso + dir, 0, 4) : (s.aniso + 1) % 5; B->textures_changed(); break;   // the texture cache keeps sampler state: rebuild it
        case mStream: s.streaming ^= 1; break;
        case mFxaa: s.fxaa ^= 1; break;
        case mSharpen: s.sharpen = (s.sharpen + (dir ? dir : 1) + 4) % 4; break;
        case mCamera: { const int n = int(g_menuCams.size()) + 1; s.camera = (s.camera + (dir ? dir : 1) + n) % n; host_camera_stop(); break; }   // the game opens it again with the new one
        case mApi: s.renderer = (s.renderer + (dir ? dir : 1) + 3) % 3; break;
        case mButtons: s.buttons = (s.buttons + (dir ? dir : 1) + 4) % 4; break;   // the icons change at the next image (glyphs_update)
        case mOnline: s.online ^= 1; break;
        case mOnName: if (dir >= 0) {
                std::string v = s.onlineName;
                if (host_input_text(L"Racing Club+ · Online", L"Tu nombre en las partidas online (letras y números, hasta 16):", v, 16)) {
                    std::string o; for (unsigned char ch : v) if (o.size() < 16 && (std::isalnum(ch) || ch == '-' || ch == '_')) o += char(ch);
                    std::snprintf(s.onlineName, sizeof s.onlineName, "%s", o.c_str()); settings_save();
                }
            } break;
        case mOnIp: if (dir >= 0) {
                std::string v = s.onlineIp;
                if (host_input_text(L"Racing Club+ · Online", L"IP (o nombre) del amigo que crea la partida. Déjalo vacío para buscar solo en tu red local:", v, 60)) {
                    std::string o; for (char ch : v) if (ch != ' ') o += ch;
                    std::snprintf(s.onlineIp, sizeof s.onlineIp, "%s", o.c_str()); settings_save();
                }
            } break;
        case mOnMine: break;
        case mApply: if (dir >= 0) { settings_save(); host_restart(); } break;
        case mReport: if (dir >= 0) { settings_save(); const std::string g = std::string(B->api_name()) + " / " + B->gpu_name(); host_report(g.c_str()); } break;
        case mPatreon: if (dir >= 0) { RLOG("[patreon] opened from F1\n"); SDL_OpenURL(kPatreonUrl); } break;
        case mQuit: if (dir >= 0) { settings_save(); SDL_Event q{}; q.type = SDL_EVENT_QUIT; SDL_PushEvent(&q); } break;   // same as closing the window
    }
    menuDirty = true;
}

// the menu bitmap, drawn at the size it is shown: 16:9, 80 % of the window height, in its middle
static void menu_overlay(int ww, int wh, Overlay& o) {
    int dh = wh * 80 / 100, dw = dh * 16 / 9;
    if (dw > ww * 94 / 100) { dw = ww * 94 / 100; dh = dw * 9 / 16; }
    dw = std::max(dw, 64); dh = std::max(dh, 64);
    static std::vector<uint32_t> px;
    static int haveW = 0, haveH = 0;
    o.changed = false;
    if (menuDirty || haveW != dw || haveH != dh) {
        menuDirty = false;
        const Settings& s = settings();
        const Settings& b0 = boot_settings();
        static const wchar_t* const res[3] = {L"1280 × 720 (original)", L"2560 × 1440", L"3840 × 2160"};
        static const wchar_t* const lang[5] = {L"English", L"Français", L"Español", L"Deutsch", L"Italiano"};
        static const wchar_t* const an[5] = {L"Solo el del juego", L"2x", L"4x", L"8x", L"16x"};
        static const wchar_t* const sh[4] = {L"No", L"Baja", L"Media", L"Alta"};
        static const wchar_t* const api[3] = {L"OpenGL", L"DirectX 11", L"Vulkan"};
        if (g_menuCams.empty()) g_menuCams = host_camera_list();
        static std::wstring cam, note, status;
        cam = L"Ninguna";
        if (s.camera > 0 && s.camera <= int(g_menuCams.size())) { cam.clear(); for (unsigned char ch : g_menuCams[size_t(s.camera - 1)]) cam += wchar_t(ch); }
        if (cam.size() > 34) cam = cam.substr(0, 33) + L"…";
        note.clear();
        {   // the API running now, when the option says another (it applies at the next start)
            const char* now = B ? B->api_name() : "";
            char opt[16]; std::snprintf(opt, sizeof opt, "%ls", api[std::clamp(s.renderer, 0, 2)]);
            if (B && std::strcmp(now, opt) != 0) { note = L"en uso: "; for (const char* q = now; *q; ++q) note += wchar_t(*q); }
            status.clear();
            if (B) { for (const char* q = B->api_name(); *q; ++q) status += wchar_t(*q); status += L"  ·  "; for (const char* q = B->gpu_name(); *q && *q != '/'; ++q) status += wchar_t((unsigned char)*q); }
        }
        using K = MenuRow::Kind;
        auto choice = [](const wchar_t* l, const wchar_t* v, bool restart = false, bool pending = false, const wchar_t* n = L"") { MenuRow r; r.kind = K::Choice; r.label = l; r.value = v; r.restart = restart; r.pending = pending; r.note = n; return r; };
        auto toggle = [](const wchar_t* l, bool on, bool restart = false, bool pending = false) { MenuRow r; r.kind = K::Toggle; r.label = l; r.on = on; r.restart = restart; r.pending = pending; return r; };
        auto action = [](const wchar_t* l, const wchar_t* v) { MenuRow r; r.kind = K::Action; r.label = l; r.value = v; return r; };
        MenuRow rows[kMenuCount + 1];
        const wchar_t* help[kMenuCount + 1] = {};
        for (int i = 0; i < kMenuCount; ++i) {
            MenuRow& r = rows[i]; const wchar_t*& hp = help[i];
            switch (kMenuOrder[i]) {
                case mScreen: r = choice(L"Modo de pantalla", s.fullscreen == 0 ? L"Ventana" : s.fullscreen == 1 ? L"Ventana sin bordes" : L"Pantalla completa");
                    hp = L"Ventana sin bordes ocupa todo el monitor y se puede capturar con OBS. Pantalla completa es el modo exclusivo. F11 o Alt+Intro alternan con la ventana."; break;
                case mScale: r = choice(L"Resolución interna", res[std::clamp(s.scale, 1, 3) - 1]);
                    hp = L"Resolución a la que se dibuja el juego antes de ajustarlo a la ventana. Más alta se ve más nítida y pide más a la tarjeta gráfica."; break;
                case mHz: r = choice(L"Imágenes por segundo", netplay::in_room() ? L"60 (partida online)" : s.gameHz == 60 ? L"60 (original)" : s.gameHz == 72 ? L"72 (ideal en 144 Hz)" : s.gameHz == 120 ? L"120 (experimental)" : L"144 (experimental)");
                    hp = L"Velocidad a la que corre el juego. 60 es la de la consola; 72 encaja con monitores de 144 Hz. 120 y 144 son experimentales. En una partida online todos juegan a 60."; break;
                case mRefresh: r = toggle(L"Adaptar la pantalla al juego", s.refresh);
                    hp = L"Cambia la frecuencia del monitor mientras juegas (por ejemplo de 144 a 120 Hz) para que cada imagen dure lo mismo y el movimiento sea suave."; break;
                case mVsync: r = toggle(L"V-Sync", s.vsync);
                    hp = L"Sincroniza cada imagen con el refresco del monitor: sin cortes horizontales (tearing), a cambio de un poco más de retardo."; break;
                case mApi: r = choice(L"API gráfica", api[std::clamp(s.renderer, 0, 2)], true, s.renderer != b0.renderer, note.c_str());
                    hp = L"Con qué se dibuja el juego. Las tres se ven igual; si una da problemas con tu tarjeta gráfica, prueba otra."; break;
                case mFxaa: r = toggle(L"Antialiasing (FXAA)", s.fxaa);
                    hp = L"Suaviza los dientes de sierra de los bordes en la imagen final."; break;
                case mSharpen: r = choice(L"Nitidez", sh[std::clamp(s.sharpen, 0, 3)]);
                    hp = L"Realza los detalles de la imagen final. Va bien junto al antialiasing, que la suaviza un poco."; break;
                case mAniso: r = choice(L"Filtrado anisótropo", an[std::clamp(s.aniso, 0, 4)]);
                    hp = L"Mantiene nítidas las texturas del suelo y la carretera vistas de lejos o en ángulo. Apenas cuesta rendimiento."; break;
                case mLang: r = choice(L"Idioma del juego", lang[std::clamp(s.language, 1, 5) - 1], true, s.language != b0.language);
                    hp = L"Idioma de los textos y menús del juego."; break;
                case mSound: r = toggle(L"Sonido", s.sound);
                    hp = L"Música y efectos de sonido del juego."; break;
                case mButtons: {
                    static const wchar_t* const bn[4] = {L"Según el mando", L"PlayStation", L"Xbox", L"Nintendo"};
                    static const wchar_t* const kn[4] = {L"sin mando: PlayStation", L"detectado: PlayStation", L"detectado: Xbox", L"detectado: Nintendo"};
                    r = choice(L"Iconos de botones", bn[std::clamp(s.buttons, 0, 3)], false, false, s.buttons == 0 ? kn[std::clamp(pad_kind(), 0, 3)] : L"");
                    hp = L"Los botones que muestran los menús del juego: los del mando conectado (Xbox, Nintendo o PlayStation) o los que elijas."; break; }
                case mCamera: r = choice(L"Cámara (EyeToy)", cam.c_str());
                    hp = L"La cámara que el juego usa como EyeToy, para la foto del piloto."; break;
                case mStream: r = toggle(L"Modo directo (OBS / Twitch)", s.streaming);
                    hp = L"Para grabar o emitir: el juego va a 60 imágenes por segundo y el monitor a 120 Hz, sea cual sea la opción de imágenes por segundo."; break;
                case mFps: r = toggle(L"Contador de FPS en el título", s.showFps);
                    hp = L"Muestra las imágenes por segundo en el título de la ventana."; break;
                case mOnline: r = toggle(L"Juego online", s.online, true, s.online != b0.online);
                    hp = L"Partidas online por conexión directa (sin servidores): Multijugador > Crear partida, o Unirse a la partida con la IP del amigo que la crea."; break;
                case mOnName: {
                    static std::wstring nm; nm.clear();
                    const std::string n = s.onlineName[0] ? std::string(s.onlineName) : netplay::player_name();
                    for (unsigned char ch : n) nm += wchar_t(ch);
                    r = action(L"Tu nombre", nm.c_str()); r.restart = true; r.pending = std::strcmp(s.onlineName, b0.onlineName) != 0;
                    hp = L"El nombre que verán los demás jugadores (letras y números, hasta 16). Se aplica al reiniciar el juego."; break; }
                case mOnIp: {
                    static std::wstring ip; ip.clear();
                    for (unsigned char ch : std::string(s.onlineIp)) ip += wchar_t(ch);
                    r = action(L"Unirse a (IP del anfitrión)", s.onlineIp[0] ? ip.c_str() : L"Solo red local");
                    hp = L"Para unirte por internet: la IP pública de quien crea la partida. En la misma red local no hace falta: las partidas se encuentran solas."; break; }
                case mOnMine: {
                    static std::wstring me; me.clear();
                    for (unsigned char ch : netplay::local_address()) me += wchar_t(ch);
                    me += L"  ·  puerto 3658";
                    r = action(L"Tu IP en la red local", me.c_str());
                    { const int u = netplay::upnp_state(); r.note = u == 2 ? L"router abierto (UPnP)" : u == 3 ? L"abre el puerto a mano" : u == 1 ? L"abriendo el router…" : L""; }
                    hp = L"Si creas la partida para jugar por internet, abre el puerto 3658 (TCP y UDP) en tu router hacia esta IP y da a tus amigos tu IP pública."; break; }
                case mApply: r = action(L"Aplicar cambios", L"Reiniciar el juego");
                    hp = L"Guarda las opciones y reinicia el juego, para aplicar las marcadas con REINICIO."; break;
                case mReport: r = action(L"Informe de problemas", L"Crear y abrir GitHub");
                    hp = L"Crea informe_problema.txt (equipo, opciones y registro, sin tu nombre de usuario) y abre la página para adjuntarlo."; break;
                case mPatreon: r = action(L"Apoyar en Patreon", L"Abrir Patreon");
                    hp = L"Abre la página de Racing Club+ en Patreon en tu navegador. Tu apoyo se destina al mantenimiento del servidor online."; break;
                case mQuit: r = action(L"Salir del juego", L"Salir");
                    hp = L"Guarda las opciones y cierra el juego."; break;
            }
        }
        bool pending = false;
        for (int i = 0; i < kMenuCount; ++i) pending |= rows[i].pending;
        const int tab = menu_tab_of(menuSel);
        MenuPage pg;
        for (int t = 0; t < kMenuTabCount; ++t) pg.tabs[t] = kMenuTabs[t];
        pg.nTabs = kMenuTabCount; pg.tab = tab;
        pg.rows = rows + kMenuTabStart[tab]; pg.nRows = kMenuTabStart[tab + 1] - kMenuTabStart[tab]; pg.sel = menuSel - kMenuTabStart[tab];
        pg.help = help[menuSel] ? help[menuSel] : L"";
        pg.status = status.c_str();
        pg.pending = pending;
        pg.pad = pad != nullptr;
        px.resize(size_t(dw) * dh);
        if (!host_menu_bitmap(pg, dw, dh, px.data())) { o.px = nullptr; haveW = haveH = 0; return; }
        haveW = dw; haveH = dh; o.changed = true;
    }
    o.px = px.data(); o.w = dw; o.h = dh;
    o.dw = dw; o.dh = dh; o.dx = (ww - dw) / 2; o.dy = (wh - dh) / 2;
}

// ------------------------------------------------------------------ notifications (trophies)
struct Toast { std::wstring title, name, detail; uint32_t rgb; std::string png; };
static std::mutex g_toastMx;
static std::deque<Toast> g_toasts;
void toast(const std::wstring& title, const std::wstring& name, const std::wstring& detail, uint32_t rgb, const std::string& png) {
    std::lock_guard<std::mutex> lk(g_toastMx);
    g_toasts.push_back({title, name, detail, rgb, png});
}

// the first queued notification, top right, for 5 seconds (then the next one)
static void toast_overlay(int ww, int wh, Overlay& o) {
    static uint64_t shownAt = 0;
    static bool ready = false;
    static std::vector<uint32_t> px;
    const int W = 520, H = 120;
    o.px = nullptr; o.changed = false;
    Toast t;
    {
        std::lock_guard<std::mutex> lk(g_toastMx);
        if (g_toasts.empty()) return;
        if (ready && SDL_GetTicks() - shownAt > 5000) { g_toasts.pop_front(); ready = false; if (g_toasts.empty()) return; }
        t = g_toasts.front();
    }
    if (!ready) {
        px.resize(size_t(W) * H);
        if (!host_toast_bitmap(t.title.c_str(), t.name.c_str(), t.detail.c_str(), t.rgb, reinterpret_cast<const uint8_t*>(t.png.data()), t.png.size(), W, H, px.data())) { std::lock_guard<std::mutex> lk(g_toastMx); g_toasts.pop_front(); return; }
        shownAt = SDL_GetTicks(); ready = true; o.changed = true;
    }
    o.px = px.data(); o.w = W; o.h = H;
    o.dw = std::min(ww * 4 / 10, W * 2); o.dh = o.dw * H / W;
    const int m = std::max(8, wh / 40);
    o.dx = ww - m - o.dw; o.dy = m;
}

// ------------------------------------------------------------------ Patreon banner (main menu, top right)
// Shown while the last menu screen the game loaded is main_menu (its sub-menus live in the same file; the title,
// car select, options, races... load their own screen). A click opens the Patreon page in the browser.
static std::atomic<bool> g_onMainMenu{false};
void screen_loaded(const std::string& screen) { g_onMainMenu = screen == "main_menu"; }
static int g_bannerRect[4] = {};          // window pixels: x, y, w, h (w 0 = not shown)
static void banner_overlay(int ww, int wh, Overlay& o) {
    static std::vector<uint32_t> px;
    static int haveLang = -1;
    const int W = 560, H = 120;
    o.changed = false;
    const int lang = std::clamp(settings().language, 0, 5);
    if (haveLang != lang) {
        static const wchar_t* const l1[6] = {L"Support Racing Club+ on Patreon", L"Support Racing Club+ on Patreon", L"Soutenez Racing Club+ sur Patreon", L"Apoya Racing Club+ en Patreon", L"Unterst\u00fctze Racing Club+ auf Patreon", L"Sostieni Racing Club+ su Patreon"};
        static const wchar_t* const l2[6] = {L"For the upkeep of the online server", L"For the upkeep of the online server", L"Pour l'entretien du serveur en ligne", L"Para el mantenimiento del servidor online", L"F\u00fcr die Wartung des Online-Servers", L"Per la manutenzione del server online"};
        static const wchar_t* const l3[6] = {L"Click here", L"Click here", L"Cliquez ici", L"Haz clic aqu\u00ed", L"Hier klicken", L"Clicca qui"};
        px.resize(size_t(W) * H);
        if (!host_banner_bitmap(l1[lang], l2[lang], l3[lang], W, H, px.data())) { o.px = nullptr; return; }
        haveLang = lang; o.changed = true;
    }
    o.px = px.data(); o.w = W; o.h = H;
    o.dw = std::max(280, ww * 23 / 100); o.dh = o.dw * H / W;
    const int m = std::max(8, wh / 40);
    o.dx = ww - m - o.dw; o.dy = m;
}
static bool in_banner(float x, float y) {   // window coordinates of a mouse event
    if (!g_bannerRect[2]) return false;
    const float s = SDL_GetWindowPixelDensity(win);
    x *= s; y *= s;
    return x >= g_bannerRect[0] && y >= g_bannerRect[1] && x < g_bannerRect[0] + g_bannerRect[2] && y < g_bannerRect[1] + g_bannerRect[3];
}

// Several controllers at once: the real DualSense plus the virtual Xbox pad that DS4Windows, DSX or Steam create
// for it. The one with its own type (PlayStation, then Nintendo) is used rather than a generic XInput one, so the
// button icons follow the controller in the player's hands. gone = a controller just removed (still listed).
static void choose_pad(SDL_JoystickID gone) {
    auto score = [](SDL_GamepadType t) {
        switch (t) {
            case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 3;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 2;
            default: return 1;
        }
    };
    int n = 0; SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_JoystickID best = 0; int bestScore = 0;
    for (int k = 0; k < n; ++k) {
        if (ids[k] == gone) continue;
        const int sc = score(SDL_GetGamepadTypeForID(ids[k]));
        const char* nm = SDL_GetGamepadNameForID(ids[k]);
        RLOG("[pad] found: %s (type %d, %04x:%04x)\n", nm ? nm : "?", int(SDL_GetGamepadTypeForID(ids[k])), SDL_GetGamepadVendorForID(ids[k]), SDL_GetGamepadProductForID(ids[k]));
        if (sc > bestScore) { best = ids[k]; bestScore = sc; }
    }
    SDL_free(ids);
    if (pad && best == SDL_GetGamepadID(pad)) return;
    if (pad && !best && gone != SDL_GetGamepadID(pad)) return;
    if (pad) { SDL_CloseGamepad(pad); pad = nullptr; }
    if (best) pad = SDL_OpenGamepad(best);
    pad_opened(); menuDirty = true;
}
static bool padHold = false;   // the menu was closed with the controller: the game sees no buttons until they are released
static void pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) quitFlag = true;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) choose_pad(0);
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) choose_pad(e.gdevice.which);   // another one connected takes over
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {   // the menu with the controller: touchpad click or PS button opens and closes it
            const int b = e.gbutton.button;
            if (b == SDL_GAMEPAD_BUTTON_TOUCHPAD || b == SDL_GAMEPAD_BUTTON_GUIDE || (menuOpen && b == SDL_GAMEPAD_BUTTON_EAST)) { menuOpen = !menuOpen; menuDirty = true; if (!menuOpen) { settings_save(); padHold = true; } }
            else if (menuOpen && b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu_move(-1);
            else if (menuOpen && b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu_move(1);
            else if (menuOpen && b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) menu_tab(-1);
            else if (menuOpen && b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) menu_tab(1);
            else if (menuOpen && b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) menu_change(-1);
            else if (menuOpen && (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_SOUTH)) menu_change(1);
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION) {   // a hand over the banner
            static SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
            static bool over = false;
            const bool now = in_banner(e.motion.x, e.motion.y);
            if (now != over) { over = now; SDL_SetCursor(now ? hand : SDL_GetDefaultCursor()); }
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && in_banner(e.button.x, e.button.y)) {
            RLOG("[patreon] banner clicked\n");
            SDL_OpenURL(kPatreonUrl);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            const SDL_Scancode sc = e.key.scancode;
            if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) { Settings& st = settings(); if (st.fullscreen) { g_lastFull = st.fullscreen; st.fullscreen = 0; } else st.fullscreen = g_lastFull; set_display_mode(st.fullscreen); menuDirty = true; settings_save(); }
            else if (sc == SDL_SCANCODE_F1 || (menuOpen && sc == SDL_SCANCODE_ESCAPE)) { menuOpen = !menuOpen; menuDirty = true; if (!menuOpen) settings_save(); }
            else if (menuOpen && sc == SDL_SCANCODE_UP) menu_move(-1);
            else if (menuOpen && sc == SDL_SCANCODE_DOWN) menu_move(1);
            else if (menuOpen && (sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_PAGEUP || (sc == SDL_SCANCODE_TAB && (e.key.mod & SDL_KMOD_SHIFT)))) menu_tab(-1);
            else if (menuOpen && (sc == SDL_SCANCODE_E || sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_TAB)) menu_tab(1);
            else if (menuOpen && sc == SDL_SCANCODE_LEFT) menu_change(-1);
            else if (menuOpen && (sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE)) menu_change(1);
        }
    }
    {   // vibration the game asks for (cellPadSetActDirect), renewed while it lasts
        static uint32_t last = 0; static uint64_t renewed = 0;
        const uint32_t rb = menuOpen || std::getenv("NORUMBLE") ? 0 : lv2_rumble();
        const uint64_t now = SDL_GetTicks();
        if (pad && (rb != last || (rb && now - renewed > 150))) {
            const uint16_t low = uint16_t((rb & 0xff) * 257), high = (rb >> 8) ? 0xffff : 0;
            SDL_RumbleGamepad(pad, low, high, rb ? 300 : 0);
            last = rb; renewed = now;
        }
    }
    if (menuOpen) { lv2_set_pad(PadState{}); return; }   // the game sees no input while the menu is open
    if (padHold) {
        if (pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH))) { lv2_set_pad(PadState{}); return; }
        padHold = false;
    }
    // input -> DualShock 3 state
    PadState s;
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto btn = [&](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
    auto axis = [&](SDL_GamepadAxis a) { return pad ? SDL_GetGamepadAxis(pad, a) / 32768.f : 0.f; };
    if (k[SDL_SCANCODE_RETURN] || btn(SDL_GAMEPAD_BUTTON_START)) s.digital1 |= 0x08;
    if (k[SDL_SCANCODE_BACKSPACE] || btn(SDL_GAMEPAD_BUTTON_BACK)) s.digital1 |= 0x01;
    if (k[SDL_SCANCODE_UP] || btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) s.digital1 |= 0x10;
    if (k[SDL_SCANCODE_RIGHT] || btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.digital1 |= 0x20;
    if (k[SDL_SCANCODE_DOWN] || btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.digital1 |= 0x40;
    if (k[SDL_SCANCODE_LEFT] || btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.digital1 |= 0x80;
    const float lt = std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), k[SDL_SCANCODE_S] ? 1.f : 0.f);
    const float rt = std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), k[SDL_SCANCODE_W] ? 1.f : 0.f);
    if (lt > 0.1f) s.digital2 |= 0x01;
    if (rt > 0.1f) s.digital2 |= 0x02;
    if (k[SDL_SCANCODE_Q] || btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) s.digital2 |= 0x04;
    if (k[SDL_SCANCODE_E] || btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) s.digital2 |= 0x08;
    if (k[SDL_SCANCODE_V] || btn(SDL_GAMEPAD_BUTTON_NORTH)) s.digital2 |= 0x10;
    if (k[SDL_SCANCODE_ESCAPE] || btn(SDL_GAMEPAD_BUTTON_EAST)) s.digital2 |= 0x20;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE] || btn(SDL_GAMEPAD_BUTTON_SOUTH)) s.digital2 |= 0x40;
    if (k[SDL_SCANCODE_C] || btn(SDL_GAMEPAD_BUTTON_WEST)) s.digital2 |= 0x80;
    if (k[SDL_SCANCODE_L] || btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)) s.digital1 |= 0x02;    // L3: headlights
    if (k[SDL_SCANCODE_H] || btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) s.digital1 |= 0x04;   // R3: horn
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (k[SDL_SCANCODE_A]) lx = -1; if (k[SDL_SCANCODE_D]) lx = 1;
    auto a8 = [](float v) { return uint16_t(std::clamp(int(128 + v * 127.5f), 0, 255)); };
    s.analog[0] = a8(axis(SDL_GAMEPAD_AXIS_RIGHTX)); s.analog[1] = a8(axis(SDL_GAMEPAD_AXIS_RIGHTY));
    s.analog[2] = a8(lx); s.analog[3] = a8(ly);
    // pressure order: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
    auto pr = [](bool b) { return uint16_t(b ? 255 : 0); };
    s.pressure[0] = pr(s.digital1 & 0x20); s.pressure[1] = pr(s.digital1 & 0x80); s.pressure[2] = pr(s.digital1 & 0x10); s.pressure[3] = pr(s.digital1 & 0x40);
    s.pressure[4] = pr(s.digital2 & 0x10); s.pressure[5] = pr(s.digital2 & 0x20); s.pressure[6] = pr(s.digital2 & 0x40); s.pressure[7] = pr(s.digital2 & 0x80);
    s.pressure[8] = pr(s.digital2 & 0x04); s.pressure[9] = pr(s.digital2 & 0x08);
    s.pressure[10] = uint16_t(std::clamp(lt, 0.f, 1.f) * 255); s.pressure[11] = uint16_t(std::clamp(rt, 0.f, 1.f) * 255);
    static bool any = false;
    if (s.digital1 || s.digital2 || std::abs(lx) > 0.2f || std::abs(ly) > 0.2f) any = true;
    if (any) lv2_set_pad(s);   // the scripted test input stays active until the user touches something
}

void init(const Config& cfg) { C = cfg; }

void set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h) {
    if (id < 8) displays[id] = Display{offset, pitch, w, h};
    RLOG("[rsx] display buffer %u: offset 0x%x pitch %u %ux%u\n", id, offset, pitch, w, h);
}


void flip(uint32_t id) {
    if (!enabled()) return;
    if (!initTried) init_window();
    if (!ready) return;
    ++frameNo;
    B->new_frame();
    {   // FPSCAP=from,to,ms: sleep ms per frame between those seconds (debug: tells per-frame from per-second clocks)
        static double a = -1, b = -1, ms = 0;
        static bool parsed = false;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("FPSCAP")) std::sscanf(e, "%lf,%lf,%lf", &a, &b, &ms); }
        const double t = SDL_GetTicks() / 1000.0;
        if (ms > 0 && t >= a && t < b) SDL_Delay(Uint32(ms));
    }
    {   // frame pacing: longest gap between two flips, reported every 2 s (a hitch shows here, not in the average)
        static uint64_t lastFlip = 0, worst = 0, since = 0; static int over = 0;
        const uint64_t t = SDL_GetPerformanceCounter(), fq = SDL_GetPerformanceFrequency();
        if (lastFlip) { const uint64_t d = t - lastFlip; worst = std::max(worst, d); if (d * 1000 > fq * 25) ++over; }
        static const bool missLog = std::getenv("MISSLOG") != nullptr;
        static uint64_t swapAtLast = 0;
        if (missLog && lastFlip && (t - lastFlip) * 1000 > fq * 25) RLOG("[gap] %.1f ms between flips (swap %.1f ms since last flip)\n", (t - lastFlip) * 1000.0 / fq, (g_swapUs.load() - swapAtLast) / 1000.0);
        swapAtLast = g_swapUs.load();
        if (lastFlip && (t - lastFlip) * 1000 > fq * 60) RLOG("[hitch] %.0f ms without a flip, ending %.2f s after launch\n", (t - lastFlip) * 1000.0 / fq, SDL_GetTicks() / 1000.0);
        lastFlip = t;
        if (t - since > 2 * fq) {
            if (since) RLOG("[pace] worst frame %.0f ms, %d frames over 25 ms\n", worst * 1000.0 / fq, over);
            since = t; worst = 0; over = 0;
        }
    }
    int ww = 1280, wh = 720;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    glyphs_update();
    Overlay ovl[2];
    toast_overlay(ww, wh, ovl[0]);
    {   // slot 0: a notification first, else the Patreon banner on the main menu
        static int slot0 = 0;   // what slot 0 showed last: 0 nothing, 1 notification, 2 banner
        g_bannerRect[2] = 0;
        if (ovl[0].px) { if (slot0 != 1) ovl[0].changed = true; slot0 = 1; }
        else if (g_onMainMenu && !menuOpen) {
            banner_overlay(ww, wh, ovl[0]);
            if (ovl[0].px) {
                if (slot0 != 2) ovl[0].changed = true;
                slot0 = 2;
                g_bannerRect[0] = ovl[0].dx; g_bannerRect[1] = ovl[0].dy; g_bannerRect[2] = ovl[0].dw; g_bannerRect[3] = ovl[0].dh;
            }
        } else slot0 = 0;
    }
    if (menuOpen) menu_overlay(ww, wh, ovl[1]);
    B->present(displays[id & 7], ww, wh, ovl, 2);
    pump_events();
    static uint64_t lastT = SDL_GetTicks(), lastF = 0;
    const uint64_t now = SDL_GetTicks();
    if (now - lastT >= 2000 && !settings().showFps && !std::getenv("FPSLOG")) { lastT = now; lastF = frameNo; }
    if (now - lastT >= 2000) {
        static const bool fpsLog = std::getenv("FPSLOG") != nullptr;   // FPSLOG=1: frame rate every 2 s in the log (benchmarks)
        if (fpsLog) RLOG("[fps] %.1f s: %.1f\n", now / 1000.0, (frameNo - lastF) * 1000.0 / (now - lastT));
        char title[160];
        std::snprintf(title, sizeof title, "Racing Club+ - %.1f fps - %s (F1: opciones)", (frameNo - lastF) * 1000.0 / (now - lastT), B->api_name());
        SDL_SetWindowTitle(win, title);
        lastT = now; lastF = frameNo;
    }
    drawsThisFrame = 0;
}

Counters counters() {
    static const uint64_t fq = SDL_GetPerformanceFrequency();
    return Counters{g_progBuilds, g_texUploads, g_texUploadBytes, g_streamBuilds, g_swapUs.load(), g_drawCount, g_drawTicks * 1000000 / fq, g_progTicks * 1000000 / fq, g_texTicks * 1000000 / fq, g_convTicks * 1000000 / fq, g_bufTicks * 1000000 / fq, g_streamVerts};
}

void method_block(uint32_t m0, const uint8_t* args, uint32_t count, bool noInc) {
    // transform constants (several matrices per draw): straight into vpConst
    if (!noInc && m0 >= 0x1f00 && m0 + 4 * count <= 0x1f80 && ready && enabled()) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t w; std::memcpy(&w, args + 4 * i, 4); w = __builtin_bswap32(w);
            const uint32_t k = cLoad * 4 + vpConstWord;
            if (k < 468 * 4) { std::memcpy(&vpConst[k], &w, 4); vpDirtyLo = std::min(vpDirtyLo, k); vpDirtyHi = std::max(vpDirtyHi, k + 1); vpConstDirty = true; }
            if (++vpConstWord == 4) { vpConstWord = 0; ++cLoad; }
        }
        return;
    }
    // plain register writes (most of a draw's state) go straight to R; only the methods with side effects take the
    // full path. This loop runs for every word of the command stream.
    if (!ready || !enabled() || rsxTrace) { for (uint32_t i = 0; i < count; ++i) { uint32_t w; std::memcpy(&w, args + 4 * i, 4); method(noInc ? m0 : m0 + 4 * i, __builtin_bswap32(w)); } return; }
    // methods with side effects, as a prefix count: a packet without any is one byte-swapping copy into R
    struct Special {
        uint16_t before[0x4001];   // number of special methods below index i
        bool is(uint32_t i) const { return before[i + 1] != before[i]; }
        Special() {
            uint16_t n = 0;
            for (uint32_t i = 0; i < 0x4000; ++i) {
                before[i] = n;
                const uint32_t m = i * 4;
                if ((m >= 0x0b80 && m < 0x0c00) || (m >= 0x1f00 && m < 0x1f80) || m == 0x1e9c || m == 0x1efc || m == 0x1808 || m == 0x1814 || m == 0x1824 || m == 0x1d94) ++n;
            }
            before[0x4000] = n;
        }
    };
    static const Special sp;
    const uint32_t i0 = m0 >> 2;
    if (!noInc && i0 + count <= 0x4000 && sp.before[i0 + count] == sp.before[i0]) { bswap32_block(R + i0, args, count); return; }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w; std::memcpy(&w, args + 4 * i, 4); w = __builtin_bswap32(w);
        const uint32_t m = noInc ? m0 : m0 + 4 * i;
        if (m >= 0x10000 || sp.is(m >> 2)) method(m, w);
        else R[m >> 2] = w;
    }
}

void method(uint32_t m, uint32_t arg) {
    if (!enabled()) return;
    if (m >= 0x10000) return;
    if (!initTried) init_window();
    if (!ready) { R[m >> 2] = arg; return; }
    if (rsxTrace) RLOG("[rsxm] %04x = %08x\n", m, arg);
    if (m >= 0x0b80 && m < 0x0c00) {   // transform program words
        if (vpLoad < 512 * 4) { vpCode[vpLoad++] = arg; ++vpGen; }
        return;
    }
    if (m >= 0x1f00 && m < 0x1f80) {   // transform constants
        const uint32_t i = cLoad * 4 + vpConstWord;
        if (i < 468 * 4) { std::memcpy(&vpConst[i], &arg, 4); vpConstDirty = true; vpDirtyLo = std::min(vpDirtyLo, i); vpDirtyHi = std::max(vpDirtyHi, i + 1); }
        if (++vpConstWord == 4) { vpConstWord = 0; ++cLoad; }
        return;
    }
    switch (m) {
        case 0x1e9c: vpLoad = (arg & 511) * 4; break;
        case 0x1efc: cLoad = arg; vpConstWord = 0; break;
        case 0x1808:   // SET_BEGIN_END
            if (arg) { prim = arg; ranges.clear(); }
            else { B->draw(); ranges.clear(); }
            break;
        case 0x1814: ranges.push_back({arg & 0xffffff, (arg >> 24) + 1}); rangesIndexed = false; break;   // DRAW_ARRAYS
        case 0x1824: ranges.push_back({arg & 0xffffff, (arg >> 24) + 1}); rangesIndexed = true; break;    // DRAW_INDEX_ARRAY
        case 0x1d94: {
            B->clear(arg);
            static const long logFrame = std::getenv("RSXLOG") ? std::atol(std::getenv("RSXLOG")) : -1;
            if (long(frameNo) == logFrame) RLOG("[clear] %x color %x (value %08x) z %x\n", arg, R[0x210 >> 2], R[0x1d90 >> 2], R[0x214 >> 2]);
            break;
        }
        default: break;
    }
    R[m >> 2] = arg;
}

} // namespace gti::recomp::rsx
