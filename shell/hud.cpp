/*
 * fpsvita overlay (see hud.h). Styled after PSVshellPlus's HUD: Mini is two
 * green lines with no box, Full a dialog box with uppercase labels on the
 * left and right-aligned values.
 */

#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include "hud.h"

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
#define FPSVITA_HUD_ROWS 9
#define FPSVITA_HUD_WIDTH 350.0f           /* same as PSVshellPlus's box */
#define FPSVITA_HUD_HEIGHT_FULL 250.0f     /* 9 rows of 26 px */
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
static uint32_t s_lastSeq = 0;
static int32_t  s_lastInterval = 0x7fffffff;
static int32_t  s_lastNative = -1;
static int32_t  s_lastAdv = -1, s_lastSoftcap = -1;

/* Row names borrow PC terms (PresentMon "CPU busy", Reflex-style latency):
 * INPUT is the frame's own newest controller sample to its flip, QUEUE its
 * submit-to-flip, both measured per frame by the helper. */
enum { ROW_FPS, ROW_LIMIT, ROW_CPU, ROW_INPUT, ROW_QUEUE, ROW_ANTILAG, ROW_LOWLAT, ROW_LIMITER, ROW_TITLE };
static const wchar_t *const s_labels[FPSVITA_HUD_ROWS] = {
	L"FPS:", L"LIMIT:", L"CPU:", L"INPUT:", L"QUEUE:", L"ANTI-LAG:", L"LOW LAT:", L"LIMITER:", L"TITLE:"
};

static ui::Text *findText(const char *id)
{
	return (ui::Text *)s_page->FindChild(IDParam(id), 0);
}

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

	/* Force a redraw on the next poll. */
	s_lastSeq = 0;
	s_lastInterval = 0x7fffffff;
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
	uint32_t now = sceKernelGetProcessTimeLow();
	if (now - s_lastPoll < FPSVITA_HUD_POLL_US)
		return;
	s_lastPoll = now;

	FpsvitaState st;
	sce_paf_memset(&st, 0, sizeof(st));
	fpsvitaGetState(&st);

	/* The kernel clears the title id when the owning process exits. */
	bool game = (st.titleid[0] & 0xFFu) != 0;
	SetVisible(game);
	if (!game)
		return;

	if (st.stats.seq == s_lastSeq && st.interval == s_lastInterval
	    && st.native_interval == s_lastNative && st.adv_flags == s_lastAdv
	    && st.softcap == s_lastSoftcap)
		return;
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
	int capfps = fpsvitaIntervalToFps(st.interval);
	int nat = fpsvitaIntervalToFps(st.native_interval);
	int reqLL = (st.adv_flags & FPSVITA_ADV_LOWLAT) != 0;
	int onLL = (s.flags & FPSVITA_STATF_LOWLATENCY) != 0;      /* clamped by the helper */
	int natLL = (s.flags & FPSVITA_STATF_LOWLAT_NATIVE) != 0;  /* the title's own setting */
	int antilag = (st.adv_flags & FPSVITA_ADV_JIT) != 0;
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
			pos = appendW(buf, pos, L" / GAME %d", nat);
		textSet(s_mini[0], buf);

		buf[0] = L'\0';
		pos = 0;
		if (s.flags & FPSVITA_STATF_INJECT)
			pos = appendW(buf, pos, L"VSYNC  ");
		if (natLL)
			pos = appendW(buf, pos, L"LOW LAT (GAME)  ");
		else if (reqLL && onLL)
			pos = appendW(buf, pos, L"LOW LAT  ");
		else if (reqLL)
			pos = appendW(buf, pos, L"LOW LAT PENDING  ");
		else if (onLL)
			pos = appendW(buf, pos, L"LOW LAT UNTIL RESTART  ");
		if (antilag) {
			if (s.jit_slack_us)
				pos = appendW(buf, pos, L"ANTI-LAG %u.%u ms  ", MS_INT(s.jit_slack_us), MS_TENTH(s.jit_slack_us));
			else
				pos = appendW(buf, pos, L"ANTI-LAG  ");
		}
		if (st.softcap > 0 && st.interval == FPSVITA_INTERVAL_UNCAPPED)
			pos = appendW(buf, pos, L"LIMITER %d  ", (int)st.softcap);
		textSet(s_mini[1], buf);
		return;
	}

	if (s.frame_us)
		setRow(ROW_FPS, L"%u.%u / %u.%u ms", fps_i, fps_t, MS_INT(s.frame_us), MS_TENTH(s.frame_us));
	else
		setRow(ROW_FPS, L"%u.%u", fps_i, fps_t);

	if (mode)
		setRow(ROW_LIMIT, nat > 0 ? L"%ls / GAME %d" : L"%ls", mode, nat);
	else
		setRow(ROW_LIMIT, nat > 0 ? L"%d / GAME %d" : L"%d FPS", capfps, nat);

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
	if (!antilag)
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

	setRow(ROW_TITLE, L"%ls", wtid);
}

} /* namespace fpsvita */
