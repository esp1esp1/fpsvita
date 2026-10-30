/*
 * fpsvita overlay (see hud.h). Styled after PSVshellPlus's HUD: Mini is one
 * small green line with no box, Full a dialog box with uppercase labels on
 * the left and right-aligned values.
 */

#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include "hud.h"
#include "compat.h"
#include "panelauto.h"

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
#define FPSVITA_HUD_ROWS 12
#define FPSVITA_HUD_WIDTH 350.0f           /* same as PSVshellPlus's box */
#define FPSVITA_HUD_HEIGHT_FULL 328.0f     /* 12 rows of 26 px */
#define FPSVITA_HUD_ROW_H 26.0f
#define FPSVITA_HUD_SETTLE_US 3000000      /* the helper's EMAs after a launch or a toggle (LAST row) */
#define FPSVITA_HUD_TIP_HOLD_US 5000000    /* a new tip must hold this long before it replaces the shown one */
#define FPSVITA_HUD_WINDOW 20              /* pushes kept for the tip rules and MISS: 5 s at four a second */
#define FPSVITA_HUD_TIP_BASELINE_US 10000000 /* settle plus a window gathered after it, before the first tip */
#define FPSVITA_HUD_WIDTH_MINI 520.0f      /* the Mini's root slot: the right-anchored copy ends at its edge */
#define FPSVITA_HUD_HEIGHT_MINI 24.0f
#define FPSVITA_SCREEN_W 960.0f
#define FPSVITA_SCREEN_H 544.0f
#define FPSVITA_HUD_MARGIN 10.0f
#define HUD_BUF 128                        /* the one-line Mini runs past 64 */

