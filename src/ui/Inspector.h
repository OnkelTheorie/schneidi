#pragma once
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QSize>
#include <QWidget>
#include <functional>
#include <optional>

class Editor;
struct EffectDescriptor;
class QButtonGroup;
class QHBoxLayout;
class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QGridLayout;
class QLabel;
class QSlider;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;
class ScrubField;

// Inspector (rechts oben) im Stil von DaVinci: Tabs Video/Audio, aufklappbare Bereiche mit
// rotem An/Aus-Punkt, Zahlenfelder zum Eintippen oder Links/Rechts-Ziehen.
// Änderungen gelten für alle ausgewählten Clips der Art; Ziehen = ein Undo-Schritt.
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Editor* editor, QWidget* parent = nullptr);
    ~Inspector() override;
    void setFrameSize(const QSize& size);

public slots:
    // Playhead (Timeline-Frame): Werte zeigen den interpolierten Wert dort, Keyframes werden dort gesetzt
    void setPlayhead(int frame);
    // Untertitel auswählen und seinen Text zum Tippen fokussieren (Doppelklick in der Timeline)
    void editSubtitle(int cueId);

signals:
    // Keyframe-Pfeile ◀ ▶: zum vorigen/nächsten Keyframe springen
    void seekRequested(int frame);

private:
    using Field = std::function<double&(Clip&)>;
    using Flag = std::function<bool&(Clip&)>;

    struct Param {
        TrackKind kind;
        bool title; // nur Titelclips (Tab "Titel")
        Field field;
        ScrubField* edit;
        QSlider* slider = nullptr;
        double min, max;
        std::optional<AnimParam> anim; // animierbar: Wert kommt aus Keys::valueAt, Ändern setzt ggf. Keyframe
        QString effect;                // Effekt-Parameter: wirkt nur auf ausgewählte Clips mit diesem Effekt
    };
    struct Section {
        QGridLayout* grid;
        TrackKind kind;
        int rows = 0;
        bool title = false;
        QString effect;              // Bereich eines Effekts (Open FX), sonst leer
        QWidget* header = nullptr;   // Kopfzeile und Inhalt (zum Ein-/Ausblenden und Umsortieren)
        QWidget* body = nullptr;
        QHBoxLayout* headerLayout = nullptr;
    };

    // Bereich mit Kopfzeile; `enabled` = roter Punkt (optional)
    // titleOnly: Bereich wirkt nur auf Titelclips; effect: nur auf Clips mit diesem Effekt
    Section addSection(QVBoxLayout* page, TrackKind kind, const QString& title, const std::function<void(Clip&)>& reset,
                       const Flag& enabled = {}, bool titleOnly = false, const QString& effect = {});
    // Bereich eines Effekts aus der Effects Library (nur sichtbar, wenn der Clip ihn hat), mit Papierkorb
    void addEffectSection(QVBoxLayout* page, const EffectDescriptor& d);
    // Effect this computer does not know (plugin missing): warning instead of controls, can be removed
    void addMissingEffectSection(QVBoxLayout* page, const QString& id);
    // Trash button in the header + right-click menu "remove effect"
    void addRemoveButton(const Section& s, const QString& id);
    // Zeile mit Schieberegler + Zahlenfeld
    // anim: animierbarer Parameter -> Keyframe-Rauten rechts (field bleibt für nicht animierbare Werte)
    Param* addSlider(Section& s, const QString& key, const QString& label, double min, double max, double def,
                     double step, int decimals, const Field& field, std::optional<AnimParam> anim = {});
    // Zeile mit X/Y-Feldern (wie Zoom/Position in DaVinci); link = Kettensymbol dazwischen
    QPair<Param*, Param*> addXY(Section& s, const QString& key, const QString& label, double min, double max,
                                double def, double step, int decimals, const Field& x, const Field& y,
                                QToolButton* link = nullptr, std::optional<AnimParam> ax = {},
                                std::optional<AnimParam> ay = {});
    Param* makeParam(const Section& s, const QString& key, const QString& text, double min, double max, double def,
                     double step, int decimals, const Field& field, std::optional<AnimParam> anim = {});
    // Keyframe-Knöpfe wie DaVinci: ◀ Raute ▶ (Raute rot = Keyframe am Playhead; Klick setzt/entfernt,
    // Rechtsklick = Verlauf); gilt für alle Parameter der Zeile
    QWidget* keyButtons(TrackKind kind, bool title, const QVector<AnimParam>& params, const QString& effect = {});
    // Clip-Frame unter dem Playhead (auf den Clip begrenzt)
    int localFrame(const Clip& c) const;
    QToolButton* resetButton(const std::function<void()>& fn);
    QLabel* rowLabel(const QString& text);

    // Zeile mit Farbfeld (QColorDialog); alpha = Deckkraft wählbar, also = zusätzlich beim Übernehmen
    // anim: animierbare Farbe (Titelfarbe)
    void addColor(Section& s, const QString& label, const QString& text, const std::function<QColor&(Clip&)>& color,
                  bool alpha, const std::function<void(Clip&)>& also = {}, std::optional<AnimParam> anim = {});
    void buildTitlePage(QVBoxLayout* page);
    void buildTransitionPage(QVBoxLayout* page);
    // Ausgewählter Übergang (Spurart + wirksame Lage), sonst false
    bool selectedTransition(TrackKind* kind, TimelineOps::TransitionSpan* span) const;
    bool refreshTransition(); // true = Übergang ausgewählt und angezeigt

    QVector<int> selectedIds(TrackKind kind, bool title = false, const QString& effect = {}) const;
    const Clip* primary(TrackKind kind, bool title = false, const QString& effect = {}) const;
    void apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn,
               bool title = false, const QString& effect = {});
    // Effekt-Bereiche in der Reihenfolge des angezeigten Clips (= Renderreihenfolge) einsortieren
    void arrangeEffectSections();
    void refresh();

    Editor* m_editor;
    QSize m_frameSize{1920, 1080};
    int m_playhead = 0;
    QLabel* m_clipName;
    QLabel* m_empty;
    QWidget* m_content;
    QStackedWidget* m_pages;
    QButtonGroup* m_tabs;
    QVector<Param*> m_params;
    QVector<std::function<void()>> m_refreshers; // weitere Anzeigen (Punkte, Farbe)
    bool m_zoomLinked = true;
    int m_lastShownId = 0; // Auswahl gewechselt -> bei Titeln den Tab "Titel" zeigen
    Param* m_crop[4] = {};
    // Effekt-Bereiche (Open FX) im Tab Video, ab m_fxIndex im Layout
    struct FxSection { QString id; QWidget* header; QWidget* body; };
    QVector<FxSection> m_fxSections;
    QVBoxLayout* m_videoLay = nullptr;
    int m_fxIndex = 0;
    QStringList m_fxOrder; // zuletzt angezeigte Reihenfolge
    // Tab "Übergang"
    TransitionKey m_transKey;
    QComboBox* m_transType = nullptr;
    QComboBox* m_transAudioType = nullptr; // Audio: Cross Fade +3/0/-3 dB
    QComboBox* m_transAlign = nullptr;
    ScrubField* m_transLen = nullptr;
    QToolButton* m_transColor = nullptr;       // Abblende-Farbe
    ScrubField* m_transSoft = nullptr;         // Wischblende: Weichheit
    ScrubField* m_transBorder = nullptr;       // Wischblende: Randbreite
    QToolButton* m_transBorderColor = nullptr; // Wischblende: Randfarbe
    QComboBox* m_transLuma = nullptr;          // Verlaufsblende: Verlaufsbild (mitgeliefert + Effekte-Ordner)
    class QCheckBox* m_transInvert = nullptr;  // Verlaufsblende: umkehren
    QVector<QWidget*> m_dipRows, m_wipeRows, m_lumaRows, m_softRows; // Zeilen nur für die jeweilige Art
    // Tab "Untertitel": Text/Zeiten des Eintrags und Stil seiner Spur (wie DaVinci Caption + Track Style)
    void buildSubtitlePage(QVBoxLayout* page);
    bool refreshSubtitle(); // true = nur Untertitel ausgewählt und angezeigt
    const SubtitleCue* selectedCue(int* track = nullptr) const;
    void changeSubtitleStyle(const QString& text, const std::function<void(TitleStyle&)>& fn, const QString& mergeKey = {});
    QPlainTextEdit* m_subText = nullptr;
    QVector<std::function<void()>> m_subRefreshers;
    // Stil des ausgewählten Übergangs ändern (mergeKey: Ziehen = ein Undo-Schritt)
    void changeTransition(const std::function<void(TransitionStyle&)>& fn, const QString& mergeKey = {});
};
