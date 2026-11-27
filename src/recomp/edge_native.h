#pragma once
// Native replacements of Edge Geometry routines (SPU program at EA 0xbaea80) other than the triangle culling
// (edge_cull.h). Each works on the job's local store only, like the SPU routine it replaces.
#include <cstdint>

namespace gti::recomp {

// Runs the native version of the routine at LS `entry` if there is one and it handles this input.
// a[0..7] = r3..r10 (preferred slots), r1 = stack pointer at entry. Returns false = not handled (run the SPU code);
// true = done, *ret = the routine's r3 (preferred slot).
bool edge_native_run(uint32_t entry, uint8_t* ls, const uint32_t* a, uint32_t r1, uint32_t* ret);

// calls handled natively / left to the SPU code, per routine (0: 0x7140, 1: 0x12780, 2: 0x12628); EDGESTAT2=1 logs them
extern uint64_t g_edgeNativeCalls[4], g_edgeNativeFallbacks[4];
// [lo, hi) of the local store where the last native call on this thread did not reproduce what the SPU code leaves
// (padding and work areas nothing reads); empty when it is exact. For fn_replay.
extern thread_local uint32_t g_edgeNativeDead[2];

} // namespace gti::recomp
