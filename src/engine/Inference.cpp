// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Inference.h"

#include "engine/Extensions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QThread>

#include <algorithm>

namespace Inference {

namespace {

// ---- The part of ONNX Runtime's C API schneidi uses, declared here (no header needed at build time) ----
// The API is a table of function pointers (struct OrtApi in onnxruntime_c_api.h) that only ever grows at the end;
// GetApi(version) hands out the table of that version. Version 11 (ONNX Runtime 1.11, 2022) covers every function
// below (all are from the first version), so any runtime from 1.11 on works, Piper's 1.14 included.
constexpr uint32_t kApiVersion = 11;

#if defined(_WIN32) && !defined(_WIN64)
#define ORT_CALL __stdcall
#else
#define ORT_CALL
#endif

struct OrtStatus;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtValue;
struct OrtMemoryInfo;
struct OrtTypeInfo;
struct OrtTensorTypeAndShapeInfo;
struct OrtAllocator;
struct OrtRunOptions;

struct OrtApiBase {
    const void* (ORT_CALL* GetApi)(uint32_t version);
    const char* (ORT_CALL* GetVersionString)();
};
using GetApiBaseFn = const OrtApiBase* (ORT_CALL*)();

// Positions in the table (onnxruntime_c_api.h, in declaration order)
enum Slot : int {
    kGetErrorMessage = 2,
    kCreateEnv = 3,
    kCreateSessionFromArray = 8,
    kRun = 9,
    kCreateSessionOptions = 10,
    kSetIntraOpNumThreads = 24,
    kSetInterOpNumThreads = 25,
    kSessionGetInputCount = 30,
    kSessionGetOutputCount = 31,
    kSessionGetInputTypeInfo = 33,
    kSessionGetOutputTypeInfo = 34,
    kSessionGetInputName = 36,
    kSessionGetOutputName = 37,
    kCreateTensorWithDataAsOrtValue = 49,
    kGetTensorMutableData = 51,
    kCastTypeInfoToTensorInfo = 55,
    kGetTensorElementType = 60,
    kGetDimensionsCount = 61,
    kGetDimensions = 62,
    kGetTensorShapeElementCount = 64,
    kGetTensorTypeAndShape = 65,
    kCreateCpuMemoryInfo = 69,
    kAllocatorFree = 76,
    kGetAllocatorWithDefaultOptions = 78,
    kReleaseEnv = 92,
    kReleaseStatus = 93,
    kReleaseMemoryInfo = 94,
    kReleaseSession = 95,
    kReleaseValue = 96,
    kReleaseTypeInfo = 98,
    kReleaseTensorTypeAndShapeInfo = 99,
    kReleaseSessionOptions = 100,
};

constexpr int kLoggingWarning = 2;  // ORT_LOGGING_LEVEL_WARNING
constexpr int kArenaAllocator = 1;  // OrtArenaAllocator
constexpr int kMemTypeDefault = 0;  // OrtMemTypeDefault
constexpr int kElementFloat = 1;    // ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT

struct Api {
    const void* const* table = nullptr;
    template <typename F>
    F fn(Slot s) const
    {
        return reinterpret_cast<F>(const_cast<void*>(table[s]));
    }
    // Calls a function that returns an OrtStatus*: true = ok, else *error = its message (status released)
    template <typename... A, typename... P>
    bool call(Slot s, QString* error, P... args) const
    {
        using F = OrtStatus*(ORT_CALL*)(A...);
        OrtStatus* st = fn<F>(s)(args...);
        if (!st) return true;
        if (error) *error = QString::fromUtf8(fn<const char*(ORT_CALL*)(const OrtStatus*)>(kGetErrorMessage)(st));
        fn<void(ORT_CALL*)(OrtStatus*)>(kReleaseStatus)(st);
        return false;
    }
    template <typename T>
    void release(Slot s, T* p) const
    {
        if (p) fn<void(ORT_CALL*)(T*)>(s)(p);
    }
};

// One library opened with its API table
struct Loaded {
    std::unique_ptr<QLibrary> lib;
    Api api;
    QString version;
};

std::optional<Loaded> load(const QString& file, QString* error)
{
    if (file.isEmpty()) {
        if (error) *error = "no file";
        return std::nullopt;
    }
#ifdef Q_OS_WIN
    // The official build needs the MSVC runtime (VCRUNTIME140, MSVCP140 …), which a fresh Windows lacks and which
    // Windows does not look for in the library's own folder: copies beside it (Extensions::provideRuntime) are loaded
    // first by their full path, so the library finds them in the process. vcruntime before msvcp (which needs it).
    if (QFileInfo(file).isAbsolute()) {
        // Piper's copy (or an older install) may lie there without them: the package's copies first (only inside
        // the extensions folder; a file already there stays)
        Extensions::provideRuntime(file);
        const QDir here = QFileInfo(file).dir();
        for (const char* pattern : {"vcruntime140*.dll", "msvcp140*.dll"})
            for (const QString& dll : here.entryList({pattern}, QDir::Files, QDir::Name)) {
                QLibrary runtimeDll(here.filePath(dll));
                runtimeDll.load(); // stays loaded (QLibrary's destructor does not unload); a failure shows below
            }
    }
#endif
    auto lib = std::make_unique<QLibrary>(file);
    if (!lib->load()) {
        if (error) *error = "cannot load " + file + ": " + lib->errorString();
        return std::nullopt;
    }
    const auto base = reinterpret_cast<GetApiBaseFn>(lib->resolve("OrtGetApiBase"));
    if (!base) {
        if (error) *error = file + " is no ONNX Runtime (OrtGetApiBase missing)";
        lib->unload();
        return std::nullopt;
    }
    const OrtApiBase* b = base();
    const void* table = b && b->GetApi ? b->GetApi(kApiVersion) : nullptr;
    const QString v = b && b->GetVersionString ? QString::fromUtf8(b->GetVersionString()) : QString();
    if (!table) {
        if (error) *error = QString("ONNX Runtime %1 is too old (1.11 or newer needed)").arg(v);
        lib->unload();
        return std::nullopt;
    }
    Loaded out;
    out.lib = std::move(lib);
    out.api.table = static_cast<const void* const*>(table);
    out.version = v;
    return out;
}

// The shared runtime: loaded once, never unloaded (sessions and the environment live until the program ends)
struct Runtime {
    QMutex mutex;
    bool tried = false;
    QStringList triedFiles; // the candidates of the last try (none loaded: tried again when they change, e.g. installed)
    std::optional<Loaded> loaded;
    QString path, error;
    OrtEnv* env = nullptr;
};

Runtime& runtime()
{
    static Runtime r;
    return r;
}

// Under the runtime's mutex
bool ensure(QString* error)
{
    Runtime& r = runtime();
    // Not loaded yet: tried again when the candidates changed (the extension installed while the program runs)
    if (r.tried && !r.loaded && candidates() != r.triedFiles) r.tried = false;
    if (!r.tried) {
        r.tried = true;
        r.triedFiles = candidates();
        QStringList errors;
        for (const QString& file : r.triedFiles) {
            QString e;
            auto l = load(file, &e);
            if (!l) {
                errors << e;
                continue;
            }
            OrtEnv* env = nullptr;
            if (!l->api.call<int, const char*, OrtEnv**>(kCreateEnv, &e, kLoggingWarning, "schneidi", &env)) {
                errors << e;
                continue;
            }
            r.loaded = std::move(l);
            r.env = env;
            r.path = file;
            break;
        }
        if (!r.loaded)
            r.error = errors.isEmpty() ? QString("ONNX Runtime is not installed (extension \"onnxruntime\")")
                                       : "ONNX Runtime not usable: " + errors.join("; ");
    }
    if (!r.loaded && error) *error = r.error;
    return bool(r.loaded);
}

} // namespace

QStringList candidates()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_ONNXRUNTIME");
    if (!own.isEmpty()) return {own};
    QStringList out;
    for (const QString& f : {Extensions::onnxRuntimeLibrary(), Extensions::piperOnnxRuntime()})
        if (!f.isEmpty()) out << f;
#ifndef Q_OS_WIN
    out << "onnxruntime"; // QLibrary adds lib…so / ….dll and searches the system's paths
#endif
    // (Windows: no bare name - Windows 11 ships an onnxruntime.dll of its own in System32, of an unknown version)
    return out;
}

