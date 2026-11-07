# Compositing: node graphs on a clip

A clip can carry a compositing graph (effect `comp`): nodes that each make a picture or a mask, linked into one
picture that replaces the clip's (after its other effects, before crop/transform). Use it when a plain effect or a
track above is not enough: a key over a background file, a blur only inside a moving mask, text behind a person, a
picture-in-picture with a frame, a glitch only on a title, a logo stuck onto a moving surface. In the app the same
graph shows on the **Comp page** (layer view and node view); every op here is one undo step there.

## Recipes first
Most wishes have a ready graph - use it before building one: `{"op":"comp","clips":[4],"recipe":"greenscreen",
"with":{"background":"/videos/beach.jpg"}}`. `effects` with `kind:'recipes'` lists them with their slots (type,
default, description). Built in:
- `greenscreen` - key the screen, background file behind (slots background, color, tolerance, softness, spill, grow,
  soften). Without a background the tracks below show through.
- `text_behind` - a title between background and the keyed subject (background, text, color, font, size, x, y,
  key_color, tolerance). Footage without a screen: a `cutout` node in the key's place (below).
- `blur_face` - a blurred ellipse that follows a face (x, y = the face at the clip's start, size, radius, soft,
  track face|point); then `{"op":"track_node","clip":4,"node":2}`.
- `pip_frame` - the clip small with a frame and drop shadow (x, y, size, frame_color, shadow).
- `glitch_title` - a title with a mild glitch (text, color, font, size, x, y, strength, seed).
- `sticker_track` - a logo/picture stuck onto a surface with a two-point tracker (sticker = file, required; x, y,
  size, opacity); then `track_node` on node 2.
`with` fills slots (files = media pool items, imported on the way); `"3.size"` sets any node value (`<node id>.<key>`);
missing slots keep the recipe's values, an unknown slot is an error. The answer's notes give the graph and the next
step; refine it node by node afterwards. `{"op":"comp","clips":[4],"save_recipe":"My look"}` keeps a graph as own
recipe (files copied along; media and text nodes become slots; `description`, `replace`); `effects` kind recipes
with `export`/`to` and `import` shares it as ZIP, `remove` deletes an own one. A recipe from someone else that
carries own code (filter chain, shader) falls under the trust rule below.

## Coordinates and units
- Positions and sizes are **0..1 of the clip's own picture** (x right, y down; a size is relative to the picture
  width). A 9:16 clip in a 16:9 timeline: 0..1 of the 9:16 picture, without the bars around it.
- Pixel amounts (blur radius, soft edges, grow, shadow offset, text size, 3D depth) are **project pixels**.
- Colors `"#rrggbb"` or `"#aarrggbb"`; choices by id (`"mode":"screen"`); a missing value = the default.
- Take a screen color from a `frames` image of the clip, not a guess.

## Which node for what
`effects` with `kind:'nodes'` lists every type with its inputs and parameters (ranges, defaults, choices) - the
authority when this list and the build differ. Inputs in brackets.
- **Sources:** `clip` (the clip's own picture; in a compound clip `track` = one inner track), `media` (a file:
  background, logo, texture; `path`, `offset`/`speed` for a video), `color`, `gradient` (linear|radial, `x0 y0 x1
  y1`, up to four colors `c0..c3` at `s0..s3`), `text` (a title with every title look: `fill` solid|gradient|texture
  with `c0..c3`/`a0..a3`/`angle` or `texture`, a 2.5D `edge` with `edge_depth/edge_angle/edge_color/edge_light`,
  `shadow` with `shadow_x/_y/_soft/_color/_opacity`, `glow` with `glow_radius/_color/_strength`, `outline`, `box`),
  `text3d` (real 3D letters: `depth`, `bevel`, `rot_x/rot_y/rot_z`, `fov`, `distance`, light `light_angle/
  light_height`, `ambient`, `color`/`side_color`, `shine`, `metal`; keyframe `comp.n<id>.rot_y` for a turning title).
  A still 3D title needs no graph: op `title` with `text3d:{…}`.
