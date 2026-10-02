#include "Editor/AssetEditors/DataStructEditor.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"

#include <imgui.h>
#include <json.hpp>

#include <cfloat>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	using FJson = nlohmann::ordered_json;

	constexpr EDataFieldType LastFieldType = EDataFieldType::Array;

	EDataFieldType GetValueType(const FDataField& Field) { return Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type; }

	const char* GetTypeLabel(EDataFieldType Type)
	{
		switch (Type)
		{
		case EDataFieldType::Bool: return "Bool (참/거짓)";
		case EDataFieldType::Int: return "Int (정수)";
		case EDataFieldType::Float: return "Float (실수)";
		case EDataFieldType::String: return "String (글자)";
		case EDataFieldType::Text: return "Text (여러 줄 글자)";
		case EDataFieldType::Vector2: return "Vector2";
		case EDataFieldType::Vector3: return "Vector3";
		case EDataFieldType::Vector4: return "Vector4";
		case EDataFieldType::Color: return "Color (sRGB)";
		case EDataFieldType::Enum: return "Enum (값 목록)";
		case EDataFieldType::Asset: return "Asset (에셋 경로)";
		case EDataFieldType::RowRef: return "RowRef (행 참조)";
		case EDataFieldType::Array: return "Array (배열)";
		}
		return "?";
	}
} // namespace

// ---------------------------------------------------------------- 상태

bool FDataStructEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		Notify(Env, "데이터 구조체를 읽지 못했습니다: " + GetDisplayName(), true);
		return false;
	}
	FDataStruct     Loaded;
	FDataLoadReport Report;
	std::string     Error;
	if (!FDataStruct::FromJsonString(Text, Loaded, &Report, &Error))
	{
		Notify(Env, std::format("데이터 구조체 형식 오류 ({}): {}", GetDisplayName(), Error), true);
		return false;
	}
	Struct       = std::move(Loaded);
	LoadWarnings = std::move(Report.Warnings);
	Renames.clear();
	FieldError.clear();
	if (SelectedField >= static_cast<int32>(Struct.Fields.size()))
	{
		SelectedField = -1;
	}
	RememberDiskState();
	RefreshUsers(Env);
	if (!bAutoArgsRead)
	{
		bAutoArgsRead = true;
		SelectedField  = Struct.FindField(FStringConv::ToUtf8(FCommandLine::FromProcess().GetValue(L"--data-select")));
		bAutoPopup     = SelectedField >= 0 && FCommandLine::FromProcess().HasFlag(L"--data-popup");
	}
	return true;
}

void FDataStructEditor::RefreshUsers(FAssetEditorEnvironment& Env)
{
	Users = Env.Editor != nullptr ? FDataLibrary::FindStructUsers(Env.Editor->ContentDirectory, GetAssetPathString()) : std::vector<std::filesystem::path>();
}

bool FDataStructEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	if (Env.Editor == nullptr)
	{
		return false;
	}
	RefreshUsers(Env);
	if (!Users.empty() && !bMigrationConfirmed)
	{
		bOpenMigrateConfirm = true; // 확인 대화상자 → 확인하면 다시 Save
		bSaveDeferred       = true;
		return false;
	}
	bMigrationConfirmed = false;

	std::vector<std::filesystem::path> Changed;
	std::string                        Error;
	if (!FDataLibrary::Get().SaveStructAndMigrate(GetAssetPathString(), Struct, Renames, Env.Editor->ContentDirectory, &Changed, &Error))
	{
		Notify(Env, "데이터 구조체 저장 실패: " + Error, true);
		return false;
	}
	if (Changed.size() > 1)
	{
		Notify(Env, std::format("구조체 저장: 사용 파일 {}개를 새 구조체에 맞춰 다시 저장했습니다", Changed.size() - 1), false);
	}
	// 이름 변경이 파일에 적용됐다 → 기록을 새로 시작 (저장 전 상태로 되돌려 다시 저장하면 이름 변경이 두 번 적용되므로)
	Renames.clear();
	History.Reset(CaptureState());
	RememberDiskState();
	return true;
}

