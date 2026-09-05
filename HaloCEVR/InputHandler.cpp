#include "InputHandler.h"
#include "Game.h"
#include "Helpers/Controls.h"
#include "Helpers/Camera.h"
#include "Helpers/Objects.h"
#include "Helpers/Player.h"
#include "Helpers/Menus.h"
#include "Helpers/Cutscene.h"
#include "Helpers/Maths.h"
#include "Logger.h"
#include <chrono>
#include <cmath>
#include <algorithm>

#include <windows.h>
#include <mmsystem.h>
// Only used for the HUD toggle sound effect
#pragma comment(lib, "winmm.lib")


#define RegisterBoolInput(set, x) x = vr->RegisterBoolInput(set, #x);
#define RegisterVector2Input(set, x) x = vr->RegisterVector2Input(set, #x);
#define ApplyBoolInput(x) controls.##x = vr->GetBoolInput(x) ? 127 : 0;
#define ApplyImpulseBoolInput(x) controls.##x = vr->GetBoolInput(x, bHasChanged) && bHasChanged ? 127 : 0;

void InputHandler::RegisterInputs()
{
	IVR* vr = Game::instance.GetVR();

	// These bindings will always be the same and won't change depending on action sets
	RegisterBoolInput("left handed", SwapWeaponHand);
	OffhandSwapWeaponHand = SwapWeaponHand;
	RegisterBoolInput("default", SwapWeaponHand);

	UpdateRegisteredInputs();
}

void InputHandler::UpdateRegisteredInputs()
{
	IVR* vr = Game::instance.GetVR();

	const char* actionSet = Game::instance.bLeftHanded ? "left handed" : "default";
	
	// These bindings will change depending on action sets
	RegisterBoolInput(actionSet, Jump);
	RegisterBoolInput(actionSet, SwitchGrenades);
	RegisterBoolInput(actionSet, Interact);
	RegisterBoolInput(actionSet, SwitchWeapons);
	RegisterBoolInput(actionSet, Melee);
	RegisterBoolInput(actionSet, Flashlight);
	RegisterBoolInput(actionSet, Grenade);
	RegisterBoolInput(actionSet, Fire);
	RegisterBoolInput(actionSet, MenuForward);
	RegisterBoolInput(actionSet, MenuBack);
	RegisterBoolInput(actionSet, Crouch);
	RegisterBoolInput(actionSet, ToggleCrosshair);
	RegisterBoolInput(actionSet, Zoom);
	RegisterBoolInput(actionSet, Reload);
	RegisterBoolInput(actionSet, TwoHandGrip);

	RegisterVector2Input(actionSet, Move);
	RegisterVector2Input(actionSet, Look);
}

float AngleBetweenVector2(const Vector2& v1, const Vector2& v2)
{
	const float dot = v1.dot(v2);
	const float determinant = v1.x * v2.y - v1.y * v2.x;
	const float angle = atan2(determinant, dot);
	return angle;
}

Vector2 RotateVector2(const Vector2& v, float angle)
{
	Vector2 rotated;
	rotated.x = v.x * cos(angle) - v.y * sin(angle);
	rotated.y = v.x * sin(angle) + v.y * cos(angle);
	return rotated;
}

#define DRAW_DEBUG_MOVE 0

bool InputHandler::IsGrenadeHeld() const
{
	IVR* vr = Game::instance.GetVR();
	return vr && vr->GetBoolInput(Grenade);
}

