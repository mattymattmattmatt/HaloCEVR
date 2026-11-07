#include "../HaloCEVR/Gore/GoreRules.h"
#include <cstdio>
#include <cstdlib>

static int g_failed = 0;

static void Expect(bool cond, const char* name)
{
	if (cond) return;
	std::printf("FAIL %s\n", name);
	g_failed++;
}

static void TestBursts()
{
	Expect(GoreRules::ScaledBursts(1.f, 1) == 1, "one burst at amount 1");
	Expect(GoreRules::ScaledBursts(1.f, 3) == 3, "hunter extra bursts unchanged at amount 1");
	Expect(GoreRules::ScaledBursts(0.f, 3) == 0, "amount 0 plays no blood");
	Expect(GoreRules::ScaledBursts(2.f, 1) == 2, "amount 2 doubles a single burst");
	Expect(GoreRules::ScaledBursts(3.f, 3) == 6, "burst cap");
	Expect(GoreRules::ScaledBursts(-2.f, 1) == 0, "negative amount is off");
	Expect(GoreRules::FlyingDripIntervalTicks(1.f, false) == 6, "flight drip cadence");
	Expect(GoreRules::FlyingDripIntervalTicks(1.f, true) == 3, "hunter flight drip cadence");
	Expect(GoreRules::FlyingDripIntervalTicks(0.f, false) > 1000, "no flight drips at amount 0");
}

static void TestImpact()
{
	GoreRules::Motion prev;
	prev.valid = true;
	prev.z = 10.f;
	// 25.5 m/s is 25.5 / 30 / 3.048 wu per tick.
	const float fast = 25.5f / (30.f * 3.048f);
	prev.vz = -fast;
	GoreRules::Impact hit = GoreRules::ClassifyImpact(prev, prev.x, prev.y, prev.z + prev.vz, 0.f, 0.f, -fast * 0.1f);
	Expect(hit.qualify, "deadly stop qualifies");
	Expect(hit.metresPerSecond >= 25.5f && hit.metresPerSecond < 25.7f, "impact speed is about 25.5 m/s");
	Expect(hit.dirZ < -0.9f, "direction follows the fall");

	GoreRules::Impact walk = GoreRules::ClassifyImpact(prev, prev.x, prev.y, prev.z, 0.f, 0.f, -fast * 0.2f);
	// previous sample above is still a fast fall; replace with a walk.
	GoreRules::Motion slow = prev;
	slow.vz = -0.02f;
	walk = GoreRules::ClassifyImpact(slow, slow.x, slow.y, slow.z + slow.vz, 0.f, 0.f, 0.f);
	Expect(!walk.qualify, "a step is not an impact");

	GoreRules::Motion coast = prev;
	GoreRules::Impact stillFast = GoreRules::ClassifyImpact(coast, coast.x, coast.y, coast.z + coast.vz, 0.f, 0.f, coast.vz);
	Expect(!stillFast.qualify, "still falling is not a stop");

	GoreRules::Impact teleport = GoreRules::ClassifyImpact(prev, 40.f, 0.f, 0.f, 0.f, 0.f, 0.f);
	Expect(!teleport.qualify, "a teleport is not an impact");

	GoreRules::Motion none;
	Expect(!GoreRules::ClassifyImpact(none, 0, 0, 0, 0, 0, 0).qualify, "no history, no impact");
}

