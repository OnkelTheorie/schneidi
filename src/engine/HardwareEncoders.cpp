// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/HardwareEncoders.h"

#include "core/I18n.h"
#include "engine/Bundle.h"
#include "engine/ExportWriter.h"
#include "engine/Extensions.h"
#include "engine/Licenses.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

namespace HardwareEncoders {

namespace {

enum class ProbeState { Idle, Running, Done };

struct State {
    std::mutex mutex;
    std::condition_variable done;
    ProbeState probe = ProbeState::Idle;
    QHash<QString, int> results; // "codec WxH" -> 1 works, -1 fails
    QString vaapiDevice;         // render device chosen by the probe (empty = first one found)
    QHash<QString, QString> reasons; // codec -> driverHint() of its failed probe test
};

State& state()
{
    static State s;
    return s;
}

QString cacheKey(const QString& codec, QSize size, bool tenBit = false)
{
    return QString("%1 %2x%3%4").arg(codec).arg(size.width()).arg(size.height()).arg(tenBit ? " 10" : "");
}

// The encoder takes this pixel format (FFmpeg's list; no list = anything)
bool encoderTakes(const QString& codec, const char* pixFmt)
{
    const AVCodec* c = avcodec_find_encoder_by_name(codec.toUtf8().constData());
    if (!c) return false;
    const AVPixelFormat want = av_get_pix_fmt(pixFmt);
    if (want == AV_PIX_FMT_NONE) return true;
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || !list) return true;
    const auto* formats = static_cast<const AVPixelFormat*>(list);
    return std::find(formats, formats + n, want) != formats + n;
}

const QSize kProbeSize(640, 360);
std::atomic<Notifier*> g_notifier{nullptr};
const int kTestTimeoutMs = 15000;

// All VAAPI render devices (Linux only; elsewhere there is no VAAPI)
QStringList vaapiDevices()
{
#ifdef Q_OS_LINUX
    const QDir dri("/dev/dri");
    QStringList list;
    for (const QString& name : dri.entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name))
        list << dri.filePath(name);
    return list;
#else
    return {};
#endif
}

QStringList argumentsFor(const QString& codec, QSize size, int crf, const QString& device, bool tenBit = false)
{
    QStringList args{"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error"};
    const bool vaapi = codec.endsWith("_vaapi");
    if (vaapi) args << "-vaapi_device" << device;
    args << "-f" << "lavfi" << "-i"
         << QString("color=black:s=%1x%2:r=25").arg(std::max(16, size.width())).arg(std::max(16, size.height()))
         << "-frames:v" << "3";
    if (vaapi) args << "-vf" << (tenBit ? "format=p010,hwupload" : "format=nv12,hwupload");
    else args << "-pix_fmt" << pixelFormat(codec, tenBit);
    args << "-c:v" << codec;
    for (const auto& [key, value] : codecOptions(codec, crf)) args << QString("-%1:v").arg(key) << value;
    args << "-f" << "null" << "-";
    return args;
}

// Runs test encodes in parallel (blocking), returns which worked; outputs: FFmpeg's output of each test
QVector<bool> runTests(const QVector<QStringList>& argLists, QStringList* outputs = nullptr)
{
    const QString ffmpeg = Bundle::tool("ffmpeg");
    std::vector<std::unique_ptr<QProcess>> procs;
    for (const QStringList& args : argLists) {
        auto p = std::make_unique<QProcess>();
        p->setProcessChannelMode(QProcess::MergedChannels);
        p->start(ffmpeg, args);
        procs.push_back(std::move(p));
    }
    QVector<bool> ok;
    for (auto& p : procs) {
        bool works = p->waitForStarted(kTestTimeoutMs) && p->waitForFinished(kTestTimeoutMs)
                     && p->exitStatus() == QProcess::NormalExit && p->exitCode() == 0;
        if (p->state() != QProcess::NotRunning) {
            p->kill(); // only our own test process
            p->waitForFinished(2000);
            works = false;
        }
        const QByteArray output = p->readAll().trimmed();
        if (!works && Bundle::verboseLogging()) qInfo("Hardware encoder test failed: %s", output.constData());
        if (outputs) *outputs << QString::fromUtf8(output);
        ok << works;
    }
    return ok;
}

