#pragma once
// Pure gore decisions shared by the retail hook and the offline tests.
// No game pointers, signatures, or rendering. Speeds use the same unit the
// piece fling already writes into object velocity: world units per tick
// (see Gore.cpp, "world units a tick"), 30 ticks a second, 3.048 m a world unit.
#include <cmath>
#include <cstdint>

namespace GoreRules
{
	constexpr float MetresPerWorldUnit = 3.048f;
	constexpr float TicksPerSecond = 30.f;
	// OpenCE's stock-map impact splat (1.35 x the deadly fall, about 25.5 m/s).
	// This mod cannot read the map's falling-damage globals.
	constexpr float ImpactSpeedMetres = 25.5f;
	constexpr float ImpactStopFraction = 0.45f;
	constexpr float TeleportSlopWu = 1.5f;
	constexpr int UnshieldedDripMs = 30000;
	constexpr int ShieldGiveUpMs = 90000;
	constexpr int MaxBursts = 6;
	constexpr float SpotRadiusWu = 0.45f;
	constexpr float SpotHeightSlopWu = 0.35f;
	constexpr float TrailStrideWu = 0.25f;
	constexpr int TrailStepsMin = 12;
	constexpr int TrailStepsSpan = 7; // 12 .. 18

	inline float ClampAmount(float amount)
	{
		if (amount < 0.f) return 0.f;
		if (amount > 3.f) return 3.f;
		return amount;
	}

	// How many of the bursts a blood amount of 1 would play. 1 reproduces
	// the previous counts. 0 plays none. Capped so a typo cannot flood effects.
	inline int ScaledBursts(float amount, int atOne)
	{
		amount = ClampAmount(amount);
		if (amount <= 0.f || atOne <= 0) return 0;
		int n = static_cast<int>(std::floor(static_cast<float>(atOne) * amount + 0.5f));
		if (n < 1) n = 1;
		if (n > MaxBursts) n = MaxBursts;
		return n;
	}

	// Ticks between a flying piece's drip effects. Amount 1 keeps the old
	// cadence (every 6 ticks, every 3 for a Hunter).
	inline int FlyingDripIntervalTicks(float amount, bool hunter)
	{
		amount = ClampAmount(amount);
		if (amount <= 0.f) return 1000000;
		const float base = hunter ? 3.f : 6.f;
		int n = static_cast<int>(std::floor(base / amount + 0.5f));
		if (n < 2) n = 2;
		if (n > 30) n = 30;
		return n;
	}

	inline float MetresPerSecond(float wuPerTick)
	{
		return wuPerTick * TicksPerSecond * MetresPerWorldUnit;
	}

	struct Motion
	{
		bool valid = false;
		float x = 0.f, y = 0.f, z = 0.f;
		float vx = 0.f, vy = 0.f, vz = 0.f;
	};

	struct Impact
	{
		bool qualify = false;
		float metresPerSecond = 0.f;
		float dirX = 0.f, dirY = 0.f, dirZ = 1.f;
	};

	// A hard stop inside one tick. A teleport (moved much further than the
	// velocity can explain) does not qualify. The caller still has to reject
	// Hunters, Flood, machines, pieces, and anyone the engine has not killed.
	inline Impact ClassifyImpact(const Motion& prev, float x, float y, float z, float vx, float vy, float vz)
	{
		Impact hit;
		if (!prev.valid) return hit;
		const float prevSpeed = std::sqrt(prev.vx * prev.vx + prev.vy * prev.vy + prev.vz * prev.vz);
		const float nowSpeed = std::sqrt(vx * vx + vy * vy + vz * vz);
		const float dx = x - prev.x, dy = y - prev.y, dz = z - prev.z;
		const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (moved > prevSpeed + TeleportSlopWu) return hit;
		const float mps = MetresPerSecond(prevSpeed);
		if (mps < ImpactSpeedMetres) return hit;
		if (!(nowSpeed < prevSpeed * ImpactStopFraction)) return hit;
		if (prevSpeed < 0.0001f) return hit;
		hit.qualify = true;
		hit.metresPerSecond = mps;
		hit.dirX = prev.vx / prevSpeed;
		hit.dirY = prev.vy / prevSpeed;
		hit.dirZ = prev.vz / prevSpeed;
		return hit;
	}

