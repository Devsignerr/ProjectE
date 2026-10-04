#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>

class FScene;
struct FEditorContext;
struct FRay;
struct FSpriteComponent;
struct FTilemapComponent;

// 2D 에디터 씬 도우미 (Phase 56-5a): 클릭 선택·선택 맞춤 경계·만들기·평면 교차. 메인 스레드 (FSprite2DLibrary 해석)
namespace Editor2DScene
{
	// 광선 ↔ 월드 Y = PlaneY 평면 (앞쪽 교차만). 없으면 false
	bool RayToPlaneY(const FRay& Ray, float PlaneY, FVector3& OutPoint);
	// 광선 ↔ 엔티티 로컬 Y = 0 평면 (스프라이트·타일맵 면). OutLocal = (로컬 X, 로컬 Z), OutT = 광선 거리 (뒤쪽이면 false)
	bool RayToEntityPlane(const FMatrix4x4& World, const FRay& Ray, FVector2& OutLocal, float& OutT);
	// 뷰포트 픽셀 → 커서 광선 (ViewProjection 역행렬)
	FRay MakeRay(const FMatrix4x4& ViewProjection, const FVector2& Pixel, const FVector2& ImageSize);

	// 스프라이트의 로컬 사각형 (로컬 X/Z, 피벗·크기·반전 반영 = 렌더러와 같은 SpriteMath::ComputeQuad). 표시할 슬라이스가 없으면 false
	bool GetSpriteLocalQuad(FSpriteComponent& Sprite, FVector2 (&OutQuad)[4]);
	// 타일맵 셀 크기 (cm, 타일셋 해석 — TilemapCollision::ResolveCellSize). 타일셋이 없으면 컴포넌트 값(0이면 false)
	bool GetTilemapCellSize(FTilemapComponent& Tilemap, FVector2& OutCellSize);

	// 커서 광선 아래 맨 앞 2D 엔티티: 보이는 스프라이트 사각형(투명 픽셀도 포함), 칠한 타일맵 칸, 2D 콜라이더·이동기만 있는 엔티티(뒤 순위).
	// 앞뒤 = Editor2DMath::FindFrontmost (정렬 레이어 → 순번 → 카메라 거리). EdgeTolerance = 열린 선분 콜라이더 판정 폭 (cm).
	// 없으면 NullEntity. OutDistance = 광선 거리
	FEntity Pick(FScene& Scene, const FRay& Ray, float EdgeTolerance, float& OutDistance);

	// 엔티티의 2D 표시 경계(월드)를 더한다: 스프라이트 사각형, 타일맵 칠한 영역, 2D 콜라이더 외곽선. 더한 것이 있으면 true
	bool AddBounds(FScene& Scene, FEntity Entity, FBox& InOutBounds);

	// ---- 만들기 (경로 = Content 기준, 비어도 됨). 이름 = 파일 이름(없으면 기본 이름). 트랜스폼 갱신·선택·MarkEdited는 호출자
	FEntity CreateSprite(FScene& Scene, const std::string& SpritePath, const FVector3& Position);
	FEntity CreateFlipbook(FScene& Scene, const std::string& FlipbookPath, const FVector3& Position);
	FEntity CreateTilemap(FScene& Scene, const std::string& TilesetPath, const FVector3& Position);
	// 직교 카메라 (+Y에서 -Y, 회전 없음). Target = 화면 가운데에 올 평면 점 (Y는 무시 — 카메라는 Y = 2000)
	FEntity CreateCamera2D(FScene& Scene, const FVector3& Target);

	// 메뉴 "2D" (엔티티 메뉴·계층 우클릭): 편집 카메라 시선이 Y = 0 평면에 닿는 곳(없으면 원점)에 만들고 Parent 아래로, 선택 + MarkEdited
	enum class ECreate2D : int32
	{
		Sprite,
		Tilemap,
		Camera,
	};
	FEntity CreateFromMenu(FEditorContext& Context, ECreate2D Kind, FEntity Parent);
} // namespace Editor2DScene
