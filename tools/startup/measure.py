#!/usr/bin/env python3
# usage: measure.py N label -- cmd...   (env passed through). Prints per-mark median of external ms (since spawn).
import subprocess, sys, time, os, re, statistics, json
n=int(sys.argv[1]); label=sys.argv[2]; cmd=sys.argv[sys.argv.index('--')+1:]
runs=[]
for i in range(n):
    t0=time.monotonic()
    p=subprocess.Popen(cmd,stderr=subprocess.PIPE,stdout=subprocess.DEVNULL,text=True,env=os.environ)
    marks={}; order=[]
    for line in p.stderr:
        now=(time.monotonic()-t0)*1000
        m=re.match(r'startup: (.+?)\s{2,}\s*(\d+) ms',line)
        if m:
            k=m.group(1).strip()
            if k not in marks: order.append(k)
            marks[k]=(now,int(m.group(2)))
        if 'quit' in line and 'TRACE' in line: pass
    try: p.wait(timeout=20)
    except: p.kill()
    runs.append((order,marks)); time.sleep(float(os.environ.get("GAP","0.5")))
order=runs[-1][0]
res=[]
for k in order:
    ext=[r[1][k][0] for r in runs if k in r[1]]; inn=[r[1][k][1] for r in runs if k in r[1]]
    res.append((k,statistics.median(ext),statistics.median(inn)))
    if "paint" in k or "expose" in k: print(k, "alle:", [round(x) for x in ext])
print(f"== {label} (n={n}, median; ext = ab Prozessstart, int = ab main)")
for k,e,i in res: print(f"  {k:40s} ext {e:7.0f}  int {i:6.0f}")
