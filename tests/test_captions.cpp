// SPDX-License-Identifier: AGPL-3.0-only
// Word captions (core/Captions): word times and the spoken word at a frame, pop-in values, line breaks, cues from
// transcript words (also on beats), word times through split/ripple/trim and through clip trims and speed
// (TranscriptCut), WebVTT, project file, the CLI op 'captions', the karaoke highlight in a rendered frame and
// subtitle text streams in exported MP4/MKV/WebM (read back with libavformat).

#include "render_check.h"
#include "timeline_guard.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Captions.h"
#include "core/Editor.h"
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TranscriptCut.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "engine/video/Text.h"
#include "ui/Inspector.h"

#include <QApplication>
#include <QEventLoop>
#include <QDebug>
#include <QFile>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
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

// ---- Two speakers transcribed on their own (the microphone and the voice chat of a game recording, 25.11.: the quiet
// chat partner was missing from the captions of the mix): one list in time order, every word at its own time (a phrase
// spoken over another one is not moved after it any more: "the texts are not exactly on time"), a speaker per word,
// so a caption never mixes the two ----
void testMergeSpeakers()
{
    std::printf("merge speakers\n");
    // 60 fps. Chat: "Heilige Scheiße." 3.9-5.0 s, "Direkt Headshot kassiert." 5.0-5.9 s, "Auch da?" 8.3 s;
    // microphone: "Einer soll?" from 5.5 s - while the chat's second sentence is still spoken
    const QVector<TranscriptCut::Word> chat{word(234, 260, "Heilige"), word(262, 300, "Scheiße."), word(300, 320, "Direkt"),
                                            word(322, 340, "Headshot"), word(342, 354, "kassiert."), word(498, 510, "Auch"),
                                            word(512, 524, "da?")};
    const QVector<TranscriptCut::Word> mic{word(330, 345, "Einer"), word(347, 362, "soll?")};
    const QVector<TranscriptCut::Word> all = Captions::mergeSpeakers({chat, mic});
    QStringList got;
    for (const TranscriptCut::Word& w : all) got << QString("%1:%2@%3-%4").arg(w.track).arg(w.text).arg(w.from).arg(w.spokenTo);
    CHECK_EQ(got.join(" "), QString("0:Heilige@234-260 0:Scheiße.@262-300 0:Direkt@300-320 0:Headshot@322-340 "
                                    "1:Einer@330-345 0:kassiert.@342-354 1:soll?@347-362 0:Auch@498-510 0:da?@512-524"));
    CHECK_EQ(all.size(), chat.size() + mic.size());
    // Captions: the speakers never in one cue, even without a sentence end between them
    QVector<TranscriptCut::Word> noStop = chat;
    for (TranscriptCut::Word& w : noStop) w.text.remove('.').remove('?');
    QVector<TranscriptCut::Word> both = noStop;
    for (TranscriptCut::Word w : mic) w.track = 1, w.text.remove('?'), both << w;
    std::stable_sort(both.begin(), both.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
    Captions::Options o;
    o.maxWords = 6;
    o.maxGapFrames = 36;
    o.bySpeaker = true;
    QStringList cues;
    for (const SubtitleCue& c : Captions::fromWords(both, o)) cues << c.text;
    for (const QString& c : cues) CHECK(!(c.contains("Einer") && c.contains("Headshot")));
    // Words of two audio tracks without speakers (the Transcript panel over all tracks, two recordings heard at once):
    // phrases as before, no cue of one word each time the track changes
    {
        Captions::Options plain;
        plain.maxWords = 4;
        plain.maxGapFrames = 36;
        QVector<TranscriptCut::Word> mixed{word(10, 20, "Eins"), word(21, 30, "zwei"), word(31, 40, "drei"), word(41, 50, "vier")};
        mixed[1].track = mixed[3].track = 1;
        QStringList got;
        for (const SubtitleCue& c : Captions::fromWords(mixed, plain)) got << c.text;
        CHECK_EQ(got.join(" | "), QString("Eins zwei drei vier"));
    }
    // Phrases of one speaker: a sentence end or a long pause closes one
    CHECK_EQ(Captions::phrases(chat, 42), (QVector<QPair<int, int>>{{0, 1}, {2, 4}, {5, 6}}));
}

// ---- Word times onto the sound (25.11.: "the texts are not exactly on time"; whisper's times are loose, mostly for
// short interjections; a voice chat is gated - digital silence between words) ----

// The sample peak of each frame (fps) of `seconds` of sound: `floor` noise everywhere, noise of `level` in the bursts
// (seconds), a deterministic noise
std::vector<float> framePeaks(double fps, double seconds, const QVector<QPair<double, double>>& bursts, double level,
                              double floor)
{
    constexpr int rate = 48000;
    const int per = int(rate / fps);
    const int frames = int(seconds * fps);
    std::vector<float> out(size_t(frames), 0.f);
    quint32 seed = 12345;
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < per; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double noise = (seed >> 8) / double(1 << 24) * 2 - 1;
            const double t = (f * per + i) / double(rate);
            bool in = false;
            for (const auto& [a, b] : bursts) in |= t >= a && t < b;
            out[size_t(f)] = std::max(out[size_t(f)], float(std::abs(noise * (in ? level : floor))));
        }
    return out;
}

