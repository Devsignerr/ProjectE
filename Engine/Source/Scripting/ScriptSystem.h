#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/GameRpc.h"
#include "Scripting/ScriptValue.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class FAbilitySystem;
class FInput;
class FLuaRuntime;
struct FAbilityScriptStart;
class FScene;
class FScriptDebugger;

// 스크립트가 쓰는 오디오 기능. 앱이 Audio 모듈(FAudioSystem/FAudioEngine)과 연결한다 (비어 있으면 무시)
struct FScriptAudioHooks
{
	std::function<void(FEntity)>             Play;        // 엔티티의 오디오 소스 재생 (처음부터)
	std::function<void(FEntity)>             Stop;        // 정지
	std::function<void(const std::string&)> PlayOneShot; // Content 기준 경로의 효과음 (비공간)
};

struct FScriptRayHit
{
	FEntity  Entity;
	FVector3 Position; // cm
	FVector3 Normal;
	float    Distance = 0.0f; // cm
};

// 겹침/쓸어 보기 모양 (Physics의 FPhysicsQueryShape와 같은 뜻, cm). 캡슐은 회전 전 +Z 축
enum class EScriptQueryShape : uint8
{
	Sphere,
	Box,
	Capsule,
};
struct FScriptQueryShape
{
	EScriptQueryShape Shape = EScriptQueryShape::Sphere;
	FVector3          HalfExtents;       // 상자
	float             Radius     = 0.0f; // 구/캡슐
	float             HalfHeight = 0.0f; // 캡슐 원기둥 절반
	FQuat             Rotation;
};

// 스크립트가 쓰는 물리 기능. 앱이 Physics 모듈(FPhysicsSystem)과 연결한다 (비어 있으면 무시). 단위 cm, kg
struct FScriptPhysicsHooks
{
	std::function<bool(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit)> Raycast;
	std::function<void(FEntity, const FVector3&)> AddForce;    // kg·cm/s²
	std::function<void(FEntity, const FVector3&)> AddImpulse;  // kg·cm/s
	std::function<void(FEntity, const FVector3&)> SetVelocity; // cm/s
	std::function<FVector3(FEntity)>              GetVelocity;
	std::function<float(FEntity)>                 GetMass;     // kg (밀도 자동 계산 포함)
	// 캐릭터 이동 (FCharacterMovementComponent): 이번 프레임 이동 방향(월드)/점프 요청, 바닥 여부
	std::function<void(FEntity, const FVector3&)> AddMovementInput;
	std::function<void(FEntity)>                  Jump;
	std::function<bool(FEntity)>                  IsGrounded;
	// 래그돌 (Physics/Ragdoll.h): 엔티티 자신이나 자손의 스켈레탈 모델
	std::function<bool(FEntity)>                  EnableRagdoll;
	std::function<void(FEntity)>                  DisableRagdoll;
	std::function<bool(FEntity)>                  IsRagdollActive;
	// 모양 질의 (FPhysicsSystem::Overlap/Sweep — 트리거 제외, Ignore(NullEntity = 없음)의 바디와 충돌을 끈 쌍 제외)
	std::function<void(const FScriptQueryShape&, const FVector3& Position, FEntity Ignore, std::vector<FEntity>& OutEntities)> Overlap;
	std::function<bool(const FScriptQueryShape&, const FVector3& Start, const FVector3& Direction, float MaxDistance, FEntity Ignore,
	                   FScriptRayHit& OutHit)>
		Sweep;
	// 충돌 레이어로 거른 레이캐스트 (LayerMask 비트 i = 레이어 칸 i, FCollisionLayerSettings). 없으면 레이어를 준 Physics.Raycast는 nil
	std::function<bool(const FVector3& Origin, const FVector3& Direction, float MaxDistance, uint32 LayerMask, FScriptRayHit& OutHit)> RaycastLayers;
};

// LAN에서 찾은 세션 (Lua Net.GetSessions의 항목)
struct FScriptLanSession
{
	std::string Name;
	std::string SceneAsset;
	std::string Address; // Net.Connect에 넘긴다
	int32       Players    = 0;
	int32       MaxPlayers = 0;
};

