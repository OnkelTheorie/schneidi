// SPDX-License-Identifier: AGPL-3.0-only
// Speech to text without whisper: reading whisper-cli's JSON (one word per segment), grouping words into subtitle
// cues (sentences, commas, pauses, length), moving stretched words to where they are spoken, the extensions catalog
// (pinned downloads, dependencies) and the dialogs (EXT_DUMP / TRANSCRIBE_DUMP = image file).
#include "check.h"

#include "app/ExtensionsDialog.h"
#include "app/TranscribeDialog.h"
#include "core/Transcript.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QLabel>

#include <cmath>
#include <cstdio>

namespace {

QString times(const QVector<Transcript::Word>& words)
{
    QStringList out;
    for (const auto& w : words) out << QString("%1-%2 %3").arg(w.from).arg(w.to).arg(w.text);
    return out.join(" | ");
}

// 10 ms levels: loud (-20 dB) in the given ranges (ms), silent elsewhere
Transcript::Levels levels(qint64 lengthMs, const QVector<QPair<qint64, qint64>>& loud)
{
    Transcript::Levels l;
    for (qint64 t = 0; t < lengthMs; t += 10) {
        bool on = false;
        for (const auto& [a, b] : loud) on = on || (t >= a && t < b);
        l.db << (on ? -20.f : -90.f);
    }
    return l;
}

QString cues(const QVector<SubtitleCue>& list)
{
    QStringList out;
    for (const SubtitleCue& c : list) out << QString("%1-%2 %3").arg(c.start).arg(c.end).arg(c.text);
    return out.join(" | ");
}

// Like whisper-cli -ml 1 -sow -oj: an empty first segment, words with leading space, punctuation split off once
const char* kJson = R"({"result":{"language":"de"},"transcription":[
 {"offsets":{"from":0,"to":320},"text":""},
 {"offsets":{"from":320,"to":600},"text":" Hallo"},
 {"offsets":{"from":600,"to":640},"text":","},
 {"offsets":{"from":700,"to":1000},"text":" schöne"},
 {"offsets":{"from":1000,"to":1400},"text":" Grüße."},
 {"offsets":{"from":1400,"to":5000},"text":" Nach"},
 {"offsets":{"from":5000,"to":5300},"text":" der"},
 {"offsets":{"from":5300,"to":5600},"text":" Pause"},
 {"offsets":{"from":5600,"to":5900},"text":" [Musik]"}]})";

} // namespace

