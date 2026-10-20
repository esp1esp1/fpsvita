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
#include <taihen.h>

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
	return SCE_KERNEL_START_SUCCESS;
}

int __module_stop(SceSize args, const void *argp)
{
	if (g_proc_event >= 0)
		ksceKernelUnregisterProcEventHandler(g_proc_event);
	return SCE_KERNEL_STOP_SUCCESS;
}
