// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which graphics card schneidi uses on Windows (Direct3D 11): the picture engine, the viewer and the video decoder
// (D3D11VA) all take the same one. Without a choice, Qt and FFmpeg take DXGI adapter 0, which on many laptops and
// desktops with the monitor on the mainboard is the integrated chip. Elsewhere (OpenGL/VAAPI/Metal) nothing changes:
// chosen() is empty there.

#include <QString>
#include <QVector>
#include <cstdint>

namespace GpuAdapter {

struct Adapter {
    QString name;               // DXGI description, e.g. "NVIDIA GeForce GTX 1050 Ti"
    std::uint32_t vendorId = 0; // PCI vendor (0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x1414 Microsoft)
    std::uint64_t dedicatedMemory = 0; // bytes of video memory of its own
    bool software = false;      // WARP or the "Microsoft Basic Render Driver": never chosen automatically
    std::int64_t luid = 0;      // LowPart | HighPart << 32
};

struct Choice {
    int index = -1;  // DXGI adapter index (IDXGIFactory1::EnumAdapters1 order), -1 = leave it to Qt/FFmpeg
    QString reason;  // for the log: "high performance", "SCHNEIDI_GPU_ADAPTER", …
};

// Pure choice (testable on every system). adapters: in DXGI enumeration order (index = position).
// highPerformance: adapter indices in IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) order, empty when
// that interface is missing (Windows before 10 1803). override: SCHNEIDI_GPU_ADAPTER (index or part of the name, any
// case), qtIndex: QT_D3D_ADAPTER_INDEX (index); both empty when not set. Order: override, qtIndex, the first hardware
// adapter of highPerformance, else the hardware adapter with the most video memory (first one on a tie).
// High performance overrides Windows' per-program graphics setting "Let Windows decide", but not an explicit "power
// saving" for schneidi: then highPerformance holds the minimum-power order (the Windows part reads the setting).
Choice choose(const QVector<Adapter>& adapters, const QVector<int>& highPerformance, const QString& override,
              const QString& qtIndex);

// Windows: the adapters, the choice (once, logged) and the chosen adapter's LUID; elsewhere empty / -1 / 0
QVector<Adapter> adapters();
const Choice& chosen();
std::int64_t chosenLuid();
// "GPU adapter: <index> <name> (<reason>, <n> adapters)" to the log (call after the log is installed). No-op outside
// Windows or when nothing was chosen.
void logChoice();
// "GPU adapter: <index> <name> (<reason>, <n> adapters)" to the log (call after the log is installed). No-op outside
// Windows or when nothing was chosen.
void logChoice();
// Before the application object: makes Qt's own D3D11 windows (the viewer widget's backing store) use the chosen
// adapter too, through QT_D3D_ADAPTER_INDEX (only when that is not set already). No-op outside Windows.
void applyToQt();

} // namespace GpuAdapter
