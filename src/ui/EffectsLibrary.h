#pragma once
#include "core/Types.h"

#include <QWidget>

class QLineEdit;
class QListWidget;
class QTreeWidget;

// Effects Library (Edit-Page wie DaVinci): links Kategorien (Toolbox → Video Transitions, Audio Transitions,
// Titles), rechts die Einträge mit Vorschausymbol. Ziehen auf einen Schnitt/eine Clipkante legt den Übergang an,
// "Text" landet als Titel auf der Videospur; Doppelklick wendet den Eintrag am Playhead/auf die Auswahl an.
class EffectsLibrary : public QWidget {
    Q_OBJECT
public:
    // Drag-Daten eines Übergangs: "video:<Art-ID>" bzw. "audio:cross_fade" (Titel nutzen MediaPool::MimeType)
    static constexpr const char* MimeType = "application/x-schneidi-transition";
    static bool parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style);

    explicit EffectsLibrary(QWidget* parent = nullptr);

signals:
    void transitionRequested(TrackKind kind, const TransitionStyle& style);
    void titleRequested();

private:
    void rebuild();

    QTreeWidget* m_categories;
    QLineEdit* m_search;
    QListWidget* m_list;
};
