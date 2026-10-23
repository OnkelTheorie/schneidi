// Reference renders of the sound (docs/archiv/plan-0.10-ton.md, step 0 "Messgrundlage"): one small project per sound
// feature (clip, track, master, reading of the source formats), rendered through the normal export path to WAV
// (48 kHz stereo, 24 bit). The written references are the sound as it is before the own audio engine; every later
// audio path must render the same projects to the same samples (or within a tolerance named per case).
//
// The references live outside the public repository (default: docs/testdaten/ton-referenz, AUDIO_REFERENCE_DIR
// overrides): quellen/ (generated sources, kept so that encoder updates do not change them), projekte/<case>.schneidi
// (media paths relative, open them in the app or the CLI), referenz/<case>.flac (24 bit lossless) and manifest.json
// (MLT/FFmpeg version, level and render time per case). Missing folder = skipped (CI).
//
//   AUDIO_REFERENCE=write          sources (if missing), projects and references of all cases, then checked once more
//   AUDIO_REFERENCE=write:a,b      only these cases
//   (unset)                        render every project and compare with its reference
//   AUDIO_REFERENCE_KEEP=<dir>     keep the renders of a check run there (listening, diff in an editor)

#include "check.h"

#include "core/Editor.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <functional>
#include <vector>

#ifndef SCHNEIDI_AUDIO_REFERENCE
#define SCHNEIDI_AUDIO_REFERENCE ""
#endif

