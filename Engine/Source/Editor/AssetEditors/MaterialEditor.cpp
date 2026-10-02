#include "Editor/AssetEditors/MaterialEditor.h"

#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/ResourceManager.h"

#include <imgui.h>

#include <algorithm>

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
	ApplyToMaterial(Env);

	if (!Preview.GetScene().GetRegistry().IsValid(PreviewEntity))
	{
		PreviewEntity = Preview.GetScene().CreateEntity("PreviewMesh");
		Preview.GetScene().GetRegistry().Emplace<FStaticMeshComponent>(PreviewEntity);
		SetPreviewShape(Env, PreviewShape.c_str());
	}
	if (TextureFiles.empty() && Env.Editor != nullptr)
	{
		ScanFiles(Env.Editor->ContentDirectory);
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
		ApplyToMaterial(Env);
	}
}

void FMaterialEditor::ApplyToMaterial(FAssetEditorEnvironment& Env)
{
	// 해석(부모 체인) + 텍스처 변경 시에만 테이블 재작성은 리소스 관리자가 한다. 이 머티리얼을 부모로 둔 열린 인스턴스도 함께 갱신된다
	Env.Resources->ApplyMaterialAsset(Material, Asset, Path.parent_path());
	RefreshInherited(Env);
}

void FMaterialEditor::RefreshInherited(FAssetEditorEnvironment& Env)
{
	ParentError.clear();
	Inherited = FMaterialAsset{};
	if (!Asset.IsInstance())
	{
		return;
	}
	// 덮어쓰는 항목이 없는 사본을 해석 = 부모 체인 값
	FMaterialAsset ParentOnly = Asset;
	ParentOnly.OverrideMask   = 0;
	Env.Resources->ResolveMaterialAsset(ParentOnly, Path, Inherited, nullptr, &ParentError);
}

void FMaterialEditor::SetPreviewShape(FAssetEditorEnvironment& Env, const char* PrimitiveName)
{
	PreviewShape                 = PrimitiveName;
	FStaticMeshComponent& Mesh   = Preview.GetScene().GetRegistry().Get<FStaticMeshComponent>(PreviewEntity);
	Mesh.Mesh                    = Env.Resources->GetOrCreatePrimitiveMesh(PreviewShape);
	Mesh.Material                = Material;
	Preview.GetScene().UpdateTransforms();
}

void FMaterialEditor::ScanFiles(const std::filesystem::path& ContentDirectory)
{
	TextureFiles = FAssetEditorWidgets::ScanImageFiles(ContentDirectory, Path.parent_path());

	// 부모 후보: Content 아래 .emat (자기 자신 제외)
	MaterialFiles.clear();
	std::error_code    ErrorCode;
	const std::wstring SelfKey = FMaterialAsset::MakePathKey(Path);
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		if (!It->is_regular_file(ErrorCode) || It->path().extension() != FMaterialAsset::Extension || FMaterialAsset::MakePathKey(It->path()) == SelfKey)
		{
			continue;
		}
		const std::filesystem::path Relative = std::filesystem::relative(It->path(), Path.parent_path(), ErrorCode);
		if (!ErrorCode)
		{
			MaterialFiles.push_back(FStringConv::ToUtf8(Relative.generic_wstring()));
		}
	}
	std::sort(MaterialFiles.begin(), MaterialFiles.end());
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

bool FMaterialEditor::DrawOverrideToggle(uint32 Field)
{
	if (!Asset.IsInstance())
	{
		return false;
	}
	ImGui::PushID(static_cast<int>(Field));
	bool bOverride = (Asset.OverrideMask & Field) != 0;
	const bool bChanged = ImGui::Checkbox("##Override", &bOverride);
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("덮어쓰기 (끄면 부모 값을 따른다)");
	}
	ImGui::PopID();
	ImGui::SameLine();
	if (bChanged)
	{
		Asset.OverrideMask = bOverride ? (Asset.OverrideMask | Field) : (Asset.OverrideMask & ~Field);
	}
	return bChanged;
}

