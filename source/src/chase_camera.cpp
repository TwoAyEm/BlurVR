// The car the game's chase camera follows (NOTES.md section 29).
#include "chase_camera.h"
#include <cmath>
#include "config.h"
#include "core.h"
#include "ai_hud.h"
#include "game_camera.h"
#include "inline_hook.h"
#include "vr_runtime.h"

static void ApplySeatForKey();

namespace {

// The chase camera's per-frame update (AmaxChaseCamera, 0x00590440 in the Steam build): a __thiscall member without arguments.  Its first 12 bytes are whole
// instructions without relative operands (push ebp / mov ebp,esp / and esp,-16 / sub esp,0xF4); the call after them reads its parameter block (the tweakables
// Camera2.Chase2.*) and the rest of the pattern is what follows.  Matches once in the Steam build's dump.
const char* const kUpdatePattern =
    "55 8B EC 83 E4 F0 81 EC F4 00 00 00 53 56 57 8D 84 24 B0 00 00 00 50 8B F1 E8 ?? ?? ?? ?? 0F 57 C0 8D 8C 24 A4 00 00 00 51 8D 54 24 54 F3 0F 11 84 24 A8 "
    "00 00 00 F3 0F 11 84 24 AC 00 00 00";
const size_t kUpdateStolen = 12;
const size_t kMaxPattern = 96;

// The camera object keeps the pose of what it follows at +8: the position (3 floats) and the rotation as a quaternion x, y, z, w (4 floats at +0x14).
const size_t kPoseOffset = 8;

typedef void(__attribute__((fastcall)) * UpdateFn)(void* self, void* edx);
UpdateFn g_origUpdate = NULL;
bool g_hooked = false;

struct RawPose {
    float pos[3], quat[4];
    unsigned long long frame; // g_vr.frameCount when it was read
    bool hasBody;             // body holds the position of the car's body as the game's own HUD finds it (the pose above is the chase camera's target)
    float body[3];
    bool hasBodyQuat;         // bodyQuat holds the body's own orientation (x, y, z, w): the pose's is the chase camera's, which can lead the car's heading when it steers
    float bodyQuat[4];
};
volatile LONG g_rawSeq = 0; // a sequence lock: the writer is the game's thread, the reader the thread that presents
RawPose g_raw;
bool g_rawValid = false;

void* g_self = NULL;              // the chase camera object the update was last called on
unsigned long long g_updateFrame = 0; // g_vr.frameCount when it was

// The pose the camera object holds now.  False when it is not numbers.
bool ReadPose(const void* self, RawPose& raw)
{
    const float* f = (const float*)((const BYTE*)self + kPoseOffset);
    for (int i = 0; i < 3; i++)
        raw.pos[i] = f[i];
    for (int i = 0; i < 4; i++)
        raw.quat[i] = f[3 + i];
    raw.frame = g_vr.frameCount;
    raw.hasBody = false;
    raw.hasBodyQuat = false;
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(raw.pos[i]))
            return false;
    for (int i = 0; i < 4; i++)
        if (!std::isfinite(raw.quat[i]))
            return false;
    return true;
}

// The car's velocity, worked out from how its pose changes between the moments it was read (smoothed over a few of them); the pose is a physics snapshot that is
// not behind the car the game draws but ahead of it, by a fixed time, so at speed the seat has to be put back by the speed times that time.
double g_velocity[3] = {0, 0, 0};
float g_lastPos[3] = {0, 0, 0};
LARGE_INTEGER g_lastTime = {};
bool g_haveLast = false;

void NoteVelocity(const RawPose& raw)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (!g_haveLast) {
        memcpy(g_lastPos, raw.pos, sizeof(g_lastPos));
        g_lastTime = now;
        g_haveLast = true;
        return;
    }
    const double seconds = (double)(now.QuadPart - g_lastTime.QuadPart) / (double)frequency.QuadPart;
    if (raw.pos[0] == g_lastPos[0] && raw.pos[1] == g_lastPos[1] && raw.pos[2] == g_lastPos[2]) {
        if (seconds > 0.25) // (not moving, or the pose is not being updated)
            g_velocity[0] = g_velocity[1] = g_velocity[2] = 0;
        return;
    }
    if (seconds > 0.001 && seconds < 0.25) {
        for (int i = 0; i < 3; i++)
            g_velocity[i] = 0.5 * g_velocity[i] + 0.5 * (raw.pos[i] - g_lastPos[i]) / seconds;
    } else {
        g_velocity[0] = g_velocity[1] = g_velocity[2] = 0;
    }
    memcpy(g_lastPos, raw.pos, sizeof(g_lastPos));
    g_lastTime = now;
}

void Publish(const RawPose& raw)
{
    NoteVelocity(raw);
    InterlockedIncrement(&g_rawSeq); // odd: being written
    MemoryBarrier();
    g_raw = raw;
    g_rawValid = true;
    MemoryBarrier();
    InterlockedIncrement(&g_rawSeq);
}

bool FindBody(const float* target, float* out, float* quat, bool* hasQuat);

void __attribute__((fastcall)) H_Update(void* self, void* edx)
{
    g_origUpdate(self, edx);
    g_self = self;
    g_updateFrame = g_vr.frameCount;
    RawPose raw;
    if (ReadPose(self, raw)) {
        // The body is read now, at the moment the pose was made: the physics steps the body a few milliseconds before the camera object follows, and a body read later (at
        // the camera data, up to five times a frame) is a step ahead of the camera by 0.25 m at 20 m/s, along the direction of travel, which in a corner or a slide is
        // sideways in the car (the trace of 2026-10-30).
        raw.hasBody = g_cfg.carSeatBody && FindBody(raw.pos, raw.body, raw.bodyQuat, &raw.hasBodyQuat);
        Publish(raw);
    }
}

bool GetRaw(RawPose& out)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        LONG before = g_rawSeq;
        if (before & 1)
            continue;
        MemoryBarrier();
        RawPose copy = g_raw;
        bool valid = g_rawValid;
        MemoryBarrier();
        if (before == g_rawSeq) {
            out = copy;
            return valid;
        }
    }
    return false;
}

