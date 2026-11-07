#!/usr/bin/env python3
"""Git merge driver for i18n/*.json: three-way union of both sides' changes, so two branches that each add texts
never conflict.

  git config merge.i18n.driver "python3 tools/merge_i18n.py %O %A %B"   (once per clone; .gitattributes names it)

Without that config git merges these files as plain text, as before. Keys added on either side are kept, keys
removed on one side (and unchanged on the other) are removed, a value changed on one side wins. Only a key changed
to different values on both sides is a real conflict: ours is kept, the key is printed and the merge stops there
(exit 1) so a person decides. Written in our order with the new keys appended; tools/i18n_check.py sorts later.
"""
import json, sys
from collections import OrderedDict


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    base_path, ours_path, theirs_path = sys.argv[1:]
    try:
        base, ours, theirs = load(base_path), load(ours_path), load(theirs_path)
    except (OSError, ValueError) as e:  # not JSON (e.g. a side with conflict markers): leave it to the person
        print(f'merge_i18n: cannot read a side ({e}), not merged')
        return 1
    out = OrderedDict(ours)
    conflicts = []
    for k in list(base) + [k for k in theirs if k not in base]:
        b, t, o = base.get(k), theirs.get(k), ours.get(k)
        if t == b or t == o:
            continue  # theirs did not change it, or both made the same change
        if o != b:
            conflicts.append((k, o, t))
            continue
        if t is None:
            out.pop(k, None)
        else:
            out[k] = t
    with open(ours_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
        f.write('\n')
    for k, o, t in conflicts:
        print(f'merge_i18n: both sides changed {k!r}: ours={o!r} theirs={t!r} (kept ours)')
    return 1 if conflicts else 0


sys.exit(main())
