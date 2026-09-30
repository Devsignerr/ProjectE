#pragma once

#include "UI/UIDrawList.h"
#include "UI/UIFont.h"

struct FUIWidget;

// 레이아웃이 끝난 위젯 트리 → 그리기 목록 (FUILayout::Compute 이후 호출).
// 순서: 위젯 자신 → 자식 (캔버스 자식은 ZOrder 오름차순). 숨김/접힘은 자식까지 건너뛴다. 불투명도는 자식에게 곱해진다.
struct FUIPainter
{
	// ViewportClip: 화면 픽셀 잘림 영역 (보통 출력 전체)
	static void Paint(const FUIWidget& Root, const FUITransform& Transform, const FUIRect& ViewportClip, FUIFontLibrary& Fonts, FUIDrawList& Out);

	// 사각형 하나 (에디터 오버레이/테스트용). Rect는 UI 단위
	static void PaintBrush(const FUIBrush& Brush, const FUIRect& Rect, float Opacity, const FUITransform& Transform, const FUIRect& ClipPixels,
	                       FUIDrawList& Out);
	// 텍스트 위젯 하나 (Geometry 안에 정렬)
	static void PaintText(const FUIWidgetData& TextWidget, const FUIRect& Geometry, float Opacity, const FUITransform& Transform,
	                      const FUIRect& ClipPixels, FUIFontLibrary& Fonts, FUIDrawList& Out);
};
