#include "Gore.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cctype>
#include <string>
#include "../Game.h"
#include "../Logger.h"
#include "../Hooking/Hooks.h"
#include "../Helpers/Objects.h"
#include "../Helpers/Assets.h"

// Retail layouts (Halo PC 1.0.10). The object fields are the decompile's
// object_data shifted by the 0x50 bytes retail keeps before the position
// (cross-checked against Objects.h: position 0x5C, health 0xE0, parent 0x11C).
namespace
{
	constexpr uint32_t NONE = 0xFFFFFFFF;

	// object
	constexpr int O_FLAGS = 0x10;            // bit 0 invisible, 23 no recompute nodes, 24 no collisions, 17 cannot be garbage
	constexpr int O_POSITION = 0x5C;
	constexpr int O_VELOCITY = 0x68;
	constexpr int O_FORWARD = 0x74;
	constexpr int O_UP = 0x80;
	constexpr int O_ANGULAR = 0x8C;
	constexpr int O_CENTRE = 0xA0;
	constexpr int O_RADIUS = 0xAC;
	constexpr int O_TYPE = 0xB4;
	constexpr int O_TEAM = 0xB8;
	constexpr int O_VARIANT = 0xBE;
	constexpr int O_BODY = 0xE0;
	constexpr int O_DAMAGE_FLAGS = 0x106;    // bit 2 dead, bit 11 cannot take damage
	constexpr int O_PARENT = 0x11C;
	constexpr int O_REGIONS_DESTROYED = 0x174;
	constexpr int O_FORCED_SHADER = 0x176;
	constexpr int O_REGION_DAMAGE = 0x178;
	constexpr int O_REGION_PERMS = 0x180;   // byte[8], 0xFF draws nothing (models.c matches by index)
	constexpr int O_CHANGE_COLORS = 0x188;  // base + outgoing, 2 x 4 rgb
	constexpr int U_PLAYER = 0x218;

	// damage_data (retail hs_damage_object builds a 0x54 byte one)
	constexpr int D_DEFINITION = 0x00;
	constexpr int D_FLAGS = 0x04;           // bit 0 area of effect
	constexpr int D_OWNER_PLAYER = 0x08;
	constexpr int D_OWNER_OBJECT = 0x0C;
	constexpr int D_OWNER_TEAM = 0x10;
	constexpr int D_EPICENTER = 0x28;
	constexpr int D_DIRECTION = 0x34;
	constexpr int D_SCALE = 0x40;
	constexpr int D_SIZE = 0x54;

	// tags
	constexpr int T_PATH = 0x10;
	constexpr int T_DATA = 0x14;
	constexpr int OBJE_MODEL = 0x34;        // tag_reference index
	constexpr int OBJE_COLLISION = 0x7C;
	constexpr int JPT_UPPER_BOUND = 0x1DC;
	constexpr int COLL_LOCALIZED_EFFECT = 0x7C;
	constexpr int COLL_AREA_EFFECT = 0x90;
	constexpr int COLL_DESTROYED_EFFECT = 0xC8;
	constexpr int MOD2_REGIONS = 0xC4;      // tag_block: count, address
	constexpr int MOD2_REGION_SIZE = 0x4C;

	constexpr float METRES_PER_WU = 3.048f;
	constexpr float FRAG_DAMAGE = 120.0f;

	struct UnitType { const char* path; float gibRadius; float vehicleThreshold; bool disabled; };
	const UnitType unitTypes[] =
	{
		{ "hunter", 0.f, 0.f, true },
		{ "grunt", 2.8f, 3.f, false },
		{ "jackal", 2.5f, 3.5f, false },
		{ "elite", 2.f, 5.f, false },
		{ "flood", 2.8f, 3.f, false },
		{ "cyborg", 2.5f, 4.f, false },
		{ "marine", 2.5f, 4.f, false },
		{ "crewman", 2.5f, 4.f, false },
		{ "captain", 2.5f, 4.f, false },
		{ "pilot", 2.5f, 4.f, false },
		{ "", 2.2f, 4.5f, false },
	};

	// The region names each piece keeps (the stock models'), the rest go with the torso.
	const char* const partRegions[][2] =
	{
		{ "head", nullptr }, { "arms", nullptr }, { "left arm", "left_arm" },
		{ "right arm", "right_arm" }, { "legs", nullptr },
	};
	constexpr int NUM_PARTS = 6; // + torso

