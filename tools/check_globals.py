#!/usr/bin/env python3
"""Post-link check: every global-block variable's physical label must sit at
GLOBAL_PTR_BASE + its _m_ offset.  A nonzero delta means equates.h and the
.data.NNN storage blocks have drifted -- macro accesses and label accesses
then touch different words, and new storage can land on top of an existing
variable (session 21b4: sh_encrypted landed on _dontstop -> silent resets)."""
import subprocess,sys
elf=sys.argv[1] if len(sys.argv)>1 else 'pocketvt.elf'
offs={};addrs={}
for l in subprocess.run(['nm',elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)!=3: continue
    v,t,n=p
    if t=='a': offs.setdefault(n,int(v,16))
    elif t in 'DdBb': addrs.setdefault(n,int(v,16))
BASE=addrs.get('GLOBAL_PTR_BASE')
if BASE is None: print("check_globals: GLOBAL_PTR_BASE not found"); sys.exit(2)
bad=[]
for n,o in offs.items():
    if o>0x2000: continue
    a=addrs.get('_'+n)
    if a is None or not (BASE<=a<BASE+0x2000): continue
    if a!=BASE+o: bad.append((o,n,a,a-(BASE+o)))
if bad:
    print("GLOBALS LAYOUT DRIFT -- %d mismatches, first few:"%len(bad))
    for o,n,a,d in sorted(bad)[:8]: print("   %-26s offset %06x -> %08x (delta %+d)"%(n,o,a,d))
    sys.exit(1)
print("check_globals: OK - all %d compared globals match their offsets"%len(offs))
