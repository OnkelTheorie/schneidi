#!/usr/bin/env python3
"""Build the built-in content templates (assets/templates/<id>/, compiled into the program, core/TemplateLibrary.h).

  tools/make_builtin_templates.py [--cli build/schneidi-cli] [--only short-hook,...]

Dev tool (Linux, needs ffmpeg with libx264 and the drawtext filter). Everything is synthetic: the placeholder media
are ffmpeg test sources (testsrc2, gradients, mandelbrot, life, cellauto) with a big slot number, drawn shapes for
the product and the logo, and a synthesized 120 BPM beat with 8 s phrases (so fit_music has structure). They are
replaced when a template is filled, so they are small (low resolution and bit rate): all five together must stay
under the size guard of tests/test_builtin_templates.cpp.

Each template is made with schneidi-cli the way a user/AI would make one (`new`, `edit` with place/title/slot ops,
`render` for the preview, `templates save`), in an isolated data/config/cache folder (no brand kit, no own
templates of the user), then copied into assets/templates/<id>/ with the media paths relative to the folder.
Names and descriptions are German keys shown through T() (translations in i18n/*.json, tools/i18n_check.py).
"""
import argparse, json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'assets', 'templates')
FPS = 30
BPM = 120

# ---------- media (ffmpeg) ----------

def ff(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)


def number_overlay(label, size):
    return (f"drawtext=text='{label}':font=Sans:fontsize={size}:fontcolor=white@0.85:x=(w-tw)/2:y=(h-th)/2:"
            "borderw=4:bordercolor=black@0.45")


def video(path, source, w, h, secs, label):
    """A placeholder clip: a moving test pattern with its slot number, silent sound (a filled clip's linked sound
    follows the placeholder's, so the placeholder needs a sound clip)."""
    src = f"{source}:s={w}x{h}:r={FPS}" if '=' in source else f"{source}=s={w}x{h}:r={FPS}"
    ff('-f', 'lavfi', '-t', str(secs), '-i', src, '-f', 'lavfi', '-t', str(secs), '-i', 'anullsrc=r=44100:cl=mono',
       '-vf', f"format=yuv420p,{number_overlay(label, h // 3)}" if label else 'format=yuv420p',
       '-c:v', 'libx264', '-preset', 'veryslow', '-crf', '35', '-g', str(FPS * 2), '-c:a', 'aac', '-b:a', '16k',
       '-shortest', '-movflags', '+faststart', path)


def music(path, secs):
    """120 BPM: kick on every beat, hi-hat off-beats, a four-bar chord loop; every 8 s phrase changes the hats and
    every second one adds sixteenths, the third phrase is louder (a "drop") - structure for `beats`/fit_music."""
    b = 'mod(t,0.5)'
    bar = 'mod(floor(t/2),4)'
    root = f'if(lt({bar},1),110,if(lt({bar},2),87.31,if(lt({bar},3),130.81,98)))'
    ph = 'floor(t/8)'
    expr = (f"0.55*sin(2*PI*(45+90*exp(-25*{b}))*{b})*exp(-9*{b})"
            f"+(0.08+0.06*mod({ph},2))*(random(0)*2-1)*exp(-70*mod(t+0.25,0.5))"
            f"+(0.04*mod({ph},2))*(random(1)*2-1)*exp(-90*mod(t,0.25))"
            f"+(0.10+0.05*gte({ph},2))*(sin(2*PI*{root}*t)+0.5*sin(2*PI*{root}*1.5*t)+0.3*sin(2*PI*{root}*2*t))"
            f"*(0.6+0.4*exp(-3*mod(t,2)))")
    ff('-f', 'lavfi', '-i', f"aevalsrc='{expr}':s=44100:d={secs}",
       '-af', f'afade=t=in:d=0.05,afade=t=out:st={secs - 1}:d=1,alimiter=limit=0.9',
       '-ac', '1', '-c:a', 'aac', '-b:a', '40k', path)


