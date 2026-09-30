#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Vector3.h"
#include "Core/Reflection/PropertyType.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// 비헤이비어 트리 공용 타입 (에셋/레지스트리/실행기/노드가 함께 쓴다)

// 노드 실행 결과
enum class EBTStatus : uint8
{
	Running,
	Success,
	Failure,
};

// 노드 분류. 컴포지트만 자식을 가지고, 데코레이터/서비스는 컴포지트·태스크 노드에 붙는다
enum class EBTNodeCategory : uint8
{
	Composite,
	Task,
	Decorator,
	Service,
};

// 데코레이터 중단 모드 (UE와 같은 의미)
//   Self          : 붙은 노드가 실행 중인데 조건이 거짓이 되면 그 가지를 중단한다
//   LowerPriority : 부모 컴포지트에서 이 노드보다 뒤(낮은 우선순위) 자식이 실행 중인데 조건이 참이 되면 그 자식을 중단하고 이 노드를 실행한다
//   Both          : 둘 다
enum class EBTAbortMode : uint8
{
	None,
	Self,
	LowerPriority,
	Both,
};

// 블랙보드 키 타입. 순서는 FBlackboardValue의 variant 순서와 같다
enum class EBlackboardKeyType : uint8
{
	Bool,
	Int,
	Float,
	Vector,
	Entity,
	String,
};

// 블랙보드 값. variant 인덱스 == static_cast<size_t>(EBlackboardKeyType)
using FBlackboardValue = std::variant<bool, int32, float, FVector3, FEntity, std::string>;

// 노드 파라미터 값: EPropertyType의 Bool/Int32/Float/String/Vector3만 지원 (블랙보드 키 참조는 String)
using FBTParamValue = std::variant<bool, int32, float, std::string, FVector3>;

struct FBTParam
{
	std::string   Name;
	FBTParamValue Value;
};

// 노드 인스턴스에 전달되는 파라미터 모음 (에셋 값 + 레지스트리 기본값을 합친 결과)
struct FBTNodeParams
{
	std::vector<FBTParam> Values;

	const FBTParamValue* Find(std::string_view Name) const;

	// 없거나 타입이 다르면 Default. Int32↔Float는 서로 변환해 준다
	bool        GetBool(std::string_view Name, bool Default = false) const;
	int32       GetInt(std::string_view Name, int32 Default = 0) const;
	float       GetFloat(std::string_view Name, float Default = 0.0f) const;
	std::string GetString(std::string_view Name, std::string_view Default = {}) const;
	FVector3    GetVector(std::string_view Name, const FVector3& Default = FVector3::ZeroVector) const;
};

namespace BehaviorTreeTypes
{
	const char* ToString(EBTStatus Status);
	const char* ToString(EBTNodeCategory Category);
	const char* ToString(EBTAbortMode Mode);
	const char* ToString(EBlackboardKeyType Type);

	std::optional<EBTNodeCategory>    ParseNodeCategory(std::string_view Text);
	std::optional<EBTAbortMode>       ParseAbortMode(std::string_view Text);
	std::optional<EBlackboardKeyType> ParseBlackboardKeyType(std::string_view Text);

	// 파라미터 값의 리플렉션 타입 (Bool/Int32/Float/String/Vector3)
	EPropertyType GetParamType(const FBTParamValue& Value);
	// 지원하는 타입이면 기본값(0/false/빈 문자열)을 돌려준다
	std::optional<FBTParamValue> MakeDefaultParam(EPropertyType Type);
	// 값을 지정 타입으로 변환 (같은 타입, Int32↔Float만 허용)
	std::optional<FBTParamValue> CoerceParam(const FBTParamValue& Value, EPropertyType Type);

	// 값이 키 타입과 일치하는지
	bool MatchesKeyType(const FBlackboardValue& Value, EBlackboardKeyType Type);
	// 문자열을 키 타입 값으로 해석 (노드 파라미터의 비교값/설정값용)
	//   Bool: true/false/1/0, Int/Float: 숫자, Vector: "x,y,z" (공백 허용), String: 그대로, Entity: 지원 안 함(false)
	bool ParseBlackboardValue(std::string_view Text, EBlackboardKeyType Type, FBlackboardValue& OutValue);
} // namespace BehaviorTreeTypes
