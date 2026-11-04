// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SfxSynth.h"

#include "core/SfxForge.h"
#include "core/SfxWhoosh.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace SfxSynth {

namespace {


struct KindInfo {
    Kind kind;
    const char* name;
    const char* description;
    double length; // default, seconds
    double peakDb;
};

// Peak levels: a whoosh or click sits under the game sound, a hit or impact may be heard a little more; all soft
// (the user found the first ones harsh)
constexpr KindInfo kInfo[] = {
    {Kind::Whoosh, "whoosh", "air rushing past: swells to its peak and falls away (peak on the cut)", 0.9, -12},
    {Kind::Riser, "riser", "a rising build (a gliding chord, an opening noise band, a speeding tremolo or a reverse swell) that peaks on the moment (a drop)", 0.9, -11},
    {Kind::Hit, "hit", "short knock on a block (a kill, a hit)", 0.2, -8},
    {Kind::Impact, "impact", "a heavy impact, a crash (the one big moment of a video)", 1.3, -6},
    {Kind::Click, "click", "a short click (a counter, a snapshot, a pop-up)", 0.15, -16},
    {Kind::Swish, "swish", "short, soft swipe of cloth or paper (a fast wipe, a title flying in)", 0.6, -14},
};

const KindInfo& info(Kind k)
{
    for (const KindInfo& i : kInfo)
        if (i.kind == k) return i;
    return kInfo[0];
}

// Whoosh and swish: the earshot pass engine (core/SfxWhoosh, MIT), three of its designed members per kind; the seed
// picks the member (1, 2, 3, then again with other noise) and the noise. A length other than the default stretches
// the motion (the sound ends where its tail has faded, so its length is about the asked one)
void makePass(Sound& s, unsigned seed, double asked, const SfxWhoosh::Member (&members)[3])
{
    const SfxWhoosh::Member m = members[(std::max(1u, seed) - 1) % 3];
    const double stretch = asked > 0 ? asked / SfxWhoosh::naturalLength(m) : 1.0;
    SfxWhoosh::Result r = SfxWhoosh::render(m, seed, stretch);
    s.samples = std::move(r.samples);
    s.anchor = r.anchor;
    s.length = double(s.samples.size()) / kRate;
}

void take(Sound& s, SfxForge::Result r)
{
    s.samples = std::move(r.samples);
    s.anchor = r.anchor;
    s.length = double(s.samples.size()) / kRate;
}

// A variant: the recipe member and its noise seed. Seeds 1-3 are the three picked takes (the ones earshot's realness
// judge rated best of eight); higher seeds cycle through the members with other noise
template <typename M>
struct Variant {
    M member;
    unsigned noise;
};

template <typename M>
Variant<M> variant(const Variant<M> (&list)[3], unsigned seed)
{
    const unsigned i = (std::max(1u, seed) - 1) % 3;
    return seed <= 3 ? list[i] : Variant<M>{list[i].member, seed * 7};
}

constexpr SfxWhoosh::Member kWhooshes[3] = {SfxWhoosh::Member::Windy, SfxWhoosh::Member::Long, SfxWhoosh::Member::Glide};
constexpr SfxWhoosh::Member kSwishes[3] = {SfxWhoosh::Member::Flick, SfxWhoosh::Member::Cloth, SfxWhoosh::Member::Paper};

} // namespace

QString kindName(Kind k) { return QString::fromLatin1(info(k).name); }

std::optional<Kind> kindFrom(const QString& name)
{
    for (const KindInfo& i : kInfo)
        if (name.compare(QLatin1String(i.name), Qt::CaseInsensitive) == 0) return i.kind;
    if (name.compare("boom", Qt::CaseInsensitive) == 0) return Kind::Impact;
    return std::nullopt;
}

QStringList kindNames()
{
    QStringList out;
    for (const KindInfo& i : kInfo) out << QString::fromLatin1(i.name);
    return out;
}

QString description(Kind k) { return QString::fromLatin1(info(k).description); }
double defaultLength(Kind k) { return info(k).length; }
double minLength(Kind k) { return k == Kind::Riser ? 0.3 : info(k).length * 0.5; }
double maxLength(Kind k) { return k == Kind::Riser ? 8.0 : info(k).length * 2.0; }
double peakLevelDb(Kind k) { return info(k).peakDb; }

