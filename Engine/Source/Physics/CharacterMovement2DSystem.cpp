#include "Physics/CharacterMovement2DSystem.h"

#include "Core/Log.h"
#include "Core/Settings/ProjectSettings.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/Physics2DWorld.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float GroundSkin       = 2.0f;  // cm, 바닥/벽 판정에 캡슐을 부풀리는 양
	constexpr float RisingEpsilon    = 1.0f;  // cm/s, 이보다 빠르게 올라가면 바닥이 아니다
	constexpr float SteepMinNormalY  = 0.05f; // 이보다 위를 보는 면 중 바닥이 아닌 것 = 가파른 경사

	float HalfSegmentOf(const FCharacterMovement2DComponent& Movement)
	{
		return std::max(Movement.CapsuleHeight * 0.5f - std::max(Movement.CapsuleRadius, 1.5f), 0.0f);
	}

	bool IsSolidCharacter(const FCharacterMovement2DComponent& Movement)
	{
		return Movement.CharacterCollision != FCharacterMovement2DComponent::ECharacterCollision::Ignore;
	}

	bool NeedsProxyRecreate(const FCharacterMovement2DComponent& A, const FCharacterMovement2DComponent& B)
	{
		return A.CapsuleRadius != B.CapsuleRadius || A.CapsuleHeight != B.CapsuleHeight || A.Layer != B.Layer || IsSolidCharacter(A) != IsSolidCharacter(B);
	}

	constexpr float  PushSideNormalY   = 0.5f; // 밀기: 법선 |Y|가 이보다 작은 캐릭터 접촉만 "옆"
	constexpr size_t MaxPushChainDepth = 4;    // 밀기 연쇄: 처음 미는 캐릭터 뒤로 밀리는 캐릭터 최대 수

	FVector3 GetWorldPosition(const FScene& Scene, FEntity Entity)
	{
		const FVector3& Local = Scene.GetTransform(Entity).Position;
		if (Scene.GetParent(Entity).IsValid() || Scene.IsSocketAttached(Entity))
		{
			return Scene.GetParentWorldMatrix(Entity).TransformPosition(Local);
		}
		return Local;
	}
} // namespace

void FCharacterMovement2DSystem::Begin(FPhysics2DSystem& InPhysics2D)
{
	End();
	Physics2D     = &InPhysics2D;
	bRecordEvents = true;
}

void FCharacterMovement2DSystem::End()
{
	for (auto& [Entity, Character] : Characters)
	{
		DestroyProxy(Character);
	}
	Characters.clear();
	PendingEvents.clear();
	WarnedLayers.clear();
	Physics2D = nullptr;
}

bool FCharacterMovement2DSystem::IsActive() const
{
	return Physics2D != nullptr && Physics2D->IsActive();
}

uint8 FCharacterMovement2DSystem::ResolveLayer(const std::string& Name)
{
	const int32 Found = FProjectSettings::Get().Collision.FindLayer(Name);
	if (Found < 0 && !Name.empty() && WarnedLayers.insert(Name).second)
	{
		E_LOG(LogPhysics, Warning, "없는 충돌 레이어 '{}' → Default로 처리합니다 (2D 캐릭터 이동기)", Name);
	}
	return static_cast<uint8>(Found < 0 ? 0 : Found);
}

