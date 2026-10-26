// Frame timing probe.  See perf_probe.h.
//
// GPU: a timestamp query is issued every time the rendering phase changes (the classifier below runs in RunDraw), so the
// time between two stamps belongs to the phase that was active in between.  Results are read a few frames later without
// ever waiting on the GPU.  CPU: QueryPerformanceCounter around the draw hooks (all of RunDraw, and the part that is the
// driver call itself) and around the stages of the present path.
#include "perf_probe.h"
#include "config.h"
#include "core.h"
#include "render_state.h"
#include "shader_tracking.h"
#include <algorithm>
#include <x86intrin.h>

bool g_perfActive = false;

static const int kFrames = 4;      // GPU frames in flight
static const int kMaxStamps = 192; // phase changes recorded per frame (the last stamp is kept for the frame end)

struct StampTag { // what a single draw was, for the phases that are timed draw by draw (width 0 = not a timed draw)
    int ps, vs; // shader ids, -1 = none bound
    unsigned short width, height;
};
struct GpuFrame {
    IDirect3DQuery9* disjoint;
    IDirect3DQuery9* stamp[kMaxStamps];
    unsigned char phase[kMaxStamps]; // phase of the interval that ends at stamp i (stamp 0 starts the frame)
    StampTag tag[kMaxStamps];
    int stampCount;
    unsigned draws[PH_COUNT];
};
static GpuFrame g_frames[kFrames];
static int g_head = 0, g_pending = 0; // oldest pending frame, number of pending frames
static int g_open = -1;               // slot of the frame being recorded, or -1
static PerfPhase g_phase = PH_OFFSCREEN;
static StampTag g_tag = {-1, -1, 0, 0};
static bool g_phaseLocked = false;
static bool g_timedDraw = false; // the draw being issued is timed on its own: a stamp follows it
static bool g_gpuUnavailable = false;
static IDirect3DQuery9* g_freqQuery = NULL;
static unsigned long long g_gpuFrequency = 0;
static unsigned g_truncatedFrames = 0, g_overrunFrames = 0, g_disjointFrames = 0;

struct CpuAccumulator {
    long long section[PS_COUNT];
    long long hook, driver;
    unsigned hookCalls, driverCalls;
};
static CpuAccumulator g_cur; // since the last frame close
struct DetailEntry {
    int ps, vs;
    unsigned width, height;
    double ms;
    unsigned draws;
};
static std::unordered_map<unsigned long long, DetailEntry> g_detail; // main-other draws by pixel shader and target size
struct Aggregate {
    double gpuMs[PH_COUNT];
    double gpuTotalMs;
    double draws[PH_COUNT];
    unsigned gpuFrames;
    CpuAccumulator cpu;
    double frameTicks;
    unsigned cpuFrames;
};
static Aggregate g_agg;
static long long g_lastClose = 0;
static struct ClockStart { // the two clocks read together once, when the DLL loads
    long long counter, ticks;
    ClockStart()
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        counter = c.QuadPart;
        ticks = (long long)__rdtsc();
    }
} g_clockStart;
static unsigned long long g_lastLog = 0;

// The CPU timers use the time stamp counter: reading it costs a few nanoseconds, the performance counter about five times that, and
// the probe reads a clock four times per draw.  kTickSeconds is measured against the performance counter once.
long long PerfNow()
{
    return (long long)__rdtsc();
}

static const char* const kPhaseNames[PH_COUNT] = {"offscreen", "scene-main", "scene-half", "lights",   "motion-blur",
                                                  "main-other", "post-low",   "hud",        "hud-compose", "copy"};

void PerfToggle()
{
    g_perfActive = !g_perfActive;
    memset(&g_cur, 0, sizeof(g_cur));
    memset(&g_agg, 0, sizeof(g_agg));
    g_lastClose = 0;
    g_lastLog = GetTickCount64();
    Log("perf probe %s", g_perfActive ? "ON (a summary line every second while racing)" : "OFF");
}

void ReleasePerf()
{
    for (GpuFrame& frame : g_frames) {
        if (frame.disjoint)
            frame.disjoint->Release();
        for (int i = 0; i < kMaxStamps; i++)
            if (frame.stamp[i])
                frame.stamp[i]->Release();
    }
    memset(g_frames, 0, sizeof(g_frames));
    if (g_freqQuery)
        g_freqQuery->Release();
    g_freqQuery = NULL;
    g_gpuFrequency = 0;
    g_head = g_pending = 0;
    g_open = -1;
}

