// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioTools.h"

#include "app/Theme.h"
#include "app/ToolErrors.h"
#include "app/VoiceoverRecorder.h"
#include "core/Ducking.h"
#include "core/EffectFolders.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TaskProgress.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Extensions.h"
#include "ui/EffectsLibrary.h"
#include "ui/ElidedLabel.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
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

// The input devices are listed when the list opens (asking the system for them takes a moment, and a headset may
// have come since)
class InputCombo : public QComboBox {
public:
    std::function<void()> beforePopup;
    void showPopup() override
    {
        if (beforePopup) beforePopup();
        QComboBox::showPopup();
    }
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
    setStyleSheet(QString("QWidget#studio_tools { background: %1; border-top: 1px solid %2; }")
                      .arg(Theme::panel.name(), Theme::border.name()));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Title row: the target track, which clips the tools take, the close button
    auto* head = new QWidget;
    head->setObjectName("PanelTitle");
    head->setAttribute(Qt::WA_StyledBackground);
    head->setStyleSheet(QString("QWidget#PanelTitle { background: %1; }").arg(Theme::panelHeader.name()));
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(6, 2, 2, 2);
    headLay->setSpacing(6);
    auto* title = new QLabel(T("Werkzeuge"));
    title->setObjectName("PanelTitle");
    headLay->addWidget(title);
    headLay->addSpacing(6);
    auto* forLabel = new QLabel(T("Spur"));
    headLay->addWidget(forLabel);
    m_targetBox = new QComboBox;
    m_targetBox->setObjectName("studio_target");
    m_targetBox->setToolTip(T("Die Spur, auf die Stimme säubern und Absenken wirken: die gewählten Clips darauf, "
                              "sonst alle ihre Clips. Folgt der Auswahl."));
    m_targetBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetBox->setMinimumContentsLength(6);
    headLay->addWidget(m_targetBox);
    auto* info = new ElidedLabel;
    m_targetInfo = info;
    m_targetInfo->setObjectName("studio_target_info");
    m_targetInfo->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    headLay->addWidget(m_targetInfo, 1);
    auto* close = new QToolButton;
    close->setObjectName("studio_tools_close");
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(T("Werkzeuge schließen"));
    connect(close, &QToolButton::clicked, this, &StudioTools::closeRequested);
    headLay->addWidget(close);
    outer->addWidget(head);

