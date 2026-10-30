// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MltLog.h"

#include <QStringDecoder>

#include <framework/mlt.h>

#include <cstdarg>
#include <cstdio>

namespace MltLog {

namespace {

Sink s_sink = nullptr;

void handler(void* service, int level, const char* fmt, va_list args)
{
    if (level > mlt_log_get_level()) return;
    char buf[2048];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);
    // Mostly UTF-8, on Windows paths partly in the ANSI code page
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = utf8(QByteArrayView(buf));
    if (utf8.hasError()) text = QString::fromLocal8Bit(buf);
    if (service) {
        const char* id = mlt_properties_get(MLT_SERVICE_PROPERTIES(static_cast<mlt_service>(service)), "mlt_service");
        if (id) text = QString("[%1] %2").arg(QString::fromUtf8(id), text);
    }
    if (s_sink) s_sink(level <= MLT_LOG_ERROR, text);
    std::fputs(buf, stderr);
}

} // namespace

void install(Sink sink)
{
    s_sink = sink;
    mlt_log_set_callback(handler);
}

} // namespace MltLog
