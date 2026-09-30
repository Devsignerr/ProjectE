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

#include <imgui.h>

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
	return Model != nullptr;
}

bool FModelEditorBase::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return true;
}

void FModelEditorBase::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	(void)State;
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
}

void FAnimationEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FModelEditorBase::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::Checkbox("뼈대", &bShowBones);
}

void FAnimationEditor::DrawPreviewOverlay(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (bShowBones)
	{
		DrawBones();
	}
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

	ImGui::DragFloat("속도", &Animation->Speed, 0.01f, 0.0f, 5.0f, "%.2fx");
	ImGui::Checkbox("반복", &Animation->bLoop);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("재생 설정은 미리보기에만 적용됩니다 (씬에서는 애니메이션 컴포넌트에서 지정).");
	ImGui::PopStyleColor();

	ImGui::Spacing();
	DrawAddToScene(Env);
	DrawImportSettings(Env);
	if (ImGui::CollapsingHeader("모델 정보"))
	{
		DrawModelInfo(Env);
	}
}

// ---------------------------------------------------------------- 임포트 설정

void FModelEditorBase::RebuildPreview(FAssetEditorEnvironment& Env)
{
	FScene& Scene = Preview.GetScene();
	if (Scene.GetRegistry().IsValid(ModelRoot))
	{
		Scene.DestroyEntity(ModelRoot);
	}
	ModelRoot = FEntity{};
	Model     = nullptr;
	LoadAsset(Env);
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
