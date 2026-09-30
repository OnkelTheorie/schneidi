// Test Projektdatei: Projekt mit allen Features speichern -> laden -> speichern ergibt dieselbe Datei, und
// alle Werte kommen unverändert zurück (auch Felder, die beim ersten Speichern verloren gehen würden).
// Dazu: verschobener Projektordner (relative Pfade), ungültige/zu neue Dateien.
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

// Feldweiser Vergleich (Ausgabe des ersten Unterschieds), damit auch Werte auffallen, die gar nicht gespeichert werden
QString diffClip(const Clip& a, const Clip& b)
{
#define D(f) if (!(a.f == b.f)) return QString("Clip %1: " #f).arg(a.id)
    D(id); D(kind); D(mediaPath); D(start); D(in); D(out); D(linkId); D(volumeDb); D(pan); D(enabled);
    D(transIn); D(transOut); D(fadeIn); D(fadeOut); D(effects); D(keys); D(speed); D(reverse); D(freeze); D(keepPitch);
    if (a.transIn > 0) D(transInStyle);
    if (a.transOut > 0) D(transOutStyle);
    D(transform.zoomX); D(transform.zoomY); D(transform.posX); D(transform.posY); D(transform.rotation);
    D(transform.cropLeft); D(transform.cropRight); D(transform.cropTop); D(transform.cropBottom);
    D(transform.opacity); D(transform.transformOn); D(transform.cropOn); D(transform.compositeOn);
    if (a.isTitle()) {
        D(title.text); D(title.font); D(title.size); D(title.color); D(title.bold); D(title.italic); D(title.align);
        D(title.posX); D(title.posY); D(title.outlineOn); D(title.outlineColor); D(title.outlineWidth);
        D(title.boxOn); D(title.boxColor); D(title.boxPad);
    }
#undef D
    return {};
}

QString diffData(const ProjectData& a, const ProjectData& b)
{
    if (a.format != b.format) return "format";
    if (a.playhead != b.playhead) return "playhead";
    if (a.lastClipId != b.lastClipId || a.lastLinkId != b.lastLinkId) return "lastClipId/lastLinkId";
    if (a.media.size() != b.media.size()) return "media.size";
    for (int i = 0; i < a.media.size(); ++i) {
        const MediaInfo &x = a.media[i], &y = b.media[i];
        if (x.path != y.path || x.name != y.name || x.length != y.length || x.hasVideo != y.hasVideo
            || x.hasAudio != y.hasAudio || x.isImage != y.isImage || x.markIn != y.markIn || x.markOut != y.markOut)
            return "media " + x.name;
    }
    const Timeline &s = a.timeline, &t = b.timeline;
    if (s.markers != t.markers || s.markIn != t.markIn || s.markOut != t.markOut || s.masterVolumeDb != t.masterVolumeDb)
        return "timeline (Marker/In/Out/Master)";
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        if (s.tracks(k).size() != t.tracks(k).size()) return "Spuranzahl";
        for (int i = 0; i < s.tracks(k).size(); ++i) {
            const Track &x = s.tracks(k)[i], &y = t.tracks(k)[i];
            const QString n = trackShortName({k, i});
            if (x.kind != y.kind || x.name != y.name || x.color != y.color || x.locked != y.locked || x.muted != y.muted
                || x.hidden != y.hidden || x.volumeDb != y.volumeDb || x.pan != y.pan || x.solo != y.solo)
                return "Spur " + n;
            if (x.clips.size() != y.clips.size()) return "Clipanzahl " + n;
            for (int j = 0; j < x.clips.size(); ++j)
                if (QString d = diffClip(x.clips[j], y.clips[j]); !d.isEmpty()) return n + " " + d;
        }
    }
    return {};
}

Clip mk(int id, const QString& path, int start, int in, int out, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    c.linkId = link;
    return c;
}

