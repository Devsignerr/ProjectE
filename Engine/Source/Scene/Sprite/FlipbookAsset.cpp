#include "Scene/Sprite/FlipbookAsset.h"

#include "Scene/Sprite/Sprite2DJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace Sprite2DJson;

namespace
{
	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() &&
		       std::equal(A.begin(), A.end(), B.begin(), [](char L, char R) { return std::tolower(static_cast<unsigned char>(L)) == std::tolower(static_cast<unsigned char>(R)); });
	}

	// 한 주기 안 위치 수 / 위치 → 프레임 (PingPong: 0..N-1, N-2..1)
	size_t GetSequenceLength(size_t FrameCount, EFlipbookLoopMode Mode)
	{
		return (Mode == EFlipbookLoopMode::PingPong && FrameCount >= 2) ? 2 * FrameCount - 2 : FrameCount;
	}

	size_t GetFrameAt(size_t Position, size_t FrameCount, EFlipbookLoopMode Mode)
	{
		return (Mode == EFlipbookLoopMode::PingPong && Position >= FrameCount) ? 2 * FrameCount - 2 - Position : Position;
	}

	double PositiveMod(double Value, double Period)
	{
		double Result = std::fmod(Value, Period);
		if (Result < 0.0)
		{
			Result += Period;
		}
		return Result >= Period ? 0.0 : Result;
	}

	void EmitFrameEvents(size_t Frame, std::span<const FFlipbookEvent> Events, std::vector<int32>& Out)
	{
		for (size_t Index = 0; Index < Events.size(); ++Index)
		{
			if (Events[Index].Frame >= 0 && static_cast<size_t>(Events[Index].Frame) == Frame)
			{
				Out.push_back(static_cast<int32>(Index));
			}
		}
	}

	double SumDurations(std::span<const float> Durations)
	{
		double Total = 0.0;
		for (const float Duration : Durations)
		{
			Total += Duration;
		}
		return Total;
	}
} // namespace

const char* ToString(EFlipbookLoopMode Mode)
{
	switch (Mode)
	{
	case EFlipbookLoopMode::Once:     return "Once";
	case EFlipbookLoopMode::PingPong: return "PingPong";
	case EFlipbookLoopMode::Loop:
	default:                          return "Loop";
	}
}

EFlipbookLoopMode ParseFlipbookLoopMode(std::string_view Name, bool* bOutValid)
{
	EFlipbookLoopMode Mode   = EFlipbookLoopMode::Loop;
	bool              bValid = true;
	if (EqualsIgnoreCase(Name, "Once"))
	{
		Mode = EFlipbookLoopMode::Once;
	}
	else if (EqualsIgnoreCase(Name, "PingPong"))
	{
		Mode = EFlipbookLoopMode::PingPong;
	}
	else
	{
		bValid = EqualsIgnoreCase(Name, "Loop");
	}
	if (bOutValid != nullptr)
	{
		*bOutValid = bValid;
	}
	return Mode;
}

// ---- FFlipbookAsset --------------------------------------------------------------------------------------------------

float FFlipbookAsset::GetFrameDuration(size_t Index) const
{
	if (Index >= Frames.size())
	{
		return 0.0f;
	}
	const FFlipbookFrame& Frame    = Frames[Index];
	const float           Duration = Frame.Duration > 0.0f ? Frame.Duration : (Fps > 0.0f ? Frame.Scale / Fps : 0.0f);
	return std::max(Duration, MinFrameDuration);
}

void FFlipbookAsset::RebuildTimeline()
{
	FrameDurations.resize(Frames.size());
	double Total = 0.0;
	for (size_t Index = 0; Index < Frames.size(); ++Index)
	{
		FrameDurations[Index] = GetFrameDuration(Index);
		Total += FrameDurations[Index];
	}
	TotalDuration = static_cast<float>(Total);
}

std::string FFlipbookAsset::ToJsonString() const
{
	nlohmann::ordered_json Root;
	Root["Version"] = Version;
	Root["Sprite"]  = Sprite;
	Root["Fps"]     = Fps;
	Root["Loop"]    = ToString(Loop);
	nlohmann::ordered_json FrameArray = nlohmann::ordered_json::array();
	for (const FFlipbookFrame& Frame : Frames)
	{
		nlohmann::ordered_json Item;
		Item["Slice"] = Frame.Slice;
		if (Frame.Duration > 0.0f)
		{
			Item["Duration"] = Frame.Duration;
		}
		else if (Frame.Scale != 1.0f)
		{
			Item["Scale"] = Frame.Scale;
		}
		FrameArray.push_back(std::move(Item));
	}
	Root["Frames"] = std::move(FrameArray);
	nlohmann::ordered_json EventArray = nlohmann::ordered_json::array();
	for (const FFlipbookEvent& Event : Events)
	{
		EventArray.push_back({ { "Frame", Event.Frame }, { "Name", Event.Name } });
	}
	Root["Events"] = std::move(EventArray);
	return Root.dump(2);
}