namespace {

constexpr int kRate = 48000;
constexpr double kSourceSeconds = 6;
// Same samples: the reference is 24 bit, a repeated render of the same path must hit it to the last bit;
// 2 LSB leave room for a differently rounding conversion to 24 bit
constexpr double kExact = 2.0 / (1 << 23);

QString g_dir; // reference folder

// ---- Sources ----

struct Source {
    QString file;     // in quellen/
    bool video = false;
    int streams = 1;  // audio streams
    int channels = 2; // of each stream
    QString path() const { return g_dir + "/quellen/" + file; }
};

const Source kChirp{"chirp.mov", true};
const Source kTone{"tone.mov", true};
const Source kNoisy{"noisy.wav"};
const Source kMono{"mono.wav", false, 1, 1};
const Source kRate44{"rate44.wav"};
const Source kSurround{"surround.wav", false, 1, 6};
const Source kFlac{"music.flac"};
const Source kMp3{"cover.mp3"};
const Source kAac{"aac.mp4", true};
const Source kLate{"late.mov", true};
const Source kStreams{"streams.mov", true, 2};

const QString kVideo = "testsrc2=size=64x36:rate=25:duration=6";
const QStringList kX264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
// Not periodic, different per channel: a shift, a swapped channel or a wrong pan shows at once
const QString kChirpL = "0.3*sin(2*PI*(200*t+150*t*t))";   // 200 -> 2000 Hz in 6 s
const QString kChirpR = "0.2*sin(2*PI*(3000*t-200*t*t))";  // 3000 -> 600 Hz
const QString kToneL = "0.25*sin(2*PI*523.25*t)*(0.75+0.25*sin(2*PI*1.3*t))";
const QString kToneR = "0.25*sin(2*PI*783.99*t)*(0.75+0.25*cos(2*PI*0.7*t))";

QString eval(const QString& l, const QString& r, int rate = kRate, double seconds = kSourceSeconds)
{
    return QString("aevalsrc=%1|%2:s=%3:d=%4").arg(l, r).arg(rate).arg(seconds);
}

bool makeSources()
{
    QDir().mkpath(g_dir + "/quellen");
    const QString chirp = eval(kChirpL, kChirpR), tone = eval(kToneL, kToneR);
    const struct { const Source& s; QStringList args; } all[] = {
        {kChirp, QStringList{"-f", "lavfi", "-i", kVideo, "-f", "lavfi", "-i", chirp} + kX264 + QStringList{"-c:a", "pcm_s16le"}},
        {kTone, QStringList{"-f", "lavfi", "-i", kVideo, "-f", "lavfi", "-i", tone} + kX264 + QStringList{"-c:a", "pcm_s16le"}},
        // speech-like tone under steady white noise (denoise)
        {kNoisy, {"-filter_complex",
                  eval("0.3*sin(2*PI*220*t)*gt(sin(2*PI*0.8*t)\\,0)", "0.3*sin(2*PI*330*t)*gt(sin(2*PI*0.8*t)\\,0)")
                      + "[a];anoisesrc=a=0.05:c=white:seed=7:d=6:r=48000,pan=stereo|c0=c0|c1=c0[n];[a][n]amix=inputs=2:normalize=0",
                  "-c:a", "pcm_s16le"}},
        {kMono, {"-f", "lavfi", "-i", QString("aevalsrc=%1:s=48000:d=6").arg(kToneL), "-c:a", "pcm_s16le"}},
        {kRate44, {"-f", "lavfi", "-i", eval(kChirpL, kChirpR, 44100), "-c:a", "pcm_s16le"}},
        {kSurround, {"-f", "lavfi", "-i",
                     "aevalsrc=0.1*sin(2*PI*300*t)|0.1*sin(2*PI*400*t)|0.1*sin(2*PI*500*t)|0.1*sin(2*PI*60*t)|"
                     "0.1*sin(2*PI*600*t)|0.1*sin(2*PI*700*t):s=48000:d=6:c=5.1",
                     "-c:a", "pcm_s16le"}},
        {kFlac, {"-f", "lavfi", "-i", chirp, "-c:a", "flac", "-sample_fmt", "s32"}},
        // MP3 with a cover picture (attached_pic): MLT must not take it for a video. A one-frame picture source:
        // with "-frames:v 1" FFmpeg ends the whole file after 48 ms
        {kMp3, {"-f", "lavfi", "-i", tone, "-f", "lavfi", "-i", "color=c=red:s=64x64:r=1:d=1", "-map", "0:a", "-map", "1:v",
                "-c:v", "mjpeg", "-disposition:v", "attached_pic", "-c:a", "libmp3lame", "-b:a", "192k", "-id3v2_version", "3"}},
        // AAC with its encoder delay (priming) as in every phone/OBS MP4
        {kAac, QStringList{"-f", "lavfi", "-i", kVideo, "-f", "lavfi", "-i", tone} + kX264
                   + QStringList{"-c:a", "aac", "-b:a", "192k", "-shortest"}},
        // Sound begins 0.3 s after the picture (start_time of the stream; edit list in MOV)
        {kLate, QStringList{"-f", "lavfi", "-i", kVideo, "-itsoffset", "0.3", "-f", "lavfi", "-i", eval(kChirpL, kChirpR, kRate, 5.7)}
                    + kX264 + QStringList{"-c:a", "pcm_s16le"}},
        // Two audio streams like OBS (game + microphone)
        {kStreams, QStringList{"-f", "lavfi", "-i", kVideo, "-f", "lavfi", "-i", tone, "-f", "lavfi", "-i", chirp, "-map",
                               "0:v", "-map", "1:a", "-map", "2:a"}
                       + kX264 + QStringList{"-c:a", "pcm_s16le"}},
    };
    for (const auto& a : all)
        if (!QFileInfo::exists(a.s.path()) && Check::makeMedia(a.s.path(), a.args).isEmpty()) return false;
    return true;
}

// ---- Projects ----

struct Proj {
    ProjectData d;
    int nextId = 0;
    explicit Proj(FrameRate rate = {25, 1})
    {
        d.format = ProjectFormat{320, 180, rate};
        d.timeline.video.resize(1);
        d.timeline.audio.resize(1);
        for (Track& t : d.timeline.audio) t.kind = TrackKind::Audio;
    }
    double fps() const { return d.format.rate.fps(); }
    void addMedia(const Source& s)
    {
        for (const MediaInfo& m : d.media)
            if (m.path == s.path()) return;
        MediaInfo m;
        m.path = s.path();
        m.name = s.file;
        m.length = int(kSourceSeconds * fps());
        m.hasVideo = s.video;
        m.hasAudio = true;
        for (int k = 0; k < s.streams; ++k) m.audioStreams << AudioStreamInfo{s.channels, {}};
        d.media << m;
    }
    Track& track(Timeline& tl, int a)
    {
        while (tl.audio.size() <= a) {
            Track t;
            t.kind = TrackKind::Audio;
            tl.audio << t;
        }
        return tl.audio[a];
    }
    Track& track(int a) { return track(d.timeline, a); }
    // Audio clip on A(a+1); the reference returned is valid until the next clip
    Clip& clip(Timeline& tl, int a, const Source& s, int start, int in, int out)
    {
        addMedia(s);
        Clip c;
        c.id = ++nextId;
        c.mediaPath = s.path();
        c.start = start;
        c.in = in;
        c.out = out;
        Track& t = track(tl, a);
        t.clips << c;
        std::sort(t.clips.begin(), t.clips.end(), [](const Clip& x, const Clip& y) { return x.start < y.start; });
        for (Clip& k : t.clips)
            if (k.id == c.id) return k;
        return t.clips.last();
    }
    Clip& clip(int a, const Source& s, int start, int in, int out) { return clip(d.timeline, a, s, start, in, out); }
    ProjectData data()
    {
        d.lastClipId = nextId;
        return d;
    }
};

void keys(Clip& c, AnimParam p, std::initializer_list<Keyframe> k) { c.keys[p] = KeyTrack(k); }

// Dissolve between two adjacent clips on the same track (both edges, same style)
void dissolve(Clip& left, Clip& right, int frames, AudioCurve curve, TransitionAlign align = TransitionAlign::Center)
{
    left.transOut = right.transIn = frames;
    left.transOutStyle.audio = right.transInStyle.audio = curve;
    left.transOutStyle.align = right.transInStyle.align = align;
}

struct Case {
    const char* name;
    const char* what; // what the case covers (manifest, failures)
    std::function<ProjectData()> make;
};

QVector<Case> cases()
{
    return {
        {"basic", "in/out, gap of silence, end at the clip end", [] {
             Proj p;
             p.clip(0, kChirp, 0, 10, 59);
             p.clip(0, kChirp, 70, 100, 124);
             return p.data();
         }},
        {"volume", "static clip volume -6 dB, +6 dB, -inf (muted)", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 49).volumeDb = -6;
             p.clip(0, kTone, 50, 50, 99).volumeDb = 6;
             p.clip(0, kChirp, 100, 0, 49).volumeDb = kMinVolumeDb;
             return p.data();
         }},
        {"volume_keys", "volume keyframes: linear, ease in/out, bezier; in point moves the keys", [] {
             Proj p;
             keys(p.clip(0, kChirp, 0, 0, 99), AnimParam::Volume,
                  {{0, -20, KeyEase::Linear}, {30, 0, KeyEase::EaseInOut}, {55, -12, KeyEase::EaseIn},
                   {75, -3, KeyEase::Bezier, -5, 2, 5, -4}, {99, 3}});
             keys(p.clip(0, kTone, 100, 40, 89), AnimParam::Volume, {{20, 0}, {60, -18, KeyEase::EaseOut}, {80, -6}});
             return p.data();
         }},
        {"pan", "static pan left/right, pan keyframes", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 49).pan = -60;
             p.clip(0, kTone, 50, 50, 99).pan = 100;
             keys(p.clip(0, kChirp, 100, 50, 99), AnimParam::Pan, {{50, -100}, {99, 100}});
             return p.data();
         }},
        {"fades", "fade handles in/out, fade longer than the clip", [] {
             Proj p;
             Clip& a = p.clip(0, kChirp, 0, 0, 74);
             a.fadeIn = 12;
             a.fadeOut = 20;
             Clip& b = p.clip(0, kTone, 80, 0, 29);
             b.fadeIn = 1;
             b.fadeOut = 60;
             return p.data();
         }},
        {"crossfades", "dissolves +3/0/-3 dB, centered and start on edit", [] {
             Proj p;
             p.clip(0, kTone, 0, 20, 69);
             p.clip(0, kChirp, 50, 20, 69);
             p.clip(0, kTone, 100, 100, 149);
             p.clip(0, kChirp, 150, 80, 129);
             QVector<Clip>& c = p.track(0).clips;
             dissolve(c[0], c[1], 10, AudioCurve::Plus3dB);
             dissolve(c[1], c[2], 12, AudioCurve::Zero, TransitionAlign::Start);
             dissolve(c[2], c[3], 16, AudioCurve::Minus3dB);
             return p.data();
         }},
        {"transition_fades", "transitions without a neighbor: from/to silence", [] {
             Proj p;
             Clip& c = p.clip(0, kChirp, 10, 0, 49);
             c.transIn = 8;
             c.transOut = 12;
             return p.data();
         }},
        {"hard_cuts", "hard cuts between sources (declick) and a through edit in one source", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 24);
             p.clip(0, kChirp, 25, 30, 54);
             p.clip(0, kTone, 50, 77, 101);
             p.clip(0, kTone, 75, 102, 126); // continues the previous clip's source
             return p.data();
         }},
        {"subframe", "sub-frame offsets 37/100, 99/100, 1/100", [] {
             Proj p;
             p.clip(0, kChirp, 0, 0, 49).subframe = 37;
             p.clip(0, kTone, 60, 0, 29).subframe = 99;
             p.clip(0, kChirp, 90, 60, 109).subframe = 1;
             return p.data();
         }},
        {"speed_pitch", "speed 50 % and 200 % keeping the pitch", [] {
             Proj p;
             Clip& a = p.clip(0, kChirp, 0, 0, 99);
             a.speed = 0.5;
             Clip& b = p.clip(0, kTone, 100, 0, 49);
             b.speed = 2.0;
             return p.data();
         }},
        {"speed_nopitch", "speed 50 % and 200 % without pitch correction, 150 % with in point", [] {
             Proj p;
             Clip& a = p.clip(0, kChirp, 0, 0, 99);
             a.speed = 0.5;
             a.keepPitch = false;
             Clip& b = p.clip(0, kTone, 100, 0, 49);
             b.speed = 2.0;
             b.keepPitch = false;
             Clip& c = p.clip(0, kChirp, 150, 20, 69);
             c.speed = 1.5;
             c.keepPitch = false;
             return p.data();
         }},
        {"reverse", "reversed, reversed at 50 % without pitch", [] {
             Proj p;
             p.clip(0, kChirp, 0, 0, 74).reverse = true;
             Clip& b = p.clip(0, kTone, 75, 10, 109);
             b.reverse = true;
             b.speed = 0.5;
             b.keepPitch = false;
             return p.data();
         }},
        {"ramp", "speed ramp: 100 % -> 50 % (hard) -> 200 % (smooth over 10 frames)", [] {
             Proj p;
             Clip& c = p.clip(0, kChirp, 0, 0, 129);
             c.ramp = {{25, 0.5, 0}, {60, 2.0, 10}};
             return p.data();
         }},
        {"freeze", "freeze frame (muted) followed by the normal clip", [] {
             Proj p;
             Clip& a = p.clip(0, kTone, 0, 0, 49);
             a.freeze = true;
             a.freezeFrame = 10;
             p.clip(0, kTone, 50, 50, 99);
             return p.data();
         }},
        {"disabled_muted", "disabled clip, muted track, hidden-free second track", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 49);
             p.clip(0, kTone, 50, 50, 99).enabled = false;
             p.clip(1, kChirp, 25, 0, 74);
             p.clip(2, kChirp, 0, 80, 129);
             p.track(2).muted = true;
             return p.data();
         }},
        {"solo", "solo on A2: A1 and A3 silent", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 74);
             p.clip(1, kChirp, 0, 0, 74);
             p.clip(2, kNoisy, 0, 0, 74);
             p.track(1).solo = true;
             return p.data();
         }},
        {"denoise", "noise reduction effect (engine/Denoise) with default values and stronger", [] {
             Proj p;
             p.clip(0, kNoisy, 0, 0, 74).effects << EffectInstance{"denoise", {}, true};
             p.clip(0, kNoisy, 75, 75, 149).effects << EffectInstance{"denoise", {{"reduction", 30.0}, {"track", false}}, true};
             return p.data();
         }},
        {"compound", "compound clip (two tracks inside, own volumes) with in point, volume and pan outside", [] {
             Proj p;
             Timeline content;
             content.video.resize(1);
             p.clip(content, 0, kTone, 0, 0, 49).volumeDb = -3;
             p.clip(content, 1, kChirp, 25, 0, 74).pan = 40;
             Sequence s{2, "Compound 1", true, 0, content};
             p.d.sequences << s;
             p.d.currentSequence = 1;
             Clip c;
             c.id = ++p.nextId;
             c.kind = ClipKind::Compound;
             c.sequenceId = 2;
             c.start = 5;
             c.in = 10;
             c.out = 89;
             c.volumeDb = -2;
             c.pan = -30;
             p.track(0).clips << c;
             return p.data();
         }},
        {"multicam", "multicam clip: sound of the active angle, switched by cuts", [] {
             Project project;
             Selection sel;
             Editor ed{&project, &sel};
             Proj p;
             p.addMedia(kTone);
             p.addMedia(kChirp);
             project.load(p.data());
             const int seq = ed.createMulticamClip("Multicam", {{"Tone", {{kTone.path(), 0}}}, {"Chirp", {{kChirp.path(), 0}}}});
             ed.addSequenceAt(seq, 0, 0);
             ed.multicamCut(40, 1, Multicam::Mode::Both);
             ed.multicamCut(80, 0, Multicam::Mode::Audio);
             ed.multicamCut(110, 1, Multicam::Mode::Video); // picture only: sound stays on the tone
             return project.data();
         }},
        {"track_eq", "track EQ: high pass, bell +9 dB, high shelf -6 dB", [] {
             Proj p;
             p.clip(0, kChirp, 0, 0, 149);
             TrackEq& eq = p.track(0).eq;
             eq.on = true;
             eq.bands[0].on = true;
             eq.bands[0].freq = 150;
             eq.bands[3].freq = 1000;
             eq.bands[3].gainDb = 9;
             eq.bands[3].q = 2;
             eq.bands[4].gainDb = -6;
             return p.data();
         }},
        {"track_dynamics", "track compressor driven hard, with makeup", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 99).volumeDb = 6;
             TrackDynamics& d = p.track(0).dynamics;
             d.on = true;
             d.thresholdDb = -24;
             d.ratio = 6;
             d.attackMs = 5;
             d.releaseMs = 80;
             d.makeupDb = 3;
             return p.data();
         }},
        {"track_mix", "track faders and pans, two tracks summed", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 99);
             p.clip(1, kChirp, 30, 0, 99);
             p.track(0).volumeDb = -4;
             p.track(0).pan = -40;
             p.track(1).volumeDb = 3;
             p.track(1).pan = 70;
             return p.data();
         }},
        {"master", "master fader and limiter on loud material", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 99).volumeDb = 12;
             p.clip(1, kChirp, 0, 0, 99).volumeDb = 12;
             p.d.timeline.masterVolumeDb = -2;
             p.d.timeline.masterLimiter = true;
             p.d.timeline.masterLimiterDb = -1;
             return p.data();
         }},
        {"master_fader", "master fader alone, no limiter", [] {
             Proj p;
             p.clip(0, kChirp, 0, 0, 49);
             p.d.timeline.masterVolumeDb = -7.5;
             return p.data();
         }},
        {"mono", "mono WAV source (one channel on both sides)", [] {
             Proj p;
             p.clip(0, kMono, 0, 0, 74);
             p.clip(0, kMono, 75, 90, 114).pan = 50;
             return p.data();
         }},
        {"rate44", "44.1 kHz source resampled, from the start and from an in point", [] {
             Proj p;
             p.clip(0, kRate44, 0, 0, 74);
             p.clip(0, kRate44, 80, 33, 82);
             return p.data();
         }},
        {"surround", "5.1 WAV source mixed down to stereo", [] {
             Proj p;
             p.clip(0, kSurround, 0, 0, 74);
             return p.data();
         }},
        {"flac", "24 bit FLAC source with in point", [] {
             Proj p;
             p.clip(0, kFlac, 0, 13, 62);
             p.clip(0, kFlac, 50, 101, 140);
             return p.data();
         }},
        {"mp3_cover", "MP3 with cover picture, from the start and from an in point", [] {
             Proj p;
             p.clip(0, kMp3, 0, 0, 74);
             p.clip(0, kMp3, 80, 40, 89);
             return p.data();
         }},
        {"aac", "AAC in MP4 (encoder delay), from the start and from in points", [] {
             Proj p;
             p.clip(0, kAac, 0, 0, 49);
             p.clip(0, kAac, 50, 37, 86);
             p.clip(0, kAac, 100, 120, 144);
             return p.data();
         }},
        {"late_start", "sound stream starts 0.3 s after the picture", [] {
             Proj p;
             p.clip(0, kLate, 0, 0, 99);
             p.clip(0, kLate, 110, 50, 89);
             return p.data();
         }},
        {"streams", "second and first audio stream of one file", [] {
             Proj p;
             p.clip(0, kStreams, 0, 0, 49).audioStream = 1;
             p.clip(0, kStreams, 50, 50, 99);
             p.clip(1, kStreams, 25, 0, 49).audioStream = 1;
             return p.data();
         }},
        {"ntsc", "29.97 fps project: cuts, fade and sub-frame at non-integer samples per frame", [] {
             Proj p({30000, 1001});
             p.clip(0, kChirp, 0, 0, 59);
             p.clip(0, kTone, 60, 13, 72).fadeIn = 9;
             p.clip(0, kChirp, 120, 100, 140).subframe = 50;
             p.clip(1, kTone, 37, 0, 88).volumeDb = -9;
             return p.data();
         }},
        {"multitrack", "three tracks summed, one at -10 dB", [] {
             Proj p;
             p.clip(0, kTone, 0, 0, 99);
             p.clip(1, kChirp, 20, 0, 79);
             p.clip(2, kNoisy, 40, 0, 59).volumeDb = -10;
             return p.data();
         }},
    };
}

