#include "Network/ReplicationServer.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <functional>

// 복제 메시지 형식 (신뢰 채널, 리틀 엔디언):
//   ReplicationSpawn:   uint32 개수, [uint32 NetId, uint8 종류(1 엔티티, 2 프리팹), uint32 부모 NetId, string 이름,
//                                    string 프리팹 경로, uint32 링크 수, [string 링크 ID, uint32 NetId]...]...
//   ReplicationDestroy: uint32 개수, [uint32 NetId]...
//   ReplicationState:   uint32 엔티티 수, [uint32 NetId, uint32 컴포넌트 수,
//                                          [string 타입 이름, uint8 동작(0 설정, 1 제거), (설정만) uint32 바이트 수, 컴포넌트 바이트]...]...
//     컴포넌트 바이트 = NetReplication::WriteComponent (모르는 타입은 바이트 수로 건너뛴다)

void FReplicationServer::Begin(FScene& InScene, FNetDriver& InDriver)
{
	End();
	Scene            = &InScene;
	Driver           = &InDriver;
	NextDynamicNetId = DynamicNetIdBase;

	NetReplication::AssignStaticNetIds(InScene);
	InScene.GetRegistry().View<FNetIdComponent>().Each([this](FEntity Entity, FNetIdComponent& NetId) {
		FTracked& Entry = Tracked[NetId.NetId];
		Entry.Entity    = Entity;
		Entry.Kind      = ESpawnKind::Static;
	});
	// 정적 엔티티는 클라이언트도 같은 값으로 로드하므로 현재 값을 "보낸 것"으로 기록한다
	BuildStateMessage(GetSortedNetIds(), false, true);
}

void FReplicationServer::End()
{
	Scene  = nullptr;
	Driver = nullptr;
	Tracked.clear();
	SendAccumulator = 0.0f;
}

void FReplicationServer::Tick(float DeltaSeconds)
{
	const ENetMode Mode = Driver != nullptr ? Driver->GetMode() : ENetMode::Standalone;
	if (Scene == nullptr || (Mode != ENetMode::ListenServer && Mode != ENetMode::DedicatedServer))
	{
		return; // Standalone(네트워크 없음)/클라이언트는 보내지 않는다
	}
	const float Interval = 1.0f / std::max(SendRate, 1.0f);
	SendAccumulator += DeltaSeconds;
	if (SendAccumulator < Interval)
	{
		return;
	}
	SendAccumulator = std::min(SendAccumulator - Interval, Interval); // 밀려도 한 번만 보낸다

	std::vector<uint32> Spawned;
	std::vector<uint32> Destroyed;
	DiscoverNewEntities(Spawned);
	CollectDestroyed(Destroyed);

	if (!Spawned.empty())
	{
		Driver->Broadcast(BuildSpawnMessage(Spawned), ENetReliability::Reliable);
	}
	if (!Destroyed.empty())
	{
		FBinaryWriter Writer;
		Writer.Write(static_cast<uint8>(ENetMessageType::ReplicationDestroy));
		Writer.Write(static_cast<uint32>(Destroyed.size()));
		for (const uint32 NetId : Destroyed)
		{
			Writer.Write(NetId);
		}
		Driver->Broadcast(Writer.GetBuffer(), ENetReliability::Reliable);
	}
	// 플레이어가 없어도 기록은 갱신한다 (다음 입장자는 어차피 전체 값을 받는다)
	const std::vector<uint8> State = BuildStateMessage(GetSortedNetIds(), false, true);
	if (!State.empty())
	{
		Driver->Broadcast(State, ENetReliability::Reliable);
	}
}

void FReplicationServer::OnPlayerJoined(FNetConnectionId Connection)
{
	if (Scene == nullptr || Driver == nullptr)
	{
		return;
	}
	const std::vector<uint32> All = GetSortedNetIds();
	std::vector<uint32>       Roots;
	for (const uint32 NetId : All)
	{
		const ESpawnKind Kind = Tracked.at(NetId).Kind;
		if (Kind == ESpawnKind::Entity || Kind == ESpawnKind::Prefab)
		{
			Roots.push_back(NetId);
		}
	}
	if (!Roots.empty())
	{
		Driver->Send(Connection, BuildSpawnMessage(Roots), ENetReliability::Reliable);
	}
	const std::vector<uint8> State = BuildStateMessage(All, true, false);
	if (!State.empty())
	{
		Driver->Send(Connection, State, ENetReliability::Reliable);
	}
}

