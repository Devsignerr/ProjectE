#include "Renderer/MaterialGraph.h"
#include "Renderer/MaterialGraphJson.h"

#include <cmath>
#include <format>
#include <utility>

namespace
{
	using nlohmann::json;

	float& Component(FVector4& Value, uint32 Index) { return (&Value.X)[Index]; }

	// 숫자 또는 숫자 배열(1~4) → 값 + 너비
	bool ReadNumbers(const json& Node, FVector4& OutValue, uint32& OutWidth)
	{
		OutValue = FVector4::ZeroVector;
		if (Node.is_number())
		{
			OutValue.X = Node.get<float>();
			OutWidth   = 1;
			return true;
		}
		if (Node.is_boolean())
		{
			OutValue.X = Node.get<bool>() ? 1.0f : 0.0f;
			OutWidth   = 1;
			return true;
		}
		if (!Node.is_array() || Node.empty() || Node.size() > 4)
		{
			return false;
		}
		for (uint32 Index = 0; Index < static_cast<uint32>(Node.size()); ++Index)
		{
			if (!Node[Index].is_number())
			{
				return false;
			}
			Component(OutValue, Index) = Node[Index].get<float>();
		}
		OutWidth = static_cast<uint32>(Node.size());
		return true;
	}

	json WriteNumbers(const FVector4& Value, uint32 Width)
	{
		if (Width <= 1)
		{
			return Value.X;
		}
		json Array = json::array();
		for (uint32 Index = 0; Index < Width && Index < 4; ++Index)
		{
			Array.push_back((&Value.X)[Index]);
		}
		return Array;
	}

	// 입력 값: "노드" / "노드:출력" 또는 상수
	bool ReadInput(const std::string& Pin, const json& Node, FMaterialGraphInput& Out)
	{
		Out     = FMaterialGraphInput{};
		Out.Pin = Pin;
		if (Node.is_string())
		{
			const std::string Text  = Node.get<std::string>();
			const size_t      Colon = Text.rfind(':');
			Out.Node                = Text;
			if (Colon != std::string::npos && Colon + 1 < Text.size() &&
			    Text.find_first_not_of("0123456789", Colon + 1) == std::string::npos)
			{
				Out.Node   = Text.substr(0, Colon);
				Out.Output = static_cast<uint32>(std::stoul(Text.substr(Colon + 1)));
			}
			return !Out.Node.empty();
		}
		return ReadNumbers(Node, Out.Constant, Out.ConstantWidth);
	}

	json WriteInput(const FMaterialGraphInput& Input)
	{
		if (Input.IsLink())
		{
			return Input.Output == 0 ? Input.Node : std::format("{}:{}", Input.Node, Input.Output);
		}
		return WriteNumbers(Input.Constant, Input.ConstantWidth);
	}

	// 종류별 Name/Option JSON 키
	const char* GetNameKey(const std::string& Type) { return Type == "TextureSample" ? "Texture" : "Parameter"; }
	const char* GetOptionKey(const std::string& Type)
	{
		if (Type == "TextureSample")
		{
			return "Sampler";
		}
		if (Type == "ComponentMask")
		{
			return "Channels";
		}
		return "Op";
	}
} // namespace

const char* GetMaterialParameterTypeName(EMaterialParameterType Type)
{
	switch (Type)
	{
	case EMaterialParameterType::Vector:       return "Vector";
	case EMaterialParameterType::Texture:      return "Texture";
	case EMaterialParameterType::StaticSwitch: return "StaticSwitch";
	default:                                   return "Scalar";
	}
}

bool ParseMaterialParameterType(std::string_view Name, EMaterialParameterType& OutType)
{
	for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialParameterType::Count); ++Index)
	{
		if (Name == GetMaterialParameterTypeName(static_cast<EMaterialParameterType>(Index)))
		{
			OutType = static_cast<EMaterialParameterType>(Index);
			return true;
		}
	}
	return false;
}

