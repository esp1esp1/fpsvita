#pragma once

#include "compat_data.h"
#include "../common/fpsvita_common.h"

/* The compatibility table (compat_data.h) and these two lookups live in
 * compat.cpp; everyone else gets the enum, the declarations here, and no
 * per-file copy of the ~170-entry table. */
namespace fpsvita {

unsigned fpsvitaCompatFlags(const char *tid);

/* The known problem at this limit, or NULL when the table names none for it.
 * The overlay shows it as the last-resort tip and the quick menu marks the
 * same case with (!). read=peek is the fix for the missing input (NOINPUT).
 * A limit equal to the game's own rate (native, 0 or less when unknown)
 * changes nothing the rate problems depend on; a cap still shakes WipEout.
 * Each string fits the ~20 characters next to the TIP label. */
const wchar_t *fpsvitaCompatWarn(unsigned flags, int32_t interval, int32_t native, bool readPeek);

} // namespace fpsvita
