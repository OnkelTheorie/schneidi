#include "ui/MulticamView.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/MulticamGrid.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <algorithm>

namespace {

constexpr int kLayouts[] = {0, 2, 3, 4}; // combo box index -> columns (0 = auto)

// Part of `kind` of the multicam clip `c` (itself or a linked part of the same multicam clip) covering `frame`
const Clip* partAt(const Timeline& tl, const Clip& c, TrackKind kind, int frame)
{
    for (int id : TimelineOps::linkedGroup(tl, c.id)) {
        TrackRef ref;
        const Clip* x = TimelineOps::findClip(tl, id, &ref);
        if (x && ref.kind == kind && x->isMulticam() && x->sequenceId == c.sequenceId && x->start <= frame
            && frame < x->end())
            return x;
    }
    return nullptr;
}

} // namespace

MulticamView::MulticamView(Project* project, Selection* selection, Editor* editor, QWidget* parent)
    : QWidget(parent), m_project(project), m_selection(selection), m_editor(editor)
{
    setObjectName("MulticamView");
    setMinimumSize(320, 200);
    setMouseTracking(false);

    m_bar = new QWidget(this);
    m_bar->setObjectName("MulticamBar");
    m_bar->setAttribute(Qt::WA_StyledBackground);
    m_bar->setStyleSheet(QString("#MulticamBar { background: %1; }").arg(Theme::panel.name()));
    auto* lay = new QHBoxLayout(m_bar);
    lay->setContentsMargins(4, 2, 4, 2);
    lay->setSpacing(2);

    auto makeButton = [this, lay](const QString& name, const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b);
        return b;
    };
    // Like DaVinci's multicam viewer: what a cut/switch changes
    m_both = makeButton("multicam_mode_both", T("Video + Audio"), T("Schnitte und Wechsel ändern Bild und Ton"));
    m_video = makeButton("multicam_mode_video", T("Video"), T("Schnitte und Wechsel ändern nur das Bild"));
    m_audio = makeButton("multicam_mode_audio", T("Audio"), T("Schnitte und Wechsel ändern nur den Ton"));
    auto* modes = new QButtonGroup(this);
    for (QToolButton* b : {m_both, m_video, m_audio}) {
        b->setCheckable(true);
        modes->addButton(b);
    }
    m_both->setChecked(true);
    connect(m_both, &QToolButton::clicked, this, [this] { setMode(Multicam::Mode::Both); });
    connect(m_video, &QToolButton::clicked, this, [this] { setMode(Multicam::Mode::Video); });
    connect(m_audio, &QToolButton::clicked, this, [this] { setMode(Multicam::Mode::Audio); });
    lay->addStretch(1);

    m_layout = new QComboBox;
    m_layout->setObjectName("multicam_layout");
    m_layout->setFocusPolicy(Qt::NoFocus);
    m_layout->setToolTip(T("Raster: automatisch nach Anzahl der Winkel oder fest"));
    m_layout->addItem(T("Automatisch"));
    for (int c : {2, 3, 4}) m_layout->addItem(QString("%1×%1").arg(c));
    connect(m_layout, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0 && i < int(std::size(kLayouts))) setColumns(kLayouts[i]);
    });
    lay->addWidget(m_layout);
    m_prev = makeButton("multicam_page_prev", "◀", T("Vorherige Seite"));
    m_pageLabel = new QLabel;
    m_pageLabel->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_pageLabel);
    m_next = makeButton("multicam_page_next", "▶", T("Nächste Seite"));
    connect(m_prev, &QToolButton::clicked, this, [this] { setPage(m_page - 1); });
    connect(m_next, &QToolButton::clicked, this, [this] { setPage(m_page + 1); });

    connect(m_project, &Project::timelineChanged, this, &MulticamView::refresh);
    connect(m_project, &Project::currentSequenceChanged, this, &MulticamView::refresh);
    connect(m_project, &Project::sequencesChanged, this, [this] {
        refresh();
        if (m_sequence) emit gridChanged(); // the multicam clip's content may have changed (resync, new angle)
        update();
    });
    connect(m_selection, &Selection::changed, this, &MulticamView::refresh);
    refresh();
}

