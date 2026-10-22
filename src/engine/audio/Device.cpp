#include "engine/audio/Device.h"

#include "engine/audio/Miniaudio.h"

#include <QFile>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

// Silence or a file, paced by a timer: one block per period, the block counts as heard when it is handed over
class TimerDevice final : public Device {
public:
    TimerDevice(const Config& config, Callback callback, const QString& path)
        : m_period(std::max(64, config.buffer / 2)), m_channels(config.channels), m_rate(config.rate),
          m_callback(std::move(callback))
    {
        if (!path.isEmpty()) {
            m_file.setFileName(path);
            if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered))
                qWarning("Audio: Datei %s lässt sich nicht schreiben", qPrintable(path));
        }
        m_thread = std::thread([this] { run(); });
    }
    ~TimerDevice() override
    {
        m_stop = true;
        m_thread.join();
    }
    int latency() const override { return m_period; }
    QString name() const override { return m_file.isOpen() ? "Datei " + m_file.fileName() : QString("Stille"); }

private:
    void run()
    {
        std::vector<float> block(size_t(m_period) * m_channels);
        std::vector<int16_t> pcm(block.size());
        const auto period = std::chrono::nanoseconds(int64_t(m_period) * 1000000000 / m_rate);
        auto due = std::chrono::steady_clock::now();
        while (!m_stop) {
            m_callback(block.data(), m_period);
            if (m_file.isOpen()) {
                for (size_t i = 0; i < block.size(); ++i)
                    pcm[i] = int16_t(std::lround(std::clamp(block[i], -1.f, 1.f) * 32767.f));
                m_file.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * sizeof(int16_t)));
            }
            due += period; // absolute deadlines: no drift
            std::this_thread::sleep_until(due);
        }
    }

    int m_period, m_channels, m_rate;
    Callback m_callback;
    QFile m_file;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

class SoundCard final : public Device {
public:
    ~SoundCard() override
    {
        if (m_device) ma_device_uninit(&m_maDevice);
        if (m_context) ma_context_uninit(&m_maContext);
    }

    bool open(const Config& config, Callback callback, QString* error)
    {
        m_callback = std::move(callback);
        ma_backend chosen{};
        bool pick = false;
        if (!config.backend.isEmpty()) {
            for (int b = 0; b < ma_backend_null; ++b)
                if (plainName(ma_get_backend_name(ma_backend(b))) == plainName(config.backend)) {
                    chosen = ma_backend(b);
                    pick = true;
                }
            if (!pick) qWarning("Audio: Treiber %s unbekannt, automatisch", qPrintable(config.backend));
        }
        if (ma_context_init(pick ? &chosen : nullptr, pick ? 1 : 0, nullptr, &m_maContext) != MA_SUCCESS) {
            if (error) *error = QString("miniaudio: kein Audio-System (%1)").arg(pick ? config.backend : "automatisch");
            return false;
        }
        m_context = true;
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format = ma_format_f32;
        c.playback.channels = ma_uint32(config.channels);
        c.sampleRate = ma_uint32(config.rate);
        c.periodSizeInFrames = ma_uint32(std::max(64, config.buffer / 2));
        c.periods = 2;
        c.dataCallback = [](ma_device* device, void* out, const void*, ma_uint32 frames) {
            static_cast<SoundCard*>(device->pUserData)->m_callback(static_cast<float*>(out), int(frames));
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

private:
    Callback m_callback;
    ma_context m_maContext{};
    ma_device m_maDevice{};
    bool m_context = false, m_device = false;
    int m_latency = 0;
    QString m_name;
};

} // namespace

std::unique_ptr<Device> Device::open(const Config& config, Callback callback, QString* error)
{
    const QString stand = qEnvironmentVariable("SCHNEIDI_AUDIO_DEVICE");
    if (stand == "null") return std::make_unique<TimerDevice>(config, std::move(callback), QString());
    if (stand.startsWith("file:")) return std::make_unique<TimerDevice>(config, std::move(callback), stand.mid(5));
    if (plainName(config.backend) != "null") {
        auto card = std::make_unique<SoundCard>();
        QString why;
        if (card->open(config, callback, &why)) return card;
        card.reset();
        qWarning("Audio: %s – spiele stumm", qPrintable(why));
        if (error) *error = why;
    }
    return std::make_unique<TimerDevice>(config, std::move(callback), QString());
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

} // namespace Audio
