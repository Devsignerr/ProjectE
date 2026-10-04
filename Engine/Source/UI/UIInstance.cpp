#include "UI/UIInstance.h"

#include "UI/UIFont.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"
#include "UI/UITextEdit.h"

#include <algorithm>
#include <cmath>

FUIInstance::FUIInstance()
{
	Asset.Root->AssignIds();
	NextWidgetId = 2;
}

FUIInstance::FUIInstance(const FUIAsset& InAsset)
{
	SetAsset(InAsset);
}

void FUIInstance::SetAsset(const FUIAsset& InAsset)
{
	Asset        = InAsset.Clone();
	Router       = FUIInputRouter{};
	bPointerOver = false;
	bLayoutDirty = false;
	NextWidgetId = 1;
	Asset.Root->ForEach([this](const FUIWidget& Widget) { NextWidgetId = FMath::Max(NextWidgetId, Widget.State.Id + 1); });
	// 에셋에 들어 있는 글자는 지금 굽는다 (실행 중 바뀌는 글자만 처음 쓸 때 굽힌다)
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	Asset.Root->ForEach([&Fonts](FUIWidget& Widget) {
		if (Widget.Type == EUIWidgetType::Text && !GetDisplayText(Widget).empty())
		{
			if (FUIFont* Font = Fonts.GetFont(Widget.Font))
			{
				Font->Prebake(GetDisplayText(Widget));
			}
		}
	});
}

void FUIInstance::Layout(const FUIRect& InViewport, FUIFontLibrary& Fonts)
{
	Viewport         = InViewport;
	Transform.Scale  = FMath::Max(FUILayout::ComputeScale(Asset.ScaleMode, Asset.DesignSize, InViewport.GetSize()), 0.01f);
	Transform.Offset = InViewport.Min;
	FUILayout::Compute(*Asset.Root, InViewport.GetSize() / Transform.Scale, Fonts);
	bLayoutDirty = false;
}

FUIWidget* FUIInstance::CloneWidget(std::string_view TemplateName, const std::string& NewName, std::string& OutError)
{
	const FUIWidget* Template = Asset.Root->FindByName(TemplateName);
	if (Template == nullptr)
	{
		OutError = "템플릿 위젯이 없습니다: " + std::string(TemplateName);
		return nullptr;
	}
	if (Template->Parent == nullptr)
	{
		OutError = "루트 위젯은 복제할 수 없습니다";
		return nullptr;
	}
	if (!Template->Parent->CanAddChild())
	{
		OutError = "템플릿의 부모가 자식을 더 받을 수 없습니다: " + Template->Parent->Name;
		return nullptr;
	}
	if (NewName.empty())
	{
		OutError = "새 위젯 이름이 비었습니다";
		return nullptr;
	}

	std::unique_ptr<FUIWidget> Copy = Template->Clone();
	Copy->Name                      = NewName;
	bool bNameTaken                 = false;
	Copy->ForEach([&](FUIWidget& Widget) {
		if (&Widget != Copy.get() && !Widget.Name.empty())
		{
			Widget.Name = NewName + "." + Widget.Name;
		}
		if (!Widget.Name.empty() && Asset.Root->FindByName(Widget.Name) != nullptr)
		{
			bNameTaken = true;
			OutError   = "같은 이름의 위젯이 이미 있습니다: " + Widget.Name;
		}
	});
	if (bNameTaken)
	{
		return nullptr;
	}
	Copy->ForEach([this](FUIWidget& Widget) { Widget.State.Id = NextWidgetId++; });
	FUIWidget* Added = Template->Parent->AddChild(std::move(Copy)); // 자손 Parent는 Clone이 맞춘다
	bLayoutDirty = true;
	return Added;
}

bool FUIInstance::RemoveWidget(std::string_view Name)
{
	FUIWidget* Widget = Asset.Root->FindByName(Name);
	if (Widget == nullptr || Widget->Parent == nullptr)
	{
		return false;
	}
	Widget->Parent->RemoveChild(Widget); // 반환된 소유권을 버려 해제
	bLayoutDirty = true;
	return true;
}

