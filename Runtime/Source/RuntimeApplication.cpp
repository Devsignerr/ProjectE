#include "RuntimeApplication.h"

#include "AI/AIModule.h"
#include "Audio/AudioReflection.h"
#include "Physics/PhysicsReflection.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Network/ReplicationTypes.h"
#include "Online/SteamSubsystem.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/SceneCamera.h"
#include "Renderer/SceneAssetResolver.h"
#include "UI/UIReflection.h"
#include "UI/UISystem.h"

E_DEFINE_LOG_CATEGORY(LogRuntime, Log)

namespace
{
	FApplicationDesc MakeRuntimeDesc()
	{
		FApplicationDesc Desc;
		Desc.Window.Title  = L"ProjectE";
		Desc.Window.Width  = 1280;
		Desc.Window.Height = 720;
		return Desc;
	}
} // namespace

FRuntimeApplication::FRuntimeApplication()
	: FApplication(MakeRuntimeDesc())
{
}

FRuntimeApplication::~FRuntimeApplication() = default;

void FRuntimeApplication::OnConfigureWindow(FWindowDesc& WindowDesc)
{
	// 프로젝트 기본값(Config/DefaultGameUserSettings.json) ← 사용자 설정. 자동 검증은 사용자 설정 없이 (결과가 PC마다 달라지지 않도록)
	if (IsAutomationRun())
	{
		if (FPaths::HasProject())
		{
			UserSettings.ApplyFile(FPaths::GetProjectConfigDirectory() / L"DefaultGameUserSettings.json");
		}
	}
	else
	{
		UserSettings = FGameUserSettings::Load();
	}
	// --window-mode <Windowed|BorderlessFullscreen>: 이번 실행만 (저장하지 않음, 검증용)
	if (const std::wstring ModeArg = FCommandLine::FromProcess().GetValue(L"--window-mode"); !ModeArg.empty())
	{
		EWindowMode Mode = UserSettings.WindowMode;
		if (TryParseWindowMode(FStringConv::ToUtf8(ModeArg), Mode))
		{
			UserSettings.WindowMode = Mode;
		}
		else
		{
			E_LOG(LogRuntime, Warning, "--window-mode 값을 알 수 없습니다: {}", FStringConv::ToUtf8(ModeArg));
		}
	}
	WindowDesc.Width  = UserSettings.WindowWidth;
	WindowDesc.Height = UserSettings.WindowHeight;
}