	// Chief (any local player unit) and Elites drip until the shield climbs
	// again. Marines, grunts, jackals, hunters, and the Flood use the timed
	// rule even if the tag has a small shield. Matches OpenCE drip_shield_rule.
	inline bool ShieldDripRule(float maxShield, bool isPlayer, bool eliteOrCyborg, bool flood)
	{
		if (maxShield <= 0.f) return false;
		if (isPlayer) return true;
		if (flood) return false;
		return eliteOrCyborg;
	}

	enum class DripPhase { None, Timed, ShieldDown };

	struct DripTrack
	{
		bool seen = false;
		float body = 0.f;
		float shield = 0.f;
		DripPhase phase = DripPhase::None;
		int untilMs = 0;
	};

	struct DripView
	{
		bool alive = false;
		bool inVehicle = false;
		bool shieldRule = false;
		float body = 0.f;
		float shield = 0.f;
		int nowMs = 0;
	};

	// One observation. Call once per frame, not once per eye.
	inline DripTrack StepDrip(DripTrack track, const DripView& view)
	{
		// Riding stops the drops in the caller. The wound itself stays, so
		// stepping off a vehicle does not forget a shield that is still down.
		if (!view.alive)
		{
			track.phase = DripPhase::None;
			track.seen = false;
			return track;
		}
		if (!track.seen)
		{
			track.seen = true;
			track.body = view.body;
			track.shield = view.shield;
			track.phase = DripPhase::None;
			return track;
		}
		const bool bodyHurt = view.body < track.body - 0.0005f;
		const bool shieldHurt = view.shield < track.shield - 0.0005f;
		const bool hurt = bodyHurt || (!view.shieldRule && shieldHurt);
		if (view.shieldRule)
		{
			if (hurt && view.shield <= 0.f)
			{
				track.phase = DripPhase::ShieldDown;
				track.untilMs = view.nowMs + ShieldGiveUpMs;
			}
			else if (track.phase == DripPhase::ShieldDown && view.shield > 0.f && view.shield > track.shield)
			{
				track.phase = DripPhase::None;
			}
			else if (track.phase == DripPhase::ShieldDown && view.nowMs >= track.untilMs)
			{
				track.phase = DripPhase::None;
			}
		}
		else
		{
			if (hurt)
			{
				track.phase = DripPhase::Timed;
				track.untilMs = view.nowMs + UnshieldedDripMs;
			}
			else if (track.phase == DripPhase::Timed && view.nowMs >= track.untilMs)
			{
				track.phase = DripPhase::None;
			}
		}
		track.body = view.body;
		track.shield = view.shield;
		return track;
	}

	inline bool Dripping(const DripTrack& track)
	{
		return track.phase != DripPhase::None;
	}

	// Milliseconds until the next drip effect. Amount 1 is about 0.4 s while
	// moving and slower while standing. Never faster than 0.2 s.
	inline int DripGapMs(float amount, bool standing)
	{
		amount = ClampAmount(amount);
		if (amount <= 0.f) return 1000000;
		// 300 ms is 9 ticks at 30 Hz, OpenCE's minimum gap between drops.
		int gap = static_cast<int>(300.f / amount);
		if (standing) gap *= 3;
		if (gap < 200) gap = 200;
		if (gap > 4000) gap = 4000;
		return gap;
	}

	// sinceMs is time since this unit's last drop. movedWu is horizontal
	// distance since that drop. 0.18 wu is about 0.55 m, inside OpenCE's
	// 0.4 to 0.8 m gap. Standing units wait three times as long.
	inline bool DripEmitNow(int sinceMs, float movedWu, bool standing, float amount)
	{
		if (ClampAmount(amount) <= 0.f) return false;
		if (sinceMs < DripGapMs(amount, false)) return false;
		if (!standing && movedWu >= 0.18f) return true;
		return standing && sinceMs >= DripGapMs(amount, true);
	}

