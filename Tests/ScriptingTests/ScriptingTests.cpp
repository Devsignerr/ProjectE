#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"
#include "Scene/SceneSerializer.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

namespace
{
	constexpr float Tol = 1.0e-4f;

	// 테스트마다 임시 Content 디렉터리에 스크립트를 쓴다
	std::filesystem::path GetTestContentDirectory()
	{
		static const std::filesystem::path Directory = [] {
			std::filesystem::path Path = std::filesystem::temp_directory_path() / L"ProjectEScriptingTests";
			std::error_code       ErrorCode;
			std::filesystem::remove_all(Path, ErrorCode);
			std::filesystem::create_directories(Path / L"Scripts");
			return Path;
		}();
		return Directory;
	}

	void WriteScript(const char* RelativePath, const char* Source)
	{
		std::ofstream File(GetTestContentDirectory() / RelativePath, std::ios::binary | std::ios::trunc);
		File << Source;
	}

	FEntity AddScriptedEntity(FScene& Scene, const char* Name, const char* ScriptAsset, const char* Overrides = "")
	{
		const FEntity     Entity    = Scene.CreateEntity(Name);
		FScriptComponent& Component = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Component.ScriptAsset       = ScriptAsset;
		Component.PropertyOverrides = Overrides;
		return Entity;
	}

	constexpr const char* GMoverScript = R"(
local Mover = { Properties = { Speed = 100.0, Count = 3, Enabled = true, Label = "mover", Offset = Vector3(1, 2, 3) } }
function Mover:OnStart()
	self.Started = true
end
function Mover:OnUpdate(dt)
	if self.Properties.Enabled then
		local Position = self.entity:GetPosition()
		self.entity:SetPosition(Position + Vector3(self.Properties.Speed * dt, 0, 0))
	end
end
return Mover
)";
} // namespace

E_TEST(ScriptProperties_OverridesRoundTrip)
{
	FScriptValueMap Overrides;
	Overrides["Speed"]   = FScriptValue::MakeNumber(2.5);
	Overrides["Count"]   = FScriptValue::MakeNumber(7, true);
	Overrides["Enabled"] = FScriptValue::MakeBool(false);
	Overrides["Label"]   = FScriptValue::MakeString("한글 라벨");
	Overrides["Offset"]  = FScriptValue::MakeVector3(FVector3(1.0f, -2.0f, 3.5f));

	const std::string     Json   = FScriptProperties::SerializeOverrides(Overrides);
	const FScriptValueMap Parsed = FScriptProperties::ParseOverrides(Json);
	E_EXPECT_EQ(Parsed.size(), Overrides.size());
	for (const auto& [Name, Value] : Overrides)
	{
		const auto Found = Parsed.find(Name);
		E_EXPECT_TRUE(Found != Parsed.end() && Found->second == Value);
	}

	E_EXPECT_TRUE(FScriptProperties::SerializeOverrides({}).empty());
	E_EXPECT_TRUE(FScriptProperties::ParseOverrides("").empty());
	E_EXPECT_TRUE(FScriptProperties::ParseOverrides("{ 잘못된 json").empty());
}

E_TEST(ScriptSystem_PropertyDeclsSorted)
{
	WriteScript("Scripts/Mover.lua", GMoverScript);
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());

	const std::vector<FScriptPropertyDecl>* Decls = Scripts.GetPropertyDecls("Scripts/Mover.lua");
	E_EXPECT_TRUE(Decls != nullptr);
	if (Decls == nullptr)
	{
		return;
	}
	E_EXPECT_EQ(Decls->size(), static_cast<size_t>(5));
	E_EXPECT_TRUE((*Decls)[0].Name == "Count" && (*Decls)[0].Default.bInteger);
	E_EXPECT_TRUE((*Decls)[1].Name == "Enabled" && (*Decls)[1].Default.Type == EScriptValueType::Bool);
	E_EXPECT_TRUE((*Decls)[2].Name == "Label" && (*Decls)[2].Default.String == "mover");
	E_EXPECT_TRUE((*Decls)[3].Name == "Offset" && (*Decls)[3].Default.Vector == FVector3(1.0f, 2.0f, 3.0f));
	E_EXPECT_TRUE((*Decls)[4].Name == "Speed" && !(*Decls)[4].Default.bInteger);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
}

