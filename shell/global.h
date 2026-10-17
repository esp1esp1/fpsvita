#pragma once

#include <psp2/kernel/modulemgr.h>
#include <paf.h>
#include "../common/fpsvita_common.h"

extern paf::Plugin *g_fpsvitaPlugin;
extern paf::ui::ScrollView *g_imposeRoot;

/*
 * The player's global interval, as opposed to whatever a running game's
 * per-game override has temporarily put in the kernel. Seeded from state.txt
 * at boot and updated when the player changes the mode with no game running.
 */
extern int32_t g_globalInterval;

/* Overlay mode and position (state.txt `hud=` and `hudpos=`), owned by the
 * shell and mirrored into the kernel so the helper knows whether to push
 * stats. See hud.h. */
extern int32_t g_hudMode;
extern int32_t g_hudPos;

/* Global latency defaults (`lowlatency=`, `jit=`, `softcap=`). A per-game
 * line overrides them. */
extern int32_t g_lowLatency;
extern int32_t g_jit;
extern int32_t g_softcap;
extern int32_t g_tear;

/* Called by ~Impose so the shell re-arms for the next quick menu open.
 * Defined in main.cpp, which owns the open/closed flag. */
void fpsvitaImposeClosed(void);

/* boot.log helpers, defined in main.cpp */
void dbg_log(const char *msg, int len);
void dbg_hex(const char *tag, uint32_t val);
#ifndef DBG
#define DBG(msg) dbg_log(msg "\n", sizeof(msg "\n") - 1)
#endif

/* ui::Text::SetString(wstring const&, int) is a real ScePafWidget export.
 * Bind it by name rather than trusting the virtual slot ScePaf-RE declares. */
extern "C" int32_t fpsvita_textSetString(void *self, const paf::wstring &str, int32_t obj)
    __asm__("_ZN3paf2ui4Text9SetStringERKNS_12basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEEEi");

/* Show/hide via Widget::PlayAnimation(Reverse)(Animation_Fadein1, cb, ud),
 * real exports that PSVshellPlus uses. ScePaf-RE's Widget::Show/Hide are
 * not exported by this firmware (see paf_stubs/gen_yml.py). */
extern "C" void fpsvita_playAnimation(void *self, int32_t anim,
                                      void (*cb)(int32_t, void *, int32_t, void *), void *ud)
    __asm__("_ZN3paf6widget6Widget13PlayAnimationENS1_9AnimationEPFviPS1_iPvES4_");
extern "C" void fpsvita_playAnimationReverse(void *self, int32_t anim,
                                             void (*cb)(int32_t, void *, int32_t, void *), void *ud)
    __asm__("_ZN3paf6widget6Widget20PlayAnimationReverseENS1_9AnimationEPFviPS1_iPvES4_");
#define FPSVITA_ANIM_FADEIN1 0

static inline void widgetShow(paf::ui::Widget *w)
{
	if (w) fpsvita_playAnimation(w, FPSVITA_ANIM_FADEIN1, NULL, NULL);
}

static inline void widgetHide(paf::ui::Widget *w)
{
	if (w) fpsvita_playAnimationReverse(w, FPSVITA_ANIM_FADEIN1, NULL, NULL);
}

static inline void textSet(paf::ui::Text *t, const wchar_t *s)
{
	if (!t) return;
	paf::wstring w(s);
	fpsvita_textSetString(t, w, 0);
}

/* Index of a softcap value in fpsvita_softcap_cycle, 0 (off) if not listed. */
static inline int32_t fpsvitaSoftcapIndex(int32_t v)
{
	for (int32_t i = 0; i < (int32_t)FPSVITA_SOFTCAP_CYCLE_LEN; i++)
		if (fpsvita_softcap_cycle[i] == v) return i;
	return 0;
}

/* 60 / interval for the three capped modes, 0 for Off, Uncapped and unknown. */
static inline int fpsvitaIntervalToFps(int32_t interval)
{
	return (interval >= 1 && interval <= 3) ? 60 / interval : 0;
}
