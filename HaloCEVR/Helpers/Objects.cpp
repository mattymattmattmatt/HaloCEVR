#include "Objects.h"
#include "Assets.h"
#include "../Hooking/Hooks.h"
#include "../Logger.h"
#include <cstring>
#include <windows.h>

namespace
{
	// Matches Chimera's s_object_creation_disposition (query buffer).
	struct ObjectCreateQuery
	{
		HaloID tagID;
		uint32_t pad0;
		uint32_t playerID;
		HaloID parent;
		uint32_t pad1;
		uint32_t pad2;
		Vector3 position;
		char rest[0x88 - 0x24];
	};
	static_assert(sizeof(ObjectCreateQuery) >= 0x24, "query header");
}

ObjectTable& Helpers::GetObjectTable()
{
	return **reinterpret_cast<ObjectTable**>(Hooks::o.ObjectTable);
}

PlayerTable& Helpers::GetPlayerTable()
{
	return **reinterpret_cast<PlayerTable**>(Hooks::o.PlayerTable);
}

BaseDynamicObject* Helpers::GetDynamicObject(HaloID& id)
{
	ObjectTable& table = GetObjectTable();

	if (!table.elements || id.index >= table.currentSize)
	{
		return nullptr;
	}

	return table.elements[id.index].dynamicObject;
}

bool Helpers::GetLocalPlayerID(HaloID& outID)
{
	PlayerTable& table = GetPlayerTable();

	if (table.currentSize == 0)
	{
		return false;
	}

	outID = table.elements->objectID;
	return true;
}

BaseDynamicObject* Helpers::GetLocalPlayer()
{
	// TODO: Test this in MP, it may not be that the first player is the local one

	HaloID playerID;
	if (!GetLocalPlayerID(playerID))
	{
		return nullptr;
	}

	return GetDynamicObject(playerID);
}

