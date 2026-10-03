#include "Core/Testing/TestFramework.h"
#include "Renderer/CloudMath.h"
#include "Renderer/HdrDisplayMath.h"
#include "Renderer/WaterMath.h"

#include <cmath>

// Phase 49: 구름(CloudMath.h ↔ VolumetricClouds.hlsl), 물(WaterMath.h ↔ Water.hlsl), HDR 출력(HdrDisplayMath.h ↔ HdrDisplay.hlsli) 순수 식

E_TEST(Cloud_HeightGradientShape)
{
	E_EXPECT_NEAR(FCloudMath::HeightGradient(0.0f, 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FCloudMath::HeightGradient(1.0f, 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FCloudMath::HeightGradient(0.4f, 1.0f), 1.0f, 1.0e-6f);
	// 층운(0)은 낮게만: 높이 0.35 위로는 0
	E_EXPECT_NEAR(FCloudMath::HeightGradient(0.35f, 0.0f), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(FCloudMath::HeightGradient(0.12f, 0.0f) > 0.9f);
	for (int32 Index = 0; Index <= 20; ++Index)
	{
		const float Value = FCloudMath::HeightGradient(static_cast<float>(Index) / 20.0f, 0.6f);
		E_EXPECT_TRUE(Value >= 0.0f && Value <= 1.0f);
	}
}

E_TEST(Cloud_CoverageMonotonic)
{
	// 덮임 0 = 구름 없음, 덮임이 늘면 밀도가 줄지 않는다
	E_EXPECT_NEAR(FCloudMath::BaseDensity(0.9f, 0.8f, 1.0f, 0.0f), 0.0f, 1.0e-6f);
	float Previous = 0.0f;
	for (int32 Index = 0; Index <= 10; ++Index)
	{
		const float Coverage = static_cast<float>(Index) / 10.0f;
		const float Density  = FCloudMath::BaseDensity(0.7f, 0.6f, 0.9f, Coverage);
		E_EXPECT_TRUE(Density + 1.0e-6f >= Previous);
		E_EXPECT_TRUE(Density >= 0.0f && Density <= 1.0f);
		Previous = Density;
	}
	E_EXPECT_TRUE(Previous > 0.3f);
	// 깎기: 세부가 0이면 그대로, 밀도가 낮은 가장자리는 많이 깎인다
	E_EXPECT_NEAR(FCloudMath::ErodeDensity(0.5f, 0.0f, 0.0f, 0.35f), 0.5f, 1.0e-5f);
	E_EXPECT_TRUE(FCloudMath::ErodeDensity(0.1f, 0.8f, 0.5f, 0.35f) < 0.1f);
}

E_TEST(Cloud_PhaseAndLighting)
{
	// 헤니-그린스타인 구면 적분 = 1
	for (const float G : { 0.0f, 0.6f, -0.3f })
	{
		double     Sum   = 0.0;
		const int  Steps = 4000;
		for (int Index = 0; Index < Steps; ++Index)
		{
			const float CosTheta = -1.0f + 2.0f * (static_cast<float>(Index) + 0.5f) / Steps;
			Sum += FCloudMath::HenyeyGreenstein(CosTheta, G) * 2.0 * FMath::Pi * (2.0 / Steps);
		}
		E_EXPECT_NEAR(static_cast<float>(Sum), 1.0f, 2.0e-3f);
	}
	// 비어-파우더는 비어 법칙보다 크지 않고, 0 깊이에서 파우더 때문에 어둡다
	for (const float Depth : { 0.0f, 0.3f, 2.0f })
	{
		E_EXPECT_TRUE(FCloudMath::BeerPowder(Depth, 1.0f) <= std::exp(-Depth) + 1.0e-6f);
	}
	E_EXPECT_NEAR(FCloudMath::BeerPowder(0.0f, 0.0f), 1.0f, 1.0e-6f);
	// 다중 산란 옥타브: 한 옥타브면 비어 × 위상
	const float One = FCloudMath::MultiScatteringOctaves(1.5f, 0.3f, 0.6f, -0.2f, 0.3f, 1);
	E_EXPECT_NEAR(One, std::exp(-1.5f) * FCloudMath::DualLobePhase(0.3f, 0.6f, -0.2f, 0.3f), 1.0e-6f);
	E_EXPECT_TRUE(FCloudMath::MultiScatteringOctaves(1.5f, 0.3f, 0.6f, -0.2f, 0.3f, 3) > One);
}

E_TEST(Cloud_RayShellIntersection)
{
	const float Ground = 6360.0f;
	const float Inner  = Ground + 1.5f;
	const float Outer  = Ground + 4.0f;
	float       Start  = 0.0f;
	float       End    = 0.0f;
	// 지면에서 위로: 층 두께 그대로
	E_EXPECT_TRUE(FCloudMath::RayShellIntersection(Ground + 0.001f, 1.0f, Inner, Outer, Ground, Start, End));
	E_EXPECT_NEAR(Start, 1.499f, 1.0e-3f);
	E_EXPECT_NEAR(End, 3.999f, 1.0e-3f);
	// 지면에서 아래로: 없음
	E_EXPECT_FALSE(FCloudMath::RayShellIntersection(Ground + 0.001f, -0.5f, Inner, Outer, Ground, Start, End));
	// 층 안에서 위로: 0부터
	E_EXPECT_TRUE(FCloudMath::RayShellIntersection(Ground + 2.0f, 1.0f, Inner, Outer, Ground, Start, End));
	E_EXPECT_NEAR(Start, 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(End, 2.0f, 1.0e-3f);
	// 지평선 쪽은 훨씬 길다
	E_EXPECT_TRUE(FCloudMath::RayShellIntersection(Ground + 0.001f, 0.0f, Inner, Outer, Ground, Start, End));
	E_EXPECT_TRUE(End - Start > 30.0f);
}

E_TEST(Water_AbsorptionAndFresnel)
{
	const FVector3 Absorption(0.45f, 0.09f, 0.065f);
	const FVector3 Behind(0.8f, 0.7f, 0.6f);
	const FVector3 Scatter(0.02f, 0.09f, 0.1f);
	const FVector3 AtZero = FWaterMath::ApplyAbsorption(Behind, Scatter, Absorption, 0.0f);
	E_EXPECT_NEAR(AtZero.X, Behind.X, 1.0e-6f);
	// 100cm = 1m: exp(-a)
	const FVector3 T = FWaterMath::Transmittance(Absorption, 100.0f);
	E_EXPECT_NEAR(T.X, std::exp(-0.45f), 1.0e-6f);
	E_EXPECT_TRUE(T.X < T.Y && T.Y < T.Z); // 빨강이 먼저 사라진다
	// 아주 깊으면 산란 색
	const FVector3 Deep = FWaterMath::ApplyAbsorption(Behind, Scatter, Absorption, 1.0e6f);
	E_EXPECT_NEAR(Deep.Z, Scatter.Z, 1.0e-4f);
	// 프레넬: 수직 F0, 스치는 각 1
	E_EXPECT_NEAR(FWaterMath::FresnelSchlick(1.0f), FWaterMath::WaterF0, 1.0e-6f);
	E_EXPECT_NEAR(FWaterMath::FresnelSchlick(0.0f), 1.0f, 1.0e-6f);
	E_EXPECT_TRUE(FWaterMath::FresnelSchlick(0.3f) > FWaterMath::FresnelSchlick(0.7f));
	// 거품: 두께 0에서 1, FoamDistance 이상 0
	E_EXPECT_NEAR(FWaterMath::EdgeFoam(0.0f, 25.0f), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(FWaterMath::EdgeFoam(30.0f, 25.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FWaterMath::EdgeFoam(10.0f, 0.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FWaterMath::RefractionScale(0.0f, 100.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FWaterMath::RefractionScale(250.0f, 100.0f), 1.0f, 1.0e-6f);
}

E_TEST(Water_UnderwaterPathLength)
{
	const FVector3 Half(500.0f, 300.0f, 100.0f);
	// 상자 안 가운데에서 +X로, 장면이 멀면 벽까지 500
	E_EXPECT_NEAR(FWaterMath::UnderwaterPathLength(FVector3::ZeroVector, FVector3(1.0f, 0.0f, 0.0f), Half, 1.0e6f), 500.0f, 1.0e-3f);
	// 장면이 가까우면 장면까지
	E_EXPECT_NEAR(FWaterMath::UnderwaterPathLength(FVector3::ZeroVector, FVector3(1.0f, 0.0f, 0.0f), Half, 120.0f), 120.0f, 1.0e-3f);
	// 밖에서 들어오는 광선: 상자 안 구간만
	E_EXPECT_NEAR(FWaterMath::UnderwaterPathLength(FVector3(-1000.0f, 0.0f, 0.0f), FVector3(1.0f, 0.0f, 0.0f), Half, 1.0e6f), 1000.0f, 1.0e-2f);
	// 빗나감
	E_EXPECT_NEAR(FWaterMath::UnderwaterPathLength(FVector3(0.0f, 0.0f, 500.0f), FVector3(1.0f, 0.0f, 0.0f), Half, 1.0e6f), 0.0f, 1.0e-6f);
}

E_TEST(Hdr_PqRoundTripAndReferencePoints)
{
	for (const float Linear : { 0.0f, 1.0e-4f, 0.01f, 0.1f, 0.5f, 1.0f })
	{
		E_EXPECT_NEAR(FHdrDisplayMath::PqDecode(FHdrDisplayMath::PqEncode(Linear)), Linear, Linear * 1.0e-3f + 1.0e-6f);
	}
	E_EXPECT_NEAR(FHdrDisplayMath::PqEncode(1.0f), 1.0f, 1.0e-5f);
	// 100 nits ≈ 0.508 (ST.2084 표), 1000 nits ≈ 0.752
	E_EXPECT_NEAR(FHdrDisplayMath::PqEncode(100.0f / 10000.0f), 0.5081f, 2.0e-3f);
	E_EXPECT_NEAR(FHdrDisplayMath::PqEncode(1000.0f / 10000.0f), 0.7518f, 2.0e-3f);
	// 흰색은 BT.2020에서도 흰색
	const FVector3 White = FHdrDisplayMath::Rec709ToRec2020(FVector3::OneVector);
	E_EXPECT_NEAR(White.X, 1.0f, 1.0e-4f);
	E_EXPECT_NEAR(White.Y, 1.0f, 1.0e-4f);
	E_EXPECT_NEAR(White.Z, 1.0f, 1.0e-4f);
	// scRGB: 종이 흰색 200 nits = 2.5
	E_EXPECT_NEAR(FHdrDisplayMath::EncodeScRgb(FVector3::OneVector, 200.0f).Y, 2.5f, 1.0e-5f);
}

E_TEST(Hdr_HighlightExpansion)
{
	const float Peak = 4.0f; // 1000 nits / 250 nits
	// 무릎 아래는 SDR 값 그대로 (중간톤·UI 밝기 일치)
	E_EXPECT_NEAR(FHdrDisplayMath::ExpandHighlights(0.3f, Peak), 0.3f, 1.0e-6f);
	E_EXPECT_NEAR(FHdrDisplayMath::ExpandHighlights(0.5f, Peak), 0.5f, 1.0e-6f);
	// s = 1에서 최대 밝기, 무릎에서 기울기 1 (연속)
	E_EXPECT_NEAR(FHdrDisplayMath::ExpandHighlights(1.0f, Peak), Peak, 1.0e-3f);
	const float Epsilon = 1.0e-3f;
	const float Slope   = (FHdrDisplayMath::ExpandHighlights(0.5f + Epsilon, Peak) - 0.5f) / Epsilon;
	E_EXPECT_NEAR(Slope, 1.0f, 0.02f);
	// 단조 증가
	float Previous = 0.0f;
	for (int32 Index = 0; Index <= 100; ++Index)
	{
		const float Value = FHdrDisplayMath::ExpandHighlights(static_cast<float>(Index) / 100.0f, Peak);
		E_EXPECT_TRUE(Value >= Previous);
		Previous = Value;
	}
	// 최대 밝기가 종이 흰색 이하면 SDR 그대로
	E_EXPECT_NEAR(FHdrDisplayMath::ExpandHighlights(0.9f, 1.0f), 0.9f, 1.0e-6f);
}
