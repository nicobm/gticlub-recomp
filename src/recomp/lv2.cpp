// PS3 system layer for the whole recompiled game: lv2 syscalls and system library imports, on host threads.
#include "recomp/lv2.h"
#include "recomp/settings.h"
#include "recomp/rsx_gl.h"
#include "recomp/spu_rt.h"
#include "recomp/edge_cull.h"
#include "recomp/edge_native.h"
#include "recomp/hle.h"
#include "recomp/spu.h"
#include "recomp/atrac3.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <zlib.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gti::recomp::netplay { bool in_room(); }
namespace gti::recomp::netplay { struct Callback { uint32_t opd; int nargs; uint64_t a[8]; }; bool enabled(); bool hle(PPU& c, const char* module, const std::string& n); void set_errno_addr(uint32_t a); std::vector<Callback> take_callbacks(); }   // netplay.cpp
namespace gti::recomp { void glyphs_patch_read(const std::string& hostPath, uint64_t fileOff, uint8_t* dst, size_t n); }   // button_glyphs.cpp
namespace gti::recomp { bool host_camera_available(int choice); bool host_camera_start(int choice); void host_camera_stop(); uint32_t host_camera_read(uint8_t* dst, uint32_t cap, int format, int w, int h, uint32_t& frame); }   // host_camera.cpp
namespace gti::recomp { void ripprof_register_thread(const char* name); void host_sleep_us(uint64_t us); void host_thread_priority_high(); void host_thread_core(bool own); void host_timer_init(); int host_display_hz(); bool host_set_refresh(int hz); void host_restore_refresh(); bool host_wait_refresh(); }   // ripprof.cpp, host_win.cpp   // ripprof.cpp
namespace gti::recomp {

extern uint64_t g_missLr;

namespace {

Guest* G = nullptr;
Lv2Config CFG;
std::mutex g_log;

const bool g_logTime = std::getenv("LOGTIME") != nullptr;   // LOGTIME=1: every log line starts with the seconds since launch
double log_seconds();
#define LOG(...) do { std::lock_guard<std::mutex> lk_(g_log); if (g_logTime) std::fprintf(stderr, "%8.3f ", log_seconds()); std::fprintf(stderr, __VA_ARGS__); } while (0)

// ------------------------------------------------------------------ guest memory allocation
std::mutex g_memMx;
uint32_t g_userNext = 0x30000000;    // sys_memory_allocate / mmapper (PS3 user memory area)
uint32_t g_stackNext = 0xD0000000;   // thread stacks
uint32_t g_heapNext = 0x60000000;    // small host-side allocations (TLS blocks, strings)

uint32_t alloc_user(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lk(g_memMx);
    g_userNext = (g_userNext + align - 1) & ~(align - 1);
    const uint32_t a = g_userNext;
    g_userNext += size;
    return a;
}
uint32_t alloc_heap(uint32_t size, uint32_t align = 16) {
    std::lock_guard<std::mutex> lk(g_memMx);
    g_heapNext = (g_heapNext + align - 1) & ~(align - 1);
    const uint32_t a = g_heapNext;
    g_heapNext += size ? size : 1;
    return a;
}
uint32_t alloc_stack(uint32_t size) {
    std::lock_guard<std::mutex> lk(g_memMx);
    size = (size + 0xFFFF) & ~0xFFFFu;
    const uint32_t lo = g_stackNext;
    g_stackNext += size + 0x10000;   // guard gap
    return lo + size;                // stack top
}

// ------------------------------------------------------------------ TLS
uint32_t g_tlsImage = 0, g_tlsFile = 0, g_tlsMem = 0;
uint32_t make_tls() {
    const uint32_t blk = alloc_heap(0x30 + g_tlsMem + 0x10, 0x80);
    std::memset(G->mem + blk, 0, 0x30 + g_tlsMem);
    if (g_tlsFile) std::memcpy(G->mem + blk + 0x30, G->mem + g_tlsImage, g_tlsFile);
    return blk + 0x7030;   // r13
}

// ------------------------------------------------------------------ threads
struct ThreadExit { uint64_t value; };
struct ProcessExit { int code; };

struct GThread {
    uint32_t id = 0;
    std::string name;
    std::thread th;
    uint64_t exitValue = 0;
    std::atomic<bool> done{false};
};
std::mutex g_thMx;
std::map<uint32_t, std::shared_ptr<GThread>> g_threads;
uint32_t g_nextTid = 0x01000100;
thread_local uint32_t t_tid = 0;
std::atomic<uint32_t> g_rumble{0};   // last cellPadSetActDirect: small motor << 8 | large motor
const bool g_loadLog = std::getenv("LOADLOG") != nullptr;   // LOADLOG=1: slow file reads and inflates, with the thread that did them
std::atomic<bool> g_exit{false};
std::atomic<int> g_exitCode{0};

void run_thread(std::shared_ptr<GThread> t, uint32_t entryOpd, uint64_t arg, uint32_t stackSize) {
    t_tid = t->id;
    ripprof_register_thread(t->name.c_str());
    PPU c;
    G->prepare(c);
    c.tid = t->id;
    c.r[1] = alloc_stack(stackSize ? stackSize : 0x10000) - 0x70;
    c.r[2] = c.rd32(entryOpd + 4);
    c.r[3] = arg;
    c.r[13] = make_tls();
    c.lr = 0;
    const uint32_t fn = c.rd32(entryOpd);
    try {
        rc_call(c, fn);
        t->exitValue = c.r[3];
    } catch (const ThreadExit& e) {
        t->exitValue = e.value;
    } catch (const ProcessExit& e) {
        g_exitCode = e.code;
        g_exit = true;
    }
    if (c.missing || c.unimplAt)
        LOG("[lv2] thread '%s' stopped: missing call 0x%x, unimplemented instruction at 0x%x\n", t->name.c_str(), c.missing, c.unimplAt);
    t->done = true;
}

// ------------------------------------------------------------------ synchronisation objects
struct HostMutex {
    std::recursive_timed_mutex m;
    std::condition_variable_any cv;
    uint32_t owner = 0;
    int count = 0;
};
std::mutex g_objMx;
std::map<uint32_t, std::shared_ptr<HostMutex>> g_lwmutex;   // by guest address of sys_lwmutex_t
std::map<uint32_t, std::shared_ptr<HostMutex>> g_mutexById;  // sys_mutex ids
std::map<uint32_t, uint32_t> g_condMutex;                    // sys_cond id -> sys_mutex id
std::map<uint32_t, std::shared_ptr<std::condition_variable_any>> g_conds;
uint32_t g_nextId = 0x10;

std::shared_ptr<HostMutex> lwm(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_objMx);
    auto& p = g_lwmutex[addr];
    if (!p) p = std::make_shared<HostMutex>();
    return p;
}
bool mlock(HostMutex& m, uint64_t timeoutUs) {
    if (timeoutUs == 0) m.m.lock();
    else if (!m.m.try_lock_for(std::chrono::microseconds(timeoutUs))) return false;
    m.owner = t_tid;
    ++m.count;
    return true;
}
void munlock(HostMutex& m) {
    if (--m.count == 0) m.owner = 0;
    m.m.unlock();
}
// waits on cv releasing every recursion level of m, then restores them
bool mwait(HostMutex& m, std::condition_variable_any& cv, uint64_t timeoutUs) {
    const int n = m.count;
    for (int k = 1; k < n; ++k) m.m.unlock();
    m.count = 0;
    m.owner = 0;
    bool ok = true;
    if (timeoutUs) ok = cv.wait_for(m.m, std::chrono::microseconds(timeoutUs)) == std::cv_status::no_timeout;
    else cv.wait(m.m);
    for (int k = 1; k < n; ++k) m.m.lock();
    m.count = n;
    m.owner = t_tid;
    return ok;
}

// ------------------------------------------------------------------ time
const auto g_t0 = std::chrono::steady_clock::now();
uint64_t now_us() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count()) + 1000000; }
double log_seconds() { return double(now_us() - 1000000) / 1e6; }

// ------------------------------------------------------------------ files
std::string cstr(PPU& c, uint32_t a) { return a ? std::string(reinterpret_cast<const char*>(c.mem + a)) : std::string(); }
std::string host_path(const std::string& p) {
    auto starts = [&](const char* pre) { return p.rfind(pre, 0) == 0; };
    std::string rest;
    if (starts("/app_home/")) rest = CFG.usrdir + "/" + p.substr(10);
    else if (starts("/dev_hdd0/game/NPEB00060/USRDIR/")) rest = CFG.usrdir + "/" + p.substr(32);
    else if (starts("/dev_hdd0/")) rest = CFG.hdd0 + "/" + p.substr(10);
    else if (starts("/dev_flash/")) rest = "dev_flash/" + p.substr(11);   // optional PS3 system files (flashATRAC.pic: the music decoder) next to the executable
    else if (starts("/dev_bdvd/PS3_GAME/USRDIR/")) rest = CFG.usrdir + "/" + p.substr(26);
    else rest = CFG.hdd0 + "/misc" + p;
    return rest;
}
struct GFile { FILE* f = nullptr; std::string path; };
std::mutex g_fsMx;
std::map<uint32_t, GFile> g_files;
struct Dir { std::vector<std::pair<std::string, bool>> list; size_t pos = 0; };
std::map<uint32_t, Dir> g_dirs;
uint32_t g_nextFd = 3;

// ------------------------------------------------------------------ helpers
void ret(PPU& c, uint64_t v) { c.r[3] = v; }
uint32_t a32(PPU& c, int i) { return uint32_t(c.r[3 + i]); }
constexpr uint32_t CELL_OK = 0, LV2_EBUSY = 0x8001000A, LV2_ETIMEDOUT = 0x8001000B, LV2_ENOENT = 0x80010006, LV2_ESRCH = 0x80010005;

// ------------------------------------------------------------------ pad
std::mutex g_padMx;
PadState g_pad;
bool g_padLive = false;
PadState pad_state() {
    {
        std::lock_guard<std::mutex> lk(g_padMx);
        if (g_padLive) return g_pad;
    }
    PadState s;
    const double t = double(now_us() - 1000000) / 1e6;
    size_t p0 = 0;
    const std::string& sc = CFG.padScript;
    while (p0 < sc.size()) {   // "button:seconds,..."
        size_t p1 = sc.find(',', p0); if (p1 == std::string::npos) p1 = sc.size();
        const std::string item = sc.substr(p0, p1 - p0);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const std::string b = item.substr(0, colon);
            const double at = std::atof(item.c_str() + colon + 1);
            if (t >= at && t < at + 0.25) {
                if (b == "start") s.digital1 |= 0x08; else if (b == "select") s.digital1 |= 0x01;
                else if (b == "up") s.digital1 |= 0x10; else if (b == "right") s.digital1 |= 0x20;
                else if (b == "down") s.digital1 |= 0x40; else if (b == "left") s.digital1 |= 0x80;
                else if (b == "cross") s.digital2 |= 0x40; else if (b == "circle") s.digital2 |= 0x20;
                else if (b == "square") s.digital2 |= 0x80; else if (b == "triangle") s.digital2 |= 0x10;
                else if (b == "r2") s.digital2 |= 0x02; else if (b == "l2") s.digital2 |= 0x01;
                else if (b == "l3") s.digital1 |= 0x02; else if (b == "r3") s.digital1 |= 0x04;
            }
        }
        p0 = p1 + 1;
    }
    return s;
}

// ------------------------------------------------------------------ GCM / RSX (first stage: command processor without drawing)
// The game writes RSX commands into its command buffer (main memory mapped as RSX "IO" space) and moves the PUT
// pointer of the control register; this processor consumes them, follows jumps/calls and performs the
// synchronisation methods (reference, semaphores/labels, flips) the game waits on.
namespace gcm {
constexpr uint32_t LOCAL_ADDR = 0xC0000000, LOCAL_SIZE = 0x0F900000;
constexpr uint32_t CB_ADDR = 0x0F000000;   // host callback (command buffer full) reached through a fake OPD
uint32_t ioAddr = 0, ioSize = 0, ctx = 0, ctrl = 0, labels = 0, cbOpd = 0;
uint32_t flipHandler = 0, vblankHandler = 0, userHandler = 0;
std::atomic<bool> started{false};
std::atomic<uint64_t> flips{0};
std::atomic<uint64_t> g_rsxBegins{0};   // draws the RSX executed (SET_BEGIN_END with a primitive)
std::atomic<bool> flipPending{false};   // a flip command was executed; the flip itself happens at the next vblank
const bool g_flipNow = std::getenv("FLIPNOW") != nullptr;
const bool g_flipLate = std::getenv("FLIPLATE") != nullptr;   // old behaviour: every flip waits for the next vblank
// The game paces its frames itself: the render thread writes label 0x81 = 1, the vblank handler turns it into 2
// and the command stream waits for that 2 right before the flip. Such a flip is already aligned to a vblank and
// is shown at once; delaying it to the next vblank would halve the frame rate.
bool g_vblankSynced = false;
// The flip handler tells the render thread that the frame was shown, and the render thread then starts rewriting
// that frame's command buffer. A few commands of the frame still follow the flip (counters, the jump into the other
// buffer): the handler runs only after the FIFO has executed them (at its next jump or when it runs dry), or a late
// FIFO thread would read commands of the next frame. The console's RSX is never late; a host thread can be.
uint64_t g_profSem = 0, g_profFlip = 0, g_profClear = 0;   // FIFOPROF: label waits, flips, clears (us)
// FRAMELOG=n: timeline (ms) of 40 frames starting at flip n: V vblank (label 0x81 before>after), P put moved,
// S label wait starts, E label wait ends, F flip, D flip handler delivered
long g_flStart = std::getenv("FRAMELOG") ? std::atol(std::getenv("FRAMELOG")) : -1;
void fl(const char* tag, uint32_t v = 0);
uint64_t g_frHoleUs = 0, g_frIdleUs = 0, g_frHoles = 0;
// MISSLOG=1: every frame whose FIFO reached the vblank wait later than one refresh after the flip, with what it did
const bool g_missLog = std::getenv("MISSLOG") != nullptr;
const bool g_frAcct = g_flStart >= 0 || g_missLog;   // FRAMELOG: time the FIFO waited at holes / for commands in this frame
std::atomic<uint32_t> g_vblankTick{0};   // bumped (and waiters woken) after every vblank handler call
// A frame whose commands end a moment after a vblank used to wait for the next one: at 144 Hz a frame that needed
// 7.2 ms instead of 6.9 took 13.9, and a scene on the edge fell to 72-100 images per second. When the RSX reaches the
// vblank wait within g_lateUs of a vblank that no frame used, that vblank still counts (its handler runs again now);
// the image is shown at the next refresh as it would be anyway. VBLANKLATE=us (0 = off).
std::atomic<uint64_t> g_lastVblankUs{0};
std::atomic<bool> g_vblankFree{false};   // the last vblank found no frame waiting for it
std::atomic<uint64_t> g_lateUsed{0};
const uint64_t g_lateUs = std::getenv("VBLANKLATE") ? std::strtoull(std::getenv("VBLANKLATE"), nullptr, 10) : 2500;
bool g_flipDue = false;
uint64_t g_flipDueSince = 0;
void call_guest(uint32_t opd, uint64_t arg);
uint64_t g_tDeliver = 0, g_tPut = 0, g_mBuild = 0, g_mRsx = 0, g_mLate = 0, g_mN = 0;   // [margin] statistics
void deliver_flip() { if (g_flipDue) { g_flipDue = false; call_guest(flipHandler, 1); g_tDeliver = now_us(); fl("D"); } }
std::atomic<uint64_t> g_lbl400{0};   // RSX writes of label 0x400 (Edge output ring position)
std::mutex callMx;   // guest handler calls from the RSX threads

uint32_t io2ea(uint32_t off) { return ioAddr + off; }
uint32_t rd(uint32_t a) { return __builtin_bswap32(__atomic_load_n(reinterpret_cast<uint32_t*>(G->mem + a), __ATOMIC_SEQ_CST)); }
void wr(uint32_t a, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(G->mem + a), __builtin_bswap32(v), __ATOMIC_SEQ_CST); }

void fl(const char* tag, uint32_t v) {
    const long f = long(flips.load());
    if (g_flStart < 0 || f < g_flStart || f > g_flStart + 40) return;
    static std::mutex m; static std::string buf; static uint64_t t0 = 0;
    std::lock_guard<std::mutex> lk(m);
    const uint64_t t = now_us();
    if (!t0) t0 = t;
    char b[48]; std::snprintf(b, sizeof b, " %s%.1f", tag, (t - t0) / 1000.0); buf += b;
    if (v) { std::snprintf(b, sizeof b, "(%x)", v); buf += b; }
    if (tag[0] == 'F' || buf.size() > 600) { LOG("[fl]%s\n", buf.c_str()); buf.clear(); }
}
void call_guest(uint32_t opd, uint64_t arg) {   // runs a guest callback (flip / vblank / user handler) on this host thread
    if (!opd) return;
    std::lock_guard<std::mutex> lk(callMx);
    static thread_local uint32_t stack = 0;
    if (!stack) stack = alloc_stack(0x10000);
    PPU c;
    G->prepare(c);
    c.tid = t_tid;
    c.r[1] = stack - 0x70;
    c.r[2] = c.rd32(opd + 4);
    c.r[3] = arg;
    c.r[13] = make_tls();
    c.lr = 0;
    rc_call(c, c.rd32(opd));
}

std::atomic<uint64_t> g_mhist[0x4000];   // METHODHIST=1: calls per method (dumped by lv2_dump_waits)
std::atomic<uint32_t> g_mlast[0x4000];
void method(uint32_t m, uint32_t arg, uint32_t& semOff406e, uint32_t& semOff4097) {
    static const bool hist = std::getenv("METHODHIST") != nullptr;
    if (hist && m < 0x10000) { ++g_mhist[m >> 2]; g_mlast[m >> 2] = arg; }
    if (m < 0x2000) rsx::method(m, arg);
    static int shown = 0;
    if ((m < 0x100 || (m >= 0x1d6c && m <= 0x1d74) || m >= 0xE000) && shown < 300) { ++shown; LOG("[rsx] method %04x = %08x\n", m, arg); }   // sync-related methods (debug)
    switch (m) {
        case 0x0050: wr(ctrl + 8, arg); break;                       // NV406E_SET_REFERENCE
        case 0x0064: semOff406e = arg; break;                        // NV406E_SEMAPHORE_OFFSET
        case 0x0068: {                                               // NV406E_SEMAPHORE_ACQUIRE: wait for the label
            const uint64_t ts = now_us();
            if (semOff406e == 0x810 && g_tDeliver) {   // frame margin: flip -> game published the frame -> FIFO reached the vblank wait
                const uint64_t build = g_tPut > g_tDeliver ? g_tPut - g_tDeliver : 0, rsxT = ts - g_tDeliver;
                g_mBuild = std::max(g_mBuild, build); g_mRsx = std::max(g_mRsx, rsxT); if (rsxT > 15500) ++g_mLate;
                if (++g_mN >= 240) {
                    LOG("[margin] last %llu frames: game published the frame up to %.1f ms after the flip, the RSX finished up to %.1f ms after it; %llu frames past 15.5 ms\n",
                        (unsigned long long)g_mN, g_mBuild / 1000.0, g_mRsx / 1000.0, (unsigned long long)g_mLate);
                    g_mBuild = g_mRsx = g_mLate = g_mN = 0;
                }
            }
            if (semOff406e == 0x810 && g_missLog && g_tDeliver) {
                static rsx::Counters c0{}; static bool have = false;
                const rsx::Counters c1 = rsx::counters();
                static const uint64_t period = 1000000 / uint64_t(settings().gameHz > 0 ? settings().gameHz : 60);
                const uint64_t rsxT = ts - g_tDeliver, build = g_tPut > g_tDeliver ? g_tPut - g_tDeliver : 0;
                if (have && rsxT > period)
                    LOG("[miss] build %.1f rsx %.1f | holes %.1f (%llu) idle %.1f | draws %llu in %.1f ms, swap %.1f, progs %llu (%.1f ms), tex %llu (%llu KB, %.1f ms), streams %llu (%llu vertices, convert %.1f ms, buffers %.1f ms)\n",
                        build / 1000.0, rsxT / 1000.0, g_frHoleUs / 1000.0, (unsigned long long)g_frHoles, g_frIdleUs / 1000.0,
                        (unsigned long long)(c1.draws - c0.draws), (c1.drawUs - c0.drawUs) / 1000.0, (c1.swapUs - c0.swapUs) / 1000.0,
                        (unsigned long long)(c1.progs - c0.progs), (c1.progUs - c0.progUs) / 1000.0, (unsigned long long)(c1.texUploads - c0.texUploads),
                        (unsigned long long)((c1.texBytes - c0.texBytes) >> 10), (c1.texUs - c0.texUs) / 1000.0, (unsigned long long)(c1.streams - c0.streams), (unsigned long long)(c1.streamVerts - c0.streamVerts), (c1.convUs - c0.convUs) / 1000.0, (c1.bufUs - c0.bufUs) / 1000.0);
                c0 = c1; have = true;
            }
            if (semOff406e == 0x810) { fl("h", uint32_t(g_frHoleUs / 100)); fl("n", uint32_t(g_frHoles)); fl("i", uint32_t(g_frIdleUs / 100)); fl("S"); g_frHoleUs = g_frIdleUs = g_frHoles = 0; }
            if (semOff406e == 0x810) {   // waits for the vblank handler: woken by the vblank thread, not by polling
                if (rd(labels + semOff406e) != arg && g_lateUs && g_vblankFree.exchange(false) && now_us() - g_lastVblankUs.load() < g_lateUs) {
                    call_guest(vblankHandler, 1); ++g_lateUsed; fl("L");
                }
                bool dumped = false;
                for (;;) {
                    const uint32_t tick = g_vblankTick.load();
                    if (rd(labels + semOff406e) == arg || now_us() - ts > 2000000) break;
                    if (g_missLog && !dumped && now_us() - ts > 20000) { dumped = true; LOG("[late] the RSX has waited %.1f ms for the game to release the vblank label:\n", (now_us() - ts) / 1000.0); lv2_dump_waits(); }
                    g_vblankTick.wait(tick);
                }
                if (dumped) LOG("[late] waited %.1f ms in all\n", (now_us() - ts) / 1000.0);
            } else
                for (const uint64_t until = ts + 2000000; rd(labels + semOff406e) != arg && now_us() < until;) std::this_thread::yield();
            g_profSem += now_us() - ts;
            if (semOff406e == 0x810) { g_vblankSynced = true; fl("E"); }
            break;
        }
        case 0x006c: wr(labels + semOff406e, arg); break;            // NV406E_SEMAPHORE_RELEASE
        case 0x1d6c: semOff4097 = arg; break;                        // NV4097_SET_SEMAPHORE_OFFSET
        case 0x1d70: if (semOff4097 == 0x400) ++g_lbl400; wr(labels + semOff4097, (arg & 0xff00ff00u) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff)); break;   // BACK_END_WRITE_SEMAPHORE_RELEASE
        case 0x1d74: if (semOff4097 == 0x400) ++g_lbl400; wr(labels + semOff4097, arg); break;            // TEXTURE_READ_SEMAPHORE_RELEASE
        case 0xFEAC:                                                 // flip (written by _cellGcmSetFlipCommand)
            if (g_flipNow || (g_vblankSynced && !g_flipLate)) {
                g_vblankSynced = false;
                while (flipPending.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));   // an earlier unsynced flip
                { const uint64_t tf = now_us(); rsx::flip(arg); g_profFlip += now_us() - tf; }
                ++flips; g_flipDue = true; g_flipDueSince = now_us(); fl("F");
                {   // RINGLOG=1: bytes of Edge output each SPU ring took per frame (largest of the last 2 s)
                    static const bool ringLog = std::getenv("RINGLOG") != nullptr;
                    if (ringLog) {
                        static uint32_t last[3], peak[3]; static int n = 0;
                        for (int r = 0; r < 3; ++r) {
                            const uint32_t e = 0x01061980u + 0x80u * r, cur = rd(e + 12), size = rd(e + 8) - rd(e);
                            const uint32_t used = size ? (cur + size - last[r]) % size : 0;
                            if (last[r]) peak[r] = std::max(peak[r], used);
                            last[r] = cur;
                        }
                        if (++n % 120 == 0) { LOG("[ring] flip %llu: peak Edge output per frame %u / %u / %u KB (ring 3072 KB)\n", (unsigned long long)flips.load(), peak[0] >> 10, peak[1] >> 10, peak[2] >> 10); peak[0] = peak[1] = peak[2] = 0; }
                    }
                }
                break;
            }
            // vsync flip mode: the RSX stalls on a second flip until the first one was shown at a vblank
            rsx::flip(arg);
            while (flipPending.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));
            flipPending = true;
            break;
        case 0xFEB0: call_guest(userHandler, arg); break;            // user command
        default: break;
    }
}

uint32_t g_lastHole = 0;
const int g_holeWaitUs = std::getenv("HOLEWAIT") ? std::atoi(std::getenv("HOLEWAIT")) : (std::getenv("NOJOBS") ? 0 : 200000);   // how long a geometry hole may stay closed
// True when `steps` consecutive command words starting at IO offset `o` decode as valid RSX headers
// (method headers, NOPs, jumps/calls into the IO area, returns); used to resynchronise after a hole.
bool plausible_stream(uint32_t o, uint32_t put, int steps) {
    bool sawHeader = false;
    for (int k = 0; k < steps; ++k) {
        if (o == put) return sawHeader;
        const uint32_t w = rd(io2ea(o));
        if (w == 0) { o += 4; --k; if (k < -64) return false; continue; }   // NOPs do not count as evidence
        if ((w & 0xE0000003) == 0x20000000 || (w & 3) == 1 || (w & 3) == 2) {
            const uint32_t t = (w & 3) ? (w & 0xFFFFFFFC) : (w & 0x1FFFFFFC);
            return t >= 0x20000 && t < ioSize && sawHeader;   // the game never branches into the initial 128 KB buffer
        }
        if (w == 0x00020000) return sawHeader;
        if (w & 0xA0030003) return false;
        const uint32_t count = (w >> 18) & 0x7ff, m = w & 0x1ffc, sub = (w >> 13) & 7;
        if (count == 0 || (sub == 0 && m < 0x40)) return false;
        sawHeader = true;
        o += 4 + 4 * count;
    }
    return true;
}

