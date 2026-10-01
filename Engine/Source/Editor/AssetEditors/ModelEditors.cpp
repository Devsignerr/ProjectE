#include "Editor/AssetEditors/ModelEditors.h"

#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Core/StringConv.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StaticMesh.h"
#include "Scene/AnimationSystem.h"
#include "Scene/ModelMetadata.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <unordered_set>

bool FModelEditorBase::HasAnimations(const std::filesystem::path& Path, FResourceManager& Resources)
{
	const FModelResources* ModelResources = FModelLoader::LoadModelResources(Path, Resources);
	return ModelResources != nullptr && !ModelResources->Model.Animations.empty();
}

bool FModelEditorBase::LoadAsset(FAssetEditorEnvironment& Env)
{
	// 모델 파일은 편집하지 않으므로 처음 한 번만 배치한다 (되돌리기에서 다시 불려도 그대로)
	FScene& Scene = Preview.GetScene();
	if (!Scene.GetRegistry().IsValid(ModelRoot))
	{
		ModelRoot = FModelLoader::LoadIntoScene(Path, Scene, *Env.Resources);
		if (!Scene.GetRegistry().IsValid(ModelRoot))
		{
			return false;
		}
		Scene.UpdateTransforms();
	}
	Model = FModelLoader::LoadModelResources(Path, *Env.Resources);
	if (Model == nullptr)
	{
		return false;
	}
	// 열 때/되돌릴 때 공유 메타데이터(.emeta)를 파일 내용으로 맞춘다
	Metadata  = Model->Metadata;
	*Metadata = FModelMetadata::LoadForSource(Path);
	SelectedSocket = std::min(SelectedSocket, static_cast<int32>(Metadata->Sockets.size()) - 1);
	return true;
}

bool FModelEditorBase::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return Metadata && Metadata->SaveForSource(Path);
}

std::string FModelEditorBase::CaptureState() const
{
	return Metadata ? Metadata->ToJsonString() : std::string();
}

void FModelEditorBase::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	if (Metadata && Metadata->FromJsonString(State))
	{
		SelectedSocket = std::min(SelectedSocket, static_cast<int32>(Metadata->Sockets.size()) - 1);
		SocketNameFor  = -1;
		SocketEulerFor = -1;
	}
}

void FModelEditorBase::DrawPreviewOverlay(FAssetEditorEnvironment& Env)
{
	(void)Env;
	DrawSocketOverlay();
}

void FModelEditorBase::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FAssetEditor::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::Checkbox("와이어프레임", &bWireframe);
}

void FModelEditorBase::RenderPreview(FAssetEditorEnvironment& Env)
{
	// 전용 렌더러를 여러 창이 공유하므로 이 창을 그리는 동안만 켠다
	Env.PreviewRenderer->bWireframe = bWireframe;
	FAssetEditor::RenderPreview(Env);
	Env.PreviewRenderer->bWireframe = false;
}

