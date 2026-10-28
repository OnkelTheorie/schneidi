// SPDX-License-Identifier: AGPL-3.0-only
// Contact sheet (core/ContactSheet.h, File › Save Contact Sheet, `frames --sheet`): frames spread evenly, the grid
// (automatic and chosen columns, tiles of different sizes fitted into one cell size), the dialog (remembered
// settings, file suffix, In/Out only when there is a range), a sheet of a real timeline (Snapshot::contactSheet).
#include "app/ContactSheetDialog.h"
#include "core/ContactSheet.h"
#include "engine/Snapshot.h"

#include "check.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("contact-sheet");
    QSettings().remove("sheet");

    // Middle of each of n equal parts
    CHECK_EQ(ContactSheet::spread(0, 100, 4), QVector<int>({12, 37, 62, 87}));
    CHECK_EQ(ContactSheet::spread(50, 60, 1), QVector<int>({55}));
    CHECK(ContactSheet::spread(0, 100, 0).isEmpty());

    const auto tiles = [](int n, QSize size) {
        QVector<QImage> images;
        QStringList labels;
        for (int i = 0; i < n; ++i) {
            QImage img(size, QImage::Format_RGB32);
            img.fill(Qt::red);
            images << img;
            labels << QString("00:00:0%1:00").arg(i);
        }
        return std::pair{images, labels};
    };
    // Automatic columns: 2 up to 4 pictures, 3 up to 9, then 4
    auto [four, l4] = tiles(4, {160, 90});
    CHECK_EQ(ContactSheet::compose(four, l4).size(), QSize(320, 180));
    auto [twelve, l12] = tiles(12, {160, 90});
    CHECK_EQ(ContactSheet::compose(twelve, l12).size(), QSize(640, 270));
    CHECK_EQ(ContactSheet::compose(twelve, l12, 6).size(), QSize(960, 180));
    // A portrait tile shares the largest cell
    four[1] = QImage(90, 160, QImage::Format_RGB32);
    four[1].fill(Qt::blue);
    CHECK_EQ(ContactSheet::compose(four, l4).size(), QSize(320, 320));
    CHECK(ContactSheet::compose({}, {}).isNull());

    // Dialog: suggestion in the given folder, suffix added, settings remembered on accept
    QTemporaryDir dir;
    {
        ContactSheetDialog d("Film", dir.path(), false, nullptr);
        CHECK(d.path().startsWith(dir.path() + "/Film"));
        CHECK(d.path().endsWith(".jpg"));
        CHECK(!d.inOut());
        const QList<QComboBox*> combos = d.findChildren<QComboBox*>();
        CHECK_EQ(combos.size(), 3); // columns, size, range (only "whole timeline")
        CHECK_EQ(combos.last()->count(), 1);
        d.findChild<QLineEdit*>()->setText(dir.filePath("sheet"));
        CHECK_EQ(d.path(), dir.filePath("sheet.jpg"));
        combos[0]->setCurrentIndex(combos[0]->findData(5));
        d.accept();
        CHECK_EQ(d.result(), int(QDialog::Accepted));
    }
    {
        ContactSheetDialog d("Film", "/somewhere/else", true, nullptr);
        CHECK_EQ(d.columns(), 5);
        CHECK(d.path().startsWith(dir.path())); // remembered folder
        CHECK_EQ(d.findChildren<QComboBox*>().last()->count(), 2);
        CHECK_EQ(d.findChild<QLineEdit*>()->text().endsWith(".png") ? 1 : 0, 0);
    }

    // A sheet of a timeline with a real clip: tiles exactly as wide as asked, picture (not black), cancel
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
    const QString media = Check::makeMedia(dir.filePath("a.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2",
                                                                   "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    ProjectFormat f;
    f.width = 320;
    f.height = 180;
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 49;
    tl.video[0].clips << c;
    QVector<int> asked;
    const QImage sheet = Snapshot::contactSheet(f, tl, ContactSheet::spread(0, 50, 6), 160, 0, [&](int done) {
        asked << done;
        return true;
    });
    CHECK_EQ(sheet.size(), QSize(3 * 160, 2 * 90));
    CHECK_EQ(asked, QVector<int>({0, 1, 2, 3, 4, 5, 6}));
    const QColor middle = sheet.pixelColor(80, 45);
    CHECK(middle.red() + middle.green() + middle.blue() > 60); // testsrc2 is colorful, not black
    CHECK(Snapshot::contactSheet(f, tl, {10, 20}, 160, 0, [](int done) { return done < 1; }).isNull());
    if (const QByteArray dump = qgetenv("SHEET_DUMP"); !dump.isEmpty()) sheet.save(QString::fromLocal8Bit(dump));
    return Check::result();
}
