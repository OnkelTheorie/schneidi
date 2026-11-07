#!/usr/bin/env python3
# Fallback when the i18n merge driver (tools/merge_i18n.py) is not configured: run after a merge stopped on conflicts.
# Resolves append-only merge conflicts: i18n dictionaries (union) and CMake lists (keep both sides)
import json,subprocess,collections,re,sys
def show(stage,p): return subprocess.run(['git','show',f':{stage}:{p}'],capture_output=True,text=True,check=True).stdout
files=subprocess.run(['git','diff','--name-only','--diff-filter=U'],capture_output=True,text=True).stdout.split()
for p in files:
    if p.startswith('i18n/') and p.endswith('.json'):
        a=json.loads(show(2,p),object_pairs_hook=collections.OrderedDict); b=json.loads(show(3,p),object_pairs_hook=collections.OrderedDict)
        for k,v in b.items():
            if k in a and a[k]!=v: print('DIFFERENT',p,k)
            a.setdefault(k,v)
        open(p,'w',encoding='utf-8').write(json.dumps(a,ensure_ascii=False,indent=1)+'\n')
    elif p.endswith('CMakeLists.txt'):
        s=open(p).read(); s=re.sub(r'^(<<<<<<< HEAD|=======|>>>>>>> \S+)\n','',s,flags=re.M); open(p,'w').write(s)
    else: print('UNRESOLVED',p)
