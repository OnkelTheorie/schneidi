#!/usr/bin/env python3
"""site/figures.json: the measured figures the project website shows, in one stable named file.

  tools/site_figures.py --write    regenerate site/figures.json from README.md (and i18n/)
  tools/site_figures.py --check    exit 1 when site/figures.json is missing, stale or invalid, or
                                   site/downloads.json breaks the website's rules
  tools/site_figures.py --check --website PATH/update_site.py
                                   also compare with the website updater: same README_SPECS, and its
                                   figures_from_json(our file) equals figures_from_readme(README.md)

The website's daily updater reads site/figures.json at the latest release tag (falling back to parsing the README
tables). This script parses the same README rows as the updater, but strictly: a row that is not found, a missing
column, a cell without the "5600X / 2400G" split or without enough numbers is an error, so restructuring the README
shows up here instead of as a wrong number on the site. Change a measured value in README.md, then run --write.

The UI languages come from i18n/*.json (+ German, the source language); their number must match the README's
"available in N languages".

site/downloads.json (download list), site/site.json (status texts, notice, What's new settings) and
site/features.json (feature cards, images in site/img/ made by tools/site_images.py) are written by hand. --check
validates them with the same rules as the website's updater (the "shared rules" block below is the same text in both:
schema, unique ids, asset patterns, store hosts, strict plain text with only `code`, **bold** and one [link] from a
fixed list), so a new package, text or card needs only a change there and appears on the site with the next release.

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
DOWNLOADS = os.path.join(ROOT, "site", "downloads.json")
SITE_JSON = os.path.join(ROOT, "site", "site.json")
FEATURES = os.path.join(ROOT, "site", "features.json")
SITE_IMG = os.path.join(ROOT, "site", "img")

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


# ---------------------------------------------------------------- site/downloads.json, site.json, features.json

# ==== BEGIN shared rules ====
# This block is the same text in the schneidi repository (tools/site_figures.py) and in the website's
# update_site.py; `tools/site_figures.py --check --website update_site.py` fails when they differ.
# They define what the files in site/ may contain. Nothing in them is ever written into a page as markup:
# texts are escaped and only `code`, **bold** and one [link text] (for a key of LINKS) become tags.
LINKS = {
    "building": "https://github.com/{repo}#building",
    "releases": "https://github.com/{repo}/releases/latest",
    "features": "https://github.com/{repo}#features",
    "readme": "https://github.com/{repo}#readme",
    "docker": "https://github.com/{repo}/tree/main/packaging/docker",
    "arch": "https://github.com/{repo}/tree/main/packaging/linux/arch",
    "download": "#download",
}
STORE_HOSTS = ("play.google.com", "f-droid.org", "flathub.org", "apps.microsoft.com", "aur.archlinux.org",
               "snapcraft.io", "github.com", "ghcr.io")
KINDS = ("asset", "store", "docker", "build")
OS_NAMES = ("Linux", "Windows", "macOS", "Android")
ID = r"[a-z][a-z0-9]{0,19}"
NAME = r"[^\W_][\w ()+.\-]{0,39}"
ASSET = r"(?:[A-Za-z0-9._+\-]|\{version\}|\{n\}){1,100}"
IMAGE_ID = r"[a-z0-9][a-z0-9\-]{0,39}"
TEXT = r"[\w .,:;()'!?/+%=*–—›…`\[\]#\-]{1,600}"
DOCKER_IMAGE = r"(?:ghcr\.io|docker\.io)/[a-z0-9]+(?:[._/\-][a-z0-9]+){0,10}(?::[A-Za-z0-9._\-]{1,40})?"
STORE_URL = r"https://(?:%s)(?:/[A-Za-z0-9._~/?=&%%:+\-]{0,300})?" % "|".join(re.escape(h) for h in STORE_HOSTS)
TEXT_KEY = r"[a-z][a-z0-9_]{0,30}"
RESERVED_KEYS = {"version", "release_date", "updated", "downloads", "features", "whatsnew", "notice", "platforms",
                 "dl_windows", "appimage_mb", "deb_mb", "arch_mb"}
MAX_ITEMS = 30
MAX_JSON = 200_000
MAX_IMAGE = 1_000_000
NEWS_RELEASES = (1, 10, 3)  # least, most, default
NEWS_CHARS = (80, 1000, 300)
KIND_FIELDS = {
    "asset": ({"id", "kind", "platform", "label", "asset", "note"}, {"os", "badge", "link", "missing"}),
    "store": ({"id", "kind", "platform", "label", "url", "note"}, {"os", "badge", "link"}),
    "docker": ({"id", "kind", "platform", "image", "note"}, {"label", "link"}),
    "build": ({"id", "kind", "platform", "note", "link"}, {"os", "label"}),
}


def text_problem(text, link=None, markup=True, empty=False):
    """None when `text` is acceptable (with its link key), else what is wrong."""
    if empty and text == "" and link is None:
        return None
    if not isinstance(text, str) or not re.fullmatch(TEXT, text):
        return "%r is not plain text (letters, digits, spaces, . , : ; ( ) ' ! ? / + %% = - # and dashes; " \
               "1 to 600 characters)" % (text,)
    parts = text.split("`")
    if len(parts) % 2 == 0:
        return "%r: unpaired backtick" % text
    if not markup and (len(parts) > 1 or "*" in text or "[" in text or "]" in text):
        return "%r: no `code`, **bold** or [link] here" % text
    links = 0
    for p in parts[0::2]:
        if p.replace("**", "").count("*") or p.count("**") % 2:
            return "%r: * only as **bold** pairs (within one piece between backticks)" % text
        found = re.findall(r"\[[^\[\]]+\]", p)
        rest = re.sub(r"\[[^\[\]]+\]", "", p)
        if "[" in rest or "]" in rest:
            return "%r: stray bracket" % text
        links += len(found)
    if links > 1:
        return "%r: more than one [link text]" % text
    if links and link is None:
        return "%r has a [link text] but no link key" % text
    if link is not None and not links:
        return "link %r given, but %r has no [link text]" % (link, text)
    if link is not None and link not in LINKS:
        return "link %r is not one of %s" % (link, ", ".join(sorted(LINKS)))
    return None


def _about_problem(data):
    if "about" in data and (not isinstance(data["about"], str) or len(data["about"]) > 2000):
        return "about must be a string of at most 2000 characters"
    return None


def downloads_problems(data):
    """Everything in a parsed site/downloads.json that the website would reject."""
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("downloads"), list):
        return ["expected {\"schema\": 1, \"downloads\": [...]}"]
    problems = [p for p in [_about_problem(data)] if p]
    problems += ["unknown field %r" % k for k in sorted(set(data) - {"schema", "about", "downloads"})]
    items = data["downloads"]
    if not 1 <= len(items) <= MAX_ITEMS:
        return problems + ["expected 1 to %d downloads, got %d" % (MAX_ITEMS, len(items))]
    ids, platforms = set(), []
    for n, e in enumerate(items):
        where = "downloads[%d]" % n
        if not isinstance(e, dict):
            problems.append(where + " is not an object")
            continue
        if isinstance(e.get("id"), str):
            where += " (%s)" % e["id"]
        kind = e.get("kind")
        if kind not in KINDS:
            problems.append("%s: kind must be one of %s" % (where, ", ".join(KINDS)))
            continue
        need, may = KIND_FIELDS[kind]
        problems += ["%s: missing field %r" % (where, k) for k in sorted(need - set(e))]
        problems += ["%s: field %r is not allowed for kind %s" % (where, k, kind) for k in sorted(set(e) - need - may)]
        if not isinstance(e.get("id"), str) or not re.fullmatch(ID, e["id"]):
            problems.append("%s: id must match %s" % (where, ID))
        elif e["id"] in ids:
            problems.append("%s: duplicate id" % where)
        else:
            ids.add(e["id"])
        for k in ("platform", "label", "badge"):
            if k in e and (not isinstance(e[k], str) or not re.fullmatch(NAME, e[k])):
                problems.append("%s: %s %r must match %s" % (where, k, e.get(k), NAME))
        if "os" in e and e["os"] not in OS_NAMES:
            problems.append("%s: os %r is not one of %s" % (where, e["os"], ", ".join(OS_NAMES)))
        if kind == "asset":
            a = e.get("asset")
            if not isinstance(a, str) or not re.fullmatch(ASSET, a) or a.count("{version}") != 1 \
                    or a.count("{n}") > 1:
                problems.append("%s: asset %r must be a file name with {version} once (and {n} at most once)"
                                % (where, a))
        if kind == "store" and (not isinstance(e.get("url"), str) or not re.fullmatch(STORE_URL, e["url"])):
            problems.append("%s: url %r must be https on one of %s" % (where, e.get("url"), ", ".join(STORE_HOSTS)))
        note = e.get("note")
        if kind == "docker":
            if not isinstance(e.get("image"), str) or not re.fullmatch(DOCKER_IMAGE, e["image"]):
                problems.append("%s: image %r must match %s" % (where, e.get("image"), DOCKER_IMAGE))
            if not isinstance(note, str) or note.count("{image}") != 1:
                problems.append("%s: a docker note must contain {image} once" % where)
            else:
                note = note.replace("{image}", "image")
        p = text_problem(note, e.get("link"))
        if p:
            problems.append("%s: note: %s" % (where, p))
        if "missing" in e:
            m = e["missing"]
            if not isinstance(m, dict) or set(m) - {"text", "link"} or "text" not in m:
                problems.append("%s: missing must be {\"text\": ..., \"link\": ...}" % where)
            else:
                p = text_problem(m["text"], m.get("link"))
                if p:
                    problems.append("%s: missing: %s" % (where, p))
        plat = e.get("platform")
        if isinstance(plat, str):
            if platforms and platforms[-1] != plat and plat in platforms:
                problems.append("%s: platform %r again after another one (a platform is one row: keep its "
                                "entries together)" % (where, plat))
            platforms.append(plat)
    return problems


def site_problems(data):
    """Everything in a parsed site/site.json that the website would reject."""
    if not isinstance(data, dict) or data.get("schema") != 1:
        return ["expected {\"schema\": 1, ...}"]
    problems = [p for p in [_about_problem(data)] if p]
    allowed = {"schema", "about", "texts", "notice", "notice_link", "news", "checksums"}
    problems += ["unknown field %r" % k for k in sorted(set(data) - allowed)]
    texts = data.get("texts", {})
    if not isinstance(texts, dict) or len(texts) > MAX_ITEMS:
        problems.append("texts must be an object with at most %d entries" % MAX_ITEMS)
    else:
        for k, v in texts.items():
            if not re.fullmatch(TEXT_KEY, k) or k in RESERVED_KEYS or k.startswith("dl_"):
                problems.append("texts: key %r must match %s and not be a reserved marker name" % (k, TEXT_KEY))
            p = text_problem(v)
            if p:
                problems.append("texts.%s: %s" % (k, p))
    if "notice" in data:
        p = text_problem(data["notice"], data.get("notice_link"), empty=True)
        if p:
            problems.append("notice: " + p)
    elif "notice_link" in data:
        problems.append("notice_link without notice")
    news = data.get("news", {})
    if not isinstance(news, dict) or set(news) - {"releases", "chars", "patches"}:
        problems.append("news must be {\"releases\": N, \"chars\": N, \"patches\": false}")
    elif not isinstance(news.get("patches", False), bool):
        problems.append("news.patches must be true or false")
    else:
        for k, (lo, hi, _) in (("releases", NEWS_RELEASES), ("chars", NEWS_CHARS)):
            v = news.get(k, lo)
            if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                problems.append("news.%s must be a whole number from %d to %d" % (k, lo, hi))
    if not isinstance(data.get("checksums", False), bool):
        problems.append("checksums must be true or false")
    return problems


def features_problems(data):
    """Everything in a parsed site/features.json that the website would reject."""
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("features"), list):
        return ["expected {\"schema\": 1, \"features\": [...]}"]
    problems = [p for p in [_about_problem(data)] if p]
    problems += ["unknown field %r" % k for k in sorted(set(data) - {"schema", "about", "features", "more"})]
    items = data["features"]
    if not 1 <= len(items) <= MAX_ITEMS:
        return problems + ["expected 1 to %d features, got %d" % (MAX_ITEMS, len(items))]
    seen = set()
    for n, f in enumerate(items):
        where = "features[%d]" % n
        if not isinstance(f, dict):
            problems.append(where + " is not an object")
            continue
        fields = {"image", "tab", "title", "text", "alt"}
        problems += ["%s: missing field %r" % (where, k) for k in sorted(fields - set(f))]
        problems += ["%s: unknown field %r" % (where, k) for k in sorted(set(f) - fields)]
        img = f.get("image")
        if not isinstance(img, str) or not re.fullmatch(IMAGE_ID, img):
            problems.append("%s: image must match %s" % (where, IMAGE_ID))
        elif img in seen:
            problems.append("%s: image %r used twice" % (where, img))
        else:
            seen.add(img)
        if not isinstance(f.get("tab"), str) or not re.fullmatch(NAME, f["tab"]):
            problems.append("%s: tab %r must match %s" % (where, f.get("tab"), NAME))
        for k, markup in (("title", False), ("text", True), ("alt", False)):
            p = text_problem(f.get(k), None, markup=markup)
            if p:
                problems.append("%s: %s: %s" % (where, k, p))
    if "more" in data:
        m = data["more"]
        if not isinstance(m, dict) or set(m) - {"text", "link"} or "text" not in m:
            problems.append("more must be {\"text\": ..., \"link\": ...}")
        else:
            p = text_problem(m["text"], m.get("link"))
            if p:
                problems.append("more: " + p)
    return problems


def webp_size(data):
    """(width, height) of a WebP file, or None when the bytes are not a plausible WebP image."""
    if not isinstance(data, (bytes, bytearray)) or len(data) < 30 or len(data) > MAX_IMAGE:
        return None
    if data[0:4] != b"RIFF" or data[8:12] != b"WEBP" or int.from_bytes(data[4:8], "little") + 8 != len(data):
        return None
    chunk = data[12:16]
    if chunk == b"VP8X":
        w, h = 1 + int.from_bytes(data[24:27], "little"), 1 + int.from_bytes(data[27:30], "little")
    elif chunk == b"VP8 " and data[23:26] == b"\x9d\x01\x2a":
        w, h = int.from_bytes(data[26:28], "little") & 0x3FFF, int.from_bytes(data[28:30], "little") & 0x3FFF
    elif chunk == b"VP8L" and data[20] == 0x2F:
        bits = int.from_bytes(data[21:25], "little")
        w, h = (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
    else:
        return None
    return (w, h) if 16 <= w <= 4096 and 16 <= h <= 4096 else None
# ==== END shared rules ====


def _load(path, checker):
    rel = os.path.relpath(path, ROOT)
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        return None, ["cannot read %s (%s)" % (rel, e.strerror)]
    if len(text.encode("utf-8")) > MAX_JSON:
        return None, ["%s is larger than the website accepts (%d bytes)" % (rel, MAX_JSON)]
    try:
        data = json.loads(text)
    except ValueError as e:
        return None, ["%s is not valid JSON: %s" % (rel, e)]
    return data, ["%s: %s" % (rel, p) for p in checker(data)]


def image_problems(features):
    """site/img/<image>-960.webp (needed) and -1920.webp (optional) for every card, as the website checks them."""
    problems, wanted = [], set()
    for f in features:
        for width in (960, 1920):
            name = "%s-%d.webp" % (f["image"], width)
            wanted.add(name)
            path = os.path.join(SITE_IMG, name)
            if not os.path.exists(path):
                if width == 960:
                    problems.append("site/img/%s is missing (tools/site_images.py --write)" % name)
                continue
            with open(path, "rb") as fh:
                data = fh.read(MAX_IMAGE + 1)
            size = webp_size(data)
            if size is None:
                problems.append("site/img/%s is not a WebP of at most %d bytes" % (name, MAX_IMAGE))
            elif size[0] > width or (width == 1920 and size[0] <= 960):
                problems.append("site/img/%s is %dx%d, not right for %dw" % (name, size[0], size[1], width))
    if os.path.isdir(SITE_IMG):
        for name in sorted(set(os.listdir(SITE_IMG)) - wanted):
            problems.append("site/img/%s is not used by site/features.json" % name)
    return problems


def check_site_files():
    """Problems of site/downloads.json, site/site.json and site/features.json (+ images); missing files are fine
    for site.json and features.json (the website then keeps what it has)."""
    problems = []
    _, p = _load(DOWNLOADS, downloads_problems)
    problems += p
    if os.path.exists(SITE_JSON):
        data, p = _load(SITE_JSON, site_problems)
        problems += p
        if data and not p:
            for k in (data.get("texts") or {}):
                if k in KEYS:
                    problems.append("site/site.json: texts.%s has the name of a figure" % k)
    if os.path.exists(FEATURES):
        data, p = _load(FEATURES, features_problems)
        problems += p
        if data and not p:
            problems += image_problems(data["features"])
    return problems


def shared_rules(text):
    a, b = text.find("# ==== BEGIN shared rules ===="), text.find("# ==== END shared rules ====")
    return text[a:b] if 0 <= a < b else None


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
    with open(path, encoding="utf-8") as f:
        theirs = shared_rules(f.read())
    with open(os.path.abspath(__file__), encoding="utf-8") as f:
        ours = shared_rules(f.read())
    if theirs is None or theirs != ours:
        problems.append("the shared rules block differs from the website's update_site.py (copy it over)")
    try:
        with open(DOWNLOADS, encoding="utf-8") as f:
            entries = site.parse_downloads(f.read())
        print("  downloads %s" % ", ".join(e["id"] for e in entries))
    except (OSError, ValueError) as e:
        problems.append("the website's update_site.py rejects site/downloads.json: %s" % e)
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
    if a.check:
        problems += check_site_files()
    if a.website:
        problems += compare_website(a.website, data)
    if problems:
        print("site_figures: site/figures.json or another file in site/ is not right:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        if a.check:
            print("Fix README.md if it is wrong, then run tools/site_figures.py --write.", file=sys.stderr)
        return 1
    if a.check:
        print("site_figures: site/figures.json matches README.md (%d figures), the other site/ files are valid"
              % len(KEYS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
