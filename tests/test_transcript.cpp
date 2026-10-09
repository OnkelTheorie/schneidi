// Speech to text without whisper: reading whisper-cli's JSON (one word per segment), grouping words into subtitle
// cues (sentences, commas, pauses, length), moving stretched words to where they are spoken, the extensions catalog
// (pinned downloads, dependencies) and the dialogs (EXT_DUMP / TRANSCRIBE_DUMP = image file).
#include "check.h"

#include "app/ExtensionsDialog.h"
#include "app/TranscribeDialog.h"
#include "core/Transcript.h"
#include "engine/Extensions.h"

#include <QApplication>
#include <QCheckBox>
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
        CHECK_EQ(ticked.join(' '), QString("A1 mix:0 A2 Audio 2 – System:1 A3 Audio 3 – Mikrofon:1 Eine Untertitelspur pro Tonspur:1"));
        dlg.show();
        if (const QString dump = qEnvironmentVariable("TRANSCRIBE_DUMP"); !dump.isEmpty()) dlg.grab().save(dump);
        TranscribeDialog one(tl, ProjectFormat{}, 0, 100, {"A1 mix", "", ""});
        CHECK(one.findChildren<QCheckBox*>().isEmpty());
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
    return Check::result();
}
