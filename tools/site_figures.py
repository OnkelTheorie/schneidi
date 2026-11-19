#!/usr/bin/env python3
"""site/figures.json: the measured figures the project website shows, in one stable named file.

  tools/site_figures.py --write    regenerate site/figures.json from README.md (and i18n/)
  tools/site_figures.py --check    exit 1 when site/figures.json is missing, stale or invalid
  tools/site_figures.py --check --website PATH/update_site.py
                                   also compare with the website updater: same README_SPECS, and its
                                   figures_from_json(our file) equals figures_from_readme(README.md)

The website's daily updater reads site/figures.json at the latest release tag (falling back to parsing the README
tables). This script parses the same README rows as the updater, but strictly: a row that is not found, a missing
column, a cell without the "5600X / 2400G" split or without enough numbers is an error, so restructuring the README
shows up here instead of as a wrong number on the site. Change a measured value in README.md, then run --write.

The UI languages come from i18n/*.json (+ German, the source language); their number must match the README's
"available in N languages".

Python 3.8+, standard library only.
"""
import argparse
import importlib.util
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
README = os.path.join(ROOT, "README.md")
I18N = os.path.join(ROOT, "i18n")
FIGURES = os.path.join(ROOT, "site", "figures.json")

# Keep in sync with README_SPECS in the website's update_site.py (same keys, rows, columns):
# (key, first cell of the row, column, part of a "5600X / 2400G" cell (None = whole cell), n-th number in it).
README_SPECS = [
    ("start_s", "Window visible, one full core", 1, 0, 0),
    ("start_s_2400g", "Window visible, one full core", 1, 1, 0),
    ("start10_s", "Window visible, 10 % of one core", 1, 0, 0),
    ("start10_s_2400g", "Window visible, 10 % of one core", 1, 1, 0),
    ("min_ram_mb", "Least RAM it starts and keeps running with", 1, 0, 0),
    ("min_ram_mb_2400g", "Least RAM it starts and keeps running with", 1, 1, 0),
    ("clip_ram_mb", "Least RAM it starts and keeps running with", 2, 0, 0),
    ("clip_ram_mb_2400g", "Least RAM it starts and keeps running with", 2, 1, 0),
    ("orig1_fps", "1 core", 1, 0, 0),
    ("orig1_fps_2400g", "1 core", 1, 1, 0),
    ("gpu1_fps", "1 core", 2, 0, 0),
    ("gpu1_fps_2400g", "1 core", 2, 1, 0),
    ("proxy1_fps", "1 core", 3, 0, 0),
    ("proxy1_fps_2400g", "1 core", 3, 1, 0),
    ("orig4_fps", "4 cores", 1, 0, 0),
    ("orig4_fps_2400g", "4 cores", 1, 1, 0),
    ("pi_start_s", "Window visible", 1, None, 0),
    ("pi_ram_mb", "Window visible", 1, None, 1),
    ("pi_720_fps", "Playback 720p30 original", 1, None, 0),
    ("pi_fps", "Playback 1080p60 from proxies", 1, None, 0),
    ("pi_jump_s", "Jumping in the timeline, original → proxies", 1, None, 1),
]

# Copied from update_site.py: what a value must look like or the website keeps its old one.
PATTERNS = {
    "num": r"\d{1,6}(\.\d{1,3})?",
    "word": r"[a-z]{3,10}",
    "languages": r"[A-Z][A-Za-z()' -]{1,40}(, [A-Z][A-Za-z()' -]{1,40}){0,40} and [A-Z][A-Za-z()' -]{1,40}",
}
KIND = {"smooth_cores_word": "word", "languages": "languages"}

NUMBER_WORDS = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six",
                7: "seven", 8: "eight", 9: "nine", 10: "ten", 11: "eleven", 12: "twelve",
                13: "thirteen", 14: "fourteen", 15: "fifteen", 16: "sixteen"}

# English names as the website writes them (no commas: they separate the list). English and German come first.
LANGUAGE_NAMES = {
    "en": "English", "de": "German", "es": "Spanish", "fr": "French", "hi": "Hindi", "id": "Indonesian",
    "it": "Italian", "ja": "Japanese", "ko": "Korean", "nl": "Dutch", "pl": "Polish",
    "pt": "Portuguese (Brazil)", "ru": "Russian", "tr": "Turkish", "uk": "Ukrainian", "zh": "Chinese (Simplified)",
}

