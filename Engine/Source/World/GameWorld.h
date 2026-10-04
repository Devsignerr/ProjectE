#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Input.h"
#include "Network/LanDiscovery.h"
#include "Network/NetTypes.h"
#include "Physics/CharacterMovement.h"
#include "Physics/CharacterMovement2D.h"
#include "Scene/GameRpc.h"
#include "Scene/Scene.h"

#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class FAISystem;
class FAbilitySystem;
class FGameModuleHost;
class FNetDriver;
class FPhysicsSystem;
class FPhysics2DSystem;
class FCharacterMovement2DSystem;
class FReplicationClient;
class FReplicationServer;
class FResourceManager;
struct FSceneDocument;
class FScene;
class FScriptSystem;

// FGameWorld가 돌리는 시스템. 모두 비소유 — 앱이 소유하고 FGameWorld보다 오래 산다
struct FGameWorldSystems
{
	FScriptSystem*        Scripts    = nullptr; // 필수
	FPhysicsSystem*       Physics    = nullptr; // 없으면 물리 없음
	FGameModuleHost*      GameModule = nullptr; // 없으면 게임 모듈 없음
	FResourceManager*     Resources  = nullptr; // 없으면 에셋 핸들 해석을 건너뛴다 (GPU 없는 서버)
	std::filesystem::path ContentDirectory;
	FNetDriver*           Net        = nullptr; // 없으면 로컬 플레이어 ID 0 (Standalone)
};

// 스크립트(Net.Host/Connect/Disconnect)가 요청한 세션 전환. 앱이 ConsumeSessionRequest로 꺼내 처리한다
struct FNetSessionRequest
{
	enum class EType : uint8
	{
		Host,       // 리슨 서버로 (Standalone이면 재시작 없이)
		Connect,    // Address 서버에 클라이언트로 (씬을 다시 연다)
		Disconnect, // Standalone으로 (씬을 다시 연다)
	};
	EType       Type = EType::Disconnect;
	std::string Address;
	uint16      Port = 0; // Host: 0이면 기본 포트
};

// 월드를 돌리는 쪽 (넷 모드에서 정해진다). Authority = 서버/Standalone(게임 로직 전부), Client = 네트워크 클라이언트
// (게임 로직은 서버가 돌리고 복제로 받는다. ClientOnly/Both 스크립트와 물리만 돌리며, 복제 엔티티의 동적 바디는 키네마틱으로 복제 트랜스폼을 따른다)
enum class EWorldRole : uint8
{
	Authority,
	Client,
};