void testSnapToSound()
{
    std::printf("snap to sound\n");
    const double fps = 60;
    // Speech at 2.00-2.60 s, an interjection "Ja." at 3.50-3.62 s, a sentence at 5.00-6.20 s; silence between
    const QVector<QPair<double, double>> bursts{{2.0, 2.6}, {3.5, 3.62}, {5.0, 6.2}};
    const std::vector<float> gated = framePeaks(fps, 8, bursts, 0.4, 0);
    // whisper: the first phrase starts 0.2 s early and ends 0.15 s early, "Ja." 0.2 s early, the last one about right
    const QVector<TranscriptCut::Word> whisper{word(108, 126, "Ich"), word(126, 138, "war"), word(138, 147, "da."),
                                               word(198, 204, "Ja."), word(302, 330, "Dann"), word(330, 371, "los.")};
    QVector<TranscriptCut::Word> w = whisper;
    CHECK_EQ(Captions::snapToSound(w, gated, fps, 42), 3);
    QStringList got;
    for (const TranscriptCut::Word& x : w) got << QString("%1@%2-%3").arg(x.text).arg(x.from).arg(x.spokenTo);
    // The edges on the sound (2.00 s = 120, 2.60 s = 156, 3.50 s = 210, 3.62 s = 218 (217.2 -> its frame), 5.00 s = 300,
    // 6.20 s = 372); the inner words kept where they still fit
    CHECK_EQ(got.join(' '), QString("Ich@120-126 war@126-138 da.@138-156 Ja.@210-218 Dann@300-330 los.@330-372"));
    for (int i = 1; i < w.size(); ++i) CHECK(w[i].from >= w[i - 1].spokenTo && w[i].from > w[i - 1].from);
    for (const TranscriptCut::Word& x : w) CHECK(x.to >= x.spokenTo);
    // A stream with constant noise (no clear edge): whisper's times stay
    QVector<TranscriptCut::Word> noisy = whisper;
    CHECK_EQ(Captions::snapToSound(noisy, framePeaks(fps, 8, bursts, 0.4, 0.15), fps, 42), 0);
    CHECK(noisy == whisper);
    // A gated stream with a little noise between the words: still found
    QVector<TranscriptCut::Word> hiss = whisper;
    Captions::snapToSound(hiss, framePeaks(fps, 8, bursts, 0.4, 0.002), fps, 42);
    CHECK_EQ(hiss[3].from, 210);
    CHECK_EQ(hiss[3].spokenTo, 218);
    // Too far away (more than 0.4 s): kept
    QVector<TranscriptCut::Word> far{word(170, 176, "Ja."), word(362, 410, "Weiter.")};
    const std::vector<float> one = framePeaks(fps, 8, {{3.5, 3.62}, {6, 7}}, 0.4, 0);
    Captions::snapToSound(far, one, fps, 42);
    CHECK_EQ(far[0].from, 170);
    // The onset moves past the first word's end: the words are spread over the speech, none under 80 ms (5 frames)
    QVector<TranscriptCut::Word> late{word(100, 106, "Na"), word(106, 112, "und"), word(112, 140, "jetzt?")};
    Captions::snapToSound(late, framePeaks(fps, 8, {{1.85, 2.4}, {6, 7}}, 0.4, 0), fps, 42);
    QStringList spread;
    for (const TranscriptCut::Word& x : late) spread << QString("%1@%2-%3").arg(x.text).arg(x.from).arg(x.spokenTo);
    CHECK_EQ(late.first().from, 111);
    CHECK_EQ(late.last().spokenTo, 144);
    for (int i = 0; i < late.size(); ++i) {
        CHECK(late[i].spokenTo - late[i].from >= 5);
        if (i) CHECK(late[i].from >= late[i - 1].spokenTo);
    }
    if (!CHECK(late[1].from > 111)) std::printf("       %s\n", qPrintable(spread.join(' ')));
    // Never over the speaker's phrase before: two sentences in one stretch of speech, the second one's start is not
    // moved back into the first (no clear edge in between)
    QVector<TranscriptCut::Word> joined{word(120, 140, "Eins."), word(150, 170, "Zwei.")};
    Captions::snapToSound(joined, framePeaks(fps, 8, {{2.0, 3.0}, {6, 7}}, 0.4, 0), fps, 42);
    CHECK_EQ(joined[0].from, 120);
    CHECK_EQ(joined[1].from, 150);
    CHECK(joined[0].spokenTo <= joined[1].from);
}

