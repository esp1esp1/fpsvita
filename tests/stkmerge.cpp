/*
 * Host test for companion/stkmerge.h: Antilag's early sticks.
 *
 * Model: a stick's true position moves over time. The syscon (our kernel poll)
 * sees it almost now; SceCtrl's sample is one vblank (16.7 ms) behind and runs
 * the raw through out = 128 + (raw-128)*k, walking to 0/255 at the rails. A
 * plugin may reshape SceCtrl's sample. Each step feeds a (raw-at-sample,
 * sample) pair to learn/gate and then asks what the game would get.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../companion/stkmerge.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int scectrl(int raw, double k)   /* SceCtrl's transform, no rail walk */
{
	int v = (int)(128 + (raw - 128) * k + (raw > 128 ? 0.5 : -0.5));
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Feed `n` deflected samples at a fixed raw so an axis can earn trust. */
static void soak(StkMerge *s, int axis, int raw, double k, int n)
{
	uint8_t rawv[4] = {128, 128, 128, 128};
	for (int i = 0; i < n; i++)
		stk_learn(s, axis, raw, scectrl(raw, k));
	(void)rawv;
}

int main(void)
{
	/* 1. An axis earns trust from consistent deflected pairs, then delivers a
	 *    fresher value. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		CHECK(s.trust[0] == 1);
		CHECK(s.learned >= 1);
		/* k learned near 0.818 */
		CHECK(s.k[0] > 50000 && s.k[0] < 57000);
		/* fresh raw shows more deflection than a vblank-old sample -> delivered */
		uint8_t raw[4] = {220, 128, 128, 128};
		uint8_t out[4] = {(uint8_t)scectrl(180, 0.818), 128, 128, 128};  /* stale */
		int m = stk_deliver(&s, raw, 1, out);
		CHECK((m & 1) == 1);
		CHECK(out[0] == (uint8_t)stk_pred(s.k[0], 220));
	}

	/* 2. Rails: never delivered (SceCtrl walks past the transform), never learned. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		uint8_t raw[4] = {255, 128, 128, 128};
		uint8_t out[4] = {200, 128, 128, 128};
		int m = stk_deliver(&s, raw, 1, out);
		CHECK((m & 1) == 0);
		CHECK(out[0] == 200);                 /* untouched */
		/* a rail pair does not move the gate */
		StkMerge s2; stkMergeInit(&s2);
		for (int i = 0; i < 50; i++) stk_learn(&s2, 0, 255, 255);
		CHECK(s2.trust[0] == 0);
	}

	/* 3. Stale raw (older than a vblank) is never delivered. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		uint8_t raw[4] = {220, 128, 128, 128};
		uint8_t out[4] = {150, 128, 128, 128};
		CHECK(stk_deliver(&s, raw, 0, out) == 0);
		CHECK(out[0] == 150);
	}

	/* 4. Resting: prediction matches the sample, nothing is overwritten. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		int rest = 113;
		uint8_t raw[4] = {(uint8_t)rest, 128, 128, 128};
		uint8_t out[4] = {(uint8_t)scectrl(rest, 0.818), 128, 128, 128};
		CHECK(stk_deliver(&s, raw, 1, out) == 0);
	}

	/* 5. A plugin reshapes the sample so it no longer matches the transform:
	 *    trust is dropped and nothing is delivered (plugin preserved). */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		CHECK(s.trust[0] == 1);
		/* plugin now halves deflection: sample no longer predicted by k */
		for (int i = 0; i < STK_DROP_MISSES + 1; i++)
			stk_learn(&s, 0, 200, scectrl(200, 0.818) / 2 + 64);
		CHECK(s.trust[0] == 0);
		CHECK(s.revoked >= 1);
		uint8_t raw[4] = {210, 128, 128, 128};
		uint8_t out[4] = {150, 128, 128, 128};
		CHECK(stk_deliver(&s, raw, 1, out) == 0);
		CHECK(out[0] == 150);
	}

	/* 6. ANALOG_WIDE: a different scale is learned, and delivery uses it. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 1, 210, 1.05, STK_TRUST_MATCHES + 40);
		CHECK(s.trust[1] == 1);
		CHECK(s.k[1] > 62000);                /* ~1.05 * 65536 = 68813, learned above stock */
		uint8_t raw[4] = {128, 205, 128, 128};
		uint8_t out[4] = {128, (uint8_t)scectrl(170, 1.05), 128, 128};
		int m = stk_deliver(&s, raw, 1, out);
		CHECK((m & 2) == 2);
	}

	/* 7. Axes are independent: learning lx leaves ly/rx/ry untrusted. */
	{
		StkMerge s; stkMergeInit(&s);
		soak(&s, 0, 200, 0.818, STK_TRUST_MATCHES + 2);
		CHECK(s.trust[0] == 1 && s.trust[1] == 0 && s.trust[2] == 0 && s.trust[3] == 0);
	}

	if (fails) { printf("stkmerge: %d CHECK(s) failed\n", fails); return 1; }
	printf("stkmerge: all passed\n");
	return 0;
}
