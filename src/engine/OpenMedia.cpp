// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/OpenMedia.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QList>

#include <algorithm>
#include <cstring>
#include <vector>

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

bool notAFile(const char* path)
{
    // Only what exists: a picture sequence's pattern ("shot_%05d.png") names no file itself. QFileInfo follows a
    // symbolic link to what it points at (a link to /dev/zero is a device).
    const QFileInfo fi(QString::fromUtf8(path));
    return fi.exists() && !fi.isFile();
}

int input(AVFormatContext** fmt, const char* path, const AVDictionary* extra)
{
    // Folders, named pipes (FIFOs), devices and sockets are no media files: opening a pipe nobody writes into blocks
    // forever (a hanging import, probe or render), /dev/zero or /dev/urandom never end
    if (path && notAFile(path)) return AVERROR(EINVAL);
    AVDictionary* options = nullptr;
    if (extra) av_dict_copy(&options, extra, 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", allowed().constData(), 0);
    const int r = avformat_open_input(fmt, path, nullptr, &options);
    av_dict_free(&options);
    return r;
}

namespace {
thread_local std::vector<ReadLimit*> t_limits; // the ReadLimits alive on this thread (demuxers read on the caller's)
} // namespace

namespace {

// Byte positions of the Matroska clusters that start in [from, from + bytes) of the file, last first (at most `max`):
// the cluster ID 1F 43 B6 75, a valid size and the cluster's timestamp element (E7) first, as muxers write it (after
// a CRC-32 element at most)
// (the four bytes alone also turn up inside packets)
std::vector<int64_t> clustersIn(const char* path, int64_t from, int64_t bytes, int max)
{
    std::vector<int64_t> out;
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly) || !f.seek(from)) return out;
    const QByteArray b = f.read(bytes);
    const auto u = [&b](qsizetype i) { return uint8_t(b[i]); };
    for (qsizetype i = b.size() - 4; i >= 0 && int(out.size()) < max; --i) {
        if (u(i) != 0x1F || u(i + 1) != 0x43 || u(i + 2) != 0xB6 || u(i + 3) != 0x75) continue;
        if (i + 4 >= b.size() || u(i + 4) == 0) continue;
        int len = 1; // EBML size: as many bytes as the first one's leading zeros + 1
        while (!(u(i + 4) & (0x80 >> (len - 1)))) ++len;
        qsizetype at = i + 4 + len;
        if (at + 1 < b.size() && u(at) == 0xBF && u(at + 1) == 0x84) at += 6; // a CRC-32 first (FFmpeg writes one)
        if (at < b.size() && u(at) == 0xE7) out.push_back(from + i);
    }
    return out;
}

} // namespace

void scanTail(AVFormatContext* fmt, const std::function<bool(const AVPacket*)>& take)
{
    if (!fmt || !fmt->pb) return;
    const int64_t size = avio_size(fmt->pb);
    const bool matroska = fmt->iformat && fmt->iformat->name && strstr(fmt->iformat->name, "matroska");
    AVPacket* p = av_packet_alloc();
    if (!p) return;
    const ReadLimit limit(fmt, kTailScanMaxBytes);
    bool counted = false;
    for (int64_t tail = 1 << 20; !counted && !limit.reached(); tail *= 8) {
        const int64_t from = size > tail ? size - tail : 0;
        std::vector<int64_t> starts{from};
        // The last clusters (the very last may be cut off without a whole packet)
        if (matroska && size > 0) starts = clustersIn(fmt->url, from, std::min(tail, size - from), 3);
        for (const int64_t at : starts) {
            if (counted || limit.reached()) break;
            if (av_seek_frame(fmt, -1, at, AVSEEK_FLAG_BYTE) < 0) continue;
            // A packet from before the place sought is none of the tail's: one the demuxer still had queued, or it
            // resynced further back (read on, the ReadLimit ends that)
            while (av_read_frame(fmt, p) >= 0) {
                if ((p->pos < 0 || p->pos >= at) && take(p)) counted = true;
                av_packet_unref(p);
            }
        }
        if (from == 0 || tail >= kTailScanMaxBytes) break; // read from the start, or enough
    }
    av_packet_free(&p);
}

ReadLimit::ReadLimit(AVFormatContext* fmt, long long bytes) : m_left(bytes)
{
    if (!fmt || !fmt->pb || (fmt->flags & AVFMT_FLAG_CUSTOM_IO) || !fmt->pb->read_packet) return;
    m_pb = fmt->pb;
    m_opaque = m_pb->opaque;
    m_read = m_pb->read_packet;
    m_pb->read_packet = &ReadLimit::read;
    t_limits.push_back(this);
}

ReadLimit::~ReadLimit()
{
    if (!m_pb) return;
    m_pb->read_packet = m_read;
    t_limits.erase(std::remove(t_limits.begin(), t_limits.end(), this), t_limits.end());
}

int ReadLimit::read(void* opaque, uint8_t* buf, int size)
{
    for (auto it = t_limits.rbegin(); it != t_limits.rend(); ++it) {
        ReadLimit* l = *it;
        if (l->m_opaque != opaque) continue;
        if (l->m_left <= 0) return AVERROR_EXIT;
        const int n = l->m_read(opaque, buf, int(std::min<long long>(size, l->m_left)));
        if (n > 0) l->m_left -= n;
        return n;
    }
    return AVERROR(EIO); // read on another thread than the limit's: not supported
}

} // namespace OpenMedia
