# schneidi features in detail

The [README](README.md) has the short overview and how mature each area is. This page has the details, grouped like
the overview. Everything listed here is also reachable from the command line and MCP unless it is a pure UI matter.

## Editing

- Multiple video and audio tracks: cut, trim, move, snapping, undo/redo
- Timeline zoom and scroll change **only on user input** – nothing jumps when clips get shorter
- Slip and slide tools; ripple and roll trims; a two-up/four-up trim view while dragging an edit
- Compound clips and several timelines per project
- Clip speed / retime and speed ramps; **smooth slow motion** with in-between frames (blended, or computed from the motion with an own optical flow; never across a scene cut) – chosen in the speed dialog (Ctrl+R)
- **Multicam:** a multicam clip from several recordings of one event, synced by sound, markers, In points or start; the viewer shows up to 16 angles at once (more on pages) – click or press 1–9 to cut, change angles later, flatten to normal clips
- Text-based editing: the Transcript panel shows what is said – delete words to cut them, remove pauses
- **Overview strip** (Timeline › Timeline View Options › Overview Timeline): the whole timeline at a glance above the detail view – clips in track colors, markers, sections, the playhead and the visible part as a rectangle; click to jump, drag to scroll, drag an edge to zoom
- Markers, notes and story sections follow ripple edits: what comes after a ripple delete or trim moves with the content, markers in the removed part go with it, closing a gap moves them to the cut
- **Timeline exchange:** import and export FCP7 XML, OpenTimelineIO and EDL – bring a cut over from Premiere Pro or DaVinci Resolve, or hand one over for finishing
- `schneidi <file>` while the app runs hands the file over: a project opens, a video lands in the open project's Media Pool; File › Open Video as Project starts a project in the video's own format
- Contact sheet: stills of the timeline with timecodes as one picture (File › Save Contact Sheet)

## Direct page