def logo(path):
    """A neutral round badge with an "S" (stands for the user's logo; the brand kit's logo replaces it)."""
    circle = "geq=lum='255':cb=128:cr=128:a='255*clip((120-hypot(X-128\\,Y-128))*2\\,0\\,1)'"
    ff('-f', 'lavfi', '-i', 'color=c=white:s=256x256', '-frames:v', '1',
       '-vf', f"format=yuva444p,{circle},format=rgba,"
              "drawtext=text='S':font=Sans:fontsize=180:fontcolor=#202020:x=(w-tw)/2:y=(h-th)/2-8",
       path)


def product(path, variant):
    """A drawn bottle on a soft background (front, angle, detail)."""
    bg = ['#d9d4cc', '#c9d6dc', '#e2d2c4'][variant]
    w = h = 480
    if variant < 2:
        bx, bw = (190, 100) if variant == 0 else (205, 80)
        boxes = [f"drawbox=x={bx}:y=150:w={bw}:h=260:color=#2f6f73:t=fill",            # body
                 f"drawbox=x={bx + bw // 2 - 22}:y=95:w=44:h=60:color=#2f6f73:t=fill",  # neck
                 f"drawbox=x={bx + bw // 2 - 26}:y=75:w=52:h=26:color=#222222:t=fill",  # cap
                 f"drawbox=x={bx}:y=230:w={bw}:h=80:color=#f2efe8:t=fill",             # label
                 f"drawbox=x={bx + 8}:y=262:w={bw - 16}:h=14:color=#2f6f73@0.6:t=fill",
                 f"drawbox=x={bx + bw - 14}:y=160:w=6:h=240:color=white@0.25:t=fill"]   # shine
    else:  # detail: the label up close
        boxes = ["drawbox=x=40:y=0:w=400:h=480:color=#2f6f73:t=fill",
                 "drawbox=x=40:y=160:w=400:h=170:color=#f2efe8:t=fill",
                 "drawbox=x=90:y=225:w=300:h=36:color=#2f6f73@0.6:t=fill",
                 "drawbox=x=380:y=0:w=14:h=480:color=white@0.25:t=fill"]
    ff('-f', 'lavfi', '-i', f'color=c={bg}:s={w}x{h}', '-frames:v', '1',
       '-vf', ','.join(boxes + ['vignette=PI/5']), '-q:v', '6', path)

def speaker(path, secs):
    """A drawn speaker silhouette (head and shoulders, slowly breathing) on a calm background - no real person."""
    w, h = 270, 480
    shape = ("geq=lum='62':cb=134:cr=124:a='235*max(clip((46-hypot(X-135\\,Y-(236+2*sin(T*1.6))))*2\\,0\\,1)\\,"
             "clip((1-hypot((X-135)/96\\,(Y-470)/150))*120\\,0\\,1))'")
    ff('-f', 'lavfi', '-t', str(secs), '-i', f'gradients=s={w}x{h}:r={FPS}:speed=0.01:c0=#3a4a5a:c1=#6d8794:c2=#c9b08f:nb_colors=3',
       '-f', 'lavfi', '-t', str(secs), '-i', f'color=c=black:s={w}x{h}:r={FPS},format=yuva444p,{shape}',
       '-f', 'lavfi', '-t', str(secs), '-i', 'anullsrc=r=44100:cl=mono',
       '-filter_complex', '[0][1]overlay=format=auto,format=yuv420p[v]', '-map', '[v]', '-map', '2:a',
       '-c:v', 'libx264', '-preset', 'veryslow', '-crf', '35', '-g', str(FPS * 2), '-c:a', 'aac', '-b:a', '16k',
       '-shortest', '-movflags', '+faststart', path)

# ---------- schneidi-cli ----------

class Cli:
    def __init__(self, exe, env):
        self.exe, self.env = exe, env

    def __call__(self, *args):
        r = subprocess.run([self.exe, *args], env=self.env, capture_output=True, text=True)
        try:  # the answer is the last line (progress lines may come before it)
            out = json.loads([l for l in r.stdout.splitlines() if l.strip()][-1])
        except (json.JSONDecodeError, IndexError):
            sys.exit(f'schneidi-cli {" ".join(args)}: no JSON\n{r.stdout}\n{r.stderr}')
        if not out.get('ok', False):
            sys.exit(f'schneidi-cli {" ".join(args[:2])}: {json.dumps(out.get("error"))}')
        return out

    def edit(self, project, ops, timeline=None):
        args = ['edit', project, '--ops', json.dumps(ops)]
        if timeline:
            args += ['--timeline', timeline]
        return self('edit', *args[1:])