// Projekt mit möglichst allen Features (Werte bewusst krumm, damit Rundungen auffallen)
ProjectData fullProject(const QString& dir)
{
    const QString a = dir + "/medien/a.mp4", b = dir + "/medien/b.mov", m = dir + "/medien/musik.wav",
                  still = dir + "/medien/bild.png";
    ProjectData d;
    d.format.width = 1080;
    d.format.height = 1920;
    d.format.rate = {30000, 1001};
    d.playhead = 1234;
    d.media = {MediaInfo{a, "a.mp4", 500, true, true, false, 10, 200},
               MediaInfo{b, "b.mov", 300, true, true, false, -1, 99},
               MediaInfo{m, "musik.wav", 900, false, true, false, 5, -1},
               MediaInfo{still, "bild.png", 0, true, false, true}};

    Timeline& tl = d.timeline;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 3);
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 3);

    // V1: a | b mit Überblendung (Wischblende mit Rand/Weichheit, Start on Edit), Fades, Transform, Effekte, Keyframes
    Clip v1 = mk(1, a, 0, 10, 209, 1);
    v1.transIn = 13;
    v1.transInStyle.type = TransitionType::DipToColor;
    v1.transInStyle.color = QColor("#336699");
    v1.transOut = 25;
    v1.transOutStyle.type = TransitionType::WipeLeft;
    v1.transOutStyle.align = TransitionAlign::Start;
    v1.transOutStyle.softness = 37.5;
    v1.transOutStyle.border = 4.25;
    v1.transOutStyle.borderColor = QColor("#ff8800");
    v1.fadeIn = 7;
    v1.transform.zoomX = 1.37;
    v1.transform.zoomY = 0.81;
    v1.transform.posX = -120.5;
    v1.transform.posY = 33.25;
    v1.transform.rotation = 12.345;
    v1.transform.cropLeft = 10;
    v1.transform.cropBottom = 22.5;
    v1.transform.opacity = 64.2;
    v1.transform.cropOn = false;
    EffectRegistry::add(v1, "color");
    EffectRegistry::instance(v1, "color")->params["temperature"] = 42.5;
    EffectRegistry::instance(v1, "color")->params["saturation"] = -17.0;
    EffectRegistry::add(v1, "blur");
    v1.effects[1].enabled = false;
    Keys::setKey(v1, AnimParam::Opacity, 10, 0);
    Keys::setKey(v1, AnimParam::Opacity, 40, 100);
    Keys::setKey(v1, AnimParam::PosX, 20, -300.75);
    Keys::setKey(v1, AnimParam::PosX, 90, 300);
    Keys::setKey(v1, AnimParam::FxTemp, 50, -80);
    Keys::setKey(v1, AnimParam::FxTemp, 120, 80);
    Keys::setEase(v1, {40}, KeyEase::EaseInOut);
    Keys::setEase(v1, {90}, KeyEase::EaseOut);
    Keys::setKey(v1, AnimParam::ZoomX, 5, 1.0);
    Keys::setKey(v1, AnimParam::ZoomX, 60, 2.5);
    Keys::setEase(v1, {5}, KeyEase::Bezier, {AnimParam::ZoomX});
    Keys::setHandle(v1.keys[AnimParam::ZoomX], 0, true, 12.25, 0.875, true); // Bezier-Griffe
    Clip v2 = mk(2, b, 200, 50, 149, 2);
    v2.transIn = 25;
    v2.transInStyle = v1.transOutStyle;
    v2.fadeOut = 30;
    v2.speed = 0.5;
    v2.keepPitch = false;
    Clip frozen = mk(3, b, 400, 20, 79);
    frozen.freeze = true;
    frozen.enabled = false;
    Clip rev = mk(4, a, 500, 0, 99);
    rev.reverse = true;
    rev.speed = 2.0;
    Clip img = mk(5, still, 620, 0, 124);
    img.transform.transformOn = false; // nur Schalter geändert, Werte neutral
    tl.video[0].clips << v1 << v2 << frozen << rev << img;

    // V2: Titel mit allen Stil-Werten, Keyframes auf Größe/Farbe, Ausblenden über Farbe
    Clip t = mk(6, {}, 30, -20, 129);
    t.kind = ClipKind::Title;
    t.title.text = "Zeile 1\nZeile \"2\" – äöü ß";
    t.title.font = "DejaVu Serif";
    t.title.size = 96.5;
    t.title.color = QColor(250, 200, 10, 180);
    t.title.bold = true;
    t.title.italic = true;
    t.title.align = 2;
    t.title.posX = -400;
    t.title.posY = 512.25;
    t.title.outlineOn = true;
    t.title.outlineColor = QColor("#102030");
    t.title.outlineWidth = 6.5;
    t.title.boxOn = true;
    t.title.boxColor = QColor(0, 0, 0, 90);
    t.title.boxPad = 31;
    t.transOut = 15;
    t.transOutStyle.type = TransitionType::CrossDissolve;
    t.transOutStyle.align = TransitionAlign::End;
    Keys::setKey(t, AnimParam::TitleSize, 0, 40);
    Keys::setKey(t, AnimParam::TitleSize, 60, 120);
    Keys::setKey(t, AnimParam::TitleColor, 10, Keys::fromColor(QColor(255, 0, 0, 200)));
    Keys::setKey(t, AnimParam::TitleColor, 80, Keys::fromColor(QColor(0, 0, 255)));
    EffectRegistry::add(t, "blur");
    EffectRegistry::instance(t, "blur")->params["strength"] = 12.75;
    tl.video[1].clips << t;
    tl.video[1].name = "Titel & Grafik";
    tl.video[1].color = "violet";
    tl.video[1].locked = true;
    tl.video[2].hidden = true;
    tl.video[2].color = "orange";

    // A1: Partner von v1/v2 mit Lautstärke, Pan, Keyframes, Audio-Übergang
    Clip a1 = mk(11, a, 0, 10, 209, 1);
    a1.volumeDb = -7.5;
    a1.pan = 33.3;
    a1.transOut = 25;
    a1.transOutStyle.audio = AudioCurve::Minus3dB;
    Keys::setKey(a1, AnimParam::Volume, 0, -60);
    Keys::setKey(a1, AnimParam::Volume, 24, 0);
    Keys::setKey(a1, AnimParam::Pan, 100, -50);
    Clip a2 = mk(12, b, 200, 50, 149, 2);
    a2.transIn = 25;
    a2.transInStyle.audio = AudioCurve::Minus3dB;
    a2.speed = 0.5;
    a2.keepPitch = false;
    tl.audio[0].clips << a1 << a2;
    tl.audio[0].muted = true;
    tl.audio[0].volumeDb = -3.25;
    tl.audio[0].pan = -20;
    // A2: Musik, gesperrt, benannt, Solo, mit Fades
    Clip mu = mk(21, m, 0, 5, 804);
    mu.fadeIn = 50;
    mu.fadeOut = 75;
    mu.transIn = 20;
    mu.transInStyle.audio = AudioCurve::Zero;
    tl.audio[1].clips << mu;
    tl.audio[1].name = "Musik";
    tl.audio[1].color = "green";
    tl.audio[1].locked = true;
    tl.audio[1].solo = true;
    tl.audio[2].name = "Leer";

    tl.markers = {0, 77, 300, 1500};
    tl.markIn = 25;
    tl.markOut = 777;
    tl.masterVolumeDb = -1.5;
    d.lastClipId = 40;
    d.lastLinkId = 9;
    return d;
}

} // namespace