static void AddStamp(GpuFrame& frame, PerfPhase endedPhase)
{
    if (frame.stampCount >= kMaxStamps)
        return;
    IDirect3DQuery9*& query = frame.stamp[frame.stampCount];
    if (!query)
        return;
    query->Issue(D3DISSUE_END);
    frame.tag[frame.stampCount] = g_tag;
    frame.phase[frame.stampCount++] = (unsigned char)endedPhase;
}

static bool OpenFrame(IDirect3DDevice9* device, PerfPhase firstPhase)
{
    if (g_gpuUnavailable)
        return false;
    if (g_pending == kFrames) {
        g_overrunFrames++;
        return false;
    }
    int slot = (g_head + g_pending) % kFrames;
    GpuFrame& frame = g_frames[slot];
    if (!frame.disjoint) {
        bool ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.disjoint));
        for (int i = 0; ok && i < kMaxStamps; i++)
            ok = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &frame.stamp[i]));
        if (!ok) {
            g_gpuUnavailable = true;
            Log("perf: this driver has no timestamp queries, GPU times are not available");
            return false;
        }
    }
    if (!g_freqQuery) {
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_freqQuery))) {
            g_gpuUnavailable = true;
            return false;
        }
        g_freqQuery->Issue(D3DISSUE_END);
    }
    frame.stampCount = 0;
    memset(frame.draws, 0, sizeof(frame.draws));
    frame.disjoint->Issue(D3DISSUE_BEGIN);
    AddStamp(frame, firstPhase);
    g_open = slot;
    g_phase = firstPhase;
    g_tag = {-1, -1, 0, 0};
    return true;
}

// Move to another phase (or, with force, start a new interval inside the same phase for a draw with its own tag).
static void SwitchPhase(IDirect3DDevice9* device, PerfPhase phase, StampTag tag, bool force)
{
    if (g_open < 0 && !OpenFrame(device, phase))
        return;
    if (phase == g_phase && !force)
        return;
    GpuFrame& frame = g_frames[g_open];
    if (frame.stampCount < kMaxStamps - 1)
        AddStamp(frame, g_phase);
    else
        g_truncatedFrames++;
    g_phase = phase;
    g_tag = tag;
}

static bool ShapeMatchesBackBuffer()
{
    if (!g_rtWidth || !g_rtHeight || !g_backBufferWidth || !g_backBufferHeight)
        return false;
    double aspect = (double)g_rtWidth / g_rtHeight, backBufferAspect = (double)g_backBufferWidth / g_backBufferHeight;
    return fabs(aspect - backBufferAspect) < 0.03 * backBufferAspect;
}

static PerfPhase ClassifyPhase(int plan)
{
    if (plan == 2)
        return PH_HUD;
    if (g_currentPsInfo && g_currentPsInfo->lightPosReg >= 0)
        return PH_LIGHTS;
    if (g_currentPsInfo && g_currentPsInfo->motionBlur)
        return PH_MOTION_BLUR;
    if (!ShapeMatchesBackBuffer())
        return PH_OFFSCREEN;
    double ratio = (double)g_rtWidth / g_backBufferWidth;
    bool mainSized = ratio >= 0.9;
    if (plan == 1)
        return mainSized ? PH_SCENE_MAIN : (ratio >= 0.2 ? PH_SCENE_HALF : PH_OFFSCREEN);
    if (mainSized)
        return PH_MAIN_OTHER;
    return ratio >= 0.05 ? PH_POST_LOW : PH_OFFSCREEN;
}

void PerfClassifyDraw(IDirect3DDevice9* device, int plan)
{
    if (g_phaseLocked)
        return;
    PerfPhase phase = ClassifyPhase(plan);
    StampTag tag = {-1, -1, 0, 0};
    bool perDraw = phase == PH_MAIN_OTHER; // a handful of full-screen draws: time each one
    if (perDraw)
        tag = {g_currentPsInfo ? g_currentPsInfo->id : -1, g_currentVsInfo ? g_currentVsInfo->id : -1,
               (unsigned short)g_rtWidth, (unsigned short)g_rtHeight};
    SwitchPhase(device, phase, tag, perDraw);
    g_timedDraw = perDraw && g_open >= 0;
    if (g_open >= 0)
        g_frames[g_open].draws[phase]++;
}

// After a draw that is timed on its own: close its interval with a second stamp, so the interval is that draw's own GPU time
// (the stamps before and after it are queued together) and what follows, up to the next draw, is the gap the CPU leaves.
void PerfDrawEnd(IDirect3DDevice9* device)
{
    (void)device;
    if (!g_timedDraw)
        return;
    g_timedDraw = false;
    if (g_open < 0)
        return;
    GpuFrame& frame = g_frames[g_open];
    if (frame.stampCount >= kMaxStamps - 1)
        return;
    AddStamp(frame, g_phase);
    g_tag = {-1, -1, 0, 0};
}

