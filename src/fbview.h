#pragma once
namespace davg {
bool FbViewInstall();        // hooks the Frostbite view builder / setupView (head tracking, probe)
void FbViewApplyConfig();    // (re)reads vr_mode, level_camera, world_scale, pose_latency_frames, debug_fake_head_yaw_deg
void FbViewOnPresent();      // render thread, before Present: hands the frame's render pose to the XR side
void FbViewDumpTrigger();
int FbViewMode();            // 0 flat, 1 head, 2 stereo (current, F8 included)
struct XrHostEye;
bool FbViewStereoPoses(XrHostEye eyes[2]);
unsigned long long FbViewNewestSerial();
bool FbViewStereoPosesForSerial(unsigned long long serial, XrHostEye eyes[2]);
void FbViewNoteStamp(unsigned long long stamp);
bool FbViewStereoPosesForCam(const float leftCamPosFwd[6], XrHostEye eyes[2]);   // exact pairing by the frame's left camera position   // eye poses/FOVs of the frame being presented (stereo)    // viewdump.trigger: dump the next frame's view structures to DAVGVR\vdNN_*.bin
}