// 게임 월드 한 프레임의 갱신 순서. 런타임, 에디터 플레이 모드, 전용 서버가 같은 순서를 쓴다.
//   게임플레이 틱 (플레이 중에만): 스크립트 → 시퀀스(컷신, 모든 역할 로컬) → (클라이언트) 물리 예측 → 캐릭터 이동 → 스크립트가 구조를 바꿨으면 에셋 해석 → 게임 모듈 → AI
//                                  시퀀스는 트랜스폼/프로퍼티/카메라 컷을 바로 쓰고 애니메이션 트랙은 클립 시각만 정한다 → 이번 프레임 표시 틱의
//                                  FAnimationSystem이 그 포즈를 쓴다. 시퀀스 이벤트는 다음 틱 스크립트 갱신에서 OnSequenceEvent_<이름>
//                                  → 게임플레이 규칙 → 사망 래그돌 켜기/끄기 → 물리 → UpdateTransforms → (클라이언트) 물리 예측 기록
//                                  게임플레이 규칙 (GameWorldGameplay.cpp, 서버): 데미지 이벤트 → OnDamaged/OnDeath, 사망 처리(점수/파괴/리스폰 예약),
//                                  리스폰, 매치 진행. 모든 역할: 매치 상태가 바뀌면 스크립트 OnMatchStateChanged(state)
//                                  (Client 역할: 게임 모듈·AI 없음 — 서버가 돌리고 복제로 받는다)
//   스크립트 ExecutionLocation 필터: Standalone/리슨 = 전부, 전용 서버 = ServerOnly/Both, 클라이언트 = ClientOnly/Both
//   입력: 클라이언트는 게임플레이 틱마다 로컬 입력 상태를 서버로 보낸다(비신뢰). 서버 스크립트의 Lua Input은
//         엔티티 소유 플레이어의 입력 (서버 소유/호스트 소유는 로컬 입력, 전용 서버의 서버 소유는 입력 없음).
//         입력 액션 값(Input.GetAction)도 함께 보낸다 — 클라이언트가 자기 바인딩으로 계산한 값. 게임 모듈은 IGameNet::GetInput
//                                  물리 → UpdateTransforms 뒤: 캐릭터 이동 상태 → 애니메이션 그래프 파라미터 (Speed/VerticalSpeed/Grounded)
//                                  → 충돌/트리거 알림 (GameWorldPhysicsEvents.cpp) → 스크립트 OnLateUpdate
//   표시 틱 (편집 중에도):         애니메이션 → 2D 플립북(트랜스폼 안 씀) → UpdateTransforms → 파티클 에셋 해석 → 파티클
// 시작/정지: BeginPlay = 물리 → 게임 모듈 → 스크립트(Lua 상태) → AI (Client 역할은 게임 모듈·AI 없음), EndPlay = 역순.
//   스크립트 OnStart는 첫 게임플레이 틱에 불리므로 AI(트리 시작)가 스크립트 뒤여도 OnStart가 블랙보드를 쓰기 전에 트리가 있다
// RPC(스크립트/게임 모듈 공용)와 입력·플레이어 이벤트는 GameWorldNet.cpp. 게임 모듈에는 IGameNet으로 자신을 넘긴다
// AI 시스템(비헤이비어 트리, 내비메시, 이동)은 FGameWorld가 소유한다 (앱마다 따로 둘 설정이 없다)
// 2D 물리(FPhysics2DSystem, Box2D — Physics/Physics2DSystem.h)도 같은 이유로 FGameWorld가 소유한다: 3D 물리 바로 뒤에 갱신하고
//   (Systems.Physics가 없어도 돈다), 보간 여부는 3D 시스템을 따르며(서버는 끔), 충돌 알림은 3D와 같은 전달 단계·같은 콜백으로 보낸다
class FGameWorld final : public IGameNet
{
public:
	FGameWorld();
	~FGameWorld();
	FGameWorld(const FGameWorld&)            = delete;
	FGameWorld& operator=(const FGameWorld&) = delete;

	// 스크립트 물리 훅(Physics.Raycast, entity:AddForce 등)도 여기서 연결한다 (네트워크 훅은 BeginPlay에서)
	void Init(const FGameWorldSystems& InSystems);

	// Mode: 이 프로세스가 맡을 넷 모드 (네트워크 연결 전에 시작하는 서버도 있으므로 드라이버 상태가 아니라 앱이 정한다)
	void       BeginPlay(FScene& InScene, ENetMode InMode = ENetMode::Standalone);
	void       EndPlay();
	bool       IsPlaying() const { return Scene != nullptr; }
	EWorldRole GetRole() const { return Mode == ENetMode::Client ? EWorldRole::Client : EWorldRole::Authority; }
	ENetMode   GetMode() const { return Mode; }

	// 게임플레이 한 프레임 (플레이 중이 아니면 무시). Input은 nullptr 허용 (UI가 입력을 가져간 경우)
	void TickGameplay(float DeltaSeconds, const FInput* Input);
	// 네트워크 드라이버를 나중에 연결/해제 (에디터 플레이: 네트워크 플레이를 시작할 때만 드라이버가 생긴다). BeginPlay 전에
	void SetNetDriver(FNetDriver* InNet) { Systems.Net = InNet; }
	// 클라이언트: 복제 클라이언트(스냅샷 버퍼) — 물리 예측이 서버 상태를 읽는다 (World/GameWorldPhysicsPrediction.cpp).
	// 없으면 물리 예측 없음 (복제 동적 바디는 키네마틱 보간). 앱이 클라이언트 BeginPlay 전에 연결하고, EndPlay가 비운다 (비소유)
	// 서브 씬 메시지 처리기도 여기서 연결한다 (서버가 불러온 서브 씬을 클라이언트가 곧바로 붙인다)
	void SetReplicationClient(FReplicationClient* InReplication);
	// 서버/Standalone: 복제 서버 (서브 씬 NetId·클라이언트 알림). 없으면 서브 씬은 로컬에만. 앱이 연결하고 해제한다 (EndPlay가 비우지 않음, 비소유)
	void SetReplicationServer(FReplicationServer* InReplication) { ReplicationServer = InReplication; }
	// 표시용 갱신. 플레이 여부와 무관하게 대상 씬을 갱신한다 (에디터는 편집 씬도)
	void TickPresentation(FScene& TargetScene, float DeltaSeconds);

