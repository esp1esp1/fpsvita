/*
 * FPSVita Helper (fpsvita-helper.suprx)
 *
 * Loaded under *ALL. Runs inside every launched application process.
 * Installs display hooks and reads cap state from the kernel module on every
 * hook invocation (no threads, no polling).
 *
 * On startup:
 *   1. Gets titleid; bails out (non-resident) in non-game processes
 *   2. Writes the titleid to the kernel module
 *   3. Snapshots the current global interval, applies a per-game override
 *      if state.txt has one for this title
 *   4. Installs the hooks
 *
 * On shutdown:
 *   - Restores the snapshotted interval, so a per-game override lasts exactly
 *     as long as the game does
 *   - Clears the titleid and native interval so the UI stops reporting a game
 *     that is no longer running
 *
 * At runtime:
 *   - Vsync-wait hooks override the interval from kernel state
 *   - SetFrameBuf hook auto-detects whether inject is needed
 *   - Native interval is captured from the game's first vsync wait
 *
 * Hook logic is based on Framecapper by Rinnegatamante, adapted for
 * runtime-configurable interval via kernel module syscalls.
 */

#include <vitasdk.h>
#include <taihen.h>

#include "../common/fpsvita_common.h"
#ifdef FPSVITA_LITE
#include "lite.h"
#endif

/* ------------------------------------------------------------------
 * Prototypes vitasdk does not provide
 * ------------------------------------------------------------------ */

/*
 * Not declared by vitasdk.h (SceLibKernel exports it under this name). It returns
 * long long; the previous implicit declaration assumed int and read only r0,
 * which happened to work only because ARM is little-endian.
 */
long long sceClibStrtoll(const char *str, char **endptr, int base);

/* ------------------------------------------------------------------
 * Hooks
 * ------------------------------------------------------------------ */

enum {
	HOOK_WAIT_VBLANK_MULTI = 0,   /* 0xDD0A13B8 sceDisplayWaitVblankStartMulti */
	HOOK_WAIT_VBLANK,             /* 0x5795E898 sceDisplayWaitVblankStart */
	HOOK_WAIT_VBLANK_MULTI_CB,    /* 0x05F27764 ...MultiCB */
	HOOK_WAIT_VBLANK_CB,          /* 0x78B41B92 ...CB */
	HOOK_WAIT_SETFB_MULTI,        /* 0x7D9864A8 sceDisplayWaitSetFrameBufMulti */
	HOOK_WAIT_SETFB,              /* 0x9423560C sceDisplayWaitSetFrameBuf */
	HOOK_WAIT_SETFB_MULTI_CB,     /* 0x3E796EF5 ...MultiCB */
	HOOK_WAIT_SETFB_CB,           /* 0x814C90AF ...CB */
	HOOK_SET_FRAMEBUF,            /* 0x7A410B64 sceDisplaySetFrameBuf */
	HOOK_GXM_INITIALIZE,          /* 0xB0F1E4EC sceGxmInitialize */
	HOOK_GXM_ADD_ENTRY,           /* 0xEC5C26B5 sceGxmDisplayQueueAddEntry */
	HOOK_CTRL_PEEK,               /* 0xA9C3CED6 sceCtrlPeekBufferPositive */
	HOOK_CTRL_READ,               /* 0x67E7AB83 sceCtrlReadBufferPositive */
	HOOK_CTRL_PEEK2,              /* 0x15F81E8C sceCtrlPeekBufferPositive2 */
	HOOK_CTRL_READ2,              /* 0xC4226A3E sceCtrlReadBufferPositive2 */
	HOOK_CTRL_PEEK_EXT,           /* 0xA59454D3 sceCtrlPeekBufferPositiveExt */
	HOOK_CTRL_READ_EXT,           /* 0xE2D99296 sceCtrlReadBufferPositiveExt */
	HOOK_CTRL_PEEK_EXT2,          /* 0x860BF292 sceCtrlPeekBufferPositiveExt2 (vitaGL) */
	HOOK_CTRL_READ_EXT2,          /* 0xA7178860 sceCtrlReadBufferPositiveExt2 */
	HOOK_CTRL_PEEK_NEG,           /* 0x104ED1A7 sceCtrlPeekBufferNegative */
	HOOK_CTRL_READ_NEG,           /* 0x15F96FB0 sceCtrlReadBufferNegative */
	HOOK_CTRL_PEEK_NEG2,          /* 0x81A89660 sceCtrlPeekBufferNegative2 */
	HOOK_CTRL_READ_NEG2,          /* 0x27A0C5FB sceCtrlReadBufferNegative2 */
	HOOK_TOUCH_PEEK,              /* 0xFF082DF0 sceTouchPeek: phase marker when no ctrl hook landed */
	HOOK_TOUCH_READ,              /* 0x169A1D58 sceTouchRead */
	HOOK_COUNT
};
#define HOOK_CTRL_FIRST HOOK_CTRL_PEEK
#define HOOK_CTRL_LAST  HOOK_CTRL_READ_NEG2

static SceUID          s_hookId[HOOK_COUNT];
static tai_hook_ref_t  s_hookRef[HOOK_COUNT];

/* ------------------------------------------------------------------
 * Auto-detect state
 *
 * g_frame_count:       incremented on every SetFrameBuf call
 * g_last_vblank_frame: set to g_frame_count on every vsync wait
 * g_inject_active:     sticky, hysteretic result of the comparison below
 * g_last_interval:     last interval seen, to detect a mode change
 * ------------------------------------------------------------------ */

static volatile uint32_t g_frame_count       = 0;
static volatile uint32_t g_last_vblank_frame = 0;

/* Debug log: a few lines per process the helper starts in, all at startup
 * (module_start, late modules, GXM init); dev builds add the probe and rate
 * lines from game threads. Lines from two threads interleaved once, so one
 * writer at a time: the rest wait up to 20 ms and then drop their line
 * rather than stall a game thread on flash. */
static volatile int s_logLock;
static void helperLog(const char *msg)
{
	int tries = 20;
	while (__sync_lock_test_and_set(&s_logLock, 1)) {
		if (--tries <= 0) return;
		sceKernelDelayThread(1000);
	}
	SceUID fd = sceIoOpen("ur0:data/fpsvita/helper.log",
	                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		int n = 0;
		while (msg[n]) n++;
		sceIoWrite(fd, msg, n);
		sceIoClose(fd);
	}
	__sync_lock_release(&s_logLock);
}

static volatile int32_t  g_inject_active     = 0;
static volatile int32_t  g_last_interval     = FPSVITA_INTERVAL_OFF;
static volatile uint32_t g_paced_streak      = 0;

/* Set once this process has hooks installed and has written its title id
 * to the kernel; module_stop only clears kernel state when set. */
static int s_ownsKernelState = 0;

/* Interval owned by whoever was running before us, restored at module_stop */
static int32_t s_savedInterval  = FPSVITA_INTERVAL_OFF;
static int     s_appliedOverride = 0;

/* Everything the helper takes from state.txt, in one read at game start. */
typedef struct HelperConfig {
	int32_t pergame;      /* "TITLEID=interval" for this title, or FPSVITA_NO_PERGAME */
	int32_t interval;     /* global "interval=", or FPSVITA_INTERVAL_OFF if absent */
	int32_t softcap;      /* "softcap=<fps>", 0 off */
	int     inject;       /* "inject=", "autodetect=": seeded by the lite build only */
	int     autodetect;
	int     lowlatency;   /* "lowlatency=1" */
	int     jit;          /* "jit=1" */
	int     tear;         /* "tear=1": Uncapped flips IMMEDIATE through the kernel */
	/* Per-game options on the title's line
	 * ("TITLEID=2,lowlat=1,antilag=0,limiter=30,tearing=0"), -1 when the
	 * line has none: the globals apply. */
	int32_t pg_ll, pg_jit, pg_sc, pg_tear;
} HelperConfig;

/* Opt-in latency tweak, read from state.txt at launch (see readGameConfig) */
static int s_lowLatency = 0;
static int s_lowLatencyApplied = 0;   /* the GXM queue is 1 deep because of us */
static int s_lowLatencyNative = 0;    /* the title asked for 1 itself */
static int32_t s_savedAdvFlags = 0, s_savedSoftcap = 0;   /* kernel values before this title */
static HelperConfig s_cfg;   /* state.txt, read once in module_start */

static void noteVsyncWait(void)
{
	g_last_vblank_frame = g_frame_count;
}

/* Process-clock time the last capped wait returned: the vblank reference
 * for the controller phase numbers (only capped titles have one). */
