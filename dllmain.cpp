#include <windows.h>

// Exportaciones Proxy para version.dll (Compatibilidad total con System32)
#pragma comment(linker, "/export:GetFileVersionInfoA=version.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=version.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=version.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=version.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=version.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=version.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=version.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=version.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=version.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=version.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=version.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=version.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=version.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=version.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=version.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=version.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=version.VerQueryValueW")

#include <MinHook.h>
#include <atomic>
#include "hook_dx11.h"
#include "hook_dx12.h"
#include "metrics.h"
#include "logger.h"
#include "config.h"

static HMODULE g_thisModule = nullptr;
static std::atomic<bool> g_abortInit{ false };
static HANDLE g_initDoneEvent = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    Logger_Init(g_thisModule);
    Config_Load(g_thisModule);

    if (g_abortInit)
    {
        SetEvent(g_initDoneEvent);
        return 0;
    }

    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK)
    {
        LOG_ERROR("MH_Initialize fallo: %d", (int)mhStatus);
        SetEvent(g_initDoneEvent);
        return 0;
    }

    if (g_abortInit)
    {
        MH_Uninitialize();
        SetEvent(g_initDoneEvent);
        return 0;
    }

    if (!HookDX11_Install())
        LOG_ERROR("HookDX11_Install fallo");
    if (!HookDX12_Install())
        LOG_ERROR("HookDX12_Install fallo");

    SetEvent(g_initDoneEvent);

    if (g_abortInit)
        return 0;

    Metrics_StartThread(500);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_thisModule = hModule;
        DisableThreadLibraryCalls(hModule);
        g_initDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_initDoneEvent)
            return FALSE;
        
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved != nullptr)
            break;

        g_abortInit = true;
        if (g_initDoneEvent)
        {
            WaitForSingleObject(g_initDoneEvent, INFINITE);
            CloseHandle(g_initDoneEvent);
            g_initDoneEvent = nullptr;
        }

        Metrics_StopThread();
        HookDX11_Uninstall();
        HookDX12_Uninstall();
        MH_Uninitialize();
        LOG_INFO("YiyoOverlay descargado correctamente");
        Logger_Shutdown();
        break;
    }
    return TRUE;
}
}