	// OpenCE gore_piece_launch: force clamped 1..3 times 0.05..0.12 wu/tick.
	// scale 1 is that speed. 0 stops the toss. Capped at 2.
	inline float LaunchSpeedWu(float force, float zeroToOne, float scale)
	{
		float f = force < 1.f ? 1.f : (force > 3.f ? 3.f : force);
		float u = zeroToOne < 0.f ? 0.f : (zeroToOne > 1.f ? 1.f : zeroToOne);
		float s = scale < 0.f ? 0.f : (scale > 2.f ? 2.f : scale);
		return f * (0.05f + 0.07f * u) * s;
	}

	// OpenCE spin magnitude is 0.1..0.45 rad/tick, not multiplied by force.
	inline float LaunchSpin(float zeroToOne, float scale)
	{
		float u = zeroToOne < 0.f ? 0.f : (zeroToOne > 1.f ? 1.f : zeroToOne);
		float s = scale < 0.f ? 0.f : (scale > 2.f ? 2.f : scale);
		return (0.1f + 0.35f * u) * s;
	}

	// OpenCE footprint strides, in world units. Human boot is 0.75 m.
	inline float StrideWu(bool hunter, bool elite, bool grunt)
	{
		if (hunter) return 1.3f / MetresPerWorldUnit;
		if (elite) return 0.95f / MetresPerWorldUnit;
		if (grunt) return 0.45f / MetresPerWorldUnit;
		return 0.75f / MetresPerWorldUnit;
	}

	struct Disc
	{
		float x, y, z, radius;
	};

	inline bool InsideDisc(float x, float y, float z, const Disc& disc)
	{
		if (std::fabs(z - disc.z) > SpotHeightSlopWu) return false;
		const float dx = x - disc.x, dy = y - disc.y;
		return dx * dx + dy * dy < disc.radius * disc.radius;
	}

	struct TrailWalker
	{
		bool wet = false;
		int stepsLeft = 0;
	};

	struct TrailResult
	{
		TrailWalker walker;
		bool emit = false;
	};

	// Inside a remembered blood spot the step count is refreshed and nothing
	// is emitted (the feet are still in the pool). Outside, one emit per
	// stride while steps remain. Re-entering sets wet again.
	inline TrailResult StepTrail(TrailWalker walker, bool inSpot, bool onGround, bool movedStride, int freshSteps)
	{
		TrailResult result;
		if (inSpot)
		{
			if (freshSteps < 1) freshSteps = 1;
			walker.wet = true;
			walker.stepsLeft = freshSteps;
			result.walker = walker;
			return result;
		}
		if (!walker.wet || walker.stepsLeft <= 0)
		{
			walker.wet = false;
			walker.stepsLeft = 0;
			result.walker = walker;
			return result;
		}
		if (!onGround || !movedStride)
		{
			result.walker = walker;
			return result;
		}
		result.emit = true;
		walker.stepsLeft--;
		if (walker.stepsLeft <= 0) walker.wet = false;
		result.walker = walker;
		return result;
	}

	// The local player's own body stays in the world (hidden) so the retail
	// death camera still has a unit. Everyone else is deleted once split.
	inline bool RetainLocalBody(bool isLocalPlayer, int parts)
	{
		return isLocalPlayer && parts >= 2;
	}

	struct Vec3
	{
		float x = 0.f, y = 0.f, z = 0.f;
	};

	// OpenCE bleed_downhill. Return value is the slope's sine (0 on flat ground).
	// Below about 6.5 degrees a pool does not start a stream.
	inline float Downhill(Vec3 normal, Vec3& out)
	{
		out.x = normal.x * normal.z;
		out.y = normal.y * normal.z;
		out.z = normal.z * normal.z - 1.f;
		const float mag = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
		if (mag > 0.0001f)
		{
			out.x /= mag; out.y /= mag; out.z /= mag;
		}
		return mag;
	}

	inline bool SlopeStreams(float sine)
	{
		return sine > 0.113f;
	}
}
