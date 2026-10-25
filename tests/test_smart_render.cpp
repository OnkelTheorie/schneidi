// Smart render (engine/SmartRender + ExportWriter): which output frames may be copied (timeline rules), and real
// exports: copied frames are bit for bit the source's, encoded ones look the same as a normal export, the cuts land
// on the right frames, the sound is the normal export's. Sources: H.264 with B-frames (decode times reordered),
// H.264 unmarked (BT.601: the export switches to 601), HEVC, open GOP (no IDR after the first: nothing copied).
// Also: an effect clip is encoded, In/Out, MKV, cancelling.

#include "check.h"

#include "engine/Exporter.h"
#include "engine/SmartRender.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

bool avcodecHas(const char* encoder)
{
    return avcodec_find_encoder_by_name(encoder) != nullptr;
}

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrameBytes = 640 * 360 * 3 / 2;

Clip clipOf(int id, const QString& media, int start, int in, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = media;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

void testSpans()
{
    using SmartRender::Span;
    Timeline tl;
    tl.video.resize(2);
    tl.video[0].clips << clipOf(1, "a", 0, 10, 50) << clipOf(2, "b", 50, 0, 50) << clipOf(3, "a", 100, 60, 50);
    // Adjacent pieces of one file in order merge; another file starts a span
    CHECK(SmartRender::spans(tl, 0, 150, false) == (QVector<Span>{{0, 50, "a", 10}, {50, 100, "b", 0}, {100, 150, "a", 60}}));
    // Export range: counted from its first frame
    CHECK(SmartRender::spans(tl, 40, 20, false) == (QVector<Span>{{0, 10, "a", 50}, {10, 20, "b", 0}}));
    // Effect, transform, speed: rendered
    Timeline fx = tl;
    fx.video[0].clips[1].effects << EffectInstance{"blur", {}, true};
    fx.video[0].clips[2].transform.zoomX = 1.5;
    fx.video[0].clips[0].speed = 2.0;
    CHECK(SmartRender::spans(fx, 0, 150, false).isEmpty());
    fx.video[0].clips[1].effects[0].enabled = false; // switched off counts as none
    CHECK(SmartRender::spans(fx, 0, 150, false) == (QVector<Span>{{50, 100, "b", 0}}));
    // Transitions and fades: generously both sides of the cut, fades on their own clip
    Timeline tr = tl;
    tr.video[0].clips[0].transOut = 5;
    tr.video[0].clips[1].transIn = 5;
    tr.video[0].clips[2].fadeIn = 10;
    CHECK(SmartRender::spans(tr, 0, 150, false)
          == (QVector<Span>{{0, 45, "a", 10}, {55, 100, "b", 5}, {110, 150, "a", 70}}));
    // Title on top: encoded there; a disabled clip on top or a hidden track lets the clip below through
    Timeline ti = tl;
    Clip title = clipOf(9, {}, 20, 0, 10);
    title.kind = ClipKind::Title;
    ti.video[1].clips << title;
    CHECK(SmartRender::spans(ti, 0, 50, false) == (QVector<Span>{{0, 20, "a", 10}, {30, 50, "a", 40}}));
    ti.video[1].clips[0].enabled = false;
    CHECK(SmartRender::spans(ti, 0, 50, false) == (QVector<Span>{{0, 50, "a", 10}}));
    ti.video[1].clips[0].enabled = true;
    ti.video[1].hidden = true;
    CHECK(SmartRender::spans(ti, 0, 50, false) == (QVector<Span>{{0, 50, "a", 10}}));
    // A clip on the upper track covers the lower one
    Timeline up = tl;
    up.video[1].clips << clipOf(10, "c", 10, 0, 20);
    CHECK(SmartRender::spans(up, 0, 50, false) == (QVector<Span>{{0, 10, "a", 10}, {10, 30, "c", 0}, {30, 50, "a", 40}}));
    // Burnt subtitles: encoded under the cue (only when burnt, only enabled tracks)
    Timeline st = tl;
    st.subtitles.resize(1);
    st.subtitles[0].cues << SubtitleCue{20, 5, 15, "x"};
    CHECK(SmartRender::spans(st, 0, 50, false) == (QVector<Span>{{0, 50, "a", 10}}));
    CHECK(SmartRender::spans(st, 0, 50, true) == (QVector<Span>{{0, 5, "a", 10}, {15, 50, "a", 25}}));
    st.subtitles[0].enabled = false;
    CHECK(SmartRender::spans(st, 0, 50, true) == (QVector<Span>{{0, 50, "a", 10}}));
    // Gap: nothing there
    Timeline gap;
    gap.video.resize(1);
    gap.video[0].clips << clipOf(1, "a", 10, 0, 10);
    CHECK(SmartRender::spans(gap, 0, 30, false) == (QVector<Span>{{10, 20, "a", 0}}));
}

struct Result {
    bool ok = false;
    int copiedPercent = 0;
};

Result render(const Timeline& tl, const ExportSettings& s, bool cancelEarly = false)
{
    Exporter ex;
    QEventLoop loop;
    Result r;
    bool finished = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        r.ok = success;
        finished = true;
        if (!success && !cancelEarly) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(msg));
        loop.quit();
    });
    if (cancelEarly)
        QObject::connect(&ex, &Exporter::progress, &ex, [&](int percent) {
            if (percent >= 1 && !finished) ex.cancel();
        });
    QString error;
    if (!ex.start(tl, s, &error)) {
        std::printf("       %s\n", qPrintable(error));
        return r;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    r.copiedPercent = ex.copiedPercent();
    return r;
}

