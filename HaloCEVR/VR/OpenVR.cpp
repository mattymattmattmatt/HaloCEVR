#include <d3d11.h>
#include <d3d9.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include "OpenVR.h"
#include "../Logger.h"
#include "../Game.h"
#include "../Profiler.h"
#include "../Helpers/DX9.h"
#include "../Helpers/Renderer.h"
#include "../Helpers/RenderTarget.h"
#include "../Helpers/Camera.h"
#include "../Helpers/Cutscene.h"
#include "../Helpers/Menus.h"
#include "../WeaponHapticsConfig.h"

#pragma comment(lib, "openvr_api.lib")
#pragma comment(lib, "d3d11.lib")

void OpenVR::Init()
{
	VR_PROFILE_SCOPE(OpenVR_Init);

	Logger::log << "[OpenVR] Initialising OpenVR" << std::endl;

	vr::EVRInitError initError = vr::VRInitError_None;
	vrSystem = vr::VR_Init(&initError, vr::VRApplication_Scene);

	if (initError != vr::VRInitError_None)
	{
		Logger::err << "[OpenVR] VR_Init failed: " << vr::VR_GetVRInitErrorAsEnglishDescription(initError) << std::endl;
		return;
	}

	vrCompositor = vr::VRCompositor();

	if (!vrCompositor)
	{
		Logger::err << "[OpenVR] VRCompositor failed" << std::endl;
		return;
	}

	vrOverlay = vr::VROverlay();

	if (!vrOverlay)
	{
		Logger::err << "[OpenVR] VROverlay failed" << std::endl;
		return;
	}

	vrInput = vr::VRInput();

	if (!vrInput)
	{
		Logger::err << "[OpenVR] VRInput failed" << std::endl;
		return;
	}

	keyboardBuffer = new char[256];

	vrOverlay->CreateOverlay("UIOverlay", "UIOverlay", &uiOverlay);
	vrOverlay->SetOverlayFlag(uiOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
	vrOverlay->SetOverlayFlag(uiOverlay, vr::VROverlayFlags_IsPremultiplied, true);
	vrOverlay->ShowOverlay(uiOverlay);

	// Three wrist HUD overlays (ammo/health/radar), not interactive, hidden
	// until the player raises their off hand to look at them
	struct { vr::VROverlayHandle_t* handle; const char* key; } wristOverlaysToCreate[] = {
		{ &wristAmmoOverlay, "WristAmmoOverlay" },
		{ &wristHealthOverlay, "WristHealthOverlay" },
		{ &wristRadarOverlay, "WristRadarOverlay" },
	};
	for (auto& entry : wristOverlaysToCreate)
	{
		vr::EVROverlayError wristCreateErr = vrOverlay->CreateOverlay(entry.key, entry.key, entry.handle);
		if (wristCreateErr != vr::VROverlayError_None)
		{
			Logger::log << "[OpenVR] Could not create " << entry.key << ": " << wristCreateErr << std::endl;
		}
		vrOverlay->SetOverlayFlag(*entry.handle, vr::VROverlayFlags_IsPremultiplied, true);
	}

	std::filesystem::path manifest = std::filesystem::current_path() / "VR" / "OpenVR" / "haloce.vrmanifest";
	vr::EVRApplicationError appErr = vr::VRApplications()->AddApplicationManifest(manifest.string().c_str());

	if (appErr != vr::VRApplicationError_None)
	{
		Logger::log << "[OpenVR] Could not add application manifest: " << appErr << std::endl;
	}

	appErr = vr::VRApplications()->IdentifyApplication(GetCurrentProcessId(), "livingfray.haloce");

	if (appErr != vr::VRApplicationError_None)
	{
		Logger::log << "[OpenVR] Could not set id: " << appErr << std::endl;
	}


	std::filesystem::path actions = std::filesystem::current_path() / "VR" / "OpenVR" / "actions.json";
	vrInput->SetActionManifestPath(actions.string().c_str());

	vr::EVRInputError ActionSetError = vrInput->GetActionSetHandle("/actions/default", &actionSets[0].ulActionSet);

	if (ActionSetError != vr::EVRInputError::VRInputError_None)
	{
		Logger::err << "[OpenVR] Could not get action set: " << ActionSetError << std::endl;
	}

	vr::EVRInputError skeletonError = vrInput->GetActionHandle("/actions/default/in/LeftHand", &leftHandSkeleton);

	if (skeletonError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get left skeleton binding: " << skeletonError << std::endl;
	}

	skeletonError = vrInput->GetActionHandle("/actions/default/in/RightHand", &rightHandSkeleton);

	if (skeletonError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get right skeleton binding: " << skeletonError << std::endl;
	}

	bHasValidTipPoses = true;
	vr::EVRInputError poseError = vrInput->GetActionHandle("/actions/default/in/LeftTip", &leftHandTip);

	if (poseError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get left hand pose: " << poseError << std::endl;
		bHasValidTipPoses = false;
	}

	poseError = vrInput->GetActionHandle("/actions/default/in/RightTip", &rightHandTip);

	if (poseError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get right hand pose: " << poseError << std::endl;
		bHasValidTipPoses = false;
	}

	vr::EVRInputError leftFireError = vrInput->GetActionHandle("/actions/default/out/LeftFire", &leftFire);
	vr::EVRInputError rightFireError = vrInput->GetActionHandle("/actions/default/out/RightFire", &rightFire);

	if (leftFireError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get leftFire (vibration) action: " << leftFireError << std::endl;
	}

	if (rightFireError != vr::EVRInputError::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get leftFire (vibration) action: " << rightFireError << std::endl;
	}

	UpdateInputs();
	UpdateSkeleton(ControllerRole::Left);
	UpdateSkeleton(ControllerRole::Right);
	UpdatePose(ControllerRole::Left);
	UpdatePose(ControllerRole::Right);

	vrSystem->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);

	// Voodoo magic to convert normal view frustums into asymmetric ones through selective cropping

	float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
	vrSystem->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);
	Logger::log << "[OpenVR] Left eye raw projection[l, r, t, b] = [" << l_left << ", " << l_right << ", " << l_top << ", " << l_bottom << "]" << std::endl;

	float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
	vrSystem->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);
	Logger::log << "[OpenVR] Right eye raw projection[l, r, t, b] = [" << r_left << ", " << r_right << ", " << r_top << ", " << r_bottom << "]" << std::endl;

	float tanHalfFov[2];

	tanHalfFov[0] = (std::max)({ -l_left, l_right, -r_left, r_right });
	tanHalfFov[1] = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });
	Logger::log << "[OpenVR] tanHalfFov[horiz, vert] = [" << tanHalfFov[0] << ", " << tanHalfFov[1] << "]" << std::endl;

	textureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
	textureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
	textureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
	textureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];
	Logger::log << "[OpenVR] Left eye textureBounds[uMin, uMax, vMin, vMax] = [" << textureBounds[0].uMin << ", " << textureBounds[0].uMax << ", " << textureBounds[0].vMin << ", " << textureBounds[0].vMax << "]" << std::endl;

	textureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
	textureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
	textureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
	textureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];
	Logger::log << "[OpenVR] Right eye textureBounds[uMin, uMax, vMin, vMax] = [" << textureBounds[1].uMin << ", " << textureBounds[1].uMax << ", " << textureBounds[1].vMin << ", " << textureBounds[1].vMax << "]" << std::endl;

	aspect = tanHalfFov[0] / tanHalfFov[1];
	fov = 2.0f * atan(tanHalfFov[1]);

	realWidth = recommendedWidth;
	realHeight = recommendedHeight;

	recommendedWidth = static_cast<uint32_t>(recommendedWidth / (std::max)(textureBounds[0].uMax - textureBounds[0].uMin, textureBounds[1].uMax - textureBounds[1].uMin));
	recommendedHeight = static_cast<uint32_t>(recommendedHeight / (std::max)(textureBounds[0].vMax - textureBounds[0].vMin, textureBounds[1].vMax - textureBounds[1].vMin));

	Logger::log << "[OpenVR] Stretched Width/Height from " << realWidth << "x" << realHeight << " to " << recommendedWidth << "x" << recommendedHeight << std::endl;
	Logger::log << "[OpenVR] Desired fov = " << (fov * (180.0f / 3.141593f)) << " Desired aspect ratio = " << aspect << std::endl;

	Logger::log << "[OpenVR] VR systems created successfully" << std::endl;
}

