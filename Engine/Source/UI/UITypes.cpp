#include "UI/UITypes.h"

#include <cmath>
#include <iterator>

namespace
{
	template <typename TEnum, size_t N>
	const char* EnumToString(TEnum Value, const char* const (&Names)[N])
	{
		const size_t Index = static_cast<size_t>(Value);
		return Index < N ? Names[Index] : "";
	}

	template <typename TEnum, size_t N>
	bool EnumFromString(std::string_view Text, TEnum& Out, const char* const (&Names)[N])
	{
		for (size_t Index = 0; Index < N; ++Index)
		{
			if (Text == Names[Index])
			{
				Out = static_cast<TEnum>(Index);
				return true;
			}
		}
		return false;
	}

	// 열거형 순서와 같아야 한다
	constexpr const char* WidgetTypeNames[]   = { "Canvas", "HorizontalBox", "VerticalBox", "Overlay", "UniformGrid", "ScrollBox",
		                                          "Border", "Image",         "Text",        "Button",  "ProgressBar", "TextBox" };
	constexpr const char* VisibilityNames[]   = { "Visible", "Collapsed", "Hidden", "HitTestInvisible", "SelfHitTestInvisible" };
	constexpr const char* HAlignNames[]       = { "Fill", "Left", "Center", "Right" };
	constexpr const char* VAlignNames[]       = { "Fill", "Top", "Center", "Bottom" };
	constexpr const char* SizeRuleNames[]     = { "Auto", "Fill" };
	constexpr const char* OrientationNames[]  = { "Vertical", "Horizontal" };
	constexpr const char* JustifyNames[]      = { "Left", "Center", "Right" };
	constexpr const char* FillDirectionNames[] = { "LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom" };
	constexpr const char* ScaleModeNames[]    = { "None", "MatchHeight", "MatchWidth", "Fit", "Fill" };
	constexpr const char* DrawAsNames[]       = { "Box", "NineSlice" };
	static_assert(std::size(WidgetTypeNames) == static_cast<size_t>(EUIWidgetType::Count));

	float SrgbToLinearChannel(float Value)
	{
		return Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
	}
} // namespace

const char* ToString(EUIWidgetType Type) { return EnumToString(Type, WidgetTypeNames); }
bool        FromString(std::string_view Text, EUIWidgetType& Out) { return EnumFromString(Text, Out, WidgetTypeNames); }
const char* ToString(EUIVisibility Value) { return EnumToString(Value, VisibilityNames); }
bool        FromString(std::string_view Text, EUIVisibility& Out) { return EnumFromString(Text, Out, VisibilityNames); }
const char* ToString(EUIHAlign Value) { return EnumToString(Value, HAlignNames); }
bool        FromString(std::string_view Text, EUIHAlign& Out) { return EnumFromString(Text, Out, HAlignNames); }
const char* ToString(EUIVAlign Value) { return EnumToString(Value, VAlignNames); }
bool        FromString(std::string_view Text, EUIVAlign& Out) { return EnumFromString(Text, Out, VAlignNames); }
const char* ToString(EUISizeRule Value) { return EnumToString(Value, SizeRuleNames); }
bool        FromString(std::string_view Text, EUISizeRule& Out) { return EnumFromString(Text, Out, SizeRuleNames); }
const char* ToString(EUIOrientation Value) { return EnumToString(Value, OrientationNames); }
bool        FromString(std::string_view Text, EUIOrientation& Out) { return EnumFromString(Text, Out, OrientationNames); }
const char* ToString(EUITextJustify Value) { return EnumToString(Value, JustifyNames); }
bool        FromString(std::string_view Text, EUITextJustify& Out) { return EnumFromString(Text, Out, JustifyNames); }
const char* ToString(EUIFillDirection Value) { return EnumToString(Value, FillDirectionNames); }
bool        FromString(std::string_view Text, EUIFillDirection& Out) { return EnumFromString(Text, Out, FillDirectionNames); }
const char* ToString(EUIBrushDrawAs Value) { return EnumToString(Value, DrawAsNames); }
bool        FromString(std::string_view Text, EUIBrushDrawAs& Out) { return EnumFromString(Text, Out, DrawAsNames); }
const char* ToString(EUIScaleMode Value) { return EnumToString(Value, ScaleModeNames); }
bool        FromString(std::string_view Text, EUIScaleMode& Out) { return EnumFromString(Text, Out, ScaleModeNames); }

FVector4 UISrgbToLinear(const FVector4& Srgb)
{
	return { SrgbToLinearChannel(Srgb.X), SrgbToLinearChannel(Srgb.Y), SrgbToLinearChannel(Srgb.Z), Srgb.W };
}
