# Check the render before handing it over

Check the rendered file, not only the project: the file is what people will see. Fix what you find, render
again, check again.

1. **`probe` the file**: length as planned (ads: exact), resolution and frame rate as intended, sound present.
2. **`frames` sheet of the file** (`count` 9-16, `sheet` true), then look at it:
   - no black or frozen frames where there should be picture (gaps in the timeline show up as black);
   - the subject is in frame in every shot (vertical reframes!);
   - titles, logo and subtitles are readable, spelled right, inside the frame and the safe area;
   - nothing unwanted: menus, loading screens, desktop, the end of a recording.
   For a closer look at a cut, `frames` with `at` a few frames before and after it.
3. **`loudness` of the file**: near the target (web: -14 LUFS ±1), true peak ≤ -1 dBTP (above = clipping).
4. **`silence` on the file**: no long silence at the start or end unless intended, no unwanted gaps in speech.
5. **Against the request**: every requested format, length, music, logo at the end, subtitles, file names.

In the report, say what you checked and the measured numbers (length, LUFS, true peak), plus anything you could
not check (e.g. how it sounds - you only measure it).
