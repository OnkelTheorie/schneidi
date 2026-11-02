// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Export writer: the pictures come from Picture::Renderer, rendered by worker threads of the writer (several frames
// at once, at most a few frames ahead of the encoder) and converted to the encoder's YUV right there
// (OutputColor::fromRgba, into a pooled AVFrame: no further copy). The sound comes from Audio::Mixer in frame order
// (the samples of each output frame on an exact grid). One thread fetches finished frames in order (picture and
// sound), another encodes (libavcodec) and muxes (libavformat). Writes video files, GIF intermediates, image
// sequences (muxer "image2"), audio only, VAAPI (upload to the card) and QSV. Option names are AVOptions, also with
// the "v"/"a" prefix like "vprofile" (the export formats' names). A different output frame rate shows timeline frame
// from + round(k * timelineFps / outputFps) at output frame k; a repeated timeline frame is rendered once.
//
// Smart render (Settings::copy, engine/SmartRender): where the output shows a file as it is, whole GOPs (key frame
// to key frame) of the file are copied instead of encoded, if the file fits (same codec H.264/H.265 as the encoder,
// size, frame rate, 4:2:0 format, color, progressive). The stretch from a cut to the next key frame and everything
// with effects is encoded, piece by piece, each piece starting with a key frame (closed, no B-frames). All pictures go
// into one stream: parameter sets in-band before every key frame (Annex B, the muxer packs them), decode times
// = packet number - kDelay, so copied GOPs with B-frames (reordered up to kDelay frames) fit. The sound comes from the
// mixer (always encoded). Problems with one GOP only mean encoding it; problems with the whole stream
// end the export with smartFailed() (the Exporter renders again without).

#include "core/ProjectFormat.h"
#include "engine/SmartRender.h"

#include <QByteArray>
#include <QPair>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>

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
        Options videoOptions;  // AVOptions of the encoder ("crf", "vprofile" …)
        QString audioCodec;    // empty = no sound
        QString sampleFormat;  // sample format of the audio encoder (FFmpeg name)
        int audioBitrateK = 0; // 0 = codec default
        int audioRate = 48000, audioChannels = 2;
        bool fastStart = false; // MP4/MOV: index at the front ("movflags=+faststart")
        int threads = 0;        // encoder threads; 0 = FFmpeg decides
        int length = 0; // frames to write
        // Smart render: output frames that show a file as it is (sorted, engine/SmartRender). Empty = encode everything.
        QVector<SmartRender::Span> copy;
        int maxBitrateK = 0; // smart render: a GOP above the export's bit rate cap (kbit/s) is encoded; 0 = no cap
        // Muxer by name (image sequences: "image2"); empty = from the file name
        QString muxer;
        QString hwDevice; // VAAPI encoder: the render device (/dev/dri/renderD128)
        // x264/x265 of the extension: its ffmpeg program encodes the video (engine/ExternalEncoder; no smart render)
        QString externalProgram;
    };
    // Picture engine and audio mixer
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
    // Can the writer write this? videoCodec empty = audio only, pixFmt "vaapi" = upload to the card. why: what does not
    // fit, for the user ("the .webm container cannot hold libx264 video"). libx264/libx265 count also when only the
    // extension has them (the caller decides that, HardwareEncoders::externalProgram).
    static bool supportsOwn(const QString& path, const QString& muxer, const QString& videoCodec, const QString& pixFmt,
                            const QString& audioCodec, const QString& sampleFormat, QString* why = nullptr);
    // Does the container of `path` (by its extension, or `muxer`) take these encoders' streams? Empty = none of that kind
    static bool containerTakes(const QString& path, const QString& muxer, const QString& videoCodec,
                               const QString& audioCodec, QString* why = nullptr);
    // As containerTakes, but only where FFmpeg knows it: the muxer lists the codec, or it is its default codec (a
    // muxer that does not say - ogg, flac - counts as no). For suggesting and accepting file extensions.
    // soundOnly: and the container is one for sound (no video by default: .aac yes, .mp4 no)
    static bool containerSurelyTakes(const QString& path, const QString& videoCodec, const QString& audioCodec,
                                     bool soundOnly = false);

    // Smart render (s.copy): the mixer delivers the sound of every output frame
    ExportWriter(const Settings& s, Own own);
    ~ExportWriter(); // cancels when still running
    void start();
    // Stops the threads and the render workers and waits; the file stays as it is (the caller removes it)
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
