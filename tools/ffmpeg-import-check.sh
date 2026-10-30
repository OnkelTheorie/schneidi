#!/usr/bin/env bash
# Import breadth of a slim FFmpeg build (packaging/ffmpeg-components.txt) against a full one: writes sample files in
# many formats (camera/phone/screen recorder codecs, containers, audio, pictures) with a full ffmpeg, then decodes
# every file with both builds (ffprobe -count_frames: demuxer + decoder of every stream, like schneidi's readers).
# A file the full build reads and the slim one does not (or differently) is a failure, unless it is a deliberate drop
# (DROPPED below: formats nobody records in).
#   tools/ffmpeg-import-check.sh <samples dir> <full prefix> <slim prefix> [ffmpeg to write samples, default: ffmpeg]
#   tools/ffmpeg-import-check.sh <samples dir> - - [ffmpeg]      only write the samples (e.g. in another container)
# Existing files in <samples dir> are kept (also real ones you put there); only missing samples are written.
set -u
DIR="$(realpath -m "$1")"; FULL="$2"; SLIM="$3"; GEN="${4:-ffmpeg}"
mkdir -p "$DIR"

V="-f lavfi -i testsrc2=size=320x240:rate=25:duration=1"
VS="-f lavfi -i testsrc2=size=352x288:rate=25:duration=1"   # H.261/H.263 sizes
VD="-f lavfi -i testsrc2=size=720x576:rate=25:duration=1"   # DV PAL
VH="-f lavfi -i testsrc2=size=1920x1080:rate=25:duration=1" # DNxHD
A="-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1"
A44="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=1"
DROPPED=" snow.avi " # Snow: FFmpeg's own experimental codec
A8="-f lavfi -i sine=frequency=440:sample_rate=8000:duration=1"
gen() { # file, ffmpeg arguments (inputs + options)
    local out="$DIR/$1"; shift
    [ -s "$out" ] && return
    command -v "$GEN" >/dev/null || return 0
    # shellcheck disable=SC2068 # word splitting of the argument strings is wanted
    "$GEN" -hide_banner -loglevel error -nostdin -y $@ "$out" 2>"$out.genlog" || { rm -f "$out"; echo "-- not written: $(basename "$out") ($(head -c 160 "$out.genlog"))"; }
    rm -f "$out.genlog"
}