bool FUIInstance::Update(const FUIRect& InViewport, const FUIPointerInput* PointerPixels, const FUIKeyInput* Keys, FUIFontLibrary& Fonts,
                         std::vector<FUIEvent>& OutEvents, float DeltaSeconds, float GameTimeScale)
{
	TickAnimations(DeltaSeconds, OutEvents, GameTimeScale); // 값을 먼저 바꾸고 레이아웃 (렌더 변환은 레이아웃 뒤 누적)
	Layout(InViewport, Fonts);

	FUIPointerInput Pointer;
	if (PointerPixels != nullptr)
	{
		Pointer          = *PointerPixels;
		Pointer.bInside  = Pointer.bInside && InViewport.Contains(Pointer.Position);
		Pointer.Position = Transform.ToUi(Pointer.Position);
	}
	const FUIKeyInput NoKeys;
	const float       ScrollBefore = Pointer.Wheel;
	bPointerOver                   = Router.Process(*Asset.Root, Pointer, Keys != nullptr ? *Keys : NoKeys, OutEvents);
	if (ScrollBefore != 0.0f)
	{
		// 스크롤 위치가 바뀌었으면 같은 프레임에 반영
		FUILayout::Compute(*Asset.Root, InViewport.GetSize() / Transform.Scale, Fonts);
	}

	// 포커스된 텍스트 상자: 캐럿 깜빡임 + 캐럿이 보이도록 가로 스크롤
	if (FUIWidget* Focused = Router.GetFocusedId() != 0 ? Asset.Root->FindById(Router.GetFocusedId()) : nullptr;
	    Focused != nullptr && Focused->Type == EUIWidgetType::TextBox)
	{
		Focused->State.CaretTime += DeltaSeconds;
		if (FUIFont* Font = Fonts.GetFont(Focused->Font))
		{
			int32             DisplayCaret = 0;
			int32             CompositionBegin = 0;
			int32             CompositionEnd   = 0;
			const std::string Display          = BuildTextBoxDisplay(*Focused, DisplayCaret, CompositionBegin, CompositionEnd);
			std::vector<float> Positions;
			Font->GetCaretPositions(Display, Focused->FontSize, Positions);
			const int32 Caret        = FMath::Clamp(DisplayCaret, 0, static_cast<int32>(Positions.size()) - 1);
			const float CaretX       = Positions[static_cast<size_t>(Caret)];
			const float ContentWidth = FMath::Max(Focused->State.Geometry.Inset(Focused->ContentPadding).GetWidth() - 2.0f, 1.0f);
			float&      Scroll       = Focused->State.TextScroll;
			Scroll                   = FMath::Clamp(Scroll, CaretX - ContentWidth, CaretX);
			Scroll                   = FMath::Clamp(Scroll, 0.0f, FMath::Max(Positions.back() - ContentWidth, 0.0f));
		}
	}
	return bPointerOver;
}

bool FUIInstance::GetTextCaretPixels(FUIRect& Out, FUIFontLibrary& Fonts)
{
	FUIWidget* Box = Router.GetFocusedId() != 0 ? Asset.Root->FindById(Router.GetFocusedId()) : nullptr;
	if (Box == nullptr || Box->Type != EUIWidgetType::TextBox)
	{
		return false;
	}
	FUIFont* Font = Fonts.GetFont(Box->Font);
	if (Font == nullptr)
	{
		return false;
	}
	int32             Caret = 0;
	int32             CompositionBegin = 0;
	int32             CompositionEnd   = 0;
	const std::string Display          = BuildTextBoxDisplay(*Box, Caret, CompositionBegin, CompositionEnd);
	std::vector<float> Positions;
	Font->GetCaretPositions(Display, Box->FontSize, Positions);
	// IME 후보 창은 조합 시작 자리에 (조합 중이 아니면 캐럿)
	const int32   Index      = FMath::Clamp(CompositionBegin < CompositionEnd ? CompositionBegin : Caret, 0, static_cast<int32>(Positions.size()) - 1);
	const FUIRect Content    = Box->State.Geometry.Inset(Box->ContentPadding);
	const float   LineHeight = Font->GetLineHeight(Box->FontSize);
	const float   Top        = Content.Min.Y + (Content.GetHeight() - LineHeight) * 0.5f;
	const float   X          = Content.Min.X - Box->State.TextScroll + Positions[static_cast<size_t>(Index)];
	// 레이아웃 좌표 → 렌더 변환 → 화면 픽셀
	const FVector2 Min = Transform.ToPixels(FVector2(X, Top) * Box->State.VisualScale + Box->State.VisualOffset);
	const FVector2 Max = Transform.ToPixels(FVector2(X + 1.0f, Top + LineHeight) * Box->State.VisualScale + Box->State.VisualOffset);
	Out                = FUIRect(Min, Max);
	return true;
}

