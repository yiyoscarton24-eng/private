#pragma once

// Instala hooks sobre IDXGISwapChain3::Present y
// ID3D12CommandQueue::ExecuteCommandLists (necesario para capturar la
// command queue real que usa el juego, ImGui-DX12 la necesita para grabar
// sus propios comandos de dibujo).
bool HookDX12_Install();
void HookDX12_Uninstall();
