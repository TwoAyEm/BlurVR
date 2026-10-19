// Finding the game's camera hand-over functions in the code of Blur.exe by what they look like, not by where they are (NOTES.md section 25).
// Plain C++ on a block of memory, so the same code runs in the proxy on the loaded module and in tools/camera_sig_check on a file.
#pragma once
#include <stddef.h>

// What a scan of the executable sections has found so far.  Feed every executable section to ScanForGameCameraCode, then ask
// ResolveGameCameraCode.
struct GameCameraScan {
    // [function][form]: function 0 = SetCameraData, 1 = SetCamera; form 0 = tight (only call targets and absolute addresses are
    // wildcards), 1 = loose (every displacement and immediate is a wildcard too)
    unsigned count[2][2];
    const unsigned char* first[2][2];
};

// The two functions, once found.  Both are __thiscall members of the render view class: SetCameraData(view, T) and
// SetCamera(view, camera, flag).
struct GameCameraCode {
    const unsigned char* setCameraData;
    const unsigned char* setCamera;
    size_t setCameraDataStolen, setCameraStolen; // whole instructions at the start of each function, without relative operands, for the hook
    size_t cameraBytes;                          // size of the camera description SetCamera copies, read from its code
    int dataForm, cameraForm;                    // 0 = tight pattern, 1 = loose pattern
};

void InitGameCameraScan(GameCameraScan& scan);
// Search one block of code.  Matches are counted over all blocks.
void ScanForGameCameraCode(const unsigned char* code, size_t size, GameCameraScan& scan);
// Both functions found exactly once (a tight match counts when the loose pattern also matched elsewhere)?  When not, `why` says what
// was seen for each of them.
bool ResolveGameCameraCode(const GameCameraScan& scan, GameCameraCode& out, char* why, size_t whySize);