void OpenVR::OnGameFinishInit()
{
	VR_PROFILE_SCOPE(OpenVR_OnGameFinishInit);

	HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &d3dDevice, NULL, NULL);

	if (FAILED(result))
	{
		Logger::log << "[OpenVR] Could not initialise DirectX11: " << result << std::endl;
	}
	
	D3DSURFACE_DESC desc, desc2;
	Helpers::GetRenderTargets()[0].renderSurface->GetDesc(&desc);
	Helpers::GetRenderTargets()[1].renderSurface->GetDesc(&desc2);

	// Create shared textures
	// Eyes
	CreateTexAndSurface(0, recommendedWidth, recommendedHeight, desc.Usage, desc.Format);
	CreateTexAndSurface(1, recommendedWidth, recommendedHeight, desc.Usage, desc.Format);
	// UI Layers. Halo's own UI RT is sometimes X8 (no alpha). The overlay is
	// premultiplied and needs a real alpha channel so unused HUD padding can
	// be punched to transparent - see Game::FixHUDOverlayAlpha.
	CreateTexAndSurface(uiSurface, Game::instance.overlayWidth, Game::instance.overlayHeight, desc2.Usage, D3DFMT_A8R8G8B8);
	CreateTexAndSurface(crosshairSurface, Game::instance.overlayWidth, Game::instance.overlayHeight, desc2.Usage, D3DFMT_A8R8G8B8);
	
	scopeWidth = static_cast<uint32_t>(Game::instance.c_ScopeRenderScale->Value() * recommendedWidth);
	scopeHeight = static_cast<uint32_t>(Game::instance.c_ScopeRenderScale->Value() * recommendedWidth * 0.75f); // Maintain the 4x3 aspect ratio halo works best with
	CreateTexAndSurface(scopeSurface, scopeWidth, scopeHeight, desc2.Usage, desc2.Format);

	vr::EVROverlayError err;

	err = vrOverlay->SetOverlayWidthInMeters(uiOverlay, Game::instance.c_UIOverlayScale->Value());
	if (err != vr::VROverlayError_None)
	{
		Logger::log << "[OpenVR] Error setting overlay width: " << err << std::endl;
	}
	Logger::log << "[OpenVR] Set UI Width = " << Game::instance.c_UIOverlayScale->Value() << std::endl;

	float curvature = Game::instance.c_UIOverlayCurvature->Value();
	if (curvature != 0.0f)
	{
		vrOverlay->SetOverlayCurvature(uiOverlay, curvature);
		Logger::log << "[OpenVR] Set UI Curvature = " << curvature << std::endl;
	}

	vr::HmdMatrix34_t overlayTransform = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 2.0f,
		0.0f, 0.0f, 1.0f, -3.0f
	};

	vrOverlay->SetOverlayTransformAbsolute(uiOverlay, vr::ETrackingUniverseOrigin::TrackingUniverseStanding, &overlayTransform);

	Logger::log << "[OpenVR] Finished Initialisation" << std::endl;
}

void OpenVR::Shutdown()
{
	if (vrSystem)
	{
		vr::VR_Shutdown();
	}
}


void OpenVR::CreateTexAndSurface(int index, UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format)
{
	VR_PROFILE_SCOPE(OpenVR_CreateTexAndSurface);

	HANDLE sharedHandle = nullptr;

	// Create texture on game
	HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(Width, Height, 1, Usage, Format, D3DPOOL_DEFAULT, &gameRenderTexture[index], &sharedHandle);
	if (FAILED(result))
	{
		Logger::err << "[OpenVR] Failed to create game " << index << " texture: " << result << std::endl;
		return;
	}

	result = gameRenderTexture[index]->GetSurfaceLevel(0, &gameRenderSurface[index]);
	if (FAILED(result))
	{
		Logger::err << "[OpenVR] Failed to retrieve game " << index << " surface: " << result << std::endl;
		return;
	}

	ID3D11Resource* tempResource = nullptr;

	// Open shared texture on vr
	result = d3dDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Resource), (void**)&tempResource);

	if (FAILED(result))
	{
		Logger::err << "[OpenVR] Failed to open shared resource " << index << ": " << result << std::endl;
		return;
	}

	result = tempResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&vrRenderTexture[index]);
	tempResource->Release();

	if (FAILED(result))
	{
		Logger::err << "[OpenVR] Failed to query texture interface " << index << ": " << result << std::endl;
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	vrRenderTexture[index]->GetDesc(&desc);

	Logger::log << "[OpenVR] Created shared texture " << index << ", " << desc.Width << "x" << desc.Height << std::endl;
}