namespace fpsvita {

static ui::Scene  *s_page = NULL;
static ui::Widget *s_root = NULL;   /* fc_plane_hud_root: moved and faded as a whole */
static ui::Widget *s_box = NULL;    /* fc_plane_hud_box: Full only */
static ui::Text   *s_mini = NULL;   /* Mini only: one line, left-anchored (left corners) */
static ui::Text   *s_miniR = NULL;  /* the same line right-anchored (right corners) */
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
static int32_t  s_lastAdv = -1, s_lastSoftcap = -1, s_lastPanel = -1;
static uint32_t s_lastTitle[3];
/* 64-bit process time: 32-bit microseconds wrap every 71.6 minutes, which
 * would read as a fresh toggle or launch. */
static uint64_t s_now;              /* process time at the current poll */
static uint64_t s_settleFrom;       /* tips wait for the EMAs after a launch or a toggle */
static uint64_t s_launchTime;       /* the helper's profile push lands just after the title id */
static bool     s_readPeek;         /* this title's state.txt line has read=peek */
static bool     s_newSample;        /* this Refresh follows a new push */

/* The last FPSVITA_HUD_WINDOW pushes: the tip rules only fire on a reading
 * that held for the whole window (a limit missed on every push, a sample
 * that was old on every push), and MISS is the window's share. Cleared with
 * the settle timer. */
struct HudSample { uint32_t presents, miss, fps, frame, queue, age, work; };
static HudSample s_win[FPSVITA_HUD_WINDOW];
static int      s_winN, s_winHead;

static void winReset(void)
{
	s_winN = s_winHead = 0;
}

static void winPush(const FpsvitaStats &s)
{
	HudSample &w = s_win[s_winHead];
	w.presents = s.presents; w.miss = s.miss; w.fps = s.fps_x10; w.frame = s.frame_us;
	w.queue = s.queue_us; w.age = s.sample_age_us; w.work = s.work_us;
	s_winHead = (s_winHead + 1) % FPSVITA_HUD_WINDOW;
	if (s_winN < FPSVITA_HUD_WINDOW)
		s_winN++;
}

static bool winFull(void) { return s_winN >= FPSVITA_HUD_WINDOW; }
static const HudSample &winOldest(void) { return s_win[winFull() ? s_winHead : 0]; }

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
enum { ROW_FPS, ROW_LIMIT, ROW_CPU, ROW_INPUT, ROW_QUEUE, ROW_MISS, ROW_ANTILAG, ROW_LOWLAT,
       ROW_LIMITER, ROW_TIP, ROW_CHANGE, ROW_TITLE };
static const wchar_t *const s_labels[FPSVITA_HUD_ROWS] = {
	L"FPS:", L"LIMIT:", L"CPU:", L"INPUT:", L"QUEUE:", L"MISS:", L"ANTI-LAG:", L"LOW LAT:",
	L"LIMITER:", L"TIP:", L"LAST:", L"TITLE:"
};

/* Rows below TIP move up a slot when `tips=0` hides it; -1 is not shown. */
static int slotOf(int row)
{
	if (g_tips || row < ROW_TIP)
		return row;
	return row == ROW_TIP ? -1 : row - 1;
}

static float fullHeight(void)
{
	return FPSVITA_HUD_HEIGHT_FULL - (g_tips ? 0.0f : FPSVITA_HUD_ROW_H);
}

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
	s_mini = findText("fc_text_hud_mini");
	s_miniR = findText("fc_text_hud_mini_r");
	bool ok = s_root && s_box && s_mini && s_miniR;
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
		if (slotOf(i) >= 0)
			textSet(s_label[slotOf(i)], s_labels[i]);
	if (!g_tips) {
		/* The freed last slot stays empty and the box loses a row. */
		textSet(s_label[FPSVITA_HUD_ROWS - 1], L"");
		textSet(s_value[FPSVITA_HUD_ROWS - 1], L"");
		math::v4 sz;
		sz.set_x(FPSVITA_HUD_WIDTH);
		sz.set_y(fullHeight());
		s_root->SetSize(sz);
		s_box->SetSize(sz);
	}

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
	bool full = (g_hudMode == FPSVITA_HUD_FULL);
	float h = full ? fullHeight() : FPSVITA_HUD_HEIGHT_MINI;
	float w = full ? FPSVITA_HUD_WIDTH : FPSVITA_HUD_WIDTH_MINI;
	bool right  = (g_hudPos == 1 || g_hudPos == 3);
	bool bottom = (g_hudPos == 2 || g_hudPos == 3);
	math::v4 pos, sz;
	pos.set_x(right  ? FPSVITA_SCREEN_W - w - FPSVITA_HUD_MARGIN : FPSVITA_HUD_MARGIN);
	pos.set_y(bottom ? -(FPSVITA_SCREEN_H - h - FPSVITA_HUD_MARGIN) : -FPSVITA_HUD_MARGIN);
	s_root->SetPos(pos);
	/* The root's size is what the right-anchored Mini copy hangs from: the
	 * template's 350 px would end it mid-screen in a right corner. */
	sz.set_x(w);
	sz.set_y(h);
	s_root->SetSize(sz);
}

