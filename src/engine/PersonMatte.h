// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Person segmentation ("AI cutout", 0.19): how much of each pixel is a person, from a small ONNX model run through
// ONNX Runtime (engine/Inference), when the "cutout" extension (Extensions::cutoutModel) and the runtime are there.
// No fallback: without them there is no matte and an error that names the extension.
//
// The catalog's model is PP-HumanSeg 2023mar of opencv_zoo (Apache-2.0): one RGB picture 1 x 3 x 192 x 192, squashed
// to that size (no padding, as the zoo's demo does), values (v / 255 - 0.5) / 0.5; out 1 x 2 x 192 x 192 (background,
// person). Other portrait models fit as well: the input size and layout (NCHW or NHWC) are read from the model, the
// output may be one channel (a probability 0..1, or logits -> sigmoid) or two (softmax, channel 1 = person).
//
// input() and alpha() are pure (tests); a Segmenter is a model function plus its input spec, so tests hand in a
// stand-in function instead of a model file.

#include "engine/Inference.h"

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace PersonMatte {

struct Spec {
    int width = 192, height = 192;
    bool nhwc = false;              // 1 x H x W x 3 instead of 1 x 3 x H x W
    float mean = 0.5f, std = 0.5f;  // value = (v / 255 - mean) / std, the same for R, G, B
};

// The model's picture: img (any size and format) squashed to spec's size (bilinear), RGB in spec's layout
Inference::Tensor input(const QImage& img, const Spec& spec);

// The model's answer as a person plane 0..255 (row by row, *w x *h = the tensor's own size): one channel = a
// probability (all values in 0..1) or logits (sigmoid), two channels = softmax with channel 1 = person. Layout NCHW
// or NHWC (1 x H x W x C with C <= 2). Empty + *error for another shape.
std::vector<uint8_t> alpha(const Inference::Tensor& out, int* w, int* h, QString* error = nullptr);

// One model input tensor -> its first output tensor
using Model = std::function<std::optional<Inference::Tensor>(const Inference::Tensor& in, QString* error)>;

struct Segmenter {
    Spec spec;
    Model run;
    QString way; // what made it, as a cache key part ("ppseg|<path>|<size>|<mtime>", a stand-in's own name)
};

// The person plane of one picture (spec-sized, see alpha); empty + *error when the model fails
std::vector<uint8_t> segment(const Segmenter& s, const QImage& img, int* w, int* h, QString* error = nullptr);

// The model is installed and the runtime loads; why not
bool available(QString* why = nullptr);
// The installed model (or a test stand-in, setStandIn); nullopt + *error (names the extension "cutout") without it
std::optional<Segmenter> open(QString* error = nullptr);
// A given model file
std::optional<Segmenter> openModel(const QString& model, QString* error = nullptr);
// Tests: open() hands out this one instead of the installed model (nullopt = the installed model again)
void setStandIn(std::optional<Segmenter> s);
// The hint shown without the extension (translated)
QString needsExtension();

} // namespace PersonMatte
