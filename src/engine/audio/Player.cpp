// SPDX-License-Identifier: AGPL-3.0-only
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
#include <numbers>
#include <optional>
#include <utility>
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
// Scrubbing (scrub()): slice lengths and edge fades in ms of the output rate; jumps keep the filters going and warm
// noise reduction up over this many samples only (Mixer::setChunked)
constexpr int kScrubMinMs = 40;
constexpr int kScrubMaxMs = 80;
constexpr int kScrubFadeMs = 4;
constexpr int kScrubWarm = 1024;

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
// peaks: the stereo levels ([track L, R ..., master L, R]), then from `split` on the per-speaker ones of a 5.1/7.1
// mix ([track ch1..chN ..., master ch1..chN]; none in stereo)
struct LevelEntry {
    uint32_t gen = 0;
    double end = 0;
    std::vector<float> peaks;
    size_t split = 0;
};

} // namespace

struct Player::Impl {
    Options opt;
    // opt.buffer for the feeder thread (leadBlocks), which reads it without the mutex; written with opt.buffer
    std::atomic<int> bufferNow{1024};

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
    bool reopen = false; // reopenFiles(): the next mix opens its files anew
    QHash<QString, int> projectLengths; // setFileLengths()
    bool deviceDirty = false;
    bool quit = false;
    std::atomic<uint32_t> gen{0};          // generation the callback plays
    std::atomic<int64_t> holdUntil{0};     // nothing of the generation holdGen is handed over before (ns)
    std::atomic<uint32_t> holdGen{0};
    std::atomic<bool> muted{false};
    std::atomic<SharedLoudness*> loudness{nullptr};

