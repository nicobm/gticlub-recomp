// RSX -> Vulkan 1.3 back end (see rsx_core.h). Same model as the OpenGL and Direct3D 11 back ends, built on what
// Vulkan 1.3 has in core: dynamic rendering (no render passes or framebuffers) and extended dynamic state (cull,
// winding, depth, stencil, depth bias, topology, restart, vertex strides change without new pipelines). A pipeline
// only depends on the program, the blend state, the formats of the targets and of the vertex attributes; new ones
// are built on worker threads (their draws are skipped meanwhile) and kept in a pipeline cache on disk (vkcache/).
// Shaders: the GLSL body the front end translates, with Vulkan-style declarations, compiled to SPIR-V by glslang.
// The RSX channel remap is done by the image views (component swizzle), like GL's texture swizzle.
// Surfaces (render targets) live in the GENERAL layout; a full memory barrier is recorded whenever the target
// changes, so a surface drawn in one pass can be sampled in the next (the game never samples the target it draws).
#include "recomp/rsx_core.h"
#include "recomp/settings.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gti::recomp::rsx {
namespace {

// ------------------------------------------------------------------ functions
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE(X) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateGraphicsPipelines) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) \
    X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearAttachments) X(vkCmdCopyBuffer)
#define VK_DECL(name) PFN_##name name = nullptr;
VK_GLOBAL(VK_DECL) VK_INSTANCE(VK_DECL) VK_DEVICE(VK_DECL)
#undef VK_DECL

VkInstance inst = VK_NULL_HANDLE;
VkPhysicalDevice phys = VK_NULL_HANDLE;
VkDevice dev = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t queueFamily = 0;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkPhysicalDeviceProperties props{};
VkPhysicalDeviceMemoryProperties memProps{};
bool customBorder = false, devOk = false, vsyncOn = false;
char gpuName[256] = "Vulkan";
VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

// ------------------------------------------------------------------ memory
// Device memory in 128 MB blocks per memory type with first-fit free lists (drivers allow ~4096 allocations);
// resources over 32 MB get their own allocation.
struct Alloc { VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize off = 0, size = 0; int block = -1; uint8_t* ptr = nullptr; };
struct Block { VkDeviceMemory mem; uint32_t type; uint8_t* ptr; std::map<VkDeviceSize, VkDeviceSize> freeList; };   // offset -> size
std::vector<Block> blocks;
std::mutex allocMx;
constexpr VkDeviceSize kBlock = VkDeviceSize(128) << 20;
// VKALLOCCHECK=1 (debug): every block allocation is checked against the live ones (overlaps, double frees)
std::map<std::pair<int, VkDeviceSize>, VkDeviceSize> liveAllocs;
const bool allocCheck = std::getenv("VKALLOCCHECK") != nullptr;
void alloc_check_add(int b, VkDeviceSize off, VkDeviceSize size) {
    auto it = liveAllocs.lower_bound({b, off});
    if (it != liveAllocs.end() && it->first.first == b && it->first.second < off + size) RLOG("[vkalloc] OVERLAP block %d %llu+%llu with next %llu+%llu\n", b, (unsigned long long)off, (unsigned long long)size, (unsigned long long)it->first.second, (unsigned long long)it->second);
    if (it != liveAllocs.begin()) { auto pv = std::prev(it); if (pv->first.first == b && pv->first.second + pv->second > off) RLOG("[vkalloc] OVERLAP block %d %llu+%llu with prev %llu+%llu\n", b, (unsigned long long)off, (unsigned long long)size, (unsigned long long)pv->first.second, (unsigned long long)pv->second); }
    liveAllocs[{b, off}] = size;
}
void alloc_check_free(int b, VkDeviceSize off) {
    if (!liveAllocs.erase({b, off})) RLOG("[vkalloc] FREE of a range not allocated: block %d off %llu\n", b, (unsigned long long)off);
}
int mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}
bool alloc_mem(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, Alloc& a) {
    int type = mem_type(r.memoryTypeBits, want);
    if (type < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = mem_type(r.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return false;
    const bool host = memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    std::lock_guard<std::mutex> lk(allocMx);
    if (r.size > kBlock / 4) {   // dedicated
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, r.size, uint32_t(type)};
        if (vkAllocateMemory(dev, &ai, nullptr, &a.mem) != VK_SUCCESS) return false;
        a.off = 0; a.size = r.size; a.block = -1;
        if (host) { void* p = nullptr; vkMapMemory(dev, a.mem, 0, VK_WHOLE_SIZE, 0, &p); a.ptr = static_cast<uint8_t*>(p); }
        return true;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(r.alignment, props.limits.bufferImageGranularity);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t b = 0; b < blocks.size(); ++b) {
            Block& bl = blocks[b];
            if (bl.type != uint32_t(type)) continue;
            for (auto it = bl.freeList.begin(); it != bl.freeList.end(); ++it) {
                const VkDeviceSize start = (it->first + align - 1) & ~(align - 1);
                if (start + r.size > it->first + it->second) continue;
                const VkDeviceSize fOff = it->first, fSize = it->second;
                bl.freeList.erase(it);
                if (start > fOff) bl.freeList[fOff] = start - fOff;
                if (start + r.size < fOff + fSize) bl.freeList[start + r.size] = fOff + fSize - (start + r.size);
                a.mem = bl.mem; a.off = start; a.size = r.size; a.block = int(b); a.ptr = bl.ptr ? bl.ptr + start : nullptr;
                if (allocCheck) alloc_check_add(int(b), start, r.size);
                return true;
            }
        }
        if (pass == 1) break;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, kBlock, uint32_t(type)};
        Block nb{VK_NULL_HANDLE, uint32_t(type), nullptr, {}};
        if (vkAllocateMemory(dev, &ai, nullptr, &nb.mem) != VK_SUCCESS) return false;
        if (host) { void* p = nullptr; vkMapMemory(dev, nb.mem, 0, VK_WHOLE_SIZE, 0, &p); nb.ptr = static_cast<uint8_t*>(p); }
        nb.freeList[0] = kBlock;
        blocks.push_back(std::move(nb));
    }
    return false;
}
void free_mem(Alloc& a) {
    if (!a.mem) return;
    std::lock_guard<std::mutex> lk(allocMx);
    if (a.block < 0) { vkFreeMemory(dev, a.mem, nullptr); a = Alloc{}; return; }
    if (allocCheck) alloc_check_free(a.block, a.off);
    auto& fl = blocks[size_t(a.block)].freeList;
    VkDeviceSize off = a.off, size = a.size;
    auto next = fl.lower_bound(off);
    if (next != fl.end() && next->first == off + size) { size += next->second; next = fl.erase(next); }
    if (next != fl.begin()) { auto prev = std::prev(next); if (prev->first + prev->second == off) { off = prev->first; size += prev->second; fl.erase(prev); } }
    fl[off] = size;
    a = Alloc{};
}

struct Buffer { VkBuffer buf = VK_NULL_HANDLE; Alloc mem; VkDeviceSize size = 0; };
bool make_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b.buf, &r);
    if (!alloc_mem(r, want, b.mem)) { vkDestroyBuffer(dev, b.buf, nullptr); b.buf = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(dev, b.buf, b.mem.mem, b.mem.off);
    b.size = size;
    return true;
}

struct Image { VkImage img = VK_NULL_HANDLE; Alloc mem; VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0, mips = 1, layers = 1; };
bool make_image(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageUsageFlags usage, bool cube) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1}; ci.mipLevels = mips; ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ci, nullptr, &im.img) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, im.img, &r);
    if (!alloc_mem(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, im.mem)) { vkDestroyImage(dev, im.img, nullptr); im.img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(dev, im.img, im.mem.mem, im.mem.off);
    im.fmt = fmt; im.w = w; im.h = h; im.mips = mips; im.layers = layers;
    return true;
}
VkImageView make_view(VkImage img, VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers, VkComponentMapping comp = {}) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img; ci.viewType = type; ci.format = fmt; ci.components = comp;
    ci.subresourceRange = {aspect, 0, mips, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(dev, &ci, nullptr, &v);
    return v;
}

// ------------------------------------------------------------------ frames
// Two frames in flight. Each has a command buffer, a fence, descriptor pools and a 64 MB host-visible ring (vertex,
// index, uniform and staging data). Resources dropped during a frame are destroyed when that frame is known done.
constexpr int kFrames = 2;
constexpr VkDeviceSize kRing = VkDeviceSize(64) << 20;
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> dpools; size_t dpoolUsed = 0;
    Buffer ring; VkDeviceSize head = 0;
    std::vector<std::function<void()>> garbage;
    bool submitted = false;
};
Frame frames[kFrames];
int fi = 0;                     // current frame
bool recording = false;         // the current frame's command buffer is open
bool rendering = false;         // inside vkCmdBeginRendering
Frame& F() { return frames[fi]; }
VkCommandBuffer cmd() { return F().cmd; }
void later(std::function<void()> f) { F().garbage.push_back(std::move(f)); }
void begin_frame();
void end_rendering();
void flush_and_wait();

// room in the frame's ring; nullptr when it is full (the caller flushes and retries)
uint8_t* ring_alloc(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize& off) {
    Frame& f = F();
    const VkDeviceSize h = (f.head + align - 1) & ~(align - 1);
    if (h + bytes > kRing) return nullptr;
    f.head = h + bytes; off = h;
    return f.ring.mem.ptr + h;
}
uint8_t* ring_get(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize& off) {
    if (bytes > kRing) return nullptr;
    if (uint8_t* p = ring_alloc(bytes, align, off)) return p;
    flush_and_wait();   // a frame that needs more than the ring: finish it and start over (rare)
    return ring_alloc(bytes, align, off);
}

void barrier_all() {   // everything written before is visible to everything after (target changes, transfers)
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
void image_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t mips = VK_REMAINING_MIP_LEVELS, uint32_t layers = VK_REMAINING_ARRAY_LAYERS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = {aspect, 0, mips, 0, layers};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ------------------------------------------------------------------ shaders (GLSL -> SPIR-V)
std::once_flag glslangInit;
bool to_spirv(const std::string& src, EShLanguage stage, std::vector<uint32_t>& out, const char* what) {
    std::call_once(glslangInit, [] { glslang::InitializeProcess(); });
    glslang::TShader sh(stage);
    const char* s = src.c_str();
    sh.setStrings(&s, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!sh.parse(GetDefaultResources(), 450, false, msg)) {
        RLOG("[vk] %s shader: %s\n", what, sh.getInfoLog());
        static int dumped = 0;
        if (dumped++ < 5) { char fn[64]; std::snprintf(fn, sizeof fn, "vk_failed_%d.glsl", dumped); if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(src.data(), 1, src.size(), f); std::fclose(f); } }
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msg)) { RLOG("[vk] %s link: %s\n", what, prog.getInfoLog()); return false; }
    glslang::SpvOptions opt;
    opt.disableOptimizer = false;
    glslang::GlslangToSpv(*prog.getIntermediate(stage), out, &opt);
    return !out.empty();
}
// SPIR-V of src from vkcache/ or compiled (and stored)
VkShaderModule shader_module(const std::string& src, EShLanguage stage, const char* what) {
    char name[96];
    std::snprintf(name, sizeof name, "vkcache/%016llx_%d.spv", (unsigned long long)fnv(src.data(), src.size()), int(stage));
    std::vector<uint32_t> code;
    if (FILE* f = std::fopen(name, "rb")) {
        std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0 && n % 4 == 0) { code.resize(size_t(n) / 4); if (std::fread(code.data(), 1, size_t(n), f) != size_t(n)) code.clear(); }
        std::fclose(f);
    }
    if (code.empty()) {
        if (!to_spirv(src, stage, code, what)) return VK_NULL_HANDLE;
        if (FILE* f = std::fopen(name, "wb")) { std::fwrite(code.data(), 4, code.size(), f); std::fclose(f); }
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size() * 4, code.data()};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}
bool spirv_cached(const std::string& src, EShLanguage stage) {
    char name[96];
    std::snprintf(name, sizeof name, "vkcache/%016llx_%d.spv", (unsigned long long)fnv(src.data(), src.size()), int(stage));
    if (FILE* f = std::fopen(name, "rb")) { std::fclose(f); return true; }
    return false;
}

const char* const kVsHead =
    "#version 450\n"
    "layout(std140, set = 0, binding = 0) uniform VC { vec4 vc[468]; };\n"
    "layout(std140, set = 0, binding = 1) uniform DC { vec4 u_vscale, u_voffset, u_surf, u_alpha, u_wpos; vec4 u_tsc[16]; };\n"
    "layout(location = 0) out vec4 v_col0; layout(location = 1) out vec4 v_col1; layout(location = 2) out vec4 v_bfc0;\n"
    "layout(location = 3) out vec4 v_bfc1; layout(location = 4) out vec4 v_fog; layout(location = 5) out vec4 v_tc[10];\n"
    "float rcc(float x) { float r = 1.0 / x; return sign(r) * clamp(abs(r), 5.42101e-20, 1.884467e19); }\n"
    "vec4 vexp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 vlog(float x) { float a = abs(x); float e = floor(log2(a)); return vec4(e, a / exp2(e), log2(a), 1.0); }\n"
    "vec4 vlit(vec4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0; return vec4(1.0, d, sp, 1.0); }\n";
