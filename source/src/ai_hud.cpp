// The game's AI HUD (UIAiHud): the markers over the AI cars (name, weapon, health, lock-on).  The game places each one in the world above its car and draws it in 2D;
// with the VR half running each one is shown over its car instead (NOTES.md section 26): a hook on the game's update and a detour inside it make the game draw each
// marker in a slot of its own on the HUD layer, and AiHudComposeMarkers cuts the slots out of the layer and draws them as billboards at the cars.
#include "ai_hud.h"
#include <stddef.h>
#include <algorithm>
#include "config.h"
#include "core.h"
#include "game_camera.h"
#include "d3d_hooks.h"
#include "game_signatures.h"
#include "hud.h"
#include "inline_hook.h"
#include "render_state.h"
#include "vr_runtime.h"

// The detour in the middle of the game's update (SiteHandle): an assembly stub, at the end of this file, that saves the registers, calls AiHudSiteCall with
// them and resumes through the trampoline that runs the instructions the detour took.
extern "C" {
void* g_aiSiteResume = NULL;
void ai_hud_site_stub();
void AiHudSiteCall(unsigned* registers);
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The game's structures, as read from the Steam build (NOTES.md section 26).  The gadget object has a vector of per-car slots at +0x1B4
// (begin pointer; capacity at +0x1B8, size at +0x1BC), 0x34 bytes each: dword 0 is the root UI element of the car's marker, dwords 1..12 are
// its child elements.  The slot index is the car index.  An element is a UI gadget; the fields below are the ones the game's update writes.
// ---------------------------------------------------------------------------------------------------------------------------------
const size_t kSlotVectorBegin = 0x1B4, kSlotVectorSize = 0x1BC, kSlotBytes = 0x34;
const size_t kGadgetBytes = 0x1D0; // how much of the gadget object is read
const int kChildCount = 12;
const int kChildPerks = 2, kChildDamage = 3; // indexes in the slot's children (ai_perks, ai_damage)
const int kMaxSlots = 24;
// offset in the slot of each child: ai_name, ai_position, ai_perks, ai_damage, ai_targeting, ai_targeting_left, ai_targeting_right, ai_winner_icon, ai_rival_icon,
// ai_rival_arrow, ai_winner_pos, ai_team_icon (what the game's template-clone code stores for each name)
const size_t kChildOffset[kChildCount] = {0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30};

enum ElementField : size_t {
    E_Alpha = 0x58,
    E_Pos = 0x70, // x, y, z
    E_Flags = 0xB0,
    E_Bytes = 0x150 // how much of an element is read
};

template <typename T>
T Rd(const BYTE* base, size_t offset)
{
    T value;
    memcpy(&value, base + offset, sizeof(T));
    return value;
}

// Is [p, p+n) readable memory?  The game's objects are read without a structured exception handler, so every pointer is checked first.
bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable))
        return false;
    return (const BYTE*)p + n <= (const BYTE*)info.BaseAddress + info.RegionSize;
}

struct ElementInfo {
    bool present;
    float alpha, pos[3];
    unsigned flags;
};

bool ReadElement(uintptr_t address, ElementInfo& e)
{
    memset(&e, 0, sizeof(e));
    if (!address || !Readable((const void*)address, E_Bytes))
        return false;
    const BYTE* p = (const BYTE*)address;
    e.present = true;
    e.alpha = Rd<float>(p, E_Alpha);
    for (int i = 0; i < 3; i++)
        e.pos[i] = Rd<float>(p, E_Pos + 4 * i);
    e.flags = Rd<unsigned>(p, E_Flags);
    return true;
}

struct Marker {
    int car;
    bool fresh; // the game's update wrote this marker during the call (seen by the detour; without it: the root's world position, alpha or flags changed)
    bool drawn; // the game draws this marker this frame (from the detour: one of the nearest few and alpha above 0; without it: the elements' flags)
    int slot;   // the slot of the HUD layer the marker was moved to, -1 = it stayed where the game put it
    ElementInfo root; // (its world position is where the car is, also when the marker was moved)
    ElementInfo child[kChildCount]; // (only ai_perks and ai_damage are read: whether the game drew the marker)
};

// What the detour saw of one car's marker while the game's update was running.
struct SiteEntry {
    int car;
    float real[3]; // the world position the game wrote
    float alpha;
    bool willDraw; // the game goes on to draw it
    int slot;      // the slot it was moved to, -1 = left alone
};

// What one call of the game's function left behind.
struct Snapshot {
    unsigned long long frame; // g_vr.frameCount at the call
    int siteCount; // cars the detour saw
    SiteEntry site[kMaxSlots];
    bool camKnown;        // the camera below is known: the one the detour placed the slots with
    bool gameCam;         // ... and it is the game's own camera for the view, read from the update's stack (else the last one SetCameraData gave)
    MainCameraPose cam;   // (projX / projY: the scales the slots were placed with)
    int count;     // markers below
    Marker marker[kMaxSlots];
};

// The root's values before the game's update ran: a slot the update did not touch keeps what it had (cars behind the camera, cars past the
// draw limit), and only the difference tells.
struct Before {
    bool ok;
    float pos[3], alpha;
    unsigned flags;
};

const unsigned kFlagDirty = 0x40000; // set on a child element when the game's update starts on its car, cleared once the element has been drawn

// Does the game draw this car's marker this frame, going by the flags?  Its update writes the marker of every car that passes the early tests and then draws only
// the nearest few (five in the Steam build: in the log, the cars whose ai_perks / ai_damage elements had the flag cleared were always the five nearest); the others
// stay flagged.  A marker with alpha 0 is not drawn either.  (The detour knows this directly; the flags are what is left without it, and what it is checked against.)
bool MarkerDrawn(const Marker& m)
{
    if (!m.root.present || !m.fresh || !(m.root.alpha > 0.002f))
        return false;
    const ElementInfo& perks = m.child[kChildPerks];
    const ElementInfo& damage = m.child[kChildDamage];
    return (perks.present && !(perks.flags & kFlagDirty)) || (damage.present && !(damage.flags & kFlagDirty));
}

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
Snapshot g_work;   // the hook's thread only
Snapshot g_latest; // under g_lock
Snapshot g_view;   // the presenting thread only
bool g_haveLatest = false;

typedef void(__attribute__((fastcall)) * AiHudDrawFn)(void* self, void* edx);
AiHudDrawFn g_origDraw = NULL;
bool g_hooked = false;
int g_insane = 0;
bool g_layoutLogged = false;

