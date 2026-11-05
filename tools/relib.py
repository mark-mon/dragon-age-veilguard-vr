"""RE helpers for the Veilguard exe image (fixed base 0x140000000). Import from analysis scripts.
   img: memory image (exe_image.bin) or disk image (same code)."""
import struct, bisect, numpy as np
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
B = 0x140000000
_d = None; _np = None; _pdata = None
TEXT = (0x1000, 0x3c19000 + 0xc251e5)   # .text + ctr

def load(path='/home/claude/dav/mem_image.bin'):
    global _d, _np, _pdata
    _d = open(path, 'rb').read(); _np = np.frombuffer(_d, dtype=np.uint8)
    pd = 0x64c1000; n = 0x4ccd40 // 12
    arr = np.frombuffer(_d[pd:pd + n * 12], dtype='<u4').reshape(-1, 3)
    arr = arr[arr[:, 0] != 0]
    _pdata = (arr[:, 0].copy(), arr[:, 1].copy())
    return _d

def d(): return _d
def q(o): return struct.unpack_from('<Q', _d, o)[0]
def u32(o): return struct.unpack_from('<I', _d, o)[0]
def f32(o): return struct.unpack_from('<f', _d, o)[0]
def cstr(va, n=200):
    o = va - B
    if not 0 < o < len(_d): return None
    e = _d.find(b'\0', o, o + n); s = _d[o:e]
    return s.decode('latin1') if s and all(32 <= c < 127 for c in s) else None

def func_of(rva):
    """(start, end) rva of the function containing rva, from .pdata (chained entries not merged)."""
    st, en = _pdata
    i = np.searchsorted(st, rva, side='right') - 1
    if i >= 0 and st[i] <= rva < en[i]: return int(st[i]), int(en[i])
    return None

def root_func(rva):
    """Follow chained unwind info back to the primary function entry."""
    f = func_of(rva)
    return f

_idx = None
def _build_index():
    """All modrm rip-relative operands in .text/ctr: (target va, disp rva) sorted by target. Cached on disk."""
    global _idx
    import os
    cache = '/home/claude/dav/riprefs.npz'
    if os.path.exists(cache):
        z = np.load(cache); _idx = (z['t'], z['p']); return
    lo, hi = TEXT
    a = _np[lo:hi]
    m = np.nonzero((a[:-5] & 0xC7) == 0x05)[0] + 1          # byte after a rip modrm = disp start
    m = m[m + 4 <= len(a)]
    v = (a[m].astype(np.int64) | (a[m + 1].astype(np.int64) << 8) | (a[m + 2].astype(np.int64) << 16) | (a[m + 3].astype(np.int64) << 24))
    v = np.where(v >= 2**31, v - 2**32, v)
    pos = (m + lo).astype(np.int64)
    ts = []; ps = []
    for tail in (4, 5, 8):          # disp last / followed by imm8 / imm32
        ts.append(B + pos + tail + v); ps.append(pos)
    t = np.concatenate(ts); p = np.concatenate(ps)
    o = np.argsort(t, kind='stable'); _idx = (t[o], p[o])
    np.savez(cache, t=_idx[0], p=_idx[1])

def rip_refs(target_va, lo=TEXT[0], hi=TEXT[1]):
    if _idx is None: _build_index()
    t, p = _idx
    a = np.searchsorted(t, target_va, 'left'); b = np.searchsorted(t, target_va, 'right')
    return sorted(set(int(x) for x in p[a:b]))

def rip_refs_slow(target_va, lo=TEXT[0], hi=TEXT[1]):
    """Offsets o in [lo,hi) where int32 at o, as a rip-relative disp ending the instruction (o+4), points to target.
       Also tries instructions with a trailing imm8/imm32 (o+5, o+8). Returns candidate rvas of the disp field."""
    seg = _d[lo:hi]
    a = np.frombuffer(seg[:len(seg) // 4 * 4 + 0], dtype=np.uint8)
    out = []
    for tail in (4, 5, 8):
        n = len(seg) - 4
        disp = np.ndarray(shape=(n,), dtype='<i4', buffer=seg, offset=0, strides=(1,)) if False else None
        # vectorised: build int32 view at every byte offset via 4 shifted arrays
        b0 = a[:n].astype(np.int64); b1 = a[1:n + 1].astype(np.int64); b2 = a[2:n + 2].astype(np.int64); b3 = a[3:n + 3].astype(np.int64)
        v = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
        v = np.where(v >= 2**31, v - 2**32, v)
        idx = np.arange(n, dtype=np.int64) + lo
        hit = np.nonzero(B + idx + tail + v == target_va)[0]
        out += [int(lo + h) for h in hit]
        del b0, b1, b2, b3, v, idx
    return sorted(set(out))

_cs = Cs(CS_ARCH_X86, CS_MODE_64); _cs.detail = False
def dis(rva, n=40, size=None):
    code = _d[rva:rva + (size or n * 15)]
    out = []
    for i in _cs.disasm(code, B + rva):
        out.append(i)
        if not size and len(out) >= n: break
    return out
def pdis(rva, n=40, size=None, mark=None):
    for i in dis(rva, n, size):
        m = '>>' if mark and i.address - B == mark else '  '
        print('%s %X: %-8s %s' % (m, i.address - B, i.mnemonic, i.op_str))
def insn_at(disp_rva):
    """Find the instruction containing a disp field: disassemble from the containing function start."""
    f = func_of(disp_rva)
    if not f: return None
    for i in dis(f[0], size=f[1] - f[0]):
        a = i.address - B
        if a <= disp_rva < a + i.size: return i
    return None
def callers(func_rva, lo=TEXT[0], hi=TEXT[1]):
    """rvas of E8 call instructions targeting func_rva."""
    seg = _np[lo:hi]
    e8 = np.nonzero(seg == 0xE8)[0]
    e8 = e8[e8 + 5 < len(seg)]
    v = seg[e8 + 1].astype(np.int64) | (seg[e8 + 2].astype(np.int64) << 8) | (seg[e8 + 3].astype(np.int64) << 16) | (seg[e8 + 4].astype(np.int64) << 24)
    v = np.where(v >= 2**31, v - 2**32, v)
    tgt = lo + e8 + 5 + v
    return [int(lo + x) for x in e8[tgt == func_rva]]
def find_bytes(pat, lo=0, hi=None):
    out = []; s = lo; hi = hi or len(_d)
    while True:
        i = _d.find(pat, s, hi)
        if i < 0: return out
        out.append(i); s = i + 1
