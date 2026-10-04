// 게임 시간 배율 (World/GameWorld.cpp "시간 배율"), 마우스 커서 Lua API (ScriptCameraBindings.cpp), 2D 넉백 Lua API:
//   배율 0.5 = 물리·이동기·타이머가 절반 속도, 0 = 정지 후 재개하면 멈추지 않은 월드와 같은 상태, 히트스톱 = 실제 시간 동안 정지,
//   비배율 타이머/WaitUnscaled/Time.UnscaledDeltaTime, 네트워크 세션에서는 거절, Input.GetMousePosition/GetMouseUIPosition/IsMouseOverUI,
//   Game.SetCursorVisible, entity:LaunchCharacter/AddKnockback/IsStunned (2D·3D), 게임 모듈 시간 배율 API(IGameNet), UI 애니메이션 게임 시간
#include "Core/Input.h"
#include "Core/InputMode.h"
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"
#include "World/GameWorld.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

namespace
{
	namespace fs = std::filesystem;
	constexpr float Step = 1.0f / 60.0f;

	fs::path GetContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectETimeScaleTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"Scripts");
			fs::create_directories(Path / L"UI");
			// 프레임 기록: 게임 시간 0.5초 타이머 / 실제 시간 0.5초 타이머 / Wait(0.5) / WaitUnscaled(0.5)가 끝난 프레임
			std::ofstream(Path / L"Scripts/Clock.lua", std::ios::binary | std::ios::trunc) << R"(
local T = { Properties = { Frame = 0, Scaled = -1, Unscaled = -1, Wait = -1, WaitU = -1, Total = 0.0, UTotal = 0.0, Scale = 1.0, HitStop = "" } }
function T:OnStart()
	local P = self.Properties
	Timer.After(0.5, function() P.Scaled = P.Frame end)
	Timer.After(0.5, function() P.Unscaled = P.Frame end, { Unscaled = true })
	Coroutine.Start(function() Wait(0.5); P.Wait = P.Frame end)
	Coroutine.Start(function() WaitUnscaled(0.5); P.WaitU = P.Frame end)
end
function T:OnUpdate(dt)
	local P = self.Properties
	P.Frame = P.Frame + 1
	P.Total = Time.TotalTime
	P.UTotal = Time.UnscaledTotalTime
	P.Scale = Time.TimeScale
end
return T
)";
			std::ofstream(Path / L"Scripts/HitStop.lua", std::ios::binary | std::ios::trunc) << R"(
local T = { Properties = { Frame = 0, HitStop = "", HitStopOk = false } }
function T:OnUpdate(dt)
	local P = self.Properties
	P.Frame = P.Frame + 1
	if P.Frame == 10 then P.HitStopOk = Game.HitStop(0.05) end
	if P.Frame >= 10 and P.Frame <= 15 then P.HitStop = P.HitStop .. string.format("%g;", Time.GetTimeScale()) end
end
return T
)";
			return Path;
		}();
		return Directory;
	}

	// 2D 레벨: 바닥 + 떨어지는 동적 원 + 오른쪽으로 달리는 2D 캐릭터 + 시계 스크립트
	struct FTimeWorld
	{
		FScene        Scene;
		FScriptSystem Scripts;
		FGameWorld    World;
		FEntity       Ball, Runner, Clock;

		explicit FTimeWorld(ENetMode Mode = ENetMode::Standalone, const char* ClockScript = "Scripts/Clock.lua")
		{
			RegisterPhysicsTypes();
			const FEntity Floor = Scene.CreateEntity("Floor");
			Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
			Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(100000.0f, 100.0f);
			Ball = Scene.CreateEntity("Ball");
			Scene.GetTransform(Ball).Position = FVector3(-500.0f, 0.0f, 3000.0f);
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Ball);
			Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Ball);
			Runner = Scene.CreateEntity("Runner");
			Scene.GetTransform(Runner).Position = FVector3(0.0f, 0.0f, 61.0f);
			Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Runner);
			Clock = Scene.CreateEntity("Clock");
			Scene.GetRegistry().Emplace<FScriptComponent>(Clock).ScriptAsset = ClockScript;
			Scene.UpdateTransforms();
			World.Init({ &Scripts, nullptr, nullptr, nullptr, GetContent() });
			World.BeginPlay(Scene, Mode);
		}
		~FTimeWorld() { World.EndPlay(); }
		void Tick(bool bRun = true)
		{
			if (bRun)
			{
				World.GetCharacters2D().AddMovementInput(Runner, FVector3(1.0f, 0.0f, 0.0f));
			}
			World.TickGameplay(Step, nullptr);
			World.TickPresentation(Scene, Step);
		}
		float  BallZ() const { return Scene.GetTransform(Ball).Position.Z; }
		float  RunnerX() const { return Scene.GetTransform(Runner).Position.X; }
		double Prop(const char* Name) { return Scripts.GetInstanceProperty(Clock, Name).Number; }
	};
} // namespace