void InputHandler::UpdateInputs(bool bInVehicle)
{
	IVR* vr = Game::instance.GetVR();

	vr->UpdateInputs();

	UpdateVirtualMouseButtons();

	const bool bSuppressGameplay = ShouldSuppressGameplayInputs();

	if (grenadePunchArmFrames > 0)
	{
		ObjectTable& objects = Helpers::GetObjectTable();
		bool anyAlive = false;
		for (int i = 0; i < grenadePunchSpawnCount; ++i)
		{
			const HaloID& spawnedID = grenadePunchSpawnedIDs[i];
			const bool slotValid = objects.elements
				&& spawnedID.index < objects.currentSize
				&& objects.elements[spawnedID.index].id == spawnedID.id;
			BaseDynamicObject* armedNade = slotValid ? objects.elements[spawnedID.index].dynamicObject : nullptr;
			if (armedNade)
			{
				// Do not restart the fuse; just keep it unfrozen / at-rest / armed.
				Helpers::ArmProjectileDetonation(armedNade, false);
				anyAlive = true;
			}
		}
		if (anyAlive)
		{
			grenadePunchArmFrames--;
		}
		else
		{
			grenadePunchArmFrames = 0;
			grenadePunchSpawnCount = 0;
		}
	}

	if (grenadePunchInvulnFrames > 0)
	{
		grenadePunchInvulnFrames--;
		UnitDynamicObject* punchPlayer = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(grenadePunchPlayerID));
		if (punchPlayer)
		{
			punchPlayer->N00000311 = grenadePunchSavedDamageFlags | (1u << 11);
			if (grenadePunchInvulnFrames == 0)
			{
				punchPlayer->N00000311 = grenadePunchSavedDamageFlags;
				punchPlayer->health = grenadePunchSavedHealth;
				punchPlayer->shield = grenadePunchSavedShield;
				punchPlayer->velocity = grenadePunchSavedVelocity;
			}
		}
		else if (grenadePunchInvulnFrames == 0)
		{
			// Player object went away; nothing to restore.
		}
	}

	static bool bHasChanged = false;

	if (Game::instance.bIsCustom)
	{
		Controls_Custom& controls = Helpers::GetControlsCustom();

		ApplyBoolInput(Jump);
		ApplyImpulseBoolInput(SwitchGrenades);
		ApplyBoolInput(Interact);
		ApplyBoolInput(Melee);
		ApplyBoolInput(Flashlight);
		if (Game::instance.c_ThrowGrenadeOnRelease->Value() && !bInVehicle)
		{
			// Throw on release: track the raw held state ourselves and only report
			// a press (127) to the game on the frame the button is released, so the
			// engine's own press-triggered throw fires at release instead of pull.
			//
			// The underlying SteamVR digital action can flicker (bChanged firing
			// repeatedly) during what is physically one continuous hold, so the raw
			// signal is debounced: a release only counts once the button has read
			// as not-held for a short window, not on the very first 0 frame.
			//
			// Skipped entirely while in a vehicle: the same control byte can be
			// reused there for something else entirely (e.g. a tank's secondary
			// machine gun), which needs the raw continuous held-state, not our
			// on-release pulse - reported as the gun firing once on release
			// instead of continuously while held.
			bool bGrenadeRawHeld = vr->GetBoolInput(Grenade);

			if (bGrenadeRawHeld)
			{
				grenadeReleaseTimer = 0.0f;
				bWasGrenadeHeld = true;
			}
			else if (bWasGrenadeHeld)
			{
				grenadeReleaseTimer += Game::instance.lastDeltaTime;
			}

			const float debounceTime = 0.08f; // ~5 frames at 60fps
			bool bReleasedThisFrame = bWasGrenadeHeld && !bGrenadeRawHeld && grenadeReleaseTimer >= debounceTime;

			if (bGrenadePunchUsedThisHold)
			{
				if (bReleasedThisFrame)
				{
					bWasGrenadeHeld = false;
					bGrenadePunchUsedThisHold = false;
					bGrenadePunchSpentThisHold = false;
					bGrenadePunchArmed = false;
					grenadePunchArmTimer = 0.0f;
				}
				grenadeThrowPulseFrames = 0;
				controls.Grenade = 0;
			}
			else
			{
				// Hold the throw pulse for a couple of frames rather than exactly one,
				// so it cannot land on a frame the engine happens not to poll cleanly.
				if (bReleasedThisFrame)
				{
					bWasGrenadeHeld = false;
					grenadeThrowPulseFrames = 3;
				}

				if (grenadeThrowPulseFrames > 0)
				{
					controls.Grenade = 127;
					grenadeThrowPulseFrames--;
				}
				else
				{
					controls.Grenade = 0;
				}
			}
		}
		else
		{
			ApplyBoolInput(Grenade);
		}
		ApplyBoolInput(Fire);
		ApplyBoolInput(MenuForward);
		ApplyBoolInput(MenuBack);
		ApplyBoolInput(Crouch);
		ApplyImpulseBoolInput(Zoom);
		ApplyBoolInput(Reload);

		if (bSuppressGameplay)
		{
			SuppressGameplayInputs(controls);
		}

		Game::instance.bIsFiring = controls.Fire;
	}
	else
	{
		Controls& controls = Helpers::GetControls();

		ApplyBoolInput(Jump);
		ApplyImpulseBoolInput(SwitchGrenades);
		ApplyBoolInput(Interact);
		ApplyBoolInput(Melee);
		ApplyBoolInput(Flashlight);
		if (Game::instance.c_ThrowGrenadeOnRelease->Value() && !bInVehicle)
		{
			// Throw on release: track the raw held state ourselves and only report
			// a press (127) to the game on the frame the button is released, so the
			// engine's own press-triggered throw fires at release instead of pull.
			//
			// The underlying SteamVR digital action can flicker (bChanged firing
			// repeatedly) during what is physically one continuous hold, so the raw
			// signal is debounced: a release only counts once the button has read
			// as not-held for a short window, not on the very first 0 frame.
			//
			// Skipped entirely while in a vehicle: the same control byte can be
			// reused there for something else entirely (e.g. a tank's secondary
			// machine gun), which needs the raw continuous held-state, not our
			// on-release pulse - reported as the gun firing once on release
			// instead of continuously while held.
			bool bGrenadeRawHeld = vr->GetBoolInput(Grenade);

			if (bGrenadeRawHeld)
			{
				grenadeReleaseTimer = 0.0f;
				bWasGrenadeHeld = true;
			}
			else if (bWasGrenadeHeld)
			{
				grenadeReleaseTimer += Game::instance.lastDeltaTime;
			}

			const float debounceTime = 0.08f; // ~5 frames at 60fps
			bool bReleasedThisFrame = bWasGrenadeHeld && !bGrenadeRawHeld && grenadeReleaseTimer >= debounceTime;

			if (bGrenadePunchUsedThisHold)
			{
				if (bReleasedThisFrame)
				{
					bWasGrenadeHeld = false;
					bGrenadePunchUsedThisHold = false;
					bGrenadePunchSpentThisHold = false;
					bGrenadePunchArmed = false;
					grenadePunchArmTimer = 0.0f;
				}
				grenadeThrowPulseFrames = 0;
				controls.Grenade = 0;
			}
			else
			{
				// Hold the throw pulse for a couple of frames rather than exactly one,
				// so it cannot land on a frame the engine happens not to poll cleanly.
				if (bReleasedThisFrame)
				{
					bWasGrenadeHeld = false;
					grenadeThrowPulseFrames = 3;
				}

				if (grenadeThrowPulseFrames > 0)
				{
					controls.Grenade = 127;
					grenadeThrowPulseFrames--;
				}
				else
				{
					controls.Grenade = 0;
				}
			}
		}
		else
		{
			ApplyBoolInput(Grenade);
		}
		ApplyBoolInput(Fire);
		ApplyBoolInput(MenuForward);
		ApplyBoolInput(MenuBack);
		ApplyBoolInput(Crouch);
		ApplyImpulseBoolInput(Zoom);
		ApplyBoolInput(Reload);

		if (bSuppressGameplay)
		{
			SuppressGameplayInputs(controls);
		}

		Game::instance.bIsFiring = controls.Fire;
	}


	unsigned char MotionControlFlashlight = UpdateFlashlight();
	if (MotionControlFlashlight > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Flashlight = MotionControlFlashlight;
		}
		else
		{
			Helpers::GetControls().Flashlight = MotionControlFlashlight;
		}
	}

	UpdateHUDToggle();

	// Toggle the floating crosshair when the bound action is pressed.
	//
	// Debounced rather than using plain edge detection: SteamVR digital actions
	// were observed (during the grenade throw-on-release work) reporting many
	// changes during what was physically a single continuous press, which with
	// plain edge detection toggles several times and lands on an effectively
	// random state. A press is only accepted once the input has read as released
	// continuously for a short window first.
	{
		bool bTogglePressedRaw = vr->GetBoolInput(ToggleCrosshair);

		if (bTogglePressedRaw)
		{
			if (!bWasTogglingCrosshair && crosshairToggleReleaseTimer >= 0.08f)
			{
				Game::instance.bShowCrosshair = !Game::instance.bShowCrosshair;
				bWasTogglingCrosshair = true;
			}
			crosshairToggleReleaseTimer = 0.0f;
		}
		else
		{
			crosshairToggleReleaseTimer += Game::instance.lastDeltaTime;
			if (crosshairToggleReleaseTimer >= 0.08f)
			{
				bWasTogglingCrosshair = false;
			}
		}
	}

	if (Game::instance.c_EnableWeaponHolsters->Value())
	{
		unsigned char HolsterSwitchWeapons = UpdateHolsterSwitchWeapons();
		bool bSwitchWeaponsPressed = vr->GetBoolInput(SwitchWeapons);

		if (HolsterSwitchWeapons > 0 && bSwitchWeaponsPressed)
		{
			if (Game::instance.bIsCustom)
			{
				Controls_Custom& controls = Helpers::GetControlsCustom();
				ApplyImpulseBoolInput(SwitchWeapons);
			}
			else
			{
				Controls& controls = Helpers::GetControls();
				ApplyImpulseBoolInput(SwitchWeapons);
			}
		}
	}
	else
	{
		if (Game::instance.bIsCustom)
		{
			Controls_Custom& controls = Helpers::GetControlsCustom();
			ApplyImpulseBoolInput(SwitchWeapons);
		}
		else
		{
			Controls& controls = Helpers::GetControls();
			ApplyImpulseBoolInput(SwitchWeapons);
		}
	}

	if (meleeAimOverrideTimer > 0.0f)
	{
		meleeAimOverrideTimer -= Game::instance.lastDeltaTime;
		if (meleeAimOverrideTimer <= 0.0f)
		{
			meleeAimOverrideTimer = 0.0f;
			bHasMeleeLockedDir = false;
		}
	}

	// The once-per-hold lock clears as soon as the button physically reads as
	// not held. The throw path's 0.08s release debounce must not gate this: a
	// quick re-grab inside that window used to leave the lock stuck on, and
	// every later punch that hold silently did nothing.
	if (bGrenadePunchSpentThisHold && !IsGrenadeHeld())
	{
		bGrenadePunchSpentThisHold = false;
	}

	if (grenadePunchArmTimer > 0.0f)
	{
		grenadePunchArmTimer -= Game::instance.lastDeltaTime;
		if (grenadePunchArmTimer <= 0.0f)
		{
			grenadePunchArmTimer = 0.0f;
			bGrenadePunchArmed = false;
		}
	}

	unsigned char MotionControlMelee = UpdateMelee();
	if (MotionControlMelee > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Melee = MotionControlMelee;
		}
		else
		{
			Helpers::GetControls().Melee = MotionControlMelee;
		}
	}

	unsigned char MotionControlCrouch = UpdateCrouch();
	if (MotionControlCrouch > 0)
	{
		if (Game::instance.bIsCustom)
		{
			Helpers::GetControlsCustom().Crouch = MotionControlCrouch;
		}
		else
		{
			Helpers::GetControls().Crouch = MotionControlCrouch;
		}
	}

	const float holdToRecentreTime = 1000.0f;

	bool bMenuChanged;
	bool bMenuPressed = vr->GetBoolInput(MenuBack, bMenuChanged);

	if (bMenuPressed)
	{
		if (bMenuChanged)
		{
			menuHeldTime = std::chrono::high_resolution_clock::now();
		}

		float heldTime = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - menuHeldTime).count();

		if (heldTime > 100.0f)
		{
			float progress = std::min(heldTime, holdToRecentreTime) / holdToRecentreTime;

			Matrix4 handTrans = vr->GetControllerTransform(ControllerRole::Left, true);

			Vector3 handPos = (handTrans * Vector3(0.0f, 0.0f, 0.0f)) * Game::instance.MetresToWorld(1.0f) + Helpers::GetCamera().position;

			Vector3 centre = handPos + Vector3(0.0f, 0.0f, Game::instance.MetresToWorld(0.1f));
			Vector3 facing = -handTrans.getLeftAxis();
			Vector3 upVector = handTrans.getForwardAxis();

			Game::instance.inGameRenderer.DrawPolygon(centre, facing, upVector, 16, Game::instance.MetresToWorld(0.01f), D3DCOLOR_XRGB(255, 0, 0), false, progress);
		}
	}
	else if (bMenuChanged)
	{
		float heldTime = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - menuHeldTime).count();

		if (heldTime > holdToRecentreTime)
		{
			Game::instance.bNeedsRecentre = true;
		}
		else
		{
			INPUT input{};
			input.type = INPUT_KEYBOARD;
			input.ki.dwFlags = KEYEVENTF_SCANCODE; // DirectInput only detects scancodes
			input.ki.wScan = 01; // Escape
			SendInput(1, &input, sizeof(INPUT));

			bHoldingMenu = true;
		}
	}
	else if (bHoldingMenu)
	{
		bHoldingMenu = false;

		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.dwFlags = KEYEVENTF_SCANCODE; // DirectInput only detects scancodes
		input.ki.wScan = 01; // Escape
		input.ki.dwFlags |= KEYEVENTF_KEYUP;
		SendInput(1, &input, sizeof(INPUT));
	}

	UpdateHandsProximity();

	Vector2 MoveInput = vr->GetVector2Input(Move);

	if (!bInVehicle && (Game::instance.c_HandRelativeMovement->Value() != 0))
	{
		Camera& cam = Helpers::GetCamera();
		Vector3 camForward = cam.lookDir;

		const bool leftHand = (Game::instance.c_HandRelativeMovement->Value() == 1);
		const ControllerRole role = leftHand ? ControllerRole::Left : ControllerRole::Right;
		Matrix4 controllerTransform = vr->GetControllerTransform(role); // TODO: Not sure about bRenderPose bool param here. Doesn't seem to matter.
		const float offset = Game::instance.c_HandRelativeOffsetRotation->Value();
		controllerTransform.rotateZ(leftHand ? offset: -offset);
		Vector3 handForward = controllerTransform.getLeftAxis();

#if DRAW_DEBUG_MOVE
		Vector3 start = cam.position + cam.lookDirUp * -Game::instance.MetresToWorld(0.1f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + camForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 255, 0, 0), false, 0.5f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + handForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 0, 0, 255), false, 0.5f);
		handForward.z = camForward.z = 0.0f;
		Game::instance.inGameRenderer.DrawLine3D(start, start + camForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 255, 75, 0), false, 0.5f);
		Game::instance.inGameRenderer.DrawLine3D(start, start + handForward * Game::instance.MetresToWorld(2.0f), D3DCOLOR_ARGB(255, 100, 0, 255), false, 0.5f);

		start = cam.position + cam.lookDir * Game::instance.MetresToWorld(2.00f);
		const Vector3 camRight = cam.lookDir.cross(cam.lookDirUp);
		Vector3 end = start + cam.lookDirUp * MoveInput.y * Game::instance.MetresToWorld(1.0f) + camRight * MoveInput.x * Game::instance.MetresToWorld(1.0f);
		Game::instance.inGameRenderer.DrawLine3D(start, end, D3DCOLOR_ARGB(255, 0, 255, 0), false, 0.5f);
