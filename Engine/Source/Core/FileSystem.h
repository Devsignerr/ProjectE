#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// 런타임 파일 읽기 창구: 마운트한 pak(.epak) → 디스크 순서로 찾는다.
// 에셋/씬/스크립트/글꼴/오디오처럼 "콘텐츠를 읽는" 코드는 std::ifstream 대신 이것을 쓴다 (패키지는 콘텐츠를 pak에 묶는다).
// 쓰기(저장, 쿠킹, 로그)와 편집기 전용 디렉터리 탐색은 디스크를 직접 쓴다.
//
// pak 항목 키 = 마운트 루트 기준 상대 경로 (소문자, '/' 구분). 절대 경로로 묻든 상대 경로로 묻든 같은 파일을 찾는다.
// 스레드 안전: 마운트는 시작 때(다른 스레드가 읽기 전)만, 읽기는 여러 스레드에서 해도 된다.
class FFileSystem
{
public:
	// PakFile을 MountRoot 아래 파일들로 마운트. 형식이 틀리면 false (로그)
	static bool Mount(const std::filesystem::path& PakFile, const std::filesystem::path& MountRoot);
	static void UnmountAll();
	static size_t GetMountedFileCount();

	static bool Exists(const std::filesystem::path& Path); // 파일 (pak 항목 또는 디스크 일반 파일)
	static bool IsInPak(const std::filesystem::path& Path);
	static bool ReadFile(const std::filesystem::path& Path, std::vector<uint8>& OutBytes);
	static bool ReadTextFile(const std::filesystem::path& Path, std::string& OutText);
	// pak 항목은 pak 파일의 수정 시각 (항목이 바뀔 일이 없으므로 캐시 무효화 비교에 안전). 없으면 nullopt
	static std::optional<std::filesystem::file_time_type> GetLastWriteTime(const std::filesystem::path& Path);

	// pak 키 규칙 (쓰기 도구와 공유): MountRoot 기준 상대 경로, 소문자, '/'. 루트 밖이면 빈 문자열
	static std::string MakeKey(const std::filesystem::path& Path, const std::filesystem::path& MountRoot);
};

// .epak 쓰기 (쿠킹 도구). Files = (키, 디스크 원본). 키는 FFileSystem::MakeKey로 만든다.
// 형식: "EPAK" u32 버전, u32 항목 수, u32 예약, u64 색인 오프셋 | 데이터... | 색인 [u16 키 길이, 키, u64 오프셋, u64 크기, u64 FNV-1a]
class FPakWriter
{
public:
	static constexpr uint32 Magic   = 0x4B415045; // "EPAK"
	static constexpr uint32 Version = 1;

	static bool Write(const std::filesystem::path& PakFile, const std::vector<std::pair<std::string, std::filesystem::path>>& Files,
	                  std::string& OutError);
};
