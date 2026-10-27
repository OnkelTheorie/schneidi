#include "engine/Stabilizer.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/Profiles.h"
#include "engine/StabilizeMotion.h"

#include <Mlt.h>
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

QString resourceOf(const Stabilizer::Request& r)
{
    if (r.speed == 1.0) return r.file;
    // timewarp reads the number with atof in the current C locale (see TimelineBuilder::producerFor)
    char num[32];
    std::snprintf(num, sizeof num, "%.10g", r.speed);
    return QString("timewarp:%1:%2").arg(QString::fromLatin1(num), r.file);
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

// ---- The filter that applies the result (own MLT filter like ColorGrade) ----

using StabilizeMotion::Outside;

struct ApplyState {
    std::vector<StabilizeMotion::Similarity> corrections; // per analyzed frame (position from the filter's in)
    double aspect = 16.0 / 9.0;
    StabilizeMotion::Border border = StabilizeMotion::Border::Auto;
};

// Border "keep": the last output picture, its uncovered parts filled from it
struct KeptPicture {
    std::mutex mutex;
    std::vector<uint8_t> data;
    int width = 0, height = 0, pos = -2;
    mlt_image_format format = mlt_image_none;
};

struct FilterData {
    std::atomic<std::shared_ptr<const ApplyState>> state;
    KeptPicture kept;
};

constexpr const char* kDataName = "_schneidi_stabilize";

void destroyData(void* p) { delete static_cast<FilterData*>(p); }

void warp(mlt_image_format format, const uint8_t* src, uint8_t* dst, int w, int h, const ApplyState& st,
          const StabilizeMotion::Similarity& s, Outside outside)
{
    double m[6];
    StabilizeMotion::pixelMap(s, w, h, st.aspect, m);
    if (format == mlt_image_rgba) {
        StabilizeMotion::warpRgba(src, w * 4, dst, w * 4, w, h, m, outside, 0xFF000000u); // opaque black
    } else if (format == mlt_image_rgb) {
        StabilizeMotion::warpRgb(src, w * 3, dst, w * 3, w, h, m, outside);
    } else { // yuv420p: chroma planes at half size, the same map in their pixels
        uint8_t* sp[4] = {};
        uint8_t* dp[4] = {};
        int ss[4] = {}, ds[4] = {};
        mlt_image_format_planes(format, w, h, const_cast<uint8_t*>(src), sp, ss);
        mlt_image_format_planes(format, w, h, dst, dp, ds);
        StabilizeMotion::warpPlane(sp[0], ss[0], dp[0], ds[0], w, h, m, outside, 16);
        const int cw = ss[1], ch = h / 2;
        double mc[6];
        const double sx = double(cw) / w, sy = double(ch) / h;
        // chroma position = luma position * (sx, sy): conjugate the map
        mc[0] = m[0];
        mc[1] = m[1] * sx / sy;
        mc[2] = m[2] * sx;
        mc[3] = m[3] * sy / sx;
        mc[4] = m[4];
        mc[5] = m[5] * sy;
        StabilizeMotion::warpPlane(sp[1], ss[1], dp[1], ds[1], cw, ch, mc, outside, 128);
        StabilizeMotion::warpPlane(sp[2], ss[2], dp[2], ds[2], cw, ch, mc, outside, 128);
    }
}

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    auto* fd = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), kDataName, nullptr));
    const std::shared_ptr<const ApplyState> st = fd ? fd->state.load() : nullptr;
    if (!st || pos < 0 || pos >= int(st->corrections.size()) || st->corrections[size_t(pos)].isIdentity())
        return mlt_frame_get_image(frame, image, format, width, height, 0);
    // Warped in the format the caller wants when it is one of ours (no conversion), else in RGBA
    if (*format != mlt_image_yuv420p && *format != mlt_image_rgba && *format != mlt_image_rgb) *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, 0);
    if (err || !*image) return err;
    if (*format != mlt_image_yuv420p && *format != mlt_image_rgba && *format != mlt_image_rgb) return 0;
    const int w = *width, h = *height;
    if (w < 2 || h < 2) return 0;
    const int size = mlt_image_format_size(*format, w, h, nullptr);
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(size));
    if (!out) return 0;
    const StabilizeMotion::Similarity& s = st->corrections[size_t(pos)];
    Outside outside = st->border == StabilizeMotion::Border::Black ? Outside::Fill : Outside::Clamp;
    if (st->border == StabilizeMotion::Border::Keep) {
        KeptPicture& k = fd->kept;
        std::lock_guard lock(k.mutex);
        // only right after the previous frame (playback/export in order); otherwise edge pixels
        if (k.format == *format && k.width == w && k.height == h && (k.pos == pos - 1 || k.pos == pos)
            && int(k.data.size()) >= size) {
            std::memcpy(out, k.data.data(), size_t(size));
            outside = Outside::Keep;
        }
        warp(*format, *image, out, w, h, *st, s, outside);
        k.data.assign(out, out + size);
        k.width = w;
        k.height = h;
        k.format = *format;
        k.pos = pos;
    } else {
        warp(*format, *image, out, w, h, *st, s, outside);
    }
    // A separate alpha plane moves with the picture
    int alphaSize = 0;
    if (uint8_t* alpha = mlt_frame_get_alpha_size(frame, &alphaSize); alpha && alphaSize == w * h) {
        auto* a = static_cast<uint8_t*>(mlt_pool_alloc(alphaSize));
        double m[6];
        StabilizeMotion::pixelMap(s, w, h, st->aspect, m);
        StabilizeMotion::warpPlane(alpha, w, a, w, w, h, m, outside == Outside::Keep ? Outside::Clamp : outside, 255);
        mlt_frame_set_alpha(frame, a, alphaSize, mlt_pool_release);
    }
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    *image = out;
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

} // namespace