void runProbe()
{
    // The x264/x265 extension's encoder list too (asks its program once, up to a quarter second from a cold disk):
    // the Deliver panel asks for it only after the probe, so not on the GUI thread
    externalProgram("libx264");
    State& st = state();
    // NVENC and Quick Sync: both formats at once
    QVector<QString> codecs;
    for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Qsv})
        for (const char* format : {"h264", "h265"}) codecs << codecName(e, format);
    QVector<QStringList> args;
    for (const QString& c : codecs) args << argumentsFor(c, kProbeSize, 22, {});
    // H.265 10 bit (Main10) separately: older cards encode 8-bit H.265 but not 10 bit
    const QVector<QString> tenBit{codecName(VideoEncoder::Nvenc, "h265-10"), codecName(VideoEncoder::Qsv, "h265-10")};
    for (const QString& c : tenBit) args << argumentsFor(c, kProbeSize, 22, {}, true);
    QStringList outputs;
    const QVector<bool> ok = runTests(args, &outputs);
    QHash<QString, int> results;
    QHash<QString, QString> reasons;
    for (int i = 0; i < tenBit.size(); ++i) results[cacheKey(tenBit[i], kProbeSize, true)] = ok[codecs.size() + i] ? 1 : -1;
    for (int i = 0; i < codecs.size(); ++i) {
        results[cacheKey(codecs[i], kProbeSize)] = ok[i] ? 1 : -1;
        if (!ok[i])
            if (const QString hint = driverHint(outputs.value(i)); !hint.isEmpty()) {
                reasons[codecs[i]] = hint;
                qWarning("%s: %s", qPrintable(codecs[i]), qPrintable(outputs.value(i).simplified()));
            }
    }

    // VAAPI: the first render device where H.264 or H.265 works (with several cards e.g. NVIDIA on renderD128 and
    // the Intel/AMD chip on renderD129)
    QString chosen;
    const QString h264 = codecName(VideoEncoder::Vaapi, "h264"), hevc = codecName(VideoEncoder::Vaapi, "h265");
    results[cacheKey(h264, kProbeSize)] = results[cacheKey(hevc, kProbeSize)] = results[cacheKey(hevc, kProbeSize, true)] = -1;
    for (const QString& device : vaapiDevices()) {
        const QVector<bool> v = runTests({argumentsFor(h264, kProbeSize, 22, device), argumentsFor(hevc, kProbeSize, 22, device),
                                          argumentsFor(hevc, kProbeSize, 22, device, true)});
        if (v[0] || v[1]) {
            results[cacheKey(h264, kProbeSize)] = v[0] ? 1 : -1;
            results[cacheKey(hevc, kProbeSize)] = v[1] ? 1 : -1;
            results[cacheKey(hevc, kProbeSize, true)] = v[2] ? 1 : -1;
            chosen = device;
            break;
        }
    }

    QStringList working;
    for (auto it = results.cbegin(); it != results.cend(); ++it)
        if (it.value() > 0) working << it.key().section(' ', 0, 0) + (it.key().endsWith(" 10") ? " (10 bit)" : "");
    qInfo("Hardware encoders: %s", working.isEmpty() ? "none" : qPrintable(working.join(", ")));

    {
        std::lock_guard lock(st.mutex);
        for (auto it = results.cbegin(); it != results.cend(); ++it) st.results.insert(it.key(), it.value());
        if (!chosen.isEmpty()) st.vaapiDevice = chosen;
        st.reasons = reasons;
        st.probe = ProbeState::Done;
    }
    st.done.notify_all();
    if (Notifier* n = g_notifier.load())
        QMetaObject::invokeMethod(n, [n] { emit n->probed(); }, Qt::QueuedConnection);
}

