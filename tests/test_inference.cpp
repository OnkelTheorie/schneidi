// SPDX-License-Identifier: AGPL-3.0-only
// ONNX Runtime through QLibrary (engine/Inference) and the YAMNet path of engine/AudioEvents, with tiny models built
// here byte by byte (ONNX protobuf) - no download, no real YAMNet. Needs an onnxruntime library on this machine (the
// build finds a system one; SCHNEIDI_ONNXRUNTIME names another); without one the test is skipped.
// - Session: input/output names and shapes (fixed and dynamic), run (Abs -> ReduceMax -> Mul), wrong input count
// - A YAMNet stand-in (waveform [1, 15600] -> 521 scores, "Applause" = the window's loudest sample): yamnetWith finds
//   applause where the noise is, at YAMNet's 0.48 s grid; nothing else
// - analyzeFile with SCHNEIDI_YAMNET pointing at the stand-in: by "yamnet", silence and the shot's exact time from
//   the heuristics, the cache keyed by the model; Engine::Heuristic ignores the model
#include "engine/AudioEvents.h"
#include "engine/Inference.h"

#include "render_check.h"
#include "synth_sounds.h"

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>

#ifndef SCHNEIDI_TEST_ONNXRUNTIME
#define SCHNEIDI_TEST_ONNXRUNTIME ""
#endif

