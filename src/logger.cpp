#include "logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

std::wstring GetModuleDirectory(HMODULE mod)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : full.substr(0, pos);
}

void Logger_Init(HMODULE moduleHandle)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) return;

    std::wstring dir = GetModuleDirectory(moduleHandle);
    std::wstring logPath = dir + L"\\YiyoOverlay.log";

    // "w" trunca el log en cada carga de la DLL; evita crecimiento indefinido
    // entre sesiones de juego.
    _wfopen_s(&g_logFile, logPath.c_str(), L"w");
    if (g_logFile)
    {
        fprintf(g_logFile, "==== YiyoOverlay log iniciado ====\n");
        fflush(g_logFile);
    }
}

void Logger_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile)
    {
        fprintf(g_logFile, "==== YiyoOverlay log cerrado ====\n");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static const char* LevelString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void Logger_Log(LogLevel level, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) return;

    time_t now = time(nullptr);
    tm localTime{};
    localtime_s(&localTime, &now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localTime);

    fprintf(g_logFile, "[%s] [%s] ", timeBuf, LevelString(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}
