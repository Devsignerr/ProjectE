#include "Core/Settings/InputSettings.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <fstream>

FInputSettings::FInputSettings()
{
	SetProjectMapping(MakeDefaultMapping());
}

FInputMapping FInputSettings::MakeDefaultMapping()
{
	FInputMapping Mapping;

	FInputAction Move;
	Move.Name        = "Move";
	Move.Type        = EInputActionType::Axis2D;
	Move.Description = "이동 (X = 오른쪽, Y = 앞)";
	Move.ActuationThreshold = 0.1f;
	Move.Bindings.push_back({ FInputSource::Key(EKey::W), { FInputModifier::MakeSwizzle() } });
	Move.Bindings.push_back({ FInputSource::Key(EKey::S), { FInputModifier::MakeSwizzle(), FInputModifier::MakeNegate() } });
	Move.Bindings.push_back({ FInputSource::Key(EKey::D), {} });
	Move.Bindings.push_back({ FInputSource::Key(EKey::A), { FInputModifier::MakeNegate() } });
	Move.Bindings.push_back({ FInputSource::Gamepad(EGamepadAxis::LeftStick), { FInputModifier::MakeDeadZone(0.25f, 1.0f, true) } });
	Mapping.Actions.push_back(std::move(Move));

	// 시점: 마우스 원시 이동(카운트/프레임)과 같은 단위로 맞춘다 — 스틱은 초당 1500카운트 × 프레임 시간, 위로 밀면 마우스를 위로 민 것(-Y)과 같게
	FInputAction Look;
	Look.Name        = "Look";
	Look.Type        = EInputActionType::Axis2D;
	Look.Description = "시점 (마우스 카운트/프레임 단위, 오른쪽 +X, 아래 +Y)";
	Look.ActuationThreshold = 0.01f;
	Look.Bindings.push_back({ FInputSource::Mouse(EMouseAxis::XY), {} });
	Look.Bindings.push_back({ FInputSource::Gamepad(EGamepadAxis::RightStick),
	                          { FInputModifier::MakeDeadZone(0.25f, 1.0f, true), FInputModifier::MakeNegate(false, true),
	                            FInputModifier::MakeScale(FVector2(1500.0f, 1000.0f)), FInputModifier::MakeScaleByDeltaTime() } });
	Mapping.Actions.push_back(std::move(Look));

	FInputAction Jump;
	Jump.Name        = "Jump";
	Jump.Type        = EInputActionType::Button;
	Jump.Description = "점프";
	Jump.Bindings.push_back({ FInputSource::Key(EKey::Space), {} });
	Jump.Bindings.push_back({ FInputSource::Gamepad(EGamepadButton::A), {} });
	Mapping.Actions.push_back(std::move(Jump));
	return Mapping;
}

void FInputSettings::SetProjectMapping(FInputMapping Mapping)
{
	ProjectMapping = std::move(Mapping);
	Rebuild();
}

void FInputSettings::Rebuild()
{
	FInputMapping Result = ProjectMapping;
	for (FInputAction& Action : Result.Actions)
	{
		if (const FInputAction* Override = UserBindings.Find(Action.Name))
		{
			Action.Bindings = Override->Bindings;
		}
	}
	Effective = std::move(Result); // 같은 객체에 대입 — FInput이 들고 있는 주소는 그대로
}

std::filesystem::path FInputSettings::GetUserBindingsPath()
{
	return FPaths::GetSavedDirectory() / L"Config" / L"InputBindings.json";
}

bool FInputSettings::UserBindingsFromJson(std::string_view Json, std::string* Error)
{
	FInputMapping Loaded;
	if (!FInputMapping::FromJson(Json, Loaded, Error))
	{
		return false;
	}
	UserBindings = std::move(Loaded);
	Rebuild();
	return true;
}

bool FInputSettings::LoadUserBindings()
{
	if (!bUserFileEnabled)
	{
		return true;
	}
	const std::filesystem::path Path = GetUserBindingsPath();
	std::error_code             ErrorCode;
	if (!std::filesystem::exists(Path, ErrorCode)) // 사용자 파일은 pak이 아니라 디스크 (<Saved>)
	{
		return true;
	}
	std::ifstream File(Path, std::ios::binary);
	std::string   Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	std::string   Error;
	if (!File || !UserBindingsFromJson(Text, &Error))
	{
		E_LOG(LogCore, Warning, "입력 재지정 파일을 읽지 못해 프로젝트 바인딩을 씁니다: {} ({})", FStringConv::ToUtf8(Path.wstring()), Error);
		return false;
	}
	if (!Error.empty())
	{
		E_LOG(LogCore, Warning, "입력 재지정 파일 일부를 건너뜀: {}", Error);
	}
	E_LOG(LogCore, Log, "입력 재지정 {}개 액션 적용: {}", UserBindings.Actions.size(), FStringConv::ToUtf8(Path.wstring()));
	return true;
}

bool FInputSettings::SaveUserBindings() const
{
	const std::filesystem::path Path = GetUserBindingsPath();
	std::error_code             ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogCore, Error, "입력 재지정을 저장할 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << UserBindings.ToJson();
	return static_cast<bool>(File);
}

void FInputSettings::SaveUserBindingsIfEnabled() const
{
	if (bUserFileEnabled)
	{
		SaveUserBindings();
	}
}

bool FInputSettings::Rebind(std::string_view ActionName, const FInputSource& OldSource, const FInputSource& NewSource)
{
	const FInputAction* Current = Effective.Find(ActionName);
	if (Current == nullptr || !NewSource.IsValid())
	{
		return false;
	}
	std::vector<FInputBinding> Bindings = Current->Bindings;
	if (OldSource.IsValid())
	{
		const auto Found = std::find_if(Bindings.begin(), Bindings.end(), [&OldSource](const FInputBinding& Binding) { return Binding.Source == OldSource; });
		if (Found == Bindings.end())
		{
			return false;
		}
		Found->Source = NewSource;
	}
	else
	{
		Bindings.push_back({ NewSource, {} });
	}

	FInputAction* Override = UserBindings.Find(ActionName);
	if (Override == nullptr)
	{
		FInputAction Added;
		Added.Name = Current->Name;
		Added.Type = Current->Type;
		UserBindings.Actions.push_back(std::move(Added));
		Override = &UserBindings.Actions.back();
	}
	Override->Bindings = std::move(Bindings);
	Rebuild();
	SaveUserBindingsIfEnabled();
	return true;
}

void FInputSettings::ResetUserBindings(std::string_view ActionName)
{
	if (ActionName.empty())
	{
		UserBindings.Actions.clear();
	}
	else
	{
		std::erase_if(UserBindings.Actions, [ActionName](const FInputAction& Action) { return Action.Name == ActionName; });
	}
	Rebuild();
	SaveUserBindingsIfEnabled();
}