// FIFOPROF=1: where the FIFO thread spends its time (ms per second): idle, waiting at holes, executing methods
uint64_t g_profIdle = 0, g_profHole = 0, g_profExec = 0, g_profLast = 0;
extern uint64_t g_profSem, g_profFlip, g_profClear;
void fifo_thread() {
    t_tid = 0x01000200;
    ripprof_register_thread("fifo");
    host_thread_core(true);
    static const bool prof = std::getenv("FIFOPROF") != nullptr;
    uint32_t semA = 0, semB = 0;
    std::vector<uint32_t> callStack;
    uint64_t idle = 0;
    for (;;) {
        const uint32_t put = rd(ctrl), get = rd(ctrl + 4);
        static uint32_t lastPut = 0xFFFFFFFFu;
        static int putLogs = 0;
        { static uint32_t flPut = 0; if (put != flPut) { flPut = put; g_tPut = now_us(); if (g_flStart >= 0) fl("P", put); } }
        if (put != lastPut && putLogs < 600) { ++putLogs; LOG("[rsx] put %x (get %x, cmd@get %08x %08x, @put-8 %08x %08x, @put %08x)\n", put, get, rd(io2ea(get)), rd(io2ea(get) + 4), rd(io2ea(put) - 8), rd(io2ea(put) - 4), rd(io2ea(put))); lastPut = put; }
        if (g_flipDue && (get == put || now_us() - g_flipDueSince > 20000)) deliver_flip();
        if (prof && now_us() - g_profLast > 4000000) {
            const double k = 1.0 / (4.0 * 1000.0);
            if (g_profLast) LOG("[fifoprof] per second: idle %.0f ms, hole wait %.0f ms, label wait %.0f ms, flip %.0f ms, other commands %.0f ms\n", g_profIdle * k, g_profHole * k, g_profSem * k, g_profFlip * k, (g_profExec - g_profSem - g_profFlip) * k);
            g_profSem = g_profFlip = 0;
            g_profIdle = g_profHole = g_profExec = 0; g_profLast = now_us();
        }
        static uint64_t waitStart = 0; static int waitKind = 0;   // FRAMELOG accounting of waits
        if (get == put) {   // nothing to run: the game publishes commands within microseconds, so yield first and only sleep after a while
            const uint64_t t0 = now_us();
            if (g_frAcct && waitKind != 2) { if (waitKind == 1) { g_frHoleUs += t0 - waitStart; ++g_frHoles; } waitStart = t0; waitKind = 2; }
            static uint64_t idleSince = 0;
            if (!idle++) idleSince = t0;
            if (t0 - idleSince < 4000) std::this_thread::yield(); else std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (prof) g_profIdle += now_us() - t0;
            continue;
        }
        idle = 0;
        {   // fast path: a run of plain method packets (no jumps, calls, returns, NOPs or holes) up to put, without the
            // per-word debug bookkeeping below; get is published once per run (and before any method that may block or
            // let the game look at it). ~1000 draws a frame of a few dozen register writes each go through here.
            static const bool slowOnly = std::getenv("FIFOSLOW") || std::getenv("METHODHIST") || std::getenv("FIFODUMP");
            if (!slowOnly) {
                const uint8_t* m = G->mem + ioAddr;
                uint32_t g = get;
                bool any = false;
                while (g != put) {
                    uint32_t cmd; std::memcpy(&cmd, m + g, 4); cmd = __builtin_bswap32(cmd);
                    if ((cmd & 0xA0000003) || cmd == 0 || cmd == 0x00020000) break;   // control flow / NOP: slow path
                    const uint32_t count = (cmd >> 18) & 0x7ff, m0 = cmd & 0xfffc;
                    if (put > g && g + 4 + 4 * count > put) break;   // not completely published yet
                    const bool noInc = (cmd & 0x40000000) != 0;
                    const uint8_t* a = m + g + 4;
                    if (m0 == 0x1808 && count) { uint32_t w; std::memcpy(&w, a, 4); if (w) ++g_rsxBegins; }
                    const uint32_t mEnd = noInc ? m0 : m0 + 4 * (count ? count - 1 : 0);   // last method of the packet
                    if (count && ((m0 >= 0x100 && mEnd < 0x1d6c) || (m0 > 0x1d74 && mEnd < 0x2000))) { rsx::method_block(m0, a, count, noInc); g += 4 + 4 * count; any = true; continue; }
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t arg; std::memcpy(&arg, a + 4 * i, 4); arg = __builtin_bswap32(arg);
                        const uint32_t mm = noInc ? m0 : m0 + 4 * i;
                        if ((mm >= 0x100 && mm < 0x1d6c) || (mm > 0x1d74 && mm < 0x2000)) rsx::method(mm, arg);
                        else { __atomic_store_n(reinterpret_cast<uint32_t*>(G->mem + ctrl + 4), __builtin_bswap32(g), __ATOMIC_RELEASE); method(mm, arg, semA, semB); }
                    }
                    g += 4 + 4 * count;
                    any = true;
                }
                if (any) { __atomic_store_n(reinterpret_cast<uint32_t*>(G->mem + ctrl + 4), __builtin_bswap32(g), __ATOMIC_RELEASE); continue; }
            }
        }
        uint32_t g2 = get;
        const uint32_t cmd = rd(io2ea(g2));
        const bool selfJump = ((cmd & 0xE0000003) == 0x20000000 && (cmd & 0x1FFFFFFC) == g2) || ((cmd & 3) == 1 && (cmd & 0xFFFFFFFC) == g2);
        if (g_frAcct) {
            const int kind = selfJump ? 1 : 0;
            if (kind != waitKind) {
                const uint64_t t = now_us();
                if (waitKind == 1) { g_frHoleUs += t - waitStart; ++g_frHoles; } else if (waitKind == 2) g_frIdleUs += t - waitStart;
                waitStart = t; waitKind = kind;
            }
        }
        if (selfJump) {
            // A jump-to-self is a hole the SPU geometry jobs fill and then turn into a jump past it. Those jobs are
            // not executed yet: when the hole stays closed for a while, the stream continues after it (zero = NOP).
            static uint32_t holeAt = 0xFFFFFFFFu;
            static auto since = std::chrono::steady_clock::now();
            if (holeAt != g2) { holeAt = g2; since = std::chrono::steady_clock::now(); continue; }
            if (std::chrono::steady_clock::now() - since < std::chrono::microseconds(g_holeWaitUs)) { const uint64_t t0 = prof ? now_us() : 0; if (std::chrono::steady_clock::now() - since < std::chrono::milliseconds(4)) std::this_thread::yield(); else std::this_thread::sleep_for(std::chrono::microseconds(50)); if (prof) g_profHole += now_us() - t0; continue; }
            static int holes = 0;
            if (holes++ < 8) {
                std::string s;
                char b[16];
                for (uint32_t k = 0; k < 12; ++k) { std::snprintf(b, sizeof b, " %08x", rd(io2ea(g2 + 4 * k))); s += b; }
                LOG("[rsx] command buffer hole at %x not filled by SPU work: skipped (put %x):%s\n", g2, rd(ctrl), s.c_str());
            }
            holeAt = 0xFFFFFFFFu;
            // The hole size varies (24, 32, ... bytes) and its tail holds stale words from an earlier frame, so
            // resume at the first word after the jump from which a plausible chain of command headers follows.
            uint32_t resume = g2 + 32;
            for (uint32_t o = g2 + 4; o <= g2 + 0x400; o += 4) {
                if (put > g2 && o >= put) { resume = put; break; }
                if (plausible_stream(o, put, 24)) { resume = o; break; }
            }
            g_lastHole = g2; wr(ctrl + 4, resume);
            continue;
        }
        {   // history of control transfers (jump / call / return / hole skips), for the bad-jump report below
            static uint32_t hist[96][2]; static unsigned hn = 0;
            static uint32_t expect = 0xFFFFFFFFu;
            if (cmd != 0 || g2 != expect) { hist[hn % 96][0] = g2; hist[hn % 96][1] = cmd; ++hn; }   // every command word (debug)
            expect = ((cmd & 3) || (cmd & 0xE0000003) == 0x20000000 || cmd == 0x00020000) ? 0xFFFFFFFFu : g2 + 4 + 4 * ((cmd >> 18) & 0x7ff);
            uint32_t tgt = 0xFFFFFFFFu;
            if ((cmd & 0xE0000003) == 0x20000000) tgt = cmd & 0x1FFFFFFC;
            else if ((cmd & 3) == 1 || (cmd & 3) == 2) tgt = cmd & 0xFFFFFFFC;
            // a branch into the initial 128 KB buffer from the rings is a stale word the PPU has not overwritten yet
            if (tgt != 0xFFFFFFFFu && (tgt >= ioSize || tgt < 0x1000 || (tgt < 0x20000 && g2 >= 0x20000))) {
                static int bad = 0;
                if (bad++ < 4) {
                    std::string s; char b[40];
                    for (unsigned k = hn - 96; k != hn; ++k) { std::snprintf(b, sizeof b, " %x:%08x", hist[k % 96][0], hist[k % 96][1]); s += b; }
                    LOG("[rsx] BAD jump at %x -> %x (put %x); history:%s\n", g2, tgt, put, s.c_str());
                    s.clear();
                    for (uint32_t o = g2 - 0x40; o <= g2 + 0x20; o += 4) { std::snprintf(b, sizeof b, " %08x", rd(io2ea(o))); s += b; }
                    LOG("[rsx]   words around: %s\n", s.c_str());
                    s.clear();
                    for (uint32_t o = g_lastHole; o < g_lastHole + 0x140; o += 4) { std::snprintf(b, sizeof b, " %08x", rd(io2ea(o))); s += b; }
                    LOG("[rsx]   from last hole %x: %s\n", g_lastHole, s.c_str());
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;   // do not follow it: wait (maybe the word is still being written)
            }
        }
        {   // FIFODUMP=n: every command word header executed during frame n (address, header, first argument)
            static const long dumpFrame = std::getenv("FIFODUMP") ? std::atol(std::getenv("FIFODUMP")) : -1;
            if (long(flips.load()) == dumpFrame && cmd != 0) LOG("[fifo] %07x: %08x %08x %08x\n", g2, cmd, rd(io2ea(g2 + 4)), rd(io2ea(g2 + 8)));
        }
        if ((cmd & 0xE0000003) == 0x20000000) { g2 = cmd & 0x1FFFFFFC; wr(ctrl + 4, g2); deliver_flip(); }   // old jump
        else if ((cmd & 3) == 1) { g2 = cmd & 0xFFFFFFFC; wr(ctrl + 4, g2); deliver_flip(); }                // jump
        else if ((cmd & 3) == 2) { callStack.push_back(g2 + 4); g2 = cmd & 0xFFFFFFFC; } // call
        else if (cmd == 0x00020000) { g2 = callStack.empty() ? g2 + 4 : callStack.back(); if (!callStack.empty()) callStack.pop_back(); }   // return
        else if (cmd == 0) {   // NOPs: Edge pads its holes with hundreds of them, skip the whole run at once
            const uint32_t limit = (put > g2 ? put : ioSize) - 4;
            const uint8_t* m = G->mem + ioAddr;
            uint32_t w = 0;
            do { g2 += 4; std::memcpy(&w, m + g2, 4); } while (w == 0 && g2 < limit);
        }
        else {
            const uint32_t count = (cmd >> 18) & 0x7ff, m0 = cmd & 0xfffc;
            const bool noInc = (cmd & 0x40000000) != 0;
            const uint32_t putNow = rd(ctrl);
            if (putNow > g2 && g2 + 4 + 4 * count > putNow) {   // the command is not completely published yet
                static std::atomic<int> partial{0};
                if (partial++ < 8) LOG("[rsx] command at %x (%08x, %u words) extends past put %x: waiting\n", g2, cmd, count, putNow);
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
            if (m0 == 0x1808 && rd(io2ea(g2 + 4))) ++g_rsxBegins;
            const uint64_t tx = prof ? now_us() : 0;
            for (uint32_t i = 0; i < count; ++i) method(noInc ? m0 : m0 + 4 * i, rd(io2ea(g2 + 4 + 4 * i)), semA, semB);
            if (prof) g_profExec += now_us() - tx;
            g2 += 4 + 4 * count;
        }
        wr(ctrl + 4, g2);
    }
}

// GAMEHZ=n: rate of the emulated vblank (experiment: the console has 60)
// streaming mode: 60 per second like the video (OBS, Twitch), so every game image is exactly one video frame
// Both follow the options menu while the game runs (vblank_thread checks them every vblank).
// online game (in a room): 60 for everyone, as on the console, so no player runs differently from the others
static int wanted_hz() { return (settings().streaming || gti::recomp::netplay::in_room()) && !std::getenv("GAMEHZ") ? 60 : settings().gameHz; }
static bool wanted_refresh() { return settings().refresh || settings().streaming; }
int g_gameHz = wanted_hz();
// The game was written for 60 images per second: parts of its logic advance a fixed step per image. Patched code
// scales that step by g_frameStep (FRAMESTEP=0: off).
static const bool g_noFrameStep = std::getenv("FRAMESTEP") && std::getenv("FRAMESTEP")[0] == '0';
const bool g_frameStepInit = (g_frameStep = g_noFrameStep ? 1.0f : 60.0f / float(g_gameHz), true);
void vblank_thread() {
    t_tid = 0x01000201;
    // Pacing of the emulated 60 Hz vblank. When the display refreshes at a multiple of 60 the vblank follows the real
    // refreshes (every image is then shown for the same number of them); a display at another rate (144 Hz) is
    // switched to 120 Hz while the game runs. REFRESCO=0 leaves the display alone, VBLANKTIMER=1 forces the timer.
    int perVblank = 0;   // display refreshes per vblank; 0 = timer
    bool refreshNow = wanted_refresh();
    int switchedTo = 0;   // the rate the display was switched to (0: its own)
    bool waitBroken = false;   // host_wait_refresh() does not wait (see below)
    const int nativeHz = host_display_hz();
    auto setup = [&] {   // at start, and again when the options menu changes the rate or the display switch
        const char* e = std::getenv("REFRESCO");
        const int want = e && std::atoi(e) > 0 ? std::atoi(e) : 120;
        const bool needSwitch = nativeHz > 0 && nativeHz % g_gameHz != 0 && refreshNow && !(e && !std::strcmp(e, "0")) && want % g_gameHz == 0;
        if (switchedTo && (!needSwitch || switchedTo != want)) { host_restore_refresh(); switchedTo = 0; LOG("[vblank] display back to %d Hz\n", nativeHz); }
        int hz = switchedTo ? switchedTo : host_display_hz();
        perVblank = 0;
        if (needSwitch && !switchedTo) {
            if (host_set_refresh(want)) { LOG("[vblank] display switched from %d to %d Hz while the game runs\n", hz, want); hz = switchedTo = want; }
            else LOG("[vblank] the display has no %d Hz mode: staying at %d Hz\n", want, hz);
        }
        if (hz > 0 && hz % g_gameHz == 0 && !std::getenv("VBLANKTIMER") && !waitBroken) perVblank = hz / g_gameHz;
        LOG("[vblank] %d images per second, display %d Hz: vblank %s\n", g_gameHz, hz, perVblank ? "follows the display refresh" : "on a timer");
    };
    setup();
    auto next = std::chrono::steady_clock::now();
    uint64_t lastTick = now_us();
    int odd = 0;
    for (;;) {
        {   // HZSWITCH=seconds:rate,... (debug): what the options menu does, at those times
            static std::vector<std::pair<double, int>> sw; static bool parsed = false;
            if (!parsed) { parsed = true; if (const char* e = std::getenv("HZSWITCH")) for (const char* q = e; *q;) { double t; int r; if (std::sscanf(q, "%lf:%d", &t, &r) == 2) sw.push_back({t, r}); while (*q && *q != ',') ++q; if (*q) ++q; } }
            static const auto t0 = std::chrono::steady_clock::now();
            if (!sw.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= sw.front().first) { settings().gameHz = sw.front().second; sw.erase(sw.begin()); }
        }
        if (const int hz = wanted_hz(); hz > 0 && (hz != g_gameHz || wanted_refresh() != refreshNow)) {   // options menu
            g_gameHz = hz; refreshNow = wanted_refresh();
            if (!g_noFrameStep) g_frameStep = 60.0f / float(g_gameHz);
            setup();
            next = std::chrono::steady_clock::now(); lastTick = now_us();
        }
        if (perVblank) {
            for (int k = 0; k < perVblank && perVblank; ++k) if (!host_wait_refresh()) perVblank = 0;
            // VBLANKFASE=us: the game's vblank comes that long after the real refresh, so its image is presented in the
            // middle of the refresh interval instead of right at the moment the compositor (DWM, windowed / borderless)
            // picks the image up: one image a refresh at 120 Hz has no slack for a present landing on either side of it
            // Default: half a refresh when every refresh carries a new image (120 on 120 Hz, 144 on 144 Hz); with two or
            // more refreshes per image (72 on 144 Hz) there is slack and nothing changes. VBLANKFASE=0 turns it off.
            {   static const char* const faseEnv = std::getenv("VBLANKFASE");
                const long fase = faseEnv ? std::atol(faseEnv) : (perVblank == 1 ? 500000 / g_gameHz : 0);
                if (fase > 0 && perVblank) host_sleep_us(uint64_t(fase)); }
            const uint64_t t = now_us(), dt = t - lastTick;
            lastTick = t;
            if (g_missLog && dt * g_gameHz > 1500000) LOG("[vgap] %.1f ms between vblanks\n", dt / 1000.0);
            // the refreshes must really be 60 Hz: most of every 240 vblanks have to be 16.67 ms apart (stalls excluded)
            static int good = 0, seen = 0;
            (void)odd;
            if (dt * g_gameHz > 936000 && dt * g_gameHz < 1062000) ++good;
            {   // the wait returning at once (seen with the NVIDIA 621 driver: ~2 us): the vblanks would come in a burst.
                // The timer then, for the rest of the run; the display keeps the rate it was switched to.
                static int instant = 0;
                instant = dt * g_gameHz < 100000 ? instant + 1 : 0;
                if (instant >= 8) { LOG("[vblank] the display's vblank wait returns at once: vblank on a timer\n"); waitBroken = true; perVblank = 0; instant = good = seen = 0; }
            }
            if (perVblank && ++seen == 240) {
                if (good < 160) { LOG("[vblank] only %d of 240 vblanks were 16.67 ms apart (display reports %d Hz): back to the 60 Hz timer\n", good, host_display_hz()); perVblank = 0; host_restore_refresh(); switchedTo = 0; }
                good = seen = 0;
            }
            next = std::chrono::steady_clock::now();
        }
        if (!perVblank) {   // sleep most of the interval, yield through the end of it
            // A high resolution timer (host_sleep_us) wakes within ~0.1-0.5 ms, so only the last VBLANKSPIN us (300) are
            // yielded through. Before, a plain sleep (1 ms or more on Windows) left 2 ms of yielding in every image:
            // a quarter of a core at 120 images per second. VBLANKSPIN=2000 with VBLANKSLEEP=0: the old way.
            static const int64_t spinUs = std::getenv("VBLANKSPIN") ? std::atoll(std::getenv("VBLANKSPIN")) : 300;
            static const bool precise = !(std::getenv("VBLANKSLEEP") && std::getenv("VBLANKSLEEP")[0] == '0');
            next += std::chrono::microseconds(1000000 / g_gameHz);
            const auto wakeAt = next - std::chrono::microseconds(spinUs);
            if (precise) {
                const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(wakeAt - std::chrono::steady_clock::now()).count();
                if (us > 0) host_sleep_us(uint64_t(us));
            } else std::this_thread::sleep_until(wakeAt);
            while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
        }
        if (g_missLog) {   // a stretch without flips while the vblanks go on: what every thread is blocked in, once per stretch
            static uint64_t lastFlips = 0, since = 0; static bool dumped = false;
            const uint64_t f = flips.load(), t = now_us();
            if (f != lastFlips) { lastFlips = f; since = t; dumped = false; }
            else if (!dumped && since && t - since > 60000) { dumped = true; LOG("[stall] no flip for %.0f ms:\n", (t - since) / 1000.0); lv2_dump_waits(); }
        }
        if (flipPending.load()) { ++flips; call_guest(flipHandler, 1); flipPending = false; }
        { const uint32_t b = labels ? rd(labels + 0x810) : 0; call_guest(vblankHandler, 1); const uint32_t a = labels ? rd(labels + 0x810) : 0; fl("V", b * 16 + a); g_vblankFree = b == a; g_lastVblankUs = now_us(); }
        ++g_vblankTick; g_vblankTick.notify_all();
        static int tick = 0;
        if (tick == 600 && ctx) {   // ~10 s: dump the command stream before the current GET (debug)
            const uint32_t gg = rd(ctrl + 4);
            std::string s;
            char b[16];
            for (uint32_t o = gg - 0x80; o <= gg; o += 4) { std::snprintf(b, sizeof b, " %08x", rd(io2ea(o))); s += b; }
            LOG("[gcm] before get %x:%s\n", gg, s.c_str());
            const uint32_t pp = rd(ctrl);
            s.clear();
            for (uint32_t o = pp - 0x40; o <= pp + 0x10; o += 4) { std::snprintf(b, sizeof b, " %08x", rd(io2ea(o))); s += b; }
            LOG("[gcm] around put %x:%s\n", pp, s.c_str());
        }
        if (++tick % 120 == 0 && ctx) LOG("[gcm] put %x get %x current %x flips %llu label400 %08x (%llu writes)  cmd@get %08x %08x\n", rd(ctrl), rd(ctrl + 4), rd(ctx + 8) - ioAddr, (unsigned long long)flips.load(), rd(labels + 0x400), (unsigned long long)g_lbl400.load(), rd(io2ea(rd(ctrl + 4))), rd(io2ea(rd(ctrl + 4)) + 4));
    }
}

void start() {
    if (started.exchange(true)) return;
    std::thread(fifo_thread).detach();
    std::thread(vblank_thread).detach();
}

// buffer full: let the RSX drain everything written so far, then restart at the beginning of the buffer
void buffer_full(PPU& c) {
    const uint32_t cx = uint32_t(c.r[3]);
    const uint32_t begin = c.rd32(cx), cur = c.rd32(cx + 8);
    static int shown = 0;
    if (shown++ < 20) LOG("[gcm] buffer full: ctx %x begin %x end %x current %x need %llu words (put %x get %x)\n", cx, begin - ioAddr, c.rd32(cx + 4) - ioAddr, cur - ioAddr, (unsigned long long)c.r[4], rd(ctrl), rd(ctrl + 4));
    wr(io2ea(cur - ioAddr), 0x20000000 | ((begin - ioAddr) & 0x1FFFFFFC));   // jump back to the start
    wr(ctrl, cur - ioAddr);
    for (int k = 0; k < 2000000 && rd(ctrl + 4) != cur - ioAddr; ++k) std::this_thread::sleep_for(std::chrono::microseconds(5));
    wr(ctrl, begin - ioAddr);
    for (int k = 0; k < 2000000 && rd(ctrl + 4) != begin - ioAddr; ++k) std::this_thread::sleep_for(std::chrono::microseconds(5));
    c.wr32(cx + 8, begin);
    c.r[3] = 0;
}

bool import(PPU& c, const std::string& n) {
    if (n == "_cellGcmInitBody") {   // (CellGcmContextData** ctx, cmdSize, ioSize, ioAddress)
        const uint32_t cmdSize = a32(c, 1);
        ioSize = a32(c, 2); ioAddr = a32(c, 3);
        ctx = alloc_heap(16, 16);
        ctrl = alloc_heap(0x40, 16);
        labels = alloc_heap(0x1000, 0x1000);
        std::memset(G->mem + labels, 0, 0x1000);
        cbOpd = alloc_heap(8, 8);
        c.wr32(cbOpd, CB_ADDR); c.wr32(cbOpd + 4, 0);
        c.wr32(ctx, ioAddr + 0x1000);             // begin (first 4 KB reserved)
        c.wr32(ctx + 4, ioAddr + cmdSize - 4);    // end
        c.wr32(ctx + 8, ioAddr + 0x1000);         // current
        c.wr32(ctx + 12, cbOpd);                  // callback
        wr(ctrl, 0x1000); wr(ctrl + 4, 0x1000); wr(ctrl + 8, 0xFFFFFFFFu);
        c.wr32(a32(c, 0), ctx);
        LOG("[gcm] init: io 0x%x size 0x%x, command buffer 0x%x bytes, control 0x%x labels 0x%x\n", ioAddr, ioSize, cmdSize, ctrl, labels);
        rsx::init(rsx::Config{G->mem, LOCAL_ADDR, ioAddr});
        start();
        ret(c, CELL_OK); return true;
    }
    if (n == "cellGcmGetConfiguration") {   // {localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency}
        const uint32_t p = a32(c, 0);
        c.wr32(p, LOCAL_ADDR); c.wr32(p + 4, ioAddr); c.wr32(p + 8, LOCAL_SIZE); c.wr32(p + 12, ioSize);
        c.wr32(p + 16, 650000000); c.wr32(p + 20, 500000000);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellGcmAddressToOffset") {    // (addr, u32* offset)
        const uint32_t a = a32(c, 0);
        uint32_t off;
        if (a >= LOCAL_ADDR && a < LOCAL_ADDR + LOCAL_SIZE) off = a - LOCAL_ADDR;
        else if (a >= ioAddr && a < ioAddr + ioSize) off = a - ioAddr;
        else { ret(c, 0x802100ff); return true; }
        c.wr32(a32(c, 1), off);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellGcmGetControlRegister") { ret(c, ctrl); return true; }
    if (n == "cellGcmGetLabelAddress") { ret(c, labels + 0x10 * (a32(c, 0) & 0xff)); return true; }
    if (n == "cellGcmGetReport" || n == "cellGcmGetTimeStamp") { ret(c, now_us() * 1000); return true; }
    if (n == "cellGcmGetTiledPitchSize") {
        static const uint32_t P[] = {0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00, 0xE00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00,
                                     0x2000, 0x2800, 0x3000, 0x3400, 0x3800, 0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
        uint32_t v = 0;
        for (uint32_t p : P) if (p >= a32(c, 0)) { v = p; break; }
        ret(c, v); return true;
    }
    if (n == "cellGcmSetDisplayBuffer") {   // (id, offset, pitch, width, height)
        rsx::set_display_buffer(a32(c, 0), a32(c, 1), a32(c, 2), a32(c, 3), a32(c, 4));
        ret(c, CELL_OK); return true;
    }
    if (n == "cellGcmSetFlipHandler") { flipHandler = a32(c, 0); ret(c, CELL_OK); return true; }
    if (n == "cellGcmSetVBlankHandler") { vblankHandler = a32(c, 0); ret(c, CELL_OK); return true; }
    if (n == "cellGcmSetUserHandler") { userHandler = a32(c, 0); ret(c, CELL_OK); return true; }
    if (n == "_cellGcmSetFlipCommand") {    // (ctx, bufferId): appends the flip method
        const uint32_t cx = a32(c, 0);
        const uint32_t cur = c.rd32(cx + 8);
        static int shownF = 0;
        if (shownF++ < 10) LOG("[gcm] flip command: ctx %x begin %x end %x current %x (io %x) buffer %u\n", cx, c.rd32(cx), c.rd32(cx + 4), cur, cur - ioAddr, a32(c, 1));
        c.wr32(cur, (1u << 18) | 0xFEAC);
        c.wr32(cur + 4, a32(c, 1));
        c.wr32(cx + 8, cur + 8);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellGcmSetFlipMode") LOG("[gcm] flip mode %u (1 = hsync, 2 = vsync)\n", a32(c, 0));
    if (n == "cellGcmSetDisplayBuffer" || n == "cellGcmSetFlipMode" || n == "cellGcmSetTile" || n == "cellGcmBindTile" || n == "cellGcmBindZcull" ||
        n == "cellGcmSetTileInfo" || n == "_cellGcmFunc15") { ret(c, CELL_OK); return true; }
    return false;
}
} // namespace gcm

// ------------------------------------------------------------------ printf family with guest varargs
// PPC64 ELF ABI: argument i lives in r3+i (i < 8) or in the caller's parameter save area at sp+0x30+8*i;
// a va_list is a pointer to consecutive 8-byte slots. Floating point varargs are also present in those slots.
struct VaArgs {
    PPU* c; int next; uint32_t list;   // list != 0: va_list pointer
    uint64_t get() {
        if (list) { const uint64_t v = c->rd64(list); list += 8; return v; }
        const int i = next++;
        return i < 8 ? c->r[3 + i] : c->rd64(uint32_t(c->r[1]) + 0x30 + 8 * i);
    }
};
std::string gformat(PPU& c, uint32_t fmtAddr, VaArgs va) {
    const char* f = reinterpret_cast<const char*>(c.mem + fmtAddr);
    std::string out;
    char spec[32], buf[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        const char* st = f++;
        if (*f == '%') { out += '%'; ++f; continue; }
        while (*f && std::strchr("-+ #0", *f)) ++f;
        if (*f == '*') { va.get(); ++f; } else while (*f >= '0' && *f <= '9') ++f;
        if (*f == '.') { ++f; if (*f == '*') { va.get(); ++f; } else while (*f >= '0' && *f <= '9') ++f; }
        int lng = 0;
        while (*f && std::strchr("hlLqjzt", *f)) { if (*f == 'l' || *f == 'q' || *f == 'j' || *f == 'L') ++lng; ++f; }
        const char conv = *f ? *f++ : 0;
        // rebuild the spec without length modifiers; values are passed as 64-bit
        size_t n = 0;
        for (const char* p = st; p < f - 1 && n < sizeof spec - 4; ++p) if (!std::strchr("hlLqjzt", *p)) spec[n++] = *p;
        switch (conv) {
            case 'd': case 'i': {
                const uint64_t v = va.get();
                const long long x = lng ? (long long)v : (long long)(int32_t)v;
                spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conv; spec[n] = 0;
                std::snprintf(buf, sizeof buf, spec, x); out += buf; break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                const uint64_t v = va.get();
                const unsigned long long x = lng ? v : (unsigned long long)(uint32_t)v;
                spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conv; spec[n] = 0;
                std::snprintf(buf, sizeof buf, spec, x); out += buf; break;
            }
            case 'c': spec[n++] = 'c'; spec[n] = 0; std::snprintf(buf, sizeof buf, spec, int(va.get() & 0xff)); out += buf; break;
            case 's': {
                const uint32_t a = uint32_t(va.get());
                spec[n++] = 's'; spec[n] = 0;
                std::snprintf(buf, sizeof buf, spec, a ? reinterpret_cast<const char*>(c.mem + a) : "(null)"); out += buf; break;
            }
            case 'p': std::snprintf(buf, sizeof buf, "0x%x", uint32_t(va.get())); out += buf; break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                const uint64_t v = va.get(); double d; std::memcpy(&d, &v, 8);
                spec[n++] = conv; spec[n] = 0;
                std::snprintf(buf, sizeof buf, spec, d); out += buf; break;
            }
            case 'n': va.get(); break;
            default: out.append(st, f); break;
        }
    }
    return out;
}
uint32_t put_str(PPU& c, uint32_t dst, uint64_t cap, const std::string& s) {   // snprintf semantics
    if (cap) { const size_t k = std::min<size_t>(s.size(), size_t(cap - 1)); std::memcpy(c.mem + dst, s.data(), k); c.mem[dst + k] = 0; }
    return uint32_t(s.size());
}

// Calls a guest function (OPD) from inside an HLE import on the same thread, then restores every register.
// The callee gets a stack frame below `sp` (callers keep their scratch structs between sp and the old r1).
uint64_t call_nested(PPU& c, uint32_t opd, uint32_t sp, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0,
                     uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0) {
    PPU saved = c;
    c.r[1] = (sp & ~0xFu) - 0x100;
    c.r[2] = c.rd32(opd + 4);
    c.r[3] = a0; c.r[4] = a1; c.r[5] = a2; c.r[6] = a3; c.r[7] = a4; c.r[8] = a5; c.r[9] = a6; c.r[10] = a7;
    c.lr = 0;
    rc_call(c, c.rd32(opd));
    const uint64_t r = c.r[3];
    const uint32_t missing = c.missing, unimpl = c.unimplAt;
    c = saved;
    if (missing && !c.missing) c.missing = missing;
    if (unimpl && !c.unimplAt) c.unimplAt = unimpl;
    return r;
}

std::mutex g_sysutilMx;
std::pair<uint32_t, uint32_t> g_sysutilCb[4];          // registered callbacks: {func OPD, userdata}
std::deque<std::pair<uint32_t, uint32_t>> g_sysutilEv;   // pending system events: {status, param}

// ------------------------------------------------------------------ save data (cellSaveDataAutoLoad2 / AutoSave2)
// Saves live in <hdd0>/home/00000001/savedata/<dirName>/ ; PARAM is kept as the raw 1552-byte
// CellSaveDataSystemFileParam in _param.bin.  Layouts follow the SDK headers (big-endian, 32-bit pointers).
namespace savedata {
constexpr uint32_t ERR_CBRESULT = 0x8002b401, ERR_NODATA = 0x8002b40b;
constexpr uint32_t PARAM_SIZE = 1552;

std::string dir_of(const std::string& name) { return CFG.hdd0 + "/home/00000001/savedata/" + name; }

uint32_t op(PPU& c, bool isSave) {
    const std::string dirName = reinterpret_cast<const char*>(c.mem + a32(c, 1));
    const uint32_t setBuf = a32(c, 3), funcStat = a32(c, 4), funcFile = a32(c, 5), userdata = a32(c, 7);
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string hdir = dir_of(dirName);
    const bool exists = fs::exists(hdir + "/_param.bin", ec);
    // scratch structs on the guest stack, below the caller's frame
    const uint32_t base = (uint32_t(c.r[1]) - 0x2000) & ~0xFu;
    const uint32_t cbRes = base, statGet = base + 0x40, statSet = base + 0x7c0, fileGet = base + 0x800, fileSet = base + 0x880;
    std::memset(c.mem + base, 0, 0x900);
    c.wr32(cbRes + 16, userdata);
    // CellSaveDataStatGet: hddFreeSizeKB, isNewData, dir{atime,mtime,ctime,dirName[32]} @8, getParam @0x40,
    // bind @0x650, sizeKB, sysSizeKB, fileNum, fileListNum, fileList @0x664
    c.wr32(statGet, 10 * 1024 * 1024);
    c.wr32(statGet + 4, exists ? 0 : 1);
    std::memcpy(c.mem + statGet + 8 + 24, dirName.c_str(), std::min<size_t>(dirName.size(), 31));
    const uint32_t listMax = c.rd32(setBuf + 4), listBuf = c.rd32(setBuf + 0x24);
    uint32_t fileNum = 0, totalKB = 0;
    if (exists) {
        if (FILE* f = std::fopen((hdir + "/_param.bin").c_str(), "rb")) { std::fread(c.mem + statGet + 0x40, 1, PARAM_SIZE, f); std::fclose(f); }
        for (auto& e : fs::directory_iterator(hdir, ec)) {
            const std::string fn = e.path().filename().string();
            if (fn == "_param.bin" || !e.is_regular_file()) continue;
            const uint64_t sz = e.file_size(ec);
            totalKB += uint32_t((sz + 1023) / 1024);
            if (fileNum < listMax && listBuf) {   // CellSaveDataFileStat (56 bytes): fileType, size @8, times, fileName[13] @0x28
                const uint32_t p = listBuf + 56 * fileNum;
                std::memset(c.mem + p, 0, 56);
                c.wr32(p, 0); c.wr64(p + 8, sz);
                std::memcpy(c.mem + p + 0x28, fn.c_str(), std::min<size_t>(fn.size(), 12));
            }
            ++fileNum;
        }
    }
    c.wr32(statGet + 0x654, totalKB); c.wr32(statGet + 0x658, 1);
    c.wr32(statGet + 0x65c, fileNum); c.wr32(statGet + 0x660, std::min(fileNum, listMax));
    c.wr32(statGet + 0x664, listBuf);
    call_nested(c, funcStat, base, cbRes, statGet, statSet);
    int32_t res = int32_t(c.rd32(cbRes));
    LOG("[save] %s '%s' (%s): funcStat result %d\n", isSave ? "save" : "load", dirName.c_str(), exists ? "exists" : "new", res);
    if (res < 0) return ERR_CBRESULT;
    if (isSave) {
        fs::create_directories(hdir, ec);
        if (const uint32_t sp = c.rd32(statSet)) {
            if (FILE* f = std::fopen((hdir + "/_param.bin").c_str(), "wb")) { std::fwrite(c.mem + sp, 1, PARAM_SIZE, f); std::fclose(f); }
        }
    } else if (!exists) return ERR_NODATA;
    if (res == 1 || res == 2) return CELL_OK;   // OK_LAST / OK_LAST_NOCONFIRM: no file operations
    for (int guard = 0; guard < 64; ++guard) {
        std::memset(c.mem + fileSet, 0, 48);
        call_nested(c, funcFile, base, cbRes, fileGet, fileSet);
        res = int32_t(c.rd32(cbRes));
        if (res < 0) return ERR_CBRESULT;
        if (res == 1 || res == 2) break;
        // CellSaveDataFileSet: fileOperation, reserved, fileType, secureFileId[16], fileName @28, fileOffset, fileSize, fileBufSize, fileBuf @44
        const uint32_t opn = c.rd32(fileSet), nameP = c.rd32(fileSet + 28), off = c.rd32(fileSet + 32), size = c.rd32(fileSet + 36), buf = c.rd32(fileSet + 44);
        const std::string fn = nameP ? reinterpret_cast<const char*>(c.mem + nameP) : "";
        const std::string hp = hdir + "/" + fn;
        uint32_t done = 0;
        if (opn == 0) {   // READ
            if (FILE* f = std::fopen(hp.c_str(), "rb")) { std::fseek(f, long(off), SEEK_SET); done = uint32_t(std::fread(c.mem + buf, 1, size, f)); std::fclose(f); }
        } else if (opn == 1 || opn == 3) {   // WRITE / WRITE_NOTRUNC
            fs::create_directories(hdir, ec);
            FILE* f = std::fopen(hp.c_str(), opn == 1 ? "wb" : (fs::exists(hp, ec) ? "r+b" : "wb"));
            if (f) { std::fseek(f, long(off), SEEK_SET); done = uint32_t(std::fwrite(c.mem + buf, 1, size, f)); std::fclose(f); }
        } else if (opn == 2) fs::remove(hp, ec);
        LOG("[save]   file op %u '%s' off %u size %u -> %u\n", opn, fn.c_str(), off, size, done);
        c.wr32(fileGet, done);
    }
    return CELL_OK;
}
} // namespace savedata

// ------------------------------------------------------------------ SPURS job chain (job manager JM2) on the host
// One host thread walks the job chain command list like the SPURS job manager and runs every job binary in the
// SPU interpreter. Commands (u64): JOB = descriptor EA (low 3 bits 0), NOP 0, RESET_PC ea|1, SYNC/LWSYNC 2|x<<3,
// NEXT ea|3, CALL ea|4, FLUSH 5, JOBLIST ea|6, 7|sub<<3: ABORT 0, GUARD 1 (ea|0xf), SET_LABEL 2, RET 14, END 15.
// Job descriptor (CellSpursJob256): eaBinary u64 @0, sizeBinary u16 (x16) @8, sizeDmaList u16 @0xa, eaHighInput @0xc,
// useInOutBuffer @0x10, sizeInOrInOut @0x14, sizeOut @0x18, sizeStack u16 @0x1c, sizeScratch u16 @0x1e,
// eaHighCache @0x20, sizeCacheDmaList @0x24, then the input DMA list {u32 size, u32 eal} @0x30, then the cache list.
// Job context (CellSpursJobContext2) as the job binaries read it: ioBuffer @0, cacheBuffer[4] @4, oBuffer @0x1c,
// dmaTag @0x24, eaJobDescriptor u64 @0x28. Binaries are position independent, entered at +0x10 with r3 = context,
// r4 = LS copy of the descriptor.
namespace jobs {
constexpr uint32_t LS_STUB = 0x100, LS_CTX = 0x2c00, LS_JOB = 0x2d00, LS_BIN = 0x3000, LS_STACK = 0x3fff0;
constexpr uint32_t EVENTFLAG_JOB = 0xbad880;   // job binary that sets a SPURS event flag (done on the host)
// Default by processor size: the game keeps ~6 other host threads busy (main, render, FIFO, job manager, sound,
// streams), so the workers get what is left: 8 with 16 hardware threads, 6 with 12, 3 at least. More workers than
// free hardware threads only add wake-up and contention costs (measured: each extra worker adds CPU use).
// 4 at most: in a race at 120 fps the jobs keep ~1.9 host cores busy; 4 workers kept 120 fps with 18% less CPU of the
// whole process than 8 (with the batched hand-over: 680% -> 555% of one core), 3 already lost a few images.
const int g_workers = std::getenv("SPUS") ? std::max(1, std::atoi(std::getenv("SPUS")))
                                          : (std::thread::hardware_concurrency() <= 4 ? 2 : std::clamp(int(std::thread::hardware_concurrency()) - 6, 3, 4));   // 2 cores / 4 threads: 2 workers kept more images (AFFINITY=f test)   // host threads running jobs. The game only describes Edge output buffers for SPUs 0-2; run_job creates those of workers 3-5 (more than 6 have none and would drop their geometry)

uint32_t list = 0;
std::thread th;
std::atomic<bool> started{false};
std::atomic<uint64_t> jobDraws{0};   // Edge jobs whose command block contains a draw
// kick(): the game restarted the job chain or opened a guard. The manager spins for a moment and then sleeps on
// kickCv (it used to yield for 50 ms: a whole core burnt whenever the chain was idle).
std::mutex kickMx; std::condition_variable kickCv; std::atomic<bool> mgrSleeping{false};
std::atomic<uint64_t> kicks{0}, jobsRun{0}, spuSteps{0}, spuNs{0}, interpTot{0}, callsTot{0};
struct Guard { int32_t count = 1; bool autoReset = true; std::atomic<int32_t> remaining{1}; };
std::mutex gmx;
std::map<uint32_t, std::shared_ptr<Guard>> guards;
const bool trace = std::getenv("JOBTRACE") != nullptr;

std::shared_ptr<Guard> guard_of(uint32_t ea) {
    std::lock_guard<std::mutex> lk(gmx);
    auto& g = guards[ea];
    if (!g) g = std::make_shared<Guard>();
    return g;
}

uint32_t rd32m(uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(G->mem + a)); }
uint16_t rd16m(uint32_t a) { return uint16_t(G->mem[a] << 8 | G->mem[a + 1]); }
uint64_t rd64m(uint32_t a) { return uint64_t(rd32m(a)) << 32 | rd32m(a + 4); }

// Waiters of cellSpursEventFlagWait sleep on this and are woken by every set_event_flag: polling with a 50 us sleep
// really slept ~2 ms on Windows, several times per frame (the render thread waits for the geometry jobs).
std::mutex g_efMx;
std::condition_variable g_efCv;
void set_event_flag(uint32_t ef, uint16_t bits) {   // same storage as cellSpursEventFlagWait reads (u16 at +0)
    uint32_t* p = reinterpret_cast<uint32_t*>(G->mem + ef);
    uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    for (;;) {
        const uint32_t nw = __builtin_bswap32(__builtin_bswap32(raw) | (uint32_t(bits) << 16));
        if (__atomic_compare_exchange_n(p, &raw, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    }
    { std::lock_guard<std::mutex> lk(g_efMx); }
    g_efCv.notify_all();
}

struct JobDesc { uint32_t jea; uint8_t d[256]; std::vector<uint8_t> in; };   // descriptor + input DMA data as grabbed   // descriptor copied when the job is grabbed (the game reuses the slots)
static inline uint32_t jd32(const JobDesc& j, uint32_t o) { return uint32_t(j.d[o]) << 24 | uint32_t(j.d[o + 1]) << 16 | uint32_t(j.d[o + 2]) << 8 | j.d[o + 3]; }
static inline uint16_t jd16(const JobDesc& j, uint32_t o) { return uint16_t(j.d[o] << 8 | j.d[o + 1]); }
thread_local int t_spuNum = 0;
Spu* g_workerSpu[16] = {};                     // per worker: its SPU and the job it is running (diagnostics)
std::atomic<uint32_t> g_workerJob[16] = {};
std::atomic<uint64_t> g_workerSince[16] = {};   // now_us() when that job started
// CULLCAP=dir,seconds,n: local store + registers at entry (LS 0x9e68) and return (0x4400) of Edge's triangle culling routine,
// for n jobs from `seconds` after the first Edge job -> dir/cull_NNNN_in.bin / _out.bin (256 KB local store, then 128 registers of 16 bytes)
static uint32_t edge_cull_capture(spurt::Q* __restrict r, Spu& s, uint32_t pc) {
    static char dir[256]; static long left = -1, first = 0; static uint64_t t0 = 0;
    static std::atomic<long> seq{0};
    static thread_local long mine = -1;
    if (left < 0) { left = 0; if (const char* e = std::getenv("CULLCAP")) { long n = 0; if (std::sscanf(e, "%255[^,],%ld,%ld", dir, &first, &n) == 3) left = n; } t0 = now_us(); }
    if (pc != 0x9e68 && pc != 0x4400) return pc;
    if (pc == 0x4400 && mine < 0) return pc;
    if (pc == 0x9e68) { if (now_us() - t0 < uint64_t(first) * 1000000u) return pc; mine = seq++; if (mine >= left) { mine = -1; return pc; } }
    char fn[300]; std::snprintf(fn, sizeof fn, "%s/cull_%04ld_%s.bin", dir, mine, pc == 0x9e68 ? "in" : "out");
    if (FILE* f = std::fopen(fn, "wb")) {
        std::fwrite(s.ls, 1, Spu::LS_SIZE, f);
        for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) { const uint32_t v = r[i].w[k]; const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 4, f); }
        std::fclose(f);
    }
    if (pc == 0x4400) mine = -1;
    return pc;
}
// FNCAP=entry,return,dir,seconds,n (hex LS addresses): the same capture for any Edge routine that only works on the
// local store -> dir/fn_NNNN_in.bin / _out.bin (tool: fn_replay). Native culling keeps running around it.
static uint32_t g_fnCapEntry = 0, g_fnCapRet = 0;
static uint32_t edge_hle(spurt::Q* __restrict r, Spu& s, uint32_t pc);
static uint32_t edge_fn_capture(spurt::Q* __restrict r, Spu& s, uint32_t pc) {
    static char dir[256]; static long left = 0, first = 0; static uint64_t t0 = 0;
    static std::atomic<long> seq{0};
    static std::once_flag once;
    static thread_local long mine = -1;
    static thread_local uint32_t entryR1 = 0;
    std::call_once(once, [] { long n = 0; if (std::sscanf(std::getenv("FNCAP"), "%x,%x,%255[^,],%ld,%ld", &g_fnCapEntry, &g_fnCapRet, dir, &first, &n) == 5) left = n; t0 = now_us(); });
    if (pc == g_fnCapEntry || (pc == g_fnCapRet && mine >= 0 && r[1].w[0] == entryR1)) {
        bool take = true;
        if (pc == g_fnCapEntry) {
            if (now_us() - t0 < uint64_t(first) * 1000000u) take = false;
            else { mine = seq++; if (mine >= left) { mine = -1; take = false; } else entryR1 = r[1].w[0]; }
        }
        if (take) {
            char fn[300]; std::snprintf(fn, sizeof fn, "%s/fn_%04ld_%s.bin", dir, mine, pc == g_fnCapEntry ? "in" : "out");
            if (FILE* f = std::fopen(fn, "wb")) {
                std::fwrite(s.ls, 1, Spu::LS_SIZE, f);
                for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) { const uint32_t v = r[i].w[k]; const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 4, f); }
                std::fclose(f);
            }
            if (pc == g_fnCapRet) mine = -1;
        }
    }
    return edge_hle(r, s, pc);
}

