#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <vector>

// 레이 트레이싱 순수 계산 (Phase 50 — RayTracingCommon.hlsli와 같은 식, 테스트 RayTracingTests).
//   셰이더와 공유하는 식(광선 원점 오프셋, 표본 순서, 원판 표본, 반그림자 반경, 텍스처 LOD)을 바꾸면 HLSL 쪽도 같은 식으로 고친다.
namespace RayTracingMath
{
	// ---- TLAS 인스턴스 마스크 (8비트, 광선의 InstanceInclusionMask와 AND해 0이 아니면 후보)
	constexpr uint8 MaskStatic  = 0x01; // 정적 메시 (불투명/Masked)
	constexpr uint8 MaskSkinned = 0x02; // 스킨 메시 (계산 셰이더 스키닝 + BLAS 갱신)
	constexpr uint8 MaskFoliage = 0x04; // 폴리지 인스턴스
	constexpr uint8 MaskTerrain = 0x08; // 지형 타일
	constexpr uint8 MaskShadowCaster = 0x10; // 그림자를 드리운다 (FMeshInstance::CastsShadow) — 그림자 광선은 이 비트만 포함
	constexpr uint8 MaskTypes   = MaskStatic | MaskSkinned | MaskFoliage | MaskTerrain; // 반사/GI 광선 포함 마스크 (종류 비트 중 하나는 항상 있다)
	constexpr uint8 MaskAll     = 0xFF;

