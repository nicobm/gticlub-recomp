// RSX -> Direct3D 11 back end (see rsx_core.h). Same model as the OpenGL one: RSX surfaces become render target
// textures, textures are converted from RSX memory and cached, the translated microcode runs as HLSL (the GLSL body
// the front end produces, rewritten: rsx_hlsl() below), draw state is rebuilt from the NV4097 registers.
// Differences with GL that matter: D3D has no texture swizzle (the RSX channel remap is a 4x4 matrix per unit in the
// pixel shader), no fans or line loops (gather_draw_indices converts them), no partial clears (scissored or masked
// clears draw a quad), window row 0 is the top (= RSX surface row 0, no flip needed).
#include "recomp/rsx_core.h"
#include "recomp/settings.h"

#ifdef _WIN32
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <condition_variable>
#include <deque>
#include <functional>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

namespace gti::recomp::rsx {
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11Device* dev = nullptr;
ID3D11DeviceContext* ctx = nullptr;
IDXGISwapChain1* swap = nullptr;
ID3D11RenderTargetView* backRtv = nullptr;
int backW = 0, backH = 0;
bool tearing = false, vsyncOn = false, devOk = false;
char gpuName[128] = "Direct3D 11";
pD3DCompile compileFn = nullptr;
typedef HRESULT(WINAPI* PFN_CreateBlob)(SIZE_T, ID3DBlob**);
PFN_CreateBlob createBlobFn = nullptr;
// what the draw path set last (clears and presents change them behind its back: reset to null there)
ID3D11VertexShader* lastVs = nullptr;
ID3D11PixelShader* lastPs = nullptr;
D3D11_PRIMITIVE_TOPOLOGY lastTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
void forget_pipeline() { lastVs = nullptr; lastPs = nullptr; lastTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; }

// ------------------------------------------------------------------ HLSL
// The GLSL body uses a small set of GLSL constructs; most map by #define or a helper function, the rest are
// rewritten here: one-argument vec4(x) (a splat, not valid HLSL) -> V4(x), and the texture calls (D3D samples
// through texture.Sample(sampler, ...); the result goes through the unit's channel remap rmap()).
static size_t skip_ws(const std::string& s, size_t i) { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; return i; }
static std::string rewrite(const std::string& s);
// arguments of the call whose '(' is at s[open]; returns the index after ')'
static size_t split_args(const std::string& s, size_t open, std::vector<std::string>& args) {
    int depth = 0; size_t start = open + 1;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') {
            if (--depth == 0) { args.push_back(s.substr(start, i - start)); return i + 1; }
        } else if (c == ',' && depth == 1) { args.push_back(s.substr(start, i - start)); start = i + 1; }
    }
    args.push_back(s.substr(start));
    return s.size();
}
static std::string trim(const std::string& a) { size_t b = a.find_first_not_of(" \t"), e = a.find_last_not_of(" \t"); return b == std::string::npos ? std::string() : a.substr(b, e - b + 1); }
static std::string rewrite(const std::string& s) {
    std::string o;
    o.reserve(s.size() + s.size() / 4);
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isalpha(uint8_t(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '_')) ++j;
            const std::string id = s.substr(i, j - i);
            const size_t p = skip_ws(s, j);
            const bool call = p < s.size() && s[p] == '(';
            if (call && (id == "vec4" || id == "texture" || id == "textureLod" || id == "textureGrad" || id == "shcmp")) {
                std::vector<std::string> a;
                const size_t after = split_args(s, p, a);
                for (auto& x : a) x = rewrite(trim(x));
                if (id == "vec4") {
                    if (a.size() == 1) o += "V4(" + a[0] + ")";
                    else { o += "float4("; for (size_t k = 0; k < a.size(); ++k) o += (k ? ", " : "") + a[k]; o += ")"; }
                } else {
                    const std::string unit = a[0].size() > 3 ? a[0].substr(3) : "0";   // "texN"
                    const std::string smp = "smp" + unit;
                    if (id == "shcmp") o += "shcmp(" + a[0] + ", " + smp + ", " + a[1] + ", " + a[2] + ")";
                    else {
                        std::string call2;
                        if (id == "texture" && a.size() == 2) call2 = a[0] + ".Sample(" + smp + ", " + a[1] + ")";
                        else if (id == "texture") call2 = a[0] + ".SampleBias(" + smp + ", " + a[1] + ", " + a[2] + ")";
                        else if (id == "textureLod") call2 = a[0] + ".SampleLevel(" + smp + ", " + a[1] + ", " + a[2] + ")";
                        else call2 = a[0] + ".SampleGrad(" + smp + ", " + a[1] + ", " + a[2] + ", " + a[3] + ")";
                        o += "rmap(" + unit + ", " + call2 + ")";
                    }
                }
                i = after;
                continue;
            }
            o += id;
            i = j;
            continue;
        }
        if (std::isdigit(uint8_t(c))) {   // numbers (with their exponent / suffix) as they are
            size_t j = i;
            while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '.' || ((s[j] == '-' || s[j] == '+') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) ++j;
            o.append(s, i, j - i);
            i = j;
            continue;
        }
        o += c;
        ++i;
    }
    return o;
}

static const char* const kHlslCommon =
    "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n#define ivec4 int4\n"
    "#define fract frac\n#define mix lerp\n#define inversesqrt rsqrt\n#define dFdx ddx\n#define dFdy ddy\n"
    "#define uintBitsToFloat asfloat\n#define floatBitsToUint asuint\n"
    "float4 V4(float x) { return float4(x, x, x, x); }\n"
    "float4 V4(float4 x) { return x; }\n"
    "float4 V4(bool4 x) { return float4(x); }\n"
    "bool4 lessThan(float4 a, float4 b) { return a < b; }\n"
    "bool4 lessThanEqual(float4 a, float4 b) { return a <= b; }\n"
    "bool4 greaterThan(float4 a, float4 b) { return a > b; }\n"
    "bool4 greaterThanEqual(float4 a, float4 b) { return a >= b; }\n"
    "bool4 equal(float4 a, float4 b) { return a == b; }\n"
    "bool4 notEqual(float4 a, float4 b) { return a != b; }\n"
    "cbuffer DC : register(b1) { float4 u_vscale, u_voffset, u_surf, u_alpha, u_wpos; float4 u_tsc[16]; float4 u_rm[64]; float4 u_ro[16]; };\n";

static const char* const kHlslVsHelpers =
    "cbuffer VC : register(b0) { float4 vc[468]; };\n"
    "float rcc(float x) { float r = 1.0 / x; return sign(r) * clamp(abs(r), 5.42101e-20, 1.884467e19); }\n"
    "float4 vexp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "float4 vlog(float x) { float a = abs(x); float e = floor(log2(a)); return float4(e, a / exp2(e), log2(a), 1.0); }\n"
    "float4 vlit(float4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0; return float4(1.0, d, sp, 1.0); }\n";

static const char* const kHlslPsHelpers =
    "bool fcond1(float c, int lt, int eq, int gt) { return (lt != 0 && c < 0.0) || (eq != 0 && c == 0.0) || (gt != 0 && c > 0.0); }\n"
    "bool fcond(float4 c, int lt, int eq, int gt) { return fcond1(c.x, lt, eq, gt) || fcond1(c.y, lt, eq, gt) || fcond1(c.z, lt, eq, gt) || fcond1(c.w, lt, eq, gt); }\n"
    "float shcmp(Texture2D t, SamplerState s, float3 p, float f) {\n"
    "  float4 d = t.GatherRed(s, p.xy); float4 r = (p.z - SHBIAS).xxxx; float4 c;\n"
    "  if (f < 1.5) c = float4(d < r); else if (f < 2.5) c = float4(d == r); else if (f < 3.5) c = float4(d <= r);\n"
    "  else if (f < 4.5) c = float4(d > r); else if (f < 5.5) c = float4(d != r); else c = float4(d >= r);\n"
    "  uint w, h; t.GetDimensions(w, h);\n"
    "  float2 fr = frac(p.xy * float2(w, h) - 0.5);\n"
    "  return lerp(lerp(c.w, c.z, fr.x), lerp(c.x, c.y, fr.x), fr.y);\n}\n"
    "float4 flit(float4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? exp2(s.w * log2(max(s.y, 1e-30))) : 0.0; return float4(1.0, d, sp, 1.0); }\n"
    "float4 frefl(float4 n, float4 e) { return float4(2.0 * dot(n.xyz, e.xyz) / dot(n.xyz, n.xyz) * n.xyz - e.xyz, 0.0); }\n"
    "uint packUnorm4x8(float4 v) { uint4 u = (uint4)round(saturate(v) * 255.0); return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24); }\n"
    "float4 unpackUnorm4x8(uint p) { return float4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24) / 255.0; }\n"
    "uint packSnorm4x8(float4 v) { int4 i = (int4)round(clamp(v, -1.0, 1.0) * 127.0); uint4 u = asuint(i) & 255u; return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24); }\n"
    "float4 unpackSnorm4x8(uint p) { int4 i = int4(asint(p << 24), asint(p << 16), asint(p << 8), asint(p)) >> 24; return clamp(float4(i) / 127.0, -1.0, 1.0); }\n"
    "uint packSnorm2x16(float2 v) { int2 i = (int2)round(clamp(v, -1.0, 1.0) * 32767.0); return (asuint(i.x) & 0xffffu) | (asuint(i.y) << 16); }\n"
    "float2 unpackSnorm2x16(uint p) { int2 i = int2(asint(p << 16), asint(p)) >> 16; return clamp(float2(i) / 32767.0, -1.0, 1.0); }\n"
    "uint packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
    "float2 unpackHalf2x16(uint p) { return float2(f16tof32(p), f16tof32(p >> 16)); }\n"
    "float4 rmap(int n, float4 s) { return float4(dot(s, u_rm[4 * n]), dot(s, u_rm[4 * n + 1]), dot(s, u_rm[4 * n + 2]), dot(s, u_rm[4 * n + 3])) + u_ro[n]; }\n";

static const char* const kVaryings =
    "  float4 pos : SV_Position; float4 col0 : COLOR0; float4 col1 : COLOR1; float4 bfc0 : COLOR2; float4 bfc1 : COLOR3; float4 fog : FOG; float4 tc[10] : TEXCOORD0;\n";

