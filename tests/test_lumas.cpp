// Test Verlaufsblenden (TransitionType::Luma): mitgelieferte Verläufe (alle erzeugbar, voller Wertebereich),
// eigene Bilder aus dem Transitions-Ordner, Cache-PNG für MLT (Größe, Umkehren), Drag-Daten "luma:<Bild>",
// Projektdatei (relativ bzw. im eigenen Ordner wiedergefunden) und Rendern über MLT (Tür horizontal: Mitte schon B,
// Ränder noch A; umgekehrt andersherum; ins Leere wie Cross Dissolve).
#include "check.h"

#include "core/EffectFolders.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/TimelineOps.h"
#include "engine/Lumas.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "ui/EffectsLibrary.h"

#include <Mlt.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <clocale>
#include <cstring>

namespace {

QString g_red, g_blue;

void testBuiltins()
{
    const auto list = EffectFolders::builtinTransitions();
    CHECK(list.size() >= 12);
    for (const auto& e : list) {
        QString err;
        const QImage img = Lumas::image(e.path, 160, 90, false, &err);
        if (!CHECK(!img.isNull())) {
            std::printf("  %s: %s\n", qPrintable(e.path), qPrintable(err));
            continue;
        }
        // Voller Bereich: dunkelster Punkt 0, hellster 255 (sonst fängt/endet die Blende nicht pünktlich)
        const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
        int lo = 255, hi = 0;
        for (int y = 0; y < g.height(); ++y)
            for (int x = 0; x < g.width(); ++x) {
                lo = std::min<int>(lo, g.constScanLine(y)[x]);
                hi = std::max<int>(hi, g.constScanLine(y)[x]);
            }
        CHECK(lo <= 1 && hi >= 254);
        CHECK(EffectFolders::isBuiltin(e.path) && EffectFolders::displayName(e.path) == e.name);
    }
    // Übersicht: LUMAS_DUMP=<datei.png> build-tests/tests/test_lumas (je Verlauf: Bild, halb durch)
    if (const QByteArray dump = qgetenv("LUMAS_DUMP"); !dump.isEmpty()) {
        QImage sheet(4 * 330, int((list.size() + 3) / 4) * 100, QImage::Format_RGB32);
        sheet.fill(Qt::darkGray);
        QPainter p(&sheet);
        for (int i = 0; i < list.size(); ++i) {
            const QImage l = Lumas::image(list[i].path, 160, 90);
            const QPoint o(i % 4 * 330, i / 4 * 100);
            p.drawImage(o, l.convertToFormat(QImage::Format_RGB32));
            p.drawImage(o + QPoint(165, 0), Lumas::preview(l, 0.5));
        }
        p.end();
        sheet.save(QString::fromLocal8Bit(dump));
    }
    // Kreis auf: Mitte dunkel (zuerst), Ecke hell; umgekehrt andersherum
    const QImage k = Lumas::image(":/lumas/kreis", 160, 90).convertToFormat(QImage::Format_Grayscale8);
    CHECK(k.pixelColor(80, 45).value() < 20 && k.pixelColor(0, 0).value() > 235);
    const QImage ki = Lumas::image(":/lumas/kreis", 160, 90, true).convertToFormat(QImage::Format_Grayscale8);
    CHECK(ki.pixelColor(80, 45).value() > 235);
    QString err;
    CHECK(Lumas::image(":/lumas/gibtsnicht", 16, 9, false, &err).isNull() && !err.isEmpty());
    CHECK(!Lumas::preview(k).isNull());
}

void testFiles(const QString& dir)
{
    QDir(EffectFolders::root()).removeRecursively(); // Testmodus: eigener Ordner, nie der des Nutzers
    EffectFolders::ensure();
    CHECK(QFileInfo(EffectFolders::transitionDir()).isDir());
    CHECK(EffectFolders::userTransitions().isEmpty());

    // Eigenes Bild (Farbe, falsche Größe): Graustufen, auf Projektgröße skaliert; Unterordner = Gruppe
    QImage own(64, 64, QImage::Format_RGB32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) own.setPixelColor(x, y, QColor(x * 4, x * 4, 0)); // links dunkel
    const QDir td(EffectFolders::transitionDir());
    QDir().mkpath(td.filePath("Kdenlive"));
    CHECK(own.save(td.filePath("Kdenlive/links.png")));
    CHECK(own.save(td.filePath("oben.jpg")));
    QFile txt(td.filePath("notiz.txt"));
    CHECK(txt.open(QIODevice::WriteOnly) && txt.write("x") == 1);
    txt.close();
    const auto user = EffectFolders::userTransitions();
    if (CHECK_EQ(user.size(), 2)) {
        CHECK_EQ(user[0].name, QString("oben"));
        CHECK_EQ(user[1].group, QString("Kdenlive"));
    }
    CHECK_EQ(EffectFolders::findUserTransition("links.png"), td.filePath("Kdenlive/links.png"));
    CHECK(EffectFolders::watchDirs().contains(td.filePath("Kdenlive")));

    const QString png = Lumas::file(td.filePath("Kdenlive/links.png"), 320, 180, false);
    if (CHECK(!png.isEmpty())) {
        const QImage img(png);
        CHECK_EQ(img.size(), QSize(320, 180));
        CHECK(img.pixelColor(2, 90).value() < 30 && img.pixelColor(317, 90).value() > 150);
        CHECK_EQ(Lumas::file(td.filePath("Kdenlive/links.png"), 320, 180, false), png); // zwischengespeichert
    }
    const QString inv = Lumas::file(td.filePath("Kdenlive/links.png"), 320, 180, true);
    CHECK(!inv.isEmpty() && inv != png && QImage(inv).pixelColor(2, 90).value() > 200);
    CHECK(!Lumas::file(":/lumas/uhr", 32, 18, false).isEmpty());
    CHECK(Lumas::file(QDir(dir).filePath("fehlt.png"), 32, 18, false).isEmpty());

    // Drag-Daten
    TrackKind kind = TrackKind::Audio;
    TransitionStyle st;
    CHECK(EffectsLibrary::parseTransition("luma::/lumas/kreis", &kind, &st));
    CHECK(kind == TrackKind::Video && st.isLuma() && st.luma == ":/lumas/kreis" && !st.isWipe());
    CHECK(!EffectsLibrary::parseTransition("luma:", &kind, &st));
}

void testProjectFile(const QString& dir)
{
    ProjectData d;
    d.media << MediaInfo{"/x/a.mp4", "a.mp4", 200, true, false, false};
    d.timeline.video.resize(1);
    Clip a, b;
    a.id = 1;
    a.mediaPath = "/x/a.mp4";
    a.out = 49;
    a.transOut = 20;
    b.id = 2;
    b.mediaPath = "/x/a.mp4";
    b.start = 50;
    b.in = 60;
    b.out = 109;
    b.transIn = 20;
    TransitionStyle st;
    st.type = TransitionType::Luma;
    st.luma = QDir(EffectFolders::transitionDir()).filePath("Kdenlive/links.png");
    st.invert = true;
    st.softness = 30;
    a.transOutStyle = b.transInStyle = st;
    d.timeline.video[0].clips << a << b;
    d.lastClipId = 2;

    const QString projPath = QDir(dir).filePath("p/film.schneidi");
    QDir().mkpath(QFileInfo(projPath).absolutePath());
    QString err;
    CHECK(ProjectFile::save(d, projPath, &err));
    ProjectData l;
    CHECK(ProjectFile::load(projPath, &l, &err));
    CHECK_EQ(ProjectFile::toJson(l, projPath), ProjectFile::toJson(d, projPath));
    if (CHECK(l.timeline.video.size() == 1 && l.timeline.video[0].clips.size() == 2))
        CHECK(l.timeline.video[0].clips[0].transOutStyle == st);

    // Mitgeliefert: kein relativer Pfad
    TransitionStyle bs = st;
    bs.luma = ":/lumas/kreis";
    a.transOutStyle = b.transInStyle = bs;
    d.timeline.video[0].clips = {a, b};
    CHECK(!ProjectFile::toJson(d, projPath).contains("lumaRel"));

    // Von einem anderen Rechner (Windows-Pfad): gleichnamig im eigenen Ordner gefunden
    QByteArray json = ProjectFile::toJson(d, projPath);
    json.replace(":/lumas/kreis", "C:\\\\Users\\\\Max\\\\Lumas\\\\links.png");
    ProjectData o;
    CHECK(ProjectFile::fromJson(json, projPath, &o, &err));
    if (CHECK(!o.timeline.video.isEmpty() && o.timeline.video[0].clips.size() == 2))
        CHECK_EQ(o.timeline.video[0].clips[0].transOutStyle.luma, st.luma);
}

QImage render(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof->width(), h = prof->height();
    const uint8_t* d = f->get_image(ifmt, w, h);
    if (!d) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.copy();
}

bool reddish(const QColor& c) { return c.red() > 150 && c.blue() < 100; }
bool bluish(const QColor& c) { return c.blue() > 150 && c.red() < 100; }

void testRender()
{
    ProjectFormat fmt;
    fmt.width = 320;
    fmt.height = 180;
    Timeline tl;
    tl.video.resize(1);
    Clip a, b;
    a.id = 1;
    a.mediaPath = g_red;
    a.out = 49;
    a.transOut = 20;
    b.id = 2;
    b.mediaPath = g_blue;
    b.start = 50;
    b.in = 25;
    b.out = 74;
    b.transIn = 20;
    TransitionStyle st;
    st.type = TransitionType::Luma;
    st.luma = ":/lumas/tuer-h"; // Mitte wechselt zuerst
    a.transOutStyle = b.transInStyle = st;
    tl.video[0].clips << a << b;

    const QImage mid = render(tl, fmt, 50); // halb durch (Übergang 40..60)
    if (CHECK(!mid.isNull())) {
        CHECK(bluish(mid.pixelColor(160, 90)));
        CHECK(reddish(mid.pixelColor(5, 90)) && reddish(mid.pixelColor(314, 90)));
    }
    CHECK(reddish(render(tl, fmt, 38).pixelColor(160, 90)));
    CHECK(bluish(render(tl, fmt, 62).pixelColor(5, 90)));

    // Umgekehrt: Ränder zuerst
    tl.video[0].clips[0].transOutStyle.invert = tl.video[0].clips[1].transInStyle.invert = true;
    const QImage inv = render(tl, fmt, 50);
    if (CHECK(!inv.isNull())) {
        CHECK(reddish(inv.pixelColor(160, 90)));
        CHECK(bluish(inv.pixelColor(5, 90)));
    }

    // Ins Leere (nur Ausblenden): wie Cross Dissolve, gleichmäßig dunkler, kein Absturz
    Timeline single;
    single.video.resize(1);
    Clip s = a;
    s.transOutStyle = st;
    s.transOutAlone = true;
    single.video[0].clips << s;
    const QImage fade = render(single, fmt, 45);
    if (CHECK(!fade.isNull())) {
        const QColor c1 = fade.pixelColor(160, 90), c2 = fade.pixelColor(5, 90);
        CHECK(std::abs(c1.red() - c2.red()) <= 3 && c1.red() < 240);
    }
}

void testWidget()
{
    EffectsLibrary lib;
    lib.resize(500, 400);
    auto* tree = lib.findChild<QTreeWidget*>();
    auto* list = lib.findChild<QListWidget*>();
    if (!CHECK(tree && list)) return;
    auto top = [&](const QString& text) -> QTreeWidgetItem* {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == text) return tree->topLevelItem(i);
        return nullptr;
    };
    auto realItems = [&] {
        int n = 0;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->flags() & Qt::ItemIsDragEnabled) ++n;
        return n;
    };
    QTreeWidgetItem* own = top("schneidi");
    QTreeWidgetItem* trans = top("Übergänge");
    if (!CHECK(own && trans && own->childCount() == 2)) return;

    // schneidi: Übergänge + LUTs mit Kopfzeilen; Unterpunkt nur Übergänge
    tree->setCurrentItem(own);
    const int builtin = int(EffectFolders::builtinTransitions().size()), luts = int(EffectFolders::builtinLuts().size());
    CHECK_EQ(realItems(), builtin + luts);
    CHECK_EQ(list->count(), builtin + luts + 2);
    tree->setCurrentItem(own->child(0));
    CHECK_EQ(realItems(), builtin);
    if (const auto d = list->item(0)->data(Qt::UserRole).toString(); CHECK(d.startsWith("luma:"))) {
        QSignalSpy spy(&lib, &EffectsLibrary::transitionRequested);
        emit list->itemDoubleClicked(list->item(0));
        CHECK_EQ(spy.size(), 1);
    }
    // Toolbox → Videoübergänge enthält die mitgelieferten Verläufe, nicht die leere Art „Verlaufsblende“
    tree->setCurrentItem(top("Toolbox")->child(0));
    CHECK_EQ(realItems(), int(std::size(kTransitionTypes)) - 1 + builtin);

    // Eigene Übergänge samt Unterordner
    tree->setCurrentItem(trans);
    CHECK_EQ(realItems(), 2);
    if (CHECK(trans->childCount() == 1)) {
        tree->setCurrentItem(trans->child(0));
        CHECK_EQ(realItems(), 1);
    }

    // Zuklappen: Pfeile sichtbar, Zustand wird gespeichert und beim nächsten Mal wiederhergestellt
    CHECK(tree->rootIsDecorated() && top("Toolbox")->isExpanded());
    top("Toolbox")->setExpanded(false);
    {
        EffectsLibrary again;
        auto* t2 = again.findChild<QTreeWidget*>();
        bool found = false;
        for (int i = 0; t2 && i < t2->topLevelItemCount(); ++i)
            if (t2->topLevelItem(i)->text(0) == "Toolbox") {
                found = true;
                CHECK(!t2->topLevelItem(i)->isExpanded());
            }
        CHECK(found);
    }
    top("Toolbox")->setExpanded(true);
    CHECK(!lib.grab().isNull());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("lumas");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testBuiltins();
    testFiles(tmp.path());
    testProjectFile(tmp.path());
    testWidget();

    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Render-Test übersprungen\n");
        QDir(EffectFolders::root()).removeRecursively();
        return Check::result();
    }
    g_red = Check::makeMedia(tmp.filePath("rot.mp4"), {"-f", "lavfi", "-i", "color=c=red:size=320x180:rate=25:duration=4",
                                                       "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    g_blue = Check::makeMedia(tmp.filePath("blau.mp4"), {"-f", "lavfi", "-i", "color=c=blue:size=320x180:rate=25:duration=4",
                                                         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (CHECK(!g_red.isEmpty() && !g_blue.isEmpty())) {
        Check::initMlt();
        std::setlocale(LC_NUMERIC, "C");
        testRender();
    }
    QDir(EffectFolders::root()).removeRecursively();
    return Check::result();
}
