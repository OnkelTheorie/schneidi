// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Key and mouse wheel bindings, customizable via keybindings.json in the config folder
// (Linux: ~/.config/schneidi/, Windows: %APPDATA%/schneidi/).
// Defaults follow DaVinci Resolve; built-in presets for people coming from Premiere Pro and Final Cut Pro.

#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QAction;
class QKeyEvent;
class QWidget;

enum class WheelAction { None, ScrollVertical, ScrollHorizontal, Zoom, TrackHeight };

class InputBindings {
public:
    static InputBindings& instance();

    struct Entry {
        QString id;       // key in keybindings.json
        QString category; // e.g. menu name
        QString text;
        QKeySequence defaultKey;
        QPointer<QAction> action;
        // Empty = the whole window. Otherwise the panel the keys belong to (e.g. "transcript", "render_queue", a Media
        // Pool list): they only act while it has the focus and win there over the window's keys, so they only clash
        // with keys of the same scope
        QString scope = {};
        QKeySequence defaultKey2 = {}; // default of the second slot
        QPointer<QWidget> scopeWidget = {}; // registerScopedAction: the widget it acts in
    };

    void load();
    // Register an action: gets the key from the file resp. its default. The same id again (a new window) replaces
    // the old entry.
    void registerAction(QAction* action, const QString& id, const QString& category, const QKeySequence& defaultKey,
                        const QString& scope = {}, const QKeySequence& defaultKey2 = {});
    // After registering all actions: write the file if it is missing or entries are missing
    void saveIfIncomplete();

    // Every action has two slots (like "Key 1"/"Key 2"), each holds a key or a mouse button: an input string is
    // a key in PortableText ("Ctrl+B"), "mouse:back" etc. for a mouse button, or empty.
    // For the key bindings dialog: changes take effect immediately and are saved
    static constexpr int kSlots = 2;
    const QVector<Entry>& entries() const { return m_entries; }
    QStringList inputs(const QString& id) const;        // always kSlots entries
    QStringList defaultInputs(const QString& id) const; // always kSlots entries
    QKeySequence current(const QString& id) const;      // first key of the action (empty if none)
    // An input taken away from another action (setInput/setInputs, removeDuplicates)
    struct Taken {
        QString id;    // the action that lost it
        QString input; // the input it lost
    };
    // Sets one slot; the same input in the other slot is cleared. A key is taken from the other actions of the same
    // scope (two actions on one key would both stop working), a mouse button from every other action (one action per
    // mouse button). Returns what was taken from whom (for the dialog's notice)
    QVector<Taken> setInput(const QString& id, int slot, const QString& input);
    QVector<Taken> setInputs(const QString& id, const QStringList& inputs);
    // One action per key and scope, one per mouse button: a key two registered actions have (a hand-edited or imported
    // file) stays with the action that has it from the file over one that has it as a default, among those with the
    // first in menu order; the others lose it (logged). Changes the set only, not the actions (callers apply it). Done
    // by load() once actions are registered, saveIfIncomplete(), importSet() and loading an own preset
    QVector<Taken> removeDuplicates();
    // Two actions that share their keys on purpose: only one of them is enabled at a time (multicam angle N and video
    // target track N, Alt+N)
    static bool sharedOnPurpose(const QString& a, const QString& b);
    void resetAll();
    // A whole set of keys, mouse buttons and wheel (same format as keybindings.json), e.g. to take it to another
    // computer or keep several: export writes the current set, import replaces it (actions missing in the file get
    // their default) and takes effect immediately
    bool exportSet(const QString& path) const;
    bool importSet(const QString& path, QString* error);
    // Built-in sets: "standard" (like DaVinci Resolve, the defaults), "beginner" (few, telling keys: S splits,
    // Ctrl+1-4 switch pages, no single-letter keys for special tools; Ctrl+wheel zooms), "premiere" and "finalcut"
    // (the well-known default keys of those programs on schneidi's actions, Cmd = Ctrl). Plus the user's own sets
    // ("custom:<name>", e.g. an imported Premiere .kys file) from the keypresets folder. Replaces the current set
    // including mouse buttons and wheel, takes effect immediately and is saved
    static QStringList presetIds();
    static QString presetName(const QString& id);
    void applyPreset(const QString& id);
    // The inputs of every registered action in a preset (for the dialog: what is "own", reset one row)
    QHash<QString, QStringList> presetInputs(const QString& id) const;
    // The preset the current set came from (empty if unknown, e.g. a file from before 0.20)
    QString currentPreset() const { return m_preset; }
    // Folder of the user's own sets (custom:<name> = <folder>/<name>.json)
    static QString customPresetDir();

    // A Premiere Pro keyboard shortcut file (.kys, XML): its commands mapped onto schneidi's actions
    struct KysResult {
        QHash<QString, QStringList> keys; // action id -> keys (PortableText), at most kSlots
        int taken = 0;                    // commands with a key that schneidi has
        int unknown = 0;                  // commands with a key that schneidi lacks (ignored)
        int dropped = 0;                  // keys left out: another panel's (context) or one a command has already
        QStringList notTaken;             // action ids whose key in the file schneidi cannot read (Esc, Ins...)
        bool ok = false;                  // a readable shortcut file
    };
    static KysResult readKys(const QByteArray& xml);
    // Imports a .kys file as a custom preset (the Premiere set plus the file's keys), applies and saves it. Returns the
    // preset id, empty on error (then `error` says why)
    QString importKys(const QString& path, KysResult* result, QString* error);

