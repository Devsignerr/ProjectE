#pragma once

#include "UI/UITypes.h"

struct FUIWidget;
struct FUIWidgetData;
struct FUISlot;

// 텍스트 크기 측정 (FUIFontLibrary가 구현, 테스트는 가짜 측정기)
class IUITextMeasurer
{
public:
	virtual ~IUITextMeasurer() = default;
	// WrapWidth <= 0이면 줄바꿈 없음. 반환: UI 단위 크기
	virtual FVector2 MeasureText(const FUIWidgetData& TextWidget, float WrapWidth) = 0;
};

// 두 단계 레이아웃 (Slate 방식): 원하는 크기를 아래→위로 계산한 뒤 위→아래로 영역을 나눠 준다.
// 결과는 각 위젯의 State(DesiredSize/Geometry/Clip, 스크롤 최대값)에 쓴다. 접힌(Collapsed) 위젯은 공간을 차지하지 않는다.
struct FUILayout
{
	// 루트를 (0, 0) ~ RootSize 영역에 배치한다
	static void Compute(FUIWidget& Root, const FVector2& RootSize, IUITextMeasurer& Measurer);

	// 렌더 변환 누적 → State.VisualGeometry/VisualClip/VisualScale/VisualOffset (Compute가 마지막에 호출)
	static void ApplyRenderTransforms(FUIWidget& Widget, const FVector2& ParentScale, const FVector2& ParentOffset, const FUIRect& ParentClip);

	// 설계 해상도 기준 배율 (화면 픽셀 / UI 단위)
	static float ComputeScale(EUIScaleMode Mode, const FVector2& DesignSize, const FVector2& ViewportSize);

	// 캔버스 자식 영역 (부모 영역 + 슬롯 + 자식 원하는 크기)
	static FUIRect ArrangeCanvasSlot(const FUIRect& Parent, const FUISlot& Slot, const FVector2& DesiredSize);
	// 영역 안에 여백/정렬로 자식 배치
	static FUIRect AlignInRect(const FUIRect& Area, const FUIMargin& Padding, const FVector2& DesiredSize, EUIHAlign HAlign, EUIVAlign VAlign);

	// 에디터 조작: 캔버스 슬롯의 결과 영역을 Delta만큼 옮기거나(늘이기 축은 양쪽 여백을 함께) 새 영역에 맞게 오프셋을 다시 계산한다
	static void MoveCanvasSlot(FUISlot& Slot, const FVector2& Delta);
	static void SetCanvasSlotRect(FUISlot& Slot, const FUIRect& Parent, const FUIRect& Desired);
	// 앵커를 바꾸되 화면상 영역은 유지 (UMG 앵커 프리셋 동작)
	static void SetAnchorsKeepRect(FUISlot& Slot, const FUIRect& Parent, const FUIRect& Current, const FVector2& AnchorMin, const FVector2& AnchorMax);
};