	struct Pending { uint32_t unit = NONE; float force = 0.f; float dir[3]{}; uint8_t damage[D_SIZE]{}; ULONGLONG time = 0; };
	struct Piece { uint32_t object = NONE; ULONGLONG born = 0; };

	constexpr int MAX_PENDING = 16;
	constexpr int MAX_PIECES = 256;
	Pending pending[MAX_PENDING];
	Piece pieces[MAX_PIECES];

	bool bActive = false;
	bool bInternal = false;    // our own kill damage on a piece: not a gib
	uint32_t seed = 0x6f726531;

	void* fnCauseDamage = nullptr;
	void* fnDelete = nullptr;
	void* fnEffectOnMarker = nullptr;

	float Rand(float a, float b) { seed = seed * 1664525u + 1013904223u; return a + (b - a) * ((seed >> 8) / 16777216.0f); }

	template<typename T> T& F(void* base, int off) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off); }

	uint8_t* GetObject(uint32_t id)
	{
		if (id == NONE) return nullptr;
		ObjectTable& table = Helpers::GetObjectTable();
		uint16_t index = id & 0xFFFF;
		if (!table.elements || index >= table.currentSize) return nullptr;
		ObjectDatum& d = table.elements[index];
		if (d.id != (id >> 16) || !d.dynamicObject) return nullptr;
		return reinterpret_cast<uint8_t*>(d.dynamicObject);
	}

	uint8_t* GetTag(uint32_t id)
	{
		if (id == NONE) return nullptr;
		Asset_Generic* array = Helpers::GetAssetArray();
		if (!array) return nullptr;
		uint8_t* tag = reinterpret_cast<uint8_t*>(&array[id & 0xFFFF]);
		if (F<uint32_t>(tag, 0xC) != id) return nullptr;
		return tag;
	}
	uint8_t* TagData(uint32_t id) { uint8_t* t = GetTag(id); return t ? F<uint8_t*>(t, T_DATA) : nullptr; }
	const char* TagPath(uint32_t id) { uint8_t* t = GetTag(id); return t ? F<const char*>(t, T_PATH) : ""; }

	const UnitType* TypeOf(uint32_t definition)
	{
		std::string path = TagPath(definition) ? TagPath(definition) : "";
		for (char& c : path) c = (char)std::tolower((unsigned char)c);
		for (const UnitType& t : unitTypes)
			if (!*t.path || path.find(t.path) != std::string::npos) return &t;
		return &unitTypes[_countof(unitTypes) - 1];
	}

	void DeleteObject(uint32_t id)
	{
		if (!GetObject(id)) return;
		void* fn = fnDelete;
		__asm { mov eax, id
				call fn }
	}

	void EffectOnObject(uint32_t effect, uint32_t object, const char* marker)
	{
		if (effect == NONE || !GetTag(effect) || !GetObject(object) || !fnEffectOnMarker) return;
		void* fn = fnEffectOnMarker;
		__asm { push marker
				mov edi, effect
				mov esi, object
				call fn
				add esp, 4 }
	}

	void CauseDamage(void* damage, uint32_t object)
	{
		typedef void(__cdecl* Fn)(void*, uint32_t, int32_t, int32_t, int32_t, void*);
		reinterpret_cast<Fn>(fnCauseDamage)(damage, object, -1, -1, -1, nullptr);
	}

	bool IsSinglePlayer()
	{
		// Every player but the local one is a split-screen/network player.
		PlayerTable& table = Helpers::GetPlayerTable();
		return table.count <= 1;
	}

	float GibForce(uint8_t* unit, uint8_t* damage, const UnitType* type)
	{
		float scale = Game::instance.c_GoreThresholdScale->Value();
		if (scale <= 0.f) scale = 1.f;
		if (!(F<uint32_t>(damage, D_FLAGS) & 1)) return 0.f; // only blasts and vehicle hits

		// A vehicle hit (physics.c): the vehicle's centre is the epicentre and
		// the owner is the vehicle or its driver.
		uint32_t owner = F<uint32_t>(damage, D_OWNER_OBJECT);
		uint8_t* ownerObj = GetObject(owner);
		uint8_t* vehicle = nullptr;
		if (ownerObj)
		{
			if (F<uint16_t>(ownerObj, O_TYPE) == 1) vehicle = ownerObj;
			else if (uint8_t* parent = GetObject(F<uint32_t>(ownerObj, O_PARENT)))
				if (F<uint16_t>(parent, O_TYPE) == 1) vehicle = parent;
		}
		float* epi = &F<float>(damage, D_EPICENTER);
		if (vehicle)
		{
			float* c = &F<float>(vehicle, O_CENTRE);
			float dx = c[0] - epi[0], dy = c[1] - epi[1], dz = c[2] - epi[2];
			if (dx * dx + dy * dy + dz * dz < 0.0001f)
			{
				float* v = &F<float>(vehicle, O_VELOCITY);
				float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 30.f;
				float threshold = type->vehicleThreshold * scale;
				return threshold > 0.f ? speed / threshold : 0.f;
			}
		}

		uint8_t* jpt = TagData(F<uint32_t>(damage, D_DEFINITION));
		if (!jpt) return 0.f;
		float full = F<float>(jpt, JPT_UPPER_BOUND);
		if (full <= 0.f) return 0.f;
		float* c = &F<float>(unit, O_CENTRE);
		float r = F<float>(unit, O_RADIUS);
		float z = epi[2];
		float bottom = c[2] - r * 0.9f, top = c[2] + r * 0.6f;
		z = z < bottom ? bottom : (z > top ? top : z);
		float dx = epi[0] - c[0], dy = epi[1] - c[1], dz = z - c[2];
		float measure = sqrtf(dx * dx + dy * dy + dz * dz) * METRES_PER_WU;
		float threshold = type->gibRadius * sqrtf(full / FRAG_DAMAGE) / scale;
		return threshold / (measure > 0.15f ? measure : 0.15f);
	}

	Piece* NewPiece()
	{
		int maximum = Game::instance.c_GoreMaxPieces->Value();
		maximum = maximum < 6 ? 6 : (maximum > MAX_PIECES ? MAX_PIECES : maximum);
		int oldest = -1;
		for (int i = 0; i < maximum; i++)
		{
			if (pieces[i].object == NONE || !GetObject(pieces[i].object)) { pieces[i].object = NONE; return &pieces[i]; }
			if (oldest < 0 || pieces[i].born < pieces[oldest].born) oldest = i;
		}
		DeleteObject(pieces[oldest].object);
		pieces[oldest].object = NONE;
		return &pieces[oldest];
	}

	uint32_t RegionMask(uint8_t* model, int part, uint32_t used)
	{
		int count = F<int32_t>(model, MOD2_REGIONS);
		uint8_t* regions = F<uint8_t*>(model, MOD2_REGIONS + 4);
		if (!regions || count <= 0) return 0;
		if (count > 8) count = 8;
		uint32_t mask = 0;
		for (int r = 0; r < count; r++)
		{
			const char* name = reinterpret_cast<const char*>(regions + r * MOD2_REGION_SIZE);
			if (part == NUM_PARTS - 1) { if (!(used & (1u << r))) mask |= 1u << r; continue; }
			for (const char* want : partRegions[part])
				if (want && _strnicmp(name, want, 32) == 0) mask |= 1u << r;
		}
		return mask & ~used;
	}

	void SpawnCopy(uint8_t* unit, uint32_t unitID, uint32_t regionMask, const Pending& p, uint32_t blood)
	{
		HaloID tag; *reinterpret_cast<uint32_t*>(&tag) = F<uint32_t>(unit, 0);
		HaloID none; none.index = 0xFFFF; none.id = 0xFFFF;
		Vector3 pos = F<Vector3>(unit, O_POSITION);
		HaloID created = Helpers::SpawnObject(tag, pos, none);
		uint32_t id = *reinterpret_cast<uint32_t*>(&created);
		uint8_t* obj = GetObject(id);
		if (!obj) return;
		unit = GetObject(unitID); // (the table may have moved nothing, but re-read anyway)
		if (!unit) { DeleteObject(id); return; }

		Piece* piece = NewPiece();
		piece->object = id;
		piece->born = GetTickCount64();

		F<Vector3>(obj, O_FORWARD) = F<Vector3>(unit, O_FORWARD);
		F<Vector3>(obj, O_UP) = F<Vector3>(unit, O_UP);
		for (int r = 0; r < 8; r++)
			F<uint8_t>(obj, O_REGION_PERMS + r) = (regionMask & (1u << r)) ? F<uint8_t>(unit, O_REGION_PERMS + r) : 0xFF;
		// The unit's own look (a red grunt's pack stays red).
		memcpy(obj + O_CHANGE_COLORS, unit + O_CHANGE_COLORS, 0x60);
		memcpy(obj + O_REGION_DAMAGE, unit + O_REGION_DAMAGE, 8);
		F<uint16_t>(obj, O_REGIONS_DESTROYED) = F<uint16_t>(unit, O_REGIONS_DESTROYED);
		F<int16_t>(obj, O_FORCED_SHADER) = F<int16_t>(unit, O_FORCED_SHADER);
		F<int16_t>(obj, O_VARIANT) = F<int16_t>(unit, O_VARIANT);
		F<int16_t>(obj, O_TEAM) = F<int16_t>(unit, O_TEAM);

		// Kill it with the same blast (no owner: nobody gets the credit), so
		// it plays a real death and the engine throws it, then add our own fling.
		uint8_t damage[D_SIZE];
		memcpy(damage, p.damage, D_SIZE);
		F<uint32_t>(damage, D_OWNER_PLAYER) = NONE;
		F<uint32_t>(damage, D_OWNER_OBJECT) = NONE;
		F<float>(damage, D_SCALE) = 50.f;
		F<float>(obj, O_BODY) = 0.0001f;
		bInternal = true;
		CauseDamage(damage, id);
		bInternal = false;
		obj = GetObject(id);
		if (!obj) { piece->object = NONE; return; }
		F<uint16_t>(obj, O_DAMAGE_FLAGS) |= (1 << 11); // cannot take damage: no second gib
		F<uint32_t>(obj, O_FLAGS) |= (1u << 17);       // never collected as garbage early

		float force = p.force < 1.f ? 1.f : (p.force > 3.f ? 3.f : p.force);
		float speed = force * Rand(0.05f, 0.12f);
		float rnd[3] = { Rand(-1, 1), Rand(-1, 1), Rand(0, 1) };
		float* v = &F<float>(obj, O_VELOCITY);
		v[0] += p.dir[0] * speed + rnd[0] * speed * 0.5f;
		v[1] += p.dir[1] * speed + rnd[1] * speed * 0.5f;
		v[2] += p.dir[2] * speed + rnd[2] * speed * 0.6f + 0.05f;
		float* w = &F<float>(obj, O_ANGULAR);
		w[0] += Rand(-0.3f, 0.3f); w[1] += Rand(-0.3f, 0.3f); w[2] += Rand(-0.3f, 0.3f);

		EffectOnObject(blood, id, "body");
	}

	void Gib(const Pending& p)
	{
		uint8_t* unit = GetObject(p.unit);
		if (!unit || F<uint16_t>(unit, O_TYPE) != 0) return;
		uint32_t def = F<uint32_t>(unit, 0);
		uint8_t* obje = TagData(def);
		if (!obje) return;
		uint8_t* model = TagData(F<uint32_t>(obje, OBJE_MODEL));
		uint8_t* coll = TagData(F<uint32_t>(obje, OBJE_COLLISION));
		uint32_t blood = NONE, trail = NONE;
		if (coll)
		{
			blood = F<uint32_t>(coll, COLL_AREA_EFFECT) != NONE ? F<uint32_t>(coll, COLL_AREA_EFFECT) : F<uint32_t>(coll, COLL_DESTROYED_EFFECT);
			trail = F<uint32_t>(coll, COLL_LOCALIZED_EFFECT);
		}

		// The burst where the unit stood, before it goes.
		EffectOnObject(blood, p.unit, "body");

		int parts = 0;
		uint32_t used = 0;
		if (model)
		{
			uint32_t masks[NUM_PARTS]{};
			for (int part = 0; part < NUM_PARTS; part++)
			{
				if (part == NUM_PARTS - 1 && parts == 0) break; // one-region model: not split
				masks[part] = RegionMask(model, part, used);
				if (!masks[part]) continue;
				used |= masks[part];
				parts++;
			}
			if (parts >= 2)
			{
				for (int part = 0; part < NUM_PARTS; part++)
					if (masks[part]) SpawnCopy(GetObject(p.unit), p.unit, masks[part], p, trail != NONE ? trail : blood);
			}
		}
		Logger::log << "[Gore] " << TagPath(def) << " came apart into " << parts << " pieces, force " << p.force << std::endl;

		// The corpse goes when it split; a one-region model just bleeds and stays.
		if (parts >= 2) DeleteObject(p.unit);
	}
}

