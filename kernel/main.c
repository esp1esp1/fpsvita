/*
 * FPSVita Kernel Module (fpsvita-kernel.skprx)
 *
 * Loaded under *KERNEL. Holds the shared state (FpsvitaState) in kernel
 * memory and exports syscall-accessible accessors so both SceShell (the UI)
 * and game processes (the helper) can read and write it.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/display.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <string.h>

#include "../common/fpsvita_common.h"

/*
 * ARM CP15 TPIDRURO manipulation for syscall context.
 * Same pattern as PSVshellPlus_Kernel/common.h.
 * Required around any kernel function exported as a user-mode syscall.
 */
#define SYSCALL_ENTER \
	unsigned int _scstate = __builtin_arm_mrc(15, 0, 13, 0, 3); \
	__builtin_arm_mcr(15, 0, _scstate << 16, 13, 0, 3);

#define SYSCALL_EXIT \
	__builtin_arm_mcr(15, 0, _scstate, 13, 0, 3);

static volatile int32_t g_interval       = FPSVITA_INTERVAL_OFF;
static volatile int32_t g_inject         = FPSVITA_INJECT_OFF;
static volatile int32_t g_autodetect     = FPSVITA_AUTODETECT_ON;
static volatile int32_t g_native_interval = 0; /* 0 = not yet detected */
static volatile uint32_t g_titleid[3]    = {0, 0, 0};
static volatile int32_t g_hud_mode       = FPSVITA_HUD_OFF;
static volatile int32_t g_hud_pos        = 0;
static volatile int32_t g_adv_flags      = 0;
static volatile int32_t g_softcap        = 0;
static FpsvitaStats     g_stats;          /* zeroed when a new title id lands */
static SceUID           g_owner_pid = -1; /* process that wrote the title id */
static SceUID           g_proc_event = -1;
static volatile int32_t g_panel_hz = 0;   /* applied panel refresh, 0 = stock */

static void panelSet(int32_t hz);   /* below, with the panel timing patch */

/*
 * Interval validation.
 *
 * The companion casts this value to unsigned and hands it to
 * sceDisplayWaitVblankStartMulti as a vblank count, so an out-of-range value
 * is not cosmetic: -2 becomes 0xFFFFFFFE and blocks the game's render thread
 * essentially forever. Two untrusted paths reach here — a hand-edited or
 * truncated state.txt (sceClibStrtoll returns 0 = UNCAPPED on a parse error),
 * and this syscall itself, which any user process can call with any int32.
 * Clamping in the single kernel-side setter closes both at once.
 */
static int32_t sanitizeInterval(int32_t v)
{
	switch (v) {
	case FPSVITA_INTERVAL_OFF:
	case FPSVITA_INTERVAL_UNCAPPED:
	case FPSVITA_INTERVAL_60FPS:
	case FPSVITA_INTERVAL_30FPS:
	case FPSVITA_INTERVAL_20FPS:
		return v;
	default:
		return FPSVITA_INTERVAL_OFF;
	}
}

int fpsvitaGetState(FpsvitaState *out)
{
	FpsvitaState s;
	int ret;
	SYSCALL_ENTER
	s.interval        = g_interval;
	s.inject          = g_inject;
	s.autodetect      = g_autodetect;
	s.native_interval = g_native_interval;
	s.titleid[0]      = g_titleid[0];
	s.titleid[1]      = g_titleid[1];
	s.titleid[2]      = g_titleid[2];
	s.hud_mode        = g_hud_mode;
	s.hud_pos         = g_hud_pos;
	s.adv_flags       = g_adv_flags;
	s.softcap         = g_softcap;
	s.stats           = g_stats;
	s.panel_hz        = g_panel_hz;
	ret = ksceKernelCopyToUser(out, &s, sizeof(s));
	SYSCALL_EXIT
	return ret;
}

void fpsvitaSetInterval(int32_t interval)
{
	SYSCALL_ENTER
	g_interval = sanitizeInterval(interval);
	SYSCALL_EXIT
}

void fpsvitaSetInject(int32_t inject)
{
	SYSCALL_ENTER
	/* Used as a boolean by the companion; normalise so a stray value from
	 * state.txt or a rogue syscall cannot mean "neither on nor off". */
	g_inject = (inject == FPSVITA_INJECT_ON) ? FPSVITA_INJECT_ON
	                                          : FPSVITA_INJECT_OFF;
	SYSCALL_EXIT
}

void fpsvitaSetAutodetect(int32_t autodetect)
{
	SYSCALL_ENTER
	g_autodetect = (autodetect == FPSVITA_AUTODETECT_OFF)
	                   ? FPSVITA_AUTODETECT_OFF : FPSVITA_AUTODETECT_ON;
	SYSCALL_EXIT
}

void fpsvitaSetNativeInterval(int32_t interval)
{
	SYSCALL_ENTER
	g_native_interval = interval;
	SYSCALL_EXIT
}

