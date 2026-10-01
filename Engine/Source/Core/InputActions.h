#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputTypes.h"
#include "Core/Math/Vector2.h"

#include <string>
#include <string_view>
#include <vector>

// 입력 액션 매핑 (언리얼 Enhanced Input 방식) — 순수 로직. 플랫폼/창/전역 상태에 의존하지 않는다 (테스트 InputActionTests).
//
//   액션(FInputAction): 이름 + 값 종류(버튼/1D/2D 축) + 바인딩 목록. 게임 코드는 키가 아니라 액션("Move", "Jump", "Look")을 읽는다.
//   바인딩(FInputBinding): 입력 소스 하나(키, 마우스 버튼, 마우스 이동/휠, 게임패드 버튼/축) + 수정자 목록(순서대로 적용).
//   소스 원시 값: 키/버튼 = (1, 0) 또는 (0, 0), 1D 축 = (값, 0), 2D 축 = (X, Y).
//   수정자: Negate(축별 부호 반전), Swizzle(X↔Y — 언리얼 YXZ), DeadZone(축별/원형, Lower~Upper를 0~1로 다시 펼침),
//           Scale(축별 배율), ScaleByDeltaTime(× 프레임 시간 — 스틱 시점 회전처럼 "초당" 값을 마우스 "프레임당" 값에 맞출 때).
//   액션 값 = 바인딩 결과의 합 (W와 S를 함께 누르면 0). 버튼은 바인딩 중 하나라도 작동 문턱을 넘으면 켜짐.
//   켜짐(작동): |값|(2D는 길이) ≥ ActuationThreshold. 눌림/떼어짐은 이전 프레임 켜짐과 비교한다 (FInput::EndFrame 기준).
//   WASD → 2D: W = Swizzle (0,1), S = Swizzle + Negate (0,-1), D = (1,0), A = Negate (-1,0) — X = 오른쪽, Y = 앞.
//
// 소스 이름 (JSON/Lua/설정 창 공용): 키 "W", "Space", "LeftShift", "0", "F1" ... (EKey), 마우스 "MouseLeft/MouseRight/MouseMiddle/
//   MouseThumb1/MouseThumb2", "MouseXY"(원시 이동 2D — 시점용, FInput::GetLookDelta), "MouseX", "MouseY", "MouseWheel",
//   게임패드 "Gamepad_A/B/X/Y/LeftShoulder/RightShoulder/Back/Start/LeftThumb/RightThumb/DPadUp/DPadDown/DPadLeft/DPadRight",
//   "Gamepad_LeftStick/RightStick"(2D), "Gamepad_LeftX/LeftY/RightX/RightY", "Gamepad_LeftTrigger/RightTrigger"(0~1)

enum class EInputActionType : uint8
{
	Button, // bool
	Axis1D, // float
	Axis2D, // (x, y)
};

enum class EInputSourceType : uint8
{
	None,
	Key,           // Code = EKey
	MouseButton,   // Code = EMouseButton
	MouseAxis,     // Code = EMouseAxis
	GamepadButton, // Code = EGamepadButton
	GamepadAxis,   // Code = EGamepadAxis
};

enum class EMouseAxis : uint8
{
	XY = 0, // 원시 이동 (장치 카운트, 프레임당)
	X,
	Y,
	Wheel,

	Count
};

struct FInputSource
{
	EInputSourceType Type = EInputSourceType::None;
	uint16           Code = 0;

	bool IsValid() const { return Type != EInputSourceType::None; }
	bool Is2D() const;
	bool operator==(const FInputSource& Other) const { return Type == Other.Type && Code == Other.Code; }

	static FInputSource Key(EKey InKey) { return { EInputSourceType::Key, static_cast<uint16>(InKey) }; }
	static FInputSource Mouse(EMouseButton Button) { return { EInputSourceType::MouseButton, static_cast<uint16>(Button) }; }
	static FInputSource Mouse(EMouseAxis Axis) { return { EInputSourceType::MouseAxis, static_cast<uint16>(Axis) }; }
	static FInputSource Gamepad(EGamepadButton Button) { return { EInputSourceType::GamepadButton, static_cast<uint16>(Button) }; }
	static FInputSource Gamepad(EGamepadAxis Axis) { return { EInputSourceType::GamepadAxis, static_cast<uint16>(Axis) }; }
};

enum class EInputModifierType : uint8
{
	Negate,
	Swizzle,
	DeadZone,
	Scale,
	ScaleByDeltaTime,
};

struct FInputModifier
{
	EInputModifierType Type = EInputModifierType::Negate;
	bool               bX   = true; // Negate: 반전할 축
	bool               bY   = true;
	FVector2           Scale = FVector2(1.0f, 1.0f); // Scale
	float              Lower   = 0.2f;  // DeadZone: 이 아래는 0
	float              Upper   = 1.0f;  // DeadZone: 이 위는 1
	bool               bRadial = true;  // DeadZone: true = 길이 기준(스틱), false = 축별