# Every key of the website's markers that comes from the repository (version, dates, download sizes and links come
# from the GitHub release API, not from this file).
EXTRA_KEYS = ["smooth_cores", "smooth_cores_word", "languages", "languages_count"]
KEYS = [s[0] for s in README_SPECS] + EXTRA_KEYS


class FigureError(Exception):
    pass


def valid(key, value):
    return isinstance(value, str) and re.fullmatch(PATTERNS[KIND.get(key, "num")], value) is not None


def table_rows(readme):
    for line in readme.splitlines():
        if line.startswith("|"):
            yield [c.strip() for c in line.strip().strip("|").split("|")]


def find_row(rows, label):
    """The row whose first cell is exactly the label (the website also accepts a prefix; here that is an error)."""
    exact = [r for r in rows if r and r[0].lower() == label.lower()]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise FigureError("README: %d table rows are labelled %r (the website takes the first)" % (len(exact), label))
    prefix = [r[0] for r in rows if r and r[0].lower().startswith(label.lower())]
    if prefix:
        raise FigureError("README: no table row labelled exactly %r, only %s (the website would take the first)"
                          % (label, ", ".join(repr(p) for p in prefix)))
    raise FigureError("README: no table row labelled %r" % label)


def from_readme(readme, languages):
    """{key: (value, source)} for every key; raises FigureError on anything unexpected."""
    rows = list(table_rows(readme))
    figs = {}
    for key, label, col, part, nth in README_SPECS:
        cells = find_row(rows, label)
        if len(cells) <= col or not cells[col]:
            raise FigureError("README row %r: no column %d (row is %r)" % (label, col, " | ".join(cells)))
        cell = cells[col]
        where = "%s, column %d" % (label, col)
        if part is not None:
            pieces = cell.split("/")
            if len(pieces) != 2:
                raise FigureError("README row %r column %d: expected \"5600X / 2400G\", got %r" % (label, col, cell))
            cell = pieces[part]
            where += ", " + ("5600X" if part == 0 else "2400G")
        nums = re.findall(r"\d+(?:\.\d+)?", cell)
        if len(nums) <= nth:
            raise FigureError("README row %r column %d: expected at least %d number(s) in %r"
                              % (label, col, nth + 1, cell))
        figs[key] = (nums[nth], where)
    # Fewest cores that play the demanding project smoothly from the original files (same rule as the website).
    core_rows = [c for c in rows if c and re.fullmatch(r"(\d+) cores?", c[0])]
    if not core_rows:
        raise FigureError("README: no playback rows labelled \"N cores\"")
    for cells in core_rows:
        if len(cells) > 1 and "smooth" in cells[1] and "stutter" not in cells[1]:
            n = int(cells[0].split()[0])
            if n not in NUMBER_WORDS:
                raise FigureError("README: %d cores has no number word for the website" % n)
            figs["smooth_cores"] = (str(n), "%s, column 1 (first \"smooth\" without \"stutter\")" % cells[0])
            figs["smooth_cores_word"] = (NUMBER_WORDS[n], figs["smooth_cores"][1])
            break
    else:
        raise FigureError("README: no \"N cores\" row plays smoothly from the original files")
    # UI languages: i18n/*.json, their count checked against the README's sentence.
    m = re.search(r"available in (\d+) languages", readme)
    if not m:
        raise FigureError("README: sentence \"available in N languages\" not found")
    if int(m.group(1)) != len(languages):
        raise FigureError("README says %s languages, i18n/ has %d (%s)"
                          % (m.group(1), len(languages), ", ".join(languages)))
    names = [LANGUAGE_NAMES[c] for c in languages]
    src = "i18n/*.json + German; README \"available in %s languages\"" % m.group(1)
    figs["languages"] = (", ".join(names[:-1]) + " and " + names[-1], src)
    figs["languages_count"] = (str(len(names)), src)
    for key, (value, _) in figs.items():
        if not valid(key, value):
            raise FigureError("%s = %r would be rejected by the website" % (key, value))
    return figs


