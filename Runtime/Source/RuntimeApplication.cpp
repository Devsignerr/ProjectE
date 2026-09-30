#include "RuntimeApplication.h"

#include "Audio/AudioReflection.h"
#include "Physics/PhysicsReflection.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Network/NetTransport.h"
#include "Network/ReplicationTypes.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/SceneCamera.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/SceneSerializer.h"

#include <chrono>
#include <format>
#include <thread>

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

bool FRuntimeApplication::OnInit()
{
	// FApplication::Run이 FPaths를 초기화했으므로 여기서는 프로젝트만 확인
	if (FPaths::HasProject())
	{
		GetWindow().SetTitle(FStringConv::ToWide(FPaths::GetProjectName()));
		E_LOG(LogRuntime, Display, "프로젝트: {}", FPaths::GetProjectName());
	}
	else
	{
		E_LOG(LogRuntime, Warning, "프로젝트가 없습니다 (--project <경로>). 자리표시 씬만 표시합니다");
	}

	FD3D12RHIDesc RhiDesc;
	RhiDesc.WindowHandle = GetWindow().GetHandle();
	RhiDesc.Width        = GetWindow().GetWidth();
	RhiDesc.Height       = GetWindow().GetHeight();
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
	RegisterNetworkTypes();
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
	std::string SceneAsset = FPaths::HasProject() ? FPaths::GetProjectDescriptor().DefaultScene : std::string();
	if (const std::wstring SceneArg = FCommandLine::FromProcess().GetValue(L"--scene"); !SceneArg.empty())
	{
		SceneAsset = FStringConv::ToUtf8(SceneArg);
	}
	bool bSceneLoaded = false;
	if (FPaths::HasProject() && !SceneAsset.empty())
	{
		const std::filesystem::path ScenePath = FPaths::GetProjectContentDirectory() / FStringConv::ToWide(SceneAsset);
		if (FSceneSerializer::LoadFromFile(Scene, ScenePath))
		{
			FSceneAssetResolver::Resolve(Scene, Resources, FPaths::GetProjectContentDirectory());
			bSceneLoaded = true;
			E_LOG(LogRuntime, Display, "씬 로드: {}", SceneAsset);
		}
		else
		{
			E_LOG(LogRuntime, Warning, "씬을 열지 못해 자리표시 씬을 표시합니다: {}", SceneAsset);
		}
	}
	if (!bSceneLoaded)
	{
		BuildPlaceholderScene();
	}

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 10.0f, 100000.0f); // cm: 근평면 10cm, 원평면 1km
	Camera.SetPosition(FVector3(-600.0f, -400.0f, 300.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 50.0f));

	// 게임 시작: 게임 월드(스크립트 콘텐츠 경로·물리 훅 연결) → BeginPlay
	World.Init({ &Scripts, &Physics, &GameModule, &Resources,
	             FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory(), &Net });
	Scripts.SetAudioHooks({
		[this](FEntity Entity) { AudioSystem.Play(Audio, Entity); },
		[this](FEntity Entity) { AudioSystem.Stop(Audio, Entity); },
		[this](const std::string& ClipAsset) { Audio.PlayOneShot(Scripts.GetContentDirectory() / FStringConv::ToWide(ClipAsset)); },
	});

	// 멀티플레이: --host [--port N] = 리슨 서버, --connect ip:port = 클라이언트, 없으면 Standalone
	FNetLaunchOptions     NetOptions = FNetLaunchOptions::FromCommandLine(FCommandLine::FromProcess());
	const FNetSessionInfo Session    = FNetSessionInfo::FromProject(SceneAsset);
	if (NetOptions.bJoinLan)
	{
		// LAN에서 같은 프로젝트 세션을 찾아 첫 번째에 접속 (최대 2초, 0.5초마다 다시 질의)
		for (int32 Attempt = 0; Attempt < 4 && Lan.GetSessions().empty(); ++Attempt)
		{
			Lan.StartSearch(Session.ProjectName);
			for (int32 Wait = 0; Wait < 50 && Lan.GetSessions().empty(); ++Wait)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				Lan.Update();
			}
		}
		if (!Lan.GetSessions().empty())
		{
			NetOptions.ConnectAddress = Lan.GetSessions().front().Address;
			E_LOG(LogRuntime, Display, "LAN 세션 '{}' ({})에 접속합니다", Lan.GetSessions().front().Name, NetOptions.ConnectAddress);
		}
		else
		{
			E_LOG(LogRuntime, Warning, "LAN에서 세션을 찾지 못했습니다");
		}
		Lan.Stop();
	}
	if (NetOptions.Mode == ENetMode::Client && Net.StartClient(CreateGnsTransport(), NetOptions.ConnectAddress, Session))
	{
		// 클라이언트: 게임 로직(스크립트/게임 모듈)은 서버가 돌리고 결과만 받는다. 물리는 복제 엔티티를 키네마틱으로 둔 채 돌린다
		ReplicationClient.Begin(Scene);
		Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) {
			if (!ReplicationClient.HandleMessage(Message))
			{
				World.HandleNetMessage(Connection, Message); // 스크립트 RPC
			}
		};
		World.BeginPlay(Scene, ENetMode::Client);
	}
	else
	{
		if (NetOptions.Mode == ENetMode::Client)
		{
			E_LOG(LogRuntime, Error, "서버 {}에 접속하지 못했습니다 (단독 실행으로 계속)", NetOptions.ConnectAddress);
		}
		ReplicationServer.Begin(Scene, Net); // 정적 NetId는 게임 시작(스크립트 생성) 전에. Standalone이면 보내지 않는다
		Net.OnPlayerJoined = [this](const FNetDriver::FRemotePlayer& Player) {
			const FEntity Pawn = Players.SpawnPlayer(Player.PlayerId);
			ReplicationServer.OnPlayerJoined(Player.Connection);
			World.OnPlayerJoined(Player.PlayerId, Pawn);
		};
		Net.OnPlayerLeft  = [this](const FNetDriver::FRemotePlayer& Player, const std::string&) {
			World.OnPlayerLeft(Player.PlayerId);
			Players.DespawnPlayer(Player.PlayerId);
		};
		Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) { World.HandleNetMessage(Connection, Message); };
		World.BeginPlay(Scene, NetOptions.Mode == ENetMode::ListenServer ? ENetMode::ListenServer : ENetMode::Standalone);
		if (NetOptions.Mode == ENetMode::ListenServer)
		{
			if (Net.StartServer(CreateGnsTransport(), NetOptions.Port, Session, false))
			{
				// 플레이어 프리팹은 멀티플레이에서만 (1인용 씬은 플레이어를 씬에 직접 둔다). 호스트도 플레이어
				Players.Begin(Scene, FPaths::HasProject() ? FPaths::GetProjectDescriptor().PlayerPrefab : std::string());
				World.OnPlayerJoined(FNetDriver::HostPlayerId, Players.SpawnPlayer(FNetDriver::HostPlayerId));
				FLanHostInfo LanInfo;
				LanInfo.Name       = std::format("{} (호스트)", FPaths::GetProjectName());
				LanInfo.Session    = Session;
				LanInfo.GamePort   = NetOptions.Port;
				LanInfo.MaxPlayers = Net.MaxPlayers;
				Lan.StartHost(LanInfo);
			}
			else
			{
				E_LOG(LogRuntime, Error, "포트 {}에서 리슨 서버를 열지 못했습니다 (단독 실행으로 계속)", NetOptions.Port);
			}
		}
	}
	if (Net.GetMode() != ENetMode::Standalone)
	{
		GetWindow().SetTitle(FStringConv::ToWide(std::format("{} [{}]", FPaths::HasProject() ? FPaths::GetProjectName() : "ProjectE", ToString(Net.GetMode()))));
	}

	E_LOG(LogRuntime, Display, "런타임 초기화 완료 (ESC 종료)");
	return true;
}

void FRuntimeApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();
	if (InputState.IsKeyPressed(EKey::Escape))
	{
		RequestExit();
	}

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
	World.TickGameplay(DeltaSeconds, &InputState); // 클라이언트 역할이면 물리만
	World.TickPresentation(Scene, DeltaSeconds);
	ReplicationServer.Tick(DeltaSeconds);

	// 주 카메라 컴포넌트가 있으면 그 시점, 없으면 자유 비행 카메라
	const FEntity CameraEntity = FSceneCamera::FindPrimary(Scene);
	if (!CameraEntity.IsValid() || !FSceneCamera::ApplyToCamera(Scene, CameraEntity, Camera.GetAspectRatio(), Camera))
	{
		CameraController.Update(Camera, InputState, DeltaSeconds);
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

void FRuntimeApplication::OnShutdown()
{
	Lan.Stop();
	Net.Shutdown();
	ReplicationServer.End();
	ReplicationClient.End();
	Players.End();
	World.EndPlay();
	AudioSystem.Reset(Audio);
	Audio.Shutdown();

	if (Rhi)
	{
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
	if (Rhi)
	{
		Rhi->RequestScreenshot(Path);
	}
}
