#include "UI/UIAsset.h"

#include "Core/StringConv.h"

#include <json.hpp>

#include <fstream>
#include <sstream>

namespace
{
	using nlohmann::json;

	json ToJson(const FVector2& Value) { return json::array({ Value.X, Value.Y }); }
	json ToJson(const FVector4& Value) { return json::array({ Value.X, Value.Y, Value.Z, Value.W }); }
	json ToJson(const FUIMargin& Value) { return json::array({ Value.Left, Value.Top, Value.Right, Value.Bottom }); }

	bool ReadFloats(const json& Object, const char* Key, float* Out, size_t Count)
	{
		const auto It = Object.find(Key);
		if (It == Object.end() || !It->is_array() || It->size() != Count)
		{
			return false;
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (!(*It)[Index].is_number())
			{
				return false;
			}
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			Out[Index] = (*It)[Index].get<float>();
		}
		return true;
	}
	void Read(const json& Object, const char* Key, FVector2& Out) { ReadFloats(Object, Key, &Out.X, 2); }
	void Read(const json& Object, const char* Key, FVector4& Out) { ReadFloats(Object, Key, &Out.X, 4); }
	void Read(const json& Object, const char* Key, FUIMargin& Out) { ReadFloats(Object, Key, &Out.Left, 4); }

