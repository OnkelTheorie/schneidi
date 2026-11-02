// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ExternalEncoder.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace {

constexpr int kWaitMs = 500;          // steps of waiting, so a cancelled export stops soon
constexpr int kStartMs = 15000;
constexpr int kBufferedFrames = 3;    // pictures the pipe may hold before write() waits

QString rate(AVRational r)
{
    return QString("%1/%2").arg(r.num).arg(std::max(1, r.den));
}

} // namespace

QStringList ExternalEncoder::arguments(const Params& p)
{
    const char* pix = av_get_pix_fmt_name(p.pixFmt);
    QStringList a{"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y",
                  // input: raw pictures from stdin
                  "-f", "rawvideo", "-pix_fmt", pix ? pix : "yuv420p",
                  "-video_size", QString("%1x%2").arg(p.width).arg(p.height), "-framerate", rate(p.rate), "-i", "pipe:0",
                  "-map", "0:v:0", "-an", "-sn", "-dn", "-c:v", p.codec, "-pix_fmt", pix ? pix : "yuv420p"};
    if (p.threads > 0) a << "-threads:v" << QString::number(p.threads);
    // Options: "vprofile" style names (the export formats') without their prefix, as the writer's applyOption does
    for (const auto& [key, value] : p.options) {
        QString name = QString::fromUtf8(key);
        if (name == "vprofile" || name == "vb" || name == "vlevel") name = name.mid(1);
        a << QString("-%1:v").arg(name) << QString::fromUtf8(value);
    }
    // Color tags into the stream (the writer copies the stream's parameters into the file)
    if (const char* m = av_color_space_name(p.matrix); m && p.matrix != AVCOL_SPC_UNSPECIFIED) a << "-colorspace:v" << m;
    if (const char* t = av_color_transfer_name(p.transfer); t && p.transfer != AVCOL_TRC_UNSPECIFIED)
        a << "-color_trc:v" << t;
    if (const char* c = av_color_primaries_name(p.primaries); c && p.primaries != AVCOL_PRI_UNSPECIFIED)
        a << "-color_primaries:v" << c;
    if (const char* r = av_color_range_name(p.range); r && p.range != AVCOL_RANGE_UNSPECIFIED) a << "-color_range:v" << r;
    if (p.aspect.num > 0 && p.aspect.den > 0 && p.aspect.num != p.aspect.den)
        a << "-aspect:v" << QString::number(double(p.width) * p.aspect.num / (double(p.height) * p.aspect.den), 'f', 6);
    if (p.codec == "libx265") a << "-x265-params" << "log-level=error";
    a << "-f" << "nut" << p.output;
    return a;
}

QString ExternalEncoder::intermediatePath(const QString& path)
{
    const QFileInfo info(path);
    return info.dir().filePath("." + info.fileName() + ".schneidi-video.nut");
}

ExternalEncoder::ExternalEncoder(Params p) : m_p(std::move(p)) {}

ExternalEncoder::~ExternalEncoder()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill(); // our own child
        m_process->waitForFinished(3000);
    }
}

QString ExternalEncoder::failure(const QString& what)
{
    QString log;
    if (m_process) log = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
    if (log.size() > 600) log = "…" + log.right(600);
    return log.isEmpty() ? what : what + ": " + log;
}

bool ExternalEncoder::start(QString* error)
{
    m_frameBytes = av_image_get_buffer_size(m_p.pixFmt, m_p.width, m_p.height, 1);
    if (m_frameBytes <= 0) {
        if (error) *error = QStringLiteral("pixel format cannot be passed to %1").arg(m_p.codec);
        return false;
    }
    m_buffer.resize(size_t(m_frameBytes));
    m_process = std::make_unique<QProcess>();
    m_process->setStandardOutputFile(QProcess::nullDevice());
    m_process->start(m_p.program, arguments(m_p));
    if (!m_process->waitForStarted(kStartMs)) {
        if (error) *error = QStringLiteral("%1 (x264/x265 extension) does not start: %2").arg(m_p.program, m_process->errorString());
        return false;
    }
    return true;
}

bool ExternalEncoder::write(const AVFrame* picture, const std::atomic<bool>& stop, QString* error)
{
    const int n = av_image_copy_to_buffer(m_buffer.data(), m_frameBytes, picture->data, picture->linesize, m_p.pixFmt,
                                          m_p.width, m_p.height, 1);
    if (n != m_frameBytes) {
        if (error) *error = QStringLiteral("picture does not fit the %1 input").arg(m_p.codec);
        return false;
    }
    if (m_process->write(reinterpret_cast<const char*>(m_buffer.data()), n) != n) {
        if (error) *error = failure(QStringLiteral("%1 does not take the pictures").arg(m_p.codec));
        return false;
    }
    // Keep the pipe short (memory): wait while more than a few pictures are still on their way
    while (m_process->bytesToWrite() > qint64(m_frameBytes) * kBufferedFrames) {
        if (stop) return false;
        if (!m_process->waitForBytesWritten(kWaitMs) && m_process->state() == QProcess::NotRunning) {
            if (error) *error = failure(QStringLiteral("%1 (x264/x265 extension) stopped").arg(m_p.codec));
            return false;
        }
    }
    return true;
}

bool ExternalEncoder::finish(const std::atomic<bool>& stop, QString* error)
{
    while (m_process->bytesToWrite() > 0 && m_process->state() != QProcess::NotRunning) {
        if (stop) return false;
        m_process->waitForBytesWritten(kWaitMs);
    }
    m_process->closeWriteChannel();
    while (m_process->state() != QProcess::NotRunning) {
        if (stop) return false;
        m_process->waitForFinished(kWaitMs);
    }
    if (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0) {
        if (error) *error = failure(QStringLiteral("%1 (x264/x265 extension) failed").arg(m_p.codec));
        return false;
    }
    return true;
}
