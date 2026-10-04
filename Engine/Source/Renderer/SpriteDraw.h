#pragma once

#include "Core/Math/Math.h"
#include "Scene/ResourceHandles.h"
#include "Scene/Sprite/SpriteAsset.h"

#include <span>
#include <vector>

// 2D 스프라이트 그리기 항목과 순수 로직 (정렬·배치·사각형 계산 — GPU 없음, 테스트 SpriteDraw_*). 그리기는 Renderer/SpriteRenderer.h
//
// 2D 규약: 평면 = 월드 X(오른쪽)·Z(위), 2D 카메라는 +Y 쪽에서 -Y를 본다(화면 오른쪽 +X, 위 +Z), 사각형 앞면 = +Y. 다만 렌더러 코어는
// 이 규약에 묶이지 않는다 — 항목은 월드 변환 행렬로 놓인 "로컬 X/Z 평면 사각형"이므로 어떤 방향의 사각형도 그린다.

// 블렌드 모드 (번호는 끝에만 추가)
enum class ESpriteBlendMode : uint8
{
	Alpha,         // 직선 알파: 색 × a + 뒤 × (1 - a). 깊이 쓰기 없음
	Premultiplied, // 프리멀티플라이드 알파 텍스처: 색 + 뒤 × (1 - a). 깊이 쓰기 없음
	Additive,      // 가산: 색 × a + 뒤. 깊이 쓰기 없음
	Masked,        // 알파 < AlphaCutoff면 버림, 나머지 불투명. 깊이 씀 (뒤에 그리는 반투명 메시·파티클이 가려진다)
	Count
};

// 텍스처 필터 = Scene/Sprite/SpriteAsset.h ESpriteFilter (Point = 최근접·항상 밉 0 — Sprite.hlsl SampleSprite 주석, Linear = 선형 + 밉·TAAU 밉 바이어스)

// 스프라이트 하나. 로컬 사각형 = 로컬 X ∈ [-Pivot.X, 1 - Pivot.X] × Size.X, 로컬 Z ∈ [-Pivot.Y, 1 - Pivot.Y] × Size.Y (Y = 0 평면),
// 월드 = 로컬 * World (행 벡터 규약). 텍스처 v는 아래로 증가: 로컬 아래 변 = UVMax.Y, 위 변 = UVMin.Y.
// 좌우/상하 반전은 UVMin/UVMax의 해당 성분을 서로 바꿔 넣는다.
struct FSpriteDrawItem
{
	FMatrix4x4     World = FMatrix4x4::Identity;
	FVector2       Size  = FVector2(100.0f, 100.0f); // cm
	FVector2       Pivot = FVector2(0.5f, 0.5f);     // 0 = 왼쪽/아래, 1 = 오른쪽/위
	FVector2       UVMin = FVector2(0.0f, 0.0f);
	FVector2       UVMax = FVector2(1.0f, 1.0f);
	FTextureHandle Texture;                                // FResourceManager 핸들 (소유하지 않음, 무효/준비 전 = 흰색)
	FVector4       Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f); // 선형 RGBA (sRGB 색은 호출자가 선형으로 바꿔 넘긴다)
	int32          SortLayer    = 0;                       // 정렬 레이어 (작은 것부터 그림)
	int32          OrderInLayer = 0;                       // 레이어 안 순번 (작은 것부터)
	ESpriteBlendMode Blend      = ESpriteBlendMode::Alpha;
	ESpriteFilter    Filter     = ESpriteFilter::Linear;
	float          AlphaCutoff  = 0.5f;  // Masked 전용
	bool           bLit         = false; // 방향광(그림자) + 로컬 라이트 + 하늘 환경광 (Sprite.hlsl ShadeSprite)
	bool           bCastShadows = false; // 후속 (아직 그림자 패스에 넣지 않음)
};

namespace SpriteMath
{
	// 월드 사각형 = Origin + U * AxisX + V * AxisZ (U, V ∈ [0, 1], V = 로컬 위쪽)
	struct FQuad
	{
		FVector3 Origin; // 로컬 왼쪽 아래 모서리
		FVector3 AxisX;  // 로컬 X 변 전체
		FVector3 AxisZ;  // 로컬 Z 변 전체
	};
	FQuad    ComputeQuad(const FSpriteDrawItem& Item);
	FVector3 GetCornerPosition(const FQuad& Quad, float U, float V);
	// 모서리 UV (Sprite.hlsl SpriteVS와 같은 식): lerp(UVMin, UVMax, (U, 1 - V))
	FVector2 GetCornerUV(const FSpriteDrawItem& Item, float U, float V);
	// 정렬 깊이 = 사각형 가운데의 카메라 시선 거리
	float    ComputeSortDepth(const FQuad& Quad, const FVector3& CameraPosition, const FVector3& CameraForward);
} // namespace SpriteMath

namespace SpriteSorting
{
	struct FKey
	{
		int32 Layer = 0;
		int32 Order = 0;
		float Depth = 0.0f; // 카메라 시선 거리 (큰 것 = 먼 것부터)
	};
	// 그리기 순서: 레이어 오름차순 → 레이어 안 순번 오름차순 → 깊이 내림차순(먼 것 먼저) → 제출 순서. 안정·결정적.
	// 키 32비트 3개를 바이트 단위 LSD 기수 정렬 (모든 항목이 같은 바이트는 그 단계를 건너뛴다 — 2D 직교처럼 깊이가 같으면 깊이 4단계가 빠진다).
	// OutOrder = 그릴 순서의 항목 번호
	void Sort(std::span<const FKey> Keys, std::vector<uint32>& OutOrder);
	// 깊이 → 내림차순 정렬 비트 (NaN = 0, -0 = +0)
	uint32 DepthToDescendingBits(float Depth);
} // namespace SpriteSorting

namespace SpriteBatching
{
	// 파이프라인 키 = 블렌드 × 조명 (PSO 하나). 텍스처·필터는 인스턴스 값(바인드리스)이라 묶음을 끊지 않는다
	inline uint32 MakePipelineKey(ESpriteBlendMode Blend, bool bLit) { return static_cast<uint32>(Blend) * 2u + (bLit ? 1u : 0u); }
	inline constexpr uint32 PipelineKeyCount = static_cast<uint32>(ESpriteBlendMode::Count) * 2u;

	struct FRun
	{
		uint32 First       = 0; // 그리기 순서 안 시작
		uint32 Count       = 0;
		uint32 PipelineKey = 0;
	};
	// 그리기 순서대로 늘어선 파이프라인 키를 같은 키 연속 구간으로 나눈다 (구간 = 인스턴스 그리기 한 번)
	void BuildRuns(std::span<const uint8> PipelineKeys, std::vector<FRun>& OutRuns);
} // namespace SpriteBatching
