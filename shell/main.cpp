/*
 * FPSVita Shell UI (fpsvita-ui.suprx)
 *
 * Loaded under *main (SceShell). Hooks into the quick menu using the
 * same two-stage pattern as PSVshellPlus:
 *
 *   Stage 1: Hook sceAVConfigRegisterCallback (one-shot init)
 *            → Load RCO plugin, obtain impose root, read state.txt,
 *              write initial values to kernel module, hook stage 2
 *
 *   Stage 2: Hook paf::system::SupportsWiredEthernet (fires on
 *            each quick menu open)
 *            → Create Impose widgets via MainThreadCallList
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/libdbg.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/vshbridge.h>
#include <taihen.h>
#include <paf.h>

/* TAI_NEXT: C++-safe hook-chain call.
 * NOTE: parameter renamed to _tai_origfn to avoid collision with _tai_hook_user::func member.
 * Uses memcpy to legally convert void* → typed function pointer. */
#define TAI_NEXT(_tai_origfn, _tai_hookref, ...) \
  ({ \
    struct _tai_hook_user *_tai_cur = (struct _tai_hook_user *)(_tai_hookref); \
    struct _tai_hook_user *_tai_nxt = (struct _tai_hook_user *)_tai_cur->next; \
    void *_tai_raw = _tai_nxt ? _tai_nxt->func : _tai_cur->old; \
    __typeof__(&_tai_origfn) _tai_fn; \
    __builtin_memcpy(&_tai_fn, &_tai_raw, sizeof(void *)); \
    _tai_fn(__VA_ARGS__); \
  })

#include "global.h"
#include "impose.h"
#include "hud.h"
#include "../common/fpsvita_common.h"
#include <psp2/kernel/processmgr.h>

/* Minimal kubridge exception API (kubridge.h clashes with the paf_compat
 * sysmem types, so declare just what is used). Layout per kubridge.h. */
extern "C" {
typedef struct KuKernelExceptionContext {
	SceUInt32 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
	SceUInt32 sp, lr, pc;
	SceUInt64 vfpRegisters[32];
	SceUInt32 SPSR, FPSCR, FPEXC, FSR, FAR;
	SceUInt32 exceptionType;
} KuKernelExceptionContext;
typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext *);
/* SceKernelFreeMemorySizeInfo; psp2/kernel/sysmem.h clashes with paf_compat. */
typedef struct FpsvitaFreeMem {
	SceSize size;
	int size_user;
	int size_cdram;
	int size_phycont;
} FpsvitaFreeMem;
int sceKernelGetFreeMemorySize(FpsvitaFreeMem *info);
int kuKernelRegisterExceptionHandler(SceUInt32 exceptionType,
                                     KuKernelExceptionHandler pHandler,
                                     KuKernelExceptionHandler *pOldHandler,
                                     void *pOpt);
}

using namespace paf;

/* boot.log: a few milestones per boot, plus the crash handler's trace. */
void dbg_log(const char *msg, int len)
{
	SceUID fd = sceIoOpen("ur0:data/fpsvita/boot.log",
	                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, msg, len);
		sceIoClose(fd);
	}
}
#define DBG(msg) dbg_log(msg "\n", sizeof(msg "\n") - 1)

void dbg_hex(const char *tag, uint32_t val)
{
	static const char h[] = "0123456789abcdef";
	char buf[160];
	int n = 0;
	/* leave room for ": 0x" + 8 hex digits + '\n' */
	for (const char *t = tag; *t && n < (int)sizeof(buf) - 14; t++) buf[n++] = *t;
	buf[n++] = ':'; buf[n++] = ' '; buf[n++] = '0'; buf[n++] = 'x';
	for (int s = 28; s >= 0; s -= 4) buf[n++] = h[(val >> s) & 0xf];
	buf[n++] = '\n';
	dbg_log(buf, n);
}

