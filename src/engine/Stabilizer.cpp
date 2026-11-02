// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Stabilizer.h"

#include "core/EffectRegistry.h"
#include "core/Retime.h"
#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/ClipFormat.h"
#include "engine/StabilizeCorrection.h"
#include "engine/StabilizeMotion.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QMutex>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>

namespace {

// Bump when the analysis changes (old results are not reused). 1 = vid.stab (.trf), 2 = own analysis (.stab)
constexpr int kVersion = 2;
constexpr int kAnalysisSide = 960; // longer side of the analysis frames (the result fits any size)
constexpr const char* kSuffix = ".stab";
constexpr const char* kHeader = "schneidi-stabilize 1";

int even(double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); }

std::optional<Stabilizer::Request> makeRequest(const Clip& c, FrameRate rate, double aspect)
{
    if (!Stabilizer::unsupportedReason(c).isEmpty() || aspect <= 0) return std::nullopt;
    const EffectInstance* e = EffectRegistry::instance(c, Stabilizer::EffectId);
    if (!e) return std::nullopt;
    Stabilizer::Request r;
    r.file = QFileInfo(c.mediaPath).absoluteFilePath();
    r.speed = c.speed != 1.0 || c.reverse ? (c.reverse ? -c.speed : c.speed) : 1.0;
    r.rate = rate;
    if (aspect >= 1.0) {
        r.width = kAnalysisSide;
        r.height = even(kAnalysisSide / aspect);
    } else {
        r.height = kAnalysisSide;
        r.width = even(kAnalysisSide * aspect);
    }
    // Handles for transitions at the edges are analyzed too (a dissolve shows frames beyond in/out)
    const int margin = std::max({0, c.transIn, c.transOut});
    r.from = std::max(0, c.in - margin);
    r.to = c.out + margin;
    r.needFrom = std::max(0, c.in);
    r.needTo = std::max(r.needFrom, c.out);
    auto param = [&](const char* key, int lo, int hi) {
        return std::clamp(int(std::lround(EffectRegistry::value(c, Stabilizer::EffectId, key).toDouble())), lo, hi);
    };
    r.shakiness = param("shakiness", 1, 10);
    r.accuracy = param("accuracy", 1, 15);
    return r;
}

// "<base>_<from>_<to>.stab" -> range
bool parseName(const QString& name, const QString& base, int* from, int* to)
{
    if (!name.startsWith(base + '_') || !name.endsWith(kSuffix)) return false;
    const QStringList parts = name.mid(base.size() + 1).chopped(int(std::strlen(kSuffix))).split('_');
    if (parts.size() != 2) return false;
    bool ok1 = false, ok2 = false;
    *from = parts[0].toInt(&ok1);
    *to = parts[1].toInt(&ok2);
    return ok1 && ok2 && *to > *from;
}

void collectMissing(const Timeline& tl, const ProjectFormat& format, QVector<Stabilizer::Request>& out, QSet<QString>& seen)
{
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips) {
            const EffectInstance* e = EffectRegistry::instance(c, Stabilizer::EffectId);
            if (!c.enabled || !e || !e->enabled) continue;
            const auto r = Stabilizer::request(c, format);
            if (!r || seen.contains(r->key()) || Stabilizer::find(*r).valid()) continue;
            seen.insert(r->key());
            out << *r;
        }
}

using StabilizeMotion::Motion;
using Motions = std::vector<Motion>;

// Result file: header line, frame count, then per frame "dx dy angle scale valid" (C locale numbers)
QByteArray writeMotions(const Motions& m)
{
    QByteArray out = QByteArray(kHeader) + "\n" + QByteArray::number(qlonglong(m.size())) + "\n";
    for (const Motion& x : m)
        out += QByteArray::number(x.dx, 'g', 10) + ' ' + QByteArray::number(x.dy, 'g', 10) + ' '
               + QByteArray::number(x.angle, 'g', 10) + ' ' + QByteArray::number(x.scale, 'g', 10) + ' '
               + (x.valid ? '1' : '0') + '\n';
    return out;
}

std::shared_ptr<const Motions> readMotions(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.size() < 2 || lines[0] != kHeader) return nullptr;
    bool ok = false;
    const int n = lines[1].toInt(&ok);
    if (!ok || n < 0 || lines.size() < n + 2) return nullptr;
    auto out = std::make_shared<Motions>(size_t(n));
    for (int i = 0; i < n; ++i) {
        const QList<QByteArray> v = lines[i + 2].split(' ');
        if (v.size() != 5) return nullptr;
        Motion& m = (*out)[size_t(i)];
        bool a = false, b = false, c = false, d = false;
        m.dx = v[0].toDouble(&a);
        m.dy = v[1].toDouble(&b);
        m.angle = v[2].toDouble(&c);
        m.scale = v[3].toDouble(&d);
        m.valid = v[4] == "1";
        if (!a || !b || !c || !d || !(m.scale > 0)) return nullptr;
    }
    return out;
}

