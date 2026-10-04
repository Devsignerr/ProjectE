#include "World/GameWorld.h"

#include "Core/Profiling.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/Assert.h"
#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/InputMode.h"
#include "Network/ReplicationTypes.h"
#include "Online/SteamSubsystem.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/DebugDraw.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/SkyAtmosphere.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scene/SequencePlayer.h"
#include "Scene/Sprite/FlipbookSystem.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <type_traits>
#include <unordered_set>
#include <variant>

E_DEFINE_LOG_CATEGORY(LogGameWorldPerf, Log)

// ---- 시간 배율 (Lua Game.SetTimeScale/GetTimeScale/HitStop, C++ FGameWorld::SetTimeScale/HitStop, 게임 모듈 IGameNet 같은 이름 + GetUnscaledDeltaSeconds)
//   게임 시간 = 앱이 넘긴 프레임 dt(실제 시간 — --fixed-delta면 고정값) × 배율. 배율은 게임플레이 틱 시작에 한 번 정하고(TickTimeScale) 그 틱과
//   바로 다음 표시 틱이 모두 같은 값을 쓴다 — 틱 도중 바꾼 배율·히트스톱은 다음 틱부터 (한 틱 안에서 시스템마다 다른 시간이 흐르지 않게).
//   히트스톱이 남아 있으면 그 틱 배율은 0이고 남은 시간은 실제 dt만큼 준다 (0.05초 = 60fps에서 3프레임 정지).
//   배율을 곱하는 것 (게임 시간): 스크립트 OnUpdate/OnLateUpdate dt·Time.DeltaTime·타이머·코루틴 Wait, 능력 시스템, 시퀀스, 게임 모듈,
//     AI, 게임플레이 규칙(리스폰 대기 등), 3D/2D 물리(고정 스텝 누적 — 0이면 스텝 없음, 보간 상태 유지), 3D/2D 캐릭터 이동기,
//     표시 틱(플레이 씬만): 애니메이션·2D 플립북·파티클·시간대
//   실제 시간 (배율 무관): 앱 프레임·UI(FUISystem 입력·UI 애니메이션 — 일시정지 메뉴가 움직인다)·오디오(소리는 멈추지 않는다 — 필요하면 스크립트가
//     Audio로 끈다)·렌더러 화면 시간(FFrameTime: 물결·구름·머티리얼 Time)·디버그 선 수명·네트워크, 스크립트 Time.UnscaledDeltaTime·
//     Timer.After(…, { Unscaled = true })·WaitUnscaled. 편집 씬 표시 틱(에디터, 플레이 밖)도 실제 시간
//   배율 0인 틱: 캐릭터 이동기는 무브를 시뮬레이션하지 않는다 (쌓인 입력은 버림, 넉백은 다음 무브까지 남음 — 정지 동안 공중에서 떨어지지 않는다).
//     스크립트는 dt 0으로 계속 불린다 (입력을 읽어 일시정지를 풀 수 있게)
//   멀티플레이: Standalone 전용. 서버가 시간을 늦추면 소유 클라이언트가 보낸 무브(클라이언트 dt)와 서버 시간·보간 시계가 어긋나므로 네트워크 모드
//     (리슨/전용 서버/클라이언트)에서는 Set/HitStop을 거절하고(경고 한 번) 배율은 1. 플레이 시작·맵 전환(BeginPlay)·모드 전환에 1로 돌아간다.
//   결정성: 배율은 프레임 dt에 곱할 뿐이므로 --fixed-delta 실행은 그대로 결정적이다

namespace
{
	// ---- --perf-capture [--perf-warmup N]: 게임 쪽 틱 구간별 CPU 평균 (렌더러 [성능] 로그와 같은 워밍업, 종료 때 [성능] 게임 틱 로그)
	enum class EGameTickTimer : uint32
	{
		Scripts,
		GameModule,
		Characters,
		Physics,
		GameplayTransforms,
		LateUpdate,
		Animation,
		PresentTransforms,
		Particles,
		Count
	};

	constexpr const char* GameTickTimerNames[] = { "스크립트", "게임 모듈", "캐릭터", "물리", "게임플레이 트랜스폼", "LateUpdate", "애니메이션", "표시 트랜스폼", "파티클" };
	static_assert(std::size(GameTickTimerNames) == static_cast<size_t>(EGameTickTimer::Count));

