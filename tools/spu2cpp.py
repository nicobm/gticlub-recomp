"""Static recompiler: SPU job binaries (embedded in the EBOOT) -> C++ (src/recomp/spu_gen/spu_<ea>*.cpp).

The program is cut into 1 KB chunks, one C++ function each, in two variants:
  fast  only real entry points (branch targets, return addresses...) are labels, so the compiler can optimize
        across instructions; direct branches inside the chunk are gotos.
  slow  every instruction is an entry point and every branch returns to the trampoline. Used when an indirect
        branch lands on an address the fast variant did not foresee, and by src/tools/spu_diff.cpp to run one
        instruction at a time against the interpreter.
Anything the generator does not handle (stop, halt, unknown words, targets outside the program) leaves the
function with s.pc set, and the interpreter in spu.cpp continues from there.
Semantics mirror spu.cpp; helpers are in src/recomp/spu_ops.h.

usage: python tools/spu2cpp.py re/EBOOT.elf src/recomp/spu_gen  ea:size[:lsbase] ...
"""
import math
import os
import struct
import sys


def load_elf(path):
    f = open(path, 'rb').read()
    phoff = struct.unpack('>Q', f[0x20:0x28])[0]
    phentsize, phnum = struct.unpack('>HH', f[0x36:0x3a])
    segs = []
    for i in range(phnum):
        p = f[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', p)
        if t == 1:
            segs.append((va, fs, ms, off))

    def read(va, n):
        out = bytearray(n)
        for k in range(n):
            a = va + k
            for (sva, fs, ms, off) in segs:
                if sva <= a < sva + ms:
                    out[k] = f[off + a - sva] if a - sva < fs else 0
                    break
        return bytes(out)
    return read


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def leaders_of(code, base, size):
    """Addresses that can be entered other than by falling through: chunk starts, direct branch targets,
    return addresses of calls, the instruction after anything that leaves to the interpreter, and code addresses
    loaded with ila. Indirect branches to anything else take the slow variant."""
    n = size // 4
    end = base + size
    L = set(range(base, end, CH))
    L.add(base + 0x10)

    def add(t):
        t &= 0x3fffc
        if base <= t < end:
            L.add(t)
    for k in range(n):
        pc = base + 4 * k
        w = struct.unpack('>I', code[4 * k: 4 * k + 4])[0]
        op9, op11, op7 = w >> 23, w >> 21, w >> 25
        s16 = sx((w >> 7) & 0xffff, 16)
        if op9 in (0x064, 0x066, 0x042, 0x040, 0x046, 0x044):
            add(pc + (s16 << 2))
        if op9 in (0x060, 0x062):
            add(s16 << 2)
        if op9 in (0x066, 0x062) or op11 in (0x1A9, 0x000, 0x140, 0x00D, 0x10D):
            add(pc + 4)
        if op7 == 0x21:
            i18 = (w >> 7) & 0x3ffff
            if (i18 & 3) == 0:
                add(i18)
    return L


CH = 1024   # bytes per chunk function (256 instructions)


def gen_lines(code, base, size, slow, leaders):
    """C++ statements of every instruction. slow: every instruction is an entry point, every branch returns to
    the trampoline and `single` stops after one instruction. Otherwise only `leaders` get labels."""
    n = size // 4
    lines = []
    starts = []
    E = lines.append
    end = base + size

    def tgt_ok(t):
        return base <= t < end and (t & 3) == 0

    cur = [0]

    def jump(t):
        t &= 0x3fffc
        if not slow and tgt_ok(t) and (t - base) // CH == cur[0]:
            return f'goto L_{t:x};'
        return f'return 0x{t:x}u;' if tgt_ok(t) else f'return 0x{0x80000000 | t:x}u;'

    def emit_one(k):
        pc = base + 4 * k
        w = struct.unpack('>I', code[4 * k: 4 * k + 4])[0]
        npc = (pc + 4) & 0x3ffff
        rt, ra, rb = w & 0x7f, (w >> 7) & 0x7f, (w >> 14) & 0x7f
        cur[0] = (pc - base) // CH
        if slow or pc in leaders:
            E(f'L_{pc:x}:')
        exit_here = f'return 0x{0x80000000 | pc:x}u;'
        s = None
        if w >> 31:   # RRR
            op4 = w >> 28
            rT, rc = (w >> 21) & 0x7f, w & 0x7f
            A, Bq, Cq = f'r[{ra}]', f'r[{rb}]', f'r[{rc}]'
            if op4 == 0x8 and VEC:
                s = f'{{ const __m128i a = V({A}), b = V({Bq}), c = V({Cq}); r[{rT}] = mk(_mm_or_si128(_mm_andnot_si128(c, a), _mm_and_si128(b, c))); }}'
            elif op4 in (0xD, 0xE, 0xF) and VEC:
                ex = {0xD: '_mm_sub_ps(PS(c), vmul_spu(PS(a), PS(b)))', 0xE: '_mm_add_ps(vmul_spu(PS(a), PS(b)), PS(c))', 0xF: '_mm_sub_ps(vmul_spu(PS(a), PS(b)), PS(c))'}[op4]
                s = f'{{ const __m128i a = V({A}), b = V({Bq}), c = V({Cq}); r[{rT}] = mk(IS({ex})); }}'
            elif op4 == 0x8:
                s = f'{{ Q t; for (int i = 0; i < 4; ++i) t.w[i] = ({A}.w[i] & ~{Cq}.w[i]) | ({Bq}.w[i] & {Cq}.w[i]); r[{rT}] = t; }}'
            elif op4 == 0xB:
                s = f'r[{rT}] = shufb({A}, {Bq}, {Cq});'
            elif op4 == 0xC:
                s = f'{{ Q t; for (int i = 0; i < 4; ++i) t.w[i] = uint32_t(int32_t(int16_t({A}.w[i])) * int32_t(int16_t({Bq}.w[i])) + int32_t({Cq}.w[i])); r[{rT}] = t; }}'
            elif op4 == 0xD:
                s = f'{{ Q t; for (int i = 0; i < 4; ++i) t.w[i] = fb(f({Cq}, i) - spu_mul(f({A}, i), f({Bq}, i))); r[{rT}] = t; }}'
            elif op4 == 0xE:
                s = f'{{ Q t; for (int i = 0; i < 4; ++i) t.w[i] = fb(spu_mul(f({A}, i), f({Bq}, i)) + f({Cq}, i)); r[{rT}] = t; }}'
            elif op4 == 0xF:
                s = f'{{ Q t; for (int i = 0; i < 4; ++i) t.w[i] = fb(spu_mul(f({A}, i), f({Bq}, i)) - f({Cq}, i)); r[{rT}] = t; }}'
            else:
                s = exit_here
            E('  ' + s)
            return
        op7 = w >> 25
        if op7 == 0x21:
            i18 = (w >> 7) & 0x3ffff
            E(f'  r[{rt}] = splat(0x{i18:x}u);')
            return
        if op7 in (0x08, 0x09):
            return
        op9 = w >> 23
        i16 = (w >> 7) & 0xffff
        s16 = sx(i16, 16)
        T = f'r[{rt}]'
        if op9 == 0x064: E('  ' + jump(pc + (s16 << 2))); return
        if op9 == 0x060: E('  ' + jump(s16 << 2)); return
        if op9 == 0x066: E(f'  {T} = pref(0x{npc:x}u); ' + jump(pc + (s16 << 2))); return
        if op9 == 0x062: E(f'  {T} = pref(0x{npc:x}u); ' + jump(s16 << 2)); return
        if op9 == 0x042: E(f'  if ({T}.w[0] != 0) ' + jump(pc + (s16 << 2))); return
        if op9 == 0x040: E(f'  if ({T}.w[0] == 0) ' + jump(pc + (s16 << 2))); return
        if op9 == 0x046: E(f'  if (({T}.w[0] & 0xffff) != 0) ' + jump(pc + (s16 << 2))); return
        if op9 == 0x044: E(f'  if (({T}.w[0] & 0xffff) == 0) ' + jump(pc + (s16 << 2))); return
        if op9 == 0x061: E(f'  {T} = ldq(ls, 0x{(s16 << 2) & 0x3fff0:x});'); return
        if op9 == 0x067: E(f'  {T} = ldq(ls, 0x{(pc + (s16 << 2)) & 0x3fff0:x});'); return
        if op9 == 0x041: E(f'  stq(ls, 0x{(s16 << 2) & 0x3fff0:x}, {T});'); return
        if op9 == 0x047: E(f'  stq(ls, 0x{(pc + (s16 << 2)) & 0x3fff0:x}, {T});'); return
        if op9 == 0x081: E(f'  {T} = splat(0x{s16 & 0xffffffff:x}u);'); return
        if op9 == 0x083: E(f'  {T} = splat(0x{(i16 << 16) | i16:x}u);'); return
        if op9 == 0x082: E(f'  {T} = splat(0x{i16 << 16:x}u);'); return
        if op9 == 0x0C1: E(f'  for (int i = 0; i < 4; ++i) {T}.w[i] |= 0x{i16:x}u;'); return
        if op9 == 0x065:
            ws = []
            for wi in range(4):
                v = 0
                for bi in range(4):
                    if (i16 >> (15 - (4 * wi + bi))) & 1:
                        v |= 0xff << (8 * (3 - bi))
                ws.append(v)
            E(f'  {T} = Q{{{{0x{ws[0]:x}u, 0x{ws[1]:x}u, 0x{ws[2]:x}u, 0x{ws[3]:x}u}}}};')
            return
        op8 = w >> 24
        i10 = sx((w >> 14) & 0x3ff, 10)
        A = f'r[{ra}]'
        u10 = i10 & 0xffffffff

        def W(expr):
            return f'{{ const Q a = {A}; Q o; for (int i = 0; i < 4; ++i) o.w[i] = {expr}; {T} = o; }}'

        def Hh(expr):
            return f'{{ const Q a = {A}; Q o; for (int i = 0; i < 8; ++i) HR(o, i) = uint16_t({expr.replace('h(', 'hr(')}); {T} = o; }}'

        def Bb(expr):
            return f'{{ const Q a = {A}; Q o; for (int i = 0; i < 16; ++i) BR(o, i) = uint8_t({expr.replace('b(a, i)', 'br(a, i)').replace('b(bb, i)', 'br(bb, i)')}); {T} = o; }}'
        ri10 = {
            0x34: f'{T} = ldq(ls, {A}.w[0] + 0x{(i10 << 4) & 0xffffffff:x}u);',
            0x24: f'stq(ls, {A}.w[0] + 0x{(i10 << 4) & 0xffffffff:x}u, {T});',
            0x1C: W(f'a.w[i] + 0x{u10:x}u'),
            0x1D: Hh(f'h(a, i) + ({i10})'),
            0x0C: W(f'0x{u10:x}u - a.w[i]'),
            0x0D: Hh(f'({i10}) - h(a, i)'),
            0x14: W(f'a.w[i] & 0x{u10:x}u'),
            0x15: Hh(f'h(a, i) & 0x{u10 & 0xffff:x}'),
            0x16: Bb(f'b(a, i) & 0x{u10 & 0xff:x}'),
            0x04: W(f'a.w[i] | 0x{u10:x}u'),
            0x05: Hh(f'h(a, i) | 0x{u10 & 0xffff:x}'),
            0x06: Bb(f'b(a, i) | 0x{u10 & 0xff:x}'),
            0x44: W(f'a.w[i] ^ 0x{u10:x}u'),
            0x45: Hh(f'h(a, i) ^ 0x{u10 & 0xffff:x}'),
            0x46: Bb(f'b(a, i) ^ 0x{u10 & 0xff:x}'),
            0x74: W(f'uint32_t(int32_t(int16_t(a.w[i])) * ({i10}))'),
            0x75: W(f'(a.w[i] & 0xffff) * 0x{u10 & 0xffff:x}u'),
            0x7C: W(f'a.w[i] == 0x{u10:x}u ? ~0u : 0u'),
            0x7D: Hh(f'h(a, i) == 0x{u10 & 0xffff:x} ? 0xffff : 0'),
            0x7E: Bb(f'b(a, i) == 0x{u10 & 0xff:x} ? 0xff : 0'),
            0x4C: W(f'int32_t(a.w[i]) > ({i10}) ? ~0u : 0u'),
            0x4D: Hh(f'int16_t(h(a, i)) > int16_t({i10}) ? 0xffff : 0'),
            0x4E: Bb(f'int8_t(b(a, i)) > int8_t({i10}) ? 0xff : 0'),
            0x5C: W(f'a.w[i] > 0x{u10:x}u ? ~0u : 0u'),
            0x5D: Hh(f'h(a, i) > 0x{u10 & 0xffff:x} ? 0xffff : 0'),
            0x5E: Bb(f'b(a, i) > 0x{u10 & 0xff:x} ? 0xff : 0'),
        }
        if VEC:
            def V1(expr):
                return f'{{ const __m128i a = V({A}); {T} = mk({expr}); }}'
            w32, s32 = f'_mm_set1_epi32(int(0x{u10:x}u))', f'_mm_set1_epi32(int(0x{u10 ^ 0x80000000:x}u))'
            h16, b8 = f'_mm_set1_epi16(short(0x{u10 & 0xffff:x}))', f'_mm_set1_epi8(char(0x{u10 & 0xff:x}))'
            vri10 = {
                0x1C: V1(f'_mm_add_epi32(a, {w32})'), 0x0C: V1(f'_mm_sub_epi32({w32}, a)'),
                0x1D: V1(f'_mm_add_epi16(a, {h16})'), 0x0D: V1(f'_mm_sub_epi16({h16}, a)'),
                0x14: V1(f'_mm_and_si128(a, {w32})'), 0x15: V1(f'_mm_and_si128(a, {h16})'), 0x16: V1(f'_mm_and_si128(a, {b8})'),
                0x04: V1(f'_mm_or_si128(a, {w32})'), 0x05: V1(f'_mm_or_si128(a, {h16})'), 0x06: V1(f'_mm_or_si128(a, {b8})'),
                0x44: V1(f'_mm_xor_si128(a, {w32})'), 0x45: V1(f'_mm_xor_si128(a, {h16})'), 0x46: V1(f'_mm_xor_si128(a, {b8})'),
                0x7C: V1(f'_mm_cmpeq_epi32(a, {w32})'), 0x7D: V1(f'_mm_cmpeq_epi16(a, {h16})'), 0x7E: V1(f'_mm_cmpeq_epi8(a, {b8})'),
                0x4C: V1(f'_mm_cmpgt_epi32(a, {w32})'), 0x4D: V1(f'_mm_cmpgt_epi16(a, {h16})'), 0x4E: V1(f'_mm_cmpgt_epi8(a, {b8})'),
                0x5C: V1(f'_mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(int(0x80000000u))), {s32})'),
                0x5D: V1(f'_mm_cmpgt_epi16(_mm_xor_si128(a, _mm_set1_epi16(short(0x8000))), _mm_set1_epi16(short(0x{(u10 & 0xffff) ^ 0x8000:x})))'),
                0x5E: V1(f'_mm_cmpgt_epi8(_mm_xor_si128(a, _mm_set1_epi8(char(0x80))), _mm_set1_epi8(char(0x{(u10 & 0xff) ^ 0x80:x})))'),
            }
            if op8 in vri10:
                E('  ' + vri10[op8])
                return
        if op8 in ri10:
            E('  ' + ri10[op8])
            return
        if op8 in (0x7F, 0x4F, 0x5F):   # halts: leave to the interpreter when taken
            cond = {0x7F: f'{A}.w[0] == 0x{u10:x}u', 0x4F: f'int32_t({A}.w[0]) > ({i10})', 0x5F: f'{A}.w[0] > 0x{u10:x}u'}[op8]
            E(f'  if ({cond}) ' + exit_here)
            return
        op10 = w >> 22
        if 0x1D8 <= op10 <= 0x1DB:
            i8 = (w >> 14) & 0xff
            up, down = float.hex(math.ldexp(1.0, 173 - i8)), float.hex(math.ldexp(1.0, -(155 - i8)))
            ex = {0x1D8: f'cflts(f(a, i), {up})', 0x1D9: f'cfltu(f(a, i), {up})',
                  0x1DA: f'csflt(a.w[i], {down})', 0x1DB: f'cuflt(a.w[i], {down})'}[op10]
            E('  ' + W(ex))
            return
        op11 = w >> 21
        i7 = sx((w >> 14) & 0x7f, 7)
        Bq = f'r[{rb}]'

        def W2(expr):
            return f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 4; ++i) o.w[i] = {expr}; {T} = o; }}'

        def WT(expr):   # uses the old value of rt
            return f'{{ const Q a = {A}, bb = {Bq}, tt = {T}; Q o; for (int i = 0; i < 4; ++i) o.w[i] = {expr}; {T} = o; }}'

        def H2(expr):
            return f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 8; ++i) HR(o, i) = uint16_t({expr.replace('h(', 'hr(')}); {T} = o; }}'

        def B2(expr):
            return f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 16; ++i) BR(o, i) = uint8_t({expr.replace('b(a, i)', 'br(a, i)').replace('b(bb, i)', 'br(bb, i)')}); {T} = o; }}'

        def F2(expr):
            return f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 4; ++i) o.w[i] = fb({expr}); {T} = o; }}'

        def QQ(expr):   # whole-quadword result from a and bb
            return f'{{ const Q a = {A}, bb = {Bq}; {T} = {expr}; }}'
        u7 = i7 & 0xffffffff
        rr = {
            0x001: '', 0x201: '', 0x002: '', 0x003: '', 0x10C: '', 0x3BA: '', 0x1AC: '', 0x1AA: '',
            0x00C: f'{T} = Q{{}};', 0x398: f'{T} = Q{{}};',
            0x00D: f'{{ uint32_t v = 0; if (!s.readChPub({ra}, v)) {exit_here} {T} = pref(v); }}',
            0x00F: f'{T} = pref(s.chCount({ra}));',
            0x10D: f'if (!s.writeChPub({ra}, {T}.w[0])) {exit_here}',
            0x1C4: f'{T} = ldq(ls, {A}.w[0] + {Bq}.w[0]);',
            0x144: f'stq(ls, {A}.w[0] + {Bq}.w[0], {T});',
            0x1F4: f'{T} = insmask({A}.w[0] + 0x{u7:x}u, 1);',
            0x1F5: f'{T} = insmask({A}.w[0] + 0x{u7:x}u, 2);',
            0x1F6: f'{T} = insmask({A}.w[0] + 0x{u7:x}u, 4);',
            0x1F7: f'{T} = insmask({A}.w[0] + 0x{u7:x}u, 8);',
            0x1D4: f'{T} = insmask({A}.w[0] + {Bq}.w[0], 1);',
            0x1D5: f'{T} = insmask({A}.w[0] + {Bq}.w[0], 2);',
            0x1D6: f'{T} = insmask({A}.w[0] + {Bq}.w[0], 4);',
            0x1D7: f'{T} = insmask({A}.w[0] + {Bq}.w[0], 8);',
            0x1A8: f'return {A}.w[0] & 0x3fffc;',
            0x1A9: f'{{ const uint32_t tg = {A}.w[0] & 0x3fffc; {T} = pref(0x{npc:x}u); return tg; }}',
            0x1AB: f'{T} = pref(0x{npc:x}u);',
            0x128: f'if ({T}.w[0] == 0) return {A}.w[0] & 0x3fffc;',
            0x129: f'if ({T}.w[0] != 0) return {A}.w[0] & 0x3fffc;',
            0x12A: f'if (({T}.w[0] & 0xffff) == 0) return {A}.w[0] & 0x3fffc;',
            0x12B: f'if (({T}.w[0] & 0xffff) != 0) return {A}.w[0] & 0x3fffc;',
            0x0C0: W2('a.w[i] + bb.w[i]'),
            0x0C8: H2('h(a, i) + h(bb, i)'),
            0x040: W2('bb.w[i] - a.w[i]'),
            0x048: H2('h(bb, i) - h(a, i)'),
            0x0C2: W2('uint32_t((uint64_t(a.w[i]) + bb.w[i]) >> 32)'),
            0x042: W2('bb.w[i] >= a.w[i] ? 1u : 0u'),
            0x340: WT('a.w[i] + bb.w[i] + (tt.w[i] & 1)'),
            0x341: WT('bb.w[i] + ~a.w[i] + (tt.w[i] & 1)'),
            0x342: WT('uint32_t((uint64_t(a.w[i]) + bb.w[i] + (tt.w[i] & 1)) >> 32)'),
            0x343: WT('uint32_t((uint64_t(bb.w[i]) + uint32_t(~a.w[i]) + (tt.w[i] & 1)) >> 32)'),
            0x3C4: W2('uint32_t(int32_t(int16_t(a.w[i])) * int32_t(int16_t(bb.w[i])))'),
            0x3CC: W2('(a.w[i] & 0xffff) * (bb.w[i] & 0xffff)'),
            0x3C5: W2('((a.w[i] >> 16) * (bb.w[i] & 0xffff)) << 16'),
            0x3C7: W2('uint32_t((int32_t(int16_t(a.w[i])) * int32_t(int16_t(bb.w[i]))) >> 16)'),
            0x3C6: W2('uint32_t(int32_t(int16_t(a.w[i] >> 16)) * int32_t(int16_t(bb.w[i] >> 16)))'),
            0x346: WT('tt.w[i] + uint32_t(int32_t(int16_t(a.w[i] >> 16)) * int32_t(int16_t(bb.w[i] >> 16)))'),
            0x3CE: W2('(a.w[i] >> 16) * (bb.w[i] >> 16)'),
            0x34E: WT('tt.w[i] + (a.w[i] >> 16) * (bb.w[i] >> 16)'),
            0x2A5: W2('a.w[i] ? uint32_t(__builtin_clz(a.w[i])) : 32u'),
            0x2B4: B2('__builtin_popcount(b(a, i))'),
            0x1B6: f'{{ const uint32_t m = {A}.w[0] & 0xffff; Q o; for (int i = 0; i < 16; ++i) B(o, i) = (m >> (15 - i)) & 1 ? 0xff : 0; {T} = o; }}',
            0x1B5: f'{{ const uint32_t m = {A}.w[0] & 0xff; Q o; for (int i = 0; i < 8; ++i) H(o, i) = (m >> (7 - i)) & 1 ? 0xffff : 0; {T} = o; }}',
            0x1B4: f'{{ const uint32_t m = {A}.w[0] & 0xf; Q o; for (int i = 0; i < 4; ++i) o.w[i] = (m >> (3 - i)) & 1 ? ~0u : 0u; {T} = o; }}',
            0x1B2: f'{T} = gather_b({A});',
            0x1B1: f'{T} = gather_h({A});',
            0x1B0: f'{T} = gather_w({A});',
            0x0D3: B2('(b(a, i) + b(bb, i) + 1) >> 1'),
            0x053: B2('b(a, i) > b(bb, i) ? b(a, i) - b(bb, i) : b(bb, i) - b(a, i)'),
            0x253: f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 4; ++i) {{ uint32_t sa = 0, sb = 0; for (int k = 0; k < 4; ++k) {{ sa += b(a, 4 * i + k); sb += b(bb, 4 * i + k); }} H(o, 2 * i) = uint16_t(sb); H(o, 2 * i + 1) = uint16_t(sa); }} {T} = o; }}',
            0x2B6: f'{{ const Q a = {A}; Q o; for (int i = 0; i < 8; ++i) H(o, i) = uint16_t(int16_t(int8_t(b(a, 2 * i + 1)))); {T} = o; }}',
            0x2AE: f'{{ const Q a = {A}; Q o; for (int i = 0; i < 4; ++i) o.w[i] = uint32_t(int32_t(int16_t(a.w[i]))); {T} = o; }}',
            0x2A6: f'{{ const Q a = {A}; Q o; for (int i = 0; i < 2; ++i) sdw(o, i, uint64_t(int64_t(int32_t(a.w[2 * i + 1])))); {T} = o; }}',
            0x0C1: W2('a.w[i] & bb.w[i]'),
            0x2C1: W2('a.w[i] & ~bb.w[i]'),
            0x041: W2('a.w[i] | bb.w[i]'),
            0x2C9: W2('a.w[i] | ~bb.w[i]'),
            0x1F0: f'{{ const Q a = {A}; {T} = pref(a.w[0] | a.w[1] | a.w[2] | a.w[3]); }}',
            0x241: W2('a.w[i] ^ bb.w[i]'),
            0x0C9: W2('~(a.w[i] & bb.w[i])'),
            0x049: W2('~(a.w[i] | bb.w[i])'),
            0x249: W2('~(a.w[i] ^ bb.w[i])'),
            0x05B: W2('(bb.w[i] & 0x3f) > 31 ? 0u : a.w[i] << (bb.w[i] & 0x3f)'),
            0x05F: H2('(h(bb, i) & 0x1f) > 15 ? 0 : h(a, i) << (h(bb, i) & 0x1f)'),
            0x07B: W2(f'{u7 & 0x3f} > 31 ? 0u : a.w[i] << {u7 & 0x3f if (u7 & 0x3f) < 32 else 0}'),
            0x07F: H2(f'{u7 & 0x1f} > 15 ? 0 : h(a, i) << {u7 & 0x1f if (u7 & 0x1f) < 16 else 0}'),
            0x058: W2('(a.w[i] << (bb.w[i] & 31)) | ((bb.w[i] & 31) ? a.w[i] >> (32 - (bb.w[i] & 31)) : 0u)'),
            0x05C: H2('(h(a, i) << (h(bb, i) & 15)) | ((h(bb, i) & 15) ? h(a, i) >> (16 - (h(bb, i) & 15)) : 0)'),
            0x078: W2(f'(a.w[i] << {u7 & 31}) | ({u7 & 31} ? a.w[i] >> ({(32 - (u7 & 31)) & 31}) : 0u)'),
            0x07C: H2(f'(h(a, i) << {u7 & 15}) | ({u7 & 15} ? h(a, i) >> ({(16 - (u7 & 15)) & 15}) : 0)'),
            0x059: W2('(uint32_t(-int32_t(bb.w[i])) & 0x3f) > 31 ? 0u : a.w[i] >> (uint32_t(-int32_t(bb.w[i])) & 0x3f)'),
            0x05D: H2('(uint32_t(-int32_t(h(bb, i))) & 0x1f) > 15 ? 0 : h(a, i) >> (uint32_t(-int32_t(h(bb, i))) & 0x1f)'),
            0x079: W2(f'{(-i7) & 0x3f} > 31 ? 0u : a.w[i] >> {(-i7) & 0x3f if ((-i7) & 0x3f) < 32 else 0}'),
            0x07D: H2(f'{(-i7) & 0x1f} > 15 ? 0 : h(a, i) >> {(-i7) & 0x1f if ((-i7) & 0x1f) < 16 else 0}'),
            0x05A: W2('uint32_t(int32_t(a.w[i]) >> std::min<uint32_t>(uint32_t(-int32_t(bb.w[i])) & 0x3f, 31))'),
            0x05E: H2('int16_t(h(a, i)) >> std::min<uint32_t>(uint32_t(-int32_t(h(bb, i))) & 0x1f, 15)'),
            0x07A: W2(f'uint32_t(int32_t(a.w[i]) >> {min((-i7) & 0x3f, 31)})'),
            0x07E: H2(f'int16_t(h(a, i)) >> {min((-i7) & 0x1f, 15)}'),
            0x1DF: QQ('qshl_bytes(a, int(bb.w[0] & 0x1f))'),
            0x1FF: (f'{T} = {A};' if (u7 & 0x1f) == 0 else QQ(f'qshl_bytes(a, {u7 & 0x1f})')),
            0x1CF: QQ('qshl_bytes(a, int((bb.w[0] >> 3) & 0x1f))'),
            0x1DC: QQ('qrot_bytes(a, int(bb.w[0] & 0xf))'),
            0x1FC: QQ(f'qrot_bytes(a, {u7 & 0xf})'),
            0x1CC: QQ('qrot_bytes(a, int((bb.w[0] >> 3) & 0xf))'),
            0x1DD: QQ('qshr_bytes(a, int(uint32_t(-int32_t(bb.w[0])) & 0x1f))'),
            0x1FD: QQ(f'qshr_bytes(a, {(-i7) & 0x1f})'),
            0x1CD: QQ('qshr_bytes(a, int(uint32_t(-int32_t(bb.w[0] >> 3)) & 0x1f))'),
            0x1DB: QQ('from128(q128(a) << (bb.w[0] & 7))'),
            0x1FB: QQ(f'from128(q128(a) << {u7 & 7})'),
            0x1D8: QQ('from128((bb.w[0] & 7) ? (q128(a) << (bb.w[0] & 7)) | (q128(a) >> (128 - (bb.w[0] & 7))) : q128(a))'),
            0x1F8: QQ(f'from128({u7 & 7} ? (q128(a) << {u7 & 7}) | (q128(a) >> {(128 - (u7 & 7)) & 127}) : q128(a))'),
            0x1D9: QQ('from128(q128(a) >> (uint32_t(-int32_t(bb.w[0])) & 7))'),
            0x1F9: QQ(f'from128(q128(a) >> {(-i7) & 7})'),
            0x3C0: W2('a.w[i] == bb.w[i] ? ~0u : 0u'),
            0x3C8: H2('h(a, i) == h(bb, i) ? 0xffff : 0'),
            0x3D0: B2('b(a, i) == b(bb, i) ? 0xff : 0'),
            0x240: W2('int32_t(a.w[i]) > int32_t(bb.w[i]) ? ~0u : 0u'),
            0x248: H2('int16_t(h(a, i)) > int16_t(h(bb, i)) ? 0xffff : 0'),
            0x250: B2('int8_t(b(a, i)) > int8_t(b(bb, i)) ? 0xff : 0'),
            0x2C0: W2('a.w[i] > bb.w[i] ? ~0u : 0u'),
            0x2C8: H2('h(a, i) > h(bb, i) ? 0xffff : 0'),
            0x2D0: B2('b(a, i) > b(bb, i) ? 0xff : 0'),
            0x2C4: F2('f(a, i) + f(bb, i)'),
            0x2C5: F2('f(a, i) - f(bb, i)'),
            0x2C6: F2('spu_mul(f(a, i), f(bb, i))'),
            0x3C2: W2('f(a, i) == f(bb, i) ? ~0u : 0u'),
            0x2C2: W2('f(a, i) > f(bb, i) ? ~0u : 0u'),
            0x3CA: W2('std::fabs(f(a, i)) == std::fabs(f(bb, i)) ? ~0u : 0u'),
            0x2CA: W2('std::fabs(f(a, i)) > std::fabs(f(bb, i)) ? ~0u : 0u'),
            0x1B8: F2('1.0f / f(a, i)'),
            0x1B9: F2('1.0f / std::sqrt(std::fabs(f(a, i)))'),
            0x3D4: f'{T} = {Bq};',
            0x2CC: f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 2; ++i) sd(o, i, d(a, i) + d(bb, i)); {T} = o; }}',
            0x2CD: f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 2; ++i) sd(o, i, d(a, i) - d(bb, i)); {T} = o; }}',
            0x2CE: f'{{ const Q a = {A}, bb = {Bq}; Q o; for (int i = 0; i < 2; ++i) sd(o, i, d(a, i) * d(bb, i)); {T} = o; }}',
            0x3B8: f'{{ const Q a = {A}; Q o; for (int i = 0; i < 2; ++i) sd(o, i, double(f(a, 2 * i))); {T} = o; }}',
            0x3B9: f'{{ const Q a = {A}; Q o{{}}; for (int i = 0; i < 2; ++i) o.w[2 * i] = fb(float(d(a, i))); {T} = o; }}',
        }
        if op11 in (0x000, 0x140, 0x3D8, 0x258, 0x2D8):
            if op11 in (0x000, 0x140):
                E('  ' + exit_here)
            else:
                cond = {0x3D8: f'{A}.w[0] == {Bq}.w[0]', 0x258: f'int32_t({A}.w[0]) > int32_t({Bq}.w[0])', 0x2D8: f'{A}.w[0] > {Bq}.w[0]'}[op11]
                E(f'  if ({cond}) ' + exit_here)
            return
        if VEC:
            def V2(expr):
                return f'{{ const __m128i a = V({A}), b = V({Bq}); {T} = mk({expr}); }}'
            ones, S = '_mm_set1_epi32(-1)', '_mm_set1_epi32(int(0x80000000u))'
            ugt = lambda x, y: f'_mm_cmpgt_epi32(_mm_xor_si128({x}, {S}), _mm_xor_si128({y}, {S}))'
            n_shl, n_shr = u7 & 0x3f, (-i7) & 0x3f
            h_shl, h_shr = u7 & 0x1f, (-i7) & 0x1f
            rot = u7 & 31
            vrr = {
                0x0C0: V2('_mm_add_epi32(a, b)'), 0x040: V2('_mm_sub_epi32(b, a)'),
                0x0C8: V2('_mm_add_epi16(a, b)'), 0x048: V2('_mm_sub_epi16(b, a)'),
                0x0C2: V2('_mm_srli_epi32(' + ugt('a', '_mm_add_epi32(a, b)') + ', 31)'),
                0x042: V2('_mm_andnot_si128(' + ugt('a', 'b') + ', _mm_set1_epi32(1))'),
                0x0C1: V2('_mm_and_si128(a, b)'), 0x2C1: V2('_mm_andnot_si128(b, a)'),
                0x041: V2('_mm_or_si128(a, b)'), 0x2C9: V2(f'_mm_or_si128(a, _mm_xor_si128(b, {ones}))'),
                0x241: V2('_mm_xor_si128(a, b)'), 0x0C9: V2(f'_mm_xor_si128(_mm_and_si128(a, b), {ones})'),
                0x049: V2(f'_mm_xor_si128(_mm_or_si128(a, b), {ones})'), 0x249: V2(f'_mm_xor_si128(_mm_xor_si128(a, b), {ones})'),
                0x05B: V2('_mm_sllv_epi32(a, _mm_and_si128(b, _mm_set1_epi32(0x3f)))'),
                0x059: V2('_mm_srlv_epi32(a, _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), b), _mm_set1_epi32(0x3f)))'),
                0x05A: V2('_mm_srav_epi32(a, _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), b), _mm_set1_epi32(0x3f)))'),
                0x07B: V2('_mm_setzero_si128()' if n_shl > 31 else f'_mm_slli_epi32(a, {n_shl})'),
                0x079: V2('_mm_setzero_si128()' if n_shr > 31 else f'_mm_srli_epi32(a, {n_shr})'),
                0x07A: V2(f'_mm_srai_epi32(a, {min(n_shr, 31)})'),
                0x07F: V2('_mm_setzero_si128()' if h_shl > 15 else f'_mm_slli_epi16(a, {h_shl})'),
                0x07D: V2('_mm_setzero_si128()' if h_shr > 15 else f'_mm_srli_epi16(a, {h_shr})'),
                0x07E: V2(f'_mm_srai_epi16(a, {min(h_shr, 15)})'),
                0x078: V2('a' if rot == 0 else f'_mm_or_si128(_mm_slli_epi32(a, {rot}), _mm_srli_epi32(a, {32 - rot}))'),
                0x3C0: V2('_mm_cmpeq_epi32(a, b)'), 0x3C8: V2('_mm_cmpeq_epi16(a, b)'), 0x3D0: V2('_mm_cmpeq_epi8(a, b)'),
                0x240: V2('_mm_cmpgt_epi32(a, b)'), 0x248: V2('_mm_cmpgt_epi16(a, b)'), 0x250: V2('_mm_cmpgt_epi8(a, b)'),
                0x2C0: V2(ugt('a', 'b')),
                0x2C8: V2('_mm_cmpgt_epi16(_mm_xor_si128(a, _mm_set1_epi16(short(0x8000))), _mm_xor_si128(b, _mm_set1_epi16(short(0x8000))))'),
                0x2D0: V2('_mm_cmpgt_epi8(_mm_xor_si128(a, _mm_set1_epi8(char(0x80))), _mm_xor_si128(b, _mm_set1_epi8(char(0x80))))'),
                0x2C4: V2('IS(_mm_add_ps(PS(a), PS(b)))'), 0x2C5: V2('IS(_mm_sub_ps(PS(a), PS(b)))'),
                0x2C6: V2('IS(vmul_spu(PS(a), PS(b)))'),
                0x3C2: V2('IS(_mm_cmpeq_ps(PS(a), PS(b)))'), 0x2C2: V2('IS(_mm_cmpgt_ps(PS(a), PS(b)))'),
                0x3CA: V2('IS(_mm_cmpeq_ps(PS(vabs(a)), PS(vabs(b))))'), 0x2CA: V2('IS(_mm_cmpgt_ps(PS(vabs(a)), PS(vabs(b))))'),
            }
            if op11 in vrr:
                E('  ' + vrr[op11])
                return
        if op11 in rr:
            if rr[op11]:
                E('  ' + rr[op11])
            return
        E('  ' + exit_here)   # unknown word (often data): the interpreter reports it if it is ever executed
    for k in range(n):
        starts.append(len(lines))
        emit_one(k)
        if slow:
            E(f'  if (single) return 0x{(base + 4 * k + 4) & 0x3ffff:x}u;')
    starts.append(len(lines))
    return lines, starts


