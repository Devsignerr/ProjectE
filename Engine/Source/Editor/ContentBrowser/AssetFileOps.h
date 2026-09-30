#pragma once

#include <filesystem>
#include <string>
#include <vector>

// 콘텐츠 파일 조작 (파일 시스템만, UI/참조 갱신 없음). 덮어쓰기는 하지 않는다.
struct FAssetFileOps
{
	enum class EResult
	{
		Ok,
		NotFound,
		AlreadyExists,  // 대상 위치에 같은 이름이 있음
		IntoOwnSubtree, // 폴더를 자기 안으로 옮기려 함
		InvalidName,
		SameLocation,
		Failed,
	};
	static const char* Describe(EResult Result);

	// Directory 안에서 겹치지 않는 경로: Stem + Extension, Stem1 + Extension, ... (Extension은 폴더면 빈 문자열)
	static std::filesystem::path MakeUniquePath(const std::filesystem::path& Directory, const std::wstring& Stem, const std::wstring& Extension);

	// 파일/폴더를 DestinationDirectory 안으로 (이름 유지)
	static EResult Move(const std::filesystem::path& Source, const std::filesystem::path& DestinationDirectory, std::filesystem::path& OutNewPath);
	// 같은 폴더 안에서 이름 바꾸기 (NewName은 확장자 포함 파일/폴더 이름)
	static EResult Rename(const std::filesystem::path& Source, const std::wstring& NewName, std::filesystem::path& OutNewPath);
	// 같은 폴더에 복사본 ("이름1.확장자"), 폴더는 통째로
	static EResult Duplicate(const std::filesystem::path& Source, std::filesystem::path& OutNewPath);
	static EResult CreateFolder(const std::filesystem::path& Parent, const std::wstring& Name, std::filesystem::path& OutNewPath);
	// 외부 파일/폴더를 DestinationDirectory로 복사 (이름이 겹치면 번호)
	static EResult Import(const std::filesystem::path& External, const std::filesystem::path& DestinationDirectory, std::filesystem::path& OutNewPath);
	// 휴지통으로 (되살릴 수 있음). 모두 성공하면 true
	static bool MoveToRecycleBin(const std::vector<std::filesystem::path>& Paths);

	// 파일 이름으로 쓸 수 없는 문자(\ / : * ? " < > |)나 빈 이름이면 false
	static bool IsValidName(const std::wstring& Name);
	// Path가 Root 자체이거나 그 아래인지 (대소문자 무시)
	static bool IsSameOrUnder(const std::filesystem::path& Path, const std::filesystem::path& Root);
};
