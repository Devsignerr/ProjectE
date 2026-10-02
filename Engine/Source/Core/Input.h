#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputActions.h"
#include "Core/InputTypes.h"
#include "Core/WindowEvent.h"

#include <bitset>
#include <string>
#include <string_view>
#include <vector>

// 액션 하나의 이번 프레임 값 (FInput::UpdateActions가 매핑으로 계산, 서버의 원격 입력은 받은 값)
struct FInputActionState
{
	std::string      Name;
	EInputActionType Type = EInputActionType::Button;
	FVector2         Value;              // Button = (0|1, 0), Axis1D = (x, 0), Axis2D = (x, y)
	bool             bActive    = false; // 작동 중 (문턱 이상)
	bool             bWasActive = false; // 이전 프레임 (EndFrame에서 갱신)
};

// 프레임 단위 키보드/마우스/게임패드 상태 + 입력 액션 값 (Core/InputActions.h)
//   앱 루프(FApplication): 창 이벤트 → 게임패드 폴링(SetGamepadState) → UpdateActions(유효 매핑) → OnUpdate → EndFrame
class FInput
{
public:
	using FKeyBits    = std::bitset<static_cast<size_t>(EKey::Count)>;
	using FButtonBits = std::bitset<static_cast<size_t>(EMouseButton::Count)>;

	// 창 이벤트로 상태 갱신
	void ProcessEvent(const FWindowEvent& Event);

	// 프레임 종료 시 호출: 현재 상태를 이전 상태로 저장, 프레임 델타 초기화
	void EndFrame();

	// 모든 눌림 상태 해제 (포커스 상실 시)
	void ClearState();

	// 게임 UI가 포인터를 가져간 프레임에 게임 로직으로 넘길 사본: 마우스 버튼/휠을 비운다 (키보드/마우스 위치는 그대로)
	FInput WithoutMouseButtons() const;
	// 게임 UI 텍스트 상자가 키보드를 가져간 프레임: 키/문자를 비운다 (마우스는 그대로)
	FInput WithoutKeyboard() const;
	// 게임이 입력을 받지 않는 프레임 (입력 모드 UIOnly, 에디터 플레이 빙의 해제): 키/마우스 버튼/휠/시점/문자/게임패드/액션을 모두 비운다
	// (마우스 위치만 유지, 이전 상태도 비워 떼어짐 판정이 생기지 않는다)
	FInput WithoutAnyInput() const;

	bool IsKeyDown(EKey Key) const { return KeyStates[static_cast<size_t>(Key)]; }
	bool IsKeyPressed(EKey Key) const;  // 이번 프레임에 눌림
	bool IsKeyRepeated(EKey Key) const; // 이번 프레임에 눌림 또는 자동 반복 (텍스트 편집: 지우기/화살표 누르고 있기)
	bool IsKeyReleased(EKey Key) const; // 이번 프레임에 떼어짐

	bool IsMouseButtonDown(EMouseButton Button) const { return ButtonStates[static_cast<size_t>(Button)]; }
	bool IsMouseButtonPressed(EMouseButton Button) const;
	bool IsMouseButtonReleased(EMouseButton Button) const;

	int32 GetMouseX() const { return MouseX; }
	int32 GetMouseY() const { return MouseY; }
	int32 GetMouseDeltaX() const { return MouseX - PrevMouseX; }
	int32 GetMouseDeltaY() const { return MouseY - PrevMouseY; }
	float GetMouseWheelDelta() const { return WheelDelta; }
	// 이번 프레임 원시 마우스 이동량 (장치 카운트 — 화면 가장자리·커서 잠금과 무관, 시점 회전용)
	float GetLookDeltaX() const { return LookDeltaX; }
	float GetLookDeltaY() const { return LookDeltaY; }
	// 이번 프레임에 입력된 문자 (WM_CHAR, 제어 문자 포함)
	const std::u32string& GetTypedText() const { return TypedText; }
	// IME 조합 중인 글자 (확정 전, 프레임을 넘어 유지 — 게임 텍스트 입력 중에만 채워진다) + 조합 안 커서
	const std::u32string& GetCompositionText() const { return CompositionText; }
	int32                 GetCompositionCursor() const { return CompositionCursor; }

