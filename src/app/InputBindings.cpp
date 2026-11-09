// SPDX-License-Identifier: AGPL-3.0-only
#include "app/InputBindings.h"

#include "core/I18n.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QWidget>
#include <QXmlStreamReader>
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


using KeyTable = QHash<QString, QStringList>; // action id -> slots ("" = none); one entry = first slot only

// Premiere Pro's default keys (Windows; on macOS Cmd = Ctrl) on schneidi's actions. What Premiere has no key for keeps
// schneidi's key if it is free (InputBindings::overlay)
const KeyTable& premiereKeys()
{
    static const KeyTable keys = {
        // Edit
        {"split", {"Ctrl+K"}}, // Add Edit
        {"split_on_tracks", {"Ctrl+Shift+K"}}, // Add Edit to All Tracks
        {"ripple_delete", {"Shift+Del"}},
        {"delete", {"Del"}},
        {"delete_alt", {"Backspace"}},
        {"insert_clip", {","}},
        {"overwrite_clip", {"."}},
        {"trim_start", {"Q"}}, // Ripple Trim Previous Edit to Playhead
        {"trim_end", {"W"}},   // Ripple Trim Next Edit to Playhead
        {"nudge_left", {"Alt+Left"}},
        {"nudge_right", {"Alt+Right"}},
        {"nudge_left_multi", {"Alt+Shift+Left"}},
        {"nudge_right_multi", {"Alt+Shift+Right"}},
        {"toggle_enabled", {"Shift+E"}},
        {"add_transition", {"Ctrl+D"}}, // Apply Video Transition
        {"link_clips", {"Ctrl+L"}},
        {"paste_attributes", {"Ctrl+Alt+V"}},
        // Tools
        {"tool_select", {"V"}},
        {"tool_blade", {"C"}}, // Razor
        {"tool_trim", {"B"}},  // Ripple Edit
        {"tool_slip", {"Y"}},
        {"tool_slide", {"U"}},
        {"select_forward", {"A"}}, // Track Select Forward
        {"select_backward", {"Shift+A"}},
        {"snapping", {"S"}},
        // Marks and markers
        {"mark_in", {"I"}},
        {"mark_out", {"O"}},
        {"clear_in", {"Ctrl+Shift+I"}},
        {"clear_out", {"Ctrl+Shift+O"}},
        {"clear_in_out", {"Ctrl+Shift+X"}},
        {"goto_in", {"Shift+I"}},
        {"goto_out", {"Shift+O"}},
        {"add_marker", {"M"}},
        {"marker_next", {"Shift+M"}},
        {"marker_prev", {"Ctrl+Shift+M"}},
        // Playback and view
        {"play_pause", {"Space"}},
        {"shuttle_back", {"J"}},
        {"stop", {"K"}},
        {"shuttle_fwd", {"L"}},
        {"frame_prev", {"Left"}},
        {"frame_next", {"Right"}},
        {"edit_prev", {"Up"}},
        {"edit_next", {"Down"}},
        {"go_start", {"Home"}},
        {"go_end", {"End"}},
        {"zoom_in", {"="}},
        {"zoom_out", {"-"}},
        {"zoom_fit", {"\\"}},
        {"viewer_large", {"`"}},              // Maximize Frame
        {"viewer_full_screen", {"Ctrl+`"}},   // Toggle Full Screen
        {"viewer_second_screen", {"Ctrl+Shift+F"}}, // schneidi's own, kept reachable
        // File
        {"project_new", {"Ctrl+Alt+N"}},
        {"new_timeline", {"Ctrl+N"}}, // New Sequence
        {"new_bin", {"Ctrl+B"}},
        {"page_deliver", {"Ctrl+M"}}, // Export Media
        {"grab_still", {"Ctrl+Shift+E"}}, // Export Frame
    };
    return keys;
}