// ---- finding code by its bytes
// Scans the executable sections for `pattern` (hex bytes, ?? = any); `found` is the first match, the result is true when there is exactly one.
bool ScanPattern(const char* pattern, const BYTE*& found, unsigned& count)
{
    unsigned char bytes[kMaxPattern], mask[kMaxPattern];
    size_t length = 0;
    for (const char* p = pattern; *p && length < kMaxPattern;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (p[0] == '?') {
            bytes[length] = 0;
            mask[length++] = 0;
        } else {
            char text[3] = {p[0], p[1], 0};
            bytes[length] = (unsigned char)strtoul(text, NULL, 16);
            mask[length++] = 0xFF;
        }
        p += 2;
    }
    found = NULL;
    count = 0;
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base || !length)
        return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !strncmp((const char*)sections[s].Name, ".bind", 8))
            continue;
        const BYTE* code = base + sections[s].VirtualAddress;
        const DWORD size = sections[s].Misc.VirtualSize ? sections[s].Misc.VirtualSize : sections[s].SizeOfRawData;
        for (size_t i = 0; i + length <= size; i++) {
            if (code[i] != bytes[0])
                continue;
            size_t k = 1;
            while (k < length && (code[i + k] & mask[k]) == bytes[k])
                k++;
            if (k == length) {
                if (!count++)
                    found = code + i;
            }
        }
    }
    return count == 1;
}

bool FindUpdate(const BYTE*& found, unsigned& count)
{
    return ScanPattern(kUpdatePattern, found, count);
}

// ---- the car the game draws
// The chase camera's pose is its target, which at speed and under acceleration is not where the car is (the seat ran ahead of the car).  The AI HUD finds each
// car's position through a short chain: UIAiHud's update calls <player>(car number) (0x00C32060: cdecl, the car's record or 0), then <vehicle>(record, car number)
// (0x00435E60: thiscall) and reads the position from the vehicle's body (the object at vehicle+0x240, position at +0x2F4).  The calls are taken from the bytes of that
// function (found by game_signatures.cpp) at fixed offsets, and each target is checked before it is used.
typedef void*(__attribute__((cdecl)) * GetPlayerFn)(int);
typedef void*(__attribute__((thiscall)) * GetVehicleFn)(void* self, int index);
GetPlayerFn g_getPlayer = NULL;
GetVehicleFn g_getVehicle = NULL;
unsigned g_bodyOffset = 0, g_positionOffset = 0, g_hiddenOffset = 0;
unsigned g_orientationOffset = 0; // the body's orientation quaternion (the getter just before the position's: four floats), 0 = not found
// Which car the player drives, for the seat saved per car: a key made from the body's mass and inertia (CarKeyOfBody), set when the player's car is found.  0 = not
// known yet.  (Cleared when the player's car is looked for again.)
unsigned g_carKey = 0;
bool g_bodyResolved = false;

const BYTE* CallTarget(const BYTE* at)
{
    if (at[0] != 0xE8)
        return NULL;
    int rel;
    memcpy(&rel, at + 1, 4);
    return at + 5 + rel;
}

unsigned ReadU32(const BYTE* p)
{
    unsigned v;
    memcpy(&v, p, 4);
    return v;
}

bool BytesAre(const BYTE* at, const char* hex)
{
    for (const char* p = hex; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (p[0] != '?') {
            char text[3] = {p[0], p[1], 0};
            if (*at != (BYTE)strtoul(text, NULL, 16))
                return false;
        }
        at++;
        p += 2;
    }
    return true;
}

// The same chain without the AI HUD: each function is found by its own bytes (every one matches once in the Steam build, and the offsets are part of the patterns).  Used
// when the AI HUD's update was not found in this build or is not laid out as in the Steam build (a build whose code is moved about: the cockpit seat then followed the
// chase camera's target, which sways against the car).  The body pointer's offset in the vehicle is only in the AI HUD's code; without it the Steam build's is tried, and a
// wrong one shows as no car near the camera.
bool ResolveBodyByScan(const BYTE* d)
{
    const BYTE *player = NULL, *hidden = NULL, *vehicle = NULL, *position = NULL, *orientation = NULL;
    unsigned counts[5] = {};
    ScanPattern("A1 ?? ?? ?? ?? 85 C0 74 33 8B 80 90 0C 00 00 85 C0 74 29 83 78 50 00 74 23", player, counts[0]);
    ScanPattern("8A 81 EB 03 00 00 C3", hidden, counts[1]);
    ScanPattern("8B 49 48 85 C9 75 05 33 C0 C2 04 00 E9 ?? ?? ?? ??", vehicle, counts[2]);
    ScanPattern("8B 44 24 04 D9 81 F4 02 00 00 D9 18 D9 81 F8 02 00 00 D9 58 04 D9 81 FC 02 00 00 D9 58 08 C2 04 00", position, counts[3]);
    ScanPattern("8B 44 24 04 D9 81 3C 02 00 00 D9 18 D9 81 40 02 00 00 D9 58 04 D9 81 44 02 00 00 D9 58 08 D9 81 48 02 00 00 D9 58 0C C2 04 00", orientation, counts[4]);
    if (counts[0] != 1 || counts[1] != 1 || counts[2] != 1 || counts[3] != 1) {
        Log("cockpit: the car's body was not found by its own functions either (player %u, hidden %u, vehicle %u, position %u matches; orientation %u): the chase camera's "
            "target is the car's place",
            counts[0], counts[1], counts[2], counts[3], counts[4]);
        return false;
    }
    unsigned bodyOffset = 0x240;
    if (d) { // (the vehicle's body is tested against zero by the AI HUD: cmp [esi+offset],0)
        for (size_t i = 0; i < 0x800; i++) {
            if (d[i] == 0x83 && d[i + 1] == 0xBE && d[i + 4] == 0 && d[i + 5] == 0 && d[i + 6] == 0 && ReadU32(d + i + 2) < 0x2000 && ReadU32(d + i + 2) > 0x100) {
                bodyOffset = ReadU32(d + i + 2);
                break;
            }
        }
    }
    g_positionOffset = ReadU32(position + 6);
    g_hiddenOffset = ReadU32(hidden + 2);
    g_bodyOffset = bodyOffset;
    g_orientationOffset = counts[4] == 1 ? ReadU32(orientation + 6) : 0;
    g_getPlayer = (GetPlayerFn)player;
    g_getVehicle = (GetVehicleFn)vehicle;
    g_bodyResolved = true;
    Log("cockpit: cars are found by their own functions (the AI HUD's way was not usable): player %p, vehicle %p, body at +0x%x, position at +0x%x, orientation %s", player, vehicle,
        g_bodyOffset, g_positionOffset, g_orientationOffset ? "found" : "not found");
    return true;
}

