#include <chrono>
#include <cmath>
#include "Game.h"
#include "Logger.h"
#include "Hooking/Hooks.h"
#include "Helpers/DX9.h"
#include "Helpers/RenderTarget.h"
#include "Helpers/Renderer.h"
#include "Helpers/Camera.h"
#include "Helpers/Menus.h"
#include "Helpers/Objects.h"
#include "Helpers/Maths.h"

#ifdef EMULATE_VR
#include "VR/VREmulator.h"
#else
#include "VR/OpenVR.h"
#endif

#if USE_PROFILER
#include <algorithm>
#endif

#include "UI/UIRenderer.h"
#include "Helpers/Version.h"
#include "Helpers/Cutscene.h"

void Game::Init()
{
	Logger::log << "[Game] HaloCEVR initialising..." << std::endl;

#if USE_PROFILER
	profiler.Init();
#endif

	SetupConfigs();

	CreateConsole();

	PatchGame();

	Logger::log << "[Game] Found Game Type: " << Helpers::GetGameTypeString() << std::endl;
	Logger::log << "[Game] Found Game Version: " << Helpers::GetVersionString() << std::endl;

	bIsCustom = std::strcmp("halor", Helpers::GetGameTypeString()) != 0;

#ifdef EMULATE_VR
	vr = new VREmulator();
#else
	vr = new OpenVR();
#endif

	vr->Init();

	Game::instance.bLeftHanded = Game::instance.c_LeftHanded->Value();
	Game::instance.bUse3DOFAiming = Game::instance.c_Use3DOFAiming->Value();
	inputHandler.RegisterInputs();

	backBufferWidth = vr->GetViewWidth();
	backBufferHeight = vr->GetViewHeight();

	bHasShutdown = false;

	Logger::log << "[Game] HaloCEVR initialised" << std::endl;
}

void Game::Shutdown()
{
	if (bHasShutdown)
	{
		return;
	}
	bHasShutdown = true;

	Logger::log << "[Game] HaloCEVR shutting down..." << std::endl;

	vr->Shutdown();

#if USE_PROFILER
	profiler.Shutdown();
#endif

	MH_STATUS hookStatus = MH_DisableHook(MH_ALL_HOOKS);

	if (hookStatus != MH_OK)
	{
		Logger::log << "[Game] Could not remove hooks: " << MH_StatusToString(hookStatus) << std::endl;
	}

	hookStatus = MH_Uninitialize();

	if (hookStatus != MH_OK)
	{
		Logger::log << "[Game] Could not uninitialise MinHook: " << MH_StatusToString(hookStatus) << std::endl;
	}

	if (c_ShowConsole && c_ShowConsole->Value())
	{
		if (consoleOut)
		{
			fclose(consoleOut);
		}
		FreeConsole();
	}
}

void Game::OnInitDirectX()
{
	Logger::log << "[Game] Game has finished DirectX initialisation" << std::endl;

	if (!Helpers::GetDirect3DDevice9())
	{
		Logger::err << "Couldn't get game's direct3d device" << std::endl;
		return;
	}

	SetForegroundWindow(GetActiveWindow());

	// Ideally these values would be in a 4:3 ratio, but this causes the mouse position to stop aligning correctly
	overlayWidth = static_cast<UINT>(std::max(vr->GetViewHeight(), vr->GetViewWidth()) * c_UIOverlayRenderScale->Value());
	if (overlayWidth < 640) { // Clamp low to 640px so user can't degrade/break the config UI 
		overlayWidth = 640; 
	}
	overlayHeight = overlayWidth;

	vr->OnGameFinishInit();

	uiSurface = vr->GetUISurface();
	crosshairSurface = vr->GetCrosshairSurface();

	scopeSurfaces[0] = vr->GetScopeSurface();
	scopeTextures[0] = vr->GetScopeTexture();

	D3DSURFACE_DESC desc;
	scopeSurfaces[0]->GetDesc(&desc);

	CreateTextureAndSurface(desc.Width, desc.Height, desc.Usage, desc.Format, &scopeSurfaces[1], &scopeTextures[1]);
	CreateTextureAndSurface(desc.Width / 2, desc.Height / 2, desc.Usage, desc.Format, &scopeSurfaces[2], &scopeTextures[2]);

	uiRenderer = new UIRenderer();

	uiRenderer->Init(Helpers::GetDirect3DDevice9());

	settingsMenu = new SettingsMenu();

	settingsMenu->CreateMenus();
}

void Game::PreDrawFrame(struct Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(Game_PreDrawFrame);

	lastDeltaTime = deltaTime;

	renderState = ERenderState::UNKNOWN;

	bool bIsLoading = Helpers::IsCampaignLoading();

	if (bWasLoading && !bIsLoading)
	{
		bNeedsRecentre = true;
	}
	bWasLoading = bIsLoading;

	//CalcFPS(deltaTime);

	vr->SetMouseVisibility(Helpers::IsMouseVisible());
	vr->UpdatePoses();

	UpdateCrosshairAndScope();

	StoreRenderTargets();

	sRect* window = Helpers::GetWindowRect();
	window->top = 0;
	window->left = 0;
	window->right = vr->GetViewWidth();
	window->bottom = vr->GetViewHeight();

	VR_PROFILE_START(Game_PreDrawFrame_ClearSurfaces);
	// Clear UI surfaces
	IDirect3DSurface9* currentSurface = nullptr;
	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &currentSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, uiSurface);
	Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, crosshairSurface);
	Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, scopeSurfaces[0]);
	Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, currentSurface);
	currentSurface->Release();
	VR_PROFILE_STOP(Game_PreDrawFrame_ClearSurfaces);

	frustum1 = renderer->frustum;
	frustum2 = renderer->frustum2;

	if (bNeedsRecentre)
	{
		bNeedsRecentre = false;
		vr->Recentre();
	}

	UnitDynamicObject* Player = static_cast<UnitDynamicObject*>(Helpers::GetLocalPlayer());
	if (Player)
	{
		bool bNewShowViewModel = Player->parent.id != 0xffff;

		if (bNewShowViewModel != bShowViewModel)
		{
			// Self modifying code is the best code
			Hooks::P_KeepViewModelVisible(bNewShowViewModel);

			bShowViewModel = bNewShowViewModel;
		}
		// On leaving a vehicle the yawOffset accumulated by the vehicle camera is
		// stale for the on-foot reconciliation, which causes a camera whip.
		// Open a short correction window rather than correcting in a single frame
		// (which is itself a visible snap).
		if (bInVehicle && !bNewShowViewModel)
		{
			vehicleExitBlendT = 1.0f;
		}

		// While the window is open, continuously drive the LIVE HMD-vs-game yaw
		// residual towards zero. Recomputing every frame, rather than easing to a
		// target captured on the exit frame, means the correction keeps tracking
		// the engine's own third-to-first person exit camera while that is still
		// moving. A fixed target goes stale as soon as the engine camera moves,
		// which is what made the settle inconsistent.
		if (vehicleExitBlendT > 0.0f)
		{
			const float duration = c_VehicleExitBlendDuration
				? c_VehicleExitBlendDuration->Value() : 0.35f;
			const float rate = c_VehicleExitBlendRate
				? c_VehicleExitBlendRate->Value() : 8.0f;

			vehicleExitBlendT -= lastDeltaTime / (duration > 0.01f ? duration : 0.01f);
			if (vehicleExitBlendT < 0.0f)
			{
				vehicleExitBlendT = 0.0f;
			}

			IVR* vr = GetVR();
			Vector3 lookHMD = vr->GetHMDTransform().getLeftAxis();
			Vector3 lookGame = bDetectedChimera ? LastLookDir : Helpers::GetCamera().lookDir;
			float yawHMD = atan2(lookHMD.y, lookHMD.x);
			float yawGame = atan2(lookGame.y, lookGame.x);

			// yawOffset is in DEGREES (see SnapTurnAmount / SmoothTurnAmount usage),
			// but atan2 returns RADIANS, so the residual must be converted.
			const float RadToDeg = 180.0f / 3.141593f;
			float residual = (yawHMD - yawGame) * RadToDeg;

			// Always correct the short way around. Without this, exiting while
			// facing near the +/-180 degree boundary sends the view the long way
			// round, which is why the wobble depended on which way you were facing.
			while (residual > 180.0f)
			{
				residual -= 360.0f;
			}
			while (residual < -180.0f)
			{
				residual += 360.0f;
			}

			// Approach the correction a fraction at a time: strong while the error
			// is large, easing off as it converges. Scaled by frame time so the
			// settle behaves the same at any framerate.
			float alpha = rate * lastDeltaTime;
			if (alpha > 1.0f)
			{
				alpha = 1.0f;
			}

			vr->SetYawOffset(vr->GetYawOffset() + residual * alpha);
		}

		bInVehicle = bNewShowViewModel;
		bHasWeapon = Player->weapon.id != 0xffff;
	}

	DrawGrenadeArc();

	UpdateGrenadeVelocityScan();

	if (c_ShowRoomCentre->Value())
	{
		VR_PROFILE_SCOPE(Game_PreDrawFrame_DrawRoomCentre);

		Vector3 position = Helpers::GetCamera().position;
		position.z -= 0.62f;
		Vector3 upVector(0.0f, 0.0f, 1.0f);
		Vector3 forwardVector(1.0f, 0.0f, 0.0f);

		inGameRenderer.DrawPolygon(position, upVector, forwardVector, 8, MetresToWorld(0.25f), D3DCOLOR_ARGB(50, 85, 250, 239), false);
	}

#if 0
	// Debug draw controller position (why, oh why, does steamvr make getting a consistent controller position/orientation so hard?)
	Matrix4 controller = vr->GetControllerTransform(ControllerRole::Right);

	Matrix3 handRotation3;

	for (int i = 0; i < 3; i++)
	{
		handRotation3.setColumn(i, &controller.get()[i * 4]);
	}

	Vector3 worldPos = Helpers::GetCamera().position;

	inGameRenderer.DrawCoordinate(controller * Vector3(0.0f, 0.0f, 0.0f) * MetresToWorld(1.0f) + worldPos, handRotation3, 0.05f, false);

	for (int i = 0; i < 31; i++)
	{
		Matrix4 bone = vr->GetControllerBoneTransform(ControllerRole::Right, i);

		for (int i = 0; i < 3; i++)
		{
			handRotation3.setColumn(i, &bone.get()[i * 4]);
		}

		inGameRenderer.DrawCoordinate(bone * Vector3(0.0f, 0.0f, 0.0f) * MetresToWorld(1.0f) + worldPos, handRotation3, i == 1 ? 0.05f : 0.005f, false);
	}

	controller = vr->GetControllerTransform(ControllerRole::Right);

	for (int i = 0; i < 3; i++)
	{
		handRotation3.setColumn(i, &controller.get()[i * 4]);
	}

	inGameRenderer.DrawCoordinate(controller * Vector3(0.0f, 0.0f, 0.0f) * MetresToWorld(1.0f) + worldPos, handRotation3, 0.05f, false);