void FCharacterMovement2DSystem::CreateProxy(FCharacter& Character, FEntity Entity, const FCharacterMovement2DComponent& Movement)
{
	DestroyProxy(Character);
	FPhysics2DWorld* World = IsActive() ? Physics2D->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return;
	}
	Character.Layer = ResolveLayer(Movement.Layer);
	FPhysics2DShapeDesc Shape;
	Shape.Shape          = EPhysics2DShape::Capsule;
	Shape.Radius         = std::max(Movement.CapsuleRadius, 1.5f);
	Shape.HalfSegment    = HalfSegmentOf(Movement);
	Shape.Friction       = 0.0f;
	Shape.bMoverProxy    = true;
	Shape.bSolidProxy    = IsSolidCharacter(Movement);
	Shape.CollisionLayer = Character.Layer;
	FPhysics2DBodyDesc Desc;
	Desc.Type            = EBodyType2D::Kinematic;
	Desc.Position        = Character.State.Position;
	Desc.bFixedRotation  = true;
	Desc.bReportContacts = true; // 받는 쪽 거르기는 FGameWorld 충돌 알림 전달이 한다
	Desc.UserData        = Entity.ToId();
	Desc.Shapes.push_back(Shape);
	Character.Proxy       = World->CreateBody(Desc);
	Character.CreatedWith = Movement;
}

void FCharacterMovement2DSystem::DestroyProxy(FCharacter& Character)
{
	if (Character.Proxy != ~0u && IsActive())
	{
		Physics2D->GetWorld()->DestroyBody(Character.Proxy);
	}
	Character.Proxy = ~0u;
}

void FCharacterMovement2DSystem::Sync(FScene& Scene)
{
	if (!IsActive())
	{
		return;
	}
	++SyncCounter;
	FRegistry&           Registry = Scene.GetRegistry();
	std::vector<FEntity> Entities;
	Registry.View<FCharacterMovement2DComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FCharacterMovement2DComponent&, FTransformComponent&) { Entities.push_back(Entity); });
	for (const FEntity Entity : Entities)
	{
		const FCharacterMovement2DComponent& Movement = Registry.Get<FCharacterMovement2DComponent>(Entity);
		auto                                 Found    = Characters.find(Entity);
		if (Found == Characters.end())
		{
			FCharacter     Character;
			const FVector3 World     = GetWorldPosition(Scene, Entity);
			Character.State.Position = FVector2(World.X, World.Z);
			Character.Depth          = World.Y;
			Found                    = Characters.emplace(Entity, Character).first;
			CreateProxy(Found->second, Entity, Movement);
		}
		else if (NeedsProxyRecreate(Found->second.CreatedWith, Movement))
		{
			CreateProxy(Found->second, Entity, Movement);
		}
		FCharacter& Character = Found->second;
		Character.LastSeen    = SyncCounter;
		Character.Settings    = Movement;
		// 스크립트/에디터가 트랜스폼을 직접 바꿨으면 순간이동 (속도 유지, 화면 오프셋은 버린다)
		if (Character.bWritten && FVector3::DistanceSquared(Scene.GetTransform(Entity).Position, Character.WrittenPosition) > 0.01f)
		{
			const FVector3 World     = GetWorldPosition(Scene, Entity);
			Character.State.Position = FVector2(World.X, World.Z);
			Character.Depth          = World.Y;
			Character.VisualOffset   = FVector2();
			Character.WrittenPosition = Scene.GetTransform(Entity).Position;
		}
		// 막는 캐릭터: 대리 바디는 지난 2D 스텝에서 속도만큼 더 갔다 — 이번 틱 다른 캐릭터 무브가 상태 위치를 보게 되돌린다
		if (IsSolidCharacter(Movement) && Character.Proxy != ~0u)
		{
			Physics2D->GetWorld()->SetTransform(Character.Proxy, Character.State.Position, 0.0f);
		}
	}
	for (auto It = Characters.begin(); It != Characters.end();)
	{
		if (It->second.LastSeen != SyncCounter)
		{
			DestroyProxy(It->second);
			It = Characters.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FCharacterMovement2DSystem::AddMovementInput(FEntity Entity, const FVector3& WorldDirection)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end() && std::isfinite(WorldDirection.X) && std::isfinite(WorldDirection.Z))
	{
		Found->second.PendingInput = Found->second.PendingInput + FVector2(WorldDirection.X, WorldDirection.Z);
	}
}

