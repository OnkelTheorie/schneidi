# schneidi server (Docker)

`schneidi-cli serve` makes schneidi an MCP server over HTTP. AIs on other devices (Claude Code on a laptop, scripts,
automation) can then edit videos on a server: look at the material, cut, add titles and subtitles, render, and
download the result. The image contains only the command line program. It has no window and needs no display or
graphics card.

## Start

```sh
docker compose -f packaging/docker/compose.yml up -d
docker compose -f packaging/docker/compose.yml logs schneidi
```

The log shows the address and how to connect. The server needs a token. Set `SCHNEIDI_TOKEN` (at least 16 characters,
e.g. `openssl rand -base64 32`) in the environment or in a `.env` file next to `compose.yml`. Without it, the server
generates a token on first start, keeps it in the `schneidi-home` volume and prints it in the log.

Without compose:

```sh
docker run -d --name schneidi -p 8642:8642 -e SCHNEIDI_TOKEN=… -v schneidi-data:/data ghcr.io/onkeltheorie/schneidi
```

Build it yourself from the repository root: `docker build -f packaging/docker/Dockerfile -t schneidi .`

## Connect an AI

Claude Code:

```sh
claude mcp add --transport http schneidi http://SERVER:8642/mcp --header "Authorization: Bearer $SCHNEIDI_TOKEN"
```

Other MCP clients use the same address (transport "Streamable HTTP") with the header
`Authorization: Bearer <token>`.

## Work folder and files

Everything happens in `/data` (the work folder). Paths in tool calls are relative to it, and nothing outside it can be
read or written. Ways to get material in and results out:

- `PUT /files/<path>` uploads a file and `GET /files/<path>` downloads one (also with byte ranges). Both need the token:
  ```sh
  curl -H "Authorization: Bearer $SCHNEIDI_TOKEN" -T clip.mp4 http://SERVER:8642/files/clips/clip.mp4
  curl -H "Authorization: Bearer $SCHNEIDI_TOKEN" -O http://SERVER:8642/files/out/film.mp4
  ```
- The `upload` tool lets the AI fetch a file from an http(s) URL into the work folder.
- The `files` tool lists the material with type, size and a download link.
- Answers of calls that write files (`render`, `frames` …) carry a `url` to download the result.
- A folder of the host as work folder: `-v /srv/videos:/data`. It must be writable for user 1000 inside the container
  (`sudo chown -R 1000:1000 /srv/videos`).

If projects or AIs use paths of another computer (the same share mounted there), `--map 'Z:\Clips=/data/clips'`
rewrites them, and the answers show them as that computer knows them.

## Long calls

A call that takes longer than about 50 seconds (`--job-wait`) answers with a job id and keeps running on the server,
e.g. a render of a long timeline. The `jobs` tool shows its progress and gives the result once it is done. Up to
`--workers` calls run at the same time. Changes to the same project run one after another.

## Speech recognition

Install whisper once into the home volume:

```sh
docker compose -f packaging/docker/compose.yml exec schneidi schneidi-cli extensions --install whisper,small
```

The `extensions` tool itself is off on a server, because AIs must not install programs there. `--allow-extensions`
turns it on.

## HTTPS and security

- The token protects everything (`/mcp` and `/files`). Anyone with the token can use all files in the work folder.
- For access from outside your home network, put a reverse proxy with TLS in front. `compose.yml` has an example with
  Caddy (automatic Let's Encrypt certificate). Alternatively use a VPN such as WireGuard or Tailscale.
- Web pages from other sites are refused (Origin check). Allow your own with `--allow-origin https://…`.

## Options

`docker compose run --rm schneidi help serve` lists all options (listen address, port, work folder, token file,
workers, job wait, upload limit, public URL, path mapping). Any other schneidi-cli command works in the container as
well, e.g. `docker run --rm -v "$PWD:/data" ghcr.io/onkeltheorie/schneidi probe clip.mp4`.
