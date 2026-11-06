# Benchmarks

Every benchmark run schneidi has on record, newest at the bottom of each table. Nothing here is a highlight reel: runs
that were disturbed by other load, partial runs and numbers that got worse are listed too. Only runs that were actually
measured appear; versions without a run have no row. Dates are ISO.

## Machines

| Name | Hardware |
|---|---|
| PC | Ryzen 5 5600X, GTX 1070, 16 GB RAM, Linux |
| Server | Ryzen 5 PRO 2400G (4 cores / 8 threads, Vega 11), 16 GB RAM, Linux, measured inside a Docker container, no GPU decoding or encoding |
| Pi | Raspberry Pi Zero 2 W: 4 x 1 GHz Cortex-A53, 512 MB RAM (415 MB usable) + swap on SD card, Debian 13 ARM64, no hardware decoding |

## Method

- **Fixed material and suite.** The server history uses one suite (`tools/bench.py`, suite version 1) and one fixed set of
  files ("material thinkstation-1": a 1080p60 H.264 game recording, a 1080p60 CS2 clip with 5 audio tracks, a browser
  recording, an HEVC copy; one normal project; one large 1080x1920 project with 27 clips and about 60 titles). Only runs
  with the same machine, the same material and the same suite version are compared. If a suite or the material changes,
  it gets a new version/name and old numbers are not mixed in.
- **What a step measures.** *startup*: time until the window is shown (median of the runs), empty and with a clip.
  *probe*: reading file information. *export*: 10 s of video written, frames per second for a plain file and for the
  project, time to the first frame (best run of the runs). *playback*: frames shown per second and the longest gap between
  two shown frames. *render*: command-line render of the project and of the large project with all threads, time and peak
  RAM. *memory*: peak RAM after playback and after memory was given back. *compose*: milliseconds to mix 4 tracks of one frame.
- **Build.** `RelWithDebInfo`, built from the commit named in the row; the server runs use Docker for constant conditions.
- **Load.** The run records the system load before it starts. Rows with a high load are marked; their time numbers are not
  reliable (RAM and compose numbers hold up better).
- **Raspberry Pi and PC tests** are single measurements on real hardware with the tools named in the sections below, not
  runs of the suite.

### Reproduce

```
cmake -S . -B build -G Ninja -DSCHNEIDI_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
tools/bench.py --build build --material material.json --out results/<name> [--compare older/result.json] [--only startup,export] [--runs 3]
```

`material.json` lists the test files (`media`, `project`, optional `big_project`, `seconds`). The script writes
`result.json`, `summary.md` and logs; with `--compare` a number more than 15 % worse than the older run is marked and the
exit code is 3. The material files themselves are not part of the repository, so a run on your own files is only
comparable with your own earlier runs. Linux only.

## Server (Ryzen 5 PRO 2400G, Docker), suite 1

Columns: window shown, empty / with clip (ms) | probe per file (ms) | export fps, plain file / project | time to first
frame, file / project (ms) | playback shown fps / longest gap (ms) | render of the project, all threads (s) | render of the
large project, all threads: s / peak RAM (MB) | memory peak / after giving back (MB) | compose 1080p RGBA / YUV (ms).