bool FRuntimeApplication::OnInit()
{
	// FApplication::Run이 FPaths를 초기화했으므로 여기서는 프로젝트만 확인
	if (FPaths::HasProject())
	{
		GetWindow().SetTitle(FStringConv::ToWide(FPaths::GetProjectDescriptor().GetDisplayName()));
		E_LOG(LogRuntime, Display, "프로젝트: {}", FPaths::GetProjectName());
	}
	else
	{
		E_LOG(LogRuntime, Warning, "프로젝트가 없습니다 (--project <경로>). 자리표시 씬만 표시합니다");
	}

	// Steam (.eproject SteamAppId): 오버레이가 D3D 장치를 잡도록 렌더러보다 먼저. 패키지 게임은 Steam 밖에서 실행되면 Steam으로 다시 실행
	// (자동 검증과 --no-steam-restart는 제외). 실패해도 Steam 없이 계속한다
	if (FPaths::HasProject())
	{
		const bool bAllowRestart = FPaths::IsPackaged() && !IsAutomationRun() && !FCommandLine::FromProcess().HasFlag(L"--no-steam-restart");
		if (FSteamSubsystem::Get().Init(FPaths::GetProjectDescriptor().SteamAppId, bAllowRestart) == FSteamSubsystem::EInitResult::RestartThroughSteam)
		{
			return false; // Steam이 게임을 다시 실행한다
		}
	}

	FD3D12RHIDesc RhiDesc;
	RhiDesc.WindowHandle = GetWindow().GetHandle();
	RhiDesc.Width        = GetWindow().GetWidth();
	RhiDesc.Height       = GetWindow().GetHeight();
	RhiDesc.bVSync       = UserSettings.bVSync;
#if E_DEBUG
	RhiDesc.bEnableDebugLayer = true;
#endif

	Rhi = std::make_unique<FD3D12RHI>();
	if (!Rhi->Init(RhiDesc))
	{
		return false;
	}
	if (!Resources.Init(*Rhi))
	{
		return false;
	}
	if (!SceneRenderer.Init(*Rhi, Resources))
	{
		return false;
	}

	RegisterAudioTypes(); // 씬 로드 전에
	RegisterPhysicsTypes();
	RegisterAITypes();
	RegisterNetworkTypes();
	RegisterUITypes();
	if (!UIRenderer.Init(*Rhi, SceneRenderer.GetShaderLibrary(), Resources, FD3D12RHI::RenderTargetFormat))
	{
		E_LOG(LogRuntime, Warning, "UI 렌더러 초기화 실패: 게임 UI를 그리지 않습니다");
	}
	// 게임 모듈 (.eproject "GameModule"): 씬 로드 전에 게임 컴포넌트 타입을 등록한다
	if (FPaths::HasProject() && !FPaths::GetProjectDescriptor().GameModule.empty())
	{
		GameModule.Load(FGameModuleHost::GetDefaultModulePath(FPaths::GetProjectDescriptor().GameModule));
	}
	if (Audio.Init() && IsAutomationRun())
	{
		Audio.SetMasterVolume(0.0f); // 자동 검증 중에는 소리를 내지 않는다
	}

	// 씬: --scene <Content 기준 상대 경로>가 있으면 그것, 아니면 프로젝트 기본 씬. 없거나 실패하면 자리표시 씬
	SceneAsset = FPaths::HasProject() ? FPaths::GetProjectDescriptor().DefaultScene : std::string();
	if (const std::wstring SceneArg = FCommandLine::FromProcess().GetValue(L"--scene"); !SceneArg.empty())
	{
		SceneAsset = FStringConv::ToUtf8(SceneArg);
	}
	LoadScene();

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 10.0f, 100000.0f); // cm: 근평면 10cm, 원평면 1km
	Camera.SetPosition(FVector3(-600.0f, -400.0f, 300.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 50.0f));

	// 게임 월드(스크립트 콘텐츠 경로·물리 훅 연결) → 세션 시작 (멀티플레이: --host / --connect / --join-lan, 없으면 Standalone)
	World.Init({ &Scripts, &Physics, &GameModule, &Resources,
	             FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory(), &Net });
	Scripts.SetAudioHooks({
		[this](FEntity Entity) { AudioSystem.Play(Audio, Entity); },
		[this](FEntity Entity) { AudioSystem.Stop(Audio, Entity); },
		[this](const std::string& ClipAsset) { Audio.PlayOneShot(Scripts.GetContentDirectory() / FStringConv::ToWide(ClipAsset)); },
	});
	// Lua Game 테이블: 종료 버튼/옵션 메뉴
	Scripts.SetAppHooks({
		[this]() { RequestExit(); },
		[this]() { return std::string(ToString(UserSettings.WindowMode)); },
		[this](const std::string& ModeName) {
			EWindowMode Mode = EWindowMode::Windowed;
			if (!TryParseWindowMode(ModeName, Mode))
			{
				return false;
			}
			ApplyWindowMode(Mode, true);
			return true;
		},
		[this]() { return UserSettings.bVSync; },
		[this](bool bEnabled) { SetVSync(bEnabled); },
	});
	StartSession(FNetLaunchOptions::FromCommandLine(FCommandLine::FromProcess()));

	// 저장된 창 모드 (RHI 초기화 후 — 크기가 바뀌면 스왑체인이 따라간다)
	if (UserSettings.WindowMode != EWindowMode::Windowed)
	{
		ApplyWindowMode(UserSettings.WindowMode, false);
	}

	E_LOG(LogRuntime, Display, "런타임 초기화 완료 (Alt+Enter 전체 화면{})", FPaths::IsPackaged() ? "" : ", ESC 종료");
	return true;
}

void FRuntimeApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();

	FSteamSubsystem::Get().RunCallbacks();
	Net.Update(DeltaSeconds); // 클라이언트: 여기서 복제 메시지 적용
	if (Lan.IsHosting())
	{
		Lan.SetPlayerCount(static_cast<uint16>(Net.GetPlayers().size() + 1)); // 호스트 포함
		Lan.Update();
	}
	if (Net.GetMode() == ENetMode::Client)
	{
		ReplicationClient.Update(DeltaSeconds); // 트랜스폼 보간
		if (ReplicationClient.ConsumeAssetsChanged())
		{
			FSceneAssetResolver::Resolve(Scene, Resources, FPaths::GetProjectContentDirectory());
		}
	}
	// 게임 UI가 먼저 입력을 본다: 포인터를 가져가면 게임 로직에는 마우스 버튼/휠을 뺀 입력을 넘긴다
	const FRenderOutput BackBuffer = Rhi->GetBackBufferOutput();
	FUIFrameInput       UIInput;
	UIInput.Viewport    = FUIRect(FVector2::ZeroVector, FVector2(static_cast<float>(BackBuffer.Width), static_cast<float>(BackBuffer.Height)));
	UIInput.bHasPointer = true;
	UIInput.Pointer     = FUISystem::MakePointer(InputState, FVector2::ZeroVector, true);
	UIInput.Keys         = FUISystem::MakeKeys(InputState);
	UIInput.DeltaSeconds = DeltaSeconds;
	FInput               BlockedInput;
	const FInput*        GameInput = &InputState;
	const FUIInputResult UIResult  = FUISystem::Update(Scene, UIInput, FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory());
	if (UIResult.bPointer || UIResult.bKeyboard)
	{
		BlockedInput = UIResult.bPointer ? InputState.WithoutMouseButtons() : InputState;
		if (UIResult.bKeyboard)
		{
			BlockedInput = BlockedInput.WithoutKeyboard();
		}
		GameInput = &BlockedInput;
	}
	// ESC 종료는 개발 실행에서만 — 패키지 게임은 ESC를 게임(일시정지 메뉴 등)에 넘기고 종료는 Game.Quit()로
	// (텍스트 상자에 입력 중이면 UI가 ESC를 받아 포커스만 푼다)
	if (!FPaths::IsPackaged() && !UIResult.bKeyboard && InputState.IsKeyPressed(EKey::Escape))
	{
		RequestExit();
	}
	// Alt+Enter: 창 ↔ 테두리 없는 전체 화면
	if ((InputState.IsKeyDown(EKey::LeftAlt) || InputState.IsKeyDown(EKey::RightAlt)) && InputState.IsKeyPressed(EKey::Enter))
	{
		ApplyWindowMode(UserSettings.WindowMode == EWindowMode::Windowed ? EWindowMode::BorderlessFullscreen : EWindowMode::Windowed, true);
	}
	World.TickGameplay(DeltaSeconds, GameInput); // 클라이언트 역할이면 물리만
	World.TickPresentation(Scene, DeltaSeconds);
	ReplicationServer.Tick(DeltaSeconds);
	if (const std::optional<FNetSessionRequest> Request = World.ConsumeSessionRequest())
	{
		HandleSessionRequest(*Request); // 스크립트의 Net.Host/Connect/Disconnect (프레임 끝에 전환)
	}

	// 주 카메라 컴포넌트가 있으면 그 시점, 없으면 자유 비행 카메라
	const FEntity CameraEntity = FSceneCamera::FindPrimary(Scene);
	if (!CameraEntity.IsValid() || !FSceneCamera::ApplyToCamera(Scene, CameraEntity, Camera.GetAspectRatio(), Camera))
	{
		CameraController.Update(Camera, *GameInput, DeltaSeconds);
	}

	// 오디오: 카메라가 청자
	Audio.SetListener({ Camera.GetPosition(), Camera.GetForwardVector(), Camera.GetUpVector() });
	AudioSystem.Update(Scene, Audio, FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : std::filesystem::path());
}

void FRuntimeApplication::OnRender()
{
	const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f };
	Rhi->BeginFrame(ClearColor);
	SceneRenderer.Render(Scene, Camera, Rhi->GetBackBufferOutput());
	UIDrawList.Clear();
	FUISystem::Paint(Scene, UIDrawList);
	UIRenderer.Render(UIDrawList, Rhi->GetBackBufferOutput(), FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory());
	Rhi->EndFrame();
}

void FRuntimeApplication::OnResize(uint32 Width, uint32 Height)
{
	if (Rhi)
	{
		Rhi->Resize(Width, Height);
		Camera.SetAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
	}
}

void FRuntimeApplication::ApplyWindowMode(EWindowMode Mode, bool bSave)
{
	UserSettings.WindowMode = Mode;
	GetWindow().SetBorderlessFullscreen(Mode == EWindowMode::BorderlessFullscreen);
	if (bSave)
	{
		SaveUserSettings();
	}
}

void FRuntimeApplication::SetVSync(bool bEnabled)
{
	UserSettings.bVSync = bEnabled;
	if (Rhi)
	{
		Rhi->SetVSync(bEnabled);
	}
	SaveUserSettings();
}

void FRuntimeApplication::SaveUserSettings() const
{
	if (!IsAutomationRun())
	{
		UserSettings.Save();
	}
}

void FRuntimeApplication::OnShutdown()
{
	// 창 모드면 마지막 창 크기를 기억한다 (최소화 상태는 제외)
	if (!GetWindow().IsBorderlessFullscreen() && !GetWindow().IsMinimized() && GetWindow().GetWidth() > 0)
	{
		UserSettings.WindowWidth  = GetWindow().GetWidth();
		UserSettings.WindowHeight = GetWindow().GetHeight();
	}
	SaveUserSettings();
	EndSession();
	Audio.Shutdown();

	if (Rhi)
	{
		UIRenderer.Shutdown();
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
	FSteamSubsystem::Get().Shutdown();
	GameModule.Unload(); // 등록 타입 제거 (씬의 게임 컴포넌트는 앱 소멸 시 정리, DLL은 프로세스 종료까지 유지)
}

void FRuntimeApplication::BuildPlaceholderScene()
{
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 3.0f;

	const FMeshHandle CubeMesh = Resources.CreateMesh(FPrimitiveShapes::MakeCube(FUnits::MetersToUnits), L"Cube");

	FMaterial GroundMaterial;
	GroundMaterial.Name                    = "Ground";
	GroundMaterial.Constants.BaseColorFactor = FVector4(0.55f, 0.6f, 0.65f, 1.0f);
	GroundMaterial.Constants.Roughness       = 0.8f;

	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.Mesh     = CubeMesh;
	GroundMesh.Material = Resources.CreateMaterial(GroundMaterial);

	Scene.UpdateTransforms();
}

void FRuntimeApplication::OnScreenshotRequested(const std::filesystem::path& Path)
{
	if (Rhi)
	{
		Rhi->RequestScreenshot(Path);
	}
}
