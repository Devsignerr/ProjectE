#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

// 물리 알림 이벤트 (Physics 모듈이 만들고 FGameWorld가 스크립트/게임 모듈에 전달). Scene에 두는 이유: 게임 모듈 인터페이스(GameModule.h)가
// Physics에 의존하지 않게. 규칙은 Physics/PhysicsSystem.h "충돌 알림" 절
enum class ECollisionEventType : uint8
{
	CollisionBegin, // 두 바디가 닿기 시작 (트리거 아님)
	CollisionEnd,   // 떨어짐 (한쪽이 사라져도)
	TriggerEnter,   // 트리거(센서) 영역에 들어옴 — 트리거 쪽과 들어온 쪽 둘 다 받는다
	TriggerExit,
	JointBreak,     // 관절이 끊어짐: Self = 관절 컴포넌트 엔티티, Other = 연결 대상 (월드면 무효)
};

// 받는 쪽(Self) 기준 한 건. 쌍 하나의 시작/끝은 양쪽에 한 번씩 생긴다 (Self/Other를 바꿔서)
struct FCollisionEvent
{
	ECollisionEventType Type = ECollisionEventType::CollisionBegin;
	FEntity             Self;
	FEntity             Other;         // 파괴되었으면 무효일 수 있다
	FVector3            Point;         // CollisionBegin: 접촉 지점 (cm, 월드)
	FVector3            Normal;        // CollisionBegin: Self를 Other에서 밀어내는 방향 (월드, 길이 1)
	float               Impulse = 0.0f;       // CollisionBegin: 충격 세기 추정 (kg·cm/s = 다가오던 속력 × 유효 질량), JointBreak: 끊은 힘 (N)
	float               ApproachSpeed = 0.0f; // CollisionBegin: 법선 방향으로 다가오던 속력 (cm/s)
};

const char* GetCollisionEventName(ECollisionEventType Type); // "CollisionBegin" 등