void FReplicationServer::DiscoverNewEntities(std::vector<uint32>& OutSpawned)
{
	FRegistry&           Registry = Scene->GetRegistry();
	std::vector<FEntity> Candidates;
	Registry.View<FReplicatedComponent>().Each([&](FEntity Entity, FReplicatedComponent&) {
		if (!Registry.Has<FNetIdComponent>(Entity))
		{
			Candidates.push_back(Entity);
		}
	});
	std::sort(Candidates.begin(), Candidates.end(), [](FEntity A, FEntity B) { return A.Index < B.Index; });

	const auto Assign = [&](FEntity Entity, ESpawnKind Kind) -> FTracked& {
		const uint32 NetId                                   = NextDynamicNetId++;
		Registry.GetOrEmplace<FNetIdComponent>(Entity).NetId = NetId;
		FTracked& Entry                                      = Tracked[NetId];
		Entry.Entity                                         = Entity;
		Entry.Kind                                           = Kind;
		return Entry;
	};

	for (const FEntity Entity : Candidates)
	{
		if (Registry.Has<FNetIdComponent>(Entity))
		{
			continue; // 앞 후보의 프리팹과 함께 처리됨
		}
		const FEntity Root = FPrefabLibrary::FindInstanceRoot(*Scene, Entity);
		if (Root.IsValid() && Registry.Has<FReplicatedComponent>(Root) && !Registry.Has<FNetIdComponent>(Root))
		{
			// 프리팹 인스턴스: 루트와 서브트리의 복제 엔티티를 함께 생성 (클라이언트는 같은 프리팹을 만들어 링크 ID로 짝짓는다)
			FTracked& RootEntry   = Assign(Root, ESpawnKind::Prefab);
			RootEntry.PrefabAsset = Registry.Get<FPrefabInstanceComponent>(Root).Asset;
			RootEntry.ParentNetId = NetReplication::GetNetId(*Scene, Scene->GetParent(Root));
			const uint32 RootId   = Registry.Get<FNetIdComponent>(Root).NetId;
			OutSpawned.push_back(RootId);

			std::vector<std::pair<std::string, uint32>> Links;
			std::function<void(FEntity)>                Visit = [&](FEntity Node) {
                for (const FEntity Child : Scene->GetChildren(Node))
                {
                    const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Child);
                    if (Link != nullptr && Link->Root == Root && Registry.Has<FReplicatedComponent>(Child) && !Registry.Has<FNetIdComponent>(Child))
                    {
                        Assign(Child, ESpawnKind::PrefabChild);
                        Links.emplace_back(Link->Id, Registry.Get<FNetIdComponent>(Child).NetId);
                    }
                    Visit(Child);
                }
			};
			Visit(Root);
			Tracked.at(RootId).PrefabLinks = std::move(Links);
			continue;
		}

		FTracked& Entry   = Assign(Entity, ESpawnKind::Entity);
		Entry.ParentNetId = NetReplication::GetNetId(*Scene, Scene->GetParent(Entity));
		if (const FNameComponent* Name = Registry.TryGet<FNameComponent>(Entity))
		{
			Entry.Name = Name->Name;
		}
		OutSpawned.push_back(Registry.Get<FNetIdComponent>(Entity).NetId);
	}
}

void FReplicationServer::CollectDestroyed(std::vector<uint32>& OutDestroyed)
{
	FRegistry& Registry = Scene->GetRegistry();
	for (auto It = Tracked.begin(); It != Tracked.end();)
	{
		const FEntity Entity = It->second.Entity;
		if (!Registry.IsValid(Entity) || !Registry.Has<FReplicatedComponent>(Entity))
		{
			if (Registry.IsValid(Entity))
			{
				Registry.Remove<FNetIdComponent>(Entity);
			}
			OutDestroyed.push_back(It->first);
			It = Tracked.erase(It);
		}
		else
		{
			++It;
		}
	}
	std::sort(OutDestroyed.begin(), OutDestroyed.end());
}