// 배율 0.5: 실제 120프레임 = 게임 1초 → 배율 1의 60프레임과 같은 곳. 게임 시간 타이머/Wait는 늦게, 실제 시간 타이머/WaitUnscaled는 그대로
E_TEST(TimeScale_HalfSpeedPhysicsMoverAndTimers)
{
	FTimeWorld Normal;
	FTimeWorld Half;
	E_EXPECT_TRUE(Half.World.SetTimeScale(0.5f));
	E_EXPECT_NEAR(Half.World.GetTimeScale(), 0.5f, 0.0f);
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Normal.Tick();
	}
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Half.Tick();
	}
	const float NormalDrop = 3000.0f - Normal.BallZ();
	const float HalfDrop   = 3000.0f - Half.BallZ();
	E_EXPECT_TRUE(NormalDrop > 300.0f);
	E_EXPECT_NEAR(HalfDrop, NormalDrop, NormalDrop * 0.05f); // 고정 스텝 경계 한 칸 이내
	E_EXPECT_TRUE(Normal.RunnerX() > 400.0f);
	E_EXPECT_NEAR(Half.RunnerX(), Normal.RunnerX(), Normal.RunnerX() * 0.03f); // 무브 dt가 절반이라 적분만 약간 다르다
	// 시계: 배율 1 → 둘 다 약 30프레임, 배율 0.5 → 게임 시간 쪽만 약 60프레임
	E_EXPECT_NEAR(Normal.Prop("Scaled"), 31.0, 1.0);
	E_EXPECT_NEAR(Normal.Prop("Unscaled"), 31.0, 1.0);
	E_EXPECT_NEAR(Half.Prop("Scaled"), 61.0, 1.0);
	E_EXPECT_NEAR(Half.Prop("Wait"), 61.0, 1.0);
	E_EXPECT_NEAR(Half.Prop("Unscaled"), 31.0, 1.0);
	E_EXPECT_NEAR(Half.Prop("WaitU"), 31.0, 1.0);
	E_EXPECT_NEAR(Half.Prop("Total"), Half.Prop("UTotal") * 0.5, 1.0e-3);
	E_EXPECT_NEAR(Half.Prop("Scale"), 0.5, 1.0e-6);
	E_EXPECT_EQ(Half.Scripts.GetErrorCount(), 0u);
	// 범위: 음수는 0, 큰 값은 상한
	E_EXPECT_TRUE(Half.World.SetTimeScale(1000.0f));
	E_EXPECT_NEAR(Half.World.GetTimeScale(), FGameWorld::MaxTimeScale, 0.0f);
}