def clip_ids(info):
    """{(track, start frame): clip id} and {title text: id} of the timeline `info` shows."""
    tl = info['timeline']
    at, titles = {}, {}
    for tr in tl['tracks']:
        for c in tr.get('clips', []):
            at[(tr['track'], c['start'])] = c['id']
            if c.get('kind') == 'title':
                titles[c.get('text', c.get('title', {}).get('text', ''))] = c['id']
    return at, titles


def s(secs):
    return int(round(secs * FPS))

# ---------- the templates ----------

def vertical_clip(path, source, label, secs=4):
    video(path, source, 270, 480, secs, label)


def wide_clip(path, source, label, secs=4):
    video(path, source, 320, 180, secs, label)

# Title styles: every title references the brand kit (color/outline/box brand1..2, font brand1); the stored values
# (white text, dark outline/box, the default font) are what shows with an empty kit.
HEAD = {'color': 'brand1', 'outline': True, 'outline_color': 'brand2', 'font': 'brand1', 'bold': True}


def short_hook(cli, work, media):
    vertical_clip(media + '/clip1.mp4', 'gradients=speed=0.02:c0=#1d2b53:c1=#7e2553:c2=#008751:nb_colors=3', '1')
    vertical_clip(media + '/clip2.mp4', 'testsrc2', '2')
    vertical_clip(media + '/clip3.mp4', 'mandelbrot', '3')
    music(media + '/music.m4a', 16)
    p = work + '/short-hook.schneidi'
    cli('new', p, '--width', '1080', '--height', '1920', '--fps', str(FPS))
    cli.edit(p, [{'op': 'place', 'media': f'{media}/clip{i + 1}.mp4', 'at': s(4 * i), 'track': 'V1'} for i in range(3)]
             + [{'op': 'place', 'media': media + '/music.m4a', 'at': 0, 'duration': s(12), 'track': 'A2'},
                {'op': 'title', 'text': 'Wait for the end…', 'at': 0, 'duration': s(3), 'track': 'V2',
                 'size': 110, 'y': 560, 'outline_width': 6, **HEAD}])
    at, titles = clip_ids(cli('info', p))
    music_clip = at[('A2', 0)]
    cli.edit(p, [
        {'op': 'volume', 'clips': [music_clip], 'db': -8},
        {'op': 'slot', 'clip': at[('V1', 0)], 'id': 'clip1', 'label': 'Clip 1', 'fit': 'freeze',
         'hint': 'the strongest moment first: it decides whether viewers stay'},
        {'op': 'slot', 'clip': at[('V1', s(4))], 'id': 'clip2', 'label': 'Clip 2', 'fit': 'speed',
         'hint': 'what leads up to the payoff'},
        {'op': 'slot', 'clip': at[('V1', s(8))], 'id': 'clip3', 'label': 'Clip 3', 'fit': 'freeze',
         'hint': 'the payoff or the punchline'},
        {'op': 'slot', 'clip': titles['Wait for the end…'], 'id': 'hook', 'label': 'Hook', 'max_chars': 40,
         'hint': 'a short line that makes viewers stay (top third, phone apps cover the bottom)'},
        {'op': 'slot', 'clip': music_clip, 'id': 'music', 'label': 'Music', 'fit_music': True,
         'hint': 'optional: a song, cut to the length on its phrases'},
    ])
    return p, dict(
        name='Short mit Hook',
        description='Hochformat-Short: Hook-Text über dem ersten Clip, drei Clips, Musik darunter.',
        tags='short,9:16,hook')


