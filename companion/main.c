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
#include "btnmerge.h"
#include "stkmerge.h"
#define FPSVITA_COMPAT_DEFINE   /* single TU: define the table here */
#include "../shell/compat_data.h"
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
	HOOK_REFRESH_RATE,            /* 0xA08CA60D sceDisplayGetRefreshRate: panel-rate lie */
	HOOK_GXM_VSH_INITIALIZE,      /* 0xA04F5FAC sceGxmVshInitialize: what newer vitaGL ports call */
	HOOK_PVR_SWAP,                /* 0xA7D1D94E PVRSRVSwapToDCBuffer: the PowerVR GLES stack's submit */
	HOOK_COUNT
};
#define HOOK_CTRL_FIRST HOOK_CTRL_PEEK
#define HOOK_CTRL_LAST  HOOK_CTRL_READ_NEG2

static SceUID          s_hookId[HOOK_COUNT];
static tai_hook_ref_t  s_hookRef[HOOK_COUNT];
static int installHooksOnLoadedModules(void);   /* with the hook table, below */
static unsigned hookMask(void);

#define logicWaitRet(r) (r)

/* Panel mode: the firmware still reports the stock 59.94 Hz, so a game that
 * computes dt from the reported rate sees the wrong one. Return the granted
 * rate instead. Runs in release builds too; the dev probe counts calls. */
static int sceDisplayGetRefreshRate_patched(float *fps)
{
	int r = TAI_CONTINUE(int, s_hookRef[HOOK_REFRESH_RATE], fps);
	if (r >= 0 && fps) {
		FpsvitaState st;
		st.panel_hz = 0;
		fpsvitaGetState(&st, sizeof(st));
		if (st.panel_hz > 0)
			*fps = (float)st.panel_hz;
	}
	return r;
}

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
static uint32_t g_rate_t0 = 0; /* rate report window start */
static int s_rateLog = 0;      /* config.txt `log=1` */

/* Debug log: a few lines per process the helper starts in, all at startup
 * (module_start, late modules, GXM init); `log=1` adds the rate lines and
 * dev builds the probe lines from game threads. Lines from two threads interleaved once, so one
 * writer at a time: the rest wait up to 20 ms and then drop their line
 * rather than stall a game thread on flash. */
#define FPSVITA_LOG_PATH "ur0:data/fpsvita/helper.log"
#define FPSVITA_LOG_MAX  (256u * 1024u)
static volatile int s_logLock;

/* One rotation per helper start, before the first line. Keeps the log bounded
 * without a flash operation in the frame path. */
static void rotateHelperLog(void)
{
	SceIoStat st;
	if (sceIoGetstat(FPSVITA_LOG_PATH, &st) >= 0 && st.st_size > FPSVITA_LOG_MAX) {
		sceIoRemove(FPSVITA_LOG_PATH ".1");
		sceIoRename(FPSVITA_LOG_PATH, FPSVITA_LOG_PATH ".1");
	}
}

static void helperLog(const char *msg)
{
	int tries = 20;
	while (__sync_lock_test_and_set(&s_logLock, 1)) {
		if (--tries <= 0) return;
		sceKernelDelayThread(1000);
	}
	SceUID fd = sceIoOpen(FPSVITA_LOG_PATH,
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
/* One vblank: 59.94 Hz unless a per-game panel=<hz> is in force (from the
 * kernel's panel_hz on every state read). */
static volatile uint32_t g_vblank_us = FPSVITA_VBLANK_US;
static volatile uint32_t g_paced_streak      = 0;

/* Set once this process has hooks installed and has written its title id
 * to the kernel; module_stop only clears kernel state when set. */
static int s_ownsKernelState = 0;

/* Interval owned by whoever was running before us, restored at module_stop */
static int32_t s_savedInterval  = FPSVITA_INTERVAL_OFF;
static int     s_savedInject    = FPSVITA_INJECT_OFF;
static int     s_savedAutodetect = FPSVITA_AUTODETECT_ON;
static int     s_appliedOverride = 0;

/* Everything the helper takes from state.txt, in one read at game start. */
typedef struct HelperConfig {
	int32_t pergame;      /* "TITLEID=interval" for this title, or FPSVITA_NO_PERGAME */
	int32_t interval;     /* global "interval=", or FPSVITA_INTERVAL_OFF if absent */
	int32_t softcap;      /* "softcap=<fps>", 0 off */
	int     inject;       /* "inject=", "autodetect=": seeded by the lite build only */
	int     autodetect;
	int     lowlatency;   /* "lowlatency=1" */
	int     jit;          /* "antilag=0/1" ("auto" reads as 1) */
	int     tear;         /* "tear=1": Uncapped flips IMMEDIATE through the kernel */
	/* Per-game options on the title's line
	 * ("TITLEID=2,lowlat=1,antilag=0,limiter=30,tearing=0"), -1 when the
	 * line has none: the globals apply. */
	int32_t pg_ll, pg_jit, pg_sc, pg_tear;
	int32_t pg_vsync;     /* per-game "vsync=": -1 absent, 0 auto, 1 off, 2 on */
	int     pg_read;      /* per-game "read=peek": blocking Read becomes Peek */
	int32_t pg_panel;     /* per-game "panel=<hz>" (experimental), 0 none */
	int     log;          /* "log=1": rate lines in helper.log */
	int     syscon;       /* "syscon=0": Antilag without the syscon presses */
	int     sticks;       /* "sticks=0": Antilag without the early analog sticks */
	int     panelmodes;   /* "panelmodes=1" (or the older "experimental=1"): per-game panel= applies */
} HelperConfig;

/* Opt-in latency tweak, read from state.txt at launch (see readGameConfig) */
static int s_lowLatency = 0;
static int s_readPeek = 0;            /* per-game "read=peek": Read becomes Peek */
static int s_lowLatencyApplied = 0;   /* the GXM queue is 1 deep because of us */
static int s_lowLatencyNative = 0;    /* the title asked for 1 itself */
static int32_t s_savedAdvFlags = 0, s_savedSoftcap = 0;   /* kernel values before this title */
static HelperConfig s_cfg;   /* state.txt, read once in module_start */

static void noteVsyncWait(void)
{
	g_last_vblank_frame = g_frame_count;
}

/* Process-clock time the last pacing wait returned: the vblank reference for
 * the controller phase numbers, and the start of the frame's own work for
 * titles that present by flipping directly (FPSVITA_STATF_FLIPWORK). */
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
	uint32_t target_us = g_vblank_us * (uint32_t)cur;
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
 * verification against those plugins.
 * ------------------------------------------------------------------ */

static int sceDisplayWaitVblankStartMulti_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	/* Off: the game paces itself, but its wait's return still marks where
	 * its own frame work starts, which is what the flip-side work uses. */
	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_MULTI], interval)));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(0));
	/* Through our own import, not TAI_CONTINUE with a changed count: on
	 * DOOM64EX1 the continuation returned 0x80290001 at once for count 2
	 * while the direct call blocked for the expected two vblanks. The
	 * Framecapper plugins do the same. */
	return logicWaitRet(vblRet(sceDisplayWaitVblankStartMulti((unsigned int)cur)));
}

static int sceDisplayWaitVblankStart_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK])));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(0));
	return logicWaitRet(vblRet(sceDisplayWaitVblankStartMulti((unsigned int)cur)));
}

static int sceDisplayWaitVblankStartMultiCB_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_MULTI_CB],
		                                        interval)));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(1));
	return logicWaitRet(vblRet(sceDisplayWaitVblankStartMultiCB((unsigned int)cur)));
}

static int sceDisplayWaitVblankStartCB_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();

	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_VBLANK_CB])));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(1));

	return logicWaitRet(vblRet(sceDisplayWaitVblankStartMultiCB((unsigned int)cur)));
}

static int sceDisplayWaitSetFrameBufMulti_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_MULTI], interval)));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(0));
	/* Own import, not TAI_CONTINUE with a changed count (see the vblank hooks). */
	return logicWaitRet(vblRet(sceDisplayWaitSetFrameBufMulti((unsigned int)cur)));
}

static int sceDisplayWaitSetFrameBuf_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();
	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB])));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(0));

	return logicWaitRet(vblRet(sceDisplayWaitSetFrameBufMulti((unsigned int)cur)));
}

