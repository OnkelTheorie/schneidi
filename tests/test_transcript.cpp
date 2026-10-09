// Speech to text without whisper: reading whisper-cli's JSON (one word per segment), grouping words into subtitle
// cues (sentences, commas, pauses, length) and the extensions catalog (pinned downloads, dependencies).
#include "check.h"

#include "core/Transcript.h"
#include "engine/Extensions.h"

#include <QCoreApplication>

namespace {

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
    QCoreApplication app(argc, argv);
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
             QString("108-135 Hallo, schöne Grüße. | 135-148 Nach | 225-240 der Pause"));
    // Short lines: a comma in the second half ends a cue; nothing longer than maxChars unless one word is
    CHECK_EQ(cues(Transcript::toCues(r.words, 25, 10, 5000)),
             QString("8-16 Hallo, | 18-25 schöne | 25-35 Grüße. | 35-133 Nach der | 133-140 Pause"));

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
