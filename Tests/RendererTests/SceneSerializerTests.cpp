#include "Core/Testing/TestFramework.h"
#include "Renderer/MaterialAsset.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <filesystem>

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 테스트 씬: 계층 + 트랜스폼 + 정적 메시(에셋 참조) + 방향광 + 모델 루트(자식은 Transient)
	void BuildTestScene(FScene& Scene)
	{
		FRegistry& Registry = Scene.GetRegistry();

		const FEntity Sun = Scene.CreateEntity("Sun");
		Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
		FDirectionalLightComponent& Light = Registry.Emplace<FDirectionalLightComponent>(Sun);
		Light.Color     = FVector3(1.0f, 0.5f, 0.25f);
		Light.Intensity = 2.5f;

		const FEntity Parent = Scene.CreateEntity("Parent");
		Scene.GetTransform(Parent).Position = FVector3(1.0f, 2.0f, 3.0f);
		Scene.GetTransform(Parent).Scale    = FVector3(2.0f, 2.0f, 2.0f);

		const FEntity Child = Scene.CreateEntity("Child");
		Scene.SetParent(Child, Parent);
		Scene.GetTransform(Child).Position = FVector3(0.5f, 0.0f, 0.0f);
		FStaticMeshComponent& Mesh = Registry.Emplace<FStaticMeshComponent>(Child);
		Mesh.MeshAsset     = "primitive:cube";
		Mesh.MaterialAsset = "Materials/Checker.emat";
		Mesh.bVisible      = false;
		Mesh.Mesh          = FMeshHandle{ 7, 1 }; // 런타임 핸들은 직렬화되지 않아야 한다

		const FEntity ModelRoot = Scene.CreateEntity("Helmet");
		Registry.Emplace<FModelComponent>(ModelRoot).AssetPath = "DamagedHelmet.glb";
		const FEntity ModelNode = Scene.CreateEntity("Node0");
		Scene.SetParent(ModelNode, ModelRoot);
		Registry.Emplace<FTransientComponent>(ModelNode);
		Registry.Emplace<FStaticMeshComponent>(ModelNode);

		Scene.UpdateTransforms();
	}

	FEntity FindByName(FScene& Scene, std::string_view Name)
	{
		FEntity Result;
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Component) {
			if (Component.Name == Name)
			{
				Result = Entity;
			}
		});
		return Result;
	}
} // namespace

E_TEST(SceneSerializer_RoundTrip)
{
	FScene Source;
	BuildTestScene(Source);

	const std::string Json = FSceneSerializer::ToJsonString(Source);
	E_EXPECT_TRUE(Json.find("\"StaticMeshComponent\"") != std::string::npos);
	E_EXPECT_TRUE(Json.find("primitive:cube") != std::string::npos);
	E_EXPECT_TRUE(Json.find("\"Node0\"") == std::string::npos); // Transient 제외
	E_EXPECT_TRUE(Json.find("\"Mesh\"") == std::string::npos);  // 핸들 제외

	FScene Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, Json));
	E_EXPECT_EQ(Loaded.GetRegistry().GetAliveCount(), 4u); // Sun, Parent, Child, Helmet

	const FEntity Sun    = FindByName(Loaded, "Sun");
	const FEntity Parent = FindByName(Loaded, "Parent");
	const FEntity Child  = FindByName(Loaded, "Child");
	const FEntity Helmet = FindByName(Loaded, "Helmet");
	E_EXPECT_TRUE(Sun.IsValid() && Parent.IsValid() && Child.IsValid() && Helmet.IsValid());
	if (!(Sun.IsValid() && Parent.IsValid() && Child.IsValid() && Helmet.IsValid()))
	{
		return;
	}

	// 계층
	E_EXPECT_TRUE(Loaded.GetParent(Child) == Parent);
	E_EXPECT_FALSE(Loaded.GetParent(Parent).IsValid());
	E_EXPECT_TRUE(Loaded.GetChildren(Helmet).empty());

	// 트랜스폼 (월드 위치까지 갱신되어야 함)
	E_EXPECT_EQUALS(Loaded.GetTransform(Parent).Position, FVector3(1.0f, 2.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(Loaded.GetTransform(Parent).Scale, FVector3(2.0f), Tol);
	E_EXPECT_EQUALS(Loaded.GetTransform(Child).GetWorldPosition(), FVector3(2.0f, 2.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(Loaded.GetTransform(Sun).Rotation, FQuat::FromEuler(-50.0f, 30.0f, 0.0f), 1.0e-3f);

	// 컴포넌트 값
	const FDirectionalLightComponent* Light = Loaded.GetRegistry().TryGet<FDirectionalLightComponent>(Sun);
	E_EXPECT_TRUE(Light != nullptr);
	if (Light)
	{
		E_EXPECT_EQUALS(Light->Color, FVector3(1.0f, 0.5f, 0.25f), Tol);
		E_EXPECT_NEAR(Light->Intensity, 2.5f, Tol);
	}
	const FStaticMeshComponent* Mesh = Loaded.GetRegistry().TryGet<FStaticMeshComponent>(Child);
	E_EXPECT_TRUE(Mesh != nullptr);
	if (Mesh)
	{
		E_EXPECT_TRUE(Mesh->MeshAsset == "primitive:cube");
		E_EXPECT_TRUE(Mesh->MaterialAsset == "Materials/Checker.emat");
		E_EXPECT_FALSE(Mesh->bVisible);
		E_EXPECT_FALSE(Mesh->Mesh.IsValid()); // 핸들은 복원 전까지 무효
	}
	const FModelComponent* Model = Loaded.GetRegistry().TryGet<FModelComponent>(Helmet);
	E_EXPECT_TRUE(Model != nullptr && Model->AssetPath == "DamagedHelmet.glb");

	// 저장→로드→저장이 안정적이어야 한다 (순서/값 보존)
	const std::string Json2 = FSceneSerializer::ToJsonString(Loaded);
	FScene            Loaded2;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded2, Json2));
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Loaded2) == Json2);
	E_EXPECT_TRUE(Json2 == Json);
}

