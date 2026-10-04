#pragma once

#include "Core/ECS/Entity.h"
#include "Physics/Physics2DWorld.h"
#include "Physics/PhysicsMath.h"
#include "Scene/CollisionEvents.h"

#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class FScene;

struct FPhysics2DHit
{
	FEntity  Entity;
	FVector2 Position; // 평면 cm (X = 월드 X, Y = 월드 Z)
	FVector2 Normal;
	float    Distance = 0.0f; // cm
	float    Fraction = 0.0f; // Distance / MaxDistance
};

// 2D 물리 (Box2D v3): 씬 ↔ 2D 월드 동기화 + 고정 스텝 + 렌더 보간. 평면 규약은 Physics/Physics2DMath.h 머리 주석 (X·Z 평면, 깊이 Y 유지,
// 각 = 화면 반시계 +), 컴포넌트는 Physics/Physics2DComponents.h. 3D FPhysicsSystem과 같은 구조·규칙이다:
//   대상: 2D 콜라이더(상자/원/캡슐/다각형/선분)가 하나라도 있는 엔티티 — 모두 한 바디의 모양. FRigidBody2DComponent가 없으면 정적,
//         bEnabled가 꺼져 있으면 바디 없음.
//   Update: 1) 새 엔티티 바디 생성 / 사라진 것 제거 / 모양·운동 형식·재질·레이어가 바뀌면 다시 생성
//           2) 정적: 트랜스폼이 바뀌면 순간이동, 키네마틱: 스텝마다 목표(이번 프레임 트랜스폼)까지 이동
//           3) 고정 스텝 (프로젝트 설정 물리 FixedStepHz/MaxSubSteps — 3D와 같은 값), 4) 동적: 보간 결과를 트랜스폼에 (X, Z, Y축 회전만 —
//           깊이 Y는 유지, 부모가 있으면 로컬로 역변환). 스크립트가 트랜스폼을 직접 바꿨으면 순간이동
//   엔티티 스케일 X/Z가 모양에 곱해지고(Y 무시), 평면 밖 회전(Y축이 기운 회전)은 무시한다 (엔티티마다 경고 한 번)
//   클라이언트 역할: SetKinematicOverride(FGameWorld — 복제 엔티티)면 동적 바디를 키네마틱으로
//   충돌 알림 (Scene/CollisionEvents.h, 3D와 같은 이벤트·같은 FGameWorld 전달 단계): 트리거 = 센서(정적 바디와는 알리지 않음),
//     보고 대상 = 트리거 || bReportContacts || SetContactReportFilter. 점/법선은 월드 3D (평면 위, Y = 받는 엔티티 깊이).
//     Box2D 이벤트·콜백(원웨이 사전 해결)에서는 게임 코드를 부르지 않고, 스텝 뒤 메인 스레드에서 GetCollisionEvents에 쌓는다
// 편집 모드에서는 쓰지 않는다 (FGameWorld가 플레이 시작 Begin, 정지 End — 앱은 따로 부르지 않는다).
class FPhysics2DSystem
{
public:
	FPhysics2DSystem();
	~FPhysics2DSystem();

	FPhysics2DSystem(const FPhysics2DSystem&)            = delete;
	FPhysics2DSystem& operator=(const FPhysics2DSystem&) = delete;

	void Begin();
	void End();
	bool IsActive() const { return World != nullptr; }

	// 반환: 이번 프레임 진행한 고정 스텝 수
	uint32 Update(FScene& Scene, float DeltaSeconds);
	// 바디 생성/제거/다시 만들기를 지금 반영 (Update도 부른다)
	void SyncBodies(FScene& Scene);

	void SetInterpolation(bool bEnabled) { bInterpolate = bEnabled; }
	void SetKinematicOverride(std::function<bool(const FScene&, FEntity)> Predicate) { KinematicOverride = std::move(Predicate); }
	void SetContactReportFilter(std::function<bool(const FScene&, FEntity)> Predicate) { ContactReportFilter = std::move(Predicate); }
	const std::vector<FCollisionEvent>& GetCollisionEvents() const { return CollisionEvents; } // 지난 Update에서 생긴 것

