// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Faces in a picture with YuNet 2023mar (opencv_zoo, MIT) through ONNX Runtime (engine/Inference), when the "faces"
// extension (Extensions::faceModel) and the runtime are there. No fallback: without them there are no faces and an
// error (callers keep their own hint, e.g. the skin share of engine/MediaIndex).
//
// The model takes one picture of 640 x 640 (1 x 3 x H x W, BGR, 0..255, no scaling; the 2023mar file has that size
// fixed): the picture is scaled so its longer side is 640 and the rest is black (below or right of it), as OpenCV's
// FaceDetectorYN pads. It answers per cell of three grids (strides 8, 16, 32): cls and obj (score = sqrt(cls * obj)),
// bbox (centre offset in cells, log of size in cells) and kps (five landmarks, unused). Decoded as FaceDetectorYN does,
// then non-maximum suppression.

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Inference {
class Session;
}

namespace FaceDetect {

inline constexpr int kInput = 640;          // the model's picture size (both sides)
inline constexpr float kMinScore = 0.6f;    // faces below are dropped (FaceDetectorYN's samples: 0.6 .. 0.9)
inline constexpr float kNmsOverlap = 0.3f;  // boxes overlapping more (IoU) than this with a better one go
inline const int kStrides[3] = {8, 16, 32};

struct Face {
    // Box, top-left origin: decode() gives model input pixels, Detector fractions 0..1 of the picture
    double x = 0, y = 0, w = 0, h = 0;
    double score = 0; // 0..1
};

// The outputs of one grid (stride): per cell row by row cls and obj (1 value), bbox (4 values)
struct Level {
    int stride = 8;
    std::vector<float> cls, obj, bbox;
};

// Pure (tests): the faces of the raw outputs of an input of inputWidth x inputHeight (multiples of 32), score >=
// minScore, before suppression, in input pixels
std::vector<Face> decode(const std::vector<Level>& levels, int inputWidth, int inputHeight, float minScore = kMinScore);
// Intersection over union of two boxes
double overlap(const Face& a, const Face& b);
// Best first; a face overlapping a better one by more than maxOverlap goes
std::vector<Face> suppress(std::vector<Face> faces, float maxOverlap = kNmsOverlap);

// The model is installed and the runtime loads; why not
bool available(QString* why = nullptr);
// How faces are found right now, as a cache key part: "" (not at all) or "yunet|<model path>|<size>|<mtime>"
// (remembered per process and model file). A cache of results with faces keys on it: installing the model measures anew.
QString way();
// The same for a given model file (empty = none); pure, for tests
QString wayKey(const QString& model);

// One model session; used by one thread at a time
class Detector {
public:
    // The installed model (Extensions::faceModel), or a given file; nullptr with *error when missing or unusable
    static std::unique_ptr<Detector> open(QString* error = nullptr);
    static std::unique_ptr<Detector> openModel(const QString& model, QString* error = nullptr);
    ~Detector();

    // Faces in a picture of packed 8-bit BGR (3 bytes per pixel, rows `stride` bytes apart), boxes as fractions of
    // the picture, best first. nullopt with *error when the model fails.
    std::optional<std::vector<Face>> detect(const uint8_t* bgr, int width, int height, int stride,
                                            QString* error = nullptr, float minScore = kMinScore);

private:
    Detector();
    std::unique_ptr<Inference::Session> m_session;
    // Output positions of cls/obj/bbox per stride (by name: cls_8 …; else the order of the 2023mar file)
    int m_cls[3] = {0, 1, 2}, m_obj[3] = {3, 4, 5}, m_bbox[3] = {6, 7, 8};
};

// 8-bit YUV 4:2:0 (planes and their line sizes; limited or full range, BT.601) to packed BGR, for detect()
std::vector<uint8_t> bgrFromYuv420(const uint8_t* const planes[3], const int linesize[3], int width, int height,
                                   bool fullRange);

} // namespace FaceDetect