bool Helpers::FindGrenadeProjectileTag(int grenadeType, HaloID& outTag)
{
	outTag.index = 0xFFFF;
	outTag.id = 0xFFFF;

	if (grenadeType < 0 || grenadeType > 1)
	{
		return false;
	}

	Asset_Generic* tags = GetAssetArray();
	if (!tags)
	{
		return false;
	}

	int count = 4096;
	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(tags, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
	{
		const char* base = static_cast<const char*>(mbi.BaseAddress);
		const size_t used = static_cast<char*>(static_cast<void*>(tags)) - base;
		if (mbi.RegionSize > used)
		{
			count = static_cast<int>((mbi.RegionSize - used) / sizeof(Asset_Generic));
			if (count > 20000)
			{
				count = 20000;
			}
		}
	}

	const char* want = (grenadeType == 1) ? "plasma grenade" : "frag grenade";
	HaloID fallback{};
	fallback.index = 0xFFFF;
	fallback.id = 0xFFFF;
	bool bHaveFallback = false;

	for (int i = 0; i < count; ++i)
	{
		const bool isProj = std::memcmp(tags[i].GroupID, "proj", 4) == 0
			|| std::memcmp(tags[i].GroupID, "jorp", 4) == 0;
		if (!isProj)
		{
			continue;
		}

		const char* path = *reinterpret_cast<char**>(reinterpret_cast<char*>(&tags[i]) + 0x10);
		if (!path)
		{
			continue;
		}

		char lower[256];
		size_t n = 0;
		for (; n < sizeof(lower) - 1 && path[n]; ++n)
		{
			const char c = path[n];
			lower[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
		}
		lower[n] = 0;

		HaloID id = *reinterpret_cast<HaloID*>(reinterpret_cast<char*>(&tags[i]) + 0x0C);
		if (id.index == 0xFFFF)
		{
			continue;
		}

		if (std::strstr(lower, want))
		{
			outTag = id;
			Logger::log << "[GrenadePunch] Found " << want << " proj tag " << outTag
				<< " path=" << path << std::endl;
			return true;
		}

		if (!bHaveFallback && std::strstr(lower, "grenade"))
		{
			fallback = id;
			bHaveFallback = true;
		}
	}

	if (bHaveFallback && grenadeType == 0)
	{
		outTag = fallback;
		Logger::log << "[GrenadePunch] Using fallback grenade proj tag " << outTag << std::endl;
		return true;
	}

	Logger::log << "[GrenadePunch] No projectile tag for type " << grenadeType << std::endl;
	return false;
}

HaloID Helpers::SpawnObject(HaloID tagID, const Vector3& position, HaloID parent)
{
	HaloID none;
	none.index = 0xFFFF;
	none.id = 0xFFFF;

	if (Hooks::o.CreateObjectQuery.Address == 0 || Hooks::o.CreateObject.Address == 0)
	{
		return none;
	}

	alignas(16) char buffer[1024]{};
	ObjectCreateQuery* query = reinterpret_cast<ObjectCreateQuery*>(buffer);

	void* queryFn = reinterpret_cast<void*>(Hooks::o.CreateObjectQuery.Address - 6);
	void* createFn = reinterpret_cast<void*>(Hooks::o.CreateObject.Address - 24);
	uint32_t tagRaw = *reinterpret_cast<uint32_t*>(&tagID);
	uint32_t parentRaw = *reinterpret_cast<uint32_t*>(&parent);
	uint32_t created = 0xFFFFFFFF;

	__asm
	{
		push parentRaw
		push tagRaw
		mov eax, query
		call queryFn
		add esp, 8
	}

	query->playerID = 0xFFFFFFFF;
	query->position = position;

	const uint32_t objectType = 0; // created by local machine
	__asm
	{
		push objectType
		push query
		call createFn
		add esp, 8
		mov created, eax
	}

	HaloID result;
	*reinterpret_cast<uint32_t*>(&result) = created;
	return result;
}

void Helpers::ArmProjectileDetonation(BaseDynamicObject* projectile, bool startFuse)
{
	if (!projectile)
	{
		return;
	}

	// This writes projectile-only fields as far in as +0x24C. A slot freed by
	// the detonation can be recycled into a smaller object while its datum
	// still looks live for a frame, and blindly writing that far into one
	// smashes the heap. Only ever touch an actual projectile.
	if (projectile->N0000027E != ObjectType::PROJECTILE)
	{
		return;
	}

	// Object AT_REST (bit 5 of +0x10) skips projectile simulation, which is
	// why a spawned punch nade sat in mid-air with FX and never cooked.
	// Keep it simulating; zero velocity so it does not fly off the fist.
	uint16_t objFlags = static_cast<uint16_t>(projectile->N0000025F);
	objFlags &= ~static_cast<uint16_t>(ObjectProperties::Stationary);
	projectile->N0000025F = static_cast<ObjectProperties>(objFlags);
	projectile->velocity = Vector3(0.0f, 0.0f, 0.0f);

	uint8_t* raw = reinterpret_cast<uint8_t*>(projectile);

	// aLTis-tested projectile flags at +0x22C:
	//   bit 3 = frozen in time (grenade_throw_fix SETS this to stop detonation)
	//   bit 4/5 = at rest (starts the grenade fuse)
	uint32_t projFlags = *reinterpret_cast<uint32_t*>(raw + 0x22C);
	projFlags &= ~(1u << 3);
	projFlags |= (1u << 4);
	projFlags |= (1u << 5);
	*reinterpret_cast<uint32_t*>(raw + 0x22C) = projFlags;

	// 0 = explode, 1 = disappear, anything higher freezes the projectile.
	*reinterpret_cast<uint16_t*>(raw + 0x230) = 0;

	// +0x244 is the detonation countdown (NOT +0x248). +0x248 is arming
	// elapsed time — grenades will not pop until this exceeds arming time.
	*reinterpret_cast<float*>(raw + 0x240) = 1.0f;
	if (startFuse)
	{
		// One game tick (Halo simulates at 30Hz). Must be > 0 so the engine
		// actually counts it down; writing 0 every frame can skip the expiry.
		*reinterpret_cast<float*>(raw + 0x244) = 1.0f / 30.0f;
	}
	*reinterpret_cast<float*>(raw + 0x248) = 10.0f;
	*reinterpret_cast<float*>(raw + 0x24C) = 0.0f;
}