#endif

		const float angle = AngleBetweenVector2(Vector2(camForward.x, camForward.y), Vector2(handForward.x, handForward.y));
		MoveInput = RotateVector2(MoveInput, angle);

#if DRAW_DEBUG_MOVE
		end = start + cam.lookDirUp * MoveInput.y * Game::instance.MetresToWorld(1.0f) + camRight * MoveInput.x * Game::instance.MetresToWorld(1.0f);
		Game::instance.inGameRenderer.DrawLine3D(start, end, D3DCOLOR_ARGB(255, 255, 255, 0), false, 0.5f);
#endif
	}

	if (Game::instance.bIsCustom)
	{
		Controls_Custom& controls = Helpers::GetControlsCustom();

		controls.Left = -MoveInput.x;
		controls.Forwards = MoveInput.y;
	}
	else
	{
		Controls& controls = Helpers::GetControls();

		controls.Left = -MoveInput.x;
		controls.Forwards = MoveInput.y;
	}
}

void InputHandler::ApplyStickTurn()
{
	IVR* vr = Game::instance.GetVR();

	Vector2 lookInput = vr->GetVector2Input(Look);

	float yawOffset = vr->GetYawOffset();

	if (Game::instance.c_SnapTurn->Value())
	{
		if (lastSnapState == 1)
		{
			if (lookInput.x < 0.4f)
			{
				lastSnapState = 0;
			}
		}
		else if (lastSnapState == -1)
		{
			if (lookInput.x > -0.4f)
			{
				lastSnapState = 0;
			}
		}
		else
		{
			if (lookInput.x > 0.6f)
			{
				lastSnapState = 1;
				yawOffset += Game::instance.c_SnapTurnAmount->Value();
			}
			else if (lookInput.x < -0.6f)
			{
				lastSnapState = -1;
				yawOffset -= Game::instance.c_SnapTurnAmount->Value();
			}
		}
	}
	else
	{
		yawOffset += lookInput.x * Game::instance.c_SmoothTurnAmount->Value() * Game::instance.lastDeltaTime;
	}

	vr->SetYawOffset(yawOffset);
}

void InputHandler::UpdateCamera(float& yaw, float& pitch)
{
	IVR* vr = Game::instance.GetVR();

	ApplyStickTurn();

	Vector3 lookHMD = vr->GetHMDTransform().getLeftAxis();
	// Get current camera angle
	Vector3 lookGame = Game::instance.bDetectedChimera ? Game::instance.LastLookDir : Helpers::GetCamera().lookDir;
	// Apply deltas
	float yawHMD = atan2(lookHMD.y, lookHMD.x);
	float yawGame = atan2(lookGame.y, lookGame.x);
	yaw = (yawHMD - yawGame);

	float pitchHMD = atan2(lookHMD.z, sqrt(lookHMD.x * lookHMD.x + lookHMD.y * lookHMD.y));
	float pitchGame = atan2(lookGame.z, sqrt(lookGame.x * lookGame.x + lookGame.y * lookGame.y));
	pitch = (pitchHMD - pitchGame);

	// Shortest-arc wrap. Without this, exiting a vehicle near the +/-180
	// boundary injects a near-full-turn slam — one reason the shake was
	// worse facing some directions than others.
	const float PI = 3.141593f;
	while (yaw > PI) yaw -= 2.0f * PI;
	while (yaw < -PI) yaw += 2.0f * PI;

	const float exitT = Game::instance.GetVehicleExitBlendT();
	if (exitT > 0.0f)
	{
		// exitT is 1 on the hop-out frame and falls to 0. Hold HMD
		// reconciliation back for the first part of Halo's 3rd→1st camera,
		// then ease it in. The previous approach applied the full residual
		// here AND shoved yawOffset in PreDrawFrame, so the two loops fought.
		float s = 1.0f - exitT;
		float delayed = (s - 0.4f) / 0.6f;
		if (delayed < 0.0f) delayed = 0.0f;
		if (delayed > 1.0f) delayed = 1.0f;
		const float inject = delayed * delayed * (3.0f - 2.0f * delayed);

		const float dt = Game::instance.lastDeltaTime;
		float rate = Game::instance.c_VehicleExitBlendRate
			? Game::instance.c_VehicleExitBlendRate->Value() : 6.0f;
		if (rate < 0.0f) rate = 0.0f;
		float absorb = 1.0f - expf(-rate * dt);
		if (absorb > 0.25f) absorb = 0.25f;
		absorb *= (1.0f - inject);

		const float RadToDeg = 180.0f / PI;
		float stepDeg = yaw * RadToDeg * absorb;
		const float maxStep = 120.0f * (dt > 0.0f ? dt : 0.0f);
		if (stepDeg > maxStep) stepDeg = maxStep;
		if (stepDeg < -maxStep) stepDeg = -maxStep;
		vr->SetYawOffset(vr->GetYawOffset() + stepDeg);

		yaw *= inject;
		pitch *= inject;
	}
}

void InputHandler::NotifyVehicleExit()
{
	vehicleFaceAimYaw = 0.0f;
	vehicleFaceAimPitch = 0.0f;
}

void InputHandler::UpdateCameraForVehicles(float& yaw, float& pitch)
{
	IVR* vr = Game::instance.GetVR();

	Vector2 lookInput = vr->GetVector2Input(Look);

	const float DegToRad = 3.141593f / 180.0f;

	const float YawDelta = lookInput.x * Game::instance.c_HorizontalVehicleTurnAmount->Value() * Game::instance.lastDeltaTime;
	const float PitchDelta = lookInput.y * Game::instance.c_VerticalVehicleTurnAmount->Value() * Game::instance.lastDeltaTime;

	// Stick always drives the body facing (yawOffset), preserving stock behaviour.
	float yawOffset = vr->GetYawOffset();
	yawOffset += YawDelta;
	vr->SetYawOffset(yawOffset);

	// Base (stock) per-frame camera deltas from the stick
	float stickYaw = -DegToRad * YawDelta;
	float stickPitch = DegToRad * PitchDelta;

	if (Game::instance.c_VehicleFaceAim && Game::instance.c_VehicleFaceAim->Value())
	{
		// EXPERIMENTAL head-aim: nudge the camera toward where the head is looking.
		// The residual between HMD facing and game facing is applied as an extra
		// delta, scaled and smoothed, so aim drifts toward head direction over time
		// rather than snapping. Stick still contributes for fine control.
		Vector3 lookHMD = vr->GetHMDTransform().getLeftAxis();
		Vector3 lookGame = Game::instance.bDetectedChimera
			? Game::instance.LastLookDir
			: Helpers::GetCamera().lookDir;

		float yawHMD = atan2(lookHMD.y, lookHMD.x);
		float yawGame = atan2(lookGame.y, lookGame.x);
		float yawResidual = yawHMD - yawGame;

		// Wrap to [-pi, pi] so it always turns the short way
		const float PI = 3.141593f;
		while (yawResidual > PI) yawResidual -= 2.0f * PI;
		while (yawResidual < -PI) yawResidual += 2.0f * PI;

		float smoothing = Game::instance.c_VehicleFaceAimSmoothing
			? Game::instance.c_VehicleFaceAimSmoothing->Value() : 0.4f;
		smoothing = std::max(0.0f, std::min(0.95f, smoothing));

		float blend = Game::instance.c_VehicleFaceAimBlend
			? Game::instance.c_VehicleFaceAimBlend->Value() : 0.8f;
		blend = std::max(0.0f, std::min(1.0f, blend));

		// Pitch residual (vertical). Same convention: angle above/below horizontal.
		float pitchHMD = atan2(lookHMD.z, sqrt(lookHMD.x * lookHMD.x + lookHMD.y * lookHMD.y));
		float pitchGame = atan2(lookGame.z, sqrt(lookGame.x * lookGame.x + lookGame.y * lookGame.y));
		float pitchResidual = pitchHMD - pitchGame;

		// Smooth both axes so the follow is gentle, not 1:1 twitchy
		vehicleFaceAimYaw = vehicleFaceAimYaw * smoothing + yawResidual * (1.0f - smoothing);
		vehicleFaceAimPitch = vehicleFaceAimPitch * smoothing + pitchResidual * (1.0f - smoothing);

		// Apply the smoothed residuals as this frame's face-aim deltas,
		// scaled by frame time for framerate independence
		float speed = Game::instance.c_VehicleFaceAimSpeed
			? Game::instance.c_VehicleFaceAimSpeed->Value() : 7.0f;
		float faceYaw = vehicleFaceAimYaw * Game::instance.lastDeltaTime * speed;
		float facePitch = vehicleFaceAimPitch * Game::instance.lastDeltaTime * speed;

		yaw = stickYaw * (1.0f - blend) + faceYaw * blend;
		pitch = stickPitch * (1.0f - blend) + facePitch * blend;
	}
	else
	{
		yaw = stickYaw;
		pitch = stickPitch;
	}
}

