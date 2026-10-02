#pragma once

// Scripting 모듈 내부 전용 (sol 타입 노출). 외부 모듈은 ScriptSystem.h만 사용한다.
#include "Core/ECS/Entity.h"
#include "Scripting/ScriptSystem.h"
#include "Scripting/ScriptValue.h"
#include "Scripting/SolInclude.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FInput;
class FScene;
struct FScriptComponent;
struct FTypeInfo;

// Lua의 Entity 값: 엔티티 핸들만 담는다 (씬은 런타임이 가진 현재 씬)
struct FScriptEntity
{
	FEntity Entity;
};

// Lua의 에셋 참조 값 (스크립트 Properties의 Asset 타입): Prefab("Prefabs/Ball.eprefab"), Asset("Sounds/Hit.wav", ".wav").
// 읽기 전용 (.Path, .Filter) — 인스턴스끼리 공유해도 안전하다
struct FScriptAssetRef
{
	std::string Path;
	std::string Filter;
};

// Lua의 컴포넌트 참조: entity:GetComponent("TransformComponent"). 필드 접근마다 리플렉션으로 현재 값을 읽고 쓴다
// (컴포넌트 포인터는 풀 재할당으로 바뀔 수 있어 보관하지 않는다)
struct FScriptComponentRef
{
	FEntity          Entity;
	const FTypeInfo* Type = nullptr;
};

// Lua의 블랙보드 참조: entity:GetBlackboard(). 엔티티만 담고 값은 매번 AI 훅으로 읽고 쓴다
struct FScriptBlackboard
{
	FEntity Entity;
};

// Lua 상태 하나 + 엔진 바인딩 + 스크립트 클래스 캐시 + (플레이 중) 인스턴스
class FLuaRuntime
{
public:
	struct FScriptClass
	{
		bool                             bValid = false;
		std::string                      Error;    // 로드 실패 메시지
		std::string                      AssetName; // 로그 표시용 (Content 기준 상대 경로)
		sol::table                       Class;
		sol::table                       Metatable; // 모든 인스턴스가 공유 (__index = Class) → 핫 리로드 시 교체만 하면 된다
		std::vector<FScriptPropertyDecl> Decls;     // Class.Properties (이름순)
	};

	FLuaRuntime(std::filesystem::path InContentDirectory, uint32& InErrorCounter);
	~FLuaRuntime();

	FLuaRuntime(const FLuaRuntime&)            = delete;
	FLuaRuntime& operator=(const FLuaRuntime&) = delete;

	// ScriptAsset(Content 기준 상대 경로) 클래스 로드 (캐시). 실패해도 항목을 남겨 같은 오류를 매 프레임 반복하지 않는다
	FScriptClass& LoadClass(const std::string& ScriptAsset);

	// 이미 로드한 적 있는 스크립트 파일이면 다시 실행해 교체. 반환: 캐시에 있었는가, OutSucceeded: 새 코드 로드 성공
	bool ReloadClass(const std::filesystem::path& ScriptPath, bool& bOutSucceeded);

	// ---- 플레이
	void SetScene(FScene* InScene) { Scene = InScene; }
	void SetAudioHooks(const FScriptAudioHooks* InHooks) { AudioHooks = InHooks; } // FScriptSystem 소유 (런타임보다 오래 산다)
	void SetPhysicsHooks(const FScriptPhysicsHooks* InHooks) { PhysicsHooks = InHooks; } // 〃
	void SetNetHooks(const FScriptNetHooks* InHooks) { NetHooks = InHooks; }             // 〃
	void SetAIHooks(const FScriptAIHooks* InHooks) { AIHooks = InHooks; }                // 〃
	void SetAppHooks(const FScriptAppHooks* InHooks) { AppHooks = InHooks; }             // 〃
	void SetSteamHooks(const FScriptSteamHooks* InHooks) { SteamHooks = InHooks; }       // 〃
	void SetPersistentValues(FScriptValueMap* InValues) { PersistentValues = InValues; } // 〃 (Game.SetPersistent)

