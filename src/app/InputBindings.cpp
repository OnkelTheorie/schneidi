#include "app/InputBindings.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

constexpr int kVersion = 2; // erhöhen, wenn sich Defaults ändern

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

// Namen in keybindings.json ("back" = hintere Seitentaste, "forward" = vordere)
const QHash<QString, Qt::MouseButton>& mouseNames()
{
    static const QHash<QString, Qt::MouseButton> names = {
        {"back", Qt::BackButton},       {"forward", Qt::ForwardButton}, {"task", Qt::TaskButton},
        {"extra4", Qt::ExtraButton4},   {"extra5", Qt::ExtraButton5},
    };
    return names;
}

QHash<int, QString> defaultMouse()
{
    return {{Qt::BackButton, "split"}};
}

} // namespace

InputBindings& InputBindings::instance()
{
    static InputBindings inst;
    return inst;
}

InputBindings::InputBindings()
{
    // Standard wie DaVinci Resolve. (KWin kann Alt+Mausrad abfangen -> in der Datei umbelegbar)
    m_wheel = {
        {"none", WheelAction::ScrollVertical},
        {"ctrl", WheelAction::ScrollHorizontal},
        {"alt", WheelAction::Zoom},
        {"shift", WheelAction::TrackHeight},
    };
    m_mouse = defaultMouse();
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

    // Ältere Datei mit überholten Defaults -> Mausrad-Defaults neu schreiben, Tasten behalten
    const bool outdated = root.value("version").toInt() < kVersion;
    if (outdated) m_fileExisted = false;

    const QJsonObject wheel = root.value("wheel").toObject();
    for (auto it = wheel.begin(); it != wheel.end() && !outdated; ++it)
        m_wheel[it.key().toLower()] = wheelNames().value(it.value().toString(), WheelAction::None);

    // Fehlt der Abschnitt (ältere Datei), bleiben die Defaults und er wird ergänzt
    if (root.contains("mouse")) {
        m_mouse.clear();
        const QJsonObject mouse = root.value("mouse").toObject();
        for (auto it = mouse.begin(); it != mouse.end(); ++it)
            if (mouseNames().contains(it.key().toLower()) && !it.value().toString().isEmpty())
                m_mouse[mouseNames().value(it.key().toLower())] = it.value().toString();
    } else {
        m_fileExisted = false;
    }

    const QJsonObject keys = root.value("shortcuts").toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it)
        m_shortcuts[it.key()] = it.value().toString();
}

void InputBindings::registerAction(QAction* action, const QString& id, const QString& category,
                                   const QKeySequence& defaultKey)
{
    m_entries.append({id, category, action->text(), defaultKey, action});
    action->setShortcut(current(id));
}

QKeySequence InputBindings::current(const QString& id) const
{
    if (m_shortcuts.contains(id)) return QKeySequence(m_shortcuts.value(id));
    for (const Entry& e : m_entries)
        if (e.id == id) return e.defaultKey;
    return {};
}

void InputBindings::setShortcut(const QString& id, const QKeySequence& key)
{
    m_shortcuts[id] = key.toString(QKeySequence::PortableText);
    for (const Entry& e : m_entries)
        if (e.id == id && e.action) e.action->setShortcut(key);
    save();
}

void InputBindings::resetAll()
{
    m_shortcuts.clear();
    m_mouse = defaultMouse();
    for (const Entry& e : m_entries)
        if (e.action) e.action->setShortcut(e.defaultKey);
    save();
}

void InputBindings::saveIfIncomplete()
{
    bool complete = m_fileExisted;
    for (const Entry& e : m_entries) complete &= m_shortcuts.contains(e.id);
    if (!complete) save();
}

void InputBindings::save()
{
    QJsonObject wheel;
    for (auto it = m_wheel.begin(); it != m_wheel.end(); ++it) wheel[it.key()] = wheelName(it.value());
    QJsonObject keys;
    for (auto it = m_shortcuts.begin(); it != m_shortcuts.end(); ++it) keys[it.key()] = it.value(); // auch unbekannte behalten
    for (const Entry& e : m_entries) {
        keys[e.id] = current(e.id).toString(QKeySequence::PortableText);
        m_shortcuts[e.id] = keys[e.id].toString();
    }
    QJsonObject mouse;
    for (auto it = mouseNames().begin(); it != mouseNames().end(); ++it) mouse[it.key()] = m_mouse.value(it.value());
    QJsonObject root{{"version", kVersion}, {"wheel", wheel}, {"mouse", mouse}, {"shortcuts", keys}};

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

const QVector<Qt::MouseButton>& InputBindings::bindableMouseButtons()
{
    static const QVector<Qt::MouseButton> buttons = {Qt::BackButton, Qt::ForwardButton, Qt::TaskButton,
                                                     Qt::ExtraButton4, Qt::ExtraButton5};
    return buttons;
}

QString InputBindings::mouseButtonText(Qt::MouseButton b)
{
    switch (b) {
    case Qt::BackButton: return "Seitentaste hinten";
    case Qt::ForwardButton: return "Seitentaste vorne";
    case Qt::TaskButton: return "Maustaste 6";
    case Qt::ExtraButton4: return "Maustaste 7";
    case Qt::ExtraButton5: return "Maustaste 8";
    default: return {};
    }
}

QString InputBindings::mouseAction(Qt::MouseButton b) const
{
    return m_mouse.value(b);
}

Qt::MouseButton InputBindings::mouseButtonFor(const QString& id) const
{
    for (auto it = m_mouse.begin(); it != m_mouse.end(); ++it)
        if (it.value() == id) return Qt::MouseButton(it.key());
    return Qt::NoButton;
}

Qt::MouseButton InputBindings::defaultMouseButtonFor(const QString& id) const
{
    const QHash<int, QString> d = defaultMouse();
    for (auto it = d.begin(); it != d.end(); ++it)
        if (it.value() == id) return Qt::MouseButton(it.key());
    return Qt::NoButton;
}

void InputBindings::setMouseButton(const QString& id, Qt::MouseButton b)
{
    // Pro Aktion eine Maustaste, pro Maustaste eine Aktion
    for (auto it = m_mouse.begin(); it != m_mouse.end();)
        it = it.value() == id ? m_mouse.erase(it) : std::next(it);
    if (b != Qt::NoButton) m_mouse[b] = id;
    save();
}

QAction* InputBindings::action(const QString& id) const
{
    for (const Entry& e : m_entries)
        if (e.id == id) return e.action;
    return nullptr;
}
