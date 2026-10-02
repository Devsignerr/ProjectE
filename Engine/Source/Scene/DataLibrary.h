#pragma once

#include "Scene/DataTable.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// 참조 문제 하나 (편집기 칸 표시) — Field = 구조체 필드 칸
struct FDataReferenceIssue
{
	int32       Field = -1;
	std::string Message; // "필드 'X': 없는 행 'Y' (...)"
};

// .estruct/.etable/.edata 공유 캐시 (엔진 DLL 전역 하나, 메인 스레드 전용). 경로는 Content 기준(FPrefabLibrary의 Content 폴더) 또는 절대.
//   - 읽기는 FFileSystem(pak → 디스크). 경로마다 한 번 읽고, 실패(파일 없음/파싱 실패)도 nullptr로 캐시해 경고를 한 번만 낸다
//   - 읽을 때 형식 경고(FDataLoadReport)와 참조 경고(ValidateReferences: 구조체 없음, 없는 행을 가리키는 RowRef, RowRef 필드의 Table 없음,
//     없는 에셋 파일/필터에 맞지 않는 확장자)를 "[데이터] <경로>: ..." 로그로 한 번 남긴다. 어떤 경우에도 크래시하지 않는다
//   - 구조체 없이 읽힌 테이블(Struct == nullptr)도 반환한다(편집기가 고칠 수 있게). 값 조회는 모두 실패로 처리된다
//   - Invalidate(경로): 그 항목만 지우고 세대를 올린다. 구조체(.estruct)면 그 구조체를 쓰는 테이블/에셋이 바뀌므로 테이블·에셋 캐시 전체를 비운다
//     세대(GetGeneration)가 바뀌면 shared_ptr을 들고 있는 쪽은 다시 Load한다(이미 받은 객체는 바뀌지 않는다 — 불변)
//   - 핫 리로드: 에디터 파일 감시(FEditorApplication::PollScriptChanges)가 바뀐 파일을 Invalidate, 콘텐츠 브라우저 이동은 전체 Invalidate
class FDataLibrary
{
public:
	static FDataLibrary& Get();

	std::shared_ptr<const FDataStruct> LoadStruct(const std::string& AssetPath);
	std::shared_ptr<const FDataTable>  LoadTable(const std::string& AssetPath);
	std::shared_ptr<const FDataAsset>  LoadDataAsset(const std::string& AssetPath);

	void   Invalidate();
	void   Invalidate(const std::string& AssetPath);
	uint32 GetGeneration() const { return Generation; }

	// 확장자 (소문자, "." 포함)가 데이터 파일인가
	static bool IsDataExtension(const std::wstring& LowerExtension);

	// 참조 검증 (Load가 자동으로 부르고 로그한다 — 편집기 표시용으로 따로 불러도 된다). 반환 = 경고 목록
	std::vector<std::string> ValidateReferences(const FDataTable& Table, const std::string& SelfPath = {});
	std::vector<std::string> ValidateReferences(const FDataAsset& Asset);
	// 편집기 칸 표시용: 레코드 하나(행/에셋 값)의 참조 문제를 필드 칸별로 (상한 없음, 구조체 수준 경고 제외).
	// Self/SelfPath = 편집 중인 테이블 (자기 테이블을 가리키는 RowRef는 디스크 대신 이것으로 확인)
	std::vector<FDataReferenceIssue> ValidateRecordReferences(const FDataStruct& Struct, const FDataRecord& Record, const std::string& SelfPath = {},
	                                                          const FDataTable* Self = nullptr);

	// ---- 저장 (편집기): 파일 쓰기 + 해당 경로 Invalidate
	bool SaveStruct(const std::string& AssetPath, const FDataStruct& Struct, std::string* OutError = nullptr);
	bool SaveTable(const std::string& AssetPath, const FDataTable& Table, std::string* OutError = nullptr);
	bool SaveDataAsset(const std::string& AssetPath, const FDataAsset& Asset, std::string* OutError = nullptr);

	// 구조체 저장 + 마이그레이션: ContentDirectory 아래 이 구조체를 쓰는 .etable/.edata를 (저장 전) 디스크의 옛 구조체로 읽어
	// 새 구조체로 Rebind(Renames 적용)한 뒤 다시 저장한다. 마지막에 전체 Invalidate. OutChangedFiles = 다시 쓴 파일(구조체 포함)
	bool SaveStructAndMigrate(const std::string& StructPath, const FDataStruct& NewStruct, const std::vector<FDataFieldRename>& Renames,
	                          const std::filesystem::path& ContentDirectory, std::vector<std::filesystem::path>* OutChangedFiles = nullptr,
	                          std::string* OutError = nullptr);
	// ContentDirectory 아래 이 구조체(Content 기준 또는 절대 경로)를 쓰는 .etable/.edata 목록 (디스크 탐색 — 편집기 전용)
	static std::vector<std::filesystem::path> FindStructUsers(const std::filesystem::path& ContentDirectory, const std::string& StructPath);

	std::filesystem::path ResolvePath(const std::string& AssetPath) const; // Content 기준 → 절대

private:
	static std::wstring MakeKey(const std::string& AssetPath);
	void                Log(const std::string& AssetPath, const std::vector<std::string>& Warnings) const;
	std::string         MakeDisplayPath(const std::string& AssetPath) const;

	std::unordered_map<std::wstring, std::shared_ptr<const FDataStruct>> Structs;
	std::unordered_map<std::wstring, std::shared_ptr<const FDataTable>>  Tables;
	std::unordered_map<std::wstring, std::shared_ptr<const FDataAsset>>  Assets;
	uint32                                                               Generation = 1;
};