def gaming_highlight(cli, work, media):
    sources = ['testsrc2', 'mandelbrot', 'life=mold=10:ratio=0.1:death_color=#202838:life_color=#e0a040',
               'cellauto=rule=110', 'gradients=speed=0.03:c0=#ff5e3a:c1=#3a1c71:nb_colors=2']
    for i, src in enumerate(sources):
        wide_clip(f'{media}/clip{i + 1}.mp4', src, str(i + 1), secs=3)
    music(media + '/music.m4a', 18)
    p = work + '/gaming-highlight.schneidi'
    cli('new', p, '--width', '1920', '--height', '1080', '--fps', str(FPS))
    cli.edit(p, [{'op': 'place', 'media': f'{media}/clip{i + 1}.mp4', 'at': s(3 * i), 'track': 'V1'} for i in range(5)]
             + [{'op': 'place', 'media': media + '/music.m4a', 'at': 0, 'duration': s(15), 'track': 'A2'},
                {'op': 'title', 'text': 'HIGHLIGHTS', 'at': 0, 'duration': s(2.5), 'track': 'V2',
                 'size': 150, 'y': 300, 'outline_width': 8, **HEAD}])
    at, titles = clip_ids(cli('info', p))
    clips = [at[('V1', s(3 * i))] for i in range(5)]
    music_clip = at[('A2', 0)]
    hints = ['the opener: a strong moment', 'build-up', 'build-up', 'the best moment, on the drop', 'the ending']
    cli.edit(p, [{'op': 'effect', 'clips': clips, 'preset': 'edit'},
                 {'op': 'volume', 'clips': [music_clip], 'db': -6}]
             + [{'op': 'slot', 'clip': c, 'id': f'clip{i + 1}', 'label': f'Clip {i + 1}', 'fit': 'freeze',
                 'hint': hints[i]} for i, c in enumerate(clips)]
             + [{'op': 'slot', 'clip': titles['HIGHLIGHTS'], 'id': 'title', 'label': 'Title', 'max_chars': 24,
                 'hint': 'the game, the player or the event'},
                {'op': 'slot', 'clip': music_clip, 'id': 'music', 'label': 'Music', 'fit_music': True,
                 'snap_cuts': True, 'hint': 'the song; cuts can go onto its beat (op snap_to_beat)'}])
    return p, dict(
        name='Gaming-Highlight',
        description='16:9-Highlight auf Musik: Titel, fünf kurze Clips mit Edit-Look, Song auf Länge geschnitten.',
        tags='gaming,16:9,music')


def product_15s(cli, work, media):
    for i in range(3):
        product(f'{media}/product{i + 1}.jpg', i)
    music(media + '/music.m4a', 18)
    logo(media + '/logo.png')
    p = work + '/product-15s.schneidi'
    cli('new', p, '--width', '1080', '--height', '1080', '--fps', str(FPS))
    box = {'box': True, 'box_color': 'brand1', 'color': 'brand2', 'font': 'brand1', 'bold': True, 'box_padding': 24}
    cli.edit(p, [{'op': 'place', 'media': f'{media}/product{i + 1}.jpg', 'at': s(5 * i), 'duration': s(5),
                  'track': 'V1'} for i in range(3)]
             + [{'op': 'place', 'media': media + '/music.m4a', 'at': 0, 'duration': s(15), 'track': 'A2'},
                {'op': 'title', 'text': 'Aurora Bottle', 'at': s(0.5), 'duration': s(4.5), 'track': 'V2',
                 'size': 88, 'y': -360, 'outline_width': 5, **HEAD},
                {'op': 'title', 'text': 'Cold for 24 hours', 'at': s(5), 'duration': s(5), 'track': 'V2',
                 'size': 64, 'y': -380, **box},
                {'op': 'title', 'text': 'Shop now', 'at': s(10), 'duration': s(5), 'track': 'V2',
                 'size': 84, 'y': -360, **box}])
    cli.edit(p, [{'op': 'place', 'media': media + '/logo.png', 'at': 0, 'duration': s(15), 'track': 'V3'}])
    at, titles = clip_ids(cli('info', p))
    logo_clip = at[('V3', 0)]
    music_clip = at[('A2', 0)]
    hints = ['the product from the front', 'in use or from another side', 'a detail, then the call to action']
    cli.edit(p, [{'op': 'transform', 'clips': [logo_clip], 'zoom': 0.14, 'x': 440, 'y': 440},
                 {'op': 'volume', 'clips': [music_clip], 'db': -4}]
             + [{'op': 'slot', 'clip': at[('V1', s(5 * i))], 'id': f'product{i + 1}', 'label': f'Product {i + 1}',
                 'fit': 'freeze', 'hint': hints[i]} for i in range(3)]
             + [{'op': 'slot', 'clip': titles['Aurora Bottle'], 'id': 'name', 'label': 'Product name', 'max_chars': 28},
                {'op': 'slot', 'clip': titles['Cold for 24 hours'], 'id': 'claim', 'label': 'Price or claim',
                 'max_chars': 32, 'hint': 'the price or what makes it special'},
                {'op': 'slot', 'clip': titles['Shop now'], 'id': 'cta', 'label': 'Call to action', 'max_chars': 24},
                {'op': 'slot', 'clip': logo_clip, 'id': 'logo', 'kind': 'logo', 'label': 'Logo',
                 'hint': 'empty: the brand kit\'s logo'},
                {'op': 'slot', 'clip': music_clip, 'id': 'music', 'label': 'Music', 'fit_music': True,
                 'hint': 'optional: a song, cut to 15 s on its phrases'}])
    return p, dict(
        name='Produktclip 15 s',
        description='Quadratischer 15-s-Produktclip: drei Bilder, Name, Preis oder Versprechen, Aufruf, Logo.',
        tags='product,ad,1:1')


