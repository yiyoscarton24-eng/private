#pragma once
#include <windows.h>
#include <string>

enum class LogLevel { Info, Warn, Error };

void Logger_Init(HMODULE moduleHandle);
void Logger_Shutdown();
void Logger_Log(LogLevel level, const char* fmt, ...);

std::wstring GetModuleDirectory(HMODULE mod);

#define LOG_INFO(...)  Logger_Log(LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  Logger_Log(LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) Logger_Log(LogLevel::Error, __VA_ARGS__)