// Parsed results by path (the files never change: their names contain everything that went into them)
std::shared_ptr<const Motions> loadMotions(const QString& path)
{
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const Motions>> cache;
    QMutexLocker lock(&mutex);
    if (auto it = cache.constFind(path); it != cache.constEnd()) return *it;
    auto m = readMotions(path);
    if (m) cache.insert(path, m);
    return m;
}

// vid.stab's results (schneidi before 0.14) are never read again
void removeOldResults()
{
    static std::once_flag once;
    std::call_once(once, [] {
        QDir dir(Stabilizer::cacheDir());
        for (const QString& name : dir.entryList({"*.trf"}, QDir::Files)) QFile::remove(dir.filePath(name));
    });
}

// Results stored by this process so far: a cached "no result yet" of StabilizeCorrection::of is stale after one
std::atomic<int> g_results{0};

// Common end of both analyses: checks, pads frames past the file's end, stores the result
bool finish(const Stabilizer::Request& r, int from, int to, Motions& motions, QString* error,
            const std::atomic<bool>* cancel, std::atomic<int>* progress)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (cancel && cancel->load()) return fail(T("Abgebrochen."));
    if (int(motions.size()) != std::max(0, to - from + 1))
        return fail(T("Die Analyse ist fehlgeschlagen: %1").arg(QFileInfo(r.file).fileName()));
    Motion none;
    none.valid = false;
    motions.resize(size_t(r.to - from + 1), none);
    if (progress) *progress = 1000;

    QDir().mkpath(Stabilizer::cacheDir());
    removeOldResults();
    const QString target = QDir(Stabilizer::cacheDir()).filePath(QString("%1_%2_%3%4").arg(r.base()).arg(from).arg(r.to).arg(kSuffix));
    if (QFileInfo::exists(target)) { // the same analysis finished elsewhere meanwhile (export + preview)
        ++g_results;
        return true;
    }
    // Only visible under the final name once complete -> nobody reads a half written file
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(writeMotions(motions)) < 0 || !f.commit())
        return fail(T("Das Analyse-Ergebnis konnte nicht gespeichert werden: %1").arg(target));
    ++g_results;
    return true;
}

using StabilizeMotion::Outside;

} // namespace

QString Stabilizer::Request::base() const
{
    const QFileInfo fi(file);
    char num[32];
    // "t": a retimed clip's frames by the sound's time (RetimeMap::pictureFrame, since 0.15.1; before rounded like
    // MLT) - analyzed again
    std::snprintf(num, sizeof num, speed != 1.0 ? "%.10gt" : "%.10g", speed);
    const QString id = QString("%1|%2|%3|%4|%5|%6/%7|%8x%9|%10|%11")
                           .arg(kVersion)
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(QString::fromLatin1(num))
                           .arg(rate.num)
                           .arg(rate.den)
                           .arg(width)
                           .arg(height)
                           .arg(shakiness)
                           .arg(accuracy);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString Stabilizer::Request::key() const
{
    return QString("%1_%2_%3").arg(base()).arg(needFrom).arg(needTo);
}

QString Stabilizer::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/stabilize";
}

QString Stabilizer::unsupportedReason(const Clip& c)
{
    if (c.isTitle()) return T("Titel lassen sich nicht stabilisieren.");
    if (c.isCompound()) return T("Compound Clips lassen sich nicht stabilisieren.");
    if (c.isMulticam()) return T("Multicam-Clips lassen sich nicht stabilisieren.");
    if (c.mediaPath.isEmpty()) return T("Kein Medium.");
    if (c.freeze) return T("Standbilder lassen sich nicht stabilisieren.");
    if (c.hasRamp()) return T("Clips mit Speed Ramp lassen sich nicht stabilisieren.");
    // one frame has nothing to steady (its analysis failed and with it the whole export)
    if (c.out <= c.in) return T("Der Clip ist zu kurz zum Stabilisieren.");
    return {};
}

std::optional<Stabilizer::Request> Stabilizer::request(const Clip& c, const ProjectFormat& format)
{
    return makeRequest(c, format.rate, format.height > 0 ? double(format.width) / format.height : 0.0);
}

