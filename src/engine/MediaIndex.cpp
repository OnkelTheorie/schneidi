// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MediaIndex.h"

#include "core/PathKey.h"
#include "engine/FaceDetect.h"
#include "engine/OpenMedia.h"
#include "engine/video/Scan.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

namespace MediaIndex {

namespace {

constexpr int kWidth = 320;          // analysis picture width (as `scenes`)
constexpr double kCutScore = 0.3;    // `scenes` default threshold
constexpr double kMinShot = 0.5;     // `scenes` default 'min'
constexpr double kPanRate = 5;       // pan checks per second
constexpr double kFaceRate = 1;      // face detections per second (YuNet: ~25 ms per picture on 2 cores)
constexpr double kAudioPiece = 30;   // files without a picture: "shots" of this length
// Laplacian/gradient ratio -> sharpness 0..1: below kSharpFloor nothing is sharp (a 10 px blur on 640 px: 0.4), from
// kSharpFloor + kSharpSpan on fully (crisp test pictures 1.5, 8-bit noise of a smooth gradient keeps it above 0)
constexpr double kSharpFloor = 0.3, kSharpSpan = 1.0;

QMutex g_mutex;
QHash<QString, Entry>& memory()
{
    static QHash<QString, Entry> m;
    return m;
}
std::function<QVector<AudioEvents::Event>(const QString&)>& eventSource()
{
    static std::function<QVector<AudioEvents::Event>(const QString&)> s;
    return s;
}

// For tests: the way taken as AudioEvents::way() and the way the event source answers with (empty = the real ones)
QString g_testWay, g_testAnswered;

QString currentWay()
{
    {
        QMutexLocker lock(&g_mutex);
        if (!g_testWay.isEmpty()) return g_testWay;
    }
    return AudioEvents::way();
}

QString hashOf(const QString& s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// How the audio events were made and how faces were found are part of the key: installing or updating the
// audio-events or the face model measures anew
QString keyOf(const QFileInfo& fi) { return cacheKey(fi.absoluteFilePath(), currentWay(), FaceDetect::way()); }

double round3(double v) { return std::round(v * 1000) / 1000; }

// ---------- Measuring ----------

struct FrameStat {
    double seconds = 0;
    double mad = -1;   // mean absolute luma difference to the picture before (-1: none)
    double luma = 0;   // mean, 0..1
    double sharp = -1; // Laplacian / gradient ratio (-1: too flat to say)
    double skin = 0;   // share of skin-coloured pixels
    int pan = -1;      // moved as a whole against the picture before: 1, not: 0, not checked: -1
    int faces = -1;    // faces found (YuNet), -1: not looked
    FaceDetect::Face largest; // the largest of them (fractions)
};

double meanDifference(const Video::Scan::Frame& a, const Video::Scan::Frame& b)
{
    if (a.width != b.width || a.height != b.height) return 0;
    uint64_t sad = 0;
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* x = a.data[0] + size_t(y) * size_t(a.linesize[0]);
        const uint8_t* z = b.data[0] + size_t(y) * size_t(b.linesize[0]);
        uint32_t row = 0;
        for (int i = 0; i < a.width; ++i) row += uint32_t(std::abs(int(x[i]) - int(z[i])));
        sad += row;
    }
    return a.width > 0 && a.height > 0 ? double(sad) / (double(a.width) * a.height) : 0.0;
}

// Sum of |Laplacian| over the sum of |gradient|: a sharp edge has its whole step in one pixel (ratio ~2), a blurred
// one spreads it (ratio ~ 2 / blur width). Independent of contrast and of how many edges there are.
double sharpness(const Video::Scan::Frame& f)
{
    const int w = f.width, h = f.height, ls = f.linesize[0];
    const uint8_t* p = f.data[0];
    uint64_t lap = 0, grad = 0;
    for (int y = 1; y < h - 1; ++y) {
        const uint8_t* r = p + size_t(y) * size_t(ls);
        for (int x = 1; x < w - 1; ++x) {
            const int c = r[x];
            lap += uint64_t(std::abs(4 * c - r[x - 1] - r[x + 1] - r[x - ls] - r[x + ls]));
            grad += uint64_t(std::abs(c - r[x + 1]) + std::abs(c - r[x + ls]));
        }
    }
    const double pixels = double(std::max(1, (w - 2) * (h - 2)));
    if (grad / pixels < 0.2) return -1; // flat (black, a single colour): no edges to judge
    return double(lap) / double(grad);
}

// YCbCr box around skin tones (all complexions; a hint, warm walls count too)
double skinShare(const Video::Scan::Frame& f)
{
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    int64_t n = 0, skin = 0;
    for (int y = 0; y < ch; ++y) {
        const uint8_t* u = f.data[1] + size_t(y) * size_t(f.linesize[1]);
        const uint8_t* v = f.data[2] + size_t(y) * size_t(f.linesize[2]);
        const uint8_t* l = f.data[0] + size_t(std::min(f.height - 1, 2 * y)) * size_t(f.linesize[0]);
        for (int x = 0; x < cw; ++x, ++n)
            if (u[x] >= 77 && u[x] <= 127 && v[x] >= 133 && v[x] <= 173 && l[std::min(f.width - 1, 2 * x)] > 60) ++skin;
    }
    // Most of the picture in skin tones is a background (sand, wood, a warm wall), not a face filling it
    const double share = n ? double(skin) / double(n) : 0.0;
    return share > 0.6 ? 0.0 : share;
}

// Does the picture match its predecessor better shifted as a whole (a camera pan) than in place?
int panOf(const Video::Scan::Frame& a, const Video::Scan::Frame& b)
{
    if (a.width != b.width || a.height != b.height) return -1;
    constexpr int kDx = 8, kDy = 4;
    const int w = a.width, h = a.height;
    const auto sadAt = [&](int dx, int dy) {
        uint64_t sad = 0;
        int n = 0;
        for (int y = kDy; y < h - kDy; y += 2) {
            const uint8_t* ra = a.data[0] + size_t(y) * size_t(a.linesize[0]);
            const uint8_t* rb = b.data[0] + size_t(y + dy) * size_t(b.linesize[0]);
            for (int x = kDx; x < w - kDx; x += 4, ++n) sad += uint64_t(std::abs(int(ra[x]) - int(rb[x + dx])));
        }
        return n ? double(sad) / n : 0.0;
    };
    const double still = sadAt(0, 0);
    if (still < 1.5) return 0; // nothing moves
    double best = still;
    int bx = 0, by = 0;
    for (int dy = -kDy; dy <= kDy; ++dy)
        for (int dx = -kDx; dx <= kDx; ++dx) {
            if (!dx && !dy) continue;
            const double s = sadAt(dx, dy);
            if (s < best) {
                best = s;
                bx = dx;
                by = dy;
            }
        }
    return (bx || by) && best < 0.6 * still ? 1 : 0;
}

struct Probe {
    double duration = 0;
    bool video = false;
};

std::optional<Probe> probe(const QString& path)
{
    AVFormatContext* raw = nullptr;
    if (OpenMedia::input(&raw, path.toUtf8().constData()) < 0) return std::nullopt;
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmt(raw, [](AVFormatContext* f) {
        avformat_close_input(&f);
    });
    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
    Probe p;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            p.video = true;
    }
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) p.duration = double(fmt->duration) / AV_TIME_BASE;
    return p;
}

