#pragma once
// Runtime interface of SPU programs statically recompiled to C++ by tools/spu2cpp.py.
// The instruction helpers the generated code uses are in spu_ops.h (needs SSSE3/SSE4.1).
#include "recomp/spu.h"

namespace gti::recomp::spurt {

// A register: four host-order 32-bit words in SPU order (word 0 = preferred slot).
struct Q { uint32_t w[4]; };

// SPUPROF=1: trampoline entries per (program, chunk), printed by the job manager
extern bool g_spuProf;
extern uint64_t g_spuProfCnt[64][256];
// entries into the slow (every instruction is an entry point) variant: indirect branches to unforeseen targets
extern uint64_t g_spuSlowEntries;
void spu_slow_note(uint32_t ea, uint32_t pc);   // SPUPROF=1: where the slow variant is entered ([spuslow] report)
void spu_slow_report();

// native replacement of SPU routines, per program ((eaBin >> 4) % 64): called by the dispatcher whenever execution
// crosses into another chunk (calls and returns between routines do); returns the pc to continue at (pc itself = not handled)
using HleHook = uint32_t (*)(Q* __restrict r, Spu& s, uint32_t pc);
extern HleHook g_spuHle[64];
// the pcs a hook wants (up to 8; all 0 = every crossing): the dispatcher calls it only there (an indirect call on
// every chunk crossing was 2-3 % of the Edge jobs)
extern uint32_t g_spuHlePcs[64][8];
inline bool spu_hle_wants(int k, uint32_t pc) {
    const uint32_t* p = g_spuHlePcs[k];
    uint32_t any = 0; bool hit = false;
    for (int i = 0; i < 8; ++i) { hit |= pc == p[i]; any |= p[i]; }
    return hit || any == 0;
}

// A chunk runs from pc until it leaves its 1 KB of code. Result: next pc, | 0x80000000 = the interpreter must
// execute the instruction at that pc, | 0x40000000 (fast chunks only) = pc is not an entry point of the fast chunk.
using FastChunk = uint32_t (*)(Q* __restrict, Spu&, uint32_t pc);
using SlowChunk = uint32_t (*)(Q* __restrict, Spu&, uint32_t pc, bool single);   // single: run one instruction

// one recompiled program: fn runs from s.pc until it needs the interpreter (stop, halt, unknown word, ...)
struct Compiled { uint32_t eaBin, lsBase, size; void (*fn)(Spu&); const SlowChunk* slow; };
const Compiled* find_compiled(uint32_t eaBin);
const Compiled* compiled_at(int index);   // nullptr past the end

} // namespace gti::recomp::spurt
