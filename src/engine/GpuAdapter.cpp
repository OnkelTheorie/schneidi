// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/GpuAdapter.h"

#include <QtGlobal>
#include <iterator>
#include <mutex>

#ifdef Q_OS_WIN
#include <dxgi1_6.h>
#endif

namespace GpuAdapter {

namespace {

bool usable(const QVector<Adapter>& adapters, int i)
{
    return i >= 0 && i < adapters.size() && !adapters[i].software;
}

} // namespace

Choice choose(const QVector<Adapter>& adapters, const QVector<int>& highPerformance, const QString& override,
              const QString& qtIndex)
{
    // The user's choice: an index (taken even for a software adapter: it is a deliberate test) or a name part
    const QString wanted = override.trimmed();
    if (!wanted.isEmpty()) {
        bool number = false;
        const int i = wanted.toInt(&number);
        if (number && i >= 0 && i < adapters.size()) return {i, QStringLiteral("SCHNEIDI_GPU_ADAPTER")};
        if (!number) {
            int match = -1;
            for (int a = 0; a < adapters.size(); ++a)
                if (adapters[a].name.contains(wanted, Qt::CaseInsensitive)
                    && (match < 0 || (adapters[match].software && !adapters[a].software)))
                    match = a;
            if (match >= 0) return {match, QStringLiteral("SCHNEIDI_GPU_ADAPTER")};
        }
    }
    if (!qtIndex.trimmed().isEmpty()) {
        bool number = false;
        const int i = qtIndex.trimmed().toInt(&number);
        if (number && i >= 0 && i < adapters.size()) return {i, QStringLiteral("QT_D3D_ADAPTER_INDEX")};
    }
    for (int i : highPerformance)
        if (usable(adapters, i)) return {i, QStringLiteral("high performance")};
    int best = -1;
    for (int i = 0; i < adapters.size(); ++i)
        if (usable(adapters, i) && (best < 0 || adapters[i].dedicatedMemory > adapters[best].dedicatedMemory)) best = i;
    if (best >= 0) return {best, QStringLiteral("most video memory")};
    return {};
}

#ifdef Q_OS_WIN

namespace {

std::int64_t luidOf(const LUID& l)
{
    return std::int64_t(std::uint64_t(l.LowPart) | (std::uint64_t(std::uint32_t(l.HighPart)) << 32));
}

struct Found {
    QVector<Adapter> adapters;
    QVector<int> highPerformance; // in the order of the preference below
    bool powerSaving = false;     // the user set "power saving" for this program in Windows' graphics settings
};

// Windows' per-program graphics setting (Settings > System > Display > Graphics): registry key
// HKCU/Software/Microsoft/DirectX/UserGpuPreferences, value = the program's full path, data "GpuPreference=1;"
// (1 power saving, 2 high performance, 0 or missing: Windows decides). Only an explicit "power saving" changes
// schneidi's choice.
bool userWantsPowerSaving()
{
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) return false;
    wchar_t data[256] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", exe, RRF_RT_REG_SZ, nullptr,
                     data, &size)
        != ERROR_SUCCESS)
        return false;
    return QString::fromWCharArray(data).remove(' ').contains(QLatin1String("GpuPreference=1;"));
}

Found enumerate()
{
    Found f;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return f;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            Adapter a;
            a.name = QString::fromWCharArray(d.Description);
            a.vendorId = d.VendorId;
            a.dedicatedMemory = d.DedicatedVideoMemory;
            // WARP has the software flag; the Basic Render Driver of a display-only adapter (VMs, no driver) does not,
            // but it is Microsoft's (0x1414) like WARP
            a.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414;
            a.luid = luidOf(d.AdapterLuid);
            f.adapters << a;
        } else {
            // Keep the list's positions equal to DXGI's indices (they go to QT_D3D_ADAPTER_INDEX and FFmpeg)
            Adapter unknown;
            unknown.name = QStringLiteral("(unknown adapter)");
            unknown.software = true; // never chosen automatically
            f.adapters << unknown;
        }
        adapter->Release();
    }
    f.powerSaving = userWantsPowerSaving();
    IDXGIFactory6* factory6 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6))) && factory6) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, f.powerSaving ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                                                  : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            __uuidof(IDXGIAdapter1),
                                                            reinterpret_cast<void**>(&adapter)))
                || !adapter)
                break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d)))
                for (int a = 0; a < f.adapters.size(); ++a)
                    if (f.adapters[a].luid == luidOf(d.AdapterLuid)) f.highPerformance << a;
            adapter->Release();
        }
        factory6->Release();
    }
    factory->Release();
    return f;
}

struct State {
    std::once_flag once;
    QVector<Adapter> adapters;
    Choice choice;
};

State& state()
{
    static State s;
    std::call_once(s.once, [] {
        const Found f = enumerate();
        s.adapters = f.adapters;
        s.choice = choose(f.adapters, f.highPerformance, qEnvironmentVariable("SCHNEIDI_GPU_ADAPTER"),
                          qEnvironmentVariable("QT_D3D_ADAPTER_INDEX"));
        if (f.powerSaving && s.choice.reason == QLatin1String("high performance"))
            s.choice.reason = QStringLiteral("power saving, Windows graphics setting");
    });
    return s;
}

} // namespace

QVector<Adapter> adapters()
{
    return state().adapters;
}

const Choice& chosen()
{
    return state().choice;
}

std::int64_t chosenLuid()
{
    const State& s = state();
    return s.choice.index >= 0 ? s.adapters[s.choice.index].luid : 0;
}

void logChoice()
{
    const State& s = state();
    if (s.choice.index >= 0)
        qInfo("GPU adapter: %d %s (%s, %d adapters)", s.choice.index, qPrintable(s.adapters[s.choice.index].name),
              qPrintable(s.choice.reason), int(s.adapters.size()));
}

void applyToQt()
{
    if (!qEnvironmentVariableIsEmpty("QT_D3D_ADAPTER_INDEX")) return; // the user's (or a test's) choice stays
    if (const int i = chosen().index; i >= 0) qputenv("QT_D3D_ADAPTER_INDEX", QByteArray::number(i));
}

#else

QVector<Adapter> adapters()
{
    return {};
}

const Choice& chosen()
{
    static const Choice none;
    return none;
}

std::int64_t chosenLuid()
{
    return 0;
}

void logChoice() {}

void applyToQt() {}

#endif

} // namespace GpuAdapter
