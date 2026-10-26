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
        // Empty = the whole window. Otherwise the panel the keys belong to (e.g. "transcript"): they only act while
        // it has the focus and come first there (the panel matches its action's keys itself), so they only clash
        // with keys of the same scope
        QString scope = {};
        QKeySequence defaultKey2 = {}; // default of the second slot
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
