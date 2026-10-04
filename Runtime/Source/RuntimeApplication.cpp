#include "RuntimeApplication.h"

#include "AI/AIModule.h"
#include "Audio/AudioReflection.h"
#include "Physics/PhysicsReflection.h"
#include "Core/CommandLine.h"
#include "Core/Console/Console.h"
#include "Core/FrameTime.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Network/NetBindPolicy.h"
#include "Network/ReplicationTypes.h"
#include "Online/SteamSubsystem.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/DebugDraw.h"
#include "Renderer/HdrOutputController.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/SceneCamera.h"
#include "Renderer/SceneAssetResolver.h"
#include "Renderer/UpscaleMath.h"
#include "UI/UIDebugDraw.h"
#include "UI/UIReflection.h"
#include "UI/UISystem.h"
#include "World/GameWorldTravel.h"

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
	// 프로젝트 기본값(설정 "화면 기본값") ← 사용자 설정. 자동 검증은 사용자 설정 없이 (결과가 PC마다 달라지지 않도록)
	UserSettings = IsAutomationRun() ? FProjectSettings::Get().Display : FGameUserSettings::Load();
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
	NetBindPolicy::Configure(IsAutomationRun()); // 자동 검증은 같은 PC 전용 대기 (방화벽 확인 창 없음)
	// 개발자 콘솔: 개발 실행은 항상, 패키지 게임은 프로젝트 설정으로. 출력은 로그 기록을 그대로 보여 준다
	Console.bEnabled = !FPaths::IsPackaged() || FProjectSettings::Get().Console.bEnableInPackagedGame;
	if (Console.bEnabled)
	{
		FLog::EnableHistory();
	}
	if (const std::wstring ConsoleText = FCommandLine::FromProcess().GetValue(L"--console-input"); !ConsoleText.empty() && Console.bEnabled)
	{
		Console.SetOpen(true); // 자동 검증: 콘솔을 연 상태 + 입력 줄 (자동 완성 후보 확인)
		Console.SetInputText(FStringConv::ToUtf8(ConsoleText));
	}

	// FApplication::Run이 FPaths를 초기화했으므로 여기서는 프로젝트만 확인
	if (FPaths::HasProject())
	{
		GetWindow().SetTitle(FStringConv::ToWide(FProjectSettings::Get().GetDisplayName()));
		E_LOG(LogRuntime, Display, "프로젝트: {}", FPaths::GetProjectName());
	}
	else
	{
		E_LOG(LogRuntime, Warning, "프로젝트가 없습니다 (--project <경로>). 자리표시 씬만 표시합니다");
	}

	// Steam (프로젝트 설정 Steam App ID): 오버레이가 D3D 장치를 잡도록 렌더러보다 먼저. 패키지 게임은 Steam 밖에서 실행되면 Steam으로 다시 실행
	// (자동 검증과 --no-steam-restart는 제외). 실패해도 Steam 없이 계속한다.
	// 자동 검증은 기본으로 Steam을 켜지 않는다 (오버레이가 D3D 객체를 프로세스 끝까지 잡아 종료 시 라이브 객체로 보고됨 — 확인하려면 --steam).
	// --no-steam: 항상 끔
	const FCommandLine SteamArgs    = FCommandLine::FromProcess();
	const bool         bSteamWanted = !SteamArgs.HasFlag(L"--no-steam") && (!IsAutomationRun() || SteamArgs.HasFlag(L"--steam"));
	if (FPaths::HasProject() && bSteamWanted)
	{
		const bool bAllowRestart = FPaths::IsPackaged() && !IsAutomationRun() && !FCommandLine::FromProcess().HasFlag(L"--no-steam-restart");
		if (FSteamSubsystem::Get().Init(FProjectSettings::Get().Info.SteamAppId, bAllowRestart) == FSteamSubsystem::EInitResult::RestartThroughSteam)
		{
			return false; // Steam이 게임을 다시 실행한다
		}
	}

	FD3D12RHIDesc RhiDesc;
	RhiDesc.WindowHandle = GetWindow().GetHandle();
	RhiDesc.Width        = GetWindow().GetWidth();
	RhiDesc.Height       = GetWindow().GetHeight();
	RhiDesc.bVSync       = UserSettings.bVSync && !FCommandLine::FromProcess().HasFlag(L"--no-vsync"); // --no-vsync: 성능 측정용 (이번 실행만)
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
	// 리소스 수거 루트: 플레이 중인 씬 (서브 씬 포함 — 루트 엔티티 아래). 렌더러/UI 렌더러 캐시는 각자 등록한다
	Resources.AddRootProvider([this](FResourceRoots& Roots) { Roots.AddScene(Scene); });
	StatOverlay.SetResources(&Resources);
	Resources.EnableAsyncLoading(IsAutomationRun()); // 텍스처 디코드/압축은 작업 스레드, 업로드는 복사 큐 (자동 검증은 프레임마다 비움)
	if (!SceneRenderer.Init(*Rhi, Resources))
	{
		return false;
	}
	// TAAU/동적 해상도 (Phase 48): 사용자 설정 → 콘솔 변수. 명령줄(--screen-percentage, --dynamic-resolution, --cvar)로 정한 값은 그대로
	SceneRenderer.bAllowScreenPercentage = true;
	SceneRenderer.bAllowRayTracing       = true; // 레이 트레이싱 (Phase 50): r.RayTracing* / 프로젝트 설정 Rendering, DXR 미지원이면 꺼짐
	{
		FConsoleManager& Cvars = FConsoleManager::Get();
		if (FConsoleVariable* Var = Cvars.FindVariable("r.ScreenPercentage"); Var != nullptr && Var->IsDefault())
		{
			Var->SetFloat(FUpscaleMath::GetPresetScreenPercentage(static_cast<int32>(UserSettings.ResolutionQuality)));
		}
		if (FConsoleVariable* Var = Cvars.FindVariable("r.DynamicResolution"); Var != nullptr && Var->IsDefault())
		{
			Var->SetBool(UserSettings.bDynamicResolution);
		}
		if (FConsoleVariable* Var = Cvars.FindVariable("r.DynamicResolution.TargetMs"); Var != nullptr && Var->IsDefault())
		{
			Var->SetFloat(UserSettings.DynamicResolutionTargetMs);
		}
		RenderThreadVar = Cvars.FindVariable("r.RenderThread");
	}
	FHdrOutputController::ApplySettings(UserSettings); // HDR 출력 (Phase 49): 사용자 설정 → r.HDR.* (명령줄 값 우선)

	RegisterAudioTypes(); // 씬 로드 전에
	RegisterPhysicsTypes();
	RegisterAITypes();
	RegisterNetworkTypes();
	RegisterUITypes();
	if (!DebugDrawRenderer.Init(*Rhi, SceneRenderer.GetShaderLibrary()))
	{
		E_LOG(LogRuntime, Warning, "디버그 선 렌더러 초기화 실패: 3D 디버그 선을 그리지 않습니다");
	}
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
	SceneAsset = FPaths::HasProject() ? FProjectSettings::Get().Maps.GameDefaultMap : std::string(); // 프로젝트 설정 "게임 기본 맵"
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
		[this](bool bLocked) { GetWindow().SetCursorLocked(bLocked && !IsAutomationRun()); },
		[this]() { return GetWindow().IsCursorLocked(); },
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

