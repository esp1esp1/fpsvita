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
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/syscon.h>
#include <psp2kern/lowio/dsi.h>
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
static volatile int     g_stk_hold;   /* sysEvent: suspend or shutdown under way, no syscon packets */
static volatile int32_t g_softcap        = 0;
static FpsvitaStats     g_stats;          /* zeroed when a new title id lands */
static SceUID           g_owner_pid = -1; /* process that wrote the title id */
static SceUID           g_proc_event = -1;
static SceUID           g_sys_event = -1;
static volatile int32_t g_panel_hz = 0;   /* applied panel refresh, 0 = stock */
static int32_t g_panel_resume = 0;        /* rate to re-apply after a resume, 0 none */

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

int fpsvitaGetState(FpsvitaState *out, uint32_t size)
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
	s.vcount          = 0xffffffffu;
	if (size > sizeof(s))
		size = sizeof(s);
	ret = ksceKernelCopyToUser(out, &s, size);
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
		g_panel_resume = 0;
		panelSet(0);   /* a panel rate belongs to the title that asked for it */
	}
	SYSCALL_EXIT
}

void fpsvitaSetHud(int32_t mode, int32_t pos)
{
	SYSCALL_ENTER
	/* The shell ORs FPSVITA_HUD_STATS in to get the helper's push with the
	 * overlay off (Panel auto); the overlay mode itself is the low byte. */
	{
		int32_t m = mode & 0xFF;
		g_hud_mode = ((m >= 0 && m < FPSVITA_HUD_MODES) ? m : FPSVITA_HUD_OFF) | (mode & FPSVITA_HUD_STATS);
	}
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
	g_adv_flags = flags & (FPSVITA_ADV_LOWLAT | FPSVITA_ADV_JIT | FPSVITA_ADV_TEAR);
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
	g_panel_resume = 0;
	panelSet(0);
}

/* Sleep with a stretched panel: resume rebuilds part of the display setup
 * from the timing entry, and after the title exited at stock timing again
 * the shell's top layers flickered half-drawn until a reboot (OLED,
 * 2026-10-23). So the stock timing goes back before suspend, and the rate
 * is re-applied once resume is done (late resume, 0x100000), which is the
 * live change that has always been clean. */
int ksceKernelUnregisterSysEventHandler(SceUID id);

static int sysEvent(int resume, int eventid, void *args, void *opt)
{
	/* No stick packets from the first suspend or shutdown phase until the
	 * resume completes: the syscon runs the power sequence. */
	g_stk_hold = !resume || eventid != 0x100000;
	if (!resume) {
		if (g_panel_hz > 0 && !g_panel_resume) {   /* first suspend phase only */
			g_panel_resume = g_panel_hz;
			panelSet(0);
		}
	} else if (eventid == 0x100000 && g_panel_resume) {
		int32_t hz = g_panel_resume;
		g_panel_resume = 0;
		if (g_owner_pid >= 0)
			panelSet(hz);
	}
	return 0;
}

/*
 * Panel refresh (experimental, per-game `panel=<hz>`): the internal panel's
 * DSI timing entry in SceLowio (xerpi's libbaremetal dsi.c layout: flags,
 * pixelclock24, subinfo24, pixelclock30, subinfo30, htotal, vtotal,
 * interlaced, HFP, HSW, HBP, VFP, VSW, VBP) is re-read every frame for vtotal
 * and VFP, so a taller vtotal with the extra lines in the front porch lowers
 * the refresh live (the wiki's DSI Registers page, 2026-10-28: the hardware
 * takes V_active = vtotal - VFP - VSW - VBP, 544 on both panels, and this
 * patch keeps vtotal - VFP constant so the active height stays 544). The OLED
 * (PCH-1000) runs from 1050 x 594 at 37.38 MHz; the Slim's LCD (PCH-2000)
 * from a different entry, 1120 x 585 at 39.27 MHz with its own porches
 * (tests/paneltest, 2026-10-29: both tables hold both entries, and
 * ksceDsiGetPixelClock reports the pixel clock word of the one in use, so
 * that is how the entry is chosen). Measured on both: 50/45/40/35/30/24/20
 * all within 0.03 Hz of the request; the picture breaks up below 30 on both
 * and the LCD is clean down to 30 (the ladder's floor). Nothing is written
 * unless the panel is in mode 0 and exactly one entry matches every known
 * word of the panel's own entry, so another firmware's table is left alone.
 * Released (the original words come back) when the title exits or another
 * one starts.
 */
#define PANEL_WORDS     14

/* SceDisplayForDriver 0xD2CED235, in the SDK stub but not its headers */
int ksceDisplayGetOutputMode(int head, unsigned int *mode, unsigned int *pixelformat);

typedef int (*GetModuleInfoFn)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid,
                           uintptr_t *func);

/* The two internal panels' stock entries (the subinfo words vary and are
 * not matched). The pixel clock word is in units of 1/16.667 Hz. */
