# schneidi on Arch Linux

Two ways to run schneidi on Arch Linux and Arch-based distributions (EndeavourOS, Manjaro, CachyOS, Garuda …).

## 1. AppImage (nothing to build)

Download `schneidi-<version>-x86_64.AppImage` from the
[releases page](https://github.com/OnkelTheorie/schneidi/releases/latest), then:

```sh
chmod +x schneidi-*-x86_64.AppImage
./schneidi-*-x86_64.AppImage            # the program
./schneidi-*-x86_64.AppImage cli help   # the command line program
```

AppImages need FUSE 2 (`sudo pacman -S fuse2`); without it, add `--appimage-extract-and-run`.

## 2. Native package with makepkg

The `PKGBUILD` in this folder builds schneidi from the source tarball of a release tag against Arch's own Qt 6 and
FFmpeg. You need `base-devel` and `git`:

```sh
sudo pacman -S --needed base-devel git
git clone https://github.com/OnkelTheorie/schneidi.git
cd schneidi/packaging/linux/arch
makepkg -si
```

`makepkg` installs the build dependencies (`cmake`, `ninja`, `miniaudio`; for the tests `desktop-file-utils`,
`appstream` and `noto-fonts`), downloads the release tarball and the RNNoise model (22 MB, for the voice cleaning effect), builds,
runs the test suite and `-i` installs the package with `pacman`. Then start schneidi from the application menu, or
run `schneidi` / `schneidi-cli help`.

The test suite (`check()`, about 240 test programs) builds schneidi a second time, as shared libraries, so that every
test program does not carry a full copy of it: it roughly doubles the build time. `makepkg -si --nocheck` skips it
(the AUR helpers have the same switch, e.g. `yay -S --mflags --nocheck schneidi`). Tests that measure real time
(label `timing`) are left out because they fail on a busy machine, and a failing test is retried once.
Two more are left out for now: `test_smart_render` (its H.265 case fails with FFmpeg 9) and `test_inspector_layout`
(pixel limits calibrated on another version of the Noto fonts). `test_tours` (a few minutes of clicking through the
program) runs alone after the others, so that a busy machine does not time it out. Without a font installed, the
text and layout tests fail: `noto-fonts` is a check dependency for that reason.

Uninstall: `sudo pacman -R schneidi`.

Optional extras (pacman lists them after the install):

| Package | For |
|---|---|
| `libpulse` | sound through PulseAudio / PipeWire's Pulse server (ALSA works without it) |
| `qt6-shadertools` | the "Shader" effect (own GLSL effects, compiled by the `qsb` program) |
| `libva-mesa-driver` / `intel-media-driver` | hardware video decoding on AMD / Intel |

**Rebuild after a Qt update.** The GPU viewer uses Qt's private RHI interface, which only stays compatible within
the same Qt version. When `pacman -Syu` brings a new `qt6-base` (6.12 → 6.13), rebuild: `makepkg -sif` in this
folder. Until then schneidi may not start; the AppImage is not affected.

### Building the local checkout instead of a release

To test changes that are not released yet (or the next version), pack the current git `HEAD` as the source tarball
and skip the checksum (it belongs to the released tarball):

```sh
cd schneidi/packaging/linux/arch
./make-local-tarball.sh          # writes schneidi-<pkgver>.tar.gz next to the PKGBUILD
makepkg -si --skipchecksums
```

Only committed files are packed. Remove the tarball afterwards, otherwise a later `makepkg` reuses it instead of
downloading the release.

### A new version

Set `pkgver` to the new version (the tag is `v<pkgver>`), `pkgrel=1`, then update the checksum and `.SRCINFO`:

```sh
updpkgsums
makepkg --printsrcinfo > .SRCINFO
namcap PKGBUILD && makepkg -f && namcap schneidi-*.pkg.tar.zst
```

## Publishing to the AUR (not done)

The package is not in the [AUR](https://aur.archlinux.org) yet. To publish it there, the maintainer would:

1. create an AUR account and add an SSH key (https://aur.archlinux.org/account),
2. `git clone ssh://aur@aur.archlinux.org/schneidi.git` (an empty repository the first time),
3. copy `PKGBUILD` and `.SRCINFO` into it (only those two files: the AUR does not want the helper script or this
   README), commit and push.

From then on, Arch users install it with an AUR helper (`yay -S schneidi`, `paru -S schneidi`) and every new
release needs a `pkgver` bump there. See the [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines).