// Final Cut Pro's default keys (Cmd = Ctrl, Option = Alt)
const KeyTable& finalCutKeys()
{
    static const KeyTable keys = {
        // Edit
        {"split", {"Ctrl+B"}},                // Blade
        {"split_on_tracks", {"Ctrl+Shift+B"}}, // Blade All
        {"ripple_delete", {"Backspace"}},     // Delete in the storyline closes the gap
        {"ripple_delete_alt", {"Del"}},
        {"delete", {"Shift+Backspace"}},      // Replace with Gap
        {"delete_alt", {"Shift+Del"}},
        {"insert_clip", {"W"}},
        {"overwrite_clip", {"D"}},
        {"append_at_end", {"E"}},
        {"place_on_top", {"Q"}}, // Connect
        {"replace_clip", {"Shift+R"}},
        {"trim_start", {"Alt+["}},
        {"trim_end", {"Alt+]"}},
        {"nudge_left", {","}},
        {"nudge_right", {"."}},
        {"nudge_left_multi", {"Shift+,"}},
        {"nudge_right_multi", {"Shift+."}},
        {"toggle_enabled", {"V"}},
        {"add_transition", {"Ctrl+T"}},
        {"paste_attributes", {"Ctrl+Shift+V"}},
        {"clip_speed", {"Ctrl+R"}},        // Retime
        {"clip_speed_reset", {"Ctrl+Alt+R"}}, // Normal Speed
        {"voiceover_record", {"Alt+Shift+A"}},
        // Tools
        {"tool_select", {"A"}},
        {"tool_trim", {"T"}},
        {"tool_blade", {"B"}},
        {"snapping", {"N"}},
        // Marks and markers
        {"mark_in", {"I"}},
        {"mark_out", {"O"}},
        {"clear_in", {"Alt+I"}},
        {"clear_out", {"Alt+O"}},
        {"clear_in_out", {"Alt+X"}},
        {"goto_in", {"Shift+I"}},
        {"goto_out", {"Shift+O"}},
        {"add_marker", {"M"}},
        {"marker_next", {"Ctrl+'"}},
        {"marker_prev", {"Ctrl+;"}},
        // Playback and view
        {"play_pause", {"Space"}},
        {"shuttle_back", {"J"}},
        {"stop", {"K"}},
        {"shuttle_fwd", {"L"}},
        {"frame_prev", {"Left"}},
        {"frame_next", {"Right"}},
        {"edit_prev", {"Up"}},
        {"edit_next", {"Down"}},
        {"go_start", {"Home"}},
        {"go_end", {"End"}},
        {"zoom_in", {"Ctrl+="}},
        {"zoom_out", {"Ctrl+-"}},
        {"zoom_fit", {"Shift+Z"}},
        {"toggle_scopes", {"Ctrl+7"}},
        {"viewer_full_screen", {"Ctrl+Shift+F"}},
        {"viewer_second_screen", {"Ctrl+F12"}}, // A/V Output on/off; Ctrl+Shift+F is full screen in Final Cut
        // File
        {"import", {"Ctrl+I"}},
        {"page_deliver", {"Ctrl+E"}}, // Share
    };
    return keys;
}