	template <typename T>
	void Read(const json& Object, const char* Key, T& Out)
		requires(std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
	{
		const auto It = Object.find(Key);
		if (It == Object.end())
		{
			return;
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			if (It->is_boolean())
			{
				Out = It->get<bool>();
			}
		}
		else if constexpr (std::is_arithmetic_v<T>)
		{
			if (It->is_number())
			{
				Out = It->get<T>();
			}
		}
		else if (It->is_string())
		{
			Out = It->get<std::string>();
		}
	}

	template <typename TEnum>
	void ReadEnum(const json& Object, const char* Key, TEnum& Out)
	{
		const auto It = Object.find(Key);
		if (It != Object.end() && It->is_string())
		{
			FromString(It->get<std::string>(), Out);
		}
	}

	json BrushToJson(const FUIBrush& Brush)
	{
		json Object;
		Object["Color"] = ToJson(Brush.Color);
		if (!Brush.Texture.empty())
		{
			Object["Texture"] = Brush.Texture;
		}
		Object["CornerRadius"] = Brush.CornerRadius;
		Object["BorderWidth"]  = Brush.BorderWidth;
		Object["BorderColor"]  = ToJson(Brush.BorderColor);
		return Object;
	}

	void ReadBrush(const json& Object, const char* Key, FUIBrush& Out)
	{
		const auto It = Object.find(Key);
		if (It == Object.end() || !It->is_object())
		{
			return;
		}
		Read(*It, "Color", Out.Color);
		Read(*It, "Texture", Out.Texture);
		Read(*It, "CornerRadius", Out.CornerRadius);
		Read(*It, "BorderWidth", Out.BorderWidth);
		Read(*It, "BorderColor", Out.BorderColor);
	}

	json SlotToJson(const FUISlot& Slot, EUIWidgetType ParentType)
	{
		json Object;
		switch (ParentType)
		{
		case EUIWidgetType::Canvas:
			Object["AnchorMin"] = ToJson(Slot.AnchorMin);
			Object["AnchorMax"] = ToJson(Slot.AnchorMax);
			Object["Offsets"]   = ToJson(Slot.Offsets);
			Object["Alignment"] = ToJson(Slot.Alignment);
			Object["AutoSize"]  = Slot.bAutoSize;
			Object["ZOrder"]    = Slot.ZOrder;
			break;
		case EUIWidgetType::HorizontalBox:
		case EUIWidgetType::VerticalBox:
			Object["SizeRule"]   = ToString(Slot.SizeRule);
			Object["FillWeight"] = Slot.FillWeight;
			[[fallthrough]];
		case EUIWidgetType::Overlay:
		case EUIWidgetType::ScrollBox:
		case EUIWidgetType::Border:
		case EUIWidgetType::Button:
			Object["Padding"] = ToJson(Slot.Padding);
			Object["HAlign"]  = ToString(Slot.HAlign);
			Object["VAlign"]  = ToString(Slot.VAlign);
			break;
		case EUIWidgetType::UniformGrid:
			Object["Row"]     = Slot.Row;
			Object["Column"]  = Slot.Column;
			Object["Padding"] = ToJson(Slot.Padding);
			Object["HAlign"]  = ToString(Slot.HAlign);
			Object["VAlign"]  = ToString(Slot.VAlign);
			break;
		default:
			break;
		}
		return Object;
	}

	void ReadSlot(const json& Object, FUISlot& Slot)
	{
		Read(Object, "AnchorMin", Slot.AnchorMin);
		Read(Object, "AnchorMax", Slot.AnchorMax);
		Read(Object, "Offsets", Slot.Offsets);
		Read(Object, "Alignment", Slot.Alignment);
		Read(Object, "AutoSize", Slot.bAutoSize);
		Read(Object, "ZOrder", Slot.ZOrder);
		Read(Object, "Padding", Slot.Padding);
		ReadEnum(Object, "HAlign", Slot.HAlign);
		ReadEnum(Object, "VAlign", Slot.VAlign);
		ReadEnum(Object, "SizeRule", Slot.SizeRule);
		Read(Object, "FillWeight", Slot.FillWeight);
		Read(Object, "Row", Slot.Row);
		Read(Object, "Column", Slot.Column);
	}

	// ParentType이 Count면 슬롯을 쓰지 않는다 (루트)
	json WidgetToJson(const FUIWidget& Widget, EUIWidgetType ParentType, bool bForceSlot)
	{
		json Object;
		Object["Type"]       = ToString(Widget.Type);
		Object["Name"]       = Widget.Name;
		Object["Visibility"] = ToString(Widget.Visibility);
		Object["Enabled"]    = Widget.bEnabled;
		Object["Opacity"]    = Widget.RenderOpacity;
		if (Widget.MinSize != FVector2::ZeroVector)
		{
			Object["MinSize"] = ToJson(Widget.MinSize);
		}
		if (ParentType != EUIWidgetType::Count)
		{
			Object["Slot"] = SlotToJson(Widget.Slot, ParentType);
		}
		else if (bForceSlot)
		{
			// 복사한 하위 트리: 어떤 부모에 붙을지 모르므로 슬롯 전체
			json Slot = SlotToJson(Widget.Slot, EUIWidgetType::Canvas);
			Slot.update(SlotToJson(Widget.Slot, EUIWidgetType::HorizontalBox));
			Slot.update(SlotToJson(Widget.Slot, EUIWidgetType::UniformGrid));
			Object["Slot"] = std::move(Slot);
		}

		switch (Widget.Type)
		{
		case EUIWidgetType::Border:
			Object["Brush"]          = BrushToJson(Widget.Brush);
			Object["ContentPadding"] = ToJson(Widget.ContentPadding);
			break;
		case EUIWidgetType::Image:
			Object["Brush"]     = BrushToJson(Widget.Brush);
			Object["ImageSize"] = ToJson(Widget.ImageSize);
			break;
		case EUIWidgetType::Text:
			Object["Text"] = Widget.Text;
			if (!Widget.Font.empty())
			{
				Object["Font"] = Widget.Font;
			}
			Object["FontSize"]     = Widget.FontSize;
			Object["TextColor"]    = ToJson(Widget.TextColor);
			Object["Justify"]      = ToString(Widget.Justify);
			Object["Wrap"]         = Widget.bWrap;
			Object["OutlineWidth"] = Widget.OutlineWidth;
			Object["OutlineColor"] = ToJson(Widget.OutlineColor);
			Object["ShadowOffset"] = ToJson(Widget.ShadowOffset);
			Object["ShadowColor"]  = ToJson(Widget.ShadowColor);
			break;
		case EUIWidgetType::Button:
			Object["Brush"]          = BrushToJson(Widget.Brush);
			Object["HoveredBrush"]   = BrushToJson(Widget.HoveredBrush);
			Object["PressedBrush"]   = BrushToJson(Widget.PressedBrush);
			Object["DisabledBrush"]  = BrushToJson(Widget.DisabledBrush);
			Object["ContentPadding"] = ToJson(Widget.ContentPadding);
			break;
		case EUIWidgetType::ProgressBar:
			Object["Brush"]         = BrushToJson(Widget.Brush);
			Object["FillBrush"]     = BrushToJson(Widget.FillBrush);
			Object["Percent"]       = Widget.Percent;
			Object["FillDirection"] = ToString(Widget.FillDirection);
			break;
		case EUIWidgetType::ScrollBox:
			Object["Orientation"]    = ToString(Widget.Orientation);
			Object["ScrollbarWidth"] = Widget.ScrollbarWidth;
			Object["ScrollbarColor"] = ToJson(Widget.ScrollbarColor);
			break;
		case EUIWidgetType::UniformGrid:
			Object["MinCellSize"] = ToJson(Widget.MinCellSize);
			break;
		default:
			break;
		}

		if (!Widget.Children.empty())
		{
			json Children = json::array();
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				Children.push_back(WidgetToJson(*Child, Widget.Type, false));
			}
			Object["Children"] = std::move(Children);
		}
		return Object;
	}

