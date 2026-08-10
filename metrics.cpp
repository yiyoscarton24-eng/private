#include "metrics.h"
#include "overlay.h"
#include "logger.h"

#include <windows.h>
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

// Estructura no expuesta en headers publicos de Windows SDK para
// ProcessorInformation; layout estable desde XP, usado ampliamente por
// herramientas de monitorizacion (misma que usa Process Explorer / HWiNFO).
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

// Senal de apagado + evento sobre el que se espera en vez de dormir un
// intervalo fijo: Metrics_StopThread() lo activa para despertar el hilo
// de inmediato en lugar de esperar hasta 500ms a que expire el sleep.
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

    // Cierra el handle de consulta PDH. Sin esto, cada PdhOpenQueryW()
    // (uno por Metrics_StartThread()) fugaba el handle correspondiente
    // durante toda la vida del proceso.
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

// CPU MHz dinamico real: CallNtPowerInformation devuelve la frecuencia
// ACTUAL de cada nucleo logico (no el clock base fijo del registro).
// Se promedia entre nucleos para un numero representativo en el overlay.
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
// GPU %  -- SOLO motor 3D (engtype_3D). Sumar todas las instancias
// (Copy, VideoDecode, Compute, etc.) sobreestima el uso real de
// renderizado; engtype_3D es el equivalente exacto al "GPU" que muestra
// el Administrador de Tareas en la pestana de Rendimiento.
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
        // Varias instancias 3D (una por proceso que usa el motor) pueden
        // sumar mas de 100% en momentos de contencion; se limita al rango
        // valido para el overlay.
        if (total > 100.0) total = 100.0;
        return (float)total;
    }

    // Cierra el handle de consulta PDH (libera tambien todos los
    // contadores engtype_3D agregados a esa query). Mismo problema que
    // CpuCounter::Shutdown(): sin esto el handle quedaba fugado.
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
// VRAM -- factory y adapter se resuelven UNA sola vez en Init() y se
// reutilizan en cada Sample(). Antes, SampleVram() llamaba a
// CreateDXGIFactory1 + EnumAdapters1 + QueryInterface(IDXGIAdapter3) en
// cada muestreo (cada 500ms), es decir, para siempre mientras el overlay
// estuviera activo: coste de CPU innecesario y churn de referencias COM
// repetido sin ninguna necesidad, ya que el adaptador no cambia en
// caliente durante la vida normal del proceso.
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
            LOG_ERROR("QueryInterface IDXGIAdapter3 fallo: 0x%08X (GPU sin soporte DXGI 1.4?)", hr);
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

        // Antes: std::this_thread::sleep_for(...) monolitico, que
        // bloqueaba el hilo hasta intervalMs completos sin importar que
        // g_shuttingDown se activara a mitad de la espera, retrasando el
        // apagado (Metrics_StopThread hace join()) hasta 500ms. Ahora se
        // espera sobre un evento que StopThread() señala para despertar
        // de inmediato; si nadie lo señala, el timeout actua igual que
        // el sleep_for original.
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
        g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset

    g_metricsRunning = true;
    g_metricsThread = std::thread(MetricsLoop, intervalMs);
    LOG_INFO("Hilo de metricas iniciado (intervalo=%dms)", intervalMs);
}

void Metrics_StopThread()
{
    if (!g_metricsRunning) return;

    g_shuttingDown = true;
    if (g_wakeEvent)
        SetEvent(g_wakeEvent);

    if (g_metricsThread.joinable())
        g_metricsThread.join();

    g_metricsRunning = false;
    LOG_INFO("Hilo de metricas detenido");
}