void FCharacterMovement2DSystem::Jump(FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.bPendingJump = true;
		Found->second.bJumpHeld    = true;
	}
}

void FCharacterMovement2DSystem::StopJumping(FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.bJumpHeld = false;
	}
}

void FCharacterMovement2DSystem::Dash(FEntity Entity, const FVector3& WorldDirection)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.bPendingDash         = true;
		Found->second.PendingDashDirection = std::isfinite(WorldDirection.X) && std::isfinite(WorldDirection.Z) ? FVector2(WorldDirection.X, WorldDirection.Z)
		                                                                                                         : FVector2();
	}
}

void FCharacterMovement2DSystem::DropDown(FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.bPendingDrop = true;
	}
}

namespace
{
	// 발사 하나를 다른 발사 위에 합친다 (성분마다 덮어쓰기면 대체, 아니면 합 — 덮어쓰기 표시는 남는다, 경직은 긴 쪽)
	void CombineLaunch(FCharacterMove2D& Into, const FVector2& Velocity, bool bOverrideX, bool bOverrideY, float StunSeconds)
	{
		if (!Into.bLaunch)
		{
			Into.bLaunch          = true;
			Into.LaunchVelocity   = FVector2();
			Into.bLaunchOverrideX = false;
			Into.bLaunchOverrideY = false;
			Into.StunSeconds      = 0.0f;
		}
		Into.LaunchVelocity.X = bOverrideX ? Velocity.X : Into.LaunchVelocity.X + Velocity.X;
		Into.LaunchVelocity.Y = bOverrideY ? Velocity.Y : Into.LaunchVelocity.Y + Velocity.Y;
		Into.bLaunchOverrideX = Into.bLaunchOverrideX || bOverrideX;
		Into.bLaunchOverrideY = Into.bLaunchOverrideY || bOverrideY;
		Into.StunSeconds      = std::max(Into.StunSeconds, StunSeconds);
	}
} // namespace

void FCharacterMovement2DSystem::LaunchCharacter(FEntity Entity, const FVector3& WorldVelocity, bool bOverrideX, bool bOverrideZ)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end() && std::isfinite(WorldVelocity.X) && std::isfinite(WorldVelocity.Z))
	{
		CombineLaunch(Found->second.PendingLaunch, FVector2(WorldVelocity.X, WorldVelocity.Z), bOverrideX, bOverrideZ, 0.0f);
	}
}

void FCharacterMovement2DSystem::AddKnockback(FEntity Entity, const FVector3& WorldVelocity, float StunSeconds)
{
	if (const auto Found = Characters.find(Entity);
	    Found != Characters.end() && std::isfinite(WorldVelocity.X) && std::isfinite(WorldVelocity.Z) && std::isfinite(StunSeconds))
	{
		CombineLaunch(Found->second.PendingLaunch, FVector2(WorldVelocity.X, WorldVelocity.Z), true, WorldVelocity.Z != 0.0f,
		              std::clamp(StunSeconds, 0.0f, FCharacterMove2D::MaxStunSeconds));
	}
}

bool FCharacterMovement2DSystem::HasPendingLaunch(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() && Found->second.PendingLaunch.bLaunch;
}

bool FCharacterMovement2DSystem::MergePendingLaunch(FEntity Entity, FCharacterMove2D& InOutMove)
{
	const auto Found = Characters.find(Entity);
	if (Found == Characters.end() || !Found->second.PendingLaunch.bLaunch)
	{
		return false;
	}
	const FCharacterMove2D& Pending = Found->second.PendingLaunch;
	// 무브에 이미 실린 발사(클라이언트가 보낸 것) 뒤에 서버의 발사를 합친다
	CombineLaunch(InOutMove, Pending.LaunchVelocity, Pending.bLaunchOverrideX, Pending.bLaunchOverrideY, Pending.StunSeconds);
	Found->second.PendingLaunch = FCharacterMove2D();
	return true;
}

