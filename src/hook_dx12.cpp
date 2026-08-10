#include "hook_dx12.h"
#include "overlay.h"
#include "logger.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static Present_t oPresent = nullptr;
static ResizeBuffers_t oResizeBuffers = nullptr;
static ExecuteCommandLists_t oExecuteCommandLists = nullptr;

static ID3D12Device* g_device = nullptr;
static ID3D12CommandQueue* g_commandQueue = nullptr; // capturada desde ExecuteCommandLists; NO la creamos nosotros
static ID3D12DescriptorHeap* g_srvHeap = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static ID3D12CommandAllocator* g_cmdAlloc = nullptr;
static ID3D12GraphicsCommandList* g_cmdList = nullptr;
static std::vector<ID3D12Resource*> g_backBuffers;
static std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> g_rtvHandles;
static UINT g_bufferCount = 0;
static HWND g_hwnd = nullptr;
static bool g_imguiInit = false;
static bool g_contextOwned = false;
static WNDPROC g_originalWndProc = nullptr;

// Sincronizacion CPU/GPU propia. Sin esto, resetear g_cmdAlloc en cada
// Present() mientras la GPU aun pudiera estar ejecutando la lista de
// comandos del frame anterior es un uso indebido de la API (comportamiento
// indefinido / crash intermitente, dependiente del driver).
static ID3D12Fence* g_fence = nullptr;
static UINT64 g_fenceValue = 0;
static HANDLE g_fenceEvent = nullptr;

static LARGE_INTEGER g_freq{};
static LARGE_INTEGER g_lastFrameTime{};

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    return CallWindowProc(g_originalWndProc, hwnd, msg, wparam, lparam);
}

static void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
{
    if (!g_commandQueue)
    {
        g_commandQueue = queue;
        LOG_INFO("DX12 CommandQueue real capturada desde ExecuteCommandLists");
    }
    oExecuteCommandLists(queue, numLists, lists);
}

// Espera a que la GPU haya terminado todo el trabajo encolado hasta este
// punto en g_commandQueue. Se usa antes de reutilizar g_cmdAlloc/g_cmdList
// y antes de liberar backbuffers en un resize.
static void WaitForGpuIdle()
{
    if (!g_commandQueue || !g_fence || !g_fenceEvent) return;

    const UINT64 value = ++g_fenceValue;
    HRESULT hr = g_commandQueue->Signal(g_fence, value);
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CommandQueue::Signal fallo: 0x%08X", hr);
        return;
    }

    if (g_fence->GetCompletedValue() < value)
    {
        hr = g_fence->SetEventOnCompletion(value, g_fenceEvent);
        if (FAILED(hr))
        {
            LOG_ERROR("DX12 Fence::SetEventOnCompletion fallo: 0x%08X", hr);
            return;
        }
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

static void ReleaseBackBuffers()
{
    for (auto*& bb : g_backBuffers)
    {
        if (bb) { bb->Release(); bb = nullptr; }
    }
    g_backBuffers.clear();
    g_rtvHandles.clear();
}

// Libera cualquier recurso D3D12 parcialmente creado durante un
// InitImGuiIfNeeded que fallo antes de completar la inicializacion, y
// libera la reclamacion del contexto ImGui para que, si corresponde,
// hook_dx11.cpp pueda intentarlo. Solo se usa en rutas de fallo previas a
// ImGui::CreateContext()/al hook de wndproc (ver InitImGuiIfNeeded): esas
// dos cosas ocurren al final, cuando ya no queda ningun paso que pueda
// fallar antes de marcar g_imguiInit = true.
static void ReleasePartialInit()
{
    ReleaseBackBuffers();
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_cmdList) { g_cmdList->Release(); g_cmdList = nullptr; }
    if (g_cmdAlloc) { g_cmdAlloc->Release(); g_cmdAlloc = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_bufferCount = 0;
    g_contextOwned = false;
    g_imguiContextClaimed = false;
}

static bool CreateBackBuffers(IDXGISwapChain3* swapChain, UINT bufferCount)
{
    // Si cambia el numero de buffers hay que recrear tambien el heap de RTVs.
    if (g_rtvHeap && g_bufferCount != bufferCount)
    {
        g_rtvHeap->Release();
        g_rtvHeap = nullptr;
    }

    if (!g_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = bufferCount;
        HRESULT hr = g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap));
        if (FAILED(hr))
        {
            LOG_ERROR("DX12 CreateDescriptorHeap (RTV) fallo: 0x%08X", hr);
            return false;
        }
    }

    g_bufferCount = bufferCount;
    UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    g_backBuffers.resize(bufferCount, nullptr);
    g_rtvHandles.resize(bufferCount);

    for (UINT i = 0; i < bufferCount; i++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvStart;
        handle.ptr += (SIZE_T)i * rtvSize;

        HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
        if (FAILED(hr))
        {
            LOG_ERROR("DX12 GetBuffer(%u) fallo: 0x%08X", i, hr);
            return false;
        }
        g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, handle);
        g_rtvHandles[i] = handle;
    }
    return true;
}

