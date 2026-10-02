#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

// Script.Require 규칙 (Scripting/ScriptModuleBindings.cpp 머리 주석)
namespace
{
	namespace fs = std::filesystem;

	fs::path GetModuleContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectEScriptModuleTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"Scripts/Lib");
			return Path;
		}();
		return Directory;
	}

	void WriteFile(const wchar_t* RelativePath, const char* Source)
	{
		std::ofstream(GetModuleContent() / RelativePath, std::ios::binary | std::ios::trunc) << Source;
	}

	FEntity AddScripted(FScene& Scene, const char* Name, const char* ScriptAsset)
	{
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = ScriptAsset;
		return Entity;
	}
} // namespace

E_TEST(ScriptModule_RequireCachesPerStateAndSharesValue)
{
	WriteFile(L"Scripts/Lib/Items.lua", R"(
ItemsLoads = (ItemsLoads or 0) + 1
local Items = { Sword = { Damage = 12 } }
function Items.Get(Id) return Items[Id] end
return Items
)");
	// 같은 모듈을 대소문자/구분자를 바꿔 불러도 같은 캐시
	WriteFile(L"Scripts/UserA.lua", R"(
local Items = Script.Require("Scripts/Lib/Items.lua")
local A = { Properties = {} }
function A:OnStart() SharedA = Items; DamageA = Items.Get("Sword").Damage end
return A
)");
	WriteFile(L"Scripts/UserB.lua", R"(
local Items = Script.Require("scripts\\lib\\ITEMS.lua")
local B = { Properties = {} }
function B:OnStart() SharedB = Items; Flag = Script.Require("Scripts/Lib/NoReturn.lua") end
return B
)");
	WriteFile(L"Scripts/Lib/NoReturn.lua", "NoReturnRan = true");

	FScene Scene;
	AddScripted(Scene, "A", "Scripts/UserA.lua");
	AddScripted(Scene, "B", "Scripts/UserB.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetModuleContent());
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(ItemsLoads == 1, 'loads ' .. tostring(ItemsLoads))"));
	E_EXPECT_TRUE(Scripts.RunString("assert(SharedA == SharedB and DamageA == 12)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Flag == true and NoReturnRan == true)")); // nil 반환 → true
	// 표준 모듈 함수는 계속 막혀 있다
	E_EXPECT_TRUE(Scripts.RunString("assert(require == nil and package == nil and dofile == nil and loadfile == nil)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();

	// 새 Lua 상태(다음 플레이)에서는 다시 실행된다
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(ItemsLoads == 1)")); // 전역도 새 상태
	Scripts.EndPlay();
}

E_TEST(ScriptModule_ErrorsAndCyclesBecomeLuaErrors)
{
	WriteFile(L"Scripts/Lib/Broken.lua", "error('모듈 안 오류')");
	WriteFile(L"Scripts/Lib/CycleA.lua", "return { B = Script.Require('Scripts/Lib/CycleB.lua') }");
	WriteFile(L"Scripts/Lib/CycleB.lua", "return { A = Script.Require('Scripts/Lib/CycleA.lua') }");
	WriteFile(L"Scripts/Probe.lua", R"(
local Probe = { Properties = {} }
function Probe:OnStart()
	local Ok, Err = pcall(Script.Require, "Scripts/Lib/Broken.lua")
	BrokenOk, BrokenErr = Ok, tostring(Err)
	Ok, Err = pcall(Script.Require, "Scripts/Lib/Missing.lua")
	MissingOk, MissingErr = Ok, tostring(Err)
	Ok, Err = pcall(Script.Require, "Scripts/Lib/CycleA.lua")
	CycleOk, CycleErr = Ok, tostring(Err)
	Ok, Err = pcall(Script.Require, "../Outside.lua")
	OutsideOk = Ok
	-- 실패는 캐시하지 않는다: 다시 불러도 같은 오류
	Ok = pcall(Script.Require, "Scripts/Lib/Broken.lua")
	BrokenAgainOk = Ok
end
return Probe
)");
	FScene Scene;
	AddScripted(Scene, "Probe", "Scripts/Probe.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetModuleContent());
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(BrokenOk == false and BrokenErr:find('모듈 안 오류', 1, true) and BrokenErr:find('Broken.lua', 1, true))"));
	E_EXPECT_TRUE(Scripts.RunString("assert(MissingOk == false and MissingErr:find('Missing.lua', 1, true))"));
	E_EXPECT_TRUE(Scripts.RunString("assert(CycleOk == false and CycleErr:find('순환', 1, true), CycleErr)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(OutsideOk == false and BrokenAgainOk == false)"));
	// 순환 뒤에도 정상 모듈은 다시 불러올 수 있다 (실패한 항목이 '로드 중'으로 남지 않음)
	WriteFile(L"Scripts/Lib/CycleB.lua", "return { Fixed = true }");
	E_EXPECT_TRUE(Scripts.RunString("assert(Script.Require('Scripts/Lib/CycleA.lua').B.Fixed == true)"));
	Scripts.EndPlay();
}

E_TEST(ScriptModule_HotReloadUpdatesDependentClasses)
{
	WriteFile(L"Scripts/Lib/Config.lua", "return { Step = 1 }");
	WriteFile(L"Scripts/Lib/Derived.lua", "local C = Script.Require('Scripts/Lib/Config.lua'); return { Step2 = C.Step * 2 }");
	WriteFile(L"Scripts/Mover.lua", R"(
local Config  = Script.Require("Scripts/Lib/Config.lua")
local Derived = Script.Require("Scripts/Lib/Derived.lua")
local Mover = { Properties = {} }
function Mover:OnUpdate(dt)
	local P = self.entity:GetPosition()
	self.entity:SetPosition(P + Vector3(Config.Step, Derived.Step2, 0))
end
return Mover
)");
	FScene        Scene;
	const FEntity Entity = AddScripted(Scene, "Mover", "Scripts/Mover.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetModuleContent());
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQUALS(Scene.GetTransform(Entity).Position, FVector3(1.0f, 2.0f, 0.0f), 1.0e-4f);

	// 문법 오류 → 기존 값 유지
	WriteFile(L"Scripts/Lib/Config.lua", "return { Step = ");
	E_EXPECT_FALSE(Scripts.ReloadScript(GetModuleContent() / L"Scripts/Lib/Config.lua"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQUALS(Scene.GetTransform(Entity).Position, FVector3(2.0f, 4.0f, 0.0f), 1.0e-4f);

	// 정상 → 모듈 값 교체 + 의존 모듈(Derived) 다시 실행 + 의존 클래스(Mover) 다시 실행 (파일 맨 위 local이 새 값)
	WriteFile(L"Scripts/Lib/Config.lua", "return { Step = 10 }");
	E_EXPECT_TRUE(Scripts.ReloadScript(GetModuleContent() / L"Scripts/Lib/Config.lua"));
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQUALS(Scene.GetTransform(Entity).Position, FVector3(12.0f, 24.0f, 0.0f), 1.0e-4f);
	E_EXPECT_TRUE(Scripts.RunString("assert(Script.Require('Scripts/Lib/Derived.lua').Step2 == 20)"));
	Scripts.EndPlay();
}
