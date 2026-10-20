/*
 * Lite build of the helper (FPSVITA_LITE): no kernel module, no quick menu.
 * The state the full build keeps in fpsvita-kernel.skprx lives here, in the
 * game process, seeded from config.txt and state.txt in module_start. Same
 * sanitising as the kernel setters.
 */
#pragma once
#include "fpsvita_common.h"

#define LITE_UNUSED __attribute__((unused))

static FpsvitaState s_kstate = { FPSVITA_INTERVAL_OFF, FPSVITA_INJECT_OFF, FPSVITA_AUTODETECT_ON,
                                 0, {0, 0, 0}, FPSVITA_HUD_OFF, 0, 0, 0, {0} };

LITE_UNUSED static int fpsvitaGetState(FpsvitaState *out) { *out = s_kstate; return 0; }

LITE_UNUSED static void fpsvitaSetInterval(int32_t v)
{
	switch (v) {
	case FPSVITA_INTERVAL_OFF: case FPSVITA_INTERVAL_UNCAPPED:
	case FPSVITA_INTERVAL_60FPS: case FPSVITA_INTERVAL_30FPS: case FPSVITA_INTERVAL_20FPS:
		s_kstate.interval = v; return;
	default:
		s_kstate.interval = FPSVITA_INTERVAL_OFF;
	}
}
LITE_UNUSED static void fpsvitaSetInject(int32_t v) { s_kstate.inject = (v == FPSVITA_INJECT_ON); }
LITE_UNUSED static void fpsvitaSetAutodetect(int32_t v) { s_kstate.autodetect = (v != FPSVITA_AUTODETECT_OFF); }
LITE_UNUSED static void fpsvitaSetNativeInterval(int32_t v) { s_kstate.native_interval = v; }
LITE_UNUSED static void fpsvitaSetTitleIdPart(int32_t part, uint32_t v)
{ if (part >= 0 && part < 3) s_kstate.titleid[part] = v; }
LITE_UNUSED static void fpsvitaSetHud(int32_t mode, int32_t pos) { (void)mode; (void)pos; }
LITE_UNUSED static void fpsvitaSetAdvanced(int32_t flags, int32_t sc)
{
	s_kstate.adv_flags = flags & (FPSVITA_ADV_LOWLAT | FPSVITA_ADV_JIT | FPSVITA_ADV_AUTO);   /* no kernel: no tear */
	s_kstate.softcap = (sc >= FPSVITA_SOFTCAP_MIN && sc <= FPSVITA_SOFTCAP_MAX) ? sc : 0;
}
LITE_UNUSED static void fpsvitaSetStats(const FpsvitaStats *in) { (void)in; }
LITE_UNUSED static int fpsvitaSetFrameBufImmediate(const void *fb) { (void)fb; return -1; }
LITE_UNUSED static void fpsvitaSetPanel(int32_t hz) { (void)hz; }   /* no kernel: stock refresh */
