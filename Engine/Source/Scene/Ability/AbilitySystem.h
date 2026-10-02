#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Scene/Ability/AbilityComponent.h"
#include "Scene/Ability/AbilityTypes.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FScene;
class FAbilitySystem;
struct FAbilitySystemComponent;

// 능력 시스템 이벤트 (FGameWorld가 같은 틱에 스크립트 OnAttributeChanged 등 + 게임 모듈 OnAbilityEvent로 전달)
enum class EAbilityEventType : uint8
{
	AttributeChanged, // Name, Value(새 현재값), OldValue
	TagChanged,       // Name(태그), Count(새 명시 수 — 0 = 사라짐). 명시 태그만 (부모 암시 수는 알리지 않음)
	AbilityActivated, // Name(능력)
	AbilityEnded,     // Name, bCancelled
	AbilityFailed,    // Name, Reason ("Cooldown", "Cost", "Blocked", "MissingTags", "Active", "NotGranted", "Dead", "NotLocallyControlled", "Rejected:<서버 이유>")
};

struct FAbilityEvent
{
	EAbilityEventType Type = EAbilityEventType::AttributeChanged;
	FEntity           Entity;
	std::string       Name;
	float             Value    = 0.0f;
	float             OldValue = 0.0f;
	int32             Count    = 0;
	std::string       Reason;
	bool              bCancelled = false;
};

// 효과 적용 매개변수
struct FEffectApplyParams
{
	FEntity                      Source;          // 원천 (가해자) — 없으면 대상 자신
	float                        Level = -1.0f;   // < 0이면 원천 컴포넌트 Level (없으면 1)
	std::map<std::string, float> SetByCaller;
};

struct FEffectApplyResult
{
	bool        bApplied = false;
	uint32      Handle   = 0;  // 지속/무한 효과의 활성 핸들 (즉시 효과는 0)
	std::string Reason;        // 실패 이유 ("Immune", "MissingTags", "Blocked", "UnknownEffect", "NoAbilitySystem", "NotAuthority")
};

struct FAbilityActivateResult
{
	bool        bActivated = false; // 권한: 발동함 / 소유 클라이언트: 예측 발동함(또는 예측 없는 능력이면 요청을 보냄)
	std::string Reason;
	uint32      InstanceId = 0;
};

// 활성 효과 보기 (UI/Lua/디버그 — 권한은 실제 효과, 클라이언트는 복제 요약 + 예측 효과)
struct FActiveEffectView
{
	uint32      Handle = 0;
	std::string Name;
	int32       Stacks    = 1;
	float       Remaining = 0.0f; // 무한이면 -1
	float       Duration  = 0.0f;
	bool        bPredicted = false;
};

// Lua 능력 스크립트 실행 (Scripting/ScriptAbilityBindings.cpp가 구현, FGameWorld가 연결). 비어 있으면 스크립트 능력은 발동 즉시 끝난다
struct FAbilityScriptStart
{
	uint32      InstanceId = 0;
	FEntity     Owner;
	std::string Ability;
	std::string Script;
	float       Level      = 1.0f;
	bool        bPredicted = false; // 소유 클라이언트 예측 실행
	bool        bAuthority = true;
};
struct FAbilityScriptHooks
{
	// self:OnActivate(ctx)를 코루틴으로 시작해 첫 대기까지 진행. 스크립트를 못 읽으면 false (능력은 취소로 끝난다).
	// 코루틴이 끝나면(반환) 호스트가 FAbilitySystem::EndAbility(InstanceId)를 부른다
	std::function<bool(const FAbilityScriptStart&)> Start;
	std::function<void(uint32 InstanceId, bool bCancelled)> Stop; // 코루틴 버림 + self:OnEnd(ctx, cancelled). 실행 중이면 재개가 끝난 뒤
	std::function<void(float DeltaSeconds)>                 Tick; // 대기 중인 능력 코루틴 재개
};

// 네트워크 (FGameWorld가 연결). 비어 있으면 Standalone처럼 동작
struct FAbilityNetHooks
{
	std::function<bool(FEntity)> IsLocallyControlled; // 이 프로세스가 조종 (입력·예측). 없으면 권한 쪽만 true
	std::function<void(FEntity, const std::string& Ability, uint32 PredictionKey)>                     SendActivate; // 클라이언트 → 서버
	std::function<void(FEntity, uint32 PredictionKey, bool bCancelled, const std::string& Reason)> SendResult;   // 서버 → 소유 클라이언트 (거절/취소)
};