// ---- Rendering and comparing ----

// Sound of a project through the export path (Exporter, as Deliver and the CLI): WAV 48 kHz stereo 24 bit
bool renderWav(const QString& projectPath, const QString& out, qint64* ms, QString* error)
{
    ProjectData d;
    if (!ProjectFile::load(projectPath, &d, error)) return false;
    Project project;
    project.load(d);
    RenderJob job;
    job.id = 1;
    job.path = out;
    job.settings.format = "wav";
    job.settings.audioBits = 24;
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        *error = msg;
        loop.quit();
    });
    QElapsedTimer t;
    t.start();
    if (!ex.start(project.renderTimeline(), RenderQueue::exportSettings(job, d.format), error)) return false;
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    *ms = t.elapsed();
    if (!ok && error->isEmpty()) *error = "timeout";
    return ok;
}

// Interleaved stereo float samples
std::vector<float> decode(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-f", "f32le", "-ac", "2", "-ar", QString::number(kRate), "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const float*>(d.constData());
    return std::vector<float>(s, s + d.size() / qsizetype(sizeof(float)));
}

bool toFlac(const QString& wav, const QString& flac)
{
    QFile::remove(flac);
    return !Check::makeMedia(flac, {"-i", wav, "-c:a", "flac", "-compression_level", "8"}).isEmpty();
}

