#include <imgui.h>

#pragma warning(push, 0)
#include <imgui_node_editor.h>
#pragma warning(pop)

#include "Editor/AssetEditors/MaterialGraphPanel.h"

#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/MaterialGraphEditing.h"
#include "Editor/EditorTheme.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <format>

namespace NodeEditor = ax::NodeEditor;

namespace
{
	// 편집기 ID: 키 << 8 | 종류 (0 노드, 1~0x3F 입력 핀 번호 + 1, 0x40~0x7F 출력 핀, 0x80~ 입력 핀으로 들어오는 링크)
	constexpr uint32    OutputKey      = 1;
	constexpr uint32    CommentKeyBase = 0x00F00000;
	constexpr uintptr_t InputBase      = 0x01;
	constexpr uintptr_t OutputBase     = 0x40;
	constexpr uintptr_t LinkBase       = 0x80;
	constexpr float     PinIconSize    = 12.0f;

	NodeEditor::NodeId NodeIdOf(uint32 Key) { return NodeEditor::NodeId(static_cast<uintptr_t>(Key) << 8); }
	NodeEditor::PinId  InputPinOf(uint32 Key, uint32 Pin) { return NodeEditor::PinId((static_cast<uintptr_t>(Key) << 8) | (InputBase + Pin)); }
	NodeEditor::PinId  OutputPinOf(uint32 Key, uint32 Output) { return NodeEditor::PinId((static_cast<uintptr_t>(Key) << 8) | (OutputBase + Output)); }
	NodeEditor::LinkId LinkOf(uint32 Key, uint32 Pin) { return NodeEditor::LinkId((static_cast<uintptr_t>(Key) << 8) | (LinkBase + Pin)); }
	uint32             KeyOf(uintptr_t Raw) { return static_cast<uint32>(Raw >> 8); }
	uintptr_t          LowOf(uintptr_t Raw) { return Raw & 0xFF; }
	bool               IsInputPin(uintptr_t Raw) { return LowOf(Raw) >= InputBase && LowOf(Raw) < OutputBase; }
	bool               IsOutputPin(uintptr_t Raw) { return LowOf(Raw) >= OutputBase && LowOf(Raw) < LinkBase; }

	ImU32 ToU32(const ImVec4& Color) { return ImGui::ColorConvertFloat4ToU32(Color); }

	// 출력 노드 핀 기본값 (FMaterialGraphCompiler와 같은 값 — 보기용)
	const std::vector<FMaterialGraphPinInfo>& GetOutputPins()
	{
		static const std::vector<FMaterialGraphPinInfo> Pins = [] {
			static const FVector4 Defaults[] = { FVector4(0.5f, 0.5f, 0.5f, 0.0f), FVector4::ZeroVector, FVector4(0.5f, 0.0f, 0.0f, 0.0f),
				                                 FVector4(0.0f, 0.0f, 1.0f, 0.0f), FVector4::OneVector,  FVector4::ZeroVector,
				                                 FVector4::OneVector,                FVector4::OneVector };
			std::vector<FMaterialGraphPinInfo> Result;
			for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialOutput::Count); ++Index)
			{
				const EMaterialOutput Output = static_cast<EMaterialOutput>(Index);
				Result.push_back({ GetMaterialOutputName(Output), EMaterialPinDefault::Constant, Defaults[Index], GetMaterialOutputWidth(Output) });
			}
			return Result;
		}();
		return Pins;
	}

	std::string FormatNumber(float Value) { return std::format("{:.4g}", Value); }

	std::string FormatValue(const FVector4& Value, uint32 Width)
	{
		if (Width <= 1)
		{
			return FormatNumber(Value.X);
		}
		std::string Text = "(";
		for (uint32 Index = 0; Index < Width && Index < 4; ++Index)
		{
			Text += (Index == 0 ? "" : ", ") + FormatNumber((&Value.X)[Index]);
		}
		return Text + ")";
	}

	std::string DescribeDefault(const FMaterialGraphPinInfo& Pin)
	{
		switch (Pin.Default)
		{
		case EMaterialPinDefault::Constant:    return "기본 " + FormatValue(Pin.Constant, Pin.ConstantWidth);
		case EMaterialPinDefault::TexCoord:    return "기본 UV0";
		case EMaterialPinDefault::Time:        return "기본 시간";
		case EMaterialPinDefault::WorldNormal: return "기본 월드 노멀";
		default:                               return "연결 필요";
		}
	}

	// 범주별 제목 띠 색
	ImU32 GetCategoryColor(std::string_view Category)
	{
		if (Category == "상수·파라미터") return IM_COL32(60, 110, 60, 255);
		if (Category == "텍스처·입력") return IM_COL32(120, 60, 50, 255);
		if (Category == "산술") return IM_COL32(55, 75, 110, 255);
		if (Category == "벡터") return IM_COL32(40, 100, 105, 255);
		if (Category == "머티리얼 함수") return IM_COL32(95, 60, 120, 255);
		return IM_COL32(80, 80, 85, 255);
	}

	void DrawPinIcon(ImU32 Color, bool bFilled)
	{
		ImGui::Dummy(ImVec2(PinIconSize, PinIconSize));
		const ImVec2 Min    = ImGui::GetItemRectMin();
		const ImVec2 Max    = ImGui::GetItemRectMax();
		const ImVec2 Center = ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
		ImDrawList*  List   = ImGui::GetWindowDrawList();
		if (bFilled)
		{
			List->AddCircleFilled(Center, PinIconSize * 0.36f, Color);
		}
		else
		{
			List->AddCircle(Center, PinIconSize * 0.36f, Color, 0, 1.6f);
		}
		NodeEditor::PinPivotRect(Min, Max);
	}

	bool InputString(const char* Label, std::string& Value, ImGuiInputTextFlags Flags = 0)
	{
		char Buffer[256];
		strncpy_s(Buffer, Value.c_str(), _TRUNCATE);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer), Flags))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	bool ContainsNoCase(std::string_view Text, std::string_view Filter)
	{
		if (Filter.empty())
		{
			return true;
		}
		const auto Lower = [](char Char) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Char))); };
		return std::search(Text.begin(), Text.end(), Filter.begin(), Filter.end(), [&](char A, char B) { return Lower(A) == Lower(B); }) != Text.end();
	}

	const char* const SamplerNames[] = { "Wrap", "Clamp" };
	const char* const CompareNames[] = { "Greater", "GreaterEqual", "Less", "LessEqual", "Equal", "NotEqual" };
	const char* const CompareSymbols[] = { ">", ">=", "<", "<=", "==", "!=" };
} // namespace

void FMaterialGraphPanel::FGraphDeleter::operator()(NodeEditor::EditorContext* Context) const
{
	NodeEditor::DestroyEditor(Context);
}

FMaterialGraphPanel::FMaterialGraphPanel()
{
	NodeEditor::Config Config;
	Config.SettingsFile = nullptr; // 위치는 에셋(EditorPosition)에 저장한다
	Editor.reset(NodeEditor::CreateEditor(&Config));
}

FMaterialGraphPanel::~FMaterialGraphPanel() = default;

uint32 FMaterialGraphPanel::GetWidthColor(uint32 Width)
{
	switch (Width)
	{
	case 1:  return IM_COL32(150, 220, 120, 255); // float1 초록
	case 2:  return IM_COL32(110, 205, 235, 255); // float2 하늘
	case 3:  return IM_COL32(240, 210, 90, 255);  // float3 노랑
	case 4:  return IM_COL32(235, 120, 190, 255); // float4 분홍
	default: return IM_COL32(150, 150, 155, 255); // 모름
	}
}

void FMaterialGraphPanel::OnAssetReloaded(bool bFrame)
{
	bApplyPositions = true;
	if (bFrame)
	{
		NavigateFrames = 2;
	}
}

uint32 FMaterialGraphPanel::GetKey(const std::string& NodeId)
{
	if (NodeId == FMaterialGraphCompiler::OutputNodeId)
	{
		return OutputKey;
	}
	const auto [It, bInserted] = Keys.emplace(NodeId, NextKey);
	if (bInserted)
	{
		++NextKey;
	}
	return It->second;
}

std::string FMaterialGraphPanel::FindNodeIdByKey(uint32 Key) const
{
	if (Key == OutputKey)
	{
		return FMaterialGraphCompiler::OutputNodeId;
	}
	for (const auto& [Id, Value] : Keys)
	{
		if (Value == Key)
		{
			return Id;
		}
	}
	return {};
}

