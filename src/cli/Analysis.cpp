// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: commands that look at the material without changing a project – shared parts (opening media,
// several files in parallel, regions), probe and extensions. Audio: AnalysisAudio.cpp, picture: AnalysisVideo.cpp
#include "cli/AnalysisDetail.h"
#include "cli/Live.h"

#include "core/I18n.h"
#include "engine/BarDetect.h"
#include "engine/Background.h"
#include "engine/Engine.h"
#include "engine/Extensions.h"
#include "engine/FaceDetect.h"
#include "engine/HardwareEncoders.h"
#include "engine/FrameGaps.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <vector>

namespace Cli::detail {

namespace {
// Project format read once for several files looked at in parallel (perFile), instead of each thread opening
// the project (or the running app's)
const ProjectFormat* g_projectFormat = nullptr;
} // namespace

MediaSource openMedia(const QJsonObject& a)
{
    const QString path = a["file"].toString();
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
    MediaSource m;
    if (a.contains("project") && g_projectFormat) {
        m.format = *g_projectFormat;
    } else if (a.contains("project")) {
        Session s(a["project"].toString());
        m.format = s.format();
    } else {
        m.clip = detectClipFormat(path);
        const ClipFormat& cf = m.clip;
        if (cf.ok) {
            m.format.rate = cf.suggested;
            m.format.width = cf.width & ~1;
            m.format.height = cf.height & ~1;
        }
    }
    m.info = Engine::probe(m.format, path);
    if (m.info.length <= 0 || (!m.info.hasVideo && !m.info.hasAudio)) fail("BAD_MEDIA", unreadableMessage(path));
    return m;
}

ProjectFormat pictureFormat(const MediaSource& m)
{
    ProjectFormat f = m.format;
    if (!m.info.hasVideo || m.info.isImage) return f;
    // Without a project openMedia detected it already (and took its size)
    const ClipFormat cf = m.clip.ok ? m.clip : detectClipFormat(m.info.path);
    if (cf.ok) {
        f.width = cf.width;
        f.height = cf.height;
    }
    return f;
}

// Audio tracks of `tl`: "A3", "A2,A5", numbers, track names or the audio stream names of their clips
// ("Discord,Mikrofon", as `probe` lists them); empty = none given
QVector<int> audioTracksOf(const Project& p, const Timeline& tl, const QJsonValue& list)
{
    static const QRegularExpression trackRef("^[AaVv]?\\d+$");
    QVector<int> out;
    for (QString t : list.toVariant().toString().split(',', Qt::SkipEmptyParts)) {
        t = t.trimmed();
        int index = -1;
        if (trackRef.match(t).hasMatch()) {
            TrackKind k;
            index = parseTrack(t, &k);
            if (k != TrackKind::Audio || index >= tl.audio.size()) fail("BAD_ARGUMENT", "no audio track " + t);
        }
        QStringList names;
        for (int i = 0; i < tl.audio.size(); ++i) {
            const Track& track = tl.audio[i];
            QStringList own{track.name};
            for (const Clip& c : track.clips)
                if (const MediaInfo* m = p.mediaInfo(c.mediaPath)) own << m->audioStreamName(c.audioStream);
            own.removeAll(QString());
            own.removeDuplicates();
            if (index < 0 && own.contains(t, Qt::CaseInsensitive)) index = i;
            if (!own.isEmpty()) names << QString("A%1 = %2").arg(i + 1).arg(own.join("/"));
        }
        if (index < 0)
            fail("BAD_ARGUMENT", QString("no audio track '%1' (tracks: %2)").arg(t, names.isEmpty() ? "none named" : names.join(", ")));
        if (!out.contains(index)) out << index;
    }
    return out;
}

QString trackLabel(const Project& p, const Timeline& tl, int audioTrack)
{
    QStringList names;
    const Track& t = tl.audio[audioTrack];
    if (!t.name.isEmpty()) names << t.name;
    else
        for (const Clip& c : t.clips)
            if (const MediaInfo* m = c.mediaPath.isEmpty() ? nullptr : p.mediaInfo(c.mediaPath);
                m && m->audioStreamCount() > 1 && !names.contains(m->audioStreamName(c.audioStream)))
                names << m->audioStreamName(c.audioStream);
    const QString ref = QString("A%1").arg(audioTrack + 1);
    return names.isEmpty() ? ref : ref + " " + names.join("/");
}

QVector<int> audioStreamsOf(const MediaInfo& info, const QString& list)
{
    static const QRegularExpression number("^\\d+$");
    const int count = info.audioStreamCount();
    QVector<int> out;
    for (QString s : list.split(',', Qt::SkipEmptyParts)) {
        s = s.trimmed();
        int index = -1;
        if (number.match(s).hasMatch()) {
            index = s.toInt();
            if (index >= count) index = -1;
        } else {
            for (int i = 0; i < count && index < 0; ++i)
                if (info.audioStreamName(i).compare(s, Qt::CaseInsensitive) == 0) index = i;
        }
        if (index < 0) {
            QStringList all;
            for (int i = 0; i < count; ++i) all << QString("%1 = %2").arg(i).arg(info.audioStreamName(i));
            fail("BAD_ARGUMENT", QString("no audio stream '%1' in %2 (streams: %3)").arg(s, info.path, all.join(", ")));
        }
        requirePlayableStream(info, index);
        if (!out.contains(index)) out << index;
    }
    if (out.isEmpty()) fail("BAD_ARGUMENT", "audio_stream is empty");
    return out;
}

void requirePlayableStream(const MediaInfo& info, int stream)
{
    const int count = info.audioStreamCount();
    if (stream < 0 || stream >= count)
        fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s); stream counts from 0").arg(count));
    if (!info.audioStreamPlayable(stream))
        fail("BAD_ARGUMENT", QString("audio stream %1 (%2) of %3 cannot be played: no decoder here for its codec, or it "
                                     "holds no sound")
                                 .arg(stream)
                                 .arg(info.audioStreamName(stream), info.path));
}

