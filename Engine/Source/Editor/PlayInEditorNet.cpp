#include "Editor/PlayInEditorNet.h"

#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/PlayMode.h"
#include "Network/NetTransport.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "World/GameWorld.h"

#include <format>
#include <fstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring Quote(const std::wstring& Text)
	{
		return L"\"" + Text + L"\"";
	}

	std::wstring SimulationArguments(const FPlayNetSettings& Settings)
	{
		return Settings.LatencyMs > 0 || Settings.LossPercent > 0.0f ? std::format(L" --net-lag {} --net-loss {}", Settings.LatencyMs, Settings.LossPercent) : std::wstring();
	}
} // namespace

FPlayInEditorNet::~FPlayInEditorNet()
{
	Stop();
}

void FPlayInEditorNet::Init(FGameWorld& InWorld, FResourceManager* InResources, std::filesystem::path InContentDirectory)
{
	World            = &InWorld;
	Resources        = InResources;
	ContentDirectory = std::move(InContentDirectory);
}

bool FPlayInEditorNet::Prepare(const FPlayNetSettings& InSettings, FScene& EditScene, FPlayOptions& OutOptions)
{
	Stop();
	if (InSettings.Mode == FPlayNetSettings::EMode::Standalone || World == nullptr)
	{
		return false;
	}
	Settings = InSettings;

	// 모든 프로세스가 같은 파일을 연다 (절대 경로 문자열이 곧 핸드셰이크의 씬 이름)
	const std::filesystem::path Directory = FPaths::GetSavedDirectory() / L"PlayInEditor";
	std::filesystem::create_directories(Directory);
	const std::filesystem::path ScenePath = Directory / L"PIE.escene";
	SceneJson                             = FSceneSerializer::ToJsonString(EditScene);
	{
		std::ofstream File(ScenePath, std::ios::binary | std::ios::trunc);
		File << SceneJson;
		if (!File)
		{
			E_LOG(LogEditor, Error, "플레이 씬 파일을 쓰지 못했습니다: {}", FStringConv::ToUtf8(ScenePath.wstring()));
			return false;
		}
	}
	SceneAsset = FStringConv::ToUtf8(std::filesystem::absolute(ScenePath).wstring());

	const FNetSessionInfo Session = FNetSessionInfo::FromProject(SceneAsset, "에디터");
	OutOptions.SceneJson          = &SceneJson;
	if (Settings.Mode == FPlayNetSettings::EMode::ListenServer)
	{
		Mode               = ENetMode::ListenServer;
		OutOptions.Mode    = ENetMode::ListenServer;
		OutOptions.BeforeBeginPlay = [this, Session](FScene& Scene) {
			PlayScene = &Scene;
			if (!Net.StartServer(CreateGnsTransport(), Settings.Port, Session, false))
			{
				E_LOG(LogEditor, Error, "포트 {}에서 리슨 서버를 열지 못했습니다", Settings.Port);
			}
			ApplySimulation();
			ReplicationServer.Begin(Scene, Net); // 정적 NetId는 스크립트가 엔티티를 만들기 전에
			Net.OnPlayerJoined = [this](const FNetDriver::FRemotePlayer& Player) {
				const FEntity Pawn = Players.SpawnPlayer(Player.PlayerId);
				ReplicationServer.OnPlayerJoined(Player.Connection);
				World->OnPlayerJoined(Player.PlayerId, Pawn);
			};
			Net.OnPlayerLeft = [this](const FNetDriver::FRemotePlayer& Player, const std::string&) {
				World->OnPlayerLeft(Player.PlayerId);
				Players.DespawnPlayer(Player.PlayerId);
			};
			Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) { World->HandleNetMessage(Connection, Message); };
		};
	}
	else
	{
		// 전용 서버 프로세스를 먼저 띄우고 에디터는 클라이언트로 (GNS가 서버가 열릴 때까지 접속을 재시도한다)
		const std::wstring Arguments = std::format(L"--project {} --scene {} --port {} --log {}", Quote(FPaths::GetProjectFile().wstring()),
		                                           Quote(FStringConv::ToWide(SceneAsset)), Settings.Port, Quote((Directory / L"Server.log").wstring())) +
		                               SimulationArguments(Settings);
		if (!LaunchProcess(FPaths::GetExecutableDirectory() / L"ProjectEServer.exe", Arguments))
		{
			return false;
		}
		Mode                       = ENetMode::Client;
		OutOptions.Mode            = ENetMode::Client;
		OutOptions.BeforeBeginPlay = [this, Session](FScene& Scene) {
			PlayScene = &Scene;
			ReplicationClient.Begin(Scene);
			Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) {
				if (!ReplicationClient.HandleMessage(Message))
				{
					World->HandleNetMessage(Connection, Message);
				}
			};
			Net.StartClient(CreateGnsTransport(), std::format("127.0.0.1:{}", Settings.Port), Session);
			ApplySimulation();
		};
	}
	return true;
}

