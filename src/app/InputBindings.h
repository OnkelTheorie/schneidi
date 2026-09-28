#pragma once
// Tasten- und Mausrad-Belegung, anpassbar über keybindings.json im Config-Ordner
// (Linux: ~/.config/schneidi/, Windows: %APPDATA%/schneidi/).
// Defaults orientieren sich an DaVinci Resolve.

#include <QHash>
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
        QString id;       // Schlüssel in keybindings.json
        QString category; // z. B. Menüname
        QString text;
        QKeySequence defaultKey;
        QPointer<QAction> action;
    };

    void load();
    // Aktion anmelden: bekommt die Taste aus der Datei bzw. ihren Default
    void registerAction(QAction* action, const QString& id, const QString& category, const QKeySequence& defaultKey);
    // Nach dem Anmelden aller Aktionen: Datei schreiben, falls sie fehlt oder Einträge fehlen
    void saveIfIncomplete();

    // Für den Tastenbelegungs-Dialog: Änderungen wirken sofort und werden gespeichert
    const QVector<Entry>& entries() const { return m_entries; }
    QKeySequence current(const QString& id) const;
    void setShortcut(const QString& id, const QKeySequence& key);
    void resetAll();

    WheelAction wheelAction(Qt::KeyboardModifiers mods) const;
    QString filePath() const;

private:
    InputBindings();
    static QString modifierKey(Qt::KeyboardModifiers mods);

    void save();

    QHash<QString, QString> m_shortcuts;       // actionId -> "Ctrl+B" (aus der Datei)
    QVector<Entry> m_entries;                  // angemeldete Aktionen in Menü-Reihenfolge
    QHash<QString, WheelAction> m_wheel;       // "none"/"shift"/"ctrl+alt" -> Aktion
    bool m_fileExisted = false;
};
