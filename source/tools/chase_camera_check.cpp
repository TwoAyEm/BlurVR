// Host check of src/chase_camera.cpp (NOTES.md section 29, [VR] CockpitView): the car's axes are matched to the chase camera's whatever the quaternion's convention and
// whichever local axis is the car's front, the delta from the game camera to the seat puts the seat at the eye's origin and the car's forward / right axes on the eye's
// z / x, the match does not change while the camera lags behind the car, and it gives up when the chase camera is not the one in use.  The file under test is included, so
// its private state can be set up directly.  Built and run by tools/chase_camera_check.py.
#include <stdio.h>
#include "../src/chase_camera.cpp"

Config g_cfg;
VrState g_vr;
char g_modDir[MAX_PATH] = "";
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  log: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}
static const unsigned char* g_aiStart = NULL;
const unsigned char* AiHudUpdateStart()
{
    return g_aiStart;
}
static int g_composed = 0;
void VrComposeSeat()
{
    g_composed++;
}
void* InstallInlineHook(void*, const BYTE*, size_t, void*)
{
    return NULL;
}
static MainCameraPose g_cam;
bool GetMainCameraPose(MainCameraPose& out)
{
    out = g_cam;
    return true;
}

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL line %d: %s -- ", __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                     \
            g_failures++;                                    \
        }                                                    \
    } while (0)

static void Cross(const double* a, const double* b, double* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void Normalize(double* v)
{
    double l = sqrt(Dot(v, v));
    for (int i = 0; i < 3; i++)
        v[i] /= l;
}

// A rotation matrix with the given columns (the car's local axes in the world) as the quaternion x, y, z, w the game would keep: the game applies the matrix of the
// conjugate of what it keeps, so the stored one is the conjugate of the quaternion of the matrix.
static void QuatFromColumns(const double c[3][3], float* q)
{
    const double m[3][3] = {{c[0][0], c[1][0], c[2][0]}, {c[0][1], c[1][1], c[2][1]}, {c[0][2], c[1][2], c[2][2]}};
    double x, y, z, w;
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        double s = sqrt(trace + 1) * 2;
        w = 0.25 * s;
        x = (m[2][1] - m[1][2]) / s;
        y = (m[0][2] - m[2][0]) / s;
        z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        w = (m[2][1] - m[1][2]) / s;
        x = 0.25 * s;
        y = (m[0][1] + m[1][0]) / s;
        z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        w = (m[0][2] - m[2][0]) / s;
        x = (m[0][1] + m[1][0]) / s;
        y = 0.25 * s;
        z = (m[1][2] + m[2][1]) / s;
    } else {
        double s = sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        w = (m[1][0] - m[0][1]) / s;
        x = (m[0][2] + m[2][0]) / s;
        y = (m[1][2] + m[2][1]) / s;
        z = 0.25 * s;
    }
    q[0] = (float)-x;
    q[1] = (float)-y;
    q[2] = (float)-z;
    q[3] = (float)w;
}

struct Scene {
    double carPos[3], right[3], up[3], fwd[3]; // the car's real axes in the world
};

// The car heading `yaw` (radians from +z towards +x), pitched `pitch` nose up, at `pos`.
static Scene MakeScene(double yaw, double pitch, const double* pos)
{
    Scene s;
    const double fx = sin(yaw) * cos(pitch), fy = sin(pitch), fz = cos(yaw) * cos(pitch);
    s.fwd[0] = fx;
    s.fwd[1] = fy;
    s.fwd[2] = fz;
    const double worldUp[3] = {0, 1, 0};
    Cross(worldUp, s.fwd, s.right); // (left-handed: right = up x forward)
    Normalize(s.right);
    Cross(s.fwd, s.right, s.up);
    Normalize(s.up);
    memcpy(s.carPos, pos, sizeof(s.carPos));
    return s;
}