// 배율 0: 공중 캐릭터·떨어지는 바디가 그 자리에 멈추고(속도 보존), 재개하면 멈추지 않은 월드와 비트 단위로 같은 상태
E_TEST(TimeScale_ZeroFreezesAndResumesExactly)
{
	FTimeWorld Reference;
	FTimeWorld Paused;
	const auto Jump = [](FTimeWorld& W) { W.World.GetCharacters2D().Jump(W.Runner); };
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		Reference.Tick();
		Paused.Tick();
	}
	Jump(Reference);
	Jump(Paused);
	for (int32 Frame = 0; Frame < 8; ++Frame)
	{
		Reference.Tick();
		Paused.Tick();
	}
	E_EXPECT_FALSE(Paused.World.GetCharacters2D().IsGrounded(Paused.Runner)); // 공중
	const FCharacterState2D Before   = Paused.World.GetCharacters2D().GetState(Paused.Runner);
	const float             BallZ    = Paused.BallZ();
	const double            Total    = Paused.Prop("Total");
	E_EXPECT_TRUE(Paused.World.SetTimeScale(0.0f));
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Jump(Paused); // 정지 중 입력은 버린다 (재개 뒤 점프가 나가지 않는다)
		Paused.Tick();
	}
	const FCharacterState2D Frozen = Paused.World.GetCharacters2D().GetState(Paused.Runner);
	E_EXPECT_TRUE(Frozen.Position == Before.Position && Frozen.Velocity == Before.Velocity); // 떨어지지 않았다 (Crypt2D 공중 낙하 버그)
	E_EXPECT_NEAR(Paused.BallZ(), BallZ, 0.0f);
	E_EXPECT_NEAR(Paused.Prop("Total"), Total, 0.0);          // 게임 시간 멈춤
	E_EXPECT_TRUE(Paused.Prop("UTotal") > Total + 0.9);       // 실제 시간은 흐름
	E_EXPECT_TRUE(Paused.World.SetTimeScale(1.0f));
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Reference.Tick();
		Paused.Tick();
	}
	const FCharacterState2D A = Reference.World.GetCharacters2D().GetState(Reference.Runner);
	const FCharacterState2D B = Paused.World.GetCharacters2D().GetState(Paused.Runner);
	E_EXPECT_TRUE(A.Position == B.Position && A.Velocity == B.Velocity && A.JumpsUsed == B.JumpsUsed);
	E_EXPECT_NEAR(Paused.BallZ(), Reference.BallZ(), 0.0f);
	E_EXPECT_EQ(Paused.Scripts.GetErrorCount(), 0u);
}

// 히트스톱: Game.HitStop(0.05) = 다음 틱부터 실제 시간 0.05초(60fps 3틱) 동안 배율 0, 그 뒤 원래 배율
E_TEST(TimeScale_HitStopUsesRealTime)
{
	FTimeWorld W(ENetMode::Standalone, "Scripts/HitStop.lua");
	for (int32 Frame = 0; Frame < 20; ++Frame)
	{
		W.Tick();
	}
	E_EXPECT_TRUE(W.Scripts.GetInstanceProperty(W.Clock, "HitStopOk").bBool);
	// 프레임 10에서 부름 → 그 틱은 1, 11·12·13 정지, 14부터 1
	E_EXPECT_TRUE(W.Scripts.GetInstanceProperty(W.Clock, "HitStop").String == "1;0;0;0;1;1;");
	E_EXPECT_NEAR(W.World.GetHitStopRemaining(), 0.0f, 0.0f);
	// 겹치면 긴 쪽
	E_EXPECT_TRUE(W.World.HitStop(0.1f));
	E_EXPECT_TRUE(W.World.HitStop(0.02f));
	E_EXPECT_NEAR(W.World.GetHitStopRemaining(), 0.1f, 1.0e-6f);
	W.Tick();
	E_EXPECT_NEAR(W.World.GetTickTimeScale(), 0.0f, 0.0f);
}

// 멀티플레이: 배율/히트스톱은 Standalone 전용 — 리슨 서버에서는 거절, 배율 1 유지. 리슨으로 바뀌면 1로 돌아간다
E_TEST(TimeScale_RejectedInNetworkSession)
{
	FTimeWorld Listen(ENetMode::ListenServer);
	E_EXPECT_FALSE(Listen.World.SetTimeScale(0.5f));
	E_EXPECT_FALSE(Listen.World.HitStop(0.1f));
	E_EXPECT_NEAR(Listen.World.GetTimeScale(), 1.0f, 0.0f);
	E_EXPECT_TRUE(Listen.Scripts.RunString("assert(Game.SetTimeScale(0.5) == false and Game.GetTimeScale() == 1)"));

	FTimeWorld Standalone;
	E_EXPECT_TRUE(Standalone.World.SetTimeScale(0.25f));
	Standalone.World.SetNetMode(ENetMode::ListenServer);
	E_EXPECT_NEAR(Standalone.World.GetTimeScale(), 1.0f, 0.0f);
	// 플레이를 다시 시작하면 1
	FTimeWorld Again;
	E_EXPECT_TRUE(Again.World.SetTimeScale(0.0f));
	Again.World.BeginPlay(Again.Scene);
	E_EXPECT_NEAR(Again.World.GetTimeScale(), 1.0f, 0.0f);
}