namespace {

// ---- A minimal protobuf writer for ONNX models ----
QByteArray varint(quint64 v)
{
    QByteArray out;
    do {
        char b = char(v & 0x7f);
        v >>= 7;
        if (v) b = char(b | 0x80);
        out += b;
    } while (v);
    return out;
}
QByteArray field(int number, quint64 value) { return varint(quint64(number) << 3) + varint(value); }
QByteArray field(int number, const QByteArray& bytes) { return varint((quint64(number) << 3) | 2) + varint(quint64(bytes.size())) + bytes; }

// ValueInfoProto: a float tensor with dims (a negative dim = dynamic, named)
QByteArray valueInfo(const char* name, const std::vector<int64_t>& dims)
{
    QByteArray shape;
    for (int64_t d : dims) shape += field(1, d >= 0 ? field(1, quint64(d)) : field(2, QByteArray("n")));
    const QByteArray tensor = field(1, quint64(1)) + field(2, shape); // elem_type FLOAT
    return field(1, QByteArray(name)) + field(2, field(1, tensor));
}
QByteArray node(const QStringList& in, const char* out, const char* op)
{
    QByteArray n;
    for (const QString& i : in) n += field(1, i.toUtf8());
    return n + field(2, QByteArray(out)) + field(4, QByteArray(op));
}
QByteArray floatTensor(const char* name, const std::vector<int64_t>& dims, const std::vector<float>& values)
{
    QByteArray t;
    for (int64_t d : dims) t += field(1, quint64(d));
    t += field(2, quint64(1));
    t += field(8, QByteArray(name));
    t += field(9, QByteArray(reinterpret_cast<const char*>(values.data()), int(values.size() * sizeof(float))));
    return t;
}
QByteArray model(const QByteArray& graph)
{
    return field(1, quint64(7)) + field(8, field(2, quint64(13))) + field(7, graph);
}

// y = max|x| * c: input [inDims], output [1, classes] with c[hot] = 1
QByteArray scoreModel(const std::vector<int64_t>& inDims, int classes, int hot)
{
    std::vector<float> c(size_t(classes), 0.f);
    c[size_t(hot)] = 1.f;
    QByteArray g;
    g += field(1, node({"waveform"}, "a", "Abs"));
    g += field(1, node({"a"}, "m", "ReduceMax")); // keepdims=1, all axes -> [1] or [1, 1]
    g += field(1, node({"m", "c"}, "scores", "Mul"));
    g += field(2, QByteArray("stand-in"));
    g += field(5, floatTensor("c", {1, classes}, c));
    g += field(11, valueInfo("waveform", inDims));
    g += field(12, valueInfo("scores", {1, classes}));
    return model(g);
}

void testSession()
{
    QString error;
    // Dynamic input: Identity keeps the shape
    QByteArray g = field(1, node({"x"}, "y", "Identity")) + field(2, QByteArray("id")) +
                   field(11, valueInfo("x", {-1, 3})) + field(12, valueInfo("y", {-1, 3}));
    auto id = Inference::Session::fromBytes(model(g), &error);
    if (!CHECK(id)) {
        std::printf("%s\n", qPrintable(error));
        return;
    }
    CHECK_EQ(int(id->inputs().size()), 1);
    CHECK_EQ(id->inputs()[0].name, QString("x"));
    CHECK(id->inputs()[0].shape == (std::vector<int64_t>{-1, 3}));
    CHECK(id->inputs()[0].isFloat);
    CHECK_EQ(id->outputs()[0].name, QString("y"));
    Inference::Tensor t{{2, 3}, {1, -2, 3, -4, 5, -6}};
    auto out = id->run({t}, &error);
    if (CHECK(out)) {
        CHECK((*out)[0].shape == (std::vector<int64_t>{2, 3}));
        CHECK((*out)[0].data == t.data);
    }
    // Wrong input count / size: an error, no crash
    CHECK(!id->run({}, &error));
    CHECK(!error.isEmpty());
    CHECK(!id->run({Inference::Tensor{{2, 3}, {1, 2}}}, &error));
    CHECK(!id->run({Inference::Tensor{{2, 4}, std::vector<float>(8, 0.f)}}, &error)); // ONNX Runtime refuses it
    CHECK(error.contains("dimension") || error.contains("shape") || error.contains("Got") || !error.isEmpty());

    // Abs -> ReduceMax -> Mul
    auto s = Inference::Session::fromBytes(scoreModel({1, 8}, 4, 2), &error);
    if (!CHECK(s)) {
        std::printf("%s\n", qPrintable(error));
        return;
    }
    CHECK(s->outputs()[0].shape == (std::vector<int64_t>{1, 4}));
    out = s->run({Inference::Tensor{{1, 8}, {0.1f, -0.7f, 0.2f, 0, 0, 0, 0.3f, 0}}}, &error);
    if (CHECK(out)) CHECK((*out)[0].data == (std::vector<float>{0, 0, 0.7f, 0}));
    // Not a model
    CHECK(!Inference::Session::fromBytes(QByteArray("definitely not onnx"), &error));
    CHECK(!error.isEmpty());
    CHECK(!Inference::Session::open("/no/such/model.onnx", &error));
}

// The class list in YAMNet's format; "Applause" at `hot`, "Speech" at 0
QString writeLabels(const QString& path, int classes, int hot)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write("index,mid,display_name\n");
    for (int i = 0; i < classes; ++i) {
        const QByteArray name = i == hot ? "Applause" : i == 0 ? "Speech" : i == 7 ? "\"Child speech, kid speaking\"" : "Class " + QByteArray::number(i);
        f.write(QByteArray::number(i) + ",/m/x" + QByteArray::number(i) + "," + name + "\n");
    }
    return path;
}