	std::unique_ptr<FUIWidget> WidgetFromJson(const json& Object)
	{
		if (!Object.is_object())
		{
			return nullptr;
		}
		EUIWidgetType Type = EUIWidgetType::Count;
		const auto    It   = Object.find("Type");
		if (It == Object.end() || !It->is_string() || !FromString(It->get<std::string>(), Type))
		{
			E_LOG(LogUI, Warning, "알 수 없는 위젯 종류를 건너뜁니다: {}", It != Object.end() ? It->dump() : "(없음)");
			return nullptr;
		}
		std::unique_ptr<FUIWidget> Widget = FUIWidget::Create(Type);
		Read(Object, "Name", Widget->Name);
		ReadEnum(Object, "Visibility", Widget->Visibility);
		Read(Object, "Enabled", Widget->bEnabled);
		Read(Object, "Opacity", Widget->RenderOpacity);
		Read(Object, "MinSize", Widget->MinSize);
		if (const auto SlotIt = Object.find("Slot"); SlotIt != Object.end() && SlotIt->is_object())
		{
			ReadSlot(*SlotIt, Widget->Slot);
		}
		ReadBrush(Object, "Brush", Widget->Brush);
		Read(Object, "ContentPadding", Widget->ContentPadding);
		Read(Object, "ImageSize", Widget->ImageSize);
		Read(Object, "Text", Widget->Text);
		Read(Object, "Font", Widget->Font);
		Read(Object, "FontSize", Widget->FontSize);
		Read(Object, "TextColor", Widget->TextColor);
		ReadEnum(Object, "Justify", Widget->Justify);
		Read(Object, "Wrap", Widget->bWrap);
		Read(Object, "OutlineWidth", Widget->OutlineWidth);
		Read(Object, "OutlineColor", Widget->OutlineColor);
		Read(Object, "ShadowOffset", Widget->ShadowOffset);
		Read(Object, "ShadowColor", Widget->ShadowColor);
		ReadBrush(Object, "HoveredBrush", Widget->HoveredBrush);
		ReadBrush(Object, "PressedBrush", Widget->PressedBrush);
		ReadBrush(Object, "DisabledBrush", Widget->DisabledBrush);
		ReadBrush(Object, "FillBrush", Widget->FillBrush);
		Read(Object, "Percent", Widget->Percent);
		ReadEnum(Object, "FillDirection", Widget->FillDirection);
		ReadEnum(Object, "Orientation", Widget->Orientation);
		Read(Object, "ScrollbarWidth", Widget->ScrollbarWidth);
		Read(Object, "ScrollbarColor", Widget->ScrollbarColor);
		Read(Object, "MinCellSize", Widget->MinCellSize);

		if (const auto ChildrenIt = Object.find("Children"); ChildrenIt != Object.end() && ChildrenIt->is_array())
		{
			for (const json& ChildObject : *ChildrenIt)
			{
				if (!Widget->CanAddChild())
				{
					E_LOG(LogUI, Warning, "'{}'({})에 붙일 수 있는 자식 수를 넘어 나머지를 버립니다", Widget->Name, ToString(Widget->Type));
					break;
				}
				if (std::unique_ptr<FUIWidget> Child = WidgetFromJson(ChildObject))
				{
					Widget->AddChild(std::move(Child));
				}
			}
		}
		return Widget;
	}
} // namespace

