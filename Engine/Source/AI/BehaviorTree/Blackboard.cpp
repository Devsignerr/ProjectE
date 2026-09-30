#include "AI/BehaviorTree/Blackboard.h"

#include "AI/AIModule.h"

void FBlackboard::SetKeys(const std::vector<FBlackboardKeyDesc>& Keys)
{
	KeyDescs = Keys;
	Values.assign(KeyDescs.size(), std::nullopt);
	Observers.clear();
}

int32 FBlackboard::FindKey(std::string_view Name) const
{
	for (size_t Index = 0; Index < KeyDescs.size(); ++Index)
	{
		if (KeyDescs[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return InvalidKey;
}

std::optional<EBlackboardKeyType> FBlackboard::GetKeyType(std::string_view Name) const
{
	const int32 Index = FindKey(Name);
	if (Index == InvalidKey)
	{
		return std::nullopt;
	}
	return KeyDescs[Index].Type;
}

bool FBlackboard::IsSet(std::string_view Name) const
{
	const int32 Index = FindKey(Name);
	return Index != InvalidKey && Values[Index].has_value();
}

bool FBlackboard::Clear(std::string_view Name)
{
	const int32 Index = FindKey(Name);
	if (Index == InvalidKey)
	{
		return false;
	}
	if (Values[Index].has_value())
	{
		Values[Index].reset();
		NotifyChanged(Index);
	}
	return true;
}

bool FBlackboard::SetValue(std::string_view Name, const FBlackboardValue& Value)
{
	const int32 Index = FindKey(Name);
	if (Index == InvalidKey)
	{
		E_LOG(LogAI, Warning, "블랙보드에 없는 키입니다: {}", Name);
		return false;
	}
	if (!BehaviorTreeTypes::MatchesKeyType(Value, KeyDescs[Index].Type))
	{
		E_LOG(LogAI, Warning, "블랙보드 키 '{}'의 타입({})과 다른 값은 설정할 수 없습니다", Name, BehaviorTreeTypes::ToString(KeyDescs[Index].Type));
		return false;
	}
	if (Values[Index].has_value() && *Values[Index] == Value)
	{
		return true;
	}
	Values[Index] = Value;
	NotifyChanged(Index);
	return true;
}

const FBlackboardValue* FBlackboard::GetValue(std::string_view Name) const
{
	const int32 Index = FindKey(Name);
	if (Index == InvalidKey || !Values[Index].has_value())
	{
		return nullptr;
	}
	return &*Values[Index];
}

FBlackboard::FObserverHandle FBlackboard::AddObserver(std::string_view Key, FObserver Observer)
{
	const int32 Index = FindKey(Key);
	if (Index == InvalidKey || !Observer)
	{
		return 0;
	}
	const FObserverHandle Handle = NextObserverHandle++;
	Observers.push_back({ Handle, Index, std::move(Observer) });
	return Handle;
}

void FBlackboard::RemoveObserver(FObserverHandle Handle)
{
	std::erase_if(Observers, [Handle](const FObserverEntry& Entry) { return Entry.Handle == Handle; });
}

void FBlackboard::NotifyChanged(int32 KeyIndex)
{
	// 콜백이 관찰자를 추가/제거할 수 있으므로 복사본으로 호출한다
	std::vector<FObserver> Callbacks;
	for (const FObserverEntry& Entry : Observers)
	{
		if (Entry.KeyIndex == KeyIndex)
		{
			Callbacks.push_back(Entry.Callback);
		}
	}
	const std::string_view Name = KeyDescs[KeyIndex].Name;
	for (const FObserver& Callback : Callbacks)
	{
		Callback(Name);
	}
}
