/*
 * FPSVita Shell UI — Impose (Quick Menu) Integration
 *
 * Creates a single-row framecap control in the PS Vita quick menu:
 *   FPS: [<] 30fps (n:60) [>]  [Save]
 *
 * When autodetect is off, inject checkbox appears alongside save.
 * Save button writes the current interval as a per-game override.
 * Native interval (n:XX) shows the game's original vsync setting.
 *
 * Widgets are created on every quick menu open and destroyed on close,
 * matching PSVshellPlus's lifecycle pattern exactly.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/libdbg.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <paf.h>

#include "global.h"
#include "impose.h"
#include "hud.h"
#include "../common/fpsvita_common.h"

using namespace paf;

namespace fpsvita {

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */


/* "Default" rather than "Off": on PC and the Steam Deck "limit off" means
 * uncapped, which here is "Uncapped". "Tearing" is Uncapped with every flip
 * IMMEDIATE through the kernel (FPSVITA_ADV_TEAR), the Deck's "Allow Tearing". */
static const wchar_t *getModeString(int32_t interval, bool tear)
{
	switch (interval) {
	case FPSVITA_INTERVAL_OFF:      return L"Default";
	case FPSVITA_INTERVAL_60FPS:    return L"60fps";
	case FPSVITA_INTERVAL_30FPS:    return L"30fps";
	case FPSVITA_INTERVAL_20FPS:    return L"20fps";
	case FPSVITA_INTERVAL_UNCAPPED: return tear ? L"Tearing" : L"Uncapped";
	default:                        return L"???";
	}
}

/* The FPS row cycles the interval table plus one Tearing entry at the end. */
#define FPSVITA_UI_CYCLE_LEN ((int32_t)FPSVITA_CYCLE_LEN + 1)

/* ------------------------------------------------------------------
 * Read titleid from kernel module (3 x uint32_t parts → string)
 * ------------------------------------------------------------------ */

void Impose::ReadTitleId()
{
	uint32_t parts[3];
	parts[0] = fpsvitaGetTitleIdPart(0);
	parts[1] = fpsvitaGetTitleIdPart(1);
	parts[2] = fpsvitaGetTitleIdPart(2);
	sce_paf_memset(titleid, 0, sizeof(titleid));
	sce_paf_memcpy(titleid, parts, sizeof(titleid));
	titleid[FPSVITA_TITLEID_LEN - 1] = '\0';

	/* Game is running if titleid is non-empty */
	hasGame = (titleid[0] != '\0');
}

/* ------------------------------------------------------------------
 * Cycle navigation
 * ------------------------------------------------------------------ */

