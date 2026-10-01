#include <imgui.h>

#pragma warning(push, 0)
#include <imgui_node_editor.h>
#pragma warning(pop)

#include "Editor/AssetEditors/AnimGraphEditor.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/ResourceManager.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace NodeEditor = ax::NodeEditor;

namespace
{
	// 그래프 ID: (번호 << 2) | 종류 (0 노드, 1 입력 핀, 2 출력 핀, 3 링크). 상태 노드 번호 = 상태 + 1, 전이 링크 번호 = 전이 + 1
	constexpr uint32 AnyStateId = 0xFFFFF;

	NodeEditor::NodeId ToNode(uint32 Id) { return NodeEditor::NodeId(static_cast<uintptr_t>(Id) << 2); }
	NodeEditor::PinId  ToInputPin(uint32 Id) { return NodeEditor::PinId((static_cast<uintptr_t>(Id) << 2) | 1); }
	NodeEditor::PinId  ToOutputPin(uint32 Id) { return NodeEditor::PinId((static_cast<uintptr_t>(Id) << 2) | 2); }
	NodeEditor::LinkId ToLink(int32 Transition) { return NodeEditor::LinkId((static_cast<uintptr_t>(Transition + 1) << 2) | 3); }
	uint32             DecodeId(uintptr_t Raw) { return static_cast<uint32>(Raw >> 2); }
	uintptr_t          DecodeKind(uintptr_t Raw) { return Raw & 3; }
	uint32             StateNodeId(int32 State) { return static_cast<uint32>(State + 1); }

	ImU32 ToU32(const ImVec4& Color) { return ImGui::ColorConvertFloat4ToU32(Color); }

	constexpr const char* OpLabels[] = { "<", "<=", ">", ">=", "==", "!=" };

	bool InputString(const char* Label, std::string& Value)
	{
		char Buffer[256];
		strncpy_s(Buffer, Value.c_str(), _TRUNCATE);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	bool IsBlendSpace(const FAnimGraphState& State) { return !State.BlendParameter.empty(); }

	std::string DescribeState(const FAnimGraphState& State)
	{
		if (!IsBlendSpace(State))
		{
			const std::string& Clip = State.Samples.empty() ? std::string() : State.Samples.front().Clip;
			return Clip.empty() ? std::string("(클립 없음)") : Clip;
		}
		return std::format("{} · 샘플 {}개", State.BlendParameter, State.Samples.size());
	}
} // namespace

void FAnimGraphEditor::FGraphDeleter::operator()(NodeEditor::EditorContext* Context) const
{
	NodeEditor::DestroyEditor(Context);
}

FAnimGraphEditor::FAnimGraphEditor(std::filesystem::path InPath)
	: FAssetEditor(std::move(InPath))
{
	NodeEditor::Config Config;
	Config.SettingsFile = nullptr; // 노드 위치는 에셋(EditorPosition)에 저장한다
	Graph.reset(NodeEditor::CreateEditor(&Config));
}

FAnimGraphEditor::~FAnimGraphEditor() = default;

// ---------------------------------------------------------------- 에셋 상태

bool FAnimGraphEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string              Text;
	std::string              Error;
	std::vector<std::string> Warnings;
	FAnimGraphAsset          Loaded;
	if (!FFileSystem::ReadTextFile(Path, Text) || !FAnimGraphAsset::FromJsonString(Text, Loaded, &Error, &Warnings))
	{
		E_LOG(LogEditor, Error, "애니메이션 그래프를 읽지 못했습니다: {} — {}", GetDisplayName(), Error);
		if (Env.Editor && Env.Editor->Notify)
		{
			Env.Editor->Notify("애니메이션 그래프를 읽지 못했습니다: " + Error, true);
		}
		return false;
	}
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogEditor, Warning, "애니메이션 그래프 {}: {}", GetDisplayName(), Warning);
	}
	Asset           = std::move(Loaded);
	bApplyPositions = true;
	NavigateFrames  = 2;
	if ((Selection.Kind == ESelectionKind::State && Selection.Index >= static_cast<int32>(Asset.States.size())) ||
	    (Selection.Kind == ESelectionKind::Transition && Selection.Index >= static_cast<int32>(Asset.Transitions.size())))
	{
		Selection = {};
	}

	// 미리보기 모델 후보 (Content 안 모델 파일, glTF 먼저 — FBX는 읽기 느림)
	if (ModelFiles.empty() && Env.Editor != nullptr)
	{
		std::error_code ErrorCode;
		for (auto It = std::filesystem::recursive_directory_iterator(Env.Editor->ContentDirectory, ErrorCode);
		     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
		{
			if (It->is_regular_file(ErrorCode) && FModelLoader::IsModelFile(It->path()))
			{
				ModelFiles.push_back(FModelLoader::MakeAssetPath(It->path()));
			}
		}
		std::sort(ModelFiles.begin(), ModelFiles.end(), [](const std::string& A, const std::string& B) {
			const bool AFbx = A.ends_with(".fbx") || A.ends_with(".FBX");
			const bool BFbx = B.ends_with(".fbx") || B.ends_with(".FBX");
			return AFbx != BFbx ? BFbx : A < B;
		});
	}
	EnsurePreviewModel(Env);

	// 자동 검증: --animgraph-select <상태 이름> 또는 <출발>-><도착> (전이) 를 선택해 속성 패널을 보인다
	if (const std::wstring Select = FCommandLine::FromProcess().GetValue(L"--animgraph-select"); !Select.empty() && Selection.Kind == ESelectionKind::None)
	{
		const std::string Arg  = FStringConv::ToUtf8(Select);
		const size_t      Arrow = Arg.find("->");
		if (Arrow == std::string::npos)
		{
			if (const int32 State = Asset.FindState(Arg); State >= 0)
			{
				Selection = { ESelectionKind::State, State };
			}
		}
		else
		{
			const std::string From = Arg.substr(0, Arrow);
			const int32       FromIndex = From == "*" ? -1 : Asset.FindState(From);
			const int32       ToIndex   = Asset.FindState(Arg.substr(Arrow + 2));
			for (size_t Index = 0; Index < Asset.Transitions.size(); ++Index)
			{
				if (Asset.Transitions[Index].From == FromIndex && Asset.Transitions[Index].To == ToIndex)
				{
					Selection = { ESelectionKind::Transition, static_cast<int32>(Index) };
					break;
				}
			}
		}
		bSyncGraphSelection = Selection.Kind != ESelectionKind::None;
		bScrollToSelection  = bSyncGraphSelection;
	}
	if (const std::wstring Value = FCommandLine::FromProcess().GetValue(L"--animgraph-param"); !Value.empty())
	{
		// 자동 검증: --animgraph-param 이름=값 (미리보기 파라미터)
		const std::string Arg = FStringConv::ToUtf8(Value);
		if (const size_t Equal = Arg.find('='); Equal != std::string::npos)
		{
			PreviewValue(Arg.substr(0, Equal)) = std::strtof(Arg.c_str() + Equal + 1, nullptr);
		}
	}
	return true;
}

bool FAnimGraphEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	// 상태 이름은 전이가 이름으로 참조하므로 비거나 겹치면 저장하지 않는다 (다시 읽을 수 없는 파일이 된다)
	std::set<std::string> Names;
	for (const FAnimGraphState& State : Asset.States)
	{
		if (State.Name.empty() || !Names.insert(State.Name).second)
		{
			if (Env.Editor && Env.Editor->Notify)
			{
				Env.Editor->Notify("상태 이름이 비었거나 겹칩니다: '" + State.Name + "' — 저장하지 않았습니다", true);
			}
			return false;
		}
	}
	if (!Asset.SaveToFile(Path))
	{
		return false;
	}
	// 핫 리로드: 이 그래프를 쓰는 컴포넌트가 다음 갱신에서 새 파일로 다시 묶인다 (파일 감시도 같은 일을 한다)
	FAnimGraphLibrary::Get().Invalidate(FModelLoader::MakeAssetPath(Path));
	return true;
}

std::string FAnimGraphEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FAnimGraphEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FAnimGraphAsset Restored;
	if (FAnimGraphAsset::FromJsonString(State, Restored))
	{
		Asset           = std::move(Restored);
		bApplyPositions = true;
		if ((Selection.Kind == ESelectionKind::State && Selection.Index >= static_cast<int32>(Asset.States.size())) ||
		    (Selection.Kind == ESelectionKind::Transition && Selection.Index >= static_cast<int32>(Asset.Transitions.size())))
		{
			Selection = {};
		}
	}
}

void FAnimGraphEditor::FramePreview(FAssetEditorEnvironment& Env)
{
	FAssetEditor::FramePreview(Env);
	NavigateFrames = 2;
}

// ---------------------------------------------------------------- 미리보기

FAnimGraphRuntime* FAnimGraphEditor::GetPreviewRuntime()
{
	FRegistry& Registry = Preview.GetScene().GetRegistry();
	if (!Registry.IsValid(ModelRoot))
	{
		return nullptr;
	}
	FAnimGraphComponent* Component = Registry.TryGet<FAnimGraphComponent>(ModelRoot);
	return Component != nullptr ? &Component->Runtime : nullptr;
}