// The chase camera: `back` metres behind the car along its heading (`lag` radians of yaw behind), `high` above, looking at a point above the car.
static void PlaceCamera(const Scene& car, double back, double high, double lag)
{
    const double yaw = atan2(car.fwd[0], car.fwd[2]) - lag;
    const double dir[3] = {sin(yaw), 0, cos(yaw)};
    for (int i = 0; i < 3; i++)
        g_cam.pos[i] = car.carPos[i] - dir[i] * back;
    g_cam.pos[1] += high;
    double target[3] = {car.carPos[0], car.carPos[1] + 1.2, car.carPos[2]};
    double f[3] = {target[0] - g_cam.pos[0], target[1] - g_cam.pos[1], target[2] - g_cam.pos[2]};
    Normalize(f);
    const double worldUp[3] = {0, 1, 0};
    double r[3], u[3];
    Cross(worldUp, f, r);
    Normalize(r);
    Cross(f, r, u);
    for (int i = 0; i < 3; i++) {
        g_cam.fwd[i] = f[i];
        g_cam.right[i] = r[i];
        g_cam.up[i] = u[i];
    }
}

// Set the chase camera's pose as the hook would have: the car's local axes `columns` (in any order the game might use) stored as a quaternion.
static void SetRaw(const Scene& car, const double columns[3][3])
{
    g_raw.frame = 100;
    g_raw.hasBody = false;
    for (int i = 0; i < 3; i++)
        g_raw.pos[i] = (float)car.carPos[i];
    QuatFromColumns(columns, g_raw.quat);
    g_rawValid = true;
    g_hooked = true;
    g_cam.frame = 100;
    g_vr.frameCount = 100;
}

static double Length3(const double* v)
{
    return sqrt(Dot(v, v));
}

// A world point in the eye's space after the delta (the camera's space first).
static void ToEye(const Matrix4& delta, const double* world, double* out)
{
    const double d[3] = {world[0] - g_cam.pos[0], world[1] - g_cam.pos[1], world[2] - g_cam.pos[2]};
    const double p[3] = {Dot(d, g_cam.right), Dot(d, g_cam.up), Dot(d, g_cam.fwd)};
    for (int k = 0; k < 3; k++)
        out[k] = p[0] * delta.m[0][k] + p[1] * delta.m[1][k] + p[2] * delta.m[2][k] + delta.m[3][k];
}

static BYTE g_fakeRecord[4][0x400], g_fakeVehicle[4][0x300], g_fakeBody[4][0x400];
static void* __attribute__((cdecl)) FakePlayer(int car)
{
    return car >= 0 && car < 4 ? g_fakeRecord[car] : NULL;
}
static void* __attribute__((thiscall)) FakeVehicle(void*, int car)
{
    return car >= 0 && car < 4 ? g_fakeVehicle[car] : NULL;
}
static void PutCar(int car, double x, double y, double z)
{
    BYTE* body = g_fakeBody[car];
    *(BYTE**)(g_fakeVehicle[car] + 0x240) = body;
    const float p[3] = {(float)x, (float)y, (float)z};
    memcpy(body + 0x2F4, p, sizeof(p));
}