static std::string hlsl_vs(const VpInfo& vi) {
    std::string h = std::string(kHlslCommon) + kHlslVsHelpers;
    h += "struct VI {\n";
    char t[160];
    for (int k = 0; k < 16; ++k) if (vi.inputs & (1u << k)) { std::snprintf(t, sizeof t, "  float4 in%d : ATTR%d;\n", k, k); h += t; }
    h += "  uint vid : SV_VertexID;\n};\nstruct VO {\n" + std::string(kVaryings) + "};\n";
    for (int k = 0; k < 16; ++k) if (vi.inputs & (1u << k)) { std::snprintf(t, sizeof t, "static float4 in%d;\n", k); h += t; }
    h += "static float4 v_col0, v_col1, v_bfc0, v_bfc1, v_fog;\nstatic float4 v_tc[10];\n"
         "VO main(VI vin) {\n";
    for (int k = 0; k < 16; ++k) if (vi.inputs & (1u << k)) { std::snprintf(t, sizeof t, "  in%d = vin.in%d;\n", k, k); h += t; }
    h += "  float4 R[64]; for (int i = 0; i < 64; ++i) R[i] = float4(0.0, 0.0, 0.0, 0.0);\n"
         "  float4 t = float4(0.0, 0.0, 0.0, 0.0), CC0 = t, CC1 = t; int4 A0 = int4(0, 0, 0, 0), A1 = A0;\n"
         "  float4 o_pos = float4(0.0, 0.0, 0.0, 1.0), o_psz = float4(1.0, 1.0, 1.0, 1.0), o_dummy = t;\n"
         "  v_col0 = float4(1.0, 1.0, 1.0, 1.0); v_col1 = t; v_bfc0 = v_col0; v_bfc1 = t; v_fog = t;\n"
         "  for (int j = 0; j < 10; ++j) v_tc[j] = float4(0.0, 0.0, 0.0, 1.0);\n";
    h += rewrite(vi.body);
    // the RSX viewport transform folded into clip space as in GL, y negated: D3D's clip y = +1 is the top row
    h += "  VO o;\n"
         "  o.pos = float4(o_pos.x * (2.0 * u_vscale.x / u_surf.x) + o_pos.w * (2.0 * u_voffset.x / u_surf.x - 1.0),\n"
         "                -(o_pos.y * (2.0 * u_vscale.y / u_surf.y) + o_pos.w * (2.0 * u_voffset.y / u_surf.y - 1.0)),\n"
         "                o_pos.z * u_vscale.z + o_pos.w * u_voffset.z, o_pos.w);\n"
         "  o.col0 = v_col0; o.col1 = v_col1; o.bfc0 = v_bfc0; o.bfc1 = v_bfc1; o.fog = v_fog;\n"
         "  for (int k = 0; k < 10; ++k) o.tc[k] = v_tc[k];\n"
         "  return o;\n}\n";
    return h;
}

static std::string hlsl_ps(const FpInfo& fi, uint32_t texDims, uint32_t control) {
    std::string h = std::string("#define SHBIAS ") + (std::getenv("SHBIAS") ? std::getenv("SHBIAS") : "0.002") + "\n" + kHlslCommon + kHlslPsHelpers;
    char t[200];
    std::snprintf(t, sizeof t, "cbuffer FC : register(b2) { float4 fc[%u]; };\n", std::max(1u, fi.nConst));
    h += t;
    for (int k = 0; k < 16; ++k) {
        if (!(fi.texUsed & (1u << k))) continue;
        const uint32_t dim = (texDims >> (2 * k)) & 3;
        std::snprintf(t, sizeof t, "%s tex%d : register(t%d); SamplerState smp%d : register(s%d);\n", dim == 1 ? "TextureCube" : dim == 2 ? "Texture3D" : "Texture2D", k, k, k, k);
        h += t;
    }
    const bool depthOut = (control & 0xe) != 0;
    h += "struct PI {\n" + std::string(kVaryings) + "  bool ff : SV_IsFrontFace;\n};\n"
         "struct PO { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;" + std::string(depthOut ? " float d : SV_Depth;" : "") + " };\n"
         "static float4 v_col0, v_col1, v_bfc0, v_bfc1, v_fog;\nstatic float4 v_tc[10];\n"
         "float4 col0() { return v_col0; }\nfloat4 col1() { return v_col1; }\n"
         "PO main(PI pin) {\n"
         "  v_col0 = pin.col0; v_col1 = pin.col1; v_bfc0 = pin.bfc0; v_bfc1 = pin.bfc1; v_fog = pin.fog;\n"
         "  for (int k = 0; k < 10; ++k) v_tc[k] = pin.tc[k];\n"
         "  float4 R[64]; float4 H[64]; for (int i = 0; i < 64; ++i) { R[i] = float4(0.0, 0.0, 0.0, 0.0); H[i] = R[i]; }\n"
         "  float4 t = float4(0.0, 0.0, 0.0, 0.0), CC0 = t, CC1 = t;\n"
         "  float4 wpos = float4(pin.pos.x * u_wpos.x, pin.pos.y * u_wpos.y, pin.pos.z, pin.pos.w);\n"
         "  float4 ssa = pin.ff ? float4(1.0, 1.0, 1.0, 1.0) : float4(-1.0, -1.0, -1.0, -1.0);\n";
    h += rewrite(fi.body);
    const bool out32 = (control & 0x40) != 0;
    const bool useR = fi.writesR0 && (!fi.writesH0 || out32);
    h += "  PO o;\n";
    h += useR ? "  o.c0 = R[0]; o.c1 = R[2]; o.c2 = R[3]; o.c3 = R[4];\n" : "  o.c0 = H[0]; o.c1 = H[4]; o.c2 = H[6]; o.c3 = H[8];\n";
    if (depthOut) h += "  o.d = R[1].z;\n";
    h += "  if (u_alpha.x > 0.5) {\n"
         "    int f = int(u_alpha.y); float a = o.c0.a, r = u_alpha.z; bool okA = true;\n"
         "    if (f == 0) okA = false; else if (f == 1) okA = a < r; else if (f == 2) okA = a == r; else if (f == 3) okA = a <= r;\n"
         "    else if (f == 4) okA = a > r; else if (f == 5) okA = a != r; else if (f == 6) okA = a >= r;\n"
         "    if (!okA) discard;\n  }\n"
         "  return o;\n}\n";
    return h;
}

// HLSL -> bytecode, kept on disk (d3dcache/): FXC takes tens of milliseconds per shader
static void cache_name(char* name, size_t n, const std::string& src, const char* profile) {
    std::snprintf(name, n, "d3dcache/%016llx_%s.bin", (unsigned long long)fnv(src.data(), src.size(), fnv(profile, std::strlen(profile))), profile);
}
static bool in_cache(const std::string& src, const char* profile) {
    char name[96]; cache_name(name, sizeof name, src, profile);
    if (FILE* f = std::fopen(name, "rb")) { std::fclose(f); return true; }
    return false;
}
static ID3DBlob* compile_hlsl(const std::string& src, const char* profile, const char* what) {   // any thread
    char name[96];
    cache_name(name, sizeof name, src, profile);
    ID3DBlob* blob = nullptr;
    if (FILE* f = std::fopen(name, "rb")) {
        std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0 && createBlobFn && SUCCEEDED(createBlobFn(size_t(n), &blob)) && std::fread(blob->GetBufferPointer(), 1, size_t(n), f) == size_t(n)) { std::fclose(f); return blob; }
        release(blob); std::fclose(f);
    }
    if (!compileFn) return nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = compileFn(src.data(), src.size(), what, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err);
    if (FAILED(hr)) {
        RLOG("[d3d] %s shader failed: %s\n", what, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        static int dumped = 0;
        if (dumped++ < 5) { char fn[64]; std::snprintf(fn, sizeof fn, "d3d_failed_%d.hlsl", dumped); if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(src.data(), 1, src.size(), f); std::fclose(f); } }
        release(err); release(blob);
        return nullptr;
    }
    release(err);
    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f); std::fclose(f); }
    return blob;
}

// ------------------------------------------------------------------ programs
// A program whose shaders are not in d3dcache/ yet is compiled by a worker thread (FXC takes 20-60 ms per program,
// a visible hitch every time a new effect appears on a first play); its draws are skipped until it is ready, which
// only shows as an object missing for a frame or two. D3DSYNC=1: compile on the spot as before.
struct Program { ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3DBlob* vsCode = nullptr; uint32_t vpInputs = 0, texUsed = 0, nConst = 0;
                 std::atomic<int> state{0};   // 0 compiling, 1 ready, 2 failed
                 bool bad() const { return state.load(std::memory_order_acquire) != 1; } };
