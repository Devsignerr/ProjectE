#pragma once

#include "Scene/GameModule.h"

#include <filesystem>
#include <functional>
#include <string>

// 게임 모듈 DLL 로드/수명 관리.
//   Load:   DLL 로드 → 버전 확인 → 모듈 생성 → (등록 소유자 = 모듈 이름으로) OnLoad
//   Unload: OnUnload → 모듈이 등록한 리플렉션 타입 제거. DLL은 프로세스 종료까지 내리지 않는다
//           (씬의 게임 컴포넌트 풀 등이 모듈 코드를 참조할 수 있으므로. 핫 리로드는 후속)
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

	bool Load(const std::filesystem::path& DllPath);
	// DLL 없이 같은 프로세스의 모듈을 붙인다 (테스트/임베드용, 비소유 — Unload까지 살아 있어야 한다)
	void Attach(IGameModule& InModule, std::string InName);
	void Unload();
	bool IsLoaded() const { return Module != nullptr; }
	const std::string& GetName() const { return Name; }

	// 로드되지 않았으면 아무것도 하지 않는다
	void BeginPlay(FScene& Scene);
	void Update(FScene& Scene, float DeltaSeconds);
	void EndPlay(FScene& Scene);
	bool IsPlaying() const { return bPlaying; }

	// 멀티플레이 (플레이 중에만 전달). Net은 BeginPlay 전에 넘기고 EndPlay 뒤에 nullptr로 되돌린다
	void SetNet(IGameNet* Net);
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

private:
	void*        Library = nullptr; // HMODULE (공개 헤더에 Windows.h 금지)
	IGameModule* Module  = nullptr; // 모듈 DLL 안의 정적 인스턴스 (비소유)
	std::string  Name;
	bool         bPlaying = false;
};
