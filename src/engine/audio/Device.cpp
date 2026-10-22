#include "engine/audio/Device.h"

#include "engine/audio/Miniaudio.h"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <thread>
#include <vector>

namespace Audio {

namespace {

// "DirectSound" == "directsound", "Core Audio" == "coreaudio" (the setting keeps SDL's names of the MLT path)
QString plainName(const QString& name)
{
    QString out;
    for (const QChar c : name)
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

// Silence or a file, paced by a timer: one block per period, the block counts as heard when it is handed over.
// stallMs > 0: stops calling back after that long (a vanished device, tests).
class TimerDevice final : public Device {
public:
    TimerDevice(const Config& config, Callback callback, const QString& path, bool fallback = false, int stallMs = 0)
        : m_period(std::max(64, config.buffer / 2)), m_channels(config.channels), m_rate(config.rate),
          m_stallMs(stallMs), m_fallback(fallback), m_callback(std::move(callback))
    {
        if (!path.isEmpty()) {
            m_file.setFileName(path);
            if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered))
                qWarning("Audio: Datei %s lässt sich nicht schreiben", qPrintable(path));
        }
        m_thread = std::thread([this] { run(); });
    }
    ~TimerDevice() override { detach(); }
    void detach() override
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }
    int latency() const override { return m_period; }
    QString name() const override { return m_file.isOpen() ? "Datei " + m_file.fileName() : QString("Stille"); }
    bool fallback() const override { return m_fallback; }

private:
    void run()
    {
        std::vector<float> block(size_t(m_period) * m_channels);
        std::vector<int16_t> pcm(block.size());
        const auto period = std::chrono::nanoseconds(int64_t(m_period) * 1000000000 / m_rate);
        const auto begin = std::chrono::steady_clock::now();
        auto due = begin;
        while (!m_stop) {
            if (m_stallMs <= 0 || due - begin < std::chrono::milliseconds(m_stallMs)) {
                m_callback(block.data(), m_period);
                if (m_file.isOpen()) {
                    for (size_t i = 0; i < block.size(); ++i)
                        pcm[i] = int16_t(std::lround(std::clamp(block[i], -1.f, 1.f) * 32767.f));
                    m_file.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * sizeof(int16_t)));
                }
            }
            due += period; // absolute deadlines: no drift
            std::this_thread::sleep_until(due);
        }
    }

    int m_period, m_channels, m_rate, m_stallMs;
    bool m_fallback;
    Callback m_callback;
    QFile m_file;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

// A miniaudio context of the chosen backend ("" = miniaudio's order)
bool initContext(const QString& backend, ma_context* context)
{
    ma_backend chosen{};
    bool pick = false;
    if (!backend.isEmpty()) {
        for (int b = 0; b < ma_backend_null; ++b)
            if (plainName(ma_get_backend_name(ma_backend(b))) == plainName(backend)) {
                chosen = ma_backend(b);
                pick = true;
            }
        if (!pick) qWarning("Audio: Treiber %s unbekannt, automatisch", qPrintable(backend));
    }
    return ma_context_init(pick ? &chosen : nullptr, pick ? 1 : 0, nullptr, context) == MA_SUCCESS;
}

// The id of a device as text that stays the same across restarts (the setting keeps it)
QString idText(ma_backend backend, const ma_device_id& id)
{
    auto text = [](const char* s, size_t size) {
        return QString::fromUtf8(s, qsizetype(std::find(s, s + size, '\0') - s));
    };
    switch (backend) {
    case ma_backend_pulseaudio: return text(id.pulse, sizeof id.pulse);
    case ma_backend_alsa: return text(id.alsa, sizeof id.alsa);
    case ma_backend_coreaudio: return text(id.coreaudio, sizeof id.coreaudio);
    case ma_backend_sndio: return text(id.sndio, sizeof id.sndio);
    case ma_backend_audio4: return text(id.audio4, sizeof id.audio4);
    case ma_backend_oss: return text(id.oss, sizeof id.oss);
    case ma_backend_wasapi: {
        QString out;
        for (size_t i = 0; i < std::size(id.wasapi) && id.wasapi[i]; ++i) out += QChar(char16_t(id.wasapi[i]));
        return out;
    }
    case ma_backend_dsound:
        return QByteArray(reinterpret_cast<const char*>(id.dsound), sizeof id.dsound).toHex();
    case ma_backend_jack: return QString::number(id.jack);
    case ma_backend_winmm: return QString::number(id.winmm);
    default: return QByteArray(reinterpret_cast<const char*>(&id), sizeof id).toHex();
    }
}