void FModelEditorBase::DrawModelInfo(FAssetEditorEnvironment& Env)
{
	if (Model == nullptr)
	{
		return;
	}
	const FModelData& Data = Model->Model;

	uint32 TotalVertices  = 0;
	uint32 TotalTriangles = 0;
	uint32 SkinnedMeshes  = 0;
	for (const FMeshHandle Handle : Model->Meshes)
	{
		if (const FStaticMesh* Mesh = Env.Resources->GetMesh(Handle))
		{
			TotalVertices += Mesh->GetVertexCount();
			TotalTriangles += Mesh->GetIndexCount() / 3;
			SkinnedMeshes += Mesh->IsSkinned() ? 1u : 0u;
		}
	}
	size_t JointCount = 0;
	for (const FModelSkin& Skin : Data.Skins)
	{
		JointCount += Skin.Joints.size();
	}

	ImGui::SeparatorText("요약");
	if (ImGui::BeginTable("##Summary", 2, ImGuiTableFlags_SizingStretchProp))
	{
		const auto Row = [](const char* Label, const std::string& Value) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", Label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Value.c_str());
		};
		Row("노드", std::to_string(Data.Nodes.size()));
		Row("메시", std::to_string(Data.Meshes.size()) + (SkinnedMeshes > 0 ? " (스킨 " + std::to_string(SkinnedMeshes) + ")" : ""));
		Row("정점", std::to_string(TotalVertices));
		Row("삼각형", std::to_string(TotalTriangles));
		Row("머티리얼", std::to_string(Data.Materials.size()));
		Row("텍스처", std::to_string(Model->TextureCount));
		Row("스킨 / 관절", std::to_string(Data.Skins.size()) + " / " + std::to_string(JointCount));
		Row("애니메이션", std::to_string(Data.Animations.size()));

		const FBox Bounds = Preview.ComputeMeshBounds(*Env.Resources);
		if (Bounds.IsValid())
		{
			const FVector3 Size = Bounds.GetSize();
			char           Text[96];
			std::snprintf(Text, sizeof(Text), "%.1f x %.1f x %.1f cm", Size.X, Size.Y, Size.Z);
			Row("크기 (바인드 포즈)", Text);
		}
		ImGui::EndTable();
	}

	if (ImGui::CollapsingHeader("메시 목록", ImGuiTreeNodeFlags_DefaultOpen) &&
	    ImGui::BeginTable("##Meshes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("이름");
		ImGui::TableSetupColumn("정점");
		ImGui::TableSetupColumn("삼각형");
		ImGui::TableSetupColumn("머티리얼");
		ImGui::TableHeadersRow();
		for (size_t Index = 0; Index < Data.Meshes.size(); ++Index)
		{
			const FModelMesh&  MeshData = Data.Meshes[Index];
			const FStaticMesh* Mesh     = Index < Model->Meshes.size() ? Env.Resources->GetMesh(Model->Meshes[Index]) : nullptr;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(MeshData.Name.empty() ? "(이름 없음)" : MeshData.Name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%u", Mesh ? Mesh->GetVertexCount() : 0u);
			ImGui::TableNextColumn();
			ImGui::Text("%u", Mesh ? Mesh->GetIndexCount() / 3 : 0u);
			ImGui::TableNextColumn();
			const bool bHasMaterial = MeshData.Material >= 0 && MeshData.Material < static_cast<int32>(Data.Materials.size());
			ImGui::TextUnformatted(bHasMaterial ? Data.Materials[MeshData.Material].Name.c_str() : "(기본)");
		}
		ImGui::EndTable();
	}

	if (ImGui::CollapsingHeader("머티리얼 (모델 내장)", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t Index = 0; Index < Data.Materials.size(); ++Index)
		{
			const FModelMaterial& Material = Data.Materials[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::ColorButton("##Color", ImVec4(Material.BaseColorFactor.X, Material.BaseColorFactor.Y, Material.BaseColorFactor.Z, 1.0f),
			                   ImGuiColorEditFlags_NoTooltip);
			ImGui::SameLine();
			const int32 TextureCount = (Material.BaseColorImage >= 0) + (Material.MetallicRoughnessImage >= 0) + (Material.NormalImage >= 0) +
			                           (Material.OcclusionImage >= 0) + (Material.EmissiveImage >= 0);
			ImGui::Text("%s", Material.Name.empty() ? "(이름 없음)" : Material.Name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("금속 %.2f / 거칠기 %.2f / 텍스처 %d", Material.MetallicFactor, Material.RoughnessFactor, TextureCount);
			ImGui::PopID();
		}
	}
}

void FModelEditorBase::DrawAddToScene(FAssetEditorEnvironment& Env)
{
	if (Env.Editor == nullptr || Env.Editor->Scene == nullptr)
	{
		return;
	}
	if (ImGui::Button("열린 씬에 추가"))
	{
		FEditorContext& Editor = *Env.Editor;
		const FEntity   Root   = FModelLoader::LoadIntoScene(Path, *Editor.Scene, *Env.Resources);
		if (Editor.Scene->GetRegistry().IsValid(Root))
		{
			Editor.Scene->UpdateTransforms();
			Editor.Select(Root);
			Editor.MarkEdited("모델 추가");
		}
	}
}

// ---------------------------------------------------------------- 스태틱 메시

void FStaticMeshEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	DrawImportSettings(Env);
	DrawSockets();
	DrawAddToScene(Env);
	DrawModelInfo(Env);
}

// ---------------------------------------------------------------- 애니메이션

bool FAnimationEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	if (!FModelEditorBase::LoadAsset(Env))
	{
		return false;
	}
	FScene& Scene = Preview.GetScene();
	ClipNames     = FAnimationSystem::GetClipNames(Scene, ModelRoot);

	// 뼈대 표시용 관절 (모든 스킨의 합집합)
	std::unordered_set<FEntity> Unique;
	Joints.clear();
	Scene.GetRegistry().View<FSkinComponent>().Each([&](FEntity, FSkinComponent& Skin) {
		for (FEntity Joint : Skin.Joints)
		{
			if (Unique.insert(Joint).second)
			{
				Joints.push_back(Joint);
			}
		}
	});
	// 첫 포즈를 미리 써 둔다 (경계 계산/첫 화면)
	FAnimationSystem::Update(Scene, 0.0f);
	Scene.UpdateTransforms();
	return true;
}

void FAnimationEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	(void)Env;
	FScene& Scene = Preview.GetScene();
	FAnimationSystem::Update(Scene, DeltaSeconds); // 일시정지면 현재 시각 포즈만 다시 쓴다
	Scene.UpdateTransforms();
	// 발생한 노티파이를 트랙 강조/최근 목록에 (Tick은 매 프레임이라 제외)
	if (const FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(ModelRoot))
	{
		for (const FAnimNotifyEvent& Event : Animation->Runtime.PendingNotifies)
		{
			if (Event.Type != EAnimNotifyEventType::StateTick)
			{
				RecentEvents.push_back({ Event, ImGui::GetTime() });
			}
		}
		while (RecentEvents.size() > 8)
		{
			RecentEvents.pop_front();
		}
	}
}

void FAnimationEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FModelEditorBase::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::Checkbox("뼈대", &bShowBones);
}

void FAnimationEditor::DrawPreviewOverlay(FAssetEditorEnvironment& Env)
{
	if (bShowBones)
	{
		DrawBones();
	}
	FModelEditorBase::DrawPreviewOverlay(Env);
}

void FAnimationEditor::DrawBones()
{
	FScene&       Scene    = Preview.GetScene();
	FRegistry&    Registry = Scene.GetRegistry();
	ImDrawList*   DrawList = ImGui::GetWindowDrawList();
	const ImVec2  ClipMin(Preview.GetViewportMin().X, Preview.GetViewportMin().Y);
	const ImVec2  ClipMax(Preview.GetViewportMax().X, Preview.GetViewportMax().Y);
	const std::unordered_set<FEntity> JointSet(Joints.begin(), Joints.end());

	DrawList->PushClipRect(ClipMin, ClipMax, true);
	for (FEntity Joint : Joints)
	{
		if (!Registry.IsValid(Joint))
		{
			continue;
		}
		FVector2 JointScreen;
		if (!Preview.ProjectToScreen(Scene.GetTransform(Joint).GetWorldPosition(), JointScreen))
		{
			continue;
		}
		const FEntity Parent = Scene.GetParent(Joint);
		FVector2      ParentScreen;
		if (JointSet.contains(Parent) && Preview.ProjectToScreen(Scene.GetTransform(Parent).GetWorldPosition(), ParentScreen))
		{
			DrawList->AddLine(ImVec2(ParentScreen.X, ParentScreen.Y), ImVec2(JointScreen.X, JointScreen.Y), IM_COL32(255, 210, 90, 220), 2.0f);
		}
		DrawList->AddCircleFilled(ImVec2(JointScreen.X, JointScreen.Y), 3.0f, IM_COL32(255, 140, 60, 255));
	}
	DrawList->PopClipRect();
}

void FAnimationEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	FScene&              Scene     = Preview.GetScene();
	FAnimationComponent* Animation = Scene.GetRegistry().TryGet<FAnimationComponent>(ModelRoot);
	if (Animation == nullptr || ClipNames.empty())
	{
		ImGui::TextDisabled("애니메이션 클립이 없습니다");
		DrawModelInfo(Env);
		return;
	}

	ImGui::SeparatorText("클립");
	const std::string Current = FAnimationSystem::GetCurrentClip(Scene, ModelRoot);
	if (ImGui::BeginListBox("##Clips", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * std::min<float>(6.0f, static_cast<float>(ClipNames.size()) + 0.5f))))
	{
		for (const std::string& Name : ClipNames)
		{
			if (ImGui::Selectable(Name.c_str(), Name == Current))
			{
				FAnimationSystem::Play(Scene, ModelRoot, Name, 0.0f);
				FAnimationSystem::SetTime(Scene, ModelRoot, 0.0f);
			}
		}
		ImGui::EndListBox();
	}

	ImGui::SeparatorText("재생");
	const float Duration = FAnimationSystem::GetCurrentClipDuration(Scene, ModelRoot);
	float       Time     = FAnimationSystem::GetTime(Scene, ModelRoot);
	if (ImGui::Button(Animation->bPlaying ? "일시정지" : "재생"))
	{
		Animation->bPlaying ? FAnimationSystem::Stop(Scene, ModelRoot) : FAnimationSystem::Resume(Scene, ModelRoot);
	}
	ImGui::SameLine();
	if (ImGui::Button("처음으로"))
	{
		FAnimationSystem::SetTime(Scene, ModelRoot, 0.0f);
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##PrevFrame", ImGuiDir_Left))
	{
		FAnimationSystem::Stop(Scene, ModelRoot);
		FAnimationSystem::SetTime(Scene, ModelRoot, Time - StepSeconds);
	}
	ImGui::SetItemTooltip("한 프레임 뒤로");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##NextFrame", ImGuiDir_Right))
	{
		FAnimationSystem::Stop(Scene, ModelRoot);
		FAnimationSystem::SetTime(Scene, ModelRoot, Time + StepSeconds);
	}
	ImGui::SetItemTooltip("한 프레임 앞으로");

	// 타임라인: 끌면 일시정지하고 그 시각 포즈를 보여준다
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::SliderFloat("##Timeline", &Time, 0.0f, FMath::Max(Duration, 0.001f), "%.2f초"))
	{
		FAnimationSystem::Stop(Scene, ModelRoot);
		FAnimationSystem::SetTime(Scene, ModelRoot, Time);
	}
	const int32 Frame      = static_cast<int32>(std::floor(Time / StepSeconds + 0.5f));
	const int32 FrameCount = static_cast<int32>(std::floor(Duration / StepSeconds + 0.5f));
	ImGui::TextDisabled("프레임 %d / %d, 길이 %.2f초", Frame, FrameCount, Duration);
	DrawNotifyTrack(Current, Duration, FAnimationSystem::GetTime(Scene, ModelRoot));

	ImGui::DragFloat("속도", &Animation->Speed, 0.01f, 0.0f, 5.0f, "%.2fx");
	ImGui::Checkbox("반복", &Animation->bLoop);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("재생 설정은 미리보기에만 적용됩니다 (씬에서는 애니메이션 컴포넌트에서 지정).");
	ImGui::PopStyleColor();

	ImGui::Spacing();
	DrawAddToScene(Env);
	DrawImportSettings(Env);
	DrawSockets();
	if (ImGui::CollapsingHeader("모델 정보"))
	{
		DrawModelInfo(Env);
	}
}

// ---------------------------------------------------------------- 임포트 설정

void FModelEditorBase::RebuildPreview(FAssetEditorEnvironment& Env)
{
	// 다시 가져오면 리소스(공유 메타데이터 포함)가 새로 만들어진다 → 저장 안 한 편집을 새 객체에 옮긴다
	const std::string Unsaved = IsDirty() ? CaptureState() : std::string();
	FScene& Scene = Preview.GetScene();
	if (Scene.GetRegistry().IsValid(ModelRoot))
	{
		Scene.DestroyEntity(ModelRoot);
	}
	ModelRoot = FEntity{};
	Model     = nullptr;
	LoadAsset(Env);
	if (!Unsaved.empty())
	{
		RestoreState(Env, Unsaved);
	}
	FramePreview(Env);
}

