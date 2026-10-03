// 능력 시스템 (언리얼 GAS식, Phase 53 사이드) — 규칙 기준.
//
// 역할
//   권한(서버/Standalone): 발동 확정, 효과 적용, 속성 변경, 효과 시간/주기, 복제 문자열 쓰기. 모든 상태가 Runtime에 있다.
//   클라이언트: 복제 문자열(Rep*)을 읽어 보기(Attributes/Tags)를 만들고, 소유 클라이언트는 그 위에 예측을 얹는다.
// 발동 (TryActivateAbility)
//   검사 순서: 부여됨 → 이미 발동 중 아님 → 살아 있음(FHealthComponent) → 필요 태그 → 차단 태그(+ 발동 중 능력의 BlockAbilitiesWithTags)
//   → 쿨다운(쿨다운 효과의 GrantedTags를 가짐) → 비용(비용 효과를 적용하면 어떤 속성 기본값이 0 미만). 실패 이유는 이벤트 AbilityFailed.
//   발동: 인스턴스(능력마다 하나) → ActivationOwnedTags → CancelAbilitiesWithTags → (ManualCommit 아니면) 커밋(비용 → 쿨다운 → ActivationEffects)
//   → 스크립트 OnActivate(코루틴, 첫 대기까지) / C++ 능력 / 둘 다 없으면 바로 끝. 스크립트가 반환하면 끝난다(ctx:EndAbility로 일찍 끝내도 됨).
// 효과 적용 (권한): 면역(대상 활성 효과의 GrantedImmunityTags에 AssetTags가 맞음) → 필요/차단 태그 → RemoveEffectsWithTags → 크기 캡처 →
//   즉시: 기본값 변경 / 지속·무한: 쌓기(같은 이름[+같은 원천] 있으면 스택+1(상한), 지속 갱신) 또는 새 인스턴스 + GrantedTags.
//   주기 효과는 Period마다 수정자를 즉시 효과처럼 실행(스택 배). 만료: ClearAll = 제거, RemoveOneAndRefresh = 스택-1 + 시간 새로.
//   수정자 계산 순서·예약 속성(Health/IncomingDamage 등)은 AbilityTypes.h 머리 주석.
// 체력 연동: Health 속성의 진실은 FHealthComponent (있을 때). 매 틱 Health 기본값 ← 컴포넌트, MaxHealth 현재값 → 컴포넌트 MaxHealth.
//   Health 즉시 변화와 IncomingDamage/IncomingHeal은 Gameplay::ApplyDamage/Heal을 거친다 → OnDamaged/OnDeath/리스폰 규칙이 그대로 돈다.
//   초기화 때 Health/MaxHealth 기본값은 컴포넌트 값을 쓴다(씬에 적힌 체력 우선).
// 예측 (소유 클라이언트, 능력 bPredicted)
//   키: 엔티티마다 1부터 늘어나는 예측 키. 발동 요청(AbilityActivate 메시지)에 실어 보낸다.
//   예측 범위: 발동 검사, 비용(기본값 변화량), 쿨다운·ActivationEffects·스크립트가 예측 창에서 자기에게 건 효과(예측 효과), 발동 태그,
//     능력 스크립트 로컬 실행(몽타주 등 연출). 예측 창 = OnActivate가 처음 대기(ctx:Wait 등)에 들어가기 전. 그 뒤 자기 효과·커밋은 서버만.
//   예측하지 않음: 다른 대상 효과·데미지·투사체·스폰(서버만, ctx:HasAuthority), 주기 실행, 효과 제거, Health/메타 속성.
//   서버: 같은 키로 발동하고 예측 창 안에서 적용한 효과에 그 키를 붙인다. 처리한 최대 키를 RepPredictionKey로 복제한다
//     (복제 문자열과 같은 컴포넌트라 "이 키까지의 결과가 이 값에 들어 있다"가 항상 맞다). 거절이면 AbilityResult(거절, 이유)를 소유자에게.
//   합치기: RepPredictionKey >= 키가 오면 그 키까지의 예측 효과·변화량을 버린다 → 서버 값만 남아 중복 적용이 없다.
//   거절: 그 키의 예측 효과·변화량을 즉시 되돌리고 로컬 인스턴스를 취소(OnEnd cancelled) + AbilityFailed("Rejected:<이유>").
//   서버가 예측 발동을 취소로 끝내면 AbilityResult(취소) → 로컬 인스턴스 취소. 3초 넘게 처리되지 않은 예측은 버린다(경고).
// 복제 문자열 (권한이 틱 끝에 값이 바뀌었을 때만 쓴다):
//   RepAttributes "이름=기본,현재;..."  RepTags "태그=수;..."(명시 수)  RepAbilities "능력=발동중(0/1),누적발동수;..."
//   RepEffects "핸들,이름,스택,남은초,지속초,예측키;..." — 효과 구조(핸들/스택/지속/갱신 수)가 바뀔 때만 다시 쓴다(남은 시간은 받은 쪽이 센다)
// 이벤트: 틱 끝에 속성 현재값·명시 태그 수를 지난번 값과 비교해 AttributeChanged/TagChanged (클라이언트는 보기 기준 — 예측 포함).
//   원격 클라이언트(조종하지 않는 쪽)의 AbilityActivated/Ended는 RepAbilities의 누적 수·발동 중 변화로 낸다.
#include "Scene/Ability/AbilitySystem.h"

#include "Core/Console/Console.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Ability/AbilityLibrary.h"
#include "Scene/Components.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace
{
	FAbilitySystem* GActiveSystem = nullptr;

	constexpr float TimeEpsilon        = 1.0e-4f;
	constexpr float PredictionTimeout  = 3.0f;
	constexpr uint32 PredictedHandleBit = 0x80000000u;

	TAutoConsoleVariable<int32> CVarAbilityDebug("ability.Debug", 0, "능력 시스템 로그: 1 = 발동/종료/실패/거절/효과를 Display 로그로 (0 = Verbose)");
	TAutoConsoleVariable<bool>  CVarClientSkipChecks("ability.ClientSkipChecks", false,
	                                                 "검증용: 소유 클라이언트가 쿨다운·비용 검사 없이 예측 발동 → 서버 거절·되돌림 확인", EConsoleFlags::Cheat);

	TAutoConsoleVariable<int32> CVarAbilityAutoCast("ability.AutoCast", 0, "검증용: 1이면 능력 데모 스크립트(Tests/Abilities HUD)가 로컬 폰의 능력을 순서대로 자동 발동");

	FAutoConsoleCommand CmdAbilityDump("ability.Dump", "능력 시스템 상태 출력 (속성·태그·활성 효과·능력). 인자: 엔티티 이름 일부 (없으면 전부)",
		[](const std::vector<std::string>& Args, const FConsoleOutput& Output) {
			FAbilitySystem* System = FAbilitySystem::GetActive();
			if (System == nullptr || !System->IsPlaying())
			{
				Output.Print("능력 시스템이 돌고 있지 않습니다 (플레이 중에만)");
				return;
			}
			int32 Count = 0;
			for (const FEntity Entity : System->GetEntities())
			{
				const std::string Text = System->Describe(Entity);
				if (!Args.empty() && Text.find(Args[0]) == std::string::npos)
				{
					continue;
				}
				size_t Start = 0;
				while (Start < Text.size())
				{
					const size_t End = Text.find('\n', Start);
					Output.Print(Text.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
					Start = End == std::string::npos ? Text.size() : End + 1;
				}
				++Count;
			}
			Output.Printf("능력 시스템 {}개 ({})", Count, System->IsAuthority() ? "권한" : "클라이언트");
		});

	void DebugLog(const std::string& Text)
	{
		if (CVarAbilityDebug.Get() > 0)
		{
			E_LOG(LogAbility, Display, "{}", Text);
		}
		else
		{
			E_LOG(LogAbility, Verbose, "{}", Text);
		}
	}

	std::string FormatFloat(float Value)
	{
		return std::format("{:.6g}", Value);
	}

	bool ParseFloat(std::string_view Text, float& Out)
	{
		const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
		return Result.ec == std::errc() && std::isfinite(Out);
	}

	bool ParseUInt(std::string_view Text, uint32& Out)
	{
		const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
		return Result.ec == std::errc();
	}

	std::vector<std::string_view> Split(std::string_view Text, char Separator)
	{
		std::vector<std::string_view> Parts;
		size_t                        Start = 0;
		for (size_t Index = 0; Index <= Text.size(); ++Index)
		{
			if (Index == Text.size() || Text[Index] == Separator)
			{
				if (Index > Start)
				{
					Parts.push_back(Text.substr(Start, Index - Start));
				}
				Start = Index + 1;
			}
		}
		return Parts;
	}

	// ";" "," 구분 이름 목록 (효과/능력 이름 — 공백 제거)
	std::vector<std::string> SplitNames(std::string_view Text)
	{
		std::vector<std::string> Result;
		std::string              Current;
		for (size_t Index = 0; Index <= Text.size(); ++Index)
		{
			const char Char = Index < Text.size() ? Text[Index] : ';';
			if (Char == ';' || Char == ',' || Char == '\n')
			{
				if (!Current.empty())
				{
					Result.push_back(Current);
				}
				Current.clear();
			}
			else if (Char != ' ' && Char != '\t' && Char != '\r')
			{
				Current += Char;
			}
		}
		return Result;
	}

	std::string DescribeEntity(const FScene& Scene, FEntity Entity)
	{
		const FNameComponent* Name = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FNameComponent>(Entity) : nullptr;
		return Name != nullptr ? Name->Name : std::format("#{}", Entity.Index);
	}

	// 즉시 실행의 수정자를 속성별로 묶는다 (표 순서 유지)
	std::vector<std::pair<std::string, std::vector<FAppliedModifier>>> GroupModifiers(const FGameplayEffectDef& Def, const std::vector<float>& Magnitudes,
	                                                                                   int32 Stacks)
	{
		std::vector<std::pair<std::string, std::vector<FAppliedModifier>>> Groups;
		for (size_t Index = 0; Index < Def.Modifiers.size(); ++Index)
		{
			const FModifierDef& Modifier = Def.Modifiers[Index];
			auto Found = std::find_if(Groups.begin(), Groups.end(), [&](const auto& Group) { return Group.first == Modifier.Attribute; });
			if (Found == Groups.end())
			{
				Groups.emplace_back(Modifier.Attribute, std::vector<FAppliedModifier>());
				Found = Groups.end() - 1;
			}
			Found->second.push_back({ Modifier.Op, Index < Magnitudes.size() ? Magnitudes[Index] : 0.0f, Stacks, static_cast<uint32>(Index) });
		}
		return Groups;
	}

	bool IsMetaAttribute(const FAbilitySet* Set, std::string_view Name)
	{
		if (Name == AbilityAttributes::IncomingDamage || Name == AbilityAttributes::IncomingHeal)
		{
			return true;
		}
		const FAttributeDef* Def = Set != nullptr ? Set->FindAttribute(Name) : nullptr;
		return Def != nullptr && Def->bMeta;
	}

	// 태그 T가 컨테이너의 명시 태그 중 하나에 맞나 (면역/능력 차단 — 컨테이너 쪽이 질의)
	bool MatchesAnyExplicit(const std::vector<std::string>& Tags, const FGameplayTagCountContainer& Queries)
	{
		for (const std::string& Tag : Tags)
		{
			for (const auto& [Query, Count] : Queries.GetExplicitTags())
			{
				if (Count > 0 && FGameplayTags::Matches(Tag, Query))
				{
					return true;
				}
			}
		}
		return false;
	}
} // namespace

// ---------------------------------------------------------------- C++ 능력 등록부

FAbilityNativeRegistry& FAbilityNativeRegistry::Get()
{
	static FAbilityNativeRegistry Instance;
	return Instance;
}

void FAbilityNativeRegistry::Register(const std::string& Name, FNativeAbility Ability, std::string Owner)
{
	if (Owner.empty())
	{
		Owner = FTypeRegistry::Get().GetRegistrationOwner();
	}
	Entries[Name] = { std::move(Ability), std::move(Owner) };
}

const FNativeAbility* FAbilityNativeRegistry::Find(std::string_view Name) const
{
	const auto Found = Entries.find(std::string(Name));
	return Found != Entries.end() ? &Found->second.Ability : nullptr;
}

size_t FAbilityNativeRegistry::RemoveByOwner(const std::string& Owner)
{
	return std::erase_if(Entries, [&](const auto& Item) { return Item.second.Owner == Owner; });
}

// ---------------------------------------------------------------- 수명

FAbilitySystem::FAbilitySystem() = default;

FAbilitySystem::~FAbilitySystem()
{
	if (GActiveSystem == this)
	{
		GActiveSystem = nullptr;
	}
}

FAbilitySystem* FAbilitySystem::GetActive()
{
	return GActiveSystem;
}

bool FAbilitySystem::IsAutoCastEnabled()
{
	return CVarAbilityAutoCast.Get() != 0;
}

void FAbilitySystem::Begin(FScene& InScene, bool bInAuthority)
{
	End();
	Scene         = &InScene;
	bAuthority    = bInAuthority;
	GActiveSystem = this;
	Events.clear();
	RejectedCount   = 0;
	RolledBackCount = 0;
	Clock           = 0.0;
	FGameplayTagRegistry::Get().LoadFromProjectSettings();
	// 이전 플레이의 런타임이 남아 있으면(같은 씬으로 다시 시작) 처음부터
	InScene.GetRegistry().View<FAbilitySystemComponent>().Each([](FEntity, FAbilitySystemComponent& Component) {
		const bool bCode = Component.Runtime.bCodeDefinitions;
		auto       Set   = Component.Runtime.Set;
		Component.Runtime = FAbilitySystemRuntime();
		if (bCode)
		{
			Component.Runtime.bCodeDefinitions = true;
			Component.Runtime.Set              = std::move(Set);
		}
	});
}

void FAbilitySystem::End()
{
	if (Scene == nullptr)
	{
		return;
	}
	std::vector<uint32> Ids;
	for (const auto& [Id, Owner] : InstanceOwners)
	{
		Ids.push_back(Id);
	}
	std::sort(Ids.begin(), Ids.end());
	for (const uint32 Id : Ids)
	{
		EndAbility(Id, true);
	}
	InstanceOwners.clear();
	NativeRunning.clear();
	Events.clear();
	Scene = nullptr;
}

std::vector<FAbilityEvent> FAbilitySystem::ConsumeEvents()
{
	std::vector<FAbilityEvent> Result;
	Result.swap(Events);
	return Result;
}

FAbilitySystemComponent* FAbilitySystem::Find(FEntity Entity)
{
	return Scene != nullptr && Scene->GetRegistry().IsValid(Entity) ? Scene->GetRegistry().TryGet<FAbilitySystemComponent>(Entity) : nullptr;
}

const FAbilitySystemComponent* FAbilitySystem::Find(FEntity Entity) const
{
	return Scene != nullptr && Scene->GetRegistry().IsValid(Entity) ? Scene->GetRegistry().TryGet<FAbilitySystemComponent>(Entity) : nullptr;
}

FAbilitySystemComponent* FAbilitySystem::Require(FEntity Entity)
{
	FAbilitySystemComponent* Component = Find(Entity);
	if (Component != nullptr && !Component->Runtime.bInitialized)
	{
		InitializeComponent(Entity, *Component);
	}
	return Component;
}

bool FAbilitySystem::EnsureInitialized(FEntity Entity)
{
	return Require(Entity) != nullptr;
}

bool FAbilitySystem::InitializeWithSet(FEntity Entity, std::shared_ptr<const FAbilitySet> Set)
{
	if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity) || Set == nullptr)
	{
		return false;
	}
	FAbilitySystemComponent& Component  = Scene->GetRegistry().GetOrEmplace<FAbilitySystemComponent>(Entity);
	Component.Runtime                   = FAbilitySystemRuntime();
	Component.Runtime.bCodeDefinitions  = true;
	Component.Runtime.Set               = std::move(Set);
	InitializeComponent(Entity, Component);
	return true;
}

