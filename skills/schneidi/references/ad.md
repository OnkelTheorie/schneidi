# Ad, product or marketing clip

Goal: an exact length, one clear message, brand at the end - often in several formats, often fully automatic.

## Structure (15 s as an example; scale it)
- 0-2 s **hook**: the most striking shot or a short claim.
- 2-11 s **product/benefit**: 3-5 shots of 1.5-3 s, each with at most one short text (`title`, 2-6 words).
- 11-15 s **end card**: logo + call to action (name, website, "available now"), 2.5-4 s, music resolves.

Plan the segment lengths so they add up exactly (15 s at 25 fps = 375 frames) and check `info` length afterwards.
Ads run on fixed slots; 15.4 s is wrong.

**From a template:** `product-15s` (1:1: three product pictures, name, claim, call to action, logo, music) or
the user's own ad template is built already - fill its slots with `templates` `fill` ([templates.md](templates.md));
the same ad for 20 products: one row per product in a CSV, `templates` `fill` with `rows`, one video each.

## Building blocks
- Images (logo, product photo) are media like video: `append`/`place` them; they last 5 s unless you set `in`/`out`.
- B-roll whose sound is not wanted: `place`/`append` with `video_only:true` (picture only; `audio_only:true` the other way round).
- Logo over a background: logo on V2, `transform` `zoom` to size it (an image is fitted to the frame first), a
  short `fade` in. Transparent PNGs keep their transparency.
- The user's brand kit (`brand`: logo, colors, fonts, intro/outro) when they have one: titles with `color`
  `"brand1"` follow it, `brand` `action` `"logo"` places the logo ([brand.md](brand.md)).
- Text: `title` with a clear style - big `size`, `bold`, `outline` or `box` for contrast. Check placement with
  `frames`. A pop-in: `"enter":"pop"` (also `fade`, `slide_up`; `exit` the same at the end; `{type, duration}`),
  not size keyframes.
- Music from the start, cut to the exact end, `fade` out over the last 0.5-1 s; about -14 LUFS ([audio.md](audio.md)).

## Several formats (16:9 and 9:16 …)
Build one format completely, render and check it. For the next format copy the project file (e.g. `ad-16x9.schneidi`
→ `ad-9x16.schneidi`), change the copy with the `format` op (`width` 1080, `height` 1920), then reframe each clip
(see [social.md](social.md)), move titles and logo into the safe area, look at a `frames` sheet and render. Name the
files by format: `<name>-16x9.mp4`, `<name>-9x16.mp4`, `<name>-1x1.mp4`.

## Fully automatic (nobody watching)
Nobody will answer questions: decide, document the decisions in the final report, render every requested format,
check every render ([check.md](check.md)) and report the paths. Use only the material and texts you were given -
never invent claims, prices or legal text for a product.