// Decoded frames (yuv420p) as md5 each, and the raw data
struct Frames {
    QVector<QByteArray> md5;
    QByteArray raw;
    QString errors;
};

Frames decode(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:v:0", "-vsync", "passthrough", "-f", "rawvideo", "-pix_fmt",
                       "yuv420p", "-"});
    Frames f;
    while (p.waitForReadyRead(30000) || p.bytesAvailable()) {
        const QByteArray chunk = p.readAllStandardOutput();
        if (chunk.isEmpty() && p.state() == QProcess::NotRunning) break;
        f.raw += chunk;
    }
    p.waitForFinished(30000);
    f.errors = QString::fromUtf8(p.readAllStandardError()).trimmed();
    for (qsizetype at = 0; at + kFrameBytes <= f.raw.size(); at += kFrameBytes)
        f.md5 << QCryptographicHash::hash(QByteArrayView(f.raw).sliced(at, kFrameBytes), QCryptographicHash::Md5);
    return f;
}

QByteArray sound(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:a:0", "-f", "s16le", "-ac", "2", "-"});
    p.waitForFinished(60000);
    return p.readAllStandardOutput();
}

// Mean absolute difference of the luma of frame i
double lumaDiff(const Frames& a, int i, const Frames& b, int j)
{
    const auto* x = reinterpret_cast<const uint8_t*>(a.raw.constData()) + qsizetype(i) * kFrameBytes;
    const auto* y = reinterpret_cast<const uint8_t*>(b.raw.constData()) + qsizetype(j) * kFrameBytes;
    double sum = 0;
    for (int k = 0; k < 640 * 360; ++k) sum += std::abs(int(x[k]) - int(y[k]));
    return sum / (640 * 360);
}

ExportSettings settings(const QString& path, const QString& codec = "libx264")
{
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.videoCodec = codec;
    s.preset = "ultrafast";
    s.crf = 18;
    s.audioCodec = "aac";
    s.smartRender = true;
    s.writer = ExportSettings::Writer::Own;
    return s;
}

// Three cuts (two of `media`, one of `other`) with sound; the middle one with an effect if wanted
Timeline cuts(const QString& media, const QString& other, bool effect)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    const Clip parts[] = {clipOf(1, media, 0, 7, 61), clipOf(2, other, 61, 30, 40), clipOf(3, media, 101, 80, 49)};
    for (const Clip& p : parts) {
        Clip v = p;
        v.linkId = p.id;
        if (effect && p.id == 2) v.effects << EffectInstance{"blur", {}, true};
        tl.video[0].clips << v;
        Clip a = p;
        a.id = p.id + 10;
        a.linkId = p.id;
        tl.audio[0].clips << a;
    }
    return tl;
}

