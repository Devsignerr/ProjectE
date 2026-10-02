#include "Physics/PhysicsReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Settings/ProjectSettings.h"
#include "Physics/CharacterMovement.h"
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
