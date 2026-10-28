// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ExportCheck.h"

#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/Retime.h"

#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QRectF>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace ExportCheck {

namespace {

QString num(double v, int decimals = 1) { return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint()); }

Issue issue(Severity s, const char* check, const QString& message, int from = -1, int to = -1, int clip = 0)
{
    return {s, QString::fromLatin1(check), message, from, to, clip};
}

double area(const QRectF& r) { return r.isEmpty() ? 0.0 : r.width() * r.height(); }

QFontMetricsF metrics(const TitleStyle& s, double size)
{
    QFont font(s.fontFamily());
    font.setPixelSize(std::max(1, int(std::lround(size))));
    font.setBold(s.bold);
    font.setItalic(s.italic);
    return QFontMetricsF(font);
}

// Box and outline grow the visible block
QRectF grown(QRectF r, const TitleStyle& s)
{
    const double g = (s.boxOn ? std::max(0.0, s.boxPad) : 0.0) + (s.outlineOn ? std::max(0.0, s.outlineWidth) : 0.0);
    return r.adjusted(-g, -g, g, g);
}

// Text block of a title at clip frame t in project pixels, as the timeline builder draws it (titleCut: lines as wide
// as the widest, centred on the title position; then the clip's zoom around the picture centre and its position;
// rotation left out)
QRectF titleBlock(const Clip& c, double t, double W, double H)
{
    const TitleStyle& s = c.title;
    const QFontMetricsF fm = metrics(s, std::max(1.0, Keys::valueAt(c, AnimParam::TitleSize, t)));
    const QStringList lines = s.text.split('\n');
    double w = 1;
    for (const QString& line : lines) w = std::max(w, fm.horizontalAdvance(line));
    const double h = fm.lineSpacing() * lines.size();
    const double cx = W / 2 + Keys::valueAt(c, AnimParam::TitlePosX, t);
    const double cy = H / 2 - Keys::valueAt(c, AnimParam::TitlePosY, t);
    QRectF r = grown(QRectF(cx - w / 2, cy - h / 2, w, h), s);
    if (Keys::hasTransform(c)) {
        const double zx = Keys::valueAt(c, AnimParam::ZoomX, t), zy = Keys::valueAt(c, AnimParam::ZoomY, t);
        const double x = Keys::valueAt(c, AnimParam::PosX, t), y = Keys::valueAt(c, AnimParam::PosY, t);
        const auto map = [&](QPointF p) { return QPointF((W - W * zx) / 2 + x + p.x() * zx, (H - H * zy) / 2 - y + p.y() * zy); };
        r = QRectF(map(r.topLeft()), map(r.bottomRight())).normalized();
    }
    return r;
}

// Subtitle lines wrapped at 90 % of the picture width like the timeline builder (wrapSubtitle), block bottom posY
// above the picture's bottom edge
QRectF subtitleBlock(const TitleStyle& s, const QString& text, double W, double H)
{
    const QFontMetricsF fm = metrics(s, std::max(1.0, s.size));
    int lines = 0;
    double w = 1;
    for (const QString& para : text.trimmed().split('\n')) {
        QString line;
        const auto flush = [&] {
            w = std::max(w, fm.horizontalAdvance(line));
            ++lines;
        };
        for (const QString& word : para.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate) > W * 0.9) {
                flush();
                line = word;
            } else {
                line = candidate;
            }
        }
        flush();
    }
    const double h = fm.lineSpacing() * lines, bottom = std::clamp(H - s.posY, 1.0, H);
    return grown(QRectF(W / 2 + s.posX - w / 2, bottom - h, w, h), s);
}

enum class Zone { None, Bottom, Right, Top, Outside };

