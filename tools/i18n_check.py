#!/usr/bin/env python3
"""Wörterbuch i18n/en.json mit den Texten im Code abgleichen.

Sammelt alle T("…")- und N_("…")-Texte aus src/ (auch über mehrere Zeilen verkettete Literale).
  tools/i18n_check.py           fehlende und überflüssige Einträge anzeigen (Exit 1, wenn etwas fehlt)
  tools/i18n_check.py --update  fehlende Einträge mit leerem Wert ergänzen, überflüssige entfernen
Leerer Wert = noch nicht übersetzt (Anzeige bleibt deutsch).
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT = os.path.join(ROOT, 'i18n', 'en.json')
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
    return found


def main():
    used = sources()
    d = json.load(open(DICT, encoding='utf-8')) if os.path.exists(DICT) else {}
    missing = sorted(used - d.keys())
    unused = sorted(d.keys() - used)
    empty = sorted(k for k in used & d.keys() if not d[k])
    if '--update' in sys.argv:
        for k in missing:
            d[k] = ''
        for k in unused:
            del d[k]
        with open(DICT, 'w', encoding='utf-8') as out:
            json.dump(dict(sorted(d.items())), out, ensure_ascii=False, indent=1)
            out.write('\n')
        print(f'{len(missing)} ergänzt, {len(unused)} entfernt, {len(empty) + len(missing)} unübersetzt')
        return 0
    for k in missing:
        print('fehlt:', repr(k))
    for k in unused:
        print('unbenutzt:', repr(k))
    for k in empty:
        print('unübersetzt:', repr(k))
    return 1 if missing or empty else 0


sys.exit(main())
