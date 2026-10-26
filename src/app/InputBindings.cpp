#include "app/InputBindings.h"

#include "core/I18n.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <algorithm>

namespace {

constexpr int kVersion = 3; // increase when defaults or the format change (3: two slots per action)

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

// Default mouse buttons: action -> input
QHash<QString, QString> defaultMouse()
{
    return {{"split", "mouse:back"}};
}

// Input string from the file: key in PortableText, "mouse:<name>" or empty
QString normalizeInput(const QString& raw)
{
    const QString s = raw.trimmed();
    if (s.startsWith("mouse:", Qt::CaseInsensitive))
        return mouseNames().contains(s.mid(6).toLower()) ? s.toLower() : QString();
    return QKeySequence::fromString(s, QKeySequence::PortableText).toString(QKeySequence::PortableText);
}

QStringList padded(QStringList list)
{
    while (list.size() < InputBindings::kSlots) list << QString();
    return list.mid(0, InputBindings::kSlots);
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

    // Older file -> rewrite it in the current format; before version 2 also with the new mouse wheel defaults
    const int version = root.value("version").toInt();
    if (version < kVersion) m_fileExisted = false;
    applyJson(root, version >= 2);
}

void InputBindings::applyJson(const QJsonObject& root, bool withWheel)
{
    const QJsonObject wheel = root.value("wheel").toObject();
    for (auto it = wheel.begin(); it != wheel.end() && withWheel; ++it)
        m_wheel[it.key().toLower()] = wheelNames().value(it.value().toString(), WheelAction::None);

    // "split": "B" (one slot) or ["B", "mouse:back"]
    const QJsonObject keys = root.value("shortcuts").toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QStringList binds;
        if (it.value().isArray())
            for (const QJsonValue& v : it.value().toArray()) binds << normalizeInput(v.toString());
        else binds << normalizeInput(it.value().toString());
        m_inputs[it.key()] = padded(binds);
    }

    // Before version 3 mouse buttons had their own section {"back": "split"} (missing = the default)
    if (root.value("version").toInt() < 3) {
        QHash<QString, QString> legacy; // input -> action
        if (root.contains("mouse")) {
            const QJsonObject mouse = root.value("mouse").toObject();
            for (auto it = mouse.begin(); it != mouse.end(); ++it)
                if (mouseNames().contains(it.key().toLower()) && !it.value().toString().isEmpty())
                    legacy["mouse:" + it.key().toLower()] = it.value().toString();
        } else {
            const QHash<QString, QString> defaults = defaultMouse();
            for (auto it = defaults.begin(); it != defaults.end(); ++it) legacy[it.value()] = it.key();
        }
        for (auto it = legacy.begin(); it != legacy.end(); ++it) {
            const QString& id = it.value();
            if (!m_inputs.contains(id) && defaultInputs(id).contains(it.key())) continue;
            QStringList binds = inputs(id);
            if (binds.contains(it.key())) continue;
            const qsizetype free = binds.indexOf(QString());
            binds[free >= 0 ? free : kSlots - 1] = it.key();
            m_inputs[id] = binds;
        }
    }
    rebuildMouse();
}

void InputBindings::registerAction(QAction* action, const QString& id, const QString& category,
                                   const QKeySequence& defaultKey, const QString& scope, const QKeySequence& defaultKey2)
{
    const Entry entry{id, category, action->text(), defaultKey, action, scope, defaultKey2};
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.id == id; });
    if (it != m_entries.end()) *it = entry;
    else m_entries.append(entry);
    apply(entry);
    rebuildMouse();
}

QStringList InputBindings::defaultInputs(const QString& id) const
{
    QStringList binds;
    for (const Entry& e : m_entries)
        if (e.id == id)
            for (const QKeySequence& key : {e.defaultKey, e.defaultKey2})
                if (!key.isEmpty()) binds << key.toString(QKeySequence::PortableText);
    if (defaultMouse().contains(id)) binds << defaultMouse().value(id);
    return padded(binds);
}

QStringList InputBindings::inputs(const QString& id) const
{
    return m_inputs.contains(id) ? padded(m_inputs.value(id)) : defaultInputs(id);
}

QKeySequence InputBindings::current(const QString& id) const
{
    for (const QString& in : inputs(id))
        if (!in.isEmpty() && inputMouseButton(in) == Qt::NoButton)
            return QKeySequence::fromString(in, QKeySequence::PortableText);
    return {};
}

void InputBindings::setInput(const QString& id, int slot, const QString& input)
{
    if (slot < 0 || slot >= kSlots) return;
    QStringList binds = inputs(id);
    const QString in = normalizeInput(input);
    if (!in.isEmpty())
        for (QString& other : binds)
            if (other == in) other.clear();
    binds[slot] = in;
    setInputs(id, binds);
}