// The covered part of a text block and the zone that covers most of it
QPair<double, Zone> covered(const QRectF& r, double W, double H, const Options& o)
{
    const double total = area(r);
    if (total <= 0) return {0.0, Zone::None};
    const QRectF frame(0, 0, W, H);
    const double bottomY = H * (1 - o.safeBottom);
    const QVector<QPair<Zone, double>> parts{
        {Zone::Outside, total - area(r.intersected(frame))},
        {Zone::Bottom, area(r.intersected(QRectF(0, bottomY, W, H - bottomY)))},
        {Zone::Top, area(r.intersected(QRectF(0, 0, W, H * o.safeTop)))},
        {Zone::Right, area(r.intersected(QRectF(W * (1 - o.safeRight), H * o.safeRightFrom, W * o.safeRight,
                                                std::max(0.0, bottomY - H * o.safeRightFrom))))}};
    double sum = 0, best = 0;
    Zone zone = Zone::None;
    for (const auto& [z, a] : parts) {
        sum += a;
        if (a > best) {
            best = a;
            zone = z;
        }
    }
    return {std::min(1.0, sum / total), zone};
}

QString zoneMessage(Zone z, const QString& what, bool several = false)
{
    switch (z) {
    case Zone::Bottom:
        return (several ? T("%1 liegen unten, wo TikTok, Reels und Shorts Beschreibung und Knöpfe zeigen – höher setzen "
                            "(etwa 25–35 % über dem unteren Rand)")
                        : T("%1 liegt unten, wo TikTok, Reels und Shorts Beschreibung und Knöpfe zeigen – höher setzen "
                            "(etwa 25–35 % über dem unteren Rand)"))
            .arg(what);
    case Zone::Right:
        return (several ? T("%1 reichen rechts unter die Knöpfe von TikTok, Reels und Shorts – schmaler machen oder "
                            "weiter nach links setzen")
                        : T("%1 reicht rechts unter die Knöpfe von TikTok, Reels und Shorts – schmaler machen oder "
                            "weiter nach links setzen"))
            .arg(what);
    case Zone::Top:
        return (several ? T("%1 liegen oben unter der Leiste von TikTok, Reels und Shorts – tiefer setzen")
                        : T("%1 liegt oben unter der Leiste von TikTok, Reels und Shorts – tiefer setzen"))
            .arg(what);
    case Zone::Outside:
    case Zone::None: break;
    }
    return (several ? T("%1 ragen aus dem Bild") : T("%1 ragt aus dem Bild")).arg(what);
}

// "A1" or "A1 (Spielton)"
QString audioTrackLabel(const Timeline& tl, int i)
{
    const QString shortName = trackShortName(TrackRef{TrackKind::Audio, i});
    const QString name = tl.audio[i].name.trimmed();
    return name.isEmpty() || name.compare(shortName, Qt::CaseInsensitive) == 0 ? shortName
                                                                                : QString("%1 (%2)").arg(shortName, name);
}

bool covers(const Track& t, int frame)
{
    return std::any_of(t.clips.begin(), t.clips.end(),
                       [frame](const Clip& c) { return c.enabled && c.start <= frame && frame < c.end(); });
}

bool nameHas(const QString& name, std::initializer_list<const char*> words)
{
    const QString n = name.toLower();
    return std::any_of(words.begin(), words.end(), [&n](const char* w) { return n.contains(QString::fromUtf8(w)); });
}

// Merged [from, to) ranges covered by visible picture inside [lo, hi)
QVector<QPair<int, int>> pictureCoverage(const Timeline& tl, int lo, int hi)
{
    QVector<QPair<int, int>> ranges;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips)
            if (c.enabled && c.end() > lo && c.start < hi) ranges.append({std::max(lo, c.start), std::min(hi, c.end())});
    }
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    for (const auto& r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = std::max(merged.last().second, r.second);
        else merged.append(r);
    }
    return merged;
}

} // namespace

const char* severityId(Severity s)
{
    switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
    }
    return "info";
}