E_TEST(ScriptSystem_UpdateMovesEntityWithOverrides)
{
	WriteScript("Scripts/Mover.lua", GMoverScript);
	FScene        Scene;
	const FEntity Default    = AddScriptedEntity(Scene, "Default", "Scripts/Mover.lua");
	const FEntity Overridden = AddScriptedEntity(Scene, "Overridden", "Scripts/Mover.lua", R"({"Speed": 10, "Label": "x"})");

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());
	Scripts.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 4; ++Frame) // 0.25초 × 4 = 1초 (MaxDeltaSeconds 이내)
	{
		Scripts.Update(0.25f, nullptr);
	}
	Scripts.Update(0.0f, nullptr);

	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(2));
	E_EXPECT_EQUALS(Scene.GetTransform(Default).Position, FVector3(100.0f, 0.0f, 0.0f), Tol);
	E_EXPECT_EQUALS(Scene.GetTransform(Overridden).Position, FVector3(10.0f, 0.0f, 0.0f), Tol);

	// 큰 델타는 MaxDeltaSeconds로 제한된다
	const FVector3 BeforeClamp = Scene.GetTransform(Default).Position;
	Scripts.Update(10.0f, nullptr);
	E_EXPECT_NEAR(Scene.GetTransform(Default).Position.X - BeforeClamp.X, 100.0f * FScriptSystem::MaxDeltaSeconds, Tol);
	Scene.GetTransform(Default).Position = BeforeClamp;

	// 오버라이드 값은 선언 타입(실수)을 따르고, 나머지는 기본값
	const FScriptValue Speed = Scripts.GetInstanceProperty(Overridden, "Speed");
	E_EXPECT_TRUE(Speed.Type == EScriptValueType::Number && !Speed.bInteger && Speed.Number == 10.0);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Overridden, "Label").String == "x");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Default, "Count").bInteger);

	// 인스턴스마다 Vector3 기본값 복사본을 가진다 (공유 userdata 아님)
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('Default'):GetScript().Started == true)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}

E_TEST(ScriptSystem_ReflectionBindingReadWrite)
{
	WriteScript("Scripts/Reflect.lua", R"(
local Reflect = {}
function Reflect:OnStart()
	local Transform = self.entity:GetComponent("TransformComponent")
	Transform.Position = Vector3(1, 2, 3)
	Transform.Scale = Transform.Scale * 2
	local Light = self.entity:GetComponent("DirectionalLight")  -- 접미사 생략 허용
	Light.Intensity = 7.5
	Light.Color = Vector3(0.5, 0.25, 1)
	assert(self.entity:GetComponent("StaticMeshComponent") == nil)
	assert(self.entity:HasComponent("DirectionalLightComponent"))
	self.entity:SetName("Renamed")
end
return Reflect
)");
	FScene        Scene;
	const FEntity Entity = AddScriptedEntity(Scene, "Reflect", "Scripts/Reflect.lua");
	Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Entity);

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.016f, nullptr);

	const FTransformComponent&        Transform = Scene.GetTransform(Entity);
	const FDirectionalLightComponent& Light     = Scene.GetRegistry().Get<FDirectionalLightComponent>(Entity);
	E_EXPECT_EQUALS(Transform.Position, FVector3(1.0f, 2.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(Transform.Scale, FVector3(2.0f, 2.0f, 2.0f), Tol);
	E_EXPECT_NEAR(Light.Intensity, 7.5f, Tol);
	E_EXPECT_EQUALS(Light.Color, FVector3(0.5f, 0.25f, 1.0f), Tol);
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FNameComponent>(Entity).Name == "Renamed");
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 잘못된 쓰기는 Lua 오류 (크래시 없음): 없는 프로퍼티, 타입 불일치
	const uint32 ErrorsBefore = Scripts.GetErrorCount();
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Renamed'):GetComponent('Transform').Nope = 1"));
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Renamed'):GetComponent('Transform').Position = 5"));
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Renamed'):GetComponent('NoSuchComponent')"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), ErrorsBefore + 3);
	Scripts.EndPlay();
}