void Hud::Apply()
{
	/* Panel auto needs the helper's push with the overlay off. */
	fpsvitaSetHud(g_hudMode | (g_panelAuto ? FPSVITA_HUD_STATS : 0), g_hudPos);

	if (g_hudMode == FPSVITA_HUD_OFF) {
		SetVisible(false);
		if (g_panelAuto && !s_registered) {
			common::MainThreadCallList::Register(Update, NULL);
			s_registered = true;
		} else if (!g_panelAuto && s_registered) {
			common::MainThreadCallList::Unregister(Update, NULL);
			s_registered = false;
		}
		return;
	}

	if (!s_root && !Open())
		return;

	bool full = (g_hudMode == FPSVITA_HUD_FULL);
	bool right = (g_hudPos == 1 || g_hudPos == 3);
	if (full) {
		widgetShow(s_box);
		widgetHide(s_mini);
		widgetHide(s_miniR);
	} else {
		widgetHide(s_box);
		if (right) { widgetHide(s_mini); widgetShow(s_miniR); }
		else       { widgetShow(s_mini); widgetHide(s_miniR); }
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
	fpsvitaGetState(&st, sizeof(st));

	/* The kernel clears the title id when the owning process exits. */
	bool game = (st.titleid[0] & 0xFFu) != 0;
	PanelAuto::Tick(st, now, game);
	SetVisible(game && g_hudMode != FPSVITA_HUD_OFF);
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
		winReset();
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
		if (st.interval != s_lastInterval || st.panel_hz != s_lastPanel)
			chg = CHG_LIMIT;
		else if (st.softcap != s_lastSoftcap)
			chg = CHG_LIMITER;
		else if ((st.adv_flags ^ s_lastAdv) & FPSVITA_ADV_LOWLAT)
			chg = CHG_LOWLAT;
		else if ((st.adv_flags ^ s_lastAdv) & FPSVITA_ADV_JIT)
			chg = CHG_ANTILAG;
		else if ((st.adv_flags ^ s_lastAdv) & FPSVITA_ADV_TEAR)
			chg = CHG_TEAR;
		/* A toggle is a change seen while the game was presenting before and
		 * after it; anything around a load or a teardown is not one, nor is
		 * the governor stepping the rate. */
		if (chg != CHG_NONE && s_prevFps && st.stats.fps_x10 && !PanelAuto::OwnChange(now)) {
			s_chg = chg;
			s_chgArmed = chg != CHG_LOWLAT;   /* Lowlat waits for the relaunch */
			s_chgCheck = s_chgUnsaved = false;
			s_chgTarget = (st.adv_flags & FPSVITA_ADV_LOWLAT) != 0;
			s_chgTime = now;
			s_settleFrom = now;
			s_tipShown = s_tipCand = TIP_WAIT;
			winReset();
			s_chgInput = s_prevInput;
			s_chgFps = s_prevFps;
			sce_paf_memcpy(s_chgTitle, st.titleid, sizeof(s_chgTitle));
		}
	}

	if (!s_redraw && st.stats.seq == s_lastSeq && st.interval == s_lastInterval
	    && st.native_interval == s_lastNative && st.adv_flags == s_lastAdv
	    && st.softcap == s_lastSoftcap && st.panel_hz == s_lastPanel)
		return;
	s_redraw = false;
	s_newSample = st.stats.seq != s_lastSeq;
	s_lastSeq = st.stats.seq;
	s_lastInterval = st.interval;
	s_lastNative = st.native_interval;
	s_lastAdv = st.adv_flags;
	s_lastSoftcap = st.softcap;
	s_lastPanel = st.panel_hz;
	if (g_hudMode == FPSVITA_HUD_OFF || !s_root)
		return;   /* polling for the governor only */
	Refresh(st);
}

/* "12.3" from microseconds, tenths of a millisecond. */
#define MS_INT(us) ((unsigned)((us) / 1000u))
#define MS_TENTH(us) ((unsigned)(((us) % 1000u) / 100u))