static volatile uint32_t s_vbl_ret;
static inline int vblRet(int r)
{
	s_vbl_ret = sceKernelGetProcessTimeLow();
	return r;
}

/*
 * Advance the detector by one presented frame; returns non-zero when vsync
 * waits appear to be absent (i.e. the game is not pacing itself and inject is
 * warranted). Split out of the hook so the state machine can be exercised
 * off-device: tests/run.sh extracts it into a host harness.
 *
 * Runs even when autodetect is off, so that toggling it on mid-game reflects
 * the game's actual behaviour immediately instead of starting from a cold
 * counter.
 */
static int autodetectTick(uint32_t frames, int32_t cur, uint32_t period_us)
{
	/*
	 * A mode change invalidates the history. Without this, switching from Off
	 * (pure passthrough) to a cap evaluates the threshold against frames
	 * accumulated under the previous mode.
	 */
	if (cur != g_last_interval) {
		g_last_interval = cur;
		g_last_vblank_frame = frames;
		g_inject_active = 0;
		g_paced_streak = 0;
	}

	if (cur <= 0)   /* Off or Uncapped: never inject */
		return 0;

	/*
	 * Engage as soon as vsync waits have clearly stopped; release only after
	 * a sustained run of paced presents.
	 *
	 * The asymmetry is the point. `since` collapses to zero on every vsync
	 * wait, so releasing on a single low reading makes any title whose gap
	 * exceeds ENGAGE toggle forever: cross ENGAGE, engage, next wait, release,
	 * repeat. A bursty title never accumulates the streak because crossing
	 * ENGAGE resets it; a title that genuinely resumes pacing reaches it in
	 * about a second.
	 *
	 * The unsigned subtraction is wraparound-safe: both counters wrap together
	 * and the difference stays small.
	 */
	uint32_t since = frames - g_last_vblank_frame;

	/* Rate gate (FPSVITA_AUTODETECT_RATE_MARGIN_PCT): engaging also needs a
	 * measured present period clearly shorter than the cap's. */
	uint32_t target_us = FPSVITA_VBLANK_US * (uint32_t)cur;
	int fast = period_us != 0 &&
	           period_us * 100u <= target_us * (100u - FPSVITA_AUTODETECT_RATE_MARGIN_PCT);

	if (since > FPSVITA_AUTODETECT_ENGAGE) {
		if (fast)
			g_inject_active = 1;
		g_paced_streak = 0;
	} else if (since <= FPSVITA_AUTODETECT_RELEASE) {
		if (g_paced_streak < FPSVITA_AUTODETECT_RELEASE_STREAK)
			g_paced_streak++;
		if (g_paced_streak >= FPSVITA_AUTODETECT_RELEASE_STREAK)
			g_inject_active = 0;
	}

	return g_inject_active ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Native interval detection
 *
 * Captured once from the game's first Multi-style vsync wait. For games
 * using a non-Multi entry point, native is 1 (60fps) by definition. For games
 * using no vsync wait at all, native stays 0 (unknown).
 *
 * Note this is a first-sample latch, not a measurement: a game that starts at
 * Multi(1) and later switches to Multi(2) keeps reporting the first value.
 * ------------------------------------------------------------------ */

/* The first sample is taken at once; a change (menu at 60, gameplay at 30)
 * is taken after FPSVITA_NATIVE_STREAK identical requests so a one-off
 * does not flap the "(n:..)" readout. One syscall per change, not per call. */
#define FPSVITA_NATIVE_STREAK 16
static volatile int32_t g_native_current = 0;   /* what the kernel holds */
static volatile int32_t g_native_candidate = 0;
static volatile int32_t g_native_streak = 0;

static void noteNative(int32_t interval)
{
	if (interval <= 0)
		return;
	if (interval == g_native_current) {
		g_native_candidate = 0;
		return;
	}
	if (!g_native_current) {
		g_native_current = interval;
		fpsvitaSetNativeInterval(interval);
		return;
	}
	if (interval != g_native_candidate) {
		g_native_candidate = interval;
		g_native_streak = 1;
		return;
	}
	if (++g_native_streak >= FPSVITA_NATIVE_STREAK) {
		g_native_current = interval;
		g_native_candidate = 0;
		fpsvitaSetNativeInterval(interval);
	}
}

static void detectNativeMulti(unsigned int interval)
{
	noteNative((int32_t)interval);
}

static void detectNativeNonMulti(void)
{
	noteNative(1);
}

/* ------------------------------------------------------------------
 * Uncapped
 * ------------------------------------------------------------------ */

static int uncappedReturn(int isCallbackVariant)
{
	/*
	 * Uncapped removes what is, for most titles, the render thread's only
	 * blocking point. Returning SCE_OK bare (as this used to) turns a
	 * well-behaved thread into a spin loop at its original, usually high,
	 * priority: on the Vita's priority-preemptive scheduler that starves
	 * lower-priority threads pinned to the same core — audio mixing, file
	 * streaming, the game's own loaders. A 1 us delay is free at any
	 * achievable frame rate and gives the scheduler a reschedule point.
	 */
	if (isCallbackVariant) {
		/*
		 * The *CB entry points are callback delivery points: the kernel
		 * processes the calling thread's pending callbacks inside them.
		 * Short-circuiting without this means a game whose main thread's
		 * only callback point is its vsync wait stops receiving power and
		 * suspend notifications and PS-button exit requests for as long as
		 * Uncap is active.
		 */
		sceKernelCheckCallback();
	}
	sceKernelDelayThread(1);
	return SCE_OK;
}

/* ------------------------------------------------------------------
 * Vsync-wait hooks
 *
 * Two families, with different semantics:
 *   WaitVblankStart*(n)   waits until n vblanks after the CALLER'S last
 *                         vblank-wait resumption
 *   WaitSetFrameBuf*(n)   waits until n vblanks after the last SetFrameBuf
 *
 * Both are canonical vsync primitives. Only the first family used to be
 * hooked, so a game using the second was invisible to the cap AND tripped
 * auto-detect: g_last_vblank_frame never advanced, so after the threshold
 * inject engaged and stacked a second wait on top of the game's own.
 *
 * The non-Multi variants cannot express "wait n vblanks" through their own
 * entry point, so a capped wait is re-issued through this module's own
 * Multi import. That deliberately bypasses the rest of the taiHEN chain for
 * that call — other plugins hooking Multi (PSVshellPlus and similar) will not
 * observe it. Left as-is because changing chain semantics needs on-device
 * verification against those plugins; see REVIEW.md.
 * ------------------------------------------------------------------ */

static int sceDisplayWaitVblankStartMulti_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_MULTI], interval);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(0);
	/* Through our own import, not TAI_CONTINUE with a changed count: on
	 * DOOM64EX1 the continuation returned 0x80290001 at once for count 2
	 * while the direct call blocked for the expected two vblanks. The
	 * Framecapper plugins do the same. */
	return vblRet(sceDisplayWaitVblankStartMulti((unsigned int)cur));
}

static int sceDisplayWaitVblankStart_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK]);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(0);
	return vblRet(sceDisplayWaitVblankStartMulti((unsigned int)cur));
}

static int sceDisplayWaitVblankStartMultiCB_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_MULTI_CB],
		                    interval);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(1);
	return vblRet(sceDisplayWaitVblankStartMultiCB((unsigned int)cur));
}

static int sceDisplayWaitVblankStartCB_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();

	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_CB]);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(1);

	return vblRet(sceDisplayWaitVblankStartMultiCB((unsigned int)cur));
}

static int sceDisplayWaitSetFrameBufMulti_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_MULTI], interval);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(0);
	/* Own import, not TAI_CONTINUE with a changed count (see the vblank hooks). */
	return vblRet(sceDisplayWaitSetFrameBufMulti((unsigned int)cur));
}

static int sceDisplayWaitSetFrameBuf_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();

	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB]);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(0);

	return vblRet(sceDisplayWaitSetFrameBufMulti((unsigned int)cur));
}

static int sceDisplayWaitSetFrameBufMultiCB_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_MULTI_CB],
		                    interval);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(1);
	return vblRet(sceDisplayWaitSetFrameBufMultiCB((unsigned int)cur));
}

static int sceDisplayWaitSetFrameBufCB_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();

	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_CB]);
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return uncappedReturn(1);

	return vblRet(sceDisplayWaitSetFrameBufMultiCB((unsigned int)cur));
}

/* ------------------------------------------------------------------
 * Present hook: frame counting and inject
 * ------------------------------------------------------------------ */


