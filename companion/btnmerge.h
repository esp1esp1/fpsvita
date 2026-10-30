/*
 * Antilag's syscon presses: which physical buttons may be delivered from the
 * syscon's own word ahead of SceCtrl's vblank sample, and which stale presses
 * to hide once the syscon shows a release. Pure state machine, no sce calls:
 * tests/btnmerge.cpp drives it on the host.
 *
 * The helper's merge runs on the game's side of SceCtrl, after every kernel
 * plugin that reshapes the sample (reVita's remaps, turbo and sticky buttons,
 * hotkey removal, AnalogsEnhancer, ds34vita). It cannot see what they did, so
 * a physical bit is trusted only once it has proved to reach the game as
 * itself:
 *
 *   pass   A press of the bit (a syscon down edge) was followed within the
 *          window by the same bit in SceCtrl's sample. From then on the bit is
 *          added to every sample while the syscon holds it. The first press of
 *          each button goes at stock latency; a remapped or disabled button
 *          never qualifies. A constant word (a PS TV with no built-in pad)
 *          never shows an edge, so nothing qualifies there.
 *   revoke SceCtrl showed the bit down during a syscon hold and then up while
 *          the syscon still held it (turbo, a combo that consumed it): the bit
 *          leaves pass. Skipped on a read where the kernel's word changed since
 *          the previous read, since a release and press between two reads look
 *          like a hold.
 *   rel    A pass bit the syscon released while SceCtrl still shows it down:
 *          hidden from the sample until SceCtrl agrees, so the release reaches
 *          the game as early as the press did. If SceCtrl still holds it a
 *          window later the press was not ours (a sticky button, an injected
 *          press on the same button): the bit leaves pass and is shown again.
 */
#ifndef FPSVITA_BTNMERGE_H
#define FPSVITA_BTNMERGE_H

#include <stdint.h>

#define BTNMERGE_BITS 16   /* SceCtrl game buttons live in bits 0..15 */

typedef struct BtnMerge {
	uint32_t pass;    /* bits proven to reach the game unchanged */
	uint32_t prev;    /* the syscon mask at the previous read */
	uint32_t seen;    /* bits SceCtrl has shown down during the current syscon hold */
	uint32_t pend;    /* bits whose press waits for SceCtrl to confirm it */
	uint32_t rel;     /* pass bits the syscon released, SceCtrl not yet */
	uint32_t pend_at[BTNMERGE_BITS];   /* press seen at (us, any 32-bit clock) */
	uint32_t rel_at[BTNMERGE_BITS];    /* release seen at */
	uint32_t revokes; /* bits that lost pass (turbo, combo, sticky, injected): a counter */
	uint32_t epend;   /* pass bits pressed since the previous read, not yet seen by SceCtrl */
	uint32_t presses; /* physical presses (syscon down edges, per bit): a counter */
	uint32_t early;   /* of those, delivered before SceCtrl's sample had them: a counter */
} BtnMerge;

/* m: the syscon's game buttons down now (SceCtrl bit layout, pressed = 1).
 * sample: SceCtrl's newest sample, pressed = 1, before any change.
 * changed: the kernel's word changed since the previous read.
 * now, window: us. On return *add holds the bits to OR into every returned
 * sample and *clear the bits to remove; they never overlap. */
static inline void btnMerge(BtnMerge *b, uint32_t m, uint32_t sample, int changed,
                            uint32_t now, uint32_t window, uint32_t *add, uint32_t *clear)
{
	uint32_t down = m & ~b->prev;
	uint32_t up   = b->prev & ~m;
	uint32_t bit;
	int i;
	b->prev = m;

	/* Presses waiting for SceCtrl: confirmed by the same bit in the sample,
	 * dropped after the window (a remap, a tap SceCtrl never saw). */
	for (i = 0; i < BTNMERGE_BITS; i++) {
		bit = 1u << i;
		if (!(b->pend & bit)) continue;
		if (sample & bit) {
			b->pass |= bit;
			b->pend &= ~bit;
		} else if (now - b->pend_at[i] > window) {
			b->pend &= ~bit;
		}
	}
	for (i = 0; i < BTNMERGE_BITS; i++) {
		bit = 1u << i;
		if ((down & ~b->pass) & bit) {
			b->pend |= bit;
			b->pend_at[i] = now;
		}
	}

	/* Revoke: the game stopped seeing a bit the syscon still holds. */
	if (!changed) {
		uint32_t gone = b->seen & m & ~sample;
		if (gone & b->pass) b->revokes++;
		b->pass &= ~gone;
		b->seen &= ~gone;
	}
	b->seen = (b->seen | (m & sample)) & m;

	/* Releases of bits we delivered early: hide them until SceCtrl agrees. */
	for (i = 0; i < BTNMERGE_BITS; i++) {
		bit = 1u << i;
		if ((up & b->pass) & bit) {
			b->rel |= bit;
			b->rel_at[i] = now;
		}
	}
	for (i = 0; i < BTNMERGE_BITS; i++) {
		bit = 1u << i;
		if (!(b->rel & bit)) continue;
		if ((m & bit) || !(sample & bit)) {
			b->rel &= ~bit;                 /* pressed again, or SceCtrl caught up */
		} else if (now - b->rel_at[i] > window) {
			b->rel &= ~bit;                 /* still down a window later: not ours */
			b->pass &= ~bit;
			b->revokes++;
		}
	}

	*add = m & b->pass;
	*clear = b->rel & ~*add;

	/* Share of presses that gained: a press counts as early once a read
	 * delivers it before SceCtrl's sample shows it. */
	for (i = 0; i < BTNMERGE_BITS; i++)
		if (down & (1u << i)) b->presses++;
	b->epend = (b->epend | (down & b->pass)) & m;
	{
		uint32_t hit = b->epend & *add & ~sample;
		for (i = 0; i < BTNMERGE_BITS; i++)
			if (hit & (1u << i)) b->early++;
		b->epend &= ~(hit | sample);
	}
}

#endif
