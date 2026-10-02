#include "Scene/Ability/GameplayTags.h"

#include "Core/Settings/ProjectSettings.h"
#include "Scene/Ability/AbilityTypes.h"

#include <algorithm>

namespace
{
	bool IsSeparator(char Char)
	{
		return Char == ';' || Char == ',' || Char == '\n' || Char == '\r';
	}

	std::string_view Trim(std::string_view Text)
	{
		while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t'))
		{
			Text.remove_prefix(1);
		}
		while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
		{
			Text.remove_suffix(1);
		}
		return Text;
	}
} // namespace

bool FGameplayTags::IsValid(std::string_view Tag)
{
	if (Tag.empty() || Tag.front() == '.' || Tag.back() == '.')
	{
		return false;
	}
	for (size_t Index = 0; Index < Tag.size(); ++Index)
	{
		const char Char = Tag[Index];
		if (Char == ' ' || Char == '\t' || IsSeparator(Char) || Char == '=' || (Char == '.' && Tag[Index + 1] == '.'))
		{
			return false;
		}
	}
	return true;
}

bool FGameplayTags::Matches(std::string_view Tag, std::string_view Query)
{
	if (Tag.empty() || Query.empty() || Tag.size() < Query.size() || Tag.compare(0, Query.size(), Query) != 0)
	{
		return false;
	}
	return Tag.size() == Query.size() || Tag[Query.size()] == '.';
}

std::vector<std::string> FGameplayTags::GetSelfAndParents(std::string_view Tag)
{
	std::vector<std::string> Result;
	for (size_t Index = 0; Index < Tag.size(); ++Index)
	{
		if (Tag[Index] == '.')
		{
			Result.emplace_back(Tag.substr(0, Index));
		}
	}
	if (!Tag.empty())
	{
		Result.emplace_back(Tag);
	}
	return Result;
}

std::vector<std::string> FGameplayTags::ParseList(std::string_view Text, std::vector<std::string>* OutInvalid)
{
	std::vector<std::string> Result;
	size_t                   Start = 0;
	for (size_t Index = 0; Index <= Text.size(); ++Index)
	{
		if (Index < Text.size() && !IsSeparator(Text[Index]))
		{
			continue;
		}
		const std::string_view Item = Trim(Text.substr(Start, Index - Start));
		Start                       = Index + 1;
		if (Item.empty())
		{
			continue;
		}
		if (!IsValid(Item))
		{
			if (OutInvalid != nullptr)
			{
				OutInvalid->emplace_back(Item);
			}
			continue;
		}
		if (std::find(Result.begin(), Result.end(), Item) == Result.end())
		{
			Result.emplace_back(Item);
		}
	}
	return Result;
}

std::string FGameplayTags::JoinList(const std::vector<std::string>& Tags)
{
	std::string Result;
	for (const std::string& Tag : Tags)
	{
		if (!Result.empty())
		{
			Result += ';';
		}
		Result += Tag;
	}
	return Result;
}

// ---------------------------------------------------------------- 수 컨테이너

bool FGameplayTagCountContainer::AddTag(std::string_view Tag, int32 Delta)
{
	if (Delta == 0 || !FGameplayTags::IsValid(Tag))
	{
		return false;
	}
	const std::string Key(Tag);
	const auto        Found    = Explicit.find(Key);
	const int32       Previous = Found != Explicit.end() ? Found->second : 0;
	const int32       Next     = std::max(0, Previous + Delta);
	const int32       Applied  = Next - Previous;
	if (Applied == 0)
	{
		return false;
	}
	if (Next == 0)
	{
		Explicit.erase(Found);
	}
	else
	{
		Explicit[Key] = Next;
	}
	for (const std::string& Name : FGameplayTags::GetSelfAndParents(Tag))
	{
		int32& Count = Implicit[Name];
		Count += Applied;
		if (Count <= 0)
		{
			Implicit.erase(Name);
		}
	}
	return (Previous == 0) != (Next == 0);
}

void FGameplayTagCountContainer::Reset()
{
	Explicit.clear();
	Implicit.clear();
}

int32 FGameplayTagCountContainer::GetCount(std::string_view Query) const
{
	const auto Found = Implicit.find(std::string(Query));
	return Found != Implicit.end() ? Found->second : 0;
}

int32 FGameplayTagCountContainer::GetExplicitCount(std::string_view Tag) const
{
	const auto Found = Explicit.find(std::string(Tag));
	return Found != Explicit.end() ? Found->second : 0;
}

bool FGameplayTagCountContainer::HasAny(const std::vector<std::string>& Queries) const
{
	return std::any_of(Queries.begin(), Queries.end(), [this](const std::string& Query) { return HasTag(Query); });
}

bool FGameplayTagCountContainer::HasAll(const std::vector<std::string>& Queries) const
{
	return std::all_of(Queries.begin(), Queries.end(), [this](const std::string& Query) { return HasTag(Query); });
}

// ---------------------------------------------------------------- 등록 목록

FGameplayTagRegistry& FGameplayTagRegistry::Get()
{
	static FGameplayTagRegistry Instance;
	return Instance;
}

void FGameplayTagRegistry::SetTags(const std::vector<std::string>& Tags)
{
	Registered.clear();
	Ordered.clear();
	Warned.clear();
	for (const std::string& Tag : Tags)
	{
		if (!FGameplayTags::IsValid(Tag))
		{
			continue;
		}
		Ordered.push_back(Tag);
		for (std::string& Name : FGameplayTags::GetSelfAndParents(Tag))
		{
			Registered.insert(std::move(Name));
		}
	}
}

void FGameplayTagRegistry::LoadFromProjectSettings()
{
	const FGameplayTagSettings& Settings = FProjectSettings::Get().GameplayTags;
	std::vector<std::string>    Invalid;
	SetTags(Settings.bWarnUnknownTags ? FGameplayTags::ParseList(Settings.Tags, &Invalid) : std::vector<std::string>());
	for (const std::string& Tag : Invalid)
	{
		E_LOG(LogAbility, Warning, "프로젝트 설정 게임플레이 태그: 잘못된 이름 '{}'를 건너뜁니다 (점 구분, 공백 없음)", Tag);
	}
}

bool FGameplayTagRegistry::IsRegistered(std::string_view Tag) const
{
	return Registered.empty() || Registered.contains(std::string(Tag));
}

bool FGameplayTagRegistry::Validate(std::string_view Tag, std::string_view Context)
{
	if (IsRegistered(Tag))
	{
		return true;
	}
	if (Warned.insert(std::string(Tag)).second)
	{
		E_LOG(LogAbility, Warning, "[능력] 등록되지 않은 게임플레이 태그 '{}' ({}) — 프로젝트 설정 → 게임플레이 태그에 추가하거나 오타를 고치세요", Tag, Context);
	}
	return false;
}
