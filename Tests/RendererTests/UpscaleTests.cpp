#include "Core/GameUserSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/DynamicResolution.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/TemporalMath.h"
#include "Renderer/UpscaleMath.h"

#include <cmath>
#include <cstddef>

// TAAU / 동적 해상도 순수 식 (Phase 48)

E_TEST(Upscale_PresetsAndInternalDimension)
{
	E_EXPECT_NEAR(FUpscaleMath::GetPresetScreenPercentage(static_cast<int32>(EResolutionQuality::Native)), 100.0f, 1.0e-4f);
	E_EXPECT_NEAR(FUpscaleMath::GetPresetScreenPercentage(static_cast<int32>(EResolutionQuality::Quality)), 77.0f, 1.0e-4f);
	E_EXPECT_NEAR(FUpscaleMath::GetPresetScreenPercentage(static_cast<int32>(EResolutionQuality::Balanced)), 67.0f, 1.0e-4f);
	E_EXPECT_NEAR(FUpscaleMath::GetPresetScreenPercentage(static_cast<int32>(EResolutionQuality::Performance)), 50.0f, 1.0e-4f);

	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1920, 100.0f), 1920u);
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1920, 50.0f), 960u);
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1080, 67.0f), 724u); // 723.6 반올림
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(720, 77.0f), 554u);  // 554.4
	// 범위: 25~100%, 1 이상, 출력 이하
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1000, 10.0f), 250u);
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1000, 150.0f), 1000u);
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(1, 25.0f), 1u);
	E_EXPECT_EQ(FUpscaleMath::ComputeInternalDimension(0, 50.0f), 0u);
	E_EXPECT_NEAR(FUpscaleMath::ClampScreenPercentage(std::nanf("")), 100.0f, 1.0e-4f);

	E_EXPECT_NEAR(FUpscaleMath::QuantizePercentage(73.0f, 5.0f), 75.0f, 1.0e-4f);
	E_EXPECT_NEAR(FUpscaleMath::QuantizePercentage(72.4f, 5.0f), 70.0f, 1.0e-4f);
	E_EXPECT_NEAR(FUpscaleMath::QuantizePercentage(72.4f, 0.0f), 72.4f, 1.0e-4f);
}

E_TEST(Upscale_MipBias)
{
	// 네이티브(같거나 큰 내부)는 항상 0 — 기존 화면과 비트 동일
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(1080, 1080, -0.3f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(1200, 1080, -0.3f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(0, 1080, -0.3f), 0.0f, 1.0e-6f);
	// 50% → log2(0.5) = -1 (+ 보정)
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(540, 1080, 0.0f), -1.0f, 1.0e-5f);
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(540, 1080, -0.3f), -1.3f, 1.0e-5f);
	E_EXPECT_NEAR(FUpscaleMath::ComputeMipBias(724, 1080, 0.0f), std::log2(724.0f / 1080.0f), 1.0e-5f);
	// 비율이 작을수록 더 음수
	E_EXPECT_TRUE(FUpscaleMath::ComputeMipBias(540, 1080, 0.0f) < FUpscaleMath::ComputeMipBias(832, 1080, 0.0f));

	// 상수 버퍼: 바이어스는 PerFrame 끝 새 행 (Mesh.hlsl cbuffer PerFrame과 1:1)
	static_assert(offsetof(FPerFrameConstants, MaterialMipBias) == 304);
	static_assert(sizeof(FPerFrameConstants) == 320);
}

E_TEST(Upscale_JitterSampleCount)
{
	E_EXPECT_EQ(FUpscaleMath::GetJitterSampleCount(1080, 1080), FTemporalMath::JitterSampleCount); // 네이티브 = 기존 8
	E_EXPECT_EQ(FUpscaleMath::GetJitterSampleCount(540, 1080), 32u);                               // 8 × 2²
	E_EXPECT_EQ(FUpscaleMath::GetJitterSampleCount(724, 1080), 18u);                               // ceil(8 × 2.2253)
	E_EXPECT_EQ(FUpscaleMath::GetJitterSampleCount(100, 1080), 64u);                               // 상한
	E_EXPECT_EQ(FUpscaleMath::GetJitterSampleCount(0, 1080), 8u);
}