void OpenVR::UpdatePoses()
{
	VR_PROFILE_SCOPE(OpenVR_UpdatePoses);

	if (!vrCompositor)
	{
		return;
	}

	VR_PROFILE_START(OpenVR_WaitGetPoses);
	vrCompositor->WaitGetPoses(renderPoses, vr::k_unMaxTrackedDeviceCount, gamePoses, vr::k_unMaxTrackedDeviceCount);
	VR_PROFILE_STOP(OpenVR_WaitGetPoses);

	UpdateSkeleton(ControllerRole::Left);
	UpdateSkeleton(ControllerRole::Right);
	UpdatePose(ControllerRole::Left);
	UpdatePose(ControllerRole::Right);

	if (!vrOverlay || !bMouseVisible)
	{
		return;
	}

	vr::VREvent_t vrEvent;
	while (vrOverlay->PollNextOverlayEvent(uiOverlay, &vrEvent, sizeof(vrEvent)))
	{
		switch (vrEvent.eventType)
		{
		case vr::VREvent_MouseMove:
			mousePos.x = vrEvent.data.mouse.x;
			mousePos.y = 1.0f - vrEvent.data.mouse.y;
			break;
		case vr::VREvent_MouseButtonDown:
			bMouseDown = true;
			break;
		case vr::VREvent_MouseButtonUp:
			bMouseDown = false;
			break;
		case vr::VREvent_KeyboardClosed_Global:
		case vr::VREvent_KeyboardDone:
			Game::instance.uiRenderer->UpdateActiveButton(nullptr);
			break;
		default:
			break;
		}
	}
}

void OpenVR::UpdateSkeleton(ControllerRole hand)
{
	VR_PROFILE_SCOPE(OpenVR_UpdateSkeleton);

	if (!vrInput)
	{
		return;
	}

	vr::InputSkeletalActionData_t actionData;

	vr::EVRInputError err = vrInput->GetSkeletalActionData(hand == ControllerRole::Left ? leftHandSkeleton : rightHandSkeleton, &actionData, sizeof(vr::InputSkeletalActionData_t));

	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not update skeleton action for hand " << static_cast<int>(hand) << ": " << err << std::endl;
		return;
	}

	if (!actionData.bActive)
	{
		return;
	}

	err = vrInput->GetSkeletalBoneData(
		hand == ControllerRole::Left ? leftHandSkeleton : rightHandSkeleton,
		vr::VRSkeletalTransformSpace_Model,
		vr::VRSkeletalMotionRange_WithController,
		bones[hand == ControllerRole::Left ? 0 : 1],
		31
	);

	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not update skeleton for hand " << static_cast<int>(hand) << ": " << err << std::endl;
		return;
	}
	
	const int h = hand == ControllerRole::Left ? 0 : 1;

	if (!bHasCachedWrists[h])
	{
		cachedWrists[h] = bones[h][1];
		bHasCachedWrists[h] = true;
	}
}

void OpenVR::UpdatePose(ControllerRole hand)
{
	VR_PROFILE_SCOPE(OpenVR_UpdatePose);

	if (!vrInput || !bHasValidTipPoses)
	{
		return;
	}

	vr::EVRInputError err = vrInput->GetPoseActionDataForNextFrame(
		hand == ControllerRole::Left ? leftHandTip : rightHandTip, vr::TrackingUniverseStanding,
		hand == ControllerRole::Left ? &leftHandTipPose : &rightHandTipPose,
		sizeof(vr::InputPoseActionData_t),
		vr::k_ulInvalidInputValueHandle
	);

	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Can't get tip pose for " << static_cast<int>(hand) << ": " << err << " (aiming may be incorrectly offset)" << std::endl;
		return;
	}
}

void OpenVR::PreDrawFrame(Renderer* renderer, float deltaTime)
{
}

void OpenVR::PositionOverlay()
{
	VR_PROFILE_SCOPE(OpenVR_PositionOverlay);

	// Get the HMD's position and rotation
	vr::HmdMatrix34_t mat = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
	vr::HmdVector3_t position;
	position.v[0] = mat.m[0][3];
	position.v[1] = mat.m[1][3];
	position.v[2] = mat.m[2][3];

	float distance = bMouseVisible ? Game::instance.c_MenuOverlayDistance->Value() : Game::instance.c_UIOverlayDistance->Value();

	vr::HmdMatrix34_t transform;

	// Head-following is deliberately skipped while a menu is open: a menu that
	// pitches and rolls with every head movement is unpleasant to read and to
	// point at, so menus always use the stock level, yaw-only placement even
	// when head following is enabled for gameplay.
	if (Game::instance.c_HUDFollowsHeadPitch->Value() && !bMouseVisible)
	{
		// Optional: place and orient the panel using the headset's FULL rotation,
		// so it follows pitch and roll as well as yaw. The stock path below
		// deliberately uses yaw only, keeping the panel level regardless of head
		// tilt; this is off by default so that behaviour is unchanged unless asked
		// for.
		//
		// Column 2 of the pose matrix is the headset's backward axis, so negating
		// it gives true forward including any pitch/roll, rather than the
		// horizontally flattened forward the yaw-only path uses.
		position.v[0] += mat.m[0][2] * -distance;
		position.v[1] += mat.m[1][2] * -distance;
		position.v[2] += mat.m[2][2] * -distance;

		// Reuse the headset's own rotation basis directly. The 0.75f vertical
		// scale from the stock path is preserved by scaling the up column, so the
		// panel keeps the same proportions in both modes.
		transform = {
			mat.m[0][0], mat.m[0][1] * 0.75f, mat.m[0][2], position.v[0],
			mat.m[1][0], mat.m[1][1] * 0.75f, mat.m[1][2], position.v[1],
			mat.m[2][0], mat.m[2][1] * 0.75f, mat.m[2][2], position.v[2]
		};
	}
	else
	{
		float len = sqrt(mat.m[0][2] * mat.m[0][2] + mat.m[2][2] * mat.m[2][2]);

		distance /= len;

		position.v[0] += mat.m[0][2] * -distance;
		position.v[2] += mat.m[2][2] * -distance;

		// Rotate only around Y for yaw
		float yaw = atan2(-mat.m[2][0], mat.m[2][2]);
		transform = {
			cos(yaw), 0, sin(yaw), position.v[0],
			0, 0.75f, 0, position.v[1],
			-sin(yaw), 0, cos(yaw), position.v[2]
		};
	}

	// Set the transform for the overlay
	vrOverlay->SetOverlayTransformAbsolute(uiOverlay, vr::TrackingUniverseStanding, &transform);
}

