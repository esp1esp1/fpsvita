#ifndef FPSVITA_COMMON_H
#define FPSVITA_COMMON_H

#include <stdint.h>

/*
 * Interval values:
 *   -1 = Off (passthrough, hooks do nothing)
 *    1 = 60fps (vsync every vblank)
 *    2 = 30fps (vsync every 2 vblanks)
 *    3 = 20fps (vsync every 3 vblanks)
 *    0 = Uncapped (skip all vblank waits)
 *
 * Inject values:
 *    0 = Off
 *    1 = On (add vblank wait inside sceDisplaySetFrameBuf)
 *
 * Inject is only meaningful when interval is 1, 2, or 3.
 * Inject + Uncapped is contradictory and suppressed by the companion.
 * Inject + Off is meaningless and suppressed by the companion.
 */

#define FPSVITA_INTERVAL_OFF      (-1)
#define FPSVITA_INTERVAL_UNCAPPED (0)
#define FPSVITA_INTERVAL_60FPS    (1)
#define FPSVITA_INTERVAL_30FPS    (2)
#define FPSVITA_INTERVAL_20FPS    (3)

#define FPSVITA_INJECT_OFF (0)
#define FPSVITA_INJECT_ON  (1)

#define FPSVITA_AUTODETECT_ON  (1)
#define FPSVITA_AUTODETECT_OFF (0)

/*
 * Auto-detect thresholds, in SetFrameBuf calls since the last vsync wait.
 *
 * ENGAGE: no vsync wait seen for this many presents => the game is not
 *   pacing on vsync, so inject. 30 (~0.5s at 60fps) rides through brief gaps
 *   such as loading screens and menu transitions. The delay at game launch is
 *   invisible because the game is still drawing its first frames.
 *
 * RELEASE: a present this soon after a vsync wait counts as "paced".
 *
 * RELEASE_STREAK: how many consecutive paced presents are required before
 *   inject is released again.
 *
 * Two thresholds alone are NOT enough. The measurement resets to zero on
 * every vsync wait, so with a plain engage/release pair any title whose gap
 * merely exceeds ENGAGE still toggles: it crosses ENGAGE, engages, then the
 * next wait drops the gap to zero and immediately releases. Widening the band
 * only moves which titles oscillate (measured: a simple 30/2 pair still
 * oscillated for gaps of 32-35 presents).
 *
 * So release requires a sustained run of paced presents, which a bursty title
 * never accumulates — its streak is reset every time the gap crosses ENGAGE —
 * while a title that genuinely resumes vsync pacing reaches it in about a
 * second. Verified off-device across gaps of 25-35 presents (tests/autodetect.cpp).
 */
#define FPSVITA_AUTODETECT_ENGAGE         (30)
#define FPSVITA_AUTODETECT_RELEASE        (2)
#define FPSVITA_AUTODETECT_RELEASE_STREAK (60)

/*
 * Rate gate on ENGAGE: besides the missing vsync waits, the game must be
 * presenting at least this much faster than the cap (present period at most
 * (100 - margin)% of the cap's period). A title already held at or below the
 * cap by a wait this plugin cannot see (VitaGrafix's withWait hook, a
 * sub-module's own vblank call) or by being GPU bound gains nothing from a
 * second wait and would be slowed to the next vblank multiple.
 */
#define FPSVITA_AUTODETECT_RATE_MARGIN_PCT (10)
#define FPSVITA_VBLANK_US (16683)

/*
 * Size of the state.txt read buffer. Shared by every parser so the writer can
 * never produce a file a reader cannot fully see. Readers that hit this limit
 * must refuse to rewrite the file rather than truncate it.
 */
#define FPSVITA_STATE_BUF (4096)

#define FPSVITA_STATE_DIR   "ur0:data/fpsvita"
#define FPSVITA_CONFIG_FILE "ur0:data/fpsvita/config.txt"  /* the global keys */
#define FPSVITA_STATE_FILE  "ur0:data/fpsvita/state.txt"   /* one line per title */
#define FPSVITA_RCO_FILE   "ur0:data/fpsvita/fpsvita_plugin.rco"