// "x,y,w,h" in pixels of the full picture, or fractions of it when all four are <= 1 ("0.75,0,0.25,0.3")
QRect cropRect(const QString& spec, QSize full)
{
    const QStringList parts = spec.split(',');
    QVector<double> v;
    for (const QString& p : parts) {
        bool ok = false;
        v << QLocale::c().toDouble(p.trimmed(), &ok);
        if (!ok) v.clear();
        if (v.isEmpty()) break;
    }
    if (v.size() != 4) fail("BAD_ARGUMENT", "crop must be \"x,y,w,h\" (pixels, or fractions 0..1 of the picture)");
    if (std::all_of(v.begin(), v.end(), [](double x) { return x >= 0 && x <= 1; }) && (v[2] < 1 || v[3] < 1))
        v = {v[0] * full.width(), v[1] * full.height(), v[2] * full.width(), v[3] * full.height()};
    const QRect r = QRect(int(std::lround(v[0])), int(std::lround(v[1])), int(std::lround(v[2])), int(std::lround(v[3])))
                        .intersected(QRect(QPoint(0, 0), full));
    if (r.width() < 4 || r.height() < 4)
        fail("BAD_ARGUMENT", QString("crop is outside the picture (%1x%2)").arg(full.width()).arg(full.height()));
    return r;
}

// One file (`file`) or several (`files`, one result each; a failing file gets its error). Several files are
// looked at in parallel, up to `parallel` at once (opening a file and ffprobe take most of the time, mostly
// waiting; ffmpeg decoding for scenes uses all cores itself).
QJsonObject perFile(const QJsonObject& a, Context& ctx, QJsonObject (*one)(const QJsonObject&, Context&),
                    int parallel)
{
    QStringList files;
    if (a.contains("file")) files << a["file"].toString();
    for (const QJsonValue& v : a["files"].toArray()) files << v.toString();
    if (files.isEmpty()) {
        // The project's media to copy from (a small model in the app's chat sent none): the one given, else the app's
        QString project = a["project"].toString();
        if (project.isEmpty() && !qEnvironmentVariable(Live::ChatServerEnv).isEmpty()) project = Live::AppProject;
        QStringList media;
        if (!project.isEmpty()) {
            try {
                const Session session(project); // not a temporary in the loop head: it died before the loop ran
                for (const MediaInfo& m : session.project.media()) media << m.path;
            } catch (const Error&) {
            }
        }
        fail("BAD_ARGUMENT", "give 'file' (a media file's absolute path; several: 'files')"
                                 + (media.isEmpty() ? QString() : "; the media of the project: " + media.join(", ")));
    }
    if (files.size() == 1 && !a.contains("files")) return one(a, ctx);
    std::optional<ProjectFormat> format;
    if (a.contains("project")) format = Session(a["project"].toString()).format();
    const int n = files.size();
    std::vector<QJsonObject> results(n);
    std::vector<std::atomic<double>> progress(n);
    std::atomic<int> next{0};
    auto work = [&] {
        for (int i; (i = next++) < n;) {
            QJsonObject args = a;
            args.remove("files");
            args["file"] = files[i];
            Context sub;
            sub.progress = [&progress, i](double p) { progress[i] = p; };
            try {
                results[i] = one(args, sub);
            } catch (const Error& e) {
                results[i] = {{"file", files[i]}, {"error", QJsonObject{{"code", e.code}, {"message", e.message}}}};
            } catch (const std::exception& e) {
                results[i] = {{"file", files[i]}, {"error", QJsonObject{{"code", "FAILED"}, {"message", e.what()}}}};
            }
            progress[i] = 1.0;
        }
    };
    g_projectFormat = format ? &*format : nullptr;
    const int threads = std::clamp(Background::threads(QThread::idealThreadCount() / 2), 1, std::min(n, parallel));
    std::atomic<int> running{threads};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            work();
            --running;
        });
    // Progress from this thread only (MCP notifications are written here)
    while (running > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (ctx.progress) {
            double sum = 0;
            for (const std::atomic<double>& p : progress) sum += p;
            ctx.progress(sum / n);
        }
    }
    for (std::thread& t : pool) t.join();
    g_projectFormat = nullptr;
    QJsonArray list;
    for (QJsonObject& r : results) list << r;
    return {{"files", list}};
}

