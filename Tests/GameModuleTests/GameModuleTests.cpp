#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "SampleGameComponents.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

E_TEST(GameModule_LoadRegisterUpdateUnload)
{
	FScene Scene; // 엔진 타입 등록 (RegisterSceneTypes)

	FGameModuleHost Host;
	E_EXPECT_TRUE(Host.Load(FGameModuleHost::GetDefaultModulePath("SampleGame")));
	E_EXPECT_TRUE(Host.IsLoaded());
	E_EXPECT_TRUE(Host.GetName() == "SampleGame");

	// 게임 모듈이 등록한 타입이 엔진 레지스트리(엔진 DLL)에 보인다
	const FTypeInfo* SpinnerType = FTypeRegistry::Get().Find("SpinnerComponent");
	E_EXPECT_TRUE(SpinnerType != nullptr && SpinnerType->bIsComponent && SpinnerType->Owner == "SampleGame");
	E_EXPECT_TRUE(FTypeRegistry::Get().Find<FSpinnerComponent>() == SpinnerType); // type_index가 DLL 경계를 넘어 같다
	if (SpinnerType == nullptr)
	{
		return;
	}

	// 리플렉션(게임 DLL 코드)으로 추가한 컴포넌트를 테스트 실행 파일의 템플릿 코드로 조회: ECS 타입 ID 공유 확인
	const FEntity Entity = Scene.CreateEntity("Spinner");
	SpinnerType->AddComponent(Scene.GetRegistry(), Entity);
	FSpinnerComponent* Spinner = Scene.GetRegistry().TryGet<FSpinnerComponent>(Entity);
	E_EXPECT_TRUE(Spinner != nullptr);
	if (Spinner == nullptr)
	{
		return;
	}
	Spinner->DegreesPerSecond = 90.0f;
	Spinner->Axis             = FVector3::UpVector;

	// 게임 시스템: 1초 → Forward가 Right로 (UE 부호: +Yaw 오른쪽)
	Host.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		Host.Update(Scene, 0.1f);
	}
	Host.EndPlay(Scene);
	const FVector3 Forward = Scene.GetTransform(Entity).Rotation.RotateVector(FVector3::ForwardVector);
	E_EXPECT_EQUALS(Forward, FVector3::RightVector, 1.0e-3f);

	// 직렬화: 게임 컴포넌트가 씬 JSON에 들어가고 다시 읽힌다
	const std::string Json = FSceneSerializer::ToJsonString(Scene);
	E_EXPECT_TRUE(Json.find("SpinnerComponent") != std::string::npos);
	FScene Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, Json));
	bool bFound = false;
	Loaded.GetRegistry().View<FSpinnerComponent>().Each([&](FEntity, FSpinnerComponent& Component) {
		bFound = Component.DegreesPerSecond == 90.0f;
	});
	E_EXPECT_TRUE(bFound);

	// 언로드: 모듈 타입만 제거되고 엔진 타입은 유지
	Host.Unload();
	E_EXPECT_FALSE(Host.IsLoaded());
	E_EXPECT_TRUE(FTypeRegistry::Get().Find("SpinnerComponent") == nullptr);
	E_EXPECT_TRUE(FTypeRegistry::Get().Find("TransformComponent") != nullptr);
}

E_TEST(GameModule_MissingDllFailsGracefully)
{
	FGameModuleHost Host;
	E_EXPECT_FALSE(Host.Load(FGameModuleHost::GetDefaultModulePath("NoSuchGameModule")));
	E_EXPECT_FALSE(Host.IsLoaded());
	FScene Scene;
	Host.BeginPlay(Scene); // 로드되지 않았으면 무시
	Host.Update(Scene, 0.1f);
}