	static FInputModifier MakeNegate(bool bInX = true, bool bInY = true);
	static FInputModifier MakeSwizzle();
	static FInputModifier MakeDeadZone(float InLower = 0.2f, float InUpper = 1.0f, bool bInRadial = true);
	static FInputModifier MakeScale(FVector2 InScale);
	static FInputModifier MakeScaleByDeltaTime();
};

struct FInputBinding
{
	FInputSource                Source;
	std::vector<FInputModifier> Modifiers;
};

struct FInputAction
{
	std::string                Name;
	EInputActionType           Type = EInputActionType::Button;
	std::string                Description;
	float                      ActuationThreshold = 0.5f;
	std::vector<FInputBinding> Bindings;
};

// 액션 목록 (프로젝트 설정 "입력" = <프로젝트>/Config/Input.json)
struct FInputMapping
{
	std::vector<FInputAction> Actions;

	const FInputAction* Find(std::string_view Name) const;
	FInputAction*       Find(std::string_view Name);
	// 액션 이름 + 종류 순서의 해시 (네트워크 입력 커맨드가 액션 값을 순서로 보낼 때 양쪽 목록이 같은지 확인)
	uint32 GetLayoutHash() const;

	// JSON: { "Version": 1, "Actions": [ { "Name", "Type", "Description", "ActuationThreshold", "Bindings": [ { "Source", "Modifiers": [...] } ] } ] }
	// 모르는 소스/수정자는 경고 문구를 Error에 남기고 그 항목만 건너뛴다 (JSON 자체가 깨졌을 때만 false)
	static bool FromJson(std::string_view Json, FInputMapping& OutMapping, std::string* Error = nullptr);
	std::string ToJson() const;
};

namespace InputNames
{
	const char* GetKeyName(EKey Key); // "W", "Space", "0" ... (Lua Input.IsKeyDown과 같은 이름)
	bool        TryParseKey(std::string_view Name, EKey& OutKey);
	const char* GetGamepadButtonName(EGamepadButton Button); // "A", "LeftShoulder" (접두사 없음)
	bool        TryParseGamepadButton(std::string_view Name, EGamepadButton& OutButton);
	const char* GetGamepadAxisName(EGamepadAxis Axis); // "LeftStick", "LeftTrigger" (접두사 없음)
	bool        TryParseGamepadAxis(std::string_view Name, EGamepadAxis& OutAxis);

	std::string ToString(const FInputSource& Source); // 소스 이름 (머리 주석 목록)
	bool        TryParseSource(std::string_view Name, FInputSource& OutSource);
	// 설정 창/재지정 목록에 보여 줄 모든 소스 이름 (키 → 마우스 → 게임패드 순)
	const std::vector<std::string>& GetAllSourceNames();

	const char* ToString(EInputActionType Type);
	bool        TryParseActionType(std::string_view Name, EInputActionType& OutType);
	const char* ToString(EInputModifierType Type);
	bool        TryParseModifierType(std::string_view Name, EInputModifierType& OutType);
} // namespace InputNames

namespace InputActionMath
{
	FVector2 ApplyModifier(const FInputModifier& Modifier, FVector2 Value, float DeltaSeconds);
	FVector2 ApplyModifiers(const std::vector<FInputModifier>& Modifiers, FVector2 Value, float DeltaSeconds);
	// 액션 종류에 맞춘 크기: 2D = 길이, 그 외 |X|
	float    GetMagnitude(EInputActionType Type, FVector2 Value);

	struct FResult
	{
		FVector2 Value;          // Button = (0|1, 0), Axis1D = (x, 0), Axis2D = (x, y)
		bool     bActive = false; // 작동 문턱을 넘음
	};

	// Read(const FInputSource&) → FVector2 원시 값
	template <typename TReader>
	FResult Evaluate(const FInputAction& Action, const TReader& Read, float DeltaSeconds)
	{
		FResult Result;
		for (const FInputBinding& Binding : Action.Bindings)
		{
			const FVector2 Value = ApplyModifiers(Binding.Modifiers, Read(Binding.Source), DeltaSeconds);
			if (Action.Type == EInputActionType::Button)
			{
				const float Magnitude = GetMagnitude(EInputActionType::Axis2D, Value);
				Result.bActive        = Result.bActive || (Magnitude > 0.0f && Magnitude >= Action.ActuationThreshold);
			}
			else
			{
				Result.Value += Value;
			}
		}
		if (Action.Type == EInputActionType::Button)
		{
			Result.Value = FVector2(Result.bActive ? 1.0f : 0.0f, 0.0f);
			return Result;
		}
		if (Action.Type == EInputActionType::Axis1D)
		{
			Result.Value.Y = 0.0f;
		}
		const float Magnitude = GetMagnitude(Action.Type, Result.Value);
		Result.bActive        = Magnitude > 0.0f && Magnitude >= Action.ActuationThreshold;
		return Result;
	}
} // namespace InputActionMath