/* ------------------------------------------------------------------
 * Submitter-thread pacing (sceGxmDisplayQueueAddEntry).
 *
 * Measured on hardware (docs/latency-measurements.md): in
 * every title checked the vsync wait and the flip run on a display thread,
 * and the thread that polls input and submits frames blocks in AddEntry.
 * So two things belong right after AddEntry returns, on that thread:
 *   - inject: for titles that never wait for vsync, waiting `cur` vblanks
 *     here paces the producer directly instead of stalling the display
 *     thread and leaving the queue full (one frame less latency);
 *   - jit (opt-in, `jit=1` in state.txt): re-poll a controller sample that
 *     is about to be replaced (freshPoll), and at 30/20 caps sleep whole
 *     vblanks when the budget allows so the poll lands on a later sample
 *     (jitSleep). Self-retreating: a missed period gates the sleep for 60
 *     frames and the 64-frame warm-up restarts. The sleep is gated on the
 *     controller reads clustering just after AddEntry returns, which is
 *     the only loop shape where a later poll is a fresher poll.
 * ------------------------------------------------------------------ */
#define FPSVITA_JIT_MARGIN_US   2500u
#define FPSVITA_JIT_SPIN_US     300u
#define FPSVITA_JIT_RAMP        64
#define FPSVITA_JIT_BACKOFF     60
#define FPSVITA_CTRL_WINDOW     128
#define FPSVITA_CTRL_AFTER_PCT  75

static int s_jitEnabled = 0;
static int32_t s_softcap = 0;   /* `softcap=<fps>`: time-based cap used in Uncapped mode */
static volatile uint32_t s_softcap_applied;
static volatile uint32_t s_ae_last_ret, s_ae_last_in, s_ae_tid, s_ae_period_ema;
static volatile uint32_t s_ae_last_raw;      /* previous return before any sleep: true period base */
static volatile uint32_t s_present_lag_ema; /* submit call -> flip, from the SetFrameBuf hook */
static volatile uint32_t s_present_last, s_present_period_ema; /* SetFrameBuf to SetFrameBuf */
static uint32_t s_stats_last_push, s_stats_seq;
#define FPSVITA_STATS_PUSH_US (250000)
static volatile uint32_t s_work_ema, s_work_win_max, s_work_prev_max, s_work_win_n;
static volatile uint32_t s_ctrl_calls, s_ctrl_after, s_ctrl_after_pct, s_ctrl_windows;
static volatile uint32_t s_jit_slack, s_jit_applied, s_jit_missed, s_jit_hurt;
static int s_jit_backoff, s_jit_ramp;
static uint32_t s_jit_k;       /* vblanks slept in force (0 = learning the baseline) */
static uint32_t s_jit_k_frames; /* frames since the sleep started: the flip EMA needs to settle first */
#define FPSVITA_JIT_SETTLE 48
static uint32_t s_jit_flag0;   /* per-frame submit-to-flip while not sleeping: the queue time to spend */

static inline void emaStep(volatile uint32_t *e, uint32_t x)
{
	uint32_t v = *e;
	*e = v ? (uint32_t)((int32_t)v + ((int32_t)(x - v) >> 4)) : x;
}

static void noteCtrlRead(void)
{
	uint32_t last = s_ae_last_ret;
	if (!last || !s_ae_period_ema)
		return;
	uint32_t phase = sceKernelGetProcessTimeLow() - last;
	/* Two threads may read the pad at once; divide by the local count so a
	 * concurrent reset of the shared one cannot make this a division by 0. */
	uint32_t calls = ++s_ctrl_calls;
	if (phase < s_ae_period_ema / 4)
		s_ctrl_after++;
	if (calls >= FPSVITA_CTRL_WINDOW) {
		s_ctrl_after_pct = s_ctrl_after * 100u / calls;
		s_ctrl_calls = 0; s_ctrl_after = 0; s_ctrl_windows++;
	}
}

/* ------------------------------------------------------------------
 * Latency instrument (docs/latency-measurements.md)
 *
 * Every controller read also records how old the newest sample it returned
 * is: SceCtrlData.timeStamp against sceKernelGetProcessTimeWide, which
 * shares its epoch (measured 2026-10-17; the system clock does not). Per
 * rate window: min, EMA, max in us. The min is the sample's own latency to
 * visibility; the EMA is what the game sees.
 *
 * ------------------------------------------------------------------ */
static volatile int32_t s_ts_min, s_ts_max, s_ts_ema;   /* process clock: shares the stamp's epoch */
static volatile uint32_t s_ts_n;
static volatile int32_t s_rd_ph_ema, s_ts_ph_ema;   /* read time and stamp vs the last capped wait return */
static volatile uint32_t s_fresh_hit, s_fresh_miss, s_fresh_spin_ema;
static volatile int32_t s_ts_out_ema;   /* age of the sample the game actually gets */
static volatile uint64_t s_out_stamp;    /* newest stamp delivered to the game */
static volatile int32_t s_scan_ema;      /* its age at the flip: input to scanout minus the panel */
/* Per-entry tie: the display queue flips in submission order, so the sample
 * and call time recorded at AddEntry can be aged at that frame's own flip
 * by popping a FIFO at SetFrameBuf (AddEntry's buffer arguments are sync
 * objects, not framebuffer addresses, so no address match is possible).
 * s_scan_ema above compares against whichever flip comes next, which in a
 * 2-deep queue is an older frame's; these are the honest per-frame numbers.
 * Entries older than half a second (a direct SetFrameBuf during a load) are
 * discarded. */
#define FPSVITA_ENTRY_RING 8
static struct { const void *fb; uint64_t stamp; uint32_t t_call; } s_entries[FPSVITA_ENTRY_RING];
static volatile unsigned s_entry_head, s_entry_tail;   /* push at head, pop at tail */
static volatile uint32_t s_entry_tied;   /* flips tied exactly by framebuffer address */
static SceSize s_cb_data_size;           /* displayQueueCallbackDataSize from sceGxmInitialize */
static volatile int32_t s_fscan_ema;     /* frame's own input age at its flip */
static volatile uint32_t s_flag_ema, s_entry_unmatched;   /* frame's own submit-to-flip; flips with no ring match */
#define FPSVITA_FRESH_DUE_US   15000u   /* sample this old: the next one is due within ~2 ms */
#define FPSVITA_FRESH_WAIT_US  2500u
static volatile uint32_t s_miss_dbl, s_miss_near, s_imm_calls, s_imm_err, s_imm_code;
static volatile int s_tearWanted, s_tearOk;   /* Uncapped + ADV_TEAR (last frame's state); last kernel flip succeeded */

static inline int32_t clampAge(int64_t a)
{ return a > 2000000000LL ? 2000000000 : a < -2000000000LL ? -2000000000 : (int32_t)a; }

static uint64_t newestStamp(const SceCtrlData *pad, int n)
{
	uint64_t ts = pad[0].timeStamp;
	int i;
	for (i = 1; i < n && i < 64; i++)
		if (pad[i].timeStamp > ts) ts = pad[i].timeStamp;
	return ts;
}

/* Returns the sample age in us (clamped). */
static int32_t noteCtrlSample(const SceCtrlData *pad, int n)
{
	if (!pad || n <= 0)
		return 0;
	uint64_t ts = newestStamp(pad, n);
	uint64_t now = sceKernelGetProcessTimeWide();
	int32_t a = clampAge((int64_t)(now - ts));
	uint32_t vb = s_vbl_ret;
	if (vb && (uint32_t)now - vb < 40000) {
		int32_t rd = (int32_t)((uint32_t)now - vb);   /* read after the vblank */
		int32_t tp = (int32_t)((uint32_t)ts - vb);    /* stamp relative to it (low 32 bits) */
		s_rd_ph_ema += (rd - s_rd_ph_ema) >> 4;
		s_ts_ph_ema += (tp - s_ts_ph_ema) >> 4;
	}
	if (!s_ts_n) { s_ts_min = s_ts_max = a; if (!s_ts_ema) s_ts_ema = a; }
	if (a < s_ts_min) s_ts_min = a;
	if (a > s_ts_max) s_ts_max = a;
	s_ts_ema += (a - s_ts_ema) >> 4;
	s_ts_n++;
	return a;
}

