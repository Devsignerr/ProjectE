#include "Physics/Physics2DSystem.h"

#include "Core/Profiling.h"
#include "Core/Settings/ProjectSettings.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
	constexpr float TeleportPositionTolerance = 0.01f;   // cm
	constexpr float TeleportRotationTolerance = 1.0e-5f; // 1 - |dot|
	constexpr float TeleportAngleTolerance    = 1.0e-4f; // 라디안
	constexpr float ShapeRelativeTolerance    = 1.0e-4f;

	// 경고/캐시 종류
	enum EKind : uint32
	{
		Kind_Polygon = 0,
		Kind_Edge    = 1,
		Kind_Tilted  = 2,
		Kind_PolygonHull = 3,
		Kind_PolygonParse = 4,
		Kind_EdgeParse = 5,
	};

	FMatrix4x4 ComputeWorldMatrix(const FScene& Scene, FEntity Entity)
	{
		return Scene.GetTransform(Entity).GetLocalMatrix() * Scene.GetParentWorldMatrix(Entity);
	}

	bool IsNear(float A, float B)
	{
		return std::abs(A - B) <= ShapeRelativeTolerance * std::max({ 1.0f, std::abs(A), std::abs(B) });
	}
	bool IsNear(const FVector2& A, const FVector2& B) { return IsNear(A.X, B.X) && IsNear(A.Y, B.Y); }

	bool IsSameShape(const FPhysics2DShapeDesc& A, const FPhysics2DShapeDesc& B)
	{
		if (A.Shape != B.Shape || A.bLoop != B.bLoop || A.Friction != B.Friction || A.Restitution != B.Restitution || A.Density != B.Density ||
		    A.bIsTrigger != B.bIsTrigger || A.bOneWay != B.bOneWay || A.CollisionLayer != B.CollisionLayer || A.Points.size() != B.Points.size())
		{
			return false;
		}
		if (!IsNear(A.Offset, B.Offset) || !IsNear(A.HalfSize, B.HalfSize) || !IsNear(A.Angle, B.Angle) || !IsNear(A.Radius, B.Radius) ||
		    !IsNear(A.HalfSegment, B.HalfSegment))
		{
			return false;
		}
		for (size_t Index = 0; Index < A.Points.size(); ++Index)
		{
			if (!IsNear(A.Points[Index], B.Points[Index]))
			{
				return false;
			}
		}
		return true;
	}

	// 다시 만들어야 하는가 (위치/각/보고 여부/UserData 제외)
	bool NeedsRecreate(const FPhysics2DBodyDesc& Old, const FPhysics2DBodyDesc& New)
	{
		if (Old.Type != New.Type || Old.Mass != New.Mass || Old.GravityScale != New.GravityScale || Old.LinearDamping != New.LinearDamping ||
		    Old.AngularDamping != New.AngularDamping || Old.bFixedRotation != New.bFixedRotation || Old.bBullet != New.bBullet ||
		    Old.Shapes.size() != New.Shapes.size())
		{
			return true;
		}
		for (size_t Index = 0; Index < Old.Shapes.size(); ++Index)
		{
			if (!IsSameShape(Old.Shapes[Index], New.Shapes[Index]))
			{
				return true;
			}
		}
		return false;
	}

	// 최단 각 보간
	float LerpAngle(float From, float To, float Alpha)
	{
		float Delta = std::fmod(To - From, FMath::TwoPi);
		if (Delta > FMath::Pi)
		{
			Delta -= FMath::TwoPi;
		}
		else if (Delta < -FMath::Pi)
		{
			Delta += FMath::TwoPi;
		}
		return From + Delta * Alpha;
	}

	float AngleDifference(float A, float B) { return std::abs(LerpAngle(A, B, 1.0f) - A); }

	bool IsSameRotation(const FQuat& A, const FQuat& B) { return 1.0f - std::abs(FQuat::Dot(A, B)) <= TeleportRotationTolerance; }

	template <typename T>
	void FillCommon(const T& Collider, FPhysics2DShapeDesc& Shape)
	{
		Shape.Friction    = Collider.Friction;
		Shape.Restitution = Collider.Restitution;
		Shape.bIsTrigger  = Collider.bIsTrigger;
		Shape.bOneWay     = Collider.bOneWay;
	}

	FVector2 ScalePoint(const FVector2& Point, float ScaleX, float ScaleZ) { return FVector2(Point.X * ScaleX, Point.Y * ScaleZ); }
} // namespace