static bool InitImGuiIfNeeded(IDXGISwapChain3* swapChain)
{
    if (g_imguiInit) return true;
    if (!g_commandQueue) return false; // aun no se ha capturado la queue real del juego

    // Igual que en hook_dx11.cpp: se reclama el contexto ImGui ANTES de
    // adquirir ningun recurso COM/D3D12. Antes, g_device se obtenia con
    // GetDevice() primero y solo despues se comprobaba g_imguiContextClaimed;
    // si hook_dx11.cpp ya lo habia reclamado, esta funcion salia sin
    // liberar esa referencia, y al no llegar nunca g_imguiInit a true se
    // volvia a llamar en cada Present(), re-fugando g_device cada frame.
    if (g_imguiContextClaimed.exchange(true))
    {
        LOG_WARN("hook_dx12: contexto ImGui ya reclamado por otro backend, se omite inicializacion");
        return false;
    }
    g_contextOwned = true;

    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&g_device));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 swapChain->GetDevice fallo: 0x%08X", hr);
        // No se llego a crear nada: se libera la reclamacion para que,
        // si corresponde, hook_dx11.cpp pueda intentarlo.
        g_contextOwned = false;
        g_imguiContextClaimed = false;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CreateDescriptorHeap (SRV) fallo: 0x%08X", hr);
        ReleasePartialInit();
        return false;
    }

    if (!CreateBackBuffers(swapChain, desc.BufferCount))
    {
        // CreateBackBuffers ya loguea la causa especifica del fallo.
        ReleasePartialInit();
        return false;
    }

    hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmdAlloc));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CreateCommandAllocator fallo: 0x%08X", hr);
        ReleasePartialInit();
        return false;
    }

    hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmdAlloc, nullptr, IID_PPV_ARGS(&g_cmdList));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CreateCommandList fallo: 0x%08X", hr);
        ReleasePartialInit();
        return false;
    }
    g_cmdList->Close();

    hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CreateFence fallo: 0x%08X", hr);
        ReleasePartialInit();
        return false;
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent)
    {
        LOG_ERROR("DX12 CreateEvent (fence) fallo: %lu", GetLastError());
        ReleasePartialInit();
        return false;
    }

    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_Init(g_device, desc.BufferCount, DXGI_FORMAT_R8G8B8A8_UNORM, g_srvHeap,
        g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
        g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastFrameTime);

    g_imguiInit = true;
    LOG_INFO("ImGui inicializado (backend DX12)");
    return true;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags)
{
    LOG_INFO("DX12 ResizeBuffers solicitado (%ux%u, buffers=%u)", width, height, bufferCount);

    if (g_imguiInit && g_contextOwned)
    {
        // Hay que garantizar que la GPU ya no referencia los backbuffers
        // actuales (via nuestras RTVs o comandos en vuelo) antes de
        // liberarlos: de lo contrario ResizeBuffers puede fallar con
        // DXGI_ERROR_INVALID_CALL o, peor, provocar un TDR/crash del driver.
        WaitForGpuIdle();
        ReleaseBackBuffers();
    }

    UINT effectiveCount = (bufferCount != 0) ? bufferCount : g_bufferCount;
    HRESULT hr = oResizeBuffers(swapChain, bufferCount, width, height, newFormat, flags);
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 ResizeBuffers original fallo: 0x%08X", hr);
        return hr;
    }

    if (g_imguiInit && g_contextOwned)
    {
        if (!CreateBackBuffers(swapChain, effectiveCount))
            LOG_ERROR("DX12 fallo al recrear backbuffers tras resize");
        else
            LOG_INFO("DX12 backbuffers recreados tras resize");
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags)
{
    if (InitImGuiIfNeeded(swapChain) && g_contextOwned)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float frameTimeMs = (float)((now.QuadPart - g_lastFrameTime.QuadPart) * 1000.0 / g_freq.QuadPart);
        g_lastFrameTime = now;
        Overlay_OnFramePresented(frameTimeMs);

        UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Overlay_Draw();

        ImGui::Render();

        // Espera a que la GPU haya terminado de consumir el cmdAlloc/cmdList
        // del frame anterior antes de resetearlos (ver WaitForGpuIdle).
        WaitForGpuIdle();

        HRESULT hr = g_cmdAlloc->Reset();
        if (FAILED(hr)) { LOG_ERROR("DX12 CommandAllocator::Reset fallo: 0x%08X", hr); return oPresent(swapChain, syncInterval, flags); }

        hr = g_cmdList->Reset(g_cmdAlloc, nullptr);
        if (FAILED(hr)) { LOG_ERROR("DX12 CommandList::Reset fallo: 0x%08X", hr); return oPresent(swapChain, syncInterval, flags); }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_backBuffers[backBufferIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_cmdList->ResourceBarrier(1, &barrier);

        g_cmdList->OMSetRenderTargets(1, &g_rtvHandles[backBufferIndex], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
        g_cmdList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &barrier);

        hr = g_cmdList->Close();
        if (FAILED(hr)) { LOG_ERROR("DX12 CommandList::Close fallo: 0x%08X", hr); return oPresent(swapChain, syncInterval, flags); }

        ID3D12CommandList* lists[] = { g_cmdList };
        g_commandQueue->ExecuteCommandLists(1, lists);
    }

    return oPresent(swapChain, syncInterval, flags);
}