void PerfSetPhase(IDirect3DDevice9* device, PerfPhase phase)
{
    if (!g_perfActive)
        return;
    SwitchPhase(device, phase, {-1, -1, 0, 0}, false);
    g_phaseLocked = true;
}

void PerfAddSection(PerfSection section, long long ticks)
{
    g_cur.section[section] += ticks;
}
void PerfAddDraw(long long hookTicks)
{
    g_cur.hook += hookTicks;
    g_cur.hookCalls++;
}
void PerfAddDriver(long long driverTicks)
{
    g_cur.driver += driverTicks;
    g_cur.driverCalls++;
}

void PerfFrameClose(IDirect3DDevice9* device, bool sceneFrame)
{
    (void)device;
    if (!g_perfActive && g_open < 0)
        return;
    long long now = PerfNow();
    if (g_open >= 0) {
        GpuFrame& frame = g_frames[g_open];
        AddStamp(frame, g_phase);
        frame.disjoint->Issue(D3DISSUE_END);
        if (g_perfActive && sceneFrame)
            g_pending++; // (a frame that is not reported leaves its queries unread; the slot is simply used again)
        g_open = -1;
    }
    g_phaseLocked = false;
    g_phase = PH_OFFSCREEN;
    g_tag = {-1, -1, 0, 0};
    if (g_perfActive && sceneFrame) {
        if (g_lastClose) {
            g_agg.frameTicks += (double)(now - g_lastClose);
            g_agg.cpuFrames++;
            for (int s = 0; s < PS_COUNT; s++)
                g_agg.cpu.section[s] += g_cur.section[s];
            g_agg.cpu.hook += g_cur.hook;
            g_agg.cpu.driver += g_cur.driver;
            g_agg.cpu.hookCalls += g_cur.hookCalls;
            g_agg.cpu.driverCalls += g_cur.driverCalls;
        }
    }
    memset(&g_cur, 0, sizeof(g_cur));
    g_lastClose = now;
}

static void ReadFrames()
{
    if (g_freqQuery && !g_gpuFrequency) {
        unsigned long long frequency = 0;
        if (g_freqQuery->GetData(&frequency, sizeof(frequency), 0) == S_OK)
            g_gpuFrequency = frequency;
    }
    while (g_pending > 0 && g_gpuFrequency) {
        GpuFrame& frame = g_frames[g_head];
        BOOL disjoint = FALSE;
        if (frame.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK)
            break; // not finished yet
        bool ok = !disjoint && frame.stampCount >= 2;
        unsigned long long previous = 0, first = 0, last = 0;
        double phaseMs[PH_COUNT];
        memset(phaseMs, 0, sizeof(phaseMs));
        for (int i = 0; ok && i < frame.stampCount; i++) {
            unsigned long long ticks = 0;
            if (frame.stamp[i]->GetData(&ticks, sizeof(ticks), 0) != S_OK) {
                ok = false;
                break;
            }
            if (i == 0)
                first = ticks;
            else {
                double ms = (double)(ticks - previous) * 1000.0 / (double)g_gpuFrequency;
                phaseMs[frame.phase[i]] += ms;
                if (frame.phase[i] == PH_MAIN_OTHER && frame.tag[i].width != 0) {
                    const StampTag& tag = frame.tag[i];
                    DetailEntry& entry = g_detail[((unsigned long long)(tag.ps + 1) << 40) |
                                                  ((unsigned long long)(tag.vs + 1) << 20) | ((unsigned)tag.width << 10 ^ tag.height)];
                    entry.ps = tag.ps;
                    entry.vs = tag.vs;
                    entry.width = tag.width;
                    entry.height = tag.height;
                    entry.ms += ms;
                    entry.draws++;
                }
            }
            previous = last = ticks;
        }
        if (ok) {
            g_agg.gpuFrames++;
            g_agg.gpuTotalMs += (double)(last - first) * 1000.0 / (double)g_gpuFrequency;
            for (int p = 0; p < PH_COUNT; p++) {
                g_agg.gpuMs[p] += phaseMs[p];
                g_agg.draws[p] += frame.draws[p];
            }
        } else if (disjoint)
            g_disjointFrames++;
        g_head = (g_head + 1) % kFrames;
        g_pending--;
    }
}

