// Timeline exchange (core/Interchange): round trips schneidi -> FCP7 XML / OTIO / EDL -> schneidi, a hand-written
// Premiere-style FCP7 XML (start/end -1 at transitions, stereo as two channel items), paths of other computers,
// frame rate conversion
#include "check.h"

#include "core/Interchange.h"
#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>

using namespace Interchange;
using Seq = Interchange::Sequence; // ::Sequence is the project's

namespace {

Clip mk(int id, const QString& path, int start, int len, int in, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.linkId = link;
    return c;
}

const QString kA = "/media/a.mp4", kB = "/media/b.mp4", kC = "/media/c c.mp4", kM = "/media/music.wav";

// V1: fade in on a, dissolve a|b (20, centered), c at 50 % with opacity keys; A1: sound of a and b (linked, a with
// volume keys), A2: music from stream 2 with a fade out; markers
Timeline sample()
{
    Timeline tl;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 2);
    Clip a = mk(1, kA, 0, 100, 10, 1), b = mk(2, kB, 100, 100, 50, 2);
    a.transIn = 10;
    a.transOut = b.transIn = 20;
    Clip c = mk(3, kC, 250, 50, 20);
    c.speed = 0.5;
    c.transform.opacity = 60;
    c.keys[AnimParam::Opacity] = {Keyframe{20, 100}, Keyframe{40, 30}};
    c.fadeOut = 10; // a fade handle: written as a fade out
    tl.video[0].clips = {a, b, c};
    Clip as = mk(4, kA, 0, 100, 10, 1), bs = mk(5, kB, 100, 100, 50, 2);
    as.volumeDb = -6;
    as.keys[AnimParam::Volume] = {Keyframe{10, -6}, Keyframe{60, -20}};
    tl.audio[0].clips = {as, bs};
    Clip m = mk(6, kM, 0, 300, 0);
    m.audioStream = 1;
    m.transOut = 25;
    m.enabled = true;
    tl.audio[1].clips = {m};
    tl.audio[1].name = "Music";
    tl.markers = {50, 150};
    return tl;
}

void addMedia(Project& p)
{
    for (const QString& path : {kA, kB, kC, kM}) {
        MediaInfo m;
        m.path = path;
        m.name = QFileInfo(path).fileName();
        m.length = 1000;
        m.hasVideo = path != kM;
        m.hasAudio = true;
        if (path == kM) m.audioStreams = {AudioStreamInfo{2, "a"}, AudioStreamInfo{2, "b"}};
        p.addMedia(m);
    }
}

// What must survive a round trip, clip by clip
// edl: only cuts, speed and transitions (no volume, opacity, links, streams, markers)
void compareTimelines(const Timeline& got, const Timeline& want, const char* what, bool edl = false)
{
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& g = got.tracks(kind);
        const QVector<Track>& w = want.tracks(kind);
        if (!CHECK(g.size() >= w.size())) {
            std::printf("       %s\n", what);
            return;
        }
        for (int t = 0; t < w.size(); ++t) {
            if (!CHECK_EQ(g[t].clips.size(), w[t].clips.size())) {
                std::printf("       %s track %d\n", what, t);
                continue;
            }
            for (int i = 0; i < w[t].clips.size(); ++i) {
                const Clip& x = g[t].clips[i];
                const Clip& y = w[t].clips[i];
                CHECK_EQ(x.mediaPath, y.mediaPath);
                CHECK_EQ(x.speed, y.speed);
                if (edl) continue;
                {
                    CHECK_EQ(x.start, y.start);
                    CHECK_EQ(x.in, y.in);
                    CHECK_EQ(x.out, y.out);
                    CHECK_EQ(x.transIn, y.transIn);
                    CHECK_EQ(x.transOut, y.transOut);
                }
                CHECK_EQ(x.audioStream, y.audioStream);
                CHECK(std::abs(x.volumeDb - y.volumeDb) < 0.01);
                CHECK(std::abs(x.transform.opacity - y.transform.opacity) < 0.01);
                CHECK_EQ(x.keys.value(AnimParam::Opacity).size(), y.keys.value(AnimParam::Opacity).size());
                CHECK_EQ(x.keys.value(AnimParam::Volume).size(), y.keys.value(AnimParam::Volume).size());
                for (int k = 0; k < x.keys.value(AnimParam::Volume).size() && k < y.keys.value(AnimParam::Volume).size(); ++k) {
                    CHECK_EQ(x.keys[AnimParam::Volume][k].frame, y.keys[AnimParam::Volume][k].frame);
                    CHECK(std::abs(x.keys[AnimParam::Volume][k].value - y.keys[AnimParam::Volume][k].value) < 0.01);
                }
                CHECK_EQ(x.linkId != 0, y.linkId != 0);
            }
        }
    }
    if (!edl) CHECK_EQ(got.markers, want.markers);
}

