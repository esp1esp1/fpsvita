/* The compatibility table and its two lookups, in one translation unit.
 * compat.h used to define the ~170-entry table and inline functions in a
 * header, so every shell source that included it carried its own copy (three
 * copies, ~4 KB). Here it lives once; callers get declarations from compat.h. */
#include <stddef.h>
#define FPSVITA_COMPAT_DEFINE
#include "compat_data.h"
#include "compat.h"

namespace fpsvita {

unsigned fpsvitaCompatFlags(const char *tid)
{
	if (!tid || !tid[0])
		return 0;
	for (unsigned i = 0; i < fpsvita_compat_count; i++) {
		const char *id = fpsvita_compat[i].id;
		int j = 0;
		while (j < 9 && id[j] && id[j] == tid[j])
			j++;
		if (j == 9 && tid[j] == '\0')
			return fpsvita_compat[i].flags;
	}
	return 0;
}

const wchar_t *fpsvitaCompatWarn(unsigned flags, int32_t interval, int32_t native, bool readPeek)
{
	bool above = interval == FPSVITA_INTERVAL_60FPS || interval == FPSVITA_INTERVAL_UNCAPPED;
	if (interval == FPSVITA_INTERVAL_OFF)
		return NULL;
	if (native > 0 && interval == native)
		flags &= FPSVITA_COMPAT_SHAKE;
	if ((flags & FPSVITA_COMPAT_NOINPUT) && above && !readPeek)
		return L"NO INPUT ABOVE 59";
	if ((flags & FPSVITA_COMPAT_FREEZE) && above)
		return L"FREEZES, USE DEFAULT";
	if (flags & FPSVITA_COMPAT_SHAKE)
		return L"SHIP SHAKES WITH CAP";
	if ((flags & FPSVITA_COMPAT_VIDEO) && interval == FPSVITA_INTERVAL_60FPS)
		return L"VIDEOS BREAK AT 60";
	if ((flags & FPSVITA_COMPAT_MENUS) && above)
		return L"MENUS RUN FAST";
	if ((flags & FPSVITA_COMPAT_SLOW) && interval == FPSVITA_INTERVAL_20FPS)
		return L"SLOW MOTION AT 20";
	if (flags & FPSVITA_COMPAT_SPEED)
		return L"SPEED TIED TO FPS";
	return NULL;
}

} // namespace fpsvita