FCharacterMove2D FCharacterMovement2DSystem::ConsumePendingMove(FEntity Entity, float DeltaSeconds)
{
	FCharacterMove2D Move;
	Move.DeltaSeconds = DeltaSeconds;
	const auto Found  = Characters.find(Entity);
	if (Found == Characters.end())
	{
		return Move;
	}
	FCharacter& Character = Found->second;
	Move.Input            = CharacterMovement2DMath::ClampInput(Character.PendingInput, Character.Settings.Mode);
	Move.bJumpPressed     = Character.bPendingJump;
	Move.bJumpHeld        = Character.bJumpHeld;
	Move.bDash            = Character.bPendingDash;
	Move.DashDirection    = Character.PendingDashDirection;
	Move.bDropDown        = Character.bPendingDrop;
	Character.PendingInput         = FVector2();
	Character.PendingDashDirection = FVector2();
	Character.bPendingJump         = false;
	Character.bPendingDash         = false;
	Character.bPendingDrop         = false;
	return Move;
}

void FCharacterMovement2DSystem::SimulateCharacter(FScene& Scene, FEntity Entity, const FCharacterMove2D& Move)
{
	const auto                           Found    = Characters.find(Entity);
	const FCharacterMovement2DComponent* Movement = Scene.GetRegistry().TryGet<FCharacterMovement2DComponent>(Entity);
	if (!IsActive() || Found == Characters.end() || Movement == nullptr)
	{
		return;
	}
	FCharacter&            Character    = Found->second;
	FPhysics2DWorld&       World        = *Physics2D->GetWorld();
	FCharacterState2D&     State        = Character.State;
	const float            DeltaSeconds = std::clamp(std::isfinite(Move.DeltaSeconds) ? Move.DeltaSeconds : 0.0f, 0.0f, FCharacterMove2D::MaxMoveDeltaSeconds);
	const bool             bPlatformer  = Movement->Mode == ECharacterMovement2DMode::Platformer;
	const float            WalkableY    = CharacterMovement2DMath::GetWalkableNormalY(*Movement);
	FCharacterMove2DEvents Events;

	FPhysics2DMover Mover  = MakeMover(Character, Entity, *Movement);
	const bool      bSolid = IsSolidCharacter(*Movement);

	// ---- 시작 접촉: 바닥 법선·발판 속도, 가파른 면
	std::vector<FPhysics2DMoverContact> Contacts;
	bool                                bStartGround = false;
	FVector2                            GroundNormal(0.0f, 1.0f);
	FVector2                            GroundVelocity;
	if (bPlatformer)
	{
		Mover.bIgnoreOneWay = State.DropTimer > 0.0f || Move.bDropDown;
		Mover.Velocity      = State.Velocity;
		World.CollideMover(Mover, State.Position, GroundSkin, Contacts);
		const FPhysics2DMoverContact* Ground = nullptr;
		for (const FPhysics2DMoverContact& Contact : Contacts)
		{
			if (Contact.Normal.Y >= WalkableY && (Ground == nullptr || Contact.Normal.Y > Ground->Normal.Y))
			{
				Ground = &Contact;
			}
		}
		if (Ground != nullptr && State.bGrounded)
		{
			bStartGround = true;
			GroundNormal = Ground->bCharacter ? FVector2(0.0f, 1.0f) : Ground->Normal; // 캐릭터 머리는 평평한 발판으로 (MoveMover와 같다)
			if (Ground->BodyType != EBodyType2D::Static)
			{
				GroundVelocity = World.GetPointVelocity(Ground->Body, Ground->Point);
			}
		}
	}

	const bool bWasGrounded = State.bGrounded;
	CharacterMovement2DMath::BeginMove(*Movement, State, Move, World.GetGravity().Y, GroundVelocity, Events);

	if (bPlatformer)
	{
		// 가파른 경사: 오르려는 수평 속도는 없앤다 (중력으로 미끄러져 내려간다)
		for (const FPhysics2DMoverContact& Contact : Contacts)
		{
			if (Contact.Normal.Y > SteepMinNormalY && Contact.Normal.Y < WalkableY && State.Velocity.X * Contact.Normal.X < 0.0f)
			{
				State.Velocity.X = 0.0f;
			}
		}
	}

	// ---- 이동: 바닥에서 걷는 중이면 경사 접선 방향 (오르막·내리막 같은 속력) + 발판 속도
	FVector2   MoveVelocity = State.Velocity;
	const bool bWalking     = bPlatformer && bStartGround && State.bGrounded && !State.IsDashing();
	if (bWalking)
	{
		MoveVelocity = FVector2(GroundNormal.Y, -GroundNormal.X) * State.Velocity.X + GroundVelocity;
	}
	Mover.bIgnoreOneWay = bPlatformer && State.DropTimer > 0.0f;
	Mover.Velocity      = State.Velocity;
	Mover.bSteepAsWall  = bWalking;
	FPhysics2DMoveResult Result;
	World.MoveMover(Mover, State.Position, MoveVelocity * DeltaSeconds, Result);
	if (Movement->CharacterCollision == FCharacterMovement2DComponent::ECharacterCollision::Push)
	{
		// 밀기: 옆의 캐릭터에 막혀 못 간 만큼 그 캐릭터를 수평(탑다운은 법선 반대)으로 밀고, 남은 만큼 한 번 더 움직인다
		const FVector2 Remaining = State.Position + MoveVelocity * DeltaSeconds - Result.Position;
		if (Remaining.LengthSquared() > 0.01f)
		{
			World.CollideMover(Mover, Result.Position, GroundSkin, Contacts);
			for (const FPhysics2DMoverContact& Contact : Contacts)
			{
				if (!Contact.bCharacter || std::abs(Contact.Normal.Y) >= PushSideNormalY)
				{
					continue;
				}
				const FVector2 Direction = bPlatformer ? FVector2(Contact.Normal.X > 0.0f ? -1.0f : 1.0f, 0.0f) : FVector2(-Contact.Normal.X, -Contact.Normal.Y);
				const float    Strength  = std::clamp(std::isfinite(Movement->PushStrength) ? Movement->PushStrength : 1.0f, 0.0f, 1.0f);
				const float    Amount    = FVector2::Dot(Remaining, Direction) * Strength;
				std::vector<FEntity> Chain { Entity };
				if (Amount > 0.0f && PushCharacter(Scene, FEntity::FromId(Contact.UserData), Direction * Amount, Chain) > 0.0f)
				{
					FPhysics2DMoveResult Second;
					World.MoveMover(Mover, Result.Position, Remaining, Second); // Mover.Velocity = 막히기 전 속도
					Result.Position = Second.Position;
					Result.Velocity = Second.Velocity;
				}
				break;
			}
		}
	}
	State.Position = Result.Position;
	State.Velocity = Result.Velocity;

	Mover.bSteepAsWall = false;
	// ---- 바닥 판정 (+ 걷던 중이면 아래로 붙이기)
	bool   bGroundedNow    = false;
	uint64 GroundCharacter = 0; // 밟기: 바닥 접촉 중 다른 캐릭터
	if (bPlatformer && State.Velocity.Y <= RisingEpsilon) // 수평 대시 중에도 (바닥 대시 뒤 가짜 착지 없음)
	{
		Mover.Velocity = State.Velocity;
		World.CollideMover(Mover, State.Position, GroundSkin, Contacts);
		for (const FPhysics2DMoverContact& Contact : Contacts)
		{
			bGroundedNow = bGroundedNow || Contact.Normal.Y >= WalkableY;
			if (Contact.bCharacter && Contact.Normal.Y >= WalkableY && GroundCharacter == 0)
			{
				GroundCharacter = Contact.UserData;
			}
		}
		if (!bGroundedNow && bWasGrounded && !Events.bJumped && !Move.bLaunch && !State.IsDashing() && Movement->GroundSnapDistance > 0.0f)
		{
			float                  Fraction = 1.0f;
			FPhysics2DMoverContact Hit;
			const FVector2         Down(0.0f, -Movement->GroundSnapDistance);
			if (World.CastMover(Mover, State.Position, Down, Fraction, Hit) && Hit.Normal.Y >= WalkableY)
			{
				State.Position = State.Position + Down * Fraction;
				bGroundedNow   = true;
			}
		}
	}
	CharacterMovement2DMath::EndMove(*Movement, State, bGroundedNow, Events);
	if (Events.bLanded && GroundCharacter != 0)
	{
		Events.bStomped      = true;
		Events.StompedEntity = GroundCharacter;
	}
	WriteTransform(Scene, Entity, Character);
	if (bSolid && Character.Proxy != ~0u)
	{
		World.SetTransform(Character.Proxy, State.Position, 0.0f); // 같은 틱 다른 캐릭터 무브가 지금 위치를 본다 (UpdateProxies가 스텝 전에 다시 맞춘다)
	}
	if (bRecordEvents && (Events.bJumped || Events.bLanded || Events.bDashStarted))
	{
		PendingEvents.push_back({ Entity, Events });
	}
}

