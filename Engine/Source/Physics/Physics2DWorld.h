#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Core/Settings/CollisionSettings.h"
#include "Physics/Physics2DComponents.h"

#include <memory>
#include <vector>

// Box2D v3 월드 래퍼. 평면 좌표(cm, X = 월드 X, Y = 월드 Z — Physics2DMath.h 규약)로 입출력하고 안에서 m로 바꾼다 (cm ↔ m는 여기서만).
// Box2D 헤더는 Physics2DWorld.cpp에서만 포함한다. 여러 인스턴스를 만들 수 있다 (Box2D 월드 최대 128개).
//
// 접촉 알림: Box2D 이벤트(스텝 뒤 버퍼)를 Step 끝에 메인 스레드에서 바디 쌍 단위로 정리한다 — 같은 쌍의 모양 여럿이 닿아도
//   시작/끝은 한 번. DestroyBody는 그 바디가 닿아 있던 쌍의 끝을 바로 만든다 (Box2D가 내는 파괴 후 이벤트는 무시).
//   트리거(센서)는 트리거가 아닌 모양이 정적 바디가 아닐 때만 (3D와 같음). Box2D 콜백(사전 해결 — 원웨이)은 작업 스레드에서 불릴 수 있어
//   읽기만 하고 게임 코드는 부르지 않는다.
enum class EPhysics2DShape : uint8
{
	Box,
	Circle,
	Capsule,
	Polygon,
	Edge,
};

// 바디 로컬 모양 하나 (cm, 스케일이 이미 곱해진 값)
struct FPhysics2DShapeDesc
{
	EPhysics2DShape       Shape    = EPhysics2DShape::Box;
	FVector2              Offset;                       // 바디 로컬 가운데
	FVector2              HalfSize = FVector2(50.0f, 50.0f); // 상자 반 크기
	float                 Angle    = 0.0f;              // 상자 추가 회전 (라디안, 반시계 +)
	float                 Radius   = 50.0f;             // 원/캡슐
	float                 HalfSegment = 50.0f;          // 캡슐: 두 반원 중심 사이 거리의 절반 (로컬 +Y = 월드 Z 축)
	std::vector<FVector2> Points;                       // 다각형(볼록·3~8점, 반시계) / 선분 체인 — 바디 로컬, Offset 미적용
	bool                  bLoop       = false;          // 선분 체인 닫힘
	float                 Friction    = 0.6f;
	float                 Restitution = 0.0f;
	float                 Density     = 100.0f;         // kg/m²
	bool                  bIsTrigger  = false;
	bool                  bOneWay     = false;          // 바디 로컬 +Y(위) 쪽에서 오는 것만 막는다
	uint8                 CollisionLayer = 0;
};

struct FPhysics2DBodyDesc
{
	EBodyType2D                      Type     = EBodyType2D::Dynamic;
	FVector2                         Position;          // cm
	float                            Angle    = 0.0f;   // 라디안, 반시계 +
	float                            Mass     = 0.0f;   // kg (0 = 밀도로)
	float                            GravityScale   = 1.0f;
	float                            LinearDamping  = 0.0f;
	float                            AngularDamping = 0.05f;
	bool                             bFixedRotation = false;
	bool                             bBullet        = false;
	bool                             bReportContacts = false;
	uint64                           UserData = 0;      // 엔티티 ToId()
	std::vector<FPhysics2DShapeDesc> Shapes;
};

enum class EPhysics2DContactEventType : uint8
{
	Begin,
	End,
};

// 바디 쌍의 접촉 시작/끝. Body1 < Body2. Begin만(트리거 제외): 접촉 점(평면 cm), 법선 = 바디 2를 1에서 밀어내는 방향,
// 다가오던 속력 (cm/s — 같은 스텝의 Box2D 충돌 이벤트가 없으면 0), 충격 세기 추정 (kg·cm/s = 다가오던 속력 × 유효 질량)
struct FPhysics2DContactEvent
{
	EPhysics2DContactEventType Type = EPhysics2DContactEventType::Begin;
	uint32   Body1 = ~0u, Body2 = ~0u;
	uint64   UserData1 = 0, UserData2 = 0;
	bool     bSensor = false;
	FVector2 Position;
	FVector2 Normal;
	float    ApproachSpeed = 0.0f;
	float    Impulse       = 0.0f;
};

