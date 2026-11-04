# schneidi-cli as a server for AIs and automation: MCP over HTTP (`schneidi-cli serve`), see README.md here.
# Build from the repository root:  docker build -f packaging/docker/Dockerfile -t schneidi .

# ---------- Build: the slim LGPL FFmpeg (as the AppImage) and only the command line program ----------
FROM debian:trixie AS build
# schneidi's build packages (as CI) + what packaging/linux/build-ffmpeg-lgpl.sh needs (only BSD/MIT/LGPL libraries;
# same list as packaging/linux/Dockerfile.ffmpeg-lgpl)
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates curl xz-utils \
        qt6-base-dev qt6-base-private-dev libminiaudio-dev \
        zlib1g-dev libbz2-dev liblzma-dev \
        nasm libopenh264-dev libkvazaar-dev libsvtav1enc-dev libvpx-dev libdav1d-dev \
        libopus-dev libvorbis-dev libmp3lame-dev libwebp-dev libva-dev libdrm-dev libvpl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# FFmpeg first: its layer stays cached while only schneidi's sources change
COPY packaging/linux/build-ffmpeg-lgpl.sh packaging/linux/
COPY packaging/ffmpeg-components.txt packaging/
RUN JOBS=$(nproc) FFMPEG_WORK=/tmp/ffmpeg packaging/linux/build-ffmpeg-lgpl.sh /opt/ffmpeg && rm -rf /tmp/ffmpeg
ENV PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig LD_LIBRARY_PATH=/opt/ffmpeg/lib
COPY . .
# SCHNEIDI_NO_GPL: configure stops unless the FFmpeg it links is LGPL; `schneidi-cli --licenses` says so too
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCHNEIDI_NO_GPL=ON \
 && cmake --build build --target schneidi-cli \
 && strip build/schneidi-cli \
 && sh packaging/docker/runtime-packages.sh build/schneidi-cli /opt/ffmpeg/bin/ffmpeg > build/runtime-packages \
 && ! grep -E '^libav|^libsw' build/runtime-packages

# ---------- Runtime ----------
FROM debian:trixie-slim
COPY --from=build /src/build/runtime-packages /tmp/runtime-packages
# The libraries schneidi-cli and ffmpeg link (found in the build stage, FFmpeg's own ones left out), a font for
# titles, tini (signals: `docker stop` ends the server cleanly)
RUN apt-get update \
 && apt-get install -y --no-install-recommends $(cat /tmp/runtime-packages) \
        fonts-dejavu-core fontconfig ca-certificates tini \
 && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages
# The slim FFmpeg: libraries + the ffmpeg program (proxies, render cache, GIF, test clips, whisper input)
COPY --from=build /opt/ffmpeg/lib/ /opt/ffmpeg/lib/
COPY --from=build /opt/ffmpeg/bin/ffmpeg /usr/local/bin/ffmpeg
RUN rm -rf /opt/ffmpeg/lib/pkgconfig && echo /opt/ffmpeg/lib > /etc/ld.so.conf.d/ffmpeg.conf && ldconfig \
 && ldd /usr/local/bin/ffmpeg | grep -q /opt/ffmpeg/lib/libavcodec
COPY --from=build /src/build/schneidi-cli /usr/local/bin/schneidi-cli
# Licence texts (MIT/BSD ask for their notice in every copy: earshot's recipe ports, RNNoise; the CC0 sounds' files)
COPY --from=build /src/LICENSE /src/THIRD_PARTY.md /usr/share/doc/schneidi/
COPY --from=build /src/third_party/earshot/LICENSE /usr/share/doc/schneidi/earshot-LICENSE.txt
COPY --from=build /src/third_party/rnnoise/COPYING /usr/share/doc/schneidi/rnnoise-COPYING.txt
COPY --from=build /src/assets/sfx/cc0/LICENSE-* /src/assets/sfx/cc0/SOURCES.md /usr/share/doc/schneidi/
RUN ldd /usr/local/bin/schneidi-cli | grep -q /opt/ffmpeg/lib/libavcodec

# Not root. Work folder /data; backups, the generated token and extensions (whisper) in the home folder
RUN useradd --create-home --uid 1000 --shell /usr/sbin/nologin schneidi \
 && mkdir -p /data /home/schneidi/.local/share/schneidi \
 && chown -R schneidi:schneidi /data /home/schneidi
USER schneidi
WORKDIR /data
VOLUME /data
ENV QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
EXPOSE 8642

# Any schneidi-cli command works too: docker run --rm -v "$PWD:/data" schneidi probe clip.mp4
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/schneidi-cli"]
# On all addresses inside the container, so a token is required: SCHNEIDI_TOKEN, or one is made and printed once
CMD ["serve", "--listen", "0.0.0.0", "--root", "/data"]