/* Fresh poll (part of jit, or exp bit 4): a sample nearly a vblank old means
 * the next one is about to land (they arrive once per vblank, 0.5 to 1.5 ms
 * after it, and a game that polls right after AddEntry returns loses that
 * race every frame: PCSE00352 read 16.2 ms old samples). Re-poll for up to
 * 2.5 ms and hand the game the new one: a frame of input latency for a
 * sub-millisecond spin (measured 0.2 to 0.5 ms, no timeouts). Self-gating by
 * age, so it needs no poll-phase gate and fires at most once per sample.
 * On the Read variants the continuation blocks until that next sample
 * instead of spinning, so the 2.5 ms check only runs after it returns; the
 * same age gate keeps that wait to the ~2 ms the sample is away. */
static int freshPoll(tai_hook_ref_t ref, int port, SceCtrlData *pad, int count, int r, int32_t age)
{
	if (!(s_jitEnabled) || r <= 0 || age < (int32_t)FPSVITA_FRESH_DUE_US || age > 40000)
		return r;
	uint64_t ts = newestStamp(pad, r);
	uint32_t t0 = sceKernelGetProcessTimeLow(), spent;
	for (;;) {
		int r2 = TAI_CONTINUE(int, ref, port, pad, count);
		spent = sceKernelGetProcessTimeLow() - t0;
		if (r2 > 0 && newestStamp(pad, r2) != ts) { r = r2; s_fresh_hit++; break; }
		if (spent > FPSVITA_FRESH_WAIT_US) { s_fresh_miss++; break; }
	}
	s_fresh_spin_ema += ((int32_t)spent - (int32_t)s_fresh_spin_ema) >> 4;
	return r;
}

static inline void noteCtrlOut(const SceCtrlData *pad, int r)
{
	if (r <= 0) return;
	uint64_t ts = newestStamp(pad, r);
	int32_t a = clampAge((int64_t)(sceKernelGetProcessTimeWide() - ts));
	s_ts_out_ema += (a - s_ts_out_ema) >> 4;
	if (ts > s_out_stamp) s_out_stamp = ts;
}

#define CTRL_HOOK(name, idx) \
	static int name##_patched(int port, SceCtrlData *pad, int count) \
	{ \
		noteCtrlRead(); \
		int r = TAI_CONTINUE(int, s_hookRef[idx], port, pad, count); \
		int32_t age = noteCtrlSample(pad, r); \
		r = freshPoll(s_hookRef[idx], port, pad, count, r, age); \
		noteCtrlOut(pad, r); \
		return r; \
	}
CTRL_HOOK(sceCtrlPeekBufferPositive,     HOOK_CTRL_PEEK)
CTRL_HOOK(sceCtrlReadBufferPositive,     HOOK_CTRL_READ)
CTRL_HOOK(sceCtrlPeekBufferPositive2,    HOOK_CTRL_PEEK2)
CTRL_HOOK(sceCtrlReadBufferPositive2,    HOOK_CTRL_READ2)
CTRL_HOOK(sceCtrlPeekBufferPositiveExt,  HOOK_CTRL_PEEK_EXT)
CTRL_HOOK(sceCtrlReadBufferPositiveExt,  HOOK_CTRL_READ_EXT)
CTRL_HOOK(sceCtrlPeekBufferPositiveExt2, HOOK_CTRL_PEEK_EXT2)
CTRL_HOOK(sceCtrlReadBufferPositiveExt2, HOOK_CTRL_READ_EXT2)
CTRL_HOOK(sceCtrlPeekBufferNegative,     HOOK_CTRL_PEEK_NEG)
CTRL_HOOK(sceCtrlReadBufferNegative,     HOOK_CTRL_READ_NEG)
CTRL_HOOK(sceCtrlPeekBufferNegative2,    HOOK_CTRL_PEEK_NEG2)
CTRL_HOOK(sceCtrlReadBufferNegative2,    HOOK_CTRL_READ_NEG2)

/* Touch counts as the poll phase only on titles with no controller import
 * (BABEL003D); elsewhere it would double-count the same poll cluster. */
static int ctrlHooked(void)
{
	int i;
	for (i = HOOK_CTRL_FIRST; i <= HOOK_CTRL_LAST; i++)
		if (s_hookId[i] >= 0) return 1;
	return 0;
}
static int sceTouchPeek_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs)
{ if (!ctrlHooked()) noteCtrlRead(); return TAI_CONTINUE(int, s_hookRef[HOOK_TOUCH_PEEK], port, pData, nBufs); }
static int sceTouchRead_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs)
{ if (!ctrlHooked()) noteCtrlRead(); return TAI_CONTINUE(int, s_hookRef[HOOK_TOUCH_READ], port, pData, nBufs); }


/* From a state already fetched this frame: no extra syscalls. */
static int injectWanted(const FpsvitaState *st)
{
	if (st->interval <= 0)
		return 0;
	if (st->autodetect == FPSVITA_AUTODETECT_ON)
		return g_inject_active;
	return st->inject == FPSVITA_INJECT_ON;
}

/* Phase-targeted (2026-10-17 measurements): controller samples land 0.5 to
 * 1.5 ms after each vblank and AddEntry returns just after one, so a sleep
 * shorter than a whole vblank only moves the read away from the landing
 * freshPoll would catch (MGS2 at a 60 cap: samples got older). The sleep is
 * k whole vblanks, the most the budget allows after the worst recent work,
 * the fresh-poll wait and a margin; freshPoll then takes the read to the
 * sample that lands after it. At a 60 cap k is always 0. Self-retreating:
 * a missed period gates it for FPSVITA_JIT_BACKOFF frames and the warm-up
 * of FPSVITA_JIT_RAMP good frames starts over. */
static void jitSleep(uint32_t t_ret, int32_t cur)
{
	uint32_t target = FPSVITA_VBLANK_US * (uint32_t)cur;
	uint32_t work = s_work_prev_max > s_work_win_max ? s_work_prev_max : s_work_win_max;
	if (work > s_work_ema * 3 + 2000)   /* one wild sample must not stall the ramp */
		work = s_work_ema * 3 + 2000;
	s_jit_slack = 0;
	if (s_jit_backoff > 0) { s_jit_backoff--; return; }
	if (s_jit_ramp < FPSVITA_JIT_RAMP) { s_jit_ramp++; return; }
	uint32_t need = work + FPSVITA_JIT_MARGIN_US + FPSVITA_FRESH_WAIT_US;
	uint32_t k = target > need ? (target - need) / FPSVITA_VBLANK_US : 0;
	if (k >= (uint32_t)cur) k = (uint32_t)cur - 1;
	/* Only time the frame would have spent queued is free to sleep: the
	 * per-frame submit-to-flip (s_flag_ema) measured while not sleeping.
	 * MGS2 at 30 queues 27 ms, so a vblank of sleep moves nothing but the
	 * read; Oddworld queues 7 ms, and the same sleep pushed its flips a
	 * vblank later. While sleeping, the flip getting later than that
	 * baseline (plus margin) means the sleep is hurting: back off, relearn. */
	if (!s_jit_k) {
		s_jit_flag0 = s_flag_ema;
		uint32_t avail = s_flag_ema > FPSVITA_JIT_MARGIN_US
		               ? (s_flag_ema - FPSVITA_JIT_MARGIN_US) / FPSVITA_VBLANK_US : 0;
		if (k > avail) k = avail;
	} else {
		if (++s_jit_k_frames >= FPSVITA_JIT_SETTLE
		    && s_flag_ema + s_jit_k * FPSVITA_VBLANK_US
		       > s_jit_flag0 + FPSVITA_JIT_MARGIN_US + FPSVITA_VBLANK_US / 4) {
			s_jit_hurt++; s_jit_backoff = FPSVITA_JIT_BACKOFF; s_jit_ramp = 0; s_jit_k = 0;
			return;
		}
		if (k > s_jit_k) k = s_jit_k;   /* never grow without relearning */
	}
	if (k && !s_jit_k) s_jit_k_frames = 0;
	s_jit_k = k;
	uint32_t slack = k * FPSVITA_VBLANK_US;
	s_jit_slack = slack;
	if (!slack)
		return;
	sceKernelDelayThread(slack - FPSVITA_JIT_SPIN_US);
	while (sceKernelGetProcessTimeLow() - t_ret < slack)
		;
	s_jit_applied++;
}