// Effective transitions of a track as [start, end) spans
QVector<QPair<int, int>> spans(const Track& t)
{
    QVector<QPair<int, int>> out;
    for (const auto& s : TimelineOps::transitions(t, [](const Clip&) { return 1000; })) out << qMakePair(s.start, s.end);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("interchange");

    Project p;
    addMedia(p);
    const Timeline original = sample();
    p.setTimeline(original);
    // What comes back: the fade handle of c as a fade out
    Timeline want = original;
    want.video[0].clips[2].fadeOut = 0;
    want.video[0].clips[2].transOut = 10;
    p.renameSequence(p.currentSequence(), "Cut 1");

    int id = 100, link = 100;
    const auto newId = [&id] { return ++id; };
    const auto newLink = [&link] { return ++link; };

    const Seq seq = fromTimeline(p);
    CHECK_EQ(seq.name, QString("Cut 1"));
    CHECK_EQ(seq.video.size(), 1);
    CHECK_EQ(seq.audio.size(), 2);
    CHECK_EQ(seq.video[0].transitions.size(), 3); // fade in + dissolve + fade handle of c
    CHECK_EQ(seq.audio[1].transitions.size(), 1); // fade out
    CHECK_EQ(mediaFiles(seq), (QStringList{kA, kB, kC, kM}));

    // FCP7 XML and OTIO: lossless for what they carry
    for (const Format f : {Format::Fcp7, Format::Otio}) {
        Seq out = seq;
        const QByteArray data = write(out, f);
        CHECK(!data.isEmpty());
        if (const QString dump = qEnvironmentVariable("INTERCHANGE_DUMP"); !dump.isEmpty()) { // look at the files
            QFile file(QDir(dump).filePath("sample." + extension(f)));
            if (file.open(QIODevice::WriteOnly)) file.write(data);
        }
        Seq in;
        try {
            in = read(data, f);
        } catch (const Error& e) {
            CHECK(false && "read failed");
            std::printf("       %s\n", qPrintable(e.message));
            continue;
        }
        CHECK_EQ(in.name, QString("Cut 1"));
        CHECK(in.format == p.format());
        CHECK(in.hasSize);
        CHECK_EQ(in.audio.size(), 2);
        CHECK_EQ(in.audio.size() > 1 ? in.audio[1].name : QString(), f == Format::Otio ? QString("Music") : QString());
        const Timeline tl = toTimeline(in, newId, newLink);
        compareTimelines(tl, want, f == Format::Fcp7 ? "fcp7" : "otio");
        CHECK_EQ(spans(tl.video[0]), spans(want.video[0]));
        CHECK_EQ(spans(tl.audio[1]), spans(want.audio[1]));
        // linked picture and sound stay pairs: a with a, b with b
        CHECK(tl.video[0].clips.size() == 3 && tl.audio[0].clips.size() == 2
              && tl.video[0].clips[0].linkId == tl.audio[0].clips[0].linkId
              && tl.video[0].clips[1].linkId == tl.audio[0].clips[1].linkId
              && tl.video[0].clips[0].linkId != tl.video[0].clips[1].linkId);
    }

    // EDL: one video track and audio, no volume; a dissolve moves the cut to its start (same picture)
    {
        Seq out = seq;
        const QByteArray data = write(out, Format::Edl);
        if (const QString dump = qEnvironmentVariable("INTERCHANGE_DUMP"); !dump.isEmpty()) {
            QFile file(QDir(dump).filePath("sample.edl"));
            if (file.open(QIODevice::WriteOnly)) file.write(data);
        }
        CHECK(data.contains("FCM: NON-DROP FRAME"));
        CHECK(data.contains("* SOURCE FILE: /media/c c.mp4"));
        CHECK(data.contains("M2   AX       012.5")); // 50 % at 25 fps
        CHECK(out.warnings.contains(T("EDL: Lautstärke und Deckkraft nicht geschrieben")));
        const Seq in = read(data, Format::Edl, FrameRate{25, 1});
        const Timeline tl = toTimeline(in, newId, newLink);
        compareTimelines(tl, want, "edl", true);
        CHECK_EQ(spans(tl.video[0]), spans(want.video[0])); // fade in [0,10), dissolve [90,110)
        CHECK_EQ(spans(tl.audio[1]), spans(want.audio[1]));
        if (CHECK_EQ(tl.video[0].clips.size(), 3)) {
            const Clip& a = tl.video[0].clips[0];
            const Clip& b = tl.video[0].clips[1];
            const Clip& c = tl.video[0].clips[2];
            CHECK_EQ(a.start, 0);
            CHECK_EQ(a.in, 10);
            CHECK_EQ(b.start, 90); // the dissolve's start
            CHECK_EQ(b.in, 40);    // the same source frame at timeline 100 as before (50)
            CHECK_EQ(b.end(), 200);
            CHECK_EQ(c.start, 250);
            CHECK_EQ(c.in, 20);
            CHECK_EQ(c.length(), 50);
        }
    }

    // Premiere-style FCP7 XML: dissolve with start/end -1, stereo file as two channel items, Windows path, 50 fps
    const QByteArray premiere = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE xmeml>
<xmeml version="4">
 <sequence id="sequence-1">
  <name>Sequenz 01</name>
  <rate><timebase>50</timebase><ntsc>FALSE</ntsc></rate>
  <media>
   <video>
    <format><samplecharacteristics><width>3840</width><height>2160</height></samplecharacteristics></format>
    <track>
     <clipitem id="clipitem-1">
      <name>A001.mov</name><start>0</start><end>-1</end><in>0</in><out>200</out>
      <file id="file-1"><name>A001.mov</name><pathurl>file://localhost/D%3a/Projekt/A001.mov</pathurl><duration>1000</duration></file>
      <link><linkclipref>clipitem-1</linkclipref></link><link><linkclipref>clipitem-3</linkclipref></link><link><linkclipref>clipitem-4</linkclipref></link>
     </clipitem>
     <transitionitem><start>190</start><end>210</end><alignment>center</alignment>
      <effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid></effect></transitionitem>
     <clipitem id="clipitem-2">
      <name>A002.mov</name><start>-1</start><end>400</end><in>100</in><out>300</out>
      <file id="file-2"><name>A002.mov</name><pathurl>file://localhost/D%3a/Projekt/A002.mov</pathurl></file>
      <filter><effect><name>Gaussian Blur</name><effectid>GaussianBlur</effectid></effect></filter>
     </clipitem>
     <generatoritem id="gen-1"><name>Farbfläche</name><start>400</start><end>450</end></generatoritem>
    </track>
   </video>
   <audio>
    <track>
     <clipitem id="clipitem-3"><name>A001.mov</name><start>0</start><end>200</end><in>0</in><out>200</out>
      <file id="file-1"/><sourcetrack><mediatype>audio</mediatype><trackindex>1</trackindex></sourcetrack>
      <filter><effect><name>Audio Levels</name><effectid>audiolevels</effectid>
       <parameter><parameterid>level</parameterid><value>0.5</value></parameter></effect></filter>
     </clipitem>
    </track>
    <track>
     <clipitem id="clipitem-4"><name>A001.mov</name><start>0</start><end>200</end><in>0</in><out>200</out>
      <file id="file-1"/><sourcetrack><mediatype>audio</mediatype><trackindex>2</trackindex></sourcetrack>
     </clipitem>
    </track>
   </audio>
  </media>
  <marker><name>Hier</name><in>120</in><out>-1</out></marker>
 </sequence>
</xmeml>)";
    {
        Seq in = readFcp7(premiere);
        CHECK_EQ(in.name, QString("Sequenz 01"));
        CHECK(in.format.rate == (FrameRate{50, 1}));
        CHECK_EQ(in.format.width, 3840);
        CHECK_EQ(in.markers, QVector<int>{120});
        if (CHECK_EQ(in.video.size(), 1) && CHECK_EQ(in.video[0].items.size(), 2)) {
            const Item& a = in.video[0].items[0];
            const Item& b = in.video[0].items[1];
            CHECK_EQ(a.path, QString("D:/Projekt/A001.mov"));
            CHECK_EQ(a.start, 0);
            CHECK_EQ(a.length, 200);
            CHECK_EQ(a.mediaLength, 1000);
            CHECK_EQ(b.start, 200); // edit point = middle of the centered dissolve
            CHECK_EQ(b.in, 100);
            CHECK_EQ(b.length, 200);
            if (CHECK_EQ(in.video[0].transitions.size(), 1)) {
                CHECK_EQ(in.video[0].transitions[0].left, 0);
                CHECK_EQ(in.video[0].transitions[0].right, 1);
            }
            CHECK(a.link > 0);
            CHECK_EQ(b.link, 0);
        }
        CHECK(in.warnings.join("\n").contains("Gaussian Blur"));
        CHECK(in.warnings.join("\n").contains("Farbfläche"));
        if (CHECK_EQ(in.audio.size(), 2)) {
            CHECK(std::abs(in.audio[0].items[0].volumeDb + 6.02) < 0.05);
            CHECK_EQ(in.audio[1].items[0].audioStream, 1);
            CHECK_EQ(in.audio[0].items[0].link, in.video[0].items[0].link);
        }
        // The file has one (stereo) stream: the second channel item is left out
        fitAudioStreams(in, [](const QString&) { return 1; });
        CHECK_EQ(in.audio[1].items[0].length, 0);
        CHECK_EQ(mediaFiles(in), (QStringList{"D:/Projekt/A001.mov", "D:/Projekt/A002.mov"}));
        // To 25 fps: everything at half the frame numbers
        convertRate(in, FrameRate{25, 1});
        CHECK_EQ(in.video[0].items[1].start, 100);
        CHECK_EQ(in.video[0].items[1].in, 50);
        CHECK_EQ(in.markers, QVector<int>{60});
        const Timeline tl = toTimeline(in, newId, newLink);
        CHECK_EQ(tl.audio[1].clips.size(), 0);
        CHECK_EQ(spans(tl.video[0]), (QVector<QPair<int, int>>{{95, 105}}));
    }

    // Paths
    CHECK_EQ(pathFromUrl("file://localhost/C%3a/Users/x/a%20b.mp4"), QString("C:/Users/x/a b.mp4"));
    CHECK_EQ(pathFromUrl("file:///home/x/a%20b.mp4"), QString("/home/x/a b.mp4"));
    CHECK_EQ(pathFromUrl("C:\\Users\\x\\a.mp4"), QString("C:/Users/x/a.mp4"));
    CHECK_EQ(pathFromUrl(urlFromPath("/tmp/ä ö.mp4")), QString("/tmp/ä ö.mp4"));
    CHECK(formatOf("x.EDL") == Format::Edl && formatOf("x.xml") == Format::Fcp7 && formatOf("x.otio") == Format::Otio);
    CHECK(!formatOf("x.mp4"));

    // Broken files
    bool threw = false;
    try {
        readFcp7("<xmeml><sequence>");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        readOtio("{\"OTIO_SCHEMA\": \"Clip.1\"}");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
    return Check::result();
}