typedef struct {
	uint32_t flags, pclk, ht, vt, hfp, hsw, hbp, vfp, vsw, vbp;
} PanelEntry;
static const PanelEntry k_panels[] = {
	{5, 2243100, 1050, 594, 20, 66, 4, 4, 4, 42},     /* OLED, PCH-1000 */
	{4, 2356363, 1120, 585, 136, 4, 20, 4, 4, 33},    /* LCD, PCH-2000 */
};

static SceUID   g_panel_lock = -1;
static SceUID   g_panel_inject = -1;
static SceUID   g_panel_modid = -1;
static int      g_panel_seg = -1;
static uint32_t g_panel_off;
static int      g_panel_scanned;    /* 1 found, -1 not this hardware/firmware */
static const PanelEntry *g_panel_entry;

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
	if (g_panel_scanned)
		return g_panel_scanned;
	g_panel_scanned = -1;
	int pclk = ksceDsiGetPixelClock(0);
	const PanelEntry *pe = NULL;
	for (unsigned i = 0; i < sizeof(k_panels) / sizeof(k_panels[0]); i++)
		if ((int)k_panels[i].pclk == pclk)
			pe = &k_panels[i];
	if (!pe)
		return -1;
	const uint32_t want[PANEL_WORDS] = {pe->flags, pe->pclk, 0, 0, 0, pe->ht, pe->vt, 0,
	                                    pe->hfp, pe->hsw, pe->hbp, pe->vfp, pe->vsw, pe->vbp};
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
	g_panel_entry = pe;
	g_panel_scanned = 1;
	return 1;
}

/* hz 0 restores the stock timing. Kernel callers only: the syscall checks
 * the caller first. */
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
		const PanelEntry *pe = g_panel_entry;
		uint64_t pclk_hz = (uint64_t)pe->pclk * 50u / 3u;
		uint32_t den = pe->ht * (uint32_t)hz;
		uint32_t vt = (uint32_t)((pclk_hz + den / 2) / den);
		/* words 5..13: htotal, vtotal, interlaced, HFP, HSW, HBP, VFP, VSW, VBP;
		 * vtotal - VFP stays what the stock entry has, so V_active does too */
		uint32_t v[9] = {pe->ht, vt, 0, pe->hfp, pe->hsw, pe->hbp, vt - (pe->vt - pe->vfp), pe->vsw, pe->vbp};
		g_panel_inject = taiInjectDataForKernel(KERNEL_PID, g_panel_modid, g_panel_seg,
		                                        g_panel_off + 5 * 4, v, sizeof(v));
		if (g_panel_inject >= 0)
			g_panel_hz = hz;
	}
	ksceKernelUnlockMutex(g_panel_lock, 1);
}

/* The running title's helper at launch, or the shell's FPS row while that
 * title runs (`experimental=1`). Released with the title either way. */
void fpsvitaSetPanel(int32_t hz)
{
	SYSCALL_ENTER
	if (callerIsOwner()
	    || (g_owner_pid >= 0 && ksceKernelGetProcessId() == ksceKernelSysrootGetShellPid()))
		panelSet(hz);
	SYSCALL_EXIT
}

/* Syscon buttons for Antilag: SceCtrl samples the syscon's controls word
 * once per vblank, the syscon driver refreshes it far more often (measured
 * 2026-10-27: a press shows in the word 10 ms before SceCtrl's sample on
 * average, 17 ms at most). A 1 kHz kernel thread keeps the word and the time
 * it last changed, so the helper can hand the game a press early and stamp
 * it honestly. The read is the driver's cached word, a microsecond; the
 * thread idles at 5 Hz while no title is owned. The word starts as all
 * released (1s) and the status says NOSYSCON until a read has succeeded: the
 * helper inverts the word, so a 0 would read as every button down. Remapper
 * plugins (reVita) reshape SceCtrl's samples below the helper; the helper's
 * per-button table (companion/btnmerge.h) copes, verified 2026-10-28. */
static volatile uint32_t g_btn_word = ~0u;
static volatile uint64_t g_btn_changed;
static volatile int      g_btn_ok;      /* a syscon read has succeeded */
static volatile int      g_btn_run = 1;
static SceUID            g_btn_thread = -1;

/* Sticks: SceCtrl reads them with syscon command 0x0104 once per vblank
 * (probe, 2026-10-28). The same command from here every 4 ms, while a title
 * with Antilag is owned, gives the helper a stick value up to a vblank
 * fresher; the helper only uses it while it matches what SceCtrl reports a
 * vblank later (companion/main.c). Reply bytes 4-7 are lx ly rx ry. */
static FpsvitaSticks g_stk;
static uint32_t g_stk_errs;

/* Synchronous exchange with a STACK packet, exactly as Sony's own
 * ksceSysconReadCommand does (it builds the packet on its stack, calls
 * ksceSysconCmdExec, reads the reply, returns). From the SceSyscon
 * decompile (2026-10-29): CmdExec is
 * synchronous, it links the packet into syscon's global command queue and
 * UNLINKS it before returning, so a transient stack packet leaves nothing
 * behind. The earlier crashes used a STATIC packet (sync 28th, async 29th):
 * that one stayed linked in the queue and the reboot/power-off sequence, which
 * is also syscon-driven, tripped over it and left SceShell's database
 * half-written. CmdExec also refuses while the suspend flag is set and
 * requires thread context, both of which hold here. Command 0x101 is the
 * 10-byte form (controls word, then lx ly rx ry at reply data bytes 4-7),
 * the same the safe button read uses for 0x100. tx = cmd lo, cmd hi, length;
 * the driver appends the checksum. */
