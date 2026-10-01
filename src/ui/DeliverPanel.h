#pragma once
#include "core/RenderJob.h"

#include <QPointer>
#include <QSize>
#include <QWidget>

class Project;
class RenderQueue;
class RenderQueuePanel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QToolButton;

// Deliver-Seite wie DaVinci: links die Render-Einstellungen mit Vorlagen (eingebaut + eigene) und
// „Zur Render-Warteschlange hinzufügen“, rechts die Warteschlange (queuePanel(), vom Hauptfenster platziert).
class DeliverPanel : public QWidget {
    Q_OBJECT
public:
    DeliverPanel(Project* project, QWidget* parent = nullptr);
    ~DeliverPanel() override;

    RenderQueuePanel* queuePanel() const { return m_queuePanel; }
    RenderQueue* renderQueue() const { return m_queue; }

    // Aktuelle Einstellungen (Auflösung als Timeline / kürzere Kante / feste Größe, je nach Wahl)
    RenderSettings settings() const;
    void setSettings(const RenderSettings& s);
    // Auftrag aus den aktuellen Einstellungen (Pfad, Größe, Bereich); ohne Rückfrage
    RenderJob makeJob() const;
    // Mit Rückfrage bei vorhandener Datei / gleichem Ziel in der Warteschlange; false = nicht hinzugefügt
    bool addToQueue(bool confirm = true);

    // Vorlagen (Index in presets(), -1 = eigene Einstellungen): auswählen, eigene speichern/löschen
    int currentPreset() const;
    void selectPreset(int index);
    bool savePreset(const QString& name); // gleicher Name = überschreiben; eingebaute nicht
    bool deletePreset(int index);         // nur eigene
    const QVector<RenderPreset>& presets() const { return m_presets; }

private:
    void applyPreset(int comboIndex);
    void updateRange();
    void updateFormat(); // Auflösungen passend zu den Projekteinstellungen
    void updateControls(); // Felder je nach Format (nur Audio, Qualität)
    void reloadPresets(const QString& select = {});
    void syncPresetToSettings(); // Einstellung geändert -> passende Vorlage oder „Eigene Einstellungen“
    void presetMenu();
    void browse();
    int selectSize(QSize s); // Auflösung wählen (ggf. hinzufügen)

    Project* m_project;
    RenderQueue* m_queue;
    // Liegt woanders im Fenster (kein Kind), hängt aber an m_queue -> stirbt mit diesem Panel
    QPointer<RenderQueuePanel> m_queuePanel;
    QVector<RenderPreset> m_presets; // eingebaute + eigene; Combo-Index = Listenindex + 1
    QComboBox* m_preset;
    QToolButton* m_presetMenu;
    QLineEdit* m_name;
    QLineEdit* m_folder;
    QComboBox* m_format;
    QComboBox* m_resolution; // Daten: QSize
    QLabel* m_rate;
    QSize m_timelineSize; // zuletzt bekannte Timeline-Auflösung
    QComboBox* m_quality;
    QComboBox* m_audioBitrate;
    QComboBox* m_cores; // CPU-Kerne fürs Rendern (Daten: Anzahl, 0 = alle)
    QComboBox* m_range; // ganze Timeline / In-Out-Bereich
    QComboBox* m_subtitles; // Untertitel: keine / einbrennen / SRT-Datei (Daten: RenderSettings::Subtitles)
    bool m_hadRange = false;
    bool m_applying = false;
    QPushButton* m_addBtn;
    QLabel* m_status;
};
