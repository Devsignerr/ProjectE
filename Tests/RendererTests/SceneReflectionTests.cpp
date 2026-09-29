#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scene/SceneReflection.h"

#include <cstddef>

E_TEST(SceneReflection_ComponentsRegistered)
{
	// FScene 생성이 등록을 보장하고, 반복 호출은 무해하다
	FScene Scene;
	RegisterSceneTypes();

	const FTypeRegistry& Registry = FTypeRegistry::Get();

	const FTypeInfo* Transform = Registry.Find<FTransformComponent>();
	E_EXPECT_TRUE(Transform != nullptr);
	if (Transform == nullptr)
	{
		return;
	}
	E_EXPECT_TRUE(Transform->bIsComponent);
	E_EXPECT_FALSE(Transform->bRemovable);
	E_EXPECT_EQ(Transform->Properties.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Transform->FindProperty("Position")->Offset, offsetof(FTransformComponent, Position));
	E_EXPECT_TRUE(Transform->FindProperty("Rotation")->Type == EPropertyType::Quat);
	E_EXPECT_TRUE(Transform->FindProperty("Scale")->HasRange());

	const FTypeInfo* Light = Registry.Find<FDirectionalLightComponent>();
	E_EXPECT_TRUE(Light != nullptr && Light->bRemovable);
	E_EXPECT_TRUE(Light->FindProperty("Color")->HasFlag(PF_Color));
	E_EXPECT_TRUE(Light->FindProperty("Intensity")->HasRange());

	const FTypeInfo* Mesh = Registry.Find<FStaticMeshComponent>();
	E_EXPECT_TRUE(Mesh != nullptr);
	E_EXPECT_TRUE(Mesh->FindProperty("Mesh")->Type == EPropertyType::ResourceHandle);
	E_EXPECT_TRUE(Mesh->FindProperty("Mesh")->HandleTypeName.find("FMeshHandleTag") != std::string::npos);
	E_EXPECT_TRUE(Mesh->FindProperty("Visible")->Type == EPropertyType::Bool);

	// 이름/계층은 숨김 + 제거 불가
	E_EXPECT_TRUE(Registry.Find<FNameComponent>()->HasFlag(TF_HiddenInInspector));
	E_EXPECT_TRUE(Registry.Find<FHierarchyComponent>()->HasFlag(TF_HiddenInInspector));
	E_EXPECT_FALSE(Registry.Find<FNameComponent>()->bRemovable);

	// 훅이 실제 씬 레지스트리와 동작
	const FEntity Entity = Scene.CreateEntity("Reflected");
	E_EXPECT_TRUE(Transform->HasComponent(Scene.GetRegistry(), Entity));
	E_EXPECT_FALSE(Light->HasComponent(Scene.GetRegistry(), Entity));
	void* Added = Light->AddComponent(Scene.GetRegistry(), Entity);
	E_EXPECT_TRUE(Added != nullptr);
	Light->FindProperty("Intensity")->GetRef<float>(Added) = 3.0f;
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FDirectionalLightComponent>(Entity).Intensity, 3.0f, 0.0f);

	size_t VisibleComponentTypes = 0;
	Registry.ForEachComponentType([&](const FTypeInfo& Info) {
		if (!Info.HasFlag(TF_HiddenInInspector))
		{
			++VisibleComponentTypes;
		}
	});
	E_EXPECT_TRUE(VisibleComponentTypes >= 3); // 트랜스폼, 스태틱 메시, 방향광
}