static int sceDisplayWaitSetFrameBufMultiCB_patched(unsigned int interval)
{
	noteVsyncWait();
	detectNativeMulti(interval);
	int32_t cur = fpsvitaGetInterval();
	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_MULTI_CB],
		                                        interval)));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(1));
	return logicWaitRet(vblRet(sceDisplayWaitSetFrameBufMultiCB((unsigned int)cur)));
}

static int sceDisplayWaitSetFrameBufCB_patched(void)
{
	noteVsyncWait();
	detectNativeNonMulti();

	int32_t cur = fpsvitaGetInterval();

	if (cur == FPSVITA_INTERVAL_OFF)
		return logicWaitRet(vblRet(TAI_CONTINUE(int, s_hookRef[HOOK_WAIT_SETFB_CB])));
	if (cur == FPSVITA_INTERVAL_UNCAPPED)
		return logicWaitRet(uncappedReturn(1));

	return logicWaitRet(vblRet(sceDisplayWaitSetFrameBufMultiCB((unsigned int)cur)));
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
 *   - antilag (`antilag=1`, the default): re-poll a controller sample
 *     that is about to be replaced (freshPoll), and at 30/20 caps sleep
 *     whole vblanks when the budget allows, so the poll lands on a later
 *     sample (jitSleep). Self-retreating: a missed period gates the sleep for 60
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

static int s_jitEnabled = 0;    /* antilag=1: fresh poll, the whole-vblank sleep, syscon presses */
static int s_sysconOn = 1;      /* config.txt syscon= (default 1): the syscon presses part of Antilag */
static int s_sticksOn = 1;      /* config.txt sticks= (default 1): the early analog sticks part of Antilag */
static volatile uint32_t s_btn_diff, s_btn_total;   /* reads where the syscon was ahead of the newest sample */
static volatile int32_t  s_btn_lead_ema;            /* on those reads: sample age minus press age, us */
static BtnMerge s_btn;                              /* which bits the syscon may deliver early */
static uint64_t s_btn_changed_last;                 /* the kernel word's change time at the previous read */
static volatile uint32_t s_btn_status;              /* FpsvitaButtons.status from the last read */
static StkMerge s_stk;                              /* early sticks: per-axis learned scale and trust */
static uint64_t s_stk_last_ts;                      /* newest sample stamp last learned from */
static volatile uint32_t s_stk_deliver_last;        /* axes delivered on the last read (bitmask) */
static int32_t s_softcap = 0;   /* `softcap=<fps>`: time-based cap used in Uncapped mode */
static volatile uint32_t s_softcap_applied;
/* Limiter schedule: the next release time, a period after the previous
 * target (Uncapped has no real vblank timestamp). 0 = start over. Only the
 * submit thread touches it. */
static uint32_t s_lim_next;
static volatile uint32_t s_ae_last_ret, s_ae_last_in, s_ae_tid, s_ae_period_ema;
static volatile uint32_t s_ae_last_raw;      /* previous return before any sleep: true period base */
static volatile uint32_t s_present_lag_ema; /* submit call -> flip, from the SetFrameBuf hook */
static volatile uint32_t s_present_last, s_present_period_ema; /* SetFrameBuf to SetFrameBuf */
static uint32_t s_stats_last_push, s_stats_seq;
#define FPSVITA_STATS_PUSH_US (250000)
static volatile uint32_t s_work_ema, s_work_win_max, s_work_prev_max, s_work_win_n;
/* The same measure without AddEntry: the frame's own time from its pacing
 * wait's return to the flip. A driver that presents by calling SetFrameBuf
 * itself (the PowerVR GLES stack) leaves nothing else to time. */
static volatile uint32_t s_flip_work_ema;
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
#define FPSVITA_FRESH_EARLY_US 1683u    /* sample a vblank less this old: the next one is due within ~2 ms */
#define FPSVITA_FRESH_WAIT_US  2500u
static volatile uint32_t s_miss_dbl, s_miss_near, s_imm_calls, s_imm_err, s_imm_code;
/* The last 512 present periods, for the overlay's 1% low (slot = present
 * count mod 512; a gap over half a second leaves its slot alone). */
#define FPSVITA_PERIOD_RING 512
static volatile uint32_t s_period_ring[FPSVITA_PERIOD_RING];
static volatile uint32_t s_period_base;   /* present count when the ring last restarted */
static volatile uint32_t s_period_regime; /* interval, Limiter and panel rate the ring was filled under */
static volatile int s_gxmInitSeen;   /* the sceGxmInitialize hook ran in this process */
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

/* Fresh poll (part of antilag, or exp bit 4): a sample nearly a vblank old means
 * the next one is about to land (they arrive once per vblank, 0.5 to 1.5 ms
 * after it, and a game that polls right after AddEntry returns loses that
 * race every frame: PCSE00352 read 16.2 ms old samples). Re-poll for up to
 * 2.5 ms and hand the game the new one: a frame of input latency for a
 * sub-millisecond spin (measured 0.2 to 0.5 ms, no timeouts). Self-gating by
 * age, so it needs no poll-phase gate and fires at most once per sample.
 * On the Read variants the continuation blocks until that next sample
 * instead of spinning, so the 2.5 ms check only runs after it returns; the
 * same age gate keeps that wait to the ~2 ms the sample is away. */
static int peekCall(int readIdx, int port, SceCtrlData *pad, int count);

/* peekIdx >= 0: a read=peek title, re-poll through that Read's Peek. */
static int freshPoll(tai_hook_ref_t ref, int peekIdx, int port, SceCtrlData *pad, int count,
                     int r, int32_t age)
{
	if (!(s_jitEnabled)
	    || r <= 0 || age < (int32_t)(g_vblank_us - FPSVITA_FRESH_EARLY_US)
	    || age > (int32_t)(g_vblank_us * 12u / 5u))
		return r;
	uint64_t ts = newestStamp(pad, r);
	uint32_t t0 = sceKernelGetProcessTimeLow(), spent;
	for (;;) {
		int r2 = peekIdx >= 0 ? peekCall(peekIdx, port, pad, count)
		                      : TAI_CONTINUE(int, ref, port, pad, count);
		spent = sceKernelGetProcessTimeLow() - t0;
		if (r2 > 0 && newestStamp(pad, r2) != ts) { r = r2; s_fresh_hit++; break; }
		if (spent > FPSVITA_FRESH_WAIT_US) { s_fresh_miss++; break; }
	}
	s_fresh_spin_ema += ((int32_t)spent - (int32_t)s_fresh_spin_ema) >> 4;
	return r;
}


/* Syscon control bits -> SceCtrlData button bits. Game buttons only.
 * Confirmed by the 2026-10-27 probe for bit 6 (Cross) and bit 10 (R). */
#define FPSVITA_SYSCON_GAME_MASK (SCE_CTRL_UP | SCE_CTRL_RIGHT | SCE_CTRL_DOWN | SCE_CTRL_LEFT \
	| SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_SELECT \
	| SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)
static uint32_t sysconToCtrl(uint32_t b)
{
	uint32_t m = 0;
	if (b & 0x0001) m |= SCE_CTRL_UP;
	if (b & 0x0002) m |= SCE_CTRL_RIGHT;
	if (b & 0x0004) m |= SCE_CTRL_DOWN;
	if (b & 0x0008) m |= SCE_CTRL_LEFT;
	if (b & 0x0010) m |= SCE_CTRL_TRIANGLE;
	if (b & 0x0020) m |= SCE_CTRL_CIRCLE;
	if (b & 0x0040) m |= SCE_CTRL_CROSS;
	if (b & 0x0080) m |= SCE_CTRL_SQUARE;
	if (b & 0x0100) m |= SCE_CTRL_SELECT;
	if (b & 0x0200) m |= SCE_CTRL_LTRIGGER;
	if (b & 0x0400) m |= SCE_CTRL_RTRIGGER;
	if (b & 0x0800) m |= SCE_CTRL_START;
	return m;
}


/* Early sticks: hand the game a fresher analog reading than SceCtrl's
 * once-per-vblank sample. The kernel polls the syscon every few ms
 * (fpsvitaGetSticks); SceCtrl's sample lags up to a vblank. `koff` moves the
 * sample's process-clock stamp into the kernel clock to pair it with the ring
 * (nearest read = learn SceCtrl's transform; newest read = deliver). Per-axis
 * gate in companion/stkmerge.h keeps it off until the learned scale matches
 * what SceCtrl reports, so a stick-reshaping plugin (AnalogsEnhancer) or
 * digital sampling simply never trusts and nothing changes. Sticks are the
 * same bytes in positive and negative buffers, so `neg` does not matter here.
 * Inert without the kernel poll, so release builds deliver nothing until it is
 * unfenced. */
