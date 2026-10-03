#include "Core/Testing/TestFramework.h"
#include "Renderer/RayTracingMath.h"
#include "Renderer/ReflectionMath.h"

#include <array>
#include <cmath>
#include <set>

// 레이 트레이싱 순수 계산 (Renderer/RayTracingMath.h, Phase 50) — RayTracingCommon.hlsli와 같은 식

E_TEST(RayTracing_InstanceMaskAndFlags)
{
	using namespace RayTracingMath;
	E_EXPECT_EQ(GetInstanceMask(false, false, false, false), MaskStatic);
	E_EXPECT_EQ(GetInstanceMask(true, false, false, false), MaskSkinned);
	E_EXPECT_EQ(GetInstanceMask(false, true, false, false), MaskFoliage);
	E_EXPECT_EQ(GetInstanceMask(false, false, true, false), MaskTerrain);
	E_EXPECT_EQ(GetInstanceMask(false, false, false), static_cast<uint8>(MaskStatic | MaskShadowCaster));
	// 그림자 광선(MaskShadowCaster)은 그림자를 끈 인스턴스를 빼고, 반사 광선(MaskTypes)은 모든 종류를 맞힌다
	E_EXPECT_EQ(GetInstanceMask(false, true, false, false) & MaskShadowCaster, 0);
	E_EXPECT_TRUE((GetInstanceMask(false, true, false, false) & MaskTypes) != 0);
	E_EXPECT_TRUE((GetInstanceMask(true, false, false, true) & MaskShadowCaster) != 0);
	// 마스크는 서로 겹치지 않는다 (광선 종류별 포함 마스크로 고를 수 있게)
	static_assert((MaskStatic & MaskSkinned) == 0 && (MaskFoliage & MaskTerrain) == 0 && ((MaskStatic | MaskSkinned) & (MaskFoliage | MaskTerrain)) == 0);

	// 불투명 = FORCE_OPAQUE, Masked = FORCE_NON_OPAQUE (후보 알파 테스트), 양면 = 컬링 끔, 반사 행렬 = 앞면 CCW
	E_EXPECT_EQ(ComputeInstanceFlags(false, false, false), InstanceFlagForceOpaque);
	E_EXPECT_EQ(ComputeInstanceFlags(true, false, false), InstanceFlagForceNonOpaque);
	E_EXPECT_EQ(ComputeInstanceFlags(false, true, true), InstanceFlagForceOpaque | InstanceFlagCullDisable | InstanceFlagFrontCounterClockwise);
	// D3D12_RAYTRACING_INSTANCE_FLAGS 값과 같아야 한다
	static_assert(InstanceFlagCullDisable == 0x1u && InstanceFlagFrontCounterClockwise == 0x2u && InstanceFlagForceOpaque == 0x4u &&
	              InstanceFlagForceNonOpaque == 0x8u);
}

E_TEST(RayTracing_HitGroupOffset)
{
	using namespace RayTracingMath;
	E_EXPECT_EQ(ComputeHitGroupOffset(0), 0u);
	E_EXPECT_EQ(ComputeHitGroupOffset(1), RayTypeCount);
	E_EXPECT_EQ(ComputeHitGroupOffset(5, 3), 15u);
	// 24비트를 넘으면 고정 PBR 슬롯으로
	E_EXPECT_EQ(ComputeHitGroupOffset(1u << 24), 0u);
	E_EXPECT_EQ(ComputeHitGroupOffset(MaxHitGroupOffset / RayTypeCount), (MaxHitGroupOffset / RayTypeCount) * RayTypeCount);
}

