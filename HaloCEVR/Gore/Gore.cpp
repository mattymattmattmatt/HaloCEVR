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
#include "../Helpers/Maths.h"

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
	// damage_definition inside jpt!: category sits 0x12 before the upper bound
	// (decompiled damage_effect_definitions.h layout, anchored on 0x1DC).
	constexpr int JPT_CATEGORY = 0x1CA;
	constexpr int16_t CAT_HIGHEXPLOSIVE = 4, CAT_MOUNTEDWEAPON = 8, CAT_SHOTGUN = 11;
	constexpr uint32_t DFLAG_FROM_WEAPON = 1u << 3; // damage_data flags bit 3
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

	struct Pending { uint32_t unit = NONE; bool headshot = false; bool hunter = false; bool shotgun = false; bool player = false; float force = 0.f; float dir[3]{}; bool vehicleHit = false; float vehicleVel[3]{}; uint8_t damage[D_SIZE]{}; ULONGLONG time = 0; };
	constexpr int MAX_NODES = 64;
	constexpr int O_NODE_MATRICES = 0x1F0;  // short size, short offset from the object
	constexpr int O_NEXT_OBJECT = 0x114;
	constexpr int O_FIRST_CHILD = 0x118;
	constexpr int U_FRAG_COUNT = 0x31E;
	constexpr int MOD2_NODES = 0xB8;
	constexpr int MOD2_NODE_SIZE = 0x9C;
	constexpr int MATRIX_SIZE = sizeof(Transform); // 52: scale, rotation, translation
	static_assert(sizeof(Transform) == 52, "node matrix");          // scale, forward, left, up, position
	// A piece: a dead copy of the whole unit; each node not in the piece is
	// shrunk to nothing at its joint every frame before the scene is drawn.
	struct Piece { uint32_t object = NONE; ULONGLONG born = 0; int nodeCount = 0; int8_t collapse[MAX_NODES]; bool logged = false; bool hunter = false; bool player = false;
		float vel[3]{}; float spin[3]{}; int reapply = 0; int keep = -1;
		uint32_t trailEffect = NONE; uint32_t splatEffect = NONE; int ticks = 0; bool airborne = true; int splats = 0;
		// Once frozen the game stops rebuilding its nodes (object flag 23) and
		// we own them: the collapsed pose, moved along with the object.
	};

	// Weapons and equipment that existed when a body split; anything new
	// near the split in the next 2 seconds is a piece's drop and goes.
	struct Sweep { Vector3 at; ULONGLONG until = 0; };
	constexpr int MAX_SWEEPS = 16;
	constexpr int MAX_GIB_PIECES = 12; // sanity: no split may spawn more pieces than this
	Sweep sweeps[MAX_SWEEPS];
	constexpr int MAX_KNOWN = 1024;
	uint32_t known[MAX_KNOWN];
	int knownCount = 0;

	constexpr int MAX_PENDING = 16;
	constexpr int MAX_PIECES = 256;
	Pending pending[MAX_PENDING];
	Piece pieces[MAX_PIECES];

	// Hunter direct-hit credit: 10 ticks at 30 ticks/s = 1/3 s. The shell's
	// detonation, not its impact damage, usually lands the killing blow.
	struct HunterHit { uint32_t unit = NONE; ULONGLONG time = 0; float dir[3]{}; float epi[3]{}; };
	constexpr ULONGLONG HUNTER_WINDOW_MS = 333;
	constexpr int MAX_HUNTER_HITS = 8;
	HunterHit hunterHits[MAX_HUNTER_HITS];
	float lastHunterDir[3] = { 0.f, 0.f, 1.f };

	// Shotgun pellets aggregated per blast: 6 ticks = 200 ms. The first
	// pellet captures the clear shooter-to-victim distance.
	struct ShotgunBlast { uint32_t victim = NONE; uint32_t owner = NONE; uint32_t def = NONE; ULONGLONG first = 0, last = 0; int pellets = 0; bool decided = false; float distance = 0.f; };
	constexpr ULONGLONG SHOTGUN_WINDOW_MS = 200;
	constexpr int MAX_SHOTGUN_BLASTS = 16;
	ShotgunBlast shotgunBlasts[MAX_SHOTGUN_BLASTS];

	bool bActive = false;
	bool lastVehicleHit = false;
	float lastVehicleVel[3]{};
	int activePieces = 0;
	bool bCollapseFailed = false;
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

	std::string LowerPath(uint32_t tag) { std::string v = TagPath(tag) ? TagPath(tag) : ""; for (char& c : v) c = (char)std::tolower((unsigned char)c); return v; }
	bool IsHunterUnit(uint8_t* unit) { return LowerPath(F<uint32_t>(unit, 0)).find("hunter") != std::string::npos; }
	// Pure machines have no flesh to gib: Sentinels, monitors (343 Guilty
	// Spark), Constructors, Enforcers. They only reach gore via the ""
	// catch-all species entry, whose blast rules then chain: a Sentinel's
	// death explosion gibs its neighbours, each into 6 more copies (~50).
	bool IsMachineUnit(uint8_t* unit)
	{
		std::string path = LowerPath(F<uint32_t>(unit, 0));
		return path.find("sentinel") != std::string::npos || path.find("monitor") != std::string::npos ||
			path.find("guilty") != std::string::npos || path.find("constructor") != std::string::npos ||
			path.find("enforcer") != std::string::npos;
	}
	// The local player's unit, from the player table - the old check on
	// the unit's player field (0x218) did NOT stop the Chief gibbing
	// (session log: cyborg pieces carrying frag 4 + rifle/pistol drops).
	bool IsPlayerUnit(uint32_t objectID)
	{
		HaloID local;
		return Helpers::GetLocalPlayerID(local) && *reinterpret_cast<uint32_t*>(&local) == objectID;
	}
	// -1 when the jpt! tag is unreadable: callers fall back to the tag path.
	int16_t DamageCategory(uint8_t* damage)
	{
		uint8_t* jpt = TagData(F<uint32_t>(damage, D_DEFINITION));
		if (!jpt) return -1;
		int16_t c = F<int16_t>(jpt, JPT_CATEGORY);
		return (c >= 0 && c <= CAT_SHOTGUN) ? c : -1;
	}

	// Runs before each damage lands (PreDamage): remember scorpion shell
	// direct hits on Hunters, and aggregate shotgun pellets per blast.
	void RecordPreDamage(uint8_t* unit, uint32_t unitID, uint8_t* d)
	{
		ULONGLONG now = GetTickCount64();
		bool dead = (F<uint16_t>(unit, O_DAMAGE_FLAGS) & 4) != 0;
		if (!dead && IsHunterUnit(unit))
		{
			// Decisive logging: EVERY damage event on a Hunter, raw.
			std::string what = LowerPath(F<uint32_t>(d, D_DEFINITION));
			uint32_t flags = F<uint32_t>(d, D_FLAGS);
			int16_t cat = DamageCategory(d);
			bool scorpion = what.find("scorpion") != std::string::npos;
			bool aoe = (flags & 1) != 0;
			float* epi = &F<float>(d, D_EPICENTER);
			float* cen = &F<float>(unit, O_CENTRE);
			float radius = F<float>(unit, O_RADIUS);
			float dx = epi[0]-cen[0], dy = epi[1]-cen[1], dz = epi[2]-cen[2];
			float dCentre = sqrtf(dx*dx+dy*dy+dz*dz);
			float clear = dCentre - radius; // <0: epicentre inside the bounding sphere
			uint32_t ownerID = F<uint32_t>(d, D_OWNER_OBJECT);
			uint8_t* ownerObj = ownerID != NONE ? GetObject(ownerID) : nullptr;
			bool tankOn = Game::instance.c_GoreHunterTank && Game::instance.c_GoreHunterTank->Value();
			// Chosen direct-hit signals (retail gives a shell NO separate
			// non-AoE impact event on the unit - the log of 2026-10-17 shows
			// only 'vehicles\\scorpion\\shell explosion' AoE plus a generic
			// 'weapons\\frag grenade\\shock wave' event from the same blast):
			//  A) legacy: scorpion, not AoE, from a weapon, explosive/mounted
			//     category (coax bullet excluded; unreadable cat falls back);
			//  B) geometric: scorpion AoE whose epicentre is at the Hunter's
			//     body (inside its bounding sphere, or within 0.30 wu past it).
			// A grenade creates neither: its tag is not scorpion.
			bool directA = scorpion && !aoe && (flags & DFLAG_FROM_WEAPON) && (cat < 0 || cat == CAT_HIGHEXPLOSIVE || cat == CAT_MOUNTEDWEAPON);
			bool directB = scorpion && aoe && clear <= 0.30f;
			bool record = tankOn && (directA || directB);
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] hunter damage: '" << TagPath(F<uint32_t>(d, D_DEFINITION)) << "' cat " << (int)cat
					<< " flags 0x" << std::hex << flags << std::dec << " aoe " << (aoe ? 1 : 0)
					<< " owner " << (ownerObj ? TagPath(F<uint32_t>(ownerObj, 0)) : "(none)") << " type " << (ownerObj ? (int)F<uint16_t>(ownerObj, O_TYPE) : -1)
					<< " epi-centre " << dCentre << " wu, clear " << clear << " wu (radius " << radius << ")"
					<< " GoreHunterTank " << (tankOn ? 1 : 0) << (record ? " -> DIRECT HIT recorded" : (scorpion ? " -> scorpion, not direct" : " -> not scorpion")) << std::endl;
			if (record)
			{
				HunterHit* slot = nullptr;
				for (HunterHit& h : hunterHits) if (h.unit == unitID) { slot = &h; break; }
				if (!slot) for (HunterHit& h : hunterHits) if (h.unit == NONE || now - h.time > HUNTER_WINDOW_MS) { slot = &h; break; }
				if (!slot) slot = &hunterHits[0];
				slot->unit = unitID; slot->time = now;
				for (int k = 0; k < 3; k++) slot->epi[k] = epi[k];
				float* dir = &F<float>(d, D_DIRECTION);
				float len = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
				if (len > 0.0001f) for (int k=0;k<3;k++) slot->dir[k]=dir[k]/len;
				else if (directB) { // an AoE blast's way: from epicentre into the body
					float bx = -dx, by = -dy, bz = -dz; float bl = sqrtf(bx*bx+by*by+bz*bz);
					if (bl > 0.0001f) { slot->dir[0]=bx/bl; slot->dir[1]=by/bl; slot->dir[2]=bz/bl; }
					else { slot->dir[0]=slot->dir[1]=0.f; slot->dir[2]=1.f; }
				}
				else { slot->dir[0]=slot->dir[1]=0.f; slot->dir[2]=1.f; }
			}
		}
		if (!dead && Game::instance.c_GoreShotgun && Game::instance.c_GoreShotgun->Value() &&
			F<uint16_t>(unit, O_TYPE) == 0 && F<uint32_t>(unit, O_PARENT) == NONE)
		{
			int16_t cat = DamageCategory(d);
			if (cat == CAT_SHOTGUN || LowerPath(F<uint32_t>(d, D_DEFINITION)).find("shotgun") != std::string::npos)
			{
				uint32_t owner = F<uint32_t>(d, D_OWNER_OBJECT);
				uint8_t* shooter = (owner != NONE && owner != unitID) ? GetObject(owner) : nullptr;
				if (shooter && F<uint16_t>(shooter, O_TYPE) == 0)
				{
					uint32_t def = F<uint32_t>(d, D_DEFINITION);
					ShotgunBlast* blast = nullptr; ShotgunBlast* freeSlot = nullptr;
					for (ShotgunBlast& b : shotgunBlasts)
					{
						if (!freeSlot && (b.victim == NONE || now - b.last > SHOTGUN_WINDOW_MS)) freeSlot = &b;
						if (b.victim == unitID && b.owner == owner && b.def == def && now - b.last <= SHOTGUN_WINDOW_MS) blast = &b;
					}
					if (!blast)
					{
						blast = freeSlot;
						if (!blast) return;
						blast->victim = unitID; blast->owner = owner; blast->def = def;
						blast->first = now; blast->pellets = 0; blast->decided = false;
						float* c1 = &F<float>(shooter, O_CENTRE); float* c2 = &F<float>(unit, O_CENTRE);
						float dx=c1[0]-c2[0], dy=c1[1]-c2[1], dz=c1[2]-c2[2];
						float dist = sqrtf(dx*dx+dy*dy+dz*dz) * METRES_PER_WU -
							(F<float>(shooter, O_RADIUS) + F<float>(unit, O_RADIUS)) * METRES_PER_WU;
						blast->distance = dist < 0.f ? 0.f : dist;
						if (Game::instance.c_GoreLog->Value())
							Logger::log << "[Gore] shotgun blast tracked on " << TagPath(F<uint32_t>(unit, 0)) << ": clear distance " << blast->distance << " m (range " << (Game::instance.c_GoreShotgunRange ? Game::instance.c_GoreShotgunRange->Value() : 2.f) << " m)" << std::endl;
					}
					blast->last = now; blast->pellets++;
				}
			}
		}
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
		if (!(F<uint32_t>(damage, D_FLAGS) & 1)) // only blasts and vehicle hits
		{
			// A stuck plasma grenade kills with its 'attached' damage, not the blast.
			std::string what = TagPath(F<uint32_t>(damage, D_DEFINITION));
			for (char& c : what) c = (char)std::tolower((unsigned char)c);
			if (what.find("grenade") != std::string::npos && what.find("attached") != std::string::npos)
			{
				if (Game::instance.c_GoreLog->Value()) Logger::log << "[Gore] stuck grenade kill on " << TagPath(F<uint32_t>(unit, 0)) << ": gibs" << std::endl;
				return 2.0f;
			}
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] kill on " << TagPath(F<uint32_t>(unit, 0)) << " by " << TagPath(F<uint32_t>(damage, D_DEFINITION))
					<< " is not a blast (flags " << std::hex << F<uint32_t>(damage, D_FLAGS) << std::dec << "): no gib" << std::endl;
			return 0.f;
		}

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
				lastVehicleHit = true;
				lastVehicleVel[0] = v[0]; lastVehicleVel[1] = v[1]; lastVehicleVel[2] = v[2];
				float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 30.f;
				float threshold = type->vehicleThreshold / scale; // (>1: slower hits gib)
				return threshold > 0.f ? speed / threshold : 0.f;
			}
		}

		uint8_t* jpt = TagData(F<uint32_t>(damage, D_DEFINITION));
		float full = jpt ? F<float>(jpt, JPT_UPPER_BOUND) : 0.f;
		if (full <= 0.f || full > 100000.f) full = FRAG_DAMAGE; // unreadable: treat it as a frag
		float* c = &F<float>(unit, O_CENTRE);
		float r = F<float>(unit, O_RADIUS);
		float z = epi[2];
		float bottom = c[2] - r * 0.9f, top = c[2] + r * 0.6f;
		z = z < bottom ? bottom : (z > top ? top : z);
		float dx = epi[0] - c[0], dy = epi[1] - c[1], dz = z - c[2];
		float measure = sqrtf(dx * dx + dy * dy + dz * dz) * METRES_PER_WU;
		// The port's radii, widened for retail's plasma (3-5 m kills): x1.6.
		float threshold = 1.6f * type->gibRadius * sqrtf(full / FRAG_DAMAGE) * scale;
		float force = threshold / (measure > 0.15f ? measure : 0.15f);
		// The port's rule: a blast within 2 m (a stuck grenade) always gibs.
		if (measure < 2.0f * scale && force < 1.5f) force = 1.5f;
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] blast on " << TagPath(F<uint32_t>(unit, 0)) << ": " << TagPath(F<uint32_t>(damage, D_DEFINITION))
				<< " full " << full << (jpt ? "" : " (no tag)") << ", " << measure << " m, gibs within " << threshold << " m, force " << force << std::endl;
		return force;
	}

	void SnapshotWeapons(const Vector3& at, ULONGLONG durationMs = 10000);

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
		if (uint8_t* oldObj = GetObject(pieces[oldest].object)) SnapshotWeapons(F<Vector3>(oldObj, O_POSITION));
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

	int NodeFind(uint8_t* model, const char* name)
	{
		int count = F<int32_t>(model, MOD2_NODES);
		uint8_t* nodes = F<uint8_t*>(model, MOD2_NODES + 4);
		if (!nodes) return -1;
		for (int i = 0; i < count && i < MAX_NODES; i++)
			if (_strnicmp(reinterpret_cast<const char*>(nodes + i * MOD2_NODE_SIZE), name, 32) == 0) return i;
		return -1;
	}

	void MarkSubtree(uint8_t* model, int node, int8_t* owner, int8_t value, int depth = 0)
	{
		int count = F<int32_t>(model, MOD2_NODES);
		uint8_t* nodes = F<uint8_t*>(model, MOD2_NODES + 4);
		while (node >= 0 && node < count && node < MAX_NODES && depth < MAX_NODES)
		{
			uint8_t* n = nodes + node * MOD2_NODE_SIZE;
			owner[node] = value;
			MarkSubtree(model, F<int16_t>(n, 0x22), owner, value, depth + 1);
			node = F<int16_t>(n, 0x20);
		}
	}

	void LogRegionsAndNodes(uint8_t* model, const char* path)
	{
		int rc = F<int32_t>(model, MOD2_REGIONS);
		uint8_t* regions = F<uint8_t*>(model, MOD2_REGIONS + 4);
		Logger::log << "[Gore] " << path << " regions(" << rc << "):";
		for (int r = 0; regions && r < rc && r < 8; r++) Logger::log << " '" << std::string(reinterpret_cast<const char*>(regions + r * MOD2_REGION_SIZE), strnlen(reinterpret_cast<const char*>(regions + r * MOD2_REGION_SIZE), 32)) << "'";
		int nc = F<int32_t>(model, MOD2_NODES);
		Logger::log << " nodes " << nc << std::endl;
	}

	// Anything a dying piece can leave: weapons, equipment (grenade pickups),
	// garbage, and projectiles nobody alive threw (a dropped live grenade).
	bool IsPieceID(uint32_t id) { for (Piece& p : pieces) if (p.object == id) return true; return false; }
	bool IsLooseItem(uint8_t* o)
	{
		uint16_t t = F<uint16_t>(o, O_TYPE);
		if (F<uint32_t>(o, O_PARENT) != NONE) return false;
		if (t == 2 || t == 3 || t == 4) return true;
		if (t == 5)
		{
			uint32_t owner = F<uint32_t>(o, 0xC4);
			return owner == NONE || IsPieceID(owner) || !GetObject(owner);
		}
		return false;
	}

	void SnapshotWeapons(const Vector3& at, ULONGLONG durationMs)
	{
		ObjectTable& table = Helpers::GetObjectTable();
		knownCount = 0;
		for (int i = 0; table.elements && i < table.currentSize && knownCount < MAX_KNOWN; i++)
		{
			ObjectDatum& d = table.elements[i];
			if (!d.dynamicObject || d.id == 0) continue;
			uint8_t* o = reinterpret_cast<uint8_t*>(d.dynamicObject);
			if (F<uint16_t>(o, O_TYPE) >= 2 && F<uint16_t>(o, O_TYPE) <= 5)
				known[knownCount++] = (uint32_t(d.id) << 16) | uint32_t(i);
		}
		for (Sweep& w : sweeps)
			if (w.until == 0 || GetTickCount64() > w.until) { w.at = at; w.until = GetTickCount64() + durationMs; return; }
	}

	void SweepDrops()
	{
		ULONGLONG now = GetTickCount64();
		bool any = false;
		for (Sweep& w : sweeps) if (w.until && now <= w.until) any = true; else w.until = 0;
		if (!any) return;
		ObjectTable& table = Helpers::GetObjectTable();
		for (int i = 0; table.elements && i < table.currentSize; i++)
		{
			ObjectDatum& d = table.elements[i];
			if (!d.dynamicObject || d.id == 0) continue;
			uint8_t* o = reinterpret_cast<uint8_t*>(d.dynamicObject);
			if (!IsLooseItem(o)) continue;
			uint32_t id = (uint32_t(d.id) << 16) | uint32_t(i);
			bool old = false;
			for (int k = 0; k < knownCount && !old; k++) old = known[k] == id;
			if (old) continue;
			Vector3 pos = F<Vector3>(o, O_POSITION);
			for (Sweep& w : sweeps)
			{
				if (!w.until) continue;
				float dx = pos.x - w.at.x, dy = pos.y - w.at.y, dz = pos.z - w.at.z;
				if (dx * dx + dy * dy + dz * dz < 9.f)
				{
					Logger::log << "[Gore] removed a piece's dropped " << TagPath(F<uint32_t>(o, 0)) << " (type " << F<uint16_t>(o, O_TYPE) << ")" << std::endl;
					DeleteObject(id);
					break;
				}
			}
		}
	}

	// Delete EVERY object parented to a piece, by scanning the object
	// table. The old first-child-chain walk logged 'removed 0' for the
	// Chief's copies: held weapons are not reliably that chain, so each
	// copy kept the player loadout and dropped it on death.
	int DeleteChildrenOf(uint32_t pieceID)
	{
		int removed = 0;
		ObjectTable& table = Helpers::GetObjectTable();
		for (int i = 0; table.elements && i < table.currentSize; i++)
		{
			ObjectDatum& d = table.elements[i];
			if (!d.dynamicObject || d.id == 0) continue;
			uint8_t* o = reinterpret_cast<uint8_t*>(d.dynamicObject);
			if (F<uint32_t>(o, O_PARENT) != pieceID) continue;
			uint32_t childID = (uint32_t(d.id) << 16) | uint32_t(i);
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] stripped a piece's attached " << TagPath(F<uint32_t>(o, 0)) << " (type " << F<uint16_t>(o, O_TYPE) << ")" << std::endl;
			DeleteObject(childID);
			removed++;
		}
		return removed;
	}

	// Fresh loose drops near a point (not in the pre-gib snapshot): a
	// piece's death/expiry drops. The original body's own death drop
	// predates the snapshot, so it is in known[] and survives - exactly
	// one normal drop set remains.
	void DeleteFreshLooseNear(const Vector3& at, float radius)
	{
		ObjectTable& table = Helpers::GetObjectTable();
		for (int i = 0; table.elements && i < table.currentSize; i++)
		{
			ObjectDatum& d = table.elements[i];
			if (!d.dynamicObject || d.id == 0) continue;
			uint8_t* o = reinterpret_cast<uint8_t*>(d.dynamicObject);
			if (!IsLooseItem(o)) continue;
			uint32_t id = (uint32_t(d.id) << 16) | uint32_t(i);
			bool old = false;
			for (int k = 0; k < knownCount && !old; k++) old = known[k] == id;
			if (old) continue;
			Vector3 pos = F<Vector3>(o, O_POSITION);
			float dx = pos.x - at.x, dy = pos.y - at.y, dz = pos.z - at.z;
			if (dx * dx + dy * dy + dz * dz < radius * radius)
			{
				Logger::log << "[Gore] removed a piece's dropped " << TagPath(F<uint32_t>(o, 0)) << " (type " << F<uint16_t>(o, O_TYPE) << ")" << std::endl;
				DeleteObject(id);
			}
		}
	}

	void SpawnCopy(uint8_t* unit, uint32_t unitID, uint32_t regionMask, const Pending& p, uint32_t blood, const int8_t* collapse = nullptr, int nodeCount = 0, int keepNode = -1, uint32_t splat = NONE)
	{
		HaloID tag; *reinterpret_cast<uint32_t*>(&tag) = F<uint32_t>(unit, 0);
		HaloID none; none.index = 0xFFFF; none.id = 0xFFFF;
		Vector3 pos = F<Vector3>(unit, O_POSITION);
		// Where this piece's kept bone is in the world (the unit's node matrices).
		Vector3 bone = F<Vector3>(unit, O_CENTRE);
		{
			int16_t size = F<int16_t>(unit, O_NODE_MATRICES), offset = F<int16_t>(unit, O_NODE_MATRICES + 2);
			if (keepNode >= 0 && offset > 0 && keepNode < size / MATRIX_SIZE)
				bone = reinterpret_cast<Transform*>(unit + offset)[keepNode].translation;
		}
		Vector3 centre = F<Vector3>(unit, O_CENTRE);
		// The piece's origin is its kept bone: the game spins and moves an
		// object about its origin, and after each update the whole pose is
		// shifted so that bone sits on it (PostObjectUpdate).
		if (keepNode >= 0) pos = bone;
		HaloID created = Helpers::SpawnObject(tag, pos, none);
		uint32_t id = *reinterpret_cast<uint32_t*>(&created);
		uint8_t* obj = GetObject(id);
		if (!obj) return;
		unit = GetObject(unitID); // (the table may have moved nothing, but re-read anyway)
		if (!unit) { DeleteObject(id); return; }

		Piece* piece = NewPiece();
		piece->object = id;
		piece->born = GetTickCount64();
		piece->logged = false;
		piece->nodeCount = collapse ? nodeCount : 0;
		piece->keep = keepNode;
		for (int n = 0; n < MAX_NODES; n++) piece->collapse[n] = collapse && n < nodeCount ? collapse[n] : -1;

		// A unit made this way can come with its starting weapons: anything
		// attached goes before the kill, or the death drops it on the floor.
		int removed = DeleteChildrenOf(id);
		obj = GetObject(id);
		if (!obj) { piece->object = NONE; return; }
		if (F<uint16_t>(obj, O_TYPE) == 0)
		{
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] piece grenades before clearing: frag " << int(F<uint8_t>(obj, U_FRAG_COUNT)) << ", plasma " << int(F<uint8_t>(obj, U_FRAG_COUNT + 1)) << std::endl;
			F<uint8_t>(obj, U_FRAG_COUNT) = 0;
			F<uint8_t>(obj, U_FRAG_COUNT + 1) = 0;
		}
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] piece " << std::hex << id << std::dec << " spawned, removed " << removed << " attached objects"
				<< (F<uint32_t>(obj, O_FIRST_CHILD) != NONE ? " (still has a child!)" : "") << std::endl;

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
		F<float>(obj, O_BODY + 4) = 0.f; // shield (an Elite's would soak the kill)
		bInternal = true;
		CauseDamage(damage, id);
		bInternal = false;
		obj = GetObject(id);
		if (!obj) { piece->object = NONE; return; }
		if (!(F<uint16_t>(obj, O_DAMAGE_FLAGS) & 4))
		{
			// It didn't die (an immune damage type): never leave a standing copy.
			DeleteObject(id);
			piece->object = NONE;
			return;
		}
		{ // anything the kill itself shook loose (drops spawn during death)
			int stripped = DeleteChildrenOf(id);
			DeleteFreshLooseNear(F<Vector3>(obj, O_POSITION), 2.f);
			if (stripped && Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] piece " << std::hex << id << std::dec << ": stripped " << stripped << " more attached objects after its kill" << std::endl;
			obj = GetObject(id);
			if (!obj) { piece->object = NONE; return; }
		}
		F<uint16_t>(obj, O_DAMAGE_FLAGS) |= (1 << 11); // cannot take damage: no second gib
		F<uint32_t>(obj, O_FLAGS) |= (1u << 17);       // never collected as garbage early

		// Each piece flies its own way (world units a tick): out from the blast
		// along the line to its kept bone, or with the vehicle for a splatter,
		// plus up and a random spread, all scaled by the force.
		float force = p.force < 1.f ? 1.f : (p.force > 3.f ? 3.f : p.force);
		float out[3];
		const float* epi = reinterpret_cast<const float*>(p.damage + D_EPICENTER);
		out[0] = bone.x - epi[0]; out[1] = bone.y - epi[1]; out[2] = bone.z - epi[2];
		if (p.vehicleHit) { out[0] = bone.x - centre.x; out[1] = bone.y - centre.y; out[2] = bone.z - centre.z; }
		float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
		if (len < 0.001f) { out[0] = p.dir[0]; out[1] = p.dir[1]; out[2] = p.dir[2]; len = 1.f; }
		for (float& o : out) o /= len;
		float speed = force * Rand(0.08f, 0.16f);
		if (p.headshot)
		{
			// The head flies along the bullet; the body just drops.
			speed = keepNode >= 0 ? Rand(0.06f, 0.1f) : 0.f;
			if (keepNode >= 0) { out[0] = p.dir[0]; out[1] = p.dir[1]; out[2] = p.dir[2]; }
		}
		if (p.shotgun || p.hunter) { out[0] = p.dir[0]; out[1] = p.dir[1]; out[2] = p.dir[2]; } // pieces scatter the shot's / shell's way
		float vel[3];
		for (int k = 0; k < 3; k++) vel[k] = out[k] * speed + Rand(-1.f, 1.f) * speed * 0.35f;
		vel[2] += speed * Rand(0.3f, 0.7f) + 0.04f;
		if (p.vehicleHit)
			for (int k = 0; k < 3; k++) vel[k] += p.vehicleVel[k] * Rand(0.6f, 1.0f);
		if (p.headshot && keepNode < 0) vel[0] = vel[1] = vel[2] = 0.f; // the body drops where it stood
		float* v = &F<float>(obj, O_VELOCITY);
		for (int k = 0; k < 3; k++) { v[k] = vel[k]; piece->vel[k] = vel[k]; }
		float* w = &F<float>(obj, O_ANGULAR);
		for (int k = 0; k < 3; k++) { piece->spin[k] = Rand(-0.4f, 0.4f) * force; w[k] = piece->spin[k]; }
		if (p.headshot && keepNode < 0) for (int k = 0; k < 3; k++) { piece->spin[k] = 0.f; w[k] = 0.f; }
		piece->reapply = 3; // the first updates can kill a fresh corpse's speed
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] piece " << std::hex << id << std::dec << " node " << keepNode << " velocity " << vel[0] << " " << vel[1] << " " << vel[2]
				<< (p.vehicleHit ? " (vehicle)" : "") << std::endl;

		EffectOnObject(blood, id, "body");
		piece->trailEffect = blood;
		piece->splatEffect = splat != NONE ? splat : blood;
		piece->ticks = 0; piece->airborne = true; piece->splats = 0; piece->hunter = p.hunter; piece->player = p.player;
		if (bCollapseFailed || !collapse) F<uint32_t>(obj, O_FLAGS) |= 1u; // never a standing whole copy
	}

	void Gib(const Pending& p)
	{
		uint8_t* unit = GetObject(p.unit);
		if (!unit || F<uint16_t>(unit, O_TYPE) != 0) return;
		uint32_t def = F<uint32_t>(unit, 0);
		if (IsMachineUnit(unit))
		{
			if (Game::instance.c_GoreLog->Value()) Logger::log << "[Gore] " << TagPath(def) << " machine/not organic, no gib" << std::endl;
			return;
		}
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
		// A Hunter carries a lot of (orange, from its own collision effects)
		// blood: three more bursts at the split. VR blood is effect-only -
		// no decal API is ported - so extra bursts stand in for bigger splats.
		if (p.hunter)
		{
			for (int b = 0; b < 3; b++) EffectOnObject(blood, p.unit, "body");
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] hunter tank gib: 3 extra blood bursts at the split on " << TagPath(def) << std::endl;
		}

		int parts = 0;
		uint32_t used = 0;
		if (model) LogRegionsAndNodes(model, TagPath(def));
		SnapshotWeapons(F<Vector3>(GetObject(p.unit), O_POSITION));

		// True limbs first: any bip01 skeleton (one-region Grunts included).
		static const char* const roots[] = { "bip01 head", "bip01 l upperarm", "bip01 r upperarm", "bip01 l thigh", "bip01 r thigh" };
		int rootNodes[5];
		bool bones = model != nullptr;
		int nodeCount = model ? F<int32_t>(model, MOD2_NODES) : 0;
		if (nodeCount > MAX_NODES) nodeCount = MAX_NODES;
		uint8_t* unitObj = GetObject(p.unit);
		if (bones && unitObj && F<int16_t>(unitObj, O_NODE_MATRICES) / MATRIX_SIZE < nodeCount)
		{
			Logger::log << "[Gore] node matrices " << F<int16_t>(unitObj, O_NODE_MATRICES) / MATRIX_SIZE << " < model nodes " << nodeCount << ": no bone split" << std::endl;
			bones = false;
		}
		for (int i = 0; bones && i < 5; i++)
		{
			rootNodes[i] = NodeFind(model, roots[i]);
			if (rootNodes[i] < 0) { Logger::log << "[Gore] no node '" << roots[i] << "': no bone split" << std::endl; bones = false; }
		}
		if (bones)
		{
			int8_t owner[MAX_NODES]{};
			uint8_t* nodes = F<uint8_t*>(model, MOD2_NODES + 4);
			for (int i = 0; i < 5; i++)
			{
				owner[rootNodes[i]] = (int8_t)(i + 1);
				MarkSubtree(model, F<int16_t>(nodes + rootNodes[i] * MOD2_NODE_SIZE, 0x22), owner, (int8_t)(i + 1));
			}
			if (p.headshot) Logger::log << "[Gore] decapitating " << TagPath(def) << std::endl;
			for (int piece = 0; piece <= (p.headshot ? 1 : 5); piece++)
			{
				int8_t collapse[MAX_NODES];
				for (int n = 0; n < MAX_NODES; n++)
				{
					// A headshot's body keeps everything but the head's nodes.
					if (p.headshot && piece == 0) { collapse[n] = (n < nodeCount && owner[n] == 1) ? (int8_t)rootNodes[0] : -1; continue; }
					if (n >= nodeCount || owner[n] == piece) collapse[n] = -1;
					else if (piece) collapse[n] = (int8_t)rootNodes[piece - 1];
					else collapse[n] = (int8_t)rootNodes[owner[n] - 1];
				}
				SpawnCopy(GetObject(p.unit), p.unit, 0xFF, p, trail != NONE ? trail : blood, collapse, nodeCount,
					piece ? rootNodes[piece - 1] : (p.headshot ? -1 : NodeFind(model, "bip01 spine")), blood);
				parts++;
			}
		}
		else if (model)
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
			// Safety cap: a model whose split would spawn more than
			// MAX_GIB_PIECES pieces aborts to a normal death + blood
			// only, so no weird model can ever explode into dozens.
			if (parts > MAX_GIB_PIECES)
			{
				Logger::log << "[Gore] " << TagPath(def) << ": split would spawn " << parts << " pieces (cap " << MAX_GIB_PIECES << "): aborting gib, blood only" << std::endl;
				parts = 0;
			}
			if (parts >= 2)
			{
				for (int part = 0; part < NUM_PARTS; part++)
					if (masks[part]) SpawnCopy(GetObject(p.unit), p.unit, masks[part], p, trail != NONE ? trail : blood);
			}
		}
		Logger::log << "[Gore] " << TagPath(def) << " came apart into " << parts << " pieces (" << (bones ? "bones" : "regions") << "), force " << p.force << (p.hunter ? " (hunter tank gib)" : "") << (p.shotgun ? " (shotgun point-blank)" : "") << (p.player ? " (player)" : "") << std::endl;

		// The corpse goes when it split; a one-region model just bleeds and stays.
		if (parts >= 2)
		{
			// Hidden at once (retail deletes at the end of the objects' update,
			// so it's still in the table right after the call).
			if (uint8_t* body = GetObject(p.unit)) F<uint32_t>(body, O_FLAGS) |= 1u;
			DeleteObject(p.unit);
			uint8_t* body = GetObject(p.unit);
			if (body && !(F<uint32_t>(body, O_FLAGS) & 1u)) Logger::log << "[Gore] the original body is still visible" << std::endl;
			else if (Game::instance.c_GoreLog->Value()) Logger::log << "[Gore] original body hidden and deleted" << (body ? " (deletion pending)" : "") << std::endl;
		}
	}
}

