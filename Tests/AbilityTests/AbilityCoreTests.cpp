// 능력 시스템 순수 규칙 + 권한(Standalone) 동작: 태그, 수정자 순서, 쌓기/갱신, 주기, 면역/차단, 쿨다운/비용, 체력 연동, C++ 능력
#include "AbilityTestCommon.h"

#include "Core/Testing/TestFramework.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/Ability/GameplayTags.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"

#include <cmath>

namespace
{
	constexpr float Step = 1.0f / 30.0f;

	float Attribute(FAbilitySystem& System, FEntity Entity, const char* Name, bool bBase = false)
	{
		float Value = -12345.0f;
		System.GetAttribute(Entity, Name, Value, bBase);
		return Value;
	}

	void TickSeconds(FAbilitySystem& System, float Seconds)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Step));
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			System.Tick(Step);
		}
	}
} // namespace

// 계층 태그: 부모 질의는 자식을 포함, 반대는 아님. 수 컨테이너의 명시/암시 수, 목록 파싱, 등록 목록 검사
E_TEST(Ability_TagQueries)
{
	E_EXPECT_TRUE(FGameplayTags::Matches("State.Stunned.Hard", "State.Stunned"));
	E_EXPECT_TRUE(FGameplayTags::Matches("State.Stunned", "State"));
	E_EXPECT_TRUE(FGameplayTags::Matches("State.Stunned", "State.Stunned"));
	E_EXPECT_FALSE(FGameplayTags::Matches("State.Stunned", "State.Stunned.Hard"));
	E_EXPECT_FALSE(FGameplayTags::Matches("State.StunnedX", "State.Stunned")); // 접두 글자가 같아도 다른 마디
	E_EXPECT_FALSE(FGameplayTags::IsValid("State..Stunned"));
	E_EXPECT_FALSE(FGameplayTags::IsValid(".State"));
	E_EXPECT_FALSE(FGameplayTags::IsValid("State Stunned"));

	std::vector<std::string> Invalid;
	const std::vector<std::string> List = FGameplayTags::ParseList(" A.B ; C,\nA.B; bad..tag ;", &Invalid);
	E_EXPECT_EQ(List.size(), size_t(2));
	E_EXPECT_TRUE(List[0] == "A.B" && List[1] == "C");
	E_EXPECT_EQ(Invalid.size(), size_t(1));

	FGameplayTagCountContainer Tags;
	E_EXPECT_TRUE(Tags.AddTag("State.Stunned.Hard"));
	E_EXPECT_FALSE(Tags.AddTag("State.Stunned.Hard")); // 1 → 2: 있음/없음 변화 아님
	E_EXPECT_TRUE(Tags.AddTag("State.Burning"));
	E_EXPECT_EQ(Tags.GetCount("State"), 3);
	E_EXPECT_EQ(Tags.GetCount("State.Stunned"), 2);
	E_EXPECT_TRUE(Tags.HasTag("State.Stunned"));
	E_EXPECT_FALSE(Tags.HasTagExact("State.Stunned"));
	E_EXPECT_TRUE(Tags.HasAll({ "State.Burning", "State.Stunned" }));
	E_EXPECT_TRUE(Tags.HasAll({}));
	E_EXPECT_FALSE(Tags.HasAny({}));
	E_EXPECT_FALSE(Tags.HasAny({ "Ability" }));
	Tags.RemoveTag("State.Stunned.Hard", 5); // 0 아래로 내려가지 않는다
	E_EXPECT_EQ(Tags.GetCount("State"), 1);
	E_EXPECT_FALSE(Tags.HasTag("State.Stunned"));

	FGameplayTagRegistry& Registry = FGameplayTagRegistry::Get();
	Registry.SetTags({ "State.Stunned", "Ability.Cooldown.Dash" });
	E_EXPECT_TRUE(Registry.IsRegistered("State"));          // 부모 자동
	E_EXPECT_TRUE(Registry.IsRegistered("Ability.Cooldown"));
	E_EXPECT_FALSE(Registry.IsRegistered("State.Stuned"));  // 오타
	E_EXPECT_FALSE(Registry.Validate("State.Stuned", "테스트"));
	Registry.SetTags({});
	E_EXPECT_TRUE(Registry.IsRegistered("Anything.Goes")); // 빈 목록 = 검사 끔
}