- **Edit by recipe** (the Direct page, Shift+1): drop clips from the Media Pool or a file manager, or take the current timeline, and pick a recipe – schneidi analyses the material on your computer and lists suggestions, each with its reason. Click one to play it in the viewer, the checkbox takes it in, drag to reorder, From/To move its edges, Delete takes it out; the strip at the bottom shows the timeline they give. **Accept** builds it as one undo step; then "Open in Edit" or "Export…". Works without any chat AI
- **Short:** the best moments as a 9:16, 1:1 or 4:5 short for YouTube Shorts, Reels or TikTok, up to three variants without shared moments (each its own timeline), the hook first and starting on the action; after Accept one more button adds word captions and mild effect sounds
- **Highlights:** a highlight video of any material – kills from a CS2 or Valorant kill feed, loud moments, laughter, applause, cheering, screams, lots of movement or scene changes; best first or in time order, a target length or all, never the same scene twice; music of your own or the music bed, fitted to the length with the cuts on its beat
- **Talk without pauses** (interviews, podcasts, vlogs, tutorials): every pause where all voices are quiet, and filler words as far as the transcript has them, as suggestions; Accept makes a copy of the timeline without the chosen ones
- **Fill a template:** the best shot of the material for each clip field of a template (long enough, sharp, well exposed, matching the field's label)
- **From a script:** the takes of each sentence of a script; the best one is on, the others ready to swap in
- **Own wish:** with an AI provider set up, a line on the page sends a wish ("only rounds with the AWP") to the AI chat, which answers with new suggestions on the page
- The same for an AI: the `direct` command (suggest, then apply all, some, with moved edges or in another order); analyses run in the background, can be canceled and are cached per file

## Shorts, templates and brand

- **A format per timeline:** a 9:16 short (or 1:1, 4:5) next to the 16:9 edit in one project; copy a timeline into another shape in one step; pasting between timelines of different shapes converts positions
- **Auto reframe:** footage fills a frame of another shape and follows its subject – the view holds still, moves smoothly and jumps only at scene cuts; in game footage the kill feed stays readable as a cropped copy at the top. Or fit the whole picture in with a blurred copy around it, on one track
- **Accents** on hits and drops in one click: zoom punches (flash and shake only when asked for)
- **4:3 game footage:** black bars are found and cropped; a 4:3 game that the recorder stretched to 16:9 is unstretched (per clip, per file or as a watch folder preset) and shown with black bars or blurred sides
- **Blur names:** finds player names in CS2 recordings (team list, kill feed, chat, the name bar) and covers them with blur windows that you check on the Color page – own text detection on your computer, no OCR service. A Valorant template exists but is less tested; small name tags in the 3D picture are often missed
- **Start templates:** begin a project as a game recording, a podcast/interview or a vertical 9:16 video – or save your own setup (tracks, names, muted mix) as the start of every new project
- **Templates:** a finished video from a template in one step – mark clips, titles, the song and the logo of any project as placeholders, then fill them by dragging files from the Media Pool onto them (File › New from Template… shows every template as a tile with preview, format, length and fields). A clip that is too short is trimmed, slowed, frozen or looped as the placeholder says, other shapes are reframed, long text gets smaller, the song is cut to length on its phrases. Five built in (a short with a hook, a gaming highlight on music, a 15 s product clip, a podcast clip, an intro/outro with logo); save your own and pass them on as a ZIP
- **Many videos from a table:** one template, a CSV from Excel or a JSON list – one project and one video per row (`schneidi-cli template fill product-15s --rows products.csv --out_dir out`), e.g. a clip for every product
- **Brand kit** (Workspace › Brand Kit): your logo with its corner and size, up to four colors, two fonts and an intro/outro clip – set once for all projects, changeable per project; titles take "brand color 1" instead of a fixed value and follow when the kit changes; logo and intro/outro go into the timeline only when you ask

## Media and import

- **Media Storage and Media Pool:** look at a file and skim through it before importing, set In/Out and insert; filter the pool by flag, clip color and type; remove clips together with their uses in the timeline in one undo step
- **Watch folders:** new files in a folder (your screen recorder's, for example) come into the Media Pool by themselves – only once they are finished; `schneidi-cli watch` for automatic workflows
- Files with several audio streams (OBS): import presets per stream title – mute the mix, leave out streams, name the tracks, pick a stream's channels
- **HDR phone footage** (HLG, PQ) looks right without a setting: shown and exported as SDR (switchable per clip). Limits: PQ above 1000 nits is pressed into white (mastering metadata is not read yet), Dolby Vision only through its HLG/HDR10 base layer, and HDR clips sit a little darker than SDR clips in a mixed timeline
- Own video decoding, also on the graphics card (NVIDIA, AMD/Intel via VA-API); proxy media and a render cache for smooth playback – schneidi offers proxies when playback cannot keep up

## Compositing and effects

- **Compositing page:** a node graph on any clip, shown as layers (blend mode, mask, opacity per layer) or as nodes with links; nodes for clips, files, colors, gradients, text, merge, transform, crop, blur, grade and any effect; handles in the viewer; "Comp from Selection" turns stacked clips into one graph with the same picture
- **Masks and keying:** rectangle, ellipse, polygon, bezier and gradient masks (soft edge, invert, combined), mattes from luma or alpha; a chroma and luma keyer with spill removal and a matte refiner (grow/shrink, soften, clip black and white)
- **Tracking:** one point, two points (position, scale and rotation – for a sticker on a surface), four corners with a corner pin, or a face; masks and transforms follow the tracker, and a lost point fades out or holds
- **Text behind an object:** click "Pick surface in picture" and then the wall, pillar or car – schneidi outlines it and a title passes behind it
- **Shapes and drawing:** circles, arrows, stars and more right on the video, a shape that blurs or pixelates what is below it, strokes painted in the viewer (P); every shape is a clip
- **AI cutout** (an optional extension): cuts out a person without a green screen, for text behind them or a new background
- **Recipes:** green screen, text behind the subject, a blurred face that follows, picture-in-picture with a frame, a title with a mild glitch, a tracked sticker – one click in the Effects Library; save your own graph as a recipe and share it as a ZIP
- **Write your own effects:** an FFmpeg filter chain or your own GLSL shader as an effect – written by you or an AI; code that comes with someone else's project or recipe runs only after you say you trust it
- Own effects (green screen with spill removal, glow, halftone …), effects with keyframes in any order; import effect presets from Kdenlive and Shotcut
- Video stabilization (own implementation)

## Titles and text

- Titles (also an animated counter and a progress bar) with **title looks** – gradient or texture fill, a 2.5D edge, drop shadow, glow – and **real 3D text** (extruded, beveled, lit, turned in space)
- Fades and transitions (dissolves, dips, wipes, push and whip)
- Subtitles from speech (whisper.cpp, offline) – pick the voice tracks, one subtitle track each; in game recordings with a separate microphone stream it listens to the microphone, so song lyrics stay out of the subtitles; an optional download under Workspace → Extensions
- **Word captions:** karaoke (the spoken word lights up), one word at a time, or one word per beat of the music, from the transcript's word times; export as SRT or WebVTT, or as a subtitle stream inside the MP4/MKV/WebM

## Color

- **Color page:** color wheels with a master wheel below each, contrast, saturation, temperature, tint and exposure, LUTs (.cube, .3dl, .csp, Hald)
- **Curves** (master, R, G, B, hue vs hue, hue vs saturation, luma vs saturation) over a histogram of the picture
- **Qualifier:** picks a color – sky, skin – by hue, saturation and brightness with soft edges, a mask view and its own correction. Can be limited to a shape window; with a qualifier or hue curves active, a fine 65-point user LUT is resampled to 33 points
- **Stills gallery:** keep a still of a grade, compare it with a wipe or side by side in the viewer and apply that grade to other clips; also from the command line (op `still`, `frames still:`)
- **Windows** (rectangle, ellipse, soft edge, invert): grade only part of the picture, or blur, pixelate or cover a region (a face, a name, an overlay); **tracking** makes a window follow a moving object
- A built-in **Edit look** preset (teal shadows, warm highlights, skin kept, a soft glow) in one click; copy and paste grades (`=`/`-`); the timeline filmstrip and the Media Pool show the grade; green screen with an eyedropper on a large picture of the clip
- Video scopes: waveform, RGB parade, vectorscope, histogram

## Sound

- **Own audio engine (SMT):** sound is read, mixed and played sample-exact – lips in sync within about 1 ms in the viewer, no clicks when stopping, jumping or moving faders, real 24-bit export; choose the output device, survives an unplugged headset
- **Studio page:** the audio tracks fill the page with large waveforms; a mixer column on the right with a narrow strip per track (EQ and compressor curves to drag, pan, mute/solo, fader) and the master with its limiter and a large loudness readout (integrated, short-term, momentary, LRA, true peak); a tools drawer below the tracks cleans a voice, ducks the music under speech, brings the whole mix to a target loudness (YouTube −14, podcast −16, broadcast −23 LUFS) in one click, records a voice-over and browses the effect sounds
- **Track automation:** a track's volume and pan as a curve – set points on the line (rubber band) or draw with the pen, smoothed; in **write mode** ride the fader or the pan knob while it plays and the moves are recorded (touch); the curves follow ripple edits and show in the Edit timeline (CLI/MCP: ops `track_volume`, `track_pan`)
- **Multichannel sound:** stereo, 5.1 or 7.1 projects; route single channels of a source to their own track (a wireless mic on channels 3/4 of a camera file), and each clip's waveform shows exactly the stream and channels it plays; a surround panner per track (depth, center, LFE send), meters per speaker, a stereo downmix to listen on headphones, loudness (EBU R128) over all channels; export 5.1/7.1 as AAC, FLAC or WAV, and 5.1 as AC-3 (Dolby Digital) in MP4
- Per-track EQ (6 bands) and compressor, ducking (music goes down while someone speaks; game sound only under its loud moments)
- **Music bed:** background music of its own, computed on your computer (no samples, no license questions) – hype, lo-fi, epic or funk, any length, ending on the last frame; its beat grid is known, so cuts snap to the beat without analysing; it sits under the program sound and ducks under speech (Direct page › Highlights, op `music_bed`)
- **Cut pauses** (op `cut_pauses`): a pause counts only where every chosen track is quiet, each with its own threshold (a noisy mic does not block the rest), optional filler words (äh, um, euh … from the transcript), markers move to the cut
- Loudness and true-peak meters, a master limiter that holds a true-peak ceiling, volume and loudness normalization
- **Clean Voice:** removes background noise (RNNoise) and evens out the level of speech in one step; pauses keep a quiet room tone instead of going silent; audio noise reduction (own implementation)
- **Voice-over recording:** arm a track and record from the microphone while the timeline plays; the take lands exactly where you heard it (input latency compensated)
- **Text to speech** (Piper, offline, an optional download): type a narration and get a voice clip with word captions; English and German voices
- **Effect sounds** (Effects › Sounds, or op `sfx`): CC0 recordings and computed risers for whooshes, hits, impacts and clicks, set as loud as the sound where they land – only where you put them or ask for them, never by themselves; your own sounds from the effects folder too
- **Sound-aware cutting:** J/L cuts and room tone, a song cut to the video's length on its phrases with its real ending, events snapped to the beat, a rough cut from a script with the best take of each line, jump cuts hidden by a zoom, B-roll that matches what is said

## Export

- Export and render queue: H.264, H.265, AV1, VP9 (WebM), ProRes and DNxHR, animated GIF, PNG/TIFF/JPEG image sequences and audio only (AAC, MP3, Opus, WAV, AIFF, FLAC, Apple Lossless)
- Hardware encoders (NVENC, VAAPI, Quick Sync) when available, else x264/x265 (included in the downloads; a GPL-free build uses OpenH264/kvazaar)
- **10-bit export** for H.265 and AV1 (software encoders and NVENC; VAAPI and Quick Sync 10-bit are not yet checked on real hardware)
- **s264** (experimental): schneidi's own H.264 encoder as an extra choice on the Deliver page – in two-pass faster than x264 medium on game, calm and animation footage, slower on camera footage and in CRF mode; a second build for CPUs with AVX2 picked at run time (same streams); a fast speed stage (`render` `speed: "fast"`); never the default, x264 stays the encoder to use
- Presets for YouTube, YouTube Shorts, Instagram Reels and TikTok cap the bit rate; an export quality "Like source"
- **Smart Render** copies unchanged parts of H.264/H.265 recordings instead of encoding them again
- **Check before export:** one click on the Deliver page (or `check` for an AI) finds what often goes wrong in finished videos – a forgotten frame between two clips, missing files, subtitles too short or too fast to read, clipping, loudness off the target, silence at the end, game sound buried under the music, text that TikTok/Reels/Shorts cover, black bars, stutter in recordings, and for an AI also the platform's length limit (`check --for tiktok`); one click fixes peaks and loudness (master limiter and fader)

## AI, command line and automation

- **AI chat in the app:** tell an AI what to do in the open project – Claude, OpenAI-compatible services (OpenAI, OpenRouter, LM Studio, your company's proxy …) or a local model with Ollama; automatic or review mode ([details](README.md#ai-chat-in-the-app))
- **See what an AI does:** clips it adds or changes light up in the timeline, the AI log lists every step (a click jumps there), its analyses and renders show with progress
- **AI director:** a searchable index of your footage (every shot with its quality, what is said and heard; search "where does he laugh"), the edit laid out as a story with a hook and a payoff and checked for pace, the AI's cut plan shown with a reason per step before anything changes, and a review render with cuts, beats and hits burned in for checking its own work
- **Command line and MCP server** (`schneidi-cli`, locally or over HTTP on a server): scripts or an AI assistant can edit almost everything the app can – find pauses, scene changes, loud moments and events in a part of the picture (kill feed, score), pick highlights (your own kills, loud fights), make a finished short of game recordings in one call, build compositing graphs (recipes, keys, masks, trackers, cutouts), fill templates (also one video per row of a table) and apply a brand kit, sync recordings by their sound, build multicam clips, transcribe, cut and trim, grade, animate, mix, duck music and normalize, measure loudness, look at frames (or a detail of them) and scopes, render ([details](README.md#command-line-and-ai-editing-mcp))

## Interface

- Seven pages – Media, Edit, Compositing, Color, Studio, Direct, Deliver – pages you don't use can be hidden, and the start page is a setting (File › Settings › General)
- Background work (analyses, proxies, the render cache) shows small at the bottom right with progress and a ✕ that cancels
- Guided tours on a built-in practice project (first steps, editing, color, compositing, task guides)
- Fully customizable keyboard shortcuts and mouse side buttons, two per function – presets (standard, beginner, Premiere, Final Cut; `.kys` import) and whole sets to save and load, a key trainer; panel sets for the Edit page, panel sizes remembered per page
- Large viewer (one click hides the panels beside it), full-screen viewer and the preview on a second monitor
- Themes, an interface size setting, 16 UI languages (English, German, Spanish, French, Polish, Russian, Ukrainian, Japanese, Portuguese (Brazil), Chinese (Simplified), Italian, Hindi, Dutch, Turkish, Korean, Indonesian – the newer ones translated by AI, improvements by native speakers welcome as issues)
