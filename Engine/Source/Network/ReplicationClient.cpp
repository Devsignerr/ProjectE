#include "Network/ReplicationClient.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetMessages.h"
#include "Network/NetTypes.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

void FReplicationClient::Begin(FScene& InScene)
{
	End();
	Scene = &InScene;
	NetReplication::AssignStaticNetIds(InScene);
	InScene.GetRegistry().View<FNetIdComponent>().Each([this](FEntity Entity, FNetIdComponent& NetId) { Entities[NetId.NetId] = Entity; });
}

void FReplicationClient::End()
{
	Scene = nullptr;
	Entities.clear();
	bAssetsChanged = false;
	TransformBuffers.clear();
	bClockValid = false;
}

bool FReplicationClient::HandleMessage(const std::vector<uint8>& Message)
{
	if (Scene == nullptr)
	{
		return false;
	}
	switch (NetMessages::PeekType(Message).value_or(ENetMessageType::Hello))
	{
	case ENetMessageType::ReplicationSpawn:   ApplySpawn(Message); return true;
	case ENetMessageType::ReplicationDestroy: ApplyDestroy(Message); return true;
	case ENetMessageType::ReplicationState:   ApplyState(Message); return true;
	case ENetMessageType::TransformSnapshot:  ApplyTransformSnapshot(Message); return true;
	default:                                  return false;
	}
}

bool FReplicationClient::ConsumeAssetsChanged()
{
	const bool bChanged = bAssetsChanged;
	bAssetsChanged      = false;
	return bChanged;
}

FEntity FReplicationClient::FindEntity(uint32 NetId) const
{
	const auto Found = Entities.find(NetId);
	return Found != Entities.end() && Scene->GetRegistry().IsValid(Found->second) ? Found->second : NullEntity;
}

void FReplicationClient::ApplySpawn(const std::vector<uint8>& Message)
{
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32 Count    = Reader.Read<uint32>();
	FRegistry&   Registry = Scene->GetRegistry();
	for (uint32 Index = 0; Index < Count && Reader.IsOk(); ++Index)
	{
		const uint32      NetId       = Reader.Read<uint32>();
		const uint8       Kind        = Reader.Read<uint8>();
		const uint32      ParentNetId = Reader.Read<uint32>();
		const std::string Name        = Reader.ReadString();
		const std::string PrefabAsset = Reader.ReadString();
		const uint32      LinkCount   = Reader.Read<uint32>();
		std::unordered_map<std::string, uint32> Links;
		for (uint32 LinkIndex = 0; LinkIndex < LinkCount && Reader.IsOk(); ++LinkIndex)
		{
			std::string LinkId = Reader.ReadString();
			Links[std::move(LinkId)] = Reader.Read<uint32>();
		}
		if (!Reader.IsOk() || FindEntity(NetId).IsValid())
		{
			continue;
		}

		const FEntity Parent = FindEntity(ParentNetId);
		FEntity       Entity;
		if (Kind == 2)
		{
			std::string Error;
			Entity = FPrefabLibrary::Get().Instantiate(*Scene, PrefabAsset, Parent, &Error);
			if (!Entity.IsValid())
			{
				E_LOG(LogNet, Error, "복제 프리팹 생성 실패 ({}): {}", PrefabAsset, Error);
				continue;
			}
			// 서버와 같은 프리팹이므로 링크 ID로 복제 대상 하위 엔티티를 짝짓는다
			std::function<void(FEntity)> Visit = [&](FEntity Node) {
				for (const FEntity Child : Scene->GetChildren(Node))
				{
					if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Child))
					{
						if (const auto Found = Links.find(Link->Id); Found != Links.end())
						{
							Registry.GetOrEmplace<FNetIdComponent>(Child).NetId = Found->second;
							Entities[Found->second]                             = Child;
						}
					}
					Visit(Child);
				}
			};
			Visit(Entity);
		}
		else
		{
			Entity = Scene->CreateEntity(Name);
			if (Parent.IsValid())
			{
				Scene->SetParent(Entity, Parent);
			}
		}
		Registry.GetOrEmplace<FNetIdComponent>(Entity).NetId = NetId;
		Entities[NetId]                                      = Entity;
		bAssetsChanged                                       = true;
	}
}

void FReplicationClient::ApplyDestroy(const std::vector<uint8>& Message)
{
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const uint32 Count = Reader.Read<uint32>();
	for (uint32 Index = 0; Index < Count && Reader.IsOk(); ++Index)
	{
		const uint32  NetId  = Reader.Read<uint32>();
		const FEntity Entity = FindEntity(NetId);
		if (Entity.IsValid())
		{
			Scene->DestroyEntity(Entity); // 하위도 함께 (하위 NetId는 FindEntity에서 무효로 걸러진다)
		}
		Entities.erase(NetId);
	}
}