// 수정자 계산 순서: (Base + ΣAdd) × ΠMultiply, Override는 가장 나중 것이 이긴다, 스택(Add × n, Multiply^n). 크기: 곡선/속성 비례/SetByCaller
E_TEST(Ability_ModifierOrder)
{
	using M = FAppliedModifier;
	E_EXPECT_NEAR(AbilityMath::Aggregate(100.0f, { M{ EModifierOp::Multiply, 0.5f }, M{ EModifierOp::Add, 20.0f } }), 60.0f, 1.0e-4f); // 순서와 무관
	E_EXPECT_NEAR(AbilityMath::Aggregate(100.0f, { M{ EModifierOp::Multiply, 0.5f }, M{ EModifierOp::Multiply, 0.5f } }), 25.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::Aggregate(100.0f, { M{ EModifierOp::Add, 5.0f, 3 } }), 115.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::Aggregate(100.0f, { M{ EModifierOp::Multiply, 0.5f, 2 } }), 25.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::Aggregate(100.0f, { M{ EModifierOp::Override, 7.0f, 1, 2 }, M{ EModifierOp::Add, 50.0f }, M{ EModifierOp::Override, 9.0f, 1, 1 } }), 7.0f, 1.0e-4f);

	const std::vector<FVector2> Curve = { FVector2(1.0f, 10.0f), FVector2(3.0f, 30.0f), FVector2(5.0f, 20.0f) };
	E_EXPECT_NEAR(AbilityMath::EvaluateCurve(Curve, 0.0f), 10.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::EvaluateCurve(Curve, 2.0f), 20.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::EvaluateCurve(Curve, 4.0f), 25.0f, 1.0e-4f);
	E_EXPECT_NEAR(AbilityMath::EvaluateCurve(Curve, 9.0f), 20.0f, 1.0e-4f);

	FMagnitudeDef Based;
	Based.Type        = EMagnitudeType::AttributeBased;
	Based.Value       = 0.5f;
	Based.PostAdd     = 2.0f;
	Based.Attribute   = "Power";
	Based.bFromSource = true;
	const AbilityMath::FAttributeReader Source = [](std::string_view Name, float& Out) { Out = Name == "Power" ? 40.0f : 0.0f; return Name == "Power"; };
	const AbilityMath::FAttributeReader Target = [](std::string_view, float&) { return false; };
	E_EXPECT_NEAR(AbilityMath::EvaluateMagnitude(Based, 1.0f, Source, Target, {}), 22.0f, 1.0e-4f);
	FMagnitudeDef Caller;
	Caller.Type            = EMagnitudeType::SetByCaller;
	Caller.SetByCallerName = "Damage";
	bool bMissing          = false;
	E_EXPECT_NEAR(AbilityMath::EvaluateMagnitude(Caller, 1.0f, Source, Target, { { "Damage", 13.0f } }, &bMissing), 13.0f, 1.0e-4f);
	E_EXPECT_FALSE(bMissing);
	E_EXPECT_NEAR(AbilityMath::EvaluateMagnitude(Caller, 1.0f, Source, Target, {}, &bMissing), 0.0f, 1.0e-4f);
	E_EXPECT_TRUE(bMissing);

	FModifierDef Inline;
	E_EXPECT_TRUE(AbilityMath::ParseInlineModifier("DamageTaken Multiply 0.5", Inline));
	E_EXPECT_TRUE(Inline.Attribute == "DamageTaken" && Inline.Op == EModifierOp::Multiply);
	E_EXPECT_NEAR(Inline.Magnitude.Value, 0.5f, 1.0e-6f);
	E_EXPECT_TRUE(AbilityMath::ParseInlineModifier("IncomingDamage Add SetByCaller:Damage", Inline));
	E_EXPECT_TRUE(Inline.Magnitude.Type == EMagnitudeType::SetByCaller && Inline.Magnitude.SetByCallerName == "Damage");
	E_EXPECT_FALSE(AbilityMath::ParseInlineModifier("Mana Plus 3", Inline));
	E_EXPECT_FALSE(AbilityMath::ParseInlineModifier("Mana Add", Inline));

	// 시스템 안: 지속 효과 수정자는 현재값만, 즉시 효과는 기본값, 최대 속성으로 자르기
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity Hero = Scene.CreateEntity("Hero");
	E_EXPECT_TRUE(System.InitializeWithSet(Hero, AbilityTest::MakeSet()));
	E_EXPECT_TRUE(System.ApplyEffect(Hero, "Haste").bApplied); // MoveSpeed ×1.5 + 50
	E_EXPECT_NEAR(Attribute(System, Hero, "MoveSpeed"), (400.0f + 50.0f) * 1.5f, 1.0e-3f);
	E_EXPECT_NEAR(Attribute(System, Hero, "MoveSpeed", true), 400.0f, 1.0e-3f);
	E_EXPECT_TRUE(System.ApplyEffect(Hero, "ManaPotion").bApplied); // 즉시 +500 → MaxMana(100)로 잘림
	E_EXPECT_NEAR(Attribute(System, Hero, "Mana", true), 100.0f, 1.0e-3f);
	System.End();
}