E_TEST(SceneSerializer_FileAndTolerance)
{
	FScene Source;
	BuildTestScene(Source);

	const std::filesystem::path Path = std::filesystem::temp_directory_path() / L"ProjectE_테스트" / L"Scene.escene";
	E_EXPECT_TRUE(FSceneSerializer::SaveToFile(Source, Path));

	FScene Loaded;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Loaded, Path));
	E_EXPECT_EQ(Loaded.GetRegistry().GetAliveCount(), 4u);
	std::filesystem::remove_all(Path.parent_path());

	// 알 수 없는 컴포넌트/프로퍼티, 잘못된 값 형식은 무시하고 나머지는 로드
	const std::string Tolerant = R"({"Version":1,"Entities":[
		{"Name":"A","Parent":-1,"Components":{"TransformComponent":{"Position":[1,1,1],"Bogus":5},
		 "NoSuchComponent":{"X":1},"DirectionalLightComponent":{"Intensity":"문자열","Color":[0,1,0]}}},
		{"Name":"B","Parent":0,"Components":{}}]})";
	FScene TolerantScene;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(TolerantScene, Tolerant));
	E_EXPECT_EQ(TolerantScene.GetRegistry().GetAliveCount(), 2u);
	const FEntity A = FindByName(TolerantScene, "A");
	const FEntity B = FindByName(TolerantScene, "B");
	E_EXPECT_TRUE(TolerantScene.GetParent(B) == A);
	E_EXPECT_EQUALS(TolerantScene.GetTransform(A).Position, FVector3(1.0f), Tol);
	const FDirectionalLightComponent* Light = TolerantScene.GetRegistry().TryGet<FDirectionalLightComponent>(A);
	E_EXPECT_TRUE(Light != nullptr && Light->Color.Equals(FVector3(0.0f, 1.0f, 0.0f), Tol) && Light->Intensity == 1.0f);

	// 깨진 JSON / 구조 없음
	FScene Broken;
	E_EXPECT_FALSE(FSceneSerializer::FromJsonString(Broken, "{ not json"));
	E_EXPECT_FALSE(FSceneSerializer::FromJsonString(Broken, "{\"Version\":1}"));

	// 로드는 기존 내용을 비운다
	FScene Reused;
	BuildTestScene(Reused);
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Reused, Tolerant));
	E_EXPECT_EQ(Reused.GetRegistry().GetAliveCount(), 2u);
}

E_TEST(MaterialAsset_RoundTrip)
{
	FMaterialAsset Asset;
	Asset.Name                       = "Test";
	Asset.BaseColorTexture           = "../Tex.png";
	Asset.Constants.BaseColorTint    = FVector4(0.1f, 0.2f, 0.3f, 0.4f);
	Asset.Constants.SpecularColor    = FVector3(0.5f, 0.6f, 0.7f);
	Asset.Constants.Shininess        = 12.0f;
	Asset.Constants.SpecularStrength = 0.25f;

	FMaterialAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_TRUE(Loaded.Name == "Test" && Loaded.BaseColorTexture == "../Tex.png");
	E_EXPECT_EQUALS(Loaded.Constants.BaseColorTint, Asset.Constants.BaseColorTint, Tol);
	E_EXPECT_EQUALS(Loaded.Constants.SpecularColor, Asset.Constants.SpecularColor, Tol);
	E_EXPECT_NEAR(Loaded.Constants.Shininess, 12.0f, Tol);
	E_EXPECT_NEAR(Loaded.Constants.SpecularStrength, 0.25f, Tol);

	// 일부 필드 누락은 기본값 유지
	FMaterialAsset Partial;
	E_EXPECT_TRUE(Partial.FromJsonString(R"({"Name":"P","Shininess":5})"));
	E_EXPECT_NEAR(Partial.Constants.Shininess, 5.0f, Tol);
	E_EXPECT_NEAR(Partial.Constants.SpecularStrength, 1.0f, Tol);
	E_EXPECT_FALSE(Partial.FromJsonString("nope"));
}
