"""Generate unique byte signatures (rel32 / rip disp32 wildcarded) for the DAVG addresses from the exe memory image."""
import re, sys, relib
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
B = relib.B
img = relib.load(sys.argv[1])
lo, hi = relib.TEXT
text = img[lo:hi]
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def masked(rva, start_rva, maxlen=96):
    """yield (pattern list, length) growing by whole instructions from start_rva"""
    pat = []
    for ins in md.disasm(img[start_rva:start_rva + 256], start_rva):
        b = list(ins.bytes)
        mask = [False] * len(b)
        # rip-relative memory operand
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.disp_size == 4:
                for k in range(ins.disp_offset, ins.disp_offset + 4): mask[k] = True
        # rel32 branches / calls
        if ins.group(1) or ins.group(2) or ins.group(7):   # JUMP, CALL, BRANCH_RELATIVE
            if ins.imm_size == 4:
                for k in range(ins.imm_offset, ins.imm_offset + 4): mask[k] = True
        pat += [None if m else v for v, m in zip(b, mask)]
        yield ins, pat
        if len(pat) >= maxlen: return

def count(pat):
    rx = b''.join(b'.' if v is None else re.escape(bytes([v])) for v in pat)
    return len(re.findall(rx, text, re.DOTALL)) if len(pat) else 0

def fmt(pat): return ' '.join('??' if v is None else '%02X' % v for v in pat)

def func_sig(name, rva):
    for ins, pat in masked(rva, rva):
        if len(pat) >= 12 and sum(v is not None for v in pat) >= 10:
            c = count(pat)
            if c == 1: print(f'{name}: rva 0x{rva:X} sig "{fmt(pat)}" offset 0'); return
    print(f'{name}: NO UNIQUE SIG')

def ref_sig(name, target_rva):
    refs = relib.rip_refs(B + target_rva)
    best = None
    for dpos in refs[:40]:
        # find the instruction start containing this disp: try back up to 10 bytes
        for back in range(1, 12):
            s = dpos - back
            ins = next(md.disasm(img[s:s + 16], s), None)
            if not ins or ins.address + ins.size <= dpos or ins.disp_size != 4 or s + ins.disp_offset != dpos: continue
            for ins2, pat in masked(s, s):
                if len(pat) >= 12:
                    c = count(pat)
                    if c == 1:
                        end = ins.size - ins.disp_offset   # bytes from disp start to instruction end
                        cand = (len(pat), f'{name}: rva 0x{target_rva:X} sig "{fmt(pat)}" offset {ins.disp_offset} rip insnEnd {end}  (ref at 0x{s:X})')
                        if not best or cand[0] < best[0]: best = cand
                        break
            break
    print(best[1] if best else f'{name}: NO UNIQUE REF SIG ({len(refs)} refs)')

for n, r in [('BuildViews', 0x29ABFE0), ('SetupView', 0x298C400), ('RenderViewDefault', 0x2A08AB0), ('RenderViewAssign', 0x2A083B0), ('ViewUpdate', 0x2A2A8E0)]:
    func_sig(n, r)
for n, r in [('ViewRegistry', 0x6207E90), ('ViewUpdateArg', 0x4E22A98)]:
    ref_sig(n, r)