	struct FGameTickPerf
	{
		bool   bInitialized = false;
		bool   bEnabled     = false;
		bool   bCounting    = false;
		uint32 WarmupFrames = 0;
		uint32 SeenFrames   = 0;
		uint32 Frames       = 0;
		double Ms[static_cast<size_t>(EGameTickTimer::Count)] = {};
	};
	FGameTickPerf GGameTickPerf;

	FGameTickPerf& GetGameTickPerf()
	{
		FGameTickPerf& Perf = GGameTickPerf;
		if (!Perf.bInitialized)
		{
			Perf.bInitialized         = true;
			const FCommandLine Line   = FCommandLine::FromProcess();
			Perf.bEnabled             = Line.HasFlag(L"--perf-capture");
			if (const std::wstring Warmup = Line.GetValue(L"--perf-warmup"); !Warmup.empty())
			{
				Perf.WarmupFrames = static_cast<uint32>(std::max(0, std::stoi(Warmup)));
			}
		}
		return Perf;
	}

	class FScopedGameTickTimer
	{
	public:
		explicit FScopedGameTickTimer(EGameTickTimer InTimer) : Timer(InTimer), bActive(GetGameTickPerf().bCounting)
		{
			if (bActive)
			{
				Start = std::chrono::steady_clock::now();
			}
		}
		~FScopedGameTickTimer()
		{
			if (bActive)
			{
				GGameTickPerf.Ms[static_cast<size_t>(Timer)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
			}
		}
		FScopedGameTickTimer(const FScopedGameTickTimer&)            = delete;
		FScopedGameTickTimer& operator=(const FScopedGameTickTimer&) = delete;

	private:
		EGameTickTimer                        Timer;
		bool                                  bActive;
		std::chrono::steady_clock::time_point Start;
	};

	void LogGameTickPerf()
	{
		FGameTickPerf& Perf = GetGameTickPerf();
		if (!Perf.bEnabled || Perf.Frames == 0)
		{
			return;
		}
		std::string Text;
		double      Total = 0.0;
		for (size_t Index = 0; Index < static_cast<size_t>(EGameTickTimer::Count); ++Index)
		{
			const double Average = Perf.Ms[Index] / Perf.Frames;
			Total += Average;
			Text += std::format("{}{} {:.3f}", Text.empty() ? "" : ", ", GameTickTimerNames[Index], Average);
		}
		E_LOG(LogGameWorldPerf, Display, "[성능] 게임 틱 {} 프레임 평균 CPU ms: 합 {:.3f} ({})", Perf.Frames, Total, Text);
		Perf.Frames = 0;
		std::fill(std::begin(Perf.Ms), std::end(Perf.Ms), 0.0);
	}

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

FGameWorld::FGameWorld() : Abilities(std::make_unique<FAbilitySystem>()), AI(std::make_unique<FAISystem>()), Physics2D(std::make_unique<FPhysics2DSystem>()),
	  Characters2D(std::make_unique<FCharacterMovement2DSystem>())
{
}
FGameWorld::~FGameWorld() = default;

bool FGameWorld::SetTimeScale(float Scale)
{
	if (Mode != ENetMode::Standalone)
	{
		if (!bWarnedNetTimeScale)
		{
			bWarnedNetTimeScale = true;
			E_LOG(LogGameWorldPerf, Warning, "게임 시간 배율/히트스톱은 Standalone 전용입니다 (네트워크 세션에서는 무시)");
		}
		return false;
	}
	TimeScale = std::isfinite(Scale) ? std::clamp(Scale, 0.0f, MaxTimeScale) : 1.0f;
	return true;
}

bool FGameWorld::HitStop(float Seconds)
{
	if (!SetTimeScale(TimeScale)) // 같은 규칙 (네트워크 모드 거절)
	{
		return false;
	}
	if (std::isfinite(Seconds) && Seconds > 0.0f)
	{
		HitStopRemaining = std::max(HitStopRemaining, std::min(Seconds, 10.0f));
	}
	return true;
}

void FGameWorld::ResetTimeScale()
{
	TimeScale        = 1.0f;
	HitStopRemaining = 0.0f;
	TickTimeScale    = 1.0f;
}

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
	ConnectAbilities();
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