void FMaterialGraphPanel::BuildStatus(const FContext& Context)
{
	NodeMessages.clear();
	NodeSeverity.clear();
	const auto Add = [&](const std::string& Node, const std::string& Message, int32 Severity) {
		std::vector<std::string>& Messages = NodeMessages[Node];
		if (std::find(Messages.begin(), Messages.end(), Message) == Messages.end())
		{
			Messages.push_back(Message);
		}
		NodeSeverity[Node] = std::max(NodeSeverity[Node], Severity);
	};
	if (Context.Compile != nullptr)
	{
		for (size_t Index = 0; Index < Context.Compile->Errors.size() && Index < Context.Compile->ErrorNodes.size(); ++Index)
		{
			if (!Context.Compile->ErrorNodes[Index].empty())
			{
				Add(Context.Compile->ErrorNodes[Index], Context.Compile->Errors[Index], 2);
			}
		}
	}
	if (Context.Analysis != nullptr)
	{
		for (size_t Index = 0; Index < Context.Analysis->Errors.size() && Index < Context.Analysis->ErrorNodes.size(); ++Index)
		{
			if (!Context.Analysis->ErrorNodes[Index].empty())
			{
				Add(Context.Analysis->ErrorNodes[Index], Context.Analysis->Errors[Index], 1);
			}
		}
	}
}

uint32 FMaterialGraphPanel::GetSourceWidth(const FContext& Context, const FMaterialGraphInput& Input) const
{
	if (!Input.IsLink())
	{
		return Input.ConstantWidth;
	}
	if (Context.Analysis != nullptr)
	{
		if (const std::vector<uint32>* Widths = Context.Analysis->FindOutputWidths(Input.Node); Widths != nullptr && Input.Output < Widths->size())
		{
			return (*Widths)[Input.Output];
		}
	}
	return 0;
}

std::string FMaterialGraphPanel::MakeSummary(const FContext& Context, const FMaterialGraphNode& Node) const
{
	const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
	if (Info == nullptr)
	{
		return {};
	}
	std::string Summary;
	if ((Info->Settings & MaterialNodeSetting_Value) != 0)
	{
		Summary = FormatValue(Node.Value, std::clamp(Node.ValueWidth, 1u, 4u));
	}
	if ((Info->Settings & MaterialNodeSetting_Tiling) != 0 && (Node.Value.X != 1.0f || Node.Value.Y != 1.0f))
	{
		Summary = "× " + FormatValue(Node.Value, 2);
	}
	if ((Info->Settings & MaterialNodeSetting_Parameter) != 0)
	{
		Summary = Node.Name.empty() ? std::string("(파라미터 없음)") : Node.Name;
		for (const FMaterialParameter& Parameter : *Context.Parameters)
		{
			if (Parameter.Name != Node.Name)
			{
				continue;
			}
			switch (Parameter.Type)
			{
			case EMaterialParameterType::Scalar:       Summary += " = " + FormatNumber(Parameter.Value.X); break;
			case EMaterialParameterType::Vector:       Summary += " = " + FormatValue(Parameter.Value, 4); break;
			case EMaterialParameterType::StaticSwitch: Summary += Parameter.GetBool() ? " (켜짐)" : " (꺼짐)"; break;
			case EMaterialParameterType::Texture:
				Summary += std::format(" · {}", GetTextureUsageName(Parameter.Usage));
				break;
			default: break;
			}
		}
	}
	if ((Info->Settings & MaterialNodeSetting_Sampler) != 0 && Node.Option == "Clamp")
	{
		Summary += " · Clamp";
	}
	if ((Info->Settings & MaterialNodeSetting_Channels) != 0)
	{
		Summary = "." + Node.Option;
	}
	if ((Info->Settings & MaterialNodeSetting_CompareOp) != 0)
	{
		Summary = "A ? B";
		for (size_t Index = 0; Index < std::size(CompareNames); ++Index)
		{
			Summary = Node.Option == CompareNames[Index] ? std::format("A {} B", CompareSymbols[Index]) : Summary;
		}
	}
	return Summary;
}

// ---------------------------------------------------------------- 그리기

void FMaterialGraphPanel::Draw(FContext& Context)
{
	BuildStatus(Context);
	FMaterialGraph& Graph = *Context.Graph;

	NodeEditor::SetCurrentEditor(Editor.get());
	NodeEditor::Begin("##MaterialGraph", ImVec2(0.0f, 0.0f));
	const bool bAppliedPositions = bApplyPositions;
	if (bApplyPositions)
	{
		ApplyPositions(Context);
		bApplyPositions = false;
	}

	DrawComments(Context);
	for (const FMaterialGraphNode& Node : Graph.Nodes)
	{
		DrawNode(Context, Node);
	}
	DrawOutputNode(Context);
	DrawLinks(Context);

	HandleCreateAndDelete(Context);
	HandleShortcuts(Context);
	DrawContextMenus(Context);

	// 선택 → 상세 패널
	{
		std::vector<NodeEditor::NodeId> Selected(static_cast<size_t>(std::max(NodeEditor::GetSelectedObjectCount(), 1)));
		const int32                     Count = NodeEditor::GetSelectedNodes(Selected.data(), static_cast<int>(Selected.size()));
		SelectedNodes.clear();
		SelectedComment = -1;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const uint32 Key = KeyOf(Selected[static_cast<size_t>(Index)].Get());
			if (Key >= CommentKeyBase)
			{
				SelectedComment = Count == 1 ? static_cast<int32>(Key - CommentKeyBase) : -1;
			}
			else if (Key == OutputKey)
			{
				SelectedNodes.push_back(FMaterialGraphCompiler::OutputNodeId);
			}
			else if (std::string Id = FindNodeIdByKey(Key); !Id.empty())
			{
				SelectedNodes.push_back(std::move(Id));
			}
		}
	}

	if (!AutoSelectId.empty())
	{
		NavigateFrames = -1; // 자동 선택은 1:1 배율로 그 노드를 가운데에 (전체 보기 대신)
	}
	if (NavigateFrames >= 0 && NavigateFrames-- == 0)
	{
		NodeEditor::NavigateToContent(0.0f);
	}
	if (!AutoSelectId.empty() && AutoSelectFrames >= 0 && AutoSelectFrames-- == 0)
	{
		if (AutoSelectId == FMaterialGraphCompiler::OutputNodeId || Graph.FindNode(AutoSelectId) != nullptr)
		{
			const NodeEditor::NodeId Target = NodeIdOf(GetKey(AutoSelectId));
			NodeEditor::SelectNode(Target);
			NodeEditor::NavigateToSelection(false, 0.0f); // 배율은 키우지 않고 가운데로 (CenterNodeOnScreen은 노드를 옮긴다)
		}
		AutoSelectId.clear();
	}
	if (bPaletteRequestCenter)
	{
		bPaletteRequestCenter = false;
		bOpenPalette          = true;
		PalettePin            = 0;
		PaletteSpawn          = FVector2(NodeEditor::ScreenToCanvas(ImGui::GetMousePos()).x, NodeEditor::ScreenToCanvas(ImGui::GetMousePos()).y);
	}

	NodeEditor::End();
	if (!bAppliedPositions && !Context.bReadOnly)
	{
		SyncFromGraph(Context);
	}
	NodeEditor::SetCurrentEditor(nullptr);

	DrawPalette(Context);

	// 그래프 순회가 끝난 뒤 구조 변경 적용
	std::vector<std::function<void()>> Changes = std::move(PendingChanges);
	PendingChanges.clear();
	for (std::function<void()>& Change : Changes)
	{
		Change();
	}
}

void FMaterialGraphPanel::ApplyPositions(FContext& Context)
{
	FMaterialGraph& Graph = *Context.Graph;
	for (const FMaterialGraphNode& Node : Graph.Nodes)
	{
		NodeEditor::SetNodePosition(NodeIdOf(GetKey(Node.Id)), ImVec2(Node.EditorPosition.X, Node.EditorPosition.Y));
	}
	NodeEditor::SetNodePosition(NodeIdOf(OutputKey), ImVec2(Graph.OutputEditorPosition.X, Graph.OutputEditorPosition.Y));
	for (size_t Index = 0; Index < Graph.Comments.size(); ++Index)
	{
		const FMaterialGraphComment& Comment = Graph.Comments[Index];
		const NodeEditor::NodeId     Id      = NodeIdOf(CommentKeyBase + static_cast<uint32>(Index));
		NodeEditor::SetNodePosition(Id, ImVec2(Comment.Position.X, Comment.Position.Y));
		NodeEditor::SetGroupSize(Id, ImVec2(Comment.Size.X, Comment.Size.Y));
	}
	CommentNodeSizes.clear(); // 크기 변화 추적을 다시 시작
}

