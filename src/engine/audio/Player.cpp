#include "engine/audio/Player.h"

#include "core/Loudness.h"
#include "core/TruePeak.h"
#include "engine/audio/Device.h"
#include "engine/audio/Mixer.h"
#include "engine/audio/Reader.h"
#include "engine/audio/Wait.h"

#include <QHash>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace Audio {

namespace {

constexpr int kChannels = Mixer::kChannels;
constexpr int kBlock = 256;     // samples per ring block
constexpr int kRing = 256;      // blocks (1.4 s): room; the feeder keeps only `lead` samples ahead
constexpr int kFade = 240;      // 5 ms: fade out at a stop/jump, fade in after it, mute ramp
constexpr int kXfade = 480;     // 10 ms crossfade to a newly built mix
constexpr int kChunk = 8192;    // backwards: mixed forward in chunks of this size, played reversed
constexpr int kOverlap = 256;   // crossfade between reversed chunks
constexpr int kMaxTape = 4;     // faster than this: silent
constexpr int64_t kHoldNs = 300'000'000;
constexpr int64_t kSilentNs = 250'000'000; // a device that took nothing for this long (at least) has vanished
constexpr int64_t kRetryNs = 2'000'000'000; // on a fallback: look for the asked output this often
constexpr int kSilentTries = 3;             // reopened this often without it calling back: silence until it is back

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Block {
    float data[kBlock * kChannels]; // interleaved
    uint32_t gen = 0;
};

// Peaks of a stretch of sound and where it ends (clock position)
struct LevelEntry {
    uint32_t gen = 0;
    double end = 0;
    std::vector<float> peaks;
};

} // namespace

struct Player::Impl {
    Options opt;

    // Ring: the feeder writes blocks, the callback reads them
    std::vector<Block> ring = std::vector<Block>(kRing);
    std::atomic<uint64_t> written{0}, consumed{0};

    // Control (play/pause/setTimeline, any thread) under `mutex`; the callback only reads the atomics
    std::mutex mutex;
    std::condition_variable wake;
    struct Command {
        uint32_t gen = 0;
        bool playing = false;
        double start = 0; // clock position the generation starts at
        double speed = 1;
    } cmd;
    double pausedAt = 0;
    int64_t heardMax = 0; // clock of the generation never runs back (position())
    std::optional<std::pair<Timeline, FrameRate>> pending;
    QHash<QString, int> projectLengths; // setFileLengths()
    bool deviceDirty = false;
    bool quit = false;
    std::atomic<uint32_t> gen{0};          // generation the callback plays
    std::atomic<int64_t> holdUntil{0};     // nothing of the generation is handed over before (ns)
    std::atomic<bool> muted{false};
    std::atomic<SharedLoudness*> loudness{nullptr};

    // Clock, published by the callback after every block (seqlock): stream = all samples handed to the device
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> cGen{~0u};
    std::atomic<int64_t> cTime{0}, cTotal{0}, cHanded{0}, cRunT{0}, cRunN{0}, cGapT{0};
    std::atomic<bool> cInRun{false};
    std::atomic<int> latency{0};

    // Callback state (device thread only)
    struct {
        uint32_t gen = ~0u;
        int64_t total = 0;  // stream samples handed over
        int64_t handed = 0; // of them, sound of the generation
        // The generation's current unbroken run in the stream: starts at stream index runT with its sample runN;
        // gapT = where the run before it ended (silence since)
        int64_t runT = 0, runN = 0, gapT = 0;
        bool inRun = false;
        int offset = 0;   // read position in the front block
        int fadeLeft = 0; // old generation still fading out
        bool sounding = false;
        float muteGain = 1.f;
    } cb;
    std::atomic<int> underruns{0}, fadeOuts{0}, mixersBuilt{0}, liveUpdates{0}, reopens{0}, reroutes{0};
    std::atomic<int64_t> lastCallback{0}; // when the device last took samples (ns)
    std::atomic<int64_t> silentLimit{kSilentNs}; // took nothing for this long: vanished (ns, from the buffer)