std::string FDataStructEditor::CaptureState() const
{
	FJson State;
	State["Struct"] = FJson::parse(Struct.ToJsonString(), nullptr, false);
	FJson RenameList = FJson::array();
	for (const FDataFieldRename& Rename : Renames)
	{
		RenameList.push_back(FJson::array({ Rename.From, Rename.To }));
	}
	State["Renames"] = std::move(RenameList);
	return State.dump(1);
}

void FDataStructEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	const FJson Root = FJson::parse(State, nullptr, false);
	if (Root.is_discarded() || !Root.is_object() || !Root.contains("Struct"))
	{
		return;
	}
	FDataStruct Restored;
	if (!FDataStruct::FromJsonString(Root["Struct"].dump(), Restored))
	{
		return;
	}
	Struct = std::move(Restored);
	Renames.clear();
	if (const auto It = Root.find("Renames"); It != Root.end() && It->is_array())
	{
		for (const FJson& Item : *It)
		{
			if (Item.is_array() && Item.size() == 2 && Item[0].is_string() && Item[1].is_string())
			{
				Renames.push_back({ Item[0].get<std::string>(), Item[1].get<std::string>() });
			}
		}
	}
	if (SelectedField >= static_cast<int32>(Struct.Fields.size()))
	{
		SelectedField = static_cast<int32>(Struct.Fields.size()) - 1;
	}
}

// ---------------------------------------------------------------- 편집

bool FDataStructEditor::ApplyField(int32 Index, FDataField Field, std::string_view Label)
{
	std::string Error;
	if (!Struct.SetField(Index, std::move(Field), &Error))
	{
		FieldError = Error;
		return false;
	}
	FieldError.clear();
	MarkEdited(Label);
	return true;
}

bool FDataStructEditor::RenameField(int32 Index, const std::string& NewName)
{
	if (Index < 0 || Index >= static_cast<int32>(Struct.Fields.size()))
	{
		return false;
	}
	const std::string OldName = Struct.Fields[static_cast<size_t>(Index)].Name;
	std::string       Error;
	if (!Struct.RenameField(OldName, NewName, &Error))
	{
		FieldError = Error;
		return false;
	}
	FieldError.clear();
	Renames.push_back({ OldName, NewName });
	MarkEdited("필드 이름 바꾸기");
	return true;
}

void FDataStructEditor::ChangeType(int32 Index, EDataFieldType Type, EDataFieldType ElementType)
{
	const FDataField& Old   = Struct.Fields[static_cast<size_t>(Index)];
	FDataField        Field = Old;
	Field.Type              = Type;
	Field.ElementType       = ElementType == EDataFieldType::Array ? EDataFieldType::Float : ElementType;
	if (GetValueType(Field) == EDataFieldType::Enum && Field.EnumValues.empty())
	{
		Field.EnumValues = { "None" };
	}
	// 기본값은 가능하면 변환 (실패하면 타입 기본값)
	Field.Default = DataValueText::Convert(Old, Field, Old.Default);
	if (!Field.Accepts(Field.Default))
	{
		Field.Default = FDataField::MakeTypeDefault(Field.Type, Field.EnumValues);
	}
	ApplyField(Index, std::move(Field), "필드 타입 바꾸기");
}

void FDataStructEditor::AddField()
{
	FDataField Field;
	Field.Name    = Struct.MakeUniqueFieldName("NewField");
	Field.Type    = EDataFieldType::Float;
	Field.Default = FDataField::MakeTypeDefault(Field.Type, {});
	const int32 Index = SelectedField >= 0 ? SelectedField + 1 : -1;
	std::string Error;
	if (Struct.AddField(std::move(Field), Index, &Error))
	{
		SelectedField = Index >= 0 ? Index : static_cast<int32>(Struct.Fields.size()) - 1;
		MarkEdited("필드 추가");
	}
	else
	{
		FieldError = Error;
	}
}

void FDataStructEditor::DuplicateField(int32 Index)
{
	if (Index < 0 || Index >= static_cast<int32>(Struct.Fields.size()))
	{
		return;
	}
	FDataField Copy = Struct.Fields[static_cast<size_t>(Index)];
	Copy.Name       = Struct.MakeUniqueFieldName(Copy.Name);
	if (Struct.AddField(std::move(Copy), Index + 1, &FieldError))
	{
		SelectedField = Index + 1;
		MarkEdited("필드 복제");
	}
}