void FRuntimeApplication::UpdateInputModeCursor(const FInput& InputState)
{
	const EInputMode        InputMode = FInputModeState::Get();
	const FInputModeRouting Routing   = GetInputModeRouting(InputMode);
	const bool              bAllowLock = !IsAutomationRun(); // Game.SetMouseLocked와 같이 자동 검증에서는 잠그지 않는다
	if (FInputModeState::GetRevision() != AppliedInputModeRevision)
	{
		// 모드에 들어갈 때의 기본값만 적용한다 (GameAndUI → GameAndUI 재설정은 스크립트가 잠근 커서를 건드리지 않는다)
		if (Routing.bLockCursor)
		{
			GetWindow().SetCursorLocked(bAllowLock);
		}
		else if (GetInputModeRouting(AppliedInputMode).bLockCursor)
		{
			GetWindow().SetCursorLocked(false);
		}
		AppliedInputModeRevision = FInputModeState::GetRevision();
		AppliedInputMode         = InputMode;
	}
	else if (Routing.bLockCursor && bAllowLock && !GetWindow().IsCursorLocked() && !Console.IsOpen() &&
	         InputState.IsMouseButtonPressed(EMouseButton::Left))
	{
		GetWindow().SetCursorLocked(true); // ESC/포커스 상실로 풀린 잠금: 창을 클릭하면 다시 (언리얼 GameOnly 캡처)
	}
}

void FRuntimeApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();
	Resources.Tick(); // 요청된 리소스 수거 (맵 전환 몇 프레임 뒤) + VRAM 예산 경고

	// 맵 전환: 직전 프레임이 검은 화면을 냈으므로 여기서 연다 (네트워크 수신 전 — 새 씬 기준으로 메시지를 받는다)
	if (PendingTravel)
	{
		const std::string NextScene = std::move(*PendingTravel);
		PendingTravel.reset();
		TravelTo(NextScene);
	}

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
	// 콘솔(` 키)이 가장 먼저 키보드를 본다: 열려 있으면 게임 UI/게임에는 키 없음
	StatOverlay.Tick(DeltaSeconds);
	if (Console.IsOpen())
	{
		RenderThread.WaitIdle(); // 콘솔 명령(셰이더 다시 로드·리소스 수거·통계 등)은 렌더 상태를 만질 수 있다
	}
	const bool bConsoleKeyboard = Console.Update(InputState, DeltaSeconds, FConsoleManager::Get());

	// 입력 모드 (Game.SetInputMode): GameOnly = UI는 입력 없음, GameAndUI = UI 먼저, UIOnly = 게임은 빈 입력
	const EInputMode        InputMode = FInputModeState::Get();
	const FInputModeRouting Routing   = GetInputModeRouting(InputMode);
	UpdateInputModeCursor(InputState);

	// 게임 UI가 먼저 입력을 본다: 포인터를 가져가면 게임 로직에는 마우스 버튼/휠을 뺀 입력을 넘긴다
	// 백버퍼 크기만 읽는다 (렌더 스레드가 Present 중일 수 있어 현재 백버퍼 RTV는 보지 않는다)
	FUIFrameInput UIInput;
	UIInput.Viewport = FUIRect(FVector2::ZeroVector, FVector2(static_cast<float>(Rhi->GetBackBufferWidth()), static_cast<float>(Rhi->GetBackBufferHeight())));
	UIInput.bHasPointer = Routing.bUIInput;
	UIInput.Pointer     = FUISystem::MakePointer(InputState, FVector2::ZeroVector, true);
	UIInput.Keys         = bConsoleKeyboard || !Routing.bUIInput ? FUIKeyInput{} : FUISystem::MakeKeys(InputState);
	UIInput.DeltaSeconds = DeltaSeconds;
	FInput               BlockedInput;
	const FUIInputResult UIResult  = FUISystem::Update(Scene, UIInput, FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory());
	// 텍스트 상자 입력 중: IME 조합을 창이 직접 받고 후보 창을 캐럿 아래에 (Phase 32-2)
	if (Console.IsOpen())
	{
		const FUIRect& Caret = Console.GetCaretRect();
		GetWindow().SetTextInput(true, static_cast<int32>(Caret.Min.X), static_cast<int32>(Caret.Min.Y), static_cast<int32>(Caret.GetHeight()));
	}
	else
	{
		GetWindow().SetTextInput(UIResult.bKeyboard && UIResult.bHasTextCaret, static_cast<int32>(UIResult.TextCaret.Min.X),
		                         static_cast<int32>(UIResult.TextCaret.Min.Y), static_cast<int32>(UIResult.TextCaret.GetHeight()));
	}
	const FInput* GameInput = &SelectGameInput(InputMode, InputState, UIResult.bPointer, UIResult.bKeyboard || bConsoleKeyboard, BlockedInput);
	// ESC 종료는 개발 실행에서만 — 패키지 게임은 ESC를 게임(일시정지 메뉴 등)에 넘기고 종료는 Game.Quit()로
	// (텍스트 상자에 입력 중이면 UI가 ESC를 받아 포커스만 푼다)
	// 커서가 잠겨 있으면 ESC는 잠금만 푼다 (게임 스크립트도 ESC를 볼 수 있다)
	// (콘솔이 열려 있으면 ESC는 콘솔을 닫는다)
	if (!UIResult.bKeyboard && !bConsoleKeyboard && InputState.IsKeyPressed(EKey::Escape) && GetWindow().IsCursorLocked())
	{
		GetWindow().SetCursorLocked(false);
	}
	else if (!FPaths::IsPackaged() && !UIResult.bKeyboard && !bConsoleKeyboard && InputState.IsKeyPressed(EKey::Escape))
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
	else if (std::optional<std::string> NextScene = FGameWorldTravel::ConsumePending(World, &Net))
	{
		PendingTravel = std::move(*NextScene); // Game.OpenScene / 서버의 맵 이동 지시 → 이번 프레임은 로딩 화면, 다음 프레임에 연다
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
	// 직전 프레임의 기록·제출이 끝난 뒤에만 렌더 상태(RHI 프레임·렌더러·사본)를 만진다. r.RenderThread는 프레임마다 읽는다 (콘솔로 전환 가능)
	const bool bThreaded = RenderThreadVar != nullptr && RenderThreadVar->GetBool();
	if (bThreaded)
	{
		RenderThread.Start();
	}
	using FClock = std::chrono::steady_clock;
	const FClock::time_point WaitStart = FClock::now();
	RenderThread.WaitIdle();
	const FClock::time_point WaitEnd = FClock::now();
	if (CpuTimes.bHasLastKick)
	{
		CpuTimes.GameMs += std::chrono::duration<double, std::milli>(WaitStart - CpuTimes.LastKickEnd).count();
	}
	const std::filesystem::path Screenshot = std::exchange(PendingScreenshot, {});
	if (PendingTravel)
	{
		// 로딩 화면: 다음 프레임에 새 씬을 여는 동안 (동기 로드라 창이 멈춘다) 검은 화면을 보인다
		const float Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		Rhi->BeginFrame(Black);
		if (!Screenshot.empty())
		{
			Rhi->RequestScreenshot(Screenshot);
		}
		Rhi->EndFrame();
		return;
	}
	FHdrOutputController::Update(*Rhi); // r.HDR.Output 변경 반영 (BeginFrame 전)
	const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f };
	const FClock::time_point BeginFrameStart = FClock::now();
	Rhi->BeginFrame(ClearColor);
	CpuTimes.BeginFrameMs += std::chrono::duration<double, std::milli>(FClock::now() - BeginFrameStart).count();
	// 화면 통계(stat fps/gpu) = 기록이 끝난 지난 프레임까지의 값
	const std::vector<std::string> StatLines = StatOverlay.BuildLines(&SceneRenderer.GetStats());

	// 게임 스레드 준비: 씬을 읽는 수집·업로드·패스 등록 (HDR 출력이면 선형 FP16 씬 타깃 — UI·디버그 선은 백버퍼 = 겹침 층).
	// 카메라·출력은 렌더 작업이 끝날 때까지 사는 사본으로 넘긴다
	RenderCamera      = Camera;
	RenderSceneOutput = Rhi->GetSceneOutput();
	SceneRenderer.BeginRender(Scene, RenderCamera, RenderSceneOutput);
	RenderDebugLines = FDebugDraw::Get().GetLines(); // 다음 프레임 갱신이 선을 바꾸므로 사본

	// 게임 UI 그리기 목록 + 화면 통계·콘솔 (게임 UI 위). 텍스처(글꼴 아틀라스·파일)는 리소스 관리자를 바꾸므로 여기서 준비
	const std::filesystem::path ContentDirectory = FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory();
	UIDrawList.Clear();
	FUISystem::Paint(Scene, UIDrawList);
	const FUIRect Screen(FVector2::ZeroVector, FVector2(static_cast<float>(Rhi->GetBackBufferWidth()), static_cast<float>(Rhi->GetBackBufferHeight())));
	FUIDebugDraw::AddTextPanel(UIDrawList, StatLines, FVector2(Screen.Max.X - 12.0f, 12.0f), true, 15.0f, { 140.0f, 70.0f }, Screen);
	Console.Paint(Screen, UIDrawList);
	UIRenderer.PrepareTextures(UIDrawList, ContentDirectory);

	// 렌더 작업: 렌더 스레드(r.RenderThread 1)에서 게임 스레드의 다음 OnUpdate와 겹쳐, 아니면 바로
	const FClock::time_point KickStart = FClock::now();
	CpuTimes.GameMs += std::chrono::duration<double, std::milli>(KickStart - WaitEnd).count();
	++CpuTimes.Frames;
	RenderThread.Kick([this, Screenshot, ContentDirectory]() { RecordAndPresent(Screenshot, ContentDirectory); }, FFrameTime::Capture(), bThreaded);
	CpuTimes.LastKickEnd  = FClock::now();
	CpuTimes.bHasLastKick = true;
}