E_TEST(ScriptSystem_ErrorsDoNotCrashAndFaultOnce)
{
	WriteScript("Scripts/Broken.lua", R"(
local Broken = {}
function Broken:OnUpdate(dt)
	local Missing = nil
	Missing.Field = 1 -- 런타임 오류
end
return Broken
)");
	WriteScript("Scripts/Syntax.lua", "local X = { \n return X");
	WriteScript("Scripts/NoTable.lua", "return 42");

	FScene Scene;
	AddScriptedEntity(Scene, "Broken", "Scripts/Broken.lua");
	AddScriptedEntity(Scene, "Syntax", "Scripts/Syntax.lua");
	AddScriptedEntity(Scene, "NoTable", "Scripts/NoTable.lua");
	AddScriptedEntity(Scene, "Missing", "Scripts/DoesNotExist.lua");

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.016f, nullptr);
	const uint32 ErrorsAfterFirst = Scripts.GetErrorCount();
	E_EXPECT_EQ(ErrorsAfterFirst, 4u); // 런타임 1 + 문법 1 + 테이블 아님 1 + 파일 없음 1

	// 오류 난 인스턴스는 멈추므로 같은 오류를 반복하지 않는다
	Scripts.Update(0.016f, nullptr);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQ(Scripts.GetErrorCount(), ErrorsAfterFirst);
	Scripts.EndPlay();
}

E_TEST(ScriptSystem_HotReloadReplacesCodeAndKeepsOldOnFailure)
{
	WriteScript("Scripts/Reload.lua", R"(
local Reload = { Properties = { Step = 1.0 } }
function Reload:OnUpdate(dt)
	local P = self.entity:GetPosition()
	self.entity:SetPosition(P + Vector3(self.Properties.Step, 0, 0))
end
return Reload
)");
	FScene        Scene;
	const FEntity Entity = AddScriptedEntity(Scene, "Reload", "Scripts/Reload.lua");

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_NEAR(Scene.GetTransform(Entity).Position.X, 1.0f, Tol);

	// 문법 오류로 저장 → 기존 코드 유지
	WriteScript("Scripts/Reload.lua", "local Reload = {");
	E_EXPECT_FALSE(Scripts.ReloadScript(GetTestContentDirectory() / L"Scripts/Reload.lua"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_NEAR(Scene.GetTransform(Entity).Position.X, 2.0f, Tol);

	// 정상 코드로 저장 → 인스턴스 상태(Properties)는 유지하고 새 함수 사용, 새 프로퍼티 기본값 추가
	WriteScript("Scripts/Reload.lua", R"(
local Reload = { Properties = { Step = 1.0, Up = 5.0 } }
function Reload:OnUpdate(dt)
	local P = self.entity:GetPosition()
	self.entity:SetPosition(P + Vector3(0, 0, self.Properties.Up))
end
return Reload
)");
	E_EXPECT_TRUE(Scripts.ReloadScript(GetTestContentDirectory() / L"Scripts/Reload.lua"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQUALS(Scene.GetTransform(Entity).Position, FVector3(2.0f, 0.0f, 5.0f), Tol);

	// 에디터용 선언도 갱신된다
	const std::vector<FScriptPropertyDecl>* Decls = Scripts.GetPropertyDecls("Scripts/Reload.lua");
	E_EXPECT_TRUE(Decls != nullptr && Decls->size() == 2);
	Scripts.EndPlay();
}

E_TEST(ScriptSystem_DeferredDestroyAndLifecycle)
{
	WriteScript("Scripts/Life.lua", R"(
local Life = {}
function Life:OnStart()
	Started = (Started or 0) + 1
	if self.entity:GetName() == "Doomed" then
		self.entity:Destroy()
		assert(self.entity:IsValid()) -- 파괴는 프레임 끝까지 지연
	end
end
function Life:OnDestroy()
	Destroyed = (Destroyed or 0) + 1
end
return Life
)");
	FScene        Scene;
	const FEntity Doomed = AddScriptedEntity(Scene, "Doomed", "Scripts/Life.lua");
	const FEntity Child  = Scene.CreateEntity("Child");
	Scene.SetParent(Child, Doomed);
	const FEntity Survivor = AddScriptedEntity(Scene, "Survivor", "Scripts/Life.lua");

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTestContentDirectory());
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.016f, nullptr);

	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Doomed));
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Child));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Survivor));
	E_EXPECT_TRUE(Scripts.ConsumeSceneStructureChanged());
	E_EXPECT_TRUE(Scripts.RunString("assert(Started == 2 and Destroyed == 1)"));
	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(1));

	// 스크립트가 만든 엔티티에 스크립트를 붙이면 다음 프레임부터 실행된다
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Spawned = Scene.Create("Spawned")
Spawned:AddComponent("ScriptComponent").ScriptAsset = "Scripts/Life.lua"
)"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(2));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	Scripts.EndPlay(); // 남은 인스턴스 OnDestroy
	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(0));
}

