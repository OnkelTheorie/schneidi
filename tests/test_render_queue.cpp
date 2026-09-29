// Test Deliver: Render-Einstellungen/Vorlagen (eingebaut + eigene), Render-Warteschlange (Projektdatei, DeliverPanel,
// nacheinander rendern mit Status, Fehler, Abbrechen). Gerendert wird klein und kurz (ffmpeg/ffprobe nötig).
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <Mlt.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <clocale>

namespace {

const RenderJob* job(const Project& p, int id)
{
    for (const RenderJob& j : p.renderQueue())
        if (j.id == id) return &j;
    return nullptr;
}

// ffprobe: "codec_type=video|codec_name=h264|width=..." je Strom + Dauer
QJsonObject probe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type,codec_name,width,height:format=duration",
                        "-of", "json", path});
    if (!p.waitForFinished(60000)) return {};
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

QJsonObject stream(const QJsonObject& probe, const QString& type)
{
    for (const QJsonValue& v : probe.value("streams").toArray())
        if (v.toObject().value("codec_type").toString() == type) return v.toObject();
    return {};
}

bool waitFor(RenderQueue& q, int ms = 120000)
{
    if (!q.isRunning()) return true;
    QEventLoop loop;
    QObject::connect(&q, &RenderQueue::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return !q.isRunning();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("render-queue");
    QFile::remove(RenderPresets::userFile()); // Test-Einstellungen vom letzten Lauf

    // --- Einstellungen: Ausgabegröße
    {
        const QSize hd(1920, 1080), portrait(1080, 1920);
        RenderSettings s;
        CHECK_EQ(s.outputSize(hd), hd);
        s.shortSide = 2160;
        CHECK_EQ(s.outputSize(hd), QSize(3840, 2160));
        s.shortSide = 720;
        CHECK_EQ(s.outputSize(portrait), QSize(720, 1280));
        s.size = QSize(1080, 1920); // fest hat Vorrang
        CHECK_EQ(s.outputSize(hd), QSize(1080, 1920));
        s.format = "wav";
        CHECK(s.audioOnly() && s.outputSize(hd).isEmpty());
        CHECK_EQ(renderFormat("gibtsnicht").id, QString("h264").toLatin1());
        // JSON hin und zurück
        RenderSettings a;
        a.format = "prores";
        a.shortSide = 1080;
        a.quality = 2;
        a.audioBitrateK = 192;
        CHECK(RenderSettings::fromJson(a.toJson()) == a);
        a.size = QSize(1080, 1920);
        CHECK(RenderSettings::fromJson(a.toJson()) == a);
    }

    // --- Eingebaute Vorlagen
    {
        const auto b = RenderPresets::builtins();
        QStringList formats;
        for (const RenderPreset& p : b) {
            CHECK(p.builtin);
            formats << p.settings.format;
        }
        CHECK(formats.contains("h264") && formats.contains("h265") && formats.contains("prores")
              && formats.contains("wav") && formats.contains("aac"));
        CHECK_EQ(b[0].name, QString("YouTube 1080p"));
        CHECK_EQ(b[1].settings.outputSize({1920, 1080}), QSize(3840, 2160));
        CHECK_EQ(b[2].settings.outputSize({1920, 1080}), QSize(1080, 1920)); // Hochformat 9:16
    }

    // --- Warteschlange in der Projektdatei
    {
        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        RenderJob j1;
        j1.id = 1;
        j1.path = "/x/out/a.mp4";
        j1.preset = "YouTube 1080p";
        j1.settings.shortSide = 1080;
        j1.size = QSize(1920, 1080);
        j1.status = RenderStatus::Done;
        j1.renderMs = 12345;
        RenderJob j2;
        j2.id = 2;
        j2.path = "/x/out/b.wav";
        j2.settings.format = "wav";
        j2.inOut = true;
        j2.from = 10;
        j2.to = 50;
        j2.status = RenderStatus::Failed;
        j2.message = "kaputt";
        RenderJob j3 = j1;
        j3.id = 3;
        j3.status = RenderStatus::Rendering; // Programm während des Renderns beendet
        d.renderQueue = {j1, j2, j3};
        ProjectData back;
        QString err;
        const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
        CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
        CHECK_EQ(back.renderQueue.size(), 3);
        if (back.renderQueue.size() == 3) {
            CHECK(back.renderQueue[0] == j1);
            CHECK(back.renderQueue[1] == j2);
            CHECK(back.renderQueue[2].status == RenderStatus::Queued);
        }
        // Speichern -> Laden -> Speichern identisch
        CHECK_EQ(ProjectFile::toJson(back, "/x/p.schneidi"),
                 ProjectFile::toJson(ProjectData{back.format, back.playhead, back.media, back.bins, back.timeline,
                                                 back.lastClipId, back.lastLinkId, back.renderQueue},
                                     "/x/p.schneidi"));
        // Leere Warteschlange: kein Eintrag (Dateien bleiben wie vorher); alte Datei ohne Eintrag
        d.renderQueue.clear();
        CHECK(!ProjectFile::toJson(d, "/x/p.schneidi").contains("renderQueue"));
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &back, &err));
        CHECK(back.renderQueue.isEmpty());
        // Doppelte/fehlende ids werden neu vergeben, Aufträge ohne Pfad verworfen
        const auto fixed = RenderQueueJson::fromJson(QJsonDocument::fromJson(
            R"([{"id":1,"path":"/a.mp4"},{"id":1,"path":"/b.mp4"},{"path":"/c.mp4"},{"id":4}])").array());
        CHECK_EQ(fixed.size(), 3);
        if (fixed.size() == 3) CHECK(fixed[0].id == 1 && fixed[1].id == 2 && fixed[2].id == 3);

        // Project: Änderung gilt als Änderung, aber ohne Undo-Schritt (wie DaVinci)
        Project p;
        const int undo = p.undoStack()->count();
        CHECK(!p.isModified());
        p.setRenderQueue({j1});
        CHECK(p.isModified());
        CHECK_EQ(p.undoStack()->count(), undo);
        CHECK_EQ(p.data().renderQueue.size(), 1);
        p.reset();
        CHECK(p.renderQueue().isEmpty());
    }

    // --- DeliverPanel: Vorlagen, eigene Vorlagen, Aufträge anlegen
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    {
        Project p;
        DeliverPanel panel(&p);
        auto* name = panel.findChild<QLineEdit*>("renderName");
        auto* folder = panel.findChild<QLineEdit*>("renderFolder");
        if (!CHECK(name && folder)) return Check::result();
        name->setText("Film");
        folder->setText(tmp.path());
        CHECK_EQ(panel.currentPreset(), 0); // YouTube 1080p
        RenderJob j = panel.makeJob();
        CHECK_EQ(j.size, QSize(1920, 1080));
        CHECK_EQ(j.path, QDir(tmp.path()).filePath("Film.mp4"));
        CHECK_EQ(j.preset, QString("YouTube 1080p"));
        CHECK(!j.inOut);
        // Hochformat, nur Audio, ProRes
        const auto& presets = panel.presets();
        for (int i = 0; i < presets.size(); ++i) {
            panel.selectPreset(i);
            CHECK_EQ(panel.currentPreset(), i);
            CHECK(panel.settings().format == presets[i].settings.format);
            j = panel.makeJob();
            CHECK(j.path.endsWith(QString(".") + renderFormat(presets[i].settings.format).extension));
            if (presets[i].settings.audioOnly()) CHECK(j.size.isEmpty());
            else CHECK_EQ(j.size, presets[i].settings.outputSize({1920, 1080}));
        }
        // Einstellung ändern -> „Eigene Einstellungen“
        panel.selectPreset(0);
        auto* format = panel.findChild<QComboBox*>("renderFormat");
        if (CHECK(format && format->count() == renderFormats().size())) {
            format->setCurrentIndex(1); // H.265 in Timeline-Auflösung = eingebaute H.265-Vorlage
            CHECK(panel.currentPreset() >= 0 && presets[panel.currentPreset()].settings.format == "h265");
            RenderSettings custom;
            custom.quality = 1; // H.264 1080p Mittel: keine Vorlage
            panel.setSettings(custom);
            CHECK_EQ(panel.currentPreset(), -1);
            format->setCurrentIndex(1);
            format->setCurrentIndex(0);
            panel.setSettings(RenderSettings{}); // H.264 Timeline Hoch: passt wieder zu YouTube 1080p
            CHECK_EQ(panel.currentPreset(), 0);
        }
        // Eigene Vorlage speichern, wiederfinden, überschreiben, löschen; eingebaute bleiben
        RenderSettings mine;
        mine.format = "h265";
        mine.shortSide = 720;
        mine.quality = 1;
        panel.setSettings(mine);
        CHECK(panel.savePreset("Mein Export"));
        CHECK(!panel.savePreset("YouTube 1080p"));
        CHECK(!panel.savePreset("  "));
        const int count = panel.presets().size();
        CHECK_EQ(count, RenderPresets::builtins().size() + 1);
        CHECK_EQ(panel.currentPreset(), count - 1);
        CHECK_EQ(RenderPresets::loadUser().size(), 1);
        CHECK(RenderPresets::loadUser().value(0).settings == mine);
        {
            DeliverPanel other(&p); // neue Instanz liest die Datei
            CHECK_EQ(other.presets().size(), count);
            CHECK_EQ(other.presets().last().name, QString("Mein Export"));
        }
        mine.quality = 2;
        panel.setSettings(mine);
        CHECK(panel.savePreset("Mein Export"));
        CHECK_EQ(panel.presets().size(), count);
        CHECK_EQ(RenderPresets::loadUser().value(0).settings.quality, 2);
        CHECK(!panel.deletePreset(0)); // eingebaut
        CHECK(panel.deletePreset(count - 1));
        CHECK(RenderPresets::loadUser().isEmpty());
        CHECK_EQ(panel.presets().size(), count - 1);

        // Aufträge: ids steigen, In/Out-Bereich wird festgehalten, Entfernen
        panel.selectPreset(0);
        CHECK(panel.addToQueue(false));
        p.edit("inout", [](Timeline& tl) {
            tl.markIn = 5;
            tl.markOut = 20;
        });
        CHECK(panel.addToQueue(false));
        CHECK_EQ(p.renderQueue().size(), 2);
        if (p.renderQueue().size() == 2) {
            CHECK(p.renderQueue()[0].id == 1 && p.renderQueue()[1].id == 2);
            CHECK(!p.renderQueue()[0].inOut);
            CHECK(p.renderQueue()[1].inOut && p.renderQueue()[1].from == 5 && p.renderQueue()[1].to == 20);
        }
        panel.queuePanel()->removeJobs({1});
        CHECK_EQ(p.renderQueue().size(), 1);
        CHECK(panel.addToQueue(false));
        CHECK_EQ(p.renderQueue().last().id, 3);
    }

    // --- Rendern (klein und kurz)
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    const QString clip = Check::makeMedia(tmp.filePath("clip.mp4"),
                                          {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2", "-f", "lavfi",
                                           "-i", "sine=frequency=440:duration=2", "-c:v", "libx264", "-pix_fmt",
                                           "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!clip.isEmpty())) return Check::result();
    Mlt::Factory::init();
    std::setlocale(LC_NUMERIC, "C");

    Project p;
    {
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        Timeline& tl = d.timeline;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
        TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
        Clip v;
        v.id = 1;
        v.mediaPath = clip;
        v.start = 0;
        v.in = 0;
        v.out = 49;
        v.linkId = 1;
        Clip a = v;
        a.id = 2;
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
        d.lastClipId = 2;
        d.lastLinkId = 1;
        p.load(d);
    }
    auto mk = [&](int id, const QString& file, const QString& format, QSize size = {}) {
        RenderJob j;
        j.id = id;
        j.path = tmp.filePath(file);
        j.settings.format = format;
        j.settings.quality = 2;
        j.size = size;
        return j;
    };
    RenderJob a = mk(1, "a.mp4", "h264", QSize(160, 90));
    a.inOut = true;
    a.from = 10;
    a.to = 29; // 20 Frames = 0,8 s
    RenderJob b = mk(2, "b.wav", "wav");
    RenderJob c = mk(3, "c.mov", "prores");
    c.inOut = true;
    c.from = 0;
    c.to = 4;
    QFile blocker(tmp.filePath("datei"));
    CHECK(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    RenderJob bad = mk(4, "datei/unmöglich.mp4", "h264"); // Ordner ist eine Datei
    RenderJob d = mk(5, "d.m4a", "aac");
    d.status = RenderStatus::Done; // fertig: wird bei „Alle rendern“ übersprungen
    p.setRenderQueue({a, b, c, bad, d});

    RenderQueue q(&p);
    QVector<int> order;
    QObject::connect(&q, &RenderQueue::jobFinished, [&](int id, RenderStatus) { order << id; });
    int progressCalls = 0;
    QObject::connect(&q, &RenderQueue::progress, [&](int, int) { ++progressCalls; });
    int done = -1, failed = -1;
    QObject::connect(&q, &RenderQueue::finished, [&](int dn, int fl) {
        done = dn;
        failed = fl;
    });
    CHECK(q.start());
    CHECK(q.isRunning());
    CHECK(!q.start()); // läuft schon
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{1, 2, 3, 4}));
    CHECK(done == 3 && failed == 1);
    CHECK(progressCalls >= 3);
    CHECK(job(p, 1)->status == RenderStatus::Done && job(p, 1)->renderMs > 0);
    CHECK(job(p, 2)->status == RenderStatus::Done && job(p, 3)->status == RenderStatus::Done);
    CHECK(job(p, 4)->status == RenderStatus::Failed && !job(p, 4)->message.isEmpty());
    CHECK(job(p, 5)->status == RenderStatus::Done && !QFileInfo::exists(d.path)); // nicht neu gerendert

    const QJsonObject pa = probe(a.path);
    const QJsonObject va = stream(pa, "video");
    CHECK_EQ(va.value("codec_name").toString(), QString("h264"));
    CHECK(va.value("width").toInt() == 160 && va.value("height").toInt() == 90);
    CHECK(!stream(pa, "audio").isEmpty());
    const double durA = pa.value("format").toObject().value("duration").toString().toDouble();
    if (!CHECK(std::abs(durA - 0.8) < 0.1)) std::printf("       Dauer a: %.3f\n", durA);
    const QJsonObject pb = probe(b.path);
    CHECK(stream(pb, "video").isEmpty());
    CHECK_EQ(stream(pb, "audio").value("codec_name").toString(), QString("pcm_s24le"));
    const double durB = pb.value("format").toObject().value("duration").toString().toDouble();
    CHECK(std::abs(durB - 2.0) < 0.1);
    const QJsonObject pc = probe(c.path);
    CHECK_EQ(stream(pc, "video").value("codec_name").toString(), QString("prores"));
    CHECK_EQ(stream(pc, "video").value("width").toInt(), 320); // Timeline-Auflösung

    // Einzelnen fertigen Auftrag gezielt neu rendern
    order.clear();
    CHECK(q.start({5}));
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{5}));
    CHECK(QFileInfo::exists(d.path) && stream(probe(d.path), "audio").value("codec_name").toString() == "aac");
    // Ohne Auswahl: alles, was nicht fertig ist -> nur der fehlerhafte Auftrag 4 (schlägt wieder fehl)
    order.clear();
    CHECK(q.start());
    CHECK(waitFor(q));
    CHECK_EQ(order, (QVector<int>{4}));
    CHECK(job(p, 4)->status == RenderStatus::Failed);

    // Abbrechen: laufender Auftrag -> „Abgebrochen“, halbe Datei weg, Rest bleibt „Wartet“
    {
        RenderJob big = mk(6, "gross.mp4", "h264", QSize(1920, 1080));
        big.settings.quality = 0;
        RenderJob after = mk(7, "danach.mp4", "h264");
        p.setRenderQueue({big, after});
        CHECK(q.start());
        QEventLoop loop;
        QObject::connect(&q, &RenderQueue::progress, &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        q.cancel();
        CHECK(!q.isRunning());
        CHECK(job(p, 6)->status == RenderStatus::Canceled);
        CHECK(job(p, 7)->status == RenderStatus::Queued);
        CHECK(!QFileInfo::exists(big.path));
        CHECK(!QFileInfo::exists(after.path));
    }
    return Check::result();
}