const char* GetTextureUsageName(ETextureUsage Usage)
{
	switch (Usage)
	{
	case ETextureUsage::Linear: return "Linear";
	case ETextureUsage::Normal: return "Normal";
	case ETextureUsage::Mask:   return "Mask";
	default:                    return "Color";
	}
}

bool ParseTextureUsage(std::string_view Name, ETextureUsage& OutUsage)
{
	for (const ETextureUsage Usage : { ETextureUsage::Color, ETextureUsage::Linear, ETextureUsage::Normal, ETextureUsage::Mask })
	{
		if (Name == GetTextureUsageName(Usage))
		{
			OutUsage = Usage;
			return true;
		}
	}
	return false;
}

const char* GetMaterialOutputName(EMaterialOutput Output)
{
	switch (Output)
	{
	case EMaterialOutput::BaseColor:        return "BaseColor";
	case EMaterialOutput::Metallic:         return "Metallic";
	case EMaterialOutput::Roughness:        return "Roughness";
	case EMaterialOutput::Normal:           return "Normal";
	case EMaterialOutput::AmbientOcclusion: return "AmbientOcclusion";
	case EMaterialOutput::Emissive:         return "Emissive";
	case EMaterialOutput::Opacity:          return "Opacity";
	case EMaterialOutput::OpacityMask:      return "OpacityMask";
	default:                                return "";
	}
}

uint32 GetMaterialOutputWidth(EMaterialOutput Output)
{
	switch (Output)
	{
	case EMaterialOutput::BaseColor:
	case EMaterialOutput::Normal:
	case EMaterialOutput::Emissive: return 3;
	default:                        return 1;
	}
}

const FMaterialGraphInput* FMaterialGraphNode::FindInput(std::string_view Pin) const
{
	for (const FMaterialGraphInput& Input : Inputs)
	{
		if (Input.Pin == Pin)
		{
			return &Input;
		}
	}
	return nullptr;
}

const FMaterialGraphNode* FMaterialGraph::FindNode(std::string_view Id) const
{
	for (const FMaterialGraphNode& Node : Nodes)
	{
		if (Node.Id == Id)
		{
			return &Node;
		}
	}
	return nullptr;
}

FMaterialGraphNode* FMaterialGraph::FindNode(std::string_view Id)
{
	return const_cast<FMaterialGraphNode*>(std::as_const(*this).FindNode(Id));
}

const std::vector<uint32>* FMaterialGraphAnalysis::FindOutputWidths(std::string_view NodeId) const
{
	for (size_t Index = 0; Index < NodeIds.size() && Index < OutputWidths.size(); ++Index)
	{
		if (NodeIds[Index] == NodeId)
		{
			return &OutputWidths[Index];
		}
	}
	return nullptr;
}

const FMaterialParameterSlot* FMaterialParameterLayout::Find(std::string_view Name) const
{
	for (const FMaterialParameterSlot& Slot : Slots)
	{
		if (Slot.Name == Name)
		{
			return &Slot;
		}
	}
	return nullptr;
}

std::vector<FVector4> FMaterialParameterLayout::BuildConstants(const std::vector<FMaterialParameter>& Parameters) const
{
	std::vector<FVector4> Registers(ConstantRegisters, FVector4::ZeroVector);
	for (const FMaterialParameterSlot& Slot : Slots)
	{
		if (Slot.Type != EMaterialParameterType::Scalar && Slot.Type != EMaterialParameterType::Vector)
		{
			continue;
		}
		const FMaterialParameter* Found = nullptr;
		for (const FMaterialParameter& Parameter : Parameters)
		{
			if (Parameter.Name == Slot.Name)
			{
				Found = &Parameter;
				break;
			}
		}
		if (Found == nullptr || Slot.Register >= ConstantRegisters)
		{
			continue;
		}
		if (Slot.Type == EMaterialParameterType::Vector)
		{
			Registers[Slot.Register] = Found->Value;
		}
		else
		{
			Component(Registers[Slot.Register], Slot.Component & 3u) = Found->Value.X;
		}
	}
	return Registers;
}

