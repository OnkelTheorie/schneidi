#pragma once
// Key and mouse wheel bindings, customizable via keybindings.json in the config folder
// (Linux: ~/.config/schneidi/, Windows: %APPDATA%/schneidi/).
// Defaults follow DaVinci Resolve.

#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QPointer>
#include <QString>
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
    };

    void load();
    // Register an action: gets the key from the file resp. its default
    void registerAction(QAction* action, const QString& id, const QString& category, const QKeySequence& defaultKey);
    // After registering all actions: write the file if it is missing or entries are missing
    void saveIfIncomplete();

    // For the key bindings dialog: changes take effect immediately and are saved
    const QVector<Entry>& entries() const { return m_entries; }
    QKeySequence current(const QString& id) const;
    void setShortcut(const QString& id, const QKeySequence& key);
    void resetAll();
    // A whole set of keys, mouse buttons and wheel (same format as keybindings.json), e.g. to take it to another
    // computer or keep several: export writes the current set, import replaces it (actions missing in the file get
    // their default) and takes effect immediately
    bool exportSet(const QString& path) const;
    bool importSet(const QString& path, QString* error);

    // Extra mouse buttons (side buttons) -> action, e.g. rear side button = cut at the playhead
    static const QVector<Qt::MouseButton>& bindableMouseButtons();
    static QString mouseButtonText(Qt::MouseButton b);
    QString mouseAction(Qt::MouseButton b) const;              // action ID or empty
    Qt::MouseButton mouseButtonFor(const QString& id) const;   // Qt::NoButton if none
    Qt::MouseButton defaultMouseButtonFor(const QString& id) const;
    void setMouseButton(const QString& id, Qt::MouseButton b); // NoButton = remove
    QAction* action(const QString& id) const;

    WheelAction wheelAction(Qt::KeyboardModifiers mods) const;
    QString filePath() const;

private:
    InputBindings();
    static QString modifierKey(Qt::KeyboardModifiers mods);

    void save();
    QJsonObject toJson() const;
    void applyJson(const QJsonObject& root, bool withWheel); // shortcuts, mouse (and wheel) from a file

    QHash<QString, QString> m_shortcuts;       // actionId -> "Ctrl+B" (from the file)
    QVector<Entry> m_entries;                  // registered actions in menu order
    QHash<int, QString> m_mouse;               // Qt::MouseButton -> actionId
    QHash<QString, WheelAction> m_wheel;       // "none"/"shift"/"ctrl+alt" -> action
    bool m_fileExisted = false;
};
