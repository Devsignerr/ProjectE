#include "Scene/AnimNotify.h"

#include "Core/Math/Math.h"

#include <algorithm>

namespace
{
	bool IsIdentifierChar(char Char, bool bFirst)
	{
		const bool bLetter = (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || Char == '_';
		return bLetter || (!bFirst && Char >= '0' && Char <= '9');
	}

	struct FTimedHit
	{
		float          Time     = 0.0f;
		int32          Priority = 0; // 같은 시각: End → Notify → Begin
		FAnimNotifyHit Hit;
	};

	int32 PriorityOf(EAnimNotifyEventType Type)
	{
		switch (Type)
		{
		case EAnimNotifyEventType::StateEnd:   return 0;
		case EAnimNotifyEventType::Notify:     return 1;
		default:                               return 2;
		}
	}
} // namespace

namespace AnimNotifyMath
{
	bool IsValidName(const std::string& Name)
	{
		if (Name.empty())
		{
			return false;
		}
		for (size_t Index = 0; Index < Name.size(); ++Index)
		{
			if (!IsIdentifierChar(Name[Index], Index == 0))
			{
				return false;
			}
		}
		return true;
	}

	std::string MakeValidName(const std::string& Name)
	{
		std::string Result;
		for (const char Char : Name)
		{
			Result.push_back(IsIdentifierChar(Char, false) ? Char : '_');
		}
		if (Result.empty())
		{
			return "Notify";
		}
		if (!IsIdentifierChar(Result[0], true))
		{
			Result.insert(Result.begin(), '_');
		}
		return Result;
	}

	void Collect(const std::vector<FAnimNotify>& Notifies, float PreviousTime, float NewTime, float Delta, float Duration, bool bLoop, bool bWrapped,
	             bool bResync, std::vector<uint8>& InOutActive, std::vector<FAnimNotifyHit>& OutHits)
	{
		InOutActive.resize(Notifies.size(), 0);
		if (Delta == 0.0f || Duration <= FMath::SmallNumber)
		{
			return;
		}

		// 역재생은 시간축을 뒤집어(D - t) 정방향과 같은 규칙으로 판정한다
		const bool bBackward = Delta < 0.0f;
		const auto Map       = [&](float Time) { return FMath::Clamp(bBackward ? Duration - Time : Time, 0.0f, Duration); };

		struct FSegment
		{
			float A             = 0.0f;
			float B             = 0.0f;
			bool  bInclusiveEnd = false; // 끝 시각(= Duration)의 노티파이 포함
		};
		FSegment Segments[2];
		int32    SegmentCount = 0;
		const float Start = Map(PreviousTime);
		const float End   = Map(NewTime);
		if (bWrapped)
		{
			Segments[SegmentCount++] = { Start, Duration, true };
			Segments[SegmentCount++] = { 0.0f, End, false };
		}
		else
		{
			Segments[SegmentCount++] = { Start, End, !bLoop && End >= Duration };
		}

		std::vector<FTimedHit> Timed;
		for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
		{
			const FSegment& Segment = Segments[SegmentIndex];
			const size_t    First   = Timed.size();
			if (bResync && SegmentIndex == 0)
			{
				// 스크럽 직후: 시작 시각을 포함한 스테이트는 여기서 들어간 것으로 본다
				for (size_t Index = 0; Index < Notifies.size(); ++Index)
				{
					const FAnimNotify& Notify = Notifies[Index];
					if (Notify.Kind != EAnimNotifyKind::State || InOutActive[Index] || Notify.Duration <= 0.0f)
					{
						continue;
					}
					const float S = bBackward ? Map(Notify.GetEndTime()) : Map(Notify.Time);
					const float E = bBackward ? Map(Notify.Time) : Map(Notify.GetEndTime());
					if (S <= Segment.A && Segment.A < E)
					{
						Timed.push_back({ Segment.A, 2, { static_cast<int32>(Index), EAnimNotifyEventType::StateBegin } });
						InOutActive[Index] = 1;
					}
				}
			}
			if (Segment.B <= Segment.A)
			{
				continue; // 진행하지 않은 구간 (반복 없는 클립이 끝에 멈춘 상태 등)
			}
			for (size_t Index = 0; Index < Notifies.size(); ++Index)
			{
				const FAnimNotify& Notify = Notifies[Index];
				const int32        Slot   = static_cast<int32>(Index);
				if (Notify.Kind == EAnimNotifyKind::Notify)
				{
					const float T = Map(Notify.Time);
					if (T >= Segment.A && (T < Segment.B || (Segment.bInclusiveEnd && T <= Segment.B)))
					{
						Timed.push_back({ T, 1, { Slot, EAnimNotifyEventType::Notify } });
					}
					continue;
				}
				if (Notify.Duration <= 0.0f)
				{
					continue;
				}
				const float S = bBackward ? Map(Notify.GetEndTime()) : Map(Notify.Time);
				const float E = bBackward ? Map(Notify.Time) : Map(Notify.GetEndTime());
				if (InOutActive[Index])
				{
					if (E <= Segment.B)
					{
						Timed.push_back({ E, 0, { Slot, EAnimNotifyEventType::StateEnd } });
						InOutActive[Index] = 0;
					}
				}
				else if (S >= Segment.A && S < Segment.B)
				{
					Timed.push_back({ S, 2, { Slot, EAnimNotifyEventType::StateBegin } });
					if (E <= Segment.B)
					{
						Timed.push_back({ E, 0, { Slot, EAnimNotifyEventType::StateEnd } }); // 한 프레임 안에 들어갔다 나옴
					}
					else
					{
						InOutActive[Index] = 1;
					}
				}
			}
			// 구간 안에서 시간 순서 (같은 시각이면 End → 노티파이 → Begin)
			std::stable_sort(Timed.begin() + static_cast<std::ptrdiff_t>(First), Timed.end(), [](const FTimedHit& L, const FTimedHit& R) {
				return L.Time != R.Time ? L.Time < R.Time : L.Priority < R.Priority;
			});
		}

		for (const FTimedHit& Hit : Timed)
		{
			OutHits.push_back(Hit.Hit);
		}
		for (size_t Index = 0; Index < Notifies.size(); ++Index)
		{
			if (InOutActive[Index])
			{
				OutHits.push_back({ static_cast<int32>(Index), EAnimNotifyEventType::StateTick });
			}
		}
	}

	void EndAll(std::vector<uint8>& InOutActive, std::vector<FAnimNotifyHit>& OutHits)
	{
		for (size_t Index = 0; Index < InOutActive.size(); ++Index)
		{
			if (InOutActive[Index])
			{
				OutHits.push_back({ static_cast<int32>(Index), EAnimNotifyEventType::StateEnd });
				InOutActive[Index] = 0;
			}
		}
	}
} // namespace AnimNotifyMath