- **Combine:** `merge` [bg, fg, mask] (modes normal, add, screen, multiply, overlay, darken, lighten, difference;
  `opacity`). A chain of merges = layers from bottom to top (the Comp page's layer view).
- **Masks:** `mask` [mask] (shape rect, ellipse, polygon, bezier, linear, radial; `x y w h rotate`, `soft`, `invert`;
  polygon/bezier corners as `points` or `p0x..p3y`; `combine` add|subtract|intersect with the mask input; `follow` =
  a tracker node id), `matte` [src] (a mask from luma or alpha of a picture: text as a window, a track matte).
- **Keying:** `key` [src] (chroma: `color`, `tolerance`, `softness`, `spill`; luma: `low`/`high`, `soft`; `invert`),
  then `matte_tune` [src] (`grow` to shrink/grow, `soft`, clip `black`/`white`, `despill` toward `color`) for a clean
  edge.
- **Cut out a person (no green screen):** `cutout` [src] (AI, extension `cutout`) - see below.
- **Change a picture:** `transform` [src] (x, y, size, angle, anchor `ax ay`; `follow` a tracker), `crop` [src]
  (left, top, right, bottom, `soft`), `blur` [src, mask] (only inside the mask when one is linked), `grade` [src,
  mask] (the Color page's lift/gamma/gain/offset, contrast, saturation, temperature, `lut`), `fx` [src, mask] (any
  picture effect: `effect` = its id, its values as `e_<key>`; also own code: `effect:'ffmpeg_filter'` with `e_chain`,
  `effect:'shader'` with `e_code`, `e_p1`…), `shadow` [src] (x, y, soft, color, opacity), `glitch` [src] (mild by
  default: `strength`, `rgb`, `blocks`, `lines`, `noise`, `seed`).
- **Follow something:** `tracker` [src] (passes its picture through; mode point, two or face; start points `ax, ay`
  and for two points `bx, by`; `size` = the search square, 0..1 of the width) - see below.

## How to build
Small steps, look after each:
1. `{"op":"node","clip":4,"add":{"type":"key","params":{"color":"#2fb84a"}}}` - a clip without a graph gets one
   (`clip` node first); the new node goes behind the output (`after` = another node, `input` = which of its inputs
   the chain feeds, `input:false` = add it unlinked). The answer's notes give the new id and the graph.
2. `frames` with `clip`, `node` and `view:"matte"` shows that node's alpha as gray: white = solid, black =
   transparent. Gray specks in the subject or a gray background = adjust the key (`{"op":"node","clip":4,"id":2,
   "params":{"tolerance":0.4}}`) or add `matte_tune`. Without `view` it shows the node's picture.
3. A background: `{"op":"node","clip":4,"add":{"type":"media","params":{"path":"/videos/beach.jpg"}},"input":false}`,
   a `merge` behind the key (`add` with type merge: the key's picture goes into its `bg`), then relink:
   `{"op":"node","clip":4,"link":[3,4,"bg"]}` and `{"op":"node","clip":4,"link":[2,4,"fg"]}`.
Or the whole graph at once with `{"op":"comp","clips":[4],"graph":{nodes, links, out}}` (example in
`help edit op:comp`). More node ops: `{"op":"node","clip":4,"id":3,"remove":true}` (what read it reads its first
input), `unlink:[from, to, input]`, `{"op":"node","clip":4,"id":6,"out":true}` (the result).
`{"op":"comp","clips":[4],"remove":true}` gives the plain clip back.

Several clips stacked on tracks: `{"op":"comp_from","clips":[3,5,8]}` makes one comp clip whose graph lays the tracks
on each other exactly as before (a `clip` node per track, merges); rebuild the chain from there.

## Animate
Every number of a node takes keyframes: `{"op":"keyframe","clips":[4],"param":"comp.n3.opacity","keys":[{"at":"2s",
"value":0},{"at":"3s","value":1}]}` (n3 = node 3).

## Follow: trackers
1. A `tracker` node on the picture to follow, start points on the thing at frame `from` (point: one spot with
   contrast; two: two spots, gives scale and angle too - for a sticker on a surface; face: `ax, ay` near the face,
   needs the `faces` extension).
2. `{"op":"track_node","clip":4,"node":5}` (from/to = timeline times inside the clip, to before from = backwards)
   writes keyframes of the node's `x, y, scale, angle` and its reference point `rx, ry` (where the thing was at the
   start).
3. **Follow rule:** followers (`mask`, `transform` with `follow` = the tracker's id) keep their own position and move
   by `(x - rx, y - ry)`, scaled and turned by `scale`/`angle` around `(x, y)`. So place the mask or the sticker where
   it belongs **at the start frame**; it moves along from there. Before an analysis nothing moves.
The answer says where it lost the point (`lost_at`, `found_at`, `gave_up`; two points: `outliers`): run it again
from there (`from`) or move the start points. A blurred face: tracker (face), `track_node`, a `mask` ellipse with
`follow` into the `mask` input of a `blur` - or the recipe `blur_face`.

## Cut out a person without a screen
A `cutout` node with `src` = the clip, then `{"op":"cutout_node","clip":4,"node":4}` finds the person once (an AI
segmentation model, extension `cutout`: `extensions` with `install:'cutout'`; without it the op answers
`NEEDS_EXTENSION`). Without that analysis (or outside its from/to range) the node shows nothing (`info` says so).
Values: `threshold` (where the edge lies, default 0.5), `soft`/`grow` (pixels), `invert` (keep the background),
`output` picture|mask. Text behind a person: clip(1) -> merge(3) bg with a `text`(2) as fg, `cutout`(4) with src 1,
merge(5) with bg 3 and fg 4. The notes give frames and ms_per_frame.

## Own effects and trust
When no node or effect does it, write it - on a clip (`{op:'effect', effect:'ffmpeg_filter', params:{chain:…}}`,
`effect:'shader'` with `params.code`) or as an `fx` node in a graph (`e_chain`, `e_code`, `e_p1`…):
- **FFmpeg filter chain:** one linear chain of allowed filters (`effects` `effect:'ffmpeg_filter'` lists them),
  `t` = clip seconds, the size stays. Other filters, looping expressions and too slow chains are refused with the
  reason.
- **Shader:** GLSL that defines `vec4 effect(vec2 uv)` (uv 0..1, y down); given `pixel(uv)`, `pixelAt(ivec2)`,
  `resolution`, `time`, `frame`, `fps`, `p1..p8` (keyframable), `color1/2`. Only bounded `for` loops, no own
  uniforms, no `#include`. Runs on the graphics card only (without one it is skipped - `check`/`render` say so);
  compile errors come back with line numbers: fix and send it again.
- **Trust:** code written in this session runs. Code that came with a project or recipe from someone else does not
  run until the user trusts it (the app asks; `check` reports `custom_code_untrusted`). Pass
  `trust_custom_code:true` (render, edit, app open) only when the user said they trust that file - never on your
  own.
If neither can do it (several inputs at once, a whole clip, a tool schneidi does not have), **compute it outside**
(`ffmpeg`, Python) into a new file and place that file as a clip or as a `media` node.

## Check
`info` shows each graph short (nodes, links, out, `unused` nodes, `cost_ms` = rough time per frame on one core;
merge, mask, key and blur also run on the graphics card when there is one). `check` reports `comp_unused_input`
(nodes not reaching the output: a forgotten link), `comp_slow` (estimated above 40 ms per frame: playback may
stutter - a smaller blur radius or fewer nodes, or tell the user to switch on the render cache for that clip in the
app; export is only slower), `key_edges` (a key with many half-transparent pixels inside the subject) and untrusted
custom code. Look at the result with `frames` (the key with `view:"matte"`) before rendering.