QVector<Issue> timeline(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o)
{
    QVector<Issue> out;
    if (to <= from) return out;
    const double fps = f.rate.fps();

    // Media that are not where the project says (moved, deleted, other computer): renders as a placeholder
    QSet<QString> seen;
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) {
                if (!c.enabled || c.kind != ClipKind::Media || c.mediaPath.isEmpty() || c.end() <= from || c.start >= to)
                    continue;
                if (seen.contains(c.mediaPath) || QFileInfo::exists(c.mediaPath)) continue;
                seen.insert(c.mediaPath);
                out << issue(Severity::Error, "offline", T("Datei fehlt: %1").arg(c.mediaPath), std::max(from, c.start),
                             std::min(to, c.end()));
            }

    // Frames without any picture: black in the video (a forgotten frame between two clips is the classic)
    const QVector<QPair<int, int>> covered = pictureCoverage(tl, from, to);
    if (covered.isEmpty()) {
        out << issue(Severity::Warning, "black", T("Die Timeline hat kein Bild (nur Ton)"), from, to);
    } else {
        int at = from;
        const auto gap = [&](int a, int b) {
            const int n = b - a;
            out << issue(Severity::Warning, "black",
                         n == 1 ? T("Lücke: 1 Bild schwarz") : T("Lücke: %1 Bilder schwarz (%2 s)").arg(n).arg(num(n / fps)),
                         a, b);
        };
        for (const auto& [a, b] : covered) {
            if (a > at) gap(at, a);
            at = b;
        }
        if (at < to) gap(at, to);
    }

    // Subtitles: readable long enough, not faster than people read, not cut off by the end
    for (const SubtitleTrack& st : tl.subtitles) {
        if (!st.enabled) continue;
        for (const SubtitleCue& cue : st.cues) {
            if (cue.end <= from || cue.start >= to) continue;
            const double seconds = cue.length() / fps;
            if (cue.end > to)
                out << issue(Severity::Warning, "subtitle_end", T("Untertitel läuft über das Ende hinaus: „%1“").arg(cue.text.simplified()),
                             cue.start, cue.end);
            if (seconds < o.subtitleMinS)
                out << issue(Severity::Warning, "subtitle_short",
                             T("Untertitel zu kurz zum Lesen (%1 s): „%2“").arg(num(seconds), cue.text.simplified()),
                             cue.start, cue.end);
            QString letters = cue.text;
            letters.remove('\n');
            const double cps = seconds > 0 ? letters.trimmed().size() / seconds : 0;
            if (seconds >= o.subtitleMinS && cps > o.subtitleMaxCps)
                out << issue(Severity::Warning, "subtitle_fast",
                             T("Untertitel zu schnell (%1 Zeichen/s, höchstens %2): „%3“")
                                 .arg(num(cps, 0), num(o.subtitleMaxCps, 0), cue.text.simplified()),
                             cue.start, cue.end);
        }
    }
    return out;
}

QVector<QPair<int, int>> silentStretches(const std::vector<float>& framePeaks, int offset, double thresholdDb,
                                         int minFrames)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20));
    QVector<QPair<int, int>> out;
    const int n = int(framePeaks.size());
    for (int i = 0; i < n;) {
        if (framePeaks[size_t(i)] >= limit) {
            ++i;
            continue;
        }
        int j = i;
        while (j < n && framePeaks[size_t(j)] < limit) ++j;
        if (j - i >= minFrames) out.append({offset + i, offset + j});
        i = j;
    }
    return out;
}

SoundFix soundFix(const QVector<Issue>& sound, double integratedLufs, const Timeline& tl, const Options& o)
{
    const auto has = [&sound](const char* check) {
        return std::any_of(sound.begin(), sound.end(), [check](const Issue& i) { return i.check == check; });
    };
    SoundFix fix;
    fix.ceiling = std::min(tl.masterLimiterDb, o.maxTruePeakDb);
    const bool limiterOn = tl.masterLimiter && tl.masterLimiterDb <= o.maxTruePeakDb;
    // Loudness: the master fader by the difference to the target (as far as it goes); up = the limiter catches the
    // peaks that grow with it
    double volume = tl.masterVolumeDb;
    if (has("loudness"))
        volume = std::clamp(tl.masterVolumeDb + (o.targetLufs - integratedLufs), kMinVolumeDb, kMaxVolumeDb);
    const bool louder = volume > tl.masterVolumeDb + 0.05, quieter = volume < tl.masterVolumeDb - 0.05;
    fix.limiter = !limiterOn && (has("clipping") || has("true_peak") || louder);
    fix.level = louder || quieter;
    fix.volumeDb = std::round(volume * 10) / 10;
    fix.change = fix.volumeDb - tl.masterVolumeDb;
    return fix;
}