#endif

	vr->PreDrawFrame(renderer, deltaTime);
}

void Game::PreDrawEye(Renderer* renderer, float deltaTime, int eye)
{
	VR_PROFILE_SCOPE(Game_PreDrawEye);

	renderState = eye == 0 ? ERenderState::LEFT_EYE : ERenderState::RIGHT_EYE;

	renderer->frustum = frustum1;
	renderer->frustum2 = frustum2;

	vr->UpdateCameraFrustum(&renderer->frustum, eye);
	vr->UpdateCameraFrustum(&renderer->frustum2, eye);

	RenderTarget* primaryRenderTarget = Helpers::GetRenderTargets();

	primaryRenderTarget[0].renderSurface = vr->GetRenderSurface(eye);
	primaryRenderTarget[0].renderTexture = vr->GetRenderTexture(eye);
	primaryRenderTarget[0].width = vr->GetViewWidth();
	primaryRenderTarget[0].height = vr->GetViewHeight();

	inGameRenderer.ExtractMatrices(renderer);
}


void Game::PostDrawEye(struct Renderer* renderer, float deltaTime, int eye)
{
	VR_PROFILE_SCOPE(Game_PostDrawEye);

	if (Helpers::IsLoading() || Helpers::IsCampaignLoading())
	{
		Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	}

	inGameRenderer.Render(Helpers::GetDirect3DDevice9());
}

bool Game::PreDrawScope(Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(Game_PreDrawScope);

	UnitDynamicObject* player = static_cast<UnitDynamicObject*>(Helpers::GetLocalPlayer());

	if (!player || player->zoom == -1)
	{
		return false;
	}

	renderState = ERenderState::SCOPE;

	renderer->frustum = frustum1;
	renderer->frustum2 = frustum2;

	Vector3 aimPos, aimDir, upDir;

	weaponHandler.GetWorldWeaponAim(aimPos, aimDir, upDir);

	if (!Game::instance.c_LockScopeRoll->Value())
	{
		upDir = Vector3(0.0f, 0.0f, 1.0f);
		upDir = aimDir.cross(upDir);
		upDir = upDir.cross(aimDir);
	}

	renderer->frustum.position = aimPos;
	renderer->frustum2.position = aimPos;
	renderer->frustum.facingDirection = aimDir;
	renderer->frustum2.facingDirection = aimDir;
	renderer->frustum.upDirection = upDir;
	renderer->frustum2.upDirection = upDir;

	RenderTarget* primaryRenderTarget = Helpers::GetRenderTargets();

	primaryRenderTarget[0].renderSurface = scopeSurfaces[0];
	primaryRenderTarget[0].renderTexture = scopeTextures[0];
	primaryRenderTarget[0].width = vr->GetScopeWidth();
	primaryRenderTarget[0].height = vr->GetScopeHeight();
	primaryRenderTarget[1].renderSurface = scopeSurfaces[1];
	primaryRenderTarget[1].renderTexture = scopeTextures[1];
	primaryRenderTarget[1].width = vr->GetScopeWidth();
	primaryRenderTarget[1].height = vr->GetScopeHeight();
	primaryRenderTarget[2].renderSurface = scopeSurfaces[2];
	primaryRenderTarget[2].renderTexture = scopeTextures[2];
	primaryRenderTarget[2].width = vr->GetScopeWidth() / 2;
	primaryRenderTarget[2].height = vr->GetScopeHeight() / 2;


	sRect* windowMain = Helpers::GetWindowRect();
	windowMain->top = 0;
	windowMain->left = 0;
	windowMain->right = vr->GetScopeWidth();
	windowMain->bottom = vr->GetScopeHeight();

	{
		sRect& window = renderer->frustum.WindowViewport;
		window.top = 0;
		window.left = 0;
		window.right = vr->GetScopeWidth();
		window.bottom = vr->GetScopeHeight();

		sRect& window2 = renderer->frustum2.WindowViewport;
		window2.top = 0;
		window2.left = 0;
		window2.right = vr->GetScopeWidth();
		window2.bottom = vr->GetScopeHeight();
	}

	return true;
}

void Game::PostDrawScope(Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(Game_PostDrawScope);

	RestoreRenderTargets();

	Vector2 innerSize = Vector2(0.0f, 0.0f);
	Vector2 size = Vector2(static_cast<float>(vr->GetScopeWidth()), static_cast<float>(vr->GetScopeHeight()));
	Vector2 centre = size / 2;
	int sides = 32;
	float radius = size.y * 0.25f;
	D3DCOLOR color = D3DCOLOR_ARGB(255, 0, 0, 0);

	scopeRenderer.ExtractMatrices(renderer);

	// Sniper scope is a rounded square, so we need to separate the quadrants and change the radius
	if (weaponHandler.IsSniperScope())
	{
		radius = size.y * 0.03125f;
		const float scopeWidth = 0.605f * size.x - radius * 2.0f;
		const float scopeHeight = 0.505f * size.y - radius * 2.0f;
		innerSize = Vector2(scopeWidth, scopeHeight);
	}

	// For original scope mode we want to show more of the original scope graphics
	if (!ShouldUseOriginalScope())
	{
		scopeRenderer.DrawInvertedShape2D(centre, innerSize, size, sides, radius, color);
	}

	scopeRenderer.Render(Helpers::GetDirect3DDevice9());
	scopeRenderer.PostRender();

}

void Game::PreDrawMirror(struct Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(Game_PreDrawMirror);

	renderState = ERenderState::GAME;

	renderer->frustum = frustum1;
	renderer->frustum2 = frustum2;

	RestoreRenderTargets();

	sRect* windowMain = Helpers::GetWindowRect();
	windowMain->top = 0;
	windowMain->left = 0;
	windowMain->right = Helpers::GetRenderTargets()[0].width;
	windowMain->bottom = Helpers::GetRenderTargets()[0].height;

	inGameRenderer.ExtractMatrices(renderer);
}

void Game::PostDrawMirror(struct Renderer* renderer, float deltaTime)
{
	VR_PROFILE_SCOPE(Game_PostDrawMirror);

	// Do something here to copy the image into the backbuffer correctly

	inGameRenderer.ClearRenderTargets();
	inGameRenderer.Render(Helpers::GetDirect3DDevice9());
}

void Game::PostDrawFrame(struct Renderer* renderer, float deltaTime)
{
	VR_PROFILE_START(Game_PostDrawFrame);

	RestoreRenderTargets();
	vr->PostDrawFrame(renderer, deltaTime);
	inGameRenderer.PostRender();

	sRect* windowMain = Helpers::GetWindowRect();
	windowMain->top = 0;
	windowMain->left = 0;
	windowMain->right = Helpers::GetRenderTargets()[0].width;
	windowMain->bottom = Helpers::GetRenderTargets()[0].height;

	if (c_DrawMirror->Value() && mirrorSource != ERenderState::GAME)
	{
		VR_PROFILE_SCOPE(Game_PostDrawFrame_Mirror);

		int sWidth = vr->GetViewWidth();
		int sHeight = vr->GetViewHeight();

		float sourceAspect = static_cast<float>(sWidth) / static_cast<float>(sHeight);

		int dWidth = Helpers::GetRenderTargets()[0].width;
		int dHeight = Helpers::GetRenderTargets()[0].height;

		int trueWidth = 640;
		int trueHeight = 480;

		float destAspect = static_cast<float>(trueWidth) / static_cast<float>(trueHeight);

		RECT destRect{};

		if (sourceAspect > destAspect)
		{
			destRect.left = 0;
			destRect.right = dWidth;

			float scale = destAspect / sourceAspect;

			int scaledSize = static_cast<int>(0.5f * (1.0f - scale) * dHeight);

			destRect.top = scaledSize;
			destRect.bottom = dHeight - scaledSize;
		}
		else
		{
			destRect.top = 0;
			destRect.bottom = dHeight;

			float scale = sourceAspect / destAspect;

			int scaledSize = static_cast<int>(0.5f * (1.0f - scale) * dWidth);

			destRect.left = scaledSize;
			destRect.right = dWidth - scaledSize;
		}


		int eye = mirrorSource == ERenderState::LEFT_EYE ? 0 : 1;

		IDirect3DSurface9* currentSurface = nullptr;
		Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &currentSurface);
		Helpers::GetDirect3DDevice9()->SetRenderTarget(0, Helpers::GetRenderTargets()[0].renderSurface);
		Helpers::GetDirect3DDevice9()->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
		Helpers::GetDirect3DDevice9()->SetRenderTarget(0, currentSurface);
		currentSurface->Release();
		Helpers::GetDirect3DDevice9()->StretchRect(vr->GetRenderSurface(eye), nullptr, Helpers::GetRenderTargets()[0].renderSurface, &destRect, D3DTEXF_LINEAR);
	}

	VR_PROFILE_STOP(Game_PostDrawFrame);

#if USE_PROFILER
	profiler.NewFrame();
#endif
}