    // The device: opened by play() and the watchdog, under deviceMutex (taken before `mutex`, never inside it)
    std::mutex deviceMutex;
    std::unique_ptr<Device> device;
    bool forcedSilence = false; // the sound card kept failing: silence until available() says it is back
    int64_t openedAt = 0;       // lastCallback when it was opened: later = it has called back since
    int opened = 0;             // devices opened so far
    QString deviceName, deviceError;

    // Watchdog thread: notices a vanished device, applies new settings, brings the asked output back
    std::thread watchdog;
    std::mutex watchMutex;
    std::condition_variable watchWake;
    bool watchQuit = false;

    std::mutex levelMutex;
    std::deque<LevelEntry> levels;

    std::thread feeder;

    // Feeder thread state
    struct Feed {
        QHash<QString, int> project;  // file lengths the project knows (frames of the project rate)
        QHash<QString, int> lengths; // measured file lengths in frames ("num/den:path")
        std::unique_ptr<Mixer> mixer, old; // old: crossfading out (forward)
        int xfade = 0;
        FrameRate fps;
        uint32_t gen = ~0u;
        double start = 0, speed = 1;
        int64_t next = 0;     // forward: next sample to mix; backward: end of the next chunk (exclusive)
        int64_t produced = 0; // output samples of the generation
        uint64_t genFrom = 0; // ring index of the generation's first block (blocks before it are dropped)
        std::vector<float> rev[kChannels], tail[kChannels], chunk[kChannels], content[kChannels], other[kChannels];
        int revPos = 0, revLen = 0;
        bool haveTail = false;
        TruePeakMeter master;
        std::vector<float> peaks;
    };

    explicit Impl(const Options& o) : opt(o) {}

    void process(float* out, int frames);
    void publishClock(int64_t now);
    double positionLocked();
    Device::Config deviceConfig();
    void openDevice(bool silent = false); // under deviceMutex
    void closeDevice();                   // under deviceMutex
    bool deviceSilent() const;
    void watch();

    void feed();
    void takeTimeline(Feed& f, const Timeline& tl, FrameRate fps, bool playingForward);
    void startGeneration(Feed& f, const Command& c);
    void produce(Feed& f, float* const* out, int n);
    void mixForward(Feed& f, int n, float* const* out);
    void mixBackward(Feed& f, int n, float* const* out);
    void notePeaks(Feed& f);
    // Ahead of the device: at least 50 ms and the asked buffer, and the device's real buffer + 20 ms – a device may
    // take its whole buffer at once (miniaudio's DirectSound: gaps up to 100 ms, then 2 periods of 1440 at once,
    // more than the 2048 asked; measured, docs/dev/ton.md)
    int leadBlocks() const
    {
        const int ahead = std::max({opt.buffer, 2400, latency.load(std::memory_order_relaxed) + opt.rate / 50});
        return (ahead + kBlock - 1) / kBlock;
    }
};

// ---- Device thread ----

