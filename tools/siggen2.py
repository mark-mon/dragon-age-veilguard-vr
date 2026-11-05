import re, sys, relib
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
B = relib.B
img = relib.load(sys.argv[1]); lo, hi = relib.TEXT; text = img[lo:hi]
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
def insns(s, e): return list(md.disasm(img[s:e], s))
def mask_of(ins):
    b = list(ins.bytes); m = [False]*len(b)
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.disp_size == 4:
            for k in range(ins.disp_offset, ins.disp_offset+4): m[k] = True
    if (ins.group(1) or ins.group(2) or ins.group(7)) and ins.imm_size == 4:
        for k in range(ins.imm_offset, ins.imm_offset+4): m[k] = True
    return [None if mm else v for v, mm in zip(b, m)]
def count(pat):
    rx = b''.join(b'.' if v is None else re.escape(bytes([v])) for v in pat)
    return len(re.findall(rx, text, re.DOTALL))
def fmt(pat): return ' '.join('??' if v is None else '%02X' % v for v in pat)
def target_of(ins):
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP: return ins.address + ins.size + op.mem.disp
        if op.type == X86_OP_IMM and (ins.group(2) or ins.group(1)): return op.imm
    return None
def sig_for(name, host, target, before=0):
    fs, fe = relib.func_of(host)
    L = insns(fs, fe)
    for i, ins in enumerate(L):
        if target_of(ins) != target: continue
        dpos = ins.disp_offset if ins.disp_size == 4 else ins.imm_offset
        for b in range(0, before+1):
            st = max(0, i-b); pat = []; off = None
            for j in range(st, len(L)):
                if j == i: off = len(pat) + dpos
                pat += mask_of(L[j])
                if j >= i and len(pat) >= 14 and count(pat) == 1:
                    print(f'{name}: rva 0x{target:X} sig "{fmt(pat)}" offset {off} rip insnEnd {ins.size-dpos}  (in 0x{fs:X} at 0x{ins.address:X})'); return
                if len(pat) > 80: break
    print(f'{name}: not found in 0x{fs:X}')
sig_for('RenderViewDefault', 0x29AC200, 0x2A08AB0, 3)
sig_for('ViewRegistry', 0x29AC200, 0x6207E90, 3)
sig_for('ViewUpdateArg', 0x29A3881, 0x4E22A98, 3)
sig_for('ViewUpdateCall', 0x29A3881, 0x2A2A8E0, 3)