// Transcribing in speech pieces (20.11.): DTW word times, packed pieces and their times, the prompt's echo
void testPieces()
{
    using Transcript::Piece;
    using Transcript::PieceSpan;
    // DTW: a word starts at its first token's t_dtw (10 ms steps), not at whisper's segment start (a laugh before it)
    {
        const QByteArray json = R"({"result":{"language":"de"},"transcription":[
            {"text":" Was","offsets":{"from":210,"to":730},"tokens":[{"text":"[_BEG_]","t_dtw":-1},{"text":" Was","t_dtw":282}]},
            {"text":" bist","offsets":{"from":1000,"to":1880},"tokens":[{"text":" bist","t_dtw":298}]},
            {"text":" du","offsets":{"from":1890,"to":2280},"tokens":[{"text":" du","t_dtw":322}]},
            {"text":"?","offsets":{"from":2680,"to":3260},"tokens":[{"text":"?","t_dtw":322}]}]})";
        Transcript::Result r;
        CHECK(Transcript::parseWhisperJson(json, &r));
        CHECK_EQ(r.words.size(), 3);
        CHECK_EQ(r.words.value(0).from, 2820);
        CHECK_EQ(r.words.value(1).from, 2980);
        CHECK_EQ(r.words.value(2).text, QString("du?"));
        CHECK_EQ(r.words.value(2).from, 3220);
        // a word ends where the next starts at the latest, never before its own start
        for (int i = 0; i + 1 < r.words.size(); ++i) {
            CHECK(r.words[i].to >= r.words[i].from);
            CHECK(r.words[i].to <= r.words[i + 1].from);
        }
        // Appending a second piece through a map (+10 s): times shifted, order kept, the language stays
        CHECK(Transcript::appendWhisperJson(json, [](qint64 t, bool) { return t + 10000; }, &r));
        CHECK_EQ(r.words.size(), 6);
        CHECK_EQ(r.words.value(3).from, 12820);
        CHECK_EQ(r.language, QString("de"));
        // Without DTW times: whisper's own
        Transcript::Result plain;
        CHECK(Transcript::parseWhisperJson(R"({"transcription":[{"text":" Hallo","offsets":{"from":500,"to":900}}]})", &plain));
        CHECK_EQ(plain.words.value(0).from, 500);
    }
    // Packing: short pauses stay, long ones shrink to 400 ms of silence, a piece stays within 28 s
    {
        const QVector<QPair<qint64, qint64>> segs{{1000, 3000}, {3500, 5000}, {20000, 22000}, {60000, 61000}};
        const QVector<Piece> pieces = Transcript::speechPieces(segs, 90000);
        CHECK_EQ(pieces.size(), 1);
        const Piece p = pieces.value(0);
        CHECK_EQ(p.size(), 3);                  // [1-5 s], [20-22 s], [60-61 s], padded by 200 ms
        CHECK_EQ(p.value(0).from, 800);         // 1000 - 200
        CHECK_EQ(p.value(0).length, 5200 - 800); // to 5000 + 200, the 500 ms pause kept
        CHECK_EQ(p.value(1).at, p.value(0).length + 400);
        CHECK_EQ(p.value(1).from, 19800);
        // Times: in a stretch -> the sound's; in the silence between -> the next stretch's start
        CHECK_EQ(Transcript::pieceTime(p, 100), 900);
        CHECK_EQ(Transcript::pieceTime(p, p.value(1).at + 1000), 20800);
        CHECK_EQ(Transcript::pieceTime(p, p.value(0).length + 200), 19800);
        // ... but an end in that silence stays at the end of the stretch before (not over the whole real pause)
        CHECK_EQ(Transcript::pieceTime(p, p.value(0).length + 200, true), 5200);
        CHECK(Transcript::pieceLength(p) <= 28000);
        // Neighbours: the padding never passes the middle of the pause
        const QVector<Piece> tight = Transcript::speechPieces({{1000, 2000}, {2100, 3000}}, 10000);
        CHECK_EQ(tight.value(0).size(), 1);
        CHECK_EQ(tight.value(0).value(0).from, 800);
        // 70 s of speech in 3 s bursts with 2 s pauses: several pieces, none over 28 s, every second in one
        QVector<QPair<qint64, qint64>> many;
        for (qint64 t = 0; t < 70000; t += 5000) many << qMakePair(t, t + 3000);
        const QVector<Piece> packed = Transcript::speechPieces(many, 70000);
        CHECK(packed.size() >= 2);
        qint64 covered = 0;
        for (const Piece& q : packed) {
            CHECK(Transcript::pieceLength(q) <= 28000);
            for (const PieceSpan& sp : q) covered += sp.length;
        }
        CHECK(covered >= 14 * 3000);
        // Dense talk (2.5 s segments, 200 ms pauses) for 70 s: pieces are cut between segments, never inside one
        QVector<QPair<qint64, qint64>> dense;
        for (qint64 t = 0; t < 70000; t += 2700) dense << qMakePair(t, t + 2500);
        const QVector<Piece> densePieces = Transcript::speechPieces(dense, 72000);
        CHECK(densePieces.size() >= 3);
        for (const auto& [from, to] : dense) {
            bool inOne = false;
            for (const Piece& q : densePieces)
                for (const PieceSpan& sp : q) inOne = inOne || (sp.from <= from && to <= sp.from + sp.length);
            if (!CHECK(inOne)) std::printf("       segment %lld-%lld cut\n", from, to);
        }
        for (const Piece& q : densePieces) CHECK(Transcript::pieceLength(q) <= 28000);
        // Full: cut at the longest pause in the second half (2 s before 22 s), not where it happens to be full
        const QVector<Piece> cutAt = Transcript::speechPieces({{0, 10000}, {10500, 20000}, {22000, 24000}, {24300, 30000}},
                                                              30000);
        CHECK_EQ(cutAt.size(), 2);
        CHECK_EQ(cutAt.value(1).value(0).from, 21800); // the second piece starts with the 22 s segment (padded)
        // Speech without a pause longer than a piece is split
        const QVector<Piece> longSpeech = Transcript::speechPieces({{0, 65000}}, 65000);
        CHECK_EQ(longSpeech.size(), 3);
        // Nothing / out of range
        CHECK(Transcript::speechPieces({}, 1000).isEmpty());
        CHECK(Transcript::speechPieces({{5000, 6000}}, 1000).isEmpty());
    }
    // The starting text whisper echoed in silence goes; the same words in another order stay
    {
        QVector<Transcript::Word> w{{0, 100, "Hallo,"}, {100, 200, "wie"}, {200, 300, "geht's?"}, {300, 400, "Gut,"},
                                    {400, 500, "und"}, {500, 600, "dir?"}, {600, 700, "Ja,"}, {700, 800, "passt."},
                                    {900, 1000, "Echt"}, {1000, 1100, "jetzt?"}};
        CHECK_EQ(Transcript::dropPromptEcho(w, "Hallo, wie geht's? Gut, und dir? Ja, passt."), 8);
        CHECK_EQ(w.size(), 2);
        QVector<Transcript::Word> kept{{0, 100, "Wie"}, {100, 200, "geht's?"}};
        CHECK_EQ(Transcript::dropPromptEcho(kept, "Hallo, wie geht's? Gut, und dir? Ja, passt."), 0);
    }
    // Repeats (whisper turbo wrote a piece's whole text twice, or the piece before once more)
    {
        const auto words = [](const QString& text, qint64 from, qint64 step) {
            QVector<Transcript::Word> out;
            for (const QString& s : text.split(' ', Qt::SkipEmptyParts)) out << Transcript::Word{from, from + step, s}, from += step;
            return out;
        };
        const auto text = [](const QVector<Transcript::Word>& ws, qsizetype from = 0) {
            QStringList out;
            for (qsizetype i = from; i < ws.size(); ++i) out << ws[i].text;
            return out.join(' ');
        };
        const QString twice = "Das Schwimmen ist hier sicher. Es gibt Familienstrände. Das Schwimmen ist hier sicher. Es "
                              "gibt Familienstrände.";
        // One piece written twice: the first copy stays with its times (whisper squeezed the copy into the piece's
        // end: 19 words at 17.3 s with DTW, measured 23.11.), its last word lasting to the copy's end
        QVector<Transcript::Word> w = words(twice, 0, 1000);
        for (int i = 8; i < w.size(); ++i) w[i].from = 8000 + (i - 8) * 10, w[i].to = w[i].from + 10;
        CHECK_EQ(Transcript::dropRepeats(w, 0), 8);
        CHECK_EQ(text(w), QString("Das Schwimmen ist hier sicher. Es gibt Familienstrände."));
        CHECK_EQ(w.first().from, 0);
        CHECK_EQ(w[3].from, 3000);
        CHECK_EQ(w.last().from, 7000);
        CHECK_EQ(w.last().to, 8080);
        // The copy a word short or with one word changed (case and punctuation do not count)
        w = words("Das Schwimmen ist hier sicher, es gibt Familienstrände das schwimmen ist hier sicher es gibt", 0, 500);
        CHECK_EQ(Transcript::dropRepeats(w, 0), 7);
        CHECK_EQ(text(w), QString("Das Schwimmen ist hier sicher, es gibt Familienstrände"));
        w = words("Das Schwimmen ist hier sicher. Es gibt Familienstrände. Das Schwimmen ist hier sicher. Es gibt "
                  "Strände.",
                  0, 500);
        CHECK_EQ(Transcript::dropRepeats(w, 0), 8);
        // Not a repeat: a short phrase said twice, a chant, different halves, too many changes
        for (const QString& kept : {QString("Ja, ja. Ja, ja."), QString("Go, go, go! Go, go, go!"),
                                    QString("ja ja ja ja ja ja ja ja"),
                                    QString("Wir gehen jetzt rein und dann gucken wir mal was da so los ist"),
                                    QString("eins zwei drei vier fünf sechs eins zwei sieben acht neun zehn")}) {
            QVector<Transcript::Word> k = words(kept, 0, 300);
            CHECK_EQ(Transcript::dropRepeats(k, 0), 0);
            CHECK_EQ(text(k), kept);
        }
        // The piece before written once more: the whole piece goes; times of the piece before stay
        QVector<Transcript::Word> two = words("Legierungen sind eine Mischung aus Metallen.", 0, 400);
        two += words("Legierungen sind eine Mischung aus Metallen.", 10000, 400);
        CHECK_EQ(Transcript::dropRepeats(two, 6, 0), 6);
        CHECK_EQ(two.size(), 6);
        CHECK_EQ(two.last().to, 2400);
        // ... but a new piece that only starts alike stays, and so does one without a piece before
        two = words("Legierungen sind eine Mischung aus Metallen.", 0, 400);
        two += words("Legierungen sind eine Mischung aus zwei oder mehr Metallen, vergessen Sie das nicht.", 10000, 400);
        CHECK_EQ(Transcript::dropRepeats(two, 6, 0), 0);
        CHECK_EQ(two.size(), 19);
        QVector<Transcript::Word> alone = words("Legierungen sind eine Mischung aus Metallen.", 0, 400);
        CHECK_EQ(Transcript::dropRepeats(alone, 0, -1), 0);
        // Only the newest piece is looked at: a doubled earlier piece stays as it was
        QVector<Transcript::Word> later = words(twice, 0, 500);
        const qsizetype second = later.size();
        later += words("Und jetzt etwas ganz anderes zum Schluss.", 20000, 400);
        CHECK_EQ(Transcript::dropRepeats(later, second, 0), 0);
        CHECK_EQ(later.size(), second + 7);
        CHECK(Transcript::dropRepeats(later, later.size()) == 0); // an empty piece
    }
    // DTW presets by model file, speech segments of the voice detection program
    CHECK_EQ(Transcriber::dtwPreset("/x/ggml-large-v3-q5_0.bin"), QString("large.v3"));
    CHECK_EQ(Transcriber::dtwPreset("ggml-large-v3-turbo-q5_0.bin"), QString("large.v3.turbo"));
    CHECK_EQ(Transcriber::dtwPreset("ggml-small-q5_1.bin"), QString("small"));
    CHECK_EQ(Transcriber::dtwPreset("ggml-base.en.bin"), QString("base.en"));
    CHECK_EQ(Transcriber::dtwPreset("ggml-tiny-q5_1.bin"), QString("tiny"));
    CHECK(Transcriber::dtwPreset("my-model.bin").isEmpty());
    CHECK(Transcriber::dtwPreset("ggml-distil-large-v3.bin").isEmpty());
    const auto segs = Transcriber::parseSpeechSegments(
        "Detected 2 speech segments:\nSpeech segment 0: start = 0.00, end = 118.00\nSpeech segment 1: start = 151.00, "
        "end = 195.50\n");
    CHECK_EQ(segs.size(), 2);
    CHECK_EQ(segs.value(1).first, 1510);
    CHECK_EQ(segs.value(1).second, 1955);
    CHECK_EQ(Transcriber::punctuationPrompt("de").isEmpty(), false);
    CHECK(Transcriber::punctuationPrompt("auto").isEmpty());
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("transcript");

    Transcript::Result r;
    CHECK(Transcript::parseWhisperJson(kJson, &r));
    CHECK_EQ(r.language, QString("de"));
    QStringList words;
    for (const auto& w : r.words) words << w.text;
    CHECK_EQ(words.join(' '), QString("Hallo, schöne Grüße. Nach der Pause"));
    CHECK(!Transcript::parseWhisperJson("{broken", &r));

    // 25 fps, offset 100: sentence end splits, the 3.6 s "pause" inside "Nach" is cut by spokenEnd and
    // the gap before "der" starts a new cue
    CHECK(Transcript::parseWhisperJson(kJson, &r));
    CHECK_EQ(cues(Transcript::toCues(r.words, 25, 42, 700, 100)),
             QString("108-135 Hallo, schöne Grüße. | 135-155 Nach | 225-251 der Pause"));
    // Short lines: a comma in the second half ends a cue; nothing longer than maxChars unless one word is
    CHECK_EQ(cues(Transcript::toCues(r.words, 25, 10, 5000)),
             QString("8-18 Hallo, | 18-25 schöne | 25-35 Grüße. | 35-133 Nach der | 133-154 Pause"));
    // A long phrase becomes even lines, not a full line and one word left over ("… beleidigt dich | nicht.")
    {
        QVector<Transcript::Word> ws;
        qint64 t = 0;
        for (const QString& s : QString("aber wenn du Softbox guckst beleidigt dich nicht.").split(' '))
            ws << Transcript::Word{t, t + 300, s}, t += 300;
        QStringList lines;
        for (const auto& c : Transcript::toCues(ws, 25, 42)) lines << c.text;
        CHECK_EQ(lines.join(" | "), QString("aber wenn du Softbox guckst | beleidigt dich nicht."));
    }

    // Levels of a 1 kHz sine at half scale: about -9 dB, silence -120 dB
    {
        QVector<qint16> pcm(16000);
        for (int i = 0; i < 8000; ++i) pcm[i] = qint16(16384 * std::sin(2 * M_PI * 1000 * i / 16000.0));
        const Transcript::Levels l = Transcript::levelsOf(pcm.constData(), pcm.size(), 16000);
        CHECK_EQ(l.db.size(), 100);
        CHECK(std::abs(l.db[10] + 9.03f) < 0.2f);
        CHECK(l.db[80] < -100.f);
    }
    using W = Transcript::Word;
    // Laughter case (real recording): laughter 6.5–13.4 s, "Diese Scheiß-Pferd, Alter" from 13.9 s; whisper starts
    // "Diese" on the laughter. The stretched run moves up to "Alter", which follows it directly
    {
        QVector<W> w{{6530, 10030, "Diese"}, {10030, 16040, "Scheiß-Pferd,"}, {16040, 16720, "Alter."}};
        Transcript::fitToSpeech(w, levels(17000, {{6540, 7670}, {8650, 9130}, {9840, 13350}, {13920, 17000}}));
        CHECK_EQ(times(w), QString("14280-14880 Diese | 14880-16040 Scheiß-Pferd, | 16040-16720 Alter."));
    }
    // Real recording: "Turm auf F7" (sound 2.57–3.86 s), silence, "Mental Advantage" from 5.55 s; whisper stretches
    // "F7," over the silence up to "Mental". The sound is at its start, so it stays there
    {
        QVector<W> w{{1320, 2860, "Turm"}, {2860, 3310, "auf"}, {3310, 5540, "F7,"}, {5540, 5640, "Mental"}};
        Transcript::fitToSpeech(w, levels(8000, {{2570, 3860}, {5550, 7590}}));
        CHECK_EQ(times(w), QString("2330-2860 Turm | 2860-3310 auf | 3400-3860 F7, | 5540-5640 Mental"));
    }
    // Stretched first word without sound at its start (whisper's pause before the speech): moves up to the next word
    {
        QVector<W> w{{1000, 6000, "Nach"}, {6000, 6400, "der"}};
        Transcript::fitToSpeech(w, levels(7000, {{5500, 6400}}));
        CHECK_EQ(times(w), QString("5470-6000 Nach | 6000-6400 der"));
    }
    // Real recording: whisper puts "Wie geht" 0.8 s before "das?" into silence, all three are spoken from 13.59 s
    {
        QVector<W> w{{12667, 12767, "Wie"}, {12667, 12767, "geht"}, {13567, 14100, "das?"}};
        Transcript::fitToSpeech(w, levels(15000, {{12020, 12330}, {13590, 14160}}));
        CHECK_EQ(times(w), QString("13467-13567 Wie | 13467-13567 geht | 13567-14100 das?"));
    }
    // Real recording (2nd run): whisper gives the words no time of their own and stretches the last one instead
    // ("Hey," "auf" at 7.79 s in digital silence, spoken from 10.3 s; "Wie" "geht" "das?" at 12.28 s, spoken from
    // 13.5 s, a crackle at 12.3 s): the words that start together go along with the stretched one
    {
        QVector<W> w{{7790, 7890, "Hey,"}, {7790, 10460, "auf"}, {10460, 11050, "YouTube."},
                     {12280, 12380, "Wie"}, {12280, 12380, "geht"}, {12280, 14050, "das?"}};
        Transcript::fitToSpeech(w, levels(15000, {{10300, 11400}, {12300, 12350}, {13500, 14100}}));
        CHECK_EQ(times(w), QString("9470-10000 Hey, | 10000-10460 auf | 10460-11050 YouTube. | "
                                   "12530-12990 Wie | 12990-13520 geht | 13520-14050 das?"));
        QVector<SubtitleCue> cs = Transcript::toCues(w, 100);
        Transcript::startAtSound(cs, levels(15000, {{10300, 11400}, {12300, 12350}, {13500, 14100}}), 100);
        CHECK_EQ(cues(cs), QString("1020-1105 Hey, auf YouTube. | 1340-1405 Wie geht das?"));
    }
    // Alone between pauses: the loud part; nothing loud or no levels: keeps its start
    {
        QVector<W> w{{1000, 1300, "So"}, {2000, 6000, "weg"}, {7000, 7300, "da"}};
        Transcript::fitToSpeech(w, levels(8000, {{1000, 1300}, {4000, 4500}, {7000, 7300}}));
        CHECK_EQ(times(w), QString("1000-1300 So | 4040-4500 weg | 7000-7300 da"));
        QVector<W> quiet{{1000, 1300, "So"}, {2000, 6000, "weg"}, {7000, 7300, "da"}};
        Transcript::fitToSpeech(quiet, {});
        CHECK_EQ(times(quiet), QString("1000-1300 So | 2000-2460 weg | 7000-7300 da"));
    }

    // Cues that start in silence begin 100 ms before the sound (25 fps, offset 100 frames = audio start)
    {
        QVector<SubtitleCue> cs{{0, 100 + 25, 100 + 75, "Pferd, geh"}, {0, 100 + 75, 100 + 90, "laut"}};
        Transcript::startAtSound(cs, levels(4000, {{2000, 2600}, {3000, 3600}}), 25, 100);
        CHECK_EQ(cues(cs), QString("147-175 Pferd, geh | 175-190 laut"));
    }
    // Cues end where their sound does (+200 ms), not after the reading minimum in the silence after it. Synthetic
    // speech: a 220 Hz tone in 4 Hz syllables from 1.0 s, its last syllable fading out by 20 dB until 2.2 s (a soft
    // ending, kept), then silence; a quiet hiss under everything (-70 dB)
    {
        constexpr int kRate = 16000;
        QVector<qint16> pcm(5 * kRate);
        quint32 seed = 1;
        for (int i = 0; i < pcm.size(); ++i) {
            const double t = double(i) / kRate;
            seed = seed * 1664525u + 1013904223u;
            double v = 10.0 * (double(seed >> 16) / 32768.0 - 1.0); // hiss
            if (t >= 1.0 && t < 2.2) {
                const double syllable = 0.55 + 0.45 * std::sin(2 * M_PI * 4 * (t - 1.0));
                const double fade = t < 2.0 ? 1.0 : std::pow(10.0, -20.0 * (t - 2.0) / 0.2 / 20);
                v += 8000 * syllable * fade * std::sin(2 * M_PI * 220 * t);
            }
            pcm[i] = qint16(std::lround(v));
        }
        const Transcript::Levels l = Transcript::levelsOf(pcm.constData(), pcm.size(), kRate);
        // Whisper's words to 2.1 s; toCues adds the reading minimum (0.6 s + 50 ms per character) into the silence
        const QVector<W> w{{1000, 1400, "Und"}, {1400, 1700, "ich"}, {1700, 1900, "hasse"},
                           {1900, 2100, "Mathe"}, {2100, 2100, "wirklich"}, {2100, 2100, "sehr."}};
        QVector<SubtitleCue> cs = Transcript::toCues(w, 25, 42, 700, 100);
        CHECK(cs.size() == 1 && cs[0].end >= 100 + 75); // 1.0 s + 2.3 s minimum: past 3.0 s
        Transcript::startAtSound(cs, l, 25, 100);
        Transcript::endAtSound(cs, l, 25, 100);
        // The fading syllable is kept (sound to ~2.2 s), then 200 ms: 2.4 s = frame 60 (+100 offset); start unchanged
        if (!CHECK(cs.size() == 1 && cs[0].start == 100 + 25 && cs[0].end >= 100 + 59 && cs[0].end <= 100 + 61))
            std::printf("       %s\n", qPrintable(cues(cs)));
        // Sound to the end of a cue (the next speaker, laughter): unchanged; a short cue keeps 0.6 s at least
        QVector<SubtitleCue> loud{{0, 100 + 30, 100 + 50, "laut"}, {0, 100 + 52, 100 + 70, "Ja!"}};
        Transcript::endAtSound(loud, l, 25, 100);
        CHECK_EQ(cues(loud), QString("130-150 laut | 152-167 Ja!"));
        // Levels of real recordings: the same with the 10 ms frames of the test helper (speech -20 dB, silence -90 dB)
        QVector<SubtitleCue> real{{0, 25, 100, "Nein, massier mir mal meine Eier"}};
        Transcript::endAtSound(real, levels(5000, {{1000, 2500}}), 25);
        CHECK_EQ(cues(real), QString("25-67 Nein, massier mir mal meine Eier"));
        // No levels: unchanged
        QVector<SubtitleCue> none{{0, 25, 100, "x"}};
        Transcript::endAtSound(none, {}, 25);
        CHECK_EQ(cues(none), QString("25-100 x"));
    }
    // A cue without any sound is made up (whisper in silence): dropped
    {
        QVector<SubtitleCue> cs{{0, 10, 20, "Hey,"}, {0, 50, 60, "da"}};
        Transcript::startAtSound(cs, levels(3000, {{2000, 2400}}), 25);
        CHECK_EQ(cues(cs), QString("50-60 da"));
    }

    // Two speakers in one track: overlapping cues share one cue (a line each), the rest stays apart
    CHECK_EQ(cues(Transcript::mergeCues({{{0, 10, 40, "Ja, ja."}, {0, 100, 120, "Okay, tschüss."}},
                                         {{0, 50, 60, "Oder da?"}, {0, 110, 130, "Ich hab's"}}})),
             QString("10-40 Ja, ja. | 50-60 Oder da? | 100-130 Okay, tschüss.\nIch hab's"));

    // Dialogs: descriptions fully visible, track choice only with several tracks (muted ones unticked)
    {
        ExtensionsDialog dlg;
        dlg.show();
        for (QLabel* l : dlg.findChildren<QLabel*>())
            if (l->text().contains("<small>") && !CHECK(l->height() >= l->sizeHint().height()))
                std::printf("       %s: %d < %d\n", qPrintable(l->text().left(40)), l->height(), l->sizeHint().height());
        if (const QString dump = qEnvironmentVariable("EXT_DUMP"); !dump.isEmpty()) dlg.grab().save(dump);
    }
    {
        Timeline tl;
        tl.audio.resize(3);
        for (Track& t : tl.audio) t.clips << Clip{};
        tl.audio[0].muted = true;
        TranscribeDialog dlg(tl, ProjectFormat{}, 0, 100, {"A1 mix", "A2 Audio 2 – System", "A3 Audio 3 – Mikrofon"});
        QStringList ticked;
        for (QCheckBox* b : dlg.findChildren<QCheckBox*>()) ticked << QString("%1:%2").arg(b->text()).arg(b->isChecked());
        // The microphone alone when there is more to hear (the game sound would bring its music's lyrics along)
        CHECK_EQ(ticked.join(' '), QString("A1 mix:0 A2 Audio 2 – System:0 A3 Audio 3 – Mikrofon:1 Eine Untertitelspur pro Tonspur:1"));
        bool told = false;
        for (QLabel* l : dlg.findChildren<QLabel*>()) told = told || l->text().startsWith("Nur die Stimme ist angehakt");
        CHECK(told);
        dlg.show();
        if (const QString dump = qEnvironmentVariable("TRANSCRIBE_DUMP"); !dump.isEmpty()) dlg.grab().save(dump);
        TranscribeDialog one(tl, ProjectFormat{}, 0, 100, {"A1 mix", "", ""});
        CHECK(one.findChildren<QCheckBox*>().isEmpty());
        // No voice track named: as heard, no hint
        TranscribeDialog plain(tl, ProjectFormat{}, 0, 100, {"A1 mix", "A2 Audio 2", "A3 Audio 3"});
        ticked.clear();
        for (QCheckBox* b : plain.findChildren<QCheckBox*>()) ticked << QString("%1:%2").arg(b->text()).arg(b->isChecked());
        CHECK_EQ(ticked.join(' '), QString("A1 mix:0 A2 Audio 2:1 A3 Audio 3:1 Eine Untertitelspur pro Tonspur:1"));
        for (QLabel* l : plain.findChildren<QLabel*>()) CHECK(!l->text().startsWith("Nur die Stimme"));
    }

    // Own whisper models: a ggml file anywhere, checked by its header; listed after the catalog, a gone one marked
    {
        QTemporaryDir dir;
        const auto write = [&](const char* name, const QByteArray& bytes) {
            const QString path = dir.filePath(name);
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(bytes);
            return path;
        };
        const QByteArray magic("lmgg", 4); // GGML_FILE_MAGIC 0x67676d6c, little-endian
        const QString good = write("ggml-own.bin", magic + QByteArray("\x59\xca\0\0", 4) + QByteArray(64, '\0'));
        const QString other = write("ggml-other.bin", magic + QByteArray("\x9a\xca\0\0", 4) + QByteArray(64, '\0'));
        const QString vad = write("ggml-silero.bin", magic + QByteArray("\x0a\0\0\0", 4) + QByteArray(64, '\0'));
        const QString text = write("notes.bin", "hello world, no model");
        const QString gguf = write("model.gguf", QByteArray("GGUF\3\0\0\0", 8) + QByteArray(64, '\0'));
        const QString gone = dir.filePath("ggml-gone.bin");
        QString error;
        CHECK(Extensions::isWhisperModelFile(good, &error));
        CHECK(error.isEmpty());
        CHECK(!Extensions::isWhisperModelFile(text, &error));
        CHECK(error.startsWith("Das ist kein whisper.cpp-Modell"));
        CHECK(!Extensions::isWhisperModelFile(vad, &error));
        CHECK(error.contains("kein Whisper-Sprachmodell"));
        CHECK(!Extensions::isWhisperModelFile(gguf, &error));
        CHECK(error.contains("GGUF"));
        CHECK(!Extensions::isWhisperModelFile(gone, &error));
        CHECK_EQ(Extensions::whisperModelPath(good), QFileInfo(good).absoluteFilePath()); // a path resolves
        CHECK(Extensions::installedWhisperModels().isEmpty()); // test paths: no catalog model
        Extensions::setCustomWhisperModels({gone});
        CHECK(!Extensions::hasWhisperModel());
        CHECK(Extensions::appWhisperModel({}).isEmpty());
        Extensions::setCustomWhisperModels({gone, good});
        CHECK(Extensions::hasWhisperModel()); // the panel's "Whisper ready" with only an own model
        CHECK_EQ(Extensions::appWhisperModel({}), QFileInfo(good).absoluteFilePath());
        CHECK_EQ(Extensions::appWhisperModel("whisper-model-turbo"), QFileInfo(good).absoluteFilePath());

        Timeline tl;
        tl.audio.resize(1);
        tl.audio[0].clips << Clip{};
        TranscribeDialog dlg(tl, ProjectFormat{}, 0, 100, {"A1 mix"});
        auto* combo = dlg.findChild<QComboBox*>("transcribe_model");
        auto* remove = dlg.findChild<QToolButton*>("transcribe_model_remove");
        auto* items = combo ? qobject_cast<QStandardItemModel*>(combo->model()) : nullptr;
        auto* start = [&]() -> QPushButton* {
            for (auto* b : dlg.findChildren<QPushButton*>())
                if (b->text() == "Erzeugen" || b->text() == "Herunterladen und erzeugen") return b;
            return nullptr;
        }();
        if (CHECK(combo && remove && items && start)) {
            const auto entries = [&] {
                QStringList out;
                for (int i = 0; i < combo->count(); ++i)
                    out << combo->itemText(i) + (items->item(i)->isEnabled() ? "" : ":off");
                return out.join(" | ");
            };
            // The quality levels first, as downloads (where the whisper package is offered), then the own models
            const bool offered = Extensions::find("whisper") != nullptr;
            const QString levels = offered ? QString("Whisper „Genau“ (large-v3) – Download 1081 MB | Whisper „Schnell“ "
                                                     "(large-v3-turbo) – Download 574 MB | ")
                                           : QString();
            const QString parakeet;
            CHECK_EQ(entries(), levels + parakeet + "ggml-gone.bin (fehlt):off | ggml-own.bin | Eigenes Modell…");
            CHECK_EQ(combo->currentText(), QString("ggml-own.bin")); // the first installed one, not the gone file
            CHECK_EQ(combo->currentData(Qt::ToolTipRole).toString(),
                     QDir::toNativeSeparators(QFileInfo(good).absoluteFilePath()));
            CHECK_EQ(start->text(), QString("Erzeugen"));
            CHECK(dlg.missingDownloads().isEmpty());
            if (offered) {
                // A missing level: downloaded with the whisper package when the run starts
                combo->setCurrentIndex(combo->findData("whisper-model-large"));
                QStringList ids;
                for (const Extensions::Item* i : dlg.missingDownloads()) ids << i->id;
                CHECK_EQ(ids.join(','), QString("whisper,whisper-model-large,whisper-vad"));
                combo->setCurrentIndex(combo->findText("ggml-own.bin"));
            }
            CHECK(!remove->isHidden());
            CHECK(!dlg.addCustomModel(text, &error));
            CHECK(!error.isEmpty());
            CHECK_EQ(Extensions::customWhisperModels().size(), 2);
            CHECK(dlg.addCustomModel(other, &error));
            CHECK_EQ(combo->currentText(), QString("ggml-other.bin"));
            CHECK_EQ(entries(), levels + parakeet + "ggml-gone.bin (fehlt):off | ggml-own.bin | ggml-other.bin | Eigenes Modell…");
            CHECK(dlg.addCustomModel(other)); // twice = once
            CHECK_EQ(Extensions::customWhisperModels().size(), 3);
            remove->click();
            CHECK_EQ(entries(), levels + parakeet + "ggml-gone.bin (fehlt):off | ggml-own.bin | Eigenes Modell…");
            CHECK_EQ(combo->currentText(), QString("ggml-own.bin"));
            combo->setCurrentIndex(combo->findText("ggml-gone.bin (fehlt)")); // the gone one can be taken out too
            CHECK(!remove->isHidden());
            remove->click();
            CHECK_EQ(Extensions::customWhisperModels(), QStringList{QFileInfo(good).absoluteFilePath()});
        }
        Extensions::setCustomWhisperModels({});
        CHECK(!QSettings().contains("transcribe/customModels"));
    }

    // Catalog: every download pinned; whisper brings the voice detection, models need the program
    for (const Extensions::Item& i : Extensions::catalog()) {
        CHECK_EQ(i.sha256.size(), 64);
        CHECK(i.size > 0);
        CHECK(i.url.startsWith("https://"));
    }
    if (const Extensions::Item* w = Extensions::find("whisper")) {
        const auto deps = Extensions::withDependencies({w});
        CHECK_EQ(deps.size(), 2);
        CHECK_EQ(deps[1]->id, QString("whisper-vad"));
    }
    CHECK(Extensions::find("whisper-model-small") != nullptr);
    // The quality levels: large-v3 "Genau" first (the best on real chat, measured 22.11.), turbo "Schnell"; "large"
    // names it. Without a GPU backend beside whisper the default is "fast"
    {
        QStringList models;
        for (const Extensions::Item& i : Extensions::catalog())
            if (i.kind == Extensions::Kind::WhisperModel) models << i.id;
        CHECK_EQ(models.value(0), QString("whisper-model-large"));
        CHECK_EQ(models.value(1), QString("whisper-model-turbo"));
        CHECK_EQ(Extensions::defaultQuality(), QString("fast"));
        CHECK_EQ(Extensions::qualityModel("accurate"), QString("whisper-model-large"));
        QString unknown;
        CHECK(Extensions::parseList("large", &unknown).value(0) == Extensions::find("whisper-model-large"));
        CHECK_EQ(Extensions::find("whisper-model-large")->sha256.size(), 64);
    }
    testPieces();
    return Check::result();
}
