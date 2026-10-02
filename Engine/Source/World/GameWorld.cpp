#include "World/GameWorld.h"

#include "Core/Profiling.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/Assert.h"
#include "Core/CommandLine.h"
#include "Network/ReplicationTypes.h"
#include "Online/SteamSubsystem.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/DebugDraw.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scene/SequencePlayer.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>
#include <variant>

namespace
{
	FPhysicsQueryShape ToPhysicsQueryShape(const FScriptQueryShape& Shape)
	{
		switch (Shape.Shape)
		{
		case EScriptQueryShape::Box:     return FPhysicsQueryShape::MakeBox(Shape.HalfExtents);
		case EScriptQueryShape::Capsule: return FPhysicsQueryShape::MakeCapsule(Shape.Radius, Shape.HalfHeight);
		default:                         return FPhysicsQueryShape::MakeSphere(Shape.Radius);
		}
	}

	const char* ToString(EAIMoveStatus Status)
	{
		switch (Status)
		{
		case EAIMoveStatus::Moving:    return "Moving";
		case EAIMoveStatus::Succeeded: return "Succeeded";
		case EAIMoveStatus::Failed:    return "Failed";
		default:                       return "Idle";
		}
	}

	// 스크립트 값 → 블랙보드 키 타입 값 (숫자는 Int/Float 키 모두 허용). 맞지 않으면 false
	bool ToBlackboardValue(const FScriptValue& Value, EBlackboardKeyType Type, FBlackboardValue& OutValue)
	{
		switch (Type)
		{
		case EBlackboardKeyType::Bool:   if (Value.Type != EScriptValueType::Bool) return false; OutValue = Value.bBool; return true;
		case EBlackboardKeyType::Int:    if (Value.Type != EScriptValueType::Number) return false; OutValue = static_cast<int32>(std::llround(Value.Number)); return true;
		case EBlackboardKeyType::Float:  if (Value.Type != EScriptValueType::Number) return false; OutValue = static_cast<float>(Value.Number); return true;
		case EBlackboardKeyType::Vector: if (Value.Type != EScriptValueType::Vector3) return false; OutValue = Value.Vector; return true;
		case EBlackboardKeyType::String: if (Value.Type != EScriptValueType::String) return false; OutValue = Value.String; return true;
		default:                         return false; // Entity는 SetBlackboardEntity
		}
	}

	EAIScriptResult ToScriptResult(const FScriptValue& Value)
	{
		switch (Value.Type)
		{
		case EScriptValueType::Nil:  return EAIScriptResult::Nil;
		case EScriptValueType::Bool: return Value.bBool ? EAIScriptResult::True : EAIScriptResult::False;
		case EScriptValueType::String:
		{
			std::string Lower = Value.String;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](char Char) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Char))); });
			if (Lower == "running") return EAIScriptResult::Running;
			if (Lower == "success") return EAIScriptResult::Success;
			if (Lower == "failure") return EAIScriptResult::Failure;
			return EAIScriptResult::Other;
		}
		default: return EAIScriptResult::Other;
		}
	}
} // namespace

FGameWorld::FGameWorld() : AI(std::make_unique<FAISystem>()) {}
FGameWorld::~FGameWorld() = default;