// 쌓기: ByTarget은 대상에 하나(상한, 지속 갱신), BySource는 원천마다 하나. RemoveOneAndRefresh 만료는 스택을 하나씩
E_TEST(Ability_StackingAndRefresh)
{
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity Dummy  = Scene.CreateEntity("Dummy");
	const FEntity Caster = Scene.CreateEntity("Caster");
	const FEntity Other  = Scene.CreateEntity("Other");
	const auto    Set    = AbilityTest::MakeSet();
	for (FEntity Entity : { Dummy, Caster, Other })
	{
		System.InitializeWithSet(Entity, Set);
	}
	FEffectApplyParams FromCaster;
	FromCaster.Source = Caster;
	const uint32 Handle = System.ApplyEffect(Dummy, "Burn", FromCaster).Handle;
	E_EXPECT_TRUE(Handle != 0);
	TickSeconds(System, 1.5f);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		E_EXPECT_EQ(System.ApplyEffect(Dummy, "Burn", FromCaster).Handle, Handle); // 같은 인스턴스
	}
	std::vector<FActiveEffectView> Effects = System.GetActiveEffects(Dummy);
	E_EXPECT_EQ(Effects.size(), size_t(1));
	E_EXPECT_EQ(Effects[0].Stacks, 3); // 상한 3
	E_EXPECT_NEAR(Effects[0].Remaining, 4.0f, 1.0e-3f); // 갱신

	// BySource: 원천마다 따로
	FEffectApplyParams FromOther;
	FromOther.Source = Other;
	System.ApplyEffect(Dummy, "Poison", FromCaster);
	System.ApplyEffect(Dummy, "Poison", FromOther);
	System.ApplyEffect(Dummy, "Poison", FromOther);
	int32 PoisonCount = 0;
	int32 PoisonStacks = 0;
	for (const FActiveEffectView& Effect : System.GetActiveEffects(Dummy))
	{
		if (Effect.Name == "Poison")
		{
			++PoisonCount;
			PoisonStacks += Effect.Stacks;
		}
	}
	E_EXPECT_EQ(PoisonCount, 2);
	E_EXPECT_EQ(PoisonStacks, 3);

	// RemoveOneAndRefresh: 스택 2 → 지속(2초)마다 하나씩 → 4초 뒤 사라짐
	System.ApplyEffect(Caster, "Focus");
	System.ApplyEffect(Caster, "Focus");
	E_EXPECT_EQ(System.GetTagCount(Caster, "State.Focused"), 1); // 부여 태그는 스택과 무관하게 1
	TickSeconds(System, 2.1f);
	Effects = System.GetActiveEffects(Caster);
	E_EXPECT_TRUE(Effects.size() == 1 && Effects[0].Stacks == 1);
	TickSeconds(System, 2.0f);
	E_EXPECT_TRUE(System.GetActiveEffects(Caster).empty());
	E_EXPECT_FALSE(System.HasTag(Caster, "State.Focused"));
	System.End();
}

