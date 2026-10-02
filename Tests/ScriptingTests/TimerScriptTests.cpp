#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <fstream>

// Lua Timer/Coroutine (Phase 41-1, 규칙은 Scripting/ScriptTimerBindings.cpp 머리 주석)
namespace
{
	std::filesystem::path GetTimerTestContentDirectory()
	{
		static const std::filesystem::path Directory = [] {
			std::filesystem::path Path = FTestRegistry::GetTempDirectory() / L"ProjectETimerScriptTests";
			std::error_code       ErrorCode;
			std::filesystem::remove_all(Path, ErrorCode);
			std::filesystem::create_directories(Path / L"Scripts");
			return Path;
		}();
		return Directory;
	}

	void WriteTimerScript(const char* RelativePath, const char* Source)
	{
		std::ofstream File(GetTimerTestContentDirectory() / RelativePath, std::ios::binary | std::ios::trunc);
		File << Source;
	}

	FEntity AddTimerScriptedEntity(FScene& Scene, const char* Name, const char* ScriptAsset, const char* Overrides = "")
	{
		const FEntity     Entity    = Scene.CreateEntity(Name);
		FScriptComponent& Component = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Component.ScriptAsset       = ScriptAsset;
		Component.PropertyOverrides = Overrides;
		return Entity;
	}

	void RunFrames(FScriptSystem& Scripts, int32 Count, float DeltaSeconds = 0.1f)
	{
		for (int32 Frame = 0; Frame < Count; ++Frame)
		{
			Scripts.Update(DeltaSeconds, nullptr);
		}
	}
} // namespace

