#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

namespace davg {

// Simple key = value config file. '#' or ';' start comments. Keys are case-insensitive.
class Config {
public:
    bool Load(const wchar_t* path);
    std::string GetStr(const char* key, const char* def) const;
    int GetInt(const char* key, int def) const;
    float GetFloat(const char* key, float def) const;
    bool GetBool(const char* key, bool def) const;
    void Set(const char* key, const std::string& value);   // in memory only (the menu writes the file itself)
    void Dump() const; // to log
private:
    std::unordered_map<std::string, std::string> m_;
    mutable std::mutex mu_;   // the menu (XR thread) sets values while other threads read them
};

// Writes a default config.txt if it does not exist.
void WriteDefaultConfig(const wchar_t* path);

} // namespace davg
