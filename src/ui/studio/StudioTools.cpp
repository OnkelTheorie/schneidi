// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioTools.h"

#include "app/Theme.h"
#include "app/ToolErrors.h"
#include "app/VoiceoverRecorder.h"
#include "core/Ducking.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "ui/EffectsLibrary.h"
#include "ui/ElidedLabel.h"

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// The armed track's / the take's input level (a bar, like the old mixer's)
class StudioInputMeter : public QWidget {
public:
    StudioInputMeter()
    {
        setFixedHeight(10);
        setMinimumWidth(60);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(T("Eingangspegel"));
    }
    void setLevel(float db)
    {
        m_db = std::max(db, m_db - 1.5f); // ~45 dB/s fall at 30 updates per second
        update();
    }
    void reset()
    {
        m_db = -100.f;
        update();
    }
    float level() const { return m_db; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Theme::well);
        constexpr float floor = -60.f;
        const float x = std::clamp((m_db - floor) / -floor, 0.f, 1.f) * width();
        const QColor c = m_db > -3.f ? Theme::meterHigh : m_db > -12.f ? Theme::meterMid : Theme::meterLow;
        p.fillRect(QRectF(0, 1, x, height() - 2), c);
    }

private:
    float m_db = -100.f;
};

namespace {

// Loudness presets: where the mix goes
struct LoudPreset {
    const char* name;
    double lufs;
};
constexpr LoudPreset kLoudPresets[] = {{N_("YouTube −14 LUFS"), -14}, {N_("Podcast −16 LUFS"), -16},
                                       {N_("Broadcast −23 LUFS"), -23}};

constexpr int kDuckMin = -30, kDuckMax = -3;

QString number(double v, int decimals = 1)
{
    return QString::number(v, 'f', decimals).replace('-', QChar(0x2212)).replace('.', I18n::decimalPoint());
}

// Sounds drag as op 'sfx' fields (EffectsLibrary::SoundMimeType), like the Effects Library's
class SoundList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty()) return nullptr;
        auto* data = new QMimeData;
        data->setData(EffectsLibrary::SoundMimeType, items.first()->data(Qt::UserRole).toByteArray());
        return data;
    }
    QStringList mimeTypes() const override { return {EffectsLibrary::SoundMimeType}; }
};

QToolButton* button(const QString& text, const QString& name, const QString& tip, bool primary = false)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setObjectName(name);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    b->setStyleSheet(QString("QToolButton { background: %1; color: %2; padding: 3px 10px; border: none; }"
                             "QToolButton:hover { background: %3; }"
                             "QToolButton:checked { background: %4; color: %5; }"
                             "QToolButton:disabled { color: %6; }")
                         .arg((primary ? Theme::primary : Theme::control).name(),
                              (primary ? Theme::onPrimary : Theme::text).name(),
                              (primary ? Theme::primary.lighter(115) : Theme::controlLight).name(),
                              Theme::recording.name(), Theme::readableOn(Theme::recording).name(),
                              Theme::textFaint.name()));
    return b;
}

QLabel* resultLine(const QString& name)
{
    auto* l = new QLabel;
    l->setObjectName(name);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    l->hide();
    return l;
}

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

} // namespace

