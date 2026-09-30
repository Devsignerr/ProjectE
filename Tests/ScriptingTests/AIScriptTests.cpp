#include "AI/AIComponents.h"
#include "AI/AIModule.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

// Lua ↔ AI: 블랙보드/이동/경로 API, Lua 태스크·데코레이터·서비스 노드 (FGameWorld가 훅을 연결한다)
namespace
{
	constexpr float Dt = 0.1f;

	FBTNodeDesc Node(const char* Type, std::vector<FBTParam> Params = {}, std::vector<FBTNodeDesc> Children = {})
	{
		FBTNodeDesc Desc;
		Desc.Type     = Type;
		Desc.Params   = std::move(Params);
		Desc.Children = std::move(Children);
		return Desc;
	}

	FBTParam Param(const char* Name, FBTParamValue Value) { return { Name, std::move(Value) }; }
	FBTParam Text(const char* Name, const char* Value) { return { Name, FBTParamValue(std::string(Value)) }; }

	FBTNodeDesc Hold() { return Node("Wait", { Param("WaitTime", 1000.0f) }); } // 루트 재시작을 막는다

	struct FAIScriptFixture
	{
		std::filesystem::path Content;
		FScene                Scene;
		FScriptSystem         Scripts;
		FGameWorld            World;

		explicit FAIScriptFixture(const wchar_t* Name)
		{
			RegisterAITypes();
			Content = FTestRegistry::GetTempDirectory() / L"AIScript" / Name;
			std::filesystem::create_directories(Content / L"AI");
		}

		~FAIScriptFixture() { World.EndPlay(); }

		void WriteText(const wchar_t* RelativePath, const char* Text)
		{
			std::ofstream File(Content / RelativePath, std::ios::binary | std::ios::trunc);
			File << Text;
		}

		void WriteTree(const wchar_t* RelativePath, std::vector<FBlackboardKeyDesc> Keys, FBTNodeDesc Root)
		{
			FBehaviorTreeAsset Asset;
			Asset.BlackboardKeys = std::move(Keys);
			Asset.Root           = std::move(Root);
			E_EXPECT_TRUE(Asset.SaveToFile(Content / RelativePath));
		}

		FEntity AddAgent(const char* TreeAsset, const char* ScriptAsset = nullptr)
		{
			const FEntity Agent = Scene.CreateEntity("Agent");
			Scene.GetRegistry().Emplace<FBehaviorTreeComponent>(Agent).Asset = TreeAsset;
			if (ScriptAsset != nullptr)
			{
				Scene.GetRegistry().Emplace<FScriptComponent>(Agent).ScriptAsset = ScriptAsset;
			}
			Scene.UpdateTransforms();
			return Agent;
		}

		void Play(ENetMode Mode = ENetMode::Standalone)
		{
			World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
			World.BeginPlay(Scene, Mode);
		}

		void Tick(int32 Frames)
		{
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				World.TickGameplay(Dt, nullptr);
			}
		}

		FBlackboard* Blackboard(FEntity Agent)
		{
			FBehaviorTreeInstance* Tree = World.GetAI().FindTree(Agent);
			return Tree ? &Tree->GetBlackboard() : nullptr;
		}
	};
} // namespace

