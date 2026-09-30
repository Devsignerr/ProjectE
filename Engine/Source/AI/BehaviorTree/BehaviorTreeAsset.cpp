#include "AI/BehaviorTree/BehaviorTreeAsset.h"

#include "Core/FileSystem.h"
#include "AI/AIModule.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace
{
	using nlohmann::json;

	void SetError(std::string* OutError, std::string Message)
	{
		if (OutError)
		{
			*OutError = std::move(Message);
		}
	}

	json ParamToJson(const FBTParamValue& Value)
	{
		switch (Value.index())
		{
		case 0: return json(std::get<bool>(Value));
		case 1: return json(std::get<int32>(Value));
		case 2: return json(std::get<float>(Value));
		case 3: return json(std::get<std::string>(Value));
		default:
		{
			const FVector3& V = std::get<FVector3>(Value);
			return json::array({ V.X, V.Y, V.Z });
		}
		}
	}

	std::optional<FBTParamValue> ParamFromJson(const json& Node)
	{
		if (Node.is_boolean())
		{
			return FBTParamValue(Node.get<bool>());
		}
		if (Node.is_number_integer())
		{
			return FBTParamValue(Node.get<int32>());
		}
		if (Node.is_number_float())
		{
			return FBTParamValue(Node.get<float>());
		}
		if (Node.is_string())
		{
			return FBTParamValue(Node.get<std::string>());
		}
		if (Node.is_array() && Node.size() == 3 && Node[0].is_number() && Node[1].is_number() && Node[2].is_number())
		{
			return FBTParamValue(FVector3(Node[0].get<float>(), Node[1].get<float>(), Node[2].get<float>()));
		}
		return std::nullopt;
	}

	json NodeToJson(const FBTNodeDesc& Node)
	{
		json Out;
		Out["Type"] = Node.Type;
		Out["Id"]   = Node.Id;
		if (!Node.Params.empty())
		{
			json Params = json::array();
			for (const FBTParam& Param : Node.Params)
			{
				Params.push_back({ { "Name", Param.Name }, { "Value", ParamToJson(Param.Value) } });
			}
			Out["Params"] = std::move(Params);
		}
		const auto WriteList = [&Out](const char* Field, const std::vector<FBTNodeDesc>& List)
		{
			if (!List.empty())
			{
				json Array = json::array();
				for (const FBTNodeDesc& Child : List)
				{
					Array.push_back(NodeToJson(Child));
				}
				Out[Field] = std::move(Array);
			}
		};
		WriteList("Decorators", Node.Decorators);
		WriteList("Services", Node.Services);
		WriteList("Children", Node.Children);
		if (Node.EditorPosition)
		{
			Out["EditorPosition"] = { Node.EditorPosition->X, Node.EditorPosition->Y };
		}
		return Out;
	}

	bool NodeFromJson(const json& In, FBTNodeDesc& OutNode, std::string* OutError)
	{
		if (!In.is_object() || !In.contains("Type") || !In["Type"].is_string())
		{
			SetError(OutError, "노드에 Type 문자열이 없습니다");
			return false;
		}
		OutNode.Type = In["Type"].get<std::string>();

		const FBTNodeInfo* Info = FBehaviorTreeNodeRegistry::Get().Find(OutNode.Type);
		if (!Info)
		{
			SetError(OutError, std::format("모르는 노드 타입입니다: {}", OutNode.Type));
			return false;
		}

		if (In.contains("Id"))
		{
			if (!In["Id"].is_number_unsigned())
			{
				SetError(OutError, std::format("노드 '{}'의 Id가 부호 없는 정수가 아닙니다", OutNode.Type));
				return false;
			}
			OutNode.Id = In["Id"].get<uint32>();
		}

		// 편집기 위치 (형식이 틀리면 무시 — 실행에 영향 없음)
		if (const auto Position = In.find("EditorPosition");
		    Position != In.end() && Position->is_array() && Position->size() == 2 && (*Position)[0].is_number() && (*Position)[1].is_number())
		{
			OutNode.EditorPosition = FVector2((*Position)[0].get<float>(), (*Position)[1].get<float>());
		}

		if (In.contains("Params"))
		{
			const json& Params = In["Params"];
			if (!Params.is_array())
			{
				SetError(OutError, std::format("노드 '{}'의 Params가 배열이 아닙니다", OutNode.Type));
				return false;
			}
			for (const json& Param : Params)
			{
				if (!Param.is_object() || !Param.contains("Name") || !Param["Name"].is_string() || !Param.contains("Value"))
				{
					SetError(OutError, std::format("노드 '{}'의 파라미터 형식이 잘못되었습니다", OutNode.Type));
					return false;
				}
				const std::string Name = Param["Name"].get<std::string>();
				std::optional<FBTParamValue> Value = ParamFromJson(Param["Value"]);
				if (!Value)
				{
					SetError(OutError, std::format("노드 '{}'의 파라미터 '{}' 값 형식을 지원하지 않습니다", OutNode.Type, Name));
					return false;
				}
				// 선언된 파라미터는 선언 타입으로 맞춘다 (JSON에서 1.0과 1의 구분이 사라진 경우 등)
				if (const FBTParamDesc* Desc = Info->FindParam(Name))
				{
					Value = BehaviorTreeTypes::CoerceParam(*Value, Desc->Type);
					if (!Value)
					{
						SetError(OutError, std::format("노드 '{}'의 파라미터 '{}' 타입이 {}가 아닙니다", OutNode.Type, Name, PropertyTypeToString(Desc->Type)));
						return false;
					}
				}
				OutNode.Params.push_back({ Name, std::move(*Value) });
			}
		}

		const auto ReadList = [&In, &OutNode, OutError](const char* Field, std::vector<FBTNodeDesc>& OutList)
		{
			if (!In.contains(Field))
			{
				return true;
			}
			const json& Array = In[Field];
			if (!Array.is_array())
			{
				SetError(OutError, std::format("노드 '{}'의 {}가 배열이 아닙니다", OutNode.Type, Field));
				return false;
			}
			for (const json& Child : Array)
			{
				FBTNodeDesc ChildNode;
				if (!NodeFromJson(Child, ChildNode, OutError))
				{
					return false;
				}
				OutList.push_back(std::move(ChildNode));
			}
			return true;
		};
		return ReadList("Decorators", OutNode.Decorators) && ReadList("Services", OutNode.Services) && ReadList("Children", OutNode.Children);
	}

	void ForEachNode(const FBTNodeDesc& Node, const std::function<void(const FBTNodeDesc&)>& Visitor)
	{
		Visitor(Node);
		for (const FBTNodeDesc& Decorator : Node.Decorators)
		{
			ForEachNode(Decorator, Visitor);
		}
		for (const FBTNodeDesc& Service : Node.Services)
		{
			ForEachNode(Service, Visitor);
		}
		for (const FBTNodeDesc& Child : Node.Children)
		{
			ForEachNode(Child, Visitor);
		}
	}

	void ForEachNodeMutable(FBTNodeDesc& Node, const std::function<void(FBTNodeDesc&)>& Visitor)
	{
		Visitor(Node);
		for (FBTNodeDesc& Decorator : Node.Decorators)
		{
			ForEachNodeMutable(Decorator, Visitor);
		}
		for (FBTNodeDesc& Service : Node.Services)
		{
			ForEachNodeMutable(Service, Visitor);
		}
		for (FBTNodeDesc& Child : Node.Children)
		{
			ForEachNodeMutable(Child, Visitor);
		}
	}

	bool ValidateNode(const FBTNodeDesc& Node, bool bExecutable, std::set<uint32>& UsedIds, std::string* OutError)
	{
		const FBTNodeInfo* Info = FBehaviorTreeNodeRegistry::Get().Find(Node.Type);
		if (!Info)
		{
			SetError(OutError, std::format("모르는 노드 타입입니다: {}", Node.Type));
			return false;
		}
		if (Node.Id != 0 && !UsedIds.insert(Node.Id).second)
		{
			SetError(OutError, std::format("노드 ID {}가 중복됩니다 ({})", Node.Id, Node.Type));
			return false;
		}

		const bool bIsExecutable = Info->Category == EBTNodeCategory::Composite || Info->Category == EBTNodeCategory::Task;
		if (bExecutable != bIsExecutable)
		{
			SetError(OutError, std::format("{} 노드 '{}'는 이 위치에 올 수 없습니다", BehaviorTreeTypes::ToString(Info->Category), Node.Type));
			return false;
		}
		if (!bIsExecutable && (!Node.Children.empty() || !Node.Decorators.empty() || !Node.Services.empty()))
		{
			SetError(OutError, std::format("{} 노드 '{}'는 자식/데코레이터/서비스를 가질 수 없습니다", BehaviorTreeTypes::ToString(Info->Category), Node.Type));
			return false;
		}
		if (Info->Category == EBTNodeCategory::Task && !Node.Children.empty())
		{
			SetError(OutError, std::format("태스크 '{}'는 자식을 가질 수 없습니다", Node.Type));
			return false;
		}

		for (const FBTNodeDesc& Decorator : Node.Decorators)
		{
			const FBTNodeInfo* DecoratorInfo = FBehaviorTreeNodeRegistry::Get().Find(Decorator.Type);
			if (DecoratorInfo && DecoratorInfo->Category != EBTNodeCategory::Decorator)
			{
				SetError(OutError, std::format("'{}'는 데코레이터가 아닙니다", Decorator.Type));
				return false;
			}
			if (!ValidateNode(Decorator, false, UsedIds, OutError))
			{
				return false;
			}
		}
		for (const FBTNodeDesc& Service : Node.Services)
		{
			const FBTNodeInfo* ServiceInfo = FBehaviorTreeNodeRegistry::Get().Find(Service.Type);
			if (ServiceInfo && ServiceInfo->Category != EBTNodeCategory::Service)
			{
				SetError(OutError, std::format("'{}'는 서비스가 아닙니다", Service.Type));
				return false;
			}
			if (!ValidateNode(Service, false, UsedIds, OutError))
			{
				return false;
			}
		}
		for (const FBTNodeDesc& Child : Node.Children)
		{
			if (!ValidateNode(Child, true, UsedIds, OutError))
			{
				return false;
			}
		}

		if (Info->Category == EBTNodeCategory::Composite)
		{
			const std::unique_ptr<FBTNode> Probe = FBehaviorTreeNodeRegistry::Get().Create(Node.Type);
			const FBTCompositeNode* Composite = static_cast<const FBTCompositeNode*>(Probe.get());
			if (Composite && Composite->IsSimpleParallel())
			{
				const FBTNodeInfo* MainInfo = Node.Children.empty() ? nullptr : FBehaviorTreeNodeRegistry::Get().Find(Node.Children[0].Type);
				if (Node.Children.empty() || Node.Children.size() > 2 || !MainInfo || MainInfo->Category != EBTNodeCategory::Task)
				{
					SetError(OutError, std::format("'{}'는 자식이 1~2개여야 하고 첫 자식(주 태스크)은 태스크여야 합니다", Node.Type));
					return false;
				}
			}
		}
		return true;
	}
} // namespace