unsigned char InputHandler::UpdateFlashlight()
{
	IVR* vr = Game::instance.GetVR();

	Vector3 headPos = vr->GetHMDTransform() * Vector3(-0.1f, 0.0f, 0.0f);

	float leftDistance = Game::instance.c_LeftHandFlashlightDistance->Value();
	float rightDistance = Game::instance.c_RightHandFlashlightDistance->Value();

	bool offhandFlashlightEnabled = Game::instance.c_OffhandHandFlashlight->Value();

	bool checkLeftHand = !offhandFlashlightEnabled || !Game::instance.bLeftHanded;
	if (checkLeftHand && leftDistance > 0.0f)
	{
		Vector3 handPos = vr->GetRawControllerTransform(ControllerRole::Left) * Vector3(0.0f, 0.0f, 0.0f);

		if ((headPos - handPos).lengthSqr() < leftDistance * leftDistance)
		{
			return 127;
		}
	}

	bool checkRightHand = !offhandFlashlightEnabled || Game::instance.bLeftHanded;
	if (checkRightHand && rightDistance > 0.0f)
	{
		Vector3 handPos = vr->GetRawControllerTransform(ControllerRole::Right) * Vector3(0.0f, 0.0f, 0.0f);

		if ((headPos - handPos).lengthSqr() < rightDistance * rightDistance)
		{
			return 127;
		}
	}

	return 0;
}

void InputHandler::PlayHUDToggleSound()
{
	const std::string& soundFile = Game::instance.c_HUDToggleSound->Value();

	if (soundFile.empty())
	{
		return;
	}

	const std::string soundPath = "VR/" + soundFile;

	// SND_ASYNC returns immediately so we never stall the frame, SND_NODEFAULT stops
	// Windows playing its default beep if the file is missing or not a valid PCM wav
	if (!PlaySoundA(soundPath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			Logger::err << "[HUDToggle] Could not play " << soundPath
				<< ". Check the file exists and is an uncompressed 16-bit PCM .wav" << std::endl;
		}
	}
}

void InputHandler::UpdateHUDToggle()
{
	const float toggleDistance = Game::instance.c_HUDToggleDistance->Value();

	if (toggleDistance <= 0.0f)
	{
		bWasTappingHUD = false;
		return;
	}

	IVR* vr = Game::instance.GetVR();

	// Use the opposite hand to the flashlight so the two head gestures never share a
	// hand. The flashlight uses the offhand when OffhandHandFlashlight is set (the hand
	// not holding a weapon, i.e. left when right handed), otherwise the dominant hand.
	bool bFlashlightUsesLeft;
	if (Game::instance.c_OffhandHandFlashlight->Value())
	{
		// Offhand: left for right handed players, right for left handed players
		bFlashlightUsesLeft = !Game::instance.bLeftHanded;
	}
	else
	{
		// Dominant hand: right for right handed players, left for left handed players
		bFlashlightUsesLeft = Game::instance.bLeftHanded;
	}

	// The HUD toggle uses the inverse hand of the flashlight
	bool bHUDUsesLeft = !bFlashlightUsesLeft;

	// The offset's y axis points left, so flip it to sit beside whichever hand is used
	Vector3 offset = Game::instance.c_HUDToggleOffset->Value();
	if (bHUDUsesLeft)
	{
		offset.y = -offset.y;
	}

	const Vector3 togglePos = vr->GetHMDTransform() * offset;

	ControllerRole hudHand = bHUDUsesLeft ? ControllerRole::Left : ControllerRole::Right;
	Vector3 handPos = vr->GetRawControllerTransform(hudHand) * Vector3(0.0f, 0.0f, 0.0f);

	// The hand must leave a larger zone before another toggle can register, otherwise
	// tracking jitter on the boundary would toggle repeatedly. 1.5x left only a ~10cm
	// gap at small distances (e.g. matching the flashlight's 0.2m default), which real
	// hand tracking jitter could still cross both ways, causing rapid re-triggering -
	// reported directly, and reproduced by the numbers: 0.2 * 1.5 = 0.3, a 10cm gap.
	const float releaseDistance = toggleDistance * 2.2f;
	const float distanceSqr = (togglePos - handPos).lengthSqr();

	if (!bWasTappingHUD)
	{
		if (distanceSqr < toggleDistance * toggleDistance)
		{
			bWasTappingHUD = true;
			Game::instance.bHideHUD = !Game::instance.bHideHUD;
			PlayHUDToggleSound();
		}
	}
	else if (distanceSqr > releaseDistance * releaseDistance)
	{
		bWasTappingHUD = false;
	}
}

unsigned char InputHandler::UpdateHolsterSwitchWeapons()
{
	IVR* vr = Game::instance.GetVR();

	Matrix4 headTransform = vr->GetHMDTransform();

	// Calculate shoulder holster positions with the correct offset
	Vector3 leftShoulderPos = headTransform * Game::instance.c_LeftShoulderHolsterOffset->Value();
	Vector3 rightShoulderPos = headTransform * Game::instance.c_RightShoulderHolsterOffset->Value();

	Vector3 handPos;
	if (Game::instance.bLeftHanded)
	{
		handPos = vr->GetRawControllerTransform(ControllerRole::Left) * Vector3(0.0f, 0.0f, 0.0f);
	}
	else
	{
		handPos = vr->GetRawControllerTransform(ControllerRole::Right) * Vector3(0.0f, 0.0f, 0.0f);
	}

	if (InputHandler::IsHandInHolster(handPos, leftShoulderPos, Game::instance.c_LeftShoulderHolsterActivationDistance->Value()) 
		|| InputHandler::IsHandInHolster(handPos, rightShoulderPos, Game::instance.c_RightShoulderHolsterActivationDistance->Value()))
	{
		return 127;
	}

	return 0;
}

// Helper function to check if a hand is in a holster
bool InputHandler::IsHandInHolster(const Vector3& handPos, const Vector3& holsterPos, const float& holsterActivationDistance)
{
	return (holsterPos - handPos).lengthSqr() < holsterActivationDistance * holsterActivationDistance;
}

unsigned char InputHandler::UpdateMelee()
{
	IVR* vr = Game::instance.GetVR();
	if (!vr)
	{
		return 0;
	}

	const float leftThreshold = Game::instance.c_LeftHandMeleeSwingSpeed->Value();
	const float rightThreshold = Game::instance.c_RightHandMeleeSwingSpeed->Value();

	Vector3 leftVel = vr->GetControllerVelocity(ControllerRole::Left) * Game::instance.WorldToMetres(1.0f);
	Vector3 rightVel = vr->GetControllerVelocity(ControllerRole::Right) * Game::instance.WorldToMetres(1.0f);

	const bool bLeftSwing = leftThreshold > 0.0f && abs(leftVel.z) > leftThreshold;
	const bool bRightSwing = rightThreshold > 0.0f && abs(rightVel.z) > rightThreshold;

	if (bLeftSwing || bRightSwing)
	{
		// Both hands swinging: use the faster vertical swing so a wild
		// off-hand flick does not steal a stronger punch.
		if (bLeftSwing && bRightSwing)
		{
			BeginMeleeAimOverride(abs(leftVel.z) >= abs(rightVel.z) ? ControllerRole::Left : ControllerRole::Right);
		}
		else
		{
			BeginMeleeAimOverride(bLeftSwing ? ControllerRole::Left : ControllerRole::Right);
		}
		return 127;
	}

	// Button melee (SteamVR binding, unset by default): still aim from the
	// weapon hand rather than the headset. Grenade punch stays off-hand only.
	const bool bMeleeButton = vr->GetBoolInput(Melee);
	if (bMeleeButton && !bWasMeleeButton)
	{
		BeginMeleeAimOverride(GetWeaponHand());
	}
	bWasMeleeButton = bMeleeButton;

	return 0;
}

ControllerRole InputHandler::GetWeaponHand() const
{
	return Game::instance.bLeftHanded ? ControllerRole::Left : ControllerRole::Right;
}

ControllerRole InputHandler::GetOffHand() const
{
	return Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
}

void InputHandler::BeginMeleeAimOverride(ControllerRole hand)
{
	meleeAimHand = hand;

	// Re-evaluated on every swing, never left over from a previous one: a
	// weapon-hand swing or a swing with no grenade held actively disarms.
	bGrenadePunchArmed = hand == GetOffHand()
		&& Game::instance.c_GrenadePunch && Game::instance.c_GrenadePunch->Value()
		&& Game::instance.c_ThrowGrenadeOnRelease && Game::instance.c_ThrowGrenadeOnRelease->Value()
		&& !bGrenadePunchSpentThisHold
		&& IsGrenadeHeld();
	// CE melee damage lands a few ticks in; give the arm the same window as
	// the aim override, then it expires on its own.
	grenadePunchArmTimer = bGrenadePunchArmed ? 0.7f : 0.0f;

	if (Game::instance.c_MeleeFromHand && !Game::instance.c_MeleeFromHand->Value())
	{
		return;
	}

	// CE melee damage lands a few ticks into the animation. Keep the
	// hand-aim override up long enough to cover that window.
	meleeAimOverrideTimer = 0.7f;
	bHasMeleeLockedDir = ComputeMeleeAimDirection(hand, meleeLockedDir);
}

bool InputHandler::IsMeleeAimOverrideActive() const
{
	if (meleeAimOverrideTimer <= 0.0f)
	{
		return false;
	}
	if (Game::instance.c_MeleeFromHand && !Game::instance.c_MeleeFromHand->Value())
	{
		return false;
	}
	return true;
}

