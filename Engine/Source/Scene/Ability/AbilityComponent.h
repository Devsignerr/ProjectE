#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Scene/Ability/AbilityTypes.h"
#include "Scene/Ability/GameplayTags.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// 활성 효과 하나 (지속/무한). 권한 쪽 실제 효과, 소유 클라이언트의 예측 효과(bPredicted)
struct FActiveGameplayEffect
{
	uint32                       Handle = 0;
	const FGameplayEffectDef*    Def    = nullptr; // Set이 살아 있는 동안 유효
	std::shared_ptr<const FAbilitySet> Set;
	std::string                  Name;
	float                        Level        = 1.0f;
	int32                        Stacks       = 1;
	float                        Duration     = 0.0f; // 무한이면 0
	float                        Remaining    = 0.0f;
	float                        PeriodRemaining = 0.0f;
	FEntity                      Source;
	std::vector<float>           Magnitudes;   // 수정자별 캡처한 크기 (Def->Modifiers와 같은 길이)
	std::map<std::string, float> SetByCaller;
	uint32                       PredictionKey = 0; // 예측 발동에서 생긴 효과 (서버 사본도 같은 키를 가진다)
	bool                         bPredicted    = false;
	float                        PredictedAge  = 0.0f;
	uint32                       RefreshCount  = 0; // 쌓기/갱신 횟수 (복제 문자열을 다시 쓸지 판단)
};

// 발동 중인 능력 인스턴스 (능력마다 동시에 하나)
struct FAbilityInstance
{
	uint32      Id = 0;          // 시스템 전체 고유 (스크립트 호스트 핸들)
	std::string Ability;
	uint32      PredictionKey = 0;
	bool        bPredicted    = false; // 소유 클라이언트의 예측 실행 (서버 확정 전)
	bool        bCommitted    = false;
	bool        bEnding       = false;
	bool        bInPredictionWindow = false; // OnActivate의 첫 대기 전 (예측 효과 허용 구간)
	float       Age           = 0.0f;
};

// 소유 클라이언트의 예측 기본값 변화 (즉시 효과 — 비용 등)
struct FPredictedBaseDelta
{
	uint32      PredictionKey = 0;
	std::string Attribute;
	float       Delta = 0.0f;
	float       Age   = 0.0f;
};

// 복제 문자열에서 읽은 효과 요약 (클라이언트 표시용)
struct FReplicatedEffectSummary
{
	uint32      Handle = 0;
	std::string Name;
	int32       Stacks    = 1;
	float       Remaining = 0.0f; // 받은 순간 값에서 로컬로 센다
	float       Duration  = 0.0f;
	uint32      PredictionKey = 0;
};

struct FAttributeValue
{
	float Base    = 0.0f;
	float Current = 0.0f;
};

// 비직렬화 런타임 (리플렉션 미등록). 복사(플레이 복제/프리팹)하면 비워진다 → 다음 틱에 다시 초기화
struct FAbilitySystemRuntime
{
	bool                                bInitialized = false;
	bool                                bCodeDefinitions = false; // InitializeWithSet (표 경로 무시, 핫 리로드 없음)
	uint32                              DefinitionGeneration = 0; // FAbilityLibrary 세대 (바뀌면 정의만 다시 묶는다)
	std::string                         DefinitionKey;            // 표 경로 묶음
	std::shared_ptr<const FAbilitySet>  Set;
	std::map<std::string, FAttributeValue> Attributes; // 이름순 (복제 문자열 순서)
	std::vector<FActiveGameplayEffect>  Effects;
	FGameplayTagCountContainer          Tags;          // 권한: 실제 / 클라이언트: 복제 + 예측을 합친 보기
	FGameplayTagCountContainer          LooseTags;     // 스크립트/게임 모듈이 직접 단 태그 (권한)
	FGameplayTagCountContainer          ImmunityTags;  // 활성 효과의 GrantedImmunityTags
	FGameplayTagCountContainer          BlockedAbilityTags; // 발동 중 능력의 BlockAbilitiesWithTags
	std::vector<std::string>            Granted;       // 부여된 능력 (표 순서)
	std::vector<FAbilityInstance>       Active;
	std::map<std::string, uint32>       ActivationCounts; // 능력별 누적 발동 수 (복제 — 원격 클라이언트 OnAbilityActivated)
	uint32                              NextEffectHandle = 1;
	uint32                              ServerProcessedKey = 0; // 서버: 소유 클라이언트가 보낸 예측 키 중 처리한 최대

