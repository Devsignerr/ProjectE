#include "Core/Input.h"
#include "Core/InputActions.h"
#include "Core/Settings/InputSettings.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Settings/SettingsRegistry.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>
#include <map>

namespace
{
	// 테스트용 원시 입력: 소스 이름 → 값
	struct FFakeReader
	{
		std::map<std::string, FVector2> Values;

		FVector2 operator()(const FInputSource& Source) const
		{
			const auto Found = Values.find(InputNames::ToString(Source));
			return Found != Values.end() ? Found->second : FVector2::ZeroVector;
		}
	};

	FInputSource Parse(std::string_view Name)
	{
		FInputSource Source;
		E_EXPECT_TRUE(InputNames::TryParseSource(Name, Source));
		return Source;
	}

	void PressKey(FInput& Input, EKey Key, bool bDown)
	{
		FWindowEvent Event;
		Event.Type = bDown ? EWindowEventType::KeyDown : EWindowEventType::KeyUp;
		Event.Key  = Key;
		Input.ProcessEvent(Event);
	}
} // namespace

// 수정자: 부정(축별), 축 바꿈, 데드존(원형/축별 — Lower~Upper를 0~1로), 배율, 프레임 시간 배율
E_TEST(InputActions_Modifiers)
{
	using namespace InputActionMath;
	E_EXPECT_EQUALS(ApplyModifier(FInputModifier::MakeNegate(), FVector2(1.0f, -2.0f), 0.0f), FVector2(-1.0f, 2.0f), 1.0e-6f);
	E_EXPECT_EQUALS(ApplyModifier(FInputModifier::MakeNegate(false, true), FVector2(1.0f, 2.0f), 0.0f), FVector2(1.0f, -2.0f), 1.0e-6f);
	E_EXPECT_EQUALS(ApplyModifier(FInputModifier::MakeSwizzle(), FVector2(1.0f, 0.0f), 0.0f), FVector2(0.0f, 1.0f), 1.0e-6f);
	E_EXPECT_EQUALS(ApplyModifier(FInputModifier::MakeScale(FVector2(2.0f, 3.0f)), FVector2(1.0f, 1.0f), 0.0f), FVector2(2.0f, 3.0f), 1.0e-6f);
	E_EXPECT_EQUALS(ApplyModifier(FInputModifier::MakeScaleByDeltaTime(), FVector2(10.0f, -20.0f), 0.5f), FVector2(5.0f, -10.0f), 1.0e-6f);

	// 원형 데드존: 길이 0.2 이하 0, 0.6 → (0.6-0.2)/0.8 = 0.5 (방향 유지), 1 넘으면 1
	const FInputModifier Radial = FInputModifier::MakeDeadZone(0.2f, 1.0f, true);
	E_EXPECT_EQUALS(ApplyModifier(Radial, FVector2(0.1f, 0.1f), 0.0f), FVector2::ZeroVector, 1.0e-6f);
	E_EXPECT_EQUALS(ApplyModifier(Radial, FVector2(0.0f, -0.6f), 0.0f), FVector2(0.0f, -0.5f), 1.0e-5f);
	E_EXPECT_NEAR(ApplyModifier(Radial, FVector2(0.9f, 0.9f), 0.0f).Length(), 1.0f, 1.0e-5f);
	// 축별 데드존: 각 축 따로 (부호 유지)
	const FInputModifier Axial = FInputModifier::MakeDeadZone(0.2f, 1.0f, false);
	E_EXPECT_EQUALS(ApplyModifier(Axial, FVector2(0.1f, -0.6f), 0.0f), FVector2(0.0f, -0.5f), 1.0e-5f);

	// 순서대로 적용: Swizzle 후 Negate = (0, -1)
	E_EXPECT_EQUALS(ApplyModifiers({ FInputModifier::MakeSwizzle(), FInputModifier::MakeNegate() }, FVector2(1.0f, 0.0f), 0.0f), FVector2(0.0f, -1.0f), 1.0e-6f);
}