std::unordered_map<uint64_t, Program> programs;
// builds p's shaders from the HLSL (any thread: D3D11 devices are free-threaded)
void build_program(Program& p, const std::string& vsrc, const std::string& psrc) {
    ID3DBlob* vsCode = compile_hlsl(vsrc, "vs_5_0", "vertex");
    ID3DBlob* psCode = compile_hlsl(psrc, "ps_5_0", "pixel");
    bool ok = vsCode && psCode && SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &p.vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &p.ps));
    release(psCode);
    p.vsCode = vsCode;
    p.state.store(ok ? 1 : 2, std::memory_order_release);
}
struct CompileQueue {
    std::mutex mx; std::condition_variable cv; std::deque<std::function<void()>> jobs; bool started = false;
    std::atomic<int> pending{0};
    void push(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(mx);
            if (!started) {
                started = true;
                const int n = std::clamp(int(SDL_GetNumLogicalCPUCores()) / 4, 1, 3);
                for (int i = 0; i < n; ++i)
                    if (SDL_Thread* t = SDL_CreateThread([](void* q) -> int { static_cast<CompileQueue*>(q)->run(); return 0; }, "shaders", this)) SDL_DetachThread(t);
            }
            jobs.push_back(std::move(f)); ++pending;
        }
        cv.notify_one();
    }
    void run() {
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        for (;;) {
            std::function<void()> f;
            { std::unique_lock<std::mutex> lk(mx); cv.wait(lk, [&] { return !jobs.empty(); }); f = std::move(jobs.front()); jobs.pop_front(); }
            f(); --pending;
        }
    }
} compileQueue;
uint64_t skippedDraws = 0;
std::vector<float> fpConsts;
Program* get_program(uint32_t vpStart, uint32_t fpEa, uint32_t texDims, uint32_t control) {
    uint32_t n = 0;
    while (vpStart + n < 512) { ++n; if (vpCode[(vpStart + n - 1) * 4 + 3] & 1) break; }
    static uint64_t vpKeyGen = 0, vpKey = 0; static uint32_t vpKeyStart = ~0u;
    if (vpKeyGen != vpGen || vpKeyStart != vpStart) { vpKey = fnv(vpCode + vpStart * 4, n * 16); vpKeyGen = vpGen; vpKeyStart = vpStart; }
    uint64_t key = vpKey;
    {
        struct FpMemo { std::vector<uint8_t> raw; uint64_t h = 0; std::vector<float> consts; };
        static std::unordered_map<uint32_t, FpMemo> memo;
        FpMemo& fm = memo[fpEa];
        if (fm.raw.empty() || std::memcmp(C.mem + fpEa, fm.raw.data(), fm.raw.size()) != 0) {
            fm.consts.clear();
            fm.h = fp_walk(fpEa, 0x51ed270b3c6f84d5ull, &fm.consts);
            const uint32_t sz = fp_size(fpEa);
            fm.raw.assign(C.mem + fpEa, C.mem + fpEa + sz);
        }
        fpConsts = fm.consts;
        key = (key ^ fm.h) * 0x9E3779B97F4A7C15ull ^ fm.h;
    }
    key ^= (uint64_t(texDims) << 1) ^ (uint64_t(control & 0x4e) << 40) ^ uint64_t(vpStart) * 0x9E3779B97F4A7C15ull;
    auto it = programs.find(key);
    if (it != programs.end()) return &it->second;
    Program& p = programs[key];
    ++g_progBuilds;
    TickScope progTime(g_progTicks);
    const uint64_t t0 = SDL_GetPerformanceCounter();
    const VpInfo vi = translate_vp(vpCode, vpStart);
    const FpInfo fi = translate_fp(fpEa, texDims, control);
    p.vpInputs = vi.inputs; p.texUsed = fi.texUsed; p.nConst = fi.nConst;
    const std::string vsrc = hlsl_vs(vi), psrc = hlsl_ps(fi, texDims, control);
    if (dumpShaders) {
        static int nShader = 0;
        char fn[128];
        std::snprintf(fn, sizeof fn, "rsxdump/d3d_%04d_%016llx.hlsl", nShader++, (unsigned long long)key);
        if (FILE* f = std::fopen(fn, "wb")) { std::fprintf(f, "%s\n// ---- pixel\n%s", vsrc.c_str(), psrc.c_str()); std::fclose(f); }
    }
    static const bool sync = std::getenv("D3DSYNC") != nullptr;
    if (sync || (in_cache(vsrc, "vs_5_0") && in_cache(psrc, "ps_5_0"))) {
        build_program(p, vsrc, psrc);
        const double ms = double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
        if (ms > 2.0) RLOG("[prog] %016llx %.1f ms (d3d11)\n", (unsigned long long)key, ms);
    } else {
        Program* pp = &p;   // (unordered_map nodes do not move)
        compileQueue.push([pp, vsrc, psrc, key, t0] {
            build_program(*pp, vsrc, psrc);
            RLOG("[prog] %016llx compiled in the background, ready %.0f ms after first use\n", (unsigned long long)key,
                 double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        });
    }
    return &p;
}

// ------------------------------------------------------------------ state objects (cached by register values)
template <class T> struct StateCache { std::unordered_map<uint64_t, T*> m; T* last = nullptr; };
StateCache<ID3D11BlendState> blendStates;
StateCache<ID3D11DepthStencilState> depthStates;
StateCache<ID3D11RasterizerState> rasterStates;
std::unordered_map<uint64_t, ID3D11SamplerState*> samplers;
ID3D11SamplerState* linearClamp = nullptr;

D3D11_BLEND blend_of(uint32_t gl, bool alpha) {
    switch (gl) {
        case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
        case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND_OP blendop_of(uint32_t gl) {
    switch (gl) { case 0x8007: return D3D11_BLEND_OP_MIN; case 0x8008: return D3D11_BLEND_OP_MAX; case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
                  case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT; default: return D3D11_BLEND_OP_ADD; }
}
D3D11_COMPARISON_FUNC cmp_of(uint32_t gl, uint32_t dflt) { const uint32_t v = gl ? gl : dflt; return (v >= 0x200 && v <= 0x207) ? D3D11_COMPARISON_FUNC(v - 0x200 + 1) : D3D11_COMPARISON_ALWAYS; }
D3D11_STENCIL_OP stencilop_of(uint32_t gl) {
    switch (gl) { case 0: return D3D11_STENCIL_OP_ZERO; case 0x1E01: return D3D11_STENCIL_OP_REPLACE; case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
                  case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT; case 0x150A: return D3D11_STENCIL_OP_INVERT; case 0x8507: return D3D11_STENCIL_OP_INCR;
                  case 0x8508: return D3D11_STENCIL_OP_DECR; default: return D3D11_STENCIL_OP_KEEP; }
}

void apply_state() {
    {   // blend
        const uint32_t en = R[0x310 >> 2] & 1, sf = R[0x314 >> 2], df = R[0x318 >> 2], eq = R[0x320 >> 2], cm = R[0x324 >> 2];
        const uint32_t k[5] = {en, sf, df, eq, cm};
        const uint64_t h = fnv(k, sizeof k);
        ID3D11BlendState*& s = blendStates.m[h];
        if (!s) {
            D3D11_BLEND_DESC d{};
            d.IndependentBlendEnable = FALSE;
            auto& rt = d.RenderTarget[0];
            rt.BlendEnable = en;
            rt.SrcBlend = blend_of(sf & 0xffff, false); rt.DestBlend = blend_of(df & 0xffff, false);
            rt.SrcBlendAlpha = blend_of(sf >> 16, true); rt.DestBlendAlpha = blend_of(df >> 16, true);
            rt.BlendOp = blendop_of(eq & 0xffff); rt.BlendOpAlpha = blendop_of(eq >> 16);
            rt.RenderTargetWriteMask = UINT8(((cm >> 16) & 1) | (((cm >> 8) & 1) << 1) | ((cm & 1) << 2) | (((cm >> 24) & 1) << 3));
            dev->CreateBlendState(&d, &s);
        }
        const uint32_t bc = R[0x31c >> 2];
        const float f[4] = {((bc >> 16) & 0xff) / 255.f, ((bc >> 8) & 0xff) / 255.f, (bc & 0xff) / 255.f, (bc >> 24) / 255.f};
        ctx->OMSetBlendState(s, f, 0xffffffffu);
    }
    {   // depth / stencil
        const uint32_t k[10] = {R[0xa74 >> 2] & 1, R[0xa6c >> 2], R[0xa70 >> 2] & 1, R[0x328 >> 2] & 1, R[0x330 >> 2], R[0x338 >> 2], R[0x33c >> 2], R[0x340 >> 2], R[0x344 >> 2], R[0x32c >> 2]};
        const uint64_t h = fnv(k, sizeof k);
        ID3D11DepthStencilState*& s = depthStates.m[h];
        if (!s) {
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = k[0]; d.DepthFunc = cmp_of(k[1], 0x201); d.DepthWriteMask = k[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            d.StencilEnable = k[3]; d.StencilReadMask = UINT8(k[5]); d.StencilWriteMask = UINT8(k[9]);
            D3D11_DEPTH_STENCILOP_DESC o{stencilop_of(k[6] ? k[6] : 0x1e00), stencilop_of(k[7] ? k[7] : 0x1e00), stencilop_of(k[8] ? k[8] : 0x1e00), cmp_of(k[4], 0x207)};
            d.FrontFace = o; d.BackFace = o;
            dev->CreateDepthStencilState(&d, &s);
        }
        ctx->OMSetDepthStencilState(s, R[0x334 >> 2] & 0xff);
    }
    {   // rasterizer
        static const bool noCull = std::getenv("RSXNOCULL") != nullptr, clampZ = std::getenv("RSXCLAMP") != nullptr;
        const uint32_t k[6] = {!noCull && (R[0x183c >> 2] & 1), R[0x1830 >> 2], R[0x1834 >> 2], R[0xa68 >> 2] & 1, R[0xa78 >> 2], R[0xa7c >> 2]};
        const uint64_t h = fnv(k, sizeof k);
        ID3D11RasterizerState*& s = rasterStates.m[h];
        if (!s) {
            D3D11_RASTERIZER_DESC d{};
            d.FillMode = D3D11_FILL_SOLID;
            const uint32_t face = k[1] ? k[1] : 0x405;
            d.CullMode = !k[0] ? D3D11_CULL_NONE : face == 0x404 ? D3D11_CULL_FRONT : face == 0x405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            const uint32_t ff = k[2] ? k[2] : 0x901;   // GL_CCW
            static const bool flipFront = std::getenv("D3DFRONTFLIP") != nullptr;   // (debug) the other winding convention
            d.FrontCounterClockwise = (ff == 0x901) != flipFront;
            if (k[3]) { d.DepthBias = INT(f32(k[5])); d.SlopeScaledDepthBias = f32(k[4]); }
            d.DepthClipEnable = !clampZ;
            d.ScissorEnable = TRUE;
            dev->CreateRasterizerState(&d, &s);
        }
        ctx->RSSetState(s);
    }
}

D3D11_TEXTURE_ADDRESS_MODE wrap_of(uint32_t w) {
    switch (w) { case 1: return D3D11_TEXTURE_ADDRESS_WRAP; case 2: return D3D11_TEXTURE_ADDRESS_MIRROR; case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
                 case 6: case 8: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE; default: return D3D11_TEXTURE_ADDRESS_CLAMP; }
}
// sampler for unit registers (plain textures) or a surface (render target: mag filter and wrap only)
ID3D11SamplerState* sampler_for(uint32_t base, uint32_t mips, bool surface) {
    const uint32_t addr = R[(base + 8) >> 2], ctl0 = R[(base + 0xc) >> 2], filt = R[(base + 0x14) >> 2], border = R[(base + 0x1c) >> 2];
    const uint32_t k[7] = {addr, surface ? 0u : ctl0, filt, border, surface ? 1u : mips, uint32_t(settings().aniso), surface};
    const uint64_t h = fnv(k, sizeof k);
    ID3D11SamplerState*& s = samplers[h];
    if (s) return s;
    D3D11_SAMPLER_DESC d{};
    const uint32_t minF = (filt >> 16) & 0xff, magF = (filt >> 24) & 7;
    const bool magLin = magF != 1;
    bool minLin, mipLin = false;
    if (surface) minLin = magLin;
    else if (mips > 1) { const uint32_t m = minF & 7; minLin = m == 2 || m == 4 || m == 6 || m == 7; mipLin = m == 5 || m == 6; }
    else minLin = !(minF == 1 || minF == 3 || minF == 5);
    d.Filter = D3D11_FILTER((minLin ? 0x10 : 0) | (magLin ? 0x4 : 0) | (mipLin ? 0x1 : 0));
    const uint32_t gameAniso = std::min<uint32_t>(4, (ctl0 >> 4) & 7);
    const uint32_t an = !surface && mips > 1 ? std::max<uint32_t>(gameAniso, uint32_t(std::clamp(settings().aniso, 0, 4))) : 0;
    if (an > 0 && minLin) { d.Filter = D3D11_FILTER_ANISOTROPIC; d.MaxAnisotropy = 1u << an; }
    d.AddressU = wrap_of(addr & 0xf); d.AddressV = wrap_of((addr >> 8) & 0xf); d.AddressW = wrap_of((addr >> 16) & 0xf);
    d.BorderColor[0] = ((border >> 16) & 0xff) / 255.f; d.BorderColor[1] = ((border >> 8) & 0xff) / 255.f; d.BorderColor[2] = (border & 0xff) / 255.f; d.BorderColor[3] = (border >> 24) / 255.f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (surface) { d.MinLOD = 0; d.MaxLOD = 0; }
    else { d.MinLOD = ((ctl0 >> 19) & 0xfff) / 256.f; d.MaxLOD = mips > 1 ? ((ctl0 >> 7) & 0xfff) / 256.f : 0.f; if (d.MaxLOD < d.MinLOD) d.MaxLOD = d.MinLOD; }
    if (!surface && mips > 1 && ((minF & 7) == 1 || (minF & 7) == 2)) { d.MinLOD = 0.f; d.MaxLOD = 0.f; }   // no mipmapping: base level only (RSX, GL)
    dev->CreateSamplerState(&d, &s);
    return s;
}

// ------------------------------------------------------------------ surfaces (render targets)
struct Surface { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr; ID3D11ShaderResourceView* srv = nullptr;
                 uint32_t w = 0, h = 0, fmt = 0; bool depth = false; DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN; };
std::map<uint64_t, Surface> surfaces;   // key: location << 33 | depth << 32 | offset
uint64_t surfGen = 1;                   // bumped when a surface is (re)created: cached bindings may be stale

DXGI_FORMAT surface_format(uint32_t fmt) {
    switch (fmt) {
        case 3: return DXGI_FORMAT_B5G6R5_UNORM; case 9: return DXGI_FORMAT_R8_UNORM; case 0xa: return DXGI_FORMAT_R8G8_UNORM;
        case 0xb: return DXGI_FORMAT_R16G16B16A16_FLOAT; case 0xc: return DXGI_FORMAT_R32G32B32A32_FLOAT; case 0xd: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
void free_surface(Surface& s) { release(s.rtv); release(s.dsv); release(s.srv); release(s.tex); }
bool make_surface(Surface& s, uint32_t w, uint32_t h, uint32_t fmt, bool depth) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    if (depth) {
        const bool d16 = fmt == 1;
        d.Format = d16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.tex))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = d16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(s.tex, &dv, &s.dsv);
        sv.Format = d16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        s.dxgi = dv.Format;
    } else {
        d.Format = surface_format(fmt);
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.tex))) { d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.tex))) return false; }
        dev->CreateRenderTargetView(s.tex, nullptr, &s.rtv);
        sv.Format = d.Format;
        s.dxgi = d.Format;
    }
    dev->CreateShaderResourceView(s.tex, &sv, &s.srv);
    s.w = w; s.h = h; s.fmt = fmt; s.depth = depth;
    return true;
}
Surface* get_surface(uint32_t loc, uint32_t off, uint32_t w, uint32_t h, uint32_t fmt, bool depth) {
    const uint64_t key = (uint64_t(loc) << 33) | (uint64_t(depth) << 32) | off;
    Surface& s = surfaces[key];
    if (s.tex && s.w == w && s.h == h && s.fmt == fmt) return &s;
    // the same address with several sizes in one frame (depth of the cube faces and of the scene): keep each
    struct Parked { uint64_t key; Surface s; };
    static std::vector<Parked> parked;
    if (s.tex) parked.push_back({key, s});
    s = Surface{};
    ++surfGen;
    for (size_t k = 0; k < parked.size(); ++k)
        if (parked[k].key == key && parked[k].s.w == w && parked[k].s.h == h && parked[k].s.fmt == fmt) { s = parked[k].s; parked.erase(parked.begin() + long(k)); return &s; }
    if (!make_surface(s, w, h, fmt, depth)) { RLOG("[d3d] surface %ux%u format %u failed\n", w, h, fmt); return nullptr; }
    RLOG("[rsx] surface %s at %s 0x%x: %ux%u format %u\n", depth ? "depth" : "color", loc ? "main" : "local", off, w, h, fmt);
    return &s;
}