void Game::DrawGrenadeArc()
{
	// Tied to the crosshair toggle so the arc counts as an "aimer" alongside the
	// world crosshair: crosshair off means a fully clean view with no aim aids,
	// crosshair on brings back both the reticle and the grenade arc together.
	if (!c_ShowGrenadeArc->Value() || !bShowCrosshair || !inputHandler.IsGrenadeHeld() || !weaponHandler.HasAnyGrenades())
	{
		return;
	}

	Vector3 startPos;
	Vector3 aim;
	if (!weaponHandler.GetGrenadeThrowPose(startPos, aim))
	{
		return;
	}

	int segments = c_GrenadeArcSegments->Value();
	if (segments < 2)
	{
		segments = 2;
	}

	const float totalTime = c_GrenadeArcSeconds->Value();
	const float dt = totalTime / static_cast<float>(segments);

	// Simple ballistic integration. The launch speed and gravity are assumed values
	// rather than read from the game's own projectile data, so they are exposed as
	// config and tuned by eye against where grenades actually land.
	Vector3 velocity = aim * MetresToWorld(c_GrenadeArcSpeed->Value());
	const float gravityPerStep = MetresToWorld(c_GrenadeArcGravity->Value()) * dt;

	Vector3 pos = startPos;

	// Dashed line: each step alternates between a drawn segment and a gap. Both the
	// dash and the gap shrink along the arc's length, so the pattern reads as dense
	// near the hand and sparse further away, similar to teleport arc visuals.
	const bool bDashed = c_GrenadeArcDashed->Value();
	bool bDrawThisStep = true;
	float dashPhaseAccum = 0.0f;

	for (int i = 0; i < segments; i++)
	{
		Vector3 nextPos = pos + velocity * dt;
		velocity.z -= gravityPerStep;

		// Fade the arc out along its length so the far, least reliable end is faintest
		const float t = static_cast<float>(i) / static_cast<float>(segments);
		const int alpha = static_cast<int>(200.0f * (1.0f - t)) + 40;

		bool bDraw = true;

		if (bDashed)
		{
			// Dash/gap length in "segment units" shrinks from ~2.5 segments near the
			// hand down to ~0.6 segments at the far end
			const float dashLength = 2.5f - 1.9f * t;

			dashPhaseAccum += 1.0f;
			if (dashPhaseAccum >= dashLength)
			{
				dashPhaseAccum -= dashLength;
				bDrawThisStep = !bDrawThisStep;
			}

			bDraw = bDrawThisStep;
		}

		if (bDraw)
		{
			// Depth respecting was tried (occlude the arc against level geometry so
			// a throw that would hit a wall visibly stops there) but the depth test
			// did not correspond correctly to the real depth buffer: the ground
			// consistently hid the whole arc regardless of a small or even a large
			// (50cm) bias towards the camera, and a large bias caused visible
			// artifacts near the hand instead of fixing anything. This points to a
			// deeper mismatch (likely the view/projection matrices used for this
			// draw not exactly matching what the world was actually rendered with)
			// that needs live D3D9 debugging to diagnose properly, not something
			// fixable by tuning a bias constant. Reverted to always-visible, which
			// is reliable even though it draws through geometry.
			inGameRenderer.DrawLine3D(pos, nextPos, D3DCOLOR_ARGB(alpha, 90, 220, 255), false, 0.02f);
		}

		pos = nextPos;
	}
}

void Game::UpdateGrenadeVelocityScan()
{
	weaponHandler.UpdateGrenadeVelocityScan();
}

bool Game::IsCurrentWeaponOneHanded() const
{
	return weaponHandler.IsCurrentWeaponOneHanded();
}

bool Game::PreDrawHUD()
{
	VR_PROFILE_SCOPE(Game_PreDrawHUD);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// Remove zoom effect from game view
		if (GetRenderState() == ERenderState::GAME)
		{
			short* zoom = &Helpers::GetInputData().zoomLevel;
			realZoom = *zoom;
			*zoom = -1;
		}

		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME || GetRenderState() == ERenderState::SCOPE;
	}

	short* zoom = &Helpers::GetInputData().zoomLevel;
	realZoom = *zoom;
	*zoom = -1;

	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &uiRealSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, uiSurface);
	uiRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = uiSurface;
	realUIWidth = Helpers::GetRenderTargets()[1].width;
	realUIHeight = Helpers::GetRenderTargets()[1].height;
	Helpers::GetRenderTargets()[1].width = overlayWidth;
	Helpers::GetRenderTargets()[1].height = overlayHeight;

	sRect* windowMain = Helpers::GetCurrentRect();
	realRect = *windowMain;

	windowMain->top = 0;
	windowMain->left = 0;
	windowMain->right = overlayWidth;
	windowMain->bottom = overlayHeight;

	return true;
}

void Game::PostDrawHUD()
{
	VR_PROFILE_SCOPE(Game_PostDrawHUD);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// Remove zoom effect from game view
		if (GetRenderState() == ERenderState::GAME)
		{
			short* zoom = &Helpers::GetInputData().zoomLevel;
			*zoom = realZoom;
		}

		return;
	}

	short* zoom = &Helpers::GetInputData().zoomLevel;
	*zoom = realZoom;

	sRect* windowMain = Helpers::GetCurrentRect();
	*windowMain = realRect;

	Helpers::GetRenderTargets()[1].width = realUIWidth;
	Helpers::GetRenderTargets()[1].height = realUIHeight;

	Helpers::GetRenderTargets()[1].renderSurface = uiRealSurface;
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, uiRealSurface);

}

bool Game::PreDrawMenu()
{
	VR_PROFILE_SCOPE(Game_PreDrawMenu);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME;
	}

	Helpers::GetDirect3DDevice9()->GetRenderTarget(0, &uiRealSurface);
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, uiSurface);
	uiRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = uiSurface;

	return true;
}

void Game::PostDrawMenu()
{
	VR_PROFILE_SCOPE(Game_PostDrawMenu);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		return;
	}

	if (Helpers::IsMouseVisible())
	{
		uiRenderer->Render();
	}

	Helpers::GetRenderTargets()[1].renderSurface = uiRealSurface;
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, uiRealSurface);
}

D3DVIEWPORT9 currentViewport;
bool Game::PreDrawLoading(int param1, struct Renderer* renderer)
{
	VR_PROFILE_SCOPE(Game_PreDrawLoading);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		// ...but try to avoid breaking the game view (for now at least)
		return GetRenderState() == ERenderState::GAME;
	}

	uiRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	Helpers::GetRenderTargets()[1].renderSurface = uiSurface;

	return true;
}

void Game::PostDrawLoading(int param1, struct Renderer* renderer)
{
	VR_PROFILE_SCOPE(Game_PostDrawLoading);

	// Only render UI once per frame
	if (GetRenderState() != ERenderState::LEFT_EYE)
	{
		return;
	}

	Helpers::GetRenderTargets()[1].renderSurface = uiRealSurface;
}

bool Game::PreDrawCrosshair(short* anchorLocation)
{
	VR_PROFILE_SCOPE(Game_PreDrawCrosshair);

	// Draw things normally for the scope
	if (GetRenderState() == ERenderState::SCOPE)
	{
		return true;
	}

	// The crosshair has been toggled off, so skip drawing the world crosshair.
	// The scope reticle is handled above and is unaffected.
	if (!bShowCrosshair && anchorLocation && *anchorLocation == 4) // Centre = 4
	{
		return false;
	}

	crosshairRealSurface = Helpers::GetRenderTargets()[1].renderSurface;
	if (anchorLocation && *anchorLocation == 4) // Centre = 4
	{
		if (realZoom != -1)
		{
			return false;
		}
		Helpers::GetRenderTargets()[1].renderSurface = crosshairSurface;
		Helpers::GetDirect3DDevice9()->SetRenderTarget(0, crosshairSurface);
	}

	return true;
}

void Game::PostDrawCrosshair()
{
	VR_PROFILE_SCOPE(Game_PostDrawCrosshair);

	// Draw things normally for the scope
	if (GetRenderState() == ERenderState::SCOPE)
	{
		return;
	}

	Helpers::GetRenderTargets()[1].renderSurface = crosshairRealSurface;
	Helpers::GetDirect3DDevice9()->SetRenderTarget(0, crosshairRealSurface);
}

void Game::PreDrawImage(void* param1, void* param2)
{
	VR_PROFILE_SCOPE(Game_PreDrawImage);

	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_ALPHAFUNC, &realAlphaFunc);
	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_SRCBLENDALPHA, &realAlphaSrc);
	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_DESTBLENDALPHA, &realAlphaDest);

	//Logger::log << realAlphaFunc << ", " << realAlphaSrc << ", " << realAlphaDest << std::endl;

	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_ALPHAFUNC, D3DBLENDOP_ADD);
	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_SRCALPHA);
	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
}

void Game::PostDrawImage(void* param1, void* param2)
{
	VR_PROFILE_SCOPE(Game_PostDrawImage);

	DWORD tmp1, tmp2, tmp3;

	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_ALPHAFUNC, &tmp1);
	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_SRCBLENDALPHA, &tmp2);
	Helpers::GetDirect3DDevice9()->GetRenderState(D3DRS_DESTBLENDALPHA, &tmp3);

	//Logger::log << tmp1 << ", " << tmp2 << ", " << tmp3 << std::endl;

	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_ALPHAFUNC, realAlphaFunc);
	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_SRCBLENDALPHA, realAlphaSrc);
	Helpers::GetDirect3DDevice9()->SetRenderState(D3DRS_DESTBLENDALPHA, realAlphaDest);
}

void Game::UpdateViewModel(HaloID& id, Vector3* pos, Vector3* facing, Vector3* up, TransformQuat* BoneTransforms, Transform* OutBoneTransforms)
{
	VR_PROFILE_SCOPE(Game_UpdateViewModel);
	weaponHandler.UpdateViewModel(id, pos, facing, up, BoneTransforms, OutBoneTransforms);

	if (Game::instance.bIsFiring)
	{
		weaponHandler.SetPlasmaPistolCharge();
	}

	weaponHandler.HandlePlasmaPistolCharge();
}

void Game::PreFireWeapon(HaloID& weaponID, short param2)
{
	VR_PROFILE_SCOPE(Game_PreFireWeapon);
	weaponHandler.PreFireWeapon(weaponID, param2);
}

void Game::PostFireWeapon(HaloID& weaponID, short param2)
{
	VR_PROFILE_SCOPE(Game_PostFireWeapon);
	weaponHandler.PostFireWeapon(weaponID, param2);
}

void Game::PreThrowGrenade(HaloID& playerID)
{
	VR_PROFILE_SCOPE(Game_PreThrowGrenade);
	weaponHandler.PreThrowGrenade(playerID);

	// TEMP TEST - grenade calibration: start the stopwatch on a real local
	// player throw, and log the launch angle at the same instant. Any throw
	// angle works now - the tag data confirms initial velocity is a single
	// fixed speed, only direction varies with aim, so we no longer need a
	// specifically vertical throw. Press F7 the instant the grenade LANDS
	// (not when it detonates - that's a separate fixed timer) to log flight
	// time, giving an (angle, time) pair per throw.
	{
		HaloID localPlayerID;
		if (Helpers::GetLocalPlayerID(localPlayerID) && localPlayerID == playerID)
		{
			Vector3 throwPos, throwAim;
			if (weaponHandler.GetGrenadeThrowPose(throwPos, throwAim))
			{
				// Local axes are (forward, left, up), aim is normalised, so pitch
				// above horizontal is asin of the up component
				double pitchDegrees = std::asin(std::max(-1.0f, std::min(1.0f, throwAim.z))) * (180.0 / 3.14159265358979);
				Logger::log << "[GrenadeCalibration] Throw detected, angle=" << pitchDegrees
					<< " degrees above horizontal. Timer started, press F7 when it lands." << std::endl;
			}

			bGrenadeCalibrationActive = true;
			grenadeCalibrationStart = std::chrono::steady_clock::now();
		}
	}
}

