#include "UI/UIAnimation.h"

#include "UI/Widget.h"

#include <algorithm>
#include <iterator>

namespace
{
	constexpr const char* PropertyNames[] = { "Opacity", "TranslationX", "TranslationY", "ScaleX", "ScaleY", "ColorR", "ColorG", "ColorB", "ColorA", "Percent" };
	constexpr const char* InterpNames[]   = { "Linear", "Constant", "EaseIn", "EaseOut", "EaseInOut" };
	static_assert(std::size(PropertyNames) == static_cast<size_t>(EUIAnimProperty::Count));
	static_assert(std::size(InterpNames) == static_cast<size_t>(EUIAnimInterp::Count));

	template <typename TEnum, size_t N>
	bool EnumFromNames(std::string_view Text, TEnum& Out, const char* const (&Names)[N])
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
} // namespace

const char* ToString(EUIAnimProperty Value)
{
	const size_t Index = static_cast<size_t>(Value);
	return Index < std::size(PropertyNames) ? PropertyNames[Index] : "";
}
bool FromString(std::string_view Text, EUIAnimProperty& Out) { return EnumFromNames(Text, Out, PropertyNames); }
const char* ToString(EUIAnimInterp Value)
{
	const size_t Index = static_cast<size_t>(Value);
	return Index < std::size(InterpNames) ? InterpNames[Index] : "";
}
bool FromString(std::string_view Text, EUIAnimInterp& Out) { return EnumFromNames(Text, Out, InterpNames); }

// ---------------------------------------------------------------- 트랙

bool FUIAnimTrack::Evaluate(float Time, float& OutValue) const
{
	if (Keys.empty())
	{
		return false;
	}
	if (Time <= Keys.front().Time)
	{
		OutValue = Keys.front().Value;
		return true;
	}
	if (Time >= Keys.back().Time)
	{
		OutValue = Keys.back().Value;
		return true;
	}
	// 첫 번째로 Time보다 늦은 키 앞에서 보간
	const auto Next = std::upper_bound(Keys.begin(), Keys.end(), Time, [](float Value, const FUIAnimKey& Key) { return Value < Key.Time; });
	const FUIAnimKey& A    = *(Next - 1);
	const FUIAnimKey& B    = *Next;
	const float       Span = B.Time - A.Time;
	const float       Alpha = Span > 0.0f ? (Time - A.Time) / Span : 1.0f;
	OutValue                = FMath::Lerp(A.Value, B.Value, FUIAnimMath::Ease(A.Interp, Alpha));
	return true;
}

