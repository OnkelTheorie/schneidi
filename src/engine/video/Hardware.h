// SPDX-License-Identifier: AGPL-3.0-only
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

// Is a device for this codec open and working already (deviceFor succeeded before)? Never opens one (opening CUDA
// takes 100+ ms: not for estimates on the GUI thread). Thread-safe.
bool deviceReady(const AVCodec* codec);

// Opens the device the readers will use (the first that works, as deviceFor), in the background: CUDA takes 100-250 ms
// to open, which the first picture of a project otherwise waits for (Windows' D3D11VA opens in a few). Nothing when
// hardware decoding is off. A deviceFor() meanwhile waits for it instead of opening a second one. Thread-safe, once.
// The app calls it when there is video to show: at once when started with a project or media (beside the window's
// setup), when a project or media is opened, else a while after the window settled (a device costs 100-300 MB of video
// memory and wakes a laptop's graphics card).
void warmUp();

// The setting (Wiedergabe › Grafikkarte zum Dekodieren nutzen); the environment variable wins
bool enabled();
void setEnabled(bool on);

} // namespace Video::Hardware
