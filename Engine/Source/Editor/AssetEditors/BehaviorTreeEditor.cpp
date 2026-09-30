#include <imgui.h>

#pragma warning(push, 0)
#include <imgui_node_editor.h>
#pragma warning(pop)

#include "Editor/AssetEditors/BehaviorTreeEditor.h"

#include "AI/AIComponents.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ModelLoader.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <format>
#include <type_traits>
#include <variant>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace NodeEditor = ax::NodeEditor;

namespace
{
	constexpr float NodeWidth      = 190.0f;
	constexpr float LayoutSpacingX = NodeWidth + 20.0f;
	constexpr float LayoutSpacingY = 150.0f;

	// 그래프 ID: 노드 ID << 2 | 종류 (0 노드, 1 입력 핀, 2 출력 핀, 3 부모 → 이 노드 링크)
	NodeEditor::NodeId ToGraphNode(uint32 Id) { return NodeEditor::NodeId(static_cast<uintptr_t>(Id) << 2); }
	NodeEditor::PinId  ToInputPin(uint32 Id) { return NodeEditor::PinId((static_cast<uintptr_t>(Id) << 2) | 1); }
	NodeEditor::PinId  ToOutputPin(uint32 Id) { return NodeEditor::PinId((static_cast<uintptr_t>(Id) << 2) | 2); }
	NodeEditor::LinkId ToLink(uint32 ChildId) { return NodeEditor::LinkId((static_cast<uintptr_t>(ChildId) << 2) | 3); }
	uint32             DecodeId(uintptr_t Raw) { return static_cast<uint32>(Raw >> 2); }
	uintptr_t          DecodeKind(uintptr_t Raw) { return Raw & 3; }

	ImU32 ToU32(const ImVec4& Color) { return ImGui::ColorConvertFloat4ToU32(Color); }

	const FBTNodeInfo* FindInfo(const std::string& Type) { return FBehaviorTreeNodeRegistry::Get().Find(Type); }

	bool IsComposite(const FBTNodeDesc& Node)
	{
		const FBTNodeInfo* Info = FindInfo(Node.Type);
		return Info != nullptr && Info->Category == EBTNodeCategory::Composite;
	}

	bool Contains(const std::vector<uint32>& Ids, uint32 Id) { return std::find(Ids.begin(), Ids.end(), Id) != Ids.end(); }

	// 컴포지트/태스크 노드만 (데코레이터/서비스 제외), 부모 → 자식 순서
	template <typename TNode, typename TFunc>
	void ForEachGraphNode(TNode& Node, TFunc&& Func)
	{
		Func(Node);
		for (auto& Child : Node.Children)
		{
			ForEachGraphNode(Child, Func);
		}
	}

	FBTNodeDesc* FindParent(FBTNodeDesc& Node, uint32 ChildId)
	{
		for (FBTNodeDesc& Child : Node.Children)
		{
			if (Child.Id == ChildId)
			{
				return &Node;
			}
			if (FBTNodeDesc* Found = FindParent(Child, ChildId))
			{
				return Found;
			}
		}
		return nullptr;
	}

	bool SubtreeContains(const FBTNodeDesc& Node, uint32 Id)
	{
		if (Node.Id == Id)
		{
			return true;
		}
		return std::any_of(Node.Children.begin(), Node.Children.end(), [Id](const FBTNodeDesc& Child) { return SubtreeContains(Child, Id); });
	}

	// 자식 실행 순서 = X 좌표 순서 (위치 없는 노드는 제자리). 순서가 바뀌었으면 true
	bool SortChildrenByX(FBTNodeDesc& Node)
	{
		std::vector<uint32> Before;
		for (const FBTNodeDesc& Child : Node.Children)
		{
			Before.push_back(Child.Id);
		}
		std::stable_sort(Node.Children.begin(), Node.Children.end(), [](const FBTNodeDesc& A, const FBTNodeDesc& B) {
			const float AX = A.EditorPosition ? A.EditorPosition->X : 0.0f;
			const float BX = B.EditorPosition ? B.EditorPosition->X : 0.0f;
			return AX < BX;
		});
		for (size_t Index = 0; Index < Node.Children.size(); ++Index)
		{
			if (Node.Children[Index].Id != Before[Index])
			{
				return true;
			}
		}
		return false;
	}