// 마우스 커서: 창 픽셀(Input.GetMousePosition), UI 레이아웃 좌표(Input.GetMouseUIPosition — 뷰포트 위치·배율 반영, 입력 모드 GameOnly여도),
// UI 위 판정(Input.IsMouseOverUI), 커서 표시(Game.SetCursorVisible — 플레이 시작에 보임으로)
E_TEST(TimeScale_MouseCursorApi)
{
	const fs::path Content = GetContent();
	{
		FUIAsset Asset;
		Asset.DesignSize  = FVector2(400.0f, 300.0f);
		Asset.ScaleMode   = EUIScaleMode::Fit; // 800x600 뷰포트 → 배율 2
		FUIWidget* Panel  = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
		Panel->Name       = "Panel";
		Panel->Slot.Offsets = FUIMargin(0.0f, 0.0f, 100.0f, 100.0f); // 레이아웃 (0,0)~(100,100)
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/Cursor.eui"));
	}
	std::ofstream(Content / L"Scripts/Cursor.lua", std::ios::binary | std::ios::trunc) << R"(
local T = { Properties = {} }
function T:OnUpdate(dt)
	PX, PY = Input.GetMousePosition()
	UX, UY, Inside = Input.GetMouseUIPosition()
	Over = Input.IsMouseOverUI()
end
return T
)";
	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("HUD");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/Cursor.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Cursor.lua";
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);

	// 에디터처럼: 창 좌표 (700, 450), 뷰포트 이미지가 창 (100, 50)에서 시작 → 뷰포트 픽셀 (600, 400) → 레이아웃 (300, 200)
	FInput Input;
	Input.SetState({}, {}, 700, 450, 0.0f);
	const auto Frame = [&](bool bUIPointer, const FVector2& WindowPixel) {
		Input.SetState({}, {}, static_cast<int32>(WindowPixel.X), static_cast<int32>(WindowPixel.Y), 0.0f);
		FUIFrameInput UIInput;
		UIInput.Viewport    = FUIRect(FVector2::ZeroVector, FVector2(800.0f, 600.0f));
		UIInput.bHasPointer = bUIPointer;
		UIInput.Pointer     = FUISystem::MakePointer(Input, FVector2(-100.0f, -50.0f), true);
		FUISystem::Update(Scene, UIInput, Content);
		World.TickGameplay(Step, &Input);
	};
	Frame(false, FVector2(700.0f, 450.0f)); // GameOnly처럼 UI 포인터 입력 없음
	E_EXPECT_TRUE(Scripts.RunString("assert(PX == 700 and PY == 450, PX .. ',' .. PY)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(UX - 300) < 1e-3 and math.abs(UY - 200) < 1e-3 and Inside == true, UX .. ',' .. UY)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Over == false)"));
	Frame(true, FVector2(200.0f, 150.0f)); // 패널 위 (레이아웃 50, 50)
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(UX - 50) < 1e-3 and math.abs(UY - 50) < 1e-3 and Over == true)"));
	Frame(true, FVector2(50.0f, 20.0f)); // 뷰포트 밖 (왼쪽 위)
	E_EXPECT_TRUE(Scripts.RunString("assert(Inside == false and Over == false)"));

	// 커서 표시: 스크립트가 숨기고, 플레이를 다시 시작하면 보임
	E_EXPECT_TRUE(Scripts.RunString("Game.SetCursorVisible(false); assert(Game.IsCursorVisible() == false)"));
	E_EXPECT_FALSE(FInputModeState::IsCursorVisible());
	World.EndPlay();
	E_EXPECT_TRUE(FInputModeState::IsCursorVisible());
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
}

// 넉백 스크립트 (2D·3D 공용): 내내 오른쪽 입력, 프레임 30 넉백(-800, 경직 0.3), 프레임 80 위로 발사
constexpr const char* KnockScript = R"(
local T = { Properties = { Frame = 0, StunSeen = false, StunEnd = -1, MinX = 1.0e9, Airborne = false } }
function T:OnUpdate(dt)
	local P = self.Properties
	local e = self.entity
	P.Frame = P.Frame + 1
	e:AddMovementInput(Vector3(1, 0, 0)) -- 내내 오른쪽
	if P.Frame == 30 then e:AddKnockback(Vector3(-800, 0, 0), 0.3) end
	if P.Frame == 32 then P.StunSeen = e:IsStunned() end
	if P.Frame > 32 and P.StunEnd < 0 and not e:IsStunned() then P.StunEnd = P.Frame end
	if P.Frame > 30 then P.MinX = math.min(P.MinX, e:GetWorldPosition().X) end
	if P.Frame == 80 then e:LaunchCharacter(Vector3(0, 0, 700)) end
	if P.Frame == 85 then P.Airborne = not e:IsGrounded() end