// WASD → 2D 합성 (키별 수정자, 언리얼 방식) + 스틱. 값은 바인딩 합
E_TEST(InputActions_WasdComposite)
{
	const FInputMapping Mapping = FInputSettings::MakeDefaultMapping();
	const FInputAction* Move    = Mapping.Find("Move");
	E_EXPECT_TRUE(Move != nullptr && Move->Type == EInputActionType::Axis2D);
	if (Move == nullptr)
	{
		return;
	}
	const auto Eval = [&](std::map<std::string, FVector2> Values) { return InputActionMath::Evaluate(*Move, FFakeReader{ std::move(Values) }, 0.016f); };

	E_EXPECT_FALSE(Eval({}).bActive);
	E_EXPECT_EQUALS(Eval({ { "W", { 1, 0 } } }).Value, FVector2(0.0f, 1.0f), 1.0e-6f);
	E_EXPECT_EQUALS(Eval({ { "S", { 1, 0 } } }).Value, FVector2(0.0f, -1.0f), 1.0e-6f);
	E_EXPECT_EQUALS(Eval({ { "D", { 1, 0 } } }).Value, FVector2(1.0f, 0.0f), 1.0e-6f);
	E_EXPECT_EQUALS(Eval({ { "A", { 1, 0 } } }).Value, FVector2(-1.0f, 0.0f), 1.0e-6f);
	E_EXPECT_EQUALS(Eval({ { "W", { 1, 0 } }, { "D", { 1, 0 } } }).Value, FVector2(1.0f, 1.0f), 1.0e-6f);
	const InputActionMath::FResult Opposite = Eval({ { "W", { 1, 0 } }, { "S", { 1, 0 } } });
	E_EXPECT_EQUALS(Opposite.Value, FVector2::ZeroVector, 1.0e-6f);
	E_EXPECT_FALSE(Opposite.bActive);
	E_EXPECT_TRUE(Eval({ { "W", { 1, 0 } } }).bActive);

	// 왼쪽 스틱: 데드존 0.25 안쪽은 0, 끝까지 위로 = (0, 1)
	E_EXPECT_FALSE(Eval({ { "Gamepad_LeftStick", { 0.1f, 0.15f } } }).bActive);
	E_EXPECT_EQUALS(Eval({ { "Gamepad_LeftStick", { 0.0f, 1.0f } } }).Value, FVector2(0.0f, 1.0f), 1.0e-5f);
}

// 버튼 액션: 바인딩 중 하나라도 문턱 이상이면 켜짐. 트리거(0~1 축)도 버튼에 묶을 수 있다
E_TEST(InputActions_ButtonThreshold)
{
	FInputAction Fire;
	Fire.Name = "Fire";
	Fire.Type = EInputActionType::Button;
	Fire.Bindings.push_back({ Parse("MouseLeft"), {} });
	Fire.Bindings.push_back({ Parse("Gamepad_RightTrigger"), {} });
	const auto Eval = [&](std::map<std::string, FVector2> Values) { return InputActionMath::Evaluate(Fire, FFakeReader{ std::move(Values) }, 0.016f); };
	E_EXPECT_FALSE(Eval({}).bActive);
	E_EXPECT_FALSE(Eval({ { "Gamepad_RightTrigger", { 0.3f, 0 } } }).bActive);
	E_EXPECT_TRUE(Eval({ { "Gamepad_RightTrigger", { 0.6f, 0 } } }).bActive);
	E_EXPECT_EQUALS(Eval({ { "MouseLeft", { 1, 0 } } }).Value, FVector2(1.0f, 0.0f), 1.0e-6f);

	// 1D 축: 합 (Q = -1, E = +1)
	FInputAction Turn;
	Turn.Name = "Turn";
	Turn.Type = EInputActionType::Axis1D;
	Turn.Bindings.push_back({ Parse("Q"), { FInputModifier::MakeNegate() } });
	Turn.Bindings.push_back({ Parse("E"), {} });
	E_EXPECT_NEAR(InputActionMath::Evaluate(Turn, FFakeReader{ { { "Q", { 1, 0 } } } }, 0.0f).Value.X, -1.0f, 1.0e-6f);
	E_EXPECT_NEAR(InputActionMath::Evaluate(Turn, FFakeReader{ { { "Q", { 1, 0 } }, { "E", { 1, 0 } } } }, 0.0f).Value.X, 0.0f, 1.0e-6f);
}

