#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// 파일/폴더 이동 한 건 (절대 경로, 이동 전 → 후)
struct FAssetMove
{
	std::filesystem::path From;
	std::filesystem::path To;
};

// 에셋 이동/이름 변경 후 다른 에셋 파일 안의 경로 참조를 새 경로로 고친다 (파일 시스템 + JSON 문자열, GPU 비의존).
//   - .escene / .eproject: 문자열 값은 Content 기준 상대 경로 (절대 경로면 절대 경로로 유지)
//   - .emat / .eparticle : 문자열 값은 그 파일이 있는 폴더 기준 상대 경로 → 파일 자신이 옮겨져도 다시 계산한다
//   확장자가 있는 문자열만 경로 후보로 보고(엔티티 이름 등 오인 방지), 이동한 파일이거나 이동한 폴더 안이면 바꾼다.
//   파일은 JSON을 다시 쓰지 않고 해당 문자열 토큰만 바꿔 원래 서식(줄바꿈/키 순서)을 유지한다.
struct FAssetReferenceUpdater
{
	struct FResult
	{
		std::vector<std::filesystem::path> ChangedFiles;
		uint32                             ReplacedCount = 0;
	};

	// 디스크 이동을 마친 뒤 호출. ProjectFile(.eproject)이 비어 있으면 건너뛴다
	static FResult UpdateAfterMove(const std::filesystem::path& ContentDirectory, const std::filesystem::path& ProjectFile,
	                               const std::vector<FAssetMove>& Moves);

	// Targets(파일 또는 폴더)를 참조하는 에셋 파일 목록 (삭제 확인용)
	static std::vector<std::filesystem::path> FindReferencingFiles(const std::filesystem::path& ContentDirectory,
	                                                               const std::filesystem::path& ProjectFile,
	                                                               const std::vector<std::filesystem::path>& Targets);

	// Content 기준 경로 문자열 하나를 이동 목록으로 다시 계산 (열린 씬의 컴포넌트 문자열 갱신용). 바뀌지 않으면 nullopt
	static std::optional<std::string> RemapContentPath(const std::string& Value, const std::filesystem::path& ContentDirectory,
	                                                   const std::vector<FAssetMove>& Moves);

	// 씬 JSON 문자열(Content 기준 경로) 안의 참조를 제자리에서 고친다 (실행 취소 기록의 스냅샷 갱신용). 반환: 바꾼 개수
	static uint32 RemapSceneJson(std::string& Json, const std::filesystem::path& ContentDirectory, const std::vector<FAssetMove>& Moves);

	// Path가 이동 대상(파일 자체 또는 폴더 안)이면 이동 후 경로
	static std::optional<std::filesystem::path> MapPath(const std::filesystem::path& Path, const std::vector<FAssetMove>& Moves);
};