int MulticamView::barHeight() const
{
    return m_bar->sizeHint().height();
}

void MulticamView::resizeEvent(QResizeEvent*)
{
    m_bar->setGeometry(0, 0, width(), barHeight());
}

QRect MulticamView::gridRect() const
{
    const QRect area(0, barHeight(), width(), std::max(1, height() - barHeight()));
    QSize frame = m_project->format().size();
    if (frame.isEmpty()) frame = QSize(16, 9);
    return MulticamGrid::fit(area, frame);
}

void MulticamView::setPosition(int frame)
{
    m_position = frame;
    refresh();
}

void MulticamView::setPlaying(bool playing)
{
    m_playing = playing;
}

void MulticamView::setFrame(const QImage& grid)
{
    m_frame = grid;
    update();
}

void MulticamView::setMode(Multicam::Mode mode)
{
    m_mode = mode;
    {
        const QSignalBlocker b1(m_both), b2(m_video), b3(m_audio);
        (mode == Multicam::Mode::Both ? m_both : mode == Multicam::Mode::Video ? m_video : m_audio)->setChecked(true);
    }
    refresh();
}

void MulticamView::setColumns(int columns)
{
    columns = columns <= 0 ? 0 : std::clamp(columns, 2, MulticamGrid::kMaxColumns);
    if (columns == m_columns) return;
    m_columns = columns;
    {
        const QSignalBlocker block(m_layout);
        const int* it = std::find(std::begin(kLayouts), std::end(kLayouts), columns);
        m_layout->setCurrentIndex(int(it - std::begin(kLayouts)));
    }
    // Stay on the active angle's page
    m_page = std::clamp(m_active >= 0 ? m_active / perPage() : m_page, 0, pageCount() - 1);
    updateControls();
    update();
    emit gridChanged();
}

void MulticamView::setPage(int page)
{
    page = std::clamp(page, 0, pageCount() - 1);
    if (page == m_page) return;
    m_page = page;
    updateControls();
    update();
    emit gridChanged();
}

int MulticamView::pageCount() const
{
    return MulticamGrid::pages(angleCount(), m_columns);
}

int MulticamView::perPage() const
{
    return MulticamGrid::perPage(angleCount(), m_columns);
}

const Clip* MulticamView::currentClip() const
{
    return Multicam::clipAt(m_project->timeline(), m_position, m_selection->ids());
}

int MulticamView::angleCount() const
{
    const Sequence* s = m_sequence ? m_project->sequence(m_sequence) : nullptr;
    return s ? int(s->angles.size()) : 0;
}

QString MulticamView::angleName(int angle) const
{
    const Sequence* s = m_sequence ? m_project->sequence(m_sequence) : nullptr;
    return s && angle >= 0 && angle < s->angles.size() ? s->angles[angle].name : QString();
}

QString MulticamView::message() const
{
    return m_sequence ? QString() : T("Kein Multicam-Clip am Playhead");
}

void MulticamView::refresh()
{
    const Timeline& tl = m_project->timeline();
    const Clip* c = currentClip();
    const Sequence* s = c ? m_project->sequence(c->sequenceId) : nullptr;
    const int sequence = s ? s->id : 0;
    const int offset = s ? m_position - Multicam::sequenceFrame(*c, m_position) : 0;
    int active = -1, sound = -1;
    if (s) {
        const Clip* video = partAt(tl, *c, TrackKind::Video, m_position);
        const Clip* audio = partAt(tl, *c, TrackKind::Audio, m_position);
        active = m_mode == Multicam::Mode::Audio ? (audio ? audio->angle : c->angle) : (video ? video->angle : c->angle);
        sound = audio ? audio->angle : c->angle;
    }
    const bool gridMoved = sequence != m_sequence || offset != m_offset;
    m_sequence = sequence;
    m_offset = offset;
    // The active angle changed (key, click, menu, playback into the next segment): show its page
    if (active != m_active && active >= 0) m_page = active / perPage();
    m_page = std::clamp(m_page, 0, pageCount() - 1);
    const bool activeChanged = active != m_active;
    m_active = active;
    const bool soundChanged = sound != m_sound;
    m_sound = sound;
    updateControls();
    if (gridMoved || activeChanged) update();
    if (gridMoved) emit gridChanged();
    else if (soundChanged && sound >= 0) emit soundAngleChanged(sound);
}