Quality qualityOf(const std::vector<FrameStat>& stats, size_t first, size_t last)
{
    Quality q;
    double luma = 0, motion = 0, skin = 0;
    int motionN = 0, panN = 0, panYes = 0;
    std::vector<double> sharp;
    for (size_t i = first; i < last; ++i) {
        const FrameStat& s = stats[i];
        luma += s.luma;
        skin += s.skin;
        if (i > first && s.mad >= 0) { // not across the cut into the shot
            motion += s.mad;
            ++motionN;
        }
        if (s.sharp >= 0) sharp.push_back(s.sharp);
        if (s.pan >= 0 && i > first) {
            ++panN;
            panYes += s.pan;
        }
    }
    const double n = double(std::max<size_t>(1, last - first));
    q.bright = round3(luma / n);
    q.skin = round3(skin / n);
    q.motion = std::round((motionN ? motion / motionN : 0.0) * 10) / 10;
    if (!sharp.empty()) {
        std::nth_element(sharp.begin(), sharp.begin() + sharp.size() / 2, sharp.end());
        q.sharp = round3(std::clamp((sharp[sharp.size() / 2] - kSharpFloor) / kSharpSpan, 0.0, 1.0));
        q.blurry = q.sharp < 0.3;
    }
    q.dark = q.bright < 0.2;
    q.overexposed = q.bright > 0.85;
    q.pan = panN >= 2 && panYes * 2 >= panN;
    q.face = q.skin >= 0.08 ? "likely" : q.skin >= 0.02 ? "maybe" : "no";
    // Faces: the most in one looked-at picture, the largest box among them (a shot without a looked-at picture,
    // shorter than 1 s, gets the nearest picture before it: the same scene, most likely)
    const auto takeFaces = [&q](const FrameStat& s) {
        q.faces = std::max(q.faces, s.faces);
        if (s.faces > 0 && s.largest.w * s.largest.h > q.faceW * q.faceH) {
            q.faceX = round3(s.largest.x);
            q.faceY = round3(s.largest.y);
            q.faceW = round3(s.largest.w);
            q.faceH = round3(s.largest.h);
        }
    };
    for (size_t i = first; i < last; ++i) takeFaces(stats[i]);
    for (size_t i = first; q.faces < 0 && i-- > 0;)
        if (stats[i].faces >= 0) takeFaces(stats[i]);
    return q;
}