struct Diff {
    qsizetype frames = 0, refFrames = 0;
    double maxDiff = 0;
    qsizetype maxAt = 0; // sample frame
    int channel = 0;
    double diffDb = -200; // RMS of the difference, dBFS
};

Diff compare(const std::vector<float>& got, const std::vector<float>& ref)
{
    Diff d;
    d.frames = qsizetype(got.size() / 2);
    d.refFrames = qsizetype(ref.size() / 2);
    const size_t n = std::min(got.size(), ref.size());
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const double e = std::abs(double(got[i]) - ref[i]);
        sum += e * e;
        if (e > d.maxDiff) {
            d.maxDiff = e;
            d.maxAt = qsizetype(i / 2);
            d.channel = int(i % 2);
        }
    }
    if (n) d.diffDb = 10 * std::log10(sum / double(n) + 1e-30);
    return d;
}

QJsonObject levels(const std::vector<float>& s)
{
    double peak[2] = {0, 0}, sum[2] = {0, 0};
    for (size_t i = 0; i < s.size(); ++i) {
        peak[i % 2] = std::max(peak[i % 2], double(std::abs(s[i])));
        sum[i % 2] += double(s[i]) * s[i];
    }
    const double frames = std::max<double>(1, double(s.size() / 2));
    auto db = [](double v) { return v > 0 ? std::round(200 * std::log10(v)) / 10 : -200.0; };
    return {{"seconds", frames / kRate},
            {"peak_db", QJsonArray{db(peak[0]), db(peak[1])}},
            {"rms_db", QJsonArray{db(std::sqrt(sum[0] / frames)), db(std::sqrt(sum[1] / frames))}}};
}

