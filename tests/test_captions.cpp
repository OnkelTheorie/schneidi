// SPDX-License-Identifier: AGPL-3.0-only
// Word captions (core/Captions): word times and the spoken word at a frame, pop-in values, line breaks, cues from
// transcript words (also on beats), word times through split/ripple/trim and through clip trims and speed
// (TranscriptCut), WebVTT, project file, the CLI op 'captions', the karaoke highlight in a rendered frame and
// subtitle text streams in exported MP4/MKV/WebM (read back with libavformat).

#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Captions.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TranscriptCut.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "engine/video/Text.h"

#include <QApplication>
#include <QEventLoop>
#include <QDebug>
#include <QFile>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

QString wordsOf(const SubtitleCue& c)
{
    QStringList out;
    const QStringList list = Captions::words(c.text);
    const QVector<CueWord> t = Captions::timing(c);
    for (int i = 0; i < list.size(); ++i) out << QString("%1@%2-%3").arg(list[i]).arg(t[i].from).arg(t[i].to);
    return QString("%1-%2:").arg(c.start).arg(c.end) + out.join(' ');
}

SubtitleCue cue(int start, int end, const QString& text, QVector<CueWord> words = {})
{
    SubtitleCue c{0, start, end, text};
    c.words = words;
    return c;
}

void testTiming()
{
    const SubtitleCue c = cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}});
    CHECK_EQ(Captions::activeWord(c, 99), -1);
    CHECK_EQ(Captions::activeWord(c, 100), 0);
    CHECK_EQ(Captions::activeWord(c, 104), 0);
    CHECK_EQ(Captions::activeWord(c, 105), 1);
    CHECK_EQ(Captions::activeWord(c, 119), 1); // stays lit in the pause after it
    CHECK_EQ(Captions::activeWord(c, 120), 2);
    CHECK_EQ(Captions::activeWord(c, 149), 2);
    CHECK_EQ(Captions::activeWord(c, 150), -1); // end exclusive
    // Late first word: nothing lit before it
    CHECK_EQ(Captions::activeWord(cue(0, 50, "x y", {{10, 20}, {20, 30}}), 5), -1);
    // Text edited (other word count) or an SRT cue without times: shared by length (+1 for the space)
    CHECK_EQ(wordsOf(cue(0, 10, "aa bb", {{0, 5}, {5, 6}, {6, 9}})), QString("0-10:aa@0-5 bb@5-10"));
    CHECK_EQ(wordsOf(cue(0, 12, "a\nbbbb")), QString("0-12:a@0-3 bbbb@3-12"));
}

void testPop()
{
    const Captions::Pop start = Captions::popIn(0, 120);
    CHECK(std::abs(start.scale - 0.6) < 1e-9);
    CHECK_EQ(start.opacity, 0.0);
    CHECK_EQ(Captions::popIn(60, 120).opacity, 1.0); // visible after half the time
    const Captions::Pop done = Captions::popIn(120, 120);
    CHECK(done.scale == 1.0 && done.opacity == 1.0);
    CHECK(Captions::popIn(30, 0).scale == 1.0); // no animation
    double peak = 0;
    for (int ms = 0; ms <= 120; ++ms) peak = std::max(peak, Captions::popIn(ms, 120).scale);
    CHECK(peak > 1.05 && peak < 1.12); // overshoot ("pop")
    CHECK(Captions::popIn(119, 120).scale > 1.0);
}

void testLines()
{
    CHECK_EQ(Captions::lineStarts({10, 10, 10, 10}, 1, 25, 0), (QVector<int>{0, 2}));
    CHECK_EQ(Captions::lineStarts({10, 10, 10, 10}, 1, 100, 3), (QVector<int>{0, 3}));
    CHECK_EQ(Captions::lineStarts({10, 10}, 1, 100, 1), (QVector<int>{0, 1}));
    CHECK_EQ(Captions::lineStarts({50, 5}, 1, 20, 0), (QVector<int>{0, 1})); // a word wider than the line stays alone
    CHECK(Captions::lineStarts({}, 1, 20, 0).isEmpty());
}

TranscriptCut::Word word(int from, int to, const QString& text)
{
    TranscriptCut::Word w;
    w.from = from;
    w.to = w.spokenTo = to;
    w.text = text;
    return w;
}