// Shots of the measured pictures: cuts as `scenes` finds them
QVector<Shot> shotsOf(std::vector<FrameStat>& stats, double duration)
{
    std::sort(stats.begin(), stats.end(), [](const FrameStat& a, const FrameStat& b) { return a.seconds < b.seconds; });
    QVector<size_t> starts{0};
    double before = 0, sceneStart = stats.empty() ? 0 : stats.front().seconds;
    for (size_t i = 0; i < stats.size(); ++i) {
        if (stats[i].mad < 0) continue;
        const double mad = stats[i].mad;
        const double score = std::clamp(std::min(mad, std::abs(mad - before)) / 100.0, 0.0, 1.0);
        before = mad;
        if (score > kCutScore && i > 0 && stats[i].seconds - sceneStart >= kMinShot && duration - stats[i].seconds >= 0.04) {
            starts << i;
            sceneStart = stats[i].seconds;
        }
    }
    QVector<Shot> shots;
    for (int k = 0; k < starts.size(); ++k) {
        const size_t a = starts[k], b = k + 1 < starts.size() ? starts[k + 1] : stats.size();
        Shot s;
        s.n = k + 1;
        s.from = k == 0 ? 0.0 : round3(stats[a].seconds);
        s.to = b < stats.size() ? round3(stats[b].seconds) : round3(std::max(duration, stats.back().seconds));
        s.thumb = round3(stats[a + (b - a) / 2].seconds);
        s.quality = qualityOf(stats, a, b);
        shots << s;
    }
    return shots;
}

QJsonObject eventJson(const AudioEvents::Event& e)
{
    return {{"kind", e.kind}, {"from_s", round3(e.from)}, {"to_s", round3(e.to)}, {"conf", std::round(e.conf * 100) / 100}};
}

