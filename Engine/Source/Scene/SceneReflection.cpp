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

	// 이름/계층은 인스펙터와 직렬화가 별도로 다루므로 목록에서 숨기고 제거 불가
	Registry.RegisterType<FNameComponent>("NameComponent", "이름")
		.Property(&FNameComponent::Name, "Name", "이름")
		.AsComponent(false)
		.Hide();

	Registry.RegisterType<FHierarchyComponent>("HierarchyComponent", "계층")
		.AsComponent(false)
		.Hide();

	Registry.RegisterType<FTransientComponent>("TransientComponent", "생성됨")
		.AsComponent(false)
		.Hide();

	// WorldMatrix는 파생 값이므로 등록하지 않는다 (직렬화 제외)
	Registry.RegisterType<FTransformComponent>("TransformComponent", "트랜스폼")
		.Property(&FTransformComponent::Position, "Position", "위치").Range(-1.0e7f, 1.0e7f, 1.0f) // cm
		.Property(&FTransformComponent::Rotation, "Rotation", "회전")
		.Property(&FTransformComponent::Scale, "Scale", "스케일").Range(0.001f, 1000.0f, 0.02f)
		.AsComponent(false);

	// 런타임 핸들은 Transient, 에셋 참조 문자열이 직렬화 대상
	Registry.RegisterType<FStaticMeshComponent>("StaticMeshComponent", "스태틱 메시")
		.Property(&FStaticMeshComponent::Mesh, "Mesh", "메시", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::Material, "Material", "머티리얼", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::bVisible, "Visible", "표시")
		.Property(&FStaticMeshComponent::MeshAsset, "MeshAsset", "메시 에셋", PF_ReadOnly)
		.Property(&FStaticMeshComponent::MaterialAsset, "MaterialAsset", "머티리얼 에셋", PF_ReadOnly)
		.AsComponent();

	Registry.RegisterType<FModelComponent>("ModelComponent", "모델")
		.Property(&FModelComponent::AssetPath, "AssetPath", "에셋 경로", PF_ReadOnly)
		.AsComponent();

	Registry.RegisterType<FDirectionalLightComponent>("DirectionalLightComponent", "방향광")
		.Property(&FDirectionalLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FDirectionalLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 50.0f, 0.05f)
		.AsComponent();

	// 런타임 상태(Runtime)는 등록하지 않는다. FSkinComponent는 런타임 전용이라 등록하지 않음
	Registry.RegisterType<FAnimationComponent>("AnimationComponent", "애니메이션")
		.Property(&FAnimationComponent::Clip, "Clip", "클립")
		.Property(&FAnimationComponent::Speed, "Speed", "속도").Range(-5.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::BlendTime, "BlendTime", "블렌드 시간").Range(0.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::bPlaying, "Playing", "재생")
		.Property(&FAnimationComponent::bLoop, "Loop", "반복")
		.Property(&FAnimationComponent::bRootMotion, "RootMotion", "루트 모션")
		.AsComponent();
}
