#pragma once
#include <windows.h>
#include <string>
#include "config.h"

namespace davg {
struct Globals {
    HMODULE self = nullptr;
    std::wstring gameDir, outDir, cfgPath;
    Config cfg;
    HANDLE initEvent = nullptr;
};
Globals& G();
}
