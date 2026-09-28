#pragma once
// Tasten- und Mausrad-Belegung, anpassbar über keybindings.json im Config-Ordner
// (Linux: ~/.config/schneidi/, Windows: %APPDATA%/schneidi/).
// Defaults orientieren sich an DaVinci Resolve.

#include <QHash>
#include <QKeySequence>
#include <QString>

enum class WheelAction { None, ScrollVertical, ScrollHorizontal, Zoom, TrackHeight };

class InputBindings {
public:
    static InputBindings& instance();

    void load();
    // Aktionen melden ihren Default; fehlt die Datei, wird sie danach komplett geschrieben
    QKeySequence shortcut(const QString& actionId, const QKeySequence& fallback);
    void saveIfMissing();

    WheelAction wheelAction(Qt::KeyboardModifiers mods) const;
    QString filePath() const;

private:
    InputBindings();
    static QString modifierKey(Qt::KeyboardModifiers mods);

    QHash<QString, QString> m_shortcuts;       // actionId -> "Ctrl+B"
    QHash<QString, QString> m_defaults;        // zum Schreiben der Datei
    QHash<QString, WheelAction> m_wheel;       // "none"/"shift"/"ctrl+alt" -> Aktion
    bool m_fileExisted = false;
};
