#!/usr/bin/env python3
"""Compare all dictionaries i18n/<lang>.json with the texts in the code (new language: copy en.json, empty the values).

Collects all T("…") and N_("…") texts from src/ (also literals concatenated over several lines) plus name and
description of the designs (assets/themes/*.json).
  tools/i18n_check.py            show missing and unused entries (exit 1 if something is missing)
  tools/i18n_check.py --update   add missing entries with an empty value, remove unused ones
  tools/i18n_check.py --lang fr  only check this language (--lang can be repeated)
Empty value = not translated yet (shown in English; in en.json: shown in German).
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGS = sorted(os.path.basename(f)[:-5] for f in glob.glob(os.path.join(ROOT, 'i18n', '*.json')))
CALL = re.compile(r'\b(?:T|N_)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)')
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def sources():
    found = set()
    for f in glob.glob(os.path.join(ROOT, 'src', '**', '*.*'), recursive=True):
        if not f.endswith(('.cpp', '.h')):
            continue
        text = open(f, encoding='utf-8').read()
        for m in CALL.finditer(text):
            found.add(unescape(''.join(LIT.findall(m.group(1)))))
    for f in glob.glob(os.path.join(ROOT, 'assets', 'themes', '*.json')):
        design = json.load(open(f, encoding='utf-8'))
        found.update(design[k] for k in ('name', 'description') if design.get(k))
    return found


def check(lang, used, update):
    path = os.path.join(ROOT, 'i18n', lang + '.json')
    d = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {}
    missing = sorted(used - d.keys())
    unused = sorted(d.keys() - used)
    empty = sorted(k for k in used & d.keys() if not d[k])
    if update:
        for k in missing:
            d[k] = ''
        for k in unused:
            del d[k]
        with open(path, 'w', encoding='utf-8') as out:
            json.dump(dict(sorted(d.items())), out, ensure_ascii=False, indent=1)
            out.write('\n')
        print(f'{lang}: {len(missing)} ergänzt, {len(unused)} entfernt, {len(empty) + len(missing)} unübersetzt')
        return True
    for k in missing:
        print(f'{lang} fehlt:', repr(k))
    for k in unused:
        print(f'{lang} unbenutzt:', repr(k))
    for k in empty:
        print(f'{lang} unübersetzt:', repr(k))
    return not missing and not empty


def main():
    args = sys.argv[1:]
    langs = [args[i + 1] for i, a in enumerate(args[:-1]) if a == '--lang'] or LANGS
    used = sources()
    ok = [check(lang, used, '--update' in args) for lang in langs]
    return 0 if all(ok) else 1


sys.exit(main())