// Premiere command names (.kys) -> action ids. Several spellings where versions differ
const QHash<QString, QString>& kysCommands()
{
    static const QHash<QString, QString> commands = {
        {"cmd.file.new.project", "project_new"},
        {"cmd.file.new.sequence", "new_timeline"},
        {"cmd.file.new.bin", "new_bin"},
        {"cmd.file.open", "project_open"},
        {"cmd.file.openproject", "project_open"},
        {"cmd.file.save", "project_save"},
        {"cmd.file.saveas", "project_save_as"},
        {"cmd.file.import", "import"},
        {"cmd.file.export.media", "page_deliver"},
        {"cmd.file.export.frame", "grab_still"},
        {"cmd.app.quit", "quit"},
        {"cmd.file.exit", "quit"},
        {"cmd.edit.undo", "undo"},
        {"cmd.edit.redo", "redo"},
        {"cmd.edit.cut", "cut"},
        {"cmd.edit.copy", "copy"},
        {"cmd.edit.paste", "paste"},
        {"cmd.edit.pasteattributes", "paste_attributes"},
        {"cmd.edit.clear", "delete"},
        {"cmd.edit.rippledelete", "ripple_delete"},
        {"cmd.edit.selectall", "select_all"},
        {"cmd.edit.deselectall", "deselect"},
        {"cmd.edit.keyboardshortcuts", "edit_keybindings"},
        {"cmd.clip.enable", "toggle_enabled"},
        {"cmd.clip.link", "link_clips"},
        {"cmd.clip.speedduration", "clip_speed"},
        {"cmd.sequence.addedit", "split"},
        {"cmd.sequence.razor", "split"},
        {"cmd.sequence.addedittoalltracks", "split_on_tracks"},
        {"cmd.sequence.razoralltracks", "split_on_tracks"},
        {"cmd.sequence.rippletrimprevious", "trim_start"},
        {"cmd.sequence.rippletrimnext", "trim_end"},
        {"cmd.sequence.applyvideotransition", "add_transition"},
        {"cmd.sequence.snap", "snapping"},
        {"cmd.sequence.zoomin", "zoom_in"},
        {"cmd.sequence.zoomout", "zoom_out"},
        {"cmd.sequence.zoomtofit", "zoom_fit"},
        {"cmd.sequence.nudgeleft", "nudge_left"},
        {"cmd.sequence.nudgeright", "nudge_right"},
        {"cmd.sequence.nudgeleftfive", "nudge_left_multi"},
        {"cmd.sequence.nudgerightfive", "nudge_right_multi"},
        {"cmd.source.insert", "insert_clip"},
        {"cmd.source.overwrite", "overwrite_clip"},
        {"cmd.marker.add", "add_marker"},
        {"cmd.marker.next", "marker_next"},
        {"cmd.marker.previous", "marker_prev"},
        {"cmd.marker.gotonext", "marker_next"},
        {"cmd.marker.gotoprevious", "marker_prev"},
        {"cmd.marker.markin", "mark_in"},
        {"cmd.marker.markout", "mark_out"},
        {"cmd.marker.setin", "mark_in"},
        {"cmd.marker.setout", "mark_out"},
        {"cmd.marker.clearin", "clear_in"},
        {"cmd.marker.clearout", "clear_out"},
        {"cmd.marker.clearinout", "clear_in_out"},
        {"cmd.marker.gotoin", "goto_in"},
        {"cmd.marker.gotoout", "goto_out"},
        {"cmd.transport.play", "play_pause"},
        {"cmd.transport.playstop", "play_pause"},
        {"cmd.transport.shuttleleft", "shuttle_back"},
        {"cmd.transport.shuttlestop", "stop"},
        {"cmd.transport.shuttleright", "shuttle_fwd"},
        {"cmd.transport.stepback", "frame_prev"},
        {"cmd.transport.stepforward", "frame_next"},
        {"cmd.transport.gotoprevedit", "edit_prev"},
        {"cmd.transport.gotonextedit", "edit_next"},
        {"cmd.transport.gotostart", "go_start"},
        {"cmd.transport.gotoend", "go_end"},
        {"cmd.tool.selection", "tool_select"},
        {"cmd.tool.razor", "tool_blade"},
        {"cmd.tool.ripple", "tool_trim"},
        {"cmd.tool.slip", "tool_slip"},
        {"cmd.tool.slide", "tool_slide"},
        {"cmd.tool.trackselectforward", "select_forward"},
        {"cmd.tool.trackselectbackward", "select_backward"},
        {"cmd.window.maximizeframe", "viewer_large"},
        {"cmd.window.fullscreen", "viewer_full_screen"},
    };
    return commands;
}