void FMaterialGraphPanel::SyncFromGraph(FContext& Context)
{
	FMaterialGraph& Graph  = *Context.Graph;
	bool            bMoved = false;
	const auto      Sync   = [&](uint32 Key, FVector2& Position) {
        const ImVec2 Current = NodeEditor::GetNodePosition(NodeIdOf(Key));
        if (Current.x == FLT_MAX)
        {
            return; // 아직 그려지지 않음
        }
        if (FMath::Abs(Current.x - Position.X) > 0.5f || FMath::Abs(Current.y - Position.Y) > 0.5f)
        {
            // 정확히 (0,0)은 "위치 없음"이므로 피한다
            Position = FVector2(Current.x, Current.x == 0.0f && Current.y == 0.0f ? 1.0f : Current.y);
            bMoved   = true;
        }
	};
	for (FMaterialGraphNode& Node : Graph.Nodes)
	{
		Sync(GetKey(Node.Id), Node.EditorPosition);
	}
	Sync(OutputKey, Graph.OutputEditorPosition);
	CommentNodeSizes.resize(Graph.Comments.size(), FVector2(-1.0f, -1.0f));
	for (size_t Index = 0; Index < Graph.Comments.size(); ++Index)
	{
		FMaterialGraphComment& Comment = Graph.Comments[Index];
		const uint32           Key     = CommentKeyBase + static_cast<uint32>(Index);
		Sync(Key, Comment.Position);
		// 크기: 노드 전체 크기의 변화만큼 그룹 크기를 바꾼다 (제목/여백 크기를 몰라도 정확)
		const ImVec2 Size = NodeEditor::GetNodeSize(NodeIdOf(Key));
		if (Size.x <= 0.0f)
		{
			continue;
		}
		FVector2& Last = CommentNodeSizes[Index];
		if (Last.X >= 0.0f && (FMath::Abs(Size.x - Last.X) > 0.5f || FMath::Abs(Size.y - Last.Y) > 0.5f))
		{
			Comment.Size = FVector2(FMath::Max(Comment.Size.X + Size.x - Last.X, 40.0f), FMath::Max(Comment.Size.Y + Size.y - Last.Y, 30.0f));
			bMoved       = true;
		}
		Last = FVector2(Size.x, Size.y);
	}
	if (bMoved && Context.OnEdited)
	{
		Context.OnEdited("노드 이동", false);
	}
}

void FMaterialGraphPanel::DrawComments(FContext& Context)
{
	FMaterialGraph& Graph = *Context.Graph;
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, ImColor(255, 255, 255, 18));
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, ImColor(255, 255, 255, 70));
	for (size_t Index = 0; Index < Graph.Comments.size(); ++Index)
	{
		const FMaterialGraphComment& Comment = Graph.Comments[Index];
		NodeEditor::BeginNode(NodeIdOf(CommentKeyBase + static_cast<uint32>(Index)));
		ImGui::PushID(static_cast<int>(Index));
		ImGui::TextUnformatted(Comment.Text.empty() ? "주석" : Comment.Text.c_str());
		NodeEditor::Group(ImVec2(Comment.Size.X, Comment.Size.Y));
		ImGui::PopID();
		NodeEditor::EndNode();
	}
	NodeEditor::PopStyleColor(2);
}

void FMaterialGraphPanel::DrawNode(FContext& Context, const FMaterialGraphNode& Node)
{
	const FMaterialGraphNodeInfo* Info     = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
	const uint32                  Key      = GetKey(Node.Id);
	const auto                    Severity = NodeSeverity.find(Node.Id);
	const int32                   Level    = Severity != NodeSeverity.end() ? Severity->second : 0;
	const ImVec4 Border = Level == 2 ? FEditorTheme::Danger : Level == 1 ? FEditorTheme::Warning : ImVec4(1.0f, 1.0f, 1.0f, 0.25f);

	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, ImColor(32, 33, 38, 240));
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, Border);
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, Level > 0 ? 2.5f : 1.0f);
	NodeEditor::BeginNode(NodeIdOf(Key));
	ImGui::PushID(static_cast<int>(Key));

	// 제목: 종류 · Id (+ 오류 아이콘)
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	if (Level > 0)
	{
		ImGui::TextColored(Level == 2 ? FEditorTheme::Danger : FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION);
		ImGui::SameLine();
	}
	ImGui::TextUnformatted(Info ? Node.Type.c_str() : ("? " + Node.Type).c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s", Node.Id.c_str());
	const float TitleBottom = ImGui::GetItemRectMax().y;
	ImGui::Dummy(ImVec2(MaterialGraphEditing::NodeWidth, 2.0f));
	const std::string Summary = MakeSummary(Context, Node);
	if (Info != nullptr && Info->Settings != MaterialNodeSetting_None)
	{
		ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "%s", Summary.c_str());
	}

	// 핀: 왼쪽 입력 / 오른쪽 출력
	const ImVec2 RowsTop = ImGui::GetCursorScreenPos();
	float        Bottom  = RowsTop.y;
	ImGui::BeginGroup();
	if (Info != nullptr)
	{
		for (uint32 Pin = 0; Pin < static_cast<uint32>(Info->Inputs.size()); ++Pin)
		{
			const FMaterialGraphPinInfo& PinInfo = Info->Inputs[Pin];
			const FMaterialGraphInput*   Input   = Node.FindInput(PinInfo.Name);
			const uint32                 Width   = Input ? GetSourceWidth(Context, *Input) : PinInfo.Default == EMaterialPinDefault::Constant ? PinInfo.ConstantWidth : 0;
			NodeEditor::BeginPin(InputPinOf(Key, Pin), NodeEditor::PinKind::Input);
			DrawPinIcon(GetWidthColor(Width), Input != nullptr && Input->IsLink());
			ImGui::SameLine();
			if (Input != nullptr && !Input->IsLink())
			{
				ImGui::Text("%s", PinInfo.Name.c_str());
				ImGui::SameLine(0.0f, 4.0f);
				ImGui::TextDisabled("%s", FormatValue(Input->Constant, Input->ConstantWidth).c_str());
			}
			else if (Input == nullptr && PinInfo.Default == EMaterialPinDefault::Required)
			{
				ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.55f, 1.0f), "%s", PinInfo.Name.c_str());
			}
			else
			{
				ImGui::TextUnformatted(PinInfo.Name.c_str());
			}
			NodeEditor::EndPin();
		}
	}
	ImGui::EndGroup();
	Bottom                = FMath::Max(Bottom, ImGui::GetItemRectMax().y);
	const float InputsEnd = ImGui::GetItemRectMax().x;

	if (Info != nullptr && !Info->Outputs.empty())
	{
		const std::vector<uint32>* Widths       = Context.Analysis ? Context.Analysis->FindOutputWidths(Node.Id) : nullptr;
		float                      OutputsWidth = 0.0f;
		for (const std::string& Name : Info->Outputs)
		{
			OutputsWidth = FMath::Max(OutputsWidth, ImGui::CalcTextSize(Name.c_str()).x);
		}
		OutputsWidth += PinIconSize + ImGui::GetStyle().ItemSpacing.x;
		const float Right = FMath::Max(InputsEnd + 24.0f + OutputsWidth, Start.x + MaterialGraphEditing::NodeWidth);
		ImGui::SetCursorScreenPos(ImVec2(Right - OutputsWidth, RowsTop.y));
		ImGui::BeginGroup();
		for (uint32 Output = 0; Output < static_cast<uint32>(Info->Outputs.size()); ++Output)
		{
			const std::string& Name      = Info->Outputs[Output];
			const float        LabelSize = Name.empty() ? 0.0f : ImGui::CalcTextSize(Name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetCursorScreenPos(ImVec2(Right - PinIconSize - LabelSize, ImGui::GetCursorScreenPos().y));
			NodeEditor::BeginPin(OutputPinOf(Key, Output), NodeEditor::PinKind::Output);
			if (!Name.empty())
			{
				ImGui::TextUnformatted(Name.c_str());
				ImGui::SameLine();
			}
			const uint32 Width = Widths != nullptr && Output < Widths->size() ? (*Widths)[Output] : 0;
			DrawPinIcon(GetWidthColor(Width), true);
			NodeEditor::EndPin();
		}
		ImGui::EndGroup();
		Bottom = FMath::Max(Bottom, ImGui::GetItemRectMax().y);
	}

	// 오류 메시지
	if (const auto Messages = NodeMessages.find(Node.Id); Messages != NodeMessages.end())
	{
		ImGui::SetCursorScreenPos(ImVec2(Start.x, Bottom + 4.0f));
		ImGui::PushTextWrapPos(Start.x + MaterialGraphEditing::NodeWidth + 60.0f);
		for (const std::string& Message : Messages->second)
		{
			ImGui::TextColored(Level == 2 ? FEditorTheme::Danger : FEditorTheme::Warning, "%s", Message.c_str());
		}
		ImGui::PopTextWrapPos();
	}

	ImGui::PopID();
	NodeEditor::EndNode();
	// 제목 띠 (범주 색)
	const ImVec2 NodeMin = ImGui::GetItemRectMin();
	const ImVec2 NodeMax = ImGui::GetItemRectMax();
	if (ImDrawList* Background = NodeEditor::GetNodeBackgroundDrawList(NodeIdOf(Key)))
	{
		const float Rounding = NodeEditor::GetStyle().NodeRounding;
		Background->AddRectFilled(ImVec2(NodeMin.x + 1.0f, NodeMin.y + 1.0f), ImVec2(NodeMax.x - 1.0f, TitleBottom + 4.0f),
		                          GetCategoryColor(Info ? Info->Category : std::string_view()), Rounding, ImDrawFlags_RoundCornersTop);
	}
	NodeEditor::PopStyleVar();
	NodeEditor::PopStyleColor(2);
}