void apply(const SoundFix& fix, Timeline& tl)
{
    if (fix.limiter) {
        tl.masterLimiter = true;
        tl.masterLimiterDb = fix.ceiling;
    }
    if (fix.level) tl.masterVolumeDb = fix.volumeDb;
}

QVector<Issue> sound(const std::vector<float>& framePeaks, int offset, double integratedLufs, double truePeakDb,
                     const ProjectFormat& f, const Options& o, bool limiter)
{
    QVector<Issue> out;
    const int n = int(framePeaks.size());
    const double fps = f.rate.fps();
    const QVector<QPair<int, int>> silent = silentStretches(framePeaks, offset, o.silenceDb, 1);
    if (n == 0 || integratedLufs <= LoudnessMeter::kSilence
        || (silent.size() == 1 && silent[0].first == offset && silent[0].second == offset + n)) {
        out << issue(Severity::Warning, "no_sound", T("Das Video hat keinen Ton"));
        return out;
    }

    if (truePeakDb > 0)
        out << issue(Severity::Error, "clipping",
                     (limiter ? T("Übersteuert: True Peak %1 dBTP (über 0 dB verzerrt)")
                              : T("Übersteuert: True Peak %1 dBTP (über 0 dB verzerrt) – Limiter im Mixer einschalten "
                                  "oder leiser mischen"))
                         .arg(num(truePeakDb)));
    else if (truePeakDb > o.maxTruePeakDb)
        out << issue(Severity::Warning, "true_peak",
                     (limiter ? T("True Peak %1 dBTP liegt über %2 dBTP (Codecs können übersteuern)")
                              : T("True Peak %1 dBTP liegt über %2 dBTP (Codecs können übersteuern) – Limiter im "
                                  "Mixer einschalten"))
                         .arg(num(truePeakDb), num(o.maxTruePeakDb)));
    if (std::abs(integratedLufs - o.targetLufs) > o.toleranceLu)
        out << issue(Severity::Warning, "loudness",
                     T("Lautheit %1 LUFS, Ziel %2 LUFS (%3)")
                         .arg(num(integratedLufs), num(o.targetLufs),
                              integratedLufs < o.targetLufs ? T("zu leise") : T("zu laut")));

    // Silence: at the end it looks like a forgotten tail, at the start and inside it may be intended
    const int endMin = int(std::lround(o.silenceEndS * fps)), middleMin = int(std::lround(o.silenceMiddleS * fps));
    for (const auto& [a, b] : silent) {
        const double seconds = (b - a) / fps;
        if (b == offset + n && b - a > endMin)
            out << issue(Severity::Warning, "silence_end", T("%1 s Stille am Ende").arg(num(seconds)), a, b);
        else if (a == offset && b - a > endMin)
            out << issue(Severity::Info, "silence_start", T("%1 s Stille am Anfang").arg(num(seconds)), a, b);
        else if (a != offset && b != offset + n && b - a > middleMin)
            out << issue(Severity::Info, "silence", T("%1 s Stille").arg(num(seconds)), a, b);
    }
    return out;
}

bool isVertical(const ProjectFormat& f) { return f.width > 0 && f.height > 0 && double(f.width) / f.height <= 0.63; }