void fpsvitaSetTitleIdPart(int32_t part, uint32_t value)
{
	SYSCALL_ENTER
	if (part >= 0 && part < 3)
		g_titleid[part] = value;
	/* A new (or cleared) title must not inherit the previous one's numbers.
	 * Remember who owns it: a process that dies without module_stop
	 * (`quit all`, a crash) is cleaned up by the process-exit handler. */
	if (part == 0) {
		FpsvitaStats z = {0};
		g_stats = z;
		g_owner_pid = value ? ksceKernelGetProcessId() : -1;
		panelSet(0);   /* a panel rate belongs to the title that asked for it */
	}
	SYSCALL_EXIT
}

void fpsvitaSetHud(int32_t mode, int32_t pos)
{
	SYSCALL_ENTER
	g_hud_mode = (mode >= 0 && mode < FPSVITA_HUD_MODES) ? mode : FPSVITA_HUD_OFF;
	g_hud_pos  = (pos >= 0 && pos < FPSVITA_HUD_POS_COUNT) ? pos : 0;
	SYSCALL_EXIT
}

/*
 * Tearing mode: the user-mode sceDisplaySetFrameBuf
 * refuses SCE_DISPLAY_SETBUF_IMMEDIATE (0x80290006 on every title tested,
 * 2026-10-17); this takes the kernel entry point from the caller's process
 * context. The SceDisplayFrameBuf (sizeof, 0x18 bytes) is copied in from user
 * memory. Only the process that owns the title id may flip: this is a
 * kernel-privilege display write, not something every user process should
 * reach. Return values lose bit 30 on the way back (see fpsvitaGetState), so
 * the helper only tests the sign.
 */
static int callerIsOwner(void)
{
	return g_owner_pid >= 0 && ksceKernelGetProcessId() == g_owner_pid;
}

int fpsvitaSetFrameBufImmediate(const void *user_fb)
{
	SceDisplayFrameBuf fb;
	int ret;
	SYSCALL_ENTER
	if (callerIsOwner()
	    && ksceKernelCopyFromUser(&fb, user_fb, sizeof(fb)) == 0 && fb.size == sizeof(fb))
		ret = ksceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE);
	else
		ret = -1;
	SYSCALL_EXIT
	return ret;
}

void fpsvitaSetAdvanced(int32_t flags, int32_t softcap)
{
	SYSCALL_ENTER
	g_adv_flags = flags & (FPSVITA_ADV_LOWLAT | FPSVITA_ADV_JIT
	                       | FPSVITA_ADV_TEAR | FPSVITA_ADV_AUTO);
	g_softcap = (softcap >= FPSVITA_SOFTCAP_MIN && softcap <= FPSVITA_SOFTCAP_MAX) ? softcap : 0;
	SYSCALL_EXIT
}

void fpsvitaSetStats(const FpsvitaStats *in)
{
	FpsvitaStats s;
	SYSCALL_ENTER
	/* Only the running title's helper pushes numbers; the shell reads them. */
	if (callerIsOwner() && ksceKernelCopyFromUser(&s, in, sizeof(s)) == 0)
		g_stats = s;
	SYSCALL_EXIT
}


/* The owning game went away without telling us: forget it, so the shell
 * stops showing its numbers and heals the interval at the next menu open. */
static void ownerGone(SceUID pid)
{
	if (pid != g_owner_pid || g_owner_pid < 0)
		return;
	FpsvitaStats z = {0};
	g_titleid[0] = g_titleid[1] = g_titleid[2] = 0;
	g_native_interval = 0;
	g_stats = z;
	g_owner_pid = -1;
	panelSet(0);
}

/*
 * Panel refresh (experimental, per-game `panel=<hz>`): the OLED's mode 0 DSI
 * timing entry in SceLowio (xerpi's libbaremetal dsi.c layout: flags,
 * pixelclock24, subinfo24, pixelclock30, subinfo30, htotal, vtotal,
 * interlaced, HFP, HSW, HBP, VFP, VSW, VBP) is re-read every frame for vtotal
 * and VFP, so a taller vtotal with the extra lines in the front porch lowers
 * the refresh live: 1050 x 890 is 40.0 Hz (measured 2026-10-18 on a PCH-1000,
 * fw 3.65, no artefacts at 24 to 50 Hz). Nothing is written unless the panel
 * is in mode 0 and exactly one entry matches every known word, so an LCD
 * model, a PSTV or another firmware's table is left alone. Released (the
 * original words come back) when the title exits or another one starts.
 */
#define PANEL_PCLK_HZ   37384615u   /* 1050 x 594 x 59.94 */
#define PANEL_HTOTAL    1050u
#define PANEL_VACTIVE   590u        /* vtotal minus the VFP it is drawn with */
#define PANEL_WORDS     14

/* SceDisplayForDriver 0xD2CED235, in the SDK stub but not its headers */
int ksceDisplayGetOutputMode(int head, unsigned int *mode, unsigned int *pixelformat);

typedef int (*GetModuleInfoFn)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid,
                           uintptr_t *func);

