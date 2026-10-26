# Check the render before handing it over

Check the rendered file, not only the project: the file is what people will see. Fix what you find, render
again, check again.

Before rendering, **`check` the project**: missing media, gaps without picture (a forgotten frame between two
clips is black in the video), subtitles too short or too fast to read or past the end, and the sound of the mix
(no sound, clipping, true peak, loudness off the target, silence at the end). Fix the errors; decide on each
warning (a fade to black or a pause can be intended). `target_lufs` for other targets (-23 broadcast, -16 podcast).

After rendering:

1. **`check` the file**: black stretches, frozen picture, no sound, loudness and true peak of what was really
   written. `probe` it: length as planned (ads: exact), resolution and frame rate as intended.
2. **`frames` sheet of the file** (`count` 9-16, `sheet` true), then look at it - `check` measures, only you see:
   - the subject is in frame in every shot (vertical reframes!);
   - titles, logo and subtitles are readable, spelled right, inside the frame and the safe area;
   - nothing unwanted: menus, loading screens, desktop, the end of a recording.
   For a closer look at a cut, `frames` with `at` a few frames before and after it.
3. **Against the request**: every requested format, length, music, logo at the end, subtitles, file names.

In the report, say what you checked and the measured numbers (length, LUFS, true peak), plus anything you could
not check (e.g. how it sounds - you only measure it).
