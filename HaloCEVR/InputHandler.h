#pragma once
#include "VR/IVR.h"
#include "Helpers/Objects.h"
#include <chrono>

class InputHandler
{
public:
	void RegisterInputs();
	void UpdateRegisteredInputs();
	void UpdateInputs(bool bInVehicle);
	// Raw held state of the grenade action, regardless of throw-on-release setting
	bool IsGrenadeHeld() const;
	void UpdateCamera(float& yaw, float& pitch);
	void UpdateCameraForVehicles(float& yaw, float& pitch);
	// Snap/smooth stick only. Does not inject HMD residuals into the game camera.
	void ApplyStickTurn();
	void NotifyVehicleExit();
	// Called from the melee-damage hook so aim is set in the same call
	// that reads it (same pattern as FireWeapon).
	void PreMeleeDamage(HaloID& unitID);
	void PostMeleeDamage(HaloID& unitID);
	void SetMousePosition(int& x, int& y);
	void UpdateMouseInfo(struct MouseInfo* mouseInfo);
	bool GetCalculatedHandPositions(Matrix4& controllerTransform, Vector3& dominantHandPos, Vector3& offHand);
	void CalculateSmoothedInput();

	Vector3 smoothedPosition = Vector3(0.0f, 0.0f, 0.0f);

	// Track previous yaw offset to detect snap turns for weapon position smoothing
	float lastSmoothingYawOffset = 0.0f;
	float vehicleFaceAimYaw = 0.0f;
	float vehicleFaceAimPitch = 0.0f;
	bool bLastYawInitialized = false;

protected:


	unsigned char UpdateFlashlight();
	void UpdateHUDToggle();
	void PlayHUDToggleSound();
	unsigned char UpdateHolsterSwitchWeapons();
	unsigned char UpdateMelee();
	void BeginMeleeAimOverride(ControllerRole hand);
	bool IsMeleeAimOverrideActive() const;
	bool ComputeMeleeAimDirection(ControllerRole hand, Vector3& outDir) const;
	Vector3 GetHandWorldPosition(ControllerRole hand) const;
	bool FindMeleeTarget(const Vector3& origin, const Vector3& handWorld, Vector3& outDir) const;
	unsigned char UpdateCrouch();

	// Update Controls that rely on the distance between hands
	void UpdateHandsProximity();
	void CheckSwapWeaponHand();
	void UpdateTwoHandedHold(float handDistance, bool handsWithinSwapWeaponDistance);

	char lastSnapState = 0;
	unsigned char mouseDownState = 0;

	bool bHoldingMenu = false;
	std::chrono::time_point<std::chrono::high_resolution_clock> menuHeldTime;

	bool bWasGripping = false;
	bool bWasSwappingHands = false;
	bool bWasTappingHUD = false;
	bool bWasGrenadeHeld = false;
	float grenadeReleaseTimer = 0.0f;
	int grenadeThrowPulseFrames = 0;
	bool bWasTogglingCrosshair = false;
	float crosshairToggleReleaseTimer = 1.0f;
	bool bWasMeleeButton = false;
	ControllerRole meleeAimHand = ControllerRole::Right;
	float meleeAimOverrideTimer = 0.0f;
	Vector3 meleeLockedDir = Vector3(1.0f, 0.0f, 0.0f);
	bool bHasMeleeLockedDir = false;
	bool bMeleeDamageOverridden = false;
	Vector3 savedMeleeFacingDir;
	Vector3 savedMeleeFacing;
	Vector3 savedMeleeDesiredAim;
	Vector3 savedMeleeAim;
	Vector3 savedMeleeAim2;
	Vector3 savedMeleeAim3;
	Vector3 savedMeleeCamLook;
	Vector3 savedMeleePlayerLook;
	
	InputBindingID Jump = 0;
	InputBindingID SwitchGrenades = 0;
	InputBindingID Interact = 0;
	InputBindingID SwitchWeapons = 0;
	InputBindingID Melee = 0;
	InputBindingID Flashlight = 0;
	InputBindingID Grenade = 0;
	InputBindingID Fire = 0;
	InputBindingID MenuForward = 0;
	InputBindingID MenuBack = 0;
	InputBindingID Crouch = 0;
	InputBindingID ToggleCrosshair = 0;
	InputBindingID Zoom = 0;
	InputBindingID Reload = 0;
	InputBindingID Move = 0;
	InputBindingID Look = 0;
	
	InputBindingID Recentre = 0;
	InputBindingID TwoHandGrip = 0;

	InputBindingID SwapWeaponHand = 0;
	InputBindingID OffhandSwapWeaponHand = 0;

private:
	bool IsHandInHolster(const Vector3& handPos, const Vector3& holsterPos, const float& holsterActivationDistance);
};