E_TEST(Upscale_JitterUvAndInputPosition)
{
	// 지터된 투영에서 지터 없는 UV의 점은 UV + JitterUv에 보인다 (NDC +Y 위 ↔ UV +Y 아래)
	const FVector2 JitterNdc = FTemporalMath::JitterPixelsToNdc(FVector2(0.25f, -0.25f), 960, 540);
	const FVector2 JitterUv  = FUpscaleMath::JitterNdcToUv(JitterNdc);
	E_EXPECT_NEAR(JitterUv.X, 0.25f / 960.0f, 1.0e-7f);
	E_EXPECT_NEAR(JitterUv.Y, -0.25f / 540.0f, 1.0e-7f);
	// NDC 오프셋 → UV 오프셋이 같은 점을 가리키는지 (NdcToUv 차이)
	const FVector2 Base    = FTemporalMath::NdcToUv(FVector2(0.1f, 0.2f));
	const FVector2 Shifted = FTemporalMath::NdcToUv(FVector2(0.1f + JitterNdc.X, 0.2f + JitterNdc.Y));
	E_EXPECT_NEAR(Shifted.X - Base.X, JitterUv.X, 1.0e-6f);
	E_EXPECT_NEAR(Shifted.Y - Base.Y, JitterUv.Y, 1.0e-6f);

	// 지터 0, 50%: 출력 픽셀 (1, 1) 중심 → 내부 연속 좌표 (1.5/2 × 2 = ...) = 출력 중심 UV × 내부 크기
	const FVector2 Uv(1.5f / 1920.0f, 1.5f / 1080.0f);
	const FVector2 Position = FUpscaleMath::GetInputPosition(Uv, FVector2::ZeroVector, 960, 540);
	E_EXPECT_NEAR(Position.X, 0.75f, 1.0e-5f);
	E_EXPECT_NEAR(Position.Y, 0.75f, 1.0e-5f);
}

// 지터 한 바퀴 동안 출력 픽셀마다 가까운(출력 픽셀 단위 거리) 표본이 오는가 — TAAU 수렴의 전제
E_TEST(Upscale_JitterCoversOutputPixels)
{
	for (const float Percentage : { 77.0f, 67.0f, 50.0f })
	{
		const uint32 Output   = 1080;
		const uint32 Internal = FUpscaleMath::ComputeInternalDimension(Output, Percentage);
		const uint32 Count    = FUpscaleMath::GetJitterSampleCount(Internal, Output);
		const float  Scale    = static_cast<float>(Output) / static_cast<float>(Internal);
		float        WorstBest = 0.0f;
		// 출력 픽셀 몇 개(서로 다른 위상)에 대해: 표본 위치(내부 픽셀 중심 - 지터) 중 가장 가까운 거리
		for (uint32 OutputPixel = 0; OutputPixel < 12; ++OutputPixel)
		{
			const float Center = (static_cast<float>(OutputPixel) + 0.5f) / Scale; // 내부 픽셀 단위
			float       Best   = 1.0e9f;
			for (uint32 Frame = 0; Frame < Count; ++Frame)
			{
				const FVector2 Jitter = FTemporalMath::GetJitterPixels(Frame, Count);
				for (int32 Pixel = -2; Pixel <= 16; ++Pixel)
				{
					// 지터된 영상의 내부 픽셀 Pixel 중심이 지터 없는 좌표에서 놓인 곳
					const float Sample = static_cast<float>(Pixel) + 0.5f - Jitter.X;
					Best               = std::fmin(Best, std::fabs(Sample - Center) * Scale);
				}
			}
			WorstBest = std::fmax(WorstBest, Best);
		}
		E_EXPECT_TRUE(WorstBest < 0.3f); // 1차원: 한 바퀴에 출력 픽셀 중심 0.3픽셀 안 표본
		E_EXPECT_TRUE(FUpscaleMath::SampleWeight(WorstBest * WorstBest) > 0.8f);
	}
	E_EXPECT_NEAR(FUpscaleMath::SampleWeight(0.0f), 1.0f, 1.0e-6f);
	E_EXPECT_TRUE(FUpscaleMath::SampleWeight(1.0f) < 0.11f);
}

E_TEST(DynamicResolution_HoldsWithinBand)
{
	FDynamicResolutionSettings Settings;
	Settings.TargetGpuMs = 10.0f;
	FDynamicResolutionController Controller;
	Controller.Reset(100.0f);
	// 목표의 85~100% 사이(히스테리시스 띠)는 계속 유지
	for (int32 Frame = 0; Frame < 300; ++Frame)
	{
		E_EXPECT_NEAR(Controller.Update(9.2f, Settings), 100.0f, 1.0e-4f);
	}
	E_EXPECT_EQ(Controller.GetChangeCount(), 0u);
	// 측정 없음(0)은 무시
	E_EXPECT_NEAR(Controller.Update(0.0f, Settings), 100.0f, 1.0e-4f);
}

