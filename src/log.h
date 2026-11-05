#pragma once
#include <windows.h>
#include <cstdarg>

namespace davg {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };

void LogInit(const wchar_t* path, int level);
void LogSetLevel(int level);
int  LogGetLevel();
void LogWrite(int level, const char* fmt, ...);
void LogFlush();

} // namespace davg

#define LOGE(...) ::davg::LogWrite(::davg::LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ::davg::LogWrite(::davg::LOG_WARN, __VA_ARGS__)
#define LOGI(...) ::davg::LogWrite(::davg::LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (::davg::LogGetLevel() >= ::davg::LOG_DEBUG) ::davg::LogWrite(::davg::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (::davg::LogGetLevel() >= ::davg::LOG_TRACE) ::davg::LogWrite(::davg::LOG_TRACE, __VA_ARGS__); } while (0)