static void stkDeliver(SceCtrlData *pad, int r, uint64_t koff, uint64_t know)
{
	FpsvitaSticks ks;
	uint32_t cnt, newest, mask = 0;
	int i, raw_fresh;
	const uint8_t *rawNow;
	static int s_stk_init;
	if (!s_stk_init) { stkMergeInit(&s_stk); s_stk_init = 1; }
	if (r <= 0 || fpsvitaGetSticks(&ks) < 0 || ks.n == 0) { s_stk_deliver_last = 0; return; }
	cnt = ks.n < FPSVITA_STK_RING ? ks.n : FPSVITA_STK_RING;
	newest = (ks.n - 1) % FPSVITA_STK_RING;
	rawNow = ks.v[newest];
	raw_fresh = (int64_t)(know - ks.at[newest]) < (int64_t)g_vblank_us;
	/* Learn once per new SceCtrl sample, from the raw read nearest its time. */
	if (pad[r - 1].timeStamp != s_stk_last_ts) {
		uint64_t want = pad[r - 1].timeStamp + koff;
		int best = -1; int64_t bestd = 0; uint32_t j;
		s_stk_last_ts = pad[r - 1].timeStamp;
		for (j = 0; j < cnt; j++) {
			int64_t d = (int64_t)(ks.at[j] - want); if (d < 0) d = -d;
			if (best < 0 || d < bestd) { best = (int)j; bestd = d; }
		}
		if (best >= 0 && bestd < (int64_t)g_vblank_us) {
			const uint8_t *rs = ks.v[best];
			uint8_t sm[4]; int a;
			sm[0] = pad[r - 1].lx; sm[1] = pad[r - 1].ly; sm[2] = pad[r - 1].rx; sm[3] = pad[r - 1].ry;
			for (a = 0; a < 4; a++) stk_learn(&s_stk, a, rs[a], sm[a]);
		}
	}
	for (i = 0; i < r && i < 64; i++) {
		uint8_t out[4]; int m;
		out[0] = pad[i].lx; out[1] = pad[i].ly; out[2] = pad[i].rx; out[3] = pad[i].ry;
		m = stk_deliver(&s_stk, rawNow, raw_fresh, out);
		if (m) { pad[i].lx = out[0]; pad[i].ly = out[1]; pad[i].rx = out[2]; pad[i].ry = out[3]; }
		mask |= m;
	}
	s_stk_deliver_last = mask;
}

/* neg: the sample is a sceCtrl*Negative buffer (bits clear = pressed). */
static inline void noteCtrlOut(SceCtrlData *pad, int r, int neg)
{
	if (r <= 0) return;
	uint64_t ts = newestStamp(pad, r);
	/* Antilag's syscon presses: add the buttons the syscon says are down
	 * right now to every returned sample, and drop the ones it says are up.
	 * SceCtrl samples the syscon once per vblank; the kernel's word is 10 ms
	 * ahead on average (17 ms max, one vblank; up to 25 ms at a 40 Hz panel).
	 * The syscon reports released buttons as 1s. The press is stamped with
	 * the time the word changed, moved into the process clock from the kernel
	 * clock the same call returned, so INPUT measures what the game really
	 * got. The kernel's status says when not to: no syscon word has been
	 * read yet (a PS TV). */
	if (s_jitEnabled && (s_sysconOn || s_sticksOn)) {
		FpsvitaButtons kb;
		if (fpsvitaGetButtons(&kb) >= 0) {
			s_btn_status = kb.status;
			if (kb.status == 0) {
				uint64_t pnow = sceKernelGetProcessTimeWide();
				if (s_sysconOn) {
					uint32_t m = sysconToCtrl(~kb.raw) & FPSVITA_SYSCON_GAME_MASK;
					uint32_t newest = neg ? ~pad[r - 1].buttons : pad[r - 1].buttons;
					uint32_t add, clear;
					int i;
					/* Which bits to deliver early and which stale presses to hide:
					 * companion/btnmerge.h. A bit qualifies once a press of it has
					 * reached the game as itself, so a remapper's consumed buttons,
					 * turbo and sticky holds and injected presses stay theirs
					 * (verified under reVita 1.0.8 on 2026-10-28). */
					btnMerge(&s_btn, m, newest & FPSVITA_SYSCON_GAME_MASK,
					         kb.changed_at != s_btn_changed_last, (uint32_t)pnow,
					         4u * g_vblank_us + 10000u, &add, &clear);
					s_btn_changed_last = kb.changed_at;
					s_btn_total++;
					if (add & ~newest) {
						/* A press the sample does not have yet: it is newer than the sample. */
						uint64_t age_k = kb.now - kb.changed_at;
						uint64_t stamp = age_k < 1000000ull ? pnow - age_k : 0;
						s_btn_diff++;
						if (stamp > ts) {
							int32_t lead = clampAge((int64_t)(stamp - ts));
							s_btn_lead_ema += (lead - s_btn_lead_ema) >> 3;
							ts = stamp;
						}
					}
					for (i = 0; i < r && i < 64; i++) {
						if (neg) pad[i].buttons = (pad[i].buttons & ~add) | clear;
						else     pad[i].buttons = (pad[i].buttons | add) & ~clear;
					}
				}
				/* Early sticks: own toggle (sticks=), the same kernel-clock offset. */
				if (s_sticksOn)
					stkDeliver(pad, r, kb.now - pnow, kb.now);
			}
		}
	}
	int32_t a = clampAge((int64_t)(sceKernelGetProcessTimeWide() - ts));
	s_ts_out_ema += (a - s_ts_out_ema) >> 4;
	if (ts > s_out_stamp) s_out_stamp = ts;
}

/* "read=peek" per title: a blocking Read becomes the matching Peek. The
 * helper's own imports are the real functions; the game's Read import is
 * only hooked, so calling them here does not recurse. */
static int peekCall(int readIdx, int port, SceCtrlData *pad, int count)
{
	switch (readIdx) {
	case HOOK_CTRL_READ:     return sceCtrlPeekBufferPositive(port, pad, count);
	case HOOK_CTRL_READ2:    return sceCtrlPeekBufferPositive2(port, pad, count);
	case HOOK_CTRL_READ_EXT: return sceCtrlPeekBufferPositiveExt(port, pad, count);
	case HOOK_CTRL_READ_EXT2:return sceCtrlPeekBufferPositiveExt2(port, pad, count);
	case HOOK_CTRL_READ_NEG: return sceCtrlPeekBufferNegative(port, pad, count);
	case HOOK_CTRL_READ_NEG2:return sceCtrlPeekBufferNegative2(port, pad, count);
	default:                 return -1;
	}
}

#define CTRL_HOOK(name, idx) \
	static int name##_patched(int port, SceCtrlData *pad, int count) \
	{ \
		noteCtrlRead(); \
		int r = TAI_CONTINUE(int, s_hookRef[idx], port, pad, count); \
		int32_t age = noteCtrlSample(pad, r); \
		r = freshPoll(s_hookRef[idx], -1, port, pad, count, r, age); \
		noteCtrlOut(pad, r, idx >= HOOK_CTRL_PEEK_NEG); \
		return r; \
	}
/* A second blocking Read returns only the samples after the first, so with
 * count > 1 a re-poll would drop the first batch: fresh poll is skipped there.
 * Peek returns the newest samples each time, so read=peek re-polls at any count. */
#define CTRL_READ_HOOK(name, idx) \
	static int name##_patched(int port, SceCtrlData *pad, int count) \
	{ \
		noteCtrlRead(); \
		int r = s_readPeek ? peekCall(idx, port, pad, count) \
		                   : TAI_CONTINUE(int, s_hookRef[idx], port, pad, count); \
		int32_t age = noteCtrlSample(pad, r); \
		if (s_readPeek || count == 1) \
			r = freshPoll(s_hookRef[idx], s_readPeek ? idx : -1, port, pad, count, r, age); \
		noteCtrlOut(pad, r, idx >= HOOK_CTRL_PEEK_NEG); \
		return r; \
	}