QVector<Issue> layout(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o)
{
    QVector<Issue> out;
    if (!isVertical(f) || to <= from) return out;
    const double W = f.width, H = f.height;
    // Titles of one track in the same zone (a counter x1 ... x16, captions) are one finding, not one per title
    struct Found {
        int track;
        Zone zone;
        int from, to, clip;
        QString text;
    };
    QVector<Found> found;
    for (int ti = 0; ti < tl.video.size(); ++ti) {
        const Track& t = tl.video[ti];
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (!c.isTitle() || !c.enabled || c.title.text.trimmed().isEmpty() || c.end() <= from || c.start >= to)
                continue;
            if (Keys::hasOpacity(c) && !Keys::animated(c, AnimParam::Opacity) && c.transform.opacity <= 0) continue;
            // Animated titles move: look at up to 24 moments of the visible part. A title sliding in from outside
            // for a moment is intended: it counts from half a second on (or the whole title, when shorter).
            const int a = std::max(from, c.start), b = std::min(to, c.end());
            const int step = std::max(1, (b - a) / 24);
            int first = -1, last = -1, bad = 0;
            QVector<int> zones(5, 0);
            for (int fr = a; fr < b; fr += step) {
                const auto [part, zone] = covered(titleBlock(c, fr - c.start, W, H), W, H, o);
                if (part < o.safeMaxCovered) continue;
                if (first < 0) first = fr;
                last = fr;
                bad += std::min(step, b - fr);
                ++zones[int(zone)];
            }
            if (first < 0 || bad < std::min(b - a, int(std::lround(0.5 * f.rate.fps())))) continue;
            const Zone zone = Zone(std::max_element(zones.begin(), zones.end()) - zones.begin());
            found.append({ti, zone, first, std::min(b, last + step), c.id, c.title.text.simplified()});
        }
    }
    for (int i = 0; i < found.size(); ++i) {
        const Found& f0 = found[i];
        if (f0.clip == 0) continue; // taken into an earlier group
        QVector<Found> group{f0};
        for (int j = i + 1; j < found.size(); ++j)
            if (found[j].clip && found[j].track == f0.track && found[j].zone == f0.zone) {
                group.append(found[j]);
                found[j].clip = 0;
            }
        std::sort(group.begin(), group.end(), [](const Found& a, const Found& b) { return a.from < b.from; });
        if (group.size() == 1) {
            out << issue(Severity::Warning, "safe_area", zoneMessage(f0.zone, T("Titel „%1“").arg(f0.text)), f0.from,
                         f0.to, f0.clip);
            continue;
        }
        Issue is = issue(Severity::Warning, "safe_area",
                         zoneMessage(f0.zone,
                                     T("%1 Titel („%2“ … „%3“)")
                                         .arg(group.size())
                                         .arg(group.first().text, group.last().text),
                                     true),
                         group.first().from, group.last().to, group.first().clip);
        for (const Found& g : group) is.clips << g.clip;
        out << is;
    }
    // Subtitles: the track style places every cue; one issue per track from its first covered cue on
    for (int i = 0; i < tl.subtitles.size(); ++i) {
        const SubtitleTrack& st = tl.subtitles[i];
        if (!st.enabled) continue;
        int first = -1, last = -1;
        QVector<int> zones(5, 0);
        for (const SubtitleCue& cue : st.cues) {
            if (cue.end <= from || cue.start >= to || cue.text.trimmed().isEmpty()) continue;
            const auto [part, zone] = covered(subtitleBlock(st.style, cue.text, W, H), W, H, o);
            if (part < o.safeMaxCovered) continue;
            if (first < 0) first = std::max(from, cue.start);
            last = std::min(to, cue.end);
            ++zones[int(zone)];
        }
        if (first < 0) continue;
        const Zone zone = Zone(std::max_element(zones.begin(), zones.end()) - zones.begin());
        out << issue(Severity::Warning, "subtitle_safe_area", zoneMessage(zone, subtitleTrackDisplayName(st, i)), first,
                     last);
    }
    return out;
}

bool wantsHook(int from, int to, const ProjectFormat& f, const Options& o)
{
    const double fps = f.rate.fps();
    return isVertical(f) && from == 0 && to > 0 && to / fps <= o.hookMaxS && to / fps >= o.hookS / 2;
}

QVector<int> hookFrames(int from, int to, const ProjectFormat& f, const Options& o)
{
    QVector<int> out;
    const double fps = f.rate.fps();
    for (int k = 0; k * 0.1 <= o.hookS + 1e-9; ++k) {
        const int fr = from + int(std::lround(k * 0.1 * fps));
        if (fr >= to) break;
        if (out.isEmpty() || fr != out.last()) out << fr;
    }
    return out;
}

