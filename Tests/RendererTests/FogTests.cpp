#include "Core/Testing/TestFramework.h"
#include "Renderer/FogMath.h"

#include <cmath>

namespace
{
	// 광선을 잘게 나눠 밀도를 더한 수치 적분 (닫힌 식과 비교용)
	float NumericOpticalDepth(float Density, float Falloff, float BaseHeight, float OriginZ, float DirZ, float Start, float End)
	{
		const int   Steps = 20000;
		const float Step  = (End - Start) / static_cast<float>(Steps);
		double      Sum   = 0.0;
		for (int Index = 0; Index < Steps; ++Index)
		{
			const float T = Start + (static_cast<float>(Index) + 0.5f) * Step;
			Sum += Density * std::exp(-Falloff * (OriginZ + DirZ * T - BaseHeight)) * Step;
		}
		return static_cast<float>(Sum);
	}
} // namespace

E_TEST(Fog_HeightOpticalDepthMatchesNumeric)
{
	const float Density = 0.02f / 100.0f; // 1/cm
	const float Falloff = 0.2f / 100.0f;
	struct FCase
	{
		float OriginZ, DirZ, Start, End;
	};
	const FCase Cases[] = {
		{ 200.0f, -0.3f, 0.0f, 3000.0f }, { 200.0f, 0.5f, 100.0f, 5000.0f }, { 0.0f, 0.0f, 0.0f, 10000.0f },
		{ 1000.0f, -0.9f, 500.0f, 1100.0f }, { 50.0f, 1.0e-6f, 0.0f, 8000.0f },
	};
	for (const FCase& Case : Cases)
	{
		const float Closed  = FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 0.0f, Case.OriginZ, Case.DirZ, Case.Start, Case.End);
		const float Numeric = NumericOpticalDepth(Density, Falloff, 0.0f, Case.OriginZ, Case.DirZ, Case.Start, Case.End);
		E_EXPECT_NEAR(Closed, Numeric, Numeric * 2.0e-3f + 1.0e-6f);
	}
	// 수평 광선 극한: 밀도 × 길이
	E_EXPECT_NEAR(FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 100.0f, 100.0f, 0.0f, 0.0f, 5000.0f), Density * 5000.0f, 1.0e-6f);
	// 시작 거리 = 구간 적분의 차
	const float Whole = FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 0.0f, 300.0f, -0.2f, 0.0f, 4000.0f);
	const float Front = FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 0.0f, 300.0f, -0.2f, 0.0f, 1000.0f);
	const float Back  = FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 0.0f, 300.0f, -0.2f, 1000.0f, 4000.0f);
	E_EXPECT_NEAR(Whole, Front + Back, Whole * 1.0e-4f);
	// 빈 구간/밀도 0
	E_EXPECT_NEAR(FFogMath::ComputeHeightFogOpticalDepth(Density, Falloff, 0.0f, 0.0f, 0.1f, 500.0f, 400.0f), 0.0f, 1.0e-9f);
	E_EXPECT_NEAR(FFogMath::ComputeHeightFogOpticalDepth(0.0f, Falloff, 0.0f, 0.0f, 0.1f, 0.0f, 400.0f), 0.0f, 1.0e-9f);
}

E_TEST(Fog_AmountAndTransmittance)
{
	// 멀수록 단조 증가, 최대 불투명도로 제한
	float Previous = 0.0f;
	for (float Distance = 500.0f; Distance <= 20000.0f; Distance += 500.0f)
	{
		const float Depth  = FFogMath::ComputeHeightFogOpticalDepth(0.0002f, 0.002f, 0.0f, 100.0f, 0.0f, 0.0f, Distance);
		const float Amount = FFogMath::ComputeFogAmount(Depth, 1.0f);
		E_EXPECT_TRUE(Amount >= Previous);
		Previous = Amount;
	}
	E_EXPECT_NEAR(FFogMath::ComputeFogAmount(100.0f, 0.6f), 0.6f, 1.0e-5f);
	E_EXPECT_NEAR(FFogMath::ComputeFogAmount(0.0f, 1.0f), 0.0f, 1.0e-6f);
	// 위를 향한 무한 광선은 수렴 (높이 감쇠): 길이를 늘려도 거의 같다
	const float Long   = FFogMath::ComputeHeightFogOpticalDepth(0.0002f, 0.002f, 0.0f, 0.0f, 0.7f, 0.0f, 100000.0f);
	const float Longer = FFogMath::ComputeHeightFogOpticalDepth(0.0002f, 0.002f, 0.0f, 0.0f, 0.7f, 0.0f, 1000000.0f);
	E_EXPECT_NEAR(Long, Longer, Longer * 1.0e-3f);
}