QPair<int, int> frozenFrames(double at, double end, const ProjectFormat& f)
{
    const double fps = f.rate.fps();
    const int from = int(std::lround(at * fps)) + 1;
    return {from, std::max(from + 1, int(std::lround(end * fps)))};
}

QJsonObject stretchJson(const Stutter::Stretch& st, const ProjectFormat& f)
{
    const auto [from, to] = frozenFrames(st.from, st.to, f);
    QJsonObject o = range(from, to, f);
    o["jumps"] = st.jumps;
    o["missing_frames"] = st.missing;
    o["longest_s"] = std::round(st.longest * 1000) / 1000;
    return o;
}

namespace {

// Jumps in the picture's time stamps (stutter): the list (at most 50) and the stretches they form
QJsonObject gapsJson(const QString& path, const ProjectFormat& f)
{
    const std::optional<Stutter::Scan> s = FrameGaps::scan(path);
    if (!s) return {{"checked", false}};
    int missing = 0;
    QJsonArray jumps;
    for (const Stutter::Jump& j : s->jumps) {
        missing += j.missing;
        if (jumps.size() >= 50) continue;
        const auto [from, to] = frozenFrames(j.at, j.end(), f);
        jumps << QJsonObject{{"at_s", std::round(j.at * 1000) / 1000}, {"length_s", std::round(j.length * 1000) / 1000},
                             {"missing_frames", j.missing}, {"from", from}, {"to", to}};
    }
    QJsonArray stretches;
    for (const Stutter::Stretch& st : Stutter::stretches(s->jumps)) stretches << stretchJson(st, f);
    QJsonObject o{{"frame_ms", std::round(s->frameDuration * 1e5) / 100}, {"frames", s->frames},
                  {"jump_count", int(s->jumps.size())}, {"missing_frames", missing},
                  {"missing_s", std::round(missing * s->frameDuration * 1000) / 1000}, {"jumps", jumps},
                  {"stutter", stretches}};
    if (s->jumps.size() > jumps.size()) o["jumps_listed"] = int(jumps.size());
    return o;
}

QJsonObject probeOne(const QJsonObject& a)
{
    const MediaSource m = openMedia(a);
    // Detecting opens the file and runs ffprobe: only once (with a project openMedia did not need it)
    const ClipFormat cf = a.contains("project") ? detectClipFormat(m.info.path) : m.clip;
    QJsonObject o{{"file", m.info.path}, {"frames", m.info.length}, {"seconds", seconds(m.info.length, m.format)},
                  {"duration_tc", tc(m.info.length, m.format)}, {"video", m.info.hasVideo}, {"audio", m.info.hasAudio},
                  {"frames_count_in", formatJson(m.format)["fps"]}};
    if (m.info.isImage) o["image"] = true;
    if (cf.ok) {
        o["width"] = cf.width;
        o["height"] = cf.height;
        o["fps"] = std::round(cf.rate.fps() * 1000) / 1000;
        if (cf.variable) o["variable_frame_rate"] = true;
    }
    if (m.info.hasAudio) {
        QJsonArray streams;
        QJsonArray dead; // streams schneidi cannot play (no decoder here, no sound): listed for their place, never used
        for (int i = 0; i < m.info.audioStreamCount(); ++i) {
            streams << m.info.audioStreamName(i);
            if (!m.info.audioStreamPlayable(i)) dead << i;
        }
        o["audio_streams"] = streams;
        if (!dead.isEmpty()) o["unplayable_audio_streams"] = dead;
    }
    if (a["gaps"].toBool() && m.info.hasVideo && !m.info.isImage) o["gaps"] = gapsJson(m.info.path, m.format);
    if (m.info.hasVideo) {
        const std::optional<QRectF> r = a["bars"].toBool() ? BarDetect::content(m.info.path) : BarDetect::cached(m.info.path);
        if (r) {
            const auto pct = [](double v) { return std::round(v * 1000) / 10; };
            o["content"] = QJsonObject{{"x", pct(r->x())}, {"y", pct(r->y())}, {"w", pct(r->width())}, {"h", pct(r->height())}};
            o["black_bars"] = BarDetect::hasBars(*r);
        }
    }
    return o;
}

QJsonObject probeFile(const QJsonObject& a, Context&) { return probeOne(a); }
} // namespace

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, probeFile); }

QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx)
{
    // "whisper,small" -> items; short model names and aliases (x265) work too
    const auto items = [](const QString& list) {
        QString unknown;
        const QVector<const Extensions::Item*> out = Extensions::parseList(list, &unknown);
        if (!unknown.isEmpty()) fail("NOT_FOUND", "no extension " + unknown + " (see `extensions`)");
        return out;
    };
    QJsonArray done;
    for (const Extensions::Item* i : Extensions::withDependencies(items(a["remove"].toString()))) {
        if (!Extensions::remove(*i)) fail("FAILED", "could not remove " + i->id + " (in use? stop running exports, speech and transcriptions first)");
        done << QJsonObject{{"removed", i->id}};
    }
    const QVector<const Extensions::Item*> install = Extensions::withDependencies(items(a["install"].toString()));
    qint64 total = 0, before = 0;
    for (const Extensions::Item* i : install) total += i->size;
    for (const Extensions::Item* i : install) {
        if (Extensions::isInstalled(*i) && !a["reinstall"].toBool()) {
            before += i->size;
            continue;
        }
        Extensions::Installer installer;
        QEventLoop loop;
        bool ok = false;
        QString message;
        QObject::connect(&installer, &Extensions::Installer::progress, &loop, [&](qint64 received, qint64) {
            if (ctx.progress && total > 0) ctx.progress(double(before + received) / total);
        });
        QObject::connect(&installer, &Extensions::Installer::finished, &loop, [&](bool success, const QString& msg) {
            ok = success;
            message = msg;
            loop.quit();
        });
        QString error;
        if (!installer.start(*i, &error)) fail("DOWNLOAD_FAILED", error);
        loop.exec();
        if (!ok) fail("DOWNLOAD_FAILED", i->id + ": " + message);
        before += i->size;
        done << QJsonObject{{"installed", i->id}};
    }
    QJsonArray list;
    for (const Extensions::Item& i : Extensions::catalog()) {
        QJsonObject o{{"id", i.id}, {"name", T(i.name)}, {"description", T(i.description)},
                      {"download_mb", std::round(i.size / 1e5) / 10}, {"installed", Extensions::isInstalled(i)}};
        list << o;
    }
    QJsonObject out{{"extensions", list}, {"folder", Extensions::rootDir()}};
    // What works now: transcribe (whisper with a model), speak (piper with a voice), x264 (H.264/H.265 with x264/x265,
    // built in or the extension), faces (the face model with a runtime that loads)
    QJsonArray ready;
    if (!Extensions::whisperProgram().isEmpty() && !Extensions::installedWhisperModels().isEmpty()) ready << "transcribe";
    if (!Extensions::piperProgram().isEmpty() && !Extensions::installedVoices().isEmpty()) ready << "speak";
    if (HardwareEncoders::builtIn("libx264") || !HardwareEncoders::externalProgram("libx264").isEmpty()) ready << "x264";
    if (FaceDetect::available()) ready << "faces"; // counted faces in `index`
    out["ready_for"] = ready;
    if (!done.isEmpty()) out["done"] = done;
    if (const QString ffmpeg = Extensions::encoderProgram(); !ffmpeg.isEmpty()) out["encoder_program"] = ffmpeg;
    if (const QString piper = Extensions::piperProgram(); !piper.isEmpty()) out["piper_program"] = piper;
    if (Extensions::find("whisper") == nullptr && !Extensions::whisperProgram().isEmpty())
        out["whisper_program"] = Extensions::whisperProgram(); // own build on PATH
    return out;
}

} // namespace Cli::detail
