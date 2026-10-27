#!/usr/bin/env python3
"""Resolve merge conflicts in i18n/*.json: three-way union of both sides' changes, written like i18n_check.py.

  tools/i18n_merge.py [repo-or-worktree]   (after a `git merge` that stopped on i18n conflicts; stages the files)

A key changed differently on both sides keeps ours and is printed.
"""
import json, subprocess, sys

wt = sys.argv[1] if len(sys.argv) > 1 else '.'
files = subprocess.run(['git', '-C', wt, 'diff', '--name-only', '--diff-filter=U'], capture_output=True, text=True,
                       check=True).stdout.split()
for f in files:
    if not (f.startswith('i18n/') and f.endswith('.json')):
        continue
    def stage(n):
        r = subprocess.run(['git', '-C', wt, 'show', f':{n}:{f}'], capture_output=True, text=True)
        return json.loads(r.stdout) if r.returncode == 0 else {}
    base, ours, theirs = stage(1), stage(2), stage(3)
    out = dict(ours)
    for k in set(base) | set(theirs):
        b, t, o = base.get(k), theirs.get(k), ours.get(k)
        if t == b:
            continue  # theirs did not change it
        if o != b and o != t:
            print(f'{f}: both changed {k!r}: ours={o!r} theirs={t!r} (kept ours)')
            continue
        if t is None:
            out.pop(k, None)
        else:
            out[k] = t
    with open(f'{wt}/{f}', 'w', encoding='utf-8') as fh:
        json.dump(dict(sorted(out.items())), fh, ensure_ascii=False, indent=1)
        fh.write('\n')
    subprocess.run(['git', '-C', wt, 'add', f], check=True)
    print(f'{f}: merged ({len(out)} keys)')