| Version | Date | Load | Start (ms) | Probe | Export fps | 1st frame (ms) | Playback | Render (s) | Large project | Memory (MB) | Compose (ms) |
|---|---|---:|---|---:|---|---|---|---:|---|---|---|
| 0.14.1 [a] | 2026-10-30 | 8.7 | 500 / 503 | 0.46 | 31.6 / 33 | 244 / 246 | 59.9 / 21.5 | 2.45 | 58.57 / 4348 | 1238 / 531 | 18.75 / 38.45 |
| 0.15.0-dev (69191ed) [a] | 2026-10-30 | 4.5 | 472 / 400 | 0.31 | 28.1 / 29.5 | 424 / 429 | 57.2 / 44.5 | 2.2 | 37.45 / 1562 | 1198 / 820 | 22.64 / 34.93 |
| 0.15.0 + speed fixes (77591ea) | 2026-10-31 | 0.3 | 403 / 394 | 0.31 | 32.2 / 33.6 | 130 / 219 | 61.6 / 40.2 | 2.14 | 37.43 / 1563 | 1156 / 641 | 19.01 / 30.7 |
| 0.15.0 | 2026-11-01 | 0.6 | 393 / 392 | 0.5 | 32.2 / 33.5 | 131 / 225 | 62.9 / 19.9 | 2.16 | 38.02 / 1533 | 1167 / 653 | 19.31 / 31.43 |
| 0.15.1 | 2026-11-02 | 0.5 | 393 / 407 | 0.3 | 32 / 33.5 | 122 / 210 | 61.7 / 21.3 | 2.16 | 38.02 / 1547 | 1158 / 635 | 18.99 / 31.42 |
| 0.16-dev (3f197ea) [b] | 2026-11-02 | 13.2 | 759 / 693 | 0.98 | 22.6 / 24.3 | 131 / 272 | 51.4 / 506.8 | 4.06 | 70.79 / 1533 | 1178 / 648 | 19.74 / 31.48 |
| 0.16-dev (3e60f00) [b] | 2026-11-03 | 15.3 | 444 / 449 | 0.32 | 32.1 / 33.6 | 109 / 230 | 59.2 / 22.4 | 2.79 | 38.04 / 1537 | 1143 / 631 | 18.98 / 30.61 |
| 0.16.0 (e661a4f) [c] | 2026-11-03 | 0.5 | 444 / 444 | - | - / - | - / - | - / - | 2.22 | 38.06 / 1880 | - / - | - / - |
| 0.16.1 [b] | 2026-11-05 | 16.5 | 412 / 391 | 0.33 | 32 / 33.4 | 126 / 231 | 60.8 / 21.5 | 2.19 | 37.28 / 1550 | 1153 / 633 | 19.13 / 30.62 |
| 0.17.0 (791ddfd) | 2026-11-05 | 4.3 | 391 / 400 | 0.32 | 31.8 / 33.5 | 143 / 212 | 59.7 / 22.8 | 2.17 | 37.88 / 1917 | 1169 / 634 | 18.79 / 30.71 |
| 0.18.0 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |

Notes on rows:

- **[a]** Measured under load (8.7 and 4.5) and before the speed fixes of 0.15.0: the "worse" numbers of the 0.15.0-dev
  row (first frame, export fps, longest playback gap, memory after giving back) were real and were fixed in the next row;
  the 0.14.1 start time was inflated by the load. These two rows measure the first frame on a 200 ms grid; from the
  "0.15.0 + speed fixes" row on it is measured to 2 ms, so first-frame numbers before that row are not comparable.
- **[b]** Load 13 to 16 before the run. The 0.16-dev (3f197ea) row was cut short under heavy load (a Windows VM and
  other builds were running); its time numbers are 20 to 90 % worse because of that, only its RAM and compose numbers
  (stable under load) are usable. The other [b] rows were also started under load; a repeat in quiet conditions of the
  second one gave 2.1 s / 37.0 s (render) and 1659 MB (large project); 0.16.1 shows no sign of the load in its times.
- **[c]** Partial run (startup and render only), in quiet conditions.
- The longest playback gap of 40.2 ms in the "0.15.0 + speed fixes" row was one gap at a cut (frame 450); it was 19.9 ms
  in the 0.15.0 release run.
- Peak RAM of the large project jumps between about 1550 MB and about 1900 MB from run to run on both 0.15.1 and 0.16.0
  (five repeats each: 1540 / 1939 / 1567 / 1558 / 1907 MB on 0.15.1 and 1547 / 1580 / 1558 / 1913 / 1570 MB on 0.16.0). It is
  noise, not a difference between versions (0.16.0: 1880 MB in the release run above, 0.17.0: 1917 MB).
- Start time of 0.16.0 was about 50 ms slower than 0.15.1 (every run: 442 to 449 ms against 390 to 394 ms); the cause was
  the layout of the inspector panel while it was built. The fix (5 runs, two rounds) brought it back to 384 to 394 ms.
  0.16.1 and 0.17.0 start in 387 to 391 ms in the separate start-only repeats.
- Reading the large project numbers: 0.14.1 needed 58.6 s and 4348 MB, from 0.15.0 on about 37 to 38 s and about
  1550 MB. With 2 threads the large project took about 57 to 59 s
  in every run measured with them.
- Export is 1.03 to 1.07 times the time of plain FFmpeg on the same file in the quiet runs.
- The 0.18.0 row is being measured and will be filled in.

## Raspberry Pi Zero 2 W

The Pi is not a target for comfortable editing; the tests show that schneidi starts, plays and renders on a computer with
415 MB of RAM. All services on the Pi were stopped during the runs. Pi timings fluctuate from hour to hour (the swap state
of the SD card changes them): the same task was 129 s at noon and 152 to 189 s in the evening.