Stabilizer::Result Stabilizer::find(const Request& r)
{
    const QString base = r.base();
    const QDir dir(cacheDir());
    Result best;
    for (const QString& name : dir.entryList({base + "_*" + kSuffix}, QDir::Files)) {
        int from = 0, to = 0;
        if (!parseName(name, base, &from, &to) || from > r.needFrom || to < r.needTo) continue;
        // the widest one (more handles)
        if (!best.valid() || to - from > best.to - best.from) best = {dir.filePath(name), from, to};
    }
    return best;
}

bool Stabilizer::needsFrameOrder(const Timeline& tl)
{
    auto keeps = [](const Timeline& t) {
        for (const Track& track : t.video)
            for (const Clip& c : track.clips)
                if (const EffectInstance* e = EffectRegistry::instance(c, EffectId);
                    c.enabled && e && e->enabled && EffectRegistry::value(c, EffectId, "border").toString() == "keep")
                    return true;
        return false;
    };
    if (keeps(tl)) return true;
    if (tl.nested)
        for (const Timeline& n : *tl.nested)
            if (keeps(n)) return true;
    return false;
}

QVector<Stabilizer::Request> Stabilizer::missing(const Timeline& tl, const ProjectFormat& format)
{
    QVector<Request> out;
    QSet<QString> seen;
    collectMissing(tl, format, out, seen);
    if (tl.nested)
        for (const Timeline& n : *tl.nested) collectMissing(n, format, out, seen);
    return out;
}

bool Stabilizer::analyze(const Request& r, QString* error, const std::atomic<bool>* cancel, std::atomic<int>* progress)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!QFileInfo::exists(r.file)) return fail(T("Datei nicht gefunden: %1").arg(r.file));
    // Own decoder, positions as the timeline counts them (Picture::Renderer): project frames of the
    // file, for a speed change the frames RetimeMap::pictureFrame picks
    Video::Reader::Options ro;
    ro.hardware = false; // in order through the whole range: the software decoder keeps up and leaves the card alone
    std::unique_ptr<Video::Reader> reader = Video::Reader::open(r.file, ro);
    if (!reader) return fail(T("Datei lässt sich nicht öffnen: %1").arg(r.file));
    reader->setFrameRate(AVRational{r.rate.num, r.rate.den});
    const Video::Info& info = reader->info();
    const double fps = r.rate.fps();
    const double srcFps = info.frameRate.num > 0 && info.frameRate.den > 0 ? av_q2d(info.frameRate) : fps;
    const int fileLength = info.duration > 0 ? int(double(info.duration) / 1e6 * fps + 0.5) : 0;
    const double speed = std::abs(r.speed);
    const bool reverse = r.speed < 0;
    Clip timing;
    timing.speed = speed;
    timing.reverse = reverse;
    const RetimeMap map(timing, fileLength);
    const int warped = speed != 1.0 || reverse ? std::max(1, int(fileLength / speed)) : fileLength;
    const int from = std::max(0, r.from);
    const int to = std::min(r.to, warped - 1);
    if (r.to <= from) return fail(T("Der Clip ist zu kurz zum Stabilisieren."));

    // The picture fit into the analysis size like the timeline shows it (black bars for another aspect ratio)
    const bool sideways = std::lround(std::abs(info.rotation)) % 180 == 90;
    const double sar = info.sar.num > 0 && info.sar.den > 0 ? av_q2d(info.sar) : 1.0;
    const double dar = sideways ? double(info.height) / (info.width * sar) : info.width * sar / info.height;
    int fw = r.width, fh = r.height;
    if (dar > double(r.width) / r.height) fh = std::max(2, int(std::lround(r.width / dar / 2)) * 2);
    else fw = std::max(2, int(std::lround(r.height * dar / 2)) * 2);
    const int ox = (r.width - fw) / 2, oy = (r.height - fh) / 2;
    std::vector<uint8_t> luma(size_t(r.width) * size_t(r.height), 16); // black (video range)
    SwsContext* sws = nullptr;
    uint8_t* planes[4] = {};
    int strides[4] = {};
    std::vector<uint8_t> yuv(size_t(fw) * size_t(fh) * 2);
    planes[0] = yuv.data();
    planes[1] = yuv.data() + size_t(fw) * size_t(fh);
    planes[2] = planes[1] + size_t(fw / 2) * size_t(fh / 2);
    strides[0] = fw;
    strides[1] = strides[2] = fw / 2;

    std::unique_ptr<StabilizeMotion::Estimator> estimator;
    Motions motions;
    motions.reserve(size_t(r.to - from + 1));
    for (int pos = from; pos <= to; ++pos) {
        if (cancel && cancel->load()) break;
        double file = pos;
        if (warped != fileLength || reverse) file = map.pictureFrame(pos); // as Picture::Renderer shows it
        const auto pic = reader->picture(int64_t(file / fps * srcFps + 0.5));
        if (!pic || !pic->frame) break;
        const AVFrame* f = pic->frame;
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), fw, fh, AV_PIX_FMT_YUV420P,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws) break;
        sws_scale(sws, f->data, f->linesize, 0, f->height, planes, strides);
        for (int y = 0; y < fh; ++y)
            std::memcpy(luma.data() + size_t(oy + y) * size_t(r.width) + size_t(ox), planes[0] + size_t(y) * size_t(fw),
                        size_t(fw));
        if (!estimator) estimator = std::make_unique<StabilizeMotion::Estimator>(r.width, r.height, r.shakiness, r.accuracy);
        motions.push_back(estimator->push(luma.data(), r.width));
        if (progress) *progress = int(qint64(pos - from + 1) * 1000 / (to - from + 1));
    }
    sws_freeContext(sws);
    reader.reset();
    return finish(r, from, to, motions, error, cancel, progress);
}

