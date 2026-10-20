#pragma once

#include <psp2/kernel/modulemgr.h>
#include <paf.h>
#include "../common/fpsvita_common.h"

namespace fpsvita {

class Impose
{
public:

	Impose(paf::Plugin *plugin, paf::ui::Box *root);

	~Impose();

	/* False when the RCO template did not open or a required widget was
	 * missing. The caller must delete the object instead of using it. */
	bool IsValid() const { return valid; }

	static void ModeDownCBFun(int32_t type, paf::ui::Handler *self,
	                          paf::ui::Event *e, void *userdata);
	static void ModeUpCBFun(int32_t type, paf::ui::Handler *self,
	                        paf::ui::Event *e, void *userdata);
	void CycleMode(int32_t dir);
	static void InjectCBFun(int32_t type, paf::ui::Handler *self,
	                         paf::ui::Event *e, void *userdata);
	static void SaveCBFun(int32_t type, paf::ui::Handler *self,
	                       paf::ui::Event *e, void *userdata);
	static void HudDownCBFun(int32_t type, paf::ui::Handler *self,
	                         paf::ui::Event *e, void *userdata);
	static void HudUpCBFun(int32_t type, paf::ui::Handler *self,
	                       paf::ui::Event *e, void *userdata);
	static void HudPosDownCBFun(int32_t type, paf::ui::Handler *self,
	                            paf::ui::Event *e, void *userdata);
	static void HudPosUpCBFun(int32_t type, paf::ui::Handler *self,
	                          paf::ui::Event *e, void *userdata);
	void CycleHud(int32_t dir);
	void CycleHudPos(int32_t dir);
	static void LowlatCBFun(int32_t type, paf::ui::Handler *self,
	                        paf::ui::Event *e, void *userdata);
	static void JitCBFun(int32_t type, paf::ui::Handler *self,
	                     paf::ui::Event *e, void *userdata);
	static void SoftcapDownCBFun(int32_t type, paf::ui::Handler *self,
	                             paf::ui::Event *e, void *userdata);
	static void SoftcapUpCBFun(int32_t type, paf::ui::Handler *self,
	                           paf::ui::Event *e, void *userdata);
	void CycleSoftcap(int32_t dir);
	void ApplyAdvanced();
	static void Update(void *arg);

private:

	void UpdateModeLabel();
	void UpdateWarning();
	void UpdateInjectState();
	void UpdateHudLabels();
	void UpdateSoftcapLabel();
	void PersistState();
	void SavePerGame();
	void ReadTitleId();

	int32_t FindCycleIndex(int32_t interval);

	paf::Plugin *plugin;
	paf::Plugin *imposePlugin;
	paf::ui::Box *root;

	paf::ui::Text *modeText;
	paf::ui::Text *warnText;
	paf::ui::CheckBox *injectBox;
	paf::ui::Text *injectLabel;
	paf::ui::Widget *saveButton;
	paf::ui::Text *hudText;
	paf::ui::Text *hudPosText;
	paf::ui::CheckBox *llBox;
	paf::ui::CheckBox *jitBox;
	paf::ui::Text *softcapText;

	int32_t currentInterval;
	int32_t currentInject;
	int32_t autodetect;
	int32_t nativeInterval;
	int32_t advFlags;     /* FPSVITA_ADV_* for the title on screen (or the globals) */
	int32_t softcap;
	char titleid[FPSVITA_TITLEID_LEN];
	bool hasGame;
	bool readPeek;     /* the title's state.txt line has read=peek */
	bool valid;
	bool registered;   /* Update was added to MainThreadCallList */
};

} /* namespace fpsvita */