std::string vk_vs(const VpInfo& vi) {
    std::string h = kVsHead;
    char t[128];
    for (int k = 0; k < 16; ++k) if (vi.inputs & (1u << k)) { std::snprintf(t, sizeof t, "layout(location = %d) in vec4 in%d;\n", k, k); h += t; }
    h += "void main() {\n"
         "  vec4 R[64]; for (int i = 0; i < 64; ++i) R[i] = vec4(0.0);\n"
         "  vec4 t = vec4(0.0), CC0 = vec4(0.0), CC1 = vec4(0.0); ivec4 A0 = ivec4(0), A1 = ivec4(0);\n"
         "  vec4 o_pos = vec4(0.0, 0.0, 0.0, 1.0), o_psz = vec4(1.0), o_dummy = vec4(0.0);\n"
         "  v_col0 = vec4(1.0); v_col1 = vec4(0.0); v_bfc0 = vec4(1.0); v_bfc1 = vec4(0.0); v_fog = vec4(0.0);\n"
         "  for (int i = 0; i < 10; ++i) v_tc[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
    h += vi.body;
    // the RSX viewport transform folded into clip space as in GL: Vulkan's clip y = -1 is framebuffer row 0, which is
    // also RSX surface row 0
    h += "  gl_Position = vec4(o_pos.x * (2.0 * u_vscale.x / u_surf.x) + o_pos.w * (2.0 * u_voffset.x / u_surf.x - 1.0),\n"
         "                     o_pos.y * (2.0 * u_vscale.y / u_surf.y) + o_pos.w * (2.0 * u_voffset.y / u_surf.y - 1.0),\n"
         "                     o_pos.z * u_vscale.z + o_pos.w * u_voffset.z, o_pos.w);\n"
         "  gl_PointSize = o_psz.x;\n}\n";
    return h;
}
std::string vk_fs(const FpInfo& fi, uint32_t texDims, uint32_t control) {
    std::string h = std::string("#version 450\n#define SHBIAS ") + (std::getenv("SHBIAS") ? std::getenv("SHBIAS") : "0.002") + "\n"
        "layout(location = 0) in vec4 v_col0; layout(location = 1) in vec4 v_col1; layout(location = 2) in vec4 v_bfc0;\n"
        "layout(location = 3) in vec4 v_bfc1; layout(location = 4) in vec4 v_fog; layout(location = 5) in vec4 v_tc[10];\n"
        "layout(std140, set = 0, binding = 1) uniform DC { vec4 u_vscale, u_voffset, u_surf, u_alpha, u_wpos; vec4 u_tsc[16]; };\n"
        "layout(location = 0) out vec4 o_c0; layout(location = 1) out vec4 o_c1; layout(location = 2) out vec4 o_c2; layout(location = 3) out vec4 o_c3;\n";
    char t[200];
    std::snprintf(t, sizeof t, "layout(std140, set = 0, binding = 2) uniform FC { vec4 fc[%u]; };\n", std::max(1u, fi.nConst));
    h += t;
    for (int k = 0; k < 16; ++k) {
        if (!(fi.texUsed & (1u << k))) continue;
        const uint32_t dim = (texDims >> (2 * k)) & 3;
        std::snprintf(t, sizeof t, "layout(set = 0, binding = %d) uniform %s tex%d;\n", 3 + k, dim == 1 ? "samplerCube" : dim == 2 ? "sampler3D" : "sampler2D", k);
        h += t;
    }
    h += "bool fcond1(float c, int lt, int eq, int gt) { return (lt != 0 && c < 0.0) || (eq != 0 && c == 0.0) || (gt != 0 && c > 0.0); }\n"
         "bool fcond(vec4 c, int lt, int eq, int gt) { return fcond1(c.x, lt, eq, gt) || fcond1(c.y, lt, eq, gt) || fcond1(c.z, lt, eq, gt) || fcond1(c.w, lt, eq, gt); }\n"
         "float shcmp(sampler2D t, vec3 p, float f) {\n"
         "  vec4 d = textureGather(t, p.xy, 0); vec4 r = vec4(p.z - SHBIAS); vec4 c;\n"
         "  if (f < 1.5) c = vec4(lessThan(d, r)); else if (f < 2.5) c = vec4(equal(d, r)); else if (f < 3.5) c = vec4(lessThanEqual(d, r));\n"
         "  else if (f < 4.5) c = vec4(greaterThan(d, r)); else if (f < 5.5) c = vec4(notEqual(d, r)); else c = vec4(greaterThanEqual(d, r));\n"
         "  vec2 fr = fract(p.xy * vec2(textureSize(t, 0)) - 0.5);\n"
         "  return mix(mix(c.w, c.z, fr.x), mix(c.x, c.y, fr.x), fr.y);\n}\n"
         "vec4 flit(vec4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? exp2(s.w * log2(max(s.y, 1e-30))) : 0.0; return vec4(1.0, d, sp, 1.0); }\n"
         "vec4 frefl(vec4 n, vec4 e) { return vec4(2.0 * dot(n.xyz, e.xyz) / dot(n.xyz, n.xyz) * n.xyz - e.xyz, 0.0); }\n"
         "vec4 col0() { return v_col0; }\nvec4 col1() { return v_col1; }\n"
         "void main() {\n"
         "  vec4 R[64]; vec4 H[64]; for (int i = 0; i < 64; ++i) { R[i] = vec4(0.0); H[i] = vec4(0.0); }\n"
         "  vec4 t = vec4(0.0), CC0 = vec4(0.0), CC1 = vec4(0.0);\n"
         "  vec4 wpos = vec4(gl_FragCoord.x * u_wpos.x, gl_FragCoord.y * u_wpos.y, gl_FragCoord.z, 1.0 / gl_FragCoord.w);\n"
         "  vec4 ssa = vec4(gl_FrontFacing ? 1.0 : -1.0);\n";
    h += fi.body;
    const bool out32 = (control & 0x40) != 0;
    const bool useR = fi.writesR0 && (!fi.writesH0 || out32);
    h += useR ? "  o_c0 = R[0]; o_c1 = R[2]; o_c2 = R[3]; o_c3 = R[4];\n" : "  o_c0 = H[0]; o_c1 = H[4]; o_c2 = H[6]; o_c3 = H[8];\n";
    if (control & 0xe) h += "  gl_FragDepth = R[1].z;\n";
    h += "  if (u_alpha.x > 0.5) {\n"
         "    int f = int(u_alpha.y); float a = o_c0.a, r = u_alpha.z; bool pass = true;\n"
         "    if (f == 0) pass = false; else if (f == 1) pass = a < r; else if (f == 2) pass = a == r; else if (f == 3) pass = a <= r;\n"
         "    else if (f == 4) pass = a > r; else if (f == 5) pass = a != r; else if (f == 6) pass = a >= r;\n"
         "    if (!pass) discard;\n  }\n}\n";
    return h;
}

// ------------------------------------------------------------------ worker threads (shaders, pipelines)
struct WorkQueue {
    std::mutex mx; std::condition_variable cv; std::deque<std::function<void()>> jobs; bool started = false;
    void push(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(mx);
            if (!started) {
                started = true;
                const int n = std::clamp(int(SDL_GetNumLogicalCPUCores()) / 4, 1, 3);
                for (int i = 0; i < n; ++i)
                    if (SDL_Thread* t = SDL_CreateThread([](void* q) -> int { static_cast<WorkQueue*>(q)->run(); return 0; }, "vkbuild", this)) SDL_DetachThread(t);
            }
            jobs.push_back(std::move(f));
        }
        cv.notify_one();
    }
    void run() {
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        for (;;) {
            std::function<void()> f;
            { std::unique_lock<std::mutex> lk(mx); cv.wait(lk, [&] { return !jobs.empty(); }); f = std::move(jobs.front()); jobs.pop_front(); }
            f();
        }
    }
} work;

// ------------------------------------------------------------------ programs and pipelines
VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
VkPipelineCache pipeCache = VK_NULL_HANDLE;
std::atomic<int> newPipelines{0};
std::mutex knownMx;
std::unordered_set<uint64_t> knownPipes;   // pipeline keys built in earlier sessions (their creation hits the cache)

struct Program { VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE; uint32_t vpInputs = 0, texUsed = 0, texDims = 0, nConst = 0; std::atomic<int> state{0}; };   // 0 building, 1 ready, 2 failed
std::unordered_map<uint64_t, Program> programs;
std::vector<float> fpConsts;
void build_program(Program& p, const std::string& vs, const std::string& fs) {
    p.vs = shader_module(vs, EShLangVertex, "vertex");
    p.fs = shader_module(fs, EShLangFragment, "fragment");
    p.state.store(p.vs && p.fs ? 1 : 2, std::memory_order_release);
}
Program* get_program(uint32_t vpStart, uint32_t fpEa, uint32_t texDims, uint32_t control, uint64_t& keyOut) {
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
            fm.raw.assign(C.mem + fpEa, C.mem + fpEa + fp_size(fpEa));
        }
        fpConsts = fm.consts;
        key = (key ^ fm.h) * 0x9E3779B97F4A7C15ull ^ fm.h;
    }
    key ^= (uint64_t(texDims) << 1) ^ (uint64_t(control & 0x4e) << 40) ^ uint64_t(vpStart) * 0x9E3779B97F4A7C15ull;
    keyOut = key;
    auto it = programs.find(key);
    if (it != programs.end()) return &it->second;
    Program& p = programs[key];
    ++g_progBuilds;
    TickScope progTime(g_progTicks);
    const VpInfo vi = translate_vp(vpCode, vpStart);
    const FpInfo fi = translate_fp(fpEa, texDims, control);
    p.vpInputs = vi.inputs; p.texUsed = fi.texUsed; p.texDims = texDims; p.nConst = fi.nConst;
    std::string vs = vk_vs(vi), fs = vk_fs(fi, texDims, control);
    if (dumpShaders) {
        static int nShader = 0;
        char fn[128];
        std::snprintf(fn, sizeof fn, "rsxdump/vk_%04d_%016llx.glsl", nShader++, (unsigned long long)key);
        if (FILE* f = std::fopen(fn, "wb")) { std::fprintf(f, "%s\n// ---- fragment\n%s", vs.c_str(), fs.c_str()); std::fclose(f); }
    }
    static const bool sync = std::getenv("VKSYNC") != nullptr;
    if (sync || (spirv_cached(vs, EShLangVertex) && spirv_cached(fs, EShLangFragment))) build_program(p, vs, fs);
    else { Program* pp = &p; work.push([pp, vs = std::move(vs), fs = std::move(fs)] { build_program(*pp, vs, fs); }); }
    return &p;
}

struct PipeKey {   // everything a pipeline depends on beyond dynamic state
    uint64_t prog; uint32_t blend[5]; uint32_t colorFmt[4]; uint32_t depthFmt; uint32_t nColor; uint32_t comps[16]; uint32_t topoClass; uint32_t clamp;
};
struct Pipe { VkPipeline pipe = VK_NULL_HANDLE; std::atomic<int> state{0}; bool requested = false; };   // state: 0 building, 1 ready, 2 failed
std::unordered_map<uint64_t, Pipe> pipes;

VkBlendFactor blend_of(uint32_t gl, bool alpha) {
    switch (gl) {
        case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA; case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA; case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blendop_of(uint32_t gl) {
    switch (gl) { case 0x8007: return VK_BLEND_OP_MIN; case 0x8008: return VK_BLEND_OP_MAX; case 0x800A: return VK_BLEND_OP_SUBTRACT;
                  case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT; default: return VK_BLEND_OP_ADD; }
}
const VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};

// a graphics pipeline; vs/fs modules, the key's formats and blend. Any thread.
VkPipeline create_pipeline(VkShaderModule vs, VkShaderModule fs, const PipeKey& k, VkPipelineLayout layout, bool vertexInput = true) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkVertexInputBindingDescription bind[16]; VkVertexInputAttributeDescription attr[16]; uint32_t nb = 0;
    static const VkFormat ff[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    if (vertexInput)
        for (uint32_t a = 0; a < 16; ++a) {
            if (!k.comps[a]) continue;
            bind[nb] = {a, 16, VK_VERTEX_INPUT_RATE_VERTEX};
            attr[nb] = {a, a, ff[k.comps[a]], 0};
            ++nb;
        }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, nb, bind, nb, attr};
    static const VkPrimitiveTopology classTopo[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, classTopo[std::min(k.topoClass, 2u)], VK_FALSE};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = k.clamp; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[4]{};
    for (uint32_t i = 0; i < k.nColor; ++i) {
        cb[i].blendEnable = k.blend[0];
        cb[i].srcColorBlendFactor = blend_of(k.blend[1] & 0xffff, false); cb[i].dstColorBlendFactor = blend_of(k.blend[2] & 0xffff, false);
        cb[i].srcAlphaBlendFactor = blend_of(k.blend[1] >> 16, true); cb[i].dstAlphaBlendFactor = blend_of(k.blend[2] >> 16, true);
        cb[i].colorBlendOp = blendop_of(k.blend[3] & 0xffff); cb[i].alphaBlendOp = blendop_of(k.blend[3] >> 16);
        const uint32_t cm = k.blend[4];
        cb[i].colorWriteMask = ((cm >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) | (cm & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cm >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, k.nColor, cb, {0, 0, 0, 0}};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, uint32_t(sizeof kDynamic / sizeof kDynamic[0]) - (vertexInput ? 0u : 1u), kDynamic};
    VkFormat cf[4];
    for (uint32_t i = 0; i < k.nColor; ++i) cf[i] = VkFormat(k.colorFmt[i]);
    const VkFormat df = VkFormat(k.depthFmt);
    const bool hasStencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, k.nColor, cf, df, hasStencil ? df : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cbs; ci.pDynamicState = &dy; ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, pipeCache, 1, &ci, nullptr, &p) != VK_SUCCESS) RLOG("[vk] pipeline creation failed\n");
    return p;
}