### Same task in two versions

| Test | 0.11.0 (2026-10-25) | 0.18 (2026-11-06) |
|---|---|---|
| Window visible, empty project | 6.5 s, 180 MiB | 4.9 s, 104 MiB |
| Window visible, project with 720p clip | 6.6 s, 262 MiB | 3.6 s, 199 MiB |
| Playback 720p30 original (frames/s of 30) | 29.1 | TBD |
| Playback 1080p60 from proxies (frames/s of 60) | 59.4 | TBD |
| Export 5 s of 720p30, x264 | 82 s, 1.8 frames/s, 303 MiB | TBD |

Notes: the 0.11.0 numbers are from the 15 s CS2 clip (1080p60) and its 720p/1080p30 transcodes; the playback rows are after
the preview conversion fix of 2026-10-25 (before: 12.2 and 26.9 frames/s). The 0.18 start rows were taken with a slightly
different project (a 720p project instead of the demo clip), so treat them as indicative until the A/B run with the same
file is in. 0.11.0 still used the MLT media library, which was removed in 0.15; its start was 3.1 s of library
initialization. Other 0.11.0 numbers (same preview fix): playback 1080p60 original 1.1 frames/s, 1080p30 original 4.0,
720p30 with a grade 28.2; jump in the timeline 2.1 s median from the original, 68 ms from the proxy.

### New on the Pi in 0.18 (development builds of 2026-11-06)

Synthetic material (test pattern, 10 s, 720p30 and 1080p30) unless noted. These are single runs.

| Task | Time | Peak RAM |
|---|---|---|
| `--version`, `probe`, `new`, `info`, `templates` | 0.6 to 1.2 s each | 51 to 66 MiB |
| `check`, 10 s 720p | 3.9 s | 68 MiB |
| `highlights`, 10 s 720p | 1.0 s | 65 MiB |
| Render 10 s 720p, Smart Render (stream copy) | 22 s | 74 MiB |
| Render 10 s 720p, re-encoded (x264 medium) | 46.6 s (6.4 frames/s) | 264 MiB |
| Fill template `short-hook` and render 12 s 1080x1920, 4 threads (before the low-memory rule) | 797 s (0.45 frames/s), paging heavily | 296 MiB |
| same, automatic threads with the low-memory rule (2 threads, shorter encoder look-ahead) | 129 s | - |
| same, `--threads 2` in the older build / without the rule: 4 threads, look-ahead 40 | 383 s / 925 s | - |
| Window shown with a 720p project (offscreen) | 3.6 s | 199 MiB |

Real CS2 clip (30 s, 1080p60, 5 audio tracks, 39 Mbit/s):

| Task | Time | Peak RAM |
|---|---|---|
| `probe` | 3.6 s | 67 MiB |
| `highlights` (2 candidates, 2 + 1 kills found) | 100 s | 121 MiB |
| `check` | 13 s | - |
| Render 15 s of it as 1080p | 392 s (3 threads, low memory) | 251 MiB |
| same, `--quality 1` | 538 s (2 threads) | - |
| Window shown with the project (offscreen) | 5.0 s | 204 MiB |
| `short --game cs2 --reframe center`, 10.8 s, 1080x1920 at 60 fps | 1494 s to 1620 s (render alone 987 s at noon, 1347 s in the evening) | about 330 MB (191 MB in RAM, 144 MB swap) |

The CS2 short is CPU-bound on the Pi (decoding 39 Mbit/s 1080p60 and scaling up 3.2 times, about 3 s of CPU per frame for
646 frames) and still pages memory out; the project's goal for the Pi is "runs stably", not speed on long shorts. The
low-memory rule (limits threads by available RAM) was built and measured in this series on development branches.

## 4K and 8K on the PC (Ryzen 5 5600X, GTX 1070, 16 GB), 2026-11-06

Synthetic material (test pattern with noise, 10 s each) plus a CS2 excerpt scaled up; every run inside a 5 GB memory
limit; the preview ran offscreen with the GPU picture engine (the real on-screen viewer was not measured). Reading a file
took under 1 ms everywhere (`probe` as a command: 0.1 to 0.3 s). The hardware decoder (NVDEC) does at most 4096 pixels
wide, so 8K is decoded by software. "2 clips" means two clips with a dissolve, a grade and a title. Run on a development
branch of 0.18 (not part of a release at the time of the measurement).

