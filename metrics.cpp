```cpp
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <powrprof.h>

#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cwchar>

#include "metrics.h"
#include "overlay.h"
#include "logger.h"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")

// ============================================================
// PROCESSOR_POWER_INFORMATION
// Compatible con x86 y x64
// ============================================================

typedef struct _PROCESSOR_POWER_INFORMATION
{
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION;


// ============================================================
// ESTADO GLOBAL
// ============================================================

static std::atomic<bool> g_metricsRunning{ false };
static std::thread g_metricsThread;

static std::atomic<bool> g_shuttingDown{ false };
static HANDLE g_wakeEvent = nullptr;


// ============================================================
// CPU %
// ============================================================

struct CpuCounter
{
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool ok = false;

    void Init()
    {
        PDH_STATUS st = PdhOpenQueryW(
            nullptr,
            0,
            &query
        );

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhOpenQueryW (CPU) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );
            return;
        }

        st = PdhAddEnglishCounterW(
            query,
            L"\\Processor(_Total)\\% Processor Time",
            0,
            &counter
        );

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhAddEnglishCounterW (CPU%%) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );

            PdhCloseQuery(query);
            query = nullptr;
            return;
        }

        // Primera lectura necesaria para inicializar PDH.
        PdhCollectQueryData(query);

        ok = true;
    }

    float Sample()
    {
        if (!ok)
            return 0.0f;

        PDH_STATUS st = PdhCollectQueryData(query);

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhCollectQueryData (CPU) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );
            return 0.0f;
        }

        PDH_FMT_COUNTERVALUE val{};

        st = PdhGetFormattedCounterValue(
            counter,
            PDH_FMT_DOUBLE,
            nullptr,
            &val
        );

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhGetFormattedCounterValue (CPU%%) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );
            return 0.0f;
        }

        return static_cast<float>(val.doubleValue);
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


// ============================================================
// FRECUENCIA ACTUAL DE CPU
// ============================================================

static float ReadCpuCurrentMHz()
{
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);

    DWORD numCores = sysInfo.dwNumberOfProcessors;

    if (numCores == 0)
        numCores = 1;

    std::vector<PROCESSOR_POWER_INFORMATION> info(numCores);

    const ULONG bufferSize =
        static_cast<ULONG>(
            sizeof(PROCESSOR_POWER_INFORMATION) * numCores
        );

    // CallNtPowerInformation devuelve LONG.
    // No utilizamos NTSTATUS.
    LONG status = CallNtPowerInformation(
        ProcessorInformation,
        nullptr,
        0,
        info.data(),
        bufferSize
    );

    if (status != 0)
    {
        LOG_ERROR(
            "CallNtPowerInformation fallo: 0x%08lX",
            static_cast<unsigned long>(status)
        );

        return 0.0f;
    }

    unsigned long long sum = 0;

    for (const auto& p : info)
    {
        sum += static_cast<unsigned long long>(
            p.CurrentMhz
        );
    }

    return static_cast<float>(sum) /
           static_cast<float>(numCores);
}


// ============================================================
// GPU %
// ============================================================

struct GpuCounter
{
    PDH_HQUERY query = nullptr;
    std::vector<PDH_HCOUNTER> counters;
    bool ok = false;

    void Init()
    {
        PDH_STATUS st = PdhOpenQueryW(
            nullptr,
            0,
            &query
        );

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhOpenQueryW (GPU) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );
            return;
        }

        DWORD pathListSize = 0;

        st = PdhExpandWildCardPathW(
            nullptr,
            L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
            nullptr,
            &pathListSize,
            0
        );

        if (pathListSize == 0)
        {
            LOG_ERROR(
                "No se encontraron instancias de GPU Engine(*engtype_3D)"
            );

            PdhCloseQuery(query);
            query = nullptr;
            return;
        }

        std::vector<wchar_t> pathList(pathListSize);

        st = PdhExpandWildCardPathW(
            nullptr,
            L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
            pathList.data(),
            &pathListSize,
            0
        );

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhExpandWildCardPathW (GPU 3D) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );

            PdhCloseQuery(query);
            query = nullptr;
            return;
        }

        for (wchar_t* p = pathList.data();
             *p;
             p += wcslen(p) + 1)
        {
            PDH_HCOUNTER counter = nullptr;

            if (PdhAddEnglishCounterW(
                    query,
                    p,
                    0,
                    &counter) == ERROR_SUCCESS)
            {
                counters.push_back(counter);
            }
        }

        if (counters.empty())
        {
            LOG_ERROR(
                "No se pudo agregar ningun contador GPU Engine(*engtype_3D)"
            );

            PdhCloseQuery(query);
            query = nullptr;
            return;
        }

        PdhCollectQueryData(query);

        ok = true;

        LOG_INFO(
            "GpuCounter: %zu instancias engtype_3D monitorizadas",
            counters.size()
        );
    }

    float Sample()
    {
        if (!ok)
            return 0.0f;

        PDH_STATUS st = PdhCollectQueryData(query);

        if (st != ERROR_SUCCESS)
        {
            LOG_ERROR(
                "PdhCollectQueryData (GPU) fallo: 0x%08X",
                static_cast<unsigned int>(st)
            );

            return 0.0f;
        }

        double total = 0.0;

        for (PDH_HCOUNTER counter : counters)
        {
            PDH_FMT_COUNTERVALUE val{};

            if (PdhGetFormattedCounterValue(
                    counter,
                    PDH_FMT_DOUBLE,
                    nullptr,
                    &val) == ERROR_SUCCESS)
            {
                total += val.doubleValue;
            }
        }

        if (total < 0.0)
            total = 0.0;

        if (total > 100.0)
            total = 100.0;

        return static_cast<float>(total);
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


// ============================================================
// VRAM
// ============================================================

struct VramCounter
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    bool ok = false;

    void Init()
    {
        HRESULT hr = CreateDXGIFactory1(
            IID_PPV_ARGS(&factory)
        );

        if (FAILED(hr))
        {
            LOG_ERROR(
                "CreateDXGIFactory1 fallo: 0x%08X",
                static_cast<unsigned int>(hr)
            );
            return;
        }

        IDXGIAdapter1* adapter1 = nullptr;

        hr = factory->EnumAdapters1(
            0,
            &adapter1
        );

        if (FAILED(hr))
        {
            LOG_ERROR(
                "EnumAdapters1(0) fallo: 0x%08X",
                static_cast<unsigned int>(hr)
            );
            return;
        }

        hr = adapter1->QueryInterface(
            IID_PPV_ARGS(&adapter3)
        );

        adapter1->Release();

        if (FAILED(hr))
        {
            LOG_ERROR(
                "QueryInterface IDXGIAdapter3 fallo: 0x%08X",
                static_cast<unsigned int>(hr)
            );
            return;
        }

        ok = true;
    }

    bool Sample(
        float& usedGB,
        float& budgetGB
    )
    {
        usedGB = 0.0f;
        budgetGB = 0.0f;

        if (!ok)
            return false;

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};

        HRESULT hr = adapter3->QueryVideoMemoryInfo(
            0,
            DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
            &info
        );

        if (FAILED(hr))
        {
            LOG_ERROR(
                "QueryVideoMemoryInfo fallo: 0x%08X",
                static_cast<unsigned int>(hr)
            );

            return false;
        }

        constexpr double GB =
            1024.0 * 1024.0 * 1024.0;

        usedGB = static_cast<float>(
            static_cast<double>(info.CurrentUsage) / GB
        );

        budgetGB = static_cast<float>(
            static_cast<double>(info.Budget) / GB
        );

        return true;
    }

    void Shutdown()
    {
        if (adapter3)
        {
            adapter3->Release();
            adapter3 = nullptr;
        }

        if (factory)
        {
            factory->Release();
            factory = nullptr;
        }

        ok = false;
    }
};


// ============================================================
// HILO DE MÉTRICAS
// ============================================================

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

    while (!g_shuttingDown.load())
    {
        // -----------------------------
        // CPU
        // -----------------------------

        g_overlay.cpuPercent = cpu.Sample();
        g_overlay.cpuMHz = ReadCpuCurrentMHz();


        // -----------------------------
        // RAM
        // -----------------------------

        if (GlobalMemoryStatusEx(&memStatus))
        {
            constexpr double GB =
                1024.0 * 1024.0 * 1024.0;

            g_overlay.ramUsedGB =
                static_cast<float>(
                    static_cast<double>(
                        memStatus.ullTotalPhys -
                        memStatus.ullAvailPhys
                    ) / GB
                );

            g_overlay.ramTotalGB =
                static_cast<float>(
                    static_cast<double>(
                        memStatus.ullTotalPhys
                    ) / GB
                );
        }
        else
        {
            LOG_ERROR(
                "GlobalMemoryStatusEx fallo: %lu",
                static_cast<unsigned long>(
                    GetLastError()
                )
            );
        }


        // -----------------------------
        // GPU
        // -----------------------------

        g_overlay.gpuPercent = gpu.Sample();


        // -----------------------------
        // VRAM
        // -----------------------------

        float vUsed = 0.0f;
        float vBudget = 0.0f;

        if (vram.Sample(vUsed, vBudget))
        {
            g_overlay.vramUsedGB = vUsed;
            g_overlay.vramTotalGB = vBudget;
        }


        // -----------------------------
        // Espera
        // -----------------------------

        if (g_wakeEvent)
        {
            WaitForSingleObject(
                g_wakeEvent,
                static_cast<DWORD>(intervalMs)
            );
        }
        else
        {
            Sleep(
                static_cast<DWORD>(intervalMs)
            );
        }
    }

    cpu.Shutdown();
    gpu.Shutdown();
    vram.Shutdown();
}


// ============================================================
// INICIAR HILO
// ============================================================

void Metrics_StartThread(int intervalMs)
{
    if (g_metricsRunning.load())
        return;

    if (intervalMs < 1)
        intervalMs = 1;

    g_shuttingDown.store(false);

    if (!g_wakeEvent)
    {
        g_wakeEvent = CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            nullptr
        );
    }

    if (!g_wakeEvent)
    {
        LOG_ERROR(
            "CreateEventW fallo: %lu",
            static_cast<unsigned long>(
                GetLastError()
            )
        );

        return;
    }

    g_metricsRunning.store(true);

    g_metricsThread =
        std::thread(
            MetricsLoop,
            intervalMs
        );

    LOG_INFO(
        "Hilo de metricas iniciado (intervalo=%dms)",
        intervalMs
    );
}


// ============================================================
// DETENER HILO
// ============================================================

void Metrics_StopThread()
{
    if (!g_metricsRunning.load())
        return;

    g_shuttingDown.store(true);

    if (g_wakeEvent)
    {
        SetEvent(g_wakeEvent);
    }

    if (g_metricsThread.joinable())
    {
        g_metricsThread.join();
    }

    if (g_wakeEvent)
    {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }

    g_metricsRunning.store(false);

    LOG_INFO(
        "Hilo de metricas detenido"
    );
}
```

