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
struct FTypeInfo;

// Lua의 Entity 값: 엔티티 핸들만 담는다 (씬은 런타임이 가진 현재 씬)
struct FScriptEntity
{
	FEntity Entity;
};

// Lua의 컴포넌트 참조: entity:GetComponent("TransformComponent"). 필드 접근마다 리플렉션으로 현재 값을 읽고 쓴다
// (컴포넌트 포인터는 풀 재할당으로 바뀔 수 있어 보관하지 않는다)
struct FScriptComponentRef
{
	FEntity          Entity;
	const FTypeInfo* Type = nullptr;
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
	void Update(float DeltaSeconds, const FInput* Input);
	void DestroyAllInstances(); // OnDestroy 호출 후 인스턴스 제거

	bool         RunString(std::string_view Code);
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

	void RegisterBindings();
	void RegisterMathBindings();
	void RegisterEntityBindings();
	void RegisterGlobals();

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

	sol::state            Lua;
	sol::protected_function Traceback; // 오류 메시지에 콜스택 추가 (debug.traceback)
	std::filesystem::path ContentDirectory;
	uint32&               ErrorCounter;
	const FScriptAudioHooks* AudioHooks = nullptr;
	const FScriptPhysicsHooks* PhysicsHooks = nullptr;

	std::unordered_map<std::string, std::unique_ptr<FScriptClass>> Classes; // 키: 정규화된 절대 경로

	// 플레이 상태 (비소유)
	FScene*       Scene = nullptr;
	const FInput* Input = nullptr;
	double        TotalTime  = 0.0;
	int64         FrameCount = 0;
	bool          bStructureChanged = false;

	std::unordered_map<uint64, FScriptInstance> Instances; // 키: FEntity::ToId()
	std::vector<FEntity>                        PendingDestroy;
	std::vector<FEntity>                        UpdateOrder; // 매 프레임 재사용 (할당 최소화)

	friend struct FLuaBindings;
};