// Positions and submits one cropped wrist HUD element. svRight/svUp/svBackward
// are the shared rotation basis (SteamVR space); stackUp shifts this element
// up (positive) or down (negative) from the base offset along svUp, so the
// three elements stack top to bottom.
static void SubmitWristElement(vr::IVROverlay* vrOverlay, vr::VROverlayHandle_t overlayHandle,
	vr::TrackedDeviceIndex_t handIndex, const Vector3& svRight, const Vector3& svUp, const Vector3& svBackward,
	const Vector3& offset, const Vector3& elementOffset, float stackUp, float scale, float heightStretch,
	const Vector3& elementRotation, float uMin, float vMin, float uMax, float vMax, ID3D11Texture2D* sourceTexture)
{
	// Per-element fine offset (forward, left, up), added on top of the shared
	// group offset before the same SteamVR-space conversion applies to both
	Vector3 combinedOffset = offset + elementOffset;
	vrOverlay->SetOverlayWidthInMeters(overlayHandle, scale);

	// Per-element rotation, applied to this panel's own basis vectors so it turns
	// in place without moving. Done per element here rather than via the shared
	// rotation so each can be angled independently.
	//   x = roll  (spin within the panel's own plane)
	//   y = pitch (tip the top toward/away from the viewer)
	//   z = yaw   (swing the left/right edges toward/away)
	Vector3 rolledRight = svRight;
	Vector3 rolledUp = svUp;
	Vector3 rolledBackward = svBackward;

	const float degToRad = 3.14159265358979f / 180.0f;

	if (elementRotation.x != 0.0f)
	{
		const float a = elementRotation.x * degToRad;
		const float c = cos(a);
		const float sn = sin(a);
		const Vector3 r = rolledRight * c + rolledUp * sn;
		const Vector3 u = rolledUp * c - rolledRight * sn;
		rolledRight = r;
		rolledUp = u;
	}

	if (elementRotation.y != 0.0f)
	{
		// Pitch: rotate up and backward around the panel's own right axis
		const float a = elementRotation.y * degToRad;
		const float c = cos(a);
		const float sn = sin(a);
		const Vector3 u = rolledUp * c + rolledBackward * sn;
		const Vector3 b = rolledBackward * c - rolledUp * sn;
		rolledUp = u;
		rolledBackward = b;
	}

	if (elementRotation.z != 0.0f)
	{
		// Yaw: rotate right and backward around the panel's own up axis
		const float a = elementRotation.z * degToRad;
		const float c = cos(a);
		const float sn = sin(a);
		const Vector3 r = rolledRight * c + rolledBackward * sn;
		const Vector3 b = rolledBackward * c - rolledRight * sn;
		rolledRight = r;
		rolledBackward = b;
	}

	// SetOverlayWidthInMeters only controls width; there is no separate "set
	// height" call. Height is instead stretched independently by scaling the
	// magnitude of the panel's own up basis vector in the transform's rotation
	// submatrix - a non-unit-length basis vector applies a scale as well as a
	// direction, so this changes height without touching width or facing.
	// Guard against a zero or negative stretch from config: zero would collapse
	// the up basis vector to zero length, producing a degenerate transform, and
	// negative would flip the panel inside out.
	const float safeHeightStretch = (heightStretch > 0.01f) ? heightStretch : 0.01f;
	Vector3 stretchedUp = rolledUp * safeHeightStretch;

	// Stacking previously used svUp (the ROTATED up basis) for the offset between
	// elements. That is correct for orienting each panel's own tilt, but since
	// WristHUDRotation includes roll, svUp is itself a diagonal direction once
	// rotated - stacking along it produced a diagonal drift between elements
	// instead of a straight vertical line, worse the larger the roll. Stacking
	// now uses a fixed, unrotated up direction (matching the same convention the
	// position offset's Z component already uses with no rotation applied), so
	// the three elements stay vertically aligned regardless of panel roll, while
	// svRight/svUp/svBackward still control each panel's own facing/tilt.
	const Vector3 stackDirection(0.0f, 1.0f, 0.0f);

	vr::HmdMatrix34_t relativeTransform = {
		rolledRight.x, stretchedUp.x, rolledBackward.x, -combinedOffset.y + stackDirection.x * stackUp,
		rolledRight.y, stretchedUp.y, rolledBackward.y, combinedOffset.z + stackDirection.y * stackUp,
		rolledRight.z, stretchedUp.z, rolledBackward.z, -combinedOffset.x + stackDirection.z * stackUp
	};
	vr::EVROverlayError transformErr = vrOverlay->SetOverlayTransformTrackedDeviceRelative(overlayHandle, handIndex, &relativeTransform);
	if (transformErr != vr::VROverlayError_None)
	{
		Logger::log << "[OpenVR] Could not set wrist element transform: " << transformErr << std::endl;
	}

	vr::VRTextureBounds_t bounds{ uMin, vMin, uMax, vMax };
	vrOverlay->SetOverlayTextureBounds(overlayHandle, &bounds);

	vr::Texture_t wristTex{ (void*)sourceTexture, vr::TextureType_DirectX, vr::ColorSpace_Auto };
	vr::EVROverlayError wristTexErr = vrOverlay->SetOverlayTexture(overlayHandle, &wristTex);
	if (wristTexErr != vr::VROverlayError_None)
	{
		Logger::log << "[OpenVR] Could not submit wrist element texture: " << wristTexErr << std::endl;
	}

	vrOverlay->ShowOverlay(overlayHandle);
}

void OpenVR::UpdateWristHUD()
{
	// uiSurface (the texture this clones) is shared with the pause/main menu,
	// so without this the wrist clone shows whatever menu is open instead of
	// gameplay HUD whenever one is active
	if (!Game::instance.c_ShowWristHUD->Value() || bMouseVisible)
	{
		vrOverlay->HideOverlay(wristAmmoOverlay);
		vrOverlay->HideOverlay(wristHealthOverlay);
		vrOverlay->HideOverlay(wristRadarOverlay);
		return;
	}

	// Off hand: the hand not used for weapon aiming, same convention used
	// throughout the mod (flashlight, HUD toggle, grenade throw, etc.)
	ControllerRole offHand = Game::instance.bLeftHanded ? ControllerRole::Right : ControllerRole::Left;
	vr::TrackedDeviceIndex_t handIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(
		offHand == ControllerRole::Left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);

#define WRIST_HUD_DEBUG 1
#if WRIST_HUD_DEBUG
	static std::chrono::steady_clock::time_point lastWristLogTime;
	bool bShouldLogNow = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastWristLogTime).count() > 0.5;
