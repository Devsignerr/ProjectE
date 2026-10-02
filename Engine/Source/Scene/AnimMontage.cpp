#include "Scene/AnimMontage.h"

#include "Core/Math/Math.h"
#include "Scene/Animation.h"

#include <cmath>

namespace AnimMontageMath
{
	float GetEndTime(const FAnimMontageInstance& Montage)
	{
		const float End = Montage.Params.EndTime;
		return End < 0.0f || End > Montage.Duration ? Montage.Duration : End;
	}

	float GetStartTime(const FAnimMontageInstance& Montage)
	{
		return FMath::Clamp(Montage.Params.StartTime, 0.0f, GetEndTime(Montage));
	}

	void Start(FAnimMontageInstance& Montage)
	{
		Montage.Time             = Montage.Params.Speed >= 0.0f ? GetStartTime(Montage) : GetEndTime(Montage);
		Montage.Elapsed          = 0.0f;
		Montage.BlendOutElapsed  = -1.0f;
		Montage.BlendOutDuration = 0.0f;
		Montage.Weight           = Montage.Params.BlendIn > 0.0f ? 0.0f : 1.0f;
		Montage.bInterrupted     = false;
		Montage.bFinished        = false;
		Montage.Notify.Reset();
		Montage.Notify.bResync = true; // 시작 시각이 스테이트 안이면 Begin
	}

	void BeginBlendOut(FAnimMontageInstance& Montage, float BlendOut, bool bInterrupted)
	{
		if (Montage.bFinished)
		{
			return;
		}
		Montage.bInterrupted   = Montage.bInterrupted || bInterrupted;
		const float Duration   = FMath::Max(BlendOut < 0.0f ? Montage.Params.BlendOut : BlendOut, 0.0f);
		if (Montage.BlendOutElapsed >= 0.0f && Duration >= Montage.BlendOutDuration - Montage.BlendOutElapsed)
		{
			return; // 이미 더 빨리 끝나게 빠지는 중
		}
		Montage.BlendOutStartWeight = Montage.Weight;
		Montage.BlendOutElapsed     = 0.0f;
		Montage.BlendOutDuration    = Duration;
		if (Duration <= 0.0f)
		{
			Montage.Weight    = 0.0f;
			Montage.bFinished = true;
		}
	}

	FMontageStep Advance(FAnimMontageInstance& Montage, float DeltaSeconds)
	{
		FMontageStep Step;
		Step.PreviousTime = Montage.Time;
		Step.NewTime      = Montage.Time;
		if (Montage.bFinished)
		{
			return Step;
		}
		const float RealDelta = std::fabs(DeltaSeconds);
		const float TimeDelta = DeltaSeconds * Montage.Params.Speed;
		const float Start     = GetStartTime(Montage);
		const float End       = GetEndTime(Montage);
		const float Length    = End - Start;

		// 클립 시각
		float Next = Montage.Time + TimeDelta;
		if (Montage.Params.bLoop && Length > FMath::SmallNumber)
		{
			if (Next >= End || Next < Start)
			{
				Step.bWrapped = TimeDelta != 0.0f;
				Next          = Start + std::fmod(Next - Start, Length);
				if (Next < Start)
				{
					Next += Length;
				}
			}
			Step.Delta = TimeDelta;
		}
		else
		{
			Next       = FMath::Clamp(Next, Start, End);
			Step.Delta = Next - Montage.Time;
		}
		Montage.Time = Next;
		Step.NewTime = Next;

		// 가중치: 들어오기 → (빠지는 중이면) 빠지기
		Montage.Elapsed += RealDelta;
		const bool bWasBlendingOut = Montage.BlendOutElapsed >= 0.0f;
		if (bWasBlendingOut)
		{
			Montage.BlendOutElapsed += RealDelta;
		}
		else
		{
			Montage.Weight = AnimationMath::ComputeCrossfadeWeight(Montage.Elapsed, Montage.Params.BlendIn);
			// 반복이 아니면 남은 재생 시간이 BlendOut 이하가 되는 순간 저절로 빠지기 시작 (구간 끝에서 0)
			const float Speed = std::fabs(Montage.Params.Speed);
			if (!Montage.Params.bLoop && Speed > FMath::SmallNumber)
			{
				const float Remaining = (Montage.Params.Speed >= 0.0f ? End - Montage.Time : Montage.Time - Start) / Speed;
				if (Remaining <= Montage.Params.BlendOut + 1.0e-5f)
				{
					BeginBlendOut(Montage, Remaining, false);
				}
			}
		}
		if (!Montage.bFinished && Montage.BlendOutElapsed >= 0.0f)
		{
			const float Alpha = AnimationMath::ComputeCrossfadeWeight(Montage.BlendOutElapsed, Montage.BlendOutDuration);
			Montage.Weight    = Montage.BlendOutStartWeight * (1.0f - Alpha);
			if (Montage.BlendOutElapsed >= Montage.BlendOutDuration)
			{
				Montage.Weight    = 0.0f;
				Montage.bFinished = true;
			}
		}
		return Step;
	}
} // namespace AnimMontageMath
