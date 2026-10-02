#include "Editor/AssetEditors/DataAssetEditor.h"

#include "Core/FileSystem.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/DataLibrary.h"

#include <imgui.h>

#include <cfloat>
#include <format>

namespace
{
	std::shared_ptr<const FDataStruct> ResolveStruct(const std::string& StructPath)
	{
		return FDataLibrary::Get().LoadStruct(StructPath);
	}
} // namespace

bool FDataAssetEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		Notify(Env, "데이터 에셋을 읽지 못했습니다: " + GetDisplayName(), true);
		return false;
	}
	FDataAsset      Loaded;
	FDataLoadReport Report;
	std::string     Error;
	if (!FDataAsset::FromJsonString(Text, ResolveStruct, Loaded, &Report, &Error))
	{
		Notify(Env, std::format("데이터 에셋 형식 오류 ({}): {}", GetDisplayName(), Error), true);
		return false;
	}
	Asset        = std::move(Loaded);
	LoadWarnings = std::move(Report.Warnings);
	++Revision;
	RememberDiskState();
	return true;
}

bool FDataAssetEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	std::string Error;
	if (!FDataLibrary::Get().SaveDataAsset(GetAssetPathString(), Asset, &Error))
	{
		Notify(Env, "데이터 에셋 저장 실패: " + Error, true);
		return false;
	}
	RememberDiskState();
	return true;
}

std::string FDataAssetEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FDataAssetEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FDataAsset      Restored;
	FDataLoadReport Report;
	if (FDataAsset::FromJsonString(State, ResolveStruct, Restored, &Report))
	{
		Asset        = std::move(Restored);
		LoadWarnings = std::move(Report.Warnings);
		++Revision;
	}
}

void FDataAssetEditor::OnDataLibraryChanged(FAssetEditorEnvironment& Env)
{
	RestoreState(Env, CaptureState());
}

void FDataAssetEditor::Edited(std::string_view Label)
{
	MarkEdited(Label);
	++Revision;
}

void FDataAssetEditor::Revalidate()
{
	ValidatedRevision = Revision;
	FieldProblems.clear();
	ReferenceWarnings.clear();
	if (Asset.Struct == nullptr)
	{
		return;
	}
	ReferenceWarnings = FDataLibrary::Get().ValidateReferences(Asset);
	for (FDataReferenceIssue& Issue : FDataLibrary::Get().ValidateRecordReferences(*Asset.Struct, Asset.Record))
	{
		std::string& Message = FieldProblems[Issue.Field];
		Message              = Message.empty() ? std::move(Issue.Message) : Message + "\n" + Issue.Message;
	}
}

void FDataAssetEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	if (ValidatedRevision != Revision && !ImGui::IsAnyItemActive())
	{
		Revalidate();
	}
	if (Asset.Struct == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_CIRCLE_EXCLAMATION " 구조체를 찾을 수 없습니다: %s", Asset.StructPath.c_str());
		FAssetEditorWidgets::Hint("오른쪽 '구조체'에서 다른 구조체를 고르면 이름이 같은 필드 값이 되살아납니다.");
		return;
	}
	if (!ImGui::BeginTable("##AssetFields", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
	                                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
	                       ImGui::GetContentRegionAvail()))
	{
		return;
	}
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("필드", ImGuiTableColumnFlags_WidthStretch, 0.3f);
	ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
	ImGui::TableHeadersRow();

	const std::vector<FDataField>& Fields = Asset.Struct->Fields;
	for (size_t Index = 0; Index < Fields.size() && Index < Asset.Record.Values.size(); ++Index)
	{
		const FDataField& Field = Fields[Index];
		ImGui::PushID(static_cast<int>(Index));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Field.Name.c_str());
		ImGui::SetItemTooltip("%s", DataValueWidgets::DescribeField(Field).c_str());
		if (!Field.Description.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", Field.Description.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::TableNextColumn();
		ImGui::PushItemWidth(-FLT_MIN);
		FDataValue         Value = Asset.Record.Values[Index];
		FDataWidgetContext Context;
		Context.Editor = Env.Editor;
		const auto Problem = FieldProblems.find(static_cast<int32>(Index));
		Context.Problem    = Problem != FieldProblems.end() ? &Problem->second : nullptr;
		const bool bChanged = DataValueWidgets::Draw(Field, Value, Context);
		ImGui::PopItemWidth();

		ImGui::TableNextColumn();
		ImGui::BeginDisabled(Value == Field.Default);
		const bool bReset = ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("구조체 기본값으로");
		if (bReset)
		{
			Value = Field.Default;
		}
		if ((bChanged || bReset) && Asset.SetValue(Field.Name, Value))
		{
			Edited(bReset ? "기본값으로" : "값 편집");
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void FDataAssetEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	if (ImGui::CollapsingHeader(ICON_FA_TABLE_LIST " 구조체", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextUnformatted(Asset.StructPath.empty() ? "(없음)" : Asset.StructPath.c_str());
		if (Asset.Struct != nullptr)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_PEN_TO_SQUARE " 구조체 편집") && Env.Editor != nullptr && Env.Editor->OpenAssetEditorRequest)
			{
				Env.Editor->OpenAssetEditorRequest(FDataLibrary::Get().ResolvePath(Asset.StructPath));
			}
			if (!Asset.Struct->Description.empty())
			{
				FAssetEditorWidgets::Hint(Asset.Struct->Description.c_str());
			}
		}
		if (Env.Editor != nullptr && ImGui::BeginCombo("구조체 바꾸기", Asset.StructPath.c_str()))
		{
			for (const std::string& StructPath : DataValueWidgets::ScanContentFiles(Env.Editor->ContentDirectory, FDataStruct::Extension))
			{
				if (ImGui::Selectable(StructPath.c_str(), StructPath == Asset.StructPath) && StructPath != Asset.StructPath)
				{
					Asset.StructPath = StructPath;
					Asset.Rebind(ResolveStruct(StructPath));
					Edited("구조체 바꾸기");
				}
			}
			ImGui::EndCombo();
		}
		ImGui::BeginDisabled(Asset.Struct == nullptr);
		if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT " 모두 기본값으로"))
		{
			Asset.ResetToDefaults();
			Edited("모두 기본값으로");
		}
		ImGui::EndDisabled();
	}

	const size_t      Count  = LoadWarnings.size() + ReferenceWarnings.size();
	const std::string Header = Count == 0 ? std::string(ICON_FA_CIRCLE_CHECK " 검증###Validation")
	                                      : std::format(ICON_FA_TRIANGLE_EXCLAMATION " 검증 ({})###Validation", Count);
	if (ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (Count == 0)
		{
			ImGui::TextColored(FEditorTheme::Success, "문제 없음 (행 참조·에셋 경로 확인됨)");
		}
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Warning);
		for (const std::string& Warning : LoadWarnings)
		{
			ImGui::TextWrapped("%s", Warning.c_str());
		}
		for (const std::string& Warning : ReferenceWarnings)
		{
			ImGui::TextWrapped("%s", Warning.c_str());
		}
		ImGui::PopStyleColor();
	}
}
