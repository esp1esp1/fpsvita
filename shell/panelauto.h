#pragma once

#include "global.h"

namespace fpsvita {

/*
 * Panel auto (`panelauto=1`, the Panel row's Auto box): a governor that
 * keeps the game at the highest step it holds: 60, 55, 50, 48, 45, 40 (panel
 * rates, one frame per refresh), then 30, 28, 26, 24, 22, 20 (the 30fps
 * interval at a panel of twice the rate: every frame shown for exactly two
 * refreshes), the way the "VRR" Switch homebrew does with its display
 * clocks. Engaged while the FPS row is at 60fps or one of the
 * refresh-rate entries, or at Default on a 60-native game, with Modes on,
 * the title not speed-tied and a panel to drive. Fed by the stats push from Hud::Update (the shell asks
 * for the push with FPSVITA_HUD_STATS even with the overlay off).
 *
 * Down: after 2 s of a rate the game runs under 95% of, or misses 3% of
 * its frames at, straight to the highest rate the measured fps clears.
 * Up: one step after a clean streak of probeWait (10 s, doubled each time
 * a probe fails within 8 s, up to 2 min, halved after a minute clean).
 * The user's pick in the FPS row is the ceiling: Auto ranges from 20 up to it.
 */
class PanelAuto
{
public:
	static void Tick(const FpsvitaState &st, uint64_t now, bool game);
	static bool Active();                 /* managing the rate right now */
	static bool OwnChange(uint64_t now);  /* a limit/panel change within 1.5 s was the governor's */
	/* While in charge: the user's own pick (interval and panel rate, 0 for
	 * stock), for the quick menu to show and Save to write instead of the
	 * governor's current step. Returns false when not in charge. */
	static bool UserPick(int32_t *interval, int32_t *panel);
};

} /* namespace fpsvita */
