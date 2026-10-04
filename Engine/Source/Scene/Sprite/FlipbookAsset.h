#pragma once

#include "Core/CoreTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// 플립북 재생 방식 (JSON은 이름 — 끝에만 추가)
enum class EFlipbookLoopMode : int32
{
	Loop,     // 0 → N-1 → 0 → ...
	Once,     // 0 → N-1 에서 멈춤 (끝나면 bFinished)
	PingPong, // 0 → N-1 → N-2 → ... → 1 → 0 → 1 ... (양 끝 프레임은 반환점에서 한 번만)
};

const char*       ToString(EFlipbookLoopMode Mode);
EFlipbookLoopMode ParseFlipbookLoopMode(std::string_view Name, bool* bOutValid = nullptr); // 대소문자 무시, 모르면 Loop

struct FFlipbookFrame
{
	std::string Slice;           // .esprite 슬라이스 이름
	float       Duration = 0.0f; // 초. 0 이하 = 파일 Fps 기준 (Scale / Fps)
	float       Scale    = 1.0f; // Duration이 0 이하일 때 1/Fps에 곱하는 배율

	bool operator==(const FFlipbookFrame& Other) const = default;
};

// 노티파이: Frame 번째 프레임에 "들어갈 때" 한 번 (규칙은 FlipbookMath::CollectEvents)
struct FFlipbookEvent
{
	int32       Frame = 0;
	std::string Name;

	bool operator==(const FFlipbookEvent& Other) const = default;
};

// .eflipbook — 스프라이트 프레임 애니메이션 (JSON)
// { "Version": 1, "Sprite": "Hero.esprite", "Fps": 10, "Loop": "Loop",
//   "Frames": [ { "Slice": "Run_0" }, { "Slice": "Run_1", "Duration": 0.2 }, { "Slice": "Run_2", "Scale": 2 } ],
//   "Events": [ { "Frame": 1, "Name": "Footstep" } ] }
//   프레임 길이 = Duration(초)이 있으면 그것, 없으면 Scale / Fps. 최소 길이 MinFrameDuration
struct FFlipbookAsset
{
	static constexpr uint32         Version          = 1;
	static constexpr const wchar_t* Extension        = L".eflipbook";
	static constexpr float          MinFrameDuration = 0.001f;

	std::string                 Sprite; // 이 파일 폴더 기준 .esprite
	float                       Fps  = 10.0f;
	EFlipbookLoopMode           Loop = EFlipbookLoopMode::Loop;
	std::vector<FFlipbookFrame> Frames;
	std::vector<FFlipbookEvent> Events;

	// 프레임 길이(초) 목록 — 읽을 때 RebuildTimeline이 채운다 (직렬화 안 함). 프레임/Fps를 고친 코드는 다시 부른다
	std::vector<float> FrameDurations;
	float              TotalDuration = 0.0f; // 한 번 0 → N-1 재생 길이

	float GetFrameDuration(size_t Index) const;
	void  RebuildTimeline();

	std::string ToJsonString() const;
	// 형식 오류만 false. 이상한 값(빈 슬라이스, 범위 밖 이벤트 프레임 등)은 경고
	static bool FromJsonString(std::string_view Json, FFlipbookAsset& OutAsset, std::vector<std::string>* OutWarnings = nullptr, std::string* OutError = nullptr);

	bool operator==(const FFlipbookAsset& Other) const
	{
		return Sprite == Other.Sprite && Fps == Other.Fps && Loop == Other.Loop && Frames == Other.Frames && Events == Other.Events;
	}
};

struct FFlipbookSample
{
	int32 Frame     = -1;    // 프레임이 없으면 -1
	bool  bFinished = false; // Once: Time >= 전체 길이 (마지막 프레임에 멈춤)
};

// 순수 계산 (테스트: Flipbook_*).
//
// 시간축: 프레임 i는 반열린 구간 [시작_i, 시작_i + 길이_i) — 경계 시각은 다음 프레임에 속한다.
//   주기 길이: Loop = 전체 길이 T, Once = T(반복 없음), PingPong = 2T - 길이_0 - 길이_(N-1) (N = 1이면 T).
//   PingPong 한 주기의 프레임 순서 = 0, 1, ..., N-1, N-2, ..., 1 (위치 p의 프레임 = p < N ? p : 2N-2-p).
//
// Evaluate(Time): Loop/PingPong은 Time을 주기로 감싼다(음수도 양의 나머지). Once는 Time < 0 → 프레임 0, Time >= T → 마지막 프레임 + bFinished.
//
// CollectEvents(Prev, New): 재생 위치가 Prev → New로 움직이는 동안 "들어간" 프레임 구간마다 그 프레임의 이벤트를 순서대로 낸다.
//   - 시간축을 주기마다 펼친(Loop/PingPong 무한 반복, Once는 [0, T) 한 번) 프레임 구간 목록으로 본다.
//   - 정방향(New > Prev): 구간 시작 s가 (Prev, New]에 있으면 들어간 것.
//   - 역방향(New < Prev, Speed < 0): 구간 끝 e가 [New, Prev)에 있으면 들어간 것 (역재생은 프레임 "끝"으로 들어간다).
//     역방향은 시간이 줄어드는 순서(e 내림차순)로 낸다.
//   - bIncludeStart(재생 시작·처음 표시): Prev가 속한 프레임(Evaluate(Prev))에도 들어간 것으로 보고 그 이벤트를 먼저 낸다.
//     위 경계 판정이 엄격(Prev 제외)이라 같은 구간을 두 번 세지 않는다. New == Prev면 이것만 (bIncludeStart가 아니면 아무것도 없음).
//   - Once는 Prev/New를 [0, T]로 자른 뒤 같은 규칙 (T를 넘은 뒤 역재생하면 마지막 프레임부터 끝으로 들어간다).
//   - 큰 dt: 이동량이 한 주기를 넘으면 앞쪽의 완전한 주기들은 건너뛰고 마지막 "한 주기 + 나머지"만 본다 → 한 호출에서 같은 이벤트는 최대 2번.
//   - 같은 프레임의 이벤트 여럿은 Events 목록 순서. 범위 밖 Frame의 이벤트는 무시. Once의 끝(T)은 이벤트가 아니다 (bFinished로 알린다).
namespace FlipbookMath
{
	float GetCycleDuration(std::span<const float> Durations, EFlipbookLoopMode Mode);
	FFlipbookSample Evaluate(float Time, std::span<const float> Durations, EFlipbookLoopMode Mode);
	// 결과 = Events 목록 번호 (OutEventIndices 뒤에 덧붙인다 — 비우지 않음)
	void CollectEvents(float PrevTime, float NewTime, std::span<const float> Durations, EFlipbookLoopMode Mode, std::span<const FFlipbookEvent> Events,
	                   bool bIncludeStart, std::vector<int32>& OutEventIndices);
	// Loop/PingPong 재생 위치를 [0, 주기)로 감싼다 (누적 시간의 float 정밀도 손실 방지 — 이벤트 수집 뒤에 부른다). Once는 그대로
	float WrapTime(float Time, std::span<const float> Durations, EFlipbookLoopMode Mode);
} // namespace FlipbookMath