E_TEST(Fog_DirectionalAndPhase)
{
	const FVector3 Light(1.0f, 0.0f, 0.0f); // +X로 진행
	E_EXPECT_NEAR(FFogMath::ComputeDirectionalInscattering(FVector3(-1.0f, 0.0f, 0.0f), Light, 8.0f), 1.0f, 1.0e-5f); // 태양을 봄
	E_EXPECT_NEAR(FFogMath::ComputeDirectionalInscattering(FVector3(1.0f, 0.0f, 0.0f), Light, 8.0f), 0.0f, 1.0e-5f);
	E_EXPECT_NEAR(FFogMath::ComputeDirectionalInscattering(FVector3(0.0f, 1.0f, 0.0f), Light, 8.0f), 0.0f, 1.0e-5f);

	// HG: g = 0이면 1/(4π), 구 적분 = 1, g > 0이면 앞쪽(CosTheta = 1)이 크다
	E_EXPECT_NEAR(FFogMath::HenyeyGreenstein(0.0f, 0.3f), 1.0f / (4.0f * FMath::Pi), 1.0e-5f);
	for (const float G : { 0.0f, 0.4f, -0.6f, 0.8f })
	{
		double    Sum   = 0.0;
		const int Steps = 4000;
		for (int Index = 0; Index < Steps; ++Index)
		{
			const float Theta = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Steps) * FMath::Pi;
			Sum += FFogMath::HenyeyGreenstein(G, std::cos(Theta)) * 2.0 * FMath::Pi * std::sin(Theta) * (FMath::Pi / Steps);
		}
		E_EXPECT_NEAR(static_cast<float>(Sum), 1.0f, 0.01f);
	}
	E_EXPECT_TRUE(FFogMath::HenyeyGreenstein(0.5f, 1.0f) > FFogMath::HenyeyGreenstein(0.5f, -1.0f));
}

E_TEST(Fog_VolumeSlicesAndIntegration)
{
	// 조각 깊이 ↔ 좌표 왕복, 가까울수록 촘촘
	for (const float W : { 0.0f, 0.1f, 0.5f, 0.9f, 1.0f })
	{
		E_EXPECT_NEAR(FFogMath::DepthToSlice(FFogMath::SliceToDepth(W, 6000.0f), 6000.0f), W, 1.0e-4f);
	}
	E_EXPECT_TRUE(FFogMath::SliceToDepth(0.1f, 6000.0f) - FFogMath::SliceToDepth(0.0f, 6000.0f) <
	              FFogMath::SliceToDepth(1.0f, 6000.0f) - FFogMath::SliceToDepth(0.9f, 6000.0f));
	E_EXPECT_EQ(FFogMath::GetVolumeDimension(1280), 160u);
	E_EXPECT_EQ(FFogMath::GetVolumeDimension(721), 91u);

	// 고른 매질: 조각 적분 누적 = 해석해 (투과율 e^{-σL}, 산란 S/σ (1 - e^{-σL}))
	const float    Sigma = 0.0005f;
	const FVector3 S(0.0002f, 0.0003f, 0.0004f);
	FFogMath::FIntegration Accumulated;
	const int      Slices = 64;
	const float    Length = 4000.0f;
	for (int Index = 0; Index < Slices; ++Index)
	{
		Accumulated = FFogMath::IntegrateSlice(Accumulated, S, Sigma, Length / Slices);
	}
	const float ExpectedT = std::exp(-Sigma * Length);
	E_EXPECT_NEAR(Accumulated.Transmittance, ExpectedT, 1.0e-4f);
	E_EXPECT_NEAR(Accumulated.Scattering.Y, S.Y / Sigma * (1.0f - ExpectedT), 1.0e-3f);
	// 소멸 0: 산란 = S × 길이
	FFogMath::FIntegration Empty = FFogMath::IntegrateSlice(FFogMath::FIntegration{}, S, 0.0f, 100.0f);
	E_EXPECT_NEAR(Empty.Scattering.X, S.X * 100.0f, 1.0e-5f);
	E_EXPECT_NEAR(Empty.Transmittance, 1.0f, 1.0e-6f);
}