QJsonObject toJson(const Entry& e, const QVector<AudioEvents::Event>& events)
{
    QJsonArray shots, evs;
    for (const Shot& s : e.shots) {
        const Quality& q = s.quality;
        QJsonObject o{{"n", s.n},          {"from", s.from},       {"to", s.to},
                      {"thumb", s.thumb},  {"sharp", q.sharp},     {"bright", q.bright},
                      {"motion", q.motion}, {"skin", q.skin},      {"dark", q.dark},
                      {"overexposed", q.overexposed}, {"blurry", q.blurry}, {"pan", q.pan},
                      {"face", q.face}};
        if (q.faces >= 0) o["faces"] = q.faces;
        if (q.faces > 0) o["face_box"] = QJsonArray{q.faceX, q.faceY, q.faceW, q.faceH};
        shots << o;
    }
    for (const AudioEvents::Event& ev : events) evs << eventJson(ev);
    return {{"version", kVersion}, {"file", e.file}, {"duration", e.duration}, {"video", e.video},
            {"shots", shots}, {"events", evs}};
}

void distribute(Entry& e, const QVector<AudioEvents::Event>& events)
{
    for (Shot& s : e.shots) {
        s.events.clear();
        for (const AudioEvents::Event& ev : events)
            if (ev.to > s.from && ev.from < s.to) s.events << ev;
    }
}

std::optional<Entry> fromJson(const QJsonObject& o)
{
    if (o["version"].toInt() != kVersion) return std::nullopt;
    Entry e;
    e.file = o["file"].toString();
    e.duration = o["duration"].toDouble();
    e.video = o["video"].toBool();
    for (const QJsonValue& v : o["shots"].toArray()) {
        const QJsonObject j = v.toObject();
        Shot s;
        s.n = j["n"].toInt();
        s.from = j["from"].toDouble();
        s.to = j["to"].toDouble();
        s.thumb = j["thumb"].toDouble();
        Quality& q = s.quality;
        q.sharp = j["sharp"].toDouble();
        q.bright = j["bright"].toDouble();
        q.motion = j["motion"].toDouble();
        q.skin = j["skin"].toDouble();
        q.dark = j["dark"].toBool();
        q.overexposed = j["overexposed"].toBool();
        q.blurry = j["blurry"].toBool();
        q.pan = j["pan"].toBool();
        q.face = j["face"].toString();
        q.faces = j["faces"].toInt(-1);
        if (const QJsonArray box = j["face_box"].toArray(); box.size() == 4) {
            q.faceX = box[0].toDouble();
            q.faceY = box[1].toDouble();
            q.faceW = box[2].toDouble();
            q.faceH = box[3].toDouble();
        }
        e.shots << s;
    }
    QVector<AudioEvents::Event> events;
    for (const QJsonValue& v : o["events"].toArray()) {
        const QJsonObject j = v.toObject();
        events << AudioEvents::Event{j["kind"].toString(), j["from_s"].toDouble(), j["to_s"].toDouble(), j["conf"].toDouble()};
    }
    distribute(e, events);
    return e;
}

std::optional<Entry> loadDisk(const QString& key)
{
    QFile f(cacheDir() + '/' + key + ".json");
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // YAMNet failed on the file and the heuristics answered: the entry lies under the heuristic way (one step only)
    if (const QString other = o["same_as"].toString(); !other.isEmpty() && other != key) {
        if (o["version"].toInt() != kVersion) return std::nullopt;
        QFile g(cacheDir() + '/' + other + ".json");
        if (!g.open(QIODevice::ReadOnly)) return std::nullopt;
        return fromJson(QJsonDocument::fromJson(g.readAll()).object());
    }
    return fromJson(o);
}

void saveJson(const QString& file, const QJsonObject& o)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0) out.commit();
}

