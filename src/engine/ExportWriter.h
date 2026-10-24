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
// GIF, image sequences, audio only and VAAPI/QSV stay with MLT's consumer: the Exporter asks supports().

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>
#include <memory>

namespace Mlt {
class Profile;
class Producer;
} // namespace Mlt

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
    };
    // Can this path write such a file? (muxer from the file name, encoders, MLT image format for pixFmt)
    static bool supports(const QString& path, const QString& videoCodec, const QString& pixFmt, const QString& audioCodec,
                         const QString& sampleFormat);
    // Own path wanted? SCHNEIDI_EXPORT=mlt (or the setting "render/writer" = "mlt") goes back to MLT's consumer
    static bool wanted();

    // source must outlive the writer (the Exporter destroys the writer first)
    ExportWriter(Mlt::Profile& profile, Mlt::Producer& source, const Settings& s);
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

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