int32 FUIAnimTrack::SetKey(float Time, float Value, EUIAnimInterp Interp)
{
	for (size_t Index = 0; Index < Keys.size(); ++Index)
	{
		if (FMath::Abs(Keys[Index].Time - Time) <= 0.001f)
		{
			Keys[Index].Value = Value;
			return static_cast<int32>(Index);
		}
	}
	Keys.push_back({ Time, Value, Interp });
	SortKeys();
	for (size_t Index = 0; Index < Keys.size(); ++Index)
	{
		if (Keys[Index].Time == Time)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

void FUIAnimTrack::SortKeys()
{
	std::stable_sort(Keys.begin(), Keys.end(), [](const FUIAnimKey& A, const FUIAnimKey& B) { return A.Time < B.Time; });
}

// ---------------------------------------------------------------- 애니메이션

FUIAnimTrack* FUIAnimation::FindTrack(std::string_view Widget, EUIAnimProperty Property)
{
	return const_cast<FUIAnimTrack*>(static_cast<const FUIAnimation*>(this)->FindTrack(Widget, Property));
}

const FUIAnimTrack* FUIAnimation::FindTrack(std::string_view Widget, EUIAnimProperty Property) const
{
	for (const FUIAnimTrack& Track : Tracks)
	{
		if (Track.Widget == Widget && Track.Property == Property)
		{
			return &Track;
		}
	}
	return nullptr;
}

FUIAnimTrack& FUIAnimation::GetOrAddTrack(std::string_view Widget, EUIAnimProperty Property)
{
	if (FUIAnimTrack* Track = FindTrack(Widget, Property))
	{
		return *Track;
	}
	FUIAnimTrack Track;
	Track.Widget   = Widget;
	Track.Property = Property;
	Tracks.push_back(std::move(Track));
	return Tracks.back();
}

float FUIAnimation::GetLastKeyTime() const
{
	float Last = 0.0f;
	for (const FUIAnimTrack& Track : Tracks)
	{
		if (!Track.Keys.empty())
		{
			Last = FMath::Max(Last, Track.Keys.back().Time);
		}
	}
	return Last;
}

void FUIAnimation::ApplyAt(FUIWidget& Root, float Time) const
{
	for (const FUIAnimTrack& Track : Tracks)
	{
		float Value = 0.0f;
		if (!Track.Evaluate(Time, Value))
		{
			continue;
		}
		if (FUIWidget* Widget = Root.FindByName(Track.Widget))
		{
			FUIAnimMath::SetProperty(*Widget, Track.Property, Value);
		}
	}
}

// ---------------------------------------------------------------- 수학 / 속성

float FUIAnimMath::Ease(EUIAnimInterp Interp, float Alpha)
{
	const float T = FMath::Clamp(Alpha, 0.0f, 1.0f);
	switch (Interp)
	{
	case EUIAnimInterp::Constant:  return T >= 1.0f ? 1.0f : 0.0f;
	case EUIAnimInterp::EaseIn:    return T * T * T;
	case EUIAnimInterp::EaseOut:   return 1.0f - (1.0f - T) * (1.0f - T) * (1.0f - T);
	case EUIAnimInterp::EaseInOut: return T < 0.5f ? 4.0f * T * T * T : 1.0f - 4.0f * (1.0f - T) * (1.0f - T) * (1.0f - T);
	default:                       return T;
	}
}

FVector4& FUIAnimMath::GetMainColor(FUIWidget& Widget)
{
	return const_cast<FVector4&>(GetMainColor(static_cast<const FUIWidget&>(Widget)));
}

const FVector4& FUIAnimMath::GetMainColor(const FUIWidget& Widget)
{
	switch (Widget.Type)
	{
	case EUIWidgetType::Text:
	case EUIWidgetType::TextBox:     return Widget.TextColor;
	case EUIWidgetType::ProgressBar: return Widget.FillBrush.Color;
	default:                         return Widget.Brush.Color;
	}
}

float FUIAnimMath::GetProperty(const FUIWidget& Widget, EUIAnimProperty Property)
{
	switch (Property)
	{
	case EUIAnimProperty::Opacity:      return Widget.RenderOpacity;
	case EUIAnimProperty::TranslationX: return Widget.RenderTranslation.X;
	case EUIAnimProperty::TranslationY: return Widget.RenderTranslation.Y;
	case EUIAnimProperty::ScaleX:       return Widget.RenderScale.X;
	case EUIAnimProperty::ScaleY:       return Widget.RenderScale.Y;
	case EUIAnimProperty::ColorR:       return GetMainColor(Widget).X;
	case EUIAnimProperty::ColorG:       return GetMainColor(Widget).Y;
	case EUIAnimProperty::ColorB:       return GetMainColor(Widget).Z;
	case EUIAnimProperty::ColorA:       return GetMainColor(Widget).W;
	case EUIAnimProperty::Percent:      return Widget.Percent;
	default:                            return 0.0f;
	}
}

void FUIAnimMath::SetProperty(FUIWidget& Widget, EUIAnimProperty Property, float Value)
{
	switch (Property)
	{
	case EUIAnimProperty::Opacity:      Widget.RenderOpacity = FMath::Clamp(Value, 0.0f, 1.0f); break;
	case EUIAnimProperty::TranslationX: Widget.RenderTranslation.X = Value; break;
	case EUIAnimProperty::TranslationY: Widget.RenderTranslation.Y = Value; break;
	case EUIAnimProperty::ScaleX:       Widget.RenderScale.X = Value; break;
	case EUIAnimProperty::ScaleY:       Widget.RenderScale.Y = Value; break;
	case EUIAnimProperty::ColorR:       GetMainColor(Widget).X = Value; break;
	case EUIAnimProperty::ColorG:       GetMainColor(Widget).Y = Value; break;
	case EUIAnimProperty::ColorB:       GetMainColor(Widget).Z = Value; break;
	case EUIAnimProperty::ColorA:       GetMainColor(Widget).W = FMath::Clamp(Value, 0.0f, 1.0f); break;
	case EUIAnimProperty::Percent:      Widget.Percent = FMath::Clamp(Value, 0.0f, 1.0f); break;
	default:                            break;
	}
}