#endif

	if (handIndex == vr::k_unTrackedDeviceIndexInvalid || !renderPoses[handIndex].bPoseIsValid)
	{
#if WRIST_HUD_DEBUG
		if (bShouldLogNow)
		{
			lastWristLogTime = std::chrono::steady_clock::now();
			Logger::log << "[WristHUDDebug] hand index invalid or pose invalid. handIndex=" << handIndex << std::endl;
		}
#endif
		vrOverlay->HideOverlay(wristAmmoOverlay);
		vrOverlay->HideOverlay(wristHealthOverlay);
		vrOverlay->HideOverlay(wristRadarOverlay);
		return;
	}

	// HMD position and forward direction, same matrix convention already used
	// in PositionOverlay to push the main UI panel out in front of the face
	vr::HmdMatrix34_t hmdMat = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
	Vector3 hmdPos(hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3]);
	Vector3 hmdForward(-hmdMat.m[0][2], -hmdMat.m[1][2], -hmdMat.m[2][2]);
	hmdForward = hmdForward.normalize();

	vr::HmdMatrix34_t handMat = renderPoses[handIndex].mDeviceToAbsoluteTracking;
	Vector3 handPos(handMat.m[0][3], handMat.m[1][3], handMat.m[2][3]);

	Vector3 toHand = handPos - hmdPos;
	float distanceToHand = toHand.length();
	Vector3 toHandDir = (distanceToHand > 0.001f) ? toHand * (1.0f / distanceToHand) : hmdForward;

	// Visible only when the hand is raised reasonably close to the headset AND
	// roughly in the direction being looked, similar to checking a real watch.
	// distanceToHand comes straight from raw SteamVR device poses, which are
	// always in real metres already - MetresToWorld converts metres INTO Halo's
	// own internal world-unit scale (divides by 3.048), which does not apply
	// here at all and was shrinking a 0.6m threshold down to under 20cm,
	// meaning the distance check could almost never pass.
	const float maxDistance = 0.6f;
	const float minDot = 0.5f; // within ~60 degrees of dead centre

	float dotValue = hmdForward.dot(toHandDir);
	bool bLookingAtWrist = distanceToHand < maxDistance && dotValue > minDot;

#if WRIST_HUD_DEBUG
	if (bShouldLogNow)
	{
		lastWristLogTime = std::chrono::steady_clock::now();
		Logger::log << "[WristHUDDebug] distanceToHand=" << distanceToHand
			<< " maxDistance=" << maxDistance << " dot=" << dotValue
			<< " minDot=" << minDot << " looking=" << bLookingAtWrist << std::endl;
	}
#endif

	if (!bLookingAtWrist)
	{
		vrOverlay->HideOverlay(wristAmmoOverlay);
		vrOverlay->HideOverlay(wristHealthOverlay);
		vrOverlay->HideOverlay(wristRadarOverlay);
		return;
	}

	// c_WristHUDOffset/c_WristHUDRotation follow the mod's own (forward, left,
	// up) convention used throughout the rest of the config, but SteamVR's
	// device-relative transform wants its own (X=right, Y=up, Z=backward)
	// space, so both the position and the rotation basis vectors are converted:
	// right = -left, up = up, backward = -forward.
	//
	// The rotation itself is built with the mod's own, already-tested Matrix4
	// rotateZ/rotateY/rotateX (same order and convention as ControllerRotation)
	// rather than constructing new rotation math by hand, then the resulting
	// basis columns are converted the same way as the position offset above.
	Vector3 rot = Game::instance.c_WristHUDRotation->Value();
	Matrix4 rotMatrix;
	rotMatrix.rotateZ(rot.z);
	rotMatrix.rotateY(rot.y);
	rotMatrix.rotateX(rot.x);

	Vector4 rotForwardCol = rotMatrix.getColumn(0); // mod X = forward
	Vector4 rotLeftCol = rotMatrix.getColumn(1);    // mod Y = left
	Vector4 rotUpCol = rotMatrix.getColumn(2);      // mod Z = up

	// SteamVR X (right) = -left, Y (up) = up, Z (backward) = -forward
	Vector3 svRight(-rotLeftCol.x, -rotLeftCol.y, -rotLeftCol.z);
	Vector3 svUp(rotUpCol.x, rotUpCol.y, rotUpCol.z);
	Vector3 svBackward(-rotForwardCol.x, -rotForwardCol.y, -rotForwardCol.z);

	Vector3 offset = Game::instance.c_WristHUDOffset->Value();
	// WristHUDScale is no longer read directly; ammo and health now have their
	// own independent scale, same as radar already did
	float spacing = Game::instance.c_WristHUDElementSpacing->Value();
	ID3D11Texture2D* sourceTexture = vrRenderTexture[uiSurface];

	// Health is the anchor (base offset, no stacking); ammo sits one spacing
	// above it, radar one spacing below, all along the panel's own up axis
	SubmitWristElement(vrOverlay, wristAmmoOverlay, handIndex, svRight, svUp, svBackward, offset, Game::instance.c_WristHUDAmmoOffset->Value(), spacing, Game::instance.c_WristHUDAmmoScale->Value(), Game::instance.c_WristHUDAmmoHeightStretch->Value(), Game::instance.c_WristHUDAmmoRotation->Value(),
		Game::instance.c_WristHUDAmmoUMin->Value(), Game::instance.c_WristHUDAmmoVMin->Value(),
		Game::instance.c_WristHUDAmmoUMax->Value(), Game::instance.c_WristHUDAmmoVMax->Value(), sourceTexture);

	SubmitWristElement(vrOverlay, wristHealthOverlay, handIndex, svRight, svUp, svBackward, offset, Game::instance.c_WristHUDHealthOffset->Value(), 0.0f, Game::instance.c_WristHUDHealthScale->Value(), Game::instance.c_WristHUDHealthHeightStretch->Value(), Game::instance.c_WristHUDHealthRotation->Value(),
		Game::instance.c_WristHUDHealthUMin->Value(), Game::instance.c_WristHUDHealthVMin->Value(),
		Game::instance.c_WristHUDHealthUMax->Value(), Game::instance.c_WristHUDHealthVMax->Value(), sourceTexture);

	float radarScale = Game::instance.c_WristHUDRadarScale->Value();
	SubmitWristElement(vrOverlay, wristRadarOverlay, handIndex, svRight, svUp, svBackward, offset, Game::instance.c_WristHUDRadarOffset->Value(), -spacing, radarScale, Game::instance.c_WristHUDRadarHeightStretch->Value(), Game::instance.c_WristHUDRadarRotation->Value(),
		Game::instance.c_WristHUDRadarUMin->Value(), Game::instance.c_WristHUDRadarVMin->Value(),
		Game::instance.c_WristHUDRadarUMax->Value(), Game::instance.c_WristHUDRadarVMax->Value(), sourceTexture);
}

