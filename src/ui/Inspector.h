// SPDX-License-Identifier: AGPL-3.0-only
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

// Inspector (top right) in DaVinci style: Video/Audio tabs, collapsible sections with
// red on/off dot, number fields for typing or left/right dragging.
// Changes apply to all selected clips of that kind; dragging = one undo step.
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Editor* editor, QWidget* parent = nullptr);
    ~Inspector() override;
    void setFrameSize(const QSize& size);

public slots:
    // Playhead (timeline frame): values show the interpolated value there, keyframes are set there
    void setPlayhead(int frame);
    // Select a subtitle and focus its text for typing (double-click in the timeline)
    void editSubtitle(int cueId);
    // Green Screen: key color of the selected video clips (one undo step, switches the key on)
    void applyKeyColor(const QColor& color);
    // Pipette button pressed or not (without keyPickRequested/keyPickCancelled)
    void setKeyPicking(bool on);

protected:
    // Dragging the header of an effect section changes the effect order
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // Keyframe arrows ◀ ▶: jump to the previous/next keyframe
    void seekRequested(int frame);
    // Effect header context menu "save effects as preset" (dialog in MainWindow)
    void savePresetRequested();
    // Green Screen pipette switched on for this clip resp. off again (MainWindow shows the picker in the viewer)
    void keyPickRequested(int clipId);
    void keyPickCancelled();

