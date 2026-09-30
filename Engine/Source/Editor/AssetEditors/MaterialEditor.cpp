#include "Editor/AssetEditors/MaterialEditor.h"

#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/ResourceManager.h"

#include <imgui.h>

bool FMaterialEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	FMaterialAsset Loaded;
	if (!Loaded.LoadFromFile(Path))
	{
		return false;
	}
	Asset = std::move(Loaded);

	// 씬과 같은 머티리얼 리소스 (경로 캐시). 열 때/되돌릴 때 파일 내용으로 맞춘다
	if (!Env.Resources->GetMaterial(Material))
	{
		Material = Env.Resources->LoadMaterial(Path);
		if (!Material.IsValid())
		{
			return false;
		}
	}
	ApplyToMaterial(Env, true);

	if (!Preview.GetScene().GetRegistry().IsValid(PreviewEntity))
	{
		PreviewEntity = Preview.GetScene().CreateEntity("PreviewMesh");
		Preview.GetScene().GetRegistry().Emplace<FStaticMeshComponent>(PreviewEntity);
		SetPreviewShape(Env, PreviewShape.c_str());
	}
	if (TextureFiles.empty() && Env.Editor != nullptr)
	{
		ScanTextureFiles(Env.Editor->ContentDirectory);
	}
	return true;
}

bool FMaterialEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return Asset.SaveToFile(Path);
}

std::string FMaterialEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FMaterialEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	if (Asset.FromJsonString(State))
	{
		ApplyToMaterial(Env, true);
	}
}

void FMaterialEditor::ApplyToMaterial(FAssetEditorEnvironment& Env, bool bTexturesChanged)
{
	if (bTexturesChanged)
	{
		Env.Resources->ApplyMaterialAsset(Material, Asset, Path.parent_path());
	}
	else if (FMaterial* Live = Env.Resources->GetMaterial(Material))
	{
		// 상수만 바뀐 경우 디스크립터 테이블은 그대로 둔다 (드래그 중 매 프레임 호출)
		Live->Name      = Asset.Name;
		Live->Constants = Asset.Constants;
	}
}

void FMaterialEditor::SetPreviewShape(FAssetEditorEnvironment& Env, const char* PrimitiveName)
{
	PreviewShape                 = PrimitiveName;
	FStaticMeshComponent& Mesh   = Preview.GetScene().GetRegistry().Get<FStaticMeshComponent>(PreviewEntity);
	Mesh.Mesh                    = Env.Resources->GetOrCreatePrimitiveMesh(PreviewShape);
	Mesh.Material                = Material;
	Preview.GetScene().UpdateTransforms();
}

void FMaterialEditor::ScanTextureFiles(const std::filesystem::path& ContentDirectory)
{
	TextureFiles = FAssetEditorWidgets::ScanImageFiles(ContentDirectory, Path.parent_path());
}

void FMaterialEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FAssetEditor::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	for (const auto& [Name, Label] : { std::pair{ "sphere", "구" }, std::pair{ "cube", "큐브" } })
	{
		ImGui::SameLine();
		if (ImGui::RadioButton(Label, PreviewShape == Name))
		{
			SetPreviewShape(Env, Name);
			FramePreview(Env);
		}
	}
}

bool FMaterialEditor::DrawTextureSlot(FAssetEditorEnvironment& Env, uint32 Slot, const char* Label)
{
	ImGui::PushID(static_cast<int>(Slot));
	bool       bChanged = false;
	std::string& Current = Asset.TexturePaths[Slot];

	// 작은 미리보기 (UI는 sRGB 텍스처를 선형으로 읽으므로 색상 텍스처가 약간 어둡게 보인다)
	const FMaterial* Live = Env.Resources->GetMaterial(Material);
	if (Live != nullptr && Live->Textures[Slot].IsValid())
	{
		const FD3D12Texture& Texture = Env.Resources->ResolveTexture(Live->Textures[Slot]);
		ImGui::Image(static_cast<ImTextureID>(Texture.GetSrv().Gpu.ptr), ImVec2(40.0f, 40.0f));
	}
	else
	{
		ImGui::Dummy(ImVec2(40.0f, 40.0f));
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRect(Min, Max, IM_COL32(90, 90, 100, 255));
	}
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::TextUnformatted(Label);
	ImGui::SetNextItemWidth(-FLT_MIN);
	bChanged = FAssetEditorWidgets::TextureCombo("##Texture", Current, TextureFiles, "(없음)");
	ImGui::EndGroup();
	ImGui::PopID();
	return bChanged;
}

void FMaterialEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	FMaterialConstants& Constants  = Asset.Constants;
	bool                bConstants = false;

	ImGui::SeparatorText("기본");
	char NameBuffer[128];
	strncpy_s(NameBuffer, sizeof(NameBuffer), Asset.Name.c_str(), _TRUNCATE);
	if (ImGui::InputText("이름", NameBuffer, sizeof(NameBuffer)))
	{
		Asset.Name = NameBuffer;
		bConstants = true;
	}

	ImGui::SeparatorText("표면");
	bConstants |= ImGui::ColorEdit4("베이스 컬러", &Constants.BaseColorFactor.X);
	bConstants |= ImGui::SliderFloat("금속성", &Constants.Metallic, 0.0f, 1.0f);
	bConstants |= ImGui::SliderFloat("거칠기", &Constants.Roughness, 0.0f, 1.0f);
	bConstants |= ImGui::ColorEdit3("발광", &Constants.EmissiveFactor.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
	bConstants |= ImGui::DragFloat("노멀 강도", &Constants.NormalScale, 0.01f, 0.0f, 4.0f);
	bConstants |= ImGui::SliderFloat("AO 강도", &Constants.OcclusionStrength, 0.0f, 1.0f);

	ImGui::SeparatorText("텍스처");
	bool bTextures = false;
	bTextures |= DrawTextureSlot(Env, MaterialSlot_BaseColor, "베이스 컬러 (sRGB)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_MetallicRoughness, "금속/거칠기 (G=거칠기, B=금속)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Normal, "노멀");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Occlusion, "AO (R)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Emissive, "발광 (sRGB)");
	if (ImGui::SmallButton("텍스처 목록 새로 고침") && Env.Editor != nullptr)
	{
		ScanTextureFiles(Env.Editor->ContentDirectory);
	}

	if (bConstants || bTextures)
	{
		ApplyToMaterial(Env, bTextures);
		MarkEdited(bTextures ? "텍스처 변경" : "머티리얼 값 변경");
	}

	ImGui::Spacing();
	FAssetEditorWidgets::Hint("열린 씬에 바로 반영됩니다. 저장하지 않고 닫으면 원래대로 돌아갑니다.");
}