struct Target { uint32_t w = 0, h = 0, aaX = 1, aaY = 1, logW = 0, logH = 0; int nColor = 0; ID3D11RenderTargetView* rtv[4] = {}; ID3D11DepthStencilView* dsv = nullptr; uint64_t key = 0; };
Target cur;
ID3D11ShaderResourceView* boundSrv[16] = {};
ID3D11SamplerState* boundSmp[16] = {};
void unbind_textures() {
    ID3D11ShaderResourceView* none[16] = {};
    ctx->PSSetShaderResources(0, 16, none);
    std::memset(boundSrv, 0, sizeof boundSrv);
}

bool bind_target() {
    const uint32_t fmtReg = R[0x208 >> 2];
    const uint32_t cfmt = fmtReg & 0x1f, zfmt = (fmtReg >> 5) & 7, aa = (fmtReg >> 12) & 0xf;
    const uint32_t clipW = R[0x200 >> 2] >> 16, clipH = R[0x204 >> 2] >> 16, clipX = R[0x200 >> 2] & 0xffff, clipY = R[0x204 >> 2] & 0xffff;
    uint32_t ax = 1, ay = 1;
    switch (aa) { case 3: ax = 2; break; case 4: case 5: ax = 2; ay = 2; break; default: break; }
    if (clipX + clipW == 1280 && clipY + clipH == 720) { const uint32_t sc = uint32_t(settings().scale); ax *= sc; ay *= sc; }
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
    uint32_t kk[12] = {w, h, fmtReg, ct, R[0x210 >> 2], R[0x218 >> 2], R[0x288 >> 2], R[0x28c >> 2], R[0x214 >> 2], R[0x194 >> 2] ^ (R[0x18c >> 2] << 1) ^ (R[0x198 >> 2] << 2), uint32_t(surfGen), uint32_t(surfGen >> 32)};
    const uint64_t key = fnv(kk, sizeof kk);
    if (key == cur.key && cur.w) return true;
    Target t;
    t.w = w; t.h = h; t.aaX = ax; t.aaY = ay; t.logW = clipX + clipW; t.logH = clipY + clipH;
    for (int k = 0; k < nList; ++k) {
        const int i = list[k];
        Surface* s = get_surface(R[dmaReg[i] >> 2] & 1, R[offReg[i] >> 2], w, h, cfmt, false);
        if (s) t.rtv[t.nColor++] = s->rtv;
    }
    if (zfmt) if (Surface* s = get_surface(R[0x198 >> 2] & 1, R[0x214 >> 2], w, h, zfmt, true)) t.dsv = s->dsv;
    if (!t.nColor && !t.dsv) return false;
    // get_surface may have made a surface: the key includes surfGen, so compute it again
    kk[10] = uint32_t(surfGen); kk[11] = uint32_t(surfGen >> 32);
    t.key = fnv(kk, sizeof kk);
    unbind_textures();   // a texture bound for sampling cannot be a render target at the same time
    ctx->OMSetRenderTargets(UINT(t.nColor), t.nColor ? t.rtv : nullptr, t.dsv);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    cur = t;
    return true;
}

void scissor_rect(D3D11_RECT& r) {
    uint32_t sx = R[0x8c0 >> 2] & 0xffff, sw = R[0x8c0 >> 2] >> 16, sy = R[0x8c4 >> 2] & 0xffff, sh = R[0x8c4 >> 2] >> 16;
    const uint32_t vx = R[0xa00 >> 2] & 0xffff, vw = R[0xa00 >> 2] >> 16, vy = R[0xa04 >> 2] & 0xffff, vh = R[0xa04 >> 2] >> 16;
    if (sw == 0 && sh == 0) { sw = 4096; sh = 4096; }
    const int x0 = int(std::max(sx, vx)), y0 = int(std::max(sy, vy));
    const int x1 = int(std::min(sx + sw, vx + vw)), y1 = int(std::min(sy + sh, vy + vh));
    r.left = LONG(x0 * int(cur.aaX)); r.top = LONG(y0 * int(cur.aaY));
    r.right = LONG(std::max(x0, x1) * int(cur.aaX)); r.bottom = LONG(std::max(y0, y1) * int(cur.aaY));
}

// ------------------------------------------------------------------ buffers
// One dynamic vertex ring and one index ring (written with NO_OVERWRITE, DISCARD when they wrap) and the constant
// buffers. Static vertex streams (local memory) are converted once into big default buffers, like the GL back end.
struct Ring { ID3D11Buffer* buf = nullptr; size_t size = 0, head = 0; };
Ring vring, iring;
ID3D11Buffer* cbVc = nullptr;   // b0: transform constants
ID3D11Buffer* cbDc = nullptr;   // b1: per-draw values
ID3D11Buffer* cbFc = nullptr;   // b2: fragment program constants
size_t cbFcSize = 0;
bool make_ring(Ring& r, size_t size, UINT bind) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = UINT(size); d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = bind; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    r.size = size; r.head = 0;
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, &r.buf));
}
// room for bytes in the ring: mapped pointer and its offset; unmap with ring_done
uint8_t* ring_map(Ring& r, size_t bytes, size_t align, size_t& off) {
    if (bytes > r.size) return nullptr;
    size_t h = (r.head + align - 1) & ~(align - 1);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (h + bytes > r.size) { h = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(r.buf, 0, mode, 0, &m))) return nullptr;
    r.head = h + bytes; off = h;
    return static_cast<uint8_t*>(m.pData) + h;
}
void ring_done(Ring& r) { ctx->Unmap(r.buf, 0); }
void write_cb(ID3D11Buffer* b, const void* p, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { std::memcpy(m.pData, p, n); ctx->Unmap(b, 0); }
}
ID3D11Buffer* make_cb(size_t bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = UINT((bytes + 15) & ~size_t(15)); d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&d, nullptr, &b);
    return b;
}

struct Stream { ID3D11Buffer* buf = nullptr; size_t offset = 0; uint32_t count = 0; uint64_t sig = 0, checked = ~0ull; };
std::unordered_map<uint64_t, Stream> streams;
constexpr size_t kArenaChunk = size_t(64) << 20, kArenaMaxChunks = 12;
std::vector<ID3D11Buffer*> arenaBufs;
size_t arenaChunk = 0, arenaHead = 0;
bool arena_alloc(size_t bytes, ID3D11Buffer*& buf, size_t& off) {
    if (bytes > kArenaChunk) return false;
    if (arenaBufs.empty() || arenaHead + bytes > kArenaChunk) {
        if (!arenaBufs.empty()) ++arenaChunk;
        arenaHead = 0;
        if (arenaChunk >= kArenaMaxChunks) {
            for (auto& [k, st] : streams) st.buf = nullptr, st.count = 0;
            arenaChunk = 0;
            RLOG("[rsx] vertex stream arena full: rebuilt\n");
        }
        while (arenaBufs.size() <= arenaChunk) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = UINT(kArenaChunk); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            ID3D11Buffer* b = nullptr;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return false;
            arenaBufs.push_back(b);
        }
    }
    buf = arenaBufs[arenaChunk]; off = arenaHead;
    arenaHead += (bytes + 255) & ~size_t(255);
    return true;
}
uint64_t stream_sig(const uint8_t* p, uint32_t stride, uint32_t count) {
    uint64_t h = fnv(&count, 4);
    const uint32_t step = std::max<uint32_t>(1, count / 24);
    for (uint32_t v = 0; v < count; v += step) h = fnv(p + size_t(v) * stride, std::min<uint32_t>(stride, 8), h);
    return fnv(p + size_t(count - 1) * stride, std::min<uint32_t>(stride, 8), h);
}
ID3D11Buffer* cached_stream(int a, uint32_t need, size_t& offset) {
    const uint32_t fmt = R[(0x1740 + 4 * a) >> 2], offReg = R[(0x1680 + 4 * a) >> 2];
    const uint32_t stride = (fmt >> 8) & 0xff;
    if ((offReg >> 31) || stride == 0 || need == 0 || need > 2000000) return nullptr;
    const uint32_t base = ea_of(offReg & 0x7fffffff, 0) + R[0x1738 >> 2];
    if (uint64_t(base) + uint64_t(need + need / 4 + 64) * stride >= 0xfff00000ull) return nullptr;
    Stream& st = streams[uint64_t(base) << 16 | (fmt & 0xffff)];
    const uint8_t* p = C.mem + base;
    if (st.count && st.checked != frameNo) {
        st.checked = frameNo;
        if (stream_sig(p, stride, st.count) != st.sig) st.count = 0;
    }
    if (st.count < need) {
        static std::vector<float> tmp;
        const uint32_t n = need + need / 4 + 64;
        tmp.resize(size_t(n) * 4);
        { TickScope t(g_convTicks); convert_attr(a, 0, n, tmp.data()); }
        TickScope bt(g_bufTicks);
        ID3D11Buffer* b = nullptr; size_t o = 0;
        if (!arena_alloc(tmp.size() * 4, b, o)) return nullptr;
        D3D11_BOX box{UINT(o), 0, 0, UINT(o + tmp.size() * 4), 1, 1};
        ctx->UpdateSubresource(b, 0, &box, tmp.data(), 0, 0);
        st.buf = b; st.offset = o;
        st.count = n; st.sig = stream_sig(p, stride, n); st.checked = frameNo;
        ++g_streamBuilds; g_streamVerts += n;
    }
    offset = st.offset;
    return st.buf;
}

