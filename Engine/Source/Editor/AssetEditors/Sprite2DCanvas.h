#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <imgui.h>

// 2D 에셋 편집기 공용 캔버스: 이미지 px 좌표(왼쪽 위 원점) ↔ 화면 좌표, 확대/이동, 점 필터 이미지 + 체커 배경 + 픽셀 격자.
//   - 휠 = 마우스 위치 기준 확대(배율 단계 2배수/정수 배율 우선), 가운데 또는 오른쪽 끌기(또는 Space + 왼쪽) = 이동, F = 화면 맞춤
//   - 왼쪽 버튼은 편집기 몫: Begin 뒤 IsLeftClicked/IsLeftDragging/GetMouseImage로 쓴다
//   사용: Begin(Id, Size) → DrawImage/그리기 → (편집기 상호작용) → End
class FSprite2DCanvas
{
public:
	// 화면 영역 확보 + 확대/이동 입력. ContentWidth/Height = 이미지 px 크기 (처음 한 번 맞춤)
	void Begin(const char* Id, const ImVec2& Size, int32 ContentWidth, int32 ContentHeight);
	void End();

	// 이미지 전체를 점(bNearest) 또는 선형 필터로. Uv = 텍스처 UV 사각형 (기본 전체)
	void DrawImage(ImTextureID Texture, int32 Width, int32 Height, bool bNearest, const ImVec2& Uv0 = ImVec2(0, 0), const ImVec2& Uv1 = ImVec2(1, 1)) const;
	// 이미지 px 사각형 [Min, Max]에 텍스처 일부 (플립북 프레임 등)
	void DrawImageRect(ImTextureID Texture, const FVector2& Min, const FVector2& Max, bool bNearest, const ImVec2& Uv0, const ImVec2& Uv1) const;
	void DrawChecker(const FVector2& Min, const FVector2& Max) const;
	// 확대가 충분할 때(픽셀당 화면 6px 이상) 픽셀 경계 선
	void DrawPixelGrid(int32 Width, int32 Height) const;

	ImVec2   ImageToScreen(const FVector2& Point) const;
	FVector2 ScreenToImage(const ImVec2& Point) const;
	FVector2 GetMouseImage() const { return ScreenToImage(ImGui::GetIO().MousePos); }
	float    GetZoom() const { return Zoom; } // 화면 px / 이미지 px
	// 화면 Pixels 픽셀 → 이미지 px (핸들 허용 거리)
	float    ScreenToImageDistance(float Pixels) const { return Pixels / Zoom; }

	void Frame(int32 ContentWidth, int32 ContentHeight);
	void RequestFrame() { bFramePending = true; }

	bool IsHovered() const { return bHovered; }
	bool IsLeftClicked() const { return bLeftClicked; }      // 이번 프레임 왼쪽 누름 (이동 중 아님)
	bool IsLeftDown() const { return bLeftActive; }          // 캔버스에서 시작한 왼쪽 누름이 계속 중
	bool IsLeftReleased() const { return bLeftReleased; }    // 이번 프레임 왼쪽 놓음
	bool IsLeftDoubleClicked() const { return bLeftDoubleClicked; }
	bool IsRightClickedWithoutDrag() const { return bRightClick; } // 오른쪽 누름 → 끌지 않고 놓음 (문맥 메뉴)

	ImDrawList* GetDrawList() const { return DrawList; }
	ImVec2      GetMin() const { return CanvasMin; }
	ImVec2      GetMax() const { return CanvasMax; }

private:
	ImDrawList* DrawList  = nullptr;
	ImVec2      CanvasMin = ImVec2(0, 0);
	ImVec2      CanvasMax = ImVec2(0, 0);
	ImVec2      Offset    = ImVec2(0, 0); // 이미지 원점의 캔버스 안 위치 (화면 px)
	float       Zoom      = 4.0f;
	bool        bFramePending      = true;
	bool        bHovered           = false;
	bool        bLeftClicked       = false;
	bool        bLeftActive        = false;
	bool        bLeftReleased      = false;
	bool        bLeftDoubleClicked = false;
	bool        bRightClick        = false;
	bool        bPanning           = false;
	bool        bRightPressed      = false;
	float       RightDragDistance  = 0.0f;
};