bool ResolveBody()
{
    // (the update was found by ai_hud.cpp, which has put its own jump over the first bytes: a scan for the function would find nothing)
    const BYTE* d = AiHudUpdateStart();
    if (!d) {
        static unsigned scans = 0;
        if (scans++ == 0)
            Log("cockpit: the AI HUD's update is not known (yet): the car's functions are looked for by their bytes");
        return ResolveBodyByScan(NULL);
    }
    const BYTE* player = CallTarget(d + 0x2EC);   // call <player>(car number)
    const BYTE* hidden = CallTarget(d + 0x304);   // call <hidden>(record): mov al,[ecx+0x3EB] / ret
    const BYTE* vehicle = CallTarget(d + 0x314);  // call <vehicle>(record, car number)
    const BYTE* position = CallTarget(d + 0x46B); // call <position>(out): the body's position
    const bool bodyUse = BytesAre(d + 0x327, "83 BE 40 02 00 00 00") && BytesAre(d + 0x457, "8B 88 40 02 00 00");
    const bool playerOk = player && BytesAre(player, "A1 ?? ?? ?? ?? 85 C0 74 33 8B 80 90 0C 00 00");
    const bool hiddenOk = hidden && BytesAre(hidden, "8A 81 EB 03 00 00 C3");
    const bool vehicleOk = vehicle && BytesAre(vehicle, "8B 49 48 85 C9");
    const bool positionOk = position && BytesAre(position, "8B 44 24 04 D9 81 ?? ?? 00 00 D9 18 D9 81 ?? ?? 00 00 D9 58 04 D9 81 ?? ?? 00 00 D9 58 08 C2 04 00");
    if (!playerOk || !hiddenOk || !vehicleOk || !positionOk || !bodyUse) {
        Log("cockpit: the AI HUD's way to a car's position is not the expected one (player %s, hidden %s, vehicle %s, position %s, body use %s; update at %p): the chase "
            "camera's target is the car's place",
            playerOk ? "ok" : "no", hiddenOk ? "ok" : "no", vehicleOk ? "ok" : "no", positionOk ? "ok" : "no", bodyUse ? "ok" : "no", d);
        if (ResolveBodyByScan(d))
            return true;
        if (d) {
            char hexText[3 * 16 + 1] = "";
            const BYTE* places[4] = {d + 0x327, d + 0x457, player, position};
            for (int k = 0; k < 4; k++) {
                if (!places[k])
                    continue;
                for (int i = 0; i < 16; i++)
                    snprintf(hexText + 3 * i, 4, "%02X ", places[k][i]);
                Log("cockpit:   bytes at %p: %s", places[k], hexText);
            }
        }
        return true;
    }
    g_orientationOffset = 0;
    {
        // the getter 0x30 bytes before the position's: the body's orientation as four floats
        const BYTE* orientation = position - 0x30;
        if (BytesAre(orientation, "8B 44 24 04 D9 81 ?? ?? 00 00 D9 18 D9 81 ?? ?? 00 00 D9 58 04 D9 81 ?? ?? 00 00 D9 58 08 D9 81 ?? ?? 00 00 D9 58 0C C2 04 00")) {
            const unsigned first = ReadU32(orientation + 6);
            if (ReadU32(orientation + 14) == first + 4 && ReadU32(orientation + 23) == first + 8 && ReadU32(orientation + 32) == first + 12 && first < 0x2000)
                g_orientationOffset = first;
        }
    }
    g_positionOffset = ReadU32(position + 6);
    g_bodyOffset = ReadU32(d + 0x327 + 2);
    g_hiddenOffset = ReadU32(hidden + 2);
    if (ReadU32(position + 14) != g_positionOffset + 4 || ReadU32(position + 23) != g_positionOffset + 8 || g_bodyOffset > 0x2000 || g_positionOffset > 0x2000 ||
        g_hiddenOffset > 0x2000) {
        Log("cockpit: the offsets in the AI HUD's way to a car's position are not the expected ones");
        if (ResolveBodyByScan(d))
            return true;
        return true;
    }
    g_getPlayer = (GetPlayerFn)player;
    g_getVehicle = (GetVehicleFn)vehicle;
    g_bodyResolved = true;
    Log("cockpit: cars are found like the AI HUD does (player %p, vehicle %p, body at +0x%x, position at +0x%x, orientation %s)", player, vehicle, g_bodyOffset, g_positionOffset,
        g_orientationOffset ? "found" : "not found");
    if (g_orientationOffset)
        Log("cockpit: the body's orientation is at +0x%x", g_orientationOffset);
    return true;
}

// The position of the body of car number `car`.  False when there is no such car.
bool BodyOfCar(int car, float* out, float* quat = NULL, bool* hasQuat = NULL)
{
    void* record = g_getPlayer(car);
    if (!record || *((const BYTE*)record + g_hiddenOffset))
        return false;
    void* vehicle = g_getVehicle(record, car);
    if (!vehicle)
        return false;
    const BYTE* body = *(const BYTE* const*)((const BYTE*)vehicle + g_bodyOffset);
    if (!body)
        return false;
    const float* position = (const float*)(body + g_positionOffset);
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
        return false;
    out[0] = position[0];
    out[1] = position[1];
    out[2] = position[2];
    if (hasQuat) {
        *hasQuat = false;
        if (g_orientationOffset && quat) {
            const float* q = (const float*)(body + g_orientationOffset);
            const float length = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (std::isfinite(length) && length > 0.9f && length < 1.1f) {
                memcpy(quat, q, 4 * sizeof(float));
                *hasQuat = true;
            }
        }
    }
    return true;
}

// The player's car.  The chase camera's target runs ahead of the car by up to 15 m at speed, so "the car nearest the target" is another car as soon as there is one
// near (the first version locked onto them).  The player's car is found while it stands (the target is then within 4 m of its body, nearer than any other car can
// be), or while it moves as the car that is behind the target on the line of travel, and its number is kept: car numbers do not change in a race.
int g_playerCar = -1;
unsigned g_lostFrames = 0;

// A key for the car model, from the physics body: its mass (+0x2A0) and the first inertia value (+0x188) are the same every time the model is raced and differ
// between models (R26 1296 kg, Focus RS 1400, Defender 1750 in the dump of 2026-10-30).  0 when they look wrong.
static unsigned CarKeyOfBody(const BYTE* body)
{
    if (!body)
        return 0;
    float mass = *(const float*)(body + 0x2A0), inertia = *(const float*)(body + 0x188);
    if (!std::isfinite(mass) || !std::isfinite(inertia) || mass < 100.0f || mass > 20000.0f)
        return 0;
    unsigned key = 2166136261u;
    const long values[2] = {lroundf(mass), lroundf(inertia)};
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < 4; b++)
            key = (key ^ (unsigned)((values[i] >> (8 * b)) & 0xFF)) * 16777619u;
    return key ? key : 1;
}