/* Global state */
Plugin *g_fpsvitaPlugin = NULL;
ui::ScrollView *g_imposeRoot = NULL;
int32_t g_globalInterval = FPSVITA_INTERVAL_OFF;
int32_t g_hudMode = FPSVITA_HUD_OFF;
int32_t g_hudPos = 0;
int32_t g_lowLatency = 0;
int32_t g_jit = 0;
int32_t g_softcap = 0;
int32_t g_tear = 0;
static Framework *s_fwkInstance = NULL; /* cached during stage 1 */

static bool s_imposeOpened = false;

/* ------------------------------------------------------------------
 * State file reader (called once at init)
 * ------------------------------------------------------------------ */

/* Defined in impose.cpp next to the writer, sharing its read buffer. */
namespace fpsvita {
bool readGlobals(int32_t *interval, int32_t *inject, int32_t *autodetect,
                 int32_t *hud, int32_t *hudpos);
void writeConfigFromGlobals(int32_t interval, int32_t inject, int32_t autodetect);
}

/* ------------------------------------------------------------------
 * Plugin load callback
 * ------------------------------------------------------------------ */

static void pluginStartCb(Plugin *plugin)
{
	if (!plugin) {
		SCE_DBG_LOG_ERROR("[FPSVita] g_fpsvitaPlugin is NULL\n");
	}
	g_fpsvitaPlugin = plugin;
}

/* ------------------------------------------------------------------
 * Impose task: runs on main thread to create widgets
 * ------------------------------------------------------------------ */

/* Bounded retry: the quick menu may not have built its widget tree on the
 * first main-thread tick after the open hook fires. */
#define FPSVITA_IMPOSE_MAX_TICKS 60

static int s_imposeTicks = 0;

/* The live Impose, if any. Tracked so __module_stop can destroy it — an
 * Impose outliving the module leaves PAF calling freed code every tick. */
static fpsvita::Impose *s_impose = NULL;

static void imposeTask(void *arg);

static void hudBootTask(void *arg)
{
	common::MainThreadCallList::Unregister(hudBootTask, NULL);
	fpsvita::Hud::Apply();
}

/* Unregister first, THEN clear the flag. The reverse order lets the hook
 * thread re-register in the gap, only for this Unregister to remove the entry
 * it just added — leaving nothing registered and the flag stuck true, i.e.
 * the FPS row silently gone for the rest of the SceShell session. */
static void imposeTaskDone(bool reopen)
{
	common::MainThreadCallList::Unregister(imposeTask, NULL);
	s_imposeTicks = 0;
	if (reopen)
		s_imposeOpened = false;
}

static void imposeTask(void *arg)
{
	ui::Box *box = g_imposeRoot
		? (ui::Box *)g_imposeRoot->FindChild(0x0EE0C8AF, 0)
		: NULL;

	if (!box) {
		if (++s_imposeTicks < FPSVITA_IMPOSE_MAX_TICKS)
			return;
		/* Previously this returned without unregistering, so a menu whose
		 * box hash never resolves left this running every tick forever. */
		DBG("imposeTask: impose box not found - giving up");
		imposeTaskDone(true);
		return;
	}

	if (!g_fpsvitaPlugin) {
		DBG("imposeTask: RCO plugin not loaded");
		imposeTaskDone(true);
		return;
	}

	fpsvita::Impose *impose = new fpsvita::Impose(g_fpsvitaPlugin, box);
	if (!impose || !impose->IsValid()) {
		DBG("imposeTask: Impose init failed");
		delete impose;          /* ~Impose calls fpsvitaImposeClosed() */
		imposeTaskDone(false);
		return;
	}

	s_impose = impose;

	/* s_imposeOpened stays true for the life of the Impose; ~Impose clears it
	 * via fpsvitaImposeClosed() when the menu closes. */
	imposeTaskDone(false);
}

void fpsvitaImposeClosed(void)
{
	s_impose = NULL;
	s_imposeOpened = false;
}

