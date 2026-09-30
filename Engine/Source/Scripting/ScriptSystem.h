#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scripting/ScriptValue.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class FInput;
class FLuaRuntime;
class FScene;

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

// 스크립트가 쓰는 물리 기능. 앱이 Physics 모듈(FPhysicsSystem)과 연결한다 (비어 있으면 무시). 단위 cm, kg
struct FScriptPhysicsHooks
{
	std::function<bool(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit)> Raycast;
	std::function<void(FEntity, const FVector3&)> AddForce;    // kg·cm/s²
	std::function<void(FEntity, const FVector3&)> AddImpulse;  // kg·cm/s
	std::function<void(FEntity, const FVector3&)> SetVelocity; // cm/s
	std::function<FVector3(FEntity)>              GetVelocity;
	std::function<float(FEntity)>                 GetMass;     // kg (밀도 자동 계산 포함)
};

// RPC (entity:CallServer/CallClient/CallMulticast). 받는 쪽은 그 엔티티 스크립트의 Server_/Client_/Multicast_<이름> 메서드
enum class EScriptRpcKind : uint8
{
	Server,    // 클라이언트 → 서버 (소유자만). 서버에서 부르면 바로 로컬 호출
	Client,    // 서버 → 엔티티 소유 클라이언트
	Multicast, // 서버 → 서버 자신 + 모든 클라이언트
};

// RPC 인자: 스크립트 값(nil/bool/숫자/문자열/Vector3/에셋) 또는 엔티티
struct FScriptRpcArg
{
	FScriptValue Value;
	FEntity      Entity;
	bool         bIsEntity = false;
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
	// RPC 라우팅 (없으면 Standalone: 바로 로컬 호출). 잘못된 호출(클라이언트에서 CallClient 등)은 std::runtime_error로 알린다
	std::function<void(FEntity, EScriptRpcKind, const std::string&, const std::vector<FScriptRpcArg>&)> SendRpc;
};

// Lua 스크립트 컴포넌트(FScriptComponent) 실행 시스템.
//
// 스크립트 형식 (Unity 스타일 테이블 반환):
//   local Rotator = { Properties = { Speed = 90.0 } }  -- 기본값 선언 (인스펙터에 표시, 씬에서 덮어쓰기)
//   function Rotator:OnStart() end                     -- 첫 OnUpdate 직전 한 번
//   function Rotator:OnUpdate(dt) end                  -- 매 프레임
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
	// 모든 인스턴스 OnDestroy 후 Lua 상태 파괴
	void EndPlay();
	bool IsPlaying() const { return PlayRuntime != nullptr; }
	// 스크립트가 엔티티/컴포넌트를 추가·제거했는가 (호출 시 초기화). true면 앱이 에셋 참조(메시 등)를 다시 해석한다
	bool ConsumeSceneStructureChanged();

	// 다음 BeginPlay부터 적용 (플레이 중이면 즉시)
	void SetAudioHooks(FScriptAudioHooks Hooks);
	void SetPhysicsHooks(FScriptPhysicsHooks Hooks);
	void SetNetHooks(FScriptNetHooks Hooks);

	// ---- 핫 리로드: 변경된 .lua 파일 (절대 경로). 실패하면 기존 스크립트를 유지한다. 반환: 성공 여부
	bool ReloadScript(const std::filesystem::path& ScriptPath);

	// ---- 에디터: 스크립트가 선언한 Properties (이름순). 로드 실패 시 nullptr + OutError
	const std::vector<FScriptPropertyDecl>* GetPropertyDecls(const std::string& ScriptAsset, std::string* OutError = nullptr);

	// ---- 테스트/디버그
	// 받은 RPC 실행: Target 엔티티 스크립트의 MethodName(self, 인자...)을 부른다. 인스턴스/메서드가 없거나 오류면 false
	bool InvokeMethod(FEntity Target, const std::string& MethodName, const std::vector<FScriptRpcArg>& Args);

	bool         RunString(std::string_view Code);                             // 플레이 상태에서 Lua 코드 실행 (오류는 로그 + false)
	size_t       GetInstanceCount() const;                                     // 살아 있는 스크립트 인스턴스 수
	FScriptValue GetInstanceProperty(FEntity Entity, const std::string& Name); // self.Properties[Name]
	uint32       GetErrorCount() const { return ErrorCount; }                  // 누적 스크립트 오류 수

private:
	std::filesystem::path        ContentDirectory;
	FScriptAudioHooks            AudioHooks;
	FScriptPhysicsHooks          PhysicsHooks;
	FScriptNetHooks              NetHooks;
	std::unique_ptr<FLuaRuntime> PlayRuntime;  // 플레이 중에만 존재
	std::unique_ptr<FLuaRuntime> EditorRuntime; // 프로퍼티 선언 조회용 (씬 없음, 게임 로직 실행 안 함)
	uint32                       ErrorCount = 0;
};