// 스크립트가 블랙보드에 목표를 쓰면 MoveTo가 움직이고, 트리가 쓴 값을 스크립트가 읽는다
E_TEST(AIScript_LuaBlackboardDrivesMoveTo)
{
	FAIScriptFixture Fixture(L"Blackboard");
	Fixture.WriteTree(L"AI/Move.ebt", { { "Goal", EBlackboardKeyType::Vector }, { "Done", EBlackboardKeyType::Bool } },
		Node("Sequence", {}, { Node("MoveTo", { Text("TargetKey", "Goal") }), Node("SetBlackboard", { Text("Key", "Done"), Text("Value", "true") }), Hold() }));
	Fixture.WriteText(L"AI/Brain.lua",
		"local B = { Properties = { SawDone = false } }\n"
		"function B:OnStart() self.bb = self.entity:GetBlackboard(); assert(self.bb:Set('Goal', Vector3(300, 0, 0))) end\n"
		"function B:OnUpdate(dt) self.Properties.SawDone = self.bb:Get('Done') == true end\n"
		"return B\n");
	const FEntity Agent = Fixture.AddAgent("AI/Move.ebt", "AI/Brain.lua");
	Fixture.Play();

	Fixture.Tick(15); // 300cm / (300cm/s × 0.1s) = 10프레임 + 시작
	E_EXPECT_TRUE(Fixture.Blackboard(Agent) != nullptr && Fixture.Blackboard(Agent)->Get<bool>("Done").value_or(false));
	E_EXPECT_TRUE(Fixture.Scene.GetTransform(Agent).GetWorldPosition().X >= 280.0f - 1.0e-3f);
	Fixture.Tick(1);
	const FScriptValue Saw = Fixture.Scripts.GetInstanceProperty(Agent, "SawDone");
	E_EXPECT_TRUE(Saw.Type == EScriptValueType::Bool && Saw.bBool);
	E_EXPECT_EQ(Fixture.Scripts.GetErrorCount(), 0u);
}

// Lua 태스크: OnExecute → OnTick이 nil이면 계속, "Success"로 끝남. Properties 오버라이드(JSON) 반영
E_TEST(AIScript_LuaTaskRunsUntilSuccess)
{
	FAIScriptFixture Fixture(L"Task");
	Fixture.WriteText(L"AI/Count.lua",
		"local T = { Properties = { Target = 3 } }\n"
		"function T:OnExecute() self.count = 0 return 'Running' end\n"
		"function T:OnTick(dt) self.count = self.count + 1\n"
		"  if self.count >= self.Properties.Target then self.entity:GetBlackboard():Set('Count', self.count) return 'Success' end end\n"
		"return T\n");
	Fixture.WriteTree(L"AI/Task.ebt", { { "Count", EBlackboardKeyType::Int } },
		Node("Sequence", {}, { Node("LuaTask", { Text("Script", "AI/Count.lua"), Text("Properties", "{\"Target\": 5}") }), Hold() }));
	const FEntity Agent = Fixture.AddAgent("AI/Task.ebt");
	Fixture.Play();

	Fixture.Tick(4);
	E_EXPECT_FALSE(Fixture.Blackboard(Agent)->IsSet("Count"));
	Fixture.Tick(2);
	E_EXPECT_EQ(Fixture.Blackboard(Agent)->Get<int32>("Count").value_or(-1), 5);
	E_EXPECT_EQ(Fixture.Scripts.GetErrorCount(), 0u);
}

// Lua 데코레이터: 관찰 키가 바뀌면 LowerPriority 중단으로 높은 우선순위 가지로 옮겨 간다
E_TEST(AIScript_LuaDecoratorObservesKeys)
{
	FAIScriptFixture Fixture(L"Decorator");
	Fixture.WriteText(L"AI/Enabled.lua",
		"local D = {}\n"
		"function D:CanExecute() return self.entity:GetBlackboard():Get('Enabled') == true end\n"
		"return D\n");
	FBTNodeDesc First = Node("Sequence", {}, { Node("SetBlackboard", { Text("Key", "First"), Text("Value", "true") }), Hold() });
	First.Decorators.push_back(Node("LuaDecorator", { Text("Script", "AI/Enabled.lua"), Text("ObservedKeys", "Enabled"), Text("AbortMode", "LowerPriority") }));
	FBTNodeDesc Fallback = Node("Sequence", {}, { Node("SetBlackboard", { Text("Key", "Fallback"), Text("Value", "true") }), Hold() });
	Fixture.WriteTree(L"AI/Decorator.ebt",
		{ { "Enabled", EBlackboardKeyType::Bool }, { "First", EBlackboardKeyType::Bool }, { "Fallback", EBlackboardKeyType::Bool } },
		Node("Selector", {}, { First, Fallback }));
	const FEntity Agent = Fixture.AddAgent("AI/Decorator.ebt");
	Fixture.Play();

	Fixture.Tick(2);
	E_EXPECT_TRUE(Fixture.Blackboard(Agent)->Get<bool>("Fallback").value_or(false));
	E_EXPECT_FALSE(Fixture.Blackboard(Agent)->IsSet("First"));

	Fixture.Blackboard(Agent)->SetBool("Enabled", true);
	Fixture.Tick(2);
	E_EXPECT_TRUE(Fixture.Blackboard(Agent)->Get<bool>("First").value_or(false));
	E_EXPECT_EQ(Fixture.Scripts.GetErrorCount(), 0u);
}