// 스크립트가 쓰는 네트워크 정보. 앱(FGameWorld)이 Network 모듈과 연결한다 (Scripting은 Network에 의존하지 않는다).
// 기본값 = Standalone (서버이자 클라이언트, 모든 스크립트 실행, 로컬 플레이어 0)
struct FScriptNetHooks
{
	bool        bRunServerScripts = true; // ExecutionLocation 필터 (세션 시작 시 고정)
	bool        bRunClientScripts = true;
	bool        bIsServer         = true;
	bool        bIsClient         = true;
	std::string ModeName          = "Standalone";
	std::function<int32()>        GetLocalPlayerId; // 없으면 0. 전용 서버 -1 (로컬 플레이어 없음), 클라이언트는 입장 후 정해진다
	std::function<int32(FEntity)> GetOwner;         // 엔티티(또는 가장 가까운 복제 조상)의 소유 플레이어, 없으면 -1
	// RPC 라우팅 (entity:CallServer 등, 규칙은 Scene/GameRpc.h). 없으면 Standalone: 바로 로컬 호출.
	// 잘못된 호출(클라이언트에서 CallClient 등)은 std::runtime_error로 알린다 (스크립트 오류가 된다)
	std::function<void(FEntity, EGameRpcKind, const std::string&, const FGameRpcArgs&)> SendRpc;
	// 스크립트가 보는 Input (없으면 Update에 넘긴 로컬 입력). 서버는 엔티티 소유 플레이어의 입력을 돌려준다 (없으면 nullptr = 입력 없음)
	std::function<const FInput*(FEntity, const FInput* LocalInput)> ResolveInput;
	// 시점 방향 (언리얼 ControlRotation, 도): 로컬 플레이어가 정하고 입력과 함께 서버로 간다.
	// Get = 엔티티 소유 플레이어의 값 (서버: 원격/호스트, 클라이언트: 자기 것만). 없으면 Standalone: 로컬 값
	std::function<void(const FVector2& YawPitch)> SetLocalControlRotation;
	std::function<FVector2(FEntity)>              GetControlRotation;

	// 세션 (로비): 찾기/목록, 전환 요청(호스트/접속/끊기 — 앱이 프레임 끝에 처리), 상태 문자열
	std::function<void()>                           FindSessions;
	std::function<std::vector<FScriptLanSession>()> GetSessions;
	std::function<void(int32 Port)>                 Host;
	std::function<void(const std::string& Address)> Connect;
	std::function<void()>                           Disconnect;
	std::function<std::string()>                    GetState;         // Standalone / Hosting / Connecting / Connected / Failed
	std::function<std::string()>                    GetFailureReason; // Failed일 때 사유

	// 맵 전환 (Lua Game.OpenScene/GetCurrentScene, FGameWorld가 연결). OpenScene: 빈 문자열 = 접수(프레임 끝에 전환), 아니면 거절 사유
	std::function<std::string(const std::string& SceneAsset)> OpenScene;
	std::function<std::string()>                              GetCurrentScene;

	// 서브 씬 (Lua Scene.LoadSubScene 등). Load: 빈 문자열 = 접수, 아니면 거절 사유
	std::function<std::string(const std::string& Asset, const FVector3& Offset)> LoadSubScene;
	std::function<bool(const std::string& Asset)>                                UnloadSubScene;
	std::function<bool(const std::string& Asset)>                                IsSubSceneLoaded;
	std::function<FEntity(const std::string& Asset)>                             GetSubSceneRoot;
};

// 스크립트가 쓰는 AI 기능 (블랙보드, 이동, 경로). 앱(FGameWorld)이 AI 모듈(FAISystem)과 연결한다 (Scripting은 AI에 비의존).
// 블랙보드 값은 FScriptValue(Bool/Number/String/Vector3) 또는 엔티티. 트리가 없거나 키가 없으면 실패
struct FScriptAIHooks
{
	// 설정 안 됨/키 없음/트리 없음이면 false. 엔티티 키면 bOutIsEntity = true + OutEntity
	std::function<bool(FEntity, const std::string& Key, FScriptValue& OutValue, FEntity& OutEntity, bool& bOutIsEntity)> GetBlackboard;
	std::function<bool(FEntity, const std::string& Key, const FScriptValue& Value)> SetBlackboard;       // 키 타입과 다르면 false
	std::function<bool(FEntity, const std::string& Key, FEntity Value)>             SetBlackboardEntity; // 〃
	std::function<bool(FEntity, const std::string& Key)>                            ClearBlackboard;
	std::function<std::string(FEntity, const FVector3& Goal, float AcceptanceRadius)> MoveTo;       // "Moving"/"Succeeded"/"Failed" (반경 < 0 = 에이전트 값)
	std::function<std::string(FEntity)>                                               GetMoveStatus; // + "Idle"
	std::function<void(FEntity)>                                                      StopMove;
	std::function<bool(const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints)> FindPath;
	std::function<bool(FEntity)> StartTree; // FBehaviorTreeComponent 에셋으로 (재)시작
	std::function<void(FEntity)> StopTree;
};