FPhysics2DSystem::FPhysics2DSystem()  = default;
FPhysics2DSystem::~FPhysics2DSystem() = default;

void FPhysics2DSystem::Begin()
{
	End();
	World = std::make_unique<FPhysics2DWorld>();
	const FPhysicsSettings& Settings = FProjectSettings::Get().Physics;
	World->SetGravity(Settings.Gravity2D);
	Stepper.StepSeconds = 1.0f / std::clamp(Settings.FixedStepHz, 15.0f, 240.0f);
	Stepper.MaxSteps    = std::clamp(Settings.MaxSubSteps, 1u, 16u);
	Stepper.Reset();
	CollisionLayers = FProjectSettings::Get().Collision;
	World->SetCollisionLayers(CollisionLayers);
	WarnedLayerNames.clear();
	WarnedEntities.clear();
}

void FPhysics2DSystem::End()
{
	Bodies.clear();
	PointCaches.clear();
	CollisionEvents.clear();
	World.reset();
	Stepper.Reset();
}

uint8 FPhysics2DSystem::ResolveCollisionLayer(const std::string& Name) const
{
	const int32 Found = CollisionLayers.FindLayer(Name);
	if (Found < 0 && !Name.empty() && WarnedLayerNames.insert(Name).second)
	{
		E_LOG(LogPhysics, Warning, "없는 충돌 레이어 '{}' → Default로 처리합니다 (2D, 프로젝트 설정 → 충돌 레이어)", Name);
	}
	return static_cast<uint8>(Found < 0 ? 0 : Found);
}

void FPhysics2DSystem::WarnOnce(FEntity Entity, uint32 Kind, const std::string& Message) const
{
	if (WarnedEntities.insert({ Entity.ToId(), Kind }).second)
	{
		E_LOG(LogPhysics, Warning, "{}", Message);
	}
}

const FPhysics2DSystem::FPointCache& FPhysics2DSystem::ParseCached(FEntity Entity, uint32 Kind, const std::string& Source)
{
	FPointCaches& Caches = PointCaches[Entity];
	FPointCache&  Cache  = Kind == Kind_Polygon ? Caches.Polygon : Caches.Edge;
	if (!Cache.bParsed || Cache.Source != Source)
	{
		Cache.bParsed = true;
		Cache.Source  = Source;
		std::string Error;
		Cache.bValid = Physics2DMath::ParsePoints(Source, Cache.Points, &Error);
		if (!Cache.bValid)
		{
			WarnOnce(Entity, Kind == Kind_Polygon ? Kind_PolygonParse : Kind_EdgeParse,
			         std::format("2D 콜라이더 점 목록 오류 (엔티티 {}): {}", Entity.ToId(), Error));
		}
	}
	return Cache;
}

