// Project files of every format version load (migration steps in ProjectFile.cpp) and are written back as the current
// version without losing anything. Fixtures: tests/data/projects/v<format>-<schneidi version>.schneidi, media offline.
// New format version -> add a fixture saved by the last release before it (docs/dev/konventionen.md).
#include "check.h"

#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>

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
        CHECK_EQ(root.value("version").toInt(), 3);
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

// Format 2 -> 3 (0.12): crop measured on the clip's picture fitted into the frame. Fixture saved before the change (9:16
// timeline 1080x1920, media offline: their shapes come from a stand-in probe). Converted so every clip crops as before.
void testVersion2Crop()
{
    int asked = 0;
    MediaAspect::setProbe([&asked](const QString& path) {
        ++asked;
        return path.endsWith("grad.mkv") ? 16.0 / 9 : path.endsWith("green.mkv") ? 9.0 / 16 : path.endsWith("sq.png") ? 1.0 : 0.0;
    });
    ProjectData d;
    QString err;
    if (!CHECK(ProjectFile::load(kData + "/v2-0.12dev-crop.schneidi", &d, &err))) return;
    CHECK_EQ(d.format.width, 1080);
    const Timeline& tl = d.timeline;
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    // Shaped like the frame (9:16): unchanged
    if (const Clip* c = clip(tl, 1); CHECK(c)) CHECK(near(c->transform.cropLeft, 50) && near(c->transform.cropTop, 30));
    // 16:9 without transform: was measured from the frame's edges, bars (656.25 px) included; 600 px off the bottom
    // only cut into the bar
    if (const Clip* c = clip(tl, 2); CHECK(c)) {
        CHECK(near(c->transform.cropTop, 700 - 656.25));
        CHECK(near(c->transform.cropBottom, 0));
        CHECK(near(c->transform.cropLeft, 100));
    }
    // 16:9 at zoom 1.5: was a share of the frame size (x of 1080, y of 1920) -> times the fitted picture (1080x607.5)
    if (const Clip* c = clip(tl, 3); CHECK(c)) {
        CHECK(near(c->transform.cropTop, 100 * 607.5 / 1920));
        CHECK(near(c->transform.cropBottom, 50 * 607.5 / 1920));
        CHECK(near(c->transform.cropRight, 200));
        CHECK_EQ(c->transform.zoomX, 1.5);
    }
    // Keyframes without transform: each value converted; the crop_top animation 600 -> 800 crossed the bar's edge
    // (656.25) and gets a keyframe there, so it starts to cut into the picture at the same frame as before
    if (const Clip* c = clip(tl, 4); CHECK(c)) {
        const KeyTrack bottom = c->keys.value(AnimParam::CropBottom), top = c->keys.value(AnimParam::CropTop);
        if (CHECK_EQ(bottom.size(), 2)) CHECK(near(bottom[0].value, 43.75) && near(bottom[1].value, 243.75));
        if (CHECK_EQ(top.size(), 3)) {
            CHECK(near(top[0].value, 0) && near(top[1].value, 0) && near(top[2].value, 143.75));
            CHECK_EQ(top[1].frame - c->in, 28); // 600 + 200 * 28/99 = 656.6
        }
        CHECK(near(c->transform.cropLeft, 20));
    }
    // A square image (fitted 1080x1080, bars of 420 px above and below)
    if (const Clip* c = clip(tl, 5); CHECK(c)) CHECK(near(c->transform.cropTop, 80) && near(c->transform.cropRight, 300));
    // A title is the frame itself
    if (const Clip* c = clip(tl, 6); CHECK(c) && CHECK(c->isTitle()))
        CHECK(near(c->transform.cropLeft, 400) && near(c->transform.cropTop, 900));
    CHECK(asked >= 3);
    // Saved as format 3 and loaded again: nothing converted twice
    QTemporaryDir tmp;
    const QString path = tmp.filePath("again.schneidi");
    if (!CHECK(ProjectFile::save(d, path, &err))) return;
    ProjectData again;
    if (CHECK(ProjectFile::load(path, &again, &err))) CHECK(again.timeline == d.timeline);
    MediaAspect::setProbe({});
    // Without a probe (or the media offline) the values stay as they are
    ProjectData plain;
    if (CHECK(ProjectFile::load(kData + "/v2-0.12dev-crop.schneidi", &plain, &err)))
        if (const Clip* c = clip(plain.timeline, 2); CHECK(c)) CHECK(near(c->transform.cropTop, 700));
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
    testVersion2Crop();
    return Check::result();
}