// ---- Two speech models must agree (25.11.: "Warzone." where nothing like it was said, "scheu?" a wrong word - whisper's
// own word probabilities did not tell them from right lines; large-v3 and turbo heard both differently) ----
void testAgreement()
{
    std::printf("two models agree\n");
    CHECK(Captions::sameWords("Heilige Scheiße.", "heilige scheiße"));
    CHECK(Captions::sameWords("Ja, und?", "Ja und?"));
    CHECK(!Captions::sameWords("Warzone.", "Wowzone."));                      // two letters in a single word
    CHECK(Captions::sameWords("Headshot", "Headshots"));                       // one letter in a long word
    CHECK(Captions::sameWords("Heilige Scheiße.", "Heilige Scheisse."));       // ß and ss
    CHECK(!Captions::sameWords("Ja.", "Nein."));
    CHECK(!Captions::sameWords("Ja, war ich nicht scheu?", "Ja. War nicht toll?"));
    CHECK(Captions::sameWords("Digga, leck meine fetten Eier!", "Digga, leck mal 'ne fette Eier!"));
    CHECK(Captions::sameWords("Der ist um 9 Uhr.", "Der ist um neun Uhr."));   // numbers written differently
    CHECK(Captions::sameWords("Wollen wir noch 'ne Granate", "Wollen wir noch eine Granate"));
    CHECK(!Captions::sameWords("Crazy.", ""));
    CHECK(Captions::sameWords("", ""));
    // Phrases: kept where the check hears the same, left out as a whole where not, with both readings
    const auto w = [](qint64 from, qint64 to, const char* text) { return TranscriptWord{from, to, QString::fromUtf8(text)}; };
    const QVector<TranscriptWord> heard{w(3870, 4100, "Heilige"), w(4100, 4380, "Scheiße."), w(5920, 6270, "Warzone."),
                                        w(8280, 8400, "Auch"), w(8400, 8520, "da?"), w(12000, 12300, "Crazy.")};
    const QVector<TranscriptWord> check{w(3880, 4090, "Heilige"), w(4110, 4400, "Scheiße."), w(5900, 6300, "Wowzone."),
                                        w(8300, 8520, "Auch da?")};
    QVector<Captions::Disagreement> left;
    const QVector<TranscriptWord> kept = Captions::keepAgreed(heard, check, 700, &left);
    QStringList got;
    for (const TranscriptWord& x : kept) got << x.text;
    CHECK_EQ(got.join(' '), QString("Heilige Scheiße. Auch da?"));
    if (CHECK_EQ(left.size(), 2)) {
        CHECK_EQ(left[0].heard, QString("Warzone."));
        CHECK_EQ(left[0].other, QString("Wowzone."));
        CHECK_EQ(left[0].from, 5920);
        CHECK_EQ(left[1].heard, QString("Crazy.")); // the check heard nothing there
        CHECK(left[1].other.isEmpty());
    }
    // Everything the same: nothing left out
    left.clear();
    CHECK_EQ(Captions::keepAgreed(heard, heard, 700, &left).size(), heard.size());
    CHECK(left.isEmpty());
}

// A cue the moment's end cuts to almost nothing (25.11.: "nee," shown 0.03 s): a word heard joins the cue before it,
// one cut off before it is heard goes
void testCueAtCut()
{
    std::printf("cue at a cut\n");
    Captions::Options o;
    o.maxWords = 4;
    o.maxGapFrames = 36;
    o.minFrames = 24;
    o.minAtCutFrames = 15; // 0.25 s at 60 fps
    o.heardFrames = 7;
    o.cuts = {1407};
    QVector<TranscriptCut::Word> w{word(1388, 1395, "Nee,"), word(1396, 1404, "nee,"), word(1405, 1420, "nee,")};
    QStringList got;
    for (const SubtitleCue& c : Captions::fromWords(w, o)) got << wordsOf(c);
    CHECK_EQ(got.join(" | "), QString("1388-1405:Nee,@0-7 nee,@8-16"));
    // Started 0.17 s before the cut: heard, so it joins the cue before it
    w = {word(1380, 1388, "Nee,"), word(1389, 1396, "nee,"), word(1397, 1420, "nee,")};
    got.clear();
    for (const SubtitleCue& c : Captions::fromWords(w, o)) got << wordsOf(c);
    CHECK_EQ(got.join(" | "), QString("1380-1407:Nee,@0-8 nee,@9-16 nee,@17-27"));
    // Without the rule (minAtCutFrames 0) the short cue stays as before
    o.minAtCutFrames = 0;
    CHECK_EQ(Captions::fromWords(w, o).size(), 2);
}