CTRL_HOOK(sceCtrlPeekBufferPositive,     HOOK_CTRL_PEEK)
CTRL_READ_HOOK(sceCtrlReadBufferPositive,     HOOK_CTRL_READ)
CTRL_HOOK(sceCtrlPeekBufferPositive2,    HOOK_CTRL_PEEK2)
CTRL_READ_HOOK(sceCtrlReadBufferPositive2,    HOOK_CTRL_READ2)
CTRL_HOOK(sceCtrlPeekBufferPositiveExt,  HOOK_CTRL_PEEK_EXT)
CTRL_READ_HOOK(sceCtrlReadBufferPositiveExt,  HOOK_CTRL_READ_EXT)
CTRL_HOOK(sceCtrlPeekBufferPositiveExt2, HOOK_CTRL_PEEK_EXT2)
CTRL_READ_HOOK(sceCtrlReadBufferPositiveExt2, HOOK_CTRL_READ_EXT2)
CTRL_HOOK(sceCtrlPeekBufferNegative,     HOOK_CTRL_PEEK_NEG)
CTRL_READ_HOOK(sceCtrlReadBufferNegative,     HOOK_CTRL_READ_NEG)
CTRL_HOOK(sceCtrlPeekBufferNegative2,    HOOK_CTRL_PEEK_NEG2)
CTRL_READ_HOOK(sceCtrlReadBufferNegative2,    HOOK_CTRL_READ_NEG2)

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
	uint32_t target = g_vblank_us * (uint32_t)cur;
	uint32_t work = s_work_prev_max > s_work_win_max ? s_work_prev_max : s_work_win_max;
	if (work > s_work_ema * 3 + 2000)   /* one wild sample must not stall the ramp */
		work = s_work_ema * 3 + 2000;
	s_jit_slack = 0;
	if (s_jit_backoff > 0) { s_jit_backoff--; return; }
	if (s_jit_ramp < FPSVITA_JIT_RAMP) { s_jit_ramp++; return; }
	uint32_t need = work + FPSVITA_JIT_MARGIN_US + FPSVITA_FRESH_WAIT_US;
	uint32_t k = target > need ? (target - need) / g_vblank_us : 0;
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
		               ? (s_flag_ema - FPSVITA_JIT_MARGIN_US) / g_vblank_us : 0;
		if (k > avail) k = avail;
	} else {
		if (++s_jit_k_frames >= FPSVITA_JIT_SETTLE
		    && s_flag_ema + s_jit_k * g_vblank_us
		       > s_jit_flag0 + FPSVITA_JIT_MARGIN_US + g_vblank_us / 4) {
			s_jit_hurt++; s_jit_backoff = FPSVITA_JIT_BACKOFF; s_jit_ramp = 0; s_jit_k = 0;
			return;
		}
		if (k > s_jit_k) k = s_jit_k;   /* never grow without relearning */
	}
	if (k && !s_jit_k) s_jit_k_frames = 0;
	s_jit_k = k;
	uint32_t slack = k * g_vblank_us;
	s_jit_slack = slack;
	if (!slack)
		return;
	sceKernelDelayThread(slack - FPSVITA_JIT_SPIN_US);
	while (sceKernelGetProcessTimeLow() - t_ret < slack)
		;
	s_jit_applied++;
}

/* The submit path, shared by sceGxmDisplayQueueAddEntry and the PowerVR
 * stack's PVRSRVSwapToDCBuffer (both block when the display has a frame in
 * hand, both run on the thread that renders). Before the call: the entry
 * ring and the CPU work. After it: the state read, the frame period, and
 * whichever of the inject wait, the Antilag sleep or the Limiter applies. */
static void submitPre(const void *fb, uint32_t tid, uint32_t t_in)
{
	s_ae_last_in = t_in;
	{
		unsigned h = s_entry_head;
		if (h - s_entry_tail >= FPSVITA_ENTRY_RING) s_entry_tail = h - FPSVITA_ENTRY_RING + 1;
		/* The SDK samples, vitaGL and vita2d put the framebuffer address first
		 * in the callback data; where a title does, the flip is tied exactly. */
		s_entries[h % FPSVITA_ENTRY_RING].fb = fb;
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
				uint32_t budget = g_vblank_us * (uint32_t)tgt;
				if (work > budget && work - budget <= 2000) s_miss_near++;
			}
			emaStep(&s_work_ema, work);
			if (work > s_work_win_max) s_work_win_max = work;
			if (++s_work_win_n >= 32) {
				s_work_prev_max = s_work_win_max; s_work_win_max = 0; s_work_win_n = 0;
			}
		}
	}

}

static void submitPost(uint32_t tid, uint32_t t_ret)
{
	FpsvitaState st;
	st.interval = FPSVITA_INTERVAL_OFF; st.autodetect = FPSVITA_AUTODETECT_ON; st.inject = 0;
	st.adv_flags = 0; st.softcap = 0;
	fpsvitaGetState(&st, sizeof(st));
	int32_t cur = st.interval;
	s_jitEnabled = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
	g_vblank_us = st.panel_hz > 0 ? 1000000u / (uint32_t)st.panel_hz : FPSVITA_VBLANK_US;
	if (s_softcap != st.softcap)
		s_lim_next = 0;
	s_softcap = st.softcap;
	if (s_ae_last_raw && tid == s_ae_tid) {
		uint32_t d = t_ret - s_ae_last_raw;      /* full frame, sleep included */
		if (d < 2000000) {
			emaStep(&s_ae_period_ema, d);
			if (cur > 0 && d > g_vblank_us * (uint32_t)cur * 5u / 4u && s_jit_slack) {
				s_jit_missed++; s_jit_backoff = FPSVITA_JIT_BACKOFF; s_jit_ramp = 0; s_jit_k = 0;
			}
		}
	}
	s_ae_last_raw = t_ret;
	s_ae_tid = tid;


	int late = 0;

	int slept = 0, limited = 0;
	if (injectWanted(&st) && !late) {
		/* Pace the producer here; the SetFrameBuf path stands down. */
		sceDisplayWaitVblankStartMulti((unsigned int)cur);
		t_ret = sceKernelGetProcessTimeLow();
		s_vbl_ret = t_ret;
	} else if (cur > 0 && s_jitEnabled && s_ctrl_windows
	           && s_ctrl_after_pct >= FPSVITA_CTRL_AFTER_PCT) {
		jitSleep(t_ret, cur);
		t_ret = sceKernelGetProcessTimeLow();   /* the sleep is not work */
		slept = 1;
	} else if (cur == FPSVITA_INTERVAL_UNCAPPED && s_softcap > 0) {
		/* Software cap without vsync: the waits return at once (Uncapped),
		 * and the producer is held here until the next slot of a fixed
		 * schedule, one period after the previous slot, so the average rate
		 * is the cap exactly. A frame late by less than a period gets a
		 * shorter next slot (the schedule catches up); a frame later than
		 * that restarts the schedule instead of bursting. The game's delta
		 * time still reads real time, so nothing desyncs; the panel just
		 * latches whatever is newest at each vblank. */
		uint32_t period = 1000000u / (uint32_t)s_softcap;
		int32_t late = (int32_t)(t_ret - s_lim_next);
		if (!s_lim_next || late > (int32_t)period || late < -(int32_t)period) {
			s_lim_next = t_ret;
		} else if (late < 0) {
			uint32_t remain = (uint32_t)-late;
			if (remain > FPSVITA_JIT_SPIN_US)
				sceKernelDelayThread(remain - FPSVITA_JIT_SPIN_US);
			while ((int32_t)(sceKernelGetProcessTimeLow() - s_lim_next) < 0)
				;
			t_ret = sceKernelGetProcessTimeLow();
			s_softcap_applied++;
		}
		s_lim_next += period;
		limited = 1;
	}
	/* Off the paths that own them, the sleep and the schedule start over
	 * when they come back: no stale vblank count or slot. */
	if (!slept) { s_jit_k = 0; s_jit_ramp = 0; s_jit_slack = 0; }
	if (!limited) s_lim_next = 0;
	s_ae_last_ret = t_ret;
}

static int sceGxmDisplayQueueAddEntry_patched(void *oldBuffer, void *newBuffer,
                                              const void *callbackData)
{
	uint32_t tid = (uint32_t)sceKernelGetThreadId();
	/* The SDK samples, vitaGL and vita2d put the framebuffer address first
	 * in the callback data; where a title does, the flip is tied exactly. */
	const void *fb = (callbackData && s_cb_data_size >= sizeof(void *))
	               ? *(const void *const *)callbackData : NULL;
	submitPre(fb, tid, sceKernelGetProcessTimeLow());
	int r = TAI_CONTINUE(int, s_hookRef[HOOK_GXM_ADD_ENTRY], oldBuffer, newBuffer,
	                     callbackData);
	submitPost(tid, sceKernelGetProcessTimeLow());
	return r;
}