void FMaterialGraphPanel::DrawOutputNode(FContext& Context)
{
	FMaterialGraph& Graph    = *Context.Graph;
	const auto      Severity = NodeSeverity.find(FMaterialGraphCompiler::OutputNodeId);
	const int32     Level    = Severity != NodeSeverity.end() ? Severity->second : 0;
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, ImColor(30, 32, 40, 245));
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, Level > 0 ? (Level == 2 ? FEditorTheme::Danger : FEditorTheme::Warning) : FEditorTheme::Accent);
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, 2.0f);
	NodeEditor::BeginNode(NodeIdOf(OutputKey));
	ImGui::PushID("MaterialOutput");
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	ImGui::TextUnformatted(ICON_FA_PALETTE " 머티리얼 출력");
	const float TitleBottom = ImGui::GetItemRectMax().y;
	ImGui::Dummy(ImVec2(MaterialGraphEditing::NodeWidth * 0.85f, 2.0f));
	const std::vector<FMaterialGraphPinInfo>& Pins = GetOutputPins();
	for (uint32 Pin = 0; Pin < static_cast<uint32>(Pins.size()); ++Pin)
	{
		const FMaterialGraphInput* Input = MaterialGraphEditing::FindInput(Graph, FMaterialGraphCompiler::OutputNodeId, Pins[Pin].Name);
		NodeEditor::BeginPin(InputPinOf(OutputKey, Pin), NodeEditor::PinKind::Input);
		DrawPinIcon(GetWidthColor(Input ? GetSourceWidth(Context, *Input) : Pins[Pin].ConstantWidth), Input != nullptr && Input->IsLink());
		ImGui::SameLine();
		if (Input == nullptr)
		{
			ImGui::TextDisabled("%s", Pins[Pin].Name.c_str());
		}
		else if (!Input->IsLink())
		{
			ImGui::Text("%s", Pins[Pin].Name.c_str());
			ImGui::SameLine(0.0f, 4.0f);
			ImGui::TextDisabled("%s", FormatValue(Input->Constant, Input->ConstantWidth).c_str());
		}
		else
		{
			ImGui::TextUnformatted(Pins[Pin].Name.c_str());
		}
		NodeEditor::EndPin();
	}
	if (const auto Messages = NodeMessages.find(FMaterialGraphCompiler::OutputNodeId); Messages != NodeMessages.end())
	{
		ImGui::PushTextWrapPos(Start.x + MaterialGraphEditing::NodeWidth + 60.0f);
		for (const std::string& Message : Messages->second)
		{
			ImGui::TextColored(Level == 2 ? FEditorTheme::Danger : FEditorTheme::Warning, "%s", Message.c_str());
		}
		ImGui::PopTextWrapPos();
	}
	ImGui::PopID();
	NodeEditor::EndNode();
	const ImVec2 NodeMin = ImGui::GetItemRectMin();
	const ImVec2 NodeMax = ImGui::GetItemRectMax();
	if (ImDrawList* Background = NodeEditor::GetNodeBackgroundDrawList(NodeIdOf(OutputKey)))
	{
		Background->AddRectFilled(ImVec2(NodeMin.x + 1.0f, NodeMin.y + 1.0f), ImVec2(NodeMax.x - 1.0f, TitleBottom + 4.0f), ToU32(ImVec4(0.0f, 0.3f, 0.6f, 1.0f)),
		                          NodeEditor::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop);
	}
	NodeEditor::PopStyleVar();
	NodeEditor::PopStyleColor(2);
}

void FMaterialGraphPanel::DrawLinks(FContext& Context)
{
	FMaterialGraph& Graph = *Context.Graph;
	const auto      Draw  = [&](uint32 ConsumerKey, uint32 Pin, const FMaterialGraphInput& Input) {
        if (!Input.IsLink() || Graph.FindNode(Input.Node) == nullptr)
        {
            return;
        }
        const uint32 Width = GetSourceWidth(Context, Input);
        NodeEditor::Link(LinkOf(ConsumerKey, Pin), OutputPinOf(GetKey(Input.Node), Input.Output), InputPinOf(ConsumerKey, Pin),
                         ImGui::ColorConvertU32ToFloat4(GetWidthColor(Width)), 2.0f);
	};
	for (const FMaterialGraphNode& Node : Graph.Nodes)
	{
		const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
		if (Info == nullptr)
		{
			continue;
		}
		for (uint32 Pin = 0; Pin < static_cast<uint32>(Info->Inputs.size()); ++Pin)
		{
			if (const FMaterialGraphInput* Input = Node.FindInput(Info->Inputs[Pin].Name))
			{
				Draw(GetKey(Node.Id), Pin, *Input);
			}
		}
	}
	const std::vector<FMaterialGraphPinInfo>& Pins = GetOutputPins();
	for (uint32 Pin = 0; Pin < static_cast<uint32>(Pins.size()); ++Pin)
	{
		if (const FMaterialGraphInput* Input = MaterialGraphEditing::FindInput(Graph, FMaterialGraphCompiler::OutputNodeId, Pins[Pin].Name))
		{
			Draw(OutputKey, Pin, *Input);
		}
	}
}

// ---------------------------------------------------------------- 편집

void FMaterialGraphPanel::Edit(FContext& Context, std::string_view Label, bool bSemantic, std::function<void()> Change)
{
	if (Context.bReadOnly)
	{
		return;
	}
	PendingChanges.push_back([&Context, Label = std::string(Label), bSemantic, Change = std::move(Change)]() {
		Change();
		if (Context.OnEdited)
		{
			Context.OnEdited(Label, bSemantic);
		}
	});
}

namespace
{
	// 핀 → (노드 Id, 핀 이름/출력 번호)
	struct FPinRef
	{
		std::string Node;
		std::string Pin;    // 입력 핀 이름
		uint32      Output = 0;
		bool        bInput = false;
	};
} // namespace

