// SPDX-License-Identifier: AGPL-3.0-only
// The Mixer panel's strips keep their natural width and fill the room they are given without gaps:
// - channel strips are never stretched (all as wide as one strip, the master 2 px more): a wide panel does not pull
//   them apart, at most the loudness meter takes a little more (up to one strip), the rest stays plain on the right;
// - at its natural width (Mixer::naturalWidth: all strips + master + loudness) nothing is empty; narrower, whole strips
//   scroll and the loudness meter takes the leftover (less than a strip) - no empty column right of it;
// - no gaps between strips, master and loudness, all down to the bottom.
// Measured at several panel widths and track counts, in a column below another panel (the Edit page: below the
// Inspector, MainWindow::updateRightColumn) and beside the timeline, with the normal and a large UI font.
// History: 0.20 put the mixer into the Inspector's column and left an empty area as wide as two strips beside the
// loudness meter; 0.21's first fix stretched the strips instead (ugly) - the column is now only as wide as needed
// (test_edit_layout) and the strips stay as they are.
// MIXER_LAYOUT_DUMP=<folder>: a picture per case.

#include "check.h"

#include "core/Project.h"
#include "engine/Engine.h"
#include "ui/Mixer.h"

#include <QApplication>
#include <QScrollArea>
#include <QSplitter>
#include <QTest>
#include <QVBoxLayout>
#include <algorithm>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

void setAudioTracks(Project& p, int count)
{
    p.edit("tracks", [count](Timeline& tl) {
        while (tl.audio.size() < count) tl.audio.append(Track{TrackKind::Audio});
        while (tl.audio.size() > count) tl.audio.removeLast();
    });
    QApplication::processEvents();
}

// The strips (channel strips, master, loudness: all "Strip") the user sees, in mixer coordinates, clipped to what is
// visible of the strip area
QVector<QRect> visibleStrips(Mixer& mixer)
{
    QVector<QRect> rects;
    auto* scroll = mixer.findChild<QScrollArea*>();
    const QRect viewport = scroll ? QRect(scroll->viewport()->mapTo(&mixer, QPoint(0, 0)), scroll->viewport()->size())
                                  : QRect();
    for (QWidget* s : mixer.findChildren<QWidget*>("Strip")) {
        if (!s->isVisible()) continue;
        QRect r(s->mapTo(&mixer, QPoint(0, 0)), s->size());
        if (scroll && scroll->isAncestorOf(s)) r &= viewport;
        if (!r.isEmpty()) rects << r;
    }
    std::sort(rects.begin(), rects.end(), [](const QRect& a, const QRect& b) { return a.left() < b.left(); });
    return rects;
}

constexpr int kStrip = 70; // a channel strip's natural width (Mixer.cpp kStripW)

// The channel strips (in the scroll area) never stretched
void checkNatural(Mixer& mixer, const QString& where)
{
    auto* scroll = mixer.findChild<QScrollArea*>();
    for (QWidget* s : mixer.findChildren<QWidget*>("Strip"))
        if (scroll && scroll->isAncestorOf(s))
            Check::report(s->width() == kStrip, "channel strip at its natural width", __FILE__, __LINE__,
                          QString("%1: %2 px").arg(where).arg(s->width()));
}