def podcast_clip(cli, work, media):
    speaker(media + '/speaker.mp4', 20)
    logo(media + '/logo.png')
    p = work + '/podcast-clip.schneidi'
    cli('new', p, '--width', '1080', '--height', '1920', '--fps', str(FPS))
    cli.edit(p, [{'op': 'place', 'media': media + '/speaker.mp4', 'at': 0, 'track': 'V1'},
                 {'op': 'title', 'text': 'Episode 12: Why simple wins', 'at': 0, 'duration': s(20), 'track': 'V2',
                  'size': 60, 'y': 640, 'box': True, 'box_color': 'brand1', 'color': 'brand2', 'font': 'brand1',
                  'bold': True, 'box_padding': 22},
                 {'op': 'place', 'media': media + '/logo.png', 'at': 0, 'duration': s(20), 'track': 'V3'}])
    # Sample word captions in the brand's colors: they show where and how the words go; the real ones come from
    # the clip's transcript (op captions after filling)
    words = ('These sample captions show where your words go: '
             'after filling, transcribe the clip and the captions follow every spoken word').split()
    cues, t = [], 0.4
    for w in words:
        cues.append({'from': f'{t:.2f}s', 'to': f'{t + 0.42:.2f}s', 'text': w})
        t += 0.45
    r = cli.edit(p, [{'op': 'captions', 'style': 'karaoke', 'words': cues, 'name': 'Captions'}])
    at, titles = clip_ids(cli('info', p))
    st = r.get('timeline', {}).get('subtitles', [{}])
    logo_clip = at[('V3', 0)]
    cli.edit(p, [{'op': 'transform', 'clips': [logo_clip], 'zoom': 0.12, 'x': -400, 'y': 820},
                 {'op': 'track', 'track': 'ST1', 'color': 'brand1', 'outline_color': 'brand2', 'font': 'brand1',
                  'highlight': '#ffd23f'},
                 {'op': 'slot', 'clip': at[('V1', 0)], 'id': 'speaker', 'label': 'Speaker', 'length': 'flexible',
                  'min': '5s', 'max': '90s', 'fit': 'trim',
                  'hint': 'the excerpt of the episode (speaker video); reframed to 9:16'},
                 {'op': 'slot', 'clip': titles['Episode 12: Why simple wins'], 'id': 'topic', 'label': 'Episode or topic',
                  'max_chars': 40},
                 {'op': 'slot', 'clip': logo_clip, 'id': 'logo', 'kind': 'logo', 'label': 'Logo',
                  'hint': 'empty: the brand kit\'s logo'}])
    return p, dict(
        name='Podcast-Ausschnitt',
        description='Hochformat-Ausschnitt aus einem Podcast: Sprecher-Video, Thema oben, Wort-Untertitel, Logo.',
        tags='podcast,9:16,captions')