std::vector<FEntity> FAbilitySystem::GetEntities() const
{
	std::vector<FEntity> Result;
	if (Scene != nullptr)
	{
		Scene->GetRegistry().View<FAbilitySystemComponent>().Each([&](FEntity Entity, FAbilitySystemComponent&) { Result.push_back(Entity); });
	}
	std::sort(Result.begin(), Result.end(), [](FEntity A, FEntity B) { return A.Index < B.Index; });
	return Result;
}

bool FAbilitySystem::IsLocallyControlled(FEntity Entity) const
{
	return NetHooks.IsLocallyControlled ? NetHooks.IsLocallyControlled(Entity) : bAuthority;
}

void FAbilitySystem::InitializeComponent(FEntity Entity, FAbilitySystemComponent& Component)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (!Runtime.bCodeDefinitions)
	{
		Runtime.DefinitionKey        = FAbilityLibrary::MakeKey(Component.AttributeTable, Component.EffectTable, Component.AbilityTable);
		Runtime.DefinitionGeneration = FAbilityLibrary::Get().GetGeneration();
		Runtime.Set                  = FAbilityLibrary::Get().Load(Component.AttributeTable, Component.EffectTable, Component.AbilityTable);
	}
	if (Runtime.Set == nullptr)
	{
		Runtime.Set = std::make_shared<FAbilitySet>(); // 표 없음 — 태그/능력 없는 빈 시스템 (경고는 라이브러리가)
	}
	Runtime.bInitialized = true;
	const FAbilitySet& Set = *Runtime.Set;

	Runtime.Attributes.clear();
	for (const FAttributeDef& Def : Set.Attributes)
	{
		Runtime.Attributes[Def.Name] = { Def.Base, Def.Base };
	}
	if (const FHealthComponent* Health = Scene->GetRegistry().TryGet<FHealthComponent>(Entity))
	{
		if (auto Found = Runtime.Attributes.find(AbilityAttributes::Health); Found != Runtime.Attributes.end())
		{
			Found->second = { Health->Health, Health->Health };
		}
		if (auto Found = Runtime.Attributes.find(AbilityAttributes::MaxHealth); Found != Runtime.Attributes.end())
		{
			Found->second = { Health->MaxHealth, Health->MaxHealth };
		}
	}

	Runtime.Granted.clear();
	const std::vector<std::string> Requested = SplitNames(Component.GrantedAbilities);
	for (const std::string& Name : Requested.empty() ? Set.AbilityOrder : Requested)
	{
		if (Set.FindAbility(Name) != nullptr)
		{
			Runtime.Granted.push_back(Name);
		}
		else
		{
			E_LOG(LogAbility, Warning, "[능력] {}: 부여할 능력 '{}'이 능력 표에 없습니다", DescribeEntity(*Scene, Entity), Name);
		}
	}

	if (bAuthority)
	{
		RecomputeAttributes(Entity, Component);
		const std::shared_ptr<const FAbilitySet> SetRef = Runtime.Set;
		for (const std::string& EffectName : SplitNames(Component.StartupEffects))
		{
			const FGameplayEffectDef* Def = SetRef->FindEffect(EffectName);
			if (Def == nullptr)
			{
				E_LOG(LogAbility, Warning, "[능력] {}: 시작 효과 '{}'가 효과 표에 없습니다", DescribeEntity(*Scene, Entity), EffectName);
				continue;
			}
			FEffectApplyParams Params;
			Params.Source = Entity;
			ApplyEffectInternal(Entity, *Def, SetRef, Params, 0);
		}
		if (FAbilitySystemComponent* Current = Find(Entity))
		{
			RecomputeAttributes(Entity, *Current);
			WriteReplicatedState(*Current);
		}
	}
	else
	{
		Runtime.RepAttributes = Runtime.Attributes; // 복제가 오기 전 보기 (정의 기본값)
		ReadReplicatedState(Entity, Component);
		RebuildClientView(Entity, Component);
	}
}

void FAbilitySystem::RebindDefinitions(FEntity Entity, FAbilitySystemComponent& Component, std::shared_ptr<const FAbilitySet> Set)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	Runtime.Set                    = Set != nullptr ? Set : std::make_shared<FAbilitySet>();
	for (const FAttributeDef& Def : Runtime.Set->Attributes)
	{
		if (!Runtime.Attributes.contains(Def.Name))
		{
			Runtime.Attributes[Def.Name] = { Def.Base, Def.Base };
		}
	}
	for (size_t Index = Runtime.Effects.size(); Index-- > 0;)
	{
		FActiveGameplayEffect&    Effect = Runtime.Effects[Index];
		const FGameplayEffectDef* Def    = Runtime.Set->FindEffect(Effect.Name);
		if (Def == nullptr)
		{
			RemoveEffectAt(Entity, Component, Index);
			continue;
		}
		AddEffectTags(Runtime, *Effect.Def, -1);
		Effect.Def = Def;
		Effect.Set = Runtime.Set;
		AddEffectTags(Runtime, *Def, 1);
		Effect.Magnitudes.resize(Def->Modifiers.size(), 0.0f);
	}
	Runtime.Granted.clear();
	const std::vector<std::string> Requested = SplitNames(Component.GrantedAbilities);
	for (const std::string& Name : Requested.empty() ? Runtime.Set->AbilityOrder : Requested)
	{
		if (Runtime.Set->FindAbility(Name) != nullptr)
		{
			Runtime.Granted.push_back(Name);
		}
	}
	E_LOG(LogAbility, Display, "[능력] {}: 정의 다시 읽음 (표 변경)", DescribeEntity(*Scene, Entity));
}

// ---------------------------------------------------------------- 틱

