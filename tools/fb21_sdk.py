#!/usr/bin/env python3
"""Dump Frostbite 2021 (Dragon Age: The Veilguard) reflected types from the in-memory exe image.
   fb21_sdk.py <exe_image.bin> <out.txt>
TypeInfoData layout (found 2026-10-29):
  +0x00 const char* name     +0x08 u32 nameHash   +0x0C u16 flags (kind = (flags>>5)&0x1F?)   +0x0E u16 size
  +0x10 GUID (16)            +0x20 module ptr     +0x28 super TypeInfo* (runtime)
  +0x30 u16 alignment        +0x32 u16 field count +0x38 runtime TypeInfo* (this type)  +0x40 FieldInfoData* fields
FieldInfoData (0x18): +0 name, +8 u32 hash, +0xC u16 flags, +0xE u16 offset, +0x10 TypeInfo* type
A runtime TypeInfo's first qword points to its TypeInfoData (first qword = name)."""
import struct, sys, re

d = open(sys.argv[1], "rb").read()
BASE = 0x140000000
N = len(d)

def q(o): return struct.unpack_from("<Q", d, o)[0] if 0 <= o <= N - 8 else 0
def cstr(va):
    o = va - BASE
    if not (0 < o < N): return None
    e = d.find(b"\0", o, o + 200)
    if e <= o: return None
    s = d[o:e]
    return s.decode("latin1") if all(32 <= c < 127 for c in s) else None
def tname(ti):
    if not ti: return None
    data = q(ti - BASE)
    return cstr(q(data - BASE)) if data else None

NAMERE = re.compile(r"^[A-Za-z_][A-Za-z0-9_:<>,\- ]*$")
classes = {}
for o in range(0, N - 0x48, 8):
    nm = q(o)
    if not (BASE < nm < BASE + N): continue
    fields = q(o + 0x40)
    if not (BASE < fields < BASE + N): continue
    cnt = struct.unpack_from("<H", d, o + 0x32)[0]
    if cnt == 0 or cnt > 2000: continue
    name = cstr(nm)
    if not name or not NAMERE.match(name): continue
    fo = fields - BASE
    if not cstr(q(fo)): continue
    flags, size = struct.unpack_from("<HH", d, o + 0x0C)
    flist = []; ok = True
    for i in range(cnt):
        r = fo + i * 0x18
        fname = cstr(q(r))
        if not fname: ok = False; break
        ffl, foff = struct.unpack_from("<HH", d, r + 0x0C)
        ft = q(r + 0x10)
        flist.append((foff, fname, tname(ft) or f"?{ft:#x}"))
    if not ok: continue
    sup = tname(q(o + 0x28)) or ""
    classes.setdefault(name, (o, flags, size, sup, q(o + 0x38), flist))

with open(sys.argv[2], "w") as f:
    for nm in sorted(classes):
        o, flags, size, sup, rti, flist = classes[nm]
        f.write(f"type {nm} : {sup}  // size {size:#x} flags {flags:#06x}, data exe+{o:X}, typeinfo {rti:#x}\n")
        for foff, fname, ftype in sorted(flist):
            f.write(f"    +{foff:#06x} {ftype} {fname}\n")
print(f"{len(classes)} types -> {sys.argv[2]}")
