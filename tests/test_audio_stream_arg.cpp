// SPDX-License-Identifier: AGPL-3.0-only
// The 'audio_stream' argument of the CLI analyses (highlights, short, peaks, loudness, beats, overview, events):
// not given -> the first stream that plays (a recording whose first stream is empty or of a codec this build lacks
// still has its sound analysed); given -> exactly that one, an error when it can't be played.

#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"

#include <QCoreApplication>
#include <QJsonObject>

namespace {

MediaInfo fileWith(std::initializer_list<bool> playable)
{
    MediaInfo m;
    m.path = "/media/rec.mkv";
    m.hasAudio = true;
    for (bool p : playable) {
        AudioStreamInfo s;
        s.playable = p;
        m.audioStreams << s;
    }
    return m;
}

// The error code of a call that fails, "" when it does not
template <class F> QString failCode(F f)
{
    try {
        f();
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("audio-stream-arg");
    using Cli::detail::audioStreamArg;

    // Not given: the first that plays
    CHECK_EQ(audioStreamArg(fileWith({true, true}), {}), 0);
    CHECK_EQ(audioStreamArg(fileWith({false, true}), {}), 1);
    CHECK_EQ(audioStreamArg(fileWith({false, false, true}), {}), 2);
    // An older project file without the stream list: one stream, playable
    MediaInfo old;
    old.hasAudio = true;
    CHECK_EQ(audioStreamArg(old, {}), 0);
    // None plays: NO_AUDIO
    CHECK_EQ(failCode([] { audioStreamArg(fileWith({false, false}), {}); }), QString("NO_AUDIO"));

    // Given: exactly that one
    CHECK_EQ(audioStreamArg(fileWith({true, true}), QJsonObject{{"audio_stream", 1}}), 1);
    CHECK_EQ(failCode([] { audioStreamArg(fileWith({false, true}), QJsonObject{{"audio_stream", 0}}); }),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(failCode([] { audioStreamArg(fileWith({true}), QJsonObject{{"audio_stream", 3}}); }),
             QString("BAD_ARGUMENT"));
    return Check::result();
}