// Playback devices of an open context
std::vector<std::pair<Device::Output, ma_device_id>> listOutputs(ma_context* context)
{
    std::vector<std::pair<Device::Output, ma_device_id>> out;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return out;
    for (ma_uint32 i = 0; i < count; ++i)
        out.push_back({{idText(context->backend, infos[i].id), QString::fromUtf8(infos[i].name),
                        bool(infos[i].isDefault)},
                       infos[i].id});
    return out;
}

class SoundCard final : public Device {
public:
    ~SoundCard() override
    {
        detach();
        if (m_device) {
            // ma_device_uninit alone does not wake miniaudio's PulseAudio thread (0.11.22): it waits for an event
            // forever when the stream lost its output and nothing arrives any more. ma_device_stop wakes it.
            ma_device_stop(&m_maDevice);
            ma_device_uninit(&m_maDevice);
        }
        if (m_context) ma_context_uninit(&m_maContext);
    }

    bool open(const Config& config, Callback callback, QString* error)
    {
        m_callback = std::move(callback);
        if (!initContext(config.backend, &m_maContext)) {
            if (error)
                *error = QString("miniaudio: kein Audio-System (%1)")
                             .arg(config.backend.isEmpty() ? QString("automatisch") : config.backend);
            return false;
        }
        m_context = true;
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        ma_device_id chosen{};
        if (!config.output.isEmpty()) {
            for (const auto& [output, id] : listOutputs(&m_maContext))
                if (output.id == config.output) {
                    chosen = id;
                    c.playback.pDeviceID = &chosen;
                }
            if (!c.playback.pDeviceID) {
                qWarning("Audio: Ausgabe %s fehlt – Systemstandard", qPrintable(config.output));
                m_fallback = true;
            }
        }
        c.playback.format = ma_format_f32;
        c.playback.channels = ma_uint32(config.channels);
        c.sampleRate = ma_uint32(config.rate);
        c.periodSizeInFrames = ma_uint32(std::max(64, config.buffer / 2));
        c.periods = 2;
        c.dataCallback = [](ma_device* device, void* out, const void*, ma_uint32 frames) {
            auto* self = static_cast<SoundCard*>(device->pUserData);
            self->m_inCallback.store(true); // seq_cst with detach(): one of both sees the other
            if (self->m_detached.load())
                std::fill_n(static_cast<float*>(out), size_t(frames) * device->playback.channels, 0.f);
            else
                self->m_callback(static_cast<float*>(out), int(frames));
            self->m_inCallback.store(false, std::memory_order_release);
        };
        c.notificationCallback = [](const ma_device_notification* n) {
            if (n->type == ma_device_notification_type_rerouted)
                static_cast<SoundCard*>(n->pDevice->pUserData)->m_reroutes.fetch_add(1, std::memory_order_relaxed);
        };
        c.pUserData = this;
        if (const ma_result r = ma_device_init(&m_maContext, &c, &m_maDevice); r != MA_SUCCESS) {
            if (error) *error = QString("miniaudio: Gerät lässt sich nicht öffnen (%1)").arg(ma_result_description(r));
            return false;
        }
        m_device = true;
        if (const ma_result r = ma_device_start(&m_maDevice); r != MA_SUCCESS) {
            if (error) *error = QString("miniaudio: Gerät startet nicht (%1)").arg(ma_result_description(r));
            return false;
        }
        const auto& p = m_maDevice.playback;
        m_latency = int(std::lround(double(p.internalPeriodSizeInFrames) * p.internalPeriods * config.rate
                                    / std::max<ma_uint32>(1, m_maDevice.playback.internalSampleRate)));
        m_name = QString("%1 (%2, %3 Hz, Puffer %4×%5)")
                     .arg(QString::fromUtf8(p.name), ma_get_backend_name(m_maContext.backend))
                     .arg(p.internalSampleRate)
                     .arg(p.internalPeriods)
                     .arg(p.internalPeriodSizeInFrames);
        return true;
    }
    int latency() const override { return m_latency; }
    QString name() const override { return m_name; }
    bool fallback() const override { return m_fallback; }
    int reroutes() const override { return m_reroutes.load(std::memory_order_relaxed); }
    void detach() override
    {
        m_detached.store(true);
        while (m_inCallback.load()) std::this_thread::yield(); // a block being filled right now: microseconds
    }

private:
    Callback m_callback;
    std::atomic<bool> m_detached{false}, m_inCallback{false};
    ma_context m_maContext{};
    ma_device m_maDevice{};
    bool m_context = false, m_device = false, m_fallback = false;
    std::atomic<int> m_reroutes{0};
    int m_latency = 0;
    QString m_name;
};

} // namespace

