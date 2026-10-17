/* Host harness for the REAL autodetectTick() state machine from
 * companion/main.c, driven through realistic per-title frame patterns. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define FPSVITA_INTERVAL_OFF (-1)
#define FPSVITA_AUTODETECT_ENGAGE  (30)
#define FPSVITA_AUTODETECT_RELEASE (2)
#define FPSVITA_AUTODETECT_RELEASE_STREAK (60)
#define FPSVITA_AUTODETECT_RATE_MARGIN_PCT (10)
#define FPSVITA_VBLANK_US (16683)

/* Present period handed to the detector; 8 ms = a game free-running at
 * ~120 presents/s, the case inject exists for. */
static uint32_t g_period_us = 8000;

static volatile uint32_t g_frame_count       = 0;
static volatile uint32_t g_last_vblank_frame = 0;
static volatile int32_t  g_inject_active     = 0;
static volatile int32_t  g_last_interval     = FPSVITA_INTERVAL_OFF;
static volatile uint32_t g_paced_streak      = 0;

#include "autodetect.inc"   /* the real autodetectTick() */

static void reset(int32_t startInterval)
{
    g_period_us = 8000;
    g_frame_count = 0; g_last_vblank_frame = 0;
    g_inject_active = 0; g_last_interval = startInterval; g_paced_streak = 0;
}
/* one presented frame; waitEvery>0 means the game issues a vsync wait every
 * Nth present (waitEvery==1 => every frame). 0 => never waits. */
static int frame(int32_t cur, int waitEvery, int flipsPerWait)
{
    (void)flipsPerWait;
    uint32_t f = ++g_frame_count;
    if (waitEvery > 0 && (f % (uint32_t)waitEvery) == 0)
        g_last_vblank_frame = f;         /* noteVsyncWait() */
    return autodetectTick(f, cur, g_period_us);
}

static int fails = 0;
static void expect(const char *w, bool ok){ printf("    %-56s %s\n", w, ok?"OK":"** FAIL **"); if(!ok) fails++; }

/* run N frames, return how many times the inject decision flipped */
static int runCount(int32_t cur, int waitEvery, int n, int *injectedOut)
{
    int flips = 0, last = -1, injected = 0;
    for (int i = 0; i < n; i++) {
        int d = frame(cur, waitEvery, 1);
        injected += d;
        if (last >= 0 && d != last) flips++;
        last = d;
    }
    if (injectedOut) *injectedOut = injected;
    return flips;
}

int main(void)
{
    int injected, flips;

    printf("== A. Well-behaved game: vsync wait every frame ==\n");
    reset(2); runCount(2, 1, 600, &injected);
    expect("never injects", injected == 0);

    printf("== B. Game that never waits (the case inject exists for) ==\n");
    reset(2); flips = runCount(2, 0, 600, &injected);
    expect("engages", injected > 0);
    expect("engages once and stays engaged", flips == 1);
    expect("engages just after ENGAGE frames", injected == 600 - FPSVITA_AUTODETECT_ENGAGE);

    printf("== C. Waits every ~30 presents (the old oscillation case) ==\n");
    for (int every = 25; every <= 35; every++) {
        reset(2); flips = runCount(2, every, 3000, &injected);
        printf("    waitEvery=%-3d flips=%-4d injectedFrames=%-5d %s\n",
               every, flips, injected, flips <= 1 ? "" : "** OSCILLATES **");
        if (flips > 1) fails++;
    }

    printf("== D. Two presents per vsync wait (OSD/overlay plane) ==\n");
    reset(2); runCount(2, 2, 600, &injected);
    expect("does not false-positive", injected == 0);

    printf("== E. Cutscene: waits, stops for 300 frames, resumes ==\n");
    reset(2);
    runCount(2, 1, 100, NULL);
    int during = 0; runCount(2, 0, 300, &during);
    expect("engages during the non-waiting stretch", during > 0);
    int after = 0; runCount(2, 1, 200, &after);
    printf("    (release took %d frames = %.2f s at 60fps)\n", after, after/60.0);
    expect("releases after waits resume", g_inject_active == 0);
    expect("release latency is about RELEASE_STREAK frames",
           after >= FPSVITA_AUTODETECT_RELEASE_STREAK - 2 &&
           after <= FPSVITA_AUTODETECT_RELEASE_STREAK + 2);
    int settled = 0; runCount(2, 1, 200, &settled);
    expect("stays released once settled", settled == 0);

    printf("== F. Mode change resets the detector ==\n");
    reset(2);
    runCount(2, 0, 200, NULL);                      /* engaged */
    expect("engaged before the change", g_inject_active == 1);
    int d = frame(3, 0, 1);                          /* user cycles 30->20fps */
    expect("mode change clears inject", d == 0 && g_inject_active == 0);
    expect("history rebased", g_last_vblank_frame == g_frame_count);

    printf("== G. Off / Uncapped never inject ==\n");
    reset(-1); runCount(-1, 0, 300, &injected); expect("Off  -> no inject", injected == 0);
    reset(0);  runCount(0,  0, 300, &injected); expect("Uncap-> no inject", injected == 0);

    printf("== H. Counter wraparound near UINT32_MAX ==\n");
    reset(2);
    g_frame_count = 0xFFFFFFF0u; g_last_vblank_frame = 0xFFFFFFF0u;
    flips = runCount(2, 1, 100, &injected);
    expect("no spurious inject across the wrap", injected == 0);
    g_frame_count = 0xFFFFFFF0u; g_last_vblank_frame = 0xFFFFFFF0u;
    g_inject_active = 0;
    runCount(2, 0, 100, &injected);
    expect("still engages correctly across the wrap", injected == 100 - FPSVITA_AUTODETECT_ENGAGE);

    printf("== I. Rate gate: no waits seen, but the game is not faster than the cap ==\n");
    reset(2); g_period_us = 2 * FPSVITA_VBLANK_US;        /* already at 30fps (foreign wait) */
    runCount(2, 0, 600, &injected); expect("at the cap: never injects", injected == 0);
    reset(2); g_period_us = 2 * FPSVITA_VBLANK_US * 95 / 100; /* 5% faster: inside the margin */
    runCount(2, 0, 600, &injected); expect("inside the margin: never injects", injected == 0);
    reset(2); g_period_us = 2 * FPSVITA_VBLANK_US * 90 / 100; /* 10% faster: at the margin */
    runCount(2, 0, 600, &injected); expect("at the margin: engages", injected > 0);
    reset(2); g_period_us = 0;                             /* nothing measured yet */
    runCount(2, 0, 600, &injected); expect("unmeasured: never injects", injected == 0);
    reset(2); g_period_us = 3 * FPSVITA_VBLANK_US;        /* GPU bound at 20fps under a 30 cap */
    runCount(2, 0, 600, &injected); expect("slower than the cap: never injects", injected == 0);
    reset(1); g_period_us = 8000;                          /* 60 cap, 120 presents/s */
    runCount(1, 0, 600, &injected); expect("60fps cap, uncapped game: engages", injected > 0);

    printf("\n%s (%d failures)\n", fails ? "RESULT: FAILURES" : "RESULT: ALL PASS", fails);
    return fails;
}