FCharacterState2D FCharacterMovement2DSystem::GetState(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() ? Found->second.State : FCharacterState2D();
}

void FCharacterMovement2DSystem::SetState(FScene& Scene, FEntity Entity, const FCharacterState2D& State)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.State = State;
		WriteTransform(Scene, Entity, Found->second);
	}
}

void FCharacterMovement2DSystem::FollowTransform(FScene& Scene, FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		const FVector3 World          = GetWorldPosition(Scene, Entity);
		FCharacter&    Character      = Found->second;
		Character.State.Position      = FVector2(World.X, World.Z);
		Character.State.Velocity      = FVector2();
		Character.Depth               = World.Y;
		Character.WrittenPosition     = Scene.GetTransform(Entity).Position;
		Character.bWritten            = true;
		if (IsSolidCharacter(Character.CreatedWith) && Character.Proxy != ~0u && IsActive())
		{
			Physics2D->GetWorld()->SetTransform(Character.Proxy, Character.State.Position, 0.0f); // 막는 캐릭터: 이번 틱 예측 캐릭터 무브가 보간 위치를 본다
		}
	}
}

FPhysics2DMover FCharacterMovement2DSystem::MakeMover(const FCharacter& Character, FEntity Entity, const FCharacterMovement2DComponent& Movement) const
{
	FPhysics2DMover Mover;
	Mover.HalfSegment          = HalfSegmentOf(Movement);
	Mover.Radius               = std::max(Movement.CapsuleRadius, 1.5f);
	Mover.CollisionLayer       = Character.Layer;
	Mover.IgnoreUserData       = Entity.ToId();
	Mover.WalkableNormalY      = CharacterMovement2DMath::GetWalkableNormalY(Movement);
	Mover.OneWayMaxPenetration = std::max(Mover.Radius * 0.3f, 3.0f);
	Mover.bCollideCharacters   = IsSolidCharacter(Movement);
	return Mover;
}