namespace
{

	// Shrink every node the piece doesn't keep onto its joint (the view
	// model's end-cap trick: scale 0). Returns how many were collapsed.
	int Collapse(Piece& piece, uint8_t* obj)
	{
		int16_t size = F<int16_t>(obj, O_NODE_MATRICES);
		int16_t offset = F<int16_t>(obj, O_NODE_MATRICES + 2);
		int count = size / MATRIX_SIZE;
		if (count < piece.nodeCount || offset <= 0) return -1;
		Transform* nodes = reinterpret_cast<Transform*>(obj + offset);
		int collapsed = 0;
		for (int n = 0; n < piece.nodeCount; n++)
		{
			int to = piece.collapse[n];
			if (to < 0 || to >= count) continue;
			nodes[n].translation = nodes[to].translation;
			for (int j = 0; j < 9; j++) nodes[n].rotation[j] = nodes[to].rotation[j];
			nodes[n].scale = 0.f;
			collapsed++;
		}
		return collapsed;
	}

	void HidePieces()
	{
		for (Piece& piece : pieces)
			if (uint8_t* obj = GetObject(piece.object)) F<uint32_t>(obj, O_FLAGS) |= 1u;
	}
	int overwrittenFrames = 0;
}

void Gore::PostObjectUpdate(uint32_t objectID)
{
	if (!bActive || activePieces == 0) return;
	__try
	{
		for (Piece& piece : pieces)
		{
			if (piece.object != objectID || piece.nodeCount == 0) continue;
			uint8_t* obj = GetObject(objectID);
			if (!obj) return;
			if (piece.reapply > 0)
			{
				float* v = &F<float>(obj, O_VELOCITY);
				float want = piece.vel[0] * piece.vel[0] + piece.vel[1] * piece.vel[1] + piece.vel[2] * piece.vel[2];
				float have = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
				if (have < want * 0.25f)
				{
					for (int k = 0; k < 3; k++) { v[k] = piece.vel[k]; F<float>(obj, O_ANGULAR + k * 4) = piece.spin[k]; }
					if (Game::instance.c_GoreLog->Value())
						Logger::log << "[Gore] piece " << std::hex << objectID << std::dec << " lost its speed on update: re-applied" << std::endl;
				}
				piece.reapply--;
			}
			// Blood: drips while it flies (the body's own localized blood
			// effect, so the colour is the species'), and a splat each time
			// it lands (the body's area blood effect; the game's effect puts
			// its decals on the level itself).
			piece.ticks++;
			{
				float* v = &F<float>(obj, O_VELOCITY);
				float speed2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
				bool onGround = (F<uint32_t>(obj, O_FLAGS) & 2u) != 0;
				// Hunter pieces drip more often and for longer (OpenCE:
				// every 6 ticks for 90, against 12/36 for other species).
				if (piece.ticks < (piece.hunter ? 270 : 150) && speed2 > 0.0009f && piece.ticks % (piece.hunter ? 3 : 6) == 0)
					EffectOnObject(piece.trailEffect, objectID, "body");
				if (piece.airborne && onGround && piece.ticks > 3 && piece.splats < (piece.hunter ? 6 : 3))
				{
					EffectOnObject(piece.splatEffect, objectID, "body");
					if (piece.hunter) EffectOnObject(piece.splatEffect, objectID, "body"); // bigger splat: effect-only blood, so a second burst
					piece.splats++;
					if (Game::instance.c_GoreLog->Value())
						Logger::log << "[Gore] piece " << std::hex << objectID << std::dec << " landed: splat " << piece.splats << std::endl;
				}
				piece.airborne = !onGround;
			}
			int collapsed = Collapse(piece, obj);
			if (collapsed >= 0 && piece.keep >= 0 && piece.keep < piece.nodeCount)
			{
				Transform* nodes = reinterpret_cast<Transform*>(obj + F<int16_t>(obj, O_NODE_MATRICES + 2));
				Vector3 at = F<Vector3>(obj, O_POSITION);
				Vector3 d(at.x - nodes[piece.keep].translation.x, at.y - nodes[piece.keep].translation.y, at.z - nodes[piece.keep].translation.z);
				for (int n = 0; n < piece.nodeCount; n++)
				{
					nodes[n].translation.x += d.x; nodes[n].translation.y += d.y; nodes[n].translation.z += d.z;
				}
				// The bounding sphere follows, or the piece is culled by the old one.
				F<Vector3>(obj, O_CENTRE) = at;
			}
			if (!piece.logged)
			{
				if (collapsed < 0) Logger::log << "[Gore] piece " << std::hex << objectID << std::dec << " has no node matrices: not collapsed" << std::endl;
				else if (Game::instance.c_GoreLog->Value()) Logger::log << "[Gore] piece " << std::hex << objectID << std::dec << " collapsed " << collapsed << " nodes after its update" << std::endl;
				piece.logged = true;
			}
			return;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Logger::log << "[Gore] exception collapsing a piece: gore switched off" << std::endl;
		bActive = false;
	}
}

// Before each draw: check (only) that the collapse survived to the draw.
// If something rebuilt the bones after the update, the pieces would be
// standing whole bodies: hide them all and leave just the blood.
void Gore::PreRender()
{
	if (!bActive || activePieces == 0 || bCollapseFailed) return;
	__try
	{
		int checked = 0, intact = 0;
		for (Piece& piece : pieces)
		{
			if (piece.object == NONE || piece.nodeCount == 0 || !piece.logged) continue;
			uint8_t* obj = GetObject(piece.object);
			if (!obj) continue;
			int16_t offset = F<int16_t>(obj, O_NODE_MATRICES + 2);
			if (offset <= 0) continue;
			Transform* nodes = reinterpret_cast<Transform*>(obj + offset);
			for (int n = 0; n < piece.nodeCount; n++)
			{
				if (piece.collapse[n] < 0) continue;
				checked++;
				if (nodes[n].scale == 0.f) intact++;
				break;
			}
		}
		if (checked == 0) return;
		if (intact == checked) { overwrittenFrames = 0; return; }
		if (++overwrittenFrames >= 3)
		{
			bCollapseFailed = true;
			Logger::log << "[Gore] the collapse is overwritten before the draw (" << intact << " of " << checked
				<< " pieces intact): falling back to hidden pieces, blood only" << std::endl;
			HidePieces();
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Logger::log << "[Gore] exception checking pieces: gore switched off" << std::endl;
		bActive = false;
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
	Logger::log << "[Gore] config: GoreEnabled=1 GoreThresholdScale=" << Game::instance.c_GoreThresholdScale->Value()
		<< " GoreHeadshots=" << (Game::instance.c_GoreHeadshots->Value() ? 1 : 0)
		<< " GoreShotgun=" << (Game::instance.c_GoreShotgun && Game::instance.c_GoreShotgun->Value() ? 1 : 0)
		<< " GoreShotgunRange=" << (Game::instance.c_GoreShotgunRange ? Game::instance.c_GoreShotgunRange->Value() : -1.f)
		<< " GoreHunterTank=" << (Game::instance.c_GoreHunterTank && Game::instance.c_GoreHunterTank->Value() ? 1 : 0)
		<< " GoreLog=" << (Game::instance.c_GoreLog->Value() ? 1 : 0) << " (keys missing from VR/config.txt keep these defaults)" << std::endl;
	return true;
}

bool Gore::IsActive() { return bActive; }

uint32_t Gore::PreDamage(void* damage, uint32_t objectID)
{
	uint8_t* obj = bActive && !bInternal ? GetObject(objectID) : nullptr;
	if (!obj) return 0;
	if (damage) RecordPreDamage(obj, objectID, reinterpret_cast<uint8_t*>(damage));
	return F<uint16_t>(obj, O_DAMAGE_FLAGS) & 4 ? 2 : 1;
}

namespace
{
	// A sniper rifle kill on the head: the hit node is in the head's subtree
	// (bip01 head and below), or, with no node, the hit point is by the head.
	bool IsSniperHeadshot(uint8_t* unit, uint8_t* damage, int32_t node)
	{
		std::string what = TagPath(F<uint32_t>(damage, D_DEFINITION));
		for (char& c : what) c = (char)std::tolower((unsigned char)c);
		if (what.find("sniper") == std::string::npos) return false;
		uint8_t* obje = TagData(F<uint32_t>(unit, 0));
		uint8_t* model = obje ? TagData(F<uint32_t>(obje, OBJE_MODEL)) : nullptr;
		if (!model) return false;
		int head = NodeFind(model, "bip01 head");
		if (head < 0) { Logger::log << "[Gore] sniper kill: model has no 'bip01 head'" << std::endl; return false; }
		bool hit = false;
		const char* how = "";
		if (node >= 0 && node < MAX_NODES)
		{
			int8_t owner[MAX_NODES]{};
			owner[head] = 1;
			uint8_t* nodes = F<uint8_t*>(model, MOD2_NODES + 4);
			MarkSubtree(model, F<int16_t>(nodes + head * MOD2_NODE_SIZE, 0x22), owner, 1);
			hit = owner[node] == 1;
			how = "node";
		}
		else
		{
			int16_t size = F<int16_t>(unit, O_NODE_MATRICES), offset = F<int16_t>(unit, O_NODE_MATRICES + 2);
			if (offset > 0 && head < size / MATRIX_SIZE)
			{
				Vector3 h = reinterpret_cast<Transform*>(unit + offset)[head].translation;
				const float* e = reinterpret_cast<const float*>(damage + D_EPICENTER);
				float dx = e[0] - h.x, dy = e[1] - h.y, dz = e[2] - h.z;
				hit = dx * dx + dy * dy + dz * dz < 0.25f * 0.25f; // within ~0.75 m
				how = "hit point";
			}
		}
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] sniper kill on " << TagPath(F<uint32_t>(unit, 0)) << ": node " << node << " (" << how << ") "
				<< (hit ? "is the head: decapitate" : "is not the head") << std::endl;
		return hit;
	}
	// A Hunter's killing hit judged. Retail wrinkle (log of 2026-10-17):
	// one shell detonation delivers BOTH 'vehicles\\scorpion\\shell
	// explosion' damage AND a generic 'weapons\\frag grenade\\shock wave'
	// event at the same epicentre, and either can be the killing event -
	// so a fresh direct-hit record qualifies the kill by EPICENTRE, not
	// by the killing tag's name alone. A thrown grenade can never create
	// a record (its tag is not scorpion), so it can never gib a Hunter.
	float HunterTankForce(uint8_t* unit, uint8_t* d, uint32_t unitID)
	{
		if (!Game::instance.c_GoreHunterTank || !Game::instance.c_GoreHunterTank->Value()) return 0.f;
		std::string what = LowerPath(F<uint32_t>(d, D_DEFINITION));
		ULONGLONG now = GetTickCount64();
		float* epi = &F<float>(d, D_EPICENTER);
		for (HunterHit& h : hunterHits)
		{
			if (h.unit != unitID || now - h.time > HUNTER_WINDOW_MS) continue;
			float dx = epi[0]-h.epi[0], dy = epi[1]-h.epi[1], dz = epi[2]-h.epi[2];
			float epiDist = sqrtf(dx*dx+dy*dy+dz*dz);
			bool qualifies = what.find("scorpion") != std::string::npos || epiDist <= 1.0f;
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] hunter kill judged: killing damage '" << TagPath(F<uint32_t>(d, D_DEFINITION)) << "', direct hit " << (now - h.time) / 33
					<< " ticks ago, killing epicentre " << epiDist << " wu from the shell's -> " << (qualifies ? "GIBS" : "not this shell's kill") << std::endl;
			if (!qualifies) return 0.f;
			for (int k = 0; k < 3; k++) lastHunterDir[k] = h.dir[k];
			h.unit = NONE; // spent
			return 2.8f;
		}
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] hunter killed by " << TagPath(F<uint32_t>(d, D_DEFINITION)) << " not gibbed: "
				<< (what.find("scorpion") != std::string::npos ? "scorpion splash, no direct shell hit on record" : "only a direct scorpion shell gibs a hunter") << std::endl;
		return 0.f;
	}

	// A killing shotgun hit's blast judged once: ANY kill within
	// GoreShotgunRange (clear metres) gibs, however hurt the victim was.
	bool ShotgunGib(uint8_t* unit, uint8_t* d, uint32_t unitID)
	{
		if (!Game::instance.c_GoreShotgun || !Game::instance.c_GoreShotgun->Value()) return false;
		ULONGLONG now = GetTickCount64();
		ShotgunBlast* blast = nullptr;
		for (ShotgunBlast& b : shotgunBlasts)
			if (b.victim == unitID && b.owner == F<uint32_t>(d, D_OWNER_OBJECT) && b.def == F<uint32_t>(d, D_DEFINITION) &&
				now - b.last <= SHOTGUN_WINDOW_MS && now - b.first <= SHOTGUN_WINDOW_MS) blast = &b;
		if (!blast)
		{
			if (Game::instance.c_GoreLog->Value() && LowerPath(F<uint32_t>(d, D_DEFINITION)).find("shotgun") != std::string::npos)
				Logger::log << "[Gore] shotgun kill on " << TagPath(F<uint32_t>(unit, 0)) << " not gibbed: no blast tracked (its first pellet was not seen)" << std::endl;
			return false;
		}
		if (blast->decided) return false;
		blast->decided = true;
		float range = Game::instance.c_GoreShotgunRange ? Game::instance.c_GoreShotgunRange->Value() : 2.f;
		if (range <= 0.f) range = 2.f;
		if (blast->distance > range)
		{
			if (Game::instance.c_GoreLog->Value())
				Logger::log << "[Gore] shotgun kill on " << TagPath(F<uint32_t>(unit, 0)) << " not gibbed: not point blank (" << blast->distance << " m clear, limit " << range << " m; " << blast->pellets << " pellets)" << std::endl;
			return false;
		}
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] shotgun point-blank kill gibs " << TagPath(F<uint32_t>(unit, 0)) << " (" << blast->pellets << " pellets, " << blast->distance << " m)" << std::endl;
		return true;
	}
}