void FGameWorld::ConnectScriptsAndAI()
{
	FScriptSystem* Scripts = Systems.Scripts;
	FAISystem*     AISys   = AI.get();

	// Lua → AI: 블랙보드, 이동, 경로, 트리 제어
	FScriptAIHooks AIHooks;
	AIHooks.GetBlackboard = [AISys](FEntity Entity, const std::string& Key, FScriptValue& OutValue, FEntity& OutEntity, bool& bOutIsEntity) {
		const FBehaviorTreeInstance* Tree  = AISys->FindTree(Entity);
		const FBlackboardValue*      Value = Tree ? Tree->GetBlackboard().GetValue(Key) : nullptr;
		if (Value == nullptr)
		{
			return false;
		}
		bOutIsEntity = std::holds_alternative<FEntity>(*Value);
		std::visit(
			[&](const auto& Item) {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::is_same_v<T, bool>) OutValue = FScriptValue::MakeBool(Item);
				else if constexpr (std::is_same_v<T, int32>) OutValue = FScriptValue::MakeNumber(Item, true);
				else if constexpr (std::is_same_v<T, float>) OutValue = FScriptValue::MakeNumber(Item);
				else if constexpr (std::is_same_v<T, FVector3>) OutValue = FScriptValue::MakeVector3(Item);
				else if constexpr (std::is_same_v<T, FEntity>) OutEntity = Item;
				else OutValue = FScriptValue::MakeString(Item);
			},
			*Value);
		return true;
	};
	AIHooks.SetBlackboard = [AISys](FEntity Entity, const std::string& Key, const FScriptValue& Value) {
		FBehaviorTreeInstance* Tree = AISys->FindTree(Entity);
		const std::optional<EBlackboardKeyType> Type = Tree ? Tree->GetBlackboard().GetKeyType(Key) : std::nullopt;
		FBlackboardValue Converted;
		return Type && ToBlackboardValue(Value, *Type, Converted) && Tree->GetBlackboard().SetValue(Key, Converted);
	};
	AIHooks.SetBlackboardEntity = [AISys](FEntity Entity, const std::string& Key, FEntity Value) {
		FBehaviorTreeInstance* Tree = AISys->FindTree(Entity);
		return Tree != nullptr && Tree->GetBlackboard().SetEntity(Key, Value);
	};
	AIHooks.ClearBlackboard = [AISys](FEntity Entity, const std::string& Key) {
		FBehaviorTreeInstance* Tree = AISys->FindTree(Entity);
		return Tree != nullptr && Tree->GetBlackboard().Clear(Key);
	};
	AIHooks.MoveTo        = [AISys](FEntity Entity, const FVector3& Goal, float Radius) { return std::string(ToString(AISys->RequestMove(Entity, Goal, Radius))); };
	AIHooks.GetMoveStatus = [AISys](FEntity Entity) { return std::string(ToString(AISys->GetMoveStatus(Entity))); };
	AIHooks.StopMove      = [AISys](FEntity Entity) { AISys->StopMove(Entity); };
	AIHooks.FindPath      = [AISys](const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints) { return AISys->FindPath(Start, End, OutPoints); };
	AIHooks.StartTree     = [AISys](FEntity Entity) { return AISys->StartTree(Entity); };
	AIHooks.StopTree      = [AISys](FEntity Entity) { AISys->StopTree(Entity); };
	Scripts->SetAIHooks(std::move(AIHooks));

	// AI → Lua: Lua 비헤이비어 트리 노드의 스크립트 객체
	AI->SetScriptHooks({
		[Scripts](const std::string& Script, const std::string& Properties, FEntity Self) { return Scripts->CreateObject(Script, Properties, Self); },
		[Scripts](uint64 Handle, const char* Method, const float* DeltaSeconds) {
			FScriptValue Result;
			bool         bFound = false;
			if (!Scripts->CallObject(Handle, Method, DeltaSeconds, Result, &bFound))
			{
				return bFound ? EAIScriptResult::Error : EAIScriptResult::NotFound;
			}
			return ToScriptResult(Result);
		},
		[Scripts](uint64 Handle) { Scripts->DestroyObject(Handle); },
	});
}

