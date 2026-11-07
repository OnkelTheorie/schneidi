# Compositing: node graphs on a clip

A clip can carry a compositing graph (effect `comp`): nodes that each make a picture or a mask, linked into one
picture that replaces the clip's (after its other effects, before crop/transform). Use it when a plain effect or a
track above is not enough: a key over a background file, a blur only inside a moving mask, text behind a person, a
picture-in-picture with a frame, a glitch only on a title.

## Which node for what
`effects` with `kind:'nodes'` lists every type with its inputs and parameters (ranges, defaults, choices).
- **Sources:** `clip` (the clip's own picture; in a compound clip `track` = one inner track), `media` (a file:
  background, logo, texture; `path`), `color`, `gradient`, `text` (a title with the title looks).
- **Combine:** `merge` (`bg` + `fg`, optional `mask`; modes normal, add, screen, multiply, overlay, darken, lighten,
  difference; `opacity`).
- **Masks:** `mask` (rect, ellipse, polygon, bezier, linear, radial; positions 0..1 of the picture, `soft`,
  `invert`, `combine` with a mask input, `follow` = a tracker node id), `matte` (a mask from luma or alpha of a
  picture: text as a window, a track matte).
- **Keying:** `key` (chroma: `color` of the screen picked from a `frames` image, `tolerance`, `softness`, `spill`;
  luma: `low`/`high`), then `matte_tune` (shrink/grow, soften, clip black/white, despill) for a clean edge.
- **Change a picture:** `transform` (x, y, size, angle; `follow` a tracker), `crop`, `blur` (with `mask` = only
  there), `grade` (Color page values, LUT), `fx` (any picture effect: `effect` = its id, its values as `e_<key>`;
  also your own: `effect:'ffmpeg_filter'` with `e_chain`, `effect:'shader'` with `e_code`, `e_p1`…), `shadow`,
  `glitch` (mild by default).
- **Follow something:** `tracker` (mode point, two or face; start points `ax, ay` as 0..1 of the clip's own picture,
  without letterbox bars), then the edit op `track_node` computes it once (keyframes x, y, scale, angle and the
  reference point rx, ry); masks and transforms with `follow` = its id move by (x - rx, y - ry) along.

Pixel amounts (blur radius, soft edge, shadow offset) are in project pixels; positions and sizes 0..1 (a size is
relative to the picture width).

## How to build
Small steps, look after each:
1. `{"op":"node","clip":4,"add":{"type":"key","params":{"color":"#2fb84a"}}}` - a clip without a graph gets one
   (`clip` node first); the new node goes behind the output. The answer's notes give the new id and the graph.
2. `frames` with `clip`, `node` and `view:"matte"` shows that node's alpha as gray: white = solid, black =
   transparent. Gray specks in the subject or a gray background = adjust the key (`{"op":"node","clip":4,"id":2,
   "params":{"tolerance":0.4}}`) or add `matte_tune`.
3. A background: `{"op":"node","clip":4,"add":{"type":"media","params":{"path":"/videos/beach.jpg"}},"input":false}`,
   a `merge` behind the key (`add` with type merge: the key's picture goes into its `bg`), then relink:
   `{"op":"node","clip":4,"link":[3,4,"bg"]}` and `{"op":"node","clip":4,"link":[2,4,"fg"]}`.
Or the whole graph at once with `{"op":"comp","clips":[4],"graph":{nodes, links, out}}` (example in
`help edit op:comp`). `{"op":"comp","clips":[4],"remove":true}` gives the plain clip back.

Several clips stacked on tracks: `{"op":"comp_from","clips":[3,5,8]}` makes one comp clip whose graph lays the tracks
on each other exactly as before (a `clip` node per track, merges); rebuild the chain from there.

## Animate and follow
Every number of a node takes keyframes: `{"op":"keyframe","clips":[4],"param":"comp.n3.opacity","keys":[{"at":"2s",
"value":0},{"at":"3s","value":1}]}` (n3 = node 3). A blurred face: `tracker` node (mode face or point on it),
`{"op":"track_node","clip":4,"node":5}`, a `mask` with `follow` 5 into the `mask` input of a `blur`. The answer says
where it lost the point (`lost_at`, `found_at`, `gave_up`): run it again from there or move the start point.

## Check
`info` shows each graph short (nodes, links, out, `unused` nodes, `cost_ms` = rough time per frame on one core).
`check` reports `comp_unused_input` (nodes not reaching the output: a forgotten link), `comp_slow` (estimated above
40 ms per frame: playback may stutter - a smaller blur radius or fewer nodes, or tell the user to switch on the
render cache for that clip in the app; export is only slower) and `key_edges` (a key with many
half-transparent pixels inside the subject). Ready-made recipes (green screen, text behind, blur face …) come later.