void Player::Impl::process(float* out, int frames)
{
    auto& s = cb;
    const uint32_t g = gen.load(std::memory_order_acquire);
    if (g != s.gen) { // new generation: fade out what sounds, count anew
        s.fadeLeft = s.sounding ? kFade : 0;
        if (s.sounding) fadeOuts.fetch_add(1, std::memory_order_relaxed);
        s.gen = g;
        s.handed = s.runT = s.runN = 0;
        s.gapT = s.total;
        s.inRun = false;
    }
    const int64_t until = holdUntil.load(std::memory_order_relaxed);
    const int64_t now = nowNs();
    const bool hold = until != 0 && now < until;
    const float muteTarget = muted.load(std::memory_order_relaxed) ? 0.f : 1.f;
    int i = 0;
    bool sound = false;
    while (i < frames) {
        const uint64_t r = consumed.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire)) break;
        const Block& b = ring[r % kRing];
        int k = 0;
        if (b.gen != g) { // old generation: fade out over what is there, drop the rest
            k = std::min({frames - i, kBlock - cb.offset, s.fadeLeft});
            for (int j = 0; j < k; ++j) {
                const float gain = s.muteGain * float(s.fadeLeft - j) / float(kFade + 1);
                for (int c = 0; c < kChannels; ++c)
                    out[(i + j) * kChannels + c] = b.data[(s.offset + j) * kChannels + c] * gain;
            }
            s.fadeLeft -= k;
            i += k;
            sound = false;
            s.offset = s.fadeLeft > 0 ? s.offset + k : kBlock;
        } else {
            if (hold) break;
            k = std::min(frames - i, kBlock - s.offset);
            if (!s.inRun) {
                s.inRun = true;
                s.runT = s.total + i;
                s.runN = s.handed;
            }
            for (int j = 0; j < k; ++j) {
                const int64_t n = s.handed + j;
                if (s.muteGain != muteTarget)
                    s.muteGain = muteTarget > s.muteGain ? std::min(1.f, s.muteGain + 1.f / kFade)
                                                        : std::max(0.f, s.muteGain - 1.f / kFade);
                const float gain = s.muteGain * (n < kFade ? float(n + 1) / float(kFade + 1) : 1.f);
                for (int c = 0; c < kChannels; ++c)
                    out[(i + j) * kChannels + c] = b.data[(s.offset + j) * kChannels + c] * gain;
            }
            s.handed += k;
            s.offset += k;
            i += k;
            sound = true;
        }
        if (s.offset >= kBlock) {
            s.offset = 0;
            consumed.store(r + 1, std::memory_order_release);
        }
    }
    if (i < frames) {
        std::fill(out + i * kChannels, out + frames * kChannels, 0.f);
        if (s.inRun) { // the sound ran dry (not: a generation that has not started yet)
            s.inRun = false;
            s.gapT = s.total + i;
            underruns.fetch_add(1, std::memory_order_relaxed);
        }
        sound = false;
    }
    s.sounding = sound;
    s.total += frames;
    lastCallback.store(now, std::memory_order_relaxed);
    publishClock(now);
}

void Player::Impl::publishClock(int64_t now)
{
    const uint32_t q = seq.load(std::memory_order_relaxed);
    seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    cGen.store(cb.gen, std::memory_order_relaxed);
    cTime.store(now, std::memory_order_relaxed);
    cTotal.store(cb.total, std::memory_order_relaxed);
    cHanded.store(cb.handed, std::memory_order_relaxed);
    cRunT.store(cb.runT, std::memory_order_relaxed);
    cRunN.store(cb.runN, std::memory_order_relaxed);
    cGapT.store(cb.gapT, std::memory_order_relaxed);
    cInRun.store(cb.inRun, std::memory_order_relaxed);
    seq.store(q + 2, std::memory_order_release);
}

// ---- Clock ----

double Player::Impl::positionLocked()
{
    if (!cmd.playing) return pausedAt;
    uint32_t g;
    int64_t time, total, handed, runT, runN, gapT;
    bool inRun;
    for (;;) {
        const uint32_t q = seq.load(std::memory_order_acquire);
        if (q & 1) continue;
        g = cGen.load(std::memory_order_relaxed);
        time = cTime.load(std::memory_order_relaxed);
        total = cTotal.load(std::memory_order_relaxed);
        handed = cHanded.load(std::memory_order_relaxed);
        runT = cRunT.load(std::memory_order_relaxed);
        runN = cRunN.load(std::memory_order_relaxed);
        gapT = cGapT.load(std::memory_order_relaxed);
        inRun = cInRun.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == q) break;
    }
    if (g != cmd.gen) return cmd.start; // the device has not reached this generation yet
    // Stream sample heard now: the device buffer still holds `latency` samples of what was handed over
    const int64_t elapsed = std::max<int64_t>(0, nowNs() - time) * opt.rate / 1000000000;
    const int64_t heardT = std::min(total, total - latency.load(std::memory_order_relaxed) + elapsed);
    int64_t heard;
    if (inRun)
        heard = heardT >= runT ? runN + (heardT - runT) : heardT >= gapT ? runN : runN - (gapT - heardT);
    else
        heard = heardT >= gapT ? handed : handed - (gapT - heardT);
    heard = std::clamp<int64_t>(heard, 0, handed);
    heardMax = std::max(heardMax, heard);
    return cmd.start + double(heardMax) * cmd.speed;
}

// ---- Feeder thread ----

