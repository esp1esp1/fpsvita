/*
 * Host test for companion/btnmerge.h: which syscon presses Antilag may deliver
 * ahead of SceCtrl's vblank sample, and which stale presses it hides.
 *
 * The model: the syscon word changes the moment a button moves; SceCtrl's
 * sample follows one vblank (16.7 ms) later; the game reads once per frame.
 * Each scenario steps a read loop and checks what the game would see.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../companion/btnmerge.h"

#define CROSS  0x4000u
#define CIRCLE 0x2000u
#define SQUARE 0x8000u
#define VBLANK 16667u
#define WINDOW (4u * VBLANK + 10000u)

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A simulated pad: physical state, and what SceCtrl reports after a lag,
 * optionally reshaped by a "plugin" callback. */
struct Sim {
	BtnMerge b;
	uint32_t phys;        /* syscon word, pressed = 1, game bits */
	uint32_t sample;      /* SceCtrl's current sample */
	uint32_t now;
	uint32_t changed_at;
	uint32_t last_changed;
	uint32_t (*plugin)(uint32_t phys, uint32_t now);   /* kernel plugin on the sample */
	uint32_t queued;      /* physical word SceCtrl will sample next */
};

static void simInit(Sim *s, uint32_t (*plugin)(uint32_t, uint32_t))
{
	memset(s, 0, sizeof(*s));
	s->plugin = plugin;
	s->now = 1000000;
}

static void press(Sim *s, uint32_t bits)   { s->phys |= bits;  s->changed_at = s->now; }
static void release(Sim *s, uint32_t bits) { s->phys &= ~bits; s->changed_at = s->now; }

/* SceCtrl samples the syscon at its vblank; the plugin reshapes it. */
static void vblank(Sim *s)
{
	uint32_t raw = s->phys;
	s->sample = s->plugin ? s->plugin(raw, s->now) : raw;
}

/* One game read: returns what the game sees. */
static uint32_t gameRead(Sim *s)
{
	uint32_t add, clear;
	int changed = s->changed_at != s->last_changed;
	s->last_changed = s->changed_at;
	btnMerge(&s->b, s->phys, s->sample, changed, s->now, WINDOW, &add, &clear);
	CHECK((add & clear) == 0);
	return (s->sample | add) & ~clear;
}

static void advance(Sim *s, uint32_t us) { s->now += us; }

/* Press at t, SceCtrl sees it at the next vblank; read right before and
 * right after. Returns the read right before the vblank sample. */
static uint32_t pressAndReadBefore(Sim *s, uint32_t bits)
{
	press(s, bits);
	advance(s, 2000);
	uint32_t seen = gameRead(s);
	advance(s, 8000);
	vblank(s);
	advance(s, 2000);
	gameRead(s);
	return seen;
}

static void holdFrames(Sim *s, int frames)
{
	for (int i = 0; i < frames; i++) { advance(s, VBLANK); vblank(s); gameRead(s); }
}

static void releaseAndSettle(Sim *s, uint32_t bits)
{
	release(s, bits);
	advance(s, 2000);
	gameRead(s);
	advance(s, VBLANK);
	vblank(s);
	gameRead(s);
	holdFrames(s, 2);
}

/* --- plugins --- */
static uint32_t pluginNone(uint32_t p, uint32_t) { return p; }
/* Cross remapped to Circle, Cross consumed. */
static uint32_t pluginRemap(uint32_t p, uint32_t)
{ return (p & ~CROSS) | ((p & CROSS) ? CIRCLE : 0); }
/* Turbo on Cross: 50% duty at 60 ms. */
static uint32_t pluginTurbo(uint32_t p, uint32_t now)
{ return (p & CROSS) && ((now / 30000) & 1) ? (p & ~CROSS) : p; }
/* Sticky: Cross stays down for 10 frames after the release. */
static uint32_t stickyUntil;
static uint32_t pluginSticky(uint32_t p, uint32_t now)
{
	if (p & CROSS) stickyUntil = now + 10 * VBLANK;
	return now < stickyUntil ? (p | CROSS) : p;
}
/* An injected press (a script) holds Square whatever the pad says. */
static uint32_t pluginInject(uint32_t p, uint32_t) { return p | SQUARE; }

static void testFirstPressStockThenEarly(void)
{
	printf("first press at stock latency, later ones early\n");
	Sim s; simInit(&s, pluginNone);
	CHECK(pressAndReadBefore(&s, CROSS) == 0);          /* not yet trusted */
	CHECK(s.b.pass & CROSS);                             /* confirmed by the sample */
	releaseAndSettle(&s, CROSS);
	CHECK(pressAndReadBefore(&s, CROSS) == CROSS);      /* one vblank early */
	CHECK(s.b.presses == 2 && s.b.early == 1);          /* the share counts the second */
	CHECK(pressAndReadBefore(&s, CIRCLE) == CROSS);     /* Cross still held; Circle earns it alone */
	releaseAndSettle(&s, CROSS | CIRCLE);
	CHECK(pressAndReadBefore(&s, CIRCLE) == CIRCLE);
}

static void testReleaseEarly(void)
{
	printf("release of an early press hidden until SceCtrl agrees\n");
	Sim s; simInit(&s, pluginNone);
	pressAndReadBefore(&s, CROSS); releaseAndSettle(&s, CROSS);
	pressAndReadBefore(&s, CROSS);
	holdFrames(&s, 3);
	release(&s, CROSS);
	advance(&s, 2000);
	CHECK(gameRead(&s) == 0);                            /* sample still down, hidden */
	advance(&s, VBLANK); vblank(&s);
	CHECK(gameRead(&s) == 0);
	CHECK(s.b.rel == 0);                                 /* SceCtrl caught up: done */
	CHECK(s.b.pass & CROSS);
}

