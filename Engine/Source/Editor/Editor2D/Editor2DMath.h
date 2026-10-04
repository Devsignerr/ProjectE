#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Sprite/TilemapData.h"

#include <vector>

// 2D 에디터 순수 로직 (Phase 56-5a — GPU·ImGui 없음, 테스트 Editor2D_*). 2D 규약은 Scene/Sprite/SpriteAsset.h 머리 주석:
// 평면 = 월드 X(오른쪽)·Z(위), 2D 카메라는 +Y 쪽에서 -Y를 본다(화면 오른쪽 = +X, 위 = +Z), 2D 각(화면 반시계 +) = FromAxisAngle(+Y, -각).
//
// 타일 스탬프: 셀 값(TileCell) 직사각형. 행 우선 (y, x), 0번 = 왼쪽 아래 (FTilemapRegion과 같은 배치). 0 = 빈칸(칠할 때 건너뜀).
//   스탬프 변환(TransformStamp)은 셀 플래그와 같은 규칙(Rotate90 = 반시계 90° (u, v) → (-v, u) → FlipX → FlipY, 스탬프 가운데 기준)으로
//   칸 배치를 옮기고, 칸마다 타일 플래그를 합성한다(ComposeTileFlags: 바깥 변환 ∘ 칸 변환 = 8가지 중 같은 것 — 2면체군 D4).
//   놓는 기준(GetStampOrigin): 커서 셀 = 스탬프 칸 ((W-1)/2, (H-1)/2) (내림) → 왼쪽 아래 셀 = 커서 - 그 칸.
//   채우기 패턴(SamplePattern): 기준 셀(사각형 왼쪽 아래 / 흐름 채우기 시작 셀의 스탬프 원점)에서 스탬프를 바둑판처럼 반복 — 빈칸도 그대로(지움 아님, 건너뜀).
//   지우개(bErase) = 스탬프 모양(빈칸 아닌 칸)만큼 지운다. 1x1이 아닌 스탬프의 선 = 선 위 셀마다 스탬프.
//   흐름 채우기 = 시작 셀과 같은 값인 4방향 연결 영역(Limit 안, MaxCells 넘으면 아무것도 안 바꾸고 -1). Limit 기본 = 칠한 영역 ∪ 시작 셀을 사방 Margin칸 넓힘
//   (빈칸 영역은 무한하므로 경계 상한).
namespace Editor2DMath
{
	// ---- 타일 변환 합성
	// 결과 플래그 = Outer 변환을 Inner 변환 뒤에 적용한 것 (Inner 먼저). 플래그 비트만 다룬다 (Id 비트는 무시)
	uint32 ComposeTileFlags(uint32 Outer, uint32 Inner);
	// 셀에 변환을 더한다 (빈칸은 그대로)
	uint32 TransformCell(uint32 Cell, uint32 Flags);

	struct FTileStamp
	{
		int32               Width  = 0;
		int32               Height = 0;
		std::vector<uint32> Cells; // 행 우선 (y, x), 0번 = 왼쪽 아래

		bool   IsValid() const { return Width > 0 && Height > 0 && Cells.size() == static_cast<size_t>(Width) * static_cast<size_t>(Height); }
		uint32 Get(int32 X, int32 Y) const { return Cells[static_cast<size_t>(Y) * static_cast<size_t>(Width) + static_cast<size_t>(X)]; }
		void   Set(int32 X, int32 Y, uint32 Cell) { Cells[static_cast<size_t>(Y) * static_cast<size_t>(Width) + static_cast<size_t>(X)] = Cell; }
		static FTileStamp Single(uint32 Cell);
		bool operator==(const FTileStamp& Other) const = default;
	};

	// 스탬프 전체 변환 (칸 배치 + 칸 플래그). Rotate90이면 폭/높이가 바뀐다
	FTileStamp TransformStamp(const FTileStamp& Stamp, uint32 Flags);
	// 타일셋 이미지 칸 사각형(열·행, 왼쪽 위 원점, 양 끝 포함·순서 무관) → 스탬프 (이미지 아래 행 = 스탬프 0행). Columns = 타일셋 열 수
	FTileStamp MakeStampFromTileset(int32 Columns, int32 Col0, int32 Row0, int32 Col1, int32 Row1);