	std::string ParamToText(const FBTParamValue& Value)
	{
		return std::visit(
			[](const auto& Item) -> std::string {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::is_same_v<T, bool>) return Item ? "true" : "false";
				else if constexpr (std::is_same_v<T, int32>) return std::to_string(Item);
				else if constexpr (std::is_same_v<T, float>) return std::format("{:g}", Item);
				else if constexpr (std::is_same_v<T, FVector3>) return std::format("({:g}, {:g}, {:g})", Item.X, Item.Y, Item.Z);
				else return Item;
			},
			Value);
	}

	std::string BlackboardValueToText(const FBlackboardValue& Value)
	{
		return std::visit(
			[](const auto& Item) -> std::string {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::is_same_v<T, bool>) return Item ? "true" : "false";
				else if constexpr (std::is_same_v<T, int32>) return std::to_string(Item);
				else if constexpr (std::is_same_v<T, float>) return std::format("{:.2f}", Item);
				else if constexpr (std::is_same_v<T, FVector3>) return std::format("({:.0f}, {:.0f}, {:.0f})", Item.X, Item.Y, Item.Z);
				else if constexpr (std::is_same_v<T, FEntity>) return std::format("엔티티 #{}", Item.Index);
				else return "\"" + Item + "\"";
			},
			Value);
	}

	// 노드 줄에 보일 요약: 에셋에 직접 적힌 파라미터 (최대 3개)
	std::string MakeSummary(const FBTNodeDesc& Node)
	{
		std::string Summary;
		int32       Count = 0;
		for (const FBTParam& Param : Node.Params)
		{
			if (Param.Name == "Properties" || Count >= 3)
			{
				continue;
			}
			Summary += (Summary.empty() ? "" : ", ") + Param.Name + "=" + ParamToText(Param.Value);
			++Count;
		}
		return Summary;
	}

	std::string DisplayNameOf(const FBTNodeDesc& Node)
	{
		const FBTNodeInfo* Info = FindInfo(Node.Type);
		return Info ? Info->DisplayName : ("? " + Node.Type);
	}

	bool InputString(const char* Label, std::string& Value, bool bMultiline = false)
	{
		char Buffer[1024];
		strncpy_s(Buffer, Value.c_str(), _TRUNCATE);
		const bool bChanged = bMultiline ? ImGui::InputTextMultiline(Label, Buffer, sizeof(Buffer), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f))
		                                 : ImGui::InputText(Label, Buffer, sizeof(Buffer));
		if (bChanged)
		{
			Value = Buffer;
		}
		return bChanged;
	}

	constexpr const char* KeyTypeNames[] = { "Bool", "Int", "Float", "Vector", "Entity", "String" };
} // namespace

void FBehaviorTreeEditor::FGraphDeleter::operator()(NodeEditor::EditorContext* Context) const
{
	NodeEditor::DestroyEditor(Context);
}

FBehaviorTreeEditor::FBehaviorTreeEditor(std::filesystem::path InPath)
	: FAssetEditor(std::move(InPath))
{
	NodeEditor::Config Config;
	Config.SettingsFile = nullptr; // 노드 위치는 에셋(EditorPosition)에 저장한다
	Graph.reset(NodeEditor::CreateEditor(&Config));
}

FBehaviorTreeEditor::~FBehaviorTreeEditor() = default;

FBehaviorTreeAsset FBehaviorTreeEditor::MakeDefaultAsset()
{
	FBehaviorTreeAsset Asset;
	FBTNodeDesc        Root;
	Root.Type           = "Selector";
	Root.Id             = 1;
	Root.EditorPosition = FVector2(0.0f, 0.0f);
	Asset.Root          = std::move(Root);
	return Asset;
}

// ---------------------------------------------------------------- 에셋 상태

bool FBehaviorTreeEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	FBehaviorTreeAsset Loaded;
	std::string        Error;
	if (!Loaded.LoadFromFile(Path, &Error))
	{
		E_LOG(LogEditor, Error, "비헤이비어 트리를 읽지 못했습니다: {} — {}", GetDisplayName(), Error);
		if (Env.Editor && Env.Editor->Notify)
		{
			Env.Editor->Notify("비헤이비어 트리를 읽지 못했습니다: " + Error, true);
		}
		return false;
	}
	Asset = std::move(Loaded);
	Asset.AssignMissingNodeIds();
	bApplyPositions = true;
	CenterFrames    = 2;
	if (FindSelectedDesc() == nullptr)
	{
		Selection = {};
	}
	return true;
}

bool FBehaviorTreeEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return Asset.SaveToFile(Path); // 플레이 중이면 파일 감시가 이 에셋을 쓰는 트리를 다시 시작한다
}

std::string FBehaviorTreeEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FBehaviorTreeEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FBehaviorTreeAsset Restored;
	if (Restored.FromJsonString(State))
	{
		Asset           = std::move(Restored);
		bApplyPositions = true;
		if (FindSelectedDesc() == nullptr)
		{
			Selection = {};
		}
	}
}

void FBehaviorTreeEditor::FramePreview(FAssetEditorEnvironment& Env)
{
	(void)Env;
	CenterFrames = 2; // Open 직후 호출된다
}

FBTNodeDesc* FBehaviorTreeEditor::FindSelectedDesc()
{
	FBTNodeDesc* Node = Selection.NodeId != 0 ? Asset.FindNode(Selection.NodeId) : nullptr;
	if (Node == nullptr || Selection.Kind == ESelectionKind::Node)
	{
		return Node;
	}
	std::vector<FBTNodeDesc>& List = Selection.Kind == ESelectionKind::Decorator ? Node->Decorators : Node->Services;
	return Selection.Index >= 0 && Selection.Index < static_cast<int32>(List.size()) ? &List[static_cast<size_t>(Selection.Index)] : nullptr;
}

FBTNodeDesc FBehaviorTreeEditor::MakeNode(const std::string& Type)
{
	FBTNodeDesc Node;
	Node.Type = Type;
	Node.Id   = Asset.GetNextNodeId();
	return Node;
}