// ------------------------------------------------------------------ surfaces (render targets)
struct Surface { Image im; VkImageView att = VK_NULL_HANDLE, smp = VK_NULL_HANDLE; uint32_t w = 0, h = 0, fmt = 0; bool depth = false; };
std::map<uint64_t, Surface> surfaces;
uint64_t surfGen = 1;
VkFormat surface_format(uint32_t fmt) {
    switch (fmt) {
        case 9: return VK_FORMAT_R8_UNORM; case 0xa: return VK_FORMAT_R8G8_UNORM; case 0xb: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0xc: return VK_FORMAT_R32G32B32A32_SFLOAT; case 0xd: return VK_FORMAT_R32_SFLOAT; default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
void free_surface(Surface s) {
    later([s]() mutable { vkDestroyImageView(dev, s.att, nullptr); if (s.smp != s.att) vkDestroyImageView(dev, s.smp, nullptr); vkDestroyImage(dev, s.im.img, nullptr); free_mem(s.im.mem); });
}
bool make_surface(Surface& s, uint32_t w, uint32_t h, uint32_t fmt, bool depth) {
    const VkFormat vf = depth ? (fmt == 1 ? VK_FORMAT_D16_UNORM : depthFormat) : surface_format(fmt);
    const VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!make_image(s.im, vf, w, h, 1, 1, use, false)) return false;
    const bool stencil = vf == VK_FORMAT_D24_UNORM_S8_UINT || vf == VK_FORMAT_D32_SFLOAT_S8_UINT;
    s.att = make_view(s.im.img, vf, VK_IMAGE_VIEW_TYPE_2D, depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    s.smp = depth ? make_view(s.im.img, vf, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1) : s.att;
    end_rendering();
    image_barrier(s.im.img, depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    s.w = w; s.h = h; s.fmt = fmt; s.depth = depth;
    return true;
}
Surface* get_surface(uint32_t loc, uint32_t off, uint32_t w, uint32_t h, uint32_t fmt, bool depth) {
    const uint64_t key = (uint64_t(loc) << 33) | (uint64_t(depth) << 32) | off;
    Surface& s = surfaces[key];
    if (s.im.img && s.w == w && s.h == h && s.fmt == fmt) return &s;
    struct Parked { uint64_t key; Surface s; };
    static std::vector<Parked> parked;
    {
        static const long tt = std::getenv("VKTRACETGT") ? std::strtol(std::getenv("VKTRACETGT"), nullptr, 16) : -1;
        if (tt >= 0 && long(off) == tt) RLOG("[vktrace] frame %llu surface %x %s: %ux%u fmt %u -> %ux%u fmt %u\n", (unsigned long long)frameNo, off, depth ? "depth" : "colour", s.w, s.h, s.fmt, w, h, fmt);
    }
    if (s.im.img) parked.push_back({key, s});
    s = Surface{};
    ++surfGen;
    for (size_t k = 0; k < parked.size(); ++k)
        if (parked[k].key == key && parked[k].s.w == w && parked[k].s.h == h && parked[k].s.fmt == fmt) { s = parked[k].s; parked.erase(parked.begin() + long(k)); return &s; }
    if (!make_surface(s, w, h, fmt, depth)) { RLOG("[vk] surface %ux%u format %u failed\n", w, h, fmt); return nullptr; }
    RLOG("[rsx] surface %s at %s 0x%x: %ux%u format %u\n", depth ? "depth" : "color", loc ? "main" : "local", off, w, h, fmt);
    return &s;
}

struct Target { uint32_t w = 0, h = 0, aaX = 1, aaY = 1, logW = 0, logH = 0, nColor = 0; VkImageView color[4] = {}; VkFormat colorFmt[4] = {}; VkImageView depth = VK_NULL_HANDLE; VkFormat depthFmt = VK_FORMAT_UNDEFINED; uint64_t key = 0; };
Target cur;
bool targetValid = false;

void begin_rendering() {
    if (rendering || !targetValid) return;
    VkRenderingAttachmentInfo ca[4]{};
    for (uint32_t i = 0; i < cur.nColor; ++i) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].imageView = cur.color[i]; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = cur.depth; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const bool stencil = cur.depthFmt == VK_FORMAT_D24_UNORM_S8_UINT || cur.depthFmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {cur.w, cur.h}}; ri.layerCount = 1;
    ri.colorAttachmentCount = cur.nColor; ri.pColorAttachments = ca;
    ri.pDepthAttachment = cur.depth ? &da : nullptr; ri.pStencilAttachment = cur.depth && stencil ? &da : nullptr;
    vkCmdBeginRendering(cmd(), &ri);
    rendering = true;
    VkViewport v{0, 0, float(cur.w), float(cur.h), 0, 1};
    vkCmdSetViewport(cmd(), 0, 1, &v);
}
void end_rendering() {
    if (!recording) return;
    if (rendering) { vkCmdEndRendering(cmd()); rendering = false; }
}

bool bind_target() {
    if (!recording) begin_frame();
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
    if (targetValid && fnv(kk, sizeof kk) == cur.key) { begin_rendering(); return true; }
    Target t;
    t.w = w; t.h = h; t.aaX = ax; t.aaY = ay; t.logW = clipX + clipW; t.logH = clipY + clipH;
    for (int k = 0; k < nList; ++k) {
        const int i = list[k];
        if (Surface* s = get_surface(R[dmaReg[i] >> 2] & 1, R[offReg[i] >> 2], w, h, cfmt, false)) { t.color[t.nColor] = s->att; t.colorFmt[t.nColor] = s->im.fmt; ++t.nColor; }
    }
    if (zfmt) if (Surface* s = get_surface(R[0x198 >> 2] & 1, R[0x214 >> 2], w, h, zfmt, true)) { t.depth = s->att; t.depthFmt = s->im.fmt; }
    if (!t.nColor && !t.depth) return false;
    kk[10] = uint32_t(surfGen); kk[11] = uint32_t(surfGen >> 32);
    t.key = fnv(kk, sizeof kk);
    end_rendering();
    barrier_all();   // what the previous target got is visible to the next passes (sampled as textures)
    cur = t; targetValid = true;
    begin_rendering();
    return true;
}

VkRect2D scissor_rect() {
    uint32_t sx = R[0x8c0 >> 2] & 0xffff, sw = R[0x8c0 >> 2] >> 16, sy = R[0x8c4 >> 2] & 0xffff, sh = R[0x8c4 >> 2] >> 16;
    const uint32_t vx = R[0xa00 >> 2] & 0xffff, vw = R[0xa00 >> 2] >> 16, vy = R[0xa04 >> 2] & 0xffff, vh = R[0xa04 >> 2] >> 16;
    if (sw == 0 && sh == 0) { sw = 4096; sh = 4096; }
    const int x0 = int(std::max(sx, vx)), y0 = int(std::max(sy, vy));
    const int x1 = int(std::min(sx + sw, vx + vw)), y1 = int(std::min(sy + sh, vy + vh));
    int l = x0 * int(cur.aaX), t = y0 * int(cur.aaY), r = std::max(x0, x1) * int(cur.aaX), b = std::max(y0, y1) * int(cur.aaY);
    l = std::clamp(l, 0, int(cur.w)); r = std::clamp(r, l, int(cur.w)); t = std::clamp(t, 0, int(cur.h)); b = std::clamp(b, t, int(cur.h));
    return VkRect2D{{l, t}, {uint32_t(r - l), uint32_t(b - t)}};
}

// ------------------------------------------------------------------ textures
struct Tex { Image im; uint64_t sig = 0, regSig = 0, checked = ~0ull; bool cube = false; uint32_t chans = 4; int fmtSwz = 0;
             std::unordered_map<uint32_t, VkImageView> views; };   // view per channel remap
std::unordered_map<uint64_t, Tex> texCache;
struct TexFmt { VkFormat f; uint32_t bpp; bool dxt; uint32_t blockBytes; int swap; uint32_t chans; int swz; };   // swz 1: 16-bit A4R4G4B4 read as R4G4B4A4
bool tex_format(uint32_t f, TexFmt& o) {
    switch (f) {
        case 0x81: o = {VK_FORMAT_R8_UNORM, 1, false, 0, 0, 1, 0}; return true;
        case 0x82: o = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, false, 0, 2, 4, 0}; return true;
        case 0x83: o = {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, false, 0, 2, 4, 1}; return true;
        case 0x84: o = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, false, 0, 2, 4, 0}; return true;
        case 0x85: case 0x9e: o = {VK_FORMAT_B8G8R8A8_UNORM, 4, false, 0, 4, 4, 0}; return true;
        case 0x86: o = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, true, 8, 0, 4, 0}; return true;
        case 0x87: o = {VK_FORMAT_BC2_UNORM_BLOCK, 0, true, 16, 0, 4, 0}; return true;
        case 0x88: o = {VK_FORMAT_BC3_UNORM_BLOCK, 0, true, 16, 0, 4, 0}; return true;
        case 0x8b: o = {VK_FORMAT_R8G8_UNORM, 2, false, 0, 0, 2, 0}; return true;
        case 0x94: o = {VK_FORMAT_R16_UNORM, 2, false, 0, 2, 1, 0}; return true;
        case 0x95: o = {VK_FORMAT_R16G16_UNORM, 4, false, 0, 2, 2, 0}; return true;
        case 0x9a: o = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, false, 0, 2, 4, 0}; return true;
        case 0x9b: o = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, false, 0, 4, 4, 0}; return true;
        case 0x9c: o = {VK_FORMAT_R32_SFLOAT, 4, false, 0, 4, 1, 0}; return true;
        case 0x9f: o = {VK_FORMAT_R16G16_SFLOAT, 4, false, 0, 2, 2, 0}; return true;
        default: return false;
    }
}
// the RSX channel remap (per output R G B A: a source channel A R G B, or 0 / 1) as a view's component mapping
VkComponentMapping remap_components(uint32_t remap, uint32_t chans, int fmtSwz) {
    static const int outOrder[4] = {1, 2, 3, 0};   // output R G B A <- RSX channel index (A = 0, R = 1, G = 2, B = 3)
    VkComponentSwizzle c[4];
    for (int k = 0; k < 4; ++k) {
        const int rc = outOrder[k];
        const uint32_t sel = (remap >> (2 * rc)) & 3, mode = (remap >> (8 + 2 * rc)) & 3;
        if (mode == 0) { c[k] = VK_COMPONENT_SWIZZLE_ZERO; continue; }
        if (mode == 1) { c[k] = VK_COMPONENT_SWIZZLE_ONE; continue; }
        VkComponentSwizzle s = sel == 0 ? VK_COMPONENT_SWIZZLE_A : sel == 1 ? VK_COMPONENT_SWIZZLE_R : sel == 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_B;
        if (fmtSwz == 1) s = sel == 0 ? VK_COMPONENT_SWIZZLE_R : sel == 1 ? VK_COMPONENT_SWIZZLE_G : sel == 2 ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_A;   // A R G B in R G B A
        if (chans == 1) s = VK_COMPONENT_SWIZZLE_R;
        else if (chans == 2) s = (sel == 1 || sel == 3) ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R;
        c[k] = s;
    }
    return VkComponentMapping{c[0], c[1], c[2], c[3]};
}

