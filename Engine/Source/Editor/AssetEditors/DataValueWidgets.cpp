#include "Editor/AssetEditors/DataValueWidgets.h"

#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorManager.h"
#include "Editor/AssetEditors/DataTableView.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cwctype>
#include <format>
#include <map>

namespace
{
	int InputTextResize(ImGuiInputTextCallbackData* Data)
	{
		if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
		{
			auto* Text = static_cast<std::string*>(Data->UserData);
			Text->resize(static_cast<size_t>(Data->BufTextLen));
			Data->Buf = Text->data();
		}
		return 0;
	}

	bool InputMultiline(const char* Id, std::string& Text, const ImVec2& Size)
	{
		return ImGui::InputTextMultiline(Id, Text.data(), Text.capacity() + 1, Size, ImGuiInputTextFlags_CallbackResize, InputTextResize, &Text);
	}

	std::wstring LowerWide(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	std::string LowerExtensionUtf8(const std::filesystem::path& Path) { return FStringConv::ToUtf8(LowerWide(Path.extension().wstring())); }

	// 경고 칸: 테두리/배경을 경고 색으로 (Pop은 EndProblem)
	void BeginProblem(const FDataWidgetContext& Context)
	{
		if (Context.Problem != nullptr)
		{
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.38f, 0.08f, 0.55f));
			ImGui::PushStyleColor(ImGuiCol_Border, FEditorTheme::Warning);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		}
	}

	void EndProblem(const FDataWidgetContext& Context)
	{
		if (Context.Problem != nullptr)
		{
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(2);
		}
	}

	void ProblemTooltip(const FDataWidgetContext& Context, const char* Extra = nullptr)
	{
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			return;
		}
		if (Context.Problem == nullptr && Extra == nullptr)
		{
			return;
		}
		ImGui::BeginTooltip();
		if (Context.Problem != nullptr)
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", Context.Problem->c_str());
		}
		if (Extra != nullptr)
		{
			ImGui::TextDisabled("%s", Extra);
		}
		ImGui::EndTooltip();
	}

	bool DrawEnum(const FDataField& Field, FDataValue& Value)
	{
		const std::string& Current  = Value.AsString();
		bool               bChanged = false;
		if (ImGui::BeginCombo("##Enum", Current.empty() ? "(없음)" : Current.c_str()))
		{
			for (const std::string& Item : Field.EnumValues)
			{
				if (ImGui::Selectable(Item.c_str(), Item == Current))
				{
					Value    = FDataValue::MakeString(Item);
					bChanged = true;
				}
			}
			ImGui::EndCombo();
		}
		return bChanged;
	}

	bool DrawRowRef(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
	{
		const std::string& Current  = Value.AsString();
		bool               bChanged = false;
		BeginProblem(Context);
		const bool bOpen = ImGui::BeginCombo("##RowRef", Current.empty() ? "(없음)" : Current.c_str(), ImGuiComboFlags_HeightLarge);
		EndProblem(Context);
		if (!bOpen)
		{
			const std::string Extra = Field.Table.empty() ? std::string("대상 테이블이 지정되지 않았습니다") : "대상: " + Field.Table;
			ProblemTooltip(Context, Extra.c_str());
		}
		if (bOpen)
		{
			// 긴 목록용 검색 (콤보마다 하나, 열릴 때 비운다)
			static char Search[64] = {};
			if (ImGui::IsWindowAppearing())
			{
				Search[0] = '\0';
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " 행 검색", Search, sizeof(Search));
			if (ImGui::Selectable("(없음)", Current.empty()))
			{
				Value    = FDataValue::MakeString(std::string());
				bChanged = true;
			}
			const std::vector<std::string> Rows = DataValueWidgets::GetTargetRowNames(Field, Context);
			if (Rows.empty())
			{
				ImGui::TextDisabled(Field.Table.empty() ? "대상 테이블 없음" : "행이 없습니다");
			}
			for (const std::string& Row : Rows)
			{
				if (!DataTableView::ContainsInsensitive(Row, Search))
				{
					continue;
				}
				if (ImGui::Selectable(Row.c_str(), Row == Current))
				{
					Value    = FDataValue::MakeString(Row);
					bChanged = true;
				}
			}
			ImGui::EndCombo();
		}
		return bChanged;
	}

	bool DrawAsset(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
	{
		std::string Text     = Value.AsString();
		bool        bChanged = false;

		const FEditorTheme::FAssetStyle Style = FEditorTheme::GetAssetStyle(LowerExtensionUtf8(FStringConv::ToWide(Text)), false);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Style.Color));
		ImGui::TextUnformatted(Text.empty() ? ICON_FA_CIRCLE_XMARK : Style.Icon);
		ImGui::PopStyleColor();
		ImGui::SameLine(0.0f, 4.0f);

		const bool bCanOpen = !Text.empty() && !Context.bCompact && Context.Editor != nullptr && Context.Editor->OpenAssetEditorRequest;
		const float ButtonWidth = bCanOpen ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ButtonWidth, 40.0f));
		BeginProblem(Context);
		if (DataValueWidgets::InputString("##Asset", Text))
		{
			Value    = FDataValue::MakeString(Text);
			bChanged = true;
		}
		EndProblem(Context);
		const std::string Extra = Field.Filter.empty() ? std::string("콘텐츠 브라우저에서 파일을 끌어 놓을 수 있습니다")
		                                                : "콘텐츠 브라우저에서 " + Field.Filter + " 파일을 끌어 놓을 수 있습니다";
		ProblemTooltip(Context, Extra.c_str());
		if (ImGui::BeginDragDropTarget())
		{
			if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
			{
				const std::vector<std::string> Extensions = Field.GetFilterExtensions();
				const std::string              Dropped    = LowerExtensionUtf8(Paths->front());
				if (Extensions.empty() || std::find(Extensions.begin(), Extensions.end(), Dropped) != Extensions.end())
				{
					Value    = FDataValue::MakeString(FPrefabLibrary::Get().MakeAssetPath(Paths->front()));
					bChanged = true;
				}
				else if (Context.Editor != nullptr && Context.Editor->Notify)
				{
					Context.Editor->Notify(std::format("'{}' 필드는 {} 파일만 받습니다", Field.Name, Field.Filter), true);
				}
			}
			ImGui::EndDragDropTarget();
		}
		if (bCanOpen)
		{
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
			{
				const std::filesystem::path File = FPrefabLibrary::Get().ResolveAssetPath(Text);
				if (FAssetEditorManager::CanOpen(File))
				{
					Context.Editor->OpenAssetEditorRequest(File);
				}
				else if (Context.Editor->Notify)
				{
					Context.Editor->Notify("편집 창이 없는 형식입니다: " + Text, false);
				}
			}
			ImGui::SetItemTooltip("편집 창에서 열기");
		}
		return bChanged;
	}

	bool DrawText(FDataValue& Value, const FDataWidgetContext& Context)
	{
		std::string Text     = Value.AsString();
		bool        bChanged = false;
		if (!Context.bCompact)
		{
			if (InputMultiline("##Text", Text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f)))
			{
				Value    = FDataValue::MakeString(Text);
				bChanged = true;
			}
			return bChanged;
		}
		// 표 칸: 첫 줄 요약 버튼 → 여러 줄 편집 팝업
		const size_t      LineEnd = Text.find('\n');
		const std::string Summary = (LineEnd == std::string::npos ? Text : Text.substr(0, LineEnd) + " \xE2\x80\xA6") + "##TextButton";
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		if (ImGui::Button(Summary.c_str(), ImVec2(-FLT_MIN, 0.0f)) || Context.bOpenPopup)
		{
			ImGui::OpenPopup("##TextPopup");
		}
		ImGui::PopStyleVar();
		ImGui::SetItemTooltip("%s", Text.empty() ? "(빈 글자) 눌러서 편집" : Text.c_str());
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing); // 칸 바로 아래
		if (ImGui::BeginPopup("##TextPopup"))
		{
			if (ImGui::IsWindowAppearing())
			{
				ImGui::SetKeyboardFocusHere();
			}
			if (InputMultiline("##TextEdit", Text, ImVec2(ImGui::GetFontSize() * 28.0f, ImGui::GetTextLineHeight() * 8.0f)))
			{
				Value    = FDataValue::MakeString(Text);
				bChanged = true;
			}
			ImGui::TextDisabled("Esc 또는 바깥을 눌러 닫기");
			ImGui::EndPopup();
		}
		return bChanged;
	}

	bool DrawScalar(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context);

	// 배열 요소 목록 (추가/삭제/순서). 요소 위젯은 표 칸 모양
	bool DrawArrayElements(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
	{
		FDataValue::FArray Elements = Value.AsArray();
		const FDataField   Element  = Field.MakeElementField();
		bool               bChanged = false;
		int32              RemoveAt = -1;
		int32              MoveFrom = -1;
		int32              MoveTo   = -1;

		FDataWidgetContext ElementContext = Context;
		ElementContext.bCompact           = true;
		ElementContext.bOpenPopup         = false;
		ElementContext.Problem            = nullptr;

		const float ButtonWidth = ImGui::GetFrameHeight();
		for (size_t Index = 0; Index < Elements.size(); ++Index)
		{
			ImGui::PushID(static_cast<int>(Index));
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%2d", static_cast<int>(Index));
			ImGui::SameLine();
			const float Spacing = ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - (ButtonWidth + Spacing) * 3.0f, 60.0f));
			ImGui::BeginGroup();
			bChanged = DrawScalar(Element, Elements[Index], ElementContext) || bChanged;
			ImGui::EndGroup();
			ImGui::SameLine();
			ImGui::BeginDisabled(Index == 0);
			if (ImGui::Button(ICON_FA_ARROW_UP, ImVec2(ButtonWidth, 0.0f)))
			{
				MoveFrom = static_cast<int32>(Index);
				MoveTo   = MoveFrom - 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Index + 1 == Elements.size());
			if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(ButtonWidth, 0.0f)))
			{
				MoveFrom = static_cast<int32>(Index);
				MoveTo   = MoveFrom + 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_XMARK, ImVec2(ButtonWidth, 0.0f)))
			{
				RemoveAt = static_cast<int32>(Index);
			}
			ImGui::SetItemTooltip("요소 삭제");
			ImGui::PopID();
		}
		if (Elements.empty())
		{
			ImGui::TextDisabled("(빈 배열)");
		}
		if (ImGui::Button(ICON_FA_PLUS " 요소 추가"))
		{
			Elements.push_back(Elements.empty() ? FDataField::MakeTypeDefault(Element.Type, Element.EnumValues) : Elements.back());
			bChanged = true;
		}
		if (!Elements.empty())
		{
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_TRASH " 모두 지우기"))
			{
				Elements.clear();
				bChanged = true;
			}
		}
		if (RemoveAt >= 0)
		{
			Elements.erase(Elements.begin() + RemoveAt);
			bChanged = true;
		}
		if (MoveFrom >= 0)
		{
			std::swap(Elements[static_cast<size_t>(MoveFrom)], Elements[static_cast<size_t>(MoveTo)]);
			bChanged = true;
		}
		if (bChanged)
		{
			Value = FDataValue::MakeArray(std::move(Elements));
		}
		return bChanged;
	}

	bool DrawArray(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
	{
		if (!Context.bCompact)
		{
			BeginProblem(Context);
			ImGui::BeginGroup();
			const bool bChanged = DrawArrayElements(Field, Value, Context);
			ImGui::EndGroup();
			EndProblem(Context);
			ProblemTooltip(Context);
			return bChanged;
		}
		// 표 칸: 요약 버튼 → 요소 편집 팝업
		const std::string Summary = DataTableView::SummarizeArray(Field, Value) + "##ArrayButton";
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		BeginProblem(Context);
		if (ImGui::Button(Summary.c_str(), ImVec2(-FLT_MIN, 0.0f)) || Context.bOpenPopup)
		{
			ImGui::OpenPopup("##ArrayPopup");
		}
		EndProblem(Context);
		ImGui::PopStyleVar();
		ProblemTooltip(Context, "눌러서 요소 편집");
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing); // 칸 바로 아래
		bool bChanged = false;
		ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetFontSize() * 30.0f));
		if (ImGui::BeginPopup("##ArrayPopup"))
		{
			ImGui::Text("%s  " ICON_FA_LIST " %s 배열", Field.Name.c_str(), ToString(Field.ElementType));
			ImGui::Separator();
			bChanged = DrawArrayElements(Field, Value, Context);
			ImGui::EndPopup();
		}
		return bChanged;
	}

	bool DrawScalar(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
	{
		switch (Field.Type)
		{
		case EDataFieldType::Bool:
		{
			bool bValue = Value.AsBool();
			if (ImGui::Checkbox("##Bool", &bValue))
			{
				Value = FDataValue::MakeBool(bValue);
				return true;
			}
			return false;
		}
		case EDataFieldType::Int:
		{
			int Number = Value.AsInt();
			if (ImGui::DragInt("##Int", &Number, 0.2f))
			{
				Value = FDataValue::MakeInt(Number);
				return true;
			}
			return false;
		}
		case EDataFieldType::Float:
		{
			float Number = Value.AsFloat();
			if (ImGui::DragFloat("##Float", &Number, 0.05f, 0.0f, 0.0f, "%g"))
			{
				Value = FDataValue::MakeFloat(Number);
				return true;
			}
			return false;
		}
		case EDataFieldType::Vector2:
		{
			FVector2 Vector = Value.AsVector2();
			if (ImGui::DragFloat2("##V2", &Vector.X, 0.05f, 0.0f, 0.0f, "%g"))
			{
				Value = FDataValue::MakeVector2(Vector);
				return true;
			}
			return false;
		}
		case EDataFieldType::Vector3:
		{
			FVector3 Vector = Value.AsVector3();
			if (ImGui::DragFloat3("##V3", &Vector.X, 0.05f, 0.0f, 0.0f, "%g"))
			{
				Value = FDataValue::MakeVector3(Vector);
				return true;
			}
			return false;
		}
		case EDataFieldType::Vector4:
		{
			FVector4 Vector = Value.AsVector4();
			if (ImGui::DragFloat4("##V4", &Vector.X, 0.05f, 0.0f, 0.0f, "%g"))
			{
				Value = FDataValue::MakeVector4(Vector);
				return true;
			}
			return false;
		}
		case EDataFieldType::Color:
		{
			// sRGB 0~1 그대로 (변환 없음)
			FVector4               Color = Value.AsVector4();
			const ImGuiColorEditFlags Flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_Float;
			if (ImGui::ColorEdit4("##Color", &Color.X, Flags))
			{
				Value = FDataValue::MakeVector4(Color);
				return true;
			}
			return false;
		}
		case EDataFieldType::Enum: return DrawEnum(Field, Value);
		case EDataFieldType::Asset: return DrawAsset(Field, Value, Context);
		case EDataFieldType::RowRef: return DrawRowRef(Field, Value, Context);
		case EDataFieldType::Text: return DrawText(Value, Context);
		case EDataFieldType::Array: return false; // 요소는 배열이 아니다 (구조체 검증)
		default:
		{
			std::string Text = Value.AsString();
			BeginProblem(Context);
			const bool bChanged = DataValueWidgets::InputString("##String", Text);
			EndProblem(Context);
			ProblemTooltip(Context);
			if (bChanged)
			{
				Value = FDataValue::MakeString(Text);
			}
			return bChanged;
		}
		}
	}
} // namespace