void Player::Impl::feed()
{
    Feed f;
    for (int c = 0; c < kChannels; ++c) {
        f.rev[c].resize(kChunk + kOverlap);
        f.tail[c].resize(kOverlap);
        f.chunk[c].resize(kChunk + kOverlap);
        f.content[c].resize(kBlock * kMaxTape);
        f.other[c].resize(kBlock * kMaxTape);
    }
    float planar[kChannels][kBlock];
    std::unique_lock<std::mutex> lock(mutex);
    while (!quit) {
        if (pending) {
            auto [tl, fps] = std::move(*pending);
            pending.reset();
            f.project = projectLengths;
            const bool forward = cmd.playing && cmd.speed > 0 && cmd.gen == f.gen;
            lock.unlock();
            takeTimeline(f, tl, fps, forward);
            lock.lock();
            continue;
        }
        const Command c = cmd;
        if (!c.playing || !f.mixer) {
            wake.wait(lock);
            continue;
        }
        if (c.gen != f.gen) startGeneration(f, c);
        // Ahead = blocks of this generation only: the old ones still in the ring are dropped by the callback. Counting
        // them too, the feeder waited after a jump while the callback emptied the ring (an underrun where the wait is
        // coarse: on Windows a plain 2-ms wait takes ~16 ms, Wait.h).
        const uint64_t w = written.load(std::memory_order_relaxed), r = consumed.load(std::memory_order_acquire);
        if (w - std::max(r, f.genFrom) >= uint64_t(leadBlocks()) || w - r >= uint64_t(kRing - 1)) {
            waitFor(wake, lock, std::chrono::milliseconds(2));
            continue;
        }
        lock.unlock();
        float* out[kChannels] = {planar[0], planar[1]};
        produce(f, out, kBlock);
        Block& b = ring[written.load(std::memory_order_relaxed) % kRing];
        for (int i = 0; i < kBlock; ++i)
            for (int ch = 0; ch < kChannels; ++ch) b.data[i * kChannels + ch] = planar[ch][i];
        b.gen = f.gen;
        written.fetch_add(1, std::memory_order_release);
        lock.lock();
    }
}

void Player::Impl::takeTimeline(Feed& f, const Timeline& tl, FrameRate fps, bool playingForward)
{
    if (f.mixer && fps == f.fps && f.mixer->updateMix(tl)) {
        ++liveUpdates;
        return;
    }
    Mixer::Options o;
    o.rate = opt.rate;
    o.fileLength = [&f, fps, rate = opt.rate](const QString& path) {
        if (const int n = f.project.value(path); n > 0) return n;
        const QString key = QString("%1/%2:%3").arg(fps.num).arg(fps.den).arg(path);
        if (const auto it = f.lengths.constFind(key); it != f.lengths.cend()) return *it;
        auto r = Reader::open(path, 0, Reader::Options{rate, kChannels});
        const int n = r ? int(std::ceil(double(r->info().end) * fps.fps() / rate)) : 0;
        f.lengths.insert(key, n);
        return n;
    };
    auto mixer = std::make_unique<Mixer>(tl, fps, o);
    mixer->setMetering(true);
    // Playing forward: crossfade from the running mix (a crossfade under way keeps its start)
    if (playingForward && f.mixer && fps == f.fps) {
        if (!f.old) {
            f.old = std::move(f.mixer);
            f.xfade = 0;
        }
    } else {
        f.old.reset();
    }
    f.mixer = std::move(mixer);
    f.fps = fps;
    ++mixersBuilt;
}

void Player::Impl::startGeneration(Feed& f, const Command& c)
{
    f.gen = c.gen;
    f.start = c.start;
    f.speed = c.speed;
    f.produced = 0;
    f.genFrom = written.load(std::memory_order_relaxed);
    f.old.reset();
    f.next = std::llround(c.start);
    f.revPos = f.revLen = 0;
    f.haveTail = false;
    f.master.reset();
}

void Player::Impl::notePeaks(Feed& f)
{
    const std::vector<float>& p = f.mixer->trackPeaks();
    if (f.peaks.size() != p.size() + 2) f.peaks.assign(p.size() + 2, 0.f);
    for (size_t i = 0; i < p.size(); ++i) f.peaks[i] = std::max(f.peaks[i], p[i]);
}