StudioTools::StudioTools(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("studio_tools");
    setProperty("tourTarget", "studio_tools");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString("QWidget#studio_tools { background: %1; border-right: 1px solid %2; }")
                      .arg(Theme::panel.name(), Theme::border.name()));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Title row: the target track
    auto* head = new QWidget;
    head->setObjectName("PanelTitle");
    head->setAttribute(Qt::WA_StyledBackground);
    head->setStyleSheet(QString("QWidget#PanelTitle { background: %1; }").arg(Theme::panelHeader.name()));
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(6, 2, 6, 2);
    headLay->setSpacing(6);
    auto* title = new ElidedLabel;
    title->setFullText(T("Werkzeuge"));
    title->setObjectName("PanelTitle");
    headLay->addWidget(title, 1);
    auto* forLabel = new QLabel(T("Spur"));
    headLay->addWidget(forLabel);
    m_targetBox = new QComboBox;
    m_targetBox->setObjectName("studio_target");
    m_targetBox->setToolTip(T("Die Spur, auf die Stimme säubern und Absenken wirken: die gewählten Clips darauf, "
                              "sonst alle ihre Clips. Folgt der Auswahl."));
    m_targetBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetBox->setMinimumContentsLength(6);
    headLay->addWidget(m_targetBox);
    outer->addWidget(head);
    m_targetInfo = new QLabel;
    m_targetInfo->setObjectName("studio_target_info");
    m_targetInfo->setWordWrap(true);
    m_targetInfo->setContentsMargins(8, 3, 8, 0);
    m_targetInfo->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    outer->addWidget(m_targetInfo);

    // The cards scroll when the column is short (a small window, a large font)
    auto* scroll = m_scroll = new QScrollArea;
    scroll->setObjectName("studio_tools_scroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    inner->setObjectName("studio_tools_inner");
    m_grid = new QGridLayout(inner);
    m_grid->setContentsMargins(6, 6, 6, 6);
    m_grid->setHorizontalSpacing(6);
    m_grid->setVerticalSpacing(6);
    scroll->setWidget(inner);
    scroll->viewport()->installEventFilter(this);
    outer->addWidget(scroll, 1);

    // ---- Stimme säubern ----
    {
        auto* body = new QWidget;
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        lay->addWidget(hint(T("Rauschen und Geräusche weg (RNNoise), Pausen leiser, dazu der EQ „Stimme“ auf der Spur.")));
        auto* go = button(T("Stimme säubern"), "studio_clean_voice",
                          T("Stimme säubern auf den Clips der Zielspur und EQ-Vorlage „Stimme“ auf der Spur – "
                            "ein Schritt zum Rückgängigmachen"),
                          true);
        connect(go, &QToolButton::clicked, this, [this] { cleanVoice(); });
        lay->addWidget(go, 0, Qt::AlignLeft);
        m_cleanLine = resultLine("studio_clean_result");
        lay->addWidget(m_cleanLine);
        m_cards << card(T("Stimme säubern"), "studio_card_voice", body);
    }

    // ---- Musik unter Sprache absenken ----
    {
        auto* body = new QWidget;
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        lay->addWidget(hint(T("Die Musik der Zielspur wird leiser, sobald auf den anderen Spuren Ton ist.")));
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_duckAmount = new QSlider(Qt::Horizontal);
        m_duckAmount->setObjectName("studio_duck_amount");
        m_duckAmount->setRange(kDuckMin, kDuckMax);
        m_duckAmount->setToolTip(T("Wie weit die Musik abgesenkt wird"));
        m_duckAmount->setMinimumWidth(80);
        const int saved = QSettings().value("studio/duckAmount", -12).toInt();
        m_duckAmount->setValue(std::clamp(saved, kDuckMin, kDuckMax));
        m_duckValue = new QLabel;
        m_duckValue->setMinimumWidth(m_duckValue->fontMetrics().horizontalAdvance(T("%1 dB").arg(number(-30, 0))) + 4);
        const auto showAmount = [this] { m_duckValue->setText(T("%1 dB").arg(number(m_duckAmount->value(), 0))); };
        showAmount();
        connect(m_duckAmount, &QSlider::valueChanged, this, [this, showAmount](int v) {
            showAmount();
            QSettings().setValue("studio/duckAmount", v);
        });
        row->addWidget(m_duckAmount, 1);
        row->addWidget(m_duckValue);
        lay->addLayout(row);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        auto* on = button(T("Absenken"), "studio_duck",
                          T("Misst, wo auf den anderen Spuren Ton ist, und senkt die Musik dort ab (Lautstärke-"
                            "Keyframes, in den Spuren als Kurve zu sehen; nochmal = mit dem neuen Wert)"),
                          true);
        connect(on, &QToolButton::clicked, this, [this] { duck(); });
        m_unduck = button(T("Aufheben"), "studio_unduck",
                          T("Nimmt das Absenken zurück: die Lautstärke-Keyframes der Clips gehen (auch gezeichnete), "
                            "ihre Lautstärke bleibt"));
        connect(m_unduck, &QToolButton::clicked, this, [this] { unduck(); });
        buttons->addWidget(on);
        buttons->addWidget(m_unduck);
        buttons->addStretch(1);
        lay->addLayout(buttons);
        m_duckLine = resultLine("studio_duck_result");
        lay->addWidget(m_duckLine);
        m_cards << card(T("Musik unter Sprache absenken"), "studio_card_duck", body);
    }

    // ---- Lautheit auf Ziel ----
    {
        auto* body = new QWidget;
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        lay->addWidget(hint(T("Misst den ganzen Mix und stellt den Master so, dass er das Ziel trifft (Limiter an).")));
        m_loudPreset = new QComboBox;
        m_loudPreset->setObjectName("studio_loudness_preset");
        m_loudPreset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_loudPreset->setMinimumContentsLength(10);
        for (const LoudPreset& p : kLoudPresets) m_loudPreset->addItem(T(p.name), p.lufs);
        // The meter's target picks the preset (the user's last choice)
        const double target = QSettings().value("audio/loudnessTarget", -14.0).toDouble();
        for (int i = 0; i < m_loudPreset->count(); ++i)
            if (std::abs(m_loudPreset->itemData(i).toDouble() - target) < 0.05) m_loudPreset->setCurrentIndex(i);
        lay->addWidget(m_loudPreset);
        auto* go = button(T("Auf Ziel bringen"), "studio_loudness_apply",
                          T("Lautheit des Mixes auf das Ziel – ein Schritt zum Rückgängigmachen"), true);
        connect(go, &QToolButton::clicked, this, [this] { loudnessToTarget(); });
        lay->addWidget(go, 0, Qt::AlignLeft);
        m_loudLine = resultLine("studio_loudness_result");
        lay->addWidget(m_loudLine);
        m_cards << card(T("Lautheit auf Ziel"), "studio_card_loudness", body);
    }

    // ---- Aufnahme ----
    {
        auto* body = new QWidget;
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        row->addWidget(new QLabel(T("Auf")));
        m_recordTrack = new QComboBox;
        m_recordTrack->setObjectName("studio_record_track");
        m_recordTrack->setToolTip(T("Die Spur für die Aufnahme: „Automatisch“ nimmt die freie Spur der Auswahl, sonst "
                                    "eine Voiceover-Spur oder eine neue. Eine gewählte Spur ist aufnahmebereit "
                                    "(Mikrofon im Spurkopf) und zeigt ihren Eingangspegel."));
        m_recordTrack->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_recordTrack->setMinimumContentsLength(8);
        row->addWidget(m_recordTrack, 1);
        lay->addLayout(row);
        auto* row2 = new QHBoxLayout;
        row2->setSpacing(6);
        m_record = button("● " + T("Aufnehmen"), "studio_record",
                          T("Voiceover aufnehmen: die Timeline spielt ab dem Abspielkopf, die Aufnahme landet auf der "
                            "Spur (Esc oder Leertaste beendet)"));
        m_record->setCheckable(true);
        m_record->setEnabled(false); // until the recorder is set
        connect(m_record, &QToolButton::clicked, this, [this] {
            syncRecorder(); // the state follows the recorder, not the click
            emit recordRequested();
        });
        row2->addWidget(m_record);
        m_input = new StudioInputMeter;
        m_input->setObjectName("studio_input_level");
        row2->addWidget(m_input, 1);
        lay->addLayout(row2);
        m_recordLine = resultLine("studio_record_result");
        lay->addWidget(m_recordLine);
        connect(m_recordTrack, &QComboBox::activated, this, [this](int i) {
            if (m_recorder && !m_recorder->recording()) m_recorder->setArmedTrack(m_recordTrack->itemData(i).toInt());
        });
        m_cards << card(T("Aufnahme"), "studio_card_record", body);
    }

    // ---- Effektklänge ----
    {
        auto* body = new QWidget;
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        m_soundSearch = new QLineEdit;
        m_soundSearch->setObjectName("studio_sound_search");
        m_soundSearch->setPlaceholderText(T("Klänge suchen…"));
        m_soundSearch->setClearButtonEnabled(true);
        lay->addWidget(m_soundSearch);
        m_sounds = new SoundList;
        m_sounds->setObjectName("studio_sounds");
        m_sounds->setDragEnabled(true);
        m_sounds->setDragDropMode(QAbstractItemView::DragOnly);
        m_sounds->setMinimumHeight(90);
        m_sounds->setToolTip(T("Doppelklick = anhören, auf eine Spur ziehen = dort einfügen"));
        lay->addWidget(m_sounds, 1);
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* play = button(T("Anhören"), "studio_sound_play", T("Den Klang im Player anhören (es wird nichts eingefügt)"));
        auto* place = button(T("Einfügen"), "studio_sound_place", T("Den Klang am Abspielkopf einfügen (eigene Spur)"), true);
        row->addWidget(play);
        row->addWidget(place);
        row->addStretch(1);
        lay->addLayout(row);
        const auto current = [this]() -> QByteArray {
            const QListWidgetItem* it = m_sounds->currentItem();
            return it && !it->isHidden() ? it->data(Qt::UserRole).toByteArray() : QByteArray();
        };
        const auto preview = [this](const QByteArray& data) {
            if (data.isEmpty()) return;
            const QString file = EffectsLibrary::soundFile(data);
            if (!file.isEmpty()) emit soundPreviewRequested(file);
        };
        connect(play, &QToolButton::clicked, this, [current, preview] { preview(current()); });
        connect(m_sounds, &QListWidget::itemDoubleClicked, this,
                [preview](QListWidgetItem* it) { preview(it->data(Qt::UserRole).toByteArray()); });
        connect(place, &QToolButton::clicked, this, [this, current] {
            if (const QByteArray d = current(); !d.isEmpty()) emit placeSoundRequested(d);
        });
        connect(m_soundSearch, &QLineEdit::textChanged, this, [this](const QString& text) {
            const QStringList words = text.simplified().split(' ', Qt::SkipEmptyParts);
            QListWidgetItem* first = nullptr;
            for (int i = 0; i < m_sounds->count(); ++i) {
                QListWidgetItem* it = m_sounds->item(i);
                const QString hay = it->text() + ' ' + it->toolTip();
                const bool show = std::all_of(words.begin(), words.end(),
                                              [&](const QString& w) { return hay.contains(w, Qt::CaseInsensitive); });
                it->setHidden(!show);
                if (show && !first) first = it;
            }
            if (m_sounds->currentItem() && m_sounds->currentItem()->isHidden()) m_sounds->setCurrentItem(first);
        });
        auto* c = card(T("Effektklänge"), "studio_card_sounds", body);
        c->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        m_cards << c;
        fillSounds();
    }

    Project* project = m_editor->project();
    connect(project, &Project::timelineChanged, this, &StudioTools::syncTargets);
    connect(m_editor->selection(), &Selection::changed, this, [this] {
        // The target follows the selection: the track of its first audio clip
        const Timeline& tl = m_editor->project()->timeline();
        for (int id : m_editor->selectedAudioClips())
            if (TrackRef ref; TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Audio) {
                setTargetTrack(ref.index);
                return;
            }
        syncTargets();
    });
    connect(m_targetBox, &QComboBox::activated, this, [this](int i) { setTargetTrack(m_targetBox->itemData(i).toInt()); });
    m_target = std::max(0, m_editor->targetAudioTrack());
    syncTargets();
    reflow();
}

