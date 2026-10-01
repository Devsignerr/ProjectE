#include "Core/Testing/TestFramework.h"
#include "Renderer/AmbientOcclusionMath.h"

#include <cmath>

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(AmbientOcclusion_OpenHemisphereIsFullyVisible)
{
	// 정면을 보는 평면: 지평선 양쪽 -1(가림 없음), 법선 = 시선 → 가시도 1
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, -1.0f, 0.0f), 1.0f, Tol);
	// 기운 법선의 한 슬라이스 값은 cos n + n sin n (1보다 클 수 있다) — 투영 길이 가중으로 모든 방향을 평균하면 1
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, -1.0f, 0.6f), std::cos(0.6f) + 0.6f * std::sin(0.6f), 1.0e-3f);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, -1.0f, -0.9f), std::cos(0.9f) + 0.9f * std::sin(0.9f), 1.0e-3f);

	// 시선 V = +Z, 법선을 Alpha만큼 기울였을 때 슬라이스 방향 평균 (AmbientOcclusion.hlsl과 같은 투영)
	for (const float Alpha : { 0.3f, 0.8f, 1.2f })
	{
		const float N[3] = { std::sin(Alpha), 0.0f, std::cos(Alpha) };
		double      Sum  = 0.0;
		const int   Count = 512;
		for (int Index = 0; Index < Count; ++Index)
		{
			const float Phi     = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Count) * FMath::Pi;
			const float Dir[3]  = { std::cos(Phi), std::sin(Phi), 0.0f };
			const float Axis[3] = { Dir[1], -Dir[0], 0.0f }; // cross(Dir, V)
			const float DotNA   = N[0] * Axis[0] + N[1] * Axis[1];
			const float P[3]    = { N[0] - Axis[0] * DotNA, N[1] - Axis[1] * DotNA, N[2] };
			const float ProjLen = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
			const float CosN    = FMath::Clamp(P[2] / ProjLen, 0.0f, 1.0f);
			const float Angle   = ((P[0] * Dir[0] + P[1] * Dir[1]) >= 0.0f ? 1.0f : -1.0f) * std::acos(CosN);
			Sum += ProjLen * FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, -1.0f, Angle);
		}
		E_EXPECT_NEAR(static_cast<float>(Sum / Count), 1.0f, 0.02f);
	}
}

E_TEST(AmbientOcclusion_HorizonOccludes)
{
	// 한쪽 지평선이 시선 방향까지 올라오면(cos = 1, 벽이 바로 옆) 그쪽 절반이 막힌다
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, 1.0f, 0.0f), 0.5f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeSliceVisibility(1.0f, 1.0f, 0.0f), 0.0f, Tol);
	// 45도 지평선: 가시도 단조 감소
	const float Half = FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, std::cos(FMath::Pi * 0.25f), 0.0f);
	E_EXPECT_TRUE(Half > 0.5f && Half < 1.0f);
	const float More = FAmbientOcclusionMath::ComputeSliceVisibility(-1.0f, std::cos(FMath::Pi * 0.1f), 0.0f);
	E_EXPECT_TRUE(More < Half);
}

E_TEST(AmbientOcclusion_FalloffAndRadius)
{
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeFalloff(0.0f, 80.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeFalloff(80.0f * 80.0f, 80.0f), 0.0f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeFalloff(200.0f * 200.0f, 80.0f), 0.0f, Tol);

	// 원근: 깊이가 두 배면 픽셀 반경 절반, 직교: 깊이 무관
	const float Near = FAmbientOcclusionMath::GetRadiusPixels(80.0f, 500.0f, 1.732f, 720.0f, false);
	const float Far  = FAmbientOcclusionMath::GetRadiusPixels(80.0f, 1000.0f, 1.732f, 720.0f, false);
	E_EXPECT_NEAR(Near, 2.0f * Far, 1.0e-3f);
	E_EXPECT_NEAR(Near, 80.0f * 0.5f * 720.0f * 1.732f / 500.0f, 1.0e-2f);
	E_EXPECT_NEAR(FAmbientOcclusionMath::GetRadiusPixels(80.0f, 500.0f, 0.002f, 720.0f, true),
	              FAmbientOcclusionMath::GetRadiusPixels(80.0f, 5000.0f, 0.002f, 720.0f, true), Tol);
}

E_TEST(AmbientOcclusion_BilateralWeightAndIntensity)
{
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeDepthWeight(500.0f, 500.0f, 40.0f), 1.0f, Tol);
	// 같은 상대 차이 = 같은 가중 (깊이에 무관), 경계(10%)는 거의 0
	E_EXPECT_NEAR(FAmbientOcclusionMath::ComputeDepthWeight(500.0f, 505.0f, 40.0f), FAmbientOcclusionMath::ComputeDepthWeight(1000.0f, 1010.0f, 40.0f),
	              Tol);
	E_EXPECT_TRUE(FAmbientOcclusionMath::ComputeDepthWeight(500.0f, 550.0f, 40.0f) < 0.02f);

	E_EXPECT_NEAR(FAmbientOcclusionMath::ApplyIntensity(0.5f, 1.0f), 0.5f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ApplyIntensity(0.5f, 2.0f), 0.25f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ApplyIntensity(0.5f, 0.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FAmbientOcclusionMath::ApplyIntensity(1.0f, 3.0f), 1.0f, Tol);
}
