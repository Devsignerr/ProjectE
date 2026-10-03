#include "Core/GameUserSettings.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

namespace
{
	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin(), [](char X, char Y)
		{
			return std::tolower(static_cast<unsigned char>(X)) == std::tolower(static_cast<unsigned char>(Y));
		});
	}

	// 창 크기 허용 범위 (FWindow 최소 추적 크기 320x240, 8K까지)
	uint32 ClampWindowSize(int64 Value, uint32 Minimum)
	{
		return static_cast<uint32>(std::clamp<int64>(Value, Minimum, 7680));
	}
} // namespace

const char* ToString(EWindowMode Mode)
{
	switch (Mode)
	{
	case EWindowMode::Windowed:             return "Windowed";
	case EWindowMode::BorderlessFullscreen: return "BorderlessFullscreen";
	}
	return "Windowed";
}

bool TryParseWindowMode(std::string_view Text, EWindowMode& OutMode)
{
	for (const EWindowMode Mode : { EWindowMode::Windowed, EWindowMode::BorderlessFullscreen })
	{
		if (EqualsIgnoreCase(Text, ToString(Mode)))
		{
			OutMode = Mode;
			return true;
		}
	}
	return false;
}

const char* ToString(EHdrOutputMode Mode)
{
	switch (Mode)
	{
	case EHdrOutputMode::Off:   return "Off";
	case EHdrOutputMode::Auto:  return "Auto";
	case EHdrOutputMode::Hdr10: return "Hdr10";
	case EHdrOutputMode::ScRgb: return "ScRgb";
	}
	return "Off";
}

bool TryParseHdrOutputMode(std::string_view Text, EHdrOutputMode& OutMode)
{
	for (const EHdrOutputMode Mode : { EHdrOutputMode::Off, EHdrOutputMode::Auto, EHdrOutputMode::Hdr10, EHdrOutputMode::ScRgb })
	{
		if (EqualsIgnoreCase(Text, ToString(Mode)))
		{
			OutMode = Mode;
			return true;
		}
	}
	return false;
}

const char* ToString(EResolutionQuality Quality)
{
	switch (Quality)
	{
	case EResolutionQuality::Native:      return "Native";
	case EResolutionQuality::Quality:     return "Quality";
	case EResolutionQuality::Balanced:    return "Balanced";
	case EResolutionQuality::Performance: return "Performance";
	}
	return "Native";
}

bool TryParseResolutionQuality(std::string_view Text, EResolutionQuality& OutQuality)
{
	for (const EResolutionQuality Quality :
	     { EResolutionQuality::Native, EResolutionQuality::Quality, EResolutionQuality::Balanced, EResolutionQuality::Performance })
	{
		if (EqualsIgnoreCase(Text, ToString(Quality)))
		{
			OutQuality = Quality;
			return true;
		}
	}
	return false;
}

bool FGameUserSettings::ApplyJson(std::string_view Json)
{
	const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Root.is_discarded() || !Root.is_object())
	{
		return false;
	}
	if (const auto It = Root.find("WindowMode"); It != Root.end() && It->is_string())
	{
		EWindowMode Mode = WindowMode;
		if (TryParseWindowMode(It->get<std::string>(), Mode))
		{
			WindowMode = Mode;
		}
		else
		{
			E_LOG(LogCore, Warning, "알 수 없는 WindowMode \"{}\" — 무시합니다", It->get<std::string>());
		}
	}
	if (const auto It = Root.find("WindowWidth"); It != Root.end() && It->is_number_integer())
	{
		WindowWidth = ClampWindowSize(It->get<int64>(), 320);
	}
	if (const auto It = Root.find("WindowHeight"); It != Root.end() && It->is_number_integer())
	{
		WindowHeight = ClampWindowSize(It->get<int64>(), 240);
	}
	if (const auto It = Root.find("VSync"); It != Root.end() && It->is_boolean())
	{
		bVSync = It->get<bool>();
	}
	if (const auto It = Root.find("ResolutionQuality"); It != Root.end() && It->is_string())
	{
		EResolutionQuality Quality = ResolutionQuality;
		if (TryParseResolutionQuality(It->get<std::string>(), Quality))
		{
			ResolutionQuality = Quality;
		}
		else
		{
			E_LOG(LogCore, Warning, "알 수 없는 ResolutionQuality \"{}\" — 무시합니다", It->get<std::string>());
		}
	}
	if (const auto It = Root.find("DynamicResolution"); It != Root.end() && It->is_boolean())
	{
		bDynamicResolution = It->get<bool>();
	}
	if (const auto It = Root.find("DynamicResolutionTargetMs"); It != Root.end() && It->is_number())
	{
		DynamicResolutionTargetMs = std::clamp(It->get<float>(), 1.0f, 100.0f);
	}
	if (const auto It = Root.find("HdrOutput"); It != Root.end() && It->is_string())
	{
		EHdrOutputMode Mode = HdrOutput;
		if (TryParseHdrOutputMode(It->get<std::string>(), Mode))
		{
			HdrOutput = Mode;
		}
		else
		{
			E_LOG(LogCore, Warning, "알 수 없는 HdrOutput \"{}\" — 무시합니다", It->get<std::string>());
		}
	}
	if (const auto It = Root.find("HdrPaperWhiteNits"); It != Root.end() && It->is_number())
	{
		HdrPaperWhiteNits = std::clamp(It->get<float>(), 80.0f, 1000.0f);
	}
	if (const auto It = Root.find("HdrMaxNits"); It != Root.end() && It->is_number())
	{
		HdrMaxNits = std::clamp(It->get<float>(), 0.0f, 10000.0f);
	}
	return true;
}

std::string FGameUserSettings::ToJson() const
{
	nlohmann::ordered_json Root;
	Root["WindowMode"]   = ToString(WindowMode);
	Root["WindowWidth"]  = WindowWidth;
	Root["WindowHeight"] = WindowHeight;
	Root["VSync"]        = bVSync;
	Root["ResolutionQuality"]         = ToString(ResolutionQuality);
	Root["DynamicResolution"]         = bDynamicResolution;
	Root["DynamicResolutionTargetMs"] = DynamicResolutionTargetMs;
	Root["HdrOutput"]                 = ToString(HdrOutput);
	Root["HdrPaperWhiteNits"]         = HdrPaperWhiteNits;
	Root["HdrMaxNits"]                = HdrMaxNits;
	return Root.dump(2) + "\n";
}

bool FGameUserSettings::ApplyFile(const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	if (!std::filesystem::exists(Path, ErrorCode))
	{
		return true;
	}
	std::ifstream File(Path, std::ios::binary);
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!File || !ApplyJson(Buffer.str()))
	{
		E_LOG(LogCore, Warning, "사용자 설정 파일을 읽을 수 없습니다 (기본값 사용): {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

bool FGameUserSettings::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogCore, Warning, "사용자 설정을 저장할 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJson();
	return static_cast<bool>(File);
}

FGameUserSettings FGameUserSettings::Load()
{
	FGameUserSettings Settings = FProjectSettings::Get().Display; // 프로젝트 기본값
	Settings.ApplyFile(GetUserSettingsPath());
	return Settings;
}

std::filesystem::path FGameUserSettings::GetUserSettingsPath()
{
	return FPaths::GetSavedDirectory() / L"Config" / L"GameUserSettings.json";
}

bool FGameUserSettings::Save() const
{
	return SaveToFile(GetUserSettingsPath());
}
