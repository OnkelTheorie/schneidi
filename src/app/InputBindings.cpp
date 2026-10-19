#include "app/InputBindings.h"

#include "core/I18n.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

constexpr int kVersion = 2; // increase when defaults change

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

// Names in keybindings.json ("back" = rear side button, "forward" = front one)
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

// Default like DaVinci Resolve. (KWin may intercept Alt+mouse wheel -> remappable in the file)
QHash<QString, WheelAction> defaultWheel()
{
    return {
        {"none", WheelAction::ScrollVertical},
        {"ctrl", WheelAction::ScrollHorizontal},
        {"alt", WheelAction::Zoom},
        {"shift", WheelAction::TrackHeight},
    };
}

// Beginner set: differences to the defaults ("" = no key). Single letters that switch tools or move clips are
// easy to hit by accident, so only playback, marks, blade/select and the common Ctrl keys stay
const QHash<QString, QString>& beginnerKeys()
{
    static const QHash<QString, QString> keys = {
        {"split", "S"},
        {"split_alt", "Ctrl+B"},
        {"page_media", "Ctrl+1"},
        {"page_edit", "Ctrl+2"},
        {"page_color", "Ctrl+3"},
        {"page_deliver", "Ctrl+4"},
        {"select_forward", ""},
        {"select_backward", ""},
        {"select_forward_all", ""},
        {"tool_trim", ""},
        {"select_edit_point", ""},
        {"edit_point_type", ""},
        {"nudge_left", ""},
        {"nudge_right", ""},
        {"nudge_left_multi", ""},
        {"nudge_right_multi", ""},
        {"toggle_enabled", ""},
        {"show_timeline", ""},
        {"grade_prev", ""},
        {"grade_prev2", ""},
    };
    return keys;
}

} // namespace

InputBindings& InputBindings::instance()
{
    static InputBindings inst;
    return inst;
}

InputBindings::InputBindings()
{
    m_wheel = defaultWheel();
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

    // Older file with outdated defaults -> rewrite mouse wheel defaults, keep keys
    const bool outdated = root.value("version").toInt() < kVersion;
    if (outdated) m_fileExisted = false;
    // If the mouse section is missing (older file), the defaults remain and it gets added
    if (!root.contains("mouse")) m_fileExisted = false;
    applyJson(root, !outdated);
}

void InputBindings::applyJson(const QJsonObject& root, bool withWheel)
{
    const QJsonObject wheel = root.value("wheel").toObject();
    for (auto it = wheel.begin(); it != wheel.end() && withWheel; ++it)
        m_wheel[it.key().toLower()] = wheelNames().value(it.value().toString(), WheelAction::None);

    if (root.contains("mouse")) {
        m_mouse.clear();
        const QJsonObject mouse = root.value("mouse").toObject();
        for (auto it = mouse.begin(); it != mouse.end(); ++it)
            if (mouseNames().contains(it.key().toLower()) && !it.value().toString().isEmpty())
                m_mouse[mouseNames().value(it.key().toLower())] = it.value().toString();
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

QJsonObject InputBindings::toJson() const
{
    QJsonObject wheel;
    for (auto it = m_wheel.begin(); it != m_wheel.end(); ++it) wheel[it.key()] = wheelName(it.value());
    QJsonObject keys;
    for (auto it = m_shortcuts.begin(); it != m_shortcuts.end(); ++it) keys[it.key()] = it.value(); // keep unknown ones too
    for (const Entry& e : m_entries) keys[e.id] = current(e.id).toString(QKeySequence::PortableText);
    QJsonObject mouse;
    for (auto it = mouseNames().begin(); it != mouseNames().end(); ++it) mouse[it.key()] = m_mouse.value(it.value());
    return {{"version", kVersion}, {"wheel", wheel}, {"mouse", mouse}, {"shortcuts", keys}};
}

void InputBindings::save()
{
    const QJsonObject root = toJson();
    const QJsonObject keys = root.value("shortcuts").toObject();
    for (const Entry& e : m_entries) m_shortcuts[e.id] = keys.value(e.id).toString();
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QFile f(filePath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(root).toJson());
    m_fileExisted = true;
}

bool InputBindings::exportSet(const QString& path) const
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(toJson()).toJson()) > 0;
}

bool InputBindings::importSet(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.object().value("shortcuts").isObject()) {
        if (error) *error = T("Keine Tastenbelegung in der Datei");
        return false;
    }
    // A whole set: what the file leaves out goes back to the default
    m_shortcuts.clear();
    m_mouse = defaultMouse();
    applyJson(doc.object(), true);
    for (const Entry& e : m_entries)
        if (e.action) e.action->setShortcut(current(e.id));
    save();
    return true;
}

QStringList InputBindings::presetIds()
{
    return {"standard", "beginner"};
}

QString InputBindings::presetName(const QString& id)
{
    if (id == "beginner") return T("Einsteiger");
    return T("Standard (wie DaVinci Resolve)");
}

void InputBindings::applyPreset(const QString& id)
{
    m_shortcuts.clear();
    m_mouse = defaultMouse();
    m_wheel = defaultWheel();
    if (id == "beginner") {
        for (auto it = beginnerKeys().begin(); it != beginnerKeys().end(); ++it) m_shortcuts[it.key()] = it.value();
        // Zoom with Ctrl like most programs, Shift scrolls sideways
        m_wheel = {
            {"none", WheelAction::ScrollVertical},
            {"ctrl", WheelAction::Zoom},
            {"shift", WheelAction::ScrollHorizontal},
            {"alt", WheelAction::TrackHeight},
        };
    }
    for (const Entry& e : m_entries)
        if (e.action) e.action->setShortcut(current(e.id));
    save();
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
    case Qt::BackButton: return T("Seitentaste hinten");
    case Qt::ForwardButton: return T("Seitentaste vorne");
    case Qt::TaskButton: return T("Maustaste 6");
    case Qt::ExtraButton4: return T("Maustaste 7");
    case Qt::ExtraButton5: return T("Maustaste 8");
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
    // One mouse button per action, one action per mouse button
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