// ---- the detour and the markers it moves (see "moving the markers" below)
const int kRelocSlots = 8;       // slots on the HUD layer that markers are moved to
bool g_siteInstalled = false;    // the detour is in the game's code
bool g_siteActive = false;       // the game's update is running: the detour does something only then
bool g_siteBroken = false;       // the game's stack at the detour was not as expected: it records and moves nothing
int g_siteMismatch = 0;
const BYTE* g_planeJump = NULL;  // the game's skip of cars behind the camera, and the 6 bytes that were there
BYTE g_planeOriginal[6];
bool g_planePatched = false;
BYTE* g_billboardFlag = NULL; // UI.HUD.BillboardToCarInsteadOfCamera: while the world markers are on it is held off during the update, so the markers face the camera
int g_siteChecks = 0, g_siteDisagree = 0; // the detour's idea of "drawn" against the elements' flags
const int kMaxCars = 64;
bool g_pickValid = false; // PlanPicks chose which cars get a marker in this update, and which cars it knew of
bool g_pickKnown[kMaxCars], g_pickWanted[kMaxCars];

// ---- the HUD's sway: HudDummy, the HUD's root gadget, moves, scales and turns the whole HUD with the car's acceleration (the UI.Viewport.* tweakables, up to 5 % of
// scale), and the markers' pictures carry that with them.  Its update is hooked and its outputs are put back to rest values while the world markers are on.
typedef void(__attribute__((fastcall)) * HudSwayFn)(void* self, void* edx);
HudSwayFn g_origSway = NULL;
bool g_swayLogged = false;

void SlotsBeginUpdate(); // (the slot allocator, below)
void PlanPicks(const BYTE* self); // (which cars get a marker, below)

bool SlotArray(const BYTE* self, const BYTE*& begin, int& count)
{
    if (!Readable(self, kGadgetBytes))
        return false;
    begin = (const BYTE*)Rd<uintptr_t>(self, kSlotVectorBegin);
    count = Rd<int>(self, kSlotVectorSize);
    if (!begin || count < 0 || count > 64)
        return false;
    return count == 0 || Readable(begin, (size_t)count * kSlotBytes);
}

int CaptureBefore(const BYTE* self, Before* before)
{
    const BYTE* begin;
    int count;
    if (!SlotArray(self, begin, count))
        return 0;
    int n = std::min(count, kMaxSlots);
    for (int i = 0; i < n; i++) {
        ElementInfo root;
        before[i].ok = ReadElement(Rd<uintptr_t>(begin, (size_t)i * kSlotBytes), root);
        if (!before[i].ok)
            continue;
        before[i].alpha = root.alpha;
        before[i].flags = root.flags;
        for (int k = 0; k < 3; k++)
            before[i].pos[k] = root.pos[k];
    }
    return n;
}

bool Finite(float v)
{
    return v == v && v > -1e9f && v < 1e9f;
}

// Reads what the update left behind: the root of every car's marker and the two children (ai_perks, ai_damage) that tell whether the game drew it.
void Harvest(const BYTE* self, const Before* before, int beforeCount)
{
    const BYTE* begin;
    int count;
    if (!SlotArray(self, begin, count)) {
        static int logged = 0;
        if (logged++ < 3)
            Log("ai hud: the gadget at %p does not look like the expected one (slot vector unreadable or too big), nothing read", self);
        return;
    }
    Snapshot& s = g_work;
    s.frame = g_vr.frameCount;
    s.count = 0;
    int sane = 0, insane = 0;
    for (int i = 0; i < std::min(count, kMaxSlots); i++) {
        Marker& m = s.marker[s.count];
        memset(&m, 0, sizeof(m));
        m.car = i;
        if (!ReadElement(Rd<uintptr_t>(begin, (size_t)i * kSlotBytes), m.root))
            continue;
        const Before* b = i < beforeCount ? &before[i] : NULL;
        m.fresh = !b || !b->ok || b->alpha != m.root.alpha || b->flags != m.root.flags || b->pos[0] != m.root.pos[0] ||
                  b->pos[1] != m.root.pos[1] || b->pos[2] != m.root.pos[2];
        m.slot = -1;
        const SiteEntry* site = NULL; // (the detour saw the cars the update went on to draw the elements of: they are the ones it wrote)
        for (int k = 0; k < s.siteCount; k++)
            if (s.site[k].car == i)
                site = &s.site[k];
        if (site) {
            m.fresh = true;
            m.slot = site->slot;
            for (int k = 0; k < 3; k++)
                m.root.pos[k] = site->real[k]; // (where the car is, also when the marker was moved)
        }
        if (Finite(m.root.pos[0]) && Finite(m.root.pos[1]) && Finite(m.root.pos[2]) && Finite(m.root.alpha))
            sane++;
        else
            insane++;
        const BYTE* slot = begin + (size_t)i * kSlotBytes;
        for (int c = 0; c < kChildCount; c++)
            if (c == kChildPerks || c == kChildDamage)
                ReadElement(Rd<uintptr_t>(slot, kChildOffset[c]), m.child[c]);
        m.drawn = site ? site->willDraw : MarkerDrawn(m);
        if (site) {
            g_siteChecks++;
            if (site->willDraw != MarkerDrawn(m))
                g_siteDisagree++;
        }
        s.count++;
    }
    if (g_siteChecks >= 400 && g_siteDisagree * 4 > g_siteChecks && !g_siteBroken) { // the detour's idea of "drawn" is not what the elements show: do not trust it
        g_siteBroken = true;
        Log("ai hud: the detour's idea of which markers are drawn disagrees with the game's elements in %d of %d cases: markers are left where the game puts them", g_siteDisagree,
            g_siteChecks);
    }
    static bool agreementLogged = false;
    if (!agreementLogged && g_siteChecks >= 200) {
        agreementLogged = true;
        Log("ai hud: the detour's idea of which markers are drawn agrees with the game's elements in %d of %d cases", g_siteChecks - g_siteDisagree, g_siteChecks);
    }
    if (insane && !sane) {
        if (++g_insane == 30)
            Log("ai hud: the markers' world positions are not numbers, the layout of this build is not the expected one");
    }
    if (!g_layoutLogged && sane) {
        g_layoutLogged = true;
        Log("ai hud: first update seen: gadget %p, %d slots at %p", self, count, begin);
    }
    EnterCriticalSection(&g_lock);
    memcpy(&g_latest, &s, offsetof(Snapshot, marker) + (size_t)s.count * sizeof(Marker));
    g_haveLatest = true;
    LeaveCriticalSection(&g_lock);
}

// The markers are shown over the cars while the VR half runs: Stereo=1 and the headset's runtime came up (without it nothing would compose the markers, and the game's
// own HUD is left alone).
bool MarkersOn()
{
    return g_cfg.vr && g_vr.active;
}