// faces: whether faces were looked for (the face model ran)
std::optional<Entry> measure(const QString& path, const Options& options, QString* error, bool* faces)
{
    *faces = false;
    const auto info = probe(path);
    if (!info) {
        if (error) *error = "cannot read media: " + path;
        return std::nullopt;
    }
    Entry e;
    e.file = path;
    e.video = info->video;
    e.duration = round3(info->duration);
    if (info->video) {
        Video::Scan::Options o;
        o.width = kWidth;
        o.height = -2;
        o.format = AV_PIX_FMT_YUV420P;
        std::mutex mutex;
        std::vector<FrameStat> stats;
        // Faces (YuNet), when installed: one session for all decoding threads, one picture at a time (the runtime
        // uses several cores itself). A model that fails on a picture is not asked again (the entry gets no faces).
        std::unique_ptr<FaceDetect::Detector> detector = FaceDetect::available() ? FaceDetect::Detector::open() : nullptr;
        std::mutex faceMutex;
        bool faceFailed = false;
        const auto findFaces = [&](const Video::Scan::Frame& f, FrameStat& s) {
            const std::vector<uint8_t> bgr = FaceDetect::bgrFromYuv420(f.data, f.linesize, f.width, f.height, f.fullRange);
            std::lock_guard lock(faceMutex);
            if (faceFailed) return;
            const auto found = detector->detect(bgr.data(), f.width, f.height, f.width * 3);
            if (!found) {
                faceFailed = true;
                return;
            }
            s.faces = int(found->size());
            for (const FaceDetect::Face& face : *found)
                if (face.w * face.h > s.largest.w * s.largest.h) s.largest = face;
        };
        const Video::Scan::Result r = Video::Scan::run(
            path, o,
            [&](const Video::Scan::Frame& f, const Video::Scan::Frame* previous) {
                FrameStat s;
                s.seconds = f.seconds;
                uint64_t sum = 0;
                for (int y = 0; y < f.height; ++y) {
                    const uint8_t* row = f.data[0] + size_t(y) * size_t(f.linesize[0]);
                    uint32_t rs = 0;
                    for (int x = 0; x < f.width; ++x) rs += row[x];
                    sum += rs;
                }
                const double mean = f.width && f.height ? double(sum) / (double(f.width) * f.height) : 0.0;
                s.luma = std::clamp(f.fullRange ? mean / 255.0 : (mean - 16.0) / 219.0, 0.0, 1.0);
                s.sharp = sharpness(f);
                s.skin = skinShare(f);
                if (previous) {
                    s.mad = meanDifference(f, *previous);
                    if (std::floor(f.seconds * kPanRate) != std::floor(previous->seconds * kPanRate))
                        s.pan = panOf(f, *previous);
                }
                if (detector && (!previous || std::floor(f.seconds * kFaceRate) != std::floor(previous->seconds * kFaceRate)))
                    findFaces(f, s);
                std::lock_guard lock(mutex);
                stats.push_back(s);
            },
            [&](double p) { return !options.progress || options.progress(p * 0.9); });
        if (!r.ok) {
            if (error) *error = r.canceled ? QString("cancelled") : "could not read the video of " + path + ": " + r.error;
            return std::nullopt;
        }
        if (stats.empty()) {
            if (error) *error = "no pictures in " + path;
            return std::nullopt;
        }
        if (e.duration <= 0) e.duration = round3(std::max_element(stats.begin(), stats.end(), [](auto& a, auto& b) {
                                                     return a.seconds < b.seconds;
                                                 })->seconds);
        if (faceFailed) // all or nothing: a half-measured file would look like one without faces
            for (FrameStat& s : stats) s.faces = -1;
        *faces = detector && !faceFailed;
        e.shots = shotsOf(stats, e.duration);
    } else {
        // Sound only (a podcast, a song): pieces of fixed length, for the speech and the events in them
        const int pieces = std::max(1, int(std::ceil(e.duration / kAudioPiece - 1e-6)));
        for (int k = 0; k < pieces; ++k) {
            Shot s;
            s.n = k + 1;
            s.from = k * kAudioPiece;
            s.to = std::min(e.duration, (k + 1) * kAudioPiece);
            s.thumb = s.from;
            s.quality.face = "no";
            e.shots << s;
        }
    }
    return e;
}

void applyNotes(Entry& e)
{
    const QVector<Note> ns = notes(e.file);
    for (Shot& s : e.shots) {
        const Note* best = nullptr;
        double bestShare = 0.5;
        for (const Note& n : ns) {
            const double overlap = std::min(s.to, n.to) - std::max(s.from, n.from);
            const double share = overlap / std::max({s.to - s.from, n.to - n.from, 1e-3});
            if (share >= bestShare) {
                bestShare = share;
                best = &n;
            }
        }
        s.desc = best ? best->desc : QString();
        s.tags = best ? best->tags : QStringList();
    }
}