/* ------------------------------------------------------------------
 * coreSetImpose: called when quick menu opens
 * ------------------------------------------------------------------ */

static void coreSetImpose(bool enable)
{
	if (enable && !s_imposeOpened) {
		common::MainThreadCallList::Register(imposeTask, NULL);
		s_imposeOpened = true;
	}
}

/* ------------------------------------------------------------------
 * coreInitImpose: obtain the quick menu root widget
 *
 * Exact sequence from PSVshellPlus main.cpp coreInitImpose().
 * The impose root is obtained via power_manage_plugin's interface
 * at vtable offset +0x54.
 * ------------------------------------------------------------------ */

/* User-space pointers on the Vita live at 0x81000000 and above, so a plain
 * ">= 0x80000000" test rejects every valid pointer. SCE error codes are
 * 0x80XXXXXX. */
static inline bool isSceError(uint32_t v)
{
	return (v & 0xFF000000u) == 0x80000000u;
}

static void coreInitImpose()
{
	SCE_DBG_LOG_INFO("[FPSVita] coreInitImpose\n");

	Plugin *powerManagePlugin = Plugin::Find("power_manage_plugin");
	if ((!powerManagePlugin || isSceError((uint32_t)powerManagePlugin))
	    && s_fwkInstance) {
		powerManagePlugin = s_fwkInstance->FindPlugin("power_manage_plugin");
		dbg_hex("pmp_FindPlugin", (uint32_t)powerManagePlugin);
	}
	if (!powerManagePlugin || isSceError((uint32_t)powerManagePlugin)) {
		DBG("coreInitImpose: plugin not found");
		return;
	}

	void *powerIf = powerManagePlugin->GetInterface(1);
	if (!powerIf) {
		DBG("coreInitImpose: GetInterface NULL");
		return;
	}

	ui::ScrollView *(*getImposeRoot)();
	getImposeRoot = (ui::ScrollView *(*)()) *(int *)((int)powerIf + 0x54);
	if (!getImposeRoot) {
		DBG("coreInitImpose: getImposeRoot NULL");
		return;
	}

	g_imposeRoot = getImposeRoot();
	if (!g_imposeRoot) {
		DBG("coreInitImpose: imposeRoot NULL");
		return;
	}

	SCE_DBG_LOG_INFO("[FPSVita] coreInitImpose OK\n");
	DBG("coreInitImpose: OK");
}

/* Exported ScePaf entry points bound by name so they can take the raw
 * InitParam buffer (see coreInitPlugin). Same symbols as in ScePafRE.yml. */
extern "C" void pafPluginInitParamCtor(void *self)
    __asm__("_ZN3paf6Plugin9InitParamC1Ev");
extern "C" void pafStringCtor(void *self, const char *str)
    __asm__("_ZN3paf6StringC1EPKc");
extern "C" void pafPluginLoadSync(const void *initParam,
                                  void (*finishCb)(Plugin *), int32_t option)
    __asm__("_ZN3paf6Plugin8LoadSyncERKNS0_9InitParamEPFvPS0_Ei");

/* ------------------------------------------------------------------
 * coreInitPlugin: load our RCO as a PAF plugin
 * ------------------------------------------------------------------ */

static void coreInitPlugin()
{
	SCE_DBG_LOG_INFO("[FPSVita] coreInitPlugin\n");

	/* Plugin::InitParam is built as a raw 0x94-byte buffer, not as the
	 * ScePaf-RE class: paf_compat swaps paf::map for std::map, which makes
	 * the class larger than the firmware's and shifts every field after
	 * 'arguments'; and ScePaf-RE's string assign() would sce_paf_free the
	 * firmware's static empty-string sentinel that the exported ctor
	 * installs. Field offsets per ScePaf-RE plugin.h (string = 12 bytes):
	 *    0 name   12 caller_name   32 start_func   44 resource_file */
	unsigned char pip[0x94];
	pafPluginInitParamCtor(pip);
	pafStringCtor(pip + 0,  "fpsvita_plugin");
	pafStringCtor(pip + 12, "__main__");
	pafStringCtor(pip + 44, FPSVITA_RCO_FILE);
	*reinterpret_cast<void (**)(Plugin *)>(pip + 32) = pluginStartCb;

	/* Cached for coreInitImpose's FindPlugin fallback. */
	s_fwkInstance = Framework::Instance();

	pafPluginLoadSync(pip, NULL, 0);

	SCE_DBG_LOG_INFO("[FPSVita] coreInitPlugin OK\n");
}

