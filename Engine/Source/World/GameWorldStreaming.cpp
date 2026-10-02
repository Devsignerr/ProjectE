// FGameWorld의 서브 씬 스트리밍 (Phase 31-2)
//   요청: Lua Scene.LoadSubScene/UnloadSubScene, 게임 모듈 IGameNet, 스트리밍 볼륨(FSubSceneVolumeComponent) — 서버/Standalone만
//   불러오기: 파일 읽기 + JSON 파싱은 std::async 백그라운드 스레드(FSceneSerializer::ParseFile — ECS 접근 없음), 끝나면 다음 게임플레이 틱
//            처음(스크립트 전, 메인 스레드)에 루트 엔티티("SubScene: <경로>", FTransientComponent + FSubSceneRootComponent, 위치 = Offset)
//            아래로 붙인다 → 프리팹 동기화 → 에셋 해석(GPU 업로드) → 복제 서버 등록(NetId 구간 + 클라이언트 SubSceneLoad) → 스크립트
//            OnSubSceneLoaded(경로). 서브 씬의 스크립트/물리 바디는 같은 틱의 스크립트 갱신·물리 갱신에서 생긴다
//   내리기: 클라이언트에 SubSceneUnload → 루트째 지연 파괴(스크립트 OnDestroy 뒤). 불러오는 중이면 결과를 버린다
//   볼륨: 기준(주 카메라, 캐릭터 이동 컴포넌트, FStreamingSourceComponent) 하나라도 상자 안이면 불러오고, 모두 상자 + UnloadMargin 밖이면
//         (기준이 없어도) 내린다. 스크립트가 불러온 것은 볼륨이 내리지 않는다
//   클라이언트: 판정·요청 없음. 서버의 SubSceneLoad를 받으면 그 자리에서(복제 메시지 처리 중) 동기로 붙인다 — 뒤따르는 상태 메시지가
//         그 엔티티를 가리키므로. NetId는 양쪽이 같은 파일을 붙인 하위 트리 순서 (NetReplication::AssignSubSceneNetIds)
//   맵 전환/EndPlay: 모두 버린다 (파싱 중인 스레드는 끝날 때까지 기다린다)
#include "World/GameWorld.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Physics/CharacterMovement.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/SubScene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

E_DECLARE_LOG_CATEGORY(LogTravel)

namespace
{
	std::filesystem::path ResolveSubScenePath(const std::filesystem::path& ContentDirectory, const std::string& Asset)
	{
		const std::filesystem::path Path = FStringConv::ToWide(Asset);
		return Path.is_absolute() ? Path : ContentDirectory / Path;
	}

	float MillisecondsSince(std::chrono::steady_clock::time_point Start)
	{
		return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - Start).count();
	}
} // namespace

void FGameWorld::SetReplicationClient(FReplicationClient* InReplication)
{
	Replication = InReplication;
	if (InReplication != nullptr)
	{
		InReplication->SetSubSceneHooks({
			[this](const std::string& Asset, uint32 InstanceId, const FVector3& Offset) { return ClientLoadSubScene(Asset, InstanceId, Offset); },
			[this](uint32 InstanceId) { ClientUnloadSubScene(InstanceId); },
		});
	}
}

FGameWorld::FSubSceneInstance* FGameWorld::FindSubScene(const std::string& Asset)
{
	const auto Found = std::find_if(SubScenes.begin(), SubScenes.end(), [&Asset](const std::unique_ptr<FSubSceneInstance>& Instance) { return Instance->Asset == Asset; });
	return Found != SubScenes.end() ? Found->get() : nullptr;
}

const FGameWorld::FSubSceneInstance* FGameWorld::FindSubScene(const std::string& Asset) const
{
	const auto Found = std::find_if(SubScenes.begin(), SubScenes.end(), [&Asset](const std::unique_ptr<FSubSceneInstance>& Instance) { return Instance->Asset == Asset; });
	return Found != SubScenes.end() ? Found->get() : nullptr;
}

bool FGameWorld::RequestLoadSubScene(const std::string& Asset, const FVector3& Offset, std::string* OutError)
{
	return StartSubSceneLoad(Asset, Offset, true, OutError);
}