	InstallScriptPhysicsHooks();

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
}

void FGameWorld::InstallScriptPhysicsHooks()
{
	// 3D(Systems.Physics — 없으면 그 기능은 무시) + 2D(Physics2D, 항상). 엔티티 함수(AddForce 등)는 2D 바디만 있는 엔티티면 2D로:
	// 둘 다 있으면 3D를 쓰고 엔티티마다 경고 한 번. 벡터는 월드 3D (2D는 X·Z 성분)
	FPhysicsSystem*   Physics = Systems.Physics;
	FPhysics2DSystem* P2D     = Physics2D.get();
	const auto        Warned  = std::make_shared<std::unordered_set<uint64>>();
	const auto        Uses2D  = [Physics, P2D, Warned](FEntity Entity) {
        const bool bHas2D = P2D->HasBody(Entity);
        if (!bHas2D)
        {
            return false;
        }
        const bool bHas3D = Physics != nullptr && (Physics->HasBody(Entity) || Physics->HasCharacter(Entity));
        if (bHas3D && Warned->insert(Entity.ToId()).second)
        {
            E_LOG(LogPhysics, Warning, "엔티티 {}에 3D와 2D 물리 바디가 모두 있어 스크립트 물리 함수는 3D를 씁니다", Entity.ToId());
        }
        return !bHas3D;
	};
	const auto ToPlane   = [](const FVector3& Vector) { return FVector2(Vector.X, Vector.Z); };
	const auto FromPlane = [](const FVector2& Vector) { return FVector3(Vector.X, 0.0f, Vector.Y); };
	const auto ToHit     = [](const FPhysicsHit& Hit) { return FScriptRayHit{ Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance }; };

	FScriptPhysicsHooks Hooks;
	Hooks.Raycast = [Physics, ToHit](const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit) {
		FPhysicsHit Hit;
		if (Physics == nullptr || !Physics->Raycast(Origin, Direction, MaxDistance, Hit))
		{
			return false;
		}
		OutHit = ToHit(Hit);
		return true;
	};
	Hooks.AddForce = [Physics, P2D, Uses2D, ToPlane](FEntity Entity, const FVector3& Force) {
		if (Uses2D(Entity))
		{
			P2D->AddForce(Entity, ToPlane(Force));
		}
		else if (Physics != nullptr)
		{
			Physics->AddForce(Entity, Force);
		}
	};
	Hooks.AddImpulse = [Physics, P2D, Uses2D, ToPlane](FEntity Entity, const FVector3& Impulse) {
		if (Uses2D(Entity))
		{
			P2D->AddImpulse(Entity, ToPlane(Impulse));
		}
		else if (Physics != nullptr)
		{
			Physics->AddImpulse(Entity, Impulse);
		}
	};
	Hooks.SetVelocity = [Physics, P2D, Uses2D, ToPlane](FEntity Entity, const FVector3& Velocity) {
		if (Uses2D(Entity))
		{
			P2D->SetVelocity(Entity, ToPlane(Velocity));
		}
		else if (Physics != nullptr)
		{
			Physics->SetVelocity(Entity, Velocity);
		}
	};
	Hooks.GetVelocity = [Physics, P2D, Uses2D, FromPlane](FEntity Entity) {
		if (Uses2D(Entity))
		{
			return FromPlane(P2D->GetVelocity(Entity));
		}
		return Physics != nullptr ? Physics->GetVelocity(Entity) : FVector3();
	};
	Hooks.GetMass = [Physics, P2D, Uses2D](FEntity Entity) {
		if (Uses2D(Entity))
		{
			return P2D->GetMass(Entity);
		}
		return Physics != nullptr ? Physics->GetMass(Entity) : 0.0f;
	};
	// 캐릭터 이동: 엔티티에 2D 이동기(FCharacterMovement2DComponent)가 있으면 2D (X·Z 성분), 아니면 3D 캐릭터
	FCharacterMovement2DSystem* C2D   = Characters2D.get();
	const auto                  Is2D  = [this](FEntity Entity) {
        return Scene != nullptr && Scene->GetRegistry().IsValid(Entity) && Scene->GetRegistry().Has<FCharacterMovement2DComponent>(Entity);
	};
	Hooks.AddMovementInput = [Physics, C2D, Is2D](FEntity Entity, const FVector3& Direction) {
		if (Is2D(Entity))
		{
			C2D->AddMovementInput(Entity, Direction);
		}
		else if (Physics != nullptr)
		{
			Physics->AddMovementInput(Entity, Direction);
		}
	};
	Hooks.Jump = [Physics, C2D, Is2D](FEntity Entity) {
		if (Is2D(Entity))
		{
			C2D->Jump(Entity);
		}
		else if (Physics != nullptr)
		{
			Physics->RequestJump(Entity);
		}
	};
	Hooks.IsGrounded = [Physics, C2D, Is2D](FEntity Entity) {
		return Is2D(Entity) ? C2D->IsGrounded(Entity) : Physics != nullptr && Physics->IsGrounded(Entity);
	};
	Hooks.StopJumping = [C2D, Is2D](FEntity Entity) {
		if (Is2D(Entity))
		{
			C2D->StopJumping(Entity); // 3D 캐릭터는 가변 점프가 없다
		}
	};
	Hooks.Dash = [C2D, Is2D](FEntity Entity, const FVector3& Direction) {
		if (Is2D(Entity))
		{
			C2D->Dash(Entity, Direction);
		}
	};
	Hooks.DropDown = [C2D, Is2D](FEntity Entity) {
		if (Is2D(Entity))
		{
			C2D->DropDown(Entity);
		}
	};
	Hooks.GetMovementVelocity = [Physics, C2D, Is2D](FEntity Entity) {
		if (Is2D(Entity))
		{
			const FVector2 Velocity = C2D->GetVelocity(Entity);
			return FVector3(Velocity.X, 0.0f, Velocity.Y);
		}
		return Physics != nullptr ? Physics->GetCharacterState(Entity).Velocity : FVector3();
	};
	Hooks.GetJumpsRemaining  = [C2D, Is2D](FEntity Entity) { return Is2D(Entity) ? C2D->GetJumpsRemaining(Entity) : 0; };
	Hooks.GetDashesRemaining = [C2D, Is2D](FEntity Entity) { return Is2D(Entity) ? C2D->GetDashesRemaining(Entity) : 0; };
	Hooks.IsDashing          = [C2D, Is2D](FEntity Entity) { return Is2D(Entity) && C2D->IsDashing(Entity); };
	// 넉백/발사: 2D 이동기면 2D (X·Z), 아니면 3D 캐릭터 (덮어쓰기 X = 수평 XY 묶음). 무브에 싣는 규칙은 World/GameWorldCharacter(2D).cpp 머리 주석
	Hooks.LaunchCharacter = [Physics, C2D, Is2D](FEntity Entity, const FVector3& Velocity, bool bOverrideX, bool bOverrideZ) {
		if (Is2D(Entity))
		{
			C2D->LaunchCharacter(Entity, Velocity, bOverrideX, bOverrideZ);
		}
		else if (Physics != nullptr)
		{
			Physics->LaunchCharacter(Entity, Velocity, bOverrideX, bOverrideZ);
		}
	};
	Hooks.AddKnockback = [Physics, C2D, Is2D](FEntity Entity, const FVector3& Velocity, float StunSeconds) {
		if (Is2D(Entity))
		{
			C2D->AddKnockback(Entity, Velocity, StunSeconds);
		}
		else if (Physics != nullptr)
		{
			Physics->AddKnockback(Entity, Velocity, StunSeconds);
		}
	};
	Hooks.IsStunned = [Physics, C2D, Is2D](FEntity Entity) {
		return Is2D(Entity) ? C2D->IsStunned(Entity) : Physics != nullptr && Physics->IsCharacterStunned(Entity);
	};
	if (Physics != nullptr)
	{
		Hooks.EnableRagdoll    = [this, Physics](FEntity Entity) { return Scene != nullptr && Physics->EnableRagdoll(*Scene, Entity); };
		Hooks.DisableRagdoll   = [this, Physics](FEntity Entity) {
            if (Scene != nullptr)
            {
                Physics->DisableRagdoll(*Scene, Entity);
            }
		};
		Hooks.IsRagdollActive = [this, Physics](FEntity Entity) { return Scene != nullptr && Physics->IsRagdollActive(*Scene, Entity); };
		Hooks.Overlap         = [Physics](const FScriptQueryShape& Shape, const FVector3& Position, FEntity Ignore, std::vector<FEntity>& OutEntities) {
            Physics->Overlap(ToPhysicsQueryShape(Shape), Position, Shape.Rotation, OutEntities, Ignore);
		};
		Hooks.Sweep = [Physics, ToHit](const FScriptQueryShape& Shape, const FVector3& Start, const FVector3& Direction, float MaxDistance, FEntity Ignore,
		                               FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Sweep(ToPhysicsQueryShape(Shape), Start, Shape.Rotation, Direction, MaxDistance, Hit, Ignore))
			{
				return false;
			}
			OutHit = ToHit(Hit);
			return true;
		};
		Hooks.RaycastLayers = [Physics, ToHit](const FVector3& Origin, const FVector3& Direction, float MaxDistance, uint32 LayerMask, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Raycast(Origin, Direction, MaxDistance, Hit, LayerMask))
			{
				return false;
			}
			OutHit = ToHit(Hit);
			return true;
		};
	}
	Hooks.Raycast2D = [P2D](const FVector2& Origin, const FVector2& Direction, float MaxDistance, uint32 LayerMask, FScriptRayHit2D& OutHit) {
		FPhysics2DHit Hit;
		if (!P2D->Raycast(Origin, Direction, MaxDistance, Hit, LayerMask))
		{
			return false;
		}
		OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance, Hit.Fraction };
		return true;
	};
	Hooks.OverlapBox2D = [P2D](const FVector2& Center, const FVector2& HalfSize, float Angle, uint32 LayerMask, std::vector<FEntity>& OutEntities) {
		P2D->OverlapBox(Center, HalfSize, Angle, OutEntities, LayerMask);
	};
	Hooks.OverlapCircle2D = [P2D](const FVector2& Center, float Radius, uint32 LayerMask, std::vector<FEntity>& OutEntities) {
		P2D->OverlapCircle(Center, Radius, OutEntities, LayerMask);
	};
	Hooks.BeginDrag2D  = [P2D](FEntity Entity, const FVector2& Point, float MaxForce) { return P2D->BeginDrag(Entity, Point, MaxForce); };
	Hooks.UpdateDrag2D = [P2D](FEntity Entity, const FVector2& Target) { return P2D->UpdateDrag(Entity, Target); };
	Hooks.EndDrag2D    = [P2D](FEntity Entity) { return P2D->EndDrag(Entity); };
	Hooks.ControlJoint2D = [this, P2D](FEntity Entity, EScriptJoint2DControl Op, float A, float B) {
		if (Scene == nullptr)
		{
			return false;
		}
		switch (Op)
		{
		case EScriptJoint2DControl::MotorSpeed:
			return P2D->SetJointMotorSpeed(*Scene, Entity, A);
		case EScriptJoint2DControl::MaxMotorForce:
			return P2D->SetJointMaxMotorForce(*Scene, Entity, A);
		case EScriptJoint2DControl::EnableMotor:
			return P2D->EnableJointMotor(*Scene, Entity, A != 0.0f);
		case EScriptJoint2DControl::Limits:
			return P2D->SetJointLimits(*Scene, Entity, A, B);
		case EScriptJoint2DControl::EnableLimit:
			return P2D->EnableJointLimit(*Scene, Entity, A != 0.0f);
		case EScriptJoint2DControl::Spring:
			return P2D->SetJointSpring(*Scene, Entity, A, B);
		case EScriptJoint2DControl::SpringTarget:
			return P2D->SetJointTarget(*Scene, Entity, A);
		}
		return false;
	};
	Hooks.QueryJoint2D = [P2D](FEntity Entity, EScriptJoint2DQuery What) {
		switch (What)
		{
		case EScriptJoint2DQuery::Angle:
			return P2D->GetJointAngle(Entity);
		case EScriptJoint2DQuery::Translation:
			return P2D->GetJointTranslation(Entity);
		case EScriptJoint2DQuery::Speed:
			return P2D->GetJointSpeed(Entity);
		}
		return 0.0f;
	};
	Systems.Scripts->SetPhysicsHooks(std::move(Hooks));
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
	ResetTimeScale(); // 시간 배율은 플레이(맵)마다 1에서
	RemoteInputs.clear();
	PredictedCharacters.clear();
	ServerCharacters.clear();
	PredictedCharacters2D.clear();
	ServerCharacters2D.clear();
	CharacterCorrections = 0;
	InputSequence = 0;
	LastMatchState       = -1;
	RespawnStartIndex    = 0;
	RagdollDeadStates.clear();
	PendingSessionRequest.reset();
	PendingSceneRequest.reset();
	FInputModeState::Reset(); // 입력 모드는 플레이(맵)마다 기본값에서 시작 — 게임 모듈/스크립트 시작 전에
	ClearSubScenes();
	GameplayValidatedScene = nullptr;
	PredictedBodies.clear();
	PredictedBodies2D.clear();
	PredictionClock        = 0.0f;
	PredictionTimeOffset   = 0.0f;
	bPredictionTimingValid = false;
	LastAckMoveTime        = -1.0f;
	LastSnapshotTime       = -1.0f;
	LastRecordTime         = 0.0f;
	PredictionStats        = {};
	PredictionStats2D      = {};
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
	// 2D 물리: 3D와 같은 역할 규칙 (클라이언트는 복제 엔티티 동적 바디를 키네마틱으로, 물리 예측 중인 바디만 동적 —
	// World/GameWorldPhysicsPrediction2D.cpp), 보간은 3D 설정을 따른다
	if (bClient)
	{
		Physics2D->SetKinematicOverride([this](const FScene& Target, FEntity Entity) {
			return Target.GetRegistry().Has<FNetIdComponent>(Entity) && !IsPhysics2DSimulatedLocally(Entity);
		});
	}
	else
	{
		Physics2D->SetKinematicOverride(nullptr);
	}
	Physics2D->SetContactReportFilter([this](const FScene& Target, FEntity Entity) { return ShouldReportContacts(Target, Entity); });
	Physics2D->SetInterpolation(Systems.Physics == nullptr || Systems.Physics->IsInterpolating());
	Physics2D->Begin();
	Characters2D->Begin(*Physics2D); // 2D 캐릭터 이동기 (대리 바디를 2D 월드에 만든다)
	if (Systems.GameModule != nullptr && !bClient) // 게임 모듈(C++ 게임 로직)은 서버에서만
	{
		Systems.GameModule->SetNet(this);
		Systems.GameModule->SetPhysics(Systems.Physics);
		Systems.GameModule->SetPhysics2D(Physics2D.get());
		Systems.GameModule->SetCharacters2D(Characters2D.get());
		Systems.GameModule->BeginPlay(InScene);
	}
	// 스크립트 BeginPlay는 Lua 상태만 만든다 (OnStart는 첫 TickGameplay). AI는 그 뒤 — 트리 시작 시 Lua 노드가 스크립트 객체를 만든다
	Systems.Scripts->BeginPlay(InScene);
	Abilities->Begin(InScene, !bClient); // 스크립트(Lua 상태) 뒤: 능력 스크립트를 그 상태에서 돌린다
	if (Systems.GameModule != nullptr && !bClient)
	{
		Systems.GameModule->SetAbilities(Abilities.get());
	}
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
		LogPhysicsPredictionStats2D("최종");
	}
	LogGameTickPerf();
	PredictedBodies.clear();
	PredictedBodies2D.clear();
	Replication = nullptr;
	AI->End(); // Lua 노드 OnAbort가 스크립트를 부르므로 Lua 상태보다 먼저
	Abilities->End(); // 발동 중 능력 취소 (능력 스크립트 OnEnd) — Lua 상태보다 먼저
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->SetAbilities(nullptr);
	}
	Systems.Scripts->EndPlay();
	Sprite2DRuntime::FlushTilemapEdits(*Scene); // OnDestroy 등의 마지막 타일 편집
	SessionSearch.Stop();
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->EndPlay(*Scene);
		Systems.GameModule->SetNet(nullptr);
		Systems.GameModule->SetPhysics(nullptr);
		Systems.GameModule->SetPhysics2D(nullptr);
		Systems.GameModule->SetCharacters2D(nullptr);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->End();
	}
	Characters2D->End();
	Physics2D->End();
	ClearSubScenes();
	FInputModeState::Reset();
	FDebugDraw::Get().Clear(); // 플레이 정지 후 편집 화면에 남지 않게
	GameplayValidatedScene = nullptr;
	Scene = nullptr;
	ResetTimeScale();
}