std::string FAnimGraphEditor::FindAutoPreviewModel(FAssetEditorEnvironment& Env) const
{
	if (Env.Editor == nullptr || Env.Resources == nullptr)
	{
		return {};
	}
	// 시작 상태(없으면 아무 상태)의 첫 클립 이름을 가진 모델
	std::string Target;
	for (int32 Pass = 0; Pass < 2 && Target.empty(); ++Pass)
	{
		for (size_t Index = 0; Index < Asset.States.size() && Target.empty(); ++Index)
		{
			if (Pass == 0 && static_cast<int32>(Index) != Asset.EntryState)
			{
				continue;
			}
			for (const FAnimBlendSample& Sample : Asset.States[Index].Samples)
			{
				if (!Sample.Clip.empty())
				{
					Target = Sample.Clip;
					break;
				}
			}
		}
	}
	std::string FirstAnimated;
	for (const std::string& File : ModelFiles)
	{
		const FModelResources* Model = FModelLoader::LoadModelResources(Env.Editor->ContentDirectory / FStringConv::ToWide(File), *Env.Resources);
		if (Model == nullptr || Model->Model.Animations.empty())
		{
			continue;
		}
		if (FirstAnimated.empty())
		{
			FirstAnimated = File;
		}
		if (Target.empty() || std::any_of(Model->Model.Animations.begin(), Model->Model.Animations.end(),
		                                  [&](const FAnimationClip& Clip) { return Clip.Name == Target; }))
		{
			return File;
		}
	}
	return FirstAnimated;
}

void FAnimGraphEditor::EnsurePreviewModel(FAssetEditorEnvironment& Env)
{
	if (Env.Editor == nullptr || Env.Resources == nullptr)
	{
		return;
	}
	if (Asset.PreviewModel.empty() && !bAutoModelSearched)
	{
		AutoModel          = FindAutoPreviewModel(Env);
		bAutoModelSearched = true;
	}
	const std::string Desired = Asset.PreviewModel.empty() ? AutoModel : Asset.PreviewModel;
	if (Desired == LoadedModel)
	{
		return;
	}
	FScene& Scene = Preview.GetScene();
	if (Scene.GetRegistry().IsValid(ModelRoot))
	{
		Scene.DestroyEntity(ModelRoot);
	}
	ModelRoot   = FEntity{};
	LoadedModel = Desired;
	ClipNames.clear();
	if (Desired.empty())
	{
		return;
	}
	ModelRoot = FModelLoader::LoadIntoScene(Env.Editor->ContentDirectory / FStringConv::ToWide(Desired), Scene, *Env.Resources);
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(ModelRoot))
	{
		E_LOG(LogEditor, Warning, "애니메이션 그래프 미리보기 모델을 읽지 못했습니다: {}", Desired);
		return;
	}
	FAnimGraphComponent& Component  = Registry.Has<FAnimGraphComponent>(ModelRoot) ? Registry.Get<FAnimGraphComponent>(ModelRoot)
	                                                                                : Registry.Emplace<FAnimGraphComponent>(ModelRoot);
	Component.Graph                 = FModelLoader::MakeAssetPath(Path);
	Component.bUseCharacterMovement = false;
	ClipNames                       = FAnimationSystem::GetClipNames(Scene, ModelRoot);
	PublishIfChanged();
	Update(Env, 0.0f);
	FAssetEditor::FramePreview(Env);
}

void FAnimGraphEditor::PublishIfChanged()
{
	// 실행에 쓰는 내용만 비교한다 (노드를 옮겨도 미리보기가 다시 시작하지 않게). 샘플은 위치순으로 (실행 규칙)
	FAnimGraphAsset Copy = Asset;
	Copy.PreviewModel.clear();
	Copy.AnyStateEditorPosition.reset();
	for (FAnimGraphState& State : Copy.States)
	{
		State.EditorPosition.reset();
		std::stable_sort(State.Samples.begin(), State.Samples.end(), [](const FAnimBlendSample& A, const FAnimBlendSample& B) { return A.Position < B.Position; });
	}
	std::string Key = Copy.ToJsonString();
	if (Published && Key == PublishedKey)
	{
		return;
	}
	PublishedKey = std::move(Key);
	Published    = std::make_shared<const FAnimGraphAsset>(std::move(Copy));
}

float& FAnimGraphEditor::PreviewValue(const std::string& Name)
{
	for (auto& [Key, Value] : PreviewValues)
	{
		if (Key == Name)
		{
			return Value;
		}
	}
	const FAnimGraphParameter* Parameter = Asset.FindParameter(Name);
	PreviewValues.emplace_back(Name, Parameter != nullptr ? Parameter->Default : 0.0f);
	return PreviewValues.back().second;
}

void FAnimGraphEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	EnsurePreviewModel(Env);
	PublishIfChanged();
	FScene& Scene = Preview.GetScene();
	if (FAnimGraphRuntime* Runtime = GetPreviewRuntime())
	{
		// 디스크 파일 대신 편집 중 사본을 재생한다 (라이브러리 세대를 맞춰 두면 FAnimationSystem이 다시 읽지 않는다)
		FRegistry&           Registry  = Scene.GetRegistry();
		FAnimGraphComponent& Component = Registry.Get<FAnimGraphComponent>(ModelRoot);
		Runtime->bResolved             = true;
		Runtime->ResolvedGraph         = Component.Graph;
		Runtime->ResolvedGeneration    = FAnimGraphLibrary::Get().GetGeneration();
		Runtime->SetAsset(Published, true);
		for (const FAnimGraphParameter& Parameter : Asset.Parameters)
		{
			Runtime->Parameters.Set(Parameter.Name, PreviewValue(Parameter.Name));
		}
		if (FAnimationComponent* Animation = Registry.TryGet<FAnimationComponent>(ModelRoot))
		{
			Animation->bPlaying = bPreviewPlaying;
			Animation->Speed    = PreviewSpeed;
		}
	}
	FAnimationSystem::Update(Scene, DeltaSeconds);
	Scene.UpdateTransforms();
}

FAnimGraphEditor::FDebugView FAnimGraphEditor::MakeDebugView(FAssetEditorEnvironment& Env)
{
	FDebugView View;
	FEditorContext* Context = Env.Editor;
	if (Context != nullptr && Context->bPlaying && Context->Scene != nullptr)
	{
		const std::string AssetPath = FModelLoader::MakeAssetPath(Path);
		FRegistry&        Registry  = Context->Scene->GetRegistry();
		const auto        Matches   = [&](FEntity Entity) {
            const FAnimGraphComponent* Component = Registry.IsValid(Entity) ? Registry.TryGet<FAnimGraphComponent>(Entity) : nullptr;
            return Component != nullptr && Component->Graph == AssetPath;
		};
		FEntity Found = FAnimationSystem::FindAnimGraph(*Context->Scene, Context->SelectedEntity);
		if (!Matches(Found))
		{
			Found = NullEntity;
			Registry.View<FAnimGraphComponent>().Each([&](FEntity Entity, FAnimGraphComponent&) {
				if (!Found.IsValid() && Matches(Entity))
				{
					Found = Entity;
				}
			});
		}
		if (Found.IsValid())
		{
			View.Runtime  = &Registry.Get<FAnimGraphComponent>(Found).Runtime;
			View.bPlaying = true;
			// 이름은 캐릭터 루트 쪽이 알아보기 쉽다 (모델 루트는 보통 "Mesh" 등)
			FEntity Named = Found;
			for (FEntity Current = Found; Current.IsValid() && Registry.IsValid(Current); Current = Context->Scene->GetParent(Current))
			{
				Named = Current;
				if (Current == Context->SelectedEntity)
				{
					break;
				}
			}
			const FNameComponent* Name = Registry.TryGet<FNameComponent>(Named);
			View.Label                 = Name ? Name->Name : std::string("?");
		}
	}
	if (View.Runtime == nullptr)
	{
		View.Runtime = GetPreviewRuntime();
		View.Label   = "미리보기";
	}
	if (View.Runtime == nullptr || !View.Runtime->Asset)
	{
		return View;
	}
	// 실행 중 에셋 번호 → 편집 중 에셋 번호 (저장 전이라 다를 수 있다 — 이름으로 맞춘다)
	const FAnimGraphAsset& Running   = *View.Runtime->Asset;
	const auto             MapState  = [&](int32 Index) {
        return Index >= 0 && Index < static_cast<int32>(Running.States.size()) ? Asset.FindState(Running.States[static_cast<size_t>(Index)].Name) : -1;
	};
	const FAnimGraphInstance& Instance = View.Runtime->Instance;
	View.CurrentState                  = MapState(Instance.GetCurrentState());
	for (size_t Layer = 0; Layer < Instance.GetLayerCount(); ++Layer)
	{
		View.Layers.emplace_back(MapState(Instance.GetLayerState(Layer)), Instance.GetLayerWeight(Layer));
	}
	View.TransitionCount = Instance.GetTransitionCount();
	if (const int32 Last = Instance.GetLastTransition(); Last >= 0 && Last < static_cast<int32>(Running.Transitions.size()))
	{
		const FAnimGraphTransition& Transition = Running.Transitions[static_cast<size_t>(Last)];
		const int32                 From       = Transition.From < 0 ? -1 : MapState(Transition.From);
		const int32                 To         = MapState(Transition.To);
		for (size_t Index = 0; Index < Asset.Transitions.size(); ++Index)
		{
			if (Asset.Transitions[Index].From == From && Asset.Transitions[Index].To == To)
			{
				View.LastTransition = static_cast<int32>(Index);
				break;
			}
		}
	}
	return View;
}