VEC = os.environ.get('SPUVEC', '1') != '0'   # whole-quadword SSE/AVX2 templates for the common ops


def gen(read, ea, size, base):
    code = read(ea, size)
    n = size // 4
    end = base + size
    leaders = leaders_of(code, base, size)
    # entry points found at run time (indirect branch targets the static scan cannot see): tools/spu_leaders/<ea>.txt,
    # one hex LS address per line, written from the [spuslow] report of SPUPROF=1
    extra = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spu_leaders', f'{ea:x}.txt')
    if os.path.exists(extra):
        for line in open(extra):
            line = line.split('#')[0].strip()
            if line and base <= int(line, 16) < end:
                leaders.add(int(line, 16) & ~3)
    fn = f'spu_{ea:x}'
    nch = (size + CH - 1) // CH
    files = []
    per_file = 8
    for slow in (False, True):
        lines, starts = gen_lines(code, base, size, slow, leaders)
        tag = 's' if slow else 'c'
        for f0 in range(0, nch, per_file):
            out = [f'// Generated by tools/spu2cpp.py from the SPU program at EA 0x{ea:x}'
                   f' ({"slow variant: every instruction is an entry point" if slow else "fast variant"}). Do not edit.',
                   '#include "recomp/spu_ops.h"', '#include <algorithm>', '', 'namespace gti::recomp::spurt {', '']
            for c in range(f0, min(nch, f0 + per_file)):
                k0, k1 = c * CH // 4, min(n, (c + 1) * CH // 4)
                if slow:
                    out.append(f'uint32_t {fn}_s{c}(Q* __restrict r, Spu& s, uint32_t pc, bool single) {{')
                else:
                    out.append(f'uint32_t {fn}_c{c}(Q* __restrict r, Spu& s, uint32_t pc) {{')
                out.append('    uint8_t* const ls = s.ls; (void)ls;')
                out.append('    switch (pc) {')
                for k in range(k0, k1):
                    if slow or (base + 4 * k) in leaders:
                        out.append(f'    case 0x{base + 4 * k:x}: goto L_{base + 4 * k:x};')
                out.append(f'    default: return 0x{0x80000000 if slow else 0x40000000:x}u | pc;')
                out.append('    }')
                out += lines[starts[k0]:starts[k1]]
                out.append(f'    return 0x{base + 4 * k1:x}u;')
                out.append('}')
                out.append('')
            out += ['} // namespace gti::recomp::spurt', '']
            files.append((f'{fn}_{tag}{f0 // per_file}.cpp', '\n'.join(out)))
    main = [f'// Generated by tools/spu2cpp.py from the SPU program at EA 0x{ea:x} ({size} bytes, LS base 0x{base:x},'
            f' {len(leaders)} entry points in {n} instructions). Do not edit.',
            '#include "recomp/spu_rt.h"', '', 'namespace gti::recomp::spurt {', '']
    for c in range(nch):
        main.append(f'uint32_t {fn}_c{c}(Q* __restrict r, Spu& s, uint32_t pc);')
        main.append(f'uint32_t {fn}_s{c}(Q* __restrict r, Spu& s, uint32_t pc, bool single);')
    main.append(f'static const FastChunk k_{fn}_fast[] = {{')
    main += [f'    {fn}_c{c},' for c in range(nch)]
    main += ['};', f'extern const SlowChunk k_{fn}_slow[];', f'const SlowChunk k_{fn}_slow[] = {{']
    main += [f'    {fn}_s{c},' for c in range(nch)]
    main += ['};', '',
             f'void {fn}(Spu& s) {{',
             '    alignas(16) Q r[128];',
             '    for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) r[i].w[k] = s.r[i].w(k);',
             '    uint32_t pc = s.pc;',
             '    uint64_t n = 0;',
             f'    while (pc >= 0x{base:x}u && pc < 0x{end:x}u) {{',
             '        ++n;',
             f'        if (HleHook h = g_spuHle[(0x{ea:x}u >> 4) % 64]; h && spu_hle_wants((0x{ea:x}u >> 4) % 64, pc)) {{ pc = h(r, s, pc); if (pc < 0x{base:x}u || pc >= 0x{end:x}u) break; }}',
             f'        const uint32_t c = (pc - 0x{base:x}u) >> 10;',
             f'        if (g_spuProf) ++g_spuProfCnt[(0x{ea:x}u >> 4) % 64][c & 255];',
             f'        pc = k_{fn}_fast[c](r, s, pc);',
             '        if (pc & 0x40000000u) {   // not an entry point of the fast chunk',
             '            ++g_spuSlowEntries;',
             f'            if (g_spuProf) spu_slow_note(0x{ea:x}u, pc & 0x3fffffffu);',
             f'            pc = k_{fn}_slow[c](r, s, pc & 0x3fffffffu, false);',
             '        }',
             '        if (pc & 0x80000000u) { pc &= 0x7fffffffu; break; }',
             '    }',
             '    s.pc = pc;',
             '    s.steps += n * 16;',
             '    for (int i = 0; i < 128; ++i) for (int k = 0; k < 4; ++k) s.r[i].sw(k, r[i].w[k]);',
             '}', '', '} // namespace gti::recomp::spurt', '']
    files.append((f'{fn}.cpp', '\n'.join(main)))
    return fn, files


