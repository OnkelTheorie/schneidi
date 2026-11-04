// SPDX-License-Identifier: AGPL-3.0-only
// Faces with YuNet (engine/FaceDetect), the "faces" and "onnxruntime" extension entries (engine/Extensions) and faces
// in the material index (engine/MediaIndex). Never downloads anything.
// Always (pure):
// - decode of hand-made output tensors of a 64 x 64 input (score = sqrt(cls * obj), clamped; boxes from the cells of
//   strides 8/16/32), suppression of overlapping boxes, overlap; YUV 4:2:0 to BGR
// - the catalog: ONNX Runtime 1.20.1 (Linux x64, Windows x64) and YuNet with pinned URL, size, SHA-256, file names and
//   the licence as second download; the face model brings the runtime; installed = model and LICENSE in faces/;
//   SCHNEIDI_YUNET; the cache key of the index names the face way, and none keeps the old key
// With the model (SCHNEIDI_YUNET = face_detection_yunet_2023mar.onnx; the test's own folders hold no extension) and a
// runtime (SCHNEIDI_ONNXRUNTIME, else the one the build found); else skipped (77) after the pure part:
// - synthetic pictures (ffmpeg test sources, grey) give no face; a drawn cartoon face (no real person) gives no face
//   above the threshold, but the best weak candidate's box lies on the drawn head (the decode's geometry is right)
// - `index` of a synthetic clip: without the model no "faces"; with it measured anew (the key changed), faces 0 per
//   shot, then cached
#include "cli/Commands.h"
#include "engine/Extensions.h"
#include "engine/FaceDetect.h"
#include "engine/Inference.h"
#include "engine/MediaIndex.h"

#include "check.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

#ifndef SCHNEIDI_TEST_ONNXRUNTIME
#define SCHNEIDI_TEST_ONNXRUNTIME ""
#endif

namespace {

bool near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }

void testDecode()
{
    // A 64 x 64 input: 8 x 8 cells at stride 8, 4 x 4 at 16, 2 x 2 at 32
    std::vector<FaceDetect::Level> levels(3);
    for (int s = 0; s < 3; ++s) {
        const int cells = (64 / FaceDetect::kStrides[s]) * (64 / FaceDetect::kStrides[s]);
        levels[size_t(s)].stride = FaceDetect::kStrides[s];
        levels[size_t(s)].cls.assign(size_t(cells), 0.0f);
        levels[size_t(s)].obj.assign(size_t(cells), 0.0f);
        levels[size_t(s)].bbox.assign(size_t(4 * cells), 0.0f);
    }
    const auto set = [&](int level, int row, int col, float cls, float obj, std::array<float, 4> box) {
        FaceDetect::Level& l = levels[size_t(level)];
        const size_t i = size_t(row) * size_t(64 / l.stride) + size_t(col);
        l.cls[i] = cls;
        l.obj[i] = obj;
        for (int k = 0; k < 4; ++k) l.bbox[4 * i + size_t(k)] = box[size_t(k)];
    };
    // Stride 8, row 2, col 3: centre (3.5, 2.5) cells = (28, 20) px, size e^ln2 x e^ln4 cells = 16 x 32 px
    set(0, 2, 3, 0.81f, 1.0f, {0.5f, 0.5f, std::log(2.0f), std::log(4.0f)});
    // Stride 16, row 1, col 1: the same box with a lower score (0.8) - suppressed
    set(1, 1, 1, 0.64f, 1.0f, {0.75f, 0.25f, 0.0f, std::log(2.0f)});
    // Stride 32, row 1, col 1: a small face elsewhere, score sqrt(0.5 * 0.98) = 0.7
    set(2, 1, 1, 0.5f, 0.98f, {0.5f, 0.5f, std::log(0.25f), std::log(0.25f)});
    // Below the threshold (0.3) and clamped out (obj < 0 -> 0)
    set(0, 6, 6, 0.3f, 0.3f, {0, 0, 0, 0});
    set(0, 7, 7, 1.5f, -0.2f, {0, 0, 0, 0});
    std::vector<FaceDetect::Face> faces = FaceDetect::decode(levels, 64, 64);
    CHECK_EQ(int(faces.size()), 3);
    faces = FaceDetect::suppress(faces);
    if (CHECK_EQ(int(faces.size()), 2)) {
        CHECK(near(faces[0].score, 0.9) && near(faces[0].x, 20) && near(faces[0].y, 4) && near(faces[0].w, 16)
              && near(faces[0].h, 32));
        CHECK(near(faces[1].score, std::sqrt(0.49), 1e-3) && near(faces[1].x, 44) && near(faces[1].y, 44)
              && near(faces[1].w, 8) && near(faces[1].h, 8));
    }
    // A lower threshold keeps the weak one too; a level of the wrong size is left out
    CHECK_EQ(int(FaceDetect::decode(levels, 64, 64, 0.25f).size()), 4);
    CHECK_EQ(int(FaceDetect::decode(levels, 128, 64).size()), 0);
    // Overlap (IoU) and suppression
    const FaceDetect::Face a{0, 0, 10, 10, 0.9}, b{5, 0, 10, 10, 0.8}, c{20, 20, 5, 5, 0.7};
    CHECK(near(FaceDetect::overlap(a, a), 1.0));
    CHECK(near(FaceDetect::overlap(a, b), 50.0 / 150.0));
    CHECK(near(FaceDetect::overlap(a, c), 0.0));
    CHECK_EQ(int(FaceDetect::suppress({b, a, c}).size()), 2);       // IoU 0.33 > 0.3: b goes
    CHECK_EQ(int(FaceDetect::suppress({b, a, c}, 0.5f).size()), 3); // allowed overlap
    CHECK(near(FaceDetect::suppress({b, a, c}).front().score, 0.9));  // best first

    // YUV 4:2:0 (limited range) to BGR: one row, red chroma; bright (R full) and BT.601 red (81, 90, 240)
    uint8_t y[4] = {235, 16, 81, 81}, u[2] = {90, 90}, v[2] = {240, 240};
    const uint8_t* planes[3] = {y, u, v};
    const int lines[3] = {4, 1, 1};
    const std::vector<uint8_t> bgr = FaceDetect::bgrFromYuv420(planes, lines, 4, 1, false);
    if (CHECK_EQ(int(bgr.size()), 12)) {
        CHECK(bgr[2] > 250 && bgr[1] > 150 && bgr[1] < 200); // Y 235 with the red chroma
        CHECK(bgr[6 + 2] > 200 && bgr[6 + 0] < 40 && bgr[6 + 1] < 40); // BT.601 red (81, 90, 240)
    }
    uint8_t g[4] = {128, 128, 128, 128}, n[2] = {128, 128};
    const uint8_t* grey[3] = {g, n, n};
    const std::vector<uint8_t> gb = FaceDetect::bgrFromYuv420(grey, lines, 4, 1, true);
    CHECK(gb.size() == 12 && gb[0] == 128 && gb[1] == 128 && gb[2] == 128);
}