void FDataStructEditor::RemoveField(int32 Index)
{
	if (Index < 0 || Index >= static_cast<int32>(Struct.Fields.size()))
	{
		return;
	}
	Struct.RemoveField(Struct.Fields[static_cast<size_t>(Index)].Name);
	SelectedField = std::min(Index, static_cast<int32>(Struct.Fields.size()) - 1);
	MarkEdited("필드 삭제");
}

void FDataStructEditor::MoveField(int32 From, int32 To)
{
	if (Struct.MoveField(From, To))
	{
		SelectedField = To;
		MarkEdited("필드 순서");
	}
}

// ---------------------------------------------------------------- 그리기

bool FDataStructEditor::DrawTypeCombo(const char* Id, EDataFieldType& InOutType, bool bElement)
{
	bool bChanged = false;
	if (ImGui::BeginCombo(Id, ToString(InOutType)))
	{
		for (int32 Index = 0; Index <= static_cast<int32>(LastFieldType); ++Index)
		{
			const EDataFieldType Type = static_cast<EDataFieldType>(Index);
			if (bElement && Type == EDataFieldType::Array)
			{
				continue;
			}
			if (ImGui::Selectable(GetTypeLabel(Type), Type == InOutType))
			{
				bChanged  = Type != InOutType;
				InOutType = Type;
			}
		}
		ImGui::EndCombo();
	}
	return bChanged;
}