static int stkPoll(void)
{
	if (g_stk_errs >= 8 || g_stk_hold) return -1;   /* the bus said no, or the system is going down */
	SceSysconPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.tx[0] = 0x01; pkt.tx[1] = 0x01; pkt.tx[2] = 0x01;
	int r = ksceSysconCmdExec(&pkt, 0);
	if (r < 0) { g_stk.err = r; g_stk_errs++; return r; }
	if (pkt.rx[SCE_SYSCON_PACKET_RX_LENGTH] < 10) {
		g_stk.err = 0x7fff0000 | pkt.rx[SCE_SYSCON_PACKET_RX_LENGTH];
		g_stk_errs++;
		return -1;
	}
	uint32_t i = g_stk.n % FPSVITA_STK_RING;
	const unsigned char *d = &pkt.rx[SCE_SYSCON_PACKET_RX_DATA(0)];
	g_stk.at[i] = ksceKernelGetSystemTimeWide();
	g_stk.v[i][0] = d[4]; g_stk.v[i][1] = d[5]; g_stk.v[i][2] = d[6]; g_stk.v[i][3] = d[7];
	g_stk.n++;
	return 0;
}

static int btnThread(SceSize args, void *argp)
{
	unsigned tick = 0;
	(void)args; (void)argp;
	while (g_btn_run) {
		if (g_owner_pid < 0) {
			ksceKernelDelayThread(200000);
			continue;
		}
		SceUInt32 w = 0;
		if (ksceSysconGetControlsInfo(&w) >= 0) {
			if (w != g_btn_word) {
				g_btn_word = w;
				g_btn_changed = ksceKernelGetSystemTimeWide();
			}
			g_btn_ok = 1;
		}
		/* Early sticks: every other tick (~2 ms apart) while Antilag is on and
		 * a title is owned. Synchronous CmdExec with a stack packet, so no
		 * packet of ours lingers in syscon's queue across a reboot (the static
		 * and async forms both bricked, 2026-10-28/29; this stack form survived
		 * repeated reboot cycles, 2026-10-29). */
		if ((++tick & 1) == 0 && (g_adv_flags & FPSVITA_ADV_JIT))
			stkPoll();
		ksceKernelDelayThread(1000);
	}
	return 0;
}

int fpsvitaGetButtons(FpsvitaButtons *out)
{
	FpsvitaButtons b;
	int ret = -1;
	SYSCALL_ENTER
	/* Owner only, like the stats push: no raw syscon read for any process. */
	if (callerIsOwner() && g_btn_thread >= 0) {
		b.raw = g_btn_word;
		b.status = g_btn_ok ? 0 : FPSVITA_BTN_NOSYSCON;
		b.changed_at = g_btn_changed;
		b.now = ksceKernelGetSystemTimeWide();
		ret = ksceKernelCopyToUser(out, &b, sizeof(b));
	}
	SYSCALL_EXIT
	return ret;
}

int fpsvitaGetSticks(FpsvitaSticks *out)
{
	int ret = -1;
	SYSCALL_ENTER
	if (callerIsOwner() && g_btn_thread >= 0) {
		FpsvitaSticks s = g_stk;
		s.now = ksceKernelGetSystemTimeWide();
		ret = ksceKernelCopyToUser(out, &s, sizeof(s));
	}
	SYSCALL_EXIT
	return ret;
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
	g_sys_event = ksceKernelRegisterSysEventHandler("fpsvita_sysevent", sysEvent, NULL);
	g_btn_thread = ksceKernelCreateThread("fpsvita_btn", btnThread, 0x3C, 0x1000, 0, 0, NULL);
	if (g_btn_thread >= 0 && ksceKernelStartThread(g_btn_thread, 0, NULL) < 0) {
		ksceKernelDeleteThread(g_btn_thread);
		g_btn_thread = -1;
	}
	return SCE_KERNEL_START_SUCCESS;
}

int __module_stop(SceSize args, const void *argp)
{
	if (g_proc_event >= 0)
		ksceKernelUnregisterProcEventHandler(g_proc_event);
	if (g_sys_event >= 0)
		ksceKernelUnregisterSysEventHandler(g_sys_event);
	panelSet(0);
	if (g_btn_thread >= 0) {
		g_btn_run = 0;
		ksceKernelWaitThreadEnd(g_btn_thread, NULL, NULL);
		ksceKernelDeleteThread(g_btn_thread);
	}
	if (g_panel_lock >= 0)
		ksceKernelDeleteMutex(g_panel_lock);
	return SCE_KERNEL_STOP_SUCCESS;
}
