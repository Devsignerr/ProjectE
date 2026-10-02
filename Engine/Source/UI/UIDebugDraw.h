#pragma once

#include "UI/UIDrawList.h"

#include <string>
#include <string_view>
#include <vector>

class FUIFont;

// 위젯 트리 없이 화면 픽셀로 바로 그리는 디버그 오버레이 도우미 (런타임 콘솔, 화면 통계).
// 색은 sRGB + 직선 알파 (그릴 때 선형으로). 글꼴은 FUIFontLibrary 기본 글꼴
struct FUIDebugDraw
{
	static FUIFont* GetFont();

	static void AddRect(FUIDrawList& Out, const FUIRect& Rect, const FVector4& SrgbColor, const FUIRect& Clip);
	// Position = 글자 블록 왼쪽 위. 반환: 그린 너비 (픽셀)
	static float AddText(FUIDrawList& Out, std::string_view Utf8, const FVector2& Position, float FontSize, const FVector4& SrgbColor, const FUIRect& Clip);
	static float MeasureText(std::string_view Utf8, float FontSize);
	static float GetLineHeight(float FontSize);

	// 줄 목록을 반투명 상자 안에 그린다. 줄 안의 '\t'는 열 구분 (ColumnWidths 순서대로 열 시작 X 간격, 모자라면 마지막 값 반복).
	// Anchor: 상자의 기준 모서리 (bRightAlign이면 오른쪽 위). 반환: 상자 영역
	static FUIRect AddTextPanel(FUIDrawList& Out, const std::vector<std::string>& Lines, const FVector2& Anchor, bool bRightAlign, float FontSize,
	                            const std::vector<float>& ColumnWidths, const FUIRect& Clip);
};