	// FNetDriver::OnGameMessage에서 받은 월드 메시지(RPC, 입력). 처리했으면 true (복제 메시지는 복제 객체가 먼저 받는다)
	bool HandleNetMessage(FNetConnectionId Connection, const std::vector<uint8>& Message);

	// 서버: 플레이어 입장/퇴장 → 스크립트 OnPlayerJoined(playerId, pawn)/OnPlayerLeft(playerId) + 게임 모듈.
	// 이미 돌고 있는 스크립트만 받는다 (방금 생성된 폰의 스크립트는 다음 게임플레이 틱에 OnStart)
	void OnPlayerJoined(uint32 PlayerId, FEntity Pawn);
	void OnPlayerLeft(uint32 PlayerId);

	// 세션 전환 (로비): 스크립트 요청 꺼내기, 플레이 중 넷 모드 바꾸기 (Standalone ↔ 리슨 서버처럼 스크립트 필터가 같은 경우만)
	std::optional<FNetSessionRequest> ConsumeSessionRequest();
	void                              SetNetMode(ENetMode InMode);
	void                              SetLanDiscoveryPort(uint16 Port) { LanDiscoveryPort = Port; } // 테스트용

	// 맵 전환 요청 (Lua Game.OpenScene, 게임 모듈 IGameNet::OpenScene). 서버/Standalone에서만, 씬 파일(Content 기준)이 있어야 한다.
	// 앱이 프레임 끝에 FGameWorldTravel::ConsumePending → Travel로 처리한다 (갱신 도중 씬을 부수지 않는다). 실패하면 false + 사유
	bool                       RequestOpenScene(const std::string& SceneAsset, std::string* OutError = nullptr);
	std::optional<std::string> ConsumeOpenSceneRequest();
	// 지금 플레이 중인 씬 (Content 기준, Lua Game.GetCurrentScene). 앱이 씬을 열 때 정한다 (Travel은 자동)
	void               SetCurrentSceneAsset(std::string SceneAsset) { CurrentSceneAsset = std::move(SceneAsset); }
	const std::string& GetCurrentSceneAsset() const { return CurrentSceneAsset; }

	// 서브 씬 스트리밍 (World/GameWorldStreaming.cpp 머리 주석). 서버/Standalone에서만 요청 가능 (클라이언트는 서버를 따른다).
	// Load: 파일 읽기·파싱은 백그라운드, 붙이기는 다음 게임플레이 틱 처음(메인 스레드). 이미 있거나 불러오는 중이면 true
	// (UnloadSubScene/IsSubSceneLoaded는 IGameNet 구현과 같은 함수 — 아래). Unload: 불러온/불러오는 중이었으면 true (루트째 지연 파괴)
	bool    RequestLoadSubScene(const std::string& Asset, const FVector3& Offset, std::string* OutError = nullptr);
	FEntity GetSubSceneRoot(const std::string& Asset) const; // 붙기 전이면 NullEntity
	bool    bAsyncSubSceneLoad = true; // false = 파싱도 붙이는 틱에 메인 스레드에서 (기다림 없이 결정적 — 테스트/측정)
	struct FSubSceneStats
	{
		uint32 Loads = 0, Unloads = 0;
		float  LastParseMs = 0.0f;  // 파일 읽기 + JSON 파싱 (백그라운드 스레드)
		float  LastAttachMs = 0.0f; // 엔티티 생성 + 프리팹 동기화 + 에셋 해석 + 복제 등록 (메인 스레드 = 멈칫함)
		float  MaxAttachMs = 0.0f;
	};
	const FSubSceneStats& GetSubSceneStats() const { return SubSceneStats; }

	FScene*                  GetScene() const { return Scene; }
	const FGameWorldSystems& GetSystems() const { return Systems; }
	// 트리 블랙보드/이동 요청/내비메시 지정 (스크립트, 에디터 디버그 표시). 항상 유효
	FAISystem& GetAI() { return *AI; }
	// 능력 시스템 (Scene/Ability, World/GameWorldAbilities.cpp). 항상 유효 — 플레이 중에만 돈다
	FAbilitySystem& GetAbilities() { return *Abilities; }
	// 2D 물리 (Box2D). 항상 유효 — 플레이 중에만 돈다 (IsActive)
	FPhysics2DSystem& GetPhysics2D() { return *Physics2D; }
	// 2D 캐릭터 이동기 (Physics/CharacterMovement2DSystem.h, 예측은 World/GameWorldCharacter2D.cpp). 항상 유효 — 플레이 중에만 돈다
	FCharacterMovement2DSystem& GetCharacters2D() { return *Characters2D; }