void FAbilitySystem::Tick(float DeltaSeconds)
{
	if (Scene == nullptr)
	{
		return;
	}
	Clock += DeltaSeconds;
	const std::vector<FEntity> Entities = GetEntities();
	const uint32               Generation = FAbilityLibrary::Get().GetGeneration();

	// 1. 초기화 / 정의 다시 묶기
	for (const FEntity Entity : Entities)
	{
		FAbilitySystemComponent* Component = Find(Entity);
		if (Component == nullptr)
		{
			continue;
		}
		if (!Component->Runtime.bInitialized)
		{
			InitializeComponent(Entity, *Component);
			continue;
		}
		if (!Component->Runtime.bCodeDefinitions && Component->Runtime.DefinitionKey != FAbilityLibrary::MakeKey(Component->AttributeTable, Component->EffectTable, Component->AbilityTable))
		{
			Component->Runtime = FAbilitySystemRuntime();
			InitializeComponent(Entity, *Component); // 표 경로가 바뀜 (스크립트가 컴포넌트를 붙인 뒤 채움, 복제로 늦게 옴) → 처음부터
			continue;
		}
		if (!Component->Runtime.bCodeDefinitions && Component->Runtime.DefinitionGeneration != Generation)
		{
			Component->Runtime.DefinitionGeneration = Generation;
			auto Set = FAbilityLibrary::Get().Load(Component->AttributeTable, Component->EffectTable, Component->AbilityTable);
			if (Set != Component->Runtime.Set)
			{
				RebindDefinitions(Entity, *Component, std::move(Set));
			}
		}
	}

	// 2. 소유자가 사라진 인스턴스
	std::vector<uint32> Orphans;
	for (const auto& [Id, Owner] : InstanceOwners)
	{
		if (Find(Owner) == nullptr)
		{
			Orphans.push_back(Id);
		}
	}
	for (const uint32 Id : Orphans)
	{
		EndAbility(Id, true);
	}

	// 3. 효과 시간 / 복제 해석
	for (const FEntity Entity : Entities)
	{
		FAbilitySystemComponent* Component = Find(Entity);
		if (Component == nullptr)
		{
			continue;
		}
		for (FAbilityInstance& Instance : Component->Runtime.Active)
		{
			Instance.Age += DeltaSeconds;
		}
		if (bAuthority)
		{
			SyncFromHealth(Entity, *Component);
			TickEffects(Entity, *Component, DeltaSeconds);
		}
		else
		{
			ReadReplicatedState(Entity, *Component);
			TickPrediction(Entity, *Component, DeltaSeconds);
		}
	}

	// 4. 능력 진행 (Lua 코루틴 → C++ 능력)
	if (ScriptHooks.Tick)
	{
		ScriptHooks.Tick(DeltaSeconds);
	}
	std::vector<uint32> NativeIds;
	for (const auto& [Id, Running] : NativeRunning)
	{
		NativeIds.push_back(Id);
	}
	std::sort(NativeIds.begin(), NativeIds.end());
	for (const uint32 Id : NativeIds)
	{
		const auto Found = NativeRunning.find(Id);
		FEntity    Owner;
		const FAbilityInstance* Instance = FindInstance(Id, &Owner);
		if (Found == NativeRunning.end() || Instance == nullptr)
		{
			continue;
		}
		const FAbilitySystemComponent* Component = Find(Owner);
		const FAbilityDef*             Def       = Component != nullptr ? Component->Runtime.Set->FindAbility(Instance->Ability) : nullptr;
		if (Def == nullptr)
		{
			continue;
		}
		const FNativeAbility  Ability = Found->second.Ability; // 콜백 중 맵이 바뀔 수 있다
		FNativeAbilityContext Context{ *this, Id, Owner, *Def, Instance->bPredicted, bAuthority };
		if (!Ability.OnTick || !Ability.OnTick(Context, DeltaSeconds))
		{
			EndAbility(Id, false);
		}
	}

	// 5. 결과 정리 → 복제 문자열 / 보기 → 이벤트
	for (const FEntity Entity : Entities)
	{
		FAbilitySystemComponent* Component = Find(Entity);
		if (Component == nullptr)
		{
			continue;
		}
		if (bAuthority)
		{
			SyncFromHealth(Entity, *Component);
			RecomputeAttributes(Entity, *Component);
			WriteReplicatedState(*Component);
		}
		else
		{
			RebuildClientView(Entity, *Component);
		}
		DetectChanges(Entity, *Component);
	}
}

void FAbilitySystem::DetectChanges(FEntity Entity, FAbilitySystemComponent& Component)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (!Runtime.bNotifiedSeeded)
	{
		Runtime.bNotifiedSeeded = true;
		for (const auto& [Name, Value] : Runtime.Attributes)
		{
			Runtime.NotifiedAttributes[Name] = Value.Current;
		}
		Runtime.NotifiedTags = Runtime.Tags.GetExplicitTags();
		return;
	}
	for (const auto& [Name, Value] : Runtime.Attributes)
	{
		const auto Found = Runtime.NotifiedAttributes.find(Name);
		if (Found == Runtime.NotifiedAttributes.end() || std::fabs(Found->second - Value.Current) > TimeEpsilon)
		{
			const float Old = Found != Runtime.NotifiedAttributes.end() ? Found->second : Value.Current;
			Runtime.NotifiedAttributes[Name] = Value.Current;
			FAbilityEvent Event;
			Event.Type     = EAbilityEventType::AttributeChanged;
			Event.Entity   = Entity;
			Event.Name     = Name;
			Event.Value    = Value.Current;
			Event.OldValue = Old;
			Events.push_back(std::move(Event));
		}
	}
	const std::map<std::string, int32>& Current = Runtime.Tags.GetExplicitTags();
	std::vector<std::string>            Names;
	for (const auto& [Name, Count] : Current)
	{
		Names.push_back(Name);
	}
	for (const auto& [Name, Count] : Runtime.NotifiedTags)
	{
		if (!Current.contains(Name))
		{
			Names.push_back(Name);
		}
	}
	for (const std::string& Name : Names)
	{
		const auto  NowIt  = Current.find(Name);
		const auto  WasIt  = Runtime.NotifiedTags.find(Name);
		const int32 Now    = NowIt != Current.end() ? NowIt->second : 0;
		const int32 Was    = WasIt != Runtime.NotifiedTags.end() ? WasIt->second : 0;
		if (Now != Was)
		{
			FAbilityEvent Event;
			Event.Type   = EAbilityEventType::TagChanged;
			Event.Entity = Entity;
			Event.Name   = Name;
			Event.Count  = Now;
			Events.push_back(std::move(Event));
		}
	}
	Runtime.NotifiedTags = Current;
}

// ---------------------------------------------------------------- 능력

void FAbilitySystem::PushFailed(FEntity Entity, std::string_view Ability, std::string Reason)
{
	DebugLog(std::format("[능력] {} 발동 실패: {} ({})", Scene != nullptr ? DescribeEntity(*Scene, Entity) : std::string(), Ability, Reason));
	FAbilityEvent Event;
	Event.Type   = EAbilityEventType::AbilityFailed;
	Event.Entity = Entity;
	Event.Name   = std::string(Ability);
	Event.Reason = std::move(Reason);
	Events.push_back(std::move(Event));
	if (FAbilitySystemComponent* Component = Find(Entity))
	{
		Component->Runtime.LastFailedAbility = std::string(Ability);
		Component->Runtime.LastFailedReason  = Events.back().Reason;
		Component->Runtime.LastFailedTime    = Clock;
	}
}

const std::vector<std::string>* FAbilitySystem::GetGrantedAbilities(FEntity Entity) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	return Component != nullptr && Component->Runtime.bInitialized ? &Component->Runtime.Granted : nullptr;
}

bool FAbilitySystem::CheckCost(FEntity Entity, const FAbilitySystemComponent& Component, const FAbilityDef& Def) const
{
	const FAbilitySystemRuntime& Runtime = Component.Runtime;
	const FGameplayEffectDef*    Cost    = Def.CostEffect.empty() ? nullptr : Runtime.Set->FindEffect(Def.CostEffect);
	if (Cost == nullptr)
	{
		return true;
	}
	FEffectApplyParams Params;
	Params.Source = Entity;
	const std::vector<float> Magnitudes = CaptureMagnitudes(Entity, *Cost, Params, Component.Level);
	for (const auto& [Attribute, Modifiers] : GroupModifiers(*Cost, Magnitudes, 1))
	{
		if (IsMetaAttribute(Runtime.Set.get(), Attribute))
		{
			continue;
		}
		const auto Found = Runtime.Attributes.find(Attribute);
		if (Found != Runtime.Attributes.end() && AbilityMath::Aggregate(Found->second.Base, Modifiers) < -TimeEpsilon)
		{
			return false;
		}
	}
	return true;
}

bool FAbilitySystem::CanActivateInternal(FEntity Entity, const FAbilitySystemComponent& Component, const FAbilityDef& Def, std::string& OutReason) const
{
	const FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (std::find(Runtime.Granted.begin(), Runtime.Granted.end(), Def.Name) == Runtime.Granted.end())
	{
		OutReason = "NotGranted";
		return false;
	}
	if (std::any_of(Runtime.Active.begin(), Runtime.Active.end(), [&](const FAbilityInstance& Instance) { return Instance.Ability == Def.Name && !Instance.bEnding; }))
	{
		OutReason = "Active";
		return false;
	}
	if (const FHealthComponent* Health = Scene->GetRegistry().TryGet<FHealthComponent>(Entity); Health != nullptr && Gameplay::IsDead(*Health))
	{
		OutReason = "Dead";
		return false;
	}
	if (!Runtime.Tags.HasAll(Def.ActivationRequiredTags))
	{
		OutReason = "MissingTags";
		return false;
	}
	if (Runtime.Tags.HasAny(Def.ActivationBlockedTags) || MatchesAnyExplicit(Def.AbilityTags, Runtime.BlockedAbilityTags))
	{
		OutReason = "Blocked";
		return false;
	}
	const bool bSkipChecks = !bAuthority && CVarClientSkipChecks.Get();
	if (!bSkipChecks && !Def.CooldownEffect.empty())
	{
		if (const FGameplayEffectDef* Cooldown = Runtime.Set->FindEffect(Def.CooldownEffect))
		{
			const bool bOnCooldown = !Cooldown->GrantedTags.empty() ? Runtime.Tags.HasAny(Cooldown->GrantedTags) : GetCooldownRemaining(Entity, Def.Name) > 0.0f;
			if (bOnCooldown)
			{
				OutReason = "Cooldown";
				return false;
			}
		}
	}
	if (!bSkipChecks && !CheckCost(Entity, Component, Def))
	{
		OutReason = "Cost";
		return false;
	}
	return true;
}

bool FAbilitySystem::CanActivateAbility(FEntity Entity, std::string_view Ability, std::string* OutReason) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	std::string                    Reason    = "NoAbilitySystem";
	bool                           bResult   = false;
	if (Component != nullptr && Component->Runtime.bInitialized)
	{
		const FAbilityDef* Def = Component->Runtime.Set->FindAbility(Ability);
		Reason                 = "NotGranted";
		bResult                = Def != nullptr && CanActivateInternal(Entity, *Component, *Def, Reason);
	}
	if (OutReason != nullptr)
	{
		*OutReason = bResult ? std::string() : Reason;
	}
	return bResult;
}

FAbilityActivateResult FAbilitySystem::TryActivateAbility(FEntity Entity, std::string_view Ability)
{
	FAbilitySystemComponent* Component = Require(Entity);
	if (Component == nullptr)
	{
		return { false, "NoAbilitySystem", 0 };
	}
	const std::shared_ptr<const FAbilitySet> Set = Component->Runtime.Set;
	const FAbilityDef*                       Def = Set->FindAbility(Ability);
	std::string                              Reason;
	if (Def == nullptr)
	{
		PushFailed(Entity, Ability, "NotGranted");
		return { false, "NotGranted", 0 };
	}
	if (!bAuthority && !IsLocallyControlled(Entity))
	{
		PushFailed(Entity, Ability, "NotLocallyControlled");
		return { false, "NotLocallyControlled", 0 };
	}
	if (!CanActivateInternal(Entity, *Component, *Def, Reason))
	{
		PushFailed(Entity, Ability, Reason);
		return { false, Reason, 0 };
	}
	if (bAuthority)
	{
		return ActivateInternal(Entity, *Def, 0, false);
	}

	// 소유 클라이언트: 예측 키 → (예측 능력이면) 로컬 발동 → 서버 요청
	const uint32 Key = ++Component->Runtime.NextPredictionKey;
	Component->Runtime.PendingKeys[Key] = Def->Name;
	FAbilityActivateResult Result{ true, {}, 0 };
	if (Def->bPredicted)
	{
		Result = ActivateInternal(Entity, *Def, Key, true);
	}
	if (NetHooks.SendActivate)
	{
		NetHooks.SendActivate(Entity, Def->Name, Key);
	}
	return Result;
}