void FReplicationClient::ApplyState(const std::vector<uint8>& Message)
{
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	FRegistry&                           Registry    = Scene->GetRegistry();
	const NetReplication::FNetIdToEntity ToEntity    = [this](uint32 NetId) { return FindEntity(NetId); };
	const uint32                         EntityCount = Reader.Read<uint32>();
	for (uint32 EntityIndex = 0; EntityIndex < EntityCount && Reader.IsOk(); ++EntityIndex)
	{
		const uint32  NetId          = Reader.Read<uint32>();
		const uint32  ComponentCount = Reader.Read<uint32>();
		const FEntity Entity         = FindEntity(NetId);
		for (uint32 ComponentIndex = 0; ComponentIndex < ComponentCount && Reader.IsOk(); ++ComponentIndex)
		{
			const std::string   TypeName = Reader.ReadString();
			const uint8         Op       = Reader.Read<uint8>();
			std::vector<uint8>  Bytes    = Op == 0 ? Reader.ReadArray<uint8>() : std::vector<uint8>();
			const FTypeInfo*    Type     = FTypeRegistry::Get().Find(TypeName);
			if (!Entity.IsValid() || Type == nullptr || !Type->bIsComponent)
			{
				continue; // 아직 모르는 엔티티(늦게 온 생성)나 이 클라이언트에 없는 타입은 건너뛴다
			}
			if (Type->Name == "TransformComponent")
			{
				TransformBuffers.erase(NetId); // 신뢰 채널로 온 값(생성/입장)은 그대로 쓰고 보간을 새로 시작한다
			}
			if (Op == 1)
			{
				if (Type->HasComponent(Registry, Entity))
				{
					Type->RemoveComponent(Registry, Entity);
				}
				continue;
			}
			FBinaryReader ValueReader(Bytes.data(), Bytes.size());
			if (!NetReplication::ReadComponent(ValueReader, *Type, Type->AddComponent(Registry, Entity), ToEntity))
			{
				E_LOG(LogNet, Warning, "복제 값 형식 불일치: {} (NetId {})", TypeName, NetId);
				continue;
			}
			for (const FPropertyInfo& Property : Type->Properties)
			{
				bAssetsChanged = bAssetsChanged || Property.Type == EPropertyType::ResourceHandle;
			}
		}
	}
	if (!Reader.IsOk())
	{
		E_LOG(LogNet, Warning, "잘린 복제 상태 메시지");
	}
}

void FReplicationClient::ApplyTransformSnapshot(const std::vector<uint8>& Message)
{
	FBinaryReader Reader(Message.data(), Message.size());
	Reader.Read<uint8>();
	const float  Time  = Reader.Read<float>();
	const uint32 Count = Reader.Read<uint32>();
	if (!Reader.IsOk())
	{
		return;
	}
	// 서버 시각 추정: 크게 어긋나면 맞추고, 아니면 조금씩 따라간다 (지연 변동에 흔들리지 않게)
	if (!bClockValid || std::abs(Time - ServerClock) > 0.25f)
	{
		ServerClock = Time;
		bClockValid = true;
	}
	else
	{
		ServerClock += (Time - ServerClock) * 0.1f;
	}

	constexpr float KeepSeconds = 1.0f;
	for (uint32 Index = 0; Index < Count && Reader.IsOk(); ++Index)
	{
		FTransformSample Sample;
		const uint32     NetId = Reader.Read<uint32>();
		Sample.ServerTime      = Time;
		Sample.Position        = Reader.Read<FVector3>();
		Sample.Rotation        = Reader.Read<FQuat>();
		Sample.Scale           = Reader.Read<FVector3>();
		if (!Reader.IsOk() || !FindEntity(NetId).IsValid())
		{
			continue;
		}
		std::deque<FTransformSample>& Buffer = TransformBuffers[NetId];
		if (!Buffer.empty() && Buffer.back().ServerTime >= Time)
		{
			continue; // 늦게 도착한 (재정렬된) 스냅샷
		}
		Buffer.push_back(Sample);
		while (Buffer.size() > 2 && Buffer.front().ServerTime < Time - KeepSeconds)
		{
			Buffer.pop_front();
		}
	}
}

void FReplicationClient::Update(float DeltaSeconds)
{
	if (Scene == nullptr || !bClockValid)
	{
		return;
	}
	ServerClock += DeltaSeconds;
	const float RenderTime = ServerClock - InterpolationDelay;

	FRegistry& Registry = Scene->GetRegistry();
	for (auto It = TransformBuffers.begin(); It != TransformBuffers.end();)
	{
		const FEntity Entity = FindEntity(It->first);
		if (!Entity.IsValid() || It->second.empty())
		{
			It = TransformBuffers.erase(It);
			continue;
		}
		const std::deque<FTransformSample>& Buffer = It->second;
		FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity);
		++It;
		if (Transform == nullptr || (TransformFilter && !TransformFilter(Entity)))
		{
			continue; // 예측 캐릭터는 클라이언트가 직접 움직인다
		}

		// RenderTime을 감싸는 두 스냅샷 사이 보간. 범위 밖이면 가장 가까운 끝 값 (외삽하지 않는다)
		const FTransformSample* From = &Buffer.front();
		const FTransformSample* To   = &Buffer.front();
		for (size_t Index = 0; Index < Buffer.size(); ++Index)
		{
			if (Buffer[Index].ServerTime <= RenderTime)
			{
				From = &Buffer[Index];
				To   = Index + 1 < Buffer.size() ? &Buffer[Index + 1] : &Buffer[Index];
			}
		}
		const float Span  = To->ServerTime - From->ServerTime;
		const float Alpha = Span > 0.0f ? std::clamp((RenderTime - From->ServerTime) / Span, 0.0f, 1.0f) : 0.0f;
		Transform->Position = FVector3::Lerp(From->Position, To->Position, Alpha);
		Transform->Rotation = FQuat::Slerp(From->Rotation, To->Rotation, Alpha).GetNormalized();
		Transform->Scale    = FVector3::Lerp(From->Scale, To->Scale, Alpha);
	}
}