// The player's car can change under the same car number (the next race, another car picked in the showroom): the key is read again while the car is followed, and when
// another one has been there for a while the seat of that car is taken.  (It was read only when the car was first found, so every car of a session got the first one's seat.)
static void NoteCarModel()
{
    static unsigned candidate = 0, streak = 0;
    void* record = g_getPlayer(g_playerCar);
    void* vehicle = record ? g_getVehicle(record, g_playerCar) : NULL;
    const BYTE* body = vehicle ? *(const BYTE* const*)((const BYTE*)vehicle + g_bodyOffset) : NULL;
    const unsigned key = CarKeyOfBody(body);
    if (!key || key == g_carKey) {
        streak = 0;
        return;
    }
    if (key != candidate) {
        candidate = key;
        streak = 0;
    }
    if (++streak < 60)
        return;
    streak = 0;
    g_carKey = key;
    ApplySeatForKey();
}

bool FindBody(const float* target, float* out, float* quat = NULL, bool* hasQuat = NULL)
{
    if (hasQuat)
        *hasQuat = false;
    if (!g_bodyResolved)
        return false;
    const double speed = sqrt(g_velocity[0] * g_velocity[0] + g_velocity[1] * g_velocity[1] + g_velocity[2] * g_velocity[2]);
    if (g_playerCar >= 0) {
        float body[3];
        if (BodyOfCar(g_playerCar, body, quat, hasQuat)) {
            const double d[3] = {body[0] - target[0], body[1] - target[1], body[2] - target[2]};
            // (the target leads the car by about a third of a second of travel: 37 m at 115 m/s on a boost, so the limit grows with the speed)
            const double reach = 40.0 + 1.5 * speed;
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < reach * reach) {
                memcpy(out, body, sizeof(body));
                g_lostFrames = 0;
                NoteCarModel();
                return true;
            }
        }
        if (++g_lostFrames > 120) { // (a new race, or the car is gone: look again)
            Log("cockpit: car %d is not at the chase camera's target any more: the player's car is looked for again", g_playerCar);
            g_playerCar = -1;
            g_carKey = 0;
        }
        return false;
    }
    double best = 1e9;
    int bestCar = -1;
    float bestBody[3] = {}, bestQuat[4] = {};
    bool bestHasQuat = false;
    for (int car = 0; car < 32; car++) {
        float body[3], bodyQuat[4];
        bool bodyHasQuat = false;
        if (!BodyOfCar(car, body, bodyQuat, &bodyHasQuat))
            continue;
        const double d[3] = {body[0] - target[0], body[1] - target[1], body[2] - target[2]};
        double score;
        if (speed < 3.0) {
            score = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (score > 4.0)
                continue;
        } else {
            // moving: behind the target on the line of travel (the target leads the car), close to that line
            const double h[3] = {g_velocity[0] / speed, g_velocity[1] / speed, g_velocity[2] / speed};
            const double along = d[0] * h[0] + d[1] * h[1] + d[2] * h[2];
            const double side = sqrt(fmax(0.0, d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along));
            if (along > 1.0 || along < -(25.0 + 0.5 * speed) || side > 2.5)
                continue;
            score = side;
        }
        if (score < best) {
            best = score;
            bestCar = car;
            memcpy(bestBody, body, sizeof(bestBody));
            memcpy(bestQuat, bodyQuat, sizeof(bestQuat));
            bestHasQuat = bodyHasQuat;
        }
    }
    if (bestCar < 0)
        return false;
    g_playerCar = bestCar;
    g_carKey = 0;
    g_lostFrames = 0;
    {
        void* record = g_getPlayer(bestCar);
        void* vehicle = record ? g_getVehicle(record, bestCar) : NULL;
        const BYTE* body = vehicle ? *(const BYTE* const*)((const BYTE*)vehicle + g_bodyOffset) : NULL;
        g_carKey = CarKeyOfBody(body);
        if (g_carKey)
            ApplySeatForKey();
    }
    Log("cockpit: the player's car is car number %d (its body is %.2f m from the chase camera's target, the car %s)", bestCar, best, speed < 3.0 ? "stands" : "moves");
    memcpy(out, bestBody, sizeof(bestBody));
    if (hasQuat && quat && bestHasQuat) {
        memcpy(quat, bestQuat, sizeof(bestQuat));
        *hasQuat = true;
    }
    return true;
}

// ---- the car and the camera of the last seat (CarSeatDelta), which the draws held to the body are moved by
struct ProbeState {
    bool valid;
    unsigned long long frame;
    double origin[3], axes[3][3];       // the car's body and its axes (right, up, forward) of the last CarSeatDelta
    double camPos[3], camAxes[3][3];     // the main camera (right, up, forward)
    bool hasBody;                        // the origin and axes are the body's own (found and locked), which the held draws need
} g_probe;
double g_defaultSeat[3] = {0, 0, 0};
bool g_defaultSeatKnown = false;

// ---- matching the car's axes to the camera's
// The game turns a point of the car into the world with the matrix of the conjugate of the stored quaternion (the helper at 0x0058AF00 negates x, y and z before it
// builds the matrix, and adds the position; tools/chase_camera_check.py checks the matrix against it).  What is not known is which of the car's local axes points
// forward, so every way to take the three axes as (right, up, forward) is tried: the 24 signed permutations that keep the handedness.  The one that fits is the one
// whose axes line up with the chase camera's (it looks along the car's heading, tilted a little) and that has the camera behind the car.
struct Candidate {
    int axis[3], sign[3]; // seat axis j (0 right, 1 up, 2 forward) = sign[j] * the car's local axis axis[j]
};
Candidate g_candidates[24];
int g_candidateCount = 0;
int g_choice = -1;
// The camera cannot say which way the car points once it swings, orbits or pulls back 15 m at speed (the first versions lost the car there), but the car's own
// motion can: its forward axis is the one along its velocity.  Votes are collected while it moves fast and straight and the choice is locked once one candidate has
// clearly won; after that the camera is not looked at.
double g_votes[24];
unsigned g_voteSamples = 0;
bool g_locked = false;
struct BodyCandidate {
    bool conjugate;
    Candidate axes;
};
BodyCandidate g_bodyCandidates[48];
int g_bodyCandidateCount = 0;
double g_bodyVotes[48];
unsigned g_bodySamples = 0;
bool g_bodyLocked = false;
int g_bodyChoice = -1;
unsigned g_logged = 0;