    // Extra mouse buttons (side buttons) -> action, e.g. rear side button = cut at the playhead
    static const QVector<Qt::MouseButton>& bindableMouseButtons();
    static QString mouseButtonText(Qt::MouseButton b);
    static QString mouseInput(Qt::MouseButton b);           // "mouse:back", empty if not bindable
    static Qt::MouseButton inputMouseButton(const QString& input); // Qt::NoButton for a key
    static QString inputText(const QString& input);         // for display: native key text or mouse button name
    QString mouseAction(Qt::MouseButton b) const;           // action ID or empty
    QAction* action(const QString& id) const;

    WheelAction wheelAction(Qt::KeyboardModifiers mods) const;
    QString filePath() const;

    // An action that only acts inside `scope` (e.g. Delete in the Media Pool's bin list): its keys work while that
    // widget has the focus and win there over window keys with the same input (Delete there never deletes timeline
    // clips); a mouse button bound to it works over that widget. Never put such an action into a menu: that makes its
    // keys global.
    void registerScopedAction(QAction* action, const QString& id, const QString& category,
                              const QKeySequence& defaultKey, QWidget* scope, const QKeySequence& defaultKey2 = {});
    // Register an action of one panel (scope, e.g. "render_queue"): its keys only count while that panel has the focus
    // and win there over the window's (Del deletes render jobs there, clips elsewhere). The panel checks them itself
    // with matches() in its ShortcutOverride and KeyPress events; the action is not added to a widget (Qt would call the
    // same key in the window ambiguous and do nothing). A mouse button bound to it only works over its parent widget.
    void registerPanelAction(QAction* action, const QString& id, const QString& category, const QString& scope,
                             const QKeySequence& defaultKey, const QKeySequence& defaultKey2 = {});
    // The keys of these window actions (whatever they are bound to) do nothing while the focus is inside `panel`: a
    // panel with items of its own (Media Pool clips, Effects, Media Storage files), where Delete must not delete the
    // timeline clips the user does not look at. Keys a child takes itself (text fields, scoped actions) stay its own.
    void shieldWindowKeys(QWidget* panel, const QStringList& ids);
    QString scope(const QString& id) const;    // empty = window-wide
    QWidget* scopeOf(const QString& id) const; // the widget a scoped/panel action acts in; nullptr = whole window
    // Is the key of this event one of the action's keys (enabled actions only)? A key of the number pad also counts
    // as the same key without the pad (Enter, Delete, digits)
    static bool matches(const QAction* action, const QKeyEvent* e);
    // The key of a key press as a binding (the key bindings dialog). A letter of a non-Latin layout (Russian, Greek...)
    // becomes the Latin key of the same physical key - the one shortcuts match there; on Windows, AltGr typing a
    // character arrives as Ctrl+Alt, which is dropped. Shift+Tab arrives as Backtab: Tab.
    // `native` = how to read the event's native codes (the running platform's; tests pass one)
    enum class NativeKeys { Windows, Xkb, None };
    static NativeKeys platformNativeKeys();
    static QKeyCombination bindingKey(const QKeyEvent* e, NativeKeys native = platformNativeKeys());
    // Keys the main window uses itself while a mode is on (Esc ends the large viewer and a voice-over take): bound to a
    // window action they clash there (the key bindings dialog marks them)
    static QStringList windowInternalKeys();
    // While a mode of the main window uses `key` itself (`owner` "large_viewer"/"voiceover": Esc ends it), window
    // actions bound to it give it up on their QAction - two enabled Esc shortcuts are ambiguous and neither fires, the
    // mode wins. The bindings stay; the actions get the key back once no owner holds it
    void holdWindowKey(const QString& owner, const QString& key, bool on);
    // The action among `actions` whose keys match the key event, else nullptr
    static QAction* matching(const QList<QAction*>& actions, const QKeyEvent* event);

private:
    InputBindings();
    static QString modifierKey(Qt::KeyboardModifiers mods);

    void save();
    void apply(const Entry& e) const; // keys of the action's slots -> QAction
    void applyAll();                  // every registered action + the mouse buttons
    void rebuildMouse();
    QJsonObject toJson() const;
    void applyJson(const QJsonObject& root, bool withWheel); // shortcuts, mouse (and wheel) from a file
    // Put `keys` (action id -> "Key" or "Key1|Key2") on top of the current set: a single key replaces the first slot,
    // the second keeps its default. Every other action loses a key one of them now uses in the same scope (schneidi's
    // own actions keep their key only while it is free)
    void loadPreset(const QString& id); // m_inputs, m_wheel and m_preset of a preset (applyPreset without applying)
    void overlay(const QHash<QString, QStringList>& keys);

    QHash<QString, QStringList> m_inputs;      // actionId -> slots (from the file or changed; else the defaults)
    QVector<Entry> m_entries;                  // registered actions in menu order
    QHash<int, QString> m_mouse;               // Qt::MouseButton -> actionId (cache of the slots)
    QHash<QString, WheelAction> m_wheel;       // "none"/"shift"/"ctrl+alt" -> action
    bool m_fileExisted = false;
    QString m_preset;                          // see currentPreset()
    QHash<QString, QSet<QString>> m_held;      // holdWindowKey: key -> owners
};