QWidget* StudioTools::card(const QString& title, const QString& name, QWidget* body)
{
    auto* c = new QWidget;
    c->setObjectName(name);
    c->setAttribute(Qt::WA_StyledBackground);
    c->setStyleSheet(QString("QWidget#%1 { background: %2; border: 1px solid %3; }").arg(name, Theme::mix(Theme::panel, Theme::panelHeader, 0.4).name(), Theme::border.name()));
    auto* lay = new QVBoxLayout(c);
    lay->setContentsMargins(8, 6, 8, 8);
    lay->setSpacing(4);
    auto* t = new ElidedLabel;
    t->setFullText(title);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    lay->addWidget(t);
    lay->addWidget(body, 1);
    c->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return c;
}

int StudioTools::cardWidth() const
{
    // About 30 characters of text: the hints wrap in two or three lines
    return std::max(220, fontMetrics().averageCharWidth() * 34);
}

void StudioTools::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    reflow();
}

bool StudioTools::eventFilter(QObject* watched, QEvent* e)
{
    if (m_scroll && watched == m_scroll->viewport() && e->type() == QEvent::Resize) fitInnerHeight();
    return QWidget::eventFilter(watched, e);
}

void StudioTools::fitInnerHeight()
{
    // QScrollArea sizes its widget by minimumSizeHint, which knows nothing of wrapped text: without this the hints
    // were squeezed under the card titles in a narrow column
    QWidget* inner = m_grid->parentWidget();
    const int w = m_scroll->viewport()->width();
    if (w <= 0) return;
    m_grid->activate();
    const int h = m_grid->hasHeightForWidth() ? m_grid->totalHeightForWidth(w) : m_grid->totalMinimumSize().height();
    inner->setMinimumHeight(h);
}

