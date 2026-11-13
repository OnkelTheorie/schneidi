# Startup measuring tools (Linux)

- `measure.py N label -- cmd…` runs a command N times and timestamps every `startup:` stderr line
  (`SCHNEIDI_STARTUP_TRACE=2`) from process spawn (`ext`) and from `main()` (`int`); prints medians.
  `GAP=<s>` sets the pause between runs.
- `sampler.cpp` → `g++ -O2 -shared -fPIC -o libsampler.so sampler.cpp`: LD_PRELOAD sampling profiler for the main
  thread (0.5 ms, `SAMPLER_STOP_MS`, `SAMPLER_OUT`). With xvfb-run preload it into schneidi only:
  `xvfb-run … env LD_PRELOAD=$PWD/libsampler.so build/schneidi`.
- `analyze.py samples.txt [from to]`: inclusive time per function (Qt names approximate without debug symbols).
- `lines.py samples.txt from to libschneidi_lib [depth]`: each sample attributed to the innermost schneidi file:line.
- `perf record --call-graph dwarf` works as well where perf is installed.

Typical run: `SCHNEIDI_TEST_SETTINGS=<copy of the settings> SCHNEIDI_STARTUP_TRACE=2 SCHNEIDI_LIVE=0
xvfb-run -a -s "-screen 0 1920x1080x24" tools/startup/measure.py 5 label -- build/schneidi`.
On a real X11 screen the monitor must be on: with DPMS off, KWin + NVIDIA wait for vblank timeouts and every new
window takes 0.6–1.7 s.