void testFromWords()
{
    Captions::Options o;
    o.maxWords = 3;
    o.maxGapFrames = 15;
    o.minFrames = 10;
    const QVector<TranscriptCut::Word> words{word(10, 14, "One"), word(14, 18, "two."), word(20, 24, "Three"),
                                             word(24, 28, "four"),  word(28, 30, "five"), word(31, 33, "six"),
                                             word(60, 64, "late")};
    const QVector<SubtitleCue> cues = Captions::fromWords(words, o);
    QStringList got;
    for (const SubtitleCue& c : cues) got << wordsOf(c);
    // Sentence end, then max 3 words, then the long pause; short cues last 10 frames up to the next one
    CHECK_EQ(got.join(" | "), QString("10-20:One@0-4 two.@4-8 | 20-30:Three@0-4 four@4-8 five@8-10 | "
                                      "31-41:six@0-2 | 60-70:late@0-4"));
    // One word per beat: each word on the nearest free beat, lasting to the next word's beat
    const QVector<TranscriptCut::Word> beat = Captions::onBeats({word(9, 11, "a"), word(12, 14, "b"), word(33, 35, "c")},
                                                                {10, 20, 30, 40});
    CHECK_EQ(beat.size(), 3);
    CHECK(beat.size() == 3 && beat[0].from == 10 && beat[0].to == 20 && beat[1].from == 20 && beat[2].from == 30
          && beat[2].to == 32);
    // More words than beats: the rest is left out
    CHECK_EQ(Captions::onBeats({word(0, 2, "a"), word(2, 4, "b")}, {5}).size(), 1);
}

void testEdits()
{
    // Split: each part keeps the words spoken in it (a plain cue keeps its text on both sides)
    SubtitleTrack t;
    t.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
    t.cues[0].id = 1;
    int next = 10;
    const int right = Subtitles::splitAt(t, 115, [&] { return next++; });
    CHECK_EQ(right, 10);
    CHECK_EQ(t.cues.size(), 2);
    if (t.cues.size() == 2) {
        CHECK_EQ(wordsOf(t.cues[0]), QString("100-115:a@0-5 bb@5-10"));
        CHECK_EQ(wordsOf(t.cues[1]), QString("115-150:ccc@5-15"));
    }
    // Ripple: removing frames 104..112 takes "bb" (105-110) out and moves "ccc" up
    SubtitleTrack r;
    r.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
    CHECK(Subtitles::ripple(r, {{112, -8}}));
    CHECK_EQ(wordsOf(r.cues[0]), QString("100-142:a@0-4 ccc@12-22"));
    // Overwritten by another cue at the end: the covered words go
    SubtitleTrack o;
    o.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
    Subtitles::place(o, cue(118, 160, "neu"));
    CHECK_EQ(wordsOf(o.cues[0]), QString("100-118:a@0-5 bb@5-10"));
    // Editor: trimming the start keeps the words where they are spoken; moving keeps them with the cue
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    SubtitleCue c = cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}});
    CaptionStyle style;
    style.mode = CaptionMode::Karaoke;
    CHECK_EQ(ed.addCaptions({c}, "Wörter", style), 0);
    const int id = p.timeline().subtitles[0].cues[0].id;
    CHECK(p.timeline().subtitles[0].captions.mode == CaptionMode::Karaoke);
    ed.trimSubtitle(id, TimelineOps::Edge::Start, 3);
    CHECK_EQ(wordsOf(p.timeline().subtitles[0].cues[0]), QString("103-150:bb@2-7 ccc@17-27"));
    ed.setSubtitleTiming(id, 203, 250);
    CHECK_EQ(wordsOf(p.timeline().subtitles[0].cues[0]), QString("203-250:bb@2-7 ccc@17-27"));
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK_EQ(wordsOf(p.timeline().subtitles[0].cues[0]), QString("100-150:a@0-5 bb@5-10 ccc@20-30"));
}

