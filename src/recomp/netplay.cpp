// Online play without PSN (src/recomp/netplay.cpp): direct connection between players, no server.
//
// The game believes it is signed in to PSN and uses NP Matching2 (rooms) and P2P UDP sockets as on the PS3. Here:
//  - the player who creates a room hosts it inside his own game: room state, members, events (TCP, port 3658);
//  - searching asks the configured address (online_ip / NETIP) and the whole local network (UDP broadcast); each
//    host answers with its room, which the game sees as a search result (its filters are applied here);
//  - joining opens a TCP connection to the host, which sends the room and every change to all members;
//  - "signaling": every pair of players exchanges UDP pings; the connection is "established" only when the other
//    side answers, then the game gets the peer's address and sends its race traffic there (P2P sockets).
// Structures and event codes as documented by RPCS3 (rpcs3/Emu/Cell/Modules/sceNp2.h, GPLv2).
#include "recomp/ppu.h"
#include "recomp/settings.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gti::recomp::netplay {

struct Callback { uint32_t opd; int nargs; uint64_t a[8]; };

namespace {
inline void ret(PPU& c, uint64_t v) { c.r[3] = v; }
inline uint32_t a32(PPU& c, int i) { return uint32_t(c.r[3 + i]); }
std::recursive_mutex g_mx;
std::vector<Callback> g_pending;
std::atomic<bool> g_enabledFlag{false};
std::atomic<bool> g_inRoomFlag{false};   // in a room (online game): the image rate is held at 60 (lv2.cpp wanted_hz)

#define NLOG(...) std::fprintf(stderr, __VA_ARGS__)

uint64_t now_ms() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

// ------------------------------------------------------------------ settings
std::string g_name = "Jugador", g_hostIp;
uint16_t g_port = 3658;
std::vector<std::pair<uint32_t, uint16_t>> g_probe;   // extra addresses to ask when searching (local tests)

std::string clean_name(const std::string& in) {   // online ids: letters, digits, '-', '_' (16 at most)
    std::string o;
    for (unsigned char ch : in) if (o.size() < 16 && (std::isalnum(ch) || ch == '-' || ch == '_')) o += char(ch);
    return o;
}

void load_settings() {
    g_name = clean_name(settings().onlineName);
    if (g_name.empty()) {   // first time: "Piloto" and 4 random digits, kept (never the Windows user name: often a real name)
        std::mt19937 rng(std::random_device{}());
        g_name = "Piloto" + std::to_string(1000 + rng() % 9000);
        std::snprintf(settings().onlineName, sizeof settings().onlineName, "%s", g_name.c_str());
        settings_save();
    }
    if (const char* e = std::getenv("NETNAME"); e && *e) g_name = e;
    if (g_name.size() > 16) g_name.resize(16);
    if (const char* e = std::getenv("NETPORT")) g_port = uint16_t(std::atoi(e));
    if (const char* e = std::getenv("NETIP")) g_hostIp = e;
    if (const char* e = std::getenv("NETPROBE"))   // "ip:port,ip:port": also asked when searching
        for (std::string s = e; !s.empty();) {
            const size_t k = s.find(','); std::string one = s.substr(0, k); s = k == std::string::npos ? "" : s.substr(k + 1);
            const size_t c = one.find(':'); in_addr a{};
            if (c != std::string::npos && inet_pton(AF_INET, one.substr(0, c).c_str(), &a) == 1) g_probe.push_back({ntohl(a.s_addr), uint16_t(std::atoi(one.c_str() + c + 1))});
        }
}

// ------------------------------------------------------------------ byte buffers (big-endian, our wire format)
struct W {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void bytes(const std::vector<uint8_t>& v) { u32(uint32_t(v.size())); b.insert(b.end(), v.begin(), v.end()); }
    void str(const std::string& s) { bytes(std::vector<uint8_t>(s.begin(), s.end())); }
};
struct R {
    const uint8_t* p; size_t n, o = 0; bool bad = false;
    R(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
    uint32_t u8() { if (o + 1 > n) { bad = true; return 0; } return p[o++]; }
    uint32_t u16() { const uint32_t h = u8(); return h << 8 | u8(); }
    uint32_t u32() { const uint32_t h = u16(); return h << 16 | u16(); }
    uint64_t u64() { const uint64_t h = u32(); return h << 32 | u32(); }
    std::vector<uint8_t> bytes() { const uint32_t k = u32(); if (bad || o + k > n) { bad = true; return {}; } std::vector<uint8_t> v(p + o, p + o + k); o += k; return v; }
    std::string str() { auto v = bytes(); return std::string(v.begin(), v.end()); }
};

// ------------------------------------------------------------------ room state (kept by the host, copied to members)
struct Attr { uint16_t id; uint32_t num = 0; std::vector<uint8_t> data; };   // int attrs use num, binary attrs data
struct Member {
    uint16_t id = 0; std::string name; uint8_t team = 0, nat = 2; uint32_t flags = 0; uint64_t joinDate = 0;
    uint32_t ip = 0; uint16_t udpPort = 0; bool endpoint = false;   // UDP endpoint as the host sees it
    bool direct = false;   // (local, not sent) ip/udpPort is where this member's packets really reach us from
    std::vector<Attr> bin;                                          // room member binary internal attributes
};
struct Room {
    uint64_t id = 0; uint32_t maxSlot = 0, flags = 0; uint16_t owner = 1, nextMember = 1;
    std::vector<Attr> binInternal, searchInt, searchBin, binExternal;
    std::vector<Member> members;
};

void put_attrs(W& w, const std::vector<Attr>& v) { w.u32(uint32_t(v.size())); for (auto& a : v) { w.u16(a.id); w.u32(a.num); w.bytes(a.data); } }
std::vector<Attr> get_attrs(R& r) { std::vector<Attr> v(std::min<uint32_t>(r.u32(), 64)); for (auto& a : v) { a.id = uint16_t(r.u16()); a.num = r.u32(); a.data = r.bytes(); } return v; }
void put_member(W& w, const Member& m) { w.u16(m.id); w.str(m.name); w.u8(m.team); w.u8(m.nat); w.u32(m.flags); w.u64(m.joinDate); w.u32(m.ip); w.u16(m.udpPort); w.u8(m.endpoint); put_attrs(w, m.bin); }
Member get_member(R& r) { Member m; m.id = uint16_t(r.u16()); m.name = r.str(); m.team = uint8_t(r.u8()); m.nat = uint8_t(r.u8()); m.flags = r.u32(); m.joinDate = r.u64(); m.ip = r.u32(); m.udpPort = uint16_t(r.u16()); m.endpoint = r.u8(); m.bin = get_attrs(r); return m; }
void put_room(W& w, const Room& rm) {
    w.u64(rm.id); w.u32(rm.maxSlot); w.u32(rm.flags); w.u16(rm.owner); w.u16(rm.nextMember);
    put_attrs(w, rm.binInternal); put_attrs(w, rm.searchInt); put_attrs(w, rm.searchBin); put_attrs(w, rm.binExternal);
    w.u32(uint32_t(rm.members.size())); for (auto& m : rm.members) put_member(w, m);
}
Room get_room(R& r) {
    Room rm; rm.id = r.u64(); rm.maxSlot = r.u32(); rm.flags = r.u32(); rm.owner = uint16_t(r.u16()); rm.nextMember = uint16_t(r.u16());
    rm.binInternal = get_attrs(r); rm.searchInt = get_attrs(r); rm.searchBin = get_attrs(r); rm.binExternal = get_attrs(r);
    rm.members.resize(std::min<uint32_t>(r.u32(), 64)); for (auto& m : rm.members) m = get_member(r);
    return rm;
}

// ------------------------------------------------------------------ guest structures
std::vector<Attr> read_bin_attrs(PPU& c, uint32_t arr, uint32_t num) {   // SceNpMatching2BinAttr {u16 id; u8 pad[2]; u32 ptr; u32 size}
    std::vector<Attr> v;
    for (uint32_t i = 0; arr && i < std::min<uint32_t>(num, 64); ++i) {
        const uint32_t e = arr + i * 12; Attr a; a.id = uint16_t(c.rd32(e) >> 16);
        const uint32_t p = c.rd32(e + 4), sz = std::min<uint32_t>(c.rd32(e + 8), 1024);
        if (p) a.data.assign(c.mem + p, c.mem + p + sz);
        v.push_back(a);
    }
    return v;
}
std::vector<Attr> read_int_attrs(PPU& c, uint32_t arr, uint32_t num) {   // SceNpMatching2IntAttr {u16 id; u8 pad[2]; u32 num}
    std::vector<Attr> v;
    for (uint32_t i = 0; arr && i < std::min<uint32_t>(num, 64); ++i) { Attr a; a.id = uint16_t(c.rd32(arr + i * 8) >> 16); a.num = c.rd32(arr + i * 8 + 4); v.push_back(a); }
    return v;
}
void merge_attrs(std::vector<Attr>& dst, const std::vector<Attr>& src) {
    for (auto& a : src) { auto it = std::find_if(dst.begin(), dst.end(), [&](const Attr& x) { return x.id == a.id; }); if (it != dst.end()) *it = a; else dst.push_back(a); }
}

// Event data: built in a host buffer with pointer fields relative to its start, relocated when the game copies it.
struct Blob {
    std::vector<uint8_t> d; std::vector<uint32_t> rel;
    uint32_t alloc(uint32_t size, uint32_t align = 8) { uint32_t o = uint32_t((d.size() + align - 1) & ~size_t(align - 1)); d.resize(o + size); return o; }
    void w8(uint32_t o, uint32_t v) { d[o] = uint8_t(v); }
    void w16(uint32_t o, uint32_t v) { d[o] = uint8_t(v >> 8); d[o + 1] = uint8_t(v); }
    void w32(uint32_t o, uint32_t v) { w16(o, v >> 16); w16(o + 2, v); }
    void w64(uint32_t o, uint64_t v) { w32(o, uint32_t(v >> 32)); w32(o + 4, uint32_t(v)); }
    void ptr(uint32_t o, uint32_t target) { w32(o, target); rel.push_back(o); }
    uint32_t bin_attrs(const std::vector<Attr>& v) {   // array of SceNpMatching2BinAttr + data
        if (v.empty()) return 0;
        const uint32_t arr = alloc(uint32_t(12 * v.size()), 4);
        for (size_t i = 0; i < v.size(); ++i) {
            w16(arr + uint32_t(i) * 12, v[i].id); w32(arr + uint32_t(i) * 12 + 8, uint32_t(v[i].data.size()));
            if (!v[i].data.empty()) { const uint32_t p = alloc(uint32_t(v[i].data.size()), 4); std::memcpy(d.data() + p, v[i].data.data(), v[i].data.size()); ptr(arr + uint32_t(i) * 12 + 4, p); }
        }
        return arr;
    }
    void bin_attr_at(uint32_t e, const Attr& a) {   // SceNpMatching2BinAttr {u16 id; u8 pad[2]; u32 ptr; u32 size} at e
        w16(e, a.id); w32(e + 8, uint32_t(a.data.size()));
        if (!a.data.empty()) { const uint32_t p = alloc(uint32_t(a.data.size()), 4); std::memcpy(d.data() + p, a.data.data(), a.data.size()); ptr(e + 4, p); }
    }
    // SceNpMatching2RoomBinAttrInternal {CellRtcTick updateDate; u16 updateMemberId; u8 pad[2]; BinAttr data} = 24
    uint32_t room_bin_internal(const std::vector<Attr>& v, uint16_t owner) {
        if (v.empty()) return 0;
        const uint32_t arr = alloc(uint32_t(24 * v.size()), 8);
        for (size_t i = 0; i < v.size(); ++i) { const uint32_t e = arr + uint32_t(i) * 24; w64(e, 0); w16(e + 8, owner); bin_attr_at(e + 12, v[i]); }
        return arr;
    }
    // SceNpMatching2RoomMemberBinAttrInternal {CellRtcTick updateDate; BinAttr data} = 20, 24 with alignment
    uint32_t member_bin_internal(const std::vector<Attr>& v) {
        if (v.empty()) return 0;
        const uint32_t arr = alloc(uint32_t(24 * v.size()), 8);
        for (size_t i = 0; i < v.size(); ++i) { const uint32_t e = arr + uint32_t(i) * 24; w64(e, 0); bin_attr_at(e + 8, v[i]); }
        return arr;
    }
    uint32_t int_attrs(const std::vector<Attr>& v) {
        if (v.empty()) return 0;
        const uint32_t arr = alloc(uint32_t(8 * v.size()), 4);
        for (size_t i = 0; i < v.size(); ++i) { w16(arr + uint32_t(i) * 8, v[i].id); w32(arr + uint32_t(i) * 8 + 4, v[i].num); }
        return arr;
    }
    void npid(uint32_t o, const std::string& name) { std::memset(d.data() + o, 0, 36); std::memcpy(d.data() + o, name.data(), std::min<size_t>(name.size(), 16)); }
    void user_info(uint32_t o, const std::string& name) {   // SceNpUserInfo2 {SceNpId npId(36); u32 onlineName*; u32 avatarUrl*} = 44
        npid(o, name);
        const uint32_t on = alloc(52, 4); std::memcpy(d.data() + on, name.data(), std::min<size_t>(name.size(), 48)); ptr(o + 36, on);
    }
    uint32_t member(const Member& m) {   // SceNpMatching2RoomMemberDataInternal (80 bytes)
        const uint32_t o = alloc(80, 8);
        user_info(o + 4, m.name);
        w64(o + 48, m.joinDate); w16(o + 56, m.id); w8(o + 58, m.team); w8(o + 64, m.nat); w32(o + 68, m.flags);
        if (const uint32_t a = member_bin_internal(m.bin)) { ptr(o + 72, a); w32(o + 76, uint32_t(m.bin.size())); }
        return o;
    }
    uint32_t room_internal(const Room& rm, uint16_t me) {   // SceNpMatching2RoomDataInternal (72 bytes)
        const uint32_t o = alloc(72, 8);
        w16(o, 1); w32(o + 4, 1); w64(o + 8, 0); w64(o + 16, rm.id); w64(o + 24, 0); w32(o + 32, rm.maxSlot);
        uint32_t prev = 0, first = 0;
        for (auto& m : rm.members) {
            const uint32_t mo = member(m);
            if (prev) ptr(prev, mo); else first = mo;
            prev = mo;
            if (m.id == me) ptr(o + 44, mo);
            if (m.id == rm.owner) ptr(o + 48, mo);
        }
        if (first) ptr(o + 36, first);
        w32(o + 40, uint32_t(rm.members.size()));
        w32(o + 60, rm.flags);
        if (const uint32_t a = room_bin_internal(rm.binInternal, rm.owner)) { ptr(o + 64, a); w32(o + 68, uint32_t(rm.binInternal.size())); }
        return o;
    }
    uint32_t room_external(const Room& rm) {   // SceNpMatching2RoomDataExternal (88 bytes)
        const uint32_t o = alloc(88, 8);
        w16(o + 4, 1); w32(o + 8, 1); w16(o + 12, uint16_t(rm.maxSlot)); w16(o + 14, 0); w64(o + 16, 0); w64(o + 24, rm.id);
        const uint32_t cur = uint32_t(rm.members.size());
        w16(o + 32, rm.maxSlot > cur ? rm.maxSlot - cur : 0); w16(o + 34, uint16_t(rm.maxSlot)); w16(o + 36, 0); w16(o + 38, cur);
        auto own = std::find_if(rm.members.begin(), rm.members.end(), [&](const Member& m) { return m.id == rm.owner; });
        if (own != rm.members.end()) { const uint32_t u = alloc(44, 4); user_info(u, own->name); ptr(o + 48, u); }
        w32(o + 60, rm.flags | (cur >= rm.maxSlot ? 0x20000000u : 0));
        if (const uint32_t a = int_attrs(rm.searchInt)) { ptr(o + 64, a); w32(o + 68, uint32_t(rm.searchInt.size())); }
        if (const uint32_t a = bin_attrs(rm.searchBin)) { ptr(o + 72, a); w32(o + 76, uint32_t(rm.searchBin.size())); }
        if (const uint32_t a = bin_attrs(rm.binExternal)) { ptr(o + 80, a); w32(o + 84, uint32_t(rm.binExternal.size())); }
        return o;
    }
};

// ------------------------------------------------------------------ game-side state
struct Ctx {
    bool started = false;
    uint32_t ctxCb = 0, ctxArg = 0, roomCb = 0, roomArg = 0, sigCb = 0, sigArg = 0, msgCb = 0, msgArg = 0;
} g_ctx;
std::map<uint32_t, Blob> g_events;   // eventKey -> data
uint32_t g_nextKey = 1, g_nextReq = 1;
struct Req { uint32_t cb = 0, arg = 0; uint16_t event = 0; };
std::map<uint32_t, Req> g_reqs;

void queue(uint32_t opd, std::initializer_list<uint64_t> args) {
    if (!opd) return;
    Callback cb{}; cb.opd = opd; cb.nargs = int(args.size()); int i = 0; for (uint64_t a : args) cb.a[i++] = a;
    std::lock_guard<std::recursive_mutex> lk(g_mx); g_pending.push_back(cb);
}
uint32_t new_request(PPU& c, uint32_t opt, uint16_t event) {
    Req r; r.event = event;
    if (opt) { r.cb = c.rd32(opt); r.arg = c.rd32(opt + 4); }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const uint32_t id = g_nextReq++;
    g_reqs[id] = r;
    return id;
}
void complete(uint32_t reqId, uint32_t error, Blob* data) {   // request callback: (ctxId, reqId, event, eventKey, errorCode, dataSize, arg)
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_reqs.find(reqId);
    if (it == g_reqs.end()) return;
    const Req r = it->second; g_reqs.erase(it);
    uint32_t key = 0, size = 0;
    if (data && !error) { key = g_nextKey++; size = uint32_t(data->d.size()); g_events[key] = std::move(*data); }
    NLOG("[net] request %u (event %04x) done: error %08x, %u bytes\n", reqId, r.event, error, size);
    queue(r.cb, {1, reqId, r.event, key, error, size, r.arg});
}
void room_event(uint64_t roomId, uint16_t event, uint32_t error, Blob* data) {   // (ctxId, roomId, event, eventKey, errorCode, dataSize, arg)
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    uint32_t key = 0, size = 0;
    if (data) { key = g_nextKey++; size = uint32_t(data->d.size()); g_events[key] = std::move(*data); }
    NLOG("[net] room event %04x (%u bytes)\n", event, size);
    queue(g_ctx.roomCb, {1, roomId, event, key, error, size, g_ctx.roomArg});
}
void signaling_event(uint64_t roomId, uint16_t member, uint16_t event) {   // (ctxId, roomId, peerMemberId, event, errorCode, arg)
    NLOG("[net] signaling %s with member %u\n", event == 0x5102 ? "established" : "dead", member);
    queue(g_ctx.sigCb, {1, roomId, member, event, 0, g_ctx.sigArg});
}

// ------------------------------------------------------------------ network: the room as this game sees it
std::shared_ptr<Room> g_room;   // the room this player is in (host: the authoritative copy)
uint16_t g_me = 0;
bool g_host = false;
SOCKET g_listen = INVALID_SOCKET, g_toHost = INVALID_SOCKET;
struct Peer { SOCKET s; uint16_t member; };
std::vector<Peer> g_peers;   // host: one TCP connection per member
struct SigState { uint64_t lastRx = 0, lastTx = 0; bool up = false; };
std::map<uint16_t, SigState> g_sig;
std::map<uint64_t, std::pair<uint32_t, uint16_t>> g_found;   // roomId -> host address (last search)

enum : uint8_t { M_JOIN = 1, M_JOIN_OK = 2, M_JOIN_FAIL = 3, M_ROOM = 4, M_MEMBER_JOINED = 5, M_MEMBER_LEFT = 6, M_DESTROYED = 7,
                 M_LEAVE = 8, M_KICKED = 9 };
enum : uint32_t { E_ROOM_FULL = 0x80022b19, E_NO_SUCH_ROOM = 0x80022b13, E_TIMEOUT = 0x8002231d };

bool send_msg(SOCKET s, uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f(5 + body.size());
    const uint32_t n = uint32_t(body.size() + 1);
    f[0] = uint8_t(n >> 24); f[1] = uint8_t(n >> 16); f[2] = uint8_t(n >> 8); f[3] = uint8_t(n); f[4] = type;
    std::memcpy(f.data() + 5, body.data(), body.size());
    for (size_t o = 0; o < f.size();) { const int k = ::send(s, reinterpret_cast<const char*>(f.data() + o), int(f.size() - o), 0); if (k <= 0) return false; o += size_t(k); }
    return true;
}
bool recv_all(SOCKET s, uint8_t* p, size_t n) { for (size_t o = 0; o < n;) { const int k = ::recv(s, reinterpret_cast<char*>(p + o), int(n - o), 0); if (k <= 0) return false; o += size_t(k); } return true; }
bool recv_msg(SOCKET s, uint8_t& type, std::vector<uint8_t>& body) {
    uint8_t h[5]; if (!recv_all(s, h, 5)) return false;
    const uint32_t n = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
    if (n == 0 || n > (1u << 20)) return false;
    type = h[4]; body.resize(n - 1);
    return n == 1 || recv_all(s, body.data(), n - 1);
}

void broadcast(uint8_t type, const std::vector<uint8_t>& body, uint16_t except = 0) {   // host -> members (g_mx held)
    for (auto& p : g_peers) if (p.member != except) send_msg(p.s, type, body);
}

// --- local UDP endpoint (P2P sockets and signaling share it)
SOCKET g_udp = INVALID_SOCKET;
std::atomic<uint64_t> g_gameTx{0}, g_gameRx{0}, g_gameRxDrop{0};
std::string g_lastTx;
void udp_send(uint32_t ip, uint16_t port, uint16_t dstVport, uint16_t srcVport, const uint8_t* data, size_t n) {
    std::vector<uint8_t> pkt(4 + n);
    pkt[0] = uint8_t(dstVport >> 8); pkt[1] = uint8_t(dstVport); pkt[2] = uint8_t(srcVport >> 8); pkt[3] = uint8_t(srcVport);
    if (n) std::memcpy(pkt.data() + 4, data, n);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    ::sendto(g_udp, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
}

// Control packets on vport 0 (never given to the game): 'Q' search query, 'A' search answer (room external),
// 'H' member hello to the host (its UDP endpoint), 'P' ping, 'O' pong.
void send_ctl(uint32_t ip, uint16_t port, uint8_t kind, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> p(1 + body.size()); p[0] = kind; std::memcpy(p.data() + 1, body.data(), body.size());
    udp_send(ip, port, 0, 0, p.data(), p.size());
}
std::vector<uint8_t> ping_body(uint8_t kind) { W w; w.u64(g_room ? g_room->id : 0); w.u16(g_me); (void)kind; return w.b; }

void member_endpoint_changed();   // (below)

struct SearchResult { Room room; uint32_t ip; uint16_t port; };
std::mutex g_searchMx; std::vector<SearchResult> g_searchResults; std::atomic<bool> g_searching{false};

void on_ctl(uint32_t ip, uint16_t port, const std::vector<uint8_t>& pkt) {
    if (pkt.empty()) return;
    std::vector<uint8_t> body(pkt.begin() + 1, pkt.end());
    R r(body);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    switch (pkt[0]) {
    case 'Q':   // someone is searching: answer with our room if we host one
        if (g_host && g_room) { W w; put_room(w, *g_room); send_ctl(ip, port, 'A', w.b); }
        break;
    case 'A':
        if (g_searching) { Room rm = get_room(r); if (!r.bad) { std::lock_guard<std::mutex> l2(g_searchMx); g_searchResults.push_back({rm, ip, port}); } }
        break;
    case 'H': {   // a member tells the host where its UDP traffic comes from
        const uint64_t room = r.u64(); const uint16_t mid = uint16_t(r.u16());
        if (!g_host || !g_room || room != g_room->id) break;
        for (auto& m : g_room->members)
            if (m.id == mid && (!m.endpoint || m.ip != ip || m.udpPort != port)) { m.ip = ip; m.udpPort = port; m.endpoint = true; member_endpoint_changed(); }
        send_ctl(ip, port, 'O', ping_body('O'));
        break;
    }
    case 'P': case 'O': {
        const uint64_t room = r.u64(); const uint16_t mid = uint16_t(r.u16());
        if (!g_room || room != g_room->id || mid == g_me) break;
        if (pkt[0] == 'P') send_ctl(ip, port, 'O', ping_body('O'));
        // the peer's address is the one its packets really come from (local network interface, NAT mapping):
        // the game checks the source of its packets against what SignalingGetConnectionStatus said
        for (auto& m : g_room->members)
            if (m.id == mid) {
                const bool changed = m.ip != ip || m.udpPort != port || !m.endpoint;
                m.ip = ip; m.udpPort = port; m.endpoint = true; m.direct = true;
                if (changed && g_host) member_endpoint_changed();
            }
        auto& s = g_sig[mid]; s.lastRx = now_ms();
        if (!s.up) { s.up = true; signaling_event(g_room->id, mid, 0x5102); }
        break;
    }
    }
}

// --- virtual addresses: the game sees every other player at 10.77.0.<member id>, port 3658. The game drops
// packets whose source is its own IP (two games on one PC, players behind the same router share it) and keeps the
// address SignalingGetConnectionStatus gave; real endpoints (LAN interface, NAT mapping) change underneath.
constexpr uint32_t kVirtNet = 0x0A4D0000;   // 10.77.0.0
uint32_t virt_ip(uint16_t member) { return kVirtNet | member; }
bool from_member(uint32_t ip, uint16_t port, uint16_t& member) {   // (g_mx held)
    if (!g_room) return false;
    for (auto& m : g_room->members) if (m.id != g_me && m.endpoint && m.ip == ip && m.udpPort == port) { member = m.id; return true; }
    return false;
}
bool to_member(uint32_t vip, uint32_t& ip, uint16_t& port) {   // (g_mx held)
    if ((vip & 0xFFFF0000u) != kVirtNet || !g_room) return false;
    for (auto& m : g_room->members) if (m.id == uint16_t(vip & 0xFFFF)) { ip = m.ip; port = m.udpPort; return m.endpoint; }
    return false;
}

// --- P2P sockets for the game
uint32_t g_errnoAddr = 0;
void set_errno(PPU& c, uint32_t e) { if (g_errnoAddr) c.wr32(g_errnoAddr, e); }
constexpr uint32_t EWOULDBLOCK_ = 35, EBADF_ = 9, EINVAL_ = 22;
struct Datagram { uint32_t ip; uint16_t port, vport; std::vector<uint8_t> data; };
struct Sock { int type = 0; uint16_t vport = 0; bool bound = false, nbio = false; std::deque<Datagram> q; };
std::map<int, Sock> g_socks;
std::condition_variable_any g_rxCv;

bool open_udp() {
    if (g_udp != INVALID_SOCKET) return true;
    g_udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) return false;
    BOOL yes = TRUE; ::setsockopt(g_udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(g_port); a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(g_udp, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        NLOG("[net] UDP port %u busy (another game open?): %d\n", g_port, WSAGetLastError());
        closesocket(g_udp); g_udp = INVALID_SOCKET; return false;
    }
    NLOG("[net] UDP port %u open (P2P and room search)\n", g_port);
    std::thread([] {
        std::vector<uint8_t> buf(65536);
        for (;;) {
            sockaddr_in from{}; int fl = sizeof from;
            const int n = ::recvfrom(g_udp, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < 4) { if (n == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK) return; continue; }
            const uint32_t ip = ntohl(from.sin_addr.s_addr); const uint16_t port = ntohs(from.sin_port);
            const uint16_t dst = uint16_t(buf[0] << 8 | buf[1]), src = uint16_t(buf[2] << 8 | buf[3]);
            if (dst == 0) { on_ctl(ip, port, std::vector<uint8_t>(buf.begin() + 4, buf.begin() + n)); continue; }
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            for (auto& [mid, s] : g_sig)   // the game's traffic also proves the connection alive
                if (g_room) for (auto& m : g_room->members) if (m.id == mid && m.ip == ip && m.udpPort == port) s.lastRx = now_ms();
            bool taken = false;
            for (auto& [fd, s] : g_socks)
                if (s.bound && s.vport == dst) { if (s.q.size() < 512) s.q.push_back({ip, port, src, std::vector<uint8_t>(buf.begin() + 4, buf.begin() + n)}); taken = true; break; }
            if (taken && std::getenv("NETDUMP") && g_gameRx < 80) { std::string h; for (int i = 4; i < std::min(n, 28); ++i) { char t[4]; std::snprintf(t, sizeof t, "%02x", buf[i]); h += t; } NLOG("[netdump] %llu RX %4d %s\n", (unsigned long long)now_ms() % 1000000, n - 4, h.c_str()); }
            if (taken) { if (g_gameRx++ < 5) NLOG("[net] game packet %d bytes from port %u vport %u\n", n - 4, port, src); }
            else if (g_gameRxDrop++ < 5) NLOG("[net] packet for vport %u dropped (no socket)\n", dst);
            g_rxCv.notify_all();
        }
    }).detach();
    return true;
}

void read_addr(PPU& c, uint32_t a, uint32_t& ip, uint16_t& port, uint16_t& vport) {   // sockaddr_in(_p2p), big-endian
    port = uint16_t(c.mem[a + 2] << 8 | c.mem[a + 3]); ip = c.rd32(a + 4); vport = uint16_t(c.mem[a + 8] << 8 | c.mem[a + 9]);
}
void write_addr(PPU& c, uint32_t a, uint32_t ip, uint16_t port, uint16_t vport) {
    std::memset(c.mem + a, 0, 16);
    c.mem[a] = 16; c.mem[a + 1] = 2; c.mem[a + 2] = uint8_t(port >> 8); c.mem[a + 3] = uint8_t(port); c.wr32(a + 4, ip);
    c.mem[a + 8] = uint8_t(vport >> 8); c.mem[a + 9] = uint8_t(vport);
}

bool sockets(PPU& c, const std::string& n) {
    if (n == "socket") {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (!open_udp()) { set_errno(c, EINVAL_); ret(c, uint64_t(-1)); return true; }
        int fd = 1; while (g_socks.count(fd)) ++fd;
        g_socks[fd].type = int(a32(c, 1));
        ret(c, uint64_t(fd)); return true;
    }
    if (n == "bind") {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto it = g_socks.find(int(a32(c, 0)));
        if (it == g_socks.end()) { set_errno(c, EBADF_); ret(c, uint64_t(-1)); return true; }
        uint32_t ip; uint16_t port, vp; read_addr(c, a32(c, 1), ip, port, vp);
        it->second.vport = it->second.type == 6 ? vp : port; it->second.bound = true;
        if (it->second.vport == 0) it->second.vport = 0xffff;   // vport 0 carries our control packets
        ret(c, 0); return true;
    }
    if (n == "setsockopt") {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto it = g_socks.find(int(a32(c, 0)));
        if (it != g_socks.end() && a32(c, 1) == 0xffff && a32(c, 2) == 0x1100 && a32(c, 3)) it->second.nbio = c.rd32(a32(c, 3)) != 0;   // SO_NBIO
        ret(c, 0); return true;
    }
    if (n == "sendto") {
        const int fd = int(a32(c, 0)); const uint32_t buf = a32(c, 1), len = a32(c, 2), to = a32(c, 4);
        uint16_t src = 0;
        { std::lock_guard<std::recursive_mutex> lk(g_mx); auto it = g_socks.find(fd); if (it == g_socks.end()) { set_errno(c, EBADF_); ret(c, uint64_t(-1)); return true; } src = it->second.vport; }
        if (!to || len > 65000) { set_errno(c, EINVAL_); ret(c, uint64_t(-1)); return true; }
        uint32_t ip; uint16_t port, vp; read_addr(c, to, ip, port, vp);
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            uint32_t rip; uint16_t rport;
            if (to_member(ip, rip, rport)) { ip = rip; port = rport; }
            else if ((ip & 0xFFFF0000u) == kVirtNet) { ret(c, len); return true; }   // player not reachable yet: dropped like a lost packet
        }
        udp_send(ip, port ? port : 3658, vp, src, c.mem + buf, len);
        if (std::getenv("NETDUMP") && g_gameTx < 80) { std::string h; for (uint32_t i = 0; i < std::min<uint32_t>(len, 24); ++i) { char t[4]; std::snprintf(t, sizeof t, "%02x", c.mem[buf + i]); h += t; } NLOG("[netdump] %llu TX %4u %s\n", (unsigned long long)now_ms() % 1000000, len, h.c_str()); }
        if (g_gameTx++ < 5) NLOG("[net] game packet %u bytes to %u.%u.%u.%u:%u vport %u (from vport %u)\n", len, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port, vp, src);
        ret(c, len); return true;
    }
    if (n == "recvfrom" || n == "recv") {
        const int fd = int(a32(c, 0)); const uint32_t buf = a32(c, 1), len = a32(c, 2), flags = a32(c, 3);
        const uint32_t from = n == "recvfrom" ? a32(c, 4) : 0, fromLen = n == "recvfrom" ? a32(c, 5) : 0;
        std::unique_lock<std::recursive_mutex> lk(g_mx);
        auto it = g_socks.find(fd);
        if (it == g_socks.end()) { set_errno(c, EBADF_); ret(c, uint64_t(-1)); return true; }
        if (it->second.q.empty()) {
            if (it->second.nbio || (flags & 0x80)) { set_errno(c, EWOULDBLOCK_); ret(c, uint64_t(-1)); return true; }
            g_rxCv.wait_for(lk, std::chrono::milliseconds(50));
            it = g_socks.find(fd);
            if (it == g_socks.end() || it->second.q.empty()) { set_errno(c, EWOULDBLOCK_); ret(c, uint64_t(-1)); return true; }
        }
        Datagram d = std::move(it->second.q.front()); it->second.q.pop_front();
        { static int k = 0; if (std::getenv("NETDUMP") && k++ < 40) NLOG("[netdump] %llu game read %zu bytes (fd %d, buf %u, flags %x, from %08x len* %08x) lr %08x\n", (unsigned long long)now_ms() % 1000000, d.data.size(), fd, len, flags, from, fromLen, uint32_t(c.lr)); }
        const uint32_t k = std::min<uint32_t>(len, uint32_t(d.data.size()));
        std::memcpy(c.mem + buf, d.data.data(), k);
        uint16_t mid = 0;
        if (from) { if (from_member(d.ip, d.port, mid)) write_addr(c, from, virt_ip(mid), 3658, d.vport); else { static int f = 0; if (f++ < 5) NLOG("[net] game packet from an unknown address %u.%u.%u.%u:%u (not a member endpoint)\n", d.ip >> 24, (d.ip >> 16) & 255, (d.ip >> 8) & 255, d.ip & 255, d.port); write_addr(c, from, d.ip, d.port, d.vport); } }
        if (fromLen) c.wr32(fromLen, 16);
        ret(c, k); return true;
    }
    if (n == "socketclose") { std::lock_guard<std::recursive_mutex> lk(g_mx); g_socks.erase(int(a32(c, 0))); ret(c, 0); return true; }
    if (n == "inet_addr") {
        in_addr a{}; ret(c, inet_pton(AF_INET, reinterpret_cast<const char*>(c.mem + a32(c, 0)), &a) == 1 ? uint64_t(ntohl(a.s_addr)) : uint64_t(0xffffffffu)); return true;
    }
    return false;
}

// ------------------------------------------------------------------ room events seen by this game
Blob member_update(const Member& m, uint8_t cause) {   // SceNpMatching2RoomMemberUpdateInfo {u32 member*; u8 cause; pad[3]; optData(20)}
    Blob b; const uint32_t o = b.alloc(28, 8); b.w8(o + 4, cause); b.ptr(o, b.member(m)); return b;
}
Blob room_update(uint8_t cause, uint32_t error) {   // SceNpMatching2RoomUpdateInfo {u8 cause; pad[3]; s32 errorCode; optData(20)}
    Blob b; const uint32_t o = b.alloc(28, 8); b.w8(o, cause); b.w32(o + 4, error); return b;
}
Blob room_internal_update(const Room& rm) {   // SceNpMatching2RoomDataInternalUpdateInfo (36 bytes) + new data
    Blob b; const uint32_t o = b.alloc(40, 8);
    b.ptr(o, b.room_internal(rm, g_me));
    const uint32_t fa = b.alloc(4, 4); b.w32(fa, rm.flags); b.ptr(o + 4, fa); b.ptr(o + 8, fa);
    if (!rm.binInternal.empty()) {
        const uint32_t arr = b.room_bin_internal(rm.binInternal, rm.owner);
        const uint32_t pp = b.alloc(uint32_t(4 * rm.binInternal.size()), 4);
        for (size_t i = 0; i < rm.binInternal.size(); ++i) b.ptr(pp + uint32_t(i) * 4, arr + uint32_t(i) * 24);
        b.ptr(o + 28, pp); b.w32(o + 32, uint32_t(rm.binInternal.size()));
    }
    return b;
}

void start_signaling() {   // (g_mx held) every other member with a known endpoint
    if (!g_room) return;
    for (auto& m : g_room->members) if (m.id != g_me && !g_sig.count(m.id)) g_sig[m.id] = SigState{};
}

void leave_room_local(bool destroyed, uint8_t cause) {   // (g_mx held)
    if (!g_room) return;
    const uint64_t id = g_room->id;
    for (auto& [mid, s] : g_sig) if (s.up) signaling_event(id, mid, 0x5101);
    g_sig.clear();
    if (destroyed) { Blob b = room_update(cause, 0); room_event(id, cause == 2 ? 0x1103 : 0x1104, 0, &b); }
    g_room.reset(); g_me = 0; g_inRoomFlag = false;
    if (g_toHost != INVALID_SOCKET) { closesocket(g_toHost); g_toHost = INVALID_SOCKET; }
}

// host: a member's UDP endpoint became known -> everyone gets the room again (addresses for signaling)
void member_endpoint_changed() {
    W w; put_room(w, *g_room); broadcast(M_ROOM, w.b);
    start_signaling();
}

void host_member_gone(uint16_t mid, uint8_t cause) {   // (g_mx held)
    if (!g_room) return;
    auto it = std::find_if(g_room->members.begin(), g_room->members.end(), [&](const Member& m) { return m.id == mid; });
    if (it == g_room->members.end()) return;
    const Member m = *it; g_room->members.erase(it);
    for (auto p = g_peers.begin(); p != g_peers.end();) if (p->member == mid) { closesocket(p->s); p = g_peers.erase(p); } else ++p;
    W w; w.u16(mid); w.u8(cause); broadcast(M_MEMBER_LEFT, w.b);
    if (g_sig.count(mid) && g_sig[mid].up) signaling_event(g_room->id, mid, 0x5101);
    g_sig.erase(mid);
    Blob b = member_update(m, cause); room_event(g_room->id, 0x1102, 0, &b);
    NLOG("[net] %s left the room\n", m.name.c_str());
}

void host_serve(SOCKET s, uint32_t ip) {   // one joining player
    uint8_t type; std::vector<uint8_t> body;
    uint16_t mid = 0;
    while (recv_msg(s, type, body)) {
        R r(body);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (type == M_JOIN) {
            if (!g_host || !g_room || r.u64() != g_room->id) { W w; w.u32(E_NO_SUCH_ROOM); send_msg(s, M_JOIN_FAIL, w.b); break; }
            if (g_room->members.size() >= g_room->maxSlot) { W w; w.u32(E_ROOM_FULL); send_msg(s, M_JOIN_FAIL, w.b); break; }
            Member m; m.id = ++g_room->nextMember; m.name = r.str(); m.team = uint8_t(r.u8()); m.udpPort = uint16_t(r.u16()); m.bin = get_attrs(r);
            m.ip = ip; m.joinDate = uint64_t(std::time(nullptr)) * 1000000ull;
            for (auto& o : g_room->members) if (o.name == m.name) m.name += "_" + std::to_string(m.id);   // names identify players: unique
            mid = m.id;
            g_room->members.push_back(m);
            g_peers.push_back({s, mid});
            W w; put_room(w, *g_room); w.u16(mid); send_msg(s, M_JOIN_OK, w.b);
            W j; put_member(j, m); broadcast(M_MEMBER_JOINED, j.b, mid);
            Blob b = member_update(m, 0); room_event(g_room->id, 0x1101, 0, &b);
            start_signaling();
            NLOG("[net] %s joined the room (member %u)\n", m.name.c_str(), mid);
        } else if (type == M_LEAVE) break;
    }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (mid) host_member_gone(mid, 1);
    else closesocket(s);
}

void upnp_open();   // (below)
bool host_start() {   // (g_mx held) the TCP port members connect to
    if (g_listen != INVALID_SOCKET) return true;
    g_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(g_port); a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(g_listen, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(g_listen, 8) != 0) {
        NLOG("[net] TCP port %u busy: %d\n", g_port, WSAGetLastError()); closesocket(g_listen); g_listen = INVALID_SOCKET; return false;
    }
    std::thread([] {
        for (;;) {
            sockaddr_in from{}; int fl = sizeof from;
            SOCKET s = ::accept(g_listen, reinterpret_cast<sockaddr*>(&from), &fl);
            if (s == INVALID_SOCKET) return;
            BOOL nd = TRUE; ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
            std::thread(host_serve, s, ntohl(from.sin_addr.s_addr)).detach();
        }
    }).detach();
    NLOG("[net] hosting on TCP port %u\n", g_port);
    upnp_open();
    return true;
}

void client_loop(SOCKET s) {   // member: messages from the host
    uint8_t type; std::vector<uint8_t> body;
    while (recv_msg(s, type, body)) {
        R r(body);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (!g_room || g_toHost != s) break;
        if (type == M_ROOM) {
            Room rm = get_room(r); if (r.bad) continue;
            const bool attrsChanged = rm.binInternal.size() != g_room->binInternal.size() || rm.flags != g_room->flags ||
                !std::equal(rm.binInternal.begin(), rm.binInternal.end(), g_room->binInternal.begin(), [](const Attr& a, const Attr& b) { return a.id == b.id && a.data == b.data; });
            // the host's copy has the addresses the HOST sees (its own entry = its LAN address, unreachable over
            // the internet); an address we already reach a member at stays: overwriting it sent our race packets
            // to the host's LAN IP and made its packets look foreign (static ghost cars online)
            for (auto& m : rm.members)
                for (const auto& o : g_room->members)
                    if (o.id == m.id && o.direct) { m.ip = o.ip; m.udpPort = o.udpPort; m.endpoint = true; m.direct = true; }
            *g_room = rm;
            start_signaling();
            if (attrsChanged) { Blob b = room_internal_update(*g_room); room_event(g_room->id, 0x1106, 0, &b); }
        } else if (type == M_MEMBER_JOINED) {
            Member m = get_member(r); if (r.bad) continue;
            g_room->members.push_back(m); start_signaling();
            Blob b = member_update(m, 0); room_event(g_room->id, 0x1101, 0, &b);
        } else if (type == M_MEMBER_LEFT) {
            const uint16_t mid = uint16_t(r.u16()); const uint8_t cause = uint8_t(r.u8());
            auto it = std::find_if(g_room->members.begin(), g_room->members.end(), [&](const Member& m) { return m.id == mid; });
            if (it == g_room->members.end()) continue;
            const Member m = *it; g_room->members.erase(it);
            if (g_sig.count(mid) && g_sig[mid].up) signaling_event(g_room->id, mid, 0x5101);
            g_sig.erase(mid);
            Blob b = member_update(m, cause); room_event(g_room->id, 0x1102, 0, &b);
        } else if (type == M_DESTROYED) { leave_room_local(true, 4); return; }
        else if (type == M_KICKED) { leave_room_local(true, 2); return; }
    }
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_room && g_toHost == s) { NLOG("[net] lost the connection to the host\n"); leave_room_local(true, 7); }
}

// signaling / keep-alive: every 250 ms (pings until the peer answers, then a keep-alive every 2 s)
void sig_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        {   // every 5 s: game traffic counters (when they change)
            static uint64_t lastT = 0, lastSum = 0;
            const uint64_t sum = g_gameTx + g_gameRx + g_gameRxDrop;
            if (now_ms() - lastT > 5000 && sum != lastSum) { NLOG("[net] game packets: %llu sent, %llu received, %llu dropped\n", (unsigned long long)g_gameTx.load(), (unsigned long long)g_gameRx.load(), (unsigned long long)g_gameRxDrop.load()); lastT = now_ms(); lastSum = sum; }
        }
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (!g_room) continue;
        const uint64_t t = now_ms();
        if (!g_host) {   // tell the host where our UDP traffic comes from (NAT mapping), until it answers
            auto host = std::find_if(g_room->members.begin(), g_room->members.end(), [](const Member& m) { return m.id == g_room->owner; });
            if (host != g_room->members.end() && host->endpoint) {
                auto& s = g_sig[host->id];
                if (!s.up || t - s.lastTx > 2000) { W w; w.u64(g_room->id); w.u16(g_me); send_ctl(host->ip, host->udpPort, 'H', w.b); s.lastTx = t; }
            }
        }
        for (auto& m : g_room->members) {
            if (m.id == g_me || !m.endpoint) continue;
            auto& s = g_sig[m.id];
            if (t - s.lastTx >= (s.up ? 2000u : 250u)) { send_ctl(m.ip, m.udpPort, 'P', ping_body('P')); s.lastTx = t; }
            if (s.up && t - s.lastRx > 12000) { s.up = false; signaling_event(g_room->id, m.id, 0x5101); }
        }
    }
}