void testCatalog(const QDir& tmp)
{
    const Extensions::Item* yunet = Extensions::find("faces");
    if (CHECK(yunet != nullptr)) {
        CHECK(yunet->kind == Extensions::Kind::FaceModel);
        CHECK_EQ(yunet->fileName, QString("face_detection_yunet_2023mar.onnx"));
        CHECK(yunet->url.startsWith("https://media.githubusercontent.com/media/opencv/opencv_zoo/"
                                    "48b10fa505aeb51fbd127fc60a5c660040e823b7/"));
        CHECK(yunet->url.endsWith("/face_detection_yunet_2023mar.onnx"));
        CHECK_EQ(yunet->sha256, QByteArray("8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"));
        CHECK_EQ(yunet->size, qint64(232589));
        // Its licence text, the same commit, beside the model
        CHECK_EQ(yunet->configName, QString("LICENSE"));
        CHECK(yunet->configUrl.startsWith("https://raw.githubusercontent.com/opencv/opencv_zoo/"
                                          "48b10fa505aeb51fbd127fc60a5c660040e823b7/"));
        CHECK_EQ(yunet->configSha256, QByteArray("c83b8120c50ccbd4c4f96edf53141bdd566ebb8f8e9227e415326aa1b1aba958"));
        CHECK_EQ(yunet->configSize, qint64(1085));
        CHECK(Extensions::parseList("faces") == QVector<const Extensions::Item*>{yunet});
    }
    const Extensions::Item* ort = Extensions::find("onnxruntime");
#if (defined(Q_OS_WIN) || defined(Q_OS_LINUX)) && defined(Q_PROCESSOR_X86_64)
    if (CHECK(ort != nullptr)) {
        CHECK(ort->kind == Extensions::Kind::Runtime);
        CHECK(ort->url.startsWith("https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/"));
        CHECK(ort->url.endsWith("/" + ort->fileName));
#ifdef Q_OS_WIN
        CHECK_EQ(ort->fileName, QString("onnxruntime-win-x64-1.20.1.zip"));
        CHECK_EQ(ort->sha256, QByteArray("78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581"));
        CHECK_EQ(ort->size, qint64(65585496));
#else
        CHECK_EQ(ort->fileName, QString("onnxruntime-linux-x64-1.20.1.tgz"));
        CHECK_EQ(ort->sha256, QByteArray("67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105"));
        CHECK_EQ(ort->size, qint64(6191581));
#endif
    }
#endif
    // The face model brings the runtime (none installed in the test's own folders)
    if (yunet && ort && Extensions::onnxRuntimeLibrary().isEmpty() && Extensions::piperOnnxRuntime().isEmpty()) {
        const auto deps = Extensions::withDependencies({yunet});
        CHECK(deps.size() == 2 && deps[0] == ort && deps[1] == yunet);
    }
    // Every entry with a second download has it pinned too
    for (const Extensions::Item& i : Extensions::catalog())
        if (!i.configUrl.isEmpty()) CHECK(i.configUrl.startsWith("https://") && i.configSha256.size() == 64 && i.configSize > 0);

    // Installed = model and licence in the extension's folder
    qunsetenv("SCHNEIDI_YUNET");
    if (yunet) {
        CHECK(!Extensions::isInstalled(*yunet));
        CHECK(Extensions::faceModel().isEmpty());
        const QDir dir(QDir(Extensions::rootDir()).filePath("faces"));
        CHECK(dir.mkpath("."));
        QFile model(dir.filePath(yunet->fileName));
        CHECK(model.open(QIODevice::WriteOnly) && model.write("not a model") > 0);
        model.close();
        CHECK(!Extensions::isInstalled(*yunet)); // the licence is missing
        QFile licence(dir.filePath("LICENSE"));
        CHECK(licence.open(QIODevice::WriteOnly) && licence.write("MIT") > 0);
        licence.close();
        CHECK(Extensions::isInstalled(*yunet));
        CHECK_EQ(Extensions::faceModel(), QFileInfo(model.fileName()).absoluteFilePath());
        // Not a model: available as a file, but no detector
        QString error;
        CHECK(FaceDetect::Detector::open(&error) == nullptr && !error.isEmpty());
        CHECK(Extensions::remove(*yunet));
        CHECK(!dir.exists(yunet->fileName) && !dir.exists("LICENSE"));
        CHECK(Extensions::faceModel().isEmpty());
    }
    QString why;
    CHECK(!FaceDetect::available(&why) && why.contains("faces"));
    CHECK(FaceDetect::way().isEmpty());
    // SCHNEIDI_YUNET names a file (missing = none)
    const QString own = tmp.filePath("my-yunet.onnx");
    QFile f(own);
    CHECK(f.open(QIODevice::WriteOnly) && f.write("x") == 1);
    f.close();
    qputenv("SCHNEIDI_YUNET", own.toUtf8());
    CHECK_EQ(Extensions::faceModel(), QFileInfo(own).absoluteFilePath());
    qputenv("SCHNEIDI_YUNET", tmp.filePath("missing.onnx").toUtf8());
    CHECK(Extensions::faceModel().isEmpty());
    qunsetenv("SCHNEIDI_YUNET");
    QString error;
    CHECK(FaceDetect::Detector::openModel(tmp.filePath("missing.onnx"), &error) == nullptr && !error.isEmpty());

    // The index's key names the face way; none keeps the key the file had before faces were measured
    CHECK(FaceDetect::wayKey({}).isEmpty());
    CHECK(FaceDetect::wayKey(own).startsWith("yunet|"));
    CHECK_EQ(MediaIndex::cacheKey("/x/a.mp4", "heuristic", QString()), MediaIndex::cacheKey("/x/a.mp4", "heuristic"));
    CHECK(MediaIndex::cacheKey("/x/a.mp4", "heuristic", FaceDetect::wayKey(own)) != MediaIndex::cacheKey("/x/a.mp4", "heuristic"));
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) return {{"failed", "NO_COMMAND"}};
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return QJsonObject{{"failed", e.code}};
    }
}