bool FGameWorld::LoadSubScene(const std::string& Asset, const FVector3& Offset)
{
	std::string Problem;
	if (!StartSubSceneLoad(Asset, Offset, true, &Problem))
	{
		E_LOG(LogTravel, Warning, "게임 모듈 LoadSubScene({}) 거절: {}", Asset, Problem);
		return false;
	}
	return true;
}

bool FGameWorld::StartSubSceneLoad(const std::string& Asset, const FVector3& Offset, bool bByScript, std::string* OutError)
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
		return Fail("서브 씬은 서버에서만 불러올 수 있습니다 (클라이언트는 서버를 따른다)");
	}
	if (FSubSceneInstance* Existing = FindSubScene(Asset))
	{
		Existing->bByScript |= bByScript;
		Existing->bByVolume |= !bByScript;
		return true;
	}
	const std::filesystem::path Path = ResolveSubScenePath(Systems.ContentDirectory, Asset);
	if (Asset.empty() || !FFileSystem::Exists(Path))
	{
		return Fail("서브 씬 파일이 없습니다: " + Asset);
	}
	auto Instance        = std::make_unique<FSubSceneInstance>();
	Instance->Asset      = Asset;
	Instance->InstanceId = NextSubSceneId++;
	Instance->Offset     = Offset;
	Instance->bByScript  = bByScript;
	Instance->bByVolume  = !bByScript;
	const auto Parse     = [Path]() {
        const auto      Start = std::chrono::steady_clock::now();
        FParsedSubScene Result;
        Result.Document = FSceneSerializer::ParseFile(Path, &Result.Error);
        Result.ParseMs  = MillisecondsSince(Start);
        return Result;
	};
	Instance->Pending = std::async(bAsyncSubSceneLoad ? std::launch::async : std::launch::deferred, Parse);
	E_LOG(LogTravel, Display, "서브 씬 불러오기 시작: {} ({})", Asset, bByScript ? "요청" : "볼륨");
	SubScenes.push_back(std::move(Instance)); // 붙이기는 TickSubScenes (볼륨 요청은 같은 틱, 그 밖은 다음 틱 처음)
	return true;
}

FEntity FGameWorld::AttachSubScene(FSubSceneInstance& Instance, const FSceneDocument& Document)
{
	const auto Start = std::chrono::steady_clock::now();
	FRegistry& Registry = Scene->GetRegistry();
	const FEntity Root  = Scene->CreateEntity("SubScene: " + Instance.Asset);
	Registry.Emplace<FTransientComponent>(Root); // 씬 저장에서 뺀다
	Registry.Emplace<FSubSceneRootComponent>(Root, FSubSceneRootComponent{ Instance.Asset, Instance.InstanceId });
	Scene->GetTransform(Root).Position = Instance.Offset;
	const std::vector<FEntity> Entities = FSceneSerializer::AppendDocument(*Scene, Document, Root);
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	Scene->UpdateTransforms();
	Instance.Root = Root;
	const float AttachMs = MillisecondsSince(Start);
	SubSceneStats.LastAttachMs = AttachMs;
	SubSceneStats.MaxAttachMs  = std::max(SubSceneStats.MaxAttachMs, AttachMs);
	++SubSceneStats.Loads;
	E_LOG(LogTravel, Display, "서브 씬 붙임: {} (엔티티 {}개, 붙이기·에셋 {:.2f}ms — 메인 스레드)", Instance.Asset, Entities.size(), AttachMs);
	return Root;
}