void FPlayInEditorNet::AfterPlay()
{
	if (!IsActive() || PlayScene == nullptr)
	{
		return;
	}
	if (Mode == ENetMode::ListenServer && Net.GetMode() == ENetMode::ListenServer)
	{
		Players.Begin(*PlayScene, FPaths::HasProject() ? FProjectSettings::Get().Maps.PlayerPrefab : std::string());
		World->OnPlayerJoined(FNetDriver::HostPlayerId, Players.SpawnPlayer(FNetDriver::HostPlayerId));
		FLanHostInfo LanInfo;
		LanInfo.Name       = std::format("{} (에디터)", FPaths::GetProjectName());
		LanInfo.Session    = FNetSessionInfo::FromProject(SceneAsset);
		LanInfo.GamePort   = Settings.Port;
		LanInfo.MaxPlayers = Net.MaxPlayers;
		LanHost.StartHost(LanInfo);
	}
	for (int32 Index = 0; Index < Settings.ClientCount; ++Index)
	{
		LaunchRuntimeClient(std::format("127.0.0.1:{}", Settings.Port), SceneAsset);
	}
	E_LOG(LogEditor, Display, "네트워크 플레이: {} + 런타임 클라이언트 {}개", ToString(Mode), Settings.ClientCount);
}

void FPlayInEditorNet::Update(float DeltaSeconds)
{
	if (!IsActive())
	{
		return;
	}
	Net.Update(DeltaSeconds);
	if (Mode == ENetMode::Client && PlayScene != nullptr)
	{
		ReplicationClient.Update(DeltaSeconds);
		if (ReplicationClient.ConsumeAssetsChanged() && Resources != nullptr)
		{
			FSceneAssetResolver::Resolve(*PlayScene, *Resources, ContentDirectory);
		}
	}
	if (LanHost.IsHosting())
	{
		LanHost.SetPlayerCount(static_cast<uint16>(Net.GetPlayers().size() + 1));
		LanHost.Update();
	}
}

void FPlayInEditorNet::PostTick(float DeltaSeconds)
{
	if (Mode == ENetMode::ListenServer)
	{
		ReplicationServer.Tick(DeltaSeconds);
	}
}

void FPlayInEditorNet::Stop()
{
	LanHost.Stop();
	Net.Shutdown();
	Net.OnPlayerJoined = nullptr;
	Net.OnPlayerLeft   = nullptr;
	Net.OnGameMessage  = nullptr;
	ReplicationServer.End();
	ReplicationClient.End();
	Players.End();
	TerminateChildren();
	Mode      = ENetMode::Standalone;
	PlayScene = nullptr;
}

void FPlayInEditorNet::ApplySimulation()
{
	Settings.LatencyMs   = PendingSettings.LatencyMs;
	Settings.LossPercent = PendingSettings.LossPercent;
	Net.SetSimulation(Settings.LatencyMs, Settings.LossPercent);
}

int32 FPlayInEditorNet::GetChildProcessCount() const
{
	int32 Running = 0;
	for (void* Process : ChildProcesses)
	{
		Running += WaitForSingleObject(static_cast<HANDLE>(Process), 0) == WAIT_TIMEOUT ? 1 : 0;
	}
	return Running;
}

bool FPlayInEditorNet::LaunchRuntimeClient(const std::string& Address, const std::string& ClientSceneAsset)
{
	// 로그: Saved/PlayInEditor/Client<N>.log (창마다 따로)
	const std::filesystem::path LogPath   = FPaths::GetSavedDirectory() / L"PlayInEditor" / std::format(L"Client{}.log", ++LaunchedClientCount);
	const std::wstring          Arguments = std::format(L"--project {} --scene {} --connect {} --log {}", Quote(FPaths::GetProjectFile().wstring()),
	                                                    Quote(FStringConv::ToWide(ClientSceneAsset)), FStringConv::ToWide(Address), Quote(LogPath.wstring())) +
	                               SimulationArguments(PendingSettings);
	return LaunchProcess(FPaths::GetExecutableDirectory() / L"ProjectERuntime.exe", Arguments);
}

bool FPlayInEditorNet::LaunchProcess(const std::wstring& Executable, const std::wstring& Arguments)
{
	std::wstring        CommandLine = Quote(Executable) + L" " + Arguments; // CreateProcessW는 쓰기 가능한 버퍼를 요구한다
	STARTUPINFOW        StartupInfo{};
	PROCESS_INFORMATION ProcessInfo{};
	StartupInfo.cb = sizeof(StartupInfo);
	if (!CreateProcessW(Executable.c_str(), CommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, FPaths::GetExecutableDirectory().c_str(), &StartupInfo,
	                    &ProcessInfo))
	{
		E_LOG(LogEditor, Error, "프로세스를 실행하지 못했습니다 ({}): {}", GetLastError(), FStringConv::ToUtf8(Executable));
		return false;
	}
	CloseHandle(ProcessInfo.hThread);
	ChildProcesses.push_back(ProcessInfo.hProcess);
	return true;
}

void FPlayInEditorNet::TerminateChildren()
{
	for (void* Process : ChildProcesses)
	{
		TerminateProcess(static_cast<HANDLE>(Process), 0);
		CloseHandle(static_cast<HANDLE>(Process));
	}
	ChildProcesses.clear();
}