struct FPhysics2DRayHit
{
	uint64   UserData = 0;
	FVector2 Position; // cm
	FVector2 Normal;
	float    Distance = 0.0f; // cm
	float    Fraction = 0.0f; // 0~1 (Distance / MaxDistance)
};

class FPhysics2DWorld
{
public:
	static constexpr uint32 InvalidBody = ~0u;

	FPhysics2DWorld();
	~FPhysics2DWorld();

	FPhysics2DWorld(const FPhysics2DWorld&)            = delete;
	FPhysics2DWorld& operator=(const FPhysics2DWorld&) = delete;

	bool IsValid() const; // Box2D 월드 생성 실패(월드 수 한도) 시 false — 모든 호출이 무시된다

	// 모양을 하나도 만들지 못하면 InvalidBody
	uint32 CreateBody(const FPhysics2DBodyDesc& Desc);
	void   DestroyBody(uint32 Body);
	uint32 GetBodyCount() const;

	void SetTransform(uint32 Body, const FVector2& Position, float Angle); // 순간이동 (속도 유지)
	void MoveKinematic(uint32 Body, const FVector2& Position, float Angle, float DeltaSeconds); // 다음 스텝 동안 목표까지
	bool GetTransform(uint32 Body, FVector2& OutPosition, float& OutAngle) const;

	// 힘 kg·cm/s², 충격량 kg·cm/s, 속도 cm/s, 각속도 rad/s (반시계 +)
	void     AddForce(uint32 Body, const FVector2& Force);
	void     AddImpulse(uint32 Body, const FVector2& Impulse);
	void     SetLinearVelocity(uint32 Body, const FVector2& Velocity);
	FVector2 GetLinearVelocity(uint32 Body) const;
	void     SetAngularVelocity(uint32 Body, float RadiansPerSecond);
	float    GetAngularVelocity(uint32 Body) const;
	float    GetMass(uint32 Body) const; // kg, 정적/키네마틱 0

	void     SetGravity(const FVector2& Gravity); // cm/s² (기본 (0, -980.665))
	FVector2 GetGravity() const;
	// 충돌 레이어 행렬 (FCollisionLayerSettings). 이후 만드는 바디에 적용된다 (FPhysics2DSystem::Begin이 바디보다 먼저 부른다)
	void SetCollisionLayers(const FCollisionLayerSettings& Layers);

	void Step(float DeltaSeconds);

	// 트리거 제외. LayerMask = 맞을 수 있는 레이어 (비트 i = 칸 i)
	bool   Raycast(const FVector2& Origin, const FVector2& Direction, float MaxDistance, FPhysics2DRayHit& OutHit,
	               uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;
	// 겹침 (닿거나 겹친 바디마다 한 번, OutUserData 끝에 붙인다). 트리거 제외. 반환: 찾은 바디 수
	uint32 OverlapBox(const FVector2& Center, const FVector2& HalfSize, float Angle, std::vector<uint64>& OutUserData,
	                  uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;
	uint32 OverlapCircle(const FVector2& Center, float Radius, std::vector<uint64>& OutUserData,
	                     uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;

	// 접촉 알림 (클래스 주석). 보고 여부는 바디를 다시 만들지 않고 바꾼다 (이미 닿아 있는 쌍에는 다음 접촉부터)
	void SetBodyReportsContacts(uint32 Body, bool bReport);
	void ConsumeContactEvents(std::vector<FPhysics2DContactEvent>& OutEvents); // 끝에 붙이고 비운다

private:
	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};
