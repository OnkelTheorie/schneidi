// SPDX-License-Identifier: AGPL-3.0-only
// Fuzz target: timelines of other programs (core/Interchange.h), built once per format (FUZZ_FORMAT = Edl, Fcp7 or
// Otio). A file is read and imported like the app does it: an empty project takes the file's format, the media
// that exist go into the media pool (some are missing), addToProject makes the new timeline; then it is written
// out again in every format and saved as a project.
//
// Rules checked: the imported timeline is a valid one (clips sorted, at least one frame long, not overlapping on a
// track, inside the timeline), and the project with it saves and loads.
#include "FuzzCommon.h"
#include "FuzzJson.h"

#include "core/Interchange.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include <QHash>
#include <QTemporaryDir>

#ifndef FUZZ_FORMAT
#error "FUZZ_FORMAT (Edl, Fcp7, Otio) is set by tests/fuzz/CMakeLists.txt"
#endif

namespace {

constexpr Interchange::Format kFormat = Interchange::Format::FUZZ_FORMAT;

QString& projectPath()
{
    static QString path;
    return path;
}

void checkTimeline(const Timeline& tl)
{
    for (const TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (int i = 0; i < t.clips.size(); ++i) {
                const Clip& c = t.clips[i];
                FUZZ_ASSERT(c.start >= 0);
                FUZZ_ASSERT(c.in >= 0);
                FUZZ_ASSERT(c.out >= c.in);
                FUZZ_ASSERT(c.speed > 0);
                if (i > 0) FUZZ_ASSERT(t.clips[i - 1].end() <= c.start);
            }
}

} // namespace

#ifdef FUZZ_JSON_INPUT // OTIO
FUZZ_JSON_MUTATOR
#endif

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Fuzz::init("interchange");
    static QTemporaryDir dir;
    if (!dir.isValid()) std::abort();
    projectPath() = dir.filePath("project.schneidi");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char*>(data), qsizetype(size));
    // Choices from the content, so that plain files are the seeds: the project's frame rate (EDL counts in it),
    // whether the project is empty (takes the file's format)
    const size_t h = qHash(bytes);
    const FrameRate rate = kFrameRates[h % std::size(kFrameRates)];
    Interchange::Sequence seq;
    try {
        seq = Interchange::read(bytes, kFormat, rate);
    } catch (const Interchange::Error&) {
        return 0;
    }

    ProjectData pd;
    pd.format.rate = rate;
    Project p;
    p.load(pd);
    if (kFormat != Interchange::Format::Edl && (h & 0x100)) { // MainWindow::importTimeline
        ProjectFormat f = p.format();
        f.rate = seq.format.rate;
        if (seq.hasSize) {
            f.width = seq.format.width;
            f.height = seq.format.height;
        }
        p.setFormat(f);
    }
    // Media: what the file names; every third file is missing, the others get made-up properties
    QHash<QString, QString> local;
    for (const QString& m : Interchange::mediaFiles(seq)) {
        const size_t mh = qHash(m);
        if (mh % 3 == 0 || m.isEmpty()) continue;
        MediaInfo info;
        info.path = m;
        info.name = m;
        info.isImage = mh % 11 == 0;
        info.length = info.isImage ? 0 : int(mh % 100000);
        info.hasVideo = mh & 1;
        info.hasAudio = !info.hasVideo || (mh & 2);
        for (int s = 0; s < int((mh >> 4) % 4); ++s) info.audioStreams << AudioStreamInfo{2, {}};
        p.addMedia(info);
        local.insert(m, m);
    }
    const auto timecode = [](const QString& path) {
        return qHash(path) & 1 ? QString("01:00:00:00") : QString();
    };
    const Interchange::Imported imp = Interchange::addToProject(p, seq, local, {}, timecode);
    const ::Sequence* s = p.sequence(imp.sequenceId);
    FUZZ_ASSERT(s);
    checkTimeline(s->timeline);

    for (const Interchange::Format f : {Interchange::Format::Edl, Interchange::Format::Fcp7, Interchange::Format::Otio}) {
        Interchange::Sequence out = Interchange::fromTimeline(p, imp.sequenceId);
        (void)Interchange::write(out, f);
    }
    ProjectData loaded;
    QString error;
    FUZZ_ASSERT(ProjectFile::fromJson(ProjectFile::toJson(p.data(), projectPath()), projectPath(), &loaded, &error));
    return 0;
}
