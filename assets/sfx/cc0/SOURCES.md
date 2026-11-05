# CC0 sounds (checked 2026-11-04)

| Files | Source | Author | Licence | Download SHA-256 |
|---|---|---|---|---|
| swish/oga-swish-*.wav (8 of 13: 4 5 6 7 9 10 11 12; 3 and 13 left out for realness < 0.6, 1 2 8 for `earshot sfx check` warnings) | https://opengameart.org/content/swishes-sound-pack (swishes.zip) | artisticdude | CC0 1.0 (listed on the page) | 7980215241b739a787dcf26f660ce510bb237900968789395a86911619795693 |
| hit/kenney-impactPunch_*.ogg (5: heavy_000-003, medium_001; heavy_004, medium_002, medium_003 left out: a click in the tail) | https://kenney.nl/assets/impact-sounds (Impact Sounds 1.0) | Kenney (www.kenney.nl) | CC0 1.0 (License.txt in the pack) | 029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8 |
| impact/freesound-ffeller-impact-big.wav | https://github.com/godotengine/godot-demo-projects/blob/b761b4cd5718a7118f2c95eb55f181d0a5f004f1/3d/ragdoll_physics/sounds/impact_big.wav (from https://freesound.org/people/FFeller/sounds/532873/, start silence removed by the Godot demo) | FFeller | CC0 1.0 (impact_big.LICENSE.md next to it, copied as LICENSE-freesound-ffeller-impact-big.md) | 8219eee486ec96b3de1b4eb5dfc7a10de47d4aa10927387b1df7a9de05b17cfc |
| impact/freesound-dorian-mastin-impact-small.wav | same repository and commit, 3d/ragdoll_physics/sounds/impact_small.wav (from https://freesound.org/people/dorian.mastin/sounds/381626/) | dorian.mastin | CC0 1.0 (impact_small.LICENSE.md, copied as LICENSE-freesound-dorian-mastin-impact-small.md) | b38e07a93d65723b7f31848aff5b42c50893e72ca895947e48f3a09f08370596 |
| impact/kenney-racing-impact.ogg | https://github.com/KenneyNL/Starter-Kit-Racing/blob/2f2e5f2646dda89cb21d4e8539bab60c6e955dc8/audio/impact.ogg | Kenney | CC0 1.0 (the kit's README: "sound effects are CC0 licensed") | 964c5fcfb91e7710e2af25c2243c8b7160756b223b0984bacc9eb25bfeea057b |
| click/kenney-citybuilder-placement-{a,b,d}.ogg | https://github.com/KenneyNL/Starter-Kit-City-Builder/tree/4535092b740b378b700efd9df9e27a631815b84a/sounds (placement-a, -b, -d.ogg; -c left out after listening) | Kenney | CC0 1.0 (the kit's README, as above) | a 55e3bad35a443c0a7cc3e0079ae2d2aaae89277495e5b7f8a9c29ca312c0585f, b 5313810e1bb9705634da0a4f07524a7abbe65015cbad8dc630d32947cf3ecc87, d 1d51fb31658bed131ebc3df59e94f8ae6dfc4336a493f60ff052c9bf80de1302 |

CC0 needs no credit; we credit anyway (THIRD_PARTY.md, --licenses).
earshot realness: swishes 0.78–0.98, punches 0.74–0.98.

sounds.json: per file the kind it serves (the longer swishes are the whooshes), its anchor, peak and length, measured once.

Added 2026-11-04 (cloud run, not yet heard by a human): impact and click. Files taken as they are (SHA-256 of the file
itself; GitHub serves them from git, no archive). Checked numerically only (earshot could not be installed there):
per-channel peak below 0 dBFS, no flat-topped runs, DC < 0.004, last 10 ms at or below -29 dB of the peak (the clicks)
or -40 dB (the impacts). Left out: truck_town impact_1-4 (no licence file), rotate/toggle (tail cut off), the racing
skid/engine (no fitting kind). No CC0 riser was reachable (opengameart.org and kenney.nl are blocked there): the riser
stays computed.
