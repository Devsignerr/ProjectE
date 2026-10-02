#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <bit>
#include <cmath>

// 레이 트레이싱 순수 계산 (Phase 50 — RayTracingCommon.hlsli와 같은 식, 테스트 RayTracingTests).
//   셰이더와 공유하는 식(광선 원점 오프셋, 표본 순서, 원판 표본, 반그림자 반경, 텍스처 LOD)을 바꾸면 HLSL 쪽도 같은 식으로 고친다.
namespace RayTracingMath
{
	// ---- TLAS 인스턴스 마스크 (8비트, 광선의 InstanceInclusionMask와 AND해 0이 아니면 후보)
	constexpr uint8 MaskStatic  = 0x01; // 정적 메시 (불투명/Masked)
	constexpr uint8 MaskSkinned = 0x02; // 스킨 메시 (계산 셰이더 스키닝 + BLAS 갱신)
	constexpr uint8 MaskFoliage = 0x04; // 폴리지 인스턴스
	constexpr uint8 MaskTerrain = 0x08; // 지형 타일
	constexpr uint8 MaskAll     = 0xFF;

	inline uint8 GetInstanceMask(bool bSkinned, bool bFoliage, bool bTerrain)
	{
		return bTerrain ? MaskTerrain : (bSkinned ? MaskSkinned : (bFoliage ? MaskFoliage : MaskStatic));
	}

	// ---- 히트 그룹 오프셋 (D3D12_RAYTRACING_INSTANCE_DESC::InstanceContributionToHitGroupIndex, 24비트)
	//   = 셰이딩 슬롯 × 광선 종류 수. 슬롯 0 = 고정 PBR(MaterialDefault), 1~ = 그래프 셰이더 해시별 히트 그룹.
	//   지금(Phase 50)은 인라인 RayQuery라 셰이더 테이블이 없지만 값을 미리 채워 둔다 — DXR 상태 객체(Phase 51 DDGI/그래프 히트)가
	//   레코드 = 슬롯 × RayTypeCount + 광선 종류(TraceRay의 RayContributionToHitGroupIndex)로 쓴다.
	constexpr uint32 RayTypeCount       = 2; // 0 = 가장 가까운 히트(반사/GI), 1 = 그림자(아무 히트)
	constexpr uint32 MaxHitGroupOffset  = (1u << 24) - 1;
	inline uint32 ComputeHitGroupOffset(uint32 ShadingSlot, uint32 RayTypes = RayTypeCount)
	{
		const uint64 Offset = static_cast<uint64>(ShadingSlot) * RayTypes;
		return Offset > MaxHitGroupOffset ? 0u : static_cast<uint32>(Offset); // 넘치면 고정 PBR 슬롯
	}

	// ---- 인스턴스 플래그 (D3D12_RAYTRACING_INSTANCE_FLAGS와 같은 값)
	constexpr uint32 InstanceFlagCullDisable          = 0x1; // TRIANGLE_CULL_DISABLE (양면 머티리얼)
	constexpr uint32 InstanceFlagFrontCounterClockwise = 0x2; // TRIANGLE_FRONT_COUNTERCLOCKWISE
	constexpr uint32 InstanceFlagForceOpaque          = 0x4; // FORCE_OPAQUE (any-hit/후보 처리 없음)
	constexpr uint32 InstanceFlagForceNonOpaque       = 0x8; // FORCE_NON_OPAQUE (Masked → 후보 알파 테스트)
	// 래스터는 화면 공간에서 앞면(CW)을 판정하므로 반사(행렬식 < 0) 인스턴스는 앞면이 CCW로 보인다. DXR은 객체 공간에서 판정하므로
	// 같은 면을 앞으로 보려면 반사 인스턴스에 FRONT_COUNTERCLOCKWISE를 준다 (래스터와 같은 면을 컬링)
	inline uint32 ComputeInstanceFlags(bool bMasked, bool bTwoSided, bool bMirrored)
	{
		return (bTwoSided ? InstanceFlagCullDisable : 0u) | (bMirrored ? InstanceFlagFrontCounterClockwise : 0u) |
		       (bMasked ? InstanceFlagForceNonOpaque : InstanceFlagForceOpaque);
	}

