// SPDX-License-Identifier: AGPL-3.0-only
// Files whose picture the preview and the thumbnails decode while the probe (import, drop on the timeline) found "no
// picture and no sound" (a user on Windows: a clip with a thumbnail but no waveform could not be imported):
// - one sound stream the probe could not settle refused the whole file: a stream without a decoder here (an unknown
//   codec, a codec the slim package's FFmpeg lacks), or one whose values the stream analysis did not find (an MPEG-TS
//   whose sound starts after the analysed first seconds: 0 channels). The reader only needs the picture stream.
// - the app kept every probe result of a file not in the Media Pool by path for the whole session (drop preview on the
//   timeline, Viewer of a Media Storage file): one probed while being written or held by another program stayed
//   "no picture, no sound", its drop refused (ProbeCache: usable results only, dropped when the file changes)
// - why such a file is unreadable says when it may only be for now (written a moment ago: still being written, copied
//   or scanned)

#include "check.h"

#include "engine/Engine.h"
#include "engine/MediaProbe.h"
#include "engine/ProbeCache.h"
#include "engine/video/Thumbnails.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

// The file with every `from` replaced by `to` (same length): a codec name the demuxer does not know
bool patched(const QString& source, const QString& target, const QByteArray& from, const QByteArray& to)
{
    QFile in(source), out(target);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) return false;
    QByteArray data = in.readAll();
    if (!data.contains(from) || from.size() != to.size()) return false;
    data.replace(from, to);
    return out.write(data) == data.size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_probe_robust");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();

    QTemporaryDir dir;
    const auto in = [&](const char* name) { return dir.filePath(name); };
    const QStringList x264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    const QStringList v{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=12"};
    const QStringList a{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12"};
    const FrameRate rate{25, 1};
    const QSize box(160, 90);

    // 1. Sound with a codec no decoder here knows (Matroska codec id "A_QQQ"): the picture alone
    if (CHECK(!Check::makeMedia(in("aac.mkv"), v + a + x264 + QStringList{"-c:a", "aac", "-shortest"}).isEmpty())
        && CHECK(patched(in("aac.mkv"), in("unknown_sound.mkv"), "A_AAC", "A_QQQ"))) {
        CHECK(!Video::stillAt(in("unknown_sound.mkv"), 1.0, box).isNull()); // the thumbnail is there
        const std::optional<MediaInfo> m = MediaProbe::read(in("unknown_sound.mkv"), rate);
        if (CHECK(m.has_value())) {
            CHECK(m->hasVideo);
            CHECK(!m->hasAudio);
            CHECK(m->length >= 299 && m->length <= 301);
        }
    }

    // 2. A known sound stream next to an unknown one: both picture and the known sound; the stream list keeps the
    // file's order (the sound reader counts streams by their place among the file's audio streams)
    if (CHECK(!Check::makeMedia(in("two.mkv"), v + a + a + x264
                                                   + QStringList{"-map", "0", "-map", "1", "-map", "2", "-c:a:0", "aac",
                                                                 "-c:a:1", "ac3", "-shortest"})
                   .isEmpty())
        && CHECK(patched(in("two.mkv"), in("one_unknown.mkv"), "A_AC3", "A_QQQ"))) {
        const std::optional<MediaInfo> m = MediaProbe::read(in("one_unknown.mkv"), rate);
        if (CHECK(m.has_value())) {
            CHECK(m->hasVideo && m->hasAudio);
            CHECK_EQ(m->audioStreams.size(), 2);
        }
    }

    // 2b. Only a cover picture beside sound that cannot be played: nothing to import (the cover is a picture only with
    // sound)
    if (CHECK(!Check::makeMedia(in("cover.png"), QStringList{"-f", "lavfi", "-i", "testsrc2=size=64x64", "-frames:v", "1"})
                   .isEmpty())
        && CHECK(!Check::makeMedia(in("song.mkv"), a + QStringList{"-c:a", "aac", "-attach", in("cover.png"),
                                                                   "-metadata:s:t", "mimetype=image/png"})
                      .isEmpty())
        && CHECK(patched(in("song.mkv"), in("cover_unknown_sound.mkv"), "A_AAC", "A_QQQ"))) {
        CHECK(MediaProbe::read(in("song.mkv"), rate).has_value());
        CHECK(!MediaProbe::read(in("cover_unknown_sound.mkv"), rate).has_value());
    }

    // 3. MPEG-TS whose sound starts 12 s in (a broadcast recording, a camera that adds sound late): the stream
    // analysis (5 s) ends before its first packet, its channels stay unknown. Analysed further, the sound is found.
    if (CHECK(!Check::makeMedia(in("late_sound.ts"),
                                QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=20", "-itsoffset",
                                            "12", "-f", "lavfi", "-i", "sine=duration=5", "-map", "0", "-map", "1"}
                                    + x264 + QStringList{"-c:a", "aac"})
                   .isEmpty())) {
        CHECK(!Video::stillAt(in("late_sound.ts"), 1.0, box).isNull());
        const std::optional<MediaInfo> m = MediaProbe::read(in("late_sound.ts"), rate);
        if (CHECK(m.has_value())) {
            CHECK(m->hasVideo);
            CHECK(m->hasAudio);
            CHECK(m->length >= 490 && m->length <= 510);
        }
    }

    // 4. Why a file is unreadable: one written a moment ago may still be being written (a recording, a download, a
    // copy) or held by another program (a virus scan) - worth trying again; an old broken one is just broken
    {
        QFile f(in("fresh.mp4"));
        CHECK(f.open(QIODevice::WriteOnly) && f.write(QByteArray(4096, '\0')) == 4096);
        f.close();
        CHECK(MediaProbe::busy(in("fresh.mp4")) == MediaProbe::Busy::Written);
        QFile old(in("fresh.mp4"));
        CHECK(old.open(QIODevice::ReadWrite)
              && old.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        old.close();
        CHECK(MediaProbe::busy(in("fresh.mp4")) == MediaProbe::Busy::No);
        CHECK(MediaProbe::busy(in("missing.mp4")) == MediaProbe::Busy::No);
    }

    // 5. The probe cache: usable results while the file stays the same; a damaged file briefly (no re-probe on every
    // drag-enter), one written moments ago not at all (it may read a moment later); a result probed while the file
    // changed is dropped (the stamp is taken before probing)
    {
        const QString path = in("cached.mp4");
        CHECK(QFile::copy(in("aac.mkv"), path));
        ProbeCache cache;
        ProbeResult r = ProbeCache::probe(path, rate);
        CHECK(r.usable() && !r.partial);
        cache.insert(r);
        CHECK(cache.find(path).has_value());
        {
            QFile f(path);
            CHECK(f.open(QIODevice::Append) && f.write("more") == 4); // overwritten: another size
        }
        CHECK(!cache.find(path).has_value());

        // Changed between the stamp and the insert: not kept
        const QString moving = in("moving.mp4");
        CHECK(QFile::copy(in("aac.mkv"), moving));
        const ProbeResult early = ProbeCache::probe(moving, rate);
        {
            QFile f(moving);
            CHECK(f.open(QIODevice::Append) && f.write("x") == 1);
        }
        cache.insert(early);
        CHECK(!cache.find(moving).has_value());

        // Damaged, written moments ago: not kept; an hour old: kept (briefly), with its reason
        const QString broken = in("broken.mp4");
        {
            QFile f(broken);
            CHECK(f.open(QIODevice::WriteOnly) && f.write(QByteArray(4096, 'x')) == 4096);
        }
        ProbeResult fresh = ProbeCache::probe(broken, rate);
        CHECK(!fresh.usable() && fresh.busy == MediaProbe::Busy::Written && !fresh.why.isEmpty());
        cache.insert(fresh);
        CHECK(!cache.find(broken).has_value());
        {
            QFile f(broken);
            CHECK(f.open(QIODevice::ReadWrite)
                  && f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        }
        const ProbeResult old = ProbeCache::probe(broken, rate);
        CHECK(!old.usable() && old.busy == MediaProbe::Busy::No);
        cache.insert(old);
        const std::optional<ProbeResult> kept = cache.find(broken);
        CHECK(kept && !kept->usable() && kept->why == old.why);

        // A quick look that skipped the long analysis is not kept
        ProbeResult partial = r;
        partial.partial = true;
        cache.clear();
        cache.insert(partial);
        CHECK(!cache.find(path).has_value());
    }

    // 6. A file without permission is no "held by another program"
#ifdef Q_OS_UNIX
    {
        const QString closed = in("closed.mp4");
        CHECK(QFile::copy(in("aac.mkv"), closed));
        QFile::setPermissions(closed, QFileDevice::WriteOwner);
        if (QFileInfo(closed).isReadable()) {
            std::printf("running as root: permissions not tested\n");
        } else {
            CHECK(MediaProbe::busy(closed) != MediaProbe::Busy::Locked);
            CHECK(!MediaProbe::whyUnreadable(closed).contains("another program"));
        }
    }
#endif

    return Check::result();
}