// Several speakers' tracks apart: a lane above the lower one only when they are shown at the same time
void testStackLanes()
{
    std::printf("stack lanes\n");
    SubtitleTrack a, b, c;
    a.style.posY = b.style.posY = c.style.posY = 576;
    a.style.size = 84;
    a.cues = {cue(0, 60, "eins zwei"), cue(100, 160, "drei")};
    b.cues = {cue(30, 90, "vier")};   // over a's first cue
    c.cues = {cue(200, 260, "fünf")}; // alone
    QVector<SubtitleTrack> lanes{a, b, c};
    Captions::stackLanes(lanes, [](const SubtitleTrack&, const SubtitleCue& cue) { return cue.text == "eins zwei" ? 120.0 : 60.0; });
    CHECK_EQ(lanes[0].style.posY, 576.0);
    CHECK_EQ(lanes[1].style.posY, std::round(576 + 120 + 0.2 * 84));
    CHECK_EQ(lanes[2].style.posY, 576.0);
    // Colours: the first keeps its own, the others clearly different ones
    CHECK_EQ(Captions::speakerColor(0, QColor(255, 214, 0)), QColor(255, 214, 0));
    CHECK(Captions::speakerColor(1, QColor(255, 214, 0)) != Captions::speakerColor(2, QColor(255, 214, 0)));
    CHECK_EQ(Captions::speakerColor(4, QColor(1, 2, 3)), Captions::speakerColor(1, QColor(9, 9, 9)));
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
    // A cut between clips at frame 26 (Transcript::cutFrames): "Three four" end there, "five" starts the next
    // caption, which lasts to the next cut at 36 at most, not its 10 frames
    o.cuts = {26, 36};
    QStringList cut;
    for (const SubtitleCue& c : Captions::fromWords(words, o)) cut << wordsOf(c);
    CHECK_EQ(cut.join(" | "), QString("10-20:One@0-4 two.@4-8 | 20-26:Three@0-4 four@4-6 | 28-36:five@0-2 six@3-5 | "
                                      "60-70:late@0-4"));
    o.cuts.clear();
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
    // Split in the pause after the last word: no empty, invisible cue on the right (the cue ends at the cut)
    {
        SubtitleTrack e;
        e.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
        e.cues[0].id = 1;
        CHECK_EQ(Subtitles::splitAt(e, 135, [&] { return next++; }), 0);
        CHECK_EQ(e.cues.size(), 1);
        for (const SubtitleCue& c : e.cues) CHECK(!c.text.isEmpty());
        if (e.cues.size() == 1) CHECK_EQ(wordsOf(e.cues[0]), QString("100-135:a@0-5 bb@5-10 ccc@20-30"));
        // ... and before the first word: the cue starts at the cut, keeping its id
        SubtitleTrack b;
        b.cues = {cue(100, 150, "a bb", {{10, 15}, {15, 20}})};
        b.cues[0].id = 1;
        CHECK_EQ(Subtitles::splitAt(b, 105, [&] { return next++; }), 0);
        CHECK(b.cues.size() == 1 && b.cues[0].id == 1);
        if (b.cues.size() == 1) CHECK_EQ(wordsOf(b.cues[0]), QString("105-150:a@5-10 bb@10-15"));
        // Overwritten over all its words: the rest of it goes too
        SubtitleTrack w;
        w.cues = {cue(100, 150, "a bb", {{0, 5}, {5, 10}})};
        Subtitles::place(w, cue(95, 125, "neu"));
        CHECK_EQ(w.cues.size(), 1);
        for (const SubtitleCue& c : w.cues) CHECK(!c.text.isEmpty());
    }
    // Ripple: removing frames 104..112 takes "bb" (105-110) out and moves "ccc" up
    SubtitleTrack r;
    r.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
    CHECK(Subtitles::ripple(r, {{112, -8}}));
    CHECK_EQ(wordsOf(r.cues[0]), QString("100-142:a@0-4 ccc@12-22"));
    // Ripple over every word of a cue that lasts past them (minFrames): the cue goes, no empty, invisible block stays
    // (cutting out a filler word "äh" by text)
    {
        SubtitleTrack f;
        f.cues = {cue(100, 112, "äh", {{0, 6}}), cue(120, 130, "gut", {{0, 5}})};
        CHECK(Subtitles::ripple(f, {{106, -6}}));
        CHECK_EQ(f.cues.size(), 1);
        for (const SubtitleCue& c : f.cues) CHECK(!c.text.isEmpty());
        if (f.cues.size() == 1) CHECK_EQ(wordsOf(f.cues[0]), QString("114-124:gut@0-5"));
    }
    // Ripple with time added inside a word caption (the clip under its first words got longer): the words spoken
    // after that point move with their clip, the cue grows; a plain cue across the point stays as it is
    {
        SubtitleTrack g;
        g.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}}), cue(160, 170, "dd", {{0, 5}})};
        CHECK(Subtitles::ripple(g, {{112, 8}}));
        CHECK_EQ(g.cues.size(), 2);
        if (g.cues.size() == 2) {
            CHECK_EQ(wordsOf(g.cues[0]), QString("100-158:a@0-5 bb@5-10 ccc@28-38"));
            CHECK_EQ(wordsOf(g.cues[1]), QString("168-178:dd@0-5"));
        }
        SubtitleTrack plain;
        plain.cues = {cue(100, 150, "a bb ccc")};
        CHECK(!Subtitles::ripple(plain, {{112, 8}}));
        CHECK_EQ(plain.cues[0].start, 100);
        CHECK_EQ(plain.cues[0].end, 150);
    }
    // Overwritten by another cue at the end: the covered words go
    SubtitleTrack o;
    o.cues = {cue(100, 150, "a bb ccc", {{0, 5}, {5, 10}, {20, 30}})};
    Subtitles::place(o, cue(118, 160, "neu"));
    CHECK_EQ(wordsOf(o.cues[0]), QString("100-118:a@0-5 bb@5-10"));
    // Editor: trimming the start keeps the words where they are spoken; moving keeps them with the cue
    Project p;
    TimelineGuard guard_p{p};
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
    // Trimming past every word stops at the last (start) or first (end) word: the cue never ends up empty
    ed.trimSubtitle(id, TimelineOps::Edge::Start, 40);
    CHECK_EQ(wordsOf(p.timeline().subtitles[0].cues[0]), QString("120-150:ccc@0-10"));
    p.undoStack()->undo();
    ed.trimSubtitle(id, TimelineOps::Edge::End, -49);
    CHECK_EQ(wordsOf(p.timeline().subtitles[0].cues[0]), QString("100-101:a@0-1"));
    p.undoStack()->undo();
    ed.setSubtitleTiming(id, 140, 150); // all words before 140: refused
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