void OpenVR::PostDrawFrame(Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(OpenVR_PostDrawFrame);

	if (!vrCompositor)
	{
		return;
	}

	// Wait for frame to finish rendering
	VR_PROFILE_START(OpenVR_QueryD3D);
	IDirect3DQuery9* pEventQuery = nullptr;
	Helpers::GetDirect3DDevice9()->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
	if (pEventQuery != nullptr)
	{
		pEventQuery->Issue(D3DISSUE_END);
		while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
		pEventQuery->Release();
	}
	VR_PROFILE_STOP(OpenVR_QueryD3D);

	VR_PROFILE_START(OpenVR_SubmitEyes);
	vr::Texture_t leftEye { (void*)vrRenderTexture[0], vr::TextureType_DirectX, vr::ColorSpace_Auto};
	vr::EVRCompositorError error = vrCompositor->Submit(vr::Eye_Left, &leftEye, &textureBounds[0], vr::Submit_Default);

	if (error != vr::VRCompositorError_None)
	{
		Logger::log << "[OpenVR] Could not submit left eye texture: " << error << std::endl;
	}

	vr::Texture_t rightEye{ (void*)vrRenderTexture[1], vr::TextureType_DirectX, vr::ColorSpace_Auto };
	error = vrCompositor->Submit(vr::Eye_Right, &rightEye, &textureBounds[1], vr::Submit_Default);

	if (error != vr::VRCompositorError_None)
	{
		Logger::log << "[OpenVR] Could not submit right eye texture: " << error << std::endl;
	}
	VR_PROFILE_STOP(OpenVR_SubmitEyes);

	PositionOverlay();
	UpdateWristHUD();

	// The HUD toggle gesture hides the floating UI by hiding the overlay rather than
	// skipping the HUD draw, so the game's HUD logic (including the shield recharge
	// sound) still runs every frame. The menu shares this overlay, so it is never
	// hidden while a menu is visible.
	if (Game::instance.bHideHUD && !bMouseVisible)
	{
		vrOverlay->HideOverlay(uiOverlay);
	}
	else
	{
		vrOverlay->ShowOverlay(uiOverlay);

		vr::Texture_t uiTex{ (void*)vrRenderTexture[uiSurface], vr::TextureType_DirectX, vr::ColorSpace_Auto };
		vr::EVROverlayError oError = vrOverlay->SetOverlayTexture(uiOverlay, &uiTex);

		if (oError != vr::EVROverlayError::VROverlayError_None)
		{
			Logger::log << "[OpenVR] Could not submit ui texture: " << oError << std::endl;
		}
	}

	VR_PROFILE_START(OpenVR_PostPresentHandoff);
	vrCompositor->PostPresentHandoff();
	VR_PROFILE_STOP(OpenVR_PostPresentHandoff);
}

void OpenVR::UpdateCameraFrustum(CameraFrustum* frustum, int eye)
{
	VR_PROFILE_SCOPE(OpenVR_UpdateCameraFrustum);
	frustum->fov = fov;

	Matrix4 eyeMatrix = GetHMDMatrixPoseEye((vr::Hmd_Eye) eye);

	Matrix4 headMatrix = GetHMDTransform(true);

	// Yaw should follow cutscene camera
	CutsceneData* cutscene = Helpers::GetCutsceneData();

	if (cutscene->bInCutscene)
	{
		float cameraYaw = atan2(frustum->facingDirection.y, frustum->facingDirection.x) * (180.0f / 3.1415926f);
		headMatrix.rotateZ(cameraYaw);
	}

	Matrix4 viewMatrix = (headMatrix * eyeMatrix.invert()).scale(Game::instance.MetresToWorld(1.0f));

	Matrix3 rotationMatrix = GetRotationMatrix(headMatrix);


	frustum->facingDirection = Vector3(1.0f, 0.0f, 0.0f);
	frustum->upDirection = Vector3(0.0f, 0.0f, 1.0f);

	frustum->facingDirection = (rotationMatrix * frustum->facingDirection).normalize();
	frustum->upDirection = (rotationMatrix * frustum->upDirection).normalize();

	Vector3 newPos = viewMatrix * Vector3(0.0f, 0.0f, 0.0f);

	frustum->position = frustum->position + newPos;
}

int OpenVR::GetViewWidth()
{
    return recommendedWidth;
}

int OpenVR::GetViewHeight()
{
    return recommendedHeight;
}

float OpenVR::GetViewWidthStretch()
{
	return recommendedWidth / static_cast<float>(realWidth);
}

float OpenVR::GetViewHeightStretch()
{
	return recommendedHeight / static_cast<float>(realHeight);
}

float OpenVR::GetAspect()
{
	return aspect;
}

int OpenVR::GetScopeWidth()
{
	return scopeWidth;
}

int OpenVR::GetScopeHeight()
{
	return scopeHeight;
}

void OpenVR::Recentre()
{
	SetLocationOffset(ConvertSteamVRMatrixToMatrix4(renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking) * Vector3(0.0f, 0.0f, 0.0f));
}

void OpenVR::SetLocationOffset(Vector3 newOffset)
{
	positionOffset = newOffset;
}

Vector3 OpenVR::GetLocationOffset()
{
	return positionOffset;
}

void OpenVR::SetYawOffset(float newOffset)
{
	yawOffset = newOffset;
}

float OpenVR::GetYawOffset()
{
	return yawOffset;
}

void OpenVR::TriggerHapticVibration(ControllerRole role, float fStartSecondsFromNow, float fDurationSeconds, float fFrequency, float fAmplitude)
{
#if HAPTICS_DEBUG
		Logger::log << "[WeaponHaptics] TriggerHapticVibration called with: \tRole: " << static_cast<int>(role)
		<<
		"\tstartSeconds: "
		<< fStartSecondsFromNow
		<< "\t"
		"duration: "
		<< fDurationSeconds
		<< "\t"
		"frequency: "
		<< fFrequency
		<< "\t"
		"amplitude: "
		<< fAmplitude
		<< "\t"
		<< std::endl;
#endif 

	vr::VRActionHandle_t* action;
	
	if (role == ControllerRole::Left)
	{
		action = &leftFire;
	}
	else 
	{
		action = &rightFire;
	}

	if (action)
	{
		vr::EVRInputError error = vrInput->TriggerHapticVibrationAction(*action, fStartSecondsFromNow, fDurationSeconds, fFrequency, fAmplitude, vr::k_ulInvalidInputValueHandle);

#if HAPTICS_DEBUG
		if (error != vr::EVRInputError::VRInputError_None)
		{
			Logger::log << "[OpenVR] Could not trigger haptic vibration action: " << error << std::endl;
		}
#endif
	}
}

