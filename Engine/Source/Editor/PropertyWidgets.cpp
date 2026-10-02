#include "Editor/PropertyWidgets.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorTheme.h"
#include "Renderer/ModelLoader.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
	float DragSpeed(const FPropertyInfo& Property, float Default)
	{
		return Property.Step > 0.0f ? Property.Step : Default;
	}

	bool DrawText(const char* Label, std::string& Value)
	{
		char Buffer[512];
		strncpy_s(Buffer, sizeof(Buffer), Value.c_str(), _TRUNCATE);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	// 선택지 콤보 (값이 비면 첫 선택지로 표시, 목록에 없는 값은 그대로 보이고 경고 색)
	bool DrawStringOptions(const char* Label, std::string& Value, const std::vector<std::string>& Options)
	{
		const bool        bKnown  = Value.empty() || std::find(Options.begin(), Options.end(), Value) != Options.end();
		const std::string Preview = Value.empty() ? (Options.empty() ? std::string() : Options.front()) : (bKnown ? Value : Value + " (없음)");
		bool              bChanged = false;
		if (!bKnown)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Warning);
		}
		const bool bOpen = ImGui::BeginCombo(Label, Preview.c_str());
		if (!bKnown)
		{
			ImGui::PopStyleColor();
		}
		if (bOpen)
		{
			for (size_t Index = 0; Index < Options.size(); ++Index)
			{
				const bool bSelected = Value == Options[Index] || (Value.empty() && Index == 0);
				if (ImGui::Selectable(Options[Index].c_str(), bSelected) && Value != Options[Index])
				{
					Value    = Options[Index];
					bChanged = true;
				}
			}
			ImGui::EndCombo();
		}
		return bChanged;
	}

	bool DrawWidget(const FPropertyInfo& Property, void* Object, const char* Label)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:
			return ImGui::Checkbox(Label, &Property.GetRef<bool>(Object));

		case EPropertyType::Int32:
		{
			int32& Value = Property.GetRef<int32>(Object);
			if (Property.IsEnum())
			{
				const int32 Count   = static_cast<int32>(Property.EnumEntries.size());
				const char* Preview = Value >= 0 && Value < Count ? Property.EnumEntries[static_cast<size_t>(Value)].DisplayName.c_str() : "?";
				bool        bChanged = false;
				if (ImGui::BeginCombo(Label, Preview))
				{
					for (int32 Index = 0; Index < Count; ++Index)
					{
						if (ImGui::Selectable(Property.EnumEntries[static_cast<size_t>(Index)].DisplayName.c_str(), Index == Value) && Index != Value)
						{
							Value    = Index;
							bChanged = true;
						}
					}
					ImGui::EndCombo();
				}
				return bChanged;
			}
			if (Property.HasRange())
			{
				return ImGui::SliderInt(Label, &Value, static_cast<int>(Property.MinValue), static_cast<int>(Property.MaxValue));
			}
			return ImGui::DragInt(Label, &Value, DragSpeed(Property, 1.0f));
		}

		case EPropertyType::UInt32:
		{
			uint32&      Value = Property.GetRef<uint32>(Object);
			const uint32 Min   = static_cast<uint32>(std::max(Property.MinValue, 0.0f));
			const uint32 Max   = static_cast<uint32>(Property.MaxValue);
			return ImGui::DragScalar(Label, ImGuiDataType_U32, &Value, DragSpeed(Property, 1.0f), Property.HasRange() ? &Min : nullptr,
			                         Property.HasRange() ? &Max : nullptr, nullptr, Property.HasRange() ? ImGuiSliderFlags_AlwaysClamp : 0);
		}

		case EPropertyType::Float:
		{
			float& Value = Property.GetRef<float>(Object);
			if (Property.HasRange())
			{
				return ImGui::DragFloat(Label, &Value, DragSpeed(Property, 0.05f), Property.MinValue, Property.MaxValue, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			}
			return ImGui::DragFloat(Label, &Value, DragSpeed(Property, 0.05f));
		}

		case EPropertyType::String:
			if (Property.StringOptions)
			{
				return DrawStringOptions(Label, Property.GetRef<std::string>(Object), Property.StringOptions());
			}
			return DrawText(Label, Property.GetRef<std::string>(Object));

		case EPropertyType::Vector2:
			return ImGui::DragFloat2(Label, &Property.GetRef<FVector2>(Object).X, DragSpeed(Property, 0.05f));

		case EPropertyType::Vector3:
		{
			FVector3& Value = Property.GetRef<FVector3>(Object);
			if (Property.HasFlag(PF_Color))
			{
				return ImGui::ColorEdit3(Label, &Value.X);
			}
			if (Property.HasRange())
			{
				return ImGui::DragFloat3(Label, &Value.X, DragSpeed(Property, 0.05f), Property.MinValue, Property.MaxValue);
			}
			return ImGui::DragFloat3(Label, &Value.X, DragSpeed(Property, 0.05f));
		}

		case EPropertyType::Vector4:
		{
			FVector4& Value = Property.GetRef<FVector4>(Object);
			if (Property.HasFlag(PF_Color))
			{
				return ImGui::ColorEdit4(Label, &Value.X);
			}
			return ImGui::DragFloat4(Label, &Value.X, DragSpeed(Property, 0.05f));
		}

		default:
			return false;
		}
	}
} // namespace

bool FPropertyWidgets::IsValueType(EPropertyType Type)
{
	switch (Type)
	{
	case EPropertyType::Bool:
	case EPropertyType::Int32:
	case EPropertyType::UInt32:
	case EPropertyType::Float:
	case EPropertyType::String:
	case EPropertyType::Vector2:
	case EPropertyType::Vector3:
	case EPropertyType::Vector4:
		return true;
	default:
		return false;
	}
}

bool FPropertyWidgets::DrawValue(const FPropertyInfo& Property, void* Object, const char* Label)
{
	const bool bChanged = DrawWidget(Property, Object, Label != nullptr ? Label : Property.DisplayName.c_str());
	if (!Property.Tooltip.empty())
	{
		ImGui::SetItemTooltip("%s", Property.Tooltip.c_str());
	}
	return bChanged;
}

bool FPropertyWidgets::DrawAssetPath(const FPropertyInfo& Property, void* Object, const char* Label, bool* bOutRejected)
{
	std::string& Value    = Property.GetRef<std::string>(Object);
	bool         bChanged = DrawText(Label, Value);
	ImGui::SetItemTooltip("%s%s콘텐츠 브라우저에서 %s 파일을 끌어 놓을 수 있습니다", Property.Tooltip.c_str(), Property.Tooltip.empty() ? "" : "\n", Property.AssetFilter.c_str());

	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			std::wstring Dropped = Paths->front().extension().wstring();
			std::transform(Dropped.begin(), Dropped.end(), Dropped.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
			const std::string Filter = ";" + Property.AssetFilter + ";";
			if (Filter.find(";" + FStringConv::ToUtf8(Dropped) + ";") != std::string::npos)
			{
				Value    = FModelLoader::MakeAssetPath(Paths->front());
				bChanged = true;
			}
			else if (bOutRejected != nullptr)
			{
				*bOutRejected = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	return bChanged;
}
