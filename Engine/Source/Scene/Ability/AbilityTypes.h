#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogAbility)

class FDataTable;

// 능력 시스템 정의 데이터 (언리얼 GAS식, Phase 53 사이드). 모두 불변 값 — FAbilitySet 하나로 묶여 shared_ptr로 공유된다.
//
// 속성(Attribute): 이름 + 기본값 + 최소/최대(+ 최대를 다른 속성 현재값으로: Health의 MaxAttribute = MaxHealth).
//   값은 기본값(Base: 즉시/주기 효과가 영구로 바꿈)과 현재값(Current: Base에 지속 효과 수정자를 얹은 결과)으로 나뉜다.
// 수정자 계산 순서 (AbilityMath::Aggregate — 지속/무한 효과의 수정자로 현재값, 즉시 효과는 같은 식으로 기본값을 바꾼다):
//   1) Override가 하나라도 있으면 가장 나중에 적용된 효과(큰 핸들)의 값으로 끝 (Add/Multiply 무시)
//   2) 아니면 (Base + ΣAdd) × ΠMultiply  — 곱하기끼리는 곱한다 (방어막 ×0.5 두 개 = ×0.25)
//   3) 최소/최대로 자른다 (최대 = min(Max, MaxAttribute의 현재값))
//   쌓인 효과(스택 n): Add × n, Multiply^n, Override는 그대로
// 크기(Magnitude): 상수 / 곡선(레벨 → 값, 구간 선형) / 속성 비례(Coefficient × (원천 또는 대상 속성 현재값) + PostAdd) /
//   SetByCaller(적용할 때 이름으로 넘긴 값, 없으면 0 + 경고). 효과 적용 순간 한 번 계산해 저장한다(캡처)
// 예약 속성 이름 (있을 때만 의미):
//   Health/MaxHealth — 대상에 FHealthComponent가 있으면 체력 컴포넌트가 진실: Health의 즉시 변화는 Gameplay::ApplyDamage/Heal을
//     거쳐(OnDamaged/OnDeath 이벤트·무적·사망 규칙 그대로) 들어가고, MaxHealth 현재값은 컴포넌트 MaxHealth로 쓴다
//   IncomingDamage (메타) — 즉시 Add 값 × DamageTaken 현재값(없으면 1)을 데미지로 준다(원천 = 가해자). 기본값은 항상 0
//   IncomingHeal (메타)   — 즉시 Add 값만큼 회복. 기본값은 항상 0
//   DamageTaken — 받는 피해 배율 (기본 1, 방어막 = Multiply 0.5)

enum class EModifierOp : uint8
{
	Add,
	Multiply,
	Override,
};

enum class EMagnitudeType : uint8
{
	Constant,
	Curve,          // Curve 점 (X = 레벨, Y = 값) 구간 선형, 범위 밖은 끝 값
	AttributeBased, // Coefficient × 속성 + PostAdd
	SetByCaller,    // 적용 매개변수 이름 값
};

enum class EEffectDurationPolicy : uint8
{
	Instant,     // 기본값을 한 번 바꾼다 (활성 효과로 남지 않음)
	HasDuration, // Duration초 동안 (주기 실행 가능)
	Infinite,    // 지울 때까지
};

// 쌓기: None = 적용마다 별도 인스턴스, ByTarget = 대상에 하나(원천 무관), BySource = 원천마다 하나
enum class EEffectStacking : uint8
{
	None,
	ByTarget,
	BySource,
};

// 스택 만료: 전체 제거 / 하나 빼고 지속 시간 새로
enum class EStackExpiration : uint8
{
	ClearAll,
	RemoveOneAndRefresh,
};

struct FMagnitudeDef
{
	EMagnitudeType        Type  = EMagnitudeType::Constant;
	float                 Value = 0.0f;   // Constant 값 / AttributeBased 계수 (Coefficient)
	std::vector<FVector2> Curve;          // Curve (X 오름차순)
	std::string           Attribute;      // AttributeBased: 읽을 속성
	bool                  bFromSource = true; // AttributeBased: true = 원천(가해자), false = 대상
	float                 PostAdd = 0.0f; // AttributeBased: 더하는 값
	std::string           SetByCallerName;
};

