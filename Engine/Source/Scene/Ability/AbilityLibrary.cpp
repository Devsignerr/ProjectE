#include "Scene/Ability/AbilityLibrary.h"

#include "Scene/DataLibrary.h"

#include <vector>

FAbilityLibrary& FAbilityLibrary::Get()
{
	static FAbilityLibrary Instance;
	return Instance;
}

uint32 FAbilityLibrary::GetGeneration() const
{
	return FDataLibrary::Get().GetGeneration();
}

std::string FAbilityLibrary::MakeKey(const std::string& AttributeTable, const std::string& EffectTable, const std::string& AbilityTable)
{
	return AttributeTable + "|" + EffectTable + "|" + AbilityTable;
}

std::shared_ptr<const FAbilitySet> FAbilityLibrary::Load(const std::string& AttributeTable, const std::string& EffectTable, const std::string& AbilityTable)
{
	if (AttributeTable.empty() && EffectTable.empty() && AbilityTable.empty())
	{
		return nullptr;
	}
	FDataLibrary&     Data       = FDataLibrary::Get();
	const std::string Key        = MakeKey(AttributeTable, EffectTable, AbilityTable);
	const uint32      Generation = Data.GetGeneration();
	if (const auto Found = Cache.find(Key); Found != Cache.end() && Found->second.Generation == Generation)
	{
		return Found->second.Set;
	}

	const std::shared_ptr<const FDataTable> Attributes = AttributeTable.empty() ? nullptr : Data.LoadTable(AttributeTable);
	const std::shared_ptr<const FDataTable> Effects    = EffectTable.empty() ? nullptr : Data.LoadTable(EffectTable);
	const std::shared_ptr<const FDataTable> Abilities  = AbilityTable.empty() ? nullptr : Data.LoadTable(AbilityTable);
	std::vector<std::string>                Warnings;
	for (const auto& [Path, Table] : { std::pair{ &AttributeTable, Attributes }, std::pair{ &EffectTable, Effects }, std::pair{ &AbilityTable, Abilities } })
	{
		if (!Path->empty() && Table == nullptr)
		{
			Warnings.push_back("표를 읽지 못했습니다: " + *Path);
		}
	}
	std::shared_ptr<const FAbilitySet> Set = FAbilitySet::FromTables(Attributes.get(), Effects.get(), Abilities.get(),
		[&Data](const std::string& Path) { return Data.LoadTable(Path); }, &Warnings);
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogAbility, Warning, "[능력] {}", Warning);
	}
	Cache[Key] = { Data.GetGeneration(), Set };
	return Set;
}