E_TEST(RayTracing_BlasKeyAndLifetime)
{
	using namespace RayTracingMath;
	const FBlasKey A{ 3, 1, 0 };
	const FBlasKey SameA{ 3, 1, 0 };
	const FBlasKey OtherLod{ 3, 1, 1 };
	const FBlasKey OtherGeneration{ 3, 2, 0 };
	E_EXPECT_TRUE(A == SameA);
	E_EXPECT_EQ(HashBlasKey(A), HashBlasKey(SameA));
	E_EXPECT_FALSE(A == OtherLod);
	E_EXPECT_TRUE(HashBlasKey(A) != HashBlasKey(OtherLod));
	E_EXPECT_TRUE(HashBlasKey(A) != HashBlasKey(OtherGeneration)); // 삭제 후 재사용된 메시 번호는 다른 BLAS

	E_EXPECT_EQ(ClampLod(2, 4), 2u);
	E_EXPECT_EQ(ClampLod(7, 4), 3u);
	E_EXPECT_EQ(ClampLod(1, 0), 0u);

	E_EXPECT_FALSE(ShouldEvictBlas(100, 100 + BlasEvictFrames));
	E_EXPECT_TRUE(ShouldEvictBlas(100, 101 + BlasEvictFrames));
	E_EXPECT_FALSE(ShouldEvictBlas(500, 10)); // 시계가 뒤로 가도 해제하지 않음
}

E_TEST(RayTracing_RayOriginOffset)
{
	using namespace RayTracingMath;
	// 법선 쪽으로 움직이고 (작은 거리), 원점 근처는 고정 소수 배율
	const FVector3 N(0.0f, 0.0f, 1.0f);
	for (const float Scale : { 0.01f, 1.0f, 100.0f, 10000.0f, 100000.0f })
	{
		const FVector3 P(Scale, -Scale, Scale);
		const FVector3 Offset = OffsetRayOrigin(P, N);
		E_EXPECT_EQ(Offset.X, P.X);      // 법선 성분 0인 축은 그대로
		E_EXPECT_EQ(Offset.Y, P.Y);
		E_EXPECT_TRUE(Offset.Z > P.Z);   // 법선 쪽으로
		E_EXPECT_TRUE(Offset.Z - P.Z < Scale * 1.0e-3f + 1.0e-3f); // 좌표 크기에 비례하는 작은 거리
	}
	// 음수 좌표·음의 법선도 법선 방향으로 (부호 처리)
	const FVector3 Down = OffsetRayOrigin(FVector3(0.0f, 0.0f, -500.0f), FVector3(0.0f, 0.0f, -1.0f));
	E_EXPECT_TRUE(Down.Z < -500.0f);
	const FVector3 Up = OffsetRayOrigin(FVector3(0.0f, 0.0f, -500.0f), FVector3(0.0f, 0.0f, 1.0f));
	E_EXPECT_TRUE(Up.Z > -500.0f);
	// 원점 근처: P + FloatScale × N
	E_EXPECT_NEAR(OffsetRayOrigin(FVector3(0.0f, 0.0f, 0.0f), N).Z, OffsetFloatScale, 1.0e-9f);

	// 1차 표면 바이어스: 깊이·비스듬함에 따라 커지고 수평 빛에서 두 배
	E_EXPECT_NEAR(ComputeSurfaceBias(0.0f, 1.0f), SurfaceBiasBase, 1.0e-6f);
	E_EXPECT_TRUE(ComputeSurfaceBias(1000.0f, 1.0f) > ComputeSurfaceBias(100.0f, 1.0f));
	E_EXPECT_NEAR(ComputeSurfaceBias(0.0f, 0.0f), SurfaceBiasBase * 2.0f, 1.0e-6f);
}

