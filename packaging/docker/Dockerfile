# schneidi-cli as a server for AIs and automation: MCP over HTTP (`schneidi-cli serve`), see README.md here.
# Build from the repository root:  docker build -f packaging/docker/Dockerfile -t schneidi .

# ---------- Build: only the command line program (no window needed on a server) ----------
FROM debian:trixie AS build
# Same packages as CI (.github/workflows/ci.yml)
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        qt6-base-dev libmlt++-dev libmlt-dev \
        libavformat-dev libavcodec-dev libswresample-dev libswscale-dev libavutil-dev librubberband-dev libminiaudio-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target schneidi-cli \
 && strip build/schneidi-cli \
 && sh packaging/docker/runtime-packages.sh build/schneidi-cli > build/runtime-packages

# ---------- Runtime ----------
FROM debian:trixie-slim
COPY --from=build /src/build/runtime-packages /tmp/runtime-packages
# The libraries schneidi-cli links (found in the build stage), MLT's modules and data, ffmpeg (test clips, whisper
# input), frei0r effects, a font for titles, tini (signals: `docker stop` ends the server cleanly)
RUN apt-get update \
 && apt-get install -y --no-install-recommends $(cat /tmp/runtime-packages) \
        libmlt7 libmlt-data ffmpeg frei0r-plugins fonts-dejavu-core fontconfig ca-certificates tini \
 && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages
COPY --from=build /src/build/schneidi-cli /usr/local/bin/schneidi-cli

# Not root. Work folder /data; backups, the generated token and extensions (whisper) in the home folder
RUN useradd --create-home --uid 1000 --shell /usr/sbin/nologin schneidi \
 && mkdir -p /data /home/schneidi/.local/share/schneidi \
 && chown -R schneidi:schneidi /data /home/schneidi
USER schneidi
WORKDIR /data
VOLUME /data
ENV QT_QPA_PLATFORM=offscreen
EXPOSE 8642

# Any schneidi-cli command works too: docker run --rm -v "$PWD:/data" schneidi probe clip.mp4
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/schneidi-cli"]
# On all addresses inside the container, so a token is required: SCHNEIDI_TOKEN, or one is made and printed once
CMD ["serve", "--listen", "0.0.0.0", "--root", "/data"]
