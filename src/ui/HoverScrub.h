#pragma once
#include <QImage>
#include <QObject>
#include <QSize>
#include <functional>

class QListWidget;
class QListWidgetItem;
class ScrubFrames;

// Hover scrub over the thumbnails of a list (Media Storage, Media Pool): moving the mouse across a video's thumbnail
// shows the picture at that point of the file (left edge = start, right edge = end) with a thin position bar. The
// pictures come from ScrubFrames in the background; the window never waits, until one is there the thumbnail stays.
// Only in the icon view, not while a button is held (dragging).
class HoverScrub : public QObject {
    Q_OBJECT
public:
    // fileOf: video file of an entry, empty = nothing to scrub (folder, audio, still image, title …)
    // show: picture of `item` at `fraction` (0..1, null picture = none decoded yet: thumbnail with the position bar);
    //       fraction < 0 = the mouse left: the normal thumbnail again
    using FileOf = std::function<QString(QListWidgetItem*)>;
    using Show = std::function<void(QListWidgetItem* item, const QImage& picture, double fraction)>;
    HoverScrub(QListWidget* list, const QSize& thumbSize, FileOf fileOf, Show show);

    void setEnabled(bool on); // off: the entry under the mouse goes back to normal
    // The list is about to be rebuilt: forget the entry under the mouse (its pointer dies) without touching it
    void forget();
    QListWidgetItem* item() const { return m_item; }
    ScrubFrames* frames() const { return m_frames; }
    // As if the mouse stood at `pos` in the list's viewport resp. left it (also for tests)
    void hover(const QPoint& pos);
    void leave();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void refresh();

    QListWidget* m_list;
    ScrubFrames* m_frames;
    FileOf m_fileOf;
    Show m_show;
    bool m_enabled = true;
    QListWidgetItem* m_item = nullptr;
    QString m_path;
    int m_slot = -1;
    double m_fraction = 0;
};