/* "9.1" or "-" for a millisecond EMA, for the one-line Mini. */
static const wchar_t *msText(wchar_t *buf, int n, uint32_t us)
{
	if (!us)
		return L"-";
	sce_paf_swprintf(buf, n, L"%u.%u", MS_INT(us), MS_TENTH(us));
	return buf;
}

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
	if (slotOf(row) < 0)
		return;
	wchar_t buf[HUD_BUF];
	va_list ap;
	va_start(ap, fmt);
	int n = sce_paf_vswprintf(buf, HUD_BUF, fmt, ap);
	va_end(ap);
	if (n > 0 && n < HUD_BUF)
		textSet(s_value[slotOf(row)], buf);
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
	int reqTear = (st.adv_flags & FPSVITA_ADV_TEAR) != 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED;
	int onTear = (s.flags & FPSVITA_STATF_TEAR) != 0;
	int haveWork = (s.flags & (FPSVITA_STATF_ADDENTRY | FPSVITA_STATF_FLIPWORK)) != 0;
	int haveGxm = (s.flags & FPSVITA_STATF_ADDENTRY) != 0;   /* the queue rows and the sleep need AddEntry */
	int initSeen = (s.flags & FPSVITA_STATF_GXM_INIT) != 0;   /* Lowlat had its chance */
	if (s_newSample)
		winPush(s);
	/* MISS needs a target period: the cap, the Limiter under Uncapped, or
	 * the game's own rate under Default (the helper counts against the same). */
	bool missTarget = st.interval > 0
	               || (st.interval == FPSVITA_INTERVAL_UNCAPPED && st.softcap > 0)
	               || (st.interval == FPSVITA_INTERVAL_OFF && st.native_interval > 0);
	uint32_t missPctX10 = 0;
	bool haveMiss = false;
	if (missTarget && winFull()) {
		const HudSample &o = winOldest();
		uint32_t dp = s.presents - o.presents, dm = s.miss - o.miss;
		if (dp > 0 && dm <= dp) {
			missPctX10 = dm * 1000u / dp;
			haveMiss = true;
		}
	}
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
		/* One line, the Full rows' numbers in order (FPS, frametime, 1% low,
		 * CPU, INPUT, QUEUE), then the limit and what is active, in the short
		 * forms so the worst case fits FPSVITA_HUD_WIDTH_MINI. */
		wchar_t w1[16], w2[16], w3[16];
		buf[0] = L'\0';
		pos = appendW(buf, 0, L"%u.%u  %lsms  ", fps_i, fps_t, msText(w1, 16, s.frame_us));
		if (s.low1_fps_x10)
			pos = appendW(buf, pos, L"1%% %u  ", (s.low1_fps_x10 + 5) / 10);
		pos = appendW(buf, pos, L"CPU %ls  IN %ls  Q %ls  ",
		              msText(w1, 16, haveWork ? s.work_us : 0), msText(w2, 16, s.input_us),
		              msText(w3, 16, s.queue_us));
		bool autoOn = PanelAuto::Active();
		if (autoOn) {   /* the governor's step: the panel rate, or 30 at its floor */
			int hz = st.panel_hz > 0 ? (int)st.panel_hz : 60;
			pos = appendW(buf, pos, L"AUTO %d  ", st.interval == FPSVITA_INTERVAL_30FPS ? hz / 2 : hz);
		} else if (mode)
			pos = appendW(buf, pos, L"%ls  ", mode);
		else
			pos = appendW(buf, pos, L"LIMIT %d  ", capfps);
		if (s.flags & FPSVITA_STATF_INJECT)
			pos = appendW(buf, pos, L"VSYNC  ");
		if (natLL || onLL)
			pos = appendW(buf, pos, L"LL  ");
		else if (reqLL && initSeen)
			pos = appendW(buf, pos, L"LL?  ");   /* requested, applies at the next launch */
		else if (reqLL && s.fps_x10)
			pos = appendW(buf, pos, L"LL-  ");   /* requested, the init call never passed the hook */
		if (antilag)
			pos = appendW(buf, pos, L"AL  ");
		if (st.softcap > 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED)
			pos = appendW(buf, pos, L"LIM %d  ", (int)st.softcap);
		if (st.panel_hz > 0 && !autoOn)
			pos = appendW(buf, pos, L"%dHZ  ", (int)st.panel_hz);
		textSet(s_mini, buf);
		textSet(s_miniR, buf);
		return;
	}

	if (s.frame_us)
		setRow(ROW_FPS, L"%u.%u / %u.%u ms", fps_i, fps_t, MS_INT(s.frame_us), MS_TENTH(s.frame_us));
	else
		setRow(ROW_FPS, L"%u.%u", fps_i, fps_t);

	if (PanelAuto::Active()) {   /* the governor holds the rate: one form for every step */
		int hz = st.panel_hz > 0 ? (int)st.panel_hz : 60;
		int step = st.interval == FPSVITA_INTERVAL_30FPS ? hz / 2 : hz;   /* a 2x step: two refreshes per frame */
		setRow(ROW_LIMIT, L"AUTO %d @%dHZ", step, hz);
	}
	else if (st.panel_hz > 0 && mode)   /* panel rate: the limit is in its vblanks */
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

	/* Misses over the last window as a share of its presents, and the 1% low
	 * over the helper's last 512 presents: the "does it hold the rate" row. */
	if (haveMiss && s.low1_fps_x10)
		setRow(ROW_MISS, L"%u.%u%%  1%% LOW %u.%u", missPctX10 / 10, missPctX10 % 10,
		       s.low1_fps_x10 / 10, s.low1_fps_x10 % 10);
	else if (haveMiss)
		setRow(ROW_MISS, L"%u.%u%%", missPctX10 / 10, missPctX10 % 10);
	else if (s.low1_fps_x10)
		setRow(ROW_MISS, L"-  1%% LOW %u.%u", s.low1_fps_x10 / 10, s.low1_fps_x10 % 10);
	else
		setRow(ROW_MISS, L"-");

	/* Anti-lag: the fresh poll is always at work once enabled; the value is
	 * the whole-vblank sleep in force and the frames it has cost. */
	/* BTN: the share of the user's presses so far that reached the game
	 * before the vblank sample had them (blank until a press has happened).
	 * Each such press gains up to a refresh; the mean lead is in the rate
	 * line. The row holds about 19 characters next to its label: "16.6
	 * ms/0 BTN 100%" is the longest form (a 2026-10-28 screenshot with a
	 * longer suffix ran into the label). */
	/* BTN share, and STK when early sticks are trusted and delivering. Both
	 * fit the ~19 chars next to the label only in compact form, so the two
	 * share one token: " BTN 12% STK", " BTN 12%", or " STK". */
	wchar_t btn[24];
	btn[0] = L'\0';
	bool haveBtn = antilag && s.btn_presses;
	bool haveStk = antilag && (s.flags & FPSVITA_STATF_STICKS);
	if (haveBtn)
		sce_paf_swprintf(btn, 24, haveStk ? L" BTN %u%% STK" : L" BTN %u%%",
		                 (unsigned)(s.btn_early * 100u / s.btn_presses));
	else if (haveStk)
		sce_paf_swprintf(btn, 24, L" STK");
	if (!antilag)
		setRow(ROW_ANTILAG, L"OFF");
	else if (s.jit_slack_us)
		setRow(ROW_ANTILAG, L"%u.%u ms/%u%ls", MS_INT(s.jit_slack_us), MS_TENTH(s.jit_slack_us), (unsigned)s.jit_missed, btn);
	else
		setRow(ROW_ANTILAG, L"ON / %u%ls", (unsigned)s.jit_missed, btn);

	/* A title that runs a 1-deep queue by itself gets nothing from the
	 * option; say so instead of implying the user turned it on or off. */
	if (natLL)
		setRow(ROW_LOWLAT, L"ON (GAME'S OWN)");
	else if (reqLL && onLL)
		setRow(ROW_LOWLAT, L"ON");
	else if (reqLL && initSeen)
		setRow(ROW_LOWLAT, L"RESTART GAME");   /* the init call came before the tick */
	else if (reqLL && s.fps_x10)
		setRow(ROW_LOWLAT, L"NOT HOOKED");     /* the game presents, its init never passed the hook */
	else if (reqLL)
		setRow(ROW_LOWLAT, L"");               /* no init yet, no frames yet */
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
	/* A tip needs a baseline: the settle time and a full window of pushes
	 * gathered after it (the window starts with the settle timer, so the
	 * baseline is their sum), and each rule's reading must have held on
	 * every push in the window. A reading that wanders across a threshold
	 * produces no tip; fewer tips beat wrong ones. */
	bool settled = s_now - s_settleFrom >= FPSVITA_HUD_TIP_BASELINE_US && winFull();
	bool notHeld = false, oldSample = false, queued = false, queueBound = false;
	uint32_t workMax = 0;
	if (settled) {
		/* Panel auto acts on a limit the game cannot hold; its own tip would
		 * only contradict it (TRY 20 at the governor's 30 floor). */
		notHeld = capfps_tip > 0 && !PanelAuto::Active();
		oldSample = queued = queueBound = true;
		for (int i = 0; i < FPSVITA_HUD_WINDOW; i++) {
			const HudSample &w = s_win[i];
			/* A limit the game ran a tenth under on every push. (A rolling
			 * double-period count was tried and dropped: one cutscene load
			 * tripped it for the next 300 frames.) */
			if (w.fps >= (uint32_t)capfps_tip * 9u) notHeld = false;
			if (w.age < 10000u) oldSample = false;
			if (w.queue < vblank) queued = false;
			if (!w.frame || w.queue * 10u < w.frame * 8u) queueBound = false;
			if (w.work > workMax) workMax = w.work;
		}
	}
	/* Antilag's whole-vblank sleep only fits when the CPU work leaves a
	 * vblank of headroom after its margins (2.5 ms each for the margin and
	 * the fresh poll); MGS2 gameplay at 12 to 14 ms of work gained nothing. */
	bool sleepFits = haveGxm && (st.interval == FPSVITA_INTERVAL_30FPS || st.interval == FPSVITA_INTERVAL_20FPS)
	              && workMax + 5000u + vblank <= vblank * (uint32_t)st.interval;
	const wchar_t *compat = fpsvitaCompatWarn(fpsvitaCompatFlags((const char *)st.titleid),
	                                          st.interval, st.native_interval, s_readPeek);
	if (compat)
		s_tipCompat = compat;
	int cand;
	if (!settled)
		cand = TIP_WAIT;
	else if (notHeld)
		cand = TIP_NOTHELD;
	else if (!antilag && (oldSample || (sleepFits && queued)))
		cand = TIP_ANTILAG;
	else if (!reqLL && !natLL && !onLL && queueBound)
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
	const wchar_t *tip = L"";   /* waiting: nothing to say yet */
	switch (s_tipShown) {
	case TIP_NONE:    tip = L""; break;   /* nothing to suggest: say nothing */
	case TIP_NOTHELD: tip = NULL; break;   /* formatted below: the rates follow the panel */
	case TIP_ANTILAG: tip = L"TRY ANTI-LAG"; break;
	case TIP_LOWLAT:  tip = L"TRY LOW LAT"; break;
	case TIP_COMPAT:  tip = s_tipCompat; break;
	default: break;
	}
	/* At a 60fps limit (stock or a refresh-rate entry), Panel modes
	 * first offers a lower refresh rate the game clears by 15 percent:
	 * Doom64EX at 44 fps held only 37 to 38 at 40 Hz, choppier than 30.
	 * Never for a speed-tied title, where a lower rate is slow motion. */
	int tryHz = 0;
	if (!tip && st.interval == FPSVITA_INTERVAL_60FPS && fpsvitaPanelUi() && !PanelAuto::Active()
	    && !(fpsvitaCompatFlags((const char *)st.titleid) & FPSVITA_COMPAT_SPEED))
		for (int32_t i = 0; i < FPSVITA_PANEL_RATES_LEN && !tryHz; i++)
			if (fpsvita_panel_rates[i] < capfps_tip
			    && s.fps_x10 * 100u >= (uint32_t)fpsvita_panel_rates[i] * 1150u)
				tryHz = fpsvita_panel_rates[i];
	if (tryHz)
		setRow(ROW_TIP, L"NOT HELD, TRY %dHZ", tryHz);
	else if (!tip && st.interval == FPSVITA_INTERVAL_60FPS && st.panel_hz > 0)
		setRow(ROW_TIP, L"%d NOT HELD, TRY 30", capfps_tip);   /* the stock 30fps entry */
	else if (!tip && st.interval >= 1 && st.interval < 3)
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