uint32_t my_lan_ip() {
    if (const char* f = std::getenv("NETFAKELAN")) { in_addr a{}; if (inet_pton(AF_INET, f, &a) == 1) return ntohl(a.s_addr); }   // tests: an unreachable LAN address
    char host[256]; if (gethostname(host, sizeof host) != 0) return 0x7f000001;
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return 0x7f000001;
    const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
}


// ------------------------------------------------------------------ UPnP: the host asks its router to open port 3658
// (TCP and UDP) towards this PC, so friends can join over the internet without configuring the router by hand.
// Windows' own UPnP NAT API (COM); its interfaces are declared here (MinGW has no natupnp.h).
struct IStaticPortMappingCollection_ : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Item(long, BSTR, IDispatch**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(long, BSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE Add(long, BSTR, long, BSTR, VARIANT_BOOL, BSTR, IDispatch**) = 0;
};
struct IUPnPNAT_ : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(IStaticPortMappingCollection_**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DynamicPortMappingCollection(IDispatch**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NATEventManager(IDispatch**) = 0;
};
const GUID kClsidUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
const GUID kIidUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};
std::atomic<int> g_upnp{0};   // 0 not tried, 1 trying, 2 open, 3 the router does not allow it
void upnp_open() {
    int expected = 0;
    if (!g_upnp.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        IUPnPNAT_* nat = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidUPnPNAT, nullptr, CLSCTX_ALL, kIidUPnPNAT, reinterpret_cast<void**>(&nat))) && nat) {
            IStaticPortMappingCollection_* col = nullptr;
            if (SUCCEEDED(nat->get_StaticPortMappingCollection(&col)) && col) {   // null when there is no UPnP router
                const uint32_t ip = my_lan_ip();
                wchar_t client[32]; std::swprintf(client, 32, L"%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
                ok = true;
                for (const wchar_t* proto : {L"UDP", L"TCP"}) {
                    BSTR p = SysAllocString(proto), cl = SysAllocString(client), desc = SysAllocString(L"Racing Club+");
                    col->Remove(g_port, p);   // a mapping from an earlier session (maybe to another PC of the house)
                    IDispatch* m = nullptr;
                    const HRESULT hr = col->Add(g_port, p, g_port, cl, VARIANT_TRUE, desc, &m);
                    if (FAILED(hr)) { ok = false; NLOG("[net] UPnP: the router refused port %u %ls (%08lx)\n", g_port, proto, (unsigned long)hr); }
                    if (m) m->Release();
                    SysFreeString(p); SysFreeString(cl); SysFreeString(desc);
                }
                col->Release();
            } else NLOG("[net] UPnP: no router answers (UPnP off or not supported)\n");
            nat->Release();
        }
        g_upnp = ok ? 2 : 3;
        if (ok) NLOG("[net] UPnP: port %u (TCP and UDP) open on the router\n", g_port);
        CoUninitialize();
    }).detach();
}