/* Cycle order for UI left/right buttons (5 entries, wraps around) */
static const int32_t fpsvita_cycle[] = {
	FPSVITA_INTERVAL_OFF,
	FPSVITA_INTERVAL_60FPS,
	FPSVITA_INTERVAL_30FPS,
	FPSVITA_INTERVAL_20FPS,
	FPSVITA_INTERVAL_UNCAPPED
};
#define FPSVITA_CYCLE_LEN (sizeof(fpsvita_cycle) / sizeof(fpsvita_cycle[0]))

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel module exports (FpsvitaForUser library, syscall-accessible).
 *
 * Values are read back through one copy-out syscall, fpsvitaGetState(),
 * not through return values: on this firmware a syscall's return has bit 30
 * cleared on the way back to user mode (-1 arrives as 0xbfffffff), which
 * breaks FPSVITA_INTERVAL_OFF and any packed title-id word containing a
 * byte with bit 6 set. The fpsvitaGet*() helpers below keep the old call
 * sites working. */
/*
 * Overlay (HUD) drawn by the shell over the running game. The shell owns the
 * mode and position (state.txt keys `hud=` and `hudpos=`), the helper only
 * reads the mode to know whether to push FpsvitaStats.
 */
#define FPSVITA_HUD_OFF   (0)
#define FPSVITA_HUD_MINI  (1)   /* fps, cap, and whatever is active */
#define FPSVITA_HUD_FULL  (2)   /* plus frame time, lag, controller phase */
#define FPSVITA_HUD_MODES (3)
#define FPSVITA_HUD_POS_COUNT (4) /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */

/*
 * Latency options in force for the running title: the per-game line's values
 * if it has them, else the global keys. The helper sets them at game start,
 * the quick menu's advanced row changes them live. jit and softcap act on the
 * next frame; lowlatency is applied once, at the title's GXM init, so a
 * change shows as "pending" in the overlay until the next launch.
 */
#define FPSVITA_ADV_LOWLAT (1 << 0)
#define FPSVITA_ADV_JIT    (1 << 1)   /* anti-lag: fresh poll and the whole-vblank sleep */
#define FPSVITA_ADV_TEAR   (1 << 2)   /* Uncapped only: every flip IMMEDIATE through the kernel */
#define FPSVITA_ADV_AUTO   (1 << 3)   /* anti-lag auto: fresh poll only; the sleep stays off */
#define FPSVITA_SOFTCAP_MIN (10)
#define FPSVITA_SOFTCAP_MAX (120)
static const int32_t fpsvita_softcap_cycle[] = {0, 20, 24, 25, 30, 40, 45, 50, 60};
#define FPSVITA_SOFTCAP_CYCLE_LEN (sizeof(fpsvita_softcap_cycle) / sizeof(fpsvita_softcap_cycle[0]))

/* Live numbers from the helper, pushed about four times a second while the
 * HUD is on. All EMAs are microseconds; 0 means "not measured". */
#define FPSVITA_STATF_INJECT     (1u << 0)  /* inject wait engaged (manual or auto) */
#define FPSVITA_STATF_LOWLATENCY (1u << 1)  /* GXM queue depth clamped at this title's init: in effect */
#define FPSVITA_STATF_JIT        (1u << 2)  /* full Antilag requested; auto is in adv_flags */
#define FPSVITA_STATF_ADDENTRY   (1u << 3)  /* AddEntry hooked: work/queue/input are valid */
#define FPSVITA_STATF_TEAR       (1u << 4)  /* tearing flips are going through the kernel */
#define FPSVITA_STATF_LOWLAT_NATIVE (1u << 5) /* the title itself asked for a 1-deep queue */

