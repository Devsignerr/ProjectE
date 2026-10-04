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

// 블렌드 모드 ESpriteBlendMode·텍스처 필터 ESpriteFilter·9-슬라이스 방식 ESpriteSliceMode는 2D 데이터 계층(Scene/Sprite/SpriteAsset.h)의
// enum을 같이 쓴다 (컴포넌트 필드와 같은 값) — 렌더러 쪽 중복 정의 금지

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
	float          AlphaCutoff  = 0.5f;  // Masked 그리기 + 그림자 깊이 clip (모든 블렌드 — SpriteShadow.hlsl)
	bool           bLit         = false; // 방향광(그림자) + 로컬 라이트 + 하늘 환경광 (Sprite.hlsl ShadeSprite)
	bool           bCastShadows = false; // 그림자 캐스터 목록 항목 (Renderer/SpriteShadowRenderer.h — 그리기 목록에서는 쓰지 않음)
	bool           bShadowStatic = false; // 그림자 캐스터: 방향광 그림자 캐시의 정적 캐스터 (그리는 값이 r.Shadow.Cache.StaticFrames 수집 연속 같음)
	// TAA (Renderer/SpriteRenderer.h 머리 주석 "TAA"): 직전 프레임 월드 행렬(Masked 움직임 벡터 — 없으면 World와 같게)과
	// 그리는 값이 직전 프레임과 같은가(반투명 계열 반응형 마스크를 끈다 — 앱 목록처럼 모르면 false = 반응형)
	FMatrix4x4     PrevWorld     = FMatrix4x4::Identity;
	bool           bHasPrevWorld = false;
	bool           bStatic       = false;
};

// 타일맵 청크 하나 (Phase 56-4b — Renderer/SpriteSceneCollector.h가 만든다). 인스턴스는 정적 GPU 버퍼에 이미 있고(타일맵 로컬 공간,
// SpriteTiles.h), 이 항목은 정렬 키와 프레임마다 바뀌는 값(월드 행렬·색·텍스처)만 가진다. 정렬은 스프라이트 항목과 같은 키로 섞이고
// 그리기 순서상 자기 자리에서 구간 하나가 된다 (청크끼리·항목과 합쳐지지 않는다). 같은 키(레이어·순번·깊이)면 청크가 항목보다 먼저(아래)
struct FSpriteChunkDraw
{
	uint64           Instances = 0;                          // 정적 인스턴스 버퍼 GPU 주소 (FSpriteInstanceGpu × Count, 로컬 공간)
	uint32           Count     = 0;
	FMatrix4x4       World     = FMatrix4x4::Identity;       // 로컬 → 월드 (행 벡터)
	FTextureHandle   Texture;                                // 타일셋 텍스처 (소유하지 않음)
	FVector4         Color     = FVector4(1.0f, 1.0f, 1.0f, 1.0f); // 선형 RGBA (인스턴스 색에 곱함)
	FVector3         Center;                                 // 월드 경계 가운데 (정렬 깊이)
	int32            SortLayer    = 0;
	int32            OrderInLayer = 0;
	ESpriteBlendMode Blend        = ESpriteBlendMode::Alpha;
	float            AlphaCutoff  = 0.5f;  // 청크 머리로 넘겨 인스턴스 값을 덮는다 (Masked 그리기 + 그림자 clip)
	bool             bLit         = false;
	// 그림자 캐스터 목록에서만 (Renderer/SpriteShadowRenderer.h)
	FBox             Bounds;                // 월드 경계 (장 프러스텀 컬링)
	bool             bShadowStatic = false; // 방향광 그림자 캐시의 정적 캐스터 (타일맵 월드·색·내용이 연속 같음)
	// TAA (FSpriteDrawItem과 같은 뜻)
	FMatrix4x4       PrevWorld     = FMatrix4x4::Identity;
	bool             bHasPrevWorld = false;
	bool             bStatic       = false;
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

// 9-슬라이스 (슬라이스 Border가 있고 그릴 크기가 슬라이스 원래 크기와 다를 때 — 씬 수집 FSpriteSceneCollector가 항목을 조각으로 나눈다).
// 방식 = CPU에서 조각 항목으로 (조각마다 FSpriteDrawItem 하나 — 같은 정렬 키·연속 제출이라 같은 구간으로 묶인다).
//   VS가 인스턴스 테두리 값으로 18정점(9칸)을 만드는 방식보다 단순하다: 인스턴스 형식(80B)·셰이더·그림자 경로·컬링·정렬이 그대로이고
//   반복(Tile)처럼 조각 수가 크기에 따라 바뀌는 경우도 같은 코드로 된다. 9-슬라이스는 보통 UI 패널·발판 같은 소수라 항목 9배 비용은 작다.
// 규칙: 모서리 = 원래 크기(px × UnitsPerPixel), 왼/오른 가장자리 = 세로만, 위/아래 가장자리 = 가로만, 가운데 = 두 축으로 늘인다(Stretch)
//   또는 원래 가운데 크기로 반복(Tile — 가운데 영역 왼쪽 아래에서 시작, 마지막 칸은 잘라 맞추고 UV도 그만큼 줄임).
//   그릴 크기가 두 테두리 합보다 작으면 그 축의 테두리를 비율로 줄인다(유니티와 같음 — UV는 그대로, 가운데는 폭 0이라 빠짐).
//   반전은 피벗을 지나는 축 거울 (조각 위치 부호 반전 + UV 교환 — 일반 항목 반전과 같은 사각형). 크기 0 조각은 만들지 않는다.
//   반복 칸 수가 축마다 MaxTilesPerAxis를 넘으면 그 축은 늘이기로 (조각 폭주 방지).
namespace SpriteNineSlice
{
	inline constexpr int32 MaxTilesPerAxis = 128;