void ensureProbed()
{
    State& st = state();
    std::unique_lock lock(st.mutex);
    if (st.probe == ProbeState::Idle) {
        st.probe = ProbeState::Running;
        lock.unlock();
        runProbe();
        return;
    }
    st.done.wait(lock, [&] { return st.probe == ProbeState::Done; });
}

} // namespace

QString codecName(VideoEncoder e, const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (!f.hardware) return softwareCodec(format);
    const QString base = QLatin1String(f.id).startsWith(QLatin1String("h265")) ? "hevc" : "h264"; // also h265-10
    switch (e) {
    case VideoEncoder::Nvenc: return base + "_nvenc";
    case VideoEncoder::Vaapi: return base + "_vaapi";
    case VideoEncoder::Qsv: return base + "_qsv";
    case VideoEncoder::Auto:
    case VideoEncoder::Software: break;
    }
    return softwareCodec(format);
}

namespace {

// Free replacements (BSD): the GPL-free build has no x264/x265
const struct {
    const char* codec;
    const char* replacement;
} kFree[] = {{"libx264", "libopenh264"}, {"libx265", "libkvazaar"}, {"libsvtav1", "libaom-av1"}};

// Encoders an ffmpeg program lists (`ffmpeg -encoders`), cached per file and modification time
QStringList programEncoders(const QString& program)
{
    static std::mutex mutex;
    static QHash<QString, QStringList> cache;
    const QFileInfo info(program);
    const QString key = info.absoluteFilePath() + '|' + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.constFind(key); it != cache.cend()) return *it;
    }
    QProcess p;
    p.start(program, {"-hide_banner", "-nostdin", "-encoders"});
    QStringList list;
    if (p.waitForStarted(10000) && p.waitForFinished(20000) && p.exitStatus() == QProcess::NormalExit) {
        // " V....D libx264              libx264 H.264 / AVC …"
        for (const QByteArray& line : p.readAllStandardOutput().split('\n')) {
            const QList<QByteArray> words = line.simplified().split(' ');
            if (words.size() >= 2 && words[0].size() == 6) list << QString::fromUtf8(words[1]);
        }
    } else if (p.state() != QProcess::NotRunning) {
        p.kill(); // our own process
        p.waitForFinished(2000);
    }
    if (list.isEmpty()) qWarning("Encoder extension %s lists no encoders", qPrintable(program));
    std::lock_guard lock(mutex);
    cache.insert(key, list);
    return list;
}

} // namespace

bool builtIn(const QString& codec)
{
    if (codec.isEmpty()) return false;
    const QStringList ignored = qEnvironmentVariable("SCHNEIDI_IGNORE_BUILTIN").split(',', Qt::SkipEmptyParts);
    return !ignored.contains(codec) && avcodec_find_encoder_by_name(codec.toUtf8().constData()) != nullptr;
}

QString externalProgram(const QString& codec)
{
    if ((codec != "libx264" && codec != "libx265") || builtIn(codec)) return {};
    const QString program = Extensions::encoderProgram();
    if (program.isEmpty() || !programEncoders(program).contains(codec)) return {};
    return program;
}

QString freeReplacement(const QString& codec)
{
    // The GPL-free build writes H.264 with schneidi's own encoder (about x264's quality at the same size, OpenH264
    // needs about twice the size); every other build with x264 missing keeps OpenH264
    if (codec == QLatin1String("libx264") && Licenses::builtWithoutGpl()) return ExportWriter::kOwnH264;
    for (const auto& r : kFree)
        if (codec == QLatin1String(r.codec) && builtIn(r.replacement)) return r.replacement;
    return {};
}

