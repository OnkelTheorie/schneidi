// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/OpenMedia.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <QByteArray>
#include <QList>

namespace OpenMedia {

namespace {

// Demuxers that open further files, addresses or programs named inside the file (FFmpeg 7; those a build does not have
// are simply missing): playlists and manifests (hls, dash, webm_dash_manifest), compositions (imf), lists of files
// (concat), stream descriptions (sdp, rtp, rtsp), a subtitle's sibling file (vobsub), filter graphs and scripts (lavfi,
// avisynth, vapoursynth) and disc structures (dvdvideo)
const QList<QByteArray>& denied()
{
    static const QList<QByteArray> names{"hls",    "applehttp", "dash",   "webm_dash_manifest", "imf",
                                         "concat", "sdp",       "rtp",    "rtsp",               "vobsub",
                                         "lavfi",  "avisynth",  "vapoursynth", "dvdvideo"};
    return names;
}

// All demuxers of this build but the refused ones, as FFmpeg's "format_whitelist" (checked after probing, before the
// demuxer reads its header and with it anything the file names)
const QByteArray& allowed()
{
    static const QByteArray list = [] {
        QByteArray out;
        void* it = nullptr;
        while (const AVInputFormat* f = av_demuxer_iterate(&it)) {
            if (refused(f->name)) continue;
            if (!out.isEmpty()) out += ',';
            out += f->name;
        }
        return out;
    }();
    return list;
}

} // namespace

bool refused(const char* demuxerName)
{
    if (!demuxerName) return false;
    for (const QByteArray& n : QByteArray(demuxerName).split(','))
        if (denied().contains(n)) return true;
    return false;
}

int input(AVFormatContext** fmt, const char* path, const AVDictionary* extra)
{
    AVDictionary* options = nullptr;
    if (extra) av_dict_copy(&options, extra, 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", allowed().constData(), 0);
    const int r = avformat_open_input(fmt, path, nullptr, &options);
    av_dict_free(&options);
    return r;
}

} // namespace OpenMedia