QString versionLine(const QString& program)
{
    QProcess p;
    p.start(program, {"-version"});
    p.waitForFinished(10000);
    return QString::fromUtf8(p.readAllStandardOutput()).section('\n', 0, 0).trimmed();
}

QString projectPath(const char* name) { return QString("%1/projekte/%2.schneidi").arg(g_dir, name); }
QString referencePath(const char* name) { return QString("%1/referenz/%2.flac").arg(g_dir, name); }

// Renders the case's project and compares it with its reference; false = differs (reported)
bool check(const Case& c, const QString& tmp, QJsonObject* entry = nullptr)
{
    const QString wav = QString("%1/%2.wav").arg(tmp, c.name);
    qint64 ms = 0;
    QString error;
    if (!CHECK(renderWav(projectPath(c.name), wav, &ms, &error))) {
        std::printf("       %s: %s\n", c.name, qPrintable(error));
        return false;
    }
    if (const QString keep = qEnvironmentVariable("AUDIO_REFERENCE_KEEP"); !keep.isEmpty()) {
        QDir().mkpath(keep);
        QFile::remove(keep + "/" + c.name + ".wav");
        QFile::copy(wav, keep + "/" + c.name + ".wav");
    }
    const std::vector<float> got = decode(wav), ref = decode(referencePath(c.name));
    const Diff d = compare(got, ref);
    if (entry) {
        *entry = levels(ref);
        (*entry)["render_ms"] = ms;
    }
    // A silent reference proves nothing (the first cover.mp3 was 48 ms long and the case compared silence)
    float refPeak = 0;
    for (float v : ref) refPeak = std::max(refPeak, std::abs(v));
    if (!CHECK(refPeak > 1e-3f)) std::printf("       %s: reference without sound\n", c.name);
    const bool ok = !ref.empty() && d.frames == d.refFrames && d.maxDiff <= kExact;
    std::printf("  %-18s %6.3f s  max. Abweichung %.2e  Differenz %7.1f dB  %5lld ms%s\n", c.name,
                double(d.frames) / kRate, d.maxDiff, d.diffDb, ms, ok ? "" : "  <- ANDERS");
    if (!ok) {
        std::printf("       %s\n", c.what);
        if (d.frames != d.refFrames)
            std::printf("       Länge %lld Samples, Referenz %lld\n", qlonglong(d.frames), qlonglong(d.refFrames));
        else
            std::printf("       größte Abweichung bei %.4f s (Bild %.1f bei 25 fps), Kanal %s\n", double(d.maxAt) / kRate,
                        double(d.maxAt) / kRate * 25, d.channel ? "rechts" : "links");
    }
    return CHECK(ok);
}

