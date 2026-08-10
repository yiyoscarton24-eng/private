#pragma once
#include <windows.h>
#include <string>

struct OverlayConfig {
    bool showFPS = true;
    bool showCPU = true;
    bool showGPU = true;
    bool showRAM = true;
    bool showVRAM = true;
    float posX = 20.0f;
    float posY = 20.0f;
    float scale = 1.0f;
};

extern OverlayConfig g_config;

// Busca config.ini junto a la DLL y sobrescribe g_config con lo encontrado.
// Si el archivo no existe o una clave falta, se conserva el valor por defecto.
void Config_Load(HMODULE moduleHandle);