// Sicherungskopien: alter Stand landet im Backup-Ordner, gleicher Stand nicht doppelt, nur die neuesten bleiben
void testBackups(const QString& dir)
{
    QDir(ProjectFile::backupDir({})).removeRecursively(); // Testmodus-Pfad, Reste früherer Läufe
    const QString path = dir + "/backup-test.schneidi";
    CHECK(ProjectFile::backup(path).isEmpty()); // noch nie gespeichert
    ProjectData d;
    QString err;
    QStringList states;
    for (int i = 0; i < 6; ++i) {
        d.playhead = i;
        const QString copy = ProjectFile::backup(path, 4);
        CHECK_EQ(copy.isEmpty(), i == 0);
        CHECK(ProjectFile::save(d, path, &err));
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        states << f.readAll();
    }
    CHECK(ProjectFile::backup(path, 4).isEmpty() == false); // Stand 5 ist noch nicht gesichert
    CHECK(ProjectFile::backup(path, 4).isEmpty());          // jetzt schon -> keine Doppelung
    const QDir bdir(ProjectFile::backupDir(path));
    const QStringList files = bdir.entryList({"*.schneidi"}, QDir::Files, QDir::Name);
    CHECK_EQ(files.size(), 4);
    // Die vier neuesten Stände 2..5 in zeitlicher Reihenfolge
    for (int i = 0; i < files.size() && i < 4; ++i) {
        QFile f(bdir.filePath(files[i]));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == states[2 + i].toUtf8());
    }
    // gleichnamiges Projekt in anderem Ordner: eigener Backup-Ordner
    CHECK(ProjectFile::backupDir(path) != ProjectFile::backupDir(dir + "/medien/backup-test.schneidi"));
    QDir(ProjectFile::backupDir({})).removeRecursively();
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("projectfile");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString dir = tmp.filePath("projekt");
    QDir().mkpath(dir + "/medien");
    for (const char* f : {"a.mp4", "b.mov", "musik.wav", "bild.png"}) {
        QFile file(dir + "/medien/" + f);
        CHECK(file.open(QIODevice::WriteOnly)); // leere Platzhalter: Projektdatei prüft nur, ob es sie gibt
    }

    testBackups(dir);

    // --- Speichern -> Laden -> Speichern identisch, alle Werte gleich
    const ProjectData orig = fullProject(dir);
    const QString path = dir + "/schnitt.schneidi";
    QString err;
    CHECK(ProjectFile::save(orig, path, &err));
    ProjectData loaded;
    CHECK(ProjectFile::load(path, &loaded, &err));
    CHECK_EQ(diffData(loaded, orig), QString());
    const QString path2 = dir + "/schnitt2.schneidi";
    CHECK(ProjectFile::save(loaded, path2, &err));
    QFile f1(path), f2(path2);
    CHECK(f1.open(QIODevice::ReadOnly) && f2.open(QIODevice::ReadOnly));
    const QByteArray json1 = f1.readAll(), json2 = f2.readAll();
    f1.close(); // Windows: Ordner mit offenen Dateien lässt sich unten nicht umbenennen
    f2.close();
    CHECK(!json1.isEmpty() && json1 == json2);
    // Direkt über toJson/fromJson ebenso (dritte Runde)
    ProjectData again;
    CHECK(ProjectFile::fromJson(json2, path, &again, &err));
    CHECK_EQ(diffData(again, orig), QString());
    CHECK(ProjectFile::toJson(again, path) == json1);
    CHECK(ProjectFile::missingMedia(loaded).isEmpty());

    // --- Clip, dessen Datei nicht im Media Pool steht: bleibt erhalten, zweites Speichern identisch
    {
        ProjectData d = orig;
        d.timeline.video[2].clips << mk(30, dir + "/medien/extra.mp4", 0, 0, 49);
        const QByteArray j1 = ProjectFile::toJson(d, path);
        ProjectData back;
        CHECK(ProjectFile::fromJson(j1, path, &back, &err));
        const Clip* c = TimelineOps::findClip(back.timeline, 30);
        CHECK(c && c->mediaPath == dir + "/medien/extra.mp4" && c->length() == 50);
        CHECK(ProjectFile::toJson(back, path) == j1);
    }

    // --- Projektordner verschoben: Medien über den relativen Pfad wiedergefunden
    {
        const QString moved = tmp.filePath("verschoben");
        CHECK(QDir().rename(dir, moved));
        ProjectData d;
        CHECK(ProjectFile::load(moved + "/schnitt.schneidi", &d, &err));
        CHECK_EQ(d.media.value(0).path, moved + "/medien/a.mp4");
        const Clip* c = TimelineOps::findClip(d.timeline, 21);
        CHECK(c && c->mediaPath == moved + "/medien/musik.wav");
        CHECK(ProjectFile::missingMedia(d).isEmpty());
        // Medien weg: bleiben offline (alter Pfad), relink findet sie in einem anderen Ordner
        const QString elsewhere = tmp.filePath("anderswo/tief");
        QDir().mkpath(elsewhere);
        CHECK(QDir().rename(moved + "/medien", elsewhere + "/medien"));
        CHECK(ProjectFile::load(moved + "/schnitt.schneidi", &d, &err));
        CHECK_EQ(ProjectFile::missingMedia(d).size(), 4);
        CHECK_EQ(ProjectFile::relink(&d, tmp.filePath("anderswo")), 4);
        c = TimelineOps::findClip(d.timeline, 1);
        CHECK(c && c->mediaPath == elsewhere + "/medien/a.mp4" && ProjectFile::missingMedia(d).isEmpty());
    }

    // --- Projekt vom anderen System (Linux <-> Windows): Dateiname aus fremden Pfaden, relink findet die Medien
    {
        CHECK_EQ(ProjectFile::fileNameAnyOs("/home/nutzer/Videos/a.mp4"), QString("a.mp4"));
        CHECK_EQ(ProjectFile::fileNameAnyOs("C:\\Users\\nutzer\\Videos\\Clip 1.mov"), QString("Clip 1.mov"));
        CHECK_EQ(ProjectFile::fileNameAnyOs("C:/Users/nutzer/Übung ä.mp4"), QString("Übung ä.mp4"));
        CHECK_EQ(ProjectFile::fileNameAnyOs("a.mp4"), QString("a.mp4"));
        ProjectData d;
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[
            {"path":"/home/nutzer/Videos/a.mp4","length":100,"hasVideo":true},
            {"path":"C:\\Users\\nutzer\\Videos\\b.mov","length":100,"hasVideo":true}],
            "timeline":{"video":[{"clips":[{"id":1,"media":1,"start":0,"in":0,"out":9}]}],"audio":[]}})",
                                    tmp.filePath("fremd.schneidi"), &d, &err));
        CHECK(d.media.size() == 2 && d.media[0].name == "a.mp4" && d.media[1].name == "b.mov");
        CHECK_EQ(ProjectFile::missingMedia(d).size(), 2);
        CHECK_EQ(ProjectFile::relink(&d, tmp.filePath("anderswo")), 2);
        CHECK(ProjectFile::missingMedia(d).isEmpty());
        const Clip* c = TimelineOps::findClip(d.timeline, 1);
        CHECK(c && QFileInfo(c->mediaPath).fileName() == "b.mov");
    }

    // --- Ungültige Dateien
    {
        ProjectData d;
        CHECK(!ProjectFile::fromJson("kein json", path, &d, &err) && !err.isEmpty());
        CHECK(!ProjectFile::fromJson(R"({"app":"anders","version":1})", path, &d, &err));
        CHECK(!ProjectFile::fromJson(R"({"app":"schneidi","version":999})", path, &d, &err));
        CHECK(!ProjectFile::load(tmp.filePath("gibtsnicht.schneidi"), &d, &err));
        // alte Datei ohne Format/Keyframes/Spurnamen: Standardwerte
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"fps":30,
            "media":[{"path":"/x/a.mp4","name":"a.mp4","length":100,"hasVideo":true}],
            "timeline":{"video":[{"name":"V1","clips":[{"id":3,"media":0,"start":5,"in":0,"out":9,
                "transOut":10,"transOutStyle":{"type":"dip_white"}}]}],"audio":[]}})",
                                    path, &d, &err));
        CHECK(d.format.width == 1920 && d.format.height == 1080 && d.format.rate == FrameRate{30, 1});
        CHECK(d.lastClipId == 3 && d.timeline.video.size() == 1 && d.timeline.video[0].name.isEmpty());
        CHECK(d.timeline.markIn == -1 && d.media[0].markIn == -1);
        const Clip& c = d.timeline.video[0].clips[0];
        CHECK(c.transOutStyle.type == TransitionType::DipToColor && c.transOutStyle.color == QColor(Qt::white));
    }
    return Check::result();
}