// Lua 서비스: 붙은 가지가 활성인 동안 Interval마다 OnTick
E_TEST(AIScript_LuaServiceTicksOnInterval)
{
	FAIScriptFixture Fixture(L"Service");
	Fixture.WriteText(L"AI/Ticker.lua",
		"local S = {}\n"
		"function S:OnBecomeRelevant() self.entity:GetBlackboard():Set('Ticks', 0) end\n"
		"function S:OnTick(dt) local bb = self.entity:GetBlackboard(); bb:Set('Ticks', bb:Get('Ticks') + 1) end\n"
		"return S\n");
	FBTNodeDesc Root = Node("Sequence", {}, { Hold() });
	Root.Services.push_back(Node("LuaService", { Text("Script", "AI/Ticker.lua"), Param("Interval", 0.2f) }));
	Fixture.WriteTree(L"AI/Service.ebt", { { "Ticks", EBlackboardKeyType::Int } }, Root);
	const FEntity Agent = Fixture.AddAgent("AI/Service.ebt");
	Fixture.Play();

	Fixture.Tick(11); // 1.1초 / 0.2초 간격 → 5번
	const int32 Ticks = Fixture.Blackboard(Agent)->Get<int32>("Ticks").value_or(-1);
	E_EXPECT_TRUE(Ticks >= 4 && Ticks <= 6);
	E_EXPECT_EQ(Fixture.Scripts.GetErrorCount(), 0u);
}

// AI.FindPath / entity:MoveTo / GetMoveStatus, 클라이언트 역할은 AI를 돌리지 않는다
E_TEST(AIScript_PathAndMoveApiAndClientRole)
{
	FAIScriptFixture Fixture(L"Api");
	Fixture.WriteText(L"AI/Mover.lua",
		"local M = { Properties = { PathCount = 0, Status = '', Later = '' } }\n"
		"function M:OnStart()\n"
		"  local Path = AI.FindPath(Vector3(0, 0, 0), Vector3(100, 0, 0))\n"
		"  self.Properties.PathCount = #Path\n"
		"  self.Properties.Status = self.entity:MoveTo(Vector3(60, 0, 0))\n"
		"end\n"
		"function M:OnUpdate(dt) self.Properties.Later = self.entity:GetMoveStatus() end\n"
		"return M\n");
	const FEntity Mover = Fixture.Scene.CreateEntity("Mover");
	Fixture.Scene.GetRegistry().Emplace<FScriptComponent>(Mover).ScriptAsset = "AI/Mover.lua";
	Fixture.Scene.UpdateTransforms();
	Fixture.Play();

	Fixture.Tick(5);
	const FScriptValue PathCount = Fixture.Scripts.GetInstanceProperty(Mover, "PathCount");
	E_EXPECT_NEAR(PathCount.Number, 2.0, 1.0e-6); // 내비메시 없음 → 직선
	E_EXPECT_TRUE(Fixture.Scripts.GetInstanceProperty(Mover, "Status").String == "Moving");
	E_EXPECT_TRUE(Fixture.Scripts.GetInstanceProperty(Mover, "Later").String == "Succeeded");
	E_EXPECT_EQ(Fixture.Scripts.GetErrorCount(), 0u);

	// 클라이언트: 트리를 만들지 않는다
	FAIScriptFixture Client(L"Client");
	Client.WriteTree(L"AI/Idle.ebt", {}, Node("Sequence", {}, { Hold() }));
	const FEntity Agent = Client.AddAgent("AI/Idle.ebt");
	Client.Play(ENetMode::Client);
	Client.Tick(2);
	E_EXPECT_TRUE(Client.World.GetAI().FindTree(Agent) == nullptr);
}