/* PVRSRVSwapToDCBuffer(hDevice, hBuffer, ui32ClipRectCount, psClipRect,
 * ui32SwapInterval, hPrivateTag), imported by libpvrPSP2_WSEGL from
 * libgpu_es4_ext (GrapheneCt/PVR_PSP2, bridged_pvr_dc_glue.c). hBuffer is a
 * PVRSRV_CLIENT_MEM_INFO whose first word is the framebuffer's CPU address,
 * so the flip ties exactly. The display class flips on its own thread
 * (SetFrameBuf then a vblank wait, both hooked) and the swap blocks until
 * the previous flip is done: one frame in flight by construction, which is
 * what Lowlat means, so the title reads as having it itself. */
static int pvrSwapToDCBuffer_patched(void *hDevice, void *hBuffer, uint32_t nRects, void *rects,
                                     uint32_t swapInterval, void *tag)
{
	uint32_t tid = (uint32_t)sceKernelGetThreadId();
	const void *fb = hBuffer ? *(const void *const *)hBuffer : NULL;
	s_lowLatencyNative = 1;
	s_gxmInitSeen = 1;
	submitPre(fb, tid, sceKernelGetProcessTimeLow());
	int r = TAI_CONTINUE(int, s_hookRef[HOOK_PVR_SWAP], hDevice, hBuffer, nRects, rects,
	                     swapInterval, tag);
	submitPost(tid, sceKernelGetProcessTimeLow());
	return r;
}


/* A submit hook landed: AddEntry or the PowerVR swap. */
static int submitHooked(void)
{
	return s_hookId[HOOK_GXM_ADD_ENTRY] >= 0 || s_hookId[HOOK_PVR_SWAP] >= 0;
}

/* Work for a title that presents by flipping directly (no AddEntry hook): the
 * frame's own CPU time is everything between the return of its pacing wait and
 * the flip that ends it. AddEntry titles keep the stricter same-thread measure,
 * so this only fills in where that one has nothing. Called at flip-hook entry,
 * before any wait this hook injects, and only when the last wait landed inside
 * this frame (a stale stamp means another phase or another thread). */
static void noteFlipWork(uint32_t t)
{
	uint32_t vb;
	if (submitHooked())
		return;
	vb = s_vbl_ret;
	if (!vb || (int32_t)(vb - s_present_last) < 0)
		return;
	uint32_t w = t - vb;
	if (w >= 100000)   /* a load or a pause, not a frame */
		return;
	if (s_flip_work_ema && w > s_flip_work_ema * 3 + 2000)   /* one wild sample must not stall the ramp */
		w = s_flip_work_ema * 3 + 2000;
	emaStep(&s_flip_work_ema, w);
}

/* 1% low over the period ring: the mean of the slowest percent of the
 * presents in it, as fps times 10. Needs 200 presents (two slowest) and uses
 * five once the ring is full. One pass with an insertion into a 5-slot top
 * list, a few microseconds every push. */
static uint32_t low1FpsX10(uint32_t frames)
{
	uint32_t n = frames - s_period_base;
	if (n > FPSVITA_PERIOD_RING)
		n = FPSVITA_PERIOD_RING;
	if (n < 200)
		return 0;
	uint32_t k = n / 100, top[5] = {0, 0, 0, 0, 0}, i, j, sum = 0;
	for (i = 0; i < n; i++) {
		uint32_t v = s_period_ring[(frames - i) % FPSVITA_PERIOD_RING];
		if (v <= top[k - 1])
			continue;
		for (j = k - 1; j > 0 && top[j - 1] < v; j--)
			top[j] = top[j - 1];
		top[j] = v;
	}
	for (i = 0; i < k; i++)
		sum += top[i];
	return sum ? (uint32_t)(10000000ull * k / sum) : 0;
}

/* Live numbers for the shell's overlay, pushed while it is on. */
static void pushStats(uint32_t now, const FpsvitaState *stIn)
{
	FpsvitaStats st;
	uint32_t frames = g_frame_count;
	s_stats_last_push = now;
	st.fps_x10      = s_present_period_ema ? 10000000u / s_present_period_ema : 0;
	st.frame_us     = s_present_period_ema;
	st.work_us      = s_work_ema ? s_work_ema : s_flip_work_ema;
	st.queue_us     = s_flag_ema;
	/* INPUT: the frame's own sample age at its flip (AddEntry tie). Without
	 * AddEntry (the PowerVR GLES stack flips through SetFrameBuf alone) the
	 * newest delivered sample's age at the flip is the same thing on a
	 * direct flip, so it stands in. */
	st.input_us     = (uint32_t)(s_fscan_ema > 0 ? s_fscan_ema
	                  : !submitHooked() && s_scan_ema > 0 ? s_scan_ema : 0);
	st.ctrl_pct     = s_ctrl_after_pct;
	st.jit_slack_us = s_jit_slack;
	st.softcap      = stIn->softcap;
	st.flags        = (injectWanted(stIn) ? FPSVITA_STATF_INJECT : 0)
	                | (s_lowLatencyApplied ? FPSVITA_STATF_LOWLATENCY : 0)
	                | (s_lowLatencyNative ? FPSVITA_STATF_LOWLAT_NATIVE : 0)
	                | ((stIn->adv_flags & FPSVITA_ADV_JIT) ? FPSVITA_STATF_JIT : 0)
	                | (submitHooked() ? FPSVITA_STATF_ADDENTRY : 0)
	                | (s_tearWanted && s_tearOk ? FPSVITA_STATF_TEAR : 0)
	                | (s_gxmInitSeen ? FPSVITA_STATF_GXM_INIT : 0)
	                | (!s_work_ema && s_flip_work_ema ? FPSVITA_STATF_FLIPWORK : 0)
	                | (s_btn_status == FPSVITA_BTN_NOSYSCON ? FPSVITA_STATF_BTN_NOSYSCON : 0)
	                | ((s_stk.trust[0] | s_stk.trust[1] | s_stk.trust[2] | s_stk.trust[3])
	                   ? FPSVITA_STATF_STICKS : 0);
	st.jit_missed   = s_jit_missed;
	st.sample_age_us = (uint32_t)(s_ts_out_ema > 0 ? s_ts_out_ema : 0);
	st.presents     = frames;
	st.miss         = s_miss_dbl;
	st.low1_fps_x10 = low1FpsX10(frames);
	st.btn_lead_us  = (uint32_t)(s_btn_lead_ema > 0 ? s_btn_lead_ema : 0);
	st.btn_presses  = s_btn.presses;
	st.btn_early    = s_btn.early;
	st.seq          = ++s_stats_seq;
	fpsvitaSetStats(&st);
}