FAbilityActivateResult FAbilitySystem::ActivateInternal(FEntity Entity, const FAbilityDef& Def, uint32 PredictionKey, bool bPredicted)
{
	FAbilitySystemComponent* Component = Find(Entity);
	FAbilityInstance         Instance;
	Instance.Id                  = NextInstanceId++;
	Instance.Ability             = Def.Name;
	Instance.PredictionKey       = PredictionKey;
	Instance.bPredicted          = bPredicted;
	Instance.bInPredictionWindow = true;
	const uint32 Id              = Instance.Id;
	Component->Runtime.Active.push_back(std::move(Instance));
	InstanceOwners[Id] = Entity;
	if (bAuthority)
	{
		for (const std::string& Tag : Def.ActivationOwnedTags)
		{
			Component->Runtime.Tags.AddTag(Tag);
		}
		++Component->Runtime.ActivationCounts[Def.Name];
	}
	RebuildBlockedTags(Component->Runtime);
	CancelAbilitiesWithTags(Entity, Def.CancelAbilitiesWithTags, Id);
	if (!bAuthority)
	{
		if (FAbilitySystemComponent* Current = Find(Entity))
		{
			RebuildClientView(Entity, *Current);
		}
	}
	DebugLog(std::format("[능력] {} 발동: {}{}", DescribeEntity(*Scene, Entity), Def.Name, bPredicted ? std::format(" (예측 키 {})", PredictionKey) : std::string()));
	FAbilityEvent Event;
	Event.Type   = EAbilityEventType::AbilityActivated;
	Event.Entity = Entity;
	Event.Name   = Def.Name;
	Events.push_back(Event);

	if (!Def.bManualCommit)
	{
		CommitAbility(Id);
	}
	if (FindInstance(Id) == nullptr)
	{
		return { true, {}, Id }; // 커밋 중 끝남 (CancelAbilitiesWithTags 등)
	}

	const FAbilitySystemComponent* Owner = Find(Entity);
	const float                    Level = Owner != nullptr ? Owner->Level : 1.0f;
	if (!Def.Script.empty() && ScriptHooks.Start)
	{
		if (!ScriptHooks.Start({ Id, Entity, Def.Name, Def.Script, Level, bPredicted, bAuthority }))
		{
			E_LOG(LogAbility, Warning, "[능력] {}: 능력 스크립트를 시작하지 못해 취소합니다 ({})", Def.Name, Def.Script);
			EndAbility(Id, true);
		}
	}
	else if (const FNativeAbility* Native = Def.Native.empty() ? nullptr : FAbilityNativeRegistry::Get().Find(Def.Native))
	{
		NativeRunning[Id]             = { *Native, Entity };
		const FNativeAbility Ability  = *Native;
		FNativeAbilityContext Context{ *this, Id, Entity, Def, bPredicted, bAuthority };
		if (Ability.OnActivate)
		{
			Ability.OnActivate(Context);
		}
		if (!Ability.OnTick && FindInstance(Id) != nullptr)
		{
			EndAbility(Id, false);
		}
	}
	else
	{
		if (!Def.Native.empty())
		{
			E_LOG(LogAbility, Warning, "[능력] {}: 등록되지 않은 C++ 능력 '{}'", Def.Name, Def.Native);
		}
		EndAbility(Id, false); // 스크립트 없는 능력: 커밋(비용·쿨다운·ActivationEffects)만
	}
	EndPredictionWindow(Id);
	return { true, {}, Id };
}

void FAbilitySystem::EndPredictionWindow(uint32 InstanceId)
{
	if (FAbilityInstance* Instance = FindInstanceMutable(InstanceId, nullptr))
	{
		Instance->bInPredictionWindow = false;
	}
}

FAbilityInstance* FAbilitySystem::FindInstanceMutable(uint32 InstanceId, FEntity* OutOwner)
{
	const auto Owner = InstanceOwners.find(InstanceId);
	if (Owner == InstanceOwners.end())
	{
		return nullptr;
	}
	FAbilitySystemComponent* Component = Find(Owner->second);
	if (Component == nullptr)
	{
		return nullptr;
	}
	for (FAbilityInstance& Instance : Component->Runtime.Active)
	{
		if (Instance.Id == InstanceId)
		{
			if (OutOwner != nullptr)
			{
				*OutOwner = Owner->second;
			}
			return &Instance;
		}
	}
	return nullptr;
}

const FAbilityInstance* FAbilitySystem::FindInstance(uint32 InstanceId, FEntity* OutOwner) const
{
	return const_cast<FAbilitySystem*>(this)->FindInstanceMutable(InstanceId, OutOwner);
}

void FAbilitySystem::RebuildBlockedTags(FAbilitySystemRuntime& Runtime) const
{
	Runtime.BlockedAbilityTags.Reset();
	for (const FAbilityInstance& Instance : Runtime.Active)
	{
		const FAbilityDef* Def = Instance.bEnding ? nullptr : Runtime.Set->FindAbility(Instance.Ability);
		if (Def != nullptr)
		{
			for (const std::string& Tag : Def->BlockAbilitiesWithTags)
			{
				Runtime.BlockedAbilityTags.AddTag(Tag);
			}
		}
	}
}

bool FAbilitySystem::CommitAbility(uint32 InstanceId)
{
	FEntity           Owner;
	FAbilityInstance* Instance = FindInstanceMutable(InstanceId, &Owner);
	if (Instance == nullptr)
	{
		return false;
	}
	if (Instance->bCommitted)
	{
		return true;
	}
	Instance->bCommitted = true;
	if (!bAuthority && !(Instance->bPredicted && Instance->bInPredictionWindow))
	{
		return true; // 예측 창 밖 커밋은 서버만 한다
	}
	const FAbilitySystemComponent* Component = Find(Owner);
	const FAbilityDef*             Def       = Component != nullptr ? Component->Runtime.Set->FindAbility(Instance->Ability) : nullptr;
	if (Def == nullptr)
	{
		return false;
	}
	const FAbilityDef DefCopy = *Def; // 적용 중 정의 집합이 바뀌어도 안전
	FEffectApplyParams Params;
	Params.Source = Owner;
	if (!DefCopy.CostEffect.empty())
	{
		ApplyEffectFromAbility(InstanceId, Owner, DefCopy.CostEffect, Params);
	}
	if (!DefCopy.CooldownEffect.empty())
	{
		ApplyEffectFromAbility(InstanceId, Owner, DefCopy.CooldownEffect, Params);
	}
	for (const std::string& Effect : DefCopy.ActivationEffects)
	{
		ApplyEffectFromAbility(InstanceId, Owner, Effect, Params);
	}
	return true;
}

bool FAbilitySystem::EndAbility(uint32 InstanceId, bool bCancelled)
{
	FEntity           Owner;
	FAbilityInstance* Instance = FindInstanceMutable(InstanceId, &Owner);
	if (Instance == nullptr)
	{
		InstanceOwners.erase(InstanceId);
		return false;
	}
	if (Instance->bEnding)
	{
		return false;
	}
	Instance->bEnding         = true;
	const std::string Ability = Instance->Ability;
	const uint32      Key     = Instance->PredictionKey;

	if (ScriptHooks.Stop)
	{
		ScriptHooks.Stop(InstanceId, bCancelled); // 호스트가 모르는 번호면 무시 (스크립트 없는 능력)
	}
	if (const auto Native = NativeRunning.find(InstanceId); Native != NativeRunning.end())
	{
		const FNativeAbility Ability_ = Native->second.Ability;
		NativeRunning.erase(Native);
		const FAbilitySystemComponent* Component = Find(Owner);
		const FAbilityDef*             Def       = Component != nullptr ? Component->Runtime.Set->FindAbility(Ability) : nullptr;
		if (Ability_.OnEnd && Def != nullptr)
		{
			FNativeAbilityContext Context{ *this, InstanceId, Owner, *Def, false, bAuthority };
			Ability_.OnEnd(Context, bCancelled);
		}
	}

	InstanceOwners.erase(InstanceId);
	if (FAbilitySystemComponent* Component = Find(Owner))
	{
		FAbilitySystemRuntime& Runtime = Component->Runtime;
		std::erase_if(Runtime.Active, [&](const FAbilityInstance& Item) { return Item.Id == InstanceId; });
		if (bAuthority)
		{
			if (const FAbilityDef* Def = Runtime.Set->FindAbility(Ability))
			{
				for (const std::string& Tag : Def->ActivationOwnedTags)
				{
					Runtime.Tags.RemoveTag(Tag);
				}
			}
		}
		RebuildBlockedTags(Runtime);
		if (!bAuthority)
		{
			RebuildClientView(Owner, *Component);
		}
	}
	DebugLog(std::format("[능력] {} 종료: {}{}", Scene != nullptr ? DescribeEntity(*Scene, Owner) : std::string(), Ability, bCancelled ? " (취소)" : ""));
	FAbilityEvent Event;
	Event.Type       = EAbilityEventType::AbilityEnded;
	Event.Entity     = Owner;
	Event.Name       = Ability;
	Event.bCancelled = bCancelled;
	Events.push_back(std::move(Event));
	if (bAuthority && Key != 0 && bCancelled && NetHooks.SendResult && Scene != nullptr && Scene->GetRegistry().IsValid(Owner))
	{
		NetHooks.SendResult(Owner, Key, true, "Cancelled");
	}
	return true;
}

bool FAbilitySystem::CancelAbility(FEntity Entity, std::string_view Ability)
{
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr)
	{
		return false;
	}
	for (const FAbilityInstance& Instance : Component->Runtime.Active)
	{
		if (Instance.Ability == Ability && !Instance.bEnding)
		{
			return EndAbility(Instance.Id, true);
		}
	}
	return false;
}

int32 FAbilitySystem::CancelAbilitiesWithTags(FEntity Entity, const std::vector<std::string>& Tags, uint32 ExceptInstance)
{
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr || Tags.empty())
	{
		return 0;
	}
	std::vector<uint32> Targets;
	for (const FAbilityInstance& Instance : Component->Runtime.Active)
	{
		const FAbilityDef* Def = Component->Runtime.Set->FindAbility(Instance.Ability);
		if (Instance.Id == ExceptInstance || Instance.bEnding || Def == nullptr)
		{
			continue;
		}
		const bool bMatch = std::any_of(Def->AbilityTags.begin(), Def->AbilityTags.end(), [&](const std::string& Tag) {
			return std::any_of(Tags.begin(), Tags.end(), [&](const std::string& Query) { return FGameplayTags::Matches(Tag, Query); });
		});
		if (bMatch)
		{
			Targets.push_back(Instance.Id);
		}
	}
	int32 Count = 0;
	for (const uint32 Id : Targets)
	{
		Count += EndAbility(Id, true) ? 1 : 0;
	}
	return Count;
}

bool FAbilitySystem::IsAbilityActive(FEntity Entity, std::string_view Ability) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr)
	{
		return false;
	}
	const FAbilitySystemRuntime& Runtime = Component->Runtime;
	if (std::any_of(Runtime.Active.begin(), Runtime.Active.end(), [&](const FAbilityInstance& Instance) { return Instance.Ability == Ability && !Instance.bEnding; }))
	{
		return true;
	}
	if (!bAuthority)
	{
		const auto Found = Runtime.RepActive.find(std::string(Ability));
		return Found != Runtime.RepActive.end() && Found->second;
	}
	return false;
}

float FAbilitySystem::GetCooldownRemaining(FEntity Entity, std::string_view Ability, float* OutDuration) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	const FAbilityDef*             Def       = Component != nullptr && Component->Runtime.Set ? Component->Runtime.Set->FindAbility(Ability) : nullptr;
	float                          Remaining = 0.0f;
	float                          Duration  = 0.0f;
	if (Def != nullptr && !Def->CooldownEffect.empty())
	{
		for (const FActiveEffectView& Effect : GetActiveEffects(Entity))
		{
			if (Effect.Name == Def->CooldownEffect && Effect.Remaining > Remaining)
			{
				Remaining = Effect.Remaining;
				Duration  = Effect.Duration;
			}
		}
	}
	if (OutDuration != nullptr)
	{
		*OutDuration = Duration;
	}
	return Remaining;
}

bool FAbilitySystem::GiveAbility(FEntity Entity, std::string_view Ability)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr || Component->Runtime.Set->FindAbility(Ability) == nullptr)
	{
		return false;
	}
	std::vector<std::string>& Granted = Component->Runtime.Granted;
	if (std::find(Granted.begin(), Granted.end(), Ability) == Granted.end())
	{
		Granted.emplace_back(Ability);
	}
	return true;
}

bool FAbilitySystem::RemoveAbility(FEntity Entity, std::string_view Ability)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr)
	{
		return false;
	}
	CancelAbility(Entity, Ability);
	if (FAbilitySystemComponent* Current = Find(Entity))
	{
		return std::erase(Current->Runtime.Granted, std::string(Ability)) > 0;
	}
	return false;
}