// ------------------------------------------------------------------ textures
struct Tex { ID3D11Resource* res = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint64_t sig = 0, regSig = 0, checked = ~0ull; bool cube = false; uint32_t mips = 1, chans = 4; };
std::unordered_map<uint64_t, Tex> texCache;
struct TexFmt { DXGI_FORMAT f; uint32_t bpp; bool dxt; uint32_t blockBytes; int swap; uint32_t chans; };
bool tex_format(uint32_t f, TexFmt& o) {
    switch (f) {
        case 0x81: o = {DXGI_FORMAT_R8_UNORM, 1, false, 0, 0, 1}; return true;
        case 0x82: o = {DXGI_FORMAT_B5G5R5A1_UNORM, 2, false, 0, 2, 4}; return true;
        case 0x83: o = {DXGI_FORMAT_B4G4R4A4_UNORM, 2, false, 0, 2, 4}; return true;
        case 0x84: o = {DXGI_FORMAT_B5G6R5_UNORM, 2, false, 0, 2, 4}; return true;
        case 0x85: case 0x9e: o = {DXGI_FORMAT_B8G8R8A8_UNORM, 4, false, 0, 4, 4}; return true;   // A R G B in memory -> B G R A
        case 0x86: o = {DXGI_FORMAT_BC1_UNORM, 0, true, 8, 0, 4}; return true;
        case 0x87: o = {DXGI_FORMAT_BC2_UNORM, 0, true, 16, 0, 4}; return true;
        case 0x88: o = {DXGI_FORMAT_BC3_UNORM, 0, true, 16, 0, 4}; return true;
        case 0x8b: o = {DXGI_FORMAT_R8G8_UNORM, 2, false, 0, 0, 2}; return true;
        case 0x94: o = {DXGI_FORMAT_R16_UNORM, 2, false, 0, 2, 1}; return true;
        case 0x95: o = {DXGI_FORMAT_R16G16_UNORM, 4, false, 0, 2, 2}; return true;
        case 0x9a: o = {DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, 0, 2, 4}; return true;
        case 0x9b: o = {DXGI_FORMAT_R32G32B32A32_FLOAT, 16, false, 0, 4, 4}; return true;
        case 0x9c: o = {DXGI_FORMAT_R32_FLOAT, 4, false, 0, 4, 1}; return true;
        case 0x9f: o = {DXGI_FORMAT_R16G16_FLOAT, 4, false, 0, 2, 2}; return true;
        default: return false;
    }
}
// the unit's channel remap (what GL does with texture swizzle) as rows of a 4x4 matrix and an offset:
// output channel = dot(sample, row) + offset; sources: RSX A = 0, R = 1, G = 2, B = 3
void remap_matrix(uint32_t remap, uint32_t chans, float* m /*16*/, float* ofs /*4*/) {
    std::memset(m, 0, 16 * sizeof(float)); std::memset(ofs, 0, 4 * sizeof(float));
    static const int outOrder[4] = {1, 2, 3, 0};   // output R G B A <- RSX channel index
    for (int k = 0; k < 4; ++k) {
        const int rc = outOrder[k];
        const uint32_t sel = (remap >> (2 * rc)) & 3, mode = (remap >> (8 + 2 * rc)) & 3;
        if (mode == 0) continue;
        if (mode == 1) { ofs[k] = 1; continue; }
        int col = sel == 0 ? 3 : int(sel) - 1;   // A -> .a, R -> .r, G -> .g, B -> .b
        if (chans == 1) col = 0;
        else if (chans == 2) col = (sel == 1 || sel == 3) ? 1 : 0;
        m[4 * k + col] = 1;
    }
}
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct UnitBind { ID3D11ShaderResourceView* srv = nullptr; ID3D11SamplerState* smp = nullptr; float tsc[4] = {1, 1, 1, 1}; float m[16]; float o[4]; };
void bind_texture(int i, UnitBind& u) {
    const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
    const uint32_t off = R[base >> 2], fmtReg = R[(base + 4) >> 2], addr = R[(base + 8) >> 2];
    const uint32_t remap = R[(base + 0x10) >> 2], rect = R[(base + 0x18) >> 2], ctl3 = R[(0x1840 + 4 * i) >> 2];
    const uint32_t loc = (fmtReg & 3) == 2 ? 1 : 0;
    const bool cube = fmtReg & 4;
    const uint32_t f = (fmtReg >> 8) & 0xff, baseFmt = f & ~0x60u;
    const bool ln = f & 0x20, un = f & 0x40;
    const uint32_t mips = std::max<uint32_t>(1, fmtReg >> 16);
    const uint32_t w = std::max<uint32_t>(1, rect >> 16), h = std::max<uint32_t>(1, rect & 0xffff);
    const uint32_t pitch = ctl3 & 0xfffff;
    u.tsc[0] = un ? 1.f / w : 1.f; u.tsc[1] = un ? 1.f / h : 1.f; u.tsc[2] = float((addr >> 28) & 7); u.tsc[3] = 1.f;
    std::memcpy(u.m, kIdentity, sizeof u.m); std::memset(u.o, 0, sizeof u.o);
    // render target?
    for (int z = 0; z < 2; ++z) {
        auto it = surfaces.find((uint64_t(loc) << 33) | (uint64_t(z) << 32) | off);
        if (it != surfaces.end() && !cube) { u.srv = it->second.srv; u.smp = sampler_for(base, 1, true); return; }
    }
    if (cube) {   // cube map whose faces are render targets (the environment the cars reflect)
        const uint32_t faceStride = (w * h * 4 + 127) & ~127u;
        const uint64_t key0 = (uint64_t(loc) << 33) | off;
        auto f0 = surfaces.find(key0);
        if (f0 != surfaces.end()) {
            struct RenderedCube { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint64_t built = ~0ull; uint32_t w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; };
            static std::map<uint64_t, RenderedCube> rcubes;
            RenderedCube& rc = rcubes[key0];
            if (!rc.tex || rc.w != w || rc.h != h || rc.fmt != f0->second.dxgi) {
                release(rc.srv); release(rc.tex);
                D3D11_TEXTURE2D_DESC d{};
                d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 6; d.Format = f0->second.dxgi; d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
                if (FAILED(dev->CreateTexture2D(&d, nullptr, &rc.tex))) return;
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = d.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sv.TextureCube.MipLevels = 1;
                dev->CreateShaderResourceView(rc.tex, &sv, &rc.srv);
                rc.w = w; rc.h = h; rc.fmt = d.Format; rc.built = ~0ull;
            }
            if (rc.built != frameNo) {   // once per frame: the faces are drawn before the scene that uses them
                rc.built = frameNo;
                for (uint32_t k = 0; k < 6; ++k) {
                    auto it = surfaces.find(key0 + uint64_t(k) * faceStride);
                    if (it == surfaces.end() || it->second.dxgi != rc.fmt) continue;
                    D3D11_BOX box{0, 0, 0, std::min(w, it->second.w), std::min(h, it->second.h), 1};
                    ctx->CopySubresourceRegion(rc.tex, k, 0, 0, 0, it->second.tex, 0, &box);
                }
            }
            u.srv = rc.srv;
            static ID3D11SamplerState* cubeSmp = nullptr;
            if (!cubeSmp) {
                D3D11_SAMPLER_DESC d{};
                d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; d.MaxLOD = 0;
                dev->CreateSamplerState(&d, &cubeSmp);
            }
            u.smp = cubeSmp;
            return;
        }
    }
    TexFmt tf;
    if (!tex_format(baseFmt, tf)) {
        static std::map<uint32_t, int> warned;
        if (!warned[baseFmt]++) RLOG("[rsx] texture format 0x%x unsupported (unit %d)\n", baseFmt, i);
        return;
    }
    if (((fmtReg >> 4) & 0xf) == 3) return;   // 3D textures: not used by this game
    const uint32_t ea = ea_of(off, loc);
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
    const uint64_t key = (uint64_t(loc) << 32 | off) ^ (uint64_t(cube) << 63);
    Tex& tx = texCache[key];
    const uint64_t regSig = fnv(&fmtReg, 4) ^ (uint64_t(rect) << 20) ^ (uint64_t(pitch) << 3) ^ uint64_t(texture_epoch()) * 0x9E3779B97F4A7C15ull;
    uint64_t sig = tx.sig;
    if (!tx.srv || tx.regSig != regSig || frameNo - tx.checked >= 4 || tx.checked > frameNo) {
        sig = regSig;
        const uint32_t step = std::max<uint32_t>(64, total / 128);
        for (uint32_t k = 0; k + 8 <= total; k += step) sig = fnv(C.mem + ea + k, 8, sig);
        sig = fnv(C.mem + ea + total - std::min<uint32_t>(total, 8), std::min<uint32_t>(total, 8), sig);
        tx.checked = frameNo; tx.regSig = regSig;
    }
    if (!tx.srv || tx.sig != sig) {
        release(tx.srv); release(tx.res);
        tx.sig = sig; tx.cube = cube; tx.mips = mips; tx.chans = tf.chans;
        ++g_texUploads; g_texUploadBytes += total;
        TickScope texTime(g_texTicks);
        if (tf.dxt && ((w & 3) || (h & 3))) { static int warned = 0; if (warned++ < 5) RLOG("[d3d] compressed texture %ux%u not a multiple of 4: skipped\n", w, h); return; }
        std::vector<D3D11_SUBRESOURCE_DATA> init(size_t(faces) * mips);
        std::vector<std::vector<uint8_t>> bufs(size_t(faces) * mips);
        for (uint32_t face = 0; face < faces; ++face) {
            uint32_t p = ea + face * faceStride;
            for (uint32_t l = 0, lw = w, lh = h; l < mips; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) {
                const uint32_t lb = levelBytes(lw, lh);
                D3D11_SUBRESOURCE_DATA& sd = init[size_t(face) * mips + l];
                if (tf.dxt) { sd.pSysMem = C.mem + p; sd.SysMemPitch = std::max(1u, (lw + 3) / 4) * tf.blockBytes; }
                else {
                    const bool swz = !ln && (lw & (lw - 1)) == 0 && (lh & (lh - 1)) == 0;
                    if (!swz && tf.swap == 0) { sd.pSysMem = C.mem + p; sd.SysMemPitch = ln && pitch ? pitch : lw * tf.bpp; }
                    else {
                        std::vector<uint8_t>& buf = bufs[size_t(face) * mips + l];
                        buf.resize(size_t(lw) * lh * tf.bpp);
                        const uint32_t lwl = ilog2(lw), lhl = ilog2(lh);
                        for (uint32_t y = 0; y < lh; ++y)
                            for (uint32_t x = 0; x < lw; ++x) {
                                const uint32_t src = swz ? swizzle_index(x, y, lwl, lhl) * tf.bpp : (ln && pitch ? y * pitch + x * tf.bpp : (y * lw + x) * tf.bpp);
                                uint8_t* d = buf.data() + (size_t(y) * lw + x) * tf.bpp;
                                const uint8_t* s = C.mem + p + src;
                                if (tf.swap == 2) for (uint32_t k = 0; k < tf.bpp; k += 2) { d[k] = s[k + 1]; d[k + 1] = s[k]; }
                                else if (tf.swap == 4) for (uint32_t k = 0; k < tf.bpp; k += 4) { d[k] = s[k + 3]; d[k + 1] = s[k + 2]; d[k + 2] = s[k + 1]; d[k + 3] = s[k]; }
                                else std::memcpy(d, s, tf.bpp);
                            }
                        if (baseFmt == 0x9e) for (size_t k = 3; k < buf.size(); k += 4) buf[k] = 0xff;   // D8R8G8B8: alpha = 1
                        sd.pSysMem = buf.data(); sd.SysMemPitch = lw * tf.bpp;
                    }
                }
                p += lb;
            }
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = mips; d.ArraySize = faces; d.Format = tf.f; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ID3D11Texture2D* t2 = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, init.data(), &t2))) { static int warned = 0; if (warned++ < 10) RLOG("[d3d] texture %ux%u format 0x%x (%u levels) failed\n", w, h, baseFmt, mips); return; }
        tx.res = t2;
        dev->CreateShaderResourceView(t2, nullptr, &tx.srv);
    }
    u.srv = tx.srv;
    u.smp = sampler_for(base, mips, false);
    remap_matrix(remap, tx.chans, u.m, u.o);
}