void BuildCandidates()
{
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    static const int parity[6] = {1, -1, -1, 1, 1, -1};
    g_candidateCount = 0;
    g_bodyCandidateCount = 0;
    for (int p = 0; p < 6; p++)
        for (int signs = 0; signs < 8; signs++) {
            Candidate& c = g_candidates[g_candidateCount];
            int product = parity[p];
            for (int j = 0; j < 3; j++) {
                c.axis[j] = perms[p][j];
                c.sign[j] = (signs >> j & 1) ? -1 : 1;
                product *= c.sign[j];
            }
            if (product != 1)
                continue;
            for (int conjugate = 0; conjugate < 2; conjugate++) {
                g_bodyCandidates[g_bodyCandidateCount].conjugate = conjugate != 0;
                g_bodyCandidates[g_bodyCandidateCount].axes = c;
                g_bodyCandidateCount++;
            }
            g_candidateCount++;
        }
}

double Dot(const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The seat's axes in the world for a candidate (rows: right, up, forward).
// The car's local axes in the world (column k of the matrix of the quaternion x, y, z, w, or of its conjugate): col[k][i].
void LocalColumns(const float* q, bool conjugate, double col[3][3])
{
    const double x = q[0], y = q[1], z = q[2], w = conjugate ? -q[3] : q[3];
    const double m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++)
            col[k][i] = m[i][k];
}

void SeatAxes(const RawPose& raw, const Candidate& c, double axes[3][3])
{
    double col[3][3];
    LocalColumns(raw.quat, true, col); // (the matrix of the conjugate, which is what the game applies to the chase camera's pose)
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            axes[j][i] = c.sign[j] * col[c.axis[j]][i];
}

// The body's own orientation.  Its convention is not known (the quaternion or its conjugate, and which local axis is forward), so the 48 ways are tried against the
// frame the chase camera's pose gives, which is the car's own to within the steering it leads by; once one has clearly won it is kept (BodyFrameVotes).
void BodyAxes(const float* q, const BodyCandidate& c, double axes[3][3])
{
    double col[3][3];
    LocalColumns(q, c.conjugate, col);
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            axes[j][i] = c.axes.sign[j] * col[c.axes.axis[j]][i];
}

// The user's seat was tuned against the chase camera's target (the first versions placed it there) and the target stands a little off the body (0.37 m under it and 0.27 m
// to the right in the car tested).  Measured while the car stands, in the car's own axes (so that it does not depend on which are taken for right, up and forward),
// and added to the seat when the body is the origin, so that the seat is where it was tuned.
double g_restOffset[3] = {0, 0, 0}; // the target against the body along the body's axes (right, up, forward)
bool g_restValid = false;

const float* Origin(const RawPose& raw)
{
    return raw.hasBody ? raw.body : raw.pos;
}

double Score(const RawPose& raw, const Candidate& c, const MainCameraPose& cam)
{
    double axes[3][3];
    SeatAxes(raw, c, axes);
    const float* origin = Origin(raw);
    const double d[3] = {cam.pos[0] - origin[0], cam.pos[1] - origin[1], cam.pos[2] - origin[2]};
    const double length = sqrt(Dot(d, d));
    if (length < 0.5)
        return -10;
    const double aligned = Dot(axes[0], cam.right) + Dot(axes[1], cam.up) + Dot(axes[2], cam.fwd);
    const double behind = (-Dot(d, axes[2]) - fabs(Dot(d, axes[0]))) / length; // (the camera is straight behind the car, a little above)
    return aligned + behind;
}

} // namespace

bool g_cockpitSeatOn = false;

bool ChaseCameraHooked()
{
    return g_hooked;
}

// ---- the seat saved per car
// BlurVR_seats.ini keeps one section per car key ([car_xxxxxxxx], X / Y / Z); a car that has none uses the seat of BlurVR.ini ([VR] CarSeatX / Y / Z).
static std::string SeatsPath()
{
    return std::string(g_modDir) + "BlurVR_seats.ini";
}

static void ApplySeatForKey()
{
    if (!g_defaultSeatKnown) {
        g_defaultSeat[0] = g_cfg.carSeatX;
        g_defaultSeat[1] = g_cfg.carSeatY;
        g_defaultSeat[2] = g_cfg.carSeatZ;
        g_defaultSeatKnown = true;
    }
    char section[32], text[3][32];
    snprintf(section, sizeof(section), "car_%08x", g_carKey);
    const char* names[3] = {"X", "Y", "Z"};
    bool all = true;
    for (int i = 0; i < 3; i++) {
        GetPrivateProfileStringA(section, names[i], "", text[i], sizeof(text[i]), SeatsPath().c_str());
        all = all && text[i][0];
    }
    if (all) {
        g_cfg.carSeatX = atof(text[0]);
        g_cfg.carSeatY = atof(text[1]);
        g_cfg.carSeatZ = atof(text[2]);
        Log("cockpit: this car is %08x: its saved seat %.2f %.2f %.2f is used", g_carKey, g_cfg.carSeatX, g_cfg.carSeatY, g_cfg.carSeatZ);
    } else {
        g_cfg.carSeatX = g_defaultSeat[0];
        g_cfg.carSeatY = g_defaultSeat[1];
        g_cfg.carSeatZ = g_defaultSeat[2];
        Log("cockpit: this car is %08x: no saved seat, the seat of BlurVR.ini (%.2f %.2f %.2f) is used until you move it (it is then kept for this car)", g_carKey,
            g_cfg.carSeatX, g_cfg.carSeatY, g_cfg.carSeatZ);
    }
}

bool ChaseCameraSaveSeat()
{
    if (!g_carKey)
        return false;
    char section[32], text[32];
    snprintf(section, sizeof(section), "car_%08x", g_carKey);
    const char* names[3] = {"X", "Y", "Z"};
    const double values[3] = {g_cfg.carSeatX, g_cfg.carSeatY, g_cfg.carSeatZ};
    for (int i = 0; i < 3; i++) {
        snprintf(text, sizeof(text), "%.2f", values[i]);
        WritePrivateProfileStringA(section, names[i], text, SeatsPath().c_str());
    }
    Log("cockpit: the seat %.2f %.2f %.2f is kept for car %08x", values[0], values[1], values[2], g_carKey);
    return true;
}