int main(int argc, char** argv)
{
    BuildCandidates();
    CHECK(g_candidateCount == 24, "%d candidates", g_candidateCount);
    const double seat[3] = {-0.35, 0.7, 0.3};
    struct Case {
        const char* name;
        int order[3]; // which of (right, up, forward) is the game's local x, y, z
        int sign[3];
    } cases[] = {
        {"x right, y up, z forward", {0, 1, 2}, {1, 1, 1}},
        {"x forward, y up, z left", {2, 1, 0}, {1, 1, -1}},
        {"x right, y forward, z down", {0, 2, 1}, {1, 1, -1}},
        {"x left, y up, z back", {0, 1, 2}, {-1, 1, -1}},
        {"x up, y right, z back", {1, 0, 2}, {1, 1, -1}},
    };
    const double yaws[] = {0.0, 0.5, 1.57, 2.9, -1.3, -2.6};
    for (const Case& c : cases) {
        for (double yaw : yaws) {
            for (double pitch : {0.0, 0.12, -0.08}) {
                const double pos[3] = {120.5, 3.0, -48.25};
                Scene car = MakeScene(yaw, pitch, pos);
                const double* real[3] = {car.right, car.up, car.fwd};
                double columns[3][3];
                for (int k = 0; k < 3; k++)
                    for (int i = 0; i < 3; i++)
                        columns[k][i] = c.sign[k] * real[c.order[k]][i];
                double det = Dot(columns[0], (double[3]){columns[1][1] * columns[2][2] - columns[1][2] * columns[2][1], columns[1][2] * columns[2][0] - columns[1][0] * columns[2][2],
                                                           columns[1][0] * columns[2][1] - columns[1][1] * columns[2][0]});
                if (det < 0)
                    continue; // (not a rotation: the game cannot keep it as a quaternion)
                g_choice = -1;
                PlaceCamera(car, 6.0, 2.0, 0.0);
                SetRaw(car, columns);
                Matrix4 delta;
                bool ok = CarSeatDelta(seat, delta);
                CHECK(ok, "%s, yaw %.2f pitch %.2f: no delta", c.name, yaw, pitch);
                if (!ok)
                    continue;
                double seatWorld[3], eye[3];
                for (int i = 0; i < 3; i++)
                    seatWorld[i] = car.carPos[i] + car.right[i] * seat[0] + car.up[i] * seat[1] + car.fwd[i] * seat[2];
                ToEye(delta, seatWorld, eye);
                CHECK(Length3(eye) < 1e-3, "%s, yaw %.2f pitch %.2f: the seat is at %.4f %.4f %.4f in the eye's space", c.name, yaw, pitch, eye[0], eye[1], eye[2]);
                double ahead[3], toRight[3], upward[3];
                for (int i = 0; i < 3; i++) {
                    ahead[i] = seatWorld[i] + car.fwd[i] * 10;
                    toRight[i] = seatWorld[i] + car.right[i] * 5;
                    upward[i] = seatWorld[i] + car.up[i] * 2;
                }
                ToEye(delta, ahead, eye);
                CHECK(fabs(eye[0]) < 1e-3 && fabs(eye[1]) < 1e-3 && fabs(eye[2] - 10) < 1e-3, "%s, yaw %.2f pitch %.2f: 10 m ahead of the seat is %.3f %.3f %.3f", c.name, yaw, pitch, eye[0], eye[1], eye[2]);
                ToEye(delta, toRight, eye);
                CHECK(fabs(eye[0] - 5) < 1e-3 && fabs(eye[1]) < 1e-3 && fabs(eye[2]) < 1e-3, "%s, yaw %.2f pitch %.2f: 5 m to the right of the seat is %.3f %.3f %.3f", c.name, yaw, pitch, eye[0], eye[1], eye[2]);
                ToEye(delta, upward, eye);
                CHECK(fabs(eye[1] - 2) < 1e-3, "%s, yaw %.2f pitch %.2f: 2 m above the seat is %.3f %.3f %.3f", c.name, yaw, pitch, eye[0], eye[1], eye[2]);
            }
        }
    }
    printf("matched every convention and heading\n");

    // the camera lags behind the car in a drift: the choice must not change, and the seat still sits in the car
    {
        const double pos[3] = {0, 0, 0};
        Scene car = MakeScene(1.0, 0.0, pos);
        const double columns[3][3] = {{car.right[0], car.right[1], car.right[2]}, {car.up[0], car.up[1], car.up[2]}, {car.fwd[0], car.fwd[1], car.fwd[2]}};
        g_choice = -1;
        PlaceCamera(car, 6.0, 2.0, 0.0);
        SetRaw(car, columns);
        Matrix4 delta;
        CHECK(CarSeatDelta(seat, delta), "straight");
        const int first = g_choice;
        for (double lag : {0.1, 0.3, 0.5, -0.4, 0.7}) {
            for (double back : {4.5, 6.0, 8.5}) {
                PlaceCamera(car, back, 2.5, lag);
                SetRaw(car, columns);
                CHECK(CarSeatDelta(seat, delta), "lag %.2f back %.1f: no delta", lag, back);
                CHECK(g_choice == first, "lag %.2f back %.1f: the choice changed from %d to %d", lag, back, first, g_choice);
                double seatWorld[3], eye[3];
                for (int i = 0; i < 3; i++)
                    seatWorld[i] = car.carPos[i] + car.right[i] * seat[0] + car.up[i] * seat[1] + car.fwd[i] * seat[2];
                ToEye(delta, seatWorld, eye);
                CHECK(Length3(eye) < 1e-3, "lag %.2f back %.1f: the seat is at %.4f %.4f %.4f", lag, back, eye[0], eye[1], eye[2]);
            }
        }
        printf("camera lag and distance do not move the seat\n");

        // not the chase camera in use
        PlaceCamera(car, 6.0, 2.0, 0.0);
        SetRaw(car, columns);
        g_vr.frameCount = 110; // (the update has not run for ten frames)
        g_cam.frame = 110;
        CHECK(!CarSeatDelta(seat, delta), "a stale update gave a delta");
        g_vr.frameCount = 100;
        g_cam.frame = 100;
        PlaceCamera(car, 0.2, 0.0, 0.0); // (a camera at the car: a bumper camera)
        CHECK(!CarSeatDelta(seat, delta), "a camera at the car gave a delta");
        g_cam.pos[0] = car.carPos[0] + 6;
        g_cam.pos[2] = car.carPos[2] - 6; // beside and behind, looking the wrong way
        for (int i = 0; i < 3; i++) {
            g_cam.right[i] = -car.right[i];
            g_cam.fwd[i] = -car.fwd[i];
        }
        CHECK(!CarSeatDelta(seat, delta), "a camera looking back at the car gave a delta");
        g_hooked = false;
        PlaceCamera(car, 6.0, 2.0, 0.0);
        CHECK(!CarSeatDelta(seat, delta), "no hook gave a delta");
        printf("gives up when the chase camera is not in use\n");
    }

    // the car's body found by the game's own way: the seat is placed on it, not on the chase camera's target, which is 3 m ahead of it
    {
        const double pos[3] = {10, 1, 20};
        Scene car = MakeScene(0.8, 0.0, pos);
        const double columns[3][3] = {{car.right[0], car.right[1], car.right[2]}, {car.up[0], car.up[1], car.up[2]}, {car.fwd[0], car.fwd[1], car.fwd[2]}};
        g_choice = -1;
        Scene behind = car;
        for (int i = 0; i < 3; i++)
            behind.carPos[i] = car.carPos[i] - car.fwd[i] * 3.0; // the body
        PlaceCamera(behind, 6.0, 2.0, 0.0);
        SetRaw(car, columns); // (the target, 3 m ahead of the body)
        g_raw.hasBody = true;
        for (int i = 0; i < 3; i++)
            g_raw.body[i] = (float)behind.carPos[i];
        Matrix4 delta;
        CHECK(CarSeatDelta(seat, delta), "no delta with a body");
        double seatWorld[3], eye[3];
        for (int i = 0; i < 3; i++)
            seatWorld[i] = behind.carPos[i] + car.right[i] * seat[0] + car.up[i] * seat[1] + car.fwd[i] * seat[2];
        ToEye(delta, seatWorld, eye);
        CHECK(Length3(eye) < 1e-3, "the seat is not on the body: %.4f %.4f %.4f", eye[0], eye[1], eye[2]);
        g_raw.hasBody = false;
        CHECK(CarSeatDelta(seat, delta), "no delta without a body");
        for (int i = 0; i < 3; i++)
            seatWorld[i] = car.carPos[i] + car.right[i] * seat[0] + car.up[i] * seat[1] + car.fwd[i] * seat[2];
        ToEye(delta, seatWorld, eye);
        CHECK(Length3(eye) < 1e-3, "the seat is not on the target without a body: %.4f %.4f %.4f", eye[0], eye[1], eye[2]);
        puts("the seat is placed on the car's body when the game's way to it finds one");
    }

    // the lock from the direction of travel: the car drives fast while the camera swings far off behind it, where the camera alone would lose the car
    {
        g_locked = false;
        g_choice = -1;
        g_voteSamples = 0;
        memset(g_votes, 0, sizeof(g_votes));
        Matrix4 delta;
        int gotDelta = 0;
        // the game's local axes: x forward, y up, z left
        const int order[3] = {2, 1, 0};
        const int sign[3] = {1, 1, -1};
        for (int k = 0; k < 80; k++) {
            const double pos[3] = {20.0 * k, 0, 5.0 * k};
            Scene car = MakeScene(0.3 + 0.02 * k, 0.0, pos);
            const double* real[3] = {car.right, car.up, car.fwd};
            double columns[3][3];
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    columns[j][i] = sign[j] * real[order[j]][i];
            for (int i = 0; i < 3; i++)
                g_velocity[i] = car.fwd[i] * 25.0;
            PlaceCamera(car, 12.0, 3.0, 1.4); // (far off to the side)
            SetRaw(car, columns);
            if (CarSeatDelta(seat, delta))
                gotDelta++;
        }
        CHECK(g_locked, "the choice was not locked");
        CHECK(gotDelta > 0, "no delta once locked");
        const double pos[3] = {-5, 1, 7};
        Scene car = MakeScene(2.2, 0.1, pos);
        const double* real[3] = {car.right, car.up, car.fwd};
        double columns[3][3];
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                columns[j][i] = sign[j] * real[order[j]][i];
        PlaceCamera(car, 15.0, 4.0, 1.2); // the camera 70 degrees off the car's heading and 15 m back
        memset(g_velocity, 0, sizeof(g_velocity)); // (no lag compensation in the position check)
        SetRaw(car, columns);
        CHECK(CarSeatDelta(seat, delta), "no delta with the camera far off");
        double seatWorld[3], eye[3], ahead[3];
        for (int i = 0; i < 3; i++) {
            seatWorld[i] = car.carPos[i] + car.right[i] * seat[0] + car.up[i] * seat[1] + car.fwd[i] * seat[2];
            ahead[i] = seatWorld[i] + car.fwd[i] * 10;
        }
        ToEye(delta, seatWorld, eye);
        CHECK(Length3(eye) < 1e-3, "locked: the seat is at %.4f %.4f %.4f", eye[0], eye[1], eye[2]);
        ToEye(delta, ahead, eye);
        CHECK(fabs(eye[0]) < 1e-3 && fabs(eye[1]) < 1e-3 && fabs(eye[2] - 10) < 1e-3, "locked: 10 m ahead is %.3f %.3f %.3f", eye[0], eye[1], eye[2]);
        puts("the car's frame is locked from its direction of travel and holds with the camera far off");
        g_locked = false;
        g_choice = -1;
        memset(g_velocity, 0, sizeof(g_velocity));
    }

    // the refresh when the game sets the camera: the car is read again, from the object, and the delta is rebuilt
    {
        float object[16] = {};
        object[2] = 7.5f; // +8: the position
        object[3] = -2.0f;
        object[4] = 31.0f;
        object[5] = 0.25f; // +0x14: the quaternion x, y, z, w
        object[6] = 0.0f;
        object[7] = 0.0f;
        object[8] = 0.9682458f;
        g_self = object;
        g_updateFrame = 100;
        g_hooked = true;
        g_cfg.cockpit = 1;
        g_vr.frameCount = 101;
        const int before = g_composed;
        ChaseCameraRefresh();
        RawPose now;
        CHECK(GetRaw(now) && now.pos[0] == 7.5f && now.pos[2] == 31.0f && now.quat[3] == 0.9682458f && now.frame == 101, "the car was not read again");
        CHECK(g_composed == before + 1, "the delta was not rebuilt");
        g_vr.frameCount = 110; // (the update has not run for a while: another camera)
        ChaseCameraRefresh();
        CHECK(g_composed == before + 1, "a stale update was used");
        puts("the refresh reads the car again and rebuilds the delta");
    }
    // the whole way to a car's position, on the code of the game when a dump of it is given
    if (argc > 1) {
        FILE* f = fopen(argv[1], "rb");
        CHECK(f != NULL, "cannot open %s", argv[1]);
        if (f) {
            fseek(f, 0, SEEK_END);
            std::vector<BYTE> file(ftell(f));
            fseek(f, 0, SEEK_SET);
            CHECK(fread(file.data(), 1, file.size(), f) == file.size(), "short read");
            fclose(f);
            const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(file.data() + ((const IMAGE_DOS_HEADER*)file.data())->e_lfanew);
            std::vector<BYTE> image(nt->OptionalHeader.SizeOfImage + 16, 0);
            const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
                memcpy(image.data() + sections[i].VirtualAddress, file.data() + sections[i].PointerToRawData, sections[i].SizeOfRawData);
            g_aiStart = image.data() + (0xC54180 - nt->OptionalHeader.ImageBase);
            g_bodyResolved = false;
            CHECK(ResolveBody(), "ResolveBody gave up");
            CHECK(g_bodyResolved, "the way to a car's position was not accepted on the game's own code");
            CHECK(g_bodyOffset == 0x240 && g_positionOffset == 0x2F4 && g_hiddenOffset == 0x3EB, "offsets %x %x %x", g_bodyOffset, g_positionOffset, g_hiddenOffset);
            puts("the way to a car's position is accepted on the game's own code");
            g_aiStart = NULL;
            g_bodyResolved = false;
        }
    }

    // the body's own orientation against a pose that leads the car's heading by 0.2 rad (the chase camera while steering): the body's frame is found and used
    {
        g_locked = true;
        for (int i = 0; i < g_candidateCount; i++)
            if (g_candidates[i].axis[0] == 0 && g_candidates[i].axis[1] == 1 && g_candidates[i].axis[2] == 2 && g_candidates[i].sign[0] == 1 && g_candidates[i].sign[1] == 1 &&
                g_candidates[i].sign[2] == 1)
                g_choice = i;
        g_bodyLocked = false;
        g_bodySamples = 0;
        memset(g_bodyVotes, 0, sizeof(g_bodyVotes));
        memset(g_velocity, 0, sizeof(g_velocity));
        Matrix4 delta;
        Scene truth = MakeScene(0, 0, (double[3]){0, 0, 0});
        const int order[3] = {2, 1, 0}; // the body's local axes: x forward, y up, z left
        const int sign[3] = {1, 1, -1};
        for (int k = 0; k < 100; k++) {
            const double pos[3] = {10.0 + k, 0.5, -3.0};
            truth = MakeScene(0.2 + 0.05 * k, 0.0, pos);
            Scene leading = MakeScene(0.4 + 0.05 * k, 0.0, pos); // (the pose's heading, 0.2 rad ahead of the car's)
            const double columnsPose[3][3] = {{leading.right[0], leading.right[1], leading.right[2]}, {leading.up[0], leading.up[1], leading.up[2]}, {leading.fwd[0], leading.fwd[1], leading.fwd[2]}};
            PlaceCamera(truth, 6.0, 2.0, 0.0);
            SetRaw(leading, columnsPose);
            g_raw.hasBody = true;
            for (int i = 0; i < 3; i++)
                g_raw.body[i] = (float)truth.carPos[i];
            const double* real[3] = {truth.right, truth.up, truth.fwd};
            double columnsBody[3][3];
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    columnsBody[j][i] = sign[j] * real[order[j]][i];
            float q[4];
            QuatFromColumns(columnsBody, q); // (the conjugate of the plain one: negate again for the plain quaternion)
            g_raw.bodyQuat[0] = -q[0];
            g_raw.bodyQuat[1] = -q[1];
            g_raw.bodyQuat[2] = -q[2];
            g_raw.bodyQuat[3] = q[3];
            g_raw.hasBodyQuat = true;
            CarSeatDelta(seat, delta);
        }
        CHECK(g_bodyLocked, "the body's orientation was not identified");
        double seatWorld[3], ahead[3], eye[3];
        for (int i = 0; i < 3; i++) {
            seatWorld[i] = g_raw.body[i] + truth.right[i] * seat[0] + truth.up[i] * seat[1] + truth.fwd[i] * seat[2];
            ahead[i] = seatWorld[i] + truth.fwd[i] * 10;
        }
        ToEye(delta, ahead, eye);
        CHECK(fabs(eye[0]) < 0.05 && fabs(eye[2] - 10) < 0.05, "the view follows the pose's heading, not the car's: 10 m ahead of the car is %.3f %.3f %.3f", eye[0], eye[1], eye[2]);
        puts("the body's own orientation is found and the view follows the car's heading, not the chase camera's");
        g_locked = false;
        g_bodyLocked = false;
        g_choice = -1;
    }

    // the player's car among others: found where it stands, kept when another car is nearer the target than it is, looked for again after a while
    {
        g_getPlayer = FakePlayer;
        g_getVehicle = FakeVehicle;
        g_bodyOffset = 0x240;
        g_positionOffset = 0x2F4;
        g_hiddenOffset = 0x3EB;
        g_bodyResolved = true;
        g_playerCar = -1;
        memset(g_velocity, 0, sizeof(g_velocity));
        PutCar(0, 0, 0, 0);
        PutCar(1, 6, 0, 3);
        PutCar(2, -30, 0, 5);
        PutCar(3, 40, 0, -2);
        float out[3];
        const float standing[3] = {0.1f, -0.4f, 0.3f};
        CHECK(FindBody(standing, out) && g_playerCar == 0, "the standing player's car was not found (car %d)", g_playerCar);
        // driving: the target is 9 m ahead of the car and another car is nearer to it than the player's
        PutCar(0, 50, 0, 0);
        PutCar(1, 58, 0, 2);
        const float ahead[3] = {59.0f, -0.4f, 0.3f};
        g_velocity[0] = 20.0;
        CHECK(FindBody(ahead, out) && g_playerCar == 0 && fabsf(out[0] - 50.0f) < 1e-3f, "the player's car was lost to the car beside the target (car %d, x %.1f)", g_playerCar, out[0]);
        // lost for good: looked for again
        PutCar(0, 500, 0, 0);
        PutCar(1, 58, 0, 6);
        PutCar(3, 40, 0, 20);
        int lost = 0;
        for (int i = 0; i < 130; i++)
            lost += FindBody(ahead, out) ? 0 : 1;
        CHECK(lost == 130 && g_playerCar == -1, "the lost car was not given up after a while (car %d)", g_playerCar);
        // moving, nothing known: the car behind the target on the line of travel, not the one ahead of it
        PutCar(0, 50, 0, 0);
        PutCar(1, 62, 0, 1);
        PutCar(2, 55, 0, 1.5);
        CHECK(FindBody(ahead, out) && g_playerCar == 0, "the moving player's car was not found (car %d)", g_playerCar);
        // the same car number with another model in it (the next race, another car from the showroom): the key follows after a while, not at once
        {
            const float inertia = 900.0f, massA = 1300.0f, massB = 1750.0f;
            memcpy(g_fakeBody[0] + 0x188, &inertia, 4);
            memcpy(g_fakeBody[0] + 0x2A0, &massA, 4);
            g_carKey = 0;
            for (int i = 0; i < 100; i++)
                FindBody(ahead, out);
            const unsigned keyA = g_carKey;
            CHECK(keyA != 0, "the first car's key was not made");
            memcpy(g_fakeBody[0] + 0x2A0, &massB, 4);
            for (int i = 0; i < 30; i++)
                FindBody(ahead, out);
            CHECK(g_carKey == keyA, "the key changed after a few frames of another mass");
            for (int i = 0; i < 100; i++)
                FindBody(ahead, out);
            CHECK(g_carKey != 0 && g_carKey != keyA, "the key did not follow the new model (%08x, was %08x)", g_carKey, keyA);
            puts("the car's key follows another model under the same car number");
        }
        g_playerCar = -1;
        g_bodyResolved = false;
        memset(g_velocity, 0, sizeof(g_velocity));
        puts("the player's car is found, kept next to other cars and given up when lost");
    }

    // the seat kept per car: written under the car's key, read back when the car is recognised again, the default for a car without one
    {
        char folder[MAX_PATH];
        GetTempPathA(MAX_PATH, folder);
        strcpy(g_modDir, folder);
        DeleteFileA((std::string(g_modDir) + "BlurVR_seats.ini").c_str());
        g_defaultSeatKnown = false;
        g_cfg.carSeatX = -0.35;
        g_cfg.carSeatY = 0.7;
        g_cfg.carSeatZ = 0.3;
        g_carKey = 0;
        CHECK(!ChaseCameraSaveSeat(), "a seat was kept for an unknown car");
        g_carKey = 0xABCD1234;
        ApplySeatForKey(); // (a car without a saved seat: the default, which is remembered)
        CHECK(g_cfg.carSeatX == -0.35 && g_cfg.carSeatZ == 0.3, "the default seat was not used for a new car");
        g_cfg.carSeatX = 0.11;
        g_cfg.carSeatY = 0.95;
        g_cfg.carSeatZ = -0.4;
        CHECK(ChaseCameraSaveSeat(), "the seat was not kept");
        g_carKey = 0x00000042; // another car
        ApplySeatForKey();
        CHECK(fabs(g_cfg.carSeatX + 0.35) < 1e-9 && fabs(g_cfg.carSeatY - 0.7) < 1e-9, "another car did not get the default seat (%.2f %.2f)", g_cfg.carSeatX, g_cfg.carSeatY);
        g_carKey = 0xABCD1234; // the first car again
        ApplySeatForKey();
        CHECK(fabs(g_cfg.carSeatX - 0.11) < 1e-9 && fabs(g_cfg.carSeatY - 0.95) < 1e-9 && fabs(g_cfg.carSeatZ + 0.4) < 1e-9, "the kept seat was not found again (%.2f %.2f %.2f)", g_cfg.carSeatX,
              g_cfg.carSeatY, g_cfg.carSeatZ);
        DeleteFileA((std::string(g_modDir) + "BlurVR_seats.ini").c_str());
        g_carKey = 0;
        g_modDir[0] = 0;
        g_cfg.carSeatX = -0.35;
        g_cfg.carSeatY = 0.7;
        g_cfg.carSeatZ = 0.3;
        puts("the seat is kept per car and the default is used for a car without one");
    }

    // the byte patterns of the game's way to a car's position
    {
        const BYTE good[] = {0x8A, 0x81, 0xEB, 0x03, 0x00, 0x00, 0xC3, 0xAA, 0xBB};
        CHECK(BytesAre(good, "8A 81 EB 03 00 00 C3"), "a matching pattern did not match");
        CHECK(BytesAre(good, "8A 81 ?? 03 ?? 00 C3"), "wildcards did not match");
        CHECK(!BytesAre(good, "8A 81 EB 03 00 00 C4"), "the last byte was not compared");
        CHECK(!BytesAre(good, "8B 81 EB 03 00 00 C3"), "the first byte was not compared");
        CHECK(BytesAre(good, "8A 81 EB 03 00 00 C3 AA BB"), "the whole buffer did not match");
        CHECK(!BytesAre(good, "8A 81 EB 03 00 00 C3 AA BC"), "a byte after the end of a short pattern was ignored");
        puts("the byte patterns are compared exactly");
    }
    // the pattern and its sizes
    CHECK(kUpdateStolen == 12, "stolen bytes");
    printf(g_failures ? "FAILED: %d checks\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