E_TEST(RayTracing_InterleavedSampleSequence)
{
	using namespace RayTracingMath;
	// 어느 4x4 창(위치 무관)에도 16개 표본 번호가 모두 있고, 프레임마다 모든 픽셀이 한 칸씩 돈다
	for (const uint32 Frame : { 0u, 1u, 7u, 15u, 16u, 1234u })
	{
		for (const std::array<uint32, 2> Origin : { std::array<uint32, 2>{ 0, 0 }, { 1, 2 }, { 3, 3 }, { 101, 57 } })
		{
			std::set<uint32> Seen;
			for (uint32 Y = 0; Y < 4; ++Y)
			{
				for (uint32 X = 0; X < 4; ++X)
				{
					Seen.insert(GetInterleavedSampleIndex(Origin[0] + X, Origin[1] + Y, Frame));
				}
			}
			E_EXPECT_EQ(Seen.size(), static_cast<size_t>(InterleavedSampleCount));
		}
	}
	E_EXPECT_EQ(GetInterleavedSampleIndex(5, 9, 3), (GetInterleavedSampleIndex(5, 9, 2) + 1) % InterleavedSampleCount);
	// 한 픽셀은 16프레임 동안 모든 표본을 한 번씩
	std::set<uint32> PixelSamples;
	for (uint32 Frame = 0; Frame < InterleavedSampleCount; ++Frame)
	{
		PixelSamples.insert(GetInterleavedSampleIndex(10, 20, Frame));
	}
	E_EXPECT_EQ(PixelSamples.size(), static_cast<size_t>(InterleavedSampleCount));
	// 이웃 픽셀은 다른 표본 (블루 노이즈 성질 — 가로/세로 이웃끼리 같은 번호 없음)
	for (uint32 Y = 0; Y < 8; ++Y)
	{
		for (uint32 X = 0; X < 8; ++X)
		{
			E_EXPECT_TRUE(GetBayer4x4(X, Y) != GetBayer4x4(X + 1, Y) && GetBayer4x4(X, Y) != GetBayer4x4(X, Y + 1));
		}
	}

	// 원판 표본: 단위 원판 안, 평균 ≈ 중심 (치우침 없음)
	FVector2 Mean;
	for (uint32 Index = 0; Index < InterleavedSampleCount; ++Index)
	{
		const FVector2 Sample = GetDiskSample(Index, InterleavedSampleCount);
		E_EXPECT_TRUE(Sample.X * Sample.X + Sample.Y * Sample.Y <= 1.0f + 1.0e-5f);
		Mean = Mean + Sample;
	}
	E_EXPECT_TRUE(std::fabs(Mean.X / 16.0f) < 0.1f && std::fabs(Mean.Y / 16.0f) < 0.1f);

	// 원뿔 방향: 축에서 반각 이내, 원판 중심 = 축
	const FVector3 Axis = FVector3(0.3f, -0.4f, 0.866f).GetNormalized();
	const float    Tan  = std::tan(0.5f * 0.53f * 3.14159265f / 180.0f);
	E_EXPECT_TRUE(SampleConeDirection(Axis, Tan, FVector2(0.0f, 0.0f)).Equals(Axis, 1.0e-5f));
	for (uint32 Index = 0; Index < InterleavedSampleCount; ++Index)
	{
		const FVector3 Direction = SampleConeDirection(Axis, Tan, GetDiskSample(Index, InterleavedSampleCount));
		E_EXPECT_TRUE(FVector3::Dot(Direction, Axis) >= std::cos(std::atan(Tan)) - 1.0e-5f);
		E_EXPECT_TRUE(Direction.IsNormalized());
	}
	// 축이 +Z여도 (도움 벡터 전환)
	E_EXPECT_TRUE(SampleConeDirection(FVector3(0.0f, 0.0f, 1.0f), Tan, FVector2(1.0f, 0.0f)).IsNormalized());
}