	// ---- IGameNet (게임 모듈용)
	bool  IsServer() const override { return Mode != ENetMode::Client; }
	bool  IsClient() const override { return Mode != ENetMode::DedicatedServer; }
	int32 GetLocalPlayerId() const override;

	// 이 클라이언트가 직접 움직이는 엔티티인가 — 소유 클라이언트가 예측하는 캐릭터, 또는 물리 예측 중인 복제 바디
	// (복제 클라이언트는 이 엔티티의 스냅샷 트랜스폼을 쓰지 않는다 — FReplicationClient::SetTransformFilter)
	bool   IsPredicted(FEntity Entity) const;
	// 물리 예측 (클라이언트, World/GameWorldPhysicsPrediction.cpp 머리 주석): 예측 캐릭터 근처/접촉한 복제 동적 바디를 로컬에서 동적으로 시뮬레이션
	// 2D 동적 바디도 같은 규칙 (World/GameWorldPhysicsPrediction2D.cpp 머리 주석)
	bool   IsPhysicsPredicted(FEntity Entity) const { return PredictedBodies.contains(Entity) || PredictedBodies2D.contains(Entity); }
	bool   IsPhysicsSimulatedLocally(FEntity Entity) const; // 예측 중 + 동적 (해제 블렌드 중이면 false)
	bool   IsPhysics2DSimulatedLocally(FEntity Entity) const;
	uint32 GetPhysicsPredictedBodyCount() const { return static_cast<uint32>(PredictedBodies.size() + PredictedBodies2D.size()); }
	uint32 GetPhysics2DPredictedBodyCount() const { return static_cast<uint32>(PredictedBodies2D.size()); }
	// 예측 옵션: 캐릭터 이동 컴포넌트(3D 또는 2D) bClientPrediction && 프로젝트 설정 네트워크 → 클라이언트 예측
	bool   UsesClientPrediction(FEntity Entity) const;
	uint32 GetCharacterCorrectionCount() const { return CharacterCorrections; }
	uint32 GetReplayOverlapRejectCount2D() const { return PredictionStats2D.ReplayOverlapRejects; } // 2D 재조정 겹침 거부 (테스트/통계)
	int32 GetOwner(FEntity Entity) const override;
	void  CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args) override;
	const FInput* GetInput(FEntity Entity) const override { return ResolveInput(Entity, TickLocalInput); }
	bool          OpenScene(const std::string& SceneAsset) override;
	bool          LoadSubScene(const std::string& Asset, const FVector3& Offset) override;
	bool          UnloadSubScene(const std::string& Asset) override;
	bool          IsSubSceneLoaded(const std::string& Asset) const override;

