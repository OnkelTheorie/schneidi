// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Own export path for video files (plan 0.11): MLT's render workers produce the images as before, but fetching and
// encoding run in two threads of their own. MLT's avformat consumer fetched, copied and encoded in a single thread,
// which was the bottleneck of every export (the workers waited for it, see archiv/plan-0.10.md › Export-Durchsatz).
// Here one thread fetches finished frames in order (picture and sound), the other encodes (libavcodec) and muxes
// (libavformat). The picture reaches the encoder without a copy: the MLT image buffer becomes a refcounted AVFrame.
//
// Behaves like MLT's avformat consumer (7.30, consumer_avformat.c): the same option names (AVOptions, also with the
// "v"/"a" prefix like "vprofile"), color tags from the profile's color standard, sound in the encoder's sample format,
// and the same rare conversions of its encoder thread (another matrix, full range, packed 4:2:2 -> planar).
// GIF, image sequences, audio only and VAAPI/QSV stay with MLT's consumer on this path: the Exporter asks supports().
//
// Own picture engine (0.15, the default): the pictures come from Picture::Renderer instead of MLT, rendered by worker
// threads of the writer (several frames at once, at most a few frames ahead of the encoder) and converted to the
// encoder's YUV right there (OutputColor::fromRgba, into a pooled AVFrame: no further copy). The sound comes from
// Audio::Mixer in frame order (the samples of each output frame on an exact grid, as engine/MixedSound did). This path
// writes every output the Exporter has – video files, GIF intermediates, image sequences (muxer "image2"), audio
// only, VAAPI (upload to the card) and QSV – so MLT's avformat consumer is not needed with it. A different output
// frame rate shows timeline frame from + round(k * timelineFps / outputFps) at output frame k (as engine/RateConverter);
// a repeated timeline frame is rendered once.
//
// Smart render (Settings::copy, engine/SmartRender): where the output shows a file as it is, whole GOPs (key frame
// to key frame) of the file are copied instead of encoded, if the file fits (same codec H.264/H.265 as the encoder,
// size, frame rate, 4:2:0 format, color, progressive). The stretch from a cut to the next key frame and everything
// with effects is encoded, piece by piece, each piece starting with a key frame (closed, no B-frames). All pictures go
// into one stream: parameter sets in-band before every key frame (Annex B, the muxer packs them), decode times
// = packet number - kDelay, so copied GOPs with B-frames (reordered up to kDelay frames) fit. The sound comes from a
// producer of its own (always encoded). Problems with one GOP only mean encoding it; problems with the whole stream
// end the export with smartFailed() (the Exporter renders again without).

#include "core/ProjectFormat.h"
#include "engine/SmartRender.h"

#include <QByteArray>
#include <QPair>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>

namespace Mlt {
class Profile;
class Producer;
} // namespace Mlt
namespace Picture {
class Renderer;
}
namespace Audio {
class Mixer;
}

class ExportWriter {
public:
    using Options = QVector<QPair<QByteArray, QByteArray>>;
    struct Settings {
        QString path;
        QString videoCodec;    // FFmpeg encoder
        QString pixFmt;        // pixel format of the encoder (FFmpeg name)
        Options videoOptions;  // AVOptions of the encoder, named like MLT consumer properties ("crf", "vprofile" …)
        QString audioCodec;    // empty = no sound
        QString sampleFormat;  // sample format of the audio encoder (FFmpeg name)
        int audioBitrateK = 0; // 0 = codec default
        int audioRate = 48000, audioChannels = 2;
        bool fastStart = false; // MP4/MOV: index at the front ("movflags=+faststart")
        int threads = 0;        // encoder threads; 0 = FFmpeg decides
        // MLT "real_time" of the render workers: -N = N frames at once, 0 = in order in the fetch thread (frame rate
        // conversion, see Exporter::startConsumer)
        int realTime = -1;
        int length = 0; // frames to write
        // Smart render: output frames that show a file as it is (sorted, engine/SmartRender). Needs `sound` in the
        // constructor. Empty = encode everything.
        QVector<SmartRender::Span> copy;
        int maxBitrateK = 0; // smart render: a GOP above the export's bit rate cap (kbit/s) is encoded; 0 = no cap
        // Own path: muxer by name (image sequences: "image2"); empty = from the file name
        QString muxer;
        QString hwDevice; // VAAPI encoder: the render device (/dev/dri/renderD128)
    };
    // Own picture engine and audio mixer instead of MLT
    struct Own {
        std::shared_ptr<Picture::Renderer> renderer; // nullptr = audio only
        std::shared_ptr<Audio::Mixer> mixer;         // nullptr = no sound
        QSize size;              // output size (even)
        FrameRate rate;          // output frame rate
        FrameRate timelineRate;  // frame rate of the timeline (differs: frame rate conversion)
        int colorspace = 709;    // matrix and tags of the file
        int64_t from = 0;        // timeline frame of output frame 0
        int audioBits = 0;       // integer sample formats: rounded to this many bits (0 = the full format)
        int parallel = 1;        // frames rendered at once
    };
    // Can this path write such a file? (muxer from the file name, encoders, MLT image format for pixFmt)
    static bool supports(const QString& path, const QString& videoCodec, const QString& pixFmt, const QString& audioCodec,
                         const QString& sampleFormat);
    // Own path wanted? SCHNEIDI_EXPORT=mlt (or the setting "render/writer" = "mlt") goes back to MLT's consumer
    static bool wanted();
    // Can the own path (Own) write this? videoCodec empty = audio only, pixFmt "vaapi" = upload to the card
    static bool supportsOwn(const QString& path, const QString& muxer, const QString& videoCodec, const QString& pixFmt,
                            const QString& audioCodec, const QString& sampleFormat);
    static void setWanted(bool own); // the setting "render/writer"

    // source (and sound) must outlive the writer (the Exporter destroys the writer first). sound: smart render only,
    // delivers the sound of the output frames (the source then only the pictures)
    ExportWriter(Mlt::Profile& profile, Mlt::Producer& source, const Settings& s, Mlt::Producer* sound = nullptr);
    // Own path. Smart render (s.copy) works here too: the mixer delivers the sound of every output frame.
    ExportWriter(const Settings& s, Own own);
    ~ExportWriter(); // cancels when still running
    void start();
    // Stops both threads and MLT's render workers and waits; the file stays as it is (the caller removes it)
    void cancel();

    enum class State { Running, Done, Failed };
    State state() const;
    // The video encoder could not be opened or failed while encoding (hardware encoder: render again with software)
    bool encoderFailed() const;
    QString error() const;
    int framesWritten() const;
    int framesCopied() const; // smart render: of framesWritten()
    // Smart render failed as a whole (e.g. the muxer refused the stream): render again without
    bool smartFailed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