static int sceGxmDisplayQueueAddEntry_patched(void *oldBuffer, void *newBuffer,
                                              const void *callbackData)
{
	uint32_t tid = (uint32_t)sceKernelGetThreadId();
	uint32_t t_in = sceKernelGetProcessTimeLow();
	s_ae_last_in = t_in;
	{
		unsigned h = s_entry_head;
		if (h - s_entry_tail >= FPSVITA_ENTRY_RING) s_entry_tail = h - FPSVITA_ENTRY_RING + 1;
		/* The SDK samples, vitaGL and vita2d put the framebuffer address first
		 * in the callback data; where a title does, the flip is tied exactly. */
		s_entries[h % FPSVITA_ENTRY_RING].fb = (callbackData && s_cb_data_size >= sizeof(void *))
		                                       ? *(const void *const *)callbackData : NULL;
		s_entries[h % FPSVITA_ENTRY_RING].stamp = s_out_stamp;
		s_entries[h % FPSVITA_ENTRY_RING].t_call = t_in;
		s_entry_head = h + 1;
	}
	if (s_ae_last_ret && tid == s_ae_tid) {
		/* work = end of our previous sleep (or the previous return) to this
		 * call: poll, simulate, render. The time AddEntry then blocks on a
		 * full queue is the idle the slack relocates, so it is not work. */
		uint32_t work = t_in - s_ae_last_ret;
		if (work < 2000000) {
			/* CPU-side near miss: over the cap's budget by 2 ms or less.
			 * GPU time is not in it, so this is a lower bound on misses. */
			int32_t tgt = g_last_interval;   /* last fetched, no extra syscall */
			if (tgt > 0) {
				uint32_t budget = FPSVITA_VBLANK_US * (uint32_t)tgt;
				if (work > budget && work - budget <= 2000) s_miss_near++;
			}
			emaStep(&s_work_ema, work);
			if (work > s_work_win_max) s_work_win_max = work;
			if (++s_work_win_n >= 32) {
				s_work_prev_max = s_work_win_max; s_work_win_max = 0; s_work_win_n = 0;
			}
		}
	}

	int r = TAI_CONTINUE(int, s_hookRef[HOOK_GXM_ADD_ENTRY], oldBuffer, newBuffer,
	                     callbackData);

	uint32_t t_ret = sceKernelGetProcessTimeLow();
	FpsvitaState st;
	st.interval = FPSVITA_INTERVAL_OFF; st.autodetect = FPSVITA_AUTODETECT_ON; st.inject = 0;
	st.adv_flags = 0; st.softcap = 0;
	fpsvitaGetState(&st);
	int32_t cur = st.interval;
	s_jitEnabled = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
	s_softcap = st.softcap;
	if (s_ae_last_raw && tid == s_ae_tid) {
		uint32_t d = t_ret - s_ae_last_raw;      /* full frame, sleep included */
		if (d < 2000000) {
			emaStep(&s_ae_period_ema, d);
			if (cur > 0 && d > FPSVITA_VBLANK_US * (uint32_t)cur * 5u / 4u && s_jit_slack) {
				s_jit_missed++; s_jit_backoff = FPSVITA_JIT_BACKOFF; s_jit_ramp = 0; s_jit_k = 0;
			}
		}
	}
	s_ae_last_raw = t_ret;
	s_ae_tid = tid;


	if (injectWanted(&st)) {
		/* Pace the producer here; the SetFrameBuf path stands down. */
		sceDisplayWaitVblankStartMulti((unsigned int)cur);
		t_ret = sceKernelGetProcessTimeLow();
		s_vbl_ret = t_ret;
	} else if (cur > 0 && s_jitEnabled && s_ctrl_windows
	           && s_ctrl_after_pct >= FPSVITA_CTRL_AFTER_PCT) {
		jitSleep(t_ret, cur);
		t_ret = sceKernelGetProcessTimeLow();   /* the sleep is not work */
	} else if (cur == FPSVITA_INTERVAL_UNCAPPED && s_softcap > 0 && s_ae_last_ret
	           && tid == s_ae_tid) {
		/* Software cap without vsync: the waits return at once (Uncapped),
		 * and the producer is held here until the period has elapsed since
		 * the previous submit. Catch-up is clamped to one period so a long
		 * frame does not trigger a burst. The game's delta time still reads
		 * real time, so nothing desyncs; the panel just latches whatever is
		 * newest at each vblank. */
		uint32_t period = 1000000u / (uint32_t)s_softcap;
		uint32_t elapsed = t_ret - s_ae_last_ret;
		if (elapsed < period) {
			uint32_t remain = period - elapsed;
			if (remain > FPSVITA_JIT_SPIN_US)
				sceKernelDelayThread(remain - FPSVITA_JIT_SPIN_US);
			while (sceKernelGetProcessTimeLow() - s_ae_last_ret < period)
				;
			s_softcap_applied++;
		}
		t_ret = sceKernelGetProcessTimeLow();
		s_jit_slack = 0;
	} else {
		s_jit_slack = 0;
	}
	s_ae_last_ret = t_ret;
	return r;
}

/* Live numbers for the shell's overlay, pushed while it is on. */
static void pushStats(uint32_t now, const FpsvitaState *stIn)
{
	FpsvitaStats st;
	s_stats_last_push = now;
	st.fps_x10      = s_present_period_ema ? 10000000u / s_present_period_ema : 0;
	st.frame_us     = s_present_period_ema;
	st.work_us      = s_work_ema;
	st.queue_us     = s_flag_ema;
	st.input_us     = (uint32_t)(s_fscan_ema > 0 ? s_fscan_ema : 0);
	st.ctrl_pct     = s_ctrl_after_pct;
	st.jit_slack_us = s_jit_slack;
	st.softcap      = stIn->softcap;
	st.flags        = (injectWanted(stIn) ? FPSVITA_STATF_INJECT : 0)
	                | (s_lowLatencyApplied ? FPSVITA_STATF_LOWLATENCY : 0)
	                | (s_lowLatencyNative ? FPSVITA_STATF_LOWLAT_NATIVE : 0)
	                | ((stIn->adv_flags & FPSVITA_ADV_JIT) ? FPSVITA_STATF_JIT : 0)
	                | (s_hookId[HOOK_GXM_ADD_ENTRY] >= 0 ? FPSVITA_STATF_ADDENTRY : 0)
	                | (s_tearWanted && s_tearOk ? FPSVITA_STATF_TEAR : 0);
	st.jit_missed   = s_jit_missed;
	st.seq          = ++s_stats_seq;
	fpsvitaSetStats(&st);
}

