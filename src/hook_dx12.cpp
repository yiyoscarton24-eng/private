#include "hook_dx12.h"
#include "logger.h"
#include "overlay.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "MinHook.h"
#include <d3d12.h>
#include <dxgi1_4.h>

typedef HRESULT(WINAPI* PresentDX12_t)(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags);
static PresentDX12_t g_targetPresentDX12 = nullptr;

static ID3D12Device* g_device = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static ID3D12CommandQueue* g_commandQueue = nullptr;
static ID3D12GraphicsCommandList* g_commandList = nullptr;
static ID3D12Resource* g_backBuffers[16] = { nullptr };
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtvHandles[16] = {};
static UINT g_bufferCount = 0;
static bool g_initialized = false;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HRESULT WINAPI HookedPresentDX12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!g_initialized) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&g_device))) {
            DXGI_SWAP_CHAIN_DESC desc;
            pSwapChain->GetDesc(&desc);

            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = desc.BufferCount;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

            if (SUCCEEDED(g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_rtvHeap)))) {
                SIZE_T rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

                g_bufferCount = desc.BufferCount;
                for (UINT i = 0; i < g_bufferCount; ++i) {
                    pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
                    g_rtvHandles[i] = rtvHandle;
                    g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, rtvHandle);
                    rtvHandle.ptr += rtvDescriptorSize;
                }

                if (g_imguiContextClaimed.exchange(true) == false) {
                    ImGui::CreateContext();
                }

                ImGui_ImplWin32_Init(desc.OutputWindow);
                ImGui_ImplDX12_Init(g_device, desc.BufferCount, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr,
                                    g_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                    g_rtvHeap->GetGPUDescriptorHandleForHeapStart());

                g_initialized = true;
            }
        }
    }

    if (g_initialized) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Overlay_Draw();

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);
    }

    return g_targetPresentDX12(pSwapChain, SyncInterval, Flags);
}

bool HookDX12_Install() {
    return true;
}

void HookDX12_Uninstall() {
    if (g_targetPresentDX12) {
        MH_DisableHook(MH_ALL_HOOKS);
    }
}