// 스크립트가 쓰는 앱 기능 (Lua Game 테이블 — 종료, 화면 설정). 런타임/에디터가 연결한다 (비어 있으면 무시)
struct FScriptAppHooks
{
	std::function<void()>                         Quit;          // 런타임: 종료 요청 / 에디터: 플레이 정지 요청 (둘 다 프레임 끝에 처리)
	std::function<std::string()>                  GetWindowMode; // "Windowed" / "BorderlessFullscreen"
	std::function<bool(const std::string& Mode)>  SetWindowMode; // 알 수 없는 이름이면 false
	std::function<bool()>                         IsVSync;
	std::function<void(bool)>                     SetVSync;
	std::function<void(bool)>                     SetMouseLocked; // 런타임: 커서 숨김 + 창에 가둠 (포커스를 잃으면 풀림)
	std::function<bool()>                         IsMouseLocked;
};

// 스크립트가 쓰는 Steam 기능 (Lua Steam 테이블). FGameWorld가 Online 모듈(FSteamSubsystem)과 연결한다 (비어 있으면 사용 불가)
struct FScriptSteamHooks
{
	std::function<bool()>                         IsAvailable;
	std::function<std::string()>                  GetPlayerName;
	std::function<std::string()>                  GetLanguage;
	std::function<bool(const std::string& Name)>  UnlockAchievement;
	std::function<bool(const std::string& Name)>  IsAchievementUnlocked;
	std::function<bool(const std::string& Name)>  ClearAchievement;
	std::function<bool(const std::string& Dialog)> ActivateOverlay;
	std::function<bool()>                         IsOverlayActive;
};

// 스크립트가 쓰는 3D 디버그 그리기 (Lua Debug 테이블). FGameWorld가 Renderer의 FDebugDraw와 연결한다 (Scripting은 Renderer 비의존).
// 단위 cm, 색 = sRGB 0~1 (RGBA), Duration 초 (0 = 한 프레임), bDepthTest = false면 항상 위. 비어 있으면 무시
struct FScriptDebugDrawHooks
{
	std::function<void(const FVector3& Start, const FVector3& End, const FVector4& Color, float Duration, bool bDepthTest)> DrawLine;
	std::function<void(const FVector3& From, const FVector3& To, const FVector4& Color, float Duration, bool bDepthTest)>   DrawArrow;
	std::function<void(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color, float Duration,
	                   bool bDepthTest)>
		DrawBox;
	std::function<void(const FVector3& Center, float Radius, const FVector4& Color, float Duration, bool bDepthTest)> DrawSphere;
	std::function<void(const FVector3& Center, float Radius, float HalfHeight, const FQuat& Rotation, const FVector4& Color, float Duration,
	                   bool bDepthTest)>
		DrawCapsule;
};

// 이름 붙은 값 목록 → Lua 테이블 하나 (InvokeMethodWithFields의 마지막 인자: 충돌 정보 등)
using FScriptEventFields = std::vector<std::pair<std::string, FGameRpcValue>>;

// 컴포넌트가 아닌 스크립트 객체 (Lua 비헤이비어 트리 노드 등): 플레이 세션 안에서만 유효한 핸들. 0 = 무효
using FScriptObjectHandle = uint64;

// Lua 스크립트 컴포넌트(FScriptComponent) 실행 시스템.
//
// 스크립트 형식 (Unity 스타일 테이블 반환):
//   local Rotator = { Properties = { Speed = 90.0 } }  -- 기본값 선언 (인스펙터에 표시, 씬에서 덮어쓰기)
//   function Rotator:OnStart() end                     -- 첫 OnUpdate 직전 한 번
//   function Rotator:OnUpdate(dt) end                  -- 매 프레임
//   function Rotator:OnLateUpdate(dt) end              -- 매 프레임, 물리/트랜스폼 갱신 뒤 (카메라 따라가기)
//   function Rotator:OnDestroy() end                   -- 엔티티/컴포넌트 제거 또는 플레이 종료 시
//   return Rotator
// 인스턴스(self)마다 self.entity(엔티티 핸들)와 self.Properties(기본값 + 오버라이드 복사본)가 있다.
// 프리팹: Properties에 Prefab("Prefabs/X.eprefab")로 선언하면 인스펙터에 드롭 칸이 생기고,
//   Scene.SpawnPrefab(self.Properties.X, position?, function(root) ... end)로 만든다. 생성은 그 프레임 스크립트 갱신이 끝난 뒤
//   (지연)이며 콜백이 그때 루트 엔티티를 받는다. 만든 엔티티의 에셋 참조는 ConsumeSceneStructureChanged로 앱이 해석한다.
//
// 플레이 세션마다 새 Lua 상태를 만든다 (전역 상태가 세션 사이에 남지 않음, Unity의 도메인 리로드와 같은 효과).
// 스크립트 오류는 로그로 남기고 해당 인스턴스만 멈춘다 (핫 리로드 시 재개). 절대 크래시하지 않는다.
// 성능: 매 프레임 대량 순회는 C++ 시스템에 둔다. 스크립트 호출은 인스턴스당 OnUpdate 한 번.
class FScriptSystem
{
public:
	FScriptSystem();
	~FScriptSystem();