void testSpeed()
{
    // Word times of the media transcript through the clip's trim and speed
    MediaInfo m;
    m.path = "/x/talk.wav";
    m.hasAudio = true;
    m.length = 250;
    MediaTranscript tr;
    tr.words = {{1000, 1400, "Hallo"}, {2000, 2400, "Welt"}};
    m.transcripts << tr;
    Timeline tl;
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = m.path;
    c.start = 100;
    c.in = 0;
    c.out = 99;
    tl.audio[0].clips << c;
    auto words = [&](bool retimed) {
        QStringList out;
        for (const TranscriptCut::Word& w : TranscriptCut::timelineWords(tl, {m}, 25, -1, retimed))
            out << QString("%1@%2-%3").arg(w.text).arg(w.from).arg(w.spokenTo);
        return out.join(' ');
    };
    CHECK_EQ(words(true), QString("Hallo@125-135 Welt@150-160"));
    tl.audio[0].clips[0].in = 10; // trimmed: everything 10 frames earlier
    CHECK_EQ(words(true), QString("Hallo@115-125 Welt@140-150"));
    tl.audio[0].clips[0].in = 0;
    tl.audio[0].clips[0].speed = 2.0; // twice as fast: source frame 25 is material frame 12.5
    tl.audio[0].clips[0].out = 49;
    CHECK_EQ(words(false), QString()); // cutting by text keeps to normal speed
    CHECK_EQ(words(true), QString("Hallo@113-118 Welt@125-130"));
    tl.audio[0].clips[0].reverse = true; // backwards: no words
    CHECK_EQ(words(true), QString());
}

void testVtt()
{
    const QVector<SubtitleCue> cues{{1, 25, 50, "Hallo <b> & -->\n\nWelt"}, {2, 75, 3000, "Lang"}};
    CHECK_EQ(QString::fromUtf8(Subtitles::toVtt(cues, 25)),
             QString("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nHallo &lt;b> &amp; --&gt;\nWelt\n\n"
                     "00:00:03.000 --> 00:02:00.000\nLang\n\n"));
    // Range: times from its start, the end cut
    CHECK_EQ(QString::fromUtf8(Subtitles::toVtt(cues, 25, 50, 100)), QString("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nLang\n\n"));
    CHECK(Subtitles::toFile("/x/a.VTT", cues, 25).startsWith("WEBVTT"));
    CHECK(Subtitles::toFile("/x/a.srt", cues, 25).startsWith("1\r\n"));
    // Text tracks for embedding: the visible track first, empty ones left out
    Timeline tl;
    SubtitleTrack a, b, empty;
    a.name = "Deutsch";
    a.enabled = false;
    a.cues = {{3, 0, 25, "Eins"}};
    b.name = "English";
    b.cues = {{4, 50, 75, "Two\nlines"}};
    tl.subtitles = {a, empty, b};
    const QVector<Subtitles::TextTrack> text = Subtitles::textTracks(tl, 25);
    CHECK_EQ(text.size(), 2);
    if (text.size() == 2) {
        CHECK(text[0].main && text[0].name == "English" && text[0].cues.size() == 1);
        CHECK(text[0].cues[0].fromMs == 2000 && text[0].cues[0].toMs == 3000 && text[0].cues[0].text == "Two\nlines");
        CHECK(!text[1].main && text[1].name == "Deutsch");
    }
}

void testProjectFile()
{
    ProjectData d;
    d.format = {1080, 1920, {25, 1}};
    SubtitleTrack t;
    t.captions.mode = CaptionMode::Word;
    t.captions.highlight = QColor(0, 255, 128);
    t.captions.highlightScale = 1.3;
    t.captions.maxWordsPerLine = 2;
    t.captions.popMs = 200;
    t.captions.uppercase = true;
    t.captions.shadow = true;
    t.cues = {cue(10, 40, "a b", {{0, 5}, {5, 30}}), cue(50, 60, "plain")};
    t.cues[0].id = 3;
    t.cues[1].id = 4;
    SubtitleTrack plain;
    plain.cues = {cue(0, 5, "x")};
    plain.cues[0].id = 5;
    d.timeline.subtitles = {t, plain};
    d.lastClipId = 5;
    const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
    CHECK(!json.contains("\"captions\": {\n            }")); // the plain track writes none
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
    CHECK_EQ(back.timeline.subtitles.size(), 2);
    if (back.timeline.subtitles.size() == 2) {
        CHECK(back.timeline.subtitles[0].captions == t.captions);
        CHECK(back.timeline.subtitles[0].cues == t.cues);
        CHECK(back.timeline.subtitles[1].captions == CaptionStyle{});
    }
    // Another frame rate: the word times follow (25 -> 50 fps)
    Timeline rt;
    rt.subtitles = {t};
    retimeTimeline(rt, {25, 1}, {50, 1});
    CHECK_EQ(wordsOf(rt.subtitles[0].cues[0]), QString("20-80:a@0-10 b@10-60"));
    // Unknown (newer) mode: plain lines
    CHECK(Captions::fromJson(QJsonObject{{"mode", "sparkle"}}).mode == CaptionMode::Lines);
    CHECK(Captions::toJson(CaptionStyle{}).isEmpty());
}

