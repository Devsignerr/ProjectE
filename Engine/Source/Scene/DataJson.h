#pragma once

// 데이터 테이블 JSON 공용 도우미 — Scene 모듈 .cpp 내부 전용 (nlohmann json 노출). 다른 모듈은 Scene/DataTable.h만 쓴다
#include "Scene/DataTable.h"

#include <json.hpp>

#include <string>

namespace DataJson
{
	using FJson = nlohmann::ordered_json; // 키 순서 유지 (필드 순서대로 저장)

	FJson       Parse(const std::string& Text); // 실패하면 discarded (주석 허용)
	std::string DumpCompact(const FJson& Value); // 한 줄, 잘못된 UTF-8은 대체 문자
	std::string GetString(const FJson& Object, const char* Key); // 없거나 문자열이 아니면 "" (value()와 달리 예외 없음)
	// 최상위 객체를 한 줄에 한 키로, 객체 배열/객체 값은 한 줄에 한 항목으로 쓴다 (행 하나 = 한 줄 → diff가 읽기 쉽다)
	std::string FormatDocument(const FJson& Root);
	std::string FormatFloat(float Value); // 가장 짧은 왕복 표현 ("0.1", "10")
	FJson       FloatToJson(float Value);

	FJson ValueToJson(const FDataField& Field, const FDataValue& Value);
	// true = Out 설정됨 (Problem이 있으면 일부 요소만 기본값으로 바꾼 경고), false = 쓸 수 없는 값(Problem에 이유)
	bool  ValueFromJson(const FDataField& Field, const FJson& Json, FDataValue& Out, std::string& Problem);
} // namespace DataJson