Vector3 InputHandler::GetHandWorldPosition(ControllerRole hand) const
{
	IVR* vr = Game::instance.GetVR();
	if (!vr)
	{
		return Helpers::GetCamera().position;
	}
	const Vector3 hmd = vr->GetHMDTransform(true) * Vector3(0.0f, 0.0f, 0.0f);
	const Vector3 handPos = vr->GetControllerTransform(hand, true) * Vector3(0.0f, 0.0f, 0.0f);
	return Helpers::GetCamera().position + (handPos - hmd) * Game::instance.MetresToWorld(1.0f);
}

bool InputHandler::FindMeleeTarget(const Vector3& origin, const Vector3& handWorld, Vector3& outDir) const
{
	HaloID localID;
	if (!Helpers::GetLocalPlayerID(localID))
	{
		return false;
	}

	ObjectTable& table = Helpers::GetObjectTable();
	const float handRadius = Game::instance.MetresToWorld(1.8f);
	const float playerRadius = Game::instance.MetresToWorld(2.6f);
	const float handRadiusSqr = handRadius * handRadius;
	const float playerRadiusSqr = playerRadius * playerRadius;

	Vector3 towardHand = handWorld - origin;
	towardHand.z = 0.0f;
	const bool bHasHandSide = towardHand.lengthSqr() > 1e-6f;
	if (bHasHandSide)
	{
		towardHand.normalize();
	}

	float bestHandDistSqr = handRadiusSqr;
	float bestPlayerDistSqr = playerRadiusSqr;
	const BaseDynamicObject* bestNearHand = nullptr;
	const BaseDynamicObject* bestNearPlayer = nullptr;

	const uint16_t count = table.currentSize;
	for (uint16_t i = 0; i < count; ++i)
	{
		if (table.elements[i].id == 0)
		{
			continue;
		}
		BaseDynamicObject* obj = table.elements[i].dynamicObject;
		if (!obj)
		{
			continue;
		}
		if (obj->N0000027E != ObjectType::BIPED && obj->N0000027E != ObjectType::VEHICLE)
		{
			continue;
		}
		if (i == localID.index)
		{
			continue;
		}
		if (obj->health <= 0.0f && obj->shield <= 0.0f)
		{
			continue;
		}

		Vector3 pos = obj->centre;
		if (pos.lengthSqr() < 1e-8f)
		{
			pos = obj->position;
		}

		const float distHandSqr = (pos - handWorld).lengthSqr();
		if (distHandSqr < bestHandDistSqr)
		{
			bestHandDistSqr = distHandSqr;
			bestNearHand = obj;
		}

		const float distPlayerSqr = (pos - origin).lengthSqr();
		if (distPlayerSqr < bestPlayerDistSqr)
		{
			if (bHasHandSide)
			{
				Vector3 toObj = pos - origin;
				toObj.z = 0.0f;
				if (toObj.lengthSqr() > 1e-8f)
				{
					toObj.normalize();
					if (toObj.dot(towardHand) < 0.15f)
					{
						continue;
					}
				}
			}
			bestPlayerDistSqr = distPlayerSqr;
			bestNearPlayer = obj;
		}
	}

	const BaseDynamicObject* target = bestNearHand ? bestNearHand : bestNearPlayer;
	if (!target)
	{
		return false;
	}

	Vector3 pos = target->centre;
	if (pos.lengthSqr() < 1e-8f)
	{
		pos = target->position;
	}
	outDir = pos - origin;
	if (outDir.lengthSqr() < 1e-8f)
	{
		return false;
	}
	outDir.normalize();
	return true;
}

bool InputHandler::ComputeMeleeAimDirection(ControllerRole hand, Vector3& outDir) const
{
	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetLocalPlayer());
	if (!player)
	{
		return false;
	}

	const Vector3 origin = player->centre.lengthSqr() > 1e-8f ? player->centre : player->position;
	const Vector3 handWorld = GetHandWorldPosition(hand);

	if (FindMeleeTarget(origin, handWorld, outDir))
	{
		return true;
	}

	// No one near the fist: aim the cone toward where the hand actually is
	// (a downward bash still points sideways if the hand is to your side).
	Vector3 toHand = handWorld - origin;
	toHand.z = 0.0f;
	if (toHand.lengthSqr() > 1e-6f)
	{
		toHand.normalize();
		outDir = toHand;
		return true;
	}

	IVR* vr = Game::instance.GetVR();
	if (vr && vr->TryGetControllerFacing(hand, outDir) && outDir.lengthSqr() > 1e-6f)
	{
		outDir.normalize();
		return true;
	}

	if (vr)
	{
		outDir = vr->GetControllerTransform(hand, true).getLeftAxis();
		if (outDir.lengthSqr() > 1e-6f)
		{
			outDir.normalize();
			return true;
		}
	}

	return false;
}

void InputHandler::SnapshotGrenadePunchTargets(HaloID localID)
{
	grenadePunchSnapCount = 0;

	ObjectTable& table = Helpers::GetObjectTable();
	if (!table.elements)
	{
		return;
	}

	const Vector3 fist = GetHandWorldPosition(GetOffHand());
	const float rangeSqr = Game::instance.MetresToWorld(3.5f) * Game::instance.MetresToWorld(3.5f);
	const uint16_t count = table.currentSize;
	for (uint16_t i = 0; i < count && grenadePunchSnapCount < kMaxGrenadePunchSnaps; ++i)
	{
		if (i == localID.index)
		{
			continue;
		}
		// id == 0 marks a free slot; its dynamicObject pointer may be stale.
		if (table.elements[i].id == 0)
		{
			continue;
		}
		BaseDynamicObject* obj = table.elements[i].dynamicObject;
		if (!obj)
		{
			continue;
		}
		if (obj->N0000027E != ObjectType::BIPED && obj->N0000027E != ObjectType::VEHICLE)
		{
			continue;
		}

		Vector3 pos = obj->centre;
		if (pos.lengthSqr() < 1e-8f)
		{
			pos = obj->position;
		}
		if ((pos - fist).lengthSqr() > rangeSqr)
		{
			continue;
		}

		GrenadePunchHpSnap& snap = grenadePunchSnaps[grenadePunchSnapCount++];
		snap.index = i;
		snap.datumId = table.elements[i].id;
		snap.health = obj->health;
		snap.shield = obj->shield;
		snap.isVehicle = obj->N0000027E == ObjectType::VEHICLE;
		snap.distSqr = (pos - fist).lengthSqr();
	}
}

bool InputHandler::GrenadePunchHitValidTarget() const
{
	if (grenadePunchSnapCount <= 0)
	{
		return false;
	}

	ObjectTable& table = Helpers::GetObjectTable();
	if (!table.elements)
	{
		return false;
	}

	// Vehicle hulls are big; their centre sits well back from the panel you
	// actually punched, so allow a generous reach for them.
	const float vehicleReach = Game::instance.MetresToWorld(2.5f);
	const float vehicleReachSqr = vehicleReach * vehicleReach;

	for (int s = 0; s < grenadePunchSnapCount; ++s)
	{
		const GrenadePunchHpSnap& snap = grenadePunchSnaps[s];
		if (snap.index >= table.currentSize)
		{
			continue;
		}
		if (snap.datumId == 0 || table.elements[snap.index].id != snap.datumId)
		{
			continue;
		}
		BaseDynamicObject* obj = table.elements[snap.index].dynamicObject;
		if (!obj)
		{
			continue;
		}
		// Vehicles often take no melee damage at all, so a hull we were close
		// enough to punch counts as a hit whether or not its health moved.
		if (snap.isVehicle && snap.distSqr <= vehicleReachSqr)
		{
			return true;
		}
		if (obj->health + 0.001f < snap.health || obj->shield + 0.001f < snap.shield)
		{
			return true;
		}
		if (snap.health > 0.0f && obj->health <= 0.0f)
		{
			return true;
		}
	}

	return false;
}

void InputHandler::PreMeleeDamage(HaloID& unitID)
{
	bMeleeDamageOverridden = false;

	HaloID localID;
	if (!Helpers::GetLocalPlayerID(localID) || localID.index != unitID.index)
	{
		return;
	}

	if (bGrenadePunchArmed && meleeAimHand == GetOffHand())
	{
		SnapshotGrenadePunchTargets(localID);
	}
	else
	{
		grenadePunchSnapCount = 0;
	}

	if (!IsMeleeAimOverrideActive())
	{
		return;
	}

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(unitID));
	if (!player)
	{
		return;
	}

	Vector3 dir;
	if (ComputeMeleeAimDirection(meleeAimHand, dir))
	{
		meleeLockedDir = dir;
		bHasMeleeLockedDir = true;
	}
	else if (!bHasMeleeLockedDir)
	{
		return;
	}

	savedMeleeFacingDir = player->facingDir;
	savedMeleeFacing = player->facing;
	savedMeleeDesiredAim = player->desiredAim;
	savedMeleeAim = player->aim;
	savedMeleeAim2 = player->aim2;
	savedMeleeAim3 = player->aim3;
	savedMeleeCamLook = Helpers::GetCamera().lookDir;
	savedMeleePlayerLook = Helpers::GetPlayer().lookDir;
	bMeleeDamageOverridden = true;

	// This function reads unit+0x23C (aim) immediately. facingDir is
	// also sampled later in the same call for the damage volume.
	player->facingDir = meleeLockedDir;
	player->facing = meleeLockedDir;
	player->desiredAim = meleeLockedDir;
	player->aim = meleeLockedDir;
	player->aimVelocity = Vector3(0.0f, 0.0f, 0.0f);
	player->aim2 = meleeLockedDir;
	player->aim3 = meleeLockedDir;
	Helpers::GetCamera().lookDir = meleeLockedDir;
	Helpers::GetPlayer().lookDir = meleeLockedDir;
}