void StudioTools::reflow()
{
    const int cols = std::clamp((width() - 12) / (cardWidth() + 6), 1, 3);
    if (cols == m_columns) return;
    m_columns = cols;
    // Which card goes into which column (the sounds' list last, it takes what height is left); each column its own
    // stack from the top, so a short card never leaves a hole next to a tall one
    static const QVector<QVector<int>> kLayouts[] = {
        {{0, 1, 2, 3, 4}},
        {{0, 1, 2}, {3, 4}},
        {{0, 1}, {2, 3}, {4}},
    };
    QWidget* inner = m_grid->parentWidget();
    for (QWidget* c : m_cards) c->setParent(inner); // out of the old columns before they go
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QVector<QVector<int>>& layout = kLayouts[cols - 1];
    for (int col = 0; col < 3; ++col) m_grid->setColumnStretch(col, col < layout.size() ? 1 : 0);
    for (int col = 0; col < layout.size(); ++col) {
        auto* column = new QWidget;
        auto* lay = new QVBoxLayout(column);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        bool stretches = false;
        for (int i : layout[col]) {
            stretches |= i == 4;
            lay->addWidget(m_cards[i], i == 4 ? 1 : 0);
            m_cards[i]->show();
        }
        if (!stretches) lay->addStretch(1);
        m_grid->addWidget(column, 0, col);
    }
    fitInnerHeight();
}

