#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Scene/AnimNotify.h"

#include <string>

// 몽타주 (언리얼 AnimMontage를 단순화): 그래프/클립 재생 위에 클립 하나를 한 번(또는 반복) 덮어 재생한다.
//   슬롯 = 이름. 애니메이션 그래프 에셋의 "Slots"에 같은 이름이 있으면 그 본 마스크로 부분만 덮고, 없으면(그래프 없음 포함) 몸 전체.
//   한 슬롯에는 하나만 재생 중: 같은 슬롯에 새 몽타주를 재생하면 이전 것은 새 몽타주의 BlendIn 시간으로 빠지며(중단) 둘이 잠시 겹친다.
//   재생 구간 [StartTime, EndTime] (EndTime < 0 = 클립 끝), 배속 Speed (FAnimationComponent의 Speed/Playing도 곱해진다).
//   가중치: 들어올 때 BlendIn 동안 smoothstep 0 → 1, 나갈 때 그 순간 가중치에서 BlendOut 동안 smoothstep → 0.
//   반복이 아니면 남은 재생 시간(÷ |Speed|)이 BlendOut 이하가 되는 순간 저절로 빠지기 시작해 구간 끝에서 0이 된다 (끝까지 재생 = 완료).
//   반복이면 StopMontage까지 계속. 다 빠지면 끝 이벤트 (Lua OnMontageEnded(clip, interrupted, slot)) — interrupted = Stop/같은 슬롯 교체.
//   로컬 전용: 복제하지 않는다 (애니메이션 파라미터와 같은 입장 — 각 프로세스가 스크립트로 같은 몽타주를 재생한다).
// 노티파이: 몽타주마다 따로 판정 (그 클립의 노티파이, Scene/AnimNotify.h 규칙). 재생 구간 전체에서 발생하고(저절로 빠지는 끝부분 포함),
//   중단(Stop/교체)되면 그 순간 진행 중 스테이트를 End하고 더는 발생하지 않는다.
//   슬롯 마스크가 없는(몸 전체) 몽타주의 가중치가 0.5 이상인 동안은 아래 원천(클립 재생 / 그래프 기본 레이어)의 노티파이를 판정하지 않는다
//   (진행 중 스테이트 End, 다시 내려가면 그 시각부터 다시 판정). 그래프 추가 레이어는 자기 규칙(레이어 가중치)을 따른다.

struct FMontagePlayParams
{
	std::string Slot;              // 빈 문자열 = "Default" (몸 전체)
	float       BlendIn   = 0.2f;  // 초
	float       BlendOut  = 0.2f;  // 초
	float       Speed     = 1.0f;  // 음수면 거꾸로 (구간 끝에서 시작)
	float       StartTime = 0.0f;  // 초
	float       EndTime   = -1.0f; // 초, < 0 = 클립 끝
	bool        bLoop     = false;
};

// 이번 진행 구간 (노티파이 판정·루트 모션 인자)
struct FMontageStep
{
	float PreviousTime = 0.0f;
	float NewTime      = 0.0f;
	float Delta        = 0.0f; // 부호 있는 클립 시각 진행량 (초)
	bool  bWrapped     = false;
};

// 재생 중인 몽타주 하나 (FAnimationRuntime::Montages). 시간/가중치 진행은 순수 함수 AnimMontageMath::Advance
struct FAnimMontageInstance
{
	std::string        Clip;
	int32              ClipIndex = -1;
	FMontagePlayParams Params;
	float              Duration = 0.0f; // 클립 길이 (초)

	float Time               = 0.0f; // 현재 클립 시각 (초)
	float Elapsed            = 0.0f; // 재생 시작 뒤 (초, 배속 무관 — 블렌드 시간 기준)
	float BlendOutElapsed    = -1.0f; // >= 0이면 빠지는 중
	float BlendOutDuration   = 0.0f;
	float BlendOutStartWeight = 1.0f;
	float Weight             = 0.0f;
	bool  bInterrupted       = false; // Stop/교체로 빠지는 중 (끝 이벤트의 interrupted, 노티파이 중단)
	bool  bFinished          = false; // 다 빠짐 → 끝 이벤트 후 목록에서 제거

	FAnimNotifyTrack Notify;
	FMontageStep     LastStep; // 직전 갱신의 진행 구간 (루트 모션 — FAnimationSystem이 채운다)
};

// 끝 이벤트 (FAnimationRuntime::PendingMontageEvents — 직전 애니메이션 갱신에서 끝난 것)
struct FAnimMontageEvent
{
	FEntity     Entity; // FAnimationComponent가 있는 모델 루트
	std::string Clip;
	std::string Slot;
	bool        bInterrupted = false;
};

namespace AnimMontageMath
{
	// 재생 구간 [시작, 끝] (EndTime < 0 또는 길이 초과면 클립 끝, 시작 > 끝이면 시작 = 끝)
	float GetStartTime(const FAnimMontageInstance& Montage);
	float GetEndTime(const FAnimMontageInstance& Montage);
	// 재생 시작 상태로 (Speed < 0이면 끝에서 시작)
	void Start(FAnimMontageInstance& Montage);
	// 빠지기 시작 (이미 빠지는 중이면 남은 시간과 새 시간 중 짧은 쪽). BlendOut < 0이면 Params.BlendOut. bInterrupted = 중단
	void BeginBlendOut(FAnimMontageInstance& Montage, float BlendOut, bool bInterrupted);
	// DeltaSeconds(실제 시간 × 컴포넌트 Speed, 정지면 0)만큼 진행 → 시각/가중치/완료. 반환 = 이번 클립 시각 진행 구간
	FMontageStep Advance(FAnimMontageInstance& Montage, float DeltaSeconds);
} // namespace AnimMontageMath