// HudDummy's update: the game works out the sway, and its outputs are put back to the rest values it started from: position (+0x190, +0x194), rotation (+0x19C),
// scale (+0x198).
void __attribute__((fastcall)) H_HudSway(void* self, void* edx)
{
    g_origSway(self, edx);
    if (!MarkersOn())
        return;
    BYTE* g = (BYTE*)self;
    if (!Readable(g, 0x1A0))
        return;
    const float pos[2] = {Rd<float>(g, 0x70), Rd<float>(g, 0x74)}, rot = Rd<float>(g, 0x84), scale = Rd<float>(g, 0x88);
    const float rest[4] = {Rd<float>(g, 0x190), Rd<float>(g, 0x194), Rd<float>(g, 0x19C), Rd<float>(g, 0x198)};
    // only values that look like a rest layout are written (a scale around 1, finite numbers)
    if (!(rest[3] > 0.5f && rest[3] < 2.0f) || !Finite(rest[0]) || !Finite(rest[1]) || !Finite(rest[2]))
        return;
    memcpy(g + 0x70, &rest[0], 4);
    memcpy(g + 0x74, &rest[1], 4);
    memcpy(g + 0x84, &rest[2], 4);
    memcpy(g + 0x88, &rest[3], 4);
    memcpy(g + 0x8C, &rest[3], 4);
    if (!g_swayLogged) {
        g_swayLogged = true;
        Log("world markers: the HUD's sway is held at rest (the game's own, first seen: position %.2f %.2f, rotation %.3f, scale %.3f; rest %.2f %.2f, %.3f, %.3f)", pos[0], pos[1], rot,
            scale, rest[0], rest[1], rest[2], rest[3]);
    }
}

void __attribute__((fastcall)) H_AiHudDraw(void* self, void* edx)
{
    if (!MarkersOn()) {
        g_origDraw(self, edx);
        return;
    }
    static Before before[kMaxSlots];
    int n = CaptureBefore((const BYTE*)self, before);
    g_work.siteCount = 0;
    g_work.camKnown = g_work.gameCam = false;
    SlotsBeginUpdate();
    PlanPicks((const BYTE*)self);
    // The game turns a marker to face its car's direction (UI.HUD.BillboardToCarInsteadOfCamera, on in the Steam build: the yaw of a marker follows the car's heading, not
    // the camera's), so a marker in its slot is seen at an angle whenever its car does not point where the camera does.  Held off for the update, the yaw is the
    // camera's and every marker is flat to the screen.
    BYTE billboard = 0;
    const bool faceCamera = g_billboardFlag && (billboard = *g_billboardFlag) != 0;
    if (faceCamera) {
        *g_billboardFlag = 0;
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("world markers: the game turns its markers to face their cars (UI.HUD.BillboardToCarInsteadOfCamera was %d): held off during the update so that they face the camera", (int)billboard);
        }
    }
    g_siteActive = g_siteInstalled && !g_siteBroken;
    g_origDraw(self, edx);
    g_siteActive = false;
    if (faceCamera)
        *g_billboardFlag = billboard;
    Harvest((const BYTE*)self, before, n);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Finding and hooking the function (like the camera functions, game_camera.cpp)
// ---------------------------------------------------------------------------------------------------------------------------------
void ScanModule(AiHudScan& scan, DWORD& imageSize, DWORD& timeStamp)
{
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base)
        return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    imageSize = nt->OptionalHeader.SizeOfImage;
    timeStamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[i].Name, ".bind", 8))
            continue;
        DWORD size = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
        ScanForAiHudCode(base + sections[i].VirtualAddress, size, scan);
    }
}

void TryInstall(unsigned frame)
{
    static unsigned tries = 0, nextTry = 1;
    if (tries >= 5 || frame < nextTry)
        return;
    tries++;
    nextTry = frame + 120; // (a build that unpacks its code late is looked at again a little later)
    AiHudScan scan;
    InitAiHudScan(scan);
    DWORD imageSize = 0, timeStamp = 0;
    ScanModule(scan, imageSize, timeStamp);
    AiHudCode code;
    char why[256] = "";
    if (!ResolveAiHudCode(scan, code, why, sizeof(why))) {
        Log("ai hud: Blur.exe image 0x%x bytes, PE time stamp 0x%08x: the AI HUD code was not found (%s) [try %u of 5]", (unsigned)imageSize,
            (unsigned)timeStamp, why, tries);
        return;
    }
    tries = 5;
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    BYTE expected[32];
    memcpy(expected, code.draw, code.drawStolen);
    g_origDraw = (AiHudDrawFn)InstallInlineHook((void*)code.draw, expected, code.drawStolen, (void*)H_AiHudDraw);
    Log("ai hud: Blur.exe image 0x%x bytes, PE time stamp 0x%08x: UIAiHud update at %p %s", (unsigned)imageSize, (unsigned)timeStamp, code.draw,
        g_origDraw ? "hooked" : "NOT HOOKED");
    g_hooked = g_origDraw != NULL;
    if (!g_hooked)
        return;
    // the two places inside the function: the detour after a marker's world position is written, and the game's skip of cars behind the camera
    if (code.rootStore) {
        BYTE expectedSite[5];
        memcpy(expectedSite, code.rootStore, sizeof(expectedSite));
        g_aiSiteResume = InstallInlineHook((void*)code.rootStore, expectedSite, sizeof(expectedSite), (void*)ai_hud_site_stub);
        g_siteInstalled = g_aiSiteResume != NULL;
    }
    g_planeJump = code.planeJump;
    if (g_planeJump)
        memcpy(g_planeOriginal, g_planeJump, sizeof(g_planeOriginal));
    if (code.billboardFlag) { // (a byte of the game's data that can be written, with a value of 0 or 1)
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(code.billboardFlag, &info, sizeof(info)) && info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
            (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) && *code.billboardFlag <= 1)
            g_billboardFlag = (BYTE*)code.billboardFlag;
    }
    Log("ai hud: the marker yaw flag (UI.HUD.BillboardToCarInsteadOfCamera) at %p %s%s", code.billboardFlag, g_billboardFlag ? "found, value " : "NOT usable",
        g_billboardFlag ? (*g_billboardFlag ? "1 (the markers face their cars)" : "0") : "");
    if (code.viewportUpdate) {
        BYTE expectedSway[16];
        memcpy(expectedSway, code.viewportUpdate, code.viewportStolen);
        g_origSway = (HudSwayFn)InstallInlineHook((void*)code.viewportUpdate, expectedSway, code.viewportStolen, (void*)H_HudSway);
        Log("ai hud: the HUD's sway update at %p %s", code.viewportUpdate, g_origSway ? "hooked" : "NOT hooked");
    } else
        Log("ai hud: the HUD's sway update was not found: the markers move with the HUD's sway");
    Log("ai hud: the detour after a marker's world position is written at %p %s; the skip of cars behind the camera at %p %s", code.rootStore,
        g_siteInstalled ? "installed" : "NOT installed", code.planeJump, g_planeJump ? "found" : "NOT found");
}

