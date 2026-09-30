#include "Core/Testing/TestFramework.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path GetSpawnContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectEPrefabSpawnTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"Scripts");
			fs::create_directories(Path / L"Prefabs");
			return Path;
		}();
		return Directory;
	}

	void WriteFile(const fs::path& Path, const char* Text)
	{
		std::ofstream(Path, std::ios::binary | std::ios::trunc) << Text;
	}

	FEntity FindNamed(FScene& Scene, const std::string& Name, size_t& OutCount)
	{
		FEntity Found;
		OutCount = 0;
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Component) {
			if (Component.Name == Name)
			{
				Found = Entity;
				++OutCount;
			}
		});
		return Found;
	}
} // namespace

E_TEST(ScriptPrefab_AssetValueOverridesRoundTrip)
{
	FScriptValueMap Overrides;
	Overrides["Ball"]  = FScriptValue::MakeAsset("Prefabs/Ball.eprefab", ".eprefab");
	Overrides["Label"] = FScriptValue::MakeString("Prefabs/Ball.eprefab"); // 같은 글자라도 문자열은 문자열
	const FScriptValueMap Parsed = FScriptProperties::ParseOverrides(FScriptProperties::SerializeOverrides(Overrides));
	E_EXPECT_TRUE(Parsed.at("Ball").Type == EScriptValueType::Asset && Parsed.at("Ball").String == "Prefabs/Ball.eprefab");
	E_EXPECT_TRUE(Parsed.at("Label").Type == EScriptValueType::String);
	E_EXPECT_TRUE(FScriptProperties::IsCompatible(FScriptValue::MakeAsset("", ".eprefab"), Parsed.at("Ball")));
	E_EXPECT_FALSE(FScriptProperties::IsCompatible(FScriptValue::MakeAsset("", ".eprefab"), Parsed.at("Label")));
}

E_TEST(ScriptPrefab_SpawnIsDeferredUntilAfterUpdate)
{
	const fs::path Content = GetSpawnContent();
	FPrefabLibrary::Get().SetContentDirectory(Content);

	// Ball 프리팹: 루트(스크립트 Tick) + 자식 Shell
	WriteFile(Content / L"Scripts/Tick.lua", R"(
local Tick = { Properties = {} }
function Tick:OnStart() Ticks = (Ticks or 0) + 1 end
return Tick
)");
	{
		FScene        Source;
		const FEntity Ball = Source.CreateEntity("Ball");
		Source.GetRegistry().Emplace<FScriptComponent>(Ball).ScriptAsset = "Scripts/Tick.lua";
		const FEntity Shell = Source.CreateEntity("Shell");
		Source.SetParent(Shell, Ball);
		Source.GetRegistry().Emplace<FStaticMeshComponent>(Shell).MeshAsset = "primitive:sphere";
		E_EXPECT_TRUE(FPrefabLibrary::Get().CreatePrefab(Source, Ball, Content / L"Prefabs/Ball.eprefab"));
	}

	WriteFile(Content / L"Scripts/Spawner.lua", R"(
local Spawner = { Properties = { Ball = Prefab("Prefabs/Ball.eprefab"), Count = 0 } }
function Spawner:OnUpdate(dt)
	if self.Properties.Count < 2 then
		self.Properties.Count = self.Properties.Count + 1
		local Index = self.Properties.Count
		Scene.SpawnPrefab(self.Properties.Ball, Vector3(100 * Index, 0, 0), function(root)
			Spawned = (Spawned or 0) + 1
			LastX = root:GetPosition().X
		end)
		-- 지연 생성: 같은 OnUpdate 안에서는 아직 없다
		SeenDuringUpdate = (SeenDuringUpdate or 0) + ((Scene.Find("Shell") ~= nil) and 1 or 0)
	end
end
return Spawner
)");

	FScene        Scene;
	const FEntity Spawner = Scene.CreateEntity("Spawner");
	Scene.GetRegistry().Emplace<FScriptComponent>(Spawner).ScriptAsset = "Scripts/Spawner.lua";

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Content);
	const std::vector<FScriptPropertyDecl>* Decls = Scripts.GetPropertyDecls("Scripts/Spawner.lua");
	E_EXPECT_TRUE(Decls != nullptr && (*Decls)[0].Name == "Ball" && (*Decls)[0].Default.Type == EScriptValueType::Asset &&
	              (*Decls)[0].Default.AssetFilter == ".eprefab");

	Scripts.BeginPlay(Scene);
	Scripts.Update(0.016f, nullptr);
	size_t        Count = 0;
	const FEntity Ball  = FindNamed(Scene, "Ball", Count);
	E_EXPECT_EQ(Count, static_cast<size_t>(1));
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Scene, Ball));
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.X, 100.0f, 1.0e-4f);
	E_EXPECT_TRUE(Scripts.ConsumeSceneStructureChanged()); // 앱이 메시 참조를 해석하도록
	E_EXPECT_TRUE(Scripts.RunString("assert(Spawned == 1 and LastX == 100 and SeenDuringUpdate == 0)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == nil)")); // 만든 엔티티 스크립트는 다음 프레임부터

	Scripts.Update(0.016f, nullptr);
	FindNamed(Scene, "Ball", Count);
	E_EXPECT_EQ(Count, static_cast<size_t>(2));
	E_EXPECT_TRUE(Scripts.RunString("assert(Spawned == 2 and LastX == 200 and SeenDuringUpdate == 1 and Ticks == 1)"));

	// 인스펙터 오버라이드로 다른 프리팹 지정 (없는 파일 → 오류 보고, 크래시 없음)
	Scene.GetRegistry().Get<FScriptComponent>(Spawner).PropertyOverrides = "{\"Ball\":{\"Asset\":\"Prefabs/Missing.eprefab\"}}";
	const uint32 ErrorsBefore = Scripts.GetErrorCount();
	E_EXPECT_TRUE(Scripts.RunString("Scene.SpawnPrefab('Prefabs/Missing.eprefab')"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.GetErrorCount() > ErrorsBefore);
	E_EXPECT_FALSE(Scripts.RunString("Scene.SpawnPrefab(42)"));
	Scripts.EndPlay();
}
