#pragma once

#include "global.h"

namespace fpsvita {

/*
 * The overlay: a page of our own RCO plugin drawn above the running game,
 * the way PSVshellPlus draws its HUD. One template holds every widget; the
 * mode only decides which of them are visible, and the position moves the
 * root plane. The page is opened once and never closed (Plugin::PageClose
 * has no verified NID on this firmware), so Off just hides the root.
 *
 * Numbers come from the helper through the kernel (FpsvitaStats), polled by
 * a main-thread task while the HUD is on. Everything here runs on the PAF
 * main thread: the quick-menu callbacks, the boot task, and Update itself.
 */
class Hud
{
public:
	/* Push g_hudMode/g_hudPos to the kernel and make the overlay match. */
	static void Apply();

	/* module_stop: stop the task, hide the overlay. */
	static void Shutdown();

private:
	static bool Open();
	static void Update(void *arg);
	static void SetPosition();
	static void SetVisible(bool on);
	static void Refresh(const FpsvitaState &st);
};

} /* namespace fpsvita */