struct FModifierDef
{
	std::string   Attribute;
	EModifierOp   Op = EModifierOp::Add;
	FMagnitudeDef Magnitude;
};

struct FAttributeDef
{
	std::string Name;
	float       Base = 0.0f;
	float       Min  = -1.0e30f;
	float       Max  = 1.0e30f;
	std::string MaxAttribute; // 비어 있지 않으면 최대 = min(Max, 이 속성 현재값)
	bool        bMeta = false; // IncomingDamage/IncomingHeal 이름이면 자동
};

struct FGameplayEffectDef
{
	std::string               Name;
	EEffectDurationPolicy     DurationPolicy = EEffectDurationPolicy::Instant;
	float                     Duration       = 0.0f; // HasDuration (초)
	float                     Period         = 0.0f; // > 0이면 주기 실행 (지속/무한): 수정자를 즉시 효과처럼 Period마다 기본값에 (스택 배)
	bool                      bExecutePeriodicOnApply = false; // 적용 순간에도 한 번
	std::vector<FModifierDef> Modifiers;
	EEffectStacking           Stacking        = EEffectStacking::None;
	int32                     StackLimit      = 1;
	bool                      bRefreshDurationOnStack = true; // 스택이 더해질 때 남은 시간을 Duration으로
	EStackExpiration          StackExpiration = EStackExpiration::ClearAll;

	std::vector<std::string> AssetTags;               // 이 효과를 나타내는 태그 (면역/제거 대상 판정)
	std::vector<std::string> GrantedTags;             // 활성 동안 대상이 가진다 (스택과 무관하게 1)
	std::vector<std::string> ApplicationRequiredTags; // 대상이 모두 가져야 적용
	std::vector<std::string> ApplicationBlockedTags;  // 대상이 하나라도 가지면 적용 안 됨
	std::vector<std::string> RemoveEffectsWithTags;   // 적용할 때 대상의 활성 효과 중 AssetTags/GrantedTags가 맞는 것 제거
	std::vector<std::string> GrantedImmunityTags;     // 활성 동안 AssetTags가 이것에 맞는 효과를 막는다 (면역)

	bool IsInstant() const { return DurationPolicy == EEffectDurationPolicy::Instant; }
	bool IsPeriodic() const { return !IsInstant() && Period > 0.0f; }
};

struct FAbilityDef
{
	std::string Name;
	std::string Script;         // Lua 능력 스크립트 (Content 기준 .lua). 비면 Native, 둘 다 없으면 발동 즉시 끝 (비용·쿨다운·ActivationEffects만)
	std::string Native;         // C++ 능력 이름 (FAbilityNativeRegistry, 게임 모듈)
	std::string CostEffect;     // 즉시 효과 (자기 대상). 발동 검사: 적용하면 어떤 속성 기본값이 0 미만이 되면 실패 "Cost"
	std::string CooldownEffect; // 지속 효과 (자기 대상). 그 GrantedTags를 가지고 있으면 실패 "Cooldown"
	std::vector<std::string> ActivationEffects; // 발동(커밋)할 때 자기에게 적용 (데이터만으로 만드는 능력: 방어막 등)
	std::string InputAction;    // 입력 액션 (Phase 24, Config/Input.json) — 조종하는 쪽이 누르면 발동
	std::vector<std::string> AbilityTags;            // 이 능력을 나타내는 태그 (Block/Cancel 판정)
	std::vector<std::string> ActivationRequiredTags; // 소유자가 모두 가져야
	std::vector<std::string> ActivationBlockedTags;  // 소유자가 하나라도 가지면 실패 "Blocked"
	std::vector<std::string> ActivationOwnedTags;    // 발동 중 소유자가 가진다
	std::vector<std::string> CancelAbilitiesWithTags; // 발동하면 AbilityTags가 맞는 다른 발동 중 능력을 취소
	std::vector<std::string> BlockAbilitiesWithTags;  // 발동 중에는 AbilityTags가 맞는 다른 능력이 발동하지 못한다
	bool bPredicted    = true;  // 소유 클라이언트가 예측 발동 (끄면 서버 확정 후에만 — 로컬 스크립트도 돌지 않는다)
	bool bManualCommit = false; // true면 스크립트가 ctx:CommitAbility()를 불러야 비용·쿨다운·ActivationEffects 적용
};