const FMaterialParameter* FMaterialParameterLayout::FindTextureParameter(uint32 TextureSlot, const std::vector<FMaterialParameter>& Parameters) const
{
	for (const FMaterialParameterSlot& Slot : Slots)
	{
		if (Slot.Type == EMaterialParameterType::Texture && Slot.Register == TextureSlot)
		{
			for (const FMaterialParameter& Parameter : Parameters)
			{
				if (Parameter.Name == Slot.Name)
				{
					return &Parameter;
				}
			}
			return nullptr;
		}
	}
	return nullptr;
}

std::string FMaterialGraphCompileResult::JoinErrors() const
{
	std::string Joined;
	for (const std::string& Error : Errors)
	{
		Joined += Joined.empty() ? Error : "\n" + Error;
	}
	return Joined;
}

namespace MaterialGraphJson
{
	void ReadParameters(const json& Array, std::vector<FMaterialParameter>& Out, std::vector<std::string>& OutWarnings)
	{
		Out.clear();
		if (!Array.is_array())
		{
			OutWarnings.push_back("Parameters는 배열이어야 합니다");
			return;
		}
		for (const json& Item : Array)
		{
			if (!Item.is_object() || !Item.contains("Name") || !Item["Name"].is_string())
			{
				OutWarnings.push_back("파라미터에 Name이 없습니다");
				continue;
			}
			FMaterialParameter Parameter;
			Parameter.Name    = Item["Name"].get<std::string>();
			const json* Value = Item.contains("Value") ? &Item["Value"] : nullptr;
			// 타입: 적혀 있으면 그것, 없으면 값 모양으로 추정 (문자열 = 텍스처, 불 = 정적 스위치, 배열 = 벡터, 숫자 = 스칼라)
			if (Item.contains("Type") && Item["Type"].is_string())
			{
				if (!ParseMaterialParameterType(Item["Type"].get<std::string>(), Parameter.Type))
				{
					OutWarnings.push_back(std::format("파라미터 {}: 알 수 없는 Type {}", Parameter.Name, Item["Type"].get<std::string>()));
					continue;
				}
			}
			else if (Value != nullptr)
			{
				Parameter.Type = Value->is_string()    ? EMaterialParameterType::Texture
				                 : Value->is_boolean() ? EMaterialParameterType::StaticSwitch
				                 : Value->is_array()   ? EMaterialParameterType::Vector
				                                       : EMaterialParameterType::Scalar;
			}
			if (Value != nullptr)
			{
				if (Parameter.Type == EMaterialParameterType::Texture)
				{
					Parameter.Texture = Value->is_string() ? Value->get<std::string>() : std::string();
				}
				else
				{
					uint32 Width = 1;
					if (!ReadNumbers(*Value, Parameter.Value, Width))
					{
						OutWarnings.push_back(std::format("파라미터 {}: Value 형식이 틀렸습니다", Parameter.Name));
					}
					else if (Parameter.Type == EMaterialParameterType::Vector && Width == 1)
					{
						Parameter.Value = FVector4(Parameter.Value.X, Parameter.Value.X, Parameter.Value.X, Parameter.Value.X);
					}
					else if (Parameter.Type == EMaterialParameterType::Vector && Width == 3)
					{
						Parameter.Value.W = 1.0f; // 색 [r, g, b] → 알파 1
					}
				}
			}
			if (Item.contains("Usage") && Item["Usage"].is_string() && !ParseTextureUsage(Item["Usage"].get<std::string>(), Parameter.Usage))
			{
				OutWarnings.push_back(std::format("파라미터 {}: 알 수 없는 Usage {}", Parameter.Name, Item["Usage"].get<std::string>()));
			}
			Out.push_back(std::move(Parameter));
		}
	}