// ---------------------------------------------------------------- 왼쪽: 그래프 + 미리보기

void FAnimGraphEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FAssetEditor::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	if (ImGui::SmallButton(bPreviewPlaying ? ICON_FA_PAUSE " 일시정지" : ICON_FA_PLAY " 재생"))
	{
		bPreviewPlaying = !bPreviewPlaying;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_BACKWARD_STEP " 처음부터"))
	{
		if (FAnimGraphRuntime* Runtime = GetPreviewRuntime())
		{
			Runtime->Instance.Reset();
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110.0f);
	ImGui::SliderFloat("##PreviewSpeed", &PreviewSpeed, 0.0f, 2.0f, "속도 %.2f");
}

void FAnimGraphEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	const FDebugView Debug = MakeDebugView(Env);
	if (Debug.TransitionCount != SeenTransitionCount)
	{
		SeenTransitionCount = Debug.TransitionCount;
		if (Debug.LastTransition >= 0)
		{
			FlashTransition     = Debug.LastTransition;
			TransitionFlashTime = ImGui::GetTime();
		}
	}

	if (ImGui::SmallButton(ICON_FA_EXPAND " 전체 보기 (F)"))
	{
		NavigateFrames = 0;
	}
	ImGui::SameLine();
	if (Debug.bPlaying)
	{
		ImGui::TextColored(FEditorTheme::Success, ICON_FA_PLAY " 디버그: %s", Debug.Label.c_str());
	}
	else if (Env.Editor && Env.Editor->bPlaying)
	{
		ImGui::TextDisabled("플레이 중 — 이 그래프를 쓰는 엔티티가 없습니다 (미리보기 표시)");
	}
	else
	{
		ImGui::TextDisabled("우클릭: 상태 추가 · 오른쪽 핀을 다른 상태로 끌어 전이 · Delete: 삭제");
	}
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
	    !ImGui::GetIO().KeyCtrl)
	{
		if (Preview.IsHovered())
		{
			FramePreview(Env);
		}
		else
		{
			NavigateFrames = 0;
		}
	}

	const float Avail       = FMath::Max(ImGui::GetContentRegionAvail().y, 200.0f);
	const float GraphHeight = FMath::Max(120.0f, Avail * GraphFraction);
	DrawGraph(Debug, GraphHeight);

	// 그래프/미리보기 경계: 끌어서 높이 조절
	ImGui::InvisibleButton("##GraphSplitter", ImVec2(ImGui::GetContentRegionAvail().x, 6.0f));
	if (ImGui::IsItemActive())
	{
		GraphFraction = FMath::Clamp(GraphFraction + ImGui::GetIO().MouseDelta.y / Avail, 0.2f, 0.85f);
	}
	if (ImGui::IsItemHovered() || ImGui::IsItemActive())
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
	}
	const ImVec2 SplitMin = ImGui::GetItemRectMin();
	const ImVec2 SplitMax = ImGui::GetItemRectMax();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(SplitMin.x, (SplitMin.y + SplitMax.y) * 0.5f), ImVec2(SplitMax.x, (SplitMin.y + SplitMax.y) * 0.5f),
	                                    IM_COL32(255, 255, 255, 50), 2.0f);

	DrawPreviewToolbar(Env);
	const ImVec2 Remaining = ImGui::GetContentRegionAvail();
	Preview.DrawViewport(FVector2(Remaining.x, FMath::Max(Remaining.y, 64.0f)));
	if (LoadedModel.empty())
	{
		ImGui::SetCursorScreenPos(ImVec2(Preview.GetViewportMin().X + 12.0f, Preview.GetViewportMin().Y + 12.0f));
		ImGui::TextColored(FEditorTheme::Warning, "미리보기 모델이 없습니다 — 오른쪽 '미리보기'에서 애니메이션 모델을 고르세요");
	}
}

void FAnimGraphEditor::AutoLayoutMissing()
{
	// 위치 없는 노드: "어느 상태든"은 왼쪽 위, 상태는 격자 (열 = √개수, 전이 화살표가 보이게 넉넉히)
	constexpr float SpacingX = 380.0f;
	constexpr float SpacingY = 210.0f;
	if (!Asset.AnyStateEditorPosition)
	{
		Asset.AnyStateEditorPosition = FVector2(-SpacingX, -SpacingY * 0.5f);
	}
	const size_t Columns = FMath::Max<size_t>(1, static_cast<size_t>(std::ceil(std::sqrt(static_cast<float>(Asset.States.size())))));
	for (size_t Index = 0; Index < Asset.States.size(); ++Index)
	{
		if (!Asset.States[Index].EditorPosition)
		{
			const float Row = static_cast<float>(Index / Columns);
			// 홀수 행은 반 칸 밀어 같은 열 노드 사이 화살표가 겹치지 않게
			Asset.States[Index].EditorPosition =
				FVector2(static_cast<float>(Index % Columns) * SpacingX + (static_cast<size_t>(Row) % 2 == 1 ? SpacingX * 0.5f : 0.0f), Row * SpacingY);
		}
	}
}

void FAnimGraphEditor::DrawGraph(const FDebugView& Debug, float Height)
{
	NodeEditor::SetCurrentEditor(Graph.get());
	NodeEditor::Begin("##AnimGraph", ImVec2(0.0f, Height));

	const bool bAppliedPositions = bApplyPositions;
	if (bApplyPositions)
	{
		AutoLayoutMissing();
		NodeEditor::SetNodePosition(ToNode(AnyStateId), ImVec2(Asset.AnyStateEditorPosition->X, Asset.AnyStateEditorPosition->Y));
		for (size_t Index = 0; Index < Asset.States.size(); ++Index)
		{
			const FVector2& Position = *Asset.States[Index].EditorPosition;
			NodeEditor::SetNodePosition(ToNode(StateNodeId(static_cast<int32>(Index))), ImVec2(Position.X, Position.Y));
		}
		bApplyPositions = false;
	}
	DrawAnyStateNode();
	for (size_t Index = 0; Index < Asset.States.size(); ++Index)
	{
		DrawStateNode(static_cast<int32>(Index), Debug);
	}

	// 전이 화살표 (방금 일어난 전이는 1초 동안 강조)
	const bool bFlash = ImGui::GetTime() - TransitionFlashTime < 1.0;
	for (size_t Index = 0; Index < Asset.Transitions.size(); ++Index)
	{
		const FAnimGraphTransition& Transition = Asset.Transitions[Index];
		if (Transition.To < 0 || Transition.To >= static_cast<int32>(Asset.States.size()) || Transition.From >= static_cast<int32>(Asset.States.size()))
		{
			continue;
		}
		const uint32 FromId      = Transition.From < 0 ? AnyStateId : StateNodeId(Transition.From);
		const bool   bHighlight  = bFlash && FlashTransition == static_cast<int32>(Index);
		const ImVec4 Color       = bHighlight ? FEditorTheme::Success : (Transition.From < 0 ? ImVec4(0.95f, 0.72f, 0.4f, 0.8f) : ImVec4(0.75f, 0.75f, 0.78f, 0.85f));
		NodeEditor::Link(ToLink(static_cast<int32>(Index)), ToOutputPin(FromId), ToInputPin(StateNodeId(Transition.To)), Color, bHighlight ? 3.5f : 2.0f);
	}
	if (bFlash && FlashTransition >= 0 && FlashTransition < static_cast<int32>(Asset.Transitions.size()) && ImGui::GetTime() - TransitionFlashTime < 0.05)
	{
		NodeEditor::Flow(ToLink(FlashTransition));
	}

	// 속성 패널에서 바꾼 선택을 그래프에 (노드/링크를 제출한 뒤여야 선택된다). 이번 프레임은 그래프 → 속성 따라가기를 건너뛴다
	const bool bSynced = bSyncGraphSelection;
	if (bSyncGraphSelection)
	{
		bSyncGraphSelection = false;
		NodeEditor::ClearSelection();
		if (Selection.Kind == ESelectionKind::State)
		{
			NodeEditor::SelectNode(ToNode(StateNodeId(Selection.Index)));
		}
		else if (Selection.Kind == ESelectionKind::AnyState)
		{
			NodeEditor::SelectNode(ToNode(AnyStateId));
		}
		else if (Selection.Kind == ESelectionKind::Transition)
		{
			NodeEditor::SelectLink(ToLink(Selection.Index));
		}
	}

	HandleGraphEdits();
	DrawGraphMenus();

	// 그래프 선택 → 속성 선택 (그래프 쪽 선택이 바뀔 때만)
	NodeEditor::NodeId Nodes[2];
	NodeEditor::LinkId Links[2];
	const int32        NodeCount = NodeEditor::GetSelectedNodes(Nodes, 2);
	const int32        LinkCount = NodeEditor::GetSelectedLinks(Links, 2);
	const uint32       NodeSel   = NodeCount == 1 ? DecodeId(Nodes[0].Get()) : 0;
	const uintptr_t    LinkSel   = NodeCount == 0 && LinkCount == 1 ? Links[0].Get() : 0;
	if (!bSynced && (NodeSel != LastGraphNode || LinkSel != LastGraphLink))
	{
		if (NodeSel == AnyStateId)
		{
			Selection = { ESelectionKind::AnyState, -1 };
		}
		else if (NodeSel != 0)
		{
			Selection = { ESelectionKind::State, static_cast<int32>(NodeSel) - 1 };
		}
		else if (LinkSel != 0)
		{
			Selection = { ESelectionKind::Transition, static_cast<int32>(DecodeId(LinkSel)) - 1 };
		}
		else
		{
			Selection = {};
		}
	}
	LastGraphNode = NodeSel;
	LastGraphLink = LinkSel;

	if (NavigateFrames >= 0 && NavigateFrames-- == 0)
	{
		NodeEditor::NavigateToContent(0.0f);
	}
	NodeEditor::End();
	if (!bAppliedPositions)
	{
		SyncPositionsFromGraph();
	}
	if (PendingChange)
	{
		const std::function<void()> Change = std::move(PendingChange);
		PendingChange                      = nullptr;
		Change();
	}
	NodeEditor::SetCurrentEditor(nullptr);
}

