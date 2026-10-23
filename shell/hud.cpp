/*
 * fpsvita overlay (see hud.h). Styled after PSVshellPlus's HUD: Mini is two
 * green lines with no box, Full a dialog box with uppercase labels on the
 * left and right-aligned values.
 */

#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include "hud.h"
#include "compat.h"

using namespace paf;

/* Plugin::PageOpen(IDParam const&, PageOpenParam const&) and the param
 * constructor are real exports (ScePaf-RE's lib names them Plugin::SetRootWidget and
 * RootWidgetInitParam). The param is filled as a raw buffer, as InitParam is
 * in main.cpp: ScePaf-RE's class layout is only trusted for the one field
 * set here. */
extern "C" void pafPageOpenParamCtor(void *self)
    __asm__("_ZN3paf6Plugin13PageOpenParamC1Ev");
extern "C" ui::Scene *pafPluginPageOpen(Plugin *self, const IDParam &id, const void *param)
    __asm__("_ZN3paf6Plugin8PageOpenERKNS_7IDParamERKNS0_13PageOpenParamE");

#define FPSVITA_PAGEOPEN_PARAM_SIZE 0x2C   /* sizeof(Plugin::PageOpenParam) */
#define FPSVITA_PAGEOPEN_PRIORITY_OFF 8    /* offsetof overwrite_draw_priority */
#define FPSVITA_HUD_DRAW_PRIORITY 6        /* PSVshellPlus uses 6: above the game */

#define FPSVITA_HUD_POLL_US 200000
#define FPSVITA_HUD_ROWS 11
#define FPSVITA_HUD_WIDTH 350.0f           /* same as PSVshellPlus's box */
#define FPSVITA_HUD_HEIGHT_FULL 302.0f     /* 11 rows of 26 px */
#define FPSVITA_HUD_SETTLE_US 3000000      /* the helper's EMAs after a launch or a toggle */
#define FPSVITA_HUD_TIP_HOLD_US 5000000    /* a new tip must hold this long before it replaces the shown one */
#define FPSVITA_HUD_HEIGHT_MINI 60.0f
#define FPSVITA_SCREEN_W 960.0f
#define FPSVITA_SCREEN_H 544.0f
#define FPSVITA_HUD_MARGIN 10.0f
#define HUD_BUF 64

