#include "Editor/AssetEditors/Sprite2DCanvas.h"

#include "Editor/ImGuiLayer.h"

#include <cmath>

namespace
{
	// 확대 단계 (픽셀 아트는 정수 배율이 깔끔하다)
	constexpr float ZoomLevels[] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f };
	constexpr float CheckerCell  = 12.0f; // 화면 px
	constexpr float MinGridZoom  = 6.0f;

	float StepZoom(float Current, bool bIn)
	{
		if (bIn)
		{
			for (const float Level : ZoomLevels)
			{
				if (Level > Current * 1.001f)
				{
					return Level;
				}
			}
			return ZoomLevels[std::size(ZoomLevels) - 1];
		}
		for (size_t Index = std::size(ZoomLevels); Index-- > 0;)
		{
			if (ZoomLevels[Index] < Current * 0.999f)
			{
				return ZoomLevels[Index];
			}
		}
		return ZoomLevels[0];
	}
} // namespace

void FSprite2DCanvas::Begin(const char* Id, const ImVec2& Size, int32 ContentWidth, int32 ContentHeight)
{
	CanvasMin = ImGui::GetCursorScreenPos();
	const ImVec2 ClampedSize(FMath::Max(Size.x, 32.0f), FMath::Max(Size.y, 32.0f));
	CanvasMax = ImVec2(CanvasMin.x + ClampedSize.x, CanvasMin.y + ClampedSize.y);
	ImGui::InvisibleButton(Id, ClampedSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	DrawList = ImGui::GetWindowDrawList();

	const ImGuiIO& IO     = ImGui::GetIO();
	bHovered              = ImGui::IsItemHovered();
	const bool bActive    = ImGui::IsItemActive();
	const bool bSpaceHeld = ImGui::IsKeyDown(ImGuiKey_Space) && !IO.WantTextInput;

	if (bFramePending && ContentWidth > 0 && ContentHeight > 0)
	{
		Frame(ContentWidth, ContentHeight);
	}

	// 이동: 가운데/오른쪽 끌기, Space + 왼쪽
	bRightClick = false;
	if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		RightDragDistance = 0.0f;
		bRightPressed     = true;
	}
	const bool bPanButton = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
	                        (bSpaceHeld && ImGui::IsMouseDown(ImGuiMouseButton_Left));
	bPanning = bActive && bPanButton;
	if (bPanning)
	{
		Offset.x += IO.MouseDelta.x;
		Offset.y += IO.MouseDelta.y;
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			RightDragDistance += FMath::Abs(IO.MouseDelta.x) + FMath::Abs(IO.MouseDelta.y);
		}
	}
	if (bRightPressed && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
	{
		bRightClick   = RightDragDistance < 4.0f;
		bRightPressed = false;
	}

	// 확대 (마우스 아래 이미지 점 고정)
	if (bHovered && IO.MouseWheel != 0.0f)
	{
		const FVector2 Anchor  = GetMouseImage();
		const float    NewZoom = StepZoom(Zoom, IO.MouseWheel > 0.0f);
		Zoom                   = NewZoom;
		Offset.x               = IO.MousePos.x - CanvasMin.x - Anchor.X * Zoom;
		Offset.y               = IO.MousePos.y - CanvasMin.y - Anchor.Y * Zoom;
	}
	if (bHovered && ImGui::IsKeyPressed(ImGuiKey_F, false) && !IO.KeyCtrl && !IO.WantTextInput && ContentWidth > 0)
	{
		Frame(ContentWidth, ContentHeight);
	}

	// 왼쪽 버튼 (편집기 몫)
	bLeftClicked       = bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bSpaceHeld;
	bLeftDoubleClicked = bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !bSpaceHeld;
	if (bLeftClicked)
	{
		bLeftActive = true;
	}
	bLeftReleased = false;
	if (bLeftActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		bLeftActive   = false;
		bLeftReleased = true;
	}

	DrawList->AddRectFilled(CanvasMin, CanvasMax, IM_COL32(28, 30, 36, 255));
	DrawList->PushClipRect(CanvasMin, CanvasMax, true);
}

void FSprite2DCanvas::End()
{
	DrawList->PopClipRect();
	DrawList->AddRect(CanvasMin, CanvasMax, IM_COL32(70, 74, 86, 255));
}

void FSprite2DCanvas::Frame(int32 ContentWidth, int32 ContentHeight)
{
	bFramePending = false;
	if (ContentWidth <= 0 || ContentHeight <= 0)
	{
		return;
	}
	const float Width  = CanvasMax.x - CanvasMin.x;
	const float Height = CanvasMax.y - CanvasMin.y;
	if (Width <= 1.0f || Height <= 1.0f)
	{
		bFramePending = true; // 영역이 아직 없다 (첫 프레임)
		return;
	}
	const float Fit = FMath::Min(Width * 0.9f / static_cast<float>(ContentWidth), Height * 0.9f / static_cast<float>(ContentHeight));
	Zoom            = ZoomLevels[0];
	for (const float Level : ZoomLevels)
	{
		if (Level <= Fit)
		{
			Zoom = Level;
		}
	}
	Offset.x = std::floor((Width - static_cast<float>(ContentWidth) * Zoom) * 0.5f);
	Offset.y = std::floor((Height - static_cast<float>(ContentHeight) * Zoom) * 0.5f);
}

