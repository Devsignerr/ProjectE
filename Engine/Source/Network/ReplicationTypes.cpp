#include "Network/ReplicationTypes.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetTypes.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

void RegisterNetworkTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FReplicatedComponent>())
	{
		return;
	}
	Registry.RegisterType<FReplicatedComponent>("ReplicatedComponent", "네트워크 복제")
		.Property(&FReplicatedComponent::OwnerPlayerId, "OwnerPlayerId", "소유 플레이어 (-1 서버)", PF_Transient | PF_ReadOnly)
		.AsComponent();
}

namespace NetReplication
{
	void AssignStaticNetIds(FScene& Scene)
	{
		// 순서 = ReplicatedComponent 풀에 추가된 순서(밀집 배열). 같은 씬 파일을 로드하면 양쪽이 같다.
		// 엔티티 인덱스 순서는 쓰지 않는다 — 씬을 비우고 다시 열면(세션 전환) 해제된 인덱스가 역순으로 재사용되기 때문
		FRegistry& Registry = Scene.GetRegistry();
		const TSparseSet<FReplicatedComponent>* Pool = Registry.TryGetPool<FReplicatedComponent>();
		if (Pool == nullptr)
		{
			return;
		}
		const std::vector<FEntity> Entities = Pool->GetEntities(); // 아래에서 다른 풀(FNetIdComponent)에 추가하므로 복사
		uint32                     NextNetId = 1;
		for (const FEntity Entity : Entities)
		{
			Registry.GetOrEmplace<FNetIdComponent>(Entity).NetId = NextNetId++;
		}
	}

	uint32 GetSubSceneNetIdBase(uint32 InstanceId)
	{
		return SubSceneNetIdBase + (InstanceId - 1) * SubSceneNetIdStride;
	}

	void AssignSubSceneNetIds(FScene& Scene, FEntity Root, uint32 InstanceId, const std::function<void(FEntity, uint32)>& OnAssigned)
	{
		FRegistry&   Registry = Scene.GetRegistry();
		const uint32 Base     = GetSubSceneNetIdBase(InstanceId);
		uint32       Index    = 0;
		bool         bWarned  = false;
		// 부모 → 자식 (자식 목록 순서) — 같은 파일을 붙이면 양쪽이 같은 순서
		std::vector<FEntity> Stack = { Root };
		while (!Stack.empty())
		{
			const FEntity Entity = Stack.back();
			Stack.pop_back();
			if (!Registry.IsValid(Entity))
			{
				continue;
			}
			if (Registry.Has<FReplicatedComponent>(Entity))
			{
				if (Index < SubSceneNetIdStride)
				{
					const uint32 NetId                             = Base + Index++;
					Registry.GetOrEmplace<FNetIdComponent>(Entity).NetId = NetId;
					if (OnAssigned)
					{
						OnAssigned(Entity, NetId);
					}
				}
				else if (!bWarned)
				{
					bWarned = true;
					E_LOG(LogNet, Warning, "서브 씬 {}의 복제 엔티티가 {}개를 넘어 나머지는 복제하지 않습니다", InstanceId, SubSceneNetIdStride);
				}
			}
			const std::vector<FEntity>& Children = Scene.GetChildren(Entity);
			for (auto It = Children.rbegin(); It != Children.rend(); ++It)
			{
				Stack.push_back(*It); // 첫 자식이 먼저 나오도록 역순으로 쌓는다
			}
		}
	}

	uint32 GetNetId(const FScene& Scene, FEntity Entity)
	{
		const FNetIdComponent* NetId = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FNetIdComponent>(Entity) : nullptr;
		return NetId != nullptr ? NetId->NetId : InvalidNetId;
	}

	bool IsReplicated(const FTypeInfo& Type)
	{
		return Type.bIsComponent && (Type.Flags & TF_NoReplicate) == 0;
	}

	bool IsReplicated(const FPropertyInfo& Property)
	{
		return !Property.HasFlag(PF_NoReplicate) && Property.Type != EPropertyType::ResourceHandle;
	}

	void WriteComponent(FBinaryWriter& Writer, const FTypeInfo& Type, const void* Component, const FEntityToNetId& ToNetId)
	{
		uint16 Count = 0;
		for (const FPropertyInfo& Property : Type.Properties)
		{
			Count += IsReplicated(Property) ? 1 : 0;
		}
		Writer.Write(Count);
		for (const FPropertyInfo& Property : Type.Properties)
		{
			if (!IsReplicated(Property))
			{
				continue;
			}
			const void* Value = Property.GetPtr(Component);
			switch (Property.Type)
			{
			case EPropertyType::Bool:
				Writer.Write(static_cast<uint8>(*static_cast<const bool*>(Value) ? 1 : 0));
				break;
			case EPropertyType::String:
				Writer.WriteString(*static_cast<const std::string*>(Value));
				break;
			case EPropertyType::Entity:
				Writer.Write(ToNetId(*static_cast<const FEntity*>(Value)));
				break;
			default: // 숫자/벡터/쿼터니언: 메모리 그대로 (float/int 배열, 리틀 엔디언)
				Writer.WriteBytes(Value, Property.Size);
				break;
			}
		}
	}

	bool ReadComponent(FBinaryReader& Reader, const FTypeInfo& Type, void* Component, const FNetIdToEntity& ToEntity)
	{
		uint16 Expected = 0;
		for (const FPropertyInfo& Property : Type.Properties)
		{
			Expected += IsReplicated(Property) ? 1 : 0;
		}
		if (Reader.Read<uint16>() != Expected || !Reader.IsOk())
		{
			return false; // 서버와 타입 정의가 다르다 (핸드셰이크에서 버전 확인을 하지만 방어)
		}
		for (const FPropertyInfo& Property : Type.Properties)
		{
			void* Value = Property.GetPtr(Component);
			if (Property.Type == EPropertyType::ResourceHandle)
			{
				// 경로 문자열이 바뀌었을 수 있으므로 핸들을 비워 에셋 해석기가 다시 채우게 한다 (THandle 기본값: Index ~0, Generation 0)
				static_assert(sizeof(THandle<void>) == 8);
				const THandle<void> Invalid;
				std::memcpy(Value, &Invalid, sizeof(Invalid));
				continue;
			}
			if (!IsReplicated(Property))
			{
				continue;
			}
			switch (Property.Type)
			{
			case EPropertyType::Bool:
				*static_cast<bool*>(Value) = Reader.Read<uint8>() != 0;
				break;
			case EPropertyType::String:
				*static_cast<std::string*>(Value) = Reader.ReadString();
				break;
			case EPropertyType::Entity:
				*static_cast<FEntity*>(Value) = ToEntity(Reader.Read<uint32>());
				break;
			default:
				Reader.ReadBytes(Value, Property.Size);
				break;
			}
		}
		return Reader.IsOk();
	}
}
