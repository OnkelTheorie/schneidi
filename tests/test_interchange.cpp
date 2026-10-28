// SPDX-License-Identifier: AGPL-3.0-only
// Timeline exchange (core/Interchange): round trips schneidi -> FCP7 XML / OTIO / EDL -> schneidi, a hand-written
// Premiere-style FCP7 XML (start/end -1 at transitions, stereo as two channel items), paths of other computers,
// frame rate conversion
#include "check.h"

#include "core/Interchange.h"
#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QRegularExpression>

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

    // Premiere-style FCP7 XML: dissolve with start/end -1 (in/out of both clips cover the dissolve), stereo file as
    // two channel items, Windows path, 50 fps
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
      <name>A001.mov</name><start>0</start><end>-1</end><in>0</in><out>210</out>
      <file id="file-1"><name>A001.mov</name><pathurl>file://localhost/D%3a/Projekt/A001.mov</pathurl><duration>1000</duration></file>
      <link><linkclipref>clipitem-1</linkclipref></link><link><linkclipref>clipitem-3</linkclipref></link><link><linkclipref>clipitem-4</linkclipref></link>
     </clipitem>
     <transitionitem><start>190</start><end>210</end><alignment>center</alignment>
      <effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid></effect></transitionitem>
     <clipitem id="clipitem-2">
      <name>A002.mov</name><start>-1</start><end>400</end><in>90</in><out>300</out>
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

    // Start timecodes: EDL source times and OTIO ranges count in the file's timecode, FCP7 XML describes it; in/out of
    // FCP7 clip items next to a dissolve cover the part under it (as Premiere and Final Cut write them)
    {
        Seq out = seq;
        applyStartTimecodes(out, [](const QString& path) { return path == kA ? QString("01:00:10:05") : QString(); });
        CHECK_EQ(out.video[0].items[0].mediaStart, 90255);
        CHECK_EQ(out.video[0].items[1].mediaStart, 0);
        Seq e = out;
        const QByteArray edl = write(e, Format::Edl);
        CHECK(edl.contains("001  AX       V     D    010 01:00:10:15 01:00:14:05 01:00:00:00 01:00:03:15")); // a from 10, fading in
        Seq x = out;
        const QString xml = QString::fromUtf8(write(x, Format::Fcp7));
        CHECK(xml.contains("<string>01:00:10:05</string>"));
        // a: in 10, to the dissolve's end (110); b: 10 frames before its in 50 (the dissolve starts at 90)
        CHECK(xml.contains(QRegularExpression("<end>-1</end>\\s*<in>10</in>\\s*<out>120</out>")));
        CHECK(xml.contains(QRegularExpression("<start>-1</start>\\s*<end>200</end>\\s*<in>40</in>\\s*<out>150</out>")));
        Seq o = out;
        const QByteArray otio = write(o, Format::Otio);
        const Seq oi = read(otio, Format::Otio);
        if (CHECK(!oi.video.isEmpty() && !oi.video[0].items.isEmpty())) {
            CHECK_EQ(oi.video[0].items[0].mediaStart, 90255);
            CHECK_EQ(oi.video[0].items[0].in, 10);
        }
        // Reading an EDL: source timecodes become frames of the file once its timecode is known
        Seq ei = read(edl, Format::Edl, FrameRate{25, 1});
        CHECK(ei.inIsTimecode);
        CHECK_EQ(ei.video[0].items[0].in, 90265);
        applyStartTimecodes(ei, [](const QString& path) { return path == kA ? QString("01:00:10:05") : QString(); });
        CHECK(!ei.inIsTimecode);
        CHECK_EQ(ei.video[0].items[0].in, 10);
        // A copy without the timecode, all its clips beyond its end: the full hours are taken off
        Seq copy = read(edl, Format::Edl, FrameRate{25, 1});
        applyStartTimecodes(copy, {}, [](const QString& path) { return path == kA ? 1000 : 0; });
        CHECK_EQ(copy.video[0].items[0].in, 265);
    }

    // Files of other programs (sample data of the OpenTimelineIO project, Apache 2.0)
    const auto otioSample = [](const QString& name) {
        QFile f(QString(SCHNEIDI_TEST_DATA) + "/interchange/" + name);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const auto itemAt = [](const QVector<TrackData>& tracks, int track, int item) -> const Item* {
        return track < tracks.size() && item < tracks[track].items.size() ? &tracks[track].items[item] : nullptr;
    };
    {
        // Premiere: clip items in their own rate (15 fps in a 30 fps sequence), a dissolve with start/end -1 (edit
        // point in its middle), a generator, a nested sequence
        const Seq in = read(otioSample("premiere_example.xml"), Format::Fcp7);
        CHECK(in.format.rate == (FrameRate{30, 1}));
        if (const Item* v1 = itemAt(in.video, 0, 0); CHECK(v1)) {
            CHECK_EQ(v1->start, 536);
            CHECK_EQ(v1->length, 100);
        }
        const Item* a = itemAt(in.video, 3, 0);
        const Item* b = itemAt(in.video, 3, 1);
        if (CHECK(a && b)) {
            CHECK_EQ(a->start, 956);
            CHECK_EQ(a->length, 208);
            CHECK_EQ(a->in, 34);
            CHECK_EQ(b->start, 1164);
            CHECK_EQ(b->length, 82);
            CHECK_EQ(b->in, 18);
            if (CHECK_EQ(in.video[3].transitions.size(), 1)) {
                CHECK_EQ(in.video[3].transitions[0].start, 1152);
                CHECK_EQ(in.video[3].transitions[0].end, 1177);
            }
        }
        CHECK(in.video.size() > 2 && in.video[2].items.isEmpty()); // the title (a generator) is left out
        CHECK(in.warnings.join("\n").contains("test_title"));
        CHECK(in.warnings.join("\n").contains(T("Verschachtelte Sequenzen weggelassen: %1").arg("sc01_sh010_anim")));
        // Its OTIO conversion says the same; the file's start timecode (99) is taken off
        const Seq o = read(otioSample("premiere_example.otio"), Format::Otio);
        const Item* oa = itemAt(o.video, 3, 0);
        const Item* ob = itemAt(o.video, 3, 1);
        if (CHECK(oa && ob)) {
            CHECK_EQ(oa->mediaStart, 99);
            CHECK(oa->start == a->start && oa->length == a->length && oa->in == a->in);
            CHECK(ob->start == b->start && ob->length == b->length && ob->in == b->in);
        }
        if (const Item* wav = itemAt(o.audio, 1, 0); CHECK(wav)) CHECK_EQ(wav->in, 0); // timecode 8497 = its start
        CHECK(o.warnings.join("\n").contains("test_title"));
        // Premiere's generators (bars, black video, counting leader …) are clip items without a file
        const Seq g = read(otioSample("premiere_generators.xml"), Format::Fcp7);
        int items = 0;
        for (const TrackData& td : g.video) items += int(td.items.size());
        CHECK_EQ(items, 0);
        CHECK(g.warnings.join("\n").contains("Universal Counting Leader"));
        // Nuke Studio writes numbers with a decimal point
        const Seq h = read(otioSample("hiero_xml_export.xml"), Format::Fcp7);
        if (const Item* it = itemAt(h.video, 0, 0); CHECK(it)) {
            CHECK_EQ(it->start, 14);
            CHECK_EQ(it->length, 1055);
        }
    }
    {
        // OTIO: a clip without a source range plays its whole media (which starts at 100)
        const Seq c = read(otioSample("simple_cut.otio"), Format::Otio);
        if (const Item* it = itemAt(c.video, 0, 3); CHECK(it)) {
            CHECK_EQ(it->start, 13);
            CHECK_EQ(it->length, 6);
            CHECK_EQ(it->in, 0);
            CHECK_EQ(it->mediaStart, 100);
        }
        // Tracks of old files ("Sequence.1"), generator references
        const Seq gr = read(otioSample("generator_reference_test.otio"), Format::Otio);
        CHECK_EQ(gr.video.size(), 1);
        CHECK(gr.warnings.join("\n").contains(T("Generatoren/Titel weggelassen: %1").arg("C")));
        // Inf, NaN and 128-bit integers in metadata
        const Seq big = read(otioSample("big_int.otio"), Format::Otio);
        CHECK(itemAt(big.video, 0, 0));
        // Dissolves at the edges of a track
        const Seq t = read(otioSample("transition_test.otio"), Format::Otio);
        CHECK(!t.video.isEmpty() && t.video[0].transitions.size() == 3);
    }
    {
        // EDL from Premiere: the timeline starts with its first event (00:59:53:11), not at the full hour
        const Seq e = read(otioSample("premiere_example.edl"), Format::Edl, FrameRate{24, 1});
        if (const Item* it = itemAt(e.video, 0, 0); CHECK(it)) {
            CHECK_EQ(it->start, 0);
            CHECK_EQ(it->length, 31);
            CHECK_EQ(it->in, 86501); // 01:00:04:05 until the file's timecode is known
            CHECK_EQ(it->name, QString("ZZ100_501.take_1.0001.exr"));
        }
        // A dissolve: the outgoing clip plays on under it
        const Seq d = read(otioSample("dissolve_test.edl"), Format::Edl, FrameRate{24, 1});
        if (CHECK(!d.video.isEmpty() && d.video[0].transitions.size() == 1)) {
            CHECK_EQ(d.video[0].transitions[0].start, 9);
            CHECK_EQ(d.video[0].transitions[0].end, 19);
            CHECK_EQ(d.video[0].transitions[0].left, 0);
            CHECK_EQ(d.video[0].transitions[0].right, 1);
        }
        // M2: 47.6 fps on 24 fps
        const Seq sp = read(otioSample("speed_effects_small.edl"), Format::Edl, FrameRate{24, 1});
        bool fast = false;
        for (const TrackData& td : sp.video)
            for (const Item& it : td.items) fast = fast || std::abs(it.speed - 47.6 / 24) < 0.01;
        CHECK(fast);
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
    // Numbers no program writes (found by fuzzing, tests/fuzz): track index -2^31 is the first audio stream, stream
    // 2^31 - 1 (a project file edited by hand) is written as track index 2^31
    {
        Seq s = readFcp7(R"(<xmeml version="4"><sequence><name>s</name><rate><timebase>25</timebase></rate><media><audio>
            <track><clipitem id="c"><name>a.mp4</name><start>0</start><end>10</end><in>0</in><out>10</out>
            <file id="f"><name>a.mp4</name><pathurl>file:///m/a.mp4</pathurl></file>
            <sourcetrack><mediatype>audio</mediatype><trackindex>-2147483648</trackindex></sourcetrack>
            </clipitem></track></audio></media></sequence></xmeml>)");
        if (CHECK(s.audio.size() == 1 && s.audio[0].items.size() == 1)) {
            CHECK_EQ(s.audio[0].items[0].audioStream, 0);
            s.audio[0].items[0].audioStream = 2147483647;
            CHECK(writeFcp7(s).contains("<trackindex>2147483648</trackindex>"));
        }
    }
    // A clip without start that ends at a transition at frame 10 but is 20 frames long: begins at frame 0, its first
    // 10 frames cut off (it began before the timeline)
    {
        const Seq s = readFcp7(R"(<xmeml><sequence><media><video><track><clipitem><in>0</in><out>20</out>
            <file><pathurl>file:///m/a.mp4</pathurl></file></clipitem><transitionitem><start>5</start><end>15</end>
            </transitionitem></track></video></media></sequence></xmeml>)");
        if (CHECK(s.video.size() == 1 && s.video[0].items.size() == 1)) {
            const Item& it = s.video[0].items[0];
            CHECK(it.start == 0 && it.in == 10 && it.length == 10);
        }
        // After a transition that ends before it starts (its cut at 5, 10 frames before its start): the clip would
        // begin 5 frames before the file; it begins 5 frames later instead
        const Seq t = readFcp7(R"(<xmeml><sequence><media><video><track><transitionitem><start>10</start><end>0</end>
            </transitionitem><clipitem><in>0</in><out>20</out><file><pathurl>file:///m/a.mp4</pathurl></file>
            </clipitem></track></video></media></sequence></xmeml>)");
        if (CHECK(t.video.size() == 1 && t.video[0].items.size() == 1)) {
            const Item& it = t.video[0].items[0];
            CHECK(it.start == 10 && it.in == 0 && it.length == 15);
        }
    }
    // FCP7: a timebase of 3 * 10^6 (NTSC: * 1000 overflowed) is not taken, a size of 10^9 pixels is brought inside
    {
        const Seq s = readFcp7(R"(<xmeml><sequence><rate><timebase>3000000</timebase><ntsc>TRUE</ntsc></rate><media><video>
            <format><samplecharacteristics><width>1000000000</width><height>1080</height></samplecharacteristics></format>
            <track><clipitem><in>0</in><out>20</out><start>0</start><end>20</end><file><pathurl>file:///m/a.mp4</pathurl>
            </file></clipitem></track></video></media></sequence></xmeml>)");
        CHECK(s.format.rate == (FrameRate{25, 1}));
        CHECK(s.format.width == kMaxFrameSize && s.format.height == 1080);
    }
    // OTIO: gaps of 2 * 10^9 frames (the track's end overflowed) make the file a broken one; a frame rate of 2^31 fps
    // becomes a possible one
    threw = false;
    try {
        readOtio(R"({"OTIO_SCHEMA":"Timeline.1","tracks":{"children":[{"OTIO_SCHEMA":"Track.1","kind":"Video","children":[
            {"OTIO_SCHEMA":"Gap.1","source_range":{"duration":{"rate":25,"value":2000000000}}},
            {"OTIO_SCHEMA":"Gap.1","source_range":{"duration":{"rate":25,"value":2000000000}}}]}]}})");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
    {
        const Seq s = readOtio(R"({"OTIO_SCHEMA":"Timeline.1","metadata":{"schneidi":{"rate_num":2147483647,"rate_den":1}},
            "global_start_time":{"rate":2147483647,"value":0},"tracks":{"children":[]}})");
        CHECK(s.format.rate.fps() >= 1 && s.format.rate.fps() <= 1000);
    }
    // A clip of -100 frames (moved the next one before the timeline's start): left out
    {
        const Seq s = readOtio(R"({"OTIO_SCHEMA":"Timeline.1","tracks":{"children":[{"OTIO_SCHEMA":"Track.1","kind":"Video",
            "children":[{"OTIO_SCHEMA":"Clip.1","name":"a","media_reference":{"target_url":"file:///m/a.mp4"},
            "source_range":{"duration":{"rate":25,"value":-100},"start_time":{"rate":25,"value":0}}},
            {"OTIO_SCHEMA":"Clip.1","name":"b","media_reference":{"target_url":"file:///m/b.mp4"},
            "source_range":{"duration":{"rate":25,"value":50},"start_time":{"rate":25,"value":0}}}]}]}})");
        if (CHECK(s.video.size() == 1 && s.video[0].items.size() == 2))
            CHECK(s.video[0].items[0].length == 0 && s.video[0].items[1].start == 0 && s.video[0].items[1].length == 50);
    }
    return Check::result();
}