void StudioTools::setTargetTrack(int track)
{
    const int count = int(m_editor->project()->timeline().audio.size());
    m_target = std::clamp(track, 0, std::max(0, count - 1));
    syncTargets();
}

QVector<int> StudioTools::targetClips() const
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_target < 0 || m_target >= tl.audio.size() || tl.audio[m_target].locked) return {};
    QVector<int> selected;
    for (int id : m_editor->selectedAudioClips())
        if (TrackRef ref; TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Audio && ref.index == m_target)
            selected << id;
    if (!selected.isEmpty()) return selected;
    return m_editor->audioTrackClips(m_target);
}

void StudioTools::syncTargets()
{
    const Timeline& tl = m_editor->project()->timeline();
    const int count = int(tl.audio.size());
    m_target = std::clamp(m_target, 0, std::max(0, count - 1));
    const auto fill = [&](QComboBox* box, bool automatic, int current) {
        const QSignalBlocker b(box);
        box->clear();
        if (automatic) box->addItem(T("Automatisch"), -1);
        for (int i = 0; i < count; ++i) {
            const TrackRef ref{TrackKind::Audio, i};
            const QString name = trackDisplayName(tl.audio[i], ref), shortName = trackShortName(ref);
            box->addItem(name == shortName ? shortName : QString("%1 · %2").arg(shortName, name), i);
        }
        box->setCurrentIndex(std::max(0, box->findData(current)));
    };
    fill(m_targetBox, false, m_target);
    fill(m_recordTrack, true, m_recorder ? m_recorder->armedTrack() : -1);
    // Which clips the tools take now
    QString info;
    if (count == 0) {
        info = T("Keine Tonspur");
    } else if (tl.audio[m_target].locked) {
        info = T("%1 ist gesperrt").arg(trackShortName({TrackKind::Audio, m_target}));
    } else {
        const QVector<int> clips = targetClips();
        int selectedOnTarget = 0;
        for (int id : clips) selectedOnTarget += m_editor->selection()->contains(id) ? 1 : 0;
        const QString track = trackShortName({TrackKind::Audio, m_target});
        info = clips.isEmpty()       ? T("%1 hat keine Clips").arg(track)
               : selectedOnTarget > 0 ? T("Für %1 gewählte Clips auf %2").arg(clips.size()).arg(track)
                                      : T("Für alle %1 Clips auf %2").arg(clips.size()).arg(track);
    }
    m_targetInfo->setText(info);
    // Ducking can be taken off when a clip has volume keyframes
    bool keyed = false;
    for (int id : targetClips())
        if (const Clip* c = TimelineOps::findClip(tl, id)) keyed |= Keys::animated(*c, AnimParam::Volume);
    m_unduck->setEnabled(keyed);
}

