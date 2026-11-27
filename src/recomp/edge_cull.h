#pragma once
#include <cstdint>

namespace gti::recomp {

struct EdgeCullResult { bool handled; uint32_t count; };

// Edge Geometry triangle culling done natively (see edge_cull.cpp). ls = SPU local store at the routine's entry,
// ctx = r3, mode = r4, entryR1 = r1. When handled, the kept indices are compacted in place, ctx+0x58 and the
// caller's count (entryR1 + 0x258) are written; the caller must set r3 = count (all slots) and return to r0.
EdgeCullResult edge_cull_native(uint8_t* ls, uint32_t ctx, uint32_t mode, uint32_t entryR1);

}  // namespace gti::recomp