int32_t Impose::FindCycleIndex(int32_t interval)
{
	for (uint32_t i = 0; i < FPSVITA_CYCLE_LEN; i++) {
		if (fpsvita_cycle[i] == interval)
			return (int32_t)i;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * State file I/O
 *
 * config.txt holds the globals and is regenerated whole by the shell:
 *   limit=<n>       -1 Default, 1 60fps, 2 30fps, 3 20fps, 0 Uncapped
 *   vsync=auto|0|1  forced vsync wait (auto = detected)
 *   hud=, hudpos=, lowlat=, antilag=, limiter=<fps>, tearing=
 * state.txt holds one "TITLEID=<limit>,lowlat=,antilag=,limiter=,tearing="
 * line per title. SavePerGame replaces or appends this title's line and
 * keeps every other line verbatim, so a line this version does not know
 * about survives a rewrite. It refuses to write a file it could not read
 * whole: rewriting from a partial read with O_TRUNC once truncated a file
 * and silently destroyed profiles.
 *
 * These buffers are static rather than stack: state.txt can be 4 KB and this
 * code runs only on the PAF main thread.
 * ------------------------------------------------------------------ */

static char s_readBuf[FPSVITA_STATE_BUF];
static char s_rebuilt[FPSVITA_STATE_BUF];

/* Whole file into s_readBuf. Bytes read; -1 missing or empty; -2 when it
 * does not fit, in which case the caller must not rewrite it: rewriting from
 * a partial read with O_TRUNC is how a 2433-byte file once became 1530
 * bytes, silently destroying per-game overrides. */
static int readWhole(const char *path)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	sce_paf_memset(s_readBuf, 0, sizeof(s_readBuf));
	int bytes = (int)sceIoRead(fd, s_readBuf, sizeof(s_readBuf) - 1);
	sceIoClose(fd);
	if (bytes <= 0)
		return -1;
	if (bytes >= (int)sizeof(s_readBuf) - 1)
		return -2;
	return bytes;
}

static bool writeWhole(const char *path, const char *data, int len)
{
	sceIoMkdir(FPSVITA_STATE_DIR, 0777);
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return false;
	if (len > 0)
		sceIoWrite(fd, data, len);
	sceIoClose(fd);
	return true;
}

/* config.txt: the globals, regenerated whole. Nothing to preserve. */
static void writeConfigFile(int32_t interval, int32_t inject, int32_t autodetect)
{
	char buf[192], vsync[8];
	if (autodetect)
		sce_paf_snprintf(vsync, sizeof(vsync), "auto");
	else
		sce_paf_snprintf(vsync, sizeof(vsync), "%d", (int)inject);
	int len = sce_paf_snprintf(buf, sizeof(buf),
	                            "limit=%d\nvsync=%s\nhud=%d\nhudpos=%d\n"
	                            "lowlat=%d\nantilag=%d\nlimiter=%d\ntearing=%d\n",
	                            interval, vsync, g_hudMode, g_hudPos,
	                            g_lowLatency, g_jit, g_softcap, g_tear);
	if (len > 0 && len < (int)sizeof(buf))
		writeWhole(FPSVITA_CONFIG_FILE, buf, len);
}

/*
 * Append "TITLEID=limit,lowlat=,antilag=,limiter=,tearing=". Returns false if it would not fit.
 *
 * The old code did `outpos += sce_paf_snprintf(out + outpos, remaining, ...)`
 * with no clamp. Under C99 semantics snprintf returns the length it WOULD
 * have written, so a truncating call pushed outpos past the buffer and the
 * following sceIoWrite read off the end of it. Bounds-checking the copy is
 * correct under either return convention.
 */
static bool appendPerGameLine(char *out, int *outpos, int outsize,
                               const char *titleid, int32_t interval,
                               int32_t advFlags, int32_t softcap)
{
	char line[80];
	int n = sce_paf_snprintf(line, sizeof(line), "%s=%d,lowlat=%d,antilag=%d,limiter=%d,tearing=%d\n",
	                         titleid, interval,
	                         (advFlags & FPSVITA_ADV_LOWLAT) ? 1 : 0,
	                         (advFlags & FPSVITA_ADV_JIT) ? 1 : 0, softcap,
	                         (advFlags & FPSVITA_ADV_TEAR) ? 1 : 0);
	if (n <= 0 || n >= (int)sizeof(line))
		return false;
	if (*outpos + n >= outsize)
		return false;

	sce_paf_memcpy(out + *outpos, line, n);
	*outpos += n;
	return true;
}

void Impose::PersistState()
{
	/* Only reached with no game running, where the value on screen IS the
	 * global one. */
	if (!hasGame)
		g_globalInterval = currentInterval;

	writeConfigFile(g_globalInterval, currentInject, autodetect);
}

/* state.txt: this title's line replaced or appended, every other title's
 * line kept verbatim, config keys and legacy junk dropped. */
void Impose::SavePerGame()
{
	if (!hasGame)
		return;

	int tidlen = 0;
	while (tidlen < FPSVITA_TITLEID_LEN && titleid[tidlen])
		tidlen++;
	if (tidlen == 0)
		return;

	int bytes = readWhole(FPSVITA_STATE_FILE);
	if (bytes == -2)
		return;   /* cannot see the whole file: better no write than a partial one */
	if (bytes < 0)
		bytes = 0;

	sce_paf_memset(s_rebuilt, 0, sizeof(s_rebuilt));
	int outpos = 0;
	bool found = false;

	char *p = s_readBuf;
	while (*p && p < s_readBuf + bytes) {
		char *eol = p;
		while (*eol && *eol != '\n') eol++;
		int linelen = (int)(eol - p);

		bool match = false;
		if (linelen > tidlen + 1 && p[tidlen] == '=') {
			match = true;
			for (int i = 0; i < tidlen; i++) {
				if (p[i] != titleid[i]) {
					match = false;
					break;
				}
			}
		}

		if (match) {
			if (!appendPerGameLine(s_rebuilt, &outpos, sizeof(s_rebuilt),
			                        titleid, currentInterval, advFlags, softcap))
				return;
			found = true;
		} else if (linelen > 0) {
			if (outpos + linelen + 1 >= (int)sizeof(s_rebuilt))
				return;
			sce_paf_memcpy(s_rebuilt + outpos, p, linelen);
			outpos += linelen;
			s_rebuilt[outpos++] = '\n';
		}

		p = (*eol == '\n') ? eol + 1 : eol;
		if (p == eol) break;
	}

	if (!found && !appendPerGameLine(s_rebuilt, &outpos, sizeof(s_rebuilt),
	                                  titleid, currentInterval, advFlags, softcap))
		return;

	writeWhole(FPSVITA_STATE_FILE, s_rebuilt, outpos);
}

/*
 * Boot-time read of the globals from config.txt. Returns false when the file
 * is missing (fresh install) so the caller writes one with the defaults.
 * Missing keys keep their defaults.
 */
bool readGlobals(int32_t *interval, int32_t *inject, int32_t *autodetect,
                 int32_t *hud, int32_t *hudpos)
{
	int bytes = readWhole(FPSVITA_CONFIG_FILE);
	if (bytes < 0)
		return false;

	char *p = s_readBuf;
	while (*p) {
		char *eol = p;
		while (*eol && *eol != '\n') eol++;
		char *eq = p;
		while (eq < eol && *eq != '=') eq++;
		if (*eq == '=') {
			int klen = (int)(eq - p);
			const char *val = eq + 1;
			if (klen == 5 && sce_paf_strncmp(p, "limit", 5) == 0)
				*interval = (int32_t)sce_paf_strtol(val, NULL, 10);
			else if (klen == 5 && sce_paf_strncmp(p, "vsync", 5) == 0) {
				*autodetect = (*val == 'a') ? 1 : 0;   /* "auto", else forced 0/1 */
				*inject = *autodetect ? 0 : ((int32_t)sce_paf_strtol(val, NULL, 10) ? 1 : 0);
			}
			else if (klen == 3 && sce_paf_strncmp(p, "hud", 3) == 0)
				*hud = (int32_t)sce_paf_strtol(val, NULL, 10);
			else if (klen == 6 && sce_paf_strncmp(p, "hudpos", 6) == 0)
				*hudpos = (int32_t)sce_paf_strtol(val, NULL, 10);
			else if (klen == 6 && sce_paf_strncmp(p, "lowlat", 6) == 0)
				g_lowLatency = sce_paf_strtol(val, NULL, 10) ? 1 : 0;
			else if (klen == 7 && sce_paf_strncmp(p, "antilag", 7) == 0)
				g_jit = sce_paf_strtol(val, NULL, 10) ? 1 : 0;
			else if (klen == 7 && sce_paf_strncmp(p, "limiter", 7) == 0)
				g_softcap = (int32_t)sce_paf_strtol(val, NULL, 10);
			else if (klen == 7 && sce_paf_strncmp(p, "tearing", 7) == 0)
				g_tear = sce_paf_strtol(val, NULL, 10) ? 1 : 0;
		}
		if (*eol != '\n') break;
		p = eol + 1;
	}
	return true;
}

/* Write config.txt from the globals as they are now (fresh install). */
void writeConfigFromGlobals(int32_t interval, int32_t inject, int32_t autodetect)
{
	writeConfigFile(interval, inject, autodetect);
}

/* ------------------------------------------------------------------
 * UI update helpers
 * ------------------------------------------------------------------ */

void Impose::UpdateModeLabel()
{
	wchar_t buf[32];
	int nfps = fpsvitaIntervalToFps(nativeInterval);

	if (!modeText)
		return;

	bool tear = (advFlags & FPSVITA_ADV_TEAR) != 0;
	if (nfps > 0) {
		/* With the rate the game asked for itself: "30fps (game 60)" */
		sce_paf_swprintf(buf, sizeof(buf) / sizeof(wchar_t),
		                  L"%ls (game %d)", getModeString(currentInterval, tear),
		                  nfps);
	} else {
		/* No native detected yet */
		sce_paf_swprintf(buf, sizeof(buf) / sizeof(wchar_t),
		                  L"%ls", getModeString(currentInterval, tear));
	}

	paf::wstring wstr(buf);
	fpsvita_textSetString(modeText, wstr, 0);
}

void Impose::UpdateInjectState()
{
	if (!injectBox || !injectLabel)
		return;

	if (autodetect == FPSVITA_AUTODETECT_ON) {
		widgetHide(injectBox);
		widgetHide(injectLabel);
		return;
	}

	widgetShow(injectBox);
	widgetShow(injectLabel);

	if (currentInterval == FPSVITA_INTERVAL_OFF ||
	    currentInterval == FPSVITA_INTERVAL_UNCAPPED) {
		injectBox->Disable(true);
		if (currentInject == FPSVITA_INJECT_ON) {
			injectBox->SetCheck(false);
			currentInject = FPSVITA_INJECT_OFF;
			fpsvitaSetInject(FPSVITA_INJECT_OFF);
		}
	} else {
		injectBox->Enable(true);
	}
}

/* ------------------------------------------------------------------
 * Button callbacks
 * ------------------------------------------------------------------ */

/* One callback per arrow. Deciding the direction from the callback's
 * widget argument (GetName().GetIDHash()) read a field at a ScePaf-RE
 * assumed offset and always fell through to "next". */
void Impose::ModeDownCBFun(int32_t type, ui::Handler *self,
                            ui::Event *e, void *userdata)
{
	((Impose *)userdata)->CycleMode(-1);
}

void Impose::ModeUpCBFun(int32_t type, ui::Handler *self,
                          ui::Event *e, void *userdata)
{
	((Impose *)userdata)->CycleMode(+1);
}

void Impose::CycleMode(int32_t dir)
{
	Impose *obj = this;
	int32_t idx = obj->FindCycleIndex(obj->currentInterval);
	if (obj->currentInterval == FPSVITA_INTERVAL_UNCAPPED && (obj->advFlags & FPSVITA_ADV_TEAR))
		idx = FPSVITA_UI_CYCLE_LEN - 1;

	if (dir < 0)
		idx = (idx == 0) ? FPSVITA_UI_CYCLE_LEN - 1 : idx - 1;
	else
		idx = (idx + 1) % FPSVITA_UI_CYCLE_LEN;

	bool tear = idx == FPSVITA_UI_CYCLE_LEN - 1;
	obj->currentInterval = tear ? FPSVITA_INTERVAL_UNCAPPED : fpsvita_cycle[idx];
	fpsvitaSetInterval(obj->currentInterval);
	int32_t flags = tear ? (obj->advFlags | FPSVITA_ADV_TEAR) : (obj->advFlags & ~FPSVITA_ADV_TEAR);
	if (flags != obj->advFlags) {
		obj->advFlags = flags;
		obj->ApplyAdvanced();   /* live, and persisted with the globals when no game runs */
	}

	/*
	 * With a game running this is that game's active interval; the player
	 * commits it with Save, which writes a per-game override.
	 *
	 * With no game running there is nothing to override — the value on screen
	 * IS the global setting, so track and persist it. Otherwise the change is
	 * silently discarded the moment a game launches, because the helper
	 * snapshots the kernel interval and restores it on exit.
	 */
	if (!obj->hasGame) {
		g_globalInterval = obj->currentInterval;
		obj->PersistState();
	}

	obj->UpdateModeLabel();
	obj->UpdateInjectState();
}

void Impose::HudDownCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleHud(-1); }
void Impose::HudUpCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleHud(+1); }
void Impose::HudPosDownCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleHudPos(-1); }
void Impose::HudPosUpCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleHudPos(+1); }