float FCharacterMovement2DSystem::PushCharacter(FScene& Scene, FEntity Other, const FVector2& InDelta, std::vector<FEntity>& Chain)
{
	const auto Found = Characters.find(Other);
	if (Found == Characters.end() || !IsActive() || !IsSolidCharacter(Found->second.Settings) ||
	    std::find(Chain.begin(), Chain.end(), Other) != Chain.end())
	{
		return 0.0f;
	}
	Chain.push_back(Other);
	FCharacter&      Character  = Found->second;
	FPhysics2DWorld& World      = *Physics2D->GetWorld();
	// 밀림 저항: 넘겨받은 거리 ÷ (1 + r) — 사슬 단계마다 받는 쪽 저항으로 나눈다 (r = 0이면 그대로 = 이전 동작)
	const float      Resistance = std::isfinite(Character.Settings.PushResistance) ? std::max(Character.Settings.PushResistance, 0.0f) : 0.0f;
	const FVector2   Delta      = Resistance > 0.0f ? InDelta * (1.0f / (1.0f + Resistance)) : InDelta;
	FPhysics2DMover  Mover     = MakeMover(Character, Other, Character.Settings);
	Mover.Velocity             = Character.State.Velocity;
	Mover.bIgnoreOneWay        = Character.State.DropTimer > 0.0f;
	const FVector2       Start = Character.State.Position;
	FPhysics2DMoveResult Result;
	World.MoveMover(Mover, Start, Delta, Result); // 벽 안으로는 밀지 않는다
	// 연쇄: 이 캐릭터가 미는 방향 앞의 캐릭터에 막혔으면 남은 만큼 그 캐릭터를 밀고 다시 움직인다 (한 단계에 하나, 사슬에 이미 있는 캐릭터 제외)
	const FVector2 Remaining = Start + Delta - Result.Position;
	if (Chain.size() <= MaxPushChainDepth && Remaining.LengthSquared() > 0.01f && Delta.LengthSquared() > 0.0f)
	{
		const FVector2                      Direction = Delta * (1.0f / Delta.Length());
		std::vector<FPhysics2DMoverContact> Contacts;
		World.CollideMover(Mover, Result.Position, GroundSkin, Contacts);
		for (const FPhysics2DMoverContact& Contact : Contacts)
		{
			if (!Contact.bCharacter || std::abs(Contact.Normal.Y) >= PushSideNormalY || FVector2::Dot(Contact.Normal, Direction) >= 0.0f)
			{
				continue;
			}
			const float Amount = FVector2::Dot(Remaining, Direction);
			if (Amount > 0.0f && PushCharacter(Scene, FEntity::FromId(Contact.UserData), Direction * Amount, Chain) > 0.0f)
			{
				FPhysics2DMoveResult Second;
				World.MoveMover(Mover, Result.Position, Remaining, Second);
				Result.Position = Second.Position;
			}
			break;
		}
	}
	const float Moved = (Result.Position - Start).Length();
	if (Moved <= 0.0f)
	{
		return 0.0f;
	}
	Character.State.Position = Result.Position;
	WriteTransform(Scene, Other, Character);
	if (Character.Proxy != ~0u)
	{
		World.SetTransform(Character.Proxy, Character.State.Position, 0.0f);
	}
	return Moved;
}

