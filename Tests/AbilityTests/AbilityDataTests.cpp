// 능력 정의 데이터: Phase 46 데이터 테이블(.estruct/.etable JSON) → FAbilitySet (RowRef 수정자 표, 곡선, 인라인 수정자, 태그 목록, 경고),
// 실제 데모 표(Projects/Sample Data/Abilities)를 FAbilityLibrary로 읽어 경고 없이 정의되는지
#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Ability/AbilityLibrary.h"
#include "Scene/Ability/AbilityTypes.h"
#include "Scene/DataLibrary.h"
#include "Scene/DataTable.h"

#include <map>
#include <memory>

namespace
{
	std::shared_ptr<const FDataStruct> ParseStruct(const char* Json)
	{
		auto        Struct = std::make_shared<FDataStruct>();
		std::string Error;
		E_EXPECT_TRUE(FDataStruct::FromJsonString(Json, *Struct, nullptr, &Error));
		return Struct;
	}
} // namespace

E_TEST(Ability_DataTableRoundTrip)
{
	std::map<std::string, std::shared_ptr<const FDataStruct>> Structs;
	Structs["Modifier.estruct"] = ParseStruct(R"({ "Version": 1, "Name": "Modifier", "Fields": [
		{ "Name": "Attribute", "Type": "String" },
		{ "Name": "Op", "Type": "Enum", "Values": ["Add", "Multiply", "Override"] },
		{ "Name": "MagnitudeType", "Type": "Enum", "Values": ["Constant", "Curve", "AttributeBased", "SetByCaller"] },
		{ "Name": "Value", "Type": "Float" },
		{ "Name": "Curve", "Type": "Array", "Element": "Vector2" },
		{ "Name": "SetByCallerName", "Type": "String" } ] })");
	Structs["Effect.estruct"] = ParseStruct(R"({ "Version": 1, "Name": "Effect", "Fields": [
		{ "Name": "DurationPolicy", "Type": "Enum", "Values": ["Instant", "HasDuration", "Infinite"] },
		{ "Name": "Duration", "Type": "Float" },
		{ "Name": "Period", "Type": "Float" },
		{ "Name": "Modifiers", "Type": "Array", "Element": "RowRef", "Table": "Modifiers.etable" },
		{ "Name": "Stacking", "Type": "Enum", "Values": ["None", "ByTarget", "BySource"] },
		{ "Name": "StackLimit", "Type": "Int", "Default": 1 },
		{ "Name": "GrantedTags", "Type": "Array", "Element": "String" },
		{ "Name": "AssetTags", "Type": "String" } ] })");
	Structs["Inline.estruct"] = ParseStruct(R"({ "Version": 1, "Name": "Inline", "Fields": [
		{ "Name": "Modifiers", "Type": "Array", "Element": "String" } ] })");
	Structs["Ability.estruct"] = ParseStruct(R"({ "Version": 1, "Name": "Ability", "Fields": [
		{ "Name": "Script", "Type": "Asset", "Filter": ".lua" },
		{ "Name": "CostEffect", "Type": "String" },
		{ "Name": "CooldownEffect", "Type": "String" },
		{ "Name": "InputAction", "Type": "String" },
		{ "Name": "ActivationOwnedTags", "Type": "Array", "Element": "String" },
		{ "Name": "Predicted", "Type": "Bool", "Default": true } ] })");
	Structs["Attribute.estruct"] = ParseStruct(R"({ "Version": 1, "Name": "Attribute", "Fields": [
		{ "Name": "Base", "Type": "Float" }, { "Name": "Min", "Type": "Float" }, { "Name": "MaxAttribute", "Type": "String" } ] })");
	const FDataStructResolver Resolver = [&](const std::string& Path) -> std::shared_ptr<const FDataStruct> {
		const auto Found = Structs.find(Path);
		return Found != Structs.end() ? Found->second : nullptr;
	};

	std::map<std::string, std::shared_ptr<const FDataTable>> Tables;
	const auto Parse = [&](const char* Name, const char* Json) {
		auto        Table = std::make_shared<FDataTable>();
		std::string Error;
		E_EXPECT_TRUE(FDataTable::FromJsonString(Json, Resolver, *Table, nullptr, &Error));
		Tables[Name] = Table;
		return Table;
	};
	Parse("Modifiers.etable", R"({ "Version": 1, "Struct": "Modifier.estruct", "Rows": [
		{ "Name": "BurnDamage", "Values": { "Attribute": "IncomingDamage", "Op": "Add", "MagnitudeType": "Curve", "Curve": [[1, 4], [3, 10]] } },
		{ "Name": "ManaCost", "Values": { "Attribute": "Mana", "Op": "Add", "Value": -15 } } ] })");
	const auto Effects = Parse("Effects.etable", R"({ "Version": 1, "Struct": "Effect.estruct", "Rows": [
		{ "Name": "Burn", "Values": { "DurationPolicy": "HasDuration", "Duration": 4, "Period": 1, "Modifiers": ["BurnDamage"], "Stacking": "ByTarget",
		  "StackLimit": 3, "GrantedTags": ["State.Burning"], "AssetTags": "Effect.Fire.Burn;Effect.DoT" } },
		{ "Name": "Cost_Fireball", "Values": { "Modifiers": ["ManaCost", "Missing"] } } ] })");
	const auto Inline = Parse("Inline.etable", R"({ "Version": 1, "Struct": "Inline.estruct", "Rows": [
		{ "Name": "Shield", "Values": { "Modifiers": ["DamageTaken Multiply 0.5", "IncomingDamage Add SetByCaller:Damage", "broken"] } } ] })");
	const auto Abilities = Parse("Abilities.etable", R"({ "Version": 1, "Struct": "Ability.estruct", "Rows": [
		{ "Name": "Fireball", "Values": { "Script": "Abilities/Fireball.lua", "CostEffect": "Cost_Fireball", "CooldownEffect": "Nope", "InputAction": "Skill1",
		  "ActivationOwnedTags": ["State.Casting"], "Predicted": false } } ] })");
	const auto Attributes = Parse("Attributes.etable", R"({ "Version": 1, "Struct": "Attribute.estruct", "Rows": [
		{ "Name": "Mana", "Values": { "Base": 80, "Min": 0, "MaxAttribute": "MaxMana" } }, { "Name": "IncomingDamage", "Values": { "Base": 5 } } ] })");
	const FAbilitySet::FTableLoader Loader = [&](const std::string& Path) -> std::shared_ptr<const FDataTable> {
		const auto Found = Tables.find(Path);
		return Found != Tables.end() ? Found->second : nullptr;
	};

	std::vector<std::string> Warnings;
	const auto Set = FAbilitySet::FromTables(Attributes.get(), Effects.get(), Abilities.get(), Loader, &Warnings);
	const FGameplayEffectDef* Burn = Set->FindEffect("Burn");
	E_EXPECT_TRUE(Burn != nullptr);
	if (Burn != nullptr)
	{
		E_EXPECT_TRUE(Burn->DurationPolicy == EEffectDurationPolicy::HasDuration && Burn->IsPeriodic());
		E_EXPECT_TRUE(Burn->Stacking == EEffectStacking::ByTarget && Burn->StackLimit == 3);
		E_EXPECT_TRUE(Burn->GrantedTags.size() == 1 && Burn->AssetTags.size() == 2 && Burn->AssetTags[1] == "Effect.DoT");
		E_EXPECT_TRUE(Burn->Modifiers.size() == 1 && Burn->Modifiers[0].Magnitude.Type == EMagnitudeType::Curve);
		E_EXPECT_NEAR(AbilityMath::EvaluateCurve(Burn->Modifiers[0].Magnitude.Curve, 2.0f), 7.0f, 1.0e-4f);
	}
	const FGameplayEffectDef* Cost = Set->FindEffect("Cost_Fireball");
	E_EXPECT_TRUE(Cost != nullptr && Cost->IsInstant() && Cost->Modifiers.size() == 1);
	const FAbilityDef* Fireball = Set->FindAbility("Fireball");
	E_EXPECT_TRUE(Fireball != nullptr && Fireball->Script == "Abilities/Fireball.lua" && !Fireball->bPredicted && Fireball->InputAction == "Skill1");
	E_EXPECT_TRUE(Set->FindAttribute("Mana") != nullptr && Set->FindAttribute("Mana")->MaxAttribute == "MaxMana");
	E_EXPECT_TRUE(Set->FindAttribute("IncomingDamage")->bMeta && Set->FindAttribute("IncomingDamage")->Base == 0.0f); // 메타는 항상 0
	// 경고: 없는 수정자 행, 없는 쿨다운 효과
	bool bMissingRow = false;
	bool bMissingEffect = false;
	for (const std::string& Warning : Warnings)
	{
		bMissingRow |= Warning.find("Missing") != std::string::npos;
		bMissingEffect |= Warning.find("Nope") != std::string::npos;
	}
	E_EXPECT_TRUE(bMissingRow && bMissingEffect);

	// 인라인 수정자 (String 배열)
	Warnings.clear();
	const auto InlineSet = FAbilitySet::FromTables(nullptr, Inline.get(), nullptr, Loader, &Warnings);
	const FGameplayEffectDef* Shield = InlineSet->FindEffect("Shield");
	E_EXPECT_TRUE(Shield != nullptr && Shield->Modifiers.size() == 2 && Shield->Modifiers[1].Magnitude.SetByCallerName == "Damage");
	E_EXPECT_EQ(Warnings.size(), size_t(1)); // "broken"

	// JSON 왕복: 저장 → 다시 읽어도 같은 정의
	FDataTable Reloaded;
	E_EXPECT_TRUE(FDataTable::FromJsonString(Effects->ToJsonString(), Resolver, Reloaded));
	const auto Again = FAbilitySet::FromTables(nullptr, &Reloaded, nullptr, Loader, nullptr);
	E_EXPECT_TRUE(Again->FindEffect("Burn") != nullptr && Again->FindEffect("Burn")->Modifiers.size() == 1 && Again->FindEffect("Burn")->Period == 1.0f);
}