void FGameWorld::Init(const FGameWorldSystems& InSystems)
{
	E_CHECKF(InSystems.Scripts != nullptr, "FGameWorld: 스크립트 시스템은 필수입니다");
	Systems = InSystems;
	Systems.Scripts->SetContentDirectory(Systems.ContentDirectory);
	AI->SetContentDirectory(Systems.ContentDirectory);
	ConnectScriptsAndAI();
	// Lua Steam 테이블 → FSteamSubsystem (초기화하지 않은 앱에서는 모두 "사용 불가")
	FSteamSubsystem& Steam = FSteamSubsystem::Get();
	Systems.Scripts->SetSteamHooks({
		[&Steam]() { return Steam.IsAvailable(); },
		[&Steam]() { return Steam.GetPlayerName(); },
		[&Steam]() { return Steam.GetGameLanguage(); },
		[&Steam](const std::string& Name) { return Steam.UnlockAchievement(Name); },
		[&Steam](const std::string& Name) { return Steam.IsAchievementUnlocked(Name); },
		[&Steam](const std::string& Name) { return Steam.ClearAchievement(Name); },
		[&Steam](const std::string& Dialog) { return Steam.ActivateOverlay(Dialog); },
		[&Steam]() { return Steam.IsOverlayActive(); },
	});
	// Lua Debug 테이블 → FDebugDraw (엔진 DLL 전역, 게임 모듈과 같은 저장소). GPU 없는 앱은 BeginPlay에서 꺼서 무시된다
	FDebugDraw& DebugDraw = FDebugDraw::Get();
	Systems.Scripts->SetDebugDrawHooks({
		[&DebugDraw](const FVector3& Start, const FVector3& End, const FVector4& Color, float Duration, bool bDepthTest) {
			DebugDraw.DrawLine(Start, End, Color, Duration, bDepthTest);
		},
		[&DebugDraw](const FVector3& From, const FVector3& To, const FVector4& Color, float Duration, bool bDepthTest) {
			DebugDraw.DrawArrow(From, To, Color, Duration, bDepthTest);
		},
		[&DebugDraw](const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color, float Duration, bool bDepthTest) {
			DebugDraw.DrawBox(Center, HalfExtents, Rotation, Color, Duration, bDepthTest);
		},
		[&DebugDraw](const FVector3& Center, float Radius, const FVector4& Color, float Duration, bool bDepthTest) {
			DebugDraw.DrawSphere(Center, Radius, Color, Duration, bDepthTest);
		},
		[&DebugDraw](const FVector3& Center, float Radius, float HalfHeight, const FQuat& Rotation, const FVector4& Color, float Duration, bool bDepthTest) {
			DebugDraw.DrawCapsule(Center, Radius, HalfHeight, Rotation, Color, Duration, bDepthTest);
		},
	});

	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr)
	{
		AI->SetMovementHooks({});
		return;
	}
	// AI 이동: 동적 강체는 물리에 수평 속도를 넘기고(Z 속도 = 중력/점프는 유지), 그 밖은 AI가 트랜스폼을 옮긴다
	AI->SetMovementHooks({
		[this, Physics](FEntity Entity, const FVector3& DesiredVelocity) {
			const FRigidBodyComponent* Body = Scene != nullptr ? Scene->GetRegistry().TryGet<FRigidBodyComponent>(Entity) : nullptr;
			if (Body == nullptr || Body->MotionType != static_cast<int32>(EPhysicsMotionType::Dynamic))
			{
				return false;
			}
			const FVector3 Current = Physics->GetVelocity(Entity);
			Physics->SetVelocity(Entity, FVector3(DesiredVelocity.X, DesiredVelocity.Y, Current.Z));
			return true;
		},
	});
	Systems.Scripts->SetPhysicsHooks({
		[Physics](const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Raycast(Origin, Direction, MaxDistance, Hit))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
		[Physics](FEntity Entity, const FVector3& Force) { Physics->AddForce(Entity, Force); },
		[Physics](FEntity Entity, const FVector3& Impulse) { Physics->AddImpulse(Entity, Impulse); },
		[Physics](FEntity Entity, const FVector3& Velocity) { Physics->SetVelocity(Entity, Velocity); },
		[Physics](FEntity Entity) { return Physics->GetVelocity(Entity); },
		[Physics](FEntity Entity) { return Physics->GetMass(Entity); },
		[Physics](FEntity Entity, const FVector3& Direction) { Physics->AddMovementInput(Entity, Direction); },
		[Physics](FEntity Entity) { Physics->RequestJump(Entity); },
		[Physics](FEntity Entity) { return Physics->IsGrounded(Entity); },
		[this, Physics](FEntity Entity) { return Scene != nullptr && Physics->EnableRagdoll(*Scene, Entity); },
		[this, Physics](FEntity Entity) {
			if (Scene != nullptr)
			{
				Physics->DisableRagdoll(*Scene, Entity);
			}
		},
		[this, Physics](FEntity Entity) { return Scene != nullptr && Physics->IsRagdollActive(*Scene, Entity); },
		[Physics](const FScriptQueryShape& Shape, const FVector3& Position, FEntity Ignore, std::vector<FEntity>& OutEntities) {
			Physics->Overlap(ToPhysicsQueryShape(Shape), Position, Shape.Rotation, OutEntities, Ignore);
		},
		[Physics](const FScriptQueryShape& Shape, const FVector3& Start, const FVector3& Direction, float MaxDistance, FEntity Ignore, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Sweep(ToPhysicsQueryShape(Shape), Start, Shape.Rotation, Direction, MaxDistance, Hit, Ignore))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
		[Physics](const FVector3& Origin, const FVector3& Direction, float MaxDistance, uint32 LayerMask, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Raycast(Origin, Direction, MaxDistance, Hit, LayerMask))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
	});
}