def main():
    elf, outdir = sys.argv[1], sys.argv[2]
    read = load_elf(elf)
    os.makedirs(outdir, exist_ok=True)
    for old in os.listdir(outdir):   # chunk files of a previous run
        if old.startswith('spu_') and old.endswith('.cpp'):
            os.remove(os.path.join(outdir, old))
    table = []
    for spec in sys.argv[3:]:
        parts = spec.split(':')
        if parts[0] == 'file':   # file:path:offset:size:lsbase:id - code that is not in the EBOOT (a module loaded at run time); id names it
            data = open(parts[1], 'rb').read()
            foff, size, base, ea = int(parts[2], 0), int(parts[3], 0), int(parts[4], 16), int(parts[5], 16)
            fn, files = gen(lambda va, n, d=data, o=foff, e=ea: d[o + va - e: o + va - e + n], ea, size, base)
        else:
            ea, size = int(parts[0], 16), int(parts[1], 0)
            base = int(parts[2], 16) if len(parts) > 2 else 0x3000
            fn, files = gen(read, ea, size, base)
        for name, src in files:
            open(os.path.join(outdir, name), 'w').write(src)
        table.append((ea, base, size, fn))
        print(f'{fn}: {size // 4} instructions')
    t = ['// Generated by tools/spu2cpp.py. Do not edit.', '#include "recomp/spu_rt.h"', '',
         'namespace gti::recomp::spurt {', '']
    for (ea, base, size, fn) in table:
        t.append(f'void {fn}(Spu& s);')
        t.append(f'extern const SlowChunk k_{fn}_slow[];')
    t += ['', 'bool g_spuProf = false;', 'uint64_t g_spuProfCnt[64][256];', 'uint64_t g_spuSlowEntries = 0;', 'HleHook g_spuHle[64];', 'uint32_t g_spuHlePcs[64][8];']
    t += ['', 'static const Compiled k_table[] = {']
    for (ea, base, size, fn) in table:
        t.append(f'    {{0x{ea:x}, 0x{base:x}, 0x{size:x}, {fn}, k_{fn}_slow}},')
    t += ['};', '',
          'const Compiled* find_compiled(uint32_t eaBin) {',
          '    for (const Compiled& c : k_table) if (c.eaBin == eaBin) return &c;',
          '    return nullptr;',
          '}', '',
          'const Compiled* compiled_at(int index) {',
          '    return index >= 0 && index < int(sizeof k_table / sizeof k_table[0]) ? &k_table[index] : nullptr;',
          '}', '', '} // namespace gti::recomp::spurt', '']
    open(os.path.join(outdir, 'spu_table.cpp'), 'w').write('\n'.join(t))


if __name__ == '__main__':
    main()
