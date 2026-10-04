#include "Physics/PhysicsReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Settings/ProjectSettings.h"
#include "Physics/CharacterMovement.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/Ragdoll.h"

void RegisterPhysicsTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();
	// 충돌 레이어: 이름 문자열 (칸 순서를 바꿔도 씬이 깨지지 않게) + 인스펙터는 프로젝트 레이어 콤보
	const auto LayerOptions = []() { return FProjectSettings::Get().Collision.GetLayerNames(); };
	constexpr const char* LayerTip = "충돌 레이어 (프로젝트 설정 → 충돌 레이어). 행렬에서 꺼진 레이어끼리는 통과하고 트리거 알림도 없다. 비면 Default";

	Registry.RegisterType<FRigidBodyComponent>("RigidBodyComponent", "강체")
		.Property(&FRigidBodyComponent::MotionType, "MotionType", "운동 (0 정적, 1 키네마틱, 2 동적)").Range(0.0f, 2.0f, 1.0f)
		.Property(&FRigidBodyComponent::Mass, "Mass", "질량 (kg, 0 = 밀도로 자동)").Range(0.0f, 100000.0f, 0.1f)
		.Property(&FRigidBodyComponent::Density, "Density", "밀도 (kg/m³, 질량 0일 때)").Range(0.01f, 100000.0f, 1.0f)
		.Property(&FRigidBodyComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FRigidBodyComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FRigidBodyComponent::LinearDamping, "LinearDamping", "선형 감쇠").Range(0.0f, 10.0f, 0.01f)
		.Property(&FRigidBodyComponent::AngularDamping, "AngularDamping", "각 감쇠").Range(0.0f, 10.0f, 0.01f)
		.Property(&FRigidBodyComponent::RollingResistance, "RollingResistance", "구르기 저항").Range(0.0f, 1.0f, 0.005f)
		.Property(&FRigidBodyComponent::bUseGravity, "UseGravity", "중력 사용")
		.Property(&FRigidBodyComponent::bLockRotation, "LockRotation", "회전 고정").Tooltip("동적 바디가 회전하지 않는다 (캐릭터 캡슐)")
		.Property(&FRigidBodyComponent::bReportContacts, "ReportContacts", "충돌 알림")
		.Tooltip("충돌 시작/끝 이벤트를 낸다 (OnCollisionBegin/End, 게임 모듈). 스크립트가 붙은 엔티티는 꺼져 있어도 알린다")
		.AsComponent();

	Registry.RegisterType<FCharacterMovementComponent>("CharacterMovementComponent", "캐릭터 이동")
		.Property(&FCharacterMovementComponent::MaxWalkSpeed, "MaxWalkSpeed", "걷기 속도 (cm/s)").Range(0.0f, 5000.0f, 1.0f)
		.Property(&FCharacterMovementComponent::JumpZVelocity, "JumpZVelocity", "점프 속도 (cm/s)").Range(0.0f, 5000.0f, 1.0f)
		.Property(&FCharacterMovementComponent::AirControl, "AirControl", "공중 제어").Range(0.0f, 1.0f, 0.01f)
		.Property(&FCharacterMovementComponent::GravityScale, "GravityScale", "중력 배율").Range(0.0f, 10.0f, 0.01f)
		.Property(&FCharacterMovementComponent::CapsuleRadius, "CapsuleRadius", "캡슐 반지름 (cm)").Range(1.0f, 500.0f, 0.5f)
		.Property(&FCharacterMovementComponent::CapsuleHalfHeight, "CapsuleHalfHeight", "캡슐 원기둥 절반 (cm)").Range(0.0f, 500.0f, 0.5f)
		.Property(&FCharacterMovementComponent::MaxStepHeight, "MaxStepHeight", "계단 높이 (cm)").Range(0.0f, 200.0f, 0.5f)
		.Property(&FCharacterMovementComponent::MaxSlopeAngle, "MaxSlopeAngle", "최대 경사 (도)").Range(0.0f, 89.0f, 0.5f)
		.Property(&FCharacterMovementComponent::Mass, "Mass", "질량 (kg)").Range(1.0f, 10000.0f, 0.5f)
		.Property(&FCharacterMovementComponent::PushForce, "PushForce", "미는 힘 (N)").Range(0.0f, 100000.0f, 10.0f)
		.Property(&FCharacterMovementComponent::bFaceControlYaw, "FaceControlYaw", "시점 방향 보기").Tooltip("끄면 이동 방향을 본다")
		.Property(&FCharacterMovementComponent::bClientPrediction, "ClientPrediction", "클라이언트 예측")
		.Tooltip("멀티플레이: 소유 클라이언트가 입력 즉시 미리 움직이고 서버 결과로 보정한다. 끄면 서버 결과를 보간해 보여 준다(반응이 왕복 지연 + 0.1초 늦음). 프로젝트 설정 네트워크 → 클라이언트 예측도 켜져 있어야 한다")
		.Property(&FCharacterMovementComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.Property(&FCharacterMovementComponent::KnockbackDeceleration, "KnockbackDeceleration", "넉백 감속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Tooltip("AddKnockback 경직 동안 수평 속도를 줄이는 감속 (입력은 무시된다)")
		.AsComponent();

	Registry.RegisterType<FBoxColliderComponent>("BoxColliderComponent", "박스 콜라이더")
		.Property(&FBoxColliderComponent::HalfExtents, "HalfExtents", "반 크기 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FBoxColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FBoxColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.Property(&FBoxColliderComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FSphereColliderComponent>("SphereColliderComponent", "구 콜라이더")
		.Property(&FSphereColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FSphereColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FSphereColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.Property(&FSphereColliderComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FCapsuleColliderComponent>("CapsuleColliderComponent", "캡슐 콜라이더")
		.Property(&FCapsuleColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::HalfHeight, "HalfHeight", "원기둥 반 높이 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.Property(&FCapsuleColliderComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	// ---- 관절 (공통 필드 설명은 PhysicsComponents.h)
	constexpr const char* TargetTip  = "연결 대상 엔티티 (바디 필요). 비우면 월드 그 자리에 고정";
	constexpr const char* AnchorTip  = "이 엔티티 로컬 기준 연결 지점";
	constexpr const char* BreakTip   = "이 힘(N)을 넘으면 끊어진다 (0 = 안 끊김). 끊기면 OnJointBreak(other, force)";
	constexpr const char* CollideTip = "이은 두 바디끼리도 부딪힌다";
	Registry.RegisterType<FFixedJointComponent>("FixedJointComponent", "고정 관절")
		.Property(&FFixedJointComponent::Target, "Target", "대상").Tooltip(TargetTip)
		.Property(&FFixedJointComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(AnchorTip)
		.Property(&FFixedJointComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FFixedJointComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FHingeJointComponent>("HingeJointComponent", "경첩 관절")
		.Property(&FHingeJointComponent::Target, "Target", "대상").Tooltip(TargetTip)
		.Property(&FHingeJointComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(AnchorTip)
		.Property(&FHingeJointComponent::Axis, "Axis", "회전축 (로컬)").Range(-1.0f, 1.0f, 0.01f)
		.Property(&FHingeJointComponent::bLimit, "Limit", "각도 제한")
		.Property(&FHingeJointComponent::MinAngle, "MinAngle", "최소 각도 (도)").Range(-180.0f, 0.0f, 1.0f)
		.Property(&FHingeJointComponent::MaxAngle, "MaxAngle", "최대 각도 (도)").Range(0.0f, 180.0f, 1.0f)
		.Property(&FHingeJointComponent::bMotor, "Motor", "모터")
		.Property(&FHingeJointComponent::MotorSpeed, "MotorSpeed", "모터 속도 (도/초)").Range(-3600.0f, 3600.0f, 1.0f)
		.Property(&FHingeJointComponent::MotorMaxTorque, "MotorMaxTorque", "모터 최대 토크 (N·m)").Range(0.0f, 1000000.0f, 1.0f)
		.Property(&FHingeJointComponent::Friction, "Friction", "마찰 토크 (N·m)").Range(0.0f, 100000.0f, 0.1f)
		.Property(&FHingeJointComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FHingeJointComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FDistanceJointComponent>("DistanceJointComponent", "거리 관절")
		.Property(&FDistanceJointComponent::Target, "Target", "대상").Tooltip(TargetTip)
		.Property(&FDistanceJointComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(AnchorTip)
		.Property(&FDistanceJointComponent::TargetAnchor, "TargetAnchor", "대상 연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Tooltip("대상 로컬 기준 (대상이 없으면 월드 위치)")
		.Property(&FDistanceJointComponent::MinDistance, "MinDistance", "최소 거리 (cm, < 0 = 처음 거리)").Range(-1.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJointComponent::MaxDistance, "MaxDistance", "최대 거리 (cm, < 0 = 처음 거리)").Range(-1.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJointComponent::SpringFrequency, "SpringFrequency", "스프링 진동수 (Hz, 0 = 딱딱함)").Range(0.0f, 60.0f, 0.1f)
		.Property(&FDistanceJointComponent::SpringDamping, "SpringDamping", "스프링 감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FDistanceJointComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FDistanceJointComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FBallJointComponent>("BallJointComponent", "구 관절")
		.Property(&FBallJointComponent::Target, "Target", "대상").Tooltip(TargetTip)
		.Property(&FBallJointComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(AnchorTip)
		.Property(&FBallJointComponent::Axis, "Axis", "원뿔 축 (로컬)").Range(-1.0f, 1.0f, 0.01f)
		.Property(&FBallJointComponent::ConeAngle, "ConeAngle", "원뿔 반각 (도, 180 = 자유)").Range(0.0f, 180.0f, 1.0f)
		.Property(&FBallJointComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FBallJointComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	// ---- 2D 물리 (Box2D — 규칙은 Physics2DSystem.h, 평면 규약은 Physics2DMath.h: X·Z 평면, 각도 반시계 +)
	Registry.RegisterType<FRigidBody2DComponent>("RigidBody2DComponent", "2D 강체")
		.Property(&FRigidBody2DComponent::BodyType, "BodyType", "운동")
		.Enum({ { "Static", "정적" }, { "Kinematic", "키네마틱" }, { "Dynamic", "동적" } })
		.Property(&FRigidBody2DComponent::Mass, "Mass", "질량 (kg, 0 = 밀도로 자동)").Range(0.0f, 100000.0f, 0.1f)
		.Property(&FRigidBody2DComponent::GravityScale, "GravityScale", "중력 배율").Range(-10.0f, 10.0f, 0.01f)
		.Property(&FRigidBody2DComponent::LinearDamping, "LinearDamping", "선형 감쇠").Range(0.0f, 10.0f, 0.01f)
		.Property(&FRigidBody2DComponent::AngularDamping, "AngularDamping", "각 감쇠").Range(0.0f, 10.0f, 0.01f)
		.Property(&FRigidBody2DComponent::bFixedRotation, "FixedRotation", "회전 고정").Tooltip("회전하지 않는다 (2D 캐릭터)")
		.Property(&FRigidBody2DComponent::bBullet, "Bullet", "연속 충돌 (총알)").Tooltip("빠른 물체가 동적 바디도 뚫지 않게 (비싸다)")
		.Property(&FRigidBody2DComponent::bReportContacts, "ReportContacts", "충돌 알림")
		.Tooltip("충돌 시작/끝 이벤트를 낸다 (OnCollisionBegin/End, 게임 모듈). 스크립트가 붙은 엔티티는 꺼져 있어도 알린다")
		.Property(&FRigidBody2DComponent::bEnabled, "Enabled", "켜기").Tooltip("끄면 바디를 만들지 않는다")
		.AsComponent();

	// 2D 캐릭터 이동기 (규칙은 Physics/CharacterMovement2D.h — 런타임 상태는 FCharacterMovement2DSystem에만, 등록 안 함)
	Registry.RegisterType<FCharacterMovement2DComponent>("CharacterMovement2DComponent", "2D 캐릭터 이동")
		.Property(&FCharacterMovement2DComponent::Mode, "Mode", "방식")
		.Enum({ { "Platformer", "플랫포머" }, { "TopDown", "탑다운" } })
		.Tooltip("플랫포머: 중력·점프·원웨이·경사 (입력 X만). 탑다운: 중력 0, 8방향 (입력 X·Z)")
		.Property(&FCharacterMovement2DComponent::CapsuleRadius, "CapsuleRadius", "캡슐 반지름 (cm)").Range(2.0f, 500.0f, 0.5f)
		.Property(&FCharacterMovement2DComponent::CapsuleHeight, "CapsuleHeight", "캡슐 전체 높이 (cm)").Range(4.0f, 2000.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::MaxSpeed, "MaxSpeed", "최대 속력 (cm/s)").Range(0.0f, 10000.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::GroundAcceleration, "GroundAcceleration", "지상 가속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Property(&FCharacterMovement2DComponent::GroundDeceleration, "GroundDeceleration", "지상 감속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Property(&FCharacterMovement2DComponent::AirAcceleration, "AirAcceleration", "공중 가속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Property(&FCharacterMovement2DComponent::AirDeceleration, "AirDeceleration", "공중 감속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Property(&FCharacterMovement2DComponent::JumpVelocity, "JumpVelocity", "점프 속도 (cm/s)").Range(0.0f, 10000.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::MaxJumps, "MaxJumps", "최대 점프 수").Range(0.0f, 10.0f, 1.0f).Tooltip("2 = 2단 점프")
		.Property(&FCharacterMovement2DComponent::GravityScale, "GravityScale", "중력 배율").Range(0.0f, 20.0f, 0.01f)
		.Property(&FCharacterMovement2DComponent::MaxFallSpeed, "MaxFallSpeed", "최대 낙하 속도 (cm/s)").Range(0.0f, 20000.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::CoyoteTime, "CoyoteTime", "코요테 시간 (초)").Range(0.0f, 1.0f, 0.005f)
		.Tooltip("발판을 떠난 뒤에도 바닥 점프를 허용하는 시간")
		.Property(&FCharacterMovement2DComponent::JumpBufferTime, "JumpBufferTime", "점프 버퍼 (초)").Range(0.0f, 1.0f, 0.005f)
		.Tooltip("착지 전에 누른 점프를 기억하는 시간")
		.Property(&FCharacterMovement2DComponent::JumpCutFactor, "JumpCutFactor", "점프 컷 비율").Range(0.0f, 1.0f, 0.01f)
		.Tooltip("가변 점프: 상승 중 버튼을 떼면(StopJumping) 상승 속도 × 이 값. 1 = 끔")
		.Property(&FCharacterMovement2DComponent::MaxSlopeAngle, "MaxSlopeAngle", "최대 경사 (도)").Range(0.0f, 89.0f, 0.5f)
		.Property(&FCharacterMovement2DComponent::GroundSnapDistance, "GroundSnapDistance", "바닥 붙이기 (cm)").Range(0.0f, 200.0f, 0.5f)
		.Property(&FCharacterMovement2DComponent::DashSpeed, "DashSpeed", "대시 속도 (cm/s)").Range(0.0f, 20000.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::DashTime, "DashTime", "대시 시간 (초)").Range(0.0f, 2.0f, 0.005f)
		.Property(&FCharacterMovement2DComponent::DashCooldown, "DashCooldown", "대시 쿨다운 (초)").Range(0.0f, 10.0f, 0.01f)
		.Property(&FCharacterMovement2DComponent::MaxAirDashes, "MaxAirDashes", "공중 대시 수").Range(0.0f, 10.0f, 1.0f)
		.Property(&FCharacterMovement2DComponent::bDashIgnoresGravity, "DashIgnoresGravity", "대시 중 중력 무시")
		.Property(&FCharacterMovement2DComponent::DropThroughTime, "DropThroughTime", "원웨이 내려가기 (초)").Range(0.0f, 2.0f, 0.01f)
		.Property(&FCharacterMovement2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.Property(&FCharacterMovement2DComponent::bClientPrediction, "ClientPrediction", "클라이언트 예측")
		.Tooltip("멀티플레이: 소유 클라이언트가 입력 즉시 미리 움직이고 서버 결과로 보정한다. 프로젝트 설정 네트워크 → 클라이언트 예측도 켜져 있어야 한다")
		.Property(&FCharacterMovement2DComponent::CharacterCollision, "CharacterCollision", "캐릭터끼리")
		.Enum({ { "Ignore", "통과" }, { "Block", "막기" }, { "Push", "밀기" } })
		.Tooltip("다른 2D 캐릭터와: 통과 / 막기(위에 설 수 있고 밟으면 OnStomped) / 밀기(막히면 수평으로 민다, 밀린 캐릭터가 앞 캐릭터를 연쇄로 민다). 두 캐릭터 모두 통과가 아니어야 상호작용")
		.Property(&FCharacterMovement2DComponent::PushStrength, "PushStrength", "밀기 세기").Range(0.0f, 1.0f, 0.01f)
		.Tooltip("밀기: 막힌 거리 중 상대에게 넘기는 비율 (작을수록 무겁게 밀린다, 1 = 기본, 0 = 막기와 같음)")
		.Property(&FCharacterMovement2DComponent::PushResistance, "PushResistance", "밀림 저항").Range(0.0f, 100.0f, 0.1f)
		.Tooltip("다른 캐릭터에게 밀릴 때 넘겨받은 거리 ÷ (1 + 이 값) (0 = 기본, 1 = 절반만 밀림). 연쇄로 밀릴 때도 단계마다 나눈다")
		.Property(&FCharacterMovement2DComponent::KnockbackDeceleration, "KnockbackDeceleration", "넉백 감속 (cm/s²)").Range(0.0f, 100000.0f, 10.0f)
		.Tooltip("AddKnockback 경직 동안 수평 속도를 줄이는 감속 (입력은 무시된다)")
		.AsComponent();

	constexpr const char* OffsetTip  = "엔티티 로컬 평면 오프셋 (X = 로컬 X, Y = 로컬 Z, cm)";
	constexpr const char* OneWayTip  = "원웨이 플랫폼: 엔티티 위쪽(로컬 +Z)에서 내려오는 것만 막고 아래·옆에서는 통과";
	constexpr const char* TriggerTip = "부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)";
	constexpr const char* PointsTip  = "점 목록 \"x,z; x,z; ...\" (cm, 엔티티 로컬)";
	Registry.RegisterType<FBoxCollider2DComponent>("BoxCollider2DComponent", "2D 상자 콜라이더")
		.Property(&FBoxCollider2DComponent::Size, "Size", "크기 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FBoxCollider2DComponent::Angle, "Angle", "각도 (도, 반시계 +)").Range(-360.0f, 360.0f, 0.5f)
		.Property(&FBoxCollider2DComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(OffsetTip)
		.Property(&FBoxCollider2DComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FBoxCollider2DComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FBoxCollider2DComponent::Density, "Density", "밀도 (kg/m²)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FBoxCollider2DComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip(TriggerTip)
		.Property(&FBoxCollider2DComponent::bOneWay, "OneWay", "원웨이").Tooltip(OneWayTip)
		.Property(&FBoxCollider2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FCircleCollider2DComponent>("CircleCollider2DComponent", "2D 원 콜라이더")
		.Property(&FCircleCollider2DComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FCircleCollider2DComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(OffsetTip)
		.Property(&FCircleCollider2DComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FCircleCollider2DComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FCircleCollider2DComponent::Density, "Density", "밀도 (kg/m²)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FCircleCollider2DComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip(TriggerTip)
		.Property(&FCircleCollider2DComponent::bOneWay, "OneWay", "원웨이").Tooltip(OneWayTip)
		.Property(&FCircleCollider2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FCapsuleCollider2DComponent>("CapsuleCollider2DComponent", "2D 캡슐 콜라이더")
		.Property(&FCapsuleCollider2DComponent::Height, "Height", "전체 높이 (cm)").Range(0.1f, 100000.0f, 1.0f).Tooltip("로컬 +Z 방향, 반원 포함")
		.Property(&FCapsuleCollider2DComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FCapsuleCollider2DComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(OffsetTip)
		.Property(&FCapsuleCollider2DComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FCapsuleCollider2DComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FCapsuleCollider2DComponent::Density, "Density", "밀도 (kg/m²)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleCollider2DComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip(TriggerTip)
		.Property(&FCapsuleCollider2DComponent::bOneWay, "OneWay", "원웨이").Tooltip(OneWayTip)
		.Property(&FCapsuleCollider2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FPolygonCollider2DComponent>("PolygonCollider2DComponent", "2D 다각형 콜라이더")
		.Property(&FPolygonCollider2DComponent::Points, "Points", "점 목록").Tooltip(std::string(PointsTip) + " — 볼록 3~8점 (넘거나 오목하면 볼록 껍질)")
		.Property(&FPolygonCollider2DComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(OffsetTip)
		.Property(&FPolygonCollider2DComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FPolygonCollider2DComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FPolygonCollider2DComponent::Density, "Density", "밀도 (kg/m²)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FPolygonCollider2DComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip(TriggerTip)
		.Property(&FPolygonCollider2DComponent::bOneWay, "OneWay", "원웨이").Tooltip(OneWayTip)
		.Property(&FPolygonCollider2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	Registry.RegisterType<FEdgeCollider2DComponent>("EdgeCollider2DComponent", "2D 선분 콜라이더")
		.Property(&FEdgeCollider2DComponent::Points, "Points", "점 목록").Tooltip(std::string(PointsTip) + " — 2점 이상")
		.Property(&FEdgeCollider2DComponent::bLoop, "Loop", "닫기").Tooltip("마지막 점과 처음 점도 잇는다 (4점 이상이면 바깥쪽만 막는 체인)")
		.Property(&FEdgeCollider2DComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(OffsetTip)
		.Property(&FEdgeCollider2DComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FEdgeCollider2DComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FEdgeCollider2DComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip(TriggerTip)
		.Property(&FEdgeCollider2DComponent::bOneWay, "OneWay", "원웨이").Tooltip(OneWayTip)
		.Property(&FEdgeCollider2DComponent::Layer, "Layer", "레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.AsComponent();

	// ---- 2D 관절 (공통 필드 설명은 Physics2DComponents.h, 규칙은 Physics2DSystem.h "관절")
	constexpr const char* Target2DTip = "연결 대상 엔티티 (2D 바디 필요). 비우면 월드 그 자리에 고정";
	constexpr const char* Anchor2DTip = "이 엔티티 로컬 평면 기준 연결 지점 (X = 로컬 X, Y = 로컬 Z)";
	Registry.RegisterType<FDistanceJoint2DComponent>("DistanceJoint2DComponent", "2D 거리 관절")
		.Property(&FDistanceJoint2DComponent::Target, "Target", "대상").Tooltip(Target2DTip)
		.Property(&FDistanceJoint2DComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(Anchor2DTip)
		.Property(&FDistanceJoint2DComponent::TargetAnchor, "TargetAnchor", "대상 연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Tooltip("대상 로컬 평면 기준 (대상이 없으면 월드 평면 위치)")
		.Property(&FDistanceJoint2DComponent::Length, "Length", "길이 (cm, < 0 = 처음 거리)").Range(-1.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJoint2DComponent::MinLength, "MinLength", "최소 길이 (cm, < 0 = 없음)").Range(-1.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJoint2DComponent::MaxLength, "MaxLength", "최대 길이 (cm, < 0 = 없음)").Range(-1.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJoint2DComponent::SpringFrequency, "SpringFrequency", "스프링 진동수 (Hz, 0 = 딱딱함)").Range(0.0f, 60.0f, 0.1f)
		.Property(&FDistanceJoint2DComponent::SpringDamping, "SpringDamping", "스프링 감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FDistanceJoint2DComponent::bMotor, "Motor", "모터 (윈치)")
		.Tooltip("길이를 모터 속도로 바꾼다 — 켜면 딱딱한 막대가 아니라 최소/최대 길이 범위 안에서 늘고 준다 (범위가 없으면 끝없이)")
		.Property(&FDistanceJoint2DComponent::MotorSpeed, "MotorSpeed", "모터 속도 (cm/s, + = 늘어남)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FDistanceJoint2DComponent::MaxMotorForce, "MaxMotorForce", "모터 최대 힘 (N)").Range(0.0f, 10000000.0f, 1.0f)
		.Property(&FDistanceJoint2DComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FDistanceJoint2DComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FRevoluteJoint2DComponent>("RevoluteJoint2DComponent", "2D 회전 관절")
		.Property(&FRevoluteJoint2DComponent::Target, "Target", "대상").Tooltip(Target2DTip)
		.Property(&FRevoluteJoint2DComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(Anchor2DTip)
		.Property(&FRevoluteJoint2DComponent::SpringFrequency, "SpringFrequency", "회전 스프링 진동수 (Hz, 0 = 없음)").Range(0.0f, 60.0f, 0.1f)
		.Tooltip("목표 각으로 되돌리는 회전 스프링 (모터·한계와 함께 쓸 수 있다)")
		.Property(&FRevoluteJoint2DComponent::SpringDamping, "SpringDamping", "회전 스프링 감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FRevoluteJoint2DComponent::TargetAngle, "TargetAngle", "스프링 목표 각 (도)").Range(-180.0f, 180.0f, 1.0f)
		.Tooltip("만든 순간의 각 = 0, 반시계 +")
		.Property(&FRevoluteJoint2DComponent::bLimit, "Limit", "각도 제한")
		.Property(&FRevoluteJoint2DComponent::LowerAngle, "LowerAngle", "최소 각도 (도)").Range(-360.0f, 0.0f, 1.0f)
		.Property(&FRevoluteJoint2DComponent::UpperAngle, "UpperAngle", "최대 각도 (도)").Range(0.0f, 360.0f, 1.0f)
		.Property(&FRevoluteJoint2DComponent::bMotor, "Motor", "모터")
		.Property(&FRevoluteJoint2DComponent::MotorSpeed, "MotorSpeed", "모터 속도 (도/초, 반시계 +)").Range(-3600.0f, 3600.0f, 1.0f)
		.Property(&FRevoluteJoint2DComponent::MaxMotorTorque, "MaxMotorTorque", "모터 최대 토크 (N·m)").Range(0.0f, 1000000.0f, 1.0f)
		.Property(&FRevoluteJoint2DComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FRevoluteJoint2DComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FPrismaticJoint2DComponent>("PrismaticJoint2DComponent", "2D 미닫이 관절")
		.Property(&FPrismaticJoint2DComponent::Target, "Target", "대상").Tooltip(Target2DTip)
		.Property(&FPrismaticJoint2DComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(Anchor2DTip)
		.Property(&FPrismaticJoint2DComponent::Axis, "Axis", "이동 축 (로컬)").Range(-1.0f, 1.0f, 0.01f)
		.Property(&FPrismaticJoint2DComponent::SpringFrequency, "SpringFrequency", "스프링 진동수 (Hz, 0 = 없음)").Range(0.0f, 60.0f, 0.1f)
		.Tooltip("목표 이동으로 되돌리는 축 방향 스프링 (모터·한계와 함께 쓸 수 있다)")
		.Property(&FPrismaticJoint2DComponent::SpringDamping, "SpringDamping", "스프링 감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FPrismaticJoint2DComponent::TargetTranslation, "TargetTranslation", "스프링 목표 이동 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Tooltip("만든 순간의 위치 = 0, + = 축 방향")
		.Property(&FPrismaticJoint2DComponent::bLimit, "Limit", "이동 제한")
		.Property(&FPrismaticJoint2DComponent::LowerTranslation, "LowerTranslation", "최소 이동 (cm)").Range(-100000.0f, 0.0f, 1.0f)
		.Property(&FPrismaticJoint2DComponent::UpperTranslation, "UpperTranslation", "최대 이동 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FPrismaticJoint2DComponent::bMotor, "Motor", "모터")
		.Property(&FPrismaticJoint2DComponent::MotorSpeed, "MotorSpeed", "모터 속도 (cm/s)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FPrismaticJoint2DComponent::MaxMotorForce, "MaxMotorForce", "모터 최대 힘 (N)").Range(0.0f, 10000000.0f, 1.0f)
		.Property(&FPrismaticJoint2DComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FPrismaticJoint2DComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FWeldJoint2DComponent>("WeldJoint2DComponent", "2D 용접 관절")
		.Property(&FWeldJoint2DComponent::Target, "Target", "대상").Tooltip(Target2DTip)
		.Property(&FWeldJoint2DComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(Anchor2DTip)
		.Property(&FWeldJoint2DComponent::LinearFrequency, "LinearFrequency", "위치 강성 (Hz, 0 = 딱딱함)").Range(0.0f, 60.0f, 0.1f)
		.Property(&FWeldJoint2DComponent::AngularFrequency, "AngularFrequency", "각 강성 (Hz, 0 = 딱딱함)").Range(0.0f, 60.0f, 0.1f)
		.Property(&FWeldJoint2DComponent::Damping, "Damping", "감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FWeldJoint2DComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FWeldJoint2DComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	Registry.RegisterType<FWheelJoint2DComponent>("WheelJoint2DComponent", "2D 바퀴 관절")
		.Property(&FWheelJoint2DComponent::Target, "Target", "차체").Tooltip(Target2DTip)
		.Property(&FWheelJoint2DComponent::Anchor, "Anchor", "연결 지점 (cm)").Range(-100000.0f, 100000.0f, 1.0f).Tooltip(Anchor2DTip)
		.Property(&FWheelJoint2DComponent::Axis, "Axis", "서스펜션 축 (로컬)").Range(-1.0f, 1.0f, 0.01f)
		.Property(&FWheelJoint2DComponent::SpringFrequency, "SpringFrequency", "서스펜션 진동수 (Hz, 0 = 스프링 없음)").Range(0.0f, 60.0f, 0.1f).Tooltip("0이면 축 방향으로 자유롭게 미끄러진다 (Box2D 바퀴 관절 — 고정하려면 이동 제한 0~0)")
		.Property(&FWheelJoint2DComponent::SpringDamping, "SpringDamping", "서스펜션 감쇠").Range(0.0f, 2.0f, 0.01f)
		.Property(&FWheelJoint2DComponent::bLimit, "Limit", "서스펜션 이동 제한")
		.Property(&FWheelJoint2DComponent::LowerTranslation, "LowerTranslation", "최소 이동 (cm)").Range(-100000.0f, 0.0f, 1.0f)
		.Property(&FWheelJoint2DComponent::UpperTranslation, "UpperTranslation", "최대 이동 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FWheelJoint2DComponent::bMotor, "Motor", "모터")
		.Property(&FWheelJoint2DComponent::MotorSpeed, "MotorSpeed", "모터 속도 (도/초, 반시계 +)").Range(-36000.0f, 36000.0f, 1.0f)
		.Property(&FWheelJoint2DComponent::MaxMotorTorque, "MaxMotorTorque", "모터 최대 토크 (N·m)").Range(0.0f, 1000000.0f, 1.0f)
		.Property(&FWheelJoint2DComponent::BreakForce, "BreakForce", "끊어지는 힘 (N)").Range(0.0f, 10000000.0f, 10.0f).Tooltip(BreakTip)
		.Property(&FWheelJoint2DComponent::bCollideConnected, "CollideConnected", "서로 충돌").Tooltip(CollideTip)
		.AsComponent();

	// ---- 래그돌 (규칙은 Ragdoll.h)
	Registry.RegisterType<FRagdollComponent>("RagdollComponent", "래그돌")
		.Property(&FRagdollComponent::bEnableOnDeath, "EnableOnDeath", "사망 시 켜기").Tooltip("체력(자신이나 조상의 HealthComponent)이 0이 되면 켜고 리스폰하면 끈다")
		.Property(&FRagdollComponent::Mass, "Mass", "질량 (kg)").Range(0.1f, 10000.0f, 0.5f)
		.Property(&FRagdollComponent::RadiusScale, "RadiusScale", "캡슐 굵기 (뼈 길이 비율)").Range(0.01f, 0.5f, 0.01f)
		.Property(&FRagdollComponent::MinRadius, "MinRadius", "최소 반지름 (cm)").Range(0.1f, 100.0f, 0.1f)
		.Property(&FRagdollComponent::MinBoneLength, "MinBoneLength", "최소 뼈 길이 (cm)").Range(0.1f, 100.0f, 0.1f)
		.Tooltip("더 짧은 뼈는 캡슐 없이 부모를 따라간다")
		.Property(&FRagdollComponent::SwingLimit, "SwingLimit", "흔들림 제한 (도)").Range(0.0f, 180.0f, 1.0f)
		.Property(&FRagdollComponent::TwistLimit, "TwistLimit", "비틀림 제한 (도)").Range(0.0f, 180.0f, 1.0f)
		.Property(&FRagdollComponent::Friction, "Friction", "마찰").Range(0.0f, 2.0f, 0.01f)
		.Property(&FRagdollComponent::ExcludeBones, "ExcludeBones", "제외 뼈 (쉼표)").Tooltip("이름 일부가 맞는 뼈와 그 아래는 캡슐을 만들지 않는다 (예: Tail,Ear)")
		.AsComponent();
}
