#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"

#include "AI/AIModule.h"
#include "AI/BehaviorTree/BuiltinNodes.h"
#include "Core/Reflection/TypeInfo.h"

const FBTParamDesc* FBTNodeInfo::FindParam(std::string_view ParamName) const
{
	for (const FBTParamDesc& Param : Params)
	{
		if (Param.Name == ParamName)
		{
			return &Param;
		}
	}
	return nullptr;
}

FBehaviorTreeNodeRegistry& FBehaviorTreeNodeRegistry::Get()
{
	// 엔진 DLL 안의 함수 지역 static (DLL 밖 바이너리와 공유된다)
	static FBehaviorTreeNodeRegistry Registry;
	return Registry;
}

FBehaviorTreeNodeRegistry::FBehaviorTreeNodeRegistry()
{
	RegisterBuiltinBehaviorTreeNodes(*this);
}

bool FBehaviorTreeNodeRegistry::Register(FBTNodeInfo Info)
{
	// 소유자를 비우면 리플렉션의 현재 등록 소유자를 쓴다 (게임 모듈 OnLoad 중이면 모듈 이름 → 언로드 때 함께 해제)
	if (Info.Owner.empty())
	{
		Info.Owner = FTypeRegistry::Get().GetRegistrationOwner();
	}
	if (Info.Owner.empty())
	{
		Info.Owner = EngineOwner;
	}
	if (Info.Name.empty() || !Info.Factory)
	{
		E_LOG(LogAI, Error, "비헤이비어 트리 노드 등록 실패: 이름 또는 팩토리가 비어 있습니다 ({})", Info.Name);
		return false;
	}
	if (Nodes.contains(Info.Name))
	{
		E_LOG(LogAI, Error, "비헤이비어 트리 노드 '{}'가 이미 등록되어 있습니다", Info.Name);
		return false;
	}
	for (const FBTParamDesc& Param : Info.Params)
	{
		if (!BehaviorTreeTypes::MakeDefaultParam(Param.Type) || BehaviorTreeTypes::GetParamType(Param.Default) != Param.Type)
		{
			E_LOG(LogAI, Error, "비헤이비어 트리 노드 '{}'의 파라미터 '{}' 기본값 타입이 선언({})과 다릅니다",
			      Info.Name, Param.Name, PropertyTypeToString(Param.Type));
			return false;
		}
	}
	if (Info.Category == EBTNodeCategory::Service)
	{
		if (!Info.FindParam("Interval"))
		{
			Info.Params.push_back({ "Interval", "간격(초)", EPropertyType::Float, FBTParamValue(0.5f), {} });
		}
		if (!Info.FindParam("RandomDeviation"))
		{
			Info.Params.push_back({ "RandomDeviation", "무작위 편차(초)", EPropertyType::Float, FBTParamValue(0.0f), {} });
		}
	}
	if (Info.DisplayName.empty())
	{
		Info.DisplayName = Info.Name;
	}
	std::string Name = Info.Name;
	Nodes.emplace(std::move(Name), std::make_unique<FBTNodeInfo>(std::move(Info)));
	return true;
}

bool FBehaviorTreeNodeRegistry::Unregister(std::string_view Name)
{
	const auto It = Nodes.find(Name);
	if (It == Nodes.end())
	{
		return false;
	}
	Nodes.erase(It);
	return true;
}

int32 FBehaviorTreeNodeRegistry::UnregisterOwner(std::string_view Owner)
{
	int32 Count = 0;
	for (auto It = Nodes.begin(); It != Nodes.end();)
	{
		if (It->second->Owner == Owner)
		{
			It = Nodes.erase(It);
			++Count;
		}
		else
		{
			++It;
		}
	}
	return Count;
}

const FBTNodeInfo* FBehaviorTreeNodeRegistry::Find(std::string_view Name) const
{
	const auto It = Nodes.find(Name);
	return It != Nodes.end() ? It->second.get() : nullptr;
}

std::vector<const FBTNodeInfo*> FBehaviorTreeNodeRegistry::GetAll() const
{
	std::vector<const FBTNodeInfo*> Result;
	Result.reserve(Nodes.size());
	for (const auto& [Name, Info] : Nodes)
	{
		Result.push_back(Info.get());
	}
	return Result;
}

std::vector<const FBTNodeInfo*> FBehaviorTreeNodeRegistry::GetByCategory(EBTNodeCategory Category) const
{
	std::vector<const FBTNodeInfo*> Result;
	for (const auto& [Name, Info] : Nodes)
	{
		if (Info->Category == Category)
		{
			Result.push_back(Info.get());
		}
	}
	return Result;
}

std::unique_ptr<FBTNode> FBehaviorTreeNodeRegistry::Create(std::string_view Name) const
{
	const FBTNodeInfo* Info = Find(Name);
	if (!Info)
	{
		return nullptr;
	}
	std::unique_ptr<FBTNode> Node = Info->Factory();
	if (!Node || Node->GetCategory() != Info->Category)
	{
		E_LOG(LogAI, Error, "비헤이비어 트리 노드 '{}' 팩토리가 {} 노드를 만들지 못했습니다", Info->Name, BehaviorTreeTypes::ToString(Info->Category));
		return nullptr;
	}
	return Node;
}