def intro_outro(cli, work, media):
    wide_clip(media + '/background.mp4', 'gradients=speed=0.015:c0=#1b1f2a:c1=#30394d:c2=#1b1f2a:nb_colors=3', '',
              secs=6)
    logo(media + '/logo.png')
    p = work + '/intro-outro.schneidi'
    cli('new', p, '--width', '1920', '--height', '1080', '--fps', str(FPS))
    name = {'size': 72, 'outline_width': 4, **HEAD}
    # Intro (4 s): background, the logo grows in, the channel name below it
    cli.edit(p, [{'op': 'place', 'media': media + '/background.mp4', 'at': 0, 'duration': s(4), 'track': 'V1'},
                 {'op': 'place', 'media': media + '/logo.png', 'at': 0, 'duration': s(4), 'track': 'V2'},
                 {'op': 'title', 'text': 'Your Channel', 'at': s(0.6), 'duration': s(3.4), 'track': 'V3',
                  'y': -260, **name}])
    at, titles = clip_ids(cli('info', p))
    bg1, logo1, name1 = at[('V1', 0)], at[('V2', 0)], titles['Your Channel']
    cli.edit(p, [{'op': 'transform', 'clips': [logo1], 'zoom': 0.26, 'y': 80},
                 {'op': 'keyframe', 'clips': [logo1], 'param': 'zoom',
                  'keys': [{'at': {'clip': logo1, 'offset': 0}, 'value': 0.2, 'ease': 'ease_out'},
                           {'at': {'clip': logo1, 'offset': '0.8s'}, 'value': 0.26}]},
                 {'op': 'keyframe', 'clips': [logo1], 'param': 'opacity',
                  'keys': [{'at': {'clip': logo1, 'offset': 0}, 'value': 0},
                           {'at': {'clip': logo1, 'offset': '0.5s'}, 'value': 100}]},
                 {'op': 'fade', 'clips': [bg1], 'in': s(0.5)},
                 {'op': 'slot', 'clip': bg1, 'id': 'background', 'label': 'Background', 'fit': 'loop',
                  'hint': 'optional: a calm moving background'},
                 {'op': 'slot', 'clip': logo1, 'id': 'logo', 'kind': 'logo', 'label': 'Logo',
                  'hint': 'empty: the brand kit\'s logo'},
                 {'op': 'slot', 'clip': name1, 'id': 'name', 'label': 'Channel name', 'max_chars': 32}])
    # Outro (6 s) as its own timeline of the same project: render it on its own for the brand kit's outro
    cli.edit(p, [{'op': 'new_timeline', 'name': 'Outro', 'open': False},
                 {'op': 'place', 'media': media + '/background.mp4', 'at': 0, 'duration': s(6), 'track': 'V1'},
                 {'op': 'place', 'media': media + '/logo.png', 'at': 0, 'duration': s(6), 'track': 'V2'},
                 {'op': 'title', 'text': 'Subscribe for more', 'at': s(0.3), 'duration': s(5.7), 'track': 'V3',
                  'y': -240, 'size': 84, 'outline_width': 5, **HEAD},
                 {'op': 'title', 'text': 'Your Channel', 'at': s(0.3), 'duration': s(5.7), 'track': 'V4',
                  'y': -350, 'size': 48, 'color': 'brand1', 'font': 'brand2'}])
    info = cli('info', p, '--timeline', 'Outro')
    at, titles = clip_ids(info)
    bg2, logo2 = at[('V1', 0)], at[('V2', 0)]
    cli.edit(p, [{'op': 'transform', 'clips': [logo2], 'zoom': 0.2, 'y': 150},
                 {'op': 'fade', 'clips': [bg2, logo2], 'out': s(0.8)},
                 {'op': 'slot', 'clip': bg2, 'id': 'background', 'fit': 'loop', 'label': 'Background'},
                 {'op': 'slot', 'clip': logo2, 'id': 'logo', 'kind': 'logo', 'label': 'Logo'},
                 {'op': 'slot', 'clip': titles['Subscribe for more'], 'id': 'cta', 'label': 'Call to action',
                  'max_chars': 32},
                 {'op': 'slot', 'clip': titles['Your Channel'], 'id': 'name', 'label': 'Channel name',
                  'max_chars': 32}], timeline='Outro')
    return p, dict(
        name='Intro und Outro',
        description='16:9-Logo-Intro (4 s) und Outro mit Aufruf (6 s, eigene Timeline) für das Brand-Kit.',
        tags='intro,outro,logo,16:9')