// Captions made in a 9:16 timeline of a 16:9 project take the timeline's portrait defaults (size, position above the
// phone apps' bottom part), not the project's
void testTimelineFormat()
{
    Project p; // 1920 x 1080
    Selection sel;
    Editor ed(&p, &sel);
    p.addTimeline("Short", 0, QSize(1080, 1920));
    CHECK_EQ(p.timelineFormat().size(), QSize(1080, 1920));
    const int index = ed.addCaptions({cue(0, 25, "hello there")}, "Words", CaptionStyle{CaptionMode::Karaoke});
    if (!CHECK(index >= 0)) return;
    const TitleStyle& st = p.timeline().subtitles[index].style;
    const TitleStyle portrait = Captions::defaultStyle(QSize(1080, 1920), CaptionMode::Karaoke);
    CHECK_EQ(st.posY, portrait.posY);
    CHECK_EQ(st.size, portrait.size);
    CHECK(st.posY != Captions::defaultStyle(QSize(1920, 1080), CaptionMode::Karaoke).posY);
}

void testCliOp(const QString& dir)
{
    const QString path = dir + "/cli.schneidi";
    ProjectData d;
    d.format = {1080, 1920, {25, 1}};
    d.timeline.video.resize(1);
    d.timeline.audio.resize(1);
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(ProjectFile::toJson(d, path));
    f.close();
    const auto run = [&](const QJsonArray& ops) {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, QJsonObject{{"project", path}, {"ops", ops}}), ctx);
        } catch (const Cli::Error& e) {
            return QJsonObject{{"error", e.code + ": " + e.message}};
        }
    };
    const QJsonArray words{QJsonObject{{"from", 10}, {"to", 20}, {"text", "Eins"}},
                           QJsonObject{{"from", 20}, {"to", 30}, {"text", "zwei"}},
                           QJsonObject{{"from", 30}, {"to", 40}, {"text", "drei."}}};
    QJsonObject o = run({QJsonObject{{"op", "captions"}, {"style", "karaoke"}, {"words", words},
                                     {"highlight", "#00ff00"}, {"size", 100}}});
    CHECK(!o.contains("error"));
    ProjectData back;
    QFile in(path);
    CHECK(in.open(QIODevice::ReadOnly));
    CHECK(ProjectFile::fromJson(in.readAll(), path, &back, nullptr));
    in.close();
    if (CHECK_EQ(back.timeline.subtitles.size(), 1)) {
        const SubtitleTrack& t = back.timeline.subtitles[0];
        CHECK(t.captions.mode == CaptionMode::Karaoke);
        CHECK_EQ(t.captions.highlight, QColor(0, 255, 0));
        CHECK_EQ(t.style.size, 100.0);
        CHECK(t.style.posY > 1920 * 0.2); // above the part phone apps cover
        CHECK(t.enabled);
        if (CHECK_EQ(t.cues.size(), 1)) CHECK_EQ(wordsOf(t.cues[0]), QString("10-40:Eins@0-10 zwei@10-20 drei.@20-30"));
    }
    // beat without beats: an error; with bpm a grid (60 bpm at 25 fps = every 25 frames)
    o = run({QJsonObject{{"op", "captions"}, {"style", "beat"}, {"words", words}}});
    CHECK(o.value("error").toString().startsWith("BAD_ARGUMENT"));
    o = run({QJsonObject{{"op", "captions"}, {"style", "beat"}, {"words", words}, {"bpm", 60}, {"first_beat", 5}}});
    CHECK(!o.contains("error"));
    QFile in2(path);
    CHECK(in2.open(QIODevice::ReadOnly));
    CHECK(ProjectFile::fromJson(in2.readAll(), path, &back, nullptr));
    if (CHECK_EQ(back.timeline.subtitles.size(), 2)) {
        const SubtitleTrack& t = back.timeline.subtitles[1];
        CHECK(t.captions.mode == CaptionMode::Word);
        CHECK(!back.timeline.subtitles[0].enabled); // the new track is the visible one
        QStringList got;
        for (const SubtitleCue& c : t.cues) got << wordsOf(c);
        CHECK_EQ(got.join(" | "), QString("5-65:Eins@0-25 zwei@25-50 drei.@50-60"));
    }
    // Track op: change the mode afterwards; a wrong mode is an error
    o = run({QJsonObject{{"op", "track"}, {"track", "ST2"}, {"captions", "karaoke"}, {"pop_ms", 0}}});
    CHECK(!o.contains("error"));
    o = run({QJsonObject{{"op", "track"}, {"track", "ST2"}, {"captions", "sparkle"}}});
    CHECK(o.value("error").toString().startsWith("BAD_ARGUMENT"));
}

