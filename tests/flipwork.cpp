/* Host harness for the REAL noteFlipWork() from companion/main.c: the CPU
 * estimate a title gets when its driver presents by flipping directly (the
 * PowerVR GLES stack) instead of through AddEntry. */
#include <stdio.h>
#include <stdint.h>

/* The helper's globals the function reads, at the same names and types. */
#define HOOK_GXM_ADD_ENTRY 10
#define HOOK_COUNT 40
static int s_hookId[HOOK_COUNT];
static volatile uint32_t s_vbl_ret;         /* last pacing wait's return */
static volatile uint32_t s_present_last;    /* previous flip */
static volatile uint32_t s_flip_work_ema;

static inline void emaStep(volatile uint32_t *e, uint32_t x)
{
	uint32_t v = *e;
	*e = v ? (uint32_t)((int32_t)v + ((int32_t)(x - v) >> 4)) : x;
}

#define HOOK_PVR_SWAP 27
static int submitHooked(void)   /* the helper's: AddEntry or the PowerVR swap landed */
{
	return s_hookId[HOOK_GXM_ADD_ENTRY] >= 0 || s_hookId[HOOK_PVR_SWAP] >= 0;
}

#include "flipwork.inc"   /* the real noteFlipWork() */

static int fails = 0;
static void expect(const char *w, bool ok)
{
	printf("    %-58s %s\n", w, ok ? "OK" : "** FAIL **");
	if (!ok) fails++;
}

/* One frame: the wait returned at `vb`, the flip happens at `t`. */
static void frame(uint32_t vb, uint32_t t, int addEntryHooked)
{
	s_hookId[HOOK_GXM_ADD_ENTRY] = addEntryHooked ? 0 : -1;
	s_hookId[HOOK_PVR_SWAP] = -1;
	s_vbl_ret = vb;
	noteFlipWork(t);
}

int main(void)
{
	printf("== A. a direct flip: the wait's return to the flip is the work ==\n");
	{
		s_flip_work_ema = 0; s_present_last = 1000;
		frame(5000, 12000, 0);            /* 7 ms of frame work */
		expect("first sample is the measured work", s_flip_work_ema == 7000);
	}
	{
		s_flip_work_ema = 7000; s_present_last = 12000;
		frame(20000, 26000, 0);           /* 6 ms */
		uint32_t want = 7000 + ((int32_t)(6000 - 7000) >> 4);
		expect("later samples move the EMA toward them", s_flip_work_ema == want);
	}

	printf("== B. samples that are not this frame's work ==\n");
	{
		s_flip_work_ema = 7000; s_present_last = 20000;
		frame(19000, 29000, 0);           /* wait returned BEFORE the last flip;
		                                   * without the gate this would read 10 ms */
		expect("a wait older than the previous flip is stale: no update", s_flip_work_ema == 7000);
	}
	{
		s_flip_work_ema = 7000; s_present_last = 20000;
		frame(21000, 160000, 0);          /* 139 ms: a load or a pause */
		expect("a 100 ms gap is not a frame", s_flip_work_ema == 7000);
	}
	{
		s_flip_work_ema = 7000; s_present_last = 20000;
		frame(21000, 30000, 1);           /* AddEntry hooked: it measures instead */
		expect("an AddEntry title keeps its own measure", s_flip_work_ema == 7000);
	}
	{
		s_flip_work_ema = 7000; s_present_last = 20000;
		s_hookId[HOOK_GXM_ADD_ENTRY] = -1; s_vbl_ret = 0;
		noteFlipWork(30000);
		expect("no wait has returned yet: no update", s_flip_work_ema == 7000);
	}

	printf("== C. one wild sample must not stall the ramp ==\n");
	{
		s_flip_work_ema = 5000; s_present_last = 20000;
		frame(21000, 61000, 0);           /* 40 ms, clamped to 3x + 2 ms */
		uint32_t want = 5000 + ((int32_t)(17000 - 5000) >> 4);
		expect("clamped to 3x the EMA plus 2 ms", s_flip_work_ema == want);
	}

	printf("== D. boundaries ==\n");
	{
		s_flip_work_ema = 4000; s_present_last = 20000;
		frame(20000, 20000, 0);           /* wait returned at the flip: 0 us */
		expect("a zero-length frame is accepted", s_flip_work_ema == 4000 - (4000 >> 4));
	}
	{
		s_flip_work_ema = 4000; s_present_last = 20000;
		frame(21000, 121000, 0);          /* exactly 100 ms: not a frame */
		expect("100 ms is the limit", s_flip_work_ema == 4000);
	}
	{
		/* The stamps are 32-bit microseconds and wrap after about 71 min. */
		s_flip_work_ema = 3000; s_present_last = 0xFFFFFF00u;
		frame(0x00000100u, 0x00000400u, 0);
		uint32_t want = 3000 + ((int32_t)(768 - 3000) >> 4);
		expect("a wrapped clock still measures the frame", s_flip_work_ema == want);
	}

	printf("\n%s\n", fails ? "FLIPWORK: FAILURES" : "FLIPWORK: all pass");
	return fails ? 1 : 0;
}