Sound make(Kind kind, unsigned seed, double length)
{
    Sound s;
    s.kind = kind;
    s.seed = seed;
    const double asked = length > 0 ? std::clamp(length, minLength(kind), maxLength(kind)) : 0.0;
    s.length = asked > 0 ? asked : defaultLength(kind);
    s.samples.assign(size_t(std::lround(s.length * kRate)), 0.f);
    switch (kind) {
    case Kind::Whoosh: makePass(s, seed, asked, kWhooshes); break;
    case Kind::Swish: makePass(s, seed, asked, kSwishes); break;
    case Kind::Riser: { // earshot's designed risers (tension, swell) and its dark riser: the length is the build to the peak
        static constexpr Variant<SfxForge::Riser> kRisers[3] = {
            {SfxForge::Riser::Tension, 1}, {SfxForge::Riser::Swell, 1}, {SfxForge::Riser::Build, 2}};
        const auto [m, noise] = variant(kRisers, seed);
        take(s, SfxForge::riser(m, noise, asked));
        break;
    }
    case Kind::Hit: { // earshot's bright knock, three takes
        static constexpr Variant<SfxForge::Knock> kKnocks[3] = {
            {SfxForge::Knock::Snap, 4}, {SfxForge::Knock::Snap, 6}, {SfxForge::Knock::Snap, 5}};
        const auto [m, noise] = variant(kKnocks, seed);
        take(s, SfxForge::knock(m, noise));
        break;
    }
    case Kind::Impact: { // earshot's designed sub drop (a boom under a click), the soft one in three takes
        static constexpr Variant<SfxForge::Drop> kDrops[3] = {
            {SfxForge::Drop::Soft, 1}, {SfxForge::Drop::Soft, 6}, {SfxForge::Drop::Soft, 3}};
        const auto [m, noise] = variant(kDrops, seed);
        take(s, SfxForge::drop(m, noise));
        break;
    }
    case Kind::Click: { // earshot's camera shutter clicks
        static constexpr Variant<SfxForge::Click> kClicks[3] = {
            {SfxForge::Click::Mirror, 2}, {SfxForge::Click::Dslr, 3}, {SfxForge::Click::Snap, 8}};
        const auto [m, noise] = variant(kClicks, seed);
        take(s, SfxForge::click(m, noise));
        break;
    }
    }
    // Fade out the last 5 ms (no click at the end; a riser ends on the moment)
    const size_t n = s.samples.size();
    const size_t fade = std::min(n, size_t(kRate / 200));
    for (size_t i = 0; i < fade; ++i) s.samples[n - 1 - i] *= float(double(i) / double(fade));
    // Peak at the kind's level
    float peak = 0;
    for (float v : s.samples) peak = std::max(peak, std::abs(v));
    if (peak > 0) {
        const float gain = float(std::pow(10.0, peakLevelDb(kind) / 20.0) / peak);
        for (float& v : s.samples) v *= gain;
    }
    return s;
}

double anchorOf(Kind kind, unsigned seed, double length)
{
    if (length <= 0)
        if (const auto r = recordedFor(kind, seed)) return r->anchor;
    return make(kind, seed, length).anchor;
}

QString recordedRoot()
{
    const QString env = qEnvironmentVariable("SCHNEIDI_SFX_DATA");
    return env.isEmpty() ? QStringLiteral(":/sfx/cc0") : env;
}

namespace {

const QVector<Recorded>& recordedTable()
{
    // Read once per data folder (tests switch it)
    static QString root;
    static QVector<Recorded> table;
    if (root == recordedRoot() && !root.isEmpty()) return table;
    root = recordedRoot();
    table.clear();
    QFile f(root + "/sounds.json");
    if (!f.open(QIODevice::ReadOnly)) return table;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll())["sounds"].toArray()) {
        const std::optional<Kind> k = kindFrom(v["kind"].toString());
        const QString file = v["file"].toString();
        if (!k || file.isEmpty() || !QFileInfo::exists(root + "/" + file)) continue;
        table << Recorded{file, *k, v["anchor_s"].toDouble(), v["peak_db"].toDouble(), v["length_s"].toDouble(),
                          v["author"].toString(), v["licence"].toString(), v["licence_file"].toString()};
    }
    return table;
}

QString recordedCachePath(const Recorded& r)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/sfx/cc0/" + QFileInfo(r.file).fileName();
}

// The recording as a file of its own (FFmpeg reads files, not the program's resources); empty = could not
QString placeRecorded(const Recorded& r)
{
    const QString path = recordedCachePath(r);
    if (QFileInfo(path).size() > 0) return path;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    if (!QFile::copy(recordedRoot() + "/" + r.file, path)) return {};
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    return path;
}

} // namespace

QVector<Recorded> recorded(Kind k)
{
    QVector<Recorded> out;
    for (const Recorded& r : recordedTable())
        if (r.kind == k) out << r;
    return out;
}

