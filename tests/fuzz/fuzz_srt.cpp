// Fuzz target: subtitle files (SRT, core/Subtitles.h). A file is read in a project frame rate, put on a new
// subtitle track like the import does (Editor::importSubtitles), written out again (whole and a range) and read back.
//
// Rules checked: the entries are sorted, at least one frame long and do not overlap (parseSrt promises it), and an
// exported file reads back to the same times.
#include "FuzzCommon.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"

#include <QHash>

namespace {

void checkCues(const QVector<SubtitleCue>& cues)
{
    for (int i = 0; i < cues.size(); ++i) {
        FUZZ_ASSERT(cues[i].start >= 0);
        FUZZ_ASSERT(cues[i].end > cues[i].start);
        if (i > 0) FUZZ_ASSERT(cues[i - 1].end <= cues[i].start);
    }
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Fuzz::init("srt");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char*>(data), qsizetype(size));
    const size_t h = qHash(bytes); // choices from the content: plain .srt files are the seeds
    const FrameRate rate = kFrameRates[h % std::size(kFrameRates)];
    int skipped = -1;
    const QVector<SubtitleCue> cues = Subtitles::parseSrt(bytes, rate.fps(), &skipped);
    FUZZ_ASSERT(skipped >= 0);
    checkCues(cues);
    if (cues.isEmpty()) return 0;

    ProjectData pd;
    pd.format.rate = rate;
    Project p;
    p.load(pd);
    Selection selection;
    Editor editor(&p, &selection);
    editor.importSubtitles(cues, "fuzz");
    for (const SubtitleTrack& t : p.timeline().subtitles) checkCues(t.cues);

    const QVector<SubtitleCue> back = Subtitles::parseSrt(Subtitles::toSrt(cues, rate.fps()), rate.fps());
    FUZZ_ASSERT(back.size() == cues.size());
    for (int i = 0; i < cues.size(); ++i) {
        FUZZ_ASSERT(back[i].start == cues[i].start);
        FUZZ_ASSERT(back[i].end == cues[i].end);
    }
    const int from = int(h >> 8) % std::max(1, cues.last().end), until = from + int(h >> 20) % 1000;
    checkCues(Subtitles::parseSrt(Subtitles::toSrt(cues, rate.fps(), from, until), rate.fps()));
    return 0;
}
