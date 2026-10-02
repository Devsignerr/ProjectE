// 맵 전환: FGameWorld의 요청 부분(RequestOpenScene) + 앱 공용 전환 순서(FGameWorldTravel). 규칙은 GameWorldTravel.h 머리 주석
#include "World/GameWorldTravel.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "World/GameWorld.h"

#include <chrono>

E_DECLARE_LOG_CATEGORY(LogTravel)
E_DEFINE_LOG_CATEGORY(LogTravel, Log)

namespace
{
	std::filesystem::path ResolveScenePath(const std::filesystem::path& ContentDirectory, const std::string& SceneAsset)
	{
		const std::filesystem::path Path = FStringConv::ToWide(SceneAsset);
		return Path.is_absolute() ? Path : ContentDirectory / Path;
	}
} // namespace

bool FGameWorld::RequestOpenScene(const std::string& SceneAsset, std::string* OutError)
{
	const auto Fail = [OutError](std::string Reason) {
		if (OutError != nullptr)
		{
			*OutError = std::move(Reason);
		}
		return false;
	};
	if (!IsPlaying())
	{
		return Fail("플레이 중이 아닙니다");
	}
	if (Mode == ENetMode::Client)
	{
		return Fail("맵 전환은 서버에서만 할 수 있습니다 (클라이언트는 서버를 따라 이동한다)");
	}
	if (SceneAsset.empty() || !FFileSystem::Exists(ResolveScenePath(Systems.ContentDirectory, SceneAsset)))
	{
		return Fail("씬 파일이 없습니다: " + SceneAsset);
	}
	if (PendingSceneRequest && *PendingSceneRequest != SceneAsset)
	{
		E_LOG(LogTravel, Warning, "맵 전환 요청이 이번 프레임에 두 번 왔습니다 — 마지막 것({})으로 바꿉니다 (이전 {})", SceneAsset, *PendingSceneRequest);
	}
	PendingSceneRequest = SceneAsset;
	return true;
}

std::optional<std::string> FGameWorld::ConsumeOpenSceneRequest()
{
	std::optional<std::string> Request = std::move(PendingSceneRequest);
	PendingSceneRequest.reset();
	return Request;
}

bool FGameWorld::OpenScene(const std::string& SceneAsset)
{
	std::string Problem;
	if (!RequestOpenScene(SceneAsset, &Problem))
	{
		E_LOG(LogTravel, Warning, "게임 모듈 OpenScene({}) 거절: {}", SceneAsset, Problem);
		return false;
	}
	return true;
}

std::optional<std::string> FGameWorldTravel::ConsumePending(FGameWorld& World, FNetDriver* Net)
{
	if (World.GetMode() == ENetMode::Client)
	{
		World.ConsumeOpenSceneRequest(); // 클라이언트는 요청을 받지 않지만 남은 것이 있으면 버린다
		return Net != nullptr ? Net->ConsumeServerTravel() : std::nullopt;
	}
	return World.ConsumeOpenSceneRequest();
}

bool FGameWorldTravel::Travel(const FSceneTravelTargets& Targets, const std::string& SceneAsset)
{
	if (Targets.World == nullptr || Targets.Scene == nullptr)
	{
		return false;
	}
	FGameWorld&    World   = *Targets.World;
	FScene&        Scene   = *Targets.Scene;
	FNetDriver*    Net     = Targets.Net;
	const ENetMode Mode    = World.IsPlaying() ? World.GetMode() : (Net != nullptr ? Net->GetMode() : ENetMode::Standalone);
	const bool     bClient = Mode == ENetMode::Client;
	const auto     Start   = std::chrono::steady_clock::now();
	E_LOG(LogTravel, Display, "맵 전환 시작: {} → {} ({})", World.GetCurrentSceneAsset(), SceneAsset, ToString(Mode));

	// 1. 이전 씬 종료 (서버는 먼저 클라이언트들에게 이동을 지시 — 이후 그들에게 이전 씬 상태를 보내지 않는다)
	if (!bClient && Net != nullptr)
	{
		Net->BeginServerTravel(SceneAsset);
	}
	World.EndPlay(); // 엔티티를 참조하므로 씬을 비우기 전에
	if (Targets.OnEndPlay)
	{
		Targets.OnEndPlay();
	}
	if (Targets.ReplicationServer != nullptr)
	{
		Targets.ReplicationServer->End();
	}
	if (Targets.ReplicationClient != nullptr)
	{
		Targets.ReplicationClient->End();
	}
	if (Targets.Players != nullptr)
	{
		Targets.Players->End(); // 폰은 씬과 함께 사라진다
	}
	Scene.Clear();
	const auto Cleared = std::chrono::steady_clock::now();

	// 2. 새 씬
	const bool bLoaded = FSceneSerializer::LoadFromFile(Scene, ResolveScenePath(Targets.ContentDirectory, SceneAsset));
	if (!bLoaded)
	{
		E_LOG(LogTravel, Error, "맵 전환: 씬을 열지 못했습니다 — 빈 씬으로 계속합니다: {}", SceneAsset);
		Scene.Clear();
	}
	const auto Loaded = std::chrono::steady_clock::now();
	if (bLoaded && Targets.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(Scene, *Targets.Resources, Targets.ContentDirectory);
	}
	Scene.UpdateTransforms();
	World.SetCurrentSceneAsset(SceneAsset);

	// 3. 새 씬 시작 (정적 NetId는 스크립트가 엔티티를 만들기 전에)
	if (bClient)
	{
		if (Targets.ReplicationClient != nullptr)
		{
			Targets.ReplicationClient->Begin(Scene);
			World.SetReplicationClient(Targets.ReplicationClient); // EndPlay가 비웠다
		}
		World.BeginPlay(Scene, ENetMode::Client);
		if (Net != nullptr)
		{
			Net->CompleteClientTravel(); // 서버가 이 클라이언트에 새 씬 전체 상태를 보낸다
		}
	}
	else
	{
		if (Targets.ReplicationServer != nullptr && Net != nullptr)
		{
			Targets.ReplicationServer->Begin(Scene, *Net);
		}
		World.BeginPlay(Scene, Mode);
		const bool bOnline = Net != nullptr && Net->GetMode() != ENetMode::Standalone;
		if (Targets.Players != nullptr && bOnline)
		{
			Targets.Players->Begin(Scene, Targets.PlayerPrefab);
			if (Mode == ENetMode::ListenServer)
			{
				World.OnPlayerJoined(FNetDriver::HostPlayerId, Targets.Players->SpawnPlayer(FNetDriver::HostPlayerId));
			}
		}
	}

	const auto  End     = std::chrono::steady_clock::now();
	const auto  Ms      = [](auto From, auto To) { return std::chrono::duration<float, std::milli>(To - From).count(); };
	E_LOG(LogTravel, Display, "맵 전환 완료: {} (엔티티 {}개 — 이전 씬 정리 {:.1f}ms, 씬 파일 {:.1f}ms, 에셋·시작 {:.1f}ms, 전체 {:.1f}ms)", SceneAsset,
	      Scene.GetRegistry().GetAliveCount(), Ms(Start, Cleared), Ms(Cleared, Loaded), Ms(Loaded, End), Ms(Start, End));
	// 이전 맵만 쓰던 GPU 리소스 수거 (새 씬이 해석·렌더되어 루트가 채워진 몇 프레임 뒤 — Renderer/ResourceCollector.h)
	if (Targets.Resources != nullptr)
	{
		Targets.Resources->RequestGarbageCollection("맵 전환");
	}
	return bLoaded;
}