// CULLTIME=1: time spent inside Edge's culling routine, against the whole Edge jobs ([culltime] every 2^16 calls)
static std::atomic<uint64_t> g_cullNs{0}, g_cullCalls{0};
static uint32_t edge_cull_time(spurt::Q* __restrict, Spu&, uint32_t pc) {
    static thread_local std::chrono::steady_clock::time_point t;
    static thread_local bool in = false;
    if (pc == 0x9e68) { t = std::chrono::steady_clock::now(); in = true; }
    else if (pc == 0x4400 && in) { in = false; g_cullNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); ++g_cullCalls; }
    return pc;
}

// Edge's triangle culling done natively (edge_cull.cpp) instead of by the SPU program. EDGENATIVE=0: SPU code;
// EDGECHECK=1: both, checking that ours keeps every triangle the SPU keeps ([edgecheck]); EDGEPOISON=1: native, and
// the per-vertex screen data the SPU routine would have written is filled with garbage (nothing later may read it).
static inline uint32_t rd32ls(const uint8_t* ls, uint32_t a) { const uint8_t* p = ls + (a & 0x3fffc); return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static thread_local uint64_t t_jobSeq = 0;   // jobs run by this worker (pairs the culling routine's entry and return)
static std::atomic<uint64_t> g_chkUnpaired{0};
static std::atomic<uint64_t> g_chkJobs{0}, g_chkBad{0}, g_chkUnhandled{0}, g_chkTrisSpu{0}, g_chkTrisOurs{0};
static uint32_t edge_hle(spurt::Q* __restrict r, Spu& s, uint32_t pc) {
    if ((pc == 0x7140 || pc == 0x12780 || pc == 0x12628) && g_fnCapEntry != pc) {   // edge_native.cpp (FNCAP of the routine itself: keep the SPU code)
        uint32_t a[8], ret = 0;
        for (int i = 0; i < 8; ++i) a[i] = r[3 + i].w[0];
        if (!edge_native_run(pc, s.ls, a, r[1].w[0], &ret)) return pc;
        // what the SPU routine leaves that may matter: r3, and its prologue's link register save at r1 + 16
        r[3].w[0] = ret;
        uint8_t* d = s.ls + ((r[1].w[0] + 16) & 0x3fff0);
        for (int k = 0; k < 4; ++k) { const uint32_t v = r[0].w[k]; d[4 * k] = uint8_t(v >> 24); d[4 * k + 1] = uint8_t(v >> 16); d[4 * k + 2] = uint8_t(v >> 8); d[4 * k + 3] = uint8_t(v); }
        return r[0].w[0] & 0x3fffc;
    }
    if (pc != 0x9e68 && pc != 0x4400) return pc;
    static const bool timing = std::getenv("CULLTIME") != nullptr, check = std::getenv("EDGECHECK") != nullptr,
                      poison = std::getenv("EDGEPOISON") != nullptr;
    static thread_local std::chrono::steady_clock::time_point t;
    static thread_local bool in = false, pending = false;
    static thread_local uint8_t* copy = nullptr;
    static thread_local uint8_t* orig = nullptr;     // EDGECHECK: entry state (local store + registers), dumped when ours misses a triangle
    static thread_local uint32_t ourCount, ourIdx, entryR1;
    static thread_local uint64_t entryJob;
    auto timeEnd = [&] {
        if (timing && in) { in = false; g_cullNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); ++g_cullCalls; }
    };
    if (pc == 0x9e68) {
        if (timing) { t = std::chrono::steady_clock::now(); in = true; }
        const uint32_t ctx = r[3].w[0], mode = r[4].w[0], r1 = r[1].w[0];
        if (check) {
            if (!copy) { copy = new uint8_t[Spu::LS_SIZE]; orig = new uint8_t[Spu::LS_SIZE + 128 * 16]; }
            std::memcpy(copy, s.ls, Spu::LS_SIZE);
            std::memcpy(orig, s.ls, Spu::LS_SIZE);
            for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) { const uint32_t v = r[i].w[k]; uint8_t* d = orig + Spu::LS_SIZE + 16 * i + 4 * k; d[0] = uint8_t(v >> 24); d[1] = uint8_t(v >> 16); d[2] = uint8_t(v >> 8); d[3] = uint8_t(v); }
            const EdgeCullResult cr = edge_cull_native(copy, ctx, mode, r1);
            if (pending) ++g_chkUnpaired;
            pending = cr.handled; entryJob = t_jobSeq; entryR1 = r1; ourCount = cr.count; ourIdx = rd32ls(copy, (ctx & 0x3fff0) + 0x50);
            if (!cr.handled) ++g_chkUnhandled;
            return pc;   // the SPU code runs as well
        }
        const EdgeCullResult cr = edge_cull_native(s.ls, ctx, mode, r1);
        if (!cr.handled) return pc;
        for (int k = 0; k < 4; ++k) r[3].w[k] = cr.count;
        if (poison) {
            const uint32_t nv = rd32ls(s.ls, (ctx & 0x3fff0) + 8) >> 16, out = rd32ls(s.ls, (ctx & 0x3fff0) + 0x14) & 0x3fff0;
            if (out + nv * 16 <= Spu::LS_SIZE) std::memset(s.ls + out, 0xA5, nv * 16);
        }
        timeEnd();
        return r[0].w[0] & 0x3fffc;   // back to the caller (0x4400)
    }
    timeEnd();
    if (pending && (entryJob != t_jobSeq || r[1].w[0] != entryR1)) { pending = false; ++g_chkUnpaired; }
    if (pending) {   // EDGECHECK: the SPU result is in s.ls / r3, ours in copy
        pending = false;
        const uint32_t spuCount = r[3].w[0];
        uint32_t k = 0; bool sup = true;
        for (uint32_t i = 0; i < spuCount && sup; i += 3) {
            while (k < ourCount && std::memcmp(copy + ourIdx + 2 * k, s.ls + ourIdx + 2 * i, 6)) k += 3;
            if (k >= ourCount) sup = false; else k += 3;
        }
        g_chkTrisSpu += spuCount / 3; g_chkTrisOurs += ourCount / 3;
        if (!sup) {   // EDGECHECK=dir: keep the job (cull_replay format) for analysis
            const long n = long(g_chkBad.load());
            const char* dir = std::getenv("EDGECHECK");
            if (n < 40 && dir && std::strlen(dir) > 1) {
                char fn[300];
                std::snprintf(fn, sizeof fn, "%s/cull_%04ld_in.bin", dir, n);
                if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(orig, 1, Spu::LS_SIZE + 128 * 16, f); std::fclose(f); }
                std::snprintf(fn, sizeof fn, "%s/cull_%04ld_out.bin", dir, n);
                if (FILE* f = std::fopen(fn, "wb")) {
                    std::fwrite(s.ls, 1, Spu::LS_SIZE, f);
                    for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) { const uint32_t v = r[i].w[k]; const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 4, f); }
                    std::fclose(f);
                }
            }
        }
        if (!sup && g_chkBad++ < 20) LOG("[edgecheck] job %llu: a triangle the SPU keeps is missing (SPU %u indices, ours %u)\n", (unsigned long long)g_chkJobs.load(), spuCount, ourCount);
        if ((++g_chkJobs & 0xffff) == 0)
            LOG("[edgecheck] %llu jobs, %llu with a missing triangle, %llu unpaired, %llu not handled, triangles SPU %llu ours %llu (+%.2f%%)\n", (unsigned long long)g_chkJobs.load(),
                (unsigned long long)g_chkBad.load(), (unsigned long long)g_chkUnpaired.load(), (unsigned long long)g_chkUnhandled.load(), (unsigned long long)g_chkTrisSpu.load(), (unsigned long long)g_chkTrisOurs.load(),
                g_chkTrisSpu ? 100.0 * (double(g_chkTrisOurs) - double(g_chkTrisSpu)) / double(g_chkTrisSpu) : 0.0);
    }
    return pc;
}