// 데모 표 (Projects/Sample/Content/Data/Abilities)를 라이브러리로: 능력 4종·효과·속성이 있고 참조가 모두 맞다
E_TEST(Ability_DemoTablesLoad)
{
	if (!FPaths::HasProject())
	{
		return;
	}
	const auto Set = FAbilityLibrary::Get().Load("Data/Abilities/DemoAttributes.etable", "Data/Abilities/DemoEffects.etable", "Data/Abilities/DemoAbilities.etable");
	E_EXPECT_TRUE(Set != nullptr);
	if (Set == nullptr)
	{
		return;
	}
	for (const char* Name : { "Dash", "Fireball", "Shield", "HealZone" })
	{
		const FAbilityDef* Def = Set->FindAbility(Name);
		E_EXPECT_TRUE(Def != nullptr);
		if (Def != nullptr)
		{
			E_EXPECT_TRUE(Def->CooldownEffect.empty() || Set->FindEffect(Def->CooldownEffect) != nullptr);
			E_EXPECT_TRUE(Def->CostEffect.empty() || Set->FindEffect(Def->CostEffect) != nullptr);
		}
	}
	for (const auto& [Name, Effect] : Set->Effects)
	{
		for (const FModifierDef& Modifier : Effect.Modifiers)
		{
			E_EXPECT_TRUE(Set->FindAttribute(Modifier.Attribute) != nullptr);
		}
	}
	E_EXPECT_TRUE(Set->FindEffect("Burn") != nullptr && Set->FindEffect("Burn")->StackLimit == 3);
	E_EXPECT_TRUE(Set->FindAttribute("Stamina") != nullptr && Set->FindAttribute("MoveSpeed") != nullptr);
}
