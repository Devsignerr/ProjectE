#include "Scene/SceneReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Scene/Components.h"

void RegisterSceneTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();

	// 이름/계층은 인스펙터가 별도로 다루므로 목록에서 숨기고 제거 불가
	Registry.RegisterType<FNameComponent>("NameComponent", "이름")
		.Property(&FNameComponent::Name, "Name", "이름")
		.AsComponent(false)
		.Hide();

	Registry.RegisterType<FHierarchyComponent>("HierarchyComponent", "계층")
		.AsComponent(false)
		.Hide();

	// WorldMatrix는 파생 값이므로 등록하지 않는다 (직렬화 제외)
	Registry.RegisterType<FTransformComponent>("TransformComponent", "트랜스폼")
		.Property(&FTransformComponent::Position, "Position", "위치")
		.Property(&FTransformComponent::Rotation, "Rotation", "회전")
		.Property(&FTransformComponent::Scale, "Scale", "스케일").Range(0.001f, 1000.0f, 0.02f)
		.AsComponent(false);

	Registry.RegisterType<FStaticMeshComponent>("StaticMeshComponent", "스태틱 메시")
		.Property(&FStaticMeshComponent::Mesh, "Mesh", "메시")
		.Property(&FStaticMeshComponent::Material, "Material", "머티리얼")
		.Property(&FStaticMeshComponent::bVisible, "Visible", "표시")
		.AsComponent();

	Registry.RegisterType<FDirectionalLightComponent>("DirectionalLightComponent", "방향광")
		.Property(&FDirectionalLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FDirectionalLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 50.0f, 0.05f)
		.AsComponent();
}
