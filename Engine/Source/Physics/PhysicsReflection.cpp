#include "Physics/PhysicsReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"

void RegisterPhysicsTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();

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
		.AsComponent();

	Registry.RegisterType<FBoxColliderComponent>("BoxColliderComponent", "박스 콜라이더")
		.Property(&FBoxColliderComponent::HalfExtents, "HalfExtents", "반 크기 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FBoxColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FBoxColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.AsComponent();

	Registry.RegisterType<FSphereColliderComponent>("SphereColliderComponent", "구 콜라이더")
		.Property(&FSphereColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FSphereColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FSphereColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.AsComponent();

	Registry.RegisterType<FCapsuleColliderComponent>("CapsuleColliderComponent", "캡슐 콜라이더")
		.Property(&FCapsuleColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::HalfHeight, "HalfHeight", "원기둥 반 높이 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::bIsTrigger, "IsTrigger", "트리거").Tooltip("부딪히지 않고 들어옴/나감만 알린다 (OnTriggerEnter/Exit)")
		.AsComponent();
}