void OpenVR::TriggerHapticPulse(ControllerRole role, short usDurationMicroSec)
{
#if HAPTICS_DEBUG
	Logger::log << "[WeaponHaptics] TriggerHapticPulse called with: \tRole: " << static_cast<int>(role)
		<<
		"\tusDurationMicroSec: "
		<< usDurationMicroSec
		<< "\t"
		<< std::endl;
#endif 
	
	vr::TrackedDeviceIndex_t controllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(role == ControllerRole::Left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
	vrSystem->TriggerHapticPulse(controllerIndex, 0, usDurationMicroSec);
}

Matrix4 OpenVR::GetHMDTransform(bool bRenderPose)
{
	VR_PROFILE_SCOPE(OpenVR_GetHMDTransform);
	if (bRenderPose)
	{
		return ConvertSteamVRMatrixToMatrix4(renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking).translate(-positionOffset).rotateZ(-yawOffset);
	}
	else
	{
		return ConvertSteamVRMatrixToMatrix4(gamePoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking).translate(-positionOffset).rotateZ(-yawOffset);
	}
}

Matrix4 OpenVR::GetRawControllerTransform(ControllerRole role, bool bRenderPose)
{
	VR_PROFILE_SCOPE(OpenVR_GetRawControllerTransform);
	vr::TrackedDeviceIndex_t controllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(role == ControllerRole::Left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);

	Matrix4 outMatrix;

	if (bRenderPose)
	{
		return ConvertSteamVRMatrixToMatrix4(renderPoses[controllerIndex].mDeviceToAbsoluteTracking).translate(-positionOffset).rotateZ(-yawOffset);
	}
	else
	{
		return ConvertSteamVRMatrixToMatrix4(gamePoses[controllerIndex].mDeviceToAbsoluteTracking).translate(-positionOffset).rotateZ(-yawOffset);
	}
}

Matrix4 OpenVR::GetControllerTransformInternal(ControllerRole role, int bone, bool bRenderPose)
{
	VR_PROFILE_SCOPE(OpenVR_GetControllerTransformInternal);
	if (!vrSystem)
	{
		return Matrix4();
	}

	vr::TrackedDeviceIndex_t controllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(role == ControllerRole::Left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);

	const int roleId = role == ControllerRole::Left ? 0 : 1;

	vr::VRBoneTransform_t boneTransform = bone < 0 ? cachedWrists[roleId] : bones[roleId][bone];

	Matrix4 outMatrix = GetRawControllerTransform(role, bRenderPose);

	Vector3 bonePos = Vector3(boneTransform.position.v[0], boneTransform.position.v[1], boneTransform.position.v[2]);
	Vector4 quat = Vector4(boneTransform.orientation.x, boneTransform.orientation.y, boneTransform.orientation.z, boneTransform.orientation.w);

	Matrix4 boneMatrix;
	Transform tempTransform;
	Helpers::MakeTransformFromQuat(&quat, &tempTransform);

	for (int x = 0; x < 3; x++)
	{
		for (int y = 0; y < 3; y++)
		{
			// Not sure why get is const, you can directly set the values with setrow/setcolumn anyway
			const_cast<float*>(boneMatrix.get())[x + y * 4] = tempTransform.rotation[x + y * 3];
		}
	}

	boneMatrix.setColumn(3, bonePos);

	Matrix4 boneMatrixGame(
		boneMatrix.get()[2 + 2 * 4], boneMatrix.get()[0 + 2 * 4], -boneMatrix.get()[1 + 2 * 4], 0.0,
		boneMatrix.get()[2 + 0 * 4], boneMatrix.get()[0 + 0 * 4], -boneMatrix.get()[1 + 0 * 4], 0.0,
		-boneMatrix.get()[2 + 1 * 4], -boneMatrix.get()[0 + 1 * 4], boneMatrix.get()[1 + 1 * 4], 0.0,
		-boneMatrix.get()[2 + 3 * 4], -boneMatrix.get()[0 + 3 * 4], boneMatrix.get()[1 + 3 * 4], 1.0f
	);

	Vector3 pos = boneMatrixGame * Vector3(0.0f, 0.0f, 0.0f);
	boneMatrixGame.translate(-pos);
	boneMatrixGame.rotate(180.0f, boneMatrixGame * Vector3(0.0f, 0.0f, 1.0f));
	boneMatrixGame.translate(pos);

	outMatrix = outMatrix * boneMatrixGame;

	return outMatrix;
}

Matrix4 OpenVR::GetControllerTransform(ControllerRole role, bool bRenderPose)
{
	return GetControllerTransformInternal(role, -1, bRenderPose);
}

Matrix4 OpenVR::GetControllerBoneTransform(ControllerRole role, int bone, bool bRenderPose)
{
	return GetControllerTransformInternal(role, bone, bRenderPose);
}

Vector3 OpenVR::GetControllerVelocity(ControllerRole role, bool bRenderPose)
{
	VR_PROFILE_SCOPE(OpenVR_GetControllerVelocity);
	if (!vrSystem)
	{
		return Vector3();
	}
	
	vr::TrackedDeviceIndex_t controllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(role == ControllerRole::Left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);

	vr::HmdVector3_t velocity;

	if (bRenderPose)
	{
		velocity = renderPoses[controllerIndex].vVelocity;
	}
	else
	{
		velocity = gamePoses[controllerIndex].vVelocity;
	}

	Matrix4 rotMat;
	rotMat.rotateZ(-yawOffset);

	return rotMat * Vector3(-velocity.v[2], -velocity.v[0], velocity.v[1]) * Game::instance.MetresToWorld(1.0f);
}

bool OpenVR::TryGetControllerFacing(ControllerRole role, Vector3& outDirection)
{
	VR_PROFILE_SCOPE(OpenVR_TryGetControllerFacing);

	// todo: only get once, cache offset, use that
	if (bHasValidTipPoses)
	{
		const vr::InputPoseActionData_t& data = role == ControllerRole::Left ? leftHandTipPose : rightHandTipPose;
		if (!data.bActive || !data.pose.bPoseIsValid)
		{
			return false;
		}

		Matrix4 facingMatrix = ConvertSteamVRMatrixToMatrix4(data.pose.mDeviceToAbsoluteTracking).translate(-positionOffset).rotateZ(-yawOffset);

		outDirection = facingMatrix.getLeftAxis();
	}
	return bHasValidTipPoses;
}

IDirect3DSurface9* OpenVR::GetRenderSurface(int eye)
{
    return gameRenderSurface[eye];
}

IDirect3DTexture9* OpenVR::GetRenderTexture(int eye)
{
    return gameRenderTexture[eye];
}

IDirect3DSurface9* OpenVR::GetUISurface()
{
    return gameRenderSurface[uiSurface];
}

IDirect3DSurface9* OpenVR::GetCrosshairSurface()
{
	return gameRenderSurface[crosshairSurface];
}

IDirect3DSurface9* OpenVR::GetScopeSurface()
{
	return gameRenderSurface[scopeSurface];
}

IDirect3DTexture9* OpenVR::GetScopeTexture()
{
	return gameRenderTexture[scopeSurface];
}

void OpenVR::SetMouseVisibility(bool bIsVisible)
{
	VR_PROFILE_SCOPE(OpenVR_SetMouseVisibility);

	if (!vrOverlay)
	{
		return;
	}

	if (bMouseVisible == bIsVisible)
	{
		return;
	}

	bMouseVisible = bIsVisible;

	if (!bMouseVisible)
	{
		bMouseDown = false;
	}

	vrOverlay->SetOverlayInputMethod(uiOverlay, bIsVisible ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
	vrOverlay->SetOverlayWidthInMeters(uiOverlay, bIsVisible ? Game::instance.c_MenuOverlayScale->Value() : Game::instance.c_UIOverlayScale->Value());
}

void OpenVR::SetCrosshairTransform(Matrix4& newTransform)
{
	VR_PROFILE_SCOPE(OpenVR_SetCrosshairTransform);

	// This should be moved into game code really

	Vector3 pos = (newTransform * Vector3(0.0f, 0.0f, 0.0f)) * Game::instance.MetresToWorld(1.0f) + Helpers::GetCamera().position;
	Matrix3 rot;
	Vector2 size(1.33f, 1.0f);
	size *= Game::instance.MetresToWorld(Game::instance.c_CrosshairScale->Value());

	newTransform.translate(-pos);
	newTransform.rotate(90.0f, newTransform.getLeftAxis());
	newTransform.rotate(-90.0f, newTransform.getUpAxis());
	newTransform.rotate(-90.0f, newTransform.getLeftAxis());

	for (int i = 0; i < 3; i++)
	{
		rot.setColumn(i, &newTransform.get()[i * 4]);
	}

	Game::instance.inGameRenderer.DrawRenderTarget(gameRenderTexture[crosshairSurface], pos, rot, size, false);
}

void OpenVR::SetActiveActionSet(int index, const std::string& actionSetName)
{	
    vr::EVRInputError error = vrInput->GetActionSetHandle(actionSetName.c_str(), &actionSets[index].ulActionSet);
    if (error != vr::VRInputError_None)
    {
        Logger::err << "[OpenVR] Could not set active action set: " << actionSetName << " Error: " << error << std::endl;
    }
}

void OpenVR::UpdateInputs()
{	
	SetActiveActionSet(0, "/actions/default");
	SetActiveActionSet(1, "/actions/left handed");

	VR_PROFILE_SCOPE(OpenVR_UpdateInputs);

	vr::EVRInputError error = vrInput->UpdateActionState(actionSets, sizeof(vr::VRActiveActionSet_t), 2);

	if (error != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not update inputs: " << error << std::endl;
	}
}

InputBindingID OpenVR::RegisterBoolInput(std::string set, std::string action)
{
	InputBindingID id;
	vr::EVRInputError err = vrInput->GetActionHandle(("/actions/" + set + "/in/" + action).c_str(), &id);
	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not register bool input /actions/" << set << "/in/" << action << ": " << err << std::endl;
	}
	else
	{
		Logger::log << "[OpenVR] Registered /actions/" << set << "/in/" << action << " with id " << id << std::endl;
	}
	return id;
}

InputBindingID OpenVR::RegisterVector2Input(std::string set, std::string action)
{
	InputBindingID id;
	vr::EVRInputError err = vrInput->GetActionHandle(("/actions/" + set + "/in/" + action).c_str(), &id);
	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not register vector2 input /actions/" << set << "/in/" << action << ": " << err << std::endl;
	}
	else
	{
		Logger::log << "[OpenVR] Registered /actions/" << set << "/in/" << action << " with id " << id << std::endl;
	}
	return id;
}

