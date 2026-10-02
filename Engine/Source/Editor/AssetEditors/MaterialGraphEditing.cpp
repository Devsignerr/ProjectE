#include "Editor/AssetEditors/MaterialGraphEditing.h"

#include "Renderer/MaterialGraphJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace MaterialGraphEditing
{
	namespace
	{
		constexpr float TitleHeight   = 24.0f;
		constexpr float SummaryHeight = 20.0f;
		constexpr float RowHeight     = 22.0f;
		constexpr float Padding       = 22.0f;

		bool IsMissing(const FVector2& Position) { return Position.X == 0.0f && Position.Y == 0.0f; }

		bool IsOutputNode(std::string_view Id) { return Id == FMaterialGraphCompiler::OutputNodeId; }

		uint32 OutputPinIndex(std::string_view Pin)
		{
			for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialOutput::Count); ++Index)
			{
				if (Pin == GetMaterialOutputName(static_cast<EMaterialOutput>(Index)))
				{
					return Index;
				}
			}
			return static_cast<uint32>(EMaterialOutput::Count);
		}

		uint32 InputPinIndex(const FMaterialGraphNode& Node, std::string_view Pin)
		{
			if (const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type))
			{
				for (uint32 Index = 0; Index < static_cast<uint32>(Info->Inputs.size()); ++Index)
				{
					if (Info->Inputs[Index].Name == Pin)
					{
						return Index;
					}
				}
			}
			return 15;
		}

		// "multiply12" → "multiply"
		std::string StripNumberSuffix(std::string_view Text)
		{
			size_t End = Text.size();
			while (End > 0 && std::isdigit(static_cast<unsigned char>(Text[End - 1])))
			{
				--End;
			}
			return std::string(Text.substr(0, End));
		}

		FMaterialParameter MakeParameter(std::string Name, EMaterialParameterType Type, const FVector4& Value, std::string Texture = {},
		                                 ETextureUsage Usage = ETextureUsage::Color)
		{
			FMaterialParameter Parameter;
			Parameter.Name    = std::move(Name);
			Parameter.Type    = Type;
			Parameter.Value   = Value;
			Parameter.Texture = std::move(Texture);
			Parameter.Usage   = Usage;
			return Parameter;
		}
	} // namespace

	FVector2 EstimateNodeSize(const FMaterialGraphNode& Node)
	{
		const FMaterialGraphNodeInfo* Info    = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
		const size_t                  Inputs  = Info ? Info->Inputs.size() : Node.Inputs.size();
		const size_t                  Outputs = Info ? Info->Outputs.size() : 1;
		const bool                    bSummary = Info && Info->Settings != MaterialNodeSetting_None;
		const float Height = Padding + TitleHeight + (bSummary ? SummaryHeight : 0.0f) + static_cast<float>(std::max<size_t>(std::max(Inputs, Outputs), 1)) * RowHeight;
		return FVector2(NodeWidth, Height);
	}

	FVector2 OutputNodeSize()
	{
		return FVector2(NodeWidth * 0.85f, Padding + TitleHeight + static_cast<float>(EMaterialOutput::Count) * RowHeight);
	}

	bool HasMissingPositions(const FMaterialGraph& Graph)
	{
		return IsMissing(Graph.OutputEditorPosition) ||
		       std::any_of(Graph.Nodes.begin(), Graph.Nodes.end(), [](const FMaterialGraphNode& Node) { return IsMissing(Node.EditorPosition); });
	}

	void AutoLayout(FMaterialGraph& Graph, bool bOnlyMissing)
	{
		const int32 Count = static_cast<int32>(Graph.Nodes.size());
		std::unordered_map<std::string, int32> IndexById;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			IndexById.emplace(Graph.Nodes[static_cast<size_t>(Index)].Id, Index);
		}
		const auto Find = [&](const std::string& Id) {
			const auto Found = IndexById.find(Id);
			return Found == IndexById.end() ? -1 : Found->second;
		};

		// 소비자 목록: (소비 노드 번호 또는 -1 = 출력, 핀 번호)
		struct FConsumer
		{
			int32  Node;
			uint32 Pin;
		};
		std::vector<std::vector<FConsumer>> Consumers(static_cast<size_t>(Count));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FMaterialGraphNode& Node = Graph.Nodes[static_cast<size_t>(Index)];
			for (const FMaterialGraphInput& Input : Node.Inputs)
			{
				if (const int32 Source = Input.IsLink() ? Find(Input.Node) : -1; Source >= 0)
				{
					Consumers[static_cast<size_t>(Source)].push_back({ Index, InputPinIndex(Node, Input.Pin) });
				}
			}
		}
		for (const FMaterialGraphInput& Output : Graph.Outputs)
		{
			if (const int32 Source = Output.IsLink() ? Find(Output.Node) : -1; Source >= 0)
			{
				Consumers[static_cast<size_t>(Source)].push_back({ -1, OutputPinIndex(Output.Pin) });
			}
		}

		// 열 = 출력까지 가장 긴 경로 (출력 = 0). 반복 완화, 상한 = 노드 수 (순환 안전)
		std::vector<int32> Column(static_cast<size_t>(Count), -1);
		const auto Relax = [&](bool bReachableOnly) {
			for (int32 Pass = 0; Pass <= Count; ++Pass)
			{
				bool bChanged = false;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					for (const FConsumer& Consumer : Consumers[static_cast<size_t>(Index)])
					{
						const int32 ConsumerColumn = Consumer.Node < 0 ? 0 : Column[static_cast<size_t>(Consumer.Node)];
						if (ConsumerColumn < 0 && bReachableOnly)
						{
							continue;
						}
						const int32 Wanted = std::min(std::max(ConsumerColumn, 0) + 1, Count);
						if (Wanted > Column[static_cast<size_t>(Index)])
						{
							Column[static_cast<size_t>(Index)] = Wanted;
							bChanged                          = true;
						}
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
		};
		Relax(true);
		std::vector<uint8> bReachable(static_cast<size_t>(Count), 0);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			bReachable[static_cast<size_t>(Index)] = Column[static_cast<size_t>(Index)] >= 0 ? 1 : 0;
		}
		// 닿지 않는 노드: 소비자가 없는 것을 열 1로 두고 같은 규칙
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Column[static_cast<size_t>(Index)] < 0 && Consumers[static_cast<size_t>(Index)].empty())
			{
				Column[static_cast<size_t>(Index)] = 1;
			}
		}
		Relax(false);
		int32 MaxColumn = 1;
		for (int32& Value : Column)
		{
			Value     = Value < 1 ? 1 : Value; // 닿지 않는 순환 등
			MaxColumn = std::max(MaxColumn, Value);
		}

		// 열 안 순서: 소비자 순서 키(출력 핀 → 앞 열 순서 * 16 + 핀), 닿는 노드 먼저, 같으면 원래 순서
		std::vector<float> Order(static_cast<size_t>(Count), 0.0f);
		std::vector<std::vector<int32>> Columns(static_cast<size_t>(MaxColumn) + 1);
		for (int32 ColumnIndex = 1; ColumnIndex <= MaxColumn; ++ColumnIndex)
		{
			std::vector<int32>& Members = Columns[static_cast<size_t>(ColumnIndex)];
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Column[static_cast<size_t>(Index)] == ColumnIndex)
				{
					Members.push_back(Index);
				}
			}
			std::vector<float> Key(static_cast<size_t>(Count), std::numeric_limits<float>::max());
			for (const int32 Index : Members)
			{
				for (const FConsumer& Consumer : Consumers[static_cast<size_t>(Index)])
				{
					const float ConsumerKey = Consumer.Node < 0 ? static_cast<float>(Consumer.Pin)
					                                            : Order[static_cast<size_t>(Consumer.Node)] * 16.0f + static_cast<float>(Consumer.Pin);
					Key[static_cast<size_t>(Index)] = std::min(Key[static_cast<size_t>(Index)], ConsumerKey);
				}
			}
			std::stable_sort(Members.begin(), Members.end(), [&](int32 A, int32 B) {
				if (bReachable[static_cast<size_t>(A)] != bReachable[static_cast<size_t>(B)])
				{
					return bReachable[static_cast<size_t>(A)] > bReachable[static_cast<size_t>(B)];
				}
				return Key[static_cast<size_t>(A)] < Key[static_cast<size_t>(B)];
			});
			for (size_t Slot = 0; Slot < Members.size(); ++Slot)
			{
				Order[static_cast<size_t>(Members[Slot])] = static_cast<float>(Slot);
			}
		}

		// 위치: 출력 노드 = (0, -높이/2), 열마다 세로로 쌓아 0을 가운데로
		const FVector2 OutputSize = OutputNodeSize();
		if (!bOnlyMissing || IsMissing(Graph.OutputEditorPosition))
		{
			Graph.OutputEditorPosition = FVector2(0.0f, -OutputSize.Y * 0.5f);
		}
		for (int32 ColumnIndex = 1; ColumnIndex <= MaxColumn; ++ColumnIndex)
		{
			const std::vector<int32>& Members = Columns[static_cast<size_t>(ColumnIndex)];
			float                     Total   = 0.0f;
			for (const int32 Index : Members)
			{
				Total += EstimateNodeSize(Graph.Nodes[static_cast<size_t>(Index)]).Y + RowGap;
			}
			float       Y = -Total * 0.5f;
			const float X = -static_cast<float>(ColumnIndex) * ColumnSpacing;
			for (const int32 Index : Members)
			{
				FMaterialGraphNode& Node = Graph.Nodes[static_cast<size_t>(Index)];
				if (!bOnlyMissing || IsMissing(Node.EditorPosition))
				{
					// 정확히 (0,0)은 "위치 없음"이므로 피한다
					Node.EditorPosition = FVector2(X, Y == 0.0f ? 1.0f : Y);
				}
				Y += EstimateNodeSize(Node).Y + RowGap;
			}
		}
	}

	std::string MakeUniqueNodeId(const FMaterialGraph& Graph, std::string_view Base)
	{
		std::string Stem = StripNumberSuffix(Base);
		if (Stem.empty())
		{
			Stem = "node";
		}
		if (!Base.empty() && Graph.FindNode(Base) == nullptr && !IsOutputNode(Base))
		{
			return std::string(Base);
		}
		for (uint32 Number = 1;; ++Number)
		{
			std::string Candidate = std::format("{}{}", Stem, Number);
			if (Graph.FindNode(Candidate) == nullptr)
			{
				return Candidate;
			}
		}
	}

	std::string MakeUniqueParameterName(const std::vector<FMaterialParameter>& Parameters, std::string_view Base)
	{
		const auto Exists = [&](const std::string& Name) {
			return std::any_of(Parameters.begin(), Parameters.end(), [&](const FMaterialParameter& Parameter) { return Parameter.Name == Name; });
		};
		const std::string Stem = StripNumberSuffix(Base).empty() ? std::string("Param") : StripNumberSuffix(Base);
		if (!Base.empty() && !Exists(std::string(Base)))
		{
			return std::string(Base);
		}
		for (uint32 Number = 1;; ++Number)
		{
			std::string Candidate = std::format("{}{}", Stem, Number);
			if (!Exists(Candidate))
			{
				return Candidate;
			}
		}
	}

	FMaterialGraphNode* AddNode(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, const std::string& Type, const FVector2& Position)
	{
		const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Type);
		if (Info == nullptr)
		{
			return nullptr;
		}
		FMaterialGraphNode Node;
		Node.Type = Type;
		std::string Base = Type;
		Base[0]          = static_cast<char>(std::tolower(static_cast<unsigned char>(Base[0])));
		Node.Id          = MakeUniqueNodeId(Graph, Base + "1");
		// 노드 편집기는 위치를 정수로 내리므로 미리 내린다 (다음 프레임 위치 동기화가 "이동"으로 보지 않게)
		const FVector2 Floored = FVector2(std::floor(Position.X), std::floor(Position.Y));
		Node.EditorPosition    = IsMissing(Floored) ? FVector2(1.0f, 0.0f) : Floored;
		if ((Info->Settings & MaterialNodeSetting_Tiling) != 0)
		{
			Node.Value      = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
			Node.ValueWidth = 2;
		}
		if ((Info->Settings & MaterialNodeSetting_Channels) != 0)
		{
			Node.Option = "xy";
		}
		if ((Info->Settings & MaterialNodeSetting_CompareOp) != 0)
		{
			Node.Option = "Greater";
		}
		if ((Info->Settings & MaterialNodeSetting_Parameter) != 0)
		{
			switch (Info->ParameterType)
			{
			case EMaterialParameterType::Scalar:
				Node.Name = MakeUniqueParameterName(Parameters, "Scalar");
				Parameters.push_back(MakeParameter(Node.Name, EMaterialParameterType::Scalar, FVector4(1.0f, 0.0f, 0.0f, 0.0f)));
				break;
			case EMaterialParameterType::Vector:
				Node.Name = MakeUniqueParameterName(Parameters, "Color");
				Parameters.push_back(MakeParameter(Node.Name, EMaterialParameterType::Vector, FVector4::OneVector));
				break;
			case EMaterialParameterType::StaticSwitch:
				Node.Name = MakeUniqueParameterName(Parameters, "Switch");
				Parameters.push_back(MakeParameter(Node.Name, EMaterialParameterType::StaticSwitch, FVector4(1.0f, 0.0f, 0.0f, 0.0f)));
				break;
			case EMaterialParameterType::Texture:
				Node.Name = MakeUniqueParameterName(Parameters, "Texture");
				Parameters.push_back(MakeParameter(Node.Name, EMaterialParameterType::Texture, FVector4::ZeroVector));
				break;
			default: break;
			}
		}
		Graph.Nodes.push_back(std::move(Node));
		return &Graph.Nodes.back();
	}

	void RemoveNodes(FMaterialGraph& Graph, const std::vector<std::string>& Ids)
	{
		const std::unordered_set<std::string> Removed(Ids.begin(), Ids.end());
		std::erase_if(Graph.Nodes, [&](const FMaterialGraphNode& Node) { return Removed.contains(Node.Id); });
		for (FMaterialGraphNode& Node : Graph.Nodes)
		{
			std::erase_if(Node.Inputs, [&](const FMaterialGraphInput& Input) { return Input.IsLink() && Removed.contains(Input.Node); });
		}
		std::erase_if(Graph.Outputs, [&](const FMaterialGraphInput& Input) { return Input.IsLink() && Removed.contains(Input.Node); });
	}

	bool WouldCreateCycle(const FMaterialGraph& Graph, std::string_view FromNode, std::string_view ToNode)
	{
		if (IsOutputNode(ToNode))
		{
			return false;
		}
		if (FromNode == ToNode)
		{
			return true;
		}
		// From이 (입력을 따라) To에 의존하면 To ← From 연결은 순환
		std::vector<std::string_view>        Stack = { FromNode };
		std::unordered_set<std::string_view> Visited;
		while (!Stack.empty())
		{
			const std::string_view Current = Stack.back();
			Stack.pop_back();
			if (Current == ToNode)
			{
				return true;
			}
			if (!Visited.insert(Current).second)
			{
				continue;
			}
			if (const FMaterialGraphNode* Node = Graph.FindNode(Current))
			{
				for (const FMaterialGraphInput& Input : Node->Inputs)
				{
					if (Input.IsLink())
					{
						Stack.push_back(Input.Node);
					}
				}
			}
		}
		return false;
	}

	namespace
	{
		std::vector<FMaterialGraphInput>* FindInputList(FMaterialGraph& Graph, std::string_view ToNode)
		{
			if (IsOutputNode(ToNode))
			{
				return &Graph.Outputs;
			}
			FMaterialGraphNode* Node = Graph.FindNode(ToNode);
			return Node ? &Node->Inputs : nullptr;
		}
	} // namespace

	bool Connect(FMaterialGraph& Graph, const std::string& FromNode, uint32 FromOutput, const std::string& ToNode, const std::string& ToPin)
	{
		if (Graph.FindNode(FromNode) == nullptr || WouldCreateCycle(Graph, FromNode, ToNode))
		{
			return false;
		}
		std::vector<FMaterialGraphInput>* Inputs = FindInputList(Graph, ToNode);
		if (Inputs == nullptr)
		{
			return false;
		}
		std::erase_if(*Inputs, [&](const FMaterialGraphInput& Input) { return Input.Pin == ToPin; });
		FMaterialGraphInput Link;
		Link.Pin    = ToPin;
		Link.Node   = FromNode;
		Link.Output = FromOutput;
		Inputs->push_back(std::move(Link));
		return true;
	}

	bool Disconnect(FMaterialGraph& Graph, const std::string& ToNode, const std::string& ToPin)
	{
		std::vector<FMaterialGraphInput>* Inputs = FindInputList(Graph, ToNode);
		return Inputs != nullptr && std::erase_if(*Inputs, [&](const FMaterialGraphInput& Input) { return Input.Pin == ToPin; }) > 0;
	}

	const FMaterialGraphInput* FindInput(const FMaterialGraph& Graph, std::string_view ToNode, std::string_view ToPin)
	{
		const std::vector<FMaterialGraphInput>* Inputs = FindInputList(const_cast<FMaterialGraph&>(Graph), ToNode);
		if (Inputs == nullptr)
		{
			return nullptr;
		}
		for (const FMaterialGraphInput& Input : *Inputs)
		{
			if (Input.Pin == ToPin)
			{
				return &Input;
			}
		}
		return nullptr;
	}

	void SetConstant(FMaterialGraph& Graph, const std::string& ToNode, const std::string& ToPin, const FVector4& Value, uint32 Width)
	{
		std::vector<FMaterialGraphInput>* Inputs = FindInputList(Graph, ToNode);
		if (Inputs == nullptr)
		{
			return;
		}
		std::erase_if(*Inputs, [&](const FMaterialGraphInput& Input) { return Input.Pin == ToPin; });
		FMaterialGraphInput Constant;
		Constant.Pin           = ToPin;
		Constant.Constant      = Value;
		Constant.ConstantWidth = std::clamp(Width, 1u, 4u);
		Inputs->push_back(std::move(Constant));
	}

	bool RenameParameter(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, const std::string& OldName, const std::string& NewName)
	{
		if (NewName.empty() || NewName == OldName ||
		    std::any_of(Parameters.begin(), Parameters.end(), [&](const FMaterialParameter& Parameter) { return Parameter.Name == NewName; }))
		{
			return false;
		}
		bool bFound = false;
		for (FMaterialParameter& Parameter : Parameters)
		{
			if (Parameter.Name == OldName)
			{
				Parameter.Name = NewName;
				bFound         = true;
			}
		}
		for (FMaterialGraphNode& Node : Graph.Nodes)
		{
			const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
			if (Info != nullptr && (Info->Settings & MaterialNodeSetting_Parameter) != 0 && Node.Name == OldName)
			{
				Node.Name = NewName;
			}
		}
		return bFound;
	}

	uint32 CountParameterUses(const FMaterialGraph& Graph, std::string_view Name)
	{
		uint32 Uses = 0;
		for (const FMaterialGraphNode& Node : Graph.Nodes)
		{
			const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
			Uses += Info != nullptr && (Info->Settings & MaterialNodeSetting_Parameter) != 0 && Node.Name == Name ? 1u : 0u;
		}
		return Uses;
	}

	std::string CopyNodes(const FMaterialGraph& Graph, const std::vector<FMaterialParameter>& Parameters, const std::vector<std::string>& Ids)
	{
		const std::unordered_set<std::string> Selected(Ids.begin(), Ids.end());
		FMaterialGraph                        Copied;
		std::vector<FMaterialParameter>       Referenced;
		for (const FMaterialGraphNode& Node : Graph.Nodes)
		{
			if (!Selected.contains(Node.Id))
			{
				continue;
			}
			FMaterialGraphNode Copy = Node;
			std::erase_if(Copy.Inputs, [&](const FMaterialGraphInput& Input) { return Input.IsLink() && !Selected.contains(Input.Node); });
			const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
			if (Info != nullptr && (Info->Settings & MaterialNodeSetting_Parameter) != 0)
			{
				for (const FMaterialParameter& Parameter : Parameters)
				{
					const bool bAlready = std::any_of(Referenced.begin(), Referenced.end(), [&](const FMaterialParameter& Item) { return Item.Name == Parameter.Name; });
					if (Parameter.Name == Node.Name && !bAlready)
					{
						Referenced.push_back(Parameter);
					}
				}
			}
			// 위치 (0,0) = 위치 없음이 되지 않게
			if (IsMissing(Copy.EditorPosition))
			{
				Copy.EditorPosition = FVector2(1.0f, 0.0f);
			}
			Copied.Nodes.push_back(std::move(Copy));
		}
		if (Copied.Nodes.empty())
		{
			return {};
		}
		nlohmann::json Document;
		Document["ProjectEMaterialNodes"] = 1;
		Document["Graph"]                 = MaterialGraphJson::WriteGraph(Copied);
		Document["Parameters"]            = MaterialGraphJson::WriteParameters(Referenced);
		return Document.dump(2);
	}

	bool IsClipboardText(std::string_view Text)
	{
		const nlohmann::json Document = nlohmann::json::parse(Text, nullptr, false);
		return !Document.is_discarded() && Document.is_object() && Document.contains("ProjectEMaterialNodes") && Document.contains("Graph");
	}

	std::vector<std::string> PasteNodes(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, std::string_view Text, const FVector2& Anchor)
	{
		if (!IsClipboardText(Text))
		{
			return {};
		}
		const nlohmann::json            Document = nlohmann::json::parse(Text, nullptr, false);
		FMaterialGraph                  Pasted;
		std::vector<FMaterialParameter> PastedParameters;
		std::vector<std::string>        Warnings;
		MaterialGraphJson::ReadGraph(Document["Graph"], Pasted, Warnings);
		if (Document.contains("Parameters"))
		{
			MaterialGraphJson::ReadParameters(Document["Parameters"], PastedParameters, Warnings);
		}
		if (Pasted.Nodes.empty())
		{
			return {};
		}

		// 새 Id (이미 붙인 것과도 겹치지 않게 하나씩 넣으며 정한다)
		std::map<std::string, std::string> Remap;
		FMaterialGraph                     Probe = Graph;
		for (const FMaterialGraphNode& Node : Pasted.Nodes)
		{
			const std::string NewId = MakeUniqueNodeId(Probe, Node.Id.empty() ? std::string("node1") : Node.Id);
			Remap[Node.Id]          = NewId;
			FMaterialGraphNode Placeholder;
			Placeholder.Id = NewId;
			Probe.Nodes.push_back(std::move(Placeholder));
		}
		FVector2 Min(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
		for (const FMaterialGraphNode& Node : Pasted.Nodes)
		{
			Min.X = std::min(Min.X, Node.EditorPosition.X);
			Min.Y = std::min(Min.Y, Node.EditorPosition.Y);
		}
		std::vector<std::string> NewIds;
		for (FMaterialGraphNode& Node : Pasted.Nodes)
		{
			Node.Id = Remap[Node.Id];
			// 붙이는 묶음 밖을 가리키는 연결은 버린다 (복사 때 이미 끊지만 손으로 쓴 클립보드 대비)
			std::erase_if(Node.Inputs, [&](const FMaterialGraphInput& Input) { return Input.IsLink() && !Remap.contains(Input.Node); });
			for (FMaterialGraphInput& Input : Node.Inputs)
			{
				if (Input.IsLink())
				{
					Input.Node = Remap[Input.Node];
				}
			}
			Node.EditorPosition = Node.EditorPosition - Min + FVector2(std::floor(Anchor.X), std::floor(Anchor.Y));
			if (IsMissing(Node.EditorPosition))
			{
				Node.EditorPosition = FVector2(1.0f, 0.0f);
			}
			NewIds.push_back(Node.Id);
			Graph.Nodes.push_back(std::move(Node));
		}
		for (FMaterialParameter& Parameter : PastedParameters)
		{
			if (std::none_of(Parameters.begin(), Parameters.end(), [&](const FMaterialParameter& Item) { return Item.Name == Parameter.Name; }))
			{
				Parameters.push_back(std::move(Parameter));
			}
		}
		return NewIds;
	}

	void ConvertToGraph(FMaterialAsset& Asset, bool bAlwaysBaseColorTexture)
	{
		// MaterialDefault.hlsli와 같은 식 (곱 순서까지): 텍스처 × 정점 색 × 팩터 등. 빈 텍스처 슬롯은 기본 텍스처(흰색/평면 노멀)와 같으므로 노드를 두지 않는다
		FMaterialGraph                  Graph;
		std::vector<FMaterialParameter> Parameters;
		const FMaterialConstants&       C        = Asset.Constants;
		const auto                      Slot     = [&](uint32 Index) -> const std::string& { return Asset.TexturePaths[Index]; };
		const auto                      AddRaw   = [&](std::string Id, std::string Type) -> FMaterialGraphNode& {
            FMaterialGraphNode Node;
            Node.Id   = std::move(Id);
            Node.Type = std::move(Type);
            Graph.Nodes.push_back(std::move(Node));
            return Graph.Nodes.back();
		};
		const auto Link = [](FMaterialGraphNode& Node, const char* Pin, const std::string& Source, uint32 Output = 0) {
			FMaterialGraphInput Input;
			Input.Pin    = Pin;
			Input.Node   = Source;
			Input.Output = Output;
			Node.Inputs.push_back(std::move(Input));
		};
		const auto Constant = [](FMaterialGraphNode& Node, const char* Pin, float Value) {
			FMaterialGraphInput Input;
			Input.Pin        = Pin;
			Input.Constant.X = Value;
			Node.Inputs.push_back(std::move(Input));
		};
		const auto Output = [&](EMaterialOutput Pin, const std::string& Source, uint32 SourceOutput = 0) {
			FMaterialGraphInput Input;
			Input.Pin    = GetMaterialOutputName(Pin);
			Input.Node   = Source;
			Input.Output = SourceOutput;
			Graph.Outputs.push_back(std::move(Input));
		};
		const auto Texture = [&](const char* ParameterName, uint32 SlotIndex, const char* NodeId) {
			Parameters.push_back(MakeParameter(ParameterName, EMaterialParameterType::Texture, FVector4::ZeroVector, Slot(SlotIndex),
			                                   FMaterialAsset::GetSlotUsage(SlotIndex)));
			AddRaw(NodeId, "TextureSample").Name = ParameterName;
		};

		// 베이스 컬러 (RGBA) = [텍스처] × 정점 색 × 팩터
		Parameters.push_back(MakeParameter("BaseColor", EMaterialParameterType::Vector, C.BaseColorFactor));
		AddRaw("vertexColor", "VertexColor");
		AddRaw("baseColorFactor", "VectorParameter").Name = "BaseColor";
		std::string Tinted = "vertexColor";
		if (bAlwaysBaseColorTexture || !Slot(MaterialSlot_BaseColor).empty())
		{
			Texture("BaseColorTexture", MaterialSlot_BaseColor, "baseColorTexture");
			FMaterialGraphNode& Mul = AddRaw("baseTexVertex", "Multiply");
			Link(Mul, "A", "baseColorTexture");
			Link(Mul, "B", "vertexColor");
			Tinted = "baseTexVertex";
		}
		FMaterialGraphNode& Base = AddRaw("baseColor", "Multiply");
		Link(Base, "A", Tinted);
		Link(Base, "B", "baseColorFactor");
		Output(EMaterialOutput::BaseColor, "baseColor");
		if (Asset.BlendMode != EMaterialBlendMode::Opaque)
		{
			FMaterialGraphNode& Alpha = AddRaw("opacity", "ComponentMask");
			Alpha.Option              = "w";
			Link(Alpha, "A", "baseColor");
			Output(EMaterialOutput::Opacity, "opacity");
			Output(EMaterialOutput::OpacityMask, "opacity");
		}

		// 금속/거칠기 = [MR 텍스처 B/G] × 팩터
		Parameters.push_back(MakeParameter("Metallic", EMaterialParameterType::Scalar, FVector4(C.Metallic, 0.0f, 0.0f, 0.0f)));
		Parameters.push_back(MakeParameter("Roughness", EMaterialParameterType::Scalar, FVector4(C.Roughness, 0.0f, 0.0f, 0.0f)));
		AddRaw("metallicFactor", "ScalarParameter").Name  = "Metallic";
		AddRaw("roughnessFactor", "ScalarParameter").Name = "Roughness";
		if (!Slot(MaterialSlot_MetallicRoughness).empty())
		{
			Texture("MetallicRoughnessTexture", MaterialSlot_MetallicRoughness, "metallicRoughnessTexture");
			FMaterialGraphNode& Metal = AddRaw("metallic", "Multiply");
			Link(Metal, "A", "metallicRoughnessTexture", 4); // B
			Link(Metal, "B", "metallicFactor");
			FMaterialGraphNode& Rough = AddRaw("roughness", "Multiply");
			Link(Rough, "A", "metallicRoughnessTexture", 3); // G
			Link(Rough, "B", "roughnessFactor");
			Output(EMaterialOutput::Metallic, "metallic");
			Output(EMaterialOutput::Roughness, "roughness");
		}
		else
		{
			Output(EMaterialOutput::Metallic, "metallicFactor");
			Output(EMaterialOutput::Roughness, "roughnessFactor");
		}

		// 노멀: 디코드(XY → Z) 뒤 XY에 강도
		if (!Slot(MaterialSlot_Normal).empty())
		{
			Texture("NormalTexture", MaterialSlot_Normal, "normalTexture");
			if (C.NormalScale == 1.0f)
			{
				Output(EMaterialOutput::Normal, "normalTexture", 1);
			}
			else
			{
				Parameters.push_back(MakeParameter("NormalScale", EMaterialParameterType::Scalar, FVector4(C.NormalScale, 0.0f, 0.0f, 0.0f)));
				AddRaw("normalScale", "ScalarParameter").Name = "NormalScale";
				FMaterialGraphNode& ScaleXY = AddRaw("normalScaleXY", "Append");
				Link(ScaleXY, "A", "normalScale");
				Link(ScaleXY, "B", "normalScale");
				FMaterialGraphNode& Scale = AddRaw("normalScaleXYZ", "Append");
				Link(Scale, "A", "normalScaleXY");
				Constant(Scale, "B", 1.0f);
				FMaterialGraphNode& Normal = AddRaw("normal", "Multiply");
				Link(Normal, "A", "normalTexture", 1);
				Link(Normal, "B", "normalScaleXYZ");
				Output(EMaterialOutput::Normal, "normal");
			}
		}

		// AO = lerp(1, AO.r, 강도)
		if (!Slot(MaterialSlot_Occlusion).empty())
		{
			Texture("OcclusionTexture", MaterialSlot_Occlusion, "occlusionTexture");
			Parameters.push_back(MakeParameter("OcclusionStrength", EMaterialParameterType::Scalar, FVector4(C.OcclusionStrength, 0.0f, 0.0f, 0.0f)));
			AddRaw("occlusionStrength", "ScalarParameter").Name = "OcclusionStrength";
			FMaterialGraphNode& Ao = AddRaw("ambientOcclusion", "Lerp");
			Constant(Ao, "A", 1.0f);
			Link(Ao, "B", "occlusionTexture", 2); // R
			Link(Ao, "Alpha", "occlusionStrength");
			Output(EMaterialOutput::AmbientOcclusion, "ambientOcclusion");
		}

		// 발광 = [텍스처 RGB] × 팩터
		const bool bEmissiveFactor = C.EmissiveFactor.X != 0.0f || C.EmissiveFactor.Y != 0.0f || C.EmissiveFactor.Z != 0.0f;
		if (bEmissiveFactor || !Slot(MaterialSlot_Emissive).empty())
		{
			Parameters.push_back(MakeParameter("Emissive", EMaterialParameterType::Vector,
			                                   FVector4(C.EmissiveFactor.X, C.EmissiveFactor.Y, C.EmissiveFactor.Z, 1.0f)));
			AddRaw("emissiveFactor", "VectorParameter").Name = "Emissive";
			if (!Slot(MaterialSlot_Emissive).empty())
			{
				Texture("EmissiveTexture", MaterialSlot_Emissive, "emissiveTexture");
				FMaterialGraphNode& Emissive = AddRaw("emissive", "Multiply");
				Link(Emissive, "A", "emissiveTexture", 1);
				Link(Emissive, "B", "emissiveFactor", 1);
				Output(EMaterialOutput::Emissive, "emissive");
			}
			else
			{
				Output(EMaterialOutput::Emissive, "emissiveFactor", 1);
			}
		}

		AutoLayout(Graph, false);
		Asset.bHasGraph  = true;
		Asset.Graph      = std::move(Graph);
		Asset.Parameters = std::move(Parameters);
		for (std::string& Path : Asset.TexturePaths)
		{
			Path.clear(); // 텍스처는 파라미터로 옮겼다 (그래프 머티리얼은 고정 슬롯을 쓰지 않음)
		}
	}

	FMaterialAsset MakeDefaultGraphMaterial(const std::string& Name)
	{
		FMaterialAsset Asset;
		Asset.Name = Name;
		ConvertToGraph(Asset, true);
		return Asset;
	}
} // namespace MaterialGraphEditing
