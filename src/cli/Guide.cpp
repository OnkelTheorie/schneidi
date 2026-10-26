#include "cli/Guide.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Cli::Guide {

namespace {

const QString kRoot = QStringLiteral(":/schneidi");

QString readResource(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

QStringList topics()
{
    QStringList out;
    for (const QFileInfo& fi : QDir(kRoot + "/references").entryInfoList({"*.md"}, QDir::Files, QDir::Name))
        out << fi.completeBaseName();
    return out;
}

QString title(const QString& topic)
{
    const QString t = text(topic);
    const QString first = t.section('\n', 0, 0);
    return first.startsWith("# ") ? first.mid(2).trimmed() : topic;
}

QString text(const QString& topic)
{
    if (!topic.isEmpty()) {
        if (topic.contains('/') || topic.contains('\\') || topic.contains("..")) return {};
        return readResource(kRoot + "/references/" + topic + ".md");
    }
    QString t = readResource(kRoot + "/SKILL.md");
    // Front matter (name, description) is for skill loaders, not for reading
    if (t.startsWith("---\n")) {
        const qsizetype end = t.indexOf("\n---\n", 4);
        if (end > 0) t = t.mid(end + 5).trimmed() + '\n';
    }
    return t;
}

bool installSkill(const QString& dir, QString* error)
{
    QDir target(dir);
    if (target.exists() && !target.removeRecursively()) {
        if (error) *error = "cannot replace " + QDir::toNativeSeparators(dir);
        return false;
    }
    QDirIterator it(kRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = target.filePath(src.mid(kRoot.size() + 1));
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile in(src);
        QSaveFile out(dst);
        if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly) || out.write(in.readAll()) < 0
            || !out.commit()) {
            if (error) *error = "cannot write " + QDir::toNativeSeparators(dst);
            return false;
        }
    }
    return true;
}

} // namespace Cli::Guide