// ------------------------------------------------------------------ input layouts
std::unordered_map<uint64_t, ID3D11InputLayout*> layouts;
ID3D11InputLayout* layout_for(Program* p, const uint32_t* comps /*16, 0 = unused*/) {
    uint64_t h = fnv(comps, 16 * 4, reinterpret_cast<uintptr_t>(p->vs));
    ID3D11InputLayout*& l = layouts[h];
    if (l) return l;
    D3D11_INPUT_ELEMENT_DESC e[16]; UINT n = 0;
    static const DXGI_FORMAT ff[5] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    for (int a = 0; a < 16; ++a) {
        if (!(p->vpInputs & (1u << a)) || !comps[a]) continue;
        e[n++] = D3D11_INPUT_ELEMENT_DESC{"ATTR", UINT(a), ff[comps[a]], UINT(a), 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    if (FAILED(dev->CreateInputLayout(e, n, p->vsCode->GetBufferPointer(), p->vsCode->GetBufferSize(), &l))) RLOG("[d3d] input layout failed\n");
    return l;
}

// ------------------------------------------------------------------ draws
struct DrawCb { float vs[4], vo[4], surf[4], alpha[4], wpos[4], tsc[16][4], rm[64][4], ro[16][4]; };
std::vector<uint32_t> idx;
std::vector<float> vtx;

void do_draw() {
    if (ranges.empty() || !devOk) return;
    struct SlowDraw { uint64_t t0 = SDL_GetPerformanceCounter(); ~SlowDraw() { const uint64_t dt = SDL_GetPerformanceCounter() - t0; g_drawTicks += dt; ++g_drawCount; } } slowDraw;
    ++drawsThisFrame;
    if (!bind_target()) return;
    D3D11_RECT sr; scissor_rect(sr);
    ctx->RSSetScissorRects(1, &sr);
    const uint32_t sp = R[0x8e4 >> 2];
    const uint32_t fpEa = ea_of(sp & ~3u, (sp & 3) == 2 ? 1 : 0);
    uint32_t texDims = 0;
    for (int i = 0; i < 16; ++i) texDims |= texDimsOf(i) << (2 * i);
    Program* p = get_program(R[0x1ea0 >> 2] & 511, fpEa, texDims, R[0x1d60 >> 2]);
// Only draws into the full-screen image may be skipped while their shaders are being built (it is drawn again
// next frame); a draw into anything else (paint layers composed once when a race loads, shadow maps, cube faces)
// waits: skipped, it would leave that texture wrong for good (the player's car came out black).
    if (p && p->state.load() == 0 && !(cur.logW == 1280 && cur.logH == 720)) while (p->state.load(std::memory_order_acquire) == 0) SDL_Delay(1);
    if (!p || p->bad()) { if (p && p->state.load() == 0) ++skippedDraws; return; }
    if (p->vs != lastVs) { ctx->VSSetShader(p->vs, nullptr, 0); lastVs = p->vs; }
    if (p->ps != lastPs) { ctx->PSSetShader(p->ps, nullptr, 0); lastPs = p->ps; }
    apply_state();
    if (vpConstDirty) { write_cb(cbVc, vpConst, sizeof vpConst); vpConstDirty = false; vpDirtyLo = 468 * 4; vpDirtyHi = 0; }
    static DrawCb dc;
    const float vs[4] = {rf(0xa30), rf(0xa34), rf(0xa38), rf(0xa3c)}, vo[4] = {rf(0xa20), rf(0xa24), rf(0xa28), rf(0xa2c)};
    std::memcpy(dc.vs, vs, 16); std::memcpy(dc.vo, vo, 16);
    dc.surf[0] = float(cur.logW); dc.surf[1] = float(cur.logH); dc.surf[2] = dc.surf[3] = 0;
    const uint32_t af = R[0x308 >> 2];
    dc.alpha[0] = float(R[0x304 >> 2] & 1); dc.alpha[1] = float(af >= 0x200 ? af - 0x200 : 7); dc.alpha[2] = (R[0x30c >> 2] & 0xff) / 255.f; dc.alpha[3] = 0;
    dc.wpos[0] = 1.f / float(cur.aaX); dc.wpos[1] = 1.f / float(cur.aaY); dc.wpos[2] = dc.wpos[3] = 0;
    {   // textures
        ID3D11ShaderResourceView* srv[16]; ID3D11SamplerState* smp[16];
        for (int i = 0; i < 16; ++i) {
            UnitBind u;
            if (p->texUsed & (1u << i)) bind_texture(i, u);
            else std::memcpy(u.m, kIdentity, sizeof u.m), std::memset(u.o, 0, sizeof u.o);
            srv[i] = u.srv; smp[i] = u.smp ? u.smp : linearClamp;
            std::memcpy(dc.tsc[i], u.tsc, 16);
            for (int r = 0; r < 4; ++r) std::memcpy(dc.rm[4 * i + r], u.m + 4 * r, 16);
            std::memcpy(dc.ro[i], u.o, 16);
        }
        int lo = 16, hi = -1;
        for (int i = 0; i < 16; ++i) if (srv[i] != boundSrv[i] || smp[i] != boundSmp[i]) { lo = std::min(lo, i); hi = i; }
        if (hi >= lo) {
            ctx->PSSetShaderResources(UINT(lo), UINT(hi - lo + 1), srv + lo);
            ctx->PSSetSamplers(UINT(lo), UINT(hi - lo + 1), smp + lo);
            std::memcpy(boundSrv, srv, sizeof srv); std::memcpy(boundSmp, smp, sizeof smp);
        }
    }
    {   // most draws repeat the previous values: rewriting the buffer only on change saves a map per draw
        static DrawCb last; static bool have = false;
        if (!have || std::memcmp(&last, &dc, sizeof dc) != 0) { write_cb(cbDc, &dc, sizeof dc); last = dc; have = true; }
    }
    if (p->nConst && !fpConsts.empty()) {
        const size_t bytes = fpConsts.size() * 4;
        if (bytes > cbFcSize) { release(cbFc); cbFcSize = std::max<size_t>(bytes, 4096); cbFc = make_cb(cbFcSize); ctx->PSSetConstantBuffers(2, 1, &cbFc); }
        static std::vector<float> lastFc; static ID3D11Buffer* lastFcBuf = nullptr;
        if (lastFcBuf != cbFc || lastFc != fpConsts) { write_cb(cbFc, fpConsts.data(), bytes); lastFc = fpConsts; lastFcBuf = cbFc; }
    }
    // indices
    uint32_t vmin, vmax;
    gather_draw_indices(idx, vmin, vmax, true);
    if (vmin > vmax || idx.empty()) return;
    static std::vector<uint32_t> uniq, vmap;
    const uint32_t span = vmax - vmin + 1;
    bool allCacheable = true, mainFloats = false;
    for (int a = 0; a < 16 && allCacheable; ++a) {
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        if (!((fmt >> 4) & 0xf)) continue;
        if (!((fmt >> 8) & 0xff)) allCacheable = false;
        else if (R[(0x1680 + 4 * a) >> 2] >> 31) { if ((fmt & 0xf) == 2) mainFloats = true; else allCacheable = false; }
    }
    const bool dense = allCacheable && vmax < 1000000 && (!mainFloats || span <= idx.size() * 12);
    const bool sparse = rangesIndexed && span > 64 && span > idx.size() * 2 && !dense;
    if (sparse) {
        uniq.clear();
        for (uint32_t v : idx) if (v != 0xffffffffu) uniq.push_back(v);
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    }
    const uint32_t nv = sparse ? uint32_t(uniq.size()) : span;
    if (nv > 4000000) return;
    // vertex data: cached streams, or converted into the ring (constant attributes: 16 bytes with stride 0)
    uint32_t comps[16] = {};
    ID3D11Buffer* vb[16] = {}; UINT strides[16] = {}, offs[16] = {};
    long slot[16]; size_t vtxFloats = 0;
    float constAttr[16][4]; int nConstAttr = 0; int constOf[16];
    for (int a = 0; a < 16; ++a) {
        slot[a] = -2; constOf[a] = -1;
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        if (!((fmt >> 4) & 0xf)) {   // no array: the attribute's constant value
            constAttr[nConstAttr][0] = rf(0x1c00 + 16 * a); constAttr[nConstAttr][1] = rf(0x1c04 + 16 * a); constAttr[nConstAttr][2] = rf(0x1c08 + 16 * a); constAttr[nConstAttr][3] = rf(0x1c0c + 16 * a);
            constOf[a] = nConstAttr++; comps[a] = 4;
            continue;
        }
        size_t so = 0;
        if (!sparse) if (ID3D11Buffer* b = cached_stream(a, vmax + 1, so)) { vb[a] = b; strides[a] = 16; offs[a] = UINT(so + size_t(vmin) * 16); comps[a] = 4; slot[a] = -1; continue; }
        comps[a] = ((fmt >> 8) & 0xff) ? attr_comps(a) : 4;
        slot[a] = long(vtxFloats);
        vtxFloats += size_t(nv) * comps[a];
    }
    const size_t vbytes = vtxFloats * 4 + size_t(nConstAttr) * 16;
    if (vbytes) {
        size_t vOff = 0;
        float* m = reinterpret_cast<float*>(ring_map(vring, vbytes, 16, vOff));
        if (!m) return;
        for (int a = 0; a < 16; ++a) {
            if (slot[a] < 0) continue;
            const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
            float* dst = m + slot[a];
            const uint32_t cc = comps[a];
            if (((fmt >> 8) & 0xff) == 0) { float c4[4]; convert_attr(a, 0, 1, c4); for (uint32_t v = 0; v < nv; ++v) std::memcpy(dst + v * 4, c4, 16); }
            else if (!sparse) convert_attr(a, vmin, nv, dst, cc);
            else
                for (uint32_t i = 0; i < nv;) {
                    uint32_t j = i + 1;
                    while (j < nv && uniq[j] == uniq[j - 1] + 1) ++j;
                    convert_attr(a, uniq[i], j - i, dst + size_t(i) * cc, cc);
                    i = j;
                }
            vb[a] = vring.buf; strides[a] = 4 * cc; offs[a] = UINT(vOff + size_t(slot[a]) * 4);
        }
        float* cst = m + vtxFloats;
        for (int a = 0; a < 16; ++a) if (constOf[a] >= 0) { std::memcpy(cst + 4 * constOf[a], constAttr[constOf[a]], 16); vb[a] = vring.buf; strides[a] = 0; offs[a] = UINT(vOff + vtxFloats * 4 + size_t(constOf[a]) * 16); }
        ring_done(vring);
    }
    if (sparse) {
        if (vmap.size() < span) vmap.resize(span);
        for (uint32_t i = 0; i < nv; ++i) vmap[uniq[i] - vmin] = i;
        for (uint32_t& v : idx) if (v != 0xffffffffu) v = vmap[v - vmin];
    }
    ctx->IASetInputLayout(layout_for(p, comps));
    ctx->IASetVertexBuffers(0, 16, vb, strides, offs);
    size_t iOff = 0;
    uint8_t* ip = ring_map(iring, idx.size() * 4, 4, iOff);
    if (!ip) return;
    std::memcpy(ip, idx.data(), idx.size() * 4);
    ring_done(iring);
    D3D11_PRIMITIVE_TOPOLOGY topo;
    switch (prim) { case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break; case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
                    case 3: case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break; case 6: case 9: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
                    default: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break; }
    if (topo != lastTopo) { ctx->IASetPrimitiveTopology(topo); lastTopo = topo; }
    ctx->DrawIndexed(UINT(idx.size()), UINT(iOff / 4), sparse ? 0 : -INT(vmin));
}

// ------------------------------------------------------------------ clears
// A clear covering the whole target with all channels is a D3D clear; anything scissored or masked draws a quad
// (D3D clears ignore the scissor and the colour mask, GL's and the RSX's do not).
ID3D11VertexShader* clearVs = nullptr; ID3D11PixelShader* clearPs = nullptr;
ID3D11Buffer* cbClear = nullptr;
void clear_surface(uint32_t mask) {
    if (!bind_target()) return;
    const uint32_t zfmt = (R[0x208 >> 2] >> 5) & 7;
    const uint32_t c = R[0x1d90 >> 2], zv = R[0x1d8c >> 2];
    const float col[4] = {((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, (c >> 24) / 255.f};
    const float depth = zfmt == 1 ? float((zv & 0xffff) / 65535.0) : float((zv >> 8) / 16777215.0);
    const bool doColor = (mask & 0xf0) && cur.nColor, doDepth = (mask & 1) && cur.dsv, doStencil = (mask & 2) && zfmt == 2 && cur.dsv;
    if (!doColor && !doDepth && !doStencil) return;
    D3D11_RECT sr; scissor_rect(sr);
    const bool full = sr.left <= 0 && sr.top <= 0 && sr.right >= LONG(cur.w) && sr.bottom >= LONG(cur.h);
    if (full && (!doColor || (mask & 0xf0) == 0xf0)) {
        if (doColor) for (int k = 0; k < cur.nColor; ++k) ctx->ClearRenderTargetView(cur.rtv[k], col);
        if (doDepth || doStencil) ctx->ClearDepthStencilView(cur.dsv, (doDepth ? D3D11_CLEAR_DEPTH : 0) | (doStencil ? D3D11_CLEAR_STENCIL : 0), depth, UINT8(zv & 0xff));
        return;
    }
    // quad: blend off with the clear's channel mask, depth ALWAYS (written if cleared), stencil REPLACE
    const uint32_t cm = doColor ? ((mask >> 5) & 1) | (((mask >> 6) & 1) << 1) | (((mask >> 7) & 1) << 2) | (((mask >> 4) & 1) << 3) : 0;
    static std::map<uint32_t, ID3D11BlendState*> bs;
    static std::map<uint32_t, ID3D11DepthStencilState*> ds;
    static ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState*& b = bs[cm];
    if (!b) { D3D11_BLEND_DESC d{}; for (auto& rt : d.RenderTarget) { rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask = UINT8(cm); } d.IndependentBlendEnable = FALSE; dev->CreateBlendState(&d, &b); }
    const uint32_t dk = uint32_t(doDepth) | uint32_t(doStencil) << 1;
    ID3D11DepthStencilState*& d2 = ds[dk];
    if (!d2) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = doDepth; d.DepthWriteMask = doDepth ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        d.StencilEnable = doStencil; d.StencilReadMask = 0xff; d.StencilWriteMask = 0xff;
        D3D11_DEPTH_STENCILOP_DESC o{D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
        d.FrontFace = o; d.BackFace = o;
        dev->CreateDepthStencilState(&d, &d2);
    }
    if (!rs) { D3D11_RASTERIZER_DESC d{}; d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE; d.DepthClipEnable = TRUE; d.ScissorEnable = TRUE; dev->CreateRasterizerState(&d, &rs); }
    const float cb[8] = {col[0], col[1], col[2], col[3], depth, 0, 0, 0};
    write_cb(cbClear, cb, sizeof cb);
    ctx->OMSetBlendState(b, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(d2, zv & 0xff);
    ctx->RSSetState(rs);
    ctx->RSSetScissorRects(1, &sr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(clearVs, nullptr, 0); ctx->PSSetShader(clearPs, nullptr, 0);
    ctx->VSSetConstantBuffers(3, 1, &cbClear); ctx->PSSetConstantBuffers(3, 1, &cbClear);
    ctx->Draw(3, 0);
    forget_pipeline();   // the draw path sets its shaders and topology again
}

// ------------------------------------------------------------------ present
ID3D11VertexShader* fsVs = nullptr;
ID3D11PixelShader* blitPs = nullptr; ID3D11PixelShader* overPs = nullptr; ID3D11PixelShader* fxaaPs = nullptr; ID3D11PixelShader* sharpPs = nullptr;
ID3D11Buffer* cbPost = nullptr;
ID3D11RasterizerState* postRs = nullptr; ID3D11BlendState* postBs = nullptr; ID3D11DepthStencilState* postDs = nullptr;
ID3D11BlendState* overBs = nullptr;   // the overlays: premultiplied alpha over the image
ID3D11Texture2D* midTex = nullptr; ID3D11RenderTargetView* midRtv = nullptr; ID3D11ShaderResourceView* midSrv = nullptr; int midW = 0, midH = 0;
ID3D11Texture2D* ovlTex[2] = {}; ID3D11ShaderResourceView* ovlSrv[2] = {}; int ovlW[2] = {}, ovlH[2] = {};

const char* const kFsVs =
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "cbuffer CL : register(b3) { float4 clearColor; float4 clearDepth; };\n"
    "VO main(uint id : SV_VertexID) { VO o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, clearDepth.x, 1.0); return o; }\n";
const char* const kClearPs =
    "cbuffer CL : register(b3) { float4 clearColor; float4 clearDepth; };\n"
    "struct PO { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; };\n"
    "PO main() { PO o; o.c0 = clearColor; o.c1 = clearColor; o.c2 = clearColor; o.c3 = clearColor; return o; }\n";
const char* const kBlitPs =
    "Texture2D src : register(t0); SamplerState smp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(src.Sample(smp, uv).rgb, 1.0); }\n";
const char* const kOverPs =   // the overlays: premultiplied alpha kept
    "Texture2D src : register(t0); SamplerState smp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return src.Sample(smp, uv); }\n";
const char* const kFxaaPs =
    "Texture2D src : register(t0); SamplerState smp : register(s0);\n"
    "cbuffer P : register(b0) { float4 u_p; };\n"
    "float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float2 rcp = u_p.xy;\n"
    "  float3 m = src.Sample(smp, uv).rgb;\n"
    "  float lM = lum(m);\n"
    "  float lNW = lum(src.Sample(smp, uv + float2(-1.0, 1.0) * rcp).rgb), lNE = lum(src.Sample(smp, uv + float2(1.0, 1.0) * rcp).rgb);\n"
    "  float lSW = lum(src.Sample(smp, uv + float2(-1.0, -1.0) * rcp).rgb), lSE = lum(src.Sample(smp, uv + float2(1.0, -1.0) * rcp).rgb);\n"
    "  float mn = min(lM, min(min(lNW, lNE), min(lSW, lSE))), mx = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  if (mx - mn < max(0.0312, mx * 0.125)) return float4(m, 1.0);\n"
    "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), float2(-8.0, -8.0), float2(8.0, 8.0)) * rcp;\n"
    "  float3 a = 0.5 * (src.Sample(smp, uv + dir * (1.0 / 3.0 - 0.5)).rgb + src.Sample(smp, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 b = a * 0.5 + 0.25 * (src.Sample(smp, uv - dir * 0.5).rgb + src.Sample(smp, uv + dir * 0.5).rgb);\n"
    "  float lB = lum(b);\n"
    "  return float4((lB < mn || lB > mx) ? a : b, 1.0);\n"
    "}\n";
const char* const kSharpPs =
    "Texture2D src : register(t0); SamplerState smp : register(s0);\n"
    "cbuffer P : register(b0) { float4 u_p; };\n"
    "float4 main(float4 pos : SV_Position, float2 u : TEXCOORD0) : SV_Target {\n"
    "  float2 rcp = u_p.xy;\n"
    "  float3 c = src.Sample(smp, u).rgb;\n"
    "  if (u_p.z <= 0.0) return float4(c, 1.0);\n"
    "  float3 n = src.Sample(smp, u + float2(0.0, rcp.y)).rgb, s = src.Sample(smp, u - float2(0.0, rcp.y)).rgb;\n"
    "  float3 e = src.Sample(smp, u + float2(rcp.x, 0.0)).rgb, w = src.Sample(smp, u - float2(rcp.x, 0.0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));\n"
    "  float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, u_p.z));\n"
    "  return float4(saturate((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt)), 1.0);\n"
    "}\n";

ID3D11VertexShader* make_vs(const char* src) {
    ID3DBlob* b = compile_hlsl(src, "vs_5_0", "fixed vertex"); ID3D11VertexShader* sh = nullptr;
    if (b) { dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sh); b->Release(); }
    return sh;
}
ID3D11PixelShader* make_ps(const char* src) {
    ID3DBlob* b = compile_hlsl(src, "ps_5_0", "fixed pixel"); ID3D11PixelShader* sh = nullptr;
    if (b) { dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sh); b->Release(); }
    return sh;
}

void resize_back(int w, int h) {
    if (w == backW && h == backH && backRtv) return;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    release(backRtv);
    cur = Target{};
    swap->ResizeBuffers(0, UINT(std::max(1, w)), UINT(std::max(1, h)), DXGI_FORMAT_UNKNOWN, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) { dev->CreateRenderTargetView(bb, nullptr, &backRtv); bb->Release(); }
    backW = w; backH = h;
}

// a full-screen triangle with pixel shader ps reading srv, into rtv over the viewport (x, y, w, h)
void post_pass(ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, float x, float y, float w, float h, const float* params) {
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    if (params) write_cb(cbPost, params, 16);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &linearClamp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

void save_display_shot(const Surface& s, uint64_t offset) {
    D3D11_TEXTURE2D_DESC d{};
    s.tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopyResource(st, s.tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        if (SDL_Surface* srf = SDL_CreateSurfaceFrom(int(d.Width), int(d.Height), SDL_PIXELFORMAT_ABGR8888, m.pData, int(m.RowPitch))) {
            char fn[96];
            std::snprintf(fn, sizeof fn, "rsxshot_%05llu_DISPLAY_%x.bmp", (unsigned long long)frameNo, uint32_t(offset));
            SDL_SaveBMP(srf, fn); SDL_DestroySurface(srf);
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
}

void present_d3d(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) {
    resize_back(ww, wh);
    if (!backRtv) return;
    unbind_textures();
    cur = Target{};   // the targets below replace the game's
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(backRtv, black);
    ctx->OMSetBlendState(postBs, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(postDs, 0);
    ctx->RSSetState(postRs);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(fsVs, nullptr, 0);
    const float noDepth[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    write_cb(cbClear, noDepth, sizeof noDepth);
    ctx->VSSetConstantBuffers(3, 1, &cbClear);
    ctx->PSSetConstantBuffers(0, 1, &cbPost);
    auto it = surfaces.find(uint64_t(d.offset));
    if (it != surfaces.end() && it->second.srv) {
        const Surface& s = it->second;
        const float sa = float(d.w ? d.w : s.w) / float(d.h ? d.h : s.h), wa = float(ww) / float(wh);
        int dw = ww, dh = wh;
        if (wa > sa) dw = int(wh * sa); else dh = int(ww / sa);
        const int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
        const Settings& st = settings();
        ID3D11ShaderResourceView* img = s.srv;
        if (st.fxaa && fxaaPs) {
            if (!midTex || midW != int(s.w) || midH != int(s.h)) {
                release(midRtv); release(midSrv); release(midTex);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = s.w; td.Height = s.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &midTex))) { dev->CreateRenderTargetView(midTex, nullptr, &midRtv); dev->CreateShaderResourceView(midTex, nullptr, &midSrv); }
                midW = int(s.w); midH = int(s.h);
            }
            if (midRtv) {
                const float pp[4] = {1.f / float(s.w), 1.f / float(s.h), 0, 0};
                post_pass(midRtv, fxaaPs, s.srv, 0, 0, float(s.w), float(s.h), pp);
                img = midSrv;
            }
        }
        static const float amount[4] = {0.f, 0.3f, 0.6f, 1.f};
        const float pp[4] = {1.f / float(s.w), 1.f / float(s.h), (st.sharpen && sharpPs) ? amount[std::clamp(st.sharpen, 0, 3)] : 0.f, 0};
        post_pass(backRtv, sharpPs ? sharpPs : blitPs, img, float(dx), float(dy), float(dw), float(dh), pp);
        static const int shotEvery = std::getenv("RSXSHOT") ? std::atoi(std::getenv("RSXSHOT")) : 0;
        if (shotEvery > 0 && frameNo % uint64_t(shotEvery) == 0) { RLOG("[shot] frame %llu at %.2f s\n", (unsigned long long)frameNo, SDL_GetTicks() / 1000.0); save_display_shot(s, d.offset); }
    } else {
        static int warned = 0;
        if (warned++ < 5) RLOG("[rsx] flip: display buffer at 0x%x was never rendered\n", d.offset);
    }
    for (int k = 0; k < nOvl && k < 2; ++k) {
        const Overlay& o = ovl[k];
        if (!o.px) continue;
        if (!ovlTex[k] || ovlW[k] != o.w || ovlH[k] != o.h) {
            release(ovlSrv[k]); release(ovlTex[k]);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(o.w); td.Height = UINT(o.h); td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &ovlTex[k]))) continue;
            dev->CreateShaderResourceView(ovlTex[k], nullptr, &ovlSrv[k]);
            ovlW[k] = o.w; ovlH[k] = o.h;
        }
        static bool uploaded[2] = {};
        if (o.changed || !uploaded[k]) { ctx->UpdateSubresource(ovlTex[k], 0, nullptr, o.px, UINT(o.w * 4), 0); uploaded[k] = true; }
        ctx->OMSetBlendState(overBs ? overBs : postBs, nullptr, 0xffffffffu);
        post_pass(backRtv, overPs ? overPs : blitPs, ovlSrv[k], float(o.dx), float(o.dy), float(o.dw), float(o.dh), nullptr);
        ctx->OMSetBlendState(postBs, nullptr, 0xffffffffu);
    }
    {   // D3DWINSHOT=seconds[,seconds...]: the final window image (game + menu + notification) as d3dwin_<s>.bmp (debug)
        static std::vector<double> at; static bool parsed = false;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("D3DWINSHOT")) for (const char* q = e; *q;) { at.push_back(std::atof(q)); while (*q && *q != ',') ++q; if (*q) ++q; } }
        if (!at.empty() && SDL_GetTicks() >= at.front() * 1000) {
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
                D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                ID3D11Texture2D* st = nullptr;
                if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st))) {
                    ctx->CopyResource(st, bb);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                        if (SDL_Surface* srf = SDL_CreateSurfaceFrom(int(d.Width), int(d.Height), SDL_PIXELFORMAT_ARGB8888, m.pData, int(m.RowPitch))) {
                            char fn[64]; std::snprintf(fn, sizeof fn, "d3dwin_%03d.bmp", int(at.front()));
                            SDL_SaveBMP(srf, fn); SDL_DestroySurface(srf);
                        }
                        ctx->Unmap(st, 0);
                    }
                    st->Release();
                }
                bb->Release();
            }
            at.erase(at.begin());
        }
    }
    const uint64_t ts = SDL_GetTicksNS();
    // without V-Sync the image still goes through the compositor at the next refresh (no tearing), like OpenGL in a
    // window; D3DTEARING=1 shows it at once (tearing) where the system allows it
    static const bool allowTear = std::getenv("D3DTEARING") != nullptr;
    const UINT flags = (!vsyncOn && tearing && allowTear) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap->Present(vsyncOn ? 1 : 0, flags);
    g_swapUs += (SDL_GetTicksNS() - ts) / 1000;
    {   // D3DDEBUG=1: the debug layer's messages into the log (each distinct one once)
        static ID3D11InfoQueue* iq = nullptr; static bool tried = false;
        if (!tried) { tried = true; if (std::getenv("D3DDEBUG")) dev->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&iq)); }
        if (iq) {
            static std::unordered_map<uint64_t, int> seen;
            const UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 k = 0; k < n; ++k) {
                SIZE_T len = 0;
                if (FAILED(iq->GetMessage(k, nullptr, &len)) || !len) continue;
                std::vector<uint8_t> buf(len);
                D3D11_MESSAGE* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                if (FAILED(iq->GetMessage(k, msg, &len))) continue;
                if (msg->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;
                if (seen[uint64_t(msg->ID)]++ == 0) RLOG("[d3ddebug] %s (frame %llu)\n", msg->pDescription, (unsigned long long)frameNo);
            }
            iq->ClearStoredMessages();
        }
    }
    // everything the draw path caches as "already set" is stale now
    forget_pipeline();
    ID3D11Buffer* vcb[2] = {cbVc, cbDc};
    ctx->VSSetConstantBuffers(0, 2, vcb);
    ID3D11Buffer* pcb[3] = {nullptr, cbDc, cbFc};
    ctx->PSSetConstantBuffers(0, 3, pcb);
    std::memset(boundSmp, 0, sizeof boundSmp);
}