	FScriptSystem(const FScriptSystem&)            = delete;
	FScriptSystem& operator=(const FScriptSystem&) = delete;

	// ScriptAsset 상대 경로의 기준 (프로젝트 Content 디렉터리)
	void                         SetContentDirectory(const std::filesystem::path& Directory);
	const std::filesystem::path& GetContentDirectory() const { return ContentDirectory; }

	// ---- 플레이 세션
	// Scene은 EndPlay까지 살아 있어야 한다 (비소유)
	bool BeginPlay(FScene& Scene);
	// 새 스크립트 컴포넌트 인스턴스화(+OnStart) → OnUpdate → 지연 삭제 적용. Input은 nullptr 허용 (입력 없음)
	// DeltaSeconds는 MaxDeltaSeconds로 제한한다 (로딩/중단점 뒤 한 프레임에 크게 튀지 않도록, Unity maximumDeltaTime과 같은 목적)
	static constexpr float MaxDeltaSeconds = 0.25f;
	void Update(float DeltaSeconds, const FInput* Input);
	// 물리·트랜스폼 갱신이 끝난 뒤 (FGameWorld::TickGameplay 끝): 스크립트 OnLateUpdate(dt) — 캐릭터를 따라가는 카메라처럼
	// "이번 프레임 최종 위치"가 필요한 일 (OnUpdate에서 읽는 월드 위치는 물리가 움직이기 전 값이다)
	// 반환: 스크립트가 씬을 바꿨을 수 있는지 (Lua 함수를 하나도 부르지 않고 파괴도 없었으면 false)
	bool LateUpdate(float DeltaSeconds, const FInput* Input);
	// 모든 인스턴스 OnDestroy 후 Lua 상태 파괴
	void EndPlay();
	bool IsPlaying() const { return PlayRuntime != nullptr; }
	// 스크립트가 엔티티/컴포넌트를 추가·제거했는가 (호출 시 초기화). true면 앱이 에셋 참조(메시 등)를 다시 해석한다
	bool ConsumeSceneStructureChanged();

	// 다음 BeginPlay부터 적용 (플레이 중이면 즉시)
	void SetAudioHooks(FScriptAudioHooks Hooks);
	void SetPhysicsHooks(FScriptPhysicsHooks Hooks);
	void SetNetHooks(FScriptNetHooks Hooks);
	void SetAIHooks(FScriptAIHooks Hooks);
	void SetAppHooks(FScriptAppHooks Hooks);
	void SetSteamHooks(FScriptSteamHooks Hooks);
	void SetDebugDrawHooks(FScriptDebugDrawHooks Hooks);
	// 능력 시스템 (Scene/Ability, FGameWorld 소유 — 비소유). Lua entity:TryActivateAbility 등이 쓴다
	void SetAbilitySystem(FAbilitySystem* InSystem);
	// 능력 스크립트 호스트 (FAbilityScriptHooks로 연결, 규칙은 Scripting/ScriptAbilityBindings.cpp 머리 주석)
	bool StartAbilityScript(const FAbilityScriptStart& Start);
	void StopAbilityScript(uint32 InstanceId, bool bCancelled);
	void TickAbilityScripts(float DeltaSeconds);

	// ---- 스크립트 객체 (컴포넌트 없이 스크립트 클래스의 인스턴스를 만든다 — Lua 비헤이비어 트리 노드용)
	// self.entity = Entity, self.Properties = 선언 기본값 + PropertyOverrides(JSON, FScriptComponent와 같은 형식).
	// 플레이 중이 아니거나 스크립트 로드에 실패하면 0. 핸들은 플레이 세션이 바뀌면 무효가 된다(호출해도 안전)
	FScriptObjectHandle CreateObject(const std::string& ScriptAsset, const std::string& PropertyOverrides, FEntity Entity);
	// self:Method(DeltaSeconds?) 호출. 메서드가 없으면 false + bOutFound = false, 오류면 false (그 객체는 멈춘다, 스크립트 저장 시 재개)
	bool CallObject(FScriptObjectHandle Handle, const char* Method, const float* DeltaSeconds, FScriptValue& OutResult, bool* bOutFound = nullptr);
	void DestroyObject(FScriptObjectHandle Handle);