// C++ 능력 (게임 모듈): 능력 표 Native 칸의 이름으로 찾는다
struct FNativeAbilityContext
{
	FAbilitySystem&    System;
	uint32             InstanceId = 0;
	FEntity            Owner;
	const FAbilityDef& Def;
	bool               bPredicted = false;
	bool               bAuthority = true;
};
struct FNativeAbility
{
	std::function<void(FNativeAbilityContext&)>        OnActivate;
	std::function<bool(FNativeAbilityContext&, float)> OnTick; // false → 끝. 없으면 OnActivate 뒤 바로 끝난다
	std::function<void(FNativeAbilityContext&, bool bCancelled)> OnEnd;
};
// 엔진 DLL 전역. 소유자 = FTypeRegistry 등록 소유자(게임 모듈 OnLoad 중이면 모듈 이름) → 모듈 언로드 때 함께 지운다
class FAbilityNativeRegistry
{
public:
	static FAbilityNativeRegistry& Get();
	void                  Register(const std::string& Name, FNativeAbility Ability, std::string Owner = {});
	const FNativeAbility* Find(std::string_view Name) const;
	size_t                RemoveByOwner(const std::string& Owner);

private:
	struct FEntry
	{
		FNativeAbility Ability;
		std::string    Owner;
	};
	std::unordered_map<std::string, FEntry> Entries;
};

// 능력 시스템 (언리얼 GAS식). FGameWorld가 소유하고 플레이 동안 씬의 FAbilitySystemComponent를 돌린다. 규칙 기준은 AbilitySystem.cpp 머리 주석.
// 권한(서버/Standalone)이 발동 확정·효과 적용·속성 변경을 하고, 소유 클라이언트는 예측 키로 비용·쿨다운·발동 태그·자기 대상 효과·로컬 연출만 미리 한다
class FAbilitySystem
{
public:
	FAbilitySystem();
	~FAbilitySystem();
	FAbilitySystem(const FAbilitySystem&)            = delete;
	FAbilitySystem& operator=(const FAbilitySystem&) = delete;

	void    Begin(FScene& InScene, bool bInAuthority);
	void    End(); // 발동 중 능력 모두 취소 (스크립트 OnEnd — Lua 상태보다 먼저 부른다)
	bool    IsPlaying() const { return Scene != nullptr; }
	bool    IsAuthority() const { return bAuthority; }
	FScene* GetScene() const { return Scene; }

	void SetNetHooks(FAbilityNetHooks Hooks) { NetHooks = std::move(Hooks); }
	void SetScriptHooks(FAbilityScriptHooks Hooks) { ScriptHooks = std::move(Hooks); }

	// 한 틱 (FGameWorld: 스크립트 갱신 뒤, 캐릭터 이동 전): 초기화 → (권한) 효과 시간·주기 / (클라이언트) 복제 해석·예측 정리 →
	// 능력 스크립트·C++ 능력 진행 → (권한) 복제 문자열 → 이벤트
	void Tick(float DeltaSeconds);
	std::vector<FAbilityEvent> ConsumeEvents();

	// ---- 컴포넌트
	FAbilitySystemComponent*       Find(FEntity Entity);
	const FAbilitySystemComponent* Find(FEntity Entity) const;
	bool                           EnsureInitialized(FEntity Entity); // 컴포넌트가 없으면 false
	// 표 대신 코드 정의로 초기화 (테스트/게임 모듈). 컴포넌트가 없으면 붙인다. 표 경로는 무시된다
	bool InitializeWithSet(FEntity Entity, std::shared_ptr<const FAbilitySet> Set);

