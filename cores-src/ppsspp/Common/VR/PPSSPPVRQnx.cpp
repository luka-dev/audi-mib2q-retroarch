// The Audi MHI2Q has no OpenXR runtime.  Keep PPSSPP's cross-platform VR API
// available to shared core code without carrying the OpenXR implementation,
// static controller maps, or runtime loader into the libretro core.

#include <cstring>

#include "Common/VR/PPSSPPVR.h"

bool IsVREnabled() { return false; }
void InitVROnAndroid(void *, void *, const char *, int, const char *) {}
void EnterVR(bool) {}
void GetVRResolutionPerEye(int *, int *) {}
void SetVRCallbacks(void (*)(const AxisInput *, size_t), bool (*)(const KeyInput &), void (*)(const TouchInput &)) {}

void SetVRAppMode(VRAppMode) {}
void UpdateVRInput(bool, float, float) {}
bool UpdateVRAxis(const AxisInput *, size_t) { return false; }
bool UpdateVRKeys(const KeyInput &) { return false; }

void PreprocessStepVR(void *) {}
void SetVRCompat(VRCompatFlag, long) {}

void *BindVRFramebuffer() { return nullptr; }
bool StartVRRender() { return false; }
void FinishVRRender() {}
void PreVRFrameRender(int) {}
void PostVRFrameRender() {}
int GetVRFBOIndex() { return 0; }
int GetVRPassesCount() { return 1; }
bool IsPassthroughSupported() { return false; }
bool IsBigScreenVRMode() { return false; }
bool IsFlatVRGame() { return false; }
bool IsFlatVRScene() { return false; }
bool IsGameVRScene() { return false; }
bool IsImmersiveVRMode() { return false; }
bool Is2DVRObject(float *, bool) { return false; }
void UpdateVRParams(float *) {}
void UpdateVRProjection(float *input, float *output) {
	if (input && output)
		std::memcpy(output, input, 16 * sizeof(float));
}
void UpdateVRView(float *, float *) {}
void UpdateVRViewMatrices() {}