ImVec2 FSprite2DCanvas::ImageToScreen(const FVector2& Point) const
{
	return ImVec2(CanvasMin.x + Offset.x + Point.X * Zoom, CanvasMin.y + Offset.y + Point.Y * Zoom);
}

FVector2 FSprite2DCanvas::ScreenToImage(const ImVec2& Point) const
{
	return FVector2((Point.x - CanvasMin.x - Offset.x) / Zoom, (Point.y - CanvasMin.y - Offset.y) / Zoom);
}

void FSprite2DCanvas::DrawChecker(const FVector2& Min, const FVector2& Max) const
{
	const ImVec2 ScreenMin = ImageToScreen(Min);
	const ImVec2 ScreenMax = ImageToScreen(Max);
	const ImVec2 ClipMin(FMath::Max(ScreenMin.x, CanvasMin.x), FMath::Max(ScreenMin.y, CanvasMin.y));
	const ImVec2 ClipMax(FMath::Min(ScreenMax.x, CanvasMax.x), FMath::Min(ScreenMax.y, CanvasMax.y));
	if (ClipMax.x <= ClipMin.x || ClipMax.y <= ClipMin.y)
	{
		return;
	}
	DrawList->AddRectFilled(ClipMin, ClipMax, IM_COL32(58, 58, 64, 255));
	// 체커 칸은 이미지 원점 기준으로 고정 (이동해도 무늬가 따라간다)
	const int32 FirstX = static_cast<int32>(std::floor((ClipMin.x - ScreenMin.x) / CheckerCell));
	const int32 FirstY = static_cast<int32>(std::floor((ClipMin.y - ScreenMin.y) / CheckerCell));
	for (int32 CellY = FirstY;; ++CellY)
	{
		const float Y0 = ScreenMin.y + static_cast<float>(CellY) * CheckerCell;
		if (Y0 >= ClipMax.y)
		{
			break;
		}
		for (int32 CellX = FirstX;; ++CellX)
		{
			const float X0 = ScreenMin.x + static_cast<float>(CellX) * CheckerCell;
			if (X0 >= ClipMax.x)
			{
				break;
			}
			if (((CellX + CellY) & 1) == 0)
			{
				continue;
			}
			DrawList->AddRectFilled(ImVec2(FMath::Max(X0, ClipMin.x), FMath::Max(Y0, ClipMin.y)),
			                        ImVec2(FMath::Min(X0 + CheckerCell, ClipMax.x), FMath::Min(Y0 + CheckerCell, ClipMax.y)), IM_COL32(78, 78, 86, 255));
		}
	}
}

void FSprite2DCanvas::DrawImage(ImTextureID Texture, int32 Width, int32 Height, bool bNearest, const ImVec2& Uv0, const ImVec2& Uv1) const
{
	DrawImageRect(Texture, FVector2(0.0f, 0.0f), FVector2(static_cast<float>(Width), static_cast<float>(Height)), bNearest, Uv0, Uv1);
}

void FSprite2DCanvas::DrawImageRect(ImTextureID Texture, const FVector2& Min, const FVector2& Max, bool bNearest, const ImVec2& Uv0, const ImVec2& Uv1) const
{
	DrawChecker(FVector2(FMath::Min(Min.X, Max.X), FMath::Min(Min.Y, Max.Y)), FVector2(FMath::Max(Min.X, Max.X), FMath::Max(Min.Y, Max.Y)));
	if (Texture == ImTextureID{})
	{
		return;
	}
	if (bNearest)
	{
		FImGuiLayer::BeginNearestSampling(DrawList);
	}
	DrawList->AddImage(Texture, ImageToScreen(Min), ImageToScreen(Max), Uv0, Uv1);
	if (bNearest)
	{
		FImGuiLayer::EndNearestSampling(DrawList);
	}
}

void FSprite2DCanvas::DrawPixelGrid(int32 Width, int32 Height) const
{
	if (Zoom < MinGridZoom || Width <= 0 || Height <= 0)
	{
		return;
	}
	const FVector2 VisibleMin = ScreenToImage(CanvasMin);
	const FVector2 VisibleMax = ScreenToImage(CanvasMax);
	const int32    X0         = FMath::Clamp(static_cast<int32>(std::floor(VisibleMin.X)), 0, Width);
	const int32    X1         = FMath::Clamp(static_cast<int32>(std::ceil(VisibleMax.X)), 0, Width);
	const int32    Y0         = FMath::Clamp(static_cast<int32>(std::floor(VisibleMin.Y)), 0, Height);
	const int32    Y1         = FMath::Clamp(static_cast<int32>(std::ceil(VisibleMax.Y)), 0, Height);
	const ImU32    Color      = IM_COL32(255, 255, 255, 28);
	for (int32 X = X0; X <= X1; ++X)
	{
		DrawList->AddLine(ImageToScreen(FVector2(static_cast<float>(X), static_cast<float>(Y0))), ImageToScreen(FVector2(static_cast<float>(X), static_cast<float>(Y1))),
		                  Color);
	}
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		DrawList->AddLine(ImageToScreen(FVector2(static_cast<float>(X0), static_cast<float>(Y))), ImageToScreen(FVector2(static_cast<float>(X1), static_cast<float>(Y))),
		                  Color);
	}
}
