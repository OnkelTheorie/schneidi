#!/usr/bin/env python3
"""Which tests does a change touch? Changed files (git diff against a base, plus uncommitted and new files) -> the test
programs that compile them, from ninja's own dependency records (`ninja -t deps`) of a build folder.

  tools/affected_tests.py <base-ref> [build] [--run] [-j N] [--mem 4G] [--all-on-config]

Prints a `--target ...` list for cmake --build and a `-R ...` regex for ctest/tools/run_tests.py (all CTest variants
of a program: _de_DE, _no_display, ...). With --run it builds those targets and runs them through tools/run_tests.py
(on Linux in a memory-capped systemd scope when systemd-run is there).

Rules: a header or test source counts for every test whose objects include it (directly or not). A library source
(src/**/Foo.cpp) has no test that compiles it, so it counts like its header: Foo.h, or the header in the same folder
whose name is the longest prefix of it (EditorTransitions.cpp -> Editor.h). i18n/*.json adds test_i18n. A changed
CMakeLists.txt/cmake/*.cmake is only reported (--all-on-config: then every test). The build folder must have been
built once (ninja records dependencies while compiling); a test that was never compiled counts when its source changed.
"""
import argparse, functools, json, os, re, shutil, subprocess, sys

def _root():
    # the checkout we are in (a worktree), else the one this script belongs to
    r = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


ROOT = _root()
REAL_ROOT = os.path.realpath(ROOT)


def git(*args):
    return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True, check=True).stdout


def changed_files(base):
    fork = git('merge-base', base, 'HEAD').strip()  # what this branch changed, not what base did since
    files = set(git('diff', '--name-only', fork).split())  # committed since the fork + uncommitted
    files |= set(git('ls-files', '--others', '--exclude-standard').split())  # new, not yet added
    return sorted(f.replace('\\', '/') for f in files)


@functools.lru_cache(maxsize=None)
def norm(path, build):
    p = path if os.path.isabs(path) else os.path.join(build, path)
    p = os.path.normpath(os.path.realpath(p))
    root = REAL_ROOT
    if os.path.commonpath([p, root]) != root:
        return None  # system/Qt headers
    return os.path.relpath(p, root).replace('\\', '/')


def ninja_deps(build):
    """({target: files its objects include}, {source: files it includes}) for every object under CMakeFiles/<t>.dir/,
    repo-relative"""
    out = subprocess.run(['ninja', '-C', build, '-t', 'deps'], capture_output=True, text=True).stdout
    deps, per_source, target, source = {}, {}, None, None
    for line in out.splitlines():
        if not line.strip():
            target = source = None
            continue
        if not line.startswith(' '):
            m = re.search(r'CMakeFiles/([^/]+)\.dir/(.+)\.o(bj)?:', line)
            target = m.group(1) if m else None
            source = m.group(2) if m else None
            if target:
                deps.setdefault(target, set())
                per_source.setdefault(source, set())
            continue
        if target:
            f = norm(line.strip(), build)
            if f:
                deps[target].add(f)
                per_source[source].add(f)
    return deps, per_source


def header_for(source, headers):
    """Header standing for a library source: same stem, else the longest same-folder header stem that prefixes it"""
    d, name = os.path.split(source)
    stem = os.path.splitext(name)[0]
    best = None
    for h in headers:
        hd, hn = os.path.split(h)
        hs = os.path.splitext(hn)[0]
        if hd == d and stem.startswith(hs) and (best is None or len(hs) > len(os.path.splitext(os.path.basename(best))[0])):
            best = h
    return best


