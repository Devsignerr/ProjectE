#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputTypes.h"
#include "Core/WindowEvent.h"

#include <bitset>

// 프레임 단위 키보드/마우스 상태 관리
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

	bool IsKeyDown(EKey Key) const { return KeyStates[static_cast<size_t>(Key)]; }
	bool IsKeyPressed(EKey Key) const;  // 이번 프레임에 눌림
	bool IsKeyReleased(EKey Key) const; // 이번 프레임에 떼어짐

	bool IsMouseButtonDown(EMouseButton Button) const { return ButtonStates[static_cast<size_t>(Button)]; }
	bool IsMouseButtonPressed(EMouseButton Button) const;
	bool IsMouseButtonReleased(EMouseButton Button) const;

	int32 GetMouseX() const { return MouseX; }
	int32 GetMouseY() const { return MouseY; }
	int32 GetMouseDeltaX() const { return MouseX - PrevMouseX; }
	int32 GetMouseDeltaY() const { return MouseY - PrevMouseY; }
	float GetMouseWheelDelta() const { return WheelDelta; }

	// 네트워크 입력 커맨드: 현재 상태를 통째로 읽고 쓴다 (서버는 원격 플레이어마다 FInput을 두고 받은 상태로 교체한다.
	// 눌림/떼어짐 판정은 로컬과 같이 EndFrame 기준)
	const FKeyBits&    GetKeyStates() const { return KeyStates; }
	const FButtonBits& GetButtonStates() const { return ButtonStates; }
	void               SetState(const FKeyBits& Keys, const FButtonBits& Buttons, int32 InMouseX, int32 InMouseY, float Wheel);

private:
	FKeyBits    KeyStates;
	FKeyBits    PrevKeyStates;
	FButtonBits ButtonStates;
	FButtonBits PrevButtonStates;

	int32 MouseX     = 0;
	int32 MouseY     = 0;
	int32 PrevMouseX = 0;
	int32 PrevMouseY = 0;
	float WheelDelta = 0.0f;
};