const FBTParamValue* FBTNodeDesc::FindParam(std::string_view Name) const
{
	for (const FBTParam& Param : Params)
	{
		if (Param.Name == Name)
		{
			return &Param.Value;
		}
	}
	return nullptr;
}

void FBTNodeDesc::SetParam(std::string_view Name, FBTParamValue Value)
{
	for (FBTParam& Param : Params)
	{
		if (Param.Name == Name)
		{
			Param.Value = std::move(Value);
			return;
		}
	}
	Params.push_back({ std::string(Name), std::move(Value) });
}

std::string FBehaviorTreeAsset::ToJsonString() const
{
	json Document;
	Document["Version"] = Version;

	json Keys = json::array();
	for (const FBlackboardKeyDesc& Key : BlackboardKeys)
	{
		Keys.push_back({ { "Name", Key.Name }, { "Type", BehaviorTreeTypes::ToString(Key.Type) } });
	}
	Document["Blackboard"] = std::move(Keys);

	if (Root)
	{
		Document["Root"] = NodeToJson(*Root);
	}
	return Document.dump(2);
}

bool FBehaviorTreeAsset::FromJsonString(const std::string& Json, std::string* OutError)
{
	const json Document = json::parse(Json, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		SetError(OutError, "JSON 문법 오류");
		return false;
	}

	int32 FileVersion = Version;
	if (Document.contains("Version"))
	{
		if (!Document["Version"].is_number_integer())
		{
			SetError(OutError, "Version이 정수가 아닙니다");
			return false;
		}
		FileVersion = Document["Version"].get<int32>();
	}
	if (FileVersion < 1 || FileVersion > Version)
	{
		SetError(OutError, std::format("지원하지 않는 비헤이비어 트리 버전입니다: {}", FileVersion));
		return false;
	}

	FBehaviorTreeAsset Loaded;
	if (Document.contains("Blackboard"))
	{
		const json& Keys = Document["Blackboard"];
		if (!Keys.is_array())
		{
			SetError(OutError, "Blackboard가 배열이 아닙니다");
			return false;
		}
		for (const json& Key : Keys)
		{
			if (!Key.is_object() || !Key.contains("Name") || !Key["Name"].is_string() || !Key.contains("Type") || !Key["Type"].is_string())
			{
				SetError(OutError, "블랙보드 키 형식이 잘못되었습니다");
				return false;
			}
			const std::string                       TypeName = Key["Type"].get<std::string>();
			const std::optional<EBlackboardKeyType> Type     = BehaviorTreeTypes::ParseBlackboardKeyType(TypeName);
			if (!Type)
			{
				SetError(OutError, std::format("모르는 블랙보드 키 타입입니다: {}", TypeName));
				return false;
			}
			Loaded.BlackboardKeys.push_back({ Key["Name"].get<std::string>(), *Type });
		}
	}

	if (Document.contains("Root") && !Document["Root"].is_null())
	{
		FBTNodeDesc RootNode;
		if (!NodeFromJson(Document["Root"], RootNode, OutError))
		{
			return false;
		}
		Loaded.Root = std::move(RootNode);
	}

	Loaded.AssignMissingNodeIds();
	if (!Loaded.Validate(OutError))
	{
		return false;
	}
	*this = std::move(Loaded);
	return true;
}

