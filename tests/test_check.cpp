// Check before export (core/ExportCheck.h, CLI/MCP `check`): gaps without picture (also from hidden tracks and
// disabled clips), offline media, subtitles too short/too fast/past the end; sound: no sound, clipping, true peak with
// and without the limiter advice, loudness off the target, silence at the end/start/inside. Vertical videos: titles
// and subtitles where TikTok/Reels/Shorts cover them, the hook. Mix: which track is music, a track with events buried
// under the music (also not when the music is ducked, not for flat noise). Stutter (core/Stutter.h): jumps in the time
// stamps, stretches, clean parts, the clips that use them. Then the commands: the packet scan of real files (MP4 with
// B-frames, MKV, a clean file, disk cache), `probe` gaps, `highlights` stutter/clean, `check` on a small project and
// its render, on a vertical project (buried game sound, title at the bottom, still start) against a normal 16:9 one
// (no new findings), and a clip over a stutter.
#include "cli/Commands.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/Stutter.h"
#include "core/Subtitles.h"
#include "engine/FrameGaps.h"

#include "check.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>

namespace {

Clip clip(int id, int start, int length, const QString& media = {})
{
    Clip c;
    c.id = id;
    c.kind = media.isEmpty() ? ClipKind::Title : ClipKind::Media;
    c.mediaPath = media;
    c.start = start;
    c.out = length - 1;
    return c;
}

QStringList checks(const QVector<ExportCheck::Issue>& issues)
{
    QStringList out;
    for (const ExportCheck::Issue& i : issues) out << i.check;
    return out;
}

const ExportCheck::Issue* find(const QVector<ExportCheck::Issue>& issues, const QString& check)
{
    for (const ExportCheck::Issue& i : issues)
        if (i.check == check) return &i;
    return nullptr;
}

float lin(double db) { return float(std::pow(10.0, db / 20)); }
// Power of a 400 ms loudness block of this many LUFS (LoudnessMeter::blocks)
double power(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10); }

QVector<int> clipsOf(const QVector<ExportCheck::Issue>& issues)
{
    QVector<int> out;
    for (const ExportCheck::Issue& i : issues) out << i.clip;
    return out;
}

Clip title(int id, int start, int length, const QString& text, double x, double y)
{
    Clip c = clip(id, start, length);
    c.title.text = text;
    c.title.posX = x;
    c.title.posY = y;
    return c;
}