void FGameWorld::BeginPlay(FScene& InScene, ENetMode InMode)
{
	if (IsPlaying())
	{
		FReplicationClient* const Keep = Replication; // 앱이 이번 BeginPlay 전에 연결한 것 (EndPlay가 비운다)
		EndPlay();
		Replication = Keep;
	}
	Scene = &InScene;
	Mode  = InMode;
	RemoteInputs.clear();
	PredictedCharacters.clear();
	ServerCharacters.clear();
	CharacterCorrections = 0;
	InputSequence = 0;
	LastMatchState       = -1;
	RespawnStartIndex    = 0;
	RagdollDeadStates.clear();
	PendingSessionRequest.reset();
	PendingSceneRequest.reset();
	ClearSubScenes();
	PredictedBodies.clear();
	PredictionClock        = 0.0f;
	PredictionTimeOffset   = 0.0f;
	bPredictionTimingValid = false;
	LastAckMoveTime        = -1.0f;
	LastSnapshotTime       = -1.0f;
	LastRecordTime         = 0.0f;
	PredictionStats        = {};
	PredictionStats.bEnabled = InMode == ENetMode::Client && FCommandLine::FromProcess().HasFlag(L"--net-physics-stats");
	InstallScriptNetHooks();
	// 3D 디버그 선: 이전 플레이 것은 지우고, GPU(Resources) 없는 앱(전용 서버)은 그리기 호출을 무시한다
	FDebugDraw::Get().Clear();
	FDebugDraw::Get().SetEnabled(Systems.Resources != nullptr);

	const bool bClient = Mode == ENetMode::Client;
	if (Systems.Physics != nullptr)
	{
		if (bClient)
		{
			// 서버가 시뮬레이션하는 복제 엔티티(NetId 보유)는 키네마틱: 복제 트랜스폼을 따라가며 로컬 물체와 충돌.
			// 물리 예측 중인 바디만 동적으로 로컬 시뮬레이션한다 (바뀌면 FPhysicsSystem이 바디를 다시 만든다)
			Systems.Physics->SetKinematicOverride([this](const FScene& Target, FEntity Entity) {
				return Target.GetRegistry().Has<FNetIdComponent>(Entity) && !IsPhysicsSimulatedLocally(Entity);
			});
		}
		else
		{
			Systems.Physics->SetKinematicOverride(nullptr);
		}
		Systems.Physics->SetContactReportFilter([this](const FScene& Target, FEntity Entity) { return ShouldReportContacts(Target, Entity); });
		Systems.Physics->Begin();
	}
	if (Systems.GameModule != nullptr && !bClient) // 게임 모듈(C++ 게임 로직)은 서버에서만
	{
		Systems.GameModule->SetNet(this);
		Systems.GameModule->SetPhysics(Systems.Physics);
		Systems.GameModule->BeginPlay(InScene);
	}
	// 스크립트 BeginPlay는 Lua 상태만 만든다 (OnStart는 첫 TickGameplay). AI는 그 뒤 — 트리 시작 시 Lua 노드가 스크립트 객체를 만든다
	Systems.Scripts->BeginPlay(InScene);
	if (!bClient) // AI도 서버에서만
	{
		AI->Begin(InScene);
	}
}