void FAnimGraphEditor::DrawAnyStateNode()
{
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, ImColor(72, 54, 38, 235));
	NodeEditor::BeginNode(ToNode(AnyStateId));
	ImGui::BeginGroup();
	ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.4f, 1.0f), ICON_FA_ASTERISK " 어느 상태든");
	ImGui::TextDisabled("현재 상태와 무관한 전이");
	ImGui::EndGroup();
	ImGui::SameLine();
	NodeEditor::BeginPin(ToOutputPin(AnyStateId), NodeEditor::PinKind::Output);
	ImGui::TextUnformatted(ICON_FA_CARET_RIGHT);
	NodeEditor::EndPin();
	NodeEditor::EndNode();
	NodeEditor::PopStyleColor();
}

void FAnimGraphEditor::DrawStateNode(int32 Index, const FDebugView& Debug)
{
	const FAnimGraphState& State    = Asset.States[static_cast<size_t>(Index)];
	const bool             bCurrent = Debug.CurrentState == Index;
	float                  Weight   = -1.0f;
	for (const auto& [LayerState, LayerWeight] : Debug.Layers)
	{
		if (LayerState == Index)
		{
			Weight = LayerWeight;
		}
	}
	const bool bBlendingOut = !bCurrent && Weight > 0.0f;

	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, IsBlendSpace(State) ? ImColor(36, 60, 58, 235) : ImColor(44, 52, 68, 235));
	NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, bCurrent       ? ImColor(ToU32(FEditorTheme::Success))
	                                                              : bBlendingOut ? ImColor(ToU32(FEditorTheme::Warning))
	                                                                             : ImColor(255, 255, 255, 60));
	NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, bCurrent ? 3.0f : bBlendingOut ? 2.0f : 1.0f);
	const uint32 Id = StateNodeId(Index);
	NodeEditor::BeginNode(ToNode(Id));
	ImGui::PushID(Index);

	NodeEditor::BeginPin(ToInputPin(Id), NodeEditor::PinKind::Input);
	ImGui::TextUnformatted(ICON_FA_CARET_RIGHT);
	NodeEditor::EndPin();
	ImGui::SameLine();

	ImGui::BeginGroup();
	if (Index == Asset.EntryState)
	{
		ImGui::TextColored(FEditorTheme::Accent, ICON_FA_FLAG);
		ImGui::SameLine();
	}
	ImGui::Text("%s %s", IsBlendSpace(State) ? ICON_FA_ARROWS_LEFT_RIGHT : ICON_FA_FILM, State.Name.c_str());
	ImGui::TextDisabled("%s", DescribeState(State).c_str());
	if (Weight > 0.0f)
	{
		ImGui::TextColored(bCurrent ? FEditorTheme::Success : FEditorTheme::Warning, "가중치 %.2f", Weight);
	}
	ImGui::Dummy(ImVec2(140.0f, 0.0f));
	ImGui::EndGroup();
	ImGui::SameLine();

	NodeEditor::BeginPin(ToOutputPin(Id), NodeEditor::PinKind::Output);
	ImGui::TextUnformatted(ICON_FA_CARET_RIGHT);
	NodeEditor::EndPin();

	ImGui::PopID();
	NodeEditor::EndNode();
	NodeEditor::PopStyleVar();
	NodeEditor::PopStyleColor(2);
}

void FAnimGraphEditor::HandleGraphEdits()
{
	const int32 StateCount = static_cast<int32>(Asset.States.size());
	// 출력 핀 → 입력 핀: 전이 추가 (목록 끝 = 가장 낮은 우선순위)
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
			const uint32 FromId = DecodeId(A);
			const uint32 ToId   = DecodeId(B);
			const int32  From   = FromId == AnyStateId ? -1 : static_cast<int32>(FromId) - 1;
			const int32  To     = ToId == AnyStateId ? -1 : static_cast<int32>(ToId) - 1;
			const bool   bValid = DecodeKind(A) == 2 && DecodeKind(B) == 1 && To >= 0 && To < StateCount && From < StateCount && From != To;
			if (!bValid)
			{
				NodeEditor::RejectNewItem(ImColor(255, 90, 90), 2.0f);
			}
			else if (NodeEditor::AcceptNewItem(ImColor(120, 230, 120), 3.0f))
			{
				PendingChange = [this, From, To]() {
					FAnimGraphTransition Transition;
					Transition.From = From;
					Transition.To   = To;
					Asset.Transitions.push_back(Transition);
					Selection           = { ESelectionKind::Transition, static_cast<int32>(Asset.Transitions.size()) - 1 };
					bSyncGraphSelection = true;
					MarkEdited("전이 추가");
				};
			}
		}
	}
	NodeEditor::EndCreate();

	if (NodeEditor::BeginDelete())
	{
		std::vector<int32> DeletedTransitions;
		std::vector<int32> DeletedStates;
		NodeEditor::LinkId Link;
		while (NodeEditor::QueryDeletedLink(&Link))
		{
			if (NodeEditor::AcceptDeletedItem())
			{
				DeletedTransitions.push_back(static_cast<int32>(DecodeId(Link.Get())) - 1);
			}
		}
		NodeEditor::NodeId Node;
		while (NodeEditor::QueryDeletedNode(&Node))
		{
			const uint32 Id = DecodeId(Node.Get());
			// "어느 상태든"과 마지막 상태는 지우지 않는다 (상태가 하나는 있어야 한다)
			if (Id == AnyStateId || StateCount - static_cast<int32>(DeletedStates.size()) <= 1)
			{
				NodeEditor::RejectDeletedItem();
			}
			else if (NodeEditor::AcceptDeletedItem())
			{
				DeletedStates.push_back(static_cast<int32>(Id) - 1);
			}
		}
		if (!DeletedTransitions.empty() || !DeletedStates.empty())
		{
			PendingChange = [this, DeletedTransitions, DeletedStates]() mutable {
				// 전이를 원래 번호로 먼저 지우고(내림차순), 그다음 상태 (상태를 지우면 그 상태의 전이도 함께 지워진다)
				std::sort(DeletedTransitions.rbegin(), DeletedTransitions.rend());
				DeletedTransitions.erase(std::unique(DeletedTransitions.begin(), DeletedTransitions.end()), DeletedTransitions.end());
				for (int32 Index : DeletedTransitions)
				{
					if (Index >= 0 && Index < static_cast<int32>(Asset.Transitions.size()))
					{
						Asset.Transitions.erase(Asset.Transitions.begin() + Index);
					}
				}
				std::sort(DeletedStates.rbegin(), DeletedStates.rend());
				for (int32 Index : DeletedStates)
				{
					Asset.RemoveState(Index);
				}
				Selection       = {};
				bApplyPositions = true; // 노드 번호가 당겨졌다
				bSyncGraphSelection = true;
				MarkEdited(DeletedStates.empty() ? "전이 삭제" : "상태 삭제");
			};
		}
	}
	NodeEditor::EndDelete();
}

std::string FAnimGraphEditor::MakeUniqueStateName(const std::string& BaseName) const
{
	std::string Name = BaseName;
	for (int32 Suffix = 1; Asset.FindState(Name) >= 0; ++Suffix)
	{
		Name = std::format("{}{}", BaseName, Suffix);
	}
	return Name;
}

int32 FAnimGraphEditor::AddState(const std::string& BaseName, bool bBlendSpace, const FVector2& Position)
{
	FAnimGraphState State;
	State.Name           = MakeUniqueStateName(BaseName);
	State.EditorPosition = Position;
	if (bBlendSpace)
	{
		for (const FAnimGraphParameter& Parameter : Asset.Parameters)
		{
			if (Parameter.Type == EAnimParamType::Float)
			{
				State.BlendParameter = Parameter.Name;
				break;
			}
		}
		if (State.BlendParameter.empty())
		{
			Asset.Parameters.push_back({ "Speed", EAnimParamType::Float, 0.0f });
			State.BlendParameter = "Speed";
		}
		const std::string First = ClipNames.empty() ? std::string() : ClipNames.front();
		State.Samples           = { FAnimBlendSample{ First, 0.0f, 1.0f }, FAnimBlendSample{ First, 100.0f, 1.0f } };
	}
	else
	{
		State.Samples = { FAnimBlendSample{ ClipNames.empty() ? std::string() : ClipNames.front(), 0.0f, 1.0f } };
	}
	Asset.States.push_back(std::move(State));
	return static_cast<int32>(Asset.States.size()) - 1;
}