/* Overlay mode and position are global settings: persisted at once, with or
 * without a game running, and applied live. */
void Impose::CycleHud(int32_t dir)
{
	g_hudMode = (g_hudMode + FPSVITA_HUD_MODES + dir) % FPSVITA_HUD_MODES;
	Hud::Apply();
	PersistState();
	UpdateHudLabels();
}

void Impose::CycleHudPos(int32_t dir)
{
	g_hudPos = (g_hudPos + FPSVITA_HUD_POS_COUNT + dir) % FPSVITA_HUD_POS_COUNT;
	Hud::Apply();
	PersistState();
	UpdateHudLabels();
}

void Impose::UpdateHudLabels()
{
	static const wchar_t *const modes[FPSVITA_HUD_MODES] = { L"Off", L"Mini", L"Full" };
	static const wchar_t *const poss[FPSVITA_HUD_POS_COUNT] = { L"TL", L"TR", L"BL", L"BR" };
	textSet(hudText, modes[(g_hudMode >= 0 && g_hudMode < FPSVITA_HUD_MODES) ? g_hudMode : 0]);
	textSet(hudPosText, poss[(g_hudPos >= 0 && g_hudPos < FPSVITA_HUD_POS_COUNT) ? g_hudPos : 0]);
}

/* Advanced row. With a game running the change is live (kernel) and is
 * written to the per-game line by Save; with none it edits the globals and
 * persists at once, like the FPS row.
 *
 * Checkbox state is tracked here, not read back: ScePaf-RE's IsChecked() is
 * an inline field read at an assumed offset. SetCheck() (a real export) sets
 * the initial state, and the box toggles itself on every decide, so flipping
 * our flag per callback stays in step with it. */