void FGameWorld::TickSubScenes()
{
	if (!IsPlaying())
	{
		return;
	}
	if (Mode != ENetMode::Client)
	{
		UpdateStreamingVolumes();
	}
	for (size_t Index = 0; Index < SubScenes.size();)
	{
		FSubSceneInstance& Instance = *SubScenes[Index];
		if (Instance.Root.IsValid() || !Instance.Pending.valid())
		{
			++Index;
			continue;
		}
		if (bAsyncSubSceneLoad && Instance.Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++Index;
			continue;
		}
		FParsedSubScene Parsed = Instance.Pending.get();
		if (Parsed.Document == nullptr)
		{
			E_LOG(LogTravel, Error, "서브 씬을 불러오지 못했습니다: {}", Parsed.Error);
			SubScenes.erase(SubScenes.begin() + static_cast<std::ptrdiff_t>(Index));
			continue;
		}
		SubSceneStats.LastParseMs = Parsed.ParseMs;
		E_LOG(LogTravel, Display, "서브 씬 파싱 끝: {} ({:.2f}ms — 백그라운드)", Instance.Asset, Parsed.ParseMs);
		const FEntity Root = AttachSubScene(Instance, *Parsed.Document);
		if (ReplicationServer != nullptr)
		{
			ReplicationServer->RegisterSubScene(Root, Instance.Asset, Instance.InstanceId, Instance.Offset);
		}
		Systems.Scripts->BroadcastMethod("OnSubSceneLoaded", { FGameRpcValue::MakeString(Instance.Asset) });
		++Index;
	}
}

void FGameWorld::UpdateStreamingVolumes()
{
	FRegistry& Registry = Scene->GetRegistry();
	if (Registry.TryGetPool<FSubSceneVolumeComponent>() == nullptr)
	{
		return;
	}
	// 위치 = 부모 월드 × 로컬 위치 (이번 틱 전에 만들어진 폰 등은 WorldMatrix가 아직 갱신 전일 수 있다)
	std::vector<FVector3> Sources;
	const auto            AddSource = [&](FEntity Entity, const FTransformComponent& Transform) {
        Sources.push_back(Scene->GetParentWorldMatrix(Entity).TransformPosition(Transform.Position));
	};
	Registry.View<FCameraComponent, FTransformComponent>().Each([&](FEntity Entity, FCameraComponent& Camera, FTransformComponent& Transform) {
		if (Camera.bPrimary)
		{
			AddSource(Entity, Transform);
		}
	});
	Registry.View<FCharacterMovementComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FCharacterMovementComponent&, FTransformComponent& Transform) { AddSource(Entity, Transform); });
	Registry.View<FStreamingSourceComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FStreamingSourceComponent&, FTransformComponent& Transform) { AddSource(Entity, Transform); });

	struct FDecision
	{
		std::string Asset;
		bool        bLoad = false;
	};
	std::vector<FDecision> Decisions; // 뷰 순회가 끝난 뒤 적용 (불러오기가 엔티티를 만든다)
	Registry.View<FSubSceneVolumeComponent, FTransformComponent>().Each([&](FEntity Entity, FSubSceneVolumeComponent& Volume, FTransformComponent& Transform) {
		if (Volume.SubScene.empty())
		{
			return;
		}
		const FVector3 Center = Scene->GetParentWorldMatrix(Entity).TransformPosition(Transform.Position);
		const auto     Within = [&](const FVector3& Point, float Margin) {
            const FVector3 Delta = Point - Center;
            return std::abs(Delta.X) <= Volume.HalfExtents.X + Margin && std::abs(Delta.Y) <= Volume.HalfExtents.Y + Margin &&
                   std::abs(Delta.Z) <= Volume.HalfExtents.Z + Margin;
		};
		bool bInside = false, bNearby = false;
		for (const FVector3& Source : Sources)
		{
			bInside = bInside || Within(Source, 0.0f);
			bNearby = bNearby || Within(Source, std::max(Volume.UnloadMargin, 0.0f));
		}
		const FSubSceneInstance* Existing = FindSubScene(Volume.SubScene);
		if (bInside && (Existing == nullptr || !Existing->bByVolume))
		{
			Decisions.push_back({ Volume.SubScene, true });
		}
		else if (!bNearby && Existing != nullptr && Existing->bByVolume && !Existing->bByScript)
		{
			Decisions.push_back({ Volume.SubScene, false });
		}
	});
	for (const FDecision& Decision : Decisions)
	{
		if (Decision.bLoad)
		{
			std::string Problem;
			if (!StartSubSceneLoad(Decision.Asset, FVector3::ZeroVector, false, &Problem))
			{
				E_LOG(LogTravel, Warning, "스트리밍 볼륨: {}", Problem);
			}
		}
		else if (FSubSceneInstance* Instance = FindSubScene(Decision.Asset))
		{
			Instance->bByVolume = false;
			UnloadSubScene(Decision.Asset);
		}
	}
}