/* ------------------------------------------------------------------
 * Hook stage 2: paf::system::SupportsWiredEthernet
 *
 * Fires each time the quick menu (impose dialog) is opened.
 * We register a MainThreadCallList task to create our widgets.
 *
 * Library NID: 0x3D643CE8 (ScePafMisc)
 * Function NID: 0xB7CFFF5C
 * ------------------------------------------------------------------ */

static tai_hook_ref_t s_hookRef[2];
static SceUID s_hookId[2] = { -1, -1 };

static bool supportsWiredEthernet_patched()
{
	if (!g_imposeRoot)
		coreInitImpose();
	if (g_imposeRoot)
		coreSetImpose(true);
	return TAI_NEXT(supportsWiredEthernet_patched, s_hookRef[1]);
}

/* ------------------------------------------------------------------
 * Hook stage 1: sceAVConfigRegisterCallback
 *
 * One-shot init hook. Fires during SceShell startup.
 * Loads the RCO plugin, obtains the impose root, reads state.txt,
 * pushes initial values to the kernel module, hooks stage 2, then
 * releases itself.
 *
 * Library NID: 0x79E0F03F (SceAVConfig)
 * Function NID: 0xFB5E3E74
 * ------------------------------------------------------------------ */

static volatile int32_t s_stage1Once = 0;


static SceInt32 sceAVConfigRegisterCallback_patched(SceUID cbid, SceInt32 a2)
{
	/* One-shot. Nothing guarantees SceShell registers exactly one AV config
	 * callback, or that it does so from a single thread. A second entry must
	 * not re-run coreInitPlugin, overwrite s_hookId[1] (leaking the first
	 * stage-2 hook id), or double-release s_hookId[0].
	 * Chaining via s_hookRef[0] is still valid here: once the release below
	 * completes the function is unpatched, so a later call never reaches us. */

	if (__sync_lock_test_and_set(&s_stage1Once, 1)) {
		DBG("stage1 cb re-entered - init skipped");
		return TAI_NEXT(sceAVConfigRegisterCallback_patched, s_hookRef[0],
		                cbid, a2);
	}

	DBG("stage1 cb entered");

	/* Load RCO plugin */
	coreInitPlugin();

	/* Read persisted state and push to kernel module */
	int32_t interval = FPSVITA_INTERVAL_OFF, inject = FPSVITA_INJECT_OFF,
	        autodetect = FPSVITA_AUTODETECT_ON, hud = FPSVITA_HUD_OFF, hudpos = 0;
	bool haveConfig = fpsvita::readGlobals(&interval, &inject, &autodetect, &hud, &hudpos);
	g_globalInterval = interval;
	fpsvitaSetInterval(interval);
	fpsvitaSetInject(inject);
	fpsvitaSetAutodetect(autodetect);
	g_hudMode = (hud >= 0 && hud < FPSVITA_HUD_MODES) ? hud : FPSVITA_HUD_OFF;
	g_hudPos = (hudpos >= 0 && hudpos < FPSVITA_HUD_POS_COUNT) ? hudpos : 0;
	fpsvitaSetHud(g_hudMode, g_hudPos);
	if (g_softcap < FPSVITA_SOFTCAP_MIN || g_softcap > FPSVITA_SOFTCAP_MAX)
		g_softcap = 0;
	fpsvitaSetAdvanced((g_lowLatency ? FPSVITA_ADV_LOWLAT : 0) | (g_jit ? FPSVITA_ADV_JIT : 0)
	                   | (g_tear ? FPSVITA_ADV_TEAR : 0), g_softcap);
	if (!haveConfig) {
		/* Fresh install: write the defaults so the file exists to edit. */
		fpsvita::writeConfigFromGlobals(interval, inject, autodetect);
		DBG("config.txt written");
	}
	{
		char line[96];
		int n = sce_paf_snprintf(line, sizeof(line),
			"stage1 done: interval=%d inject=%d autodetect=%d hud=%d pos=%d\n",
			(int)interval, (int)inject, (int)autodetect, (int)g_hudMode, (int)g_hudPos);
		if (n > 0 && n < (int)sizeof(line))
			dbg_log(line, n);
	}

	/* The overlay is PAF work; build it from the main thread, once. */
	if (g_hudMode != FPSVITA_HUD_OFF)
		common::MainThreadCallList::Register(hudBootTask, NULL);

	/* Hook stage 2: impose open detection */
	s_hookId[1] = taiHookFunctionImport(
		&s_hookRef[1],
		"SceShell",
		0x3D643CE8, /* ScePafMisc */
		0xB7CFFF5C, /* paf::system::SupportsWiredEthernet */
		supportsWiredEthernet_patched);

	/* Call original before releasing — TAI_NEXT uses hookRef which is
	 * invalidated by taiHookRelease. Release after return value captured. */
	SceInt32 ret = TAI_NEXT(sceAVConfigRegisterCallback_patched, s_hookRef[0],
	                        cbid, a2);
	SceUID id = s_hookId[0];
	s_hookId[0] = -1;              /* before release, so __module_stop cannot
	                                * release the same UID a second time */
	taiHookRelease(id, s_hookRef[0]);
	return ret;
}