bool FFlipbookAsset::FromJsonString(std::string_view Json, FFlipbookAsset& OutAsset, std::vector<std::string>* OutWarnings, std::string* OutError)
{
	FJson Root;
	if (!ParseRoot(Json, Version, "플립북(.eflipbook)", Root, OutWarnings, OutError))
	{
		return false;
	}
	FFlipbookAsset Asset;
	Asset.Sprite = GetString(Root, "Sprite");
	Asset.Fps    = GetFloat(Root, "Fps", 10.0f);
	if (!(Asset.Fps > 0.0f))
	{
		Warn(OutWarnings, std::format("Fps {}는 양수여야 합니다 (10 사용)", Asset.Fps));
		Asset.Fps = 10.0f;
	}
	if (const std::string LoopName = GetString(Root, "Loop"); !LoopName.empty())
	{
		bool bValid = true;
		Asset.Loop  = ParseFlipbookLoopMode(LoopName, &bValid);
		if (!bValid)
		{
			Warn(OutWarnings, std::format("모르는 Loop '{}' (Loop 사용)", LoopName));
		}
	}
	if (Asset.Sprite.empty())
	{
		Warn(OutWarnings, "Sprite가 비었습니다");
	}
	if (const FJson* FrameArray = GetArray(Root, "Frames"))
	{
		for (const FJson& Item : *FrameArray)
		{
			if (!Item.is_object())
			{
				Warn(OutWarnings, "Frames 항목이 객체가 아닙니다 (무시)");
				continue;
			}
			FFlipbookFrame Frame;
			Frame.Slice    = GetString(Item, "Slice");
			Frame.Duration = std::max(0.0f, GetFloat(Item, "Duration", 0.0f));
			Frame.Scale    = GetFloat(Item, "Scale", 1.0f);
			if (!(Frame.Scale > 0.0f))
			{
				Warn(OutWarnings, std::format("프레임 {} Scale {}는 양수여야 합니다 (1 사용)", Asset.Frames.size(), Frame.Scale));
				Frame.Scale = 1.0f;
			}
			if (Frame.Slice.empty())
			{
				Warn(OutWarnings, std::format("프레임 {}의 Slice가 비었습니다 (첫 슬라이스로 그림)", Asset.Frames.size()));
			}
			Asset.Frames.push_back(std::move(Frame));
		}
	}
	if (Asset.Frames.empty())
	{
		Warn(OutWarnings, "프레임이 없습니다");
	}
	if (const FJson* EventArray = GetArray(Root, "Events"))
	{
		for (const FJson& Item : *EventArray)
		{
			if (!Item.is_object())
			{
				continue;
			}
			FFlipbookEvent Event;
			Event.Frame = GetInt(Item, "Frame", 0);
			Event.Name  = GetString(Item, "Name");
			if (Event.Name.empty())
			{
				Warn(OutWarnings, "이름 없는 이벤트 (무시)");
				continue;
			}
			if (Event.Frame < 0 || static_cast<size_t>(Event.Frame) >= Asset.Frames.size())
			{
				Warn(OutWarnings, std::format("이벤트 '{}'의 프레임 {}이 범위 밖입니다 (발생하지 않음)", Event.Name, Event.Frame));
			}
			Asset.Events.push_back(std::move(Event));
		}
	}
	Asset.RebuildTimeline();
	OutAsset = std::move(Asset);
	return true;
}

// ---- FlipbookMath ----------------------------------------------------------------------------------------------------

float FlipbookMath::GetCycleDuration(std::span<const float> Durations, EFlipbookLoopMode Mode)
{
	const double Total = SumDurations(Durations);
	if (Mode == EFlipbookLoopMode::PingPong && Durations.size() >= 2)
	{
		return static_cast<float>(2.0 * Total - Durations.front() - Durations.back());
	}
	return static_cast<float>(Total);
}

FFlipbookSample FlipbookMath::Evaluate(float Time, std::span<const float> Durations, EFlipbookLoopMode Mode)
{
	FFlipbookSample Sample;
	const size_t    Count = Durations.size();
	if (Count == 0)
	{
		return Sample;
	}
	double Local = Time;
	if (Mode == EFlipbookLoopMode::Once)
	{
		const double Total = SumDurations(Durations);
		if (Local >= Total)
		{
			Sample.Frame     = static_cast<int32>(Count - 1);
			Sample.bFinished = true;
			return Sample;
		}
		if (Local < 0.0)
		{
			Sample.Frame = 0;
			return Sample;
		}
	}
	else
	{
		const double Cycle = GetCycleDuration(Durations, Mode);
		Local              = Cycle > 0.0 ? PositiveMod(Local, Cycle) : 0.0;
	}
	const size_t Length = GetSequenceLength(Count, Mode);
	double       Start  = 0.0;
	for (size_t Position = 0; Position < Length; ++Position)
	{
		const size_t Frame = GetFrameAt(Position, Count, Mode);
		if (Local < Start + Durations[Frame])
		{
			Sample.Frame = static_cast<int32>(Frame);
			return Sample;
		}
		Start += Durations[Frame];
	}
	Sample.Frame = static_cast<int32>(GetFrameAt(Length - 1, Count, Mode)); // 반올림으로 주기 끝에 닿은 경우
	return Sample;
}