void FGameWorld::EndPlay()
{
	if (!IsPlaying())
	{
		return;
	}
	if (PredictionStats.bEnabled)
	{
		LogPhysicsPredictionStats("최종");
	}
	PredictedBodies.clear();
	Replication = nullptr;
	AI->End(); // Lua 노드 OnAbort가 스크립트를 부르므로 Lua 상태보다 먼저
	Systems.Scripts->EndPlay();
	SessionSearch.Stop();
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->EndPlay(*Scene);
		Systems.GameModule->SetNet(nullptr);
		Systems.GameModule->SetPhysics(nullptr);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->End();
	}
	ClearSubScenes();
	FDebugDraw::Get().Clear(); // 플레이 정지 후 편집 화면에 남지 않게
	Scene = nullptr;
}

void FGameWorld::TickGameplay(float DeltaSeconds, const FInput* Input)
{
	E_PROFILE_SCOPE("게임플레이 틱");
	if (!IsPlaying())
	{
		return;
	}
	TickLocalInput = Input;
	FDebugDraw::Get().Tick(DeltaSeconds); // 지난 틱에 그린 디버그 선 수명 (지속 시간 0 = 여기서 사라짐), 이번 틱 스크립트/게임 모듈이 다시 그린다
	if (Mode == ENetMode::Client && Input != nullptr)
	{
		SendLocalInput(*Input); // 서버 스크립트가 이 플레이어 소유 엔티티에서 읽는다
	}
	if (SessionSearch.IsSearching())
	{
		SessionSearch.Update(); // Net.FindSessions 응답 수집
	}
	TickSubScenes(); // 파싱이 끝난 서브 씬 붙이기 + 스트리밍 볼륨 판정 (스크립트 전 — 새 스크립트가 이번 틱에 OnStart)
	Systems.Scripts->Update(DeltaSeconds, Input); // 실행 위치 필터는 BeginPlay에서 정했다
	FSequenceSystem::Update(*Scene, DeltaSeconds); // 컷신: 스크립트 PlaySequence가 이번 틱에 반영, 쓴 트랜스폼은 이번 물리/트랜스폼 갱신에
	TickPhysicsPrediction(DeltaSeconds);         // 클라이언트: 물리 예측 대상/서버 상태 수렴 (캐릭터가 밀기 전에)
	TickCharacters(DeltaSeconds);                 // 스크립트가 넣은 이동 입력으로 (물리 스텝 전)
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		E_PROFILE_SCOPE("게임 모듈");
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	AI->Update(*Scene, DeltaSeconds); // Client 역할은 Begin하지 않았으므로 아무것도 하지 않는다
	TickGameplayRules(DeltaSeconds);  // 이번 프레임 데미지 이벤트·사망·리스폰·매치 (물리 전: 리스폰 순간이동이 이번 스텝에 반영)
	TickRagdolls();                   // 사망/리스폰 → 래그돌 켜기/끄기 (모든 역할, 복제된 체력 기준 — 물리 전: 이번 스텝부터 쓰러진다)
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->Update(*Scene, DeltaSeconds);
	}
	Scene->UpdateTransforms();
	RecordPhysicsPrediction();               // 이번 스텝 결과 기록 (서버 스냅샷과 비교할 로컬 과거)
	UpdateCharacterAnimParams(DeltaSeconds); // 이번 프레임 이동 결과 → 다음 표시 틱 애니메이션
	UpdateFootIkProbes();                    // 발 IK 바닥 (이번 프레임 최종 위치 기준 — 다음 표시 틱 애니메이션이 쓴다)
	DispatchCollisionEvents();               // 이번 프레임 물리 스텝의 충돌/트리거 알림 (스크립트·게임 모듈, 메인 스레드)
	// 이번 프레임 최종 위치 기준 (카메라 따라가기 등)
	Systems.Scripts->LateUpdate(DeltaSeconds, Input);
	Scene->UpdateTransforms();
	for (auto& [PlayerId, Remote] : RemoteInputs)
	{
		Remote.Input.EndFrame(); // 원격 입력의 눌림/떼어짐은 서버 틱 한 번만
	}
	TickLocalInput = nullptr;
}

void FGameWorld::TickPresentation(FScene& TargetScene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("표시 틱");
	FAnimationSystem::Update(TargetScene, DeltaSeconds);
	TargetScene.UpdateTransforms();
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::ResolveParticles(TargetScene, *Systems.Resources, Systems.ContentDirectory);
	}
	FParticleSystem::Update(TargetScene, DeltaSeconds);
}