	// 종류 비트 하나 + 그림자 비트. 광선 포함 마스크는 OR 판정(인스턴스 마스크 & 광선 마스크 ≠ 0)이라 "그림자 캐스터"와 "종류"를 따로 고른다
	inline uint8 GetInstanceMask(bool bSkinned, bool bFoliage, bool bTerrain, bool bCastShadow = true)
	{
		const uint8 Type = bTerrain ? MaskTerrain : (bSkinned ? MaskSkinned : (bFoliage ? MaskFoliage : MaskStatic));
		return static_cast<uint8>(Type | (bCastShadow ? MaskShadowCaster : 0));
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

	// 스킨 모델 BLAS(지오메트리 여러 개) 인스턴스 플래그: 불투명/Masked는 지오메트리별 OPAQUE 플래그가 정하므로
	//   Masked가 하나라도 있으면 FORCE_* 없음, 모두 불투명이면 FORCE_OPAQUE. 양면은 인스턴스 단위라 묶음 키로 나눈다
	inline uint32 ComputeSkinnedGroupInstanceFlags(bool bAnyMasked, bool bTwoSided)
	{
		return (bTwoSided ? InstanceFlagCullDisable : 0u) | (bAnyMasked ? 0u : InstanceFlagForceOpaque);
	}

	// ---- 스킨 BLAS 갱신 주기: 카메라에서 RefitDistance 안이면 매 프레임, 밖이면 거리에 따라 2, 3, … MaxInterval 프레임마다.
	//   묶음마다 고정 위상(Phase)으로 엇갈리게 갱신한다 (프레임 번호만의 결정적 함수 — 자동 검증 결정성).
	//   갱신하지 않는 프레임에는 스키닝도 건너뛰어 BLAS와 히트 정점이 같은 시점에 머문다
	inline uint32 GetSkinnedRefitInterval(float Distance, float RefitDistance, uint32 MaxInterval)
	{
		if (MaxInterval <= 1 || Distance <= RefitDistance || RefitDistance <= 0.0f)
		{
			return 1;
		}
		const float Steps = std::floor(Distance / RefitDistance);
		return std::min(MaxInterval, 1u + static_cast<uint32>(std::min(Steps, 1.0e6f)));
	}
	// 이번 프레임 갱신 여부: 위상이 맞는 프레임, 또는 주기가 바뀌어 마지막 갱신에서 Interval 이상 지났을 때
	inline bool ShouldRefitSkinned(uint64 FrameNumber, uint64 LastRefitFrame, uint32 Phase, uint32 Interval)
	{
		if (Interval <= 1)
		{
			return true;
		}
		return (FrameNumber + Phase) % Interval == 0 || FrameNumber >= LastRefitFrame + Interval;
	}
	inline uint32 ComputeRefitPhase(uint64 Key)
	{
		uint64 Hash = Key * 0x9E3779B97F4A7C15ull; // 피보나치 해시 (상위 비트)
		return static_cast<uint32>(Hash >> 40);
	}

	// ---- 범위 하위 할당 (스킨 정점 풀/스킨 BLAS 풀): 오프셋 순 빈 칸 목록에서 결정적 첫 맞춤, 해제 시 이웃과 병합.
	//   Grow는 기존 할당 위치를 유지하고 끝에 빈 칸을 늘린다
	class FRangeAllocator
	{
	public:
		static constexpr uint64 InvalidOffset = ~0ull;

		void Reset(uint64 InCapacity)
		{
			Capacity = InCapacity;
			FreeRanges.clear();
			if (InCapacity > 0)
			{
				FreeRanges.push_back({ 0, InCapacity });
			}
		}
		void Grow(uint64 NewCapacity)
		{
			if (NewCapacity <= Capacity)
			{
				return;
			}
			if (!FreeRanges.empty() && FreeRanges.back().Offset + FreeRanges.back().Size == Capacity)
			{
				FreeRanges.back().Size += NewCapacity - Capacity;
			}
			else
			{
				FreeRanges.push_back({ Capacity, NewCapacity - Capacity });
			}
			Capacity = NewCapacity;
		}
		uint64 Allocate(uint64 Size, uint64 Alignment)
		{
			Alignment = std::max<uint64>(Alignment, 1);
			for (size_t Index = 0; Index < FreeRanges.size(); ++Index)
			{
				const FRange Range = FreeRanges[Index];
				const uint64 Start = (Range.Offset + Alignment - 1) / Alignment * Alignment;
				const uint64 End   = Range.Offset + Range.Size;
				if (Size == 0 || Start + Size > End)
				{
					continue;
				}
				// 앞 여백 + 뒤 나머지를 빈 칸으로 남긴다
				FreeRanges.erase(FreeRanges.begin() + static_cast<std::ptrdiff_t>(Index));
				size_t Insert = Index;
				if (Start > Range.Offset)
				{
					FreeRanges.insert(FreeRanges.begin() + static_cast<std::ptrdiff_t>(Insert++), { Range.Offset, Start - Range.Offset });
				}
				if (Start + Size < End)
				{
					FreeRanges.insert(FreeRanges.begin() + static_cast<std::ptrdiff_t>(Insert), { Start + Size, End - (Start + Size) });
				}
				return Start;
			}
			return InvalidOffset;
		}
		void Release(uint64 Offset, uint64 Size)
		{
			if (Size == 0 || Offset == InvalidOffset)
			{
				return;
			}
			auto It = std::lower_bound(FreeRanges.begin(), FreeRanges.end(), Offset, [](const FRange& Range, uint64 Value) { return Range.Offset < Value; });
			It      = FreeRanges.insert(It, { Offset, Size });
			// 뒤와 병합
			if (auto Next = It + 1; Next != FreeRanges.end() && It->Offset + It->Size == Next->Offset)
			{
				It->Size += Next->Size;
				FreeRanges.erase(Next);
			}
			// 앞과 병합
			if (It != FreeRanges.begin())
			{
				auto Prev = It - 1;
				if (Prev->Offset + Prev->Size == It->Offset)
				{
					Prev->Size += It->Size;
					FreeRanges.erase(It);
				}
			}
		}
		uint64 GetCapacity() const { return Capacity; }
		uint64 GetFreeBytes() const
		{
			uint64 Total = 0;
			for (const FRange& Range : FreeRanges)
			{
				Total += Range.Size;
			}
			return Total;
		}
		size_t GetFreeRangeCount() const { return FreeRanges.size(); }

	private:
		struct FRange
		{
			uint64 Offset = 0;
			uint64 Size   = 0;
		};
		std::vector<FRange> FreeRanges; // 오프셋 순, 겹치지 않고 붙어 있지 않다
		uint64              Capacity = 0;
	};

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

	// ---- 지형 높이장 타일 BLAS
	// RT 정점 간격(셀): 1부터 2배씩, (셀/간격 + 1)² ≤ 상한이 될 때까지 (타일 크기를 넘지 않음 — 타일 크기는 2의 거듭제곱이라 나누어떨어진다)
	inline uint32 SelectTerrainStep(uint32 Cells, uint32 TileCells, uint32 MaxVertices)
	{
		uint32 Step = 1;
		while (Step < TileCells)
		{
			const uint64 Side = Cells / Step + 1;
			if (Side * Side <= MaxVertices)
			{
				break;
			}
			Step *= 2;
		}
		return Step;
	}
	// 바뀐 정점 사각형 한 축 [MinVertex, MaxVertex](양 끝 포함) → 다시 만들 타일 범위 [OutMin, OutMax].
	// 법선이 이웃 정점 중심 차분이라 1칸 넓히고, 경계 정점(타일 크기의 배수)은 양쪽 타일에 들어간다
	inline void GetDirtyTileRange(int32 MinVertex, int32 MaxVertex, uint32 TileCells, uint32 TilesPerSide, uint32& OutMin, uint32& OutMax)
	{
		const int32 Low  = std::max(MinVertex - 1, 0);
		const int32 High = std::max(MaxVertex + 1, 0);
		OutMin           = Low == 0 ? 0u : static_cast<uint32>(Low - 1) / TileCells;
		OutMax           = std::min(static_cast<uint32>(High) / TileCells, TilesPerSide - 1);
		OutMin           = std::min(OutMin, OutMax);
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

	// ---- RT 앰비언트 오클루전 (RTAO, RayTracedAmbientOcclusion.hlsl과 같은 식): DDGI 프로브(간격 수십 cm)가 못 담는 근거리 간접 가림.
	//   픽셀마다 광선 RaysPerPixel개 = 교차 표본 번호(GetInterleavedSampleIndex) + 16 × k 의 Vogel 원판 표본(전체 16 × 광선 수)을 반구로 올린
	//   코사인 가중 방향(Malley) → 가장 가까운 히트 거리 → 가림 = (1 - 거리/반경)^지수. 5x5 텐트 필터(가장자리 0.5)가 주기 4 패턴의 칸마다
	//   같은 가중을 주므로 평평한 면의 필터 결과는 프레임 회전과 무관하다 (결정적 — 시간 안정성 규칙)
	constexpr float  DefaultAoRadius        = 150.0f; // cm — DDGI 프로브 간격(Tests/GI·Apartment 100cm)의 1.5배: 경로 추적 기준 rmse 60cm 11.4 → 150cm 10.2
	constexpr uint32 DefaultAoRaysPerPixel  = 2;
	constexpr float  DefaultAoFalloffPower  = 2.0f;  // 큰 반경에서도 접촉부에 몰리게 (같은 반경 지수 1보다 기준에 가깝다)
	// 법선 N(정규화) 둘레 코사인 가중 반구 방향: 단위 원판 표본 Disk를 반구로 올린다 (원판 균등 → 반구 코사인 가중)
	inline FVector3 SampleCosineHemisphere(const FVector3& N, const FVector2& Disk)
	{
		const FVector3 Helper    = FMath::Abs(N.Z) < 0.999f ? FVector3(0.0f, 0.0f, 1.0f) : FVector3(1.0f, 0.0f, 0.0f);
		const FVector3 Tangent   = FVector3::Cross(Helper, N).GetNormalized();
		const FVector3 Bitangent = FVector3::Cross(N, Tangent);
		const float    Z         = std::sqrt(FMath::Clamp(1.0f - (Disk.X * Disk.X + Disk.Y * Disk.Y), 0.0f, 1.0f));
		return (Tangent * Disk.X + Bitangent * Disk.Y + N * Z).GetNormalized();
	}
	// 히트 거리 → 가림 [0, 1]: 빗나감(음수)·반경 밖 0, 접촉 1
	inline float ComputeAoOcclusion(float HitDistance, float Radius, float FalloffPower)
	{
		if (HitDistance < 0.0f || HitDistance >= Radius)
		{
			return 0.0f;
		}
		return std::pow(FMath::Clamp(1.0f - HitDistance / FMath::Max(Radius, 1.0e-3f), 0.0f, 1.0f), FalloffPower);
	}
	// 픽셀 Pixel의 k번째 광선 표본 번호 (전체 InterleavedSampleCount × RaysPerPixel 중)
	inline uint32 GetAoSampleIndex(uint32 X, uint32 Y, uint32 Frame, uint32 Ray)
	{
		return GetInterleavedSampleIndex(X, Y, Frame) + Ray * InterleavedSampleCount;
	}
	// 공간 필터 텐트 가중 (오프셋 -2~2): 가장자리 ±2는 0.5 — ±2는 주기 4로 같은 칸이라 합이 1
	inline float GetAoFilterTent(int32 Offset)
	{
		return (Offset == 2 || Offset == -2) ? 0.5f : 1.0f;
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
