#pragma once

#include "Scene/GameModule.h"

#include <filesystem>
#include <functional>
#include <string>

// 게임 모듈 DLL 로드/수명 관리.
//   Load:   DLL 로드 → 버전 확인 → 모듈 생성 → (등록 소유자 = 모듈 이름으로) OnLoad
//   Unload: OnUnload → 모듈이 등록한 리플렉션 타입 제거. DLL은 프로세스 종료까지 내리지 않는다
//           (씬의 게임 컴포넌트 풀 등이 모듈 코드를 참조할 수 있으므로)
//   Reload: 에디터 핫 리로드 (Editor/GameModuleHotReload — 그림자 복사본 로드, 씬 JSON 왕복은 에디터가 한다)
class FGameModuleHost
{
public:
	~FGameModuleHost();

	// 실행 파일 폴더의 <Name>.dll
	static std::filesystem::path GetDefaultModulePath(const std::string& Name);

	// 언로드 때 Scene 밖 모듈의 등록 정보(AI 비헤이비어 트리 노드 등)를 소유자(모듈 이름)로 정리하는 콜백. 프로세스 전역,
	// 해당 모듈의 Register*Types()가 한 번 추가한다. 리플렉션 타입 제거 뒤에 불린다
	using FOwnerCleanup = std::function<void(const std::string& Owner)>;
	static void AddUnloadCleanup(FOwnerCleanup Cleanup);

	// LogicalName: 모듈 이름(등록 소유자). 비우면 DLL 파일 이름 — 에디터 그림자 복사본(<이름>_<번호>.dll)은 원래 이름을 넘긴다
	bool Load(const std::filesystem::path& DllPath, std::string LogicalName = {});
	// DLL 없이 같은 프로세스의 모듈을 붙인다 (테스트/임베드용, 비소유 — Unload까지 살아 있어야 한다)
	void Attach(IGameModule& InModule, std::string InName);
	void Unload();
	// 핫 리로드 (에디터, 플레이 중 아님): OnUnload → 모듈 타입 제거 + ECS 타입 ID 폐기 + 소유자별 정리 → NewDllPath 로드 + OnLoad.
	// 이전 DLL은 FreeLibrary하지 않는다 (남은 함수 포인터·풀 가상 함수 안전 — 버전마다 메모리에 누적).
	// 새 DLL을 로드하지 못하면 이전 DLL을 다시 붙이고(OnLoad 다시) false
	bool Reload(const std::filesystem::path& NewDllPath);
	bool IsLoaded() const { return Module != nullptr; }
	const std::string&           GetName() const { return Name; }
	const std::filesystem::path& GetLoadedPath() const { return LoadedPath; } // 실제로 로드한 DLL (그림자 복사본이면 복사본)
	uint32                       GetReloadCount() const { return ReloadCount; }

	// 로드되지 않았으면 아무것도 하지 않는다
	void BeginPlay(FScene& Scene);
	void Update(FScene& Scene, float DeltaSeconds);
	void EndPlay(FScene& Scene);
	bool IsPlaying() const { return bPlaying; }

	// 멀티플레이 (플레이 중에만 전달). Net은 BeginPlay 전에 넘기고 EndPlay 뒤에 nullptr로 되돌린다
	void SetNet(IGameNet* Net);
	void SetPhysics(FPhysicsSystem* Physics); // Net과 같은 수명 (BeginPlay 전 ~ EndPlay 뒤 nullptr)
	void SetPhysics2D(FPhysics2DSystem* Physics2D); // 〃
	void PlayerJoined(FScene& Scene, uint32 PlayerId, FEntity Pawn);
	void PlayerLeft(FScene& Scene, uint32 PlayerId);
	void Rpc(FScene& Scene, FEntity Target, EGameRpcKind Kind, const std::string& RpcName, const FGameRpcArgs& Args);

	// 게임플레이 이벤트 (플레이 중에만 전달)
	void Damaged(FScene& Scene, FEntity Target, float Amount, FEntity Instigator);
	void Death(FScene& Scene, FEntity Target, FEntity Instigator);
	void Respawned(FScene& Scene, FEntity Target);

	// 물리 알림 (플레이 중에만). WantsCollisionEvents: 모듈이 없거나 플레이 중이 아니면 false
	bool WantsCollisionEvents(const FScene& Scene, FEntity Entity) const;
	void CollisionEvent(FScene& Scene, const FCollisionEvent& Event); // 종류별 OnCollisionBegin 등으로

	// 능력 시스템 (플레이 중에만). SetAbilities는 Net과 같은 수명
	void SetAbilities(FAbilitySystem* Abilities);
	void AbilityEvent(FScene& Scene, const FAbilityEvent& Event);

private:
	void UnloadInternal(bool bRetireComponentTypeIds);

	void*                 Library = nullptr; // HMODULE (공개 헤더에 Windows.h 금지)
	IGameModule*          Module  = nullptr; // 모듈 DLL 안의 정적 인스턴스 (비소유)
	std::string           Name;
	std::filesystem::path LoadedPath;
	uint32                ReloadCount = 0;
	bool                  bPlaying    = false;
};
