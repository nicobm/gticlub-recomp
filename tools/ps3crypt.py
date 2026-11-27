"""PS3 package extraction and NPDRM executable decryption for Racing Club+ (no emulator needed).

The user's own game: GTI Club+ (NPEB00060) .pkg files + their .rap licences -> plain files + EBOOT.elf.
Algorithms and public constants as in RPCS3 (GPLv2, rpcs3/Crypto: unpkg.cpp, unself.cpp, key_vault.cpp/.h).
AES comes from Windows (bcrypt.dll through ctypes): no Python package to install, and fast enough for ~500 MB.

usage:  python ps3crypt.py list <file.pkg>
        python ps3crypt.py extract <file.pkg> <out folder>          (files go to <out>/<TITLE_ID>/...)
        python ps3crypt.py self <EBOOT.BIN> <EBOOT.elf> <licence.rap>
"""
import ctypes, os, struct, sys, zlib
from ctypes import wintypes

# ------------------------------------------------------------------ AES (Windows CNG)
_bc = ctypes.WinDLL('bcrypt')
_bc.BCryptOpenAlgorithmProvider.argtypes = [ctypes.POINTER(ctypes.c_void_p), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.ULONG]
_bc.BCryptSetProperty.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG]
_bc.BCryptGenerateSymmetricKey.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, wintypes.ULONG, ctypes.c_char_p, wintypes.ULONG, wintypes.ULONG]
for _f in (_bc.BCryptEncrypt, _bc.BCryptDecrypt):
    _f.argtypes = [ctypes.c_void_p, ctypes.c_char_p, wintypes.ULONG, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
_bc.BCryptDestroyKey.argtypes = [ctypes.c_void_p]
_algs = {}


def _alg(mode):
    if mode not in _algs:
        h = ctypes.c_void_p()
        if _bc.BCryptOpenAlgorithmProvider(ctypes.byref(h), 'AES', None, 0): raise OSError('BCrypt AES not available')
        m = ctypes.create_unicode_buffer(mode)
        if _bc.BCryptSetProperty(h, 'ChainingMode', m, ctypes.sizeof(m), 0): raise OSError('BCrypt chaining mode')
        _algs[mode] = h
    return _algs[mode]


def _aes(mode, key, data, iv=None, decrypt=False):
    k = ctypes.c_void_p()
    if _bc.BCryptGenerateSymmetricKey(_alg(mode), ctypes.byref(k), None, 0, bytes(key), len(key), 0): raise OSError('BCrypt key')
    try:
        out = ctypes.create_string_buffer(len(data))
        n = wintypes.ULONG()
        ivb = ctypes.create_string_buffer(bytes(iv), 16) if iv is not None else None
        fn = _bc.BCryptDecrypt if decrypt else _bc.BCryptEncrypt
        if fn(k, bytes(data), len(data), None, ivb, 16 if ivb is not None else 0, out, len(data), ctypes.byref(n), 0): raise OSError('BCrypt AES')
        return out.raw[:n.value]
    finally:
        _bc.BCryptDestroyKey(k)


def ecb_enc(key, data): return _aes('ChainingModeECB', key, data)
def ecb_dec(key, data): return _aes('ChainingModeECB', key, data, decrypt=True)
def cbc_dec(key, iv, data): return _aes('ChainingModeCBC', key, data, iv, decrypt=True)


def ctr(key, counter, data):
    """AES-CTR with a 128-bit big-endian counter starting at `counter` (int)."""
    out = []
    for p in range(0, len(data), 1 << 20):
        chunk = data[p:p + (1 << 20)]
        nb = (len(chunk) + 15) // 16
        c0 = counter + p // 16
        ctrs = b''.join(((c0 + i) & ((1 << 128) - 1)).to_bytes(16, 'big') for i in range(nb))
        ks = ecb_enc(key, ctrs)[:len(chunk)]
        out.append((int.from_bytes(chunk, 'big') ^ int.from_bytes(ks, 'big')).to_bytes(len(chunk), 'big'))
    return b''.join(out)


# ------------------------------------------------------------------ packages
PKG_AES_KEY = bytes.fromhex('2e7b71d7c9c9a14ea3221f188828b8f8')
PKG_AES_KEY_IDU = bytes.fromhex('5db911e6b7e50a7d321538fd7c66f17b')


class Pkg:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0xC0)
        if h[:4] != b'\x7fPKG': raise ValueError('%s is not a PS3 package' % path)
        self.release, self.platform = struct.unpack('>HH', h[4:8])
        if self.release != 0x8000 or self.platform != 1: raise ValueError('%s: only retail PS3 packages are supported' % path)
        self.file_count = struct.unpack('>I', h[0x14:0x18])[0]
        self.data_offset, self.data_size = struct.unpack('>QQ', h[0x20:0x30])
        self.content_id = h[0x30:0x60].split(b'\0')[0].decode('ascii', 'replace')
        self.nonce = int.from_bytes(h[0x70:0x80], 'big')
        self.key = PKG_AES_KEY
        self.entries = self._entries()
        if self.entries is None:
            self.key = PKG_AES_KEY_IDU
            self.entries = self._entries()
        if self.entries is None: raise ValueError('%s: the package table cannot be read' % path)

    def read(self, off, size):   # decrypted bytes of the data region
        a = off & ~15
        self.f.seek(self.data_offset + a)
        raw = self.f.read(off + size - a)
        raw += b'\0' * (-len(raw) % 16)
        return ctr(self.key, self.nonce + a // 16, raw)[off - a:off - a + size]

    def _entries(self):
        tab = self.read(0, self.file_count * 0x20)
        out = []
        for i in range(self.file_count):
            no, ns, fo, fs, ty = struct.unpack('>IIQQI', tab[i * 0x20:i * 0x20 + 0x1C])
            if no + ns > self.data_size or fo + fs > self.data_size: return None
            name = self.read(no, ns).split(b'\0')[0]
            try: name = name.decode('utf-8')
            except UnicodeDecodeError: return None
            out.append((name, fo, fs, ty))
        return out

    def extract(self, out, progress=None):
        title = self.content_id[7:16] if len(self.content_id) >= 16 else 'GAME'
        root = os.path.join(out, title)
        total = sum(e[2] for e in self.entries if (e[3] & 0xff) not in (4, 0x12)) or 1
        done = 0
        for name, fo, fs, ty in self.entries:
            dst = os.path.join(root, *name.replace('\\', '/').split('/'))
            if (ty & 0xff) in (4, 0x12):
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as w:
                for p in range(0, fs, 16 << 20):
                    w.write(self.read(fo + p, min(16 << 20, fs - p)))
                    if progress: progress(done + min(fs, p + (16 << 20)), total)
            done += fs
        return root


# ------------------------------------------------------------------ NPDRM SELF -> ELF
RAP_KEY = bytes.fromhex('869f7745c13fd890ccf29188e3cc3edf')
RAP_PBOX = bytes.fromhex('0c030604010b0f0802070005 0a0e0d09'.replace(' ', ''))
RAP_E1 = bytes.fromhex('a93e1fd67c55a329b75fdda62a95c7a5')
RAP_E2 = bytes.fromhex('67d45da3296d006a4e7c537bf5538c74')
NP_KLIC_FREE = bytes.fromhex('72f990788f9cff745725f08e4c128387')
NP_KLIC_KEY = bytes.fromhex('f2fbca7a75b04edc1390638ccdfdd1ee')
NPDRM_KEYS = {   # revision -> (erk, riv)
    0x0001: ('F9EDD0301F770FABBA8863D9897F0FEA6551B09431F61312654E28F43533EA6B', 'A551CCB4A42C37A734A2B4F9657D5540'),
    0x0002: ('8E737230C80E66AD0162EDDD32F1F774EE5E4E187449F19079437A508FCF9C86', '7AAECC60AD12AED90C348D8C11D2BED5'),
    0x0003: ('1B715B0C3E8DC4C1A5772EBA9C5D34F7CCFE5B82025D453F3167566497239664', 'E31E206FBB8AEA27FAB0D9A2FFB6B62F'),
    0x0004: ('BB4DBF66B744A33934172D9F8379A7A5EA74CB0F559BB95D0E7AECE91702B706', 'ADF7B207A15AC601110E61DDFC210AF6'),
    0x0006: ('8B4C52849765D2B5FA3D5628AFB17644D52B9FFEE235B4C0DB72A62867EAA020', '05719DF1B1D0306C03910ADDCE4AF887'),
    0x0007: ('3946DFAA141718C7BE339A0D6C26301C76B568AEBC5CD52652F2E2E0297437C3', 'E4897BE553AE025CDCBF2B15D1C9234E'),
    0x0009: ('0786F4B0CA5937F515BDCE188F569B2EF3109A4DA0780A7AA07BD89C3350810A', '04AD3C2F122A3B35E804850CAD142C6D'),
    0x000A: ('03C21AD78FBB6A3D425E9AAB1298F9FD70E29FD4E6E3A3C151205DA50C413DE4', '0A99D4D4F8301A88052D714AD2FB565E'),
    0x000C: ('357EBBEA265FAEC271182D571C6CD2F62CFA04D325588F213DB6B2E0ED166D92', 'D26E6DD2B74CD78E866E742E5571B84F'),
    0x000D: ('337A51416105B56E40D7CAF1B954CDAF4E7645F28379904F35F27E81CA7B6957', '8405C88E042280DBD794EC7E22B74002'),
    0x000F: ('135C098CBE6A3E037EBE9F2BB9B30218DDE8D68217346F9AD33203352FBB3291', '4070C898C2EAAD1634A288AA547A35A8'),
    0x0010: ('4B3CD10F6A6AA7D99F9B3A660C35ADE08EF01C2C336B9E46D1BB5678B4261A61', 'C0F2AB86E6E0457552DB50D7219371C5'),
    0x0013: ('265C93CF48562EC5D18773BEB7689B8AD10C5EB6D21421455DEBC4FB128CBF46', '8DEA5FF959682A9B98B688CEA1EF4A1D'),
    0x0016: ('7910340483E419E55F0D33E4EA5410EEEC3AF47814667ECA2AA9D75602B14D4B', '4AD981431B98DFD39B6388EDAD742A8E'),
    0x0019: ('FBDA75963FE690CFF35B7AA7B408CF631744EDEF5F7931A04D58FD6A921FFDB3', 'F72C1D80FFDA2E3BF085F4133E6D2805'),
    0x001C: ('8103EA9DB790578219C4CEDF0592B43064A7D98B601B6C7BC45108C4047AA80F', '246F4B8328BE6A2D394EDE20479247C5'),
}


def rap_to_rif(rap):
    key = bytearray(cbc_dec(RAP_KEY, b'\0' * 16, rap[:16]))
    for _ in range(5):
        for i in range(16):
            p = RAP_PBOX[i]; key[p] ^= RAP_E1[p]
        for i in range(15, 0, -1):
            p, pp = RAP_PBOX[i], RAP_PBOX[i - 1]; key[p] ^= key[pp]
        o = 0
        for i in range(16):
            p = RAP_PBOX[i]
            kc = (key[p] - o) & 0xff
            ec2 = RAP_E2[p]
            if o != 1 or kc != 0xff:
                o = 1 if kc < ec2 else 0
                key[p] = (kc - ec2) & 0xff
            elif kc == 0xff:
                key[p] = (kc - ec2) & 0xff
            else:
                key[p] = kc
    return bytes(key)


def self_to_elf(src, rap=None):
    d = open(src, 'rb').read()
    if d[:4] != b'SCE\0': raise ValueError('%s is not a PS3 executable (SELF)' % src)
    se_flags, se_type, se_meta, se_hsize = struct.unpack('>HHIQ', d[8:0x18])
    (_, prog_id_off, ehdr_off, phdr_off, _, seg_off, _, sup_off, sup_size) = struct.unpack('>9Q', d[0x20:0x68])
    # supplemental headers: the NPDRM one (type 3) says which licence
    npd_license = None
    p, end = sup_off, sup_off + sup_size
    while p + 16 <= end:
        ty, sz, nxt = struct.unpack('>IIQ', d[p:p + 16])
        if ty == 3: npd_license = struct.unpack('>I', d[p + 16 + 8:p + 16 + 12])[0]
        if not nxt or sz == 0: break
        p += sz
    meta_info = d[se_meta + 0x20:se_meta + 0x60]
    if not (se_flags & 0x8000):
        if npd_license is not None:
            if npd_license in (1, 2):
                if rap is None: raise ValueError('the game needs its licence (.rap file)')
                k = rap_to_rif(rap)
            else:
                k = NP_KLIC_FREE
            k = ecb_dec(NP_KLIC_KEY, k)
            meta_info = cbc_dec(k, b'\0' * 16, meta_info)
        if se_flags not in NPDRM_KEYS: raise ValueError('unknown key revision %#x' % se_flags)
        erk, riv = NPDRM_KEYS[se_flags]
        meta_info = cbc_dec(bytes.fromhex(erk), bytes.fromhex(riv), meta_info)
    mkey, kpad, miv, ivpad = meta_info[:16], meta_info[16:32], meta_info[32:48], meta_info[48:64]
    if kpad[0] or ivpad[0]: raise ValueError('wrong licence (.rap) for this game: the executable does not decrypt')
    hdrs = d[se_meta + 0x60:se_hsize]
    hdrs = ctr(mkey, int.from_bytes(miv, 'big'), hdrs) if not (se_flags & 0x8000) else hdrs
    _, _, nsec, nkey = struct.unpack('>QIII', hdrs[:0x14])
    secs = [struct.unpack('>QQIIIIIIII', hdrs[0x20 + i * 0x30:0x50 + i * 0x30]) for i in range(nsec)]
    keys = hdrs[0x20 + nsec * 0x30:0x20 + nsec * 0x30 + nkey * 0x10]
    # ELF: header + program headers from the SELF, then every segment decrypted/decompressed at its p_offset
    eh = bytearray(d[ehdr_off:ehdr_off + 0x40])
    phoff, = struct.unpack('>Q', eh[0x20:0x28])
    phnum, = struct.unpack('>H', eh[0x38:0x3A])
    shoff, = struct.unpack('>Q', eh[0x28:0x30])
    phdrs = [struct.unpack('>IIQQQQQQ', d[phdr_off + i * 0x38:phdr_off + (i + 1) * 0x38]) for i in range(phnum)]
    size = max([phoff + phnum * 0x38] + [ph[2] + ph[5] for ph in phdrs])
    out = bytearray(size)
    out[0:0x40] = eh
    out[phoff:phoff + phnum * 0x38] = d[phdr_off:phdr_off + phnum * 0x38]
    for (doff, dsize, ty, pidx, _, _, enc, kidx, ividx, comp) in secs:
        if ty != 2 or pidx >= phnum: continue
        buf = d[doff:doff + dsize]
        if enc == 3:
            buf = ctr(keys[kidx * 16:kidx * 16 + 16], int.from_bytes(keys[ividx * 16:ividx * 16 + 16], 'big'), buf)
        ph = phdrs[pidx]
        if comp == 2:
            buf = zlib.decompress(buf)
        out[ph[2]:ph[2] + len(buf)] = buf[:ph[5]] if len(buf) > ph[5] else buf
    # section headers (debugging info only) are copied when the SELF carries them
    if shoff:
        shnum, = struct.unpack('>H', eh[0x3C:0x3E])
        shdr_off_self, = struct.unpack('>Q', d[0x40:0x48])
        if shnum and shdr_off_self:
            sh = d[shdr_off_self:shdr_off_self + shnum * 0x40]
            if len(out) < shoff + len(sh): out.extend(b'\0' * (shoff + len(sh) - len(out)))
            out[shoff:shoff + len(sh)] = sh
    return bytes(out)


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'list':
        p = Pkg(a[1])
        print(p.content_id, '%d files' % p.file_count)
        for name, fo, fs, ty in p.entries: print('%08x %10d %s' % (ty, fs, name))
    elif len(a) >= 3 and a[0] == 'extract':
        p = Pkg(a[1])
        last = [-1]
        def prog(d, t):
            pc = d * 100 // t
            if pc // 10 != last[0]: last[0] = pc // 10; print('  %d%%' % pc, flush=True)
        print(p.extract(a[2], prog))
    elif len(a) >= 3 and a[0] == 'self':
        rap = open(a[3], 'rb').read() if len(a) > 3 else None
        open(a[2], 'wb').write(self_to_elf(a[1], rap))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