// 주기 효과: 4초 지속·1초 주기 = 4번 (만료 순간 포함), 스택 배. 적용 순간 실행 옵션. 데미지는 IncomingDamage → 체력 컴포넌트
E_TEST(Ability_PeriodicEffect)
{
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity Dummy = Scene.CreateEntity("Dummy");
	FHealthComponent& Health = Scene.GetRegistry().Emplace<FHealthComponent>(Dummy);
	Health.MaxHealth = Health.Health = 200.0f;
	const FEntity Caster = Scene.CreateEntity("Caster");
	System.InitializeWithSet(Dummy, AbilityTest::MakeSet());
	System.InitializeWithSet(Caster, AbilityTest::MakeSet());
	E_EXPECT_NEAR(Attribute(System, Dummy, "Health"), 200.0f, 1.0e-3f); // 체력 컴포넌트 값으로 시작
	E_EXPECT_NEAR(Attribute(System, Dummy, "MaxHealth"), 200.0f, 1.0e-3f);

	FEffectApplyParams Params;
	Params.Source = Caster;
	System.ApplyEffect(Dummy, "Burn", Params);
	TickSeconds(System, 5.0f);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Dummy).Health, 200.0f - 4 * 5.0f, 1.0e-3f);
	E_EXPECT_TRUE(System.GetActiveEffects(Dummy).empty());
	// 데미지 이벤트는 체력 컴포넌트 큐에 (가해자 = 원천)
	const std::vector<FDamageEvent>& Pending = Scene.GetRegistry().Get<FHealthComponent>(Dummy).Runtime.PendingEvents;
	E_EXPECT_EQ(Pending.size(), size_t(4));
	E_EXPECT_TRUE(!Pending.empty() && Pending[0].Instigator == Caster);

	// 스택 2: 주기마다 2배, Regen은 적용 순간에도 실행
	Scene.GetRegistry().Get<FHealthComponent>(Dummy).Runtime.PendingEvents.clear();
	System.ApplyEffect(Dummy, "Burn", Params);
	System.ApplyEffect(Dummy, "Burn", Params);
	TickSeconds(System, 1.0f);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Dummy).Health, 180.0f - 10.0f, 1.0e-3f);
	System.SetBaseAttribute(Caster, "Mana", 10.0f);
	System.ApplyEffect(Caster, "Regen"); // 무한, 1초마다 Mana +5, 적용 순간 실행
	E_EXPECT_NEAR(Attribute(System, Caster, "Mana"), 15.0f, 1.0e-3f);
	TickSeconds(System, 2.0f);
	E_EXPECT_NEAR(Attribute(System, Caster, "Mana"), 25.0f, 1.0e-3f);

	// 방어막: DamageTaken ×0.5 → 받는 피해 절반
	const float Before = Scene.GetRegistry().Get<FHealthComponent>(Dummy).Health;
	System.ApplyEffect(Dummy, "Shield");
	FEffectApplyParams Hit;
	Hit.Source                    = Caster;
	Hit.SetByCaller["Damage"]     = 30.0f;
	System.ApplyEffect(Dummy, "Damage", Hit);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Dummy).Health, Before - 15.0f, 1.0e-3f);
	System.End();
}