// Pixels in the highlight colour (yellow) and in white in a region
int yellow(const QImage& img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.red() > 200 && c.green() > 160 && c.blue() < 80) ++n;
        }
    return n;
}
// Inked columns of a picture: first, last and the widths of the empty runs between (gaps of at least 4 pixels: the
// spaces between words, not the room inside a letter)
struct Ink {
    int first = -1, last = -1;
    QVector<int> gaps;
};
Ink ink(const QImage& img)
{
    QVector<bool> on(img.width());
    for (int x = 0; x < img.width(); ++x)
        for (int y = 0; y < img.height() && !on[x]; ++y) on[x] = qAlpha(img.pixel(x, y)) > 60;
    Ink out;
    out.first = int(on.indexOf(true));
    out.last = int(on.lastIndexOf(true));
    for (int x = out.first; out.first >= 0 && x <= out.last; ++x) {
        if (on[x]) continue;
        int e = x;
        while (!on[e]) ++e;
        if (e - x >= 4) out.gaps << e - x;
        x = e;
    }
    return out;
}

// Karaoke layout: the space before and after the highlighted (larger) word is as wide as in plain text, at every
// highlighted word and with the outline of the caption style (the grown word covered the spaces: "Thisis word"), and
// the other spaces do not get wider either (room kept for the grown word made "is  word")
void testWordLayout()
{
    QFont font(QStringLiteral("DejaVu Sans"));
    font.setPixelSize(84);
    font.setBold(true);
    const QStringList list = Captions::words(QStringLiteral("This is word captions,"));
    const QFontMetricsF fm(font);
    const auto image = [] {
        QImage img(1600, 200, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        return img;
    };
    Text::Params plain;
    plain.text = list.join(' ');
    plain.font = font;
    plain.outline = plain.room = 12;
    QImage ref = image();
    plain.area = QRectF(0, 0, ref.width(), ref.height());
    Text::draw(ref.bits(), ref.width(), ref.height(), plain);
    const Ink refInk = ink(ref);
    if (!CHECK_EQ(refInk.gaps.size(), 3)) return;
    for (const double scale : {1.15, 1.4})
        for (int active = -1; active < list.size(); ++active) {
            Text::WordParams p;
            p.font = font;
            p.reserve = scale;
            p.outline = 12;
            p.lines = {{}};
            for (int i = 0; i < list.size(); ++i)
                p.lines[0] << Text::Word{list[i], i == active ? QColor(255, 214, 0) : QColor(Qt::white), i == active ? scale : 1.0};
            QImage img = image();
            p.area = QRectF(0, 0, img.width(), img.height());
            Text::drawWords(img.bits(), img.width(), img.height(), p);
            const Ink got = ink(img);
            const QString at = QString("scale %1 word %2: gaps %3, plain %4").arg(scale).arg(active)
                                   .arg(QDebug::toString(got.gaps), QDebug::toString(refInk.gaps));
            if (!CHECK_EQ(got.gaps.size(), 3)) {
                qWarning().noquote() << at;
                continue;
            }
            // The grown word's outline is a little wider (pen grows with it)
            for (int i = 0; i < 3; ++i)
                if (!CHECK(std::abs(got.gaps[i] - refInk.gaps[i]) <= 3)) qWarning().noquote() << at;
            // As wide as the plain text plus what the highlighted word grew
            const double grown = active < 0 ? 0.0 : fm.horizontalAdvance(list[active]) * (scale - 1);
            const double width = got.last - got.first, refWidth = refInk.last - refInk.first;
            if (!CHECK(std::abs(width - refWidth - grown) <= 6))
                qWarning().noquote() << at << "width" << width << "plain" << refWidth << "+" << grown;
        }
}

// Right-to-left text: the first word stands on the right; scripts without spaces, emoji and a word wider than the
// area are drawn (shrunk into the area, nothing outside it)
void testWordScripts()
{
    QFont font(QStringLiteral("DejaVu Sans"));
    font.setPixelSize(60);
    const auto draw = [&](const QStringList& list, int active, QRectF area = QRectF(0, 0, 800, 200)) {
        Text::WordParams p;
        p.font = font;
        p.area = area;
        p.outline = 6;
        p.lines = {{}};
        for (int i = 0; i < list.size(); ++i)
            p.lines[0] << Text::Word{list[i], i == active ? QColor(255, 214, 0) : QColor(Qt::white), i == active ? 1.2 : 1.0};
        QImage img(800, 200, QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        Text::drawWords(img.bits(), img.width(), img.height(), p);
        return img;
    };
    // Mean x of the highlight colour
    const auto yellowX = [](const QImage& img) {
        double sum = 0;
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (c.alpha() > 200 && c.red() > 200 && c.green() > 160 && c.blue() < 80) sum += x, ++n;
            }
        return n ? sum / n : -1.0;
    };
    const QStringList hebrew{QStringLiteral("\u05E9\u05DC\u05D5\u05DD"), QStringLiteral("\u05E2\u05D5\u05DC\u05DD"),
                             QStringLiteral("\u05D8\u05D5\u05D1")};
    CHECK(yellowX(draw(hebrew, 0)) > 400); // first word right
    CHECK(yellowX(draw(hebrew, 2)) < 400);
    const QStringList latin{"one", "two", "three"};
    CHECK(yellowX(draw(latin, 0)) < 400);
    // No spaces, emoji, one very long word: something is drawn and it stays inside the area
    for (const QString& text : {QStringLiteral("\u4ECA\u65E5\u306F\u3044\u3044\u5929\u6C17\u3067\u3059\u306D"),
                                QStringLiteral("GG \U0001F525\U0001F525 wow"), QString(200, QChar('W'))}) {
        const QImage img = draw(Captions::words(text), 0, QRectF(100, 0, 600, 200));
        const Ink in = ink(img);
        CHECK(in.first >= 99 && in.last <= 701);
        CHECK(in.last > in.first);
    }
}

int white(const QImage& img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qGray(img.pixel(x, y)) > 200 && std::abs(img.pixelColor(x, y).blue() - img.pixelColor(x, y).red()) < 30) ++n;
    return n;
}

