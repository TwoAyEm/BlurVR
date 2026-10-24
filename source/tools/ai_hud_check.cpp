// Host check of src/ai_hud.cpp (NOTES.md section 26): the signature scan finds a function that starts like the game's UIAiHud update, the inline hook forwards to
// it, the hook reads the slots it leaves behind (fresh and stale markers, children, an unreadable slot); the detour in the middle of the update
// records the markers and moves them to slots, the stub keeps every register, the game's skip of cars behind the camera is switched off and on; the world markers'
// geometry and the device calls they make are checked against a recording device.
// The "game" is a function written in assembly below: the real bytes of the update's prologue and of the two places inside it that are patched, so the scan finds it
// exactly once in this very program and the hook, the detour and the patch go into real code.  Built and run by tools/ai_hud_check.py.
#include <stdio.h>
#include "ai_hud.cpp" // (the module itself, found through -I src, to reach what it keeps private)

// ---- the fake game ------------------------------------------------------------------------------------------------------------------------------------------
extern "C" {
extern char fake_aihud_start[];
int fake_view() { return 1; }
unsigned fake_skip = 0;     // bit n: the update skips car n (as it skips a car behind the camera)
int fake_badidx = 0;        // the update puts a wrong car number on its stack (a build whose stack is laid out differently)
int fake_rival = -1;        // the car the update flags as the rival
unsigned fake_edx[3], fake_esi[3], fake_xmm3[3]; // what the registers held after the detour, per car
float fake_pi = 3.14159f;
float fake_cam[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // the camera the update "works out": a 3x3 matrix (row by row), then the position
unsigned char fake_billboard = 1;       // the game's UI.HUD.BillboardToCarInsteadOfCamera
unsigned char fake_billboard_seen = 99; // what it was while the update ran
unsigned fake_drew[3];                  // the cars whose markers the update "drew" (passed its limit test)
void fake_copy_cam(float* destination)
{
    memcpy(destination, fake_cam, sizeof(fake_cam));
}
}

// slot layout of the fake gadget, as the game's (ai_hud.cpp)
static BYTE g_gadget[0x300];
static BYTE g_slots[3 * 0x34];
static BYTE g_elements[3][13][0x200]; // [slot][0 = root, 1..12 = children]
static int g_step = 0;

static void SetFloat(BYTE* element, size_t offset, float v)
{
    memcpy(element + offset, &v, 4);
}
static float GetFloat(const BYTE* element, size_t offset)
{
    float v;
    memcpy(&v, element + offset, 4);
    return v;
}

// What the "game" does before it goes through its cars: the roots' world positions and alphas of this step (the cars it will not skip).  Step 0: cars 0 and 2 are
// processed, car 1 is skipped; in step 2 all three.
extern "C" void fake_body(void* self)
{
    (void)self;
    fake_billboard_seen = fake_billboard;
    memset(fake_drew, 0, sizeof(fake_drew));
    fake_skip = g_step == 2 ? 0 : 2;
    for (int car = 0; car < 3; car++) {
        if (fake_skip & (1u << car))
            continue;
        BYTE* root = g_elements[car][0];
        SetFloat(root, 0x70, 10.0f * (car + 1) + g_step);
        SetFloat(root, 0x74, 1.5f);
        SetFloat(root, 0x78, 40.0f);
        SetFloat(root, 0x58, 0.5f + 0.1f * car);
    }
}

// UIAiHud's update, as far as it matters here: the prologue bytes up to the first call, a call to fake_body, then a loop over three cars that does what the game does
// around the two patched places: the plane test (at +0x3FA, never run here, only its bytes are there), and at +0x68B the store of the root's world position and the
// calls that draw the targeting elements (their pointers are null here).  Before it a car's locals are put on the stack where the game keeps them (+0x3C car number,
// +0x30 markers drawn so far, +0x20 alpha, +0x1A / +0x1B rival / winner), the camera is put at +0x98 (matrix) and +0xBC (position) as the real update has it, the
// registers the detour must not change get values, and after it they are recorded.
asm(".text\n"
    ".globl _fake_aihud_start\n"
    ".p2align 4\n"
    "_fake_aihud_start:\n"
    ".byte 0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x84,0x01,0x00,0x00,0x53,0x56,0x57,0x8B,0xF9\n"
    ".byte 0x80,0xBF,0x44,0x01,0x00,0x00,0x00\n"
    ".byte 0x0F,0x84\n.long fake_done - . - 4\n"
    ".byte 0x80,0xBF,0x90,0x01,0x00,0x00,0x00\n"
    ".byte 0x0F,0x85\n.long fake_done - . - 4\n"
    ".byte 0x83,0xBF,0xAC,0x01,0x00,0x00,0xFF\n"
    ".byte 0x0F,0x84\n.long fake_done - . - 4\n"
    ".byte 0xE8\n.long _fake_view - . - 4\n"
    ".byte 0x8B,0xD8,0x89,0x5C,0x24,0x24\n"
    "pushl %edi\n"
    "call _fake_body\n"
    "addl $4, %esp\n"
    "leal 0x98(%esp), %eax\n"
    "pushl %eax\n"
    "call _fake_copy_cam\n"
    "addl $4, %esp\n"
    "xorl %ebx, %ebx\n"
    "movl $0, 0x30(%esp)\n"
    "fake_loop:\n"
    "cmpl $3, %ebx\n"
    "jge fake_done\n"
    "btl %ebx, _fake_skip\n"
    "jc fake_next\n"
    "movl %ebx, 0x3C(%esp)\n"
    "cmpl $0, _fake_badidx\n"
    "je 1f\n"
    "incl 0x3C(%esp)\n"
    "1:\n"
    "movl 0x1B4(%edi), %esi\n"
    "imull $0x34, %ebx, %eax\n"
    "addl %eax, %esi\n"
    "movl (%esi), %eax\n"
    "movl 0x58(%eax), %ecx\n"
    "movl %ecx, 0x20(%esp)\n"
    "movb $0, 0x1A(%esp)\n"
    "movb $0, 0x1B(%esp)\n"
    "cmpl _fake_rival, %ebx\n"
    "jne 2f\n"
    "movb $1, 0x1A(%esp)\n"
    "2:\n"
    "addl $0x70, %eax\n"
    "movss 8(%eax), %xmm0\n"
    "movl $0xCAFEBABE, %edx\n"
    "movss _fake_pi, %xmm3\n"
    "jmp fake_site\n"
    ".org _fake_aihud_start + 0x3F0\n"
    ".byte 0xE9\n.long fake_plane_end - . - 4\n"
    ".org _fake_aihud_start + 0x3FA\n"
    ".byte 0x0F,0x57,0xC9,0x0F,0x2F,0xC8,0x89,0x44,0x24,0x38,0x0F,0x87,0xD4,0x06,0x00,0x00,0x33,0xC9,0xBA,0x00,0x00,0x04,0x00,0x8B,0x04,0x8E\n"
    "fake_plane_end:\n"
    ".org _fake_aihud_start + 0x68B\n"
    "fake_site:\n"
    ".byte 0xF3,0x0F,0x11,0x40,0x08\n"
    ".byte 0x8B,0x4E,0x14,0x85,0xC9,0x74,0x07,0x8B,0x01,0x8B,0x50,0x28,0xFF,0xD2\n"
    ".byte 0x8B,0x4E,0x18,0x85,0xC9,0x74,0x07,0x8B,0x01,0x8B,0x50,0x28,0xFF,0xD2\n"
    ".byte 0x8B,0x4E,0x1C,0x85,0xC9,0x74,0x07,0x8B,0x01,0x8B,0x50,0x28,0xFF,0xD2\n"
    "jmp fake_tail\n"
    ".org _fake_aihud_start + 0x6EF\n" // the choice of the marker's yaw (never run here: the bytes are there for the scan)
    ".byte 0x80,0x3D\n.long _fake_billboard\n.byte 0x00,0x0F,0x84\n.long fake_done - . - 4\n"
    ".byte 0x8B,0x4C,0x24,0x1C,0x83,0xB9,0xE0,0x00,0x00,0x00,0x05,0x0F,0x84\n.long fake_done - . - 4\n"
    ".byte 0x8B,0x44,0x24,0x38\n"
    "fake_tail:\n"
    "movl %edx, _fake_edx(,%ebx,4)\n"
    "movl %esi, _fake_esi(,%ebx,4)\n"
    "movss %xmm3, _fake_xmm3(,%ebx,4)\n"
    "cmpl $0, 0x20(%esp)\n"
    "je fake_next\n"
    "movl 0x30(%esp), %eax\n"
    "cmpl 0x194(%edi), %eax\n"
    "jge fake_next\n"
    "movl $1, _fake_drew(,%ebx,4)\n"
    "incl 0x30(%esp)\n"
    "fake_next:\n"
    "incl %ebx\n"
    "jmp fake_loop\n"
    "fake_done:\n"
    "popl %edi\n"
    "popl %esi\n"
    "popl %ebx\n"
    "movl %ebp, %esp\n"
    "popl %ebp\n"
    "ret\n");

// HudDummy's update, as far as the scan and the hook go: the 6 bytes the hook takes, the bytes of the start pattern (the branches go to the body below), a body that
// writes a "sway" into the gadget's outputs, and at +0x297 the tail the scan checks.
asm(".text\n"
    ".globl _fake_sway_start\n"
    ".p2align 4\n"
    "_fake_sway_start:\n"
    ".byte 0x83,0xEC,0x28,0x56,0x8B,0xF1\n"
    ".byte 0x80,0xBE,0x44,0x01,0x00,0x00,0x00\n"
    ".byte 0x0F,0x84\n.long fake_sway_body - . - 4\n"
    ".byte 0x8B,0x8E,0xA0,0x01,0x00,0x00,0x85,0xC9\n"
    ".byte 0x0F,0x84\n.long fake_sway_body - . - 4\n"
    ".byte 0x8B,0x01,0xF3,0x0F,0x10,0x05\n.long 0\n"
    ".byte 0xF3,0x0F,0x59,0x05\n.long 0\n"
    ".byte 0x8B,0x50,0x4C,0xF3,0x0F,0x11,0x44,0x24,0x04\n"
    "fake_sway_body:\n"
    "movl $0x41200000, 0x70(%esi)\n"  // position x = 10
    "movl $0x41A00000, 0x74(%esi)\n"  // position y = 20
    "movl $0x3E800000, 0x84(%esi)\n"  // rotation 0.25
    "movl $0x3F866666, 0x88(%esi)\n"  // scale 1.05
    "movl $0x3F866666, 0x8C(%esi)\n"
    "popl %esi\n"
    "addl $0x28, %esp\n"
    "ret\n"
    ".org _fake_sway_start + 0x297\n"
    ".byte 0x0F,0x28,0xEC,0xF3,0x0F,0x59,0xE8,0xF3,0x0F,0x58,0xAE,0x90,0x01,0x00,0x00,0xF3,0x0F,0x10,0x86,0x70,0x01,0x00,0x00,0xF3,0x0F,0x11,0x6E,0x70,0x0F,0x28,0xE8\n"
    ".byte 0xF3,0x0F,0x59,0xEC,0xF3,0x0F,0x59,0xEA,0xF3,0x0F,0x58,0xAE,0x94,0x01,0x00,0x00,0xF3,0x0F,0x11,0x96,0x84,0x01,0x00,0x00,0xF3,0x0F,0x10,0x96,0x98,0x01,0x00,0x00\n"
    ".byte 0xC3\n");
extern "C" char fake_sway_start[];

static int g_failures = 0;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            g_failures++;                                          \
            printf("FAIL line %d: %s -- ", __LINE__, #cond);       \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)

// ---- a device that records what the world markers send to it (slot 35 is ColorFill; the other calls go through g_deviceOriginals) --------------------------------
struct Recorder {
    static HRESULT STDMETHODCALLTYPE SetTexture(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9* t)
    {
        events.push_back(t ? 1 : 0);
        return D3D_OK;
    }
    static HRESULT STDMETHODCALLTYPE SetScissorRect(IDirect3DDevice9*, const RECT* r)
    {
        scissor = *r;
        return D3D_OK;
    }
    static HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(IDirect3DDevice9*, D3DPRIMITIVETYPE type, UINT primitives, const void* data, UINT stride)
    {
        if (type != D3DPT_TRIANGLESTRIP || primitives != 2 || stride != sizeof(ScreenVertex))
            bad++;
        Draw d;
        d.scissor = scissor;
        memcpy(d.v, data, sizeof(d.v));
        draws.push_back(d);
        events.push_back(2);
        return D3D_OK;
    }
    static HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DDevice9*, IDirect3DSurface9* surface, const RECT* r, D3DCOLOR color)
    {
        if (color != 0)
            bad++;
        fills.push_back(*r);
        fillSurfaces.push_back(surface);
        events.push_back(3);
        return D3D_OK;
    }
    struct Draw {
        RECT scissor;
        ScreenVertex v[4];
    };
    static void Clear()
    {
        events.clear();
        draws.clear();
        fills.clear();
        fillSurfaces.clear();
    }
    static std::vector<int> events;
    static std::vector<Draw> draws;
    static std::vector<RECT> fills;
    static std::vector<IDirect3DSurface9*> fillSurfaces;
    static RECT scissor;
    static int bad;
};
std::vector<int> Recorder::events;
std::vector<Recorder::Draw> Recorder::draws;
std::vector<RECT> Recorder::fills;
std::vector<IDirect3DSurface9*> Recorder::fillSurfaces;
RECT Recorder::scissor;
int Recorder::bad = 0;

int main()
{
    g_cfg.vr = 1; // (the markers run with the VR half: Enabled=1, Stereo=1, and the headset's runtime up)
    g_vr.active = true;
    g_cfg.keyDump = 0;
    g_cfg.seamGutter = 0;
    g_backBufferWidth = 4032;
    g_backBufferHeight = 2240;
    // the fake gadget: visible, flag 0x190 clear, view 0, three slots, a limit of five drawn markers; car 2's ai_damage child pointer is unreadable
    g_gadget[0x144] = 1;
    g_gadget[0x190] = 0;
    int view = 0, count = 3, limit = 5;
    uintptr_t slots = (uintptr_t)g_slots;
    memcpy(g_gadget + 0x1AC, &view, 4);
    memcpy(g_gadget + 0x1B4, &slots, sizeof(slots));
    memcpy(g_gadget + 0x1BC, &count, 4);
    memcpy(g_gadget + 0x194, &limit, 4);
    for (int car = 0; car < 3; car++) {
        uintptr_t root = (uintptr_t)g_elements[car][0];
        memcpy(g_slots + car * 0x34, &root, sizeof(root));
        for (int c = 1; c <= 12; c++) {
            uintptr_t child = (c == 4 && car == 2) ? 0x1234 : (uintptr_t)g_elements[car][c];
            if (c == 5 || c == 6 || c == 7)
                child = 0; // the targeting elements: none, so the game's calls that draw them are skipped
            memcpy(g_slots + car * 0x34 + 4 * c, &child, sizeof(child));
        }
        SetFloat(g_elements[car][3], 0x58, 0.75f); // ai_perks alpha
        SetFloat(g_elements[car][3], 0x110, 100.0f + car);
    }
    CHECK(Readable(g_gadget, sizeof(g_gadget)), "gadget readable");
    CHECK(!Readable((void*)0x1234, 4), "0x1234 unreadable");
    CHECK(!Readable(NULL, 4), "NULL unreadable");

    // the scan finds the fake function, once, and the hook and the detour go in
    AiHudPoll();
    CHECK(g_hooked, "hook installed");
    CHECK(g_siteInstalled && g_aiSiteResume, "detour installed");
    CHECK(g_planeJump == (const BYTE*)fake_aihud_start + 0x404, "plane test found at +0x404");
    CHECK(g_origSway != NULL, "the HUD sway update is hooked");
    typedef void(__attribute__((fastcall)) * UpdateFn)(void*, void*);
    UpdateFn update = (UpdateFn)fake_aihud_start;

    update(g_gadget, NULL); // step 0: cars 0 and 2 are processed, car 1 is skipped
    Snapshot snap;
    CHECK(CopyLatest(snap), "snapshot published");
    CHECK(snap.count == 3, "3 slots (got %d)", snap.count);
    CHECK(snap.marker[0].fresh && !snap.marker[1].fresh && snap.marker[2].fresh, "first call: cars 0 and 2 processed, car 1 not (%d %d %d)", snap.marker[0].fresh,
          snap.marker[1].fresh, snap.marker[2].fresh);
    CHECK(snap.siteCount == 2 && snap.site[0].car == 0 && snap.site[1].car == 2, "the detour saw cars 0 and 2 (%d)", snap.siteCount);
    CHECK(snap.marker[0].drawn && snap.marker[2].drawn && !snap.marker[1].drawn, "drawn: cars 0 and 2 (%d %d %d)", snap.marker[0].drawn, snap.marker[1].drawn,
          snap.marker[2].drawn);
    CHECK(snap.marker[0].root.pos[0] == 10.0f && snap.marker[2].root.pos[0] == 30.0f && snap.marker[0].root.pos[2] == 40.0f, "world positions read");
    CHECK(snap.marker[0].child[kChildPerks].present && snap.marker[0].child[kChildPerks].alpha == 0.75f, "child ai_perks read");
    CHECK(!snap.marker[2].child[kChildDamage].present, "an unreadable child pointer is skipped, not followed");
    // the detour changed no register of the game's: the values the "game" put in edx, esi and xmm3 were there after it
    unsigned pi;
    memcpy(&pi, &fake_pi, 4);
    CHECK(fake_edx[0] == 0xCAFEBABE && fake_edx[2] == 0xCAFEBABE && fake_xmm3[0] == pi && fake_xmm3[2] == pi, "the detour keeps edx and xmm3 (%08x %08x)", fake_edx[0], fake_xmm3[0]);
    CHECK(fake_esi[0] == (unsigned)(uintptr_t)(g_slots + 0 * 0x34) && fake_esi[2] == (unsigned)(uintptr_t)(g_slots + 2 * 0x34), "and esi");

    g_step = 1;
    update(g_gadget, NULL); // cars 0 and 2 moved, car 1 did not
    CHECK(CopyLatest(snap), "snapshot 2");
    CHECK(snap.marker[0].fresh && !snap.marker[1].fresh && snap.marker[2].fresh, "car 1 is stale (%d %d %d)", snap.marker[0].fresh, snap.marker[1].fresh,
          snap.marker[2].fresh);
    CHECK(snap.marker[0].root.pos[0] == 11.0f, "moved position read (%g)", snap.marker[0].root.pos[0]);

    g_step = 2;
    update(g_gadget, NULL);
    CHECK(CopyLatest(snap) && snap.marker[1].fresh && snap.siteCount == 3, "car 1 processed again");

    // the draw limit: with room for one marker, car 2 is not drawn; a car flagged as the rival is drawn whatever the limit
    int one = 1;
    memcpy(g_gadget + 0x194, &one, 4);
    update(g_gadget, NULL);
    CHECK(CopyLatest(snap) && snap.marker[0].drawn && !snap.marker[1].drawn && !snap.marker[2].drawn, "limit 1: only car 0 (%d %d %d)", snap.marker[0].drawn, snap.marker[1].drawn,
          snap.marker[2].drawn);
    fake_rival = 2;
    update(g_gadget, NULL);
    CHECK(CopyLatest(snap) && snap.marker[2].drawn, "the rival is drawn past the limit");
    fake_rival = -1;
    memcpy(g_gadget + 0x194, &limit, 4);

    // an invisible gadget: the game's function returns at once, the hook publishes a snapshot without sites
    g_gadget[0x144] = 0;
    update(g_gadget, NULL);
    CHECK(CopyLatest(snap) && snap.siteCount == 0, "the hook still runs when the gadget is hidden: no sites (%d)", snap.siteCount);
    g_gadget[0x144] = 1;

    // a gadget whose slot vector is nonsense is left alone
    int absurd = 100000;
    memcpy(g_gadget + 0x1BC, &absurd, 4);
    update(g_gadget, NULL);
    memcpy(g_gadget + 0x1BC, &count, 4);

    // the projection: a camera at the origin looking down +z with unit projection scales, a 1000x500 layer
    g_backBufferWidth = 1000;
    g_backBufferHeight = 500;
    MainCameraPose pose;
    memset(&pose, 0, sizeof(pose));
    pose.right[0] = 1;
    pose.up[1] = 1;
    pose.fwd[2] = 1;
    pose.projX = pose.projY = 1;
    float world[3] = {1.0f, 0.5f, 2.0f};
    Predicted p = Predict(&pose, world);
    CHECK(p.inFront && p.ok && fabs(p.ax - 750) < 1e-6 && fabs(p.ay - 187.5) < 1e-6, "pixel (%.2f %.2f)", p.ax, p.ay);
    float behind[3] = {0.0f, 0.0f, -3.0f};
    CHECK(!Predict(&pose, behind).inFront, "behind the camera");
    // a camera turned 90 degrees to the right: the point straight ahead of the world axis lands on its left edge
    pose.right[0] = 0;
    pose.right[2] = -1;
    pose.fwd[2] = 0;
    pose.fwd[0] = 1;
    float side[3] = {2.0f, 0.0f, 0.0f};
    p = Predict(&pose, side);
    CHECK(p.inFront && fabs(p.ax - 500) < 1e-6, "camera turned: ahead is the centre (%.2f)", p.ax);

    // ---- world markers: which markers are drawn, going by the flags (what is left without the detour)
    {
        Marker m;
        memset(&m, 0, sizeof(m));
        m.root.present = true;
        m.fresh = true;
        m.root.alpha = 1.0f;
        m.child[kChildPerks].present = m.child[kChildDamage].present = true;
        m.child[kChildPerks].flags = m.child[kChildDamage].flags = 0x3A; // the game cleared the flag: it drew the elements
        CHECK(MarkerDrawn(m), "drawn: written, visible, elements drawn");
        m.child[kChildPerks].flags = m.child[kChildDamage].flags = 0x4003A; // flagged and never drawn (past the draw limit)
        CHECK(!MarkerDrawn(m), "not drawn: elements still flagged");
        m.child[kChildDamage].flags = 0x3A;
        CHECK(MarkerDrawn(m), "drawn: one of the two elements was drawn");
        m.root.alpha = 0.0f;
        CHECK(!MarkerDrawn(m), "not drawn: alpha 0");
        m.root.alpha = 1.0f;
        m.fresh = false;
        CHECK(!MarkerDrawn(m), "not drawn: stale");
    }

    // ---- world markers: the cut-out and the billboard
    g_backBufferWidth = 4032;
    g_backBufferHeight = 2240;
    MainCameraPose cam;
    memset(&cam, 0, sizeof(cam));
    cam.right[0] = 1;
    cam.up[1] = 1;
    cam.fwd[2] = 1;
    cam.projX = 0.872;
    cam.projY = 1.5697;
    const MarkerParams params = FixedMarkerParams();
    // where the car is: the cut-out is around the car's own pixel
    auto inPlace = [&](const float* where, Cut& cut) {
        Predicted pr = Predict(&cam, where);
        cut.px = pr.ax;
        cut.py = pr.ay;
        cut.z = pr.cam[2];
        cut.projX = cam.projX;
        cut.projY = cam.projY;
        return pr.ok;
    };
    // the billboard of a marker at a world position, seen from a camera
    auto Build = [&](const MainCameraPose& c, const Matrix4& head, const float* w, const Cut& cut, MarkerGeom& g) {
        const double d[3] = {w[0] - c.pos[0], w[1] - c.pos[1], w[2] - c.pos[2]};
        double a[3];
        ToCameraSpace(c, d, a);
        return BuildMarkerGeometry(c, head, a, cut, params, g);
    };
    {
        MarkerGeom g;
        Cut cut;
        float anchor[3] = {0.0f, 0.0f, 20.0f};
        CHECK(inPlace(anchor, cut) && Build(cam, Identity(), anchor, cut, g), "marker 20 m ahead");
        CHECK(g.rect[0] == 1866 && g.rect[2] == 2166 && g.rect[1] == 1005 && g.rect[3] == 1270, "cut-out %d %d %d %d (expected 1866 1005 2166 1270)", g.rect[0], g.rect[1],
              g.rect[2], g.rect[3]);
        CHECK(fabs(g.corner[0][2] - 20) < 1e-9 && fabs(g.corner[3][2] - 20) < 1e-9, "billboard at the anchor's depth");
        double width = g.corner[1][0] - g.corner[0][0], height = g.corner[0][1] - g.corner[2][1];
        CHECK(fabs(width - 300 / 87.9) < 0.01 && fabs(height - 265 / 87.9) < 0.01, "billboard %.3f x %.3f m (expected 3.413 x 3.015)", width, height);
        CHECK(fabs(g.uv[0] - 1866.0 / 4032) < 1e-6 && fabs(g.uv[3] - 1270.0 / 2240) < 1e-6, "texture coordinates");

        // far away: made as wide as 3 degrees; very near: no wider than 20 degrees
        float distant[3] = {0.0f, 0.0f, 100.0f};
        CHECK(inPlace(distant, cut) && Build(cam, Identity(), distant, cut, g), "marker 100 m ahead");
        width = g.corner[1][0] - g.corner[0][0];
        CHECK(fabs(width - 2 * 100 * tan(1.5 * 3.14159265358979 / 180)) < 0.01, "minimum angle: width %.3f m (expected 5.236)", width);
        float closeAnchor[3] = {0.0f, 0.0f, 2.0f};
        CHECK(inPlace(closeAnchor, cut) && Build(cam, Identity(), closeAnchor, cut, g), "marker 2 m ahead");
        width = g.corner[1][0] - g.corner[0][0];
        CHECK(fabs(width - 2 * 2 * tan(10 * 3.14159265358979 / 180)) < 0.01, "maximum angle: width %.3f m (expected 0.705)", width);
        float tooNear[3] = {0.0f, 0.0f, 1.0f};
        CHECK(inPlace(tooNear, cut) && !Build(cam, Identity(), tooNear, cut, g), "nearer than MinDistance: not placed");

        // at the edge of the layer the cut-out is clamped and the billboard keeps the metres that are really there
        float edge[3] = {-10.5f, 0.0f, 10.0f};
        CHECK(inPlace(edge, cut) && Build(cam, Identity(), edge, cut, g), "marker at the left edge");
        double expectedLeft = (2016 * (1 + 0.872 * (-1.05))) / (2016 * 0.872 / 10);
        double leftOfAnchor = sqrt((g.corner[0][0] + 10.5) * (g.corner[0][0] + 10.5) + (g.corner[0][2] - 10.0) * (g.corner[0][2] - 10.0)); // (along the billboard, which faces the head)
        CHECK(g.rect[0] == 0 && fabs(leftOfAnchor - expectedLeft) < 0.02, "clamped cut-out: %d, %.3f m left of the anchor (expected %.3f)", g.rect[0], leftOfAnchor, expectedLeft);

        // the billboard is flat to the head's view, not turned to the car: seen from a head that looks along the game's forward axis it is across it whatever the car's
        // direction, and from one turned a quarter turn to the right (looking along +x) it is across that, upright, with its width kept
        CHECK(inPlace(edge, cut) && Build(cam, Identity(), edge, cut, g), "marker at the edge, head looking ahead");
        CHECK(fabs(g.corner[0][2] - g.corner[3][2]) < 1e-9 && fabs(g.corner[0][2] - g.corner[1][2]) < 1e-9, "face on to a head that looks ahead: one depth for all corners (%.4f %.4f)", g.corner[0][2],
              g.corner[3][2]);
        Matrix4 turned = Identity(); // the head's axes are the columns: right = -z, up = y, forward = +x
        turned.m[0][0] = 0;
        turned.m[2][0] = -1;
        turned.m[0][2] = 1;
        turned.m[2][2] = 0;
        CHECK(inPlace(anchor, cut) && Build(cam, turned, anchor, cut, g), "marker seen by a head turned to the right");
        CHECK(fabs(g.corner[0][0] - g.corner[1][0]) < 1e-9 && fabs(g.corner[0][0] - g.corner[3][0]) < 1e-9, "its plane is across where the head looks (x %.4f %.4f)", g.corner[0][0], g.corner[3][0]);
        CHECK(fabs(g.corner[0][1] - g.corner[1][1]) < 1e-9, "upright");
        double dz = g.corner[1][2] - g.corner[0][2];
        CHECK(fabs(fabs(dz) - 300 / 87.9) < 0.01, "width kept (%.3f)", fabs(dz));
        // the same marker from heads in different places and directions has the same shape: face on is the same size everywhere in the field of view
        Matrix4 moved = Identity();
        moved.m[3][0] = -5.0; // the head is 5 m to the right of the game camera
        CHECK(inPlace(anchor, cut) && Build(cam, moved, anchor, cut, g), "marker seen from a head to the side");
        CHECK(fabs(g.corner[0][2] - g.corner[3][2]) < 1e-9, "still across the head's forward axis");

        // scaled about the lift: with the minimum angle at work (a car at 100 m) the picture's bottom stays that far above the anchor times the growth, about the 0.12 m height
        float distantCar[3] = {0.0f, 0.0f, 100.0f};
        CHECK(inPlace(distantCar, cut) && Build(cam, Identity(), distantCar, cut, g), "far marker");
        {
            const double pxX = 2016 * 0.872 / 100, pxY = 1120 * 1.5697 / 100;
            const double mL = (cut.px - g.rect[0]) / pxX, mR = (g.rect[2] - cut.px) / pxX, mU = (cut.py - g.rect[1]) / pxY, mD = (g.rect[3] - cut.py) / pxY;
            const double growth = 2 * 100 * tan(1.5 * 3.14159265358979 / 180) / (mL + mR);
            CHECK(growth > 1.3, "the minimum angle is at work (growth %.3f)", growth);
            CHECK(fabs(g.corner[0][1] - (0.12 + (mU - 0.12) * growth)) < 1e-6 && fabs(g.corner[2][1] - (0.12 + (-mD - 0.12) * growth)) < 1e-6,
                  "scaled about the lift: top %.4f (expected %.4f) bottom %.4f (expected %.4f)", g.corner[0][1], 0.12 + (mU - 0.12) * growth, g.corner[2][1], 0.12 + (-mD - 0.12) * growth);
        }

        // a car behind the camera has a place too: its picture comes from a slot, and the billboard stands behind the camera
        float rear[3] = {2.0f, 0.0f, -20.0f};
        Cut slot3 = SlotCut(cam, params, 3);
        CHECK(Build(cam, Identity(), rear, slot3, g), "marker of a car behind the camera");
        CHECK(g.corner[0][2] < -15 && g.corner[3][2] < -15, "stands behind the camera (z %.2f)", g.corner[0][2]);
    }

    // ---- world markers: the slots tile the layer without overlap, the same pixel answers for the world position that is given to the game
    {
        int previousRight = -1;
        for (int k = 0; k < kRelocSlots; k++) {
            Cut c = SlotCut(cam, params, k);
            MarkerGeom g;
            float ahead[3] = {0.0f, 0.0f, 30.0f};
            CHECK(Build(cam, Identity(), ahead, c, g), "slot %d usable", k);
            CHECK(g.rect[0] >= previousRight && g.rect[2] <= 4032 && g.rect[0] >= 0, "slot %d: %d..%d after %d", k, g.rect[0], g.rect[2], previousRight);
            CHECK(g.rect[1] > 400 && g.rect[3] < 1700, "slot %d in the free band (y %d..%d)", k, g.rect[1], g.rect[3]);
            previousRight = g.rect[2];
            float w[3];
            CutWorld(cam, c, w);
            Predicted pr = Predict(&cam, w);
            CHECK(pr.ok && fabs(pr.ax - c.px) < 0.05 && fabs(pr.ay - c.py) < 0.05 && fabs(pr.cam[2] - c.z) < 1e-3, "slot %d: the world position gives pixel %.2f %.2f depth %.3f (cut %.2f %.2f %.2f)", k,
                  pr.ax, pr.ay, pr.cam[2], c.px, c.py, c.z);
        }
        MainCameraPose turned = cam; // a camera that looks elsewhere and sits elsewhere: the same pixels
        turned.pos[0] = 500;
        turned.pos[2] = -300;
        turned.right[0] = 0;
        turned.right[2] = -1;
        turned.fwd[2] = 0;
        turned.fwd[0] = 1;
        Cut c = SlotCut(turned, params, 5);
        float w[3];
        CutWorld(turned, c, w);
        Predicted pr = Predict(&turned, w);
        CHECK(pr.ok && fabs(pr.ax - c.px) < 0.05 && fabs(pr.ay - c.py) < 0.05, "a turned camera: the same pixel (%.2f %.2f against %.2f %.2f)", pr.ax, pr.ay, c.px, c.py);
    }

    // ---- world markers: what is sent to the device
    {
        static void* vtable[128];
        static void* deviceObject = vtable;
        IDirect3DDevice9* device = (IDirect3DDevice9*)&deviceObject;
        vtable[35] = (void*)Recorder::ColorFill;
        g_deviceOriginals[VT_SetTexture] = (void*)Recorder::SetTexture;
        g_deviceOriginals[VT_SetScissorRect] = (void*)Recorder::SetScissorRect;
        g_deviceOriginals[VT_DrawPrimitiveUP] = (void*)Recorder::DrawPrimitiveUP;
        g_cfg.vr = 1;
        const double a = 0.872, b = 1.5697;
        for (int e = 0; e < 3; e++)
            g_vr.eyeCameraDelta[e] = Identity();
        g_vr.eyeBox[0] = g_vr.eyeBox[1] = {-1 / a, 1 / a, 1 / b, 1 / b}; // each eye sees what the game's camera saw
        Snapshot fake;
        memset(&fake, 0, sizeof(fake));
        fake.count = 3;
        const float where[3][3] = {{0, 0, 20}, {3, 0, 30}, {-4, 1, 60}};
        for (int i = 0; i < 3; i++) {
            Marker& m = fake.marker[i];
            m.car = i + 1;
            m.fresh = true;
            m.drawn = i != 1; // car 2 is past the game's draw limit
            m.slot = -1;      // (the game drew them where the cars are)
            m.root.present = true;
            m.root.alpha = 1.0f;
            for (int k = 0; k < 3; k++)
                m.root.pos[k] = where[i][k];
        }
        void* texture = (void*)0x10;
        void* surface = (void*)0x20;
        void* glow = (void*)0x30;
        int n = DrawMarkers(device, (IDirect3DTexture9*)texture, (IDirect3DSurface9*)surface, NULL, 0, 0, fake, cam);
        CHECK(n == 2, "two markers drawn (got %d)", n);
        CHECK(Recorder::bad == 0, "draw calls and fills have the expected form (%d bad)", Recorder::bad);
        const int expectedEvents[] = {1, 2, 2, 2, 2, 0, 3, 3, 1};
        CHECK(Recorder::events.size() == 9 && std::equal(Recorder::events.begin(), Recorder::events.end(), expectedEvents),
              "texture bound, 4 quads (2 eyes x 2 markers), texture unbound, 2 fills, texture bound again (got %zu events)", Recorder::events.size());
        if (Recorder::draws.size() == 4 && Recorder::fills.size() == 2) {
            // far to near: the 60 m marker first (car 3), then the 20 m one (car 1)
            CHECK(Recorder::draws[0].scissor.right == 2016 && Recorder::draws[1].scissor.left == 2016, "one quad per eye half");
            const Recorder::Draw& closeDraw = Recorder::draws[2]; // car 1, left eye
            CHECK(fabs(closeDraw.v[0].x - (1866 / 2.0 - 0.5)) < 0.01 && fabs(closeDraw.v[0].y - (1005 - 0.5)) < 0.01 && fabs(closeDraw.v[3].x - (2166 / 2.0 - 0.5)) < 0.01 &&
                      fabs(closeDraw.v[3].y - (1270 - 0.5)) < 0.01,
                  "car 1 in the left eye lands where the game drew it, at half the width: (%.2f %.2f) .. (%.2f %.2f)", closeDraw.v[0].x, closeDraw.v[0].y, closeDraw.v[3].x,
                  closeDraw.v[3].y);
            const Recorder::Draw& rightEyeDraw = Recorder::draws[3];
            CHECK(fabs(rightEyeDraw.v[0].x - (2016 + 1866 / 2.0 - 0.5)) < 0.01, "and in the right eye, on the right half (%.2f)", rightEyeDraw.v[0].x);
            CHECK(fabs(closeDraw.v[0].rhw - 1.0 / 20) < 1e-6, "depth: 1/z of the car (%.5f)", closeDraw.v[0].rhw);
            CHECK(Recorder::fills[1].left == 1866 && Recorder::fills[1].top == 1005 && Recorder::fills[1].right == 2166 && Recorder::fills[1].bottom == 1270,
                  "the cut-out of car 1 is cleared from the layer");
        }

        // the glow of the UI is cleared where the markers were: the same part of its (much smaller) picture, and a margin
        Recorder::Clear();
        n = DrawMarkers(device, (IDirect3DTexture9*)texture, (IDirect3DSurface9*)surface, (IDirect3DSurface9*)glow, 135, 66, fake, cam);
        CHECK(n == 2 && Recorder::fills.size() == 4, "two markers: a fill of the layer and of the glow each (%zu fills)", Recorder::fills.size());
        if (Recorder::fills.size() == 4) {
            CHECK(Recorder::fillSurfaces[1] == (IDirect3DSurface9*)glow && Recorder::fillSurfaces[0] == (IDirect3DSurface9*)surface, "the layer, then the glow");
            const RECT& g = Recorder::fills[3]; // car 1: layer 1866 1005 2166 1270 on 4032 x 2240
            CHECK(g.left == 58 && g.top == 25 && g.right == 77 && g.bottom == 42, "glow cut-out %ld %ld %ld %ld (expected 58 25 77 42)", g.left, g.top, g.right, g.bottom);
        }

        // a marker the game moved to a slot: its picture comes from the slot, wherever the car is, even behind the camera; seen from a head that looks back it is in front
        Recorder::Clear();
        Snapshot moved;
        memset(&moved, 0, sizeof(moved));
        moved.count = 1;
        moved.marker[0].car = 7;
        moved.marker[0].fresh = moved.marker[0].drawn = true;
        moved.marker[0].slot = 2;
        moved.marker[0].root.present = true;
        moved.marker[0].root.alpha = 1.0f;
        moved.marker[0].root.pos[0] = 0;
        moved.marker[0].root.pos[1] = 0;
        moved.marker[0].root.pos[2] = -20; // 20 m behind the game's camera
        n = DrawMarkers(device, (IDirect3DTexture9*)texture, (IDirect3DSurface9*)surface, NULL, 0, 0, moved, cam);
        CHECK(n == 1 && Recorder::draws.size() == 0 && Recorder::fills.size() == 1, "behind a head that looks ahead: placed, not drawn (%zu quads)", Recorder::draws.size());
        const Cut slot2 = SlotCut(cam, FixedMarkerParams(), 2);
        CHECK(Recorder::fills.size() == 1 && Recorder::fills[0].left < slot2.px && Recorder::fills[0].right > slot2.px, "the cut-out is the slot's (%ld..%ld around %.0f)", Recorder::fills[0].left,
              Recorder::fills[0].right, slot2.px);
        Recorder::Clear();
        for (int e = 0; e < 3; e++) { // a head turned half a turn: x and z reversed
            g_vr.eyeCameraDelta[e] = Identity();
            g_vr.eyeCameraDelta[e].m[0][0] = -1;
            g_vr.eyeCameraDelta[e].m[2][2] = -1;
        }
        n = DrawMarkers(device, (IDirect3DTexture9*)texture, (IDirect3DSurface9*)surface, NULL, 0, 0, moved, cam);
        CHECK(n == 1 && Recorder::draws.size() == 2, "a head that looks back sees it in both eyes (%zu quads)", Recorder::draws.size());
        for (int e = 0; e < 3; e++)
            g_vr.eyeCameraDelta[e] = Identity();

        // in a frame that is being dumped nothing is cleared, and no marker drawn adds nothing
        Recorder::Clear();
        for (int i = 0; i < 3; i++)
            fake.marker[i].drawn = false;
        CHECK(DrawMarkers(device, (IDirect3DTexture9*)texture, (IDirect3DSurface9*)surface, NULL, 0, 0, fake, cam) == 0 && Recorder::events.empty(), "no marker drawn: nothing is sent");
    }

    // ---- world markers: the detour moves the markers to slots, and the game's skip of cars behind the camera is switched off while it does
    {
        g_cfg.vr = 1;
        cam.frame = g_vr.frameCount;
        PublishMainCameraPose(cam);
        static const BYTE nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
        BYTE original[6];
        memcpy(original, g_planeOriginal, 6);
        CHECK(memcmp(g_planeJump, original, 6) == 0 && original[0] == 0x0F && original[1] == 0x87, "before: the game's own ja");
        AiHudPoll();
        CHECK(g_planePatched && memcmp(g_planeJump, nop6, 6) == 0, "the skip of cars behind the camera is a nop while markers are moved");
        g_step = 0;
        update(g_gadget, NULL); // cars 0 and 2 are processed and drawn
        CHECK(CopyLatest(snap) && snap.siteCount == 2, "the detour saw two cars");
        CHECK(snap.site[0].slot == 3 && snap.site[1].slot == 4 && snap.marker[0].slot == 3 && snap.marker[2].slot == 4 && snap.marker[1].slot == -1, "slots 3 and 4, the middle ones (%d %d)",
              snap.site[0].slot, snap.site[1].slot);
        CHECK(fabs(snap.marker[0].root.pos[0] - 10) < 1e-4 && fabs(snap.marker[2].root.pos[0] - 30) < 1e-4, "the snapshot keeps where the cars are (%g %g)", snap.marker[0].root.pos[0],
              snap.marker[2].root.pos[0]);
        // what the game's elements now hold: the slots' world positions, which put the pictures at the slots' pixels at the slots' depth
        for (int slotIndex = 3; slotIndex < 5; slotIndex++) {
            int car = slotIndex == 3 ? 0 : 2;
            float moved3[3] = {GetFloat(g_elements[car][0], 0x70), GetFloat(g_elements[car][0], 0x74), GetFloat(g_elements[car][0], 0x78)};
            Cut c = SlotCut(cam, FixedMarkerParams(), slotIndex);
            Predicted pr = Predict(&cam, moved3);
            CHECK(pr.ok && fabs(pr.ax - c.px) < 0.05 && fabs(pr.ay - c.py) < 0.05 && fabs(pr.cam[2] - 14) < 1e-3, "car %d's root is at slot %d: pixel %.2f %.2f depth %.3f", car, slotIndex, pr.ax, pr.ay,
                  pr.cam[2]);
        }
        CHECK(fake_edx[0] == 0xCAFEBABE && fake_edx[2] == 0xCAFEBABE && fake_xmm3[0] == pi && fake_xmm3[2] == pi, "registers kept with the markers moved too");
        // ... and what the compositor takes out of the layer: the slots, one per marker, far to near
        {
            Recorder::Clear();
            static void* vtable2[128];
            static void* deviceObject2 = vtable2;
            vtable2[35] = (void*)Recorder::ColorFill;
            IDirect3DDevice9* device2 = (IDirect3DDevice9*)&deviceObject2;
            int n2 = DrawMarkers(device2, (IDirect3DTexture9*)0x10, (IDirect3DSurface9*)0x20, NULL, 0, 0, snap, cam);
            CHECK(n2 == 2 && Recorder::fills.size() == 2, "two markers cut out of the slots (%d, %zu fills)", n2, Recorder::fills.size());
            if (Recorder::fills.size() == 2)
                CHECK(Recorder::fills[0].right <= Recorder::fills[1].left || Recorder::fills[1].right <= Recorder::fills[0].left, "the two cut-outs do not overlap (%ld..%ld, %ld..%ld)",
                      Recorder::fills[0].left, Recorder::fills[0].right, Recorder::fills[1].left, Recorder::fills[1].right);
        }
        // a car the game will not draw (past its limit) stays where it is
        memcpy(g_gadget + 0x194, &one, 4);
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.site[0].slot == 3 && snap.site[1].slot == -1 && !snap.site[1].willDraw, "past the limit: not moved, and car 0 keeps its slot");
        CHECK(fabs(GetFloat(g_elements[2][0], 0x70) - 30) < 1e-4, "its root keeps the game's position (%g)", GetFloat(g_elements[2][0], 0x70));
        memcpy(g_gadget + 0x194, &limit, 4);
        g_siteBroken = true; // (what the module does when the game's stack is not as expected: it moves nothing)
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.siteCount == 0 && snap.marker[0].slot == -1, "the detour given up: nothing moved or recorded");
        AiHudPoll();
        CHECK(!g_planePatched && memcmp(g_planeJump, original, 6) == 0, "and the game's own ja is back");
        g_siteBroken = false;
        AiHudPoll();
        CHECK(g_planePatched, "patched again");
        g_cfg.vr = 0;
        AiHudPoll();
        CHECK(!g_planePatched && memcmp(g_planeJump, original, 6) == 0, "switched off: the game's own ja is back");
        g_cfg.vr = 1;
    }

    // ---- world markers: the game's own camera, read from the update's stack
    {
        g_cfg.vr = 1;
        AiHudPoll();
        MainCameraPose given = cam; // what SetCameraData gave: at the origin, looking along +z
        given.frame = g_vr.frameCount;
        PublishMainCameraPose(given);
        // the game's camera at the update: 1.5 m ahead, 0.2 m to the right, turned 2 degrees to the right
        const double where[3] = {0.2, 0, 1.5};
        double sy = 0, cy = 1;
        auto setCamera = [&](double degrees) {
            const double yaw = degrees * 3.14159265358979 / 180.0;
            cy = cos(yaw);
            sy = sin(yaw);
            const double right[3] = {cy, 0, -sy}, up[3] = {0, 1, 0}, fwd[3] = {sy, 0, cy};
            const double* axes[3] = {right, up, fwd};
            for (int a = 0; a < 3; a++)
                for (int k = 0; k < 3; k++)
                    fake_cam[a * 3 + k] = (float)axes[a][k];
            for (int k = 0; k < 3; k++)
                fake_cam[9 + k] = (float)where[k];
        };
        g_step = 0;
        setCamera(2.0);
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.camKnown && snap.gameCam, "the game's camera is read: %d %d", snap.camKnown, snap.gameCam);
        CHECK(fabs(snap.cam.pos[0] - 0.2) < 1e-5 && fabs(snap.cam.pos[2] - 1.5) < 1e-5 && fabs(snap.cam.fwd[0] - sy) < 1e-6 && fabs(snap.cam.right[2] + sy) < 1e-6,
              "its position and axes (%.3f %.3f, fwd.x %.5f)", snap.cam.pos[0], snap.cam.pos[2], snap.cam.fwd[0]);
        CHECK(snap.cam.projX == cam.projX && snap.cam.projY == cam.projY, "with SetCameraData's projection");
        for (int slotIndex = 3; slotIndex < 5; slotIndex++) { // the roots are at the slots' pixels and depth in the game's camera, not in the one SetCameraData gave
            int car = slotIndex == 3 ? 0 : 2;
            float moved3[3] = {GetFloat(g_elements[car][0], 0x70), GetFloat(g_elements[car][0], 0x74), GetFloat(g_elements[car][0], 0x78)};
            Cut c = SlotCut(snap.cam, FixedMarkerParams(), slotIndex);
            Predicted pr = Predict(&snap.cam, moved3), other = Predict(&given, moved3);
            CHECK(pr.ok && fabs(pr.ax - c.px) < 0.05 && fabs(pr.ay - c.py) < 0.05 && fabs(pr.cam[2] - 14) < 1e-3, "slot %d, in the game's camera: pixel %.2f %.2f depth %.3f", slotIndex, pr.ax,
                  pr.ay, pr.cam[2]);
            CHECK(fabs(other.ax - c.px) > 20, "and not in the other one (%.1f against %.1f)", other.ax, c.px);
        }
        // the compositor takes a car at the same place relative to the game's camera as it was at the update, in the camera that is drawn: car 0 is at world (10, 1.5, 40)
        {
            MarkerGeom geoms[kMaxSlots];
            int placed = BuildMarkers(snap, given, Identity(), geoms, kMaxSlots);
            bool found = false;
            for (int i = 0; i < placed; i++)
                if (geoms[i].car == 0) {
                    found = true;
                    CHECK(fabs(geoms[i].corner[0][2] - 38.5) < 1e-6 && fabs(geoms[i].corner[3][2] - 38.5) < 1e-6, "car 0's billboard is %.3f m ahead (expected 38.5 = 40 - 1.5)", geoms[i].corner[0][2]);
                }
            CHECK(found, "car 0 is placed");
        }
        // a camera turned 20 degrees from the given one is still the same camera, 30 degrees is not
        setCamera(20.0);
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.gameCam && fabs(snap.cam.fwd[0] - sy) < 1e-6, "20 degrees apart: read");
        setCamera(30.0);
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.camKnown && !snap.gameCam, "30 degrees apart: not the camera");
        // things that are not the camera: a zero matrix, a position 500 m away, a matrix that is not a rotation: the given camera is used
        for (int bad = 0; bad < 4; bad++) {
            setCamera(2.0);
            if (bad == 0)
                memset(fake_cam, 0, sizeof(fake_cam));
            else if (bad == 1)
                fake_cam[11] = 500.0f;
            else if (bad == 2)
                fake_cam[1] = 5.0f;
            else
                fake_cam[3] = fake_cam[4] = fake_cam[5] = 0.0f; // (an up axis of length 0)
            update(g_gadget, NULL);
            CHECK(CopyLatest(snap) && snap.camKnown && !snap.gameCam && fabs(snap.cam.pos[0]) < 1e-9 && fabs(snap.cam.pos[2]) < 1e-9, "not a camera (case %d): the given camera is used (%d)", bad,
                  snap.gameCam);
        }
        // with no usable camera on the stack a car is at its world position in the camera that is drawn
        memset(fake_cam, 0, sizeof(fake_cam));
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && snap.camKnown && !snap.gameCam, "the given camera is used");
        {
            MarkerGeom geoms[kMaxSlots];
            int placed = BuildMarkers(snap, given, Identity(), geoms, kMaxSlots);
            bool found = false;
            for (int i = 0; i < placed; i++)
                if (geoms[i].car == 0) {
                    found = true;
                    CHECK(fabs(geoms[i].corner[0][2] - 40.0) < 1e-6, "without the game's camera car 0 is at its world position (%.3f)", geoms[i].corner[0][2]);
                }
            CHECK(found, "car 0 is placed");
        }
        // a pose from long ago is no camera to place anything by: nothing is moved
        MainCameraPose old = given;
        old.frame = 0;
        g_vr.frameCount = 10;
        PublishMainCameraPose(old);
        setCamera(2.0);
        update(g_gadget, NULL);
        CHECK(CopyLatest(snap) && !snap.camKnown && snap.site[0].slot == -1, "an old camera: markers are not moved (known %d, slot %d)", snap.camKnown, snap.site[0].slot);
        g_vr.frameCount = 0;
        PublishMainCameraPose(given);
        const float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // (the camera for what follows: the given one)
        memcpy(fake_cam, identity, sizeof(identity));
        g_cfg.vr = 1;
    }

    // ---- world markers: they face the camera, not their cars: the game's billboard flag is held off during the update and put back
    {
        CHECK(g_billboardFlag == &fake_billboard, "the flag the update compares is found (%p against %p)", (void*)g_billboardFlag, (void*)&fake_billboard);
        g_cfg.vr = 1;
        fake_billboard = 1;
        g_step = 0;
        update(g_gadget, NULL);
        CHECK(fake_billboard_seen == 0 && fake_billboard == 1, "world markers on: the flag is off during the update (%d) and back after (%d)", fake_billboard_seen, fake_billboard);
        fake_billboard = 0;
        update(g_gadget, NULL);
        CHECK(fake_billboard_seen == 0 && fake_billboard == 0, "a flag that was off stays off (%d %d)", fake_billboard_seen, fake_billboard);
        g_cfg.vr = 0;
        fake_billboard = 1;
        update(g_gadget, NULL);
        CHECK(fake_billboard_seen == 1 && fake_billboard == 1, "world markers off: the game's own flag is left alone (%d)", fake_billboard_seen);
        g_cfg.vr = 1;
    }

    // ---- world markers: which cars get a marker: the nearest to the head inside a cone around where it looks, not the game's own order
    {
        g_cfg.vr = 1;
        AiHudPoll();
        MainCameraPose given;
        memset(&given, 0, sizeof(given));
        given.right[0] = 1;
        given.up[1] = 1;
        given.fwd[2] = 1;
        given.projX = 0.872;
        given.projY = 1.5697;
        given.frame = g_vr.frameCount;
        PublishMainCameraPose(given);
        const float identityCam[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        memcpy(fake_cam, identityCam, sizeof(identityCam));
        g_vr.poseValid = false;
        memcpy(g_gadget + 0x194, &one, 4); // the draw limit: one marker
        g_step = 2;                        // the cars are at x = 12, 22, 32 (z = 40): the game goes through them in that order
        update(g_gadget, NULL);            // (a snapshot with all three, with no pose of the head: the game's own rule draws car 0)
        CHECK(fake_drew[0] == 1 && fake_drew[1] == 0 && fake_drew[2] == 0, "no head pose: the game's own pick, the first car (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        g_vr.poseValid = true;
        // the head is 3 m from car 2 (at world x = 35, z = 40), looking along -x: the nearest car in front of it is car 2, though the game goes through car 0 first
        Matrix4 head = Identity();
        head.m[0][0] = 0, head.m[0][1] = 0, head.m[0][2] = -1;
        head.m[1][0] = 0, head.m[1][1] = 1, head.m[1][2] = 0;
        head.m[2][0] = 1, head.m[2][1] = 0, head.m[2][2] = 0;
        head.m[3][0] = -40, head.m[3][1] = 0, head.m[3][2] = 35;
        double hp[3], look[3];
        HeadPlacement(head, hp, look);
        CHECK(fabs(hp[0] - 35) < 1e-9 && fabs(hp[2] - 40) < 1e-9 && fabs(look[0] + 1) < 1e-9, "the head's place and view direction (%.2f %.2f %.2f, looks %.2f)", hp[0], hp[1], hp[2], look[0]);
        g_vr.eyeCameraDelta[2] = head;
        update(g_gadget, NULL);
        CHECK(fake_drew[0] == 0 && fake_drew[1] == 0 && fake_drew[2] == 1, "the pick: the car nearest the head, in front of it (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        CHECK(CopyLatest(snap) && snap.marker[2].drawn && !snap.marker[0].drawn && !snap.marker[1].drawn && snap.site[2].willDraw && !snap.site[0].willDraw,
              "and the detour's idea of drawn is the same (%d %d %d)", snap.marker[0].drawn, snap.marker[1].drawn, snap.marker[2].drawn);
        CHECK(snap.site[2].slot >= 0 && snap.site[0].slot == -1, "only the picked car is moved to a slot (%d %d)", snap.site[0].slot, snap.site[2].slot);
        // with room for two markers: the two nearest, cars 2 and 1
        int two = 2;
        memcpy(g_gadget + 0x194, &two, 4);
        update(g_gadget, NULL);
        CHECK(fake_drew[0] == 0 && fake_drew[1] == 1 && fake_drew[2] == 1, "a limit of 2: cars 1 and 2 (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        memcpy(g_gadget + 0x194, &one, 4);
        // the cars behind the head are not eligible: looking along +x from the same place there is no car in front, so none is drawn
        head.m[0][2] = 1, head.m[2][0] = -1, head.m[3][0] = 40, head.m[3][2] = -35;
        HeadPlacement(head, hp, look);
        CHECK(fabs(hp[0] - 35) < 1e-9 && fabs(hp[2] - 40) < 1e-9 && fabs(look[0] - 1) < 1e-9, "the head turned round (%.2f %.2f, looks %.2f)", hp[0], hp[2], look[0]);
        g_vr.eyeCameraDelta[2] = head;
        update(g_gadget, NULL);
        CHECK(fake_drew[0] == 0 && fake_drew[1] == 0 && fake_drew[2] == 0, "nothing in front of the head: no marker (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        // looking along -x again: car 2 is picked again
        head.m[0][2] = -1, head.m[2][0] = 1, head.m[3][0] = -40, head.m[3][2] = 35;
        g_vr.eyeCameraDelta[2] = head;
        update(g_gadget, NULL);
        CHECK(fake_drew[0] == 0 && fake_drew[1] == 0 && fake_drew[2] == 1, "car 2 is picked again (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        // a car that has a marker counts as nearer (15 %): the head looks along +z from (27, 0, 30), cars 1 and 2 are the same distance away (11.3 m), and car 2 had the marker
        head = Identity();
        head.m[3][0] = -27, head.m[3][2] = -30;
        g_vr.eyeCameraDelta[2] = head;
        update(g_gadget, NULL);
        CHECK(fake_drew[1] == 0 && fake_drew[2] == 1, "the car that has a marker is kept while another is as near (%u %u)", fake_drew[1], fake_drew[2]);
        head.m[3][0] = -22.6; // car 1 is 10.05 m away, car 2 13.8 m (11.7 with the bonus): given up
        g_vr.eyeCameraDelta[2] = head;
        update(g_gadget, NULL);
        CHECK(fake_drew[1] == 1 && fake_drew[2] == 0, "and given up when another is a lot nearer (%u %u)", fake_drew[1], fake_drew[2]);
        // no pose of the head: the game's own pick again
        g_vr.poseValid = false;
        update(g_gadget, NULL);
        CHECK(fake_drew[0] == 1 && fake_drew[1] == 0 && fake_drew[2] == 0, "no head pose: the game's own pick (%u %u %u)", fake_drew[0], fake_drew[1], fake_drew[2]);
        g_vr.eyeCameraDelta[2] = Identity();
        memcpy(g_gadget + 0x194, &limit, 4);
        PublishMainCameraPose(given);
        g_step = 0;
        g_vr.poseValid = false;
        SlotsBeginUpdate(); // (the slots the updates above gave are forgotten for the next test)
        SlotsBeginUpdate();
    }

    // ---- the slots: new cars from the middle out, a car keeps its slot while it is drawn, whatever the order of the cars
    {
        SlotsBeginUpdate();
        CHECK(SlotForCar(5) == 3 && SlotForCar(9) == 4 && SlotForCar(2) == 2 && SlotForCar(7) == 5, "four cars: the middle first (3 4 2 5)");
        SlotsBeginUpdate(); // the nearest cars change places: 9 is now nearer than 5, and 2 and 7 have swapped too
        CHECK(SlotForCar(9) == 4 && SlotForCar(5) == 3 && SlotForCar(7) == 5 && SlotForCar(2) == 2, "the same cars in another order: the same slots");
        SlotsBeginUpdate(); // car 5 is no longer drawn, a new car 11 arrives before the others: it does not take the slot car 5 had a moment ago while other slots are free
        CHECK(SlotForCar(11) == 1 && SlotForCar(9) == 4 && SlotForCar(7) == 5 && SlotForCar(2) == 2, "a new car takes a free slot, not the one a car had a moment ago");
        SlotsBeginUpdate();
        CHECK(SlotForCar(11) == 1 && SlotForCar(5) == 3, "and the slot is free for use again one update later");
        SlotsBeginUpdate(); // all 8 slots in use, a ninth car gets none
        for (int car = 20; car < 28; car++)
            SlotForCar(car);
        CHECK(SlotForCar(30) == -1, "no ninth slot");
        SlotsBeginUpdate();
    }

    // ---- the HUD's sway is held at rest while the world markers are on
    {
        static BYTE root[0x300];
        SetFloat(root, 0x190, 100.0f);
        SetFloat(root, 0x194, 50.0f);
        SetFloat(root, 0x19C, 0.0f);
        SetFloat(root, 0x198, 1.0f);
        typedef void(__attribute__((fastcall)) * SwayFn)(void*, void*);
        SwayFn sway = (SwayFn)fake_sway_start;
        g_cfg.vr = 1;
        sway(root, NULL);
        CHECK(GetFloat(root, 0x70) == 100.0f && GetFloat(root, 0x74) == 50.0f && GetFloat(root, 0x84) == 0.0f && GetFloat(root, 0x88) == 1.0f && GetFloat(root, 0x8C) == 1.0f,
              "world markers on: the outputs are the rest values (%g %g %g %g)", GetFloat(root, 0x70), GetFloat(root, 0x74), GetFloat(root, 0x84), GetFloat(root, 0x88));
        g_cfg.vr = 0;
        sway(root, NULL);
        CHECK(GetFloat(root, 0x70) == 10.0f && fabs(GetFloat(root, 0x88) - 1.05) < 1e-5, "Stereo=0: the game's own sway (%g %g)", GetFloat(root, 0x70), GetFloat(root, 0x88));
        // a layout that does not look like a rest layout is not written
        SetFloat(root, 0x198, 0.0f);
        g_cfg.vr = 1;
        sway(root, NULL);
        CHECK(GetFloat(root, 0x70) == 10.0f, "a rest scale of 0 is not trusted, nothing is written");
        g_cfg.vr = 1;
    }

    // ---- a game whose stack is not laid out as expected: the detour records and moves nothing, and the skip is put back (last: this is final)
    {
        AiHudPoll();
        CHECK(g_planePatched, "patched for the last test");
        fake_badidx = 1;
        for (int i = 0; i < 5; i++)
            update(g_gadget, NULL);
        CHECK(g_siteBroken, "the detour gives up when the car number is not where it should be");
        CHECK(CopyLatest(snap) && snap.siteCount == 0, "it recorded nothing");
        AiHudPoll();
        CHECK(!g_planePatched, "and the game's skip is back");
        fake_badidx = 0;
    }

    printf(g_failures ? "ai_hud_check: %d FAILURE(S)\n" : "ai_hud_check: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