std::unordered_map<uint64_t, VkSampler> samplers;
VkSampler linearClamp = VK_NULL_HANDLE, cubeSampler = VK_NULL_HANDLE;
VkSamplerAddressMode wrap_of(uint32_t w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 6: case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
VkSampler sampler_for(uint32_t base, uint32_t mips, bool surface) {
    const uint32_t addr = R[(base + 8) >> 2], ctl0 = R[(base + 0xc) >> 2], filt = R[(base + 0x14) >> 2], border = R[(base + 0x1c) >> 2];
    const uint32_t k[7] = {addr, surface ? 0u : ctl0, filt, border, surface ? 1u : mips, uint32_t(settings().aniso), surface};
    const uint64_t h = fnv(k, sizeof k);
    VkSampler& s = samplers[h];
    if (s) return s;
    const uint32_t minF = (filt >> 16) & 0xff, magF = (filt >> 24) & 7;
    const bool magLin = magF != 1;
    bool minLin, mipLin = false;
    if (surface) minLin = magLin;
    else if (mips > 1) { const uint32_t m = minF & 7; minLin = m == 2 || m == 4 || m == 6 || m == 7; mipLin = m == 5 || m == 6; }
    else minLin = !(minF == 1 || minF == 3 || minF == 5);
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = magLin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.minFilter = minLin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mipLin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_of(addr & 0xf); ci.addressModeV = wrap_of((addr >> 8) & 0xf); ci.addressModeW = wrap_of((addr >> 16) & 0xf);
    const uint32_t gameAniso = std::min<uint32_t>(4, (ctl0 >> 4) & 7);
    const uint32_t an = !surface && mips > 1 ? std::max<uint32_t>(gameAniso, uint32_t(std::clamp(settings().aniso, 0, 4))) : 0;
    if (an > 0 && minLin) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min(float(1u << an), props.limits.maxSamplerAnisotropy); }
    if (surface) { ci.minLod = 0; ci.maxLod = 0; }
    else { ci.minLod = ((ctl0 >> 19) & 0xfff) / 256.f; ci.maxLod = mips > 1 ? ((ctl0 >> 7) & 0xfff) / 256.f : 0.f; if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod; }
    // minification without mipmapping (1 nearest, 2 linear) reads the base level only, as on the RSX and in GL: the
    // car liveries are copied from a 512x512 texture with stale smaller levels into 256x256 targets (cars white,
    // black or without decals after a race)
    if (!surface && mips > 1 && ((minF & 7) == 1 || (minF & 7) == 2)) { ci.minLod = 0.f; ci.maxLod = 0.f; }
    const float bc[4] = {((border >> 16) & 0xff) / 255.f, ((border >> 8) & 0xff) / 255.f, (border & 0xff) / 255.f, (border >> 24) / 255.f};
    VkSamplerCustomBorderColorCreateInfoEXT cbc{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (customBorder) {
        std::memcpy(cbc.customBorderColor.float32, bc, sizeof bc);
        cbc.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &cbc; ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else ci.borderColor = bc[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : (bc[0] + bc[1] + bc[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    vkCreateSampler(dev, &ci, nullptr, &s);
    return s;
}

// copies data (already in the ring at off) into an image's subresources and leaves it in `final`
void upload_regions(VkImage img, const std::vector<VkBufferImageCopy>& regions, VkImageLayout from, VkImageLayout final) {
    end_rendering();
    image_barrier(img, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd(), F().ring.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    image_barrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final);
}

struct Dummies { Image im2d, imCube; VkImageView v2d = VK_NULL_HANDLE, vCube = VK_NULL_HANDLE; } dummy;

struct UnitBind { VkImageView view = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; float tsc[4] = {1, 1, 1, 1}; };
bool g_traceDraw = false;
// VKTRACETGT=hex colour target (debug): describe the draws into it and their textures
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
    for (int z = 0; z < 2; ++z) {   // render target?
        auto it = surfaces.find((uint64_t(loc) << 33) | (uint64_t(z) << 32) | off);
        if (it != surfaces.end() && !cube) { if (g_traceDraw) RLOG("[vktrace]   unit %d: surface %x z%d %ux%u\n", i, off, z, it->second.w, it->second.h); u.view = it->second.smp; u.smp = sampler_for(base, 1, true); u.layout = VK_IMAGE_LAYOUT_GENERAL; return; }
    }
    if (cube) {   // cube map whose faces are render targets (the environment the cars reflect)
        const uint32_t faceStride = (w * h * 4 + 127) & ~127u;
        const uint64_t key0 = (uint64_t(loc) << 33) | off;
        auto f0 = surfaces.find(key0);
        if (f0 != surfaces.end()) {
            struct RenderedCube { Image im; VkImageView view = VK_NULL_HANDLE; uint64_t built = ~0ull; };
            static std::map<uint64_t, RenderedCube> rcubes;
            RenderedCube& rc = rcubes[key0];
            if (!rc.im.img || rc.im.w != w || rc.im.h != h || rc.im.fmt != f0->second.im.fmt) {
                if (rc.im.img) { RenderedCube old = rc; later([old]() mutable { vkDestroyImageView(dev, old.view, nullptr); vkDestroyImage(dev, old.im.img, nullptr); free_mem(old.im.mem); }); }
                rc = RenderedCube{};
                if (!make_image(rc.im, f0->second.im.fmt, w, h, 1, 6, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true)) return;
                rc.view = make_view(rc.im.img, rc.im.fmt, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
                end_rendering();
                image_barrier(rc.im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            }
            if (rc.built != frameNo) {   // once per frame: the faces are drawn before the scene that uses them
                rc.built = frameNo;
                end_rendering();
                barrier_all();
                for (uint32_t k = 0; k < 6; ++k) {
                    auto it = surfaces.find(key0 + uint64_t(k) * faceStride);
                    if (it == surfaces.end() || it->second.im.fmt != rc.im.fmt) continue;
                    VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1}, {0, 0, 0}, {std::min(w, it->second.w), std::min(h, it->second.h), 1}};
                    vkCmdCopyImage(cmd(), it->second.im.img, VK_IMAGE_LAYOUT_GENERAL, rc.im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                }
                barrier_all();
            }
            u.view = rc.view; u.smp = cubeSampler; u.layout = VK_IMAGE_LAYOUT_GENERAL;
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
    if (!tx.im.img || tx.regSig != regSig || frameNo - tx.checked >= 4 || tx.checked > frameNo) {
        sig = regSig;
        const uint32_t step = std::max<uint32_t>(64, total / 128);
        for (uint32_t k = 0; k + 8 <= total; k += step) sig = fnv(C.mem + ea + k, 8, sig);
        sig = fnv(C.mem + ea + total - std::min<uint32_t>(total, 8), std::min<uint32_t>(total, 8), sig);
        tx.checked = frameNo; tx.regSig = regSig;
    }
    const bool force = false;
    if (g_traceDraw) RLOG("[vktrace]   unit %d: texture %x fmt %08x %ux%u %s ctl0 %08x filt %08x addr %08x remap %08x ctl3 %08x\n", i, off, fmtReg, w, h, (!tx.im.img || tx.sig != sig || force) ? "UPLOAD" : "cached",
                          R[(base + 0xc) >> 2], R[(base + 0x14) >> 2], addr, remap, ctl3);
    if (g_traceDraw) {
        uint64_t sum[4] = {}; const uint32_t nb = std::min<uint32_t>(total, w * h * 4) & ~3u;
        for (uint32_t k = 0; k < nb; k += 4) for (int c = 0; c < 4; ++c) sum[c] += C.mem[ea + k + c];
        const uint32_t np = std::max<uint32_t>(1, nb / 4);
        RLOG("[vktrace]   unit %d: bytes avg %llu %llu %llu %llu (ea %x, %u bytes)\n", i, (unsigned long long)(sum[0] / np), (unsigned long long)(sum[1] / np), (unsigned long long)(sum[2] / np), (unsigned long long)(sum[3] / np), ea, total);
    }
    if (!tx.im.img || tx.sig != sig || force) {
        if (tx.im.img) {   // re-upload: the old image may still be in use by frames in flight
            Tex old; old.im = tx.im; old.views = std::move(tx.views);
            later([old]() mutable { for (auto& [k2, v] : old.views) vkDestroyImageView(dev, v, nullptr); vkDestroyImage(dev, old.im.img, nullptr); free_mem(old.im.mem); });
            tx.im = Image{}; tx.views.clear();
        }
        tx.sig = sig; tx.cube = cube; tx.chans = tf.chans; tx.fmtSwz = tf.swz;
        if (tf.dxt && ((w & 3) || (h & 3))) { static int warned = 0; if (warned++ < 5) RLOG("[vk] compressed texture %ux%u not a multiple of 4: skipped\n", w, h); return; }
        ++g_texUploads; g_texUploadBytes += total;
        TickScope texTime(g_texTicks);
        // the converted levels go into the ring, then one copy per level and face
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize need = 0;
        for (uint32_t l = 0, lw = w, lh = h; l < mips; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2))
            need += ((tf.dxt ? VkDeviceSize(std::max(1u, (lw + 3) / 4)) * std::max(1u, (lh + 3) / 4) * tf.blockBytes : VkDeviceSize(lw) * lh * tf.bpp) + 15) & ~VkDeviceSize(15);
        need *= faces;
        VkDeviceSize base0 = 0;
        uint8_t* dst = ring_get(need, 16, base0);
        if (!dst) return;
        if (!make_image(tx.im, tf.f, w, h, mips, faces, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cube)) { static int warned = 0; if (warned++ < 10) RLOG("[vk] texture %ux%u format 0x%x failed\n", w, h, baseFmt); return; }
        VkDeviceSize o = 0;
        for (uint32_t face = 0; face < faces; ++face) {
            uint32_t p = ea + face * faceStride;
            for (uint32_t l = 0, lw = w, lh = h; l < mips; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) {
                const uint32_t lb = levelBytes(lw, lh);
                uint8_t* d0 = dst + o;
                VkDeviceSize bytes;
                if (tf.dxt) { bytes = VkDeviceSize(std::max(1u, (lw + 3) / 4)) * std::max(1u, (lh + 3) / 4) * tf.blockBytes; std::memcpy(d0, C.mem + p, size_t(bytes)); }
                else {
                    bytes = VkDeviceSize(lw) * lh * tf.bpp;
                    const bool swz = !ln && (lw & (lw - 1)) == 0 && (lh & (lh - 1)) == 0;
                    const uint32_t lwl = ilog2(lw), lhl = ilog2(lh);
                    for (uint32_t y = 0; y < lh; ++y) {
                        if (!swz && tf.swap == 0) { std::memcpy(d0 + size_t(y) * lw * tf.bpp, C.mem + p + (ln && pitch ? y * pitch : y * lw * tf.bpp), size_t(lw) * tf.bpp); continue; }
                        for (uint32_t x = 0; x < lw; ++x) {
                            const uint32_t src = swz ? swizzle_index(x, y, lwl, lhl) * tf.bpp : (ln && pitch ? y * pitch + x * tf.bpp : (y * lw + x) * tf.bpp);
                            uint8_t* d = d0 + (size_t(y) * lw + x) * tf.bpp;
                            const uint8_t* s = C.mem + p + src;
                            if (tf.swap == 2) for (uint32_t k = 0; k < tf.bpp; k += 2) { d[k] = s[k + 1]; d[k + 1] = s[k]; }
                            else if (tf.swap == 4) for (uint32_t k = 0; k < tf.bpp; k += 4) { d[k] = s[k + 3]; d[k + 1] = s[k + 2]; d[k + 2] = s[k + 1]; d[k + 3] = s[k]; }
                            else std::memcpy(d, s, tf.bpp);
                        }
                    }
                    if (baseFmt == 0x9e) for (VkDeviceSize k = 3; k < bytes; k += 4) d0[k] = 0xff;   // D8R8G8B8: alpha = 1
                }
                VkBufferImageCopy r{};
                r.bufferOffset = base0 + o;
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1};
                r.imageExtent = {lw, lh, 1};
                regions.push_back(r);
                o += (bytes + 15) & ~VkDeviceSize(15);
                p += lb;
            }
        }
        upload_regions(tx.im.img, regions, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkImageView& v = tx.views[remap];
    if (!v) v = make_view(tx.im.img, tx.im.fmt, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, tx.im.mips, tx.im.layers, remap_components(remap, tx.chans, tx.fmtSwz));
    u.view = v; u.smp = sampler_for(base, mips, false); u.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ------------------------------------------------------------------ vertex streams (static local memory, device-local)
struct Stream { Buffer* buf = nullptr; VkDeviceSize offset = 0; uint32_t count = 0; uint64_t sig = 0, checked = ~0ull; };
std::unordered_map<uint64_t, Stream> streams;
constexpr VkDeviceSize kArenaChunk = VkDeviceSize(64) << 20; constexpr size_t kArenaMaxChunks = 12;
std::vector<Buffer> arenaBufs;
size_t arenaChunk = 0; VkDeviceSize arenaHead = 0;
bool arena_alloc(VkDeviceSize bytes, Buffer*& buf, VkDeviceSize& off) {
    if (bytes > kArenaChunk) return false;
    if (arenaBufs.empty() || arenaHead + bytes > kArenaChunk) {
        if (!arenaBufs.empty()) ++arenaChunk;
        arenaHead = 0;
        if (arenaChunk >= kArenaMaxChunks) {
            for (auto& [k, st] : streams) st.buf = nullptr, st.count = 0;
            arenaChunk = 0;
            flush_and_wait();   // the arena is reused from the start: nothing in flight may still read it
            RLOG("[rsx] vertex stream arena full: rebuilt\n");
        }
        arenaBufs.reserve(kArenaMaxChunks);
        while (arenaBufs.size() <= arenaChunk) {
            Buffer b;
            if (!make_buffer(b, kArenaChunk, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return false;
            arenaBufs.push_back(b);
        }
    }
    buf = &arenaBufs[arenaChunk]; off = arenaHead;
    arenaHead += (bytes + 255) & ~VkDeviceSize(255);
    return true;
}
uint64_t stream_sig(const uint8_t* p, uint32_t stride, uint32_t count) {
    uint64_t h = fnv(&count, 4);
    const uint32_t step = std::max<uint32_t>(1, count / 24);
    for (uint32_t v = 0; v < count; v += step) h = fnv(p + size_t(v) * stride, std::min<uint32_t>(stride, 8), h);
    return fnv(p + size_t(count - 1) * stride, std::min<uint32_t>(stride, 8), h);
}
Buffer* cached_stream(int a, uint32_t need, VkDeviceSize& offset) {
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
        const uint32_t n = need + need / 4 + 64;
        const VkDeviceSize bytes = VkDeviceSize(n) * 16;
        VkDeviceSize so = 0;
        float* tmp = reinterpret_cast<float*>(ring_get(bytes, 16, so));
        if (!tmp) return nullptr;
        { TickScope t(g_convTicks); convert_attr(a, 0, n, tmp); }
        Buffer* b = nullptr; VkDeviceSize o = 0;
        if (!arena_alloc(bytes, b, o)) return nullptr;
        end_rendering();
        VkBufferCopy c{so, o, bytes};
        vkCmdCopyBuffer(cmd(), F().ring.buf, b->buf, 1, &c);
        barrier_all();
        st.buf = b; st.offset = o;
        st.count = n; st.sig = stream_sig(p, stride, n); st.checked = frameNo;
        ++g_streamBuilds; g_streamVerts += n;
    }
    offset = st.offset;
    return st.buf;
}

// ------------------------------------------------------------------ descriptors
VkDescriptorSet alloc_set() {
    Frame& f = F();
    for (;;) {
        if (f.dpoolUsed == f.dpools.size()) {
            VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 2048}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 2048}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2048, 2, sizes};
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev, &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, f.dpools[f.dpoolUsed], 1, &setLayout};
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev, &ai, &s) == VK_SUCCESS) return s;
        ++f.dpoolUsed;
    }
}

// ------------------------------------------------------------------ draws
struct DrawCb { float vs[4], vo[4], surf[4], alpha[4], wpos[4], tsc[16][4]; };
constexpr VkDeviceSize kFcRange = 8192;   // fragment program constants: up to 512
std::vector<uint32_t> idx;
uint64_t skippedDraws = 0;
// what the current command buffer has bound (reset with each frame and after fixed-function passes)
struct Bound { VkPipeline pipe = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; uint32_t offs[3] = {~0u, ~0u, ~0u}; bool dyn = false; uint32_t dynKey[28] = {}; } bound;
uint64_t ringGen = 1;   // +1 at every begin_frame (also the flushes inside a frame): ring offsets and descriptor sets cached by the draws are valid while it stays
VkDeviceSize vcOff = 0, dcOff = 0, fcOff = 0; uint64_t vcFrame = ~0ull, dcFrame = ~0ull, fcFrame = ~0ull;

void apply_dynamic_state(uint32_t topoPrim) {
    {   // the registers this state comes from: nothing to record when they are what the last draw set
        static const uint16_t regs[] = {0x183c, 0x1830, 0x1834, 0xa74, 0xa70, 0xa6c, 0x328, 0x33c, 0x340, 0x344, 0x330, 0x338, 0x32c, 0x334, 0xa68, 0xa78, 0xa7c, 0x31c, 0x8c0, 0x8c4, 0xa00, 0xa04};
        uint32_t k[28];
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; ++i) k[i] = R[regs[i] >> 2];
        k[22] = topoPrim; k[23] = cur.aaX; k[24] = cur.aaY; k[25] = cur.w; k[26] = cur.h; k[27] = 0;
        if (bound.dyn && std::memcmp(k, bound.dynKey, sizeof k) == 0) return;
        std::memcpy(bound.dynKey, k, sizeof k); bound.dyn = true;
    }
    VkCommandBuffer c = cmd();
    static const bool noCull = std::getenv("RSXNOCULL") != nullptr;
    const bool cull = !noCull && (R[0x183c >> 2] & 1);
    const uint32_t face = R[0x1830 >> 2] ? R[0x1830 >> 2] : 0x405;
    vkCmdSetCullMode(c, !cull ? VK_CULL_MODE_NONE : face == 0x404 ? VK_CULL_MODE_FRONT_BIT : face == 0x405 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK);
    const uint32_t ff = R[0x1834 >> 2] ? R[0x1834 >> 2] : 0x901;
    static const bool flip = std::getenv("VKFRONTFLIP") != nullptr;   // (debug) the other winding convention
    vkCmdSetFrontFace(c, (ff == 0x901) != flip ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    VkPrimitiveTopology topo;
    switch (topoPrim) { case 1: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break; case 2: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
                        case 3: case 4: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break; case 6: case 9: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
                        default: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break; }
    vkCmdSetPrimitiveTopology(c, topo);
    vkCmdSetPrimitiveRestartEnable(c, topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    auto cmp = [](uint32_t gl, uint32_t dflt) { const uint32_t v = gl ? gl : dflt; return (v >= 0x200 && v <= 0x207) ? VkCompareOp(v - 0x200) : VK_COMPARE_OP_ALWAYS; };
    vkCmdSetDepthTestEnable(c, R[0xa74 >> 2] & 1);
    vkCmdSetDepthWriteEnable(c, R[0xa70 >> 2] & 1);
    vkCmdSetDepthCompareOp(c, cmp(R[0xa6c >> 2], 0x201));
    const bool st = R[0x328 >> 2] & 1;
    vkCmdSetStencilTestEnable(c, st);
    auto sop = [](uint32_t gl) {
        switch (gl) { case 0: return VK_STENCIL_OP_ZERO; case 0x1E01: return VK_STENCIL_OP_REPLACE; case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                      case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 0x150A: return VK_STENCIL_OP_INVERT; case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                      case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
    };
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, sop(R[0x33c >> 2] ? R[0x33c >> 2] : 0x1e00), sop(R[0x344 >> 2] ? R[0x344 >> 2] : 0x1e00), sop(R[0x340 >> 2] ? R[0x340 >> 2] : 0x1e00), cmp(R[0x330 >> 2], 0x207));
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, R[0x338 >> 2] & 0xff);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, R[0x32c >> 2] & 0xff);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, R[0x334 >> 2] & 0xff);
    const bool bias = R[0xa68 >> 2] & 1;
    vkCmdSetDepthBiasEnable(c, bias);
    if (bias) vkCmdSetDepthBias(c, rf(0xa7c), 0.f, rf(0xa78));
    const uint32_t bc = R[0x31c >> 2];
    const float f[4] = {((bc >> 16) & 0xff) / 255.f, ((bc >> 8) & 0xff) / 255.f, (bc & 0xff) / 255.f, (bc >> 24) / 255.f};
    vkCmdSetBlendConstants(c, f);
    const VkRect2D sc = scissor_rect();
    vkCmdSetScissor(c, 0, 1, &sc);
}

// VKDROPLOG=1 (debug): draws that are not recorded, with the reason and the colour target
#define DROPLOG(why) do { static const bool on_ = std::getenv("VKDROPLOG") != nullptr; static int n_ = 0; if (on_ && n_++ < 400) RLOG("[vkdrop] %s: target %08x %ux%u frame %llu\n", why, R[0x210 >> 2], cur.logW, cur.logH, (unsigned long long)frameNo); } while (0)
void do_draw() {
    if (ranges.empty() || !devOk) return;
    struct DrawTime { uint64_t t0 = SDL_GetPerformanceCounter(); ~DrawTime() { g_drawTicks += SDL_GetPerformanceCounter() - t0; ++g_drawCount; } } drawTime;
    ++drawsThisFrame;
    if (!bind_target()) { DROPLOG("target"); return; }
    const uint32_t sp = R[0x8e4 >> 2];
    const uint32_t fpEa = ea_of(sp & ~3u, (sp & 3) == 2 ? 1 : 0);
    uint32_t texDims = 0;
    for (int i = 0; i < 16; ++i) texDims |= texDimsOf(i) << (2 * i);
    uint64_t progKey = 0;
    Program* p = get_program(R[0x1ea0 >> 2] & 511, fpEa, texDims, R[0x1d60 >> 2], progKey);
// Only draws into the full-screen image may be skipped while their shaders are being built (it is drawn again
// next frame); a draw into anything else (paint layers composed once when a race loads, shadow maps, cube faces)
// waits: skipped, it would leave that texture wrong for good (the player's car came out black).
    const bool mayskip = cur.logW == 1280 && cur.logH == 720;
    int ps = p->state.load(std::memory_order_acquire);
    while (ps == 0 && !mayskip) { SDL_Delay(1); ps = p->state.load(std::memory_order_acquire); }
    if (ps != 1) { if (ps == 0) ++skippedDraws; DROPLOG(ps ? "program failed" : "program pending"); return; }
    // indices
    uint32_t vmin, vmax;
    gather_draw_indices(idx, vmin, vmax, true);
    if (vmin > vmax || idx.empty()) { DROPLOG("no indices"); return; }
    const bool strip = prim == 3 || prim == 4 || prim == 6 || prim == 9;
    if (!strip && (R[0x1dac >> 2] & 1)) idx.erase(std::remove(idx.begin(), idx.end(), 0xffffffffu), idx.end());   // restart in a list: dropped
    if (idx.empty()) return;
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
    // pipeline (formats of the attributes are part of it)
    uint32_t comps[16] = {};
    for (int a = 0; a < 16; ++a) {
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        comps[a] = !((fmt >> 4) & 0xf) ? 4 : 0;   // constant attribute: 4 floats, stride 0; arrays decided below
    }
    // vertex data: cached streams or converted into the ring
    VkBuffer vb[16] = {}; VkDeviceSize voff[16] = {}, vstride[16] = {};
    long slot[16]; size_t vtxFloats = 0; int nConstAttr = 0; int constOf[16];
    float constAttr[16][4];
    for (int a = 0; a < 16; ++a) {
        slot[a] = -2; constOf[a] = -1;
        if (!(p->vpInputs & (1u << a))) continue;
        const uint32_t fmt = R[(0x1740 + 4 * a) >> 2];
        if (!((fmt >> 4) & 0xf)) {
            constAttr[nConstAttr][0] = rf(0x1c00 + 16 * a); constAttr[nConstAttr][1] = rf(0x1c04 + 16 * a); constAttr[nConstAttr][2] = rf(0x1c08 + 16 * a); constAttr[nConstAttr][3] = rf(0x1c0c + 16 * a);
            constOf[a] = nConstAttr++;
            continue;
        }
        VkDeviceSize so = 0;
        if (!sparse) if (Buffer* b = cached_stream(a, vmax + 1, so)) { vb[a] = b->buf; vstride[a] = 16; voff[a] = so + VkDeviceSize(vmin) * 16; comps[a] = 4; slot[a] = -1; continue; }
        comps[a] = ((fmt >> 8) & 0xff) ? attr_comps(a) : 4;
        slot[a] = long(vtxFloats);
        vtxFloats += size_t(nv) * comps[a];
    }
    // the pipeline for this state
    PipeKey pk;
    std::memset(&pk, 0, sizeof pk);   // (hashed whole: padding included)
    pk.prog = progKey;
    pk.blend[0] = R[0x310 >> 2] & 1; pk.blend[1] = R[0x314 >> 2]; pk.blend[2] = R[0x318 >> 2]; pk.blend[3] = R[0x320 >> 2]; pk.blend[4] = R[0x324 >> 2];
    if (!pk.blend[0]) pk.blend[1] = pk.blend[2] = pk.blend[3] = 0;
    pk.nColor = cur.nColor;
    for (uint32_t i = 0; i < cur.nColor; ++i) pk.colorFmt[i] = uint32_t(cur.colorFmt[i]);
    pk.depthFmt = uint32_t(cur.depth ? cur.depthFmt : VK_FORMAT_UNDEFINED);
    std::memcpy(pk.comps, comps, sizeof comps);
    pk.topoClass = prim == 1 ? 0 : (prim >= 2 && prim <= 4) ? 1 : 2;
    static const bool clampZ = std::getenv("RSXCLAMP") != nullptr;
    pk.clamp = clampZ;
    const uint64_t pkh = fnv(&pk, sizeof pk);
    Pipe& pipe = pipes[pkh];
    int pst = pipe.state.load(std::memory_order_acquire);
    if (!pipe.requested) {
        pipe.requested = true;
        bool known;
        { std::lock_guard<std::mutex> lk(knownMx); known = knownPipes.count(pkh) != 0; }
        static const bool sync = std::getenv("VKSYNC") != nullptr;
        Pipe* pp = &pipe;
        auto job = [pp, pk, pkh, vs = p->vs, fs = p->fs] {
            pp->pipe = create_pipeline(vs, fs, pk, pipeLayout);
            pp->state.store(pp->pipe ? 1 : 2, std::memory_order_release);
            if (pp->pipe) { std::lock_guard<std::mutex> lk(knownMx); if (knownPipes.insert(pkh).second) ++newPipelines; }
        };
        if (sync || known) { const uint64_t t0 = SDL_GetPerformanceCounter(); job(); const double ms = double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()); if (ms > 2.0) RLOG("[prog] pipeline %.1f ms (vulkan%s)\n", ms, known ? ", known" : ""); }
        else work.push(job);
        pst = pipe.state.load(std::memory_order_acquire);
    }
    while (pst == 0 && !mayskip) { SDL_Delay(1); pst = pipe.state.load(std::memory_order_acquire); }
    if (pst != 1) { if (pst == 0) ++skippedDraws; DROPLOG(pst ? "pipeline failed" : "pipeline pending"); return; }
    // Textures first (an upload may need to flush the frame), then room for everything this draw puts in the ring:
    // a flush between its allocations would record the draw against the next frame's ring (garbage vertices,
    // constants). After a race the car select uploads many textures at once and the one-shot paint layers of the
    // cars were lost that way (cars white or black).
    {
        static const long tt = std::getenv("VKTRACETGT") ? std::strtol(std::getenv("VKTRACETGT"), nullptr, 16) : -1;
        static const double tfrom = std::getenv("VKTRACEFROM") ? std::atof(std::getenv("VKTRACEFROM")) : 0;   // with VKTRACETGT=1: every draw from that second, 1 frame
        static uint64_t tframe = 0;
        if (tt == 1 && !tframe && SDL_GetTicks() >= tfrom * 1000) tframe = frameNo;
        g_traceDraw = (tt >= 0 && (long(R[0x210 >> 2]) == tt || ((R[0x220 >> 2] & 0xf) > 1 && (long(R[0x218 >> 2]) == tt || long(R[0x288 >> 2]) == tt || long(R[0x28c >> 2]) == tt)))) || (tt == 1 && tframe && frameNo == tframe);
        if (g_traceDraw) RLOG("[vktrace] frame %llu draw %llu prog %016llx tex %x verts %u blend %u mask %08x | %ux%u fmt %08x sf %08x df %08x eq %08x alpha %u/%x/%x fc0 %g %g %g %g\n", (unsigned long long)frameNo, (unsigned long long)drawsThisFrame, (unsigned long long)progKey, p->texUsed, nv, R[0x310 >> 2], R[0x324 >> 2],
                                 cur.logW, cur.logH, R[0x208 >> 2], R[0x314 >> 2], R[0x318 >> 2], R[0x320 >> 2], R[0x304 >> 2], R[0x308 >> 2], R[0x30c >> 2],
                                 fpConsts.size() > 3 ? fpConsts[0] : 0.f, fpConsts.size() > 3 ? fpConsts[1] : 0.f, fpConsts.size() > 3 ? fpConsts[2] : 0.f, fpConsts.size() > 3 ? fpConsts[3] : 0.f);
    }
    if (g_traceDraw) RLOG("[vktrace]   depth %x/%x write %x z %x stencil %x cull %x/%x scissor %08x %08x viewport %08x %08x clip %08x %08x ctl %08x shade %x\n", R[0xa74 >> 2], R[0xa6c >> 2], R[0xa70 >> 2], R[0x214 >> 2], R[0x328 >> 2], R[0x183c >> 2], R[0x1830 >> 2],
                          R[0x8c0 >> 2], R[0x8c4 >> 2], R[0xa00 >> 2], R[0xa04 >> 2], R[0x200 >> 2], R[0x204 >> 2], R[0x1d60 >> 2], R[0x368 >> 2]);
    if (g_traceDraw)
        for (int a = 0; a < 16; ++a) {
            if (!(p->vpInputs & (1u << a))) continue;
            float t[16] = {}; convert_attr(a, vmin, std::min<uint32_t>(4, vmax - vmin + 1), t, 4);
            RLOG("[vktrace]   attr %d fmt %08x slot %d: %g %g %g %g | %g %g %g %g | %g %g | %g %g\n", a, R[(0x1740 + 4 * a) >> 2], slot[a], t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[12], t[13]);
        }
    UnitBind units[16];
    for (int i = 0; i < 16; ++i) if (p->texUsed & (1u << i)) bind_texture(i, units[i]);
    g_traceDraw = false;
    {
        const VkDeviceSize ua0 = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
        const VkDeviceSize need = VkDeviceSize(vtxFloats) * 4 + VkDeviceSize(nConstAttr) * 16 + idx.size() * 4 + sizeof vpConst + sizeof(DrawCb) + kFcRange + 8 * ua0;
        if (need <= kRing && F().head + need > kRing) flush_and_wait();
    }
    // vertex data into the ring
    if (vtxFloats || nConstAttr) {
        VkDeviceSize vOff = 0;
        float* m = reinterpret_cast<float*>(ring_get(VkDeviceSize(vtxFloats) * 4 + VkDeviceSize(nConstAttr) * 16, 16, vOff));
        if (!m) { DROPLOG("ring vertices"); return; }
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
            vb[a] = F().ring.buf; vstride[a] = 4 * cc; voff[a] = vOff + VkDeviceSize(slot[a]) * 4;
        }
        float* cst = m + vtxFloats;
        for (int a = 0; a < 16; ++a) if (constOf[a] >= 0) { std::memcpy(cst + 4 * constOf[a], constAttr[constOf[a]], 16); vb[a] = F().ring.buf; vstride[a] = 0; voff[a] = vOff + VkDeviceSize(vtxFloats) * 4 + VkDeviceSize(constOf[a]) * 16; }
    }
    if (sparse) {
        if (vmap.size() < span) vmap.resize(span);
        for (uint32_t i = 0; i < nv; ++i) vmap[uniq[i] - vmin] = i;
        for (uint32_t& v : idx) if (v != 0xffffffffu) v = vmap[v - vmin];
    }
    VkDeviceSize iOff = 0;
    uint8_t* ip = ring_get(idx.size() * 4, 4, iOff);
    if (!ip) { DROPLOG("ring indices"); return; }
    std::memcpy(ip, idx.data(), idx.size() * 4);
    // uniforms: transform constants, draw values, fragment constants (each written again only when it changed)
    const VkDeviceSize ua = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
    if (vpConstDirty || vcFrame != ringGen) {
        if (uint8_t* u = ring_get(sizeof vpConst, ua, vcOff)) { std::memcpy(u, vpConst, sizeof vpConst); vpConstDirty = false; vcFrame = ringGen; }
    }
    {
        static DrawCb dc, last;
        const float vs4[4] = {rf(0xa30), rf(0xa34), rf(0xa38), rf(0xa3c)}, vo4[4] = {rf(0xa20), rf(0xa24), rf(0xa28), rf(0xa2c)};
        std::memcpy(dc.vs, vs4, 16); std::memcpy(dc.vo, vo4, 16);
        dc.surf[0] = float(cur.logW); dc.surf[1] = float(cur.logH); dc.surf[2] = dc.surf[3] = 0;
        const uint32_t af = R[0x308 >> 2];
        dc.alpha[0] = float(R[0x304 >> 2] & 1); dc.alpha[1] = float(af >= 0x200 ? af - 0x200 : 7); dc.alpha[2] = (R[0x30c >> 2] & 0xff) / 255.f; dc.alpha[3] = 0;
        dc.wpos[0] = 1.f / float(cur.aaX); dc.wpos[1] = 1.f / float(cur.aaY); dc.wpos[2] = dc.wpos[3] = 0;
        for (int i = 0; i < 16; ++i) std::memcpy(dc.tsc[i], units[i].tsc, 16);
        static const long unitLog = std::getenv("VKUNITLOG") ? std::strtol(std::getenv("VKUNITLOG"), nullptr, 16) : -1;   // (debug) texUsed mask to describe
        bool anyCube = false;
        for (int i = 0; i < 16; ++i) if ((p->texUsed & (1u << i)) && ((p->texDims >> (2 * i)) & 3) == 1) anyCube = true;
        static const long unitFrame = std::getenv("VKUNITFRAME") ? std::atol(std::getenv("VKUNITFRAME")) : -1;
        if (unitLog >= 0 && (p->texUsed == uint32_t(unitLog) || (unitLog == 0xfff && anyCube)) && (unitFrame < 0 || long(frameNo) == unitFrame)) {
            static std::set<uint64_t> seen;
            std::string s;
            char b[160];
            for (int i = 0; i < 16; ++i) if (p->texUsed & (1u << i)) {
                const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
                std::snprintf(b, sizeof b, " | u%d off %08x fmt %08x remap %08x %s", i, R[base >> 2], R[(base + 4) >> 2], R[(base + 0x10) >> 2],
                              units[i].layout == VK_IMAGE_LAYOUT_GENERAL ? "surface/cube" : units[i].view ? "texture" : "NONE");
                s += b;
            }
            if (seen.insert(progKey ^ fnv(s.data(), s.size())).second && seen.size() < 60) {
                RLOG("[vkunits] frame %llu prog %016llx consts %zu blend %u %08x %08x mask %08x targets %u%s\n", (unsigned long long)frameNo, (unsigned long long)progKey, fpConsts.size() / 4, R[0x310 >> 2] & 1, R[0x314 >> 2], R[0x318 >> 2], R[0x324 >> 2], cur.nColor, s.c_str());
            }
        }
        if (dcFrame != ringGen || std::memcmp(&dc, &last, sizeof dc) != 0) {
            if (uint8_t* u = ring_get(sizeof dc, ua, dcOff)) { std::memcpy(u, &dc, sizeof dc); last = dc; dcFrame = ringGen; }
        }
        static std::vector<float> lastFc;   // what the block at fcOff holds
        if (p->nConst && !fpConsts.empty()) {
            if (fcFrame != ringGen || lastFc != fpConsts) {
                if (uint8_t* u = ring_get(kFcRange, ua, fcOff)) { std::memcpy(u, fpConsts.data(), std::min<size_t>(fpConsts.size() * 4, size_t(kFcRange))); lastFc = fpConsts; fcFrame = ringGen; }
            }
        } else if (fcFrame != ringGen) {
            // a program without constants still needs a valid offset; the block is not filled, so it must not pass
            // for the last constants uploaded (the next draw with those constants read garbage: the car liveries
            // copied after a race came out white or black)
            VkDeviceSize o2; if (ring_get(kFcRange, ua, o2)) { fcOff = o2; fcFrame = ringGen; lastFc.clear(); }
        }
        // descriptor set: textures (the buffers are dynamic: their offsets change without a new set)
        static VkDescriptorImageInfo lastImg[16]; static VkDescriptorSet lastSet = VK_NULL_HANDLE; static uint64_t lastSetFrame = ~0ull;
        VkDescriptorImageInfo img[16];
        for (int i = 0; i < 16; ++i) {
            const bool cubeUnit = ((p->texDims >> (2 * i)) & 3) == 1;
            if (units[i].view) img[i] = {units[i].smp ? units[i].smp : linearClamp, units[i].view, units[i].layout};
            else {
                img[i] = {linearClamp, cubeUnit ? dummy.vCube : dummy.v2d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if (p->texUsed & (1u << i)) {   // a unit the shader samples has nothing usable: say so once per address
                    static std::set<uint64_t> told;
                    const uint32_t base = i < 8 ? 0x1a00 + 0x20 * i : 0x1b00 + 0x20 * (i - 8);
                    if (told.insert(uint64_t(R[base >> 2]) << 8 | uint64_t(i)).second && told.size() < 40)
                        RLOG("[vk] unit %d: no texture (offset %08x format %08x rect %08x)\n", i, R[base >> 2], R[(base + 4) >> 2], R[(base + 0x18) >> 2]);
                }
            }
        }
        if (!rendering) begin_rendering();   // (texture uploads above may have ended it)
        if (lastSetFrame != ringGen || std::memcmp(img, lastImg, sizeof img) != 0) {
            VkDescriptorSet s = alloc_set();
            if (!s) { DROPLOG("descriptor set"); return; }
            VkDescriptorBufferInfo bi[3] = {{F().ring.buf, 0, sizeof vpConst}, {F().ring.buf, 0, sizeof(DrawCb)}, {F().ring.buf, 0, kFcRange}};
            VkWriteDescriptorSet w[4]{};
            w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, 0, 0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, bi, nullptr};
            w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, 3, 0, 16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, img, nullptr, nullptr};
            vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
            lastSet = s; std::memcpy(lastImg, img, sizeof img); lastSetFrame = ringGen;
        }
        const uint32_t offs[3] = {uint32_t(vcOff), uint32_t(dcOff), uint32_t(fcOff)};
        if (bound.set != lastSet || std::memcmp(bound.offs, offs, sizeof offs) != 0) {
            vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &lastSet, 3, offs);
            bound.set = lastSet; std::memcpy(bound.offs, offs, sizeof offs);
        }
    }
    if (bound.pipe != pipe.pipe) { vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipe); bound.pipe = pipe.pipe; }
    apply_dynamic_state(prim);
    for (int a = 0; a < 16; ++a) if (vb[a]) vkCmdBindVertexBuffers2(cmd(), uint32_t(a), 1, &vb[a], &voff[a], nullptr, &vstride[a]);
    vkCmdBindIndexBuffer(cmd(), F().ring.buf, iOff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd(), uint32_t(idx.size()), 1, 0, sparse ? 0 : -int32_t(vmin), 0);
}

