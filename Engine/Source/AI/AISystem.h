#pragma once

#include "AI/Navigation/NavMesh.h"
#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Vector3.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FBehaviorTreeAsset;
class FBehaviorTreeInstance;
class FScene;

enum class EAIMoveStatus : uint8
{
	Idle,      // 이동 요청 없음 (또는 StopMove)
	Moving,
	Succeeded, // 목표 AcceptanceRadius 안에 도착
	Failed,    // 경로 없음, 또는 부분 경로 끝에서 목표에 닿지 못함
};

// 이동을 물리에 맡기는 훅 (AI는 Physics에 비의존 — FGameWorld가 연결한다)
struct FAIMovementHooks
{
	// 엔티티가 동적 강체면 원하는 수평 속도(cm/s, Z = 0)를 적용하고 true. false면 AI가 트랜스폼을 직접 옮긴다
	std::function<bool(FEntity, const FVector3& DesiredVelocity)> ApplyVelocity;
};

// Lua 스크립트 메서드 호출 결과 (Lua 노드가 상태로 바꾼다)
enum class EAIScriptResult : uint8
{
	NotFound, // 객체 없음/메서드 없음
	Error,    // 스크립트 오류 (그 객체는 멈춘다)
	Nil,
	True,
	False,
	Running,  // 문자열 "Running"/"Success"/"Failure" (대소문자 무시)
	Success,
	Failure,
	Other,    // 그 밖의 값
};

// Lua 비헤이비어 트리 노드용 스크립트 연결 (AI는 Scripting에 비의존 — FGameWorld가 FScriptSystem 스크립트 객체와 연결한다)
struct FAIScriptHooks
{
	// self.entity = Self, self.Properties = 선언 기본값 + Properties(JSON). 0 = 실패 (플레이 중 아님, 로드 오류)
	std::function<uint64(const std::string& Script, const std::string& Properties, FEntity Self)> CreateObject;
	std::function<EAIScriptResult(uint64 Handle, const char* Method, const float* DeltaSeconds)>  Call;
	std::function<void(uint64 Handle)>                                                          DestroyObject;
};

// AI 실행: FBehaviorTreeComponent 엔티티마다 트리 인스턴스를 만들어 틱하고, MoveTo 요청을 내비메시 경로로 수행한다.
// 서버/Standalone에서만 돈다 (FGameWorld가 Client 역할이면 부르지 않는다). 실행 상태는 모두 여기 있고 컴포넌트에는 없다.
// 순서 (FGameWorld): 스크립트 → 게임 모듈 → AI(Update: 트리 틱 → 이동) → 물리 → UpdateTransforms
class FAISystem
{
public:
	FAISystem();
	~FAISystem();
	FAISystem(const FAISystem&)            = delete;
	FAISystem& operator=(const FAISystem&) = delete;

	void SetContentDirectory(const std::filesystem::path& Directory) { ContentDirectory = Directory; }
	void SetMovementHooks(FAIMovementHooks Hooks) { MovementHooks = std::move(Hooks); }
	void SetScriptHooks(FAIScriptHooks Hooks) { ScriptHooks = std::move(Hooks); }
	// Lua 노드가 쓴다 (FAISystem 수명 동안 유효)
	const FAIScriptHooks& GetScriptHooks() const { return ScriptHooks; }

	// 내비메시(FNavMeshComponent.NavMeshAsset)를 읽고 자동 시작 트리를 만든다. 이미 활성이면 End 후 다시 시작
	void Begin(FScene& InScene);
	// 새/바뀐 컴포넌트 반영 → 트리 틱 → 이동. Begin 전이면 무시
	void Update(FScene& InScene, float DeltaSeconds);
	// 모든 트리를 멈추고 상태를 비운다 (내비메시 포함). 소멸자도 부르므로 Begin에 준 씬은 End(또는 파괴)까지 살아 있어야 한다
	void End();
	bool IsActive() const { return Scene != nullptr; }