	json WriteParameters(const std::vector<FMaterialParameter>& Parameters, bool bWriteType)
	{
		json Array = json::array();
		for (const FMaterialParameter& Parameter : Parameters)
		{
			json Item;
			Item["Name"] = Parameter.Name;
			if (bWriteType)
			{
				Item["Type"] = GetMaterialParameterTypeName(Parameter.Type);
			}
			switch (Parameter.Type)
			{
			case EMaterialParameterType::Scalar:       Item["Value"] = Parameter.Value.X; break;
			case EMaterialParameterType::Vector:       Item["Value"] = WriteNumbers(Parameter.Value, 4); break;
			case EMaterialParameterType::StaticSwitch: Item["Value"] = Parameter.GetBool(); break;
			case EMaterialParameterType::Texture:
				Item["Value"] = Parameter.Texture;
				Item["Usage"] = GetTextureUsageName(Parameter.Usage);
				break;
			default: break;
			}
			Array.push_back(std::move(Item));
		}
		return Array;
	}

	void ReadGraph(const json& Object, FMaterialGraph& Out, std::vector<std::string>& OutWarnings)
	{
		Out = FMaterialGraph{};
		if (!Object.is_object())
		{
			OutWarnings.push_back("Graph는 객체여야 합니다");
			return;
		}
		if (const auto Nodes = Object.find("Nodes"); Nodes != Object.end() && Nodes->is_array())
		{
			for (const json& Item : *Nodes)
			{
				if (!Item.is_object())
				{
					OutWarnings.push_back("노드는 객체여야 합니다");
					continue;
				}
				FMaterialGraphNode Node;
				Node.Id   = Item.value("Id", std::string());
				Node.Type = Item.value("Type", std::string());
				if (Node.Type == "TexCoord")
				{
					Node.Value      = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
					Node.ValueWidth = 2;
				}
				if (const auto Value = Item.find("Value"); Value != Item.end() && !ReadNumbers(*Value, Node.Value, Node.ValueWidth))
				{
					OutWarnings.push_back(std::format("[{}] Value 형식이 틀렸습니다", Node.Id));
				}
				if (const auto Tiling = Item.find("Tiling"); Tiling != Item.end())
				{
					uint32 Width = 0;
					if (!ReadNumbers(*Tiling, Node.Value, Width))
					{
						OutWarnings.push_back(std::format("[{}] Tiling 형식이 틀렸습니다", Node.Id));
					}
					else if (Width == 1)
					{
						Node.Value.Y = Node.Value.X;
					}
					Node.ValueWidth = 2;
				}
				for (const char* Key : { "Parameter", "Texture" })
				{
					if (Item.contains(Key) && Item[Key].is_string())
					{
						Node.Name = Item[Key].get<std::string>();
					}
				}
				for (const char* Key : { "Sampler", "Channels", "Op" })
				{
					if (Item.contains(Key) && Item[Key].is_string())
					{
						Node.Option = Item[Key].get<std::string>();
					}
				}
				Node.Index = Item.value("Channel", 0);
				if (const auto Position = Item.find("EditorPosition"); Position != Item.end() && Position->is_array() && Position->size() == 2)
				{
					Node.EditorPosition = FVector2((*Position)[0].get<float>(), (*Position)[1].get<float>());
				}
				if (const auto Inputs = Item.find("Inputs"); Inputs != Item.end() && Inputs->is_object())
				{
					for (auto It = Inputs->begin(); It != Inputs->end(); ++It)
					{
						FMaterialGraphInput Input;
						if (ReadInput(It.key(), It.value(), Input))
						{
							Node.Inputs.push_back(std::move(Input));
						}
						else
						{
							OutWarnings.push_back(std::format("[{}] 입력 {} 형식이 틀렸습니다", Node.Id, It.key()));
						}
					}
				}
				Out.Nodes.push_back(std::move(Node));
			}
		}
		if (const auto Outputs = Object.find("Output"); Outputs != Object.end() && Outputs->is_object())
		{
			for (auto It = Outputs->begin(); It != Outputs->end(); ++It)
			{
				FMaterialGraphInput Input;
				if (ReadInput(It.key(), It.value(), Input))
				{
					Out.Outputs.push_back(std::move(Input));
				}
				else
				{
					OutWarnings.push_back(std::format("출력 {} 형식이 틀렸습니다", It.key()));
				}
			}
		}
		// 노드 편집기 정보 (코드 생성에 쓰지 않음)
		const auto ReadVector2 = [](const json& Item, FVector2& Out) {
			if (Item.is_array() && Item.size() == 2 && Item[0].is_number() && Item[1].is_number())
			{
				Out = FVector2(Item[0].get<float>(), Item[1].get<float>());
			}
		};
		if (const auto Position = Object.find("EditorOutputPosition"); Position != Object.end())
		{
			ReadVector2(*Position, Out.OutputEditorPosition);
		}
		if (const auto Comments = Object.find("EditorComments"); Comments != Object.end() && Comments->is_array())
		{
			for (const json& Item : *Comments)
			{
				if (!Item.is_object())
				{
					continue;
				}
				FMaterialGraphComment Comment;
				Comment.Text = Item.value("Text", std::string());
				if (const auto Position = Item.find("Position"); Position != Item.end())
				{
					ReadVector2(*Position, Comment.Position);
				}
				if (const auto Size = Item.find("Size"); Size != Item.end())
				{
					ReadVector2(*Size, Comment.Size);
				}
				Out.Comments.push_back(std::move(Comment));
			}
		}
	}