void testRender(const QString& dir)
{
    const QString black = Check::makeMedia(dir + "/black.mp4", {"-f", "lavfi", "-i", "color=black:size=360x640:rate=25:duration=4",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!black.isEmpty())) return;
    const ProjectFormat fmt{360, 640, {25, 1}};
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = black;
    c.out = 99;
    tl.video[0].clips << c;
    SubtitleTrack st;
    st.style = Captions::defaultStyle(fmt.size());
    st.captions.mode = CaptionMode::Karaoke;
    st.cues = {cue(10, 60, "eins zwei drei", {{5, 15}, {15, 25}, {25, 40}})};
    st.cues[0].id = 2;
    tl.subtitles << st;
    const QImage lit = Check::renderFrame(tl, fmt, 20, {}, false, true); // "zwei" spoken
    if (!CHECK(!lit.isNull())) return;
    if (qEnvironmentVariableIsSet("CAPTIONS_DUMP")) lit.save(qEnvironmentVariable("CAPTIONS_DUMP"));
    CHECK(yellow(lit) > 50);
    CHECK(white(lit) > 100); // the other words
    const QImage before = Check::renderFrame(tl, fmt, 12, {}, false, true); // shown, no word yet
    CHECK_EQ(yellow(before), 0);
    CHECK(white(before) > 100);
    // Word by word: only one word, nothing before the first word
    tl.subtitles[0].captions.mode = CaptionMode::Word;
    const QImage one = Check::renderFrame(tl, fmt, 30, {}, false, true);
    const int wordPixels = white(one);
    CHECK(wordPixels > 50);
    CHECK(wordPixels < white(lit)); // one word, not three
    CHECK_EQ(white(Check::renderFrame(tl, fmt, 12, {}, false, true)), 0);
    // Pop-in: the first frame of a word is smaller/fainter than later
    tl.subtitles[0].captions.popMs = 200;
    CHECK(white(Check::renderFrame(tl, fmt, 35, {}, false, true)) < white(Check::renderFrame(tl, fmt, 45, {}, false, true)));
    CHECK_EQ(white(Check::renderFrame(tl, fmt, 30, {}, false, false)), 0); // export without burn-in
}

