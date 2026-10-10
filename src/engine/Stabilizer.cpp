#include "engine/Stabilizer.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "engine/Profiles.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace {

constexpr int kVersion = 1;          // bump when the analysis changes (old results are not reused)
constexpr int kAnalysisSide = 1280;  // longer side of the analysis frames (vid.stab scales the result to any size)

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

// "<base>_<from>_<to>.trf" -> range
bool parseName(const QString& name, const QString& base, int* from, int* to)
{
    if (!name.startsWith(base + '_') || !name.endsWith(".trf")) return false;
    const QStringList parts = name.mid(base.size() + 1).chopped(4).split('_');
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

bool Stabilizer::available()
{
    if (!mlt_factory_repository()) return false; // MLT not started (yet)
    static const bool ok = [] {
        Mlt::Profile p;
        Mlt::Filter f(p, "vidstab");
        return f.is_valid();
    }();
    return ok;
}

QString Stabilizer::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/stabilize";
}

QString Stabilizer::unsupportedReason(const Clip& c)
{
    if (c.isTitle()) return T("Titel lassen sich nicht stabilisieren.");
    if (c.isCompound()) return T("Compound Clips lassen sich nicht stabilisieren.");
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
    for (const QString& name : dir.entryList({base + "_*.trf"}, QDir::Files)) {
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
    if (!available()) return fail(T("Das MLT-Modul „vid.stab“ fehlt – Stabilisieren ist nicht möglich."));
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

    QDir().mkpath(cacheDir());
    const QString target = QDir(cacheDir()).filePath(QString("%1_%2_%3.trf").arg(r.base()).arg(from).arg(to));
    const QString part = target + ".part-" + QUuid::createUuid().toString(QUuid::Id128);
    bool done = false;
    {
        Mlt::Filter f(*profile, "vidstab");
        if (!f.is_valid()) return fail(T("Das MLT-Modul „vid.stab“ fehlt – Stabilisieren ist nicht möglich."));
        f.set("filename", QDir::toNativeSeparators(part).toUtf8().constData());
        f.set("shakiness", r.shakiness);
        f.set("accuracy", r.accuracy);
        // Filter range = analyzed range: vid.stab counts frames from its in point (the timeline attaches the same way)
        f.set_in_and_out(from, to);
        p->attach(f);
        // vid.stab requires every frame in order, from its first one
        for (int pos = from; pos <= to; ++pos) {
            if (cancel && cancel->load()) break;
            p->seek(pos);
            std::unique_ptr<Mlt::Frame> frame(p->get_frame());
            if (!frame) break;
            mlt_image_format fmt = mlt_image_yuv420p;
            int w = r.width, h = r.height;
            if (!frame->get_image(fmt, w, h)) break;
            if (progress) *progress = int(qint64(pos - from + 1) * 1000 / (to - from + 1));
        }
        const char* results = f.get("results"); // set by vid.stab after the last frame
        done = results && *results && !(cancel && cancel->load());
        p->detach(f);
    }
    p.reset();
    if (!done) {
        QFile::remove(part);
        if (cancel && cancel->load()) return fail(T("Abgebrochen."));
        return fail(T("Die Analyse ist fehlgeschlagen: %1").arg(QFileInfo(r.file).fileName()));
    }
    // Only now visible under the final name -> nobody reads a half written file
    if (QFileInfo::exists(target)) { // the same analysis finished elsewhere meanwhile (export + preview)
        QFile::remove(part);
        return true;
    }
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        return fail(T("Das Analyse-Ergebnis konnte nicht gespeichert werden: %1").arg(target));
    }
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
    Mlt::Filter f(profile, "vidstab");
    if (!f.is_valid()) return false;
    f.set("results", QDir::toNativeSeparators(res.path).toUtf8().constData());
    // integers only: MLT would write other numbers as text in the current locale
    f.set("smoothing", std::clamp(int(std::lround(EffectRegistry::value(c, EffectId, "smoothing").toDouble())), 0, 100));
    f.set("zoom", std::clamp(int(std::lround(EffectRegistry::value(c, EffectId, "zoom").toDouble())), -500, 500));
    const QString border = EffectRegistry::value(c, EffectId, "border").toString();
    f.set("optzoom", border == "adaptive" ? 2 : border == "black" || border == "keep" ? 0 : 1);
    f.set("crop", border == "black" ? 1 : 0);
    // positions count from the analyzed range (frames outside it, e.g. far handles, stay as they are)
    f.set_in_and_out(res.from, res.to);
    cut.attach(f);
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
    if (!available()) return;
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
    if (!available() || find(r).valid() || (m_current && m_current->key() == key)) return;
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