void Game::PostThrowGrenade(HaloID& playerID)
{
	VR_PROFILE_SCOPE(Game_PostThrowGrenade);
	weaponHandler.PostThrowGrenade(playerID);
}

// Live in-headset HUD placement adjuster.
//
// Positioning VR elements by editing config and relaunching is extremely slow -
// you cannot see what you are changing while you change it. This lets the values
// be nudged with the keyboard while looking at the result, then saved back to
// config.txt so nothing is lost on restart.
//
//   F1        cycle which property is being adjusted
//   F2 / F3   decrease / increase the selected property's first axis
//   F4 / F6   decrease / increase the second axis (where the property has one)
//             (F5 is deliberately avoided - it is the profiler dump key)
//   F8        cycle the step size (fine 0.01 / medium 0.05 / coarse 0.20)
//   F9        save current values to VR/config.txt
//   F10       log all current wrist HUD values to the log file
//
// Every action logs what it did, so the log doubles as a record of what was
// tried, and nothing has to be memorised while wearing a headset.
void Game::UpdateLiveHUDAdjuster()
{
	if (!c_EnableLiveHUDAdjuster->Value())
	{
		return;
	}

	auto keyJustPressed = [](int vk, bool& wasDown) -> bool
	{
		bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool justPressed = isDown && !wasDown;
		wasDown = isDown;
		return justPressed;
	};

	static bool bF1 = false, bF2 = false, bF3 = false, bF4 = false;
	static bool bF6 = false, bF8 = false, bF9 = false, bF10 = false;

	// F2/F3 = first axis, F4/F6 = second axis, cycling through:
	//   0/1: radar position, then radar scale
	//   2/3: health position, then health scale
	//   4/5: ammo position, then ammo scale
	//   6:   the whole group's position (left-right / forward-back)
	//   7:   the whole group's tilt (roll / a second tilt axis)
	//   8:   the whole group's height and yaw (up-down / rotate left-right)
	const char* targetNames[] = {
		"Radar position: F2/F3 left-right, F4/F6 forward-back",
		"Radar scale: F2/F3 size, F4/F6 height stretch",
		"Health position: F2/F3 left-right, F4/F6 forward-back",
		"Health scale: F2/F3 size, F4/F6 height stretch",
		"Ammo position: F2/F3 left-right, F4/F6 forward-back",
		"Ammo scale: F2/F3 size, F4/F6 height stretch",
		"Group position: F2/F3 left-right, F4/F6 forward-back",
		"Group tilt: F2/F3 roll, F4/F6 second tilt axis",
		"Group: F2/F3 up-down, F4/F6 rotate left-right",
	};
	const int targetCount = 9;

	if (keyJustPressed(VK_F1, bF1))
	{
		liveAdjustTarget = (liveAdjustTarget + 1) % targetCount;
		Logger::log << "[HUDAdjust] Now adjusting: " << targetNames[liveAdjustTarget]
			<< " (step " << liveAdjustStep << ")" << std::endl;
	}

	if (keyJustPressed(VK_F8, bF8))
	{
		if (liveAdjustStep < 0.02f) { liveAdjustStep = 0.05f; }
		else if (liveAdjustStep < 0.1f) { liveAdjustStep = 0.20f; }
		else { liveAdjustStep = 0.01f; }
		Logger::log << "[HUDAdjust] Step size now " << liveAdjustStep << std::endl;
	}

	float axis1 = 0.0f;
	float axis2 = 0.0f;
	if (keyJustPressed(VK_F2, bF2)) { axis1 -= liveAdjustStep; }
	if (keyJustPressed(VK_F3, bF3)) { axis1 += liveAdjustStep; }
	if (keyJustPressed(VK_F4, bF4)) { axis2 -= liveAdjustStep; }
	if (keyJustPressed(VK_F6, bF6)) { axis2 += liveAdjustStep; }

	if (axis1 != 0.0f || axis2 != 0.0f)
	{
		// Mod convention: offset.x = forward, offset.y = left, offset.z = up.
		// axis1 (F2/F3) moves left-right, axis2 (F4/F6) moves forward-back,
		// except targets 4/5 where the mapping is noted per case below.
		switch (liveAdjustTarget)
		{
		case 0: // Radar, position
		{
			Vector3 v = c_WristHUDRadarOffset->Value();
			v.y += axis1;
			v.x += axis2;
			c_WristHUDRadarOffset->SetValue(v);
			Logger::log << "[HUDAdjust] WristHUDRadarOffset = (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
			break;
		}
		case 1: // Radar, scale (size / height stretch)
		{
			float sizeV = c_WristHUDRadarScale->Value() + axis1;
			if (sizeV < 0.01f) { sizeV = 0.01f; }
			c_WristHUDRadarScale->SetValue(sizeV);

			float stretchV = c_WristHUDRadarHeightStretch->Value() + axis2;
			if (stretchV < 0.1f) { stretchV = 0.1f; }
			c_WristHUDRadarHeightStretch->SetValue(stretchV);

			Logger::log << "[HUDAdjust] WristHUDRadarScale = " << sizeV
				<< ", WristHUDRadarHeightStretch = " << stretchV << std::endl;
			break;
		}
		case 2: // Health, position
		{
			Vector3 v = c_WristHUDHealthOffset->Value();
			v.y += axis1;
			v.x += axis2;
			c_WristHUDHealthOffset->SetValue(v);
			Logger::log << "[HUDAdjust] WristHUDHealthOffset = (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
			break;
		}
		case 3: // Health, scale (size / height stretch)
		{
			float sizeV = c_WristHUDHealthScale->Value() + axis1;
			if (sizeV < 0.01f) { sizeV = 0.01f; }
			c_WristHUDHealthScale->SetValue(sizeV);

			float stretchV = c_WristHUDHealthHeightStretch->Value() + axis2;
			if (stretchV < 0.1f) { stretchV = 0.1f; }
			c_WristHUDHealthHeightStretch->SetValue(stretchV);

			Logger::log << "[HUDAdjust] WristHUDHealthScale = " << sizeV
				<< ", WristHUDHealthHeightStretch = " << stretchV << std::endl;
			break;
		}
		case 4: // Ammo, position
		{
			Vector3 v = c_WristHUDAmmoOffset->Value();
			v.y += axis1;
			v.x += axis2;
			c_WristHUDAmmoOffset->SetValue(v);
			Logger::log << "[HUDAdjust] WristHUDAmmoOffset = (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
			break;
		}
		case 5: // Ammo, scale (size / height stretch)
		{
			float sizeV = c_WristHUDAmmoScale->Value() + axis1;
			if (sizeV < 0.01f) { sizeV = 0.01f; }
			c_WristHUDAmmoScale->SetValue(sizeV);

			float stretchV = c_WristHUDAmmoHeightStretch->Value() + axis2;
			if (stretchV < 0.1f) { stretchV = 0.1f; }
			c_WristHUDAmmoHeightStretch->SetValue(stretchV);

			Logger::log << "[HUDAdjust] WristHUDAmmoScale = " << sizeV
				<< ", WristHUDAmmoHeightStretch = " << stretchV << std::endl;
			break;
		}
		case 6: // Whole group, position (left-right / forward-back)
		{
			Vector3 v = c_WristHUDOffset->Value();
			v.y += axis1;
			v.x += axis2;
			c_WristHUDOffset->SetValue(v);
			Logger::log << "[HUDAdjust] WristHUDOffset = (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
			break;
		}
		case 7: // Whole group, tilt (roll / second tilt axis). Degrees, bigger step.
		{
			Vector3 v = c_WristHUDRotation->Value();
			v.x += axis1 * 100.0f;
			v.y += axis2 * 100.0f;
			c_WristHUDRotation->SetValue(v);
			Logger::log << "[HUDAdjust] WristHUDRotation = (" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
			break;
		}
		case 8: // Whole group, up-down (offset.z) and yaw (rotation.z)
		{
			Vector3 v = c_WristHUDOffset->Value();
			v.z += axis1;
			c_WristHUDOffset->SetValue(v);

			Vector3 r = c_WristHUDRotation->Value();
			r.z += axis2 * 100.0f;
			c_WristHUDRotation->SetValue(r);

			Logger::log << "[HUDAdjust] WristHUDOffset.z = " << v.z
				<< ", WristHUDRotation.z = " << r.z << std::endl;
			break;
		}
		default:
			break;
		}
	}

	if (keyJustPressed(VK_F10, bF10))
	{
		Vector3 off = c_WristHUDOffset->Value();
		Vector3 rot = c_WristHUDRotation->Value();
		Logger::log << "[HUDAdjust] ---- current values ----" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDOffset = (" << off.x << ", " << off.y << ", " << off.z << ")" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDRotation = (" << rot.x << ", " << rot.y << ", " << rot.z << ")" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDElementSpacing = " << c_WristHUDElementSpacing->Value() << std::endl;
		Vector3 ammoOff = c_WristHUDAmmoOffset->Value();
		Vector3 healthOff = c_WristHUDHealthOffset->Value();
		Vector3 radarOff = c_WristHUDRadarOffset->Value();
		Logger::log << "[HUDAdjust] WristHUDAmmoOffset = (" << ammoOff.x << ", " << ammoOff.y << ", " << ammoOff.z << ")" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDHealthOffset = (" << healthOff.x << ", " << healthOff.y << ", " << healthOff.z << ")" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDRadarOffset = (" << radarOff.x << ", " << radarOff.y << ", " << radarOff.z << ")" << std::endl;
		Logger::log << "[HUDAdjust] WristHUDAmmoScale = " << c_WristHUDAmmoScale->Value()
			<< ", HeightStretch = " << c_WristHUDAmmoHeightStretch->Value() << std::endl;
		Logger::log << "[HUDAdjust] WristHUDHealthScale = " << c_WristHUDHealthScale->Value()
			<< ", HeightStretch = " << c_WristHUDHealthHeightStretch->Value() << std::endl;
		Logger::log << "[HUDAdjust] WristHUDRadarScale = " << c_WristHUDRadarScale->Value()
			<< ", HeightStretch = " << c_WristHUDRadarHeightStretch->Value() << std::endl;
	}

	if (keyJustPressed(VK_F9, bF9))
	{
		if (config.SaveToFile("VR/config.txt"))
		{
			Logger::log << "[HUDAdjust] Saved current values to VR/config.txt" << std::endl;
		}
		else
		{
			Logger::err << "[HUDAdjust] FAILED to save VR/config.txt" << std::endl;
		}
	}
}