bool DataValueWidgets::Draw(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context)
{
	// 저장 형식이 필드와 다르면(있어서는 안 됨) 기본값으로 보여 주고 고치면 바로잡힌다
	if (!Field.Accepts(Value))
	{
		Value = Field.Default;
	}
	if (Field.Type == EDataFieldType::Array)
	{
		return DrawArray(Field, Value, Context);
	}
	return DrawScalar(Field, Value, Context);
}

bool DataValueWidgets::InputString(const char* Id, std::string& Text, int Flags)
{
	return ImGui::InputText(Id, Text.data(), Text.capacity() + 1, Flags | ImGuiInputTextFlags_CallbackResize, InputTextResize, &Text);
}

bool DataValueWidgets::InputTextCommit(const char* Id, const std::string& Current, std::string& OutText, const char* Hint)
{
	// 활성 중에는 내부 버퍼(위젯 ID별 하나)를 편집하고, 끝난 프레임에 결과를 돌려준다
	static ImGuiID     EditingId = 0;
	static std::string Editing;
	const ImGuiID      WidgetId = ImGui::GetID(Id);
	std::string        Local    = EditingId == WidgetId ? Editing : Current;
	const int          Flags    = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AutoSelectAll;
	if (Hint != nullptr)
	{
		ImGui::InputTextWithHint(Id, Hint, Local.data(), Local.capacity() + 1, Flags, InputTextResize, &Local);
	}
	else
	{
		ImGui::InputText(Id, Local.data(), Local.capacity() + 1, Flags, InputTextResize, &Local);
	}
	if (ImGui::IsItemActive())
	{
		EditingId = WidgetId;
		Editing   = Local;
		return false;
	}
	if (EditingId == WidgetId)
	{
		EditingId = 0;
		if (ImGui::IsItemDeactivatedAfterEdit() && Local != Current)
		{
			OutText = Local;
			return true;
		}
	}
	return false;
}