// 면역(GrantedImmunityTags ↔ AssetTags), 적용 차단/필요 태그, RemoveEffectsWithTags
E_TEST(Ability_ImmunityAndBlocking)
{
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity Target = Scene.CreateEntity("Target");
	System.InitializeWithSet(Target, AbilityTest::MakeSet());

	System.ApplyEffect(Target, "Burn");
	E_EXPECT_TRUE(System.HasTag(Target, "State.Burning"));
	E_EXPECT_TRUE(System.ApplyEffect(Target, "FireWard").bApplied); // RemoveEffectsWithTags Effect.Fire + 면역
	E_EXPECT_FALSE(System.HasTag(Target, "State.Burning"));
	const FEffectApplyResult Immune = System.ApplyEffect(Target, "Burn");
	E_EXPECT_FALSE(Immune.bApplied);
	E_EXPECT_TRUE(Immune.Reason == "Immune");
	E_EXPECT_TRUE(System.ApplyEffect(Target, "Poison").bApplied); // 불 아님

	// 필요 태그: Execute는 State.Stunned가 있어야
	E_EXPECT_TRUE(System.ApplyEffect(Target, "Execute").Reason == "MissingTags");
	System.ApplyEffect(Target, "Stun");
	E_EXPECT_TRUE(System.ApplyEffect(Target, "Execute").bApplied);
	// 차단 태그: Haste는 State.Stunned면 안 된다
	E_EXPECT_TRUE(System.ApplyEffect(Target, "Haste").Reason == "Blocked");
	E_EXPECT_EQ(System.RemoveEffectsWithTags(Target, { "State.Stunned" }), 1);
	E_EXPECT_TRUE(System.ApplyEffect(Target, "Haste").bApplied);
	E_EXPECT_TRUE(System.ApplyEffect(Target, "NoSuchEffect").Reason == "UnknownEffect");
	System.End();
}

// 발동: 비용(Mana)·쿨다운(태그 부여 지속 효과)·발동 중 태그·Block/Cancel 태그, 실패 이유 이벤트
E_TEST(Ability_CooldownAndCost)
{
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity Hero = Scene.CreateEntity("Hero");
	System.InitializeWithSet(Hero, AbilityTest::MakeSet());
	System.ConsumeEvents();

	FAbilityActivateResult Result = System.TryActivateAbility(Hero, "Bolt"); // 스크립트 없음: 커밋만 하고 끝
	E_EXPECT_TRUE(Result.bActivated);
	E_EXPECT_NEAR(Attribute(System, Hero, "Mana"), 70.0f, 1.0e-3f);
	E_EXPECT_TRUE(System.HasTag(Hero, "Ability.Cooldown.Bolt"));
	E_EXPECT_FALSE(System.IsAbilityActive(Hero, "Bolt"));
	float Duration = 0.0f;
	E_EXPECT_NEAR(System.GetCooldownRemaining(Hero, "Bolt", &Duration), 2.0f, 1.0e-3f);
	E_EXPECT_NEAR(Duration, 2.0f, 1.0e-3f);
	Result = System.TryActivateAbility(Hero, "Bolt");
	E_EXPECT_FALSE(Result.bActivated);
	E_EXPECT_TRUE(Result.Reason == "Cooldown");
	TickSeconds(System, 2.1f);
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Bolt").bActivated);
	TickSeconds(System, 2.1f);
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Bolt").bActivated); // Mana 10 남음
	TickSeconds(System, 2.1f);
	Result = System.TryActivateAbility(Hero, "Bolt");
	E_EXPECT_TRUE(Result.Reason == "Cost");
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Missing").Reason == "NotGranted");

	bool bSawFailed = false;
	bool bSawActivated = false;
	for (const FAbilityEvent& Event : System.ConsumeEvents())
	{
		bSawFailed |= Event.Type == EAbilityEventType::AbilityFailed && Event.Name == "Bolt" && Event.Reason == "Cost";
		bSawActivated |= Event.Type == EAbilityEventType::AbilityActivated && Event.Name == "Bolt";
	}
	E_EXPECT_TRUE(bSawFailed && bSawActivated);

	// C++ 능력 (지속): 발동 중 State.Channeling, BlockAbilitiesWithTags로 Bolt 막음, 기절(차단 태그)이 오면 Stun 효과가 Channel을 취소하지는 않지만 발동은 막는다
	int32 Ticks = 0;
	bool  bEndedCancelled = false;
	FAbilityNativeRegistry::Get().Register("Test.Channel", { nullptr, [&](FNativeAbilityContext&, float) { return ++Ticks < 15; },
	                                                          [&](FNativeAbilityContext&, bool bCancelled) { bEndedCancelled = bCancelled; } },
	                                       "AbilityTests");
	System.SetBaseAttribute(Hero, "Mana", 100.0f);
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Channel").bActivated);
	E_EXPECT_TRUE(System.IsAbilityActive(Hero, "Channel"));
	E_EXPECT_TRUE(System.HasTag(Hero, "State.Channeling"));
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Bolt").Reason == "Blocked");
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Channel").Reason == "Active");
	TickSeconds(System, 0.6f); // 15틱 → 끝
	E_EXPECT_FALSE(System.IsAbilityActive(Hero, "Channel"));
	E_EXPECT_FALSE(bEndedCancelled);
	E_EXPECT_FALSE(System.HasTag(Hero, "State.Channeling"));
	// 다시 발동 → Interrupt(CancelAbilitiesWithTags Ability.Channel)가 취소
	Ticks = 0;
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Channel").bActivated);
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Interrupt").bActivated);
	E_EXPECT_FALSE(System.IsAbilityActive(Hero, "Channel"));
	E_EXPECT_TRUE(bEndedCancelled);
	// 기절 중에는 발동 못 함, 죽으면 못 함
	System.ApplyEffect(Hero, "Stun");
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Channel").Reason == "Blocked");
	E_EXPECT_EQ(FAbilityNativeRegistry::Get().RemoveByOwner("AbilityTests"), size_t(1));
	System.End();
}

