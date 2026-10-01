// Test Effekte importieren (LUTs): Leser für .3dl, .csp und Hald-CLUT, mitgelieferte Looks (Kategorie „schneidi“),
// eigener LUTs-Ordner mit Unterordnern, LUT aus der Effects Library auf einen Clip (mit Undo), Projektdatei
// (mitgelieferte LUT ohne relativen Pfad, fehlende LUT im eigenen Ordner wiedergefunden) und das Widget samt
// automatischem Neueinlesen. Bild der Kategorie „schneidi“: LUTS_DUMP=<datei.png> build-tests/tests/test_luts
#include "check.h"

#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "ui/EffectsLibrary.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>
#include <functional>

namespace {

using Fn = std::function<void(double&, double&, double&)>;

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray cube(int n, const Fn& fn)
{
    QByteArray s = "LUT_3D_SIZE " + QByteArray::number(n) + "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                double x = r / double(n - 1), y = g / double(n - 1), z = b / double(n - 1);
                fn(x, y, z);
                s += QByteArray::number(x, 'f', 6) + " " + QByteArray::number(y, 'f', 6) + " " +
                     QByteArray::number(z, 'f', 6) + "\n";
            }
    return s;
}

// .3dl: Gitterzeile + ganzzahlige Werte (maxOut), B schnellster Index
QByteArray threeDl(int n, int maxOut, const Fn& fn, bool meshLine = true)
{
    QByteArray s = "# Test\n";
    if (meshLine) {
        for (int i = 0; i < n; ++i) s += QByteArray::number(i * 1023 / (n - 1)) + (i + 1 < n ? " " : "\n");
    }
    for (int r = 0; r < n; ++r)
        for (int g = 0; g < n; ++g)
            for (int b = 0; b < n; ++b) {
                double x = r / double(n - 1), y = g / double(n - 1), z = b / double(n - 1);
                fn(x, y, z);
                s += QByteArray::number(qRound(x * maxOut)) + " " + QByteArray::number(qRound(y * maxOut)) + " " +
                     QByteArray::number(qRound(z * maxOut)) + "\n";
            }
    return s;
}

// Grauverlauf 0..255 (eine Zeile) durch die LUT
std::vector<uint8_t> graded(const ColorGrade::Lut* lut)
{
    std::vector<uint8_t> px(256 * 4);
    for (int i = 0; i < 256; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = uint8_t(i);
        px[i * 4 + 3] = 255;
    }
    ColorGrade::apply(px.data(), 256, 1, ColorGrade::Params(), lut);
    return px;
}
int R(const std::vector<uint8_t>& px, int i) { return px[i * 4]; }

bool isIdentity(const ColorGrade::Lut* lut)
{
    const auto o = graded(lut);
    for (int i = 0; i < 256; ++i)
        if (std::abs(R(o, i) - i) > 1) return false;
    return true;
}
bool isInvert(const ColorGrade::Lut* lut)
{
    const auto o = graded(lut);
    for (int i = 0; i < 256; ++i)
        if (std::abs(R(o, i) - (255 - i)) > 1) return false;
    return true;
}

const Fn kIdent = [](double&, double&, double&) {};
const Fn kInvert = [](double& r, double& g, double& b) {
    r = 1 - r;
    g = 1 - g;
    b = 1 - b;
};