static int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam,
                                          SceDisplaySetBufSync sync)
{
	noteFlipWork(sceKernelGetProcessTimeLow());
	/* A submit path may still be missing (the PowerVR stack's WSEGL loads
	 * after the first flips): look again at a few frames in. */
	if (!submitHooked() && (g_frame_count == 120 || g_frame_count == 600))
		installHooksOnLoadedModules();
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
	}
	else {
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
		 * framebuffer. Everything older is then discarded (flipped or skipped);
		 * the tied entry (index i - 1) is the one measured below. Setting the
		 * tail to i skipped it, so a title tied on every flip (The Swapper)
		 * never got an INPUT or QUEUE reading. */
		for (i = head; i != tail && pParam; i--) {
			unsigned t = (i - 1) % FPSVITA_ENTRY_RING;
			if (s_entries[t].fb && s_entries[t].fb == pParam->base) {
				s_entry_tail = i - 1; tail = i - 1; s_entry_tied++; break;
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

	/* Atomic: some titles present from more than one thread, and a plain
	 * read-modify-write silently loses increments. */
	uint32_t frames = __sync_add_and_fetch((uint32_t *)&g_frame_count, 1);

	/* One syscall per present for everything this hook needs. */
	FpsvitaState st;
	st.interval = FPSVITA_INTERVAL_OFF; st.hud_mode = FPSVITA_HUD_OFF;
	st.autodetect = FPSVITA_AUTODETECT_ON; st.inject = 0; st.adv_flags = 0; st.softcap = 0;
	fpsvitaGetState(&st, sizeof(st));
	s_jitEnabled = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
	g_vblank_us = st.panel_hz > 0 ? 1000000u / (uint32_t)st.panel_hz : FPSVITA_VBLANK_US;
	int32_t cur = st.interval;
	s_tearWanted = (cur == FPSVITA_INTERVAL_UNCAPPED) && (st.adv_flags & FPSVITA_ADV_TEAR) != 0;

	/* Present period, the rate the game actually achieves. A gap over half a
	 * second is a load or a pause, not a frame. */
	uint32_t now = sceKernelGetProcessTimeLow();
	if (s_present_last) {
		uint32_t d = now - s_present_last;
		if (d < 500000) {
			emaStep(&s_present_period_ema, d);
			/* The 1% low is over one regime: a limit, Limiter or panel change
			 * restarts the ring, else it would report the old rate for 512 frames. */
			uint32_t regime = ((uint32_t)cur << 24) ^ ((uint32_t)st.softcap << 8) ^ (uint32_t)st.panel_hz;
			if (regime != s_period_regime) {
				s_period_regime = regime;
				s_period_base = frames;
			}
			s_period_ring[frames % FPSVITA_PERIOD_RING] = d;
			/* A present period of about twice the target: a frame missed its
			 * vblank (GPU included; the CPU-only near count is in AddEntry).
			 * The target is the cap, else the Limiter's period under Uncapped,
			 * else the game's own rate under Default. */
			uint32_t budget = 0;
			if (cur > 0)
				budget = g_vblank_us * (uint32_t)cur;
			else if (cur == FPSVITA_INTERVAL_UNCAPPED && st.softcap > 0)
				budget = 1000000u / (uint32_t)st.softcap;
			else if (cur == FPSVITA_INTERVAL_OFF && st.native_interval > 0)
				budget = g_vblank_us * (uint32_t)st.native_interval;
			if (budget) {
				if (d > budget * 3 / 2 && d < budget * 5 / 2) s_miss_dbl++;
			}
		}
	}
	s_present_last = now;

	int detected = autodetectTick(frames, cur, s_present_period_ema);

	if (st.hud_mode != FPSVITA_HUD_OFF && now - s_stats_last_push >= FPSVITA_STATS_PUSH_US)
		pushStats(now, &st);

	/* Rate reports (config.txt `log=1`, off by default: this is a flash
	 * write from the thread that flips, a few ms every 50 to 100 s, which a
	 * user should not pay for unasked): one line per 3000 presents, each
	 * measuring the preceding 600. The first lands at frame 900 so a sweep
	 * sees it inside its wait. tools/ingest_playlog.py collects them. */
	if (s_rateLog) {
		uint32_t ph = frames % 3000u;
		if (ph == 300 || ph == 2400) {
			g_rate_t0 = sceKernelGetProcessTimeLow();
		} else if (ph == 900 || ph == 0) {
			uint32_t dt = sceKernelGetProcessTimeLow() - g_rate_t0;
			/* sceClibSnprintf does not terminate on truncation (garbage after
			 * over-long lines, 2026-10-17): size the buffer for the full line
			 * and force the terminator and newline anyway. */
			char line[640];
			int len = sceClibSnprintf(line, sizeof(line) - 2,
				"helper: rate frames=%u flips/s=%u cur=%d inject=%d since_vblank=%u ae_period=%u work=%u work_max=%u ctrl_after_pct=%u jit=%d slack=%u applied=%u missed=%u present_lag=%u softcap=%d softcap_applied=%u ll=%d"
				" ts=%d/%d/%d ts_n=%u rd_ph=%d ts_ph=%d fresh=%u/%u/%u out=%d scan=%d fscan=%d flag=%u tied=%u unmatched=%u hurt=%u miss=%u near=%u low1=%u imm=%u/%u/0x%08x",
				frames, dt ? (unsigned)((600ull * 1000000ull) / dt) : 0, (int)cur,
				g_inject_active, frames - g_last_vblank_frame, s_ae_period_ema, s_work_ema,
				s_work_prev_max > s_work_win_max ? s_work_prev_max : s_work_win_max,
				s_ctrl_after_pct, s_jitEnabled,
				s_jit_slack, s_jit_applied, s_jit_missed, s_present_lag_ema, (int)s_softcap, s_softcap_applied,
				s_lowLatencyApplied || s_lowLatencyNative,
				s_ts_min, s_ts_ema, s_ts_max, s_ts_n, s_rd_ph_ema, s_ts_ph_ema,
				s_fresh_hit, s_fresh_miss, s_fresh_spin_ema, s_ts_out_ema, s_scan_ema,
				s_fscan_ema, s_flag_ema, s_entry_tied, s_entry_unmatched, s_jit_hurt,
				s_miss_dbl, s_miss_near, low1FpsX10(frames), s_imm_calls, s_imm_err, s_imm_code);
			if (len < 0 || len > (int)sizeof(line) - 3) len = (int)sizeof(line) - 3;
			if (len == 0 || line[len - 1] != '\n') { line[len++] = '\n'; }
			line[len] = '\0';
			helperLog(line);
			s_ts_n = 0;   /* min/max restart with the next sample */
		}
	}

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

static int gxmInitialize(int hook, const char *name, const SceGxmInitializeParams *params)
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
	 *
	 * sceGxmVshInitialize (SceGxmInternalForVsh) takes the same params; vitaGL,
	 * statically linked into every port's eboot, imports it instead.
	 */
	{
		char line[96];
		sceClibSnprintf(line, sizeof(line), "helper: %s pending=%u lowlat=%d\n", name,
		                params ? (unsigned)params->displayQueueMaxPendingCount : 0xFFFFFFFFu, s_lowLatency);
		helperLog(line);
	}
	s_gxmInitSeen = 1;
	if (params)
		s_cb_data_size = params->displayQueueCallbackDataSize;
	if (params && params->displayQueueMaxPendingCount <= 1)
		s_lowLatencyNative = 1;   /* the title's own choice, nothing to clamp */
	if (s_lowLatency && params && params->displayQueueMaxPendingCount > 1) {
		SceGxmInitializeParams p = *params;
		p.displayQueueMaxPendingCount = 1;
		s_lowLatencyApplied = 1;
		return TAI_CONTINUE(int, s_hookRef[hook], &p);
	}

	return TAI_CONTINUE(int, s_hookRef[hook], params);
}

static int sceGxmInitialize_patched(const SceGxmInitializeParams *params)
{
	return gxmInitialize(HOOK_GXM_INITIALIZE, "gxm init", params);
}

static int sceGxmVshInitialize_patched(const SceGxmInitializeParams *params)
{
	return gxmInitialize(HOOK_GXM_VSH_INITIALIZE, "gxm vsh init", params);
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
	    || (linelen > 7  && sceClibStrncmp(p, "hudpos=",  7) == 0)
	    || (linelen > 4  && sceClibStrncmp(p, "log=",     4) == 0)
	    || (linelen > 7  && sceClibStrncmp(p, "syscon=",  7) == 0)
	    || (linelen > 7  && sceClibStrncmp(p, "sticks=",  7) == 0)
	    || (linelen > 13 && sceClibStrncmp(p, "experimental=", 13) == 0)
	    || (linelen > 11 && sceClibStrncmp(p, "panelmodes=", 11) == 0)
	    || (linelen > 10 && sceClibStrncmp(p, "panelauto=", 10) == 0)
	    || (linelen > 5  && sceClibStrncmp(p, "tips=",    5) == 0);
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

/* The main file, or the last complete copy when a save was interrupted. */
static int readFileIntoBak(const char *path, char *buf, int size)
{
	int bytes = readFileInto(path, buf, size);
	if (bytes > 0)
		return bytes;
	char bak[64];
	sceClibSnprintf(bak, sizeof(bak), "%s.bak", path);
	return readFileInto(bak, buf, size);
}


/* antilag=0 is off; 1, and the "auto" earlier versions wrote, are on. */
static int antilagValue(const char *v)
{
	return *v == 'a' || (int)sceClibStrtoll(v, NULL, 10) ? 1 : 0;
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
					cfg->jit = antilagValue(p + 8);
				else if (linelen > 8 && sceClibStrncmp(p, "tearing=", 8) == 0)
					cfg->tear = (int)sceClibStrtoll(p + 8, NULL, 10) ? 1 : 0;
				else if (linelen > 4 && sceClibStrncmp(p, "log=", 4) == 0)
					cfg->log = (int)sceClibStrtoll(p + 4, NULL, 10) ? 1 : 0;
				else if (linelen > 7 && sceClibStrncmp(p, "syscon=", 7) == 0)
					cfg->syscon = (int)sceClibStrtoll(p + 7, NULL, 10) ? 1 : 0;
				else if (linelen > 7 && sceClibStrncmp(p, "sticks=", 7) == 0)
					cfg->sticks = (int)sceClibStrtoll(p + 7, NULL, 10) ? 1 : 0;
				else if (linelen > 13 && sceClibStrncmp(p, "experimental=", 13) == 0)
					cfg->panelmodes = (int)sceClibStrtoll(p + 13, NULL, 10) ? 1 : 0;
				else if (linelen > 11 && sceClibStrncmp(p, "panelmodes=", 11) == 0)
					cfg->panelmodes = (int)sceClibStrtoll(p + 11, NULL, 10) ? 1 : 0;
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
						cfg->pg_jit = antilagValue(q + 9);
					else if (eol - q > 9 && sceClibStrncmp(q, ",limiter=", 9) == 0)
						cfg->pg_sc = (int32_t)sceClibStrtoll(q + 9, NULL, 10);
					else if (eol - q > 9 && sceClibStrncmp(q, ",tearing=", 9) == 0)
						cfg->pg_tear = sceClibStrtoll(q + 9, NULL, 10) ? 1 : 0;
					else if (eol - q > 7 && sceClibStrncmp(q, ",vsync=", 7) == 0) {
						const char *v = q + 7;
						cfg->pg_vsync = *v == 'a' ? 0 : (sceClibStrtoll(v, NULL, 10) ? 2 : 1);
					}
					else if (eol - q > 9 && sceClibStrncmp(q, ",read=peek", 10) == 0)
						cfg->pg_read = 1;
					else if (eol - q > 7 && sceClibStrncmp(q, ",panel=", 7) == 0)
						cfg->pg_panel = (int32_t)sceClibStrtoll(q + 7, NULL, 10);
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
	cfg->jit = 1;          /* the shell's fresh-install default: on */
	cfg->tear = 0;
	cfg->pg_ll = cfg->pg_jit = cfg->pg_sc = cfg->pg_tear = cfg->pg_vsync = -1;
	cfg->pg_read = 0;
	cfg->pg_panel = 0;
	cfg->log = 0;
	cfg->syscon = 1;
	cfg->sticks = 1;
	cfg->panelmodes = 0;

	/* One buffer size shared with the shell (FPSVITA_STATE_BUF), so the
	 * writer can never produce a file this reader cannot fully see. Static
	 * because module_start runs on a thread whose stack is not ours to size;
	 * it is the helper's only large object. */
	static char buf[FPSVITA_STATE_BUF];
	int n = readFileIntoBak(FPSVITA_CONFIG_FILE, buf, sizeof(buf));
	if (n > 0)
		parseConfigLines(buf, n, titleid, cfg, 1);   /* the globals */
	n = readFileIntoBak(FPSVITA_STATE_FILE, buf, sizeof(buf));
	if (n > 0)
		parseConfigLines(buf, n, titleid, cfg, 0);   /* this title's line */
	if (!cfg->panelmodes)
		cfg->pg_panel = 0;   /* panel refresh is opt-in: panelmodes=1 */
}

/* The compatibility list's known-bad input titles get read=peek without a
 * state.txt edit. The key remains for titles the list does not know. */
static unsigned compatFlagsFor(const char *tid)
{
	if (!tid || !tid[0])
		return 0;
	for (unsigned i = 0; i < fpsvita_compat_count; i++) {
		const char *id = fpsvita_compat[i].id;
		int j = 0;
		while (j < 9 && id[j] && id[j] == tid[j])
			j++;
		if (j == 9 && tid[j] == '\0')
			return fpsvita_compat[i].flags;
	}
	return 0;
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

/* Exp bit 32 source probe: the dev-only handlers are installed with the rest
 * of the hook table when one of the probe/dilation bits is set; the counters
 * surface in the rate window as a separate `helper: probe` line. */

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
	{ 0xA08CA60D, (const void *)sceDisplayGetRefreshRate_patched },
	{ 0xA04F5FAC, (const void *)sceGxmVshInitialize_patched },
	{ 0xA7D1D94E, (const void *)pvrSwapToDCBuffer_patched },
};

static char    s_titleid[32];
static int32_t s_pergame = FPSVITA_NO_PERGAME;

/* One line at game start: the VitaGrafix section for this title (so a
 * resolution or FPS patch is visible next to the measurements) and the
 * clocks another plugin has set. The config is only a declaration: a
 * section with ENABLED=1 still depends on VitaGrafix being loaded and
 * matching the build, but it is the best signal available from user mode. */
static void copyConfigValue(char *dst, int cap, const char *p, const char *eol)
{
	int n = (int)(eol - p);
	if (n < 0) n = 0;
	if (n >= cap) n = cap - 1;
	sceClibMemcpy(dst, p, n);
	dst[n] = '\0';
}

/* One line of VitaGrafix config. Returns 1 once the title's section has been
 * read and left, so the caller can stop. `*in` tracks being inside it. */
static int vgLine(const char *p, int len, const char *titleid, int tidlen,
                  int *in, int *enabled, int *fps, char *ib, char *fb)
{
	while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' '))
		len--;
	if (len > 2 && p[0] == '[' && p[len - 1] == ']') {
		int was = *in;
		*in = (len - 2 == tidlen && sceClibStrncmp(p + 1, titleid, tidlen) == 0);
		return was && !*in;   /* left our section: done */
	}
	if (!*in)
		return 0;
	if (len > 8 && sceClibStrncmp(p, "ENABLED=", 8) == 0)
		*enabled = (int)sceClibStrtoll(p + 8, NULL, 10);
	else if (len > 4 && sceClibStrncmp(p, "FPS=", 4) == 0)
		*fps = (p[4] == 'O' || p[4] == 'o') ? -1 : (int)sceClibStrtoll(p + 4, NULL, 10);
	else if (len > 3 && sceClibStrncmp(p, "IB=", 3) == 0)
		copyConfigValue(ib, 16, p + 3, p + len);
	else if (len > 3 && sceClibStrncmp(p, "FB=", 3) == 0)
		copyConfigValue(fb, 16, p + 3, p + len);
	return 0;
}

/* Stream the VitaGrafix config a chunk at a time rather than loading it whole:
 * the file can be large (hundreds of titles) but only one short section
 * matters, so a small chunk and a short line buffer replace a 16 KB static. */
static void readVitaGrafixEnv(const char *titleid, int *enabled, int *fps,
                              char *ib, char *fb)
{
	static const char *const paths[2] = {
		"ux0:data/VitaGrafix/config.txt",
		"ur0:data/VitaGrafix/config.txt",
	};
	char chunk[256], line[96];
	int li = 0, in = 0, done = 0, over = 0, i, tidlen = 0;
	SceUID fd = -1;

	while (titleid[tidlen] && tidlen < 31)
		tidlen++;
	*enabled = -1;
	*fps = -1;
	ib[0] = fb[0] = '\0';

	for (i = 0; i < 2 && fd < 0; i++)
		fd = sceIoOpen(paths[i], SCE_O_RDONLY, 0);
	if (fd < 0)
		return;

	for (;;) {
		int n = sceIoRead(fd, chunk, sizeof(chunk)), c;
		if (n <= 0)
			break;
		for (c = 0; c < n && !done; c++) {
			char ch = chunk[c];
			if (ch == '\n') {
				if (!over)
					done = vgLine(line, li, titleid, tidlen, &in, enabled, fps, ib, fb);
				li = 0;
				over = 0;
			} else if (li < (int)sizeof(line) - 1) {
				line[li++] = ch;    /* a key we care about fits; longer lines are truncated */
			} else {
				over = 1;
			}
		}
		if (done)
			break;
	}
	if (!done && li > 0 && !over)
		vgLine(line, li, titleid, tidlen, &in, enabled, fps, ib, fb);
	sceIoClose(fd);
}

static void logEnvironment(void)
{
	char ib[16], fb[16], line[160];
	int vg_enabled, vg_fps;

	readVitaGrafixEnv(s_titleid, &vg_enabled, &vg_fps, ib, fb);
	sceClibSnprintf(line, sizeof(line),
	                "helper: env vg=%d ib=%s fb=%s fps=%d arm=%d gpu=%d bus=%d\n",
	                vg_enabled, ib[0] ? ib : "-", fb[0] ? fb : "-", vg_fps,
	                scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
	                scePowerGetBusClockFrequency());
	helperLog(line);
}

/* Try the display hooks on one module's imports. Each hook is installed at
 * most once (the patched functions carry a single ref per NID), so the first
 * module that imports a function keeps it. Returns the number added.
 *
 * Imports only, never SceGxm's exports: taiHEN writes a user export hook into
 * the library's single mapping above 0xE0000000, shared by every process
 * (patches.c, "TODO: Take care of SHARED_PID"), so each other process then
 * jumps to a handler address that exists only in this one. An AddEntry
 * export hook killed SceShell on its next frame (2026-10-27). */
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

/* Try the hooks on every module loaded into this process. The loader hooks
 * only see what the main module loads itself; a module loaded by another
 * module (libpvrPSP2_WSEGL, loaded by libIMGEGL) is only reachable this way.
 * Modules mapped at or above 0xE0000000 are the system's shared libraries:
 * their import stubs live in the one mapping every process shares, and a
 * hook there is the SceGxm export trap again (the first walk, 2026-10-28,
 * put eleven hooks into SceCommonDialog and friends). Skipped. Returns the
 * number of hooks added. */
static int installHooksOnLoadedModules(void)
{
	static SceUID mods[128];
	static SceKernelModuleInfo info;
	SceSize num = sizeof(mods) / sizeof(mods[0]);
	int added = 0;
	if (sceKernelGetModuleList(0xFF, mods, &num) < 0)
		return 0;
	for (SceSize i = 0; i < num; i++) {
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(mods[i], &info) < 0)
			continue;
		if ((uint32_t)info.segments[0].vaddr >= 0xE0000000u)
			continue;   /* shared system library */
		/* Only the app's own modules (app0: or ux0:app/<id>/): not the
		 * system's per-process ones, not other plugins, and above all not
		 * this module, whose own import stubs carry the injected waits and
		 * the fresh poll (the first walk hooked them and halved the rate). */
		if (sceClibStrncmp(info.path, "app0:", 5) != 0 && sceClibStrncmp(info.path, "ux0:app/", 8) != 0)
			continue;
		added += installDisplayHooks(info.module_name);
	}
	if (added > 0) {
		char line[80];
		sceClibSnprintf(line, sizeof(line), "helper: modwalk hooks+=%d mask=0x%04x\n", added, hookMask());
		helperLog(line);
	}
	return added;
}

