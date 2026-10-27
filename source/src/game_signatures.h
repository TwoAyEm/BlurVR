// Finding the game's camera hand-over functions in the code of Blur.exe by what they look like, not by where they are (NOTES.md section 25).
// Plain C++ on a block of memory, so the same code runs in the proxy on the loaded module and in tools/camera_sig_check on a file.
#pragma once
#include <stddef.h>

// What a scan of the executable sections has found so far.  Feed every executable section to ScanForGameCameraCode, then ask
// ResolveGameCameraCode.
enum { kMaxScanBlocks = 8 };
struct GameCameraScan {
    // [function][form]: function 0 = SetCameraData, 1 = SetCamera; form 0 = tight (only call targets and absolute addresses are
    // wildcards), 1 = loose (every displacement and immediate is a wildcard too)
    unsigned count[2][2];
    const unsigned char* first[2][2];
    // The blocks that were searched, kept for the second way of finding SetCamera (through the calls to SetCameraData, below).
    struct Block {
        const unsigned char* code;
        size_t size;
        unsigned long long va; // the address the block has in the game's memory (in a file read by a tool that is not where `code` points)
    } blocks[kMaxScanBlocks];
    unsigned blockCount;
};

// The two functions, once found.  Both are __thiscall members of the render view class: SetCameraData(view, T) and
// SetCamera(view, camera, flag).
struct GameCameraCode {
    const unsigned char* setCameraData;
    const unsigned char* setCamera;
    size_t setCameraDataStolen, setCameraStolen; // whole instructions at the start of each function, without relative operands, for the hook
    size_t cameraBytes;                          // size of the camera description SetCamera copies, read from its code
    int dataForm, cameraForm;                    // 0 = tight pattern, 1 = loose pattern, 2 = found through the calls to SetCameraData
    // cameraForm 2 only.  A build whose code was obfuscated can have moved SetCamera's body elsewhere and left a jump stub at its address, which is what the
    // game's calls go through; the hook then rewrites the stub's target and leaves the stub alone (setCameraStolen is 0 and `setCamera` is the stub).
    int cameraStub;                              // 0 = SetCamera itself is there (hooked at its start), 1 = jmp [slot] (the slot is rewritten), 2 = jmp rel32, 3 = push imm32 / ret
    const unsigned char* cameraOperand;          // cameraStub != 0: the 4 bytes that name where the stub goes (the slot's value, the rel32, the imm32)
    unsigned cameraBody;                         // cameraStub != 0: the address they name now, which is the function's body
    char how[200];                               // cameraForm 2: a line for the log saying what was found
};

void InitGameCameraScan(GameCameraScan& scan);
// Search one block of code (`va` = where it is in the game's memory when that is not `code`).  Matches are counted over all blocks.
void ScanForGameCameraCode(const unsigned char* code, size_t size, GameCameraScan& scan, unsigned long long va = 0);
// Both functions found exactly once (a tight match counts when the loose pattern also matched elsewhere)?  When SetCameraData is found and SetCamera is not
// (a build that rewrote the function), SetCamera is looked for through the calls to SetCameraData: the function the code around those calls calls too.
// When not found, `why` says what was seen for each of them.
bool ResolveGameCameraCode(const GameCameraScan& scan, GameCameraCode& out, char* why, size_t whySize);
// That second way on its own (ResolveGameCameraCode uses it by itself; a tool can ask for it on a build whose SetCamera was found by its bytes, to compare).
// `setCameraData` is the function found by the scan; `note` says what it saw.
bool FindSetCameraThroughCallSites(const GameCameraScan& scan, const unsigned char* setCameraData, GameCameraCode& out, char* note, size_t noteSize);

// The AI HUD gadget's per-frame function (UIAiHud, NOTES.md section 26): a __thiscall member without arguments that places each AI car's marker in
// the world.  One pattern (call targets are the only wildcards); the function is used when it matches exactly once.
struct AiHudScan {
    unsigned count;
    const unsigned char* first;
    size_t available; // bytes of the code block from the first match to its end
    unsigned viewportCount; // the same for the HUD root's update (HudDummy, which sways the whole HUD with the car's acceleration)
    const unsigned char* viewportFirst;
    size_t viewportAvailable;
};
struct AiHudCode {
    const unsigned char* draw;
    size_t drawStolen; // whole instructions at the start of the function, without relative operands, for the hook
    // Two places inside the function (NULL when the bytes around them are not the expected ones, which leaves the markers where the game puts them):
    const unsigned char* planeJump; // the 6-byte `ja` that skips a car behind the camera's forward plane
    const unsigned char* rootStore; // the 5 bytes (mov ecx,[esi+0x14] / test ecx,ecx) right after the marker's root element got its world position
    const unsigned char* billboardFlag; // the game's byte UI.HUD.BillboardToCarInsteadOfCamera: the address in the `cmp byte [flag],0` that picks the marker's yaw (NULL when the bytes there are not the expected ones)
    const unsigned char* viewportUpdate; // HudDummy's update (NULL when it was not found exactly once or its end is not the expected one)
    size_t viewportStolen;               // whole instructions at its start, without relative operands, for a hook
};
void InitAiHudScan(AiHudScan& scan);
void ScanForAiHudCode(const unsigned char* code, size_t size, AiHudScan& scan);
bool ResolveAiHudCode(const AiHudScan& scan, AiHudCode& out, char* why, size_t whySize);