/* ------------------------------------------------------------------
 * Module lifecycle
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * Crash trace. kubridge lets a user module see its process's faults.
 * Log the registers to boot.log, then hand the fault to whoever was
 * registered before us so the normal crash handling still happens.
 * Runs on the faulting thread's stack: keep it small, take no locks.
 * ------------------------------------------------------------------ */
extern "C" int __module_start(SceSize args, const void *argp);

static KuKernelExceptionHandler s_prevExcHandler[3];

/* Static so the exception handler keeps its stack use small. */
static SceUID s_modList[256];
static SceKernelModuleInfo s_modInfo;

static void logModuleFor(const char *tag, uint32_t addr)
{
	SceSize num = sizeof(s_modList) / sizeof(s_modList[0]);
	if (sceKernelGetModuleList(0xFF, s_modList, &num) < 0)
		return;
	for (SceSize i = 0; i < num; i++) {
		s_modInfo.size = sizeof(s_modInfo);
		if (sceKernelGetModuleInfo(s_modList[i], &s_modInfo) < 0)
			continue;
		for (int seg = 0; seg < 4; seg++) {
			uint32_t base = (uint32_t)s_modInfo.segments[seg].vaddr;
			uint32_t size = s_modInfo.segments[seg].memsz;
			if (size && addr >= base && addr < base + size) {
				dbg_log(tag, (int)sce_paf_strlen(tag));
				dbg_log(" in ", 4);
				dbg_log(s_modInfo.module_name,
				        (int)sce_paf_strlen(s_modInfo.module_name));
				dbg_hex(" seg", (uint32_t)seg);
				dbg_hex(" base", base);
				dbg_hex(" offset", addr - base);
				return;
			}
		}
	}
	dbg_hex("no module for", addr);
}