QString notesFile(const QString& file) { return notesDir() + '/' + hashOf(PathKey::of(file)) + ".json"; }

// The notes by the file's content: found when its path has none - a moved project folder, another drive letter.
// File name "c-<name+size>-<name+size+first MB>.json": the first part needs only the file's name and size, so the first
// MB is read only for a file that has a stored copy of its name and size (one per file and process).
std::atomic<int> g_contentReads{0};
QString contentPrefix(const QFileInfo& fi) { return "c-" + hashOf(QString("%1|%2").arg(fi.fileName()).arg(fi.size())).left(16); }

QString contentNotesFile(const QString& file)
{
    const QFileInfo fi(file);
    const QString key = QString("%1|%2|%3").arg(PathKey::of(file)).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    static QMutex mutex;
    static QHash<QString, QString> known;
    {
        QMutexLocker lock(&mutex);
        if (const auto it = known.constFind(key); it != known.constEnd()) return *it;
    }
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    ++g_contentReads;
    // A short read (a network drive gone, the file locked) is no key: nothing is kept, the next call reads again
    const QByteArray data = f.read(1 << 20);
    if (data.size() != std::min<qint64>(fi.size(), 1 << 20)) return {};
    const QByteArray head = QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex();
    const QString path = notesDir() + '/' + contentPrefix(fi) + '-'
                         + hashOf(QString("%1|%2|%3").arg(fi.fileName()).arg(fi.size()).arg(QString::fromLatin1(head))) + ".json";
    QMutexLocker lock(&mutex);
    known.insert(key, path);
    return path;
}

// The name+size parts of the stored content copies, listed once and again when the folder changed (annotate here or
// in another process) - a Media Pool search asks for every file on every key press
QMutex g_contentMutex;
QSet<QString> g_contentPrefixes;
QDateTime g_contentListed;
bool g_contentValid = false;

bool hasContentCopy(const QFileInfo& fi)
{
    const QDateTime changed = QFileInfo(notesDir()).lastModified();
    QMutexLocker lock(&g_contentMutex);
    if (!g_contentValid || changed != g_contentListed) {
        g_contentPrefixes.clear();
        for (const QString& name : QDir(notesDir()).entryList({"c-*.json"}, QDir::Files))
            g_contentPrefixes.insert(name.section('-', 0, 1));
        g_contentListed = changed;
        g_contentValid = true;
    }
    return g_contentPrefixes.contains(contentPrefix(fi));
}

void contentChanged()
{
    QMutexLocker lock(&g_contentMutex);
    g_contentValid = false;
}

} // namespace

QString cacheKey(const QString& file, const QString& audioWay, const QString& faceWay)
{
    const QFileInfo fi(file);
    QString id = QString("%1|%2|%3|%4|%5")
                     .arg(PathKey::of(file))
                     .arg(fi.size())
                     .arg(fi.lastModified().toMSecsSinceEpoch())
                     .arg(kVersion)
                     .arg(audioWay);
    if (!faceWay.isEmpty()) id += "|faces:" + faceWay; // none: the key as before faces were measured
    return hashOf(id);
}

int contentReadCount() { return g_contentReads.load(); }

QString cacheDir() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/index"; }
QString notesDir() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/index"; }

std::optional<Entry> cached(const QString& file)
{
    const QFileInfo fi(file);
    if (!fi.isFile()) return std::nullopt;
    const QString key = keyOf(fi);
    std::optional<Entry> e;
    {
        QMutexLocker lock(&g_mutex);
        if (const auto it = memory().constFind(key); it != memory().constEnd()) e = *it;
    }
    if (!e) {
        e = loadDisk(key);
        if (!e) return std::nullopt;
        e->file = fi.absoluteFilePath();
        QMutexLocker lock(&g_mutex);
        memory().insert(key, *e);
    }
    e->cached = true;
    applyNotes(*e);
    return e;
}

