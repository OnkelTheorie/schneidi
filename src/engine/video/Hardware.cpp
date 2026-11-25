// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Hardware.h"
#include "engine/CpuPath.h"
#include "engine/GpuAdapter.h"

#include <QSettings>
#include <QThreadPool>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#ifdef Q_OS_ANDROID
#include <libavcodec/jni.h>
#endif
}

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#endif

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#if defined(Q_OS_LINUX) && !defined(__ANDROID__) // bionic declares setpriority differently and has no NVIDIA decoder
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// NVIDIA's libnvcuvid lowers "the process" whenever it creates a decoder: setpriority(PRIO_PROCESS, getpid(), 10)
// from one of its threads. On Linux that is only the main thread, i.e. the window, and every thread started after it
// inherits nice 10; without rights it never goes back up. The preview then loses against any other load. This
// definition replaces the C library's in the executables (exported there, see CMakeLists.txt) and turns such a call
// from another thread into one for the calling thread. Everything else passes through unchanged.
int setpriority(__priority_which_t which, id_t who, int prio) noexcept
{
    const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
    if (which == PRIO_PROCESS && who != 0 && static_cast<pid_t>(who) == getpid() && self != getpid())
        who = static_cast<id_t>(self);
    return static_cast<int>(syscall(SYS_setpriority, which, who, prio));
}
#endif

namespace Video::Hardware {

namespace {

constexpr const char* kSettingKey = "playback/hardwareDecoding";

struct Opened {
    bool opening = false;           // a thread opens it right now (outside g_mutex), the others wait (g_opened)
    bool done = false;              // tried
    AVBufferRef* context = nullptr; // nullptr after done = does not work here
};

std::mutex g_mutex;
std::condition_variable g_opened;
std::map<AVHWDeviceType, Opened> g_devices; // guarded by g_mutex
std::atomic<int> g_enabled{-1};             // -1 = not read yet

// Device types in order of preference for this system; empty = off
std::vector<AVHWDeviceType> candidates()
{
    const QByteArray env = qgetenv("SCHNEIDI_HWDEC").trimmed().toLower();
    if (!env.isEmpty()) {
        if (env == "off" || env == "0" || env == "none") return {};
        const AVHWDeviceType t = av_hwdevice_find_type_by_name(env.constData());
        if (t != AV_HWDEVICE_TYPE_NONE) return {t};
    }
    if (!enabled() || CpuPath::on()) return {};
#if defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA};
#elif defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#elif defined(Q_OS_ANDROID)
    return {AV_HWDEVICE_TYPE_MEDIACODEC}; // only as "on": decoding goes through mediaCodecDecoder(), no device
#else
    return {AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI};
#endif
}

AVPixelFormat hwFormat(const AVCodec* codec, AVHWDeviceType type)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return AV_PIX_FMT_NONE;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) return cfg->pix_fmt;
    }
}

// Opens the device once; later calls return the same one (or nullptr if it failed). The driver call (CUDA: 100-250
// ms) runs without g_mutex: deviceReady() never waits for it, a second caller for the same type waits for the first.
AVBufferRef* open(AVHWDeviceType type)
{
    std::unique_lock lock(g_mutex);
    Opened& o = g_devices[type]; // std::map: the reference stays valid
    if (o.opening) g_opened.wait(lock, [&o] { return o.done; });
    if (o.done) return o.context;
    o.opening = true;
    lock.unlock();
    AVBufferRef* ctx = nullptr;
    // VAAPI: libva's default is the first render node; CUDA: device 0; D3D11VA: the adapter GpuAdapter chose (the
    // picture engine's card: decoded frames need no trip between two cards), else adapter 0
    QByteArray device;
    if (type == AV_HWDEVICE_TYPE_D3D11VA && GpuAdapter::chosen().index >= 0)
        device = QByteArray::number(GpuAdapter::chosen().index);
    const bool ok = av_hwdevice_ctx_create(&ctx, type, device.isEmpty() ? nullptr : device.constData(), nullptr, 0) >= 0;
    qInfo("Video decoding: %s %s", av_hwdevice_get_type_name(type), ok ? "ready" : "not available");
    lock.lock();
    o.context = ok ? ctx : nullptr;
    o.done = true;
    o.opening = false;
    lock.unlock();
    g_opened.notify_all();
    return ok ? ctx : nullptr;
}

} // namespace

const AVCodec* mediaCodecDecoder(int codecId)
{
#ifdef Q_OS_ANDROID
    const std::vector<AVHWDeviceType> types = candidates();
    if (types.size() != 1 || types.front() != AV_HWDEVICE_TYPE_MEDIACODEC) return nullptr;
    const char* name = nullptr;
    switch (AVCodecID(codecId)) {
    case AV_CODEC_ID_H264: name = "h264_mediacodec"; break;
    case AV_CODEC_ID_HEVC: name = "hevc_mediacodec"; break;
    case AV_CODEC_ID_VP9: name = "vp9_mediacodec"; break;
    case AV_CODEC_ID_AV1: name = "av1_mediacodec"; break;
    default: return nullptr;
    }
    static std::once_flag once;
    static bool jvm = false;
    std::call_once(once, [] {
        JavaVM* vm = QJniEnvironment::javaVM();
        jvm = vm && av_jni_set_java_vm(vm, nullptr) >= 0;
        qInfo("Video decoding: mediacodec %s", jvm ? "ready" : "not available (no Java VM)");
    });
    return jvm ? avcodec_find_decoder_by_name(name) : nullptr;
#else
    Q_UNUSED(codecId);
    return nullptr;
#endif
}

bool deviceReady(const AVCodec* codec)
{
    if (!codec) return false;
    if (mediaCodecDecoder(codec->id)) return true;
    const std::vector<AVHWDeviceType> types = candidates();
    std::lock_guard lock(g_mutex);
    for (AVHWDeviceType type : types) {
        if (hwFormat(codec, type) == AV_PIX_FMT_NONE) continue;
        const auto it = g_devices.find(type);
        if (it != g_devices.end() && it->second.done && it->second.context) return true;
    }
    return false;
}

Device deviceFor(const AVCodec* codec)
{
    if (!codec) return {};
    const std::vector<AVHWDeviceType> types = candidates();
    for (AVHWDeviceType type : types) {
        if (type == AV_HWDEVICE_TYPE_MEDIACODEC) continue; // a decoder of its own (mediaCodecDecoder), no device
        const AVPixelFormat fmt = hwFormat(codec, type);
        if (fmt == AV_PIX_FMT_NONE) continue;
        // The context is never freed while the program runs (g_devices keeps its reference)
        if (AVBufferRef* ctx = open(type)) return {av_buffer_ref(ctx), type, fmt};
    }
    return {};
}

void warmUp()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const std::vector<AVHWDeviceType> types = candidates();
        if (types.empty()) return;
        // The global pool: it is waited for at exit, never a driver call cut off halfway
        QThreadPool::globalInstance()->start([types] {
            for (AVHWDeviceType type : types)
                if (type != AV_HWDEVICE_TYPE_MEDIACODEC && open(type)) break;
        });
    });
}

bool enabled()
{
    int v = g_enabled.load();
    if (v < 0) {
        v = QSettings().value(kSettingKey, true).toBool() ? 1 : 0;
        g_enabled = v;
    }
    return v == 1;
}

void setEnabled(bool on)
{
    QSettings().setValue(kSettingKey, on);
    g_enabled = on ? 1 : 0;
}

} // namespace Video::Hardware