void Game::UpdateInputs()
{
	VR_PROFILE_SCOPE(Game_UpdateInputs);

	UpdateLiveHUDAdjuster();

	// Hot reload this flag
	bUse3DOFAiming = c_Use3DOFAiming->Value();

	inputHandler.UpdateInputs(bInVehicle);

	UpdateRoomScaleMovement();

#if USE_PROFILER
	static bool bWasPressed = false;

	bool bPressed = GetAsyncKeyState(VK_F5) & 0x8000;

	if (bPressed && !bWasPressed)
	{
		DumpProfilerData();
	}

	bWasPressed = bPressed;
#endif

	// TEMP TEST - grenade calibration stopwatch stop key
	{
		static bool bWasF7Pressed = false;
		bool bF7Pressed = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		if (bF7Pressed && !bWasF7Pressed && bGrenadeCalibrationActive)
		{
			auto elapsed = std::chrono::steady_clock::now() - grenadeCalibrationStart;
			double seconds = std::chrono::duration<double>(elapsed).count();
			Logger::log << "[GrenadeCalibration] Flight time: " << seconds << " seconds" << std::endl;
			bGrenadeCalibrationActive = false;
		}
		bWasF7Pressed = bF7Pressed;
	}
}

void Game::CalculateSmoothedInput()
{
	VR_PROFILE_SCOPE(Game_CalculateSmoothedInput);
	inputHandler.CalculateSmoothedInput();
}

void Game::UpdateRoomScaleMovement()
{
	VR_PROFILE_SCOPE(Game_UpdateRoomScaleMovement);

	if (!c_RoomScaleMovement->Value())
	{
		return;
	}

	UnitDynamicObject* Player = static_cast<UnitDynamicObject*>(Helpers::GetLocalPlayer());

	const bool bNoPlayer = Player == nullptr;
	const bool bInCutscene = Helpers::GetCutsceneData()->bInCutscene;

	// Restrict roomscale movement to normal movement (there's probably an elegant way to determine if the player can move, but I haven't RE'd it)
	if (bNoPlayer || bInCutscene || bInVehicle)
	{
		bIgnoreNextRoomScaleMovement = true;
		return;
	}

	// Calculate real-world offset from last frame
	Vector3 headPos = vr->GetHMDTransform(true) * Vector3(0.0f, 0.0f, 0.0f);

	// Convert to game units
	Vector3 desiredOffset = headPos * MetresToWorld(1.0f);
	desiredOffset.z = 0.0f;

	const float Rotation = vr->GetYawOffset() * (3.141593f / 180.0f);

#if 0
	inGameRenderer.DrawPolygon(Player->position + desiredOffset, Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f), 4, MetresToWorld(0.25f), D3DCOLOR_ARGB(50, 85, 250, 239), false);
#endif

	// Directly adjust position, collisions are handled later in the tick
	if (!bIgnoreNextRoomScaleMovement)
	{
		Player->position += desiredOffset;
	}
	bIgnoreNextRoomScaleMovement = false;

	{
		// Rotate desiredOffset by yaw offset
		const float cosAngle = std::cos(Rotation);
		const float sinAngle = std::sin(Rotation);
		const float newX = desiredOffset.x * cosAngle - desiredOffset.y * sinAngle;
		const float newY = desiredOffset.x * sinAngle + desiredOffset.y * cosAngle;
		desiredOffset.x = newX;
		desiredOffset.y = newY;
	}

	// Move the camera offset backwards to recentre the player
	vr->SetLocationOffset(desiredOffset * WorldToMetres(1.0f) + vr->GetLocationOffset());
}

bool Game::GetCalculatedHandPositions(Matrix4& controllerTransform, Vector3& dominantHandPos, Vector3& offHand)
{
	VR_PROFILE_SCOPE(Game_GetCalculatedHandPositions);
	return inputHandler.GetCalculatedHandPositions(controllerTransform, dominantHandPos, offHand);
}

void Game::ReloadStart(HaloID param1, short param2, bool param3)
{
	VR_PROFILE_SCOPE(Game_ReloadStart);

	WeaponDynamicObject* weaponObject = static_cast<WeaponDynamicObject*>((Helpers::GetDynamicObject(param1)));

	HaloID playerID{};
	Helpers::GetLocalPlayerID(playerID);

	if (!weaponObject || weaponObject->parent != playerID)
	{
		return;
	}

	Weapon& weapon = weaponObject->weaponData[param2];

	// Reload function gets called whenever the player tries to reload, if reloadstate is 1 then a reload was actually triggered
	if (weapon.reloadState == 1)
	{
		bIsReloading = true;
		//Logger::log << "Reload Start (" << param1 << ", " << param2 << ", " << param3 << ")" << std::endl;
	}
}

void Game::ReloadEnd(short param1, HaloID param2)
{
	VR_PROFILE_SCOPE(Game_ReloadEnd);

	WeaponDynamicObject* weaponObject = static_cast<WeaponDynamicObject*>((Helpers::GetDynamicObject(param2)));

	HaloID playerID{};
	Helpers::GetLocalPlayerID(playerID);

	if (!weaponObject || weaponObject->parent != playerID)
	{
		return;
	}

	bIsReloading = false;
	//Logger::log << "Reload End" << std::endl;
}

Vector3 Game::GetSmoothedInput() const
{
	return inputHandler.smoothedPosition;
}

void Game::UpdateCamera(float& yaw, float& pitch)
{
	VR_PROFILE_SCOPE(Game_UpdateCamera);
	// Don't bother simulating inputs if we aren't actually in vr
#ifdef EMULATE_VR
	return;
#endif

	// During a cutscene the cinematic script is driving the game camera. Injecting
	// our own corrections towards the headset fights that scripted motion, which
	// shows up as the view oscillating back and forth. Leave the camera to the
	// script; the headset still controls where the player looks within the view.
	if (c_StabiliseCutsceneCamera && c_StabiliseCutsceneCamera->Value()
		&& Helpers::GetCutsceneData()->bInCutscene)
	{
		yaw = 0.0f;
		pitch = 0.0f;
		return;
	}

	if (bInVehicle && !bHasWeapon)
	{
		inputHandler.UpdateCameraForVehicles(yaw, pitch);
	}
	else
	{
		inputHandler.UpdateCamera(yaw, pitch);
	}
}

void Game::SetMousePosition(int& x, int& y)
{
	VR_PROFILE_SCOPE(Game_SetMousePosition);
	// Don't bother simulating inputs if we aren't actually in vr
#ifndef EMULATE_VR
	inputHandler.SetMousePosition(x, y);
#endif
	if (Helpers::IsMouseVisible())
	{
		uiRenderer->MoveCursor(static_cast<float>(x), static_cast<float>(y));
#ifndef EMULATE_VR
		// Stop menu hover events happening while ui is up
		if (settingsMenu->bVisible)
		{
			x = 0;
			y = 0;
		}
#endif
	}
}

void Game::UpdateMouseInfo(MouseInfo* mouseInfo)
{
	VR_PROFILE_SCOPE(Game_UpdateMouseInfo);

	static char realLeftClickValue = 0;

	// Don't bother simulating inputs if we aren't actually in vr
#ifndef EMULATE_VR
	inputHandler.UpdateMouseInfo(mouseInfo);
#endif
	if (Helpers::IsMouseVisible() && mouseInfo->buttonState[0] == 1)
	{
		uiRenderer->Click();

#ifndef EMULATE_VR
		// Don't allow any mouse inputs to pass through to the real UI while ours is up
		if (settingsMenu->bVisible)
		{
			mouseInfo->buttonState[0] = 0;
		}
#endif
	}

	// Hack fix for closing settings menu if the player exits a menu with it open
	if (!Helpers::IsMouseVisible() && settingsMenu->bVisible)
	{
		uiRenderer->MoveCursor(0.0f, 0.0f);
		uiRenderer->Click();
	}
}

void Game::SetViewportScale(Viewport* viewport)
{
	VR_PROFILE_SCOPE(Game_SetViewportScale);
	// This appears to be broken, the code this is overriding does something very weird with the scaling
	/*
	viewport->left = -1.0f;
	viewport->right = 1.0f;
	viewport->bottom = 1.0f;
	viewport->top = -1.0f;
	*/
	viewport->left = c_TEMPViewportLeft->Value();
	viewport->right = c_TEMPViewportRight->Value();
	viewport->top = c_TEMPViewportTop->Value();
	viewport->bottom = c_TEMPViewportBottom->Value();
}

float Game::MetresToWorld(float m) const
{
	return m / 3.048f;
}

float Game::WorldToMetres(float w) const
{
	return w * 3.048f;
}

void Game::CreateConsole()
{
	if (!c_ShowConsole || !c_ShowConsole->Value())
	{
		return;
	}

	AllocConsole();
	freopen_s(&consoleOut, "CONOUT$", "w", stdout);
	std::cout.clear();
}

void Game::PatchGame()
{
	VR_PROFILE_SCOPE(Game_PatchGame);
	MH_STATUS hookStatus;

	if ((hookStatus = MH_Initialize()) != MH_OK)
	{
		Logger::err << "Could not initialise MinHook: " << MH_StatusToString(hookStatus) << std::endl;
	}
	else
	{
		Logger::log << "[Game] Creating hooks" << std::endl;
		Hooks::InitHooks();
		Logger::log << "[Game] Enabling hooks" << std::endl;
		Hooks::EnableAllHooks();
	}
}

