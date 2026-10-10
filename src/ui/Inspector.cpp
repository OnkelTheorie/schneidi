#include "ui/Inspector.h"

#include "app/Theme.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "ui/ScrubField.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDir>
#include <QComboBox>
#include <QFontComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include "ui/InspectorDetail.h"

using namespace InspectorDetail;

Inspector::Inspector(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    auto* title = new QLabel("Inspector");
    title->setObjectName("PanelTitle");

    m_empty = new QLabel(T("Kein Clip ausgewählt"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    // ---- Tabs oben (Symbol über Text wie in DaVinci) ----
    auto* tabBar = new QWidget;
    tabBar->setObjectName("InspectorTabs");
    tabBar->setStyleSheet(QString("QWidget#InspectorTabs { background: %1; border-bottom: 1px solid %2; }"
                                  "QToolButton { color: %3; border: none; background: transparent; padding: 4px 14px; }"
                                  "QToolButton:checked { color: %4; }"
                                  "QToolButton:disabled { color: %5; }")
                              .arg(Theme::panel.name(), Theme::border.name(), Theme::textDim.name(), Theme::text.name(),
                                   Theme::textFaint.name()));
    auto* tabLay = new QHBoxLayout(tabBar);
    tabLay->setContentsMargins(4, 2, 4, 2);
    tabLay->setSpacing(0);
    m_tabs = new QButtonGroup(this);
    m_pages = new QStackedWidget;
    // Seiten-Index = Button-ID; "Titel" (nur bei Titelclips, wie DaVinci) steht vorne
    const struct { int id; const char* icon; const char* text; } tabs[] = {
        {kTitlePage, "T", N_("Titel")}, {kTransitionPage, "⧓", N_("Übergang")}, {kSubtitlePage, "CC", N_("Untertitel")},
        {0, "▣", "Video"}, {1, "♫", "Audio"}, {kEffectsPage, "fx", N_("Effekte")}};
    for (const auto& t : tabs) {
        auto* b = new QToolButton;
        b->setText(QString("%1\n%2").arg(t.icon, T(t.text)));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        m_tabs->addButton(b, t.id);
        tabLay->addWidget(b);
    }
    tabLay->addStretch(1);
    connect(m_tabs, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);

    auto makePage = [this]() {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        auto* scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_pages->addWidget(scroll);
        return lay;
    };
    QVBoxLayout* videoLay = makePage();
    QVBoxLayout* audioLay = makePage();
    buildTitlePage(makePage());
    buildTransitionPage(makePage());
    buildSubtitlePage(makePage());
    QVBoxLayout* fxLay = makePage();

    const TrackKind V = TrackKind::Video;

    // ---- Transform ----
    Section tr = addSection(videoLay, V, "Transform", [](Clip& c) {
        const ClipTransform d;
        c.transform.zoomX = d.zoomX;
        c.transform.zoomY = d.zoomY;
        c.transform.posX = d.posX;
        c.transform.posY = d.posY;
        c.transform.rotation = d.rotation;
        c.transform.transformOn = true;
        for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation})
            Keys::clear(c, p);
    }, [](Clip& c) -> bool& { return c.transform.transformOn; });

    auto* link = new QToolButton;
    link->setIcon(chainIcon());
    link->setIconSize(QSize(16, 16));
    link->setStyleSheet("QToolButton { border: none; background: transparent; }");
    link->setToolTip(T("Zoom X/Y gekoppelt"));
    link->setCheckable(true);
    link->setChecked(true);
    link->setAutoRaise(true);
    link->setFocusPolicy(Qt::NoFocus);
    link->setFixedSize(20, 20);
    connect(link, &QToolButton::toggled, this, [this](bool on) { m_zoomLinked = on; });
    const auto zoom = addXY(tr, "zoom", "Zoom", 0.0, 100.0, 1.0, 0.005, 3,
                            [](Clip& c) -> double& { return c.transform.zoomX; },
                            [](Clip& c) -> double& { return c.transform.zoomY; }, link, AnimParam::ZoomX,
                            AnimParam::ZoomY);
    // gekoppelt: X und Y gemeinsam ändern (animiert: Keyframe am Playhead, wie DaVinci)
    zoom.first->edit->onChange = [this, zy = zoom.second](double v) {
        if (m_zoomLinked) zy->edit->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            Keys::setValue(c, AnimParam::ZoomX, localFrame(c), v);
            if (m_zoomLinked) Keys::setValue(c, AnimParam::ZoomY, localFrame(c), v);
        });
    };
    zoom.second->edit->onChange = [this, zx = zoom.first](double v) {
        if (m_zoomLinked) zx->edit->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            Keys::setValue(c, AnimParam::ZoomY, localFrame(c), v);
            if (m_zoomLinked) Keys::setValue(c, AnimParam::ZoomX, localFrame(c), v);
        });
    };
    addXY(tr, "pos", "Position", -10000, 10000, 0, 1, 3, [](Clip& c) -> double& { return c.transform.posX; },
          [](Clip& c) -> double& { return c.transform.posY; }, nullptr, AnimParam::PosX, AnimParam::PosY);
    addSlider(tr, "rotation", "Rotation", -360, 360, 0, 0.2, 3, [](Clip& c) -> double& { return c.transform.rotation; },
              AnimParam::Rotation);

    // ---- Beschneiden ----
    Section crop = addSection(videoLay, V, T("Beschneiden"), [](Clip& c) {
        c.transform.cropLeft = c.transform.cropRight = c.transform.cropTop = c.transform.cropBottom = 0;
        c.transform.cropOn = true;
        for (AnimParam p : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom})
            Keys::clear(c, p);
    }, [](Clip& c) -> bool& { return c.transform.cropOn; });
    m_crop[0] = addSlider(crop, "cropLeft", T("Links"), 0, 960, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropLeft; },
                          AnimParam::CropLeft);
    m_crop[1] = addSlider(crop, "cropRight", T("Rechts"), 0, 960, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropRight; },
                          AnimParam::CropRight);
    m_crop[2] = addSlider(crop, "cropTop", T("Oben"), 0, 540, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropTop; },
                          AnimParam::CropTop);
    m_crop[3] = addSlider(crop, "cropBottom", T("Unten"), 0, 540, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropBottom; },
                          AnimParam::CropBottom);

    // ---- Composite ----
    Section comp = addSection(videoLay, V, "Composite", [](Clip& c) {
        c.transform.opacity = 100;
        c.transform.compositeOn = true;
        Keys::clear(c, AnimParam::Opacity);
    }, [](Clip& c) -> bool& { return c.transform.compositeOn; });
    addSlider(comp, "opacity", T("Deckkraft"), 0, 100, 100, 0.5, 2, [](Clip& c) -> double& { return c.transform.opacity; },
              AnimParam::Opacity);

    // ---- Green Screen (Effekt aus der EffectRegistry) ----
    Section key = addSection(videoLay, V, "Green Screen", [](Clip& c) {
        c.effects.removeIf([](const EffectInstance& x) { return x.effectId == "chromakey"; });
    }, [](Clip& c) -> bool& { return ensureEffect(c, "chromakey").enabled; });
    auto* swatch = new QToolButton;
    swatch->setFixedSize(46, 18);
    swatch->setFocusPolicy(Qt::NoFocus);
    swatch->setToolTip(T("Key-Farbe wählen – im Dialog gibt es eine Pipette zum Picken im Viewer"));
    connect(swatch, &QToolButton::clicked, this, [this] {
        const Clip* c = primary(TrackKind::Video);
        if (!c) return;
        const QColor col = QColorDialog::getColor(effectParam(*c, "chromakey", "color").value<QColor>(), this,
                                                  T("Key-Farbe wählen"));
        if (!col.isValid()) return;
        apply(TrackKind::Video, {}, T("Key-Farbe"), [col](Clip& clip) {
            EffectInstance& e = ensureEffect(clip, "chromakey");
            e.params["color"] = col;
            e.enabled = true;
        });
    });
    m_refreshers << [this, swatch] {
        if (const Clip* c = primary(TrackKind::Video)) {
            const QColor col = effectParam(*c, "chromakey", "color").value<QColor>();
            swatch->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                      .arg(col.name(), Theme::border.name()));
        }
    };
    auto* colorBox = new QHBoxLayout;
    colorBox->setContentsMargins(0, 0, 0, 0);
    colorBox->addStretch(1);
    colorBox->addWidget(swatch);
    key.grid->addWidget(rowLabel(T("Farbe")), key.rows, 0);
    key.grid->addLayout(colorBox, key.rows, 1);
    ++key.rows;
    // Registry speichert 0..1, angezeigt wird Prozent; eigenes onChange schreibt in den Effekt
    Param* tol = addSlider(key, "keyTolerance", T("Toleranz"), 0, 100, 30, 0.5, 2, [](Clip& c) -> double& {
        static double shown;
        shown = effectParam(c, "chromakey", "distance").toDouble() * 100.0;
        return shown;
    });
    tol->edit->onChange = [this, tol](double v) {
        const QSignalBlocker b(tol->slider);
        tol->slider->setValue(sliderPos(v, tol->min, tol->max));
        apply(TrackKind::Video, "keyTolerance", T("Key-Toleranz"), [v](Clip& c) {
            EffectInstance& e = ensureEffect(c, "chromakey");
            e.params["distance"] = v / 100.0;
            e.enabled = true;
        });
    };
    videoLay->addStretch(1);

    // ---- Effects tab: effects from the Effects Library (Open FX) and the Color page grade ----
    auto* fxBar = new QWidget;
    auto* fxBarLay = new QHBoxLayout(fxBar);
    fxBarLay->setContentsMargins(10, 6, 6, 6);
    auto* fxHint = new QLabel(T("Entfernen: Papierkorb am Effekt"));
    fxHint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    auto* removeAll = new QPushButton(T("Alle entfernen"));
    removeAll->setFocusPolicy(Qt::NoFocus);
    connect(removeAll, &QPushButton::clicked, this,
            [this] { m_editor->removeAllEffects(selectedIds(TrackKind::Video)); });
    fxBarLay->addWidget(fxHint, 1);
    fxBarLay->addWidget(removeAll);
    fxLay->addWidget(fxBar);
    m_gradeRow = new QWidget;
    auto* gradeLay = new QHBoxLayout(m_gradeRow);
    gradeLay->setContentsMargins(10, 4, 6, 4);
    m_gradeText = new QLabel;
    m_gradeText->setWordWrap(true);
    auto* gradeTrash = new QToolButton;
    gradeTrash->setIcon(trashIcon());
    gradeTrash->setIconSize(QSize(14, 14));
    gradeTrash->setAutoRaise(true);
    gradeTrash->setFocusPolicy(Qt::NoFocus);
    gradeTrash->setToolTip(T("Effekt entfernen"));
    gradeTrash->setStyleSheet("QToolButton { border: none; background: transparent; }");
    connect(gradeTrash, &QToolButton::clicked, this,
            [this] { m_editor->removeEffect(selectedIds(TrackKind::Video, false, "grade"), "grade"); });
    gradeLay->addWidget(m_gradeText, 1);
    gradeLay->addWidget(gradeTrash);
    m_gradeRow->setStyleSheet(QString("QWidget { background: %1; }").arg(Theme::panel.name()));
    fxLay->addWidget(m_gradeRow);
    m_fxLay = fxLay;
    m_fxIndex = fxLay->count(); // sections are created once a clip has the effect (arrangeEffectSections)
    fxLay->addStretch(1);

    // ---- Audio ----
    const TrackKind A = TrackKind::Audio;
    Section vol = addSection(audioLay, A, T("Lautstärke"), [](Clip& c) {
        c.volumeDb = 0;
        c.pan = 0;
        Keys::clear(c, AnimParam::Volume);
        Keys::clear(c, AnimParam::Pan);
    });
    Param* volume = addSlider(vol, "volume", T("Lautstärke"), kMinVolumeDb, kMaxVolumeDb, 0, 0.1, 2,
                              [](Clip& c) -> double& { return c.volumeDb; }, AnimParam::Volume);
    volume->edit->setMinimumText("-∞");
    addSlider(vol, "pan", "Pan", -100, 100, 0, 1, 2, [](Clip& c) -> double& { return c.pan; }, AnimParam::Pan);
    // Ton-Stream der Datei (OBS: Desktop-Ton, Mikro … in einer Datei); nur sichtbar, wenn es mehrere gibt
    auto* streamLabel = rowLabel(T("Quell-Tonspur"));
    auto* streamBox = new QComboBox;
    streamBox->setFocusPolicy(Qt::ClickFocus);
    streamBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    streamBox->setMinimumContentsLength(8);
    streamBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(streamBox, &QComboBox::activated, this, [this, streamBox, A](int stream) {
        const Project* project = m_editor->project();
        apply(A, {}, T("Quell-Tonspur"), [project, stream](Clip& c) {
            const MediaInfo* m = project->mediaInfo(c.mediaPath);
            if (m && stream < m->audioStreamCount()) c.audioStream = stream; // andere Dateien ohne diesen Stream bleiben
        });
    });
    m_refreshers << [this, streamBox, streamLabel] {
        const Clip* c = primary(TrackKind::Audio);
        const MediaInfo* m = c && !c->mediaPath.isEmpty() ? m_editor->project()->mediaInfo(c->mediaPath) : nullptr;
        const int n = m ? m->audioStreamCount() : 0;
        streamBox->setVisible(n > 1);
        streamLabel->setVisible(n > 1);
        if (n <= 1) return;
        const QSignalBlocker b(streamBox);
        streamBox->clear();
        for (int k = 0; k < n; ++k)
            streamBox->addItem(QString("%1: %2").arg(k + 1).arg(m->audioStreamName(k)));
        streamBox->setCurrentIndex(std::clamp(c->audioStream, 0, n - 1));
    };
    vol.grid->addWidget(streamLabel, vol.rows, 0);
    vol.grid->addWidget(streamBox, vol.rows++, 1, 1, 2);
    audioLay->addStretch(1);

    // ---- Gesamtaufbau ----
    m_clipName = new QLabel;
    m_clipName->setAlignment(Qt::AlignCenter);
    m_clipName->setContentsMargins(8, 4, 8, 4);
    m_clipName->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    m_content = new QWidget;
    auto* cl = new QVBoxLayout(m_content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(tabBar);
    cl->addWidget(m_clipName);
    cl->addWidget(m_pages, 1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(m_content, 1);
    lay->addWidget(m_empty, 1);

    m_tabs->button(0)->setChecked(true);
    connect(editor->selection(), &Selection::changed, this, &Inspector::refresh);
    connect(editor->project(), &Project::timelineChanged, this, &Inspector::refresh);
    refresh();
}

Inspector::~Inspector()
{
    qDeleteAll(m_params);
}

void Inspector::setFrameSize(const QSize& size)
{
    m_frameSize = size;
    for (int i = 0; i < 4; ++i) {
        const double max = (i < 2 ? size.width() : size.height()) / 2.0;
        m_crop[i]->max = max;
        m_crop[i]->edit->setRange(0, max);
    }
    refresh();
}

int Inspector::localFrame(const Clip& c) const
{
    return std::clamp(m_playhead - c.start, 0, std::max(0, c.length() - 1));
}

void Inspector::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    m_playhead = frame;
    // Nur neu anzeigen, wenn sich etwas ändern kann (beim Abspielen jedes Frame)
    const Clip* shown[] = {primary(TrackKind::Video), primary(TrackKind::Audio)};
    if (std::any_of(std::begin(shown), std::end(shown), [](const Clip* c) { return c && Keys::hasKeys(*c); })) refresh();
}

