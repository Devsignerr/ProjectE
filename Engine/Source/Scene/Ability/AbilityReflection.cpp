#include "Scene/Ability/AbilityReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Scene/Ability/AbilityComponent.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/GameModuleHost.h"

void RegisterAbilityTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	// 정의는 데이터 테이블 (Scene/Ability/AbilityTypes.cpp 머리 주석의 필드). Rep*는 서버 상태의 복제 + 인스펙터 디버그 표시 (저장 안 함)
	Registry.RegisterType<FAbilitySystemComponent>("AbilitySystemComponent", "능력 시스템")
		.Property(&FAbilitySystemComponent::AttributeTable, "AttributeTable", "속성 표").AssetFilter(".etable")
		.Tooltip("행 = 속성 (Base/Min/Max/MaxAttribute/Meta)")
		.Property(&FAbilitySystemComponent::EffectTable, "EffectTable", "효과 표").AssetFilter(".etable")
		.Tooltip("행 = 게임플레이 효과 (지속 정책, 수정자, 쌓기, 태그)")
		.Property(&FAbilitySystemComponent::AbilityTable, "AbilityTable", "능력 표").AssetFilter(".etable")
		.Tooltip("행 = 능력 (Lua 스크립트, 비용/쿨다운 효과, 입력 액션, 태그)")
		.Property(&FAbilitySystemComponent::GrantedAbilities, "GrantedAbilities", "부여 능력").Tooltip("\";\" 구분 능력 이름. 비면 능력 표 전부")
		.Property(&FAbilitySystemComponent::StartupEffects, "StartupEffects", "시작 효과").Tooltip("\";\" 구분 효과 이름. 시작할 때 자신에게 (재생 등)")
		.Property(&FAbilitySystemComponent::Level, "Level", "레벨").Range(0.0f, 1000.0f, 1.0f).Tooltip("곡선 크기의 X (레벨)")
		.Property(&FAbilitySystemComponent::bAcceptInput, "AcceptInput", "입력으로 발동").Tooltip("조종하는 쪽의 입력 액션으로 능력을 발동한다 (플레이어 폰만)")
		.Property(&FAbilitySystemComponent::RepAttributes, "RepAttributes", "속성 (상태)", PF_Transient | PF_ReadOnly)
		.Property(&FAbilitySystemComponent::RepTags, "RepTags", "태그 (상태)", PF_Transient | PF_ReadOnly)
		.Property(&FAbilitySystemComponent::RepEffects, "RepEffects", "활성 효과 (상태)", PF_Transient | PF_ReadOnly)
		.Property(&FAbilitySystemComponent::RepAbilities, "RepAbilities", "능력 (상태)", PF_Transient | PF_ReadOnly)
		.Property(&FAbilitySystemComponent::RepPredictionKey, "RepPredictionKey", "처리한 예측 키", PF_Transient | PF_ReadOnly)
		.AsComponent();

	FGameModuleHost::AddUnloadCleanup([](const std::string& Owner) { FAbilityNativeRegistry::Get().RemoveByOwner(Owner); });
}
