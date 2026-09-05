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
	void TryGrenadePunch(HaloID& unitID);
	void SetMousePosition(int& x, int& y);
	void UpdateMouseInfo(struct MouseInfo* mouseInfo);
	// Drives the VR menu click directly, for when Halo skips its own mouse
	// update because the physical mouse device has gone away.
	void UpdateVirtualMouseButtons();
	// Called every rendered frame from Game::PreDrawFrame with whether a Halo
	// menu is currently up.
	void NotifyMenuVisible(bool bVisible);
	// Double click of the right stick logs where the off hand is, relative to
	// the weapon hand, so a per-weapon hold pose can be authored in game.
	void UpdatePoseCapture();
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
	ControllerRole GetWeaponHand() const;
	ControllerRole GetOffHand() const;
	bool ComputeMeleeAimDirection(ControllerRole hand, Vector3& outDir) const;
	Vector3 GetHandWorldPosition(ControllerRole hand) const;
	bool FindMeleeTarget(const Vector3& origin, const Vector3& handWorld, Vector3& outDir) const;
	void SnapshotGrenadePunchTargets(HaloID localID);
	bool GrenadePunchHitValidTarget() const;
	unsigned char UpdateCrouch();
	// True while gameplay actions should be swallowed: any Halo menu is open,
	// or a button pressed on that menu is still being held after it closed.
	bool ShouldSuppressGameplayInputs();
	template<typename T> void SuppressGameplayInputs(T& controls) const
	{
		// Everything that acts on the world. MenuForward/MenuBack are left
		// alone so the menu itself stays navigable.
		controls.Fire = 0;
		controls.Melee = 0;
		controls.Grenade = 0;
		controls.Jump = 0;
		controls.Interact = 0;
		controls.Reload = 0;
		controls.Flashlight = 0;
		controls.Crouch = 0;
		controls.Zoom = 0;
		controls.SwitchWeapons = 0;
		controls.SwitchGrenades = 0;
	}
	bool bGameplayInputLatched = false;
	bool bWasPoseGripping = false;
	bool bWasCapturePosePressed = false;
	float poseCaptureClickTimer = 0.0f;
	int gameplayInputGraceFrames = 0;

	// Update Controls that rely on the distance between hands
	void UpdateHandsProximity();
	void CheckSwapWeaponHand();
	void UpdateTwoHandedHold(float handDistance, bool handsWithinSwapWeaponDistance);

	char lastSnapState = 0;
	// buttonState[0]: pulsed for one frame on press. Halo's UI dispatches an
	// activation every frame this reads non-zero, so a trigger held for ten
	// frames fires ten activations and retriggers the menu sound each time,
	// leaving it inaudible. A real mouse click only lasts a frame or two.
	unsigned char mouseDownState = 0;
	// buttonState2[0]: set for the single frame a held button is released,
	// which is how Halo marks a completed click.
	unsigned char mouseReleaseEdge = 0;
	bool bMouseWasDown = false;

	bool bHoldingMenu = false;
	std::chrono::time_point<std::chrono::high_resolution_clock> menuHeldTime;

	bool bWasGripping = false;
	bool bWasSwappingHands = false;
	bool bWasTappingHUD = false;
	bool bWasGrenadeHeld = false;
	// Suppresses the real throw on the release that ends a punching hold.
	// Cleared by the throw path's debounced release, so it stays in step with
	// the throw pulse it is guarding.
	bool bGrenadePunchUsedThisHold = false;
	// Enforces one punch per hold. Cleared as soon as the button physically
	// reads as not held, independent of the throw debounce.
	bool bGrenadePunchSpentThisHold = false;
	bool bInGrenadePunch = false;
	struct ReentryGuard
	{
		bool& flag;
		explicit ReentryGuard(bool& f) : flag(f) { flag = true; }
		~ReentryGuard() { flag = false; }
	};
	bool bGrenadePunchArmed = false;
	// Arming only lasts for the swing that set it, so a stale arm from an
	// earlier punch can never fire a later one.
	float grenadePunchArmTimer = 0.0f;
	int grenadePunchInvulnFrames = 0;
	int grenadePunchArmFrames = 0;
	uint16_t grenadePunchSavedDamageFlags = 0;
	float grenadePunchSavedHealth = 0.0f;
	float grenadePunchSavedShield = 0.0f;
	Vector3 grenadePunchSavedVelocity = Vector3(0.0f, 0.0f, 0.0f);
	HaloID grenadePunchPlayerID{};
	static const int kMaxGrenadePunchSpawns = 8;
	HaloID grenadePunchSpawnedIDs[kMaxGrenadePunchSpawns]{};
	int grenadePunchSpawnCount = 0;
	struct GrenadePunchHpSnap
	{
		uint16_t index = 0;
		uint16_t datumId = 0;
		float health = 0.0f;
		float shield = 0.0f;
		bool isVehicle = false;
		// Distance from the fist at the moment the swing landed.
		float distSqr = 0.0f;
	};
	static const int kMaxGrenadePunchSnaps = 48;
	GrenadePunchHpSnap grenadePunchSnaps[kMaxGrenadePunchSnaps];
	int grenadePunchSnapCount = 0;
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
	InputBindingID CapturePose = 0;

	InputBindingID SwapWeaponHand = 0;
	InputBindingID OffhandSwapWeaponHand = 0;

private:
	bool IsHandInHolster(const Vector3& handPos, const Vector3& holsterPos, const float& holsterActivationDistance);
};

