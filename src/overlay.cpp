#include "overlay.h"
#include "config.h"
#include "imgui.h"
#include <windows.h>
#include <vector>

OverlayState g_overlay;
std::atomic<bool> g_imguiContextClaimed{ false };

void Overlay_OnFramePresented(float frameTimeMs)
{
    g_overlay.frameTimeMs = frameTimeMs;
    g_overlay.fps = (frameTimeMs > 0.0f) ? (1000.0f / frameTimeMs) : 0.0f;

    std::lock_guard<std::mutex> lock(g_overlay.historyMutex);
    g_overlay.fpsHistory.push_back(g_overlay.fps);
    g_overlay.frameTimeHistory.push_back(frameTimeMs);
    if (g_overlay.fpsHistory.size() > OverlayState::kHistoryMaxSamples)
        g_overlay.fpsHistory.pop_front();
    if (g_overlay.frameTimeHistory.size() > OverlayState::kHistoryMaxSamples)
        g_overlay.frameTimeHistory.pop_front();
}

void Overlay_PollToggleKey()
{
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (isDown && !wasDown)
        g_overlay.visible = !g_overlay.visible;
    wasDown = isDown;
}

void Overlay_Draw()
{
    Overlay_PollToggleKey();
    if (!g_overlay.visible)
        return;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::SetNextWindowPos(ImVec2(g_config.posX, g_config.posY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::GetIO().FontGlobalScale = g_config.scale;

    if (ImGui::Begin("YiyoOverlay", nullptr, flags))
    {
        if (g_config.showFPS)
            ImGui::Text("FPS   %.0f", g_overlay.fps);
        if (g_config.showFPS)
            ImGui::Text("FT    %.1f ms", g_overlay.frameTimeMs);
        if (g_config.showFPS)
            ImGui::Separator();

        if (g_config.showCPU)
            ImGui::Text("CPU   %.0f%%  (%.0f MHz)", g_overlay.cpuPercent.load(), g_overlay.cpuMHz.load());
        if (g_config.showRAM)
            ImGui::Text("RAM   %.1f / %.1f GB", g_overlay.ramUsedGB.load(), g_overlay.ramTotalGB.load());
        if (g_config.showGPU)
            ImGui::Text("GPU   %.0f%%", g_overlay.gpuPercent.load());
        if (g_config.showVRAM)
            ImGui::Text("VRAM  %.1f / %.1f GB", g_overlay.vramUsedGB.load(), g_overlay.vramTotalGB.load());

        if (g_config.showFPS)
        {
            ImGui::Separator();
            std::lock_guard<std::mutex> lock(g_overlay.historyMutex);
            if (!g_overlay.frameTimeHistory.empty())
            {
                std::vector<float> samples(g_overlay.frameTimeHistory.begin(), g_overlay.frameTimeHistory.end());
                ImGui::PlotLines("##frametime", samples.data(), (int)samples.size(),
                                  0, "frametime (ms)", 0.0f, 33.0f, ImVec2(220, 50));
            }
        }
    }
    ImGui::End();
}
