#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

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

	static std::vector<std::string> GetClipNames(FScene& Scene, FEntity Entity);
	// 현재 재생 중인 클립 이름 (없으면 빈 문자열)
	static std::string GetCurrentClip(FScene& Scene, FEntity Entity);
};