// ------------------------------------------------------------------ requests
bool filters_match(PPU& c, uint32_t req, const Room& rm) {   // SceNpMatching2SearchRoomRequest int filters (EQ..GE)
    const uint32_t f = c.rd32(req + 32), num = c.rd32(req + 36);
    for (uint32_t i = 0; f && i < std::min<uint32_t>(num, 16); ++i) {
        const uint8_t op = c.mem[f + i * 12]; const uint16_t id = uint16_t(c.rd32(f + i * 12 + 4) >> 16); const uint32_t v = c.rd32(f + i * 12 + 8);
        auto it = std::find_if(rm.searchInt.begin(), rm.searchInt.end(), [&](const Attr& a) { return a.id == id; });
        const uint32_t have = it == rm.searchInt.end() ? 0 : it->num;
        const bool ok = op == 1 ? have == v : op == 2 ? have != v : op == 3 ? have < v : op == 4 ? have <= v : op == 5 ? have > v : op == 6 ? have >= v : true;
        if (!ok) return false;
    }
    return rm.members.size() < rm.maxSlot && !(rm.flags & 0x50000000u);   // not full, closed or hidden
}

void do_search(uint32_t reqId, std::vector<uint8_t> filtersCopy, uint32_t req, uint32_t maxResults) {
    // filtersCopy: the request's int filters, read on the game thread (a 16-byte image per filter: op, id, value)
    std::thread([reqId, filtersCopy, maxResults] {
        { std::lock_guard<std::mutex> l2(g_searchMx); g_searchResults.clear(); }
        g_searching = true;
        std::vector<std::pair<uint32_t, uint16_t>> targets;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            in_addr a{};
            std::string hostIp = g_hostIp.empty() ? std::string(settings().onlineIp) : g_hostIp;   // (the menu may change it while playing)
            uint16_t hostPort = 3658;
            if (const size_t k = hostIp.find(':'); k != std::string::npos) { hostPort = uint16_t(std::atoi(hostIp.c_str() + k + 1)); hostIp.resize(k); }
            if (!hostIp.empty() && inet_pton(AF_INET, hostIp.c_str(), &a) != 1) {   // a name (DynDNS...): resolved
                addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
                if (getaddrinfo(hostIp.c_str(), nullptr, &hints, &res) == 0 && res) { a = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr; freeaddrinfo(res); }
                else a.s_addr = 0;
            }
            if (!hostIp.empty() && a.s_addr) targets.push_back({ntohl(a.s_addr), hostPort});
            for (auto& p : g_probe) targets.push_back(p);
            targets.push_back({0xffffffffu, 3658});   // local network
        }
        for (int round = 0; round < 3; ++round) {
            for (auto& [ip, port] : targets) send_ctl(ip, port, 'Q', {});
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        g_searching = false;
        std::vector<SearchResult> found;
        { std::lock_guard<std::mutex> l2(g_searchMx); found = g_searchResults; }
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        std::vector<SearchResult> rooms;
        for (auto& f : found) {
            if (std::any_of(rooms.begin(), rooms.end(), [&](const SearchResult& x) { return x.room.id == f.room.id; })) continue;
            if (g_room && f.room.id == g_room->id) continue;
            bool ok = f.room.members.size() < f.room.maxSlot && !(f.room.flags & 0x50000000u);
            for (size_t i = 0; ok && i + 12 <= filtersCopy.size(); i += 12) {
                const uint8_t op = filtersCopy[i]; const uint16_t id = uint16_t(filtersCopy[i + 4] << 8 | filtersCopy[i + 5]);
                const uint32_t v = uint32_t(filtersCopy[i + 8]) << 24 | uint32_t(filtersCopy[i + 9]) << 16 | uint32_t(filtersCopy[i + 10]) << 8 | filtersCopy[i + 11];
                auto it = std::find_if(f.room.searchInt.begin(), f.room.searchInt.end(), [&](const Attr& a) { return a.id == id; });
                const uint32_t have = it == f.room.searchInt.end() ? 0 : it->num;
                ok = op == 1 ? have == v : op == 2 ? have != v : op == 3 ? have < v : op == 4 ? have <= v : op == 5 ? have > v : op == 6 ? have >= v : true;
            }
            if (ok) rooms.push_back(f);
            if (rooms.size() >= maxResults) break;
        }
        for (auto& f : found) g_found[f.room.id] = {f.ip, f.port};
        Blob b;   // SceNpMatching2SearchRoomResponse {Range {u32 start, total, size}; u32 roomDataExternal*}
        const uint32_t o = b.alloc(16, 8);
        b.w32(o, 1); b.w32(o + 4, uint32_t(rooms.size())); b.w32(o + 8, uint32_t(rooms.size()));
        uint32_t prev = 0;
        for (auto& f : rooms) { const uint32_t e = b.room_external(f.room); if (prev) b.ptr(prev, e); else b.ptr(o + 12, e); prev = e; }
        NLOG("[net] search: %zu answers, %zu rooms match\n", found.size(), rooms.size());
        complete(reqId, 0, &b);
    }).detach();
    (void)req;
}