// Windows virtual key code (Premiere stores those, also on macOS) -> key name
QString virtualKeyName(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return QString(QChar(vk));
    if (vk >= 0x70 && vk <= 0x7B) return QString("F%1").arg(vk - 0x70 + 1);       // F1-F12
    if (vk >= 0x60 && vk <= 0x69) return QString(QChar('0' + vk - 0x60));          // number pad digits
    static const QHash<int, QString> names = {
        {0x08, "Backspace"}, {0x09, "Tab"},  {0x0D, "Return"}, {0x1B, "Esc"},  {0x20, "Space"},
        {0x21, "PgUp"},      {0x22, "PgDown"}, {0x23, "End"},  {0x24, "Home"}, {0x25, "Left"},
        {0x26, "Up"},        {0x27, "Right"}, {0x28, "Down"},  {0x2D, "Ins"},  {0x2E, "Del"},
        {0x6A, "*"},         {0x6B, "+"},     {0x6D, "-"},     {0x6E, "."},    {0x6F, "/"},
        {0xBA, ";"},         {0xBB, "="},     {0xBC, ","},     {0xBD, "-"},    {0xBE, "."},
        {0xBF, "/"},         {0xC0, "`"},     {0xDB, "["},     {0xDC, "\\"},   {0xDD, "]"},
        {0xDE, "'"},
    };
    return names.value(vk);
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
    m_preset = root.value("preset").toString();
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
    // Actions that became the second key of another one (0.13 test builds had Backspace in the bin list as an action of
    // its own): its key moves into the free slot, the old id is gone
    static const QHash<QString, QString> merged{{"pool_remove_bin_alt", "pool_remove_bin"}};
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        if (!m_inputs.contains(it.key())) continue;
        const QStringList old = m_inputs.take(it.key());
        if (!m_inputs.contains(it.value())) continue; // not in the file: its defaults have both keys
        QStringList binds = m_inputs.value(it.value());
        for (const QString& in : old) {
            const qsizetype free = binds.indexOf(QString());
            if (!in.isEmpty() && !binds.contains(in) && free >= 0) binds[free] = in;
        }
        m_inputs[it.value()] = binds;
        m_fileExisted = false; // written anew without the old id
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
    m_preset = "standard";
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
    QJsonObject root{{"version", kVersion}, {"wheel", wheel}, {"shortcuts", keys}};
    if (!m_preset.isEmpty()) root["preset"] = m_preset; // optional, older versions ignore it
    return root;
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

QString InputBindings::customPresetDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("keypresets");
}

QStringList InputBindings::presetIds()
{
    QStringList ids{"standard", "beginner", "premiere", "finalcut"};
    for (const QFileInfo& f : QDir(customPresetDir()).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        ids << "custom:" + f.completeBaseName();
    return ids;
}

QString InputBindings::presetName(const QString& id)
{
    if (id == "beginner") return T("Einsteiger");
    if (id == "premiere") return "Premiere Pro";
    if (id == "finalcut") return "Final Cut Pro";
    if (id.startsWith("custom:")) return id.mid(7);
    return T("Standard");
}

void InputBindings::overlay(const QHash<QString, QStringList>& keys)
{
    QSet<QPair<QString, int>> fixed; // (id, slot) set by `keys`
    QSet<QString> taken;             // scope + '\n' + input
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QStringList binds = inputs(it.key());
        for (int slot = 0; slot < kSlots && slot < it->size(); ++slot) {
            binds[slot] = normalizeInput(it->at(slot));
            fixed.insert({it.key(), slot});
            if (!binds[slot].isEmpty()) taken.insert(scope(it.key()) + '\n' + binds[slot]);
        }
        m_inputs[it.key()] = binds;
    }
    // The other actions (and the kept second slots) give up keys the set uses now
    QStringList ids = m_inputs.keys();
    for (const Entry& e : m_entries) ids << e.id;
    ids.removeDuplicates();
    for (const QString& id : ids) {
        QStringList binds = inputs(id);
        bool changed = false;
        for (int slot = 0; slot < kSlots; ++slot)
            if (!fixed.contains({id, slot}) && !binds[slot].isEmpty() && taken.contains(scope(id) + '\n' + binds[slot])) {
                binds[slot].clear();
                changed = true;
            }
        if (changed) m_inputs[id] = binds;
    }
}

void InputBindings::applyPreset(const QString& id)
{
    loadPreset(id);
    for (const Entry& e : m_entries) apply(e);
    rebuildMouse();
    save();
}

QHash<QString, QStringList> InputBindings::presetInputs(const QString& id) const
{
    InputBindings set(*this); // a copy: nothing reaches the actions or the file
    set.loadPreset(id);
    QHash<QString, QStringList> inputs;
    for (const Entry& e : m_entries) inputs[e.id] = set.inputs(e.id);
    return inputs;
}

void InputBindings::loadPreset(const QString& id)
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
    } else if (id == "premiere") {
        overlay(premiereKeys()); // Alt+wheel zooms there too (the default)
    } else if (id == "finalcut") {
        overlay(finalCutKeys());
    } else if (id.startsWith("custom:")) {
        QFile f(QDir(customPresetDir()).filePath(id.mid(7) + ".json"));
        if (f.open(QIODevice::ReadOnly)) applyJson(QJsonDocument::fromJson(f.readAll()).object(), true);
    }
    m_preset = id;
}