std::optional<Entry> build(const QString& file, const Options& options, QString* error)
{
    const QFileInfo fi(file);
    if (!fi.isFile()) {
        if (error) *error = "file not found: " + file;
        return std::nullopt;
    }
    if (!options.refresh)
        if (auto e = cached(file)) return e;
    bool faces = false;
    std::optional<Entry> e = measure(fi.absoluteFilePath(), options, error, &faces);
    if (!e) return std::nullopt;
    // Keyed as the face model says now, also when it failed on this file (a broken model file, a picture it cannot
    // read): cached() looks there, and a failure measured again would fail again (a new model file changes the key)
    const QString faceWay = FaceDetect::way();
    (void)faces;
    std::function<QVector<AudioEvents::Event>(const QString&)> source;
    {
        QMutexLocker lock(&g_mutex);
        source = eventSource();
    }
    // The sound part asks for a cancel too (a sound-only file never did: closing the app waited for all of it)
    const auto cancelled = [&] {
        if (error) *error = "canceled";
        return std::nullopt;
    };
    if (options.progress && !options.progress(0.9)) return cancelled();
    QVector<AudioEvents::Event> events;
    bool incomplete = false;
    const QString current = currentWay();
    QString way = current; // the way the events were made (YAMNet failing on a file: the heuristics)
    if (source) {
        events = source(e->file);
        QMutexLocker lock(&g_mutex);
        if (!g_testAnswered.isEmpty()) way = g_testAnswered;
    } else {
        AudioEvents::Options ao;
        if (options.progress) ao.progress = [&options](double p) { return options.progress(0.9 + 0.1 * std::clamp(p, 0.0, 1.0)); };
        const AudioEvents::Result r = AudioEvents::analyzeFile(e->file, 0, ao);
        if (options.progress && r.error.contains("cancel", Qt::CaseInsensitive)) return cancelled();
        events = r.events;
        incomplete = r.incomplete;
        if (r.by == "heuristic") way = AudioEvents::wayKey({});
    }
    distribute(*e, events);
    if (!incomplete) { // a read error is not the file's sound: measured again next time
        const QString key = cacheKey(fi.absoluteFilePath(), way, faceWay);
        saveJson(cacheDir() + '/' + key + ".json", toJson(*e, events));
        // Found under the way of now too (cached() looks there): a pointer to it, not a copy labelled YAMNet
        const QString currentKey = cacheKey(fi.absoluteFilePath(), current, faceWay);
        if (currentKey != key)
            saveJson(cacheDir() + '/' + currentKey + ".json", QJsonObject{{"version", kVersion}, {"same_as", key}});
        QMutexLocker lock(&g_mutex);
        memory().insert(key, *e);
        memory().insert(currentKey, *e);
    }
    if (options.progress) options.progress(1.0);
    applyNotes(*e);
    return e;
}

void addSpeech(Entry& e, const QVector<TranscriptWord>& words)
{
    for (Shot& s : e.shots) {
        QStringList said;
        for (const TranscriptWord& w : words) {
            const double at = w.from / 1000.0;
            if (at >= s.from && at < s.to) said << w.text;
        }
        if (!said.isEmpty()) s.speech = (s.speech.isEmpty() ? QString() : s.speech + ' ') + said.join(' ');
    }
}

QVector<Note> notes(const QString& file)
{
    QFile f(notesFile(file));
    if (!f.open(QIODevice::ReadOnly)) {
        // No notes under this path: the same file elsewhere (read only when a copy of its name and size exists)
        const QFileInfo fi(file);
        if (!fi.isFile() || !hasContentCopy(fi)) return {};
        f.setFileName(contentNotesFile(file));
        if (f.fileName().isEmpty() || !f.open(QIODevice::ReadOnly)) return {};
    }
    QVector<Note> out;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object()["notes"].toArray()) {
        const QJsonObject o = v.toObject();
        Note n;
        n.shot = o["shot"].toInt();
        n.from = o["from"].toDouble();
        n.to = o["to"].toDouble();
        n.desc = o["desc"].toString();
        for (const QJsonValue& t : o["tags"].toArray()) n.tags << t.toString();
        out << n;
    }
    return out;
}