void MulticamView::updateControls()
{
    const int pages = pageCount();
    m_prev->setEnabled(m_page > 0);
    m_next->setEnabled(m_page < pages - 1);
    m_pageLabel->setText(pages > 1 ? T("Seite %1/%2").arg(m_page + 1).arg(pages) : QString());
}

void MulticamView::cutToAngle(int angle)
{
    if (!m_sequence || angle < 0 || angle >= angleCount()) return;
    m_editor->multicamCut(m_position, angle, m_mode);
    refresh();
}

void MulticamView::switchToAngle(int angle)
{
    if (!m_sequence || angle < 0 || angle >= angleCount()) return;
    m_editor->multicamSwitch(m_position, angle, m_mode);
    refresh();
}

void MulticamView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    const int angle = MulticamGrid::angleAt(gridRect(), angleCount(), m_page, e->pos(), m_columns);
    if (angle < 0) return;
    // Like DaVinci: while playing a click cuts, while stopped it switches the segment (Alt+click: cut)
    if (m_playing || (e->modifiers() & Qt::AltModifier)) cutToAngle(angle);
    else switchToAngle(angle);
}

void MulticamView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::viewerBg);
    const QRect grid = gridRect();
    p.fillRect(grid, Qt::black);
    if (!m_sequence) {
        p.setPen(Theme::textDim);
        p.drawText(grid, Qt::AlignCenter | Qt::TextWordWrap, message());
        return;
    }
    if (!m_frame.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(grid, m_frame);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    }
    const int n = angleCount(), per = perPage();
    QFont font = p.font();
    font.setPixelSize(std::clamp(grid.height() / (MulticamGrid::columns(n, m_columns) * 9), 10, 14));
    p.setFont(font);
    const QFontMetrics fm(font);
    for (int i = 0; i < per; ++i) {
        const int angle = m_page * per + i;
        const QRect cell = MulticamGrid::cell(grid, n, i, m_columns);
        if (cell.isNull()) continue;
        // Thin separators between the cells
        p.fillRect(QRect(cell.left(), cell.bottom(), cell.width(), 1), Theme::viewerBg);
        p.fillRect(QRect(cell.right(), cell.top(), 1, cell.height()), Theme::viewerBg);
        if (angle >= n) continue;
        // Number (= key) and name in the top left corner
        const QString label = QString("%1  %2").arg(i + 1).arg(angleName(angle));
        const QString text = fm.elidedText(label, Qt::ElideRight, std::max(10, cell.width() - 16));
        const QRect box(cell.left() + 4, cell.top() + 4, fm.horizontalAdvance(text) + 10, fm.height() + 4);
        p.fillRect(box, Theme::alpha(Theme::window, 190));
        p.setPen(Theme::text);
        p.drawText(box, Qt::AlignCenter, text);
        if (angle == m_active) { // red frame inside the cell
            const int w = 3;
            const QColor red = Theme::multicamActive;
            p.fillRect(QRect(cell.left(), cell.top(), cell.width(), w), red);
            p.fillRect(QRect(cell.left(), cell.bottom() - w + 1, cell.width(), w), red);
            p.fillRect(QRect(cell.left(), cell.top(), w, cell.height()), red);
            p.fillRect(QRect(cell.right() - w + 1, cell.top(), w, cell.height()), red);
        }
    }
}
