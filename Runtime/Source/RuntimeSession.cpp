// FRuntimeApplication의 세션 부분: 씬 로드, 넷 모드별 시작/종료, 스크립트 요청(Net.Host/Connect/Disconnect) 처리
#include "RuntimeApplication.h"

#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Network/NetTransport.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/SceneSerializer.h"

#include <chrono>
#include <format>
#include <thread>

E_DECLARE_LOG_CATEGORY(LogRuntime)

void FRuntimeApplication::LoadScene()
{
	Scene.Clear();
	if (FPaths::HasProject() && !SceneAsset.empty())
	{
		const std::filesystem::path ScenePath = FPaths::GetProjectContentDirectory() / FStringConv::ToWide(SceneAsset);
		if (FSceneSerializer::LoadFromFile(Scene, ScenePath))
		{
			FSceneAssetResolver::Resolve(Scene, Resources, FPaths::GetProjectContentDirectory());
			E_LOG(LogRuntime, Display, "씬 로드: {}", SceneAsset);
			return;
		}
		E_LOG(LogRuntime, Warning, "씬을 열지 못해 자리표시 씬을 표시합니다: {}", SceneAsset);
	}
	BuildPlaceholderScene();
}

void FRuntimeApplication::StartSession(FNetLaunchOptions Options)
{
	const FNetSessionInfo Session = FNetSessionInfo::FromProject(SceneAsset);
	if (Options.bJoinLan)
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
			Options.ConnectAddress = Lan.GetSessions().front().Address;
			E_LOG(LogRuntime, Display, "LAN 세션 '{}' ({})에 접속합니다", Lan.GetSessions().front().Name, Options.ConnectAddress);
		}
		else
		{
			E_LOG(LogRuntime, Warning, "LAN에서 세션을 찾지 못했습니다");
		}
		Lan.Stop();
	}

	if (Options.Mode == ENetMode::Client && Net.StartClient(CreateGnsTransport(), Options.ConnectAddress, Session))
	{
		// 클라이언트: 게임 로직(서버 스크립트/게임 모듈)은 서버가 돌리고 결과만 받는다. 물리는 복제 엔티티를 키네마틱으로 둔 채 돌린다
		ReplicationClient.Begin(Scene);
		ReplicationClient.SetTransformFilter([this](FEntity Entity) { return !World.IsPredicted(Entity); }); // 내 캐릭터는 예측으로
		Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) {
			if (!ReplicationClient.HandleMessage(Message))
			{
				World.HandleNetMessage(Connection, Message); // RPC
			}
		};
		World.BeginPlay(Scene, ENetMode::Client);
	}
	else
	{
		if (Options.Mode == ENetMode::Client)
		{
			E_LOG(LogRuntime, Error, "서버 '{}'에 접속하지 못했습니다 (단독 실행으로 계속)", Options.ConnectAddress);
		}
		ReplicationServer.Begin(Scene, Net); // 정적 NetId는 게임 시작(스크립트 생성) 전에. Standalone이면 보내지 않는다
		Net.OnPlayerJoined = [this](const FNetDriver::FRemotePlayer& Player) {
			const FEntity Pawn = Players.SpawnPlayer(Player.PlayerId);
			ReplicationServer.OnPlayerJoined(Player.Connection);
			World.OnPlayerJoined(Player.PlayerId, Pawn);
		};
		Net.OnPlayerLeft = [this](const FNetDriver::FRemotePlayer& Player, const std::string&) {
			World.OnPlayerLeft(Player.PlayerId);
			Players.DespawnPlayer(Player.PlayerId);
		};
		Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) { World.HandleNetMessage(Connection, Message); };
		World.BeginPlay(Scene, ENetMode::Standalone);
		if (Options.Mode == ENetMode::ListenServer)
		{
			StartListenServer(Options.Port);
		}
	}
	if (Net.GetMode() != ENetMode::Standalone && (Options.SimulatedLatencyMs > 0 || Options.SimulatedLossPercent > 0.0f))
	{
		Net.SetSimulation(Options.SimulatedLatencyMs, Options.SimulatedLossPercent); // --net-lag / --net-loss
	}
	UpdateWindowTitle();
}

void FRuntimeApplication::StartListenServer(uint16 Port)
{
	if (!Net.StartServer(CreateGnsTransport(), Port, FNetSessionInfo::FromProject(SceneAsset), false))
	{
		E_LOG(LogRuntime, Error, "포트 {}에서 리슨 서버를 열지 못했습니다 (단독 실행으로 계속)", Port);
		return;
	}
	World.SetNetMode(ENetMode::ListenServer);
	// 플레이어 프리팹은 멀티플레이에서만 (1인용 씬은 플레이어를 씬에 직접 둔다). 호스트도 플레이어
	Players.Begin(Scene, FPaths::HasProject() ? FProjectSettings::Get().Maps.PlayerPrefab : std::string());
	World.OnPlayerJoined(FNetDriver::HostPlayerId, Players.SpawnPlayer(FNetDriver::HostPlayerId));
	FLanHostInfo LanInfo;
	LanInfo.Name       = std::format("{} (호스트)", FPaths::HasProject() ? FPaths::GetProjectName() : "ProjectE");
	LanInfo.Session    = FNetSessionInfo::FromProject(SceneAsset);
	LanInfo.GamePort   = Port;
	LanInfo.MaxPlayers = Net.MaxPlayers;
	Lan.StartHost(LanInfo);
}

void FRuntimeApplication::EndSession()
{
	Lan.Stop();
	Net.Shutdown();
	Net.OnPlayerJoined = nullptr;
	Net.OnPlayerLeft   = nullptr;
	Net.OnGameMessage  = nullptr;
	ReplicationServer.End();
	ReplicationClient.End();
	Players.End();
	World.EndPlay();
	AudioSystem.Reset(Audio);
}

void FRuntimeApplication::HandleSessionRequest(const FNetSessionRequest& Request)
{
	FNetLaunchOptions Options;
	switch (Request.Type)
	{
	case FNetSessionRequest::EType::Host:
		Options.Port = Request.Port != 0 ? Request.Port : GetConfiguredNetPort();
		if (Net.GetMode() == ENetMode::Standalone)
		{
			StartListenServer(Options.Port); // 지금 씬 그대로 호스트가 된다
			UpdateWindowTitle();
			return;
		}
		Options.Mode = ENetMode::ListenServer;
		break;
	case FNetSessionRequest::EType::Connect:
		Options.Mode           = ENetMode::Client;
		Options.ConnectAddress = Request.Address;
		break;
	case FNetSessionRequest::EType::Disconnect:
		if (Net.GetMode() == ENetMode::Standalone)
		{
			return;
		}
		break;
	}
	// 다른 세션으로: 정적 NetId는 "새로 로드한 씬"에 기대므로 씬을 다시 연다
	E_LOG(LogRuntime, Display, "세션 전환 → {}", Options.Mode == ENetMode::Client ? "클라이언트 " + Options.ConnectAddress : std::string(ToString(Options.Mode)));
	EndSession();
	LoadScene();
	StartSession(Options);
}

void FRuntimeApplication::UpdateWindowTitle()
{
	const std::string Project = FPaths::HasProject() ? FProjectSettings::Get().GetDisplayName() : "ProjectE";
	GetWindow().SetTitle(FStringConv::ToWide(Net.GetMode() == ENetMode::Standalone ? Project : std::format("{} [{}]", Project, ToString(Net.GetMode()))));
}