void Impose::LowlatCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{
	Impose *obj = (Impose *)userdata;
	obj->advFlags ^= FPSVITA_ADV_LOWLAT;
	obj->ApplyAdvanced();
}

void Impose::JitCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{
	Impose *obj = (Impose *)userdata;
	obj->advFlags ^= FPSVITA_ADV_JIT;
	obj->ApplyAdvanced();
}

void Impose::SoftcapDownCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleSoftcap(-1); }
void Impose::SoftcapUpCBFun(int32_t type, ui::Handler *self, ui::Event *e, void *userdata)
{ ((Impose *)userdata)->CycleSoftcap(+1); }

void Impose::CycleSoftcap(int32_t dir)
{
	int32_t n = (int32_t)FPSVITA_SOFTCAP_CYCLE_LEN;
	int32_t idx = (fpsvitaSoftcapIndex(softcap) + n + dir) % n;
	softcap = fpsvita_softcap_cycle[idx];
	UpdateSoftcapLabel();
	ApplyAdvanced();
}

void Impose::ApplyAdvanced()
{
	fpsvitaSetAdvanced(advFlags, softcap);
	if (!hasGame) {
		g_lowLatency = (advFlags & FPSVITA_ADV_LOWLAT) ? 1 : 0;
		g_jit = (advFlags & FPSVITA_ADV_JIT) ? 1 : 0;
		g_tear = (advFlags & FPSVITA_ADV_TEAR) ? 1 : 0;
		g_softcap = softcap;
		PersistState();
	}
}