// ------------------------------------------------------------------ fixed passes (clear quads, present)
VkPipelineLayout fixedLayout = VK_NULL_HANDLE;   // push constants (16 floats) + one texture
VkDescriptorSetLayout fixedSetLayout = VK_NULL_HANDLE;
VkShaderModule fsVs = VK_NULL_HANDLE, clearFs = VK_NULL_HANDLE, blitFs = VK_NULL_HANDLE, overFs = VK_NULL_HANDLE, fxaaFs = VK_NULL_HANDLE, sharpFs = VK_NULL_HANDLE;
std::unordered_map<uint64_t, VkPipeline> fixedPipes;
PFN_vkCmdPushConstants vkCmdPushConstants_ = nullptr;
VkPipeline fixed_pipeline(VkShaderModule fs, uint32_t nColor, const VkFormat* fmts, VkFormat depthFmt, uint32_t writeMask, bool over = false) {
    uint64_t k[8] = {uint64_t(reinterpret_cast<uintptr_t>(fs)), nColor, writeMask | (over ? 0x100u : 0u), uint64_t(depthFmt), 0, 0, 0, 0};
    for (uint32_t i = 0; i < nColor && i < 4; ++i) k[4 + i] = uint64_t(fmts[i]);
    const uint64_t h = fnv(k, sizeof k);
    VkPipeline& p = fixedPipes[h];
    if (p) return p;
    PipeKey pk;
    std::memset(&pk, 0, sizeof pk);
    pk.nColor = nColor;
    for (uint32_t i = 0; i < nColor; ++i) pk.colorFmt[i] = uint32_t(fmts[i]);
    pk.depthFmt = uint32_t(depthFmt);
    pk.blend[4] = ((writeMask & 1) << 16) | (((writeMask >> 1) & 1) << 8) | ((writeMask >> 2) & 1) | (((writeMask >> 3) & 1) << 24);
    if (over) { pk.blend[0] = 1; pk.blend[1] = 1u | (1u << 16); pk.blend[2] = 0x303u | (0x303u << 16); pk.blend[3] = 0x8006u | (0x8006u << 16); }   // premultiplied alpha (overlays)
    pk.topoClass = 2;
    p = create_pipeline(fsVs, fs, pk, fixedLayout, false);
    return p;
}
void fixed_state(const VkRect2D& sc) {
    VkCommandBuffer c = cmd();
    vkCmdSetCullMode(c, VK_CULL_MODE_NONE); vkCmdSetFrontFace(c, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(c, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); vkCmdSetPrimitiveRestartEnable(c, VK_FALSE);
    vkCmdSetDepthTestEnable(c, VK_FALSE); vkCmdSetDepthWriteEnable(c, VK_FALSE); vkCmdSetDepthCompareOp(c, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(c, VK_FALSE); vkCmdSetDepthBiasEnable(c, VK_FALSE); vkCmdSetDepthBias(c, 0, 0, 0);
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    const float bc[4] = {0, 0, 0, 0}; vkCmdSetBlendConstants(c, bc);
    vkCmdSetScissor(c, 0, 1, &sc);
    bound = Bound{};
}

void clear_surface(uint32_t mask) {
    {
        static const long tt = std::getenv("VKTRACETGT") ? std::strtol(std::getenv("VKTRACETGT"), nullptr, 16) : -1;
        if (tt >= 0 && (long(R[0x210 >> 2]) == tt || long(R[0x218 >> 2]) == tt || long(R[0x288 >> 2]) == tt || long(R[0x28c >> 2]) == tt)) RLOG("[vktrace] frame %llu ct %x A %x B %x clear mask %x colour %08x scissor %08x %08x\n", (unsigned long long)frameNo, mask, R[0x1d90 >> 2], R[0x8c0 >> 2], R[0x8c4 >> 2]);
    }
    if (!bind_target()) return;
    const uint32_t zfmt = (R[0x208 >> 2] >> 5) & 7;
    const uint32_t c = R[0x1d90 >> 2], zv = R[0x1d8c >> 2];
    const float col[4] = {((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, (c >> 24) / 255.f};
    const float depth = zfmt == 1 ? float((zv & 0xffff) / 65535.0) : float((zv >> 8) / 16777215.0);
    const bool doColor = (mask & 0xf0) && cur.nColor, doDepth = (mask & 1) && cur.depth;
    const bool stencilFmt = cur.depthFmt == VK_FORMAT_D24_UNORM_S8_UINT || cur.depthFmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const bool doStencil = (mask & 2) && zfmt == 2 && cur.depth && stencilFmt;
    if (!doColor && !doDepth && !doStencil) return;
    const VkRect2D sr = scissor_rect();
    if (!sr.extent.width || !sr.extent.height) return;
    VkClearRect rect{sr, 0, 1};
    VkClearAttachment at[5]; uint32_t n = 0;
    if (doColor && (mask & 0xf0) == 0xf0)
        for (uint32_t k = 0; k < cur.nColor; ++k) { at[n] = {VK_IMAGE_ASPECT_COLOR_BIT, k, {}}; std::memcpy(at[n].clearValue.color.float32, col, sizeof col); ++n; }
    if (doDepth || doStencil) { at[n] = {VkImageAspectFlags((doDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (doStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, {}}; at[n].clearValue.depthStencil = {depth, zv & 0xff}; ++n; }
    if (n) vkCmdClearAttachments(cmd(), n, at, 1, &rect);
    if (doColor && (mask & 0xf0) != 0xf0) {   // some channels only: a quad with that write mask
        const uint32_t wm = ((mask >> 5) & 1) | (((mask >> 6) & 1) << 1) | (((mask >> 7) & 1) << 2) | (((mask >> 4) & 1) << 3);
        VkPipeline pl = fixed_pipeline(clearFs, cur.nColor, cur.colorFmt, cur.depth ? cur.depthFmt : VK_FORMAT_UNDEFINED, wm);
        if (!pl) return;
        vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        fixed_state(sr);
        float pc[16] = {col[0], col[1], col[2], col[3]};
        vkCmdPushConstants_(cmd(), fixedLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        vkCmdDraw(cmd(), 3, 1, 0, 0);
    }
}

// ------------------------------------------------------------------ swap chain and present
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkFormat swapFmt = VK_FORMAT_B8G8R8A8_UNORM;
std::vector<VkImage> swapImages; std::vector<VkImageView> swapViews; std::vector<VkSemaphore> swapDone;
uint32_t swapW = 0, swapH = 0;
bool swapVsync = false;
bool make_swapchain(uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(dev);
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    if (caps.currentExtent.width != 0xFFFFFFFFu) { w = caps.currentExtent.width; h = caps.currentExtent.height; }
    if (!w || !h) return false;
    uint32_t n = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n); vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());
    // without V-Sync: MAILBOX (newest image at each refresh, no tearing, like the other back ends); VKIMMEDIATE=1 tears
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsyncOn) {
        const bool imm = std::getenv("VKIMMEDIATE") != nullptr;
        for (VkPresentModeKHR m : modes) if (m == (imm ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR)) mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface; ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = swapFmt; ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; ci.imageExtent = {w, h}; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = mode; ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &ci, nullptr, &sc) != VK_SUCCESS) { RLOG("[vk] swap chain failed\n"); return false; }
    for (VkImageView v : swapViews) vkDestroyImageView(dev, v, nullptr);
    for (VkSemaphore s : swapDone) vkDestroySemaphore(dev, s, nullptr);
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = sc;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(dev, swapchain, &ni, nullptr);
    swapImages.resize(ni); vkGetSwapchainImagesKHR(dev, swapchain, &ni, swapImages.data());
    swapViews.clear(); swapDone.clear();
    for (VkImage im : swapImages) {
        swapViews.push_back(make_view(im, swapFmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore s; vkCreateSemaphore(dev, &si, nullptr, &s); swapDone.push_back(s);
    }
    swapW = w; swapH = h; swapVsync = vsyncOn;
    RLOG("[vk] swap chain %ux%u, %u images, %s\n", w, h, ni, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo (v-sync)");
    return true;
}

void begin_frame() {
    Frame& f = F();
    if (f.submitted) { vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX); f.submitted = false; }
    vkResetFences(dev, 1, &f.fence);
    for (auto& g : f.garbage) g();
    f.garbage.clear();
    for (size_t i = 0; i < f.dpoolUsed + 1 && i < f.dpools.size(); ++i) vkResetDescriptorPool(dev, f.dpools[i], 0);
    f.dpoolUsed = 0;
    f.head = 0;
    vkResetCommandPool(dev, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(f.cmd, &bi);
    recording = true; rendering = false;
    bound = Bound{};
    targetValid = false;   // the next draw begins rendering on its target again
    ++ringGen;
}
// submits what was recorded (optionally waiting / signalling the swap chain semaphores) and moves to the next frame
void submit(VkSemaphore wait, VkSemaphore signal) {
    Frame& f = F();
    end_rendering();
    vkEndCommandBuffer(f.cmd);
    const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, wait ? 1u : 0u, wait ? &wait : nullptr, wait ? &ws : nullptr, 1, &f.cmd, signal ? 1u : 0u, signal ? &signal : nullptr};
    vkQueueSubmit(queue, 1, &si, f.fence);
    f.submitted = true;
    recording = false;
    fi = (fi + 1) % kFrames;
}
void flush_and_wait() {
    if (!recording) return;
    { static int n = 0; if (n++ < 50) RLOG("[vk] ring full: frame flushed (head %llu MB)\n", (unsigned long long)(F().head >> 20)); }
    const bool wasRendering = rendering;
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(dev);
    begin_frame();
    (void)wasRendering;
}

void save_pipeline_cache() {
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, pipeCache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<uint8_t> d(n);
    if (vkGetPipelineCacheData(dev, pipeCache, &n, d.data()) != VK_SUCCESS) return;
    if (FILE* f = std::fopen("vkcache/pipelines.bin", "wb")) { std::fwrite(d.data(), 1, n, f); std::fclose(f); }
    std::lock_guard<std::mutex> lk(knownMx);
    if (FILE* f = std::fopen("vkcache/pipelines.txt", "wb")) { for (uint64_t k : knownPipes) std::fprintf(f, "%016llx\n", (unsigned long long)k); std::fclose(f); }
}

struct OverlayTex { Image im; VkImageView view = VK_NULL_HANDLE; int w = 0, h = 0; bool uploaded = false; } ovlTex[2];
Image midIm; VkImageView midView = VK_NULL_HANDLE;
VkDescriptorSet fixed_set(VkImageView view, VkImageLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, VK_NULL_HANDLE, 1, &fixedSetLayout};
    // (from the frame's pools: their set layout differs, so a small pool of their own)
    static VkDescriptorPool pools[kFrames] = {};
    static uint64_t resetAt[kFrames] = {};
    if (!pools[fi]) {
        VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 64, 1, &sz};
        vkCreateDescriptorPool(dev, &ci, nullptr, &pools[fi]);
    }
    if (resetAt[fi] != frameNo) { vkResetDescriptorPool(dev, pools[fi], 0); resetAt[fi] = frameNo; }
    ai.descriptorPool = pools[fi];
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii{linearClamp, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return s;
}
// one full-screen triangle into (view, fmt) over the rectangle, sampling src
void post_pass(VkImageView target, VkFormat fmt, uint32_t tw, uint32_t th, VkShaderModule fs, VkImageView src, VkImageLayout srcLayout, VkRect2D r, const float* params, bool over = false) {
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = target; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {tw, th}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    VkPipeline pl = fixed_pipeline(fs, 1, &fmt, VK_FORMAT_UNDEFINED, 0xf, over);
    VkDescriptorSet s = fixed_set(src, srcLayout);
    if (!pl || !s) return;
    vkCmdBeginRendering(cmd(), &ri);
    VkViewport v{float(r.offset.x), float(r.offset.y), float(r.extent.width), float(r.extent.height), 0, 1};
    vkCmdSetViewport(cmd(), 0, 1, &v);
    vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    fixed_state(r);
    vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, fixedLayout, 0, 1, &s, 0, nullptr);
    float pc[16] = {};
    if (params) std::memcpy(pc, params, 16);
    vkCmdPushConstants_(cmd(), fixedLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd(), 3, 1, 0, 0);
    vkCmdEndRendering(cmd());
}

// image -> BMP through the ring (debug captures; waits for the GPU)
// VKWATCH=hex surface,seconds (debug): from that second, the mean colour of that render target at every image when it changes
void watch_surface() {
    static long addr = -2; static double from = 0;
    if (addr == -2) { addr = -1; if (const char* e = std::getenv("VKWATCH")) { addr = std::strtol(e, nullptr, 16); if (const char* c = std::strchr(e, ',')) from = std::atof(c + 1); } }
    if (addr < 0 || SDL_GetTicks() < from * 1000) return;
    if (const char* m = std::getenv("VKWATCHMEM")) {   // hex address in RSX local memory, 512x512x4 bytes: hash when it changes
        const uint32_t ea = 0xc0000000u + uint32_t(std::strtoul(m, nullptr, 16));
        const uint64_t h = fnv(C.mem + ea, 512 * 512 * 4, 1);
        static uint64_t lastH = 0;
        if (h != lastH) { RLOG("[vkwatch] frame %llu memory %x changed (hash %016llx)\n", (unsigned long long)frameNo, ea, (unsigned long long)h); lastH = h; }
    }
    auto it = surfaces.find(uint64_t(addr));
    if (it == surfaces.end()) return;
    const uint32_t w = it->second.w, h = it->second.h;
    end_rendering(); barrier_all();
    VkDeviceSize off = 0;
    if (!ring_get(VkDeviceSize(w) * h * 4, 16, off)) return;
    VkBufferImageCopy r{}; r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd(), it->second.im.img, VK_IMAGE_LAYOUT_GENERAL, F().ring.buf, 1, &r);
    Frame& f = F();
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE); vkDeviceWaitIdle(dev);
    uint64_t sum[4] = {};
    for (uint32_t k = 0; k < w * h; ++k) for (int c = 0; c < 4; ++c) sum[c] += f.ring.mem.ptr[off + k * 4 + c];
    static uint64_t last[4] = {~0ull};
    const uint64_t n = uint64_t(w) * h;
    if (sum[0] / n != last[0] || sum[1] / n != last[1] || sum[2] / n != last[2]) {
        RLOG("[vkwatch] frame %llu draws %llu: %llu %llu %llu %llu\n", (unsigned long long)frameNo, (unsigned long long)drawsThisFrame, (unsigned long long)(sum[0] / n), (unsigned long long)(sum[1] / n), (unsigned long long)(sum[2] / n), (unsigned long long)(sum[3] / n));
        for (int c = 0; c < 4; ++c) last[c] = sum[c] / n;
    }
    begin_frame();
}
void save_image(VkImage img, VkImageLayout layout, uint32_t w, uint32_t h, bool bgra, const char* fn) {
    end_rendering();
    barrier_all();
    VkDeviceSize off = 0;
    uint8_t* p = ring_get(VkDeviceSize(w) * h * 4, 16, off);
    if (!p) return;
    VkBufferImageCopy r{};
    r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd(), img, layout, F().ring.buf, 1, &r);
    Frame& f = F();
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(dev);
    if (SDL_Surface* srf = SDL_CreateSurfaceFrom(int(w), int(h), bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888, f.ring.mem.ptr + off, int(w * 4))) { SDL_SaveBMP(srf, fn); SDL_DestroySurface(srf); }
    begin_frame();
}

void present_vk(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) {
    if (!recording) begin_frame();
    watch_surface();
    end_rendering();
    barrier_all();
    targetValid = false;
    if (uint32_t(ww) != swapW || uint32_t(wh) != swapH || vsyncOn != swapVsync) make_swapchain(uint32_t(ww), uint32_t(wh));
    auto it = surfaces.find(uint64_t(d.offset));
    {   // VKSURFSHOT=seconds (debug): every colour render target up to 512x512, once
        static const double surfAt = std::getenv("VKSURFSHOT") ? std::atof(std::getenv("VKSURFSHOT")) : -1;
        static bool surfDone = false;
        if (surfAt >= 0 && !surfDone && SDL_GetTicks() >= surfAt * 1000) {
            surfDone = true;
            for (auto& sf : surfaces) {
                if (sf.second.depth || sf.second.w > 512 || sf.second.im.fmt != VK_FORMAT_R8G8B8A8_UNORM) continue;
                char fn[96]; std::snprintf(fn, sizeof fn, "vksurf_%x.bmp", uint32_t(sf.first));
                save_image(sf.second.im.img, VK_IMAGE_LAYOUT_GENERAL, sf.second.w, sf.second.h, false, fn);
            }
        }
    }
    static const int shotEvery = std::getenv("RSXSHOT") ? std::atoi(std::getenv("RSXSHOT")) : 0;
    if (shotEvery > 0 && frameNo % uint64_t(shotEvery) == 0 && it != surfaces.end() && it->second.im.fmt == VK_FORMAT_R8G8B8A8_UNORM) {
        RLOG("[shot] frame %llu at %.2f s\n", (unsigned long long)frameNo, SDL_GetTicks() / 1000.0);
        char fn[96]; std::snprintf(fn, sizeof fn, "rsxshot_%05llu_DISPLAY_%x.bmp", (unsigned long long)frameNo, d.offset);
        save_image(it->second.im.img, VK_IMAGE_LAYOUT_GENERAL, it->second.w, it->second.h, false, fn);
        it = surfaces.find(uint64_t(d.offset));
    }
    Frame& f = F();
    uint32_t imgIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imgIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { make_swapchain(uint32_t(ww), uint32_t(wh)); ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imgIndex); }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { submit(VK_NULL_HANDLE, VK_NULL_HANDLE); return; }
    VkImage sw = swapImages[imgIndex];
    image_barrier(sw, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    {   // black background (the bars around the 16:9 image)
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = swapViews[imgIndex]; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {swapW, swapH}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd(), &ri); vkCmdEndRendering(cmd());
    }
    if (it != surfaces.end() && it->second.smp) {
        const Surface& s = it->second;
        const float sa = float(d.w ? d.w : s.w) / float(d.h ? d.h : s.h), wa = float(swapW) / float(swapH);
        int dw = int(swapW), dh = int(swapH);
        if (wa > sa) dw = int(float(swapH) * sa); else dh = int(float(swapW) / sa);
        const int dx = (int(swapW) - dw) / 2, dy = (int(swapH) - dh) / 2;
        const Settings& st = settings();
        VkImageView img = s.smp;
        if (st.fxaa) {
            if (!midIm.img || midIm.w != s.w || midIm.h != s.h) {
                if (midIm.img) { Image old = midIm; VkImageView ov = midView; later([old, ov]() mutable { vkDestroyImageView(dev, ov, nullptr); vkDestroyImage(dev, old.img, nullptr); free_mem(old.mem); }); }
                midIm = Image{};
                if (make_image(midIm, VK_FORMAT_R8G8B8A8_UNORM, s.w, s.h, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false)) {
                    midView = make_view(midIm.img, midIm.fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
                    image_barrier(midIm.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                }
            }
            if (midView) {
                const float pp[4] = {1.f / float(s.w), 1.f / float(s.h), 0, 0};
                post_pass(midView, VK_FORMAT_R8G8B8A8_UNORM, s.w, s.h, fxaaFs, s.smp, VK_IMAGE_LAYOUT_GENERAL, {{0, 0}, {s.w, s.h}}, pp);
                barrier_all();
                img = midView;
            }
        }
        static const float amount[4] = {0.f, 0.3f, 0.6f, 1.f};
        const float pp[4] = {1.f / float(s.w), 1.f / float(s.h), amount[std::clamp(st.sharpen, 0, 3)], 0};
        post_pass(swapViews[imgIndex], swapFmt, swapW, swapH, sharpFs, img, VK_IMAGE_LAYOUT_GENERAL, {{dx, dy}, {uint32_t(dw), uint32_t(dh)}}, pp);
    } else {
        static int warned = 0;
        if (warned++ < 5) RLOG("[rsx] flip: display buffer at 0x%x was never rendered\n", d.offset);
    }
    for (int k = 0; k < nOvl && k < 2; ++k) {
        const Overlay& o = ovl[k];
        if (!o.px) continue;
        OverlayTex& t = ovlTex[k];
        if (!t.im.img || t.w != o.w || t.h != o.h) {
            if (t.im.img) { OverlayTex old = t; later([old]() mutable { vkDestroyImageView(dev, old.view, nullptr); vkDestroyImage(dev, old.im.img, nullptr); free_mem(old.im.mem); }); }
            t = OverlayTex{};
            if (!make_image(t.im, VK_FORMAT_B8G8R8A8_UNORM, uint32_t(o.w), uint32_t(o.h), 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) continue;
            t.view = make_view(t.im.img, t.im.fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
            t.w = o.w; t.h = o.h;
        }
        if (o.changed || !t.uploaded) {
            VkDeviceSize off = 0;
            if (uint8_t* p = ring_get(VkDeviceSize(o.w) * o.h * 4, 16, off)) {
                std::memcpy(p, o.px, size_t(o.w) * o.h * 4);
                VkBufferImageCopy r{}; r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {uint32_t(o.w), uint32_t(o.h), 1};
                upload_regions(t.im.img, {r}, t.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                t.uploaded = true;
            }
        }
        post_pass(swapViews[imgIndex], swapFmt, swapW, swapH, overFs, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, {{o.dx, o.dy}, {uint32_t(std::max(1, o.dw)), uint32_t(std::max(1, o.dh))}}, nullptr, true);
    }
    {   // VKWINSHOT=seconds[,...]: the final window image (debug)
        static std::vector<double> at; static bool parsed = false;
        if (!parsed) { parsed = true; if (const char* e = std::getenv("VKWINSHOT")) for (const char* q = e; *q;) { at.push_back(std::atof(q)); while (*q && *q != ',') ++q; if (*q) ++q; } }
        if (!at.empty() && SDL_GetTicks() >= at.front() * 1000) {
            barrier_all();
            VkDeviceSize off = 0;
            if (ring_alloc(VkDeviceSize(swapW) * swapH * 4, 16, off)) {
                VkBufferImageCopy r{}; r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {swapW, swapH, 1};
                vkCmdCopyImageToBuffer(cmd(), sw, VK_IMAGE_LAYOUT_GENERAL, F().ring.buf, 1, &r);
                static VkDeviceSize pendingOff = 0; static int pendingFrame = -1; static char pendingName[64];
                pendingOff = off; pendingFrame = fi; std::snprintf(pendingName, sizeof pendingName, "vkwin_%05.1f.bmp", at.front());
                image_barrier(sw, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                Frame& fr = F();
                submit(f.acquired, swapDone[imgIndex]);
                vkWaitForFences(dev, 1, &fr.fence, VK_TRUE, UINT64_MAX);
                if (SDL_Surface* srf = SDL_CreateSurfaceFrom(int(swapW), int(swapH), SDL_PIXELFORMAT_ARGB8888, fr.ring.mem.ptr + pendingOff, int(swapW * 4))) { SDL_SaveBMP(srf, pendingName); SDL_DestroySurface(srf); }
                at.erase(at.begin());
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &swapDone[imgIndex], 1, &swapchain, &imgIndex, nullptr};
                vkQueuePresentKHR(queue, &pi);
                return;
            }
        }
    }
    image_barrier(sw, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    submit(f.acquired, swapDone[imgIndex]);
    const uint64_t ts = SDL_GetTicksNS();
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &swapDone[imgIndex], 1, &swapchain, &imgIndex, nullptr};
    const VkResult pr = vkQueuePresentKHR(queue, &pi);
    g_swapUs += (SDL_GetTicksNS() - ts) / 1000;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) swapW = 0;   // made again at the next present
    {   // the pipeline cache to disk now and then while new pipelines appear
        static uint64_t lastSave = 0;
        const uint64_t now = SDL_GetTicks();
        if (newPipelines.load() > 0 && now - lastSave > 20000) { lastSave = now; newPipelines = 0; save_pipeline_cache(); }
    }
    {   // [vkskip]: draws skipped while their shaders or pipelines were being built
        static uint64_t lastLog = 0, lastSkipped = 0;
        const uint64_t now = SDL_GetTicks();
        if (now - lastLog > 2000) { if (skippedDraws != lastSkipped) RLOG("[vkskip] %llu draws waited for shaders/pipelines in the last 2 s\n", (unsigned long long)(skippedDraws - lastSkipped)); lastSkipped = skippedDraws; lastLog = now; }
    }
}

// ------------------------------------------------------------------ fixed shaders (GLSL)
const char* const kFsVsSrc =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 p0, p1, p2, p3; };\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, p1.x, 1.0); }\n";
const char* const kClearFsSrc =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 p0, p1, p2, p3; };\n"
    "layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1; layout(location = 2) out vec4 o2; layout(location = 3) out vec4 o3;\n"
    "void main() { o0 = p0; o1 = p0; o2 = p0; o3 = p0; }\n";