	json WriteGraph(const FMaterialGraph& Graph)
	{
		json Nodes = json::array();
		for (const FMaterialGraphNode& Node : Graph.Nodes)
		{
			json Item;
			Item["Id"]   = Node.Id;
			Item["Type"] = Node.Type;
			if (Node.Type == "Constant")
			{
				Item["Value"] = WriteNumbers(Node.Value, Node.ValueWidth);
			}
			if (Node.Type == "TexCoord")
			{
				if (Node.Value.X != 1.0f || Node.Value.Y != 1.0f)
				{
					Item["Tiling"] = { Node.Value.X, Node.Value.Y };
				}
				if (Node.Index != 0)
				{
					Item["Channel"] = Node.Index;
				}
			}
			if (!Node.Name.empty())
			{
				Item[GetNameKey(Node.Type)] = Node.Name;
			}
			if (!Node.Option.empty())
			{
				Item[GetOptionKey(Node.Type)] = Node.Option;
			}
			if (!Node.Inputs.empty())
			{
				json Inputs = json::object();
				for (const FMaterialGraphInput& Input : Node.Inputs)
				{
					Inputs[Input.Pin] = WriteInput(Input);
				}
				Item["Inputs"] = std::move(Inputs);
			}
			if (Node.EditorPosition.X != 0.0f || Node.EditorPosition.Y != 0.0f)
			{
				Item["EditorPosition"] = { Node.EditorPosition.X, Node.EditorPosition.Y };
			}
			Nodes.push_back(std::move(Item));
		}
		json Outputs = json::object();
		for (const FMaterialGraphInput& Output : Graph.Outputs)
		{
			Outputs[Output.Pin] = WriteInput(Output);
		}
		json Object;
		Object["Nodes"]  = std::move(Nodes);
		Object["Output"] = std::move(Outputs);
		if (Graph.OutputEditorPosition.X != 0.0f || Graph.OutputEditorPosition.Y != 0.0f)
		{
			Object["EditorOutputPosition"] = { Graph.OutputEditorPosition.X, Graph.OutputEditorPosition.Y };
		}
		if (!Graph.Comments.empty())
		{
			json Comments = json::array();
			for (const FMaterialGraphComment& Comment : Graph.Comments)
			{
				json Item;
				Item["Text"]     = Comment.Text;
				Item["Position"] = { Comment.Position.X, Comment.Position.Y };
				Item["Size"]     = { Comment.Size.X, Comment.Size.Y };
				Comments.push_back(std::move(Item));
			}
			Object["EditorComments"] = std::move(Comments);
		}
		return Object;
	}
} // namespace MaterialGraphJson