void FBehaviorTreeEditor::AutoLayoutMissing()
{
	if (!Asset.Root)
	{
		return;
	}
	// 잎은 왼쪽부터 차례로, 부모는 자식들 가운데, 깊이마다 아래로 (위치가 없는 노드에만 적용)
	float      NextLeafX = 0.0f;
	const auto Layout    = [&](auto& Self, FBTNodeDesc& Node, int32 Depth) -> float {
		float X = 0.0f;
		if (Node.Children.empty())
		{
			X = NextLeafX;
			NextLeafX += LayoutSpacingX;
		}
		else
		{
			float Sum = 0.0f;
			for (FBTNodeDesc& Child : Node.Children)
			{
				Sum += Self(Self, Child, Depth + 1);
			}
			X = Sum / static_cast<float>(Node.Children.size());
		}
		if (!Node.EditorPosition)
		{
			Node.EditorPosition = FVector2(X, static_cast<float>(Depth) * LayoutSpacingY);
		}
		return Node.EditorPosition->X;
	};
	Layout(Layout, *Asset.Root, 0);
}

const FBehaviorTreeInstance* FBehaviorTreeEditor::FindDebugTree(FAssetEditorEnvironment& Env, std::string* OutEntityName) const
{
	FEditorContext* Context = Env.Editor;
	if (Context == nullptr || !Context->bPlaying || Context->AI == nullptr || Context->Scene == nullptr)
	{
		return nullptr;
	}
	const std::string AssetPath = FModelLoader::MakeAssetPath(Path);
	FRegistry&        Registry  = Context->Scene->GetRegistry();
	const auto        TryEntity = [&](FEntity Entity) -> const FBehaviorTreeInstance* {
		const FBehaviorTreeComponent* Component = Registry.IsValid(Entity) ? Registry.TryGet<FBehaviorTreeComponent>(Entity) : nullptr;
		const FBehaviorTreeInstance*  Tree      = Component && Component->Asset == AssetPath ? Context->AI->FindTree(Entity) : nullptr;
		if (Tree != nullptr && OutEntityName != nullptr)
		{
			const FNameComponent* Name = Registry.TryGet<FNameComponent>(Entity);
			*OutEntityName             = Name ? Name->Name : std::string("?");
		}
		return Tree;
	};
	// 선택한 엔티티가 이 트리를 쓰면 그것, 아니면 처음 찾은 엔티티
	if (const FBehaviorTreeInstance* Selected = TryEntity(Context->SelectedEntity))
	{
		return Selected;
	}
	const FBehaviorTreeInstance* Found = nullptr;
	Registry.View<FBehaviorTreeComponent>().Each([&](FEntity Entity, FBehaviorTreeComponent&) {
		if (Found == nullptr)
		{
			Found = TryEntity(Entity);
		}
	});
	return Found;
}

// ---------------------------------------------------------------- 그래프

void FBehaviorTreeEditor::DrawMainPanel(FAssetEditorEnvironment& Env)
{
	std::string                  DebugEntity;
	const FBehaviorTreeInstance* DebugTree = FindDebugTree(Env, &DebugEntity);
	const std::vector<uint32>    ActiveIds = DebugTree ? DebugTree->GetActiveNodeIds() : std::vector<uint32>{};

	if (ImGui::SmallButton(ICON_FA_EXPAND " 전체 보기 (F)"))
	{
		NavigateFrames = 0;
	}
	ImGui::SameLine();
	if (DebugTree != nullptr)
	{
		ImGui::TextColored(FEditorTheme::Success, ICON_FA_PLAY " 디버그: %s", DebugEntity.c_str());
	}
	else if (Env.Editor && Env.Editor->bPlaying)
	{
		ImGui::TextDisabled("플레이 중 — 이 트리를 쓰는 엔티티가 없습니다");
	}
	else
	{
		ImGui::TextDisabled("우클릭: 노드 추가 · 출력 핀을 끌어 부모 변경 · Delete: 삭제 · 옆으로 옮기면 실행 순서가 바뀝니다");
	}
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
	    !ImGui::GetIO().KeyCtrl)
	{
		NavigateFrames = 0;
	}

	NodeEditor::SetCurrentEditor(Graph.get());
	NodeEditor::Begin("##BehaviorTreeGraph", ImVec2(0.0f, 0.0f));
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_SourceDirection, ImVec2(0.0f, 1.0f)); // 링크는 아래로 나가고
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_TargetDirection, ImVec2(0.0f, -1.0f)); // 위에서 들어온다

	const bool bAppliedPositions = bApplyPositions;
	bSubItemClicked              = false;
	if (Asset.Root)
	{
		if (bApplyPositions)
		{
			AutoLayoutMissing();
			ForEachGraphNode(*Asset.Root, [](FBTNodeDesc& Node) {
				NodeEditor::SetNodePosition(ToGraphNode(Node.Id), ImVec2(Node.EditorPosition->X, Node.EditorPosition->Y));
			});
			bApplyPositions = false;
		}
		DrawGraphNode(*Asset.Root, -1, ActiveIds);
		DrawGraphLinks(*Asset.Root, ActiveIds);
	}
	HandleGraphEdits();
	DrawGraphMenus();
	NodeEditor::PopStyleVar(2);

	// 그래프 선택 → 속성 선택 (그래프 쪽 선택이 바뀔 때만 따라가 데코레이터/서비스 선택을 덮어쓰지 않는다)
	NodeEditor::NodeId Selected[2];
	const int32        SelectedCount = NodeEditor::GetSelectedNodes(Selected, 2);
	const uint32       GraphSelection = SelectedCount == 1 ? DecodeId(Selected[0].Get()) : 0;
	if (GraphSelection != LastGraphSelection && !bSubItemClicked)
	{
		Selection = GraphSelection != 0 ? FSelection{ GraphSelection, ESelectionKind::Node, -1 } : FSelection{};
	}
	LastGraphSelection = GraphSelection;

	if (NavigateFrames >= 0 && NavigateFrames-- == 0)
	{
		NodeEditor::NavigateToContent(0.0f);
	}
	// 열 때: 큰 트리 전체로 축소하지 않고 루트 + 첫 단계 자식에 맞춘다 (루트뿐이면 확대하지 않고 가운데만). 선택은 되돌린다
	if (CenterFrames >= 0 && CenterFrames-- == 0 && Asset.Root)
	{
		NodeEditor::SelectNode(ToGraphNode(Asset.Root->Id));
		for (const FBTNodeDesc& Child : Asset.Root->Children)
		{
			NodeEditor::SelectNode(ToGraphNode(Child.Id), true);
		}
		NodeEditor::NavigateToSelection(!Asset.Root->Children.empty(), 0.0f);
		NodeEditor::ClearSelection();
	}
	NodeEditor::End();
	if (!bAppliedPositions)
	{
		SyncPositionsFromGraph();
	}
	NodeEditor::SetCurrentEditor(nullptr);

	if (PendingChange)
	{
		const std::function<void()> Change = std::move(PendingChange);
		PendingChange                      = nullptr;
		Change();
	}
	PreviousActiveIds = ActiveIds;
}