	// ---- 비헤이비어 트리
	// 엔티티의 트리 인스턴스 (없으면 nullptr). 블랙보드 접근용
	FBehaviorTreeInstance* FindTree(FEntity Entity);
	// FBehaviorTreeComponent.Asset으로 트리를 만들어 시작한다 (bAutoStart = false용, 이미 있으면 재시작). 실패하면 false
	bool StartTree(FEntity Entity);
	void StopTree(FEntity Entity);
	// 에셋 파일이 바뀌었을 때 (Content 기준 경로): 캐시를 비우고 그 에셋을 쓰는 트리를 다시 만들어 시작한다
	void ReloadBehaviorTree(const std::string& AssetPath);

	// ---- 내비게이션
	const FNavMesh* GetNavMesh() const { return NavMesh.IsValid() ? &NavMesh : nullptr; }
	// 구운 내비메시를 직접 지정 (에디터 굽기, 테스트). 이미 진행 중인 이동은 이전 경로를 따른다 — Begin 전에 지정하는 것이 좋다.
	// Begin은 FNavMeshComponent 파일이 있으면 그것으로 바꾸고, End는 비운다
	void SetNavMesh(FNavMesh&& InNavMesh) { NavMesh = std::move(InNavMesh); }
	// 내비메시 경로 (없으면 [Start, End] 직선). Failed면 false
	bool FindPath(const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints) const;

	// ---- 이동 (엔티티당 하나. 새 요청은 이전 요청을 대체한다)
	// AcceptanceRadius < 0이면 FNavAgentComponent 값. 이미 도착했으면 바로 Succeeded
	EAIMoveStatus RequestMove(FEntity Entity, const FVector3& Goal, float AcceptanceRadius = -1.0f);
	EAIMoveStatus GetMoveStatus(FEntity Entity) const;
	void          StopMove(FEntity Entity);
	// 현재 경로 (디버그 표시용, 없으면 nullptr)
	const std::vector<FVector3>* GetMovePath(FEntity Entity) const;

	// Entity를 Point 쪽(수평)으로 최대 MaxDegrees만큼 돌린다. 방향이 ToleranceDegrees 안이면 true (RotateTo, 이동 방향 회전 공용)
	static bool TurnTowards(FScene& TargetScene, FEntity Entity, const FVector3& Point, float MaxDegrees, float ToleranceDegrees);

private:
	struct FMoveState
	{
		EAIMoveStatus         Status = EAIMoveStatus::Idle;
		std::vector<FVector3> Path;
		size_t                NextPoint        = 0;
		FVector3              Goal             = FVector3::ZeroVector;
		float                 AcceptanceRadius = 20.0f;
		bool                  bPhysicsDriven   = false; // 직전 이동을 물리 훅이 맡았는지 (멈출 때 속도 0 전달)
	};

	struct FAgent
	{
		std::unique_ptr<FBehaviorTreeInstance> Tree;
		std::string                            TreeAsset; // 트리를 만든 에셋 경로 (컴포넌트 값이 바뀌면 다시 만든다)
		bool                                   bTreeFailed = false; // 로드 실패 (같은 경로로 매 프레임 다시 시도하지 않음)
		FMoveState                             Move;
	};

	const FBehaviorTreeAsset* LoadAsset(const std::string& AssetPath);
	bool                      CreateTree(FEntity Entity, FAgent& Agent, const std::string& AssetPath);
	void                      SyncComponents();
	void                      UpdateMovement(FEntity Entity, FMoveState& Move, float DeltaSeconds);
	void                      FinishMove(FEntity Entity, FMoveState& Move, EAIMoveStatus Status);
	void                      MoveEntityTo(FEntity Entity, const FVector3& WorldPosition);

	FScene*                                                            Scene = nullptr; // 비소유 (Begin~End)
	std::filesystem::path                                              ContentDirectory;
	FAIMovementHooks                                                   MovementHooks;
	FAIScriptHooks                                                     ScriptHooks;
	FNavMesh                                                           NavMesh;
	std::unordered_map<uint64, FAgent>                                 Agents; // FEntity::ToId()
	std::unordered_map<std::string, std::unique_ptr<FBehaviorTreeAsset>> AssetCache; // 실패한 경로는 nullptr
	bool                                                               bWarnedNoNavMesh = false;
};
