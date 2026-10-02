#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <filesystem>

// 에디터 핫 리로드 경로 (FGameModuleHost::Reload): 같은 DLL의 복사본 두 개를 차례로 로드한다.
// 주의: 이 테스트는 게임 컴포넌트를 테스트 실행 파일의 템플릿 코드(Registry.Get<FSpinnerComponent>)로 만지지 않는다 —
// 다시 로드는 ECS 타입 ID를 폐기하므로 ID를 먼저 캐시한 바이너리와 새 DLL의 ID가 달라진다 (모든 접근은 리플렉션 = 현재 DLL 코드)
namespace
{
	void* GetComponentByName(FScene& Scene, FEntity Entity, const char* TypeName)
	{
		const FTypeInfo* Type = FTypeRegistry::Get().Find(TypeName);
		return Type != nullptr ? Type->GetComponent(Scene.GetRegistry(), Entity) : nullptr;
	}

	FEntity FindByName(FScene& Scene, const std::string& Name)
	{
		FEntity Found;
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Component) {
			if (Component.Name == Name)
			{
				Found = Entity;
			}
		});
		return Found;
	}
} // namespace

E_TEST(GameModule_HotReloadRestoresSceneWithNewTypes)
{
	FScene Scene;

	// 그림자 복사본 두 개 (에디터는 <Saved>/HotReload/<이름>_<번호>.dll)
	const std::filesystem::path Source    = FGameModuleHost::GetDefaultModulePath("SampleGame");
	const std::filesystem::path Directory = std::filesystem::temp_directory_path() / L"ProjectEHotReloadTest";
	std::error_code             Error;
	std::filesystem::create_directories(Directory, Error);
	for (const std::filesystem::directory_entry& Old : std::filesystem::directory_iterator(Directory, Error))
	{
		std::error_code RemoveError;
		std::filesystem::remove(Old.path(), RemoveError); // 이전 실행의 복사본 (로드 중이던 프로세스는 끝났다)
	}
	const uint32               Stamp = static_cast<uint32>(std::filesystem::file_time_type::clock::now().time_since_epoch().count() % 1000000);
	const std::filesystem::path Copy1 = Directory / (L"SampleGame_" + std::to_wstring(Stamp) + L"_1.dll");
	const std::filesystem::path Copy2 = Directory / (L"SampleGame_" + std::to_wstring(Stamp) + L"_2.dll");
	E_EXPECT_TRUE(std::filesystem::copy_file(Source, Copy1, std::filesystem::copy_options::overwrite_existing, Error));
	E_EXPECT_TRUE(std::filesystem::copy_file(Source, Copy2, std::filesystem::copy_options::overwrite_existing, Error));

	FGameModuleHost Host;
	E_EXPECT_TRUE(Host.Load(Copy1, "SampleGame"));
	E_EXPECT_TRUE(Host.GetName() == "SampleGame"); // 복사본 파일 이름이 아니라 모듈 이름
	const FTypeInfo* Spinner = FTypeRegistry::Get().Find("SpinnerComponent");
	E_EXPECT_TRUE(Spinner != nullptr && Spinner->Owner == "SampleGame");
	if (Spinner == nullptr)
	{
		return;
	}

	const FEntity Entity = Scene.CreateEntity("HotReloadSpinner");
	void*         Value  = Spinner->AddComponent(Scene.GetRegistry(), Entity);
	Spinner->FindProperty("DegreesPerSecond")->GetRef<float>(Value) = 90.0f;
	const std::string Json = FSceneSerializer::ToJsonString(Scene);

	// 다시 로드: 타입 제거 → 새 DLL OnLoad → 같은 이름으로 다시 등록
	const uint32 IdBefore = FRegistry::FindOrAssignComponentTypeId(typeid(int)); // 엔진 타입 ID 표가 그대로인지 비교용
	E_EXPECT_TRUE(Host.Reload(Copy2));
	E_EXPECT_EQ(Host.GetReloadCount(), 1u);
	E_EXPECT_TRUE(Host.GetLoadedPath() == Copy2);
	E_EXPECT_EQ(FRegistry::FindOrAssignComponentTypeId(typeid(int)), IdBefore);
	const FTypeInfo* NewSpinner = FTypeRegistry::Get().Find("SpinnerComponent");
	E_EXPECT_TRUE(NewSpinner != nullptr && NewSpinner->Owner == "SampleGame");

	// 새 타입(새 ECS 풀)으로는 옛 컴포넌트가 보이지 않는다 → 씬 JSON으로 다시 만든다 (에디터 절차)
	E_EXPECT_TRUE(GetComponentByName(Scene, Entity, "SpinnerComponent") == nullptr);
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Scene, Json));
	const FEntity Restored = FindByName(Scene, "HotReloadSpinner");
	E_EXPECT_TRUE(Restored.IsValid());
	void* RestoredValue = GetComponentByName(Scene, Restored, "SpinnerComponent");
	E_EXPECT_TRUE(RestoredValue != nullptr);
	if (RestoredValue == nullptr)
	{
		return;
	}
	E_EXPECT_NEAR(NewSpinner->FindProperty("DegreesPerSecond")->GetRef<float>(RestoredValue), 90.0f, 1.0e-4f);

	// 새 DLL의 시스템이 새 풀의 컴포넌트를 돌린다: 1초 → Forward가 Right로
	Host.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		Host.Update(Scene, 0.1f);
	}
	Host.EndPlay(Scene);
	const FVector3 Forward = Scene.GetTransform(Restored).Rotation.RotateVector(FVector3::ForwardVector);
	E_EXPECT_EQUALS(Forward, FVector3::RightVector, 1.0e-3f);

	// 존재하지 않는 DLL로 다시 로드하면 이전 DLL로 되돌아간다 (타입 유지)
	E_EXPECT_FALSE(Host.Reload(Directory / L"NoSuchModule.dll"));
	E_EXPECT_TRUE(Host.IsLoaded() && Host.GetLoadedPath() == Copy2);
	E_EXPECT_TRUE(FTypeRegistry::Get().Find("SpinnerComponent") != nullptr);

	Host.Unload();
	Scene.Clear(); // 옛 풀 정리는 옛 DLL 코드 (프로세스 끝까지 로드 유지)
	E_EXPECT_TRUE(FTypeRegistry::Get().Find("SpinnerComponent") == nullptr);
}