void testYamnetStandIn(const QDir& dir)
{
    constexpr int kClasses = 521, kApplause = 61;
    const QString modelPath = dir.filePath("stand-in.onnx");
    {
        QFile f(modelPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(scoreModel({1, 15600}, kClasses, kApplause));
    }
    const QString labels = writeLabels(dir.filePath("stand-in.csv"), kClasses, kApplause);
    // Noise 3.0-5.0 s, a shot at 8.0 s, else (nearly) silence
    Synth::Builder b(AudioEvents::kRate, 10, 3);
    for (size_t i = size_t(3.0 * b.rate); i < size_t(5.0 * b.rate); ++i) b.s[i] += float(0.6 * b.noise());
    Synth::shot(b, 8.0);
    b.floor();
    QString error;
    const QVector<AudioEvents::Event> ev = AudioEvents::yamnetWith(modelPath, labels, b.s, &error);
    CHECK(error.isEmpty());
    int applause = 0;
    for (const AudioEvents::Event& e : ev) {
        CHECK_EQ(e.by, QString("yamnet"));
        if (e.kind != "applause") continue;
        ++applause;
        // The first window reaching 3.0 s starts at 2.4 (0.48 grid, 0.96 long); the last ending after 5.0 ends 5.76
        if (applause == 1) {
            CHECK(std::abs(e.from - 2.4) < 0.01);
            CHECK(e.to > 4.9 && e.to < 5.8);
            CHECK(e.conf > 0.55 && e.conf <= 1.0);
        }
    }
    CHECK_EQ(applause, 2); // the noise and the shot (both loud windows)
    CHECK(ev.size() == applause);
    // Errors: missing labels, labels of another size, not a model
    CHECK(AudioEvents::yamnetWith(modelPath, dir.filePath("none.csv"), b.s, &error).isEmpty());
    CHECK(error.contains("class list"));
    CHECK(AudioEvents::yamnetWith(modelPath, writeLabels(dir.filePath("small.csv"), 10, 3), b.s, &error).isEmpty());
    CHECK(error.contains("10 class"));
    CHECK(AudioEvents::yamnetWith(labels, labels, b.s, &error).isEmpty());
    CHECK(!error.isEmpty());

    // The whole file path with the stand-in as the installed model
    qputenv("SCHNEIDI_YAMNET", modelPath.toUtf8());
    CHECK(AudioEvents::yamnetAvailable());
    const QString wav = dir.filePath("stand-in.wav");
    CHECK(Synth::writeWav(wav, b.s, AudioEvents::kRate));
    const AudioEvents::Result r = AudioEvents::analyzeFile(wav);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.by, QString("yamnet"));
    bool silence = false, shot = false;
    for (const AudioEvents::Event& e : r.events) {
        silence |= e.kind == "silence" && e.by == "heuristic" && e.from < 0.01 && std::abs(e.to - 3.0) < 0.05;
        // YAMNet's stand-in says nothing about shots: the heuristic's shot stays, by itself
        shot |= e.kind == "shot" && std::abs(e.from - 8.0) <= 0.02;
    }
    CHECK(silence);
    CHECK(shot);
    CHECK(AudioEvents::analyzeFile(wav).cached);
    AudioEvents::Options h;
    h.engine = AudioEvents::Engine::Heuristic;
    const AudioEvents::Result hr = AudioEvents::analyzeFile(wav, 0, h);
    CHECK_EQ(hr.by, QString("heuristic"));
    CHECK(!hr.cached); // another key than the YAMNet result
    AudioEvents::Options y;
    y.engine = AudioEvents::Engine::Yamnet;
    CHECK_EQ(AudioEvents::analyzeFile(wav, 0, y).by, QString("yamnet"));
    qunsetenv("SCHNEIDI_YAMNET");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("inference");
    Check::initEngine();
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_ONNXRUNTIME") && QString(SCHNEIDI_TEST_ONNXRUNTIME).isEmpty())
        return Check::skip("no onnxruntime library on this machine");
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_ONNXRUNTIME")) qputenv("SCHNEIDI_ONNXRUNTIME", SCHNEIDI_TEST_ONNXRUNTIME);
    QString error, version;
    if (!Inference::available(&error)) {
        std::printf("onnxruntime does not load: %s\n", qPrintable(error));
        return Check::skip("onnxruntime does not load");
    }
    CHECK(Inference::probe(Inference::libraryPath(), &error, &version));
    CHECK(!version.isEmpty());
    std::printf("ONNX Runtime %s (%s)\n", qPrintable(Inference::version()), qPrintable(Inference::libraryPath()));
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/audio-events").removeRecursively();
    QTemporaryDir tmp;
    testSession();
    testYamnetStandIn(QDir(tmp.path()));
    return Check::result();
}