bool FMaterialEditor::DrawTextureSlot(FAssetEditorEnvironment& Env, uint32 Slot, const char* Label)
{
	ImGui::PushID(static_cast<int>(Slot));
	bool         bChanged = DrawOverrideToggle(FMaterialAsset::GetTextureField(Slot));
	std::string& Current  = Asset.TexturePaths[Slot];

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
	if (FAssetEditorWidgets::TextureCombo("##Texture", Current, TextureFiles, "(없음)"))
	{
		Asset.OverrideMask |= FMaterialAsset::GetTextureField(Slot);
		bChanged = true;
	}
	ImGui::EndGroup();
	ImGui::PopID();
	return bChanged;
}

bool FMaterialEditor::DrawParent(FAssetEditorEnvironment& Env)
{
	std::string Parent = Asset.Parent;
	if (!FAssetEditorWidgets::TextureCombo("부모", Parent, MaterialFiles, "(없음 — 일반 머티리얼)"))
	{
		return false;
	}
	if (Parent.empty())
	{
		// 인스턴스 → 일반: 지금 보이는(해석된) 값을 모두 자기 값으로
		FMaterialAsset Resolved;
		Env.Resources->ResolveMaterialAsset(Asset, Path, Resolved);
		const std::string Name = Asset.Name;
		Asset                  = Resolved;
		Asset.Name             = Name;
	}
	else
	{
		// 일반 → 인스턴스: 처음에는 덮어쓰는 항목 없음 (모두 부모 값). 인스턴스끼리 부모만 바꾸면 덮어쓰기는 유지
		if (!Asset.IsInstance())
		{
			Asset.OverrideMask = 0;
		}
		Asset.Parent = Parent;
	}
	return true;
}

void FMaterialEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	// 인스턴스: 덮어쓰지 않은 항목은 부모 값을 보여 준다 (파일에는 쓰지 않음 — ToJsonString은 OverrideMask 항목만)
	if (Asset.IsInstance())
	{
		const uint32 Mask = Asset.OverrideMask;
		FMaterialAsset& Shown = Asset;
		const auto Inherit = [&](uint32 Field, auto& Value, const auto& Source) {
			if ((Mask & Field) == 0)
			{
				Value = Source;
			}
		};
		Inherit(FMaterialAsset::Field_BaseColorFactor, Shown.Constants.BaseColorFactor, Inherited.Constants.BaseColorFactor);
		Inherit(FMaterialAsset::Field_EmissiveFactor, Shown.Constants.EmissiveFactor, Inherited.Constants.EmissiveFactor);
		Inherit(FMaterialAsset::Field_Metallic, Shown.Constants.Metallic, Inherited.Constants.Metallic);
		Inherit(FMaterialAsset::Field_Roughness, Shown.Constants.Roughness, Inherited.Constants.Roughness);
		Inherit(FMaterialAsset::Field_NormalScale, Shown.Constants.NormalScale, Inherited.Constants.NormalScale);
		Inherit(FMaterialAsset::Field_OcclusionStrength, Shown.Constants.OcclusionStrength, Inherited.Constants.OcclusionStrength);
		Inherit(FMaterialAsset::Field_AlphaCutoff, Shown.Constants.AlphaCutoff, Inherited.Constants.AlphaCutoff);
		Inherit(FMaterialAsset::Field_BlendMode, Shown.BlendMode, Inherited.BlendMode);
		Inherit(FMaterialAsset::Field_TwoSided, Shown.bTwoSided, Inherited.bTwoSided);
		for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
		{
			Inherit(FMaterialAsset::GetTextureField(Slot), Shown.TexturePaths[Slot], Inherited.TexturePaths[Slot]);
		}
	}

	FMaterialConstants& Constants = Asset.Constants;
	bool                bChanged  = false;
	bool                bTextures = false;
	// 값 위젯: 인스턴스에서 고치면 그 항목을 덮어쓰기로 바꾼다
	const auto Edited = [&](uint32 Field, bool bWidgetChanged) {
		if (bWidgetChanged)
		{
			Asset.OverrideMask |= Field;
			bChanged = true;
		}
	};

	ImGui::SeparatorText("기본");
	char NameBuffer[128];
	strncpy_s(NameBuffer, sizeof(NameBuffer), Asset.Name.c_str(), _TRUNCATE);
	if (ImGui::InputText("이름", NameBuffer, sizeof(NameBuffer)))
	{
		Asset.Name = NameBuffer;
		bChanged   = true;
	}
	if (DrawParent(Env))
	{
		bChanged  = true;
		bTextures = true;
	}
	if (!ParentError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
		ImGui::TextWrapped("%s", ParentError.c_str());
		ImGui::PopStyleColor();
	}
	else if (Asset.IsInstance())
	{
		FAssetEditorWidgets::Hint("인스턴스: 체크한 항목만 부모 값을 덮어쓴다. 값을 고치면 자동으로 체크된다.");
	}

	ImGui::SeparatorText("렌더 상태");
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_BlendMode);
	{
		static const char* const Labels[] = { "불투명 (Opaque)", "마스크 (Masked — 알파 테스트)", "반투명 (Translucent)", "가산 (Additive)" };
		int32                    Mode     = static_cast<int32>(Asset.BlendMode);
		if (ImGui::Combo("블렌드 모드", &Mode, Labels, IM_ARRAYSIZE(Labels)))
		{
			Asset.BlendMode = static_cast<EMaterialBlendMode>(Mode);
			Edited(FMaterialAsset::Field_BlendMode, true);
		}
	}
	if (Asset.BlendMode == EMaterialBlendMode::Masked)
	{
		bChanged |= DrawOverrideToggle(FMaterialAsset::Field_AlphaCutoff);
		Edited(FMaterialAsset::Field_AlphaCutoff, ImGui::SliderFloat("알파 컷오프", &Constants.AlphaCutoff, 0.0f, 1.0f));
	}
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_TwoSided);
	Edited(FMaterialAsset::Field_TwoSided, ImGui::Checkbox("양면 (컬링 없음)", &Asset.bTwoSided));

	ImGui::SeparatorText("표면");
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_BaseColorFactor);
	Edited(FMaterialAsset::Field_BaseColorFactor, ImGui::ColorEdit4("베이스 컬러", &Constants.BaseColorFactor.X));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_Metallic);
	Edited(FMaterialAsset::Field_Metallic, ImGui::SliderFloat("금속성", &Constants.Metallic, 0.0f, 1.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_Roughness);
	Edited(FMaterialAsset::Field_Roughness, ImGui::SliderFloat("거칠기", &Constants.Roughness, 0.0f, 1.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_EmissiveFactor);
	Edited(FMaterialAsset::Field_EmissiveFactor, ImGui::ColorEdit3("발광", &Constants.EmissiveFactor.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_NormalScale);
	Edited(FMaterialAsset::Field_NormalScale, ImGui::DragFloat("노멀 강도", &Constants.NormalScale, 0.01f, 0.0f, 4.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_OcclusionStrength);
	Edited(FMaterialAsset::Field_OcclusionStrength, ImGui::SliderFloat("AO 강도", &Constants.OcclusionStrength, 0.0f, 1.0f));

	ImGui::SeparatorText("텍스처");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_BaseColor, "베이스 컬러 (sRGB, A = 알파)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_MetallicRoughness, "금속/거칠기 (G=거칠기, B=금속)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Normal, "노멀");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Occlusion, "AO (R)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Emissive, "발광 (sRGB)");
	if (ImGui::SmallButton("파일 목록 새로 고침") && Env.Editor != nullptr)
	{
		ScanFiles(Env.Editor->ContentDirectory);
	}

	if (bChanged || bTextures)
	{
		ApplyToMaterial(Env);
		MarkEdited(bTextures ? "텍스처 변경" : "머티리얼 값 변경");
	}

	ImGui::Spacing();
	FAssetEditorWidgets::Hint("열린 씬에 바로 반영됩니다(이 머티리얼을 부모로 둔 인스턴스 포함). 저장하지 않고 닫으면 원래대로 돌아갑니다.");
}
