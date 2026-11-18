// SPDX-License-Identifier: AGPL-3.0-only
// Choosing the graphics card on Windows (GpuAdapter::choose): pure logic, runs on every system
#include "check.h"

#include "engine/GpuAdapter.h"

#include <QCoreApplication>

namespace {

using GpuAdapter::Adapter;
using GpuAdapter::choose;

constexpr std::uint64_t GB = 1024ull * 1024 * 1024;

Adapter hw(const char* name, std::uint32_t vendor, std::uint64_t memory)
{
    Adapter a;
    a.name = name;
    a.vendorId = vendor;
    a.dedicatedMemory = memory;
    return a;
}

Adapter basicRender()
{
    Adapter a = hw("Microsoft Basic Render Driver", 0x1414, 0);
    a.software = true;
    return a;
}

void testAutomatic()
{
    // Laptop: Intel first, NVIDIA second; Windows 10 1803+ says which one is fast
    const QVector<Adapter> laptop{hw("Intel(R) UHD Graphics 620", 0x8086, 128 * 1024 * 1024),
                                  hw("NVIDIA GeForce MX150", 0x10de, 2 * GB), basicRender()};
    GpuAdapter::Choice c = choose(laptop, {1, 0, 2}, {}, {});
    CHECK_EQ(c.index, 1);
    CHECK_EQ(c.reason, QString("high performance"));
    // No IDXGIFactory6 (older Windows): the card with the most video memory
    CHECK_EQ(choose(laptop, {}, {}, {}).index, 1);
    // The preference list starting with a software adapter skips it
    CHECK_EQ(choose(laptop, {2, 0, 1}, {}, {}).index, 0);

    // The VM of 16.11.: NVIDIA first, then the display-only Basic Render Driver (flags 0, vendor 0x1414) and WARP
    const QVector<Adapter> vm{hw("NVIDIA GeForce GTX 1050 Ti", 0x10de, 4 * GB), basicRender(), basicRender()};
    CHECK_EQ(choose(vm, {0, 1, 2}, {}, {}).index, 0);
    CHECK_EQ(choose(vm, {}, {}, {}).index, 0);
    // Equal memory: the first one
    CHECK_EQ(choose({hw("A", 0x1002, GB), hw("B", 0x1002, GB)}, {}, {}, {}).index, 0);
    // Only software adapters, or none: leave it to Qt/FFmpeg
    CHECK_EQ(choose({basicRender()}, {0}, {}, {}).index, -1);
    CHECK_EQ(choose({}, {}, {}, {}).index, -1);
    // An adapter whose description could not be read stays in the list as a software placeholder: the indices stay
    // DXGI's (they go to QT_D3D_ADAPTER_INDEX and FFmpeg), and it is never chosen
    Adapter unknown;
    unknown.name = "(unknown adapter)";
    unknown.software = true;
    CHECK_EQ(choose({unknown, hw("NVIDIA GeForce RTX 3060", 0x10de, 12 * GB)}, {1}, {}, {}).index, 1);
    CHECK_EQ(choose({unknown, hw("NVIDIA GeForce RTX 3060", 0x10de, 12 * GB)}, {}, {}, {}).index, 1);
    // An out-of-range preference index (adapter vanished between the two enumerations) is ignored
    CHECK_EQ(choose(laptop, {7, 1}, {}, {}).index, 1);
}

void testOverrides()
{
    const QVector<Adapter> list{hw("Intel(R) UHD Graphics 620", 0x8086, 128 * 1024 * 1024),
                                hw("NVIDIA GeForce MX150", 0x10de, 2 * GB), basicRender()};
    // SCHNEIDI_GPU_ADAPTER: index (even a software one: a deliberate test) or part of the name, any case
    GpuAdapter::Choice c = choose(list, {1}, "0", {});
    CHECK(c.index == 0 && c.reason == "SCHNEIDI_GPU_ADAPTER");
    CHECK_EQ(choose(list, {1}, " 2 ", {}).index, 2);
    CHECK_EQ(choose(list, {1}, "intel", {}).index, 0);
    CHECK_EQ(choose(list, {0}, "GeForce", {}).index, 1);
    // A name matching a software and a hardware adapter: the hardware one
    CHECK_EQ(choose({basicRender(), hw("Microsoft Hyper-V Video", 0x1234, GB)}, {}, "microsoft", {}).index, 1);
    // Unknown name or index: the automatic choice
    CHECK_EQ(choose(list, {1}, "Radeon", {}).index, 1);
    CHECK_EQ(choose(list, {1}, "5", {}).index, 1);
    CHECK_EQ(choose(list, {1}, "-1", {}).index, 1);
    // QT_D3D_ADAPTER_INDEX is respected (after SCHNEIDI_GPU_ADAPTER)
    c = choose(list, {1}, {}, "0");
    CHECK(c.index == 0 && c.reason == "QT_D3D_ADAPTER_INDEX");
    CHECK_EQ(choose(list, {1}, "nvidia", "0").index, 1);
    CHECK_EQ(choose(list, {1}, {}, "9").index, 1);
    CHECK_EQ(choose(list, {1}, {}, "x").index, 1);
}

void testOtherSystems()
{
#ifndef Q_OS_WIN
    // Linux/macOS: nothing chosen, nothing set
    CHECK(GpuAdapter::adapters().isEmpty());
    CHECK_EQ(GpuAdapter::chosen().index, -1);
    CHECK_EQ(GpuAdapter::chosenLuid(), std::int64_t(0));
    const bool had = qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX");
    GpuAdapter::applyToQt();
    CHECK_EQ(qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX"), had);
#else
    // Windows: whatever is chosen is a valid index
    const int i = GpuAdapter::chosen().index;
    CHECK(i >= -1 && i < GpuAdapter::adapters().size());
    std::printf("GPU adapter here: %d\n", i);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    testAutomatic();
    testOverrides();
    testOtherSystems();
    return Check::result();
}
