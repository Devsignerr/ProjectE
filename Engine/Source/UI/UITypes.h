#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"

#include <string>

E_DECLARE_ENGINE_LOG_CATEGORY(LogUI)

// 게임 UI 공용 타입. 좌표는 "UI 단위"(설계 해상도 기준 픽셀)이며 +X 오른쪽, +Y 아래 (화면 좌상단 원점).

enum class EUIWidgetType : uint8
{
	Canvas = 0,    // 자식을 앵커/오프셋으로 자유 배치
	HorizontalBox, // 자식을 가로로 나열 (자동/채우기 크기)
	VerticalBox,   // 자식을 세로로 나열
	Overlay,       // 자식을 같은 영역에 겹침
	UniformGrid,   // 같은 크기 칸에 행/열로 배치
	ScrollBox,     // 세로/가로로 나열 + 넘치면 스크롤 (영역 밖은 잘림)
	Border,        // 배경(브러시) + 자식 1개
	Image,
	Text,
	Button, // 배경(상태별 브러시) + 자식 1개, 클릭 이벤트
	ProgressBar,
	TextBox, // 한 줄 텍스트 입력 (클릭/Tab으로 포커스, Enter 확정)
	Count
};

// UMG와 같은 의미
enum class EUIVisibility : uint8
{
	Visible = 0,          // 그리고 입력을 받는다 (아래 게임 입력을 막는다)
	Collapsed,            // 그리지 않고 공간도 차지하지 않는다
	Hidden,               // 그리지 않지만 공간은 차지한다
	HitTestInvisible,     // 그리지만 자신과 자식 모두 입력을 받지 않는다
	SelfHitTestInvisible, // 그리지만 자신은 입력을 받지 않는다 (자식은 받음)
};

enum class EUIHAlign : uint8
{
	Fill = 0,
	Left,
	Center,
	Right,
};

enum class EUIVAlign : uint8
{
	Fill = 0,
	Top,
	Center,
	Bottom,
};

// 박스(가로/세로) 슬롯의 주축 크기
enum class EUISizeRule : uint8
{
	Auto = 0, // 원하는 크기
	Fill,     // 남은 공간을 FillWeight 비율로 나눔
};

enum class EUIOrientation : uint8
{
	Vertical = 0,
	Horizontal,
};

enum class EUITextJustify : uint8
{
	Left = 0,
	Center,
	Right,
};

enum class EUIFillDirection : uint8
{
	LeftToRight = 0,
	RightToLeft,
	BottomToTop,
	TopToBottom,
};

// 설계 해상도 → 화면 배율 규칙
enum class EUIScaleMode : uint8
{
	None = 0,    // 1배 (UI 단위 = 화면 픽셀)
	MatchHeight, // 화면 높이 / 설계 높이 (기본)
	MatchWidth,
	Fit,  // 두 비율 중 작은 값 (전체가 보임)
	Fill, // 두 비율 중 큰 값
};

struct FUIMargin
{
	float Left   = 0.0f;
	float Top    = 0.0f;
	float Right  = 0.0f;
	float Bottom = 0.0f;

	constexpr FUIMargin() = default;
	constexpr explicit FUIMargin(float Uniform) : Left(Uniform), Top(Uniform), Right(Uniform), Bottom(Uniform) {}
	constexpr FUIMargin(float Horizontal, float Vertical) : Left(Horizontal), Top(Vertical), Right(Horizontal), Bottom(Vertical) {}
	constexpr FUIMargin(float InLeft, float InTop, float InRight, float InBottom) : Left(InLeft), Top(InTop), Right(InRight), Bottom(InBottom) {}

	constexpr float    GetHorizontal() const { return Left + Right; }
	constexpr float    GetVertical() const { return Top + Bottom; }
	constexpr FVector2 GetTotal() const { return { GetHorizontal(), GetVertical() }; }
	constexpr bool     operator==(const FUIMargin& Other) const = default;
};

struct FUIRect
{
	FVector2 Min;
	FVector2 Max;

	constexpr FUIRect() = default;
	constexpr FUIRect(const FVector2& InMin, const FVector2& InMax) : Min(InMin), Max(InMax) {}
	static constexpr FUIRect FromPositionSize(const FVector2& Position, const FVector2& Size) { return { Position, Position + Size }; }
	// 잘라내기 없음을 뜻하는 아주 큰 영역
	static constexpr FUIRect Infinite() { return { FVector2(-1.0e9f, -1.0e9f), FVector2(1.0e9f, 1.0e9f) }; }

	constexpr float    GetWidth() const { return Max.X - Min.X; }
	constexpr float    GetHeight() const { return Max.Y - Min.Y; }
	constexpr FVector2 GetSize() const { return Max - Min; }
	constexpr FVector2 GetCenter() const { return (Min + Max) * 0.5f; }
	constexpr bool     IsEmpty() const { return Max.X <= Min.X || Max.Y <= Min.Y; }
	constexpr bool     Contains(const FVector2& Point) const { return Point.X >= Min.X && Point.X < Max.X && Point.Y >= Min.Y && Point.Y < Max.Y; }

