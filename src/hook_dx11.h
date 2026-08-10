#pragma once

// Instala el hook sobre IDXGISwapChain::Present usando MinHook.
// No falla si el juego usa DX12 en vez de DX11: simplemente el hook
// nunca se dispara porque no se crea ningún swapchain DX11.
bool HookDX11_Install();
void HookDX11_Uninstall();
