#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""One benchmark run of schneidi with a fixed suite, so every version is measured the same way.

    tools/bench.py --build <build dir with -DSCHNEIDI_TESTS=ON> --material <material.json> --out <result dir>
                   [--compare <older result.json>] [--only startup,export,...] [--runs 3]

material.json names the test files (absolute paths, or relative to the json file):
    {"name": "thinkstation-1", "media": ["val-h264.mp4", ...], "project": "val.schneidi",
     "big_project": "kill-edit.schneidi", "seconds": 10}
  media        files for import (probe), startup and export next to plain FFmpeg
  project      a normal edit for playback, export and the CLI render
  big_project  optional: many clips and titles (memory of a render)
  seconds      length measured of exports and playback

Writes result.json (all numbers), summary.md (table, with --compare the change per number) and logs/ (the raw output
of every program). With --compare a number more than 15 % worse than before is marked and the exit code is 3.
Linux only (peak memory of a child process from wait4).
"""
import argparse
import datetime
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

SUITE_VERSION = 1   # raise when what a step measures changes (results of different versions do not compare)
WORSE_LIMIT = 0.15  # the speed brake: more than 15 % worse than the compared run
STEPS = ["startup", "probe", "export", "playback", "render", "memory", "compose"]

# Which direction is better per number (anything not listed is information only)
LOWER_BETTER = ("_ms", "_mb", "_s", "ratio")
HIGHER_BETTER = ("_fps",)


def run(cmd, log, timeout=1800, env=None):
    """Runs cmd, writes its output to log; returns (exit code, output, seconds, peak RSS of the process in MB)."""
    start = time.monotonic()
    with open(log, "w") as f:
        f.write("$ " + " ".join(map(str, cmd)) + "\n")
        f.flush()
        p = subprocess.Popen(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        out = []
        deadline = start + timeout
        for line in iter(p.stdout.readline, b""):
            text = line.decode(errors="replace")
            out.append(text)
            f.write(text)
            if time.monotonic() > deadline:
                p.kill()
                break
        _, status, usage = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
    return p.returncode, "".join(out), time.monotonic() - start, usage.ru_maxrss / 1024


def number(pattern, text, cast=float):
    m = re.search(pattern, text, re.M)
    return cast(m.group(1)) if m else None


def environment(build):
    cpu = ""
    try:
        cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
    except (OSError, StopIteration):
        pass
    root = Path(__file__).resolve().parent.parent
    describe = subprocess.run(["git", "-C", str(root), "describe", "--tags", "--always", "--dirty"],
                              capture_output=True, text=True).stdout.strip()
    cache = Path(build) / "CMakeCache.txt"
    build_type = number(r"^CMAKE_BUILD_TYPE:STRING=(\S+)", cache.read_text(), str) if cache.exists() else None
    return {
        "date": datetime.datetime.now().isoformat(timespec="seconds"),
        "host": platform.node(), "cpu": cpu, "threads": len(os.sched_getaffinity(0)),
        "kernel": platform.release(), "git": describe, "build_type": build_type,
        "load_before": os.getloadavg()[0],
        "in_container": Path("/.dockerenv").exists(),
    }


def step_startup(b, m, logs, runs):
    """Window shown (ms, as the app logs it) and peak memory, empty and with a clip; median of the runs."""
    res = {}
    for case, extra in (("empty", []), ("clip", ["--demo", m["media"][0]])):
        shown, peak = [], []
        for i in range(runs):
            shot = logs / f"startup-{case}-{i}.png"
            code, out, _, rss = run([b.app, "--screenshot", shot, *extra], logs / f"startup-{case}-{i}.log", 120)
            ms = number(r"(?:Startzeit|Start time).*?(\d+) ms\s*$", out)
            if code == 0 and ms is not None:
                shown.append(ms)
                peak.append(rss)
            shot.unlink(missing_ok=True)
        if shown:
            res[f"{case}_window_ms"] = statistics.median(shown)
            res[f"{case}_peak_mb"] = round(statistics.median(peak))
    return res


def step_probe(b, m, logs, runs):
    code, out, _, _ = run([b.tests / "bench_probe", *m["media"], "--reps", runs], logs / "probe.log")
    return {"per_file_ms": number(r"own [\d.]+ ms \(([\d.]+) per file\)", out),
            "import_all_ms": number(r"import of \d+ files.*own ([\d.]+) ms", out)}


def export_numbers(out):
    return {"fps": number(r"^schneidi .*?=\s*([\d.]+) fps", out),
            "first_frame_ms": number(r"^1st frame\s+(\d+) ms", out),
            "cpu_cores": number(r"CPU ([\d.]+) cores", out),
            "peak_mb": number(r"peak (\d+) MB", out),
            "vs_ffmpeg_ratio": number(r"needs ([\d.]+)x the time", out)}


def step_export(b, m, logs, runs):
    res = {}
    targets = [("media", m["media"][0], ["--ref"])]
    if m.get("project"):
        targets.append(("project", m["project"], ["--no-ref"]))
    for name, src, ref in targets:
        best = None
        for i in range(runs):
            code, out, _, _ = run([b.tests / "bench_export", src, "--seconds", m["seconds"], *ref],
                                  logs / f"export-{name}-{i}.log")
            n = export_numbers(out)
            if code == 0 and n["fps"] and (best is None or n["fps"] > best["fps"]):
                best = n  # the least disturbed run
        for k, v in (best or {}).items():
            if v is not None:
                res[f"{name}_{k}"] = v
    return res


def step_playback(b, m, logs, runs):
    if not m.get("project"):
        return {}
    code, out, _, _ = run([b.tests / "bench_playback", m["project"], m["seconds"]], logs / "playback.log")
    gaps = [float(g) for g in re.findall(r"longest gap\s+([\d.]+) ms", out)[1:]]  # second 0 includes the start
    return {"shown_fps": number(r"=\s*([\d.]+) fps", out),
            "worst_gap_ms": max(gaps) if gaps else None,
            "behind_sound_frames": number(r"picture behind the sound: mean ([\d.]+)", out)}


def step_render(b, m, logs, runs):
    """The CLI render (what an AI uses): time and peak memory with all threads and with 2."""
    res = {}
    for name in ("project", "big_project"):
        if not m.get(name):
            continue
        for threads in (0, 2):
            out_file = logs / f"render-{name}.mp4"
            cmd = [b.cli, "render", "--project", m[name], "--out", out_file, "--overwrite", "--encoder", "software",
                   "--to", f"{m['seconds']}s"]
            if threads:
                cmd += ["--threads", threads]
            code, out, secs, rss = run(cmd, logs / f"render-{name}-threads-{threads or 'all'}.log")
            out_file.unlink(missing_ok=True)
            if code == 0:
                tag = f"{name}_threads_{threads or 'all'}"
                res[f"{tag}_s"] = round(secs, 2)
                res[f"{tag}_peak_mb"] = round(rss)
    return res


def step_memory(b, m, logs, runs):
    code, out, _, _ = run([b.tests / "bench_memory", "--seconds", m["seconds"], m["media"][0]], logs / "memory.log")
    peaks = [int(p) for p in re.findall(r"peak\s+(\d+)\)", out)]
    return {"peak_mb": max(peaks) if peaks else None,
            "after_release_mb": number(r"^IdleRelease after stopping\s+RSS\s+(\d+) MB", out),
            "end_mb": number(r"^end\s+RSS\s+(\d+) MB", out)}


def step_compose(b, m, logs, runs):
    code, out, _, _ = run([b.tests / "bench_compose", 120], logs / "compose.log")
    return {"rgba_1080p_ms": number(r"1920x1080 RGBA .*own\s+([\d.]+) ms", out),
            "yuv_1080p_ms": number(r"1920x1080 YUV.*own\s+([\d.]+) ms", out),
            "rgba_540p_ms": number(r"960x540 RGBA .*own\s+([\d.]+) ms", out)}


def direction(key):
    if key.endswith(HIGHER_BETTER):
        return 1
    if key.endswith(LOWER_BETTER) and not key.endswith("cpu_cores"):
        return -1
    return 0


def compare(now, old):
    """Rows (step, key, old, new, change, worse?) for numbers in both runs."""
    rows = []
    for step, values in now["steps"].items():
        before = old.get("steps", {}).get(step, {})
        for key, v in values.items():
            o = before.get(key)
            if not isinstance(v, (int, float)) or not isinstance(o, (int, float)) or o == 0:
                continue
            change = (v - o) / o
            worse = direction(key) * change < -WORSE_LIMIT
            rows.append((step, key, o, v, change, worse))
    return rows


def summary(result, rows):
    e = result["environment"]
    lines = [f"# Benchmark {e['git']} on {e['host']} ({e['date'][:10]})", "",
             f"CPU {e['cpu']}, {e['threads']} threads, build {e['build_type']}, "
             f"{'container' if e['in_container'] else 'host'}, load before {e['load_before']:.1f}; "
             f"material {result['material']['name']}, suite {result['suite']}.", ""]
    if rows:
        lines += [f"Compared with {result['compared_with']}:", "", "| step | number | before | now | change |",
                  "|---|---|---:|---:|---:|"]
        for step, key, o, v, change, worse in rows:
            lines.append(f"| {step} | {key} | {o:g} | {v:g} | {change:+.0%}{' **worse**' if worse else ''} |")
    else:
        lines += ["| step | number | value |", "|---|---|---:|"]
        for step, values in result["steps"].items():
            for key, v in values.items():
                lines.append(f"| {step} | {key} | {v} |")
    if result["errors"]:
        lines += ["", "Errors: " + "; ".join(f"{k}: {v}" for k, v in result["errors"].items())]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--material", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--compare")
    ap.add_argument("--only", help="comma-separated steps: " + ",".join(STEPS))
    ap.add_argument("--runs", type=int, default=3)
    a = ap.parse_args()

    build = Path(a.build).resolve()
    b = argparse.Namespace(app=build / "schneidi", cli=build / "schneidi-cli", tests=build / "tests")
    missing = [p for p in (b.app, b.cli, b.tests / "bench_export") if not p.exists()]
    if missing:
        sys.exit(f"missing in the build: {', '.join(map(str, missing))} (configure with -DSCHNEIDI_TESTS=ON)")
    mpath = Path(a.material).resolve()
    m = json.loads(mpath.read_text())
    resolve = lambda p: str((mpath.parent / p).resolve())
    m["media"] = [resolve(p) for p in m["media"]]
    for k in ("project", "big_project"):
        if m.get(k):
            m[k] = resolve(m[k])
    m.setdefault("seconds", 10)

    out = Path(a.out)
    logs = out / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    # Own settings and caches, so the user's profile neither changes the numbers nor gets touched
    home = out / "home"
    for var, sub in (("XDG_CONFIG_HOME", ".config"), ("XDG_DATA_HOME", ".local/share"), ("XDG_CACHE_HOME", ".cache")):
        os.environ[var] = str(home / sub)

    result = {"suite": SUITE_VERSION, "environment": environment(build), "material": m, "steps": {}, "errors": {}}
    for step in (a.only.split(",") if a.only else STEPS):
        print(f">> {step}", flush=True)
        try:
            values = globals()[f"step_{step}"](b, m, logs, a.runs)
            result["steps"][step] = {k: v for k, v in values.items() if v is not None}
            if not result["steps"][step]:
                result["errors"][step] = "no numbers (see logs/)"
        except Exception as ex:  # one broken step must not cost the others
            result["errors"][step] = repr(ex)
        print(json.dumps(result["steps"].get(step, {})), flush=True)
    shutil.rmtree(home, ignore_errors=True)

    rows = []
    if a.compare:
        old = json.loads(Path(a.compare).read_text())
        result["compared_with"] = f"{old['environment']['git']} ({old['environment']['host']}, {old['environment']['date'][:10]})"
        if old.get("suite") != SUITE_VERSION or old.get("material", {}).get("name") != m.get("name"):
            result["errors"]["compare"] = "other suite or material: numbers do not compare"
        else:
            rows = compare(result, old)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    (out / "summary.md").write_text(summary(result, rows))
    print((out / "summary.md").read_text())
    if any(r[5] for r in rows):
        sys.exit(3)
    if result["errors"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