void FGameWorld::TickGameplay(float DeltaSeconds, const FInput* Input)
{
	E_PROFILE_SCOPE("게임플레이 틱");
	if (!IsPlaying())
	{
		return;
	}
	TickLocalInput = Input;
	// 시간 배율 (머리 주석 "시간 배율"): 이 틱 배율을 정하고 아래 게임 시스템은 모두 배율을 곱한 dt를 쓴다
	const float UnscaledDeltaSeconds = DeltaSeconds;
	TickUnscaledDeltaSeconds         = UnscaledDeltaSeconds;
	TickTimeScale                    = TimeScale;
	if (HitStopRemaining > 1.0e-4f)
	{
		TickTimeScale    = 0.0f;
		HitStopRemaining = std::max(HitStopRemaining - UnscaledDeltaSeconds, 0.0f);
	}
	else
	{
		HitStopRemaining = 0.0f;
	}
	DeltaSeconds = UnscaledDeltaSeconds * TickTimeScale;
	Systems.Scripts->SetFrameTime(UnscaledDeltaSeconds, TickTimeScale);
	FDebugDraw::Get().Tick(UnscaledDeltaSeconds); // 지난 틱에 그린 디버그 선 수명 (지속 시간 0 = 여기서 사라짐), 이번 틱 스크립트/게임 모듈이 다시 그린다
	if (Mode == ENetMode::Client && Input != nullptr)
	{
		SendLocalInput(*Input); // 서버 스크립트가 이 플레이어 소유 엔티티에서 읽는다
	}
	if (SessionSearch.IsSearching())
	{
		SessionSearch.Update(); // Net.FindSessions 응답 수집
	}
	TickSubScenes(); // 파싱이 끝난 서브 씬 붙이기 + 스트리밍 볼륨 판정 (스크립트 전 — 새 스크립트가 이번 틱에 OnStart)
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::Scripts);
		Systems.Scripts->Update(DeltaSeconds, Input); // 실행 위치 필터는 BeginPlay에서 정했다
	}
	TickAbilities(DeltaSeconds); // 능력: 입력 발동·효과 시간/주기·능력 스크립트 대기 재개 (캐릭터 이동 전 — 대시 이동 입력·MoveSpeed가 이번 무브에)
	FSequenceSystem::Update(*Scene, DeltaSeconds); // 컷신: 스크립트 PlaySequence가 이번 틱에 반영, 쓴 트랜스폼은 이번 물리/트랜스폼 갱신에
	TickPhysicsPrediction(DeltaSeconds);         // 클라이언트: 물리 예측 대상/서버 상태 수렴 (캐릭터가 밀기 전에)
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::Characters);
		TickCharacters(DeltaSeconds);   // 스크립트가 넣은 이동 입력으로 (물리 스텝 전)
		TickCharacters2D(DeltaSeconds); // 2D 이동기 — 같은 단계, 같은 예측 규칙 (World/GameWorldCharacter2D.cpp)
	}
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		E_PROFILE_SCOPE("게임 모듈");
		const FScopedGameTickTimer Timer(EGameTickTimer::GameModule);
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	AI->Update(*Scene, DeltaSeconds); // Client 역할은 Begin하지 않았으므로 아무것도 하지 않는다
	TickGameplayRules(DeltaSeconds);  // 이번 프레임 데미지 이벤트·사망·리스폰·매치 (물리 전: 리스폰 순간이동이 이번 스텝에 반영)
	TickRagdolls();                   // 사망/리스폰 → 래그돌 켜기/끄기 (모든 역할, 복제된 체력 기준 — 물리 전: 이번 스텝부터 쓰러진다)
	if (Systems.Physics != nullptr)
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::Physics);
		Systems.Physics->Update(*Scene, DeltaSeconds);
	}
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::Physics);
		Characters2D->UpdateProxies();           // 2D 캐릭터 대리 바디를 이번 위치로 (스텝에서 동적 바디를 민다)
		Physics2D->Update(*Scene, DeltaSeconds); // 2D 물리 (3D 바로 뒤, 같은 단계 — 2D 바디가 없으면 거의 비용 없음)
	}
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::GameplayTransforms);
		Scene->UpdateTransforms();
	}
	RecordPhysicsPrediction();               // 이번 스텝 결과 기록 (서버 스냅샷과 비교할 로컬 과거)
	UpdateCharacterAnimParams(DeltaSeconds); // 이번 프레임 이동 결과 → 다음 표시 틱 애니메이션
	UpdateFootIkProbes();                    // 발 IK 바닥 (이번 프레임 최종 위치 기준 — 다음 표시 틱 애니메이션이 쓴다)
	// 이번 프레임 물리 스텝의 충돌/트리거 알림 (스크립트·게임 모듈, 메인 스레드)
	bool bMayHaveChangedScene = DispatchCollisionEvents();
	bMayHaveChangedScene      = DispatchCharacter2DEvents() || bMayHaveChangedScene; // 2D 이동기 점프/착지/대시 (같은 단계)
	// 이번 프레임 최종 위치 기준 (카메라 따라가기 등)
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::LateUpdate);
		bMayHaveChangedScene = Systems.Scripts->LateUpdate(DeltaSeconds, Input) || bMayHaveChangedScene;
	}
	// 위 트랜스폼 갱신 뒤 씬을 바꿀 수 있는 것은 충돌 알림과 OnLateUpdate(+ 지연 파괴)뿐이다 (기록·애니메이션 파라미터·발 IK 탐색은 읽기만).
	// 둘 다 아무것도 하지 않았으면 다시 갱신해도 모든 엔티티가 캐시와 같으므로 건너뛴다 (결과 동일)
	if (bMayHaveChangedScene)
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::GameplayTransforms);
		Scene->UpdateTransforms();
	}
	// 이번 틱의 게임플레이 타일 편집(Lua SetTile 등)을 TileData로 한 번에 인코딩 (저장·복제 대상 — Sprite2DComponents.h "지연 커밋")
	Sprite2DRuntime::FlushTilemapEdits(*Scene);
	GameplayValidatedScene = Scene; // 바로 다음 표시 틱은 그 틱이 쓴 엔티티만 다시 본다 (UpdateTransformsPartial)
	for (auto& [PlayerId, Remote] : RemoteInputs)
	{
		Remote.Input.EndFrame(); // 원격 입력의 눌림/떼어짐은 서버 틱 한 번만
	}
	TickLocalInput = nullptr;
}