// 소스 이름 왕복 (키/마우스/게임패드 전부), 모르는 이름은 실패
E_TEST(InputActions_SourceNames)
{
	for (const std::string& Name : InputNames::GetAllSourceNames())
	{
		FInputSource Source;
		E_EXPECT_TRUE(InputNames::TryParseSource(Name, Source));
		E_EXPECT_EQ(InputNames::ToString(Source), Name);
	}
	FInputSource Source;
	E_EXPECT_TRUE(InputNames::TryParseSource("Space", Source) && Source == FInputSource::Key(EKey::Space));
	E_EXPECT_TRUE(InputNames::TryParseSource("Gamepad_A", Source) && Source == FInputSource::Gamepad(EGamepadButton::A));
	E_EXPECT_TRUE(InputNames::TryParseSource("Gamepad_RightStick", Source) && Source.Is2D());
	E_EXPECT_TRUE(InputNames::TryParseSource("MouseXY", Source) && Source.Is2D());
	E_EXPECT_FALSE(InputNames::TryParseSource("Gamepad_Z", Source));
	E_EXPECT_FALSE(InputNames::TryParseSource("NoSuchKey", Source));
}

// JSON 왕복 + 배치 해시. 모르는 소스/수정자는 그 항목만 건너뛰고 경고 문구를 남긴다
E_TEST(InputActions_JsonRoundTrip)
{
	const FInputMapping Default = FInputSettings::MakeDefaultMapping();
	FInputMapping       Loaded;
	E_EXPECT_TRUE(FInputMapping::FromJson(Default.ToJson(), Loaded));
	E_EXPECT_EQ(Loaded.ToJson(), Default.ToJson());
	E_EXPECT_EQ(Loaded.GetLayoutHash(), Default.GetLayoutHash());
	const FInputAction* Look = Loaded.Find("Look");
	E_EXPECT_TRUE(Look != nullptr && Look->Bindings.size() == 2 && Look->Bindings[1].Modifiers.size() == 4);

	std::string Error;
	E_EXPECT_TRUE(FInputMapping::FromJson(R"({ "Actions": [ { "Name": "Use", "Type": "Button", "Bindings": [
		{ "Source": "E" }, { "Source": "Nope" }, { "Source": "Gamepad_X", "Modifiers": [ { "Type": "Bogus" }, { "Type": "Negate", "X": false } ] } ] } ] })",
	                                      Loaded, &Error));
	E_EXPECT_FALSE(Error.empty());
	const FInputAction* Use = Loaded.Find("Use");
	E_EXPECT_TRUE(Use != nullptr && Use->Bindings.size() == 2 && Use->Bindings[1].Modifiers.size() == 1 && !Use->Bindings[1].Modifiers[0].bX);
	E_EXPECT_FALSE(FInputMapping::FromJson("{ 깨진", Loaded));

	// 액션 이름/종류가 바뀌면 해시가 바뀐다 (바인딩은 무관)
	FInputMapping Changed = Default;
	Changed.Actions[0].Bindings.clear();
	E_EXPECT_EQ(Changed.GetLayoutHash(), Default.GetLayoutHash());
	Changed.Actions[0].Type = EInputActionType::Axis1D;
	E_EXPECT_TRUE(Changed.GetLayoutHash() != Default.GetLayoutHash());
}