void FCharacterMovement2DSystem::GetCharacterContacts(FEntity Entity, std::vector<FEntity>& OutEntities) const
{
	OutEntities.clear();
	const auto Found = Characters.find(Entity);
	if (Found == Characters.end() || !IsActive())
	{
		return;
	}
	const FCharacter&   Character = Found->second;
	const float         Radius    = std::max(Character.Settings.CapsuleRadius, 1.5f) + GroundSkin;
	std::vector<uint64> UserData;
	Physics2D->GetWorld()->OverlapBox(Character.State.Position, FVector2(Radius, HalfSegmentOf(Character.Settings) + Radius), 0.0f, UserData);
	for (const uint64 Id : UserData)
	{
		if (Id != 0 && Id != Entity.ToId())
		{
			OutEntities.push_back(FEntity::FromId(Id));
		}
	}
}

float FCharacterMovement2DSystem::GetDynamicPenetration(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	if (Found == Characters.end() || !IsActive())
	{
		return 0.0f;
	}
	FPhysics2DMover Mover      = MakeMover(Found->second, Entity, Found->second.Settings);
	Mover.bIgnoreOneWay        = true; // 원웨이는 겹쳐도 막지 않는다
	Mover.bCollideCharacters   = false;
	Mover.bIncludeDynamicSides = true;
	std::vector<FPhysics2DMoverContact> Contacts;
	Physics2D->GetWorld()->CollideMover(Mover, Found->second.State.Position, 0.0f, Contacts);
	float Deepest = 0.0f;
	for (const FPhysics2DMoverContact& Contact : Contacts)
	{
		if (Contact.BodyType == EBodyType2D::Dynamic)
		{
			Deepest = std::max(Deepest, Contact.Penetration);
		}
	}
	return Deepest;
}

