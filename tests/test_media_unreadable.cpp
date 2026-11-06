// SPDX-License-Identifier: AGPL-3.0-only
// Files that are no media or that no decoder here can turn into pictures (found by feeding schneidi-cli damaged and
// odd files):
// - a named pipe (FIFO) called clip.mp4: opening it waited forever for a writer (probe, import, frames hung), a
//   symbolic link to /dev/zero never ended
// - a still picture whose header states an absurd size (one flipped bit) or a size beyond every decoder here
//   (16384x16384 PNG): accepted by probe and import, then black in a render and "could not decode" in `frames`
// - a sound shorter than half a frame (a 1 ms click): rounded to 0 frames, so "cannot read media"
// - a WAV of 0 channels: refused by probe, but `index` made an entry for it
// - why a file is unreadable, for the message (folder, pipe, empty, no permission, subtitles only, 64-bit samples …)

#include "check.h"

#include "engine/Engine.h"
#include "engine/MediaIndex.h"
#include "engine/MediaProbe.h"
#include "engine/audio/Reader.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/crc.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <chrono>
#include <future>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// A PNG of one chunk list: the IHDR states width x height, the picture data is a few bytes (enough for the header
// reader, not for the picture)
QByteArray pngClaiming(quint32 width, quint32 height)
{
    const auto chunk = [](const char* type, const QByteArray& body) {
        QByteArray out(4, '\0');
        qToBigEndian<quint32>(quint32(body.size()), out.data());
        const QByteArray typed = QByteArray(type, 4) + body;
        out += typed;
        const quint32 crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0xFFFFFFFFu,
                                   reinterpret_cast<const uint8_t*>(typed.constData()), size_t(typed.size()))
                            ^ 0xFFFFFFFFu;
        QByteArray c(4, '\0');
        qToBigEndian<quint32>(crc, c.data());
        return out + c;
    };
    QByteArray ihdr(13, '\0');
    qToBigEndian<quint32>(width, ihdr.data());
    qToBigEndian<quint32>(height, ihdr.data() + 4);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // RGB
    const QByteArray data = qCompress(QByteArray(64, '\0')).mid(4); // zlib stream without Qt's length prefix
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", data) + chunk("IEND", {});
}

bool write(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_unreadable");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();

    QTemporaryDir dir;
    const auto in = [&](const char* name) { return dir.filePath(name); };
    const FrameRate rate{25, 1};
    ProjectFormat format;
    format.rate = rate;

#ifdef Q_OS_UNIX
    // Named pipe: every reader refuses it at once instead of waiting for a writer
    {
        const QString fifo = in("pipe.mp4");
        if (CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0)) {
            auto probing = std::async(std::launch::async, [&] {
                const bool own = MediaProbe::read(fifo, rate).has_value();
                const MediaInfo m = Engine::probe(format, fifo);
                const bool video = Video::Reader::open(fifo) != nullptr;
                const bool audio = Audio::Reader::open(fifo) != nullptr;
                return !own && !m.hasVideo && !m.hasAudio && !video && !audio;
            });
            const bool returned = probing.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
            CHECK(returned);
            // Unblock each waiting open (a writer that closes again = end of the stream) until all readers are done
            while (probing.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
                const int fd = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0) ::close(fd);
            }
            CHECK(probing.get());
            CHECK(MediaProbe::whyUnreadable(fifo).contains("pipe"));
        }
        // A link to a device that never ends
        const QString zero = in("zero.mp4");
        if (QFileInfo::exists("/dev/zero") && CHECK(QFile::link("/dev/zero", zero))) {
            CHECK(!Engine::probe(format, zero).hasVideo && !Video::Reader::open(zero));
            CHECK(MediaProbe::whyUnreadable(zero).contains("device"));
        }
    }
