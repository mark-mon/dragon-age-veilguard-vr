// Signature resolver - see resolver.h. Cache: DAVGVR\offsets.txt, one [exe size timestamp] section per game build;
// a section that does not match the running exe is ignored, so stale entries never apply to another build. Entries
// may be filled in by hand (rva:0x...) when a signature fails after a game patch.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "resolver.h"
#include "globals.h"
#include "log.h"

namespace davg {

struct ExeInfo { uint8_t* base = nullptr; size_t size = 0; uint32_t ts = 0; std::vector<std::pair<uint8_t*, size_t>> code; std::string key; };
static std::mutex g_mu;
static std::map<std::string, std::map<std::string, std::string>> g_cache;
static bool g_loaded = false, g_dirty = false;

static const ExeInfo& Exe() {
    static ExeInfo e; static bool done = false;
    if (done) return e;
    done = true;
    e.base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(e.base + ((IMAGE_DOS_HEADER*)e.base)->e_lfanew);
    e.size = nt->OptionalHeader.SizeOfImage; e.ts = nt->FileHeader.TimeDateStamp;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) e.code.push_back({e.base + sec->VirtualAddress, (size_t)sec->Misc.VirtualSize});
    char k[96]; snprintf(k, sizeof(k), "exe 0x%08zx 0x%08x", e.size, e.ts); e.key = k;
    LOGI("resolver: %s, %zu code sections", e.key.c_str(), e.code.size());
    return e;
}
static std::wstring CachePath() { return G().outDir + L"offsets.txt"; }
static std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static void LoadCache() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = _wfopen(CachePath().c_str(), L"r");
    if (!f) return;
    char line[512]; std::string section;
    while (fgets(line, sizeof(line), f)) {
        std::string l = line; size_t c = l.find('#'); if (c != std::string::npos) l = l.substr(0, c);
        l = Trim(l); if (l.empty()) continue;
        if (l[0] == '[') { section = Trim(l.substr(1, l.find(']') - 1)); continue; }
        size_t eq = l.find('='); if (eq == std::string::npos || section.empty()) continue;
        g_cache[section][Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    fclose(f);
}
void ResolverSaveCache() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_dirty) return;
    FILE* f = _wfopen(CachePath().c_str(), L"w");
    if (!f) return;
    fputs("# DAVG VR address cache: one [exe size timestamp] section per game build. Values rva:0x... (exe-relative).\n"
          "# Written by the signature scanner; may be filled in by hand for a new build if a signature fails.\n", f);
    for (auto& s : g_cache) { fprintf(f, "\n[%s]\n", s.first.c_str()); for (auto& kv : s.second) fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str()); }
    fclose(f);
    g_dirty = false;
}
static bool Parse(const char* p, std::vector<int>& out) {
    out.clear();
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { out.push_back(-1); p++; if (*p == '?') p++; continue; }
        unsigned v; if (sscanf(p, "%2x", &v) != 1) return false;
        out.push_back((int)v); p += 2;
    }
    return !out.empty();
}
static void Scan(uint8_t* start, size_t size, const std::vector<int>& pat, std::vector<uint8_t*>& hits, size_t maxHits) {
    const size_t n = pat.size(); if (size < n) return;
    size_t a = 0; while (a < n && pat[a] < 0) a++;
    if (a == n) return;
    const uint8_t first = (uint8_t)pat[a];
    size_t i = 0;
    while (i + n <= size) {
        uint8_t* p = (uint8_t*)memchr(start + i + a, first, size - n - i + 1);
        if (!p) break;
        i = (size_t)(p - start) - a;
        bool ok = true;
        for (size_t j = 0; j < n; j++) if (pat[j] >= 0 && start[i + j] != (uint8_t)pat[j]) { ok = false; break; }
        if (ok) { hits.push_back(start + i); if (hits.size() >= maxHits) return; }
        i++;
    }
}
void* ResolveSig(const char* name, const char* pattern, int offset, char resolve, int insnEnd) {
    const ExeInfo& e = Exe();
    std::lock_guard<std::mutex> lk(g_mu);
    LoadCache();
    auto sec = g_cache.find(e.key);
    if (sec != g_cache.end()) {
        auto it = sec->second.find(name);
        if (it != sec->second.end() && it->second.rfind("rva:", 0) == 0) {
            uintptr_t rva = (uintptr_t)strtoull(it->second.c_str() + 4, nullptr, 0);
            if (rva && rva < e.size) { LOGI("resolver: %-20s cached  exe+%llX", name, (unsigned long long)rva); return e.base + rva; }
        }
    }
    std::vector<int> pat;
    if (!Parse(pattern, pat)) { LOGE("resolver: bad pattern for %s", name); return nullptr; }
    DWORD t0 = GetTickCount();
    std::vector<uint8_t*> hits;
    for (auto& c : e.code) { Scan(c.first, c.second, pat, hits, 2); if (hits.size() >= 2) break; }
    if (hits.size() != 1) {
        LOGE("resolver: %-20s signature matched %zu times (need exactly 1) - game build not supported for this feature; "
             "an rva can be entered in DAVGVR\\offsets.txt [%s]", name, hits.size(), e.key.c_str());
        return nullptr;
    }
    uint8_t* p = hits[0] + offset;
    if (resolve == 'r') { int32_t d; memcpy(&d, p, 4); p = p + insnEnd + d; }
    uintptr_t rva = (uintptr_t)(p - e.base);
    if (rva >= e.size) { LOGE("resolver: %s resolved outside the exe", name); return nullptr; }
    char v[32]; snprintf(v, sizeof(v), "rva:0x%08llX", (unsigned long long)rva);
    g_cache[e.key][name] = v; g_dirty = true;
    LOGI("resolver: %-20s scanned exe+%llX (%lu ms)", name, (unsigned long long)rva, GetTickCount() - t0);
    return p;
}

} // namespace davg