void FAbilitySystem::HandleInput(FEntity Entity, const std::function<bool(const std::string&)>& WasPressed)
{
	FAbilitySystemComponent* Component = Require(Entity);
	if (Component == nullptr || !WasPressed)
	{
		return;
	}
	const std::vector<std::string> Granted = Component->Runtime.Granted;
	const auto                     Set     = Component->Runtime.Set;
	for (const std::string& Name : Granted)
	{
		const FAbilityDef* Def = Set->FindAbility(Name);
		if (Def != nullptr && !Def->InputAction.empty() && WasPressed(Def->InputAction))
		{
			TryActivateAbility(Entity, Name);
		}
	}
}

// ---------------------------------------------------------------- 네트워크

void FAbilitySystem::ServerHandleActivate(FEntity Entity, const std::string& Ability, uint32 PredictionKey)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr || PredictionKey == 0 || PredictionKey <= Component->Runtime.ServerProcessedKey)
	{
		return; // 중복/오래된 요청
	}
	const std::shared_ptr<const FAbilitySet> Set = Component->Runtime.Set;
	const FAbilityDef*                       Def = Set->FindAbility(Ability);
	std::string                              Reason;
	bool                                     bOk = Def != nullptr && CanActivateInternal(Entity, *Component, *Def, Reason);
	if (Def == nullptr)
	{
		Reason = "NotGranted";
	}
	if (bOk)
	{
		ActivateInternal(Entity, *Def, PredictionKey, false);
	}
	if (FAbilitySystemComponent* Current = Find(Entity))
	{
		Current->Runtime.ServerProcessedKey = PredictionKey;
		WriteReplicatedState(*Current); // 처리한 키 (이번 발동 결과와 함께)
	}
	if (!bOk)
	{
		DebugLog(std::format("[능력] {} 예측 발동 거절: {} 키 {} ({})", DescribeEntity(*Scene, Entity), Ability, PredictionKey, Reason));
		if (NetHooks.SendResult)
		{
			NetHooks.SendResult(Entity, PredictionKey, false, Reason);
		}
	}
}

void FAbilitySystem::ClientHandleResult(FEntity Entity, uint32 PredictionKey, bool bCancelled, const std::string& Reason)
{
	FAbilitySystemComponent* Component = bAuthority ? nullptr : Require(Entity);
	if (Component == nullptr)
	{
		return;
	}
	uint32      LocalInstance = 0;
	std::string Ability;
	for (const FAbilityInstance& Instance : Component->Runtime.Active)
	{
		if (Instance.PredictionKey == PredictionKey && !Instance.bEnding)
		{
			LocalInstance = Instance.Id;
			Ability       = Instance.Ability;
		}
	}
	if (const auto Pending = Component->Runtime.PendingKeys.find(PredictionKey); Pending != Component->Runtime.PendingKeys.end())
	{
		Ability = Pending->second;
	}
	if (bCancelled)
	{
		if (LocalInstance != 0)
		{
			EndAbility(LocalInstance, true);
		}
		return;
	}
	++RejectedCount;
	DropPrediction(*Component, PredictionKey, false);
	E_LOG(LogAbility, Display, "[능력] {} 예측 발동이 서버에서 거절됨: {} 키 {} ({}) — 예측 되돌림", DescribeEntity(*Scene, Entity), Ability, PredictionKey, Reason);
	if (LocalInstance != 0)
	{
		EndAbility(LocalInstance, true);
	}
	if (FAbilitySystemComponent* Current = Find(Entity))
	{
		Current->Runtime.PendingKeys.erase(PredictionKey);
		RebuildClientView(Entity, *Current);
	}
	PushFailed(Entity, Ability, "Rejected:" + Reason);
}

void FAbilitySystem::DropPrediction(FAbilitySystemComponent& Component, uint32 PredictionKey, bool bUpToKey)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	const auto             Matches = [&](uint32 Key) { return Key != 0 && (bUpToKey ? Key <= PredictionKey : Key == PredictionKey); };
	size_t                 Removed = 0;
	Removed += std::erase_if(Runtime.Effects, [&](const FActiveGameplayEffect& Effect) { return Effect.bPredicted && Matches(Effect.PredictionKey); });
	Removed += std::erase_if(Runtime.PredictedDeltas, [&](const FPredictedBaseDelta& Delta) { return Matches(Delta.PredictionKey); });
	if (!bUpToKey)
	{
		RolledBackCount += static_cast<uint32>(Removed);
	}
	else
	{
		std::erase_if(Runtime.PendingKeys, [&](const auto& Item) { return Item.first <= PredictionKey; });
	}
}

// ---------------------------------------------------------------- 효과

std::vector<float> FAbilitySystem::CaptureMagnitudes(FEntity Target, const FGameplayEffectDef& Def, const FEffectApplyParams& Params, float Level) const
{
	const FEntity Source = Params.Source.IsValid() ? Params.Source : Target;
	const auto    Reader = [this](FEntity Entity) {
		return [this, Entity](std::string_view Attribute, float& OutValue) { return GetAttribute(Entity, Attribute, OutValue); };
	};
	const AbilityMath::FAttributeReader SourceReader = Reader(Source);
	const AbilityMath::FAttributeReader TargetReader = Reader(Target);
	std::vector<float>                  Result;
	Result.reserve(Def.Modifiers.size());
	for (const FModifierDef& Modifier : Def.Modifiers)
	{
		bool bMissing = false;
		Result.push_back(AbilityMath::EvaluateMagnitude(Modifier.Magnitude, Level, SourceReader, TargetReader, Params.SetByCaller, &bMissing));
		if (bMissing)
		{
			E_LOG(LogAbility, Warning, "[능력] 효과 '{}': 크기 값을 찾지 못해 0으로 ({} {})", Def.Name, AbilityMath::ToString(Modifier.Magnitude.Type),
			      Modifier.Magnitude.Type == EMagnitudeType::SetByCaller ? Modifier.Magnitude.SetByCallerName : Modifier.Magnitude.Attribute);
		}
	}
	return Result;
}

FEffectApplyResult FAbilitySystem::ApplyEffect(FEntity Target, std::string_view Effect, const FEffectApplyParams& Params)
{
	if (!bAuthority)
	{
		return { false, 0, "NotAuthority" };
	}
	// 정의는 원천의 집합에서 먼저, 없으면 대상의 집합에서 찾는다
	std::shared_ptr<const FAbilitySet> Set;
	const FGameplayEffectDef*          Def = nullptr;
	for (const FEntity Holder : { Params.Source, Target })
	{
		if (const FAbilitySystemComponent* Component = Holder.IsValid() ? Require(Holder) : nullptr; Component != nullptr && Def == nullptr)
		{
			Def = Component->Runtime.Set->FindEffect(Effect);
			Set = Component->Runtime.Set;
		}
	}
	if (Find(Target) == nullptr)
	{
		return { false, 0, "NoAbilitySystem" };
	}
	if (Def == nullptr)
	{
		E_LOG(LogAbility, Warning, "[능력] 없는 효과 '{}' (대상 {})", Effect, DescribeEntity(*Scene, Target));
		return { false, 0, "UnknownEffect" };
	}
	return ApplyEffectInternal(Target, *Def, Set, Params, 0);
}

FEffectApplyResult FAbilitySystem::ApplyEffectFromAbility(uint32 InstanceId, FEntity Target, std::string_view Effect, const FEffectApplyParams& Params)
{
	FEntity           Owner;
	FAbilityInstance* Instance = FindInstanceMutable(InstanceId, &Owner);
	if (Instance == nullptr)
	{
		return { false, 0, "NotActive" };
	}
	FEffectApplyParams Resolved = Params;
	if (!Resolved.Source.IsValid())
	{
		Resolved.Source = Owner;
	}
	FAbilitySystemComponent* OwnerComponent = Find(Owner);
	const std::shared_ptr<const FAbilitySet> Set = OwnerComponent->Runtime.Set;
	const FGameplayEffectDef*                Def = Set->FindEffect(Effect);
	if (Def == nullptr)
	{
		E_LOG(LogAbility, Warning, "[능력] 능력 '{}': 없는 효과 '{}'", Instance->Ability, Effect);
		return { false, 0, "UnknownEffect" };
	}
	if (bAuthority)
	{
		const uint32 Key = Instance->bInPredictionWindow ? Instance->PredictionKey : 0;
		if (Find(Target) == nullptr)
		{
			return { false, 0, "NoAbilitySystem" };
		}
		return ApplyEffectInternal(Target, *Def, Set, Resolved, Key);
	}
	if (Instance->bPredicted && Instance->bInPredictionWindow && Target == Owner)
	{
		return ApplyPredictedEffect(Owner, *OwnerComponent, *Def, Instance->PredictionKey, Resolved);
	}
	return { false, 0, "NotAuthority" };
}

FEffectApplyResult FAbilitySystem::ApplyEffectInternal(FEntity Target, const FGameplayEffectDef& Def, const std::shared_ptr<const FAbilitySet>& Set,
                                                       const FEffectApplyParams& Params, uint32 PredictionKey)
{
	FAbilitySystemComponent* Component = Require(Target);
	if (Component == nullptr)
	{
		return { false, 0, "NoAbilitySystem" };
	}
	FAbilitySystemRuntime& Runtime = Component->Runtime;
	if (MatchesAnyExplicit(Def.AssetTags, Runtime.ImmunityTags))
	{
		DebugLog(std::format("[능력] {}: 효과 {} 면역", DescribeEntity(*Scene, Target), Def.Name));
		return { false, 0, "Immune" };
	}
	if (!Runtime.Tags.HasAll(Def.ApplicationRequiredTags))
	{
		return { false, 0, "MissingTags" };
	}
	if (Runtime.Tags.HasAny(Def.ApplicationBlockedTags))
	{
		return { false, 0, "Blocked" };
	}
	float Level = Params.Level;
	if (Level < 0.0f)
	{
		const FAbilitySystemComponent* SourceComponent = Params.Source.IsValid() ? Find(Params.Source) : nullptr;
		Level = SourceComponent != nullptr ? SourceComponent->Level : Component->Level;
	}
	if (!Def.RemoveEffectsWithTags.empty())
	{
		RemoveEffectsWithTags(Target, Def.RemoveEffectsWithTags);
	}
	const std::vector<float> Magnitudes = CaptureMagnitudes(Target, Def, Params, Level);
	DebugLog(std::format("[능력] {} ← 효과 {}{}", DescribeEntity(*Scene, Target), Def.Name, PredictionKey != 0 ? std::format(" (예측 키 {})", PredictionKey) : std::string()));

	if (Def.IsInstant())
	{
		ExecuteModifiers(Target, Def, Magnitudes, 1, Params.Source);
		return { true, 0, {} };
	}

	FAbilitySystemComponent* Current = Find(Target);
	if (Current == nullptr)
	{
		return { false, 0, "NoAbilitySystem" };
	}
	FAbilitySystemRuntime& Live = Current->Runtime;
	if (Def.Stacking != EEffectStacking::None)
	{
		for (FActiveGameplayEffect& Existing : Live.Effects)
		{
			if (Existing.Name == Def.Name && !Existing.bPredicted && (Def.Stacking == EEffectStacking::ByTarget || Existing.Source == Params.Source))
			{
				Existing.Stacks = std::min(Def.StackLimit, Existing.Stacks + 1);
				if (Def.bRefreshDurationOnStack)
				{
					Existing.Remaining = Existing.Duration;
				}
				Existing.PredictionKey = PredictionKey != 0 ? PredictionKey : Existing.PredictionKey;
				++Existing.RefreshCount;
				const uint32 Handle = Existing.Handle;
				RecomputeAttributes(Target, *Current);
				return { true, Handle, {} };
			}
		}
	}
	FActiveGameplayEffect Effect;
	Effect.Handle          = Live.NextEffectHandle++;
	Effect.Def             = &Def;
	Effect.Set             = Set;
	Effect.Name            = Def.Name;
	Effect.Level           = Level;
	Effect.Duration        = Def.DurationPolicy == EEffectDurationPolicy::HasDuration ? Def.Duration : 0.0f;
	Effect.Remaining       = Effect.Duration;
	Effect.PeriodRemaining = Def.Period;
	Effect.Source          = Params.Source;
	Effect.Magnitudes      = Magnitudes;
	Effect.SetByCaller     = Params.SetByCaller;
	Effect.PredictionKey   = PredictionKey;
	const uint32 Handle    = Effect.Handle;
	Live.Effects.push_back(std::move(Effect));
	AddEffectTags(Live, Def, 1);
	if (Def.IsPeriodic() && Def.bExecutePeriodicOnApply)
	{
		ExecuteModifiers(Target, Def, Magnitudes, 1, Params.Source);
	}
	if (FAbilitySystemComponent* After = Find(Target))
	{
		RecomputeAttributes(Target, *After);
	}
	return { true, Handle, {} };
}

