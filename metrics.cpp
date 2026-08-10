#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winternl.h>

#include "metrics.h"
#include "overlay.h"
#include "logger.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <powrprof.h>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <numeric>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")

// Estructura no expuesta en headers publicos de Windows SDK para ProcessorInformation
typedef struct _PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION;

static std::atomic<bool> g_metricsRunning{ false };
static std::thread g_metricsThread;

static std::atomic<bool> g_shuttingDown{ false };
static HANDLE g_wakeEvent = nullptr;

// ---------------------------------------------------------------------
// CPU %
// ---------------------------------------------------------------------
struct CpuCounter {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool ok = false;

    void Init()
    {
        PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &query);
        if (st != ERROR_SUCCESS) { LOG_ERROR("PdhOpenQueryW (CPU) fallo: 0x%08X", st); return; }

        st = PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &counter);
        if (st != ERROR_SUCCESS) { LOG_ERROR("PdhAddEnglishCounterW (CPU%%) fallo: 0x%08X", st); return; }

        PdhCollectQueryData(query);
        ok = true;
    }

    float Sample()
    {
        if (!ok) return 0.0f;
        PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE val{};
        PDH_STATUS st = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &val);
        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR("PdhGetFormattedCounterValue (CPU%%) fallo: 0x%08X", st);
            return 0.0f;
        }
        return (float)val.doubleValue;
    }

    void Shutdown()
    {
        if (query)
        {
            PdhCloseQuery(query);
            query = nullptr;
        }
        counter = nullptr;
        ok = false;
    }
};

// Frecuencia actual de CPU
static float ReadCpuCurrentMHz()
{
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;
    if (numCores == 0) numCores = 1;

    std::vector<PROCESSOR_POWER_INFORMATION> info(numCores);
    NTSTATUS status = CallNtPowerInformation(
        ProcessorInformation, nullptr, 0,
        info.data(), (ULONG)(sizeof(PROCESSOR_POWER_INFORMATION) * numCores));

    if (status != 0 /* STATUS_SUCCESS */)
    {
        LOG_ERROR("CallNtPowerInformation fallo: 0x%08X", (unsigned)status);
        return 0.0f;
    }

    unsigned long long sum = 0;
    for (auto& p : info)
        sum += p.CurrentMhz;

    return (float)sum / (float)numCores;
}

// ---------------------------------------------------------------------
// GPU % (Motor 3D)
// ---------------------------------------------------------------------
struct GpuCounter {
    PDH_HQUERY query = nullptr;
    std::vector<PDH_HCOUNTER> counters;
    bool ok = false;

    void Init()
    {
        PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &query);
        if (st != ERROR_SUCCESS) { LOG_ERROR("PdhOpenQueryW (GPU) fallo: 0x%08X", st); return; }

        DWORD pathListSize = 0;
        PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                nullptr, &pathListSize, 0);
        if (pathListSize == 0)
        {
            LOG_ERROR("No se encontraron instancias de GPU Engine(*engtype_3D)");
            return;
        }

        std::vector<wchar_t> pathList(pathListSize);
        st = PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                    pathList.data(), &pathListSize, 0);
        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR("PdhExpandWildCardPathW (GPU 3D) fallo: 0x%08X", st);
            return;
        }

        for (wchar_t* p = pathList.data(); *p; p += wcslen(p) + 1)
        {
            PDH_HCOUNTER c;
            if (PdhAddEnglishCounterW(query, p, 0, &c) == ERROR_SUCCESS)
                counters.push_back(c);
        }

        if (counters.empty())
        {
            LOG_ERROR("No se pudo agregar ningun contador GPU Engine(*engtype_3D)");
            return;
        }

        PdhCollectQueryData(query);
        ok = true;
        LOG_INFO("GpuCounter: %zu instancias engtype_3D monitorizadas", counters.size());
    }

    float Sample()
    {
        if (!ok) return 0.0f;
        PDH_STATUS st = PdhCollectQueryData(query);
        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR("PdhCollectQueryData (GPU) fallo: 0x%08X", st);
            return 0.0f;
        }

        double total = 0.0;
        for (auto c : counters)
        {
            PDH_FMT_COUNTERVALUE val{};
            if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS)
                total += val.doubleValue;
        }

        if (total > 100.0) total = 100.0;
        return (float)total;
    }

    void Shutdown()
    {
        if (query)
        {
            PdhCloseQuery(query);
            query = nullptr;
        }
        counters.clear();
        ok = false;
    }
};

