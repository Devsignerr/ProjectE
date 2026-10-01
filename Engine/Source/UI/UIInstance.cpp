#include "UI/UIInstance.h"

#include "UI/UIFont.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"

#include <algorithm>
#include <cmath>

FUIInstance::FUIInstance()
{
	Asset.Root->AssignIds();
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
}

bool FUIInstance::Update(const FUIRect& InViewport, const FUIPointerInput* PointerPixels, const FUIKeyInput* Keys, FUIFontLibrary& Fonts,
                         std::vector<FUIEvent>& OutEvents, float DeltaSeconds)
{
	TickAnimations(DeltaSeconds, OutEvents); // 값을 먼저 바꾸고 레이아웃 (렌더 변환은 레이아웃 뒤 누적)
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
			std::vector<float> Positions;
			Font->GetCaretPositions(Focused->Text, Focused->FontSize, Positions);
			const int32 Caret        = FMath::Clamp(Focused->State.CaretIndex, 0, static_cast<int32>(Positions.size()) - 1);
			const float CaretX       = Positions[static_cast<size_t>(Caret)];
			const float ContentWidth = FMath::Max(Focused->State.Geometry.Inset(Focused->ContentPadding).GetWidth() - 2.0f, 1.0f);
			float&      Scroll       = Focused->State.TextScroll;
			Scroll                   = FMath::Clamp(Scroll, CaretX - ContentWidth, CaretX);
			Scroll                   = FMath::Clamp(Scroll, 0.0f, FMath::Max(Positions.back() - ContentWidth, 0.0f));
		}
	}
	return bPointerOver;
}

void FUIInstance::Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const
{
	FUIPainter::Paint(*Asset.Root, Transform, Viewport, Fonts, Out);
}

bool FUIInstance::PlayAnimation(std::string_view Name, int32 Loops, float Speed)
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

void FUIInstance::TickAnimations(float DeltaSeconds, std::vector<FUIEvent>& OutEvents)
{
	for (size_t Index = 0; Index < Playing.size();)
	{
		FUIAnimationPlayback& Playback  = Playing[Index];
		const FUIAnimation&   Animation = Asset.Animations[static_cast<size_t>(Playback.AnimationIndex)];
		const float           Length    = FMath::Max(Animation.Length, 0.001f);
		Playback.Time += DeltaSeconds * Playback.Speed;
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