std::optional<Recorded> recordedFor(Kind k, unsigned seed)
{
    const QVector<Recorded> list = recorded(k);
    if (list.isEmpty()) return std::nullopt;
    return list[qsizetype((std::max(1u, seed) - 1) % unsigned(list.size()))];
}

std::optional<Recorded> recordedOfFile(const QString& path)
{
    const QString name = QFileInfo(QString(path).replace('\\', '/')).fileName();
    for (const Recorded& r : recordedTable())
        if (QFileInfo(r.file).fileName() == name) return r;
    return std::nullopt;
}

double soundLength(Kind kind, unsigned seed, double length)
{
    if (length <= 0)
        if (const auto r = recordedFor(kind, seed)) return r->length;
    return make(kind, seed, length).length;
}

double levelGainDb(Kind kind, unsigned seed, double length)
{
    if (length <= 0)
        if (const auto r = recordedFor(kind, seed)) return peakLevelDb(kind) - r->peakDb;
    return 0.0;
}

double peakDb(const std::vector<float>& samples)
{
    float peak = 0;
    for (float v : samples) peak = std::max(peak, std::abs(v));
    return peak > 0 ? 20.0 * std::log10(double(peak)) : -std::numeric_limits<double>::infinity();
}

bool writeWav(const QString& path, const std::vector<float>& samples, int rate)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const auto u32 = [&](std::uint32_t v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    const auto u16 = [&](std::uint16_t v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    const std::uint32_t bytes = std::uint32_t(samples.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(std::uint32_t(rate));
    u32(std::uint32_t(rate) * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    QByteArray data(qsizetype(bytes), Qt::Uninitialized);
    // Triangular dither of +-2 steps (about -87 dBFS; the same every time: hashed from the sample index), so the slow
    // crest of a sub drop does not turn into runs of equal samples (clip detectors read those as flat tops)
    const auto hash = [](std::uint64_t z) {
        z = (z + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return double((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);
    };
    for (size_t i = 0; i < samples.size(); ++i) {
        const double dither = 2.0 * (hash(2 * i) - hash(2 * i + 1));
        const long v = std::clamp(std::lround(double(samples[i]) * 32767.0 + dither), -32767L, 32767L);
        qToLittleEndian(std::int16_t(v), data.data() + i * 2);
    }
    f.write(data);
    return f.commit();
}

QString cachedFile(Kind kind, unsigned seed, double length)
{
    if (length <= 0)
        if (const auto r = recordedFor(kind, seed)) return placeRecorded(*r);
    // The name holds the real length (a whoosh ends where its tail has faded) and the asked one when there was one:
    // kind-seed-<ms>ms[-a<asked ms>]-v<version>.wav. Making the sound to know its length is cheap (milliseconds)
    const Sound s = make(kind, seed, length);
    const QString asked = length > 0 ? QString("-a%1").arg(std::lround(std::clamp(length, minLength(kind), maxLength(kind)) * 1000)) : QString();
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                         + QString("/sfx/%1-%2-%3ms%4-v%5.wav")
                               .arg(kindName(kind))
                               .arg(seed)
                               .arg(std::lround(s.length * 1000))
                               .arg(asked)
                               .arg(kVersion);
    if (QFileInfo(path).size() > 44) return path;
    return writeWav(path, s.samples) ? path : QString();
}

std::optional<CachedName> parseCachedFile(const QString& path)
{
    static const QRegularExpression re(R"(^([a-z]+)-(\d+)-(\d+)ms(?:-a(\d+))?-v(\d+)$)");
    const QRegularExpressionMatch m = re.match(QFileInfo(path).completeBaseName());
    if (!m.hasMatch()) return std::nullopt;
    const std::optional<Kind> kind = kindFrom(m.captured(1));
    if (!kind) return std::nullopt;
    return CachedName{*kind, m.captured(2).toUInt(), m.captured(3).toDouble() / 1000.0,
                      m.captured(4).isEmpty() ? 0.0 : m.captured(4).toDouble() / 1000.0, m.captured(5).toInt()};
}

QString restoreCachedFile(const QString& missingPath)
{
    if (const auto r = recordedOfFile(missingPath)) return placeRecorded(*r);
    const QString name = QFileInfo(QString(missingPath).replace('\\', '/')).fileName();
    const std::optional<CachedName> n = parseCachedFile(name);
    if (!n || n->version != kVersion) return {};
    const QString path = cachedFile(n->kind, n->seed, n->asked);
    return QFileInfo(path).fileName() == name ? path : QString();
}

} // namespace SfxSynth