static void crashHandler(KuKernelExceptionContext *c)
{
	DBG("=== CRASH ===");
	dbg_hex("exc_type", c->exceptionType);
	dbg_hex("pc", c->pc);
	dbg_hex("lr", c->lr);
	dbg_hex("sp", c->sp);
	dbg_hex("far", c->FAR);
	dbg_hex("fsr", c->FSR);
	dbg_hex("r0", c->r0);
	dbg_hex("r1", c->r1);
	dbg_hex("r2", c->r2);
	dbg_hex("r3", c->r3);
	dbg_hex("r4", c->r4);
	dbg_hex("r12", c->r12);
	dbg_hex("spsr", c->SPSR);
	dbg_hex("fpsvita_ui_module_start", (uint32_t)&__module_start);
	logModuleFor("pc", c->pc);
	logModuleFor("lr", c->lr);
	uint32_t t = c->exceptionType;
	if (t < 3 && s_prevExcHandler[t])
		s_prevExcHandler[t](c);
	else
		sceKernelExitProcess(0);
}

static void installCrashHandler()
{
	for (uint32_t t = 0; t < 3; t++) {
		int r = kuKernelRegisterExceptionHandler(t, crashHandler,
		                                         &s_prevExcHandler[t], NULL);
		if (r < 0)
			dbg_hex("kuKernelRegisterExceptionHandler failed", (uint32_t)r);
	}
}

extern "C" {

int __module_start(SceSize args, const void *argp)
{
	sceIoMkdir("ur0:data/fpsvita", 0777);
	DBG("module_start entered");

	/* Verify RCO exists */
	SceUID fd = sceIoOpen(FPSVITA_RCO_FILE, SCE_O_RDONLY, 0);
	if (fd < 0) {
		DBG("RCO not found - bailing");
		return SCE_KERNEL_START_NO_RESIDENT;
	}
	sceIoClose(fd);

	/* Verify kernel module is loaded */
	SceInt32 opt[2] = {0, 0};
	if (_vshKernelSearchModuleByName("fpsvita-kernel", (const void *)opt) <= 0) {
		DBG("kernel module not found - bailing");
		return SCE_KERNEL_START_NO_RESIDENT;
	}



	/* Process memory headroom: SceShell under LowMemMode has little. */
	FpsvitaFreeMem mem;
	mem.size = sizeof(mem);
	if (sceKernelGetFreeMemorySize(&mem) >= 0)
		dbg_hex("free_main", (uint32_t)mem.size_user);
	installCrashHandler();

	/* Hook stage 1: sceAVConfigRegisterCallback (one-shot init) */
	s_hookId[0] = taiHookFunctionImport(
		&s_hookRef[0],
		"SceShell",
		0x79E0F03F, /* SceAVConfig */
		0xFB5E3E74, /* sceAVConfigRegisterCallback */
		sceAVConfigRegisterCallback_patched);
	DBG("stage1 hook installed");

	return SCE_KERNEL_START_SUCCESS;
}

int __module_stop(SceSize args, const void *argp)
{
	/*
	 * Previously a bare return. Everything below outlives the module
	 * otherwise: SceShell keeps a hook branching into unmapped memory and
	 * keeps calling freed main-thread tasks every frame. That made a plugin
	 * reload a guaranteed crash, and forced a reboot per test cycle.
	 *
	 * Stage 2 goes first — once it is gone no new imposeTask can be queued.
	 */
	if (s_hookId[1] >= 0) {
		taiHookRelease(s_hookId[1], s_hookRef[1]);
		s_hookId[1] = -1;
	}
	if (s_hookId[0] >= 0) {
		taiHookRelease(s_hookId[0], s_hookRef[0]);
		s_hookId[0] = -1;
	}

	/* Drop any queued task, then the live widget row (~Impose unregisters
	 * its own Update and clears s_imposeOpened). */
	common::MainThreadCallList::Unregister(imposeTask, NULL);
	delete s_impose;
	s_impose = NULL;
	s_imposeOpened = false;
	common::MainThreadCallList::Unregister(hudBootTask, NULL);
	fpsvita::Hud::Shutdown();

	return SCE_KERNEL_STOP_SUCCESS;
}

void __module_exit(void)
{
}

} /* extern "C" */