end
return T
)";

// 넉백 Lua API: entity:AddKnockback(속도, 경직) — 경직 동안 이동 입력 무시, entity:LaunchCharacter(속도) — 더하기 발사로 뜬다
E_TEST(TimeScale_KnockbackLuaApi)
{
	const fs::path Content = GetContent();
	std::ofstream(Content / L"Scripts/Knock.lua", std::ios::binary | std::ios::trunc) << KnockScript;
	RegisterPhysicsTypes();
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(100000.0f, 100.0f);
	const FEntity Hero = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(0.0f, 0.0f, 61.0f);
	Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Hero);
	Scene.GetRegistry().Emplace<FScriptComponent>(Hero).ScriptAsset = "Scripts/Knock.lua";
	Scene.UpdateTransforms();
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	float XAt30 = 0.0f;
	for (int32 Frame = 1; Frame <= 100; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
		if (Frame == 30)
		{
			XAt30 = Scene.GetTransform(Hero).Position.X;
		}
	}
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "StunSeen").bBool);
	// 넉백 무브 = 같은 틱(프레임 30, 스크립트 뒤 이동기) → 경직 0.3초 = 무브 30~47 → 프레임 48 스크립트에서 풀린 것을 본다
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Hero, "StunEnd").Number, 48.0, 0.0);
	// 입력과 반대로 밀려났다: 800 × 0.3 - ½ × 1500 × 0.3² ≈ 172cm
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "MinX").Number < XAt30 - 120.0);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Airborne").bBool);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 같은 넉백 Lua 스크립트(Knock.lua)가 3D 캐릭터(FCharacterMovementComponent)에서도 같은 시점·같은 규칙으로 동작
E_TEST(TimeScale_KnockbackLuaApi3D)
{
	const fs::path Content = GetContent();
	std::ofstream(Content / L"Scripts/Knock.lua", std::ios::binary | std::ios::trunc) << KnockScript;
	RegisterPhysicsTypes();
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(50000.0f, 3000.0f, 10.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Floor).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);
	const FEntity Hero = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(0.0f, 0.0f, 91.0f);
	Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Hero);
	Scene.GetRegistry().Emplace<FScriptComponent>(Hero).ScriptAsset = "Scripts/Knock.lua";
	Scene.UpdateTransforms();
	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	float XAt30 = 0.0f;
	for (int32 Frame = 1; Frame <= 100; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
		if (Frame == 30)
		{
			XAt30 = Scene.GetTransform(Hero).Position.X;
		}
	}
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "StunSeen").bBool);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Hero, "StunEnd").Number, 48.0, 0.0); // 2D와 같은 시점
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "MinX").Number < XAt30 - 120.0);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Airborne").bBool);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 게임 모듈 시간 배율 API (IGameNet — Lua와 같은 규칙): Set/Get/HitStop/GetHitStopRemaining/GetUnscaledDeltaSeconds, 네트워크 세션에서는 거절
E_TEST(TimeScale_GameModuleApi)
{
	FTimeWorld Time;
	IGameNet&  Net = Time.World;
	E_EXPECT_TRUE(Net.SetTimeScale(0.25f));
	E_EXPECT_NEAR(Net.GetTimeScale(), 0.25f, 0.0f);
	Time.Tick();
	E_EXPECT_NEAR(Net.GetUnscaledDeltaSeconds(), Step, 0.0f);
	E_EXPECT_NEAR(Time.Prop("Scale"), 0.25, 1.0e-6);
	E_EXPECT_TRUE(Net.HitStop(0.05f));
	E_EXPECT_NEAR(Net.GetHitStopRemaining(), 0.05f, 0.0f);
	E_EXPECT_NEAR(Time.World.GetUpcomingTimeScale(), 0.0f, 0.0f);
	Time.Tick();
	E_EXPECT_NEAR(Time.World.GetTickTimeScale(), 0.0f, 0.0f);
	E_EXPECT_NEAR(Net.GetHitStopRemaining(), 0.05f - Step, 1.0e-6f);
	E_EXPECT_TRUE(Net.SetTimeScale(-1.0f)); // 0으로 자름
	E_EXPECT_NEAR(Net.GetTimeScale(), 0.0f, 0.0f);

	FTimeWorld Server(ENetMode::DedicatedServer);
	IGameNet&  ServerNet = Server.World;
	E_EXPECT_FALSE(ServerNet.SetTimeScale(0.5f));
	E_EXPECT_FALSE(ServerNet.HitStop(0.1f));
	E_EXPECT_NEAR(ServerNet.GetTimeScale(), 1.0f, 0.0f);
}