void FGameWorld::TickPresentation(FScene& TargetScene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("표시 틱");
	if (FGameTickPerf& Perf = GetGameTickPerf(); Perf.bEnabled)
	{
		Perf.bCounting = Perf.SeenFrames++ >= Perf.WarmupFrames;
		Perf.Frames += Perf.bCounting ? 1u : 0u;
	}
	// 게임플레이 틱이 이 씬을 방금 전체 갱신했으면, 이 틱에서 로컬을 쓰는 것(시간대 태양, 평가한 애니메이션 모델)만 다시 본다 —
	// 갱신 빈도 LOD로 건너뛴 모델·정적 물체는 입력이 그대로라 전체 갱신이어도 모두 캐시 적중 (결과 동일)
	const bool bPartial    = GameplayValidatedScene == &TargetScene;
	GameplayValidatedScene = nullptr;
	if (IsPlaying() && &TargetScene == Scene)
	{
		DeltaSeconds *= TickTimeScale; // 플레이 씬 표시(애니메이션·플립북·파티클·시간대)도 게임 시간 (직전 게임플레이 틱과 같은 배율)
	}
	PresentationWritten.clear();
	if (const FEntity Sun = FTimeOfDaySystem::Update(TargetScene, DeltaSeconds, IsPlaying()); Sun.IsValid()) // 시간대 → 태양 회전 (Phase 49, 트랜스폼 갱신 전)
	{
		PresentationWritten.push_back({ Sun, {} });
	}
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::Animation);
		FAnimationSystem::Update(TargetScene, DeltaSeconds, bPartial ? &PresentationWritten : nullptr);
		// 2D 플립북 (Phase 56): 스프라이트 표시 프레임 + PendingEvents. 트랜스폼은 쓰지 않으므로 PresentationWritten과 무관하다.
		// 편집 중에도 돌아 미리보기가 되고, 이벤트(OnFlipbookEvent_<이름>/OnFlipbookFinished, 게임 모듈)는 플레이 중 다음 게임플레이 틱
		// 스크립트·게임 모듈 갱신이 노티파이와 같은 자리에서 배달한다
		FFlipbookSystem::Update(TargetScene, DeltaSeconds);
	}
	{
		const FScopedGameTickTimer Timer(EGameTickTimer::PresentTransforms);
		if (bPartial)
		{
			TargetScene.UpdateTransformsPartial(PresentationWritten);
		}
		else
		{
			TargetScene.UpdateTransforms();
		}
	}
	const FScopedGameTickTimer Timer(EGameTickTimer::Particles);
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::ResolveParticles(TargetScene, *Systems.Resources, Systems.ContentDirectory);
	}
	FParticleSystem::Update(TargetScene, DeltaSeconds);
}
