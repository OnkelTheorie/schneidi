#pragma once
// Key and mouse wheel bindings, customizable via keybindings.json in the config folder
// (Linux: ~/.config/schneidi/, Windows: %APPDATA%/schneidi/).
// Defaults follow DaVinci Resolve.

#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QPointer>
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
    // Sets one slot; the same input in the other slot is cleared, a mouse button is taken from other actions
    // (one action per mouse button)
    void setInput(const QString& id, int slot, const QString& input);
    void setInputs(const QString& id, const QStringList& inputs);
    void resetAll();
    // A whole set of keys, mouse buttons and wheel (same format as keybindings.json), e.g. to take it to another
    // computer or keep several: export writes the current set, import replaces it (actions missing in the file get
    // their default) and takes effect immediately
    bool exportSet(const QString& path) const;
    bool importSet(const QString& path, QString* error);
    // Built-in sets: "standard" (like DaVinci Resolve, the defaults) and "beginner" (few, telling keys: S splits,
    // Ctrl+1-4 switch pages, no single-letter keys for special tools; Ctrl+wheel zooms). Replaces the current set
    // including mouse buttons and wheel, takes effect immediately and is saved
    static QStringList presetIds();
    static QString presetName(const QString& id);
    void applyPreset(const QString& id);

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
    // The action among `actions` whose keys match the key event, else nullptr
    static QAction* matching(const QList<QAction*>& actions, const QKeyEvent* event);

private:
    InputBindings();
    static QString modifierKey(Qt::KeyboardModifiers mods);

    void save();
    void apply(const Entry& e) const; // keys of the action's slots -> QAction
    void rebuildMouse();
    QJsonObject toJson() const;
    void applyJson(const QJsonObject& root, bool withWheel); // shortcuts, mouse (and wheel) from a file

    QHash<QString, QStringList> m_inputs;      // actionId -> slots (from the file or changed; else the defaults)
    QVector<Entry> m_entries;                  // registered actions in menu order
    QHash<int, QString> m_mouse;               // Qt::MouseButton -> actionId (cache of the slots)
    QHash<QString, WheelAction> m_wheel;       // "none"/"shift"/"ctrl+alt" -> action
    bool m_fileExisted = false;
};