// The free replacement for a format, only if it takes the format's pixel format (kvazaar: 8 bit only, so no H.265
// 10 bit in the GPL-free build without a Main10 graphics card)
static QString freeReplacementFor(const RenderFormatInfo& f)
{
    const QString r = freeReplacement(f.videoCodec);
    return r.isEmpty() || encoderTakes(r, f.pixFmt) ? r : QString();
}

bool isFreeReplacement(const QString& codec)
{
    return codec == "libopenh264" || codec == "libkvazaar";
}

QString betterQualityHint(const QString& codec)
{
    if (!isFreeReplacement(codec)) return {};
    const bool h264 = codec == "libopenh264";
    const Extensions::Item* item = Extensions::find("x264");
    QString text = T("%1 wird mit %2 kodiert (keine Grafikkarte, die kodiert): für die gleiche Qualität etwa doppelt so "
                     "große Dateien.")
                       .arg(h264 ? "H.264" : "H.265", h264 ? "OpenH264" : "kvazaar");
    if (item)
        text += ' ' + T("Bessere Qualität mit der Erweiterung x264/x265 (%1 MB): `schneidi-cli extensions --install x264` "
                        "(MCP: extensions mit install \"x264\").")
                          .arg(qRound(item->size / 1e6));
    return text;
}

bool needsBetterEncoder(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (!f.hardware || QLatin1String(f.id) != format) return false;
    return available(format).isEmpty() && isFreeReplacement(softwareCodec(format));
}

QString softwareCodec(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (!*f.videoCodec) return {};
    if (ExportWriter::isOwnH264(f.videoCodec) || builtIn(f.videoCodec) || !externalProgram(f.videoCodec).isEmpty())
        return f.videoCodec;
    return freeReplacementFor(f);
}

bool formatAvailable(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (QLatin1String(f.id) != format) return false;
    if (*f.audioCodec && !builtIn(renderFormat(format).audioCodec)) return false;
    // (the extension last: asking its program takes a moment, and the panel's format list asks at start)
    if (!*f.videoCodec || ExportWriter::isOwnH264(f.videoCodec) || builtIn(f.videoCodec) || !freeReplacementFor(f).isEmpty()
        || !externalProgram(f.videoCodec).isEmpty())
        return true;
    return f.hardware && probed() && !available(format).isEmpty();
}

QVector<QPair<QString, QString>> softwareOptions(const QString& codec, int crf, QSize size, double fps)
{
    if (crf <= 0) return {};
    if (codec == "libopenh264") {
        // No constant quality mode: a bit rate target from the picture size, like x264 at that CRF gives for typical
        // material (about 0.1 bit per pixel at 22, doubling every 6 steps), in "quality" rate control
        const double pixels = double(std::max(16, size.width())) * std::max(16, size.height()) * std::max(1.0, fps);
        const double bitsPerPixel = 0.1 * std::pow(2.0, (22 - crf) / 6.0);
        const qint64 kbit = std::max<qint64>(200, qint64(pixels * bitsPerPixel / 1000));
        return {{"rc_mode", "quality"}, {"b", QString::number(kbit) + "k"}, {"allow_skip_frames", "0"},
                {"profile", "high"}, {"coder", "cabac"}};
    }
    if (codec == "libkvazaar") // constant QP (kvazaar has no CRF); QP ≈ x265's CRF + 2 for similar sizes
        return {{"kvazaar-params", QString("preset=fast,qp=%1").arg(std::clamp(crf + 2, 1, 51))}};
    return {};
}

QStringList proxyVideoArguments(QSize size, bool full)
{
    if (builtIn("libx264"))
        return {"-c:v", "libx264", "-preset", "veryfast", "-crf", full ? "21" : "23", "-tune", "fastdecode"};
    // GPL-free build: OpenH264 at a bit rate that suits the proxy size: 6 Mbit/s at 960x540, growing with the pixels
    // (720p ~10, 1080p ~24, 4K ~95 at most 100), a quarter more at full resolution
    const double pixels = std::max(1.0, double(size.width()) * size.height());
    const double mbit = std::clamp(6.0 * pixels / (960.0 * 540.0) * (full ? 1.25 : 1.0), 6.0, 100.0);
    return {"-c:v", "libopenh264", "-rc_mode", "quality", "-b:v", QString("%1M").arg(int(std::lround(mbit))),
            "-allow_skip_frames", "0"};
}

