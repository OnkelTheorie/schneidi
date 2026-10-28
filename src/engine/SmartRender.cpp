// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/SmartRender.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "engine/video/Reader.h"

#include <QHash>

#include <algorithm>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

// Shows the file as it is: no video effect (grade, stabilizer … count too), no transform/crop/opacity, normal speed
bool plain(const Clip& c)
{
    if (c.kind != ClipKind::Media || c.mediaPath.isEmpty() || c.isRetimed()) return false;
    if (Keys::hasTransform(c) || Keys::hasCrop(c) || Keys::hasOpacity(c)) return false;
    for (const EffectInstance& e : c.effects) {
        if (!e.enabled) continue;
        const EffectDescriptor* d = EffectRegistry::find(e.effectId);
        if (!d || d->video) return false;
    }
    return true;
}

} // namespace

namespace SmartRender {

int colorspace(const QVector<Span>& spans, const QString& videoCodec, QSize size)
{
    const AVCodec* encoder = avcodec_find_encoder_by_name(videoCodec.toUtf8().constData());
    if (!encoder) return 0;
    QHash<QString, int> frames;
    for (const Span& s : spans) frames[s.file] += s.to - s.from;
    QHash<int, int> votes;
    for (auto it = frames.cbegin(); it != frames.cend(); ++it) {
        Video::Reader::Options o;
        o.threads = 1;
        o.hardware = false;
        auto reader = Video::Reader::open(it.key(), o);
        if (!reader) continue;
        const Video::Info& info = reader->info();
        const AVCodecParameters* par = reader->container()->streams[info.index]->codecpar;
        if (par->codec_id != encoder->id || QSize(info.width, info.height) != size) continue;
        if (info.colorspace == 601 || info.colorspace == 709) votes[info.colorspace] += it.value();
    }
    int best = 0;
    for (auto it = votes.cbegin(); it != votes.cend(); ++it)
        if (!best || it.value() > votes.value(best)) best = it.key();
    return best;
}

bool enabled()
{
    const QString v = qEnvironmentVariable("SCHNEIDI_SMART_RENDER").trimmed().toLower();
    return v != QLatin1String("off") && v != QLatin1String("0");
}

QVector<Span> spans(const Timeline& tl, int first, int length, bool burnSubtitles)
{
    const int last = first + length; // exclusive
    // Frames something blends over: transitions and fades on any video track (generously: a dissolve centered on the
    // cut or a fade, both edges), subtitles burnt in
    std::vector<std::pair<int, int>> mixed;
    std::vector<int> cuts{first, last};
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled) continue;
            cuts.push_back(c.start);
            cuts.push_back(c.end());
            if (c.transIn > 0) mixed.emplace_back(c.start - c.transIn, c.start + c.transIn);
            if (c.transOut > 0) mixed.emplace_back(c.end() - c.transOut, c.end() + c.transOut);
            if (c.fadeIn > 0) mixed.emplace_back(c.start, c.start + c.fadeIn);
            if (c.fadeOut > 0) mixed.emplace_back(c.end() - c.fadeOut, c.end());
        }
    }
    if (burnSubtitles)
        for (const SubtitleTrack& st : tl.subtitles)
            if (st.enabled)
                for (const SubtitleCue& cue : st.cues) mixed.emplace_back(cue.start, cue.end);
    for (const auto& [a, b] : mixed) {
        cuts.push_back(a);
        cuts.push_back(b);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    QVector<Span> out;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const int a = std::max(cuts[i], first), b = std::min(cuts[i + 1], last);
        if (a >= b) continue;
        // Within [a, b) nothing changes: the same top clip, mixed or not
        const bool blended = std::any_of(mixed.cbegin(), mixed.cend(), [&](const auto& m) { return m.first < b && m.second > a; });
        if (blended) continue;
        const Clip* top = nullptr;
        for (int ti = int(tl.video.size()) - 1; ti >= 0 && !top; --ti) {
            const Track& t = tl.video[ti];
            if (t.hidden) continue;
            for (const Clip& c : t.clips)
                if (c.enabled && c.start <= a && c.end() > a) {
                    top = &c;
                    break;
                }
        }
        if (!top || !plain(*top)) continue; // nothing (black) or something rendered
        const int64_t source = int64_t(top->in) + (a - top->start);
        if (!out.isEmpty() && out.last().to == a - first && out.last().file == top->mediaPath
            && out.last().sourceFrame + (out.last().to - out.last().from) == source) {
            out.last().to = b - first;
            continue;
        }
        out.append(Span{a - first, b - first, top->mediaPath, source});
    }
    return out;
}

} // namespace SmartRender