void FAnimGraphEditor::DrawGraphMenus()
{
	const ImVec2 MouseCanvas = ImGui::GetMousePos(); // Begin~End 안이라 그래프 좌표
	NodeEditor::Suspend();
	NodeEditor::NodeId ContextNode;
	NodeEditor::LinkId ContextLink;
	if (NodeEditor::ShowNodeContextMenu(&ContextNode))
	{
		ContextNodeId = DecodeId(ContextNode.Get());
		ImGui::OpenPopup("##AGNodeMenu");
	}
	else if (NodeEditor::ShowLinkContextMenu(&ContextLink))
	{
		ContextLinkId = ContextLink.Get();
		ImGui::OpenPopup("##AGLinkMenu");
	}
	else if (NodeEditor::ShowBackgroundContextMenu())
	{
		ContextCanvasPosition = FVector2(MouseCanvas.x, MouseCanvas.y);
		ImGui::OpenPopup("##AGBackgroundMenu");
	}

	if (ImGui::BeginPopup("##AGNodeMenu"))
	{
		const int32 State = ContextNodeId == AnyStateId ? -1 : static_cast<int32>(ContextNodeId) - 1;
		if (State < static_cast<int32>(Asset.States.size()))
		{
			if (State >= 0 && ImGui::MenuItem(ICON_FA_FLAG " 시작 상태로 지정", nullptr, State == Asset.EntryState))
			{
				Asset.EntryState = State;
				MarkEdited("시작 상태");
			}
			if (ImGui::BeginMenu(ICON_FA_RIGHT_LONG " 전이 추가"))
			{
				for (size_t Target = 0; Target < Asset.States.size(); ++Target)
				{
					if (static_cast<int32>(Target) != State && ImGui::MenuItem(Asset.States[Target].Name.c_str()))
					{
						FAnimGraphTransition Transition;
						Transition.From = State;
						Transition.To   = static_cast<int32>(Target);
						Asset.Transitions.push_back(Transition);
						Selection           = { ESelectionKind::Transition, static_cast<int32>(Asset.Transitions.size()) - 1 };
						bSyncGraphSelection = true;
						MarkEdited("전이 추가");
					}
				}
				ImGui::EndMenu();
			}
			if (State >= 0)
			{
				if (ImGui::MenuItem(ICON_FA_COPY " 복제"))
				{
					FAnimGraphState Copy = Asset.States[static_cast<size_t>(State)];
					Copy.Name            = MakeUniqueStateName(Copy.Name);
					Copy.EditorPosition  = Copy.EditorPosition.value_or(FVector2()) + FVector2(40.0f, 60.0f);
					Asset.States.push_back(std::move(Copy));
					bApplyPositions     = true;
					Selection           = { ESelectionKind::State, static_cast<int32>(Asset.States.size()) - 1 };
					bSyncGraphSelection = true;
					MarkEdited("상태 복제");
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_TRASH " 삭제", "Del", false, Asset.States.size() > 1))
				{
					NodeEditor::DeleteNode(ToNode(ContextNodeId)); // 다음 프레임 BeginDelete에서 처리
				}
			}
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("##AGLinkMenu"))
	{
		const int32 Transition = static_cast<int32>(DecodeId(ContextLinkId)) - 1;
		if (Transition >= 0 && Transition < static_cast<int32>(Asset.Transitions.size()))
		{
			if (ImGui::MenuItem(ICON_FA_ARROW_UP " 먼저 검사 (우선순위 올림)"))
			{
				MoveTransition(Transition, -1);
			}
			if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " 나중에 검사 (우선순위 내림)"))
			{
				MoveTransition(Transition, 1);
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_FA_TRASH " 전이 삭제", "Del"))
			{
				NodeEditor::DeleteLink(NodeEditor::LinkId(ContextLinkId));
			}
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("##AGBackgroundMenu"))
	{
		const auto AddAt = [this](const char* BaseName, bool bBlend, const char* Label) {
			const int32 Added   = AddState(BaseName, bBlend, ContextCanvasPosition);
			bApplyPositions     = true;
			Selection           = { ESelectionKind::State, Added };
			bSyncGraphSelection = true;
			MarkEdited(Label);
		};
		if (ImGui::MenuItem(ICON_FA_FILM " 클립 상태 추가"))
		{
			AddAt("State", false, "상태 추가");
		}
		if (ImGui::MenuItem(ICON_FA_ARROWS_LEFT_RIGHT " 1D 블렌드 스페이스 상태 추가"))
		{
			AddAt("Blend", true, "블렌드 스페이스 추가");
		}
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FA_EXPAND " 전체 보기", "F"))
		{
			NavigateFrames = 0;
		}
		ImGui::EndPopup();
	}
	NodeEditor::Resume();
}

void FAnimGraphEditor::SyncPositionsFromGraph()
{
	bool       bMoved = false;
	const auto Sync   = [&](uint32 Id, std::optional<FVector2>& Stored) {
        const ImVec2 Position = NodeEditor::GetNodePosition(ToNode(Id));
        if (Position.x == FLT_MAX)
        {
            return;
        }
        if (!Stored || FMath::Abs(Stored->X - Position.x) > 0.5f || FMath::Abs(Stored->Y - Position.y) > 0.5f)
        {
            Stored = FVector2(Position.x, Position.y);
            bMoved = true;
        }
	};
	Sync(AnyStateId, Asset.AnyStateEditorPosition);
	for (size_t Index = 0; Index < Asset.States.size(); ++Index)
	{
		Sync(StateNodeId(static_cast<int32>(Index)), Asset.States[Index].EditorPosition);
	}
	if (bMoved)
	{
		MarkEdited("노드 이동");
	}
}

void FAnimGraphEditor::MoveTransition(int32 Index, int32 Delta)
{
	// 같은 출발(From)의 전이 사이에서만 순서를 바꾼다 (다른 상태의 전이와는 서로 영향이 없어서)
	if (Index < 0 || Index >= static_cast<int32>(Asset.Transitions.size()))
	{
		return;
	}
	const int32 From = Asset.Transitions[static_cast<size_t>(Index)].From;
	for (int32 Other = Index + Delta; Other >= 0 && Other < static_cast<int32>(Asset.Transitions.size()); Other += Delta)
	{
		if (Asset.Transitions[static_cast<size_t>(Other)].From == From)
		{
			std::swap(Asset.Transitions[static_cast<size_t>(Index)], Asset.Transitions[static_cast<size_t>(Other)]);
			if (Selection.Kind == ESelectionKind::Transition && Selection.Index == Index)
			{
				Selection.Index     = Other;
				bSyncGraphSelection = true;
			}
			MarkEdited("전이 우선순위");
			return;
		}
	}
}

// ---------------------------------------------------------------- 오른쪽: 속성

void FAnimGraphEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	const FDebugView Debug = MakeDebugView(Env);
	DrawPreviewSection(Env, Debug);
	if (ImGui::CollapsingHeader(ICON_FA_SLIDERS " 파라미터", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawParameters();
	}
	if (bScrollToSelection)
	{
		ImGui::SetScrollHereY(0.0f);
		bScrollToSelection = false;
	}
	if (ImGui::CollapsingHeader(ICON_FA_DIAGRAM_PROJECT " 선택 항목", ImGuiTreeNodeFlags_DefaultOpen))
	{
		switch (Selection.Kind)
		{
		case ESelectionKind::State:
			if (Selection.Index >= 0 && Selection.Index < static_cast<int32>(Asset.States.size()))
			{
				DrawStateProperties(Selection.Index);
			}
			break;
		case ESelectionKind::Transition:
			if (Selection.Index >= 0 && Selection.Index < static_cast<int32>(Asset.Transitions.size()))
			{
				DrawTransitionProperties(Selection.Index);
			}
			break;
		case ESelectionKind::AnyState:
			ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.4f, 1.0f), ICON_FA_ASTERISK " 어느 상태든");
			FAssetEditorWidgets::Hint("여기서 나가는 전이는 지금 어느 상태에 있든 검사합니다 (도착 상태 자신은 제외). 사망/피격처럼 언제든 끼어드는 상태에 씁니다.");
			ImGui::SeparatorText("나가는 전이 (위가 먼저)");
			DrawTransitionList(-1, true);
			break;
		default:
			FAssetEditorWidgets::Hint("그래프에서 상태나 전이 화살표를 선택하세요.");
			ImGui::SeparatorText("모든 전이 (위가 먼저 검사)");
			DrawTransitionList(-1, false);
			break;
		}
	}
}