# --- video: today's recorders and phones ---
gen h264_aac.mp4 $V $A -c:v libx264 -pix_fmt yuv420p -c:a aac
gen h264_10bit.mkv $V -c:v libx264 -pix_fmt yuv420p10le
gen h264_422.mov $V -c:v libx264 -pix_fmt yuv422p
gen h264_opus.mkv $V $A -c:v libx264 -c:a libopus
gen h264_flac.mkv $V $A -c:v libx264 -c:a flac
gen h264_pcm.mov $V $A -c:v libx264 -c:a pcm_s16le
gen h264_ac3.mts $V $A -c:v libx264 -c:a ac3 -f mpegts
gen h264_aac.m2ts $V $A -c:v libx264 -c:a aac -f mpegts
gen h264_mp2.ts $V $A -c:v libx264 -c:a mp2
gen h264_aac.flv $V $A -c:v libx264 -c:a aac
gen h264_mp3.avi $V $A -c:v libx264 -c:a libmp3lame
gen h264_aac.3gp $V $A -c:v libx264 -c:a aac
gen h264.264 $V -c:v libx264 -f h264
gen hevc_aac.mp4 $V $A -c:v libx265 -tag:v hvc1 -c:a aac
gen hevc_10bit.mov $V -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1
gen hevc_eac3.mkv $V $A -c:v libx265 -c:a eac3
gen hevc.ts $V -c:v libx265
gen hevc.265 $V -c:v libx265 -f hevc
gen av1_opus.webm $V $A -c:v libaom-av1 -cpu-used 8 -c:a libopus
gen av1_aac.mp4 $V $A -c:v libsvtav1 -c:a aac
gen av1_10bit.mkv $V -c:v libsvtav1 -pix_fmt yuv420p10le
gen av1.ivf $V -c:v libaom-av1 -cpu-used 8
gen av1.obu $V -c:v libaom-av1 -cpu-used 8 -f obu
gen vp9_opus.webm $V $A -c:v libvpx-vp9 -deadline realtime -c:a libopus
gen vp9_alpha.webm "-f lavfi -i testsrc2=size=320x240:rate=25:duration=1,format=yuva420p" -c:v libvpx-vp9 -deadline realtime -pix_fmt yuva420p
gen vp9_vorbis.mkv $V $A -c:v libvpx-vp9 -deadline realtime -c:a libvorbis
gen vp9.mp4 $V -c:v libvpx-vp9 -deadline realtime
gen vp8_vorbis.webm $V $A -c:v libvpx -c:a libvorbis
gen vp8.ivf $V -c:v libvpx
# --- editing / intermediate codecs ---
for p in 0 1 2 3; do gen "prores_$p.mov" $V $A -c:v prores_ks -profile:v $p -c:a pcm_s24le; done
gen prores_4444_alpha.mov "-f lavfi -i testsrc2=size=320x240:rate=25:duration=1,format=yuva444p10le" -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le
gen prores_aw.mov $V -c:v prores_aw
gen dnxhd.mov $VH -c:v dnxhd -b:v 36M -pix_fmt yuv422p
gen dnxhr_hq.mxf $VH -c:v dnxhd -profile:v dnxhr_hq -pix_fmt yuv422p
gen dnxhr_hqx.mov $V -c:v dnxhd -profile:v dnxhr_hqx -pix_fmt yuv422p10le
gen cfhd.mov $V -c:v cfhd
gen hap.mov $V -c:v hap
gen dv.dv $VD -c:v dvvideo -pix_fmt yuv420p -f dv
gen dv_pcm.avi $VD $A -c:v dvvideo -pix_fmt yuv420p -c:a pcm_s16le
gen ffv1.mkv $V -c:v ffv1
gen huffyuv.avi $V -c:v huffyuv
gen ffvhuff.avi $V -c:v ffvhuff
gen utvideo.avi $V -c:v utvideo
gen magicyuv.avi $V -c:v magicyuv
gen zmbv.avi $V -c:v zmbv
gen qtrle.mov $V -c:v qtrle
gen rawvideo.avi $V -c:v rawvideo -pix_fmt yuv420p
gen rawvideo_rgb.mov $V -c:v rawvideo -pix_fmt rgb24
gen v210.mov $V -c:v v210
gen r210.mov $V -c:v r210
gen v410.mov $V -c:v v410
gen y4m.y4m $V -pix_fmt yuv420p
gen flashsv.flv $V -c:v flashsv
gen flashsv2.flv $V -c:v flashsv2
# --- older cameras, DVD, broadcast, web ---
gen mpeg2_mp2.mpg $V $A -c:v mpeg2video -c:a mp2
gen mpeg2_ac3.vob $V $A -c:v mpeg2video -c:a ac3 -f vob
gen mpeg2.ts $V $A -c:v mpeg2video -c:a mp2
gen mpeg2_pcm.mxf $V $A -c:v mpeg2video -c:a pcm_s16le -ar 48000
gen mpeg2.m2v $V -c:v mpeg2video
gen mpeg1.mpg $V $A -c:v mpeg1video -c:a mp2
gen mpeg4_mp3.avi $V $A -c:v mpeg4 -c:a libmp3lame
gen mpeg4_xvid.avi $V -c:v mpeg4 -vtag xvid
gen mpeg4_aac.mp4 $V $A -c:v mpeg4 -c:a aac
gen mpeg4.m4v $V -c:v mpeg4 -f m4v
gen msmpeg4v3.avi $V -c:v msmpeg4
gen msmpeg4v2.avi $V -c:v msmpeg4v2
gen wmv2_wma.wmv $V $A -c:v wmv2 -c:a wmav2
gen wmv1_wma1.asf $V $A -c:v wmv1 -c:a wmav1
gen h263_aac.3gp "-f lavfi -i testsrc2=size=176x144:rate=25:duration=1" $A8 -c:v h263 -c:a aac
gen h263p.mov $VS -c:v h263p
gen h261.avi $VS -c:v h261
gen flv1.flv $V $A44 -c:v flv -c:a libmp3lame
gen mjpeg_pcm.avi $V $A -c:v mjpeg -c:a pcm_s16le
gen mjpeg.mov $V -c:v mjpeg
gen mjpeg.mjpeg $V -c:v mjpeg -f mjpeg
gen theora_vorbis.ogv $V $A -c:v libtheora -c:a libvorbis
gen snow.avi $V -c:v snow
gen svq1.mov $V -c:v svq1
gen cinepak.avi $V -c:v cinepak
gen msvideo1.avi $V -c:v msvideo1
gen rpza.mov $V -c:v rpza
gen smc.mov $V -c:v smc -pix_fmt pal8
gen rv10.rm $V -c:v rv10
gen rv20.rm $V -c:v rv20
gen mpeg4_nut.nut $V $A -c:v mpeg4 -c:a pcm_s16le
gen png.mov $V -c:v png
gen gif_anim.gif $V -pix_fmt pal8
gen apng.apng $V -plays 0
gen jpeg2000.mov $V -c:v jpeg2000
# --- audio ---
gen aac.m4a $A -c:a aac
gen aac.aac $A -c:a aac
gen alac.m4a $A -c:a alac
gen mp3.mp3 $A -c:a libmp3lame
gen mp2.mp2 $A -c:a mp2
gen opus.opus $A -c:a libopus
gen vorbis.ogg $A -c:a libvorbis
gen flac.flac $A -c:a flac
gen flac24.flac $A -c:a flac -sample_fmt s32
for f in s16le s24le s32le f32le u8 f64le alaw mulaw; do gen "pcm_$f.wav" $A -c:a "pcm_$f"; done
gen pcm_s24be.aiff $A -c:a pcm_s24be
gen pcm_s16be.aiff $A -c:a pcm_s16be
gen pcm_s16le.caf $A -c:a pcm_s16le
gen alac.caf $A -c:a alac
gen pcm.w64 $A -c:a pcm_s16le
gen pcm.au $A -c:a pcm_s16be
gen pcm.mka $A -c:a pcm_s24le
gen adpcm_ms.wav $A -c:a adpcm_ms
gen adpcm_ima_wav.wav $A -c:a adpcm_ima_wav
gen adpcm_ima_qt.mov $A -c:a adpcm_ima_qt
gen ac3.ac3 $A -c:a ac3
gen eac3.eac3 $A -c:a eac3
gen ac3_51.ac3 "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1" -af "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0" -c:a ac3
gen dts.dts $A -strict -2 -c:a dca
gen truehd.thd $A -strict -2 -c:a truehd
gen wavpack.wv $A -c:a wavpack
gen tta.tta $A -c:a tta
gen wma2.wma $A -c:a wmav2
gen speex.spx "$A" -c:a libspeex -ar 16000
gen gsm.wav $A8 -c:a libgsm_ms -ar 8000 -ac 1
gen g722.wav $A -c:a g722 -ar 16000 -ac 1
gen g726.wav $A8 -c:a g726 -ar 8000 -ac 1
gen mp3.mka $A -c:a libmp3lame
gen opus.mka $A -c:a libopus
gen aac.mkv $A -c:a aac
gen s302m.ts $A -c:a s302m -ac 2 -strict -2
gen mlp.mlp $A -strict -2 -c:a mlp
# --- pictures ---
P="-f lavfi -i testsrc2=size=320x240:rate=1:duration=1 -frames:v 1"
gen still.png $P
gen still16.png $P -pix_fmt rgb48be
gen still_alpha.png "-f lavfi -i testsrc2=size=320x240:rate=1:duration=1,format=rgba -frames:v 1"
gen still.jpg $P
gen still_444.jpg $P -pix_fmt yuvj444p
gen still.webp $P -c:v libwebp
gen still_lossless.webp $P -c:v libwebp -lossless 1
gen still.gif $P
gen still.tiff $P
gen still_16.tiff $P -pix_fmt rgb48le
gen still.bmp $P
gen still.tga $P
gen still.ppm $P
gen still.pgm $P -pix_fmt gray
gen still.pam $P -c:v pam
gen still.exr $P -pix_fmt gbrpf32le
gen still.dpx $P
gen still.sgi $P
gen still.qoi $P
gen still.hdr $P -pix_fmt gbrpf32le
gen still.j2k $P -c:v jpeg2000
gen still.pcx $P
gen still.xwd $P
gen still.sun $P -c:v sunrast
gen still.xbm $P -pix_fmt monow
gen still.jxl $P -c:v libjxl
gen still.avif $P -c:v libaom-av1 -still-picture 1
gen seq_%03d.png "-f lavfi -i testsrc2=size=320x240:rate=25:duration=0.2"
gen seq_%03d.jpg "-f lavfi -i testsrc2=size=320x240:rate=25:duration=0.2"
[ "$FULL" = - ] && exit 0
FULL="$(realpath "$FULL")"; SLIM="$(realpath "$SLIM")"