	// ---- 인스턴스 정보 플래그 (FRayTracingInstanceGpu::Flags — 셰이더가 읽음)
	constexpr uint32 InstanceInfoTwoSided = 1u << 0;
	constexpr uint32 InstanceInfoMasked   = 1u << 1;
	constexpr uint32 InstanceInfoSkinned  = 1u << 2; // 정점이 이미 월드 공간 (인스턴스 변환 = 항등)
	constexpr uint32 InstanceInfoMirrored = 1u << 3; // 행렬식 < 0 (탄젠트 부호 반전)

	// ---- BLAS 캐시 키: 메시 핸들(번호 + 세대) + LOD. 같은 메시를 쓰는 인스턴스는 BLAS 하나를 공유한다.
	//   세대가 바뀌면(메시 삭제 후 재사용) 다른 키 → 예전 항목은 쓰이지 않다가 수명(BlasEvictFrames)이 지나면 지연 해제
	struct FBlasKey
	{
		uint32 MeshIndex      = ~0u;
		uint32 MeshGeneration = 0;
		uint32 Lod            = 0;

		bool operator==(const FBlasKey& Other) const = default;
	};
	inline uint64 HashBlasKey(const FBlasKey& Key)
	{
		uint64 Hash = 14695981039346656037ull; // FNV-1a 64
		for (const uint32 Value : { Key.MeshIndex, Key.MeshGeneration, Key.Lod })
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte)
			{
				Hash ^= (Value >> (Byte * 8)) & 0xFFu;
				Hash *= 1099511628211ull;
			}
		}
		return Hash;
	}
	// LOD 번호 정규화: 메시의 LOD 수 밖이면 마지막 LOD (FStaticMesh::GetLod와 같은 규칙)
	inline uint32 ClampLod(uint32 Lod, uint32 LodCount) { return LodCount == 0 ? 0u : (Lod < LodCount ? Lod : LodCount - 1); }
	// 수명: 마지막으로 TLAS에 들어간 프레임 + 이만큼 지나면 해제 (그동안 다시 보이면 재사용 — 카메라를 돌릴 때 재빌드 방지)
	constexpr uint64 BlasEvictFrames = 300;
	inline bool ShouldEvictBlas(uint64 LastUsedFrame, uint64 CurrentFrame, uint64 EvictFrames = BlasEvictFrames)
	{
		return CurrentFrame > LastUsedFrame + EvictFrames;
	}

	// ---- 광선 원점 오프셋 (Wächter & Binder, "A Fast and Robust Method for Avoiding Self-Intersection", Ray Tracing Gems 6장).
	//   교차점(무게중심 보간 위치)을 기하 법선 쪽으로 몇 ulp 민다 — 좌표 크기에 비례하므로 cm 단위 큰 좌표에도 맞는다.
	//   깊이 버퍼에서 되살린 위치(1차 표면)는 오차가 더 크므로 ComputeSurfaceBias를 따로 더한다
	constexpr float OffsetOrigin     = 1.0f / 32.0f;
	constexpr float OffsetFloatScale = 1.0f / 65536.0f;
	constexpr float OffsetIntScale   = 256.0f;
	inline float OffsetComponent(float P, float N)
	{
		const int32 Of     = static_cast<int32>(OffsetIntScale * N);
		const float Stepped = std::bit_cast<float>(std::bit_cast<int32>(P) + (P < 0.0f ? -Of : Of));
		return std::fabs(P) < OffsetOrigin ? P + OffsetFloatScale * N : Stepped;
	}
	inline FVector3 OffsetRayOrigin(const FVector3& P, const FVector3& N)
	{
		return FVector3(OffsetComponent(P.X, N.X), OffsetComponent(P.Y, N.Y), OffsetComponent(P.Z, N.Z));
	}

	// 깊이 버퍼로 되살린 1차 표면의 법선 방향 바이어스 (cm): 기본 + 뷰 깊이 비례 + 빛에 비스듬할수록 더 (그림자 경계 여드름 방지)
	//   (D32 깊이 재구성 오차는 깊이² 비례로 작지만, 보간 법선 ≠ 면 법선인 낮은 폴리곤 곡면의 자기 교차를 덮을 만큼 준다)
	constexpr float SurfaceBiasBase       = 0.5f;    // cm
	constexpr float SurfaceBiasDepthScale = 0.0005f; // 뷰 깊이 1cm당
	inline float ComputeSurfaceBias(float ViewDepth, float NdotL, float Scale = 1.0f)
	{
		const float Grazing = 1.0f - FMath::Clamp(NdotL, 0.0f, 1.0f);
		return (SurfaceBiasBase + FMath::Max(ViewDepth, 0.0f) * SurfaceBiasDepthScale) * (1.0f + Grazing) * Scale;
	}

	// ---- 결정적 표본 순서 (블루 노이즈 성질의 교차 표본 — Keller & Heidrich 2001 interleaved sampling):
	//   픽셀 4x4 블록 안에서 Bayer 순서로 표본 번호를 나눠 가져 어느 4x4 창에도 16개 표본이 모두 있고, 프레임마다 한 칸씩 돌린다.
	//   → 정지 화면에서 4x4 이상 공간 필터의 결과가 프레임마다 같다 (확률 표본의 TV 노이즈 없음 — 시간 안정성 규칙)
	constexpr uint32 InterleavedSampleCount = 16;
	inline uint32 GetBayer4x4(uint32 X, uint32 Y)
	{
		constexpr uint32 Bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
		return Bayer[(Y & 3u) * 4u + (X & 3u)];
	}
	inline uint32 GetInterleavedSampleIndex(uint32 X, uint32 Y, uint32 Frame)
	{
		return (GetBayer4x4(X, Y) + Frame) % InterleavedSampleCount;
	}
	// 단위 원판 위 표본 Index/Count (Vogel 황금각 나선: 반경 ∝ sqrt — 면적 고르게)
	inline FVector2 GetDiskSample(uint32 Index, uint32 Count)
	{
		const float T     = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Count);
		const float Angle = static_cast<float>(Index) * 2.39996323f;
		const float R     = std::sqrt(T);
		return FVector2(R * std::cos(Angle), R * std::sin(Angle));
	}
	// 방향 Axis(정규화) 둘레 반각 tan = TanHalfAngle 원뿔 안 방향: 원판 표본 Disk(단위 원판)를 축에 수직인 평면에 올린다
	inline FVector3 SampleConeDirection(const FVector3& Axis, float TanHalfAngle, const FVector2& Disk)
	{
		const FVector3 Helper = FMath::Abs(Axis.Z) < 0.999f ? FVector3(0.0f, 0.0f, 1.0f) : FVector3(1.0f, 0.0f, 0.0f);
		const FVector3 Tangent   = FVector3::Cross(Helper, Axis).GetNormalized();
		const FVector3 Bitangent = FVector3::Cross(Axis, Tangent);
		return (Axis + (Tangent * Disk.X + Bitangent * Disk.Y) * TanHalfAngle).GetNormalized();
	}

	// ---- 반그림자 공간 필터 반경 (픽셀): 차폐물까지 거리 × tan(태양 반각) = 반그림자 반폭(cm) → 화면 픽셀.
	//   교차 표본 패턴(4x4)을 지우는 최소 반경 MinRadius는 항상 (차폐물 없음/가까운 접촉 그림자도)
	inline float ComputePenumbraRadiusPixels(float BlockerDistance, float TanHalfAngle, float PixelsPerUnit, float MinRadius, float MaxRadius)
	{
		const float Radius = FMath::Max(BlockerDistance, 0.0f) * TanHalfAngle * PixelsPerUnit;
		return FMath::Clamp(Radius, MinRadius, MaxRadius);
	}

	// ---- 히트 텍스처 LOD (광선 원뿔, Akenine-Möller et al. "Texture Level of Detail Strategies for Real-Time Ray Tracing", RTG 20장)
	//   LOD = 0.5 log2(텍셀 면적 / 월드 면적) + log2(원뿔 폭 / |N·D|). 텍셀 면적 = 삼각형 UV 면적 × 텍스처 W × H
	inline float ComputeRayConeLod(float ConeWidth, float AbsCosNormalRay, float UvArea, float TextureTexels, float WorldArea)
	{
		const float TexelArea = FMath::Max(UvArea * TextureTexels, 1.0e-12f);
		const float Lambda    = 0.5f * std::log2(TexelArea / FMath::Max(WorldArea, 1.0e-12f));
		return FMath::Max(Lambda + std::log2(FMath::Max(ConeWidth, 1.0e-6f) / FMath::Max(AbsCosNormalRay, 1.0e-3f)), 0.0f);
	}
	// 원뿔 폭 (월드): 1차 표면까지 거리 × 픽셀 각 + 히트까지 거리 × (픽셀 각 + 거칠기 퍼짐 각)
	inline float ComputeReflectionConeWidth(float SurfaceDistance, float HitDistance, float PixelAngle, float SpreadAngle)
	{
		return SurfaceDistance * PixelAngle + HitDistance * (PixelAngle + SpreadAngle);
	}
} // namespace RayTracingMath