    // Scrubbing (paused). The newest request under `mutex`; scrubCmd = cmd.gen of the scrub generation (none: ~0),
    // scrubGen the same for the callback (a scrub's end is no dropout)
    struct ScrubRequest {
        uint64_t serial = 0;
        int64_t pos = 0;
        int len = 0; // samples of the slice
    } scrubReq;
    uint64_t scrubTaken = 0; // serial the feeder took last
    uint32_t scrubCmd = ~0u;
    std::atomic<uint32_t> scrubGen{~0u};
    int64_t scrubAt = 0;      // when the last request came (ns), GUI side
    int64_t scrubPos = -1;    // its position
    double scrubSpeed = 0;    // how fast the requests move (timeline seconds per second, smoothed)
    std::atomic<int> scrubRequests{0}, scrubSlices{0}, scrubJoins{0}, scrubReplaced{0}, scrubCuts{0};

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
        bool fading = false; // the callback before ended inside that fade-out (more of the old sound follows)
        float muteGain = 1.f;
        // The sound broke off (the feeder could not keep up): what comes next fades in from here (a sample of handed)
        int64_t fadeInAt = 0;
        // At the end of the callback before: the ring's write count and the time (did the feeder have a chance?)
        uint64_t prevWritten = 0;
        int64_t prevNow = 0;
    } cb;
    std::atomic<int64_t> feedStallUntil{0}; // tests: the feeder makes nothing before this (ns)
    std::atomic<int> underruns{0}, fadeOuts{0}, mixersBuilt{0}, liveUpdates{0}, reopens{0}, reroutes{0};
    // The device called back later than its buffer lasts while sound ran: it most likely played silence in between
    // (the sound server starved, not schneidi's feeder). lateMaxNs: the longest such gap since the last report
    std::atomic<int> lateCallbacks{0};
    std::atomic<bool> steady{false}; // the open device calls back steadily (Device::steadyCallbacks)
    std::atomic<int64_t> lateMaxNs{0};
    // What the log already reported (GUI thread, under `mutex`): pause() reports only what is new
    int reportedUnderruns = 0, reportedLate = 0;
    std::atomic<int64_t> lastCallback{0}; // when the device last took samples (ns)
    std::atomic<int64_t> silentLimit{kSilentNs}; // took nothing for this long: vanished (ns, from the buffer)

    // The device: opened by play() and the watchdog, under deviceMutex (taken before `mutex`, never inside it)
    std::mutex deviceMutex;
    std::unique_ptr<Device> device;
    // Set once the first device is open (stays set): play() checks this instead of taking deviceMutex, which the
    // watchdog holds while the driver takes its time (100 ms to seconds when headphones are plugged in or out)
    std::atomic<bool> deviceOpened{false};
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
        std::optional<Timeline> timeline;  // what the mixer mixes (reopenFiles builds it again)
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
        // Backwards: track peaks per block of the chunk mixed last (Mixer::blockPeaks) and of its first block (the
        // overlap the next chunk fades in from)
        std::vector<float> chunkPeaks, tailPeaks;
        // 5.1/7.1: per speaker, tracks then master (Mixer::channelPeaks); the same per block backwards
        std::vector<float> chPeaks, chunkChPeaks, tailChPeaks;
        // Scrubbing: mixed slices waiting for the ring (planar, samples [sPos, sLen)). With sTail the last `fade`
        // samples are the sound after the slice, not faded yet: the next slice continues after them (sNext) or
        // crossfades over them, or they fade out when nothing follows
        std::vector<float> staged[kChannels], slice[kChannels];
        int sPos = 0, sLen = 0;
        bool sTail = false;
        int64_t sNext = 0;
        int sPlayed = 0; // samples of the slice under way handed to the ring
    };

    explicit Impl(const Options& o) : opt(o), bufferNow(o.buffer) {}

    void process(float* out, int frames);
    // under `mutex`: a log line when sound broke off since the last report; true when the device itself ran dry
    bool reportDropouts();
    void publishClock(int64_t now);
    double positionLocked();
    Device::Config deviceConfig();
    void openDevice(bool silent = false); // under deviceMutex
    void closeDevice();                   // under deviceMutex
    bool deviceSilent() const;
    void watch();

    void feed();
    // fresh = a new mixer even if the running one could take it live (files overwritten: it keeps their readers)
    void takeTimeline(Feed& f, const Timeline& tl, FrameRate fps, bool playingForward, bool fresh = false);
    void startGeneration(Feed& f, const Command& c);
    void produce(Feed& f, float* const* out, int n);
    void mixForward(Feed& f, int n, float* const* out);
    void mixBackward(Feed& f, int n, float* const* out);
    void notePeaks(Feed& f, const float* p, size_t n);
    // Scrubbing, one step of the feeder (under `mutex`, unlocks while mixing); false = nothing to do, wait
    bool scrubStep(Feed& f, std::unique_lock<std::mutex>& lock);
    void mixSlice(Feed& f, const ScrubRequest& req, bool cut);
    void writeStaged(Feed& f, int n); // n samples of the staged sound into the ring as blocks (padded with silence)
    int scrubFade() const { return opt.rate * kScrubFadeMs / 1000; }
    // A request this close to where the mixed sound goes on continues it (a slow drag runs behind by the lead)
    bool joins(const Feed& f, int64_t pos) const
    {
        const int64_t tol = int64_t(opt.rate) * kScrubMaxMs / 1000;
        return f.sTail && pos >= f.sNext - 2 * tol && pos <= f.sNext + tol;
    }
    void noteChannelPeaks(Feed& f, const float* p, size_t n);
    // Ahead of the device: at least 50 ms and the asked buffer, and the device's real buffer + 20 ms – a device may
    // take its whole buffer at once (miniaudio's DirectSound: gaps up to 100 ms, then 2 periods of 1440 at once,
    // more than the 2048 asked; measured, docs/dev/ton.md)
    int leadBlocks() const
    {
        const int ahead = std::max({bufferNow.load(std::memory_order_relaxed), 2400, latency.load(std::memory_order_relaxed) + opt.rate / 50});
        return (ahead + kBlock - 1) / kBlock;
    }
};

// ---- Device thread ----

