#pragma once
// Host implementation of the PS3 system layer (lv2 kernel calls + system library imports) for running the whole
// recompiled game natively: threads, synchronisation, memory, time, files, tty.
#include "recomp/guest.h"
#include <string>

namespace gti::recomp {

struct Lv2Config {
    std::string usrdir;          // host folder of NPEB00060/USRDIR (game data)
    std::string hdd0;            // host folder standing in for /dev_hdd0 (saves etc.)
    bool trace = false;          // log every system call
    std::string padScript;       // test input: "start:6,cross:9.5" = press for 0.25 s at those seconds
};

// DualShock 3 state as the game reads it (cellPadGetData)
struct PadState {
    uint16_t digital1 = 0;   // SELECT 0x01, L3 0x02, R3 0x04, START 0x08, UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80
    uint16_t digital2 = 0;   // L2 0x01, R2 0x02, L1 0x04, R1 0x08, TRIANGLE 0x10, CIRCLE 0x20, CROSS 0x40, SQUARE 0x80
    uint16_t analog[4] = {0x80, 0x80, 0x80, 0x80};   // right x, right y, left x, left y
    uint16_t pressure[12] = {};
};
void lv2_set_pad(const PadState& s);
uint32_t lv2_rumble();   // vibration the game asks for: small motor (0/1) << 8 | large motor (0..255)   // live input from the host (overrides the test script)

void lv2_install(Guest& g, const Lv2Config& cfg);   // also installs the import stubs (hle_install)
void lv2_dump_waits();   // what every guest thread is blocked in (debug)
unsigned long long lv2_flip_count();   // flips executed by the RSX so far
bool audio_out_open();   // audio_out.cpp: 48 kHz stereo float output
void audio_out_clear();
int audio_out_queued();
void audio_out_push(const float* stereo, int frames);
int lv2_run_main(Guest& g);                          // boots the game at the ELF entry; returns exit code

} // namespace gti::recomp