void testFormats(const QString& dir)
{
    QString err;
    auto path = [&](const char* name) { return QDir(dir).filePath(name); };

    // .3dl: 10 und 12 Bit, mit und ohne Gitterzeile, Bittiefe aus „Mesh“
    CHECK(writeFile(path("id10.3dl"), threeDl(17, 1023, kIdent)));
    CHECK(writeFile(path("inv12.3dl"), threeDl(9, 4095, kInvert)));
    CHECK(writeFile(path("inv-ohne.3dl"), threeDl(5, 1023, kInvert, false)));
    CHECK(writeFile(path("mesh.3dl"), "3DMESH\nMesh 4 12\n" + threeDl(5, 4095, kIdent).mid(7)));
    for (const char* n : {"id10.3dl", "mesh.3dl"})
        if (auto l = ColorGrade::parseLut(path(n), &err); CHECK(l)) CHECK(isIdentity(l.get()));
    for (const char* n : {"inv12.3dl", "inv-ohne.3dl"})
        if (auto l = ColorGrade::parseLut(path(n), &err); CHECK(l)) CHECK(isInvert(l.get()));
    // Achsen-Reihenfolge: nur Rot invertiert -> Grün/Blau bleiben
    CHECK(writeFile(path("rot.3dl"), threeDl(5, 1023, [](double& r, double&, double&) { r = 1 - r; })));
    if (auto l = ColorGrade::parseLut(path("rot.3dl")); CHECK(l)) {
        std::vector<uint8_t> px{255, 0, 64, 255};
        ColorGrade::apply(px.data(), 1, 1, ColorGrade::Params(), l.get());
        CHECK(px[0] <= 1 && px[1] <= 1 && std::abs(px[2] - 64) <= 1);
    }
    CHECK(writeFile(path("kaputt.3dl"), "0 512 1023\n0 0 0\n"));
    err.clear();
    CHECK(!ColorGrade::parseLut(path("kaputt.3dl"), &err) && !err.isEmpty());

    // .csp: 3D mit Metadaten, Vorab-Kurve (halbiert den Eingang), 1D
    QByteArray csp = "CSPLUTV100\n3D\n\nBEGIN METADATA\nTest\nEND METADATA\n\n";
    for (int c = 0; c < 3; ++c) csp += "2\n0.0 1.0\n0.0 1.0\n";
    csp += "\n9 9 9\n" + cube(9, kInvert).mid(QByteArray("LUT_3D_SIZE 9\n").size());
    CHECK(writeFile(path("inv.csp"), csp));
    if (auto l = ColorGrade::parseLut(path("inv.csp"), &err); CHECK(l)) {
        CHECK(l->shaperIn[0].isEmpty()); // Identität -> keine Vorab-Kurve
        CHECK(isInvert(l.get()));
    }
    QByteArray half = "CSPLUTV100\n3D\n";
    for (int c = 0; c < 3; ++c) half += "3\n0.0 0.5 1.0\n0.0 0.25 0.5\n";
    half += "2 2 2\n" + cube(2, kIdent).mid(QByteArray("LUT_3D_SIZE 2\n").size());
    CHECK(writeFile(path("half.csp"), half));
    if (auto l = ColorGrade::parseLut(path("half.csp"), &err); CHECK(l)) {
        CHECK_EQ(l->shaperIn[1].size(), 3);
        const auto o = graded(l.get());
        CHECK(std::abs(R(o, 255) - 128) <= 1 && std::abs(R(o, 128) - 64) <= 1);
    }
    CHECK(writeFile(path("eins.csp"), "CSPLUTV100\n1D\n2\n0 1\n0 1\n2\n0 1\n0 1\n2\n0 1\n0 1\n2\n1 1 1\n0 0 0\n"));
    if (auto l = ColorGrade::parseLut(path("eins.csp"), &err); CHECK(l && !l->is3d)) CHECK(isInvert(l.get()));
    CHECK(writeFile(path("falsch.csp"), "CSPLUTV100\n3D\n2\n0 1\n0 1\n"));
    CHECK(!ColorGrade::parseLut(path("falsch.csp")));

    // Hald-CLUT (Stufe 4: 64x64, Würfel 16), Identität und invertiert, auch als 16-Bit-Bild
    for (bool invert : {false, true}) {
        const int level = 4, n = level * level, w = level * level * level;
        QImage img(w, w, QImage::Format_RGBA64);
        for (int i = 0; i < w * w; ++i) {
            double r = (i % n) / double(n - 1), g = (i / n % n) / double(n - 1), b = (i / (n * n)) / double(n - 1);
            if (invert) kInvert(r, g, b);
            img.setPixelColor(i % w, i / w, QColor::fromRgbF(float(r), float(g), float(b)));
        }
        const QString p = path(invert ? "hald-inv.png" : "hald-id.png");
        CHECK(img.save(p));
        if (auto l = ColorGrade::parseLut(p, &err); CHECK(l)) {
            CHECK_EQ(l->size, 16);
            CHECK(invert ? isInvert(l.get()) : isIdentity(l.get()));
        }
    }
    QImage notHald(50, 50, QImage::Format_RGB32);
    notHald.fill(Qt::red);
    CHECK(notHald.save(path("kein-hald.png")));
    err.clear();
    CHECK(!ColorGrade::parseLut(path("kein-hald.png"), &err) && !err.isEmpty());

    // Unbekannte Endung, fehlende Datei; Zwischenspeicher bei loadLut
    CHECK(!ColorGrade::parseLut(path("x.txt")));
    CHECK(!ColorGrade::loadLut(path("fehlt.3dl")));
    auto a = ColorGrade::loadLut(path("id10.3dl"));
    CHECK(a && ColorGrade::loadLut(path("id10.3dl")) == a);
    CHECK(EffectFolders::isLutFile("x/LOOK.CUBE") && EffectFolders::isLutFile("a.tiff") && !EffectFolders::isLutFile("a.mp4"));
}