void Player::Impl::process(float* out, int frames)
{
    auto& s = cb;
    const uint32_t g = gen.load(std::memory_order_acquire);
    if (g != s.gen) { // new generation: fade out what sounds, count anew
        // A fade under way goes on from its gain: one still fading in (handed < kFade) fades out from where it got,
        // a fade-out keeps going over the old sound (starting it anew from full level was a step up, dropping it a
        // break from half level – a jump or stop within 5 ms of the one before)
        if (s.sounding) {
            // (from where its fade-in after a gap got: one just faded out at a gap is silent already)
            s.fadeLeft = int(std::clamp<int64_t>(s.handed - s.fadeInAt, 0, kFade));
            fadeOuts.fetch_add(1, std::memory_order_relaxed);
        } else if (!s.fading) {
            // Nothing sounds: a rest of an earlier fade must not let blocks of a generation never heard (or heard
            // after a gap) sound for a few ms
            s.fadeLeft = 0;
        }
        s.gen = g;
        s.handed = s.runT = s.runN = s.fadeInAt = 0;
        s.gapT = s.total;
        s.inRun = false;
    }
    // The hold belongs to its generation: a play() between loading `gen` and here must not hold the generation that
    // is sounding (a hard cut without the fade)
    const int64_t until = holdUntil.load(std::memory_order_acquire);
    const int64_t now = nowNs();
    if (s.sounding && s.prevNow != 0 && steady.load(std::memory_order_relaxed)) {
        // Sound ran in the callback before: a gap longer than the device's buffer (+ 5 ms of scheduling slack) means
        // the device ran dry in between - a click the feeder never sees
        const int lat = latency.load(std::memory_order_relaxed);
        const int64_t gap = now - s.prevNow;
        if (lat > 0 && gap > int64_t(lat) * 1'000'000'000 / opt.rate + 5'000'000) {
            lateCallbacks.fetch_add(1, std::memory_order_relaxed);
            if (gap > lateMaxNs.load(std::memory_order_relaxed)) lateMaxNs.store(gap, std::memory_order_relaxed);
        }
    }
    const bool hold = until != 0 && now < until && holdGen.load(std::memory_order_relaxed) == g;
    // Scrubbing: the sound ends after every slice on purpose (no dropout), the slices fade themselves
    const bool scrubbing = scrubGen.load(std::memory_order_relaxed) == g;
    const float muteTarget = muted.load(std::memory_order_relaxed) ? 0.f : 1.f;
    int i = 0;
    bool sound = false, fade = false;
    int own = -1; // where this generation's sound starts in `out` (-1: none of it here)
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
            fade = k > 0 && s.fadeLeft > 0;
            s.offset = s.fadeLeft > 0 ? s.offset + k : kBlock;
        } else {
            s.fadeLeft = 0; // the old sound ran out before its fade ended: nothing of it is left to fade
            if (hold) break;
            k = std::min(frames - i, kBlock - s.offset);
            if (!s.inRun) {
                s.inRun = true;
                s.runT = s.total + i;
                s.runN = s.handed;
            }
            if (own < 0) own = i;
            for (int j = 0; j < k; ++j) {
                const int64_t n = s.handed + j - s.fadeInAt;
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
            fade = false;
        }
        if (s.offset >= kBlock) {
            s.offset = 0;
            consumed.store(r + 1, std::memory_order_release);
        }
    }
    // The feeder fell behind (a starved computer): the sound that is there fades out instead of breaking off, and
    // what comes after the gap fades in (a hard stop and a hard start were two clicks). The ring runs dry at a block
    // boundary, mostly the end of a callback: so the fade starts in the callback whose successor cannot be filled.
    const auto fadeOutTail = [&](int end) {
        const int m = own < 0 ? 0 : std::min(kFade, end - own);
        for (int j = 0; j < m; ++j)
            for (int c = 0; c < kChannels; ++c) out[(end - m + j) * kChannels + c] *= float(m - j) / float(m + 1);
        s.fadeInAt = s.handed;
    };
    if (i < frames) {
        std::fill(out + i * kChannels, out + frames * kChannels, 0.f);
        // A fade-out under way broke off with the ring: what of the old sound comes later stays silent
        s.fadeLeft = 0;
        fade = false;
        if (s.inRun) { // the sound ran dry (not: a generation that has not started yet)
            if (s.fadeInAt != s.handed) fadeOutTail(i); // not faded out at the end of the callback before
            s.inRun = false;
            s.gapT = s.total + i;
            if (!scrubbing) underruns.fetch_add(1, std::memory_order_relaxed);
        }
        sound = false;
    } else if (s.inRun && !hold && !scrubbing && own >= 0 && s.handed - s.fadeInAt > kFade
               && written.load(std::memory_order_acquire) == s.prevWritten && now - s.prevNow >= 4'000'000) {
        // Full, but the next callback could not be: less than `frames` of this generation left in the ring, and the
        // feeder wrote nothing since the callback before although it had the time (a feeder that keeps up tops the
        // ring up every 2 ms; a device that calls back in bursts, e.g. DirectSound, gives it no time in between - no
        // underrun there, so no dip)
        int64_t left = 0;
        const uint64_t w = written.load(std::memory_order_acquire);
        for (uint64_t r = consumed.load(std::memory_order_relaxed); r < w && left < frames; ++r)
            if (ring[r % kRing].gen == g) left += kBlock - (r == consumed.load(std::memory_order_relaxed) ? s.offset : 0);
        if (left < frames) fadeOutTail(frames);
    }
    s.sounding = sound;
    s.fading = fade;
    s.prevWritten = written.load(std::memory_order_acquire);
    s.prevNow = now;
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
        if (pending || (reopen && f.timeline)) {
            const bool fresh = std::exchange(reopen, false);
            auto [tl, fps] = pending ? std::move(*pending) : std::pair{*f.timeline, f.fps};
            pending.reset();
            f.project = projectLengths;
            const bool forward = cmd.playing && cmd.speed > 0 && cmd.gen == f.gen;
            lock.unlock();
            takeTimeline(f, tl, fps, forward, fresh);
            lock.lock();
            continue;
        }
        const Command c = cmd;
        if (!c.playing && c.gen == scrubCmd && f.mixer) {
            if (!scrubStep(f, lock)) wake.wait(lock);
            continue;
        }
        if (!c.playing || !f.mixer) {
            wake.wait(lock);
            continue;
        }
        if (c.gen != f.gen) startGeneration(f, c);
        if (const int64_t stall = feedStallUntil.load(std::memory_order_relaxed); stall != 0 && nowNs() < stall) {
            waitFor(wake, lock, std::chrono::milliseconds(2));
            continue;
        }
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