void run_job(const JobDesc& J) {
    static const bool cullCapOn = [] {
        if (std::getenv("CULLCAP")) spurt::g_spuHle[(0xbaea80u >> 4) % 64] = edge_cull_capture;
        else if (!std::getenv("EDGENATIVE") || std::atoi(std::getenv("EDGENATIVE")) != 0 || std::getenv("EDGECHECK")) spurt::g_spuHle[(0xbaea80u >> 4) % 64] = edge_hle;
        else if (std::getenv("CULLTIME")) spurt::g_spuHle[(0xbaea80u >> 4) % 64] = edge_cull_time;
        // all three hooks only look at the culling routine's entry and its return
        uint32_t* pcs = spurt::g_spuHlePcs[(0xbaea80u >> 4) % 64];
        pcs[0] = 0x9e68; pcs[1] = 0x4400;
        pcs[2] = 0x7140; pcs[3] = 0x12780; pcs[4] = 0x12628;   // native routines of edge_native.cpp (edge_hle)
        if (std::getenv("FNCAP")) {
            spurt::g_spuHle[(0xbaea80u >> 4) % 64] = edge_fn_capture;
            uint32_t e = 0, rt = 0;
            std::sscanf(std::getenv("FNCAP"), "%x,%x", &e, &rt);
            pcs[6] = e; pcs[7] = rt;
        }
        return true; }();
    (void)cullCapOn;
    ++t_jobSeq;
    const uint32_t jea = J.jea;
    const uint32_t eaBin = uint32_t(jd32(J, 4)) & ~0xFu;
    // one local store per job binary on each worker: the game interleaves the dispatcher (0xba3680) with the Edge
    // jobs, and with a single one the 64 KB Edge binary was copied in again for a third of its jobs. Each binary
    // keeps its own resident image, as it would while consecutive jobs use it. JOBONELS=1: the old single one.
    struct LsSlot { uint32_t bin = 0, resident = 0; Spu* spu = nullptr; };
    static thread_local LsSlot slots[4];
    static const bool oneLs = std::getenv("JOBONELS") != nullptr;
    LsSlot* slot = &slots[0];
    if (!oneLs) {
        int k = 0;
        while (k < 4 && slots[k].spu && slots[k].bin != eaBin) ++k;
        if (k == 4) k = 3;   // more binaries than slots: the last slot is shared
        slot = &slots[k];
        if (!slot->spu) slot->bin = eaBin;
    }
    if (!slot->spu) { slot->spu = new Spu; std::memset(slot->spu->ls, 0, Spu::LS_SIZE); }
    Spu* const spu = slot->spu;
    const uint32_t sizeBin = uint32_t(jd16(J, 8)) * 16, sizeDma = jd16(J, 0xa);
    const uint32_t sizeInOut = jd32(J, 0x14), sizeOut = jd32(J, 0x18);
    const uint32_t sizeCacheDma = jd32(J, 0x24);
    ++jobsRun;
    static thread_local uint32_t seenHere[8]; static thread_local unsigned nSeenHere = 0;   // binaries this worker has met
    bool newHere = true;
    for (unsigned k = 0; k < nSeenHere; ++k) if (seenHere[k] == eaBin) { newHere = false; break; }
    if (newHere && nSeenHere < 8) seenHere[nSeenHere++] = eaBin;
    if (newHere) {   // (a global lock per job was measurable with 8 workers)
        static std::mutex seenMx; static std::map<uint32_t, int> seen;
        std::lock_guard<std::mutex> lk(seenMx);
        if (!seen[eaBin]++) LOG("[job] binary %x: %u bytes (descriptor %x)\n", eaBin, sizeBin, jea);
        // First Edge geometry job: replace the game's three 3 MB output rings (one per SPU) with larger ones.
        // The rings are sized for the console, where the RSX frees them fast enough; here jobs kept waiting for
        // space (30 fps) and some frames did not fit at all. EDGERING=MB sets the size, 0 keeps the game's rings.
        static bool ringsDone = false;
        if (eaBin == 0xbaea80 && !ringsDone) {
            ringsDone = true;
            const uint32_t mb = std::getenv("EDGERING") ? uint32_t(std::atoi(std::getenv("EDGERING"))) : 24;
            // The game fills the buffer descriptions of SPUs 0-2 only (its job chain runs on 3 SPUs). Workers 3-5
            // get descriptions of their own, copied from SPU 0, with their own ring and RSX label (0x600...), so
            // six host threads can share the Edge jobs.
            for (uint32_t r = 0; r < uint32_t(std::min(g_workers, 8)) && mb; ++r) {
                const uint32_t e = 0x01061980u + 0x80u * r, size = mb << 20;
                if (r >= 3) {
                    std::memcpy(G->mem + e, G->mem + 0x01061980u, 0x80);
                    gcm::wr(e + 20, (gcm::rd(e + 20) & 0xfffff000u) | (0x600u + 0x10u * (r - 3)));
                }
                const uint32_t oldStart = gcm::rd(e), label = gcm::rd(e + 20) & 0xff0;
                // Edge keeps 29 bits of the RSX offset of its index buffers: the rings must lie below ioAddr + 512 MB.
                // 0x44000000 up is unused (user memory ends at 0x3D500000, the host heap starts at 0x60000000).
                const uint32_t start = 0x44000000u + r * (size + 0x1000);
                if (!oldStart || !label || start < gcm::ioAddr || uint64_t(start) + size - gcm::ioAddr >= 0x20000000ull) { LOG("[job] Edge ring %u left as it is (start %x, new %x)\n", r, oldStart, start); continue; }
                gcm::wr(e, start); gcm::wr(e + 4, start - gcm::ioAddr); gcm::wr(e + 8, start + size); gcm::wr(e + 12, start); gcm::wr(e + 24, start + size);
                gcm::wr(gcm::labels + label, start + size);
                LOG("[job] Edge ring %u: %x..%x (%u MB) instead of %x\n", r, start, start + size, mb, oldStart);
            }
        }
    }
    if (eaBin == EVENTFLAG_JOB) {
        const uint32_t ef = jd32(J, 0x34);
        const uint16_t bits = uint16_t(jd32(J, 0x3c));
        if (trace) LOG("[job] event flag %x |= %04x\n", ef, bits);
        set_event_flag(ef, bits);
        return;
    }
    Spu& s = *spu;
    s.mem = G->mem;
    s.reset();
    s.steps = 0;
    // the kernel area and the job context/descriptor are rewritten below; the 10 KB between them stay zero from the
    // creation of the local store (nothing writes there; clearing all 12 KB per job was 2-3 % of the workers). JOBCLEAR=1: as before
    static const bool clearAll = std::getenv("JOBCLEAR") != nullptr;
    if (clearAll) std::memset(s.ls, 0, 0x3000); else { std::memset(s.ls, 0, 0x400); std::memset(s.ls + LS_CTX, 0, LS_BIN - LS_CTX); }
    s.lssw(LS_STUB, 0x00003fff);   // stop 0x3fff: the job returned
    // SPURS kernel context (LS 0x100..): spuNum at 0x1C8 (cellSpursGetCurrentSpuId). Edge geometry keeps one output
    // ring per SPU, so every worker must report its own number.
    s.lssw(0x1C8, uint32_t(t_spuNum));
    std::memcpy(s.ls + LS_JOB, J.d, 256);
    // like the SPURS job manager: the local store is not cleared, and the binary stays resident while consecutive
    // jobs on this SPU use the same one (JOBRELOAD=1 restores the old clear-and-reload behaviour)
    static const bool reload = std::getenv("JOBRELOAD") != nullptr;
    uint32_t& residentBin = slot->resident;
    if (reload) std::memset(s.ls + 0x3000, 0, Spu::LS_SIZE - 0x3000);
    if (reload || residentBin != eaBin) { std::memcpy(s.ls + LS_BIN, G->mem + eaBin, std::min<uint32_t>(sizeBin, 0x30000)); residentBin = eaBin; }
    const uint32_t lsIo = (LS_BIN + sizeBin + 127) & ~127u;   // buffers follow the binary (its size covers .bss)
    // inputs, gathered in DMA list order, each 16-byte aligned
    uint32_t off = lsIo;
    size_t inPos = 0;
    int jobMode = -1;   // Edge culling mode of this job (job parameters, input 17)
    for (uint32_t k = 0; k < sizeDma / 8; ++k) {
        const uint32_t sz = jd32(J, 0x30 + 8 * k) & 0x7fff, ea = jd32(J, 0x34 + 8 * k);
        std::memcpy(s.ls + (off | (ea & 15)), (J.in.size() ? J.in.data() + inPos : G->mem + ea), sz);   // MFC list element: LS keeps the EA's low 4 bits
        {   // EDGECULL=n: replace the culling mode (low 2 bits of byte 1 of the 16-byte job parameters, input 17)
            static const int cullOverride = std::getenv("EDGECULL") ? std::atoi(std::getenv("EDGECULL")) : -1;
            if (eaBin == 0xbaea80 && k == 17 && sz == 16) jobMode = s.ls[(off | (ea & 15)) + 1] & 3;
            if (cullOverride >= 0 && eaBin == 0xbaea80 && k == 17 && sz == 16) {
                uint8_t* prm = s.ls + (off | (ea & 15));
                prm[1] = uint8_t((prm[1] & ~3) | (cullOverride & 3));
            }
        }
        {   // EDGEDUMP=n: descriptor and input buffers of Edge geometry jobs n..n+2 (counted over all workers)
            static const long first = std::getenv("EDGEDUMP") ? std::atol(std::getenv("EDGEDUMP")) : -1;
            static std::atomic<long> seenEdge{0};
            static thread_local long mine = -1;
            static const bool viewLog = std::getenv("EDGEVIEW") != nullptr;   // EDGEVIEW=1: local-to-world matrices the jobs get, when they change
            if (viewLog && eaBin == 0xbaea80 && k == 16 && sz == 48) {
                static std::mutex vm; static uint8_t last[48]; static int shown = 0;
                std::lock_guard<std::mutex> lk(vm);
                const uint8_t* src = s.ls + (off | (ea & 15));
                if (std::memcmp(last, src, 48) && shown++ < 400) {
                    std::memcpy(last, src, 48);
                    float m[12];
                    for (int q = 0; q < 12; ++q) { const uint32_t u = uint32_t(src[4 * q]) << 24 | src[4 * q + 1] << 16 | src[4 * q + 2] << 8 | src[4 * q + 3]; std::memcpy(&m[q], &u, 4); }
                    LOG("[edgeview] flip %llu from %08x: %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f\n", (unsigned long long)gcm::flips.load(), ea,
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
                }
            }
            if (first >= 0 && eaBin == 0xbaea80) {
                if (k == 0) {
                    mine = seenEdge++;
                    if (mine >= first && mine < first + 3) {
                        std::string ds; char b[16];
                        for (uint32_t o = 0; o < 0x80 + sizeDma; o += 4) { std::snprintf(b, sizeof b, " %08x", jd32(J, o)); ds += b; }
                        LOG("[edge] job %ld descriptor %x:%s\n", mine, jea, ds.c_str());
                    }
                }
                if (mine >= first && mine < first + 3) {
                    std::string ds; char b[16];
                    const uint8_t* src = s.ls + (off | (ea & 15));
                    for (uint32_t o = 0; o + 4 <= sz && o < 0xc0; o += 4) { std::snprintf(b, sizeof b, " %02x%02x%02x%02x", src[o], src[o + 1], src[o + 2], src[o + 3]); ds += b; }
                    LOG("[edge] job %ld input %u: %u bytes from %08x -> LS %05x:%s\n", mine, k, sz, ea, off | (ea & 15), ds.c_str());
                }
            }
        }
        inPos += sz;
        off += (sz + 15) & ~15u;
    }
    const uint32_t ioEnd = lsIo + ((std::max(sizeInOut, off - lsIo) + 127) & ~127u);
    const uint32_t oBuf = ioEnd;
    uint32_t cache = (oBuf + sizeOut + 127) & ~127u;
    s.lssw(LS_CTX + 0x00, lsIo);
    if (ioEnd + sizeOut > LS_STACK - 0x4000) LOG("[job] %x binary %x: buffers do not fit in local store (end 0x%x)\n", jea, eaBin, ioEnd + sizeOut);
    for (uint32_t k = 0; k < sizeCacheDma / 8 && k < 4; ++k) {
        const uint32_t sz = jd32(J, 0x30 + sizeDma + 8 * k) & 0x7fff, ea = jd32(J, 0x34 + sizeDma + 8 * k);
        std::memcpy(s.ls + (cache | (ea & 15)), (J.in.size() ? J.in.data() + inPos : G->mem + ea), sz);
        inPos += sz;
        s.lssw(LS_CTX + 4 + 4 * k, cache | (ea & 15));
        cache = (cache + (ea & 15) + sz + 127) & ~127u;
    }
    s.lssw(LS_CTX + 0x1c, oBuf);
    const uint32_t sizeScratch = uint32_t(jd16(J, 0x1e)) * 16;   // scratch buffer pointer @0x20
    const uint32_t sBuf = cache;
    s.lssw(LS_CTX + 0x20, sBuf);
    if (sBuf + sizeScratch > LS_STACK - 0x4000) LOG("[job] %x binary %x: scratch does not fit in local store (end 0x%x)\n", jea, eaBin, sBuf + sizeScratch);
    s.lssw(LS_CTX + 0x24, 5);   // DMA tag
    s.lssw(LS_CTX + 0x28, 0); s.lssw(LS_CTX + 0x2c, jea);
    s.r[1].sw(0, LS_STACK);
    s.r[3].sw(0, LS_CTX);
    s.r[4].sw(0, LS_JOB);
    s.r[0].sw(0, LS_STUB);
    s.pc = LS_BIN + 0x10;
    s.hookBelow = LS_BIN;
    {   // statically recompiled job binary (tools/spu2cpp.py), unless NOSPUREC is set
        static const bool off = std::getenv("NOSPUREC") != nullptr;
        const spurt::Compiled* cp = off ? nullptr : spurt::find_compiled(eaBin);
        if (cp && cp->lsBase == LS_BIN) { s.compiled = cp->fn; s.compiledLo = LS_BIN; s.compiledHi = LS_BIN + cp->size; }
        else s.compiled = nullptr;
    }
    bool done = false;
    s.onBranch = [&](Spu& sp, uint32_t t) {
        if (t == LS_STUB) { done = true; return false; }
        LOG("[job] binary %x branched to LS 0x%x (r3 %08x r4 %08x lr %08x): unsupported kernel call\n", eaBin, t, sp.r[3].w(0), sp.r[4].w(0), sp.r[0].w(0));
        return false;
    };
    s.onStop = [&](Spu& sp, uint32_t code) {
        if (code == 0x3fff) { done = true; return false; }
        LOG("[job] binary %x: stop 0x%x at LS 0x%x\n", eaBin, code, sp.pc);
        return false;
    };
    s.onWriteCh = [&](Spu& sp, uint32_t ch, uint32_t v) { if (trace) LOG("[job] wrch %u <- %08x\n", ch, v); return true; };
    s.onReadCh = [&](Spu& sp, uint32_t ch, uint32_t& v) { v = 0; if (trace) LOG("[job] rdch %u\n", ch); return true; };
    const auto tj = std::chrono::steady_clock::now();
    static const bool profOn = (spurt::g_spuProf = std::getenv("SPUPROF") != nullptr);
    (void)profOn;
    const uint64_t i0 = s.interpSteps, c0 = s.compiledCalls;
    g_workerSpu[t_spuNum & 15] = &s; g_workerSince[t_spuNum & 15] = now_us(); g_workerJob[t_spuNum & 15] = jea;
    {   // EDGETRACE=n: after n jobs, trace one Edge job of each culling mode in the interpreter (edgetrace_m<mode>.txt)
        static const long traceAfter = std::getenv("EDGETRACE") ? std::atol(std::getenv("EDGETRACE")) : -1;
        static std::atomic<bool> traced[4];
        if (traceAfter >= 0 && long(jobsRun.load()) > traceAfter && jobMode >= 0 && !traced[jobMode].exchange(true)) {
            char fn[64]; std::snprintf(fn, sizeof fn, "edgetrace_m%d.txt", jobMode);
            if (FILE* tf = std::fopen(fn, "w")) {
                std::fprintf(tf, "job %x mode %d ctx %x desc %x io %x\n", jea, jobMode, LS_CTX, LS_JOB, lsIo);
                s.compiled = nullptr; s.running = true;
                for (uint64_t k = 0; k < 20000000ull; ++k) {
                    const uint32_t pc0 = s.pc, w = s.lsw(pc0);
                    const bool ok = s.step();
                    const unsigned rt = (w >> 31) ? ((w >> 21) & 0x7f) : (w & 0x7f);
                    std::fprintf(tf, "%05x %08x r%u=%08x %08x %08x %08x\n", pc0, w, rt, s.r[rt].w(0), s.r[rt].w(1), s.r[rt].w(2), s.r[rt].w(3));
                    if (!ok) break;
                }
                std::fclose(tf);
            }
        }
    }
    {   // EDGESTAT=n: after n jobs, run 4000 Edge jobs in the interpreter and report what the culling routine returns
        static const long statAfter = std::getenv("EDGESTAT") ? std::atol(std::getenv("EDGESTAT")) : -1;
        static std::atomic<int> statLeft{4000};
        if (statAfter >= 0 && long(jobsRun.load()) > statAfter && jobMode >= 0 && s.pc == LS_BIN + 0x10 && statLeft.fetch_sub(1) > 0) {
            s.compiled = nullptr; s.running = true;
            long cullRet = -1; uint32_t prm0 = 0, prm2 = 0;
            std::string mfc;
            static const long traceIdx = std::getenv("EDGETRACEIDX") ? std::atol(std::getenv("EDGETRACEIDX")) : -1;   // also trace jobs with this index count
            std::string tr;
            for (uint64_t k = 0; k < 20000000ull; ++k) {
                if (s.pc == 0x9e68) { const uint32_t a3 = s.r[3].w(0) & 0x3fff0; prm0 = s.lsw(a3); prm2 = s.lsw(a3 + 8); }
                if (s.pc == 0x4400) cullRet = long(s.r[3].w(0));
                {
                    const uint32_t w = s.lsw(s.pc);
                    if ((w >> 21) == 0x10D && ((w >> 7) & 0x7f) == 21) {   // MFC command: opcode, size, EA
                        char mb[40];
                        std::snprintf(mb, sizeof mb, " %02x:%x@%08x", s.r[w & 0x7f].w(0) & 0xff, s.mfcSize, s.mfcEal);
                        if (mfc.size() < 400) mfc += mb;
                    }
                }
                const uint32_t wAt = s.lsw(s.pc);
                const bool isMfc = (wAt >> 21) == 0x10D && ((wAt >> 7) & 0x7f) == 21;
                const uint32_t mfcOp = isMfc ? (s.r[wAt & 0x7f].w(0) & 0xff) : 0, lsa = s.mfcLsa & 0x3ff80, mea = s.mfcEal;
                if (isMfc && mfcOp == 0x22 && mfc.size() < 30000) {   // the command buffer hole: what is there now, what the job puts
                    char mb[24];
                    const uint32_t ls0 = s.mfcLsa & 0x3fffc;
                    char gb[64];
                    std::snprintf(gb, sizeof gb, "\n      RSX get %07x put %07x", gcm::rd(gcm::ctrl + 4), gcm::rd(gcm::ctrl)); mfc += gb;
                    std::snprintf(mb, sizeof mb, "\n      hole@%08x was:", mea); mfc += mb;
                    for (uint32_t o = 0; o < 0x30 && o < s.mfcSize; o += 4) { std::snprintf(mb, sizeof mb, " %08x", rd32m(mea + o)); mfc += mb; }
                    mfc += "\n      job puts:";
                    for (uint32_t o = 0; o < 0x800 && o < s.mfcSize; o += 4) { std::snprintf(mb, sizeof mb, " %08x", s.lsw(ls0 + o)); mfc += mb; }
                }
                const uint32_t pcT = s.pc;
                const bool more = s.step();
                if (traceIdx >= 0 && tr.size() < 200000000) {
                    char tb[80];
                    const unsigned rt = (wAt >> 31) ? ((wAt >> 21) & 0x7f) : (wAt & 0x7f);
                    tr.append(tb, size_t(std::snprintf(tb, sizeof tb, "%05x %08x r%u=%08x %08x %08x %08x\n", pcT, wAt, rt, s.r[rt].w(0), s.r[rt].w(1), s.r[rt].w(2), s.r[rt].w(3))));
                }
                if (isMfc && (mfcOp == 0xd0 || mfcOp == 0xb4 || mfcOp == 0xb0) && mfc.size() < 2000) {   // atomic line: what was read / written
                    char mb[24];
                    std::snprintf(mb, sizeof mb, "\n      %02x@%08x:", mfcOp, mea); mfc += mb;
                    for (uint32_t o = 0; o < 0x40; o += 4) { std::snprintf(mb, sizeof mb, " %08x", s.lsw(lsa + o)); mfc += mb; }
                }
                if (!more) break;
            }
            if (traceIdx >= 0 && long(prm2 & 0xffff) == traceIdx) {
                static std::atomic<bool> doneCulled{false}, doneVisible{false};
                std::atomic<bool>& d = cullRet > 0 ? doneVisible : doneCulled;
                if (!d.exchange(true))
                    if (FILE* tf = std::fopen(cullRet > 0 ? "edgetrace_visible.txt" : "edgetrace_culled.txt", "w")) {
                        std::fprintf(tf, "job %x params %08x %08x cull returned %ld\n", jea, prm0, prm2, cullRet);
                        std::fwrite(tr.data(), 1, tr.size(), tf); std::fclose(tf);
                    }
            }
            LOG("[edgestat] mode %d params %08x %08x cull returned %ld steps %llu mfc%s\n", jobMode, prm0, prm2, cullRet, (unsigned long long)s.steps, mfc.c_str());
        }
    }
    if (s.pc == LS_BIN + 0x10) s.run(200000000ull);   // not traced above
    g_workerJob[t_spuNum & 15] = 0;
    if (eaBin == 0xbaea80) {   // the job's last MFC transfer is the command block it put into its hole: does it draw?
        const uint32_t l0 = s.mfcLsa & 0x3fffc, n = std::min<uint32_t>(s.mfcSize, 0x1000);
        for (uint32_t o = 0; o + 8 <= n; o += 4)
            if (s.lsw(l0 + o) == 0x00041808 && s.lsw(l0 + o + 4) != 0) { ++jobDraws; break; }
    }
    interpTot += s.interpSteps - i0; callsTot += s.compiledCalls - c0;
    {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tj).count());
        static std::mutex bmx; static std::map<uint32_t, std::pair<uint64_t, uint64_t>> perBin; static auto last = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(bmx);
        auto& e = perBin[eaBin]; e.first += 1; e.second += ns;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(4)) {
            last = std::chrono::steady_clock::now();
            for (auto& kv : perBin) LOG("[jobs]   binary %x: %llu jobs, %.1f us/job\n", kv.first, (unsigned long long)kv.second.first, kv.second.second / 1e3 / std::max<uint64_t>(1, kv.second.first));
            if (std::getenv("EDGESTAT2")) LOG("[edgenative] native / left to the SPU: 0x7140 %llu/%llu, 0x12780 %llu/%llu, 0x12628 %llu/%llu\n", (unsigned long long)g_edgeNativeCalls[0], (unsigned long long)g_edgeNativeFallbacks[0],
                (unsigned long long)g_edgeNativeCalls[1], (unsigned long long)g_edgeNativeFallbacks[1], (unsigned long long)g_edgeNativeCalls[2], (unsigned long long)g_edgeNativeFallbacks[2]);
            if (g_cullCalls) { LOG("[culltime] %llu culling calls, %.1f us each\n", (unsigned long long)g_cullCalls.load(), g_cullNs.load() / 1e3 / g_cullCalls.load()); g_cullNs = 0; g_cullCalls = 0; }
            perBin.clear();
            if (spurt::g_spuProf) {
                const int row = int((0xbaea80u >> 4) % 64);
                std::vector<std::pair<uint64_t, int>> v;
                for (int c = 0; c < 256; ++c) if (spurt::g_spuProfCnt[row][c]) v.push_back({spurt::g_spuProfCnt[row][c], c});
                std::sort(v.rbegin(), v.rend());
                std::string out; char b[48];
                for (size_t k = 0; k < v.size() && k < 12; ++k) { std::snprintf(b, sizeof b, " %x:%llu", 0x3000 + v[k].second * 0x400, (unsigned long long)v[k].first); out += b; }
                LOG("[spuprof] edge chunks (LS addr:entries):%s\n", out.c_str());
                spurt::spu_slow_report();
                std::memset(spurt::g_spuProfCnt, 0, sizeof spurt::g_spuProfCnt);
            }
        }
    }
    spuSteps += s.steps; spuNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tj).count());
    {
        static std::mutex repMx;
        std::lock_guard<std::mutex> rl(repMx);
        static auto lastRep = std::chrono::steady_clock::now();
        static uint64_t j0 = 0, st0 = 0, ns0 = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRep > std::chrono::seconds(4)) {
            const double dt = std::chrono::duration<double>(now - lastRep).count();
            static uint64_t ip0 = 0, ca0 = 0, sl0 = 0, jd0 = 0, rb0 = 0;
            const uint64_t jd = jobDraws, rb = gcm::g_rsxBegins;
            LOG("[jobs] Edge jobs with a draw %.0f/s, draws executed by the RSX %.0f/s\n", (jd - jd0) / dt, (rb - rb0) / dt);
            jd0 = jd; rb0 = rb;
            const uint64_t sl = spurt::g_spuSlowEntries;
            LOG("[jobs] %.0f jobs/s, SPU busy %.0f%%, interpreted %.1f M instr/s, %.2f M compiled entries/s, %.0f slow-path entries/s\n", (jobsRun - j0) / dt,
                (spuNs - ns0) / dt / 1e7 / g_workers, (interpTot - ip0) / dt / 1e6, (callsTot - ca0) / dt / 1e6, (sl - sl0) / dt);
            lastRep = now; j0 = jobsRun; st0 = spuSteps; ns0 = spuNs; ip0 = interpTot; ca0 = callsTot; sl0 = sl;
        }
    }
    if (std::getenv("HOLETRACE")) {   // descriptor words pointing into the command rings (holes) and what they hold now
        std::string hs; char b[48];
        for (uint32_t o = 0x30; o < 0x100; o += 4) {
            const uint32_t v = jd32(J, o);
            if (v >= 0x3223e980u && v < 0x32a3e980u) { std::snprintf(b, sizeof b, " [+%x] io %x = %08x %08x", o, v - 0x30100000u, rd32m(v & ~3u), rd32m((v & ~3u) + 4)); hs += b; }
        }
        LOG("[job] %x bin %x %s holes:%s\n", jea, eaBin, done ? "done" : "STOPPED", hs.c_str());
    }
    if (!done) {
        std::string hs; char b[16];
        for (uint32_t o = 0; o < 0x100; o += 4) { std::snprintf(b, sizeof b, " %08x", jd32(J, o)); hs += b; }
        LOG("[job] descriptor:%s  (binary 0x%x bytes at LS 0x%x, io at 0x%x)\n", hs.c_str(), sizeBin, LS_BIN, lsIo);
        const uint32_t pe = s.r[24].w(0);
        LOG("[job]   last DMA: lsa %x ea %x size %x; r24 %x -> %08x; r10 %x r12 %x r17 %x r18 %x\n", s.mfcLsa, s.mfcEal, s.mfcSize, pe, pe < 0xF0000000u ? rd32m(pe & ~3u) : 0,
            s.r[10].w(0), s.r[12].w(0), s.r[17].w(0), s.r[18].w(0));
    }
    if (trace || !done) LOG("[job] %x binary %x (in %u B, inout %u, out %u): %s after %llu steps (pc 0x%x, bad %08x)\n", jea, eaBin, off - lsIo, sizeInOut, sizeOut,
                            done ? "done" : "STOPPED", (unsigned long long)s.steps, s.pc, s.badInstr);
}

// worker pool: the SPUs of the job chain (max contention 3 on the PS3); SYNC / GUARD / END wait until all are idle
std::mutex qmx;
std::condition_variable qcv, idlecv;
std::deque<JobDesc> queue;
int busy = 0;
// Wakeups cost more than most jobs (~140k jobs/s in a race): jobs the manager reads one after the other are handed
// over together (flush_jobs) and only sleeping workers are woken; the manager is woken only when all went idle.
// JOBBATCH=0: one job at a time, every worker / manager notified each time (old).
int sleepers = 0, idleWaiters = 0;
const bool g_jobBatch = !(std::getenv("JOBBATCH") && std::getenv("JOBBATCH")[0] == '0');
std::vector<JobDesc> g_batch;   // manager thread only
// Edge geometry writes its output into one ring per SPU and waits for the RSX (through a label) when the ring is
// full. The game only hands a frame to the RSX once all its jobs are done, so a frame that needs more than a whole
// ring can never finish: the job waits for the RSX and the RSX has nothing to run. The console never gets there;
// here it can happen (e.g. the title screen camera sits at the world origin and sees far too much). When a job has
// been running for a long time while the RSX is idle, declare that SPU's ring free: the frame overwrites some of
// its own earliest geometry, but the game goes on.
std::atomic<uint64_t> g_ringRescues{0};
void ring_watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!gcm::ctrl || !gcm::labels || gcm::rd(gcm::ctrl) != gcm::rd(gcm::ctrl + 4)) continue;
        const uint64_t now = now_us();
        for (int w = 0; w < std::min(g_workers, 8); ++w) {
            if (!g_workerJob[w].load() || now - g_workerSince[w].load() < 150000) continue;
            const uint32_t e = 0x01061980u + 0x80u * uint32_t(w);   // that SPU's output buffer info
            const uint32_t cur = gcm::rd(e + 12), label = gcm::rd(e + 20) & 0xff0;
            if (!cur || !label) continue;
            gcm::wr(gcm::labels + label, cur);
            g_workerSince[w] = now;
            if (g_ringRescues++ < 5 || g_ringRescues % 200 == 0) LOG("[jobs] SPU %d waited for Edge output space with the RSX idle: ring released (%llu times)\n", w, (unsigned long long)g_ringRescues.load());
        }
    }
}

void worker(int idx) {
    t_tid = 0x01000210 + idx;
    if (idx == 0) std::thread(ring_watchdog).detach();
    t_spuNum = idx;
    { char nm[16]; std::snprintf(nm, sizeof nm, "spu%d", idx); ripprof_register_thread(nm); }
    host_thread_core(false);
    for (;;) {
        JobDesc jd;
        {
            std::unique_lock<std::mutex> lk(qmx);
            if (queue.empty()) { ++sleepers; qcv.wait(lk, [] { return !queue.empty(); }); --sleepers; }
            jd = std::move(queue.front()); queue.pop_front(); ++busy;
        }
        run_job(jd);
        bool wake;
        { std::lock_guard<std::mutex> lk(qmx); --busy; wake = !g_jobBatch || (busy == 0 && queue.empty() && idleWaiters > 0); }
        if (wake) idlecv.notify_all();
    }
}
void flush_jobs() {
    if (g_batch.empty()) return;
    const int n = int(g_batch.size());
    int sl;
    {
        std::lock_guard<std::mutex> lk(qmx);
        for (auto& j : g_batch) queue.push_back(std::move(j));
        sl = sleepers;
    }
    g_batch.clear();
    if (sl <= 0) return;
    if (n >= sl) qcv.notify_all(); else for (int k = 0; k < n; ++k) qcv.notify_one();
}
void submit(uint32_t jea) {
    JobDesc jd;
    jd.jea = jea;
    std::memcpy(jd.d, G->mem + jea, 256);
    static const bool snapIn = !std::getenv("NOJOBSNAP");
    if (snapIn && g_workers > 1 && jd32(jd, 4) != 0 && (jd32(jd, 4) & ~0xFu) != EVENTFLAG_JOB) {
        const uint32_t sizeDma = jd16(jd, 0xa), sizeCacheDma = jd32(jd, 0x24);
        size_t total = 0;
        for (uint32_t k = 0; k < sizeDma / 8; ++k) total += jd32(jd, 0x30 + 8 * k) & 0x7fff;
        for (uint32_t k = 0; k < sizeCacheDma / 8 && k < 4; ++k) total += jd32(jd, 0x30 + sizeDma + 8 * k) & 0x7fff;
        jd.in.resize(total ? total : 1);
        size_t pos = 0;
        for (uint32_t k = 0; k < sizeDma / 8; ++k) { const uint32_t sz = jd32(jd, 0x30 + 8 * k) & 0x7fff; std::memcpy(jd.in.data() + pos, G->mem + jd32(jd, 0x34 + 8 * k), sz); pos += sz; }
        for (uint32_t k = 0; k < sizeCacheDma / 8 && k < 4; ++k) { const uint32_t sz = jd32(jd, 0x30 + sizeDma + 8 * k) & 0x7fff; std::memcpy(jd.in.data() + pos, G->mem + jd32(jd, 0x34 + sizeDma + 8 * k), sz); pos += sz; }
    }
    if (g_workers == 1) { run_job(jd); return; }
    if (g_jobBatch) { g_batch.push_back(std::move(jd)); static const int batchN = std::getenv("JOBBATCHN") ? std::atoi(std::getenv("JOBBATCHN")) : 2 * g_workers; if (int(g_batch.size()) >= batchN) flush_jobs(); return; }
    { std::lock_guard<std::mutex> lk(qmx); queue.push_back(std::move(jd)); }
    qcv.notify_one();
}
// MGRSTAT=1: per 4 s, how often and how long the manager waited for the workers, by reason (0 sync, 1 lwsync,
// 2 guard, 3 end, 4 event flag job), and the jobs it handed out
std::atomic<uint64_t> g_wiN[5], g_wiUs[5], g_mgrJobs{0}, g_mgrEndUs{0}, g_mgrGuardUs{0};
const bool g_mgrStat = std::getenv("MGRSTAT") != nullptr;
void wait_idle(int why = 0) {
    if (g_workers == 1) return;
    const uint64_t t0 = g_mgrStat ? now_us() : 0;
    flush_jobs();
    {
        std::unique_lock<std::mutex> lk(qmx);
        ++idleWaiters;
        idlecv.wait(lk, [] { return queue.empty() && busy == 0; });
        --idleWaiters;
    }
    if (g_mgrStat) { ++g_wiN[why]; g_wiUs[why] += now_us() - t0; }
}

std::atomic<uint32_t> g_mgrPc{0}, g_mgrWhat{0};   // diagnostics: command list position and what the manager waits for (1 guard, 2 end, 3 sync)
// how long the manager keeps yielding before it sleeps: a sleep is 1 ms or more on Windows, and the next kick then starts
// the image's geometry jobs late (at 144 Hz every other image missed its vblank at the start grid). MGRSPIN=us.
const uint64_t g_mgrSpinUs = std::getenv("MGRSPIN") ? std::strtoull(std::getenv("MGRSPIN"), nullptr, 10) : 300;
void kick() { ++kicks; if (mgrSleeping.load()) { std::lock_guard<std::mutex> lk(kickMx); kickCv.notify_all(); } }
// one round of the manager's idle wait: yields at first, then sleeps until a kick newer than k (or 2 ms)
void mgr_idle(uint64_t t0, uint64_t k) {
    if (now_us() - t0 < g_mgrSpinUs) { std::this_thread::yield(); return; }
    std::unique_lock<std::mutex> lk(kickMx);
    mgrSleeping = true;
    if (kicks.load() == k) kickCv.wait_for(lk, std::chrono::milliseconds(2));
    mgrSleeping = false;
}
uint32_t g_mgrHist[64][3]; unsigned g_mgrHistN = 0;   // diagnostics: last commands read (pc, low word, time us)
void manager() {
    t_tid = 0x01000202;
    ripprof_register_thread("jobmgr");
    if (g_workers > 1) for (int k = 0; k < g_workers; ++k) std::thread(worker, k).detach();
    uint32_t pc = list;
    std::vector<uint32_t> calls;
    // The event flag job wakes the render thread, which then rewrites this command list for the next frame. The SPURS
    // kernel has grabbed the following commands by then; here the job is held back until the manager has read on to
    // the next job or wait point (guard, end), or it could follow the new list from the middle and end up on a stale
    // END: the game froze every minute or two at 144 Hz.
    std::vector<uint32_t> pendingEf;
    const auto flushEf = [&] { for (uint32_t j : pendingEf) submit(j); pendingEf.clear(); };
    for (;;) {
        const uint64_t kRead = kicks.load();   // kicks seen before this command was read (END waits for a newer one)
        const uint64_t w = rd64m(pc);
        g_mgrPc = pc; g_mgrWhat = 0;
        { const unsigned h = g_mgrHistN++ % 64; g_mgrHist[h][0] = pc; g_mgrHist[h][1] = uint32_t(w); g_mgrHist[h][2] = uint32_t(now_us()); }
        const uint32_t ea = uint32_t(w) & ~7u;
        const uint32_t op = uint32_t(w) & 7, sub = (uint32_t(w) >> 3) & 0xf;
        if (w == 0) {   // a zero may be a slot the game is rewriting right now: give it a moment before skipping it
                        // (skipping a NEXT being written left the manager on a stale END for good, ~1 run in 2 at 144 Hz)
            bool changed = false;
            flush_jobs();
            for (const uint64_t t0 = now_us(); now_us() - t0 < 2000;) { if (rd64m(pc) != 0) { changed = true; break; } std::this_thread::yield(); }
            if (changed) continue;
            pc += 8; continue;
        }
        if (op == 0) {
            if ((rd32m(ea + 4) & ~0xFu) == EVENTFLAG_JOB) { wait_idle(4); pendingEf.push_back(ea); pc += 8; continue; }
            flushEf(); submit(ea); ++g_mgrJobs; pc += 8; continue;
        }
        flush_jobs();   // any other command: the jobs read so far go to the workers now
        switch (op) {
            case 1: pc = ea; continue;                                  // RESET_PC
            case 2: case 5: g_mgrWhat = 3; wait_idle(op == 2 ? 0 : 1); pc += 8; continue;             // SYNC / LWSYNC / labels / FLUSH
            case 3: pc = ea; continue;                                  // NEXT
            case 4: calls.push_back(pc + 8); pc = ea; continue;         // CALL
            case 6: {                                                   // JOBLIST {u32 numJobs, u32 sizeOfJob, u64 eaJobList}
                flushEf();
                const uint32_t n = rd32m(ea), sz = rd32m(ea + 4), lst = uint32_t(rd64m(ea + 8));
                for (uint32_t k = 0; k < n; ++k) submit(lst + k * sz);
                flush_jobs();
                pc += 8; continue;
            }
            default: break;
        }
        if (sub == 1) {                                                 // GUARD: wait for cellSpursJobGuardNotify
            g_mgrWhat = 1;
            flushEf();
            wait_idle(2);
            const uint64_t tg0 = g_mgrStat ? now_us() : 0;
            auto g = guard_of(uint32_t(w) & ~0x7fu);
            for (const uint64_t t0 = now_us();;) { const uint64_t k = kicks.load(); if (g->remaining.load() <= 0) break; mgr_idle(t0, k); }
            if (g->autoReset) g->remaining = g->count;
            if (g_mgrStat) g_mgrGuardUs += now_us() - tg0;
            pc += 8; continue;
        }
        if (sub == 14) { if (!calls.empty()) { pc = calls.back(); calls.pop_back(); } else pc += 8; continue; }   // RET
        if (sub == 15 || sub == 0) {                                    // END / ABORT: idle until the command is rewritten
            g_mgrWhat = 2;
            flushEf();
            wait_idle(3);
            const uint64_t te0 = g_mgrStat ? now_us() : 0;
            struct EndT { uint64_t t; ~EndT() { if (g_mgrStat) g_mgrEndUs += now_us() - t; } } endT{te0};
            if (g_mgrStat) {
                static uint64_t last = 0;
                if (te0 - last > 4000000) {
                    if (last) {
                        const double k = 1000.0 / double(te0 - last);
                        LOG("[mgrstat] per ms: jobs %.2f | waits n/ms-per-s: sync %.2f/%.0f lwsync %.2f/%.0f guard %.2f/%.0f end %.2f/%.0f ef %.2f/%.0f | idle at guard %.0f, at end %.0f ms/s\n",
                            g_mgrJobs * k, g_wiN[0] * k, g_wiUs[0] * k, g_wiN[1] * k, g_wiUs[1] * k, g_wiN[2] * k, g_wiUs[2] * k, g_wiN[3] * k, g_wiUs[3] * k, g_wiN[4] * k, g_wiUs[4] * k, g_mgrGuardUs * k, g_mgrEndUs * k);
                    }
                    last = te0; g_mgrJobs = 0; g_mgrGuardUs = 0; g_mgrEndUs = 0;
                    for (int i = 0; i < 5; ++i) { g_wiN[i] = 0; g_wiUs[i] = 0; }
                }
            }
            // Like the SPURS kernel: an idle chain reads its list again only when the game kicks it (cellSpursRunJobChain,
            // or a guard notify that opens the guard), i.e. after the game finished rewriting it. Resuming as soon as the
            // END word changed raced with those rewrites (a NEXT half written -> stale END -> game frozen). A changed word
            // without any kick is still followed after 20 ms.
            const uint64_t k0 = kRead;
            uint64_t changedAt = 0;
            for (const uint64_t t0 = now_us(); kicks.load() == k0;) {
                if (rd64m(pc) != w) { if (!changedAt) changedAt = now_us(); else if (now_us() - changedAt > 20000) break; }
                mgr_idle(t0, k0);
            }
            continue;
        }
        pc += 8;                                                        // SET_LABEL and the rest
    }
}

void start(uint32_t commandList) {
    list = commandList;
    if (started.exchange(true)) return;
    th = std::thread(manager);
    th.detach();
}
} // namespace jobs