InputBindings::KysResult InputBindings::readKys(const QByteArray& xml)
{
    // Premiere writes one element per key, e.g. <item><commandname>cmd.edit.undo</commandname>
    // <virtualkey>90</virtualkey><modifier.ctrl>true</modifier.ctrl></item>. Tolerant: element names in any case,
    // "command"/"key" spellings, macOS modifier names
    KysResult r;
    QSet<QString> taken, unknown;
    QXmlStreamReader x(xml);
    QHash<QString, QString> item; // child element -> text of the open item
    int depth = 0, itemDepth = -1;
    QString root;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            ++depth;
            const QString name = x.name().toString().toLower();
            if (root.isEmpty()) root = name;
            if (itemDepth < 0 && (name == "item" || name == "shortcut")) {
                itemDepth = depth;
                item.clear();
            } else if (itemDepth >= 0 && depth == itemDepth + 1) {
                item[name] = x.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                --depth; // readElementText consumed the end element
            }
        } else if (x.isEndElement()) {
            if (depth == itemDepth) {
                itemDepth = -1;
                const QString command = item.value("commandname", item.value("command")).toLower();
                QString key = item.contains("virtualkey") ? virtualKeyName(item.value("virtualkey").toInt())
                                                          : item.value("key");
                if (command.isEmpty() || key.isEmpty()) {
                    --depth;
                    continue;
                }
                auto on = [&](const char* m) { return item.value(m).compare("true", Qt::CaseInsensitive) == 0; };
                QStringList mods;
                if (on("modifier.ctrl") || on("modifier.command") || on("modifier.cmd")) mods << "Ctrl";
                if (on("modifier.alt") || on("modifier.option")) mods << "Alt";
                if (on("modifier.shift")) mods << "Shift";
                if (on("modifier.control")) mods << "Meta"; // macOS Control (Ctrl there is Cmd)
                mods << key;
                const QString input = normalizeInput(mods.join('+'));
                const QString id = kysCommands().value(command);
                if (input.isEmpty()) {
                    // a key schneidi cannot name: not counted
                } else if (id.isEmpty()) {
                    unknown.insert(command);
                } else {
                    taken.insert(command);
                    QStringList& keys = r.keys[id];
                    if (!keys.contains(input) && keys.size() < kSlots) keys << input;
                }
            }
            --depth;
        }
    }
    r.ok = !x.hasError() && !root.isEmpty() && (root.contains("premiere") || root.contains("shortcut"));
    r.taken = int(taken.size());
    r.unknown = int(unknown.size());
    return r;
}

QString InputBindings::importKys(const QString& path, KysResult* result, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return {};
    }
    // Premiere's own files are well under 1 MB: a huge file is something else, not read into memory
    constexpr qint64 kMaxKysBytes = 5 * 1024 * 1024;
    if (f.size() > kMaxKysBytes) {
        if (error) *error = T("Die Datei ist zu groß für eine Tastenbelegung.");
        return {};
    }
    KysResult r = readKys(f.readAll());
    if (result) *result = r;
    if (!r.ok || r.taken == 0) {
        if (error) *error = T("Keine Premiere-Tastenbelegung in der Datei");
        return {};
    }
    // Premiere's set underneath: what the file leaves out works like in Premiere. A command with one key in the file
    // has only that one (the second slot is cleared). Built on a copy: the current set changes only once the preset
    // file is written
    InputBindings set(*this);
    set.loadPreset("premiere");
    for (QStringList& keys : r.keys)
        while (keys.size() < kSlots) keys << QString();
    set.overlay(r.keys);
    QString name = QFileInfo(path).completeBaseName().simplified();
    name.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_");
    // Windows drops trailing dots and spaces and reserves device names, also before a dot ("con.x.json" -> "con_.x")
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    static const QRegularExpression reserved(R"(^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$)",
                                             QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = reserved.match(name); m.hasMatch()) name.insert(m.capturedEnd(1), '_');
    if (name.isEmpty()) name = "Premiere";
    QDir().mkpath(customPresetDir());
    const QString id = "custom:" + name;
    set.m_preset = id;
    QFile out(QDir(customPresetDir()).filePath(name + ".json"));
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(set.toJson()).toJson()) <= 0) {
        if (error) *error = T("Die Datei lässt sich nicht schreiben.");
        return {};
    }
    out.close();
    m_inputs = set.m_inputs;
    m_wheel = set.m_wheel;
    m_preset = id;
    for (const Entry& e : m_entries) apply(e);
    rebuildMouse();
    save();
    return id;
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