static bool GetDx12Addresses(void** outPresent, void** outResizeBuffers, void** outExecute)
{
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"YiyoOverlayDummy12", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
    {
        LOG_ERROR("D3D12CreateDevice (dummy) fallo: 0x%08X", hr);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
    {
        LOG_ERROR("DX12 CreateCommandQueue (dummy) fallo: 0x%08X", hr);
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    IDXGIFactory4* factory = nullptr;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateDXGIFactory1 (dummy DX12) fallo: 0x%08X", hr);
        queue->Release();
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = 2;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    IDXGISwapChain1* swapChain1 = nullptr;
    bool ok = false;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scDesc, nullptr, nullptr, &swapChain1);
    if (SUCCEEDED(hr))
    {
        void** presentVtable = *reinterpret_cast<void***>(swapChain1);
        *outPresent = presentVtable[8];        // IDXGISwapChain::Present
        *outResizeBuffers = presentVtable[13]; // IDXGISwapChain::ResizeBuffers

        void** queueVtable = *reinterpret_cast<void***>(queue);
        *outExecute = queueVtable[10]; // ID3D12CommandQueue::ExecuteCommandLists

        ok = true;
        swapChain1->Release();
    }
    else
    {
        LOG_ERROR("CreateSwapChainForHwnd (dummy DX12) fallo: 0x%08X", hr);
    }

    factory->Release();
    queue->Release();
    device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return ok;
}

bool HookDX12_Install()
{
    void* presentAddr = nullptr;
    void* resizeAddr = nullptr;
    void* executeAddr = nullptr;
    if (!GetDx12Addresses(&presentAddr, &resizeAddr, &executeAddr))
    {
        LOG_ERROR("HookDX12_Install: no se pudieron obtener direcciones de vtable");
        return false;
    }

    if (MH_CreateHook(presentAddr, &HookedPresent, reinterpret_cast<void**>(&oPresent)) != MH_OK)
    { LOG_ERROR("MH_CreateHook (DX12 Present) fallo"); return false; }

    if (MH_CreateHook(resizeAddr, &HookedResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK)
    { LOG_ERROR("MH_CreateHook (DX12 ResizeBuffers) fallo"); return false; }

    if (MH_CreateHook(executeAddr, &HookedExecuteCommandLists, reinterpret_cast<void**>(&oExecuteCommandLists)) != MH_OK)
    { LOG_ERROR("MH_CreateHook (DX12 ExecuteCommandLists) fallo"); return false; }

    bool ok = MH_EnableHook(presentAddr) == MH_OK
        && MH_EnableHook(resizeAddr) == MH_OK
        && MH_EnableHook(executeAddr) == MH_OK;

    if (ok)
        LOG_INFO("Hook DX12 instalado (Present + ResizeBuffers + ExecuteCommandLists)");
    else
        LOG_ERROR("MH_EnableHook (DX12) fallo");

    return ok;
}

void HookDX12_Uninstall()
{
    if (g_imguiInit && g_contextOwned)
    {
        WaitForGpuIdle();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_originalWndProc)
            SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
    }

    ReleaseBackBuffers();
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_cmdList) { g_cmdList->Release(); g_cmdList = nullptr; }
    if (g_cmdAlloc) { g_cmdAlloc->Release(); g_cmdAlloc = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}