void testBuiltins()
{
    const auto luts = EffectFolders::builtinLuts();
    CHECK(luts.size() >= 10);
    for (const auto& l : luts) {
        QString err;
        const auto lut = ColorGrade::loadLut(l.path, &err);
        if (!CHECK(lut)) std::printf("  %s: %s\n", qPrintable(l.path), qPrintable(err));
        CHECK(!l.name.isEmpty() && EffectFolders::isBuiltin(l.path));
        CHECK_EQ(EffectFolders::displayName(l.path), l.name);
    }
    // Schwarzweiß macht Farbe grau, Warm macht Grau wärmer
    if (auto sw = ColorGrade::loadLut(":/luts/schwarzweiss.cube"); CHECK(sw)) {
        std::vector<uint8_t> px{200, 60, 30, 255};
        ColorGrade::apply(px.data(), 1, 1, ColorGrade::Params(), sw.get());
        CHECK(std::abs(px[0] - px[1]) <= 1 && std::abs(px[1] - px[2]) <= 1);
    }
    if (auto warm = ColorGrade::loadLut(":/luts/warm.cube"); CHECK(warm)) {
        std::vector<uint8_t> px{128, 128, 128, 255};
        ColorGrade::apply(px.data(), 1, 1, ColorGrade::Params(), warm.get());
        CHECK(px[0] > px[2] + 10);
    }
    CHECK_EQ(EffectFolders::displayName("/a/b/look.cube"), QString("look.cube"));
}

void testUserFolder(const QString& dir)
{
    QDir(EffectFolders::root()).removeRecursively(); // Testmodus: eigener Ordner, nie der des Nutzers
    EffectFolders::ensure();
    CHECK(QFileInfo(EffectFolders::lutDir()).isDir());
    CHECK(!QDir(EffectFolders::root()).entryList({"*.txt"}, QDir::Files).isEmpty()); // Liesmich
    CHECK(EffectFolders::userLuts().isEmpty());

    const QDir luts(EffectFolders::lutDir());
    CHECK(writeFile(luts.filePath("Zeta.cube"), cube(2, kIdent)));
    CHECK(writeFile(luts.filePath("alpha.3dl"), threeDl(2, 1023, kIdent)));
    CHECK(writeFile(luts.filePath("Film/Kodak/k2.cube"), cube(2, kInvert)));
    CHECK(writeFile(luts.filePath("Film/notiz.txt"), "keine LUT"));
    const auto all = EffectFolders::userLuts();
    if (CHECK_EQ(all.size(), 3)) {
        CHECK_EQ(all[0].name, QString("alpha")); // oben zuerst, nach Name
        CHECK_EQ(all[1].name, QString("Zeta"));
        CHECK_EQ(all[2].group, QString("Film/Kodak"));
    }
    CHECK_EQ(EffectFolders::watchDirs().size(), 4); // LUTs + 2 Unterordner + Transitions
    CHECK_EQ(EffectFolders::findUserLut("k2.cube"), luts.filePath("Film/Kodak/k2.cube"));
    CHECK(EffectFolders::findUserLut("gibtsnicht.cube").isEmpty());

    // LUT aus der Effects Library auf einen Clip (wie Ziehen/Doppelklick), Undo
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int v = p.timeline().video[0].clips[0].id;
    const int a = p.timeline().audio[0].clips[0].id;
    auto clip = [&](int id) { return *TimelineOps::findClip(p.timeline(), id); };
    const QString builtin = EffectFolders::builtinLuts().first().path;
    const int s0 = p.undoStack()->index();
    ed.addEffect({v, a}, EffectFolders::LutPrefix + builtin);
    CHECK_EQ(p.undoStack()->index(), s0 + 1);
    CHECK_EQ(ColorGrade::lutPath(clip(v)), builtin);
    CHECK(ColorGrade::active(clip(v)));
    CHECK(!EffectRegistry::has(clip(a), "grade")); // nur Video
    p.undoStack()->undo();
    CHECK(ColorGrade::lutPath(clip(v)).isEmpty());
    p.undoStack()->redo();

    // Projektdatei: mitgelieferte LUT ohne relativen Pfad, lädt wieder
    const QString projPath = QDir(dir).filePath("p/film.schneidi");
    QDir().mkpath(QFileInfo(projPath).absolutePath());
    CHECK(!ProjectFile::toJson(p.data(), projPath).contains("relPaths"));
    QString err;
    CHECK(ProjectFile::save(p.data(), projPath, &err));
    ProjectData loaded;
    CHECK(ProjectFile::load(projPath, &loaded, &err));
    CHECK_EQ(ColorGrade::lutPath(loaded.timeline.video[0].clips[0]), builtin);

    // Projekt von einem anderen Rechner (Windows-Pfad): LUT gleichen Namens im eigenen Ordner gefunden
    const QByteArray other = R"({"app":"schneidi","version":2,"fps":25,"media":[{"path":"/x/a.mp4","name":"a.mp4",
        "length":100,"hasVideo":true}],"timeline":{"video":[{"clips":[{"id":1,"start":0,"in":0,"out":49,"media":0,
        "effects":[{"id":"grade","params":{"lut":"C:\\Users\\Max\\Looks\\k2.cube"},"relPaths":{"lut":"../Looks/k2.cube"}}]}]}],
        "audio":[]}})";
    ProjectData o;
    CHECK(ProjectFile::fromJson(other, projPath, &o, &err));
    if (CHECK(!o.timeline.video.isEmpty() && !o.timeline.video[0].clips.isEmpty()))
        CHECK_EQ(ColorGrade::lutPath(o.timeline.video[0].clips[0]), luts.filePath("Film/Kodak/k2.cube"));
}