void FBehaviorTreeEditor::DrawGraphNode(FBTNodeDesc& Node, int32 ChildOrder, const std::vector<uint32>& ActiveIds)
{
	const FBTNodeInfo* Info       = FindInfo(Node.Type);
	const bool         bComposite = Info != nullptr && Info->Category == EBTNodeCategory::Composite;
	const bool         bRoot      = Asset.Root && &Node == &*Asset.Root;
	const bool         bActive    = Contains(ActiveIds, Node.Id);

	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, bComposite ? ImColor(44, 52, 68, 235) : ImColor(58, 44, 70, 235));
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, bActive ? ImColor(ToU32(FEditorTheme::Success)) : ImColor(255, 255, 255, 60));
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, bActive ? 3.0f : 1.0f);
	NodeEditor::BeginNode(ToGraphNode(Node.Id));
	ImGui::PushID(static_cast<int>(Node.Id));

	if (!bRoot)
	{
		NodeEditor::BeginPin(ToInputPin(Node.Id), NodeEditor::PinKind::Input);
		NodeEditor::PinPivotAlignment(ImVec2(0.5f, 0.0f));
		ImGui::Dummy(ImVec2(NodeWidth, 4.0f));
		NodeEditor::EndPin();
	}

	// 노드 안 데코레이터/서비스 줄: 클릭하면 속성 패널이 그 항목을 보인다
	const auto AttachmentRow = [&](const FBTNodeDesc& Attachment, ESelectionKind Kind, int32 Index, const ImVec4& Color, const char* Icon) {
		const bool bSelected = Selection.NodeId == Node.Id && Selection.Kind == Kind && Selection.Index == Index;
		const bool bLive     = Contains(ActiveIds, Attachment.Id);
		ImGui::PushStyleColor(ImGuiCol_Text, bLive ? FEditorTheme::Success : Color);
		ImGui::Text("%s %s%s", Icon, DisplayNameOf(Attachment).c_str(), bSelected ? "  " ICON_FA_CARET_LEFT : "");
		ImGui::PopStyleColor();
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			Selection       = { Node.Id, Kind, Index };
			bSubItemClicked = true;
		}
		const std::string Summary = MakeSummary(Attachment);
		if (!Summary.empty())
		{
			ImGui::TextDisabled("   %s", Summary.c_str());
		}
	};
	for (size_t Index = 0; Index < Node.Decorators.size(); ++Index)
	{
		AttachmentRow(Node.Decorators[Index], ESelectionKind::Decorator, static_cast<int32>(Index), ImVec4(0.55f, 0.72f, 1.0f, 1.0f), ICON_FA_DIAMOND);
	}

	// 제목: 실행 순서 번호 + 분류 아이콘 + 이름
	const char* Icon = bComposite ? ICON_FA_SITEMAP : ICON_FA_BOLT;
	if (ChildOrder >= 0)
	{
		ImGui::TextDisabled("%d", ChildOrder + 1);
		ImGui::SameLine();
	}
	if (Info == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 알 수 없는 노드: %s", Node.Type.c_str());
	}
	else
	{
		ImGui::Text("%s %s", Icon, Info->DisplayName.c_str());
	}
	const std::string Summary = MakeSummary(Node);
	if (!Summary.empty())
	{
		ImGui::TextDisabled("%s", Summary.c_str());
	}

	for (size_t Index = 0; Index < Node.Services.size(); ++Index)
	{
		AttachmentRow(Node.Services[Index], ESelectionKind::Service, static_cast<int32>(Index), ImVec4(0.5f, 0.88f, 0.62f, 1.0f), ICON_FA_GEARS);
	}

	if (bComposite)
	{
		NodeEditor::BeginPin(ToOutputPin(Node.Id), NodeEditor::PinKind::Output);
		NodeEditor::PinPivotAlignment(ImVec2(0.5f, 1.0f));
		ImGui::Dummy(ImVec2(NodeWidth, 4.0f));
		NodeEditor::EndPin();
	}

	ImGui::PopID();
	NodeEditor::EndNode();
	NodeEditor::PopStyleVar();
	NodeEditor::PopStyleColor(2);

	for (size_t Index = 0; Index < Node.Children.size(); ++Index)
	{
		DrawGraphNode(Node.Children[Index], static_cast<int32>(Index), ActiveIds);
	}
}

