#include "InputHandler.h"
#include "Game.h"
#include "Helpers/Controls.h"
#include "Helpers/Camera.h"
#include "Helpers/Menus.h"
#include "Helpers/Maths.h"
#include "Logger.h"

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

void InputHandler::UpdateCamera(float& yaw, float& pitch)
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
	// tracking jitter on the boundary would toggle repeatedly
	const float releaseDistance = toggleDistance * 1.5f;
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

	Vector3 handVel = vr->GetControllerVelocity(ControllerRole::Left);

	handVel *= Game::instance.WorldToMetres(1.0f);

	if (Game::instance.c_LeftHandMeleeSwingSpeed->Value() > 0.0f && abs(handVel.z) > Game::instance.c_LeftHandMeleeSwingSpeed->Value())
	{
		return 127;
	}

	handVel = vr->GetControllerVelocity(ControllerRole::Right);

	handVel *= Game::instance.WorldToMetres(1.0f);

	if (Game::instance.c_RightHandMeleeSwingSpeed->Value() > 0.0f && abs(handVel.z) > Game::instance.c_RightHandMeleeSwingSpeed->Value())
	{
		return 127;
	}

    return 0;
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
	if (Game::instance.GetVR()->GetMouseDown())
	{
		if (mouseDownState < 255)
		{
			mouseDownState++;
		}
	}
	else
	{
		mouseDownState = 0;
	}

	mouseInfo->buttonState[0] = mouseDownState;
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

				const Vector3 handFwd = aimingTransform.getLeftAxis();
				const Vector3 handUp = aimingTransform.getUpAxis();
				const Vector3 handRight = handUp.cross(handFwd);

				const float lx = toOffHand.dot(handFwd);
				const float ly = toOffHand.dot(handRight);
				const float lz = toOffHand.dot(handUp);

				const float degToRad = 0.0174532925f;
				const float pitch = asin(lz) + pitchAdjust * degToRad;
				const float yaw = atan2(ly, lx) + yawAdjust * degToRad;

				const float cp = cos(pitch);
				toOffHand = handFwd * (cp * cos(yaw)) + handRight * (cp * sin(yaw)) + handUp * sin(pitch);
				toOffHand.normalize();
			}
		}
#define TWOHAND_CALIBRATION 1
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