namespace StabilizeCorrection {

std::shared_ptr<const Correction> of(const Clip& c, FrameRate rate, double aspect)
{
    const char* const EffectId = Stabilizer::EffectId;
    const EffectInstance* e = EffectRegistry::instance(c, EffectId);
    if (!e || !e->enabled) return nullptr;
    const auto r = makeRequest(c, rate, aspect);
    if (!r) return nullptr;
    StabilizeMotion::PathParams pp;
    pp.smoothing = std::clamp(int(std::lround(EffectRegistry::value(c, EffectId, "smoothing").toDouble())), 0, 100);
    pp.zoom = std::clamp(EffectRegistry::value(c, EffectId, "zoom").toDouble(), -90.0, 500.0);
    const QString border = EffectRegistry::value(c, EffectId, "border").toString();
    using StabilizeMotion::Border;
    pp.border = border == "adaptive" ? Border::Adaptive : border == "black" ? Border::Black : border == "keep" ? Border::Keep : Border::Auto;
    pp.aspect = aspect > 0 ? aspect : 16.0 / 9.0;
    // Cached by everything that goes in; a missing result is looked for again after a second (analysis running)
    struct Entry {
        std::shared_ptr<const Correction> value;
        std::chrono::steady_clock::time_point at;
        int results = 0; // g_results when it was looked up
    };
    static std::mutex mutex;
    static QHash<QString, Entry> cache;
    const QString key = QString("%1|%2|%3|%4|%5").arg(r->key()).arg(pp.smoothing).arg(pp.zoom).arg(int(pp.border)).arg(pp.aspect);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.constFind(key); it != cache.constEnd()
                                            && (it->value || (now - it->at < std::chrono::seconds(1) && it->results == g_results)))
            return it->value;
    }
    const int results = g_results;
    std::shared_ptr<Correction> out;
    const Stabilizer::Result res = Stabilizer::find(*r);
    if (const auto motions = res.valid() ? loadMotions(res.path) : nullptr;
        motions && int(motions->size()) == res.to - res.from + 1) {
        out = std::make_shared<Correction>();
        out->corrections = StabilizeMotion::corrections(*motions, pp);
        out->from = res.from;
        out->aspect = pp.aspect;
        out->border = pp.border;
    }
    std::lock_guard lock(mutex);
    if (cache.size() > 256) cache.clear();
    cache.insert(key, Entry{out, now, results});
    return out;
}

bool warpRgba(const Correction& c, int pos, const uint8_t* src, uint8_t* dst, int w, int h,
                          const uint8_t* previous, double widthShare, double heightShare)
{
    const int i = pos - c.from;
    if (i < 0 || i >= int(c.corrections.size()) || w < 2 || h < 2) return false;
    StabilizeMotion::Similarity s = c.corrections[size_t(i)];
    if (s.isIdentity()) return false;
    // Normalized coordinates of the frame (unit: its width) -> of the picture (unit: the picture's width), both
    // centred: the shift grows, rotation and scale stay
    double aspect = c.aspect;
    if (widthShare > 0 && heightShare > 0 && (widthShare != 1.0 || heightShare != 1.0)) {
        s.t /= widthShare;
        aspect *= widthShare / heightShare;
    }
    Outside outside = c.border == StabilizeMotion::Border::Black ? Outside::Fill : Outside::Clamp;
    if (c.border == StabilizeMotion::Border::Keep && previous) {
        std::memcpy(dst, previous, size_t(w) * size_t(h) * 4);
        outside = Outside::Keep;
    }
    double m[6];
    StabilizeMotion::pixelMap(s, w, h, aspect, m);
    StabilizeMotion::warpRgba(src, w * 4, dst, w * 4, w, h, m, outside, 0xFF000000u); // opaque black
    return true;
}

} // namespace StabilizeCorrection