VideoEncoder encoderOfCodec(const QString& codec)
{
    if (codec.endsWith("_nvenc")) return VideoEncoder::Nvenc;
    if (codec.endsWith("_vaapi")) return VideoEncoder::Vaapi;
    if (codec.endsWith("_qsv")) return VideoEncoder::Qsv;
    return VideoEncoder::Software;
}

bool isHardwareCodec(const QString& codec)
{
    return encoderOfCodec(codec) != VideoEncoder::Software;
}

QString vaapiDevice()
{
    State& st = state();
    {
        std::lock_guard lock(st.mutex);
        if (!st.vaapiDevice.isEmpty()) return st.vaapiDevice;
    }
    return vaapiDevices().value(0);
}

QVector<QPair<QString, QString>> codecOptions(const QString& codec, int crf)
{
    const auto q = [](int v) { return QString::number(std::clamp(v, 1, 51)); };
    switch (encoderOfCodec(codec)) {
    case VideoEncoder::Nvenc:
        // Variable bit rate with a constant quality target (like CRF), no bit rate cap; p5 = slower, better quality
        return {{"preset", "p5"}, {"tune", "hq"}, {"rc", "vbr"}, {"cq", q(crf + 1)}, {"b", "0"}};
    case VideoEncoder::Vaapi:
        // Constant QP works on every VAAPI driver (Intel and AMD); a bit above the CRF for similar sizes
        return {{"qp", q(crf + 2)}};
    case VideoEncoder::Qsv:
        // Intelligent constant quality (ICQ): global_quality without a bit rate
        return {{"preset", "medium"}, {"global_quality", q(crf + 2)}, {"b", "0"}};
    case VideoEncoder::Auto:
    case VideoEncoder::Software: break;
    }
    return {};
}

namespace {
QVector<QPair<QString, QString>> averageRate(int kbit)
{
    const auto k = [](qint64 v) { return QString::number(std::max<qint64>(1, v)) + "k"; };
    return {{"b", k(kbit)}, {"maxrate", k(qint64(kbit) * 3 / 2)}, {"bufsize", k(qint64(kbit) * 2)}};
}
} // namespace

QVector<QPair<QString, QString>> bitrateOptions(const QString& codec, int kbit)
{
    if (kbit <= 0) return {};
    QVector<QPair<QString, QString>> o;
    switch (encoderOfCodec(codec)) {
    case VideoEncoder::Nvenc: o = {{"preset", "p5"}, {"tune", "hq"}, {"rc", "vbr"}}; break;
    case VideoEncoder::Vaapi: o = {{"rc_mode", "VBR"}}; break;
    case VideoEncoder::Qsv: o = {{"preset", "medium"}}; break; // b < maxrate = VBR
    case VideoEncoder::Auto:
    case VideoEncoder::Software: return {};
    }
    return o + averageRate(kbit);
}

QVector<QPair<QString, QString>> softwareBitrateOptions(const QString& codec, int kbit)
{
    if (kbit <= 0) return {};
    const QString k = QString::number(kbit) + "k";
    if (codec == "libopenh264") return {{"rc_mode", "bitrate"}, {"b", k}, {"allow_skip_frames", "0"}, {"profile", "high"}, {"coder", "cabac"}};
    if (codec == "libkvazaar") return {{"b", k}, {"kvazaar-params", "preset=fast"}};
    if (codec == "libsvtav1") return {{"b", k}}; // VBR; its peak rate needs CRF mode
    return averageRate(kbit); // libx264, libx265, libvpx-vp9
}

