#pragma once
#include "core/Types.h"

#include <QWidget>

class QLineEdit;
class QListWidget;
class QTreeWidget;

// Effects Library (Edit-Page wie DaVinci): links Kategorien (Toolbox → Video Transitions, Audio Transitions,
// Titles; Open FX → Filters), rechts die Einträge mit Vorschausymbol. Ziehen auf einen Schnitt/eine Clipkante legt
// den Übergang an, "Text" landet als Titel auf der Videospur, ein Filter auf einem Videoclip hängt den Effekt an;
// Doppelklick wendet den Eintrag am Playhead/auf die Auswahl an.
class EffectsLibrary : public QWidget {
    Q_OBJECT
public:
    // Drag-Daten eines Übergangs: "video:<Art-ID>" bzw. "audio:<Kurven-ID>" (Titel nutzen MediaPool::MimeType)
    static constexpr const char* MimeType = "application/x-schneidi-transition";
    static bool parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style);
    // Drag-Daten eines Filters (Open FX): id aus der EffectRegistry
    static constexpr const char* EffectMimeType = "application/x-schneidi-effect";

    explicit EffectsLibrary(QWidget* parent = nullptr);

signals:
    void transitionRequested(TrackKind kind, const TransitionStyle& style);
    void titleRequested();
    void effectRequested(const QString& effectId);

private:
    void rebuild();

    QTreeWidget* m_categories;
    QLineEdit* m_search;
    QListWidget* m_list;
};
