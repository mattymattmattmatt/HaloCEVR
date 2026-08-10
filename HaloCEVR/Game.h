#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include "Config/Config.h"
#include "VR/IVR.h"
#include "Helpers/Renderer.h"
#include "Helpers/RenderTarget.h"
#include "Helpers/Objects.h"
#include "Maths/Vectors.h"
#include "WeaponHandler.h"
#include "InputHandler.h"
#include "InGameRenderer.h"
#include "WeaponHapticsConfig.h"
#include "Profiler.h"
#include "UI/UIRenderer.h"
#include "UI/SettingsMenu.h"

enum class ERenderState { UNKNOWN, LEFT_EYE, RIGHT_EYE, GAME, SCOPE};

class Game
{
public:
	// Whether the currently equipped weapon is a one handed weapon (pistol, plasma pistol, plasma rifle or needler)
	bool IsCurrentWeaponOneHanded() const;
	// Debug-only, see GRENADE_VELOCITY_DEBUG in WeaponHandler.cpp
	void UpdateGrenadeVelocityScan();

	// Live in-headset adjustment of HUD element placement, so positioning can be
	// tuned by watching it move rather than by editing config and relaunching
	void UpdateLiveHUDAdjuster();
	int liveAdjustTarget = 0;
	float liveAdjustStep = 0.01f;
	// Draws the predicted grenade trajectory while the grenade button is held
	void DrawGrenadeArc();
	static Game instance;

	void Init();
	void Shutdown();

	void OnInitDirectX();
	void PreDrawFrame(struct Renderer* renderer, float deltaTime);
	void PreDrawEye(struct Renderer* renderer, float deltaTime, int eye);
	void PostDrawEye(struct Renderer* renderer, float deltaTime, int eye);
	bool PreDrawScope(struct Renderer* renderer, float deltaTime);
	void PostDrawScope(struct Renderer* renderer, float deltaTime);
	void PreDrawMirror(struct Renderer* renderer, float deltaTime);
	void PostDrawMirror(struct Renderer* renderer, float deltaTime);
	void PostDrawFrame(struct Renderer* renderer, float deltaTime);
	Vector3 GetSmoothedInput() const;

	bool PreDrawHUD();
	void PostDrawHUD();

	bool PreDrawMenu();
	void PostDrawMenu();

	bool PreDrawLoading(int param1, struct Renderer* renderer);
	void PostDrawLoading(int param1, struct Renderer* renderer);

	bool PreDrawCrosshair(short* anchorLocation);
	void PostDrawCrosshair();

	void PreDrawImage(void* param1, void* param2);
	void PostDrawImage(void* param1, void* param2);

	void UpdateViewModel(HaloID& id, struct Vector3* pos, struct Vector3* facing, struct Vector3* up, struct TransformQuat* BoneTransforms, struct Transform* OutBoneTransforms);
	void PreFireWeapon(HaloID& WeaponID, short param2);
	void PostFireWeapon(HaloID& WeaponID, short param2);
	void PreThrowGrenade(HaloID& playerID);
	void PostThrowGrenade(HaloID& playerID);
	bool GetCalculatedHandPositions(Matrix4& controllerTransform, Vector3& dominantHandPos, Vector3& offHand); 
	void ReloadStart(HaloID param1, short param2, bool param3);
	void ReloadEnd(short param1, HaloID param2);

	void UpdateInputs();
	void CalculateSmoothedInput();
	void UpdateRoomScaleMovement();

	void UpdateCamera(float& yaw, float& pitch);
	void SetMousePosition(int& x, int& y);
	void UpdateMouseInfo(struct MouseInfo* mouseInfo);

	void SetViewportScale(struct Viewport* viewport);

	bool GetDrawMirror() const { return mirrorSource == ERenderState::GAME && c_DrawMirror->Value(); }

	ERenderState GetRenderState() const { return renderState; }

	bool ShouldUseOriginalScope() const { return bUse3DOFAiming || c_UseOriginalScope->Value(); }

	float GetScopeSize() const { return ShouldUseOriginalScope() ? c_3DOFScopeScale->Value() : c_ScopeScale->Value(); }