QString pixelFormat(const QString& codec, bool tenBit)
{
    switch (encoderOfCodec(codec)) {
    case VideoEncoder::Vaapi: return "vaapi"; // the writer converts to NV12 (10 bit: P010) and uploads to the device
    case VideoEncoder::Qsv: return tenBit ? "p010le" : "nv12";
    default: return tenBit ? "p010le" : "yuv420p"; // NVENC: P010 selects Main10
    }
}

QStringList testArguments(const QString& codec, QSize size, int crf, bool tenBit)
{
    return argumentsFor(codec, size, crf, vaapiDevice(), tenBit);
}

int cachedResult(const QString& codec, QSize size, bool tenBit)
{
    State& st = state();
    std::lock_guard lock(st.mutex);
    return st.results.value(cacheKey(codec, size, tenBit), 0);
}

void remember(const QString& codec, QSize size, bool works, bool tenBit)
{
    State& st = state();
    std::lock_guard lock(st.mutex);
    st.results[cacheKey(codec, size, tenBit)] = works ? 1 : -1;
}

bool test(const QString& codec, QSize size, int crf, bool tenBit)
{
    if (const int r = cachedResult(codec, size, tenBit)) return r > 0;
    if (codec.endsWith("_vaapi") && vaapiDevice().isEmpty()) return false;
    const bool works = runTests({testArguments(codec, size, crf, tenBit)}).value(0);
    remember(codec, size, works, tenBit);
    return works;
}

bool tenBitFormat(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    return QLatin1String(f.id) == format && QByteArray(f.pixFmt).contains("10");
}

QVector<VideoEncoder> available(const QString& format)
{
    if (!renderFormat(format).hardware) return {};
    ensureProbed();
    QVector<VideoEncoder> list;
    const bool tenBit = tenBitFormat(format); // Main10 has a probe result of its own
    for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Qsv, VideoEncoder::Vaapi})
        if (cachedResult(codecName(e, format), kProbeSize, tenBit) > 0) list << e;
    return list;
}

bool probed()
{
    State& st = state();
    std::lock_guard lock(st.mutex);
    return st.probe == ProbeState::Done;
}

void probeInBackground()
{
    notifier(); // create it in this (GUI) thread
    State& st = state();
    {
        std::lock_guard lock(st.mutex);
        if (st.probe != ProbeState::Idle) return;
        st.probe = ProbeState::Running;
    }
    QThreadPool::globalInstance()->start([] { runProbe(); });
}

Notifier* notifier()
{
    // Created by the first caller, which must be the GUI thread (DeliverPanel); lives until the program ends
    if (!g_notifier.load()) g_notifier = new Notifier;
    return g_notifier.load();
}

// The choice between hardware and software encoders (below)
static Choice resolveHardware(VideoEncoder wanted, const QString& format);

Choice resolve(VideoEncoder wanted, const QString& format)
{
    Choice c = resolveHardware(wanted, format);
    // x264/x265 of the extension runs as a separate program: if it fails, the free replacement takes over
    if (c.fallback.isEmpty() && !externalProgram(c.codec).isEmpty()) c.fallback = freeReplacement(c.codec);
    return c;
}