QVector<int> Inspector::selectedIds(TrackKind kind, bool title, const QString& effect) const
{
    QVector<int> ids;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : m_editor->selection()->ids()) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == kind && (!title || c->isTitle()) && (effect.isEmpty() || EffectRegistry::has(*c, effect)))
            ids << id;
    }
    return ids;
}

const Clip* Inspector::primary(TrackKind kind, bool title, const QString& effect) const
{
    // Angezeigt wird der früheste ausgewählte Clip der Art
    const Clip* best = nullptr;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : selectedIds(kind, title, effect)) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (c && (!best || c->start < best->start)) best = c;
    }
    return best;
}

void Inspector::apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn,
                      bool title, const QString& effect)
{
    // key leer = eigener Undo-Schritt, sonst werden Ziehbewegungen zusammengefasst
    m_editor->modifyClips(selectedIds(kind, title, effect), text, fn, key.isEmpty() ? QString() : "inspector:" + key);
}

void Inspector::refresh()
{
    m_tabs->button(kSubtitlePage)->setVisible(false);
    if (refreshTransition()) return;
    if (refreshSubtitle()) return;
    const Clip* v = primary(TrackKind::Video);
    const Clip* a = primary(TrackKind::Audio);
    const bool any = v || a;
    m_content->setVisible(any);
    m_empty->setVisible(!any);
    if (!any) return;

    const Clip* shown = v ? v : a;
    // Count linked video + audio clips once (a recording with 5 audio streams is one clip, not "+5")
    QSet<int> groups;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : m_editor->selection()->ids())
        if (const Clip* c = TimelineOps::findClip(tl, id)) groups.insert(c->linkId ? -c->linkId : c->id);
    const int count = int(groups.size());
    QString name = m_editor->project()->clipName(*shown); // Compound Clips: Name der Sequenz
    if (count > 1) name += QString("  (+%1)").arg(count - 1);
    m_clipName->setText(name);
    m_clipName->setToolTip(shown->mediaPath);

    // Tabs ohne passenden Clip ausgrauen wie in DaVinci; Titel-Tab nur bei Titelclips
    const Clip* t = primary(TrackKind::Video, true);
    m_tabs->button(0)->setEnabled(v);
    m_tabs->button(1)->setEnabled(a);
    m_tabs->button(kTitlePage)->setEnabled(t);
    m_tabs->button(kTitlePage)->setVisible(t);
    const int fx = v ? effectCount(*v) : 0;
    m_tabs->button(kEffectsPage)->setText(QString("fx\n%1").arg(fx ? T("Effekte (%1)").arg(fx) : T("Effekte")));
    m_tabs->button(kEffectsPage)->setVisible(fx > 0);
    int page = m_pages->currentIndex();
    if (t && shown->id != m_lastShownId) page = kTitlePage; // neu ausgewählter Titel -> Tab T("Titel")
    if (v && v->id == m_lastShownId && fx > m_fxCount) page = kEffectsPage; // effect just added: show it
    m_lastShownId = shown->id;
    m_fxCount = fx;
    if ((page == 0 && !v) || (page == 1 && !a) || (page == kTitlePage && !t) || (page == kEffectsPage && !fx))
        page = v ? 0 : 1;
    m_tabs->button(page)->setChecked(true);
    m_pages->setCurrentIndex(page);

    arrangeEffectSections();
    for (Param* p : m_params) {
        const Clip* c = p->title ? t : p->kind == TrackKind::Video ? v : a;
        if (c && !p->effect.isEmpty() && !EffectRegistry::has(*c, p->effect)) c = nullptr; // Bereich ausgeblendet
        if (!c) continue;
        Clip copy = *c;
        const double val = p->anim ? Keys::valueAt(copy, *p->anim, localFrame(copy)) : p->field(copy);
        p->edit->setValue(val);
        if (p->slider) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(val, p->min, p->max));
        }
    }
    for (const auto& fn : m_refreshers) fn();
}