const char* const kBlitFsSrc =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(location = 0) in vec2 uv; layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(texture(src, uv).rgb, 1.0); }\n";
const char* const kOverFsSrc =   // the overlays: premultiplied alpha kept
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(location = 0) in vec2 uv; layout(location = 0) out vec4 o;\n"
    "void main() { o = texture(src, uv); }\n";
const char* const kFxaaFsSrc =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { vec4 u_p; };\n"
    "layout(location = 0) in vec2 uv; layout(location = 0) out vec4 o;\n"
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
const char* const kSharpFsSrc =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { vec4 u_p; };\n"
    "layout(location = 0) in vec2 u; layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 rcp = u_p.xy;\n"
    "  vec3 c = texture(src, u).rgb;\n"
    "  if (u_p.z <= 0.0) { o = vec4(c, 1.0); return; }\n"
    "  vec3 n = texture(src, u + vec2(0.0, rcp.y)).rgb, s = texture(src, u - vec2(0.0, rcp.y)).rgb;\n"
    "  vec3 e = texture(src, u + vec2(rcp.x, 0.0)).rgb, w = texture(src, u - vec2(rcp.x, 0.0)).rgb;\n"
    "  vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));\n"
    "  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));\n"
    "  vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, u_p.z));\n"
    "  o = vec4(clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0), 1.0);\n"
    "}\n";