static void WriteSummary()
{
    // ticks per second of the time stamp counter, measured against the performance counter since the DLL was loaded
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    double elapsed = (double)(now.QuadPart - g_clockStart.counter) / (double)frequency.QuadPart;
    double ticksPerSecond = elapsed > 0 ? (double)((long long)__rdtsc() - g_clockStart.ticks) / elapsed : 1e9;
    const double toMs = 1000.0 / ticksPerSecond;
    char line[900];
    if (g_agg.cpuFrames) {
        double frames = g_agg.cpuFrames, frameMs = g_agg.frameTicks * toMs / frames;
        double hookMs = g_agg.cpu.hook * toMs / frames, driverMs = g_agg.cpu.driver * toMs / frames;
        double sectionMs[PS_COUNT], sectionSum = 0;
        for (int s = 0; s < PS_COUNT; s++) {
            sectionMs[s] = g_agg.cpu.section[s] * toMs / frames;
            sectionSum += sectionMs[s];
        }
        double modPerDrawUs = g_agg.cpu.hookCalls ? (g_agg.cpu.hook - g_agg.cpu.driver) * toMs * 1000.0 / g_agg.cpu.hookCalls : 0;
        const char* test = g_cfg.skipShadows ? (g_cfg.skipReflections ? " [reflections and (test) shadows left out]" : " [(test) shadows left out]")
                                             : (g_cfg.skipReflections ? " [reflections left out]" : ""); // (draw_dispatch.cpp: ApplyReflectionSwitch, TestSkipDraw)
        snprintf(line, sizeof(line),
                 "perf%s: %.1f fps, frame %.2f ms | cpu per frame (ms): draw hooks %.2f (driver %.2f, mod %.2f = %.2f us/draw), "
                 "hud compose %.2f, readback %.2f, upload %.2f, submit %.2f, present+limiter %.2f, pose wait %.2f, "
                 "game and other %.2f | draws per frame: game %.0f, issued %.0f",
                 test, 1000.0 / frameMs, frameMs, hookMs, driverMs, hookMs - driverMs, modPerDrawUs, sectionMs[PS_COMPOSE],
                 sectionMs[PS_READBACK], sectionMs[PS_UPLOAD], sectionMs[PS_SUBMIT], sectionMs[PS_PRESENT],
                 sectionMs[PS_POSE], frameMs - hookMs - sectionSum, g_agg.cpu.hookCalls / frames,
                 g_agg.cpu.driverCalls / frames);
        Log("%s", line);
    }
    if (g_agg.gpuFrames) {
        double frames = g_agg.gpuFrames;
        int n = snprintf(line, sizeof(line), "perf: gpu %.2f ms per frame (%u frames):", g_agg.gpuTotalMs / frames,
                         g_agg.gpuFrames);
        for (int p = 0; p < PH_COUNT; p++)
            n += snprintf(line + n, sizeof(line) - n, " %s %.2f (%.0f draws)%s", kPhaseNames[p], g_agg.gpuMs[p] / frames,
                          g_agg.draws[p] / frames, p + 1 < PH_COUNT ? "," : "");
        if (g_truncatedFrames || g_overrunFrames || g_disjointFrames)
            snprintf(line + n, sizeof(line) - n, " | dropped: %u too many phase changes, %u queries not back, %u disjoint",
                     g_truncatedFrames, g_overrunFrames, g_disjointFrames);
        Log("%s", line);
        std::vector<DetailEntry> detail;
        for (const auto& item : g_detail)
            detail.push_back(item.second);
        std::sort(detail.begin(), detail.end(), [](const DetailEntry& a, const DetailEntry& b) { return a.ms > b.ms; });
        double drawMs = 0;
        for (const DetailEntry& entry : detail)
            drawMs += entry.ms / frames;
        n = snprintf(line, sizeof(line),
                     "perf: main-other, each draw bracketed (its own GPU ms per frame, draws per frame; %.2f of the phase's %.2f ms is "
                     "in these draws, the rest is the gaps between them):",
                     drawMs, g_agg.gpuMs[PH_MAIN_OTHER] / frames);
        for (size_t d = 0; d < detail.size() && d < 8; d++)
            n += snprintf(line + n, sizeof(line) - n, " ps %d vs %d %ux%u %.2f ms x%.1f%s", detail[d].ps, detail[d].vs,
                          detail[d].width, detail[d].height, detail[d].ms / frames, detail[d].draws / frames,
                          d + 1 < detail.size() && d + 1 < 8 ? "," : "");
        if (!detail.empty())
            Log("%s", line);
    } else if (g_gpuUnavailable)
        Log("perf: no GPU timestamps on this driver (CPU lines only)");
    g_detail.clear();
    memset(&g_agg, 0, sizeof(g_agg));
    g_truncatedFrames = g_overrunFrames = g_disjointFrames = 0;
}

void PerfPoll()
{
    if (!g_perfActive && g_pending == 0)
        return;
    ReadFrames();
    if (!g_perfActive)
        return;
    unsigned long long now = GetTickCount64();
    if (now - g_lastLog >= 1000) {
        g_lastLog = now;
        WriteSummary();
    }
}
