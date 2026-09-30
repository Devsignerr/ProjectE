#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

// 강체 운동 형식 (직렬화 값 고정)
enum class EPhysicsMotionType : int32
{
	Static    = 0, // 움직이지 않음 (트랜스폼을 바꾸면 순간이동)
	Kinematic = 1, // 트랜스폼이 물리를 끌고 간다 (충돌로 밀리지 않음)
	Dynamic   = 2, // 물리가 트랜스폼을 결정
};

// 강체. 콜라이더 컴포넌트(박스/구/캡슐 중 하나)와 함께 쓴다. 콜라이더만 있고 강체가 없으면 정적 바디.
// 런타임 바디 핸들은 FPhysicsSystem이 엔티티별로 보관하며 컴포넌트에는 데이터만 둔다 (복제/직렬화 안전).
struct FRigidBodyComponent
{
	int32 MotionType        = static_cast<int32>(EPhysicsMotionType::Dynamic); // EPhysicsMotionType
	float Mass              = 0.0f;   // kg (Dynamic만). 0이면 콜라이더 부피 × Density로 자동 계산
	float Density           = 500.0f; // kg/m³ (Mass가 0일 때만). 기본값은 나무 정도 — 100cm 큐브 500kg, 60cm 상자 108kg
	float Friction          = 0.5f;
	float Restitution       = 0.0f; // 반발 (0 = 튀지 않음, 1 = 완전 탄성)
	float LinearDamping     = 0.05f;
	float AngularDamping    = 0.05f;
	float RollingResistance = 0.05f; // 구르기 저항 계수: 무언가에 닿아 있을 때만 회전을 줄인다 (구는 약 계수 × g로 감속). 0 = 없음
	bool  bUseGravity       = true;
};

// 박스 콜라이더. 크기는 트랜스폼 월드 스케일이 곱해진다 (바디 생성 시)
struct FBoxColliderComponent
{
	FVector3 HalfExtents = FVector3(50.0f, 50.0f, 50.0f); // cm (기본: 100cm 큐브 = 내장 큐브 메시)
	FVector3 Offset;                                       // cm, 엔티티 로컬
};

struct FSphereColliderComponent
{
	float    Radius = 50.0f; // cm (스케일 성분 중 최대값이 곱해진다)
	FVector3 Offset;
};

// 캡슐: 엔티티 로컬 +Z 축 방향 (Jolt 캡슐은 +Y 축이므로 내부에서 회전)
struct FCapsuleColliderComponent
{
	float    Radius     = 30.0f; // cm
	float    HalfHeight = 60.0f; // cm, 원기둥 부분의 절반 (전체 높이 = 2 * (HalfHeight + Radius))
	FVector3 Offset;
};