void Player::Impl::mixForward(Feed& f, int n, float* const* out)
{
    f.mixer->render(f.next, n, out);
    notePeaks(f);
    if (f.old) {
        float* o[kChannels];
        for (int c = 0; c < kChannels; ++c) o[c] = f.other[c].data();
        f.old->render(f.next, n, o);
        for (int i = 0; i < n; ++i) {
            const float w = std::min(1.f, float(f.xfade + i + 1) / kXfade);
            for (int c = 0; c < kChannels; ++c) out[c][i] = o[c][i] * (1.f - w) + out[c][i] * w;
        }
        f.xfade += n;
        if (f.xfade >= kXfade) f.old.reset();
    }
    f.next += n;
}

// Backwards: chunks mixed forward and reversed; neighbouring chunks overlap by kOverlap and crossfade there (every
// chunk starts the mixer over)
void Player::Impl::mixBackward(Feed& f, int n, float* const* out)
{
    f.old.reset(); // a new mix takes over at the next chunk
    for (int done = 0; done < n;) {
        if (f.revPos == f.revLen) {
            constexpr int len = kChunk + kOverlap;
            float* buf[kChannels];
            for (int c = 0; c < kChannels; ++c) buf[c] = f.chunk[c].data();
            f.mixer->render(f.next - len, len, buf);
            notePeaks(f);
            for (int c = 0; c < kChannels; ++c) {
                float* rev = f.rev[c].data();
                for (int i = 0; i < len; ++i) rev[i] = buf[c][len - 1 - i];
                if (f.haveTail)
                    for (int i = 0; i < kOverlap; ++i) {
                        const float w = float(i + 1) / (kOverlap + 1);
                        rev[i] = f.tail[c][size_t(i)] * (1.f - w) + rev[i] * w;
                    }
                std::copy(rev + kChunk, rev + len, f.tail[c].begin());
            }
            f.haveTail = true;
            f.revPos = 0;
            f.revLen = kChunk;
            f.next -= kChunk;
        }
        const int k = std::min(n - done, f.revLen - f.revPos);
        for (int c = 0; c < kChannels; ++c)
            std::copy(f.rev[c].begin() + f.revPos, f.rev[c].begin() + f.revPos + k, out[c] + done);
        f.revPos += k;
        done += k;
    }
}

void Player::Impl::produce(Feed& f, float* const* out, int n)
{
    const double speed = std::abs(f.speed);
    const int tape = int(speed);
    if (speed != tape || tape < 1 || tape > kMaxTape) {
        for (int c = 0; c < kChannels; ++c) std::fill(out[c], out[c] + n, 0.f);
    } else {
        float* src[kChannels];
        for (int c = 0; c < kChannels; ++c) src[c] = tape == 1 ? out[c] : f.content[c].data();
        if (f.speed > 0) mixForward(f, n * tape, src);
        else mixBackward(f, n * tape, src);
        if (tape > 1) // faster like tape: the mean of `tape` samples
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < n; ++i) {
                    float sum = 0;
                    for (int k = 0; k < tape; ++k) sum += src[c][i * tape + k];
                    out[c][i] = sum / float(tape);
                }
    }
    if (SharedLoudness* live = loudness.load(std::memory_order_relaxed); live && live->active && f.speed == 1.0) {
        std::lock_guard<std::mutex> g(live->mutex);
        auto sample = [&](int c, int i) { return out[c][i]; };
        live->meter.add(n, kChannels, opt.rate, sample);
        live->truePeak.add(n, kChannels, opt.rate, sample);
    }
    f.master.resetPeaks();
    f.master.add(n, kChannels, opt.rate, [&](int c, int i) { return out[c][i]; });
    if (f.peaks.size() < 2) f.peaks.assign(2, 0.f);
    f.peaks[f.peaks.size() - 2] = std::max(f.peaks[f.peaks.size() - 2], float(f.master.peak(0)));
    f.peaks[f.peaks.size() - 1] = std::max(f.peaks[f.peaks.size() - 1], float(f.master.peak(1)));
    f.produced += n;
    {
        std::lock_guard<std::mutex> g(levelMutex);
        levels.push_back({f.gen, f.start + double(f.produced) * f.speed, f.peaks});
        while (levels.size() > 400) levels.pop_front(); // nobody asks
    }
    std::fill(f.peaks.begin(), f.peaks.end(), 0.f);
}

