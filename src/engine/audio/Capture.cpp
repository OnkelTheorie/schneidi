// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/audio/Capture.h"

#include "engine/audio/DeviceDetail.h"
#include "engine/audio/Miniaudio.h"
#include "engine/audio/Wait.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <numbers>
#include <thread>

namespace Audio {

namespace {

// RIFF/WAVE header for 24-bit PCM; sizes are filled in when the recording stops. An odd data chunk (mono, an odd
// number of samples) is followed by a pad byte that the RIFF size counts (RIFF chunks are word aligned)
QByteArray wavHeader(int rate, int channels, quint32 dataBytes)
{
    QByteArray h(44, '\0');
    auto u32 = [&](int at, quint32 v) { qToLittleEndian<quint32>(v, h.data() + at); };
    auto u16 = [&](int at, quint16 v) { qToLittleEndian<quint16>(v, h.data() + at); };
    const int block = channels * 3;
    memcpy(h.data(), "RIFF", 4);
    u32(4, 36 + dataBytes + (dataBytes & 1));
    memcpy(h.data() + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1); // PCM
    u16(22, quint16(channels));
    u32(24, quint32(rate));
    u32(28, quint32(rate * block));
    u16(32, quint16(block));
    u16(34, 24);
    memcpy(h.data() + 36, "data", 4);
    u32(40, dataBytes);
    return h;
}

} // namespace

struct Capture::Impl {
    Config config;
    QString path, name;
    QFile file;
    int latency = 0;

    // Single producer (device thread), single consumer (writer thread); counters in samples, never wrap in practice
    std::vector<float> ring;
    std::atomic<long long> written{0}, read{0};
    std::atomic<long long> frames{0}, dropped{0};
    std::atomic<float> peak{0.f};
    std::atomic<bool> stopInput{false}, stopWriter{false};
    std::atomic<bool> inCallback{false};
    bool writeFailed = false, stopped = false;
    long long dataBytes = 0;

    std::thread writer, timer;
    ma_context maContext{};
    ma_device maDevice{};
    bool context = false, device = false;

    // Device thread: no locks, no allocation
    void push(const float* in, int count)
    {
        const int ch = config.channels;
        const long long n = (long long)count * ch;
        const long long w = written.load(std::memory_order_relaxed);
        const long long r = read.load(std::memory_order_acquire);
        const long long size = (long long)ring.size();
        float p = 0.f;
        for (long long i = 0; i < n; ++i) p = std::max(p, std::abs(in[i]));
        float old = peak.load(std::memory_order_relaxed);
        while (p > old && !peak.compare_exchange_weak(old, p, std::memory_order_relaxed)) {}
        frames.fetch_add(count, std::memory_order_relaxed);
        if (w + n - r > size) { // writer far behind: the block is lost (counted), the file stays continuous
            dropped.fetch_add(count, std::memory_order_relaxed);
            return;
        }
        for (long long i = 0; i < n; ++i) ring[size_t((w + i) % size)] = in[i];
        written.store(w + n, std::memory_order_release);
    }

    // Writer thread: ring -> file
    void drain()
    {
        std::vector<char> pcm;
        const long long size = (long long)ring.size();
        const long long r = read.load(std::memory_order_relaxed);
        const long long w = written.load(std::memory_order_acquire);
        if (w == r) return;
        pcm.resize(size_t(w - r) * 3);
        char* o = pcm.data();
        for (long long i = r; i < w; ++i) {
            const float s = std::clamp(ring[size_t(i % size)], -1.f, 1.f);
            const qint32 v = qint32(std::lround(double(s) * 8388607.0));
            *o++ = char(v & 0xff);
            *o++ = char((v >> 8) & 0xff);
            *o++ = char((v >> 16) & 0xff);
        }
        read.store(w, std::memory_order_release);
        // Only what reached the file counts (a full disk: the header must not claim more than is there)
        const qint64 n = file.write(pcm.data(), qint64(pcm.size()));
        if (n != qint64(pcm.size())) writeFailed = true;
        if (n > 0) dataBytes += n;
    }

    void writerLoop()
    {
        while (!stopWriter.load()) {
            drain();
            sleepFor(std::chrono::milliseconds(20));
        }
        drain();
    }

    // Stand-in input paced by a timer: silence or a sine
    void timerLoop(double hz)
    {
        const int period = std::max(64, config.buffer / 2);
        std::vector<float> block(size_t(period) * config.channels);
        const auto step = std::chrono::nanoseconds(int64_t(period) * 1000000000 / config.rate);
        auto due = std::chrono::steady_clock::now() + step; // a block arrives once it has been "heard"
        long long n = 0;
        while (!stopInput.load()) {
            sleepUntil(due);
            if (stopInput.load()) break;
            for (int i = 0; i < period; ++i, ++n) {
                const float s = hz > 0 ? float(0.25 * std::sin(2 * std::numbers::pi * hz * double(n) / config.rate))
                                       : 0.f;
                for (int c = 0; c < config.channels; ++c) block[size_t(i) * config.channels + c] = s;
            }
            push(block.data(), period);
            due += step;
        }
    }

