/* Panel auto: see panelauto.h. */

#include "panelauto.h"
#include "compat.h"

namespace fpsvita {

#define PA_WINDOW_US     2000000ull    /* decide every 2 s */
#define PA_PROBE_MIN_US  10000000ull   /* first up-probe after 10 s clean */
#define PA_PROBE_MAX_US  120000000ull
#define PA_PROBE_FAIL_US 8000000ull    /* a down-step this soon after an up-step is a failed probe */
#define PA_OWN_US        1500000ull
#define PA_HOLD_PCT      90           /* frames over wall time; loading stalls cost a few percent without doubled frames */
#define PA_FLOOR_MAX_US  40000000ull  /* the floor's probe backoff cap: a menu must not pin the game at 30 */
#define PA_MISS_PCT      3

static bool     s_active;
static uint32_t s_title[3];
static int32_t  s_rate;          /* 60, 50, 45, 40 or 30: the step in force last seen */
static int32_t  s_ceiling;       /* the user's pick in the FPS row: never probed above */
static int32_t  s_base;          /* the user's interval under the panel steps: 60fps or Default */
static bool     s_atThirty;      /* the 30 step (a 30fps limit at the stock refresh) is ours */
static uint32_t s_lastSeq;
static uint64_t s_winStart, s_clean, s_lastUp, s_lastOwn;
static uint64_t s_probeWait[5];  /* per step probed: doubled when a probe of that step fails */
static int32_t  s_goodRate;      /* the rate an up-probe left: where a failed probe returns to */
static int      s_badWins;       /* consecutive windows the rate was not held */
static uint32_t s_winN, s_presents0, s_miss0;
static uint64_t s_fpsSum;

/* The ladder, top to bottom. 30 is the 30fps limit at the stock refresh
 * (steady 33 ms frames, input still sampled at 60 Hz), not a 30 Hz panel. */
static const int32_t s_steps[] = { 60, 50, 45, 40, 30 };
#define PA_STEPS ((int32_t)(sizeof(s_steps) / sizeof(s_steps[0])))

static int32_t stepIdx(int32_t hz)
{
	for (int32_t i = 0; i < PA_STEPS; i++)
		if (s_steps[i] == hz) return i;
	return 0;
}

/* The wait before probing the step above `rate`: that step's own backoff.
 * From the floor it is capped at 40 s: a failed probe there costs a 30 fps
 * menu little, and the game must not sit at 30 long after its menus. */
static uint64_t probeWait(int32_t rate)
{
	int32_t up = stepIdx(rate) - 1;
	uint64_t w = up >= 0 ? s_probeWait[up] : PA_PROBE_MIN_US;
	if (rate == s_steps[PA_STEPS - 1] && w > PA_FLOOR_MAX_US)
		w = PA_FLOOR_MAX_US;
	return w;
}

static void winReset(uint64_t now)
{
	s_winStart = now;
	s_winN = 0;
	s_fpsSum = 0;
}

static void reset(uint64_t now)
{
	winReset(now);
	s_clean = 0;
	for (int32_t i = 0; i < PA_STEPS; i++)
		s_probeWait[i] = PA_PROBE_MIN_US;
	s_lastUp = 0;
	s_rate = 0;
	s_ceiling = 0;
	s_goodRate = 0;
	s_badWins = 0;
	s_atThirty = false;
}

static void apply(int32_t hz, uint64_t now, uint32_t fps_x10, uint32_t missPct)
{
	if (hz == 30) {
		fpsvitaSetPanel(0);
		fpsvitaSetInterval(FPSVITA_INTERVAL_30FPS);
		s_atThirty = true;
	} else {
		if (s_atThirty) {
			fpsvitaSetInterval(s_base);
			s_atThirty = false;
		}
		fpsvitaSetPanel(hz == 60 ? 0 : hz);
	}
	s_lastOwn = now;
	s_rate = hz;
}

bool PanelAuto::Active() { return s_active; }
bool PanelAuto::UserPick(int32_t *interval, int32_t *panel)
{
	if (!s_active || !s_ceiling)
		return false;
	*interval = s_base;
	*panel = s_ceiling == 60 ? 0 : s_ceiling;
	return true;
}
bool PanelAuto::OwnChange(uint64_t now) { return s_lastOwn && now - s_lastOwn < PA_OWN_US; }

void PanelAuto::Tick(const FpsvitaState &st, uint64_t now, bool game)
{
	if (!g_panelAuto || !game || !fpsvitaPanelUi()) {
		if (s_active && game && s_atThirty) {
			/* Auto unticked while holding our 30: give the user's limit back. */
			fpsvitaSetInterval(s_base);
			s_atThirty = false;
			s_lastOwn = now;
		}
		s_active = false;
		return;
	}
	if (st.titleid[0] != s_title[0] || st.titleid[1] != s_title[1] || st.titleid[2] != s_title[2]) {
		sce_paf_memcpy(s_title, st.titleid, sizeof(s_title));
		reset(now);
	}
	/* From the 60fps entry (stock or a refresh-rate one), or from Default on
	 * a game whose own rate is 60: it waits one vblank per frame, so a lower
	 * panel rate gives one frame per refresh, and our own 30 step. A
	 * 30-native game at Default would halve (20 fps at 40 Hz); a 30 or 20
	 * limit the user set, Uncapped and the Limiter are their choice. */
	bool sixty = st.interval == FPSVITA_INTERVAL_60FPS
	          || (st.interval == FPSVITA_INTERVAL_OFF && st.native_interval == FPSVITA_INTERVAL_60FPS);
	bool ours30 = st.interval == FPSVITA_INTERVAL_30FPS && s_atThirty;
	bool engaged = (sixty || ours30) && st.softcap == 0
	            && !(fpsvitaCompatFlags((const char *)st.titleid) & FPSVITA_COMPAT_SPEED);
	if (!engaged) {
		if (s_active) reset(now);
		s_active = false;
		return;
	}
	s_active = true;

	int32_t rate = ours30 ? 30 : st.panel_hz > 0 ? st.panel_hz : 60;
	if (rate != s_rate) {
		/* Not ours landing: the user's pick in the FPS row is the ceiling
		 * (60fps, Default or a refresh-rate entry), and a new baseline. */
		if (!OwnChange(now) || s_rate == 0) {
			s_ceiling = rate;
			s_base = st.interval;
			s_atThirty = false;
			for (int32_t i = 0; i < PA_STEPS; i++)
				s_probeWait[i] = PA_PROBE_MIN_US;
			s_lastUp = 0;
			s_goodRate = 0;
		}
		s_rate = rate;
		winReset(now);
		s_clean = 0;
		s_badWins = 0;
	}
	if (st.stats.seq == s_lastSeq)
		return;
	s_lastSeq = st.stats.seq;
	if (!st.stats.fps_x10)
		return;   /* loading: no frames to judge */
	if (s_winN == 0) {
		s_winStart = now;
		s_presents0 = st.stats.presents;
		s_miss0 = st.stats.miss;
	}
	s_fpsSum += st.stats.fps_x10;
	s_winN++;
	if (now - s_winStart < PA_WINDOW_US)
		return;

	/* The window's rate from frames over wall time: the helper's EMA lags
	 * and a single load hitch drags it for seconds (THUG read 37.5 at a
	 * held 40 with no doubled frame). */
	uint32_t dp = st.stats.presents - s_presents0, dm = st.stats.miss - s_miss0;
	uint64_t span = now - s_winStart;
	uint32_t mean = span ? (uint32_t)((uint64_t)dp * 10000000ull / span) : 0;   /* x10 */
	(void)s_fpsSum;
	uint32_t missPct = dp ? dm * 100u / dp : 0;
	bool holds = mean >= (uint32_t)rate * 10u * PA_HOLD_PCT / 100u && missPct <= PA_MISS_PCT;
	winReset(now);

	if (!holds) {
		s_clean = 0;
		s_badWins++;
		/* A failed probe acts at once; anything else waits for a second bad
		 * window, so a level load (THUG: 30 fps for 2 s) does not drop the
		 * rate the game then has to climb back to. */
		bool probeFail = s_lastUp && now - s_lastUp < PA_PROBE_FAIL_US;
		if (!probeFail && s_badWins < 2)
			return;
		if (rate > s_steps[PA_STEPS - 1]) {
			/* Straight to the highest step the measured fps clears, else the
			 * bottom. A failed probe goes back where it came from instead:
			 * the fps at a rate the game misses is dragged down by the
			 * doubled frames (THUG read 43 at 60 having just held 50). */
			/* A doubled frame holds the panel for two periods but counts as
			 * one present, so the measured fps undercounts what the game could
			 * do at a rate it holds: 50 with a fifth doubled read 39.9 where
			 * 45 was right. Credit the doubled share back before choosing. */
			uint32_t able = mean + mean * missPct / 100u;
			int32_t target = s_steps[PA_STEPS - 1];
			for (int32_t i = 0; i < PA_STEPS; i++)
				if (s_steps[i] < rate && (uint32_t)s_steps[i] * 10u <= able) {
					target = s_steps[i];
					break;
				}
			if (probeFail) {
				if (s_goodRate > 0 && s_goodRate < rate)
					target = s_goodRate;
				uint64_t *w = &s_probeWait[stepIdx(rate)];   /* this step failed */
				if (*w < PA_PROBE_MAX_US)
					*w *= 2;
			}
			s_lastUp = 0;
			s_badWins = 0;
			apply(target, now, mean, missPct);
		}
		return;
	}

	s_badWins = 0;
	s_clean += PA_WINDOW_US;
	if (s_clean >= 60000000ull)
		for (int32_t i = 0; i < PA_STEPS; i++)
			if (s_probeWait[i] > PA_PROBE_MIN_US) s_probeWait[i] /= 2;
	if (rate < s_ceiling && s_clean >= probeWait(rate)) {
		int32_t up = s_ceiling;
		for (int32_t i = PA_STEPS - 1; i >= 0; i--)
			if (s_steps[i] > rate) { up = s_steps[i] < s_ceiling ? s_steps[i] : s_ceiling; break; }
		s_lastUp = now;
		s_goodRate = rate;
		s_clean = 0;
		apply(up, now, mean, missPct);
	}
}

} /* namespace fpsvita */