E_TEST(DynamicResolution_DecreasesInStepsAndConverges)
{
	FDynamicResolutionSettings Settings;
	Settings.TargetGpuMs = 10.0f;
	FDynamicResolutionController Controller;
	Controller.Reset(100.0f);
	// GPU 비용 ∝ 비율²인 가짜 GPU: 100%에서 20ms → 목표 10ms면 약 70%
	float  Percentage   = 100.0f;
	uint32 LastChange   = 0;
	uint32 ChangeFrames = 0;
	for (uint32 Frame = 0; Frame < 600; ++Frame)
	{
		const float Ratio = Percentage / 100.0f;
		const float Next  = Controller.Update(20.0f * Ratio * Ratio, Settings);
		if (Next != Percentage)
		{
			// 단계(5%) 단위, 바꾼 간격 >= 최소 대기(급할 때 1/3)
			E_EXPECT_NEAR(std::fmod(Next, Settings.StepPercentage), 0.0f, 1.0e-3f);
			if (ChangeFrames > 0)
			{
				E_EXPECT_TRUE(Frame - LastChange >= Settings.MinFramesBetweenChanges / 3);
			}
			LastChange = Frame;
			++ChangeFrames;
		}
		Percentage = Next;
	}
	const float FinalMs = 20.0f * (Percentage / 100.0f) * (Percentage / 100.0f);
	E_EXPECT_TRUE(FinalMs <= Settings.TargetGpuMs);
	E_EXPECT_TRUE(FinalMs >= Settings.TargetGpuMs * Settings.IncreaseThreshold * 0.9f);
	E_EXPECT_TRUE(Percentage >= 60.0f && Percentage <= 70.0f);
	E_EXPECT_TRUE(ChangeFrames <= 4); // 진동하지 않는다
}

E_TEST(DynamicResolution_IncreasesWhenUnderBudgetAndClamps)
{
	FDynamicResolutionSettings Settings;
	Settings.TargetGpuMs   = 16.0f;
	Settings.MinPercentage = 50.0f;
	Settings.MaxPercentage = 90.0f;
	FDynamicResolutionController Controller;
	Controller.Reset(50.0f);
	float Percentage = 50.0f;
	for (uint32 Frame = 0; Frame < 600; ++Frame)
	{
		Percentage = Controller.Update(2.0f, Settings); // 매우 가벼움 → 최대까지
	}
	E_EXPECT_NEAR(Percentage, 90.0f, 1.0e-4f);
	// 아주 무거우면 최소에서 멈춘다
	for (uint32 Frame = 0; Frame < 600; ++Frame)
	{
		Percentage = Controller.Update(100.0f, Settings);
	}
	E_EXPECT_NEAR(Percentage, 50.0f, 1.0e-4f);
	// 원하는 비율 식: 2배 무거우면 1/√2
	E_EXPECT_NEAR(FDynamicResolutionController::ComputeDesiredPercentage(100.0f, 20.0f, 10.0f, 1.0f), 100.0f / std::sqrt(2.0f), 1.0e-3f);
}

E_TEST(GameUserSettings_ResolutionJson)
{
	FGameUserSettings Settings;
	E_EXPECT_TRUE(Settings.ResolutionQuality == EResolutionQuality::Native);
	E_EXPECT_TRUE(Settings.ApplyJson(R"({ "ResolutionQuality": "balanced", "DynamicResolution": true, "DynamicResolutionTargetMs": 8.5 })"));
	E_EXPECT_TRUE(Settings.ResolutionQuality == EResolutionQuality::Balanced);
	E_EXPECT_TRUE(Settings.bDynamicResolution);
	E_EXPECT_NEAR(Settings.DynamicResolutionTargetMs, 8.5f, 1.0e-5f);

	FGameUserSettings RoundTrip;
	E_EXPECT_TRUE(RoundTrip.ApplyJson(Settings.ToJson()));
	E_EXPECT_TRUE(RoundTrip.ResolutionQuality == EResolutionQuality::Balanced);
	E_EXPECT_TRUE(RoundTrip.bDynamicResolution);
	// 모르는 값은 무시 (이전 값 유지)
	E_EXPECT_TRUE(RoundTrip.ApplyJson(R"({ "ResolutionQuality": "Ultra" })"));
	E_EXPECT_TRUE(RoundTrip.ResolutionQuality == EResolutionQuality::Balanced);
}
