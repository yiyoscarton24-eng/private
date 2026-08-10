#include <windows.h>

// Directivas de enlace proxy para version.dll (Redirección limpia a System32)
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

// Sincroniza InitThread (que instala los hooks de forma asincrona tras
// DLL_PROCESS_ATTACH) con un DLL_PROCESS_DETACH concurrente disparado por
// un FreeLibrary explicito. Sin esto, MH_Uninitialize()/HookDX11_Uninstall()/
// HookDX12_Uninstall() podian ejecutarse mientras InitThread seguia en medio
// de MH_CreateHook/MH_EnableHook, corrompiendo el estado interno de MinHook
// y provocando un crash.
static std::atomic<bool> g_abortInit{ false };
static HANDLE g_initDoneEvent = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    Logger_Init(g_thisModule);
    Config_Load(g_thisModule);

    // Comprobacion temprana: si DLL_PROCESS_DETACH ya solicito abortar
    // (FreeLibrary concurrente disparado justo al arrancar este hilo), no
    // tiene sentido tocar MinHook en absoluto.
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

    // Segunda comprobacion tras MH_Initialize: si el abort llego mientras
    // esa llamada estaba en curso, se deshace y se sale sin instalar hooks.
    if (g_abortInit)
    {
        MH_Uninitialize();
        SetEvent(g_initDoneEvent);
        return 0;
    }

    // Se instalan ambos hooks: el que no corresponda a la API real del
    // juego nunca se dispara (nunca se crea el objeto correspondiente),
    // asi que no hace falta detectar DX11 vs DX12 de antemano. El guard
    // g_imguiContextClaimed en overlay.h asegura un unico ImGui context
    // aunque, por alguna razon, ambos backends llegaran a inicializarse.
    if (!HookDX11_Install())
        LOG_ERROR("HookDX11_Install fallo");
    if (!HookDX12_Install())
        LOG_ERROR("HookDX12_Install fallo");

    // Se senaliza AQUI, antes de arrancar el hilo de metricas: en cuanto
    // el evento se dispara, DLL_PROCESS_DETACH (si estaba esperando) da
    // por hecho que el estado de los hooks de MinHook ya es estable y
    // puede empezar a desinstalarlos con seguridad.
    SetEvent(g_initDoneEvent);

    if (g_abortInit)
    {
        // DLL_PROCESS_DETACH ya esta despierto y a punto de desinstalar
        // hooks/parar hilos: no arrancar un hilo de metricas nuevo que
        // ese DETACH no tiene forma de saber que debe esperar.
        return 0;
    }

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
        {
            // Sin el evento no hay forma segura de sincronizar el DETACH
            // con InitThread; es preferible abortar la carga a arriesgar
            // una carrera silenciosa mas adelante.
            return FALSE;
        }
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        // Si el proceso esta terminando (lpReserved != nullptr), el
        // sistema ya detuvo el resto de hilos (incluido InitThread, si
        // seguia vivo) antes de invocar DllMain con DLL_PROCESS_DETACH:
        // no existe carrera posible, y sincronizar aqui (esperar eventos,
        // uninstalar hooks, unir hilos) es inseguro y puede colgar el
        // proceso. Se sigue la guia oficial de Microsoft: en terminacion
        // de proceso no se libera nada, simplemente se retorna.
        if (lpReserved != nullptr)
            break;

        // Descarga explicita (FreeLibrary): puede solaparse con InitThread
        // todavia instalando hooks. Se le pide abortar y se espera a que
        // termine (o confirme que nunca llego a tocar MinHook) antes de
        // desinstalar nada.
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
