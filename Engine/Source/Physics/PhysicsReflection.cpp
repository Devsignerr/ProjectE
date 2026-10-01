#include "Physics/PhysicsReflection.h"

#include "Core/Reflection/TypeInfo.h"
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
		.AsComponent();

	Registry.RegisterType<FBoxColliderComponent>("BoxColliderComponent", "박스 콜라이더")
		.Property(&FBoxColliderComponent::HalfExtents, "HalfExtents", "반 크기 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FBoxColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.AsComponent();

	Registry.RegisterType<FSphereColliderComponent>("SphereColliderComponent", "구 콜라이더")
		.Property(&FSphereColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FSphereColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.AsComponent();

	Registry.RegisterType<FCapsuleColliderComponent>("CapsuleColliderComponent", "캡슐 콜라이더")
		.Property(&FCapsuleColliderComponent::Radius, "Radius", "반지름 (cm)").Range(0.1f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::HalfHeight, "HalfHeight", "원기둥 반 높이 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Property(&FCapsuleColliderComponent::Offset, "Offset", "오프셋 (cm)").Range(-100000.0f, 100000.0f, 1.0f)
		.AsComponent();
}