void FBehaviorTreeEditor::DrawGraphLinks(const FBTNodeDesc& Node, const std::vector<uint32>& ActiveIds)
{
	for (const FBTNodeDesc& Child : Node.Children)
	{
		const bool bActive = Contains(ActiveIds, Child.Id);
		NodeEditor::Link(ToLink(Child.Id), ToOutputPin(Node.Id), ToInputPin(Child.Id),
		                 bActive ? FEditorTheme::Success : ImVec4(0.75f, 0.75f, 0.78f, 0.8f), bActive ? 3.0f : 2.0f);
		if (bActive && !Contains(PreviousActiveIds, Child.Id))
		{
			NodeEditor::Flow(ToLink(Child.Id)); // 새로 실행된 가지로 흐름 표시
		}
		DrawGraphLinks(Child, ActiveIds);
	}
}

void FBehaviorTreeEditor::HandleGraphEdits()
{
	if (!Asset.Root)
	{
		return;
	}
	// 출력 핀(컴포지트) → 입력 핀(노드): 그 노드(하위 트리째)를 새 부모 아래로 옮긴다
	if (NodeEditor::BeginCreate(ImColor(255, 255, 255), 2.0f))
	{
		NodeEditor::PinId Start;
		NodeEditor::PinId End;
		if (NodeEditor::QueryNewLink(&Start, &End))
		{
			uintptr_t A = Start.Get();
			uintptr_t B = End.Get();
			if (DecodeKind(A) == 1 && DecodeKind(B) == 2)
			{
				std::swap(A, B);
			}
			const uint32       ParentId = DecodeId(A);
			const uint32       ChildId  = DecodeId(B);
			FBTNodeDesc*       Parent   = Asset.FindNode(ParentId);
			FBTNodeDesc*       Child    = Asset.FindNode(ChildId);
			const FBTNodeDesc* Current  = Child ? FindParent(*Asset.Root, ChildId) : nullptr;
			const bool         bValid   = DecodeKind(A) == 2 && DecodeKind(B) == 1 && Parent && Child && Current && Current != Parent &&
			                      !SubtreeContains(*Child, ParentId);
			if (!bValid)
			{
				NodeEditor::RejectNewItem(ImColor(255, 90, 90), 2.0f);
			}
			else if (NodeEditor::AcceptNewItem(ImColor(120, 230, 120), 3.0f))
			{
				PendingChange = [this, ParentId, ChildId]() {
					FBTNodeDesc* OldParent = FindParent(*Asset.Root, ChildId);
					const auto   Found     = std::find_if(OldParent->Children.begin(), OldParent->Children.end(),
					                                      [ChildId](const FBTNodeDesc& Node) { return Node.Id == ChildId; });
					FBTNodeDesc  Moved     = std::move(*Found);
					OldParent->Children.erase(Found);
					FBTNodeDesc* NewParent = Asset.FindNode(ParentId); // 지운 뒤 다시 찾는다 (벡터 재배치)
					NewParent->Children.push_back(std::move(Moved));
					SortChildrenByX(*NewParent);
					MarkEdited("부모 변경");
				};
			}
		}
	}
	NodeEditor::EndCreate();

	// 삭제: 링크만 지우는 것은 막고(트리가 끊어진다), 노드는 하위 트리째 (루트 제외)
	if (NodeEditor::BeginDelete())
	{
		NodeEditor::LinkId Link;
		while (NodeEditor::QueryDeletedLink(&Link))
		{
			NodeEditor::RejectDeletedItem();
		}
		std::vector<uint32> Deleted;
		NodeEditor::NodeId  Node;
		while (NodeEditor::QueryDeletedNode(&Node))
		{
			const uint32 Id = DecodeId(Node.Get());
			if (Id == Asset.Root->Id)
			{
				NodeEditor::RejectDeletedItem();
			}
			else if (NodeEditor::AcceptDeletedItem())
			{
				Deleted.push_back(Id);
			}
		}
		if (!Deleted.empty())
		{
			PendingChange = [this, Deleted]() {
				for (uint32 Id : Deleted)
				{
					if (FBTNodeDesc* Parent = FindParent(*Asset.Root, Id))
					{
						std::erase_if(Parent->Children, [Id](const FBTNodeDesc& Child) { return Child.Id == Id; });
					}
				}
				if (FindSelectedDesc() == nullptr)
				{
					Selection = {};
				}
				MarkEdited("노드 삭제");
			};
		}
	}
	NodeEditor::EndDelete();
}

