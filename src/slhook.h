#pragma once
#include <cstdint>
namespace davg {
bool SlHookInstall();                         // hooks sl.interposer exports (call early, before DLSS init)
void SlHookEnable(bool on);
void SlHookSetTagRouting(int mode);
void SlHookSetRightJitter(int mode);   // 0 game, 1 zero, 2 negated, 3 previous frame   // 0 call order, 1 per buffer type
void SlHookReset();   // output size changed: forget texture -> eye mappings                   // per-eye DLSS: right eye on viewport 1
void SlHookSetOutputs(void* left, void* right);   // replay: this frame's DLSS output textures per eye
uint64_t SlHookRemapped();
void SlHookReport();
void SlHookLogConstants(int n);
void SlHookLearnDepthEye(void* depth, int eye);   // replay: the depth texture of the DLSS evaluate executed 1st / 2nd in a frame
}