void InputHandler::TryGrenadePunch(HaloID& unitID)
{
	// Spawning and detonating from inside the engine's melee damage call can
	// re-enter that same call through the blast's own damage. Never nest.
	if (bInGrenadePunch)
	{
		return;
	}
	ReentryGuard guard(bInGrenadePunch);

	static int s_logged = 0;
	auto logOnce = [&](const char* why)
	{
		if (s_logged < 12)
		{
			Logger::log << "[GrenadePunch] skip: " << why << std::endl;
			s_logged++;
		}
	};

	if (!Game::instance.c_GrenadePunch || !Game::instance.c_GrenadePunch->Value())
	{
		logOnce("GrenadePunch config off");
		return;
	}
	if (!Game::instance.c_ThrowGrenadeOnRelease || !Game::instance.c_ThrowGrenadeOnRelease->Value())
	{
		logOnce("ThrowGrenadeOnRelease off");
		return;
	}
	if (!bGrenadePunchArmed || grenadePunchArmTimer <= 0.0f)
	{
		logOnce("swing was not armed (grenade not held on an off-hand swing)");
		return;
	}
	if (meleeAimHand != GetOffHand())
	{
		logOnce("weapon-hand melee");
		return;
	}

	HaloID localID;
	if (!Helpers::GetLocalPlayerID(localID) || localID.index != unitID.index)
	{
		return;
	}

	if (!GrenadePunchHitValidTarget())
	{
		logOnce("no character or vehicle hit");
		return;
	}

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(unitID));
	if (!player)
	{
		logOnce("no player object");
		return;
	}

	if (player->parent.index != 0xFFFF)
	{
		BaseDynamicObject* parentObj = Helpers::GetDynamicObject(player->parent);
		if (parentObj)
		{
			logOnce("in vehicle/parented");
			return;
		}
	}

	int grenadeType = player->currentGrenadeIndex;
	if (grenadeType == 1 && player->plasmaGrenadeCount > 0)
	{
		player->plasmaGrenadeCount--;
	}
	else if (player->fragGrenadeCount > 0)
	{
		grenadeType = 0;
		player->fragGrenadeCount--;
	}
	else if (player->plasmaGrenadeCount > 0)
	{
		grenadeType = 1;
		player->plasmaGrenadeCount--;
	}
	else
	{
		return;
	}

	HaloID grenadeTag;
	if (!Helpers::FindGrenadeProjectileTag(grenadeType, grenadeTag))
	{
		if (grenadeType == 1)
		{
			player->plasmaGrenadeCount++;
		}
		else
		{
			player->fragGrenadeCount++;
		}
		logOnce("no grenade projectile tag");
		return;
	}

	ControllerRole fist = GetOffHand();
	const Vector3 origin = GetHandWorldPosition(fist);
	Vector3 blastPos = origin;
	if (bHasMeleeLockedDir)
	{
		blastPos = origin + meleeLockedDir * Game::instance.MetresToWorld(0.35f);
	}

	// Detonating below the contact point makes Halo's radial impulse push the
	// target up and away, instead of driving it straight into the floor.
	float blastDrop = Game::instance.c_GrenadePunchBlastDrop ? Game::instance.c_GrenadePunchBlastDrop->Value() : 0.6f;
	blastDrop = (std::max)(0.0f, (std::min)(blastDrop, 3.0f));
	blastPos.z -= Game::instance.MetresToWorld(blastDrop);

	// Never drop the blast below the ground you are standing on. A projectile
	// spawned inside BSP geometry is a reliable way to crash the collision code.
	const float floorZ = player->position.z + Game::instance.MetresToWorld(0.15f);
	if (blastPos.z < floorZ)
	{
		blastPos.z = floorZ;
	}

	float power = Game::instance.c_GrenadePunchPower ? Game::instance.c_GrenadePunchPower->Value() : 1.0f;
	int blastCount = static_cast<int>(power + 0.5f);
	blastCount = (std::max)(1, (std::min)(blastCount, kMaxGrenadePunchSpawns));

	HaloID noParent;
	noParent.index = 0xFFFF;
	noParent.id = 0xFFFF;

	grenadePunchSpawnCount = 0;
	for (int i = 0; i < blastCount; ++i)
	{
		// Stack the extra blasts a few centimetres apart so the engine treats
		// them as separate projectiles rather than collapsing them.
		Vector3 spawnPos = blastPos;
		spawnPos.z += Game::instance.MetresToWorld(0.08f * i);

		Logger::log << "[GrenadePunch] spawning " << (i + 1) << "/" << blastCount
			<< " type=" << grenadeType << std::endl;
		HaloID spawned = Helpers::SpawnObject(grenadeTag, spawnPos, noParent);
		BaseDynamicObject* grenade = Helpers::GetDynamicObject(spawned);
		if (!grenade)
		{
			continue;
		}

		uint8_t* raw = reinterpret_cast<uint8_t*>(grenade);
		*reinterpret_cast<HaloID*>(raw + 0xC4) = unitID;
		*reinterpret_cast<HaloID*>(raw + 0x234) = unitID;
		Helpers::ArmProjectileDetonation(grenade);
		grenadePunchSpawnedIDs[grenadePunchSpawnCount++] = spawned;
	}

	if (grenadePunchSpawnCount == 0)
	{
		if (grenadeType == 1)
		{
			player->plasmaGrenadeCount++;
		}
		else
		{
			player->fragGrenadeCount++;
		}
		logOnce("spawn object failed");
		return;
	}

	grenadePunchArmFrames = 6;

	Logger::log << "[GrenadePunch] spawned type=" << grenadeType << " tag=" << grenadeTag
		<< " count=" << grenadePunchSpawnCount << " drop=" << blastDrop << std::endl;

	grenadePunchSavedDamageFlags = player->N00000311;
	grenadePunchSavedHealth = player->health;
	grenadePunchSavedShield = player->shield;
	grenadePunchSavedVelocity = player->velocity;
	grenadePunchPlayerID = unitID;
	player->N00000311 = static_cast<uint16_t>(grenadePunchSavedDamageFlags | (1u << 11));
	grenadePunchInvulnFrames = 24;

	bGrenadePunchUsedThisHold = true;
	bGrenadePunchSpentThisHold = true;
	bGrenadePunchArmed = false;
	grenadePunchArmTimer = 0.0f;
	grenadeThrowPulseFrames = 0;

	IVR* vr = Game::instance.GetVR();
	if (vr)
	{
		vr->TriggerHapticVibration(fist, 0.0f, 0.22f, 90.0f, 1.0f);
		vr->TriggerHapticPulse(fist, 3000);
	}
}

void InputHandler::PostMeleeDamage(HaloID& unitID)
{
	TryGrenadePunch(unitID);

	if (!bMeleeDamageOverridden)
	{
		return;
	}

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetDynamicObject(unitID));
	if (player)
	{
		player->facingDir = savedMeleeFacingDir;
		player->facing = savedMeleeFacing;
		player->desiredAim = savedMeleeDesiredAim;
		player->aim = savedMeleeAim;
		player->aim2 = savedMeleeAim2;
		player->aim3 = savedMeleeAim3;
	}

	Helpers::GetCamera().lookDir = savedMeleeCamLook;
	Helpers::GetPlayer().lookDir = savedMeleePlayerLook;
	bMeleeDamageOverridden = false;
}

unsigned char InputHandler::UpdateCrouch()
{
	IVR* vr = Game::instance.GetVR();

	Vector3 headPos = vr->GetHMDTransform() * Vector3(0.0f, 0.0f, 0.0f);

	float crouchHeight = Game::instance.c_CrouchHeight->Value();

	if (crouchHeight < 0)
	{
		return 0;
	}

	if (headPos.z < -crouchHeight)
	{
		return 127;
	}

	return 0;
}

void InputHandler::SetMousePosition(int& x, int& y)
{
	Vector2 mousePos = Game::instance.GetVR()->GetMousePos();
	// Best I can tell the mouse is always scaled to a 640x480 canvas
	x = static_cast<int>(mousePos.x * 640);
	y = static_cast<int>(mousePos.y * 480);
}

void InputHandler::UpdateMouseInfo(MouseInfo* mouseInfo)
{
	// State is advanced once per frame by UpdateVirtualMouseButtons; this only
	// stamps the result over whatever the real device reported, and only while
	// the VR trigger is actually involved - otherwise a physical mouse click
	// would be overwritten with zero and never register.
	if (bMouseWasDown || mouseDownState || mouseReleaseEdge)
	{
		mouseInfo->buttonState[0] = mouseDownState;
		mouseInfo->buttonState2[0] = mouseReleaseEdge;
	}
}

void InputHandler::NotifyMenuVisible(bool bVisible)
{
	if (!bVisible)
	{
		return;
	}

	if (!bGameplayInputLatched)
	{
		Logger::log << "[Menu] Suppressing gameplay inputs" << std::endl;
	}
	bGameplayInputLatched = true;
	// While the menu owns the overlay, SteamVR routes the trigger to it and the
	// game's own Fire action reads as released. Without this window the latch
	// sees "nothing held" on the frame the menu closes, clears itself, and the
	// still-held trigger fires on the very next frame - the rocket into the
	// wall on Resume.
	gameplayInputGraceFrames = 20;
}