typedef struct FpsvitaStats {
	uint32_t fps_x10;       /* presents per second, times 10 */
	uint32_t frame_us;      /* present period EMA (frametime) */
	uint32_t work_us;       /* CPU frame time EMA */
	uint32_t queue_us;      /* the frame's own submit-to-flip EMA */
	uint32_t input_us;      /* the frame's own newest controller sample to its flip EMA */
	uint32_t ctrl_pct;      /* controller reads landing after AddEntry, percent */
	uint32_t jit_slack_us;  /* current jit sleep, 0 when gated */
	int32_t  softcap;       /* fps, 0 off */
	uint32_t flags;         /* FPSVITA_STATF_* */
	uint32_t jit_missed;    /* frames over budget while jit was sleeping (each one gates it) */
	uint32_t sample_age_us; /* age of the controller sample the game gets, EMA (0: no ctrl hook) */
	uint32_t seq;           /* bumps per push; 0 = nothing pushed yet */
} FpsvitaStats;

typedef struct FpsvitaState {
	int32_t  interval;
	int32_t  inject;
	int32_t  autodetect;
	int32_t  native_interval;
	uint32_t titleid[3];
	int32_t  hud_mode;
	int32_t  hud_pos;
	int32_t  adv_flags;     /* FPSVITA_ADV_* requested for the running title */
	int32_t  softcap;       /* fps, 0 off */
	FpsvitaStats stats;
} FpsvitaState;

/* Copies sizeof(FpsvitaState) to user memory. The three modules must be
 * deployed together: a reader built against a shorter struct would have its
 * stack overrun by the copy.
 *
 * The lite helper (FPSVITA_LITE) has no kernel module: the same functions
 * are file-local over a process-private copy of the state (companion/lite.h). */
#ifdef FPSVITA_LITE
#define FPSVITA_API static
#else
#define FPSVITA_API
#endif
FPSVITA_API int  fpsvitaGetState(FpsvitaState *out);
FPSVITA_API void fpsvitaSetInterval(int32_t interval);
FPSVITA_API void fpsvitaSetInject(int32_t inject);
FPSVITA_API void fpsvitaSetAutodetect(int32_t autodetect);
FPSVITA_API void fpsvitaSetNativeInterval(int32_t interval);
FPSVITA_API void fpsvitaSetHud(int32_t mode, int32_t pos);
FPSVITA_API void fpsvitaSetAdvanced(int32_t flags, int32_t softcap);
FPSVITA_API void fpsvitaSetStats(const FpsvitaStats *in);  /* copy-in from user memory */
FPSVITA_API int  fpsvitaSetFrameBufImmediate(const void *fb); /* experiment: kernel IMMEDIATE flip */

static inline int32_t fpsvitaGetInterval(void)
{ FpsvitaState s; s.interval = FPSVITA_INTERVAL_OFF; fpsvitaGetState(&s); return s.interval; }
static inline int32_t fpsvitaGetInject(void)
{ FpsvitaState s; s.inject = 0; fpsvitaGetState(&s); return s.inject; }
static inline int32_t fpsvitaGetAutodetect(void)
{ FpsvitaState s; s.autodetect = 1; fpsvitaGetState(&s); return s.autodetect; }
static inline int32_t fpsvitaGetNativeInterval(void)
{ FpsvitaState s; s.native_interval = 0; fpsvitaGetState(&s); return s.native_interval; }

/*
 * Titleid is stored as 3 x uint32_t (12 bytes, fits 9-char titleid + null).
 * Avoids kernel string copy. Companion sets all 3 parts at game launch,
 * UI reads all 3 parts on menu open.
 */
FPSVITA_API void fpsvitaSetTitleIdPart(int32_t part, uint32_t value);
static inline uint32_t fpsvitaGetTitleIdPart(int32_t part)
{ FpsvitaState s; s.titleid[0] = s.titleid[1] = s.titleid[2] = 0; fpsvitaGetState(&s); return (part >= 0 && part < 3) ? s.titleid[part] : 0; }

/* Max length for titleid strings (9 chars + null) */
#define FPSVITA_TITLEID_LEN 12

#ifdef __cplusplus
}
#endif

#endif /* FPSVITA_COMMON_H */