QVector<Issue> hook(double motion, int from, int to, const ProjectFormat& f, const Options& o)
{
    if (motion >= o.hookMinMotion) return {};
    const int end = std::min(to, from + int(std::lround(o.hookS * f.rate.fps())));
    return {issue(Severity::Info, "slow_hook",
                  T("Langsamer Einstieg: in der ersten Sekunde bewegt sich kaum etwas (Bewegung %1) – mit dem "
                    "stärksten Moment beginnen")
                      .arg(num(motion)),
                  from, end)};
}

SoundRole soundRole(const Timeline& tl, int track, const std::function<bool(const QString& path)>& hasPicture)
{
    const Track& t = tl.audio[track];
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& x) { return x.solo; });
    if (t.muted || (anySolo && !t.solo)) return SoundRole::Other;
    if (nameHas(t.name, {"music", "musik", "musique", "música", "musica", "muzyk", "музык", "song", "bgm", "soundtrack"}))
        return SoundRole::Music;
    if (nameHas(t.name, {"voice", "voz", "voix", "stimme", "sprache", "speech", "głos", "голос", "mic", "mikro",
                         "narrat", "kommentar", "comment", "dialog", "game", "spiel", "jeu", "juego", "gra", "игр", "sfx"}))
        return SoundRole::Events;
    bool any = false, picture = false;
    for (const Clip& c : t.clips) {
        if (!c.enabled) continue;
        any = true;
        if (c.isNested() || hasPicture(c.mediaPath)) picture = true;
    }
    if (!any) return SoundRole::Other;
    return picture ? SoundRole::Events : SoundRole::Undecided;
}

bool continuous(const Timeline& tl, int track, const std::vector<float>& framePeaks, int offset)
{
    std::vector<float> v;
    const int n = int(framePeaks.size());
    for (const Clip& c : tl.audio[track].clips) {
        if (!c.enabled) continue;
        for (int fr = std::max(offset, c.start); fr < std::min(offset + n, c.end()); ++fr) v.push_back(framePeaks[size_t(fr - offset)]);
    }
    if (v.size() < 10) return false;
    std::vector<float> sorted = v;
    const size_t p95 = sorted.size() * 95 / 100;
    std::nth_element(sorted.begin(), sorted.begin() + p95, sorted.end());
    const float loud = sorted[p95];
    if (loud <= 1e-5f) return false; // silent
    const float limit = loud * 0.0316f; // -30 dB
    const auto quiet = std::count_if(v.begin(), v.end(), [limit](float p) { return p < limit; });
    return size_t(quiet) <= v.size() / 10;
}