void FDataStructEditor::DrawFieldList(FAssetEditorEnvironment& Env)
{
	const int32 Count = static_cast<int32>(Struct.Fields.size());
	if (ImGui::Button(ICON_FA_PLUS " 필드 추가"))
	{
		AddField();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedField < 0);
	if (ImGui::Button(ICON_FA_CLONE))
	{
		DuplicateField(SelectedField);
	}
	ImGui::SetItemTooltip("필드 복제");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_TRASH_CAN))
	{
		RemoveField(SelectedField);
	}
	ImGui::SetItemTooltip("필드 삭제 (이 구조체를 쓰는 파일의 그 값도 저장 시 사라집니다)");
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedField <= 0);
	if (ImGui::ArrowButton("##FieldUp", ImGuiDir_Up))
	{
		MoveField(SelectedField, SelectedField - 1);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedField < 0 || SelectedField + 1 >= Count);
	if (ImGui::ArrowButton("##FieldDown", ImGuiDir_Down))
	{
		MoveField(SelectedField, SelectedField + 1);
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("필드 %d개", Count);
	if (!FieldError.empty())
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_CIRCLE_EXCLAMATION " %s", FieldError.c_str());
	}

	const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
	                              ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("##Fields", 5, Flags, ImGui::GetContentRegionAvail()))
	{
		return;
	}
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.8f);
	ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	ImGui::TableSetupColumn("타입", ImGuiTableColumnFlags_WidthStretch, 0.7f);
	ImGui::TableSetupColumn("기본값", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("설명", ImGuiTableColumnFlags_WidthStretch, 1.2f);
	ImGui::TableHeadersRow();

	int32 PendingRename = -1;
	std::string RenameTo;
	for (int32 Index = 0; Index < static_cast<int32>(Struct.Fields.size()); ++Index)
	{
		const FDataField& Field = Struct.Fields[static_cast<size_t>(Index)];
		ImGui::PushID(Index);
		ImGui::TableNextRow();
		if (Index == SelectedField)
		{
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
		}
		ImGui::TableNextColumn();
		if (ImGui::Selectable(std::to_string(Index).c_str(), Index == SelectedField, ImGuiSelectableFlags_AllowOverlap))
		{
			SelectedField = Index;
		}

		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (std::string NewName; DataValueWidgets::InputTextCommit("##Name", Field.Name, NewName))
		{
			PendingRename = Index;
			RenameTo      = NewName;
		}
		if (ImGui::IsItemActive())
		{
			SelectedField = Index;
		}

		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-FLT_MIN);
		const std::string TypeLabel = Field.Type == EDataFieldType::Array ? std::format("Array<{}>", ToString(Field.ElementType)) : ToString(Field.Type);
		if (ImGui::BeginCombo("##Type", TypeLabel.c_str()))
		{
			SelectedField = Index;
			for (int32 TypeIndex = 0; TypeIndex <= static_cast<int32>(LastFieldType); ++TypeIndex)
			{
				const EDataFieldType Type = static_cast<EDataFieldType>(TypeIndex);
				if (ImGui::Selectable(GetTypeLabel(Type), Type == Field.Type) && Type != Field.Type)
				{
					ChangeType(Index, Type, Type == EDataFieldType::Array ? Field.Type : Field.ElementType);
				}
			}
			ImGui::EndCombo();
		}

		ImGui::TableNextColumn();
		{
			const FDataField&  Current = Struct.Fields[static_cast<size_t>(Index)]; // ChangeType 뒤 최신
			FDataValue         Value   = Current.Default;
			FDataWidgetContext Context;
			Context.Editor     = Env.Editor;
			Context.bCompact   = true;
			Context.bOpenPopup = bAutoPopup && Index == SelectedField; // 자동 검증: 기본값 배열/글자 팝업
			bAutoPopup         = bAutoPopup && !Context.bOpenPopup;
			ImGui::PushItemWidth(-FLT_MIN);
			ImGui::BeginGroup();
			if (DataValueWidgets::Draw(Current, Value, Context))
			{
				FDataField Changed = Current;
				Changed.Default    = Value;
				ApplyField(Index, std::move(Changed), "기본값 편집");
			}
			ImGui::EndGroup();
			ImGui::PopItemWidth();
			if (ImGui::IsItemActive())
			{
				SelectedField = Index;
			}
		}

		ImGui::TableNextColumn();
		{
			std::string Description = Struct.Fields[static_cast<size_t>(Index)].Description;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (DataValueWidgets::InputString("##Description", Description))
			{
				FDataField Changed  = Struct.Fields[static_cast<size_t>(Index)];
				Changed.Description = Description;
				ApplyField(Index, std::move(Changed), "필드 설명");
			}
			if (ImGui::IsItemActive())
			{
				SelectedField = Index;
			}
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
	if (PendingRename >= 0)
	{
		RenameField(PendingRename, RenameTo);
	}
}

void FDataStructEditor::DrawFieldDetails(FAssetEditorEnvironment& Env)
{
	if (!ImGui::CollapsingHeader(ICON_FA_SLIDERS " 선택한 필드", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	if (SelectedField < 0 || SelectedField >= static_cast<int32>(Struct.Fields.size()))
	{
		ImGui::TextDisabled("왼쪽 목록에서 필드를 고르세요");
		return;
	}
	const int32       Index = SelectedField;
	const FDataField& Field = Struct.Fields[static_cast<size_t>(Index)];
	ImGui::PushID("FieldDetails");

	if (std::string NewName; DataValueWidgets::InputTextCommit("이름", Field.Name, NewName))
	{
		RenameField(Index, NewName);
		ImGui::PopID();
		return;
	}
	ImGui::SetItemTooltip("영문/숫자/_ (숫자로 시작 불가, \"Name\"은 예약어). 저장하면 사용 파일의 값도 새 이름으로 옮겨집니다");

	EDataFieldType Type = Field.Type;
	if (DrawTypeCombo("타입", Type, false))
	{
		ChangeType(Index, Type, Type == EDataFieldType::Array ? Field.Type : Field.ElementType);
		ImGui::PopID();
		return;
	}
	if (Field.Type == EDataFieldType::Array)
	{
		EDataFieldType Element = Field.ElementType;
		if (DrawTypeCombo("요소 타입", Element, true))
		{
			ChangeType(Index, EDataFieldType::Array, Element);
			ImGui::PopID();
			return;
		}
	}

	const EDataFieldType ValueType = GetValueType(Field);
	if (ValueType == EDataFieldType::Enum)
	{
		ImGui::SeparatorText("Enum 값");
		std::vector<std::string> Values  = Field.EnumValues;
		bool                     bChange = false;
		int32                    Remove  = -1;
		for (size_t ValueIndex = 0; ValueIndex < Values.size(); ++ValueIndex)
		{
			ImGui::PushID(static_cast<int>(ValueIndex));
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 3.0f);
			if (std::string NewValue; DataValueWidgets::InputTextCommit("##Value", Values[ValueIndex], NewValue))
			{
				Values[ValueIndex] = NewValue;
				bChange            = true;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(ValueIndex == 0);
			if (ImGui::ArrowButton("##Up", ImGuiDir_Up))
			{
				std::swap(Values[ValueIndex], Values[ValueIndex - 1]);
				bChange = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(ValueIndex + 1 == Values.size());
			if (ImGui::ArrowButton("##Down", ImGuiDir_Down))
			{
				std::swap(Values[ValueIndex], Values[ValueIndex + 1]);
				bChange = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Values.size() <= 1);
			if (ImGui::Button(ICON_FA_XMARK))
			{
				Remove = static_cast<int32>(ValueIndex);
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		if (ImGui::Button(ICON_FA_PLUS " 값 추가"))
		{
			std::string Name = "Value";
			for (int32 Suffix = static_cast<int32>(Values.size()) + 1; std::find(Values.begin(), Values.end(), Name) != Values.end(); ++Suffix)
			{
				Name = std::format("Value{}", Suffix);
			}
			Values.push_back(Name);
			bChange = true;
		}
		if (Remove >= 0)
		{
			Values.erase(Values.begin() + Remove);
			bChange = true;
		}
		FAssetEditorWidgets::Hint("목록 순서가 표의 정렬 순서입니다. 값 이름을 바꾸거나 지우면 그 값을 쓰던 칸은 기본값이 됩니다 (Enum 값 이름 마이그레이션 미지원).");
		if (bChange)
		{
			FDataField Changed = Field;
			Changed.EnumValues = std::move(Values);
			ApplyField(Index, std::move(Changed), "Enum 값 편집");
			ImGui::PopID();
			return;
		}
	}
	if (ValueType == EDataFieldType::Asset)
	{
		std::string Filter = Field.Filter;
		if (DataValueWidgets::InputString("필터", Filter))
		{
			FDataField Changed = Field;
			Changed.Filter     = Filter;
			ApplyField(Index, std::move(Changed), "에셋 필터");
			ImGui::PopID();
			return;
		}
		ImGui::SetItemTooltip("허용 확장자 (예: .png;.jpg). 비우면 모든 파일");
	}
	if (ValueType == EDataFieldType::RowRef)
	{
		if (Env.Editor != nullptr && ImGui::BeginCombo("대상 테이블", Field.Table.empty() ? "(지정 안 함)" : Field.Table.c_str(), ImGuiComboFlags_HeightLarge))
		{
			for (const std::string& TablePath : DataValueWidgets::ScanContentFiles(Env.Editor->ContentDirectory, FDataTable::Extension))
			{
				if (ImGui::Selectable(TablePath.c_str(), TablePath == Field.Table) && TablePath != Field.Table)
				{
					FDataField Changed = Field;
					Changed.Table      = TablePath;
					ApplyField(Index, std::move(Changed), "RowRef 대상");
				}
			}
			ImGui::EndCombo();
		}
		if (Field.Table.empty())
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 대상 테이블을 지정하세요");
		}
	}

	ImGui::SeparatorText("기본값");
	{
		const FDataField&  Current = Struct.Fields[static_cast<size_t>(Index)];
		FDataValue         Value   = Current.Default;
		FDataWidgetContext Context;
		Context.Editor = Env.Editor;
		ImGui::PushItemWidth(-FLT_MIN);
		if (DataValueWidgets::Draw(Current, Value, Context))
		{
			FDataField Changed = Current;
			Changed.Default    = Value;
			ApplyField(Index, std::move(Changed), "기본값 편집");
		}
		ImGui::PopItemWidth();
	}
	ImGui::SeparatorText("설명");
	{
		std::string Description = Struct.Fields[static_cast<size_t>(Index)].Description;
		ImGui::PushItemWidth(-FLT_MIN);
		if (DataValueWidgets::InputString("##FieldDescription", Description))
		{
			FDataField Changed  = Struct.Fields[static_cast<size_t>(Index)];
			Changed.Description = Description;
			ApplyField(Index, std::move(Changed), "필드 설명");
		}
		ImGui::PopItemWidth();
	}
	ImGui::PopID();
}

void FDataStructEditor::DrawMigrateConfirm(FAssetEditorEnvironment& Env)
{
	if (bOpenMigrateConfirm)
	{
		ImGui::OpenPopup("구조체 저장##DataStructMigrate");
		bOpenMigrateConfirm = false;
	}
	if (!ImGui::BeginPopupModal("구조체 저장##DataStructMigrate", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	ImGui::Text("이 구조체를 쓰는 파일 %d개도 새 구조체에 맞춰 다시 저장됩니다.", static_cast<int32>(Users.size()));
	if (ImGui::BeginChild("##Users", ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetTextLineHeightWithSpacing() * std::min<float>(6.0f, static_cast<float>(Users.size()) + 0.5f)),
	                      ImGuiChildFlags_Borders))
	{
		for (const std::filesystem::path& User : Users)
		{
			ImGui::BulletText("%s", FPrefabLibrary::Get().MakeAssetPath(User).c_str());
		}
	}
	ImGui::EndChild();
	if (!Renames.empty())
	{
		ImGui::Text("필드 이름 변경 %d개는 값을 새 이름으로 옮깁니다.", static_cast<int32>(Renames.size()));
	}
	ImGui::TextDisabled("지운 필드의 값은 사라지고, 타입이 바뀐 값은 변환됩니다(실패하면 기본값).\n열린 편집 창의 저장하지 않은 변경은 디스크 변경 알림으로 표시됩니다.");
	if (ImGui::Button(ICON_FA_FLOPPY_DISK " 저장 및 마이그레이션"))
	{
		ImGui::CloseCurrentPopup();
		bMigrationConfirmed = true;
		Save(Env);
	}
	ImGui::SameLine();
	if (ImGui::Button("취소") || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void FDataStructEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	DrawFieldList(Env);
	DrawMigrateConfirm(Env);
}

void FDataStructEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	if (ImGui::CollapsingHeader(ICON_FA_TABLE_LIST " 구조체", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (DataValueWidgets::InputString("이름", Struct.Name))
		{
			MarkEdited("구조체 이름");
		}
		if (DataValueWidgets::InputString("설명", Struct.Description))
		{
			MarkEdited("구조체 설명");
		}
		for (const std::string& Warning : LoadWarnings)
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", Warning.c_str());
		}
	}
	DrawFieldDetails(Env);

	const std::string UsersHeader = std::format(ICON_FA_LINK " 이 구조체를 쓰는 파일 {}개###Users", Users.size());
	if (ImGui::CollapsingHeader(UsersHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::SmallButton(ICON_FA_ARROWS_ROTATE " 다시 찾기"))
		{
			RefreshUsers(Env);
		}
		for (const std::filesystem::path& User : Users)
		{
			const std::string Label = FPrefabLibrary::Get().MakeAssetPath(User);
			if (ImGui::Selectable(Label.c_str()) && Env.Editor != nullptr && Env.Editor->OpenAssetEditorRequest)
			{
				Env.Editor->OpenAssetEditorRequest(User);
			}
			ImGui::SetItemTooltip("눌러서 편집 창 열기");
		}
		FAssetEditorWidgets::Hint("저장하면 위 파일들을 새 구조체에 맞춰 함께 다시 씁니다 (이름 변경 값 이동, 지운 필드 값 삭제, 타입 변환).");
	}
	if (!Renames.empty() && ImGui::CollapsingHeader(ICON_FA_PEN " 저장 대기 중인 이름 변경", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (const FDataFieldRename& Rename : Renames)
		{
			ImGui::BulletText("%s \xE2\x86\x92 %s", Rename.From.c_str(), Rename.To.c_str());
		}
	}
}