// The buffers the draw reads.  All zero when the device does not say.
static void StreamFingerprint(IDirect3DDevice9* device, unsigned* vbSize, unsigned* vbOffset, unsigned* stride, unsigned* ibSize)
{
    *vbSize = *vbOffset = *stride = *ibSize = 0;
    if (!device)
        return;
    IDirect3DVertexBuffer9* vb = NULL;
    UINT offset = 0, step = 0;
    if (SUCCEEDED(device->GetStreamSource(0, &vb, &offset, &step)) && vb) {
        D3DVERTEXBUFFER_DESC desc;
        if (SUCCEEDED(vb->GetDesc(&desc)))
            *vbSize = desc.Size;
        vb->Release();
        *vbOffset = offset;
        *stride = step;
    }
    IDirect3DIndexBuffer9* ib = NULL;
    if (SUCCEEDED(device->GetIndices(&ib)) && ib) {
        D3DINDEXBUFFER_DESC desc;
        if (SUCCEEDED(ib->GetDesc(&desc)))
            *ibSize = desc.Size;
        ib->Release();
    }
}

// ---- the car's own draws held to the body (ChaseCameraPinWorldView / ChaseCameraPinWorld)
// The drawn chassis springs off the physics body over bumps (up to 0.35 m and 18 degrees, back in about a second), and the seat is on the body, so the cabin moved against
// the view.  A draw of the car that is near the body's middle line (not the wheels) is drawn where it is against the body when the car rides level, a slow average kept per
// draw (its shader and buffers), carried to the body of this frame: it cannot move against the seat.  The average only follows what stays within 3 cm and about 2 degrees of
// it, and starts again at what is drawn if the draw stays away for 150 frames.
struct PinEntry {
    unsigned key;
    bool valid;
    unsigned long long frame;
    unsigned off;
    Matrix4 rest; // the draw's matrix in the body's frame (right, up, forward) as it usually is
};
static const int kPinMax = 96;
static PinEntry g_pins[kPinMax];
static unsigned g_pinCar = 0;

static bool PinActive()
{
    return g_probe.valid && g_probe.hasBody && g_vr.frameCount <= g_probe.frame + 1;
}

static void PinBodyMatrices(Matrix4& body, Matrix4& inverse)
{
    body = Identity();
    inverse = Identity();
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            body.m[j][i] = g_probe.axes[j][i];
            inverse.m[i][j] = g_probe.axes[j][i];
        }
        body.m[3][j] = g_probe.origin[j];
        inverse.m[3][j] = -Dot(g_probe.origin, g_probe.axes[j]);
    }
}

static bool PinWorld(IDirect3DDevice9* device, int shader, const Matrix4& world, bool skinned, Matrix4& fixedWorld)
{
    const unsigned carKey = g_carKey * 31u + (unsigned)(g_playerCar + 1);
    if (carKey != g_pinCar) {
        g_pinCar = carKey;
        memset(g_pins, 0, sizeof(g_pins));
    }
    Matrix4 body, inverse;
    PinBodyMatrices(body, inverse);
    const Matrix4 inBody = Multiply(world, inverse);
    if (fabs(inBody.m[3][0]) > 0.45 || fabs(inBody.m[3][1]) > 1.5 || fabs(inBody.m[3][2]) > 4.0)
        return false;
    for (int i = 0; i < 3; i++) { // (an object's axes, not some other matrix)
        const double length = inBody.m[i][0] * inBody.m[i][0] + inBody.m[i][1] * inBody.m[i][1] + inBody.m[i][2] * inBody.m[i][2];
        if (!(length > 0.0001) || length > 1e6)
            return false;
    }
    unsigned vbSize, vbOffset, stride, ibSize;
    StreamFingerprint(device, &vbSize, &vbOffset, &stride, &ibSize);
    unsigned key = (unsigned)shader * 2654435761u ^ vbSize * 40503u ^ ibSize * 977u ^ vbOffset * 131u ^ stride * 7919u;
    if (!key)
        key = 1;
    PinEntry* entry = NULL;
    PinEntry* oldest = NULL; // (a free slot, else the one not seen for longest)
    for (int i = 0; i < kPinMax; i++) {
        if (g_pins[i].valid && g_pins[i].key == key) {
            entry = &g_pins[i];
            break;
        }
        if (!oldest || (oldest->valid && (!g_pins[i].valid || g_pins[i].frame < oldest->frame)))
            oldest = &g_pins[i];
    }
    if (!entry) {
        entry = oldest;
        entry->key = key;
        entry->valid = true;
        entry->off = 0;
        entry->rest = inBody;
        entry->frame = g_vr.frameCount;
    } else if (entry->frame != g_vr.frameCount) {
        entry->frame = g_vr.frameCount;
        double moved = 0, turned = 0;
        for (int i = 0; i < 3; i++)
            moved += (inBody.m[3][i] - entry->rest.m[3][i]) * (inBody.m[3][i] - entry->rest.m[3][i]);
        moved = sqrt(moved);
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++)
                turned = fmax(turned, fabs(inBody.m[i][k] - entry->rest.m[i][k]) / fmax(0.05, sqrt(entry->rest.m[i][0] * entry->rest.m[i][0] + entry->rest.m[i][1] * entry->rest.m[i][1] + entry->rest.m[i][2] * entry->rest.m[i][2])));
        if (moved < 0.03 && turned < 0.035) {
            for (int i = 0; i < 4; i++)
                for (int k = 0; k < 4; k++)
                    entry->rest.m[i][k] += (inBody.m[i][k] - entry->rest.m[i][k]) * 0.02;
            entry->off = 0;
        } else {
            if (++entry->off > 150) {
                entry->rest = inBody;
                entry->off = 0;
            }
        }
    }
    fixedWorld = Multiply(entry->rest, body);
    if (skinned && !g_cfg.cockpitDriver) { // (the driver: its bones move against the car, and the head is inside it)
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++)
                fixedWorld.m[i][k] = 0;
    }
    return true;
}

bool ChaseCameraPinWorldView(IDirect3DDevice9* device, int shader, const Matrix4& worldView, Matrix4& out)
{
    if (!PinActive())
        return false;
    double rel[3];
    for (int i = 0; i < 3; i++)
        rel[i] = g_probe.camPos[i] + g_probe.camAxes[0][i] * worldView.m[3][0] + g_probe.camAxes[1][i] * worldView.m[3][1] + g_probe.camAxes[2][i] * worldView.m[3][2] -
                 g_probe.origin[i];
    if (!(Dot(rel, rel) < 25.0))
        return false;
    Matrix4 camera = Identity(), cameraInverse = Identity();
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            camera.m[j][i] = g_probe.camAxes[j][i];
            cameraInverse.m[i][j] = g_probe.camAxes[j][i];
        }
        camera.m[3][j] = g_probe.camPos[j];
        cameraInverse.m[3][j] = -Dot(g_probe.camPos, g_probe.camAxes[j]);
    }
    Matrix4 fixedWorld;
    if (!PinWorld(device, shader, Multiply(worldView, camera), false, fixedWorld))
        return false;
    out = Multiply(fixedWorld, cameraInverse);
    return true;
}

