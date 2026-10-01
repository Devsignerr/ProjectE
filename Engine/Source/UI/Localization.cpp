#include "UI/Localization.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "UI/UIPlatform.h"
#include "UI/UITypes.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace
{
	using nlohmann::json;

	std::string Trim(std::string_view Text)
	{
		size_t Begin = 0;
		size_t End   = Text.size();
		while (Begin < End && std::isspace(static_cast<unsigned char>(Text[Begin])))
		{
			++Begin;
		}
		while (End > Begin && std::isspace(static_cast<unsigned char>(Text[End - 1])))
		{
			--End;
		}
		return std::string(Text.substr(Begin, End - Begin));
	}

	bool IsDigits(std::string_view Text)
	{
		return !Text.empty() && std::all_of(Text.begin(), Text.end(), [](char Char) { return Char >= '0' && Char <= '9'; });
	}
} // namespace

// ---------------------------------------------------------------- FStringTable

bool FStringTable::FromJsonString(const std::string& JsonText, std::string* OutError)
{
	const json Document = json::parse(JsonText, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		if (OutError != nullptr)
		{
			*OutError = "JSON 파싱 실패";
		}
		return false;
	}
	Languages.clear();
	Strings.clear();
	if (const auto It = Document.find("Languages"); It != Document.end() && It->is_array())
	{
		for (const json& Value : *It)
		{
			if (Value.is_string())
			{
				AddLanguage(Value.get<std::string>());
			}
		}
	}
	if (const auto It = Document.find("Strings"); It != Document.end() && It->is_object())
	{
		for (const auto& [Key, Values] : It->items())
		{
			std::map<std::string, std::string>& Entry = Strings[Key];
			if (!Values.is_object())
			{
				continue;
			}
			for (const auto& [LanguageKey, Value] : Values.items())
			{
				if (Value.is_string())
				{
					const std::string Code = FLocalization::NormalizeLanguage(LanguageKey);
					Entry[Code]            = Value.get<std::string>();
					AddLanguage(Code); // 목록에 빠진 언어도 값이 있으면 보이게
				}
			}
		}
	}
	return true;
}

std::string FStringTable::ToJsonString() const
{
	json Document;
	Document["Version"]   = Version;
	Document["Languages"] = Languages;
	json Object           = json::object();
	for (const auto& [Key, Values] : Strings)
	{
		json Entry = json::object();
		for (const auto& [Code, Value] : Values)
		{
			if (!Value.empty())
			{
				Entry[Code] = Value;
			}
		}
		Object[Key] = std::move(Entry);
	}
	Document["Strings"] = std::move(Object);
	return Document.dump(2);
}