// 정의 묶음 (속성 + 효과 + 능력). 컴포넌트가 가리키는 표 3개(+ 수정자 표)에서 만든다 — FAbilityLibrary 캐시
struct FAbilitySet
{
	std::vector<FAttributeDef>                         Attributes; // 표 순서
	std::unordered_map<std::string, FGameplayEffectDef> Effects;
	std::unordered_map<std::string, FAbilityDef>        Abilities;
	std::vector<std::string>                            AbilityOrder; // 표 순서 (GrantedAbilities가 비면 전부 이 순서로)

	const FAttributeDef*      FindAttribute(std::string_view Name) const;
	const FGameplayEffectDef* FindEffect(std::string_view Name) const;
	const FAbilityDef*        FindAbility(std::string_view Name) const;

	void AddAttribute(FAttributeDef Def);
	void AddEffect(FGameplayEffectDef Def);
	void AddAbility(FAbilityDef Def);

	// 데이터 테이블에서 (필드 이름으로 읽는다 — 머리 주석의 .estruct 필드 목록은 AbilityTypes.cpp). 없는 표는 nullptr.
	// LoadTable: 효과 Modifiers 필드가 RowRef 배열이면 그 Table(.etable)을 읽는 함수. 경고는 OutWarnings (태그 등록 검사 포함)
	using FTableLoader = std::function<std::shared_ptr<const FDataTable>(const std::string& Path)>;
	static std::shared_ptr<FAbilitySet> FromTables(const FDataTable* AttributeTable, const FDataTable* EffectTable, const FDataTable* AbilityTable,
	                                               const FTableLoader& LoadTable, std::vector<std::string>* OutWarnings = nullptr);
};

// 수정자 하나를 계산에 넣을 형태 (크기는 이미 계산됨)
struct FAppliedModifier
{
	EModifierOp Op       = EModifierOp::Add;
	float       Value    = 0.0f;
	int32       Stacks   = 1;
	uint32      Order    = 0; // Override 우선순위 (큰 값 = 나중)
};

namespace AbilityMath
{
	// 머리 주석의 계산 순서. Min/Max 자르기는 하지 않는다 (ClampAttribute)
	float Aggregate(float Base, const std::vector<FAppliedModifier>& Modifiers);
	float EvaluateCurve(const std::vector<FVector2>& Points, float X);
	// 속성 값 조회 (없으면 nullptr 반환하는 함수). 크기 계산용
	using FAttributeReader = std::function<bool(std::string_view Attribute, float& OutValue)>;
	float EvaluateMagnitude(const FMagnitudeDef& Magnitude, float Level, const FAttributeReader& Source, const FAttributeReader& Target,
	                        const std::map<std::string, float>& SetByCaller, bool* bOutMissing = nullptr);

	const char* ToString(EModifierOp Op);
	const char* ToString(EMagnitudeType Type);
	const char* ToString(EEffectDurationPolicy Policy);
	const char* ToString(EEffectStacking Stacking);
	bool        Parse(std::string_view Text, EModifierOp& Out);
	bool        Parse(std::string_view Text, EMagnitudeType& Out);
	bool        Parse(std::string_view Text, EEffectDurationPolicy& Out);
	bool        Parse(std::string_view Text, EEffectStacking& Out);
	bool        Parse(std::string_view Text, EStackExpiration& Out);

	// 인라인 수정자 글자 "속성 연산 크기" (예: "Mana Add -20", "DamageTaken Multiply 0.5", "IncomingDamage Add SetByCaller:Damage").
	// 효과 Modifiers 필드가 String 배열일 때. 실패하면 false
	bool ParseInlineModifier(std::string_view Text, FModifierDef& Out);
} // namespace AbilityMath

// 예약 속성 이름 (머리 주석)
namespace AbilityAttributes
{
	inline constexpr const char* Health         = "Health";
	inline constexpr const char* MaxHealth      = "MaxHealth";
	inline constexpr const char* IncomingDamage = "IncomingDamage";
	inline constexpr const char* IncomingHeal   = "IncomingHeal";
	inline constexpr const char* DamageTaken    = "DamageTaken";
} // namespace AbilityAttributes
