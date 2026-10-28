// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/HardwareEncoders.h"

#include "core/I18n.h"
#include "engine/Bundle.h"

#include <QDir>
#include <QHash>
#include <QProcess>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
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
};

State& state()
{
    static State s;
    return s;
}

QString cacheKey(const QString& codec, QSize size)
{
    return QString("%1 %2x%3").arg(codec).arg(size.width()).arg(size.height());
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

QStringList argumentsFor(const QString& codec, QSize size, int crf, const QString& device)
{
    QStringList args{"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error"};
    const bool vaapi = codec.endsWith("_vaapi");
    if (vaapi) args << "-vaapi_device" << device;
    args << "-f" << "lavfi" << "-i"
         << QString("color=black:s=%1x%2:r=25").arg(std::max(16, size.width())).arg(std::max(16, size.height()))
         << "-frames:v" << "3";
    if (vaapi) args << "-vf" << "format=nv12,hwupload";
    else args << "-pix_fmt" << pixelFormat(codec);
    args << "-c:v" << codec;
    for (const auto& [key, value] : codecOptions(codec, crf)) args << QString("-%1:v").arg(key) << value;
    args << "-f" << "null" << "-";
    return args;
}

// Runs test encodes in parallel (blocking), returns which worked
QVector<bool> runTests(const QVector<QStringList>& argLists)
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
        if (!works && Bundle::verboseLogging())
            qInfo("Hardware encoder test failed: %s", p->readAll().trimmed().constData());
        ok << works;
    }
    return ok;
}

void runProbe()
{
    State& st = state();
    // NVENC and Quick Sync: both formats at once
    QVector<QString> codecs;
    for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Qsv})
        for (const char* format : {"h264", "h265"}) codecs << codecName(e, format);
    QVector<QStringList> args;
    for (const QString& c : codecs) args << argumentsFor(c, kProbeSize, 22, {});
    const QVector<bool> ok = runTests(args);
    QHash<QString, int> results;
    for (int i = 0; i < codecs.size(); ++i) results[cacheKey(codecs[i], kProbeSize)] = ok[i] ? 1 : -1;

    // VAAPI: the first render device where H.264 or H.265 works (with several cards e.g. NVIDIA on renderD128 and
    // the Intel/AMD chip on renderD129)
    QString chosen;
    const QString h264 = codecName(VideoEncoder::Vaapi, "h264"), hevc = codecName(VideoEncoder::Vaapi, "h265");
    results[cacheKey(h264, kProbeSize)] = results[cacheKey(hevc, kProbeSize)] = -1;
    for (const QString& device : vaapiDevices()) {
        const QVector<bool> v = runTests({argumentsFor(h264, kProbeSize, 22, device), argumentsFor(hevc, kProbeSize, 22, device)});
        if (v[0] || v[1]) {
            results[cacheKey(h264, kProbeSize)] = v[0] ? 1 : -1;
            results[cacheKey(hevc, kProbeSize)] = v[1] ? 1 : -1;
            chosen = device;
            break;
        }
    }

    QStringList working;
    for (auto it = results.cbegin(); it != results.cend(); ++it)
        if (it.value() > 0) working << it.key().section(' ', 0, 0);
    qInfo("Hardware encoders: %s", working.isEmpty() ? "none" : qPrintable(working.join(", ")));

    {
        std::lock_guard lock(st.mutex);
        for (auto it = results.cbegin(); it != results.cend(); ++it) st.results.insert(it.key(), it.value());
        if (!chosen.isEmpty()) st.vaapiDevice = chosen;
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
    const QString base = QLatin1String(f.id) == QLatin1String("h265") ? "hevc" : "h264";
    switch (e) {
    case VideoEncoder::Nvenc: return base + "_nvenc";
    case VideoEncoder::Vaapi: return base + "_vaapi";
    case VideoEncoder::Qsv: return base + "_qsv";
    case VideoEncoder::Auto:
    case VideoEncoder::Software: break;
    }
    return softwareCodec(format);
}

QString softwareCodec(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (!*f.videoCodec) return {};
    const auto have = [](const char* name) { return avcodec_find_encoder_by_name(name) != nullptr; };
    if (have(f.videoCodec)) return f.videoCodec;
    // Free replacements (BSD): the GPL-free build has no x264/x265
    static const struct {
        const char* codec;
        const char* replacement;
    } kFree[] = {{"libx264", "libopenh264"}, {"libx265", "libkvazaar"}, {"libsvtav1", "libaom-av1"}};
    for (const auto& r : kFree)
        if (qstrcmp(f.videoCodec, r.codec) == 0 && have(r.replacement)) return r.replacement;
    return {};
}

bool formatAvailable(const QString& format)
{
    const RenderFormatInfo& f = renderFormat(format);
    if (QLatin1String(f.id) != format) return false;
    if (*f.audioCodec && !avcodec_find_encoder_by_name(renderFormat(format).audioCodec)) return false;
    if (!*f.videoCodec || !softwareCodec(format).isEmpty()) return true;
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

QStringList proxyVideoArguments()
{
    if (avcodec_find_encoder_by_name("libx264"))
        return {"-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-tune", "fastdecode"};
    // GPL-free build: OpenH264 at a bit rate that suits the proxy size (at most 960 px wide, see ProxyManager)
    return {"-c:v", "libopenh264", "-rc_mode", "quality", "-b:v", "6M", "-allow_skip_frames", "0"};
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

QString pixelFormat(const QString& codec)
{
    switch (encoderOfCodec(codec)) {
    case VideoEncoder::Vaapi: return "vaapi"; // MLT converts to NV12 and uploads to the device
    case VideoEncoder::Qsv: return "nv12";
    default: return "yuv420p";
    }
}

QStringList testArguments(const QString& codec, QSize size, int crf)
{
    return argumentsFor(codec, size, crf, vaapiDevice());
}

int cachedResult(const QString& codec, QSize size)
{
    State& st = state();
    std::lock_guard lock(st.mutex);
    return st.results.value(cacheKey(codec, size), 0);
}

void remember(const QString& codec, QSize size, bool works)
{
    State& st = state();
    std::lock_guard lock(st.mutex);
    st.results[cacheKey(codec, size)] = works ? 1 : -1;
}

bool test(const QString& codec, QSize size, int crf)
{
    if (const int r = cachedResult(codec, size)) return r > 0;
    if (codec.endsWith("_vaapi") && vaapiDevice().isEmpty()) return false;
    const bool works = runTests({testArguments(codec, size, crf)}).value(0);
    remember(codec, size, works);
    return works;
}

QVector<VideoEncoder> available(const QString& format)
{
    if (!renderFormat(format).hardware) return {};
    ensureProbed();
    QVector<VideoEncoder> list;
    for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Qsv, VideoEncoder::Vaapi})
        if (cachedResult(codecName(e, format), kProbeSize) > 0) list << e;
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

Choice resolve(VideoEncoder wanted, const QString& format)
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
        }
        return c;
    }
    if (list.contains(wanted)) {
        c.fallback = c.codec;
        c.codec = codecName(wanted, format);
    } else {
        c.codec = software;
        c.note = T("%1 ist auf diesem Rechner nicht verfügbar, mit Software gerendert.").arg(videoEncoderLabel(wanted));
    }
    return c;
}

void resetForTests()
{
    State& st = state();
    std::unique_lock lock(st.mutex);
    st.done.wait(lock, [&] { return st.probe != ProbeState::Running; });
    st.probe = ProbeState::Idle;
    st.results.clear();
    st.vaapiDevice.clear();
}

} // namespace HardwareEncoders
