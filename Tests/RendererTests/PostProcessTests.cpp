#include "Core/Testing/TestFramework.h"
#include "Renderer/PostProcess.h"
#include "Renderer/PostProcessMath.h"

#include <cmath>

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(PostProcess_BloomMipChain)
{
	// 1920x1080: 960x540, 480x270, 240x135, 120x67, 60x33, 30x16 → 최대 6단계
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipCount(1920, 1080), 6u);
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipDimension(1920, 0), 960u);
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipDimension(1080, 3), 67u);
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipDimension(1080, 5), 16u);

	// 작은 화면: 짧은 변이 4 미만이 되는 레벨은 만들지 않는다
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipCount(64, 32), 3u); // 32x16, 16x8, 8x4
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipCount(16, 16), 2u); // 8x8, 4x4
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipCount(7, 7), 0u);
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipCount(1, 1), 0u);

	// 크기는 최소 1 (시프트 초과 방어)
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipDimension(3, 10), 1u);
	E_EXPECT_EQ(FPostProcessMath::GetBloomMipDimension(100, 40), 1u);
}

E_TEST(PostProcess_AdaptationConverges)
{
	// 시간 0이면 변화 없음, 첫 프레임(이전 값 없음)은 즉시 목표
	E_EXPECT_NEAR(FPostProcessMath::ComputeAdaptedLuminance(0.5f, 2.0f, 0.0f, 1.5f), 0.5f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeAdaptedLuminance(0.0f, 2.0f, 0.016f, 1.5f), 2.0f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeAdaptedLuminance(NAN, 2.0f, 0.016f, 1.5f), 2.0f, Tol);

	// 매 프레임 적응: 단조롭게 접근하고 충분한 시간 후 수렴
	float Luminance = 0.1f;
	float Previous  = Luminance;
	for (int32 Frame = 0; Frame < 600; ++Frame) // 60fps x 10초
	{
		Luminance = FPostProcessMath::ComputeAdaptedLuminance(Luminance, 1.0f, 1.0f / 60.0f, 1.5f);
		E_EXPECT_TRUE(Luminance >= Previous && Luminance <= 1.0f);
		Previous = Luminance;
	}
	E_EXPECT_NEAR(Luminance, 1.0f, 1.0e-3f);

	// 프레임 분할과 무관: 한 번에 1초 = 60번에 나눠 1초
	float Split = 0.1f;
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Split = FPostProcessMath::ComputeAdaptedLuminance(Split, 1.0f, 1.0f / 60.0f, 1.5f);
	}
	const float Single = FPostProcessMath::ComputeAdaptedLuminance(0.1f, 1.0f, 1.0f, 1.5f);
	E_EXPECT_NEAR(Split, Single, 1.0e-4f);

	// 속도 0이면 멈춤
	E_EXPECT_NEAR(FPostProcessMath::ComputeAdaptedLuminance(0.3f, 1.0f, 1.0f, 0.0f), 0.3f, Tol);
}

E_TEST(PostProcess_AutoExposureEV)
{
	// 18% 회색이면 보정 없음, 두 배 밝으면 -1 스톱
	E_EXPECT_NEAR(FPostProcessMath::ComputeAutoExposureEV(0.18f, -10.0f, 10.0f), 0.0f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeAutoExposureEV(0.36f, -10.0f, 10.0f), -1.0f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeAutoExposureEV(0.09f, -10.0f, 10.0f), 1.0f, Tol);
	// 범위 제한, 0 휘도 방어
	E_EXPECT_NEAR(FPostProcessMath::ComputeAutoExposureEV(100.0f, -4.0f, 6.0f), -4.0f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeAutoExposureEV(0.0f, -4.0f, 6.0f), 6.0f, Tol);
}

E_TEST(PostProcess_BloomThresholdKnee)
{
	// 임계값 1, 니 0.5: 임계값 훨씬 아래는 0, 훨씬 위는 (B - T) / B, 부근은 부드럽게
	E_EXPECT_NEAR(FPostProcessMath::ComputeBloomContribution(0.2f, 1.0f, 0.5f), 0.0f, Tol);
	E_EXPECT_NEAR(FPostProcessMath::ComputeBloomContribution(4.0f, 1.0f, 0.5f), 0.75f, Tol);
	const float Near = FPostProcessMath::ComputeBloomContribution(1.0f, 1.0f, 0.5f);
	E_EXPECT_TRUE(Near > 0.0f && Near < 0.25f);

	// 밝기에 대해 단조 증가 (기여 절대량 = 배율 * 밝기)
	float Previous = 0.0f;
	for (float Brightness = 0.0f; Brightness < 5.0f; Brightness += 0.05f)
	{
		const float Amount = FPostProcessMath::ComputeBloomContribution(Brightness, 1.0f, 0.5f) * Brightness;
		E_EXPECT_TRUE(Amount + 1.0e-5f >= Previous);
		Previous = Amount;
	}

	// 니 0이면 경계가 날카롭다
	E_EXPECT_NEAR(FPostProcessMath::ComputeBloomContribution(0.99f, 1.0f, 0.0f), 0.0f, 1.0e-3f);
}

E_TEST(PostProcess_SettingsDefaults)
{
	const FPostProcessSettings Settings;
	E_EXPECT_TRUE(Settings.Tonemapper == ETonemapOperator::AcesFit);
	E_EXPECT_NEAR(Settings.ExposureEV, 0.0f, 0.0f);
	E_EXPECT_TRUE(Settings.bBloomEnabled);
	E_EXPECT_TRUE(Settings.BloomIntensity >= 0.05f && Settings.BloomIntensity <= 0.1f);
	E_EXPECT_TRUE(Settings.BloomThreshold > 0.0f);
	E_EXPECT_FALSE(Settings.bAutoExposure);
	E_EXPECT_TRUE(Settings.AutoExposureMinEV < Settings.AutoExposureMaxEV);
	E_EXPECT_TRUE(Settings.AdaptationSpeed > 0.0f);
}