void Player::Impl::takeTimeline(Feed& f, const Timeline& tl, FrameRate fps, bool playingForward, bool fresh)
{
    f.timeline = tl;
    if (fresh) f.lengths.clear(); // measured from the old contents
    if (!fresh && f.mixer && fps == f.fps && f.mixer->updateMix(tl)) {
        ++liveUpdates;
        return;
    }
    Mixer::Options o;
    o.rate = opt.rate;
    o.outputChannels = kChannels; // the device hears stereo: a 5.1/7.1 mix as its ITU downmix
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

// ---- Scrubbing ----

bool Player::Impl::scrubStep(Feed& f, std::unique_lock<std::mutex>& lock)
{
    if (f.gen != cmd.gen) { // a new scrub: nothing staged from before
        f.gen = cmd.gen;
        f.genFrom = written.load(std::memory_order_relaxed);
        f.old.reset();
        f.sPos = f.sLen = 0;
        f.sTail = false;
        f.sPlayed = 0;
    }
    // As far ahead as playback keeps (the device may take a whole period at once): the newest request is taken
    // when the ring needs more, so a fast drag never queues up
    const uint64_t w = written.load(std::memory_order_relaxed), r = consumed.load(std::memory_order_acquire);
    if (w - std::max(r, f.genFrom) >= uint64_t(leadBlocks()) || w - r >= uint64_t(kRing - 1)) {
        waitFor(wake, lock, std::chrono::milliseconds(2));
        return true;
    }
    const int fade = scrubFade();
    const bool fresh = scrubReq.serial != scrubTaken;
    const int firm = f.sLen - f.sPos - (f.sTail ? fade : 0); // staged sound that plays whatever comes next
    // A request elsewhere cuts a slice that played its shortest length (the newest position wins)
    const int minLen = opt.rate * kScrubMinMs / 1000;
    const bool cut = fresh && firm >= kBlock && f.sPlayed >= minLen && !joins(f, scrubReq.pos);
    if (firm >= kBlock && !cut) {
        lock.unlock();
        writeStaged(f, kBlock);
        lock.lock();
        return true;
    }
    if (fresh) {
        const ScrubRequest req = scrubReq;
        scrubTaken = req.serial;
        lock.unlock();
        mixSlice(f, req, cut);
        lock.lock();
        return true;
    }
    if (f.sLen > f.sPos) { // nothing new: the last slice fades out over its tail and ends
        lock.unlock();
        if (f.sTail)
            for (int i = 0; i < fade; ++i) {
                const double x = (i + 0.5) / fade * std::numbers::pi / 2;
                const float out = float(std::cos(x) * std::cos(x));
                for (int c = 0; c < kChannels; ++c) f.staged[c][size_t(f.sLen - fade + i)] *= out;
            }
        f.sTail = false;
        writeStaged(f, f.sLen - f.sPos);
        lock.lock();
        return true;
    }
    return false;
}

void Player::Impl::mixSlice(Feed& f, const ScrubRequest& req, bool cut)
{
    const int fade = scrubFade();
    if (cut) { // the staged rest goes: the sound right after what is in the ring becomes the tail that fades out
        f.sLen = f.sPos + fade;
        f.sTail = true;
        scrubCuts.fetch_add(1, std::memory_order_relaxed);
    }
    const bool join = !cut && joins(f, req.pos);
    const int64_t from = join ? f.sNext : req.pos;
    const int n = req.len + fade;
    // Staged sound to the front, room for the slice
    const int keep = f.sLen - f.sPos;
    for (int c = 0; c < kChannels; ++c) {
        std::copy(f.staged[c].begin() + f.sPos, f.staged[c].begin() + f.sLen, f.staged[c].begin());
        if (f.staged[c].size() < size_t(keep + n)) f.staged[c].resize(size_t(keep + n));
        if (f.slice[c].size() < size_t(n)) f.slice[c].resize(size_t(n));
    }
    f.sPos = 0;
    f.sLen = keep;
    float* out[kChannels];
    for (int c = 0; c < kChannels; ++c) out[c] = f.slice[c].data();
    f.mixer->setChunked(kScrubWarm); // a jump keeps the filters going and warms noise reduction up briefly
    if (!join) f.mixer->settleLevels(); // a mute or fader changed since the last slice holds from the start
    f.mixer->setMeterBlock(0);
    f.mixer->render(from, n, out);
    int at = 0; // samples of the slice that overlap the staged tail
    if (!join) {
        // Fade in (sin²); over a tail before it crossfades with that tail fading out (cos²: the two add up to one)
        for (int i = 0; i < fade; ++i) {
            const double x = (i + 0.5) / fade * std::numbers::pi / 2;
            const float in = float(std::sin(x) * std::sin(x)), outGain = 1.f - in;
            for (int c = 0; c < kChannels; ++c) {
                out[c][i] *= in;
                if (f.sTail) {
                    float& t = f.staged[c][size_t(f.sLen - fade + i)];
                    t = t * outGain + out[c][i];
                }
            }
        }
        if (f.sTail) at = fade;
        f.sPlayed = 0;
    } else {
        scrubJoins.fetch_add(1, std::memory_order_relaxed);
    }
    for (int c = 0; c < kChannels; ++c) std::copy(out[c] + at, out[c] + n, f.staged[c].begin() + f.sLen);
    f.sLen += n - at;
    f.sTail = true;
    f.sNext = from + n;
    scrubSlices.fetch_add(1, std::memory_order_relaxed);
}

void Player::Impl::writeStaged(Feed& f, int n)
{
    for (int done = 0; done < n; done += kBlock) {
        const int k = std::min(kBlock, n - done);
        Block& b = ring[written.load(std::memory_order_relaxed) % kRing];
        for (int i = 0; i < kBlock; ++i)
            for (int c = 0; c < kChannels; ++c)
                b.data[i * kChannels + c] = i < k ? f.staged[c][size_t(f.sPos + done + i)] : 0.f;
        b.gen = f.gen;
        written.fetch_add(1, std::memory_order_release);
    }
    f.sPos += n;
    f.sPlayed += n;
    if (f.sPos >= f.sLen) f.sPos = f.sLen = 0;
}

// Track peaks (L, R per track) into the peaks of the block being made
void Player::Impl::notePeaks(Feed& f, const float* p, size_t n)
{
    if (f.peaks.size() != n + 2) f.peaks.assign(n + 2, 0.f);
    for (size_t i = 0; i < n; ++i) f.peaks[i] = std::max(f.peaks[i], p[i]);
}

// Per-speaker peaks of a 5.1/7.1 mix (tracks, then master) into the block being made
void Player::Impl::noteChannelPeaks(Feed& f, const float* p, size_t n)
{
    if (f.chPeaks.size() != n) f.chPeaks.assign(n, 0.f);
    for (size_t i = 0; i < n; ++i) f.chPeaks[i] = std::max(f.chPeaks[i], p[i]);
}

void Player::Impl::mixForward(Feed& f, int n, float* const* out)
{
    f.mixer->setChunked(-1);
    f.mixer->setMeterBlock(0);
    f.mixer->render(f.next, n, out);
    notePeaks(f, f.mixer->trackPeaks().data(), f.mixer->trackPeaks().size());
    if (f.mixer->channels() > kChannels) noteChannelPeaks(f, f.mixer->channelPeaks().data(), f.mixer->channelPeaks().size());
    if (f.old) {
        float* o[kChannels];
        for (int c = 0; c < kChannels; ++c) o[c] = f.other[c].data();
        f.old->setChunked(-1);
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

// Backwards: chunks mixed forward and reversed; neighbouring chunks overlap by kOverlap and crossfade there. Every
// chunk is a jump for the mixer, but its track filters, compressor and limiter go on from the chunk before, and noise
// reduction/voice cleaning only warm up over the overlap (Mixer::setChunked): no level flutter at the chunk rate, no
// seek and half a second of decoding per chunk. The meters follow block by block (a chunk is 33 blocks).
void Player::Impl::mixBackward(Feed& f, int n, float* const* out)
{
    constexpr int len = kChunk + kOverlap;
    static_assert(kChunk % kBlock == 0 && kOverlap % kBlock == 0, "blocks of the meters");
    f.old.reset(); // a new mix takes over at the next chunk
    f.mixer->setMeterBlock(kBlock);
    const size_t stride = f.mixer->trackPeaks().size();
    const size_t chStride = f.mixer->channels() > kChannels ? f.mixer->channelPeaks().size() : 0;
    for (int done = 0; done < n;) {
        if (f.revPos == f.revLen) {
            float* buf[kChannels];
            for (int c = 0; c < kChannels; ++c) buf[c] = f.chunk[c].data();
            if (f.haveTail && f.chunkPeaks.size() >= stride) // the chunk before's first block: the overlap
                f.tailPeaks.assign(f.chunkPeaks.begin(), f.chunkPeaks.begin() + ptrdiff_t(stride));
            else
                f.tailPeaks.clear();
            if (f.haveTail && chStride > 0 && f.chunkChPeaks.size() >= chStride)
                f.tailChPeaks.assign(f.chunkChPeaks.begin(), f.chunkChPeaks.begin() + ptrdiff_t(chStride));
            else
                f.tailChPeaks.clear();
            // only a chunk right after the one before carries the filter state on; the first chunk after a seek or
            // a change of direction starts over like the export (no gain reduction or noise estimate from elsewhere)
            f.mixer->setChunked(f.haveTail ? kOverlap : -1);
            f.mixer->render(f.next - len, len, buf);
            f.chunkPeaks = f.mixer->blockPeaks();
            if (chStride > 0) f.chunkChPeaks = f.mixer->blockChannelPeaks();
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
        // Meters: the blocks of the chunk that sound here (reversed position r = chunk sample len - 1 - r)
        if (stride > 0 && f.chunkPeaks.size() >= stride * (len / kBlock)) {
            for (int b = (len - f.revPos - k) / kBlock, last = (len - f.revPos - 1) / kBlock; b <= last; ++b)
                notePeaks(f, f.chunkPeaks.data() + size_t(b) * stride, stride);
            if (f.revPos < kOverlap && f.tailPeaks.size() == stride) notePeaks(f, f.tailPeaks.data(), stride);
        }
        if (chStride > 0 && f.chunkChPeaks.size() >= chStride * (len / kBlock)) {
            for (int b = (len - f.revPos - k) / kBlock, last = (len - f.revPos - 1) / kBlock; b <= last; ++b)
                noteChannelPeaks(f, f.chunkChPeaks.data() + size_t(b) * chStride, chStride);
            if (f.revPos < kOverlap && f.tailChPeaks.size() == chStride) noteChannelPeaks(f, f.tailChPeaks.data(), chStride);
        }
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
        // A 5.1/7.1 mix is measured over all its channels (as the export writes it), not its downmix
        static const std::vector<float> kNone;
        const std::vector<float>& full = f.mixer ? f.mixer->lastMix() : kNone;
        const int ch = f.mixer ? f.mixer->channels() : kChannels;
        if (ch > kChannels && full.size() == size_t(n) * size_t(ch)) {
            auto sample = [&](int c, int i) { return full[size_t(c) * size_t(n) + size_t(i)]; };
            live->meter.add(n, ch, opt.rate, sample);
            live->truePeak.add(n, ch, opt.rate, sample);
        } else {
            auto sample = [&](int c, int i) { return out[c][i]; };
            live->meter.add(n, kChannels, opt.rate, sample);
            live->truePeak.add(n, kChannels, opt.rate, sample);
        }
    }
    f.master.resetPeaks();
    f.master.add(n, kChannels, opt.rate, [&](int c, int i) { return out[c][i]; });
    if (f.peaks.size() < 2) f.peaks.assign(2, 0.f);
    f.peaks[f.peaks.size() - 2] = std::max(f.peaks[f.peaks.size() - 2], float(f.master.peak(0)));
    f.peaks[f.peaks.size() - 1] = std::max(f.peaks[f.peaks.size() - 1], float(f.master.peak(1)));
    f.produced += n;
    if (!f.mixer || f.mixer->channels() <= kChannels) f.chPeaks.clear(); // stereo: the speakers are L, R
    {
        // one vector as before (no extra allocation per block): stereo levels, then the speakers
        std::vector<float> all;
        all.reserve(f.peaks.size() + f.chPeaks.size());
        all.insert(all.end(), f.peaks.begin(), f.peaks.end());
        all.insert(all.end(), f.chPeaks.begin(), f.chPeaks.end());
        std::lock_guard<std::mutex> g(levelMutex);
        levels.push_back({f.gen, f.start + double(f.produced) * f.speed, std::move(all), f.peaks.size()});
        while (levels.size() > 400) levels.pop_front(); // nobody asks
    }
    std::fill(f.peaks.begin(), f.peaks.end(), 0.f);
    std::fill(f.chPeaks.begin(), f.chPeaks.end(), 0.f);
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
    cb.prevNow = 0; // the gap to the old device's last callback is no late callback (reopens are logged apart)
    // Until the new device's own values are known its callbacks are not judged (the old latency would be compared)
    steady = false;
    silentLimit = std::max<int64_t>(kSilentNs, int64_t(c.buffer) * 6 * 1'000'000'000 / c.rate);
    openedAt = nowNs();
    ++opened;
    lastCallback.store(openedAt, std::memory_order_relaxed);
    QString error;
    device = Device::open(c, [this](float* out, int frames) { process(out, frames); }, &error);
    deviceOpened.store(true);
    latency = device->latency();
    steady = device->steadyCallbacks();
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
        d->reportDropouts(); // closed while playing
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

void Player::reopenFiles()
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->reopen = true;
    }
    d->wake.notify_all();
}

void Player::setFileLengths(const QHash<QString, int>& lengths)
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        // A file the mix knows got another length (overwritten: reopenFiles() came first and built the mix with the
        // old one): reversed, retimed and transitioned clips depend on it, so the mix is built anew. New files alone
        // change nothing (the mix measured them itself).
        bool changed = false;
        for (auto it = d->projectLengths.cbegin(); it != d->projectLengths.cend() && !changed; ++it)
            changed = lengths.value(it.key(), -1) != it.value();
        d->projectLengths = lengths;
        if (!changed) return;
        d->reopen = true;
    }
    d->wake.notify_all();
}

void Player::setDevice(int buffer, const QString& backend, const QString& output)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (buffer == d->opt.buffer && backend == d->opt.backend && output == d->opt.output) return;
    d->opt.buffer = buffer;
    d->bufferNow.store(buffer, std::memory_order_relaxed);
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
    if (!d->deviceOpened.load()) { // only the first play: later the watchdog keeps a device open
        std::lock_guard<std::mutex> dl(d->deviceMutex);
        if (!d->device) d->openDevice();
    }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmd = {d->cmd.gen + 1, true, position, speed};
        d->heardMax = 0;
        d->scrubTaken = d->scrubReq.serial; // a scrub request not mixed yet is dropped
        d->scrubAt = 0;
        d->holdGen.store(d->cmd.gen, std::memory_order_relaxed);
        d->holdUntil.store(nowNs() + kHoldNs, std::memory_order_release); // after holdGen: seen with it
        d->gen.store(d->cmd.gen, std::memory_order_release);
    }
    d->wake.notify_all();
}