QString Stabilizer::Request::base() const
{
    const QFileInfo fi(file);
    char num[32];
    std::snprintf(num, sizeof num, "%.10g", speed);
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
    return {};
}

std::optional<Stabilizer::Request> Stabilizer::request(const Clip& c, const ProjectFormat& format)
{
    return makeRequest(c, format.rate, format.height > 0 ? double(format.width) / format.height : 0.0);
}

std::optional<Stabilizer::Request> Stabilizer::request(const Clip& c, Mlt::Profile& profile)
{
    // display aspect: an export in another size (same aspect ratio) finds the same analysis
    const double dar = profile.display_aspect_den() > 0 ? double(profile.display_aspect_num()) / profile.display_aspect_den() : 0.0;
    return makeRequest(c, FrameRate{profile.frame_rate_num(), profile.frame_rate_den()}, dar);
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
    ProjectFormat format;
    format.width = r.width;
    format.height = r.height;
    format.rate = r.rate;
    auto profile = makeProfile(format);
    ProducerFactory factory(*profile);
    std::unique_ptr<Mlt::Producer> p = factory.open(resourceOf(r));
    if (!p || !p->is_valid()) return fail(T("Datei lässt sich nicht öffnen: %1").arg(r.file));
    p->set("audio_index", -1);
    const int len = p->get_length();
    const int to = std::min(r.to, len - 1);
    const int from = std::clamp(r.from, 0, std::max(0, to));
    if (to - from < 1) return fail(T("Der Clip ist zu kurz zum Stabilisieren."));

    // Every frame in order: the motion from each one to the next
    std::unique_ptr<StabilizeMotion::Estimator> estimator;
    Motions motions;
    motions.reserve(size_t(to - from + 1));
    for (int pos = from; pos <= to; ++pos) {
        if (cancel && cancel->load()) break;
        p->seek(pos);
        std::unique_ptr<Mlt::Frame> frame(p->get_frame());
        if (!frame) break;
        mlt_image_format fmt = mlt_image_yuv420p;
        int w = r.width, h = r.height;
        const uint8_t* img = frame->get_image(fmt, w, h);
        if (!img || fmt != mlt_image_yuv420p || w < 16 || h < 16) break;
        if (!estimator) estimator = std::make_unique<StabilizeMotion::Estimator>(w, h, r.shakiness, r.accuracy);
        motions.push_back(estimator->push(img, w)); // luma = the first plane
        if (progress) *progress = int(qint64(pos - from + 1) * 1000 / (to - from + 1));
    }
    p.reset();
    if (cancel && cancel->load()) return fail(T("Abgebrochen."));
    if (int(motions.size()) != to - from + 1) return fail(T("Die Analyse ist fehlgeschlagen: %1").arg(QFileInfo(r.file).fileName()));

    QDir().mkpath(cacheDir());
    removeOldResults();
    const QString target = QDir(cacheDir()).filePath(QString("%1_%2_%3%4").arg(r.base()).arg(from).arg(to).arg(kSuffix));
    if (QFileInfo::exists(target)) return true; // the same analysis finished elsewhere meanwhile (export + preview)
    // Only visible under the final name once complete -> nobody reads a half written file
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(writeMotions(motions)) < 0 || !f.commit())
        return fail(T("Das Analyse-Ergebnis konnte nicht gespeichert werden: %1").arg(target));
    return true;
}

bool Stabilizer::attach(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c)
{
    const EffectInstance* e = EffectRegistry::instance(c, EffectId);
    if (!e || !e->enabled) return false;
    const auto r = request(c, profile);
    if (!r) return false;
    const Result res = find(*r);
    if (!res.valid()) return false;
    const auto motions = loadMotions(res.path);
    if (!motions || int(motions->size()) != res.to - res.from + 1) return false;
    StabilizeMotion::PathParams pp;
    pp.smoothing = std::clamp(int(std::lround(EffectRegistry::value(c, EffectId, "smoothing").toDouble())), 0, 100);
    pp.zoom = std::clamp(EffectRegistry::value(c, EffectId, "zoom").toDouble(), -90.0, 500.0);
    const QString border = EffectRegistry::value(c, EffectId, "border").toString();
    using StabilizeMotion::Border;
    pp.border = border == "adaptive" ? Border::Adaptive : border == "black" ? Border::Black : border == "keep" ? Border::Keep : Border::Auto;
    pp.aspect = profile.dar() > 0 ? profile.dar() : 16.0 / 9.0;
    auto st = std::make_shared<ApplyState>();
    st->corrections = StabilizeMotion::corrections(*motions, pp);
    st->aspect = pp.aspect;
    st->border = pp.border;
    mlt_filter f = mlt_filter_new();
    if (!f) return false;
    f->process = process;
    auto* fd = new FilterData;
    fd->state.store(std::move(st));
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), kDataName, fd, 0, destroyData, nullptr);
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    // positions count from the analyzed range (frames outside it, e.g. far handles, stay as they are)
    filter.set_in_and_out(res.from, res.to);
    cut.attach(filter);
    return true;
}

bool Stabilizer::transfer(mlt_filter live, mlt_filter fresh)
{
    if (live->process != process || fresh->process != process) return false;
    auto* l = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(live), kDataName, nullptr));
    auto* f = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(fresh), kDataName, nullptr));
    if (!l || !f) return false;
    l->state.store(f->state.load());
    return true;
}

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
        if (m_current && m_current->key() == key) continue;
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
    if (find(r).valid() || (m_current && m_current->key() == key)) return;
    if (std::none_of(m_queue.begin(), m_queue.end(), [&](const Request& q) { return q.key() == key; })) m_queue << r;
    emit stateChanged();
    next();
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