// ---------------------------------------------------------------------
// VRAM
// ---------------------------------------------------------------------
struct VramCounter {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    bool ok = false;

    void Init()
    {
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { LOG_ERROR("CreateDXGIFactory1 fallo: 0x%08X", hr); return; }

        IDXGIAdapter1* adapter1 = nullptr;
        hr = factory->EnumAdapters1(0, &adapter1);
        if (FAILED(hr)) { LOG_ERROR("EnumAdapters1(0) fallo: 0x%08X", hr); return; }

        hr = adapter1->QueryInterface(IID_PPV_ARGS(&adapter3));
        adapter1->Release();
        if (FAILED(hr))
        {
            LOG_ERROR("QueryInterface IDXGIAdapter3 fallo: 0x%08X", hr);
            return;
        }

        ok = true;
    }

    bool Sample(float& usedGB, float& budgetGB)
    {
        usedGB = budgetGB = 0.0f;
        if (!ok) return false;

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        HRESULT hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        if (FAILED(hr))
        {
            LOG_ERROR("QueryVideoMemoryInfo fallo: 0x%08X", hr);
            return false;
        }

        usedGB = (float)(info.CurrentUsage / (1024.0 * 1024.0 * 1024.0));
        budgetGB = (float)(info.Budget / (1024.0 * 1024.0 * 1024.0));
        return true;
    }

    void Shutdown()
    {
        if (adapter3) { adapter3->Release(); adapter3 = nullptr; }
        if (factory) { factory->Release(); factory = nullptr; }
        ok = false;
    }
};

static void MetricsLoop(int intervalMs)
{
    CpuCounter cpu;
    cpu.Init();
    GpuCounter gpu;
    gpu.Init();
    VramCounter vram;
    vram.Init();

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);

    while (!g_shuttingDown)
    {
        g_overlay.cpuPercent = cpu.Sample();
        g_overlay.cpuMHz = ReadCpuCurrentMHz();

        if (GlobalMemoryStatusEx(&memStatus))
        {
            g_overlay.ramUsedGB = (float)((memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
            g_overlay.ramTotalGB = (float)(memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        }
        else
        {
            LOG_ERROR("GlobalMemoryStatusEx fallo: %lu", GetLastError());
        }

        g_overlay.gpuPercent = gpu.Sample();

        float vUsed, vBudget;
        if (vram.Sample(vUsed, vBudget))
        {
            g_overlay.vramUsedGB = vUsed;
            g_overlay.vramTotalGB = vBudget;
        }

        WaitForSingleObject(g_wakeEvent, (DWORD)intervalMs);
    }

    cpu.Shutdown();
    gpu.Shutdown();
    vram.Shutdown();
}

void Metrics_StartThread(int intervalMs)
{
    if (g_metricsRunning) return;

    g_shuttingDown = false;
    if (!g_wakeEvent)
        g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    g_metricsRunning = true;
    g_metricsThread = std::thread(MetricsLoop, intervalMs);
    LOG_INFO("Hilo de metricas iniciado (intervalo=%dms)", intervalMs);
}

void Metrics_StopThread()
{
    if (!g_metricsRunning) return;

    g_shuttingDown = true;
    if (g_wakeEvent)
    {
        SetEvent(g_wakeEvent);
    }

    if (g_metricsThread.joinable())
        g_metricsThread.join();

    if (g_wakeEvent)
    {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }

    g_metricsRunning = false;
    LOG_INFO("Hilo de metricas detenido");
}