// FInput: 키/게임패드 → 액션 값, 눌림/떼어짐(EndFrame 기준), UI가 키보드를 가져간 사본은 다시 계산
E_TEST(InputActions_FInputStates)
{
	const FInputMapping Mapping = FInputSettings::MakeDefaultMapping();
	FInput              Input;
	Input.UpdateActions(Mapping, 0.016f);
	E_EXPECT_FALSE(Input.IsActionDown("Jump"));
	E_EXPECT_EQUALS(Input.GetActionValue("Move"), FVector2::ZeroVector, 1.0e-6f);

	PressKey(Input, EKey::Space, true);
	PressKey(Input, EKey::W, true);
	Input.UpdateActions(Mapping, 0.016f);
	E_EXPECT_TRUE(Input.IsActionDown("Jump") && Input.WasActionPressed("Jump"));
	E_EXPECT_EQUALS(Input.GetActionValue("Move"), FVector2(0.0f, 1.0f), 1.0e-6f);
	const FInput NoKeyboard = Input.WithoutKeyboard();
	E_EXPECT_FALSE(NoKeyboard.IsActionDown("Jump"));
	E_EXPECT_EQUALS(NoKeyboard.GetActionValue("Move"), FVector2::ZeroVector, 1.0e-6f);
	Input.EndFrame();

	Input.UpdateActions(Mapping, 0.016f);
	E_EXPECT_TRUE(Input.IsActionDown("Jump") && !Input.WasActionPressed("Jump"));
	PressKey(Input, EKey::Space, false);
	Input.UpdateActions(Mapping, 0.016f);
	E_EXPECT_TRUE(!Input.IsActionDown("Jump") && Input.WasActionReleased("Jump"));
	Input.EndFrame();
	PressKey(Input, EKey::W, false);

	// 가짜 게임패드: A = 점프, 왼쪽 스틱 = 이동, 오른쪽 스틱 = 시점 (위로 밀면 -Y, 초당 값 × 프레임 시간)
	FGamepadState Pad;
	Pad.bConnected = true;
	Pad.SetButton(EGamepadButton::A, true);
	Pad.LeftX  = 1.0f;
	Pad.RightY = 1.0f;
	Input.SetGamepadState(Pad);
	Input.UpdateActions(Mapping, 0.5f);
	E_EXPECT_TRUE(Input.IsGamepadConnected() && Input.IsGamepadButtonPressed(EGamepadButton::A));
	E_EXPECT_TRUE(Input.WasActionPressed("Jump"));
	E_EXPECT_EQUALS(Input.GetActionValue("Move"), FVector2(1.0f, 0.0f), 1.0e-5f);
	E_EXPECT_EQUALS(Input.GetActionValue("Look"), FVector2(0.0f, -500.0f), 1.0e-3f);
	Input.EndFrame();
	E_EXPECT_FALSE(Input.IsGamepadButtonPressed(EGamepadButton::A));

	// 없는 액션은 0/false
	E_EXPECT_FALSE(Input.IsActionDown("NoSuchAction"));
	E_EXPECT_TRUE(Input.FindAction("NoSuchAction") == nullptr);
}

// 서버 원격 입력: 받은 값으로 교체 (다시 계산하지 않음), 눌림 판정은 EndFrame 기준
E_TEST(InputActions_SetActionValues)
{
	const FInputMapping            Mapping = FInputSettings::MakeDefaultMapping();
	std::vector<FInputActionState> Values(Mapping.Actions.size());
	Values[2].Value   = FVector2(1.0f, 0.0f); // Jump
	Values[2].bActive = true;
	Values[0].Value   = FVector2(0.5f, 0.5f); // Move
	Values[0].bActive = true;
	FInput Remote;
	Remote.SetActionValues(Mapping, Values);
	E_EXPECT_TRUE(Remote.WasActionPressed("Jump"));
	E_EXPECT_EQUALS(Remote.GetActionValue("Move"), FVector2(0.5f, 0.5f), 1.0e-6f);
	Remote.EndFrame();
	E_EXPECT_TRUE(Remote.IsActionDown("Jump") && !Remote.WasActionPressed("Jump"));
	E_EXPECT_TRUE(Remote.WithoutKeyboard().IsActionDown("Jump")); // 원격 값은 키 상태와 무관
}