// ------------------------------------------------------------------ device
bool init_vk() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) { RLOG("[vk] no Vulkan loader: %s\n", SDL_GetError()); return false; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) return false;
#define VK_LOAD_G(name) name = reinterpret_cast<PFN_##name>(gipa(VK_NULL_HANDLE, #name));
    VK_GLOBAL(VK_LOAD_G)
#undef VK_LOAD_G
    if (!vkCreateInstance) return false;
    uint32_t apiv = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&apiv);
    if (apiv < VK_API_VERSION_1_3) { RLOG("[vk] Vulkan 1.3 needed (loader has %u.%u)\n", VK_API_VERSION_MAJOR(apiv), VK_API_VERSION_MINOR(apiv)); return false; }
    Uint32 nExt = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&nExt);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Racing Club+", 1, "Racing Club+", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, nExt, exts};
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("VKDEBUG")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        if (ici.enabledLayerCount) { RLOG("[vk] validation layer not installed: running without it\n"); ici.enabledLayerCount = 0; }
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { RLOG("[vk] vkCreateInstance failed\n"); return false; }
    }
#define VK_LOAD_I(name) name = reinterpret_cast<PFN_##name>(gipa(inst, #name)); if (!name) { RLOG("[vk] missing %s\n", #name); return false; }
    VK_INSTANCE(VK_LOAD_I)
