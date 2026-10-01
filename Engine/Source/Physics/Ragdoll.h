#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>
#include <string_view>
#include <vector>

// 사망 래그돌 (Phase 30-3). 스켈레탈 모델 루트(FAnimationComponent가 있는 엔티티)에 FRagdollComponent를 단다.
//
// 만들기 (FPhysicsSystem::EnableRagdoll): 스킨 뼈(FSkinComponent Joints)마다 "뼈 시작 → 자식 뼈들 평균 위치"(끝 뼈는 부모 방향으로 연장)
//   캡슐 하나를 현재(애니메이션) 자세 그대로 만들고, 부모 쪽 캡슐과 비틀림·흔들림 제한 관절(SwingTwist)로 잇는다.
//   MinBoneLength보다 짧은 뼈와 ExcludeBones에 걸린 뼈(와 그 아래)는 캡슐 없이 가장 가까운 부모 캡슐을 따라간다.
//   이웃 캡슐·처음부터 겹친 캡슐·주인(조상)의 바디/캐릭터 캡슐과는 충돌하지 않는다. 처음 속도 = 주인 바디/캐릭터 속도.
// 구동: 켜져 있는 동안 애니메이션은 그 모델의 포즈를 쓰지 않고(FAnimationRuntime::bPhysicsPose), 물리 갱신이 캡슐 자세를
//   뼈 엔티티 로컬 트랜스폼으로 쓴다(렌더 보간 포함). 모델 루트 엔티티는 움직이지 않는다.
// 끄기 (DisableRagdoll): 바디/관절을 지우고 켤 때 저장한 뼈 로컬 트랜스폼을 되돌린 뒤 애니메이션이 다시 포즈를 쓴다.
// 사망 연동 (FGameWorld, World/GameWorldRagdoll.cpp): bEnableOnDeath면 자신이나 조상 FHealthComponent가 죽으면 켜고 되살아나면 끈다.
//   모든 역할(서버/클라이언트)이 복제된 체력으로 각자 판단하는 로컬 연출이다 — 래그돌 자세는 복제하지 않는다.
//   Lua: entity:EnableRagdoll() / DisableRagdoll() / IsRagdollActive() (엔티티 자신이나 자손의 모델)
// 범위 밖: 애니메이션 + 물리 섞기(부분 래그돌), 뼈별 설정 저장(.emeta)
struct FRagdollComponent
{
	bool        bEnableOnDeath = true;   // 체력이 0이 되면 켜고 리스폰하면 끈다
	float       Mass           = 30.0f;  // kg, 캡슐 부피 비율로 나눈다
	float       RadiusScale    = 0.25f;  // 캡슐 반지름 = 뼈 길이 × 이 값 (MinRadius 이상, 길이의 절반 이하)
	float       MinRadius      = 2.0f;   // cm
	float       MinBoneLength  = 3.0f;   // cm, 이보다 짧은 뼈는 부모 캡슐에 붙어 간다
	float       SwingLimit     = 45.0f;  // 도, 관절 흔들림(원뿔 반각)
	float       TwistLimit     = 30.0f;  // 도, 뼈 축 비틀림 ±
	float       Friction       = 0.8f;   // 바닥 마찰
	std::string ExcludeBones;            // 쉼표로 구분한 이름 일부 (예: "Tail,Ear") — 걸린 뼈와 그 아래는 캡슐을 만들지 않는다
};

// 래그돌 설정 계산 (순수 함수, PhysicsTests). 위치는 월드 cm
struct FRagdollBoneInput
{
	FVector3 Position;         // 뼈 원점 (월드)
	int32    Parent   = -1;    // 노드 부모 인덱스
	bool     bJoint   = false; // 스킨 뼈인가
	bool     bExclude = false; // 이름 제외 목록에 걸림 (자손도 제외)
};

struct FRagdollPartLayout
{
	int32    Node       = -1;
	int32    ParentPart = -1; // 이 캡슐을 잇는 부모 캡슐 (결과 배열 인덱스, -1 = 래그돌 루트)
	FVector3 Start;           // 뼈 원점 = 부모 관절 위치
	FVector3 End;
	float    Radius = 0.0f;
};

namespace RagdollMath
{
	// 부모가 먼저 오는 순서로 결과를 낸다
	std::vector<FRagdollPartLayout> BuildLayout(const std::vector<FRagdollBoneInput>& Bones, const FRagdollComponent& Settings);
	// 선분 두 개 사이 최단 거리 (캡슐 겹침 판정)
	float SegmentDistance(const FVector3& A0, const FVector3& A1, const FVector3& B0, const FVector3& B1);
	// +Z를 Direction(정규화)으로 돌리는 회전 (캡슐은 로컬 +Z 축)
	FQuat RotationFromZ(const FVector3& Direction);
	// ExcludeBones 목록의 이름 일부가 Name에 들어 있는가 (대소문자 무시, 빈 항목 무시)
	bool MatchesExclude(std::string_view Name, std::string_view ExcludeList);
} // namespace RagdollMath