void FlipbookMath::CollectEvents(float PrevTime, float NewTime, std::span<const float> Durations, EFlipbookLoopMode Mode, std::span<const FFlipbookEvent> Events,
                                 bool bIncludeStart, std::vector<int32>& OutEventIndices)
{
	const size_t Count = Durations.size();
	if (Count == 0 || Events.empty())
	{
		return;
	}
	const bool   bOnce  = Mode == EFlipbookLoopMode::Once;
	const double Cycle  = bOnce ? SumDurations(Durations) : static_cast<double>(GetCycleDuration(Durations, Mode));
	const size_t Length = GetSequenceLength(Count, Mode);
	if (!(Cycle > 0.0))
	{
		return;
	}
	double Prev = PrevTime;
	double New  = NewTime;
	if (bOnce)
	{
		// Once는 [0, T] 밖이 없다 (앞은 프레임 0, 뒤는 멈춘 마지막 프레임) — 잘라서 같은 규칙을 쓴다
		Prev = std::clamp(Prev, 0.0, Cycle);
		New  = std::clamp(New, 0.0, Cycle);
	}

	// 재생 시작: Prev가 속한 프레임에 들어간 것으로 본다 (그 뒤 경계 판정은 엄격 — 같은 구간을 두 번 세지 않는다)
	if (bIncludeStart)
	{
		const FFlipbookSample Start = Evaluate(static_cast<float>(Prev), Durations, Mode);
		if (Start.Frame >= 0)
		{
			EmitFrameEvents(static_cast<size_t>(Start.Frame), Events, OutEventIndices);
		}
	}
	if (New == Prev)
	{
		return;
	}

	if (New > Prev)
	{
		// 정방향: 구간 시작 s ∈ (Lo, Hi]
		double       Lo = Prev;
		const double Hi = New;
		if (!bOnce && Hi - Lo > Cycle)
		{
			const double Skip = std::floor((Hi - Lo) / Cycle) - 1.0;
			if (Skip > 0.0)
			{
				Lo += Skip * Cycle;
			}
		}
		const double FirstCycle = bOnce ? 0.0 : std::floor(Lo / Cycle);
		const double LastCycle  = bOnce ? 0.0 : std::floor(Hi / Cycle);
		for (double CycleIndex = FirstCycle; CycleIndex <= LastCycle; CycleIndex += 1.0)
		{
			double Start = CycleIndex * Cycle;
			for (size_t Position = 0; Position < Length; ++Position)
			{
				if (Start > Hi)
				{
					return;
				}
				const size_t Frame = GetFrameAt(Position, Count, Mode);
				if (Start > Lo)
				{
					EmitFrameEvents(Frame, Events, OutEventIndices);
				}
				Start += Durations[Frame];
			}
		}
		return;
	}

	// 역방향: 구간 끝 e ∈ [Lo, Hi), e 내림차순
	const double Lo = New;
	double       Hi = Prev;
	if (!bOnce && Hi - Lo > Cycle)
	{
		const double Skip = std::floor((Hi - Lo) / Cycle) - 1.0;
		if (Skip > 0.0)
		{
			Hi -= Skip * Cycle;
		}
	}
	const double FirstCycle = bOnce ? 0.0 : std::floor(Hi / Cycle);
	const double LastCycle  = bOnce ? 0.0 : std::floor(Lo / Cycle);
	for (double CycleIndex = FirstCycle; CycleIndex >= LastCycle; CycleIndex -= 1.0)
	{
		double End = (CycleIndex + 1.0) * Cycle;
		for (size_t Step = 0; Step < Length; ++Step)
		{
			if (End < Lo)
			{
				return;
			}
			const size_t Position = Length - 1 - Step;
			const size_t Frame    = GetFrameAt(Position, Count, Mode);
			if (End < Hi)
			{
				EmitFrameEvents(Frame, Events, OutEventIndices);
			}
			End -= Durations[Frame];
		}
	}
}

float FlipbookMath::WrapTime(float Time, std::span<const float> Durations, EFlipbookLoopMode Mode)
{
	if (Mode == EFlipbookLoopMode::Once || Durations.empty())
	{
		return Time;
	}
	const double Cycle = GetCycleDuration(Durations, Mode);
	return Cycle > 0.0 ? static_cast<float>(PositiveMod(Time, Cycle)) : 0.0f;
}
