#!/usr/bin/env python3
# analyze.py samples.txt [t_from t_to] [focus-substring]: inclusive/self ms per function (0.5 ms per sample)
import sys, subprocess, bisect, collections, re
fn=sys.argv[1]; t0=int(sys.argv[2]) if len(sys.argv)>2 else 0; t1=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
focus=sys.argv[4] if len(sys.argv)>4 else None
maps=[]; samples=[]
for line in open(fn):
    if line.startswith('M '):
        p=line[2:].split()
        if len(p)>=6 and 'x' in p[1]:
            a,b=[int(x,16) for x in p[0].split('-')]; maps.append((a,b,int(p[2],16),p[5]))
    elif line.startswith('S '):
        p=line.split(); t=int(p[1])
        if t0<=t<=t1: samples.append([int(x,16) for x in p[2:]])
maps.sort(); starts=[m[0] for m in maps]
def where(a):
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and maps[i][0]<=a<maps[i][1]: return maps[i][3], a-maps[i][0]+maps[i][2]
    return None,a
byfile=collections.defaultdict(set)
for s in samples:
    for a in s:
        f,o=where(a-1); 
        if f: byfile[f].add(o)
names={}
nmcache={}
def nmsyms(f):
    if f in nmcache: return nmcache[f]
    out=subprocess.run(['nm','-D','-C','--defined-only',f],capture_output=True,text=True).stdout
    out+=subprocess.run(['nm','-C','--defined-only',f],capture_output=True,text=True).stdout
    syms=[]
    for l in out.splitlines():
        m=re.match(r'([0-9a-f]+) [tTwW] (.*)',l)
        if m: syms.append((int(m.group(1),16),m.group(2)))
    syms.sort(); nmcache[f]=( [x[0] for x in syms],[x[1] for x in syms]); return nmcache[f]
import os
def segfix(f):
    out=subprocess.run(['readelf','-lW',f],capture_output=True,text=True).stdout
    segs=[]
    for l in out.splitlines():
        p=l.split()
        if p and p[0]=='LOAD': segs.append((int(p[1],16),int(p[2],16),int(p[4],16)))
    return segs
def tovaddr(segs,o):
    for off,va,fs in segs:
        if off<=o<off+fs: return o-off+va
    return o
for f,offs in byfile.items():
    offs=sorted(offs); short=os.path.basename(f); sg=segfix(f)
    va={o:tovaddr(sg,o) for o in offs}
    res=subprocess.run(['addr2line','-f','-C','-e',f]+[hex(va[o]) for o in offs],capture_output=True,text=True).stdout.splitlines()
    for i,o in enumerate(offs):
        nm=res[2*i] if 2*i<len(res) else '??'
        if nm=='??':
            a,n=nmsyms(f); j=bisect.bisect_right(a,va[o])-1
            nm=n[j] if j>=0 else '??'
        names[(f,o)]=short+': '+re.sub(r'\(.*','(..)',nm)[:110]
incl=collections.Counter(); selfc=collections.Counter()
for s in samples:
    fr=[]
    for a in s[1:]:  # skip handler frame-ish (first is in sampler)
        f,o=where(a-1)
        if f and 'libsampler' not in f and 'libc.so' not in f or (f and 'libc.so' in f): fr.append(names.get((f,o),'?'))
    if focus and not any(focus in x for x in fr): continue
    for x in set(fr): incl[x]+=1
    if fr: selfc[fr[0]]+=1
tot=len(samples)
print(f"samples {tot} = {tot/2:.0f} ms (window {t0}-{t1})")
print("-- inclusive top")
for k,v in incl.most_common(70): print(f"{v/2:7.1f} ms  {k}")
print("-- self top")
for k,v in selfc.most_common(25): print(f"{v/2:7.1f} ms  {k}")