// 플레이어 재지정: 바인딩 교체/추가/되돌리기, 사용자 파일 꺼짐(기본)이면 파일을 쓰지 않는다
E_TEST(InputActions_UserRebind)
{
	FInputSettings Settings;
	E_EXPECT_FALSE(Settings.IsUserFileEnabled());
	const FInputMapping& Effective = Settings.GetEffectiveMapping();
	E_EXPECT_TRUE(Settings.Rebind("Jump", FInputSource::Key(EKey::Space), FInputSource::Key(EKey::K)));
	E_EXPECT_TRUE(&Effective == &Settings.GetEffectiveMapping()); // 주소 고정
	E_EXPECT_TRUE(Effective.Find("Jump")->Bindings[0].Source == FInputSource::Key(EKey::K));
	E_EXPECT_TRUE(Settings.GetProjectMapping().Find("Jump")->Bindings[0].Source == FInputSource::Key(EKey::Space));
	E_EXPECT_FALSE(Settings.Rebind("Jump", FInputSource::Key(EKey::Space), FInputSource::Key(EKey::L))); // 이미 없음
	E_EXPECT_FALSE(Settings.Rebind("NoSuchAction", {}, FInputSource::Key(EKey::L)));
	E_EXPECT_TRUE(Settings.Rebind("Jump", {}, FInputSource::Mouse(EMouseButton::Right))); // 추가
	E_EXPECT_EQ(Effective.Find("Jump")->Bindings.size(), 3u);

	// 사용자 파일 형식 왕복 → 프로젝트 매핑이 바뀌어도 재지정은 덮인다
	const std::string Json = Settings.UserBindingsToJson();
	FInputSettings    Other;
	E_EXPECT_TRUE(Other.UserBindingsFromJson(Json));
	E_EXPECT_TRUE(Other.GetEffectiveMapping().Find("Jump")->Bindings[0].Source == FInputSource::Key(EKey::K));
	FInputMapping Project = FInputSettings::MakeDefaultMapping();
	Project.Actions[0].Description = "바뀜";
	Other.SetProjectMapping(Project);
	E_EXPECT_TRUE(Other.HasUserBindings("Jump") && Other.GetEffectiveMapping().Find("Jump")->Bindings[0].Source == FInputSource::Key(EKey::K));

	Settings.ResetUserBindings("Jump");
	E_EXPECT_FALSE(Settings.HasUserBindings("Jump"));
	E_EXPECT_TRUE(Effective.Find("Jump")->Bindings[0].Source == FInputSource::Key(EKey::Space));
}

// 설정 섹션 "Input": 사용자 정의 JSON으로 파일 저장/읽기, 기본값으로 되돌리기
E_TEST(InputActions_SettingsSection)
{
	FSettingsSection* Section = FSettingsRegistry::Get().Find("Input");
	E_EXPECT_TRUE(Section != nullptr && Section->IsCustom() && Section->Scope == ESettingsScope::Project);
	if (Section == nullptr)
	{
		return;
	}
	FInputSettings&     Input    = FProjectSettings::Get().Input;
	const FInputMapping Original = Input.GetProjectMapping();

	FInputMapping Edited = Original;
	FInputAction  Crouch;
	Crouch.Name = "Crouch";
	Crouch.Bindings.push_back({ FInputSource::Key(EKey::C), {} });
	Edited.Actions.push_back(Crouch);
	Input.SetProjectMapping(Edited);
	const std::filesystem::path Path = FTestRegistry::GetTempDirectory() / L"ProjectEInputSettingsTest.json";
	E_EXPECT_TRUE(Section->SaveTo(Path));

	Section->ResetToDefaults();
	E_EXPECT_TRUE(Input.GetProjectMapping().Find("Crouch") == nullptr);
	E_EXPECT_TRUE(Section->LoadFrom(Path));
	E_EXPECT_TRUE(Input.GetEffectiveMapping().Find("Crouch") != nullptr);

	Input.SetProjectMapping(Original);
	std::error_code ErrorCode;
	std::filesystem::remove(Path, ErrorCode);
}
