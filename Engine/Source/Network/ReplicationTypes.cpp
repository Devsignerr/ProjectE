#include "Network/ReplicationTypes.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Serialization/BinaryArchive.h"
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
		FRegistry&           Registry = Scene.GetRegistry();
		std::vector<FEntity> Entities;
		Registry.View<FReplicatedComponent>().Each([&](FEntity Entity, FReplicatedComponent&) { Entities.push_back(Entity); });
		// 새 씬에 로드하면 엔티티 인덱스가 파일 순서대로 매겨지므로 인덱스 순이 양쪽에서 같다
		std::sort(Entities.begin(), Entities.end(), [](FEntity A, FEntity B) { return A.Index < B.Index; });
		uint32 NextNetId = 1;
		for (const FEntity Entity : Entities)
		{
			Registry.GetOrEmplace<FNetIdComponent>(Entity).NetId = NextNetId++;
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
