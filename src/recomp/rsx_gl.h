#pragma once
// RSX (NV47 / "Curie") renderer on OpenGL 4.5 for the fully recompiled game: consumes the NV4097 methods the
// command processor in lv2.cpp decodes, translates the RSX vertex / fragment microcode to GLSL and shows the
// display buffers in an SDL window. Every call comes from the RSX command thread (which owns the GL context).
#include <cstdint>
#include <string>

namespace gti::recomp::rsx {

struct Config {
    uint8_t* mem = nullptr;        // guest address space
    uint32_t localAddr = 0;        // EA of RSX local memory (offset 0)
    uint32_t ioAddr = 0;           // EA of IO offset 0 (main memory mapped for the RSX)
};

bool enabled();                                       // RSX=0 in the environment disables drawing (headless)
void init(const Config& cfg);
void method(uint32_t m, uint32_t arg);                // every NV4097 method (subchannel 0), in stream order
// a whole method packet: count big-endian argument words at args (increasing methods unless noInc)
void method_block(uint32_t m0, const uint8_t* args, uint32_t count, bool noInc);
void set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h);
void flip(uint32_t bufferId);                         // shows a display buffer and pumps window events
void screen_loaded(const std::string& screen);   // a menu screen (data/sumotool/ps3/<screen>) was loaded: the Patreon banner shows on main_menu
void toast(const std::wstring& title, const std::wstring& name, const std::wstring& detail, uint32_t rgb, const std::string& png = {});   // a notification (trophy) for 5 s
bool quit_requested();
int pad_kind();                                       // the controller connected: 0 none, 1 PlayStation, 2 Xbox or other, 3 Nintendo
void textures_rewritten();                            // texture data changed in place (button icons): every texture is read again
uint32_t texture_epoch();                             // bumped by textures_rewritten (part of the texture signatures)
// running work counters of the RSX thread (MISSLOG: what a late frame had to build)
struct Counters { uint64_t progs, texUploads, texBytes, streams, swapUs, draws, drawUs, progUs, texUs, convUs, bufUs, streamVerts; };
Counters counters();

} // namespace gti::recomp::rsx