void FAbilitySystem::AddEffectTags(FAbilitySystemRuntime& Runtime, const FGameplayEffectDef& Def, int32 Sign)
{
	for (const std::string& Tag : Def.GrantedTags)
	{
		Runtime.Tags.AddTag(Tag, Sign);
	}
	for (const std::string& Tag : Def.GrantedImmunityTags)
	{
		Runtime.ImmunityTags.AddTag(Tag, Sign);
	}
}

void FAbilitySystem::ExecuteModifiers(FEntity Target, const FGameplayEffectDef& Def, const std::vector<float>& Magnitudes, int32 Stacks, FEntity Source)
{
	for (const auto& [Attribute, Modifiers] : GroupModifiers(Def, Magnitudes, Stacks))
	{
		FAbilitySystemComponent* Component = Find(Target);
		if (Component == nullptr)
		{
			return;
		}
		FAbilitySystemRuntime& Runtime = Component->Runtime;
		if (IsMetaAttribute(Runtime.Set.get(), Attribute))
		{
			ExecuteMeta(Target, *Component, Attribute, AbilityMath::Aggregate(0.0f, Modifiers), Source);
			continue;
		}
		const auto Found = Runtime.Attributes.find(Attribute);
		if (Found == Runtime.Attributes.end())
		{
			continue; // 대상에 없는 속성 (정의 경고는 읽을 때)
		}
		ChangeBase(Target, *Component, Attribute, AbilityMath::Aggregate(Found->second.Base, Modifiers), Source);
	}
	if (FAbilitySystemComponent* Component = Find(Target))
	{
		RecomputeAttributes(Target, *Component);
	}
}

void FAbilitySystem::ExecuteMeta(FEntity Target, FAbilitySystemComponent& Component, const std::string& Attribute, float Amount, FEntity Source)
{
	if (Amount <= 0.0f)
	{
		return;
	}
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	FHealthComponent*      Health  = Scene->GetRegistry().TryGet<FHealthComponent>(Target);
	if (Attribute == AbilityAttributes::IncomingHeal)
	{
		if (Health != nullptr)
		{
			Gameplay::Heal(*Scene, Target, Amount);
		}
		else if (const auto Found = Runtime.Attributes.find(AbilityAttributes::Health); Found != Runtime.Attributes.end())
		{
			ChangeBase(Target, Component, AbilityAttributes::Health, Found->second.Base + Amount, Source);
		}
		SyncFromHealth(Target, Component);
		return;
	}
	if (Attribute != AbilityAttributes::IncomingDamage)
	{
		return; // 다른 메타 속성은 게임 코드가 OnAttributeChanged 대신 효과 이벤트로 처리 (예약 이름만 엔진이 해석)
	}
	float Scale = 1.0f;
	if (const auto Found = Runtime.Attributes.find(AbilityAttributes::DamageTaken); Found != Runtime.Attributes.end())
	{
		Scale = std::max(0.0f, Found->second.Current);
	}
	const float Damage = Amount * Scale;
	if (Damage <= 0.0f)
	{
		return;
	}
	const FEntity Instigator = Source.IsValid() && Source != Target ? Source : FEntity();
	if (Health != nullptr)
	{
		Gameplay::ApplyDamage(*Scene, Target, Damage, Instigator);
	}
	else if (const auto Found = Runtime.Attributes.find(AbilityAttributes::Health); Found != Runtime.Attributes.end())
	{
		ChangeBase(Target, Component, AbilityAttributes::Health, Found->second.Base - Damage, Source);
	}
	SyncFromHealth(Target, Component);
}

void FAbilitySystem::ChangeBase(FEntity Target, FAbilitySystemComponent& Component, const std::string& Attribute, float NewBase, FEntity Source)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	const auto             Found   = Runtime.Attributes.find(Attribute);
	if (Found == Runtime.Attributes.end())
	{
		return;
	}
	if (Attribute == AbilityAttributes::Health)
	{
		if (FHealthComponent* Health = Scene->GetRegistry().TryGet<FHealthComponent>(Target))
		{
			const float Delta = NewBase - Health->Health;
			if (Delta < 0.0f)
			{
				Gameplay::ApplyDamage(*Scene, Target, -Delta, Source.IsValid() && Source != Target ? Source : FEntity());
			}
			else if (Delta > 0.0f)
			{
				Gameplay::Heal(*Scene, Target, Delta);
			}
			Found->second.Base = Health->Health;
			return;
		}
	}
	Found->second.Base = ClampAttribute(Runtime, Runtime.Set->FindAttribute(Attribute), NewBase);
}

float FAbilitySystem::ClampAttribute(const FAbilitySystemRuntime& Runtime, const FAttributeDef* Def, float Value) const
{
	if (Def == nullptr)
	{
		return Value;
	}
	float Max = Def->Max;
	if (!Def->MaxAttribute.empty())
	{
		if (const auto Found = Runtime.Attributes.find(Def->MaxAttribute); Found != Runtime.Attributes.end())
		{
			Max = std::min(Max, Found->second.Current);
		}
	}
	return std::clamp(Value, std::min(Def->Min, Max), Max);
}

void FAbilitySystem::RecomputeAttributes(FEntity Entity, FAbilitySystemComponent& Component)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	std::vector<FAppliedModifier> Modifiers;
	// 1차: 최대 속성을 따르지 않는 속성 (최대값 원천), 2차: 나머지
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (auto& [Name, Value] : Runtime.Attributes)
		{
			const FAttributeDef* Def = Runtime.Set->FindAttribute(Name);
			if ((Def != nullptr && !Def->MaxAttribute.empty()) != (Pass == 1))
			{
				continue;
			}
			Modifiers.clear();
			for (const FActiveGameplayEffect& Effect : Runtime.Effects)
			{
				for (size_t Index = 0; Effect.Def != nullptr && !Effect.Def->IsPeriodic() && Index < Effect.Def->Modifiers.size(); ++Index) // 주기 효과 수정자는 주기마다 기본값에만
				{
					if (Effect.Def->Modifiers[Index].Attribute == Name)
					{
						Modifiers.push_back({ Effect.Def->Modifiers[Index].Op, Index < Effect.Magnitudes.size() ? Effect.Magnitudes[Index] : 0.0f, Effect.Stacks,
						                      Effect.Handle & ~PredictedHandleBit });
					}
				}
			}
			const bool bHealthDriven = Name == AbilityAttributes::Health && Scene->GetRegistry().Has<FHealthComponent>(Entity);
			if (!bHealthDriven && Def != nullptr)
			{
				Value.Base = ClampAttribute(Runtime, Def, Value.Base);
			}
			Value.Current = ClampAttribute(Runtime, Def, AbilityMath::Aggregate(Value.Base, Modifiers));
		}
	}
	SyncToHealth(Entity, Component);
}

void FAbilitySystem::SyncFromHealth(FEntity Entity, FAbilitySystemComponent& Component)
{
	const FHealthComponent* Health = Scene->GetRegistry().TryGet<FHealthComponent>(Entity);
	if (Health == nullptr)
	{
		return;
	}
	if (const auto Found = Component.Runtime.Attributes.find(AbilityAttributes::Health); Found != Component.Runtime.Attributes.end())
	{
		Found->second.Base = Health->Health;
	}
}

void FAbilitySystem::SyncToHealth(FEntity Entity, FAbilitySystemComponent& Component)
{
	FHealthComponent* Health = Scene->GetRegistry().TryGet<FHealthComponent>(Entity);
	if (Health == nullptr)
	{
		return;
	}
	auto& Attributes = Component.Runtime.Attributes;
	if (const auto Max = Attributes.find(AbilityAttributes::MaxHealth); Max != Attributes.end() && std::fabs(Health->MaxHealth - Max->second.Current) > TimeEpsilon)
	{
		Health->MaxHealth = std::max(1.0f, Max->second.Current);
		Health->Health    = std::min(Health->Health, Health->MaxHealth);
	}
	if (const auto Found = Attributes.find(AbilityAttributes::Health); Found != Attributes.end())
	{
		Found->second.Base    = Health->Health;
		Found->second.Current = Health->Health;
	}
}

void FAbilitySystem::RemoveEffectAt(FEntity Target, FAbilitySystemComponent& Component, size_t Index)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (Index >= Runtime.Effects.size())
	{
		return;
	}
	if (Runtime.Effects[Index].Def != nullptr && !Runtime.Effects[Index].bPredicted)
	{
		AddEffectTags(Runtime, *Runtime.Effects[Index].Def, -1);
	}
	DebugLog(std::format("[능력] {}: 효과 {} 끝", DescribeEntity(*Scene, Target), Runtime.Effects[Index].Name));
	Runtime.Effects.erase(Runtime.Effects.begin() + static_cast<std::ptrdiff_t>(Index));
}

bool FAbilitySystem::RemoveEffect(FEntity Target, uint32 Handle)
{
	FAbilitySystemComponent* Component = bAuthority ? Find(Target) : nullptr;
	if (Component == nullptr)
	{
		return false;
	}
	for (size_t Index = 0; Index < Component->Runtime.Effects.size(); ++Index)
	{
		if (Component->Runtime.Effects[Index].Handle == Handle)
		{
			RemoveEffectAt(Target, *Component, Index);
			RecomputeAttributes(Target, *Component);
			return true;
		}
	}
	return false;
}

int32 FAbilitySystem::RemoveEffectsWithTags(FEntity Target, const std::vector<std::string>& Tags)
{
	FAbilitySystemComponent* Component = bAuthority ? Find(Target) : nullptr;
	if (Component == nullptr || Tags.empty())
	{
		return 0;
	}
	int32 Removed = 0;
	for (size_t Index = Component->Runtime.Effects.size(); Index-- > 0;)
	{
		const FGameplayEffectDef* Def = Component->Runtime.Effects[Index].Def;
		const auto Matches = [&](const std::vector<std::string>& List) {
			return std::any_of(List.begin(), List.end(), [&](const std::string& Tag) {
				return std::any_of(Tags.begin(), Tags.end(), [&](const std::string& Query) { return FGameplayTags::Matches(Tag, Query); });
			});
		};
		if (Def != nullptr && (Matches(Def->AssetTags) || Matches(Def->GrantedTags)))
		{
			RemoveEffectAt(Target, *Component, Index);
			++Removed;
		}
	}
	if (Removed > 0)
	{
		RecomputeAttributes(Target, *Component);
	}
	return Removed;
}

