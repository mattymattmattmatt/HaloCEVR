#pragma once
#include "Maths/Vectors.h"
#include "Maths/Matrices.h"
#include "Helpers/Objects.h"

#define DRAW_DEBUG_AIM 0

enum class WeaponType
{
	Unknown,
	Pistol,
	AssaultRifle,
	Shotgun,
	RocketLauncher,
	Sniper,
	Flamethrower,
	PlasmaPistol,
	PlasmaRifle,
	PlasmaCannon,
	Needler,
	FuelRod
};

class WeaponHandler
{
public:
	bool IsCurrentWeaponOneHanded() const;
	// Origin and aim direction a thrown grenade will actually use, in world space
	bool GetGrenadeThrowPose(struct Vector3& outPos, struct Vector3& outAim) const;

	// One-off diagnostic state, see GRENADE_VELOCITY_DEBUG in WeaponHandler.cpp
	struct Vector3 lastGrenadeThrowOrigin;
	int grenadeVelocityScanFramesRemaining = 0;
	int grenadeVelocityScanIndex = 0;

	// Debug-only, see GRENADE_COUNT_HUNT_DEBUG in WeaponHandler.cpp
	void DumpPlayerBytesForGrenadeCountHunt(struct BaseDynamicObject* player, int throwIndex);
	void UpdateGrenadeVelocityScan();
	void UpdateViewModel(struct HaloID& id, struct Vector3* pos, struct Vector3* facing, struct Vector3* up, struct TransformQuat* boneTransforms, struct Transform* outBoneTransforms);
	void HandlePlasmaPistolCharge();
	void SetPlasmaPistolCharge();
	void PreFireWeapon(HaloID& weaponID, short param2);
	void PostFireWeapon(HaloID& weaponID, short param2);
	void PreThrowGrenade(HaloID& playerID);
	void PostThrowGrenade(HaloID& playerID);

	bool GetLocalWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetWorldWeaponAim(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetLocalWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;
	bool GetWorldWeaponScope(Vector3& outPosition, Vector3& outAim, Vector3& upDir) const;

	bool IsSniperScope() const;

	Vector3 localOffset;
	Vector3 localRotation;

protected:
	void RelocatePlayer(HaloID& PlayerID, bool bUseOffHand = false);

	inline void CalculateBoneTransform(int boneIndex, struct Bone* boneArray, struct Transform& root, struct TransformQuat* boneTransforms, struct Transform& outTransform) const;
	inline void CalculateHandTransform(Vector3* pos, Matrix4& handTransform) const;
	inline void CreateEndCap(int boneIndex, const struct Bone& currentBone, struct Transform* outBoneTransforms) const;
	inline void MoveBoneToTransform(int boneIndex, const class Matrix4& newTransform, struct Transform* realTransforms, struct Transform* outBoneTransforms) const;
	inline void UpdateCache(struct HaloID& id, struct AssetData_ModelAnimations* animationData);
	inline WeaponType GetWeaponType(struct Asset_Weapon* weapon) const;
	inline void HandleWeaponHaptics() const;

	inline void TransformToMatrix4(struct Transform& inTransform, class Matrix4& outMatrix) const;

	inline Vector3 GetScopeLocation(WeaponType Type) const;

	Matrix4 GetDominantHandTransform() const;
	Matrix4 GetOffHandTransform() const;

	struct ViewModelCache
	{
		HaloID currentAsset{ 0, 0 };
		int leftWristIndex = -1;
		int rightWristIndex = -1;
		int gunIndex = -1;
		int displayIndex = -1;
		Vector3 cookedFireOffset;
		Matrix3 cookedFireRotation;
		Vector3 fireOffset;
		Vector3 gunOffset;
		Matrix3 fireRotation;
		WeaponType weaponType = WeaponType::Unknown;
		bool IsShooting = false;

	} cachedViewModel;

	UnitDynamicObject* weaponFiredPlayer = nullptr;
	Vector3 realPlayerPosition;
	Vector3 realPlayerAim;

	// Smoothed facing direction for 3DOF mode weapon model
	Vector3 smoothed3DOFFacingDir = Vector3(1.0f, 0.0f, 0.0f);
	bool bWasIn3DOFMode = false;

	// Track previous yaw offset to detect snap turns
	float lastYawOffset = 0.0f;

	// Debug stuff for checking where bullets are coming from/going
#if DRAW_DEBUG_AIM
	mutable Vector3 lastFireLocation;
	mutable Vector3 lastFireAim;
#endif
};