void FModelEditorBase::DrawImportSettings(FAssetEditorEnvironment& Env)
{
	if (!bImportSettingsLoaded)
	{
		SavedImportSettings   = FModelImportSettings::LoadForSource(Path);
		ImportSettings        = SavedImportSettings;
		bImportSettingsLoaded = true;
	}
	if (!ImGui::CollapsingHeader(ICON_FA_FILE_IMPORT " 임포트 설정", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	FModelImportSettings& S = ImportSettings;
	ImGui::DragFloat("크기 배율", &S.Scale, 0.01f, 0.001f, 1000.0f, "%.3f");
	ImGui::SetItemTooltip("원본 단위를 cm로 바꾼 뒤 곱하는 값 (너무 크거나 작게 들어올 때)");
	ImGui::SliderFloat("방향 (도)", &S.YawDegrees, -180.0f, 180.0f, "%.0f");
	ImGui::SetItemTooltip("위쪽 축 기준 회전 — 모델이 엉뚱한 쪽을 볼 때");
	for (const float Preset : { -90.0f, 0.0f, 90.0f, 180.0f })
	{
		ImGui::SameLine();
		if (ImGui::SmallButton(std::format("{:.0f}", Preset).c_str()))
		{
			S.YawDegrees = Preset;
		}
	}
	ImGui::Checkbox("머티리얼·텍스처 가져오기", &S.bImportMaterials);
	ImGui::Checkbox("스킨(뼈대 변형) 가져오기", &S.bImportSkin);
	ImGui::Checkbox("애니메이션 가져오기", &S.bImportAnimations);
	ImGui::Checkbox("법선 다시 계산", &S.bRecomputeNormals);
	ImGui::SameLine();
	ImGui::Checkbox("탄젠트 다시 계산", &S.bRecomputeTangents);
	ImGui::Checkbox("LOD 자동 생성", &S.bGenerateLods);
	if (S.bGenerateLods)
	{
		ImGui::SameLine();
		int32 LodCount = static_cast<int32>(S.LodCount);
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::SliderInt("LOD 단계", &LodCount, 1, 4))
		{
			S.LodCount = static_cast<uint32>(LodCount);
		}
	}

	// 추가 애니메이션 파일 (같은 뼈대 이름을 가진 다른 모델 파일의 클립)
	ImGui::TextUnformatted("추가 애니메이션 파일");
	for (size_t Index = 0; Index < S.AnimationSources.size(); ++Index)
	{
		ImGui::PushID(static_cast<int>(Index));
		if (ImGui::SmallButton(ICON_FA_XMARK))
		{
			S.AnimationSources.erase(S.AnimationSources.begin() + static_cast<std::ptrdiff_t>(Index));
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(S.AnimationSources[Index].c_str());
		ImGui::PopID();
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##AddAnimation", ICON_FA_PLUS " 애니메이션 파일 추가"))
	{
		std::error_code ErrorCode;
		const std::filesystem::path Root = Env.Editor != nullptr ? Env.Editor->ContentDirectory : Path.parent_path();
		for (auto It = std::filesystem::recursive_directory_iterator(Root, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
		     It.increment(ErrorCode))
		{
			if (!It->is_regular_file(ErrorCode) || !FModelLoader::IsModelFile(It->path()) || std::filesystem::equivalent(It->path(), Path, ErrorCode))
			{
				continue;
			}
			const std::string Relative = FStringConv::ToUtf8(std::filesystem::relative(It->path(), Path.parent_path(), ErrorCode).generic_wstring());
			if (!ErrorCode && ImGui::Selectable(Relative.c_str()) &&
			    std::find(S.AnimationSources.begin(), S.AnimationSources.end(), Relative) == S.AnimationSources.end())
			{
				S.AnimationSources.push_back(Relative);
			}
		}
		ImGui::EndCombo();
	}

	const bool bChanged = S.ToJsonString() != SavedImportSettings.ToJsonString();
	const bool bPlaying = Env.Editor != nullptr && Env.Editor->bPlaying;
	ImGui::BeginDisabled(!bChanged || bPlaying);
	ImGui::PushStyleColor(ImGuiCol_Button, bChanged ? FEditorTheme::Accent : ImGui::GetStyleColorVec4(ImGuiCol_Button));
	const bool bApply = ImGui::Button(ICON_FA_ROTATE " 적용 (다시 가져오기)");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::Button("되돌리기"))
	{
		S = SavedImportSettings;
	}
	ImGui::EndDisabled();
	if (bApply)
	{
		if (!S.SaveForSource(Path))
		{
			if (Env.Editor != nullptr && Env.Editor->Notify)
			{
				Env.Editor->Notify("임포트 설정을 저장하지 못했습니다", true);
			}
		}
		else
		{
			SavedImportSettings = S;
			if (Env.Editor != nullptr && Env.Editor->ReimportModel)
			{
				Env.Editor->ReimportModel(Path); // 이 창의 미리보기도 관리자가 다시 만든다
			}
		}
	}
	FAssetEditorWidgets::Hint("적용하면 원본 옆 .eimport 파일에 저장하고 모델을 다시 가져옵니다. 열린 씬의 같은 모델도 바뀝니다.");
	ImGui::Spacing();
}

// ---------------------------------------------------------------- 소켓

bool FModelEditorBase::GetBoneWorld(const std::string& Bone, FMatrix4x4& OutWorld)
{
	FScene&    Scene    = Preview.GetScene();
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(ModelRoot))
	{
		return false;
	}
	if (Bone.empty())
	{
		OutWorld = Scene.GetTransform(ModelRoot).WorldMatrix;
		return true;
	}
	const FModelComponent* ModelComponent = Registry.TryGet<FModelComponent>(ModelRoot);
	if (ModelComponent == nullptr)
	{
		return false;
	}
	for (const FEntity Node : ModelComponent->Runtime.NodeEntities)
	{
		const FNameComponent* Name = Registry.IsValid(Node) ? Registry.TryGet<FNameComponent>(Node) : nullptr;
		if (Name != nullptr && Name->Name == Bone)
		{
			OutWorld = Scene.GetTransform(Node).WorldMatrix;
			return true;
		}
	}
	return false;
}

void FModelEditorBase::DrawSockets()
{
	if (!Metadata || Model == nullptr || !ImGui::CollapsingHeader(ICON_FA_LINK " 소켓 (부착 지점)", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	std::vector<FModelSocket>& Sockets = Metadata->Sockets;
	if (SelectedSocket >= static_cast<int32>(Sockets.size()))
	{
		SelectedSocket = -1;
	}
	const auto IsNameUsed = [&](const std::string& Name, int32 Except) {
		for (int32 Index = 0; Index < static_cast<int32>(Sockets.size()); ++Index)
		{
			if (Index != Except && Sockets[Index].Name == Name)
			{
				return true;
			}
		}
		return false;
	};

	for (int32 Index = 0; Index < static_cast<int32>(Sockets.size()); ++Index)
	{
		const FModelSocket& Socket = Sockets[Index];
		ImGui::PushID(Index);
		const std::string Label = Socket.Name + (Socket.Bone.empty() ? std::string("   (모델 루트)") : "   " ICON_FA_BONE " " + Socket.Bone);
		if (ImGui::Selectable(Label.c_str(), SelectedSocket == Index))
		{
			SelectedSocket = Index;
		}
		ImGui::PopID();
	}
	if (Sockets.empty())
	{
		ImGui::TextDisabled("소켓이 없습니다");
	}

	if (ImGui::Button(ICON_FA_PLUS " 소켓 추가"))
	{
		FModelSocket Socket;
		Socket.Bone = SelectedSocket >= 0 ? Sockets[SelectedSocket].Bone : std::string();
		for (int32 Number = 1;; ++Number)
		{
			Socket.Name = Number == 1 ? std::string("Socket") : std::format("Socket{}", Number);
			if (!IsNameUsed(Socket.Name, -1))
			{
				break;
			}
		}
		Sockets.push_back(Socket);
		SelectedSocket = static_cast<int32>(Sockets.size()) - 1;
		MarkEdited("소켓 추가");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedSocket < 0);
	if (ImGui::Button(ICON_FA_TRASH " 삭제##Socket"))
	{
		Sockets.erase(Sockets.begin() + SelectedSocket);
		SelectedSocket = -1;
		MarkEdited("소켓 삭제");
	}
	ImGui::EndDisabled();
	if (SelectedSocket < 0)
	{
		FAssetEditorWidgets::Hint("소켓은 무기·장신구 등을 붙일 지점입니다. 씬에서는 붙일 엔티티에 '소켓 부착' 컴포넌트를 추가해 고릅니다.");
		return;
	}

	FModelSocket& Socket = Sockets[SelectedSocket];
	if (SocketNameFor != SelectedSocket)
	{
		std::snprintf(SocketNameBuffer, sizeof(SocketNameBuffer), "%s", Socket.Name.c_str());
		SocketNameFor = SelectedSocket;
	}
	if (ImGui::InputText("이름##Socket", SocketNameBuffer, sizeof(SocketNameBuffer)))
	{
		const std::string Candidate = SocketNameBuffer;
		if (!Candidate.empty() && !IsNameUsed(Candidate, SelectedSocket))
		{
			Socket.Name = Candidate;
			MarkEdited("소켓 이름");
		}
	}
	if (std::string(SocketNameBuffer) != Socket.Name)
	{
		ImGui::TextColored(FEditorTheme::Warning, "비었거나 같은 이름의 소켓이 있어 적용하지 않았습니다");
	}

	if (ImGui::BeginCombo("뼈 (부모)", Socket.Bone.empty() ? "(모델 루트)" : Socket.Bone.c_str(), ImGuiComboFlags_HeightLarge))
	{
		if (ImGui::Selectable("(모델 루트)", Socket.Bone.empty()))
		{
			Socket.Bone.clear();
			MarkEdited("소켓 뼈 변경");
		}
		std::unordered_set<std::string> Seen;
		for (const FModelNode& Node : Model->Model.Nodes)
		{
			if (Node.Name.empty() || !Seen.insert(Node.Name).second)
			{
				continue;
			}
			if (ImGui::Selectable(Node.Name.c_str(), Node.Name == Socket.Bone))
			{
				Socket.Bone = Node.Name;
				MarkEdited("소켓 뼈 변경");
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::DragFloat3("위치 (cm)", &Socket.Position.X, 0.25f, 0.0f, 0.0f, "%.2f"))
	{
		MarkEdited("소켓 위치");
	}
	if (SocketEulerFor != SelectedSocket || !SocketEulerRotation.Equals(Socket.Rotation, 1.0e-6f))
	{
		Socket.Rotation.ToEuler(SocketEulerDegrees.X, SocketEulerDegrees.Y, SocketEulerDegrees.Z);
		SocketEulerRotation = Socket.Rotation;
		SocketEulerFor      = SelectedSocket;
	}
	if (ImGui::DragFloat3("회전 (도)", &SocketEulerDegrees.X, 0.5f, 0.0f, 0.0f, "%.1f"))
	{
		Socket.Rotation     = FQuat::FromEuler(SocketEulerDegrees.X, SocketEulerDegrees.Y, SocketEulerDegrees.Z);
		SocketEulerRotation = Socket.Rotation;
		MarkEdited("소켓 회전");
	}
	ImGui::SetItemTooltip("Pitch / Yaw / Roll");
	if (ImGui::DragFloat3("스케일", &Socket.Scale.X, 0.01f, 0.001f, 100.0f, "%.3f"))
	{
		MarkEdited("소켓 스케일");
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("기즈모");
	ImGui::SameLine();
	if (ImGui::RadioButton("이동", !bSocketRotateMode))
	{
		bSocketRotateMode = false;
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("회전", bSocketRotateMode))
	{
		bSocketRotateMode = true;
	}
	FAssetEditorWidgets::Hint("미리보기의 기즈모로 끌어 옮길 수 있습니다 (위치·회전은 뼈 기준). 저장하면 원본 옆 .emeta에 기록됩니다.");
}

void FModelEditorBase::DrawSocketOverlay()
{
	Preview.bBlockCameraInput = false;
	if (!Metadata || Metadata->Sockets.empty() || !Preview.WasDrawnThisFrame())
	{
		return;
	}
	FScene&      Scene    = Preview.GetScene();
	ImDrawList*  DrawList = ImGui::GetWindowDrawList();
	const ImVec2 ClipMin(Preview.GetViewportMin().X, Preview.GetViewportMin().Y);
	const ImVec2 ClipMax(Preview.GetViewportMax().X, Preview.GetViewportMax().Y);
	const float  AxisLength = FMath::Max(Preview.GetOrbit().Distance * 0.06f, 1.0f);

	// 모든 소켓: 작은 축 (X 빨강 / Y 초록 / Z 파랑) + 이름
	DrawList->PushClipRect(ClipMin, ClipMax, true);
	for (int32 Index = 0; Index < static_cast<int32>(Metadata->Sockets.size()); ++Index)
	{
		const FModelSocket& Socket = Metadata->Sockets[Index];
		FMatrix4x4          World;
		FVector2            Origin;
		if (!Scene.GetSocketWorldMatrix(ModelRoot, Socket.Name, World) || !Preview.ProjectToScreen(World.GetOrigin(), Origin))
		{
			continue;
		}
		const FVector3 Axes[3]   = { World.GetAxisX(), World.GetAxisY(), World.GetAxisZ() };
		const ImU32    Colors[3] = { IM_COL32(235, 80, 80, 255), IM_COL32(90, 210, 90, 255), IM_COL32(90, 140, 255, 255) };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			FVector2 End;
			if (Preview.ProjectToScreen(World.GetOrigin() + Axes[Axis].GetNormalized() * AxisLength, End))
			{
				DrawList->AddLine(ImVec2(Origin.X, Origin.Y), ImVec2(End.X, End.Y), Colors[Axis], 2.0f);
			}
		}
		const bool bSelected = Index == SelectedSocket;
		DrawList->AddCircleFilled(ImVec2(Origin.X, Origin.Y), bSelected ? 5.0f : 3.5f, bSelected ? IM_COL32(255, 200, 60, 255) : IM_COL32(230, 230, 230, 255));
		DrawList->AddText(ImVec2(Origin.X + 7.0f, Origin.Y - 7.0f), IM_COL32(240, 240, 240, 230), Socket.Name.c_str());
	}
	DrawList->PopClipRect();

	// 선택 소켓 기즈모 (뼈 기준 로컬로 되돌려 저장)
	if (SelectedSocket < 0 || SelectedSocket >= static_cast<int32>(Metadata->Sockets.size()))
	{
		return;
	}
	FModelSocket& Socket = Metadata->Sockets[SelectedSocket];
	FMatrix4x4    BoneWorld;
	if (!GetBoneWorld(Socket.Bone, BoneWorld))
	{
		return;
	}
	FMatrix4x4       World      = Socket.GetLocalMatrix() * BoneWorld;
	const FMatrix4x4 View       = Preview.GetCamera().GetViewMatrix();
	const FMatrix4x4 Projection = Preview.GetCamera().GetProjectionMatrix();
	ImGuizmo::PushID(static_cast<const void*>(this)); // 메인 뷰포트 기즈모와 구분
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(Preview.GetImageMin().X, Preview.GetImageMin().Y, Preview.GetImageSize().X, Preview.GetImageSize().Y);
	const bool bChanged = ImGuizmo::Manipulate(&View.M[0][0], &Projection.M[0][0], bSocketRotateMode ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE,
	                                           ImGuizmo::LOCAL, &World.M[0][0]);
	Preview.bBlockCameraInput = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
	ImGuizmo::PopID();
	if (bChanged)
	{
		const FMatrix4x4 Local = World * BoneWorld.GetInverse();
		FVector3         IgnoredScale;
		Local.Decompose(Socket.Position, Socket.Rotation, IgnoredScale); // 스케일은 기즈모로 바꾸지 않는다
		MarkEdited(bSocketRotateMode ? "소켓 회전" : "소켓 이동");
	}
}

// ---------------------------------------------------------------- 노티파이 트랙

void FAnimationEditor::DrawNotifyTrack(const std::string& Clip, float Duration, float Time)
{
	if (!Metadata || Duration <= 0.0f)
	{
		return;
	}
	ImGui::SeparatorText(ICON_FA_BELL "  노티파이 (애니메이션 이벤트)");

	std::vector<FAnimNotify>* List = nullptr;
	for (FClipNotifies& Entry : Metadata->Clips)
	{
		if (Entry.Clip == Clip)
		{
			List = &Entry.Notifies;
		}
	}
	if (SelectedNotifyClip != Clip)
	{
		SelectedNotifyClip = Clip;
		SelectedNotify     = -1;
		NotifyNameFor      = -1;
	}
	const int32 Count = List != nullptr ? static_cast<int32>(List->size()) : 0;
	if (SelectedNotify >= Count)
	{
		SelectedNotify = -1;
	}
	const auto AddNotify = [&](EAnimNotifyKind Kind, float At) {
		std::vector<FAnimNotify>& Target = Metadata->GetOrAddNotifies(Clip);
		FAnimNotify               Notify;
		Notify.Kind     = Kind;
		Notify.Name     = Kind == EAnimNotifyKind::State ? "State" : "Notify";
		Notify.Duration = Kind == EAnimNotifyKind::State ? FMath::Min(0.5f, FMath::Max(Duration - At, 0.05f)) : 0.0f;
		Notify.Time     = FMath::Clamp(At, 0.0f, Duration - Notify.Duration);
		Target.push_back(Notify);
		SelectedNotify = static_cast<int32>(Target.size()) - 1;
		NotifyNameFor  = -1;
		MarkEdited(Kind == EAnimNotifyKind::State ? "스테이트 추가" : "노티파이 추가");
	};

	// 줄 배치: 0번 줄 = 점 노티파이, 그 아래 = 스테이트 (겹치면 다음 줄)
	std::vector<int32> Lanes(static_cast<size_t>(Count), 0);
	std::vector<float> LaneEnds;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FAnimNotify& Notify = (*List)[Index];
		if (Notify.Kind != EAnimNotifyKind::State)
		{
			continue;
		}
		size_t Lane = 0;
		while (Lane < LaneEnds.size() && LaneEnds[Lane] > Notify.Time)
		{
			++Lane;
		}
		if (Lane == LaneEnds.size())
		{
			LaneEnds.push_back(0.0f);
		}
		LaneEnds[Lane] = Notify.GetEndTime();
		Lanes[Index]   = static_cast<int32>(Lane) + 1;
	}
	const int32  LaneCount = 1 + FMath::Max(static_cast<int32>(LaneEnds.size()), 1);
	const float  RowHeight = ImGui::GetFrameHeight();
	const float  Width     = FMath::Max(ImGui::GetContentRegionAvail().x, 60.0f);
	const float  Height    = RowHeight * static_cast<float>(LaneCount);
	const ImVec2 Origin    = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##NotifyTrack", ImVec2(Width, Height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool bHovered = ImGui::IsItemHovered();
	const bool bActive  = ImGui::IsItemActive();

	const auto ToX = [&](float T) { return Origin.x + T / Duration * Width; };
	const auto ToT = [&](float X) { return FMath::Clamp((X - Origin.x) / Width * Duration, 0.0f, Duration); };

	// 항목 사각형 (클릭 판정 겸용)
	struct FItemRect
	{
		ImVec2 Min;
		ImVec2 Max;
	};
	std::vector<FItemRect> Rects(static_cast<size_t>(Count));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FAnimNotify& Notify = (*List)[Index];
		const float        Top    = Origin.y + RowHeight * static_cast<float>(Lanes[Index]);
		if (Notify.Kind == EAnimNotifyKind::State)
		{
			Rects[Index] = { ImVec2(ToX(Notify.Time), Top + 2.0f), ImVec2(FMath::Max(ToX(Notify.GetEndTime()), ToX(Notify.Time) + 4.0f), Top + RowHeight - 2.0f) };
		}
		else
		{
			const float X = ToX(Notify.Time);
			Rects[Index]  = { ImVec2(X - 6.0f, Top + 2.0f), ImVec2(X + 6.0f, Top + RowHeight - 2.0f) };
		}
	}
	const ImVec2 Mouse = ImGui::GetIO().MousePos;
	int32        Hit   = -1;
	for (int32 Index = Count - 1; Index >= 0; --Index)
	{
		if (Mouse.x >= Rects[Index].Min.x && Mouse.x <= Rects[Index].Max.x && Mouse.y >= Rects[Index].Min.y && Mouse.y <= Rects[Index].Max.y)
		{
			Hit = Index;
			break;
		}
	}

	// 그리기
	ImDrawList* Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + Height), IM_COL32(24, 24, 26, 255), 3.0f);
	for (int32 Lane = 1; Lane < LaneCount; ++Lane)
	{
		const float Y = Origin.y + RowHeight * static_cast<float>(Lane);
		Draw->AddLine(ImVec2(Origin.x, Y), ImVec2(Origin.x + Width, Y), IM_COL32(45, 45, 48, 255));
	}
	for (int32 Tick = 1; Tick < 10; ++Tick)
	{
		const float X = Origin.x + Width * static_cast<float>(Tick) / 10.0f;
		Draw->AddLine(ImVec2(X, Origin.y), ImVec2(X, Origin.y + Height), IM_COL32(38, 38, 40, 255));
	}
	const double Now = ImGui::GetTime();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FAnimNotify& Notify    = (*List)[Index];
		const bool         bSelected = Index == SelectedNotify;
		// 방금 발생한 항목은 밝게 (미리보기 재생 중 확인용)
		bool bFired = false;
		for (const FRecentEvent& Recent : RecentEvents)
		{
			bFired |= Recent.Event.Clip == Clip && Recent.Event.Name == Notify.Name && Now - Recent.TimeSeconds < 0.35;
		}
		const FItemRect& Rect = Rects[Index];
		if (Notify.Kind == EAnimNotifyKind::State)
		{
			const ImU32 Fill = bFired ? IM_COL32(120, 200, 255, 255) : (bSelected ? IM_COL32(70, 140, 220, 255) : IM_COL32(50, 100, 170, 255));
			Draw->AddRectFilled(Rect.Min, Rect.Max, Fill, 3.0f);
			if (bSelected)
			{
				Draw->AddRect(Rect.Min, Rect.Max, IM_COL32(255, 200, 60, 255), 3.0f, 0, 1.5f);
			}
			Draw->PushClipRect(Rect.Min, Rect.Max, true);
			Draw->AddText(ImVec2(Rect.Min.x + 4.0f, Rect.Min.y + 1.0f), IM_COL32(240, 240, 240, 255), Notify.Name.c_str());
			Draw->PopClipRect();
		}
		else
		{
			const ImVec2 Center((Rect.Min.x + Rect.Max.x) * 0.5f, (Rect.Min.y + Rect.Max.y) * 0.5f);
			const float  R    = 6.0f;
			const ImU32  Fill = bFired ? IM_COL32(255, 240, 120, 255) : (bSelected ? IM_COL32(255, 200, 60, 255) : IM_COL32(230, 150, 60, 255));
			Draw->AddQuadFilled(ImVec2(Center.x, Center.y - R), ImVec2(Center.x + R, Center.y), ImVec2(Center.x, Center.y + R), ImVec2(Center.x - R, Center.y), Fill);
			Draw->AddText(ImVec2(Center.x + R + 2.0f, Rect.Min.y + 1.0f), IM_COL32(220, 220, 220, 255), Notify.Name.c_str());
		}
	}
	const float PlayheadX = ToX(Time);
	Draw->AddLine(ImVec2(PlayheadX, Origin.y), ImVec2(PlayheadX, Origin.y + Height), ImGui::ColorConvertFloat4ToU32(FEditorTheme::Accent), 2.0f);

	// 조작: 클릭 선택/빈 곳 클릭 = 그 시각으로 이동, 끌기 = 이동 (스테이트 오른쪽 끝은 길이), 더블클릭 = 추가, 우클릭 = 메뉴
	FScene& Scene = Preview.GetScene();
	if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Hit < 0)
	{
		AddNotify(EAnimNotifyKind::Notify, ToT(Mouse.x));
	}
	else if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (Hit >= 0)
		{
			SelectedNotify = Hit;
			const FAnimNotify& Notify = (*List)[Hit];
			DragMode       = Notify.Kind == EAnimNotifyKind::State && Mouse.x > Rects[Hit].Max.x - 5.0f ? 2 : 1;
			DragGrabOffset = ToT(Mouse.x) - Notify.Time;
		}
		else
		{
			SelectedNotify = -1;
			DragMode       = 0;
			FAnimationSystem::Stop(Scene, ModelRoot);
			FAnimationSystem::SetTime(Scene, ModelRoot, ToT(Mouse.x));
		}
	}
	if (bActive && DragMode != 0 && SelectedNotify >= 0 && List != nullptr && ImGui::GetIO().MouseDelta.x != 0.0f)
	{
		FAnimNotify& Notify = (*List)[SelectedNotify];
		if (DragMode == 1)
		{
			const float MaxStart = Notify.Kind == EAnimNotifyKind::State ? Duration - Notify.Duration : Duration;
			Notify.Time          = FMath::Clamp(ToT(Mouse.x) - DragGrabOffset, 0.0f, FMath::Max(MaxStart, 0.0f));
		}
		else
		{
			Notify.Duration = FMath::Clamp(ToT(Mouse.x) - Notify.Time, 0.01f, Duration - Notify.Time);
		}
		MarkEdited(DragMode == 1 ? "노티파이 이동" : "스테이트 길이");
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		DragMode = 0;
	}
	if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		ContextTime = ToT(Mouse.x);
		ContextHit  = Hit;
		ImGui::OpenPopup("##NotifyMenu");
	}
	if (bHovered && Hit >= 0 && DragMode == 0)
	{
		const FAnimNotify& Notify = (*List)[Hit];
		ImGui::SetTooltip("%s  (%s, %.2f초%s)", Notify.Name.c_str(), Notify.Kind == EAnimNotifyKind::State ? "스테이트" : "노티파이", Notify.Time,
		                  Notify.Kind == EAnimNotifyKind::State ? std::format(" ~ {:.2f}초", Notify.GetEndTime()).c_str() : "");
	}
	if (ImGui::BeginPopup("##NotifyMenu"))
	{
		if (ContextHit >= 0 && List != nullptr && ContextHit < static_cast<int32>(List->size()))
		{
			if (ImGui::MenuItem(ICON_FA_COPY "  복제"))
			{
				FAnimNotify Copy = (*List)[ContextHit];
				Copy.Time        = FMath::Min(Copy.Time + 0.1f, FMath::Max(Duration - Copy.Duration, 0.0f));
				List->push_back(Copy);
				SelectedNotify = static_cast<int32>(List->size()) - 1;
				MarkEdited("노티파이 복제");
			}
			if (ImGui::MenuItem(ICON_FA_TRASH "  삭제"))
			{
				List->erase(List->begin() + ContextHit);
				SelectedNotify = -1;
				MarkEdited("노티파이 삭제");
			}
		}
		else
		{
			if (ImGui::MenuItem(std::format(ICON_FA_DIAMOND "  노티파이 추가 ({:.2f}초)", ContextTime).c_str()))
			{
				AddNotify(EAnimNotifyKind::Notify, ContextTime);
			}
			if (ImGui::MenuItem(std::format(ICON_FA_RULER_HORIZONTAL "  스테이트 추가 ({:.2f}초부터)", ContextTime).c_str()))
			{
				AddNotify(EAnimNotifyKind::State, ContextTime);
			}
		}
		ImGui::EndPopup();
	}

	// 선택 항목 속성
	List = nullptr;
	for (FClipNotifies& Entry : Metadata->Clips)
	{
		if (Entry.Clip == Clip)
		{
			List = &Entry.Notifies;
		}
	}
	if (List != nullptr && SelectedNotify >= 0 && SelectedNotify < static_cast<int32>(List->size()))
	{
		FAnimNotify& Notify = (*List)[SelectedNotify];
		if (NotifyNameFor != SelectedNotify)
		{
			std::snprintf(NotifyNameBuffer, sizeof(NotifyNameBuffer), "%s", Notify.Name.c_str());
			NotifyNameFor = SelectedNotify;
		}
		if (ImGui::InputText("이름##Notify", NotifyNameBuffer, sizeof(NotifyNameBuffer)))
		{
			const std::string Valid = AnimNotifyMath::MakeValidName(NotifyNameBuffer);
			if (Valid != Notify.Name)
			{
				Notify.Name = Valid;
				MarkEdited("노티파이 이름");
			}
		}
		if (Notify.Name != NotifyNameBuffer)
		{
			ImGui::TextColored(FEditorTheme::Warning, "영문/숫자/_만 쓸 수 있어 '%s'(으)로 저장됩니다", Notify.Name.c_str());
		}
		const bool bState = Notify.Kind == EAnimNotifyKind::State;
		ImGui::TextDisabled("%s", bState ? "스테이트 (구간)" : "노티파이 (한 시점)");
		if (ImGui::DragFloat(bState ? "시작 (초)" : "시각 (초)", &Notify.Time, 0.005f, 0.0f, Duration, "%.3f"))
		{
			Notify.Time = FMath::Clamp(Notify.Time, 0.0f, bState ? FMath::Max(Duration - Notify.Duration, 0.0f) : Duration);
			MarkEdited("노티파이 시각");
		}
		if (bState && ImGui::DragFloat("길이 (초)", &Notify.Duration, 0.005f, 0.01f, Duration, "%.3f"))
		{
			Notify.Duration = FMath::Clamp(Notify.Duration, 0.01f, FMath::Max(Duration - Notify.Time, 0.01f));
			MarkEdited("스테이트 길이");
		}
		if (ImGui::Button(ICON_FA_TRASH " 삭제##Notify"))
		{
			List->erase(List->begin() + SelectedNotify);
			SelectedNotify = -1;
			MarkEdited("노티파이 삭제");
		}
		else if (bState)
		{
			ImGui::TextDisabled("Lua: OnAnimNotifyBegin_%s() / OnAnimNotifyTick_%s(dt) / OnAnimNotifyEnd_%s()", Notify.Name.c_str(),
			                    Notify.Name.c_str(), Notify.Name.c_str());
		}
		else
		{
			ImGui::TextDisabled("Lua: function 스크립트:OnAnimNotify_%s() end", Notify.Name.c_str());
		}
	}
	else
	{
		FAssetEditorWidgets::Hint("더블클릭: 노티파이 추가 · 우클릭: 메뉴(스테이트 추가/복제/삭제) · 끌기: 이동 · 스테이트 오른쪽 끝 끌기: 길이 · 빈 곳 클릭: 그 시각으로 이동");
	}

	// 최근 발생 (미리보기 재생 중). 매 프레임 Tick은 빼고 보여준다
	if (!RecentEvents.empty() && ImGui::TreeNodeEx("최근 발생", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (auto It = RecentEvents.rbegin(); It != RecentEvents.rend(); ++It)
		{
			static constexpr const char* TypeNames[] = { "노티파이", "시작", "진행", "끝" };
			ImGui::TextDisabled("%5.1f초 전", Now - It->TimeSeconds);
			ImGui::SameLine();
			ImGui::Text("%s  %s  (%s)", It->Event.Name.c_str(), TypeNames[static_cast<int32>(It->Event.Type)], It->Event.Clip.c_str());
		}
		ImGui::TreePop();
	}
}