void FAbilitySystem::TickEffects(FEntity Entity, FAbilitySystemComponent& Component, float DeltaSeconds)
{
	// 실행 중 효과 목록이 바뀔 수 있으므로 핸들로 다시 찾는다
	std::vector<uint32> Handles;
	for (const FActiveGameplayEffect& Effect : Component.Runtime.Effects)
	{
		Handles.push_back(Effect.Handle);
	}
	for (const uint32 Handle : Handles)
	{
		const auto Locate = [&]() -> FActiveGameplayEffect* {
			FAbilitySystemComponent* Current = Find(Entity);
			if (Current == nullptr)
			{
				return nullptr;
			}
			for (FActiveGameplayEffect& Effect : Current->Runtime.Effects)
			{
				if (Effect.Handle == Handle)
				{
					return &Effect;
				}
			}
			return nullptr;
		};
		FActiveGameplayEffect* Effect = Locate();
		if (Effect == nullptr || Effect->Def == nullptr)
		{
			continue;
		}
		const float Remaining = Effect->Duration > 0.0f ? Effect->Remaining : 1.0e30f;
		// 주기: 이번 틱에 남은 시간 안에서 돌아온 횟수만큼 (만료와 같은 순간의 마지막 주기도 실행)
		if (Effect->Def->IsPeriodic())
		{
			Effect->PeriodRemaining -= DeltaSeconds;
			int32 Guard = 0;
			while (Effect != nullptr && Effect->PeriodRemaining <= TimeEpsilon && Guard++ < 16)
			{
				// 이번 주기가 돌아온 시점 = 틱 시작 + (dt + PeriodRemaining). 만료(틱 시작 + Remaining) 뒤면 실행하지 않는다
				if (Effect->Duration > 0.0f && Effect->PeriodRemaining + DeltaSeconds > Remaining + TimeEpsilon)
				{
					break;
				}
				Effect->PeriodRemaining += Effect->Def->Period;
				const FGameplayEffectDef* Def        = Effect->Def;
				const std::vector<float>  Magnitudes = Effect->Magnitudes;
				const int32               Stacks     = Effect->Stacks;
				const FEntity             Source     = Effect->Source;
				ExecuteModifiers(Entity, *Def, Magnitudes, Stacks, Source);
				Effect = Locate();
			}
		}
		if (Effect == nullptr || Effect->Duration <= 0.0f)
		{
			continue;
		}
		Effect->Remaining -= DeltaSeconds;
		if (Effect->Remaining > TimeEpsilon)
		{
			continue;
		}
		if (Effect->Def->StackExpiration == EStackExpiration::RemoveOneAndRefresh && Effect->Stacks > 1)
		{
			--Effect->Stacks;
			Effect->Remaining = Effect->Duration;
			++Effect->RefreshCount;
			continue;
		}
		FAbilitySystemComponent* Current = Find(Entity);
		for (size_t Index = 0; Current != nullptr && Index < Current->Runtime.Effects.size(); ++Index)
		{
			if (Current->Runtime.Effects[Index].Handle == Handle)
			{
				RemoveEffectAt(Entity, *Current, Index);
				break;
			}
		}
	}
}

std::vector<FActiveEffectView> FAbilitySystem::GetActiveEffects(FEntity Entity) const
{
	std::vector<FActiveEffectView> Result;
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr)
	{
		return Result;
	}
	const FAbilitySystemRuntime& Runtime = Component->Runtime;
	if (!bAuthority)
	{
		for (const FReplicatedEffectSummary& Effect : Runtime.RepEffects)
		{
			Result.push_back({ Effect.Handle, Effect.Name, Effect.Stacks, Effect.Duration > 0.0f ? Effect.Remaining : -1.0f, Effect.Duration, false });
		}
	}
	for (const FActiveGameplayEffect& Effect : Runtime.Effects)
	{
		Result.push_back({ Effect.Handle, Effect.Name, Effect.Stacks, Effect.Duration > 0.0f ? Effect.Remaining : -1.0f, Effect.Duration, Effect.bPredicted });
	}
	return Result;
}

// ---------------------------------------------------------------- 속성 / 태그

bool FAbilitySystem::GetAttribute(FEntity Entity, std::string_view Attribute, float& OutValue, bool bBase) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr || !Component->Runtime.bInitialized)
	{
		return false;
	}
	const auto Found = Component->Runtime.Attributes.find(std::string(Attribute));
	if (Found == Component->Runtime.Attributes.end())
	{
		return false;
	}
	OutValue = bBase ? Found->second.Base : Found->second.Current;
	return true;
}

bool FAbilitySystem::SetBaseAttribute(FEntity Entity, std::string_view Attribute, float Value)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr || !Component->Runtime.Attributes.contains(std::string(Attribute)) || !std::isfinite(Value))
	{
		return false;
	}
	ChangeBase(Entity, *Component, std::string(Attribute), Value, FEntity());
	if (FAbilitySystemComponent* Current = Find(Entity))
	{
		RecomputeAttributes(Entity, *Current);
	}
	return true;
}

int32 FAbilitySystem::GetTagCount(FEntity Entity, std::string_view Tag) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	return Component != nullptr ? Component->Runtime.Tags.GetCount(Tag) : 0;
}

bool FAbilitySystem::HasTagExact(FEntity Entity, std::string_view Tag) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	return Component != nullptr && Component->Runtime.Tags.HasTagExact(Tag);
}

bool FAbilitySystem::AddLooseTag(FEntity Entity, std::string_view Tag, int32 Count)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr || Count <= 0 || !FGameplayTags::IsValid(Tag))
	{
		return false;
	}
	FGameplayTagRegistry::Get().Validate(Tag, "AddLooseTag");
	Component->Runtime.LooseTags.AddTag(Tag, Count);
	Component->Runtime.Tags.AddTag(Tag, Count);
	return true;
}

bool FAbilitySystem::RemoveLooseTag(FEntity Entity, std::string_view Tag, int32 Count)
{
	FAbilitySystemComponent* Component = bAuthority ? Require(Entity) : nullptr;
	if (Component == nullptr || Count <= 0)
	{
		return false;
	}
	const int32 Have    = Component->Runtime.LooseTags.GetExplicitCount(Tag);
	const int32 Removed = std::min(Have, Count);
	if (Removed <= 0)
	{
		return false;
	}
	Component->Runtime.LooseTags.RemoveTag(Tag, Removed);
	Component->Runtime.Tags.RemoveTag(Tag, Removed);
	return true;
}

bool FAbilitySystem::HasAnimNotify(FEntity Owner, std::string_view Name) const
{
	if (Scene == nullptr || !Scene->GetRegistry().IsValid(Owner))
	{
		return false;
	}
	const FRegistry&     Registry = Scene->GetRegistry();
	std::vector<FEntity> Stack{ Owner };
	while (!Stack.empty())
	{
		const FEntity Entity = Stack.back();
		Stack.pop_back();
		if (const FAnimationComponent* Animation = Registry.TryGet<FAnimationComponent>(Entity))
		{
			for (const FAnimNotifyEvent& Event : Animation->Runtime.PendingNotifies)
			{
				if (Event.Name == Name && (Event.Type == EAnimNotifyEventType::Notify || Event.Type == EAnimNotifyEventType::StateBegin))
				{
					return true;
				}
			}
		}
		for (const FEntity Child : Scene->GetChildren(Entity))
		{
			Stack.push_back(Child);
		}
	}
	return false;
}

// ---------------------------------------------------------------- 복제 (권한 쓰기)

void FAbilitySystem::WriteReplicatedState(FAbilitySystemComponent& Component)
{
	const FAbilitySystemRuntime& Runtime = Component.Runtime;
	std::string                  Attributes;
	for (const auto& [Name, Value] : Runtime.Attributes)
	{
		Attributes += std::format("{}={},{};", Name, FormatFloat(Value.Base), FormatFloat(Value.Current));
	}
	std::string Tags;
	for (const auto& [Name, Count] : Runtime.Tags.GetExplicitTags())
	{
		Tags += std::format("{}={};", Name, Count);
	}
	std::string Structure;
	for (const FActiveGameplayEffect& Effect : Runtime.Effects)
	{
		Structure += std::format("{},{},{},{},{},{};", Effect.Handle, Effect.Name, Effect.Stacks, FormatFloat(Effect.Duration), Effect.PredictionKey, Effect.RefreshCount);
	}
	if (Structure != Runtime.EffectsStructureKey)
	{
		Component.Runtime.EffectsStructureKey = Structure;
		std::string Effects;
		for (const FActiveGameplayEffect& Effect : Runtime.Effects)
		{
			Effects += std::format("{},{},{},{},{},{};", Effect.Handle, Effect.Name, Effect.Stacks, FormatFloat(std::max(0.0f, Effect.Remaining)),
			                       FormatFloat(Effect.Duration), Effect.PredictionKey);
		}
		Component.RepEffects = std::move(Effects);
	}
	std::string Abilities;
	for (const std::string& Name : Runtime.Granted)
	{
		const bool bActive = std::any_of(Runtime.Active.begin(), Runtime.Active.end(), [&](const FAbilityInstance& Instance) { return Instance.Ability == Name; });
		const auto Count   = Runtime.ActivationCounts.find(Name);
		Abilities += std::format("{}={},{};", Name, bActive ? 1 : 0, Count != Runtime.ActivationCounts.end() ? Count->second : 0u);
	}
	if (Component.RepAttributes != Attributes)
	{
		Component.RepAttributes = std::move(Attributes);
	}
	if (Component.RepTags != Tags)
	{
		Component.RepTags = std::move(Tags);
	}
	if (Component.RepAbilities != Abilities)
	{
		Component.RepAbilities = std::move(Abilities);
	}
	Component.RepPredictionKey = static_cast<int32>(Runtime.ServerProcessedKey);
}

// ---------------------------------------------------------------- 클라이언트 (복제 읽기 + 예측)

void FAbilitySystem::ReadReplicatedState(FEntity Entity, FAbilitySystemComponent& Component)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (Component.RepAttributes != Runtime.LastRepAttributes)
	{
		Runtime.LastRepAttributes = Component.RepAttributes;
		for (const std::string_view Item : Split(Component.RepAttributes, ';'))
		{
			const size_t Equal = Item.find('=');
			if (Equal == std::string_view::npos)
			{
				continue;
			}
			const std::vector<std::string_view> Values = Split(Item.substr(Equal + 1), ',');
			FAttributeValue                     Value;
			if (Values.size() == 2 && ParseFloat(Values[0], Value.Base) && ParseFloat(Values[1], Value.Current))
			{
				Runtime.RepAttributes[std::string(Item.substr(0, Equal))] = Value;
			}
		}
	}
	if (Component.RepTags != Runtime.LastRepTags)
	{
		Runtime.LastRepTags = Component.RepTags;
		Runtime.RepTags.Reset();
		for (const std::string_view Item : Split(Component.RepTags, ';'))
		{
			const size_t Equal = Item.find('=');
			uint32       Count = 0;
			if (Equal != std::string_view::npos && ParseUInt(Item.substr(Equal + 1), Count))
			{
				Runtime.RepTags.AddTag(Item.substr(0, Equal), static_cast<int32>(Count));
			}
		}
	}
	if (Component.RepEffects != Runtime.LastRepEffects)
	{
		Runtime.LastRepEffects = Component.RepEffects;
		Runtime.RepEffects.clear();
		for (const std::string_view Item : Split(Component.RepEffects, ';'))
		{
			const std::vector<std::string_view> Fields = Split(Item, ',');
			FReplicatedEffectSummary            Summary;
			uint32                              Stacks = 1;
			if (Fields.size() == 6 && ParseUInt(Fields[0], Summary.Handle) && ParseUInt(Fields[2], Stacks) && ParseFloat(Fields[3], Summary.Remaining) &&
			    ParseFloat(Fields[4], Summary.Duration) && ParseUInt(Fields[5], Summary.PredictionKey))
			{
				Summary.Name   = std::string(Fields[1]);
				Summary.Stacks = static_cast<int32>(Stacks);
				Runtime.RepEffects.push_back(std::move(Summary));
			}
		}
	}
	if (Component.RepAbilities != Runtime.LastRepAbilities)
	{
		Runtime.LastRepAbilities = Component.RepAbilities;
		const bool bLocal = IsLocallyControlled(Entity);
		for (const std::string_view Item : Split(Component.RepAbilities, ';'))
		{
			const size_t Equal = Item.find('=');
			if (Equal == std::string_view::npos)
			{
				continue;
			}
			const std::string                   Name(Item.substr(0, Equal));
			const std::vector<std::string_view> Values = Split(Item.substr(Equal + 1), ',');
			uint32                              Active = 0;
			uint32                              Count  = 0;
			if (Values.size() != 2 || !ParseUInt(Values[0], Active) || !ParseUInt(Values[1], Count))
			{
				continue;
			}
			const bool         bWasActive = Runtime.RepActive[Name];
			const uint32       WasCount   = Runtime.RepActivationCounts[Name];
			const FAbilityDef* Def        = Runtime.Set != nullptr ? Runtime.Set->FindAbility(Name) : nullptr;
			// 조종하지 않는 쪽(원격 캐릭터), 또는 예측하지 않는 능력이면 복제 변화로 이벤트 (예측한 쪽은 로컬 발동에서 이미 냈다)
			const bool bReport = Runtime.bRepCountsSeeded && (!bLocal || (Def != nullptr && !Def->bPredicted));
			if (bReport && Count > WasCount)
			{
				Events.push_back({ EAbilityEventType::AbilityActivated, Entity, Name });
			}
			if (bReport && bWasActive && Active == 0)
			{
				FAbilityEvent Event{ EAbilityEventType::AbilityEnded, Entity, Name };
				Events.push_back(std::move(Event));
			}
			Runtime.RepActive[Name]           = Active != 0;
			Runtime.RepActivationCounts[Name] = Count;
		}
		Runtime.bRepCountsSeeded = true;
	}
	const uint32 Processed = static_cast<uint32>(std::max(0, Component.RepPredictionKey));
	if (Processed > Runtime.RepProcessedKey)
	{
		Runtime.RepProcessedKey = Processed;
		DropPrediction(Component, Processed, true);
	}
}