bool available(QString* error)
{
    QMutexLocker lock(&runtime().mutex);
    return ensure(error);
}

QString libraryPath()
{
    QMutexLocker lock(&runtime().mutex);
    ensure(nullptr);
    return runtime().path;
}

QString version()
{
    QMutexLocker lock(&runtime().mutex);
    return ensure(nullptr) ? runtime().loaded->version : QString();
}

bool probe(const QString& file, QString* error, QString* versionOut)
{
    auto l = load(file, error);
    if (!l) return false;
    if (versionOut) *versionOut = l->version;
    // Stays loaded (QLibrary's destructor does not unload): ONNX Runtime is not made for being unloaded again
    return true;
}

// ---------- Session ----------

struct Session::Impl {
    Api api;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::vector<Port> inputs, outputs;
    std::vector<QByteArray> inputNames, outputNames;

    ~Impl()
    {
        api.release(kReleaseMemoryInfo, memory);
        api.release(kReleaseSession, session);
    }

    // Name and shape of input/output i
    bool port(bool input, size_t i, Port* p, QByteArray* name, QString* error)
    {
        OrtAllocator* alloc = nullptr;
        if (!api.call<OrtAllocator**>(kGetAllocatorWithDefaultOptions, error, &alloc)) return false;
        char* n = nullptr;
        if (!api.call<const OrtSession*, size_t, OrtAllocator*, char**>(
                input ? kSessionGetInputName : kSessionGetOutputName, error, session, i, alloc, &n))
            return false;
        *name = QByteArray(n);
        api.call<OrtAllocator*, void*>(kAllocatorFree, nullptr, alloc, static_cast<void*>(n));
        p->name = QString::fromUtf8(*name);
        OrtTypeInfo* type = nullptr;
        if (!api.call<const OrtSession*, size_t, OrtTypeInfo**>(
                input ? kSessionGetInputTypeInfo : kSessionGetOutputTypeInfo, error, session, i, &type))
            return false;
        const OrtTensorTypeAndShapeInfo* info = nullptr; // owned by `type`
        bool ok = api.call<const OrtTypeInfo*, const OrtTensorTypeAndShapeInfo**>(kCastTypeInfoToTensorInfo, error,
                                                                                  type, &info);
        if (ok && info) {
            int element = 0;
            size_t rank = 0;
            ok = api.call<const OrtTensorTypeAndShapeInfo*, int*>(kGetTensorElementType, error, info, &element)
                 && api.call<const OrtTensorTypeAndShapeInfo*, size_t*>(kGetDimensionsCount, error, info, &rank);
            if (ok) {
                p->isFloat = element == kElementFloat;
                p->shape.assign(rank, 0);
                ok = rank == 0
                     || api.call<const OrtTensorTypeAndShapeInfo*, int64_t*, size_t>(kGetDimensions, error, info,
                                                                                     p->shape.data(), rank);
            }
        } else if (ok) { // not a tensor (a map, a sequence): unusable here
            p->isFloat = false;
        }
        api.release(kReleaseTypeInfo, type);
        return ok;
    }
};