	// ---- 스크립트 디버거 (Scripting/ScriptDebugger.h): 다음 BeginPlay의 Lua 상태에 연결한다 (비소유, 이 시스템보다 오래 산다).
	// 에디터만 연결한다 — 런타임/서버/테스트 기본은 없음 (중단점·오류 정지 없음, 훅 비용 없음)
	void             SetDebugger(FScriptDebugger* InDebugger) { Debugger = InDebugger; }
	FScriptDebugger* GetDebugger() const { return Debugger; }

	// ---- 핫 리로드: 변경된 .lua 파일 (절대 경로). 실패하면 기존 스크립트를 유지한다. 반환: 성공 여부
	bool ReloadScript(const std::filesystem::path& ScriptPath);

	// ---- 에디터: 스크립트가 선언한 Properties (이름순). 로드 실패 시 nullptr + OutError
	const std::vector<FScriptPropertyDecl>* GetPropertyDecls(const std::string& ScriptAsset, std::string* OutError = nullptr);

	// ---- 테스트/디버그
	// 받은 RPC 실행: Target 엔티티 스크립트의 MethodName(self, 인자...)을 부른다. 인스턴스/메서드가 없거나 오류면 false
	// (게임 모듈이 같은 RPC를 처리할 수 있으므로 메서드가 없어도 경고하지 않는다)
	bool InvokeMethod(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args);
	// MethodName을 정의한 모든 인스턴스에서 호출 (OnPlayerJoined 등 전역 이벤트). 정의하지 않은 인스턴스는 건너뛴다
	void BroadcastMethod(const std::string& MethodName, const FGameRpcArgs& Args);
	// InvokeMethod + 마지막 인자로 Fields를 담은 테이블 (예: OnCollisionBegin(other, info) — info.Point/Normal/Impulse)
	bool InvokeMethodWithFields(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args, const FScriptEventFields& Fields);
	// 엔티티(자식 포함) 지연 파괴 — Lua entity:Destroy()와 같은 경로 (스크립트 OnDestroy 후 다음 Update/LateUpdate 끝에 파괴).
	// 플레이 중이 아니면 false (호출한 쪽이 직접 파괴한다)
	bool RequestDestroy(FEntity Entity);

	// ---- 씬 사이에 남는 값 (Lua Game.SetPersistent/GetPersistent): 플레이 세션(Lua 상태)이 바뀌어도 유지된다 — 맵 전환으로 점수 등을 넘긴다.
	// 이 프로세스 메모리에만 있다 (복제·저장 안 함, 영구 저장은 SaveGame). 에디터는 플레이 정지 때 비운다
	FScriptValueMap& GetPersistentValues() { return PersistentValues; }
	void             ClearPersistentValues() { PersistentValues.clear(); }

	bool         RunString(std::string_view Code);                             // 플레이 상태에서 Lua 코드 실행 (오류는 로그 + false)
	size_t       GetInstanceCount() const;                                     // 살아 있는 스크립트 인스턴스 수
	FScriptValue GetInstanceProperty(FEntity Entity, const std::string& Name); // self.Properties[Name]
	uint32       GetErrorCount() const { return ErrorCount; }                  // 누적 스크립트 오류 수

private:
	std::filesystem::path        ContentDirectory;
	FScriptAudioHooks            AudioHooks;
	FScriptPhysicsHooks          PhysicsHooks;
	FScriptNetHooks              NetHooks;
	FScriptAIHooks               AIHooks;
	FScriptAppHooks              AppHooks;
	FScriptSteamHooks            SteamHooks;
	FScriptDebugDrawHooks        DebugDrawHooks;
	FAbilitySystem*              AbilitySystem = nullptr; // 비소유
	FScriptDebugger*             Debugger = nullptr; // 비소유 (에디터)
	std::unique_ptr<FLuaRuntime> PlayRuntime;  // 플레이 중에만 존재
	uint32                       PlaySession = 0; // BeginPlay마다 증가 (스크립트 객체 핸들 상위 32비트)
	std::unique_ptr<FLuaRuntime> EditorRuntime; // 프로퍼티 선언 조회용 (씬 없음, 게임 로직 실행 안 함)
	uint32                       ErrorCount = 0;
	FScriptValueMap              PersistentValues; // Game.SetPersistent (플레이 세션 사이 유지)
};