# --- decode with both builds ---
probe() { # prefix file -> one line per stream: type codec decoded-frames; or "FAIL <error>"
    local out
    if out="$(LD_LIBRARY_PATH="$1/lib" "$1/bin/ffprobe" -v error -count_frames -read_intervals "%+5" \
        -show_entries stream=codec_type,codec_name,nb_read_frames -of csv=p=0 "$2" 2>&1)" && [ -n "$out" ]; then
        printf '%s' "$out" | tr '\n' ' '
    else
        printf 'FAIL %s' "$(printf '%s' "$out" | head -c 160 | tr '\n' ' ')"
    fi
}
fails=0; diffs=0; n=0
printf '%-28s %-8s %s\n' FILE RESULT "SLIM (FULL when different)"
for f in "$DIR"/*; do
    [ -f "$f" ] || continue
    [[ "$f" == */seq_* && "$f" != */seq_001.* ]] && continue # a sequence: its first picture
    n=$((n + 1))
    full="$(probe "$FULL" "$f")"; slim="$(probe "$SLIM" "$f")"
    name="$(basename "$f")"
    if [[ "$full" == FAIL* ]]; then printf '%-28s %-8s %s\n' "$name" "full-x" "$full"
    elif [ "$slim" != "$full" ] && [[ "$DROPPED" == *" $name "* ]]; then printf '%-28s %-8s %s\n' "$name" dropped "$slim"
    elif [[ "$slim" == FAIL* ]]; then printf '%-28s %-8s %s | full: %s\n' "$name" "FAIL" "$slim" "$full"; fails=$((fails + 1))
    elif [ "$slim" != "$full" ]; then printf '%-28s %-8s %s | full: %s\n' "$name" "DIFF" "$slim" "$full"; diffs=$((diffs + 1))
    else printf '%-28s %-8s %s\n' "$name" ok "$slim"; fi
done
echo "== $n files: $fails fail in slim only, $diffs read differently (full-x = the full build cannot read it either)"
[ "$fails" = 0 ] && [ "$diffs" = 0 ]