void FUIInstance::Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const
{
	FUIPainter::Paint(*Asset.Root, Transform, Viewport, Fonts, Out);
}

bool FUIInstance::PlayAnimation(std::string_view Name, int32 Loops, float Speed, std::optional<bool> UseGameTime)
{
	for (size_t Index = 0; Index < Asset.Animations.size(); ++Index)
	{
		if (Asset.Animations[Index].Name != Name)
		{
			continue;
		}
		StopAnimation(Name);
		FUIAnimationPlayback Playback;
		Playback.AnimationIndex = static_cast<int32>(Index);
		Playback.Speed          = Speed;
		Playback.LoopsRemaining = FMath::Max(Loops, 0);
		Playback.bUseGameTime   = UseGameTime.value_or(Asset.Animations[Index].bUseGameTime);
		Playback.Time           = Speed < 0.0f ? Asset.Animations[Index].Length : 0.0f;
		Asset.Animations[Index].ApplyAt(*Asset.Root, Playback.Time);
		Playing.push_back(Playback);
		return true;
	}
	E_LOG(LogUI, Warning, "UI 애니메이션이 없습니다: {}", Name);
	return false;
}

void FUIInstance::StopAnimation(std::string_view Name)
{
	std::erase_if(Playing, [this, Name](const FUIAnimationPlayback& Playback) { return Asset.Animations[static_cast<size_t>(Playback.AnimationIndex)].Name == Name; });
}

void FUIInstance::StopAllAnimations()
{
	Playing.clear();
}

bool FUIInstance::IsAnimationPlaying(std::string_view Name) const
{
	return std::any_of(Playing.begin(), Playing.end(), [this, Name](const FUIAnimationPlayback& Playback) {
		return Asset.Animations[static_cast<size_t>(Playback.AnimationIndex)].Name == Name;
	});
}

void FUIInstance::TickAnimations(float DeltaSeconds, std::vector<FUIEvent>& OutEvents, float GameTimeScale)
{
	const float GameScale = std::isfinite(GameTimeScale) ? FMath::Max(GameTimeScale, 0.0f) : 1.0f;
	for (size_t Index = 0; Index < Playing.size();)
	{
		FUIAnimationPlayback& Playback  = Playing[Index];
		const FUIAnimation&   Animation = Asset.Animations[static_cast<size_t>(Playback.AnimationIndex)];
		const float           Length    = FMath::Max(Animation.Length, 0.001f);
		Playback.Time += DeltaSeconds * (Playback.bUseGameTime ? GameScale : 1.0f) * Playback.Speed;
		bool       bFinished = false;
		const bool bPastEnd  = Playback.Speed >= 0.0f ? Playback.Time >= Length : Playback.Time <= 0.0f;
		if (bPastEnd)
		{
			if (Playback.LoopsRemaining == 1)
			{
				Playback.Time = Playback.Speed >= 0.0f ? Length : 0.0f; // 끝 값에 멈춘다
				bFinished     = true;
			}
			else
			{
				if (Playback.LoopsRemaining > 1)
				{
					--Playback.LoopsRemaining;
				}
				Playback.Time = Playback.Speed >= 0.0f ? std::fmod(Playback.Time, Length) : Length + std::fmod(Playback.Time, Length);
			}
		}
		Animation.ApplyAt(*Asset.Root, Playback.Time);
		if (bFinished)
		{
			OutEvents.push_back({ EUIEventType::AnimationFinished, 0, Animation.Name });
			Playing.erase(Playing.begin() + static_cast<std::ptrdiff_t>(Index));
			continue;
		}
		++Index;
	}
}