    bool openCard(QString* error)
    {
        if (!initContext(config.backend, &maContext)) {
            if (error) *error = QString("miniaudio: kein Audio-System (%1)").arg(config.backend);
            return false;
        }
        context = true;
        ma_device_config c = ma_device_config_init(ma_device_type_capture);
        ma_device_id chosen{};
        if (!config.input.isEmpty()) {
            for (const auto& [input, id] : listDevices(&maContext, true))
                if (input.id == config.input) {
                    chosen = id;
                    c.capture.pDeviceID = &chosen;
                }
            if (!c.capture.pDeviceID) qWarning("Audio: Eingang %s fehlt – Systemstandard", qPrintable(config.input));
        }
        c.capture.format = ma_format_f32;
        c.capture.channels = ma_uint32(config.channels);
        c.sampleRate = ma_uint32(config.rate);
        c.periodSizeInFrames = ma_uint32(std::max(64, config.buffer / 2));
        c.periods = 2;
        c.dataCallback = [](ma_device* dev, void*, const void* in, ma_uint32 count) {
            auto* self = static_cast<Impl*>(dev->pUserData);
            self->inCallback.store(true);
            if (!self->stopInput.load() && in) self->push(static_cast<const float*>(in), int(count));
            self->inCallback.store(false, std::memory_order_release);
        };
        c.pUserData = this;
        if (const ma_result r = ma_device_init(&maContext, &c, &maDevice); r != MA_SUCCESS) {
            if (error) *error = QString("miniaudio: Eingang lässt sich nicht öffnen (%1)").arg(ma_result_description(r));
            return false;
        }
        device = true;
        const auto& p = maDevice.capture;
        latency = int(std::lround(double(p.internalPeriodSizeInFrames) * p.internalPeriods * config.rate
                                  / std::max<ma_uint32>(1, p.internalSampleRate)));
        name = QString("%1 (%2, %3 Hz)").arg(QString::fromUtf8(p.name), ma_get_backend_name(maContext.backend))
                   .arg(p.internalSampleRate);
        if (const ma_result r = ma_device_start(&maDevice); r != MA_SUCCESS) {
            if (error) *error = QString("miniaudio: Eingang startet nicht (%1)").arg(ma_result_description(r));
            return false;
        }
        return true;
    }

    void stopInputs()
    {
        stopInput.store(true);
        while (inCallback.load()) std::this_thread::yield();
        if (timer.joinable()) timer.join();
        if (device) {
            ma_device_stop(&maDevice);
            ma_device_uninit(&maDevice);
            device = false;
        }
        if (context) {
            ma_context_uninit(&maContext);
            context = false;
        }
    }

    ~Impl()
    {
        stopInputs();
        stopWriter.store(true);
        if (writer.joinable()) writer.join();
    }
};

Capture::Capture(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}

Capture::~Capture()
{
    stop();
}

std::unique_ptr<Capture> Capture::start(const Config& config, const QString& path, QString* error)
{
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->config.channels = std::clamp(config.channels, 1, 8);
    impl->config.rate = std::clamp(config.rate, 8000, 192000);
    impl->path = path;
    impl->ring.assign(size_t(impl->config.rate) * impl->config.channels * 8, 0.f); // 8 s against a stalled disk
    impl->file.setFileName(path);
    if (!impl->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = impl->file.errorString();
        return nullptr;
    }
    impl->file.write(wavHeader(impl->config.rate, impl->config.channels, 0));
    Impl* raw = impl.get();

    const QString stand = qEnvironmentVariable("SCHNEIDI_AUDIO_INPUT");
    if (stand == "null" || stand.startsWith("sine:") || plainName(config.backend) == "null") {
        const double hz = stand.startsWith("sine:") ? stand.mid(5).toDouble() : 0.0;
        impl->latency = std::max(64, impl->config.buffer / 2);
        impl->name = hz > 0 ? QString("Testton %1 Hz").arg(hz) : QString("Stille");
        impl->timer = std::thread([raw, hz] { raw->timerLoop(hz); });
    } else if (!impl->openCard(error)) {
        impl->file.close();
        QFile::remove(path);
        return nullptr;
    }
    impl->writer = std::thread([raw] { raw->writerLoop(); });
    return std::unique_ptr<Capture>(new Capture(std::move(impl)));
}

bool Capture::stop(QString* error)
{
    if (d->stopped) return !d->writeFailed;
    d->stopped = true;
    d->stopInputs();
    d->stopWriter.store(true);
    if (d->writer.joinable()) d->writer.join();
    // Whole sample frames only (a write cut short by a full disk can end inside one). WAV sizes are 32 bit: a
    // longer recording keeps the maximum (readers take the rest from the file length)
    const long long block = 3LL * d->config.channels;
    const long long whole = d->dataBytes - d->dataBytes % block;
    const long long limit = 0xffffffffLL - 37;
    const quint32 bytes = quint32(std::min(whole, limit - limit % block));
    if (whole <= limit) {
        // Cut a partial frame off, then the pad byte after an odd data chunk
        if (whole != d->dataBytes && !d->file.resize(44 + whole)) d->writeFailed = true;
        if (whole % 2 && (!d->file.seek(44 + whole) || d->file.write("\0", 1) != 1)) d->writeFailed = true;
    }
    if (!d->file.seek(0) || d->file.write(wavHeader(d->config.rate, d->config.channels, bytes)) != 44)
        d->writeFailed = true;
    d->file.close();
    if (d->writeFailed && error) *error = d->file.errorString();
    return !d->writeFailed;
}

long long Capture::frames() const { return d->frames.load(std::memory_order_relaxed); }
long long Capture::dropped() const { return d->dropped.load(std::memory_order_relaxed); }
float Capture::takePeak() { return d->peak.exchange(0.f, std::memory_order_relaxed); }
int Capture::latency() const { return d->latency; }
int Capture::rate() const { return d->config.rate; }
int Capture::channels() const { return d->config.channels; }
QString Capture::name() const { return d->name; }
QString Capture::path() const { return d->path; }

std::vector<Device::Output> Capture::inputs(const QString& backend)
{
    std::vector<Device::Output> out;
    if (plainName(backend) == "null") return out;
    ma_context context;
    if (!initContext(backend, &context)) return out;
    for (auto& [input, id] : listDevices(&context, true)) out.push_back(std::move(input));
    ma_context_uninit(&context);
    return out;
}

} // namespace Audio