    // Below: the list of the tools on the left, the chosen one beside it (it scrolls when the drawer is short)
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto* list = new QWidget;
    list->setObjectName("studio_tool_list");
    list->setAttribute(Qt::WA_StyledBackground);
    list->setStyleSheet(QString("QWidget#studio_tool_list { background: %1; border-right: 1px solid %2; }")
                            .arg(Theme::mix(Theme::panel, Theme::panelHeader, 0.4).name(), Theme::border.name()));
    m_list = new QVBoxLayout(list);
    m_list->setContentsMargins(0, 4, 0, 4);
    m_list->setSpacing(1);
    m_tabs = new QButtonGroup(this);
    body->addWidget(list);
    auto* scroll = m_scroll = new QScrollArea;
    scroll->setObjectName("studio_tools_scroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stack = new QStackedWidget;
    m_stack->setObjectName("studio_tools_inner");
    scroll->setWidget(m_stack);
    scroll->viewport()->installEventFilter(this);
    body->addWidget(scroll, 1);
    outer->addLayout(body, 1);

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
        addTool(T("Stimme säubern"), "voice", body);
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
        // Which tracks make it duck: all other audible tracks (the default) or the ones picked here (remembered)
        auto* byRow = new QHBoxLayout;
        byRow->setSpacing(6);
        byRow->addWidget(new QLabel(T("Durch")));
        m_duckByButton = button(QString(), "studio_duck_by",
                                T("Welche Spuren die Musik absenken: alle anderen hörbaren Spuren oder nur die hier "
                                  "gewählten"));
        m_duckByButton->setPopupMode(QToolButton::InstantPopup);
        auto* byMenu = new QMenu(m_duckByButton);
        byMenu->setObjectName("studio_duck_by_menu");
        m_duckByButton->setMenu(byMenu);
        connect(byMenu, &QMenu::aboutToShow, this, [this, byMenu] { fillDuckByMenu(byMenu); });
        for (const QVariant& v : QSettings().value("studio/duckBy").toList()) m_duckBy << v.toInt();
        byRow->addWidget(m_duckByButton, 1);
        lay->addLayout(byRow);
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
        addTool(T("Musik absenken"), "duck", body, T("Musik unter Sprache absenken"));
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
        addTool(T("Lautheit auf Ziel"), "loudness", body);
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
        // The input device (the same choice as Wiedergabe › Audio-Ausgabe › Eingabegerät)
        auto* inputRow = new QHBoxLayout;
        inputRow->setSpacing(6);
        inputRow->addWidget(new QLabel(T("Eingang")));
        auto* input = new InputCombo;
        m_recordInput = input;
        input->setObjectName("studio_record_input");
        input->setToolTip(T("Das Mikrofon für die Aufnahme (dasselbe wie Wiedergabe › Audio-Ausgabe › Eingabegerät)"));
        input->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        input->setMinimumContentsLength(8);
        input->beforePopup = [this] { fillInputs(); };
        connect(input, &QComboBox::activated, this, [this](int i) {
            const QString id = m_recordInput->itemData(i).toString();
            if (id == Voiceover::Settings::load().input) return;
            VoiceoverRecorder::setInput(id);
            if (m_recorder) m_recorder->inputChanged();
        });
        inputRow->addWidget(input, 1);
        lay->addLayout(inputRow);
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
        addTool(T("Aufnahme"), "record", body);
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
        // Never taller than the drawer gives it: the list scrolls, the panel does not grow with the sounds
        m_sounds->setMinimumHeight(60);
        m_sounds->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
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
        connect(m_soundSearch, &QLineEdit::textChanged, this, &StudioTools::filterSounds);
        addTool(T("Effektklänge"), "sounds", body);
        fillSounds();
        // Sound packs installed or removed, own sounds added: the list anew (the folders watched like the Effects
        // Library's, changes collected after a short pause)
        m_soundWatcher = new QFileSystemWatcher(this);
        m_soundRescan = new QTimer(this);
        m_soundRescan->setSingleShot(true);
        m_soundRescan->setInterval(300);
        connect(m_soundWatcher, &QFileSystemWatcher::directoryChanged, m_soundRescan, qOverload<>(&QTimer::start));
        connect(m_soundRescan, &QTimer::timeout, this, [this] {
            watchSounds();
            fillSounds();
        });
        watchSounds();
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
    m_list->addStretch(1);
    // The last tool chosen (one click away once the drawer is open)
    setCurrentTool(QSettings().value("studio/tool", 0).toInt());
    connect(m_tabs, &QButtonGroup::idClicked, this, [this](int id) {
        setCurrentTool(id);
        QSettings().setValue("studio/tool", id);
    });
    syncTargets();
}

void StudioTools::addTool(const QString& tab, const QString& id, QWidget* body, const QString& title)
{
    const int index = m_stack->count();
    auto* b = new QPushButton(tab); // left-aligned text (a QToolButton centres it whatever the style says)
    b->setObjectName("studio_tab_" + id);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!title.isEmpty()) b->setToolTip(title);
    b->setStyleSheet(QString("QPushButton { background: transparent; color: %1; padding: 3px 12px 3px 10px; border: none;"
                             " text-align: left; border-left: 3px solid transparent; }"
                             "QPushButton:hover { background: %2; }"
                             "QPushButton:checked { background: %3; color: %4; border-left: 3px solid %5; }")
                         .arg(Theme::textDim.name(), Theme::control.name(), Theme::panel.name(), Theme::text.name(),
                              Theme::primary.name()));
    m_tabs->addButton(b, index);
    m_list->addWidget(b);
    auto* card = new QWidget;
    card->setObjectName("studio_card_" + id);
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 6, 10, 8);
    lay->setSpacing(4);
    auto* t = new ElidedLabel;
    t->setFullText(title.isEmpty() ? tab : title);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    lay->addWidget(t);
    // The effect sounds' list takes the height there is; the other tools sit at the top
    const bool grows = body->findChild<QListWidget*>() != nullptr;
    lay->addWidget(body, grows ? 1 : 0);
    if (!grows) lay->addStretch(1);
    m_stack->addWidget(card);
}

int StudioTools::toolCount() const { return m_stack->count(); }

int StudioTools::currentTool() const { return m_stack->currentIndex(); }

void StudioTools::setCurrentTool(int index)
{
    index = std::clamp(index, 0, m_stack->count() - 1);
    m_stack->setCurrentIndex(index);
    if (QAbstractButton* b = m_tabs->button(index)) b->setChecked(true);
    fitInnerHeight();
}

bool StudioTools::eventFilter(QObject* watched, QEvent* e)
{
    if (m_scroll && watched == m_scroll->viewport() && e->type() == QEvent::Resize) fitInnerHeight();
    return QWidget::eventFilter(watched, e);
}