// Smart export against the normal one: every frame either the source's exactly (copied) or close to the normal
// export (encoded); the copied share as reported; the same sound
void compare(const char* name, const QString& dir, const Timeline& tl, ExportSettings s, int minCopied, int maxCopied)
{
    const QString ext = QFileInfo(s.path).suffix();
    s.path = dir + "/" + name + "-smart." + ext;
    const Result smart = render(tl, s);
    if (!CHECK(smart.ok)) return;
    s.path = dir + "/" + name + "-normal." + ext;
    s.smartRender = false;
    if (!CHECK(render(tl, s).ok)) return;
    const Frames fs = decode(dir + "/" + name + "-smart." + ext), fn = decode(s.path);
    if (!CHECK(fs.errors.isEmpty())) std::printf("       %s: %s\n", name, qPrintable(fs.errors.left(300)));
    if (!CHECK_EQ(fs.md5.size(), fn.md5.size())) return;
    QHash<QString, Frames> sources;
    int exact = 0, far = 0;
    const Track& track = tl.video[0];
    for (int i = 0; i < fs.md5.size(); ++i) {
        const int timelineFrame = s.from + i;
        const Clip* c = nullptr;
        for (const Clip& k : track.clips)
            if (k.start <= timelineFrame && timelineFrame < k.end()) c = &k;
        if (!CHECK(c)) return;
        if (!sources.contains(c->mediaPath)) sources.insert(c->mediaPath, decode(c->mediaPath));
        const Frames& src = sources[c->mediaPath];
        const int at = c->in + timelineFrame - c->start;
        if (c->effects.isEmpty() && at < src.md5.size() && fs.md5[i] == src.md5[at]) {
            ++exact;
            continue;
        }
        // Encoded: the source picture (without effect; the export may be in another color standard than the normal
        // one) or like the normal export. Another frame differs far more.
        const double d = c->effects.isEmpty() ? lumaDiff(fs, i, src, at) : lumaDiff(fs, i, fn, i);
        if (d > 3.0 && ++far <= 3) std::printf("       %s: frame %d differs (%.1f)\n", name, i, d);
    }
    CHECK_EQ(far, 0);
    if (!CHECK(exact >= minCopied && exact <= maxCopied)) std::printf("       %s: %d frames copied\n", name, exact);
    CHECK_EQ(smart.copiedPercent, exact ? std::clamp(int(std::lround(exact * 100.0 / fs.md5.size())), 1, 100) : 0);
    // Sound: the same mix, same length (AAC encodes the same samples alike)
    const QByteArray as = sound(dir + "/" + name + "-smart." + ext), an = sound(s.path);
    if (CHECK_EQ(as.size(), an.size())) {
        const auto* x = reinterpret_cast<const int16_t*>(as.constData());
        const auto* y = reinterpret_cast<const int16_t*>(an.constData());
        double diff = 0;
        for (qsizetype k = 0; k < as.size() / 2; ++k) diff += std::abs(int(x[k]) - int(y[k]));
        if (!CHECK(diff / std::max<qsizetype>(1, as.size() / 2) < 0.5)) std::printf("       %s: sound differs\n", name);
    }
}