void do_join(uint32_t reqId, uint64_t roomId, std::vector<Attr> memberBin, uint8_t team) {
    std::thread([reqId, roomId, memberBin, team] {
        std::pair<uint32_t, uint16_t> host;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            auto it = g_found.find(roomId);
            if (it == g_found.end()) { complete(reqId, E_NO_SUCH_ROOM, nullptr); return; }
            host = it->second;
        }
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(host.second); a.sin_addr.s_addr = htonl(host.first);
        DWORD to = 8000; ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { NLOG("[net] cannot reach the host: %d\n", WSAGetLastError()); closesocket(s); complete(reqId, E_TIMEOUT, nullptr); return; }
        BOOL nd = TRUE; ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
        W w; w.u64(roomId); w.str(g_name); w.u8(team); w.u16(g_port); put_attrs(w, memberBin);
        uint8_t type; std::vector<uint8_t> body;
        if (!send_msg(s, M_JOIN, w.b) || !recv_msg(s, type, body)) { closesocket(s); complete(reqId, E_TIMEOUT, nullptr); return; }
        R r(body);
        if (type != M_JOIN_OK) { const uint32_t e = r.u32(); closesocket(s); complete(reqId, e ? e : E_NO_SUCH_ROOM, nullptr); return; }
        Room rm = get_room(r); const uint16_t me = uint16_t(r.u16());
        if (r.bad) { closesocket(s); complete(reqId, E_NO_SUCH_ROOM, nullptr); return; }
        to = 0; ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (auto& m : rm.members) if (m.id == rm.owner) { m.ip = host.first; m.udpPort = host.second; m.endpoint = true; m.direct = true; }   // the host as we reach it
        g_room = std::make_shared<Room>(rm); g_inRoomFlag = true; g_me = me; g_host = false; g_toHost = s; g_sig.clear();
        start_signaling();
        Blob b; const uint32_t o = b.alloc(8, 8); b.ptr(o, b.room_internal(*g_room, g_me));   // SceNpMatching2JoinRoomResponse
        NLOG("[net] joined room %016llx as member %u\n", (unsigned long long)roomId, me);
        complete(reqId, 0, &b);
        std::thread(client_loop, s).detach();
    }).detach();
}

} // namespace