private:
	// 잘못된 호출(클라이언트에서 Client/Multicast 등)은 std::runtime_error (Lua에서는 스크립트 오류가 된다)
	void          RouteRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args);
	void          InvokeRpcLocally(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args);
	void          ReceiveRpc(FNetConnectionId Connection, const std::vector<uint8>& Message);
	const FInput* ResolveInput(FEntity Entity, const FInput* LocalInput) const;
	void          SendLocalInput(const FInput& Input);
	void          ReceivePlayerInput(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void          InstallScriptNetHooks();
	// 스크립트 ↔ AI 훅 연결 (Lua 블랙보드/이동 API, Lua 비헤이비어 트리 노드)
	void          ConnectScriptsAndAI();

	struct FRemoteInput
	{
		FInput   Input;
		FVector2 ControlRotation; // yaw, pitch (도)
		uint32   LastSequence = 0;
		bool     bWarnedActionLayout = false; // 액션 목록 불일치 경고는 한 번만
	};
	const FInput* TickLocalInput = nullptr; // TickGameplay 동안만: 앱이 넘긴 로컬 입력 (게임 모듈 GetInput)
	FVector2 LocalControlRotation; // 로컬 플레이어 (Lua Net.SetControlRotation) — 클라이언트는 입력과 함께 보낸다

	struct FPredictedCharacter // 클라이언트: 서버가 아직 확인하지 않은 내 무브
	{
		std::deque<FCharacterMove> Moves;
		std::deque<std::pair<uint32, float>> MoveTimes; // 순번 → 그 무브를 시뮬레이션한 물리 예측 시계 (ack ↔ 스냅샷 시각 맞추기)
		uint32                     NextSequence    = 0;
		uint32                     LastAckSequence = 0;
		FVector3                   VisualOffset; // 보정으로 생긴 위치 차이를 화면에서만 천천히 흡수 (시뮬레이션은 즉시 보정)
	};
	struct FServerCharacter // 서버: 원격 플레이어 캐릭터
	{
		std::vector<FCharacterMove> Queue;            // 받았지만 아직 적용하지 않은 무브 (순번 순)
		uint32                      LastQueued   = 0; // 큐에 넣은 마지막 순번 (겹쳐 온 무브 거르기)
		uint32                      LastApplied  = 0;
	};
	std::unordered_map<FEntity, FPredictedCharacter> PredictedCharacters;
	std::unordered_map<FEntity, FServerCharacter>    ServerCharacters;
	uint32                                           CharacterCorrections = 0; // 재조정에서 1cm 넘게 고친 횟수 (테스트/통계)
	FVector2 GetControlRotation(FEntity Entity) const;

	// 캐릭터 이동 (FCharacterMovementComponent): 조종하는 쪽은 입력으로 시뮬레이션, 아니면 복제 트랜스폼을 따라간다
	// 클라이언트 예측 (World/GameWorldCharacter.cpp 머리 주석): 소유 클라이언트는 무브를 바로 적용·기록·전송하고, 서버 ack로 재조정한다
	void TickCharacters(float DeltaSeconds);
	void SendCharacterMoves(FEntity Entity);
	void ReceiveCharacterMoves(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void ReceiveCharacterAck(const std::vector<uint8>& Message);
	void SendCharacterAck(FEntity Entity, uint32 Sequence);
	bool IsLocallyControlled(FEntity Entity) const; // 이 프로세스가 조종: 소유 플레이어가 로컬이거나, 서버 소유(owner < 0)를 서버/Standalone이
	// 2D 캐릭터 이동기 (FCharacterMovement2DComponent, World/GameWorldCharacter2D.cpp 머리 주석): 3D와 같은 예측/재조정 구조의 2D 병렬 경로
	struct FPredictedCharacter2D
	{
		std::deque<FCharacterMove2D> Moves;
		std::deque<std::pair<uint32, float>> MoveTimes; // 순번 → 그 무브를 시뮬레이션한 물리 예측 시계 (3D와 같은 용도)
		uint32                       NextSequence    = 0;
		uint32                       LastAckSequence = 0;
		FVector2                     VisualOffset;
	};
	struct FServerCharacter2D
	{
		std::vector<FCharacterMove2D> Queue;
		uint32                        LastQueued  = 0;
		uint32                        LastApplied = 0;
	};
	std::unordered_map<FEntity, FPredictedCharacter2D> PredictedCharacters2D;
	std::unordered_map<FEntity, FServerCharacter2D>    ServerCharacters2D;
	void TickCharacters2D(float DeltaSeconds);
	void SendCharacterMoves2D(FEntity Entity);
	void ReceiveCharacterMoves2D(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void ReceiveCharacterAck2D(const std::vector<uint8>& Message);
	void SendCharacterAck2D(FEntity Entity, uint32 Sequence);
	bool DispatchCharacter2DEvents(); // 점프/착지/대시/밟기 → Lua OnJumped(n)/OnLanded()/OnDashStarted()/OnStomped(other)/OnStompedBy(other) + 게임 모듈 (충돌 알림 단계). 무엇이든 보냈으면 true
	// 캐릭터 이동 → 애니메이션 그래프 파라미터 (World/GameWorldAnimation.cpp, FAnimGraphComponent::bUseCharacterMovement). 물리·트랜스폼 갱신 뒤
	void UpdateCharacterAnimParams(float DeltaSeconds);
	// 발 IK 바닥 탐색 (Scene/AnimIK.h): 직전 애니메이션의 발 위치에서 FPhysicsSystem::Raycast → FFootIkComponent::Runtime. 물리·트랜스폼 갱신 뒤
	void UpdateFootIkProbes();

	// 물리 예측 (클라이언트, World/GameWorldPhysicsPrediction.cpp)
	struct FBodyHistorySample
	{
		float    Time = 0.0f; // 물리 예측 시계
		FVector3 Position;
		FQuat    Rotation;
	};
	struct FPredictedBody
	{
		bool     bBlendingOut = false; // 해제 중: 키네마틱으로 돌아가 화면을 스냅샷 보간 위치로 옮기는 중
		float    IdleSeconds  = 0.0f;  // 예측 캐릭터 근처/접촉이 없었던 시간
		float    BlendSeconds = 0.0f;
		FVector3 BlendFromPosition;
		FQuat    BlendFromRotation;
		float    LastSampleTime = -1.0f; // 처리한 마지막 스냅샷 서버 시각
		FVector3 PositionError;          // 아직 적용하지 않은 보정 (매 프레임 일부씩)
		FVector3 VelocityError;
		FVector3 AngularError;           // rad/s
		FQuat    RotationError;
		FVector3 ServerVelocity;         // 마지막 스냅샷 두 개의 차분
		std::deque<FBodyHistorySample> History; // 로컬 시뮬레이션 기록 (스텝 결과, 보정도 함께 옮긴다)
	};
	void TickPhysicsPrediction(float DeltaSeconds);   // 스크립트 뒤·캐릭터 이동 전: 대상 선정, 진입/해제, 서버 상태 수렴
	void RecordPhysicsPrediction();                   // 물리·UpdateTransforms 뒤: 기록 + (--net-physics-stats) 측정
	void UpdatePhysicsPredictionTiming();             // 새 스냅샷마다 로컬 시계 ↔ 서버 시각 오프셋
	void ProcessBodySnapshot(FEntity Entity, FPredictedBody& Body);
	void ApplyBodyCorrection(FEntity Entity, FPredictedBody& Body, float DeltaSeconds);
	void BeginBodyBlendOut(FEntity Entity, FPredictedBody& Body);
	// 캐릭터 재조정의 다시 적용: 무브를 처음 시뮬레이션한 시계(MoveTime) 직전 기록 위치로 예측 바디를 옮긴다 (충돌 질의용, 끝나면 되돌린다)
	void PoseBodiesForReplay(float MoveTime);
	void RestoreBodiesAfterReplay();
	bool IsPhysicsPredictionEnabled() const;
	bool IsPhysicsPredictionTimingEnabled() const; // 시각 맞추기 (3D·2D 공용): 클라이언트 + 스냅샷 버퍼 + 설정
	void CollectPredictionCharacters(std::vector<FEntity>& OutCharacters) const;
	void TickPhysicsPredictionStats();
	void LogPhysicsPredictionStats(const char* Label) const;
	FReplicationClient*                         Replication = nullptr;
	std::unordered_map<FEntity, FPredictedBody> PredictedBodies;
	float  PredictionClock        = 0.0f;  // 물리 예측 시계 (게임플레이 틱 dt 누적)
	float  PredictionTimeOffset   = 0.0f;  // 로컬 시계 = 서버 시각 + 오프셋 (서버가 이 스냅샷 상태를 만든 무브를 로컬이 시뮬레이션한 시각)
	bool   bPredictionTimingValid = false;
	float  LastAckMoveTime        = -1.0f; // 마지막 ack 무브를 시뮬레이션한 시계
	float  LastSnapshotTime       = -1.0f; // 처리한 가장 최근 스냅샷 서버 시각
	float  LastRecordTime         = 0.0f;
	struct FMotionTrack // 측정: 화면 위치의 프레임당 튐 = 등속 외삽과의 차이
	{
		FVector3 Previous, Current;
		float    PreviousDelta = 0.0f;
		uint32   Samples       = 0;
		float Push(const FVector3& Position, float DeltaSeconds); // 이번 튐 (cm, 첫 두 프레임은 0)
	};
	struct FBodyStats
	{
		FMotionTrack Track;
		float        ContactTime = -1.0f; // 내 캐릭터가 닿은(닿을 위치에 온) 시각
		float        MoveTime    = -1.0f; // 화면 위치가 멈춘 자리에서 움직이기 시작한 시각
		FVector3     RestPosition, LastPosition;
		bool         bInitialized = false;
		bool         bReacted     = false;
	};
	struct FPhysicsPredictionStats
	{
		bool   bEnabled = false;
		float  Elapsed = 0.0f, NextLog = 2.0f, LastDelta = 0.0f, InterpolationMargin = 0.0f; // 보간 여유 = 최근 스냅샷 - 보간 시각
		std::unordered_map<FEntity, FMotionTrack> Characters;
		std::unordered_map<FEntity, FBodyStats>   Bodies;
		float  CharacterMaxJump = 0.0f, BodyMaxJump = 0.0f, CorrectionMax = 0.0f;
		uint32 CharacterJumpFrames = 0, BodyJumpFrames = 0, Frames = 0, Snaps = 0, BigCorrections = 0; // BigCorrections: 5cm 초과 캐릭터 보정
		std::vector<float> ReactionDelays;
	};
	FPhysicsPredictionStats PredictionStats;
	// 2D 측정 (--net-physics-stats, 3D와 같은 항목을 2D 캐릭터·2D 바디로 — 따로 한 줄). 보정·재조정 겹침 거부 횟수는 측정을 꺼도 센다
	struct FPhysicsPredictionStats2D
	{
		float  Elapsed = 0.0f, NextLog = 2.0f;
		std::unordered_map<FEntity, FMotionTrack> Characters;
		std::unordered_map<FEntity, FBodyStats>   Bodies;
		float  CharacterMaxJump = 0.0f, BodyMaxJump = 0.0f, CorrectionMax = 0.0f;
		uint32 CharacterJumpFrames = 0, BodyJumpFrames = 0, Frames = 0, Snaps = 0, Corrections = 0, BigCorrections = 0;
		uint32 ReplayOverlapRejects = 0; // 재조정 ②(기록 위치 다시 적용) 결과가 지금 바디와 겹쳐 버린 횟수
		std::vector<float> ReactionDelays;
	};
	FPhysicsPredictionStats2D PredictionStats2D;
	void TickPhysicsPredictionStats2D();
	void LogPhysicsPredictionStats2D(const char* Label) const;

	// 2D 물리 예측 (클라이언트, World/GameWorldPhysicsPrediction2D.cpp — 3D와 같은 규칙·상수·시계, 평면 상태)
	struct FBodyHistorySample2D
	{
		float    Time = 0.0f;
		FVector2 Position; // 평면 cm
		float    Angle = 0.0f; // 라디안 반시계 +
	};
	struct FPredictedBody2D
	{
		bool     bBlendingOut = false;
		float    IdleSeconds  = 0.0f;
		float    BlendSeconds = 0.0f;
		FVector3 BlendFromPosition;
		FQuat    BlendFromRotation;
		float    LastSampleTime = -1.0f;
		FVector2 PositionError;
		FVector2 VelocityError;
		float    AngleError        = 0.0f;
		float    AngularSpeedError = 0.0f; // rad/s
		FVector2 ServerVelocity;
		std::deque<FBodyHistorySample2D> History;
	};
	void TickPhysicsPrediction2D(float DeltaSeconds);
	void RecordPhysicsPrediction2D();
	bool IsPhysicsPrediction2DEnabled() const;
	void ProcessBodySnapshot2D(FEntity Entity, FPredictedBody2D& Body);
	void ApplyBodyCorrection2D(FEntity Entity, FPredictedBody2D& Body, float DeltaSeconds);
	void BeginBodyBlendOut2D(FEntity Entity, FPredictedBody2D& Body);
	void PoseBodiesForReplay2D(float MoveTime);
	void RestoreBodiesAfterReplay2D();
	std::unordered_map<FEntity, FPredictedBody2D> PredictedBodies2D;

	// 게임플레이 규칙 (Scene/Gameplay.h 체력·게임 모드, World/GameWorldGameplay.cpp)
	void  TickGameplayRules(float DeltaSeconds);
	void  DispatchDamageEvents();
	void  HandleDeath(FEntity Victim, FEntity Instigator);
	void  Respawn(FEntity Entity);
	int32 GetScoringPlayer(FEntity Instigator, FEntity Victim) const; // 처치 점수를 받을 플레이어 (-1 = 없음)
	int32  LastMatchState    = -1; // OnMatchStateChanged 감지 (-1 = 게임 모드 없음/시작 전)
	uint32 RespawnStartIndex = 0;  // 리스폰 PlayerStart 순번

	// 물리 알림 (World/GameWorldPhysicsEvents.cpp): 물리·UpdateTransforms 뒤 충돌/트리거 이벤트 → 스크립트 + 게임 모듈
	bool DispatchCollisionEvents(); // 이벤트가 있었으면 true (스크립트·게임 모듈이 씬을 바꿨을 수 있음)
	bool ShouldReportContacts(const FScene& Target, FEntity Entity) const; // FPhysicsSystem 보고 필터 (역할 규칙 포함)

	// 능력 시스템 (World/GameWorldAbilities.cpp): 스크립트 갱신 뒤·캐릭터 이동 전. 입력 발동 → FAbilitySystem::Tick → MoveSpeed → 이벤트
	void ConnectAbilities();
	void TickAbilities(float DeltaSeconds);
	void DispatchAbilityEvents();
	void ReceiveAbilityActivate(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void ReceiveAbilityResult(const std::vector<uint8>& Message);
	std::unique_ptr<FAbilitySystem> Abilities;

	// 사망 래그돌 (World/GameWorldRagdoll.cpp): FRagdollComponent bEnableOnDeath 모델의 체력 변화 → 켜기/끄기
	void TickRagdolls();
	std::unordered_map<FEntity, bool> RagdollDeadStates; // 모델 → 지난 틱에 죽어 있었나

	FGameWorldSystems          Systems;
	std::unique_ptr<FAISystem> AI;
	std::unique_ptr<FPhysics2DSystem> Physics2D;
	std::unique_ptr<FCharacterMovement2DSystem> Characters2D;
	void InstallScriptPhysicsHooks(); // 3D(Systems.Physics, 없으면 무시) + 2D 물리 → 스크립트 (Init)
	FScene*           Scene = nullptr; // 플레이 중인 씬 (비소유, BeginPlay~EndPlay)
	// 표시 틱 부분 트랜스폼 갱신: 게임플레이 틱이 이 씬의 트랜스폼을 전체 갱신한 직후면 그 씬 (표시 틱이 소비). 사이에 다른 코드가
	// 트랜스폼을 쓰지 않는 앱 순서(TickGameplay → TickPresentation)에서만 맞으므로 BeginPlay/EndPlay와 다음 표시 틱이 비운다
	FScene*              GameplayValidatedScene = nullptr;
	std::vector<FTransformChangedSubtree> PresentationWritten; // 표시 틱이 로컬 트랜스폼을 쓴 엔티티 (태양 + 평가한 애니메이션 모델)
	ENetMode          Mode  = ENetMode::Standalone;

	std::unordered_map<uint32, FRemoteInput> RemoteInputs;      // 서버: 플레이어 ID → 받은 입력
	uint32                                   InputSequence = 0; // 클라이언트: 보낸 입력 순번

	FLanDiscovery                     SessionSearch; // Net.FindSessions
	uint16                            LanDiscoveryPort = 0; // 0 = 프로젝트 설정
	std::optional<FNetSessionRequest> PendingSessionRequest;

	std::optional<std::string> PendingSceneRequest; // 맵 전환 (프레임 끝에 앱이 처리)
	std::string                CurrentSceneAsset;

	// 서브 씬 (World/GameWorldStreaming.cpp)
	struct FParsedSubScene // 백그라운드 스레드 결과 (future 완료 후에만 읽는다)
	{
		std::shared_ptr<const FSceneDocument> Document;
		std::string                           Error;
		float                                 ParseMs = 0.0f;
	};
	struct FSubSceneInstance
	{
		std::string Asset;
		uint32      InstanceId = 0;
		FVector3    Offset;
		FEntity     Root;              // 붙기 전 NullEntity
		bool        bByScript = false; // 스크립트/게임 모듈 요청 (볼륨이 내리지 않는다)
		bool        bByVolume = false;
		std::future<FParsedSubScene> Pending; // 백그라운드 파싱 (붙이면 비운다)
	};
	void               TickSubScenes();      // 게임플레이 틱 처음: 파싱 끝난 것 붙이기 + 볼륨 판정 (서버/Standalone)
	void               UpdateStreamingVolumes();
	bool               StartSubSceneLoad(const std::string& Asset, const FVector3& Offset, bool bByScript, std::string* OutError);
	FEntity            AttachSubScene(FSubSceneInstance& Instance, const FSceneDocument& Document);
	void               DestroySubScene(FSubSceneInstance& Instance);
	FEntity            ClientLoadSubScene(const std::string& Asset, uint32 InstanceId, const FVector3& Offset); // 서버 지시 (동기)
	void               ClientUnloadSubScene(uint32 InstanceId);
	FSubSceneInstance* FindSubScene(const std::string& Asset);
	const FSubSceneInstance* FindSubScene(const std::string& Asset) const;
	void               ClearSubScenes();
	std::vector<std::unique_ptr<FSubSceneInstance>> SubScenes;
	uint32                                          NextSubSceneId = 1;
	FSubSceneStats                                  SubSceneStats;
	FReplicationServer*                             ReplicationServer = nullptr;
};
