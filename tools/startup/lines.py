#!/usr/bin/env python3
# lines.py samples t0 t1 lib depth: per sample, the innermost frames in <lib> (file:line), attributed (depth levels)
import sys, subprocess, bisect, collections
fn,t0,t1,lib=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4]; depth=int(sys.argv[5]) if len(sys.argv)>5 else 1
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
libf=None; offs=set(); per=[]
for s in samples:
    fr=[]
    for a in s:
        f,o=where(a-1)
        if f and lib in f: libf=f; fr.append(o); offs.add(o)
    per.append(fr)
out=subprocess.run(['readelf','-lW',libf],capture_output=True,text=True).stdout
segs=[(int(p[1],16),int(p[2],16),int(p[4],16)) for p in (l.split() for l in out.splitlines()) if p and p[0]=='LOAD']
def va(o):
    for off,v,fs in segs:
        if off<=o<off+fs: return o-off+v
offs=sorted(offs)
res=subprocess.run(['addr2line','-f','-C','-e',libf]+[hex(va(o)) for o in offs],capture_output=True,text=True).stdout.splitlines()
name={o:(res[2*i].split('(')[0][-50:]+' '+res[2*i+1].split('/src/')[-1].split(' ')[0]) for i,o in enumerate(offs)}
c=collections.Counter()
for fr in per:
    if not fr: c['(outside '+lib+')']+=1; continue
    key=' <- '.join(name[o] for o in fr[:depth][::-1][::-1])
    c[key]+=1
for k,v in c.most_common(45): print(f"{v/2:6.1f} ms  {k}")