#endif

    // Still pictures no decoder here can turn into pixels: unreadable at the probe already
    {
        CHECK(write(in("small.png"), pngClaiming(64, 32)));
        CHECK(write(in("huge.png"), pngClaiming(16384, 16384)));
        CHECK(write(in("absurd.png"), pngClaiming(0xFFFFFF7Fu, 0x7FFFFFFFu)));
        const std::optional<MediaInfo> small = MediaProbe::read(in("small.png"), rate); // header-only test is still fine
        CHECK(small && small->isImage);
        CHECK(!MediaProbe::read(in("huge.png"), rate));
        CHECK(!Engine::probe(format, in("huge.png")).hasVideo);
        CHECK(MediaProbe::whyUnreadable(in("huge.png")).contains("too large"));
        CHECK(!MediaProbe::read(in("absurd.png"), rate));
        // The same picture without its extension goes through FFmpeg's picture demuxer first: refused there as well
        CHECK(QFile::copy(in("huge.png"), in("huge_picture")));
        CHECK(!MediaProbe::read(in("huge_picture"), rate));
    }

    // A 1 ms sound: one frame long, not "no length"
    {
        const QString click = Check::makeMedia(
            in("click.wav"), {"-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000", "-t", "0.001", "-c:a", "pcm_s16le"});
        if (CHECK(!click.isEmpty())) {
            const MediaInfo m = Engine::probe(format, in("click.wav"));
            CHECK_EQ(m.length, 1);
            CHECK(m.hasAudio && !m.hasVideo);
        }
    }

    // The index refuses what probe refuses: a WAV header of 0 channels opened in FFmpeg and got an (empty) entry
    {
        const auto le32 = [](quint32 v) { QByteArray b(4, '\0'); qToLittleEndian<quint32>(v, b.data()); return b; };
        const auto le16 = [](quint16 v) { QByteArray b(2, '\0'); qToLittleEndian<quint16>(v, b.data()); return b; };
        const QByteArray body(9600, '\0');
        const QByteArray wav = QByteArray("RIFF") + le32(36 + quint32(body.size())) + "WAVEfmt " + le32(16) + le16(1)
                               + le16(0) + le32(48000) + le32(0) + le16(0) + le16(16) + "data"
                               + le32(quint32(body.size())) + body;
        CHECK(write(in("nochannels.wav"), wav));
        CHECK(!Engine::probe(format, in("nochannels.wav")).hasAudio);
        QString error;
        CHECK(!MediaIndex::build(in("nochannels.wav"), {}, &error));
        CHECK(error.contains("cannot read media"));
    }

    // Why unreadable files are unreadable (the CLI's "cannot read media: <path> (<why>)")
    {
        CHECK(QDir(dir.path()).mkdir("folder.mp4"));
        CHECK(MediaProbe::whyUnreadable(in("folder.mp4")).contains("folder"));
        CHECK(MediaProbe::whyUnreadable(in("missing.mp4")).contains("does not exist"));
        CHECK(write(in("empty.mp4"), {}));
        CHECK(MediaProbe::whyUnreadable(in("empty.mp4")).contains("empty"));
        CHECK(write(in("text.mkv"), "not a video\n"));
        CHECK(MediaProbe::whyUnreadable(in("text.mkv")).contains("no known media format"));
        CHECK(write(in("cues.srt"), "1\n00:00:00,000 --> 00:00:02,000\nhello\n"));
        if (!Check::makeMedia(in("subtitles.mkv"), {"-i", in("cues.srt"), "-c:s", "srt"}).isEmpty()) {
            CHECK(!Engine::probe(format, in("subtitles.mkv")).hasAudio);
            const QString why = MediaProbe::whyUnreadable(in("subtitles.mkv"));
            if (!CHECK(why.contains("no picture and no sound"))) std::printf("       why: %s\n", qPrintable(why));
        }
        if (!Check::makeMedia(in("f64.wav"), {"-f", "lavfi", "-i", "sine=duration=1", "-c:a", "pcm_f64le"}).isEmpty()) {
            const QString why = MediaProbe::whyUnreadable(in("f64.wav"));
            if (!CHECK(why.contains("sample format"))) std::printf("       why: %s\n", qPrintable(why));
        }
#ifdef Q_OS_UNIX
        if (geteuid() != 0 && CHECK(write(in("locked.mp4"), "x"))) {
            QFile::setPermissions(in("locked.mp4"), QFileDevice::Permissions{});
            CHECK(MediaProbe::whyUnreadable(in("locked.mp4")).contains("permission"));
        }
#endif
    }
    return Check::result();
}
