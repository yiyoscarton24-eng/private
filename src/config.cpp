#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

OverlayConfig g_config;

static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool ParseBool(const std::string& value, bool defaultValue)
{
    std::string v = ToLower(Trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on")  return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return defaultValue;
}

static float ParseFloat(const std::string& value, float defaultValue)
{
    try { return std::stof(Trim(value)); }
    catch (...) { return defaultValue; }
}

void Config_Load(HMODULE moduleHandle)
{
    std::wstring dir = GetModuleDirectory(moduleHandle);
    std::wstring path = dir + L"\\config.ini";

    std::ifstream file(path);
    if (!file.is_open())
    {
        LOG_WARN("config.ini no encontrado en %ls, se usan valores por defecto", path.c_str());
        return;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line))
    {
        lineNumber++;
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[')
            continue;

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            LOG_WARN("config.ini linea %d ignorada (sin '='): %s", lineNumber, trimmed.c_str());
            continue;
        }

        std::string key = Trim(trimmed.substr(0, eq));
        std::string rawValue = trimmed.substr(eq + 1);
        // Corta comentarios en linea: "PosX = 20 ; comentario"
        size_t comment = rawValue.find_first_of(";#");
        if (comment != std::string::npos)
            rawValue = rawValue.substr(0, comment);
        std::string value = Trim(rawValue);

        std::string keyLower = ToLower(key);

        if      (keyLower == "showfps")  g_config.showFPS  = ParseBool(value, g_config.showFPS);
        else if (keyLower == "showcpu")  g_config.showCPU  = ParseBool(value, g_config.showCPU);
        else if (keyLower == "showgpu")  g_config.showGPU  = ParseBool(value, g_config.showGPU);
        else if (keyLower == "showram")  g_config.showRAM  = ParseBool(value, g_config.showRAM);
        else if (keyLower == "showvram") g_config.showVRAM = ParseBool(value, g_config.showVRAM);
        else if (keyLower == "posx")     g_config.posX     = ParseFloat(value, g_config.posX);
        else if (keyLower == "posy")     g_config.posY     = ParseFloat(value, g_config.posY);
        else if (keyLower == "scale")    g_config.scale    = ParseFloat(value, g_config.scale);
        else LOG_WARN("config.ini linea %d: clave desconocida '%s'", lineNumber, key.c_str());
    }

    if (g_config.scale < 0.5f || g_config.scale > 3.0f)
    {
        LOG_WARN("Scale fuera de rango (%.2f), se fuerza a 1.0", g_config.scale);
        g_config.scale = 1.0f;
    }

    LOG_INFO("config.ini cargado: FPS=%d CPU=%d GPU=%d RAM=%d VRAM=%d Pos=(%.0f,%.0f) Scale=%.2f",
        g_config.showFPS, g_config.showCPU, g_config.showGPU, g_config.showRAM, g_config.showVRAM,
        g_config.posX, g_config.posY, g_config.scale);
}
