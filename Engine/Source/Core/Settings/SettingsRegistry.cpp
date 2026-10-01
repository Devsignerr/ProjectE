#include "Core/Settings/SettingsRegistry.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/Reflection/ReflectionJson.h"
#include "Core/StringConv.h"

#include <fstream>

std::filesystem::path FSettingsSection::GetFilePath() const
{
	const std::wstring FileName = FStringConv::ToWide(Id) + L".json";
	if (Scope == ESettingsScope::EditorUser)
	{
		return FSettingsRegistry::GetEditorUserDirectory() / FileName;
	}
	if (Scope == ESettingsScope::ProjectUser)
	{
		return FPaths::GetSavedDirectory() / L"Config" / FileName;
	}
	return FPaths::HasProject() ? FPaths::GetProjectConfigDirectory() / FileName : std::filesystem::path();
}

bool FSettingsSection::Load() const
{
	const std::filesystem::path Path = GetFilePath();
	return Path.empty() || LoadFrom(Path);
}

bool FSettingsSection::Save() const
{
	const std::filesystem::path Path = GetFilePath();
	return !Path.empty() && SaveTo(Path);
}

bool FSettingsSection::LoadFrom(const std::filesystem::path& Path) const
{
	if (!FFileSystem::Exists(Path))
	{
		return true;
	}
	std::string Text;
	std::string Error;
	if (!FFileSystem::ReadTextFile(Path, Text) || !FReflectionJson::Apply(*Type, Object, Text, &Error))
	{
		E_LOG(LogCore, Warning, "설정 파일을 읽지 못해 기본값을 씁니다: {} ({})", FStringConv::ToUtf8(Path.wstring()), Error);
		return false;
	}
	return true;
}

bool FSettingsSection::SaveTo(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogCore, Error, "설정을 저장할 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << FReflectionJson::Write(*Type, Object);
	return static_cast<bool>(File);
}

FSettingsRegistry& FSettingsRegistry::Get()
{
	static FSettingsRegistry Registry;
	return Registry;
}

FSettingsSection& FSettingsRegistry::AddSection(FDesc Desc)
{
	auto Section              = std::make_unique<FSettingsSection>();
	Section->Id               = std::move(Desc.Id);
	Section->DisplayName      = std::move(Desc.DisplayName);
	Section->Category         = std::move(Desc.Category);
	Section->Description      = std::move(Desc.Description);
	Section->Scope            = Desc.Scope;
	Section->bRequiresRestart = Desc.bRequiresRestart;
	Section->bHidden          = Desc.bHidden;
	Section->Type             = std::make_unique<FTypeInfo>();
	Section->Type->Name        = Section->Id + "Settings";
	Section->Type->DisplayName = Section->DisplayName;

	for (std::unique_ptr<FSettingsSection>& Existing : Sections)
	{
		if (Existing->Id == Section->Id)
		{
			Existing = std::move(Section);
			return *Existing;
		}
	}
	Sections.push_back(std::move(Section));
	return *Sections.back();
}

FSettingsSection* FSettingsRegistry::Find(std::string_view Id)
{
	for (const std::unique_ptr<FSettingsSection>& Section : Sections)
	{
		if (Section->Id == Id)
		{
			return Section.get();
		}
	}
	return nullptr;
}

void FSettingsRegistry::LoadAll(ESettingsScope Scope)
{
	for (const std::unique_ptr<FSettingsSection>& Section : Sections)
	{
		if (Section->Scope == Scope)
		{
			Section->Load();
		}
	}
}

std::filesystem::path FSettingsRegistry::GetEditorUserDirectory()
{
	wchar_t     Buffer[MAX_PATH * 4];
	const DWORD Length = GetEnvironmentVariableW(L"LOCALAPPDATA", Buffer, static_cast<DWORD>(std::size(Buffer)));
	if (Length == 0 || Length >= std::size(Buffer))
	{
		return FPaths::GetEngineDirectory() / L"Saved" / L"EditorPreferences";
	}
	return std::filesystem::path(Buffer, Buffer + Length) / L"ProjectE" / L"EditorPreferences";
}
