// Timeline: drag and drop from the media pool, file manager and effects library.
#include "ui/timeline/TimelineView.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/I18n.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QContextMenuEvent>
#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <array>
#include <tuple>
#include <cmath>
#include "ui/timeline/TimelineViewDetail.h"

using namespace TimelineViewDetail;

// ---------- Drag & Drop (Media Pool oder direkt aus dem Dateimanager) ----------

QStringList TimelineView::dropPaths(const QMimeData* mime) const
{
    if (mime->hasFormat(MediaPool::MimeType)) return {QString::fromUtf8(mime->data(MediaPool::MimeType))};
    QStringList paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    return paths;
}

bool TimelineView::dropRange(const QMimeData* mime, int* in, int* out)
{
    if (!mime->hasFormat(Viewer::RangeMimeType)) return false;
    const QStringList parts = QString::fromUtf8(mime->data(Viewer::RangeMimeType)).split(' ');
    if (parts.size() != 2) return false;
    *in = parts[0].toInt();
    *out = parts[1].toInt();
    return *out >= *in;
}

int TimelineView::dropTrackAt(int y) const
{
    // V2 und A2 gehören zusammen -> Index der Spur unter der Maus, egal ob Video oder Audio
    const auto all = rows();
    for (const Row& r : all)
        if (y >= r.y && y < r.y + r.h) return r.ref.index;
    const Timeline& tl = m_editor->project()->timeline();
    if (!all.isEmpty() && y >= all.last().y + all.last().h) return tl.audio.size(); // unterhalb: neue Spur
    return 0;
}

// Schnitt/Clipkante für einen Übergang unter der Maus: nächste Kante des Clips unter der Maus
// (oder bis 30 px daneben); links vom Schnitt = End on Edit, rechts = Start on Edit, nah dran = mittig.
std::optional<TimelineView::TransitionDrop> TimelineView::transitionDropAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != m_transDragKind || isLocked(*row)) return std::nullopt;
    const auto& clips = m_editor->project()->timeline().track(row->ref).clips;
    const double f = xToFrame(pos.x());
    std::optional<TransitionDrop> best;
    double bestDx = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const bool over = f >= c.start && f < c.end();
        const int prev = i > 0 && clips[i - 1].end() == c.start ? clips[i - 1].id : 0;
        const int next = i + 1 < clips.size() && clips[i + 1].start == c.end() ? clips[i + 1].id : 0;
        for (const auto& [frame, left, right] : {std::tuple{c.start, prev, c.id}, std::tuple{c.end(), c.id, next}}) {
            const double dx = pos.x() - frameToX(frame);
            if (!over && std::abs(dx) > 30) continue;
            if (best && std::abs(dx) >= std::abs(bestDx)) continue;
            TransitionDrop d;
            d.ref = row->ref;
            d.leftId = left;
            d.rightId = right;
            bestDx = dx;
            best = d;
        }
    }
    if (!best) return std::nullopt;
    best->style = m_transDragStyle;
    if (best->leftId && best->rightId) {
        const double w = m_editor->project()->fps() * m_view.pxPerFrame; // Standardlänge 1 s
        const double zone = std::max(4.0, w / 6);
        best->style.align = std::abs(bestDx) <= zone ? TransitionAlign::Center
                            : bestDx < 0             ? TransitionAlign::End
                                                     : TransitionAlign::Start;
    }
    const auto span = m_editor->previewTransitionAt(best->leftId, best->rightId, best->style);
    if (!span) return std::nullopt; // passt nicht (keine Handles)
    best->span = *span;
    return best;
}

int TimelineView::fxDropClipAt(const QPoint& pos) const
{
    const auto row = rowAt(pos.y());
    return row && row->ref.kind == TrackKind::Video && !m_editor->isTrackLocked(row->ref) ? clipAt(pos) : 0;
}