def ui_languages():
    codes = sorted(f[:-5] for f in os.listdir(I18N) if f.endswith(".json"))
    unknown = [c for c in codes if c not in LANGUAGE_NAMES]
    if unknown:
        raise FigureError("i18n/: no English name for %s - add it to LANGUAGE_NAMES in %s"
                          % (", ".join(unknown), os.path.relpath(__file__, ROOT)))
    rest = sorted((c for c in codes + ["de"] if c not in ("en", "de")), key=lambda c: LANGUAGE_NAMES[c])
    return [c for c in ("en", "de") if c == "de" or c in codes] + rest


def generate():
    with open(README, encoding="utf-8") as f:
        readme = f.read()
    figs = from_readme(readme, ui_languages())
    return {
        "schema": 1,
        "about": "Measured figures for the project website, generated from README.md by tools/site_figures.py "
                 "--write; do not edit by hand.",
        "figures": {k: {"value": figs[k][0], "source": figs[k][1]} for k in KEYS},
    }


def dump(data):
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def check(data):
    """Problems of site/figures.json compared with what README.md gives now."""
    try:
        with open(FIGURES, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        return ["cannot read site/figures.json (%s)" % e.strerror]
    try:
        have = json.loads(text)
    except ValueError as e:
        return ["site/figures.json is not valid JSON: %s" % e]
    if not isinstance(have, dict) or have.get("schema") != 1 or not isinstance(have.get("figures"), dict):
        return ["site/figures.json: expected {\"schema\": 1, \"figures\": {...}}"]
    problems = []
    got = have["figures"]
    for key in KEYS:
        want = data["figures"][key]["value"]
        entry = got.get(key)
        value = entry.get("value") if isinstance(entry, dict) else entry
        if entry is None:
            problems.append("missing %s (README: %s)" % (key, want))
        elif not valid(key, value):
            problems.append("%s = %r would be rejected by the website" % (key, value))
        elif value != want:
            problems.append("%s = %s, but README gives %s (%s)" % (key, value, want, data["figures"][key]["source"]))
    for key in sorted(set(got) - set(KEYS)):
        problems.append("unknown key %s (the website does not use it)" % key)
    if not problems and text != dump(data):
        problems.append("site/figures.json differs from the generated file (sources or formatting)")
    return problems


def compare_website(path, data):
    sys.dont_write_bytecode = True  # the website copy may be read-only: leave no __pycache__ there
    spec = importlib.util.spec_from_file_location("schneidi_update_site", path)
    site = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(site)
    problems = []
    if [tuple(s) for s in site.README_SPECS] != [tuple(s) for s in README_SPECS]:
        problems.append("README_SPECS differ from the website's update_site.py")
    from_json = site.figures_from_json(dump(data))
    with open(README, encoding="utf-8") as f:
        from_readme_site = site.figures_from_readme(f.read())
    for key in sorted(set(from_json) | set(from_readme_site)):
        a, b = from_json.get(key), from_readme_site.get(key)
        if a == b:
            print("  same     %-18s %s" % (key, a))
        elif b is None:
            print("  json only %-17s %s (the website's README parser finds nothing)" % (key, a))
        else:
            problems.append("%s: figures.json gives %r, the website's README parser %r" % (key, a, b))
    for key in KEYS:
        if not site.valid(key, from_json.get(key)):
            problems.append("%s = %r fails the website's pattern" % (key, from_json.get(key)))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true", help="regenerate site/figures.json")
    g.add_argument("--check", action="store_true", help="fail when site/figures.json is missing, stale or invalid")
    ap.add_argument("--website", metavar="update_site.py", help="also compare with the website's updater")
    a = ap.parse_args()
    try:
        data = generate()
    except (FigureError, OSError) as e:
        print("site_figures: %s" % e, file=sys.stderr)
        return 1
    if a.write:
        os.makedirs(os.path.dirname(FIGURES), exist_ok=True)
        with open(FIGURES, "w", encoding="utf-8") as f:
            f.write(dump(data))
        print("site_figures: wrote site/figures.json (%d figures)" % len(KEYS))
        problems = []
    else:
        problems = check(data)
    if a.website:
        problems += compare_website(a.website, data)
    if problems:
        print("site_figures: site/figures.json is not right:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        if a.check:
            print("Fix README.md if it is wrong, then run tools/site_figures.py --write.", file=sys.stderr)
        return 1
    if a.check:
        print("site_figures: site/figures.json matches README.md (%d figures)" % len(KEYS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