bool Gore::Init()
{
	bActive = false;
	for (Pending& p : pending) p.unit = NONE;
	for (Piece& p : pieces) p.object = NONE;
	if (!Game::instance.c_GoreEnabled || !Game::instance.c_GoreEnabled->Value())
	{
		Logger::log << "[Gore] disabled in config" << std::endl;
		return false;
	}
	// (the hooked entry: our own kills go through the hook, flagged internal)
	fnCauseDamage = Hooks::o.ObjectCauseDamage.Address ? reinterpret_cast<void*>(Hooks::o.ObjectCauseDamage.Address) : nullptr;
	if (SigScanner::UpdateOffset(Hooks::o.ObjectDelete) >= 0) fnDelete = reinterpret_cast<void*>(Hooks::o.ObjectDelete.Address);
	if (SigScanner::UpdateOffset(Hooks::o.EffectNewOnObjectMarker) >= 0) fnEffectOnMarker = reinterpret_cast<void*>(Hooks::o.EffectNewOnObjectMarker.Address);
	if (!fnCauseDamage || !fnDelete || Hooks::o.CreateObject.Address == 0 || Hooks::o.CreateObjectQuery.Address == 0)
	{
		Logger::log << "[Gore] a game function wasn't found (damage " << (fnCauseDamage != nullptr) << ", delete " << (fnDelete != nullptr)
			<< ", create " << (Hooks::o.CreateObject.Address != 0) << "): gore is off" << std::endl;
		return false;
	}
	if (!fnEffectOnMarker) Logger::log << "[Gore] effect function not found: gibs without blood bursts" << std::endl;
	bActive = true;
	Logger::log << "[Gore] active" << std::endl;
	return true;
}