void TimelineView::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(EffectsLibrary::EffectMimeType)) {
        m_fxDragging = QString::fromUtf8(e->mimeData()->data(EffectsLibrary::EffectMimeType));
        m_fxDropClip = 0;
        e->acceptProposedAction();
        return;
    }
    if (e->mimeData()->hasFormat(EffectsLibrary::MimeType)) {
        m_transDragging = EffectsLibrary::parseTransition(e->mimeData()->data(EffectsLibrary::MimeType),
                                                          &m_transDragKind, &m_transDragStyle);
        if (m_transDragging) e->acceptProposedAction();
        return;
    }
    const QStringList paths = dropPaths(e->mimeData());
    if (paths.isEmpty()) return;
    m_dropItems.clear();
    for (const QString& path : paths) {
        if (path == MediaPool::TitleItem) { // Titel aus dem Media Pool: 5 s, nur Video
            m_dropItems << DropItem{5 * m_editor->project()->fps(), true, 0};
            continue;
        }
        if (const int seq = MediaPool::sequenceOfItem(path)) { // Timeline/Compound Clip aus dem Media Pool
            const Project* project = m_editor->project();
            const Sequence* s = project->sequence(seq);
            if (!s || !project->canNest(seq, project->currentSequence())) continue; // nie in sich selbst
            auto has = [](const QVector<Track>& tracks) {
                return std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return !t.clips.isEmpty(); });
            };
            const int len = TimelineOps::endFrame(s->timeline);
            if (len > 0) m_dropItems << DropItem{len, has(s->timeline.video), has(s->timeline.audio) ? 1 : 0};
            continue;
        }
        MediaInfo info;
        if (const MediaInfo* known = m_editor->project()->mediaInfo(path)) info = *known;
        else if (m_probe) info = m_probe(path);
        int in = 0, out = 0;
        if (dropRange(e->mimeData(), &in, &out)) info.length = out - in + 1; // Quell-In/Out aus dem Viewer
        if (info.length > 0 && (info.hasVideo || info.hasAudio))
            m_dropItems << DropItem{info.length, info.hasVideo, info.audioStreamCount()};
    }
    if (m_dropItems.isEmpty()) return;
    e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_fxDragging.isEmpty()) {
        m_fxDropClip = fxDropClipAt(e->position().toPoint());
        if (m_fxDropClip) e->acceptProposedAction();
        else e->ignore();
        update();
        return;
    }
    if (m_transDragging) {
        m_transDrop = transitionDropAt(e->position().toPoint());
        if (m_transDrop) e->acceptProposedAction();
        else e->ignore();
        update();
        return;
    }
    if (m_dropItems.isEmpty()) return;
    int total = 0;
    for (const DropItem& it : m_dropItems) total += it.length;
    const QPoint pos = e->position().toPoint();
    int frame = std::max(0, int(std::lround(xToFrame(pos.x()))));
    frame += snapDelta({frame, frame + total}, {});
    m_dropFrame = std::max(0, frame);
    m_dropTrack = dropTrackAt(pos.y());
    e->acceptProposedAction();
    update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropFrame = -1;
    m_dropItems.clear();
    m_transDragging = false;
    m_transDrop.reset();
    m_fxDragging.clear();
    m_fxDropClip = 0;
    update();
}

void TimelineView::dropEvent(QDropEvent* e)
{
    if (!m_fxDragging.isEmpty()) {
        // Wie DaVinci: nur auf den Clip unter der Maus (auch wenn andere ausgewählt sind)
        if (const int id = fxDropClipAt(e->position().toPoint())) {
            m_editor->addEffect({id}, m_fxDragging);
            e->acceptProposedAction();
        }
        m_fxDragging.clear();
        m_fxDropClip = 0;
        update();
        return;
    }
    if (m_transDragging) {
        if (const auto d = transitionDropAt(e->position().toPoint())) {
            m_editor->addTransitionAt(d->leftId, d->rightId, d->style);
            e->acceptProposedAction();
        }
        m_transDragging = false;
        m_transDrop.reset();
        update();
        return;
    }
    int in = 0, out = 0;
    const QStringList paths = dropPaths(e->mimeData());
    if (m_dropFrame >= 0 && paths.size() == 1 && dropRange(e->mimeData(), &in, &out))
        emit rangeDropRequested(paths.first(), in, out, m_dropFrame, m_dropTrack);
    else if (m_dropFrame >= 0)
        emit dropRequested(paths, m_dropFrame, m_dropTrack);
    m_dropFrame = -1;
    m_dropItems.clear();
    e->acceptProposedAction();
    update();
}