void FRuntimeApplication::RecordAndPresent(const std::filesystem::path& Screenshot, const std::filesystem::path& ContentDirectory)
{
	using FClock = std::chrono::steady_clock;
	const FClock::time_point RecordStart = FClock::now();
	SceneRenderer.FinishRender(); // 그래프 컴파일·실행 (명령 기록)
	const FRenderOutput Back = Rhi->GetBackBufferOutput();
	// 3D 디버그 선: 씬 깊이가 백버퍼와 같은 크기일 때만 깊이 테스트 (픽셀 아트 모드는 "항상 위" 선만, TAAU면 출력 해상도로 옮긴 깊이)
	DebugDrawRenderer.Render(RenderDebugLines, RenderCamera, Back, SceneRenderer.GetOverlayDepthDsv(Back.Width, Back.Height));
	UIRenderer.Render(UIDrawList, Back, ContentDirectory);
	if (!Screenshot.empty())
	{
		Rhi->RequestScreenshot(Screenshot);
	}
	const FClock::time_point SubmitStart = FClock::now();
	Rhi->EndFrame();
	CpuTimes.RecordMs += std::chrono::duration<double, std::milli>(SubmitStart - RecordStart).count();
	CpuTimes.SubmitMs += std::chrono::duration<double, std::milli>(FClock::now() - SubmitStart).count();
}