void Impose::UpdateSoftcapLabel()
{
	wchar_t buf[8];
	if (softcap > 0)
		sce_paf_swprintf(buf, 8, L"%d", (int)softcap);
	else
		sce_paf_swprintf(buf, 8, L"off");
	textSet(softcapText, buf);
}

void Impose::InjectCBFun(int32_t type, ui::Handler *self,
                          ui::Event *e, void *userdata)
{
	Impose *obj = (Impose *)userdata;
	obj->currentInject = (obj->currentInject == FPSVITA_INJECT_ON) ? FPSVITA_INJECT_OFF
	                                                               : FPSVITA_INJECT_ON;
	fpsvitaSetInject(obj->currentInject);
}

void Impose::SaveCBFun(int32_t type, ui::Handler *self,
                        ui::Event *e, void *userdata)
{
	Impose *obj = (Impose *)userdata;

	if (obj->hasGame) {
		/* Save per-game override (also writes global state) */
		obj->SavePerGame();
	} else {
		/* No game running — save global state only */
		obj->PersistState();
	}
}

/* ------------------------------------------------------------------
 * Periodic update (menu close detection)
 * ------------------------------------------------------------------ */

void Impose::Update(void *arg)
{
	Impose *obj = (Impose *)arg;

	/* The impose box disappearing means the quick menu closed. */
	if (!g_imposeRoot || !g_imposeRoot->FindChild(0x0EE0C8AF)) {
		delete obj;
		return;
	}
}

/* ------------------------------------------------------------------
 * Constructor: create widgets, register callbacks
 * ------------------------------------------------------------------ */