void Player::pause()
{
    int grown = 0;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->cmd.playing) return;
        d->pausedAt = d->positionLocked();
        d->cmd.playing = false;
        ++d->cmd.gen;
        d->gen.store(d->cmd.gen, std::memory_order_release);
        // The sound server did not take the sound in time (the device played silence: a click). A bigger buffer
        // bridges such gaps; it is reopened now, while paused, where the short break is not heard. Only up to
        // kMaxAutoBuffer (85 ms at 48 kHz): more lags behind the picture when scrubbing.
        if (d->reportDropouts() && d->opt.buffer < kMaxAutoBuffer) {
            d->opt.buffer = std::min(d->opt.buffer * 2, kMaxAutoBuffer);
            d->bufferNow.store(d->opt.buffer, std::memory_order_relaxed);
            d->deviceDirty = true; // the watchdog reopens with it
            grown = d->opt.buffer;
            qInfo("Audio: buffer grown to %d samples after the dropout", grown);
        }
    }
    d->wake.notify_all();
    if (grown && d->opt.bufferGrown) d->opt.bufferGrown(grown);
}

bool Player::Impl::reportDropouts()
{
    const int under = underruns.load(std::memory_order_relaxed);
    const int late = lateCallbacks.load(std::memory_order_relaxed);
    if (under == reportedUnderruns && late == reportedLate) return false;
    const int64_t maxNs = lateMaxNs.exchange(0, std::memory_order_relaxed);
    const int lat = latency.load(std::memory_order_relaxed);
    qInfo("Audio dropouts while playing: %d feeder underruns, %d late device callbacks (longest gap %lld ms, "
          "device buffer %d ms)",
          under - reportedUnderruns, late - reportedLate, static_cast<long long>(maxNs / 1'000'000),
          opt.rate > 0 ? lat * 1000 / opt.rate : 0);
    const bool deviceRanDry = late != reportedLate;
    reportedUnderruns = under;
    reportedLate = late;
    return deviceRanDry;
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

void Player::scrub(double position)
{
    if (!d->deviceOpened.load()) { // like play(): only until the first device is open
        std::lock_guard<std::mutex> dl(d->deviceMutex);
        if (!d->device) d->openDevice();
    }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->cmd.playing) return;
        const int64_t now = nowNs();
        const int64_t pos = std::max<int64_t>(0, std::llround(position));
        const double dt = d->scrubAt != 0 ? double(now - d->scrubAt) * 1e-9 : 1e9;
        if (pos == d->scrubPos && dt < 0.5) return; // the same place again (a mouse move within one frame)
        // How fast the scrub moves: smoothed over the requests of one drag; a pause between requests starts anew
        const double v = dt < 0.25 && d->scrubPos >= 0 ? double(std::abs(pos - d->scrubPos)) / d->opt.rate / std::max(dt, 1e-3) : 0;
        d->scrubSpeed = dt < 0.25 ? 0.5 * d->scrubSpeed + 0.5 * v : 0;
        // Slow (up to real time) the longest slices, from 4x the shortest
        const double t = std::clamp((d->scrubSpeed - 1.0) / 3.0, 0.0, 1.0);
        const int maxLen = d->opt.rate * kScrubMaxMs / 1000, minLen = d->opt.rate * kScrubMinMs / 1000;
        const int len = int(std::lround(maxLen - t * (maxLen - minLen)));
        if (d->scrubReq.serial != d->scrubTaken) d->scrubReplaced.fetch_add(1, std::memory_order_relaxed);
        d->scrubReq = {d->scrubReq.serial + 1, pos, len};
        d->scrubAt = now;
        d->scrubPos = pos;
        d->scrubRequests.fetch_add(1, std::memory_order_relaxed);
        if (d->scrubCmd != d->cmd.gen) { // the first request since playing or the last scrub: a generation of its own
            ++d->cmd.gen;
            d->scrubCmd = d->cmd.gen;
            d->scrubGen.store(d->cmd.gen, std::memory_order_relaxed);
            d->gen.store(d->cmd.gen, std::memory_order_release); // after scrubGen: the callback sees both
        }
    }
    d->wake.notify_all();
}