bool ChaseCameraPinWorld(IDirect3DDevice9* device, int shader, const Matrix4& world, bool skinned, Matrix4& out)
{
    if (!PinActive())
        return false;
    double rel[3] = {world.m[3][0] - g_probe.origin[0], world.m[3][1] - g_probe.origin[1], world.m[3][2] - g_probe.origin[2]};
    if (!(Dot(rel, rel) < 25.0))
        return false;
    return PinWorld(device, shader, world, skinned, out);
}

void ChaseCameraRefresh()
{
    // The game has just given the main view its camera.  The pose the update read is from before that, and at 70 m/s a frame is a metre: the seat trailed the
    // car by it.  Read the car's pose again now, so that it and the camera are from the same moment, and rebuild the delta with them.
    g_cockpitSeatOn = false;
    if (!g_hooked || !g_self || !g_cfg.cockpit || g_vr.frameCount > g_updateFrame + 3)
        return;
    g_cockpitSeatOn = true;
    RawPose raw, published;
    if (ReadPose(g_self, raw)) {
        // (the pose the update published, with the body of its moment, is kept while the object still holds it)
        const bool same = GetRaw(published) && published.hasBody && memcmp(published.pos, raw.pos, sizeof(raw.pos)) == 0 && memcmp(published.quat, raw.quat, sizeof(raw.quat)) == 0;
        if (!same) {
            raw.hasBody = g_cfg.carSeatBody && FindBody(raw.pos, raw.body, raw.bodyQuat, &raw.hasBodyQuat);
            Publish(raw);
        } else {
            published.frame = g_vr.frameCount;
            Publish(published);
        }
    }
    VrComposeSeat();
}