bool InputHandler::ShouldSuppressGameplayInputs()
{
	IVR* vr = Game::instance.GetVR();
	if (!vr)
	{
		return false;
	}

	if (Helpers::IsMouseVisible())
	{
		NotifyMenuVisible(true);
		return true;
	}

	// The menu has closed, but the press that closed it is usually still held.
	// Keep swallowing until every gameplay button has actually been let go,
	// and never trust a release seen inside the grace window.
	if (bGameplayInputLatched)
	{
		if (gameplayInputGraceFrames > 0)
		{
			gameplayInputGraceFrames--;
			return true;
		}

		const bool bStillHeld = vr->GetBoolInput(Fire) || vr->GetBoolInput(Jump)
			|| vr->GetBoolInput(Melee) || vr->GetBoolInput(Grenade)
			|| vr->GetBoolInput(Interact) || vr->GetBoolInput(Reload)
			|| vr->GetBoolInput(Crouch) || vr->GetBoolInput(Flashlight)
			|| vr->GetBoolInput(SwitchWeapons) || vr->GetBoolInput(SwitchGrenades);

		if (bStillHeld)
		{
			return true;
		}

		bGameplayInputLatched = false;
		Logger::log << "[Menu] Gameplay inputs restored" << std::endl;
	}

	return false;
}

void InputHandler::UpdateVirtualMouseButtons()
{
	IVR* vr = Game::instance.GetVR();
	if (!vr)
	{
		return;
	}

	// Present the trigger to Halo as a quick click rather than a held button.
	// Halo's converter at halo+0x91BC0 counts frames held, and the UI acts on
	// any non-zero value, so holding the trigger dispatches an activation every
	// frame - which restarts the menu sound before it can be heard. Pulsing for
	// a single frame matches what a real mouse click looks like.
	const bool bDown = vr->GetMouseDown();
	mouseDownState = (bDown && !bMouseWasDown) ? 1 : 0;
	mouseReleaseEdge = (!bDown && bMouseWasDown) ? 1 : 0;
	const bool bWasDown = bMouseWasDown;
	bMouseWasDown = bDown;

	// Say nothing unless this is our click, so a physical mouse still works
	// normally when one is awake.
	if (!bDown && !bWasDown)
	{
		return;
	}

	// Halo only calls its UpdateMouseInfo (and so our hook) from a block gated
	// on the DirectInput mouse device being non-null. A sleeping Bluetooth
	// mouse nulls that pointer, the whole block is skipped, and menu clicks
	// stop working even though the VR cursor still moves. Writing the state
	// straight into the mouse block keeps menus usable with no mouse present.
	MouseInfo* mouseInfo = Helpers::GetMouseInfo();
	if (mouseInfo)
	{
		mouseInfo->buttonState[0] = mouseDownState;
		mouseInfo->buttonState2[0] = mouseReleaseEdge;
	}
}

bool InputHandler::GetCalculatedHandPositions(Matrix4& controllerTransform, Vector3& dominantHandPos, Vector3& offHand)
{
	ControllerRole dominant = Game::instance.bLeftHanded ? ControllerRole::Left : ControllerRole::Right;
	ControllerRole nonDominant = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;

	controllerTransform = Game::instance.GetVR()->GetControllerTransform(dominant, true);

	Vector3 poseDirection;
	bool bHasPoseData = Game::instance.GetVR()->TryGetControllerFacing(dominant, poseDirection);

	// When 2h aiming point the main hand at the offhand 
	if (Game::instance.bUseTwoHandAim || bHasPoseData)
	{
		Matrix4 aimingTransform = Game::instance.GetVR()->GetRawControllerTransform(dominant, true);
		Matrix4 offHandTransform = Game::instance.GetVR()->GetRawControllerTransform(nonDominant, true);

		const Vector3 actualControllerPos = controllerTransform * Vector3(0.0f, 0.0f, 0.0f);
		const Vector3 mainHandPos = aimingTransform * Vector3(0.0f, 0.0f, 0.0f);
		const Vector3 offHandPos = Game::instance.bUseTwoHandAim ? offHandTransform * Vector3(0.0f, 0.0f, 0.0f) : mainHandPos + poseDirection;
		Vector3 toOffHand = (offHandPos - mainHandPos);

		// Avoid NaN errors
		if (toOffHand.lengthSqr() < 1e-8)
		{
			return false;
		}

		toOffHand.normalize();

		// Blend the pure hand-line direction toward the controller's own facing.
		// Hand-line alone ignores wrist pitch entirely: as the wrist tilts during
		// normal movement the aim can swing independently of where the controller
		// is actually pointing, worst on weapons where the hands sit furthest apart
		// (confirmed via logging: aim pitch showed near-zero correlation with
		// controller facing during a spin test on the rocket launcher). Blending in
		// facing lets wrist pitch contribute while the off hand still steers.
		const float facingBlend = Game::instance.c_TwoHandFacingBlend->Value();
		// Scoped to the rocket launcher only. The other three two-handed weapons
		// are already well corrected by their fixed pitch/yaw offsets alone, so
		// this should not affect them regardless of what it is set to.
		if (Game::instance.bUseTwoHandAim && bHasPoseData && facingBlend > 0.0f
			&& Game::instance.GetCurrentWeaponType() == WeaponType::RocketLauncher)
		{
			Vector3 blended = toOffHand * (1.0f - facingBlend) + poseDirection * facingBlend;
			if (blended.lengthSqr() > 1e-8f)
			{
				blended.normalize();
				toOffHand = blended;
			}
		}

#define TWOHAND_SPIN_DEBUG 0
#if TWOHAND_SPIN_DEBUG
		// Continuous log while two-hand aiming, throttled, so a spin can be traced.
		// Captures the raw inputs the aim is built from, not just the result, so we
		// can tell whether the aim is tracking the hands faithfully (and the hands
		// are moving) or whether the aim is diverging from them.
		if (Game::instance.bUseTwoHandAim)
		{
			static std::chrono::steady_clock::time_point lastSpinLog;
			auto nowT = std::chrono::steady_clock::now();
			if (std::chrono::duration<double>(nowT - lastSpinLog).count() > 0.1)
			{
				lastSpinLog = nowT;
				const float r2d = 57.2957795f;
				const Vector3 handSep = offHandPos - mainHandPos;
				Vector3 facing = poseDirection;
				if (facing.lengthSqr() > 1e-8f) { facing.normalize(); }
				Logger::log << "[SpinDbg]"
					<< " aimPitch=" << asin(toOffHand.z) * r2d
					<< " aimYaw=" << atan2(toOffHand.y, toOffHand.x) * r2d
					<< " facePitch=" << asin(facing.z) * r2d
					<< " faceYaw=" << atan2(facing.y, facing.x) * r2d
					<< " sep=" << handSep.length()
					<< " main=(" << mainHandPos.x << "," << mainHandPos.y << "," << mainHandPos.z << ")"
					<< " off=(" << offHandPos.x << "," << offHandPos.y << "," << offHandPos.z << ")"
					<< std::endl;
			}
		}
#endif
		if (Game::instance.bUseTwoHandAim)
		{
			FloatProperty* pitchProp = nullptr;
			FloatProperty* yawProp = nullptr;

			switch (Game::instance.GetCurrentWeaponType())
			{
			case WeaponType::AssaultRifle:
				pitchProp = Game::instance.c_TwoHandPitchOffsetAssaultRifle;
				yawProp = Game::instance.c_TwoHandYawOffsetAssaultRifle;
				break;
			case WeaponType::Shotgun:
				pitchProp = Game::instance.c_TwoHandPitchOffsetShotgun;
				yawProp = Game::instance.c_TwoHandYawOffsetShotgun;
				break;
			case WeaponType::Sniper:
				pitchProp = Game::instance.c_TwoHandPitchOffsetSniper;
				yawProp = Game::instance.c_TwoHandYawOffsetSniper;
				break;
			case WeaponType::RocketLauncher:
				pitchProp = Game::instance.c_TwoHandPitchOffsetRocket;
				yawProp = Game::instance.c_TwoHandYawOffsetRocket;
				break;
			default:
				break;
			}

			if (pitchProp && yawProp)
			{
				float pitchAdjust = pitchProp->Value();
				float yawAdjust = yawProp->Value();

				if (Game::instance.bLeftHanded)
				{
					yawAdjust = -yawAdjust;
				}

				const float degToRad = 0.0174532925f;

				// Rocket yaw offset is ~13°, far larger than the other two-handers.
				// In the hand frame that correction rolls with the wrist, so a
				// turn-in-place with locked arms swings the tube around. World-up
				// keeps the same heading bias without that orbit.
				const bool bWorldFrame = Game::instance.c_TwoHandRollStabilised->Value()
					|| Game::instance.GetCurrentWeaponType() == WeaponType::RocketLauncher;

				if (bWorldFrame)
				{
					// World-referenced: pitch is elevation above horizontal, yaw is a compass
					// heading. Z is up in this space (the SteamVR conversion maps its Y into Z,
					// and yaw is a rotateZ). Nothing here reads the controller's roll, so the
					// correction cannot swing as the wrist rolls.
					const float pitch = asin(toOffHand.z) + pitchAdjust * degToRad;
					const float yaw = atan2(toOffHand.y, toOffHand.x) + yawAdjust * degToRad;
					const float cp = cos(pitch);
					toOffHand = Vector3(cp * cos(yaw), cp * sin(yaw), sin(pitch));
					toOffHand.normalize();
				}
				else
				{
					// Original hand-referenced frame: holds regardless of facing, but rolls
					// with the wrist, so large corrections are unstable.
					const Vector3 handFwd = aimingTransform.getLeftAxis();
					const Vector3 handUp = aimingTransform.getUpAxis();
					const Vector3 handRight = handUp.cross(handFwd);

					const float lx = toOffHand.dot(handFwd);
					const float ly = toOffHand.dot(handRight);
					const float lz = toOffHand.dot(handUp);

					const float pitch = asin(lz) + pitchAdjust * degToRad;
					const float yaw = atan2(ly, lx) + yawAdjust * degToRad;

					const float cp = cos(pitch);
					toOffHand = handFwd * (cp * cos(yaw)) + handRight * (cp * sin(yaw)) + handUp * sin(pitch);
					toOffHand.normalize();
				}
			}
		}
#define TWOHAND_CALIBRATION 0
#if TWOHAND_CALIBRATION
		// Calibration: on the frame two-hand grip engages, compare the aim the
		// weapon HAD (the controller's own tip facing, which is what one-handed
		// aiming uses and is correct) against the aim it is ABOUT to take (the
		// straight line between the two hands). The difference is exactly the
		// correction that weapon needs.
		//
		// Both are expressed in the dominant hand's own frame rather than world
		// space, so the numbers hold regardless of which way the player happens
		// to be facing when calibrating.
		{
			static bool bWasTwoHanding = false;
			const bool bNowTwoHanding = Game::instance.bUseTwoHandAim;

			if (bNowTwoHanding && !bWasTwoHanding && bHasPoseData)
			{
				Vector3 correctAim = poseDirection;
				correctAim.normalize();

				// Dominant hand's own axes, to convert both vectors out of world space
				const Vector3 handFwd = aimingTransform.getLeftAxis();
				const Vector3 handUp = aimingTransform.getUpAxis();
				const Vector3 handRight = handUp.cross(handFwd);

				auto toLocal = [&](const Vector3& v) {
					if (Game::instance.c_TwoHandRollStabilised->Value())
					{
						return v; // already world-referenced; Z is up
					}
					return Vector3(v.dot(handFwd), v.dot(handRight), v.dot(handUp));
				};
				const Vector3 correctLocal = toLocal(correctAim);
				const Vector3 gripLocal = toLocal(toOffHand);

				const float radToDeg = 57.2957795f;
				auto pitchOf = [&](const Vector3& v) { return asin(v.z) * radToDeg; };
				auto yawOf = [&](const Vector3& v) { return atan2(v.y, v.x) * radToDeg; };

				const char* names[] = { "Unknown", "Pistol", "AssaultRifle", "Shotgun",
					"RocketLauncher", "Sniper", "Flamethrower", "PlasmaPistol",
					"PlasmaRifle", "PlasmaCannon", "Needler", "FuelRod" };
				const int typeIdx = static_cast<int>(Game::instance.GetCurrentWeaponType());
				const char* weaponName = (typeIdx >= 0 && typeIdx < 12) ? names[typeIdx] : "OutOfRange";

				Logger::log << "[TwoHandCal] weapon=" << weaponName
					<< " pitchDelta=" << (pitchOf(correctLocal) - pitchOf(gripLocal))
					<< " yawDelta=" << (yawOf(correctLocal) - yawOf(gripLocal))
					<< "  (correct pitch=" << pitchOf(correctLocal) << " yaw=" << yawOf(correctLocal)
					<< " | grip pitch=" << pitchOf(gripLocal) << " yaw=" << yawOf(gripLocal) << ")"
					<< std::endl;
			}
			bWasTwoHanding = bNowTwoHanding;
		}
#endif

		dominantHandPos = actualControllerPos; 
		offHand = toOffHand; 

		return true; 
	}

	return false; 
}

