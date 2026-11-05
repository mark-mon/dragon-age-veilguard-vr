#pragma once
#include <windows.h>

namespace davg {
enum class ProxyKind { Unknown, Dinput8, Version };
void ProxyDetect(HMODULE self);
ProxyKind ProxyGetKind();
const wchar_t* ProxySelfName();
FARPROC ProxyReal(const char* name);
}