// "Spurstil zurücksetzen" in the Inspector on a word caption track goes back to the caption defaults of its mode
// (bold, outline, bigger for word-by-word), not to the plain subtitle style
void testResetTrackStyle()
{
    std::printf("reset track style\n");
    for (CaptionMode mode : {CaptionMode::Karaoke, CaptionMode::Word}) {
        Project p;
        TimelineGuard guard_p{p};
        Selection sel;
        Editor ed(&p, &sel);
        const int index = ed.addCaptions({cue(0, 25, "hello there")}, "Words", CaptionStyle{mode});
        if (!CHECK(index >= 0)) return;
        ed.setSubtitleStyle(index, "change", [](TitleStyle& t) { t.size = 11; t.outlineOn = false; });
        sel.set({p.timeline().subtitles[index].cues.first().id});
        Inspector insp(&ed);
        QToolButton* reset = nullptr;
        for (QToolButton* b : insp.findChildren<QToolButton*>())
            if (b->text() == T("Spurstil"))
                for (QToolButton* r : b->parentWidget()->findChildren<QToolButton*>())
                    if (r->text() == QString::fromUtf8("↺")) reset = r;
        if (!CHECK(reset)) return;
        reset->click();
        const TitleStyle& st = p.timeline().subtitles[index].style;
        const TitleStyle want = Captions::defaultStyle(p.timelineFormat().size(), mode);
        CHECK_EQ(st.size, want.size);
        CHECK_EQ(st.outlineOn, want.outlineOn);
        CHECK_EQ(st.outlineWidth, want.outlineWidth);
        CHECK_EQ(st.boxOn, want.boxOn);
        CHECK_EQ(st.bold, want.bold);
    }
}

