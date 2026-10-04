#pragma once

#include "Core/Math/Math.h"
#include "Renderer/ShaderTypes.h"

#include <span>
#include <vector>

struct FTilesetAsset;
struct FTileDefinition;

// 타일맵 청크 인스턴스 생성 (Phase 56-4b, 순수 로직 — GPU 없음, 테스트 SpriteTiles_*). 그리기 규칙은 Renderer/SpriteRenderer.h 머리 주석,
// 씬 수집·청크 캐시는 Renderer/SpriteSceneCollector.h.
//
// 청크 인스턴스는 **타일맵 엔티티 로컬 공간**(cm, 로컬 X 오른쪽·Z 위, Y = 0 평면)의 FSpriteInstanceGpu다 — 월드 행렬·색·텍스처 칸은
// 프레임마다 청크 머리(FSpriteChunkGpu)로 따로 넘기므로, 타일맵이 움직이거나 색·텍스처(비동기 로드 완료·밉 교체)가 바뀌어도 다시 만들지 않는다.
//
// 타일 사각형 = 셀 가운데 C + T(u - 0.5, v - 0.5) × 셀 크기 (u, v ∈ [0, 1] = 사각형 매개변수, 텍스처는 lerp(UVMin, UVMax, (u, 1 - v))).
//   T = TilemapMath::TransformTileUv(셀 플래그: Rotate90 → FlipX → FlipY)는 선형이므로 Origin = C + T(-0.5, -0.5) × 크기,
//   AxisX = T(1, 0) × 크기, AxisZ = T(0, 1) × 크기. 회전·반전을 UV가 아니라 사각형 축으로 표현한다 (셰이더 UV 식은 축 정렬 lerp뿐) —
//   반전은 와인딩을 뒤집지만 스프라이트 파이프라인은 컬링이 없고 조명 법선은 카메라 쪽으로 뒤집힌다. 정사각형이 아닌 셀의 회전은 칸에 늘여 맞춘다
//   (TilemapData.h 규약과 같음).
// 애니메이션 타일(FTileDefinition::Animation이 있는 타일): 청크 버퍼에 넣지 않고 셀 목록(FAnimatedCell)으로 따로 둔다 — 수집이 프레임마다
//   SelectAnimationFrame(시간 = FFrameTime 총 시간)으로 그릴 타일을 골라 일반 스프라이트 항목으로 그린다 (보통 셀 수가 적다).
namespace SpriteTiles
{
	inline constexpr uint32 FlagPoint        = 1u; // Sprite.hlsl E_SPRITE_FLAG_POINT (SpriteRenderer SpriteFlag_Point)
	inline constexpr uint32 FlagShadowDither = 2u; // SpriteCommon.hlsli E_SPRITE_FLAG_SHADOW_DITHER (그림자 깊이만 — FSpriteShadowRenderer 반투명 그림자)

	// 셀 (X, Y)의 로컬 사각형 (FSpriteInstanceGpu Origin/AxisX/AxisZ 의미, 로컬 Y = 0)
	struct FTileQuad
	{
		FVector3 Origin;
		FVector3 AxisX;
		FVector3 AxisZ;
	};
	FTileQuad ComputeTileQuad(int32 X, int32 Y, uint32 CellFlags, const FVector2& CellSize);

	// 셀 하나 → 로컬 인스턴스 (TileId = 그릴 타일 — 애니메이션이면 고른 프레임, 텍스처 칸 0·색 흰색: 청크 머리가 채움)
	FSpriteInstanceGpu MakeTileInstance(int32 X, int32 Y, uint32 CellFlags, int32 TileId, const FTilesetAsset& Tileset, const FVector2& CellSize);

	// 애니메이션 프레임 선택: 프레임 길이 합으로 감싼 시간이 놓인 프레임의 타일 (반열린 구간 [시작, 끝)). 길이 0 이하 프레임은 건너뛰고,
	// 모두 0 이하이거나 비면 BaseTileId. 음수 시간도 주기로 감싼다
	int32 SelectAnimationFrame(const FTileDefinition& Definition, int32 BaseTileId, double Time);

	struct FAnimatedCell
	{
		int32  X    = 0;
		int32  Y    = 0;
		uint32 Cell = 0; // 원래 셀 값 (플래그 포함)
	};

	struct FChunkBuild
	{
		std::vector<FSpriteInstanceGpu> Instances;   // 셀 방문 순서 (청크 안 (y, x) 행 우선)
		std::vector<FAnimatedCell>      Animated;    // 애니메이션 타일 셀 (방문 순서)
		FBox                            LocalBounds; // 정적 + 애니메이션 셀 전체 (로컬, Y = 0 두께 0)
		uint64                          Hash = 0;    // 셀 좌표·값 (FNV-1a) — 같으면 내용이 같다
	};
	// Cells = 한 청크의 빈칸이 아닌 셀 (X, Y, 값). 범위 밖 타일 Id도 그대로 넣는다 (UV 빈 사각형 = 안 보임 — FTilesetAsset::ComputeTileUv)
	struct FCellRecord
	{
		int32  X    = 0;
		int32  Y    = 0;
		uint32 Cell = 0;
	};
	void   BuildChunk(std::span<const FCellRecord> Cells, const FTilesetAsset& Tileset, const FVector2& CellSize, FChunkBuild& Out);
	// 해시만 (내용 비교용 — 캐시 재사용 판정)
	uint64 HashCells(std::span<const FCellRecord> Cells);
} // namespace SpriteTiles