bool FStringTable::LoadFromFile(const std::filesystem::path& Path)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		E_LOG(LogUI, Error, "문자열 표를 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::string Error;
	if (!FromJsonString(Text, &Error))
	{
		E_LOG(LogUI, Error, "문자열 표 로드 실패 ({}): {}", Error, FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

bool FStringTable::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogUI, Error, "문자열 표를 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return static_cast<bool>(File);
}

bool FStringTable::AddLanguage(std::string_view Language)
{
	const std::string Code = FLocalization::NormalizeLanguage(Language);
	if (Code.empty() || std::find(Languages.begin(), Languages.end(), Code) != Languages.end())
	{
		return false;
	}
	Languages.push_back(Code);
	return true;
}

// ---------------------------------------------------------------- FLocalization

FLocalization& FLocalization::Get()
{
	static FLocalization Instance; // 엔진 DLL 안 하나
	return Instance;
}

void FLocalization::EnsureInitialized()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;
	if (!FPaths::IsInitialized() || !FPaths::HasProject())
	{
		return;
	}
	DefaultLanguage = NormalizeLanguage(FProjectSettings::Get().Localization.DefaultLanguage);
	LoadProjectTables();
	ChooseInitialLanguage();
}

void FLocalization::LoadProjectTables()
{
	const std::filesystem::path Content = FPaths::GetProjectContentDirectory();
	std::vector<std::filesystem::path> Files;
	const std::string& List = FProjectSettings::Get().Localization.StringTables;
	if (!Trim(List).empty())
	{
		size_t Start = 0;
		while (Start <= List.size())
		{
			const size_t      End  = std::min(List.find(';', Start), List.size());
			const std::string Item = Trim(std::string_view(List).substr(Start, End - Start));
			if (!Item.empty())
			{
				Files.push_back(Content / FStringConv::ToWide(Item));
			}
			Start = End + 1;
		}
	}
	else
	{
		// 지정이 없으면 디스크 폴더를 뒤진다 (pak은 목록을 모르므로 설정에 적어야 한다)
		std::error_code             ErrorCode;
		const std::filesystem::path Folder = Content / L"Localization";
		if (std::filesystem::is_directory(Folder, ErrorCode))
		{
			for (const auto& Entry : std::filesystem::recursive_directory_iterator(Folder, ErrorCode))
			{
				if (Entry.is_regular_file() && Entry.path().extension() == FStringTable::Extension)
				{
					Files.push_back(Entry.path());
				}
			}
			std::sort(Files.begin(), Files.end());
		}
	}
	for (const std::filesystem::path& File : Files)
	{
		FStringTable Table;
		if (Table.LoadFromFile(File))
		{
			AddTable(Table);
		}
	}
	if (!Files.empty())
	{
		E_LOG(LogUI, Log, "문자열 표 {}개: 키 {}개, 언어 {}개", Files.size(), Strings.size(), Languages.size());
	}
}

void FLocalization::ChooseInitialLanguage()
{
	std::string Chosen;
	// 명령줄 (자동 검증/테스트)
	const std::wstring CommandLineValue = FCommandLine::FromProcess().GetValue(L"--language");
	if (!CommandLineValue.empty())
	{
		Chosen = MatchLanguage(FStringConv::ToUtf8(CommandLineValue), Languages);
		if (Chosen.empty())
		{
			E_LOG(LogUI, Warning, "--language {}: 문자열 표에 없는 언어입니다", FStringConv::ToUtf8(CommandLineValue));
		}
	}
	// 사용자 설정
	if (Chosen.empty())
	{
		std::string Text;
		const std::filesystem::path UserPath = GetUserSettingsPath();
		std::error_code             ErrorCode;
		if (std::filesystem::exists(UserPath, ErrorCode) && FFileSystem::ReadTextFile(UserPath, Text))
		{
			const json Document = json::parse(Text, nullptr, false, true);
			if (Document.is_object() && Document.contains("Language") && Document["Language"].is_string())
			{
				Chosen = MatchLanguage(Document["Language"].get<std::string>(), Languages);
			}
		}
	}
	// 시스템 언어
	if (Chosen.empty() && FProjectSettings::Get().Localization.bDetectSystemLanguage)
	{
		Chosen = MatchLanguage(FUIPlatform::GetSystemLanguage(), Languages);
	}
	if (Chosen.empty())
	{
		Chosen = MatchLanguage(DefaultLanguage, Languages);
	}
	if (Chosen.empty() && !Languages.empty())
	{
		Chosen = Languages.front();
	}
	if (Chosen.empty())
	{
		Chosen = DefaultLanguage;
	}
	if (Chosen != Language)
	{
		Language = Chosen;
		++Revision;
	}
	if (!Languages.empty())
	{
		E_LOG(LogUI, Log, "언어: {}", Language);
	}
}

void FLocalization::Reload()
{
	const bool bWasInitialized = bInitialized;
	Strings.clear();
	Languages.clear();
	MissingKeys.clear();
	++Revision;
	if (!bWasInitialized)
	{
		EnsureInitialized();
		return;
	}
	if (FPaths::IsInitialized() && FPaths::HasProject())
	{
		DefaultLanguage = NormalizeLanguage(FProjectSettings::Get().Localization.DefaultLanguage);
		LoadProjectTables();
		if (MatchLanguage(Language, Languages).empty())
		{
			ChooseInitialLanguage();
		}
	}
}

void FLocalization::ResetForTests()
{
	Strings.clear();
	Languages.clear();
	MissingKeys.clear();
	Language.clear();
	DefaultLanguage.clear();
	bInitialized = true;
	++Revision;
}

void FLocalization::AddTable(const FStringTable& Table)
{
	for (const std::string& Code : Table.Languages)
	{
		if (std::find(Languages.begin(), Languages.end(), Code) == Languages.end())
		{
			Languages.push_back(Code);
		}
	}
	for (const auto& [Key, Values] : Table.Strings)
	{
		FLanguageValues& Target = Strings[Key];
		for (const auto& [Code, Value] : Values)
		{
			if (!Value.empty())
			{
				Target[Code] = Value;
			}
		}
	}
	MissingKeys.clear();
	++Revision;
}

const std::string& FLocalization::GetLanguage()
{
	EnsureInitialized();
	return Language;
}

const std::string& FLocalization::GetDefaultLanguage()
{
	EnsureInitialized();
	return DefaultLanguage;
}

std::vector<std::string> FLocalization::GetLanguages()
{
	EnsureInitialized();
	return Languages;
}

bool FLocalization::SetLanguage(std::string_view InLanguage, bool bSaveUserSetting)
{
	EnsureInitialized();
	const std::string Code = MatchLanguage(InLanguage, Languages);
	if (Code.empty())
	{
		E_LOG(LogUI, Warning, "문자열 표에 없는 언어입니다: {}", InLanguage);
		return false;
	}
	if (Code != Language)
	{
		Language = Code;
		++Revision;
		E_LOG(LogUI, Log, "언어 변경: {}", Language);
	}
	if (bSaveUserSetting && FPaths::IsInitialized())
	{
		const std::filesystem::path Path = GetUserSettingsPath();
		std::error_code             ErrorCode;
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		json          Document;
		Document["Language"] = Language;
		File << Document.dump(2);
		if (!File)
		{
			E_LOG(LogUI, Warning, "언어 설정을 저장하지 못했습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		}
	}
	return true;
}

void FLocalization::SetDefaultLanguage(std::string_view InLanguage)
{
	EnsureInitialized();
	DefaultLanguage = NormalizeLanguage(InLanguage);
	++Revision;
}

const std::string* FLocalization::Find(std::string_view Key, std::string_view InLanguage)
{
	EnsureInitialized();
	const auto It = Strings.find(Key);
	if (It == Strings.end())
	{
		return nullptr;
	}
	const auto Value = It->second.find(InLanguage);
	return Value != It->second.end() ? &Value->second : nullptr;
}

const std::string& FLocalization::Lookup(std::string_view Key)
{
	EnsureInitialized();
	if (const auto It = Strings.find(Key); It != Strings.end())
	{
		if (const auto Value = It->second.find(Language); Value != It->second.end())
		{
			return Value->second;
		}
		if (const auto Value = It->second.find(DefaultLanguage); Value != It->second.end())
		{
			return Value->second;
		}
		if (!It->second.empty())
		{
			return It->second.begin()->second; // 기본 언어도 없으면 아무 번역이라도
		}
	}
	auto Missing = MissingKeys.find(Key);
	if (Missing == MissingKeys.end())
	{
		E_LOG(LogUI, Warning, "문자열 표에 없는 키: \"{}\" (키를 그대로 표시합니다)", Key);
		Missing = MissingKeys.emplace(Key).first;
	}
	return *Missing;
}

std::string FLocalization::Get(std::string_view Key, const FLocFormatArgs& Args)
{
	return Format(Lookup(Key), Args);
}

bool FLocalization::Has(std::string_view Key)
{
	EnsureInitialized();
	return Strings.find(Key) != Strings.end();
}

std::vector<std::string> FLocalization::GetKeys()
{
	EnsureInitialized();
	std::vector<std::string> Keys;
	Keys.reserve(Strings.size());
	for (const auto& [Key, Values] : Strings)
	{
		Keys.push_back(Key);
	}
	std::sort(Keys.begin(), Keys.end());
	return Keys;
}

std::string FLocalization::Format(std::string_view Pattern, const FLocFormatArgs& Args)
{
	std::string Result;
	Result.reserve(Pattern.size());
	for (size_t Index = 0; Index < Pattern.size(); ++Index)
	{
		const char Char = Pattern[Index];
		if (Char == '{' && Index + 1 < Pattern.size() && Pattern[Index + 1] == '{')
		{
			Result.push_back('{');
			++Index;
			continue;
		}
		if (Char == '}' && Index + 1 < Pattern.size() && Pattern[Index + 1] == '}')
		{
			Result.push_back('}');
			++Index;
			continue;
		}
		if (Char == '{')
		{
			const size_t Close = Pattern.find('}', Index + 1);
			if (Close != std::string_view::npos)
			{
				const std::string_view Name     = Pattern.substr(Index + 1, Close - Index - 1);
				const std::string*     Argument = nullptr;
				if (IsDigits(Name))
				{
					const size_t Position = static_cast<size_t>(std::stoul(std::string(Name)));
					if (Position < Args.Positional.size())
					{
						Argument = &Args.Positional[Position];
					}
				}
				else
				{
					for (const auto& [ArgName, Value] : Args.Named)
					{
						if (ArgName == Name)
						{
							Argument = &Value;
							break;
						}
					}
				}
				if (Argument != nullptr)
				{
					Result += *Argument;
					Index = Close;
					continue;
				}
			}
		}
		Result.push_back(Char);
	}
	return Result;
}

std::string FLocalization::NormalizeLanguage(std::string_view InLanguage)
{
	std::string Code = Trim(InLanguage);
	for (char& Char : Code)
	{
		Char = Char == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
	}
	return Code;
}

std::string FLocalization::MatchLanguage(std::string_view Wanted, const std::vector<std::string>& Available)
{
	const std::string Code = NormalizeLanguage(Wanted);
	if (Code.empty())
	{
		return {};
	}
	if (std::find(Available.begin(), Available.end(), Code) != Available.end())
	{
		return Code;
	}
	// "ko-kr" → "ko", 또는 "zh" → "zh-cn" (앞부분이 같은 첫 언어)
	const std::string Base = Code.substr(0, Code.find('-'));
	for (const std::string& Candidate : Available)
	{
		if (Candidate == Base)
		{
			return Candidate;
		}
	}
	for (const std::string& Candidate : Available)
	{
		if (Candidate.substr(0, Candidate.find('-')) == Base)
		{
			return Candidate;
		}
	}
	return {};
}

std::string FLocalization::GetLanguageDisplayName(std::string_view InLanguage)
{
	struct FName
	{
		const char* Code;
		const char* Name;
	};
	static constexpr FName Names[] = {
		{ "ko", "한국어" },  { "en", "English" }, { "ja", "日本語" },    { "zh-cn", "简体中文" }, { "zh-tw", "繁體中文" }, { "zh", "中文" },
		{ "de", "Deutsch" }, { "fr", "Français" }, { "es", "Español" }, { "ru", "Русский" },  { "pt-br", "Português (Brasil)" },
	};
	const std::string Code = NormalizeLanguage(InLanguage);
	for (const FName& Entry : Names)
	{
		if (Code == Entry.Code)
		{
			return Entry.Name;
		}
	}
	return Code;
}

std::filesystem::path FLocalization::GetUserSettingsPath()
{
	return FPaths::GetSavedDirectory() / L"Config" / L"Language.json";
}