FUIAsset::FUIAsset()
	: Root(FUIWidget::Create(EUIWidgetType::Canvas))
{
	Root->Name = "Root";
}

FUIAsset FUIAsset::MakeDefault()
{
	return FUIAsset{};
}

FUIAsset FUIAsset::Clone() const
{
	FUIAsset Copy;
	Copy.DesignSize = DesignSize;
	Copy.ScaleMode  = ScaleMode;
	Copy.Root       = Root ? Root->Clone() : FUIWidget::Create(EUIWidgetType::Canvas);
	Copy.Root->AssignIds();
	return Copy;
}

std::string FUIAsset::ToJsonString() const
{
	json Document;
	Document["Version"]    = Version;
	Document["DesignSize"] = ToJson(DesignSize);
	Document["ScaleMode"]  = ToString(ScaleMode);
	if (Root)
	{
		Document["Root"] = WidgetToJson(*Root, EUIWidgetType::Count, false);
	}
	return Document.dump(2);
}

bool FUIAsset::FromJsonString(const std::string& JsonText)
{
	const json Document = json::parse(JsonText, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogUI, Error, "UI 에셋 JSON 파싱 실패");
		return false;
	}
	const int32 FileVersion = Document.value("Version", 0);
	if (FileVersion > Version)
	{
		E_LOG(LogUI, Warning, "UI 에셋 버전 {}은(는) 이 엔진({})보다 새롭습니다. 모르는 값은 무시합니다", FileVersion, Version);
	}
	DesignSize = FVector2(1920.0f, 1080.0f);
	ScaleMode  = EUIScaleMode::MatchHeight;
	Read(Document, "DesignSize", DesignSize);
	ReadEnum(Document, "ScaleMode", ScaleMode);
	DesignSize.X = FMath::Max(DesignSize.X, 1.0f);
	DesignSize.Y = FMath::Max(DesignSize.Y, 1.0f);

	Root.reset();
	if (const auto It = Document.find("Root"); It != Document.end())
	{
		Root = WidgetFromJson(*It);
	}
	if (!Root)
	{
		Root       = FUIWidget::Create(EUIWidgetType::Canvas);
		Root->Name = "Root";
	}
	Root->AssignIds();
	return true;
}

bool FUIAsset::LoadFromFile(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogUI, Error, "UI 에셋을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!FromJsonString(Buffer.str()))
	{
		E_LOG(LogUI, Error, "UI 에셋 로드 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

bool FUIAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogUI, Error, "UI 에셋을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return static_cast<bool>(File);
}

std::string FUIAsset::WidgetToJsonString(const FUIWidget& Widget)
{
	return WidgetToJson(Widget, EUIWidgetType::Count, true).dump(2);
}

std::unique_ptr<FUIWidget> FUIAsset::WidgetFromJsonString(const std::string& JsonText)
{
	const json Document = json::parse(JsonText, nullptr, false, true);
	if (Document.is_discarded())
	{
		return nullptr;
	}
	return WidgetFromJson(Document);
}