static void testDoubleTap(void)
{
	printf("two taps faster than a vblank stay two presses\n");
	Sim s; simInit(&s, pluginNone);
	pressAndReadBefore(&s, CROSS); releaseAndSettle(&s, CROSS);
	/* down, SceCtrl samples it, release and press again before the next sample */
	press(&s, CROSS); advance(&s, 5000); vblank(&s); advance(&s, 1000);
	CHECK(gameRead(&s) == CROSS);
	advance(&s, 4000); release(&s, CROSS); advance(&s, 1000);
	CHECK(gameRead(&s) == 0);                            /* the gap is visible */
	advance(&s, 2000); press(&s, CROSS); advance(&s, 1000);
	CHECK(gameRead(&s) == CROSS);
	CHECK(s.b.pass & CROSS);
}

static void testRemapNeverPasses(void)
{
	printf("a consumed remap never qualifies\n");
	Sim s; simInit(&s, pluginRemap);
	for (int i = 0; i < 5; i++) {
		CHECK(pressAndReadBefore(&s, CROSS) == 0);      /* Cross never leaks */
		holdFrames(&s, 2);
		CHECK(gameRead(&s) == CIRCLE);
		releaseAndSettle(&s, CROSS);
	}
	CHECK((s.b.pass & CROSS) == 0);
	CHECK(s.b.pend == 0);
}

static void testTurboRevokes(void)
{
	printf("turbo: pass revoked once the game stops seeing a held bit\n");
	Sim s; simInit(&s, pluginTurbo);
	press(&s, CROSS);
	int sawUp = 0;
	for (int i = 0; i < 20; i++) {
		advance(&s, VBLANK); vblank(&s);
		uint32_t g = gameRead(&s);
		if (s.b.pass & CROSS) {
			/* trusted only until the first off phase */
		}
		if (!(g & CROSS)) sawUp = 1;
	}
	CHECK(sawUp);                                        /* turbo still toggles */
	CHECK((s.b.pass & CROSS) == 0);
	/* and stays untrusted on the next press */
	release(&s, CROSS); holdFrames(&s, 3);
	CHECK(pressAndReadBefore(&s, CROSS) == 0);
}

static void testStickyRevokes(void)
{
	printf("sticky: a hold SceCtrl keeps past the window is not ours\n");
	Sim s; simInit(&s, pluginSticky);
	pressAndReadBefore(&s, CROSS); releaseAndSettle(&s, CROSS);
	/* sticky is still holding from the first press; let it run out */
	holdFrames(&s, 12);
	CHECK(gameRead(&s) == 0);
	pressAndReadBefore(&s, CROSS);
	holdFrames(&s, 2);
	release(&s, CROSS);
	advance(&s, 2000);
	int hidden = 0, shown = 0;
	for (int i = 0; i < 8; i++) {
		advance(&s, VBLANK); vblank(&s);
		uint32_t g = gameRead(&s);
		if (g & CROSS) shown = 1; else hidden = 1;
	}
	CHECK(hidden);                                       /* masked for a window */
	CHECK(shown);                                        /* then given back */
	CHECK((s.b.pass & CROSS) == 0);                      /* and no longer trusted */
}

static void testInjectedUntouched(void)
{
	printf("an injected press the syscon never shows is left alone\n");
	Sim s; simInit(&s, pluginInject);
	holdFrames(&s, 5);
	CHECK(gameRead(&s) == SQUARE);
	pressAndReadBefore(&s, CROSS); releaseAndSettle(&s, CROSS);
	CHECK(pressAndReadBefore(&s, CROSS) == (CROSS | SQUARE));
	releaseAndSettle(&s, CROSS);
	CHECK(gameRead(&s) == SQUARE);
	CHECK((s.b.pass & SQUARE) == 0);
}

static void testConstantWord(void)
{
	printf("a constant word (no pad) never qualifies anything\n");
	Sim s; simInit(&s, pluginNone);
	/* the "syscon" says everything is down, forever; the real pad (a DS4
	 * through SceCtrl) presses Cross */
	s.phys = 0xFFFFu;
	s.b.prev = 0;                                        /* first read sees it all as edges */
	advance(&s, 1000);
	uint32_t add, clear;
	btnMerge(&s.b, s.phys, 0, 1, s.now, WINDOW, &add, &clear);
	CHECK(add == 0);
	for (int i = 0; i < 3; i++) {
		advance(&s, VBLANK); s.sample = CROSS;
		btnMerge(&s.b, s.phys, s.sample, 0, s.now, WINDOW, &add, &clear);
	}
	/* the first edge did pend everything; Cross got confirmed by the DS4 */
	CHECK(add == CROSS);                                 /* no new bits beyond the sample */
	s.sample = 0;
	advance(&s, VBLANK);
	btnMerge(&s.b, s.phys, s.sample, 0, s.now, WINDOW, &add, &clear);
	CHECK(add == 0);                                     /* revoked: the game dropped it while "held" */
	CHECK((s.b.pass & CROSS) == 0);
}

int main(void)
{
	testFirstPressStockThenEarly();
	testReleaseEarly();
	testDoubleTap();
	testRemapNeverPasses();
	testTurboRevokes();
	testStickyRevokes();
	testInjectedUntouched();
	testConstantWord();
	if (fails) { printf("btnmerge: %d FAILED\n", fails); return 1; }
	printf("btnmerge: all passed\n");
	return 0;
}
