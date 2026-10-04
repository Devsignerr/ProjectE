#pragma once

#include "Core/CoreTypes.h"
#include "Scene/CollisionEvents.h"
#include "Scene/GameRpc.h"

class FScene;
class FPhysicsSystem;
class FPhysics2DSystem;
class FCharacterMovement2DSystem;
struct FCharacterMove2DEvents;
class FAbilitySystem;
struct FAnimNotifyEvent;
struct FAbilityEvent;

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
	// 2D 플립북 (Scene/Sprite/FlipbookSystem.h): 플레이 중, 발생 다음 프레임 OnUpdate 직전 (노티파이 다음). Entity = 플립북 엔티티
	virtual void OnFlipbookEvent(FScene& /*Scene*/, FEntity /*Entity*/, const std::string& /*Name*/, int32 /*Frame*/) {}
	virtual void OnFlipbookFinished(FScene& /*Scene*/, FEntity /*Entity*/) {} // Once 재생이 끝에 닿은 순간 한 번
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
	// 물리 (Physics/PhysicsSystem.h를 포함해 쓴다): 레이캐스트, 겹침 검사/쓸어 보기(Overlap*/Sweep/*Cast), 힘/속도.
	// GetNet()과 같은 수명 (OnBeginPlay ~ OnEndPlay, 물리 없는 앱이면 nullptr)
	FPhysicsSystem* GetPhysics() const { return Physics; }
	void            SetPhysics(FPhysicsSystem* InPhysics) { Physics = InPhysics; } // 엔진(FGameModuleHost)만 부른다
	// 2D 물리 (Physics/Physics2DSystem.h, Box2D — 평면 X·Z): 레이캐스트/겹침, 힘/속도. GetNet()과 같은 수명. 충돌 알림은 아래 3D와 같은 콜백으로 온다
	FPhysics2DSystem* GetPhysics2D() const { return Physics2D; }
	void              SetPhysics2D(FPhysics2DSystem* InPhysics2D) { Physics2D = InPhysics2D; } // 엔진(FGameModuleHost)만 부른다
	// 2D 캐릭터 이동기 (Physics/CharacterMovement2DSystem.h): 입력(AddMovementInput/Jump/StopJumping/Dash/DropDown — 조종하는 쪽에서)과
	// 상태(IsGrounded/GetVelocity/GetJumpsRemaining …). GetNet()과 같은 수명
	FCharacterMovement2DSystem* GetCharacters2D() const { return Characters2D; }
	void                        SetCharacters2D(FCharacterMovement2DSystem* InCharacters) { Characters2D = InCharacters; } // 엔진(FGameModuleHost)만 부른다
	// 2D 캐릭터 이동 이벤트 (점프/착지/대시 시작 — 서버가 시뮬레이션한 무브): 같은 엔티티 스크립트의 OnJumped/OnLanded/OnDashStarted 다음
	virtual void OnCharacter2DEvent(FScene& /*Scene*/, FEntity /*Entity*/, const FCharacterMove2DEvents& /*Events*/) {}

	// 물리 알림 (Scene/CollisionEvents.h, 규칙은 Physics/PhysicsSystem.h): 서버(Standalone 포함)에서 물리 스텝 뒤, 같은 엔티티 스크립트의
	// OnCollisionBegin 등 다음에 불린다. 쌍 하나는 양쪽 엔티티(Event.Self)로 한 번씩 온다.
	// 보고 대상: 트리거, RigidBody ReportContacts, 스크립트가 붙은 엔티티, 그리고 WantsCollisionEvents가 참인 엔티티 (플레이 중 매 프레임 묻는다)
	virtual bool WantsCollisionEvents(const FScene& /*Scene*/, FEntity /*Entity*/) const { return false; }
	virtual void OnCollisionBegin(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnCollisionEnd(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnTriggerEnter(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	virtual void OnTriggerExit(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}
	// 관절이 끊어짐 (Event.Self = 관절 엔티티, Other = 대상, Impulse = 끊은 힘 N). 보고 대상과 무관하게 모든 끊어짐
	virtual void OnJointBreak(FScene& /*Scene*/, const FCollisionEvent& /*Event*/) {}

	// 능력 시스템 (Scene/Ability/AbilitySystem.h, 서버에서만): 같은 틱 능력 단계 끝에 이벤트마다 (속성/태그 변화, 능력 발동/종료/실패).
	// 같은 엔티티 스크립트의 OnAttributeChanged 등 다음에 불린다. C++ 능력은 OnLoad에서 FAbilityNativeRegistry::Get().Register(이름, ...)
	virtual void OnAbilityEvent(FScene& /*Scene*/, const FAbilityEvent& /*Event*/) {}
	// 능력 시스템 (발동/효과/속성/태그 API). GetNet()과 같은 수명
	FAbilitySystem* GetAbilities() const { return Abilities; }
	void            SetAbilities(FAbilitySystem* InAbilities) { Abilities = InAbilities; } // 엔진(FGameModuleHost)만 부른다

private:
	IGameNet*       Net     = nullptr;
	FPhysicsSystem* Physics = nullptr;
	FAbilitySystem* Abilities = nullptr;
	FPhysics2DSystem* Physics2D = nullptr;
	FCharacterMovement2DSystem* Characters2D = nullptr;
};

// 게임 모듈과 엔진이 약속한 인터페이스 버전 (IGameModule 가상 함수 구성이 바뀌면 올린다)
inline constexpr uint32 GameModuleApiVersion = 13; // 2: OnAnimNotify 추가, 3: 멀티플레이 (OnPlayerJoined/Left, OnRpc, GetNet), 4: IGameNet::GetInput (입력 액션), 5: 게임플레이 (OnDamaged/OnDeath/OnRespawned), 6: IGameNet::OpenScene (맵 전환), 7: IGameNet 서브 씬 (Load/Unload/IsSubSceneLoaded), 8: 물리 알림 (WantsCollisionEvents, OnCollisionBegin/End, OnTriggerEnter/Exit, OnJointBreak), 9: GetPhysics (모양 질의) + FAnimationRuntime 구조 변경 (몽타주/IK/노티파이 트랙), 10: 능력 시스템 (OnAbilityEvent, GetAbilities), 11: GetPhysics2D (2D 물리), 12: OnFlipbookEvent/OnFlipbookFinished (2D 플립북), 13: GetCharacters2D/OnCharacter2DEvent (2D 캐릭터 이동기)

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