static void TestDrip()
{
	Expect(GoreRules::ShieldDripRule(1.f, true, false, false), "player uses the shield rule");
	Expect(GoreRules::ShieldDripRule(1.f, false, true, false), "elite uses the shield rule");
	Expect(!GoreRules::ShieldDripRule(1.f, false, true, true), "flood elite does not");
	Expect(!GoreRules::ShieldDripRule(0.5f, false, false, false), "marine uses the timed rule");
	Expect(!GoreRules::ShieldDripRule(0.f, true, false, false), "no shield means the timed rule");

	GoreRules::DripTrack track;
	GoreRules::DripView view;
	view.alive = true;
	view.shieldRule = true;
	view.body = 1.f;
	view.shield = 1.f;
	view.nowMs = 1000;
	track = GoreRules::StepDrip(track, view);
	Expect(track.seen && !GoreRules::Dripping(track), "first sight remembers, does not drip");

	view.body = 0.4f;
	view.shield = 0.f;
	view.nowMs = 1100;
	track = GoreRules::StepDrip(track, view);
	Expect(GoreRules::Dripping(track), "shield break with a body hit starts drips");

	view.shield = 0.2f;
	view.nowMs = 1200;
	track = GoreRules::StepDrip(track, view);
	Expect(!GoreRules::Dripping(track), "shield recharge stops drips");

	GoreRules::DripTrack grunt;
	GoreRules::DripView g;
	g.alive = true;
	g.shieldRule = false;
	g.body = 1.f;
	g.shield = 0.f;
	g.nowMs = 0;
	grunt = GoreRules::StepDrip(grunt, g);
	g.body = 0.5f;
	g.nowMs = 100;
	grunt = GoreRules::StepDrip(grunt, g);
	Expect(GoreRules::Dripping(grunt), "unshielded hit starts the timer");
	g.nowMs = 100 + 30000;
	grunt = GoreRules::StepDrip(grunt, g);
	Expect(!GoreRules::Dripping(grunt), "unshielded drips last 30 s");

	g.body = 0.2f;
	g.nowMs = 50000;
	grunt = GoreRules::StepDrip(grunt, g);
	Expect(GoreRules::Dripping(grunt), "a new hit restarts the 30 s");
	g.alive = false;
	g.nowMs = 50100;
	grunt = GoreRules::StepDrip(grunt, g);
	Expect(!GoreRules::Dripping(grunt), "death stops drips");

	GoreRules::DripTrack elite;
	GoreRules::DripView e;
	e.alive = true;
	e.shieldRule = true;
	e.body = 0.5f;
	e.shield = 0.f;
	e.nowMs = 0;
	elite = GoreRules::StepDrip(elite, e);
	e.nowMs = 10;
	e.body = 0.4f;
	elite = GoreRules::StepDrip(elite, e);
	e.shield = 0.f;
	e.body = 0.4f;
	e.nowMs = 10 + 90000;
	elite = GoreRules::StepDrip(elite, e);
	Expect(!GoreRules::Dripping(elite), "a shield that never returns is given up at 90 s");
	Expect(!GoreRules::DripEmitNow(100, 1.f, false, 1.f), "drops cannot stack every frame");
	Expect(GoreRules::DripEmitNow(400, 0.2f, false, 1.f), "a moving wound drops about every half metre");
	Expect(!GoreRules::DripEmitNow(400, 0.01f, true, 1.f), "standing waits longer");
	Expect(GoreRules::DripEmitNow(1200, 0.01f, true, 1.f), "standing still drops eventually");
	Expect(!GoreRules::DripEmitNow(5000, 1.f, false, 0.f), "amount 0 emits nothing");
}

static void TestTrail()
{
	GoreRules::Disc spot{ 0.f, 0.f, 0.f, GoreRules::SpotRadiusWu };
	Expect(GoreRules::InsideDisc(0.1f, 0.1f, 0.f, spot), "standing in the spot");
	Expect(!GoreRules::InsideDisc(2.f, 0.f, 0.f, spot), "outside the spot");
	Expect(!GoreRules::InsideDisc(0.f, 0.f, 2.f, spot), "a floor above is not the spot");

	GoreRules::TrailWalker walker;
	GoreRules::TrailResult in = GoreRules::StepTrail(walker, true, true, true, 12);
	Expect(in.walker.wet && in.walker.stepsLeft == 12 && !in.emit, "entering refreshes and does not print");
	GoreRules::TrailResult step = GoreRules::StepTrail(in.walker, false, true, true, 12);
	Expect(step.emit && step.walker.stepsLeft == 11, "one print per stride outside");
	GoreRules::TrailResult air = GoreRules::StepTrail(step.walker, false, false, true, 12);
	Expect(!air.emit && air.walker.stepsLeft == 11, "airborne does not print");
	GoreRules::TrailResult again = GoreRules::StepTrail(air.walker, true, true, false, 18);
	Expect(again.walker.stepsLeft == 18 && !again.emit, "re-entering refreshes the count");
	walker = again.walker;
	walker.stepsLeft = 1;
	GoreRules::TrailResult last = GoreRules::StepTrail(walker, false, true, true, 12);
	Expect(last.emit && !last.walker.wet, "the last step ends the trail");
}

static void TestBody()
{
	Expect(GoreRules::RetainLocalBody(true, 6), "a split player body is kept");
	Expect(!GoreRules::RetainLocalBody(false, 6), "an enemy body is not kept");
	Expect(!GoreRules::RetainLocalBody(true, 1), "a body that did not split is left to the game");
}

int main()
{
	TestBursts();
	TestImpact();
	TestDrip();
	TestTrail();
	TestBody();
	if (g_failed)
	{
		std::printf("%d failed\n", g_failed);
		return 1;
	}
	std::printf("gore rules ok\n");
	return 0;
}