static int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam,
                                          SceDisplaySetBufSync sync)
{
	/* Tearing mode: the flip goes through the kernel module
	 * (ksceDisplaySetFrameBuf, IMMEDIATE), falling back to the normal flip
	 * when that fails so the screen keeps updating. */
	int ret;
	if ((s_tearWanted) && pParam && sync == SCE_DISPLAY_SETBUF_NEXTFRAME) {
		/* Tearing: the flip takes effect at once instead of at the next
		 * vblank (verified 2026-10-17: the wait after it returns immediately,
		 * seams on camera). The first call of a process fails; fall back. */
		s_imm_calls++;
		ret = fpsvitaSetFrameBufImmediate(pParam);
		s_tearOk = ret >= 0;
		if (ret < 0) {
			s_imm_err++;
			if (!s_imm_code) s_imm_code = (uint32_t)ret;
			ret = TAI_CONTINUE(int, s_hookRef[HOOK_SET_FRAMEBUF], pParam, sync);
		}
	} else {
		s_tearOk = 0;
		ret = TAI_CONTINUE(int, s_hookRef[HOOK_SET_FRAMEBUF], pParam, sync);
	}
	/* The 64-bit stamps are written on another thread without atomicity: a
	 * torn read shows up as a negative age, so both EMAs also reject those. */
	if (s_out_stamp) {
		int32_t a = clampAge((int64_t)(sceKernelGetProcessTimeWide() - s_out_stamp));
		if (a >= 0 && a < 500000) s_scan_ema += (a - s_scan_ema) >> 4;
	}
	{
		int hit = 0;
		uint32_t tnow = sceKernelGetProcessTimeLow();
		unsigned i, head = s_entry_head, tail = s_entry_tail;
		/* Exact tie first: the newest entry whose callback data names this
		 * framebuffer. Everything older is then discarded (flipped or skipped). */
		for (i = head; i != tail && pParam; i--) {
			unsigned t = (i - 1) % FPSVITA_ENTRY_RING;
			if (s_entries[t].fb && s_entries[t].fb == pParam->base) {
				s_entry_tail = i; tail = i; s_entry_tied++; break;
			}
		}
		while (tail != head) {
			unsigned t = tail % FPSVITA_ENTRY_RING;
			uint32_t lag = tnow - s_entries[t].t_call;
			uint64_t stamp = s_entries[t].stamp;
			tail++; s_entry_tail = tail;
			if (lag >= 500000) continue;   /* stale: not this flip's entry */
			emaStep(&s_flag_ema, lag);
			if (stamp) {
				int32_t a = clampAge((int64_t)(sceKernelGetProcessTimeWide() - stamp));
				if (a >= 0 && a < 500000) s_fscan_ema += (a - s_fscan_ema) >> 4;
			}
			hit = 1; break;
		}
		if (!hit) s_entry_unmatched++;
	}
	if (s_ae_last_in) {
		uint32_t lag = sceKernelGetProcessTimeLow() - s_ae_last_in;
		if (lag < 500000) emaStep(&s_present_lag_ema, lag);
	}

	/* Atomic: some titles present from more than one thread, and a plain
	 * read-modify-write silently loses increments. */
	uint32_t frames = __sync_add_and_fetch((uint32_t *)&g_frame_count, 1);

	/* One syscall per present for everything this hook needs. */
	FpsvitaState st;
	st.interval = FPSVITA_INTERVAL_OFF; st.hud_mode = FPSVITA_HUD_OFF;
	st.autodetect = FPSVITA_AUTODETECT_ON; st.inject = 0; st.adv_flags = 0; st.softcap = 0;
	fpsvitaGetState(&st);
	s_jitEnabled = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
	s_softcap = st.softcap;
	int32_t cur = st.interval;
	s_tearWanted = (cur == FPSVITA_INTERVAL_UNCAPPED) && (st.adv_flags & FPSVITA_ADV_TEAR) != 0;

	/* Present period, the rate the game actually achieves. A gap over half a
	 * second is a load or a pause, not a frame. */
	uint32_t now = sceKernelGetProcessTimeLow();
	if (s_present_last) {
		uint32_t d = now - s_present_last;
		if (d < 500000) {
			emaStep(&s_present_period_ema, d);
			/* A present period of about twice the cap: a frame missed its
			 * vblank (GPU included; the CPU-only near count is in AddEntry). */
			if (cur > 0) {
				uint32_t budget = FPSVITA_VBLANK_US * (uint32_t)cur;
				if (d > budget * 3 / 2 && d < budget * 5 / 2) s_miss_dbl++;
			}
		}
	}
	s_present_last = now;

	int detected = autodetectTick(frames, cur, s_present_period_ema);

	if (st.hud_mode != FPSVITA_HUD_OFF && now - s_stats_last_push >= FPSVITA_STATS_PUSH_US)
		pushStats(now, &st);


	if (cur <= 0) /* Off or Uncapped: no inject ever */
		return ret;

	/* While the submit thread is going through AddEntry, inject runs there.
	 * Judged by activity, not by the hook existing: a title can import
	 * AddEntry and still present a phase (FMV, a 2D menu) with direct
	 * SetFrameBuf calls, and that phase needs the wait here. */
	if (s_ae_last_in && now - s_ae_last_in < 500000)
		return ret;

	if (st.autodetect == FPSVITA_AUTODETECT_ON) {
		if (!detected)
			return ret;
	} else {
		if (st.inject != FPSVITA_INJECT_ON)
			return ret;
	}

	/*
	 * LOAD-BEARING: this call leaves through THIS module's own import stub.
	 * Our hooks patch TAI_MAIN_MODULE's import table, so the injected wait is
	 * invisible to them and does NOT advance g_last_vblank_frame.
	 *
	 * That is what makes auto-detect stable. If the injected wait were
	 * observed, `since` would collapse to 0 on the next frame, inject would
	 * release, and re-engage ENGAGE frames later — forever. Anyone switching
	 * these hooks to taiHookFunctionExport (the natural fix for games that
	 * call sceDisplay from a sub-module) must re-check this.
	 */
	sceDisplayWaitVblankStartMulti((unsigned int)cur);
	s_vbl_ret = sceKernelGetProcessTimeLow();
	return ret;
}

/* ------------------------------------------------------------------
 * sceGxmInitialize: optional presentation-latency reduction
 * ------------------------------------------------------------------ */

static int sceGxmInitialize_patched(const SceGxmInitializeParams *params)
{
	/*
	 * displayQueueMaxPendingCount is how many presents the game may have in
	 * flight. Sony's samples and libvita2d use 2, which is a full frame of
	 * queued presentation latency bought purely as tolerance for frame-time
	 * spikes. Clamping to 1 gives that frame back (~16.7 ms at 60fps, ~33 ms
	 * under the 30fps cap) at the cost of less spike headroom, so it is
	 * opt-in via `lowlatency=1` in state.txt.
	 *
	 * Never clamp to 0. Verified on hardware (2026-10-15): sceGxmInitialize
	 * accepts 1, and lowlatency with jit held its cap on the titles tested;
	 * the per-title fps cost shows in the overlay's FPS and QUEUE rows.
	 */
	{
		char line[96];
		sceClibSnprintf(line, sizeof(line), "helper: gxm init pending=%u lowlat=%d\n",
		                params ? (unsigned)params->displayQueueMaxPendingCount : 0xFFFFFFFFu, s_lowLatency);
		helperLog(line);
	}
	if (params)
		s_cb_data_size = params->displayQueueCallbackDataSize;
	if (params && params->displayQueueMaxPendingCount <= 1)
		s_lowLatencyNative = 1;   /* the title's own choice, nothing to clamp */
	if (s_lowLatency && params && params->displayQueueMaxPendingCount > 1) {
		SceGxmInitializeParams p = *params;
		p.displayQueueMaxPendingCount = 1;
		s_lowLatencyApplied = 1;
		return TAI_CONTINUE(int, s_hookRef[HOOK_GXM_INITIALIZE], &p);
	}

	return TAI_CONTINUE(int, s_hookRef[HOOK_GXM_INITIALIZE], params);
}

/* ------------------------------------------------------------------
 * Config files
 *
 * config.txt (the globals, written whole by the shell):
 *   limit=<n>       -1 Default, 1 60fps, 2 30fps, 3 20fps, 0 Uncapped
 *   vsync=auto|0|1  forced vsync wait around the present (auto = detect)
 *   lowlat=, antilag=, tearing=, limiter=<fps>, hud=, hudpos=
 * state.txt: one "TITLEID=<limit>[,lowlat=,antilag=,limiter=,tearing=]" line per
 * title, and nothing else.
 *
 * One pass per file extracts the globals (config.txt) and this title's line
 * (state.txt). Deliberately noinline: it owns a large stack
 * buffer, and module_start runs in every process on the device, including the
 * ones that bail out immediately.
 */

#define FPSVITA_NO_PERGAME (-99)

static int lineIsGlobalKey(const char *p, int linelen)
{
	return (linelen > 6  && sceClibStrncmp(p, "limit=",   6) == 0)
	    || (linelen > 6  && sceClibStrncmp(p, "vsync=",   6) == 0)
	    || (linelen > 7  && sceClibStrncmp(p, "lowlat=",  7) == 0)
	    || (linelen > 8  && sceClibStrncmp(p, "antilag=", 8) == 0)
	    || (linelen > 8  && sceClibStrncmp(p, "tearing=", 8) == 0)
	    || (linelen > 8  && sceClibStrncmp(p, "limiter=", 8) == 0)
	    || (linelen > 4  && sceClibStrncmp(p, "hud=",     4) == 0)
	    || (linelen > 7  && sceClibStrncmp(p, "hudpos=",  7) == 0);
}


/* Read a whole file into buf; bytes read, or -1 when missing or empty. */
static int readFileInto(const char *path, char *buf, int size)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	sceClibMemset(buf, 0, size);
	int bytes = sceIoRead(fd, buf, size - 1);
	sceIoClose(fd);
	return bytes > 0 ? bytes : -1;
}

/* One pass over a key=value file. Global keys are taken only when `globals`
 * is set; the title's own line ("TITLEID=interval[,ll=,jit=,sc=]") always. */
