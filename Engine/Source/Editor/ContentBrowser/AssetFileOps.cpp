#include "Editor/ContentBrowser/AssetFileOps.h"

#include "Core/Platform/WindowsHeaders.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

namespace
{
	std::wstring LowerGeneric(const std::filesystem::path& Path)
	{
		std::wstring Text = Path.lexically_normal().generic_wstring();
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		while (Text.size() > 1 && Text.back() == L'/')
		{
			Text.pop_back();
		}
		return Text;
	}
} // namespace

const char* FAssetFileOps::Describe(EResult Result)
{
	switch (Result)
	{
	case EResult::Ok:             return "완료";
	case EResult::NotFound:       return "원본을 찾을 수 없습니다";
	case EResult::AlreadyExists:  return "같은 이름이 이미 있습니다";
	case EResult::IntoOwnSubtree: return "폴더를 자기 안으로 옮길 수 없습니다";
	case EResult::InvalidName:    return "쓸 수 없는 이름입니다";
	case EResult::SameLocation:   return "이미 그 위치에 있습니다";
	default:                      return "파일 작업에 실패했습니다";
	}
}

bool FAssetFileOps::IsValidName(const std::wstring& Name)
{
	if (Name.empty() || Name == L"." || Name == L".." || Name.back() == L' ' || Name.back() == L'.')
	{
		return false;
	}
	return Name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

bool FAssetFileOps::IsSameOrUnder(const std::filesystem::path& Path, const std::filesystem::path& Root)
{
	const std::wstring PathKey = LowerGeneric(Path);
	const std::wstring RootKey = LowerGeneric(Root);
	return PathKey == RootKey || PathKey.rfind(RootKey + L"/", 0) == 0;
}

std::filesystem::path FAssetFileOps::MakeUniquePath(const std::filesystem::path& Directory, const std::wstring& Stem, const std::wstring& Extension)
{
	std::error_code ErrorCode;
	for (uint32_t Index = 0;; ++Index)
	{
		const std::filesystem::path Candidate = Directory / (Index == 0 ? Stem + Extension : Stem + std::to_wstring(Index) + Extension);
		if (!std::filesystem::exists(Candidate, ErrorCode))
		{
			return Candidate;
		}
	}
}

FAssetFileOps::EResult FAssetFileOps::Move(const std::filesystem::path& Source, const std::filesystem::path& DestinationDirectory,
                                           std::filesystem::path& OutNewPath)
{
	std::error_code ErrorCode;
	if (!std::filesystem::exists(Source, ErrorCode))
	{
		return EResult::NotFound;
	}
	if (LowerGeneric(Source.parent_path()) == LowerGeneric(DestinationDirectory))
	{
		return EResult::SameLocation;
	}
	if (std::filesystem::is_directory(Source, ErrorCode) && IsSameOrUnder(DestinationDirectory, Source))
	{
		return EResult::IntoOwnSubtree;
	}
	const std::filesystem::path Target = DestinationDirectory / Source.filename();
	if (std::filesystem::exists(Target, ErrorCode))
	{
		return EResult::AlreadyExists;
	}
	std::filesystem::rename(Source, Target, ErrorCode);
	if (ErrorCode)
	{
		return EResult::Failed;
	}
	OutNewPath = Target;
	return EResult::Ok;
}

FAssetFileOps::EResult FAssetFileOps::Rename(const std::filesystem::path& Source, const std::wstring& NewName, std::filesystem::path& OutNewPath)
{
	std::error_code ErrorCode;
	if (!std::filesystem::exists(Source, ErrorCode))
	{
		return EResult::NotFound;
	}
	if (!IsValidName(NewName))
	{
		return EResult::InvalidName;
	}
	const std::filesystem::path Target = Source.parent_path() / NewName;
	if (Target.filename() == Source.filename())
	{
		return EResult::SameLocation;
	}
	// 대소문자만 바꾸는 경우는 같은 파일로 보이므로 존재 검사를 건너뛴다
	if (LowerGeneric(Target) != LowerGeneric(Source) && std::filesystem::exists(Target, ErrorCode))
	{
		return EResult::AlreadyExists;
	}
	std::filesystem::rename(Source, Target, ErrorCode);
	if (ErrorCode)
	{
		return EResult::Failed;
	}
	OutNewPath = Target;
	return EResult::Ok;
}

FAssetFileOps::EResult FAssetFileOps::Duplicate(const std::filesystem::path& Source, std::filesystem::path& OutNewPath)
{
	std::error_code ErrorCode;
	if (!std::filesystem::exists(Source, ErrorCode))
	{
		return EResult::NotFound;
	}
	const bool                  bDirectory = std::filesystem::is_directory(Source, ErrorCode);
	const std::filesystem::path Target =
		MakeUniquePath(Source.parent_path(), bDirectory ? Source.filename().wstring() : Source.stem().wstring(), bDirectory ? L"" : Source.extension().wstring());
	std::filesystem::copy(Source, Target, std::filesystem::copy_options::recursive, ErrorCode);
	if (ErrorCode)
	{
		return EResult::Failed;
	}
	OutNewPath = Target;
	return EResult::Ok;
}

FAssetFileOps::EResult FAssetFileOps::CreateFolder(const std::filesystem::path& Parent, const std::wstring& Name, std::filesystem::path& OutNewPath)
{
	if (!IsValidName(Name))
	{
		return EResult::InvalidName;
	}
	const std::filesystem::path Target = MakeUniquePath(Parent, Name, L"");
	std::error_code             ErrorCode;
	if (!std::filesystem::create_directory(Target, ErrorCode) || ErrorCode)
	{
		return EResult::Failed;
	}
	OutNewPath = Target;
	return EResult::Ok;
}

FAssetFileOps::EResult FAssetFileOps::Import(const std::filesystem::path& External, const std::filesystem::path& DestinationDirectory,
                                             std::filesystem::path& OutNewPath)
{
	std::error_code ErrorCode;
	if (!std::filesystem::exists(External, ErrorCode))
	{
		return EResult::NotFound;
	}
	if (IsSameOrUnder(DestinationDirectory, External))
	{
		return EResult::IntoOwnSubtree;
	}
	const bool                  bDirectory = std::filesystem::is_directory(External, ErrorCode);
	const std::filesystem::path Target     = MakeUniquePath(DestinationDirectory, bDirectory ? External.filename().wstring() : External.stem().wstring(),
	                                                        bDirectory ? L"" : External.extension().wstring());
	std::filesystem::copy(External, Target, std::filesystem::copy_options::recursive, ErrorCode);
	if (ErrorCode)
	{
		return EResult::Failed;
	}
	OutNewPath = Target;
	return EResult::Ok;
}

bool FAssetFileOps::MoveToRecycleBin(const std::vector<std::filesystem::path>& Paths)
{
	if (Paths.empty())
	{
		return true;
	}
	// SHFileOperation: 이중 NUL로 끝나는 경로 목록
	std::wstring From;
	for (const std::filesystem::path& Path : Paths)
	{
		From += std::filesystem::absolute(Path).wstring();
		From.push_back(L'\0');
	}
	From.push_back(L'\0');

	SHFILEOPSTRUCTW Operation{};
	Operation.wFunc  = FO_DELETE;
	Operation.pFrom  = From.c_str();
	Operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	return SHFileOperationW(&Operation) == 0 && !Operation.fAnyOperationsAborted;
}
