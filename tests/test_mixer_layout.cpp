// SPDX-License-Identifier: AGPL-3.0-only
// The Mixer panel fills its room: no empty column right of the loudness meter, no gaps between the channel strips,
// master and loudness, all of them down to the bottom. Measured on the widget geometry at several panel widths and
// track counts, in a column below another panel (the Edit page: below the Inspector, MainWindow::updateRightColumn)
// and beside the timeline in a side-by-side splitter, with the normal and a large UI font.
// Came back once already: with the mixer moved into the Inspector's column (0.20) its width followed the Inspector's
// and the strips kept their fixed width -> an empty area as wide as two strips beside the loudness meter.
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

void checkFilled(Mixer& mixer, const QString& where)
{
    const QVector<QRect> rects = visibleStrips(mixer);
    if (!Check::report(rects.size() >= 3, "strips shown", __FILE__, __LINE__, where)) return;
    // Up to the right edge: no empty column beside the loudness meter
    const int rightGap = mixer.width() - 1 - rects.last().right();
    Check::report(rightGap <= 2, "no empty area on the right", __FILE__, __LINE__,
                  QString("%1: %2 px empty right of the loudness meter (mixer %3 px)").arg(where).arg(rightGap).arg(mixer.width()));
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
        for (const int width : {300, 360, 452, 520, 700, 900}) {
            host.resize(width, 720);
            settle();
            const QString where = QString("column %1 px, %2 tracks, %3 font").arg(width).arg(tracks).arg(font);
            CHECK_EQ(mixer->width(), width); // the column decides, the mixer follows
            checkFilled(*mixer, where);
            dump(&host, QString("column-%1-%2-%3").arg(width).arg(tracks).arg(font));
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
