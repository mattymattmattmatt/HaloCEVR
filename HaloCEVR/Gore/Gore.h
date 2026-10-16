#pragma once
#include <cstdint>

// Gore for retail Halo PC 1.0.10, ported from the HaloCE-OpenCE gore prototype.
// Fail-safe: if a needed game function isn't found, gore switches itself off
// and the game runs exactly as it does without it.
namespace Gore
{
	// After the hooks are created. Returns true if gore can run.
	bool Init();
	bool IsActive();

	// Around retail object_cause_damage (the hook runs the original between).
	// PreDamage's result goes to PostDamage (the damage can nest).
	uint32_t PreDamage(void* damage, uint32_t objectID);
	void PostDamage(void* damage, uint32_t objectID, uint32_t state);

	// Once per rendered frame (not per eye): spawns queued gibs, ages pieces.
	void Update();
}
