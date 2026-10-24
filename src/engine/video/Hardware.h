#pragma once
// Hardware decoding for the own video reader (plan 0.11 point 7): one device per process, shared by all decoders.
// Linux: CUDA (NVIDIA, NVDEC), else VAAPI (AMD/Intel); Windows: D3D11VA (all vendors); macOS: VideoToolbox.
// SCHNEIDI_HWDEC=off|cuda|vaapi|d3d11va|videotoolbox picks one by hand (off = software only).

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

struct AVBufferRef;
struct AVCodec;

namespace Video::Hardware {

struct Device {
    AVBufferRef* context = nullptr; // own reference (av_buffer_unref when done); nullptr = none
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE; // the decoder's hardware picture format
};

// A device for this codec (opened on first use, shared); empty if none works or decoding is off.
// Thread-safe.
Device deviceFor(const AVCodec* codec);

// The setting (Wiedergabe › Grafikkarte zum Dekodieren nutzen); the environment variable wins
bool enabled();
void setEnabled(bool on);

} // namespace Video::Hardware
