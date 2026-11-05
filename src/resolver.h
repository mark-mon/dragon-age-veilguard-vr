#pragma once
// Update-proof game addresses: byte signatures (wildcards) scanned in the exe's executable sections at start-up,
// cached per exe build in DAVGVR\offsets.txt (after the SWTOR VR resolver). A signature must match exactly once.
namespace davg {
// pattern "48 8B 0D ?? ?? ?? ??"; offset from the match start; resolve 'r' = rel32 / rip disp32 at match+offset
// (target = disp field + insnEnd + disp), 0 = the match address + offset. Returns nullptr when not found.
void* ResolveSig(const char* name, const char* pattern, int offset = 0, char resolve = 0, int insnEnd = 4);
void ResolverSaveCache();
}
