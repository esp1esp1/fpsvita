/*
 * stkmerge.h - Antilag's early sticks. SceCtrl samples the analog sticks from
 * the syscon once per vblank; the kernel polls them every few ms
 * (fpsvitaGetSticks, dev). This hands the game a fresher stick reading, the
 * same idea as the syscon button presses, with a per-axis gate that keeps it
 * from fighting SceCtrl's own transform or a stick-reshaping plugin
 * (AnalogsEnhancer): it only delivers once the learned transform has matched
 * what SceCtrl actually reports, and stops the moment it stops matching.
 *
 * SceCtrl reports out = 128 + (raw - 128) * k (k ~ 0.818 in the stock sampling
 * mode, different in ANALOG_WIDE, so it is learned per title, 2026-10-28/29).
 * The scale is learned as an EMA of (sample - 128) / (raw - 128) over
 * deflected, off-rail pairs; an axis is trusted after STK_TRUST_MATCHES
 * samples where the prediction is within STK_RESID of SceCtrl's value, and
 * dropped after STK_DROP_MISSES that are not. At the rails SceCtrl walks the
 * output past the linear transform to 0/255, so rail reads are excluded from
 * both learning and delivery (delivering there would undershoot).
 *
 * Host test: tests/stkmerge.cpp. Plain C, no device headers.
 */
#ifndef FPSVITA_STKMERGE_H
#define FPSVITA_STKMERGE_H

#include <stdint.h>

#define STK_K_SHIFT       16
#define STK_K_ONE         (1 << STK_K_SHIFT)
#define STK_K_INIT        53608            /* 0.818 * 65536 */
#define STK_K_MIN         32768            /* 0.5, sanity floor on a learned ratio */
#define STK_K_MAX         131072           /* 2.0, sanity ceiling */
#define STK_TRUST_MATCHES 32
#define STK_DROP_MISSES   3
#define STK_RESID         5                /* counts: prediction vs SceCtrl */
#define STK_DEFLECT       24               /* |raw-128| to learn from (clear of centre filtering) */
#define STK_RAIL          10               /* raw within this of 0/255 = rail, skip */
#define STK_STEADY        6                /* max sample change since the last read to learn/verify */

typedef struct StkMerge {
	int32_t  k[4];           /* learned scale, Q16, per axis lx ly rx ry */
	int16_t  matches[4];     /* toward trust */
	int16_t  misses[4];      /* toward revoke */
	uint8_t  trust[4];
	uint8_t  prev[4];        /* last sample seen, to gate on motion */
	uint32_t learned;        /* axes that reached trust */
	uint32_t delivered;      /* samples overwritten */
	uint32_t revoked;        /* axes that lost trust */
} StkMerge;

static inline void stkMergeInit(StkMerge *s)
{
	int a;
	for (a = 0; a < 4; a++) {
		s->k[a] = STK_K_INIT;
		s->matches[a] = s->misses[a] = 0;
		s->trust[a] = 0;
		s->prev[a] = 128;
	}
	s->learned = s->delivered = s->revoked = 0;
}

static inline int stk_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int stk_pred(int32_t k, int raw) { return stk_clamp(128 + (((raw - 128) * k) >> STK_K_SHIFT)); }
static inline int stk_isRail(int v) { return v <= STK_RAIL || v >= 255 - STK_RAIL; }

/*
 * Learn and gate one axis from a pair: `raw` read nearest the sample's time,
 * `samp` what SceCtrl reported for that sample.
 */
static inline void stk_learn(StkMerge *s, int a, int raw, int samp)
{
	int32_t ratio, pred;
	int moved = samp - s->prev[a]; if (moved < 0) moved = -moved;
	s->prev[a] = (uint8_t)samp;
	if (moved > STK_STEADY)
		return;                                   /* moving fast: the raw-to-sample pairing is too loose to trust a match or a miss */
	if (stk_isRail(raw) || samp == 0 || samp == 255)
		return;                                   /* rail: SceCtrl walks past the transform */
	if (raw > 128 - STK_DEFLECT && raw < 128 + STK_DEFLECT)
		return;                                   /* too near centre to fit a scale */
	/* Update the EMA only from a sane ratio (converge faster until trusted),
	 * but account the match/miss on the current prediction either way, so a
	 * plugin that warps the sample (a wild ratio we skip) still counts as a
	 * mismatch and drops trust. */
	ratio = (int32_t)((((int64_t)(samp - 128)) << STK_K_SHIFT) / (raw - 128));
	if (ratio >= STK_K_MIN && ratio <= STK_K_MAX)
		s->k[a] += (ratio - s->k[a]) >> (s->trust[a] ? 4 : 2);
	pred = stk_pred(s->k[a], raw);
	if (pred - samp <= STK_RESID && samp - pred <= STK_RESID) {
		s->misses[a] = 0;
		if (!s->trust[a] && ++s->matches[a] >= STK_TRUST_MATCHES) {
			s->trust[a] = 1;
			s->learned++;
		}
	} else if (s->trust[a]) {
		if (++s->misses[a] >= STK_DROP_MISSES) {
			s->trust[a] = 0;
			s->matches[a] = s->misses[a] = 0;
			s->revoked++;
		}
	} else {
		s->matches[a] = 0;                        /* must be a clean run to earn trust */
	}
}

/*
 * Deliver: for each trusted axis, if the freshest raw read is younger than a
 * vblank and not at a rail, and the fresher prediction differs from SceCtrl's
 * value by more than the residual (i.e. the stick actually moved since that
 * vblank-old sample), overwrite it. Returns a bitmask of axes written.
 * `out` holds the sample's lx ly rx ry and is modified in place.
 */
static inline int stk_deliver(StkMerge *s, const uint8_t rawNow[4], int raw_fresh, uint8_t out[4])
{
	int a, m = 0;
	if (!raw_fresh)
		return 0;
	for (a = 0; a < 4; a++) {
		int raw, pred;
		if (!s->trust[a])
			continue;
		raw = rawNow[a];
		if (stk_isRail(raw))
			continue;                             /* keep SceCtrl's rail-walked value */
		pred = stk_pred(s->k[a], raw);
		if (pred - out[a] <= STK_RESID && out[a] - pred <= STK_RESID)
			continue;                             /* no meaningful change: leave it (no resting jitter) */
		out[a] = (uint8_t)pred;
		m |= 1 << a;
		s->delivered++;
	}
	return m;
}

#endif /* FPSVITA_STKMERGE_H */
