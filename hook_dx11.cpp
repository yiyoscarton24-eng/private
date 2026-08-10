#include "hook_dx11.h"
#include "overlay.h"
#include "logger.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static Present_t oPresent = nullptr;
static ResizeBuffers_t oResizeBuffers = nullptr;

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_hwnd = nullptr;
static bool g_imguiInit = false;      // true solo si ESTE backend posee el contexto ImGui
static bool g_contextOwned = false;   // idem, distinguido para claridad en Uninstall
static WNDPROC g_originalWndProc = nullptr;

static LARGE_INTEGER g_freq{};
static LARGE_INTEGER g_lastFrameTime{};

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    return CallWindowProc(g_originalWndProc, hwnd, msg, wparam, lparam);
}

static bool CreateRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 GetBuffer(0) fallo: 0x%08X", hr);
        return false;
    }

    hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 CreateRenderTargetView fallo: 0x%08X", hr);
        return false;
    }
    return true;
}

static void ReleaseRenderTarget()
{
    if (g_rtv)
    {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

static void InitImGuiIfNeeded(IDXGISwapChain* swapChain)
{
    if (g_imguiInit) return;

    // El intento de reclamar el contexto ImGui va ANTES de tocar cualquier
    // recurso COM. Antes, GetDevice()/GetImmediateContext() se llamaban
    // primero y solo despues se comprobaba si el contexto ya estaba
    // reclamado por hook_dx12.cpp: si asi era, esta funcion salia sin
    // liberar esas dos referencias, y como g_imguiInit nunca llegaba a
    // true, InitImGuiIfNeeded se volvia a invocar en CADA Present(),
    // re-fugando g_device/g_context en cada frame de forma indefinida.
    if (g_imguiContextClaimed.exchange(true))
    {
        LOG_WARN("hook_dx11: contexto ImGui ya reclamado por otro backend, se omite inicializacion");
        return;
    }
    g_contextOwned = true;

    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&g_device));
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 swapChain->GetDevice fallo: 0x%08X", hr);
        // No se llego a crear nada: se libera la reclamacion para que,
        // si corresponde, hook_dx12.cpp pueda intentarlo.
        g_contextOwned = false;
        g_imguiContextClaimed = false;
        return;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    if (!CreateRenderTarget(swapChain))
    {
        LOG_ERROR("DX11 fallo al crear render target inicial, overlay deshabilitado");

        // Deshacer TODO lo adquirido en esta funcion. Antes este camino
        // devolvia sin liberar nada: el contexto ImGui, el hook de
        // wndproc y las referencias COM a g_device/g_context quedaban
        // vivos para siempre (g_imguiInit nunca llegaba a true, asi que
        // HookDX11_Uninstall tampoco los limpiaba), y ademas esta funcion
        // se re-ejecutaba en cada Present() repitiendo la fuga.
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_originalWndProc)
        {
            SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
            g_originalWndProc = nullptr;
        }
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_contextOwned = false;
        return;
    }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastFrameTime);

    g_imguiInit = true;
    LOG_INFO("ImGui inicializado (backend DX11)");
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags)
{
    LOG_INFO("DX11 ResizeBuffers solicitado (%ux%u, buffers=%u)", width, height, bufferCount);

    // El RTV mantiene una referencia al backbuffer: hay que soltarlo antes
    // de llamar a ResizeBuffers() o DXGI devuelve DXGI_ERROR_INVALID_CALL.
    ReleaseRenderTarget();

    HRESULT hr = oResizeBuffers(swapChain, bufferCount, width, height, newFormat, flags);
    if (FAILED(hr))
    {
        LOG_ERROR("DX11 ResizeBuffers original fallo: 0x%08X", hr);
        return hr;
    }

    if (g_imguiInit && g_contextOwned)
    {
        if (!CreateRenderTarget(swapChain))
            LOG_ERROR("DX11 fallo al recrear render target tras resize");
        else
            LOG_INFO("DX11 render target recreado tras resize");
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    InitImGuiIfNeeded(swapChain);

    if (g_imguiInit && g_contextOwned && g_rtv)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float frameTimeMs = (float)((now.QuadPart - g_lastFrameTime.QuadPart) * 1000.0 / g_freq.QuadPart);
        g_lastFrameTime = now;
        Overlay_OnFramePresented(frameTimeMs);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Overlay_Draw();

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return oPresent(swapChain, syncInterval, flags);
}

static bool GetDx11Addresses(void** outPresent, void** outResizeBuffers)
{
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"YiyoOverlayDummy11", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    D3D_FEATURE_LEVEL level;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swapChain,
        &device, &level, &context);

    bool ok = false;
    if (SUCCEEDED(hr))
    {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        *outPresent = vtable[8];        // IDXGISwapChain::Present
        *outResizeBuffers = vtable[13]; // IDXGISwapChain::ResizeBuffers
        ok = true;
    }
    else
    {
        LOG_ERROR("D3D11CreateDeviceAndSwapChain (dummy) fallo: 0x%08X", hr);
    }

    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return ok;
}

bool HookDX11_Install()
{
    void* presentAddr = nullptr;
    void* resizeAddr = nullptr;
    if (!GetDx11Addresses(&presentAddr, &resizeAddr))
    {
        LOG_ERROR("HookDX11_Install: no se pudieron obtener direcciones de vtable");
        return false;
    }

    if (MH_CreateHook(presentAddr, &HookedPresent, reinterpret_cast<void**>(&oPresent)) != MH_OK)
    {
        LOG_ERROR("MH_CreateHook (DX11 Present) fallo");
        return false;
    }
    if (MH_CreateHook(resizeAddr, &HookedResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK)
    {
        LOG_ERROR("MH_CreateHook (DX11 ResizeBuffers) fallo");
        return false;
    }

    bool ok = MH_EnableHook(presentAddr) == MH_OK && MH_EnableHook(resizeAddr) == MH_OK;
    if (ok)
        LOG_INFO("Hook DX11 instalado (Present + ResizeBuffers)");
    else
        LOG_ERROR("MH_EnableHook (DX11) fallo");
    return ok;
}

void HookDX11_Uninstall()
{
    if (g_imguiInit && g_contextOwned)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_originalWndProc)
            SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
    }
    ReleaseRenderTarget();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}
