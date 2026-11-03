// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Local neural networks through ONNX Runtime (MIT), an optional extension: schneidi never links it. The library is
// loaded at run time with QLibrary (one symbol, OrtGetApiBase, then the C API's function table), so the program
// starts and works the same without it; everything that uses a model has a fallback (engine/AudioEvents: heuristics).
//
// Where the library comes from, first found wins: SCHNEIDI_ONNXRUNTIME (a file; only that one is tried, also for
// tests), the "onnxruntime" extension (Extensions::onnxRuntimeLibrary), the copy inside the Piper extension (piper
// ships its own onnxruntime), the system's ("onnxruntime" on the library path).
//
// Models are read into memory and handed over as bytes (no wide-character path API on Windows). Float tensors only
// (what YAMNet and YuNet take). A Session is used by one thread at a time; several sessions may run in parallel.

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Inference {

// The runtime is there and works (loaded on the first call, then remembered). error: why not
bool available(QString* error = nullptr);
// The library file that was loaded ("" = none) and its version ("1.21.0")
QString libraryPath();
QString version();
// The candidates in search order (for messages and tests)
QStringList candidates();

// Tries one library file without touching the shared runtime: what available() would say about it. For tests and
// the extension check after an install (a broken download, a file that is no ONNX Runtime).
bool probe(const QString& file, QString* error = nullptr, QString* version = nullptr);

struct Tensor {
    std::vector<int64_t> shape; // -1 = dynamic (in a model's description)
    std::vector<float> data;
};

struct Port {
    QString name;
    std::vector<int64_t> shape;
    bool isFloat = true;
};

class Session {
public:
    // A model file (.onnx); nullptr with *error when the runtime is missing or the model cannot be loaded
    static std::unique_ptr<Session> open(const QString& modelPath, QString* error = nullptr);
    // A model in memory (tests build tiny ones)
    static std::unique_ptr<Session> fromBytes(const QByteArray& model, QString* error = nullptr);
    ~Session();

    const std::vector<Port>& inputs() const;
    const std::vector<Port>& outputs() const;

    // One input tensor per model input, in order; all outputs in order. nullopt with *error on failure.
    std::optional<std::vector<Tensor>> run(const std::vector<Tensor>& in, QString* error = nullptr);

private:
    Session();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Inference