void FMaterialGraphPanel::HandleCreateAndDelete(FContext& Context)
{
	FMaterialGraph& Graph      = *Context.Graph;
	const auto      ResolvePin = [&](uintptr_t Raw, FPinRef& Out) {
        const uint32 Key = KeyOf(Raw);
        Out.Node         = FindNodeIdByKey(Key);
        if (Out.Node.empty())
        {
            return false;
        }
        if (IsOutputPin(Raw))
        {
            Out.bInput = false;
            Out.Output = static_cast<uint32>(LowOf(Raw) - OutputBase);
            return true;
        }
        if (!IsInputPin(Raw))
        {
            return false;
        }
        Out.bInput       = true;
        const uint32 Pin = static_cast<uint32>(LowOf(Raw) - InputBase);
        if (Key == OutputKey)
        {
            Out.Pin = Pin < GetOutputPins().size() ? GetOutputPins()[Pin].Name : std::string();
        }
        else if (const FMaterialGraphNode* Node = Graph.FindNode(Out.Node))
        {
            const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node->Type);
            Out.Pin                            = Info && Pin < Info->Inputs.size() ? Info->Inputs[Pin].Name : std::string();
        }
        return !Out.Pin.empty();
	};

	if (NodeEditor::BeginCreate(ImColor(255, 255, 255), 2.0f))
	{
		NodeEditor::PinId Start;
		NodeEditor::PinId End;
		if (NodeEditor::QueryNewLink(&Start, &End))
		{
			uintptr_t A = Start.Get();
			uintptr_t B = End.Get();
			if (IsInputPin(A) && IsOutputPin(B))
			{
				std::swap(A, B);
			}
			FPinRef    From;
			FPinRef    To;
			const bool bValid = !Context.bReadOnly && IsOutputPin(A) && IsInputPin(B) && ResolvePin(A, From) && ResolvePin(B, To) &&
			                    !MaterialGraphEditing::WouldCreateCycle(Graph, From.Node, To.Node);
			if (!bValid)
			{
				NodeEditor::RejectNewItem(ImColor(255, 90, 90), 2.0f);
			}
			else if (NodeEditor::AcceptNewItem(ImColor(120, 230, 120), 3.0f))
			{
				Edit(Context, "연결", true, [&Graph, From, To]() { MaterialGraphEditing::Connect(Graph, From.Node, From.Output, To.Node, To.Pin); });
			}
		}
		NodeEditor::PinId Dropped;
		if (NodeEditor::QueryNewNode(&Dropped))
		{
			if (Context.bReadOnly)
			{
				NodeEditor::RejectNewItem();
			}
			else if (NodeEditor::AcceptNewItem())
			{
				// 링크를 빈 곳에 놓음: 팔레트를 열고 새 노드에 자동 연결
				PalettePin   = Dropped.Get();
				bOpenPalette = true;
				const ImVec2 Canvas = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());
				PaletteSpawn        = FVector2(Canvas.x, Canvas.y);
			}
		}
	}
	NodeEditor::EndCreate();

	if (NodeEditor::BeginDelete())
	{
		std::vector<FPinRef>     Unlinked;
		std::vector<std::string> RemovedNodes;
		std::vector<uint32>      RemovedComments;
		NodeEditor::LinkId       Link;
		while (NodeEditor::QueryDeletedLink(&Link))
		{
			const uintptr_t Raw = Link.Get();
			FPinRef         To;
			if (!Context.bReadOnly && ResolvePin((static_cast<uintptr_t>(KeyOf(Raw)) << 8) | (InputBase + LowOf(Raw) - LinkBase), To) &&
			    NodeEditor::AcceptDeletedItem())
			{
				Unlinked.push_back(To);
			}
			else
			{
				NodeEditor::RejectDeletedItem();
			}
		}
		NodeEditor::NodeId Node;
		while (NodeEditor::QueryDeletedNode(&Node))
		{
			const uint32 Key = KeyOf(Node.Get());
			if (Context.bReadOnly || Key == OutputKey)
			{
				NodeEditor::RejectDeletedItem(); // 출력 노드는 지우지 않는다
			}
			else if (NodeEditor::AcceptDeletedItem(false))
			{
				if (Key >= CommentKeyBase)
				{
					RemovedComments.push_back(Key - CommentKeyBase);
				}
				else if (std::string Id = FindNodeIdByKey(Key); !Id.empty())
				{
					RemovedNodes.push_back(std::move(Id));
				}
			}
		}
		if (!Unlinked.empty() || !RemovedNodes.empty() || !RemovedComments.empty())
		{
			const bool bSemantic = !Unlinked.empty() || !RemovedNodes.empty();
			Edit(Context, RemovedNodes.empty() && RemovedComments.empty() ? "연결 끊기" : "삭제", bSemantic, [this, &Graph, Unlinked, RemovedNodes, RemovedComments]() {
				for (const FPinRef& Pin : Unlinked)
				{
					MaterialGraphEditing::Disconnect(Graph, Pin.Node, Pin.Pin);
				}
				MaterialGraphEditing::RemoveNodes(Graph, RemovedNodes);
				std::vector<uint32> Comments = RemovedComments;
				std::sort(Comments.rbegin(), Comments.rend());
				for (const uint32 Index : Comments)
				{
					if (Index < Graph.Comments.size())
					{
						Graph.Comments.erase(Graph.Comments.begin() + Index);
					}
				}
				if (!Comments.empty())
				{
					bApplyPositions = true; // 주석 번호가 밀린다
				}
			});
		}
	}
	NodeEditor::EndDelete();
}

void FMaterialGraphPanel::CopySelection(FContext& Context, bool bCut)
{
	std::vector<std::string> Ids;
	for (const std::string& Id : SelectedNodes)
	{
		if (Id != FMaterialGraphCompiler::OutputNodeId)
		{
			Ids.push_back(Id);
		}
	}
	const std::string Text = MaterialGraphEditing::CopyNodes(*Context.Graph, *Context.Parameters, Ids);
	if (Text.empty())
	{
		return;
	}
	ImGui::SetClipboardText(Text.c_str());
	if (bCut)
	{
		Edit(Context, "잘라내기", true, [&Context, Ids]() { MaterialGraphEditing::RemoveNodes(*Context.Graph, Ids); });
	}
}

void FMaterialGraphPanel::PasteAt(FContext& Context, const std::string& Text, const FVector2& CanvasPosition)
{
	if (!MaterialGraphEditing::IsClipboardText(Text))
	{
		return;
	}
	Edit(Context, "붙여넣기", true, [this, &Context, Text, CanvasPosition]() {
		const std::vector<std::string> NewIds = MaterialGraphEditing::PasteNodes(*Context.Graph, *Context.Parameters, Text, CanvasPosition);
		bApplyPositions                       = true;
		PendingSelection                      = NewIds;
	});
}

void FMaterialGraphPanel::HandleShortcuts(FContext& Context)
{
	if (!PendingSelection.empty())
	{
		NodeEditor::ClearSelection();
		for (const std::string& Id : PendingSelection)
		{
			NodeEditor::SelectNode(NodeIdOf(GetKey(Id)), true);
		}
		PendingSelection.clear();
	}
	if (!NodeEditor::BeginShortcut())
	{
		NodeEditor::EndShortcut();
		return;
	}
	const ImVec2   Mouse  = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());
	const FVector2 Canvas = FVector2(Mouse.x, Mouse.y);
	if (NodeEditor::AcceptCopy())
	{
		CopySelection(Context, false);
	}
	if (NodeEditor::AcceptCut())
	{
		CopySelection(Context, true);
	}
	if (NodeEditor::AcceptPaste())
	{
		if (const char* Clipboard = ImGui::GetClipboardText())
		{
			PasteAt(Context, Clipboard, Canvas);
		}
	}
	if (NodeEditor::AcceptDuplicate())
	{
		std::vector<std::string> Ids;
		FVector2                 Min(FLT_MAX, FLT_MAX);
		for (const std::string& Id : SelectedNodes)
		{
			if (const FMaterialGraphNode* Node = Context.Graph->FindNode(Id))
			{
				Ids.push_back(Id);
				Min = FVector2(FMath::Min(Min.X, Node->EditorPosition.X), FMath::Min(Min.Y, Node->EditorPosition.Y));
			}
		}
		const std::string Text = MaterialGraphEditing::CopyNodes(*Context.Graph, *Context.Parameters, Ids);
		if (!Text.empty())
		{
			PasteAt(Context, Text, Min + FVector2(40.0f, 40.0f));
		}
	}
	if (NodeEditor::AcceptCreateNode() && !Context.bReadOnly)
	{
		bOpenPalette = true; // 스페이스: 마우스 위치에 노드 추가
		PalettePin   = 0;
		PaletteSpawn = Canvas;
	}
	NodeEditor::EndShortcut();
}

void FMaterialGraphPanel::AddCommentAroundSelection(FContext& Context, const FVector2& FallbackPosition)
{
	FVector2 Min(FLT_MAX, FLT_MAX);
	FVector2 Max(-FLT_MAX, -FLT_MAX);
	for (const std::string& Id : SelectedNodes)
	{
		const NodeEditor::NodeId Node     = NodeIdOf(GetKey(Id));
		const ImVec2             Position = NodeEditor::GetNodePosition(Node);
		const ImVec2             Size     = NodeEditor::GetNodeSize(Node);
		if (Position.x == FLT_MAX)
		{
			continue;
		}
		Min = FVector2(FMath::Min(Min.X, Position.x), FMath::Min(Min.Y, Position.y));
		Max = FVector2(FMath::Max(Max.X, Position.x + Size.x), FMath::Max(Max.Y, Position.y + Size.y));
	}
	FMaterialGraphComment Comment;
	Comment.Text = "주석";
	if (Min.X <= Max.X)
	{
		Comment.Position = Min - FVector2(24.0f, 56.0f);
		Comment.Size     = Max - Min + FVector2(48.0f, 48.0f);
	}
	else
	{
		Comment.Position = FallbackPosition;
	}
	Edit(Context, "주석 추가", false, [this, &Context, Comment]() {
		Context.Graph->Comments.push_back(Comment);
		bApplyPositions = true;
	});
}

