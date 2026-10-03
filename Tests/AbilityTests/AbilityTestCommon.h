#pragma once

#include "Scene/Ability/AbilityTypes.h"

#include <memory>
#include <string>

// 테스트 공용 정의 (코드로 만든 FAbilitySet — 표 없이)
namespace AbilityTest
{
	inline FModifierDef Mod(const char* Attribute, EModifierOp Op, float Value)
	{
		FModifierDef Modifier;
		Modifier.Attribute       = Attribute;
		Modifier.Op              = Op;
		Modifier.Magnitude.Value = Value;
		return Modifier;
	}

	inline FGameplayEffectDef Effect(const char* Name, EEffectDurationPolicy Policy, float Duration, std::vector<FModifierDef> Modifiers)
	{
		FGameplayEffectDef Def;
		Def.Name           = Name;
		Def.DurationPolicy = Policy;
		Def.Duration       = Duration;
		Def.Modifiers      = std::move(Modifiers);
		return Def;
	}

	// SpellScript: "Spell" 능력의 Lua 스크립트 (비면 스크립트 없음)
	inline std::shared_ptr<FAbilitySet> MakeSet(const std::string& SpellScript = {})
	{
		auto Set = std::make_shared<FAbilitySet>();
		Set->AddAttribute({ "Health", 100.0f, 0.0f, 1.0e30f, "MaxHealth" });
		Set->AddAttribute({ "MaxHealth", 100.0f, 1.0f });
		Set->AddAttribute({ "Mana", 100.0f, 0.0f, 1.0e30f, "MaxMana" });
		Set->AddAttribute({ "MaxMana", 100.0f, 0.0f });
		Set->AddAttribute({ "MoveSpeed", 400.0f, 0.0f });
		Set->AddAttribute({ "DamageTaken", 1.0f, 0.0f });
		Set->AddAttribute({ "IncomingDamage" });
		Set->AddAttribute({ "IncomingHeal" });

		using P = EEffectDurationPolicy;
		using O = EModifierOp;
		FGameplayEffectDef Haste = Effect("Haste", P::Infinite, 0.0f, { Mod("MoveSpeed", O::Add, 50.0f), Mod("MoveSpeed", O::Multiply, 1.5f) });
		Haste.ApplicationBlockedTags = { "State.Stunned" };
		Haste.GrantedTags            = { "State.Hasted" };
		Set->AddEffect(Haste);
		Set->AddEffect(Effect("ManaPotion", P::Instant, 0.0f, { Mod("Mana", O::Add, 500.0f) }));

		FGameplayEffectDef Burn = Effect("Burn", P::HasDuration, 4.0f, { Mod("IncomingDamage", O::Add, 5.0f) });
		Burn.Period     = 1.0f;
		Burn.Stacking   = EEffectStacking::ByTarget;
		Burn.StackLimit = 3;
		Burn.AssetTags  = { "Effect.Fire.Burn" };
		Burn.GrantedTags = { "State.Burning" };
		Set->AddEffect(Burn);

		FGameplayEffectDef Poison = Effect("Poison", P::HasDuration, 10.0f, {});
		Poison.Stacking   = EEffectStacking::BySource;
		Poison.StackLimit = 5;
		Poison.AssetTags  = { "Effect.Poison" };
		Set->AddEffect(Poison);

		FGameplayEffectDef Focus = Effect("Focus", P::HasDuration, 2.0f, {});
		Focus.Stacking        = EEffectStacking::ByTarget;
		Focus.StackLimit      = 3;
		Focus.StackExpiration = EStackExpiration::RemoveOneAndRefresh;
		Focus.GrantedTags     = { "State.Focused" };
		Set->AddEffect(Focus);

		FGameplayEffectDef Regen = Effect("Regen", P::Infinite, 0.0f, { Mod("Mana", O::Add, 5.0f) });
		Regen.Period                  = 1.0f;
		Regen.bExecutePeriodicOnApply = true;
		Set->AddEffect(Regen);

		FGameplayEffectDef Shield = Effect("Shield", P::HasDuration, 5.0f, { Mod("DamageTaken", O::Multiply, 0.5f) });
		Shield.GrantedTags = { "State.Shielded" };
		Set->AddEffect(Shield);

		FGameplayEffectDef Damage = Effect("Damage", P::Instant, 0.0f, { Mod("IncomingDamage", O::Add, 0.0f) });
		Damage.Modifiers[0].Magnitude.Type            = EMagnitudeType::SetByCaller;
		Damage.Modifiers[0].Magnitude.SetByCallerName = "Damage";
		Set->AddEffect(Damage);

		FGameplayEffectDef FireWard = Effect("FireWard", P::Infinite, 0.0f, {});
		FireWard.RemoveEffectsWithTags = { "Effect.Fire" };
		FireWard.GrantedImmunityTags   = { "Effect.Fire" };
		Set->AddEffect(FireWard);

		FGameplayEffectDef Execute = Effect("Execute", P::Instant, 0.0f, { Mod("IncomingDamage", O::Add, 1.0f) });
		Execute.ApplicationRequiredTags = { "State.Stunned" };
		Set->AddEffect(Execute);

		FGameplayEffectDef Stun = Effect("Stun", P::HasDuration, 1.0f, {});
		Stun.GrantedTags = { "State.Stunned" };
		Set->AddEffect(Stun);

		FGameplayEffectDef Fortify = Effect("Fortify", P::Infinite, 0.0f, { Mod("MaxHealth", O::Add, 50.0f) });
		Fortify.AssetTags = { "Effect.Buff" };
		Set->AddEffect(Fortify);
		Set->AddEffect(Effect("Heal", P::Instant, 0.0f, { Mod("IncomingHeal", O::Add, 100.0f) }));

		Set->AddEffect(Effect("Cost_Bolt", P::Instant, 0.0f, { Mod("Mana", O::Add, -30.0f) }));
		FGameplayEffectDef BoltCooldown = Effect("Cooldown_Bolt", P::HasDuration, 2.0f, {});
		BoltCooldown.GrantedTags = { "Ability.Cooldown.Bolt" };
		Set->AddEffect(BoltCooldown);
		Set->AddEffect(Effect("Cost_Spell", P::Instant, 0.0f, { Mod("Mana", O::Add, -20.0f) }));
		FGameplayEffectDef SpellCooldown = Effect("Cooldown_Spell", P::HasDuration, 2.0f, {});
		SpellCooldown.GrantedTags = { "Ability.Cooldown.Spell" };
		Set->AddEffect(SpellCooldown);

		FAbilityDef Bolt;
		Bolt.Name                  = "Bolt";
		Bolt.CostEffect            = "Cost_Bolt";
		Bolt.CooldownEffect        = "Cooldown_Bolt";
		Bolt.AbilityTags           = { "Ability.Bolt" };
		Bolt.ActivationBlockedTags = { "State.Stunned" };
		Set->AddAbility(Bolt);

		FAbilityDef Channel;
		Channel.Name                   = "Channel";
		Channel.Native                 = "Test.Channel";
		Channel.AbilityTags            = { "Ability.Channel" };
		Channel.ActivationOwnedTags    = { "State.Channeling" };
		Channel.BlockAbilitiesWithTags = { "Ability.Bolt" };
		Channel.ActivationBlockedTags  = { "State.Stunned" };
		Set->AddAbility(Channel);

		FAbilityDef Interrupt;
		Interrupt.Name                    = "Interrupt";
		Interrupt.CancelAbilitiesWithTags = { "Ability.Channel" };
		Set->AddAbility(Interrupt);

		FAbilityDef Spell;
		Spell.Name                  = "Spell";
		Spell.Script                = SpellScript;
		Spell.CostEffect            = "Cost_Spell";
		Spell.CooldownEffect        = "Cooldown_Spell";
		Spell.ActivationOwnedTags   = { "State.Casting" };
		Spell.ActivationBlockedTags = { "State.Stunned" };
		Spell.InputAction           = "Skill1";
		Set->AddAbility(Spell);
		return Set;
	}
} // namespace AbilityTest