bool CopyLatest(Snapshot& out)
{
    if (!g_lockReady)
        return false;
    EnterCriticalSection(&g_lock);
    bool have = g_haveLatest;
    if (have)
        memcpy(&out, &g_latest, offsetof(Snapshot, marker) + (size_t)g_latest.count * sizeof(Marker));
    LeaveCriticalSection(&g_lock);
    return have;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Where a marker's world position lands on the game's flat picture
// ---------------------------------------------------------------------------------------------------------------------------------
struct Predicted {
    bool inFront;
    double cam[3]; // the position in the camera's space
    bool ok;       // in front of the camera: ax, ay is the pixel on the HUD layer
    double ax, ay;
};

// A vector in the world, in a camera's axes.
void ToCameraSpace(const MainCameraPose& cam, const double* d, double* out)
{
    const double* axes[3] = {cam.right, cam.up, cam.fwd};
    for (int i = 0; i < 3; i++)
        out[i] = d[0] * axes[i][0] + d[1] * axes[i][1] + d[2] * axes[i][2];
}

Predicted Predict(const MainCameraPose* pose, const float* world)
{
    Predicted p;
    memset(&p, 0, sizeof(p));
    if (!pose)
        return p;
    double d[3] = {world[0] - pose->pos[0], world[1] - pose->pos[1], world[2] - pose->pos[2]};
    ToCameraSpace(*pose, d, p.cam);
    p.inFront = p.cam[2] > 0.01;
    double w = g_backBufferWidth, h = g_backBufferHeight;
    if (p.inFront && w > 0 && h > 0) {
        p.ok = true;
        p.ax = w / 2 * (1 + pose->projX * p.cam[0] / p.cam[2]);
        p.ay = h / 2 * (1 - pose->projY * p.cam[1] / p.cam[2]);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// World markers: where a marker's picture is on the HUD layer, and the billboard that carries it (NOTES.md section 26)
// ---------------------------------------------------------------------------------------------------------------------------------
struct MarkerParams {
    double left, right, up, down;                   // metres around the anchor, in the plane of the picture, that are cut out of the HUD layer
    double scale, minAngleDeg, maxAngleDeg;         // size factor; how small / large the marker's width may look
    double minDistance;                             // nearer than this (metres) a marker is not shown in 3D
    double lift;                                    // metres above the anchor that the picture starts; it is scaled about that height
};

// The values were settings while the feature was tuned in the headset; these are the ones that worked.
const double kSlotDepthMetres = 14.0; // how large a marker is drawn in its slot (nearer = larger and sharper)
const double kBandY = 0.5;            // the height of the slots on the HUD layer, as a fraction of it (the middle, where no other UI is)
const double kPickAngleDeg = 80.0;    // the cone around where the head looks inside which the nearest cars get a marker

MarkerParams FixedMarkerParams()
{
    MarkerParams p;
    p.left = 1.7;
    p.right = 1.7;
    p.up = 1.3;
    p.down = 1.7;
    p.scale = 1.0;
    p.minAngleDeg = 3.0;
    p.maxAngleDeg = 20.0;
    p.minDistance = 1.5;
    p.lift = 0.12;
    return p;
}

// Where a marker's picture is on the HUD layer: the pixel of its anchor, and the depth (metres, in the game camera's space) the game drew it at.  What is
// drawn around the anchor is metres * pixels-per-metre at that depth.
struct Cut {
    double px, py, z;
    double projX, projY; // the scales of the projection the game's HUD was placed with (cot of the half angles)
};

// ---- moving the markers
// The game draws a marker where its car is, so its picture can sit on top of other UI (a marker at 4 m is 1500 px wide on the 4032 px layer), a car beside or
// behind the camera has none (the game skips it, and it would be drawn outside the layer), and the picture is as small as the car is far.  So while the game's
// update runs, a detour right after it wrote a marker's world position moves the marker's root element to a slot: a place on the layer, in a band that nothing
// else uses, at a depth that makes the picture crisp, one slot per marker (the ones the game goes on to draw).  The marker is drawn there, alone; its real
// position is kept, and AiHudComposeMarkers puts the picture over the car.  The game's skip of cars behind the camera is switched off while this runs.
bool RelocationWanted()
{
    return MarkersOn() && g_siteInstalled && !g_siteBroken;
}

// The depth the slots are at: deep enough for all of them to stand side by side on the layer.
double SlotDepth(const MarkerParams& p, const MainCameraPose& pose)
{
    const double W = g_backBufferWidth;
    const double fit = (p.left + p.right) * (W / 2) * pose.projX / std::max(1.0, W / kRelocSlots - 3.0); // (a slot is 2 pixels wider than its cut-out, so that two never touch)
    return std::max(kSlotDepthMetres, fit);
}

Cut SlotCut(const MainCameraPose& pose, const MarkerParams& p, int slot)
{
    const double W = g_backBufferWidth, H = g_backBufferHeight, z = SlotDepth(p, pose);
    const double pxX = W / 2 * pose.projX / z, slotWidth = (p.left + p.right) * pxX + 2.0;
    const double centre = W / 2 + (slot - (kRelocSlots - 1) / 2.0) * slotWidth;
    Cut c;
    c.z = z;
    c.projX = pose.projX;
    c.projY = pose.projY;
    c.px = centre - (p.right - p.left) / 2 * pxX; // (the cut-out is centred on the slot)
    c.py = kBandY * H;
    return c;
}

// The world position that the game's camera sees at the cut's pixel and depth.
void CutWorld(const MainCameraPose& pose, const Cut& c, float* world)
{
    const double W = g_backBufferWidth, H = g_backBufferHeight;
    const double x = (c.px / (W / 2) - 1) * c.z / pose.projX, y = (1 - c.py / (H / 2)) * c.z / pose.projY;
    for (int k = 0; k < 3; k++)
        world[k] = (float)(pose.pos[k] + pose.right[k] * x + pose.up[k] * y + pose.fwd[k] * c.z);
}

// Which slot a car's marker is in.  A car keeps its slot for as long as its marker is drawn, so that a change in the order of the nearest cars moves no
// picture; new cars take the free slots nearest the middle first (the middle is where the game's own projection is most exact), and not a slot that a car had a
// moment ago unless there is no other.
const int kSlotOrder[kRelocSlots] = {3, 4, 2, 5, 1, 6, 0, 7}; // positions, from the middle out
int g_slotOfCar[kMaxCars], g_prevSlotOfCar[kMaxCars];
int g_slotOwner[kRelocSlots], g_prevSlotOwner[kRelocSlots];
bool g_slotsReady = false;

void SlotsBeginUpdate()
{
    if (!g_slotsReady) {
        for (int i = 0; i < kMaxCars; i++)
            g_slotOfCar[i] = -1;
        for (int i = 0; i < kRelocSlots; i++)
            g_slotOwner[i] = -1;
        g_slotsReady = true;
    }
    memcpy(g_prevSlotOfCar, g_slotOfCar, sizeof(g_slotOfCar));
    memcpy(g_prevSlotOwner, g_slotOwner, sizeof(g_slotOwner));
    for (int i = 0; i < kMaxCars; i++)
        g_slotOfCar[i] = -1;
    for (int i = 0; i < kRelocSlots; i++)
        g_slotOwner[i] = -1;
}

int SlotForCar(int car)
{
    if (car < 0 || car >= kMaxCars || !g_slotsReady)
        return -1;
    int had = g_prevSlotOfCar[car];
    if (had >= 0 && g_slotOwner[had] < 0) {
        g_slotOwner[had] = car;
        return g_slotOfCar[car] = had;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < kRelocSlots; i++) {
            int slot = kSlotOrder[i];
            if (g_slotOwner[slot] >= 0 || (pass == 0 && g_prevSlotOwner[slot] >= 0 && g_prevSlotOwner[slot] != car))
                continue;
            g_slotOwner[slot] = car;
            return g_slotOfCar[car] = slot;
        }
    return -1;
}

// The head's position and the direction it looks in, in the game camera's space (`head` is eyeCameraDelta[2]: the head's pose there; its columns are its axes).
void HeadPlacement(const Matrix4& head, double* position, double* look)
{
    for (int k = 0; k < 3; k++)
        position[k] = -(head.m[3][0] * head.m[k][0] + head.m[3][1] * head.m[k][1] + head.m[3][2] * head.m[k][2]);
    look[0] = head.m[0][2];
    look[1] = head.m[1][2];
    look[2] = head.m[2][2];
}

// ---- which cars get a marker
// The game draws a marker for the nearest few cars (its draw limit, `this+0x194`, 5 in the Steam build) in its own order, the cars nearest the player's car.  In the
// headset what matters is what is near the head in the direction it looks, so while the markers are moved the detour decides: before the update the cars of the last
// one are ranked by their distance from the head, the ones inside a cone around the head's view direction (kPickAngleDeg) are eligible, the nearest `limit` of
// them are wanted (a car that has a marker now counts as 15 % nearer, so that two cars at the same distance do not take turns), and at each car the detour makes the game's own
// draw test agree: it compares the number of markers drawn so far, a counter on its stack, with the limit.  A rival or winner marker is drawn past the limit as before.
void PlanPicks(const BYTE* self)
{
    g_pickValid = false;
    memset(g_pickKnown, 0, sizeof(g_pickKnown));
    memset(g_pickWanted, 0, sizeof(g_pickWanted));
    if (!RelocationWanted() || !g_vr.poseValid)
        return;
    const int limit = Rd<int>(self, 0x194);
    if (limit <= 0 || limit > kMaxSlots)
        return;
    MainCameraPose pose;
    static Snapshot last;
    if (!GetMainCameraPose(pose) || pose.frame + 1 < g_vr.frameCount || !CopyLatest(last))
        return;
    double hp[3], look[3];
    HeadPlacement(g_vr.eyeCameraDelta[2], hp, look);
    const double minCos = cos(kPickAngleDeg * 3.14159265358979 / 180.0), minDistance = FixedMarkerParams().minDistance;
    const double* from = last.camKnown && last.gameCam ? last.cam.pos : pose.pos; // (as BuildMarkers puts the cars in the camera that is drawn)
    struct Candidate {
        int car;
        double key;
    } list[kMaxSlots];
    int n = 0, known = 0;
    for (int i = 0; i < last.count; i++) {
        const Marker& m = last.marker[i];
        if (!m.fresh || !m.root.present || m.car < 0 || m.car >= kMaxCars)
            continue;
        const double d[3] = {m.root.pos[0] - from[0], m.root.pos[1] - from[1], m.root.pos[2] - from[2]};
        double a[3];
        ToCameraSpace(pose, d, a);
        const double v[3] = {a[0] - hp[0], a[1] - hp[1], a[2] - hp[2]};
        const double dist = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(dist == dist))
            continue;
        g_pickKnown[m.car] = true;
        known++;
        if (dist < minDistance || (v[0] * look[0] + v[1] * look[1] + v[2] * look[2]) / dist < minCos)
            continue;
        list[n].car = m.car;
        list[n].key = m.drawn ? dist * 0.85 : dist;
        n++;
    }
    if (!known)
        return;
    std::sort(list, list + n, [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    for (int i = 0; i < n && i < limit; i++)
        g_pickWanted[list[i].car] = true;
    g_pickValid = true;
}

// The camera the game places the markers with.  The update begins by working out the camera of the view it draws for: a 3x3 rotation matrix and a position, as
// locals at +0x98 and +0xBC of its stack frame (it uses them for the yaw of the markers, the plane test of cars behind the camera and the distance fade), and the car
// positions it writes are the cars' positions at that time.  The camera that SetCameraData last gave is not necessarily the same one: when the update runs before
// this frame's picture is set up it is the previous picture's, up to a frame behind, which at 70 m/s is a metre or two, and at the slots' depth of 14 m that is a zoom
// of 7 to 17 percent of everything the markers were placed with (seen in the dumps).  `ref` (SetCameraData's) says
// that it is a camera at all (the rows of the matrix are the camera's axes: the update reads the forward axis from the third row for the markers' yaw, atan2 of its x
// and z).
const size_t kGameMatrix = 0x98, kGamePosition = 0xBC, kGameCameraBytes = 0xC8;

bool ReadGameCamera(const BYTE* sp, const MainCameraPose& ref, MainCameraPose& out, const char*& why)
{
    if (!Readable(sp, kGameCameraBytes)) {
        why = "the stack is not readable";
        return false;
    }
    float m[9], p[3];
    for (int i = 0; i < 9; i++)
        m[i] = Rd<float>(sp, kGameMatrix + 4 * i);
    for (int i = 0; i < 3; i++)
        p[i] = Rd<float>(sp, kGamePosition + 4 * i);
    for (int i = 0; i < 9; i++)
        if (!Finite(m[i]) || fabsf(m[i]) > 1.01f) {
            why = "the matrix is not a rotation";
            return false;
        }
    for (int i = 0; i < 3; i++)
        if (!Finite(p[i])) {
            why = "the position is not a number";
            return false;
        }
    const double* refAxes[3] = {ref.right, ref.up, ref.fwd};
    double* outAxes[3] = {out.right, out.up, out.fwd};
    for (int a = 0; a < 3; a++) {
        double axis[3], dot = 0;
        for (int k = 0; k < 3; k++) {
            axis[k] = m[a * 3 + k];
            dot += axis[k] * refAxes[a][k];
        }
        const double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (length < 0.98 || length > 1.02) {
            why = "the rows of the matrix are not unit vectors";
            return false;
        }
        if (dot < 0.9) { // (a camera that is more than 25 degrees from the one SetCameraData gave is another camera, or not one)
            why = "its axes are not those of the camera SetCameraData gave";
            return false;
        }
        for (int k = 0; k < 3; k++)
            outAxes[a][k] = axis[k];
    }
    const double dx = p[0] - ref.pos[0], dy = p[1] - ref.pos[1], dz = p[2] - ref.pos[2];
    if (dx * dx + dy * dy + dz * dz > 150.0 * 150.0) {
        why = "its position is far from the camera SetCameraData gave";
        return false;
    }
    for (int k = 0; k < 3; k++)
        out.pos[k] = p[k];
    out.projX = ref.projX; // (the projection is SetCameraData's: the scales of the UI's own camera are not known)
    out.projY = ref.projY;
    out.frame = ref.frame;
    return true;
}

// The camera the markers of this update are placed with: the game's own when it can be read, else the last one SetCameraData gave.
void NoteCamera(const BYTE* sp)
{
    Snapshot& s = g_work;
    s.camKnown = s.gameCam = false;
    MainCameraPose pose;
    if (!GetMainCameraPose(pose))
        return;
    if (pose.frame + 1 < g_vr.frameCount) // (one from more than a frame ago is not a camera to place anything by)
        return;
    s.camKnown = true;
    s.cam = pose;
    MainCameraPose game;
    const char* why = "";
    if (ReadGameCamera(sp, pose, game, why)) {
        s.cam = game;
        s.gameCam = true;
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("world markers: the game's own camera is read from the update's stack, the markers are placed with it");
        }
    } else {
        static int failures = 0;
        if (failures++ < 4)
            Log("world markers: the game's own camera on the update's stack was not usable (%s): the camera SetCameraData gave is used", why);
    }
}

// The detour (see the stub at the end of this file): the game's update has just written a car's marker position into its root element and is about to
// draw the elements.  `r` is what the stub saved, in the order of pushal: edi (the gadget), esi (the car's slot), ebp, esp (4 below the game's: the flags were
// pushed first), ebx, edx, ecx, eax.  The game's own locals are read from its stack: the car index at +0x3C, the number of markers drawn so far at +0x30,
// the marker's alpha at +0x20 and the two "rival" / "winner" bytes at +0x1A / +0x1B, which are what its draw limit (the gadget's +0x194) is tested against.
void SiteHandle(const unsigned* r)
{
    if (!g_siteActive)
        return;
    const BYTE* self = (const BYTE*)r[0];
    const BYTE* slot = (const BYTE*)r[1];
    const BYTE* sp = (const BYTE*)(r[3] + 4);
    const BYTE* begin;
    int count;
    if (!SlotArray(self, begin, count) || slot < begin || slot >= begin + (size_t)count * kSlotBytes || (size_t)(slot - begin) % kSlotBytes ||
        !Readable(sp, 0x40))
        return;
    const int car = (int)((slot - begin) / kSlotBytes);
    if (Rd<int>(sp, 0x3C) != car) { // the game's stack is not laid out as expected: do nothing, and give up after a few
        if (++g_siteMismatch == 4) {
            g_siteBroken = true;
            Log("ai hud: at the detour the game's car number is not where it was expected (%d against %d): markers are left where the game puts them", Rd<int>(sp, 0x3C),
                car);
        }
        return;
    }
    BYTE* root = (BYTE*)Rd<uintptr_t>(slot, 0);
    if (!root || !Readable(root, E_Bytes) || g_work.siteCount >= kMaxSlots)
        return;
    SiteEntry& e = g_work.site[g_work.siteCount++];
    if (g_work.siteCount == 1) // (the camera is the same for every car of an update)
        NoteCamera(sp);
    e.car = car;
    for (int k = 0; k < 3; k++)
        e.real[k] = Rd<float>(root, E_Pos + 4 * k);
    e.alpha = Rd<float>(sp, 0x20);
    const int drawnSoFar = Rd<int>(sp, 0x30), limit = Rd<int>(self, 0x194);
    bool withinLimit = drawnSoFar < limit;
    if (g_pickValid && limit > 0 && car >= 0 && car < kMaxCars) {
        // the cars that PlanPicks chose (a car it did not know of is drawn from the next update on): the game's own test is made to say the same, its counter of the
        // markers drawn so far is put below the limit for a wanted car and at it for the others
        withinLimit = g_pickKnown[car] && g_pickWanted[car];
        const int counter = withinLimit ? 0 : limit;
        memcpy(const_cast<BYTE*>(sp) + 0x30, &counter, sizeof(counter));
    }
    e.willDraw = e.alpha != 0.0f && (withinLimit || sp[0x1B] != 0 || sp[0x1A] != 0);
    e.slot = -1;
    if (!e.willDraw || !RelocationWanted() || !Finite(e.real[0]) || !Finite(e.real[1]) || !Finite(e.real[2]) || !g_backBufferWidth || !g_backBufferHeight)
        return;
    if (!g_work.camKnown)
        return;
    const MainCameraPose& cam = g_work.cam;
    const int slotIndex = SlotForCar(car);
    if (slotIndex < 0)
        return;
    float world[3];
    CutWorld(cam, SlotCut(cam, FixedMarkerParams(), slotIndex), world);
    memcpy(root + E_Pos, world, sizeof(world));
    e.slot = slotIndex;
}

// Switch the game's skip of cars behind the camera off while the markers are moved (and on again when they are not): the 6 bytes of its `ja` become a nop.
void UpdatePlanePatch()
{
    if (!g_planeJump || g_planePatched == RelocationWanted())
        return;
    const bool want = RelocationWanted();
    static const BYTE nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    DWORD old = 0;
    if (!VirtualProtect((void*)g_planeJump, 6, PAGE_EXECUTE_READWRITE, &old))
        return;
    memcpy((void*)g_planeJump, want ? nop6 : g_planeOriginal, 6);
    VirtualProtect((void*)g_planeJump, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_planeJump, 6);
    g_planePatched = want;
    Log("world markers: cars beside and behind the camera %s", want ? "are handled now (the game's skip of them is off)" : "are skipped by the game again");
}

struct MarkerGeom {
    int car;
    double dist;      // distance from the head
    int rect[4];      // x0, y0, x1, y1 on the HUD layer: the part that is cut out
    float uv[4];      // the same as u0, v0, u1, v1
    double corner[4][3]; // top left, top right, bottom left, bottom right, in the game camera's space
};

// The cut-out around a marker's anchor and the billboard that shows it.  The game draws a marker as a billboard of real size (about a metre and a half
// wide), so what is on the HUD layer around the cut's pixel is that billboard seen from the game's camera at the cut's depth: its pixels are metres *
// pixels-per-metre there.  The billboard here has the same metres, stands at the car (`anchorCam`: where the car is in the camera `pose`'s space, which is the space
// `head` is in), is upright (the game's up axis) and turned to face the head, and is then made no smaller than 3 degrees and no larger than 20 as
// seen from the head (about the anchor).  `head` is eyeCameraDelta[2]: the head's pose in the game camera's space.  The car can be anywhere around the camera.
bool BuildMarkerGeometry(const MainCameraPose& pose, const Matrix4& head, const double* anchorCam, const Cut& cut, const MarkerParams& p, MarkerGeom& out)
{
    if (!Finite((float)anchorCam[0]) || !Finite((float)anchorCam[1]) || !Finite((float)anchorCam[2]))
        return false;
    const double W = g_backBufferWidth, H = g_backBufferHeight, z = cut.z;
    if (!(W > 0 && H > 0 && z > 0))
        return false;
    const double pxX = W / 2 * cut.projX / z, pxY = H / 2 * cut.projY / z; // pixels per metre at the cut's depth
    if (!(pxX > 0 && pxY > 0))
        return false;
    const double x0 = std::max(0.0, floor(cut.px - p.left * pxX)), x1 = std::min(W, ceil(cut.px + p.right * pxX));
    const double y0 = std::max(0.0, floor(cut.py - p.up * pxY)), y1 = std::min(H, ceil(cut.py + p.down * pxY));
    if (x1 - x0 < 4 || y1 - y0 < 4)
        return false;
    // what is cut out, in metres around the anchor (after the clamp to the layer)
    const double mL = (cut.px - x0) / pxX, mR = (x1 - cut.px) / pxX, mU = (cut.py - y0) / pxY, mD = (y1 - cut.py) / pxY;
    // the head's position in the game camera's space, and the billboard's axes: the game's up, and "right" across the line of sight from the head
    double hp[3], look[3]; // (the head's forward axis in the game camera's space)
    HeadPlacement(head, hp, look);
    const double v[3] = {anchorCam[0] - hp[0], anchorCam[1] - hp[1], anchorCam[2] - hp[2]};
    const double dist = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (dist < p.minDistance)
        return false;
    const double toRad = 3.14159265358979323846 / 180.0, width = mL + mR;
    double target = width * p.scale;
    target = std::max(target, 2 * dist * tan(p.minAngleDeg * toRad / 2));
    target = std::min(target, 2 * dist * tan(p.maxAngleDeg * toRad / 2));
    const double f = target / width;
    // The billboard is flat to the head's view: perpendicular to where the head looks, so every marker has the same orientation and is face on to the viewer, whatever
    // the direction of its car.  Its up is the game's up (rolling the head does not turn it) as far as the view direction allows.
    const double world_up[3] = {pose.right[1], pose.up[1], pose.fwd[1]}; // the world's up (+y) in the camera's axes
    double r[3] = {world_up[1] * look[2] - world_up[2] * look[1], world_up[2] * look[0] - world_up[0] * look[2], world_up[0] * look[1] - world_up[1] * look[0]};
    double rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-3) { // looking straight up or down: the head's own right
        r[0] = head.m[0][0];
        r[1] = head.m[1][0];
        r[2] = head.m[2][0];
        rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    }
    for (int k = 0; k < 3; k++)
        r[k] /= rl > 1e-9 ? rl : 1.0;
    double up[3] = {look[1] * r[2] - look[2] * r[1], look[2] * r[0] - look[0] * r[2], look[0] * r[1] - look[1] * r[0]};
    const double ul = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    for (int k = 0; k < 3; k++)
        up[k] /= ul > 1e-9 ? ul : 1.0;
    // the cut-out's corners: across by (-mL, +mR) scaled about the anchor; up by (+mU, -mD) scaled about the lift (the picture's own bottom stays that far above the car)
    const double pivot = p.lift;
    const double mx[4] = {-mL * f, mR * f, -mL * f, mR * f};
    const double my[4] = {pivot + (mU - pivot) * f, pivot + (mU - pivot) * f, pivot + (-mD - pivot) * f, pivot + (-mD - pivot) * f};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            out.corner[i][k] = anchorCam[k] + r[k] * mx[i] + up[k] * my[i];
    out.dist = dist;
    out.rect[0] = (int)x0;
    out.rect[1] = (int)y0;
    out.rect[2] = (int)x1;
    out.rect[3] = (int)y1;
    out.uv[0] = (float)(x0 / W);
    out.uv[1] = (float)(y0 / H);
    out.uv[2] = (float)(x1 / W);
    out.uv[3] = (float)(y1 / H);
    return true;
}

// The markers to show in this frame, far to near.  `snap` is the game's last update, `pose` the camera of the picture that is being drawn (SetCameraData's).  A
// marker that was moved to a slot has its picture there; one the game drew where its car is has it at the car's pixel in the camera the HUD was placed with.
// That camera (`hud`: the game's own at the update when it could be read, else SetCameraData's) and the car positions are the game's at the update, and `pose` is
// the camera that is drawn: when the game's own camera was read, the car is taken at the same place relative to the camera as it was at the update.
int BuildMarkers(const Snapshot& snap, const MainCameraPose& pose, const Matrix4& head, MarkerGeom* out, int maxOut)
{
    const MarkerParams params = FixedMarkerParams();
    const MainCameraPose& hud = snap.camKnown ? snap.cam : pose;
    const double* from = snap.camKnown && snap.gameCam ? snap.cam.pos : pose.pos;
    int n = 0;
    for (int i = 0; i < snap.count && n < maxOut; i++) {
        const Marker& m = snap.marker[i];
        if (!m.drawn)
            continue;
        const double d[3] = {m.root.pos[0] - from[0], m.root.pos[1] - from[1], m.root.pos[2] - from[2]};
        double anchorCam[3];
        ToCameraSpace(pose, d, anchorCam);
        Cut cut;
        if (m.slot >= 0)
            cut = SlotCut(hud, params, m.slot);
        else {
            const Predicted pr = Predict(&hud, m.root.pos);
            if (!pr.ok)
                continue;
            cut.px = pr.ax;
            cut.py = pr.ay;
            cut.z = pr.cam[2];
            cut.projX = hud.projX;
            cut.projY = hud.projY;
        }
        MarkerGeom g;
        if (!BuildMarkerGeometry(pose, head, anchorCam, cut, params, g))
            continue;
        g.car = m.car;
        out[n++] = g;
    }
    std::sort(out, out + n, [](const MarkerGeom& a, const MarkerGeom& b) { return a.dist > b.dist; });
    return n;
}

// Show the markers of snapshot `s` (taken with camera `pose`) in both eyes, then clear what was cut out of the layer and of the game's glow of its UI.
int DrawMarkers(IDirect3DDevice9* device, IDirect3DTexture9* hudTexture, IDirect3DSurface9* hudSurface, IDirect3DSurface9* glowSurface, UINT glowWidth, UINT glowHeight,
                const Snapshot& s, const MainCameraPose& pose)
{
    MarkerGeom markers[kMaxSlots];
    int n = BuildMarkers(s, pose, g_vr.eyeCameraDelta[2], markers, kMaxSlots);
    if (!n)
        return 0;
    PFN_SetTexture setTexture = (PFN_SetTexture)g_deviceOriginals[VT_SetTexture];
    PFN_SetScissorRect setScissorRect = (PFN_SetScissorRect)g_deviceOriginals[VT_SetScissorRect];
    PFN_DrawPrimitiveUP drawPrimitiveUP = (PFN_DrawPrimitiveUP)g_deviceOriginals[VT_DrawPrimitiveUP];
    const double bw = g_backBufferWidth, bh = g_backBufferHeight, eyeW = bw / 2.0;
    setTexture(device, 0, hudTexture);
    int quads = 0;
    for (int i = 0; i < n; i++) {
        for (int eye = 0; eye < 2; eye++) {
            const Matrix4& d = g_vr.eyeCameraDelta[eye];
            const EyeBox& box = g_vr.eyeBox[eye];
            const double tanWidth = fmax(1e-3, box.right - box.left), tanHeight = fmax(1e-3, box.up + box.down);
            ScreenVertex v[4];
            bool visible = true;
            for (int c = 0; c < 4 && visible; c++) {
                const double* P = markers[i].corner[c];
                double vx = P[0] * d.m[0][0] + P[1] * d.m[1][0] + P[2] * d.m[2][0] + d.m[3][0];
                double vy = P[0] * d.m[0][1] + P[1] * d.m[1][1] + P[2] * d.m[2][1] + d.m[3][1];
                double vz = P[0] * d.m[0][2] + P[1] * d.m[1][2] + P[2] * d.m[2][2] + d.m[3][2];
                if (vz < 0.08) {
                    visible = false;
                    break;
                }
                v[c].x = (float)(eye * eyeW + (vx / vz - box.left) / tanWidth * eyeW - 0.5);
                v[c].y = (float)((box.up - vy / vz) / tanHeight * bh - 0.5);
                v[c].z = 0.0f;
                v[c].rhw = (float)(1.0 / vz);
                v[c].u = markers[i].uv[(c & 1) ? 2 : 0];
                v[c].v = markers[i].uv[(c & 2) ? 3 : 1];
            }
            if (!visible)
                continue;
            RECT scissor;
            scissor.left = (LONG)(eye * eyeW);
            scissor.right = (LONG)((eye + 1) * eyeW);
            scissor.top = 0;
            scissor.bottom = (LONG)bh;
            if (eye == 0)
                scissor.right -= g_cfg.seamGutter;
            else
                scissor.left += g_cfg.seamGutter;
            setScissorRect(device, &scissor);
            drawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, v, sizeof(ScreenVertex));
            quads++;
        }
    }
    // the flat panel must not show the markers as well: clear what was cut out, and its glow
    setTexture(device, 0, NULL);
    for (int i = 0; i < n; i++) {
        RECT r = {markers[i].rect[0], markers[i].rect[1], markers[i].rect[2], markers[i].rect[3]};
        device->ColorFill(hudSurface, &r, 0x00000000);
        if (glowSurface && glowWidth && glowHeight) { // (the glow is a blurred copy of the UI on a much smaller target: the same part of the picture, and a margin for the blur)
            const int margin = 4;
            RECT g = {(LONG)floor(markers[i].rect[0] * (double)glowWidth / bw) - margin, (LONG)floor(markers[i].rect[1] * (double)glowHeight / bh) - margin,
                      (LONG)ceil(markers[i].rect[2] * (double)glowWidth / bw) + margin, (LONG)ceil(markers[i].rect[3] * (double)glowHeight / bh) + margin};
            g.left = std::max<LONG>(g.left, 0);
            g.top = std::max<LONG>(g.top, 0);
            g.right = std::min<LONG>(g.right, (LONG)glowWidth);
            g.bottom = std::min<LONG>(g.bottom, (LONG)glowHeight);
            if (g.right > g.left && g.bottom > g.top && FAILED(device->ColorFill(glowSurface, &g, 0x00000000))) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    Log("world markers: the game's glow buffer cannot be cleared (ColorFill failed): a soft glow can stay where the markers were");
                }
            }
        }
    }
    setTexture(device, 0, hudTexture);
    static unsigned logged = 0;
    if (logged++ < 4 || logged % 3000 == 0) {
        char cars[96] = "";
        for (int i = 0; i < n && strlen(cars) < 80; i++)
            snprintf(cars + strlen(cars), sizeof(cars) - strlen(cars), "%s%d (%.0f m)", i ? ", " : "", markers[i].car, markers[i].dist);
        Log("world markers: %d placed, %d quads, %s: car %s", n, quads, RelocationWanted() ? "moved to slots" : "in place", cars);
    }
    return n;
}

} // namespace