E_TEST(RayTracing_DenoiseRadius)
{
	using namespace RayTracingMath;
	// 반그림자: 차폐물 거리 비례, 최소/최대로 자름
	const float Tan = std::tan(0.5f * 3.14159265f / 180.0f);
	E_EXPECT_NEAR(ComputePenumbraRadiusPixels(0.0f, Tan, 1.0f, 2.0f, 16.0f), 2.0f, 1.0e-6f);
	E_EXPECT_NEAR(ComputePenumbraRadiusPixels(1000.0f, Tan, 1.0f, 2.0f, 16.0f), 1000.0f * Tan, 1.0e-4f);
	E_EXPECT_NEAR(ComputePenumbraRadiusPixels(1.0e6f, Tan, 1.0f, 2.0f, 16.0f), 16.0f, 1.0e-6f);
	E_EXPECT_TRUE(ComputePenumbraRadiusPixels(500.0f, Tan, 2.0f, 0.0f, 100.0f) > ComputePenumbraRadiusPixels(250.0f, Tan, 2.0f, 0.0f, 100.0f));

	// RT 반사 흐림은 SSR과 같은 식 (ReflectionMath — SsrResolve PSBlur 재사용): 거울 0, 거칠수록 크다
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrBlurRadiusPixels(0.0f, 500.0f, 1.0f, 16.0f), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(FReflectionMath::ComputeSsrBlurRadiusPixels(0.4f, 500.0f, 1.0f, 64.0f) >
	              FReflectionMath::ComputeSsrBlurRadiusPixels(0.2f, 500.0f, 1.0f, 64.0f));

	// 히트 텍스처 LOD: 원뿔이 넓을수록, 비스듬할수록 거친 밉
	const float Base = ComputeRayConeLod(1.0f, 1.0f, 1.0f, 1024.0f * 1024.0f, 10000.0f);
	E_EXPECT_TRUE(ComputeRayConeLod(4.0f, 1.0f, 1.0f, 1024.0f * 1024.0f, 10000.0f) > Base);
	E_EXPECT_TRUE(ComputeRayConeLod(1.0f, 0.25f, 1.0f, 1024.0f * 1024.0f, 10000.0f) > Base);
	E_EXPECT_NEAR(ComputeRayConeLod(4.0f, 1.0f, 1.0f, 1024.0f * 1024.0f, 10000.0f) - Base, 2.0f, 1.0e-4f);
	E_EXPECT_EQ(ComputeRayConeLod(1.0e-6f, 1.0f, 1.0f, 1.0f, 1.0e6f), 0.0f); // 음수 LOD는 0
	E_EXPECT_NEAR(ComputeReflectionConeWidth(100.0f, 200.0f, 0.001f, 0.01f), 100.0f * 0.001f + 200.0f * 0.011f, 1.0e-5f);
}