void StudioTools::fitInnerHeight()
{
    // QScrollArea sizes its widget by minimumSizeHint, which knows nothing of wrapped text (and a stack takes the
    // largest of its pages): the chosen tool as tall as its text needs at this width, the area scrolls beyond
    QWidget* card = m_stack->currentWidget();
    const int w = m_scroll->viewport()->width();
    if (!card || w <= 0) return;
    QLayout* l = card->layout();
    l->activate();
    const int h = l->hasHeightForWidth() ? l->totalHeightForWidth(w) : l->totalMinimumSize().height();
    m_stack->setMinimumHeight(h);
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

void StudioTools::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_syncPending) syncTargets();
    if (m_recordInput->count() == 0) fillInputs(); // the devices once the page shows, not when it is built
}

void StudioTools::syncTargets()
{
    const Timeline& tl = m_editor->project()->timeline();
    const int count = int(tl.audio.size());
    m_target = std::clamp(m_target, 0, std::max(0, count - 1));
    // Every change of the timeline lands here: on another page only remembered (done when the page shows)
    if (!isVisible()) {
        m_syncPending = true;
        return;
    }
    m_syncPending = false;
    QStringList names;
    for (int i = 0; i < count; ++i) {
        const TrackRef ref{TrackKind::Audio, i};
        const QString name = trackDisplayName(tl.audio[i], ref), shortName = trackShortName(ref);
        names << (name == shortName ? shortName : QString("%1 · %2").arg(shortName, name));
    }
    const auto fill = [&](QComboBox* box, bool automatic, int current) {
        const QSignalBlocker b(box);
        // Refilled only when the tracks changed (most edits change clips, not tracks; an open list stays as it is)
        bool same = box->count() == names.size() + (automatic ? 1 : 0);
        for (int i = 0; same && i < names.size(); ++i) same = box->itemText(i + (automatic ? 1 : 0)) == names[i];
        if (!same) {
            box->clear();
            if (automatic) box->addItem(T("Automatisch"), -1);
            for (int i = 0; i < count; ++i) box->addItem(names[i], i);
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
    static_cast<ElidedLabel*>(m_targetInfo)->setFullText(info);
    // Ducking can be taken off when a clip has volume keyframes
    bool keyed = false;
    for (int id : targetClips())
        if (const Clip* c = TimelineOps::findClip(tl, id)) keyed |= Keys::animated(*c, AnimParam::Volume);
    m_unduck->setEnabled(keyed);
    m_duckByButton->setText(duckByText());
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
    // The tracks picked under "Durch" (muted or not: they were chosen), those with clips
    if (const QVector<int> picked = pickedDuckTracks(); !picked.isEmpty()) {
        QVector<int> out;
        for (int i : picked)
            if (!tl.audio[i].clips.isEmpty()) out << i;
        return out;
    }
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
    // Measuring the whole mix several times takes a while on a long edit (~50 s for 30 min): a dialog with progress and
    // cancel, like the ducking's. It shows at once: its event loop runs inside the command's open undo step, so the
    // window must not take input meanwhile (the AIs wait too, Cli::Live), nor may a recording start.
    QProgressDialog progress(T("Mix wird gemessen…"), T("Abbrechen"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    const bool recordable = m_record->isEnabled();
    m_record->setEnabled(false);
    say(m_loudLine, T("Mix wird gemessen…"));
    QJsonObject reply;
    {
        const TaskProgress::Scope scope([&](double f) {
            if (progress.wasCanceled()) return false;
            progress.setValue(int(f * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
        reply = m_run("edit", {{"project", "@app"}, {"ops", QJsonArray{op}}}, T("Lautheit auf Ziel"));
    }
    progress.reset();
    m_record->setEnabled(recordable);
    syncRecorder();
    if (!reply.value("ok").toBool()) {
        const QJsonObject error = reply.value("error").toObject();
        const QString code = error["code"].toString();
        if (code == "CANCELED") {
            say(m_loudLine, T("Abgebrochen – nichts geändert"));
            return false;
        }
        say(m_loudLine, code == "FAILED" || code == "EMPTY_TIMELINE" ? T("Kein Ton zum Messen")
                                                                     : ToolErrors::text(code, error["message"].toString()),
            true);
        return false;
    }
    // The meter's target follows (its color says "on target")
    QSettings().setValue("audio/loudnessTarget", target);
    emit loudnessTargetChanged();
    for (const QJsonValue& n : reply.value("result").toObject().value("notes").toArray())
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
    m_recordInput->setEnabled(!on);
}

void StudioTools::fillSounds()
{
    // Again after a change of the folders: the chosen sound and the search stay
    const QByteArray current = m_sounds->currentItem() ? m_sounds->currentItem()->data(Qt::UserRole).toByteArray() : QByteArray();
    m_sounds->clear();
    QListWidgetItem* again = nullptr;
    for (const EffectsLibrary::SoundEntry& e : EffectsLibrary::soundEntries()) {
        auto* it = new QListWidgetItem(e.name, m_sounds);
        it->setData(Qt::UserRole, e.data);
        it->setToolTip(e.group + " · " + e.tip);
        if (!current.isEmpty() && e.data == current) again = it;
    }
    if (again) m_sounds->setCurrentItem(again);
    else if (m_sounds->count()) m_sounds->setCurrentRow(0);
    filterSounds(m_soundSearch->text());
}

void StudioTools::filterSounds(const QString& text)
{
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
}

void StudioTools::watchSounds()
{
    // The own sounds' folder with its subfolders and the packs' folder (installing one renames a folder in it);
    // a folder that does not exist yet: its parent, so that its making is seen
    QStringList dirs;
    const auto add = [&](const QString& dir, bool deep) {
        QString d = dir;
        while (!d.isEmpty() && !QFileInfo(d).isDir()) {
            const QString up = QFileInfo(d).absolutePath();
            if (up == d) return;
            d = up;
            deep = false;
        }
        dirs << d;
        if (!deep) return;
        QDirIterator it(d, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) dirs << it.next();
    };
    add(EffectFolders::soundDir(), true);
    add(Extensions::soundPacksDir(), false);
    dirs.removeDuplicates();
    if (!m_soundWatcher->directories().isEmpty()) m_soundWatcher->removePaths(m_soundWatcher->directories());
    m_soundWatcher->addPaths(dirs);
}

void StudioTools::fillInputs()
{
    const QString current = Voiceover::Settings::load().input;
    const QSignalBlocker b(m_recordInput);
    m_recordInput->clear();
    for (const VoiceoverRecorder::InputChoice& c : VoiceoverRecorder::inputChoices()) m_recordInput->addItem(c.name, c.id);
    m_recordInput->setCurrentIndex(std::max(0, m_recordInput->findData(current)));
}

QString StudioTools::duckByText() const
{
    const QVector<int> picked = pickedDuckTracks();
    if (picked.isEmpty()) return T("Alle anderen Spuren");
    QStringList names;
    for (int i : picked) names << trackShortName({TrackKind::Audio, i});
    return names.join(", ");
}

void StudioTools::fillDuckByMenu(QMenu* menu)
{
    menu->clear();
    const Timeline& tl = m_editor->project()->timeline();
    const QVector<int> picked = pickedDuckTracks();
    QAction* all = menu->addAction(T("Alle anderen hörbaren Spuren"));
    all->setCheckable(true);
    all->setChecked(picked.isEmpty());
    connect(all, &QAction::triggered, this, [this] { setDuckBy({}); });
    menu->addSeparator();
    for (int i = 0; i < tl.audio.size(); ++i) {
        if (i == m_target) continue; // the music itself
        const TrackRef ref{TrackKind::Audio, i};
        const QString name = trackDisplayName(tl.audio[i], ref), shortName = trackShortName(ref);
        QAction* a = menu->addAction(name == shortName ? shortName : QString("%1 · %2").arg(shortName, name));
        a->setCheckable(true);
        a->setChecked(picked.contains(i));
        connect(a, &QAction::triggered, this, [this, i](bool on) {
            QVector<int> by = pickedDuckTracks();
            if (on && !by.contains(i)) by << i;
            if (!on) by.removeAll(i);
            std::sort(by.begin(), by.end());
            setDuckBy(by);
        });
    }
}

void StudioTools::setDuckBy(const QVector<int>& tracks)
{
    m_duckBy = tracks;
    QVariantList list;
    for (int i : tracks) list << i;
    QSettings().setValue("studio/duckBy", list);
    m_duckByButton->setText(duckByText());
}

QVector<int> StudioTools::pickedDuckTracks() const
{
    // The picked tracks that are there in this project (the choice is remembered across projects) and not the music
    const Timeline& tl = m_editor->project()->timeline();
    QVector<int> out;
    for (int i : m_duckBy)
        if (i >= 0 && i < tl.audio.size() && i != m_target && !out.contains(i)) out << i;
    return out;
}