	struct FInput
	{
		FVector2         Size;            // 그릴 크기 (cm, SpriteMath::ComputeSize)
		FVector2         OriginalSize;    // 슬라이스 원래 크기 (px × UnitsPerPixel, cm)
		FVector2         Pivot;           // 슬라이스 피벗 (반전 전, 아래 0)
		float            BorderLeft   = 0.0f; // cm (px × UnitsPerPixel)
		float            BorderTop    = 0.0f;
		float            BorderRight  = 0.0f;
		float            BorderBottom = 0.0f;
		FVector2         UVMin;           // 슬라이스 UV 왼쪽 위 (반전 전)
		FVector2         UVMax;           // 오른쪽 아래
		bool             bFlipX = false;
		bool             bFlipY = false;
		ESpriteSliceMode Mode   = ESpriteSliceMode::Stretch;
	};
	// 조각: 로컬 (X, Z) 사각형 (cm, 원점 = 엔티티 원점 = 피벗) + UV (FSpriteDrawItem 규약: UVMin.X = 로컬 왼쪽, UVMin.Y = 로컬 위)
	struct FPiece
	{
		FVector2 Min;
		FVector2 Max;
		FVector2 UVMin;
		FVector2 UVMax;
	};
	// 9-슬라이스로 그려야 하는가 (테두리가 있고 크기가 원래와 다름 — 같으면 한 장으로 그린 것과 같다)
	bool ShouldSlice(const FSpriteSlice& Slice, const FVector2& Size, const FVector2& OriginalSize);
	// 조각 목록 (아래 줄 → 위 줄, 줄 안 왼쪽 → 오른쪽 — 반전 전 기준 순서). Out은 비우고 채운다
	void Build(const FInput& Input, std::vector<FPiece>& Out);
	// 조각 → 항목 (Base의 월드·색·텍스처·정렬·블렌드 등을 그대로, 크기·피벗·UV만 조각 것)
	FSpriteDrawItem MakePieceItem(const FSpriteDrawItem& Base, const FPiece& Piece);
} // namespace SpriteNineSlice

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
	// 파이프라인 키 = 블렌드 × 조명 × 정지 (PSO 하나). 텍스처·필터는 인스턴스 값(바인드리스)이라 묶음을 끊지 않는다.
	// 정지(bStatic) = 반투명 계열이 씬 컬러 알파(TAA 반응형 마스크)를 건드리지 않는 변형 — Masked는 움직임 벡터를 쓰므로 정지 변형이 없다(false로 접음)
	inline uint32 MakePipelineKey(ESpriteBlendMode Blend, bool bLit, bool bStatic = false)
	{
		const bool bStaticVariant = bStatic && Blend != ESpriteBlendMode::Masked;
		return (static_cast<uint32>(Blend) * 2u + (bLit ? 1u : 0u)) * 2u + (bStaticVariant ? 1u : 0u);
	}
	inline constexpr uint32 PipelineKeyCount = static_cast<uint32>(ESpriteBlendMode::Count) * 4u;

	struct FRun
	{
		uint32 First       = 0; // 항목 구간: 항목만 센 그리기 순서 안 시작 / 청크 구간: 청크 번호
		uint32 Count       = 0; // 항목 구간: 항목 수 / 청크 구간: 0 (호출자가 청크 인스턴스 수로 채운다)
		uint32 PipelineKey = 0;
		bool   bChunk      = false;
	};
	// 그리기 순서대로 늘어선 파이프라인 키를 같은 키 연속 구간으로 나눈다 (구간 = 인스턴스 그리기 한 번)
	void BuildRuns(std::span<const uint8> PipelineKeys, std::vector<FRun>& OutRuns);
	// 청크가 섞인 그리기 순서: ChunkIndices[i] >= 0이면 그 자리는 청크(혼자 한 구간, First = 청크 번호), -1이면 항목.
	// 항목 구간의 First는 청크를 뺀 항목 순번 (항목 인스턴스는 업로드 버퍼에 청크 없이 이어 쓴다). 청크를 사이에 둔 같은 키 항목은 합치지 않는다
	void BuildRuns(std::span<const uint8> PipelineKeys, std::span<const int32> ChunkIndices, std::vector<FRun>& OutRuns);
} // namespace SpriteBatching