bool init_d3d() {
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) { RLOG("[d3d] no window handle\n"); return false; }
    if (HMODULE m = LoadLibraryA("d3dcompiler_47.dll")) compileFn = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")));
    if (HMODULE m = GetModuleHandleA("d3dcompiler_47.dll")) createBlobFn = reinterpret_cast<PFN_CreateBlob>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCreateBlob")));
    if (!compileFn) { RLOG("[d3d] d3dcompiler_47.dll not found\n"); return false; }
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (std::getenv("D3DDEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    // GPU=text (test): the graphics card whose name contains that text (e.g. GPU=AMD for the processor's own graphics)
    IDXGIAdapter1* pick = nullptr;
    if (const char* want = std::getenv("GPU")) {
        using CreateF = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dx = LoadLibraryA("dxgi.dll");
        auto create = dx ? reinterpret_cast<CreateF>(reinterpret_cast<void*>(GetProcAddress(dx, "CreateDXGIFactory1"))) : nullptr;
        IDXGIFactory1* f1 = nullptr;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1)))) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; !pick && f1->EnumAdapters1(i, &a) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
                char n[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n, nullptr, nullptr);
                std::string ln(n), lw(want);
                for (auto& ch : ln) ch = char(std::tolower(uint8_t(ch)));
                for (auto& ch : lw) ch = char(std::tolower(uint8_t(ch)));
                if (ln.find(lw) != std::string::npos) pick = a; else a->Release();
            }
            f1->Release();
        }
        RLOG("[d3d] GPU=%s: %s\n", want, pick ? "found" : "not found, using the default one");
    }
    const D3D_DRIVER_TYPE dtype = pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(pick, dtype, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (hr == E_INVALIDARG) hr = D3D11CreateDevice(pick, dtype, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {   // the debug layer is an optional Windows component ("Graphics Tools")
        RLOG("[d3d] debug layer not installed: running without it\n");
        flags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (hr == E_INVALIDARG) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    if (FAILED(hr)) { RLOG("[d3d] D3D11CreateDevice failed 0x%08lx\n", static_cast<unsigned long>(hr)); return false; }
    IDXGIDevice* dxd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fac = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxd));
    if (dxd) dxd->GetAdapter(&ad);
    if (ad) {
        DXGI_ADAPTER_DESC ds{};
        if (SUCCEEDED(ad->GetDesc(&ds))) WideCharToMultiByte(CP_UTF8, 0, ds.Description, -1, gpuName, sizeof gpuName, nullptr, nullptr);
        ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac));
    }
    if (!fac) { RLOG("[d3d] no DXGI factory\n"); release(ad); release(dxd); return false; }
    {
        IDXGIFactory5* f5 = nullptr;
        BOOL allow = FALSE;
        if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearing = allow;
            f5->Release();
        }
    }
    int ww = 1280, wh = 720;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(ww); sd.Height = UINT(wh); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &swap);
    if (SUCCEEDED(hr)) fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    release(fac); release(ad); release(dxd);
    if (FAILED(hr)) { RLOG("[d3d] swap chain failed 0x%08lx\n", static_cast<unsigned long>(hr)); return false; }
    resize_back(ww, wh);
    vsyncOn = settings().vsync != 0;
    if (!make_ring(vring, size_t(64) << 20, D3D11_BIND_VERTEX_BUFFER) || !make_ring(iring, size_t(16) << 20, D3D11_BIND_INDEX_BUFFER)) { RLOG("[d3d] buffers failed\n"); return false; }
    ctx->IASetIndexBuffer(iring.buf, DXGI_FORMAT_R32_UINT, 0);
    cbVc = make_cb(sizeof vpConst); cbDc = make_cb(sizeof(DrawCb)); cbClear = make_cb(32); cbPost = make_cb(16);
    cbFcSize = 4096; cbFc = make_cb(cbFcSize);
    {
        D3D11_SAMPLER_DESC d{};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; d.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&d, &linearClamp);
        D3D11_RASTERIZER_DESC r{}; r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&r, &postRs);
        D3D11_BLEND_DESC b{}; b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&b, &postBs);
        b.RenderTarget[0].BlendEnable = TRUE; b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        dev->CreateBlendState(&b, &overBs);
        D3D11_DEPTH_STENCIL_DESC z{}; z.DepthEnable = FALSE; z.StencilEnable = FALSE;
        dev->CreateDepthStencilState(&z, &postDs);
    }
    SDL_CreateDirectory("d3dcache");
    fsVs = make_vs(kFsVs);
    clearVs = fsVs;
    clearPs = make_ps(kClearPs);
    blitPs = make_ps(kBlitPs);
    overPs = make_ps(kOverPs);
    fxaaPs = make_ps(kFxaaPs);
    sharpPs = make_ps(kSharpPs);
    if (!fsVs || !clearPs || !blitPs) { RLOG("[d3d] fixed shaders failed\n"); return false; }
    ID3D11Buffer* vcb[2] = {cbVc, cbDc};
    ctx->VSSetConstantBuffers(0, 2, vcb);
    ID3D11Buffer* pcb[3] = {nullptr, cbDc, cbFc};
    ctx->PSSetConstantBuffers(0, 3, pcb);
    if (dumpShaders) SDL_CreateDirectory("rsxdump");
    RLOG("[rsx] Direct3D 11 (feature level %x) on %s%s\n", unsigned(got), gpuName, tearing ? ", tearing allowed" : "");
    devOk = true;
    return true;
}

} // namespace

struct D3d11Backend final : Backend {
    uint32_t window_flags() override { return 0; }
    bool init() override { return init_d3d(); }
    void new_frame() override {}
    void clear(uint32_t mask) override { clear_surface(mask); }
    void draw() override { do_draw(); }
    void present(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) override { present_d3d(d, ww, wh, ovl, nOvl); }
    void set_vsync(bool on) override { vsyncOn = on; }
    void textures_changed() override {}
    const char* gpu_name() override { return gpuName; }
    const char* api_name() override { return "DirectX 11"; }
};
Backend* make_d3d11_backend() { static D3d11Backend b; return &b; }

} // namespace gti::recomp::rsx

#else
namespace gti::recomp::rsx {
struct NoD3d final : Backend {
    uint32_t window_flags() override { return 0; }
    bool init() override { return false; }
    void new_frame() override {}
    void clear(uint32_t) override {}
    void draw() override {}
    void present(const Display&, int, int, const Overlay*, int) override {}
    void set_vsync(bool) override {}
    void textures_changed() override {}
    const char* gpu_name() override { return "none"; }
    const char* api_name() override { return "none"; }
};
Backend* make_d3d11_backend() { static NoD3d b; return &b; }
} // namespace gti::recomp::rsx
#endif
