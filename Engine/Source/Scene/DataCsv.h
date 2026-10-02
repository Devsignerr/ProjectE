#pragma once

#include "Scene/DataTable.h"

#include <string>
#include <string_view>
#include <vector>

// 데이터 테이블 CSV 가져오기/내보내기 (순수 함수 — 파일 입출력은 호출자, 편집기 46-B가 UI를 연결한다).
// 규칙:
//   - 첫 줄 = 머리글: 첫 칸 "Name"(행 이름 열 — 머리글 글자는 무시), 나머지 = 필드 이름(정확히 같은 이름 → 없으면 대소문자 무시로 찾음)
//   - 둘째 줄부터 행 하나씩, 첫 칸 = 행 이름. 빈 줄은 건너뛴다. RFC 4180 따옴표("a,b", "say ""hi""", 칸 안 줄바꿈) 지원
//   - 구분자는 쉼표, 머리글에 쉼표가 없고 탭이 있으면 탭(TSV). UTF-8 BOM은 읽을 때 무시, 내보낼 때 기본으로 붙인다(엑셀 한글)
//   - 칸 글자 ↔ 값은 DataValueText: Bool true/false(읽기 1/0/yes/no도), Int/Float 10진, Vector/Color 성분 "x;y;z"(읽기 쉼표·공백·괄호도,
//     Color는 "#RRGGBB[AA]"도), 문자열 5종 그대로, Enum은 이름, 배열은 요소를 '|'로 이음(요소 안 '|'와 '\'는 '\'로 이스케이프)
//   - 빈 칸 = 문자열 타입은 "", Enum/숫자/벡터는 필드 기본값, 배열은 빈 배열
//   - 가져오기 문제(모르는 열, 없는 열, 읽을 수 없는 칸, 중복/빈 행 이름)는 경고 후 계속: 모르는 열 무시, 없는 열/잘못된 칸 = 기본값,
//     중복 행 이름 = "이름_2"로 바꿈, 빈 행 이름 = "Row" 계열 이름. 머리글이 없거나 구조체가 없으면 실패
namespace DataCsv
{
	// 표 글자 → 칸 (줄 단위). Delimiter 0 = 자동 (쉼표, 머리글에 쉼표 없이 탭만 있으면 탭)
	std::vector<std::vector<std::string>> Parse(std::string_view Text, char Delimiter = 0);
	std::string                           Write(const std::vector<std::vector<std::string>>& Cells, char Delimiter = ',', bool bWriteBom = true);

	std::string Export(const FDataTable& Table, bool bWriteBom = true);
	// Table.Struct 기준으로 행 목록을 만든다 (Table은 바꾸지 않음 — 적용은 Table.SetRows(OutRows) 또는 ImportInto)
	bool        Import(std::string_view Csv, const FDataTable& Table, std::vector<FDataRow>& OutRows, FDataLoadReport* Report = nullptr,
	                   std::string* OutError = nullptr);
	bool        ImportInto(std::string_view Csv, FDataTable& Table, FDataLoadReport* Report = nullptr, std::string* OutError = nullptr); // 전체 교체
} // namespace DataCsv