bool OpenVR::GetBoolInput(InputBindingID id)
{
	static bool dummy = false;
	return GetBoolInput(id, dummy);
}

bool OpenVR::GetBoolInput(InputBindingID id, bool& bHasChanged)
{
	static vr::InputDigitalActionData_t digital;
	vr::EVRInputError err = vrInput->GetDigitalActionData(id, &digital, sizeof(vr::InputDigitalActionData_t), vr::k_ulInvalidInputValueHandle);
	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get digital action: " << err << std::endl;
	}

	bHasChanged = digital.bChanged;

	if (digital.bChanged && digital.bState)
	{
		Logger::log << "[OpenVR] Getting binding " << id << std::endl;
	}

	return digital.bState;
}

Vector2 OpenVR::GetVector2Input(InputBindingID id)
{
	static vr::InputAnalogActionData_t Analog;
	vr::EVRInputError err = vrInput->GetAnalogActionData(id, &Analog, sizeof(vr::InputAnalogActionData_t), vr::k_ulInvalidInputValueHandle);
	if (err != vr::VRInputError_None)
	{
		Logger::log << "[OpenVR] Could not get analog action: " << err << std::endl;
	}

	return Vector2(Analog.x, Analog.y);
}

Vector2 OpenVR::GetMousePos()
{
	return mousePos;
}

bool OpenVR::GetMouseDown()
{
	return bMouseDown;
}

void OpenVR::ShowKeyboard(const std::string& textBuffer)
{
	if (!vrOverlay)
	{
		return;
	}

	strncpy(keyboardBuffer, textBuffer.c_str(), 256);

	vrOverlay->ShowKeyboardForOverlay(uiOverlay, vr::k_EGamepadTextInputModeSubmit, vr::k_EGamepadTextInputLineModeSingleLine, vr::KeyboardFlag_Modal, "VR Settings", 256, textBuffer.c_str(), 0);
	bKeyboardVisible = true;
}

bool OpenVR::IsKeyboardVisible()
{
	return bKeyboardVisible;
}

void OpenVR::HideKeyboard()
{
	if (!vrOverlay)
	{
		return;
	}

	vrOverlay->HideKeyboard();
	bKeyboardVisible = false;
}

std::string OpenVR::GetKeyboardInput()
{
	if (!vrOverlay)
	{
		return std::string();
	}

	vrOverlay->GetKeyboardText(keyboardBuffer, 256);
	return keyboardBuffer;
}

std::string OpenVR::GetDeviceName()
{
	if (!vrSystem)
	{
		return "Unknown";
	}

	char str[128] = {};

	uint32_t size = vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, str, 128);

	if (size == 0)
	{
		return "Unknown";
	}

	return str;
}