bool annotate(Entry& e, int shot, const std::optional<QString>& desc, const std::optional<QStringList>& tags,
              QString* error)
{
    Shot* s = nullptr;
    for (Shot& x : e.shots)
        if (x.n == shot) s = &x;
    if (!s) {
        if (error) *error = QString("%1 has no shot %2 (it has %3)").arg(e.file).arg(shot).arg(e.shots.size());
        return false;
    }
    if (desc) s->desc = desc->trimmed();
    if (tags) {
        s->tags.clear();
        for (const QString& t : *tags)
            if (!t.trimmed().isEmpty() && !s->tags.contains(t.trimmed())) s->tags << t.trimmed();
    }
    QMutexLocker lock(&g_mutex); // one writer at a time per process (the file is replaced atomically)
    QVector<Note> ns = notes(e.file);
    // The shot's earlier note goes (also one written for a shot that overlaps it mostly)
    ns.erase(std::remove_if(ns.begin(), ns.end(),
                            [&](const Note& n) {
                                const double overlap = std::min(s->to, n.to) - std::max(s->from, n.from);
                                return overlap / std::max({s->to - s->from, n.to - n.from, 1e-3}) >= 0.5;
                            }),
             ns.end());
    if (!s->desc.isEmpty() || !s->tags.isEmpty()) ns << Note{s->from, s->to, s->n, s->desc, s->tags};
    QJsonArray arr;
    for (const Note& n : ns)
        arr << QJsonObject{{"shot", n.shot}, {"from", n.from}, {"to", n.to}, {"desc", n.desc},
                           {"tags", QJsonArray::fromStringList(n.tags)}};
    // By path, and a copy by content (name, size, first MB) for the file found under another path later
    const QByteArray json = QJsonDocument(QJsonObject{{"file", e.file}, {"notes", arr}}).toJson(QJsonDocument::Indented);
    for (const QString& path : {notesFile(e.file), contentNotesFile(e.file)}) {
        if (path.isEmpty()) continue; // unreadable now: by path only
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(json) < 0 || !out.commit()) {
            if (error) *error = "cannot write " + QDir::toNativeSeparators(path);
            contentChanged();
            return false;
        }
    }
    contentChanged();
    return true;
}

QVector<IndexSearch::Shot> searchable(const Entry& e)
{
    QVector<IndexSearch::Shot> out;
    for (const Shot& s : e.shots) {
        IndexSearch::Shot x;
        x.file = e.file;
        x.n = s.n;
        x.from = s.from;
        x.to = s.to;
        x.desc = s.desc;
        x.tags = s.tags;
        x.speech = s.speech;
        for (const AudioEvents::Event& ev : s.events) x.events << IndexSearch::Event{ev.kind, ev.from, ev.to, ev.conf};
        const Quality& q = s.quality;
        x.motion = q.motion;
        x.dark = q.dark;
        x.overexposed = q.overexposed;
        x.blurry = q.blurry;
        x.sharp = e.video && q.sharp >= 0.6;
        x.pan = q.pan;
        x.face = q.faces >= 0 ? q.faces > 0 : q.face == QLatin1String("likely"); // found faces over the skin hint
        if (!e.video) x.motion = -1; // no picture: neither calm nor busy
        out << x;
    }
    return out;
}

void clearMemoryCache()
{
    QMutexLocker lock(&g_mutex);
    memory().clear();
}

void setEventSource(std::function<QVector<AudioEvents::Event>(const QString&)> source)
{
    QMutexLocker lock(&g_mutex);
    eventSource() = std::move(source);
}

void setWaysForTests(const QString& current, const QString& answered)
{
    QMutexLocker lock(&g_mutex);
    g_testWay = current;
    g_testAnswered = answered;
}

} // namespace MediaIndex