void checkFilled(Mixer& mixer, const QString& where)
{
    checkNatural(mixer, where);
    const QVector<QRect> rects = visibleStrips(mixer);
    if (!Check::report(rects.size() >= 3, "strips shown", __FILE__, __LINE__, where)) return;
    // The loudness meter: the rightmost strip
    QWidget* loudness = nullptr;
    for (QWidget* w : mixer.findChildren<QWidget*>("Strip"))
        if (w->isVisible() && (!loudness || w->mapTo(&mixer, QPoint()).x() > loudness->mapTo(&mixer, QPoint()).x()))
            loudness = w;
    if (!CHECK(loudness)) return;
    const int loudnessMin = loudness->minimumSizeHint().width();
    // It takes at most one strip more than it needs
    Check::report(loudness->width() <= loudnessMin + kStrip + 1, "loudness meter not stretched", __FILE__, __LINE__,
                  QString("%1: loudness %2 px, needs %3").arg(where).arg(loudness->width()).arg(loudnessMin));
    // Up to the right edge, unless the panel is more than a strip wider than the strips need (plain room on the right)
    const int needed = loudness->mapTo(&mixer, QPoint()).x() + loudnessMin;
    const int rightGap = mixer.width() - 1 - rects.last().right();
    const int allowed = std::max(2, mixer.width() - needed - kStrip);
    Check::report(rightGap <= allowed, "no empty area on the right", __FILE__, __LINE__,
                  QString("%1: %2 px empty right of the loudness meter (mixer %3 px, strips need %4 px)")
                      .arg(where).arg(rightGap).arg(mixer.width()).arg(needed));
    Check::report(rects.first().left() <= 1, "starts at the left edge", __FILE__, __LINE__,
                  QString("%1: first strip at x=%2").arg(where).arg(rects.first().left()));
    // No gaps between neighbours (strips, master, loudness)
    for (int i = 1; i < rects.size(); ++i) {
        const int gap = rects[i].left() - rects[i - 1].right() - 1;
        Check::report(gap <= 1, "no gap between strips", __FILE__, __LINE__,
                      QString("%1: %2 px between strip %3 and %4").arg(where).arg(gap).arg(i - 1).arg(i));
    }
    // Down to the bottom (a scrollbar under the strips may take the strip area's last pixels)
    for (const QRect& r : rects) {
        const int bottomGap = mixer.height() - 1 - r.bottom();
        Check::report(bottomGap <= 24, "down to the bottom", __FILE__, __LINE__,
                      QString("%1: %2 px empty below a strip").arg(where).arg(bottomGap));
    }
}

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("MIXER_LAYOUT_DUMP");
    if (!dir.isEmpty()) w->grab().save(dir + "/" + name + ".png");
}

// The Edit page's right column: a panel above, the mixer below, the column as wide as the user made it
void testColumn(Engine& engine, const QString& font)
{
    Project p;
    QWidget host;
    auto* lay = new QVBoxLayout(&host);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* column = new QSplitter(Qt::Vertical);
    column->addWidget(new QWidget); // the Inspector's place
    auto* mixer = new Mixer(&p, &engine);
    column->addWidget(mixer);
    column->setSizes({300, 420});
    lay->addWidget(column);
    host.show();
    for (const int tracks : {1, 2, 3, 5, 8}) {
        setAudioTracks(p, tracks);
        for (const int width : {300, 360, 452, 520, 700, 900, -1}) { // -1: the mixer's natural width
            host.resize(width > 0 ? width : mixer->naturalWidth(), 720);
            settle();
            const QString where = QString("column %1 px, %2 tracks, %3 font").arg(host.width()).arg(tracks).arg(font);
            CHECK_EQ(mixer->width(), host.width()); // the column decides, the mixer follows
            checkFilled(*mixer, where);
            dump(&host, QString("column-%1-%2-%3").arg(host.width()).arg(tracks).arg(font));
        }
    }
}

// Beside the timeline (a side-by-side splitter, as in older layouts and other pages): never wider than it needs,
// the timeline gets the rest; inside, filled as well
void testBeside(Engine& engine, const QString& font)
{
    Project p;
    QSplitter row(Qt::Horizontal);
    row.addWidget(new QWidget); // the timeline's place
    auto* mixer = new Mixer(&p, &engine);
    row.addWidget(mixer);
    row.setStretchFactor(0, 1);
    row.show();
    for (const int tracks : {1, 2, 5}) {
        setAudioTracks(p, tracks);
        for (const int width : {300, 520, 900}) {
            row.resize(1400, 500);
            row.setSizes({1400 - width, width});
            settle();
            checkFilled(*mixer, QString("beside %1 px, %2 tracks, %3 font").arg(width).arg(tracks).arg(font));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("mixer-layout");
    Engine engine;
    const QFont normal = QApplication::font();
    testColumn(engine, "normal");
    testBeside(engine, "normal");
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.4); // like QT_FONT_DPI=120 or a large system font
    QApplication::setFont(big);
    testColumn(engine, "large");
    testBeside(engine, "large");
    QApplication::setFont(normal);
    return Check::result();
}