void Game::SetupConfigs()
{
	VR_PROFILE_SCOPE(Game_SetupConfigs);

	// Window settings
	c_ShowConsole = config.RegisterBool("ShowConsole", "Create a console window at launch for debugging purposes", false);
	c_DrawMirror = config.RegisterBool("DrawMirror", "Update the desktop window display to show the current game view, rather than leaving it on the splash screen", true);
	c_MirrorEye = config.RegisterInt("MirrorEye", "Index of the eye to use for the mirror view  (0 = left, 1 = right)", 0);
	// UI settings
	c_CrosshairDistance = config.RegisterFloat("CrosshairDistance", "Distance in metres in front of the weapon to display the crosshair", 15.0f);
	c_MenuOverlayDistance = config.RegisterFloat("MenuOverlayDistance", "Distance in metres in front of the HMD to display the menu", 15.0f);
	c_UIOverlayDistance = config.RegisterFloat("UIOverlayDistance", "Distance in metres in front of the HMD to display the UI", 15.0f);
	c_UIOverlayScale = config.RegisterFloat("UIOverlayScale", "Width of the UI overlay in metres", 10.0f);
	c_MenuOverlayScale = config.RegisterFloat("MenuOverlayScale", "Width of the menu overlay in metres", 10.0f);
	c_CrosshairScale = config.RegisterFloat("CrosshairScale", "Width of the crosshair overlay in metres", 10.0f);
	c_UIOverlayCurvature = config.RegisterFloat("UIOverlayCurvature", "Curvature of the UI Overlay, on a scale of 0 to 1", 0.1f);
	c_UIOverlayRenderScale = config.RegisterFloat("UIOverlayRenderScale", "Resolution of the UI overlay, expressed as a proportion of the headset's render scale (e.g. 0.5 = half resolution), low values default to 640px", 0.5f);
	c_ShowCrosshair = config.RegisterBool("ShowCrosshair", "Display a floating crosshair in the world at the location you are aiming", true);
	bShowCrosshair = c_ShowCrosshair->Value();
	// Control settings
	c_LeftHanded = config.RegisterBool("LeftHanded", "Make the left hand the dominant hand by default. This swaps the button bindings but doesn't swap the sticks. Left handed bindings with the sticks swapped can be found in the SteamVR overlay", false);
	c_SnapTurn = config.RegisterBool("SnapTurn", "The look input will instantly rotate the view by a fixed amount, rather than smoothly rotating", true);
	c_SnapTurnAmount = config.RegisterFloat("SnapTurnAmount", "Rotation in degrees a single snap turn will rotate the view by", 45.0f);
	c_SmoothTurnAmount = config.RegisterFloat("SmoothTurnAmount", "Rotation in degrees per second the view will turn at when not using snap turning", 90.0f);
	c_RoomScaleMovement = config.RegisterBool("RoomScaleMovement", "Attempt to move the character to always match the headset's position. (May cause motion sickness, as collisions can cause a desync between physical and in-game movements)", true);
	c_HandRelativeMovement = config.RegisterInt("HandRelativeMovement", "Movement is relative to hand orientation, rather than head, 0 = off, 1 = left, 2 = right", 0);
	c_HandRelativeOffsetRotation = config.RegisterFloat("HandRelativeOffsetRotation", "Hand direction rotational offset in degrees used for hand-relative movement", -20.0f);
	c_HorizontalVehicleTurnAmount = config.RegisterFloat("HorizontalVehicleTurnAmount", "Rotation in degrees per second the view will turn horizontally when in vehicles (<0 to invert)", 90.0f);
	c_VerticalVehicleTurnAmount = config.RegisterFloat("VerticalVehicleTurnAmount", "Rotation in degrees per second the view will turn vertically when in vehicles (<0 to invert)", 45.0f);
	c_VehicleFaceAim = config.RegisterBool("VehicleFaceAim", "EXPERIMENTAL. When true, vehicle aiming tracks where your head is looking, blended with the stick. Off by default", false);
	c_ShowWristHUD = config.RegisterBool("ShowWristHUD", "CALIBRATION STEP. Clones the entire HUD texture onto the off hand wrist, visible only when raising the hand to look at it, so the real element layout can be inspected before building a cropped final version. Off by default", false);
	c_WristHUDScale = config.RegisterFloat("WristHUDScale", "Width in metres of the wrist HUD overlay", 0.15f);
	c_WristHUDAmmoScale = config.RegisterFloat("WristHUDAmmoScale", "Width in metres of just the ammo element", 0.15f);
	c_WristHUDHealthScale = config.RegisterFloat("WristHUDHealthScale", "Width in metres of just the health element", 0.15f);
	c_WristHUDAmmoHeightStretch = config.RegisterFloat("WristHUDAmmoHeightStretch", "Stretches the ammo element taller (>1) or shorter (<1) independently of its width", 1.0f);
	c_WristHUDHealthHeightStretch = config.RegisterFloat("WristHUDHealthHeightStretch", "Stretches the health element taller (>1) or shorter (<1) independently of its width", 1.0f);
	c_WristHUDRadarHeightStretch = config.RegisterFloat("WristHUDRadarHeightStretch", "Stretches the radar element taller (>1) or shorter (<1) independently of its width", 1.0f);
	c_WristHUDOffset = config.RegisterVector3("WristHUDOffset", "The (forward, left, up) offset of the wrist HUD relative to the off hand controller", Vector3(0.0f, 0.0f, 0.05f));
	c_WristHUDRotation = config.RegisterVector3("WristHUDRotation", "Rotation in degrees (X, Y, Z, same convention as ControllerRotation) applied to the wrist HUD so it faces you correctly. Needs tuning per controller", Vector3(0.0f, 0.0f, 0.0f));
	c_WristHUDElementSpacing = config.RegisterFloat("WristHUDElementSpacing", "Vertical gap in metres between the three stacked wrist HUD elements", 0.04f);
	c_WristHUDRadarScale = config.RegisterFloat("WristHUDRadarScale", "Width in metres of just the radar element. The radar crop is closer to square than ammo/health, so at the same width it renders much taller and can overlap health - kept separate so it can be sized down independently", 0.08f);
	c_EnableLiveHUDAdjuster = config.RegisterBool("EnableLiveHUDAdjuster", "Enables keyboard hotkeys (F1 select, F2/F3 and F4/F6 adjust, F8 step size, F9 save to config, F10 log values) for tuning wrist HUD placement live in the headset instead of editing config and relaunching", false);
	c_WristHUDAmmoOffset = config.RegisterVector3("WristHUDAmmoOffset", "Fine (forward, left, up) position of just the ammo element, on top of the shared WristHUDOffset", Vector3(0.0f, 0.0f, 0.0f));
	c_WristHUDHealthOffset = config.RegisterVector3("WristHUDHealthOffset", "Fine (forward, left, up) position of just the health element, on top of the shared WristHUDOffset", Vector3(0.0f, 0.0f, 0.0f));
	c_WristHUDRadarOffset = config.RegisterVector3("WristHUDRadarOffset", "Fine (forward, left, up) position of just the radar element, on top of the shared WristHUDOffset", Vector3(0.0f, 0.0f, 0.0f));
	// Starting guesses based on a screenshot of the whole cloned texture, each
	// crop is a fraction (0-1) of the underlying 640x640 render target. Needs
	// visual tuning: adjust one edge at a time and compare against what's
	// actually visible in the headset.
	c_WristHUDAmmoUMin = config.RegisterFloat("WristHUDAmmoUMin", "Left edge (0-1) of the ammo crop from the wrist HUD texture", 0.0f);
	c_WristHUDAmmoVMin = config.RegisterFloat("WristHUDAmmoVMin", "Top edge (0-1) of the ammo crop from the wrist HUD texture", 0.35f);
	c_WristHUDAmmoUMax = config.RegisterFloat("WristHUDAmmoUMax", "Right edge (0-1) of the ammo crop from the wrist HUD texture", 0.45f);
	c_WristHUDAmmoVMax = config.RegisterFloat("WristHUDAmmoVMax", "Bottom edge (0-1) of the ammo crop from the wrist HUD texture", 0.55f);
	c_WristHUDHealthUMin = config.RegisterFloat("WristHUDHealthUMin", "Left edge (0-1) of the health crop from the wrist HUD texture", 0.5f);
	c_WristHUDHealthVMin = config.RegisterFloat("WristHUDHealthVMin", "Top edge (0-1) of the health crop from the wrist HUD texture", 0.35f);
	c_WristHUDHealthUMax = config.RegisterFloat("WristHUDHealthUMax", "Right edge (0-1) of the health crop from the wrist HUD texture", 0.9f);
	c_WristHUDHealthVMax = config.RegisterFloat("WristHUDHealthVMax", "Bottom edge (0-1) of the health crop from the wrist HUD texture", 0.5f);
	c_WristHUDRadarUMin = config.RegisterFloat("WristHUDRadarUMin", "Left edge (0-1) of the radar crop from the wrist HUD texture", 0.0f);
	c_WristHUDRadarVMin = config.RegisterFloat("WristHUDRadarVMin", "Top edge (0-1) of the radar crop from the wrist HUD texture", 0.6f);
	c_WristHUDRadarUMax = config.RegisterFloat("WristHUDRadarUMax", "Right edge (0-1) of the radar crop from the wrist HUD texture", 0.3f);
	c_WristHUDRadarVMax = config.RegisterFloat("WristHUDRadarVMax", "Bottom edge (0-1) of the radar crop from the wrist HUD texture", 0.85f);
	c_DisableTwoHandForOneHanded = config.RegisterBool("DisableTwoHandForOneHanded", "Prevent the two hand grip from activating while holding a one handed weapon (pistol, plasma pistol, plasma rifle or needler), which has no real two handed hold", true);
	c_ThrowGrenadeOnRelease = config.RegisterBool("ThrowGrenadeOnRelease", "Throw the grenade when the grenade button is released, rather than immediately when pressed. Lets you hold the button while winding up the throw motion", false);
	c_ShowGrenadeArc = config.RegisterBool("ShowGrenadeArc", "Draw a predicted trajectory arc from your throwing hand while the grenade button is held", false);
	c_GrenadeArcSpeed = config.RegisterFloat("GrenadeArcSpeed", "Grenade launch speed in metres per second for the predicted arc. Measured via two independent in-game timing tests (26.49 and 26.60 m/s, agreeing within 0.4%), rather than guessed", 26.5f);
	c_GrenadeArcGravity = config.RegisterFloat("GrenadeArcGravity", "Assumed gravity in metres per second squared for the predicted arc. Tune alongside GrenadeArcSpeed", 9.8f);
	c_GrenadeArcSeconds = config.RegisterFloat("GrenadeArcSeconds", "How many seconds of flight the predicted arc covers", 2.5f);
	c_GrenadeArcSegments = config.RegisterInt("GrenadeArcSegments", "Number of line segments used to draw the predicted arc. Higher is smoother", 40);
	c_GrenadeArcDashed = config.RegisterBool("GrenadeArcDashed", "Draw the grenade arc as a dashed line, with dashes shrinking towards the far end, rather than a solid line", true);
	c_VehicleFaceAimBlend = config.RegisterFloat("VehicleFaceAimBlend", "How much head-aim vs stick contributes in vehicles (0 = pure stick, 1 = pure head aim)", 0.8f);
	c_VehicleFaceAimSmoothing = config.RegisterFloat("VehicleFaceAimSmoothing", "Smoothing applied to vehicle head-aim (0 = instant, 0.5 = moderate, 0.9 = heavy lag)", 0.4f);
	c_VehicleFaceAimSpeed = config.RegisterFloat("VehicleFaceAimSpeed", "How quickly vehicle head-aim follows your head. Higher is faster/snappier", 7.0f);
	c_VehicleExitBlendDuration = config.RegisterFloat("VehicleExitBlendDuration", "How long in seconds the camera correction runs for after leaving a vehicle", 0.35f);
	c_VehicleExitBlendRate = config.RegisterFloat("VehicleExitBlendRate", "How strongly the camera is corrected after leaving a vehicle. Higher settles faster but is more abrupt", 8.0f);
	c_StabiliseCutsceneCamera = config.RegisterBool("StabiliseCutsceneCamera", "EXPERIMENTAL. Stop injecting VR camera corrections during cutscenes. The cinematic script drives the camera, so correcting towards the headset fights it and can cause the view to oscillate", false);
	c_ToggleGrip = config.RegisterBool("ToggleGrip", "When true releasing two handed weapons requires pressing the grip action again", false);
	c_TwoHandDistance = config.RegisterFloat("TwoHandDistance", "Maximum distance between both hands where the off hand grip action will enable two handed aiming (<0 for any distance)", 0.8f);
	c_SwapHandDistance = config.RegisterFloat("SwapHandDistance", "Maximum distance between both hands where the swap weapon hand grip action will swap your weapon into the opposite hand (<0 to disable)", 0.2f);
	c_OffhandHandFlashlight = config.RegisterBool("OffhandHandFlashlight", "Use your offhand for toggling the flashlight, your offhand hand is the hand not holding a weapon", true);
	c_LeftHandFlashlightDistance = config.RegisterFloat("LeftHandFlashlight", "Bringing the left hand within this distance of the head will toggle the flashlight (<0 to disable)", 0.2f);
	c_RightHandFlashlightDistance = config.RegisterFloat("RightHandFlashlight", "Bringing the right hand within this distance of the head will toggle the flashlight (<0 to disable)", 0.2f);
	c_HUDToggleDistance = config.RegisterFloat("HUDToggleDistance", "Bringing the hand opposite the flashlight hand within this distance of the HUD toggle zone will show/hide the floating HUD (<0 to disable)", -1.0f);
	c_HUDToggleOffset = config.RegisterVector3("HUDToggleOffset", "The (forward, left, up) Offset of the HUD toggle zone relative to the headset's location. Mirrored horizontally to sit beside the hand used for the toggle", Vector3(-0.05f, -0.18f, 0.0f));
	c_HUDToggleSound = config.RegisterString("HUDToggleSound", "Filename of a 16-bit PCM .wav inside the VR folder to play when the HUD is toggled (blank to disable)", "");
	c_LeftHandMeleeSwingSpeed = config.RegisterFloat("LeftHandMeleeSwingSpeed", "Minimum vertical velocity of left hand required to initiate a melee attack in m/s (<0 to disable)", 2.5f);
	c_RightHandMeleeSwingSpeed = config.RegisterFloat("RightHandMeleeSwingSpeed", "Minimum vertical velocity of right hand required to initiate a melee attack in m/s (<0 to disable)", 2.5f);
	c_CrouchHeight = config.RegisterFloat("CrouchHeight", "Minimum height to duck by in metres to automatically trigger the crouch input in game (<0 to disable)", 0.15f);
	// Hand settings
	c_ControllerOffset = config.RegisterVector3("ControllerOffset", "Offset from the controller's position used when calculating the in game hand position", Vector3(0.0f, 0.0f, 0.0f));
	c_ControllerRotation = config.RegisterVector3("ControllerRotation", "Rotation added to the controller when calculating the in game hand rotation", Vector3(0.0f, 0.0f, 0.0f));
	c_ScopeRenderScale = config.RegisterFloat("ScopeRenderScale", "Size of the scope render target, expressed as a proportion of the headset's render scale (e.g. 0.5 = half resolution)", 1.0f);
	c_ScopeScale = config.RegisterFloat("ScopeScale", "Width of the scope view in metres (6DOF mode)", 0.05f);	
	c_LockScopeRoll = config.RegisterBool("LockScopeRoll", "Set to true to keep the horizon level at all times in scopes. Leaving as false causes the scope view to rotate with the gun (pre-v1.3.0 behaviour)", true);
	c_UseOriginalScope = config.RegisterBool("UseOriginalScope", "Use original Halo scope graphics instead of VR weapon-attached scope. Automatically enabled in 3DOF mode.", false);
	c_ScopeOffsetPistol = config.RegisterVector3("ScopeOffsetPistol", "Offset of the scope view relative to the pistol's location", Vector3(-0.1f, 0.0f, 0.15f));
	c_ScopeOffsetSniper = config.RegisterVector3("ScopeOffsetSniper", "Offset of the scope view relative to the pistol's location", Vector3(-0.15f, 0.0f, 0.15f));
	c_ScopeOffsetRocket = config.RegisterVector3("ScopeOffsetRocket", "Offset of the scope view relative to the pistol's location", Vector3(0.1f, 0.2f, 0.1f));
	c_WeaponSmoothingAmountNoZoom = config.RegisterFloat("UnzoomedWeaponSmoothingAmount", "Amount of smoothing applied to weapon movement when not zoomed in (0 is disabled, 2.0 is maximum, recommended around 0-0.7)", 0.0f);
	c_WeaponSmoothingAmountOneZoom = config.RegisterFloat("Zoom1WeaponSmoothingAmount", "Amount of smoothing applied to weapon movement when zoomed in once, eg zooming on the pistol (0 is disabled, 2.0 is maximum, recommended around 0.3-1.0)", 0.4f);
	c_WeaponSmoothingAmountTwoZoom = config.RegisterFloat("Zoom2WeaponSmoothingAmount", "Amount of smoothing applied to weapon movement when zoomed in twice, eg second zoom on sniper (0 is disabled, 2.0 is maximum, recommended around 0.6-1.25)", 0.6f);
	c_Use3DOFAiming = config.RegisterBool("Use3DOFAiming", "Use controller rotation only for aiming (3DOF) instead of position+rotation (6DOF). When enabled, weapon models follow hand position but bullets fire based on controller rotation only", false);
	c_3DOFWeaponOffset = config.RegisterVector3("3DOFWeaponOffset", "This is a cosmetic setting for the position offset for the 3DOF weapon model. This has no impact on gameplay. (right, forward, up in metres). Use negative Z to lower the weapon", Vector3(0.0f, 0.0f, -0.08f));
	c_3DOFWeaponSmoothingAmount = config.RegisterFloat("3DOFWeaponSmoothingAmount", "This is a cosmetic setting that controls the amount of smoothing applied to 3DOF weapon facing direction. This has no impact on gameplay. (0 is disabled, 2.0 is maximum, default is 1.5)", 1.5f);
	c_3DOFScopeScale = config.RegisterFloat("3DOFScopeScale", "Width of the scope view in metres (3DOF mode)", 7.f);
	// Weapon holster settings
	c_EnableWeaponHolsters = config.RegisterBool("EnableWeaponHolsters", "When enabled Weapons can only be switched by using the 'SwitchWeapons' binding while the dominant hand is within distance of a holster", true);
	c_LeftShoulderHolsterActivationDistance = config.RegisterFloat("LeftShoulderHolsterDistance", "The 'size' of the left shoulder holster. This is the distance that the dominant hand needs to be from the holster to change weapons (<0 to disable)", 0.3f);
	c_LeftShoulderHolsterOffset = config.RegisterVector3("LeftShoulderHolsterOffset", "The (foward, left, up) Offset of the left shoulder holster relative to the headset's location", Vector3(-0.15f, 0.25f, -0.25f));
	c_RightShoulderHolsterActivationDistance = config.RegisterFloat("RightShoulderHolsterDistance", "The 'size' of the right shoulder holster. This is the distance that the dominant hand needs to be from the holster to change weapons (<0 to disable)", 0.3f);
	c_RightShoulderHolsterOffset = config.RegisterVector3("RightShoulderHolsterOffset", "The (foward, left, up) Offset of the right shoulder holster relative to the headset's location", Vector3(-0.15f, -0.25f, -0.25f));
	// Misc settings
	c_ShowRoomCentre = config.RegisterBool("ShowRoomCentre", "Draw an indicator at your feet to show where the player character is actually positioned", true);
	c_d3d9Path = config.RegisterString("CustomD3D9Path", "If set first try to load d3d9.dll from the specified path instead of from system32", "");
	c_TEMPViewportLeft = config.RegisterFloat("TEMP_ViewportLeft", "Some headsets experience warping when turning, as a workaround the viewport scaling has been exposed so users can adjust them until the warping stops", -1.0f);
	c_TEMPViewportRight = config.RegisterFloat("TEMP_ViewportRight", "Some headsets experience warping when turning, as a workaround the viewport scaling has been exposed so users can adjust them until the warping stops", 1.0f);
	c_TEMPViewportTop = config.RegisterFloat("TEMP_ViewportTop", "Some headsets experience warping when turning, as a workaround the viewport scaling has been exposed so users can adjust them until the warping stops", -1.0f);
	c_TEMPViewportBottom = config.RegisterFloat("TEMP_ViewportBottom", "Some headsets experience warping when turning, as a workaround the viewport scaling has been exposed so users can adjust them until the warping stops", 1.0f);

	bLoadedConfig = config.LoadFromFile("VR/config.txt");
	bSavedConfig = config.SaveToFile("VR/config.txt");

	if (!bLoadedConfig)
	{
		Logger::log << "[Config] First time startup, generating default config file" << std::endl;
	}

	if (!bSavedConfig)
	{
		Logger::log << "[Config] Couldn't save config file, halo is likely running as non-administrator from a protected directory" << std::endl;
	}

	// First run, but couldn't create config file
	if (!bLoadedConfig && !bSavedConfig)
	{
		Logger::err << "Could not create /VR/config.txt.\nHalo may have been installed in Program Files, to generate the config file either run halo.exe as an administrator or reinstall the game in a non-protected folder (e.g. Documents)." << std::endl;
	}

	weaponHandler.localOffset = Vector3(c_ControllerOffset->Value().x, c_ControllerOffset->Value().y, c_ControllerOffset->Value().z);
	weaponHandler.localRotation = Vector3(c_ControllerRotation->Value().x, c_ControllerRotation->Value().y, c_ControllerRotation->Value().z);

	if (c_MirrorEye->Value() == 0)
	{
		mirrorSource = ERenderState::LEFT_EYE;
	}
	else if (c_MirrorEye->Value() == 1)
	{
		mirrorSource = ERenderState::RIGHT_EYE;
	}
	else if (c_MirrorEye->Value() == 2)
	{
		Logger::log << "[Config] MirrorEye set to 'game'. This is intended for debugging, may not look correct, and will likely impact performance" << std::endl;
		mirrorSource = ERenderState::GAME;
	}
	else
	{
		Logger::log << "[Config] Invalid value for MirrorEye, defaulting to left eye" << std::endl;
		mirrorSource = ERenderState::LEFT_EYE;
	}

	WeaponHapticsConfigManager weaponHapticsConfig;

	//Logger::log << "[Config] Loaded configs" << std::endl;
}