bool in_room() { return g_inRoomFlag; }

// ------------------------------------------------------------------ entry points (lv2.cpp)
std::string local_address() {   // for the menu: what a friend on the same network types
    const uint32_t ip = my_lan_ip();
    char t[32]; std::snprintf(t, sizeof t, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return t;
}
bool enabled();
int upnp_state() { return g_upnp; }
std::string player_name() { static bool loaded = false; if (!loaded) { loaded = true; if (!enabled()) load_settings(); } return g_name; }
void set_errno_addr(uint32_t a) { g_errnoAddr = a; }

bool enabled() {
    static const bool on = [] {
        const char* env = std::getenv("NETPLAY");
        const bool e = env ? std::atoi(env) != 0 : settings().online != 0;
        if (e) {
            load_settings();
            WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
            std::thread(sig_thread).detach();
        }
        return e;
    }();
    return on;
}

std::vector<Callback> take_callbacks() { std::lock_guard<std::recursive_mutex> lk(g_mx); std::vector<Callback> v; v.swap(g_pending); return v; }

bool hle(PPU& c, const char* module, const std::string& n) {
    if (!enabled()) return false;
    const std::string m = module;
    if (m != "sceNp" && m != "sceNp2" && m != "sys_net" && m != "cellNetCtl" && m != "cellSysutilAvc2") return false;
    if (n.rfind("sceNpTrophy", 0) == 0) return false;   // trophies: lv2.cpp
    static const bool verbose = std::getenv("NETLOG") != nullptr;
    if (verbose && n != "recvfrom" && n != "sendto" && n != "sceNpManagerGetStatus")
        NLOG("[net] %s(%08x, %08x, %08x, %08x, %08x, %08x) lr %08x\n", n.c_str(), a32(c, 0), a32(c, 1), a32(c, 2), a32(c, 3), a32(c, 4), a32(c, 5), uint32_t(c.lr));
    // --- connected and signed in
    if (n == "cellNetCtlGetState") { if (a32(c, 0)) c.wr32(a32(c, 0), 3); ret(c, 0); return true; }
    if (n == "cellNetCtlGetInfo") {
        const uint32_t code = a32(c, 0), out = a32(c, 1);
        if (code == 16 && out) {   // IP_ADDRESS
            const uint32_t ip = my_lan_ip(); char s[16]; std::snprintf(s, sizeof s, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
            std::memset(c.mem + out, 0, 16); std::memcpy(c.mem + out, s, std::strlen(s));
        } else if (out) std::memset(c.mem + out, 0, 4);
        ret(c, 0); return true;
    }
    if (n == "sceNpManagerGetStatus") { if (a32(c, 0)) c.wr32(a32(c, 0), 3); ret(c, 0); return true; }
    if (n == "sceNpManagerGetNpId") { if (a32(c, 0)) { std::memset(c.mem + a32(c, 0), 0, 36); std::memcpy(c.mem + a32(c, 0), g_name.data(), g_name.size()); } ret(c, 0); return true; }
    if (n == "sceNpManagerGetOnlineId") { if (a32(c, 0)) { std::memset(c.mem + a32(c, 0), 0, 20); std::memcpy(c.mem + a32(c, 0), g_name.data(), g_name.size()); } ret(c, 0); return true; }
    if (n == "sceNpManagerGetContentRatingFlag") { if (a32(c, 0)) c.wr32(a32(c, 0), 0); if (a32(c, 1)) c.wr32(a32(c, 1), 99); ret(c, 0); return true; }
    if (n == "sceNpManagerGetChatRestrictionFlag") { if (a32(c, 0)) c.wr32(a32(c, 0), 0); ret(c, 0); return true; }
    if (sockets(c, n)) return true;
    if (n == "sceNpUtilCmpNpId") {   // 0 when both SceNpId name the same player (online id, 16 chars)
        const uint32_t x = a32(c, 0), y = a32(c, 1);
        const bool same = x && y && std::strncmp(reinterpret_cast<const char*>(c.mem + x), reinterpret_cast<const char*>(c.mem + y), 16) == 0;
        ret(c, same ? 0 : 0x80029b0bu);   // SCE_NP_UTIL_ERROR_NOT_MATCH
        return true;
    }
    // --- Matching2: context and callbacks
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (n == "sceNpMatching2CreateContext") { if (a32(c, 3)) { c.mem[a32(c, 3)] = 0; c.mem[a32(c, 3) + 1] = 1; } ret(c, 0); return true; }
    if (n == "sceNpMatching2RegisterContextCallback") { g_ctx.ctxCb = a32(c, 1); g_ctx.ctxArg = a32(c, 2); ret(c, 0); return true; }
    if (n == "sceNpMatching2RegisterRoomEventCallback") { g_ctx.roomCb = a32(c, 1); g_ctx.roomArg = a32(c, 2); ret(c, 0); return true; }
    if (n == "sceNpMatching2RegisterSignalingCallback") { g_ctx.sigCb = a32(c, 1); g_ctx.sigArg = a32(c, 2); ret(c, 0); return true; }
    if (n == "sceNpMatching2RegisterRoomMessageCallback") { g_ctx.msgCb = a32(c, 1); g_ctx.msgArg = a32(c, 2); ret(c, 0); return true; }
    if (n == "sceNpMatching2ContextStart") {
        g_ctx.started = true; open_udp();
        queue(g_ctx.ctxCb, {1, 0x6f02, 11, 0, g_ctx.ctxArg});   // (ctxId, event Start, cause CONTEXT_ACTION, error, arg)
        ret(c, 0); return true;
    }
    if (n == "sceNpMatching2ContextStop") {
        g_ctx.started = false;
        if (g_room) { if (g_host) { broadcast(M_DESTROYED, {}); for (auto& p : g_peers) closesocket(p.s); g_peers.clear(); } leave_room_local(false, 0); }
        queue(g_ctx.ctxCb, {1, 0x6f03, 11, 0, g_ctx.ctxArg});
        ret(c, 0); return true;
    }
    if (n == "sceNpMatching2GetServerIdListLocal") { if (a32(c, 1) && a32(c, 2)) { c.mem[a32(c, 1)] = 0; c.mem[a32(c, 1) + 1] = 1; } ret(c, 1); return true; }
    if (n == "sceNpMatching2GetEventData") {
        const uint32_t key = a32(c, 1), buf = a32(c, 2), len = a32(c, 3);
        auto it = g_events.find(key);
        if (it == g_events.end() || !buf) { ret(c, 0); return true; }
        Blob& b = it->second;
        const uint32_t k = std::min<uint32_t>(len, uint32_t(b.d.size()));
        std::memcpy(c.mem + buf, b.d.data(), k);
        for (uint32_t o : b.rel) if (o + 4 <= k) { const uint32_t v = c.rd32(buf + o); if (v || true) c.wr32(buf + o, buf + v); }
        g_events.erase(it);
        ret(c, k); return true;
    }
    if (n == "sceNpMatching2ClearEventData") { g_events.erase(a32(c, 1)); ret(c, 0); return true; }
    if (n == "sceNpMatching2AbortRequest") { complete(a32(c, 1), 0x8002232f, nullptr); ret(c, 0); return true; }
    // --- Matching2 requests: (ctxId, request*, optParam*, u32* assignedReqId)
    auto request = [&](uint16_t event) { const uint32_t id = new_request(c, a32(c, 2), event); if (a32(c, 3)) c.wr32(a32(c, 3), id); return id; };
    const uint32_t req = a32(c, 1);
    if (n == "sceNpMatching2GetServerInfo") {
        const uint32_t id = request(0x0001);
        Blob b; const uint32_t o = b.alloc(4, 4); b.w16(o, 1); b.w8(o + 2, 1);   // server 1, AVAILABLE
        complete(id, 0, &b); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2GetWorldInfoList") {
        const uint32_t id = request(0x0002);
        Blob b; const uint32_t o = b.alloc(8, 8); const uint32_t wo = b.alloc(60, 4);
        b.w32(wo, 1); b.w32(wo + 12, 0); b.w32(wo + 16, g_room ? 1 : 0); b.ptr(o, wo); b.w32(o + 4, 1);
        complete(id, 0, &b); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2SearchRoom") {
        {   // (NETLOG) what the game asks for
            std::string ids; const uint32_t ap = c.rd32(req + 48), an = c.rd32(req + 52);
            for (uint32_t i = 0; ap && i < std::min<uint32_t>(an, 16); ++i) { char t[8]; std::snprintf(t, sizeof t, " %04x", c.rd32(ap + i * 2) >> 16); ids += t; }
            std::string fl; const uint32_t fp = c.rd32(req + 32), fnum = c.rd32(req + 36);
            for (uint32_t i = 0; fp && i < std::min<uint32_t>(fnum, 16); ++i) { char t[40]; std::snprintf(t, sizeof t, " [op%u %04x %u]", c.mem[fp + i * 12], c.rd32(fp + i * 12 + 4) >> 16, c.rd32(fp + i * 12 + 8)); fl += t; }
            NLOG("[net] search: option %x range %u+%u flagFilter %08x flagAttr %08x attrs%s filters%s bin %u\n", c.rd32(req), c.rd32(req + 16), c.rd32(req + 20), c.rd32(req + 24), c.rd32(req + 28), ids.c_str(), fl.c_str(), c.rd32(req + 44));
        }
        const uint32_t id = request(0x0106);
        std::vector<uint8_t> filters;
        const uint32_t f = c.rd32(req + 32), num = std::min<uint32_t>(c.rd32(req + 36), 16);
        for (uint32_t i = 0; f && i < num; ++i) filters.insert(filters.end(), c.mem + f + i * 12, c.mem + f + i * 12 + 12);
        const uint32_t maxR = std::max<uint32_t>(1, std::min<uint32_t>(c.rd32(req + 20), 20));
        do_search(id, filters, req, maxR);
        ret(c, 0); return true;
    }
    if (n == "sceNpMatching2CreateJoinRoom") {
        const uint32_t id = request(0x0101);
        if (!host_start()) { complete(id, 0x80022b02, nullptr); ret(c, 0); return true; }
        auto rm = std::make_shared<Room>();
        std::mt19937_64 rng(std::random_device{}() ^ now_ms());
        rm->id = rng() | 1; rm->maxSlot = std::clamp<uint32_t>(c.rd32(req + 16), 2, 8); rm->flags = c.rd32(req + 20) & ~0x20000000u;
        rm->binInternal = read_bin_attrs(c, c.rd32(req + 24), c.rd32(req + 28));
        rm->searchInt = read_int_attrs(c, c.rd32(req + 32), c.rd32(req + 36));
        rm->searchBin = read_bin_attrs(c, c.rd32(req + 40), c.rd32(req + 44));
        rm->binExternal = read_bin_attrs(c, c.rd32(req + 48), c.rd32(req + 52));
        Member me; me.id = 1; me.name = g_name; me.team = c.mem[req + 100]; me.flags = 0x80000000u; me.joinDate = uint64_t(std::time(nullptr)) * 1000000ull;
        me.bin = read_bin_attrs(c, c.rd32(req + 92), c.rd32(req + 96)); me.ip = my_lan_ip(); me.udpPort = g_port; me.endpoint = true;
        rm->members.push_back(me); rm->owner = 1; rm->nextMember = 1;
        g_room = rm; g_inRoomFlag = true; g_me = 1; g_host = true; g_sig.clear();
        Blob b; const uint32_t o = b.alloc(8, 8); b.ptr(o, b.room_internal(*g_room, g_me));   // SceNpMatching2CreateJoinRoomResponse
        NLOG("[net] room created: up to %u players, flags %08x\n", rm->maxSlot, rm->flags);
        for (auto* v : {&rm->binInternal, &rm->searchInt, &rm->searchBin, &rm->binExternal})
            for (auto& a : *v) NLOG("[net]   attr %04x: num %u, %zu bytes\n", a.id, a.num, a.data.size());
        complete(id, 0, &b); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2JoinRoom") {
        const uint32_t id = request(0x0102);
        const uint64_t roomId = uint64_t(c.rd32(req)) << 32 | c.rd32(req + 4);
        do_join(id, roomId, read_bin_attrs(c, c.rd32(req + 16), c.rd32(req + 20)), c.mem[req + 44]);
        ret(c, 0); return true;
    }
    if (n == "sceNpMatching2LeaveRoom") {
        const uint32_t id = request(0x0103);
        if (g_room) {
            if (g_host) { broadcast(M_DESTROYED, {}); for (auto& p : g_peers) closesocket(p.s); g_peers.clear(); }
            else if (g_toHost != INVALID_SOCKET) send_msg(g_toHost, M_LEAVE, {});
            leave_room_local(false, 0);
        }
        complete(id, 0, nullptr); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2SetRoomDataInternal") {
        const uint32_t id = request(0x0109);
        if (g_room && g_host) {
            const uint32_t ff = c.rd32(req + 8), fa = c.rd32(req + 12);
            g_room->flags = (g_room->flags & ~ff) | (fa & ff);
            merge_attrs(g_room->binInternal, read_bin_attrs(c, c.rd32(req + 16), c.rd32(req + 20)));
            W w; put_room(w, *g_room); broadcast(M_ROOM, w.b);
            Blob b = room_internal_update(*g_room); room_event(g_room->id, 0x1106, 0, &b);
        }
        complete(id, 0, nullptr); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2SetRoomDataExternal") {
        const uint32_t id = request(0x0004);
        if (g_room && g_host) {
            merge_attrs(g_room->searchInt, read_int_attrs(c, c.rd32(req + 8), c.rd32(req + 12)));
            merge_attrs(g_room->searchBin, read_bin_attrs(c, c.rd32(req + 16), c.rd32(req + 20)));
            merge_attrs(g_room->binExternal, read_bin_attrs(c, c.rd32(req + 24), c.rd32(req + 28)));
        }
        complete(id, 0, nullptr); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2KickoutRoomMember") {
        const uint32_t id = request(0x0105);
        const uint16_t target = uint16_t(c.rd32(req + 8) >> 16);
        if (g_room && g_host)
            for (auto& p : g_peers) if (p.member == target) { send_msg(p.s, M_KICKED, {}); break; }
        if (g_host) host_member_gone(target, 2);
        complete(id, 0, nullptr); ret(c, 0); return true;
    }
    if (n == "sceNpMatching2SignalingGetConnectionStatus") {
        // (ctxId, roomId, memberId, int* connStatus, in_addr* peerAddr, u16* peerPort)
        const uint16_t mid = uint16_t(a32(c, 2)); const uint32_t st = a32(c, 3), pa = a32(c, 4), pp = a32(c, 5);
        if (st) c.wr32(st, 0);
        if (!g_room) { ret(c, 0x8002a80e); return true; }
        if (mid == g_me) { ret(c, 0x8002a816); return true; }
        auto it = std::find_if(g_room->members.begin(), g_room->members.end(), [&](const Member& x) { return x.id == mid; });
        if (it == g_room->members.end()) { ret(c, 0x8002a80e); return true; }
        const bool up = g_sig.count(mid) && g_sig[mid].up;
        if (st) c.wr32(st, up ? 2 : 1);
        if (pa) c.wr32(pa, virt_ip(mid));
        if (pp) { c.mem[pp] = uint8_t(3658 >> 8); c.mem[pp + 1] = uint8_t(3658 & 255); }
        ret(c, 0); return true;
    }
    if (n == "sceNpMatching2SignalingGetPingInfo") {
        const uint32_t id = request(0x0e01);
        Blob b; const uint32_t o = b.alloc(40, 8); b.w16(o, 1); b.w32(o + 4, 1); b.w64(o + 8, g_room ? g_room->id : 0); b.w32(o + 16, 30000);
        complete(id, 0, &b); ret(c, 0); return true;
    }
    ret(c, 0);   // everything else (lookup, score, basic, voice chat): accepted, nothing done
    return true;
}

} // namespace gti::recomp::netplay
