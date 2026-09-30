#pragma once

#include "AI/BehaviorTree/BehaviorTreeTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 블랙보드 키 정의 (에셋에 저장된다)
struct FBlackboardKeyDesc
{
	std::string        Name;
	EBlackboardKeyType Type = EBlackboardKeyType::Bool;
};

// 비헤이비어 트리 인스턴스의 키-값 저장소.
// 키마다 값이 "설정 안 됨"일 수 있다(IsSet). Set/Get은 키 타입을 검사해 다르면 거부한다.
// 관찰자는 값이 실제로 바뀔 때만(다른 값으로 설정, 설정 안 됨 → 설정, 설정 → Clear) 호출된다.
class FBlackboard
{
public:
	using FObserverHandle = uint32;
	// 인자: 바뀐 키 이름
	using FObserver = std::function<void(std::string_view Key)>;

	static constexpr int32 InvalidKey = -1;

	// 키 정의를 바꾸고 모든 값을 비운다 (관찰자는 모두 제거)
	void SetKeys(const std::vector<FBlackboardKeyDesc>& Keys);
	const std::vector<FBlackboardKeyDesc>& GetKeys() const { return KeyDescs; }

	int32                             FindKey(std::string_view Name) const;
	std::optional<EBlackboardKeyType> GetKeyType(std::string_view Name) const;

	bool IsSet(std::string_view Name) const;
	// 값 비우기. 키가 없으면 false
	bool Clear(std::string_view Name);

	// 타입이 키와 다르거나 키가 없으면 false (값은 그대로, 경고 로그)
	bool SetValue(std::string_view Name, const FBlackboardValue& Value);
	// 설정 안 됐거나 키가 없으면 nullptr
	const FBlackboardValue* GetValue(std::string_view Name) const;

	bool SetBool(std::string_view Name, bool Value) { return SetValue(Name, FBlackboardValue(Value)); }
	bool SetInt(std::string_view Name, int32 Value) { return SetValue(Name, FBlackboardValue(Value)); }
	bool SetFloat(std::string_view Name, float Value) { return SetValue(Name, FBlackboardValue(Value)); }
	bool SetVector(std::string_view Name, const FVector3& Value) { return SetValue(Name, FBlackboardValue(Value)); }
	bool SetEntity(std::string_view Name, FEntity Value) { return SetValue(Name, FBlackboardValue(Value)); }
	bool SetString(std::string_view Name, std::string Value) { return SetValue(Name, FBlackboardValue(std::move(Value))); }

	// T는 bool/int32/float/FVector3/FEntity/std::string. 키가 없거나 타입이 다르거나 설정 안 됐으면 nullopt
	template <typename T>
	std::optional<T> Get(std::string_view Name) const
	{
		const FBlackboardValue* Value = GetValue(Name);
		if (Value && std::holds_alternative<T>(*Value))
		{
			return std::get<T>(*Value);
		}
		return std::nullopt;
	}

	// 키별 값 변경 관찰. 키가 없으면 0(무효 핸들)
	FObserverHandle AddObserver(std::string_view Key, FObserver Observer);
	void            RemoveObserver(FObserverHandle Handle);

private:
	void NotifyChanged(int32 KeyIndex);

	struct FObserverEntry
	{
		FObserverHandle Handle   = 0;
		int32           KeyIndex = InvalidKey;
		FObserver       Callback;
	};

	std::vector<FBlackboardKeyDesc>              KeyDescs;
	std::vector<std::optional<FBlackboardValue>> Values;
	std::vector<FObserverEntry>                  Observers;
	FObserverHandle                              NextObserverHandle = 1;
};