double Player::position() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->positionLocked();
}

std::vector<float> Player::takeLevels(std::vector<float>* speakers)
{
    if (speakers) speakers->clear();
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
            if (out.size() != e.split) out.assign(e.split, 0.f);
            for (size_t i = 0; i < out.size(); ++i) out[i] = std::max(out[i], e.peaks[i]);
            if (speakers) {
                const size_t n = e.peaks.size() - e.split;
                if (speakers->size() != n) speakers->assign(n, 0.f);
                for (size_t i = 0; i < n; ++i) (*speakers)[i] = std::max((*speakers)[i], e.peaks[e.split + i]);
            }
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

void Player::stallFeedForTest(int ms) { d->feedStallUntil.store(nowNs() + int64_t(ms) * 1000000); }

void Player::lateCallbackForTest() { d->lateCallbacks.fetch_add(1, std::memory_order_relaxed); }

Player::Stats Player::stats() const
{
    Stats s;
    s.underruns = d->underruns.load();
    s.lateCallbacks = d->lateCallbacks.load();
    s.mixers = d->mixersBuilt.load();
    s.liveUpdates = d->liveUpdates.load();
    s.fadeOuts = d->fadeOuts.load();
    s.reopens = d->reopens.load();
    s.reroutes = d->reroutes.load();
    s.scrubRequests = d->scrubRequests.load();
    s.scrubSlices = d->scrubSlices.load();
    s.scrubJoins = d->scrubJoins.load();
    s.scrubReplaced = d->scrubReplaced.load();
    s.scrubCuts = d->scrubCuts.load();
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        s.buffer = d->opt.buffer;
    }
    return s;
}

} // namespace Audio
