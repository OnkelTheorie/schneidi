# miniaudio (vendored fallback)

- Version: 0.11.22 (2025-02-24), the same as Debian 13 "trixie" `libminiaudio-dev` 0.11.22+dfsg-1 (the header is byte-identical)
- Source: the release tag `0.11.22` of https://github.com/mackron/miniaudio
  (`https://raw.githubusercontent.com/mackron/miniaudio/0.11.22/miniaudio.h` and `.../LICENSE`; identical to the
  files in `https://github.com/mackron/miniaudio/archive/refs/tags/0.11.22.tar.gz`)
- SHA-256:
  - `miniaudio.h` 9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410
  - `LICENSE` 457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb
  - tag tarball `0.11.22.tar.gz` bcb07bfb27e6fa94d34da73ba2d5642d4940b208ec2a660dbf4e52e6b7cd492f
- License: public domain (Unlicense) or MIT No Attribution, at the choice of the user (`LICENSE`)

Used only when the system has no `miniaudio.h` (openSUSE, Fedora, …), see `SCHNEIDI_SYSTEM_MINIAUDIO` in
`CMakeLists.txt`. The system header is preferred. Unchanged; to update, replace both files from a newer release tag
and update the hashes above.