std::vector<uint8> FReplicationServer::BuildStateMessage(const std::vector<uint32>& NetIds, bool bFull, bool bCommit)
{
	FRegistry&                           Registry = Scene->GetRegistry();
	const NetReplication::FEntityToNetId ToNetId  = [this](FEntity Entity) { return NetReplication::GetNetId(*Scene, Entity); };

	FBinaryWriter Body;
	uint32        EntityCount = 0;
	for (const uint32 NetId : NetIds)
	{
		FTracked&     Entry  = Tracked.at(NetId);
		const FEntity Entity = Entry.Entity;
		if (!Registry.IsValid(Entity))
		{
			continue;
		}

		FBinaryWriter            Components;
		uint32                   ComponentCount = 0;
		std::vector<std::string> Present;
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			if (!NetReplication::IsReplicated(Type) || !Type.HasComponent(Registry, Entity))
			{
				return;
			}
			Present.push_back(Type.Name);
			FBinaryWriter Value;
			NetReplication::WriteComponent(Value, Type, Type.GetComponent(Registry, Entity), ToNetId);
			const auto Found    = Entry.LastSent.find(Type.Name);
			const bool bChanged = Found == Entry.LastSent.end() || Found->second != Value.GetBuffer();
			if (bFull || bChanged)
			{
				Components.WriteString(Type.Name);
				Components.Write(static_cast<uint8>(0));
				Components.WriteArray(Value.GetBuffer());
				++ComponentCount;
			}
			if (bCommit)
			{
				Entry.LastSent[Type.Name] = Value.GetBuffer();
			}
		});
		// 지난번에 있었는데 없어진 컴포넌트
		std::vector<std::string> Removed;
		for (const auto& [TypeName, Value] : Entry.LastSent)
		{
			if (std::find(Present.begin(), Present.end(), TypeName) == Present.end())
			{
				Components.WriteString(TypeName);
				Components.Write(static_cast<uint8>(1));
				++ComponentCount;
				Removed.push_back(TypeName);
			}
		}
		if (bCommit)
		{
			for (const std::string& TypeName : Removed)
			{
				Entry.LastSent.erase(TypeName);
			}
		}

		if (ComponentCount > 0)
		{
			Body.Write(NetId);
			Body.Write(ComponentCount);
			Body.WriteBytes(Components.GetBuffer().data(), Components.GetBuffer().size());
			++EntityCount;
		}
	}
	if (EntityCount == 0)
	{
		return {};
	}
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::ReplicationState));
	Writer.Write(EntityCount);
	Writer.WriteBytes(Body.GetBuffer().data(), Body.GetBuffer().size());
	return Writer.GetBuffer();
}

std::vector<uint8> FReplicationServer::BuildSpawnMessage(const std::vector<uint32>& NetIds) const
{
	FBinaryWriter Writer;
	Writer.Write(static_cast<uint8>(ENetMessageType::ReplicationSpawn));
	Writer.Write(static_cast<uint32>(NetIds.size()));
	for (const uint32 NetId : NetIds)
	{
		const FTracked& Entry = Tracked.at(NetId);
		Writer.Write(NetId);
		Writer.Write(static_cast<uint8>(Entry.Kind == ESpawnKind::Prefab ? 2 : 1));
		Writer.Write(Entry.ParentNetId);
		Writer.WriteString(Entry.Name);
		Writer.WriteString(Entry.PrefabAsset);
		Writer.Write(static_cast<uint32>(Entry.PrefabLinks.size()));
		for (const auto& [LinkId, LinkNetId] : Entry.PrefabLinks)
		{
			Writer.WriteString(LinkId);
			Writer.Write(LinkNetId);
		}
	}
	return Writer.GetBuffer();
}

std::vector<uint32> FReplicationServer::GetSortedNetIds() const
{
	std::vector<uint32> NetIds;
	NetIds.reserve(Tracked.size());
	for (const auto& [NetId, Entry] : Tracked)
	{
		NetIds.push_back(NetId);
	}
	std::sort(NetIds.begin(), NetIds.end()); // 생성 순서 (부모가 먼저)
	return NetIds;
}