static void parseConfigLines(char *buf, int bytes, const char *titleid,
                             HelperConfig *cfg, int globals)
{
	int tidlen = 0;
	while (titleid[tidlen] && tidlen < FPSVITA_TITLEID_LEN)
		tidlen++;

	char *p = buf;
	while (*p && p < buf + bytes) {
		char *eol = p;
		while (*eol && *eol != '\n' && eol < buf + bytes)
			eol++;

		int linelen = (int)(eol - p);

		if (lineIsGlobalKey(p, linelen)) {
			if (globals) {
				if (linelen > 7 && sceClibStrncmp(p, "lowlat=", 7) == 0)
					cfg->lowlatency = (int)sceClibStrtoll(p + 7, NULL, 10) ? 1 : 0;
				else if (linelen > 6 && sceClibStrncmp(p, "limit=", 6) == 0)
					cfg->interval = (int32_t)sceClibStrtoll(p + 6, NULL, 10);
				else if (linelen > 6 && sceClibStrncmp(p, "vsync=", 6) == 0) {
					/* "auto" (default), else forced 0/1 */
					cfg->autodetect = p[6] == 'a';
					cfg->inject = cfg->autodetect ? 0 : ((int)sceClibStrtoll(p + 6, NULL, 10) ? 1 : 0);
				}
				else if (linelen > 8 && sceClibStrncmp(p, "limiter=", 8) == 0)
					cfg->softcap = (int32_t)sceClibStrtoll(p + 8, NULL, 10);
				else if (linelen > 8 && sceClibStrncmp(p, "antilag=", 8) == 0)
					cfg->jit = (int)sceClibStrtoll(p + 8, NULL, 10) ? 1 : 0;
				else if (linelen > 8 && sceClibStrncmp(p, "tearing=", 8) == 0)
					cfg->tear = (int)sceClibStrtoll(p + 8, NULL, 10) ? 1 : 0;
			}
		} else if (linelen > tidlen + 1 && p[tidlen] == '=') {
			int match = 1;
			int i;
			for (i = 0; i < tidlen; i++) {
				if (p[i] != titleid[i]) {
					match = 0;
					break;
				}
			}
			if (match) {
				/* The kernel setter clamps the interval, so a malformed
				 * value can never reach the vblank wait as a huge count. */
				cfg->pergame = (int32_t)sceClibStrtoll(p + tidlen + 1, NULL, 10);
				const char *q = p + tidlen + 1;
				while (q < eol) {
					if (eol - q > 8 && sceClibStrncmp(q, ",lowlat=", 8) == 0)
						cfg->pg_ll = sceClibStrtoll(q + 8, NULL, 10) ? 1 : 0;
					else if (eol - q > 9 && sceClibStrncmp(q, ",antilag=", 9) == 0)
						cfg->pg_jit = sceClibStrtoll(q + 9, NULL, 10) ? 1 : 0;
					else if (eol - q > 9 && sceClibStrncmp(q, ",limiter=", 9) == 0)
						cfg->pg_sc = (int32_t)sceClibStrtoll(q + 9, NULL, 10);
					else if (eol - q > 9 && sceClibStrncmp(q, ",tearing=", 9) == 0)
						cfg->pg_tear = sceClibStrtoll(q + 9, NULL, 10) ? 1 : 0;
					q++;
				}
			}
		}

		p = (*eol == '\n') ? eol + 1 : eol;
		if (p == eol) break;
	}
}

/*
 * config.txt holds the globals, state.txt one line per title; nothing is
 * taken from state.txt but this title's line. Two reads at game start, one
 * shared buffer.
 */
static __attribute__((noinline)) void readGameConfig(const char *titleid,
                                                      HelperConfig *cfg)
{
	cfg->pergame = FPSVITA_NO_PERGAME;
	cfg->interval = FPSVITA_INTERVAL_OFF;
	cfg->inject = 0;
	cfg->autodetect = 1;
	cfg->softcap = 0;
	cfg->lowlatency = 0;
	cfg->jit = 0;
	cfg->tear = 0;
	cfg->pg_ll = cfg->pg_jit = cfg->pg_sc = cfg->pg_tear = -1;

	/* One buffer size shared with the shell (FPSVITA_STATE_BUF), so the
	 * writer can never produce a file this reader cannot fully see. Static
	 * because module_start runs on a thread whose stack is not ours to size;
	 * it is the helper's only large object. */
	static char buf[FPSVITA_STATE_BUF];
	int n = readFileInto(FPSVITA_CONFIG_FILE, buf, sizeof(buf));
	if (n > 0)
		parseConfigLines(buf, n, titleid, cfg, 1);   /* the globals */
	n = readFileInto(FPSVITA_STATE_FILE, buf, sizeof(buf));
	if (n > 0)
		parseConfigLines(buf, n, titleid, cfg, 0);   /* this title's line */
}

/* ------------------------------------------------------------------
 * Kernel titleid (3 x uint32_t parts)
 * ------------------------------------------------------------------ */

static void writeTitleIdToKernel(const char *titleid)
{
	uint32_t parts[3] = {0, 0, 0};
	int i;
	for (i = 0; i < FPSVITA_TITLEID_LEN && titleid[i]; i++)
		((char *)parts)[i] = titleid[i];

	fpsvitaSetTitleIdPart(0, parts[0]);
	fpsvitaSetTitleIdPart(1, parts[1]);
	fpsvitaSetTitleIdPart(2, parts[2]);
}

static void clearTitleIdInKernel(void)
{
	fpsvitaSetTitleIdPart(0, 0);
	fpsvitaSetTitleIdPart(1, 0);
	fpsvitaSetTitleIdPart(2, 0);
}

/* ------------------------------------------------------------------
 * Module lifecycle
 * ------------------------------------------------------------------ */


void _start() __attribute__((weak, alias("module_start")));

struct HookSpec {
	uint32_t nid;
	const void *func;
};

static const struct HookSpec s_specs[HOOK_COUNT] = {
	{ 0xDD0A13B8, (const void *)sceDisplayWaitVblankStartMulti_patched },
	{ 0x5795E898, (const void *)sceDisplayWaitVblankStart_patched },
	{ 0x05F27764, (const void *)sceDisplayWaitVblankStartMultiCB_patched },
	{ 0x78B41B92, (const void *)sceDisplayWaitVblankStartCB_patched },
	{ 0x7D9864A8, (const void *)sceDisplayWaitSetFrameBufMulti_patched },
	{ 0x9423560C, (const void *)sceDisplayWaitSetFrameBuf_patched },
	{ 0x3E796EF5, (const void *)sceDisplayWaitSetFrameBufMultiCB_patched },
	{ 0x814C90AF, (const void *)sceDisplayWaitSetFrameBufCB_patched },
	{ 0x7A410B64, (const void *)sceDisplaySetFrameBuf_patched },
	{ 0xB0F1E4EC, (const void *)sceGxmInitialize_patched },
	{ 0xEC5C26B5, (const void *)sceGxmDisplayQueueAddEntry_patched },
	{ 0xA9C3CED6, (const void *)sceCtrlPeekBufferPositive_patched },
	{ 0x67E7AB83, (const void *)sceCtrlReadBufferPositive_patched },
	{ 0x15F81E8C, (const void *)sceCtrlPeekBufferPositive2_patched },
	{ 0xC4226A3E, (const void *)sceCtrlReadBufferPositive2_patched },
	{ 0xA59454D3, (const void *)sceCtrlPeekBufferPositiveExt_patched },
	{ 0xE2D99296, (const void *)sceCtrlReadBufferPositiveExt_patched },
	{ 0x860BF292, (const void *)sceCtrlPeekBufferPositiveExt2_patched },
	{ 0xA7178860, (const void *)sceCtrlReadBufferPositiveExt2_patched },
	{ 0x104ED1A7, (const void *)sceCtrlPeekBufferNegative_patched },
	{ 0x15F96FB0, (const void *)sceCtrlReadBufferNegative_patched },
	{ 0x81A89660, (const void *)sceCtrlPeekBufferNegative2_patched },
	{ 0x27A0C5FB, (const void *)sceCtrlReadBufferNegative2_patched },
	{ 0xFF082DF0, (const void *)sceTouchPeek_patched },
	{ 0x169A1D58, (const void *)sceTouchRead_patched },
};

static char    s_titleid[32];
static int32_t s_pergame = FPSVITA_NO_PERGAME;

/* Try the display hooks on one module's imports. Each hook is installed at
 * most once (the patched functions carry a single ref per NID), so the first
 * module that imports a function keeps it. Returns the number added. */
static int installDisplayHooks(const char *modname)
{
	int i, added = 0;
	for (i = 0; i < HOOK_COUNT; i++) {
		if (s_hookId[i] >= 0)
			continue;
		s_hookId[i] = taiHookFunctionImport(&s_hookRef[i], modname,
		                                    TAI_ANY_LIBRARY, s_specs[i].nid,
		                                    (void *)s_specs[i].func);
		if (s_hookId[i] >= 0)
			added++;
	}
	return added;
}

/* Hooks that can pace or observe presentation (display waits, SetFrameBuf,
 * GXM). Controller and touch hooks alone are instrumentation: a main module
 * with its input in the executable and its renderer in a PRX loaded later
 * must still get the loader hooks, and must not take the kernel state until
 * the display imports are found. */
