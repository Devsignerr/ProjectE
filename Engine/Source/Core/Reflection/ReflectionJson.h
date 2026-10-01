#pragma once

#include <string>
#include <string_view>

struct FTypeInfo;

// 리플렉션 객체 ↔ JSON (설정 파일 등 값 타입 구조체용). 엔티티/리소스 핸들 프로퍼티와 PF_Transient는 다루지 않는다
// (씬 컴포넌트는 Scene/EntityJson이 엔티티 참조까지 처리한다).
//   - Int32 enum(EnumEntries)은 이름 문자열로 쓰고, 읽을 때 이름(대소문자 무시) 또는 숫자를 받는다
//   - 읽기: 파일에 없는 키는 현재 값 유지, 모르는 키는 무시, 타입이 틀린 값은 경고 후 건너뜀, 범위(Range)가 있으면 잘라 넣는다
class FReflectionJson
{
public:
	static std::string Write(const FTypeInfo& Type, const void* Object); // 들여쓰기 2, 끝에 줄바꿈
	// JSON 오류(객체가 아님 포함)면 false + OutError, 값은 그대로
	static bool Apply(const FTypeInfo& Type, void* Object, std::string_view Json, std::string* OutError = nullptr);
};