	// 안쪽으로 여백만큼 줄인 영역 (음수 크기는 0으로)
	constexpr FUIRect Inset(const FUIMargin& Margin) const
	{
		FUIRect Result(FVector2(Min.X + Margin.Left, Min.Y + Margin.Top), FVector2(Max.X - Margin.Right, Max.Y - Margin.Bottom));
		Result.Max.X = FMath::Max(Result.Max.X, Result.Min.X);
		Result.Max.Y = FMath::Max(Result.Max.Y, Result.Min.Y);
		return Result;
	}
	constexpr FUIRect Intersect(const FUIRect& Other) const
	{
		FUIRect Result(FVector2(FMath::Max(Min.X, Other.Min.X), FMath::Max(Min.Y, Other.Min.Y)),
		               FVector2(FMath::Min(Max.X, Other.Max.X), FMath::Min(Max.Y, Other.Max.Y)));
		Result.Max.X = FMath::Max(Result.Max.X, Result.Min.X);
		Result.Max.Y = FMath::Max(Result.Max.Y, Result.Min.Y);
		return Result;
	}
	constexpr bool operator==(const FUIRect& Other) const { return Min == Other.Min && Max == Other.Max; }
};

// 브러시 그리는 방식
enum class EUIBrushDrawAs : uint8
{
	Box = 0,   // 사각형에 늘여 그림 (둥근 모서리/테두리 가능)
	NineSlice, // 가장자리(Margin)는 원래 크기 유지, 가운데만 늘림 (둥근 모서리/테두리 무시)
};

// UI 단위 → 화면 픽셀: Pixel = Ui * (Scale * Stretch) + Offset. Stretch는 위젯 렌더 변환의 축별 배율 (보통 1)
struct FUITransform
{
	float    Scale = 1.0f;
	FVector2 Offset;
	FVector2 Stretch = FVector2(1.0f, 1.0f);

	constexpr FVector2 GetAxisScale() const { return Stretch * Scale; }
	// 반지름/테두리/글꼴 거리처럼 한 값으로 줄이는 것들
	constexpr float    GetUniformScale() const { return Scale * FMath::Min(FMath::Abs(Stretch.X), FMath::Abs(Stretch.Y)); }
	constexpr FVector2 ToPixels(const FVector2& Ui) const { return Ui * GetAxisScale() + Offset; }
	constexpr FVector2 ToUi(const FVector2& Pixels) const
	{
		const FVector2 Axis = GetAxisScale();
		return FVector2((Pixels.X - Offset.X) / Axis.X, (Pixels.Y - Offset.Y) / Axis.Y);
	}
	constexpr FUIRect ToPixels(const FUIRect& Rect) const
	{
		const FVector2 A = ToPixels(Rect.Min);
		const FVector2 B = ToPixels(Rect.Max);
		return { FVector2(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)), FVector2(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)) };
	}
};

// 배경/테두리/이미지 칠하기. 색은 sRGB(에디터 색 선택 그대로) + 직선 알파이며 그릴 때 선형으로 바꾼다.
struct FUIBrush
{
	FVector4    Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f); // 텍스처가 있으면 곱해진다
	std::string Texture;                               // Content 기준 경로. 비면 단색
	float       CornerRadius = 0.0f;                   // UI 단위
	float       BorderWidth  = 0.0f;                   // UI 단위 (안쪽으로)
	FVector4    BorderColor  = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
	// 9-slice: Margin = 텍스처 비율(0~0.5, UMG와 같음), 화면 가장자리 두께 = Margin × TextureSize (UI 단위)
	EUIBrushDrawAs DrawAs      = EUIBrushDrawAs::Box;
	FUIMargin      Margin      = FUIMargin(0.25f);
	FVector2       TextureSize = FVector2(64.0f, 64.0f);

	bool IsVisible() const { return Color.W > 0.0f || (BorderWidth > 0.0f && BorderColor.W > 0.0f); }
	bool operator==(const FUIBrush& Other) const = default;
};

// 문자열 ↔ 열거형 (JSON/에디터 표시). 모르는 이름이면 false
const char* ToString(EUIWidgetType Type);
bool        FromString(std::string_view Text, EUIWidgetType& Out);
const char* ToString(EUIVisibility Value);
bool        FromString(std::string_view Text, EUIVisibility& Out);
const char* ToString(EUIHAlign Value);
bool        FromString(std::string_view Text, EUIHAlign& Out);
const char* ToString(EUIVAlign Value);
bool        FromString(std::string_view Text, EUIVAlign& Out);
const char* ToString(EUISizeRule Value);
bool        FromString(std::string_view Text, EUISizeRule& Out);
const char* ToString(EUIOrientation Value);
bool        FromString(std::string_view Text, EUIOrientation& Out);
const char* ToString(EUITextJustify Value);
bool        FromString(std::string_view Text, EUITextJustify& Out);
const char* ToString(EUIFillDirection Value);
bool        FromString(std::string_view Text, EUIFillDirection& Out);
const char* ToString(EUIBrushDrawAs Value);
bool        FromString(std::string_view Text, EUIBrushDrawAs& Out);
const char* ToString(EUIScaleMode Value);
bool        FromString(std::string_view Text, EUIScaleMode& Out);

// sRGB 색 → 선형 (알파는 그대로)
FVector4 UISrgbToLinear(const FVector4& Srgb);
