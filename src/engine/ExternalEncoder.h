// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// x264/x265 through the encoder extension (engine/Extensions "x264"). They are GPL, so schneidi never links them: the
// extension's ffmpeg program runs as a separate process, gets the export's finished pictures (already converted to
// the encoder's YUV by the export writer) as raw video on stdin and writes the encoded stream into a NUT file next to
// the output (exact time stamps, B-frames kept, readable by the bundled FFmpeg). The export writer then copies that
// stream into the real file together with the sound it encoded itself, so container, tags and fast start stay its own.

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

struct AVFrame;
class QProcess;

class ExternalEncoder {
public:
    struct Params {
        QString program;  // the extension's ffmpeg
        QString codec;    // libx264 / libx265
        int width = 0, height = 0;
        AVRational rate{25, 1};
        AVRational aspect{1, 1};
        AVPixelFormat pixFmt = AV_PIX_FMT_YUV420P; // of the pictures written (and of the stream)
        AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
        AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
        AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
        AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
        // AVOptions of the encoder as the export writer gets them ("crf", "preset", "maxrate", "vprofile" …)
        QVector<QPair<QByteArray, QByteArray>> options;
        int threads = 0;  // 0 = the encoder decides
        QString output;   // the NUT file to write
    };
    // Command line of the program (no shell): raw video from stdin -> codec -> NUT file
    static QStringList arguments(const Params& p);
    // Intermediate file for an export to `path`: beside it (same disk), hidden name
    static QString intermediatePath(const QString& path);

    explicit ExternalEncoder(Params p);
    ~ExternalEncoder(); // stops a program still running (our own child)

    bool start(QString* error);
    // One picture (pixFmt, width x height). Blocks while the program is behind; false = it failed or `stop` was set
    bool write(const AVFrame* picture, const std::atomic<bool>& stop, QString* error);
    // End of the pictures: waits until the program has finished the file
    bool finish(const std::atomic<bool>& stop, QString* error);

private:
    QString failure(const QString& what);
    Params m_p;
    std::unique_ptr<QProcess> m_process;
    std::vector<uint8_t> m_buffer;
    int m_frameBytes = 0;
};