	float MetresToWorld(float m) const;
	float WorldToMetres(float w) const;

	inline IVR* GetVR() const { return vr; }

	UINT backBufferWidth = 600;
	UINT backBufferHeight = 600;

	// HACK: Some places it is hard to get the delta time (e.g. updating the camera)
	// Using the last known delta time should be good enough
	float lastDeltaTime = 0.0f;

	bool bNeedsRecentre = true;
	bool bUseTwoHandAim = false;
	bool bLeftHanded = false;
	// Set by the HUD toggle gesture, when true the floating UI layer is not drawn
	bool bHideHUD = false;
	// TEMP TEST - grenade calibration stopwatch, see Game::UpdateInputs
	bool bGrenadeCalibrationActive = false;
	std::chrono::steady_clock::time_point grenadeCalibrationStart;
	// Runtime crosshair visibility, initialised from c_ShowCrosshair, toggled by the ToggleCrosshair binding
	bool bShowCrosshair = true;
	bool bUse3DOFAiming = false;

	Config config;
	bool bIsFiring = false;
	bool bIsReloading = false;

	InGameRenderer inGameRenderer;
	InGameRenderer scopeRenderer;
	UIRenderer* uiRenderer;
	SettingsMenu* settingsMenu;

	bool bDetectedChimera = false;
	Vector3 LastLookDir;
	WeaponHapticsConfigManager weaponHapticsConfig;

	bool bLoadedConfig = false;
	bool bSavedConfig = false;

	bool bIsCustom = false;

	UINT overlayWidth = 640;
	UINT overlayHeight = 640;

#if USE_PROFILER
	Profiler profiler;
#endif
protected:

	void CreateConsole();

	void PatchGame();

	void SetupConfigs();

	void CalcFPS(float deltaTime);
#if USE_PROFILER
	void DumpProfilerData();
#endif

	void UpdateCrosshairAndScope();
	void SetScopeTransform(Matrix4& newTransform, bool bIsVisible);

	void StoreRenderTargets();
	void RestoreRenderTargets();

	void CreateTextureAndSurface(UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format, struct IDirect3DSurface9** OutSurface, struct IDirect3DTexture9** OutTexture);

	WeaponHandler weaponHandler;
	InputHandler inputHandler;

	struct FPSTracker
	{
		float timeSinceFPSUpdate = 0.0f;
		int framesSinceFPSUpdate = 0;

		int fps = 0;
	} fpsTracker;

	FILE* consoleOut = nullptr;

	IVR* vr;

	RenderTarget gameRenderTargets[8];

	struct IDirect3DSurface9* uiSurface;
	struct IDirect3DSurface9* crosshairSurface;
	struct IDirect3DSurface9* uiRealSurface;
	struct IDirect3DSurface9* crosshairRealSurface;

	struct IDirect3DSurface9* scopeSurfaces[3];
	struct IDirect3DTexture9* scopeTextures[3];

	ERenderState renderState = ERenderState::UNKNOWN;

	CameraFrustum frustum1;
	CameraFrustum frustum2;

	short realZoom = -1;
	sRect realRect;
	sRect realLoadRect;
	UINT realUIWidth;
	UINT realUIHeight;

	DWORD realAlphaFunc;
	DWORD realAlphaSrc;
	DWORD realAlphaDest;

	bool bShowViewModel = false;

	bool bInVehicle = false;
	// Vehicle-to-foot camera exit blend state
	float vehicleExitBlendT = 0.0f;      // counts 1 -> 0 while the correction window is open
	bool bHasWeapon = true;

	ERenderState mirrorSource;

	bool bHasShutdown = true;

	bool bWasLoading = false;

	bool bIgnoreNextRoomScaleMovement = false;

	//======Configs======//
public:

	BoolProperty* c_ShowConsole = nullptr;
	BoolProperty* c_DrawMirror = nullptr;
	IntProperty* c_MirrorEye = nullptr;
	FloatProperty* c_CrosshairDistance = nullptr;
	FloatProperty* c_CrosshairScale = nullptr;
	FloatProperty* c_MenuOverlayDistance = nullptr;
	FloatProperty* c_UIOverlayDistance = nullptr;
	FloatProperty* c_UIOverlayScale = nullptr;
	FloatProperty* c_MenuOverlayScale = nullptr;
	FloatProperty* c_UIOverlayCurvature = nullptr;
	FloatProperty* c_UIOverlayRenderScale = nullptr;
	BoolProperty* c_ShowCrosshair = nullptr;
	BoolProperty* c_SnapTurn = nullptr;
	FloatProperty* c_SnapTurnAmount = nullptr;
	FloatProperty* c_SmoothTurnAmount = nullptr;
	BoolProperty* c_RoomScaleMovement = nullptr;
	IntProperty* c_HandRelativeMovement = nullptr;
	FloatProperty* c_HandRelativeOffsetRotation = nullptr;
	FloatProperty* c_HorizontalVehicleTurnAmount = nullptr;
	FloatProperty* c_VerticalVehicleTurnAmount = nullptr;
	BoolProperty* c_VehicleFaceAim = nullptr;
	BoolProperty* c_ShowWristHUD = nullptr;
	// WristHUDScale was replaced by per-element WristHUDAmmoScale/HealthScale/
	// RadarScale and removed entirely - kept as a comment rather than silently
	// vanishing from history, so a search for the old name finds an explanation
	FloatProperty* c_WristHUDAmmoScale = nullptr;
	FloatProperty* c_WristHUDHealthScale = nullptr;
	FloatProperty* c_WristHUDAmmoHeightStretch = nullptr;
	FloatProperty* c_WristHUDHealthHeightStretch = nullptr;
	FloatProperty* c_WristHUDRadarHeightStretch = nullptr;
	Vector3Property* c_WristHUDOffset = nullptr;
	Vector3Property* c_WristHUDRotation = nullptr;
	FloatProperty* c_WristHUDElementSpacing = nullptr;
	FloatProperty* c_WristHUDRadarScale = nullptr;
	BoolProperty* c_EnableLiveHUDAdjuster = nullptr;
	// Windows blocks foreground-window changes from a process that is not
	// already focused, which is exactly the case launching from Steam/Virtual
	// Desktop - a single attempt at init reliably loses that race. Retried for
	// a short window instead, see ForceGameWindowFocus() in Game.cpp.
	BoolProperty* c_ForceWindowFocus = nullptr;
	HWND gameWindow = nullptr;
	int focusAttemptsLeft = 0;
	void ForceGameWindowFocus();
	BoolProperty* c_HUDFollowsHeadPitch = nullptr;
	// Per-element fine position, on top of the shared WristHUDOffset/spacing
	Vector3Property* c_WristHUDAmmoOffset = nullptr;
	Vector3Property* c_WristHUDHealthOffset = nullptr;
	Vector3Property* c_WristHUDRadarOffset = nullptr;
	// Per-element rotation (degrees), applied on top of the shared group rotation
	// Per-element rotation (degrees): x = roll, y = pitch (tilt forward/back),
	// z = yaw (tilt left/right). Applied on top of the shared group rotation.
	Vector3Property* c_WristHUDAmmoRotation = nullptr;
	Vector3Property* c_WristHUDHealthRotation = nullptr;
	Vector3Property* c_WristHUDRadarRotation = nullptr;
	FloatProperty* c_WristHUDAmmoUMin = nullptr;
	FloatProperty* c_WristHUDAmmoVMin = nullptr;
	FloatProperty* c_WristHUDAmmoUMax = nullptr;
	FloatProperty* c_WristHUDAmmoVMax = nullptr;
	FloatProperty* c_WristHUDHealthUMin = nullptr;
	FloatProperty* c_WristHUDHealthVMin = nullptr;
	FloatProperty* c_WristHUDHealthUMax = nullptr;
	FloatProperty* c_WristHUDHealthVMax = nullptr;
	FloatProperty* c_WristHUDRadarUMin = nullptr;
	FloatProperty* c_WristHUDRadarVMin = nullptr;
	FloatProperty* c_WristHUDRadarUMax = nullptr;
	FloatProperty* c_WristHUDRadarVMax = nullptr;
	BoolProperty* c_DisableTwoHandForOneHanded = nullptr;
	BoolProperty* c_ThrowGrenadeOnRelease = nullptr;
	BoolProperty* c_ShowGrenadeArc = nullptr;
	FloatProperty* c_GrenadeArcSpeed = nullptr;
	FloatProperty* c_GrenadeArcGravity = nullptr;
	FloatProperty* c_GrenadeArcSeconds = nullptr;
	IntProperty* c_GrenadeArcSegments = nullptr;
	BoolProperty* c_GrenadeArcDashed = nullptr;
	FloatProperty* c_VehicleFaceAimBlend = nullptr;
	FloatProperty* c_VehicleFaceAimSmoothing = nullptr;
	FloatProperty* c_VehicleFaceAimSpeed = nullptr;
	BoolProperty* c_StabiliseCutsceneCamera = nullptr;
	FloatProperty* c_VehicleExitBlendDuration = nullptr;
	FloatProperty* c_VehicleExitBlendRate = nullptr;
	BoolProperty* c_OffhandHandFlashlight = nullptr;
	FloatProperty* c_LeftHandFlashlightDistance = nullptr;
	FloatProperty* c_RightHandFlashlightDistance = nullptr;
	FloatProperty* c_HUDToggleDistance = nullptr;
	Vector3Property* c_HUDToggleOffset = nullptr;
	StringProperty* c_HUDToggleSound = nullptr;
	BoolProperty* c_EnableWeaponHolsters = nullptr;
	FloatProperty* c_LeftShoulderHolsterActivationDistance = nullptr;
	Vector3Property* c_LeftShoulderHolsterOffset = nullptr;
	FloatProperty* c_RightShoulderHolsterActivationDistance = nullptr;
	Vector3Property* c_RightShoulderHolsterOffset = nullptr;
	Vector3Property* c_ControllerOffset = nullptr;
	Vector3Property* c_ControllerRotation = nullptr;
	FloatProperty* c_ScopeRenderScale = nullptr;
	FloatProperty* c_ScopeScale = nullptr;
	FloatProperty* c_ScopeInnerScaleVR = nullptr;
	FloatProperty* c_ScopeInnerScaleOriginal = nullptr;
	FloatProperty* c_ScopeDepth = nullptr;
	BoolProperty* c_LockScopeRoll = nullptr;
	Vector3Property* c_ScopeOffsetPistol = nullptr;
	Vector3Property* c_ScopeOffsetSniper = nullptr;
	Vector3Property* c_ScopeOffsetRocket = nullptr;
	FloatProperty* c_LeftHandMeleeSwingSpeed = nullptr;
	FloatProperty* c_RightHandMeleeSwingSpeed = nullptr;
	FloatProperty* c_CrouchHeight = nullptr;
	BoolProperty* c_ShowRoomCentre = nullptr;
	BoolProperty* c_ToggleGrip = nullptr;
	FloatProperty* c_TwoHandDistance = nullptr;
	BoolProperty* c_LeftHanded = nullptr;
	FloatProperty* c_SwapHandDistance = nullptr;
	StringProperty* c_d3d9Path = nullptr;
	FloatProperty* c_WeaponSmoothingAmountNoZoom = nullptr;
	FloatProperty* c_WeaponSmoothingAmountOneZoom = nullptr;
	FloatProperty* c_WeaponSmoothingAmountTwoZoom = nullptr;
	FloatProperty* c_TEMPViewportLeft = nullptr;
	FloatProperty* c_TEMPViewportRight = nullptr;
	FloatProperty* c_TEMPViewportTop = nullptr;
	FloatProperty* c_TEMPViewportBottom = nullptr;
	BoolProperty* c_Use3DOFAiming = nullptr;
	BoolProperty* c_UseOriginalScope = nullptr;
	Vector3Property* c_3DOFWeaponOffset = nullptr;
	FloatProperty* c_3DOFWeaponSmoothingAmount = nullptr;
	FloatProperty* c_3DOFScopeScale = nullptr;
};

