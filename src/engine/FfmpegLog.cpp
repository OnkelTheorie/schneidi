// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/FfmpegLog.h"
#include "engine/Bundle.h"

#include <QStringDecoder>

extern "C" {
#include <libavutil/log.h>
}

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace FfmpegLog {

namespace {

Sink s_sink = nullptr;

void handler(void* avcl, int level, const char* fmt, va_list args)
{
    if (level > av_log_get_level()) return;
    char buf[2048];
    int prefix = 1;
    if (fmt && std::strncmp(fmt, "Format not on whitelist", 23) == 0) {
        // A file refused by its demuxer (engine/OpenMedia): FFmpeg would print the whole allowed list (~3 KB)
        av_log_format_line2(avcl, level, "Format not allowed (opens other files or addresses), file refused\n", args,
                            buf, sizeof buf, &prefix);
    } else {
        va_list copy;
        va_copy(copy, args);
        av_log_format_line2(avcl, level, fmt, copy, buf, sizeof buf, &prefix);
        va_end(copy);
    }
    // Mostly UTF-8, on Windows paths partly in the ANSI code page
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = utf8(QByteArrayView(buf));
    if (utf8.hasError()) text = QString::fromLocal8Bit(buf);
    if (s_sink) s_sink(level <= AV_LOG_ERROR, text.trimmed());
    std::fputs(buf, stderr);
}

} // namespace

void install(Sink sink)
{
    s_sink = sink;
    // Warnings are noise in normal operation, e.g. "[opus] Could not update timestamps for skipped/discarded samples"
    av_log_set_level(Bundle::verboseLogging() ? AV_LOG_WARNING : AV_LOG_ERROR);
    av_log_set_callback(handler);
}

} // namespace FfmpegLog