	// 네트워크 입력 커맨드: 현재 상태를 통째로 읽고 쓴다 (서버는 원격 플레이어마다 FInput을 두고 받은 상태로 교체한다.
	// 눌림/떼어짐 판정은 로컬과 같이 EndFrame 기준)
	const FKeyBits&    GetKeyStates() const { return KeyStates; }
	const FButtonBits& GetButtonStates() const { return ButtonStates; }
	void               SetState(const FKeyBits& Keys, const FButtonBits& Buttons, int32 InMouseX, int32 InMouseY, float Wheel);

	// ---- 게임패드 (첫 번째로 연결된 패드 — 앱이 매 프레임 폴링해 넣는다)
	void                 SetGamepadState(const FGamepadState& State) { Gamepad = State; }
	const FGamepadState& GetGamepad() const { return Gamepad; }
	bool                 IsGamepadConnected() const { return Gamepad.bConnected; }
	bool                 IsGamepadButtonDown(EGamepadButton Button) const { return Gamepad.IsButtonDown(Button); }
	bool                 IsGamepadButtonPressed(EGamepadButton Button) const { return Gamepad.IsButtonDown(Button) && !PrevGamepad.IsButtonDown(Button); }
	bool                 IsGamepadButtonReleased(EGamepadButton Button) const { return !Gamepad.IsButtonDown(Button) && PrevGamepad.IsButtonDown(Button); }

	// 입력 소스의 이번 프레임 원시 값 (키/버튼 = (1|0, 0), 1D 축 = (값, 0), 2D = (X, Y). MouseXY = 원시 마우스 이동)
	FVector2 ReadSource(const FInputSource& Source) const;

	// ---- 입력 액션
	// 현재 원시 상태로 액션 값을 계산한다. Mapping은 비소유 — 이 FInput(과 사본)보다 오래 살아야 한다 (보통 FInputSettings의 유효 매핑).
	// WithoutMouseButtons/WithoutKeyboard 사본은 이 매핑으로 다시 계산한다
	void UpdateActions(const FInputMapping& Mapping, float DeltaSeconds);
	// 네트워크: 받은 액션 값으로 교체 (서버의 원격 플레이어 입력 — 클라이언트가 자기 바인딩으로 계산한 값). Values는 Mapping.Actions 순서
	void SetActionValues(const FInputMapping& Mapping, const std::vector<FInputActionState>& Values);

	const std::vector<FInputActionState>& GetActions() const { return Actions; }
	const FInputActionState*              FindAction(std::string_view Name) const;
	FVector2 GetActionValue(std::string_view Name) const;  // 없으면 0
	bool     IsActionDown(std::string_view Name) const;     // 작동 중 (누르고 있음)
	bool     WasActionPressed(std::string_view Name) const; // 이번 프레임에 작동 시작
	bool     WasActionReleased(std::string_view Name) const; // 이번 프레임에 작동 끝

private:
	void ReevaluateActions();

	FKeyBits    KeyStates;
	FKeyBits    PrevKeyStates;
	FKeyBits    RepeatStates; // 이번 프레임에 KeyDown(반복 포함)이 온 키
	FButtonBits ButtonStates;
	FButtonBits PrevButtonStates;

	int32 MouseX     = 0;
	int32 MouseY     = 0;
	int32 PrevMouseX = 0;
	int32 PrevMouseY = 0;
	float WheelDelta = 0.0f;
	float LookDeltaX = 0.0f;
	float LookDeltaY = 0.0f;

	std::u32string TypedText;
	std::u32string CompositionText;
	int32          CompositionCursor = 0;

	FGamepadState Gamepad;
	FGamepadState PrevGamepad;

	std::vector<FInputActionState> Actions;
	const FInputMapping*           ActionMapping     = nullptr; // 비소유 (UpdateActions 참고)
	float                          ActionDeltaSeconds = 0.0f;
};