void AiHudPoll()
{
    static unsigned frame = 0;
    if (g_hooked)
        UpdatePlanePatch(); // (also when the markers are switched off: the game's skip of cars behind the camera is put back)
    if (!g_cfg.vr)
        return;
    frame++;
    if (!g_hooked)
        TryInstall(frame);
}

void AiHudComposeMarkers(IDirect3DDevice9* device, IDirect3DTexture9* hudTexture, IDirect3DSurface9* hudSurface, IDirect3DSurface9* glowSurface, UINT glowWidth,
                          UINT glowHeight)
{
    if (!MarkersOn() || !g_hooked || !hudTexture || !hudSurface || !g_vr.poseValid || !g_backBufferWidth || !g_backBufferHeight)
        return;
    Snapshot& s = g_view;
    if (!CopyLatest(s) || s.frame != g_vr.frameCount) // (the game's update of this frame: a hidden gadget leaves old data behind)
        return;
    MainCameraPose pose;
    if (!GetMainCameraPose(pose) || pose.frame + 1 < g_vr.frameCount) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("world markers: the main camera is not known this frame (the hooks of game_camera.cpp are not installed or do not recognise it), the markers stay on the HUD panel");
        }
        return;
    }
    DrawMarkers(device, hudTexture, hudSurface, glowSurface, glowWidth, glowHeight, s, pose);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The detour's stub.  It runs in the middle of the game's update, where nothing is known about the registers, so it saves everything (flags, the general registers
// and xmm0-7), aligns the stack for the compiler's code, calls AiHudSiteCall(registers), restores everything and goes on through the trampoline, which runs the
// instructions the detour replaced and jumps back.
// ---------------------------------------------------------------------------------------------------------------------------------
extern "C" void AiHudSiteCall(unsigned* registers)
{
    SiteHandle(registers);
}

asm(".text\n"
    ".globl _ai_hud_site_stub\n"
    ".p2align 4\n"
    "_ai_hud_site_stub:\n"
    "pushfl\n"
    "pushal\n"
    "movl %esp, %ebp\n"
    "subl $0x90, %esp\n"
    "andl $-16, %esp\n"
    "movdqu %xmm0, 0x00(%esp)\n"
    "movdqu %xmm1, 0x10(%esp)\n"
    "movdqu %xmm2, 0x20(%esp)\n"
    "movdqu %xmm3, 0x30(%esp)\n"
    "movdqu %xmm4, 0x40(%esp)\n"
    "movdqu %xmm5, 0x50(%esp)\n"
    "movdqu %xmm6, 0x60(%esp)\n"
    "movdqu %xmm7, 0x70(%esp)\n"
    "subl $12, %esp\n"
    "pushl %ebp\n"
    "call _AiHudSiteCall\n"
    "addl $16, %esp\n"
    "movdqu 0x00(%esp), %xmm0\n"
    "movdqu 0x10(%esp), %xmm1\n"
    "movdqu 0x20(%esp), %xmm2\n"
    "movdqu 0x30(%esp), %xmm3\n"
    "movdqu 0x40(%esp), %xmm4\n"
    "movdqu 0x50(%esp), %xmm5\n"
    "movdqu 0x60(%esp), %xmm6\n"
    "movdqu 0x70(%esp), %xmm7\n"
    "movl %ebp, %esp\n"
    "popal\n"
    "popfl\n"
    "jmp *_g_aiSiteResume\n");
