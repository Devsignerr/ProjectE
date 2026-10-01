#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputTypes.h"

#include <array>

// XInput 게임패드 폴링 (Xbox 호환 컨트롤러, 최대 4개). 시스템 DLL(xinput1_4 → xinput9_1_0)을 실행 중에 불러오므로
// 링크 의존이 없고, DLL이 없으면 항상 연결 안 됨으로 동작한다. Windows 헤더는 GamepadInput.cpp에서만.
//   - 연결된 슬롯은 매 프레임, 연결 안 된 슬롯은 ReconnectIntervalSeconds마다 확인한다 (XInputGetState는 빈 슬롯에서 느리다)
//   - 스틱/트리거는 원시 값 (-1~1 / 0~1, 데드존 없음 — 액션 수정자 DeadZone이 처리)
//   - GetPrimary = 연결된 첫 번째 패드 (FInput에 넣는 패드). 여러 개는 GetState(인덱스)
class FGamepadInput
{
public:
	static constexpr uint32 MaxGamepads              = 4;
	static constexpr float  ReconnectIntervalSeconds = 1.0f;

	FGamepadInput();
	~FGamepadInput();

	FGamepadInput(const FGamepadInput&)            = delete;
	FGamepadInput& operator=(const FGamepadInput&) = delete;

	void Poll(float DeltaSeconds);

	bool                 IsAvailable() const { return GetStateFunction != nullptr; } // XInput DLL을 불러왔는지
	const FGamepadState& GetState(uint32 Index) const { return States[Index < MaxGamepads ? Index : 0]; }
	const FGamepadState& GetPrimary() const;
	int32                GetPrimaryIndex() const; // 없으면 -1

private:
	void*                                   Module           = nullptr; // HMODULE
	void*                                   GetStateFunction = nullptr; // XInputGetState
	std::array<FGamepadState, MaxGamepads>  States;
	std::array<float, MaxGamepads>          RetrySeconds = {}; // 연결 안 된 슬롯의 다음 확인까지 남은 시간
};