bool Gore::IsActive() { return bActive; }

uint32_t Gore::PreDamage(void* damage, uint32_t objectID)
{
	uint8_t* obj = bActive && !bInternal ? GetObject(objectID) : nullptr;
	if (!obj) return 0;
	return F<uint16_t>(obj, O_DAMAGE_FLAGS) & 4 ? 2 : 1;
}

void Gore::PostDamage(void* damage, uint32_t objectID, uint32_t preDeadFlag)
{
	if (!bActive || bInternal || !preDeadFlag || !damage) return;
	uint8_t* unit = GetObject(objectID);
	if (!unit || F<uint16_t>(unit, O_TYPE) != 0) return;          // bipeds only
	if (!(F<uint16_t>(unit, O_DAMAGE_FLAGS) & 4)) return;          // it has to be dead now
	if (F<uint32_t>(unit, O_FLAGS) & 1) return;                    // (hidden)
	if (F<uint32_t>(unit, U_PLAYER) != NONE) return;               // the Chief: later milestone
	if (!IsSinglePlayer()) return;
	const UnitType* type = TypeOf(F<uint32_t>(unit, 0));
	if (type->disabled) return;
	uint8_t* d = reinterpret_cast<uint8_t*>(damage);
	float force = GibForce(unit, d, type);
	if (Game::instance.c_GoreLog->Value() && force > 0.f)
		Logger::log << "[Gore] hit on " << TagPath(F<uint32_t>(unit, 0)) << " force " << force << (preDeadFlag == 2 ? " (corpse)" : "") << std::endl;
	if (force < 1.f) return;
	for (Pending& p : pending) if (p.unit == objectID) return;
	for (Pending& p : pending)
	{
		if (p.unit != NONE) continue;
		p.unit = objectID;
		p.force = force;
		p.time = GetTickCount64();
		memcpy(p.damage, d, D_SIZE);
		float* dir = &F<float>(d, D_DIRECTION);
		float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (len > 0.0001f) { p.dir[0] = dir[0] / len; p.dir[1] = dir[1] / len; p.dir[2] = dir[2] / len; }
		else { p.dir[0] = p.dir[1] = 0.f; p.dir[2] = 1.f; }
		return;
	}
}

void Gore::Update()
{
	if (!bActive) return;
	__try
	{
		for (Pending& p : pending)
		{
			if (p.unit == NONE) continue;
			Pending copy = p;
			p.unit = NONE;
			Gib(copy);
		}
		ULONGLONG now = GetTickCount64();
		ULONGLONG life = (ULONGLONG)(Game::instance.c_GorePieceSeconds->Value() * 1000.f);
		for (Piece& piece : pieces)
		{
			if (piece.object == NONE) continue;
			if (!GetObject(piece.object)) { piece.object = NONE; continue; }
			if (now - piece.born > life) { DeleteObject(piece.object); piece.object = NONE; }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Logger::log << "[Gore] exception in update: gore switched off" << std::endl;
		bActive = false;
	}
}