	FTileCoord GetStampOrigin(const FTileStamp& Stamp, const FTileCoord& Cursor);
	// 바둑판 반복 값 (Origin = 스탬프 0번 칸이 놓이는 셀)
	uint32 SamplePattern(const FTileStamp& Stamp, int32 X, int32 Y, const FTileCoord& Origin);

	// ---- 칠하기 연산. 반환 = 값이 바뀐 셀 수 (흐름 채우기는 영역이 너무 크면 -1)
	int32     PaintStamp(FTilemapData& Data, const FTileStamp& Stamp, const FTileCoord& Cursor, bool bErase);
	int32     FillRectPattern(FTilemapData& Data, const FTileRect& Rect, const FTileStamp& Stamp, bool bErase);
	int32     LineStamp(FTilemapData& Data, const FTileCoord& From, const FTileCoord& To, const FTileStamp& Stamp, bool bErase);
	FTileRect ComputeFloodLimit(const FTilemapData& Data, const FTileCoord& Start, int32 Margin = 8);
	int32     FloodFillPattern(FTilemapData& Data, const FTileCoord& Start, const FTileStamp& Stamp, bool bErase, const FTileRect& Limit,
	                           int32 MaxCells = FTilemapData::DefaultFloodFillLimit);
	// 흐름 채우기 영역만 구한다 (바꾸지 않음, 방문 순서 = 너비 우선). 넘치면 false
	bool CollectFloodRegion(const FTilemapData& Data, const FTileCoord& Start, const FTileRect& Limit, int32 MaxCells, std::vector<FTileCoord>& OutCells);

	// ---- 2D 뷰 (+Y에서 -Y를 보는 회전 없는 직교 카메라 — 화면 오른쪽 = +X, 위 = +Z)
	// CameraPlane = 카메라 위치 (X, Z), OrthoHeight = 화면 세로가 담는 높이 (cm), Pixel = 이미지 왼쪽 위 기준 픽셀
	FVector2 ScreenToPlane(const FVector2& CameraPlane, float OrthoHeight, const FVector2& ImageSize, const FVector2& Pixel);
	FVector2 PlaneToScreen(const FVector2& CameraPlane, float OrthoHeight, const FVector2& ImageSize, const FVector2& Plane);
	// 커서 아래 평면 점을 고정한 채 직교 높이를 바꿀 때의 새 카메라 위치 (X, Z)
	FVector2 ZoomAroundCursor(const FVector2& CameraPlane, float OldHeight, float NewHeight, const FVector2& ImageSize, const FVector2& Pixel);
	// 2D 카메라 회전 (yaw -90: 앞 = -Y, 오른쪽 = +X, 위 = +Z)
	FQuat Get2DCameraRotation();
	// 2D 각 (도, 화면 반시계 +) ↔ 회전 (FromAxisAngle(+Y, -각))
	FQuat MakeRotation2D(float AngleDegrees);
	// Step 배수로 반올림 (Origin 기준, Step <= 0이면 그대로)
	float SnapToStep(float Value, float Step, float Origin = 0.0f);

	// ---- 2D 클릭 판정
	bool IsPointInConvexPolygon(const FVector2* Points, int32 Count, const FVector2& Point);
	bool IsPointInPolygon(const std::vector<FVector2>& Points, const FVector2& Point); // 오목 허용 (짝홀)
	float DistanceToSegment(const FVector2& A, const FVector2& B, const FVector2& Point);

	struct FPick2DHit
	{
		FEntity Entity;
		int32   Priority  = 0;    // 큰 것 우선 (스프라이트·타일맵 1, 콜라이더만 있는 엔티티 0)
		uint32  SortLayer = 0;    // 정렬 레이어 칸 (큰 것이 위)
		int32   Order     = 0;    // 레이어 안 순번 (큰 것이 위)
		float   Distance  = 0.0f; // 커서 광선 거리 (작은 것 = 카메라에 가까움 = 위)
	};
	// 맨 앞 후보 번호 (-1 = 없음): Priority → 레이어 → 순번 → 가까운 것 → 목록 앞 (렌더러 정렬과 같은 앞뒤 관계)
	int32 FindFrontmost(const std::vector<FPick2DHit>& Hits);
} // namespace Editor2DMath