	// ---- 능력
	FAbilityActivateResult TryActivateAbility(FEntity Entity, std::string_view Ability);
	bool  CanActivateAbility(FEntity Entity, std::string_view Ability, std::string* OutReason = nullptr) const;
	bool  CancelAbility(FEntity Entity, std::string_view Ability);
	int32 CancelAbilitiesWithTags(FEntity Entity, const std::vector<std::string>& Tags, uint32 ExceptInstance = 0);
	bool  EndAbility(uint32 InstanceId, bool bCancelled = false);
	bool  CommitAbility(uint32 InstanceId); // 비용 + 쿨다운 + ActivationEffects (이미 했으면 true)
	bool  IsAbilityActive(FEntity Entity, std::string_view Ability) const;
	float GetCooldownRemaining(FEntity Entity, std::string_view Ability, float* OutDuration = nullptr) const;
	bool  GiveAbility(FEntity Entity, std::string_view Ability);   // 권한
	bool  RemoveAbility(FEntity Entity, std::string_view Ability); // 권한 (발동 중이면 취소)
	const std::vector<std::string>* GetGrantedAbilities(FEntity Entity) const;
	// 인스턴스 조회 (스크립트 ctx). 끝났으면 nullptr
	const FAbilityInstance* FindInstance(uint32 InstanceId, FEntity* OutOwner = nullptr) const;
	void  EndPredictionWindow(uint32 InstanceId); // 스크립트 호스트: OnActivate가 처음 대기에 들어감
	// 조종하는 쪽 입력: WasPressed(액션 이름)가 참인 부여 능력을 발동 (FGameWorld, bAcceptInput 컴포넌트만)
	void  HandleInput(FEntity Entity, const std::function<bool(const std::string&)>& WasPressed);

	// ---- 네트워크 (FGameWorld가 메시지를 풀어 부른다)
	void ServerHandleActivate(FEntity Entity, const std::string& Ability, uint32 PredictionKey);
	void ClientHandleResult(FEntity Entity, uint32 PredictionKey, bool bCancelled, const std::string& Reason);

	// ---- 효과
	FEffectApplyResult ApplyEffect(FEntity Target, std::string_view Effect, const FEffectApplyParams& Params = {}); // 권한
	// 능력 안에서 (ctx:ApplyEffectToSelf/Target): 권한은 예측 창 안이면 그 예측 키를 붙여 적용, 소유 클라이언트는 예측 창 안의 자기 대상만 예측
	FEffectApplyResult ApplyEffectFromAbility(uint32 InstanceId, FEntity Target, std::string_view Effect, const FEffectApplyParams& Params = {});
	bool  RemoveEffect(FEntity Target, uint32 Handle);                               // 권한
	int32 RemoveEffectsWithTags(FEntity Target, const std::vector<std::string>& Tags); // 권한 (AssetTags/GrantedTags)
	std::vector<FActiveEffectView> GetActiveEffects(FEntity Entity) const;

	// ---- 속성 (현재값/기본값). 없으면 false
	bool GetAttribute(FEntity Entity, std::string_view Attribute, float& OutValue, bool bBase = false) const;
	bool SetBaseAttribute(FEntity Entity, std::string_view Attribute, float Value); // 권한 (Health는 체력 컴포넌트 경유)

	// ---- 태그 (부모 질의 포함)
	int32 GetTagCount(FEntity Entity, std::string_view Tag) const;
	bool  HasTag(FEntity Entity, std::string_view Tag) const { return GetTagCount(Entity, Tag) > 0; }
	bool  HasTagExact(FEntity Entity, std::string_view Tag) const;
	bool  AddLooseTag(FEntity Entity, std::string_view Tag, int32 Count = 1); // 권한
	bool  RemoveLooseTag(FEntity Entity, std::string_view Tag, int32 Count = 1);

	// ---- 스크립트 대기 (ctx:WaitAnimNotify): 직전 애니메이션 갱신에서 Owner(와 자손) 모델에 그 이름의 노티파이가 났나
	bool HasAnimNotify(FEntity Owner, std::string_view Name) const;

	// ---- 디버그 (콘솔 ability.Dump, 통계)
	std::string Describe(FEntity Entity) const;
	std::vector<FEntity> GetEntities() const; // 컴포넌트가 있는 엔티티 (인덱스 순)
	static FAbilitySystem* GetActive();       // 가장 최근 Begin한 시스템 (콘솔 명령용, 메인 스레드)
	uint32 GetRejectedCount() const { return RejectedCount; }   // 테스트/통계: 클라이언트가 받은 거절 수
	uint32 GetRolledBackCount() const { return RolledBackCount; } // 테스트/통계: 거절로 되돌린 예측 효과/값 수

private:
	struct FNativeRunning
	{
		FNativeAbility Ability;
		FEntity        Owner;
	};

