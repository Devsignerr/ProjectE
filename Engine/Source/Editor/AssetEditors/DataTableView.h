#pragma once

#include "Scene/DataTable.h"

#include <string>
#include <string_view>
#include <vector>

// 데이터 테이블 편집기의 순수 로직 (ImGui 없음 — EditorTests가 검증한다).
// 보기(검색/정렬)는 행 번호 목록만 만들고 파일 순서를 바꾸지 않는다. 순서를 바꾸는 것은 "정렬 적용"(SortedRows)뿐이다.
namespace DataTableView
{
	constexpr int32 NoSort      = -1; // 파일 순서
	constexpr int32 NameColumn  = 0;  // 행 이름
	// 필드 i의 열 번호 = 1 + i

	// 대소문자 무시(ASCII) 부분 문자열
	bool ContainsInsensitive(std::string_view Text, std::string_view Needle);

	// 검색: 행 이름 또는 어떤 칸의 글자(DataValueText::ToText)에 Filter가 들어 있으면 true (빈 Filter = 모두)
	bool RowMatches(const FDataTable& Table, const FDataRow& Row, std::string_view Filter);

	// 두 값 비교 (<0 / 0 / >0): Bool/Int/Float = 수, 벡터/색 = 성분 순서, Enum = 목록 순서, 문자열 계열 = 대소문자 무시 후 그대로,
	// 배열 = 요소 수 → 글자
	int32 CompareValues(const FDataField& Field, const FDataValue& A, const FDataValue& B);

	// 보이는 행 번호 목록: 검색 통과 → SortColumn 기준 안정 정렬 (같은 값은 파일 순서)
	std::vector<int32> BuildView(const FDataTable& Table, std::string_view Filter, int32 SortColumn, bool bDescending);

	// "정렬 적용": 모든 행(검색 무시)을 SortColumn 순서로 늘어놓은 목록 (Table.SetRows에 넘긴다)
	std::vector<FDataRow> SortedRows(const FDataTable& Table, int32 SortColumn, bool bDescending);

	// CSV 병합: 같은 이름 행은 가져온 값으로 바꾸고(위치·Unknown 원문 유지), 새 이름은 가져온 순서대로 끝에 덧붙인다
	std::vector<FDataRow> MergeRows(const std::vector<FDataRow>& Existing, const std::vector<FDataRow>& Imported);

	// 배열 칸 요약 "[3] a, b, c" (MaxChars 바이트를 넘으면 "…"로 줄임 — UTF-8 글자 경계 유지)
	std::string SummarizeArray(const FDataField& Field, const FDataValue& Value, size_t MaxChars = 48);

	// 행 이름 변경 뒤 이 테이블 안의 RowRef(배열 요소 포함) 중 From을 가리키던 값을 To로 바꾼다. Fields = 자기 테이블을 가리키는 필드 칸.
	// 반환 = 바꾼 값 수
	int32 RenameRowReferences(FDataTable& Table, const std::vector<int32>& Fields, const std::string& From, const std::string& To);

	// 열 기본 폭 (픽셀, 글꼴 크기 배율 전) — 필드 타입별
	float GetDefaultColumnWidth(const FDataField& Field);
} // namespace DataTableView
