// SPDX-License-Identifier: AGPL-3.0-only
#include "app/BugReport.h"

#include "app/Log.h"
#include "core/I18n.h"
#include "engine/picture/gpu/Gpu.h"

#include <QCoreApplication>
#include <QDir>
#include <QSysInfo>

namespace BugReport {

namespace {

constexpr int kMaxValue = 1500; // characters per field: the whole address stays far below ~6000

QString cut(QString s)
{
    s = s.trimmed();
    if (s.size() > kMaxValue) s = s.left(kMaxValue) + QStringLiteral("…");
    return s;
}

} // namespace

QString shortHome(const QString& path, const QString& home)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString h = QDir::cleanPath(QDir::fromNativeSeparators(home));
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    const QString tilde = QStringLiteral("%USERPROFILE%");
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
    const QString tilde = QStringLiteral("~");
#endif
    QString out = p;
    if (!h.isEmpty() && h != QLatin1String("/")) {
        if (p.compare(h, cs) == 0)
            out = tilde;
        else if (p.startsWith(h + QLatin1Char('/'), cs))
            out = tilde + p.mid(h.size());
    }
    return QDir::toNativeSeparators(out);
}

Facts collect()
{
    Facts f;
    f.version = QCoreApplication::applicationVersion();
    f.os = QSysInfo::prettyProductName();
    f.kernel = QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion();
    f.cpuArch = QSysInfo::currentCpuArchitecture();
    if (Picture::Gpu::enabled()) { // false with SCHNEIDI_GPU=0: then no device is opened just for this
        const Picture::Gpu::DeviceInfo& g = Picture::Gpu::probe();
        f.gpu = g.device;
        f.gpuApi = g.api;
    }
    f.language = I18n::installedLanguage();
    f.logDir = shortHome(Log::directory(), QDir::homePath());
    return f;
}

QUrl issueUrl(const Facts& f)
{
    QString system = f.os;
    if (!f.kernel.trimmed().isEmpty()) system += QStringLiteral(", kernel ") + f.kernel.trimmed();
    if (!f.cpuArch.isEmpty()) system += QStringLiteral(", ") + f.cpuArch;
    if (!f.gpu.isEmpty())
        system += QStringLiteral(", GPU: ") + f.gpu + (f.gpuApi.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(f.gpuApi));
    if (!f.language.isEmpty()) system += QStringLiteral(", UI language: ") + f.language;

    const QString log =
        QStringLiteral("Please attach schneidi.log from %1 (drag the file here; logs/kept/ has the logs of runs that "
                       "crashed or hung). Starting with `schneidi --verbose` writes more detail.")
            .arg(f.logDir.isEmpty() ? QStringLiteral("the log folder (Help › Open Log Folder)") : f.logDir);

    // Every value fully percent-encoded by hand: "+" "&" "#" "%" in a GPU name or path must not break the query
    QByteArray query = "template=bug_report.yml";
    auto add = [&query](const char* key, const QString& value) {
        query += '&';
        query += key;
        query += '=';
        query += QUrl::toPercentEncoding(cut(value));
    };
    add("version", f.version);
    add("system", system);
    add("log", log);
    QUrl url(QStringLiteral("https://github.com/OnkelTheorie/schneidi/issues/new"));
    url.setQuery(QString::fromLatin1(query), QUrl::StrictMode);
    return url;
}

} // namespace BugReport
