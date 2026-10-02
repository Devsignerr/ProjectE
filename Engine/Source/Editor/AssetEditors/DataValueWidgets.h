#pragma once

#include "Scene/DataTable.h"

#include <filesystem>
#include <string>
#include <vector>

struct FEditorContext;

// 데이터 값 편집 위젯 (데이터 테이블 칸 / 데이터 에셋 인스펙터 / 구조체 기본값 공용)
struct FDataWidgetContext
{
	FEditorContext*       Editor    = nullptr; // Content 폴더, 알림, 편집 창 열기 (없을 수 있음)
	std::filesystem::path SelfPath;            // 편집 중인 테이블 파일 — RowRef가 이 파일을 가리키면 SelfTable의 행 목록을 쓴다
	const FDataTable*     SelfTable = nullptr;
	const std::string*    Problem   = nullptr; // 이 칸의 참조 경고 (경고 색 + 툴팁)
	bool                  bCompact  = false;   // 표 칸: 한 줄, 긴 편집(배열/여러 줄 글자)은 팝업
	bool                  bOpenPopup = false;  // 자동 검증: 이 칸의 배열/글자 팝업을 연다
};

namespace DataValueWidgets
{
	// 값 하나 편집 (현재 항목 폭 사용, ID는 호출자가 PushID). 반환: 바뀜 (Value는 Field.Accepts를 만족)
	bool Draw(const FDataField& Field, FDataValue& Value, const FDataWidgetContext& Context);

	// std::string 직접 편집 (버퍼 크기 자동). 반환: 바뀜
	bool InputString(const char* Id, std::string& Text, int Flags = 0);
	// 이름처럼 끝났을 때만 적용하는 글자 칸: 편집이 끝나면(Enter/포커스 잃음) OutText에 새 글자 + true
	bool InputTextCommit(const char* Id, const std::string& Current, std::string& OutText, const char* Hint = nullptr);

	// Content 아래 확장자(소문자, "." 포함)가 같은 파일의 Content 기준 경로 목록 (이름순, 2초 캐시)
	const std::vector<std::string>& ScanContentFiles(const std::filesystem::path& ContentDirectory, const std::wstring& Extension);
	// RowRef 대상 테이블의 행 이름 (SelfPath와 같은 파일이면 SelfTable)
	std::vector<std::string> GetTargetRowNames(const FDataField& Field, const FDataWidgetContext& Context);
	// 경로 문자열 두 개가 같은 파일인가 (Content 기준/절대 섞여도)
	bool IsSameDataFile(const std::string& AssetPath, const std::filesystem::path& File);

	// 필드 이름 + 타입/설명 툴팁 (구조체 설명 표시 공용)
	std::string DescribeField(const FDataField& Field);
} // namespace DataValueWidgets