namespace {

// Keys of the scoped actions of one widget: while it has the focus they win over the window's shortcuts
class ScopeKeys : public QObject {
public:
    static constexpr const char* Name = "schneidi_scope_keys";
    explicit ScopeKeys(QWidget* scope) : QObject(scope)
    {
        setObjectName(Name);
        scope->installEventFilter(this);
    }
    QList<QAction*> actions;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::ShortcutOverride && e->type() != QEvent::KeyPress) return QObject::eventFilter(obj, e);
        QAction* a = InputBindings::matching(actions, static_cast<QKeyEvent*>(e));
        if (!a) return QObject::eventFilter(obj, e);
        if (e->type() == QEvent::ShortcutOverride) e->accept(); // the window's shortcut stays quiet, the key comes here
        else a->trigger();
        return true;
    }
};

// Window keys that stay quiet inside one panel (shieldWindowKeys). Key events a child does not take itself come up to
// the panel: the ShortcutOverride is accepted (no window shortcut), the key press that follows ends here.
class ShieldKeys : public QObject {
public:
    ShieldKeys(QWidget* panel, const QStringList& ids) : QObject(panel), m_ids(ids) { panel->installEventFilter(this); }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::ShortcutOverride && e->type() != QEvent::KeyPress) return QObject::eventFilter(obj, e);
        const auto* key = static_cast<QKeyEvent*>(e);
        for (const QString& id : m_ids)
            if (InputBindings::matches(InputBindings::instance().action(id), key)) {
                e->accept();
                return true;
            }
        return QObject::eventFilter(obj, e);
    }

private:
    QStringList m_ids;
};

} // namespace

void InputBindings::shieldWindowKeys(QWidget* panel, const QStringList& ids)
{
    new ShieldKeys(panel, ids);
}

void InputBindings::registerScopedAction(QAction* action, const QString& id, const QString& category,
                                         const QKeySequence& defaultKey, QWidget* scope, const QKeySequence& defaultKey2)
{
    // Scope for conflicts: the widget itself, so keys of different widgets never clash
    registerAction(action, id, category, defaultKey, "widget:" + QString::number(quintptr(scope), 16), defaultKey2);
    for (Entry& e : m_entries)
        if (e.id == id) e.scopeWidget = scope;
    auto* keys = dynamic_cast<ScopeKeys*>(scope->findChild<QObject*>(ScopeKeys::Name, Qt::FindDirectChildrenOnly));
    if (!keys) keys = new ScopeKeys(scope);
    keys->actions << action;
}

void InputBindings::registerPanelAction(QAction* action, const QString& id, const QString& category, const QString& scope,
                                        const QKeySequence& defaultKey, const QKeySequence& defaultKey2)
{
    action->setShortcutContext(Qt::WidgetShortcut); // never fires through Qt (not added to a widget), see matches()
    registerAction(action, id, category, defaultKey, scope, defaultKey2);
}

QString InputBindings::scope(const QString& id) const
{
    for (const Entry& e : m_entries)
        if (e.id == id) return e.scope;
    return {};
}

QWidget* InputBindings::scopeOf(const QString& id) const
{
    for (const Entry& e : m_entries)
        if (e.id == id) {
            if (e.scopeWidget || e.scope.isEmpty() || !e.action) return e.scopeWidget;
            return qobject_cast<QWidget*>(e.action->parent()); // a panel action: the panel that owns it
        }
    return nullptr;
}

bool InputBindings::matches(const QAction* action, const QKeyEvent* e)
{
    if (!action || !action->isEnabled() || !e || e->key() == 0 || e->key() == Qt::Key_unknown) return false;
    const QKeySequence key(e->keyCombination());
    const Qt::KeyboardModifiers mods =
        e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const QKeySequence plain(QKeyCombination(mods, Qt::Key(e->key())));
    for (const QKeySequence& s : action->shortcuts())
        if (s == key || s == plain) return true;
    return false;
}

QAction* InputBindings::matching(const QList<QAction*>& actions, const QKeyEvent* event)
{
    for (QAction* a : actions)
        if (matches(a, event)) return a;
    return nullptr;
}
