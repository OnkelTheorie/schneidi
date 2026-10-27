#!/usr/bin/env python3
"""Run the test suite so that a busy machine does not produce false failures.

  tools/run_tests.py [build-dir] [-j N] [-R regex]

1. every test without the label "timing" in parallel (-j N, default: CPU count - 2),
2. the "timing" tests (real time, other processes) one by one,
3. what failed once more, one by one: a test that passes then counts as flaky (shown, exit code stays 0),
   one that fails again is a real failure (exit code 1).
Results: <build-dir>/Testing/Temporary/LastTest*.log as usual; a summary at the end.
"""
import argparse, os, re, subprocess, sys


def ctest(build, args):
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', SDL_AUDIODRIVER='dummy')
    env.setdefault('LANG', 'de_DE.UTF-8')
    p = subprocess.run(['ctest', '--test-dir', build, '--timeout', '900'] + args, env=env, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(p.stdout)
    sys.stdout.flush()
    return re.findall(r'\d+ - (\S+) \((?:Failed|Timeout|SEGFAULT|Subprocess aborted|Child aborted|Exception|Not Run)',
                      p.stdout), p.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build', nargs='?', default='build')
    ap.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('-R', default='')
    a = ap.parse_args()
    select = ['-R', a.R] if a.R else []
    failed, _ = ctest(a.build, ['-j', str(a.j), '-LE', 'timing'] + select)
    timing, _ = ctest(a.build, ['-L', 'timing'] + select)
    failed += timing
    flaky, real = [], []
    for name in failed:
        again, _ = ctest(a.build, ['-R', '^' + re.escape(name) + '$'])
        (real if again else flaky).append(name)
    print('\n==== run_tests: ' + ('all passed' if not failed else '%d failed in the first run' % len(failed)))
    if flaky:
        print('flaky (failed under load, passed alone): ' + ', '.join(flaky))
    if real:
        print('FAILED (also alone): ' + ', '.join(real))
    return 1 if real else 0


sys.exit(main())