void InputHandler::CalculateSmoothedInput()
{
	Matrix4 controllerTransform;
	Vector3 actualControllerPos;
	Vector3 toOffHand;

	if (!GetCalculatedHandPositions(controllerTransform, actualControllerPos, toOffHand))
	{
		return;
	}

	if (!bLastYawInitialized)
	{
		lastSmoothingYawOffset = Game::instance.GetVR()->GetYawOffset();
		bLastYawInitialized = true;
	}

	// Detect snap turns and rotate smoothed position to prevent lerping artifact
	float currentYawOffset = Game::instance.GetVR()->GetYawOffset();
	float yawDelta = currentYawOffset - lastSmoothingYawOffset;

	// Detect snap turns and rotate smoothed position
	Helpers::RotateForSnapTurn(smoothedPosition, yawDelta, Game::instance.c_SnapTurnAmount->Value());

	// Update tracked yaw offset for next frame
	lastSmoothingYawOffset = currentYawOffset;

	float userInput = 0.0f;
	short zoom = Helpers::GetInputData().zoomLevel;

	if (zoom == -1)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountNoZoom->Value();
	}
	else if (zoom == 0)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountOneZoom->Value();
	}
	else if (zoom == 1)
	{
		userInput = Game::instance.c_WeaponSmoothingAmountTwoZoom->Value();
	}

	float clampedValue = std::clamp(userInput, 0.0f, 2.0f);
	if (clampedValue == 0.0f)
	{
		smoothedPosition = actualControllerPos + toOffHand;
	}
	else
	{
		const float scaleFactor = (-20.0f / 9.0f);

		float h = 90.0f * log2(1.0f - exp(clampedValue * scaleFactor));

		float t = 1.0f - pow(2.0f, Game::instance.lastDeltaTime * h);

		smoothedPosition = Helpers::Lerp(smoothedPosition, actualControllerPos + toOffHand, t);
	}
}

void InputHandler::UpdateHandsProximity()
{
	float swapHandDistance = Game::instance.c_SwapHandDistance->Value();
	
	const Vector3 leftPos = Game::instance.GetVR()->GetControllerTransform(ControllerRole::Left, true) * Vector3(0.0f, 0.0f, 0.0f);
	const Vector3 rightPos = Game::instance.GetVR()->GetControllerTransform(ControllerRole::Right, true) * Vector3(0.0f, 0.0f, 0.0f);
	float handDistance = (rightPos - leftPos).lengthSqr();

	bool handsWithinSwapWeaponDistance = false;
	if (swapHandDistance >= 0.0f && handDistance < swapHandDistance * swapHandDistance)
	{
		handsWithinSwapWeaponDistance = true;
		CheckSwapWeaponHand();
	}

	UpdateTwoHandedHold(handDistance, handsWithinSwapWeaponDistance);
}

void InputHandler::CheckSwapWeaponHand()
{
	IVR* vr = Game::instance.GetVR();

	bool bWeaponHandChanged;
	bool bOffhandWeaponHandChanged;
	bool bIsSwitchHandsPressed = vr->GetBoolInput(SwapWeaponHand, bWeaponHandChanged);
	bool bIsOffhandSwitchHandsPressed = vr->GetBoolInput(OffhandSwapWeaponHand, bOffhandWeaponHandChanged);

	bool offHandGrabbedWeapon = false;
	bool dominantHandReleasedWeapon = false;

    if (!Game::instance.bLeftHanded)
    {
		offHandGrabbedWeapon = bIsSwitchHandsPressed && bWeaponHandChanged && !bIsOffhandSwitchHandsPressed;
		dominantHandReleasedWeapon = bIsSwitchHandsPressed && !bIsOffhandSwitchHandsPressed && bOffhandWeaponHandChanged;
    }
    else
    {
        offHandGrabbedWeapon = bIsOffhandSwitchHandsPressed && bOffhandWeaponHandChanged && !bIsSwitchHandsPressed;
		dominantHandReleasedWeapon = bIsOffhandSwitchHandsPressed && !bIsSwitchHandsPressed && bWeaponHandChanged;
    }

	if (offHandGrabbedWeapon || dominantHandReleasedWeapon)
    {
		// Enable left handed and update the bindings to use the relevant action set
        Game::instance.bLeftHanded = !Game::instance.bLeftHanded;
		
		UpdateRegisteredInputs();
    }
}

void InputHandler::UpdateTwoHandedHold(float handDistance, bool handsWithinSwapWeaponDistance)
{
	// Two hand aim is disabled when 3DOF is enabled.
	if (Game::instance.bUse3DOFAiming) {
		Game::instance.bUseTwoHandAim = false;
		return;
	}

	// Optionally disable two hand grip for one handed weapons (pistol/plasma
	// pistol), where there is no real two handed hold to switch to.
	if (Game::instance.c_DisableTwoHandForOneHanded->Value()
		&& Game::instance.IsCurrentWeaponOneHanded())
	{
		Game::instance.bUseTwoHandAim = false;
		return;
	}

	IVR* vr = Game::instance.GetVR();

	bool bGripChanged;
	bool bIsGripping = vr->GetBoolInput(TwoHandGrip, bGripChanged);

	if (handsWithinSwapWeaponDistance)
	{
		if (!bIsGripping) {
	        Game::instance.bUseTwoHandAim = false;
	    }
		return;
	}

	if (Game::instance.c_ToggleGrip->Value())
	{
	    if (bGripChanged && bIsGripping)
	    {
	        bWasGripping ^= true;
	    }
	    bIsGripping = bWasGripping;
	}

	float twoHandDistance = Game::instance.c_TwoHandDistance->Value();
	if (twoHandDistance >= 0.0f)
	{
	    if (bGripChanged)
	    {
	        if (bIsGripping)
	        {
				if (handDistance < twoHandDistance * twoHandDistance)
	            {
	                Game::instance.bUseTwoHandAim = true;
	            }
	        }
	        else
	        {
	            Game::instance.bUseTwoHandAim = false;
	        }
	    }
	}
	else
	{
	    Game::instance.bUseTwoHandAim = bIsGripping;
	}
}

#undef ApplyBoolInput
#undef ApplyImpulseBoolInput
#undef RegisterBoolInput
#undef RegisterVector2Input