void FCharacterMovement2DSystem::SetVisualOffset(FScene& Scene, FEntity Entity, const FVector2& Offset)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.VisualOffset = Offset;
		WriteTransform(Scene, Entity, Found->second);
	}
}

void FCharacterMovement2DSystem::UpdateProxies()
{
	if (!IsActive())
	{
		return;
	}
	FPhysics2DWorld& World = *Physics2D->GetWorld();
	for (auto& [Entity, Character] : Characters)
	{
		if (Character.Proxy == ~0u)
		{
			continue;
		}
		// 순간이동 + 속도: 스텝 동안 동적 바디가 캐릭터가 가는 쪽으로 밀린다 (다음 프레임에 다시 맞춘다)
		World.SetTransform(Character.Proxy, Character.State.Position, 0.0f);
		World.SetLinearVelocity(Character.Proxy, Character.State.Velocity);
	}
}

void FCharacterMovement2DSystem::WriteTransform(FScene& Scene, FEntity Entity, FCharacter& Character)
{
	const FVector2       Plane     = Character.State.Position + Character.VisualOffset;
	const FVector3       World(Plane.X, Character.Depth, Plane.Y);
	FTransformComponent& Transform = Scene.GetTransform(Entity);
	if (Scene.GetParent(Entity).IsValid() || Scene.IsSocketAttached(Entity))
	{
		FMatrix4x4 Inverse;
		if (Scene.GetParentWorldMatrix(Entity).TryGetInverse(Inverse))
		{
			Transform.Position = Inverse.TransformPosition(World);
		}
	}
	else
	{
		Transform.Position = World;
	}
	Character.WrittenPosition = Transform.Position;
	Character.bWritten        = true;
}

bool FCharacterMovement2DSystem::IsGrounded(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() && Found->second.State.bGrounded;
}

bool FCharacterMovement2DSystem::IsDashing(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() && Found->second.State.IsDashing();
}

bool FCharacterMovement2DSystem::IsStunned(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() && Found->second.State.IsStunned();
}

FVector2 FCharacterMovement2DSystem::GetVelocity(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return Found != Characters.end() ? Found->second.State.Velocity : FVector2();
}

int32 FCharacterMovement2DSystem::GetJumpsRemaining(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	if (Found == Characters.end() || Found->second.Settings.Mode != ECharacterMovement2DMode::Platformer)
	{
		return 0;
	}
	return std::max(Found->second.Settings.MaxJumps - static_cast<int32>(Found->second.State.JumpsUsed), 0);
}

int32 FCharacterMovement2DSystem::GetDashesRemaining(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	if (Found == Characters.end())
	{
		return 0;
	}
	const FCharacterState2D&             State    = Found->second.State;
	const FCharacterMovement2DComponent& Movement = Found->second.Settings;
	if (State.IsDashing() || State.DashCooldownTimer > 0.0f || Movement.DashTime <= 0.0f)
	{
		return 0;
	}
	return State.bGrounded ? std::max(Movement.MaxAirDashes, 1) : std::max(Movement.MaxAirDashes - static_cast<int32>(State.AirDashesUsed), 0);
}

void FCharacterMovement2DSystem::ConsumeEvents(std::vector<FEvent>& OutEvents)
{
	OutEvents.insert(OutEvents.end(), PendingEvents.begin(), PendingEvents.end());
	PendingEvents.clear();
}