void testExports(const QString& dir)
{
    // 6 s, key frame (IDR) every 20 frames, B-frames; sound that never repeats (a shift would show)
    const QStringList src{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=6", "-f", "lavfi", "-i",
                          "aevalsrc=0.5*sin(2*PI*(200+150*t)*t):s=48000:d=6", "-c:a", "aac", "-shortest", "-pix_fmt", "yuv420p"};
    const QStringList x264{"-c:v", "libx264", "-preset", "veryfast", "-bf", "3", "-g", "20", "-sc_threshold", "0"};
    const QStringList tag709{"-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"};
    const QString a = Check::makeMedia(dir + "/a.mp4", QStringList(src) << x264 << tag709);
    const QString b = Check::makeMedia(dir + "/b.mkv", QStringList(src) << x264 << tag709 << "-vf" << "hue=h=90");
    const QString sd = Check::makeMedia(dir + "/601.mp4", QStringList(src) << x264); // unmarked: BT.601
    const QString open = Check::makeMedia(dir + "/open.mp4", QStringList(src) << "-c:v" << "libx264" << "-bf" << "3"
                                                                              << "-x264-params"
                                                                              << "keyint=20:open-gop=1:scenecut=0" << tag709);
    if (!CHECK(!a.isEmpty() && !b.isEmpty() && !sd.isEmpty() && !open.isEmpty())) return;

    // Cuts at 7 (IDR at 20), 30 (IDR 40), 80 (IDR 80): copied 20..59 of the first, 40..69 of the second, 80..128 of the
    // third = 40 + 30 + 40 (the third ends at 128, its last whole GOP is 100..119; 120..128 encoded)
    compare("h264", dir, cuts(a, b, false), settings(dir + "/x.mp4"), 100, 120);
    compare("mkv", dir, cuts(a, b, false), settings(dir + "/x.mkv"), 100, 120);
    compare("effect", dir, cuts(a, b, true), settings(dir + "/x.mp4"), 70, 90);
    // Bit rate cap ("Web" quality): GOPs above it are encoded (a game recording would make the file 4x larger), below
    // it copied
    {
        ExportSettings s = settings(dir + "/cap-low.mp4");
        s.maxBitrateK = 1;
        const Result low = render(cuts(a, b, false), s);
        CHECK(low.ok);
        CHECK_EQ(low.copiedPercent, 0);
        s.path = dir + "/cap-high.mp4";
        s.maxBitrateK = 1'000'000;
        const Result high = render(cuts(a, b, false), s);
        CHECK(high.ok);
        CHECK(high.copiedPercent > 50);
    }
    {
        ExportSettings s = settings(dir + "/x.mp4");
        s.from = 30;
        s.to = 109;
        compare("inout", dir, cuts(a, b, false), s, 40, 70);
    }
    // Unmarked SD source: MLT takes it for BT.601, the export follows it so it can copy (tagged 601)
    compare("601", dir, cuts(sd, sd, false), settings(dir + "/x.mp4"), 100, 120);
    // Open GOP: key frames are no IDR, nothing can be cut there
    compare("opengop", dir, cuts(open, open, false), settings(dir + "/x.mp4"), 0, 0);
    if (avcodecHas("libx265")) {
        const QString h = Check::makeMedia(dir + "/hevc.mp4", QStringList(src) << "-c:v" << "libx265" << "-preset" << "ultrafast"
                                                                                << "-x265-params"
                                                                                << "log-level=0:keyint=20:min-keyint=20:open-gop=0:scenecut=0"
                                                                                << tag709);
        if (CHECK(!h.isEmpty())) compare("hevc", dir, cuts(h, h, false), settings(dir + "/x.mp4", "libx265"), 100, 120);
    }
    // Cancelling while copying/encoding: nothing left behind
    Timeline longer = cuts(a, b, false);
    for (int i = 1; i < 20; ++i)
        for (Track* t : {&longer.video[0], &longer.audio[0]}) {
            Clip c = t->clips.first();
            c.id = 100 + i + (t == &longer.audio[0] ? 50 : 0);
            c.linkId = 0;
            c.start = 150 + i * 61;
            t->clips << c;
        }
    ExportSettings s = settings(dir + "/cancel.mp4");
    CHECK(!render(longer, s, true).ok);
    CHECK(!QFileInfo::exists(s.path));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("smart_render");
    testSpans();
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (qEnvironmentVariableIsSet("SMART_KEEP")) { // keep the files for a look
        tmp.setAutoRemove(false);
        std::printf("files in %s\n", qPrintable(tmp.path()));
    }
    if (CHECK(tmp.isValid())) testExports(tmp.path());
    return Check::result();
}