bool FPhysics2DSystem::BuildDesc(FScene& Scene, FEntity Entity, const FVector3& Scale, FPhysics2DBodyDesc& OutDesc)
{
	FRegistry&  Registry = Scene.GetRegistry();
	const float ScaleX   = Scale.X;
	const float ScaleZ   = Scale.Z;
	const float AbsX     = std::abs(ScaleX);
	const float AbsZ     = std::abs(ScaleZ);
	OutDesc.Shapes.clear();

	if (const FBoxCollider2DComponent* Box = Registry.TryGet<FBoxCollider2DComponent>(Entity))
	{
		FPhysics2DShapeDesc Shape;
		Shape.Shape          = EPhysics2DShape::Box;
		Shape.HalfSize       = FVector2(std::abs(Box->Size.X) * 0.5f * AbsX, std::abs(Box->Size.Y) * 0.5f * AbsZ);
		Shape.Angle          = Box->Angle * FMath::DegToRad;
		Shape.Offset         = ScalePoint(Box->Offset, ScaleX, ScaleZ);
		Shape.Density        = Box->Density;
		Shape.CollisionLayer = ResolveCollisionLayer(Box->Layer);
		FillCommon(*Box, Shape);
		OutDesc.Shapes.push_back(std::move(Shape));
	}
	if (const FCircleCollider2DComponent* Circle = Registry.TryGet<FCircleCollider2DComponent>(Entity))
	{
		FPhysics2DShapeDesc Shape;
		Shape.Shape          = EPhysics2DShape::Circle;
		Shape.Radius         = std::abs(Circle->Radius) * std::max(AbsX, AbsZ);
		Shape.Offset         = ScalePoint(Circle->Offset, ScaleX, ScaleZ);
		Shape.Density        = Circle->Density;
		Shape.CollisionLayer = ResolveCollisionLayer(Circle->Layer);
		FillCommon(*Circle, Shape);
		OutDesc.Shapes.push_back(std::move(Shape));
	}
	if (const FCapsuleCollider2DComponent* Capsule = Registry.TryGet<FCapsuleCollider2DComponent>(Entity))
	{
		FPhysics2DShapeDesc Shape;
		Shape.Shape          = EPhysics2DShape::Capsule;
		Shape.Radius         = std::abs(Capsule->Radius) * AbsX;
		Shape.HalfSegment    = std::max(std::abs(Capsule->Height) * AbsZ * 0.5f - Shape.Radius, 0.0f);
		Shape.Offset         = ScalePoint(Capsule->Offset, ScaleX, ScaleZ);
		Shape.Density        = Capsule->Density;
		Shape.CollisionLayer = ResolveCollisionLayer(Capsule->Layer);
		FillCommon(*Capsule, Shape);
		OutDesc.Shapes.push_back(std::move(Shape));
	}
	if (const FPolygonCollider2DComponent* Polygon = Registry.TryGet<FPolygonCollider2DComponent>(Entity))
	{
		const FPointCache& Cache = ParseCached(Entity, Kind_Polygon, Polygon->Points);
		std::vector<FVector2> Points;
		Points.reserve(Cache.Points.size());
		for (const FVector2& Point : Cache.Points)
		{
			Points.push_back(ScalePoint(Point, ScaleX, ScaleZ));
		}
		if (!Physics2DMath::IsConvexPolygon(Points) || Points.size() > 8)
		{
			std::vector<FVector2> Hull = Physics2DMath::ComputeConvexHull(Points);
			if (Hull.size() > 8)
			{
				Hull = Physics2DMath::ReduceConvexPolygon(std::move(Hull), 8);
			}
			if (Cache.bValid && Hull.size() >= 3)
			{
				WarnOnce(Entity, Kind_PolygonHull,
				         std::format("2D 다각형 콜라이더가 오목하거나 8점을 넘어 볼록 껍질({}점)로 만듭니다 (엔티티 {})", Hull.size(), Entity.ToId()));
			}
			Points = std::move(Hull);
		}
		else if (Physics2DMath::SignedArea(Points) < 0.0f)
		{
			std::reverse(Points.begin(), Points.end());
		}
		if (Points.size() >= 3)
		{
			FPhysics2DShapeDesc Shape;
			Shape.Shape          = EPhysics2DShape::Polygon;
			Shape.Points         = std::move(Points);
			Shape.Offset         = ScalePoint(Polygon->Offset, ScaleX, ScaleZ);
			Shape.Density        = Polygon->Density;
			Shape.CollisionLayer = ResolveCollisionLayer(Polygon->Layer);
			FillCommon(*Polygon, Shape);
			OutDesc.Shapes.push_back(std::move(Shape));
		}
		else if (Cache.bValid)
		{
			WarnOnce(Entity, Kind_Polygon, std::format("2D 다각형 콜라이더는 넓이가 있는 3점 이상이 필요합니다 (엔티티 {})", Entity.ToId()));
		}
	}
	if (const FEdgeCollider2DComponent* Edge = Registry.TryGet<FEdgeCollider2DComponent>(Entity))
	{
		const FPointCache& Cache = ParseCached(Entity, Kind_Edge, Edge->Points);
		if (Cache.Points.size() >= 2)
		{
			FPhysics2DShapeDesc Shape;
			Shape.Shape = EPhysics2DShape::Edge;
			Shape.Points.reserve(Cache.Points.size());
			for (const FVector2& Point : Cache.Points)
			{
				Shape.Points.push_back(ScalePoint(Point, ScaleX, ScaleZ));
			}
			Shape.bLoop          = Edge->bLoop;
			Shape.Offset         = ScalePoint(Edge->Offset, ScaleX, ScaleZ);
			Shape.Density        = 0.0f;
			Shape.CollisionLayer = ResolveCollisionLayer(Edge->Layer);
			FillCommon(*Edge, Shape);
			OutDesc.Shapes.push_back(std::move(Shape));
		}
		else if (Cache.bValid)
		{
			WarnOnce(Entity, Kind_Edge, std::format("2D 선분 콜라이더는 2점 이상이 필요합니다 (엔티티 {})", Entity.ToId()));
		}
	}
	return !OutDesc.Shapes.empty();
}