	// ---- 클라이언트 (복제 해석 + 예측)
	uint32                              NextPredictionKey = 0;
	std::vector<FPredictedBaseDelta>    PredictedDeltas;
	std::map<uint32, std::string>       PendingKeys; // 서버로 보낸 예측 키 → 능력 이름 (거절 이벤트 이름)
	std::map<std::string, FAttributeValue> RepAttributes;
	FGameplayTagCountContainer          RepTags;
	std::vector<FReplicatedEffectSummary> RepEffects;
	std::map<std::string, uint32>       RepActivationCounts;
	std::map<std::string, bool>         RepActive;
	uint32                              RepProcessedKey = 0;
	std::string                         LastRepAttributes, LastRepTags, LastRepEffects, LastRepAbilities; // 마지막으로 해석한 문자열
	bool                                bRepCountsSeeded = false;

	// ---- 권한: 복제 문자열을 다시 쓸지 (효과 구조가 바뀔 때만 남은 시간을 갱신)
	std::string                         EffectsStructureKey;

	// ---- 마지막 발동 실패 (HUD 표시용, Lua entity:GetLastAbilityFailure)
	std::string                         LastFailedAbility, LastFailedReason;
	double                              LastFailedTime = -1.0;

	// ---- 이벤트 감지 (지난번 알린 값)
	std::map<std::string, float>        NotifiedAttributes;
	std::map<std::string, int32>        NotifiedTags;
	bool                                bNotifiedSeeded = false;

	FAbilitySystemRuntime() = default;
	FAbilitySystemRuntime(const FAbilitySystemRuntime&) {}
	FAbilitySystemRuntime& operator=(const FAbilitySystemRuntime&) { return *this; }
	FAbilitySystemRuntime(FAbilitySystemRuntime&&) noexcept            = default; // 풀 재배치는 값을 유지한다
	FAbilitySystemRuntime& operator=(FAbilitySystemRuntime&&) noexcept = default;
};

// 능력 시스템 컴포넌트 (언리얼 AbilitySystemComponent). 정의는 데이터 테이블 3개 (+ 효과 표가 가리키는 수정자 표).
// 상태는 FAbilitySystem(FGameWorld 소유)이 Runtime에 두고, 서버가 Rep* 문자열(PF_Transient — 저장 안 함, 복제됨)로 클라이언트에 알린다.
// 복제 문자열 형식은 Scene/Ability/AbilitySystem.cpp 머리 주석
struct FAbilitySystemComponent
{
	std::string AttributeTable;   // .etable (속성)
	std::string EffectTable;      // .etable (효과)
	std::string AbilityTable;     // .etable (능력)
	std::string GrantedAbilities; // ";" 구분 능력 이름 (비면 능력 표 전부)
	std::string StartupEffects;   // ";" 구분 — 시작할 때 자기에게 적용 (재생 등)
	float       Level        = 1.0f;  // 곡선 크기의 레벨
	bool        bAcceptInput = false; // 조종하는 쪽 입력 액션으로 능력 발동 (플레이어 폰만 켠다)

	// ---- 복제 상태 (서버 → 클라이언트, PF_Transient | PF_ReadOnly — 인스펙터 디버그 표시 겸용)
	std::string RepAttributes;
	std::string RepTags;
	std::string RepEffects;
	std::string RepAbilities;
	int32       RepPredictionKey = 0; // 서버가 처리한 소유 클라이언트 예측 키 (이 키까지의 예측은 위 값에 반영됐거나 거절됨)

	FAbilitySystemRuntime Runtime;
};