TEMPLATES = {'short-hook': short_hook, 'gaming-highlight': gaming_highlight, 'product-15s': product_15s,
             'podcast-clip': podcast_clip, 'intro-outro': intro_outro}


def preview(cli, project, work, tid, timelines=(None,)):
    """preview.mp4: the template as it is (its own placeholders, captions burned in), 360p, small; several
    timelines one after another."""
    raws = []
    for i, tl in enumerate(timelines):
        raws.append(f'{work}/{tid}-raw{i}.mp4')
        cli('render', project, '--out', raws[-1], '--height', '360', '--quality', '2', '--subtitles', 'burn',
            '--overwrite', 'true', *(['--timeline', tl] if tl else []))
    out = f'{work}/preview.mp4'
    inputs = [x for r in raws for x in ('-i', r)]
    graph = ''.join(f'[{i}:v][{i}:a]' for i in range(len(raws))) + f'concat=n={len(raws)}:v=1:a=1[v][a];[v]fps=15[vo]'
    ff(*inputs, '-filter_complex', graph, '-map', '[vo]', '-map', '[a]',
       '-c:v', 'libx264', '-preset', 'veryslow', '-crf', '33', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '32k', '-ac', '1', '-movflags', '+faststart', out)
    return out


def relative_paths(project_file, folder):
    """Media paths relative to the template folder (no path of the machine that made it)."""
    with open(project_file, encoding='utf-8') as f:
        d = json.load(f)
    for m in d.get('media', []):
        if m.get('relPath'):
            m['path'] = m['relPath']
    text = json.dumps(d, ensure_ascii=False, indent=1) + '\n'
    if folder in text:
        sys.exit(f'{project_file}: still contains {folder}')
    with open(project_file, 'w', encoding='utf-8') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cli', default=os.path.join(ROOT, 'build', 'schneidi-cli'))
    ap.add_argument('--only', default='', help='comma list of template ids')
    ap.add_argument('--keep', action='store_true', help='keep the work folder')
    ap.add_argument('--work', default=None, help='parent folder of the work folder (default: the temp folder)')
    a = ap.parse_args()
    work_root = tempfile.mkdtemp(prefix='schneidi-builtin-', dir=a.work)
    env = dict(os.environ, XDG_DATA_HOME=work_root + '/data', XDG_CONFIG_HOME=work_root + '/config',
               XDG_CACHE_HOME=work_root + '/cache', QT_QPA_PLATFORM='offscreen', LC_ALL='C.UTF-8')
    cli = Cli(a.cli, env)
    only = [x for x in a.only.split(',') if x]
    try:
        for tid, make in TEMPLATES.items():
            if only and tid not in only:
                continue
            work = f'{work_root}/{tid}'
            media = f'{work}/media'
            os.makedirs(media)
            project, meta = make(cli, work, media)
            prev = preview(cli, project, work, tid, ('Timeline 1', 'Outro') if tid == 'intro-outro' else (None,))
            cli('templates', '--save', project, '--name', meta['name'], '--description', meta['description'],
                '--tags', meta['tags'], '--preview', prev)
            own = os.path.join(work_root, 'data', 'schneidi', 'templates')
            folder = os.path.join(own, next(n for n in os.listdir(own) if not n.startswith('.')))
            target = os.path.join(OUT, tid)
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(folder, target)
            shutil.rmtree(folder)
            relative_paths(os.path.join(target, 'template.schneidi'), work_root)
            size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(target) for f in fs)
            print(f'{tid}: {size / 1024:.0f} KB ({meta["name"]})')
    finally:
        if a.keep:
            print('work folder:', work_root)
        else:
            shutil.rmtree(work_root, ignore_errors=True)


if __name__ == '__main__':
    main()