void FMaterialGraphPanel::DrawContextMenus(FContext& Context)
{
	FMaterialGraph& Graph = *Context.Graph;
	const ImVec2    Mouse = NodeEditor::ScreenToCanvas(ImGui::GetMousePos());
	NodeEditor::Suspend();
	NodeEditor::NodeId Node;
	NodeEditor::PinId  Pin;
	NodeEditor::LinkId Link;
	if (NodeEditor::ShowNodeContextMenu(&Node))
	{
		ContextNode = Node.Get();
		ImGui::OpenPopup("##MaterialNodeMenu");
	}
	else if (NodeEditor::ShowPinContextMenu(&Pin))
	{
		ContextPin = Pin.Get();
		ImGui::OpenPopup("##MaterialPinMenu");
	}
	else if (NodeEditor::ShowLinkContextMenu(&Link))
	{
		ContextLink = Link.Get();
		ImGui::OpenPopup("##MaterialLinkMenu");
	}
	else if (NodeEditor::ShowBackgroundContextMenu())
	{
		ContextCanvas = FVector2(Mouse.x, Mouse.y);
		ImGui::OpenPopup("##MaterialBackgroundMenu");
	}

	if (ImGui::BeginPopup("##MaterialNodeMenu"))
	{
		const uint32      Key = KeyOf(ContextNode);
		const std::string Id  = Key >= CommentKeyBase ? std::string() : FindNodeIdByKey(Key);
		const bool        bRegular = !Id.empty() && Id != FMaterialGraphCompiler::OutputNodeId;
		ImGui::BeginDisabled(Context.bReadOnly);
		if (bRegular && ImGui::MenuItem(ICON_FA_CLONE " 복제", "Ctrl+D"))
		{
			const FMaterialGraphNode* Source = Graph.FindNode(Id);
			const std::string         Text   = MaterialGraphEditing::CopyNodes(Graph, *Context.Parameters, { Id });
			if (Source != nullptr && !Text.empty())
			{
				PasteAt(Context, Text, Source->EditorPosition + FVector2(40.0f, 40.0f));
			}
		}
		if (bRegular && ImGui::MenuItem(ICON_FA_COPY " 복사", "Ctrl+C"))
		{
			ImGui::SetClipboardText(MaterialGraphEditing::CopyNodes(Graph, *Context.Parameters, { Id }).c_str());
		}
		if (!Id.empty() && ImGui::MenuItem(ICON_FA_LINK_SLASH " 입력 연결 모두 끊기"))
		{
			Edit(Context, "연결 끊기", true, [&Graph, Id]() {
				if (Id == FMaterialGraphCompiler::OutputNodeId)
				{
					std::erase_if(Graph.Outputs, [](const FMaterialGraphInput& Input) { return Input.IsLink(); });
				}
				else if (FMaterialGraphNode* Target = Graph.FindNode(Id))
				{
					std::erase_if(Target->Inputs, [](const FMaterialGraphInput& Input) { return Input.IsLink(); });
				}
			});
		}
		if ((bRegular || Key >= CommentKeyBase) && ImGui::MenuItem(ICON_FA_TRASH " 삭제", "Del"))
		{
			NodeEditor::DeleteNode(NodeEditor::NodeId(ContextNode)); // 다음 프레임 BeginDelete에서 처리
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("##MaterialPinMenu"))
	{
		ImGui::BeginDisabled(Context.bReadOnly);
		if (IsInputPin(ContextPin) && ImGui::MenuItem(ICON_FA_LINK_SLASH " 연결/상수 지우기"))
		{
			const uint32 Pin2 = static_cast<uint32>(LowOf(ContextPin) - InputBase);
			NodeEditor::DeleteLink(LinkOf(KeyOf(ContextPin), Pin2));
			// 상수는 링크가 없으므로 직접 지운다
			const std::string Node2 = FindNodeIdByKey(KeyOf(ContextPin));
			std::string       PinName;
			if (KeyOf(ContextPin) == OutputKey)
			{
				PinName = Pin2 < GetOutputPins().size() ? GetOutputPins()[Pin2].Name : std::string();
			}
			else if (const FMaterialGraphNode* Target = Graph.FindNode(Node2))
			{
				const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Target->Type);
				PinName                            = Info && Pin2 < Info->Inputs.size() ? Info->Inputs[Pin2].Name : std::string();
			}
			if (!PinName.empty())
			{
				Edit(Context, "연결 끊기", true, [&Graph, Node2, PinName]() { MaterialGraphEditing::Disconnect(Graph, Node2, PinName); });
			}
		}
		if (IsOutputPin(ContextPin) && ImGui::MenuItem(ICON_FA_LINK_SLASH " 이 출력의 연결 모두 끊기"))
		{
			const std::string Source = FindNodeIdByKey(KeyOf(ContextPin));
			const uint32      Output = static_cast<uint32>(LowOf(ContextPin) - OutputBase);
			Edit(Context, "연결 끊기", true, [&Graph, Source, Output]() {
				const auto Matches = [&](const FMaterialGraphInput& Input) { return Input.IsLink() && Input.Node == Source && Input.Output == Output; };
				for (FMaterialGraphNode& Target : Graph.Nodes)
				{
					std::erase_if(Target.Inputs, Matches);
				}
				std::erase_if(Graph.Outputs, Matches);
			});
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("##MaterialLinkMenu"))
	{
		ImGui::BeginDisabled(Context.bReadOnly);
		if (ImGui::MenuItem(ICON_FA_LINK_SLASH " 연결 끊기", "Del"))
		{
			NodeEditor::DeleteLink(NodeEditor::LinkId(ContextLink));
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("##MaterialBackgroundMenu"))
	{
		ImGui::BeginDisabled(Context.bReadOnly);
		if (ImGui::MenuItem(ICON_FA_PLUS " 노드 추가...", "Space"))
		{
			bOpenPalette = true;
			PalettePin   = 0;
			PaletteSpawn = ContextCanvas;
		}
		const char* Clipboard = ImGui::GetClipboardText();
		if (ImGui::MenuItem(ICON_FA_PASTE " 붙여넣기", "Ctrl+V", false, Clipboard != nullptr && MaterialGraphEditing::IsClipboardText(Clipboard)))
		{
			PasteAt(Context, Clipboard, ContextCanvas);
		}
		if (ImGui::MenuItem(ICON_FA_NOTE_STICKY " 주석 상자 (선택 노드 감싸기)"))
		{
			AddCommentAroundSelection(Context, ContextCanvas);
		}
		if (ImGui::MenuItem(ICON_FA_TABLE_COLUMNS " 자동 정렬"))
		{
			Edit(Context, "자동 정렬", false, [this, &Graph]() {
				MaterialGraphEditing::AutoLayout(Graph, false);
				bApplyPositions = true;
				NavigateFrames  = 1;
			});
		}
		ImGui::EndDisabled();
		if (ImGui::MenuItem(ICON_FA_EXPAND " 전체 보기", "F"))
		{
			NavigateFrames = 0;
		}
		ImGui::EndPopup();
	}
	NodeEditor::Resume();
}

void FMaterialGraphPanel::DrawPalette(FContext& Context)
{
	if (bOpenPalette)
	{
		bOpenPalette     = false;
		PaletteFilter[0] = '\0';
		ImGui::OpenPopup("##MaterialNodePalette");
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(340.0f, 480.0f));
	if (!ImGui::BeginPopup("##MaterialNodePalette"))
	{
		PalettePin = 0;
		return;
	}
	if (ImGui::IsWindowAppearing())
	{
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool bEnter = ImGui::InputTextWithHint("##Filter", ICON_FA_MAGNIFYING_GLASS " 노드 검색 (Enter = 첫 결과)", PaletteFilter, sizeof(PaletteFilter),
	                                             ImGuiInputTextFlags_EnterReturnsTrue);
	std::string Picked;
	const std::string_view Filter = PaletteFilter;
	const auto             Item   = [&](const FMaterialGraphNodeInfo& Info) {
        if (ImGui::Selectable(Info.Type.c_str()))
        {
            Picked = Info.Type;
        }
        if (ImGui::IsItemHovered())
        {
            std::string Pins;
            for (const FMaterialGraphPinInfo& Pin : Info.Inputs)
            {
                Pins += (Pins.empty() ? "" : ", ") + Pin.Name;
            }
            ImGui::SetTooltip("%s\n입력: %s\n출력 %zu개", Info.Type.c_str(), Pins.empty() ? "(없음)" : Pins.c_str(), Info.Outputs.size());
        }
	};
	if (ImGui::BeginChild("##PaletteList", ImVec2(0.0f, 360.0f)))
	{
		const std::vector<FMaterialGraphNodeInfo>& Infos = FMaterialGraphCompiler::GetNodeInfos();
		if (!Filter.empty())
		{
			for (const FMaterialGraphNodeInfo& Info : Infos)
			{
				if (ContainsNoCase(Info.Type, Filter) || ContainsNoCase(Info.Category, Filter))
				{
					if (bEnter && Picked.empty())
					{
						Picked = Info.Type;
					}
					Item(Info);
				}
			}
		}
		else
		{
			// 범주: 표에 처음 나온 순서
			std::vector<std::string> Categories;
			for (const FMaterialGraphNodeInfo& Info : Infos)
			{
				if (std::find(Categories.begin(), Categories.end(), Info.Category) == Categories.end())
				{
					Categories.push_back(Info.Category);
				}
			}
			for (const std::string& Category : Categories)
			{
				if (ImGui::TreeNodeEx(Category.empty() ? "기타" : Category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				{
					for (const FMaterialGraphNodeInfo& Info : Infos)
					{
						if (Info.Category == Category)
						{
							Item(Info);
						}
					}
					ImGui::TreePop();
				}
			}
		}
	}
	ImGui::EndChild();

	if (!Picked.empty() && !Context.bReadOnly)
	{
		const uintptr_t Pin      = PalettePin;
		const FVector2  Position = PaletteSpawn;
		PalettePin               = 0;
		ImGui::CloseCurrentPopup();
		// 팔레트는 그래프 밖에서 그리므로 바로 적용 (선택은 다음 프레임)
		FPinRef PinRef;
		bool    bHasPin = false;
		if (Pin != 0)
		{
			PinRef.Node   = FindNodeIdByKey(KeyOf(Pin));
			PinRef.bInput = IsInputPin(Pin);
			if (PinRef.bInput)
			{
				const uint32 Index = static_cast<uint32>(LowOf(Pin) - InputBase);
				if (KeyOf(Pin) == OutputKey)
				{
					PinRef.Pin = Index < GetOutputPins().size() ? GetOutputPins()[Index].Name : std::string();
				}
				else if (const FMaterialGraphNode* Target = Context.Graph->FindNode(PinRef.Node))
				{
					const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Target->Type);
					PinRef.Pin                         = Info && Index < Info->Inputs.size() ? Info->Inputs[Index].Name : std::string();
				}
			}
			else
			{
				PinRef.Output = static_cast<uint32>(LowOf(Pin) - OutputBase);
			}
			bHasPin = !PinRef.Node.empty() && (!PinRef.bInput || !PinRef.Pin.empty());
		}
		FMaterialGraphNode* Created = MaterialGraphEditing::AddNode(*Context.Graph, *Context.Parameters, Picked, Position);
		if (Created != nullptr)
		{
			const std::string             NewId = Created->Id;
			const FMaterialGraphNodeInfo* Info  = FMaterialGraphCompiler::FindNodeInfo(Picked);
			if (bHasPin && PinRef.bInput && Info != nullptr && !Info->Outputs.empty())
			{
				MaterialGraphEditing::Connect(*Context.Graph, NewId, 0, PinRef.Node, PinRef.Pin);
			}
			else if (bHasPin && !PinRef.bInput && Info != nullptr && !Info->Inputs.empty())
			{
				MaterialGraphEditing::Connect(*Context.Graph, PinRef.Node, PinRef.Output, NewId, Info->Inputs[0].Name);
			}
			bApplyPositions  = true;
			PendingSelection = { NewId };
			if (Context.OnEdited)
			{
				Context.OnEdited("노드 추가", true);
			}
		}
	}
	ImGui::EndPopup();
}

// ---------------------------------------------------------------- 상세 패널

bool FMaterialGraphPanel::DrawParameterValue(FMaterialParameter& Parameter, const char* Label, const std::vector<std::string>* TextureFiles,
                                             const std::filesystem::path& AssetDirectory, bool& bOutTexture, bool bEditUsage)
{
	bool bChanged = false;
	switch (Parameter.Type)
	{
	case EMaterialParameterType::Scalar:
		bChanged = ImGui::DragFloat(Label, &Parameter.Value.X, 0.01f);
		break;
	case EMaterialParameterType::Vector:
		bChanged = ImGui::ColorEdit4(Label, &Parameter.Value.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
		break;
	case EMaterialParameterType::StaticSwitch:
	{
		bool bValue = Parameter.GetBool();
		if (ImGui::Checkbox(Label, &bValue))
		{
			Parameter.Value.X = bValue ? 1.0f : 0.0f;
			bChanged          = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(정적 — 셰이더 변형)");
		break;
	}
	case EMaterialParameterType::Texture:
	{
		static const std::vector<std::string> NoFiles;
		if (FAssetEditorWidgets::TextureCombo(Label, Parameter.Texture, TextureFiles ? *TextureFiles : NoFiles, "(기본 텍스처)"))
		{
			bChanged = bOutTexture = true;
		}
		if (FAssetEditorWidgets::AcceptTextureDrop(Parameter.Texture, AssetDirectory))
		{
			bChanged = bOutTexture = true;
		}
		ImGui::SetItemTooltip("콘텐츠 브라우저에서 이미지를 끌어 놓을 수 있습니다");
		if (!bEditUsage)
		{
			ImGui::TextDisabled("용도: %s", GetTextureUsageName(Parameter.Usage));
			break;
		}
		static const ETextureUsage Usages[] = { ETextureUsage::Color, ETextureUsage::Linear, ETextureUsage::Normal, ETextureUsage::Mask };
		int32                      Current  = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Current = Usages[Index] == Parameter.Usage ? Index : Current;
		}
		static const char* const UsageLabels[] = { "Color (sRGB)", "Linear", "Normal (XY → Z)", "Mask (R)" };
		ImGui::PushID(Label);
		if (ImGui::Combo("용도", &Current, UsageLabels, IM_ARRAYSIZE(UsageLabels)))
		{
			Parameter.Usage = Usages[Current];
			bChanged = bOutTexture = true;
		}
		ImGui::PopID();
		break;
	}
	default: break;
	}
	return bChanged;
}

bool FMaterialGraphPanel::DrawSelectionDetails(FContext& Context)
{
	FMaterialGraph& Graph = *Context.Graph;
	if (SelectedComment >= 0 && SelectedComment < static_cast<int32>(Graph.Comments.size()))
	{
		ImGui::SeparatorText(ICON_FA_NOTE_STICKY " 주석 상자");
		ImGui::BeginDisabled(Context.bReadOnly);
		if (InputString("내용", Graph.Comments[static_cast<size_t>(SelectedComment)].Text) && Context.OnEdited)
		{
			Context.OnEdited("주석 내용", false);
		}
		ImGui::EndDisabled();
		return true;
	}
	if (SelectedNodes.empty())
	{
		return false;
	}
	if (SelectedNodes.size() > 1)
	{
		ImGui::Text("노드 %zu개 선택", SelectedNodes.size());
		FAssetEditorWidgets::Hint("Ctrl+C/V 복사·붙여넣기, Ctrl+D 복제, Delete 삭제, 배경 우클릭 → 주석 상자로 감싸기");
		return true;
	}
	const std::string& Id = SelectedNodes.front();
	ImGui::BeginDisabled(Context.bReadOnly);
	if (Id == FMaterialGraphCompiler::OutputNodeId)
	{
		ImGui::SeparatorText(ICON_FA_PALETTE " 머티리얼 출력");
		DrawPinInputs(Context, Id, GetOutputPins());
	}
	else if (FMaterialGraphNode* Node = Graph.FindNode(Id))
	{
		DrawNodeDetails(Context, *Node);
	}
	ImGui::EndDisabled();
	if (const auto Messages = NodeMessages.find(Id); Messages != NodeMessages.end())
	{
		ImGui::SeparatorText("오류");
		const bool bError = NodeSeverity[Id] == 2;
		ImGui::PushStyleColor(ImGuiCol_Text, bError ? FEditorTheme::Danger : FEditorTheme::Warning);
		for (const std::string& Message : Messages->second)
		{
			ImGui::TextWrapped("%s", Message.c_str());
		}
		ImGui::PopStyleColor();
		if (!bError)
		{
			FAssetEditorWidgets::Hint("출력에 연결되지 않은 노드라 셰이더에는 영향이 없습니다.");
		}
	}
	return true;
}

bool FMaterialGraphPanel::DrawNodeDetails(FContext& Context, FMaterialGraphNode& Node)
{
	const FMaterialGraphNodeInfo* Info = FMaterialGraphCompiler::FindNodeInfo(Node.Type);
	ImGui::SeparatorText(Node.Type.c_str());
	ImGui::TextDisabled("Id: %s%s%s", Node.Id.c_str(), Info ? " · " : "", Info ? Info->Category.c_str() : "");
	if (Info == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, "알 수 없는 노드 종류입니다");
		return false;
	}
	bool       bChanged = false;
	const auto Changed  = [&](std::string_view Label, bool bSemantic = true) {
        bChanged = true;
        if (Context.OnEdited)
        {
            Context.OnEdited(Label, bSemantic);
        }
	};

	if ((Info->Settings & MaterialNodeSetting_Value) != 0)
	{
		int32 Width = static_cast<int32>(std::clamp(Node.ValueWidth, 1u, 4u));
		if (ImGui::SliderInt("성분 수", &Width, 1, 4, "float%d"))
		{
			Node.ValueWidth = static_cast<uint32>(Width);
			Changed("상수 너비");
		}
		if (ImGui::DragScalarN("값", ImGuiDataType_Float, &Node.Value.X, Width, 0.01f))
		{
			Changed("상수 값");
		}
		if (Width >= 3 && ImGui::ColorEdit4("색", &Node.Value.X, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
		                                                             (Width == 3 ? ImGuiColorEditFlags_NoAlpha : 0)))
		{
			Changed("상수 값");
		}
	}
	if ((Info->Settings & MaterialNodeSetting_Tiling) != 0)
	{
		if (ImGui::DragFloat2("타일링", &Node.Value.X, 0.01f))
		{
			Node.ValueWidth = 2;
			Changed("UV 타일링");
		}
		if (Node.Index != 0)
		{
			ImGui::TextColored(FEditorTheme::Warning, "UV 채널 %d (채널 0만 있음)", Node.Index);
		}
	}
	if ((Info->Settings & MaterialNodeSetting_Parameter) != 0)
	{
		// 파라미터 고르기 (같은 타입) + 새로 만들기
		if (ImGui::BeginCombo("파라미터", Node.Name.empty() ? "(없음)" : Node.Name.c_str()))
		{
			for (const FMaterialParameter& Parameter : *Context.Parameters)
			{
				if (Parameter.Type == Info->ParameterType && ImGui::Selectable(Parameter.Name.c_str(), Parameter.Name == Node.Name))
				{
					Node.Name = Parameter.Name;
					Changed("파라미터 변경");
				}
			}
			ImGui::Separator();
			if (ImGui::Selectable(ICON_FA_PLUS " 새 파라미터"))
			{
				FMaterialParameter Parameter;
				Parameter.Name  = MaterialGraphEditing::MakeUniqueParameterName(*Context.Parameters, GetMaterialParameterTypeName(Info->ParameterType));
				Parameter.Type  = Info->ParameterType;
				Parameter.Value = Info->ParameterType == EMaterialParameterType::Vector ? FVector4::OneVector : FVector4(1.0f, 0.0f, 0.0f, 0.0f);
				Node.Name       = Parameter.Name;
				Context.Parameters->push_back(std::move(Parameter));
				Changed("새 파라미터");
			}
			ImGui::EndCombo();
		}
		FMaterialParameter* Parameter = nullptr;
		for (FMaterialParameter& Item : *Context.Parameters)
		{
			Parameter = Item.Name == Node.Name ? &Item : Parameter;
		}
		if (Parameter != nullptr)
		{
			// 이름 바꾸기 (입력이 끝날 때 적용 — 이 이름을 쓰는 노드도 함께)
			const std::string Owner     = Node.Id;
			const bool        bEditing  = ParameterNameEditOwner == Owner;
			std::string       NameField = bEditing ? ParameterNameEdit : Parameter->Name;
			if (InputString("이름", NameField))
			{
				ParameterNameEdit      = NameField;
				ParameterNameEditOwner = Owner;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && ParameterNameEditOwner == Owner)
			{
				const std::string OldName = Parameter->Name;
				if (MaterialGraphEditing::RenameParameter(*Context.Graph, *Context.Parameters, OldName, ParameterNameEdit))
				{
					Changed("파라미터 이름");
				}
				ParameterNameEditOwner.clear();
			}
			bool bTexture = false;
			for (FMaterialParameter& Item : *Context.Parameters)
			{
				if (Item.Name == Node.Name && DrawParameterValue(Item, "값", Context.TextureFiles, Context.AssetDirectory, bTexture))
				{
					Changed(bTexture ? "텍스처 변경" : "파라미터 값");
				}
			}
		}
		else if (!Node.Name.empty())
		{
			ImGui::TextColored(FEditorTheme::Danger, "파라미터 %s가 없습니다", Node.Name.c_str());
		}
	}
	if ((Info->Settings & MaterialNodeSetting_Sampler) != 0)
	{
		int32 Sampler = Node.Option == "Clamp" ? 1 : 0;
		if (ImGui::Combo("샘플러", &Sampler, SamplerNames, IM_ARRAYSIZE(SamplerNames)))
		{
			Node.Option = SamplerNames[Sampler];
			Changed("샘플러");
		}
	}
	if ((Info->Settings & MaterialNodeSetting_Channels) != 0)
	{
		std::string Channels = Node.Option;
		if (InputString("채널", Channels, ImGuiInputTextFlags_CharsNoBlank))
		{
			std::erase_if(Channels, [](char Char) { return std::string_view("xyzwrgba").find(Char) == std::string_view::npos; });
			Node.Option = Channels.substr(0, 4);
			Changed("채널");
		}
		ImGui::SetItemTooltip("xyzw 또는 rgba 1~4글자 (순서 바꾸기·반복 가능, 예: xy, zw, bgr)");
	}
	if ((Info->Settings & MaterialNodeSetting_CompareOp) != 0)
	{
		int32 Op = 0;
		for (int32 Index = 0; Index < IM_ARRAYSIZE(CompareNames); ++Index)
		{
			Op = Node.Option == CompareNames[Index] ? Index : Op;
		}
		if (ImGui::Combo("연산", &Op, CompareNames, IM_ARRAYSIZE(CompareNames)))
		{
			Node.Option = CompareNames[Op];
			Changed("비교 연산");
		}
	}
	if (!Info->Inputs.empty())
	{
		ImGui::SeparatorText("입력");
		bChanged |= DrawPinInputs(Context, Node.Id, Info->Inputs);
	}
	return bChanged;
}

bool FMaterialGraphPanel::DrawPinInputs(FContext& Context, const std::string& NodeId, const std::vector<FMaterialGraphPinInfo>& Pins)
{
	FMaterialGraph& Graph    = *Context.Graph;
	bool            bChanged = false;
	for (const FMaterialGraphPinInfo& Pin : Pins)
	{
		ImGui::PushID(Pin.Name.c_str());
		const FMaterialGraphInput* Input = MaterialGraphEditing::FindInput(Graph, NodeId, Pin.Name);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Pin.Name.c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.32f);
		if (Input != nullptr && Input->IsLink())
		{
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(GetWidthColor(GetSourceWidth(Context, *Input))), ICON_FA_ARROW_LEFT " %s%s", Input->Node.c_str(),
			                   Input->Output != 0 ? std::format(":{}", Input->Output).c_str() : "");
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_LINK_SLASH "##Unlink"))
			{
				MaterialGraphEditing::Disconnect(Graph, NodeId, Pin.Name);
				bChanged = true;
				Context.OnEdited("연결 끊기", true);
			}
			ImGui::SetItemTooltip("연결 끊기");
		}
		else
		{
			bool bConstant = Input != nullptr;
			if (ImGui::Checkbox("##Constant", &bConstant))
			{
				if (bConstant)
				{
					const bool bPinConstant = Pin.Default == EMaterialPinDefault::Constant;
					MaterialGraphEditing::SetConstant(Graph, NodeId, Pin.Name, bPinConstant ? Pin.Constant : FVector4::ZeroVector,
					                                  bPinConstant ? Pin.ConstantWidth : 1);
				}
				else
				{
					MaterialGraphEditing::Disconnect(Graph, NodeId, Pin.Name);
				}
				bChanged = true;
				Context.OnEdited("핀 상수", true);
			}
			ImGui::SetItemTooltip("상수 지정 (끄면 핀 기본값)");
			ImGui::SameLine();
			Input = MaterialGraphEditing::FindInput(Graph, NodeId, Pin.Name);
			if (Input != nullptr)
			{
				FVector4 Value = Input->Constant;
				int32    Width = static_cast<int32>(std::clamp(Input->ConstantWidth, 1u, 4u));
				ImGui::SetNextItemWidth(56.0f);
				bool bEdited = ImGui::SliderInt("##Width", &Width, 1, 4, "f%d");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-FLT_MIN);
				bEdited |= ImGui::DragScalarN("##Value", ImGuiDataType_Float, &Value.X, Width, 0.01f);
				if (bEdited)
				{
					MaterialGraphEditing::SetConstant(Graph, NodeId, Pin.Name, Value, static_cast<uint32>(Width));
					bChanged = true;
					Context.OnEdited("핀 상수", true);
				}
			}
			else
			{
				ImGui::TextDisabled("%s", DescribeDefault(Pin).c_str());
			}
		}
		ImGui::PopID();
	}
	return bChanged;
}