// op captions from the project's transcripts on two audio tracks, without speakers (as before 25.11.): one track,
// named as the op says; split into speaker tracks only when the words carry speakers or speakers is given
void testCliOpTwoTracks(const QString& dir)
{
    const QString path = dir + "/two-tracks.schneidi";
    ProjectData d;
    d.format = {1080, 1920, {25, 1}};
    d.timeline.video.resize(1);
    d.timeline.audio.resize(2);
    for (int k = 0; k < 2; ++k) {
        MediaInfo m;
        m.path = Check::absPath(QString("/x/voice%1.wav").arg(k));
        m.name = QFileInfo(m.path).fileName();
        m.length = 250;
        m.hasAudio = true;
        m.transcripts << MediaTranscript{0, "de", {TranscriptWord{k == 0 ? 400 : 2400, k == 0 ? 800 : 2800, k == 0 ? "Hallo." : "Servus."}}};
        d.media << m;
        Clip c;
        c.id = k + 1;
        c.mediaPath = m.path;
        c.kind = ClipKind::Media;
        c.start = 0;
        c.in = 0;
        c.out = 249;
        d.timeline.audio[k].clips << c;
    }
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
    const auto load = [&] {
        ProjectData back;
        QFile in(path);
        CHECK(in.open(QIODevice::ReadOnly));
        CHECK(ProjectFile::fromJson(in.readAll(), path, &back, nullptr));
        return back;
    };
    QJsonObject o = run({QJsonObject{{"op", "captions"}, {"style", "karaoke"}, {"name", "Untertitel"}}});
    if (!CHECK(!o.contains("error"))) std::printf("       %s\n", qPrintable(o.value("error").toString()));
    ProjectData back = load();
    if (CHECK_EQ(back.timeline.subtitles.size(), 1)) {
        CHECK_EQ(back.timeline.subtitles[0].name, QString("Untertitel"));
        CHECK_EQ(back.timeline.subtitles[0].cues.size(), 2);
    }
    // speakers given: a track each, named after them (op name before them)
    o = run({QJsonObject{{"op", "captions"}, {"style", "karaoke"}, {"name", "CC"}, {"speakers", QJsonArray{"Mikrofon", "Discord"}},
                         {"words", QJsonArray{QJsonObject{{"from", 10}, {"to", 20}, {"text", "Ja."}, {"speaker", 0}},
                                              QJsonObject{{"from", 60}, {"to", 70}, {"text", "Nein."}, {"speaker", 1}}}}}});
    if (!CHECK(!o.contains("error"))) std::printf("       %s\n", qPrintable(o.value("error").toString()));
    back = load();
    if (CHECK_EQ(back.timeline.subtitles.size(), 3)) {
        CHECK_EQ(back.timeline.subtitles[1].name, QString("CC – Mikrofon"));
        CHECK_EQ(back.timeline.subtitles[2].name, QString("CC – Discord"));
    }
    // beat with several speakers: the note that the beats ended, as with one
    o = run({QJsonObject{{"op", "captions"}, {"style", "beat"}, {"beats", QJsonArray{5}},
                         {"words", QJsonArray{QJsonObject{{"from", 10}, {"to", 20}, {"text", "Ja"}, {"speaker", 0}},
                                              QJsonObject{{"from", 30}, {"to", 40}, {"text", "Nein"}, {"speaker", 1}}}}}});
    CHECK(QJsonDocument(o["notes"].toArray()).toJson().contains("got a beat"));
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
    if (!CHECK(!o.contains("error"))) std::printf("       %s\n", qPrintable(o.value("error").toString()));
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
    in2.close(); // Windows: an open file cannot be replaced by the next save (SAVE_FAILED)
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
    if (!CHECK(!o.contains("error"))) std::fprintf(stderr, "       %s\n", qPrintable(o.value("error").toString()));
    o = run({QJsonObject{{"op", "track"}, {"track", "ST2"}, {"captions", "sparkle"}}});
    CHECK(o.value("error").toString().startsWith("BAD_ARGUMENT"));

    // Two speakers talking over each other (25.11.: "the texts are not exactly on time"): a visible track each, named
    // after the voice, every phrase at its own time, the second speaker above the first while both are shown and in
    // another colour; the SRT carries both at once; check finds nothing wrong with it
    const QJsonArray two{QJsonObject{{"from", 10}, {"to", 20}, {"text", "Heilige"}, {"speaker", 0}},
                         QJsonObject{{"from", 20}, {"to", 40}, {"text", "Scheiße."}, {"speaker", 0}},
                         QJsonObject{{"from", 30}, {"to", 45}, {"text", "Crazy."}, {"speaker", 1}},
                         QJsonObject{{"from", 80}, {"to", 95}, {"text", "Ja."}, {"speaker", 1}}};
    o = run({QJsonObject{{"op", "captions"}, {"style", "karaoke"}, {"words", two}, {"speakers", QJsonArray{"Mikrofon", "Discord"}}}});
    if (!CHECK(!o.contains("error"))) std::printf("       %s\n", qPrintable(o.value("error").toString()));
    QFile in3(path);
    CHECK(in3.open(QIODevice::ReadOnly));
    CHECK(ProjectFile::fromJson(in3.readAll(), path, &back, nullptr));
    in3.close();
    if (CHECK_EQ(back.timeline.subtitles.size(), 4)) {
        const SubtitleTrack& mic = back.timeline.subtitles[2];
        const SubtitleTrack& chat = back.timeline.subtitles[3];
        CHECK_EQ(mic.name, QString("Mikrofon"));
        CHECK_EQ(chat.name, QString("Discord"));
        CHECK(mic.enabled && chat.enabled && !back.timeline.subtitles[0].enabled && !back.timeline.subtitles[1].enabled);
        CHECK_EQ(mic.captions.highlight, CaptionStyle().highlight);
        CHECK(chat.captions.highlight != mic.captions.highlight);
        // Above the first one's text (one line of 84 px and more)
        CHECK(chat.style.posY > mic.style.posY + 84);
        QStringList a, b;
        for (const SubtitleCue& c : mic.cues) a << wordsOf(c);
        for (const SubtitleCue& c : chat.cues) b << wordsOf(c);
        CHECK_EQ(a.join(" | "), QString("10-40:Heilige@0-10 Scheiße.@10-30"));
        CHECK_EQ(b.join(" | "), QString("30-45:Crazy.@0-15 | 80-95:Ja.@0-15"));
        // One SRT with both: while both are shown one entry, the upper (Discord) line first
        const QVector<Subtitles::SrtFile> files = Subtitles::srtFiles(back.timeline, dir + "/two.srt");
        if (CHECK_EQ(files.size(), 3)) { // + the two earlier, hidden tracks next to it
            CHECK_EQ(files[0].track, 2);
            CHECK_EQ(files[0].also, QVector<int>{3});
            QStringList entries;
            for (const SubtitleCue& c : Subtitles::cuesOf(back.timeline, files[0]))
                entries << QString("%1-%2:%3").arg(c.start).arg(c.end).arg(QString(c.text).replace('\n', '/'));
            CHECK_EQ(entries.join(" | "), QString("10-30:Heilige Scheiße. | 30-40:- Crazy./- Heilige Scheiße. | "
                                                  "40-45:Crazy. | 80-95:Ja."));
        }
        const QVector<Subtitles::TextTrack> streams = Subtitles::textTracks(back.timeline, 25);
        if (CHECK(!streams.isEmpty())) CHECK_EQ(streams[0].name, QString("Mikrofon + Discord"));
    }
    // check (its timeline and layout rules): nothing about the overlapping captions, no text where phone apps cover it
    {
        QVector<ExportCheck::Issue> issues = ExportCheck::timeline(back.timeline, 0, 100, back.format);
        issues += ExportCheck::layout(back.timeline, 0, 100, back.format);
        for (const ExportCheck::Issue& i : issues)
            if (i.check.startsWith("subtitle")) {
                std::printf("       %s %s\n", qPrintable(i.check), qPrintable(i.message));
                CHECK(false);
            }
    }
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

    // Two speakers at once: two lines, the second one above in its own colour, apart from each other
    tl.subtitles[0].captions = CaptionStyle{};
    tl.subtitles[0].captions.mode = CaptionMode::Karaoke;
    SubtitleTrack second = tl.subtitles[0];
    second.captions.highlight = Captions::speakerColor(1, second.captions.highlight);
    second.cues = {cue(15, 50, "drei vier", {{0, 10}, {10, 30}})};
    second.cues[0].id = 3;
    QVector<SubtitleTrack> lanes{tl.subtitles[0], second};
    Captions::stackLanes(lanes, [&](const SubtitleTrack& st, const SubtitleCue& c) {
        return ExportCheck::subtitleCueBlock(st, c.text, fmt.width, fmt.height).height();
    });
    tl.subtitles = lanes;
    const QImage both = Check::renderFrame(tl, fmt, 30, {}, false, true);
    if (qEnvironmentVariableIsSet("CAPTIONS_DUMP")) both.save(qEnvironmentVariable("CAPTIONS_DUMP") + ".two.png");
    // Rows with yellow (first speaker) and with cyan (second): the cyan ones all above the yellow ones
    int yellowTop = INT_MAX, cyanBottom = -1, cyan = 0;
    for (int y = 0; y < both.height(); ++y)
        for (int x = 0; x < both.width(); ++x) {
            const QColor c = both.pixelColor(x, y);
            if (c.red() > 200 && c.green() > 160 && c.blue() < 80) yellowTop = std::min(yellowTop, y);
            if (c.red() < 120 && c.green() > 170 && c.blue() > 200) cyanBottom = std::max(cyanBottom, y), ++cyan;
        }
    CHECK(cyan > 50);
    CHECK(yellowTop < INT_MAX);
    if (!CHECK(cyanBottom < yellowTop)) std::printf("       cyan to %d, yellow from %d\n", cyanBottom, yellowTop);
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

// The cue texts of a file's subtitle streams as stored (stream by stream, " | " between), line breaks as '\n'
QString textPayloads(const QString& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) return "cannot open";
    avformat_find_stream_info(fc, nullptr);
    QMap<int, QStringList> texts;
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(fc, p) >= 0) {
        const AVStream* st = fc->streams[p->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && p->size > 2) {
            QByteArray b(reinterpret_cast<const char*>(p->data), p->size);
            if (st->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT) // 16-bit length, then the text (then style boxes)
                b = b.mid(2, (uchar(b[0]) << 8) | uchar(b[1]));
            texts[p->stream_index] << QString::fromUtf8(b).replace("\r\n", "\n");
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
    avformat_close_input(&fc);
    QStringList out;
    for (const QStringList& t : texts) out << t.join(',');
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
    // ASS escapes and override tags typed by the user show as typed (the text encoders read them as ASS)
    en.cues = {{5, 10, 30, "Hello \\N \\h {\\i1}x"}};
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
        CHECK_EQ(textPayloads(job.path), QString("Hallo {Welt},zwei\nZeilen | Hello \\N \\h {\\i1}x"));
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

// The cue texts of a file's subtitle streams as a player gets them: decoded by FFmpeg's decoders (ASS events, the
// text after the 8th comma; ASS line breaks as '\n'), stream by stream with " | " between
QString decodedTexts(const QString& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) return "cannot open";
    avformat_find_stream_info(fc, nullptr);
    std::vector<AVCodecContext*> dec(fc->nb_streams, nullptr);
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVCodecParameters* par = fc->streams[i]->codecpar;
        const AVCodec* codec = par->codec_type == AVMEDIA_TYPE_SUBTITLE ? avcodec_find_decoder(par->codec_id) : nullptr;
        if (!codec) continue;
        dec[i] = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec[i], par);
        dec[i]->pkt_timebase = fc->streams[i]->time_base;
        if (avcodec_open2(dec[i], codec, nullptr) < 0) avcodec_free_context(&dec[i]);
    }
    QMap<int, QStringList> texts;
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(fc, p) >= 0) {
        AVCodecContext* d = dec[size_t(p->stream_index)];
        AVSubtitle sub{};
        int got = 0;
        if (d && p->size > 2 && avcodec_decode_subtitle2(d, &sub, &got, p) >= 0 && got) {
            for (unsigned r = 0; r < sub.num_rects; ++r)
                if (sub.rects[r]->ass) {
                    const QString ass = QString::fromUtf8(sub.rects[r]->ass);
                    texts[p->stream_index] << ass.section(',', 8).replace("\\N", "\n");
                }
            avsubtitle_free(&sub);
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
    for (AVCodecContext*& d : dec) avcodec_free_context(&d);
    avformat_close_input(&fc);
    QStringList out;
    for (const QStringList& t : texts) out << t.join(',');
    return out.join(" | ");
}

// What a player shows of the embedded text: '<', '&' and "-->" as typed in WebM's WebVTT (FFmpeg's decoder and
// browsers read them as tags/entities: "1 <2" lost everything after '<'); a mov_text cue over 64 KB cut on a
// character, not inside one
void testEmbedPlayers(const QString& dir)
{
    const QString media = Check::makeMedia(dir + "/src2.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1",
                                                                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!media.isEmpty())) return;
    const ProjectFormat fmt{320, 180, {25, 1}};
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 24;
    tl.video[0].clips << c;
    SubtitleTrack t;
    t.name = "T";
    const QString typed = QStringLiteral("1 <2 & 3 &amp; x --> y");
    const QString longText = QString(65534, 'a') + QChar(0xe4); // 65536 bytes in UTF-8, the last character 2 of them
    t.cues = {{1, 2, 10, typed}, {2, 12, 20, longText}};
    tl.subtitles = {t};
    struct Case { const char* file; const char* format; QString expected; };
    // (SRT in MKV has no escape for '<': players read it as a tag there as in an .srt file)
    for (const Case& k : {Case{"p.webm", "vp9", typed + "," + longText}, Case{"p.mp4", "h264", typed + "," + QString(65534, 'a')}}) {
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
        if (!CHECK(ex.start(tl, RenderQueue::exportSettings(job, fmt), &error))) continue;
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!CHECK(ok)) continue;
        const QString got = decodedTexts(job.path);
        if (!CHECK(got == k.expected))
            std::printf("       %s: %s\n", k.file, qPrintable(got.left(80) + " ... " + got.right(20)));
    }
}

