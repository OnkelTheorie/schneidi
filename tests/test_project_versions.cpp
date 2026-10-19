// Project files of every format version load (migration steps in ProjectFile.cpp) and are written back as the current
// version without losing anything. Fixtures: tests/data/projects/v<format>-<schneidi version>.schneidi, media offline.
// New format version -> add a fixture saved by the last release before it (docs/dev/konventionen.md).
#include "check.h"

#include "core/Keyframes.h"
#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

const QString kData = QStringLiteral(SCHNEIDI_TEST_DATA "/projects");

const Clip* clip(const Timeline& tl, int id)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (const Clip& c : t.clips)
                if (c.id == id) return &c;
    return nullptr;
}

// Every fixture: loads, saves as the current version, and a second save gives the same file (nothing lost or added)
void testRoundTrip(const QString& tmp)
{
    const QStringList files = QDir(kData).entryList({"*.schneidi"}, QDir::Files, QDir::Name);
    CHECK(files.size() >= 2);
    for (const QString& name : files) {
        ProjectData d;
        QString err;
        if (!Check::report(ProjectFile::load(kData + "/" + name, &d, &err), qPrintable(name), __FILE__, __LINE__, err))
            continue;
        const QString path = tmp + "/" + name;
        const QByteArray first = ProjectFile::toJson(d, path);
        const QJsonObject root = QJsonDocument::fromJson(first).object();
        CHECK_EQ(root.value("version").toInt(), 2);
        ProjectData again;
        CHECK(ProjectFile::fromJson(first, path, &again, &err));
        Check::report(ProjectFile::toJson(again, path) == first, qPrintable(name + ": save -> load -> save is stable"),
                      __FILE__, __LINE__);
    }
}

// Format 1 (0.1): only "fps", track names only as abbreviation, no format block
void testVersion1()
{
    ProjectData d;
    QString err;
    if (!CHECK(ProjectFile::load(kData + "/v1-0.1.schneidi", &d, &err))) return;
    CHECK_EQ(d.format.width, 1920);
    CHECK_EQ(d.format.height, 1080);
    CHECK_EQ(d.format.rate.num, 30);
    CHECK_EQ(d.format.rate.den, 1);
    CHECK_EQ(d.playhead, 42);
    CHECK_EQ(d.media.size(), 2);
    const Timeline& tl = d.timeline;
    CHECK_EQ(tl.video.size(), 2);
    CHECK_EQ(tl.audio.size(), 2);
    CHECK(tl.video[0].name.isEmpty()); // abbreviation only -> default name
    CHECK(tl.video[1].hidden);
    CHECK(tl.audio[1].muted);
    CHECK_EQ(tl.markers, (QVector<int>{30, 200}));
    if (const Clip* c = clip(tl, 1); CHECK(c)) {
        CHECK_EQ(c->in, 10);
        CHECK_EQ(c->out, 149);
        CHECK_EQ(c->transform.zoomX, 1.2);
        CHECK_EQ(c->transform.cropLeft, 40.0);
        CHECK_EQ(c->transform.opacity, 80.0);
        CHECK_EQ(c->transform.posY, -60.0);
        CHECK(c->linkId != 0);
    }
    if (const Clip* c = clip(tl, 3); CHECK(c)) CHECK(!c->enabled);
    if (const Clip* c = clip(tl, 2); CHECK(c)) {
        CHECK_EQ(c->volumeDb, -6.0);
        CHECK_EQ(c->audioStream, 0); // format 2 field, absent in 1
    }
    if (const Clip* c = clip(tl, 4); CHECK(c)) CHECK_EQ(c->pan, -0.5);
    if (const Clip* c = clip(tl, 5); CHECK(c) && CHECK_EQ(c->effects.size(), 1)) {
        CHECK_EQ(c->effects[0].effectId, QString("chromakey"));
        CHECK_EQ(c->effects[0].params.value("distance").toDouble(), 0.25);
    }
}

// Format 2 (0.9): everything a current project can hold
void testVersion2()
{
    ProjectData d;
    QString err;
    if (!CHECK(ProjectFile::load(kData + "/v2-0.9.schneidi", &d, &err))) return;
    const Timeline& tl = d.timeline;
    CHECK_EQ(tl.video[0].name, QString("Kamera"));
    CHECK(!tl.subtitles.isEmpty() && tl.subtitles[0].cues.size() == 1);
    CHECK_EQ(tl.audio[0].volumeDb, -3.0);
    CHECK(tl.masterLimiter);
    bool title = false, ramp = false, window = false, keys = false;
    QVector<const Timeline*> all{&d.timeline}; // the opened timeline is in `timeline`, not in its sequence
    for (const Sequence& s : d.sequences) all << &s.timeline;
    for (const Timeline* x : all)
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : x->tracks(k))
                for (const Clip& c : t.clips) {
                    title |= c.isTitle() && c.title.text == "Hallo\nWelt";
                    ramp |= !c.ramp.isEmpty();
                    window |= c.effects.size() && std::any_of(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) {
                                  return e.effectId == "grade" && e.params.contains("w1.shape");
                              });
                    keys |= !c.keys.value(AnimParam::ZoomX).isEmpty();
                }
    CHECK(title);
    CHECK(ramp);
    CHECK(window);
    CHECK(keys);
    int compound = 0, multicam = 0, named = 0;
    for (const Sequence& s : d.sequences) {
        compound += s.compound && !s.multicam;
        multicam += s.multicam;
        named += s.name == "Zweite";
    }
    CHECK_EQ(compound, 1);
    CHECK_EQ(multicam, 1);
    CHECK_EQ(named, 1);
    const auto a = std::find_if(d.media.begin(), d.media.end(), [](const MediaInfo& m) { return m.name == "a.mp4"; });
    if (CHECK(a != d.media.end())) {
        CHECK_EQ(a->clipColor, QString("purple"));
        CHECK_EQ(a->flags, QStringList{"red"});
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("project-versions");
    QTemporaryDir tmp;
    testRoundTrip(tmp.path());
    testVersion1();
    testVersion2();
    return Check::result();
}
