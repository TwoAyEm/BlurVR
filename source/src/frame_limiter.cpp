// Frame rate cap.  See frame_limiter.h.
#include "frame_limiter.h"

#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x1F0003
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void LimitFrameRate(int maxFps)
{
    if (maxFps <= 0)
        return;
    static LONGLONG frequency = 0, slot = 0; // slot: when the previous frame was due to end
    static HANDLE timer = NULL;
    static bool timerTried = false;
    if (!frequency) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = f.QuadPart;
    }
    if (!timerTried) { // a high-resolution timer (Windows 10 1803 and later); without it Sleep is as coarse as the system timer
        timerTried = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const LONGLONG deadline = slot ? slot + frequency / maxFps : 0;
    if (!deadline || counter.QuadPart >= deadline) { // first frame, or this one was late: no wait, the schedule starts here
        slot = counter.QuadPart;
        return;
    }
    slot = deadline;
    for (;;) {
        QueryPerformanceCounter(&counter);
        LONGLONG remaining = deadline - counter.QuadPart;
        if (remaining <= 0)
            break;
        double ms = remaining * 1000.0 / (double)frequency;
        if (ms > 2.0) { // sleep most of it, spin the last stretch
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((ms - 1.5) * 10000.0); // relative, in 100 ns units
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(timer, 100);
            } else
                Sleep((DWORD)(ms - 2.0 > 1.0 ? ms - 2.0 : 1.0));
        } else
            YieldProcessor();
    }
}