def ctest_names(build):
    """{program name: [ctest names]} from ctest's own listing"""
    p = subprocess.run(['ctest', '--test-dir', build, '--show-only=json-v1'], capture_output=True, text=True)
    names = {}
    try:
        tests = json.loads(p.stdout).get('tests', [])
    except ValueError:
        return names
    for t in tests:
        cmd = t.get('command') or []
        if cmd:
            prog = os.path.basename(cmd[0])
            prog = prog[:-4] if prog.lower().endswith('.exe') else prog
            names.setdefault(prog, []).append(t['name'])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', help='git ref to compare with, e.g. dev-0.19 or HEAD~3')
    ap.add_argument('build', nargs='?', default='build')
    ap.add_argument('--run', action='store_true', help='build the targets and run their tests')
    ap.add_argument('-j', type=int, default=0, help='build jobs for --run (default: cmake/ninja default)')
    ap.add_argument('--mem', default='4G', help='MemoryMax of the systemd scope for --run (Linux)')
    ap.add_argument('--all-on-config', action='store_true', help='a changed CMakeLists.txt selects every test')
    a = ap.parse_args()
    build = os.path.abspath(a.build if os.path.isabs(a.build) else os.path.join(ROOT, a.build))
    if not os.path.exists(os.path.join(build, 'build.ninja')):
        print(f'affected_tests: {build} is no Ninja build folder')
        return 2

    files = changed_files(a.base)
    deps, per_source = ninja_deps(build)
    tests = {t for t in deps if t.startswith('test_')}
    tests |= {os.path.splitext(os.path.basename(f))[0] for f in git('ls-files', 'tests/test_*.cpp').split()}
    headers = sorted({f for s in deps.values() for f in s if f.endswith('.h')} |
                     {f for f in git('ls-files', 'src').split() if f.endswith('.h')})

    hit, notes = set(), []
    config = [f for f in files if os.path.basename(f) == 'CMakeLists.txt' or f.startswith('cmake/')]
    for f in files:
        look = f
        if not os.path.exists(os.path.join(ROOT, f)):
            continue  # deleted: whoever included it changed too
        if f.startswith('src/') and f.endswith(('.cpp', '.c')):
            look = header_for(f, headers)
            if not look:  # e.g. cli/EditOpsComp.cpp: the headers of its own folder that it includes
                own = {h for h in per_source.get(f, ()) if h.endswith('.h') and os.path.dirname(h) == os.path.dirname(f)}
                for t in tests:
                    if own & deps.get(t, set()):
                        hit.add(t)
                if not own:
                    notes.append(f'{f}: no header stands for it (no test selected)')
                continue
        if f.startswith('tests/') and re.fullmatch(r'tests/test_\w+\.cpp', f):
            hit.add(os.path.splitext(os.path.basename(f))[0])  # also when never compiled
        if f.startswith('i18n/') and f.endswith('.json'):
            hit.add('test_i18n')
        for t in tests:
            if look in deps.get(t, ()):
                hit.add(t)
    if config:
        notes.append('build configuration changed: ' + ', '.join(config) +
                      (' -> every test' if a.all_on_config else ' (only reported; --all-on-config selects every test)'))
        if a.all_on_config:
            hit |= tests
    hit &= tests

    names = ctest_names(build)
    ctests = sorted(n for t in hit for n in names.get(t, [t]))
    print(f'{len(files)} changed files, {len(hit)} test programs, {len(ctests)} ctest tests')
    for n in notes:
        print('note: ' + n)
    if not hit:
        return 0
    targets = sorted(hit)
    regex = '^(' + '|'.join(re.escape(n) for n in ctests) + ')$'
    print('--target ' + ' '.join(targets))
    print("-R '" + regex + "'")
    if not a.run:
        return 0

    prefix = []
    if sys.platform.startswith('linux') and shutil.which('systemd-run'):
        prefix = ['systemd-run', '--user', '--scope', '-q', '-p', f'MemoryMax={a.mem}', '-p', 'MemorySwapMax=0',
                  'nice', '-n', '19']
    cmd = prefix + ['cmake', '--build', build, '--target'] + targets + (['-j', str(a.j)] if a.j else [])
    print('+ ' + ' '.join(cmd), flush=True)
    if subprocess.run(cmd).returncode != 0:
        return 1
    run = prefix + [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'run_tests.py'), build,
                    '-R', regex]
    print('+ ' + ' '.join(run), flush=True)
    return subprocess.run(run).returncode


sys.exit(main())