uint32 FPhysics2DSystem::Update(FScene& Scene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("2D 물리");
	if (!World)
	{
		return 0;
	}
	++FrameCounter;
	CollisionEvents.clear();
	SyncBodies(Scene);
	CollectContactEvents(); // 사라진 바디의 접촉 끝

	const uint32 Steps = Stepper.Advance(DeltaSeconds);
	for (uint32 Step = 0; Step < Steps; ++Step)
	{
		const float StepAlpha = static_cast<float>(Step + 1) / static_cast<float>(Steps);
		for (auto& [Entity, State] : Bodies)
		{
			if (State.CreatedDesc.Type == EBodyType2D::Kinematic)
			{
				World->MoveKinematic(State.Body, State.PreviousPosition + (State.CurrentPosition - State.PreviousPosition) * StepAlpha,
				                     LerpAngle(State.PreviousAngle, State.CurrentAngle, StepAlpha), Stepper.StepSeconds);
			}
			else if (State.CreatedDesc.Type == EBodyType2D::Dynamic)
			{
				State.PreviousPosition = State.CurrentPosition;
				State.PreviousAngle    = State.CurrentAngle;
			}
		}
		World->Step(Stepper.StepSeconds);
		CollectContactEvents();
		for (auto& [Entity, State] : Bodies)
		{
			if (State.CreatedDesc.Type == EBodyType2D::Dynamic)
			{
				World->GetTransform(State.Body, State.CurrentPosition, State.CurrentAngle);
			}
		}
	}
	if (Steps > 0)
	{
		for (auto& [Entity, State] : Bodies)
		{
			if (State.CreatedDesc.Type == EBodyType2D::Kinematic)
			{
				State.PreviousPosition = State.CurrentPosition;
				State.PreviousAngle    = State.CurrentAngle;
			}
		}
	}
	WriteDynamicTransforms(Scene);
	return Steps;
}