	FAbilitySystemComponent* Require(FEntity Entity); // 초기화까지
	bool IsLocallyControlled(FEntity Entity) const;
	void InitializeComponent(FEntity Entity, FAbilitySystemComponent& Component);
	void RebindDefinitions(FEntity Entity, FAbilitySystemComponent& Component, std::shared_ptr<const FAbilitySet> Set);

	// 발동
	bool CanActivateInternal(FEntity Entity, const FAbilitySystemComponent& Component, const FAbilityDef& Def, std::string& OutReason) const;
	FAbilityActivateResult ActivateInternal(FEntity Entity, const FAbilityDef& Def, uint32 PredictionKey, bool bPredicted);
	bool CheckCost(FEntity Entity, const FAbilitySystemComponent& Component, const FAbilityDef& Def) const;
	FAbilityInstance* FindInstanceMutable(uint32 InstanceId, FEntity* OutOwner);
	void PushFailed(FEntity Entity, std::string_view Ability, std::string Reason);

	// 효과 (권한)
	FEffectApplyResult ApplyEffectInternal(FEntity Target, const FGameplayEffectDef& Def, const std::shared_ptr<const FAbilitySet>& Set,
	                                       const FEffectApplyParams& Params, uint32 PredictionKey);
	std::vector<float> CaptureMagnitudes(FEntity Target, const FGameplayEffectDef& Def, const FEffectApplyParams& Params, float Level) const;
	void ExecuteModifiers(FEntity Target, const FGameplayEffectDef& Def, const std::vector<float>& Magnitudes, int32 Stacks, FEntity Source);
	void ChangeBase(FEntity Target, FAbilitySystemComponent& Component, const std::string& Attribute, float NewBase, FEntity Source);
	void AddEffectTags(FAbilitySystemRuntime& Runtime, const FGameplayEffectDef& Def, int32 Sign);
	void RemoveEffectAt(FEntity Target, FAbilitySystemComponent& Component, size_t Index);
	void TickEffects(FEntity Entity, FAbilitySystemComponent& Component, float DeltaSeconds);
	void RecomputeAttributes(FEntity Entity, FAbilitySystemComponent& Component);
	float ClampAttribute(const FAbilitySystemRuntime& Runtime, const FAttributeDef* Def, float Value) const;
	void SyncFromHealth(FEntity Entity, FAbilitySystemComponent& Component);
	void SyncToHealth(FEntity Entity, FAbilitySystemComponent& Component);
	void WriteReplicatedState(FAbilitySystemComponent& Component);

	// 클라이언트 (복제 + 예측)
	void ReadReplicatedState(FEntity Entity, FAbilitySystemComponent& Component);
	void TickPrediction(FEntity Entity, FAbilitySystemComponent& Component, float DeltaSeconds);
	void RebuildClientView(FEntity Entity, FAbilitySystemComponent& Component);
	FEffectApplyResult ApplyPredictedEffect(FEntity Owner, FAbilitySystemComponent& Component, const FGameplayEffectDef& Def, uint32 PredictionKey,
	                                        const FEffectApplyParams& Params);
	void DropPrediction(FAbilitySystemComponent& Component, uint32 PredictionKey, bool bUpToKey);

	void DetectChanges(FEntity Entity, FAbilitySystemComponent& Component);

	FScene*                  Scene      = nullptr;
	bool                     bAuthority = true;
	FAbilityNetHooks         NetHooks;
	FAbilityScriptHooks      ScriptHooks;
	std::vector<FAbilityEvent> Events;
	uint32                   NextInstanceId = 1;
	std::unordered_map<uint32, FNativeRunning> NativeRunning;
	std::unordered_map<uint32, FEntity>        InstanceOwners; // 발동 중 인스턴스 → 소유 엔티티
	void RebuildBlockedTags(FAbilitySystemRuntime& Runtime) const;
	void ExecuteMeta(FEntity Target, FAbilitySystemComponent& Component, const std::string& Attribute, float Amount, FEntity Source);
	uint32                   RejectedCount   = 0;
	uint32                   RolledBackCount = 0;
};