// ---- Control ----

Device::Config Player::Impl::deviceConfig()
{
    std::lock_guard<std::mutex> lock(mutex);
    Device::Config c;
    c.rate = opt.rate;
    c.channels = kChannels;
    c.buffer = opt.buffer;
    c.backend = opt.backend;
    c.output = opt.output;
    return c;
}

void Player::Impl::openDevice(bool silent)
{
    Device::Config c = deviceConfig();
    {
        std::lock_guard<std::mutex> lock(mutex);
        deviceDirty = false;
    }
    if (silent) c.backend = "null";
    forcedSilence = silent;
    closeDevice(); // the old callback stops first
    silentLimit = std::max<int64_t>(kSilentNs, int64_t(c.buffer) * 6 * 1'000'000'000 / c.rate);
    openedAt = nowNs();
    ++opened;
    lastCallback.store(openedAt, std::memory_order_relaxed);
    QString error;
    device = Device::open(c, [this](float* out, int frames) { process(out, frames); }, &error);
    latency = device->latency();
    qInfo("Audio: %s, Latenz %d Samples", qPrintable(device->name()), latency.load());
    std::lock_guard<std::mutex> lock(mutex);
    deviceName = device->name();
    deviceError = silent ? QString("Ausgabe antwortet nicht") : error;
}

bool Player::Impl::deviceSilent() const
{
    return nowNs() - lastCallback.load(std::memory_order_relaxed) > silentLimit.load(std::memory_order_relaxed);
}

void Player::Impl::closeDevice()
{
    if (!device) return;
    if (!deviceSilent()) {
        device.reset();
        return;
    }
    // A vanished device: closing it may take long or hang in the backend (seen with miniaudio and PulseAudio), so it
    // never calls us again and is closed aside
    device->detach();
    std::thread([old = std::move(device)]() mutable { old.reset(); }).detach();
}

void Player::Impl::watch()
{
    int64_t lastTry = 0;
    int tries = 0;   // reopens in a row without the device calling back
    int seenReroutes = 0;
    int seen = 0;
    std::unique_lock<std::mutex> wl(watchMutex);
    while (!watchQuit) {
        watchWake.wait_for(wl, std::chrono::milliseconds(50));
        if (watchQuit) break;
        wl.unlock();
        bool retry = false;
        Device::Config want;
        {
            std::lock_guard<std::mutex> dl(deviceMutex);
            if (device) {
                if (opened != seen) { // opened anew (here or by play())
                    seen = opened;
                    seenReroutes = 0;
                }
                bool dirty;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    dirty = deviceDirty;
                }
                const int64_t now = nowNs();
                const int64_t silent = now - lastCallback.load(std::memory_order_relaxed);
                if (const int r = device->reroutes(); r != seenReroutes) {
                    qInfo("Audio: das System hat die Ausgabe verlegt");
                    reroutes.fetch_add(r - seenReroutes, std::memory_order_relaxed);
                    seenReroutes = r;
                }
                if (dirty) {
                    openDevice();
                    tries = 0;
                } else if (deviceSilent()) {
                    ++tries;
                    reopens.fetch_add(1, std::memory_order_relaxed);
                    qWarning("Audio: %s nimmt seit %lld ms nichts mehr – öffne neu", qPrintable(device->name()),
                             (long long)(silent / 1'000'000));
                    openDevice(tries >= kSilentTries); // the clock must run: silence after a few tries
                    lastTry = now;
                } else {
                    if (!forcedSilence && lastCallback.load(std::memory_order_relaxed) > openedAt) tries = 0;
                    if ((forcedSilence || device->fallback()) && now - lastTry > kRetryNs) {
                        lastTry = now;
                        retry = true;
                        want = deviceConfig();
                    }
                }
            }
        }
        // Looking for the asked output takes a few ms (the audio system lists its devices): not under deviceMutex
        if (retry && Device::available(want)) {
            std::lock_guard<std::mutex> dl(deviceMutex);
            if (device && (forcedSilence || device->fallback())) {
                qInfo("Audio: gewünschte Ausgabe ist wieder da");
                reopens.fetch_add(1, std::memory_order_relaxed);
                tries = 0;
                openDevice();
            }
        }
        wl.lock();
    }
}

