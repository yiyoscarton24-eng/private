#pragma once

// Arranca el hilo de métricas (CPU/RAM/GPU/VRAM). Seguro de llamar una sola vez.
// Actualiza g_overlay.* cada `intervalMs`.
void Metrics_StartThread(int intervalMs = 500);
void Metrics_StopThread();