void InputBindings::setInputs(const QString& id, const QStringList& list)
{
    QStringList binds;
    for (const QString& in : padded(list)) {
        const QString n = normalizeInput(in);
        binds << (binds.contains(n) ? QString() : n);
    }
    // One action per mouse button: take it from the others
    QStringList ids = m_inputs.keys();
    for (const Entry& e : m_entries) ids << e.id;
    for (const QString& in : binds) {
        if (inputMouseButton(in) == Qt::NoButton) continue;
        for (const QString& other : ids) {
            if (other == id) continue;
            QStringList o = inputs(other);
            if (o.removeAll(in) > 0) m_inputs[other] = padded(o);
        }
    }
    m_inputs[id] = binds;
    for (const Entry& e : m_entries)
        if (e.id == id) apply(e);
    rebuildMouse();
    save();
}

void InputBindings::apply(const Entry& e) const
{
    if (!e.action) return;
    QList<QKeySequence> keys;
    for (const QString& in : inputs(e.id))
        if (!in.isEmpty() && inputMouseButton(in) == Qt::NoButton)
            keys << QKeySequence::fromString(in, QKeySequence::PortableText);
    e.action->setShortcuts(keys);
}

void InputBindings::rebuildMouse()
{
    m_mouse.clear();
    for (const Entry& e : m_entries)
        for (const QString& in : inputs(e.id))
            if (const Qt::MouseButton b = inputMouseButton(in); b != Qt::NoButton && !m_mouse.contains(b))
                m_mouse[b] = e.id;
}

void InputBindings::resetAll()
{
    m_inputs.clear();
    for (const Entry& e : m_entries) apply(e);
    rebuildMouse();
    save();
}

void InputBindings::saveIfIncomplete()
{
    bool complete = m_fileExisted;
    for (const Entry& e : m_entries) complete &= m_inputs.contains(e.id);
    if (!complete) save();
}

QJsonObject InputBindings::toJson() const
{
    QJsonObject wheel;
    for (auto it = m_wheel.begin(); it != m_wheel.end(); ++it) wheel[it.key()] = wheelName(it.value());
    QStringList ids = m_inputs.keys(); // keep unknown ones too
    for (const Entry& e : m_entries) ids << e.id;
    QJsonObject keys;
    for (const QString& id : ids) {
        const QStringList binds = inputs(id);
        if (binds.mid(1).join(QString()).isEmpty()) keys[id] = binds[0]; // one slot: plain string like before
        else keys[id] = QJsonArray::fromStringList(binds);
    }
    return {{"version", kVersion}, {"wheel", wheel}, {"shortcuts", keys}};
}

void InputBindings::save()
{
    for (const Entry& e : m_entries) m_inputs[e.id] = inputs(e.id);
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QFile f(filePath());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(toJson()).toJson());
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
    m_inputs.clear();
    applyJson(doc.object(), true);
    for (const Entry& e : m_entries) apply(e);
    rebuildMouse();
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
    return T("Standard");
}

void InputBindings::applyPreset(const QString& id)
{
    m_inputs.clear();
    m_wheel = defaultWheel();
    if (id == "beginner") {
        // Replaces the first slot, the default side button stays
        for (auto it = beginnerKeys().begin(); it != beginnerKeys().end(); ++it) {
            QStringList binds = defaultInputs(it.key());
            binds[0] = it.value();
            m_inputs[it.key()] = binds;
        }
        // Zoom with Ctrl like most programs, Shift scrolls sideways
        m_wheel = {
            {"none", WheelAction::ScrollVertical},
            {"ctrl", WheelAction::Zoom},
            {"shift", WheelAction::ScrollHorizontal},
            {"alt", WheelAction::TrackHeight},
        };
    }
    for (const Entry& e : m_entries) apply(e);
    rebuildMouse();
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

QString InputBindings::mouseInput(Qt::MouseButton b)
{
    for (auto it = mouseNames().begin(); it != mouseNames().end(); ++it)
        if (it.value() == b) return "mouse:" + it.key();
    return {};
}

Qt::MouseButton InputBindings::inputMouseButton(const QString& input)
{
    if (!input.startsWith("mouse:")) return Qt::NoButton;
    return mouseNames().value(input.mid(6), Qt::NoButton);
}

QString InputBindings::inputText(const QString& input)
{
    if (const Qt::MouseButton b = inputMouseButton(input); b != Qt::NoButton) return mouseButtonText(b);
    return QKeySequence::fromString(input, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

QString InputBindings::mouseAction(Qt::MouseButton b) const
{
    return m_mouse.value(b);
}

QAction* InputBindings::action(const QString& id) const
{
    for (const Entry& e : m_entries)
        if (e.id == id) return e.action;
    return nullptr;
}