int writeReferences(const QStringList& only, const QString& tmp)
{
    if (!CHECK(makeSources())) return Check::result();
    QDir().mkpath(g_dir + "/projekte");
    QDir().mkpath(g_dir + "/referenz");
    QJsonObject manifest;
    if (QFile f(g_dir + "/manifest.json"); f.open(QIODevice::ReadOnly))
        manifest = QJsonDocument::fromJson(f.readAll()).object();
    QJsonObject entries = manifest.value("cases").toObject();
    for (const Case& c : cases()) {
        if (!only.isEmpty() && !only.contains(c.name)) continue;
        QString error;
        if (!CHECK(ProjectFile::save(c.make(), projectPath(c.name), &error))) {
            std::printf("       %s: %s\n", c.name, qPrintable(error));
            continue;
        }
        const QString wav = QString("%1/%2-ref.wav").arg(tmp, c.name);
        qint64 ms = 0;
        if (!CHECK(renderWav(projectPath(c.name), wav, &ms, &error)) || !CHECK(toFlac(wav, referencePath(c.name)))) {
            std::printf("       %s: %s\n", c.name, qPrintable(error));
            continue;
        }
        // A second render must give the same samples – otherwise the path itself is not deterministic
        QJsonObject entry;
        check(c, tmp, &entry);
        entry["what"] = c.what;
        entries[c.name] = entry;
    }
    manifest["cases"] = entries;
    manifest["written"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    manifest["host"] = QHostInfo::localHostName();
    manifest["mlt"] = QString(mlt_version_get_string());
    manifest["ffmpeg"] = versionLine("ffmpeg");
    manifest["format"] = "WAV export 48 kHz stereo 24 bit through Exporter, stored as FLAC";
    QFile f(g_dir + "/manifest.json");
    if (CHECK(f.open(QIODevice::WriteOnly))) f.write(QJsonDocument(manifest).toJson());
    return Check::result();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    // The references are the MLT sound path's (above); the own engine (the default) is checked against the same
    // projects by test_export_audio and test_audio_mixer
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_AUDIO")) qputenv("SCHNEIDI_AUDIO", "mlt");
    QApplication app(argc, argv);
    Check::initApp("audio_reference");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    g_dir = qEnvironmentVariable("AUDIO_REFERENCE_DIR", SCHNEIDI_AUDIO_REFERENCE);
    const QString mode = qEnvironmentVariable("AUDIO_REFERENCE");
    const bool write = mode.startsWith("write");
    if (g_dir.isEmpty() || (!write && !QFileInfo::exists(g_dir + "/manifest.json")))
        return Check::skip("keine Ton-Referenzen (docs/testdaten/ton-referenz, AUDIO_REFERENCE=write legt sie an)");
    g_dir = QDir(g_dir).absolutePath();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initMlt();
    if (write) {
        const QString list = mode.section(':', 1);
        return writeReferences(list.isEmpty() ? QStringList{} : list.split(','), tmp.path());
    }
    for (const Case& c : cases()) {
        if (!QFileInfo::exists(projectPath(c.name))) {
            std::printf("  %-18s fehlt (AUDIO_REFERENCE=write:%s)\n", c.name, c.name);
            CHECK(false);
            continue;
        }
        check(c, tmp.path());
    }
    return Check::result();
}
