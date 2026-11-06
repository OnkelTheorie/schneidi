// SPDX-License-Identifier: AGPL-3.0-only
#include "core/BrandKit.h"

#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

std::optional<QColor> BrandKit::color(int n) const
{
    if (n < 1 || n > colors.size() || !colors[n - 1].isValid()) return std::nullopt;
    return colors[n - 1];
}

std::optional<QString> BrandKit::font(int n) const
{
    if (n < 1 || n > fonts.size() || fonts[n - 1].trimmed().isEmpty()) return std::nullopt;
    return fonts[n - 1];
}

BrandKit BrandKit::clamped() const
{
    BrandKit k = *this;
    k.logoCorner = std::clamp(k.logoCorner, 0, 4);
    k.logoSize = std::isfinite(k.logoSize) ? std::clamp(k.logoSize, 0.01, 1.0) : BrandKit().logoSize;
    k.colors.removeIf([](const QColor& c) { return !c.isValid(); });
    if (k.colors.size() > kMaxColors) k.colors.resize(kMaxColors);
    for (QString& f : k.fonts) f = f.trimmed();
    k.fonts.removeAll(QString());
    while (k.fonts.size() > kMaxFonts) k.fonts.removeLast();
    return k;
}

namespace Brand {

namespace {

// A Windows path (C:\x, \\server\share) with forward slashes on every system (QDir::fromNativeSeparators
// leaves backslashes alone on Linux, where a kit or project from Windows is read too)
bool windowsPath(const QString& p)
{
    return (p.size() > 2 && p[0].isLetter() && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || p.startsWith("\\\\");
}
QString portable(const QString& p) { return windowsPath(p) ? QString(p).replace('\\', '/') : QDir::fromNativeSeparators(p); }
bool absoluteAnyOs(const QString& p) { return QDir::isAbsolutePath(p) || windowsPath(p) || p.startsWith("//"); }

// A path as written: absolute always (with forward slashes, also Windows paths), relative only with a base
void putPath(QJsonObject& o, const char* key, const QString& path, const QDir* base)
{
    if (path.isEmpty()) return;
    const QString abs = portable(path);
    o[key] = abs;
    if (base && QDir::isAbsolutePath(abs)) o[QString(key) + "Rel"] = base->relativeFilePath(abs);
}

// The absolute path if the file is there, else the relative one against the base if that one is, else the absolute
// one (stays missing until the user picks another file)
QString getPath(const QJsonObject& o, const char* key, const QDir* base)
{
    const QString abs = portable(o.value(key).toString());
    if (abs.isEmpty() || !base) return abs;
    if (absoluteAnyOs(abs) && QFileInfo::exists(abs)) return abs;
    const QString rel = o.value(QString(key) + "Rel").toString();
    if (!rel.isEmpty()) {
        const QString candidate = QDir::cleanPath(base->absoluteFilePath(rel));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return absoluteAnyOs(abs) ? abs : QDir::cleanPath(base->absoluteFilePath(abs));
}

} // namespace

QJsonObject toJson(const BrandKit& kit0, const QDir* base)
{
    const BrandKit kit = kit0.clamped();
    const BrandKit def;
    QJsonObject o;
    putPath(o, "logo", kit.logo, base);
    if (kit.logoCorner != def.logoCorner) o["logoCorner"] = kit.logoCorner;
    if (kit.logoSize != def.logoSize) o["logoSize"] = kit.logoSize;
    if (!kit.colors.isEmpty()) {
        QJsonArray colors;
        for (const QColor& c : kit.colors) colors << c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
        o["colors"] = colors;
    }
    if (!kit.fonts.isEmpty()) o["fonts"] = QJsonArray::fromStringList(kit.fonts);
    putPath(o, "intro", kit.intro, base);
    putPath(o, "outro", kit.outro, base);
    return o;
}

BrandKit fromJson(const QJsonObject& o, const QDir* base)
{
    BrandKit k;
    k.logo = getPath(o, "logo", base);
    k.logoCorner = o.value("logoCorner").toInt(k.logoCorner);
    k.logoSize = o.value("logoSize").toDouble(k.logoSize);
    for (const QJsonValue& v : o.value("colors").toArray()) k.colors << QColor(v.toString());
    for (const QJsonValue& v : o.value("fonts").toArray()) k.fonts << v.toString();
    k.intro = getPath(o, "intro", base);
    k.outro = getPath(o, "outro", base);
    return k.clamped();
}

QString globalPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("brand-kit.json");
}

BrandKit global()
{
    QFile f(globalPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool setGlobal(const BrandKit& kit, QString* err)
{
    const QString path = globalPath();
    if (kit.clamped().isEmpty()) {
        if (!QFileInfo::exists(path) || QFile::remove(path)) return true;
        if (err) *err = QString("cannot remove %1").arg(path);
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QString("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(QJsonDocument(toJson(kit)).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = QString("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

std::unique_ptr<QLockFile> lockGlobal(int timeoutMs)
{
    const QString path = globalPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto l = std::make_unique<QLockFile>(path + ".lock");
    l->setStaleLockTime(0);
    if (!l->tryLock(timeoutMs)) return nullptr;
    return l;
}

BrandKit effective(const std::optional<BrandKit>& projectBrand) { return projectBrand ? *projectBrand : global(); }

BrandKit effective(const ProjectData& project) { return effective(project.brand); }

bool resolve(TitleStyle& s, const BrandKit& kit)
{
    const TitleStyle before = s;
    if (const auto c = kit.color(s.colorRef)) s.color = *c;
    if (const auto c = kit.color(s.outlineRef)) s.outlineColor = *c;
    if (const auto c = kit.color(s.boxRef)) {
        // The box keeps its own transparency (a brand colour is usually opaque; the default box is see-through)
        QColor box = *c;
        if (box.alpha() == 255) box.setAlpha(s.boxColor.alpha());
        s.boxColor = box;
    }
    if (const auto f = kit.font(s.fontRef)) s.font = *f;
    return !(s == before);
}

bool resolve(Clip& clip, const BrandKit& kit)
{
    bool changed = false;
    if (kit.color(clip.title.colorRef) && Keys::animated(clip, AnimParam::TitleColor)) {
        Keys::clear(clip, AnimParam::TitleColor);
        changed = true;
    }
    return resolve(clip.title, kit) || changed;
}

} // namespace Brand