static int pacingHooked(void)
{
	int i;
	for (i = 0; i < HOOK_CTRL_FIRST; i++)
		if (s_hookId[i] >= 0) return 1;
	return 0;
}

static unsigned hookMask(void)
{
	unsigned mask = 0;
	int i;
	for (i = 0; i < HOOK_COUNT; i++)
		if (s_hookId[i] >= 0) mask |= 1u << i;
	return mask;
}


/* Hooks are live: this process now owns the kernel's game state.
 * The interval is set from the state file, not inherited: a process the
 * system kills (a forced quit, a crash) never runs module_stop,
 * so whatever it had applied would otherwise leak into the next title. */
static void takeKernelOwnership(void)
{
	if (s_ownsKernelState)
		return;
	writeTitleIdToKernel(s_titleid);
	fpsvitaSetNativeInterval(0);
	s_savedInterval = s_cfg.interval;
	{
		/* Latency options for this title go to the kernel, where the quick
		 * menu can change them live; the frame path reads them back. */
		/* Restore target at module_stop: the globals from config.txt, like
		 * the interval. Not the kernel's current values, which after a killed
		 * title (no module_stop) are still that title's. */
		s_savedAdvFlags = (s_cfg.lowlatency ? FPSVITA_ADV_LOWLAT : 0) | (s_cfg.jit ? FPSVITA_ADV_JIT : 0)
		                | (s_cfg.tear ? FPSVITA_ADV_TEAR : 0);
		s_savedSoftcap = s_cfg.softcap;
		int jit = s_cfg.pg_jit >= 0 ? s_cfg.pg_jit : s_cfg.jit;
		int tear = s_cfg.pg_tear >= 0 ? s_cfg.pg_tear : s_cfg.tear;
		int32_t sc = s_cfg.pg_sc >= 0 ? s_cfg.pg_sc : s_cfg.softcap;
		fpsvitaSetAdvanced((s_lowLatency ? FPSVITA_ADV_LOWLAT : 0) | (jit ? FPSVITA_ADV_JIT : 0)
		                   | (tear ? FPSVITA_ADV_TEAR : 0), sc);
	}
	if (s_pergame != FPSVITA_NO_PERGAME) {
		fpsvitaSetInterval(s_pergame);
		s_appliedOverride = 1;
	} else {
		fpsvitaSetInterval(s_savedInterval);
	}
	g_last_interval = fpsvitaGetInterval();
	s_ownsKernelState = 1;
}

/* ------------------------------------------------------------------
 * Late modules. Some titles (HELP04040, PCSA00029) keep their display
 * calls in a module they load after start, so the main module imports
 * nothing we can hook. Hook the loaders and retry on each new module.
 * ------------------------------------------------------------------ */

enum { LOADER_LOAD_START = 0, LOADER_START, LOADER_LOAD_START_INTERNAL, LOADER_COUNT };
static SceUID         s_loaderId[LOADER_COUNT] = { -1, -1, -1 };
static tai_hook_ref_t s_loaderRef[LOADER_COUNT];

static void onModuleStarted(SceUID modid)
{
	char line[96];
	SceKernelModuleInfo info;
	if (modid < 0 || s_ownsKernelState)
		return;
	info.size = sizeof(info);
	if (sceKernelGetModuleInfo(modid, &info) < 0)
		return;
	int added = installDisplayHooks(info.module_name);
	if (added > 0) {
		sceClibSnprintf(line, sizeof(line), "helper: late module %s hooks+=%d mask=0x%04x\n",
		                info.module_name, added, hookMask());
		helperLog(line);
	}
	if (pacingHooked())
		takeKernelOwnership();
}

static SceUID sceKernelLoadStartModule_patched(const char *path, SceSize args,
                                               void *argp, int flags,
                                               void *option, int *status)
{
	SceUID r = TAI_CONTINUE(SceUID, s_loaderRef[LOADER_LOAD_START], path, args,
	                        argp, flags, option, status);
	onModuleStarted(r);
	return r;
}

static int sceKernelStartModule_patched(SceUID modid, SceSize args, void *argp,
                                        int flags, void *option, int *status)
{
	int r = TAI_CONTINUE(int, s_loaderRef[LOADER_START], modid, args, argp,
	                     flags, option, status);
	if (r >= 0)
		onModuleStarted(modid);
	return r;
}

static SceUID _sceKernelLoadStartModule_patched(const char *path, SceSize args,
                                                void *argp, int flags,
                                                void *option, int *status)
{
	SceUID r = TAI_CONTINUE(SceUID, s_loaderRef[LOADER_LOAD_START_INTERNAL],
	                        path, args, argp, flags, option, status);
	onModuleStarted(r);
	return r;
}

static int installLoaderHooks(void)
{
	static const struct HookSpec specs[LOADER_COUNT] = {
		{ 0x2DCC4AFA, (const void *)sceKernelLoadStartModule_patched },  /* SceLibKernel */
		{ 0x702425D5, (const void *)sceKernelStartModule_patched },      /* SceLibKernel */
		{ 0x60647592, (const void *)_sceKernelLoadStartModule_patched }, /* SceModulemgr */
	};
	int i, n = 0;
	for (i = 0; i < LOADER_COUNT; i++) {
		s_loaderId[i] = taiHookFunctionImport(&s_loaderRef[i], TAI_MAIN_MODULE,
		                                      TAI_ANY_LIBRARY, specs[i].nid,
		                                      (void *)specs[i].func);
		if (s_loaderId[i] >= 0)
			n++;
	}
	return n;
}

int module_start(SceSize argc, const void *args)
{
	int i;
	char line[96];

	for (i = 0; i < HOOK_COUNT; i++)
		s_hookId[i] = -1;

	sceClibMemset(s_titleid, 0, sizeof(s_titleid));
	/* Param 12 = title id (what VitaGrafix and PSVshellPlus use). */
	int pr = sceAppMgrAppParamGetString(0, 12, s_titleid, sizeof(s_titleid));
	/* No title id, or a system app (NPXS*: screenshot service, trophy
	 * manager, ...): nothing to pace. Leave without touching the flash. */
	if (pr < 0 || s_titleid[0] == '\0' || sceClibStrncmp(s_titleid, "NPXS", 4) == 0)
		return SCE_KERNEL_START_NO_RESIDENT;

	readGameConfig(s_titleid, &s_cfg);
	s_pergame = s_cfg.pergame;
#ifdef FPSVITA_LITE
	/* No shell to seed the globals: this process is its own kernel. */
	fpsvitaSetInterval(s_cfg.interval);
	fpsvitaSetInject(s_cfg.inject);
	fpsvitaSetAutodetect(s_cfg.autodetect);
#endif
	s_lowLatency = s_cfg.pg_ll >= 0 ? s_cfg.pg_ll : s_cfg.lowlatency;

	int installed = installDisplayHooks(TAI_MAIN_MODULE);
	int pacing = pacingHooked();
	int loaders = pacing ? 0 : installLoaderHooks();
	sceClibSnprintf(line, sizeof(line),
	                "helper: %s hooks=%d mask=0x%04x loaders=%d pergame=%d\n",
	                s_titleid, installed, hookMask(), loaders, (int)s_pergame);
	helperLog(line);

	if (pacing)
		takeKernelOwnership();
	/* Stay resident while any hook or loader hook points into this module.
	 * Nothing at all (e.g. NPXS10036, the screenshot service): leave. */
	return (installed > 0 || loaders > 0) ? SCE_KERNEL_START_SUCCESS
	                                      : SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize argc, const void *args)
{
	int i;
	for (i = 0; i < HOOK_COUNT; i++) {
		if (s_hookId[i] >= 0) {
			taiHookRelease(s_hookId[i], s_hookRef[i]);
			s_hookId[i] = -1;   /* a second module_stop must not re-release */
		}
	}
	for (i = 0; i < LOADER_COUNT; i++) {
		if (s_loaderId[i] >= 0) {
			taiHookRelease(s_loaderId[i], s_loaderRef[i]);
			s_loaderId[i] = -1;
		}
	}
	if (s_appliedOverride) {
		fpsvitaSetInterval(s_savedInterval);
		s_appliedOverride = 0;
	}
	if (s_ownsKernelState) {
		clearTitleIdInKernel();
		fpsvitaSetNativeInterval(0);
		fpsvitaSetAdvanced(s_savedAdvFlags, s_savedSoftcap);
		s_ownsKernelState = 0;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