QStringList checksOf(const QJsonObject& result)
{
    QStringList out;
    for (const QJsonValue& v : result["issues"].toArray()) out << v["check"].toString();
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("check");
    const ProjectFormat f; // 1920x1080, 25 fps

    // --- Picture: a forgotten frame between clips, a hidden track and a disabled clip do not count as picture
    {
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << clip(1, 0, 50) << clip(2, 51, 49);
        tl.video[1].hidden = true;
        tl.video[1].clips << clip(3, 50, 1); // would cover the gap, but the track is hidden
        auto issues = ExportCheck::timeline(tl, 0, 100, f);
        CHECK_EQ(checks(issues), QStringList({"black"}));
        if (const auto* gap = find(issues, "black")) {
            CHECK_EQ(gap->from, 50);
            CHECK_EQ(gap->to, 51);
            CHECK(gap->severity == ExportCheck::Severity::Warning);
        }
        tl.video[1].hidden = false;
        CHECK(ExportCheck::timeline(tl, 0, 100, f).isEmpty());
        tl.video[1].clips[0].enabled = false;
        CHECK_EQ(checks(ExportCheck::timeline(tl, 0, 100, f)), QStringList({"black"}));
        // Black at the end of the range (sound goes on) and nothing outside the checked range
        CHECK_EQ(checks(ExportCheck::timeline(tl, 51, 120, f)), QStringList({"black"}));
        CHECK(ExportCheck::timeline(tl, 0, 50, f).isEmpty());
        // A timeline without any picture
        Timeline sound;
        sound.audio.resize(1);
        sound.audio[0].clips << clip(5, 0, 100, "/x/a.wav");
        CHECK(checks(ExportCheck::timeline(sound, 0, 100, f)).contains("black"));
    }

    // --- Offline media: an error, once per file, only clips in the range
    {
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 50, "/does/not/exist.mp4") << clip(2, 50, 50, "/does/not/exist.mp4");
        tl.audio[0].clips << clip(3, 0, 50, "/does/not/exist.mp4") << clip(4, 200, 10, "/other/missing.wav");
        const auto issues = ExportCheck::timeline(tl, 0, 100, f);
        CHECK_EQ(checks(issues), QStringList({"offline"}));
        CHECK_EQ(ExportCheck::errors(issues), 1);
    }

    // --- Subtitles: too short, too fast, past the end; disabled tracks are not exported
    {
        Timeline tl;
        tl.video.resize(1);
        tl.video[0].clips << clip(1, 0, 250);
        tl.subtitles.resize(1);
        tl.subtitles[0].cues << SubtitleCue{10, 0, 10, "Hi"}                                 // 0.4 s
                             << SubtitleCue{11, 25, 50, QString(40, 'x')}                     // 40 chars in 1 s
                             << SubtitleCue{12, 100, 150, "Fine to read"}
                             << SubtitleCue{13, 225, 300, "Cut off"};                          // past frame 250
        const auto issues = ExportCheck::timeline(tl, 0, 250, f);
        CHECK_EQ(checks(issues), QStringList({"subtitle_short", "subtitle_fast", "subtitle_end"}));
        tl.subtitles[0].enabled = false;
        CHECK(ExportCheck::timeline(tl, 0, 250, f).isEmpty());
    }

    // --- Sound
    {
        const std::vector<float> normal(250, lin(-12));
        CHECK(ExportCheck::sound(normal, 0, -14.2, -2.0, f).isEmpty());
        // Off the target: too quiet / too loud, the tolerance holds
        CHECK_EQ(checks(ExportCheck::sound(normal, 0, -20, -6, f)), QStringList({"loudness"}));
        CHECK(ExportCheck::sound(normal, 0, -15.5, -2, f).isEmpty());
        ExportCheck::Options broadcast;
        broadcast.targetLufs = -23;
        CHECK(ExportCheck::sound(normal, 0, -23.4, -9, f, broadcast).isEmpty());
        // True peak: above 0 = error, above -1 = warning; advice for the limiter only when it is off
        auto issues = ExportCheck::sound(normal, 0, -14, 0.8, f);
        CHECK_EQ(checks(issues), QStringList({"clipping"}));
        CHECK_EQ(ExportCheck::errors(issues), 1);
        issues = ExportCheck::sound(normal, 0, -14, -0.5, f);
        CHECK_EQ(checks(issues), QStringList({"true_peak"}));
        const QString advice = issues[0].message;
        issues = ExportCheck::sound(normal, 0, -14, -0.5, f, {}, true);
        CHECK(issues[0].message.size() < advice.size());
        // The fix (Fix button, `check`'s "fix"). Too quiet with peaks near 0 (a colleague's Valorant clip: -17.1 LUFS,
        // -0.1 dBTP): fader up by the difference and the limiter at -1; peaks only: the limiter; too loud with the
        // limiter already on: the fader down only; nothing off: no fix
        Timeline mix;
        auto fix = ExportCheck::soundFix(ExportCheck::sound(normal, 0, -17.1, -0.1, f), -17.1, mix);
        CHECK(fix.limiter && fix.ceiling == -1 && fix.level);
        CHECK(std::abs(fix.volumeDb - 3.1) < 1e-9 && std::abs(fix.change - 3.1) < 1e-9);
        fix = ExportCheck::soundFix(ExportCheck::sound(normal, 0, -14, -0.5, f), -14, mix);
        CHECK(fix.limiter && !fix.level);
        mix.masterLimiter = true;
        mix.masterVolumeDb = 2;
        fix = ExportCheck::soundFix(ExportCheck::sound(normal, 0, -10, -1.5, f, {}, true), -10, mix);
        CHECK(!fix.limiter && fix.level && std::abs(fix.volumeDb + 2) < 1e-9 && std::abs(fix.change + 4) < 1e-9);
        ExportCheck::apply(fix, mix);
        CHECK(mix.masterVolumeDb == -2 && mix.masterLimiter);
        CHECK(!ExportCheck::soundFix(ExportCheck::sound(normal, 0, -14.2, -2.0, f), -14.2, Timeline{}).any());
        // No sound at all
        CHECK_EQ(checks(ExportCheck::sound(std::vector<float>(250, 0.f), 0, -200, -200, f)), QStringList({"no_sound"}));
        CHECK_EQ(checks(ExportCheck::sound({}, 0, -200, -200, f)), QStringList({"no_sound"}));
        // Silence: 3 s at the end = warning, 3 s at the start = info, 6 s inside = info, 1 s inside = nothing
        std::vector<float> peaks(1000, lin(-12));
        for (int i = 0; i < 75; ++i) peaks[size_t(i)] = 0;               // start
        for (int i = 300; i < 450; ++i) peaks[size_t(i)] = 0;            // 6 s inside
        for (int i = 600; i < 625; ++i) peaks[size_t(i)] = lin(-70);     // 1 s inside
        for (int i = 925; i < 1000; ++i) peaks[size_t(i)] = 0;           // end
        issues = ExportCheck::sound(peaks, 100, -14, -3, f);
        CHECK_EQ(checks(issues), QStringList({"silence_start", "silence", "silence_end"}));
        if (const auto* end = find(issues, "silence_end")) {
            CHECK_EQ(end->from, 1025);
            CHECK_EQ(end->to, 1100);
            CHECK(end->severity == ExportCheck::Severity::Warning);
        }
        CHECK(find(issues, "silence_start")->severity == ExportCheck::Severity::Info);
    }

    // --- Stutter: 60 fps with frames 61-89 missing (a jump of 0.5 s at 1 s), 2 at 2 s (3 frame durations) and one
    // at 3 s (exactly 2 frame durations: no jump)
    {
        std::vector<double> t;
        for (int i = 0; i < 240; ++i)
            if (!(i > 60 && i < 90) && i != 121 && i != 122 && i != 181) t.push_back(i / 60.0);
        std::reverse(t.begin(), t.end()); // any order
        const Stutter::Scan s = Stutter::fromTimestamps(t);
        CHECK(std::abs(s.frameDuration - 1 / 60.0) < 1e-9);
        CHECK_EQ(s.frames, int(t.size()));
        if (CHECK_EQ(s.jumps.size(), 2)) {
            CHECK(std::abs(s.jumps[0].at - 1.0) < 1e-9);
            CHECK(std::abs(s.jumps[0].length - 0.5) < 1e-9);
            CHECK_EQ(s.jumps[0].missing, 29);
            CHECK(std::abs(s.jumps[1].at - 2.0) < 1e-9);
            CHECK_EQ(s.jumps[1].missing, 2);
        }
        // Inside a range: not when the range ends on the last good frame or starts on the next one
        CHECK(Stutter::inRange(s, 0, 1.0).isEmpty());
        CHECK_EQ(Stutter::inRange(s, 0, 1.2).size(), 1);
        CHECK(Stutter::inRange(s, 1.5, 2.0).isEmpty());
        CHECK_EQ(Stutter::inRange(s, 1.2, 2.5).size(), 2);
        // Jumps 0.5 s apart are one stretch, with a shorter join two
        const auto both = Stutter::stretches(s.jumps);
        if (CHECK_EQ(both.size(), 1)) {
            CHECK_EQ(both[0].jumps, 2);
            CHECK_EQ(both[0].missing, 31);
            CHECK(std::abs(both[0].to - 2.05) < 1e-9);
            CHECK(std::abs(both[0].longest - 0.5) < 1e-9);
        }
        CHECK_EQ(Stutter::stretches(s.jumps, 0.4).size(), 2);
        // Clean parts of 0-3.5 s: up to the end of the last good frame before the stutter, from 0.1 s after it
        const auto pieces = Stutter::cleanPieces(s, 0, 3.5);
        if (CHECK_EQ(pieces.size(), 2)) {
            CHECK(std::abs(pieces[0].second - (1 + 1 / 60.0)) < 1e-9);
            CHECK(std::abs(pieces[1].first - 2.15) < 1e-9);
            CHECK(std::abs(pieces[1].second - 3.5) < 1e-9);
        }
        CHECK_EQ(Stutter::cleanPieces(s, 0, 1.9).size(), 1); // the 0.3 s after the jump are too short
        CHECK(Stutter::fromTimestamps({0.0, 0.04}).jumps.isEmpty());

        // Clips over it: the timeline frames where the picture stands still, with the clip; not a clip that only
        // uses clean source, a frozen clip or a hidden track
        Stutter::Scan one = s;
        one.jumps.removeLast();
        Timeline tl;
        tl.video.resize(2);
        Clip over = clip(1, 100, 50, "/x/gap.mp4"); // source 0-2 s (25 fps)
        Clip clean = clip(2, 200, 25, "/x/gap.mp4");
        clean.in = 40; // source 1.6-2.6 s
        clean.out = 64;
        Clip frozen = over;
        frozen.id = 3;
        frozen.start = 300;
        frozen.freeze = true;
        tl.video[0].clips << over << clean << frozen;
        tl.video[1].hidden = true;
        tl.video[1].clips << clip(4, 0, 50, "/x/gap.mp4");
        const auto scanOf = [&](const QString& p) {
            return p == "/x/gap.mp4" ? std::optional<Stutter::Scan>(one) : std::nullopt;
        };
        const auto length = [](const QString&) { return 100; };
        auto issues = ExportCheck::stutter(tl, 0, 400, f, scanOf, length);
        if (CHECK_EQ(checks(issues), QStringList({"stutter"}))) {
            CHECK_EQ(issues[0].clip, 1);
            CHECK_EQ(issues[0].from, 125); // 1 s + one frame of 60 fps = frame 25.4 of the clip
            CHECK_EQ(issues[0].to, 138);   // the next frame at 1.5 s = 37.5
            CHECK(issues[0].message.contains("29"));
        }
        CHECK(ExportCheck::stutter(tl, 0, 120, f, scanOf, length).isEmpty()); // checked range before the stutter
        // Reversed: the same source frames show elsewhere on the timeline
        tl.video[0].clips[0].reverse = true; // file frames 99..50 = 3.96..2 s
        CHECK(ExportCheck::stutter(tl, 0, 160, f, scanOf, length).isEmpty());
        tl.video[0].clips[0].in = 50; // file frames 49..0 = 1.96..0 s
        tl.video[0].clips[0].out = 99;
        issues = ExportCheck::stutter(tl, 0, 160, f, scanOf, length);
        if (CHECK_EQ(checks(issues), QStringList({"stutter"}))) {
            CHECK_EQ(issues[0].from, 111); // 1.5 s = file frame 37.5 shows 11.5 frames after the clip start
            CHECK_EQ(issues[0].to, 124);
        }
    }

    // --- Layout of a vertical video: what TikTok, Reels and Shorts cover
    {
        ProjectFormat v;
        v.width = 1080;
        v.height = 1920;
        CHECK(ExportCheck::isVertical(v));
        CHECK(!ExportCheck::isVertical(f));
        ProjectFormat feed = v;
        feed.height = 1350; // 4:5 (feed) is no full-screen video
        CHECK(!ExportCheck::isVertical(feed));
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << clip(1, 0, 300, "/x/a.mp4");
        tl.video[1].clips << title(2, 0, 50, "HOOK", 0, 0)            // middle: fine
                          << title(3, 50, 50, "x16", 0, -700)         // the caption/buttons at the bottom
                          << title(4, 100, 50, "RIGHT", 420, 0)       // the button column
                          << title(5, 150, 50, "TOP", 0, 910)         // the bar at the top
                          << title(6, 200, 50, "UPPER THIRD", 0, 450) // fine
                          << title(7, 250, 50, "OUT", -700, 0);       // outside the picture
        auto issues = ExportCheck::layout(tl, 0, 300, v);
        CHECK_EQ(checks(issues), QStringList({"safe_area", "safe_area", "safe_area", "safe_area"}));
        CHECK_EQ(clipsOf(issues), QVector<int>({3, 4, 5, 7}));
        if (issues.size() == 4) {
            CHECK_EQ(issues[0].from, 50);
            CHECK_EQ(issues[0].to, 100);
            CHECK(issues[0].message.contains("x16"));
            QStringList advice; // each zone its own advice
            for (const ExportCheck::Issue& i : issues) advice << i.message.section("–", 1);
            advice.removeDuplicates();
            CHECK_EQ(advice.size(), 4);
        }
        // Lifted out of the bottom zone by the clip's transform; a title moving into it is reported from there on
        tl.video[1].clips[1].transform.posY = 600;
        CHECK_EQ(clipsOf(ExportCheck::layout(tl, 0, 300, v)), QVector<int>({4, 5, 7}));
        tl.video[1].clips[1].transform.posY = 0;
        Keys::setKey(tl.video[1].clips[1], AnimParam::TitlePosY, 0, 0);
        Keys::setKey(tl.video[1].clips[1], AnimParam::TitlePosY, 49, -900);
        issues = ExportCheck::layout(tl, 50, 100, v);
        if (CHECK_EQ(clipsOf(issues), QVector<int>({3}))) {
            CHECK(issues[0].from > 60);
            CHECK_EQ(issues[0].to, 100);
        }
        // A counter x1, x2, x3 at the bottom of one track: one finding for all of them, from the first to the last
        {
            Timeline counter;
            counter.video.resize(2);
            counter.video[0].clips << clip(1, 0, 300, "/x/a.mp4");
            counter.video[1].clips << title(12, 100, 50, "x2", 0, -700) << title(11, 0, 50, "x1", 0, -700)
                                   << title(13, 200, 50, "x3", 0, -700) << title(14, 250, 50, "TOP", 0, 910);
            const auto grouped = ExportCheck::layout(counter, 0, 300, v);
            if (CHECK_EQ(clipsOf(grouped), QVector<int>({11, 14}))) {
                CHECK_EQ(grouped[0].clips, QVector<int>({11, 12, 13}));
                CHECK_EQ(grouped[0].from, 0);
                CHECK_EQ(grouped[0].to, 250);
                CHECK(grouped[0].message.contains("3") && grouped[0].message.contains("x1")
                      && grouped[0].message.contains("x3"));
                CHECK(grouped[1].clips.isEmpty()); // alone in its zone
            }
        }
        // The same in 16:9, and outside the checked range: nothing
        CHECK(ExportCheck::layout(tl, 0, 300, f).isEmpty());
        CHECK(ExportCheck::layout(tl, 0, 50, v).isEmpty());
        // Subtitles: the default style of a vertical project sits in the bottom zone; a third up is fine
        Timeline subs;
        subs.subtitles.resize(1);
        subs.subtitles[0].style = Subtitles::defaultStyle(v.size());
        subs.subtitles[0].cues << SubtitleCue{20, 0, 50, "Hello there"} << SubtitleCue{21, 60, 100, "Second line"};
        issues = ExportCheck::layout(subs, 0, 100, v);
        if (CHECK_EQ(checks(issues), QStringList({"subtitle_safe_area"}))) {
            CHECK_EQ(issues[0].from, 0);
            CHECK_EQ(issues[0].to, 100);
        }
        subs.subtitles[0].style.posY = 1920 * 0.3;
        CHECK(ExportCheck::layout(subs, 0, 100, v).isEmpty());

        // Hook: only vertical, from the start, up to 3 min; 11 frames 0.1 s apart; a still start is a hint
        CHECK(ExportCheck::wantsHook(0, 750, v));
        CHECK(!ExportCheck::wantsHook(0, 750, f));
        CHECK(!ExportCheck::wantsHook(25, 750, v));
        CHECK(!ExportCheck::wantsHook(0, 25 * 200, v));
        CHECK_EQ(ExportCheck::hookFrames(0, 750, v), QVector<int>({0, 3, 5, 8, 10, 13, 15, 18, 20, 23, 25}));
        CHECK_EQ(ExportCheck::hookFrames(0, 12, v).size(), 5);
        issues = ExportCheck::hook(0.4, 0, 750, v);
        if (CHECK_EQ(checks(issues), QStringList({"slow_hook"}))) {
            CHECK(issues[0].severity == ExportCheck::Severity::Info);
            CHECK_EQ(issues[0].to, 25);
        }
        CHECK(ExportCheck::hook(5, 0, 750, v).isEmpty());
    }

    // --- Mix: which track is music, a track with events buried under it
    {
        Timeline tl;
        tl.audio.resize(3);
        tl.audio[0].clips << clip(1, 0, 250, "/x/game.mp4");
        tl.audio[1].clips << clip(2, 0, 250, "/x/song.mp3");
        tl.audio[2].clips << clip(3, 0, 250, "/x/voice.wav");
        const auto picture = [](const QString& p) { return p.endsWith(".mp4"); };
        using ExportCheck::SoundRole;
        CHECK(ExportCheck::soundRole(tl, 0, picture) == SoundRole::Events);    // the sound of a recording
        CHECK(ExportCheck::soundRole(tl, 1, picture) == SoundRole::Undecided); // sound files only
        tl.audio[1].name = "Musik";
        tl.audio[2].name = "Mikrofon";
        CHECK(ExportCheck::soundRole(tl, 1, picture) == SoundRole::Music);
        CHECK(ExportCheck::soundRole(tl, 2, picture) == SoundRole::Events);
        tl.audio[0].name = "BGM";
        CHECK(ExportCheck::soundRole(tl, 0, picture) == SoundRole::Music); // the name wins
        tl.audio[0].name.clear();
        tl.audio[2].muted = true;
        CHECK(ExportCheck::soundRole(tl, 2, picture) == SoundRole::Other);
        tl.audio[1].solo = true;
        CHECK(ExportCheck::soundRole(tl, 0, picture) == SoundRole::Other);
        CHECK(ExportCheck::soundRole(tl, 1, picture) == SoundRole::Music);
        tl.audio[1].solo = false;
        // Without pauses = music, with pauses (speech) = not
        std::vector<float> song(250, lin(-8)), speech(250, lin(-10));
        for (size_t i = 0; i < speech.size(); i += 4) speech[i] = lin(-60);
        CHECK(ExportCheck::continuous(tl, 1, song, 0));
        CHECK(!ExportCheck::continuous(tl, 1, speech, 0));
        CHECK(!ExportCheck::continuous(tl, 1, std::vector<float>(250, 0.f), 0));

        // 10 s: music at -10 LUFS, game sound with shots at -36 and quiet steps at -50: the shots 26 LU under it
        const size_t blocks = 97;
        std::vector<double> music(blocks, power(-10)), game(blocks), loud(blocks), noise(blocks, power(-60));
        for (size_t k = 0; k < blocks; ++k) {
            game[k] = power(k % 5 == 0 ? -36 : -50);
            loud[k] = power(k % 5 == 0 ? -16 : -30);
        }
        const ExportCheck::TrackSound song0{1, true, music};
        auto issues = ExportCheck::mix(tl, {{0, false, game}, song0}, 0, f);
        if (CHECK_EQ(checks(issues), QStringList({"under_music"}))) {
            CHECK_EQ(issues[0].from, 0);
            CHECK(std::abs(issues[0].to - 250) <= 2);
            CHECK(issues[0].message.contains("26"));
            CHECK(issues[0].message.contains("A1"));
            CHECK(issues[0].message.contains("A2 (Musik)"));
        }
        CHECK(ExportCheck::mix(tl, {{0, false, loud}, song0}, 0, f).isEmpty());  // 6 LU under: audible
        CHECK(ExportCheck::mix(tl, {{0, false, noise}, song0}, 0, f).isEmpty()); // no events
        CHECK(ExportCheck::mix(tl, {{0, false, game}, {1, false, music}}, 0, f).isEmpty()); // no music at all
        // The music ducked under the shots: fine, although it is loud in between
        std::vector<double> ducked = music;
        for (size_t k = 0; k < blocks; k += 5) ducked[k] = power(-40);
        CHECK(ExportCheck::mix(tl, {{0, false, game}, {1, true, ducked}}, 0, f).isEmpty());
        // Music only in the first 4 s: the finding covers only that part
        tl.audio[1].clips[0].out = 99;
        issues = ExportCheck::mix(tl, {{0, false, game}, song0}, 0, f);
        if (CHECK_EQ(checks(issues), QStringList({"under_music"}))) CHECK(std::abs(issues[0].to - 100) <= 5);
    }

    // --- The command on a project and on its render
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8()); // backups of the test project
    const QString media = Check::makeMedia(dir.filePath("a.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=4", "-f", "lavfi", "-i",
                                            "sine=f=440:d=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                            "-shortest"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    const QString project = dir.filePath("p.schneidi"), out = dir.filePath("out.mp4");
    const auto run = [](const QString& name, const QJsonObject& raw) {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, raw), ctx);
        } catch (const Cli::Error& e) {
            std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
            return QJsonObject{{"failed", e.code}};
        }
    };
    run("new", {{"project", project}, {"width", 320}, {"height", 180}, {"media", QJsonArray{media}}});
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0},{"op":"place","media":"%1","at":"4.04s"},
                                    {"op":"subtitles","cues":[{"from":"1s","to":"1.2s","text":"Hi"}]}])")
                             .arg(media)}});
    QJsonObject r = run("check", {{"source", project}});
    CHECK_EQ(checksOf(r), QStringList({"loudness", "subtitle_short", "black"})); // warnings sorted by time
    CHECK_EQ(r["errors"].toInt(), 0);
    CHECK_EQ(r["warnings"].toInt(), 3);
    const QJsonArray issues = r["issues"].toArray();
    if (CHECK_EQ(issues.size(), 3)) {
        CHECK_EQ(issues[2]["from"].toInt(), 100);
        CHECK_EQ(issues[2]["to"].toInt(), 101);
        CHECK(issues[2]["from_tc"].isString());
    }
    const double lufs = r["sound"].toObject()["integrated_lufs"].toDouble();
    CHECK(lufs < -16);
    // Quick checks only, other target, a range without the gap
    r = run("check", {{"source", project}, {"sound", false}, {"to", 99}});
    CHECK_EQ(checksOf(r), QStringList({"subtitle_short"}));
    CHECK(!r.contains("sound"));
    r = run("check", {{"source", project}, {"target_lufs", lufs}, {"from", "2s"}, {"to", "3.9s"}});
    CHECK(checksOf(r).isEmpty());
    CHECK(!r.contains("fix"));
    // Too quiet: `fix` has the ops for `edit` (on a copy); applied, the loudness meets the target
    {
        const QString copy = dir.filePath("fix.schneidi");
        QFile::copy(project, copy);
        r = run("check", {{"source", copy}});
        const QJsonObject fix = r["fix"].toObject();
        const QJsonArray ops = fix["ops"].toArray();
        if (CHECK_EQ(ops.size(), 1)) {
            const QJsonObject mixer = ops[0].toObject();
            CHECK_EQ(mixer["op"].toString(), QString("mixer"));
            CHECK_EQ(mixer["track"].toString(), QString("master"));
            CHECK(mixer["volume_db"].toDouble() > 2 && mixer["limiter"].toBool());
        }
        CHECK(fix["what"].toString().contains("master fader +"));
        // With a timeline named, the ops start by choosing it
        const QJsonObject info = run("info", {{"project", copy}});
        const QString name = info["timelines"].toArray().at(0).toObject()["name"].toString();
        const QJsonObject named = run("check", {{"source", copy}, {"timeline", name}})["fix"].toObject();
        const QJsonArray namedOps = named["ops"].toArray();
        if (CHECK_EQ(namedOps.size(), 2)) CHECK_EQ(namedOps[0].toObject()["op"].toString(), QString("timeline"));
        run("edit", {{"project", copy}, {"ops", QString::fromUtf8(QJsonDocument(ops).toJson())}});
        r = run("check", {{"source", copy}});
        CHECK(!checksOf(r).contains("loudness"));
        CHECK(!r.contains("fix"));
    }

    // The rendered file: the sound checks again; the gap of one frame is too short for the picture check of a file
    run("render", {{"project", project}, {"out", out}});
    r = run("check", {{"source", out}});
    CHECK_EQ(checksOf(r), QStringList({"loudness"}));
    CHECK_EQ(r["frames"].toInt(), 201);
    // A file with a long black stretch and no sound
    const QString black = Check::makeMedia(dir.filePath("black.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2", "-f", "lavfi", "-i",
                                            "color=c=black:s=320x180:r=25:d=2", "-filter_complex", "[0][1]concat=n=2",
                                            "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    r = run("check", {{"source", black}});
    CHECK_EQ(checksOf(r), QStringList({"no_audio", "black_file"}));
    if (const QJsonArray list = r["issues"].toArray(); list.size() == 2) {
        CHECK(std::abs(list[1]["from"].toInt() - 50) <= 1);
        CHECK(std::abs(list[1]["to"].toInt() - 100) <= 1);
    }

    // --- Stutter of real files: 30 fps with frames 45-59 dropped (a jump of 16 frame durations at 44/30 s), as MP4
    // with B-frames and as MKV without; tone bursts at 0.6 s and 3.4 s for `highlights`
    const QStringList gapArgs{"-f", "lavfi", "-i", "testsrc2=s=160x90:r=30:d=5", "-f", "lavfi", "-i",
                              "aevalsrc='if(between(t,0.6,0.8)+between(t,3.4,3.6),0.5*sin(2*PI*440*t),0)':d=5:s=48000",
                              "-vf", "select='not(between(n,45,59))'", "-fps_mode", "vfr", "-pix_fmt", "yuv420p",
                              "-c:a", "aac"};
    const QString gapMp4 = Check::makeMedia(dir.filePath("gap.mp4"), gapArgs + QStringList{"-c:v", "libx264"});
    const QString gapMkv = Check::makeMedia(dir.filePath("gap.mkv"), gapArgs + QStringList{"-c:v", "libx264", "-bf", "0"});
    if (!CHECK(!gapMp4.isEmpty() && !gapMkv.isEmpty())) return Check::result();
    for (const QString& file : {gapMp4, gapMkv}) {
        const auto s = FrameGaps::scan(file);
        if (!CHECK(s.has_value())) continue;
        CHECK(std::abs(s->frameDuration - 1 / 30.0) < 1e-3);
        CHECK_EQ(s->frames, 135);
        if (CHECK_EQ(s->jumps.size(), 1)) {
            CHECK(std::abs(s->jumps[0].at - 44 / 30.0) < 2e-3);
            CHECK(std::abs(s->jumps[0].length - 16 / 30.0) < 2e-3);
            CHECK_EQ(s->jumps[0].missing, 15);
        }
        // From the disk cache the same
        FrameGaps::clearMemoryCache();
        CHECK(FrameGaps::scan(file) == s);
    }
    CHECK(FrameGaps::scan(media)->jumps.isEmpty()); // constant frame rate
    CHECK(!FrameGaps::scan(dir.filePath("black.mp4") + ".missing").has_value());

    // probe gaps: the dropped frames as a range of the file's frames, and as one stretch
    r = run("probe", {{"file", gapMp4}, {"gaps", true}});
    const QJsonObject gaps = r["gaps"].toObject();
    CHECK_EQ(gaps["jump_count"].toInt(), 1);
    CHECK_EQ(gaps["missing_frames"].toInt(), 15);
    if (CHECK_EQ(gaps["jumps"].toArray().size(), 1)) {
        CHECK_EQ(gaps["jumps"][0]["from"].toInt(), 45);
        CHECK_EQ(gaps["jumps"][0]["to"].toInt(), 60);
    }
    CHECK_EQ(gaps["stutter"].toArray().size(), 1);
    CHECK(!run("probe", {{"file", gapMp4}}).contains("gaps"));
    CHECK_EQ(run("probe", {{"file", media}, {"gaps", true}})["gaps"].toObject()["jump_count"].toInt(), 0);

    // highlights: the candidates over the jump carry it and their clean parts, keep_clean leaves the lag out
    r = run("highlights", {{"file", gapMp4}});
    int withStutter = 0;
    for (const QJsonValue& v : r["candidates"].toArray()) {
        if (!v.toObject().contains("stutter")) continue;
        ++withStutter;
        CHECK_EQ(v["stutter"][0]["missing_frames"].toInt(), 15);
        for (const QJsonValue& piece : v["clean"].toArray())
            CHECK(piece["to"].toInt() <= 45 || piece["from"].toInt() >= 60);
    }
    CHECK(withStutter >= 1);
    if (CHECK(r.contains("keep_clean")))
        for (const QJsonValue& piece : r["keep_clean"].toArray())
            CHECK(piece[1].toInt() <= 45 || piece[0].toInt() >= 60);

    // check: a clip over the jump warns with the clip and the frames that stand still, one beside it not
    const QString stutterProject = dir.filePath("stutter.schneidi");
    run("new", {{"project", stutterProject}, {"width", 160}, {"height", 90}, {"fps", 30}});
    run("edit", {{"project", stutterProject},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0,"in":0,"out":"2.5s"},
                                    {"op":"place","media":"%1","at":"2.5s","in":"2.5s","out":"4.4s"}])")
                             .arg(gapMp4)}});
    r = run("check", {{"source", stutterProject}, {"sound", false}});
    CHECK_EQ(checksOf(r), QStringList({"stutter"}));
    if (const QJsonArray list = r["issues"].toArray(); CHECK_EQ(list.size(), 1)) {
        CHECK(list[0]["clip"].toInt() > 0);
        CHECK(std::abs(list[0]["from"].toInt() - 45) <= 1);
        CHECK(std::abs(list[0]["to"].toInt() - 60) <= 1);
    }

    // --- A vertical short against a normal 16:9 video. Game sound: shots every second over quiet steps; music: a
    // tone without pauses from a sound file
    const QString game = Check::makeMedia(
        dir.filePath("game.mp4"),
        {"-f", "lavfi", "-i", "color=c=gray:s=270x480:r=25:d=6", "-f", "lavfi", "-i",
         "aevalsrc='0.4*sin(2*PI*700*t)*lt(mod(t\\,1)\\,0.08)+0.004*sin(2*PI*200*t)':d=6:s=48000", "-c:v", "libx264",
         "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    const QString moving = Check::makeMedia(
        dir.filePath("moving.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=6,scroll=h=0.01", "-f", "lavfi", "-i",
         "aevalsrc='0.4*sin(2*PI*700*t)*lt(mod(t\\,1)\\,0.08)+0.004*sin(2*PI*200*t)':d=6:s=48000", "-c:v", "libx264",
         "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    const QString song = Check::makeMedia(dir.filePath("song.wav"), {"-f", "lavfi", "-i", "sine=frequency=300:duration=6"});
    if (!CHECK(!game.isEmpty() && !moving.isEmpty() && !song.isEmpty())) return Check::result();
    const QString vertical = dir.filePath("vertical.schneidi"), wide = dir.filePath("wide.schneidi");
    run("new", {{"project", vertical}, {"width", 270}, {"height", 480}, {"fps", 25}});
    run("edit", {{"project", vertical},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0},
                                    {"op":"place","media":"%2","at":0,"track":"A2"},
                                    {"op":"mixer","track":"A1","volume_db":-30},
                                    {"op":"title","text":"x16","at":"2s","duration":"1s","size":30,"y":-190}])")
                             .arg(game, song)}});
    r = run("check", {{"source", vertical}});
    const QStringList found = checksOf(r);
    CHECK(found.contains("under_music"));
    CHECK(found.contains("safe_area"));
    CHECK(found.contains("slow_hook"));
    for (const QJsonValue& v : r["issues"].toArray()) {
        if (v["check"].toString() == "under_music") {
            CHECK(v["message"].toString().contains("A1"));
            CHECK(v["from"].toInt() <= 5);
            CHECK(v["to"].toInt() >= 140);
        } else if (v["check"].toString() == "safe_area") {
            CHECK_EQ(v["from"].toInt(), 50);
            CHECK(v["clip"].toInt() > 0);
        } else if (v["check"].toString() == "slow_hook") {
            CHECK_EQ(v["severity"].toString(), QString("info"));
        }
    }
    // Game sound raised, title a third up, starting with motion: none of the three
    run("edit", {{"project", vertical},
                 {"ops", QString(R"([{"op":"mixer","track":"A1","volume_db":0},
                                    {"op":"mixer","track":"A2","volume_db":-6},
                                    {"op":"place","media":"%1","at":0,"track":"V1","in":0,"out":"1.5s"}])")
                             .arg(moving)}});
    const QJsonObject titles = run("info", {{"project", vertical}});
    for (const QJsonValue& t : titles["timeline"]["tracks"].toArray())
        for (const QJsonValue& c : t["clips"].toArray())
            if (c["kind"].toString() == "title")
                run("edit", {{"project", vertical},
                             {"ops", QString(R"([{"op":"title","clip":%1,"y":60}])").arg(c["id"].toInt())}});
    for (const QString& c : checksOf(run("check", {{"source", vertical}})))
        CHECK(c != "under_music" && c != "safe_area" && c != "slow_hook");

    // The same material in a normal 16:9 project, the title as a lower third: no new findings
    run("new", {{"project", wide}, {"width", 320}, {"height", 180}, {"fps", 25}});
    run("edit", {{"project", wide},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0},
                                    {"op":"place","media":"%2","at":0,"track":"A2"},
                                    {"op":"mixer","track":"A2","volume_db":-6},
                                    {"op":"title","text":"Name","at":"1s","duration":"2s","size":20,"y":-70}])")
                             .arg(moving, song)}});
    for (const QString& c : checksOf(run("check", {{"source", wide}})))
        CHECK(c != "under_music" && c != "safe_area" && c != "subtitle_safe_area" && c != "slow_hook" && c != "stutter");

    return Check::result();
}