void Game::CalcFPS(float deltaTime)
{
	fpsTracker.framesSinceFPSUpdate++;
	fpsTracker.timeSinceFPSUpdate += deltaTime;

	if (fpsTracker.timeSinceFPSUpdate > 1.0f)
	{
		fpsTracker.timeSinceFPSUpdate = 0.0f;
		fpsTracker.fps = fpsTracker.framesSinceFPSUpdate;
		fpsTracker.framesSinceFPSUpdate = 0;
		Logger::log << fpsTracker.fps << std::endl;
	}
}

#if USE_PROFILER
void Game::DumpProfilerData()
{
	VR_PROFILE_SCOPE(Game_DumpProfilerData);

	// TODO: Create a better system for viewing this data (e.g. real time display/proper dedicated profile files)
	std::vector<Profiler::FrameTimings*> frameTimings;
	Game::instance.profiler.GetTimings(frameTimings);

	Logger::log << "[Profiler] Dumping last 30 seconds of profiler data..." << std::endl;

	float minFrame = FLT_MAX;
	float maxFrame = -FLT_MAX;
	float totalFrame = 0.0f;
	int numFrames = 0;

	Profiler::time_point now = std::chrono::high_resolution_clock::now();

	std::unordered_map<std::string, Profiler::Timings> totalTimes;

	for (auto it = frameTimings.rbegin(); it != frameTimings.rend(); ++it)
	{
		float ago = std::chrono::duration<float, std::milli>(now - (*it)->frameEnd).count();
		if (ago > 30.0f * 1000.0f)
		{
			break;
		}

		float duration = std::chrono::duration<float, std::milli>((*it)->frameEnd - (*it)->frameStart).count();

		minFrame = (std::min)(minFrame, duration);
		maxFrame = (std::max)(maxFrame, duration);
		totalFrame += duration;
		numFrames++;

		for (auto& kv : (*it)->timings)
		{
			const std::string& eventName = kv.first;
			const Profiler::Timings* timings = kv.second;
			/*
			Logger::log << "[Profiler] "
				<< eventName
				<< ": calls " << timings->numHits
				<< " min. " << timings->minTime
				<< " avg. " << timings->totalTime / timings->numHits
				<< " max. " << timings->maxTime
				<< std::endl;
			*/

			totalTimes[eventName].minTime = (std::min)(totalTimes[eventName].minTime, timings->minTime);
			totalTimes[eventName].maxTime = (std::max)(totalTimes[eventName].maxTime, timings->maxTime);
			totalTimes[eventName].numHits += timings->numHits;
			totalTimes[eventName].totalTime += timings->totalTime;
		}
	}

	Logger::log << "[Profiler] Frame times: min. " << minFrame << " avg. " << (totalFrame / numFrames) << " max. " << maxFrame << std::endl;
	Logger::log << "[Profiler] Total Event times: " << std::endl;

	std::vector<std::pair<std::string, Profiler::Timings>> topTimings;
	for (auto& kv : totalTimes)
	{
		topTimings.push_back(kv);
	}

	std::sort(topTimings.begin(), topTimings.end(), [](const auto& a, const auto& b)
		{
			return a.second.totalTime > b.second.totalTime;
		}
	);

	for (auto& kv : topTimings)
	{
		Logger::log << "[Profiler] "
			<< kv.first
			<< ": calls " << kv.second.numHits
			<< " min. " << kv.second.minTime
			<< " avg. " << kv.second.totalTime / kv.second.numHits
			<< " max. " << kv.second.maxTime
			<< std::endl;
	}
}
#endif