QVector<Issue> mix(const Timeline& tl, const QVector<TrackSound>& sounds, int from, const ProjectFormat& f,
                   const Options& o)
{
    QVector<Issue> out;
    const double fps = f.rate.fps();
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    size_t n = 0;
    QStringList musicNames;
    QVector<int> musicTracks;
    for (const TrackSound& s : sounds) {
        n = std::max(n, s.blocks.size());
        if (s.music) {
            musicNames << audioTrackLabel(tl, s.track);
            musicTracks << s.track;
        }
    }
    if (musicTracks.isEmpty()) return out;
    std::vector<double> music(n, 0.0);
    for (const TrackSound& s : sounds)
        if (s.music)
            for (size_t k = 0; k < s.blocks.size(); ++k) music[k] += s.blocks[k];
    // Block k covers [k, k + 4) tenths of a second from `from`; its middle decides which clips play in it
    const auto middle = [&](size_t k) { return from + int(std::lround((k * 0.1 + 0.2) * fps)); };
    const auto musicPlays = [&](size_t k) {
        return std::any_of(musicTracks.begin(), musicTracks.end(), [&](int t) { return covers(tl.audio[t], middle(k)); });
    };
    const auto db = [](double power) { return 10 * std::log10(std::max(power, 1e-30)); };
    for (const TrackSound& s : sounds) {
        if (s.music) continue;
        const std::vector<double>& e = s.blocks;
        std::vector<size_t> both; // both play here (the track's clip, even when quiet) and the music sounds
        for (size_t k = 0; k < e.size(); ++k)
            if (music[k] > absGate && covers(tl.audio[s.track], middle(k)) && musicPlays(k)) both.push_back(k);
        // Stretches of playing together (pauses up to 2 s bridged), each judged on its own
        for (size_t i = 0; i < both.size();) {
            size_t j = i;
            while (j + 1 < both.size() && both[j + 1] - both[j] <= 20) ++j;
            std::vector<double> levels;
            for (size_t x = i; x <= j; ++x) levels.push_back(db(e[both[x]]));
            std::sort(levels.begin(), levels.end());
            // Its events: the loud moments (shots, words), the blocks within 10 LU of its 95th percentile - not
            // the mean, which sparse shots pull down to the quiet steps between them. Flat noise has none, a
            // silent track neither (its loud moments must pass the absolute gate of -70 LUFS).
            const double p95 = levels.empty() ? 0 : levels[levels.size() * 95 / 100];
            const bool events = levels.size() >= 10 && p95 > db(absGate) && p95 - levels[levels.size() / 2] >= 6;
            double ev = 0, mu = 0;
            int count = 0;
            for (size_t x = i; x <= j; ++x)
                if (db(e[both[x]]) >= p95 - 10) {
                    ev += e[both[x]];
                    mu += music[both[x]];
                    ++count;
                }
            const double under = count > 0 ? db(mu) - db(ev) : 0; // sums of the same blocks: the ratio of the means
            if (events && count >= 2 && under > o.underMusicLu) {
                const QString label = audioTrackLabel(tl, s.track);
                out << issue(Severity::Warning, "under_music",
                             T("%1 geht unter der Musik unter: %2 LU leiser als %3 – %1 lauter stellen oder die Musik "
                               "absenken (Ducking)")
                                 .arg(label, num(under), musicNames.join(", ")),
                             from + int(std::lround(both[i] * 0.1 * fps)),
                             from + int(std::lround((both[j] * 0.1 + 0.4) * fps)));
            }
            i = j + 1;
        }
    }
    return out;
}

QVector<Issue> stutter(const Timeline& tl, int from, int to, const ProjectFormat& f,
                       const std::function<std::optional<Stutter::Scan>(const QString& path)>& scanOf,
                       const std::function<int(const QString& path)>& fileLength)
{
    QVector<Issue> out;
    const double fps = f.rate.fps();
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || c.kind != ClipKind::Media || c.freeze || c.mediaPath.isEmpty() || c.end() <= from
                || c.start >= to)
                continue;
            const std::optional<Stutter::Scan> scan = scanOf(c.mediaPath);
            if (!scan || scan->jumps.isEmpty()) continue;
            const RetimeMap map(c, fileLength(c.mediaPath));
            const int a = std::max(from, c.start), b = std::min(to, c.end());
            // Source seconds the visible part shows (reversed or ramped clips run through it differently)
            const auto sourceS = [&](int frame) { return map.fileFrameAt(c.in + (frame - c.start)) / fps; };
            const auto timelineAt = [&](double s) { return c.start + (map.materialAtFile(s * fps) - c.in); };
            const double s0 = sourceS(a), s1 = sourceS(b);
            for (const Stutter::Stretch& st : Stutter::stretches(Stutter::inRange(*scan, std::min(s0, s1), std::max(s0, s1)))) {
                // The picture stands still from the frame after the last good one to the next frame
                double t0 = timelineAt(st.from + scan->frameDuration), t1 = timelineAt(st.to);
                if (t0 > t1) std::swap(t0, t1);
                const int x0 = std::clamp(int(std::floor(t0)), a, b - 1), x1 = std::clamp(int(std::ceil(t1)), x0 + 1, b);
                const QString file = QFileInfo(c.mediaPath).fileName();
                const QString what = st.missing == 1 ? T("Ruckler in der Aufnahme: 1 Bild fehlt – %1").arg(file)
                                                     : T("Ruckler in der Aufnahme: %1 Bilder fehlen (%2 s) – %3")
                                                           .arg(st.missing)
                                                           .arg(num(st.missing * scan->frameDuration, 2), file);
                out << issue(Severity::Warning, "stutter", what, x0, x1, c.id);
            }
        }
    }
    return out;
}

int errors(const QVector<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Error; }));
}

int warnings(const QVector<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Warning; }));
}

} // namespace ExportCheck
