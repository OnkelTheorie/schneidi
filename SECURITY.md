# Security

## Reporting a vulnerability

Please report security problems privately through GitHub:
**[Report a vulnerability](https://github.com/OnkelTheorie/schneidi/security/advisories/new)** (Security tab ›
Report a vulnerability). Don't open a public issue for them. You get an answer within a few days; fixes go into the
next release and the release notes name them.

Only the latest release is supported – please check that the problem is still there.

## What to look at

schneidi is a desktop editor, but some parts talk to other programs or the network:

- **`schneidi-cli serve`** (MCP over HTTP, Docker image): listens on 127.0.0.1 by default; on any other address a
  token is required. Clients must not read or write anything outside the work folder (`--root`), and the `upload`
  tool must not fetch from places it should not.
- **Live connection of the app** (an AI edits the open project): a local socket that only the same user can open;
  the app's own AI chat connects with a secret of its own.
- **AI chat in the app:** API keys come from the environment, a company file or the system keychain (Secret Service,
  Windows Credential Manager) and are only sent to the provider's address.
- **Project files and media** from other people: a project file must not make schneidi read or write files the user
  did not choose.
- **Update check:** at most once a day a request to the GitHub API for the latest release (can be switched off).

Problems in the libraries schneidi uses (Qt, MLT, FFmpeg, frei0r, whisper.cpp) are best reported to those projects;
tell us too if schneidi should update or work around them.