// ---- Background queue ----

Stabilizer* Stabilizer::instance()
{
    static Stabilizer* s = new Stabilizer(QCoreApplication::instance());
    return s;
}

Stabilizer::Stabilizer(QObject* parent) : QObject(parent)
{
    m_poll.setInterval(200);
    connect(&m_poll, &QTimer::timeout, this, &Stabilizer::stateChanged);
}

Stabilizer::~Stabilizer()
{
    if (m_thread) {
        *m_cancel = true;
        m_thread->disconnect(this);
        m_thread->wait();
        delete m_thread;
    }
}

Stabilizer::State Stabilizer::state(const Request& r, double* progress) const
{
    if (progress) *progress = 0;
    if (find(r).valid()) {
        if (progress) *progress = 1;
        return State::Done;
    }
    const QString key = r.key();
    if (m_current && m_current->key() == key) {
        if (progress && m_progress) *progress = m_progress->load() / 1000.0;
        return State::Running;
    }
    if (std::any_of(m_queue.begin(), m_queue.end(), [&](const Request& q) { return q.key() == key; })) return State::Queued;
    if (m_cancelled.contains(key)) return State::Cancelled;
    if (m_failed.contains(key)) return State::Failed;
    return State::Missing;
}

QString Stabilizer::error(const Request& r) const
{
    return m_failed.value(r.key());
}

void Stabilizer::sync(const QVector<Request>& requests)
{
    QVector<Request> queue;
    QSet<QString> wanted;
    for (const Request& r : requests) {
        const QString key = r.key();
        if (wanted.contains(key)) continue;
        wanted.insert(key);
        if (m_cancelled.contains(key) || m_failed.contains(key)) continue;
        if (runningUncancelled(key)) continue;
        if (find(r).valid()) continue;
        queue << r;
    }
    const bool stop = m_current && !wanted.contains(m_current->key()) && m_cancel && !m_cancel->load();
    if (queue == m_queue && !stop) return;
    m_queue = queue;
    if (stop) *m_cancel = true; // not needed any more: finishes through the thread, not marked as cancelled
    emit stateChanged();
    next();
}

void Stabilizer::start(const Request& r)
{
    const QString key = r.key();
    m_cancelled.remove(key);
    m_failed.remove(key);
    if (find(r).valid() || runningUncancelled(key)) return;
    if (std::none_of(m_queue.begin(), m_queue.end(), [&](const Request& q) { return q.key() == key; })) m_queue << r;
    emit stateChanged();
    next();
}

// The analysis of `key` runs and goes on. One that is being cancelled still runs until its thread ends: the same
// request again (start right after cancel, effect back by undo) is queued behind it, not dropped.
bool Stabilizer::runningUncancelled(const QString& key) const
{
    return m_current && m_current->key() == key && !(m_cancel && m_cancel->load());
}

void Stabilizer::cancel(const Request& r)
{
    const QString key = r.key();
    m_cancelled.insert(key);
    m_queue.removeIf([&](const Request& q) { return q.key() == key; });
    if (m_current && m_current->key() == key && m_cancel) *m_cancel = true; // finishes through the thread
    emit stateChanged();
}

void Stabilizer::next()
{
    if (m_thread || m_queue.isEmpty()) return;
    const Request r = m_queue.takeFirst();
    if (find(r).valid()) { // done meanwhile (export)
        QTimer::singleShot(0, this, &Stabilizer::next);
        emit resultReady();
        return;
    }
    m_current = r;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_progress = std::make_shared<std::atomic<int>>(0);
    m_error = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    auto cancel = m_cancel;
    auto progress = m_progress;
    auto error = m_error;
    m_thread = QThread::create([r, ok, cancel, progress, error] {
        Background::lowerThisThread(Background::Level::Lowest);
        *ok = analyze(r, error.get(), cancel.get(), progress.get());
    });
    connect(m_thread, &QThread::finished, this, [this, ok] {
        const Request r = *m_current;
        const bool cancelled = m_cancel->load();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
        m_current.reset();
        m_poll.stop();
        if (!*ok && !cancelled) {
            m_failed.insert(r.key(), *m_error);
            qWarning("Stabilize: %s", qPrintable(*m_error));
        }
        emit stateChanged();
        if (*ok) emit resultReady();
        next();
    });
    m_thread->start(QThread::LowestPriority);
    m_poll.start();
    emit stateChanged();
}
