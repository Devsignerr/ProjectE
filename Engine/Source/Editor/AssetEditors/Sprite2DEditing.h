#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Sprite/FlipbookAsset.h"
#include "Scene/Sprite/SpriteAsset.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// 2D 에셋 편집기(Phase 56-5b — 스프라이트 아틀라스/플립북/타일셋)의 순수 편집 연산. ImGui 없이 에셋 구조체만 고친다 (EditorTests: Sprite2DEditing_*).
// 화면은 SpriteAtlasEditor/FlipbookEditor/TilesetEditor가 그리고, 구조를 바꾸는 편집은 모두 이 함수들을 거친다.
//
// 규칙
//   - 좌표: 이미지 px(왼쪽 위 원점, 아래로 +) 실수. 캔버스가 마우스 위치를 이 좌표로 바꿔 넘긴다.
//   - 픽셀 스냅: 사각형 모서리·다각형 점은 가장 가까운 정수 px(반올림). 사각형은 정규화(음수 크기 → 모서리 교환) 후 텍스처 안으로 자른다
//     (텍스처 크기 0 이하 = 자르지 않음). 크기 조절이 반대쪽 모서리를 넘으면 뒤집혀 그쪽 모서리가 되고, 크기는 최소 1px.
//   - 이동은 크기를 유지한 채 텍스처 안에 머문다.
//   - 이름: 슬라이스 이름은 아틀라스 안에서 고유(대소문자 구분). 새 이름이 겹치면 MakeUniqueName이 "<이름>_<번호>"(1부터)로 비켜 간다.
//   - 격자 자르기(SliceGridWithOptions): 셀 크기 모드 = SpriteMath::SliceGrid 그대로, 개수 모드 = 셀 크기 = (텍스처 - 2×Margin - (n-1)×Spacing) / n 내림.
//     빈 칸(알파가 모두 0) 건너뛰기는 이미지 픽셀이 있을 때만. 이름 번호는 남은 칸에 0부터 다시 매긴다.
//   - 플립북 프레임 조작: 프레임을 옮기거나 지우면 이벤트의 Frame 번호도 같은 프레임을 따라간다(지운 프레임의 이벤트는 지움).
//     선택 = 프레임 번호 목록(정렬·중복 제거해서 다룸), 결과 선택을 돌려준다.
//   - 타일 다각형: 점은 타일 로컬 px [0, 타일 크기]로 자른다. 물리는 오목하거나 8점을 넘으면 볼록 껍질(8점)로 바꾸므로 편집기는 경고만 한다.
namespace Sprite2DEditing
{
	struct FPixelRect
	{
		int32 X = 0;
		int32 Y = 0;
		int32 W = 0;
		int32 H = 0;

		bool operator==(const FPixelRect& Other) const = default;
	};

	FPixelRect GetRect(const FSpriteSlice& Slice);
	void       SetRect(FSpriteSlice& Slice, const FPixelRect& Rect);

	// 사각형 끌기 핸들
	enum class ERectHandle : int32
	{
		None,
		Move,
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
	};

	// 두 점(드래그 시작/현재)을 픽셀 스냅한 사각형. 크기가 0이면 W/H = 0 (너무 작은 드래그 — 호출자가 무시)
	FPixelRect MakeRectFromDrag(const FVector2& Start, const FVector2& End, int32 TextureWidth, int32 TextureHeight);
	// 끌기 시작 사각형 + 핸들 + 이동량(px, 실수 — 반올림) → 새 사각형
	FPixelRect ApplyRectDrag(const FPixelRect& Original, ERectHandle Handle, const FVector2& DeltaPixels, int32 TextureWidth, int32 TextureHeight);
	// 음수 크기 정규화 (모서리 교환) + 최소 1px
	FPixelRect NormalizeRect(int32 Left, int32 Top, int32 Right, int32 Bottom);
	FPixelRect ClampRect(const FPixelRect& Rect, int32 TextureWidth, int32 TextureHeight);
	// 점(이미지 px)이 어느 핸들 위인지. HandleRadius = px 단위 허용 거리 (캔버스 배율로 화면 몇 픽셀을 바꾼 값)
	ERectHandle HitTestRect(const FPixelRect& Rect, const FVector2& Point, float HandleRadius);

	// 피벗: 이미지 점 → 슬라이스 피벗 (아래 0 → 위 1). bSnap이면 픽셀 경계/가운데(0.5px) 단위
	FVector2 PivotFromImagePoint(const FPixelRect& Rect, const FVector2& Point, bool bSnap);
	FVector2 PivotToImagePoint(const FPixelRect& Rect, const FVector2& Pivot);

	struct FPivotPreset
	{
		const char* Label;
		FVector2    Pivot;
	};
	std::span<const FPivotPreset> GetPivotPresets();

	// 9-슬라이스 테두리: Side 0 왼 1 위 2 오른 3 아래. 점(이미지 px)까지의 거리로 그 쪽 테두리 값을 정하고 반대쪽과 합이 크기를 넘지 않게 자른다
	void ApplyBorderDrag(FSpriteSlice& Slice, int32 Side, const FVector2& Point);