std::unique_ptr<Device> Device::open(const Config& config, Callback callback, QString* error)
{
    const QString stand = qEnvironmentVariable("SCHNEIDI_AUDIO_DEVICE");
    if (stand == "null") return std::make_unique<TimerDevice>(config, std::move(callback), QString());
    if (stand.startsWith("null:stall=")) {
        static std::atomic<bool> stalled{false};
        const int ms = stalled.exchange(true) ? 0 : stand.mid(11).toInt();
        return std::make_unique<TimerDevice>(config, std::move(callback), QString(), false, ms);
    }
    if (stand.startsWith("file:")) return std::make_unique<TimerDevice>(config, std::move(callback), stand.mid(5));
    if (plainName(config.backend) != "null") {
        auto card = std::make_unique<SoundCard>();
        QString why;
        if (card->open(config, callback, &why)) return card;
        card.reset();
        qWarning("Audio: %s – spiele stumm", qPrintable(why));
        if (error) *error = why;
        return std::make_unique<TimerDevice>(config, std::move(callback), QString(), true);
    }
    return std::make_unique<TimerDevice>(config, std::move(callback), QString());
}

bool Device::available(const Config& config)
{
    if (qEnvironmentVariableIsSet("SCHNEIDI_AUDIO_DEVICE") || plainName(config.backend) == "null") return true;
    ma_context context;
    if (!initContext(config.backend, &context)) return false;
    const auto list = listOutputs(&context);
    ma_context_uninit(&context);
    return std::any_of(list.begin(), list.end(),
                       [&](const auto& o) { return config.output.isEmpty() || o.first.id == config.output; });
}

QStringList Device::backends()
{
    QStringList out{QString()};
    ma_backend list[MA_BACKEND_COUNT];
    size_t n = 0;
    if (ma_get_enabled_backends(list, MA_BACKEND_COUNT, &n) != MA_SUCCESS) return out;
    for (size_t i = 0; i < n; ++i)
        if (list[i] != ma_backend_null && list[i] != ma_backend_custom) out << plainName(ma_get_backend_name(list[i]));
    return out;
}

std::vector<Device::Output> Device::outputs(const QString& backend)
{
    std::vector<Output> out;
    if (plainName(backend) == "null") return out;
    ma_context context;
    if (!initContext(backend, &context)) return out;
    for (auto& [output, id] : listOutputs(&context)) out.push_back(std::move(output));
    ma_context_uninit(&context);
    return out;
}

} // namespace Audio