Player::Player(const Options& options) : d(std::make_unique<Impl>(options))
{
    d->feeder = std::thread([this] { d->feed(); });
    d->watchdog = std::thread([this] { d->watch(); });
}

Player::~Player()
{
    {
        std::lock_guard<std::mutex> lock(d->watchMutex);
        d->watchQuit = true;
    }
    d->watchWake.notify_all();
    d->watchdog.join();
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->quit = true;
    }
    d->wake.notify_all();
    d->feeder.join();
    d->closeDevice();
}

void Player::setTimeline(const Timeline& timeline, FrameRate fps)
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pending.emplace(timeline, fps);
    }
    d->wake.notify_all();
}

void Player::setFileLengths(const QHash<QString, int>& lengths)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    d->projectLengths = lengths;
}

void Player::setDevice(int buffer, const QString& backend, const QString& output)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (buffer == d->opt.buffer && backend == d->opt.backend && output == d->opt.output) return;
    d->opt.buffer = buffer;
    d->opt.backend = backend;
    d->opt.output = output;
    d->deviceDirty = true; // the watchdog reopens an open device; play() opens with them anyway
}

void Player::play(double position, double speed)
{
    if (speed == 0.0) {
        pause();
        return;
    }
    {
        std::lock_guard<std::mutex> dl(d->deviceMutex);
        if (!d->device) d->openDevice();
    }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmd = {d->cmd.gen + 1, true, position, speed};
        d->heardMax = 0;
        d->holdUntil.store(nowNs() + kHoldNs, std::memory_order_relaxed);
        d->gen.store(d->cmd.gen, std::memory_order_release);
    }
    d->wake.notify_all();
}

void Player::pause()
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->cmd.playing) return;
        d->pausedAt = d->positionLocked();
        d->cmd.playing = false;
        ++d->cmd.gen;
        d->gen.store(d->cmd.gen, std::memory_order_release);
    }
    d->wake.notify_all();
}

bool Player::playing() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->cmd.playing;
}

double Player::speed() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->cmd.playing ? d->cmd.speed : 0.0;
}

void Player::release() { d->holdUntil.store(0, std::memory_order_relaxed); }

void Player::setMuted(bool muted) { d->muted.store(muted, std::memory_order_relaxed); }

double Player::position() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->positionLocked();
}

std::vector<float> Player::takeLevels()
{
    double pos, speed;
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->cmd.playing) return {};
        pos = d->positionLocked();
        speed = d->cmd.speed;
        gen = d->cmd.gen;
    }
    std::vector<float> out;
    std::lock_guard<std::mutex> lock(d->levelMutex);
    while (!d->levels.empty()) {
        const LevelEntry& e = d->levels.front();
        if (e.gen == gen) {
            if (speed > 0 ? e.end > pos : e.end < pos) break; // not heard yet
            if (out.size() != e.peaks.size()) out.assign(e.peaks.size(), 0.f);
            for (size_t i = 0; i < out.size(); ++i) out[i] = std::max(out[i], e.peaks[i]);
        }
        d->levels.pop_front();
    }
    return out;
}

void Player::setLoudness(SharedLoudness* loudness) { d->loudness.store(loudness); }

QString Player::deviceName() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->deviceName;
}

QString Player::deviceError() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->deviceError;
}

Player::Stats Player::stats() const
{
    Stats s;
    s.underruns = d->underruns.load();
    s.mixers = d->mixersBuilt.load();
    s.liveUpdates = d->liveUpdates.load();
    s.fadeOuts = d->fadeOuts.load();
    s.reopens = d->reopens.load();
    s.reroutes = d->reroutes.load();
    return s;
}

} // namespace Audio