bool FBehaviorTreeAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogAI, Error, "비헤이비어 트리를 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return static_cast<bool>(File);
}

bool FBehaviorTreeAsset::LoadFromFile(const std::filesystem::path& Path, std::string* OutError)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		SetError(OutError, std::format("파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring())));
		return false;
	}
	std::string Error;
	if (!FromJsonString(Text, &Error))
	{
		E_LOG(LogAI, Warning, "비헤이비어 트리를 읽지 못했습니다: {} ({})", FStringConv::ToUtf8(Path.wstring()), Error);
		SetError(OutError, std::move(Error));
		return false;
	}
	return true;
}

bool FBehaviorTreeAsset::Validate(std::string* OutError) const
{
	std::set<std::string> KeyNames;
	for (const FBlackboardKeyDesc& Key : BlackboardKeys)
	{
		if (Key.Name.empty() || !KeyNames.insert(Key.Name).second)
		{
			SetError(OutError, std::format("블랙보드 키 이름이 비었거나 중복됩니다: '{}'", Key.Name));
			return false;
		}
	}
	if (!Root)
	{
		return true;
	}
	std::set<uint32> UsedIds;
	return ValidateNode(*Root, true, UsedIds, OutError);
}

void FBehaviorTreeAsset::AssignMissingNodeIds()
{
	if (!Root)
	{
		return;
	}
	uint32 NextId = GetNextNodeId();
	ForEachNodeMutable(*Root, [&NextId](FBTNodeDesc& Node)
	{
		if (Node.Id == 0)
		{
			Node.Id = NextId++;
		}
	});
}

uint32 FBehaviorTreeAsset::GetNextNodeId() const
{
	uint32 MaxId = 0;
	if (Root)
	{
		ForEachNode(*Root, [&MaxId](const FBTNodeDesc& Node) { MaxId = std::max(MaxId, Node.Id); });
	}
	return MaxId + 1;
}

FBTNodeDesc* FBehaviorTreeAsset::FindNode(uint32 Id)
{
	return const_cast<FBTNodeDesc*>(static_cast<const FBehaviorTreeAsset*>(this)->FindNode(Id));
}

const FBTNodeDesc* FBehaviorTreeAsset::FindNode(uint32 Id) const
{
	const FBTNodeDesc* Found = nullptr;
	if (Root && Id != 0)
	{
		ForEachNode(*Root, [&Found, Id](const FBTNodeDesc& Node)
		{
			if (!Found && Node.Id == Id)
			{
				Found = &Node;
			}
		});
	}
	return Found;
}
