#pragma once
#include <deque>
#include <mutex>
#include <atomic>
#include <vector>

struct OverlayState {
    std::atomic<bool> visible{ true };

    float fps = 0.0f;
    float frameTimeMs = 0.0f;

    std::atomic<float> cpuPercent{ 0.0f };
    std::atomic<float> cpuMHz{ 0.0f };
    std::atomic<float> ramUsedGB{ 0.0f };
    std::atomic<float> ramTotalGB{ 0.0f };
    std::atomic<float> gpuPercent{ 0.0f };
    std::atomic<float> vramUsedGB{ 0.0f };
    std::atomic<float> vramTotalGB{ 0.0f };

    std::mutex historyMutex;
    std::deque<float> fpsHistory;
    std::deque<float> frameTimeHistory;
    static constexpr size_t kHistoryMaxSamples = 300;
};

extern OverlayState g_overlay;

// Garantiza un unico ImGui::CreateContext() en todo el proceso, sin importar
// si lo dispara hook_dx11.cpp o hook_dx12.cpp. Devuelve true solo para el
// primer llamador (el que debe crear el contexto).
extern std::atomic<bool> g_imguiContextClaimed;

void Overlay_OnFramePresented(float frameTimeMs);
void Overlay_Draw();
void Overlay_PollToggleKey();