// The faces of a picture file as raw BGR (ffmpeg -f rawvideo); minScore as given
std::optional<std::vector<FaceDetect::Face>> facesOf(FaceDetect::Detector& d, const QByteArray& bgr, int w, int h,
                                                     float minScore = FaceDetect::kMinScore)
{
    if (bgr.size() != qsizetype(w) * h * 3) return std::nullopt;
    QString error;
    auto faces = d.detect(reinterpret_cast<const uint8_t*>(bgr.constData()), w, h, w * 3, &error, minScore);
    if (!faces) std::printf("detect: %s\n", qPrintable(error));
    return faces;
}

void testModel(const QDir& tmp, const QString& model)
{
    QString error;
    auto d = FaceDetect::Detector::openModel(model, &error);
    if (!CHECK(d != nullptr)) {
        std::printf("%s\n", qPrintable(error));
        return;
    }
    CHECK(!d->detect(nullptr, 10, 10, 30, &error) && !error.isEmpty());
    // Synthetic pictures: no faces (at 640 x 360, the index's 320 x 180, and a tall one)
    if (Check::haveFfmpeg()) {
        for (const QString& source : {QString("testsrc2"), QString("smptehdbars"), QString("mandelbrot"),
                                      QString("color=c=gray"), QString("cellauto")}) {
            for (const QSize size : {QSize(640, 360), QSize(320, 180), QSize(360, 640)}) {
                const QString file = tmp.filePath(QString("pic-%1-%2.bgr").arg(source.section('=', 0, 0)).arg(size.width()));
                const QString lavfi = source + (source.contains('=') ? ":" : "=") + QString("s=%1x%2").arg(size.width()).arg(size.height());
                if (!CHECK(!Check::makeMedia(file, {"-f", "lavfi", "-i", lavfi, "-frames:v", "1", "-f", "rawvideo",
                                                    "-pix_fmt", "bgr24"})
                                .isEmpty()))
                    continue;
                QFile f(file);
                CHECK(f.open(QIODevice::ReadOnly));
                const auto faces = facesOf(*d, f.readAll(), size.width(), size.height());
                if (CHECK(faces.has_value())) {
                    if (!CHECK_EQ(int(faces->size()), 0))
                        std::printf("  %s %dx%d: face %.2f\n", qPrintable(source), size.width(), size.height(),
                                    faces->front().score);
                }
            }
        }
    }
    // A drawn cartoon head (shapes, no real person): no face above the threshold, but YuNet's best guess is where it is
    QImage c(640, 360, QImage::Format_RGB32);
    c.fill(QColor(60, 90, 140));
    {
        QPainter p(&c);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(224, 172, 140));
        p.drawEllipse(QPointF(320, 180), 70, 92); // the head: x 250..390, y 88..272
        p.setBrush(QColor(70, 45, 30));
        p.drawChord(QRectF(250, 80, 140, 90), 0, 180 * 16);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(294, 165), 13, 8);
        p.drawEllipse(QPointF(346, 165), 13, 8);
        p.setBrush(QColor(50, 40, 30));
        p.drawEllipse(QPointF(294, 165), 6, 6);
        p.drawEllipse(QPointF(346, 165), 6, 6);
        p.setPen(QPen(QColor(150, 90, 80), 4));
        p.drawLine(320, 175, 315, 205);
        p.drawArc(QRectF(295, 205, 50, 25), 200 * 16, 140 * 16);
    }
    const QImage bgr = c.convertToFormat(QImage::Format_BGR888);
    const auto strict = d->detect(bgr.constBits(), bgr.width(), bgr.height(), int(bgr.bytesPerLine()), &error);
    const auto weak = d->detect(bgr.constBits(), bgr.width(), bgr.height(), int(bgr.bytesPerLine()), &error, 0.05f);
    if (CHECK(strict && weak)) {
        CHECK_EQ(int(strict->size()), 0);
        if (CHECK(!weak->empty())) {
            const FaceDetect::Face& f = weak->front();
            std::printf("cartoon: best %.3f at %.3f %.3f %.3f %.3f\n", f.score, f.x, f.y, f.w, f.h);
            CHECK(std::abs(f.x + f.w / 2 - 0.5) < 0.06 && std::abs(f.y + f.h / 2 - 0.5) < 0.1);
            CHECK(f.w > 0.15 && f.w < 0.35 && f.h > 0.35 && f.h < 0.7); // the head: 0.22 x 0.51 of the picture
        }
    }

    // The index: without the model no faces, with it measured anew and cached
    if (!Check::haveFfmpeg()) return;
    const QString media = Check::makeMedia(tmp.filePath("clip.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=25:d=3", "-c:v", "libx264",
                                            "-pix_fmt", "yuv420p"});
    if (!CHECK(!media.isEmpty())) return;
    MediaIndex::setEventSource([](const QString&) { return QVector<AudioEvents::Event>{}; });
    qunsetenv("SCHNEIDI_YUNET");
    QJsonObject r = run("index", {{"files", QJsonArray{media}}});
    QJsonArray shots = r["media"][0]["shots"].toArray();
    CHECK(!shots.isEmpty());
    for (const QJsonValue& s : shots) CHECK(!s["quality"].toObject().contains("faces"));
    qputenv("SCHNEIDI_YUNET", model.toUtf8());
    CHECK(!FaceDetect::way().isEmpty());
    r = run("index", {{"files", QJsonArray{media}}});
    CHECK_EQ(r["cached"].toBool(true), false); // the face model is part of the key
    shots = r["media"][0]["shots"].toArray();
    CHECK(!shots.isEmpty());
    for (const QJsonValue& s : shots) {
        CHECK_EQ(s["quality"]["faces"].toInt(-1), 0);
        CHECK(!s["quality"].toObject().contains("face_box"));
    }
    MediaIndex::clearMemoryCache();
    r = run("index", {{"files", QJsonArray{media}}});
    CHECK(r["cached"].toBool());
    CHECK_EQ(r["media"][0]["shots"][0]["quality"]["faces"].toInt(-1), 0);
    MediaIndex::setEventSource({});
}

} // namespace