const std::vector<std::string>& DataValueWidgets::ScanContentFiles(const std::filesystem::path& ContentDirectory, const std::wstring& Extension)
{
	struct FCacheEntry
	{
		std::vector<std::string>              Files;
		std::chrono::steady_clock::time_point Time;
		bool                                  bValid = false;
	};
	static std::map<std::wstring, FCacheEntry> Cache;
	FCacheEntry&                               Entry = Cache[LowerWide(ContentDirectory.wstring()) + L"|" + Extension];
	const auto                                 Now   = std::chrono::steady_clock::now();
	if (Entry.bValid && Now - Entry.Time < std::chrono::seconds(2))
	{
		return Entry.Files;
	}
	Entry.Files.clear();
	std::error_code ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		if (It->is_regular_file(ErrorCode) && LowerWide(It->path().extension().wstring()) == Extension)
		{
			Entry.Files.push_back(FPrefabLibrary::Get().MakeAssetPath(It->path()));
		}
	}
	std::sort(Entry.Files.begin(), Entry.Files.end());
	Entry.Time   = Now;
	Entry.bValid = true;
	return Entry.Files;
}

bool DataValueWidgets::IsSameDataFile(const std::string& AssetPath, const std::filesystem::path& File)
{
	if (AssetPath.empty() || File.empty())
	{
		return false;
	}
	return LowerWide(FPrefabLibrary::Get().ResolveAssetPath(AssetPath).lexically_normal().generic_wstring()) ==
	       LowerWide(File.lexically_normal().generic_wstring());
}

std::vector<std::string> DataValueWidgets::GetTargetRowNames(const FDataField& Field, const FDataWidgetContext& Context)
{
	if (Field.Table.empty())
	{
		return {};
	}
	if (Context.SelfTable != nullptr && IsSameDataFile(Field.Table, Context.SelfPath))
	{
		return Context.SelfTable->GetRowNames();
	}
	const std::shared_ptr<const FDataTable> Target = FDataLibrary::Get().LoadTable(Field.Table);
	return Target != nullptr ? Target->GetRowNames() : std::vector<std::string>();
}

std::string DataValueWidgets::DescribeField(const FDataField& Field)
{
	std::string Type = ToString(Field.Type);
	if (Field.Type == EDataFieldType::Array)
	{
		Type += std::format("<{}>", ToString(Field.ElementType));
	}
	const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
	if (ValueType == EDataFieldType::RowRef)
	{
		Type += " \xE2\x86\x92 " + (Field.Table.empty() ? std::string("(대상 없음)") : Field.Table);
	}
	else if (ValueType == EDataFieldType::Asset && !Field.Filter.empty())
	{
		Type += " (" + Field.Filter + ")";
	}
	return Field.Description.empty() ? Type : Type + "\n" + Field.Description;
}
