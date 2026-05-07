#pragma once

#include <stdarg.h>

void DebugLogInit();
void DebugLogShutdown();
void DebugLogWrite(const char* fmt, ...);

#define DLOG(...) DebugLogWrite(__VA_ARGS__)