Impose::Impose(Plugin *plugin, ui::Box *root)
{
	this->plugin = plugin;
	this->root = root;
	this->imposePlugin = Plugin::Find("impose_plugin");
	this->modeText = NULL;
	this->injectBox = NULL;
	this->injectLabel = NULL;
	this->saveButton = NULL;
	this->hudText = NULL;
	this->hudPosText = NULL;
	this->llBox = NULL;
	this->jitBox = NULL;
	this->softcapText = NULL;
	this->advFlags = 0;
	this->softcap = 0;
	this->valid = false;
	this->registered = false;

	if (!plugin || !root)
		return;

	/* Read current game titleid from kernel module (before the interval, so
	 * hasGame is known when deciding whose interval is on screen). */
	ReadTitleId();

	/*
	 * Heal a leaked per-game override. The helper restores the previous
	 * interval in its module_stop, but a hard crash or a forced exit skips
	 * that. With no game running the kernel's interval must be the global
	 * one, so if it is not, put it back rather than letting a dead game's
	 * override quietly cap the next launch.
	 */
	if (!hasGame && fpsvitaGetInterval() != g_globalInterval)
		fpsvitaSetInterval(g_globalInterval);
	if (!hasGame)
		fpsvitaSetAdvanced((g_lowLatency ? FPSVITA_ADV_LOWLAT : 0) | (g_jit ? FPSVITA_ADV_JIT : 0)
		                   | (g_tear ? FPSVITA_ADV_TEAR : 0), g_softcap);

	/* Read current state from kernel module */
	this->currentInterval = fpsvitaGetInterval();
	this->currentInject = fpsvitaGetInject();
	this->autodetect = fpsvitaGetAutodetect();
	this->nativeInterval = fpsvitaGetNativeInterval();
	if (hasGame) {
		/* What the helper put in force for this title. */
		FpsvitaState st;
		st.adv_flags = 0; st.softcap = 0;
		fpsvitaGetState(&st);
		this->advFlags = st.adv_flags;
		this->softcap = st.softcap;
	} else {
		this->advFlags = (g_lowLatency ? FPSVITA_ADV_LOWLAT : 0) | (g_jit ? FPSVITA_ADV_JIT : 0)
		               | (g_tear ? FPSVITA_ADV_TEAR : 0);
		this->softcap = g_softcap;
	}
	{
		/* One line per menu open: "Impose: game=<titleid> interval=.. native=.." */
		char line[96];
		int n = sce_paf_snprintf(line, sizeof(line),
			"Impose: game=%s interval=%d native=%d inject=%d autodetect=%d\n",
			hasGame ? titleid : "-", (int)currentInterval, (int)nativeInterval,
			(int)currentInject, (int)autodetect);
		if (n > 0 && n < (int)sizeof(line))
			dbg_log(line, n);
	}

	/* 20px separator plane above the row, exactly as PSVshellPlus draws
	 * before each of its groups (impose_plugin plane style 0x26c7781e). */
	if (this->imposePlugin) {
		ui::Widget *sep = this->imposePlugin->CreateWidget(
			this->root, "plane", IDParam(0x7ac075f7u), IDParam(0x26c7781eu));
			if (sep) {
			math::v4 sz;
			sz.set_x(835.0f);
			sz.set_y(20.0f);
			sep->SetSize(sz);
		}
	}

	Plugin::TemplateOpenParam tmpParam;
	int32_t topen = this->plugin->TemplateOpen(this->root,
	                                           IDParam("fc_template_impose"),
	                                           tmpParam);
	if (topen < 0) {
		dbg_hex("Impose: TemplateOpen failed", (uint32_t)topen);
		return;
	}

	/* Find child widgets */
	this->modeText = (ui::Text *)this->root->FindChild(
		IDParam("fc_text_mode_value"), 0);
	this->injectBox = (ui::CheckBox *)this->root->FindChild(
		IDParam("fc_check_inject"), 0);
	this->injectLabel = (ui::Text *)this->root->FindChild(
		IDParam("fc_text_inject_label"), 0);
	this->saveButton = this->root->FindChild(
		IDParam("fc_button_save"), 0);

	ui::Widget *modeDown = this->root->FindChild(
		IDParam("fc_button_mode_down"), 0);
	ui::Widget *modeUp = this->root->FindChild(
		IDParam("fc_button_mode_up"), 0);

	/* Second row: overlay mode and position */
	this->hudText = (ui::Text *)this->root->FindChild(IDParam("fc_text_hud_value"), 0);
	this->hudPosText = (ui::Text *)this->root->FindChild(IDParam("fc_text_hud_pos_value"), 0);
	ui::Widget *hudDown = this->root->FindChild(IDParam("fc_button_hud_down"), 0);
	ui::Widget *hudUp = this->root->FindChild(IDParam("fc_button_hud_up"), 0);
	ui::Widget *hudPosDown = this->root->FindChild(IDParam("fc_button_hud_pos_down"), 0);
	ui::Widget *hudPosUp = this->root->FindChild(IDParam("fc_button_hud_pos_up"), 0);
	/* Third row: latency options */
	this->llBox = (ui::CheckBox *)this->root->FindChild(IDParam("fc_check_lowlat"), 0);
	this->jitBox = (ui::CheckBox *)this->root->FindChild(IDParam("fc_check_jit"), 0);
	this->softcapText = (ui::Text *)this->root->FindChild(IDParam("fc_text_softcap_value"), 0);
	ui::Widget *scDown = this->root->FindChild(IDParam("fc_button_softcap_down"), 0);
	ui::Widget *scUp = this->root->FindChild(IDParam("fc_button_softcap_up"), 0);

	/* Every widget in the template is required. Partial construction would
	 * leave callbacks bound to an object whose members are NULL. */
	if (!modeText || !injectBox || !injectLabel || !saveButton
	    || !modeDown || !modeUp || !hudText || !hudPosText
	    || !hudDown || !hudUp || !hudPosDown || !hudPosUp
	    || !llBox || !jitBox || !softcapText || !scDown || !scUp) {
		DBG("Impose: a template widget is missing");
		return;
	}

	/* Set initial display state */
	UpdateModeLabel();
	injectBox->SetCheck(this->currentInject == FPSVITA_INJECT_ON);
	UpdateInjectState();
	UpdateHudLabels();
	llBox->SetCheck((advFlags & FPSVITA_ADV_LOWLAT) != 0);
	jitBox->SetCheck((advFlags & FPSVITA_ADV_JIT) != 0);
	UpdateSoftcapLabel();

	/* Show save button only when a game is running */
	if (hasGame)
		widgetShow(saveButton);
	else
		widgetHide(saveButton);

	/* Register button callbacks */
	modeDown->AddEventCallback(ui::Button::CB_BTN_DECIDE, ModeDownCBFun, this);
	modeUp->AddEventCallback(ui::Button::CB_BTN_DECIDE, ModeUpCBFun, this);
	injectBox->AddEventCallback(ui::Button::CB_BTN_DECIDE, InjectCBFun, this);
	saveButton->AddEventCallback(ui::Button::CB_BTN_DECIDE, SaveCBFun, this);
	hudDown->AddEventCallback(ui::Button::CB_BTN_DECIDE, HudDownCBFun, this);
	hudUp->AddEventCallback(ui::Button::CB_BTN_DECIDE, HudUpCBFun, this);
	hudPosDown->AddEventCallback(ui::Button::CB_BTN_DECIDE, HudPosDownCBFun, this);
	hudPosUp->AddEventCallback(ui::Button::CB_BTN_DECIDE, HudPosUpCBFun, this);
	llBox->AddEventCallback(ui::Button::CB_BTN_DECIDE, LowlatCBFun, this);
	jitBox->AddEventCallback(ui::Button::CB_BTN_DECIDE, JitCBFun, this);
	scDown->AddEventCallback(ui::Button::CB_BTN_DECIDE, SoftcapDownCBFun, this);
	scUp->AddEventCallback(ui::Button::CB_BTN_DECIDE, SoftcapUpCBFun, this);

	/* Register periodic update for menu close detection */
	common::MainThreadCallList::Register(Update, this);
	this->registered = true;
	this->valid = true;
}

/* ------------------------------------------------------------------
 * Destructor
 * ------------------------------------------------------------------ */

Impose::~Impose()
{
	if (registered)
		common::MainThreadCallList::Unregister(Update, this);

	/* Re-arm the shell so the next quick menu open builds a fresh row. */
	fpsvitaImposeClosed();
}

} /* namespace fpsvita */