E_TEST(SceneCloner_CloneMatchesSourceAndIsIndependent)
{
	FScene        Source;
	const FEntity Sun = Source.CreateEntity("Sun");
	Source.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 500.0f);
	Source.GetRegistry().Emplace<FDirectionalLightComponent>(Sun).Intensity = 3.0f;

	// 중간 엔티티를 지워 인덱스에 구멍을 만든다 (원본과 복제본의 인덱스가 달라도 참조가 맞아야 함)
	const FEntity Temp = Source.CreateEntity("Temp");
	const FEntity Root = Source.CreateEntity("Root");
	Source.DestroyEntity(Temp);
	const FEntity ChildA = Source.CreateEntity("ChildA");
	const FEntity ChildB = Source.CreateEntity("ChildB");
	Source.SetParent(ChildA, Root);
	Source.SetParent(ChildB, Root);
	Source.GetTransform(ChildB).Rotation = FQuat::FromEuler(10.0f, 20.0f, 30.0f);
	FScriptComponent& Script  = Source.GetRegistry().Emplace<FScriptComponent>(ChildA);
	Script.ScriptAsset        = "Scripts/Mover.lua";
	Script.PropertyOverrides  = R"({"Speed":2})";
	Source.GetRegistry().Emplace<FCameraComponent>(Root).FovYDegrees = 75.0f;
	const FEntity Generated = Source.CreateEntity("Generated");
	Source.GetRegistry().Emplace<FTransientComponent>(Generated);
	Source.SetParent(Generated, ChildB);
	Source.UpdateTransforms();

	FScene                    Clone;
	FSceneCloner::FEntityMap  Map;
	Clone.CreateEntity("Leftover"); // 기존 내용은 비워져야 한다
	FSceneCloner::Clone(Source, Clone, &Map);

	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Source) == FSceneSerializer::ToJsonString(Clone));
	E_EXPECT_EQ(Clone.GetRegistry().GetAliveCount(), Source.GetRegistry().GetAliveCount());

	// Transient 엔티티도 복제되고 계층이 다시 매핑된다
	const FEntity CloneGenerated = Map[Generated.ToId()];
	E_EXPECT_TRUE(Clone.GetRegistry().Has<FTransientComponent>(CloneGenerated));
	E_EXPECT_TRUE(Clone.GetParent(CloneGenerated) == Map[ChildB.ToId()]);
	E_EXPECT_EQ(Clone.GetChildren(Map[Root.ToId()]).size(), static_cast<size_t>(2));
	E_EXPECT_EQUALS(Clone.GetTransform(CloneGenerated).WorldMatrix, Source.GetTransform(Generated).WorldMatrix, Tol);

	// 독립성: 복제본 수정/파괴가 원본에 영향 없음
	Clone.GetTransform(Map[Sun.ToId()]).Position = FVector3(1.0f, 1.0f, 1.0f);
	Clone.DestroyEntity(Map[Root.ToId()]);
	E_EXPECT_EQUALS(Source.GetTransform(Sun).Position, FVector3(0.0f, 0.0f, 500.0f), Tol);
	E_EXPECT_TRUE(Source.GetRegistry().IsValid(ChildA));
	E_EXPECT_EQ(Source.GetChildren(Root).size(), static_cast<size_t>(2));
}