void FAbilitySystem::TickPrediction(FEntity Entity, FAbilitySystemComponent& Component, float DeltaSeconds)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	for (FReplicatedEffectSummary& Effect : Runtime.RepEffects)
	{
		if (Effect.Duration > 0.0f)
		{
			Effect.Remaining = std::max(0.0f, Effect.Remaining - DeltaSeconds);
		}
	}
	bool bTimedOut = false;
	for (size_t Index = Runtime.Effects.size(); Index-- > 0;)
	{
		FActiveGameplayEffect& Effect = Runtime.Effects[Index];
		Effect.PredictedAge += DeltaSeconds;
		if (Effect.Duration > 0.0f)
		{
			Effect.Remaining -= DeltaSeconds;
		}
		const bool bExpired = Effect.Duration > 0.0f && Effect.Remaining <= TimeEpsilon;
		const bool bTimeout = Effect.PredictedAge > PredictionTimeout && Effect.PredictionKey > Runtime.RepProcessedKey;
		bTimedOut |= bTimeout;
		if (bExpired || bTimeout)
		{
			Runtime.Effects.erase(Runtime.Effects.begin() + static_cast<std::ptrdiff_t>(Index));
		}
	}
	for (FPredictedBaseDelta& Delta : Runtime.PredictedDeltas)
	{
		Delta.Age += DeltaSeconds;
	}
	const size_t Before = Runtime.PredictedDeltas.size();
	std::erase_if(Runtime.PredictedDeltas, [](const FPredictedBaseDelta& Delta) { return Delta.Age > PredictionTimeout; });
	if (bTimedOut || Runtime.PredictedDeltas.size() != Before)
	{
		E_LOG(LogAbility, Warning, "[능력] {}: {}초 넘게 서버가 처리하지 않은 예측을 버렸습니다", DescribeEntity(*Scene, Entity), PredictionTimeout);
	}
}

void FAbilitySystem::RebuildClientView(FEntity Entity, FAbilitySystemComponent& Component)
{
	(void)Entity;
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	std::vector<FAppliedModifier> Modifiers;
	for (const auto& [Name, Rep] : Runtime.RepAttributes)
	{
		FAttributeValue Value = Rep;
		for (const FPredictedBaseDelta& Delta : Runtime.PredictedDeltas)
		{
			if (Delta.Attribute == Name)
			{
				Value.Base += Delta.Delta;
				Value.Current += Delta.Delta;
			}
		}
		Modifiers.clear();
		for (const FActiveGameplayEffect& Effect : Runtime.Effects)
		{
			for (size_t Index = 0; Effect.Def != nullptr && !Effect.Def->IsPeriodic() && Index < Effect.Def->Modifiers.size(); ++Index) // 주기 효과 수정자는 주기마다 기본값에만
			{
				if (Effect.Def->Modifiers[Index].Attribute == Name)
				{
					Modifiers.push_back({ Effect.Def->Modifiers[Index].Op, Index < Effect.Magnitudes.size() ? Effect.Magnitudes[Index] : 0.0f, Effect.Stacks, Effect.Handle });
				}
			}
		}
		if (!Modifiers.empty())
		{
			Value.Current = AbilityMath::Aggregate(Value.Current, Modifiers);
		}
		const FAttributeDef* Def = Runtime.Set != nullptr ? Runtime.Set->FindAttribute(Name) : nullptr;
		if (Def != nullptr && Def->MaxAttribute.empty())
		{
			Value.Base    = std::clamp(Value.Base, std::min(Def->Min, Def->Max), Def->Max);
			Value.Current = std::clamp(Value.Current, std::min(Def->Min, Def->Max), Def->Max);
		}
		Runtime.Attributes[Name] = Value;
	}
	for (auto& [Name, Value] : Runtime.Attributes) // 최대 속성 기준 자르기 (예측 비용이 최대를 넘지 않게)
	{
		const FAttributeDef* Def = Runtime.Set != nullptr ? Runtime.Set->FindAttribute(Name) : nullptr;
		if (Def != nullptr && !Def->MaxAttribute.empty())
		{
			Value.Base    = ClampAttribute(Runtime, Def, Value.Base);
			Value.Current = ClampAttribute(Runtime, Def, Value.Current);
		}
	}

	Runtime.Tags = Runtime.RepTags;
	for (const FActiveGameplayEffect& Effect : Runtime.Effects)
	{
		for (const std::string& Tag : Effect.Def != nullptr ? Effect.Def->GrantedTags : std::vector<std::string>())
		{
			Runtime.Tags.AddTag(Tag);
		}
	}
	for (const FAbilityInstance& Instance : Runtime.Active)
	{
		const FAbilityDef* Def = Runtime.Set != nullptr ? Runtime.Set->FindAbility(Instance.Ability) : nullptr;
		if (Def != nullptr && !Instance.bEnding && Instance.bPredicted && Instance.PredictionKey > Runtime.RepProcessedKey)
		{
			for (const std::string& Tag : Def->ActivationOwnedTags)
			{
				Runtime.Tags.AddTag(Tag);
			}
		}
	}
}

FEffectApplyResult FAbilitySystem::ApplyPredictedEffect(FEntity Owner, FAbilitySystemComponent& Component, const FGameplayEffectDef& Def, uint32 PredictionKey,
                                                        const FEffectApplyParams& Params)
{
	FAbilitySystemRuntime& Runtime = Component.Runtime;
	if (!Runtime.Tags.HasAll(Def.ApplicationRequiredTags))
	{
		return { false, 0, "MissingTags" };
	}
	if (Runtime.Tags.HasAny(Def.ApplicationBlockedTags))
	{
		return { false, 0, "Blocked" };
	}
	const std::vector<float> Magnitudes = CaptureMagnitudes(Owner, Def, Params, Params.Level >= 0.0f ? Params.Level : Component.Level);
	if (Def.IsInstant())
	{
		for (const auto& [Attribute, Modifiers] : GroupModifiers(Def, Magnitudes, 1))
		{
			const auto Found = Runtime.Attributes.find(Attribute);
			if (Found == Runtime.Attributes.end() || IsMetaAttribute(Runtime.Set.get(), Attribute) || Attribute == AbilityAttributes::Health)
			{
				continue; // 체력/메타는 예측하지 않는다
			}
			const float Delta = AbilityMath::Aggregate(Found->second.Base, Modifiers) - Found->second.Base;
			Runtime.PredictedDeltas.push_back({ PredictionKey, Attribute, Delta, 0.0f });
		}
	}
	else
	{
		FActiveGameplayEffect* Existing = nullptr;
		if (Def.Stacking != EEffectStacking::None)
		{
			for (FActiveGameplayEffect& Effect : Runtime.Effects)
			{
				if (Effect.Name == Def.Name)
				{
					Existing = &Effect;
				}
			}
		}
		if (Existing != nullptr)
		{
			Existing->Stacks        = std::min(Def.StackLimit, Existing->Stacks + 1);
			Existing->Remaining     = Def.bRefreshDurationOnStack ? Existing->Duration : Existing->Remaining;
			Existing->PredictionKey = PredictionKey;
		}
		else
		{
			FActiveGameplayEffect Effect;
			Effect.Handle        = PredictedHandleBit | Runtime.NextEffectHandle++;
			Effect.Def           = &Def;
			Effect.Set           = Runtime.Set;
			Effect.Name          = Def.Name;
			Effect.Duration      = Def.DurationPolicy == EEffectDurationPolicy::HasDuration ? Def.Duration : 0.0f;
			Effect.Remaining     = Effect.Duration;
			Effect.Source        = Owner;
			Effect.Magnitudes    = Magnitudes;
			Effect.PredictionKey = PredictionKey;
			Effect.bPredicted    = true;
			Runtime.Effects.push_back(std::move(Effect));
		}
	}
	RebuildClientView(Owner, Component);
	return { true, 0, {} };
}

// ---------------------------------------------------------------- 디버그

std::string FAbilitySystem::Describe(FEntity Entity) const
{
	const FAbilitySystemComponent* Component = Find(Entity);
	if (Component == nullptr || Scene == nullptr)
	{
		return {};
	}
	const FAbilitySystemRuntime& Runtime = Component->Runtime;
	std::string                  Text    = std::format("[{}] 레벨 {}{}\n  속성:", DescribeEntity(*Scene, Entity), FormatFloat(Component->Level),
	                                                   Runtime.bInitialized ? "" : " (초기화 전)");
	for (const auto& [Name, Value] : Runtime.Attributes)
	{
		Text += std::format(" {}={}/{}", Name, FormatFloat(Value.Current), FormatFloat(Value.Base));
	}
	Text += "\n  태그:";
	for (const auto& [Name, Count] : Runtime.Tags.GetExplicitTags())
	{
		Text += Count > 1 ? std::format(" {}×{}", Name, Count) : " " + Name;
	}
	Text += "\n  효과:";
	for (const FActiveEffectView& Effect : GetActiveEffects(Entity))
	{
		Text += std::format(" {}{}({}{})", Effect.Name, Effect.Stacks > 1 ? std::format("×{}", Effect.Stacks) : std::string(),
		                    Effect.Remaining >= 0.0f ? std::format("{:.1f}초", Effect.Remaining) : std::string("무한"), Effect.bPredicted ? ", 예측" : "");
	}
	Text += "\n  능력:";
	for (const std::string& Name : Runtime.Granted)
	{
		float       Duration  = 0.0f;
		const float Cooldown  = GetCooldownRemaining(Entity, Name, &Duration);
		Text += std::format(" {}{}{}", Name, IsAbilityActive(Entity, Name) ? "[발동 중]" : "", Cooldown > 0.0f ? std::format("[쿨다운 {:.1f}초]", Cooldown) : std::string());
	}
	if (!bAuthority)
	{
		Text += std::format("\n  예측: 처리된 키 {}, 예측 효과 {}, 변화량 {}, 거절 {}", Runtime.RepProcessedKey,
		                    std::count_if(Runtime.Effects.begin(), Runtime.Effects.end(), [](const FActiveGameplayEffect& Effect) { return Effect.bPredicted; }),
		                    Runtime.PredictedDeltas.size(), RejectedCount);
	}
	return Text;
}