private:
    using Field = std::function<double&(Clip&)>;
    using Flag = std::function<bool&(Clip&)>;

    struct Param {
        TrackKind kind;
        bool title; // title clips only (tab "Titel")
        Field field;
        ScrubField* edit;
        QSlider* slider = nullptr;
        double min, max;
        std::optional<AnimParam> anim; // animatable: value comes from Keys::valueAt, changing sets a keyframe if needed
        QString effect;                // effect parameter: only acts on selected clips with this effect
    };
    struct Section {
        QGridLayout* grid;
        TrackKind kind;
        int rows = 0;
        bool title = false;
        QString effect;              // section of an effect (Open FX), otherwise empty
        QWidget* header = nullptr;   // header row and content (for showing/hiding and reordering)
        QWidget* body = nullptr;
        QHBoxLayout* headerLayout = nullptr;
        QToolButton* toggle = nullptr; // the title (click = fold)
    };

    // Section with header row; `enabled` = red dot (optional)
    // titleOnly: section only acts on title clips; effect: only on clips with this effect
    Section addSection(QVBoxLayout* page, TrackKind kind, const QString& title, const std::function<void(Clip&)>& reset,
                       const Flag& enabled = {}, bool titleOnly = false, const QString& effect = {});
    // Section of an effect from the Effects Library (only visible if the clip has it), with trash can
    void addEffectSection(QVBoxLayout* page, const EffectDescriptor& d);
    // Effect this computer does not know (plugin missing): warning instead of controls, can be removed
    void addMissingEffectSection(QVBoxLayout* page, const QString& id, TrackKind kind);
    // Stabilization: analysis state with start/cancel button (engine/Stabilizer)
    void addStabilizeRow(Section& s);
    // Trash button in the header + right-click menu "remove effect" (and "move up/down" for a movable effect)
    void addRemoveButton(const Section& s, const QString& id);
    // Effect order: grip on the header of a movable effect; the header (grip, title, empty part) can be dragged to
    // another place among the movable effects. Fixed stages (stabilization) get the same indent without a grip
    void addMoveHandle(const Section& s, const QString& id);
    // Place of `id` among the movable effects of the shown clip of `kind` (-1 = none) and their count
    int effectPlace(TrackKind kind, const QString& id, int* count = nullptr) const;
    // Header drag: follow the mouse (drop line in the gap it would land in), end it (apply = move the effect)
    void dragEffect(const QPoint& globalPos);
    void endEffectDrag(bool apply);
    // Row with slider + number field
    // anim: animatable parameter -> keyframe diamonds on the right (field stays for non-animatable values)
    Param* addSlider(Section& s, const QString& key, const QString& label, double min, double max, double def,
                     double step, int decimals, const Field& field, std::optional<AnimParam> anim = {});
    // Row with X/Y fields (like Zoom/Position in DaVinci); link = chain icon in between
    QPair<Param*, Param*> addXY(Section& s, const QString& key, const QString& label, double min, double max,
                                double def, double step, int decimals, const Field& x, const Field& y,
                                QToolButton* link = nullptr, std::optional<AnimParam> ax = {},
                                std::optional<AnimParam> ay = {});
    Param* makeParam(const Section& s, const QString& key, const QString& text, double min, double max, double def,
                     double step, int decimals, const Field& field, std::optional<AnimParam> anim = {});
    // Keyframe buttons like DaVinci: ◀ diamond ▶ (red diamond = keyframe at the playhead; click sets/removes,
    // right-click = interpolation); applies to all parameters of the row
    QWidget* keyButtons(TrackKind kind, bool title, const QVector<AnimParam>& params, const QString& effect = {});
    // Clip frame under the playhead (limited to the clip)
    int localFrame(const Clip& c) const;
    QToolButton* resetButton(const std::function<void()>& fn);
    QLabel* rowLabel(const QString& text);

    // Row with color field (QColorDialog); alpha = opacity selectable, i.e. = additionally when applying
    // anim: animatable color (title color)
    void addColor(Section& s, const QString& label, const QString& text, const std::function<QColor&(Clip&)>& color,
                  bool alpha, const std::function<void(Clip&)>& also = {}, std::optional<AnimParam> anim = {});
    void buildTitlePage(QVBoxLayout* page);
    void buildTransitionPage(QVBoxLayout* page);
    // Selected transition (track kind + effective position), otherwise false
    bool selectedTransition(TrackKind* kind, TimelineOps::TransitionSpan* span) const;
    bool refreshTransition(); // true = transition selected and shown

    QVector<int> selectedIds(TrackKind kind, bool title = false, const QString& effect = {}) const;
    const Clip* primary(TrackKind kind, bool title = false, const QString& effect = {}) const;
    void apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn,
               bool title = false, const QString& effect = {});
    // Sort effect sections into the render order of the shown clip: fixed stages first, then the clip's order
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
    QVector<std::function<void()>> m_refreshers; // further displays (dots, color)
    bool m_zoomLinked = true;
    int m_lastShownId = 0; // selection changed -> for titles show the "Titel" tab
    Param* m_crop[4] = {};
    QToolButton* m_keyPipette = nullptr;
    // Effect sections: video (Open FX) in the Effects tab from m_fxIndex, audio (Audio FX) in the Audio tab from
    // m_audioFxIndex
    struct FxSection { QString id; QWidget* header; QWidget* body; TrackKind kind = TrackKind::Video; };
    QVector<FxSection> m_fxSections;
    QVBoxLayout* m_fxLay = nullptr;
    int m_fxIndex = 0;
    QVBoxLayout* m_audioFxLay = nullptr;
    int m_audioFxIndex = 0;
    QStringList m_audioFxOrder;
    std::function<void()> m_stabilizeRefresh; // only the analysis row (progress ticks)
    QWidget* m_gradeRow = nullptr; // Color page grade/LUT: shown here, edited on the Color page
    QLabel* m_gradeText = nullptr;
    int m_fxCount = 0;             // effects of the shown clip (tab label; a new one opens the tab)
    int effectCount(const Clip& c) const;
    QStringList m_fxOrder; // last displayed order
    // Header drag of an effect section: pressed header (nullptr = none), its effect, press point (global), started
    // (past the drag distance), target place among the movable effects (-1 = where it is)
    struct FxDrag {
        QWidget* header = nullptr;
        QString id;
        TrackKind kind = TrackKind::Video;
        QPoint start;
        bool active = false;
        int to = -1;
        QString style; // the header's own style sheet while it is highlighted
    };
    FxDrag m_fxDrag;
    QWidget* m_dropLine = nullptr; // line in the gap the dragged effect lands in
    // Tab "Übergang"
    TransitionKey m_transKey;
    QComboBox* m_transType = nullptr;
    QComboBox* m_transAudioType = nullptr; // Audio: Cross Fade +3/0/-3 dB
    QComboBox* m_transAlign = nullptr;
    ScrubField* m_transLen = nullptr;
    QToolButton* m_transColor = nullptr;       // fade-to-color color
    ScrubField* m_transSoft = nullptr;         // wipe: softness
    ScrubField* m_transBorder = nullptr;       // wipe: border width
    QToolButton* m_transBorderColor = nullptr; // wipe: border color
    QComboBox* m_transLuma = nullptr;          // Gradient wipe: gradient image (bundled + effects folder)
    class QCheckBox* m_transInvert = nullptr;  // gradient wipe: invert
    QVector<QWidget*> m_dipRows, m_wipeRows, m_lumaRows, m_softRows; // rows only for the respective kind
    // Tab "Untertitel": text/times of the entry and style of its track (like DaVinci Caption + Track Style)
    void buildSubtitlePage(QVBoxLayout* page);
    bool refreshSubtitle(); // true = only subtitles selected and shown
    const SubtitleCue* selectedCue(int* track = nullptr) const;
    void changeSubtitleStyle(const QString& text, const std::function<void(TitleStyle&)>& fn, const QString& mergeKey = {});
    QPlainTextEdit* m_subText = nullptr;
    QVector<std::function<void()>> m_subRefreshers;
    // Change the style of the selected transition (mergeKey: dragging = one undo step)
    void changeTransition(const std::function<void(TransitionStyle&)>& fn, const QString& mergeKey = {});
};