	// ---- 게임플레이 API (평면 cm, kg). 바디가 없는 엔티티는 무시 / 0
	void     AddForce(FEntity Entity, const FVector2& Force);     // kg·cm/s²
	void     AddImpulse(FEntity Entity, const FVector2& Impulse); // kg·cm/s
	void     SetVelocity(FEntity Entity, const FVector2& Velocity);
	FVector2 GetVelocity(FEntity Entity) const;
	void     SetAngularVelocity(FEntity Entity, float RadiansPerSecond); // 반시계 +
	float    GetAngularVelocity(FEntity Entity) const;
	float    GetMass(FEntity Entity) const; // 동적 바디가 아니면 0
	bool     HasBody(FEntity Entity) const { return Bodies.contains(Entity); }
	bool     IsDynamicBody(FEntity Entity) const;

	// ---- 질의 (평면 cm, 트리거 제외). LayerMask 비트 i = 충돌 레이어 칸 i (FCollisionLayerSettings::MakeMask)
	bool   Raycast(const FVector2& Origin, const FVector2& Direction, float MaxDistance, FPhysics2DHit& OutHit,
	               uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;
	// 겹친 엔티티 (중복 없음, OutEntities 끝에 붙인다). Angle = 라디안 반시계 +. 반환: 찾은 수
	uint32 OverlapBox(const FVector2& Center, const FVector2& HalfSize, float Angle, std::vector<FEntity>& OutEntities,
	                  uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;
	uint32 OverlapCircle(const FVector2& Center, float Radius, std::vector<FEntity>& OutEntities,
	                     uint32 LayerMask = FCollisionLayerSettings::AllLayersMask) const;

	uint32               GetBodyCount() const { return World ? World->GetBodyCount() : 0; }
	const FFixedStepper& GetStepper() const { return Stepper; }
	FPhysics2DWorld*     GetWorld() { return World.get(); }

private:
	struct FBodyState
	{
		uint32             Body = FPhysics2DWorld::InvalidBody;
		FPhysics2DBodyDesc CreatedDesc; // 생성 설정 (위치·각·보고 여부 제외하고 비교)
		uint64             LastSeenFrame = 0;
		float              Depth = 0.0f; // 월드 Y (바디가 바꾸지 않는다)

		FVector2 PreviousPosition, CurrentPosition;
		float    PreviousAngle = 0.0f, CurrentAngle = 0.0f;
		FVector3 WrittenPosition;
		FQuat    WrittenRotation;
		bool     bWritten = false;
	};
	// 문자열 점 목록 파싱 캐시 (다각형/선분 — 매 프레임 다시 읽지 않게)
	struct FPointCache
	{
		std::string           Source;
		std::vector<FVector2> Points;
		bool                  bParsed = false;
		bool                  bValid  = false;
	};
	struct FPointCaches
	{
		FPointCache Polygon;
		FPointCache Edge;
	};

	bool  BuildDesc(FScene& Scene, FEntity Entity, const FVector3& Scale, FPhysics2DBodyDesc& OutDesc);
	uint8 ResolveCollisionLayer(const std::string& Name) const;
	void  WarnOnce(FEntity Entity, uint32 Kind, const std::string& Message) const;
	const FPointCache& ParseCached(FEntity Entity, uint32 Kind, const std::string& Source);
	void  WriteDynamicTransforms(FScene& Scene);
	void  CollectContactEvents();

	std::unique_ptr<FPhysics2DWorld>            World;
	std::unordered_map<FEntity, FBodyState>     Bodies;
	std::unordered_map<FEntity, FPointCaches>   PointCaches;
	FFixedStepper                               Stepper;
	uint64                                      FrameCounter = 0;
	bool                                        bInterpolate = true;
	FCollisionLayerSettings                     CollisionLayers;
	mutable std::unordered_set<std::string>     WarnedLayerNames;
	mutable std::set<std::pair<uint64, uint32>> WarnedEntities;  // (엔티티, 경고 종류)
	std::function<bool(const FScene&, FEntity)> KinematicOverride;
	std::function<bool(const FScene&, FEntity)> ContactReportFilter;
	std::vector<FCollisionEvent>                CollisionEvents;
	std::vector<FPhysics2DContactEvent>         ContactScratch;
};
