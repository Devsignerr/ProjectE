#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// 스크립트 Properties 값 (Lua ↔ C++ ↔ JSON). 인스펙터 편집과 씬 저장이 지원하는 타입만 둔다.
enum class EScriptValueType : uint8
{
	Nil,
	Bool,
	Number, // bInteger면 정수 (Lua 5.4 정수 하위 타입)
	String,
	Vector3,
	Asset, // Content 기준 에셋 경로 (String) + 받는 확장자 (AssetFilter, 예 ".eprefab"). Lua: Prefab("...") / Asset("...", ".ext")
};

struct FScriptValue
{
	EScriptValueType Type     = EScriptValueType::Nil;
	bool             bBool    = false;
	bool             bInteger = false;
	double           Number   = 0.0;
	std::string      String;      // String 값, Asset이면 경로
	FVector3         Vector;
	std::string      AssetFilter; // Asset: 인스펙터가 받는 확장자 (";" 구분). 선언 기본값에서 오며 오버라이드 JSON에는 저장하지 않는다

	static FScriptValue MakeBool(bool bValue);
	static FScriptValue MakeNumber(double Value, bool bIsInteger = false);
	static FScriptValue MakeString(std::string Value);
	static FScriptValue MakeVector3(const FVector3& Value);
	static FScriptValue MakeAsset(std::string Path, std::string Filter);

	bool IsNil() const { return Type == EScriptValueType::Nil; }
	bool operator==(const FScriptValue& Other) const;
};

// 스크립트가 Properties 테이블에 선언한 항목 하나 (이름 + 기본값)
struct FScriptPropertyDecl
{
	std::string  Name;
	FScriptValue Default;
};

using FScriptValueMap = std::map<std::string, FScriptValue>; // 이름순 정렬 (인스펙터 표시 순서)

// FScriptComponent::PropertyOverrides(JSON 객체 문자열) 변환. 순수 로직.
struct FScriptProperties
{
	// 빈 문자열이면 빈 맵. 형식 오류는 경고 로그 후 빈 맵 (또는 해당 항목 제외)
	static FScriptValueMap ParseOverrides(std::string_view Json);

	// 빈 맵이면 빈 문자열. Asset 값은 { "Asset": "경로" } 객체로 (일반 문자열과 구분)
	static std::string SerializeOverrides(const FScriptValueMap& Overrides);

	// Value를 선언 기본값의 타입으로 쓸 수 있는가 (정수/실수는 서로 호환)
	static bool IsCompatible(const FScriptValue& Default, const FScriptValue& Value);
};