// 체력 연동: 체력 컴포넌트가 진실. 바깥 데미지(Gameplay::ApplyDamage)가 속성에 반영, MaxHealth 수정자 → 컴포넌트 MaxHealth, 죽으면 발동 불가.
// 속성/태그 변화 이벤트
E_TEST(Ability_HealthComponentSync)
{
	FScene         Scene;
	FAbilitySystem System;
	System.Begin(Scene, true);
	const FEntity     Hero   = Scene.CreateEntity("Hero");
	FHealthComponent& Health = Scene.GetRegistry().Emplace<FHealthComponent>(Hero);
	Health.MaxHealth = Health.Health = 100.0f;
	System.InitializeWithSet(Hero, AbilityTest::MakeSet());
	System.Tick(Step);
	System.ConsumeEvents();

	Gameplay::ApplyDamage(Scene, Hero, 30.0f, FEntity());
	System.Tick(Step);
	E_EXPECT_NEAR(Attribute(System, Hero, "Health"), 70.0f, 1.0e-3f);
	bool bHealthEvent = false;
	for (const FAbilityEvent& Event : System.ConsumeEvents())
	{
		bHealthEvent |= Event.Type == EAbilityEventType::AttributeChanged && Event.Name == "Health" && std::fabs(Event.Value - 70.0f) < 1.0e-3f &&
		                std::fabs(Event.OldValue - 100.0f) < 1.0e-3f;
	}
	E_EXPECT_TRUE(bHealthEvent);

	System.ApplyEffect(Hero, "Fortify"); // MaxHealth +50
	System.Tick(Step);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Hero).MaxHealth, 150.0f, 1.0e-3f);
	System.ApplyEffect(Hero, "Heal"); // IncomingHeal 100 → 150까지
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Hero).Health, 150.0f, 1.0e-3f);
	E_EXPECT_TRUE(System.RemoveEffectsWithTags(Hero, { "Effect.Buff" }) == 1);
	System.Tick(Step);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Hero).MaxHealth, 100.0f, 1.0e-3f);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Hero).Health, 100.0f, 1.0e-3f); // 줄어든 최대로 잘림

	System.AddLooseTag(Hero, "State.Marked");
	System.Tick(Step);
	bool bTagEvent = false;
	for (const FAbilityEvent& Event : System.ConsumeEvents())
	{
		bTagEvent |= Event.Type == EAbilityEventType::TagChanged && Event.Name == "State.Marked" && Event.Count == 1;
	}
	E_EXPECT_TRUE(bTagEvent);

	Gameplay::ApplyDamage(Scene, Hero, 1000.0f, FEntity());
	E_EXPECT_TRUE(System.TryActivateAbility(Hero, "Bolt").Reason == "Dead");
	System.End();
}
