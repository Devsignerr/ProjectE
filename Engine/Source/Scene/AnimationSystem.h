#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/AnimMontage.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FScene;

// FAnimationComponent를 가진 모델 루트를 순회하며 클립을 샘플링/크로스페이드하고 노드 엔티티의 로컬 트랜스폼에 쓴다.
// 호출 순서: Update → FScene::UpdateTransforms → 렌더 (스킨 팔레트가 조인트 월드 행렬을 사용)
// 스크립트/게임플레이는 아래 공개 API로 재생을 제어한다 (Lua 바인딩 대상).
class FAnimationSystem
{
public:
	static void Update(FScene& Scene, float DeltaSeconds);

	// 클립 재생 요청. BlendTime < 0이면 컴포넌트의 BlendTime 사용. 클립이 없거나 컴포넌트가 없으면 false
	static bool Play(FScene& Scene, FEntity Entity, std::string_view ClipName, float BlendTime = -1.0f);
	// 일시정지 (현재 포즈 유지). 다시 재생하려면 Play 또는 Resume
	static void Stop(FScene& Scene, FEntity Entity);
	static void Resume(FScene& Scene, FEntity Entity);
	static void SetSpeed(FScene& Scene, FEntity Entity, float Speed);

	// 현재 클립 재생 위치 (초). 설정은 [0, 길이]로 제한되고 크로스페이드를 끝낸다. 포즈는 다음 Update에서 반영
	// (일시정지 중에도 Update(0)이 그 시각 포즈를 쓴다 — 타임라인 스크럽용)
	static void  SetTime(FScene& Scene, FEntity Entity, float Seconds);
	static float GetTime(FScene& Scene, FEntity Entity);
	// 현재 클립 길이 (초). 클립이 없으면 0
	static float GetCurrentClipDuration(FScene& Scene, FEntity Entity);

	static std::vector<std::string> GetClipNames(FScene& Scene, FEntity Entity);
	// 현재 재생 중인 클립 이름 (없으면 빈 문자열). 그래프 재생 중이면 가중치가 가장 큰 클립
	static std::string GetCurrentClip(FScene& Scene, FEntity Entity);

	// ---- 애니메이션 그래프 (Scene/AnimGraph.h). Entity = FAnimGraphComponent가 있는 엔티티 또는 그 조상(캐릭터 루트 등) —
	//      자신부터 깊이 우선으로 처음 찾은 그래프 컴포넌트를 쓴다. 없으면 false / nullopt / 빈 문자열
	static FEntity              FindAnimGraph(const FScene& Scene, FEntity Entity);
	static bool                 SetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name, float Value);
	static bool                 SetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name, bool bValue);
	static std::optional<float> GetAnimParam(FScene& Scene, FEntity Entity, std::string_view Name); // bool은 0/1
	static bool                 IsAnimParamBool(FScene& Scene, FEntity Entity, std::string_view Name); // 그래프에 bool로 선언됨
	static std::string          GetAnimState(FScene& Scene, FEntity Entity); // 현재(들어가는 중인) 상태 이름

	// ---- 몽타주 (Scene/AnimMontage.h, 로컬 전용). Entity = FAnimationComponent가 있는 엔티티 또는 그 조상 (자신부터 깊이 우선)
	static constexpr const char* DefaultMontageSlot = "Default";
	static FEntity FindAnimation(const FScene& Scene, FEntity Entity);
	// 클립이 모델에 없거나 애니메이션 컴포넌트가 없으면 false. 같은 슬롯의 재생 중 몽타주는 새 BlendIn 시간으로 빠진다 (중단)
	static bool PlayMontage(FScene& Scene, FEntity Entity, std::string_view ClipName, const FMontagePlayParams& Params);
	// Slot이 비면 모든 슬롯. BlendOut < 0이면 몽타주마다 재생 때 준 BlendOut. 멈춘 몽타주가 있으면 true
	static bool StopMontage(FScene& Scene, FEntity Entity, std::string_view Slot, float BlendOut);
	// Slot이 비면 아무 슬롯. 중단되어 빠지는 중인 것은 재생 중이 아니다 (저절로 빠지는 끝부분은 재생 중)
	static bool IsMontagePlaying(FScene& Scene, FEntity Entity, std::string_view Slot);

	// ---- 시선 IK (Scene/AnimIK.h). Entity = FLookAtComponent가 있는 모델 루트 또는 그 조상. 컴포넌트가 없으면 false
	//      스크립트 목표가 있으면 컴포넌트 Target 엔티티보다 먼저 쓴다. 엔티티 목표는 매 프레임 그 위치를 따라간다
	static bool SetLookAtTarget(FScene& Scene, FEntity Entity, const FVector3& WorldPosition);
	static bool SetLookAtTargetEntity(FScene& Scene, FEntity Entity, FEntity Target);
	static bool ClearLookAtTarget(FScene& Scene, FEntity Entity);
};