// ------------------------------------------------------------------ system library imports
uint32_t g_jobChainEntry = 0;   // u64 command list of the (single) SPURS job chain
const bool g_jobsOn = !std::getenv("NOJOBS");   // run the job chain on the SPU interpreter
// what each guest thread is currently blocked in (for lv2_dump_waits)
// What each guest thread is blocked in (lv2_dump_waits, WAITPROF). Every import and syscall passes through here,
// thousands of times per frame, so entering and leaving a scope must cost next to nothing: one slot per thread,
// written without locks; the list of slots is only locked when a thread makes its slot.
struct WaitSlot { char what[40] = ""; uint32_t lr = 0, tid = 0; std::atomic<uint64_t> since{0}; };
std::mutex g_waitMx;
std::vector<WaitSlot*> g_waitSlots;
thread_local WaitSlot* t_waitSlot = nullptr;
// ------------------------------------------------------------------ sound: cellAudio port + SPURS tasks on the host
// The game mixes with MultiStream: a SPURS task (SPU program) that the 'MultiStream PPU Thread' signals for every
// block, and whose output that thread copies into the cellAudio port ring. Here every such task gets its own SPU
// (interpreter) on a host thread, and a host thread plays the ring through SDL and advances its read index.
namespace snd {
std::atomic<uint64_t> g_sndSys[4];   // SPURS task syscalls of the sound task (statistics)
struct Task {
    Spu spu;
    std::mutex m; std::condition_variable cv;
    bool signalled = false;
    uint32_t taskset = 0, id = 0, ctxSave = 0;
};
std::mutex g_tm;
std::atomic<uint64_t> g_signals{0}, g_passes{0};
std::atomic<uint64_t> g_waits{0}, g_busySum{0}, g_busyMax{0}, g_wake{0}, g_slow{0};   // mixer pass statistics
std::vector<Task*> g_tasks;
uint32_t be32(uint32_t a) { uint32_t x; std::memcpy(&x, G->mem + a, 4); return __builtin_bswap32(x); }

// Our ATRAC3 decoder in place of the system software's module (flashATRAC.pic). The mixer calls the module's decode
// function with r3 = LS of the compressed frame (2 sound units of 0x98 bytes), r4 = LS of the output (channel c at
// r4 + c * 0x1000), r6 = 0x400, r7 = LS of the voice's context (+0x20: flags, top byte 2 when the voice starts a new
// sound, 1 otherwise; +0x34: the context's address in main memory, used here as the voice's identity).
// Per voice the module alternates: on a start it decodes nothing and asks for a frame (returns 0x200); then one call
// decodes the frame it is given (returns 0) and the next one hands out the rest (returns 0x200, a new frame please).
// Each call writes 1024 big-endian 16-bit samples per channel (inverted polarity, x * -32768) and 4 more samples
// extrapolated from the last two: after a decode, the second half of the previous frame and the first half of the
// new one; on the next call, the whole new frame. Returns r3 = {0 or 0x200, 2, 0x98, 0}.
// Matches the module's output to +-1 in the 16-bit samples (FFmpeg-exact decoding; see NOTAS_NOCHE.md).
struct AtracVoice {
    static constexpr int kMax = 6;
    int nch = 0;
    std::unique_ptr<atrac3::Decoder> dec;
    float cur[kMax][1024] = {}, prevHalf[kMax][512] = {};
    int stage = 0;   // 0 start (next call asks for a frame), 1 a frame comes now, 2 second half
    uint64_t frames = 0, calls = 0, firstUs = 0;   // statistics (ATRACSTAT)
    uint32_t resUnit = 0, resBits = 0;   // result block: offset of the last decoded sound unit, bits the decoder read of it
    void reset(int channels) {
        if (channels != nch || !dec) { nch = channels; dec = std::make_unique<atrac3::Decoder>(channels, 0x98); }
        else dec->reset();
        std::memset(cur, 0, sizeof cur); std::memset(prevHalf, 0, sizeof prevHalf); stage = 0;
    }
};
std::map<uint32_t, std::unique_ptr<AtracVoice>> g_atracVoices;   // only the mixer task thread uses it
std::atomic<uint64_t> g_atracFrames{0}, g_atracBad{0};
// ATRACSEQ=from,to (seconds since launch): one line per decode call (voice, flags, first frame bytes, r3 returned)
void atrac_seq(const char* who, uint32_t key, uint32_t flags, const uint8_t* fr, uint32_t ret) {
    static double from = -1, to = 0;
    if (from < 0) { from = 1e9; if (const char* e = std::getenv("ATRACSEQ")) std::sscanf(e, "%lf,%lf", &from, &to); }
    const double t = log_seconds();
    if (t < from || t > to) return;
    LOG("[seq] %s %8.3f voice %08x flags %08x frame %02x%02x%02x%02x ret %x%c", who, t, key, flags, fr ? fr[0] : 0, fr ? fr[1] : 0, fr ? fr[2] : 0, fr ? fr[3] : 0, ret, 10);
}
// channels of a voice: second byte of its context flags (2 or 6; the frame holds that many 0x98-byte sound units)
int atrac_channels(uint32_t flags) { const int n = int((flags >> 16) & 0xff); return n >= 1 && n <= AtracVoice::kMax ? n : 2; }
// one call: the n x 1028 samples to write (n = channels of the voice); returns the value for r3
uint32_t atrac_compute(const uint8_t* frame, uint32_t key, uint32_t flags, int16_t (&s)[AtracVoice::kMax][1028], int& n) {
    auto& slot = g_atracVoices[key];
    if (!slot) slot = std::make_unique<AtracVoice>();
    AtracVoice& v = *slot;
    n = atrac_channels(flags);
    if (!v.firstUs) v.firstUs = now_us();
    ++v.calls;
    if ((flags >> 24) == 2 || v.nch != n) v.reset(n);
    float win[AtracVoice::kMax][1024];
    float* o[AtracVoice::kMax];
    for (int c = 0; c < n; ++c) o[c] = v.cur[c];
    uint32_t ret;
    if (v.stage == 1) {
        for (int c = 0; c < n; ++c) std::memcpy(v.prevHalf[c], v.cur[c] + 512, sizeof v.prevHalf[c]);
        if (frame) { if (!v.dec->decode(frame, o)) ++g_atracBad; ++g_atracFrames; ++v.frames; v.resUnit = 0x98u * uint32_t(n - 1); v.resBits = uint32_t(v.dec->bitsUsed(n - 1)); }
        for (int c = 0; c < n; ++c) { std::memcpy(win[c], v.prevHalf[c], 512 * 4); std::memcpy(win[c] + 512, v.cur[c], 512 * 4); }
        v.stage = 2; ret = 0;
    } else {
        if (v.stage == 0 && frame && (frame[0] >> 2) == 0x28) {   // start: the first frame primes the decoder; out goes
            if (!v.dec->decode(frame, o)) ++g_atracBad;            // (silence) + the first half of that frame
            v.resUnit = 0x98u * uint32_t(n - 1); v.resBits = uint32_t(v.dec->bitsUsed(n - 1));
            for (int c = 0; c < n; ++c) { std::memset(win[c], 0, 512 * 4); std::memcpy(win[c] + 512, v.cur[c], 512 * 4); }
        } else for (int c = 0; c < n; ++c) std::memcpy(win[c], v.cur[c], 1024 * 4);
        v.stage = 1; ret = 0x200;
    }
    for (int c = 0; c < n; ++c) {
        for (int i = 0; i < 1024; ++i) s[c][i] = int16_t(std::clamp(std::lrintf(win[c][i] * -32768.0f), -32768L, 32767L));
        for (int i = 0; i < 4; ++i)   // 16-bit wrap-around like the module (no clamping)
            s[c][1024 + i] = int16_t(uint16_t(int(s[c][1023]) + (i + 1) * (int(s[c][1023]) - int(s[c][1022]))));
    }
    return ret;
}
bool atrac_decode_call(Spu& sp) {
    const uint32_t in = sp.r[3].w(0) & 0x3ffff, out = sp.r[4].w(0) & 0x3ffff, ctx = sp.r[7].w(0) & 0x3ffff;
    const uint32_t flags = sp.lsw(ctx + 0x20);
    int16_t s[AtracVoice::kMax][1028];
    int n = 0;
    const uint32_t ret = atrac_compute(in + 0x98u * uint32_t(atrac_channels(flags)) <= Spu::LS_SIZE ? sp.ls + in : nullptr, sp.lsw(ctx + 0x34), flags, s, n);
    atrac_seq("ours", sp.lsw(ctx + 0x34), flags, sp.ls + in, ret);
    {   // the result block the module leaves at r8 + 0x8d0 (the mixer reads it): r3's words, then the address of the last
        // sound unit decoded and how many bits of it were read
        const uint32_t res = (sp.r[8].w(0) + 0x8d0) & 0x3fff0;
        const AtracVoice& v = *g_atracVoices[sp.lsw(ctx + 0x34)];
        sp.lssw(res, ret); sp.lssw(res + 4, 2); sp.lssw(res + 8, 0x98); sp.lssw(res + 12, 0);
        sp.lssw(res + 16, in + v.resUnit); sp.lssw(res + 20, v.resBits);
    }
    for (int c = 0; c < n; ++c) {   // channel c at r4 + c * 0x1000
        uint32_t a = out + uint32_t(c) * 0x1000;
        if (a + 1028 * 2 > Spu::LS_SIZE) continue;
        for (int i = 0; i < 1028; ++i, a += 2) { sp.ls[a] = uint8_t(uint16_t(s[c][i]) >> 8); sp.ls[a + 1] = uint8_t(s[c][i]); }
    }
    if (const char* z = std::getenv("ATRACZERO")) {   // debug: clear (part of) the three buffers except our two outputs
        const int m = std::atoi(z);
        if (m & 1) std::memset(sp.ls + 0x12a00, 0, 0x8000);                       // r3 buffer (input + module scratch)
        if (m & 2) { std::memset(sp.ls + 0x1b208, 0, 0x1ba00 - 0x1b208); std::memset(sp.ls + 0x1c208, 0, 0x22a00 - 0x1c208); }   // rest of r4
        if (m & 4) std::memset(sp.ls + 0x22a00, 0, 0x8000);                       // r5 buffer
    }
    sp.r[3].sw(0, ret); sp.r[3].sw(1, 2); sp.r[3].sw(2, 0x98); sp.r[3].sw(3, 0);
    sp.pc = sp.r[0].w(0) & 0x3fffc;
    static uint64_t calls = 0, starts = 0, last = 0;
    ++calls; if ((sp.lsw(ctx + 0x20) >> 24) == 2) ++starts;
    static const uint64_t delayUs = std::getenv("ATRACDELAY") ? std::strtoull(std::getenv("ATRACDELAY"), nullptr, 10) : 0;   // debug: busy wait per call
    if (delayUs) for (const uint64_t t0 = now_us(); now_us() - t0 < delayUs;) {}
    if (std::getenv("ATRACSTAT") && now_us() - last > 4000000) {   // debug: ATRACSTAT=1
        last = now_us();
        for (auto& [k, vp] : g_atracVoices) if (vp->frames > 50) LOG("[atrac]   voice %08x %d ch: %.1f frames/s, %.1f calls/s%c", k, vp->nch, vp->frames * 1e6 / double(now_us() - vp->firstUs), vp->calls * 1e6 / double(now_us() - vp->firstUs), 10);
        LOG("[atrac] built-in: %llu calls, %llu frames, %llu starts, %zu voices, bad %llu%c", (unsigned long long)calls, (unsigned long long)g_atracFrames.load(), (unsigned long long)starts, g_atracVoices.size(), (unsigned long long)g_atracBad.load(), 10);
    }
    return true;
}
// ATRACCHECK=1 (debug): the system's module decodes as usual; after each decode call ours computes the same call and
// the outputs are compared ([atrac] lines every 4 s: calls, samples off by more than 2, worst difference)
std::atomic<uint64_t> g_chkCalls{0}, g_chkBadSamples{0}, g_chkWorst{0}, g_chkRet{0}, g_chkRes{0};
uint32_t g_chkCtx[32], g_chkRegs[3], g_chkIn[128][4];
std::vector<uint8_t> g_chkLs(Spu::LS_SIZE);   // LS at the entry of the call (ATRACUNDO)

void run_task(Task* t, uint32_t elf, uint64_t a0, uint64_t a1, uint64_t tsArgs, uint32_t spurs) {
    ripprof_register_thread("sndtask");
    if (!std::getenv("SNDNOPRIO")) host_thread_priority_high();
    Spu& s = t->spu;
    s.mem = G->mem; s.reset();
    std::memset(s.ls, 0, sizeof s.ls);
    const uint32_t phoff = be32(elf + 0x1c), phnum = be32(elf + 0x2c) >> 16, entry = be32(elf + 0x18);
    for (uint32_t k = 0; k < phnum; ++k) {
        const uint32_t ph = elf + phoff + 32 * k;
        if (be32(ph) != 1) continue;
        const uint32_t off = be32(ph + 4), va = be32(ph + 8), fsz = be32(ph + 16);
        if (va + fsz <= Spu::LS_SIZE) std::memcpy(s.ls + va, G->mem + elf + off, fsz);
    }
    s.lssw(0x1C4, spurs); s.lssw(0x1C8, 0); s.lssw(0x1CC, 0x1f);
    s.lssw(0x2FB8, 0x2700);   // SPURS task library: pointer to the taskset context (system call entry at +0xC4)
    s.lssw(0x2780, uint32_t(a0 >> 32)); s.lssw(0x2784, uint32_t(a0)); s.lssw(0x2788, uint32_t(a1 >> 32)); s.lssw(0x278C, uint32_t(a1));   // task info: arguments, ELF, context save area
    s.lssw(0x2794, elf); s.lssw(0x279C, t->ctxSave);
    s.lssw(0x27BC, t->taskset); s.lssw(0x27C4, 0xA70); s.lssw(0x27CC, 0); s.lssw(0x27D0, 0x1f); s.lssw(0x27D4, t->id);
    s.r[1].sw(0, 0x3FFB0);
    s.r[3].sd(0, a0); s.r[3].sd(1, a1);
    s.r[4].sd(0, tsArgs); s.r[4].sd(1, spurs);
    s.pc = entry;
    s.hookBelow = 0x3000;
    s.compiled = nullptr;
    const spurt::Compiled* cpMain = std::getenv("NOSPUREC") ? nullptr : spurt::find_compiled(elf + 0x100);
    const spurt::Compiled* cpAtrac = std::getenv("NOSPUREC") ? nullptr : spurt::find_compiled(0xa7ac00);   // flashATRAC.pic at LS 0x32980
    const spurt::Compiled* cpMod = std::getenv("NOSPUREC") ? nullptr : spurt::find_compiled(0xbce880);   // mstream_dsp_reverb.pic
    if (cpMain && !std::memcmp(s.ls + cpMain->lsBase, G->mem + cpMain->eaBin, cpMain->size)) { s.compiled = cpMain->fn; s.compiledLo = cpMain->lsBase; s.compiledHi = cpMain->lsBase + cpMain->size; }
    if (std::getenv("SNDTRACE")) s.mfcTrace = 4000;
    if (std::getenv("ATRACHLE")) {   // debug: ATRACHLE=1 uses our decoder even when the system's module is loaded (A/B tests)
        s.region3Lo = 0x32980; s.region3Hi = 0x32980 + 0x7d40;
        s.onRegion3 = [](Spu& sp, bool enter) { return enter && sp.pc == 0x36c58 && atrac_decode_call(sp); };
    }
    if (std::getenv("ATRACCHECK")) {
        s.region3Lo = 0x32980; s.region3Hi = 0x32980 + 0x7d40;
        s.onRegion3 = [](Spu& sp, bool enter) {
            static uint32_t out = 0, key = 0, phase = 0; static bool decoding = false; static uint8_t frame[0x98 * AtracVoice::kMax];
            if (enter) {
                decoding = sp.pc == 0x36c58;
                static const bool noPut = std::getenv("ATRACNOPUT") != nullptr;   // debug: the module's writes to main memory are dropped
                sp.dropPuts = noPut && decoding;
                const uint32_t in = sp.r[3].w(0) & 0x3ffff;
                out = sp.r[4].w(0) & 0x3ffff; key = sp.lsw((sp.r[7].w(0) & 0x3ffff) + 0x34); phase = sp.lsw((sp.r[9].w(0) & 0x3ffff) + 0x10);
                if (in + sizeof frame <= Spu::LS_SIZE) std::memcpy(frame, sp.ls + in, sizeof frame);
                for (int i = 0; i < 32; ++i) g_chkCtx[i] = sp.lsw((sp.r[7].w(0) & 0x3ffff) + 4 * i);
                g_chkRegs[0] = in; g_chkRegs[1] = out; g_chkRegs[2] = sp.r[5].w(0);
                {   // ATRACCTX=1: for the first decode calls of each voice, its context in main memory and the module's DMA
                    static std::map<uint32_t, int> seen;
                    if (decoding && std::getenv("ATRACCTX") && seen[key]++ < 2) {
                        LOG("[ctxmem] voice %08x main memory:", key);
                        for (int i = 0; i < 48; ++i) LOG(" %08x", be32(key + 4 * i));
                        LOG("%c", 10);
                        sp.mfcTrace = 8;
                    }
                }
                for (int r = 0; r < 128; ++r) for (int w = 0; w < 4; ++w) g_chkIn[r][w] = sp.r[r].w(w);
                std::memcpy(g_chkLs.data(), sp.ls, Spu::LS_SIZE);
                return false;
            }
            sp.dropPuts = false;
            if (!decoding) return false;
            decoding = false;
            int16_t s[AtracVoice::kMax][1028];
            int nch = 0;
            const uint32_t ourRet = atrac_compute(frame, key, g_chkCtx[8], s, nch);
            if (sp.r[3].w(0) != ourRet) ++g_chkRet;
            {
                const AtracVoice& v = *g_atracVoices[key];
                const uint32_t res = (g_chkIn[8][0] + 0x8d0) & 0x3fff0;
                if (sp.lsw(res + 16) != g_chkRegs[0] + v.resUnit || sp.lsw(res + 20) != v.resBits) {
                    ++g_chkRes;
                    static int shownRes = 0;
                    if (shownRes++ < 6) LOG("[atrac] result block differs: sony %05x %x, ours %05x %x (%d ch)%c", sp.lsw(res + 16), sp.lsw(res + 20), g_chkRegs[0] + v.resUnit, v.resBits, nch, 10);
                }
            }
            atrac_seq("sony", key, g_chkCtx[8], frame, sp.r[3].w(0));
            uint64_t bad = 0, worst = 0;
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < 1028; ++i) {
                    const uint32_t a = (out + uint32_t(c) * 0x1000 + uint32_t(i) * 2) & (Spu::LS_SIZE - 2);
                    const int16_t sony = int16_t(uint16_t(sp.ls[a]) << 8 | sp.ls[a + 1]);
                    const uint64_t d = uint64_t(std::abs(int(sony) - int(s[c][i])));
                    if (d > 2) ++bad;
                    if (d > worst) worst = d;
                }
            {   // where Sony's decoder restarts a voice (0x200 twice in a row): the context words of that call
                static std::map<uint32_t, uint32_t> lastRet; static int shownRestart = 0;
                const uint32_t r = sp.r[3].w(0);
                auto it = lastRet.find(key);
                const bool restart = r == 0x200 && (it == lastRet.end() || it->second == 0x200);
                static std::map<uint32_t, int> follow;   // calls still to show after a start flag (ctx word 8, top byte 2)
                if ((g_chkCtx[8] >> 24) == 2 && shownRestart < 6) { ++shownRestart; follow[key] = 7; }
                if (follow[key] > 0) {
                    --follow[key];
                    LOG("[atrac] seq voice %08x w8 %08x phase %u sony r3 %x %x %x %x, frame %02x%02x, ours bad %llu, regs r3 %05x r4 %05x r5 %05x%c", key, g_chkCtx[8], phase,
                        sp.r[3].w(0), sp.r[3].w(1), sp.r[3].w(2), sp.r[3].w(3), frame[0], frame[1], (unsigned long long)bad, g_chkRegs[0], g_chkRegs[1], g_chkRegs[2], 10);
                }
                (void)restart;
                lastRet[key] = r;
            }
            {   // registers the module changes besides r3 (the first calls): the mixer may read more than r3
                static int shownRegs = 0;
                if (shownRegs < 6) {
                    ++shownRegs;
                    LOG("[atrac] regs changed by the module (call %llu):", (unsigned long long)g_chkCalls.load());
                    for (int r = 1; r < 128; ++r)
                        for (int w = 0; w < 4; ++w)
                            if (sp.r[r].w(w) != g_chkIn[r][w] && r != 3) { LOG(" r%d.%d %08x->%08x", r, w, g_chkIn[r][w], sp.r[r].w(w)); break; }
                    LOG("%c", 10);
                }
            }
            {   // which 0x1000 blocks of the three buffers the module writes, for 2- and 6-channel voices
                static int shown2 = 0, shown6 = 0;
                const int chans = int((g_chkCtx[8] >> 16) & 0xff);
                int& shown = chans == 6 ? shown6 : shown2;
                static int dumped6 = 0;   // the local store before and after a few 6-channel calls -> atrac6_<n>_in/out.bin
                if (chans == 6 && (g_chkCtx[8] >> 24) == 1 && dumped6 < 6) {
                    char name[48];
                    std::snprintf(name, sizeof name, "atrac6_%d_in.bin", dumped6);
                    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(g_chkLs.data(), 1, Spu::LS_SIZE, f); std::fclose(f); }
                    std::snprintf(name, sizeof name, "atrac6_%d_out.bin", dumped6);
                    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(sp.ls, 1, Spu::LS_SIZE, f); std::fclose(f); }
                    ++dumped6;
                }
                static int shownR5 = 0;
                bool r5w = false;
                for (uint32_t b = 0x1b208; b < 0x2aa00 && !r5w; ++b) if ((b < 0x1ba00 || b >= 0x1c208) && sp.ls[b] != g_chkLs[b]) r5w = true;
                if (shown < 3 || (r5w && shownR5 < 10)) {
                    ++shown; if (r5w) ++shownR5;
                    LOG("[atrac] %d-ch voice %08x flags %08x ret %x writes outside the 2 outputs:", chans, key, g_chkCtx[8], sp.r[3].w(0));
                    for (uint32_t b = 0x1b208; b < 0x2aa00;) {
                        if ((b >= 0x1ba00 && b < 0x1c208) || sp.ls[b] == g_chkLs[b]) { ++b; continue; }
                        uint32_t e = b; while (e < 0x2aa00 && (sp.ls[e] != g_chkLs[e] || (e + 8 < 0x2aa00 && std::memcmp(sp.ls + e, g_chkLs.data() + e, 8)))) ++e;
                        LOG(" %05x-%05x", b, e); b = e;
                    }
                    const uint32_t a = 0x23a00;
                    LOG(" | 23a00: %04x %04x %04x %04x%c", uint32_t(sp.ls[a]) << 8 | sp.ls[a + 1], uint32_t(sp.ls[a + 2]) << 8 | sp.ls[a + 3], uint32_t(sp.ls[a + 4]) << 8 | sp.ls[a + 5], uint32_t(sp.ls[a + 6]) << 8 | sp.ls[a + 7], 10);
                }
            }
            {   // ATRACR8=1: words of the mixer's area (r8 = 0x3c200 .. 0x3d200) the module changes, for the first calls
                static int shownR8 = 0;
                static uint32_t follow = 0;
                if (std::getenv("ATRACR8") && shownR8 >= 24 && shownR8 < 90 && (follow == 0 || follow == key)) {   // one voice in a row: the result block
                    follow = key; ++shownR8;
                    LOG("[r8seq] flags %08x in %05x r8 %05x ret %x | 3cad0:", g_chkCtx[8], g_chkRegs[0], g_chkIn[8][0], sp.r[3].w(0));
                    for (uint32_t a = 0x3cad0; a < 0x3caf0; a += 4) LOG(" %08x", sp.lsw(a));
                    LOG(" | frame %02x%02x%02x%02x%c", frame[0], frame[1], frame[2], frame[3], 10);
                }
                if (std::getenv("ATRACR8") && shownR8 < 24) {
                    ++shownR8;
                    LOG("[r8] voice %08x ret %x:", key, sp.r[3].w(0));
                    for (uint32_t a = 0x3c200; a < 0x3d200; a += 4) {
                        const uint32_t before = uint32_t(g_chkLs[a]) << 24 | uint32_t(g_chkLs[a + 1]) << 16 | uint32_t(g_chkLs[a + 2]) << 8 | g_chkLs[a + 3];
                        if (before != sp.lsw(a)) LOG(" %05x:%08x->%08x", a, before, sp.lsw(a));
                    }
                    LOG("%c", 10);
                }
            }
            static int shownBig = 0;
            if (worst > 1000 && shownBig++ < 8) {
                int at = 0, cc = 0, so = 0, us = 0;
                for (int c = 0; c < nch; ++c)
                    for (int i = 0; i < 1028; ++i) {
                        const uint32_t a = (out + uint32_t(c) * 0x1000 + uint32_t(i) * 2) & (Spu::LS_SIZE - 2);
                        const int sv = int(int16_t(uint16_t(sp.ls[a]) << 8 | sp.ls[a + 1]));
                        if (std::abs(sv - int(s[c][i])) == int(worst)) { at = i; cc = c; so = sv; us = s[c][i]; }
                    }
                LOG("[atrac] big diff %llu voice %08x flags %08x sony ret %x ours %x, ch %d sample %d sony %d ours %d, bad %llu, frame %02x%02x%c", (unsigned long long)worst, key, g_chkCtx[8],
                    sp.r[3].w(0), ourRet, cc, at, so, us, (unsigned long long)bad, frame[0], frame[1], 10);
            }
            static int shownDetail = 0;
            if (bad > 100 && (g_chkCtx[8] >> 24) == 2 && shownDetail++ < 2) {   // a start call that differs: both outputs, sampled
                LOG("[atrac] start detail voice %08x:", key);
                for (int c = 0; c < nch; ++c)
                    for (int i = 0; i < 1028; i += 64) {
                        const uint32_t a = (out + uint32_t(c) * 0x1000 + uint32_t(i) * 2) & (Spu::LS_SIZE - 2);
                        LOG(" c%d[%d] %d/%d", c, i, int(int16_t(uint16_t(sp.ls[a]) << 8 | sp.ls[a + 1])), int(s[c][i]));
                    }
                LOG("%c", 10);
            }
            static int shownBad = 0;
            if ((bad > 100 || sp.r[3].w(0) != ourRet) && shownBad++ < 16)
                LOG("[atrac] mismatch: voice %08x phase %u sony r3 %08x %08x %08x %08x, bad %llu, frame %02x%02x%02x%02x, regs r4 %05x r5 %05x r6 %x r8 %05x%c", key, phase,
                    sp.r[3].w(0), sp.r[3].w(1), sp.r[3].w(2), sp.r[3].w(3), (unsigned long long)bad, frame[0], frame[1], frame[2], frame[3], sp.r[4].w(0), sp.r[5].w(0), sp.r[6].w(0), sp.r[8].w(0), 10);
            {   // ATRACUNDO=lo-hi (hex LS range): put back what that range held before Sony's call (which of its writes matter)
                static uint32_t ulo = 0, uhi = 0;
                static bool parsed = false;
                if (!parsed) { parsed = true; if (const char* u = std::getenv("ATRACUNDO")) std::sscanf(u, "%x-%x", &ulo, &uhi); }
                if (uhi > ulo && uhi <= Spu::LS_SIZE) std::memcpy(sp.ls + ulo, g_chkLs.data() + ulo, uhi - ulo);
            }
            if (std::getenv("ATRACREGS"))   // debug: registers back to their entry values except r3 (does the mixer read others?)
                for (int r = 2; r < 128; ++r) if (r != 3) for (int w = 0; w < 4; ++w) sp.r[r].sw(w, g_chkIn[r][w]);
            static const bool overwrite = std::getenv("ATRACCHECK")[0] == '2';   // ATRACCHECK=2: then replace Sony's output by ours
            if (overwrite)
                for (int c = 0; c < nch; ++c)
                    for (int i = 0; i < 1028; ++i) {
                        const uint32_t a = (out + uint32_t(c) * 0x1000 + uint32_t(i) * 2) & (Spu::LS_SIZE - 2);
                        sp.ls[a] = uint8_t(uint16_t(s[c][i]) >> 8); sp.ls[a + 1] = uint8_t(s[c][i]);
                    }
            ++g_chkCalls; g_chkBadSamples += bad;
            if (worst > g_chkWorst) g_chkWorst = worst;
            static uint64_t last = 0;
            if (now_us() - last > 4000000) {
                last = now_us();
                LOG("[atrac] check: %llu calls, %llu samples off by more than 2, worst %llu, wrong return %llu, wrong result block %llu, voices %zu, bad frames %llu%c", (unsigned long long)g_chkCalls.load(),
                    (unsigned long long)g_chkBadSamples.load(), (unsigned long long)g_chkWorst.load(), (unsigned long long)g_chkRet.load(), (unsigned long long)g_chkRes.load(), g_atracVoices.size(), (unsigned long long)g_atracBad.load(), 10);
            }
            return false;
        };
    }
    if (const char* cl = std::getenv("CODECLOG")) {   // debug: CODECLOG=n: the first n calls into the music decoder (LS 0x32980..), arguments and result
        static int left = std::atoi(cl);
        s.region3Lo = 0x32980; s.region3Hi = 0x32980 + 0x7d40;
        s.onRegion3 = [](Spu& sp, bool enter) {
            if (left <= 0) return false;
            if (enter) LOG("[codec] enter %05x lr %05x sp %05x r3 %08x %08x %08x %08x r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x r9 %08x%c", sp.pc, sp.r[0].w(0), sp.r[1].w(0),
                           sp.r[3].w(0), sp.r[3].w(1), sp.r[3].w(2), sp.r[3].w(3), sp.r[4].w(0), sp.r[5].w(0), sp.r[6].w(0), sp.r[7].w(0), sp.r[8].w(0), sp.r[9].w(0), 10);
            else { LOG("[codec] leave to %05x r3 %08x %08x %08x %08x%c", sp.pc, sp.r[3].w(0), sp.r[3].w(1), sp.r[3].w(2), sp.r[3].w(3), 10); --left; }
            // CODECDUMP=first,n: whole local store at entry and exit of calls first..first+n-1 -> codec_<k>_in/out.bin
            static int k = 0, first = -1, n = 0;
            if (first < 0) { first = 1 << 30; if (const char* d = std::getenv("CODECDUMP")) std::sscanf(d, "%d,%d", &first, &n); }
            if (k >= first && k < first + n) {
                char name[64]; std::snprintf(name, sizeof name, "codec_%03d_%s.bin", k, enter ? "in" : "out");
                if (FILE* f = std::fopen(name, "wb")) { std::fwrite(sp.ls, 1, Spu::LS_SIZE, f); std::fclose(f); }
            }
            // CODECMFC=first,n: the DMA commands the decoder issues during those calls, with its stream context words
            static int mf = -1, mn = 0;
            if (mf < 0) { mf = 1 << 30; if (const char* d = std::getenv("CODECMFC")) std::sscanf(d, "%d,%d", &mf, &mn); }
            if (k >= mf && k < mf + mn) {
                if (enter) {
                    LOG("[codec] call %d ctx r7:", k); for (int i = 0; i < 24; ++i) LOG(" %08x", sp.lsw(0x2eb80 + 4 * i)); LOG("%c", 10);
                    LOG("[codec] call %d r9:", k); for (int i = 0; i < 24; ++i) LOG(" %08x", sp.lsw(0x2ea80 + 4 * i)); LOG("%c", 10);
                    sp.mfcTrace = 200;
                } else sp.mfcTrace = 0;
            }
            if (!enter) ++k;
            return false;
        };
    }
    int shown = 0;
    static const bool trace = std::getenv("SNDTRACE") != nullptr;
    s.onBranch = [&](Spu& sp, uint32_t tg) {
        const uint32_t nr = sp.r[3].w(0), lr = sp.r[0].w(0);
        { static std::set<uint64_t> seen; if (seen.size() < 24 && seen.insert(uint64_t(tg) << 40 | uint64_t(lr) << 8 | (nr & 0xff)).second) LOG("[snd] task %u: first call of LS 0x%x from %05x with r3 %08x%c", t->id, tg, lr, nr, 10); }
        if (trace && shown++ < 200) LOG("[snd] task %u: call LS 0x%x r3 %08x r4 %08x r5 %08x lr %05x\n", t->id, tg, nr, sp.r[4].w(0), sp.r[5].w(0), lr);
        if (tg == 0) {   // codec module that was never loaded (no flashATRAC.pic): our own ATRAC3 decoder answers the calls
            static bool told = false;
            if (!told) { told = true; LOG("[snd] no flashATRAC.pic: music and ambience streams use the built-in ATRAC3 decoder%c", 10); }
            ++g_passes;
            { const uint64_t d = now_us() - g_wake; g_busySum += d; ++g_waits; if (d > g_busyMax) g_busyMax = d; if (d > 8000) ++g_slow; }   // debug: time since the last signal
            if (sp.r[6].w(0) == 0x400) {   // decode
                atrac_decode_call(sp);
                if (cpMod && !sp.compiled2 && !std::memcmp(sp.ls + cpMod->lsBase, G->mem + cpMod->eaBin, cpMod->size)) { sp.compiled2 = cpMod->fn; sp.compiled2Lo = cpMod->lsBase; sp.compiled2Hi = cpMod->lsBase + cpMod->size; }
                return true;
            }
            if (cpMod && !sp.compiled2 && !std::memcmp(sp.ls + cpMod->lsBase, G->mem + cpMod->eaBin, cpMod->size)) { sp.compiled2 = cpMod->fn; sp.compiled2Lo = cpMod->lsBase; sp.compiled2Hi = cpMod->lsBase + cpMod->size; }
            sp.r[3].sw(0, 0); sp.pc = lr & 0x3fffc;
            return true;
        }
        if (tg != 0xA70) { LOG("[snd] task %u branched to LS 0x%x (r3 %08x lr %05x): not handled; LS 2ee50 = %08x %08x %08x %08x\n", t->id, tg, nr, lr, sp.lsw(0x2ee50), sp.lsw(0x2ee54), sp.lsw(0x2ee58), sp.lsw(0x2ee5c)); return false; }
        switch (nr) {
            case 0: LOG("[snd] task %u exited\n", t->id); return false;     // exit
            case 1: ++g_sndSys[1]; std::this_thread::yield(); sp.r[3].sw(0, 0); break;         // yield
            case 2: {                                                           // wait signal
                const uint64_t t0 = now_us();
                if (g_wake) { const uint64_t busy = t0 - g_wake; g_busySum += busy; if (busy > g_busyMax) g_busyMax = busy; ++g_waits; }
                std::unique_lock<std::mutex> lk(t->m);
                t->cv.wait(lk, [&] { return t->signalled; });
                t->signalled = false;
                g_wake = now_us();
                // the module regions are enabled once the program has loaded them. The ATRAC decoder comes from the
                // console's system files (flashATRAC.pic, relocated by the loader): recognised by its checksum.
                if (cpAtrac && !sp.compiled3 && sp.lsw(cpAtrac->lsBase + 0x10)) {
                    uint32_t h = 0x811c9dc5;
                    for (uint32_t k = 0; k < cpAtrac->size; ++k) h = (h ^ sp.ls[cpAtrac->lsBase + k]) * 0x01000193;
                    if (h == 0x29043cac) { sp.compiled3 = cpAtrac->fn; sp.compiled3Lo = cpAtrac->lsBase; sp.compiled3Hi = cpAtrac->lsBase + cpAtrac->size; LOG("[snd] ATRAC decoder runs recompiled%c", 10); }
                    else { static bool told = false; if (!told) { told = true; LOG("[snd] ATRAC decoder of another system version (checksum %08x): interpreted%c", h, 10); } }
                }
                if (cpMod && !sp.compiled2 && !std::memcmp(sp.ls + cpMod->lsBase, G->mem + cpMod->eaBin, cpMod->size)) { sp.compiled2 = cpMod->fn; sp.compiled2Lo = cpMod->lsBase; sp.compiled2Hi = cpMod->lsBase + cpMod->size; LOG("[snd] reverb module runs recompiled\n"); }
                sp.r[3].sw(0, 0);
                break;
            }
            case 3: ++g_sndSys[3]; sp.r[3].sw(0, 0); break;                                    // poll: nothing else wants this SPU
            default: LOG("[snd] task %u: SPURS task syscall %u not handled\n", t->id, nr); return false;
        }
        sp.pc = lr & 0x3fffc;
        return true;
    };
    s.onStop = [&](Spu& sp, uint32_t code) { LOG("[snd] task %u: stop 0x%x at LS 0x%x\n", t->id, code, sp.pc); return false; };
    s.onWriteCh = [&](Spu&, uint32_t ch, uint32_t v) { if (shown++ < 200) LOG("[snd] task %u: wrch %u <- %08x\n", t->id, ch, v); return true; };
    s.onReadCh = [&](Spu&, uint32_t ch, uint32_t& v) { v = 0; if (shown++ < 200) LOG("[snd] task %u: rdch %u\n", t->id, ch); return true; };
    LOG("[snd] task %u of taskset %x: SPU program %x, entry 0x%x\n", t->id, t->taskset, elf, entry);
    if (const char* w = std::getenv("SNDWATCH")) {   // debug: SNDWATCH=<LS address in hex>: which instruction changes that word
        const uint32_t a = uint32_t(std::strtoul(w, nullptr, 16));
        s.running = true;
        for (int n = 0; n < 30;) {
            const uint32_t prev = s.lsw(a), pc0 = s.pc;
            if (!s.step()) break;
            if (s.lsw(a) != prev) { ++n; LOG("[snd] LS %05x: %08x -> %08x at pc %05x (mfc ls %05x ea %08x size %x)\n", a, prev, s.lsw(a), pc0, s.mfcLsa, s.mfcEal, s.mfcSize); }
        }
    }
    s.run();
    LOG("[snd] task %u stopped at LS 0x%x after %llu instructions (bad instruction %08x)\n", t->id, s.pc, (unsigned long long)s.steps, s.badInstr);
}