// Oberste Kategorie mit diesem Namen, sonst die erste darunter („LUTs“ gibt es auch unter „schneidi“)
QTreeWidgetItem* findCategory(QTreeWidget* tree, const QString& text)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == text) return tree->topLevelItem(i);
    const auto hits = tree->findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
    return hits.isEmpty() ? nullptr : hits.first();
}

int realItems(QListWidget* list)
{
    int n = 0;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->flags() & Qt::ItemIsDragEnabled) ++n;
    return n;
}

void testWidget()
{
    EffectsLibrary lib;
    lib.resize(500, 400);
    auto* tree = lib.findChild<QTreeWidget*>();
    auto* list = lib.findChild<QListWidget*>();
    if (!CHECK(tree && list)) return;

    QTreeWidgetItem* top = findCategory(tree, "LUTs");
    if (!CHECK(top && top->childCount() == 2 && !findCategory(tree, "schneidi"))) return;
    QTreeWidgetItem* builtin = top->child(0); // Mitgeliefert
    QTreeWidgetItem* luts = top->child(1);    // Eigene
    tree->setCurrentItem(top); // LUTs: Mitgeliefert + Eigene
    if (const QByteArray dump = qgetenv("LUTS_DUMP"); !dump.isEmpty()) // Bild der Kategorie: LUTS_DUMP=<datei.png>
        lib.grab().save(QString::fromLocal8Bit(dump));
    CHECK_EQ(realItems(list), int(EffectFolders::builtinLuts().size()) + 3);
    tree->setCurrentItem(builtin);
    CHECK_EQ(realItems(list), int(EffectFolders::builtinLuts().size()));
    CHECK(!list->item(0)->icon().isNull());

    tree->setCurrentItem(luts);
    CHECK_EQ(realItems(list), 3);
    QTreeWidgetItem* film = findCategory(tree, "Film");
    QTreeWidgetItem* kodak = findCategory(tree, "Kodak");
    if (CHECK(film && kodak && kodak->parent() == film)) {
        tree->setCurrentItem(film);
        CHECK_EQ(realItems(list), 1); // samt Unterordner
    }

    // Doppelklick = Signal mit "lut:<Pfad>"
    QSignalSpy spy(&lib, &EffectsLibrary::effectRequested);
    tree->setCurrentItem(builtin);
    emit list->itemDoubleClicked(list->item(0));
    if (CHECK_EQ(spy.size(), 1))
        CHECK(spy.first().first().toString().startsWith(EffectFolders::LutPrefix));

    // Neue Datei + neuer Unterordner erscheinen von selbst, kaputte LUT sichtbar, aber nicht ziehbar
    tree->setCurrentItem(luts);
    const QDir dir(EffectFolders::lutDir());
    CHECK(writeFile(dir.filePath("neu.cube"), cube(2, kIdent)));
    CHECK(writeFile(dir.filePath("Kaputt/x.cube"), "LUT_3D_SIZE 2\n0 0 0\n"));
    QTRY_COMPARE_WITH_TIMEOUT(realItems(list), 4, 5000);
    CHECK_EQ(list->count(), 5);
    CHECK(findCategory(tree, "Kaputt"));
    CHECK(tree->currentItem() == luts); // Auswahl bleibt
    CHECK(QDir(dir.filePath("Kaputt")).removeRecursively());
    QTRY_VERIFY_WITH_TIMEOUT(!findCategory(tree, "Kaputt"), 5000);
    CHECK(!lib.grab().isNull());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("luts");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testFormats(tmp.path());
    testBuiltins();
    testUserFolder(tmp.path());
    testWidget();
    QDir(EffectFolders::root()).removeRecursively();
    return Check::result();
}
