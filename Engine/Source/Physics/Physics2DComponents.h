#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>

// 2D 물리 컴포넌트 (Box2D, 규칙은 Physics/Physics2DSystem.h 머리 주석).
// 2D 평면 = 월드 X(오른쪽)·Z(위), 깊이 = 월드 Y (바디는 깊이를 바꾸지 않는다). 치수는 cm, 각도는 도(화면 반시계 +).
// 콜라이더만 있고 FRigidBody2DComponent가 없으면 정적 바디. 한 엔티티의 2D 콜라이더(종류마다 하나)는 모두 한 바디의 모양이 된다.

// 2D 바디 운동 형식 (씬 JSON에는 번호로 저장된다 — 끝에만 추가. 인스펙터는 이름 콤보)
enum class EBodyType2D : int32
{
	Static    = 0, // 움직이지 않음 (트랜스폼을 바꾸면 순간이동)
	Kinematic = 1, // 트랜스폼을 따라간다 (충돌로 밀리지 않음)
	Dynamic   = 2, // 물리가 트랜스폼을 결정
};

struct FRigidBody2DComponent
{
	EBodyType2D BodyType       = EBodyType2D::Dynamic;
	float       Mass           = 0.0f; // kg (Dynamic만). 0이면 모양 넓이 × 콜라이더 Density
	float       GravityScale   = 1.0f;
	float       LinearDamping  = 0.0f;
	float       AngularDamping = 0.05f;
	bool        bFixedRotation = false; // 회전하지 않음 (2D 캐릭터)
	bool        bBullet        = false; // 빠른 물체: 동적 바디끼리도 연속 충돌 검사 (비싸다 — 총알 등에만)
	bool        bReportContacts = false; // 충돌 시작/끝 이벤트 (스크립트가 붙은 엔티티는 꺼져 있어도 FGameWorld가 보고시킨다)
	bool        bEnabled       = true;  // 끄면 바디를 만들지 않는다 (충돌·질의 모두 없음)
};

// ---- 콜라이더 공통 필드 (모양마다 반복 — 리플렉션은 평평한 구조체만 다룬다)
//   Offset       엔티티 로컬 평면 오프셋 (cm, X = 로컬 X, Y = 로컬 Z). 엔티티 스케일 X/Z가 곱해진다
//   Friction     마찰 (0~), Restitution 반발 (0 = 튀지 않음), Density 밀도 (kg/m², 강체 Mass가 0일 때 질량 계산)
//   bIsTrigger   트리거: 부딪히지 않고 OnTriggerEnter/Exit만 (정적 바디와는 알리지 않는다 — 3D와 같음)
//   bOneWay      원웨이 플랫폼: 엔티티 로컬 +Z(위) 쪽에서 내려오는 것만 막는다 (아래·옆에서 오는 것은 통과)
//   Layer        충돌 레이어 이름 (프로젝트 설정 "충돌 레이어" — 3D와 같은 행렬). 비었거나 없는 이름 = Default

// 상자: Size = 전체 크기 (cm, X = 폭, Y = 높이), Angle = 엔티티 기준 추가 회전 (도, 반시계 +)
struct FBoxCollider2DComponent
{
	FVector2    Size        = FVector2(100.0f, 100.0f);
	float       Angle       = 0.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 원: 반지름은 스케일 X/Z 중 큰 값이 곱해진다
struct FCircleCollider2DComponent
{
	float       Radius      = 50.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 세로 캡슐 (엔티티 로컬 +Z 축): Height = 전체 높이 (반원 포함, 2 × Radius보다 작으면 원), 반지름은 스케일 X, 높이는 스케일 Z
struct FCapsuleCollider2DComponent
{
	float       Height      = 200.0f;
	float       Radius      = 50.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 볼록 다각형: Points = "x,z; x,z; ..." (cm, 엔티티 로컬 — Physics2DMath::ParsePoints). 3~8점.
// 오목하거나 8점을 넘으면 볼록 껍질(넘으면 넓이를 가장 적게 잃는 점부터 빼서 8점)로 만들고 경고한다
struct FPolygonCollider2DComponent
{
	std::string Points      = "-50,-50; 50,-50; 0,50";
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 선분 체인 (정적 지형·타일 경계용, 질량 없음): Points 형식은 다각형과 같다 (2점 이상). bLoop = 마지막 점과 처음 점도 잇는다.
// 닫힌 체인(4점 이상)은 Box2D 체인(이음매 걸림 없음, 바깥쪽만 막음 — 감는 방향은 자동으로 맞춘다), 그 밖은 양면 선분들
struct FEdgeCollider2DComponent
{
	std::string Points      = "-100,0; 100,0";
	bool        bLoop       = false;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};