namespace fpsvita {

static ui::Scene  *s_page = NULL;
static ui::Widget *s_root = NULL;   /* fc_plane_hud_root: moved and faded as a whole */
static ui::Widget *s_box = NULL;    /* fc_plane_hud_box: Full only */
static ui::Text   *s_mini[2];       /* Mini only */
static ui::Text   *s_label[FPSVITA_HUD_ROWS];
static ui::Text   *s_value[FPSVITA_HUD_ROWS];
static bool s_registered = false;
static bool s_openFailed = false;   /* RCO/shell mismatch: do not try again, do not leak pages */
static bool s_visible = true;       /* a freshly opened template is visible */
static uint32_t s_lastPoll = 0;
static bool     s_redraw = false;   /* mode or corner changed: draw on the next poll */
static uint32_t s_lastSeq = 0;
static int32_t  s_lastInterval = 0x7fffffff;
static int32_t  s_lastNative = -1;
static int32_t  s_lastAdv = -1, s_lastSoftcap = -1;
static uint32_t s_lastTitle[3];
/* 64-bit process time: 32-bit microseconds wrap every 71.6 minutes, which
 * would read as a fresh toggle or launch. */
static uint64_t s_now;              /* process time at the current poll */
static uint64_t s_settleFrom;       /* tips wait for the EMAs after a launch or a toggle */
static uint64_t s_launchTime;       /* the helper's profile push lands just after the title id */
static bool     s_readPeek;         /* this title's state.txt line has read=peek */

/* Before/after readout: the INPUT and FPS values just before the last
 * toggle, shown against the current ones once they have settled. A Lowlat
 * toggle applies at the title's next launch, so its "before" is held until
 * the same title comes back. */
enum { CHG_NONE, CHG_LIMIT, CHG_LOWLAT, CHG_ANTILAG, CHG_LIMITER, CHG_TEAR };
static int      s_chg = CHG_NONE;
static bool     s_chgArmed;         /* in effect: the settle timer is running */
static bool     s_chgCheck;         /* Lowlat: the relaunch happened, confirm it took once the profile is in */
static bool     s_chgUnsaved;       /* Lowlat: relaunched without the toggle (Save was not pressed) */
static int      s_chgTarget;        /* Lowlat: the state the toggle asked for */
static uint64_t s_chgTime;
static uint32_t s_chgInput, s_chgFps, s_chgTitle[3];
static uint32_t s_prevInput, s_prevFps;   /* slow averages of what was shown: the "before" of the next toggle */

/* The tip rules sit on thresholds the averages wander across at every scene
 * change; the shown tip only follows a candidate that has held for a while. */
enum { TIP_WAIT, TIP_NONE, TIP_NOTHELD, TIP_ANTILAG, TIP_LOWLAT, TIP_COMPAT };
static int      s_tipShown = TIP_WAIT, s_tipCand = TIP_WAIT;
static uint64_t s_tipCandSince;
static const wchar_t *s_tipCompat = L"";   /* the shown compat tip, kept through the hold */

/* Row names borrow PC terms (PresentMon "CPU busy", Reflex-style latency):
 * INPUT is the frame's own newest controller sample to its flip, QUEUE its
 * submit-to-flip, both measured per frame by the helper. */
enum { ROW_FPS, ROW_LIMIT, ROW_CPU, ROW_INPUT, ROW_QUEUE, ROW_ANTILAG, ROW_LOWLAT, ROW_LIMITER,
       ROW_TIP, ROW_CHANGE, ROW_TITLE };
static const wchar_t *const s_labels[FPSVITA_HUD_ROWS] = {
	L"FPS:", L"LIMIT:", L"CPU:", L"INPUT:", L"QUEUE:", L"ANTI-LAG:", L"LOW LAT:", L"LIMITER:",
	L"TIP:", L"LAST:", L"TITLE:"
};

static ui::Text *findText(const char *id)
{
	return (ui::Text *)s_page->FindChild(IDParam(id), 0);
}

bool titleReadsPeek(const char *titleid);   /* impose.cpp */

bool Hud::Open()
{
	if (!g_fpsvitaPlugin)
		return false;

	/* One attempt. Pages cannot be closed (see hud.h), so a failure after
	 * PageOpen must not be retried on the next click: each retry would leave
	 * another page open. */
	if (s_openFailed)
		return false;
	s_openFailed = true;

	uint8_t param[FPSVITA_PAGEOPEN_PARAM_SIZE] __attribute__((aligned(8)));
	pafPageOpenParamCtor(param);
	*reinterpret_cast<int32_t *>(param + FPSVITA_PAGEOPEN_PRIORITY_OFF) = FPSVITA_HUD_DRAW_PRIORITY;

	s_page = pafPluginPageOpen(g_fpsvitaPlugin, IDParam("fc_page_hud"), param);
	if (!s_page) {
		DBG("Hud: PageOpen failed");
		return false;
	}

	Plugin::TemplateOpenParam tmpParam;
	int32_t r = g_fpsvitaPlugin->TemplateOpen(s_page, IDParam("fc_template_hud"), tmpParam);
	if (r < 0) {
		dbg_hex("Hud: TemplateOpen failed", (uint32_t)r);
		return false;
	}

	s_root = s_page->FindChild(IDParam("fc_plane_hud_root"), 0);
	s_box  = s_page->FindChild(IDParam("fc_plane_hud_box"), 0);
	s_mini[0] = findText("fc_text_hud_mini0");
	s_mini[1] = findText("fc_text_hud_mini1");
	bool ok = s_root && s_box && s_mini[0] && s_mini[1];
	char id[24];
	for (int i = 0; i < FPSVITA_HUD_ROWS; i++) {
		sce_paf_snprintf(id, sizeof(id), "fc_text_hud_l%d", i);
		s_label[i] = findText(id);
		sce_paf_snprintf(id, sizeof(id), "fc_text_hud_v%d", i);
		s_value[i] = findText(id);
		ok = ok && s_label[i] && s_value[i];
	}
	if (!ok) {
		/* Never leave a half-built box on screen (an RCO/shell mismatch). */
		DBG("Hud: a template widget is missing");
		if (s_root) widgetHide(s_root);
		s_root = NULL;
		return false;
	}
	for (int i = 0; i < FPSVITA_HUD_ROWS; i++)
		textSet(s_label[i], s_labels[i]);

	/* Nothing to show until a game is running. */
	SetVisible(false);
	s_openFailed = false;
	return true;
}

void Hud::SetVisible(bool on)
{
	if (!s_root || on == s_visible)
		return;
	s_visible = on;
	if (on)
		widgetShow(s_root);
	else
		widgetHide(s_root);
}

/* Same coordinate convention as PSVshellPlus's HUD: the root plane's
 * layout_hint anchors its top-left corner, x grows right, y grows down as
 * negative values. */
void Hud::SetPosition()
{
	if (!s_root)
		return;
	float h = (g_hudMode == FPSVITA_HUD_FULL) ? FPSVITA_HUD_HEIGHT_FULL : FPSVITA_HUD_HEIGHT_MINI;
	bool right  = (g_hudPos == 1 || g_hudPos == 3);
	bool bottom = (g_hudPos == 2 || g_hudPos == 3);
	math::v4 pos;
	pos.set_x(right  ? FPSVITA_SCREEN_W - FPSVITA_HUD_WIDTH - FPSVITA_HUD_MARGIN : FPSVITA_HUD_MARGIN);
	pos.set_y(bottom ? -(FPSVITA_SCREEN_H - h - FPSVITA_HUD_MARGIN) : -FPSVITA_HUD_MARGIN);
	s_root->SetPos(pos);
}

void Hud::Apply()
{
	fpsvitaSetHud(g_hudMode, g_hudPos);

	if (g_hudMode == FPSVITA_HUD_OFF) {
		SetVisible(false);
		if (s_registered) {
			common::MainThreadCallList::Unregister(Update, NULL);
			s_registered = false;
		}
		return;
	}

	if (!s_root && !Open())
		return;

	bool full = (g_hudMode == FPSVITA_HUD_FULL);
	if (full) {
		widgetShow(s_box);
		widgetHide(s_mini[0]);
		widgetHide(s_mini[1]);
	} else {
		widgetHide(s_box);
		widgetShow(s_mini[0]);
		widgetShow(s_mini[1]);
	}
	SetPosition();

	/* Redraw on the next poll. Not by faking a changed limit: the toggle
	 * detection would record that as one. */
	s_redraw = true;
	s_lastPoll = 0;

	if (!s_registered) {
		common::MainThreadCallList::Register(Update, NULL);
		s_registered = true;
	}
}

void Hud::Shutdown()
{
	if (s_registered) {
		common::MainThreadCallList::Unregister(Update, NULL);
		s_registered = false;
	}
	SetVisible(false);
}

void Hud::Update(void *arg)
{
	uint64_t now = sceKernelGetProcessTimeWide();
	if ((uint32_t)now - s_lastPoll < FPSVITA_HUD_POLL_US)
		return;
	s_lastPoll = (uint32_t)now;

	FpsvitaState st;
	sce_paf_memset(&st, 0, sizeof(st));
	fpsvitaGetState(&st);

	/* The kernel clears the title id when the owning process exits. */
	bool game = (st.titleid[0] & 0xFFu) != 0;
	SetVisible(game);
	if (!game) {
		s_lastTitle[0] = 0;   /* a relaunch of the same title is a launch, not a toggle */
		return;
	}
	s_now = now;

	bool sameTitle = st.titleid[0] == s_lastTitle[0] && st.titleid[1] == s_lastTitle[1]
	              && st.titleid[2] == s_lastTitle[2];
	if (!sameTitle) {
		/* A launch: EMAs start over. A pending Lowlat readout for this very
		 * title is what the relaunch was for; anything else is stale. */
		sce_paf_memcpy(s_lastTitle, st.titleid, sizeof(s_lastTitle));
		s_settleFrom = s_launchTime = now;
		s_tipShown = s_tipCand = TIP_WAIT;
		s_prevInput = s_prevFps = 0;
		s_readPeek = titleReadsPeek((const char *)st.titleid);
		if (s_chg == CHG_LOWLAT && !s_chgArmed && st.titleid[0] == s_chgTitle[0]
		    && st.titleid[1] == s_chgTitle[1] && st.titleid[2] == s_chgTitle[2]) {
			s_chgCheck = true;   /* decided below, once the helper's profile push is in */
		} else {
			s_chg = CHG_NONE;
		}
	} else if (now - s_launchTime >= 1000000) {
		if (s_chgCheck) {
			/* A Lowlat toggle only survives the relaunch through Save. */
			s_chgCheck = false;
			if (((st.adv_flags & FPSVITA_ADV_LOWLAT) != 0) == s_chgTarget) {
				s_chgArmed = true;
				s_chgUnsaved = false;
				s_chgTime = now;
			} else {
				s_chgUnsaved = true;
			}
		}
		int chg = CHG_NONE;
		if (st.interval != s_lastInterval)
			chg = CHG_LIMIT;
		else if (st.softcap != s_lastSoftcap)
			chg = CHG_LIMITER;
		else if ((st.adv_flags ^ s_lastAdv) & FPSVITA_ADV_LOWLAT)
			chg = CHG_LOWLAT;
		else if ((st.adv_flags ^ s_lastAdv) & (FPSVITA_ADV_JIT | FPSVITA_ADV_AUTO))
			chg = CHG_ANTILAG;
		else if ((st.adv_flags ^ s_lastAdv) & FPSVITA_ADV_TEAR)
			chg = CHG_TEAR;
		/* A toggle is a change seen while the game was presenting before and
		 * after it; anything around a load or a teardown is not one. */
		if (chg != CHG_NONE && s_prevFps && st.stats.fps_x10) {
			s_chg = chg;
			s_chgArmed = chg != CHG_LOWLAT;   /* Lowlat waits for the relaunch */
			s_chgCheck = s_chgUnsaved = false;
			s_chgTarget = (st.adv_flags & FPSVITA_ADV_LOWLAT) != 0;
			s_chgTime = now;
			s_settleFrom = now;
			s_tipShown = s_tipCand = TIP_WAIT;
			s_chgInput = s_prevInput;
			s_chgFps = s_prevFps;
			sce_paf_memcpy(s_chgTitle, st.titleid, sizeof(s_chgTitle));
		}
	}

	if (!s_redraw && st.stats.seq == s_lastSeq && st.interval == s_lastInterval
	    && st.native_interval == s_lastNative && st.adv_flags == s_lastAdv
	    && st.softcap == s_lastSoftcap)
		return;
	s_redraw = false;
	s_lastSeq = st.stats.seq;
	s_lastInterval = st.interval;
	s_lastNative = st.native_interval;
	s_lastAdv = st.adv_flags;
	s_lastSoftcap = st.softcap;
	Refresh(st);
}

/* "12.3" from microseconds, tenths of a millisecond. */
#define MS_INT(us) ((unsigned)((us) / 1000u))
#define MS_TENTH(us) ((unsigned)(((us) % 1000u) / 100u))

/* Format into a HUD line; a failed or overlong write leaves pos where it was. */
static int appendW(wchar_t *buf, int pos, const wchar_t *fmt, ...)
{
	if (pos < 0 || pos >= HUD_BUF - 1)
		return pos;
	va_list ap;
	va_start(ap, fmt);
	int n = sce_paf_vswprintf(buf + pos, HUD_BUF - pos, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= HUD_BUF - pos) {
		buf[pos] = L'\0';
		return pos;
	}
	return pos + n;
}

static void setRow(int row, const wchar_t *fmt, ...)
{
	wchar_t buf[HUD_BUF];
	va_list ap;
	va_start(ap, fmt);
	int n = sce_paf_vswprintf(buf, HUD_BUF, fmt, ap);
	va_end(ap);
	if (n > 0 && n < HUD_BUF)
		textSet(s_value[row], buf);
}

void Hud::Refresh(const FpsvitaState &st)
{
	const FpsvitaStats &s = st.stats;
	unsigned fps_i = s.fps_x10 / 10, fps_t = s.fps_x10 % 10;
	int capfps = fpsvitaIntervalToFpsAt(st.interval, st.panel_hz);   /* what the limit gives */
	int nat = fpsvitaIntervalToFps(st.native_interval);              /* what the game asked for */
	int reqLL = (st.adv_flags & FPSVITA_ADV_LOWLAT) != 0;
	int onLL = (s.flags & FPSVITA_STATF_LOWLATENCY) != 0;      /* clamped by the helper */
	int natLL = (s.flags & FPSVITA_STATF_LOWLAT_NATIVE) != 0;  /* the title's own setting */
	int antilag = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
	int antilagAuto = (st.adv_flags & FPSVITA_ADV_AUTO) != 0;
	int reqTear = (st.adv_flags & FPSVITA_ADV_TEAR) != 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED;
	int onTear = (s.flags & FPSVITA_STATF_TEAR) != 0;
	int haveWork = (s.flags & FPSVITA_STATF_ADDENTRY) != 0;
	wchar_t wtid[FPSVITA_TITLEID_LEN];
	{
		const char *tid = (const char *)st.titleid;
		int i;
		for (i = 0; i < FPSVITA_TITLEID_LEN - 1 && tid[i]; i++)
			wtid[i] = (wchar_t)(unsigned char)tid[i];
		wtid[i] = L'\0';
	}
	const wchar_t *mode = st.interval == FPSVITA_INTERVAL_OFF ? L"DEFAULT"
	                    : st.interval == FPSVITA_INTERVAL_UNCAPPED
	                      ? (reqTear ? (onTear ? L"TEARING" : L"TEARING (NO KERNEL)") : L"UNCAPPED")
	                    : NULL;
	wchar_t buf[HUD_BUF];
	int pos;

	if (g_hudMode != FPSVITA_HUD_FULL) {
		/* Line 1: rate and limit. Line 2: what is being done to the game. */
		buf[0] = L'\0';
		pos = appendW(buf, 0, L"%u FPS  ", fps_i);
		if (mode)
			pos = appendW(buf, pos, L"%ls", mode);
		else
			pos = appendW(buf, pos, L"LIMIT %d", capfps);
		if (nat > 0)
			pos = appendW(buf, pos, L" / OG:%d", nat);
		textSet(s_mini[0], buf);

		/* Short forms of what is active, so the worst case ("VSYNC  LL?  AL+
		 * LIM 50") fits the box: the full words run off the screen in the
		 * right-hand corners. Full mode has the detail. */
		buf[0] = L'\0';
		pos = 0;
		if (s.flags & FPSVITA_STATF_INJECT)
			pos = appendW(buf, pos, L"VSYNC  ");
		if (natLL || onLL)
			pos = appendW(buf, pos, L"LL  ");
		else if (reqLL)
			pos = appendW(buf, pos, L"LL?  ");   /* requested, applies at the next launch */
		if (antilag)
			pos = appendW(buf, pos, L"AL+  ");
		else if (antilagAuto)
			pos = appendW(buf, pos, L"AL  ");
		if (st.softcap > 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED)
			pos = appendW(buf, pos, L"LIM %d  ", (int)st.softcap);
		if (st.panel_hz > 0)
			pos = appendW(buf, pos, L"%dHZ  ", (int)st.panel_hz);
		textSet(s_mini[1], buf);
		return;
	}

	if (s.frame_us)
		setRow(ROW_FPS, L"%u.%u / %u.%u ms", fps_i, fps_t, MS_INT(s.frame_us), MS_TENTH(s.frame_us));
	else
		setRow(ROW_FPS, L"%u.%u", fps_i, fps_t);

	if (st.panel_hz > 0 && mode)   /* experimental panel rate: the limit is in its vblanks */
		setRow(ROW_LIMIT, L"%ls @%dHZ", mode, (int)st.panel_hz);
	else if (st.panel_hz > 0)
		setRow(ROW_LIMIT, L"%d @%dHZ", capfps, (int)st.panel_hz);
	else if (mode)
		setRow(ROW_LIMIT, nat > 0 ? L"%ls / OG:%d" : L"%ls", mode, nat);
	else
		setRow(ROW_LIMIT, nat > 0 ? L"%d / OG:%d" : L"%d FPS", capfps, nat);

	if (haveWork)
		setRow(ROW_CPU, L"%u.%u ms", MS_INT(s.work_us), MS_TENTH(s.work_us));
	else
		setRow(ROW_CPU, L"-");
	if (s.input_us)
		setRow(ROW_INPUT, L"%u.%u ms", MS_INT(s.input_us), MS_TENTH(s.input_us));
	else
		setRow(ROW_INPUT, L"-");
	if (s.queue_us)
		setRow(ROW_QUEUE, L"%u.%u ms", MS_INT(s.queue_us), MS_TENTH(s.queue_us));
	else
		setRow(ROW_QUEUE, L"-");

	/* Anti-lag: the fresh poll is always at work once enabled; the value is
	 * the whole-vblank sleep in force and the frames it has cost. */
	if (antilagAuto)
		setRow(ROW_ANTILAG, L"AUTO");
	else if (!antilag)
		setRow(ROW_ANTILAG, L"OFF");
	else if (s.jit_slack_us)
		setRow(ROW_ANTILAG, L"%u.%u ms / %u", MS_INT(s.jit_slack_us), MS_TENTH(s.jit_slack_us), (unsigned)s.jit_missed);
	else
		setRow(ROW_ANTILAG, L"ON / %u", (unsigned)s.jit_missed);

	/* A title that runs a 1-deep queue by itself gets nothing from the
	 * option; say so instead of implying the user turned it on or off. */
	if (natLL)
		setRow(ROW_LOWLAT, L"ON (GAME'S OWN)");
	else if (reqLL && onLL)
		setRow(ROW_LOWLAT, L"ON");
	else if (reqLL)
		setRow(ROW_LOWLAT, L"RESTART GAME");
	else if (onLL)
		setRow(ROW_LOWLAT, L"ON UNTIL RESTART");
	else
		setRow(ROW_LOWLAT, L"OFF");

	if (st.softcap > 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED)
		setRow(ROW_LIMITER, L"%d FPS", (int)st.softcap);
	else if (st.softcap > 0)
		setRow(ROW_LIMITER, L"%d FPS (NEEDS UNCAPPED)", (int)st.softcap);
	else
		setRow(ROW_LIMITER, L"OFF");

	/* Tip: what the readings say is worth trying, first match wins. The
	 * thresholds come from the 2026-10 device batches
	 * (docs/latency-measurements.md): Antilag paid off wherever the sample
	 * was a frame old or the frame sat a vblank in the queue (MGS2 had a
	 * fresh sample and still gained the most, from the queue); Lowlat only
	 * where the queue held a frame; a limit the game misses in a tenth of
	 * its frames is a limit it cannot hold. */
	int capfps_tip = capfps;
	uint32_t vblank = st.panel_hz > 0 ? 1000000u / (uint32_t)st.panel_hz : FPSVITA_VBLANK_US;
	bool settled = s_now - s_settleFrom >= FPSVITA_HUD_SETTLE_US;
	/* A limit the game runs a tenth under. (A rolling double-period count was
	 * tried and dropped: one cutscene load tripped it for the next 300 frames.) */
	bool notHeld = capfps_tip > 0 && s.fps_x10 < (uint32_t)capfps_tip * 9u;
	/* Antilag's whole-vblank sleep only fits when the CPU work leaves a
	 * vblank of headroom after its margins (2.5 ms each for the margin and
	 * the fresh poll); MGS2 gameplay at 12 to 14 ms of work gained nothing. */
	bool sleepFits = haveWork && (st.interval == FPSVITA_INTERVAL_30FPS || st.interval == FPSVITA_INTERVAL_20FPS)
	              && s.work_us + 5000u + vblank <= vblank * (uint32_t)st.interval;
	const wchar_t *compat = fpsvitaCompatWarn(fpsvitaCompatFlags((const char *)st.titleid),
	                                          st.interval, st.native_interval, s_readPeek);
	if (compat)
		s_tipCompat = compat;
	int cand;
	if (!settled)
		cand = TIP_WAIT;
	else if (notHeld)
		cand = TIP_NOTHELD;
	else if ((!antilag && !antilagAuto && s.sample_age_us >= 10000u)
	         || (!antilag && sleepFits && s.queue_us >= vblank))
		cand = TIP_ANTILAG;
	else if (!reqLL && !natLL && !onLL && s.frame_us && s.queue_us * 10u >= s.frame_us * 8u)
		cand = TIP_LOWLAT;
	else if (compat)
		cand = TIP_COMPAT;
	else
		cand = TIP_NONE;
	if (cand != s_tipCand) {
		s_tipCand = cand;
		s_tipCandSince = s_now;
	}
	if (s_tipShown == TIP_WAIT || cand == TIP_WAIT || s_now - s_tipCandSince >= FPSVITA_HUD_TIP_HOLD_US)
		s_tipShown = cand;
	const wchar_t *tip = L"...";
	switch (s_tipShown) {
	case TIP_NONE:    tip = L"NONE"; break;
	case TIP_NOTHELD: tip = NULL; break;   /* formatted below: the rates follow the panel */
	case TIP_ANTILAG: tip = L"TRY FULL ANTI-LAG"; break;
	case TIP_LOWLAT:  tip = L"TRY LOW LAT"; break;
	case TIP_COMPAT:  tip = s_tipCompat; break;
	default: break;
	}
	if (!tip && st.interval >= 1 && st.interval < 3)
		setRow(ROW_TIP, L"%d NOT HELD, TRY %d", capfps_tip,
		       fpsvitaIntervalToFpsAt(st.interval + 1, st.panel_hz));
	else if (!tip)
		setRow(ROW_TIP, L"%d NOT HELD", capfps_tip);
	else
		setRow(ROW_TIP, L"%ls", tip);

	/* Last toggle: INPUT before > after, the real evidence. The value column
	 * next to this label holds about 20 characters. */
	static const wchar_t *const chgName[] = { L"", L"LIMIT", L"LOW LAT", L"ANTI-LAG", L"LIMITER", L"TEARING" };
	if (s_chg == CHG_NONE)
		setRow(ROW_CHANGE, L"-");
	else if (!s_chgArmed)
		setRow(ROW_CHANGE, s_chgUnsaved ? L"LOW LAT: NOT SAVED" : L"LOW LAT: RESTART");
	else if (s_now - s_chgTime < FPSVITA_HUD_SETTLE_US)
		setRow(ROW_CHANGE, L"%ls ...", chgName[s_chg]);
	else if (s_chgInput && s.input_us) {
		unsigned before = MS_INT(s_chgInput + 500), after = MS_INT(s.input_us + 500);
		int dfps = ((int)s.fps_x10 - (int)s_chgFps - 5) / 10;   /* a loss, rounded: never 0 here */
		if (s_chgFps && s.fps_x10 * 20 < s_chgFps * 19)   /* cost side: lost over 5 percent */
			setRow(ROW_CHANGE, L"%ls %u>%u %dFPS", chgName[s_chg], before, after, dfps);
		else
			setRow(ROW_CHANGE, L"%ls %u>%u MS", chgName[s_chg], before, after);
	} else if (s_chgFps && s.fps_x10)
		setRow(ROW_CHANGE, L"%ls %u>%u FPS", chgName[s_chg], (unsigned)(s_chgFps + 5) / 10, (unsigned)(s.fps_x10 + 5) / 10);
	else
		setRow(ROW_CHANGE, L"%ls -", chgName[s_chg]);

	/* Slow averages (about 2 s at four pushes a second), so a menu opened
	 * on a scene transition does not fix the "before" to one odd frame. */
	if (s.input_us)
		s_prevInput = s_prevInput ? s_prevInput + ((int32_t)s.input_us - (int32_t)s_prevInput) / 8 : s.input_us;
	if (s.fps_x10)
		s_prevFps = s_prevFps ? s_prevFps + ((int32_t)s.fps_x10 - (int32_t)s_prevFps) / 8 : s.fps_x10;

	setRow(ROW_TITLE, L"%ls", wtid);
}

} /* namespace fpsvita */