void Gore::PostDamage(void* damage, uint32_t objectID, uint32_t preDeadFlag, int32_t node)
{
	if (!bActive || bInternal || !preDeadFlag || !damage) return;
	uint8_t* unit = GetObject(objectID);
	if (!unit || F<uint16_t>(unit, O_TYPE) != 0) return;          // bipeds only
	if (!(F<uint16_t>(unit, O_DAMAGE_FLAGS) & 4)) return;          // it has to be dead now
	if (F<uint32_t>(unit, O_FLAGS) & 1) return;                    // (hidden)
	if (!IsSinglePlayer()) return;
	bool isPlayer = IsPlayerUnit(objectID);
	if (IsMachineUnit(unit))
	{
		if (Game::instance.c_GoreLog->Value())
			Logger::log << "[Gore] " << TagPath(F<uint32_t>(unit, 0)) << " machine/not organic, no gib" << std::endl;
		return;
	}
	uint8_t* d = reinterpret_cast<uint8_t*>(damage);
	const UnitType* type = TypeOf(F<uint32_t>(unit, 0));
	bool headshot = false, hunter = false, shotgun = false;
	float force = 0.f;
	if (IsHunterUnit(unit))
	{
		// Hunters never gib - except a direct Scorpion shell (GoreHunterTank).
		// Only the killing transition is judged; corpse hits just spam.
		if (preDeadFlag != 1) return;
		force = HunterTankForce(unit, d, objectID);
		if (force < 1.f) return;
		hunter = true;
	}
	else
	{
		if (type->disabled) return;
		lastVehicleHit = false;
		force = GibForce(unit, d, type);
		if (Game::instance.c_GoreLog->Value() && force > 0.f)
			Logger::log << "[Gore] hit on " << TagPath(F<uint32_t>(unit, 0)) << " force " << force << (preDeadFlag == 2 ? " (corpse)" : "") << std::endl;
		if (force < 1.f)
		{
			// Only the killing shot (not a corpse), behind GoreHeadshots.
			if (preDeadFlag == 1 && Game::instance.c_GoreHeadshots->Value() && IsSniperHeadshot(unit, d, node)) { headshot = true; force = 1.f; }
			// Any point-blank shotgun kill gibs, however hurt the victim was.
			else if (preDeadFlag == 1 && ShotgunGib(unit, d, objectID)) { shotgun = true; force = 2.2f; }
			else return;
		}
	}
	for (Pending& p : pending) if (p.unit == objectID) return;
	for (Pending& p : pending)
	{
		if (p.unit != NONE) continue;
		p.unit = objectID;
		p.force = force;
		p.headshot = headshot;
		p.hunter = hunter;
		p.shotgun = shotgun;
		p.player = isPlayer;
		p.vehicleHit = lastVehicleHit;
		memcpy(p.vehicleVel, lastVehicleVel, sizeof(p.vehicleVel));
		p.time = GetTickCount64();
		memcpy(p.damage, d, D_SIZE);
		float* dir = &F<float>(d, D_DIRECTION);
		float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (len > 0.0001f) { p.dir[0] = dir[0] / len; p.dir[1] = dir[1] / len; p.dir[2] = dir[2] / len; }
		else { p.dir[0] = p.dir[1] = 0.f; p.dir[2] = 1.f; }
		if (hunter) for (int k = 0; k < 3; k++) p.dir[k] = lastHunterDir[k]; // scatter along the shell's way, not the detonation's
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
		SweepDrops();
		{ // player pieces: catch drops their deaths shake loose late
			static ULONGLONG lastWatch = 0;
			ULONGLONG now0 = GetTickCount64();
			if (now0 - lastWatch >= 250)
			{
				lastWatch = now0;
				for (Piece& piece : pieces)
					if (piece.player && piece.object != NONE && now0 - piece.born < 10000)
						if (uint8_t* po = GetObject(piece.object)) DeleteFreshLooseNear(F<Vector3>(po, O_POSITION), 2.f);
			}
		}
		activePieces = 0;
		for (Piece& piece : pieces) if (piece.object != NONE) activePieces++;
		ULONGLONG now = GetTickCount64();
		ULONGLONG life = (ULONGLONG)(Game::instance.c_GorePieceSeconds->Value() * 1000.f);
		for (Piece& piece : pieces)
		{
			if (piece.object == NONE) continue;
			if (!GetObject(piece.object)) { piece.object = NONE; continue; }
			if (now - piece.born > life)
			{ // arm a sweep first: whatever its deletion drops is fresh meat
				SnapshotWeapons(F<Vector3>(GetObject(piece.object), O_POSITION));
				DeleteObject(piece.object); piece.object = NONE;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Logger::log << "[Gore] exception in update: gore switched off" << std::endl;
		bActive = false;
	}
}
