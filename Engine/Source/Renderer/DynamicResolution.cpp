#include "Renderer/DynamicResolution.h"

#include "Core/Math/Math.h"
#include "Renderer/UpscaleMath.h"

#include <cmath>

void FDynamicResolutionController::Reset(float InPercentage)
{
	Percentage        = InPercentage;
	SmoothedMs        = 0.0f;
	Samples           = 0;
	FramesSinceChange = 0;
}

float FDynamicResolutionController::ComputeDesiredPercentage(float Current, float InSmoothedMs, float TargetMs, float Headroom)
{
	if (InSmoothedMs <= 0.0f || TargetMs <= 0.0f)
	{
		return Current;
	}
	return Current * std::sqrt(TargetMs * Headroom / InSmoothedMs);
}

float FDynamicResolutionController::Update(float GpuMs, const FDynamicResolutionSettings& Settings)
{
	const float MinPercent = FUpscaleMath::ClampScreenPercentage(FMath::Min(Settings.MinPercentage, Settings.MaxPercentage));
	const float MaxPercent = FUpscaleMath::ClampScreenPercentage(FMath::Max(Settings.MinPercentage, Settings.MaxPercentage));
	Percentage             = FMath::Clamp(Percentage, MinPercent, MaxPercent);
	++FramesSinceChange;
	if (GpuMs <= 0.0f || Settings.TargetGpuMs <= 0.0f)
	{
		return Percentage;
	}

	SmoothedMs = Samples == 0 ? GpuMs : FMath::Lerp(SmoothedMs, GpuMs, FMath::Clamp(Settings.Smoothing, 0.01f, 1.0f));
	++Samples;

	const float  Ratio      = SmoothedMs / Settings.TargetGpuMs;
	const bool   bOver      = Ratio > Settings.DecreaseThreshold;
	const bool   bUnder     = Ratio < Settings.IncreaseThreshold;
	const uint32 WaitFrames = Ratio > Settings.UrgentThreshold ? FMath::Max(Settings.MinFramesBetweenChanges / 3, 1u) : Settings.MinFramesBetweenChanges;
	if ((!bOver && !bUnder) || FramesSinceChange < WaitFrames || Samples < 4)
	{
		return Percentage;
	}

	float Desired = ComputeDesiredPercentage(Percentage, SmoothedMs, Settings.TargetGpuMs, Settings.Headroom);
	Desired       = FMath::Clamp(FUpscaleMath::QuantizePercentage(Desired, Settings.StepPercentage), MinPercent, MaxPercent);
	// 방향이 맞는 변화만 (줄여야 하는데 반올림으로 그대로/늘어나는 경우는 한 단계 강제)
	if (bOver && Desired >= Percentage)
	{
		Desired = FMath::Max(MinPercent, Percentage - FMath::Max(Settings.StepPercentage, 0.0f));
	}
	if (bUnder && Desired <= Percentage)
	{
		return Percentage; // 늘릴 여유가 한 단계에 못 미친다
	}
	if (Desired == Percentage)
	{
		return Percentage;
	}

	const float Scale = Desired / Percentage;
	SmoothedMs *= Scale * Scale; // 예측치로 옮긴다
	Percentage        = Desired;
	FramesSinceChange = 0;
	++ChangeCount;
	return Percentage;
}
