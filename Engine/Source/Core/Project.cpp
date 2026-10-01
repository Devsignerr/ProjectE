#include "Core/Project.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <fstream>
#include <sstream>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

bool FProjectLegacyFields::IsEmpty() const
{
	return DefaultScene.empty() && PlayerPrefab.empty() && DisplayName.empty() && Version.empty() && Company.empty() && ExecutableName.empty() &&
	       Icon.empty() && SteamAppId == 0;
}

bool FProjectDescriptor::LoadFromFile(const std::filesystem::path& Path)
{
	const std::string PathUtf8 = FStringConv::ToUtf8(Path.wstring());

	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogCore, Error, "프로젝트 파일을 열 수 없습니다: {}", PathUtf8);
		return false;
	}

	std::stringstream Buffer;
	Buffer << File.rdbuf();

	const nlohmann::json Json = nlohmann::json::parse(Buffer.str(), nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Json.is_discarded() || !Json.is_object())
	{
		E_LOG(LogCore, Error, "프로젝트 파일이 올바른 JSON 객체가 아닙니다: {}", PathUtf8);
		return false;
	}

	Name          = Json.value("Name", std::string());
	EngineVersion = Json.value("EngineVersion", std::string("0.1.0"));
	GameModule    = Json.value("GameModule", std::string());

	Legacy                = FProjectLegacyFields{};
	Legacy.DefaultScene   = Json.value("DefaultScene", std::string());
	Legacy.PlayerPrefab   = Json.value("PlayerPrefab", std::string());
	Legacy.DisplayName    = Json.value("DisplayName", std::string());
	Legacy.Version        = Json.value("Version", std::string());
	Legacy.Company        = Json.value("Company", std::string());
	Legacy.ExecutableName = Json.value("ExecutableName", std::string());
	Legacy.Icon           = Json.value("Icon", std::string());
	Legacy.SteamAppId     = Json.value("SteamAppId", 0u);

	if (Name.empty())
	{
		// 이름이 없으면 파일 이름으로 대체
		Name = FStringConv::ToUtf8(Path.stem().wstring());
	}
	return true;
}

bool FProjectDescriptor::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);

	nlohmann::json Json;
	Json["Name"]          = Name;
	Json["EngineVersion"] = EngineVersion;
	if (!GameModule.empty())
	{
		Json["GameModule"] = GameModule;
	}

	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogCore, Error, "프로젝트 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << Json.dump(2) << "\n";
	return static_cast<bool>(File);
}
