#include "engine/video/Hardware.h"

#include <QSettings>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace Video::Hardware {

namespace {

constexpr const char* kSettingKey = "playback/hardwareDecoding";

struct Opened {
    AVBufferRef* context = nullptr; // nullptr = tried, does not work here
};

std::mutex g_mutex;
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
    if (!enabled()) return {};
#if defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA};
#elif defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
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

// Opens the device once; later calls return the same one (or nullptr if it failed). g_mutex held.
AVBufferRef* open(AVHWDeviceType type)
{
    auto [it, fresh] = g_devices.try_emplace(type);
    if (fresh) {
        AVBufferRef* ctx = nullptr;
        // VAAPI: libva's default is the first render node; CUDA: device 0; D3D11VA: default adapter
        const bool ok = av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) >= 0;
        if (ok) it->second.context = ctx;
        qInfo("Video decoding: %s %s", av_hwdevice_get_type_name(type), ok ? "ready" : "not available");
    }
    return it->second.context;
}

} // namespace

Device deviceFor(const AVCodec* codec)
{
    if (!codec) return {};
    const std::vector<AVHWDeviceType> types = candidates();
    if (types.empty()) return {};
    std::lock_guard lock(g_mutex);
    for (AVHWDeviceType type : types) {
        const AVPixelFormat fmt = hwFormat(codec, type);
        if (fmt == AV_PIX_FMT_NONE) continue;
        if (AVBufferRef* ctx = open(type)) return {av_buffer_ref(ctx), type, fmt};
    }
    return {};
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