void FPhysics2DSystem::SyncBodies(FScene& Scene)
{
	if (!World)
	{
		return;
	}
	FRegistry& Registry = Scene.GetRegistry();

	std::vector<FEntity>        Candidates;
	std::unordered_set<FEntity> Seen;
	const auto                  Collect = [&](FEntity Entity) {
        if (Seen.insert(Entity).second)
        {
            Candidates.push_back(Entity);
        }
	};
	Registry.View<FTransformComponent, FBoxCollider2DComponent>().Each([&](FEntity Entity, FTransformComponent&, FBoxCollider2DComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FCircleCollider2DComponent>().Each([&](FEntity Entity, FTransformComponent&, FCircleCollider2DComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FCapsuleCollider2DComponent>().Each([&](FEntity Entity, FTransformComponent&, FCapsuleCollider2DComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FPolygonCollider2DComponent>().Each([&](FEntity Entity, FTransformComponent&, FPolygonCollider2DComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FEdgeCollider2DComponent>().Each([&](FEntity Entity, FTransformComponent&, FEdgeCollider2DComponent&) { Collect(Entity); });

	for (FEntity Entity : Candidates)
	{
		const FRigidBody2DComponent* RigidBody = Registry.TryGet<FRigidBody2DComponent>(Entity);
		if (RigidBody != nullptr && !RigidBody->bEnabled)
		{
			continue; // 바디 없음 (있던 것은 아래에서 지운다)
		}
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
		PhysicsMath::DecomposeWorld(ComputeWorldMatrix(Scene, Entity), Position, Rotation, Scale);
		bool        bTilted = false;
		const float Angle   = Physics2DMath::AngleFromRotation(Rotation, &bTilted);
		if (bTilted)
		{
			WarnOnce(Entity, Kind_Tilted,
			         std::format("2D 물리 엔티티 {}의 회전에 평면(X·Z) 밖 성분이 있어 무시합니다 (Y축 회전만 쓴다)", Entity.ToId()));
		}

		FPhysics2DBodyDesc Desc;
		if (!BuildDesc(Scene, Entity, Scale, Desc))
		{
			continue;
		}
		const FVector2 PlanePosition = Physics2DMath::ToPlane(Position);
		Desc.Position = PlanePosition;
		Desc.Angle    = Angle;
		Desc.UserData = Entity.ToId();
		if (RigidBody != nullptr)
		{
			Desc.Type           = static_cast<EBodyType2D>(std::clamp(static_cast<int32>(RigidBody->BodyType), 0, 2));
			Desc.Mass           = RigidBody->Mass;
			Desc.GravityScale   = RigidBody->GravityScale;
			Desc.LinearDamping  = RigidBody->LinearDamping;
			Desc.AngularDamping = RigidBody->AngularDamping;
			Desc.bFixedRotation = RigidBody->bFixedRotation;
			Desc.bBullet        = RigidBody->bBullet;
			if (Desc.Type == EBodyType2D::Dynamic && KinematicOverride && KinematicOverride(Scene, Entity))
			{
				Desc.Type = EBodyType2D::Kinematic;
			}
		}
		else
		{
			Desc.Type = EBodyType2D::Static;
		}
		Desc.bReportContacts = (RigidBody != nullptr && RigidBody->bReportContacts) || (ContactReportFilter && ContactReportFilter(Scene, Entity));

		auto Found = Bodies.find(Entity);
		if (Found == Bodies.end() || NeedsRecreate(Found->second.CreatedDesc, Desc))
		{
			if (Found != Bodies.end())
			{
				World->DestroyBody(Found->second.Body);
			}
			FBodyState State;
			State.Body = World->CreateBody(Desc);
			if (State.Body == FPhysics2DWorld::InvalidBody)
			{
				if (Found != Bodies.end())
				{
					Bodies.erase(Found);
				}
				continue;
			}
			State.CreatedDesc      = Desc;
			State.LastSeenFrame    = FrameCounter;
			State.Depth            = Position.Y;
			State.PreviousPosition = State.CurrentPosition = PlanePosition;
			State.PreviousAngle    = State.CurrentAngle    = Angle;
			Bodies[Entity]         = std::move(State);
			continue;
		}

		FBodyState& State   = Found->second;
		State.LastSeenFrame = FrameCounter;
		if (State.CreatedDesc.bReportContacts != Desc.bReportContacts)
		{
			World->SetBodyReportsContacts(State.Body, Desc.bReportContacts);
			State.CreatedDesc.bReportContacts = Desc.bReportContacts;
		}
		switch (State.CreatedDesc.Type)
		{
		case EBodyType2D::Static:
			State.Depth = Position.Y;
			if ((State.CurrentPosition - PlanePosition).LengthSquared() > TeleportPositionTolerance * TeleportPositionTolerance ||
			    AngleDifference(State.CurrentAngle, Angle) > TeleportAngleTolerance)
			{
				World->SetTransform(State.Body, PlanePosition, Angle);
				State.CurrentPosition = PlanePosition;
				State.CurrentAngle    = Angle;
			}
			break;
		case EBodyType2D::Kinematic:
			State.Depth           = Position.Y;
			State.CurrentPosition = PlanePosition; // 스텝 동안 Previous → Current
			State.CurrentAngle    = Angle;
			break;
		case EBodyType2D::Dynamic:
		{
			const FTransformComponent& Transform = Scene.GetTransform(Entity);
			if (State.bWritten &&
			    (FVector3::DistanceSquared(Transform.Position, State.WrittenPosition) > TeleportPositionTolerance * TeleportPositionTolerance ||
			     !IsSameRotation(Transform.Rotation, State.WrittenRotation)))
			{
				World->SetTransform(State.Body, PlanePosition, Angle);
				State.Depth            = Position.Y;
				State.PreviousPosition = State.CurrentPosition = PlanePosition;
				State.PreviousAngle    = State.CurrentAngle    = Angle;
			}
			break;
		}
		}
	}

	for (auto It = Bodies.begin(); It != Bodies.end();)
	{
		if (It->second.LastSeenFrame != FrameCounter)
		{
			World->DestroyBody(It->second.Body);
			It = Bodies.erase(It);
		}
		else
		{
			++It;
		}
	}
	// 사라진 엔티티의 점 목록 캐시
	for (auto It = PointCaches.begin(); It != PointCaches.end();)
	{
		const FEntity Entity = It->first;
		if (!Registry.IsValid(Entity) || (!Registry.Has<FPolygonCollider2DComponent>(Entity) && !Registry.Has<FEdgeCollider2DComponent>(Entity)))
		{
			It = PointCaches.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FPhysics2DSystem::WriteDynamicTransforms(FScene& Scene)
{
	const float Alpha = bInterpolate ? Stepper.GetAlpha() : 1.0f;
	for (auto& [Entity, State] : Bodies)
	{
		if (State.CreatedDesc.Type != EBodyType2D::Dynamic)
		{
			continue;
		}
		const FVector2 PlanePosition = State.PreviousPosition + (State.CurrentPosition - State.PreviousPosition) * Alpha;
		const float    Angle         = LerpAngle(State.PreviousAngle, State.CurrentAngle, Alpha);
		const FVector3 WorldPosition = Physics2DMath::FromPlane(PlanePosition, State.Depth);
		const FQuat    WorldRotation = Physics2DMath::RotationFromAngle(Angle);

		FTransformComponent& Transform = Scene.GetTransform(Entity);
		if (Scene.GetParent(Entity).IsValid() || Scene.IsSocketAttached(Entity))
		{
			PhysicsMath::WorldToLocal(Scene.GetParentWorldMatrix(Entity), WorldPosition, WorldRotation, Transform.Position, Transform.Rotation);
		}
		else
		{
			Transform.Position = WorldPosition;
			Transform.Rotation = WorldRotation;
		}
		State.WrittenPosition = Transform.Position;
		State.WrittenRotation = Transform.Rotation;
		State.bWritten        = true;
	}
}

void FPhysics2DSystem::CollectContactEvents()
{
	ContactScratch.clear();
	World->ConsumeContactEvents(ContactScratch);
	for (const FPhysics2DContactEvent& Contact : ContactScratch)
	{
		const bool          bBegin = Contact.Type == EPhysics2DContactEventType::Begin;
		ECollisionEventType Type   = bBegin ? ECollisionEventType::CollisionBegin : ECollisionEventType::CollisionEnd;
		if (Contact.bSensor)
		{
			Type = bBegin ? ECollisionEventType::TriggerEnter : ECollisionEventType::TriggerExit;
		}
		FCollisionEvent First;
		First.Type          = Type;
		First.Self          = FEntity::FromId(Contact.UserData1);
		First.Other         = FEntity::FromId(Contact.UserData2);
		const auto Depth    = [this](FEntity Entity) {
            const auto Found = Bodies.find(Entity);
            return Found != Bodies.end() ? Found->second.Depth : 0.0f;
		};
		const FVector3 Normal = Physics2DMath::FromPlane(Contact.Normal, 0.0f); // 바디 2를 1에서 밀어내는 방향
		First.Point           = Physics2DMath::FromPlane(Contact.Position, Depth(First.Self));
		First.Normal          = -Normal;
		First.Impulse         = Contact.Impulse;
		First.ApproachSpeed   = Contact.ApproachSpeed;
		FCollisionEvent Second = First;
		Second.Self            = First.Other;
		Second.Other           = First.Self;
		Second.Point           = Physics2DMath::FromPlane(Contact.Position, Depth(Second.Self));
		Second.Normal          = Normal;
		CollisionEvents.push_back(First);
		CollisionEvents.push_back(Second);
	}
}

void FPhysics2DSystem::AddForce(FEntity Entity, const FVector2& Force)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->AddForce(Found->second.Body, Force);
	}
}

void FPhysics2DSystem::AddImpulse(FEntity Entity, const FVector2& Impulse)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->AddImpulse(Found->second.Body, Impulse);
	}
}

void FPhysics2DSystem::SetVelocity(FEntity Entity, const FVector2& Velocity)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->SetLinearVelocity(Found->second.Body, Velocity);
	}
}

FVector2 FPhysics2DSystem::GetVelocity(FEntity Entity) const
{
	const auto Found = Bodies.find(Entity);
	return World && Found != Bodies.end() ? World->GetLinearVelocity(Found->second.Body) : FVector2();
}

void FPhysics2DSystem::SetAngularVelocity(FEntity Entity, float RadiansPerSecond)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->SetAngularVelocity(Found->second.Body, RadiansPerSecond);
	}
}

float FPhysics2DSystem::GetAngularVelocity(FEntity Entity) const
{
	const auto Found = Bodies.find(Entity);
	return World && Found != Bodies.end() ? World->GetAngularVelocity(Found->second.Body) : 0.0f;
}

float FPhysics2DSystem::GetMass(FEntity Entity) const
{
	const auto Found = Bodies.find(Entity);
	return World && Found != Bodies.end() ? World->GetMass(Found->second.Body) : 0.0f;
}

bool FPhysics2DSystem::IsDynamicBody(FEntity Entity) const
{
	const auto Found = Bodies.find(Entity);
	return World && Found != Bodies.end() && Found->second.CreatedDesc.Type == EBodyType2D::Dynamic;
}

bool FPhysics2DSystem::Raycast(const FVector2& Origin, const FVector2& Direction, float MaxDistance, FPhysics2DHit& OutHit, uint32 LayerMask) const
{
	FPhysics2DRayHit Hit;
	if (!World || !World->Raycast(Origin, Direction, MaxDistance, Hit, LayerMask))
	{
		return false;
	}
	OutHit.Entity   = FEntity::FromId(Hit.UserData);
	OutHit.Position = Hit.Position;
	OutHit.Normal   = Hit.Normal;
	OutHit.Distance = Hit.Distance;
	OutHit.Fraction = Hit.Fraction;
	return true;
}

uint32 FPhysics2DSystem::OverlapBox(const FVector2& Center, const FVector2& HalfSize, float Angle, std::vector<FEntity>& OutEntities, uint32 LayerMask) const
{
	if (!World)
	{
		return 0;
	}
	std::vector<uint64> UserData;
	World->OverlapBox(Center, HalfSize, Angle, UserData, LayerMask);
	for (const uint64 Data : UserData)
	{
		OutEntities.push_back(FEntity::FromId(Data));
	}
	return static_cast<uint32>(UserData.size());
}

uint32 FPhysics2DSystem::OverlapCircle(const FVector2& Center, float Radius, std::vector<FEntity>& OutEntities, uint32 LayerMask) const
{
	if (!World)
	{
		return 0;
	}
	std::vector<uint64> UserData;
	World->OverlapCircle(Center, Radius, UserData, LayerMask);
	for (const uint64 Data : UserData)
	{
		OutEntities.push_back(FEntity::FromId(Data));
	}
	return static_cast<uint32>(UserData.size());
}