E_TEST(ScriptTimer_AfterEveryCancel)
{
	WriteTimerScript("Scripts/Timers.lua", R"(
local T = { Properties = {} }
function T:OnStart()
	self.AfterCount, self.EveryCount, self.CancelledCount, self.StopCount = 0, 0, 0, 0
	self.AfterId = Timer.After(0.25, function()
		self.AfterCount = self.AfterCount + 1
		self.AfterFrame = Time.FrameCount
		self.ActiveInside = Timer.IsActive(self.AfterId)
	end)
	self.EveryId = Timer.Every(0.2, function() self.EveryCount = self.EveryCount + 1 end)
	local Cancelled = Timer.After(0.1, function() self.CancelledCount = self.CancelledCount + 1 end)
	assert(Timer.IsActive(Cancelled) and Timer.Cancel(Cancelled) and not Timer.Cancel(Cancelled))
	self.StopId = Timer.Every(0.1, function()
		self.StopCount = self.StopCount + 1
		return self.StopCount < 2 -- false를 돌려주면 멈춘다
	end)
end
return T
)");
	FScene Scene;
	AddTimerScriptedEntity(Scene, "A", "Scripts/Timers.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTimerTestContentDirectory());
	Scripts.BeginPlay(Scene);
	RunFrames(Scripts, 10); // 0.1초 × 10 (OnStart는 1프레임)

	// After(0.25): 만든 프레임(1) 다음부터 0.1씩 → 4프레임에 발사. Every(0.2): 3, 5, 7, 9프레임
	E_EXPECT_TRUE(Scripts.RunString(R"(
local S = Scene.Find('A'):GetScript()
assert(S.AfterCount == 1, 'After ' .. S.AfterCount)
assert(S.AfterFrame == 4, 'AfterFrame ' .. tostring(S.AfterFrame))
assert(S.ActiveInside == false)
assert(not Timer.IsActive(S.AfterId))
assert(S.EveryCount == 4, 'Every ' .. S.EveryCount)
assert(Timer.IsActive(S.EveryId))
assert(S.CancelledCount == 0)
assert(S.StopCount == 2 and not Timer.IsActive(S.StopId))
assert(Timer.Cancel(S.EveryId))
)"));
	RunFrames(Scripts, 4);
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('A'):GetScript().EveryCount == 4)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 인스턴스 코드 밖(RunString)에서는 소유자가 없어 오류
	E_EXPECT_TRUE(!Scripts.RunString("Timer.After(1, function() end)"));
	E_EXPECT_TRUE(!Scripts.RunString("Coroutine.Start(function() end)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 2u);
	Scripts.EndPlay();
}

E_TEST(ScriptCoroutine_WaitSequence)
{
	WriteTimerScript("Scripts/Coroutines.lua", R"(
local C = { Properties = {} }
function C:OnStart()
	self.Log = {}
	self.Id = Coroutine.Start(function(Arg)
		table.insert(self.Log, 'start' .. Arg .. ':' .. Time.FrameCount)
		Wait()
		table.insert(self.Log, 'frame:' .. Time.FrameCount)
		WaitFrames(2)
		table.insert(self.Log, 'frames:' .. Time.FrameCount)
		Wait(0.25)
		table.insert(self.Log, 'sec:' .. Time.FrameCount)
		WaitUntil(function() return self.Go end)
		table.insert(self.Log, 'until:' .. Time.FrameCount)
		-- 코루틴 안에서 타이머/코루틴을 더 만들 수 있다
		Timer.After(0, function() self.TimerFromCoroutine = Time.FrameCount end)
	end, 'X')
	self.Stopped = Coroutine.Start(function() Wait(); self.ShouldNotRun = true end)
	assert(Coroutine.IsRunning(self.Stopped) and Coroutine.Stop(self.Stopped))
	self.Immediate = Coroutine.Start(function() self.RanImmediately = true end) -- 대기 없이 끝남
end
return C
)");
	FScene Scene;
	AddTimerScriptedEntity(Scene, "C", "Scripts/Coroutines.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTimerTestContentDirectory());
	Scripts.BeginPlay(Scene);
	RunFrames(Scripts, 9);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local S = Scene.Find('C'):GetScript()
assert(table.concat(S.Log, ',') == 'startX:1,frame:2,frames:4,sec:7', table.concat(S.Log, ','))
assert(Coroutine.IsRunning(S.Id))
assert(S.ShouldNotRun == nil and S.RanImmediately == true and not Coroutine.IsRunning(S.Immediate))
S.Go = true
)"));
	RunFrames(Scripts, 2);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local S = Scene.Find('C'):GetScript()
assert(table.concat(S.Log, ',') == 'startX:1,frame:2,frames:4,sec:7,until:10', table.concat(S.Log, ','))
assert(not Coroutine.IsRunning(S.Id))
assert(S.TimerFromCoroutine == 11, tostring(S.TimerFromCoroutine))
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 코루틴 밖에서 Wait → 그 인스턴스의 오류
	E_EXPECT_TRUE(!Scripts.RunString("Wait(1)"));
	Scripts.EndPlay();
}

E_TEST(ScriptTimer_ErrorStopsOnlyThatInstance)
{
	WriteTimerScript("Scripts/TimerError.lua", R"(
local E = { Properties = { Fail = false, InCoroutine = false } }
function E:OnStart()
	self.Updates = 0
	if self.Properties.Fail then
		Timer.After(0.1, function() error('타이머 실패') end)
	end
	if self.Properties.InCoroutine then
		Coroutine.Start(function() Wait() error('코루틴 실패') end)
	end
	Timer.Every(0, function() self.Ticks = (self.Ticks or 0) + 1 end)
end
function E:OnUpdate(dt)
	self.Updates = self.Updates + 1
end
return E
)");
	FScene Scene;
	AddTimerScriptedEntity(Scene, "Bad", "Scripts/TimerError.lua", R"({"Fail": true})");
	AddTimerScriptedEntity(Scene, "BadCoroutine", "Scripts/TimerError.lua", R"({"InCoroutine": true})");
	AddTimerScriptedEntity(Scene, "Good", "Scripts/TimerError.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTimerTestContentDirectory());
	Scripts.BeginPlay(Scene);
	RunFrames(Scripts, 6);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 2u);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Bad, BadCo, Good = Scene.Find('Bad'):GetScript(), Scene.Find('BadCoroutine'):GetScript(), Scene.Find('Good'):GetScript()
assert(Bad.Updates == 2, 'Bad ' .. Bad.Updates)     -- 2프레임 타이머 오류로 멈춤 (3프레임부터 OnUpdate 없음)
assert(BadCo.Updates == 2, 'BadCo ' .. BadCo.Updates)
assert(Good.Updates == 6 and Good.Ticks == 5, 'Good ' .. Good.Updates .. ' ' .. tostring(Good.Ticks))
)"));
	Scripts.EndPlay();
}

E_TEST(ScriptTimer_ClearedOnDestroyReloadAndEndPlay)
{
	WriteTimerScript("Scripts/TimerOwner.lua", R"(
local O = { Properties = {} }
function O:OnStart()
	Timer.After(0.3, function() AfterFired = true end)
	Timer.Every(0.1, function() Ticks = (Ticks or 0) + 1 end)
	Coroutine.Start(function() while true do Wait() CoroutineTicks = (CoroutineTicks or 0) + 1 end end)
end
function O:OnDestroy()
	Timer.After(0, function() DestroyTimerFired = true end) -- OnDestroy가 만든 것도 함께 사라진다
end
return O
)");
	FScene        Scene;
	const FEntity Doomed = AddTimerScriptedEntity(Scene, "Doomed", "Scripts/TimerOwner.lua");
	AddTimerScriptedEntity(Scene, "Reloaded", "Scripts/TimerOwner.lua");
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetTimerTestContentDirectory());
	Scripts.BeginPlay(Scene);
	RunFrames(Scripts, 2); // 2프레임: 두 인스턴스의 Every/코루틴 한 번씩
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == 2 and CoroutineTicks == 2, tostring(Ticks) .. ' ' .. tostring(CoroutineTicks))"));

	// 엔티티 파괴 (지연 → 이번 Update 끝): 그 인스턴스 것만 사라진다
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Doomed'):Destroy()"));
	RunFrames(Scripts, 1);
	E_EXPECT_TRUE(!Scene.GetRegistry().IsValid(Doomed));
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == 4 and CoroutineTicks == 4, tostring(Ticks) .. ' ' .. tostring(CoroutineTicks))"));
	RunFrames(Scripts, 3);
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == 7 and CoroutineTicks == 7 and DestroyTimerFired == nil)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(AfterFired == true)")); // 남은 인스턴스의 After(0.3)는 4프레임에

	// 핫 리로드: 그 스크립트 인스턴스의 타이머/코루틴 취소 (인스턴스는 계속 돈다)
	E_EXPECT_TRUE(Scripts.ReloadScript(GetTimerTestContentDirectory() / L"Scripts/TimerOwner.lua"));
	RunFrames(Scripts, 3);
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == 7 and CoroutineTicks == 7, tostring(Ticks) .. ' ' .. tostring(CoroutineTicks))"));
	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(1));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// EndPlay: 대기 중인 코루틴/타이머가 있어도 깨끗이 끝나고, 다시 시작하면 새 상태
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Reloaded'):Destroy()"));
	AddTimerScriptedEntity(Scene, "Fresh", "Scripts/TimerOwner.lua");
	RunFrames(Scripts, 2);
	Scripts.EndPlay();
	E_EXPECT_EQ(Scripts.GetInstanceCount(), static_cast<size_t>(0));
	Scripts.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString("assert(Ticks == nil and CoroutineTicks == nil)"));
	Scripts.EndPlay();
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
}