// UI 애니메이션 게임 시간 (UI/UIAnimation.h bUseGameTime): 기본 = 실제 시간(배율 0에서도 진행), 에셋 플래그 또는 Lua { GameTime = true }면
// 게임 시간 배율을 따른다 (FUIFrameInput::GameTimeScale = FGameWorld::GetUpcomingTimeScale)
E_TEST(TimeScale_UIAnimationGameTime)
{
	const fs::path Content = GetContent();
	{
		FUIAsset Asset;
		Asset.DesignSize = FVector2(400.0f, 300.0f);
		for (const char* Name : { "A", "B", "C" })
		{
			FUIWidget* Widget = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
			Widget->Name      = Name;
			FUIAnimation Animation;
			Animation.Name   = std::string("Fade") + Name;
			Animation.Length = 1.0f;
			FUIAnimTrack& Track = Animation.GetOrAddTrack(Name, EUIAnimProperty::Opacity);
			Track.SetKey(0.0f, 0.0f);
			Track.SetKey(1.0f, 1.0f);
			Animation.bUseGameTime = std::string_view(Name) == "B"; // B만 에셋에서 게임 시간
			Asset.Animations.push_back(std::move(Animation));
		}
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/GameTimeAnim.eui"));
		FUIAsset Loaded;
		E_EXPECT_TRUE(Loaded.LoadFromFile(Content / L"UI/GameTimeAnim.eui"));
		E_EXPECT_EQ(Loaded.Animations.size(), size_t(3));
		E_EXPECT_FALSE(Loaded.Animations[0].bUseGameTime);
		E_EXPECT_TRUE(Loaded.Animations[1].bUseGameTime); // 파일 왕복
	}
	std::ofstream(Content / L"Scripts/UIAnim.lua", std::ios::binary | std::ios::trunc) << R"(
local T = { Properties = { Frame = 0 } }
function T:OnUpdate(dt)
	local P = self.Properties
	P.Frame = P.Frame + 1
	if P.Frame == 1 then
		assert(self.entity:PlayUIAnimation("FadeA"))
		assert(self.entity:PlayUIAnimation("FadeB"))
		assert(self.entity:PlayUIAnimation("FadeC", 1, 1, { GameTime = true }))
		Game.SetTimeScale(0)
	end
end
return T
)";
	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("HUD");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/GameTimeAnim.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/UIAnim.lua";
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	const auto Frame = [&]() {
		FUIFrameInput UIInput;
		UIInput.Viewport      = FUIRect(FVector2::ZeroVector, FVector2(800.0f, 600.0f));
		UIInput.DeltaSeconds  = Step;
		UIInput.GameTimeScale = World.GetUpcomingTimeScale();
		FUISystem::Update(Scene, UIInput, Content);
		World.TickGameplay(Step, nullptr);
	};
	const auto Opacity = [&](const char* Name) {
		FUIInstance* Instance = Scene.GetRegistry().Get<FUIComponent>(Entity).Runtime.Instance.get();
		return Instance != nullptr && Instance->FindWidget(Name) != nullptr ? Instance->FindWidget(Name)->RenderOpacity : -1.0f;
	};
	Frame(); // 인스턴스 생성 + 스크립트가 재생 시작, 배율 0
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Frame();
	}
	E_EXPECT_NEAR(Opacity("A"), 0.5f, 0.02f); // 실제 시간: 정지 중에도 진행
	E_EXPECT_NEAR(Opacity("B"), 0.0f, 0.0f);  // 에셋 게임 시간: 멈춤
	E_EXPECT_NEAR(Opacity("C"), 0.0f, 0.0f);  // 재생 옵션 게임 시간: 멈춤
	E_EXPECT_TRUE(World.SetTimeScale(1.0f));
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Frame();
	}
	E_EXPECT_NEAR(Opacity("A"), 1.0f, 1.0e-4f);
	E_EXPECT_NEAR(Opacity("B"), 0.5f, 0.02f);
	E_EXPECT_NEAR(Opacity("C"), 0.5f, 0.02f);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}