E_TEST(RayTracing_TerrainTiles)
{
	using namespace RayTracingMath;
	// 정점 간격: 상한 안이면 1, 넘으면 2배씩 (타일 크기 상한)
	E_EXPECT_EQ(SelectTerrainStep(256, 64, 131072), 1u);   // 257² = 66049
	E_EXPECT_EQ(SelectTerrainStep(1024, 64, 131072), 4u);  // 1025² → 513² = 263169 → 257² = 66049
	E_EXPECT_EQ(SelectTerrainStep(4096, 32, 1000), 32u);   // 타일 크기에서 멈춤
	E_EXPECT_EQ(SelectTerrainStep(1024, 64, 2000000), 1u);

	// 바뀐 정점 → 타일 (법선 1칸 + 경계 정점은 양쪽 타일)
	uint32 Min = 0;
	uint32 Max = 0;
	GetDirtyTileRange(10, 20, 64, 16, Min, Max);
	E_EXPECT_TRUE(Min == 0 && Max == 0);
	GetDirtyTileRange(64, 64, 64, 16, Min, Max); // 경계 정점 → 타일 0과 1
	E_EXPECT_TRUE(Min == 0 && Max == 1);
	GetDirtyTileRange(66, 70, 64, 16, Min, Max); // 법선 이웃 65 → 타일 1만
	E_EXPECT_TRUE(Min == 1 && Max == 1);
	GetDirtyTileRange(65, 70, 64, 16, Min, Max); // 이웃 64(경계) → 타일 0도
	E_EXPECT_TRUE(Min == 0 && Max == 1);
	GetDirtyTileRange(1000, 1024, 64, 16, Min, Max); // 끝은 마지막 타일로 자름
	E_EXPECT_TRUE(Min == 15 && Max == 15);
	GetDirtyTileRange(0, 1024, 64, 16, Min, Max); // 전체
	E_EXPECT_TRUE(Min == 0 && Max == 15);
}
E_TEST(RayTracing_AmbientOcclusionSampling)
{
	using namespace RayTracingMath;
	// 픽셀당 광선 2개: 어느 4x4 창에도 표본 32개(16 × 2)가 모두 있다 (프레임 회전과 무관)
	constexpr uint32 Rays = 2;
	for (const uint32 Frame : { 0u, 5u, 31u })
	{
		std::set<uint32> Seen;
		for (uint32 Y = 0; Y < 4; ++Y)
		{
			for (uint32 X = 0; X < 4; ++X)
			{
				for (uint32 Ray = 0; Ray < Rays; ++Ray)
				{
					Seen.insert(GetAoSampleIndex(7 + X, 3 + Y, Frame, Ray));
				}
			}
		}
		E_EXPECT_EQ(Seen.size(), static_cast<size_t>(InterleavedSampleCount * Rays));
	}

	// 코사인 가중 반구: 모든 방향이 법선 쪽, 원판 반경 = sinθ라 θ > 60°(r² > 0.75) 비율이 정확히 25% (코사인 가중 CDF sin²θ)
	const FVector3 N     = FVector3(0.2f, -0.5f, 0.84f).GetNormalized();
	const uint32   Count = InterleavedSampleCount * Rays;
	uint32         Grazing = 0;
	float          MeanCos = 0.0f;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const FVector3 Direction = SampleCosineHemisphere(N, GetDiskSample(Index, Count));
		const float    Cos       = FVector3::Dot(Direction, N);
		E_EXPECT_TRUE(Cos > 0.0f);
		E_EXPECT_NEAR(Direction.Length(), 1.0f, 1.0e-4f);
		Grazing += Cos < 0.5f ? 1u : 0u;
		MeanCos += Cos;
	}
	E_EXPECT_EQ(Grazing, Count / 4);
	E_EXPECT_NEAR(MeanCos / static_cast<float>(Count), 2.0f / 3.0f, 0.02f); // 코사인 가중 E[cosθ] = 2/3
	E_EXPECT_TRUE(SampleCosineHemisphere(N, FVector2(0.0f, 0.0f)).Equals(N, 1.0e-5f));

	// 가림 감쇠: 접촉 1, 반경에서 0, 빗나감 0, 지수가 클수록 먼 히트가 덜 가린다
	E_EXPECT_NEAR(ComputeAoOcclusion(0.0f, 60.0f, 1.0f), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeAoOcclusion(30.0f, 60.0f, 1.0f), 0.5f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeAoOcclusion(60.0f, 60.0f, 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeAoOcclusion(-1.0f, 60.0f, 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(ComputeAoOcclusion(30.0f, 60.0f, 2.0f) < ComputeAoOcclusion(30.0f, 60.0f, 1.0f));

	// 5x5 텐트: 주기 4 패턴의 칸(오프셋 mod 4)마다 가중 합 1 → 평평한 면 필터 결과가 프레임 회전과 무관 (결정적)
	float ClassWeight[4][4] = {};
	for (int32 Y = -2; Y <= 2; ++Y)
	{
		for (int32 X = -2; X <= 2; ++X)
		{
			ClassWeight[(Y + 4) % 4][(X + 4) % 4] += GetAoFilterTent(X) * GetAoFilterTent(Y);
		}
	}
	for (const auto& Row : ClassWeight)
	{
		for (const float Weight : Row)
		{
			E_EXPECT_NEAR(Weight, 1.0f, 1.0e-6f);
		}
	}
}

E_TEST(RayTracing_SkinnedGroupFlagsAndRefit)
{
	using namespace RayTracingMath;
	// 모델 BLAS: 모두 불투명이면 FORCE_OPAQUE, Masked가 섞이면 지오메트리 OPAQUE 플래그에 맡긴다 (FORCE_* 없음)
	E_EXPECT_EQ(ComputeSkinnedGroupInstanceFlags(false, false), InstanceFlagForceOpaque);
	E_EXPECT_EQ(ComputeSkinnedGroupInstanceFlags(true, false), 0u);
	E_EXPECT_EQ(ComputeSkinnedGroupInstanceFlags(true, true), InstanceFlagCullDisable);

	// 갱신 주기: 가까우면 1, 멀수록 늘고 상한에서 멈춘다
	E_EXPECT_EQ(GetSkinnedRefitInterval(500.0f, 1500.0f, 4), 1u);
	E_EXPECT_EQ(GetSkinnedRefitInterval(1500.0f, 1500.0f, 4), 1u);
	E_EXPECT_EQ(GetSkinnedRefitInterval(2000.0f, 1500.0f, 4), 2u);
	E_EXPECT_EQ(GetSkinnedRefitInterval(3500.0f, 1500.0f, 4), 3u);
	E_EXPECT_EQ(GetSkinnedRefitInterval(100000.0f, 1500.0f, 4), 4u);
	E_EXPECT_EQ(GetSkinnedRefitInterval(100000.0f, 1500.0f, 1), 1u); // 1 = 모두 매 프레임
	E_EXPECT_EQ(GetSkinnedRefitInterval(100000.0f, 0.0f, 4), 1u);

	// 고정 주기에서는 정확히 Interval 프레임마다 한 번, 위상이 다른 모델은 다른 프레임에
	for (const uint32 Interval : { 1u, 2u, 3u, 4u })
	{
		for (const uint32 Phase : { 0u, 1u, 7u, 12345u })
		{
			uint64 Last   = 0;
			uint32 Refits = 0;
			for (uint64 Frame = 1; Frame <= 120; ++Frame)
			{
				if (ShouldRefitSkinned(Frame, Last, Phase, Interval))
				{
					E_EXPECT_TRUE(Last == 0 || Frame - Last <= Interval); // 간격이 주기를 넘지 않는다
					Last = Frame;
					++Refits;
				}
			}
			E_EXPECT_EQ(Refits, 120u / Interval);
		}
	}
	// 주기가 바뀌어 위상이 어긋나도 Interval 프레임 안에 갱신
	E_EXPECT_TRUE(ShouldRefitSkinned(110, 106, 1, 4));
	// 위상: 같은 키 → 같은 값 (결정적)
	E_EXPECT_EQ(ComputeRefitPhase(42), ComputeRefitPhase(42));
	std::set<uint32> PhasesMod4;
	for (uint64 Key = 1; Key <= 64; ++Key)
	{
		PhasesMod4.insert(ComputeRefitPhase(Key << 2) % 4u);
	}
	E_EXPECT_EQ(PhasesMod4.size(), size_t(4)); // 연속 키도 고르게 엇갈린다
}

E_TEST(RayTracing_RangeAllocator)
{
	using namespace RayTracingMath;
	FRangeAllocator Allocator;
	Allocator.Reset(1024);
	const uint64 A = Allocator.Allocate(256, 256);
	const uint64 B = Allocator.Allocate(256, 256);
	const uint64 C = Allocator.Allocate(256, 256);
	E_EXPECT_EQ(A, 0ull);
	E_EXPECT_EQ(B, 256ull);
	E_EXPECT_EQ(C, 512ull);
	E_EXPECT_EQ(Allocator.Allocate(512, 256), FRangeAllocator::InvalidOffset); // 남은 256으로는 부족
	// 가운데를 풀면 첫 맞춤으로 그 자리를 다시 쓴다
	Allocator.Release(B, 256);
	E_EXPECT_EQ(Allocator.Allocate(128, 1), 256ull);
	E_EXPECT_EQ(Allocator.Allocate(128, 256), 768ull); // 정렬 맞는 다음 빈 칸
	E_EXPECT_EQ(Allocator.GetFreeBytes(), 256ull); // 정렬 여백 [384, 512) + 끝 [896, 1024)
	// 해제 병합: 모두 풀면 빈 칸 하나
	Allocator.Release(256, 128);
	Allocator.Release(A, 256);
	Allocator.Release(C, 256);
	Allocator.Release(768, 128);
	E_EXPECT_EQ(Allocator.GetFreeRangeCount(), size_t(1));
	E_EXPECT_EQ(Allocator.GetFreeBytes(), 1024ull);
	// 확장: 기존 할당 위치 유지, 끝 빈 칸과 합쳐진다
	const uint64 D = Allocator.Allocate(1024, 256);
	E_EXPECT_EQ(D, 0ull);
	E_EXPECT_EQ(Allocator.Allocate(256, 256), FRangeAllocator::InvalidOffset);
	Allocator.Grow(2048);
	E_EXPECT_EQ(Allocator.GetCapacity(), 2048ull);
	E_EXPECT_EQ(Allocator.Allocate(1024, 256), 1024ull);
	Allocator.Release(512, 256); // 가운데 구멍 + 끝은 꽉 참
	Allocator.Grow(4096);
	E_EXPECT_EQ(Allocator.GetFreeRangeCount(), size_t(2));
	E_EXPECT_EQ(Allocator.Allocate(512, 256), 2048ull); // 구멍(256)에는 안 들어간다
	E_EXPECT_EQ(Allocator.Allocate(0, 256), FRangeAllocator::InvalidOffset);
}