int main(int argc, char** argv)
{
    // The caller's model (SCHNEIDI_YUNET) is kept for the model part; the pure part sets its own
    const QString model = qEnvironmentVariable("SCHNEIDI_YUNET");
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("face-detect");
    Check::initEngine();
    QTemporaryDir tmp;
    QDir(Extensions::rootDir()).removeRecursively();
    QDir(MediaIndex::cacheDir()).removeRecursively();
    testDecode();
    testCatalog(QDir(tmp.path()));
    if (Check::fails()) return Check::result();

    if (qEnvironmentVariableIsEmpty("SCHNEIDI_ONNXRUNTIME") && !QString(SCHNEIDI_TEST_ONNXRUNTIME).isEmpty())
        qputenv("SCHNEIDI_ONNXRUNTIME", SCHNEIDI_TEST_ONNXRUNTIME);
    if (model.isEmpty() || !QFileInfo(model).isFile())
        return Check::skip("YuNet not given (SCHNEIDI_YUNET = face_detection_yunet_2023mar.onnx)");
    QString why;
    if (!Inference::available(&why)) {
        std::printf("%s\n", qPrintable(why));
        return Check::skip("ONNX Runtime not there");
    }
    std::printf("ONNX Runtime %s (%s), model %s\n", qPrintable(Inference::version()), qPrintable(Inference::libraryPath()),
                qPrintable(model));
    testModel(QDir(tmp.path()), model);
    return Check::result();
}