bool CarSeatDelta(const double seat[3], Matrix4& cameraToSeat)
{
    RawPose raw;
    MainCameraPose cam;
    if (!g_hooked || !GetRaw(raw) || !GetMainCameraPose(cam))
        return false;
    if (g_vr.frameCount > raw.frame + 3 || g_vr.frameCount > cam.frame + 3) // (the chase camera is not the one updating: a bumper camera, a replay, a menu)
        return false;
    const float qlen = raw.quat[0] * raw.quat[0] + raw.quat[1] * raw.quat[1] + raw.quat[2] * raw.quat[2] + raw.quat[3] * raw.quat[3];
    const float* origin = Origin(raw);
    const double d[3] = {cam.pos[0] - origin[0], cam.pos[1] - origin[1], cam.pos[2] - origin[2]};
    const double distance = sqrt(Dot(d, d));
    if (qlen < 0.9f || qlen > 1.1f || distance < 1.0 || distance > 40.0)
        return false;
    if (!g_candidateCount)
        BuildCandidates();
    const double velocityLength = sqrt(Dot(g_velocity, g_velocity));
    if (!g_locked && velocityLength > 8.0) {
        const double heading[3] = {g_velocity[0] / velocityLength, g_velocity[1] / velocityLength, g_velocity[2] / velocityLength};
        if (fabs(heading[1]) < 0.3) { // (not up a ramp or off a jump)
            static const double worldUp[3] = {0, 1, 0};
            for (int i = 0; i < g_candidateCount; i++) {
                double axes[3][3];
                SeatAxes(raw, g_candidates[i], axes);
                g_votes[i] += Dot(axes[2], heading) + 0.5 * Dot(axes[1], worldUp);
            }
            g_voteSamples++;
            int first = 0, second = -1;
            for (int i = 1; i < g_candidateCount; i++)
                if (g_votes[i] > g_votes[first])
                    first = i;
            for (int i = 0; i < g_candidateCount; i++)
                if (i != first && (second < 0 || g_votes[i] > g_votes[second]))
                    second = i;
            if (g_voteSamples >= 40 && g_votes[first] > 1.2 * g_voteSamples && g_votes[first] - g_votes[second] > 0.3 * g_voteSamples) {
                g_locked = true;
                g_choice = first;
                const Candidate& c = g_candidates[first];
                Log("cockpit: the car's frame is LOCKED from its direction of travel (%u samples): right = %cL%d, up = %cL%d, forward = %cL%d of its local axes (votes %.1f against %.1f)",
                    g_voteSamples, c.sign[0] < 0 ? '-' : '+', c.axis[0], c.sign[1] < 0 ? '-' : '+', c.axis[1], c.sign[2] < 0 ? '-' : '+', c.axis[2], g_votes[first] / g_voteSamples,
                    g_votes[second] / g_voteSamples);
            }
        }
    }
    if (!g_locked) {
        // Until then the camera is used: at rest it is behind the car, and the axes that line up with it and have it behind are the car's.
        int best = 0;
        double bestScore = -1e9;
        for (int i = 0; i < g_candidateCount; i++) {
            const double score = Score(raw, g_candidates[i], cam);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        // (stay with the choice made until another is clearly better: the candidates are 90 degrees apart)
        if (g_choice < 0 || bestScore > Score(raw, g_candidates[g_choice], cam) + 0.5) {
            if (g_choice != best && g_logged++ < 12) {
                const Candidate& c = g_candidates[best];
                Log("cockpit: the car's frame by the camera: right = %cL%d, up = %cL%d, forward = %cL%d of its local axes, score %.2f of 4 (car at %.1f %.1f %.1f, %.1f m from the camera, quaternion %.3f %.3f %.3f %.3f)",
                    c.sign[0] < 0 ? '-' : '+', c.axis[0], c.sign[1] < 0 ? '-' : '+', c.axis[1],
                    c.sign[2] < 0 ? '-' : '+', c.axis[2], bestScore, raw.pos[0], raw.pos[1], raw.pos[2], distance, raw.quat[0], raw.quat[1], raw.quat[2], raw.quat[3]);
            }
            g_choice = best;
        }
        if (Score(raw, g_candidates[g_choice], cam) < 2.0) // (a camera that is not behind the car: no pick yet)
            return false;
    }
    const Candidate& choice = g_candidates[g_choice];
    double axes[3][3];
    SeatAxes(raw, choice, axes);
    if (g_locked && raw.hasBodyQuat) {
        if (!g_bodyLocked) {
            // (the pose's frame is the car's within the steering the camera leads by; the body's candidate that agrees with it over a while, across headings, is the body's)
            for (int i = 0; i < g_bodyCandidateCount; i++) {
                double candidateAxes[3][3];
                BodyAxes(raw.bodyQuat, g_bodyCandidates[i], candidateAxes);
                g_bodyVotes[i] += Dot(candidateAxes[0], axes[0]) + Dot(candidateAxes[1], axes[1]) + Dot(candidateAxes[2], axes[2]);
            }
            g_bodySamples++;
            int first = 0, second = -1;
            for (int i = 1; i < g_bodyCandidateCount; i++)
                if (g_bodyVotes[i] > g_bodyVotes[first])
                    first = i;
            for (int i = 0; i < g_bodyCandidateCount; i++)
                if (i != first && (second < 0 || g_bodyVotes[i] > g_bodyVotes[second]))
                    second = i;
            if (g_bodySamples >= 60 && g_bodyVotes[first] > 2.4 * g_bodySamples && g_bodyVotes[first] - g_bodyVotes[second] > 0.12 * g_bodySamples) {
                g_bodyLocked = true;
                g_bodyChoice = first;
                const BodyCandidate& c = g_bodyCandidates[first];
                Log("cockpit: the body's own orientation is used from now on (%u samples): %s, right = %cL%d, up = %cL%d, forward = %cL%d (agreement %.2f of 3 with the pose, %.2f for the next best)",
                    g_bodySamples, c.conjugate ? "conjugate" : "plain", c.axes.sign[0] < 0 ? '-' : '+', c.axes.axis[0], c.axes.sign[1] < 0 ? '-' : '+', c.axes.axis[1],
                    c.axes.sign[2] < 0 ? '-' : '+', c.axes.axis[2], g_bodyVotes[first] / g_bodySamples, g_bodyVotes[second] / g_bodySamples);
            }
        }
        if (g_bodyLocked)
            BodyAxes(raw.bodyQuat, g_bodyCandidates[g_bodyChoice], axes);
    }
    // Where the car is: its body when the game's own way to it was found, else the chase camera's target.
    const double apartNow[3] = {raw.pos[0] - (raw.hasBody ? raw.body[0] : raw.pos[0]), raw.pos[1] - (raw.hasBody ? raw.body[1] : raw.pos[1]),
                                raw.pos[2] - (raw.hasBody ? raw.body[2] : raw.pos[2])};
    // (standing, and the target within a metre of the body: the target against the body, along the body's axes; further apart it is still catching up with a stop)
    if (raw.hasBody && sqrt(Dot(g_velocity, g_velocity)) < 0.5 && Dot(apartNow, apartNow) < 1.0) {
        // (along the BODY's axes, right / up / forward: this offset used to be turned with the chase camera object's own orientation, which is not the body's on
        // bumps and in turns, so the seat swayed inside the car by the offset (about 0.45 m) times the angle between the two)
        for (int k = 0; k < 3; k++) {
            const double local = Dot(apartNow, axes[k]);
            g_restOffset[k] = g_restValid ? 0.9 * g_restOffset[k] + 0.1 * local : local;
        }
        g_restValid = true;
    }
    double seatHere[3] = {seat[0], seat[1], seat[2]};
    if (raw.hasBody && g_restValid) {
        for (int j = 0; j < 3; j++)
            seatHere[j] += g_restOffset[j];
    }
    double position[3];
    for (int i = 0; i < 3; i++)
        position[i] = origin[i] + axes[0][i] * seatHere[0] + axes[1][i] * seatHere[1] + axes[2][i] * seatHere[2];
    static int bodyLogged = -1;
    if (bodyLogged != (raw.hasBody ? 1 : 0)) { // (once, and again if the body is lost or found)
        bodyLogged = raw.hasBody ? 1 : 0;
        Log("cockpit: %s", raw.hasBody ? "the seat is placed on the car's body" : "the car's body was not found, the seat is placed on the chase camera's target");
    }
    for (int i = 0; i < 3; i++) {
        g_probe.origin[i] = origin[i];
        g_probe.camPos[i] = cam.pos[i];
        for (int k = 0; k < 3; k++)
            g_probe.axes[k][i] = axes[k][i];
        g_probe.camAxes[0][i] = cam.right[i];
        g_probe.camAxes[1][i] = cam.up[i];
        g_probe.camAxes[2][i] = cam.fwd[i];
    }
    g_probe.frame = g_vr.frameCount;
    g_probe.hasBody = raw.hasBody && g_bodyLocked;
    g_probe.valid = true;
    const double* camera[3] = {cam.right, cam.up, cam.fwd};
    const double offset[3] = {cam.pos[0] - position[0], cam.pos[1] - position[1], cam.pos[2] - position[2]};
    cameraToSeat = Identity();
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++)
            cameraToSeat.m[i][k] = Dot(camera[i], axes[k]);
        cameraToSeat.m[3][k] = Dot(offset, axes[k]);
    }
    return true;
}

void ChaseCameraPoll()
{
    static unsigned frame = 0, tries = 0, nextTry = 1, bodyTries = 0;
    frame++;
    if (g_hooked && !g_bodyResolved && bodyTries < 20 && frame % 60 == 0) { // (the AI HUD may be hooked a little after the chase camera's update)
        bodyTries++;
        if (ResolveBody())
            bodyTries = 20;
        else if (bodyTries == 20)
            Log("cockpit: the AI HUD's update was never found: the chase camera's target is the car's place");
    }
    if (g_hooked || !g_cfg.vr || !g_cfg.cockpit || tries >= 5 || frame < nextTry)
        return;
    tries++;
    nextTry = frame + 120;
    const BYTE* found = NULL;
    unsigned count = 0;
    if (!FindUpdate(found, count)) {
        Log("cockpit: the chase camera's update was not found by its bytes (%u matches) [try %u of 5]: the cockpit view is not available", count, tries);
        return;
    }
    BYTE expected[kUpdateStolen];
    memcpy(expected, found, kUpdateStolen);
    g_origUpdate = (UpdateFn)InstallInlineHook((void*)found, expected, kUpdateStolen, (void*)H_Update);
    g_hooked = g_origUpdate != NULL;
    tries = 5;
    Log("cockpit: the chase camera's update at %p %s", found, g_hooked ? "hooked: the seat is placed in the car's frame" : "NOT HOOKED: the cockpit view is not available");
    if (g_hooked && ResolveBody())
        bodyTries = 20;
}
