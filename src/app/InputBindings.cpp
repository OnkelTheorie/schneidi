#include "app/InputBindings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

const QHash<QString, WheelAction>& wheelNames()
{
    static const QHash<QString, WheelAction> names = {
        {"none", WheelAction::None},
        {"scroll_vertical", WheelAction::ScrollVertical},
        {"scroll_horizontal", WheelAction::ScrollHorizontal},
        {"zoom", WheelAction::Zoom},
        {"track_height", WheelAction::TrackHeight},
    };
    return names;
}

QString wheelName(WheelAction a)
{
    return wheelNames().key(a, "none");
}

} // namespace

InputBindings& InputBindings::instance()
{
    static InputBindings inst;
    return inst;
}

InputBindings::InputBindings()
{
    // Standard: wie vermutet aus DaVinci. Ctrl als Alternative zu Alt, weil KWin
    // Alt+Maus unter KDE teils selbst abfängt.
    m_wheel = {
        {"none", WheelAction::ScrollVertical},
        {"shift", WheelAction::ScrollHorizontal},
        {"alt", WheelAction::Zoom},
        {"ctrl", WheelAction::Zoom},
        {"ctrl+shift", WheelAction::TrackHeight},
    };
}

QString InputBindings::filePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath("keybindings.json");
}

void InputBindings::load()
{
    QFile f(filePath());
    m_fileExisted = f.exists();
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    const QJsonObject wheel = root.value("wheel").toObject();
    for (auto it = wheel.begin(); it != wheel.end(); ++it)
        m_wheel[it.key().toLower()] = wheelNames().value(it.value().toString(), WheelAction::None);

    const QJsonObject keys = root.value("shortcuts").toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it)
        m_shortcuts[it.key()] = it.value().toString();
}

QKeySequence InputBindings::shortcut(const QString& actionId, const QKeySequence& fallback)
{
    m_defaults[actionId] = fallback.toString();
    if (m_shortcuts.contains(actionId)) return QKeySequence(m_shortcuts.value(actionId));
    return fallback;
}

void InputBindings::saveIfMissing()
{
    if (m_fileExisted) return;
    QJsonObject wheel;
    for (auto it = m_wheel.begin(); it != m_wheel.end(); ++it) wheel[it.key()] = wheelName(it.value());
    QJsonObject keys;
    for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it) keys[it.key()] = it.value();
    QJsonObject root{{"wheel", wheel}, {"shortcuts", keys}};

    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QFile f(filePath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(root).toJson());
    m_fileExisted = true;
}

QString InputBindings::modifierKey(Qt::KeyboardModifiers mods)
{
    QStringList parts;
    if (mods & Qt::ControlModifier) parts << "ctrl";
    if (mods & Qt::ShiftModifier) parts << "shift";
    if (mods & Qt::AltModifier) parts << "alt";
    if (mods & Qt::MetaModifier) parts << "meta";
    return parts.isEmpty() ? "none" : parts.join('+');
}

WheelAction InputBindings::wheelAction(Qt::KeyboardModifiers mods) const
{
    return m_wheel.value(modifierKey(mods), WheelAction::None);
}
