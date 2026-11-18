# schneidi-cli as a server for AIs and automation: MCP over HTTP (`schneidi-cli serve`), see README.md here.
# Build from the repository root:  docker build -f packaging/docker/Dockerfile -t schneidi .

# ---------- Build: the slim FFmpeg (as the AppImage) and only the command line program ----------
# FFmpeg with x264/x265 (GPL-2.0+, like the AppImage and Windows downloads since 0.21; schneidi's own code stays the
# same). A GPL-free image: docker build --build-arg GPL_ENCODERS=0 … (OpenH264/kvazaar, SCHNEIDI_NO_GPL).
FROM debian:trixie AS build
ARG GPL_ENCODERS=1
# schneidi's build packages (as CI) + what packaging/linux/build-ffmpeg-lgpl.sh needs (BSD/MIT/LGPL libraries and
# x264/x265; same list as packaging/linux/Dockerfile.ffmpeg-lgpl)
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates curl xz-utils \
        qt6-base-dev qt6-base-private-dev libminiaudio-dev \
        zlib1g-dev libbz2-dev liblzma-dev \
        nasm libopenh264-dev libkvazaar-dev libsvtav1enc-dev libvpx-dev libdav1d-dev \
        libopus-dev libvorbis-dev libmp3lame-dev libwebp-dev libva-dev libdrm-dev libvpl-dev \
        libx264-dev libx265-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# FFmpeg first: its layer stays cached while only schneidi's sources change
COPY packaging/linux/build-ffmpeg-lgpl.sh packaging/linux/
COPY packaging/ffmpeg-components.txt packaging/
RUN JOBS=$(nproc) FFMPEG_WORK=/tmp/ffmpeg GPL_ENCODERS=$GPL_ENCODERS packaging/linux/build-ffmpeg-lgpl.sh /opt/ffmpeg \
 && rm -rf /tmp/ffmpeg
ENV PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig LD_LIBRARY_PATH=/opt/ffmpeg/lib
COPY . .
# GPL_ENCODERS=0 -> SCHNEIDI_NO_GPL: configure stops unless the FFmpeg it links is LGPL; `schneidi-cli --licenses`
# says what the image has either way
RUN NO_GPL=$([ "$GPL_ENCODERS" = 0 ] && echo ON || echo OFF) \
 && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCHNEIDI_NO_GPL=$NO_GPL \
 && cmake --build build --target schneidi-cli \
 && strip build/schneidi-cli \
 && sh packaging/docker/runtime-packages.sh build/schneidi-cli /opt/ffmpeg/bin/ffmpeg > build/runtime-packages \
 && ! grep -E '^libav|^libsw' build/runtime-packages \
 && VERSION=$(sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt) \
 && sed "s/@VERSION@/$VERSION/g" packaging/SOURCE-OFFER.txt.in > build/SOURCE-OFFER.txt

# ---------- Runtime ----------
FROM debian:trixie-slim
COPY --from=build /src/build/runtime-packages /tmp/runtime-packages
# The libraries schneidi-cli and ffmpeg link (found in the build stage, FFmpeg's own ones left out; libx264/libx265
# with their copyright files in /usr/share/doc), a font for titles, tini (signals: `docker stop` ends the server
# cleanly)
# (x264-x265-versions.txt: the Debian packages of x264/x265/libnuma in the image, for the source archive of
# packaging/collect-gpl-sources.sh)
RUN apt-get update \
 && apt-get install -y --no-install-recommends $(cat /tmp/runtime-packages) \
        fonts-dejavu-core fontconfig ca-certificates tini \
 && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages \
 && mkdir -p /usr/share/doc/schneidi \
 && { dpkg-query -W -f='${Package} ${Version} ${source:Package} ${source:Version}\n' 'libx264-*' 'libx265-*' libnuma1 \
        2>/dev/null || true; } > /usr/share/doc/schneidi/x264-x265-versions.txt
# The slim FFmpeg: libraries + the ffmpeg program (proxies, render cache, GIF, test clips, whisper input) and its
# licence texts (GPL-2.0 with x264/x265)
COPY --from=build /opt/ffmpeg/lib/ /opt/ffmpeg/lib/
COPY --from=build /opt/ffmpeg/share/licenses/ffmpeg/ /usr/share/doc/schneidi/ffmpeg/
COPY --from=build /opt/ffmpeg/bin/ffmpeg /usr/local/bin/ffmpeg
RUN rm -rf /opt/ffmpeg/lib/pkgconfig && echo /opt/ffmpeg/lib > /etc/ld.so.conf.d/ffmpeg.conf && ldconfig \
 && ldd /usr/local/bin/ffmpeg | grep -q /opt/ffmpeg/lib/libavcodec
COPY --from=build /src/build/schneidi-cli /usr/local/bin/schneidi-cli
# Licence texts (MIT/BSD ask for their notice in every copy: earshot's recipe ports, RNNoise; the CC0 sounds' files)
COPY --from=build /src/LICENSE /src/THIRD_PARTY.md /src/build/SOURCE-OFFER.txt /usr/share/doc/schneidi/
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
