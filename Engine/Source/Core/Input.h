#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputTypes.h"
#include "Core/WindowEvent.h"

#include <bitset>

// 프레임 단위 키보드/마우스 상태 관리
class FInput
{
public:
	// 창 이벤트로 상태 갱신
	void ProcessEvent(const FWindowEvent& Event);

	// 프레임 종료 시 호출: 현재 상태를 이전 상태로 저장, 프레임 델타 초기화
	void EndFrame();

	// 모든 눌림 상태 해제 (포커스 상실 시)
	void ClearState();

	// 게임 UI가 포인터를 가져간 프레임에 게임 로직으로 넘길 사본: 마우스 버튼/휠을 비운다 (키보드/마우스 위치는 그대로)
	FInput WithoutMouseButtons() const;

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

private:
	std::bitset<static_cast<size_t>(EKey::Count)>         KeyStates;
	std::bitset<static_cast<size_t>(EKey::Count)>         PrevKeyStates;
	std::bitset<static_cast<size_t>(EMouseButton::Count)> ButtonStates;
	std::bitset<static_cast<size_t>(EMouseButton::Count)> PrevButtonStates;

	int32 MouseX     = 0;
	int32 MouseY     = 0;
	int32 PrevMouseX = 0;
	int32 PrevMouseY = 0;
	float WheelDelta = 0.0f;
};