	// 스크립트 객체 (LuaAIBindings.cpp). 0 = 실패. 호출 오류가 난 객체는 멈춘다(핫 리로드 성공 시 재개)
	uint32 CreateObject(const std::string& ScriptAsset, const std::string& Overrides, FEntity Entity);
	bool   CallObject(uint32 Id, const char* Method, const float* DeltaSeconds, FScriptValue& OutResult, bool* bOutFound);
	void   DestroyObject(uint32 Id);
	void Update(float DeltaSeconds, const FInput* Input);
	void LateUpdate(float DeltaSeconds, const FInput* Input); // 시작된 인스턴스의 OnLateUpdate(dt)
	void DestroyAllInstances(); // OnDestroy 호출 후 인스턴스 제거

	bool         RunString(std::string_view Code);
	bool         InvokeMethod(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args);
	void         BroadcastMethod(const std::string& MethodName, const FGameRpcArgs& Args);
	// InvokeMethod + 마지막 인자로 필드 표(Lua 테이블) — 충돌 정보 등 (ScriptPhysicsBindings.cpp)
	bool         InvokeMethodWithFields(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args, const FScriptEventFields& Fields);
	void         RequestDestroy(FEntity Entity) { PendingDestroy.push_back(Entity); } // entity:Destroy()와 같은 지연 파괴
	size_t       GetInstanceCount() const { return Instances.size(); }
	FScriptValue GetInstanceProperty(FEntity Entity, const std::string& Name);

	// 스크립트가 엔티티/컴포넌트를 추가·제거했는가 (호출 시 초기화). 앱은 true면 에셋 참조를 다시 해석한다
	bool ConsumeStructureChanged()
	{
		const bool bChanged = bStructureChanged;
		bStructureChanged   = false;
		return bChanged;
	}

	sol::state& GetState() { return Lua; }

private:
	struct FScriptInstance
	{
		FEntity       Entity;
		std::string   ScriptAsset;
		std::string   ClassKey;
		sol::table    Self;
		bool          bStarted = false;
		bool          bFaulted = false; // 오류 후 정지 (핫 리로드 시 해제)
	};

	// 직전 애니메이션 갱신의 노티파이를 스크립트 함수로 전달 (OnAnimNotify_<이름> 등)
	void DispatchAnimNotifies();
	void RegisterBindings();
	void RegisterMathBindings();
	void RegisterEntityBindings();
	void RegisterGlobals();
	void RegisterPrefabBindings(); // Asset/Prefab 값, Scene.SpawnPrefab
	void RegisterNetBindings();    // Net 테이블, entity:GetOwner/IsLocallyOwned
	void RegisterAIBindings();     // AI 테이블, entity:GetBlackboard/MoveTo 등 (LuaAIBindings.cpp)
	// 선언 기본값 + 오버라이드로 Properties 테이블을 만든다 (인스턴스/스크립트 객체 공용)
	sol::table MakeProperties(const FScriptClass& Class, const std::string& ScriptAsset, const std::string& Overrides);

	bool  ShouldRunHere(const FScriptComponent& Component) const; // ExecutionLocation 필터
	int32 GetLocalPlayerId() const;
	int32 GetOwner(FEntity Entity) const;
	void RegisterUIBindings();     // entity:GetWidget, UIWidget 값 (ScriptUIBindings.cpp)
	void RegisterGameBindings();   // Game 테이블: 종료, 화면 설정 + Steam 테이블 (ScriptGameBindings.cpp)
	void RegisterGameplayBindings(); // 체력/데미지(entity:ApplyDamage 등), GameMode, SaveGame 테이블 (ScriptGameplayBindings.cpp)
	void RegisterAnimationGraphBindings(); // entity:SetAnimParam/GetAnimParam/GetAnimState (ScriptAnimationBindings.cpp)
	void RegisterPhysicsBindings();        // entity:EnableRagdoll/DisableRagdoll/IsRagdollActive (ScriptPhysicsBindings.cpp)
	// 이번 프레임 게임 UI 이벤트를 스크립트 함수로 전달 (OnUIClicked_<위젯 이름> 등, ScriptUIBindings.cpp)
	void DispatchUIEvents();
	void RegisterSequenceBindings(); // entity:PlaySequence/StopSequence 등 (ScriptSequenceBindings.cpp)
	// 직전 시퀀스 갱신의 이벤트 → OnSequenceEvent_<이름>, OnSequenceFinished (ScriptSequenceBindings.cpp)
	void DispatchSequenceEvents();
	// 직전 애니메이션 갱신에서 끝난 몽타주 → OnMontageEnded(clip, interrupted, slot) (ScriptAnimationBindings.cpp)
	void DispatchMontageEvents();

