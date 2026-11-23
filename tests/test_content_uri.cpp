// SPDX-License-Identifier: AGPL-3.0-only
// Android's content:// addresses (engine/ContentUri): FFmpeg reads them as /proc/self/fd/<n> of a descriptor the
// ContentResolver opened. Here (no Android): the same way through a descriptor of a file opened by QFile - the demuxer
// finds the stream; other paths pass unchanged; a content address without Android is refused, not crashed.

#include "check.h"

#include "engine/ContentUri.h"
#include "engine/OpenMedia.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

namespace {

// A second of 8 kHz mono 16-bit silence as a WAV file
QByteArray wav()
{
    const quint32 rate = 8000, samples = 8000, bytes = samples * 2;
    QByteArray d;
    auto u32 = [&d](quint32 v) { v = qToLittleEndian(v); d.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&d](quint16 v) { v = qToLittleEndian(v); d.append(reinterpret_cast<const char*>(&v), 2); };
    d += "RIFF";
    u32(36 + bytes);
    d += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    d += "data";
    u32(bytes);
    d.append(QByteArray(int(bytes), '\0'));
    return d;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("content_uri");
    QTemporaryDir dir;
    const QString path = dir.filePath("silence.wav");
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(wav()) > 0);
    }
    CHECK(!ContentUri::is(path));
    CHECK(ContentUri::is("content://com.android.providers.media.documents/document/video%3A42"));
    CHECK(ContentUri::forImport({path}) == QStringList{path});
#ifndef Q_OS_ANDROID
    CHECK(ContentUri::openFd("content://x/y") < 0); // no provider here: refused
    AVFormatContext* none = nullptr;
    CHECK(OpenMedia::input(&none, "content://x/y") < 0);
#endif
#ifdef Q_OS_UNIX
    // Read through a descriptor, as a content address is on Android
    QFile f(path);
    if (CHECK(f.open(QIODevice::ReadOnly))) {
        const QByteArray viaFd = "/proc/self/fd/" + QByteArray::number(f.handle());
        AVFormatContext* fmt = nullptr;
        const int r = OpenMedia::input(&fmt, viaFd.constData());
        if (CHECK(r >= 0 && fmt)) {
            CHECK(avformat_find_stream_info(fmt, nullptr) >= 0);
            CHECK(fmt->nb_streams == 1 && fmt->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO);
            avformat_close_input(&fmt);
        }
    }
#endif
    return Check::result();
}