static Choice resolveHardware(VideoEncoder wanted, const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    Choice c;
    c.codec = softwareCodec(format);
    // No software encoder in this FFmpeg: keep the format's name, the export then reports it as not available
    const QString software = c.codec.isEmpty() ? QString(f.videoCodec) : c.codec;
    if (!f.hardware || (wanted == VideoEncoder::Software && !c.codec.isEmpty())) {
        c.codec = software;
        return c;
    }
    const QVector<VideoEncoder> list = available(format);
    if (wanted == VideoEncoder::Auto || wanted == VideoEncoder::Software) { // Software without one: the graphics card
        if (!list.isEmpty()) {
            c.fallback = c.codec;
            c.codec = codecName(list.first(), format);
        } else {
            c.codec = software;
            // An NVIDIA card whose driver is too old for NVENC: say so (a driver update makes exports much faster)
            if (const QString reason = unavailableReason(VideoEncoder::Nvenc); !reason.isEmpty())
                c.note = T("%1 Mit Software gerendert.").arg(reason);
        }
        return c;
    }
    if (list.contains(wanted)) {
        c.fallback = c.codec;
        c.codec = codecName(wanted, format);
    } else {
        c.codec = software;
        const QString reason = unavailableReason(wanted);
        c.note = reason.isEmpty()
                     ? T("%1 ist auf diesem Rechner nicht verfügbar, mit Software gerendert.").arg(videoEncoderLabel(wanted))
                     : T("%1 Mit Software gerendert.").arg(reason);
    }
    return c;
}

QString driverHint(const QString& ffmpegOutput)
{
    // FFmpeg's nvenc.c: "Driver does not support the required nvenc API version. Required: 13.0 Found: 12.2" and
    // "The minimum required Nvidia driver for nvenc is 570.0 or newer"
    static const QRegularExpression api(QStringLiteral("nvenc API version\\.?\\s*Required:\\s*(\\d+\\.\\d+)\\s*Found:\\s*(\\d+\\.\\d+)"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression driver(QStringLiteral("minimum required Nvidia driver for nvenc is\\s*([0-9][0-9.]*[0-9])"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch a = api.match(ffmpegOutput), d = driver.match(ffmpegOutput);
    if (!a.hasMatch() && !d.hasMatch()) return {};
    if (a.hasMatch() && d.hasMatch())
        return T("NVIDIA-Treiber zu alt für NVENC (gefunden: API %1, nötig: API %2, Treiber %3 oder neuer) – bitte "
                 "aktualisieren.")
            .arg(a.captured(2), a.captured(1), d.captured(1));
    if (a.hasMatch())
        return T("NVIDIA-Treiber zu alt für NVENC (gefunden: API %1, nötig: API %2) – bitte aktualisieren.")
            .arg(a.captured(2), a.captured(1));
    return T("NVIDIA-Treiber zu alt für NVENC (nötig: Treiber %1 oder neuer) – bitte aktualisieren.").arg(d.captured(1));
}

QString unavailableReason(VideoEncoder e)
{
    if (e != VideoEncoder::Nvenc && e != VideoEncoder::Qsv && e != VideoEncoder::Vaapi) return {};
    State& st = state();
    std::lock_guard lock(st.mutex);
    for (const char* format : {"h264", "h265"})
        if (const QString r = st.reasons.value(codecName(e, format)); !r.isEmpty()) return r;
    return {};
}

void resetForTests()
{
    State& st = state();
    std::unique_lock lock(st.mutex);
    st.done.wait(lock, [&] { return st.probe != ProbeState::Running; });
    st.probe = ProbeState::Idle;
    st.results.clear();
    st.vaapiDevice.clear();
    st.reasons.clear();
}

void fakeProbeForTests(const QStringList& working, const QString& nvencReason)
{
    resetForTests();
    State& st = state();
    std::lock_guard lock(st.mutex);
    if (!nvencReason.isEmpty())
        for (const char* format : {"h264", "h265"}) st.reasons[codecName(VideoEncoder::Nvenc, format)] = nvencReason;
    for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Qsv, VideoEncoder::Vaapi})
        for (const char* format : {"h264", "h265"}) {
            const QString codec = codecName(e, format);
            st.results[cacheKey(codec, kProbeSize)] = working.contains(codec) ? 1 : -1;
            if (QLatin1String(format) == QLatin1String("h265"))
                st.results[cacheKey(codec, kProbeSize, true)] = working.contains(codec + ":10") ? 1 : -1;
        }
    st.probe = ProbeState::Done;
}

} // namespace HardwareEncoders