	// Scene.SpawnPrefab 요청: 스크립트 갱신 루프 밖에서 만든다 (ApplyPendingSpawns)
	struct FPendingSpawn
	{
		std::string             Asset;
		bool                    bHasPosition = false;
		FVector3                Position;
		sol::protected_function OnSpawned; // 만든 뒤 루트 엔티티로 호출 (없을 수 있음)
	};
	void ApplyPendingSpawns();

	std::string MakeClassKey(const std::filesystem::path& AbsolutePath) const;
	bool        ExecuteClassFile(FScriptClass& Class, const std::filesystem::path& AbsolutePath);

	void CreateInstance(FEntity Entity, const std::string& ScriptAsset, const std::string& Overrides);
	bool CallMethod(FScriptInstance& Instance, const char* MethodName, float DeltaSeconds = 0.0f, bool bPassDelta = false);
	void DestroyInstance(uint64 EntityId);
	void ApplyPendingDestroys();
	void ReportError(const std::string& Message);

	// 값 변환
	FScriptValue ToScriptValue(const sol::object& Object);
	sol::object  FromScriptValue(const FScriptValue& Value);
	sol::object  CopyValue(const sol::object& Object); // Vector3 등 값 타입 userdata 깊은 복사
	FGameRpcValue ToRpcValue(const sol::object& Object); // RPC로 보낼 수 없는 값이면 std::runtime_error
	sol::object   FromRpcValue(const FGameRpcValue& Value);
	void          CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const sol::variadic_args& Args);

	sol::state            Lua;
	sol::protected_function Traceback; // 오류 메시지에 콜스택 추가 (debug.traceback)
	std::filesystem::path ContentDirectory;
	uint32&               ErrorCounter;
	const FScriptAudioHooks* AudioHooks = nullptr;
	const FScriptPhysicsHooks* PhysicsHooks = nullptr;
	const FScriptNetHooks*     NetHooks     = nullptr;
	const FScriptAIHooks*      AIHooks      = nullptr;
	const FScriptAppHooks*     AppHooks     = nullptr;
	const FScriptSteamHooks*   SteamHooks   = nullptr;
	FScriptValueMap*           PersistentValues = nullptr;
	FVector2                   LocalControlRotation; // 훅이 없을 때(테스트) Net.SetControlRotation 값

	std::unordered_map<std::string, std::unique_ptr<FScriptClass>> Classes; // 키: 정규화된 절대 경로

	// 플레이 상태 (비소유)
	FScene*       Scene = nullptr;
	const FInput* Input = nullptr;
	double        TotalTime  = 0.0;
	int64         FrameCount = 0;
	bool          bStructureChanged = false;

	std::unordered_map<uint64, FScriptInstance> Instances; // 키: FEntity::ToId()
	std::vector<FEntity>                        PendingDestroy;
	std::vector<FPendingSpawn>                  PendingSpawns;
	std::vector<FEntity>                        UpdateOrder; // 매 프레임 재사용 (할당 최소화)

	struct FScriptObject
	{
		std::string ScriptAsset;
		sol::table  Self;
		bool        bFaulted = false;
	};
	std::unordered_map<uint32, FScriptObject> Objects; // 스크립트 객체 (컴포넌트 없음)
	uint32                                    NextObjectId = 1;

	friend struct FLuaBindings;
};