void FGameWorld::DestroySubScene(FSubSceneInstance& Instance)
{
	if (!Instance.Root.IsValid() || Scene == nullptr || !Scene->GetRegistry().IsValid(Instance.Root))
	{
		return;
	}
	// 스크립트 OnDestroy 뒤 프레임 끝에 (플레이 중이 아니면 바로)
	if (!Systems.Scripts->RequestDestroy(Instance.Root))
	{
		Scene->DestroyEntity(Instance.Root);
	}
	++SubSceneStats.Unloads;
	E_LOG(LogTravel, Display, "서브 씬 내림: {}", Instance.Asset);
	if (Systems.Resources != nullptr)
	{
		Systems.Resources->RequestGarbageCollection("서브 씬 내림"); // 파괴는 프레임 끝 — 수거는 몇 프레임 뒤
	}
}

bool FGameWorld::UnloadSubScene(const std::string& Asset)
{
	if (!IsPlaying() || Mode == ENetMode::Client)
	{
		return false;
	}
	const auto Found = std::find_if(SubScenes.begin(), SubScenes.end(), [&Asset](const std::unique_ptr<FSubSceneInstance>& Instance) { return Instance->Asset == Asset; });
	if (Found == SubScenes.end())
	{
		return false;
	}
	std::unique_ptr<FSubSceneInstance> Instance = std::move(*Found);
	SubScenes.erase(Found);
	if (Instance->Root.IsValid() && ReplicationServer != nullptr)
	{
		ReplicationServer->UnregisterSubScene(Instance->InstanceId);
	}
	DestroySubScene(*Instance); // 아직 파싱 중이면 future 소멸이 끝날 때까지 기다린다 (결과는 버린다)
	return true;
}

bool FGameWorld::IsSubSceneLoaded(const std::string& Asset) const
{
	const FSubSceneInstance* Instance = FindSubScene(Asset);
	return Instance != nullptr && Instance->Root.IsValid();
}

FEntity FGameWorld::GetSubSceneRoot(const std::string& Asset) const
{
	const FSubSceneInstance* Instance = FindSubScene(Asset);
	return Instance != nullptr ? Instance->Root : NullEntity;
}

FEntity FGameWorld::ClientLoadSubScene(const std::string& Asset, uint32 InstanceId, const FVector3& Offset)
{
	if (!IsPlaying() || Mode != ENetMode::Client)
	{
		return NullEntity;
	}
	std::string Error;
	const auto  Start    = std::chrono::steady_clock::now();
	const auto  Document = FSceneSerializer::ParseFile(ResolveSubScenePath(Systems.ContentDirectory, Asset), &Error);
	if (Document == nullptr)
	{
		E_LOG(LogTravel, Error, "서버가 불러온 서브 씬을 열지 못했습니다: {}", Error);
		return NullEntity;
	}
	SubSceneStats.LastParseMs = MillisecondsSince(Start);
	auto Instance        = std::make_unique<FSubSceneInstance>();
	Instance->Asset      = Asset;
	Instance->InstanceId = InstanceId;
	Instance->Offset     = Offset;
	const FEntity Root   = AttachSubScene(*Instance, *Document);
	SubScenes.push_back(std::move(Instance));
	Systems.Scripts->BroadcastMethod("OnSubSceneLoaded", { FGameRpcValue::MakeString(Asset) });
	return Root;
}

void FGameWorld::ClientUnloadSubScene(uint32 InstanceId)
{
	const auto Found = std::find_if(SubScenes.begin(), SubScenes.end(), [InstanceId](const std::unique_ptr<FSubSceneInstance>& Instance) { return Instance->InstanceId == InstanceId; });
	if (Found == SubScenes.end())
	{
		return;
	}
	std::unique_ptr<FSubSceneInstance> Instance = std::move(*Found);
	SubScenes.erase(Found);
	DestroySubScene(*Instance);
}

void FGameWorld::ClearSubScenes()
{
	SubScenes.clear(); // 엔티티는 씬과 함께 정리된다. 파싱 중인 future는 소멸하며 기다린다
	NextSubSceneId = 1;
	SubSceneStats  = {};
}