void FAnimGraphEditor::DrawPreviewSection(FAssetEditorEnvironment& Env, const FDebugView& Debug)
{
	(void)Env;
	if (!ImGui::CollapsingHeader(ICON_FA_PERSON_RUNNING " 미리보기", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	const std::string Current = Asset.PreviewModel.empty() ? "(자동) " + (AutoModel.empty() ? std::string("없음") : AutoModel) : Asset.PreviewModel;
	if (ImGui::BeginCombo("모델", Current.c_str()))
	{
		if (ImGui::Selectable("(자동)", Asset.PreviewModel.empty()))
		{
			Asset.PreviewModel.clear();
			MarkEdited("미리보기 모델");
		}
		for (const std::string& File : ModelFiles)
		{
			if (ImGui::Selectable(File.c_str(), File == Asset.PreviewModel))
			{
				Asset.PreviewModel = File;
				MarkEdited("미리보기 모델");
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("미리보기에만 쓰는 모델 (에셋에 저장). 그래프는 상태의 클립 이름으로 모델 클립을 찾는다");
	if (!LoadedModel.empty() && ClipNames.empty())
	{
		ImGui::TextColored(FEditorTheme::Warning, "이 모델에는 애니메이션 클립이 없습니다");
	}

	if (Debug.Runtime != nullptr)
	{
		const char* StateName = Debug.CurrentState >= 0 ? Asset.States[static_cast<size_t>(Debug.CurrentState)].Name.c_str() : "-";
		ImGui::TextColored(Debug.bPlaying ? FEditorTheme::Success : ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s %s: %s", Debug.bPlaying ? ICON_FA_PLAY : ICON_FA_EYE,
		                   Debug.Label.c_str(), StateName);
		for (const auto& [State, Weight] : Debug.Layers)
		{
			const std::string Label = std::format("{} {:.2f}", State >= 0 ? Asset.States[static_cast<size_t>(State)].Name : std::string("(삭제된 상태)"), Weight);
			ImGui::ProgressBar(Weight, ImVec2(-FLT_MIN, 0.0f), Label.c_str());
		}
		if (Debug.bPlaying)
		{
			// 플레이 중 엔티티의 실제 파라미터 값 (읽기 전용)
			for (const FAnimGraphParameter& Parameter : Asset.Parameters)
			{
				float Value = 0.0f;
				if (!Debug.Runtime->Parameters.TryGet(Parameter.Name, Value))
				{
					Value = Parameter.Default;
				}
				ImGui::TextDisabled("   %s = %s", Parameter.Name.c_str(),
				                    Parameter.Type == EAnimParamType::Bool ? (Value != 0.0f ? "true" : "false") : std::format("{:.2f}", Value).c_str());
			}
		}
	}
}

void FAnimGraphEditor::RenameParameter(const std::string& OldName, const std::string& NewName)
{
	for (FAnimGraphState& State : Asset.States)
	{
		if (State.BlendParameter == OldName)
		{
			State.BlendParameter = NewName;
		}
	}
	for (FAnimGraphTransition& Transition : Asset.Transitions)
	{
		for (FAnimTransitionCondition& Condition : Transition.Conditions)
		{
			if (Condition.Parameter == OldName)
			{
				Condition.Parameter = NewName;
			}
		}
	}
	for (auto& [Key, Value] : PreviewValues)
	{
		if (Key == OldName)
		{
			Key = NewName;
		}
	}
}

void FAnimGraphEditor::DrawParameters()
{
	FAssetEditorWidgets::Hint("미리보기 열 값은 아래 미리보기 재생에만 쓰인다 (저장 안 함). 게임에서는 Lua entity:SetAnimParam 또는 캐릭터 이동이 넣는다.");
	int32 RemoveIndex = -1;
	if (ImGui::BeginTable("##Parameters", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("종류", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("기본값", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("미리보기", ImGuiTableColumnFlags_WidthStretch, 1.3f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableHeadersRow();
		for (size_t Index = 0; Index < Asset.Parameters.size(); ++Index)
		{
			FAnimGraphParameter& Parameter = Asset.Parameters[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			const std::string OldName = Parameter.Name;
			std::string       NewName = Parameter.Name;
			if (InputString("##Name", NewName) && !NewName.empty())
			{
				Parameter.Name = NewName;
				RenameParameter(OldName, NewName);
				MarkEdited("파라미터 이름");
			}
			if (std::count_if(Asset.Parameters.begin(), Asset.Parameters.end(), [&](const FAnimGraphParameter& Other) { return Other.Name == Parameter.Name; }) > 1)
			{
				ImGui::SetItemTooltip("이름이 겹칩니다");
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ToU32(ImVec4(0.5f, 0.15f, 0.15f, 0.6f)));
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			int32 Type = Parameter.Type == EAnimParamType::Bool ? 1 : 0;
			if (ImGui::Combo("##Type", &Type, "Float\0Bool\0"))
			{
				Parameter.Type    = Type == 1 ? EAnimParamType::Bool : EAnimParamType::Float;
				Parameter.Default = Parameter.Type == EAnimParamType::Bool ? (Parameter.Default != 0.0f ? 1.0f : 0.0f) : Parameter.Default;
				MarkEdited("파라미터 종류");
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (Parameter.Type == EAnimParamType::Bool)
			{
				bool bValue = Parameter.Default != 0.0f;
				if (ImGui::Checkbox("##Default", &bValue))
				{
					Parameter.Default = bValue ? 1.0f : 0.0f;
					MarkEdited("파라미터 기본값");
				}
			}
			else if (ImGui::DragFloat("##Default", &Parameter.Default, 1.0f))
			{
				MarkEdited("파라미터 기본값");
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-FLT_MIN);
			float& Value = PreviewValue(Parameter.Name);
			if (Parameter.Type == EAnimParamType::Bool)
			{
				bool bValue = Value != 0.0f;
				if (ImGui::Checkbox("##Preview", &bValue))
				{
					Value = bValue ? 1.0f : 0.0f;
				}
			}
			else
			{
				ImGui::DragFloat("##Preview", &Value, 2.0f);
			}
			ImGui::TableNextColumn();
			if (ImGui::SmallButton(ICON_FA_XMARK))
			{
				RemoveIndex = static_cast<int32>(Index);
			}
			ImGui::SetItemTooltip("파라미터 삭제 (이 파라미터를 쓰는 조건/블렌드는 기본값 0으로 동작)");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (RemoveIndex >= 0)
	{
		Asset.Parameters.erase(Asset.Parameters.begin() + RemoveIndex);
		MarkEdited("파라미터 삭제");
	}
	if (ImGui::Button(ICON_FA_PLUS " 파라미터 추가"))
	{
		std::string Name = "Param";
		for (int32 Suffix = 1; Asset.FindParameter(Name) != nullptr; ++Suffix)
		{
			Name = std::format("Param{}", Suffix);
		}
		Asset.Parameters.push_back({ Name, EAnimParamType::Float, 0.0f });
		MarkEdited("파라미터 추가");
	}
}

bool FAnimGraphEditor::ClipCombo(const char* Id, std::string& Clip)
{
	const bool bKnown = Clip.empty() || std::find(ClipNames.begin(), ClipNames.end(), Clip) != ClipNames.end();
	if (!bKnown)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Warning);
	}
	bool bChanged = false;
	if (ImGui::BeginCombo(Id, Clip.empty() ? "(없음)" : Clip.c_str()))
	{
		for (const std::string& Name : ClipNames)
		{
			if (ImGui::Selectable(Name.c_str(), Name == Clip))
			{
				Clip     = Name;
				bChanged = true;
			}
		}
		ImGui::EndCombo();
	}
	if (!bKnown)
	{
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("미리보기 모델에 이 클립이 없습니다");
	}
	return bChanged;
}

bool FAnimGraphEditor::ParameterCombo(const char* Id, std::string& Parameter, bool bFloatOnly)
{
	bool bChanged = false;
	if (ImGui::BeginCombo(Id, Parameter.empty() ? "(없음)" : Parameter.c_str()))
	{
		for (const FAnimGraphParameter& Candidate : Asset.Parameters)
		{
			if (bFloatOnly && Candidate.Type != EAnimParamType::Float)
			{
				continue;
			}
			if (ImGui::Selectable(Candidate.Name.c_str(), Candidate.Name == Parameter))
			{
				Parameter = Candidate.Name;
				bChanged  = true;
			}
		}
		ImGui::EndCombo();
	}
	return bChanged;
}

void FAnimGraphEditor::DrawStateProperties(int32 Index)
{
	FAnimGraphState& State = Asset.States[static_cast<size_t>(Index)];
	std::string      Name  = State.Name;
	if (InputString("이름", Name))
	{
		// 비거나 겹치는 이름은 적용하지 않는다 (입력 중에는 그대로 보이고, 끝나면 원래 이름으로)
		if (!Name.empty() && Asset.FindState(Name) < 0)
		{
			State.Name = Name;
			MarkEdited("상태 이름");
		}
	}
	if (Index == Asset.EntryState)
	{
		ImGui::TextColored(FEditorTheme::Accent, ICON_FA_FLAG " 시작 상태");
	}
	else if (ImGui::SmallButton(ICON_FA_FLAG " 시작 상태로 지정"))
	{
		Asset.EntryState = Index;
		MarkEdited("시작 상태");
	}

	int32 Kind = IsBlendSpace(State) ? 1 : 0;
	if (ImGui::RadioButton("클립", &Kind, 0) && IsBlendSpace(State))
	{
		State.BlendParameter.clear();
		State.Samples.resize(1);
		MarkEdited("상태 종류");
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("1D 블렌드 스페이스", &Kind, 1) && !IsBlendSpace(State))
	{
		std::string Parameter;
		for (const FAnimGraphParameter& Candidate : Asset.Parameters)
		{
			if (Candidate.Type == EAnimParamType::Float)
			{
				Parameter = Candidate.Name;
				break;
			}
		}
		if (Parameter.empty())
		{
			Asset.Parameters.push_back({ "Speed", EAnimParamType::Float, 0.0f });
			Parameter = "Speed";
		}
		FAnimGraphState& Edited = Asset.States[static_cast<size_t>(Index)];
		Edited.BlendParameter   = Parameter;
		if (Edited.Samples.size() < 2)
		{
			Edited.Samples.push_back({ Edited.Samples.front().Clip, Edited.Samples.front().Position + 100.0f, 1.0f });
		}
		MarkEdited("상태 종류");
	}

	FAnimGraphState& Edited = Asset.States[static_cast<size_t>(Index)];
	if (!IsBlendSpace(Edited))
	{
		if (ClipCombo("클립", Edited.Samples.front().Clip))
		{
			MarkEdited("상태 클립");
		}
		if (ImGui::DragFloat("클립 배속", &Edited.Samples.front().Rate, 0.01f, 0.01f, 10.0f))
		{
			MarkEdited("클립 배속");
		}
	}
	else
	{
		if (ParameterCombo("블렌드 파라미터", Edited.BlendParameter, true))
		{
			MarkEdited("블렌드 파라미터");
		}
		DrawBlendSpaceAxis(Edited);
		int32 RemoveSample = -1;
		if (ImGui::BeginTable("##Samples", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("클립", ImGuiTableColumnFlags_WidthStretch, 1.6f);
			ImGui::TableSetupColumn("위치", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("배속", ImGuiTableColumnFlags_WidthStretch, 0.8f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
			ImGui::TableHeadersRow();
			for (size_t Sample = 0; Sample < Edited.Samples.size(); ++Sample)
			{
				FAnimBlendSample& Item = Edited.Samples[Sample];
				ImGui::PushID(static_cast<int>(Sample));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ClipCombo("##Clip", Item.Clip))
				{
					MarkEdited("샘플 클립");
				}
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragFloat("##Position", &Item.Position, 1.0f))
				{
					MarkEdited("샘플 위치");
				}
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragFloat("##Rate", &Item.Rate, 0.01f, 0.01f, 10.0f))
				{
					MarkEdited("샘플 배속");
				}
				ImGui::TableNextColumn();
				ImGui::BeginDisabled(Edited.Samples.size() <= 1);
				if (ImGui::SmallButton(ICON_FA_XMARK))
				{
					RemoveSample = static_cast<int32>(Sample);
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (RemoveSample >= 0)
		{
			Edited.Samples.erase(Edited.Samples.begin() + RemoveSample);
			MarkEdited("샘플 삭제");
		}
		if (ImGui::SmallButton(ICON_FA_PLUS " 샘플 추가"))
		{
			float Last = 0.0f;
			for (const FAnimBlendSample& Sample : Edited.Samples)
			{
				Last = FMath::Max(Last, Sample.Position);
			}
			Edited.Samples.push_back({ ClipNames.empty() ? std::string() : ClipNames.front(), Last + 100.0f, 1.0f });
			MarkEdited("샘플 추가");
		}
	}
	if (ImGui::DragFloat("상태 배속", &Edited.Speed, 0.01f, 0.0f, 10.0f))
	{
		MarkEdited("상태 배속");
	}
	if (ImGui::Checkbox("반복", &Edited.bLoop))
	{
		MarkEdited("반복");
	}
	ImGui::SeparatorText("나가는 전이 (위가 먼저 검사)");
	DrawTransitionList(Index, true);
}

void FAnimGraphEditor::DrawBlendSpaceAxis(FAnimGraphState& State)
{
	if (State.Samples.empty())
	{
		return;
	}
	// 축 범위 = 샘플 위치 ± 8% (끄는 동안은 고정)
	float Lo = State.Samples.front().Position;
	float Hi = Lo;
	for (const FAnimBlendSample& Sample : State.Samples)
	{
		Lo = FMath::Min(Lo, Sample.Position);
		Hi = FMath::Max(Hi, Sample.Position);
	}
	if (Hi - Lo < 1.0e-3f)
	{
		Lo -= 100.0f;
		Hi += 100.0f;
	}
	const float Pad = (Hi - Lo) * 0.08f;
	Lo -= Pad;
	Hi += Pad;
	if (AxisDragSample != -1)
	{
		Lo = AxisLo;
		Hi = AxisHi;
	}

	const float  Width  = ImGui::GetContentRegionAvail().x;
	const float  Height = 70.0f;
	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##BlendAxis", ImVec2(Width, Height));
	const bool  bHovered = ImGui::IsItemHovered();
	ImDrawList* Draw     = ImGui::GetWindowDrawList();
	const float AxisY    = Origin.y + Height * 0.55f;
	const auto  ToX      = [&](float Value) { return Origin.x + 8.0f + (Value - Lo) / (Hi - Lo) * (Width - 16.0f); };
	const auto  ToValue  = [&](float X) { return Lo + (X - Origin.x - 8.0f) / FMath::Max(Width - 16.0f, 1.0f) * (Hi - Lo); };

	Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + Height), IM_COL32(28, 30, 34, 255), 4.0f);
	Draw->AddLine(ImVec2(ToX(Lo), AxisY), ImVec2(ToX(Hi), AxisY), IM_COL32(150, 150, 150, 255), 2.0f);

	// 현재 미리보기 값의 샘플 가중치 (위치순 정렬본으로 계산 → 원래 번호로)
	float&              Value = PreviewValue(State.BlendParameter);
	std::vector<size_t> Order(State.Samples.size());
	for (size_t Index = 0; Index < Order.size(); ++Index)
	{
		Order[Index] = Index;
	}
	std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return State.Samples[A].Position < State.Samples[B].Position; });
	std::vector<float> Positions;
	for (size_t Index : Order)
	{
		Positions.push_back(State.Samples[Index].Position);
	}
	std::vector<float> SortedWeights;
	AnimGraphMath::ComputeBlendSpace1DWeights(Positions, Value, SortedWeights);
	std::vector<float> Weights(State.Samples.size(), 0.0f);
	for (size_t Index = 0; Index < Order.size(); ++Index)
	{
		Weights[Order[Index]] = SortedWeights[Index];
	}

	int32       Hovered = -1;
	const float MouseX  = ImGui::GetIO().MousePos.x;
	for (size_t Index = 0; Index < State.Samples.size(); ++Index)
	{
		const float X = ToX(State.Samples[Index].Position);
		if (bHovered && FMath::Abs(MouseX - X) < 7.0f)
		{
			Hovered = static_cast<int32>(Index);
		}
	}
	for (size_t Rank = 0; Rank < Order.size(); ++Rank)
	{
		const size_t Index  = Order[Rank];
		const float  X      = ToX(State.Samples[Index].Position);
		const bool   bHot   = Hovered == static_cast<int32>(Index) || AxisDragSample == static_cast<int32>(Index);
		const ImU32  Color  = bHot ? IM_COL32(255, 255, 255, 255) : ToU32(FEditorTheme::Accent);
		const float  Radius = 6.0f;
		Draw->AddQuadFilled(ImVec2(X, AxisY - Radius), ImVec2(X + Radius, AxisY), ImVec2(X, AxisY + Radius), ImVec2(X - Radius, AxisY), Color);
		// 가중치 막대 (축 아래) + 이름 (위, 번갈아 높이)
		const float Bar = Weights[Index] * 18.0f;
		if (Bar > 0.5f)
		{
			Draw->AddRectFilled(ImVec2(X - 3.0f, AxisY + 8.0f), ImVec2(X + 3.0f, AxisY + 8.0f + Bar), ToU32(FEditorTheme::Success));
		}
		const std::string Label     = State.Samples[Index].Clip.empty() ? std::string("?") : State.Samples[Index].Clip;
		const ImVec2      LabelSize = ImGui::CalcTextSize(Label.c_str());
		const float       LabelY    = Origin.y + 2.0f + static_cast<float>(Rank % 2) * (LabelSize.y - 2.0f);
		Draw->AddText(ImVec2(FMath::Clamp(X - LabelSize.x * 0.5f, Origin.x + 2.0f, Origin.x + Width - LabelSize.x - 2.0f), LabelY),
		              IM_COL32(220, 220, 220, 255), Label.c_str());
	}
	const float ValueX = FMath::Clamp(ToX(Value), Origin.x + 2.0f, Origin.x + Width - 2.0f);
	Draw->AddLine(ImVec2(ValueX, AxisY - 14.0f), ImVec2(ValueX, AxisY + 14.0f), ToU32(FEditorTheme::Danger), 2.0f);

	// 끌기: 샘플 마름모 = 위치 바꾸기, 빈 곳 = 미리보기 값
	if (ImGui::IsItemActivated())
	{
		AxisDragSample = Hovered >= 0 ? Hovered : -2;
		AxisLo         = Lo;
		AxisHi         = Hi;
	}
	if (ImGui::IsItemActive() && AxisDragSample != -1)
	{
		const float Dragged = ToValue(MouseX);
		if (AxisDragSample >= 0 && AxisDragSample < static_cast<int32>(State.Samples.size()))
		{
			State.Samples[static_cast<size_t>(AxisDragSample)].Position = std::round(Dragged);
			MarkEdited("샘플 위치");
		}
		else
		{
			Value = Dragged;
		}
	}
	else if (!ImGui::IsItemActive())
	{
		AxisDragSample = -1;
	}
	if (bHovered)
	{
		ImGui::SetTooltip("마름모를 끌어 샘플 위치 변경 · 빈 곳을 끌어 미리보기 값 (%s = %.1f)", State.BlendParameter.c_str(), Value);
	}
	ImGui::TextDisabled("범위 %.1f ~ %.1f · 현재 %.1f (빨간 선)", Lo + Pad, Hi - Pad, Value);
}

void FAnimGraphEditor::DrawTransitionList(int32 FromState, bool bOnlyFrom)
{
	std::vector<int32> Shown;
	for (size_t Index = 0; Index < Asset.Transitions.size(); ++Index)
	{
		if (!bOnlyFrom || Asset.Transitions[Index].From == FromState)
		{
			Shown.push_back(static_cast<int32>(Index));
		}
	}
	if (Shown.empty())
	{
		ImGui::TextDisabled("(없음)");
		return;
	}
	const auto StateName = [this](int32 State) {
		return State < 0 ? std::string("*") : State < static_cast<int32>(Asset.States.size()) ? Asset.States[static_cast<size_t>(State)].Name : std::string("?");
	};
	for (size_t Row = 0; Row < Shown.size(); ++Row)
	{
		const int32                 Index      = Shown[Row];
		const FAnimGraphTransition& Transition = Asset.Transitions[static_cast<size_t>(Index)];
		ImGui::PushID(Index);
		const std::string Label = std::format("{}. {} → {}  (조건 {}개{}, {:.2f}초)", Row + 1, StateName(Transition.From), StateName(Transition.To),
		                                      Transition.Conditions.size(), Transition.ExitTime >= 0.0f ? ", 종료 시점" : "", Transition.Duration);
		const bool bSelected = Selection.Kind == ESelectionKind::Transition && Selection.Index == Index;
		if (ImGui::Selectable(Label.c_str(), bSelected, 0, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 2.4f, 0.0f)))
		{
			Selection           = { ESelectionKind::Transition, Index };
			bSyncGraphSelection = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_ARROW_UP))
		{
			MoveTransition(Index, -1);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
		{
			MoveTransition(Index, 1);
		}
		ImGui::PopID();
	}
}

void FAnimGraphEditor::DrawTransitionProperties(int32 Index)
{
	FAnimGraphTransition& Transition = Asset.Transitions[static_cast<size_t>(Index)];
	const auto            StateLabel = [this](int32 State) {
        return State < 0 ? std::string(ICON_FA_ASTERISK " 어느 상태든") : Asset.States[static_cast<size_t>(State)].Name;
	};
	if (ImGui::BeginCombo("출발", StateLabel(Transition.From).c_str()))
	{
		for (int32 State = -1; State < static_cast<int32>(Asset.States.size()); ++State)
		{
			if (State != Transition.To && ImGui::Selectable(StateLabel(State).c_str(), State == Transition.From))
			{
				Transition.From = State;
				MarkEdited("전이 출발");
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::BeginCombo("도착", StateLabel(Transition.To).c_str()))
	{
		for (int32 State = 0; State < static_cast<int32>(Asset.States.size()); ++State)
		{
			if (State != Transition.From && ImGui::Selectable(StateLabel(State).c_str(), State == Transition.To))
			{
				Transition.To = State;
				MarkEdited("전이 도착");
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::DragFloat("크로스페이드 (초)", &Transition.Duration, 0.01f, 0.0f, 5.0f))
	{
		MarkEdited("크로스페이드");
	}
	bool bExitTime = Transition.ExitTime >= 0.0f;
	if (ImGui::Checkbox("종료 시점 조건", &bExitTime))
	{
		Transition.ExitTime = bExitTime ? 0.9f : -1.0f;
		MarkEdited("종료 시점");
	}
	ImGui::SetItemTooltip("켜면 출발 상태 재생 진행률(0~1)이 이 값 이상일 때만 전이한다");
	if (Transition.ExitTime >= 0.0f && ImGui::SliderFloat("진행률 ≥", &Transition.ExitTime, 0.0f, 1.0f, "%.2f"))
	{
		MarkEdited("종료 시점");
	}

	// 우선순위: 같은 출발 전이 중 몇 번째인지 (목록 순서대로 검사, 처음 맞는 하나만)
	int32 Rank  = 0;
	int32 Count = 0;
	for (size_t Other = 0; Other < Asset.Transitions.size(); ++Other)
	{
		if (Asset.Transitions[Other].From == Transition.From)
		{
			++Count;
			if (static_cast<int32>(Other) <= Index)
			{
				++Rank;
			}
		}
	}
	ImGui::TextWrapped("우선순위: %s에서 나가는 전이 %d개 중 %d번째", Transition.From < 0 ? "어느 상태든" : Asset.States[static_cast<size_t>(Transition.From)].Name.c_str(), Count,
	            Rank);
	ImGui::TextDisabled("검사 순서");
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_ARROW_UP "##Up"))
	{
		MoveTransition(Index, -1);
		return;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "##Down"))
	{
		MoveTransition(Index, 1);
		return;
	}

	ImGui::SeparatorText("조건 (모두 참이어야 전이)");
	FAnimGraphTransition& Edited          = Asset.Transitions[static_cast<size_t>(Index)];
	int32                 RemoveCondition = -1;
	for (size_t ConditionIndex = 0; ConditionIndex < Edited.Conditions.size(); ++ConditionIndex)
	{
		FAnimTransitionCondition&  Condition = Edited.Conditions[ConditionIndex];
		const FAnimGraphParameter* Parameter = Asset.FindParameter(Condition.Parameter);
		const bool                 bBool     = Parameter != nullptr && Parameter->Type == EAnimParamType::Bool;
		ImGui::PushID(static_cast<int>(ConditionIndex));
		const float Column = ImGui::GetContentRegionAvail().x;
		ImGui::SetNextItemWidth(Column * 0.34f);
		if (ParameterCombo("##Parameter", Condition.Parameter, false))
		{
			MarkEdited("조건 파라미터");
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(Column * 0.22f);
		int32 Op = static_cast<int32>(Condition.Op);
		if (ImGui::Combo("##Op", &Op, OpLabels, IM_ARRAYSIZE(OpLabels)))
		{
			Condition.Op = static_cast<EAnimConditionOp>(Op);
			MarkEdited("조건 비교");
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(Column * 0.26f);
		if (bBool)
		{
			bool bValue = Condition.Value != 0.0f;
			if (ImGui::Checkbox(bValue ? "참###Value" : "거짓###Value", &bValue))
			{
				Condition.Value = bValue ? 1.0f : 0.0f;
				MarkEdited("조건 값");
			}
		}
		else if (ImGui::DragFloat("##Value", &Condition.Value, 1.0f))
		{
			MarkEdited("조건 값");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_XMARK))
		{
			RemoveCondition = static_cast<int32>(ConditionIndex);
		}
		if (Parameter == nullptr)
		{
			ImGui::TextColored(FEditorTheme::Warning, "   선언되지 않은 파라미터 (항상 0)");
		}
		ImGui::PopID();
	}
	if (RemoveCondition >= 0)
	{
		Edited.Conditions.erase(Edited.Conditions.begin() + RemoveCondition);
		MarkEdited("조건 삭제");
	}
	if (ImGui::SmallButton(ICON_FA_PLUS " 조건 추가"))
	{
		FAnimTransitionCondition Condition;
		if (!Asset.Parameters.empty())
		{
			const FAnimGraphParameter& First = Asset.Parameters.front();
			Condition.Parameter              = First.Name;
			Condition.Op                     = First.Type == EAnimParamType::Bool ? EAnimConditionOp::Equal : EAnimConditionOp::Greater;
			Condition.Value                  = First.Type == EAnimParamType::Bool ? 1.0f : 0.0f;
		}
		Edited.Conditions.push_back(Condition);
		MarkEdited("조건 추가");
	}
	if (Edited.Conditions.empty() && Edited.ExitTime < 0.0f)
	{
		ImGui::TextColored(FEditorTheme::Warning, "조건이 없으면 출발 상태에 들어가자마자 전이합니다");
	}
	ImGui::Spacing();
	if (ImGui::Button(ICON_FA_TRASH " 전이 삭제"))
	{
		Asset.Transitions.erase(Asset.Transitions.begin() + Index);
		Selection           = {};
		bSyncGraphSelection = true;
		MarkEdited("전이 삭제");
	}
}
