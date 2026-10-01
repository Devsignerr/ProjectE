#pragma once

#include "Core/CoreTypes.h"
#include "Scene/CollisionEvents.h"
#include "Scene/GameRpc.h"

class FScene;
struct FAnimNotifyEvent;

// 게임 모듈 인터페이스. 프로젝트의 C++ 게임 코드(<프로젝트>/Source → <이름>.dll)가 구현한다.
// 게임 모듈은 엔진 DLL(ProjectEEngine.dll)을 링크하고 같은 컴파일러/CRT로 빌드되어야 한다 (STL 객체가 경계를 넘는다).
//   OnLoad:      로드 직후 한 번. 게임 컴포넌트를 FTypeRegistry에 등록하면 인스펙터/직렬화/Lua에 자동 노출된다
//   OnBeginPlay: 게임 시작 (런타임 씬 로드 후, 에디터 플레이 시작 — 플레이 씬)
//   OnUpdate:    매 프레임 게임 C++ 시스템 (스크립트 뒤, 애니메이션/물리 앞). 대량 순회는 여기(C++)에 둔다
//   OnAnimNotify: 애니메이션 노티파이 (Scene/AnimNotify.h). 플레이 중, 발생 다음 프레임 OnUpdate 직전에 이벤트마다 한 번
//   OnEndPlay:   게임 종료 (에디터 정지 포함)
//   OnUnload:    언로드 직전 (등록 타입은 호스트가 제거한다)
// 멀티플레이: 게임 모듈은 서버(Standalone 포함)에서만 돈다 (클라이언트는 복제로 결과를 받는다).
//   OnPlayerJoined/OnPlayerLeft: 플레이어 입장(폰 생성 직후)/퇴장
//   OnRpc: 받은 RPC (Server RPC와 서버 자신의 Multicast). 같은 엔티티 스크립트의 Server_/Multicast_ 메서드와 함께 불린다
//   GetNet(): RPC 보내기/소유권 조회 (OnBeginPlay ~ OnEndPlay 동안 유효, 그 밖에서는 nullptr)
// 게임플레이 (Scene/Gameplay.h, 서버에서만): 데미지는 Gameplay::ApplyDamage(Scene, 대상, 양, 가해자)로 준다.
//   OnDamaged/OnDeath: 같은 프레임 게임플레이 단계(AI 뒤, 물리 앞)에서 대상 스크립트의 OnDamaged/OnDeath 다음에 불린다
//   OnRespawned: 리스폰 직후 (체력 회복 + 위치 이동 뒤)
class IGameModule
{
public:
	virtual ~IGameModule() = default;

	virtual void OnLoad() {}
	virtual void OnBeginPlay(FScene& /*Scene*/) {}
	virtual void OnUpdate(FScene& /*Scene*/, float /*DeltaSeconds*/) {}
	virtual void OnAnimNotify(FScene& /*Scene*/, const FAnimNotifyEvent& /*Event*/) {}
	virtual void OnEndPlay(FScene& /*Scene*/) {}
	virtual void OnUnload() {}

	virtual void OnPlayerJoined(FScene& /*Scene*/, uint32 /*PlayerId*/, FEntity /*Pawn*/) {}
	virtual void OnPlayerLeft(FScene& /*Scene*/, uint32 /*PlayerId*/) {}
	virtual void OnRpc(FScene& /*Scene*/, FEntity /*Target*/, EGameRpcKind /*Kind*/, const std::string& /*Name*/, const FGameRpcArgs& /*Args*/) {}

	virtual void OnDamaged(FScene& /*Scene*/, FEntity /*Target*/, float /*Amount*/, FEntity /*Instigator*/) {}
	virtual void OnDeath(FScene& /*Scene*/, FEntity /*Target*/, FEntity /*Instigator*/) {}
	virtual void OnRespawned(FScene& /*Scene*/, FEntity /*Target*/) {}

	IGameNet* GetNet() const { return Net; }
	void      SetNet(IGameNet* InNet) { Net = InNet; } // 엔진(FGameModuleHost)만 부른다

	// 물리 알림 (Scene/CollisionEvents.h, 규칙은 Physics/PhysicsSystem.h): 서버(Standalone 포함)에서 물리 스텝 뒤, 같은 엔티티 스크립트의
	// OnCollisionBegin 등 다음에 불린다. 쌍 하나는 양쪽 엔티티(Event.Self)로 한 번씩 온다.
	// 보고 대상: 트리거, RigidBody ReportContacts, 스크립트가 붙은 엔티티, 그리고 WantsCollisionEvents가 참인 엔티티 (플레이 중 매 프레임 묻는다)
	virtual bool WantsCollisionEvents(const FScene& /*Scene*/, FEntity /*Entity*/) const { return false; }
	virtual void OnCollisionBegin(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnCollisionEnd(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnTriggerEnter(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnTriggerExit(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}

private:
	IGameNet* Net = nullptr;
};

// 게임 모듈과 엔진이 약속한 인터페이스 버전 (IGameModule 가상 함수 구성이 바뀌면 올린다)
inline constexpr uint32 GameModuleApiVersion = 6; // 2: OnAnimNotify 추가, 3: 멀티플레이 (OnPlayerJoined/Left, OnRpc, GetNet), 4: IGameNet::GetInput (입력 액션), 5: 게임플레이 (OnDamaged/OnDeath/OnRespawned), 6: 물리 알림 (WantsCollisionEvents, OnCollisionBegin/End, OnTriggerEnter/Exit)

// 게임 모듈 .cpp 하나에 한 번: E_IMPLEMENT_GAME_MODULE(FMyGameModule)
#define E_IMPLEMENT_GAME_MODULE(ModuleClass)                                                   \
	extern "C" __declspec(dllexport) uint32 ProjectE_GetGameModuleApiVersion()               \
	{                                                                                          \
		return GameModuleApiVersion;                                                           \
	}                                                                                          \
	extern "C" __declspec(dllexport) IGameModule* ProjectE_CreateGameModule()                \
	{                                                                                          \
		static ModuleClass Instance;                                                           \
		return &Instance;                                                                      \
	}