// The subtitle streams of a file: codec name, title, default flag and the cues (ms) as libavformat reads them
QString textStreams(const QString& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) return "cannot open";
    avformat_find_stream_info(fc, nullptr);
    QStringList out;
    QHash<int, QStringList> cues;
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(fc, p) >= 0) {
        const AVStream* st = fc->streams[p->stream_index];
        // (MP4 fills the gaps between cues with empty samples)
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && p->size > 2) {
            const int64_t ms = av_rescale_q(p->pts, st->time_base, AVRational{1, 1000});
            const int64_t dur = av_rescale_q(p->duration, st->time_base, AVRational{1, 1000});
            cues[p->stream_index] << QString("%1+%2").arg(ms).arg(dur);
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
        if (!title) title = av_dict_get(st->metadata, "handler_name", nullptr, 0); // MP4
        out << QString("%1 %2%3 %4")
                   .arg(avcodec_get_name(st->codecpar->codec_id), title ? title->value : "-",
                        (st->disposition & AV_DISPOSITION_DEFAULT) ? "*" : "", cues[int(i)].join(','));
    }
    avformat_close_input(&fc);
    return out.join(" | ");
}

void testEmbed(const QString& dir)
{
    const QString media = Check::makeMedia(dir + "/src.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2",
                                                               "-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-c:v",
                                                               "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                               "-c:a", "aac", "-shortest"});
    if (!CHECK(!media.isEmpty())) return;
    const ProjectFormat fmt{320, 180, {25, 1}};
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 49;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    SubtitleTrack de, en;
    de.name = "Deutsch";
    de.cues = {{3, 5, 20, "Hallo {Welt}"}, {4, 25, 40, "zwei\nZeilen"}};
    en.name = "English";
    en.enabled = false;
    en.cues = {{5, 10, 30, "Hello"}};
    de.captions.mode = CaptionMode::Karaoke; // the stream carries the plain text anyway
    tl.subtitles = {de, en};
    struct Case { const char* file; const char* format; const char* codec; };
    for (const Case& k : {Case{"e.mp4", "h264", "mov_text"}, Case{"e.mkv", "h264", "subrip"}, Case{"e.webm", "vp9", "webvtt"}}) {
        RenderJob job;
        job.id = 1;
        job.path = dir + "/" + k.file;
        job.settings.format = k.format;
        job.settings.embedSubtitles = true;
        job.settings.smartRender = false;
        Exporter ex;
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            ok = success;
            loop.quit();
        });
        QString error;
        if (!CHECK(ex.start(tl, RenderQueue::exportSettings(job, fmt), &error))) {
            std::printf("       %s\n", qPrintable(error));
            continue;
        }
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!CHECK(ok)) continue;
        CHECK_EQ(textStreams(job.path),
                 QString("%1 Deutsch* 200+600,1000+600 | %1 English 400+800").arg(k.codec));
    }
    // Off: no stream
    RenderJob job;
    job.id = 1;
    job.path = dir + "/none.mp4";
    job.settings.format = "h264";
    CHECK(!RenderQueue::exportSettings(job, fmt).embedSubtitles);
    // Settings round trip
    RenderSettings s;
    s.embedSubtitles = true;
    s.subtitles = RenderSettings::VttFile;
    const RenderSettings back = RenderSettings::fromJson(s.toJson());
    CHECK(back == s);
    CHECK(back.embedSubtitles && back.subtitles == RenderSettings::VttFile);
    job.settings.subtitles = RenderSettings::VttFile;
    CHECK(RenderQueue::exportSettings(job, fmt).subtitlePath.endsWith("none.vtt"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("captions");
    std::setlocale(LC_NUMERIC, "C");
    testTiming();
    testPop();
    testLines();
    testFromWords();
    testEdits();
    testSpeed();
    testVtt();
    testProjectFile();
    testWordLayout();
    testWordScripts();
    testTimelineFormat();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testCliOp(tmp.path());
    if (!Check::haveFfmpeg()) {
        Check::result();
        return Check::skip("ffmpeg nicht gefunden (Render-Teil)");
    }
    Check::initEngine();
    testRender(tmp.path());
    testEmbed(tmp.path());
    return Check::result();
}