| Material | Playback without proxy (1 clip / 2 clips) | Proxy creation for 10 s | Playback with proxy | Render |
|---|---|---|---|---|
| 1080p60 H.264 | 60 / 55 fps | 2 s | 60 / 56 fps | 16.5 s, 1.0 GB (16.8 s before) |
| 4K60 H.264 | 60 / 50 fps (gap 0.5 s) | 6 s | 55 to 57 fps | to 4K: 82 s (7 frames/s), 3.6 GB; to 1080p: 18 s |
| 4K30 H.264 320 Mbit/s, HEVC 10-bit, AV1, ProRes | 28 to 30 / 17 to 30 fps (H.264, 2 clips: gaps of 1.9 s, NVDEC at 100 %) | 4 to 9 s | 29 to 55 fps | - |
| 8K30 H.264 / AV1 / ProRes | 27 to 29 fps; 2 clips: AV1 17 fps (gap 2.7 s), ProRes with dissolve above 5 GB (stopped) | 7 to 9 s | 29 to 30 fps | 8K H.264 to 1080p: above 5 GB (stopped) with 10 threads, 3.5 GB with 6 |
| 8K30 HEVC | 26 fps (gap 0.6 s) / 17 fps (gap 3.2 s) | 9 s | 29 fps | - |
| 8K60 HEVC | 1.8 to 6 fps, gaps of 4 to 5 s (software decoder: 0.8 to 1.4 fps) | 19 to 23 s | 60 fps | to 8K: above 5 GB, stopped at 40 %; to 1080p: 30 s (20 frames/s), 3.1 GB; H.265 NVENC at 8K: 12.5 frames/s |

- 8K60 HEVC: jump in the timeline 1.8 s median (max 4.2 s), scrubbing at rest 3.5 s (1080p: 0.3 s and 5 ms). Memory:
  peak 3.0 GB, 2.5 GB after idle release.
- 12K and 16K: `probe`, a clip in an 8K timeline, playback (2 to 3 fps), render to 1080p and a 16K PNG work; a timeline wider
  than 8192 is refused with a clear error.
- Result: 8K is editable with proxies (smooth everywhere; creating them takes 0.7 to 2.3 times real time); without proxies
  8K30 mostly works, 8K60 does not.
- Fixes that came out of this test: the thread model now reads the cgroup memory limit (8K inside 5 GB: 4 threads instead
  of 9) and the encoder look-ahead is shortened when 40 frames would exceed a third of the free RAM (8K with
  `--threads 2` was killed after 6 s before, finished with 4.1 GB after).

## Earlier hardware-limit tests (PC, 0.10 and 0.11, 2026-10-24 and 2026-10-25)

CPU and RAM limited with Linux cgroups and `taskset` on the Ryzen 5 5600X. Single measurements, not suite runs. The
2400G equivalents of these tables are in the [README](README.md#how-light-is-it).

| Test (0.11) | Result |
|---|---|
| Least RAM, empty project / with 1080p clip | 128 MB works, 112 MB does not / 288 MB works, 272 MB does not |
| Window visible, empty / with clip: one full core | 0.6 s / 0.5 s |
| Window visible, 10 % of one core | 6.0 s / 6.3 s |
| Window visible, 5 % of one core | 11.4 s / 15.3 s |

Playback of 12 gaming clips at 1080p60 (140 Mbit/s) with a grade on every clip, 15 s, frames/s out of 60:

| Cores | Before 0.11 (originals) | 0.11 (originals) | 0.11 (graphics-card decoding) | 0.11 (proxies) |
|---|---|---|---|---|
| 1 | 3.6 | 8.7 to 14.1 | 53.3 to 55.7 | 55.3 to 56.9 |
| 2 | 8.9 | 42.5 to 46.6 | 58.9 to 59.5 | 57.2 |
| 4 | 38 | 56.9 to 57.5 | - | - |
| 6 | 48.5 | 56.9 to 58.5 | - | - |
| 12 threads | 44.8 | 58.1 to 58.5 | - | - |

(The "before 0.11" column is from 2026-10-24; the last row means all 12 threads of the 5600X.)

Decoding and export, 0.11 against the previous media-library engine (same PC, 1080p60 at 138 Mbit/s). Decoding alone:
previous engine 274 frames/s, own decoder 333 in software and 253 on the graphics card (the latter with 1.5 instead of 25 ms
CPU per frame). NVENC export of 10 s: 190 frames/s using 6.1 cores before, 209 frames/s using 0.9 cores now.
