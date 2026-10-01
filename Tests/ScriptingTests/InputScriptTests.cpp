#include "Core/Input.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

// Lua 입력 액션 API: GetAction(종류별 반환), IsActionPressed/WasActionPressed/WasActionReleased, 재지정(Rebind/GetBindings/ResetBindings)
E_TEST(InputScript_ActionsAndRebind)
{
	FInputSettings& Settings = FProjectSettings::Get().Input;
	Settings.ResetUserBindings();
	E_EXPECT_FALSE(Settings.IsUserFileEnabled()); // 테스트는 사용자 파일을 쓰지 않는다

	FScene        Scene;
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(FTestRegistry::GetTempDirectory());
	Scripts.BeginPlay(Scene);

	// 입력 없음: 매핑의 종류대로 0/false, 모르는 액션은 오류
	E_EXPECT_TRUE(Scripts.RunString(R"(
local X, Y = Input.GetAction('Move')
assert(X == 0 and Y == 0)
assert(Input.GetAction('Jump') == false)
assert(not Input.IsActionPressed('Jump') and not Input.WasActionPressed('Jump'))
)"));
	E_EXPECT_FALSE(Scripts.RunString("Input.GetAction('NoSuchAction')"));

	FInput       Input;
	FWindowEvent Event;
	Event.Type = EWindowEventType::KeyDown;
	Event.Key  = EKey::D;
	Input.ProcessEvent(Event);
	Event.Key = EKey::Space;
	Input.ProcessEvent(Event);
	Input.UpdateActions(Settings.GetEffectiveMapping(), 1.0f / 60.0f);
	Scripts.Update(1.0f / 60.0f, &Input);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local X, Y = Input.GetAction('Move')
assert(X == 1 and Y == 0, 'D = 오른쪽')
assert(Input.GetAction('Jump') == true and Input.IsActionPressed('Jump') and Input.WasActionPressed('Jump'))
assert(not Input.WasActionReleased('Jump'))
)"));

	// 재지정: Jump Space → K (게임패드 A는 그대로)
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(Input.Rebind('Jump', 'Space', 'K'))
assert(not Input.Rebind('Jump', 'Space', 'L'))
local B = Input.GetBindings('Jump')
assert(#B == 2 and B[1] == 'K' and B[2] == 'Gamepad_A')
)"));
	E_EXPECT_FALSE(Scripts.RunString("Input.Rebind('Jump', 'K', 'NoSuchKey')"));
	Input.EndFrame();
	Input.UpdateActions(Settings.GetEffectiveMapping(), 1.0f / 60.0f); // Space는 이제 점프가 아니다
	Scripts.Update(1.0f / 60.0f, &Input);
	E_EXPECT_TRUE(Scripts.RunString("assert(not Input.IsActionPressed('Jump') and Input.WasActionReleased('Jump'))"));
	E_EXPECT_TRUE(Scripts.RunString("Input.ResetBindings('Jump'); assert(Input.GetBindings('Jump')[1] == 'Space')"));
	E_EXPECT_FALSE(Settings.HasUserBindings("Jump"));

	// 게임패드 상태 (가짜)
	FGamepadState Pad;
	Pad.bConnected = true;
	Pad.SetButton(EGamepadButton::B, true);
	Input.SetGamepadState(Pad);
	E_EXPECT_TRUE(Scripts.RunString("assert(Input.IsGamepadConnected() and Input.IsGamepadButtonDown('B') and not Input.IsGamepadButtonDown('A'))"));
	E_EXPECT_FALSE(Scripts.RunString("Input.IsGamepadButtonDown('Q')"));
	Scripts.EndPlay();
	Settings.ResetUserBindings();
}