Session::Session() : d(std::make_unique<Impl>()) {}
Session::~Session() = default;

std::unique_ptr<Session> Session::open(const QString& modelPath, QString* error)
{
    QFile f(modelPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "cannot read the model " + modelPath;
        return nullptr;
    }
    return fromBytes(f.readAll(), error);
}

std::unique_ptr<Session> Session::fromBytes(const QByteArray& model, QString* error)
{
    QMutexLocker lock(&runtime().mutex);
    if (!ensure(error)) return nullptr;
    const Api api = runtime().loaded->api;
    std::unique_ptr<Session> s(new Session);
    s->d->api = api;
    OrtSessionOptions* options = nullptr;
    if (!api.call<OrtSessionOptions**>(kCreateSessionOptions, error, &options)) return nullptr;
    // Gentle on the machine: half the cores at most (the analyses run beside playback and other work)
    const int threads = std::clamp(QThread::idealThreadCount() / 2, 1, 4);
    api.call<OrtSessionOptions*, int>(kSetIntraOpNumThreads, nullptr, options, threads);
    api.call<OrtSessionOptions*, int>(kSetInterOpNumThreads, nullptr, options, 1);
    const bool ok = api.call<const OrtEnv*, const void*, size_t, const OrtSessionOptions*, OrtSession**>(
        kCreateSessionFromArray, error, runtime().env, static_cast<const void*>(model.constData()),
        size_t(model.size()), options, &s->d->session);
    api.release(kReleaseSessionOptions, options);
    if (!ok) return nullptr;
    if (!api.call<int, int, OrtMemoryInfo**>(kCreateCpuMemoryInfo, error, kArenaAllocator, kMemTypeDefault,
                                             &s->d->memory))
        return nullptr;
    size_t nIn = 0, nOut = 0;
    if (!api.call<const OrtSession*, size_t*>(kSessionGetInputCount, error, s->d->session, &nIn)
        || !api.call<const OrtSession*, size_t*>(kSessionGetOutputCount, error, s->d->session, &nOut))
        return nullptr;
    for (size_t i = 0; i < nIn + nOut; ++i) {
        const bool input = i < nIn;
        Port p;
        QByteArray name;
        if (!s->d->port(input, input ? i : i - nIn, &p, &name, error)) return nullptr;
        (input ? s->d->inputs : s->d->outputs).push_back(p);
        (input ? s->d->inputNames : s->d->outputNames).push_back(name);
    }
    return s;
}

