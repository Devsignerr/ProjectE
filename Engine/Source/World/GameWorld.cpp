#include "World/GameWorld.h"

#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/Assert.h"
#include "Network/ReplicationTypes.h"
#include "Online/SteamSubsystem.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>
#include <variant>

namespace
{
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
	});
}

void FGameWorld::BeginPlay(FScene& InScene, ENetMode InMode)
{
	if (IsPlaying())
	{
		EndPlay();
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
	PendingSessionRequest.reset();
	InstallScriptNetHooks();

	const bool bClient = Mode == ENetMode::Client;
	if (Systems.Physics != nullptr)
	{
		if (bClient)
		{
			// 서버가 시뮬레이션하는 복제 엔티티(NetId 보유)는 키네마틱: 복제 트랜스폼을 따라가며 로컬 물체와 충돌
			Systems.Physics->SetKinematicOverride([](const FScene& Target, FEntity Entity) { return Target.GetRegistry().Has<FNetIdComponent>(Entity); });
		}
		else
		{
			Systems.Physics->SetKinematicOverride(nullptr);
		}
		Systems.Physics->Begin();
	}
	if (Systems.GameModule != nullptr && !bClient) // 게임 모듈(C++ 게임 로직)은 서버에서만
	{
		Systems.GameModule->SetNet(this);
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
	AI->End(); // Lua 노드 OnAbort가 스크립트를 부르므로 Lua 상태보다 먼저
	Systems.Scripts->EndPlay();
	SessionSearch.Stop();
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->EndPlay(*Scene);
		Systems.GameModule->SetNet(nullptr);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->End();
	}
	Scene = nullptr;
}

void FGameWorld::TickGameplay(float DeltaSeconds, const FInput* Input)
{
	if (!IsPlaying())
	{
		return;
	}
	if (Mode == ENetMode::Client && Input != nullptr)
	{
		SendLocalInput(*Input); // 서버 스크립트가 이 플레이어 소유 엔티티에서 읽는다
	}
	if (SessionSearch.IsSearching())
	{
		SessionSearch.Update(); // Net.FindSessions 응답 수집
	}
	Systems.Scripts->Update(DeltaSeconds, Input); // 실행 위치 필터는 BeginPlay에서 정했다
	TickCharacters(DeltaSeconds);                 // 스크립트가 넣은 이동 입력으로 (물리 스텝 전)
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	AI->Update(*Scene, DeltaSeconds); // Client 역할은 Begin하지 않았으므로 아무것도 하지 않는다
	TickGameplayRules(DeltaSeconds);  // 이번 프레임 데미지 이벤트·사망·리스폰·매치 (물리 전: 리스폰 순간이동이 이번 스텝에 반영)
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->Update(*Scene, DeltaSeconds);
	}
	Scene->UpdateTransforms();
	// 이번 프레임 최종 위치 기준 (카메라 따라가기 등)
	Systems.Scripts->LateUpdate(DeltaSeconds, Input);
	Scene->UpdateTransforms();
	for (auto& [PlayerId, Remote] : RemoteInputs)
	{
		Remote.Input.EndFrame(); // 원격 입력의 눌림/떼어짐은 서버 틱 한 번만
	}
}

void FGameWorld::TickPresentation(FScene& TargetScene, float DeltaSeconds)
{
	FAnimationSystem::Update(TargetScene, DeltaSeconds);
	TargetScene.UpdateTransforms();
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::ResolveParticles(TargetScene, *Systems.Resources, Systems.ContentDirectory);
	}
	FParticleSystem::Update(TargetScene, DeltaSeconds);
}