bool FBehaviorTreeEditor::DrawNodeTypeMenu(EBTNodeCategory Category, std::string& OutType)
{
	bool bPicked = false;
	for (const FBTNodeInfo* Info : FBehaviorTreeNodeRegistry::Get().GetByCategory(Category))
	{
		if (ImGui::MenuItem(Info->DisplayName.c_str()))
		{
			OutType = Info->Name;
			bPicked = true;
		}
		ImGui::SetItemTooltip("%s (%s)", Info->Name.c_str(), Info->Owner.c_str());
	}
	return bPicked;
}

void FBehaviorTreeEditor::DrawGraphMenus()
{
	NodeEditor::Suspend();
	NodeEditor::NodeId ContextNode;
	if (NodeEditor::ShowNodeContextMenu(&ContextNode))
	{
		ContextNodeId = DecodeId(ContextNode.Get());
		ImGui::OpenPopup("##BTNodeMenu");
	}
	else if (NodeEditor::ShowBackgroundContextMenu())
	{
		ImGui::OpenPopup("##BTBackgroundMenu");
	}

	if (ImGui::BeginPopup("##BTNodeMenu"))
	{
		FBTNodeDesc* Target = Asset.FindNode(ContextNodeId);
		if (Target != nullptr)
		{
			const uint32 TargetId = Target->Id;
			std::string  Type;
			if (IsComposite(*Target) && ImGui::BeginMenu(ICON_FA_PLUS " 자식 추가"))
			{
				bool bPicked = false;
				if (ImGui::BeginMenu("컴포지트"))
				{
					bPicked = DrawNodeTypeMenu(EBTNodeCategory::Composite, Type);
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("태스크"))
				{
					bPicked = DrawNodeTypeMenu(EBTNodeCategory::Task, Type) || bPicked;
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
				if (bPicked)
				{
					// 새 자식: 부모 아래, 기존 자식들의 오른쪽
					const ImVec2 ParentPosition = NodeEditor::GetNodePosition(ToGraphNode(TargetId));
					const ImVec2 ParentSize     = NodeEditor::GetNodeSize(ToGraphNode(TargetId));
					float        X              = ParentPosition.x;
					for (const FBTNodeDesc& Child : Target->Children)
					{
						X = FMath::Max(X, (Child.EditorPosition ? Child.EditorPosition->X : X) + LayoutSpacingX);
					}
					FBTNodeDesc NewNode    = MakeNode(Type);
					NewNode.EditorPosition = FVector2(X, ParentPosition.y + ParentSize.y + 70.0f);
					PendingChange          = [this, TargetId, NewNode]() {
						const uint32 NewId = NewNode.Id;
						Asset.FindNode(TargetId)->Children.push_back(NewNode);
						bApplyPositions = true;
						Selection       = { NewId, ESelectionKind::Node, -1 };
						MarkEdited("노드 추가");
					};
				}
			}
			const auto AddAttachment = [&](const char* Label, EBTNodeCategory Category, bool bDecorator) {
				if (ImGui::BeginMenu(Label))
				{
					std::string AttachmentType;
					if (DrawNodeTypeMenu(Category, AttachmentType))
					{
						const FBTNodeDesc NewAttachment = MakeNode(AttachmentType);
						PendingChange                   = [this, TargetId, NewAttachment, bDecorator]() {
							FBTNodeDesc&              Owner = *Asset.FindNode(TargetId);
							std::vector<FBTNodeDesc>& List  = bDecorator ? Owner.Decorators : Owner.Services;
							List.push_back(NewAttachment);
							Selection = { TargetId, bDecorator ? ESelectionKind::Decorator : ESelectionKind::Service, static_cast<int32>(List.size()) - 1 };
							MarkEdited(bDecorator ? "데코레이터 추가" : "서비스 추가");
						};
					}
					ImGui::EndMenu();
				}
			};
			AddAttachment(ICON_FA_DIAMOND " 데코레이터 추가", EBTNodeCategory::Decorator, true);
			AddAttachment(ICON_FA_GEARS " 서비스 추가", EBTNodeCategory::Service, false);
			ImGui::Separator();
			const bool bRoot = TargetId == Asset.Root->Id;
			if (ImGui::MenuItem(ICON_FA_TRASH " 삭제 (하위 포함)", "Del", false, !bRoot))
			{
				NodeEditor::DeleteNode(ToGraphNode(TargetId)); // 다음 프레임 BeginDelete에서 처리
			}
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("##BTBackgroundMenu"))
	{
		if (!Asset.Root && ImGui::BeginMenu(ICON_FA_PLUS " 루트 추가"))
		{
			std::string Type;
			bool        bPicked = false;
			if (ImGui::BeginMenu("컴포지트"))
			{
				bPicked = DrawNodeTypeMenu(EBTNodeCategory::Composite, Type);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("태스크"))
			{
				bPicked = DrawNodeTypeMenu(EBTNodeCategory::Task, Type) || bPicked;
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
			if (bPicked)
			{
				FBTNodeDesc Root    = MakeNode(Type);
				Root.EditorPosition = FVector2(0.0f, 0.0f);
				PendingChange       = [this, Root]() {
					Asset.Root      = Root;
					bApplyPositions = true;
					MarkEdited("루트 추가");
				};
			}
		}
		if (ImGui::MenuItem(ICON_FA_EXPAND " 전체 보기", "F"))
		{
			NavigateFrames = 0;
		}
		ImGui::EndPopup();
	}
	NodeEditor::Resume();
}

void FBehaviorTreeEditor::SyncPositionsFromGraph()
{
	if (!Asset.Root)
	{
		return;
	}
	bool bMoved = false;
	ForEachGraphNode(*Asset.Root, [&](FBTNodeDesc& Node) {
		const ImVec2 Position = NodeEditor::GetNodePosition(ToGraphNode(Node.Id));
		if (Position.x == FLT_MAX)
		{
			return; // 아직 그려지지 않은 노드
		}
		if (!Node.EditorPosition || FMath::Abs(Node.EditorPosition->X - Position.x) > 0.5f || FMath::Abs(Node.EditorPosition->Y - Position.y) > 0.5f)
		{
			Node.EditorPosition = FVector2(Position.x, Position.y);
			bMoved              = true;
		}
	});
	bool bReordered = false;
	ForEachGraphNode(*Asset.Root, [&](FBTNodeDesc& Node) { bReordered = SortChildrenByX(Node) || bReordered; });
	if (bReordered)
	{
		MarkEdited("실행 순서 변경");
	}
	else if (bMoved)
	{
		MarkEdited("노드 이동");
	}
}

// ---------------------------------------------------------------- 속성

void FBehaviorTreeEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	const FBehaviorTreeInstance* DebugTree = FindDebugTree(Env, nullptr);
	if (ImGui::CollapsingHeader(ICON_FA_BRAIN " 블랙보드", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawBlackboard(DebugTree);
	}
	if (ImGui::CollapsingHeader(ICON_FA_LIST_CHECK " 선택 항목", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawSelection();
	}
}

void FBehaviorTreeEditor::DrawBlackboard(const FBehaviorTreeInstance* DebugTree)
{
	int32 RemoveIndex = -1;
	for (size_t Index = 0; Index < Asset.BlackboardKeys.size(); ++Index)
	{
		FBlackboardKeyDesc& Key = Asset.BlackboardKeys[Index];
		ImGui::PushID(static_cast<int>(Index));
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
		if (InputString("##Name", Key.Name))
		{
			MarkEdited("블랙보드 키 이름");
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
		int32 Type = static_cast<int32>(Key.Type);
		if (ImGui::Combo("##Type", &Type, KeyTypeNames, IM_ARRAYSIZE(KeyTypeNames)))
		{
			Key.Type = static_cast<EBlackboardKeyType>(Type);
			MarkEdited("블랙보드 키 타입");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_XMARK))
		{
			RemoveIndex = static_cast<int32>(Index);
		}
		ImGui::SetItemTooltip("키 삭제 (이 키를 쓰는 노드 파라미터는 직접 고쳐야 합니다)");
		if (DebugTree != nullptr)
		{
			const FBlackboardValue* Value = DebugTree->GetBlackboard().GetValue(Key.Name);
			ImGui::TextColored(FEditorTheme::Success, "   = %s", Value ? BlackboardValueToText(*Value).c_str() : "(설정 안 됨)");
		}
		ImGui::PopID();
	}
	if (RemoveIndex >= 0)
	{
		Asset.BlackboardKeys.erase(Asset.BlackboardKeys.begin() + RemoveIndex);
		MarkEdited("블랙보드 키 삭제");
	}
	if (ImGui::Button(ICON_FA_PLUS " 키 추가"))
	{
		std::string Name = "NewKey";
		for (int32 Suffix = 1; std::any_of(Asset.BlackboardKeys.begin(), Asset.BlackboardKeys.end(), [&](const FBlackboardKeyDesc& Key) { return Key.Name == Name; });
		     ++Suffix)
		{
			Name = std::format("NewKey{}", Suffix);
		}
		Asset.BlackboardKeys.push_back({ Name, EBlackboardKeyType::Bool });
		MarkEdited("블랙보드 키 추가");
	}
}

void FBehaviorTreeEditor::DrawSelection()
{
	FBTNodeDesc* Desc = FindSelectedDesc();
	if (Desc == nullptr)
	{
		ImGui::TextDisabled("그래프에서 노드나 데코레이터/서비스 줄을 선택하세요");
		return;
	}
	const FBTNodeInfo* Info = FindInfo(Desc->Type);
	if (Info == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, "알 수 없는 노드 타입: %s (게임 모듈이 로드되지 않았을 수 있습니다)", Desc->Type.c_str());
		return;
	}
	ImGui::TextUnformatted(Info->DisplayName.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s · #%u", Info->Name.c_str(), Desc->Id);
	if (DrawParams(*Desc, *Info))
	{
		MarkEdited("노드 속성");
	}

	if (Selection.Kind != ESelectionKind::Node)
	{
		if (ImGui::SmallButton(ICON_FA_ARROW_UP " 붙은 노드 선택"))
		{
			Selection = { Selection.NodeId, ESelectionKind::Node, -1 };
		}
		return;
	}
	FBTNodeDesc& Node = *Desc;
	DrawAttachmentList(Node, Node.Decorators, ESelectionKind::Decorator, "데코레이터 (위에서 아래로 평가)");
	DrawAttachmentList(Node, Node.Services, ESelectionKind::Service, "서비스");
}

bool FBehaviorTreeEditor::DrawParams(FBTNodeDesc& Desc, const FBTNodeInfo& Info)
{
	bool bChanged = false;
	for (const FBTParamDesc& Param : Info.Params)
	{
		ImGui::PushID(Param.Name.c_str());
		const FBTParamValue* Stored = Desc.FindParam(Param.Name);
		FBTParamValue        Value  = Stored ? *Stored : Param.Default;
		if (BehaviorTreeTypes::GetParamType(Value) != Param.Type)
		{
			Value = BehaviorTreeTypes::CoerceParam(Value, Param.Type).value_or(Param.Default);
		}
		const char* Label   = Param.DisplayName.c_str();
		bool        bEdited = false;
		switch (Param.Type)
		{
		case EPropertyType::Bool:    bEdited = ImGui::Checkbox(Label, &std::get<bool>(Value)); break;
		case EPropertyType::Int32:   bEdited = ImGui::DragInt(Label, &std::get<int32>(Value)); break;
		case EPropertyType::Float:   bEdited = ImGui::DragFloat(Label, &std::get<float>(Value), 0.05f); break;
		case EPropertyType::Vector3: bEdited = ImGui::DragFloat3(Label, &std::get<FVector3>(Value).X, 1.0f); break;
		case EPropertyType::String:
		{
			std::string& Text = std::get<std::string>(Value);
			const bool   bKey = Param.Name.size() >= 3 && Param.Name.ends_with("Key");
			if (!Param.Options.empty() || bKey)
			{
				// 선택지: 파라미터 옵션, 또는 이름이 ...Key면 블랙보드 키 목록
				std::vector<std::string> Options = Param.Options;
				if (bKey)
				{
					Options = { "" };
					for (const FBlackboardKeyDesc& Key : Asset.BlackboardKeys)
					{
						Options.push_back(Key.Name);
					}
				}
				if (ImGui::BeginCombo(Label, Text.empty() ? "(없음)" : Text.c_str()))
				{
					for (const std::string& Option : Options)
					{
						if (ImGui::Selectable(Option.empty() ? "(없음)" : Option.c_str(), Option == Text))
						{
							Text    = Option;
							bEdited = true;
						}
					}
					ImGui::EndCombo();
				}
			}
			else
			{
				bEdited = InputString(Label, Text, Param.Name == "Properties");
				// 스크립트 경로: 콘텐츠 브라우저에서 .lua 끌어 놓기
				if (Param.Name == "Script" && ImGui::BeginDragDropTarget())
				{
					if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload(); Paths && !Paths->empty())
					{
						Text    = FModelLoader::MakeAssetPath(Paths->front());
						bEdited = true;
					}
					ImGui::EndDragDropTarget();
				}
			}
			break;
		}
		default: break;
		}
		if (bEdited)
		{
			Desc.SetParam(Param.Name, Value);
			bChanged = true;
		}
		ImGui::PopID();
	}
	return bChanged;
}

void FBehaviorTreeEditor::DrawAttachmentList(FBTNodeDesc& Node, std::vector<FBTNodeDesc>& List, ESelectionKind Kind, const char* Label)
{
	ImGui::SeparatorText(Label);
	ImGui::PushID(Label);
	int32 RemoveIndex = -1;
	int32 MoveUpIndex = -1;
	for (size_t Index = 0; Index < List.size(); ++Index)
	{
		ImGui::PushID(static_cast<int>(Index));
		const bool bSelected = Selection.NodeId == Node.Id && Selection.Kind == Kind && Selection.Index == static_cast<int32>(Index);
		if (ImGui::Selectable(DisplayNameOf(List[Index]).c_str(), bSelected, 0, ImVec2(ImGui::GetContentRegionAvail().x - 60.0f, 0.0f)))
		{
			Selection = { Node.Id, Kind, static_cast<int32>(Index) };
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(Index == 0);
		if (ImGui::SmallButton(ICON_FA_ARROW_UP))
		{
			MoveUpIndex = static_cast<int32>(Index);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TRASH))
		{
			RemoveIndex = static_cast<int32>(Index);
		}
		ImGui::PopID();
	}
	if (ImGui::SmallButton(ICON_FA_PLUS " 추가"))
	{
		ImGui::OpenPopup("##AddAttachment");
	}
	if (ImGui::BeginPopup("##AddAttachment"))
	{
		std::string Type;
		if (DrawNodeTypeMenu(Kind == ESelectionKind::Decorator ? EBTNodeCategory::Decorator : EBTNodeCategory::Service, Type))
		{
			List.push_back(MakeNode(Type));
			MarkEdited(Kind == ESelectionKind::Decorator ? "데코레이터 추가" : "서비스 추가");
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();

	if (MoveUpIndex > 0)
	{
		std::swap(List[static_cast<size_t>(MoveUpIndex)], List[static_cast<size_t>(MoveUpIndex - 1)]);
		MarkEdited("순서 변경");
	}
	if (RemoveIndex >= 0)
	{
		List.erase(List.begin() + RemoveIndex);
		if (Selection.NodeId == Node.Id && Selection.Kind == Kind)
		{
			Selection = { Node.Id, ESelectionKind::Node, -1 };
		}
		MarkEdited("항목 삭제");
	}
}