	// 이름
	bool        IsNameTaken(const FSpriteAsset& Asset, std::string_view Name, int32 IgnoreIndex = -1);
	std::string MakeUniqueName(const FSpriteAsset& Asset, std::string_view Base, int32 IgnoreIndex = -1);
	// 이름 바꾸기 검사: 빈 이름/중복이면 오류 문구, 괜찮으면 빈 문자열
	std::string ValidateRename(const FSpriteAsset& Asset, int32 Index, std::string_view NewName);
	// 이름 바꾸기 + 이 아틀라스를 쓰는 플립북 프레임은 호출자가 따로 고친다 (파일 밖이라 여기서 다루지 않음)
	bool        RenameSlice(FSpriteAsset& Asset, int32 Index, std::string_view NewName, std::string* OutError = nullptr);

	enum class ESliceSort : int32
	{
		Name,     // 이름 (숫자는 크기순 — "A2" < "A10")
		Position, // 위 → 아래, 왼 → 오른 (Y 다음 X)
	};
	// 정렬. 선택된 슬라이스 이름을 유지하려면 호출자가 이름으로 다시 찾는다
	void SortSlices(FSpriteAsset& Asset, ESliceSort Sort);
	// 자연 정렬 비교 (숫자 덩어리는 값으로)
	bool NaturalLess(std::string_view A, std::string_view B);

	// 새 슬라이스 추가 (이름 자동 고유). 반환 = 칸
	int32 AddSlice(FSpriteAsset& Asset, const FPixelRect& Rect, std::string_view BaseName);

	struct FGridSliceOptions
	{
		bool        bByCount   = false; // false = 셀 크기, true = 열/행 개수
		int32       CellWidth  = 16;
		int32       CellHeight = 16;
		int32       Columns    = 4;
		int32       Rows       = 4;
		int32       Margin     = 0;
		int32       Spacing    = 0;
		std::string NamePrefix = "Slice_";
		bool        bSkipEmpty = true;
	};
	// 이미지 픽셀 (RGBA8, 위 행부터). Pixels가 비면 빈 칸 판정 없음
	struct FImageView
	{
		int32                 Width  = 0;
		int32                 Height = 0;
		std::span<const uint8> Pixels;
	};
	bool IsRegionTransparent(const FImageView& Image, const FPixelRect& Rect);
	// 개수 모드의 셀 크기 (0 이하면 자를 수 없음)
	void ComputeGridCellSize(int32 TextureWidth, int32 TextureHeight, const FGridSliceOptions& Options, int32& OutCellWidth, int32& OutCellHeight);
	std::vector<FSpriteSlice> SliceGridWithOptions(int32 TextureWidth, int32 TextureHeight, const FGridSliceOptions& Options, const FImageView& Image);
	// 자른 결과를 아틀라스에: bReplace면 기존을 지우고, 아니면 덧붙이며 이름이 겹치면 고유하게
	void ApplyGridSlices(FSpriteAsset& Asset, std::vector<FSpriteSlice> Slices, bool bReplace);

	// ---- 플립북
	std::vector<int32> NormalizeSelection(std::vector<int32> Selection, int32 Count);
	// Index 앞에 슬라이스 이름마다 프레임 하나씩 (Index < 0 또는 범위 밖 = 끝). 이벤트는 뒤로 밀린다. 반환 = 새 프레임 번호들
	std::vector<int32> InsertFrames(FFlipbookAsset& Asset, int32 Index, std::span<const std::string> SliceNames);
	// 선택 프레임들을 InsertBefore(0~N, 원래 번호 기준) 앞으로 순서 유지한 채 옮긴다. 반환 = 옮긴 뒤 선택
	std::vector<int32> MoveFrames(FFlipbookAsset& Asset, std::vector<int32> Selection, int32 InsertBefore);
	// 선택 프레임들의 사본을 마지막 선택 프레임 뒤에 (이벤트는 복제하지 않음). 반환 = 사본 번호들
	std::vector<int32> DuplicateFrames(FFlipbookAsset& Asset, std::vector<int32> Selection);
	// 선택 프레임 삭제 (그 프레임 이벤트도). 반환 = 삭제 뒤 선택할 번호 (없으면 -1)
	int32 RemoveFrames(FFlipbookAsset& Asset, std::vector<int32> Selection);

	// ---- 타일 다각형
	FVector2 SnapTilePoint(const FVector2& Point, int32 TileWidth, int32 TileHeight, bool bSnap);
	// 가장 가까운 점 (MaxDistance 밖이면 -1)
	int32 FindNearestPoint(std::span<const FVector2> Points, const FVector2& Point, float MaxDistance);
	// 가장 가까운 변에 점 끼우기 (점이 2개 이하면 끝에). 반환 = 새 점 번호
	int32 InsertPointOnNearestEdge(std::vector<FVector2>& Points, const FVector2& Point);
	// 경고 문구 (없으면 빈 문자열): 3점 미만 / 8점 초과 / 오목·같은 직선
	std::string GetPolygonWarning(std::span<const FVector2> Points);
	constexpr uint32 MaxPhysicsPolygonPoints = 8;

	// 타일셋: 고친 타일 정의를 넣는다 (기본값이면 목록에서 빠짐 — Normalize)
	FTileDefinition GetTile(const FTilesetAsset& Asset, int32 Id);
	void            SetTile(FTilesetAsset& Asset, const FTileDefinition& Tile);
} // namespace Sprite2DEditing