uint32_t create_task(uint32_t taskset, uint32_t elf, uint32_t ctxSave, uint64_t a0, uint64_t a1) {
    Task* t = new Task;
    if (std::getenv("SNDTRACE")) std::thread([=] {   // debug: what the game puts in the task's command word
        uint32_t last = 0; int n = 0;
        while (n < 40) { const uint32_t v = be32(uint32_t(a0)); if (v != last) { last = v; ++n; LOG("[snd] command word = %08x (%llu signals so far)\n", v, (unsigned long long)g_signals.load()); } std::this_thread::yield(); }
    }).detach();
    std::lock_guard<std::mutex> lk(g_tm);
    t->taskset = taskset; t->id = uint32_t(g_tasks.size()); t->ctxSave = ctxSave;
    g_tasks.push_back(t);
    std::thread([=] { run_task(t, elf, a0, a1, 0, 0); }).detach();
    return t->id;
}
void signal(uint32_t taskset, uint32_t id) {
    ++g_signals;
    g_wake = now_us();
    std::lock_guard<std::mutex> lk(g_tm);
    if (id >= g_tasks.size() || g_tasks[id]->taskset != taskset) return;
    Task* t = g_tasks[id];
    { std::lock_guard<std::mutex> l2(t->m); t->signalled = true; }
    t->cv.notify_all();
}
// cellAudio port 0: `blocks` blocks of 256 samples x `ch` big-endian floats, played from the read index
uint32_t g_port = 0, g_readIdx = 0, g_ch = 8, g_blocks = 8;
std::atomic<bool> g_started{false};
constexpr uint32_t kUnwritten = 0x7fc0dead;
std::atomic<uint64_t> g_holes{0}, g_waited{0};
void port_thread() {
    // The game waits for its mixer even with the sound off: the port then runs on its clock and nothing is played.
    // settings().sound is followed while the game runs (options menu): the device is opened the first time it is on
    static const bool noSound = std::getenv("NOSOUND") != nullptr;
    bool play = settings().sound && !noSound && audio_out_open(), openFailed = settings().sound && !noSound && !play;
    host_thread_priority_high();
    LOG("[snd] audio port %s: %u channels, %u blocks\n", play ? "playing" : "running silent (sound off)", g_ch, g_blocks);
    std::vector<float> out(512);
    uint64_t blocksOut = 0; float peak = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        // One block every 256/48000 s on a steady clock, like the console: the game's mixer thread polls the read
        // index and must see it advance evenly (bursts make it miss blocks). The SDL queue absorbs the jitter; the
        // clock is nudged to keep about 6 blocks queued, which also follows the sound card's real rate.
        next += std::chrono::nanoseconds(5333333);
        if (const bool want = settings().sound && !noSound; want != play && !(want && openFailed)) {
            if (want) { play = audio_out_open(); openFailed = !play; }
            else { play = false; audio_out_clear(); }
            LOG("[snd] sound %s\n", play ? "on" : "off");
        }
        {
            const int q = play ? audio_out_queued() : 12 * 256;
            if (q > 16 * 256) next += std::chrono::microseconds(300); else if (q < 10 * 256) next -= std::chrono::microseconds(300);   // about 70 ms queued: room for a late mixer pass when another program (OBS) takes CPU time
            auto now = std::chrono::steady_clock::now();
            if (next < now - std::chrono::milliseconds(50)) next = now;   // stalled (debugger, suspend): do not catch up in a burst
            if (next - now > std::chrono::microseconds(1500)) std::this_thread::sleep_for(next - now - std::chrono::microseconds(1500));
            while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
        }
        const uint32_t idx = be32(g_readIdx + 4) % g_blocks;
        const uint8_t* p = G->mem + g_port + idx * 256 * g_ch * 4;
        // The mixer thread runs on its own clock and sometimes has not written this block yet. Played blocks are
        // marked (a NaN pattern in their first sample), so an unwritten one is recognisable: wait a little for it
        // instead of playing a hole; the SDL queue covers the wait.
        if (be32(g_port + idx * 256 * g_ch * 4) == kUnwritten) {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (be32(g_port + idx * 256 * g_ch * 4) == kUnwritten && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
            if (be32(g_port + idx * 256 * g_ch * 4) == kUnwritten) ++g_holes; else ++g_waited;
            next = std::chrono::steady_clock::now();
        }
        static uint64_t relHist[8];
        if (std::getenv("SNDTRACE")) {   // debug: which blocks (relative to the read index) hold data when one is played
            for (uint32_t r = 0; r < g_blocks && r < 8; ++r) {
                const uint32_t* q = reinterpret_cast<const uint32_t*>(G->mem + g_port + ((idx + r) % g_blocks) * 256 * g_ch * 4);
                bool any = false;
                for (uint32_t k = 0; k < 256 * g_ch && !any; k += 8) any = q[k] != 0;
                relHist[r] += any;
            }
            if (blocksOut % 1875 == 1874) { LOG("[snd] blocks holding data at read time, by distance from the read index: %llu %llu %llu %llu %llu %llu %llu %llu%c", (unsigned long long)relHist[0], (unsigned long long)relHist[1], (unsigned long long)relHist[2], (unsigned long long)relHist[3], (unsigned long long)relHist[4], (unsigned long long)relHist[5], (unsigned long long)relHist[6], (unsigned long long)relHist[7], 10); std::memset(relHist, 0, sizeof relHist); }
        }
        auto smp = [&](uint32_t i, uint32_t c) { uint32_t u; std::memcpy(&u, p + (i * g_ch + c) * 4, 4); u = __builtin_bswap32(u); if ((u & 0x7f800000) == 0x7f800000) u = 0; float f; std::memcpy(&f, &u, 4); return f; };
        for (uint32_t i = 0; i < 256; ++i) {
            float l = smp(i, 0), r = smp(i, 1);
            if (g_ch >= 6) { const float c = smp(i, 2) * 0.707f + smp(i, 3) * 0.5f; l += c + smp(i, 4) * 0.707f; r += c + smp(i, 5) * 0.707f; }
            if (g_ch >= 8) { l += smp(i, 6) * 0.707f; r += smp(i, 7) * 0.707f; }
            out[2 * i] = std::clamp(l, -1.f, 1.f); out[2 * i + 1] = std::clamp(r, -1.f, 1.f);
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        if (play) audio_out_push(out.data(), 256);
        { static FILE* wf = std::getenv("SNDWAV") ? std::fopen("snd_out.f32", "wb") : nullptr; if (wf) std::fwrite(out.data(), 4, 512, wf); }   // debug: raw stereo float dump
        std::memset(G->mem + g_port + idx * 256 * g_ch * 4, 0, 256 * g_ch * 4);
        gcm::wr(g_port + idx * 256 * g_ch * 4, kUnwritten);
        gcm::wr(g_readIdx + 4, (idx + 1) % g_blocks);
        if (++blocksOut % 1875 == 0) {
            LOG("[snd] 10 s of audio played, peak level %.3f, %llu signals\n", peak, (unsigned long long)g_signals.load()); peak = 0;
            LOG("[snd]   task syscalls in 10 s: yield %llu, poll %llu\n", (unsigned long long)g_sndSys[1].exchange(0), (unsigned long long)g_sndSys[3].exchange(0));
            LOG("[snd]   %llu codec calls, main %s, module %s%c", (unsigned long long)g_passes.load(), !g_tasks.empty() && g_tasks[0]->spu.compiled ? "recompiled" : "interpreted", !g_tasks.empty() && g_tasks[0]->spu.compiled2 ? "recompiled" : "interpreted", 10);
            if (std::getenv("SNDDUMP") && blocksOut == 1875 && !g_tasks.empty()) if (FILE* f = std::fopen("snd_ls.bin", "wb")) { std::fwrite(g_tasks[0]->spu.ls, 1, Spu::LS_SIZE, f); std::fclose(f); }
            { const uint64_t w = g_waits.exchange(0), bs = g_busySum.exchange(0), bm = g_busyMax.exchange(0); LOG("[snd]   codec calls: %.2f ms after the signal on average, %.2f ms at most, %llu over 8 ms%c", w ? bs / 1000.0 / w : 0.0, bm / 1000.0, (unsigned long long)g_slow.exchange(0), 10); LOG("[snd]   blocks waited for: %llu, played as holes: %llu, %d frames queued%c", (unsigned long long)g_waited.exchange(0), (unsigned long long)g_holes.exchange(0), audio_out_queued(), 10); }
            if (std::getenv("SNDPROF") && !g_tasks.empty()) {   // debug: where the interpreter still runs
                Spu& sp = g_tasks[0]->spu;
                static uint32_t hist[1024];
                if (sp.interpHist) {
                    std::string o; char b[40];
                    for (int k = 0; k < 6; ++k) { int best = 0; for (int i = 1; i < 1024; ++i) if (hist[i] > hist[best]) best = i; if (!hist[best]) break; std::snprintf(b, sizeof b, " %05x:%.1fM", best << 8, hist[best] / 1e6); o += b; hist[best] = 0; }
                    LOG("[snd]   interpreted in 10 s:%s; %llu interpreted, %llu compiled entries\n", o.c_str(), (unsigned long long)sp.interpSteps, (unsigned long long)sp.compiledCalls);
                }
                std::memset(hist, 0, sizeof hist); sp.interpHist = hist;
            }
            for (Task* t : g_tasks) LOG("[snd]   task %u at LS 0x%x, %llu instructions, lr %05x sp %05x\n", t->id, t->spu.pc, (unsigned long long)t->spu.steps, t->spu.r[0].w(0), t->spu.r[1].w(0));
        }
    }
}
} // namespace snd

// WAITPROF=1: time every guest thread spends blocked, by wait site, in ms per second (printed every 4 s)
const bool g_waitProf = std::getenv("WAITPROF") != nullptr;
std::map<std::pair<uint32_t, std::string>, uint64_t> g_waitAcc;
void waitprof_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(4));
        std::string o;
        {
            std::lock_guard<std::mutex> lk(g_waitMx);
            char b[160];
            for (auto& [k, us] : g_waitAcc) if (us > 40000) { std::snprintf(b, sizeof b, " | %x %s %.0f", k.first & 0xfff, k.second.c_str(), us / 4000.0); o += b; }
            g_waitAcc.clear();
        }
        LOG("[waitprof] blocked ms/s%s\n", o.c_str());
    }
}
struct WaitScope {
    WaitSlot* s;
    WaitScope(const char* w, uint32_t lr) {
        if (!t_waitSlot) {
            t_waitSlot = new WaitSlot;
            t_waitSlot->tid = t_tid;
            std::lock_guard<std::mutex> lk(g_waitMx);
            g_waitSlots.push_back(t_waitSlot);
            static bool profStarted = false;
            if (g_waitProf && !profStarted) { profStarted = true; std::thread(waitprof_thread).detach(); }
        }
        s = t_waitSlot;
        std::strncpy(s->what, w, sizeof s->what - 1);
        s->lr = lr; s->tid = t_tid;
        s->since = now_us();
    }
    ~WaitScope() {
        if (g_waitProf) {
            char b[64]; std::snprintf(b, sizeof b, "%.14s@%x", s->what, s->lr);
            std::lock_guard<std::mutex> lk(g_waitMx);
            g_waitAcc[{s->tid, b}] += now_us() - s->since.load();
        }
        s->since = 0;
    }
};