static SceUID   g_panel_lock = -1;
static SceUID   g_panel_inject = -1;
static SceUID   g_panel_modid = -1;
static int      g_panel_seg = -1;
static uint32_t g_panel_off;
static int      g_panel_scanned;    /* 1 found, -1 not this hardware/firmware */

/* ksceKernelGetModuleInfo's NID changed in 3.63; resolve whichever exists. */
static GetModuleInfoFn getModuleInfoFn(void)
{
	static const uint32_t nids[2][2] = {{0xC445FA63, 0xD269F915},    /* 3.60 */
	                                    {0x92C9FFC2, 0xDAA90093}};   /* 3.63+ */
	uintptr_t fn = 0;
	for (int i = 0; i < 2; i++)
		if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", nids[i][0], nids[i][1], &fn) >= 0
		    && fn)
			return (GetModuleInfoFn)fn;
	return NULL;
}

static int panelFind(void)
{
	static const uint32_t want[PANEL_WORDS] = {5, 2243100, 0, 0, 0, PANEL_HTOTAL, 594,
	                                           0, 20, 66, 4, 4, 4, 42};
	if (g_panel_scanned)
		return g_panel_scanned;
	g_panel_scanned = -1;
	GetModuleInfoFn getInfo = getModuleInfoFn();
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	if (!getInfo || taiGetModuleInfoForKernel(KERNEL_PID, "SceLowio", &ti) < 0)
		return -1;
	SceKernelModuleInfo mi;
	memset(&mi, 0, sizeof(mi));
	mi.size = sizeof(mi);
	if (getInfo(KERNEL_PID, ti.modid, &mi) < 0)
		return -1;
	int hits = 0;
	for (int i = 0; i < 4; i++) {
		const uint32_t *w = mi.segments[i].vaddr;
		uint32_t n = mi.segments[i].memsz / 4;
		if (!w)
			continue;
		for (uint32_t k = 0; k + PANEL_WORDS <= n; k++) {
			int j;
			for (j = 0; j < PANEL_WORDS; j++)
				if (j != 2 && j != 4 && w[k + j] != want[j])   /* the subinfo words vary */
					break;
			if (j == PANEL_WORDS) {
				hits++;
				g_panel_seg = i;
				g_panel_off = k * 4;
			}
		}
	}
	if (hits != 1)
		return -1;
	g_panel_modid = ti.modid;
	g_panel_scanned = 1;
	return 1;
}

/* hz 0 restores the stock timing. Kernel callers only: the syscall checks
 * the owner first. */
static void panelSet(int32_t hz)
{
	if (g_panel_lock < 0)
		return;
	ksceKernelLockMutex(g_panel_lock, 1, NULL);
	if (g_panel_inject >= 0) {
		taiInjectReleaseForKernel(g_panel_inject);
		g_panel_inject = -1;
		g_panel_hz = 0;
	}
	unsigned int mode = 1, pf = 0;
	if (hz >= FPSVITA_PANEL_HZ_MIN && hz <= FPSVITA_PANEL_HZ_MAX
	    && ksceDisplayGetOutputMode(0, &mode, &pf) >= 0 && mode == 0 && panelFind() > 0) {
		uint32_t vt = (PANEL_PCLK_HZ + PANEL_HTOTAL * (uint32_t)hz / 2) / (PANEL_HTOTAL * (uint32_t)hz);
		/* words 5..13: htotal, vtotal, interlaced, HFP, HSW, HBP, VFP, VSW, VBP */
		uint32_t v[9] = {PANEL_HTOTAL, vt, 0, 20, 66, 4, vt - PANEL_VACTIVE, 4, 42};
		g_panel_inject = taiInjectDataForKernel(KERNEL_PID, g_panel_modid, g_panel_seg,
		                                        g_panel_off + 5 * 4, v, sizeof(v));
		if (g_panel_inject >= 0)
			g_panel_hz = hz;
	}
	ksceKernelUnlockMutex(g_panel_lock, 1);
}

void fpsvitaSetPanel(int32_t hz)
{
	SYSCALL_ENTER
	if (callerIsOwner())
		panelSet(hz);
	SYSCALL_EXIT
}

static int procExit(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{ ownerGone(pid); return 0; }
static int procKill(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{ ownerGone(pid); return 0; }

int __module_start(SceSize args, const void *argp)
{
	SceProcEventHandler h;
	h.size = sizeof(h);
	h.create = NULL;
	h.exit = procExit;
	h.kill = procKill;
	h.stop = NULL;
	h.start = NULL;
	h.switch_process = NULL;
	g_proc_event = ksceKernelRegisterProcEventHandler("fpsvita", &h, 0);
	g_panel_lock = ksceKernelCreateMutex("fpsvita_panel", 0, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int __module_stop(SceSize args, const void *argp)
{
	if (g_proc_event >= 0)
		ksceKernelUnregisterProcEventHandler(g_proc_event);
	panelSet(0);
	if (g_panel_lock >= 0)
		ksceKernelDeleteMutex(g_panel_lock);
	return SCE_KERNEL_STOP_SUCCESS;
}