// The bundled FFmpeg (AppImage, Windows, Docker) is built from packaging/ffmpeg-components.txt: it must list an encoder
// for each subtitle codec the export picks (MP4/MOV mov_text, MKV SubRip, WebM WebVTT), or every packaged export
// leaves the subtitle streams out without a word (found on Windows, 0.21). The names come from the FFmpeg the test
// links, by codec ID (configure names drop the underscore: mov_text is "movtext").
void testBundledComponents()
{
    QFile f(Check::sourceDir() + "/packaging/ffmpeg-components.txt");
    if (!CHECK(f.open(QIODevice::ReadOnly | QIODevice::Text))) return;
    QSet<QString> encoders, decoders, filters;
    for (const QString& line : QString::fromUtf8(f.readAll()).split('\n')) {
        const QStringList w = line.simplified().split(' ');
        if (w.size() < 2) continue;
        if (w[0] == "encoder") encoders << QString(w[1]).remove('_');
        if (w[0] == "decoder") decoders << QString(w[1]).remove('_');
        if (w[0] == "filter") filters << w[1];
    }
    // Proxies of interlaced material are deinterlaced (engine/ProxyManager.cpp: bwdif, yadif as the fallback)
    for (const char* filter : {"bwdif", "yadif"})
        if (!CHECK(filters.contains(filter))) std::printf("       ffmpeg-components.txt lacks filter %s\n", filter);
    for (const AVCodecID id : {AV_CODEC_ID_MOV_TEXT, AV_CODEC_ID_SUBRIP, AV_CODEC_ID_WEBVTT}) {
        bool encoder = false, decoder = false;
        void* it = nullptr;
        while (const AVCodec* c = av_codec_iterate(&it)) {
            if (c->id != id) continue;
            const QString name = QString::fromLatin1(c->name).remove('_');
            if (av_codec_is_encoder(c) && encoders.contains(name)) encoder = true;
            if (av_codec_is_decoder(c) && decoders.contains(name)) decoder = true;
        }
        if (!CHECK(encoder && decoder)) std::printf("       ffmpeg-components.txt lacks %s\n", avcodec_get_name(id));
    }
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
    testMergeSpeakers();
    testSnapToSound();
    testCueAtCut();
    testAgreement();
    testStackLanes();
    testEdits();
    testSpeed();
    testVtt();
    testProjectFile();
    testWordLayout();
    testWordScripts();
    testTimelineFormat();
    testResetTrackStyle();
    testBundledComponents();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testCliOp(tmp.path());
    testCliOpTwoTracks(tmp.path());
    if (!Check::haveFfmpeg()) {
        Check::result();
        return Check::skip("ffmpeg nicht gefunden (Render-Teil)");
    }
    Check::initEngine();
    testRender(tmp.path());
    testEmbed(tmp.path());
    testEmbedPlayers(tmp.path());
    return Check::result();
}