const std::vector<Port>& Session::inputs() const { return d->inputs; }
const std::vector<Port>& Session::outputs() const { return d->outputs; }

std::optional<std::vector<Tensor>> Session::run(const std::vector<Tensor>& in, QString* error)
{
    const Api& api = d->api;
    if (in.size() != d->inputs.size()) {
        if (error) *error = QString("the model takes %1 input(s), got %2").arg(d->inputs.size()).arg(in.size());
        return std::nullopt;
    }
    std::vector<OrtValue*> values(in.size(), nullptr), outs(d->outputs.size(), nullptr);
    const auto cleanUp = [&] {
        for (OrtValue* v : values) api.release(kReleaseValue, v);
        for (OrtValue* v : outs) api.release(kReleaseValue, v);
    };
    for (size_t i = 0; i < in.size(); ++i) {
        size_t count = 1;
        for (int64_t dim : in[i].shape) count *= size_t(std::max<int64_t>(0, dim));
        if (count != in[i].data.size()) {
            if (error) *error = QString("input %1: shape and data size differ").arg(i);
            cleanUp();
            return std::nullopt;
        }
        // ONNX Runtime reads the caller's buffer (no copy) and never writes into an input
        if (!api.call<const OrtMemoryInfo*, void*, size_t, const int64_t*, size_t, int, OrtValue**>(
                kCreateTensorWithDataAsOrtValue, error, d->memory, const_cast<float*>(in[i].data.data()),
                in[i].data.size() * sizeof(float), in[i].shape.data(), in[i].shape.size(), kElementFloat,
                &values[i])) {
            cleanUp();
            return std::nullopt;
        }
    }
    std::vector<const char*> inNames, outNames;
    for (const QByteArray& n : d->inputNames) inNames.push_back(n.constData());
    for (const QByteArray& n : d->outputNames) outNames.push_back(n.constData());
    std::vector<const OrtValue*> inValues(values.begin(), values.end());
    if (!api.call<OrtSession*, const OrtRunOptions*, const char* const*, const OrtValue* const*, size_t,
                  const char* const*, size_t, OrtValue**>(kRun, error, d->session, nullptr, inNames.data(),
                                                          inValues.data(), inValues.size(), outNames.data(),
                                                          outNames.size(), outs.data())) {
        cleanUp();
        return std::nullopt;
    }
    std::vector<Tensor> result(outs.size());
    for (size_t i = 0; i < outs.size(); ++i) {
        if (!d->outputs[i].isFloat) continue; // left empty
        OrtTensorTypeAndShapeInfo* info = nullptr;
        size_t rank = 0, count = 0;
        bool ok = api.call<const OrtValue*, OrtTensorTypeAndShapeInfo**>(kGetTensorTypeAndShape, error, outs[i], &info)
                  && api.call<const OrtTensorTypeAndShapeInfo*, size_t*>(kGetDimensionsCount, error, info, &rank);
        if (ok) {
            result[i].shape.assign(rank, 0);
            ok = (rank == 0
                  || api.call<const OrtTensorTypeAndShapeInfo*, int64_t*, size_t>(kGetDimensions, error, info,
                                                                                  result[i].shape.data(), rank))
                 && api.call<const OrtTensorTypeAndShapeInfo*, size_t*>(kGetTensorShapeElementCount, error, info,
                                                                       &count);
        }
        api.release(kReleaseTensorTypeAndShapeInfo, info);
        void* data = nullptr;
        if (ok) ok = api.call<OrtValue*, void**>(kGetTensorMutableData, error, outs[i], &data);
        if (!ok) {
            cleanUp();
            return std::nullopt;
        }
        const float* f = static_cast<const float*>(data);
        result[i].data.assign(f, f + count);
    }
    cleanUp();
    return result;
}

} // namespace Inference