/* Hooks that can pace or observe presentation (display waits, SetFrameBuf,
 * AddEntry). The GXM init hooks do not count: a main module that only
 * initialises GXM and renders from a PRX must keep the late-module search
 * going (ownership ends it). Controller and touch hooks alone are instrumentation: a main module
 * with its input in the executable and its renderer in a PRX loaded later
 * must still get the loader hooks, and must not take the kernel state until
 * the display imports are found. */
static int pacingHooked(void)
{
	int i;
	for (i = 0; i < HOOK_CTRL_FIRST; i++)
		if (s_hookId[i] >= 0 && i != HOOK_GXM_INITIALIZE) return 1;
	return s_hookId[HOOK_PVR_SWAP] >= 0;
}

static unsigned hookMask(void)
{
	unsigned mask = 0;
	int i;
	/* The mask is 32 bits; dev probe entries past bit 31 are not shown
	 * (a shift by 32 or more is undefined). */
	for (i = 0; i < HOOK_COUNT && i < 32; i++)
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
	/* Do not export-hook AddEntry here for titles that import it nowhere
	 * (Sony's PowerVR GLES stack, HELP04040): tried 2026-10-27, the device
	 * froze hard within the first minute (power-button restart). The init
	 * export hooks are fine; AddEntry's hook sleeps and waits on the
	 * calling thread, which that driver evidently cannot afford. */
	if (!submitHooked())
		installHooksOnLoadedModules();
	writeTitleIdToKernel(s_titleid);   /* releases any panel rate the last title left */
	if (s_cfg.pg_panel > 0) {
		/* The kernel range-checks it and refuses on anything but the OLED's
		 * stock mode 0 table; the line says what was granted. */
		FpsvitaState st;
		char line[64];
		fpsvitaSetPanel(s_cfg.pg_panel);
		st.panel_hz = 0;
		fpsvitaGetState(&st, sizeof(st));
		sceClibSnprintf(line, sizeof(line), "helper: panel want=%d got=%d\n",
		                (int)s_cfg.pg_panel, (int)st.panel_hz);
		helperLog(line);
	}
	fpsvitaSetNativeInterval(0);
	s_savedInterval = s_cfg.interval;
	s_savedInject = s_cfg.inject;
	s_savedAutodetect = s_cfg.autodetect;
	{
		int vs = s_cfg.pg_vsync;
		fpsvitaSetAutodetect(vs == 0 ? FPSVITA_AUTODETECT_ON
		                    : vs == 1 ? FPSVITA_AUTODETECT_OFF : s_cfg.autodetect);
		fpsvitaSetInject(vs == 2 ? FPSVITA_INJECT_ON
		                : vs == 1 ? FPSVITA_INJECT_OFF : s_cfg.inject);
	}
	{
		/* Latency options for this title go to the kernel, where the quick
		 * menu can change them live; the frame path reads them back. */
		/* Restore target at module_stop: the globals from config.txt, like
		 * the interval. Not the kernel's current values, which after a killed
		 * title (no module_stop) are still that title's. */
		s_savedAdvFlags = (s_cfg.lowlatency ? FPSVITA_ADV_LOWLAT : 0)
		                | (s_cfg.jit ? FPSVITA_ADV_JIT : 0)
		                | (s_cfg.tear ? FPSVITA_ADV_TEAR : 0);
		s_savedSoftcap = s_cfg.softcap;
		int jit = s_cfg.pg_jit >= 0 ? s_cfg.pg_jit : s_cfg.jit;
		int tear = s_cfg.pg_tear >= 0 ? s_cfg.pg_tear : s_cfg.tear;
		int32_t sc = s_cfg.pg_sc >= 0 ? s_cfg.pg_sc : s_cfg.softcap;
		fpsvitaSetAdvanced((s_lowLatency ? FPSVITA_ADV_LOWLAT : 0)
		                   | (jit ? FPSVITA_ADV_JIT : 0)
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
	/* No title id, a system app (NPXS*: screenshot service, trophy
	 * manager, ...), or the PSP emulator (Adrenaline): nothing to pace.
	 * Leave without touching the flash. */
	if (pr < 0 || s_titleid[0] == '\0' || sceClibStrncmp(s_titleid, "NPXS", 4) == 0
	    || sceClibStrncmp(s_titleid, FPSVITA_ADRENALINE_TITLEID,
	                      sizeof(FPSVITA_ADRENALINE_TITLEID) - 1) == 0)
		return SCE_KERNEL_START_NO_RESIDENT;

	rotateHelperLog();
	readGameConfig(s_titleid, &s_cfg);
	s_pergame = s_cfg.pergame;
	s_readPeek = s_cfg.pg_read
	             || (compatFlagsFor(s_titleid) & FPSVITA_COMPAT_NOINPUT);
	s_rateLog = s_cfg.log;
	s_sysconOn = s_cfg.syscon;
	s_sticksOn = s_cfg.sticks;
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
	/* init=: the GXM init imports found on the main module, sceGxmInitialize
	 * then sceGxmVshInitialize (i hooked, - not); late modules may add one. */
	char init[3] = { s_hookId[HOOK_GXM_INITIALIZE] >= 0 ? 'i' : '-',
	                 s_hookId[HOOK_GXM_VSH_INITIALIZE] >= 0 ? 'i' : '-', 0 };
	sceClibSnprintf(line, sizeof(line),
	                "helper: %s hooks=%d mask=0x%04x loaders=%d pergame=%d read=%d init=%s\n",
	                s_titleid, installed, hookMask(), loaders, (int)s_pergame, s_readPeek, init);
	helperLog(line);
	logEnvironment();

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
	/* Title id first: the shell polls every 200 ms and treats a limit or
	 * latency change seen while a title is still named as a user toggle
	 * (the overlay's LAST row). With the title gone, the restores below
	 * are just the globals coming back. */
	if (s_ownsKernelState)
		clearTitleIdInKernel();
	if (s_appliedOverride) {
		fpsvitaSetInterval(s_savedInterval);
		s_appliedOverride = 0;
	}
	if (s_ownsKernelState) {
		fpsvitaSetNativeInterval(0);
		fpsvitaSetInject(s_savedInject);
		fpsvitaSetAutodetect(s_savedAutodetect);
		fpsvitaSetAdvanced(s_savedAdvFlags, s_savedSoftcap);
		s_ownsKernelState = 0;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