void FRuntimeApplication::LogRenderThreadStats() const
{
	const FRenderThread::FStats Stats = RenderThread.GetStats();
	if (Stats.Frames == 0 || CpuTimes.Frames == 0)
	{
		return;
	}
	const double Frames = static_cast<double>(Stats.Frames);
	const double Cpu    = static_cast<double>(CpuTimes.Frames);
	E_LOG(LogRuntime, Display,
	      "[성능] 렌더 스레드: 작업 {}개 (렌더 스레드 {}개), 렌더 작업 평균 {:.3f} ms, 게임 스레드 대기 평균 {:.3f} ms | CPU 구간 평균: 게임 스레드 {:.3f} ms (BeginFrame {:.3f}), "
	      "기록 {:.3f} ms, 제출·Present {:.3f} ms",
	      Stats.Frames, Stats.Threaded, Stats.WorkMs / Frames, Stats.WaitMs / Frames, CpuTimes.GameMs / Cpu, CpuTimes.BeginFrameMs / Cpu, CpuTimes.RecordMs / Cpu, CpuTimes.SubmitMs / Cpu);
}

void FRuntimeApplication::OnResize(uint32 Width, uint32 Height)
{
	if (Rhi)
	{
		RenderThread.WaitIdle(); // 스왑체인·백버퍼를 다시 만든다 (진행 중 Present가 끝난 뒤)
		Rhi->Resize(Width, Height);
		Camera.SetAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
	}
}

void FRuntimeApplication::ApplyWindowMode(EWindowMode Mode, bool bSave)
{
	UserSettings.WindowMode = Mode;
	RenderThread.WaitIdle(); // 창 스타일·크기 변경 (크기가 바뀌면 OnResize가 스왑체인을 다시 만든다)
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
		RenderThread.WaitIdle(); // Present가 읽는다
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
	// 렌더 스레드: 마지막 프레임 기록·제출(스크린샷 기록 포함)을 끝내고 멈춘다 — 이후는 모두 게임 스레드
	RenderThread.Stop();
	LogRenderThreadStats();
	// 창 모드면 마지막 창 크기를 기억한다 (최소화 상태는 제외)
	if (!GetWindow().IsBorderlessFullscreen() && !GetWindow().IsMinimized() && GetWindow().GetWidth() > 0)
	{
		UserSettings.WindowWidth  = GetWindow().GetWidth();
		UserSettings.WindowHeight = GetWindow().GetHeight();
	}
	SaveUserSettings();
	EndSession();
	Audio.Shutdown();
	// Steam 오버레이가 스왑체인/큐를 잡고 있으므로 렌더러보다 먼저 끈다 (반대면 종료 시 라이브 D3D 객체 보고)
	FSteamSubsystem::Get().Shutdown();

	if (Rhi)
	{
		Rhi->GetGraphicsQueue().Flush(); // 마지막 프레임이 쓰던 PSO/리소스를 즉시 해제하기 전에 GPU 완료 대기 (디버그 레이어 CORRUPTION 방지)
		UIRenderer.Shutdown();
		DebugDrawRenderer.Shutdown();
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
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
	PendingScreenshot = Path; // 이번 프레임 렌더 작업이 EndFrame 직전에 RHI에 넘긴다 (렌더 스레드가 앞 프레임을 Present하는 중일 수 있다)
}
