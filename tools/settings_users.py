# For a settings TypeInfo: find the cache globals filled by getSettings(mgr, &TI), then every function reading
# those caches, then accesses of the given field offsets in those functions.
import sys, relib as R
R.load()
TI = int(sys.argv[1], 16); offs = [int(x, 16) for x in sys.argv[2].split(',')]
refs = R.rip_refs(TI)
caches = set()
for r in refs:
    f = R.func_of(r)
    if not f: continue
    ins = R.dis(r - 3, 12)
    for i in ins:
        if i.mnemonic == 'mov' and i.op_str.startswith('qword ptr [rip +') and i.op_str.endswith('rax'):
            disp = int(i.op_str.split('+')[1].split(']')[0], 16)
            caches.add(i.address + i.size + disp - R.B); break
print('caches:', [hex(c) for c in sorted(caches)])
funcs = set()
for c in caches:
    for r in R.rip_refs(R.B + c):
        f = R.func_of(r)
        if f: funcs.add(f)
print(len(funcs), 'functions read the caches')
pat = ['+ %s]' % hex(o) for o in offs]
for f in sorted(funcs):
    ins = R.dis(f[0], size=f[1] - f[0])
    hits = [i for i in ins if any(p in i.op_str for p in pat) and 'rsp' not in i.op_str and 'rbp' not in i.op_str and 'call' not in i.mnemonic]
    if hits:
        print('func %X-%X' % f)
        for i in hits: print('   %X %s %s' % (i.address - R.B, i.mnemonic, i.op_str))