#undef VK_LOAD_I
    if (!SDL_Vulkan_CreateSurface(win, inst, nullptr, &surface)) { RLOG("[vk] surface: %s\n", SDL_GetError()); return false; }
    // device: Vulkan 1.3, graphics + present queue; discrete GPUs first
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, nullptr);
    std::vector<VkPhysicalDevice> devs(nd); vkEnumeratePhysicalDevices(inst, &nd, devs.data());
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < nd; ++i) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf.data());
        for (uint32_t q = 0; q < nq; ++q) {
            VkBool32 present = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
                if (const char* want = std::getenv("GPU")) {   // GPU=text (test): the device whose name contains it
                    std::string n(p.deviceName), w(want);
                    for (auto& ch : n) ch = char(std::tolower(uint8_t(ch)));
                    for (auto& ch : w) ch = char(std::tolower(uint8_t(ch)));
                    if (n.find(w) != std::string::npos) score = 10;
                }
                if (score > bestScore) { bestScore = score; best = int(i); queueFamily = q; }
                break;
            }
        }
    }
    if (best < 0) { RLOG("[vk] no GPU with Vulkan 1.3 and presentation\n"); return false; }
    phys = devs[size_t(best)];
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    std::snprintf(gpuName, sizeof gpuName, "%s", props.deviceName);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> de(ne); vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, de.data());
    auto hasExt = [&](const char* n) { for (auto& e : de) if (!std::strcmp(e.extensionName, n)) return true; return false; };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    const bool hasCbc = hasExt(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (hasCbc) f13.pNext = &cbf;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    if (!f13.dynamicRendering) { RLOG("[vk] dynamic rendering not supported\n"); return false; }
    customBorder = hasCbc && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    // what is enabled
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    cbe.customBorderColors = VK_TRUE; cbe.customBorderColorWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    if (customBorder) e13.pNext = &cbe;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e13};
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.textureCompressionBC = f2.features.textureCompressionBC;
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.shaderImageGatherExtended = f2.features.shaderImageGatherExtended;
    std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (customBorder) dext.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queueFamily, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2, 0, 1, &qci, 0, nullptr, uint32_t(dext.size()), dext.data(), nullptr};
    if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) { RLOG("[vk] vkCreateDevice failed\n"); return false; }
#define VK_LOAD_D(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name)); if (!name) { RLOG("[vk] missing %s\n", #name); return false; }
    VK_DEVICE(VK_LOAD_D)
#undef VK_LOAD_D
    vkCmdPushConstants_ = reinterpret_cast<PFN_vkCmdPushConstants>(vkGetDeviceProcAddr(dev, "vkCmdPushConstants"));
    if (!vkCmdPushConstants_) return false;
    vkGetDeviceQueue(dev, queueFamily, 0, &queue);
    {   // D24S8 is missing on AMD: D32S8 then
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    {   // swap chain format
        uint32_t n = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> sf(n); vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, sf.data());
        bool found = false;
        for (auto& f : sf) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) found = true;
        if (!found && n) swapFmt = sf[0].format;
    }
    // frames
    for (Frame& f : frames) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queueFamily};
        vkCreateCommandPool(dev, &pci, nullptr, &f.pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, f.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        vkAllocateCommandBuffers(dev, &cai, &f.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &sci, nullptr, &f.acquired);
        if (!make_buffer(f.ring, kRing, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) || !f.ring.mem.ptr) { RLOG("[vk] ring buffer failed\n"); return false; }
    }
    // descriptor and pipeline layouts
    {
        VkDescriptorSetLayoutBinding b[19];
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (uint32_t k = 0; k < 16; ++k) b[3 + k] = {3 + k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 19, b};
        vkCreateDescriptorSetLayout(dev, &ci, nullptr, &setLayout);
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &setLayout, 0, nullptr};
        vkCreatePipelineLayout(dev, &pl, nullptr, &pipeLayout);
        VkDescriptorSetLayoutBinding fb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo fci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &fb};
        vkCreateDescriptorSetLayout(dev, &fci, nullptr, &fixedSetLayout);
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo fpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &fixedSetLayout, 1, &pr};
        vkCreatePipelineLayout(dev, &fpl, nullptr, &fixedLayout);
    }
    // pipeline cache from the last session
    SDL_CreateDirectory("vkcache");
    {
        std::vector<uint8_t> data;
        if (FILE* f = std::fopen("vkcache/pipelines.bin", "rb")) {
            std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
            if (n > 0) { data.resize(size_t(n)); if (std::fread(data.data(), 1, size_t(n), f) != size_t(n)) data.clear(); }
            std::fclose(f);
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, data.size(), data.empty() ? nullptr : data.data()};
        if (vkCreatePipelineCache(dev, &ci, nullptr, &pipeCache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = nullptr; vkCreatePipelineCache(dev, &ci, nullptr, &pipeCache); }
        if (!data.empty())
            if (FILE* f = std::fopen("vkcache/pipelines.txt", "rb")) { unsigned long long k; while (std::fscanf(f, "%llx", &k) == 1) knownPipes.insert(uint64_t(k)); std::fclose(f); }
    }
    {   // samplers
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; ci.maxLod = 0;
        vkCreateSampler(dev, &ci, nullptr, &linearClamp);
        vkCreateSampler(dev, &ci, nullptr, &cubeSampler);
    }
    // fixed shaders
    fsVs = shader_module(kFsVsSrc, EShLangVertex, "fixed vertex");
    clearFs = shader_module(kClearFsSrc, EShLangFragment, "clear");
    blitFs = shader_module(kBlitFsSrc, EShLangFragment, "blit");
    overFs = shader_module(kOverFsSrc, EShLangFragment, "overlay");
    fxaaFs = shader_module(kFxaaFsSrc, EShLangFragment, "fxaa");
    sharpFs = shader_module(kSharpFsSrc, EShLangFragment, "sharpen");
    if (!fsVs || !clearFs || !blitFs || !overFs || !fxaaFs || !sharpFs) { RLOG("[vk] fixed shaders failed\n"); return false; }
    vsyncOn = settings().vsync != 0;
    int ww = 1280, wh = 720;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    if (!make_swapchain(uint32_t(ww), uint32_t(wh))) return false;
    // dummy textures (units the shader declares but the game left without a texture)
    begin_frame();
    if (!make_image(dummy.im2d, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false) ||
        !make_image(dummy.imCube, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true)) return false;
    dummy.v2d = make_view(dummy.im2d.img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    dummy.vCube = make_view(dummy.imCube.img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
    {
        VkDeviceSize off = 0;
        uint8_t* p = ring_alloc(64, 16, off);
        std::memset(p, 0, 64);
        std::vector<VkBufferImageCopy> r1(1), r6(6);
        r1[0].bufferOffset = off; r1[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r1[0].imageExtent = {1, 1, 1};
        for (uint32_t k = 0; k < 6; ++k) { r6[k] = r1[0]; r6[k].imageSubresource.baseArrayLayer = k; }
        upload_regions(dummy.im2d.img, r1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        upload_regions(dummy.imCube.img, r6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (dumpShaders) SDL_CreateDirectory("rsxdump");
    RLOG("[rsx] Vulkan %u.%u on %s (driver %u.%u), depth %s%s\n", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), gpuName,
         VK_API_VERSION_MAJOR(props.driverVersion), VK_API_VERSION_MINOR(props.driverVersion), depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         customBorder ? ", custom border colours" : "");
    devOk = true;
    return true;
}

} // namespace

struct VulkanBackend final : Backend {
    uint32_t window_flags() override { return SDL_WINDOW_VULKAN; }
    bool init() override { return init_vk(); }
    void new_frame() override {}
    void clear(uint32_t mask) override { if (devOk) clear_surface(mask); }
    void draw() override { do_draw(); }
    void present(const Display& d, int ww, int wh, const Overlay* ovl, int nOvl) override { if (devOk) present_vk(d, ww, wh, ovl, nOvl); }
    void set_vsync(bool on) override { vsyncOn = on; }
    void textures_changed() override {}
    const char* gpu_name() override { return gpuName; }
    const char* api_name() override { return "Vulkan"; }
};
Backend* make_vulkan_backend() { static VulkanBackend b; return &b; }

} // namespace gti::recomp::rsx
