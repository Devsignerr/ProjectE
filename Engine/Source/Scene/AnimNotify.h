#pragma once

#include "Core/ECS/Entity.h"

#include <string>
#include <vector>

// 애니메이션 노티파이 (언리얼 AnimNotify / AnimNotifyState 방식, 이름 기반 전달)
//   노티파이: 클립의 한 시점. 지나가면 한 번 발생 → Lua OnAnimNotify_<이름>(self), 게임 모듈 OnAnimNotify
//   스테이트: 클립의 구간 [시작, 시작+길이). 들어갈 때 Begin, 머무는 동안 매 프레임 Tick(dt), 나올 때 End
// 규칙 (AnimationSystem):
//   - 재생이 멈춰 있으면(진행량 0) 아무것도 발생하지 않는다. 역재생(속도 음수)은 거꾸로 지나가며 같은 규칙
//   - 루프 경계는 끝까지 + 처음부터로 나눠 판정 (누락/중복 없음). 반복 없는 클립은 끝 시각의 노티파이를 한 번만
//   - 클립이 바뀌면(크로스페이드 포함) 이전 클립의 진행 중 스테이트는 즉시 End, 사라지는 클립의 노티파이는 발생하지 않는다
//   - SetTime(스크럽)은 점 노티파이를 발생시키지 않고, 진행 중 스테이트를 End한 뒤 다음 진행에서 그 시각을 포함한 스테이트를 Begin
//   - 이벤트는 애니메이션 갱신에서 모이고 다음 프레임에 전달된다 (게임 모듈: OnUpdate 직전, Lua: OnUpdate 뒤).
//     Lua 수신자는 노티파이가 난 모델 루트의 스크립트, 없으면 가장 가까운 조상의 스크립트

enum class EAnimNotifyKind : uint8
{
	Notify, // 한 시점
	State,  // 구간
};

struct FAnimNotify
{
	std::string     Name;              // 식별자 ([A-Za-z_][A-Za-z0-9_]*) — Lua 함수 이름이 된다
	EAnimNotifyKind Kind     = EAnimNotifyKind::Notify;
	float           Time     = 0.0f;   // 초 (스테이트는 시작)
	float           Duration = 0.0f;   // 스테이트 길이 (초, > 0)

	float GetEndTime() const { return Time + Duration; }
};

enum class EAnimNotifyEventType : uint8
{
	Notify,
	StateBegin,
	StateTick,
	StateEnd,
};

// 전달되는 이벤트 하나
struct FAnimNotifyEvent
{
	FEntity              Entity;       // FAnimationComponent가 있는 모델 루트
	std::string          Name;
	std::string          Clip;
	EAnimNotifyEventType Type         = EAnimNotifyEventType::Notify;
	float                DeltaSeconds = 0.0f; // Tick일 때 프레임 시간
};

// 판정 결과 (노티파이 목록의 인덱스 + 종류)
struct FAnimNotifyHit
{
	int32                Index = -1;
	EAnimNotifyEventType Type  = EAnimNotifyEventType::Notify;
};

namespace AnimNotifyMath
{
	// 이름이 식별자 규칙에 맞는지
	bool IsValidName(const std::string& Name);
	// 입력 문자열을 식별자로 고친다 (허용되지 않는 문자는 '_', 숫자로 시작하면 앞에 '_', 비면 "Notify")
	std::string MakeValidName(const std::string& Name);

	// 한 번의 진행(PreviousTime → NewTime)에서 발생한 이벤트를 시간 순서로 OutHits에 추가한다.
	//   Delta: 부호 있는 진행량 (0이면 아무것도 하지 않음), bWrapped: 루프 경계를 넘었는지 (AdvanceTime 결과)
	//   InOutActive: 스테이트별 진행 중 여부 (Notifies와 같은 크기로 맞춘다)
	//   bResync: 진행 시작 시각이 이미 스테이트 안이면 Begin (스크럽 직후)
	void Collect(const std::vector<FAnimNotify>& Notifies, float PreviousTime, float NewTime, float Delta, float Duration, bool bLoop, bool bWrapped,
	             bool bResync, std::vector<uint8>& InOutActive, std::vector<FAnimNotifyHit>& OutHits);

	// 진행 중인 스테이트를 모두 End (클립 전환/스크럽)
	void EndAll(std::vector<uint8>& InOutActive, std::vector<FAnimNotifyHit>& OutHits);
} // namespace AnimNotifyMath