void Game::UpdateCrosshairAndScope()
{
	VR_PROFILE_SCOPE(Game_UpdateCrosshairAndScope);

	auto fixupRotation = [](Matrix4& m, Vector3& pos) {
		m.translate(-pos);
		m.rotate(90.0f, m.getUpAxis());
		m.rotate(-90.0f, m.getLeftAxis());
		m.translate(pos);
		};

	Vector3 aimPos, aimDir, upDir;

	if (bInVehicle && !bHasWeapon)
	{
		aimPos = Vector3();
		aimDir = Helpers::GetCamera().lookDir;
	}
	else
	{
		bool bHasCrosshair = weaponHandler.GetLocalWeaponAim(aimPos, aimDir, upDir);

		if (!bHasCrosshair)
		{
			return;
		}
	}

	Matrix4 overlayTransform;

	Vector3 hmdPos = vr->GetHMDTransform(true) * Vector3(0.0f, 0.0f, 0.0f);

	// In 3DOF mode, crosshair projects from HMD position (matching bullet origin)
	// In 6DOF mode, crosshair projects from controller/weapon position
	Vector3 crosshairOrigin = bUse3DOFAiming ? hmdPos : aimPos;
	Vector3 targetPos = crosshairOrigin + aimDir * c_CrosshairDistance->Value();

	overlayTransform.translate(targetPos);
	overlayTransform.lookAt(hmdPos, Vector3(0.0f, 0.0f, 1.0f));

	fixupRotation(overlayTransform, targetPos);

	if (bShowCrosshair)
	{
		vr->SetCrosshairTransform(overlayTransform);
	}
	overlayTransform.identity();

	short zoom = Helpers::GetInputData().zoomLevel;

	bool bHasScope;
	Vector3 scopePos;

	if (ShouldUseOriginalScope())
	{
		// Original scope: position at crosshair
		bHasScope = (zoom != -1);
		scopePos = targetPos;
	}
	else
	{
		// VR scope: position at weapon
		bHasScope = (zoom != -1) && weaponHandler.GetLocalWeaponScope(aimPos, aimDir, upDir);
		scopePos = aimPos;
	}

	if (!bHasScope)
	{
		SetScopeTransform(overlayTransform, false);
		return;
	}

	overlayTransform.translate(scopePos);
	overlayTransform.lookAt(scopePos - aimDir, upDir);

	fixupRotation(overlayTransform, scopePos);

	SetScopeTransform(overlayTransform, true);
}

void Game::SetScopeTransform(Matrix4& newTransform, bool bIsVisible)
{
	VR_PROFILE_SCOPE(Game_SetScopeTransform);

	if (!bIsVisible)
	{
		return;
	}

	Vector3 scopeUp = newTransform.getForwardAxis();
	Vector3 scopeFacing = -newTransform.getLeftAxis();

	Vector3 pos = (newTransform * Vector3(0.0f, 0.0f, 0.0f)) * MetresToWorld(1.0f) + Helpers::GetCamera().position;
	Matrix3 rot;
	Vector2 size(1.0f, 0.75f);
	size *= MetresToWorld(GetScopeSize());

	newTransform.translate(-pos);
	newTransform.rotate(90.0f, newTransform.getLeftAxis());
	newTransform.rotate(-90.0f, newTransform.getUpAxis());
	newTransform.rotate(-90.0f, newTransform.getLeftAxis());

	for (int i = 0; i < 3; i++)
	{
		rot.setColumn(i, &newTransform.get()[i * 4]);
	}

	inGameRenderer.DrawPolygon(pos, scopeFacing, scopeUp, 32, MetresToWorld(GetScopeSize() * 0.5f), D3DCOLOR_ARGB(0, 0, 0, 0), false);

	float SCOPE_DEPTH = 2.0f;
	float SCOPE_INNER_SCALE = ShouldUseOriginalScope() ? 1.6f : 80.0f;

	pos = pos - scopeFacing * MetresToWorld(SCOPE_DEPTH);
	size *= SCOPE_INNER_SCALE;

	inGameRenderer.DrawRenderTarget(vr->GetScopeTexture(), pos, rot, size, false, true);
}

void Game::StoreRenderTargets()
{
	VR_PROFILE_SCOPE(Game_StoreRenderTargets);

	for (int i = 0; i < 8; i++)
	{
		gameRenderTargets[i].width = Helpers::GetRenderTargets()[i].width;
		gameRenderTargets[i].height = Helpers::GetRenderTargets()[i].height;
		gameRenderTargets[i].format = Helpers::GetRenderTargets()[i].format;
		gameRenderTargets[i].renderSurface = Helpers::GetRenderTargets()[i].renderSurface;
		gameRenderTargets[i].renderTexture = Helpers::GetRenderTargets()[i].renderTexture;
	}
}

void Game::RestoreRenderTargets()
{
	VR_PROFILE_SCOPE(Game_RestoreRenderTargets);

	for (int i = 0; i < 8; i++)
	{
		Helpers::GetRenderTargets()[i].width = gameRenderTargets[i].width;
		Helpers::GetRenderTargets()[i].height = gameRenderTargets[i].height;
		Helpers::GetRenderTargets()[i].format = gameRenderTargets[i].format;
		Helpers::GetRenderTargets()[i].renderSurface = gameRenderTargets[i].renderSurface;
		Helpers::GetRenderTargets()[i].renderTexture = gameRenderTargets[i].renderTexture;
	}
}

void Game::CreateTextureAndSurface(UINT Width, UINT Height, DWORD Usage, D3DFORMAT Format, IDirect3DSurface9** OutSurface, IDirect3DTexture9** OutTexture)
{
	VR_PROFILE_SCOPE(Game_CreateTextureAndSurface);

	HRESULT result = Helpers::GetDirect3DDevice9()->CreateTexture(Width, Height, 1, Usage, Format, D3DPOOL_DEFAULT, OutTexture, nullptr);
	if (FAILED(result))
	{
		Logger::err << "[DX9] Failed to create game texture: " << result << std::endl;
		return;
	}

	result = (*OutTexture)->GetSurfaceLevel(0, OutSurface);
	if (FAILED(result))
	{
		Logger::err << "[DX9] Failed to retrieve game surface: " << result << std::endl;
		return;
	}
}