void StudioTools::say(QLabel* line, const QString& text, bool bad)
{
    line->setText(text);
    line->setStyleSheet(QString("color: %1;").arg((bad ? Theme::warning : Theme::textDim).name()));
    line->setVisible(!text.isEmpty());
}

int StudioTools::cleanVoice()
{
    const QVector<int> clips = targetClips();
    const QVector<int> done = m_editor->cleanVoice(clips, {}, true);
    if (done.isEmpty())
        say(m_cleanLine, T("Nichts zu säubern: wähl Tonclips aus oder leg sie auf die Spur %1")
                             .arg(trackShortName({TrackKind::Audio, m_target})),
            true);
    else
        say(m_cleanLine, T("Gesäubert: %1 Clips, EQ „Stimme“ auf %2 – Strg+Z nimmt es zurück")
                             .arg(done.size())
                             .arg(trackShortName({TrackKind::Audio, m_target})));
    return int(done.size());
}

QVector<int> StudioTools::underTracks() const
{
    const Timeline& tl = m_editor->project()->timeline();
    const bool solo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    QVector<int> out;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        if (i == m_target || t.clips.isEmpty() || (solo ? !t.solo : t.muted)) continue;
        out << i;
    }
    return out;
}

int StudioTools::duck()
{
    const QVector<int> music = targetClips();
    const QString track = trackShortName({TrackKind::Audio, m_target});
    if (music.isEmpty()) {
        say(m_duckLine, T("Keine Musik: leg sie auf %1 oder wähl ihre Clips aus").arg(track), true);
        return 0;
    }
    const QVector<int> under = underTracks();
    if (under.isEmpty()) {
        say(m_duckLine, T("Keine andere Spur mit Ton: die Sprache gehört auf eine eigene Spur"), true);
        return 0;
    }
    Project* project = m_editor->project();
    Ducking::Params params = Ducking::defaults(project->format().rate.fps());
    params.amountDb = m_duckAmount->value();
    // Measure where the other tracks have sound (mixed like the export; originals, never proxies)
    QProgressDialog progress(T("Ton wird gemessen…"), T("Abbrechen"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    const auto step = [&](double f) {
        progress.setValue(int(f * 1000));
        QApplication::processEvents();
        return !progress.wasCanceled();
    };
    const auto ranges = AudioAnalysis::soundRanges(project->format(), project->renderTimeline(), under, params, step);
    const bool canceled = progress.wasCanceled();
    progress.reset();
    if (canceled) return 0;
    QStringList names;
    for (int i : under) names << trackShortName({TrackKind::Audio, i});
    if (!ranges) {
        say(m_duckLine, T("Der Ton der Spuren ließ sich nicht messen."), true);
        return 0;
    }
    if (ranges->isEmpty()) {
        say(m_duckLine, T("Auf %1 ist kein Ton über dem Threshold.").arg(names.join(", ")), true);
        return 0;
    }
    const int ducked = m_editor->duck(music, *ranges, params);
    say(m_duckLine, T("%1 Clips um %2 dB abgesenkt, wenn auf %3 Ton ist")
                        .arg(ducked)
                        .arg(number(params.amountDb, 0))
                        .arg(names.join(", ")));
    return ducked;
}

int StudioTools::unduck()
{
    const int n = m_editor->unduck(targetClips());
    say(m_duckLine, n ? T("Absenken aufgehoben (%1 Clips)").arg(n) : T("Nichts abgesenkt"), n == 0);
    return n;
}

double StudioTools::loudnessPreset() const { return m_loudPreset->currentData().toDouble(); }

bool StudioTools::loudnessToTarget()
{
    if (!m_run) return false; // no command layer (a bare library build)
    const double target = loudnessPreset();
    const QJsonObject op{{"op", "mixer"}, {"track", "master"}, {"limiter", true}, {"target_lufs", target}};
    QApplication::setOverrideCursor(Qt::WaitCursor);
    say(m_loudLine, T("Mix wird gemessen…"));
    QApplication::processEvents();
    const QJsonObject reply = m_run("edit", {{"project", "@app"}, {"ops", QJsonArray{op}}}, T("Lautheit auf Ziel"));
    QApplication::restoreOverrideCursor();
    if (!reply["ok"].toBool()) {
        const QJsonObject error = reply["error"].toObject();
        const QString code = error["code"].toString();
        say(m_loudLine, code == "FAILED" || code == "EMPTY_TIMELINE" ? T("Kein Ton zum Messen")
                                                                     : ToolErrors::text(code, error["message"].toString()),
            true);
        return false;
    }
    // The meter's target follows (its color says "on target")
    QSettings().setValue("audio/loudnessTarget", target);
    for (const QJsonValue& n : reply["result"]["notes"].toArray())
        if (const QJsonObject r = n.toObject()["loudness"].toObject(); !r.isEmpty()) {
            const double after = r["after_lufs"].toDouble();
            QString text = T("Vorher %1 LUFS → jetzt %2 LUFS, True Peak %3 dBTP")
                               .arg(number(r["before_lufs"].toDouble()), number(after), number(r["true_peak_db"].toDouble()));
            const bool missed = after < target - 0.5;
            if (missed) text += "\n" + T("Ziel nicht ganz erreicht: der Limiter nimmt die Spitzen (ein Kompressor auf der lautesten Spur hilft)");
            say(m_loudLine, text, missed);
            return true;
        }
    say(m_loudLine, T("Lautheit eingestellt"));
    return true;
}

void StudioTools::setRecorder(VoiceoverRecorder* recorder)
{
    m_recorder = recorder;
    if (!recorder) return;
    m_record->setEnabled(true);
    connect(recorder, &VoiceoverRecorder::armedChanged, this, &StudioTools::syncTargets);
    connect(recorder, &VoiceoverRecorder::recordingChanged, this, [this](bool on) {
        syncRecorder();
        if (!on) m_input->reset();
    });
    connect(recorder, &VoiceoverRecorder::inputLevel, m_input, &StudioInputMeter::setLevel);
    connect(recorder, &VoiceoverRecorder::liveChanged, this, [this](bool on) {
        if (on) say(m_recordLine, T("Aufnahme läuft – Esc oder Leertaste beendet"));
    });
    connect(recorder, &VoiceoverRecorder::finished, this, [this](const QString& path, bool placed, const QString& error) {
        if (placed)
            say(m_recordLine, T("Auf %1 gelegt: %2")
                                  .arg(trackShortName({TrackKind::Audio, m_recorder->placedTrack()}), QFileInfo(path).fileName()));
        else
            say(m_recordLine, error, !error.isEmpty());
    });
    connect(recorder, &VoiceoverRecorder::failed, this, [this](const QString& error) { say(m_recordLine, error, true); });
    syncTargets();
    syncRecorder();
}

void StudioTools::syncRecorder()
{
    const bool on = m_recorder && m_recorder->recording();
    const QSignalBlocker b(m_record);
    m_record->setChecked(on);
    m_record->setText(on ? "■ " + T("Beenden") : "● " + T("Aufnehmen"));
    m_recordTrack->setEnabled(!on);
}

void StudioTools::fillSounds()
{
    m_sounds->clear();
    for (const EffectsLibrary::SoundEntry& e : EffectsLibrary::soundEntries()) {
        auto* it = new QListWidgetItem(e.name, m_sounds);
        it->setData(Qt::UserRole, e.data);
        it->setToolTip(e.group + " · " + e.tip);
    }
    if (m_sounds->count()) m_sounds->setCurrentRow(0);
}