const char* g_hotName[4] = {};
const bool g_jobDump = std::getenv("JOBDUMP") != nullptr;
bool imports(PPU& c, const char* module, const char* name) {
    {   // The imports the game calls thousands of times per frame, recognised by the address of their name
        // (set the first time they come through the normal path below).
        if (name == g_hotName[0]) { c.wr64(a32(c, 0), t_tid); ret(c, CELL_OK); return true; }   // sys_ppu_thread_get_id
        if (name == g_hotName[1]) {                                                             // sys_lwmutex_lock
            auto m = lwm(a32(c, 0));
            const bool ok = mlock(*m, c.r[4]);
            if (ok) { c.wr32(a32(c, 0), t_tid); c.wr32(a32(c, 0) + 0xc, uint32_t(m->count)); }
            ret(c, ok ? CELL_OK : LV2_ETIMEDOUT); return true;
        }
        if (name == g_hotName[2]) {                                                             // sys_lwmutex_unlock
            auto m = lwm(a32(c, 0));
            munlock(*m);
            c.wr32(a32(c, 0) + 0xc, uint32_t(m->count));
            if (!m->count) c.wr32(a32(c, 0), 0xFFFFFFFFu);
            ret(c, CELL_OK); return true;
        }
        if (name == g_hotName[3] && g_jobsOn && !g_jobDump) {                                   // cellSpursRunJobChain
            jobs::start(g_jobChainEntry); jobs::kick();
            ret(c, CELL_OK); return true;
        }
        if (!g_hotName[0] && !std::strcmp(name, "sys_ppu_thread_get_id")) g_hotName[0] = name;
        else if (!g_hotName[1] && !std::strcmp(name, "sys_lwmutex_lock")) g_hotName[1] = name;
        else if (!g_hotName[2] && !std::strcmp(name, "sys_lwmutex_unlock")) g_hotName[2] = name;
        else if (!g_hotName[3] && !std::strcmp(name, "cellSpursRunJobChain")) g_hotName[3] = name;
    }
    const std::string n = name, mod = module;
    WaitScope ws_(name, uint32_t(c.lr));
    if (CFG.trace) LOG("[call %x] %s::%s r3=%llx r4=%llx r5=%llx\n", t_tid, module, name, (unsigned long long)c.r[3], (unsigned long long)c.r[4], (unsigned long long)c.r[5]);
    if (mod == "cellGcmSys" && gcm::import(c, n)) return true;
    // --- sysutil: video out 1280x720, English, X to confirm, no save data yet
    if (n == "cellVideoOutGetState") {   // (videoOut, deviceIndex, CellVideoOutState* {u8 state, colorSpace, reserved[6], displayMode{u8 resId, scanMode, conversion, aspect, reserved[2], u16 refreshRates}})
        const uint32_t p = a32(c, 2);
        std::memset(c.mem + p, 0, 16);
        c.wr8(p, 0); c.wr8(p + 1, 1);
        c.wr8(p + 8, 2); c.wr8(p + 9, 1); c.wr8(p + 10, 0); c.wr8(p + 11, 2); c.wr16(p + 14, 1);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellVideoOutGetResolution") {   // (resolutionId, {u16 width, u16 height})
        static const uint16_t R[][2] = {{0, 0}, {1920, 1080}, {1280, 720}, {720, 480}, {720, 576}};
        const uint32_t id = a32(c, 0) < 5 ? a32(c, 0) : 2;
        c.wr16(a32(c, 1), R[id][0]); c.wr16(a32(c, 1) + 2, R[id][1]);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellVideoOutConfigure" || n == "cellAudioOutConfigure") { ret(c, CELL_OK); return true; }
    if (n == "cellAudioOutGetSoundAvailability") { ret(c, a32(c, 1) == 0 ? 2 : 0); return true; }   // (audioOut, coding type, fs, option): stereo LPCM only, no Dolby/DTS
    if (n == "cellSysutilGetSystemParamInt") {
        const uint32_t id = a32(c, 0);
        int32_t v = 0;
        if (id == 0x0111) v = settings().language;   // system language (options menu)
        else if (id == 0x0112) v = 1;   // enter button: cross
        else if (id == 0x0114) v = 0;   // date format
        else if (id == 0x0116) v = 0;   // summer time
        c.wr32(a32(c, 1), uint32_t(v));
        ret(c, CELL_OK); return true;
    }
    // --- sysutil callbacks (system events delivered from cellSysutilCheckCallback on the polling thread)
    if (n == "cellSysutilRegisterCallback") {   // (slot, func, userdata)
        std::lock_guard<std::mutex> lk(g_sysutilMx);
        if (a32(c, 0) < 4) g_sysutilCb[a32(c, 0)] = {a32(c, 1), a32(c, 2)};
        ret(c, CELL_OK); return true;
    }
    if (n == "cellSysutilUnregisterCallback") { std::lock_guard<std::mutex> lk(g_sysutilMx); if (a32(c, 0) < 4) g_sysutilCb[a32(c, 0)] = {0, 0}; ret(c, CELL_OK); return true; }
    if (n == "cellSysutilCheckCallback") {
        for (;;) {
            std::pair<uint32_t, uint32_t> ev;
            std::pair<uint32_t, uint32_t> cbs[4];
            {
                std::lock_guard<std::mutex> lk(g_sysutilMx);
                if (g_sysutilEv.empty()) break;
                ev = g_sysutilEv.front(); g_sysutilEv.pop_front();
                for (int k = 0; k < 4; ++k) cbs[k] = g_sysutilCb[k];
            }
            LOG("[sysutil] event 0x%x param 0x%x\n", ev.first, ev.second);
            for (int k = 0; k < 4; ++k)
                if (cbs[k].first) call_nested(c, cbs[k].first, uint32_t(c.r[1]) - 0x800, ev.first, ev.second, cbs[k].second);
        }
        if (netplay::enabled())   // online play: the game's NP callbacks (requests, room events, signaling) run here, as on the PS3
            for (const auto& cb : netplay::take_callbacks())
                call_nested(c, cb.opd, uint32_t(c.r[1]) - 0x800, cb.a[0], cb.a[1], cb.a[2], cb.a[3], cb.a[4], cb.a[5], cb.a[6], cb.a[7]);
        ret(c, CELL_OK); return true;
    }
    if ((n == "_sys_net_errno_loc" || n == "sys_net_errno_loc") && netplay::enabled()) { static const uint32_t e = alloc_user(16, 16); netplay::set_errno_addr(e); ret(c, e); return true; }
    if (n != "cellNetCtlNetStartDialogLoadAsync" && n != "cellNetCtlNetStartDialogUnloadAsync" && netplay::hle(c, module, n)) return true;   // NETPLAY: online play
    // --- network: no connection (as a console without network): the start dialog finishes with "not connected"
    if (n == "cellNetCtlNetStartDialogLoadAsync") {
        std::lock_guard<std::mutex> lk(g_sysutilMx);
        g_sysutilEv.push_back({0x0801, 0});   // CELL_SYSUTIL_NET_CTL_NETSTART_LOADED
        g_sysutilEv.push_back({0x0802, 0});   // CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED
        ret(c, CELL_OK); return true;
    }
    if (n == "cellNetCtlNetStartDialogUnloadAsync") {   // (CellNetCtlNetStartDialogResult* {u32 size; s32 result})
        if (a32(c, 0)) c.wr32(a32(c, 0) + 4, netplay::enabled() ? 0u : 0x80130106u);   // connected (online play) or CELL_NET_CTL_ERROR_NET_NOT_CONNECTED
        std::lock_guard<std::mutex> lk(g_sysutilMx);
        g_sysutilEv.push_back({0x0803, 0});   // CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED
        ret(c, CELL_OK); return true;
    }
    if (n == "socket") { ret(c, uint64_t(-1)); return true; }   // no network stack: every socket fails
    if (n == "recvfrom" || n == "recv") { std::this_thread::sleep_for(std::chrono::milliseconds(20)); ret(c, uint64_t(-1)); return true; }
    if (n == "sceNpManagerGetStatus") { if (a32(c, 0)) c.wr32(a32(c, 0), 0xFFFFFFFFu); ret(c, CELL_OK); return true; }   // SCE_NP_MANAGER_STATUS_OFFLINE
    if (n == "cellNetCtlGetState") { if (a32(c, 0)) c.wr32(a32(c, 0), 0); ret(c, CELL_OK); return true; }   // Disconnected
    if (n == "cellNetCtlGetInfo") { ret(c, 0x80130106u); return true; }
    if (n == "cellSaveDataAutoLoad2") { ret(c, savedata::op(c, false)); return true; }
    if (n == "cellSaveDataAutoSave2") { ret(c, savedata::op(c, true)); return true; }
    if (n == "cellSaveDataListLoad2") { ret(c, savedata::ERR_NODATA); return true; }
    // --- trophies (sceNpTrophy): the game's own list (TROPDIR/*/TROPHY.TRP next to USRDIR, in the game's language),
    //     unlocked ones kept in dev_hdd0/trofeos.txt; an unlock shows a notification on screen
    if (n.rfind("sceNpTrophy", 0) == 0) {
        struct Trophy { int id; char type; std::wstring name, detail; std::string icon; };
        static std::vector<Trophy> list;
        static std::set<int> unlocked;
        static bool loaded = false;
        static const std::string savePath = CFG.hdd0 + "/trofeos.txt";
        if (!loaded) {
            loaded = true;
            std::error_code ec;
            for (const auto& d : std::filesystem::directory_iterator(std::filesystem::path(CFG.usrdir).parent_path() / "TROPDIR", ec)) {
                FILE* f = _wfopen((d.path() / "TROPHY.TRP").wstring().c_str(), L"rb");
                if (!f) continue;
                std::vector<uint8_t> trp;
                for (int ch; (ch = std::fgetc(f)) != EOF;) trp.push_back(uint8_t(ch));
                std::fclose(f);
                auto be = [&](size_t o, int bytes) { uint64_t v = 0; for (int i = 0; i < bytes && o + i < trp.size(); ++i) v = v << 8 | trp[o + i]; return v; };
                auto entry = [&](const std::string& want) -> std::string {
                    const uint32_t cnt = uint32_t(be(0x10, 4)), esz = uint32_t(be(0x14, 4));
                    for (uint32_t i = 0; i < cnt && 0x40 + (i + 1) * esz <= trp.size(); ++i) {
                        const size_t o = 0x40 + size_t(i) * esz;
                        if (std::string(reinterpret_cast<const char*>(&trp[o])) != want) continue;
                        const uint64_t off = be(o + 32, 8), sz = be(o + 40, 8);
                        if (off + sz <= trp.size()) return std::string(reinterpret_cast<const char*>(&trp[off]), size_t(sz));
                    }
                    return {};
                };
                static const char* const langFile[6] = {"TROP.SFM", "TROP_01.SFM", "TROP_02.SFM", "TROP_03.SFM", "TROP_04.SFM", "TROP_05.SFM"};
                std::string xml = entry(langFile[std::clamp(settings().language, 0, 5)]);
                if (xml.empty()) xml = entry("TROP.SFM");
                auto wide = [](const std::string& s) {   // UTF-8, or Latin-1 when it is not valid UTF-8
                    std::wstring w; bool ok = true;
                    for (size_t i = 0; i < s.size() && ok;) {
                        const unsigned char c = uint8_t(s[i]);
                        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 0;
                        if (!len || i + len > s.size()) { ok = false; break; }
                        uint32_t cp = len == 1 ? c : len == 2 ? c & 31 : c & 15;
                        for (int k = 1; k < len; ++k) { if ((uint8_t(s[i + k]) >> 6) != 2) { ok = false; break; } cp = cp << 6 | (uint8_t(s[i + k]) & 63); }
                        w += wchar_t(cp); i += size_t(len);
                    }
                    if (!ok) { w.clear(); for (unsigned char c : s) w += wchar_t(c); }
                    return w;
                };
                auto trim = [](std::wstring s) { while (!s.empty() && (s.back() == L' ' || s.back() == L'\n' || s.back() == L'\r')) s.pop_back(); return s; };
                for (size_t p = 0; (p = xml.find("<trophy ", p)) != std::string::npos; ++p) {
                    const size_t end = xml.find("</trophy>", p);
                    if (end == std::string::npos) break;
                    const std::string t = xml.substr(p, end - p);
                    auto attr = [&](const char* a) { const size_t q = t.find(std::string(a) + "=\""); return q == std::string::npos ? std::string() : t.substr(q + std::strlen(a) + 2, t.find('"', q + std::strlen(a) + 2) - q - std::strlen(a) - 2); };
                    auto tag = [&](const char* g) { const size_t q = t.find(std::string("<") + g + ">"); if (q == std::string::npos) return std::string(); const size_t s0 = q + std::strlen(g) + 2; return t.substr(s0, t.find("</", s0) - s0); };
                    const std::string ty = attr("ttype");
                    list.push_back({std::atoi(attr("id").c_str()), ty.empty() ? 'B' : ty[0], trim(wide(tag("name"))), trim(wide(tag("detail"))), {}});
                    { char ic[16]; std::snprintf(ic, sizeof ic, "TROP%03d.PNG", list.back().id); list.back().icon = entry(ic); }
                }
                break;
            }
            if (FILE* f = std::fopen(savePath.c_str(), "r")) { for (int id; std::fscanf(f, "%d%*[^\n]", &id) == 1;) unlocked.insert(id); std::fclose(f); }
            LOG("[trophy] %zu trophies, %zu unlocked%c", list.size(), unlocked.size(), 10);
            if (std::getenv("TROPHYTEST") && !list.empty())   // debug: the notification of a trophy (not unlocked)
                for (const auto& t : list) if (t.id == std::atoi(std::getenv("TROPHYTEST"))) rsx::toast(L"Trofeo desbloqueado", t.name, t.detail, t.type == 'G' ? 0xFFC83Cu : t.type == 'S' ? 0xC8CDD4u : 0xCD7F32u, t.icon);
        }
        auto find = [&](int id) -> const Trophy* { for (const auto& t : list) if (t.id == id) return &t; return nullptr; };
        if (n == "sceNpTrophyCreateContext" || n == "sceNpTrophyCreateHandle") { if (a32(c, 0)) c.wr32(a32(c, 0), 1); ret(c, CELL_OK); return true; }
        if (n == "sceNpTrophyGetTrophyUnlockState") {   // (ctx, handle, SceNpTrophyFlagArray* {u32 flag_bits[4]}, u32* count)
            const uint32_t fl = a32(c, 2);
            if (fl) { for (int i = 0; i < 4; ++i) c.wr32(fl + 4 * i, 0); for (int id : unlocked) if (id >= 0 && id < 128) c.wr32(fl + 4 * (id >> 5), c.rd32(fl + 4 * (id >> 5)) | 1u << (id & 31)); }
            if (a32(c, 3)) c.wr32(a32(c, 3), uint32_t(list.size()));
            ret(c, CELL_OK); return true;
        }
        if (n == "sceNpTrophyGetTrophyInfo") {   // (ctx, handle, id, SceNpTrophyDetails* {s32 id, u32 grade, char name[128], char desc[1024], u8 hidden}, SceNpTrophyData*)
            const int id = int(a32(c, 2));
            const Trophy* t = find(id);
            if (const uint32_t d = a32(c, 3)) {
                std::memset(c.mem + d, 0, 4 + 4 + 128 + 1024 + 4);
                c.wr32(d, uint32_t(id));
                c.wr32(d + 4, t ? (t->type == 'P' ? 1u : t->type == 'G' ? 2u : t->type == 'S' ? 3u : 4u) : 0u);
                if (t) {
                    auto put = [&](uint32_t a, const std::wstring& w, size_t cap) { std::string u; for (wchar_t ch : w) { if (ch < 0x80) u += char(ch); else if (ch < 0x800) { u += char(0xC0 | (ch >> 6)); u += char(0x80 | (ch & 63)); } else { u += char(0xE0 | (ch >> 12)); u += char(0x80 | ((ch >> 6) & 63)); u += char(0x80 | (ch & 63)); } } std::memcpy(c.mem + a, u.data(), std::min(u.size(), cap - 1)); };
                    put(d + 8, t->name, 128); put(d + 136, t->detail, 1024);
                }
            }
            if (const uint32_t d = a32(c, 4)) { std::memset(c.mem + d, 0, 16); c.wr32(d + 8, uint32_t(id)); c.wr8(d + 12, unlocked.count(id) ? 1 : 0); }
            ret(c, t ? CELL_OK : 0x8002290du /* SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID */); return true;
        }
        if (n == "sceNpTrophyUnlockTrophy") {   // (ctx, handle, id, s32* platinumId)
            const int id = int(a32(c, 2));
            if (a32(c, 3)) c.wr32(a32(c, 3), 0xFFFFFFFFu);   // no platinum unlocked by this
            auto unlock = [&](const Trophy& t) {
                unlocked.insert(t.id);
                if (FILE* f = std::fopen(savePath.c_str(), "a")) { std::fprintf(f, "%d %lld\n", t.id, (long long)std::time(nullptr)); std::fclose(f); }
                static const wchar_t* const title[6] = {L"Trophy unlocked", L"Trophy unlocked", L"Trophée débloqué", L"Trofeo desbloqueado", L"Trophäe freigeschaltet", L"Trofeo sbloccato"};
                const uint32_t rgb = t.type == 'P' ? 0x9FD6FFu : t.type == 'G' ? 0xFFC83Cu : t.type == 'S' ? 0xC8CDD4u : 0xCD7F32u;
                rsx::toast(title[std::clamp(settings().language, 0, 5)], t.name, t.detail, rgb, t.icon);
                LOG("[trophy] unlocked %d%c", t.id, 10);
            };
            const Trophy* t = find(id);
            if (t && !unlocked.count(id)) {
                unlock(*t);
                // the platinum trophy, if there is one, comes with all the others
                const Trophy* plat = nullptr; bool all = true;
                for (const auto& x : list) { if (x.type == 'P') plat = &x; else if (!unlocked.count(x.id)) all = false; }
                if (plat && all && !unlocked.count(plat->id)) { unlock(*plat); if (a32(c, 3)) c.wr32(a32(c, 3), uint32_t(plat->id)); }
            }
            ret(c, CELL_OK); return true;
        }
        ret(c, CELL_OK); return true;   // Init, Term, RegisterContext, Destroy*
    }
    // --- camera (cellCamera): the EyeToy features use a PlayStation Eye; here it is the host camera chosen in the menu
    //     (host_camera.cpp). CellCameraInfoEx: format, resolution, framerate, buffer, bytesize, width, height, dev_num,
    //     guid, info_ver, container, read_mode, pbuf[2]. The game opens it as RAW8 (Bayer), VGA, 60 fps.
    if (n.rfind("cellCamera", 0) == 0) {
        static uint32_t camFormat = 0, camW = 0, camH = 0, camBuf = 0, camBytes = 0;
        { static int shown = 0; if (shown++ < 40) LOG("[camera] %s(%x, %x, %x) lr %llx%c", n.c_str(), a32(c, 0), a32(c, 1), a32(c, 2), (unsigned long long)c.lr, 10); }
        if (n == "cellCameraInit" || n == "cellCameraEnd" || n == "cellCameraReset") { ret(c, CELL_OK); return true; }
        // one camera, device 0 (the game looks for devices 0, 1, 2... until one is not attached)
        const bool dev0 = a32(c, 0) == 0 && host_camera_available(settings().camera);
        if (n == "cellCameraIsAttached") { ret(c, dev0 ? 1 : 0); return true; }
        if (n == "cellCameraGetType") {   // CELL_CAMERA_EYETOY2 = the PlayStation Eye
            if (a32(c, 1)) c.wr32(a32(c, 1), dev0 ? 2 : 0);
            ret(c, dev0 ? CELL_OK : 0x80140808u); return true;
        }
        if (n == "cellCameraOpenEx") {
            if (!dev0) { ret(c, 0x80140808u); return true; }   // CELL_CAMERA_ERROR_DEVICE_NOT_FOUND
            const uint32_t p = a32(c, 1);
            camFormat = c.rd32(p);
            const uint32_t res = c.rd32(p + 4);
            camW = res == 2 ? 320 : res == 4 ? c.rd32(p + 0x14) : 640;
            camH = res == 2 ? 240 : res == 3 ? 360 : res == 4 ? c.rd32(p + 0x18) : 480;
            const uint32_t bpp = camFormat == 5 ? 4 : camFormat == 3 ? 2 : 1;
            camBytes = camW * camH * bpp;
            static uint32_t ownBuf = 0, ownBytes = 0;   // allocated once, kept for later opens
            camBuf = c.rd32(p + 0xc);
            if (!camBuf) { if (ownBytes < camBytes) { ownBuf = alloc_user(camBytes, 0x100); ownBytes = camBytes; } camBuf = ownBuf; c.wr32(p + 0xc, camBuf); }
            c.wr32(p + 0x10, camBytes); c.wr32(p + 0x14, camW); c.wr32(p + 0x18, camH);
            LOG("[camera] open: format %u, %ux%u, %u fps, buffer %08x (%u bytes), read mode %u%c", camFormat, camW, camH, c.rd32(p + 8), camBuf, camBytes, c.rd32(p + 0x2c), 10);
            ret(c, CELL_OK); return true;
        }
        if (n == "cellCameraStart") { ret(c, host_camera_start(settings().camera) ? CELL_OK : 0x80140808u /* CELL_CAMERA_ERROR_DEVICE_NOT_FOUND */); return true; }
        if (n == "cellCameraRead") {   // (dev, u32* frame_num, u32* bytes_read): the latest image into the buffer given at open
            uint32_t frame = 0, bytes = 0;
            if (camBuf && camBytes) bytes = host_camera_read(G->mem + camBuf, camBytes, int(camFormat), int(camW), int(camH), frame);
            if (a32(c, 1)) c.wr32(a32(c, 1), frame);
            if (a32(c, 2)) c.wr32(a32(c, 2), bytes);
            ret(c, CELL_OK); return true;
        }
        if (n == "cellCameraStop" || n == "cellCameraClose") { host_camera_stop(); ret(c, CELL_OK); return true; }
    }
    // --- pad (sys_io): one DualShock 3 connected on port 0; buttons from the host pad state (pad_state())
    if (n == "cellPadInit" || n == "cellPadEnd" || n == "cellPadSetPortSetting") { ret(c, CELL_OK); return true; }
    if (n == "cellPadSetActDirect") {   // (port, CellPadActParam* {u8 motor[6]}): motor 0 = small (on/off), motor 1 = large (0..255)
        if (a32(c, 0) == 0 && a32(c, 1)) g_rumble.store(uint32_t(c.rd8(a32(c, 1)) ? 1 : 0) << 8 | c.rd8(a32(c, 1) + 1));
        { static int shown = 0; if (g_rumble.load() && shown < 4) { ++shown; LOG("[pad] vibration %03x%c", g_rumble.load(), 10); } }
        ret(c, CELL_OK); return true;
    }
    if (n == "cellPadGetInfo2") {   // {max_connect, now_connect, system_info, port_status[7], port_setting[7], device_capability[7], device_type[7]}
        const uint32_t p = a32(c, 0);
        std::memset(c.mem + p, 0, 4 * (3 + 28));
        c.wr32(p, 7); c.wr32(p + 4, 1);
        c.wr32(p + 12, 1);             // port 0 connected
        c.wr32(p + 12 + 28 * 2, 0x13); // capability: PS3 conformity | press mode | actuator (vibration)
        ret(c, CELL_OK); return true;
    }
    if (n == "cellPadGetInfo") {   // {max_connect, now_connect, system_info, u16 vendor[127], u16 product[127], u8 status[127]}
        const uint32_t p = a32(c, 0);
        std::memset(c.mem + p, 0, 12 + 127 * 5);
        c.wr32(p, 7); c.wr32(p + 4, 1);
        c.wr16(p + 12, 0x054C); c.wr16(p + 12 + 254, 0x0268);
        c.wr8(p + 12 + 508, 1);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellPadGetData") {   // (port, CellPadData* {s32 len, u16 button[64]})
        const uint32_t p = a32(c, 1);
        std::memset(c.mem + p, 0, 4 + 128);
        if (a32(c, 0) == 0) {
            const PadState s = pad_state();
            c.wr32(p, 24);
            c.wr16(p + 4 + 2 * 2, s.digital1);
            c.wr16(p + 4 + 2 * 3, s.digital2);
            for (int k = 0; k < 4; ++k) c.wr16(p + 4 + 2 * (4 + k), s.analog[k]);
            // pressure (press mode): right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
            static const uint16_t bit[12][2] = {{1, 0x20}, {1, 0x80}, {1, 0x10}, {1, 0x40}, {2, 0x10}, {2, 0x20}, {2, 0x40}, {2, 0x80}, {2, 0x04}, {2, 0x08}, {2, 0x01}, {2, 0x02}};
            for (int k = 0; k < 12; ++k) {
                const bool down = (bit[k][0] == 1 ? s.digital1 : s.digital2) & bit[k][1];
                c.wr16(p + 4 + 2 * (8 + k), s.pressure[k] ? s.pressure[k] : (down ? 255 : 0));
            }
            for (int k = 0; k < 4; ++k) c.wr16(p + 4 + 2 * (20 + k), 512);   // sixaxis X, Y, Z, gyro: level
            c.wr32(p, 24);
        }
        ret(c, CELL_OK); return true;
    }
    // --- libc-style helpers of sysPrxForUser
    if (n == "_sys_printf") { const std::string s = gformat(c, a32(c, 0), VaArgs{&c, 1, 0}); { std::lock_guard<std::mutex> lk(g_log); std::fwrite(s.data(), 1, s.size(), stdout); } ret(c, s.size()); return true; }
    if (n == "_sys_vprintf") { const std::string s = gformat(c, a32(c, 0), VaArgs{&c, 0, a32(c, 1)}); { std::lock_guard<std::mutex> lk(g_log); std::fwrite(s.data(), 1, s.size(), stdout); } ret(c, s.size()); return true; }
    if (n == "_sys_sprintf") { ret(c, put_str(c, a32(c, 0), ~0ull, gformat(c, a32(c, 1), VaArgs{&c, 2, 0}))); return true; }
    if (n == "_sys_snprintf") { ret(c, put_str(c, a32(c, 0), c.r[4], gformat(c, a32(c, 2), VaArgs{&c, 3, 0}))); return true; }
    if (n == "_sys_vsnprintf") { ret(c, put_str(c, a32(c, 0), c.r[4], gformat(c, a32(c, 2), VaArgs{&c, 0, a32(c, 3)}))); return true; }
    if (n == "_sys_vsprintf") { ret(c, put_str(c, a32(c, 0), ~0ull, gformat(c, a32(c, 1), VaArgs{&c, 0, a32(c, 2)}))); return true; }
    if (n == "_sys_strchr") {
        const char* s = reinterpret_cast<const char*>(c.mem + a32(c, 0));
        const char* p = std::strchr(s, int(c.r[4] & 0xff));
        ret(c, p ? a32(c, 0) + uint32_t(p - s) : 0); return true;
    }
    if (n == "_sys_strncmp") { ret(c, uint64_t(int64_t(std::strncmp(reinterpret_cast<char*>(c.mem + a32(c, 0)), reinterpret_cast<char*>(c.mem + a32(c, 1)), size_t(c.r[5]))))); return true; }
    if (n == "_sys_strncpy") { std::strncpy(reinterpret_cast<char*>(c.mem + a32(c, 0)), reinterpret_cast<char*>(c.mem + a32(c, 1)), size_t(c.r[5])); ret(c, a32(c, 0)); return true; }
    if (n == "_sys_strncat") { std::strncat(reinterpret_cast<char*>(c.mem + a32(c, 0)), reinterpret_cast<char*>(c.mem + a32(c, 1)), size_t(c.r[5])); ret(c, a32(c, 0)); return true; }
    // --- sysPrxForUser: process / threads / lwmutex / lwcond / memory
    if (n == "sys_initialize_tls") {
        g_tlsImage = a32(c, 1); g_tlsFile = a32(c, 2); g_tlsMem = a32(c, 3);
        c.r[13] = make_tls();
        ret(c, CELL_OK); return true;
    }
    if (n == "sys_ppu_thread_get_id") { c.wr64(a32(c, 0), t_tid); ret(c, CELL_OK); return true; }
    if (n == "sys_ppu_thread_create") {   // (u64* id, u32 entry_opd, u64 arg, s32 prio, u32 stacksize, u64 flags, char* name)
        auto t = std::make_shared<GThread>();
        {
            std::lock_guard<std::mutex> lk(g_thMx);
            t->id = g_nextTid++;
            g_threads[t->id] = t;
        }
        t->name = cstr(c, uint32_t(c.r[9]));
        c.wr64(a32(c, 0), t->id);
        const uint32_t opd = a32(c, 1), stack = uint32_t(c.r[7]);
        const uint64_t arg = c.r[5];
        LOG("[lv2] thread create '%s' id %x entry 0x%x\n", t->name.c_str(), t->id, c.rd32(opd));
        t->th = std::thread(run_thread, t, opd, arg, stack);
        ret(c, CELL_OK); return true;
    }
    if (n == "sys_ppu_thread_exit") throw ThreadExit{c.r[3]};
    if (n == "sys_process_exit") throw ProcessExit{int(c.r[3])};
    if (n == "_sys_process_atexitspawn" || n == "_sys_process_at_Exitspawn" || n == "sys_prx_exitspawn_with_level") { ret(c, CELL_OK); return true; }
    if (n == "sys_time_get_system_time") { static const bool lg = std::getenv("TBPROF") != nullptr; if (lg) { static std::mutex m; static std::map<uint32_t, int> h; static uint64_t t0 = now_us(); std::lock_guard<std::mutex> lk(m); ++h[uint32_t(c.lr)]; if (now_us() - t0 > 3000000) { for (auto& [k, v] : h) LOG("[systime] lr %x: %d\n", k, v); h.clear(); t0 = now_us(); } } ret(c, now_us()); return true; }
    if (n == "sys_lwmutex_create") {   // (sys_lwmutex_t*, attr*)
        const uint32_t m = a32(c, 0), attr = a32(c, 1);
        c.wr32(m, 0); c.wr32(m + 4, 0); c.wr32(m + 8, attr ? c.rd32(attr) : 0); c.wr32(m + 0xc, 0);
        lwm(m);
        ret(c, CELL_OK); return true;
    }
    if (n == "sys_lwmutex_destroy") { ret(c, CELL_OK); return true; }
    if (n == "sys_lwmutex_lock") {
        auto m = lwm(a32(c, 0));
        const bool ok = mlock(*m, c.r[4]);
        if (ok) { c.wr32(a32(c, 0), t_tid); c.wr32(a32(c, 0) + 0xc, uint32_t(m->count)); }
        ret(c, ok ? CELL_OK : LV2_ETIMEDOUT); return true;
    }
    if (n == "sys_lwmutex_trylock") {
        auto m = lwm(a32(c, 0));
        const bool ok = m->m.try_lock();
        if (ok) { m->owner = t_tid; ++m->count; c.wr32(a32(c, 0), t_tid); c.wr32(a32(c, 0) + 0xc, uint32_t(m->count)); }
        ret(c, ok ? CELL_OK : LV2_EBUSY); return true;
    }
    if (n == "sys_lwmutex_unlock") {
        auto m = lwm(a32(c, 0));
        munlock(*m);
        c.wr32(a32(c, 0) + 0xc, uint32_t(m->count));
        if (!m->count) c.wr32(a32(c, 0), 0xFFFFFFFFu);
        ret(c, CELL_OK); return true;
    }
    if (n == "sys_lwcond_create") {   // (sys_lwcond_t*, sys_lwmutex_t*, attr*)
        c.wr32(a32(c, 0), a32(c, 1));
        ret(c, CELL_OK); return true;
    }
    if (n == "sys_lwcond_destroy") { ret(c, CELL_OK); return true; }
    if (n == "sys_lwcond_wait") {     // (sys_lwcond_t*, timeout)
        auto m = lwm(c.rd32(a32(c, 0)));
        const bool ok = mwait(*m, m->cv, c.r[4]);
        ret(c, ok ? CELL_OK : LV2_ETIMEDOUT); return true;
    }
    if (n == "sys_lwcond_signal" || n == "sys_lwcond_signal_to") { lwm(c.rd32(a32(c, 0)))->cv.notify_one(); ret(c, CELL_OK); return true; }
    if (n == "sys_lwcond_signal_all") { lwm(c.rd32(a32(c, 0)))->cv.notify_all(); ret(c, CELL_OK); return true; }
    if (n == "_sys_malloc") { ret(c, alloc_heap(a32(c, 0))); return true; }
    if (n == "_sys_memalign") { ret(c, alloc_heap(a32(c, 1), std::max<uint32_t>(a32(c, 0), 16))); return true; }
    if (n == "_sys_free") { ret(c, CELL_OK); return true; }
    if (n == "sys_prx_load_module" || n == "sys_prx_start_module" || n == "sys_prx_stop_module" || n == "sys_prx_unload_module") { ret(c, CELL_OK); return true; }
    if (n == "sys_get_random_number") { for (uint32_t k = 0; k < a32(c, 1); ++k) c.wr8(a32(c, 0) + k, uint8_t(std::rand())); ret(c, CELL_OK); return true; }
    if (n == "cellSpursCreateTask" && a32(c, 2) != 0xbc5180) {   // (taskset, u32* taskId, elf, contextSave, contextSize, lsPattern, arg): the Zlib task is done on the host
        const uint32_t arg = a32(c, 6);
        const uint32_t id = snd::create_task(a32(c, 0), a32(c, 2), a32(c, 3), arg ? c.rd64(arg) : 0, arg ? c.rd64(arg + 8) : 0);
        c.wr32(a32(c, 1), id);
        ret(c, CELL_OK); return true;
    }
    if (n == "_cellSpursSendSignal") { snd::signal(a32(c, 0), a32(c, 1)); ret(c, CELL_OK); return true; }
    if (n == "cellAudioPortOpen") {   // (CellAudioPortParam* {u64 channels, u64 blocks, u64 attr, float level}, u32* portNum)
        snd::g_ch = uint32_t(c.rd64(a32(c, 0))); snd::g_blocks = uint32_t(c.rd64(a32(c, 0) + 8));
        if (!snd::g_port) { snd::g_port = alloc_heap(snd::g_blocks * 256 * snd::g_ch * 4, 0x100); snd::g_readIdx = alloc_heap(16, 16); }
        std::memset(G->mem + snd::g_port, 0, snd::g_blocks * 256 * snd::g_ch * 4);
        c.wr64(snd::g_readIdx, 0);
        c.wr32(a32(c, 1), 0);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellAudioGetPortConfig") {   // (portNum, CellAudioPortConfig*)
        const uint32_t p = a32(c, 1);
        c.wr32(p, snd::g_readIdx); c.wr32(p + 4, snd::g_started ? 2 : 1); c.wr64(p + 8, snd::g_ch); c.wr64(p + 16, snd::g_blocks);
        c.wr32(p + 24, snd::g_blocks * 256 * snd::g_ch * 4); c.wr32(p + 28, snd::g_port);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellAudioPortStart") {
        if (!snd::g_started.exchange(true)) std::thread(snd::port_thread).detach();
        ret(c, CELL_OK); return true;
    }
    // --- SPURS: the Edge Zlib inflate task is done on the host (same result as the SPU task), event flags polled
    if (n == "_cellSpursJobChainAttributeInitialize") {   // (attr, jmRev, sdkRev, u64* jobChainEntry, sizeJobDescriptor, maxGrabbedJob, prio*, maxContention, ...)
        g_jobChainEntry = a32(c, 3);
        LOG("[spurs] %d SPU worker threads (%u hardware threads)\n", jobs::g_workers, std::thread::hardware_concurrency());
        LOG("[spurs] job chain attribute: command list 0x%x, job descriptor size %u, max grabbed %u, max contention %u (lr %llx)\n",
            g_jobChainEntry, a32(c, 4), a32(c, 5), a32(c, 7), (unsigned long long)c.lr);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellSpursRunJobChain" || n == "cellSpursJobGuardNotify") {
        static int shown = 0;
        if (std::getenv("JOBDUMP") && shown++ < 6) {
            std::string s; char b[40];
            for (uint32_t k = 0; k < 48; ++k) { std::snprintf(b, sizeof b, " %016llx", (unsigned long long)c.rd64(g_jobChainEntry + 8 * k)); s += b; }
            LOG("[spurs] %s(%x) lr %llx, command list:%s\n", n.c_str(), a32(c, 0), (unsigned long long)c.lr, s.c_str());
            for (uint32_t k = 0; k < 48; ++k) {
                const uint64_t w = c.rd64(g_jobChainEntry + 8 * k);
                if (w == 0x7f) break;
                if ((w & 7) || !w) continue;
                s.clear();
                for (uint32_t o = 0; o < 0x100; o += 4) { std::snprintf(b, sizeof b, " %08x", c.rd32(uint32_t(w) + o)); s += b; }
                LOG("[spurs]   job %llx:%s\n", (unsigned long long)w, s.c_str());
            }
        }
        if (g_jobsOn) {
            if (n == "cellSpursRunJobChain") { jobs::start(g_jobChainEntry); jobs::kick(); }
            else {   // like SPURS: a notify on a guard that is already open is dropped (it used to go negative and let the
                     // chain pass the next guard early, ahead of the render thread: a deadlock every minute or two at 144 Hz)
                auto g = jobs::guard_of(a32(c, 0));
                int32_t r = g->remaining.load();
                while (r > 0 && !g->remaining.compare_exchange_weak(r, r - 1)) {}
                if (r == 1) jobs::kick();
            }
        }
        ret(c, CELL_OK); return true;
    }
    if (n == "cellSpursJobGuardInitialize") {   // (jobChain, guard, notifyCount, requestSpuCount, autoReset)
        auto g = jobs::guard_of(a32(c, 1));
        g->count = int32_t(a32(c, 2)); g->remaining = g->count; g->autoReset = a32(c, 4) != 0;
        LOG("[spurs] job guard %x: notify count %d, auto reset %d\n", a32(c, 1), g->count, int(g->autoReset));
        ret(c, CELL_OK); return true;
    }
    if (n == "_cellSpursEventFlagInitialize") {   // (spurs, taskset, CellSpursEventFlag*, clearMode, direction)
        const uint32_t ef = a32(c, 2);
        std::memset(c.mem + ef, 0, 128);
        c.wr8(ef + 0x0F, uint8_t(c.r[6]));
        c.wr8(ef + 0x0E, uint8_t(c.r[7]));
        ret(c, CELL_OK); return true;
    }
    if (n == "cellSpursEventFlagWait" || n == "cellSpursEventFlagTryWait") {   // (ef, u16* mask, mode: 0 OR / 1 AND)
        const uint32_t ef = a32(c, 0), mp = a32(c, 1);
        const uint16_t want = c.rd16(mp);
        const bool all = (c.r[5] & 1) != 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            // SPU job chains / tasks other than Edge Zlib are not executed yet: after a while the wait is satisfied
            // as if the SPU work had finished, so the game logic keeps running (logged)
            if (n == "cellSpursEventFlagWait" && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(g_jobsOn ? 600000 : 30)) {
                static std::atomic<int> faked{0};
                if (faked++ < 10) LOG("[spurs] event flag %x wait %04x not set by any SPU work: assumed done (lr 0x%llx)\n", ef, want, (unsigned long long)c.lr);
                c.wr16(mp, want);
                ret(c, CELL_OK); return true;
            }
            uint32_t* p = reinterpret_cast<uint32_t*>(c.mem + ef);
            uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST);
            const uint16_t ev = uint16_t(__builtin_bswap32(raw) >> 16);
            const uint16_t hit = ev & want;
            if (all ? hit == want : hit != 0) {
                if (c.rd8(ef + 0x0F) == 0) {   // CLEAR_AUTO: consume the bits we return
                    for (;;) {
                        const uint32_t cur = __builtin_bswap32(raw);
                        const uint32_t nw = __builtin_bswap32(cur & ~(uint32_t(hit) << 16));
                        if (__atomic_compare_exchange_n(p, &raw, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
                    }
                }
                c.wr16(mp, hit);
                ret(c, CELL_OK); return true;
            }
            if (n == "cellSpursEventFlagTryWait") { ret(c, 0x80410901); return true; }   // CELL_SPURS_TASK_ERROR_BUSY
            {   // woken by set_event_flag; the 1 ms timeout covers bits set by anything else
                std::unique_lock<std::mutex> lk(jobs::g_efMx);
                const uint32_t seen = __atomic_load_n(p, __ATOMIC_SEQ_CST);
                jobs::g_efCv.wait_for(lk, std::chrono::milliseconds(1), [&] { return __atomic_load_n(p, __ATOMIC_SEQ_CST) != seen; });
            }
        }
    }
    if (n == "_cellSpursLFQueuePushBody") {   // (lfqueue, const void* element, isBlocking)
        const uint32_t lr = uint32_t(c.lr);
        if (lr >= 0x4970f8 && lr < 0x4972e0) {   // Edge Zlib inflate queue (edgeZlibAddInflateQueueElement)
            const uint32_t e = a32(c, 1);
            // element: src, dst, compressed size, uncompressed size, counter | type, event flag, u16 bits
            const uint32_t src = c.rd32(e), dst = c.rd32(e + 4), srcSize = c.rd32(e + 8), dstMax = c.rd32(e + 0xC);
            const uint32_t ctrw = c.rd32(e + 0x10), ef = c.rd32(e + 0x14);
            const uint16_t bits = c.rd16(e + 0x18);
            const uint32_t counter = ctrw & ~3u, mode = ctrw & 3;
            uLongf outLen = dstMax;
            int zr = Z_STREAM_END;
            const uint64_t tz = now_us();
            for (int wbits : {-15, 15}) {   // raw deflate, then zlib-wrapped
                z_stream zs{};
                zs.next_in = c.mem + src; zs.avail_in = srcSize;
                zs.next_out = c.mem + dst; zs.avail_out = dstMax;
                inflateInit2(&zs, wbits);
                zr = inflate(&zs, Z_FINISH);
                outLen = zs.total_out;
                inflateEnd(&zs);
                if (zr == Z_STREAM_END) break;
            }
            if (zr != Z_STREAM_END && srcSize == dstMax) { std::memcpy(c.mem + dst, c.mem + src, srcSize); zr = Z_STREAM_END; outLen = srcSize; }   // stored
            if (g_loadLog && now_us() - tz > 3000) LOG("[load] thread %x: inflate %u -> %lu bytes took %.1f ms (flip %llu)%c", t_tid, srcSize, (unsigned long)outLen, (now_us() - tz) / 1000.0, (unsigned long long)gcm::flips.load(), 10);
            static int logged = 0;
            if (std::getenv("ZLIBDUMP")) {
                std::string s; char b[16];
                for (uint32_t k = 0; k < 0x20; k += 4) { std::snprintf(b, sizeof b, " %08x", c.rd32(e + k)); s += b; }
                LOG("[zlib] element lr %x:%s\n", lr, s.c_str());
            }
            if (CFG.trace || zr != Z_STREAM_END || logged++ < 3)
                LOG("[zlib] src %x (%u) -> dst %x (max %u) mode %u: %d, %lu bytes, counter %x flag %x bits %x\n", src, srcSize, dst, dstMax, mode, zr, (unsigned long)outLen, counter, ef, bits);
            if (counter) {   // decrement the work-to-do counter; the last element raises the event flag bits
                uint32_t* p = reinterpret_cast<uint32_t*>(c.mem + counter);
                uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST), nv;
                do { nv = __builtin_bswap32(__builtin_bswap32(raw) - 1); } while (!__atomic_compare_exchange_n(p, &raw, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
                if (__builtin_bswap32(nv) == 0 && ef) {
                    uint32_t* q = reinterpret_cast<uint32_t*>(c.mem + ef);
                    uint32_t r0 = __atomic_load_n(q, __ATOMIC_SEQ_CST), n1;
                    do { n1 = __builtin_bswap32(__builtin_bswap32(r0) | (uint32_t(bits) << 16)); } while (!__atomic_compare_exchange_n(q, &r0, n1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
                }
            } else if (ef) {
                uint32_t* q = reinterpret_cast<uint32_t*>(c.mem + ef);
                uint32_t r0 = __atomic_load_n(q, __ATOMIC_SEQ_CST), n1;
                do { n1 = __builtin_bswap32(__builtin_bswap32(r0) | (uint32_t(bits) << 16)); } while (!__atomic_compare_exchange_n(q, &r0, n1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            }
            ret(c, CELL_OK); return true;
        }
        LOG("[spurs] LFQueue push from 0x%x not handled (queue %x)\n", lr, a32(c, 0));
        ret(c, CELL_OK); return true;
    }
    // --- file system (sys_fs)
    if (n == "cellFsOpen") {   // (path, flags, s32* fd, arg, size)
        const std::string gp = cstr(c, a32(c, 0)), hp = host_path(gp);
        const uint32_t fl = a32(c, 1);
        const char* mode = (fl & 0x3) == 0 ? "rb" : ((fl & 0x40) ? ((fl & 0x200) ? "w+b" : "a+b") : "r+b");
        FILE* f = std::fopen(hp.c_str(), mode);
        LOG("[fs] open %s%s\n", gp.c_str(), f ? "" : "  NOT FOUND");
        if (const size_t k = gp.find("/sumotool/ps3/"); f && k != std::string::npos) {   // the screen the menus show now
            const std::string rest = gp.substr(k + 14), dir = rest.substr(0, rest.find('/'));
            if (rest == dir + "/" + dir + ".dat" && dir != "global" && dir != "popups" && dir != "numberplate") rsx::screen_loaded(dir);
        }
        if (!f) { ret(c, LV2_ENOENT); return true; }
        std::lock_guard<std::mutex> lk(g_fsMx);
        const uint32_t fd = g_nextFd++;
        g_files[fd] = GFile{f, hp};
        c.wr32(a32(c, 2), fd);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsClose") {
        std::lock_guard<std::mutex> lk(g_fsMx);
        auto it = g_files.find(a32(c, 0));
        if (it != g_files.end()) { std::fclose(it->second.f); g_files.erase(it); }
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsRead" || n == "cellFsWrite") {   // (fd, buf, size, u64* n)
        FILE* f = nullptr;
        { std::lock_guard<std::mutex> lk(g_fsMx); auto it = g_files.find(a32(c, 0)); if (it != g_files.end()) f = it->second.f; }
        if (!f) { ret(c, 0x80010009); return true; }
        const uint64_t tio = now_us();
        const int64_t pos = _ftelli64(f);
        const size_t k = n == "cellFsRead" ? std::fread(c.mem + a32(c, 1), 1, size_t(c.r[5]), f) : std::fwrite(c.mem + a32(c, 1), 1, size_t(c.r[5]), f);
        if (g_loadLog && now_us() - tio > 3000) LOG("[load] thread %x: %s of %llu bytes took %.1f ms (flip %llu)%c", t_tid, n.c_str(), (unsigned long long)c.r[5], (now_us() - tio) / 1000.0, (unsigned long long)gcm::flips.load(), 10);
        if (n == "cellFsRead" && pos >= 0) {   // menu screens: the button icons of the controller in use
            std::string hp; { std::lock_guard<std::mutex> lk(g_fsMx); auto it = g_files.find(a32(c, 0)); if (it != g_files.end()) hp = it->second.path; }
            glyphs_patch_read(hp, uint64_t(pos), c.mem + a32(c, 1), k);
        }
        if (a32(c, 3)) c.wr64(a32(c, 3), k);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsLseek") {   // (fd, s64 offset, whence, u64* pos)
        FILE* f = nullptr;
        { std::lock_guard<std::mutex> lk(g_fsMx); auto it = g_files.find(a32(c, 0)); if (it != g_files.end()) f = it->second.f; }
        if (!f) { ret(c, 0x80010009); return true; }
        _fseeki64(f, int64_t(c.r[4]), int(c.r[5]) == 0 ? SEEK_SET : int(c.r[5]) == 1 ? SEEK_CUR : SEEK_END);
        if (a32(c, 3)) c.wr64(a32(c, 3), uint64_t(_ftelli64(f)));
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsOpendir") {   // (path, s32* fd)
        const std::string gp = cstr(c, a32(c, 0)), hp = host_path(gp);
        std::error_code ec;
        if (!std::filesystem::is_directory(hp, ec)) { LOG("[fs] opendir %s -> %s NOT FOUND\n", gp.c_str(), hp.c_str()); ret(c, LV2_ENOENT); return true; }
        std::vector<std::pair<std::string, bool>> list = {{".", true}, {"..", true}};
        for (auto& de : std::filesystem::directory_iterator(hp, ec)) list.push_back({de.path().filename().string(), de.is_directory()});
        std::lock_guard<std::mutex> lk(g_fsMx);
        const uint32_t fd = g_nextFd++;
        g_dirs[fd] = Dir{list, 0};
        c.wr32(a32(c, 1), fd);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsReaddir") {   // (fd, CellFsDirent* {u8 type, u8 namlen, char name[256]}, u64* nread)
        std::lock_guard<std::mutex> lk(g_fsMx);
        auto it = g_dirs.find(a32(c, 0));
        if (it == g_dirs.end()) { ret(c, 0x80010009); return true; }
        auto& d = it->second;
        if (d.pos >= d.list.size()) { c.wr64(a32(c, 2), 0); ret(c, CELL_OK); return true; }
        const auto& e = d.list[d.pos++];
        const uint32_t p = a32(c, 1);
        c.wr8(p, e.second ? 1 : 2);
        c.wr8(p + 1, uint8_t(std::min<size_t>(e.first.size(), 255)));
        std::memset(c.mem + p + 2, 0, 256);
        std::memcpy(c.mem + p + 2, e.first.data(), std::min<size_t>(e.first.size(), 255));
        c.wr64(a32(c, 2), 0x102);
        ret(c, CELL_OK); return true;
    }
    if (n == "cellFsClosedir") { std::lock_guard<std::mutex> lk(g_fsMx); g_dirs.erase(a32(c, 0)); ret(c, CELL_OK); return true; }
    if (n == "cellFsStat" || n == "cellFsFstat") {   // CellFsStat: mode u32, uid, gid, atime u64, mtime, ctime, size u64, blksize u64
        std::string hp;
        if (n == "cellFsStat") hp = host_path(cstr(c, a32(c, 0)));
        else { std::lock_guard<std::mutex> lk(g_fsMx); auto it = g_files.find(a32(c, 0)); if (it != g_files.end()) hp = it->second.path; }
        std::error_code ec;
        if (hp.empty() || !std::filesystem::exists(hp, ec)) { if (CFG.trace) LOG("[fs] stat %s missing\n", hp.c_str()); ret(c, LV2_ENOENT); return true; }
        const bool isDir = std::filesystem::is_directory(hp, ec);
        const uint64_t fsize = isDir ? 0 : uint64_t(std::filesystem::file_size(hp, ec));
        const uint32_t s = a32(c, 1);
        std::memset(c.mem + s, 0, 0x34);
        c.wr32(s, isDir ? 0x41ff : 0x81ff);
        c.wr64(s + 0x24, fsize);
        c.wr64(s + 0x2c, 512);
        ret(c, CELL_OK); return true;
    }
    return false;   // not handled here: built-in stubs of hle.cpp (logged once, answer CELL_OK)
}

// ------------------------------------------------------------------ kernel objects (semaphores, event queues/ports, timers,
// mutexes, condition variables, event flags)
struct Sema { std::mutex m; std::condition_variable cv; int64_t val = 0, max = 0; };
struct Event { uint64_t source, d1, d2, d3; };
struct EQueue { std::mutex m; std::condition_variable cv; std::deque<Event> q; uint64_t key = 0; bool destroyed = false; };
struct EPort { uint32_t queue = 0; uint64_t name = 0; };
struct EFlag { std::mutex m; std::condition_variable cv; uint64_t bits = 0; };
struct KMutex { std::recursive_timed_mutex m; uint32_t owner = 0; int count = 0; };
struct KCond { uint32_t mutex = 0; std::condition_variable_any cv; };
struct KTimer { uint32_t queue = 0; uint64_t name = 0, d1 = 0, d2 = 0; std::atomic<bool> running{false}; std::thread th; };

std::mutex g_kMx;
std::map<uint32_t, std::shared_ptr<Sema>> g_sema;
std::map<uint32_t, std::shared_ptr<EQueue>> g_equeue;
std::map<uint32_t, EPort> g_eport;
std::map<uint32_t, std::shared_ptr<EFlag>> g_eflag;
std::map<uint32_t, std::shared_ptr<KMutex>> g_kmutex;
std::map<uint32_t, std::shared_ptr<KCond>> g_kcond;
std::map<uint32_t, std::shared_ptr<KTimer>> g_ktimer;
std::atomic<uint32_t> g_kId{0x40000100};

template <class T> std::shared_ptr<T> kget(std::map<uint32_t, std::shared_ptr<T>>& m, uint32_t id) {
    std::lock_guard<std::mutex> lk(g_kMx);
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
}
std::shared_ptr<EQueue> equeue(uint32_t id) { return kget(g_equeue, id); }
std::mutex g_evCntMx;
std::map<uint32_t, uint64_t> g_evCnt;   // events posted per queue (lv2_dump_waits)
bool post_event(uint32_t queue, const Event& e) {
    auto q = equeue(queue);
    if (!q) return false;
    { std::lock_guard<std::mutex> lk(q->m); q->q.push_back(e); }
    q->cv.notify_one();
    { std::lock_guard<std::mutex> lk(g_evCntMx); ++g_evCnt[queue]; }
    if (std::getenv("EVTRACE")) LOG("[ev] queue %x <- source %llx d1 %llx d2 %llx d3 %llx (thread %x)\n", queue, (unsigned long long)e.source, (unsigned long long)e.d1, (unsigned long long)e.d2, (unsigned long long)e.d3, t_tid);
    return true;
}
template <class Pred, class Lock, class CV> bool wait_us(CV& cv, Lock& lk, uint64_t us, Pred p) {
    if (!us) { cv.wait(lk, p); return true; }
    return cv.wait_for(lk, std::chrono::microseconds(us), p);
}

bool kernel_syscall(PPU& c, uint32_t nr) {
    switch (nr) {
        case 90: {   // sys_semaphore_create(u32* id, attr*, s32 initial, s32 max)
            auto s = std::make_shared<Sema>(); s->val = int32_t(c.r[5]); s->max = int32_t(c.r[6]);
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_sema[id] = s; }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 91: { std::lock_guard<std::mutex> lk(g_kMx); g_sema.erase(a32(c, 0)); ret(c, CELL_OK); return true; }
        case 92: case 93: {   // wait(id, timeout) / trywait(id)
            auto s = kget(g_sema, a32(c, 0));
            if (!s) { ret(c, LV2_ESRCH); return true; }
            std::unique_lock<std::mutex> lk(s->m);
            if (nr == 93) { if (s->val > 0) { --s->val; ret(c, CELL_OK); } else ret(c, LV2_EBUSY); return true; }
            if (!wait_us(s->cv, lk, c.r[4], [&] { return s->val > 0; })) { ret(c, LV2_ETIMEDOUT); return true; }
            --s->val; ret(c, CELL_OK); return true;
        }
        case 94: {   // post(id, count)
            auto s = kget(g_sema, a32(c, 0));
            if (!s) { ret(c, LV2_ESRCH); return true; }
            { std::lock_guard<std::mutex> lk(s->m); s->val += int32_t(c.r[4]); }
            s->cv.notify_all(); ret(c, CELL_OK); return true;
        }
        case 114: { auto s = kget(g_sema, a32(c, 0)); if (s) { std::lock_guard<std::mutex> lk(s->m); c.wr32(a32(c, 1), uint32_t(s->val)); } ret(c, s ? CELL_OK : LV2_ESRCH); return true; }
        case 128: {   // sys_event_queue_create(u32* id, attr*, u64 key, s32 size)
            auto q = std::make_shared<EQueue>(); q->key = c.r[5];
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_equeue[id] = q; }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 129: {   // destroy(id, mode): wakes waiters with ECANCELED
            auto q = equeue(a32(c, 0));
            if (q) { { std::lock_guard<std::mutex> lk(q->m); q->destroyed = true; } q->cv.notify_all(); std::lock_guard<std::mutex> lk(g_kMx); g_equeue.erase(a32(c, 0)); }
            ret(c, CELL_OK); return true;
        }
        case 130: {   // receive(id, event*, timeout) -> r4..r7 = source, data1..3
            auto q = equeue(a32(c, 0));
            if (!q) { ret(c, LV2_ESRCH); return true; }
            std::unique_lock<std::mutex> lk(q->m);
            if (!wait_us(q->cv, lk, c.r[5], [&] { return !q->q.empty() || q->destroyed; })) { ret(c, LV2_ETIMEDOUT); return true; }
            if (q->q.empty()) { ret(c, 0x80010013); return true; }   // ECANCELED
            const Event e = q->q.front(); q->q.pop_front();
            c.r[4] = e.source; c.r[5] = e.d1; c.r[6] = e.d2; c.r[7] = e.d3;
            ret(c, CELL_OK); return true;
        }
        case 131: {   // tryreceive(id, sys_event_t* array, size, u32* number)
            auto q = equeue(a32(c, 0));
            if (!q) { ret(c, LV2_ESRCH); return true; }
            std::lock_guard<std::mutex> lk(q->m);
            uint32_t n = 0;
            while (!q->q.empty() && n < a32(c, 2)) {
                const Event e = q->q.front(); q->q.pop_front();
                const uint32_t p = a32(c, 1) + n * 32;
                c.wr64(p, e.source); c.wr64(p + 8, e.d1); c.wr64(p + 16, e.d2); c.wr64(p + 24, e.d3);
                ++n;
            }
            c.wr32(a32(c, 3), n); ret(c, CELL_OK); return true;
        }
        case 133: { auto q = equeue(a32(c, 0)); if (q) { std::lock_guard<std::mutex> lk(q->m); q->q.clear(); } ret(c, CELL_OK); return true; }   // drain
        case 134: {   // sys_event_port_create(u32* id, type, u64 name)
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_eport[id] = EPort{0, c.r[5]}; }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 135: { std::lock_guard<std::mutex> lk(g_kMx); g_eport.erase(a32(c, 0)); ret(c, CELL_OK); return true; }
        case 136: { std::lock_guard<std::mutex> lk(g_kMx); g_eport[a32(c, 0)].queue = a32(c, 1); ret(c, CELL_OK); return true; }   // connect_local
        case 137: { std::lock_guard<std::mutex> lk(g_kMx); g_eport[a32(c, 0)].queue = 0; ret(c, CELL_OK); return true; }
        case 138: {   // send(port, d1, d2, d3)
            EPort p;
            { std::lock_guard<std::mutex> lk(g_kMx); auto it = g_eport.find(a32(c, 0)); if (it == g_eport.end()) { ret(c, LV2_ESRCH); return true; } p = it->second; }
            if (!p.queue) { ret(c, 0x8001000E); return true; }   // ENOTCONN
            post_event(p.queue, Event{p.name, c.r[4], c.r[5], c.r[6]});
            ret(c, CELL_OK); return true;
        }
        case 70: {   // sys_timer_create(u32* id)
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_ktimer[id] = std::make_shared<KTimer>(); }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 71: { auto t = kget(g_ktimer, a32(c, 0)); if (t) { t->running = false; if (t->th.joinable()) t->th.detach(); } ret(c, CELL_OK); return true; }
        case 73: {   // _sys_timer_start(id, u64 base_time, u64 period)
            auto t = kget(g_ktimer, a32(c, 0));
            if (!t) { ret(c, LV2_ESRCH); return true; }
            const uint64_t base = c.r[4], period = c.r[5];
            t->running = true;
            t->th = std::thread([t, base, period] {
                uint64_t next = base ? base : now_us() + period;
                while (t->running) {
                    const uint64_t n = now_us();
                    if (n < next) std::this_thread::sleep_for(std::chrono::microseconds(next - n));
                    if (!t->running) break;
                    if (t->queue) post_event(t->queue, Event{t->name, t->d1, t->d2, next});
                    if (!period) { t->running = false; break; }
                    next += period;
                }
            });
            t->th.detach();
            ret(c, CELL_OK); return true;
        }
        case 74: { auto t = kget(g_ktimer, a32(c, 0)); if (t) t->running = false; ret(c, CELL_OK); return true; }
        case 75: {   // sys_timer_connect_event_queue(id, queue, u64 name, u64 d1, u64 d2)
            auto t = kget(g_ktimer, a32(c, 0));
            if (!t) { ret(c, LV2_ESRCH); return true; }
            t->queue = a32(c, 1); t->name = c.r[5]; t->d1 = c.r[6]; t->d2 = c.r[7];
            ret(c, CELL_OK); return true;
        }
        case 76: { auto t = kget(g_ktimer, a32(c, 0)); if (t) t->queue = 0; ret(c, CELL_OK); return true; }
        case 100: {   // sys_mutex_create(u32* id, attr*)
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_kmutex[id] = std::make_shared<KMutex>(); }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 101: ret(c, CELL_OK); return true;
        case 102: case 103: {   // lock(id, timeout) / trylock(id)
            auto m = kget(g_kmutex, a32(c, 0));
            if (!m) { ret(c, LV2_ESRCH); return true; }
            bool ok;
            if (nr == 103) ok = m->m.try_lock();
            else if (c.r[4]) ok = m->m.try_lock_for(std::chrono::microseconds(c.r[4]));
            else { m->m.lock(); ok = true; }
            if (ok) { m->owner = t_tid; ++m->count; }
            ret(c, ok ? CELL_OK : (nr == 103 ? LV2_EBUSY : LV2_ETIMEDOUT)); return true;
        }
        case 104: {
            auto m = kget(g_kmutex, a32(c, 0));
            if (!m) { ret(c, LV2_ESRCH); return true; }
            if (--m->count == 0) m->owner = 0;
            m->m.unlock(); ret(c, CELL_OK); return true;
        }
        case 105: {   // sys_cond_create(u32* id, mutex_id, attr*)
            auto k = std::make_shared<KCond>(); k->mutex = a32(c, 1);
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_kcond[id] = k; }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 106: ret(c, CELL_OK); return true;
        case 107: {   // wait(id, timeout): releases the mutex (all recursion levels) while waiting
            auto k = kget(g_kcond, a32(c, 0));
            auto m = k ? kget(g_kmutex, k->mutex) : nullptr;
            if (!m) { ret(c, LV2_ESRCH); return true; }
            const int n = m->count;
            for (int i = 1; i < n; ++i) m->m.unlock();
            m->count = 0; m->owner = 0;
            bool ok = true;
            if (c.r[4]) ok = k->cv.wait_for(m->m, std::chrono::microseconds(c.r[4])) == std::cv_status::no_timeout;
            else k->cv.wait(m->m);
            for (int i = 1; i < n; ++i) m->m.lock();
            m->count = n; m->owner = t_tid;
            ret(c, ok ? CELL_OK : LV2_ETIMEDOUT); return true;
        }
        case 108: case 110: { auto k = kget(g_kcond, a32(c, 0)); if (k) k->cv.notify_one(); ret(c, CELL_OK); return true; }
        case 109: { auto k = kget(g_kcond, a32(c, 0)); if (k) k->cv.notify_all(); ret(c, CELL_OK); return true; }
        case 82: {   // sys_event_flag_create(u32* id, attr*, u64 init)
            auto f = std::make_shared<EFlag>(); f->bits = c.r[5];
            const uint32_t id = g_kId++;
            { std::lock_guard<std::mutex> lk(g_kMx); g_eflag[id] = f; }
            c.wr32(a32(c, 0), id); ret(c, CELL_OK); return true;
        }
        case 83: ret(c, CELL_OK); return true;
        case 85: case 86: {   // wait(id, u64 bitptn, u32 mode, u64* result, timeout) / trywait(id, bitptn, mode, u64* result)
            auto f = kget(g_eflag, a32(c, 0));
            if (!f) { ret(c, LV2_ESRCH); return true; }
            const uint64_t pat = c.r[4]; const uint32_t mode = uint32_t(c.r[5]);
            auto okp = [&] { return (mode & 2) ? (f->bits & pat) != 0 : (f->bits & pat) == pat; };   // 1 = AND, 2 = OR
            std::unique_lock<std::mutex> lk(f->m);
            if (nr == 86) { if (!okp()) { ret(c, LV2_EBUSY); return true; } }
            else if (!wait_us(f->cv, lk, c.r[7], okp)) { ret(c, LV2_ETIMEDOUT); return true; }
            if (a32(c, 3)) c.wr64(a32(c, 3), f->bits);
            if (mode & 0x20) f->bits = 0; else if (mode & 0x10) f->bits &= ~pat;   // CLEAR_ALL / CLEAR
            ret(c, CELL_OK); return true;
        }
        case 87: { auto f = kget(g_eflag, a32(c, 0)); if (f) { { std::lock_guard<std::mutex> lk(f->m); f->bits |= c.r[4]; } f->cv.notify_all(); } ret(c, f ? CELL_OK : LV2_ESRCH); return true; }
        case 118: { auto f = kget(g_eflag, a32(c, 0)); if (f) { std::lock_guard<std::mutex> lk(f->m); f->bits &= c.r[4]; } ret(c, f ? CELL_OK : LV2_ESRCH); return true; }
        case 139: { auto f = kget(g_eflag, a32(c, 0)); if (f) { std::lock_guard<std::mutex> lk(f->m); c.wr64(a32(c, 1), f->bits); } ret(c, f ? CELL_OK : LV2_ESRCH); return true; }
        case 48: c.wr32(a32(c, 1), 1000); ret(c, CELL_OK); return true;    // sys_ppu_thread_get_priority
        case 47: ret(c, CELL_OK); return true;                            // sys_ppu_thread_set_priority
        case 49: c.wr32(a32(c, 0), 0xD0000000); c.wr32(a32(c, 0) + 4, 0x100000); ret(c, CELL_OK); return true;   // get_stack_information
        case 169: ret(c, CELL_OK); return true;                           // sys_spu_initialize
        default: return false;
    }
}

// ------------------------------------------------------------------ lv2 syscalls
void syscall(PPU& c) {
    const uint32_t nr = uint32_t(c.r[11]);
    if (CFG.trace) LOG("[sc %x] %u r3=%llx r4=%llx\n", t_tid, nr, (unsigned long long)c.r[3], (unsigned long long)c.r[4]);
    char wsName[40]; std::snprintf(wsName, sizeof wsName, "syscall %d r3=0x%x", int(nr), uint32_t(c.r[3]));
    WaitScope ws_(wsName, uint32_t(c.lr));
    if (kernel_syscall(c, nr)) return;
    switch (nr) {
        case 1: ret(c, 1); return;                                    // sys_process_getpid
        case 3: throw ProcessExit{int(c.r[3])};                       // sys_process_exit
        case 22: throw ProcessExit{int(c.r[3])};                      // sys_process_exit2
        case 41: throw ThreadExit{c.r[3]};                            // sys_ppu_thread_exit
        case 43: std::this_thread::yield(); ret(c, CELL_OK); return;  // sys_ppu_thread_yield
        case 44: {                                                    // sys_ppu_thread_join(id, u64* status)
            std::shared_ptr<GThread> t;
            { std::lock_guard<std::mutex> lk(g_thMx); auto it = g_threads.find(a32(c, 0)); if (it != g_threads.end()) t = it->second; }
            if (!t) { ret(c, LV2_ESRCH); return; }
            if (t->th.joinable()) t->th.join();
            if (a32(c, 1)) c.wr64(a32(c, 1), t->exitValue);
            ret(c, CELL_OK); return;
        }
        case 141: {   // sys_timer_usleep. Windows cannot sleep less than about a millisecond: short waits (the render thread
            // polls a label every 30 us right after each flip) are done by yielding until the time has passed.
            const uint64_t us = c.r[3];
            if (g_loadLog && us >= 50000) LOG("[load] thread %x: usleep %llu us (lr %llx, flip %llu)%c", t_tid, (unsigned long long)us, (unsigned long long)c.lr, (unsigned long long)gcm::flips.load(), 10);
            static const uint64_t spinBelow = std::getenv("USLEEPSPIN") ? std::strtoull(std::getenv("USLEEPSPIN"), nullptr, 10) : 100;
            if (us >= spinBelow) host_sleep_us(us);
            else { const uint64_t until = now_us() + us; while (now_us() < until) std::this_thread::yield(); }
            ret(c, CELL_OK); return;
        }
        case 142:   // sys_timer_sleep. 0x1e300: after sceNpManagerGetStatus answers "offline" (there is no PSN here) the game
                    // waits a whole second for a sign-in that cannot come, right after START at the title: skipped.
            if (uint32_t(c.lr) == 0x1e300 && c.r[3] == 1) { ret(c, CELL_OK); return; }
            if (g_loadLog) LOG("[load] thread %x: sleep %llu s (lr %llx)%c", t_tid, (unsigned long long)c.r[3], (unsigned long long)c.lr, 10); std::this_thread::sleep_for(std::chrono::seconds(c.r[3])); ret(c, CELL_OK); return;        // sys_timer_sleep
        case 145: { const uint64_t u = now_us() + 1700000000ull * 1000000ull;                               // sys_time_get_current_time(u64* sec, u64* nsec)
            if (a32(c, 0)) c.wr64(a32(c, 0), u / 1000000); if (a32(c, 1)) c.wr64(a32(c, 1), (u % 1000000) * 1000); ret(c, CELL_OK); return; }
        case 147: ret(c, 79800000); return;                                                                    // sys_time_get_timebase_frequency
        case 348: {                                                   // sys_memory_allocate(size, flags, u32* addr)
            const uint32_t size = a32(c, 0), align = (c.r[1 + 0] & 0) | ((c.r[4] & 0x400) ? 0x100000 : 0x10000);
            const uint32_t a = alloc_user(size, align);
            std::memset(G->mem + a, 0, size);
            c.wr32(a32(c, 2), a);
            ret(c, CELL_OK); return;
        }
        case 349: ret(c, CELL_OK); return;                            // sys_memory_free
        case 352: c.wr32(a32(c, 0), 0x0D500000); c.wr32(a32(c, 0) + 4, 0x0D500000 - (g_userNext - 0x30000000)); ret(c, CELL_OK); return;
        case 403: {                                                   // sys_tty_write(ch, buf, len, u32* written)
            const uint32_t buf = a32(c, 1), len = a32(c, 2);
            { std::lock_guard<std::mutex> lk(g_log); std::fwrite(G->mem + buf, 1, len, stdout); std::fflush(stdout); }
            if (a32(c, 3)) c.wr32(a32(c, 3), len);
            ret(c, CELL_OK); return;
        }
        default: {
            static std::set<uint32_t> seen;
            std::lock_guard<std::mutex> lk(g_log);
            if (seen.insert(nr).second) std::fprintf(stderr, "[sc] unimplemented syscall %u (r3=%llx r4=%llx r5=%llx)\n", nr, (unsigned long long)c.r[3], (unsigned long long)c.r[4], (unsigned long long)c.r[5]);
            ret(c, CELL_OK); return;
        }
    }
}

} // namespace

void lv2_set_pad(const PadState& s) { std::lock_guard<std::mutex> lk(g_padMx); g_pad = s; g_padLive = true; }
uint32_t lv2_rumble() { return g_rumble.load(); }

void lv2_install(Guest& g, const Lv2Config& cfg) {
    G = &g;
    CFG = cfg;
    hle_install();
    g_hleExt = imports;
    g_syscallHook = syscall;
    static HostHook base = g_hostHook;
    g_hostHook = [](PPU& c, uint32_t addr) -> bool {
        if (addr == gcm::CB_ADDR) { gcm::buffer_full(c); return true; }   // command buffer full callback
        return base(c, addr);
    };
}

unsigned long long lv2_flip_count() { return gcm::flips.load(); }
void lv2_dump_waits() {
    for (int n = 0; n < 4; ++n) {   // Edge geometry output buffers: shared, then one per SPU (start, offset, end, current, location, label cmd, ?)
        const uint32_t e = 0x01061900u + 0x80u * n;
        LOG("[wait] Edge output buffer %d: start %08x end %08x current %08x word6 %08x label word %08x\n", n, jobs::rd32m(e), jobs::rd32m(e + 8), jobs::rd32m(e + 12), jobs::rd32m(e + 24), jobs::rd32m(e + 20));
    }
    if (gcm::labels) LOG("[wait] RSX labels 0x400 %08x, 0x410 %08x, 0x420 %08x\n", gcm::rd(gcm::labels + 0x400), gcm::rd(gcm::labels + 0x410), gcm::rd(gcm::labels + 0x420));
    {
        std::lock_guard<std::mutex> lk(jobs::qmx);
        const uint32_t pc = jobs::g_mgrPc.load();
        LOG("[wait] job manager: pc %x word %016llx, waits for %u (1 guard, 2 end, 3 sync), queue %zu, busy %d, kicks %llu, jobs run %llu\n", pc, pc ? (unsigned long long)jobs::rd64m(pc) : 0ull,
            jobs::g_mgrWhat.load(), jobs::queue.size(), jobs::busy, (unsigned long long)jobs::kicks.load(), (unsigned long long)jobs::jobsRun.load());
        if (pc) for (int k = -4; k <= 4; ++k) LOG("[wait]   %x: %016llx\n", pc + 8 * k, (unsigned long long)jobs::rd64m(pc + 8 * k));
        std::string s; char b[48];
        for (unsigned k = jobs::g_mgrHistN - std::min(jobs::g_mgrHistN, 64u); k != jobs::g_mgrHistN; ++k) { const auto& h = jobs::g_mgrHist[k % 64]; std::snprintf(b, sizeof b, " %x:%x@%u", h[0], h[1], h[2] % 10000000u); s += b; }
        LOG("[wait] job manager history (pc:word@us):%s\n", s.c_str());
    }
    for (int w = 0; w < 16; ++w)   // jobs still running (exact pc only with NOSPUREC=1)
        if (const uint32_t j = jobs::g_workerJob[w].load()) {
            const Spu& sp = *jobs::g_workerSpu[w];
            LOG("[wait] SPU worker %d: job %x at pc 0x%x, lr %x sp %x r3 %08x r4 %08x r5 %08x\n", w, j, sp.pc, sp.r[0].w(0), sp.r[1].w(0), sp.r[3].w(0), sp.r[4].w(0), sp.r[5].w(0));
        }
    if (std::getenv("METHODHIST"))
        for (uint32_t k = 0; k < 0x4000; ++k)
            if (gcm::g_mhist[k]) LOG("[mhist] %04x %10llu  last %08x\n", k * 4, (unsigned long long)gcm::g_mhist[k].load(), gcm::g_mlast[k].load());
    {
        std::lock_guard<std::mutex> l3(g_evCntMx);
        for (auto& [q, n] : g_evCnt) {
            size_t pending = 0;
            if (auto eq = equeue(q)) { std::lock_guard<std::mutex> l4(eq->m); pending = eq->q.size(); }
            LOG("[wait] event queue %x: %llu events posted, %zu pending\n", q, (unsigned long long)n, pending);
        }
    }
    LOG("[wait] gcm flips %llu, labels 0x4c0 = %x, 0x4d0 = %x\n", (unsigned long long)gcm::flips.load(), gcm::rd(gcm::labels + 0x4c0), gcm::rd(gcm::labels + 0x4d0));
    {   // render context of the game (TOC-0x1ce4): buffer index @0x300, expected label values @0x304/0x308
        const uint32_t rc = __builtin_bswap32(*reinterpret_cast<uint32_t*>(G->mem + 0xc6f210 - 0x1ce4));
        if (rc) LOG("[wait] render ctx %x: index %u, expected %x %x\n", rc, gcm::rd(rc + 0x300), gcm::rd(rc + 0x304), gcm::rd(rc + 0x308));
    }
    std::lock_guard<std::mutex> lk(g_waitMx);
    const uint64_t now = now_us();
    for (WaitSlot* w : g_waitSlots) {
        const uint64_t since = w->since.load();
        if (!since) continue;
        std::string name;
        { std::lock_guard<std::mutex> l2(g_thMx); auto it = g_threads.find(w->tid); if (it != g_threads.end()) name = it->second->name; }
        LOG("[wait] thread %x '%s': in %s for %.1f s (lr 0x%x)\n", w->tid, name.c_str(), w->what, (now - since) / 1e6, w->lr);
    }
}

int lv2_run_main(Guest& g) {
    host_timer_init();
    {   // The game's logic scheduler (0xd4848) clamps the image time to at least 1/60 s and steps the logic in 1/60 s
        // steps (constants of its TOC: 1/60 at 0xc6929c, 1/59.94 at 0xc692a8 for the other video mode). Above 60
        // images per second every image then advanced the game by 1/60 s: the countdown, menus and HUD animations ran
        // 2.4 times too fast at 144. With 1/rate the logic takes one step of the real image time per image.
        // Unchanged at 60 Hz. FRAMESTEP=0: off.
        Guest* gp = &g;
        const auto setf = [gp](uint32_t ea, float expect, float v) {
            Guest& g = *gp;
            uint32_t u; std::memcpy(&u, g.mem + ea, 4); u = __builtin_bswap32(u);
            float cur; std::memcpy(&cur, &u, 4);
            if (std::fabs(cur - expect) > 1e-6f) { LOG("[frame] constant at 0x%x is %g, not %g: logic step left alone\n", ea, cur, expect); return; }
            std::memcpy(&u, &v, 4); u = __builtin_bswap32(u); std::memcpy(g.mem + ea, &u, 4);
        };
        if (std::getenv("STEPLOG")) std::thread([gp] {   // STEPLOG=1: the scheduler's latched step, its flag, accumulator and mode every 5 s (debug)
            const auto r32 = [gp](uint32_t a) { uint32_t u; std::memcpy(&u, gp->mem + a, 4); return __builtin_bswap32(u); };
            const auto rf = [&](uint32_t a) { const uint32_t u = r32(a); float f; std::memcpy(&f, &u, 4); return double(f); };
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const uint32_t r25 = r32(0xc6f210 - 24616), r23 = r32(0xc6f210 - 24416), r22 = r32(0xc6f210 - 24636);
                const uint32_t g0 = r22 ? r32(r22) : 0, o144 = g0 ? r32(g0 + 144) : 0, o204 = g0 ? r32(g0 + 204) : 0;
                LOG("[steplog] flips %llu: step(+712) %g, latched %u, mode %u, image dt(+188) %g, const %g\n", (unsigned long long)gcm::flips.load(),
                    r25 ? rf(r25 + 712) : -1.0, r23 ? unsigned(gp->mem[r23]) : 99u, o144 ? r32(o144 + 536) : 99u, o204 ? rf(o204 + 188) : -1.0, rf(0xc6929c));
            }
        }).detach();
    }
    // main thread, started like lv2 starts a process: r3 argc, r4 argv, r5 envp, r7 thread id, r8-r10 TLS image
    auto t = std::make_shared<GThread>();
    t->id = g_nextTid++;
    t->name = "main";
    g_threads[t->id] = t;
    t_tid = t->id;
    ripprof_register_thread("main");
    PPU c;
    g.prepare(c);
    c.tid = t->id;
    c.r[1] = alloc_stack(0x100000) - 0x70;
    const uint32_t argv = alloc_heap(16), arg0 = alloc_heap(64);
    std::strcpy(reinterpret_cast<char*>(g.mem + arg0), "/app_home/EBOOT.BIN");
    c.wr32(argv, arg0); c.wr32(argv + 4, 0);
    c.r[3] = 1; c.r[4] = argv; c.r[5] = 0; c.r[6] = 0;
    c.r[7] = t->id;
    c.r[8] = 0xc8858c; c.r[9] = 4; c.r[10] = 0x1b8;   // PT_TLS of this EBOOT
    c.r[12] = 0x100000;
    try {
        rc_call(c, 0x10230);
        LOG("[lv2] main returned 0x%llx\n", (unsigned long long)c.r[3]);
    } catch (const ProcessExit& e) {
        LOG("[lv2] process exit %d\n", e.code);
        g_exitCode = e.code;
    } catch (const ThreadExit& e) {
        LOG("[lv2] main thread exit 0x%llx\n", (unsigned long long)e.value);
    }
    if (c.missing || c.unimplAt) LOG("[lv2] main stopped: missing call 0x%x, unimplemented instruction at 0x%x (lr 0x%llx ctr 0x%llx)\n", c.missing, c.unimplAt, (unsigned long long)c.lr, (unsigned long long)c.ctr);
    return g_exitCode;
}

} // namespace gti::recomp
